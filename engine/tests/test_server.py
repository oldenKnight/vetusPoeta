#!/usr/bin/env python3
"""Integration test for `vpengine serve` (DESIGN section 9): a full session over the JSON-lines protocol, framing
errors, a 10,000-line ping burst, ping latency during a 5,000-cue job, cancel, kill -9 + project.recover, clean
exit on EOF, and a check that the binary has no network symbols. These parts run the stub engine (VP_FORCE_STUB=1)
on the fixture lexicon, so they are the same on every machine.
Real engine (C8, skipped with a message when data/work/latin.vpl or data/work/nlp/english.*.vpt are absent): a
session on tests/samples/sample.en.srt with the rules engine (Latin for all 12 cues, reasons in the UI shapes of
DESIGN 9.2, word.inspect, words.list, export byte for byte, names.set, corrections, speaker gender), the pair
report when the NLP models are missing, the online check through the scripted transport (VP_ONLINE_MOCK=1, no
network), and a local-model run (skipped without a model file or without the local model in the build).
Python 3 stdlib only.

usage: test_server.py <path/to/vpengine> <work dir> [--stub-only]
Environment: VP_TEST_PING_MS (default 100) ping latency budget during a job; VP_TEST_STUB_ONLY=1 = --stub-only;
VP_MODEL_GGUF or <repo>/models/*.gguf for the model run (VP_LLM_SKIP_MODEL=1 skips it).
"""
import glob
import json
import os
import queue
import re
import shutil
import signal
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
FIXTURE = os.path.join(ROOT, "tests", "fixtures", "cli", "thirty_cues.srt")
LATIN_VPL = os.path.join(ROOT, "tests", "fixtures", "lex", "latin.vpl")
WORK = os.path.join(ROOT, "data", "work")
SAMPLES = os.path.join(ROOT, "tests", "samples")
SANITIZER_MARKS = ("ERROR: AddressSanitizer", "ERROR: LeakSanitizer", "runtime error:", "ThreadSanitizer")

FAILURES = []


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what, flush=True)
    if not cond:
        FAILURES.append(what)
    return cond


class Engine:
    def __init__(self, exe, data, lexicons, log_path, extra_env=None, stub=True):
        env = dict(os.environ)
        env["VP_AUTOSAVE_MS"] = "300"
        env["VP_LOG"] = "info"
        env.pop("VP_STUB_DELAY_US", None)
        env.pop("VP_ONLINE_MOCK", None)
        env.pop("VP_MODEL_GGUF", None)
        env.pop("VP_FORCE_STUB", None)
        if stub:
            env["VP_FORCE_STUB"] = "1"
        if extra_env:
            env.update(extra_env)
        self.log_path = log_path
        self.log = open(log_path, "ab")
        self.p = subprocess.Popen([exe, "serve", "--data", data, "--lexicons", lexicons], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=self.log, env=env, bufsize=0)
        self.q = queue.Queue()
        self.events = []
        self.ev_cv = threading.Condition()
        self.next_id = 1
        self.wlock = threading.Lock()
        self.bad_lines = []
        self.lines = 0
        self.t = threading.Thread(target=self._read, daemon=True)
        self.t.start()

    @property
    def pid(self):
        return self.p.pid

    def _read(self):
        for raw in self.p.stdout:
            self.lines += 1
            if not raw.endswith(b"\n") or b"\r" in raw:
                self.bad_lines.append(raw[:200])
            try:
                msg = json.loads(raw.decode("utf-8"))
            except ValueError:
                self.bad_lines.append(raw[:200])
                continue
            if "event" in msg:
                with self.ev_cv:
                    self.events.append(msg)
                    self.ev_cv.notify_all()
            else:
                self.q.put((time.time(), msg))
        self.q.put((time.time(), None))

    def send_raw(self, data):
        with self.wlock:
            self.p.stdin.write(data)
            self.p.stdin.flush()

    def send(self, cmd, params=None):
        with self.wlock:
            rid = self.next_id
            self.next_id += 1
            line = json.dumps({"id": rid, "cmd": cmd, "params": params or {}}, ensure_ascii=False) + "\n"
            self.p.stdin.write(line.encode("utf-8"))
            self.p.stdin.flush()
        return rid

    def wait(self, rid, timeout=30.0, with_time=False):
        deadline = time.time() + timeout
        stash = []
        try:
            while True:
                left = deadline - time.time()
                if left <= 0:
                    raise TimeoutError("no response to id %s" % rid)
                at, msg = self.q.get(timeout=left)
                if msg is None:
                    stash.append((at, msg))
                    raise RuntimeError("engine closed stdout while waiting for id %s" % rid)
                if msg.get("id") == rid:
                    return (at, msg) if with_time else msg
                stash.append((at, msg))
        finally:
            for m in stash:
                self.q.put(m)

    def req(self, cmd, params=None, timeout=30.0):
        msg = self.wait(self.send(cmd, params), timeout)
        if not msg.get("ok"):
            raise AssertionError("%s failed: %s" % (cmd, msg.get("error")))
        return msg["result"]

    def err(self, cmd, params=None, timeout=30.0):
        msg = self.wait(self.send(cmd, params), timeout)
        if msg.get("ok"):
            raise AssertionError("%s unexpectedly succeeded" % cmd)
        return msg["error"]

    def wait_event(self, name, pred=None, timeout=30.0):
        deadline = time.time() + timeout
        with self.ev_cv:
            while True:
                for e in self.events:
                    if e["event"] == name and (pred is None or pred(e)):
                        return e
                left = deadline - time.time()
                if left <= 0:
                    raise TimeoutError("no event " + name)
                self.ev_cv.wait(left)

    def events_of(self, name, job=None):
        with self.ev_cv:
            return [e for e in self.events if e["event"] == name and (job is None or e.get("jobId") == job)]

    def shutdown(self, timeout=20.0):
        try:
            self.req("engine.shutdown", timeout=timeout)
        except Exception as e:  # noqa: BLE001
            print("  shutdown request: %s" % e)
        try:
            self.p.stdin.close()
        except OSError:
            pass
        code = self.p.wait(timeout=timeout)
        self.log.close()
        return code

    def kill(self):
        os.kill(self.p.pid, signal.SIGKILL)
        self.p.wait(timeout=10)
        self.log.close()


