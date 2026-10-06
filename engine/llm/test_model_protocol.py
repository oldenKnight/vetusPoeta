#!/usr/bin/env python3
"""Protocol test of the model.* commands (DESIGN 9) against a real `vpengine serve`. Python 3 stdlib only.

usage: test_model_protocol.py <path/to/vpengine> <work dir>
Part 1 (always): no model file -> hello/model.status unavailable, model.test model_missing, model.locate errors.
Part 2 (when a model exists: $VP_MODEL_GGUF, else <repo>/models/*.gguf; skipped with VP_LLM_SKIP_MODEL=1 or when
the binary was built without the local model): locate, status with SHA-256, test (load, choose, unload), a
translate job with engines.model=true (no warning, model unloaded after the job).
"""
import glob
import json
import os
import queue
import shutil
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
FIXTURE = os.path.join(ROOT, "tests", "fixtures", "cli", "thirty_cues.srt")
LEXICONS = os.path.join(ROOT, "tests", "fixtures", "lex")
FAILURES = []


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what, flush=True)
    if not cond:
        FAILURES.append(what)
    return cond


class Engine:
    def __init__(self, exe, data, log_path):
        env = dict(os.environ)
        env.pop("VP_MODEL_GGUF", None)
        env["VP_LOG"] = "info"
        self.log = open(log_path, "ab")
        self.p = subprocess.Popen([exe, "serve", "--data", data, "--lexicons", LEXICONS], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=self.log, env=env, bufsize=0)
        self.q = queue.Queue()
        self.events = queue.Queue()
        self.next_id = 1
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        for raw in self.p.stdout:
            msg = json.loads(raw.decode("utf-8"))
            (self.events if "event" in msg else self.q).put(msg)
        self.q.put(None)

    def call(self, cmd, params=None, timeout=120):
        rid = self.next_id
        self.next_id += 1
        self.p.stdin.write((json.dumps({"id": rid, "cmd": cmd, "params": params or {}}) + "\n").encode("utf-8"))
        self.p.stdin.flush()
        deadline = time.time() + timeout
        while True:
            msg = self.q.get(timeout=max(0.1, deadline - time.time()))
            if msg is None:
                raise RuntimeError("engine exited during " + cmd)
            if msg.get("id") == rid:
                return msg

    def wait_event(self, name, timeout=120):
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                ev = self.events.get(timeout=0.5)
            except queue.Empty:
                continue
            if ev.get("event") == name:
                return ev
        return None

    def close(self):
        try:
            self.call("engine.shutdown", timeout=30)
        except Exception:
            pass
        try:
            self.p.stdin.close()
        except Exception:
            pass
        try:
            self.p.wait(timeout=30)
        except subprocess.TimeoutExpired:
            self.p.kill()
        self.log.close()


def find_model():
    if os.environ.get("VP_LLM_SKIP_MODEL") == "1":
        return ""
    env = os.environ.get("VP_MODEL_GGUF", "")
    if env:
        return env
    found = sorted(glob.glob(os.path.join(ROOT, "models", "*.gguf")))
    return found[0] if found else ""


def main():
    exe, work = sys.argv[1], sys.argv[2]
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    log = os.path.join(work, "engine.log")
    data = os.path.join(work, "data")
    eng = Engine(exe, data, log)
    try:
        print("part 1: no model file")
        hello = eng.call("engine.hello")["result"]
        built = hello["model"].get("reason") != "not_built"
        check(hello["model"]["available"] is False, "hello: model unavailable")
        st = eng.call("model.status")
        check(st["ok"] and st["result"]["available"] is False and st["result"]["loaded"] is False,
              "model.status: unavailable, not loaded")
        check(set(["available", "path", "sizeBytes", "sha256ok", "loaded", "cpuOk", "lastLoadMs"]) <= set(st["result"]),
              "model.status: every DESIGN 9 field")
        t = eng.call("model.test")
        check(not t["ok"] and t["error"]["code"] == "model_missing" and t["error"]["hint"], "model.test -> model_missing + hint")
        loc = eng.call("model.locate", {"path": os.path.join(work, "nothing.gguf")})
        check(not loc["ok"] and loc["error"]["code"] == "not_found", "model.locate missing file -> not_found")
        junk = os.path.join(work, "junk.gguf")
        with open(junk, "wb") as f:
            f.write(b"x" * 4096)
        loc = eng.call("model.locate", {"path": junk})
        want = "model_load_failed" if built else "model_missing"
        check(not loc["ok"] and loc["error"]["code"] == want, "model.locate junk -> " + want)
        check(eng.call("model.unload")["ok"], "model.unload ok without a model")
        model = find_model()
        if not built or not model or not hello["model"]["cpuOk"]:
            print("part 2 skipped (built %s, model %r, cpuOk %s)" % (built, model, hello["model"]["cpuOk"]))
            return
        print("part 2: model " + model)
        t0 = time.time()
        loc = eng.call("model.locate", {"path": model})
        check(loc["ok"] and loc["result"]["sha256ok"] is True and loc["result"]["available"] is True,
              "model.locate: sha256ok, available (%.1f s)" % (time.time() - t0))
        check(eng.call("settings.get")["result"]["modelPath"] == model, "settings.modelPath recorded")
        st = eng.call("model.status")["result"]
        check(st["available"] and not st["loaded"] and st["sizeBytes"] > 0, "model.status: available, not loaded")
        t = eng.call("model.test", timeout=300)
        ok = check(t["ok"], "model.test ok")
        if ok:
            r = t["result"]
            check(isinstance(r["test"]["chosen"], int) and r["loaded"] is False and r["lastLoadMs"] > 0,
                  "model.test: chosen %s (%s), %d ms, unloaded after" % (r["test"]["chosen"], r["test"]["chosenText"],
                                                                     r["test"]["ms"]))
        pn = eng.call("project.new", {"kind": "subs", "pair": "en-la", "sourcePath": FIXTURE})
        check(pn["ok"], "project.new")
        ts = eng.call("translate.start", {"engines": {"rules": True, "model": True, "online": False}, "fidelity": 2})
        check(ts["ok"] and "model_missing" not in ts["result"]["warnings"], "translate.start engines.model: no warning")
        check(eng.wait_event("translate.done") is not None, "translate.done")
        st = eng.call("model.status")["result"]
        check(st["loaded"] is False, "model not loaded after the job")
        eng.call("project.close", {"discard": True})
    finally:
        eng.close()
        if FAILURES:
            with open(log, "rb") as f:
                sys.stdout.write(f.read().decode("utf-8", "replace")[-4000:])
    if FAILURES:
        print("FAILED: %d" % len(FAILURES))
        sys.exit(1)
    print("all model protocol checks passed")


if __name__ == "__main__":
    main()
