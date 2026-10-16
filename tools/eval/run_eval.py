#!/usr/bin/env python3
"""Error-measurement harness (DESIGN section 15, PREPLAN section 6). Python 3 stdlib only.

Drives `vpengine serve` over one subtitle file for every engine combination x fidelity cell, collects per-cue
records (checks A1-A9, confidence, flags, gold match), and writes report.json + report.md (numbers with numerator,
denominator and Clopper-Pearson 95 % interval) and one cues_<cell>.jsonl per cell into --out. See docs/EVAL.md.

  run_eval.py --file tests/regression/own_dialogue.en.srt \
              --gold tests/regression/expected/own_dialogue.la.gold.txt --out data/work/eval/regression/
  run_eval.py --file F --dump-source data/work/gt/NAME.src.txt          (input for tools/eval/gt_compare.js)
  run_eval.py --file F --gold G --compare-system data/work/gt/NAME.gt.la.txt --system-name "Google Translate"
  run_eval.py --heldout --file tests/heldout/own_heldout.en.srt          (guarded, no per-cue text in report.md)
"""
import argparse
import datetime
import html
import json
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import evallib as L  # noqa: E402

ROOT = L.ROOT
DEFAULT_ENGINE = os.path.join(ROOT, "build-eval", "engine", "cli", "vpengine")
SAFE_OUT = os.path.join(ROOT, "data", "work")
PREPLAN_RULE = ("PREPLAN 6.1: '< 1 %' may be claimed only if the upper bound of the 95 % interval on the "
                "expert-reviewed sample is below 1 %, or if every cue was reviewed and fewer than 1 % are wrong.")


def rel(path):
    try:
        r = os.path.relpath(os.path.abspath(path), ROOT)
        return r if not r.startswith("..") else os.path.abspath(path)
    except ValueError:
        return os.path.abspath(path)


def die(msg, code=2):
    sys.stderr.write("run_eval: " + msg + "\n")
    sys.exit(code)


def check_out_dir(path):
    """Outputs carry cue texts (possibly the owner's subtitles or third-party output): inside the repository they
    may only go under data/work/ (gitignored)."""
    if L.is_under(path, ROOT) and not L.is_under(path, SAFE_OUT):
        die("refusing to write %s inside the repository: use a path under data/work/ (gitignored) or outside the "
            "repository" % rel(path))


def parse_combos(text):
    out = []
    for tok in text.split(","):
        tok = tok.strip().upper()
        if not tok:
            continue
        parts = set(tok.split("+"))
        if not parts or not parts <= {"R", "M", "O"}:
            die("bad combination %r (use R, M, O joined by +)" % tok)
        out.append("+".join(x for x in ("R", "M", "O") if x in parts))
    return out


def default_lexicons():
    work = os.path.join(ROOT, "data", "work")
    if os.path.isfile(os.path.join(work, "latin.vpl")):
        return work
    return os.path.join(ROOT, "tests", "fixtures", "lex")


def cross_lang(pair):
    a, _, b = (pair or "").partition("-")
    return a != b


def parse_settings(items):
    patch = {}
    for it in items or []:
        if "=" not in it:
            die("--setting needs key=value, got %r" % it)
        k, v = it.split("=", 1)
        try:
            patch[k] = json.loads(v)
        except ValueError:
            patch[k] = v
    return patch


# ------------------------------------------------------------------------------------------------------ one cell
def copied_source(source, target):
    """PREPLAN 6.1 vocabulary fault 'untranslated English left in': the target equals the source after
    normalisation and has at least two words (a lone name such as 'Alice!' may legitimately stay)."""
    a, b = L.normalise(source), L.normalise(target)
    return bool(a) and a == b and len(a.split()) >= 2


def make_records(page, details, gold, burned, keep_text, cross_lang=True):
    recs = []
    for i, base in enumerate(page):
        d = details[i]
        cv = d.get("cue", base)
        checks = {}
        for c in d.get("checks", []):
            checks[c["id"]] = {"ok": bool(c.get("ok")), "detail": c.get("detail", "")}
        unknown = [t.get("text", "") for t in d.get("tokens", []) if t.get("unknown")]
        failed = sorted(k for k, v in checks.items() if not v["ok"])
        target = cv.get("target", "") or ""
        untranslated = not target.strip() or cv.get("state") == "new"
        copied = cross_lang and not untranslated and copied_source(base.get("source", ""), target)
        auto_failed = [k for k in failed if k in L.AUTO_ERROR_CHECKS]
        auto_error = bool(auto_failed or unknown or untranslated or copied)
        flagged = bool(failed or unknown or untranslated or copied) or cv.get("confidence") != "ok"
        g = gold[i] if gold is not None and i < len(gold) else None
        is_burned = str(cv.get("idRaw", base.get("idRaw"))) in burned
        r = {"index": i, "idRaw": cv.get("idRaw"), "counted": L.counted(base.get("source", "")) and not is_burned,
             "soundOnly": not L.counted(base.get("source", "")), "burned": is_burned,
             "confidence": cv.get("confidence"), "score": cv.get("score"), "state": cv.get("state"),
             "cps": cv.get("cps"), "flags": cv.get("flags", []), "overflow": "overflow" in cv.get("flags", []),
             "checks": checks, "failed": failed, "unknownTokens": len(unknown), "untranslated": untranslated,
             "copiedSource": copied,
             "autoError": auto_error, "autoFailed": auto_failed, "flagged": flagged,
             "goldGiven": g is not None, "goldMatch": L.gold_match(target, g) if g is not None else None,
             "reasons": len(d.get("reasons", []))}
        if keep_text:
            r["source"] = L.one_line(base.get("source", ""))
            r["target"] = target
            r["gold"] = g
            r["unknown"] = unknown
        recs.append(r)
    return recs


