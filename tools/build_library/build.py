#!/usr/bin/env python3
"""vetus poeta library build: raw Wiktionary dumps -> intermediate files -> (later stages) .vpl files.

  python3 tools/build_library/build.py --raw data/raw --out data/work --lang la,grc,en,es --stage kaikki,resolve
  python3 tools/build_library/build.py --out data/work --check      # compare counts with expected_counts.json
  python3 tools/build_library/build.py --out data/work --report     # print <out>/report.json
  python3 tools/build_library/build.py --raw data/raw --out data/work --next --stage kaikki,resolve,import_aux,gloss,tiers,pack
      # builds into data/work/next/ (intermediates in next/<lang>/, the .vpl files in next/), never touching the live
      # data/work/*.vpl; inputs a requested stage needs but next/ lacks are copied from data/work/<lang>/ first
  --out-vpl DIR   where the pack stage writes the .vpl files (default: --out, or --out/next with --next)

Each stage writes <out>/<lang>/<stage>.json (input SHA-256s, code hash, output sizes, counts, duration, peak RSS)
and is skipped when its inputs, its code and its outputs are unchanged, unless --force.
Drift rule for --check: fail when a count moves by more than 15 % or a counted key disappears.
"""
import argparse
import hashlib
import json
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from common import LANGS, RAW_FILES, file_info, log, peak_rss_mb, read_json, write_json  # noqa: E402

STAGES = ("kaikki", "resolve", "import_aux", "gloss", "tiers", "pack")
STAGE_CODE = {"kaikki": ("kaikki.py", "common.py", "vptext.py", "tagmap.py", "features.py", "grcfix.py"),
              "resolve": ("resolve.py", "common.py", "vptext.py", "tagmap.py", "features.py"),
              "import_aux": ("import_aux.py", "betacode.py", "lexdata.py", "common.py", "vptext.py"),
              "gloss": ("gloss.py", "tiers.py", "lexdata.py", "common.py", "vptext.py", "tagmap.py",
                        "stopwords_en.txt", "stopwords_es.txt"),
              "tiers": ("tiers.py", "lexdata.py", "common.py", "vptext.py"),
              "pack": ("pack.py", "resolve.py", "lexdata.py", "common.py", "vptext.py", "tagmap.py", "features.py",
                       "grcfix.py", "morphcut.py", "whitaker_gen.py", "supplement.py", "esgloss.py", "gloss.py")}
# stages that exist only for some languages
STAGE_LANGS = {"import_aux": ("la", "grc"), "gloss": ("la", "grc"), "tiers": ("la", "grc")}
CURATED = os.path.normpath(os.path.join(HERE, "..", "..", "data", "curated"))
KAIKKI_OUTPUTS = {"la": [], "grc": [], "en": ["translations_la.tsv", "translations_grc.tsv", "translations_es.tsv"],
                  "es": ["latin_glosses.tsv", "translations_la.tsv", "translations_grc.tsv", "translations_en.tsv"]}
