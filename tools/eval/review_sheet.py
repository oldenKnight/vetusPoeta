#!/usr/bin/env python3
"""Expert review sheet (PREPLAN 6.4). Python 3 stdlib only.

export: builds a CSV for the reviewer from the per-cue records of run_eval.py (cues_<cell>.jsonl, and optionally a
        second system: another cell or system_<name>.jsonl for a blind A/B sheet). Sample = every flagged cue (any
        check failing, unknown word, no output, confidence check/fix; in A/B mode flagged by either system) + a
        seeded random sample of the unflagged cues (default 300). Rows are shuffled; A/B sides are assigned at
        random per row; a seeded 10 % of the rows is marked for the second reader. The key (which side is which
        system, strata, seed) goes to <sheet>.key.json and must not be shown to the reviewer.
import: reads the filled sheet back with its key and writes review_report.json/.md: expert error rate per stratum
        with Clopper-Pearson 95 % intervals, the file-level estimate and its upper bound, the rule-of-three
        statement, the '< 1 %' sentence only when PREPLAN 6.1 allows it, McNemar for A/B, Cohen's kappa for the
        second reader.

  review_sheet.py export --cues data/work/eval/X/cues_R_T2.jsonl --out data/work/review/X.csv [--seed 20261006]
  review_sheet.py export --cues .../cues_R_T2.jsonl --b .../system_google_translate.jsonl --a-name "vetus poeta" \
                         --b-name "Google Translate" --out data/work/review/X_ab.csv
  review_sheet.py import data/work/review/X.csv            (key read from X.csv.key.json)

error_type values: grammar, meaning, vocabulary, orthography, markup, none (Spanish names accepted: gramatica,
significado, vocabulario, ortografia, formato, ninguno). Empty = not reviewed.
"""
import argparse
import csv
import json
import datetime
import os
import random
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import evallib as L  # noqa: E402

DEFAULT_SEED = 20261006
ALIASES = {"gramatica": "grammar", "gramática": "grammar", "significado": "meaning", "sentido": "meaning",
           "vocabulario": "vocabulary", "ortografia": "orthography", "ortografía": "orthography",
           "formato": "markup", "marcado": "markup", "ninguno": "none", "ninguna": "none", "ok": "none",
           "correcto": "none"}


def die(msg, code=2):
    sys.stderr.write("review_sheet: " + msg + "\n")
    sys.exit(code)


def checks_summary(r):
    bad = []
    for cid in sorted(r.get("checks", {})):
        c = r["checks"][cid]
        if not c.get("ok"):
            bad.append("%s fail%s" % (cid, (": " + c["detail"]) if c.get("detail") else ""))
    if r.get("unknown"):
        bad.append("unknown: " + " ".join(r["unknown"]))
    elif r.get("unknownTokens"):
        bad.append("unknown words: %d" % r["unknownTokens"])
    if r.get("untranslated"):
        bad.append("no output")
    if r.get("copiedSource"):
        bad.append("source copied")
    present = ",".join(sorted(r.get("checks", {}))) or "none"
    return "; ".join(bad) if bad else "ok (checks present: %s)" % present


def load_records(path):
    recs = L.read_jsonl(path)
    if recs and "source" not in recs[0]:
        die("%s has no cue texts (held-out run without --heldout-texts)" % path)
    return {r["index"]: r for r in recs}


