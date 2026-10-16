#!/usr/bin/env python3
"""Library of the evaluation harness (DESIGN section 15, PREPLAN section 6). Python 3 stdlib only.

Contents
  Engine            client for `vpengine serve` (JSON lines, request ids, events, timeouts, clean shutdown, peak RSS)
  load_project      project.new {sourcePath} + cue.page in windows of 200
  translate         translate.start, collects translate.cue batches and translate.done stats
  cue_details       cue.get for every cue (checks, reasons, tokens)
  export_and_verify export.write to a temporary path + byte-level structure check against the source
  verify_structure  numbering/timing/tag identity of two subtitle documents (SRT, VTT, ASS, TXT)
  normalise, gold_match, load_gold   gold comparison (NFKD, lower case, marks stripped, punctuation/space, u/v, i/j)
  clopper_pearson, proportion, rule_of_three, mcnemar_exact, cohen_kappa   statistics
  heldout_guard, read_burned          held-out hygiene (FROZEN.sha256, BURNED.txt)
  targets_hash, peak_rss_kb, counted  determinism hash, VmHWM, the cue denominator rule of PREPLAN 6.1
"""
import hashlib
import json
import math
import os
import queue
import random
import re
import resource
import shutil
import subprocess
import tempfile
import threading
import time
import unicodedata

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
HELDOUT_DIR = os.path.join(ROOT, "tests", "heldout")

# Checks whose failure is an automatic error (DESIGN 10.4, PREPLAN 6.3), and the subset used to score a third-party
# system (PREPLAN 6.6 step 4: A1/A3/A4; its plain text has no markup and it makes no tier promise).
AUTO_ERROR_CHECKS = ("A1", "A3", "A4", "A5")
SYSTEM_ERROR_CHECKS = ("A1", "A3", "A4")
ALL_CHECKS = tuple("A%d" % i for i in range(1, 10))
ERROR_TYPES = ("grammar", "meaning", "vocabulary", "orthography", "markup", "none")


class EngineError(Exception):
    def __init__(self, cmd, error):
        error = error or {}
        self.cmd = cmd
        self.code = error.get("code", "?")
        self.message = error.get("message", "")
        self.hint = error.get("hint", "")
        Exception.__init__(self, "%s -> %s: %s" % (cmd, self.code, self.message))