def summarise(recs):
    cnt = [r for r in recs if r["counted"]]
    n = len(cnt)
    flagged = [r for r in cnt if r["flagged"]]
    unflagged = [r for r in cnt if not r["flagged"]]
    conf = {"ok": 0, "check": 0, "fix": 0, "other": 0}
    for r in cnt:
        conf[r["confidence"] if r["confidence"] in conf else "other"] += 1
    checks = {}
    for cid in L.ALL_CHECKS:
        present = [r for r in cnt if cid in r["checks"]]
        fails = sum(1 for r in present if not r["checks"][cid]["ok"])
        checks[cid] = {"present": len(present), "fail": fails, "failRate": L.proportion(fails, len(present))}
    other_ids = sorted({k for r in cnt for k in r["checks"]} - set(L.ALL_CHECKS))
    for cid in other_ids:
        present = [r for r in cnt if cid in r["checks"]]
        fails = sum(1 for r in present if not r["checks"][cid]["ok"])
        checks[cid] = {"present": len(present), "fail": fails, "failRate": L.proportion(fails, len(present))}
    gold = [r for r in cnt if r["goldGiven"]]
    s = {
        "cues": len(recs), "counted": n, "soundOnly": sum(1 for r in recs if r["soundOnly"]),
        "burnedExcluded": sum(1 for r in recs if r["burned"]),
        "autoError": L.proportion(sum(1 for r in cnt if r["autoError"]), n),
        "autoErrorBy": {"A1": sum(1 for r in cnt if "A1" in r["autoFailed"]),
                        "A3": sum(1 for r in cnt if "A3" in r["autoFailed"]),
                        "A4": sum(1 for r in cnt if "A4" in r["autoFailed"]),
                        "A5": sum(1 for r in cnt if "A5" in r["autoFailed"]),
                        "unknownToken": sum(1 for r in cnt if r["unknownTokens"]),
                        "untranslated": sum(1 for r in cnt if r["untranslated"]),
                        "copiedSource": sum(1 for r in cnt if r["copiedSource"])},
        "flagged": L.proportion(len(flagged), n),
        "autoErrorFlagged": L.proportion(sum(1 for r in flagged if r["autoError"]), len(flagged)),
        "autoErrorUnflagged": L.proportion(sum(1 for r in unflagged if r["autoError"]), len(unflagged)),
        "confidence": {k: L.proportion(v, n) for k, v in conf.items()},
        "checks": checks,
        "checksAbsent": [c for c in L.ALL_CHECKS if checks[c]["present"] == 0],
        "gold": L.proportion(sum(1 for r in gold if r["goldMatch"]), len(gold)) if gold else None,
        "cps": {"over": sum(1 for r in cnt if "cps" in r["flags"]), "overflow": sum(1 for r in cnt if r["overflow"])},
        "review": {"reviewed": 0, "coverage": 0.0, "note": "no expert review in this run"},
    }
    return s