BASE_OUTPUTS = ["lemmas.jsonl", "table_forms.tsv", "formpages.tsv"]
RESOLVE_OUTPUTS = ["analyses.tsv", "lemma_index.tsv"]
EXPECTED_PATH = os.path.join(HERE, "expected_counts.json")
DRIFT = 0.15
# counts that go into expected_counts.json (only when >= MIN_EXPECTED, so tiny counts do not trip the 15 % rule)
CHECKED = {"kaikki": ("entries", "lemma_records", "lemmas_with_table", "form_pages", "formpage_rows", "table_rows",
                      "head_rows", "kind_lemma", "kind_form", "kind_alt", "kind_formtable", "kind_alttable",
                      "translations_la", "translations_grc", "translations_es", "es_glosses_la", "es_glosses_grc",
                      "es_entries_la", "es_entries_grc", "es_translations_la", "es_translations_grc", "es_translations_en", "el_desc",
                      "el_same"),
           "resolve": ("lemmas", "analyses", "distinct_keys", "formpage_rows", "unresolved_rows",
                       "unresolved_pages", "lemma_self_analyses"),
           "import_aux": ("whitaker_entries", "whitaker_joined", "whitaker_inflects", "ls_entries", "ls_joined",
                          "lsj_entries", "lsj_joined_exact", "dcc_la_joined", "dcc_la_es_joined", "dcc_grc_joined"),
           "gloss": ("gloss_en_covered", "gloss_es_covered", "gloss_es_pivot", "gloss_es_eswikt", "gloss_es_dcc",
                     "gloss_es_curated", "senses", "revx_en_keywords", "revx_en_candidates", "revx_es_keywords",
                     "revx_es_candidates", "pivot_rows_joined"),
           "tiers": ("tier1", "tier2", "tier3", "tier0", "emoji_matched", "freq_rank_whitaker", "shared_el"),
           "pack": ("lemmas", "analyses", "senses", "candidates", "genx_cells", "size", "gloss_en_covered")}
MIN_EXPECTED = 100


def code_hash(stage):
    h = hashlib.sha256()
    for name in STAGE_CODE[stage]:
        with open(os.path.join(HERE, name), "rb") as f:
            h.update(name.encode() + b"\0" + f.read())
    return h.hexdigest()


def stage_applies(stage, lang):
    return lang in STAGE_LANGS.get(stage, LANGS)


def _work(out, lang, names):
    return [os.path.join(out, lang, n) for n in names]


def _curated(names):
    return [os.path.join(CURATED, n) for n in names if os.path.exists(os.path.join(CURATED, n))]


def stage_inputs(stage, lang, raw, out):
    if stage == "kaikki":
        return [os.path.join(raw, RAW_FILES[lang])]
    if stage == "resolve":
        return [os.path.join(out, lang, n) for n in BASE_OUTPUTS]
    import import_aux
    import gloss
    if stage == "import_aux":
        return import_aux.inputs(lang, os.path.join(raw, "aux")) + _work(out, lang, ["lemmas.jsonl", "formpages.tsv"])
    if stage == "gloss":
        names = ["lemmas.jsonl"] + import_aux.outputs(lang)
        extra = [os.path.join(out, "en", "translations_%s.tsv" % lang), os.path.join(out, "en", "translations_es.tsv"),
                 os.path.join(out, "es", "translations_%s.tsv" % lang), os.path.join(out, "es", "latin_glosses.tsv"),
                 os.path.join(out, "en", "analyses.tsv"), os.path.join(out, "es", "analyses.tsv"),
                 # the gloss lemmatiser also reads the lemma indexes (undeclared before B4b)
                 os.path.join(out, "en", "lemma_index.tsv"), os.path.join(out, "es", "lemma_index.tsv")]
        cur = ["tiers_%s.tsv" % lang, "gloss_es_%s.tsv" % lang, "valency_%s.tsv" % lang]
        return _work(out, lang, names) + extra + _curated(cur)
    if stage == "tiers":
        names = ["lemmas.jsonl", "gloss.tsv"] + [n for n in import_aux.outputs(lang) if not n.startswith("ls")]
        return _work(out, lang, names) + _curated(["tiers_%s.tsv" % lang, "emoji_%s.tsv" % lang])
    if stage == "pack":
        import morphcut
        import pack
        names = ["lemmas.jsonl", "analyses.tsv"]
        if lang in ("la", "grc"):
            names += ["table_forms.tsv", "tiers.tsv"] + gloss.outputs(lang)
        extra = []
        if lang == "la":
            names += ["whitaker.tsv", "whitaker_inflects.tsv"]
            extra = _curated([pack.LA_MACRONS])
        elif lang == "grc":
            extra = _curated([pack.GRC_OVERRIDES, pack.supplement.FILE])
        else:
            extra = morphcut.input_files(out, raw, CURATED, lang)
            if lang == "es":
                import esgloss
                extra += [p for p in esgloss.input_files(out, raw) if p not in extra]
        return _work(out, lang, names) + extra
    raise ValueError(stage)


