"""Shared helpers for the library stages: streaming readers, TSV writing, hashing, logging, RSS."""
import gzip
import hashlib
import json
import os
import resource
import sys
import time

LANGS = ("la", "grc", "en", "es")
RAW_FILES = {"la": "kaikki-Latin.jsonl", "grc": "kaikki-AncientGreek.jsonl", "en": "kaikki-English.jsonl",
             "es": "es-extract.jsonl.gz"}
# the es stage also reads Latin/Greek entries of the es.wiktionary extract; the en stage only the English file


QUIET = os.environ.get("VP_QUIET") == "1"


def log(msg):
    if QUIET:
        return
    sys.stderr.write("%s %s\n" % (time.strftime("%H:%M:%S"), msg))
    sys.stderr.flush()


def peak_rss_mb():
    """Peak resident set size of this process in MB (Linux reports KB)."""
    return round(resource.getrusage(resource.RUSAGE_SELF).ru_maxrss / 1024.0, 1)


def open_text(path):
    if path.endswith(".gz"):
        return gzip.open(path, "rt", encoding="utf-8")
    return open(path, "r", encoding="utf-8")


def iter_jsonl(path, label="", every=100000, counters=None):
    """Stream one JSON object per line; malformed lines are counted and skipped. Logs every `every` lines."""
    t0 = time.time()
    n = bad = 0
    with open_text(path) as f:
        for line in f:
            n += 1
            if n % every == 0:
                extra = ""
                if counters is not None:
                    extra = " " + " ".join("%s=%d" % kv for kv in sorted(counters.items()) if isinstance(kv[1], int))
                log("[%s] %d lines, %.1f s%s" % (label, n, time.time() - t0, extra))
            line = line.strip()
            if not line:
                continue
            try:
                yield json.loads(line)
            except ValueError:
                bad += 1
                if counters is not None:
                    counters["bad_lines"] = counters.get("bad_lines", 0) + 1
    if counters is not None:
        counters["lines"] = n


def clean(s):
    """A value safe for one TSV field: no tabs, newlines or carriage returns."""
    if s is None:
        return ""
    s = str(s)
    if "\t" in s or "\n" in s or "\r" in s:
        s = s.replace("\t", " ").replace("\r", " ").replace("\n", " ")
    return s


class TsvWriter(object):
    def __init__(self, path):
        self.path = path
        self.f = open(path, "w", encoding="utf-8", newline="\n")
        self.rows = 0

    def write(self, *fields):
        self.f.write("\t".join(clean(x) for x in fields))
        self.f.write("\n")
        self.rows += 1

    def close(self):
        self.f.close()


def read_tsv(path):
    with open(path, "r", encoding="utf-8", newline="\n") as f:
        for line in f:
            yield line.rstrip("\n").split("\t")


def sha256_file(path, bufsize=1 << 22):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while True:
            b = f.read(bufsize)
            if not b:
                break
            h.update(b)
    return h.hexdigest()


def file_info(path, cache=None):
    """{size, mtime, sha256}; reuses a cached sha256 when size and mtime are unchanged."""
    st = os.stat(path)
    info = {"size": st.st_size, "mtime": int(st.st_mtime)}
    if cache and cache.get("size") == info["size"] and cache.get("mtime") == info["mtime"] and cache.get("sha256"):
        info["sha256"] = cache["sha256"]
    else:
        info["sha256"] = sha256_file(path)
    return info


def write_json(path, obj):
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8", newline="\n") as f:
        json.dump(obj, f, ensure_ascii=False, indent=1, sort_keys=True)
        f.write("\n")
    os.replace(tmp, path)


def read_json(path, default=None):
    try:
        with open(path, "r", encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return default