# ------------------------------------------------------------------------------------------------------ export
def export(args):
    a = load_records(args.cues)
    b = load_records(args.b) if args.b else None
    if b is not None and sorted(a) != sorted(b):
        die("the two record files cover different cues")
    counted = sorted(i for i, r in a.items() if r.get("counted"))
    flagged = sorted(i for i in counted if a[i].get("flagged") or (b is not None and b[i].get("flagged")))
    fset = set(flagged)
    unflagged = [i for i in counted if i not in fset]
    sample = L.seeded_sample(unflagged, args.n_unflagged, args.seed)
    rows_idx = flagged + sample
    random.Random(args.seed + 1).shuffle(rows_idx)
    side_rng = random.Random(args.seed + 2)
    sides = {i: (side_rng.random() < 0.5) for i in rows_idx}     # True: A = first system
    n2 = int(round(len(rows_idx) * args.second_reader))
    second = set(random.Random(args.seed + 3).sample(rows_idx, n2)) if n2 else set()
    names = [args.a_name] + ([args.b_name] if b is not None else [])

    key_rows, out_rows = [], []
    for row, i in enumerate(rows_idx, 1):
        ra = a[i]
        k = {"row": row, "index": i, "idRaw": ra.get("idRaw"), "stratum": "census" if i in fset else "sample",
             "secondReader": i in second}
        if b is None:
            out_rows.append([row, ra.get("idRaw"), ra.get("source", ""), ra.get("target", ""), ra.get("confidence", ""),
                             checks_summary(ra), "", "", "yes" if i in second else "", ""])
        else:
            first, other = (ra, b[i]) if sides[i] else (b[i], ra)
            k["A"], k["B"] = (names[0], names[1]) if sides[i] else (names[1], names[0])
            out_rows.append([row, ra.get("idRaw"), ra.get("source", ""), first.get("target", ""),
                             checks_summary(first), other.get("target", ""), checks_summary(other), "", "", "",
                             "yes" if i in second else "", "", ""])
        key_rows.append(k)
    header = (["row", "cue", "source", "latin", "confidence", "checks", "error_type", "note", "second_reader",
               "error_type_2"] if b is None else
              ["row", "cue", "source", "latin_A", "checks_A", "latin_B", "checks_B", "error_type_A", "error_type_B",
               "note", "second_reader", "error_type_A_2", "error_type_B_2"])
    out = os.path.abspath(args.out)
    if L.is_under(out, L.ROOT) and not L.is_under(out, os.path.join(L.ROOT, "data", "work")):
        die("refusing to write a review sheet inside the repository outside data/work/")
    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
    with open(out, "w", encoding="utf-8-sig", newline="") as f:
        w = csv.writer(f)
        w.writerow(header)
        w.writerows(out_rows)
    key = {"schema": 1, "mode": "single" if b is None else "ab", "seed": args.seed,
           "seedUse": "seed: unflagged sample; seed+1: row order; seed+2: A/B sides; seed+3: second-reader rows",
           "createdAt": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
           "systems": names, "recordFiles": [os.path.basename(args.cues)] + ([os.path.basename(args.b)] if b else []),
           "population": {"counted": len(counted), "census": len(flagged), "unflaggedPopulation": len(unflagged),
                          "unflaggedSampled": len(sample), "secondReaderRows": len(second)},
           "rows": key_rows}
    L.write_json(out + ".key.json", key)
    print("wrote %s: %d rows (%d flagged, %d of %d unflagged sampled, %d for the second reader); key %s" % (
        out, len(rows_idx), len(flagged), len(sample), len(unflagged), len(second), out + ".key.json"))
    return 0


# ------------------------------------------------------------------------------------------------------ import
def norm_type(v, row, col, problems):
    v = (v or "").strip().lower()
    if not v:
        return None
    v = ALIASES.get(v, v)
    if v not in L.ERROR_TYPES:
        problems.append("row %s column %s: unknown error type %r" % (row, col, v))
        return None
    return v


def read_sheet(path):
    with open(path, encoding="utf-8-sig", newline="") as f:
        text = f.read()
    first = text.split("\n", 1)[0]
    delim = ";" if first.count(";") > first.count(",") else ("\t" if "\t" in first and "," not in first else ",")
    return list(csv.DictReader(text.splitlines(), delimiter=delim))