def stage_outputs(stage, lang):
    if stage == "kaikki":
        return BASE_OUTPUTS + KAIKKI_OUTPUTS[lang]
    if stage == "resolve":
        return RESOLVE_OUTPUTS
    import importlib
    if stage == "pack":
        import pack
        outs = [os.path.join(VPL_DIR or "..", pack.FILE_NAMES[lang])]
        return outs + (["whitaker_only.tsv"] if lang == "la" else [])
    return importlib.import_module(stage).outputs(lang)


def input_name(p):
    """Key of an input in the stage meta: the basename for the A4 stages (unchanged), parent/basename otherwise."""
    parent = os.path.basename(os.path.dirname(p))
    return os.path.basename(p) if parent in LANGS or parent in ("raw",) else parent + "/" + os.path.basename(p)


# --out-vpl: the directory the pack stage writes the .vpl files to (None = <out>)
VPL_DIR = None


def seed_next(base, out, stages, langs, raw):
    """--next: copy into <out>/<lang>/ the inputs of the requested stages that <out> lacks but <base> has (with their
    stage meta), so a partial rebuild in next/ never reads or writes the live files. Copies, never links: a stage
    rewriting an intermediate in place must not truncate the live one."""
    import shutil
    copied = 0
    produced = set()  # outputs of the requested stages are written by the build itself: never seeded
    for lang in langs:
        for stage in STAGES:
            if stage in stages and stage_applies(stage, lang):
                produced.update(os.path.abspath(os.path.join(out, lang, n)) for n in stage_outputs(stage, lang))
    for lang in langs:
        for stage in STAGES:
            if stage not in stages or not stage_applies(stage, lang):
                continue
            try:
                inputs = stage_inputs(stage, lang, raw, out)
            except Exception:  # noqa: BLE001 - a missing module input is reported by run_stage
                inputs = []
            for p in inputs:
                rel = os.path.relpath(p, out)
                if rel.startswith(".."):
                    continue  # raw or curated input
                src = os.path.join(base, rel)
                if os.path.abspath(p) in produced or os.path.exists(p) or not os.path.exists(src):
                    continue
                os.makedirs(os.path.dirname(p), exist_ok=True)
                shutil.copy2(src, p)
                copied += 1
                # the stage meta of the producing stage, so an unchanged stage is still skipped
                d = os.path.dirname(src)
                for prod in STAGES:
                    m = os.path.join(d, prod + ".json")
                    t = os.path.join(os.path.dirname(p), prod + ".json")
                    if os.path.exists(m) and not os.path.exists(t) and prod not in stages:
                        shutil.copy2(m, t)
    if copied:
        log("--next: copied %d input file(s) from %s" % (copied, base))
    return copied


def up_to_date(meta, stage, lang, inputs, out):
    if not meta or meta.get("code") != code_hash(stage):
        return False
    old_in = meta.get("inputs", {})
    for p in inputs:
        if not os.path.exists(p):
            return False
        old = old_in.get(input_name(p))
        if not old or file_info(p, old)["sha256"] != old.get("sha256"):
            return False
    for name, info in meta.get("outputs", {}).items():
        p = os.path.join(out, lang, name)
        if not os.path.exists(p) or os.path.getsize(p) != info.get("size"):
            return False
    return True