def run_cell(args, combo, fid, run_no, work, burned, gold, patch):
    name = "%s/T%d" % (combo, fid)
    engines = {"rules": "R" in combo, "model": "M" in combo, "online": "O" in combo}
    data = os.path.join(work, "%s-T%d-run%d" % (combo, fid, run_no))
    os.makedirs(data, exist_ok=True)
    log = os.path.join(args.out, "logs", "%s-T%d-run%d.log" % (combo, fid, run_no))
    cell = {"name": name, "combo": combo, "fidelity": fid, "engines": engines, "available": True}
    t_cell = time.time()
    with L.Engine(args.engine, data, args.lexicons, log, timeout=args.timeout) as eng:
        hello = eng.request("engine.hello")
        cell["hello"] = hello
        settings = dict(patch)
        settings.update({"defaultFidelity": fid, "engines.model": engines["model"],
                         "engines.online": engines["online"]})
        if engines["online"]:
            settings["online.wiktionary"] = True
        if engines["online"] and not args.allow_online:
            cell.update(available=False, reason="online engine not allowed in this run (pass --allow-online)")
            return cell, None
        if engines["model"] and not (hello.get("model") or {}).get("available"):
            cell.update(available=False, reason="no local model (engine.hello model.available = false, reason %s)"
                        % (hello.get("model") or {}).get("reason", "?"))
            return cell, None
        if not engines["rules"] and combo not in (hello.get("modes") or []):
            cell.update(available=False, reason="the engine exposes no %s-only mode (translate.start ignores "
                                                "engines.rules = false; engine.hello has no 'modes' entry)" % combo)
            return cell, None
        eng.request("settings.set", {"patch": settings})
        if engines["online"]:
            msg = eng.call("online.test", timeout=60)
            if not msg.get("ok"):
                err = msg.get("error") or {}
                cell.update(available=False, reason="online.test: %s %s" % (err.get("code"), err.get("message", "")))
                return cell, None
        _proj, page = L.load_project(eng, args.file, args.pair)
        if gold is not None and len(gold) > len(page):
            die("the gold file has %d entries but the subtitle file has %d cues" % (len(gold), len(page)))
        job = L.translate(eng, engines, fid, timeout=args.timeout * 30)
        if job["warnings"]:
            cell.update(available=False, reason="translate.start warnings: %s (the cell would silently run "
                                                "without the requested engine)" % ", ".join(job["warnings"]))
            return cell, None
        if job["errors"]:
            e = job["errors"][0]
            cell.update(available=False, reason="translate.error %s: %s" % (e.get("code"), e.get("message")))
            return cell, None
        t1 = time.time()
        details = L.cue_details(eng, len(page))
        get_ms = (time.time() - t1) * 1000.0
        ev = eng.request("eval.run", {"outPath": os.path.join(data, "engine_report.json")})
        with open(ev["reportPath"], encoding="utf-8") as f:
            engine_report = json.load(f)
        structure = L.export_and_verify(eng, args.file, data)
        rss = eng.peak_rss_kb()
    recs = make_records(page, details, gold, burned, keep_text=not args.heldout or args.heldout_texts,
                        cross_lang=cross_lang(args.pair))
    targets = [d.get("cue", {}).get("target", "") for d in details]
    cell.update({
        "engineVersion": hello.get("engine"), "appVersion": hello.get("version"),
        "stats": job["stats"], "translateWallMs": job["wallMs"], "cueGetMs": round(get_ms, 1),
        "cellWallMs": round((time.time() - t_cell) * 1000.0, 1), "peakRssKb": rss, "structure": structure,
        "targetsHash": L.targets_hash(targets),
        "engineEvalRun": {"cues": engine_report.get("cues"), "confidence": engine_report.get("confidence"),
                          "checks": engine_report.get("checks")},
    })
    return cell, recs


# ------------------------------------------------------------------------------------------------------ system scoring
def score_system(args, lines, fid, gold, burned, patch, work):
    """Scores another system's lines with our checker: each line goes through cue.set on a scratch project (the
    server runs Engine::check on it), then cue.get returns the checks and tokens."""
    data = os.path.join(work, "system")
    os.makedirs(data, exist_ok=True)
    log = os.path.join(args.out, "logs", "system.log")
    with L.Engine(args.engine, data, args.lexicons, log, timeout=args.timeout) as eng:
        settings = dict(patch)
        settings["defaultFidelity"] = fid
        eng.request("settings.set", {"patch": settings})
        _proj, page = L.load_project(eng, args.file, args.pair)
        if len(lines) != len(page):
            die("%s has %d lines but the subtitle file has %d cues (one line per cue expected; produce the input with "
                "--dump-source)" % (rel(args.compare_system), len(lines), len(page)))
        for i, text in enumerate(lines):
            if L.counted(page[i].get("source", "")) and text.strip():
                eng.request("cue.set", {"index": i, "text": text})
        details = L.cue_details(eng, len(page))
    recs = make_records(page, details, gold, burned, keep_text=True, cross_lang=cross_lang(args.pair))
    for r, text in zip(recs, lines):
        r["target"] = text
        r["untranslated"] = not text.strip()
        r["sysFailed"] = [k for k in r["failed"] if k in L.SYSTEM_ERROR_CHECKS]
        r["copiedSource"] = bool(text.strip()) and copied_source(page[r["index"]].get("source", ""), text)
        r["sysError"] = bool(r["sysFailed"] or r["unknownTokens"] or r["untranslated"] or r["copiedSource"])
    return recs


def sys_error(rec):
    failed = [k for k in rec["failed"] if k in L.SYSTEM_ERROR_CHECKS]
    return bool(failed or rec["unknownTokens"] or rec["untranslated"] or rec.get("copiedSource"))