def system_stats(name, judgements, pop):
    """judgements: list of (stratum, error_type or None). Stratified estimate over census + one sampled stratum."""
    cen = [t for s, t in judgements if s == "census"]
    smp = [t for s, t in judgements if s == "sample"]
    cen_rev = [t for t in cen if t is not None]
    smp_rev = [t for t in smp if t is not None]
    wc = sum(1 for t in cen_rev if t != "none")
    ws = sum(1 for t in smp_rev if t != "none")
    N = pop["counted"]
    Ns = pop["unflaggedPopulation"]
    res = {"system": name,
           "reviewed": len(cen_rev) + len(smp_rev), "rows": len(judgements),
           "flagged": L.proportion(wc, len(cen_rev)), "flaggedUnreviewed": len(cen) - len(cen_rev),
           "unflaggedSample": L.proportion(ws, len(smp_rev)), "unflaggedUnreviewed": len(smp) - len(smp_rev),
           "errorTypes": {t: sum(1 for x in cen_rev + smp_rev if x == t) for t in L.ERROR_TYPES}}
    full = len(cen) == len(cen_rev) and len(cen_rev) == pop["census"] and len(smp_rev) == Ns
    res["fullReview"] = bool(full)
    if len(smp_rev):
        res["ruleOfThree"] = L.rule_of_three(len(smp_rev)) if ws == 0 else None
    sentence, claim = "", False
    if len(cen) != len(cen_rev) or (Ns and not smp_rev):
        res["estimate"] = None
        sentence = ("No file-level rate for %s: %d flagged and %d sampled cues are still unreviewed (every flagged "
                    "cue must be reviewed)." % (name, len(cen) - len(cen_rev), len(smp) - len(smp_rev)))
    elif full:
        rate = (wc + ws) / float(N) if N else None
        res["estimate"] = {"wrong": wc + ws, "counted": N, "rate": rate, "upper95": rate, "method": "full review"}
        claim = N > 0 and rate < 0.01
        sentence = (("Fewer than 1 % of the counted cues are wrong for %s: %d of %d (every cue reviewed)."
                     % (name, wc + ws, N)) if claim else
                    "%s: %d of %d counted cues are wrong (%s, every cue reviewed); no '< 1 %%' claim."
                    % (name, wc + ws, N, L.pct(rate)))
    else:
        _lo, hi = L.clopper_pearson(ws, len(smp_rev)) if smp_rev else (0.0, 0.0)
        est = (wc + Ns * (ws / float(len(smp_rev)) if smp_rev else 0.0)) / float(N) if N else None
        up = (wc + Ns * hi) / float(N) if N else None
        res["estimate"] = {"wrong": wc, "counted": N, "rate": est, "upper95": up,
                           "method": "census of flagged cues + Clopper-Pearson upper bound of the unflagged sample "
                                     "applied to the unflagged population"}
        claim = up is not None and up < 0.01
        sentence = (("Fewer than 1 % of the counted cues are wrong for %s: estimated %s, 95 %% upper bound %s "
                     "(%d wrong of %d flagged cues, all reviewed; %d wrong of %d sampled unflagged cues)."
                     % (name, L.pct(est, 2), L.pct(up, 2), wc, len(cen_rev), ws, len(smp_rev))) if claim else
                    "%s: estimated %s of the counted cues wrong, 95 %% upper bound %s; no '< 1 %%' claim "
                    "(PREPLAN 6.1 needs the upper bound below 1 %%)." % (name, L.pct(est, 2), L.pct(up, 2)))
    if smp_rev and ws == 0:
        res["ruleOfThreeSentence"] = ("0 of %d sampled unflagged cues were wrong for %s: by the rule of three the 95 %% "
                                      "upper bound on the unflagged error rate is 3/%d = %s (exact Clopper-Pearson: "
                                      "%s)." % (len(smp_rev), name, len(smp_rev), L.pct(3.0 / len(smp_rev), 2),
                                                L.pct(L.clopper_pearson(0, len(smp_rev))[1], 2)))
    res["lessThanOnePercent"] = bool(claim)
    res["sentence"] = sentence
    return res