def run_stage(stage, lang, raw, out, force):
    d = os.path.join(out, lang)
    os.makedirs(d, exist_ok=True)
    meta_path = os.path.join(d, stage + ".json")
    meta = read_json(meta_path)
    inputs = stage_inputs(stage, lang, raw, out)
    for p in inputs:
        if not os.path.exists(p):
            hint = " (run tools/build_library/fetch.sh aux)" if stage == "import_aux" else ""
            raise SystemExit("missing input for %s/%s: %s%s" % (lang, stage, p, hint))
    if not force and up_to_date(meta, stage, lang, inputs, out):
        log("[%s %s] inputs unchanged, skipped (use --force to rerun)" % (lang, stage))
        return meta
    old_in = (meta or {}).get("inputs", {})
    in_info = {input_name(p): file_info(p, old_in.get(input_name(p))) for p in inputs}
    t0 = time.time()
    if stage == "kaikki":
        import kaikki
        res = kaikki.run(lang, raw, out)
    elif stage == "resolve":
        import resolve
        res = resolve.run(lang, out)
    else:
        res = run_isolated(stage, lang, raw, out)
    outputs = {}
    for name in stage_outputs(stage, lang):
        p = os.path.join(d, name)
        if os.path.exists(p):
            outputs[name] = {"size": os.path.getsize(p)}
    meta = dict(res)
    meta.update({"stage": stage, "lang": lang, "inputs": in_info, "code": code_hash(stage), "outputs": outputs,
                 "wall_s": round(time.time() - t0, 1),
                 "peak_rss_mb": res.get("peak_rss_mb") if stage not in ("kaikki", "resolve") else peak_rss_mb()})
    write_json(meta_path, meta)
    return meta


ISOLATED = """import json, os, sys
sys.path.insert(0, %r)
stage, lang, raw, out, dest = sys.argv[1:6]
import importlib
m = importlib.import_module(stage)
if stage == "import_aux":
    res = m.run(lang, os.path.join(raw, "aux"), out)
elif stage == "pack":
    res = m.run(lang, out, raw, sys.argv[6] or None)
else:
    res = m.run(lang, out)
with open(dest, "w", encoding="utf-8") as f:
    json.dump(res, f)
"""


def run_isolated(stage, lang, raw, out):
    """Run a B4 stage in a fresh interpreter so its peak RSS is its own (ru_maxrss of the child)."""
    import subprocess
    import tempfile
    fd, dest = tempfile.mkstemp(prefix="stage-", suffix=".json", dir=os.path.join(out, lang))
    os.close(fd)
    try:
        subprocess.run([sys.executable, "-c", ISOLATED % HERE, stage, lang, raw, out, dest, VPL_DIR or ""],
                       check=True)
        return read_json(dest)
    finally:
        os.unlink(dest)


def dump_date(raw):
    info = read_json(os.path.join(raw, "SOURCES.json"), {}) or {}
    return {k: v.get("last_modified") for k, v in info.items() if isinstance(v, dict)}


