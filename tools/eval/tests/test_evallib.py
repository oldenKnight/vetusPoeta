#!/usr/bin/env python3
"""Tests of the evaluation harness (task C10). Run: python3 -m unittest discover -s tools/eval/tests
The integration test needs a built vpengine: $VP_ENGINE, else build-eval/ or build/ (skipped when none exists)."""
import csv
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
EVAL = os.path.dirname(HERE)
ROOT = os.path.dirname(os.path.dirname(EVAL))
sys.path.insert(0, EVAL)
import evallib as L  # noqa: E402
import review_sheet  # noqa: E402

REG_SRT = os.path.join(ROOT, "tests", "regression", "own_dialogue.en.srt")
REG_GOLD = os.path.join(ROOT, "tests", "regression", "expected", "own_dialogue.la.gold.txt")


def read_text(path, encoding="utf-8"):
    with open(path, encoding=encoding) as f:
        return f.read()


def read_bytes(path):
    with open(path, "rb") as f:
        return f.read()


def load_json(path):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def quiet(fn, *a):
    with open(os.devnull, "w") as dn:
        old, sys.stdout = sys.stdout, dn
        try:
            return fn(*a)
        finally:
            sys.stdout = old


def find_engine():
    cands = [os.environ.get("VP_ENGINE", "")] + [os.path.join(ROOT, d, "engine", "cli", "vpengine")
                                                 for d in ("build-eval", "build", "build-cli")]
    for c in cands:
        if c and os.path.isfile(c) and os.access(c, os.X_OK):
            return c
    return None


class Normalise(unittest.TestCase):
    def test_macrons_and_breves(self):
        self.assertEqual(L.normalise("Rōma"), L.normalise("Roma"))
        self.assertEqual(L.normalise("puĕllă"), "puella")
        self.assertEqual(L.normalise("Rōma"), L.normalise("Rōma"))   # precomposed vs combining
        self.assertEqual(L.normalise("Sērō veniō!"), "sero ueneo".replace("ueneo", "uenio"))

    def test_u_v_i_j(self):
        self.assertEqual(L.normalise("Vīvō"), L.normalise("uiuo"))
        self.assertEqual(L.normalise("Iūlia"), L.normalise("Julia"))
        self.assertEqual(L.normalise("iānua"), L.normalise("janua"))

    def test_punctuation_space_case_tags_emoji(self):
        self.assertEqual(L.normalise("  Quis   es?! "), "quis es")
        self.assertEqual(L.normalise("<i>Ō mē miseram,</i> ō mē!"), "o me miseram o me")
        self.assertEqual(L.normalise("{\\an8}Canis 🐕 dormit."), "canis dormit")
        self.assertNotEqual(L.normalise("Quis es"), L.normalise("Quid es"))

    def test_alternatives(self):
        self.assertTrue(L.gold_match("Exspecta me, quaeso", ["Manē mē, quaesō.", "Exspectā mē, quaesō."]))
        self.assertFalse(L.gold_match("Manē, quaesō", ["Manē mē, quaesō."]))
        self.assertIsNone(L.gold_match("x", None))

    def test_load_gold(self):
        gold = L.load_gold(REG_GOLD)
        self.assertEqual(len(gold), 114)
        self.assertEqual(gold[4], ["Manē mē, quaesō.", "Exspectā mē, quaesō."])
        self.assertEqual(gold[0], ["Quō īs?"])

    def test_counted(self):
        self.assertTrue(L.counted("Where are you going?"))
        self.assertFalse(L.counted("[music]"))
        self.assertFalse(L.counted("♪ ♪"))
        self.assertTrue(L.counted("♪ Twinkle, twinkle ♪"))
        self.assertFalse(L.counted("(laughs) ..."))