def log_clean(path):
    with open(path, "rb") as f:
        text = f.read().decode("utf-8", "replace")
    return not any(m in text for m in SANITIZER_MARKS)


def srt_blocks(data):
    """Splits SRT bytes into blocks of lines (bytes), keeping every byte."""
    blocks, cur = [], []
    for line in data.split(b"\n"):
        if line == b"" and cur:
            blocks.append(cur)
            cur = []
        elif line != b"":
            cur.append(line)
    if cur:
        blocks.append(cur)
    return blocks


def make_big_srt(path, n):
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        t = 0
        for i in range(1, n + 1):
            a, b = t, t + 1500
            fmt = lambda ms: "%02d:%02d:%02d,%03d" % (ms // 3600000, ms // 60000 % 60, ms // 1000 % 60, ms % 1000)
            f.write("%d\n%s --> %s\nLine number %d of the long test file.\n\n" % (i, fmt(a), fmt(b), i))
            t = b + 100


# ------------------------------------------------------------------------------------------------ parts
def part_session(exe, work, lex):
    print("session: hello, settings, project, translate, edit, inspect, export, history")
    data = os.path.join(work, "data")
    eng = Engine(exe, data, lex, os.path.join(work, "session.log"))
    hello = eng.req("engine.hello")
    check(bool(hello["version"]) and hello["dataDir"] == data, "hello: version and dataDir")
    langs = {l["lang"]: l for l in hello["lexicons"]}
    check(sorted(langs) == ["en", "es", "grc", "la"], "hello: four lexicon slots")
    check(langs["la"]["available"] and langs["la"]["lemmas"] > 0 and langs["la"]["notice"], "hello: latin.vpl opened")
    check(not langs["grc"]["available"], "hello: missing greek.vpl reported, not an error")
    check(hello["model"]["available"] is False and hello["threads"] >= 1, "hello: model unavailable, threads")

    s0 = eng.req("settings.get")
    s1 = eng.req("settings.set", {"patch": {"cps.adult": 15, "theme": "dark"}})
    s2 = eng.req("settings.get")
    check(s0["cps"]["adult"] == 17 and s1["cps"]["adult"] == 15 and s2["theme"] == "dark", "settings round trip")
    check(eng.err("settings.set", {"patch": {"textScale": 9999}})["code"] == "bad_params", "settings: bad value refused")
    check(os.path.isfile(os.path.join(data, "settings.json")), "settings saved in the data folder")
    eng.req("settings.set", {"patch": {"cps.adult": None, "theme": None}})

    check(eng.err("cue.page")["code"] == "bad_params", "cue.page without project -> bad_params")
    proj = eng.req("project.new", {"kind": "subs", "pair": "en-la", "sourcePath": FIXTURE})["project"]
    check(proj["stats"]["total"] == 30 and proj["subs"]["format"] == "srt", "project.new: 30 cues")
    page = eng.req("cue.page", {"from": 0, "count": 200})
    check(page["total"] == 30 and len(page["cues"]) == 30, "cue.page returns all 30")
    c4 = page["cues"][3]
    check(c4["source"] == "Rome, many years ago." and c4["index"] == 3 and c4["idRaw"] == "4" and c4["start"] == 10000 and c4["durationMs"] == 2400,
          "CueView: source without tags, idRaw, timing")
    check(c4["confidence"] == "check" and c4["state"] == "new" and c4["target"] == "", "new cue: state new, no target")
    check(eng.err("cue.get", {"index": 30})["code"] == "bad_params", "cue.get out of range -> bad_params")
    page2 = eng.req("cue.page", {"from": 25, "count": 10})
    check(len(page2["cues"]) == 5 and page2["cues"][0]["index"] == 25, "cue.page window at the end")

    job = eng.req("translate.start", {"engines": {"rules": True, "model": False, "online": False}, "fidelity": 2})["jobId"]
    done = eng.wait_event("translate.done", lambda e: e["jobId"] == job)
    cue_events = eng.events_of("translate.cue", job)
    sent = sum(len(e["cues"]) for e in cue_events)
    check(done["stats"]["done"] == done["stats"]["translated"] == 30 and not done["stats"]["cancelled"],
          "translate.done: 30 cues")
    check(sent == 30 and all(len(e["cues"]) <= 20 for e in cue_events), "translate.cue: 30 cues in batches <= 20")
    check(len(eng.events_of("translate.progress", job)) >= 1, "translate.progress emitted")
    c1 = eng.req("cue.get", {"index": 0})
    check(c1["cue"]["state"] == "translated" and c1["cue"]["confidence"] == "check" and
          c1["reasons"][0]["text"] == "stub engine" and len(c1["tokens"]) == 3, "cue.get after stub translation")

    r = eng.req("cue.set", {"index": 4, "text": "Puella mātrī cantat.", "remember": "phrase"})
    check(r["cue"]["state"] == "edited" and r["correctionAdded"]["scope"] == "phrase", "cue.set with remember")
    check(any(c["id"] == r["correctionAdded"]["id"] for c in eng.req("corrections.list")["corrections"]),
          "corrections.list has the new correction")
    eng.req("cue.set", {"index": 8, "text": "Canis 🐕 dormit in sole."})
    eng.req("cue.set", {"index": 12, "text": "Agricola agros suos et parvam casam suam valde amat et semper colit."})
    eng.req("cue.set", {"index": 29, "text": "Valēte, amīcī!"})
    u = eng.req("history.undo")
    check(u["changedIndices"] == [29] and u["canRedo"], "history.undo")
    check(eng.req("cue.get", {"index": 29})["cue"]["target"] == "Goodbye, my friends.", "undo restored the cue")
    eng.req("history.redo")
    check(eng.req("cue.get", {"index": 29})["cue"]["target"] == "Valēte, amīcī!", "redo applied again")
    check(eng.req("cue.review", {"indices": [0, 1, 1], "reviewed": True})["count"] == 2, "cue.review counts cues")

    insp = eng.req("word.inspect", {"text": "puellam", "lang": "la"})
    a = insp["analyses"]
    check(len(a) >= 1 and a[0]["lemma"]["head"] == "puella" and a[0]["features"]["case"] == "accusative",
          "word.inspect puellam -> puella, accusative")
    unk = eng.req("word.inspect", {"text": "puellx", "lang": "la"})
    check(unk["analyses"] == [] and len(unk["suggestions"]) >= 1, "word.inspect unknown word gives suggestions")
    check(eng.err("word.inspect", {"text": "λόγος", "lang": "grc"})["code"] == "lexicon_missing", "grc without lexicon")
    lem = eng.req("lemma.get", {"lang": "la", "id": a[0]["lemma"]["id"]})
    check(len(lem["cells"]) >= 10 and len(lem["senses"]) >= 1, "lemma.get: cells and senses")
    check(eng.req("names.set", {"name": "Rome", "policy": "translate", "form": "Rōma"})["affectedCues"] == [3],
          "names.set reports affected cues")
    check(eng.req("names.list")["names"][0]["form"] == "Rōma", "names.list")
    w = eng.req("words.list")
    check("tierShare" in w and w["tokens"] > 0, "words.list")
    report = os.path.join(work, "report.json")
    eng.req("eval.run", {"outPath": report})
    with open(report, encoding="utf-8") as f:
        rep = json.load(f)
    check(rep["cues"] == 30 and sum(rep["confidence"].values()) == 30, "eval.run writes report.json")
    check(eng.err("model.test")["code"] == "model_missing", "model.test -> model_missing")
    check(eng.req("model.status")["available"] is False, "model.status unavailable")
    check(eng.err("online.test")["code"] == "online_disabled", "online.test -> online_disabled")

    out = os.path.join(work, "out.srt")
    w = eng.req("export.write", {"path": out, "format": "srt", "emoji": False, "macrons": False, "rebreak": True})
    e = eng.err("export.write", {"path": out})
    check(e["code"] == "io" and "exists" in e["message"], "export.write refuses to overwrite")
    eng.req("export.write", {"path": out, "format": "srt", "emoji": False, "macrons": False, "overwrite": True})
    with open(FIXTURE, "rb") as f:
        src = f.read()
    with open(out, "rb") as f:
        dst = f.read()
    sb, db = srt_blocks(src), srt_blocks(dst)
    check(len(sb) == len(db) == 30, "export: 30 blocks")
    check(all(a[0] == b[0] and a[1] == b[1] for a, b in zip(sb, db)), "export: numbering and timing byte-identical")
    check(src.count(b"\n\n") == dst.count(b"\n\n") and dst.endswith(b"\n"), "export: blank lines kept")
    check(db[3][2].startswith(b"{\\an8}"), "export: {\\an8} kept")
    ital = [i for i, b in enumerate(sb) if b[2].startswith(b"<i>")]
    check(all(db[i][2].startswith(b"<i>") and db[i][-1].endswith(b"</i>") for i in ital), "export: italics kept")
    unchanged = [i for i in range(30) if i not in (1, 2, 4, 8, 12, 19, 29) and len(sb[i]) == 3]
    check(all(sb[i] == db[i] for i in unchanged), "export: untouched cues byte-identical")
    check(db[4][2] == "Puella matri cantat.".encode(), "export: macrons stripped")
    check(db[8][2] == b"Canis dormit in sole.", "export: emoji dropped")
    check(all(len(l.decode()) <= 42 for l in db[12][2:]) and len(db[12]) == 4, "export: long cue re-broken in 2 lines")
    check(any(x["kind"] == "cps" for x in w["warnings"]), "export: cps warning reported")
    vtt = os.path.join(work, "out.vtt")
    eng.req("export.write", {"path": vtt, "format": "vtt"})
    with open(vtt, "rb") as f:
        v = f.read()
    check(v.startswith(b"WEBVTT\n\n1\n00:00:01.000 --> 00:00:03.400\n"), "export: SRT -> VTT")
    pv = eng.req("export.preview", {"indices": [4, 29], "macrons": True})["cues"]
    check(pv[0]["lines"] == ["Puella mātrī cantat."] and pv[1]["index"] == 29, "export.preview")

    proj_path = os.path.join(work, "session.vpoeta")
    eng.req("project.saveAs", {"path": proj_path})
    check(os.path.isfile(proj_path) and os.path.isfile(proj_path + ".lock"), "saveAs writes project and lock")
    unsaved = os.path.join(data, "unsaved")
    check(not any(f.endswith(".autosave") or f.endswith(".lock") for f in os.listdir(unsaved)),
          "unsaved autosave and lock removed after saveAs")
    eng.req("project.close")
    check(not os.path.exists(proj_path + ".lock"), "close removes the lock")
    reopened = eng.req("project.open", {"path": proj_path})
    check(reopened["recoverable"] is None and reopened["project"]["stats"]["edited"] == 4, "project.open after save")
    check(eng.req("cue.get", {"index": 4})["cue"]["target"] == "Puella mātrī cantat.", "edited text survived save/open")
    eng.req("project.new", {"kind": "text", "pair": "la-en", "text": "Prima pars.\n\nSecunda pars textus."})
    check(eng.req("cue.page", {"from": 0, "count": 10})["total"] == 2, "text project: paragraphs are pseudo-cues")
    check(eng.shutdown() == 0, "engine.shutdown exits with 0")
    check(not os.path.exists(proj_path + ".lock"), "no lock left after shutdown")
    check(not eng.bad_lines, "every output line is one JSON object ending in LF")
    check(log_clean(eng.log_path), "no sanitizer report in the session log")


def part_framing(exe, work, lex):
    print("framing: malformed lines, unknown commands, 10,000-line ping burst")
    eng = Engine(exe, os.path.join(work, "data2"), lex, os.path.join(work, "framing.log"))
    eng.send_raw(b"this is not json\n{\n[]\n{\"id\":5}\n{\"id\":6,\"cmd\":\"x\",\"params\":3}\n\n   \n")
    eng.send_raw(b"{\"id\":7,\"cmd\":\"no.such.command\"}\n")
    eng.send_raw("{\"id\":8,\"cmd\":\"engine.ping\",\"params\":{\"text\":\"\\u00e6\\n\\u0000 ok\"}}\n".encode())
    eng.send_raw(b"\xff\xfe\x00bad bytes\n")
    msgs = []
    deadline = time.time() + 10
    while len(msgs) < 8 and time.time() < deadline:
        try:
            msgs.append(eng.q.get(timeout=1)[1])
        except queue.Empty:
            pass
    nulls = [m for m in msgs if m and m.get("id") is None]
    check(len(nulls) >= 4 and all(m["error"]["code"] == "bad_params" and m["error"]["hint"] for m in nulls),
          "malformed lines -> id:null bad_params with hint")
    by_id = {m["id"]: m for m in msgs if m and m.get("id") is not None}
    check(by_id.get(5, {}).get("error", {}).get("code") == "bad_params", "missing cmd -> bad_params")
    check(by_id.get(6, {}).get("error", {}).get("code") == "bad_params", "params not an object -> bad_params")
    check(by_id.get(7, {}).get("error", {}).get("code") == "bad_params", "unknown command answered")
    check(by_id.get(8, {}).get("ok") is True, "ping with escapes answered")
    check(eng.p.poll() is None and eng.req("engine.ping") == {}, "server alive after bad input")

    n = 10000
    first = eng.next_id
    eng.next_id += n
    payload = "".join('{"id":%d,"cmd":"engine.ping"}\n' % (first + i) for i in range(n)).encode()
    t0 = time.time()
    th = threading.Thread(target=eng.send_raw, args=(payload,))
    th.start()
    got = []
    while len(got) < n and time.time() - t0 < 20:
        try:
            _, m = eng.q.get(timeout=1)
        except queue.Empty:
            continue
        if m and isinstance(m.get("id"), int) and m["id"] >= first:
            got.append(m["id"])
    th.join()
    dt = time.time() - t0
    check(len(got) == n and got == sorted(got), "10,000 pings answered in order")
    check(dt < 10.0, "10,000 pings in %.2f s (< 10 s)" % dt)
    check(eng.shutdown() == 0 and not eng.bad_lines, "clean shutdown after the burst")
    check(log_clean(eng.log_path), "no sanitizer report in the framing log")


def part_long_job(exe, work, lex):
    print("long job: ping during a 5,000-cue translate, cancel")
    big = os.path.join(work, "big.srt")
    make_big_srt(big, 5000)
    eng = Engine(exe, os.path.join(work, "data3"), lex, os.path.join(work, "longjob.log"),
                 {"VP_STUB_DELAY_US": "300"})
    eng.req("project.new", {"kind": "subs", "pair": "en-la", "sourcePath": big})
    job = eng.req("translate.start", {"engines": {"rules": True}})["jobId"]
    eng.wait_event("translate.cue", lambda e: e["jobId"] == job)
    budget = float(os.environ.get("VP_TEST_PING_MS", "100")) / 1000.0
    lat = []
    for _ in range(5):
        t0 = time.time()
        at, m = eng.wait(eng.send("engine.ping"), timeout=5, with_time=True)
        lat.append(at - t0)
        time.sleep(0.05)
    with eng.ev_cv:
        running = not any(e["event"] == "translate.done" and e["jobId"] == job for e in eng.events)
    check(running, "the job was still running while pinged")
    check(max(lat) < budget, "ping during the job: max %.1f ms (< %d ms)" % (max(lat) * 1000, budget * 1000))
    # The worker is busy with the job: 1,100 queued requests overflow the 1,024-entry queue.
    first = eng.next_id
    eng.next_id += 1100
    eng.send_raw("".join('{"id":%d,"cmd":"settings.get"}\n' % (first + i) for i in range(1100)).encode())
    answers = {}
    deadline = time.time() + 60
    while len(answers) < 1100 and time.time() < deadline:
        try:
            _, m = eng.q.get(timeout=1)
        except queue.Empty:
            continue
        if m and isinstance(m.get("id"), int) and m["id"] >= first:
            answers[m["id"]] = m
    busy = sum(1 for m in answers.values() if not m["ok"] and m["error"]["code"] == "busy")
    check(len(answers) == 1100 and busy >= 1 and len(answers) - busy >= 1024 - 1,
          "bounded queue: %d busy, %d served, every request answered" % (busy, len(answers) - busy))
    done = eng.wait_event("translate.done", lambda e: e["jobId"] == job, timeout=120)
    prog = eng.events_of("translate.progress", job)
    gaps = [b["done"] - a["done"] for a, b in zip(prog, prog[1:])]
    check(done["stats"]["done"] == 5000 and len(prog) >= 5 and min(gaps or [1]) >= 0, "5,000 cues with progress events")
    check(len(prog) <= 5000 // 20 + 20, "progress throttled (%d events)" % len(prog))

    job2 = eng.req("translate.start", {"indices": list(range(5000))})["jobId"]
    eng.wait_event("translate.cue", lambda e: e["jobId"] == job2)
    eng.req("translate.cancel", {"jobId": job2})
    d2 = eng.wait_event("translate.done", lambda e: e["jobId"] == job2, timeout=30)
    check(d2["stats"]["cancelled"] and d2["stats"]["done"] < 5000, "translate.cancel stops at a cue (%d done)" % d2["stats"]["done"])
    check(eng.req("cue.page", {"from": 4990, "count": 10})["total"] == 5000, "engine responsive after cancel")
    check(eng.shutdown() == 0 and not eng.bad_lines, "clean shutdown after the long job")
    check(log_clean(eng.log_path), "no sanitizer report in the long-job log")


def part_crash(exe, work, lex):
    print("crash: kill -9 mid-session, then project.open reports recoverable and project.recover works")
    data = os.path.join(work, "data4")
    proj = os.path.join(work, "crash.vpoeta")
    a = Engine(exe, data, lex, os.path.join(work, "crash_a.log"))
    a.req("project.new", {"kind": "subs", "pair": "en-la", "sourcePath": FIXTURE})
    a.req("project.saveAs", {"path": proj})
    a.req("cue.set", {"index": 6, "text": "Puer aquam ex puteō portat."})
    ev = a.wait_event("project.autosaved", timeout=15)
    check(ev["path"] == proj + ".autosave" and os.path.isfile(proj + ".autosave"), "autosave event and file")
    a.kill()
    check(os.path.isfile(proj + ".lock"), "lock left behind by the killed engine")
    b = Engine(exe, data, lex, os.path.join(work, "crash_b.log"))
    o = b.req("project.open", {"path": proj})
    check(o["recoverable"] is not None and o["recoverable"]["autosavePath"] == proj + ".autosave",
          "project.open reports recoverable")
    check(b.req("cue.get", {"index": 6})["cue"]["target"] == "", "the saved file has the old state")
    r = b.req("project.recover", {"path": proj})
    check(r["path"] == proj, "project.recover answers {path, at}")
    check(b.req("cue.get", {"index": 6})["cue"]["target"] == "Puer aquam ex puteō portat.", "recovered edit is back")
    b.req("project.save")
    check(not os.path.exists(proj + ".autosave"), "save removes the autosave")
    check(b.shutdown() == 0, "second engine exits cleanly")
    c = Engine(exe, data, lex, os.path.join(work, "crash_c.log"))
    o = c.req("project.open", {"path": proj})
    check(o["recoverable"] is None and c.req("cue.get", {"index": 6})["cue"]["state"] == "edited",
          "after save: nothing to recover, edit persisted")
    c.req("cue.set", {"index": 7, "text": "Audī ventum."})
    c.p.stdin.close()   # EOF: the dirty project is autosaved and the lock removed
    check(c.p.wait(timeout=20) == 0, "EOF on stdin exits with 0")
    c.log.close()
    check(os.path.isfile(proj + ".autosave") and not os.path.exists(proj + ".lock"), "EOF: autosave written, lock removed")
    for log in ("crash_a.log", "crash_b.log", "crash_c.log"):
        check(log_clean(os.path.join(work, log)), "no sanitizer report in " + log)


def part_offline(exe):
    print("offline: no network symbols in the binary (no strace needed)")
    nm = shutil.which("nm")
    if not nm:
        print("  skip (nm not found)")
        return
    syms = ""
    for args in ([nm, "-D", "--undefined-only", exe], [nm, exe]):
        try:
            syms += subprocess.run(args, capture_output=True, text=True, timeout=60).stdout
        except (OSError, subprocess.SubprocessError):
            pass
    if not syms.strip():
        print("  skip (no symbols readable)")
        return
    check("fwrite" in syms or "WriteFile" in syms, "symbol table readable (sanity)")
    bad = re.compile(r"\b(socket|connect|getaddrinfo|gethostbyname|gethostbyname_r|WSAStartup|curl_easy_init|"
                     r"InternetOpenW?|WinHttpOpen)(@\S*)?$")
    hits = sorted({l.split()[-1] for l in syms.splitlines() if l.split() and bad.search(l.split()[-1])})
    check(not hits, "no network symbols (%s)" % (", ".join(hits) or "none"))


# ------------------------------------------------------------------------------------------------ real engine (C8)
def real_data_missing():
    need = [os.path.join(WORK, "latin.vpl"), os.path.join(WORK, "nlp", "english.tag.vpt"),
            os.path.join(WORK, "nlp", "english.dep.vpt")]
    missing = [p for p in need if not os.path.isfile(p)]
    return missing


def latin_tokens_ok(cue, detail):
    """The target is Latin made by the engine: not the source, tokens found in order, most words in the lexicon."""
    t = cue["target"]
    if not t or t == cue["source"]:
        return False
    toks = detail["tokens"]
    pos = 0
    for k in toks:
        at = t.find(k["text"], pos)
        if at < 0:
            return False
        pos = at + len(k["text"])
    words = [k for k in toks if not k["text"].startswith("[")]
    known = [k for k in words if "lemmaId" in k]
    return len(words) > 0 and len(known) * 2 >= len(words)


REASON_SHAPES = {
    "sense": {"source", "sense", "context"},
    "candidate": {"lemmaId", "head", "form", "tier", "band", "chosen", "gloss"},
    "form": {"features"},
    "evidence": {"source", "state"},
}


def check_reason_shapes(detail, what):
    rs = detail["reasons"]
    by_kind = {}
    for r in rs:
        by_kind.setdefault(r["kind"], []).append(r)
    ok = True
    for kind, keys in REASON_SHAPES.items():
        lst = [r for r in by_kind.get(kind, []) if isinstance(r["data"], dict) and r["tokenIndex"] >= 0]
        if kind == "candidate":
            lst = [r for r in lst if "was" not in r["data"]]
        if not lst:
            ok = check(False, "%s: a %s reason on a token" % (what, kind)) and ok
            continue
        bad = [r for r in lst if not keys <= set(r["data"])]
        ok = check(not bad, "%s: %s reasons carry %s" % (what, kind, sorted(keys))) and ok
    ev = [r["data"] for r in by_kind.get("evidence", []) if isinstance(r["data"], dict) and r["tokenIndex"] >= 0]
    srcs = {e["source"] for e in ev}
    ok = check(srcs == {"wiktionary", "whitaker", "model", "online"} and
               all(e["state"] in ("yes", "no", "off", "none") for e in ev),
               "%s: evidence rows for wiktionary/whitaker/model/online with yes|no|off|none" % what) and ok
    cands = {}
    for r in by_kind.get("candidate", []):
        if isinstance(r["data"], dict) and "was" not in r["data"]:
            cands.setdefault(r["tokenIndex"], []).append(r["data"]["chosen"])
    ok = check(all(v.count(True) == 1 for v in cands.values()), "%s: one chosen candidate per token" % what) and ok
    toks = detail["tokens"]
    ok = check(all(-1 <= r["tokenIndex"] < len(toks) for r in rs), "%s: reason tokenIndex inside tokens" % what) and ok
    return ok


def part_real(exe, work, _lex):
    print("real engine: rules engine on tests/samples/sample.en.srt (data/work lexicons and NLP models)")
    missing = real_data_missing()
    if missing:
        print("  SKIP real-engine session: missing " + ", ".join(missing))
        return
    data = os.path.join(work, "data")
    eng = Engine(exe, data, WORK, os.path.join(work, "real.log"), stub=False)
    hello = eng.req("engine.hello", timeout=60)
    check(hello.get("engineKind") == "rules", "hello: the rules engine is in use (%s)" % hello.get("engine"))
    check("en-la" in hello["pairs"] and {"R", "O"} <= set(hello["modes"]), "hello: pairs %s, modes %s" % (hello["pairs"], hello["modes"]))
    un = {u["pair"]: u for u in hello["pairsUnavailable"]}
    check(all(u["hint"] and u["code"] != "internal" for u in un.values()), "hello: every unavailable pair has a hint")
    la = [l for l in hello["lexicons"] if l["lang"] == "la"][0]
    check(la["available"] and la["tiers"]["t1"] > 100 and la["tiers"]["t3"] > la["tiers"]["t1"], "hello: lexicon tiers %s" % la["tiers"])
    check(isinstance(hello["model"].get("rerankEnabled"), bool), "hello: model.rerankEnabled")
    samples = {x["lang"]: x["path"] for x in hello["samples"]}
    en_sample = os.path.join(data, "samples", "sample.en.srt")
    check(samples.get("en") == en_sample and os.path.isfile(en_sample) and set(samples) == {"en", "es", "la", "grc"},
          "hello: samples installed in <dataDir>/samples")
    with open(en_sample, "rb") as f, open(os.path.join(SAMPLES, "sample.en.srt"), "rb") as g:
        check(f.read() == g.read(), "installed sample identical to tests/samples")

    proj = eng.req("project.new", {"kind": "subs", "pair": "en-la", "sourcePath": en_sample})["project"]
    check(proj["stats"]["total"] == 12, "sample: 12 cues")
    t0 = time.time()
    tr = eng.req("translate.start", {"engines": {"rules": True, "model": False, "online": False}, "fidelity": 2})
    done = eng.wait_event("translate.done", lambda e: e["jobId"] == tr["jobId"], timeout=300)
    check(done["stats"]["done"] == 12 and not eng.events_of("translate.error", tr["jobId"]),
          "translate: 12 cues in %.1f s" % (time.time() - t0))
    cues = eng.req("cue.page", {"from": 0, "count": 12})["cues"]
    details = [eng.req("cue.get", {"index": i}) for i in range(12)]
    latin = [i for i in range(12) if latin_tokens_ok(cues[i], details[i])]
    check(len(latin) == 12, "Latin for all 12 cues (%d)" % len(latin))
    check(all(c["confidence"] in ("ok", "check", "fix") and c["state"] == "translated" for c in cues),
          "every cue has a confidence (%s)" % [c["confidence"] for c in cues])
    check(all({"A1", "A3", "A4", "A5", "A8"} <= {x["id"] for x in d["checks"]} for d in details), "every cue has checks A1-A9")
    check(all("emoji" in c["flags"] for c in cues if any("emoji" in t for t in details[c["index"]]["tokens"])),
          "cue flag emoji when a token carries an emoji")
    check(all("cps" in c and isinstance(c["flags"], list) for c in cues), "CueView cps and flags")
    check_reason_shapes(details[0], "cue 0")
    off = [r["data"]["state"] for r in details[0]["reasons"] if r["kind"] == "evidence" and isinstance(r["data"], dict)
           and r["data"].get("source") in ("model", "online") and r["tokenIndex"] >= 0]
    check(off and all(x == "off" for x in off), "evidence: model and online off when not asked")

    word = next(t for t in details[0]["tokens"] if "lemmaId" in t)
    insp = eng.req("word.inspect", {"text": word["text"], "lang": "la"})
    a = insp["analyses"]
    check(a and any(x["lemma"]["id"] == word["lemmaId"] for x in a) and all("flags" in x["lemma"] for x in a),
          "word.inspect %s: analyses with the token's lemma" % word["text"])
    check(any(any(x["features"][k] for k in ("case", "number", "person", "tense")) for x in a),
          "word.inspect: features in words (%s)" % a[0]["features"])
    lem = eng.req("lemma.get", {"lang": "la", "id": word["lemmaId"]})
    check(len(lem["cells"]) >= 5 and isinstance(lem["lemma"]["flags"], list), "lemma.get: paradigm cells")
    w = eng.req("words.list")
    share = w["tierShare"]
    check(w["words"] and share["t1"] > 0.3 and sum(share.values()) <= 1.001 and all(x["tier"] in (0, 1, 2, 3) for x in w["words"]),
          "words.list: tiers from the engine's tokens %s" % share)

    out = os.path.join(work, "out.srt")
    ex = eng.req("export.write", {"path": out, "format": "srt", "emoji": False, "macrons": True, "rebreak": True})
    with open(en_sample, "rb") as f:
        src = f.read()
    with open(out, "rb") as f:
        dst = f.read()
    sb, db = srt_blocks(src), srt_blocks(dst)
    check(len(sb) == len(db) == 12 and all(a[0] == b[0] and a[1] == b[1] for a, b in zip(sb, db)),
          "export: numbering and timing byte-identical")
    check(db[4][2].startswith(b"{\\an8}") and not db[4][2].startswith(b"{\\an8}The"), "export: the {\\an8} cue keeps its tag")
    check(db[8][2].startswith(b"<i>") and db[8][-1].endswith(b"</i>"), "export: the italic cue stays italic")
    check(all(b[2:] != a[2:] for a, b in zip(sb, db)), "export: every text line is the Latin")
    check(not [x for x in ex["warnings"] if x["kind"] in ("untranslated", "tags_dropped")], "export: no untranslated/tags_dropped warnings")

    ns = eng.req("names.set", {"name": "Marcus", "policy": "translate", "form": "Quīntus"})
    check(ns["affectedCues"] == [3] and eng.req("cue.get", {"index": 3})["cue"]["state"] == "stale",
          "names.set: cue 3 affected and stale")
    j = eng.req("translate.start", {"indices": ns["affectedCues"]})["jobId"]
    eng.wait_event("translate.done", lambda e: e["jobId"] == j, timeout=120)
    c3 = eng.req("cue.get", {"index": 3})["cue"]
    check(c3["state"] == "translated" and "Quīnt" in c3["target"], "names.set: re-translated with the glossary form (%s)" % c3["target"])

    before2 = eng.req("cue.get", {"index": 2})["cue"]["target"]
    fixed = "Agricola aquam portat."
    eng.req("cue.set", {"index": 2, "text": fixed})
    r = eng.req("cue.set", {"index": 2, "text": fixed, "remember": "phrase"})
    check(r["correctionAdded"]["target"] == fixed and r["cue"]["state"] == "edited", "cue.set {remember}: correction added")
    u = eng.req("history.undo")
    check(u["changedIndices"] == [2] and eng.req("cue.get", {"index": 2})["cue"]["target"] == before2,
          "remember on the same text adds no second history step (one undo restores the translation)")
    eng.req("history.redo")
    check(len(eng.req("corrections.list")["corrections"]) == 1, "corrections.list: one correction")
    j = eng.req("translate.start", {"indices": [2]})["jobId"]
    eng.wait_event("translate.done", lambda e: e["jobId"] == j, timeout=120)
    d2 = eng.req("cue.get", {"index": 2})
    check(d2["cue"]["target"] == fixed and any(x["kind"] == "correction" for x in d2["reasons"]),
          "the next translation of the cue uses the correction")
    ed = eng.req("cue.set", {"index": 0, "text": "Puella rosam spectat."})
    d0 = eng.req("cue.get", {"index": 0})
    check(ed["cue"]["state"] == "edited" and latin_tokens_ok(d0["cue"], d0) and
          all(-1 <= x["tokenIndex"] < len(d0["tokens"]) for x in d0["reasons"]), "edit: tokens re-checked, reasons re-attached")

    # speaker gender (settings.speakerGender -> Options.speakerGender) on a text project
    forms = {}
    for g in ("m", "f"):
        eng.req("settings.set", {"patch": {"speakerGender": g}})
        eng.req("project.new", {"kind": "text", "pair": "en-la", "text": "I am tired."})
        j = eng.req("translate.start", {})["jobId"]
        eng.wait_event("translate.done", lambda e, j=j: e["jobId"] == j, timeout=120)
        forms[g] = eng.req("cue.get", {"index": 0})["cue"]["target"]
    check(forms["m"] != forms["f"], "speakerGender: m %r, f %r" % (forms["m"], forms["f"]))
    eng.req("settings.set", {"patch": {"speakerGender": None}})

    # Latin -> English (C11): source-side tokens and "analysis" reasons pass through; words.list on the Latin side
    if "la-en" in hello["pairs"]:
        eng.req("project.new", {"kind": "subs", "pair": "la-en", "sourcePath": samples.get("la")})
        j = eng.req("translate.start", {})["jobId"]
        eng.wait_event("translate.done", lambda e: e["jobId"] == j, timeout=120)
        d = eng.req("cue.get", {"index": 0})
        kinds = {r["kind"] for r in d["reasons"]}
        check(d["cue"]["target"] and "analysis" in kinds and "evidence" in kinds, "la-en: target %r, reasons %s" % (d["cue"]["target"], sorted(kinds)))
        wl = eng.req("words.list")
        check(wl["words"] and wl["unknown"] < wl["tokens"], "la-en: words.list from the Latin side (%d words)" % len(wl["words"]))
    # a pair the engine does not serve: a hint, not lexicon_missing
    if "la-en" not in hello["pairs"]:
        eng.req("project.new", {"kind": "text", "pair": "la-en", "text": "Puella rosam videt."})
        e = eng.err("translate.start", {})
        check(e["code"] != "lexicon_missing" and e["hint"], "unavailable pair la-en: %s (%s)" % (e["code"], e["hint"]))
    # Orbergise: a Latin project; refused with a hint while the rules engine has no Latin -> Latin rewrite
    la_sample = samples.get("la")
    eng.req("project.new", {"kind": "subs", "pair": "la-la", "sourcePath": la_sample})
    if "la-la" in hello["pairs"]:
        j = eng.req("orbergise.start", {"tier": 1, "originalPath": en_sample})["jobId"]
        eng.wait_event("translate.done", lambda e: e["jobId"] == j, timeout=300)
        d = eng.req("cue.get", {"index": 0})
        check("meaning" in d and d.get("original") == "The girl sees the rose.", "orbergise: meaning and original")
    else:
        e = eng.err("orbergise.start", {"tier": 1, "originalPath": en_sample})
        check(e["code"] != "internal" and e["hint"], "orbergise refused with a hint (%s: %s)" % (e["code"], e["hint"]))
    check(eng.shutdown() == 0 and not eng.bad_lines, "real engine: clean shutdown")
    check(log_clean(eng.log_path), "no sanitizer report in the real-engine log")

    print("real engine: missing NLP models")
    empty = os.path.join(work, "no-nlp")
    os.makedirs(empty)
    eng = Engine(exe, os.path.join(work, "data-nonlp"), WORK, os.path.join(work, "nonlp.log"),
                 {"VP_NLP_DIR": empty}, stub=False)
    hello = eng.req("engine.hello", timeout=60)
    un = {u["pair"]: u for u in hello["pairsUnavailable"]}
    check("en-la" not in hello["pairs"] and un.get("en-la", {}).get("code") == "not_found" and
          "english.tag.vpt" in un["en-la"]["hint"], "no NLP: en-la unavailable with a hint naming the files")
    eng.req("project.new", {"kind": "subs", "pair": "en-la", "sourcePath": os.path.join(SAMPLES, "sample.en.srt")})
    e = eng.err("translate.start", {})
    check(e["code"] == "not_found" and "english" in e["hint"], "no NLP: translate.start refused with the hint, not lexicon_missing")
    check(eng.shutdown() == 0 and log_clean(eng.log_path), "no-NLP engine: clean shutdown")


def part_online_mock(exe, work, _lex):
    print("real engine: online check through the scripted transport (VP_ONLINE_MOCK=1, no network)")
    if real_data_missing():
        print("  SKIP (real data missing)")
        return
    eng = Engine(exe, os.path.join(work, "data"), WORK, os.path.join(work, "online.log"), {"VP_ONLINE_MOCK": "1"}, stub=False)
    hello = eng.req("engine.hello", timeout=60)
    check(hello["online"]["mock"] and not hello["online"]["allowed"], "hello: mock transport, online off by default")
    eng.req("project.new", {"kind": "subs", "pair": "en-la", "sourcePath": os.path.join(SAMPLES, "sample.en.srt")})
    tr = eng.req("translate.start", {"indices": [0, 1], "engines": {"rules": True, "online": True}})
    check(tr["warnings"] == ["online_disabled"], "online asked while off: warning in the result")
    w = eng.wait_event("translate.warning", lambda e: e["jobId"] == tr["jobId"])
    check(w["engine"] == "online" and w["code"] == "online_disabled" and w["hint"], "translate.warning event {engine, code, hint}")
    eng.wait_event("translate.done", lambda e: e["jobId"] == tr["jobId"], timeout=120)
    check(eng.req("engine.hello")["online"]["mockCalls"] == 0, "online off: no transport call")
    eng.req("settings.set", {"patch": {"engines.online": True, "online.wiktionary": True}})
    tr = eng.req("translate.start", {"indices": [0, 1], "engines": {"rules": True, "online": True}})
    check(tr["warnings"] == [], "online on: no warning")
    eng.wait_event("translate.done", lambda e: e["jobId"] == tr["jobId"], timeout=180)
    d = eng.req("cue.get", {"index": 0})
    st = [r["data"]["state"] for r in d["reasons"] if r["kind"] == "evidence" and isinstance(r["data"], dict)
          and r["data"].get("source") == "online" and r["tokenIndex"] >= 0]
    check("yes" in st and "off" not in st, "online evidence rows: %s" % st)
    check(eng.req("engine.hello")["online"]["mockCalls"] > 0, "the scripted transport answered")
    check(any(r["kind"] == "evidence" and isinstance(r["data"], dict) and r["data"].get("source") == "online" and
              r["tokenIndex"] == -1 for r in d["reasons"]), "the engine's own online evidence reason")
    eng.req("settings.set", {"patch": {"engines.online": None, "online.wiktionary": None}})
    check(eng.shutdown() == 0 and log_clean(eng.log_path), "online-mock engine: clean shutdown")


def find_model():
    if os.environ.get("VP_LLM_SKIP_MODEL") == "1":
        return None
    env = os.environ.get("VP_MODEL_GGUF")
    if env and os.path.isfile(env):
        return env
    found = sorted(glob.glob(os.path.join(ROOT, "models", "*.gguf")))
    return found[0] if found else None


def part_model(exe, work, _lex):
    print("real engine: local-model advisor run")
    model = find_model()
    if real_data_missing() or not model:
        print("  SKIP (%s)" % ("no model file" if not model else "real data missing"))
        return
    eng = Engine(exe, os.path.join(work, "data"), WORK, os.path.join(work, "model.log"), stub=False)
    hello = eng.req("engine.hello", timeout=60)
    if hello["model"].get("reason") == "not_built" or not hello["model"]["cpuOk"]:
        print("  SKIP (local model not in this build or CPU without AVX2)")
        eng.shutdown()
        return
    loc = eng.req("model.locate", {"path": model}, timeout=120)
    check(loc["available"], "model.locate")
    check("M" in eng.req("engine.hello")["modes"], "hello: mode M once the model is available")
    eng.req("project.new", {"kind": "subs", "pair": "en-la", "sourcePath": os.path.join(SAMPLES, "sample.en.srt")})
    t0 = time.time()
    # three cues: every closed question scores each option with the model (about 50 s per cue on 4 busy cores)
    tr = eng.req("translate.start", {"indices": [0, 1, 2], "engines": {"rules": True, "model": True}})
    check(tr["warnings"] == [], "model run: no warning")
    eng.wait_event("translate.done", lambda e: e["jobId"] == tr["jobId"], timeout=900)
    check(not eng.events_of("translate.warning", tr["jobId"]), "model run: no translate.warning (%.1f s)" % (time.time() - t0))
    states = []
    for i in range(3):
        d = eng.req("cue.get", {"index": i})
        states += [r["data"]["state"] for r in d["reasons"] if r["kind"] == "evidence" and isinstance(r["data"], dict)
                   and r["data"].get("source") == "model" and r["tokenIndex"] >= 0]
    check(states and "off" not in states and "yes" in states, "model evidence rows: %d yes of %d" % (states.count("yes"), len(states)))
    check(eng.req("model.status")["loaded"] is False, "model unloaded after the job")
    check(eng.shutdown() == 0 and log_clean(eng.log_path), "model engine: clean shutdown")


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    exe = os.path.abspath(sys.argv[1])
    work = os.path.abspath(sys.argv[2])
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    lex = os.path.join(work, "lexicons")
    os.makedirs(lex)
    shutil.copy(LATIN_VPL, os.path.join(lex, "latin.vpl"))
    stub_only = "--stub-only" in sys.argv[3:] or os.environ.get("VP_TEST_STUB_ONLY") == "1"
    parts = [part_session, part_framing, part_long_job, part_crash]
    if not stub_only:
        parts += [part_real, part_online_mock, part_model]
    t0 = time.time()
    for part in parts:
        t = time.time()
        try:
            sub = os.path.join(work, part.__name__)
            os.makedirs(sub)
            part(exe, sub, lex)
        except Exception as e:  # noqa: BLE001
            import traceback
            traceback.print_exc()
            check(False, "%s raised %s" % (part.__name__, e))
        print("  (%s: %.1f s)" % (part.__name__, time.time() - t))
    part_offline(exe)
    print("total %.1f s, %d failure(s)" % (time.time() - t0, len(FAILURES)))
    for f in FAILURES:
        print("FAILED: " + f)
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