# ------------------------------------------------------------------------------------------------------ engine client
class Engine(object):
    """One `vpengine serve` process. Use as a context manager; close() shuts it down and joins the reader thread."""

    def __init__(self, exe, data_dir, lexicons=None, log_path=None, env=None, timeout=120.0):
        self.exe = exe
        self.timeout = timeout
        cmd = [exe, "serve", "--data", data_dir]
        if lexicons:
            cmd += ["--lexicons", lexicons]
        e = dict(os.environ)
        e.setdefault("VP_LOG", "info")
        e.pop("VP_STUB_DELAY_US", None)
        if env:
            e.update(env)
        self._log = open(log_path, "ab") if log_path else open(os.devnull, "wb")
        self.p = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=self._log, env=e,
                                  bufsize=0)
        self._q = queue.Queue()
        self._events = []
        self._ev_cv = threading.Condition()
        self._next = 1
        self._wlock = threading.Lock()
        self.bad_lines = 0
        self._t = threading.Thread(target=self._read, name="vpengine-reader")
        self._t.daemon = True
        self._t.start()
        self.closed = False
        self.peak_rss = None

    @property
    def pid(self):
        return self.p.pid

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()
        return False

    def _read(self):
        for raw in self.p.stdout:
            try:
                msg = json.loads(raw.decode("utf-8"))
            except ValueError:
                self.bad_lines += 1
                continue
            if isinstance(msg, dict) and "event" in msg:
                with self._ev_cv:
                    self._events.append(msg)
                    self._ev_cv.notify_all()
            else:
                self._q.put(msg)
        self._q.put(None)
        with self._ev_cv:
            self._ev_cv.notify_all()

    def send(self, cmd, params=None):
        with self._wlock:
            rid = self._next
            self._next += 1
            line = json.dumps({"id": rid, "cmd": cmd, "params": params or {}}, ensure_ascii=False) + "\n"
            self.p.stdin.write(line.encode("utf-8"))
            self.p.stdin.flush()
        return rid

    def wait(self, rid, timeout=None):
        deadline = time.time() + (timeout or self.timeout)
        stash = []
        try:
            while True:
                left = deadline - time.time()
                if left <= 0:
                    raise TimeoutError("vpengine: no response to request %d" % rid)
                try:
                    msg = self._q.get(timeout=left)
                except queue.Empty:
                    continue
                if msg is None:
                    stash.append(None)
                    raise RuntimeError("vpengine closed its output (exit code %s)" % self.p.poll())
                if msg.get("id") == rid:
                    return msg
                stash.append(msg)
        finally:
            for m in stash:
                self._q.put(m)

    def call(self, cmd, params=None, timeout=None):
        """Raw response object {id, ok, result|error}."""
        return self.wait(self.send(cmd, params), timeout)

    def request(self, cmd, params=None, timeout=None):
        msg = self.call(cmd, params, timeout)
        if not msg.get("ok"):
            raise EngineError(cmd, msg.get("error"))
        return msg.get("result") or {}

    def wait_event(self, name, pred=None, timeout=None):
        deadline = time.time() + (timeout or self.timeout)
        with self._ev_cv:
            while True:
                for e in self._events:
                    if e.get("event") == name and (pred is None or pred(e)):
                        return e
                if self.p.poll() is not None and not self._t.is_alive():
                    raise RuntimeError("vpengine exited while waiting for event %s" % name)
                left = deadline - time.time()
                if left <= 0:
                    raise TimeoutError("vpengine: no event %s" % name)
                self._ev_cv.wait(min(left, 1.0))

    def take_events(self, job_id):
        """Removes and returns every event of one job (keeps memory bounded across cells)."""
        with self._ev_cv:
            mine = [e for e in self._events if e.get("jobId") == job_id]
            self._events = [e for e in self._events if e.get("jobId") != job_id]
        return mine

    def peak_rss_kb(self):
        """VmHWM of the engine process (Linux /proc); None elsewhere or after exit."""
        return peak_rss_kb(self.p.pid)

    def close(self, timeout=30.0):
        if self.closed:
            return self.p.returncode
        self.closed = True
        try:
            self.peak_rss = self.peak_rss_kb()
        except Exception:  # noqa: BLE001
            self.peak_rss = None
        try:
            if self.p.poll() is None:
                self.call("engine.shutdown", timeout=timeout)
        except Exception:  # noqa: BLE001
            pass
        try:
            self.p.stdin.close()
        except OSError:
            pass
        try:
            code = self.p.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            self.p.kill()
            code = self.p.wait(timeout=10)
        self._t.join(timeout=10)
        try:
            self.p.stdout.close()
        except OSError:
            pass
        self._log.close()
        return code


def peak_rss_kb(pid):
    try:
        with open("/proc/%d/status" % pid) as f:
            for line in f:
                if line.startswith("VmHWM:"):
                    return int(line.split()[1])
    except (OSError, ValueError, IndexError):
        return None
    return None


def children_maxrss_kb():
    """Largest RSS of any waited-for child so far (getrusage); a fallback where /proc is missing."""
    try:
        return resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss
    except (OSError, ValueError):
        return None


# ------------------------------------------------------------------------------------------------------ protocol steps
def load_project(eng, source_path, pair="en-la"):
    proj = eng.request("project.new", {"kind": "subs", "pair": pair, "sourcePath": os.path.abspath(source_path)})
    proj = proj.get("project", proj)
    cues, total = [], None
    while total is None or len(cues) < total:
        page = eng.request("cue.page", {"from": len(cues), "count": 200})
        total = page["total"]
        if not page["cues"]:
            break
        cues.extend(page["cues"])
    return proj, cues