def do_import(args):
    key_path = args.key or args.sheet + ".key.json"
    with open(key_path, encoding="utf-8") as f:
        key = json.load(f)
    rows = read_sheet(args.sheet)
    by_row = {int(k["row"]): k for k in key["rows"]}
    problems = []
    if len(rows) != len(by_row):
        problems.append("sheet has %d rows, key has %d" % (len(rows), len(by_row)))
    ab = key["mode"] == "ab"
    judg = {name: [] for name in key["systems"]}
    pairs_paired, kappa_pairs = [], []
    for r in rows:
        try:
            k = by_row[int(r["row"])]
        except (KeyError, ValueError):
            problems.append("row %r not in the key" % r.get("row"))
            continue
        if str(k.get("idRaw")) != str(r.get("cue")):
            problems.append("row %s: cue %s does not match the key (%s)" % (r["row"], r.get("cue"), k.get("idRaw")))
        if not ab:
            t = norm_type(r.get("error_type"), r["row"], "error_type", problems)
            judg[key["systems"][0]].append((k["stratum"], t))
            t2 = norm_type(r.get("error_type_2"), r["row"], "error_type_2", problems)
            if t is not None and t2 is not None:
                kappa_pairs.append((t, t2))
        else:
            ta = norm_type(r.get("error_type_A"), r["row"], "error_type_A", problems)
            tb = norm_type(r.get("error_type_B"), r["row"], "error_type_B", problems)
            judg[k["A"]].append((k["stratum"], ta))
            judg[k["B"]].append((k["stratum"], tb))
            if ta is not None and tb is not None:
                first = ta if k["A"] == key["systems"][0] else tb
                second = tb if k["A"] == key["systems"][0] else ta
                pairs_paired.append((first != "none", second != "none"))
            for col, t in (("error_type_A_2", ta), ("error_type_B_2", tb)):
                t2 = norm_type(r.get(col), r["row"], col, problems)
                if t is not None and t2 is not None:
                    kappa_pairs.append((t, t2))
    if problems and not args.force:
        die("the sheet has problems (fix them or pass --force):\n  " + "\n  ".join(problems[:50]))
    rep = {"schema": 1, "sheet": os.path.basename(args.sheet), "seed": key["seed"], "mode": key["mode"],
           "population": key["population"], "problems": problems,
           "systems": [system_stats(n, judg[n], key["population"]) for n in key["systems"]],
           "kappa": {"categories": L.cohen_kappa(kappa_pairs),
                     "binary": L.cohen_kappa([(a != "none", b != "none") for a, b in kappa_pairs])},
           "rule": "PREPLAN 6.1: '< 1 %' only if the upper bound of the 95 % interval on the expert-reviewed sample "
                   "is below 1 %, or if every cue was reviewed and fewer than 1 % are wrong."}
    if ab:
        b = sum(1 for x, y in pairs_paired if x and not y)
        c = sum(1 for x, y in pairs_paired if not x and y)
        rep["mcnemar"] = dict(L.mcnemar_exact(b, c), pairs=len(pairs_paired), first=key["systems"][0],
                              second=key["systems"][1],
                              note="on the reviewed sheet rows (flagged cues are over-represented), not weighted")
    out = args.out or os.path.splitext(args.sheet)[0] + ".review_report"
    L.write_json(out + ".json", rep)
    md = ["# Expert review: %s" % rep["sheet"], "", "Seed %s; %d counted cues, %d flagged (census), %d of %d unflagged "
          "sampled." % (key["seed"], key["population"]["counted"], key["population"]["census"],
                        key["population"]["unflaggedSampled"], key["population"]["unflaggedPopulation"]), ""]
    md += ["| System | Reviewed | Wrong among flagged | Wrong among sampled unflagged | Estimate | 95 % upper |",
           "|---|---|---|---|---|---|"]
    for s in rep["systems"]:
        e = s["estimate"]
        md.append("| %s | %d / %d | %s | %s | %s | %s |" % (
            s["system"], s["reviewed"], s["rows"], L.fmt_prop(s["flagged"]), L.fmt_prop(s["unflaggedSample"]),
            L.pct(e["rate"], 2) if e else "n/a", L.pct(e["upper95"], 2) if e else "n/a"))
    md.append("")
    for s in rep["systems"]:
        md.append("- " + s["sentence"])
        if s.get("ruleOfThreeSentence"):
            md.append("- " + s["ruleOfThreeSentence"])
        md.append("- Error types (%s): %s" % (s["system"], ", ".join("%s %d" % kv for kv in s["errorTypes"].items())))
    if ab:
        m = rep["mcnemar"]
        md.append("- McNemar exact (%s vs %s, %d rows judged on both sides): b = %d, c = %d, p = %.4g (%s)." % (
            m["first"], m["second"], m["pairs"], m["b"], m["c"], m["p"], m["note"]))
    kc = rep["kappa"]["categories"]
    md.append("- Second reader: %s" % ("Cohen's kappa %.3f on %d judgements (binary: %s)" % (
        kc["kappa"], kc["n"], rep["kappa"]["binary"]["kappa"]) if kc["kappa"] is not None else
        "%d paired judgements, kappa undefined" % kc["n"]))
    md.append("")
    md.append("Rule: " + rep["rule"])
    with open(out + ".md", "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(md) + "\n")
    print("wrote %s.json and %s.md" % (out, out))
    for s in rep["systems"]:
        print(s["sentence"])
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="mode")
    e = sub.add_parser("export")
    e.add_argument("--cues", required=True, help="cues_<cell>.jsonl from run_eval.py (system A)")
    e.add_argument("--b", help="second record file for a blind A/B sheet")
    e.add_argument("--a-name", default="vetus poeta")
    e.add_argument("--b-name", default="Google Translate")
    e.add_argument("--seed", type=int, default=DEFAULT_SEED)
    e.add_argument("--n-unflagged", type=int, default=300)
    e.add_argument("--second-reader", type=float, default=0.10, help="share of rows for the second reader")
    e.add_argument("--out", required=True)
    i = sub.add_parser("import")
    i.add_argument("sheet")
    i.add_argument("--key")
    i.add_argument("--out", help="output prefix (default <sheet without .csv>.review_report)")
    i.add_argument("--force", action="store_true", help="report even when some cells could not be read")
    args = ap.parse_args(argv)
    if args.mode == "export":
        return export(args)
    if args.mode == "import":
        return do_import(args)
    ap.print_help()
    return 2


if __name__ == "__main__":
    sys.exit(main())