def lang_report(out, lang):
    k = read_json(os.path.join(out, lang, "kaikki.json"))
    r = read_json(os.path.join(out, lang, "resolve.json"))
    if not k:
        return None
    kc = k.get("counts", {})
    rep = {"entries": kc.get("entries", 0), "lemma_entries": kc.get("lemma_records", 0),
           "kinds": {n[5:]: v for n, v in sorted(kc.items()) if n.startswith("kind_")},
           "form_pages": kc.get("form_pages", 0), "formpage_rows": kc.get("formpage_rows", 0),
           "table_rows": kc.get("table_rows", 0), "head_rows": kc.get("head_rows", 0),
           "top_pos": dict(sorted(k.get("pos", {}).items(), key=lambda kv: (-kv[1], kv[0]))[:30]),
           "kaikki_counts": kc, "time_s": {"kaikki": k.get("wall_s")},
           "peak_rss_mb": {"kaikki": k.get("peak_rss_mb")}}
    if r:
        rc = r.get("counts", {})
        rep.update({"analyses": rc.get("analyses", 0), "distinct_keys": rc.get("distinct_keys", 0),
                    "analyses_per_key": r.get("analyses_per_key"),
                    "unresolved_form_pages": {"rows": rc.get("unresolved_rows", 0),
                                              "pages": rc.get("unresolved_pages", 0),
                                              "sample": r.get("unresolved_sample", [])},
                    "chain_depth": r.get("chain_depth"), "flags": r.get("flags"),
                    "unknown_tags": r.get("tags", {}).get("unknown", {}),
                    "lossy_tags": r.get("tags", {}).get("lossy", {}), "resolve_counts": rc})
        rep["time_s"]["resolve"] = r.get("wall_s")
        rep["peak_rss_mb"]["resolve"] = r.get("peak_rss_mb")
    for stage in ("import_aux", "gloss", "tiers", "pack"):
        m = read_json(os.path.join(out, lang, stage + ".json"))
        if not m:
            continue
        rep["time_s"][stage] = m.get("wall_s")
        rep["peak_rss_mb"][stage] = m.get("peak_rss_mb")
        mc = m.get("counts", {})
        if stage == "import_aux":
            rep["join_rates"] = {k: v for k, v in sorted(m.get("join", {}).items()) if not isinstance(v, dict)}
            rep["whitaker_letters"] = {k: v for k, v in m.get("join", {}).items() if isinstance(v, dict)}
            rep["import_aux_counts"] = mc
        elif stage == "gloss":
            rep["gloss_en"] = {k[9:]: v for k, v in sorted(mc.items()) if k.startswith("gloss_en_")}
            rep["gloss_es"] = {k[9:]: v for k, v in sorted(mc.items()) if k.startswith("gloss_es_")}
            rep["revx"] = {k: mc.get(k, 0) for k in ("revx_en_keywords", "revx_en_candidates", "revx_es_keywords",
                                                     "revx_es_candidates")}
            rep["gloss_counts"] = mc
        elif stage == "tiers":
            rep["tiers"] = {k: mc.get(k, 0) for k in ("tier1", "tier2", "tier3", "tier0")}
            rep["emoji"] = {"matched": mc.get("emoji_matched", 0), "unmatched": mc.get("emoji_unmatched", 0),
                            "unmatched_keys": m.get("emoji_unmatched", [])}
            rep["tiers_counts"] = mc
            rep["curated_tier_unmatched"] = m.get("curated_unmatched", [])
        else:
            rep["vpl"] = {"file": m.get("file"), "size": m.get("size"), "sha256": m.get("sha256"),
                          "sections": m.get("sections"), "counts": mc}
    rep["time_s"]["total"] = round(sum(v for v in rep["time_s"].values() if v), 1)
    tv = parse_time_v(os.path.join(out, lang, "time-v.txt"))
    if tv:
        rep["time_v"] = tv
    return rep


def parse_time_v(path):
    """Wall time and peak RSS from a GNU `/usr/bin/time -v` log saved next to the outputs (optional)."""
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            text = f.read()
    except OSError:
        return None
    res = {}
    for line in text.splitlines():
        line = line.strip()
        if line.startswith("Elapsed (wall clock) time"):
            res["wall"] = line.rsplit(" ", 1)[-1]
        elif line.startswith("Maximum resident set size (kbytes):"):
            res["max_rss_mb"] = round(int(line.rsplit(" ", 1)[-1]) / 1024.0, 1)
        elif line.startswith("Command being timed:"):
            res["command"] = line.split(":", 1)[1].strip().strip('"')
    return res or None


def write_reports(out, raw):
    full = {"dump_last_modified": dump_date(raw), "langs": {}}
    for lang in LANGS:
        rep = lang_report(out, lang)
        if rep is None:
            continue
        write_json(os.path.join(out, lang, "report.json"), rep)
        full["langs"][lang] = rep
    write_json(os.path.join(out, "report.json"), full)
    return full


def current_counts(out):
    cur = {}
    for lang in LANGS:
        for stage in STAGES:
            meta = read_json(os.path.join(out, lang, stage + ".json"))
            if not meta:
                continue
            for name, v in meta.get("counts", {}).items():
                cur.setdefault(lang, {})["%s.%s" % (stage, name)] = v
    return cur