def translate(eng, engines, fidelity, indices=None, timeout=3600.0):
    """Runs one translate job. Returns {jobId, warnings, cues:{index: CueView}, stats, wallMs, batches}."""
    params = {"engines": engines, "fidelity": fidelity}
    if indices is not None:
        params["indices"] = list(indices)
    t0 = time.time()
    start = eng.request("translate.start", params)
    job = start["jobId"]
    done = eng.wait_event("translate.done", lambda e: e.get("jobId") == job, timeout=timeout)
    wall = (time.time() - t0) * 1000.0
    evs = eng.take_events(job)
    errors = [e for e in evs if e["event"] == "translate.error"]
    cues, batches = {}, 0
    for e in evs:
        if e["event"] == "translate.cue":
            batches += 1
            for c in e.get("cues", []):
                cues[c["index"]] = c
    return {"jobId": job, "warnings": start.get("warnings", []), "cues": cues, "stats": done.get("stats", {}),
            "wallMs": round(wall, 1), "batches": batches, "errors": errors}


def cue_details(eng, count):
    return [eng.request("cue.get", {"index": i}) for i in range(count)]


def export_and_verify(eng, source_path, work_dir, fmt=None, macrons=True, emoji=False, rebreak=True):
    ext = os.path.splitext(source_path)[1].lower().lstrip(".") or "srt"
    fmt = fmt or ext
    out = os.path.join(work_dir, "export." + fmt)
    res = eng.request("export.write", {"path": out, "format": fmt, "macrons": macrons, "emoji": emoji,
                                       "rebreak": rebreak, "overwrite": True})
    with open(source_path, "rb") as f:
        src = f.read()
    with open(out, "rb") as f:
        dst = f.read()
    v = verify_structure(src, dst, fmt)
    v["exportWarnings"] = len(res.get("warnings", []))
    v["exportWarningKinds"] = sorted({w.get("kind", "?") for w in res.get("warnings", [])})
    return v


# ------------------------------------------------------------------------------------------------------ structure check
_LEAD_TAGS = re.compile(rb"^((?:\{[^}]*\}|<[^>]+>)*)")
_TRAIL_TAGS = re.compile(rb"((?:</[^>]+>)*)\s*$")


def _blocks(data):
    """Blank-line separated blocks of lines; lines keep their bytes except the LF (a CR stays in the line)."""
    blocks, cur = [], []
    for line in data.split(b"\n"):
        if line.strip(b"\r\t ") == b"":
            if cur:
                blocks.append(cur)
                cur = []
        else:
            cur.append(line)
    if cur:
        blocks.append(cur)
    return blocks


def _split_block(block):
    """(header lines up to and including the timing line, text lines)."""
    for i, line in enumerate(block):
        if b"-->" in line:
            return block[:i + 1], block[i + 1:]
    return block, []


def _tags(text_lines):
    if not text_lines:
        return b"", b""
    lead = _LEAD_TAGS.match(text_lines[0].lstrip(b"\xef\xbb\xbf")).group(1)
    trail = _TRAIL_TAGS.search(text_lines[-1].rstrip(b"\r")).group(1)
    return lead, trail