class Statistics(unittest.TestCase):
    def test_clopper_pearson_known_values(self):
        lo, hi = L.clopper_pearson(5, 10)      # R binom.test(5, 10): 0.1870860 0.8129140
        self.assertAlmostEqual(lo, 0.1870860, places=6)
        self.assertAlmostEqual(hi, 0.8129140, places=6)
        lo, hi = L.clopper_pearson(1, 10)      # R binom.test(1, 10): 0.002528579 0.445016117
        self.assertAlmostEqual(lo, 0.002528579, places=8)
        self.assertAlmostEqual(hi, 0.445016117, places=8)
        lo, hi = L.clopper_pearson(0, 300)     # closed form 1 - 0.025^(1/300)
        self.assertEqual(lo, 0.0)
        self.assertAlmostEqual(hi, 1 - 0.025 ** (1 / 300.0), places=10)
        lo, hi = L.clopper_pearson(20, 20)
        self.assertAlmostEqual(lo, 0.025 ** (1 / 20.0), places=10)
        self.assertEqual(hi, 1.0)
        self.assertEqual(L.clopper_pearson(0, 0), (None, None))

    def test_betainc(self):
        self.assertAlmostEqual(L.betainc(1, 1, 0.3), 0.3, places=12)
        self.assertAlmostEqual(L.betainc(2, 3, 0.4), 0.5248, places=10)   # 1 - sum_{k<2} C(4,k) .4^k .6^(4-k)

    def test_proportion_and_rule_of_three(self):
        p = L.proportion(2, 114)
        self.assertEqual((p["num"], p["den"]), (2, 114))
        self.assertTrue(p["ci95"][0] < p["rate"] < p["ci95"][1])
        self.assertAlmostEqual(L.rule_of_three(300), 0.01)

    def test_mcnemar(self):
        self.assertAlmostEqual(L.mcnemar_exact(1, 9)["p"], 2 * 11 / 1024.0)   # 0.021484375
        self.assertEqual(L.mcnemar_exact(0, 0)["p"], 1.0)
        self.assertEqual(L.mcnemar_exact(5, 5)["p"], 1.0)
        self.assertAlmostEqual(L.mcnemar_exact(0, 6)["p"], 2 / 64.0)

    def test_kappa(self):
        pairs = [("a", "a")] * 20 + [("a", "b")] * 5 + [("b", "a")] * 10 + [("b", "b")] * 15
        k = L.cohen_kappa(pairs)            # po 0.7, pe 0.5 -> kappa 0.4
        self.assertAlmostEqual(k["kappa"], 0.4, places=6)
        self.assertIsNone(L.cohen_kappa([("x", "x")] * 3)["kappa"])


class HeldoutGuard(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="vp-heldout-")
        self.dir = os.path.join(self.tmp, "heldout")
        shutil.copytree(L.HELDOUT_DIR, self.dir)

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_real_set_is_intact(self):
        g = L.heldout_guard()
        self.assertTrue(g["ok"], g["reason"])
        self.assertEqual(g["checked"], 3)

    def test_tampered_copy_refused(self):
        self.assertTrue(L.heldout_guard(self.dir)["ok"])
        with open(os.path.join(self.dir, "own_heldout.en.srt"), "ab") as f:
            f.write(b"\n")
        g = L.heldout_guard(self.dir)
        self.assertFalse(g["ok"])
        self.assertEqual(g["mismatched"], ["own_heldout.en.srt"])

    def test_missing_and_unlisted(self):
        os.remove(os.path.join(self.dir, "oz_dialogue.en.srt"))
        with open(os.path.join(self.dir, "extra.srt"), "w") as f:
            f.write("1\n")
        g = L.heldout_guard(self.dir)
        self.assertFalse(g["ok"])
        self.assertEqual(g["missing"], ["oz_dialogue.en.srt"])
        self.assertEqual(g["unlisted"], ["extra.srt"])

    def test_burned(self):
        with open(os.path.join(self.dir, "BURNED.txt"), "w") as f:
            f.write("# moved to regression\nown_heldout.en.srt 12\nown_heldout.en.srt 40  # debugging\n")
        self.assertEqual(L.read_burned(self.dir), {"own_heldout.en.srt": {"12", "40"}})
        self.assertTrue(L.heldout_guard(self.dir)["ok"])   # BURNED.txt is metadata, not a data file