def compare_systems(ours, theirs, our_name, sys_name):
    pairs = [(a, b) for a, b in zip(ours, theirs) if a["counted"]]
    n = len(pairs)
    a_err = sum(1 for a, _ in pairs if sys_error(a))
    b_err = sum(1 for _, b in pairs if b["sysError"])
    auto = L.mcnemar_exact(sum(1 for a, b in pairs if sys_error(a) and not b["sysError"]),
                           sum(1 for a, b in pairs if not sys_error(a) and b["sysError"]))
    gp = [(a, b) for a, b in pairs if a["goldGiven"]]
    gold = None
    if gp:
        gold = {"ours": L.proportion(sum(1 for a, _ in gp if a["goldMatch"]), len(gp)),
                "system": L.proportion(sum(1 for _, b in gp if b["goldMatch"]), len(gp)),
                "mcnemar": L.mcnemar_exact(sum(1 for a, b in gp if not a["goldMatch"] and b["goldMatch"]),
                                           sum(1 for a, b in gp if a["goldMatch"] and not b["goldMatch"]))}
    a6 = sum(1 for _, b in pairs if "A6" in b["checks"] and not b["checks"]["A6"]["ok"])
    return {"ours": our_name, "system": sys_name, "counted": n,
            "definition": "automatic error = A1, A3 or A4 fails, an unknown token, no output, or the source copied "
                          "unchanged (PREPLAN 6.6 step 4); "
                          "A5 and A6 are reported, not counted",
            "autoErrorOurs": L.proportion(a_err, n), "autoErrorSystem": L.proportion(b_err, n),
            "autoMcnemar": auto, "gold": gold, "systemA6Fail": a6,
            "systemChecksAbsent": [c for c in ("A1", "A3", "A4", "A6") if not any(c in b["checks"] for _, b in pairs)]}


def write_compare_html(path, cmp_, ours, theirs, sys_name, cell_name, file_name):
    with open(os.path.join(HERE, "templates", "compare.html"), encoding="utf-8") as f:
        tpl = f.read()
    rows = []
    for a, b in zip(ours, theirs):
        if not a["counted"]:
            continue
        def cell(r, err):
            marks = []
            if r["goldGiven"]:
                marks.append('<span class="%s">%s</span>' % ("good" if r["goldMatch"] else "bad",
                                                              "gold" if r["goldMatch"] else "not gold"))
            fl = r.get("sysFailed") if "sysFailed" in r else [k for k in r["failed"] if k in L.SYSTEM_ERROR_CHECKS]
            if err:
                why = (fl + (["unknown word"] if r["unknownTokens"] else []) + (["empty"] if r["untranslated"] else [])
                       + (["source copied"] if r.get("copiedSource") else []))
                marks.append('<span class="bad">%s</span>' % html.escape(", ".join(why) or "error"))
            return "<td>%s<div class=\"m\">%s</div></td>" % (html.escape(r.get("target") or ""), " ".join(marks))
        rows.append("<tr><td class=\"n\">%s</td><td>%s</td><td class=\"g\">%s</td>%s%s</tr>" % (
            html.escape(str(a["idRaw"])), html.escape(a.get("source", "")),
            html.escape(" | ".join(a.get("gold") or [])), cell(a, sys_error(a)), cell(b, b["sysError"])))
    def p(x):
        return html.escape(L.fmt_prop(x))
    summary = ["<li>Cues compared: %d (cell %s)</li>" % (cmp_["counted"], html.escape(cell_name)),
               "<li>Automatic errors, vetus poeta: %s</li>" % p(cmp_["autoErrorOurs"]),
               "<li>Automatic errors, %s: %s</li>" % (html.escape(sys_name), p(cmp_["autoErrorSystem"])),
               "<li>McNemar exact (discordant b=%d, c=%d): p = %.4g</li>" % (
                   cmp_["autoMcnemar"]["b"], cmp_["autoMcnemar"]["c"], cmp_["autoMcnemar"]["p"])]
    if cmp_["gold"]:
        g = cmp_["gold"]
        summary += ["<li>Gold match, vetus poeta: %s</li>" % p(g["ours"]),
                    "<li>Gold match, %s: %s</li>" % (html.escape(sys_name), p(g["system"])),
                    "<li>McNemar exact on gold (b=%d, c=%d): p = %.4g</li>" % (g["mcnemar"]["b"], g["mcnemar"]["c"],
                                                                            g["mcnemar"]["p"])]
    out = (tpl.replace("{{TITLE}}", html.escape("%s: vetus poeta vs %s" % (file_name, sys_name)))
              .replace("{{SYSTEM}}", html.escape(sys_name))
              .replace("{{DEFINITION}}", html.escape(cmp_["definition"]))
              .replace("{{SUMMARY}}", "\n".join(summary))
              .replace("{{ROWS}}", "\n".join(rows)))
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(out)