def verify_structure(src, dst, fmt="srt"):
    """Compares the non-text bytes of two subtitle documents. Result: {ok, format, cuesSource, cuesOutput,
    headerDiffs:[block numbers], tagDiffs:[block numbers], blankLinesEqual, finalNewline}."""
    fmt = (fmt or "srt").lower()
    res = {"format": fmt, "headerDiffs": [], "tagDiffs": []}
    if fmt == "ass" or fmt == "ssa":
        def dialogue(data):
            out = []
            for line in data.split(b"\n"):
                if line.startswith(b"Dialogue:"):
                    parts = line.split(b",", 9)
                    text = parts[9] if len(parts) > 9 else b""
                    out.append((b",".join(parts[:9]), _LEAD_TAGS.match(text).group(1)))
            return out
        a, b = dialogue(src), dialogue(dst)
        res["cuesSource"], res["cuesOutput"] = len(a), len(b)
        for i, (x, y) in enumerate(zip(a, b)):
            if x[0] != y[0]:
                res["headerDiffs"].append(i)
            if x[1] != y[1]:
                res["tagDiffs"].append(i)
        res["blankLinesEqual"] = True
    elif fmt == "txt":
        a, b = _blocks(src), _blocks(dst)
        res["cuesSource"], res["cuesOutput"] = len(a), len(b)
        res["blankLinesEqual"] = True
    else:
        a, b = _blocks(src), _blocks(dst)
        cue_a = [x for x in a if any(b"-->" in l for l in x)]
        cue_b = [x for x in b if any(b"-->" in l for l in x)]
        res["cuesSource"], res["cuesOutput"] = len(cue_a), len(cue_b)
        for i, (x, y) in enumerate(zip(a, b)):
            hx, tx = _split_block(x)
            hy, ty = _split_block(y)
            if hx != hy:
                res["headerDiffs"].append(i)
            elif tx and _tags(tx) != _tags(ty):
                res["tagDiffs"].append(i)
        if len(a) != len(b):
            res["headerDiffs"].append(min(len(a), len(b)))
        sep = b"\r\n\r\n" if b"\r\n\r\n" in src else b"\n\n"
        res["blankLinesEqual"] = src.count(sep) == dst.count(sep)
    res["finalNewline"] = dst.endswith(b"\n") == src.endswith(b"\n")
    res["ok"] = (res["cuesSource"] == res["cuesOutput"] and not res["headerDiffs"] and not res["tagDiffs"]
                 and res["blankLinesEqual"] and res["finalNewline"])
    return res


# ------------------------------------------------------------------------------------------------------ text, gold
_TAG_TEXT = re.compile(r"\{\\[^}]*\}|<[^>]+>")
_SOUND = re.compile(r"\[[^\]]*\]|\([^)]*\)|[♪♫♩♬]")


def strip_tags(s):
    return _TAG_TEXT.sub("", s or "")


def counted(source):
    """PREPLAN 6.1: a cue counts when its text, without sound descriptions ([music], (laughs)) and music notes,
    contains at least one letter. Others are reported separately as sound-only cues."""
    rest = _SOUND.sub(" ", strip_tags(source))
    return any(ch.isalpha() for ch in rest)


def one_line(s):
    return " ".join(strip_tags(s or "").split())


def normalise(s):
    """Gold-comparison key: tags removed, compatibility decomposition (NFKD, folds precomposed letters such as
    U+0101 a-macron), lower case, every combining mark dropped (macron, breve, diaeresis, accents), punctuation,
    symbols (emoji included) and whitespace collapsed to single spaces, v -> u and j -> i."""
    s = unicodedata.normalize("NFC", strip_tags(s or "")).lower()
    s = unicodedata.normalize("NFKD", s)
    out = []
    for ch in s:
        cat = unicodedata.category(ch)
        if cat.startswith("M") or ch in "‍️︎":
            continue
        if cat[0] in "PSZC" or ch.isspace():
            out.append(" ")
        else:
            out.append(ch)
    s = "".join(out).replace("v", "u").replace("j", "i")
    return " ".join(s.split())


def gold_match(target, alternatives):
    if not alternatives:
        return None
    key = normalise(target)
    return any(key == normalise(a) for a in alternatives if a.strip())


def load_gold(path):
    """One line per cue; lines starting with '#' are comments; alternatives separated by ' | '; an empty line or
    a lone '-' means no gold for that cue (None)."""
    out = []
    with open(path, encoding="utf-8-sig") as f:
        for line in f:
            line = line.rstrip("\r\n")
            if line.lstrip().startswith("#"):
                continue
            alts = [a.strip() for a in line.split(" | ") if a.strip()]
            out.append(alts if alts and alts != ["-"] else None)
    while out and out[-1] is None:
        out.pop()
    return out