class Structure(unittest.TestCase):
    SRC = (b"1\n00:00:01,000 --> 00:00:02,000\n<i>Hello there.</i>\n\n"
           b"2\n00:00:03,000 --> 00:00:04,000\n{\\an8}Up here.\n\n")

    def test_identical_structure(self):
        dst = self.SRC.replace(b"Hello there.", b"Salve.").replace(b"Up here.", b"Hic sursum.")
        self.assertTrue(L.verify_structure(self.SRC, dst, "srt")["ok"])

    def test_timing_and_tags_changed(self):
        v = L.verify_structure(self.SRC, self.SRC.replace(b"00:00:03,000", b"00:00:03,001"), "srt")
        self.assertFalse(v["ok"])
        self.assertEqual(v["headerDiffs"], [1])
        v = L.verify_structure(self.SRC, self.SRC.replace(b"{\\an8}", b""), "srt")
        self.assertEqual(v["tagDiffs"], [1])
        v = L.verify_structure(self.SRC, self.SRC.split(b"\n\n2")[0] + b"\n\n", "srt")
        self.assertFalse(v["ok"])

    def test_hash(self):
        self.assertEqual(L.targets_hash(["a", "b"]), L.targets_hash(["a", "b"]))
        self.assertNotEqual(L.targets_hash(["a\nb"]), L.targets_hash(["a", "b"]))


def fake_records(n=500, flagged_every=7):
    recs = []
    for i in range(n):
        fl = i % flagged_every == 0
        recs.append({"index": i, "idRaw": str(i + 1), "counted": i % 50 != 49, "flagged": fl,
                     "source": "Sentence %d." % i, "target": "Sententia %d." % i, "confidence": "check" if fl else "ok",
                     "checks": {"A1": {"ok": not fl, "detail": "x" if fl else ""}}, "unknownTokens": 0,
                     "untranslated": False, "copiedSource": False})
    return recs