# ------------------------------------------------------------------------------------------------------ report
def md_escape(s):
    return (s or "").replace("|", "\\|").replace("\n", " ")


def claim_for(cell):
    p = cell["summary"]["autoError"]
    return {"lessThanOnePercent": False,
            "sentence": "Automatic checks marked %s of the counted cues as errors in cell %s. This is not an "
                        "expert error rate; no '< 1 %%' claim is made (no expert review in this run)."
                        % (L.fmt_prop(p), cell["name"]),
            "rule": PREPLAN_RULE}


def provenance(args, cells, hello):
    stub = str(hello.get("engine", "")).startswith("stub")
    if args.heldout:
        tuned = False
    elif args.tuned or L.is_under(args.file, os.path.join(ROOT, "tests", "regression")):
        tuned = True
    else:
        tuned = None
    lines = []
    if stub:
        lines.append("STUB ENGINE: these numbers come from the stub rules engine (engine.hello.engine = '%s'), which "
                     "copies the English source into the target. They test the harness, not the translator."
                     % hello.get("engine"))
    if tuned is True:
        lines.append("TUNED FILE: this file is used for rule tuning (tests/regression or the acceptance file), so its "
                     "numbers are optimistic; print them next to the held-out numbers.")
    elif tuned is False:
        lines.append("HELD-OUT FILE: not used for rule tuning.")
    else:
        lines.append("Tuning status of this file not declared (pass --tuned for the acceptance file).")
    return {"stubEngine": stub, "tunedFile": tuned, "heldout": bool(args.heldout), "statements": lines}