# ------------------------------------------------------------------------------------------------------ statistics
def _betacf(a, b, x):
    """Continued fraction of the incomplete beta function (modified Lentz)."""
    tiny = 1e-300
    qab, qap, qam = a + b, a + 1.0, a - 1.0
    c, d = 1.0, 1.0 - qab * x / qap
    d = 1.0 / (d if abs(d) > tiny else tiny)
    h = d
    for m in range(1, 1000):
        m2 = 2 * m
        aa = m * (b - m) * x / ((qam + m2) * (a + m2))
        d = 1.0 + aa * d
        d = 1.0 / (d if abs(d) > tiny else tiny)
        c = 1.0 + aa / c
        c = c if abs(c) > tiny else tiny
        h *= d * c
        aa = -(a + m) * (qab + m) * x / ((a + m2) * (qap + m2))
        d = 1.0 + aa * d
        d = 1.0 / (d if abs(d) > tiny else tiny)
        c = 1.0 + aa / c
        c = c if abs(c) > tiny else tiny
        delta = d * c
        h *= delta
        if abs(delta - 1.0) < 1e-15:
            break
    return h


def betainc(a, b, x):
    """Regularised incomplete beta function I_x(a, b)."""
    if x <= 0.0:
        return 0.0
    if x >= 1.0:
        return 1.0
    lbt = math.lgamma(a + b) - math.lgamma(a) - math.lgamma(b) + a * math.log(x) + b * math.log1p(-x)
    bt = math.exp(lbt)
    if x < (a + 1.0) / (a + b + 2.0):
        return bt * _betacf(a, b, x) / a
    return 1.0 - bt * _betacf(b, a, 1.0 - x) / b


def beta_ppf(q, a, b):
    """Quantile of the Beta(a, b) distribution by bisection on betainc."""
    lo, hi = 0.0, 1.0
    for _ in range(200):
        mid = (lo + hi) / 2.0
        if betainc(a, b, mid) < q:
            lo = mid
        else:
            hi = mid
        if hi - lo < 1e-15:
            break
    return (lo + hi) / 2.0


def clopper_pearson(k, n, conf=0.95):
    """Exact two-sided interval for k successes in n trials; (None, None) when n == 0."""
    if n <= 0:
        return (None, None)
    alpha = 1.0 - conf
    lo = 0.0 if k <= 0 else beta_ppf(alpha / 2.0, k, n - k + 1)
    hi = 1.0 if k >= n else beta_ppf(1.0 - alpha / 2.0, k + 1, n - k)
    return (lo, hi)


def proportion(k, n):
    lo, hi = clopper_pearson(k, n)
    return {"num": k, "den": n, "rate": (float(k) / n) if n else None,
            "ci95": [round(lo, 6), round(hi, 6)] if n else None}


def rule_of_three(n):
    return 3.0 / n if n else None


def binom_two_sided_half(k, n):
    """Exact two-sided p for k of n under p = 0.5 (used by McNemar)."""
    if n == 0:
        return 1.0
    k = min(k, n - k)
    tail = sum(math.comb(n, i) for i in range(0, k + 1)) / float(2 ** n)
    return min(1.0, 2.0 * tail)


def mcnemar_exact(a_wrong_b_right, a_right_b_wrong):
    """Paired exact McNemar test on the discordant pairs b, c. Returns {b, c, n, p}."""
    b, c = int(a_wrong_b_right), int(a_right_b_wrong)
    return {"b": b, "c": c, "n": b + c, "p": binom_two_sided_half(b, b + c)}


def cohen_kappa(pairs):
    """pairs: list of (label1, label2). Returns {n, observed, expected, kappa} (kappa None when undefined)."""
    pairs = [(a, b) for a, b in pairs]
    n = len(pairs)
    if n == 0:
        return {"n": 0, "observed": None, "expected": None, "kappa": None}
    labels = sorted({x for p in pairs for x in p})
    po = sum(1 for a, b in pairs if a == b) / float(n)
    pe = 0.0
    for l in labels:
        pe += (sum(1 for a, _ in pairs if a == l) / float(n)) * (sum(1 for _, b in pairs if b == l) / float(n))
    kappa = None if abs(1.0 - pe) < 1e-12 else (po - pe) / (1.0 - pe)
    return {"n": n, "observed": round(po, 6), "expected": round(pe, 6),
            "kappa": None if kappa is None else round(kappa, 6)}


def pct(x, digits=1):
    return "n/a" if x is None else ("%." + str(digits) + "f %%") % (100.0 * x)


