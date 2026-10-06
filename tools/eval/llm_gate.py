#!/usr/bin/env python3
"""Latin minimal-pair gate for the local model (PREPLAN 2.2, DESIGN 1.1). Python 3 stdlib only.

Runs `vpengine llm-gate <latin.vpl> <model.gguf> --pairs N --json <details>` (pair generation and scoring are in
C++: engine/llm/src/gate.cpp), recomputes every number from the per-pair scores, and writes a numbers-only report
(tools/eval/llm_gate_report.json by default; the per-pair details with the clauses stay in the scratch file).

Decision rule, fixed before the first run: primary metric = pairwise accuracy on the summed log-probability of the
clause (correct clause strictly higher). Latin-side reranking ships only if accuracy >= 75 % and the lower bound of
the 95 % Wilson interval is above 70 %. The per-token mean is reported as a secondary number only.

    python3 tools/eval/llm_gate.py [--vpengine build-llm/engine/cli/vpengine] [--lexicon data/work/latin.vpl]
                                   [--model models/<file>.gguf] [--pairs 1000] [--seed 1] [--out <report.json>]
                                   [--details <pairs.json>] [--from-details <pairs.json>]
"""
import argparse
import collections
import glob
import json
import math
import os
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def wilson(k, n, z=1.96):
    if n <= 0:
        return {"p": 0.0, "lo": 0.0, "hi": 0.0}
    p = k / n
    denom = 1 + z * z / n
    centre = (p + z * z / (2 * n)) / denom
    half = z * math.sqrt(p * (1 - p) / n + z * z / (4 * n * n)) / denom
    return {"p": p, "lo": max(0.0, centre - half), "hi": min(1.0, centre + half)}


def rounded(iv):
    return {k: round(v, 4) for k, v in iv.items()}


def breakdown(items, key):
    acc = collections.OrderedDict()
    for name in sorted({it[key] for it in items}):
        sub = [it for it in items if it[key] == name]
        k = sum(1 for it in sub if it["goodLp"] > it["badLp"])
        acc[name] = {"pairs": len(sub), "correct": k, "interval": rounded(wilson(k, len(sub)))}
    return acc


def same_len(items):
    """Pairs whose two clauses have the same number of tokens: the summed metric without a length advantage."""
    sub = [it for it in items if it["goodTokens"] == it["badTokens"]]
    k = sum(1 for it in sub if it["goodLp"] > it["badLp"])
    return {"pairs": len(sub), "correct": k, "interval": rounded(wilson(k, len(sub)))}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--vpengine", default=os.path.join(ROOT, "build-llm", "engine", "cli", "vpengine"))
    ap.add_argument("--lexicon", default=os.path.join(ROOT, "data", "work", "latin.vpl"))
    ap.add_argument("--model", default="")
    ap.add_argument("--pairs", type=int, default=1000)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--threads", type=int, default=4)
    ap.add_argument("--out", default=os.path.join(ROOT, "tools", "eval", "llm_gate_report.json"))
    ap.add_argument("--details", default="")
    ap.add_argument("--from-details", default="", help="skip the run; recompute from an existing details file")
    a = ap.parse_args()

    if a.from_details:
        details_path = a.from_details
        wall = None
    else:
        model = a.model or (sorted(glob.glob(os.path.join(ROOT, "models", "*.gguf"))) or [""])[0]
        if not model or not os.path.isfile(model):
            sys.exit("llm_gate: no model file (use --model or put a .gguf in models/)")
        if not os.path.isfile(a.lexicon):
            sys.exit("llm_gate: no lexicon at %s (build it with tools/build_library)" % a.lexicon)
        details_path = a.details or os.path.join(tempfile.gettempdir(), "vp_llm_gate_pairs.json")
        cmd = [a.vpengine, "llm-gate", a.lexicon, model, "--pairs", str(a.pairs), "--seed", str(a.seed),
               "--threads", str(a.threads), "--json", details_path]
        t0 = time.time()
        r = subprocess.run(cmd)
        wall = time.time() - t0
        if r.returncode != 0:
            sys.exit("llm_gate: vpengine llm-gate failed (%d)" % r.returncode)

    with open(details_path, encoding="utf-8") as f:
        d = json.load(f)
    items = d["items"]
    n = len(items)
    k_sum = sum(1 for it in items if it["goodLp"] > it["badLp"])
    k_mean = sum(1 for it in items if it["goodTokens"] > 0 and it["badTokens"] > 0
                 and it["goodLp"] / it["goodTokens"] > it["badLp"] / it["badTokens"])
    ties = sum(1 for it in items if it["goodLp"] == it["badLp"])
    if k_sum != d["correct"] or k_mean != d["correctMean"]:
        sys.exit("llm_gate: recomputed counts differ from the engine's (%d/%d vs %d/%d)"
                 % (k_sum, k_mean, d["correct"], d["correctMean"]))
    s = wilson(k_sum, n)
    m = wilson(k_mean, n)
    passed = n > 0 and s["p"] >= 0.75 and s["lo"] > 0.70
    report = collections.OrderedDict([
        ("gate", "Latin minimal pairs, PREPLAN 2.2"),
        ("rule", "pass iff accuracy >= 0.75 and Wilson 95% lower bound > 0.70 (summed log-probability)"),
        ("pairsRequested", d["requested"]),
        ("pairs", n),
        ("seed", d["seed"]),
        ("threads", d["threads"]),
        ("promptVersion", 1),
        ("summedLogProb", {"correct": k_sum, "interval": rounded(s)}),
        ("meanLogProbPerToken", {"correct": k_mean, "interval": rounded(m)}),
        ("ties", ties),
        ("sameTokenCount", same_len(items)),
        ("byTemplate", breakdown(items, "template")),
        ("byCorruption", breakdown(items, "corruption")),
        ("meanTokens", {"good": round(sum(it["goodTokens"] for it in items) / max(n, 1), 2),
                        "bad": round(sum(it["badTokens"] for it in items) / max(n, 1), 2)}),
        ("msPerPair", round(d["msPerPair"], 1)),
        ("loadMs", d["loadMs"]),
        ("passed", passed),
    ])
    if wall is not None:
        report["wallSeconds"] = round(wall, 1)
    with open(a.out, "w", encoding="utf-8") as f:
        json.dump(report, f, indent=1)
        f.write("\n")
    print("pairs %d: summed %d/%d = %.1f %% [%.1f, %.1f]; mean %d/%d = %.1f %% [%.1f, %.1f]; %s"
          % (n, k_sum, n, 100 * s["p"], 100 * s["lo"], 100 * s["hi"], k_mean, n, 100 * m["p"], 100 * m["lo"],
             100 * m["hi"], "PASSED" if passed else "NOT PASSED"))
    print("report: %s" % a.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