def write_report_md(path, rep, cells, primary, primary_recs, args, system_cmp=None):
    o = []
    f = rep["file"]
    o.append("# Evaluation report: %s (%s)" % (f["name"], f["pair"]))
    o.append("")
    o.append("Generated %s by tools/eval/run_eval.py. Engine `%s` (app %s), lexicons from `%s`." % (
        rep["createdAt"], rep["engine"]["engine"], rep["engine"]["version"], rep["engine"]["lexicons"]))
    o.append("")
    for s in rep["provenance"]["statements"]:
        o.append("> **%s**" % s)
        o.append("")
    if rep.get("heldoutGuard"):
        g = rep["heldoutGuard"]
        o.append("Held-out guard: %s (%d files checked against FROZEN.sha256); burned cues excluded: %d." % (
            "OK" if g["ok"] else "FAILED", g["checked"], f["burnedExcluded"]))
        o.append("")
    o.append("File: %d cues, %d counted (text with at least one letter), %d sound-only reported separately, "
             "sha256 `%s`." % (f["cues"], f["counted"], f["soundOnly"], f["sha256"][:16]))
    o.append("")
    o.append("## Publication table (PREPLAN 6.7)")
    o.append("")
    o.append("Wrong = automatic error: A1, A3, A4 or A5 fails, an unknown token, no output, or the source copied "
             "unchanged (DESIGN 10.4, PREPLAN 6.1). "
             "Expert review coverage is 0 % in this run, so no cell has an expert error rate yet.")
    o.append("")
    o.append("| Cell | Cues | Wrong | Rate | 95 % CI | Flagged / unflagged | Gold match | Review coverage | Burned | "
             "Determinism | Run time | Peak RSS |")
    o.append("|---|---|---|---|---|---|---|---|---|---|---|---|")
    for c in cells:
        if not c["available"]:
            o.append("| %s | not available: %s | | | | | | | | | | |" % (c["name"], md_escape(c["reason"])))
            continue
        s = c["summary"]
        p = s["autoError"]
        lo, hi = p["ci95"] if p["den"] else (None, None)
        gold = L.fmt_prop(s["gold"]) if s["gold"] else "no gold"
        det = c["determinism"]
        o.append("| %s | %d | %d | %s | %s-%s | %d / %d | %s | %s | %d | %s `%s` | %.1f s | %s |" % (
            c["name"], p["den"], p["num"], L.pct(p["rate"]), L.pct(lo)[:-2], L.pct(hi),
            s["flagged"]["num"], s["flagged"]["den"] - s["flagged"]["num"], gold, L.pct(s["review"]["coverage"], 0),
            s["burnedExcluded"], "identical" if det["identical"] else ("DIFFERENT" if det["runs"] > 1 else "1 run"),
            det["hashes"][0][:12], c["cellWallMs"] / 1000.0,
            ("%.1f MB" % (c["peakRssKb"] / 1024.0)) if c.get("peakRssKb") else "n/a"))
    o.append("")
    if primary:
        o.append("## Claim (PREPLAN 6.1)")
        o.append("")
        o.append(rep["claim"]["sentence"])
        o.append("")
        o.append("Rule: " + PREPLAN_RULE)
        o.append("")
    measured = [c for c in cells if c["available"]]
    if measured:
        o.append("## Confidence distribution and flags")
        o.append("")
        o.append("| Cell | OK | Check | Fix | Wrong among flagged | Wrong among unflagged | Over CPS | Overflow |")
        o.append("|---|---|---|---|---|---|---|---|")
        for c in measured:
            s = c["summary"]
            cf = s["confidence"]
            o.append("| %s | %d | %d | %d | %s | %s | %d | %d |" % (
                c["name"], cf["ok"]["num"], cf["check"]["num"], cf["fix"]["num"], L.fmt_prop(s["autoErrorFlagged"]),
                L.fmt_prop(s["autoErrorUnflagged"]), s["cps"]["over"], s["cps"]["overflow"]))
        o.append("")
        o.append("## Checks reported by the engine (cell %s)" % primary["name"])
        o.append("")
        s = primary["summary"]
        o.append("| Check | Present on | Failed | Fail rate |")
        o.append("|---|---|---|---|")
        for cid, v in s["checks"].items():
            o.append("| %s | %d | %d | %s |" % (cid, v["present"], v["fail"],
                                               L.fmt_prop(v["failRate"]) if v["present"] else "absent"))
        o.append("")
        if s["checksAbsent"]:
            o.append("Absent checks (%s) were never reported by this engine: a missing check is not a pass, and the "
                     "automatic error rate covers only the checks present." % ", ".join(s["checksAbsent"]))
            o.append("")
        o.append("## Cue identity (export.write, byte level)")
        o.append("")
        for c in measured:
            st = c["structure"]
            o.append("- %s: %s (%d -> %d cues, numbering/timing differences %d, tag differences %d, blank lines %s, "
                     "export warnings %d %s)" % (c["name"], "identical" if st["ok"] else "CHANGED", st["cuesSource"],
                                                 st["cuesOutput"], len(st["headerDiffs"]), len(st["tagDiffs"]),
                                                 "kept" if st["blankLinesEqual"] else "CHANGED", st["exportWarnings"],
                                                 st["exportWarningKinds"]))
        o.append("")
    if system_cmp:
        sc = system_cmp
        o.append("## Comparison with %s (cell %s, same cues)" % (sc["system"], sc["ours"]))
        o.append("")
        o.append("Definition: " + sc["definition"] + ".")
        o.append("")
        o.append("| System | Automatic errors | Gold match |")
        o.append("|---|---|---|")
        o.append("| vetus poeta %s | %s | %s |" % (sc["ours"], L.fmt_prop(sc["autoErrorOurs"]),
                                                  L.fmt_prop(sc["gold"]["ours"]) if sc["gold"] else "no gold"))
        o.append("| %s | %s | %s |" % (sc["system"], L.fmt_prop(sc["autoErrorSystem"]),
                                       L.fmt_prop(sc["gold"]["system"]) if sc["gold"] else "no gold"))
        o.append("")
        m = sc["autoMcnemar"]
        o.append("McNemar exact test, automatic errors: discordant pairs b = %d (ours wrong, theirs right), c = %d, "
                 "p = %.4g." % (m["b"], m["c"], m["p"]))
        if sc["gold"]:
            g = sc["gold"]["mcnemar"]
            o.append("McNemar exact test, gold mismatch: b = %d, c = %d, p = %.4g." % (g["b"], g["c"], g["p"]))
        o.append("A6 (tier) failures of %s: %d (reported, not counted). Checks absent from the scoring engine: %s." % (
            sc["system"], sc["systemA6Fail"], ", ".join(sc["systemChecksAbsent"]) or "none"))
        o.append("Side by side: compare.html (data/work only; never committed).")
        o.append("")
    if primary_recs is not None and not args.heldout:
        miss = [r for r in primary_recs if r["counted"] and r["goldGiven"] and not r["goldMatch"]]
        if any(r["goldGiven"] for r in primary_recs):
            o.append("## Gold mismatches, cell %s (first %d of %d)" % (primary["name"], min(40, len(miss)), len(miss)))
            o.append("")
            o.append("| # | Source | Gold | Ours | Failed checks |")
            o.append("|---|---|---|---|---|")
            for r in miss[:40]:
                o.append("| %s | %s | %s | %s | %s |" % (r["idRaw"], md_escape(r.get("source")),
                                                         md_escape(" | ".join(r.get("gold") or [])),
                                                         md_escape(r.get("target")),
                                                         ", ".join(r["failed"] + (["unknown"] if r["unknownTokens"] else [])
                                                                   + (["copied"] if r["copiedSource"] else [])) or "-"))
            o.append("")
    elif args.heldout:
        o.append("Held-out run: per-cue texts are not printed (reading a held-out cue burns it, PREPLAN 6.5).")
        o.append("")
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(o))