class ReviewSheet(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="vp-review-")

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def _fill(self, sheet, fn):
        with open(sheet, encoding="utf-8-sig", newline="") as f:
            rows = list(csv.DictReader(f))
        for r in rows:
            fn(r)
        with open(sheet, "w", encoding="utf-8-sig", newline="") as f:
            w = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
            w.writeheader()
            w.writerows(rows)
        return rows

    def test_single_round_trip(self):
        cues = os.path.join(self.tmp, "cues.jsonl")
        L.write_jsonl(cues, fake_records())
        sheet = os.path.join(self.tmp, "sheet.csv")
        quiet(review_sheet.main, ["export", "--cues", cues, "--out", sheet, "--seed", "7"])
        key = load_json(sheet + ".key.json")
        pop = key["population"]
        counted = 500 - 10
        flagged = sum(1 for i in range(500) if i % 7 == 0 and i % 50 != 49)
        self.assertEqual((pop["counted"], pop["census"]), (counted, flagged))
        self.assertEqual(pop["unflaggedSampled"], 300)
        self.assertEqual(pop["secondReaderRows"], round((flagged + 300) * 0.1))
        # same seed -> same sheet
        sheet2 = os.path.join(self.tmp, "sheet2.csv")
        quiet(review_sheet.main, ["export", "--cues", cues, "--out", sheet2, "--seed", "7"])
        self.assertEqual(read_bytes(sheet), read_bytes(sheet2))
        census = {str(k["idRaw"]) for k in key["rows"] if k["stratum"] == "census"}

        def fill(r):
            # flagged cues: every third wrong (grammar); unflagged sample: none wrong; reader 2 disagrees once
            n = int(r["cue"])
            r["error_type"] = ("gramática" if n % 3 == 0 else "none") if r["cue"] in census else "none"
            if r["second_reader"] == "yes":
                r["error_type_2"] = r["error_type"]
        rows = self._fill(sheet, fill)
        second = [r for r in rows if r["second_reader"] == "yes"]
        self._fill(sheet, lambda r: r.update(error_type_2="meaning") if r["row"] == second[0]["row"] else None)
        quiet(review_sheet.main, ["import", sheet])
        rep = load_json(os.path.join(self.tmp, "sheet.review_report.json"))
        s = rep["systems"][0]
        wrong_flagged = sum(1 for c in census if int(c) % 3 == 0)
        self.assertEqual((s["flagged"]["num"], s["flagged"]["den"]), (wrong_flagged, flagged))
        self.assertEqual((s["unflaggedSample"]["num"], s["unflaggedSample"]["den"]), (0, 300))
        self.assertIn("rule of three", s["ruleOfThreeSentence"])
        self.assertIn("3/300 = 1.00 %", s["ruleOfThreeSentence"])
        self.assertFalse(s["lessThanOnePercent"])        # many flagged cues are wrong
        self.assertIn("no '< 1 %' claim", s["sentence"])
        self.assertEqual(s["errorTypes"]["grammar"], wrong_flagged)
        self.assertLess(rep["kappa"]["categories"]["kappa"], 1.0)
        self.assertEqual(rep["kappa"]["categories"]["n"], len(second))

    def test_ab_blind_and_mcnemar(self):
        a = fake_records(120, 4)
        b = [dict(r, target="Other %d." % r["index"], flagged=(r["index"] % 5 == 0)) for r in a]
        pa, pb = os.path.join(self.tmp, "a.jsonl"), os.path.join(self.tmp, "b.jsonl")
        L.write_jsonl(pa, a)
        L.write_jsonl(pb, b)
        sheet = os.path.join(self.tmp, "ab.csv")
        quiet(review_sheet.main, ["export", "--cues", pa, "--b", pb, "--a-name", "ours", "--b-name", "theirs",
                                   "--out", sheet, "--n-unflagged", "20"])
        key = load_json(sheet + ".key.json")
        sides = {k["A"] for k in key["rows"]}
        self.assertEqual(sides, {"ours", "theirs"})                      # sides really are shuffled
        header = read_text(sheet, "utf-8-sig").split("\n", 1)[0]
        self.assertNotIn("ours", header)
        self.assertNotIn("theirs", header)
        kmap = {str(k["row"]): k for k in key["rows"]}

        def fill(r):
            # "theirs" is wrong on every row, "ours" never
            k = kmap[r["row"]]
            r["error_type_A"] = "meaning" if k["A"] == "theirs" else "none"
            r["error_type_B"] = "meaning" if k["B"] == "theirs" else "none"
        self._fill(sheet, fill)
        quiet(review_sheet.main, ["import", sheet])
        rep = load_json(os.path.join(self.tmp, "ab.review_report.json"))
        m = rep["mcnemar"]
        n = len(key["rows"])
        self.assertEqual((m["b"], m["c"], m["first"]), (0, n, "ours"))
        self.assertAlmostEqual(m["p"], L.binom_two_sided_half(0, n))
        by = {s["system"]: s for s in rep["systems"]}
        self.assertEqual(by["ours"]["flagged"]["num"] + by["ours"]["unflaggedSample"]["num"], 0)
        self.assertEqual(by["theirs"]["reviewed"], n)

    def test_bad_error_type_refused(self):
        cues = os.path.join(self.tmp, "cues.jsonl")
        L.write_jsonl(cues, fake_records(40))
        sheet = os.path.join(self.tmp, "s.csv")
        quiet(review_sheet.main, ["export", "--cues", cues, "--out", sheet])
        self._fill(sheet, lambda r: r.update(error_type="typo"))
        old_err, sys.stderr = sys.stderr, open(os.devnull, "w")
        try:
            with self.assertRaises(SystemExit):
                review_sheet.main(["import", sheet])
        finally:
            sys.stderr.close()
            sys.stderr = old_err