def fmt_prop(p, digits=1):
    """'k/n = r % (95 % CI lo-hi %)'."""
    if not p or not p.get("den"):
        return "0/0 (no cues)"
    lo, hi = p["ci95"]
    return "%d/%d = %s (95 %% CI %s-%s)" % (p["num"], p["den"], pct(p["rate"], digits), pct(lo, digits)[:-2],
                                             pct(hi, digits))


# ------------------------------------------------------------------------------------------------------ held-out hygiene
def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


HELDOUT_META = ("README.md", "FROZEN.sha256", "BURNED.txt")


def heldout_guard(heldout_dir=HELDOUT_DIR):
    """Recomputes the SHA-256 of every file listed in FROZEN.sha256 and flags data files not listed there.
    Returns {ok, checked, mismatched:[names], missing:[names], unlisted:[names], reason}."""
    res = {"ok": False, "checked": 0, "mismatched": [], "missing": [], "unlisted": [], "reason": ""}
    frozen = os.path.join(heldout_dir, "FROZEN.sha256")
    if not os.path.isfile(frozen):
        res["reason"] = "FROZEN.sha256 not found in " + heldout_dir
        return res
    listed = {}
    with open(frozen, encoding="utf-8") as f:
        for line in f:
            parts = line.strip().split()
            if len(parts) >= 2 and re.match(r"^[0-9a-f]{64}$", parts[0]):
                listed[parts[-1].lstrip("*")] = parts[0]
    for name, want in sorted(listed.items()):
        path = os.path.join(heldout_dir, name)
        if not os.path.isfile(path):
            res["missing"].append(name)
            continue
        res["checked"] += 1
        if sha256_file(path) != want:
            res["mismatched"].append(name)
    for name in sorted(os.listdir(heldout_dir)):
        if os.path.isfile(os.path.join(heldout_dir, name)) and name not in listed and name not in HELDOUT_META:
            res["unlisted"].append(name)
    res["ok"] = bool(listed) and not res["mismatched"] and not res["missing"] and not res["unlisted"]
    if not res["ok"]:
        res["reason"] = "held-out set changed since it was frozen: mismatched %s, missing %s, unlisted %s" % (
            res["mismatched"] or "-", res["missing"] or "-", res["unlisted"] or "-")
    return res


def read_burned(heldout_dir=HELDOUT_DIR):
    """BURNED.txt: one '<file name> <cue number>' per line (cue number = the file's own label, idRaw); '#' comments.
    Returns {file name: set of idRaw strings}."""
    path = os.path.join(heldout_dir, "BURNED.txt")
    out = {}
    if not os.path.isfile(path):
        return out
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.split("#", 1)[0].strip()
            if not line:
                continue
            parts = line.split()
            if len(parts) >= 2:
                out.setdefault(parts[0], set()).add(parts[1])
    return out


def is_under(path, directory):
    a, b = os.path.realpath(path), os.path.realpath(directory)
    return a == b or a.startswith(b + os.sep)


# ------------------------------------------------------------------------------------------------------ misc
def targets_hash(targets):
    """SHA-256 over the targets in cue order, each followed by a NUL byte (multi-line targets cannot collide)."""
    h = hashlib.sha256()
    for t in targets:
        h.update((t or "").encode("utf-8"))
        h.update(b"\x00")
    return h.hexdigest()


def seeded_sample(items, n, seed):
    items = sorted(items)
    if len(items) <= n:
        return list(items)
    return sorted(random.Random(seed).sample(items, n))


def scratch_dir(prefix="vp-eval-"):
    return tempfile.mkdtemp(prefix=prefix)


def remove_dir(path):
    shutil.rmtree(path, ignore_errors=True)


def write_json(path, obj):
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8", newline="\n") as f:
        json.dump(obj, f, ensure_ascii=False, indent=2, sort_keys=False)
        f.write("\n")
    os.replace(tmp, path)


def write_jsonl(path, rows):
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8", newline="\n") as f:
        for r in rows:
            f.write(json.dumps(r, ensure_ascii=False) + "\n")
    os.replace(tmp, path)


def read_jsonl(path):
    with open(path, encoding="utf-8") as f:
        return [json.loads(l) for l in f if l.strip()]