# ------------------------------------------------------------------------------------------------------ main
def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--engine", default=DEFAULT_ENGINE, help="vpengine executable (default build-eval/...)")
    ap.add_argument("--file", default=os.path.join(ROOT, "tests", "regression", "own_dialogue.en.srt"))
    ap.add_argument("--gold", help="gold file: one line per cue, alternatives separated by ' | '")
    ap.add_argument("--pair", default="en-la")
    ap.add_argument("--combos", default="R,R+M,R+O,R+M+O")
    ap.add_argument("--fidelity", default="1,2,3")
    ap.add_argument("--out", help="output directory (default data/work/eval/<file stem>/)")
    ap.add_argument("--lexicons", default=None, help="lexicon folder (default data/work if latin.vpl is there, else "
                                                     "tests/fixtures/lex)")
    ap.add_argument("--runs", type=int, default=2, help="runs per cell for the determinism hash (default 2)")
    ap.add_argument("--primary", default="R/T2", help="cell for the claim, the mismatch table and comparisons")
    ap.add_argument("--setting", action="append", help="extra settings patch key=value (JSON value), repeatable")
    ap.add_argument("--heldout", action="store_true", help="held-out run: guard + no per-cue text in report.md")
    ap.add_argument("--heldout-texts", action="store_true", help="keep cue texts in cues_*.jsonl in a held-out run "
                                                                 "(only for the owner's review sheet)")
    ap.add_argument("--tuned", action="store_true", help="declare the file as used for tuning (acceptance file)")
    ap.add_argument("--allow-online", action="store_true", help="allow cells with O (network, settings on)")
    ap.add_argument("--timeout", type=float, default=120.0, help="seconds per request (jobs get 30x)")
    ap.add_argument("--dump-source", metavar="PATH", help="write one plain line per cue and exit")
    ap.add_argument("--compare-system", metavar="PATH", help="another system's output, one line per cue")
    ap.add_argument("--system-name", default="Google Translate")
    ap.add_argument("--keep-temp", action="store_true")
    args = ap.parse_args(argv)

    args.engine = os.path.abspath(args.engine)
    args.file = os.path.abspath(args.file)
    if not os.path.isfile(args.engine):
        die("engine not found: %s (build it: cmake --build build-eval --target vpengine)" % args.engine)
    if not os.path.isfile(args.file):
        die("file not found: " + args.file)
    args.lexicons = os.path.abspath(args.lexicons or default_lexicons())
    patch = parse_settings(args.setting)
    stem = os.path.basename(args.file).split(".")[0]

    if args.dump_source:
        out = os.path.abspath(args.dump_source)
        check_out_dir(out)
        work = L.scratch_dir()
        try:
            with L.Engine(args.engine, work, args.lexicons, timeout=args.timeout) as eng:
                _p, page = L.load_project(eng, args.file, args.pair)
        finally:
            L.remove_dir(work)
        os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
        with open(out, "w", encoding="utf-8", newline="\n") as fh:
            for c in page:
                fh.write(L.one_line(c.get("source", "")) + "\n")
        print("wrote %d lines to %s" % (len(page), rel(out)))
        return 0

    args.out = os.path.abspath(args.out or os.path.join(ROOT, "data", "work", "eval", stem))
    check_out_dir(args.out)
    os.makedirs(os.path.join(args.out, "logs"), exist_ok=True)

    guard, burned = None, set()
    if args.heldout:
        if L.is_under(args.file, L.HELDOUT_DIR):
            guard = L.heldout_guard()
            if not guard["ok"]:
                L.write_json(os.path.join(args.out, "report.json"),
                             {"refused": True, "heldoutGuard": guard, "file": rel(args.file)})
                die("REFUSED: " + guard["reason"] + ". No held-out numbers are reported.", 3)
            burned = L.read_burned().get(os.path.basename(args.file), set())
        else:
            guard = {"ok": True, "checked": 0, "mismatched": [], "missing": [], "unlisted": [],
                     "reason": "file outside tests/heldout (owner's held-out file): no FROZEN.sha256 to check"}
    gold = L.load_gold(args.gold) if args.gold else None

    combos = parse_combos(args.combos)
    fids = []
    for x in args.fidelity.split(","):
        if x.strip():
            if x.strip() not in ("1", "2", "3"):
                die("fidelity must be 1, 2 or 3")
            fids.append(int(x))
    work = L.scratch_dir()
    cells, recs_by_cell, hello = [], {}, {}
    t0 = time.time()
    try:
        for combo in combos:
            for fid in fids:
                hashes, first, recs = [], None, None
                for run in range(1, max(1, args.runs) + 1):
                    cell, r = run_cell(args, combo, fid, run, work, burned, gold, patch)
                    hello = cell.pop("hello", hello) or hello
                    if not cell["available"]:
                        first = cell
                        break
                    hashes.append(cell["targetsHash"])
                    if first is None:
                        first, recs = cell, r
                if first["available"]:
                    first["determinism"] = {"runs": len(hashes), "hashes": hashes,
                                            "identical": len(set(hashes)) == 1}
                    first["summary"] = summarise(recs)
                    fname = "cues_%s_T%d.jsonl" % (combo, fid)
                    L.write_jsonl(os.path.join(args.out, fname), recs)
                    first["cuesFile"] = fname
                    recs_by_cell[first["name"]] = recs
                cells.append(first)
                print("%-8s %s" % (first["name"], L.fmt_prop(first["summary"]["autoError"]) if first["available"]
                                   else "not available: " + first["reason"]), flush=True)

        measured = [c for c in cells if c["available"]]
        primary = next((c for c in measured if c["name"] == args.primary), measured[0] if measured else None)
        file_sum = primary["summary"] if primary else {}
        rep = {
            "schema": 1, "tool": "tools/eval/run_eval.py",
            "createdAt": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
            "file": {"path": rel(args.file), "name": os.path.basename(args.file), "sha256": L.sha256_file(args.file),
                     "pair": args.pair, "cues": file_sum.get("cues", 0), "counted": file_sum.get("counted", 0),
                     "soundOnly": file_sum.get("soundOnly", 0), "burnedExcluded": file_sum.get("burnedExcluded", 0),
                     "gold": rel(args.gold) if args.gold else None},
            "engine": {"exe": rel(args.engine), "engine": hello.get("engine"), "version": hello.get("version"),
                       "lexicons": rel(args.lexicons),
                       "lexiconFiles": [{k: l.get(k) for k in ("lang", "available", "version", "lemmas")}
                                        for l in hello.get("lexicons", [])],
                       "model": {k: (hello.get("model") or {}).get(k) for k in ("available", "cpuOk", "reason")}},
            "settingsPatch": patch,
            "provenance": provenance(args, cells, hello),
            "heldoutGuard": guard,
            "definitions": {
                "counted": "cues whose text without [sound]/(sound) descriptions and music notes has a letter",
                "autoError": "A1, A3, A4 or A5 fails, an unknown token, no output, or the source copied unchanged "
                             "(>= 2 words; DESIGN 10.4, PREPLAN 6.1/6.3)",
                "flagged": "any check fails, an unknown token, no output, or confidence check/fix",
                "goldMatch": "normalised equality with any ' | ' alternative (NFKD, lower case, combining marks, "
                             "punctuation, symbols and spacing ignored, v=u, j=i)",
                "interval": "Clopper-Pearson exact 95 %",
                "determinism": "SHA-256 over the targets in cue order, each followed by NUL; one engine process "
                               "per run",
                "peakRssKb": "VmHWM of the engine process of the cell's first run"},
            "cells": cells,
            "primaryCell": primary["name"] if primary else None,
            "claim": claim_for(primary) if primary else None,
            "wallSeconds": None,
            "childrenMaxRssKb": L.children_maxrss_kb(),
        }

        system_cmp = None
        if args.compare_system:
            if not primary:
                die("no measured cell to compare with")
            with open(args.compare_system, encoding="utf-8-sig") as fh:
                lines = fh.read().split("\n")
            if lines and lines[-1] == "":
                lines.pop()
            theirs = score_system(args, lines, primary["fidelity"], gold, burned, patch, work)
            ours = recs_by_cell[primary["name"]]
            system_cmp = compare_systems(ours, theirs, primary["name"], args.system_name)
            slug = "".join(ch if ch.isalnum() else "_" for ch in args.system_name.lower()).strip("_")
            L.write_jsonl(os.path.join(args.out, "system_%s.jsonl" % slug), theirs)
            write_compare_html(os.path.join(args.out, "compare.html"), system_cmp, ours, theirs, args.system_name,
                               primary["name"], os.path.basename(args.file))
            system_cmp["recordsFile"] = "system_%s.jsonl" % slug
            rep["system"] = system_cmp

        rep["wallSeconds"] = round(time.time() - t0, 2)
        L.write_json(os.path.join(args.out, "report.json"), rep)
        write_report_md(os.path.join(args.out, "report.md"), rep, cells, primary,
                        recs_by_cell.get(primary["name"]) if primary else None, args, system_cmp)
        print("report: %s" % rel(os.path.join(args.out, "report.md")))
        for s in rep["provenance"]["statements"]:
            print("note: " + s)
        return 0
    finally:
        if args.keep_temp:
            print("kept " + work)
        else:
            L.remove_dir(work)


if __name__ == "__main__":
    sys.exit(main())