class GtCompareJs(unittest.TestCase):
    def test_pure_functions(self):
        node = shutil.which("node")
        if not node:
            self.skipTest("node not installed")
        js = ("var g=require(%s);var items=[];for(var i=0;i<60;i++)items.push({text:'Line number '+i+'.'});"
              "var b=g.makeBatches(items,25,4500,7000);var big=g.makeBatches([{text:new Array(3000).join('a')},"
              "{text:new Array(3000).join('b')}],25,4500);"
              "console.log(JSON.stringify([b.length,b[0].length,b[2].length,big.length,"
              "g.splitParts('Salve.\\n\\nVale.\\n  \\n\\nIterum.'),g.hasLetter('♪ ♪'),g.hasLetter('[music]'),g.makeBatches(items,25,4500,200).every(function(b){var n=0;b.forEach(function(it){"
              "n+=encodeURIComponent(it.text).length+6;});return n<=200&&b.length>=1;}),g.hasLetter('♪ Hi ♪'),"
              "g.pageUrl({baseUrl:'https://x/',sl:'en',tl:'la'},'a b\\n\\nc')]))"
              % json.dumps(os.path.join(EVAL, "gt_compare.js")))
        out = subprocess.run([node, "-e", js], capture_output=True, text=True, timeout=60)
        self.assertEqual(out.returncode, 0, out.stderr)
        self.assertEqual(json.loads(out.stdout), [3, 25, 10, 2, ["Salve.", "Vale.", "Iterum."], False, False, True, True,
                                                 "https://x/?sl=en&tl=la&op=translate&text=a%20b%0A%0Ac"])


CHROME = os.environ.get("VP_CHROMIUM", "/opt/pw-browsers/chromium-1194/chrome-linux/chrome")
NODE_PATH = os.environ.get("NODE_PATH", "/opt/node-tools/node_modules")


@unittest.skipUnless(shutil.which("node") and os.path.isfile(CHROME) and
                     os.path.isdir(os.path.join(NODE_PATH, "playwright")), "node + Playwright + Chromium not installed")
class GtCompareMock(unittest.TestCase):
    """gt_compare.js end to end against tests/fixtures/gt_mock.html (file: URL, no network): URL submission of a
    whole batch, per-line fallback when the part count differs, typing fallback, 25-line batch splitting."""

    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="vp-gt-")
        self.mock = "file://" + os.path.join(HERE, "fixtures", "gt_mock.html")
        src = read_text(os.path.join(ROOT, "tests", "regression", "own_dialogue.en.txt")).split("\n")
        self.lines = [l for l in src if l.strip()][:30]
        self.inp = os.path.join(self.tmp, "in.txt")
        with open(self.inp, "w", encoding="utf-8") as f:
            f.write("\n".join(self.lines[:29] + ["[music]"]) + "\n")

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def run_tool(self, name, mode, limit):
        env = dict(os.environ, NODE_PATH=NODE_PATH)
        p = subprocess.run(["node", os.path.join(EVAL, "gt_compare.js"), self.inp, "--name", name, "--out-dir",
                            self.tmp, "--base-url", self.mock + "?mode=" + mode, "--wait-ms", "2500", "--limit",
                            str(limit)], capture_output=True, text=True, timeout=300, env=env)
        self.assertEqual(p.returncode, 0, p.stderr)
        out = read_text(os.path.join(self.tmp, name + ".gt.la.txt")).split("\n")[:-1]
        return out, load_json(os.path.join(self.tmp, name + ".gt.meta.json"))["stats"]

    def test_url_batches(self):
        out, st = self.run_tool("url", "", 30)
        self.assertEqual(out, ["LA: " + l for l in self.lines[:29]] + ["[music]"])   # no-letter line copied
        self.assertEqual((st["batches"], st["requests"], st["fallbacks"], st["typed"]), (2, 2, 0, 0))

    def test_count_mismatch_falls_back_per_line(self):
        out, st = self.run_tool("merge", "merge", 5)
        self.assertEqual(out, ["LA: " + l for l in self.lines[:5]])
        self.assertEqual((st["fallbacks"], st["requests"]), (1, 6))

    def test_typing_fallback(self):
        out, st = self.run_tool("typed", "nourl", 5)
        self.assertEqual(out, ["LA: " + l for l in self.lines[:5]])
        self.assertEqual(st["typed"], 1)