def drift_errors(expected, current, langs=None, drift=DRIFT):
    """List of human-readable failures; empty when within the drift rule."""
    errs = []
    for lang, exp in sorted(expected.items()):
        if lang.startswith("_") or not isinstance(exp, dict):
            continue
        if langs and lang not in langs:
            continue
        cur = current.get(lang, {})
        for name, ev in sorted(exp.items()):
            if name not in cur:
                errs.append("%s %s: missing (expected %s)" % (lang, name, ev))
                continue
            av = cur[name]
            base = max(abs(ev), 1)
            if abs(av - ev) / float(base) > drift:
                errs.append("%s %s: %s -> %s (%+.1f %%)" % (lang, name, ev, av, 100.0 * (av - ev) / base))
    return errs


def expected_from(current):
    exp = {"_rule": "fail when a count moves > 15 %% or disappears; counts < %d are not checked" % MIN_EXPECTED}
    for lang, counts in sorted(current.items()):
        sel = {}
        for name, v in counts.items():
            stage, cname = name.split(".", 1)
            if cname in CHECKED.get(stage, ()) and isinstance(v, int) and v >= MIN_EXPECTED:
                sel[name] = v
        exp[lang] = dict(sorted(sel.items()))
    return exp


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--raw", default="data/raw")
    ap.add_argument("--out", default="data/work")
    ap.add_argument("--lang", default=",".join(LANGS))
    ap.add_argument("--stage", default="")
    ap.add_argument("--force", action="store_true")
    ap.add_argument("--check", action="store_true", help="compare counts with expected_counts.json")
    ap.add_argument("--report", action="store_true", help="print <out>/report.json")
    ap.add_argument("--write-expected", action="store_true", help="write expected_counts.json from this run")
    ap.add_argument("--expected", default=EXPECTED_PATH)
    ap.add_argument("--next", action="store_true",
                    help="build into <out>/next (intermediates and .vpl files); the live <out> stays untouched")
    ap.add_argument("--out-vpl", default="", help="directory for the .vpl files (default: the output directory)")
    a = ap.parse_args(argv)
    global VPL_DIR
    base = a.out
    if a.next:
        a.out = os.path.join(a.out, "next")
        os.makedirs(a.out, exist_ok=True)
    VPL_DIR = os.path.abspath(a.out_vpl) if a.out_vpl else None
    if VPL_DIR:
        os.makedirs(VPL_DIR, exist_ok=True)
    langs = [x for x in a.lang.split(",") if x]
    for lang in langs:
        if lang not in LANGS:
            ap.error("unknown language %r" % lang)
    stages = [x for x in a.stage.split(",") if x]
    for s in stages:
        if s not in STAGES:
            ap.error("unknown stage %r (this build has %s)" % (s, ", ".join(STAGES)))
    t0 = time.time()
    if a.next and stages:
        seed_next(base, a.out, stages, langs, a.raw)
    for lang in langs:
        for s in STAGES:
            if s in stages and stage_applies(s, lang):
                run_stage(s, lang, a.raw, a.out, a.force)
    if stages or a.report or a.write_expected:
        full = write_reports(a.out, a.raw)
        full["build_wall_s"] = round(time.time() - t0, 1)
        full["build_peak_rss_mb"] = peak_rss_mb()
    rc = 0
    if a.write_expected:
        exp = expected_from(current_counts(a.out))
        old = read_json(a.expected, {}) or {}
        old.update(exp)
        write_json(a.expected, old)
        log("wrote %s" % a.expected)
    if a.check:
        expected = read_json(a.expected)
        if expected is None:
            log("no %s" % a.expected)
            return 2
        errs = drift_errors(expected, current_counts(a.out), langs)
        for e in errs:
            log("DRIFT " + e)
        log("check: %s" % ("FAIL (%d)" % len(errs) if errs else "OK"))
        rc = 1 if errs else 0
    if a.report:
        sys.stdout.write(json.dumps(read_json(os.path.join(a.out, "report.json")), ensure_ascii=False, indent=1))
        sys.stdout.write("\n")
    return rc


if __name__ == "__main__":
    sys.exit(main())