@unittest.skipUnless(find_engine(), "no vpengine build (set VP_ENGINE or build build-eval)")
class EndToEnd(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp(prefix="vp-eval-e2e-")
        cls.out = os.path.join(cls.tmp, "out")
        cls.engine = find_engine()
        # A fake "other system": the preferred gold lines (our own sentences), one per cue.
        cls.sys_file = os.path.join(cls.tmp, "other.txt")
        with open(cls.sys_file, "w", encoding="utf-8") as f:
            for g in L.load_gold(REG_GOLD):
                f.write((g[0] if g else "") + "\n")
        cmd = [sys.executable, os.path.join(EVAL, "run_eval.py"), "--engine", cls.engine, "--file", REG_SRT,
               "--gold", REG_GOLD, "--combos", "R,R+M,R+O,M", "--fidelity", "1,2", "--out", cls.out,
               "--lexicons", os.path.join(ROOT, "tests", "fixtures", "lex"),
               "--compare-system", cls.sys_file, "--system-name", "Gold stand-in"]
        cls.proc = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
        cls.dump = os.path.join(cls.tmp, "src.txt")
        cls.dump_proc = subprocess.run([sys.executable, os.path.join(EVAL, "run_eval.py"), "--engine", cls.engine,
                                        "--file", REG_SRT, "--dump-source", cls.dump, "--lexicons",
                                        os.path.join(ROOT, "tests", "fixtures", "lex")],
                                       capture_output=True, text=True, timeout=120)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def test_run_succeeded(self):
        self.assertEqual(self.proc.returncode, 0, self.proc.stderr + self.proc.stdout)
        for name in ("report.json", "report.md", "compare.html", "cues_R_T1.jsonl", "cues_R_T2.jsonl",
                     "system_gold_stand_in.jsonl"):
            self.assertTrue(os.path.isfile(os.path.join(self.out, name)), name)

    def test_report_structure(self):
        rep = load_json(os.path.join(self.out, "report.json"))
        cells = {c["name"]: c for c in rep["cells"]}
        self.assertEqual(sorted(cells), sorted(["R/T1", "R/T2", "R+M/T1", "R+M/T2", "R+O/T1", "R+O/T2",
                                                "M/T1", "M/T2"]))
        self.assertTrue(rep["provenance"]["stubEngine"] or rep["engine"]["engine"])
        self.assertTrue(rep["provenance"]["tunedFile"])
        for name in ("R+O/T1", "M/T2"):
            self.assertFalse(cells[name]["available"])
            self.assertTrue(cells[name]["reason"])
        for name in ("R/T1", "R/T2"):
            c = cells[name]
            s = c["summary"]
            self.assertEqual(s["cues"], 114)
            self.assertEqual(s["counted"] + s["soundOnly"] + s["burnedExcluded"], 114)
            self.assertEqual(s["autoError"]["den"], s["counted"])
            self.assertEqual(s["flagged"]["den"], s["counted"])
            self.assertEqual(s["autoErrorFlagged"]["den"] + s["autoErrorUnflagged"]["den"], s["counted"])
            self.assertEqual(s["autoErrorFlagged"]["num"] + s["autoErrorUnflagged"]["num"], s["autoError"]["num"])
            self.assertEqual(sum(v["num"] for v in s["confidence"].values()), s["counted"])
            self.assertEqual(s["gold"]["den"], 114)
            lo, hi = s["autoError"]["ci95"]
            self.assertTrue(0.0 <= lo <= s["autoError"]["rate"] <= hi <= 1.0)
            self.assertTrue(c["structure"]["ok"], c["structure"])
            self.assertEqual(c["structure"]["cuesSource"], 114)
            self.assertEqual(c["determinism"]["runs"], 2)
            self.assertTrue(c["determinism"]["identical"])
            self.assertTrue(c["peakRssKb"] is None or c["peakRssKb"] > 0)
            self.assertEqual(c["engineEvalRun"]["cues"], 114)
            recs = L.read_jsonl(os.path.join(self.out, c["cuesFile"]))
            self.assertEqual(len(recs), 114)
            self.assertEqual(sum(1 for r in recs if r["counted"] and r["autoError"]), s["autoError"]["num"])
            if rep["provenance"]["stubEngine"]:
                self.assertEqual(s["gold"]["num"], 0)                       # the stub echoes English
                self.assertEqual(s["autoError"]["num"], 114)                # every cue is the source copied
        self.assertFalse(rep["claim"]["lessThanOnePercent"])

    def test_report_md(self):
        md = read_text(os.path.join(self.out, "report.md"))
        self.assertIn("## Publication table (PREPLAN 6.7)", md)
        self.assertIn("not available", md)
        self.assertIn("no '< 1 %' claim is made", md)
        self.assertIn("TUNED FILE", md)
        if "STUB" in md:
            self.assertIn("STUB ENGINE", md)
        self.assertIn("## Gold mismatches, cell R/T2 (first 40 of 114)", md)
        rows = [l for l in md.split("\n") if re.match(r"^\| (R|M|O)[+A-Z]*/T\d \|", l)]
        self.assertEqual(len(rows), 8 + 2)            # publication table: 8 cells; confidence table: 2 measured
        pub = md.split("## Publication table")[1].split("##")[0]
        widths = {l.count(" | ") for l in pub.split("\n") if l.startswith("| R/T")}
        self.assertEqual(widths, {11})                 # 12 columns in every measured row

    def test_system_comparison(self):
        rep = load_json(os.path.join(self.out, "report.json"))
        sc = rep["system"]
        self.assertEqual(sc["counted"], 114)
        self.assertEqual(sc["gold"]["system"]["num"], 114)             # the stand-in is the gold itself
        m = sc["gold"]["mcnemar"]
        self.assertEqual((m["b"], m["c"]), (114 - sc["gold"]["ours"]["num"], 0))
        self.assertLess(m["p"], 0.001)
        html = read_text(os.path.join(self.out, "compare.html"))
        self.assertIn("Gold stand-in", html)
        self.assertEqual(html.count("<tr><td class=\"n\">"), 114)

    def test_dump_source(self):
        self.assertEqual(self.dump_proc.returncode, 0, self.dump_proc.stderr)
        lines = read_text(self.dump).split("\n")[:-1]
        self.assertEqual(len(lines), 114)
        self.assertEqual(lines[0], "Where are you going?")

    def test_heldout_run_hides_texts(self):
        out = os.path.join(self.tmp, "heldout")
        p = subprocess.run([sys.executable, os.path.join(EVAL, "run_eval.py"), "--engine", self.engine, "--heldout",
                            "--file", os.path.join(L.HELDOUT_DIR, "own_heldout.en.srt"), "--combos", "R",
                            "--fidelity", "2", "--runs", "1", "--out", out, "--lexicons",
                            os.path.join(ROOT, "tests", "fixtures", "lex")], capture_output=True, text=True, timeout=300)
        self.assertEqual(p.returncode, 0, p.stderr)
        rep = load_json(os.path.join(out, "report.json"))
        self.assertTrue(rep["heldoutGuard"]["ok"])
        self.assertFalse(rep["provenance"]["tunedFile"])
        recs = L.read_jsonl(os.path.join(out, "cues_R_T2.jsonl"))
        self.assertTrue(recs and all("source" not in r and "target" not in r for r in recs))
        md = read_text(os.path.join(out, "report.md"))
        self.assertIn("Held-out guard: OK", md)
        self.assertIn("per-cue texts are not printed", md)
        self.assertNotIn("Gold mismatches", md)

    def test_refuses_output_in_repo(self):
        p = subprocess.run([sys.executable, os.path.join(EVAL, "run_eval.py"), "--engine", self.engine,
                            "--out", os.path.join(ROOT, "tools", "eval", "out_should_not_exist")],
                           capture_output=True, text=True, timeout=60)
        self.assertNotEqual(p.returncode, 0)
        self.assertIn("refusing", p.stderr)
        self.assertFalse(os.path.exists(os.path.join(ROOT, "tools", "eval", "out_should_not_exist")))


if __name__ == "__main__":
    unittest.main()
