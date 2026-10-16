#!/usr/bin/env python3
"""Tests of the core vocabulary audit (task C41, tools/eval/core_audit.py). No engine needed: the comparison of an
engine gloss with a core-list sense, the core-list reader and the report. Our own toy rows only (no core-list text).
Run: python3 -m unittest discover -s tools/eval/tests"""
import os
import shutil
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import core_audit as A  # noqa: E402


class VerdictTest(unittest.TestCase):
    def test_core_sense(self):
        self.assertEqual(A.verdict("The army.", "army", {"army", "host"}, "en", "exercitus")[0], "ok")
        self.assertEqual(A.verdict("I go away.", "go away", {"go", "away", "depart"}, "en", "abeō")[0], "ok")
        self.assertEqual(A.verdict("He seeks.", "to seek", {"seek", "ask"}, "en", "petō")[0], "ok")       # "to" dropped
        self.assertEqual(A.verdict("Men.", "men", {"man", "husband"}, "en", "ἀνήρ")[0], "ok")            # irregular
        self.assertEqual(A.verdict("Buscan.", "buscar", {"buscar", "pedir"}, "es", "quaerō")[0], "ok")

    def test_wrong_sense_and_oddities(self):
        self.assertEqual(A.verdict("The exercise.", "exercise", {"army"}, "en", "exercitus")[0], "nosense")
        self.assertEqual(A.verdict("The not a lie.", "not a lie", {"truth"}, "en", "ἀλήθεια")[0], "nosense")
        self.assertEqual(A.verdict("x", "the act of cutting or lopping something off", {"slaughter"}, "en", "caedēs")[0], "long")
        self.assertEqual(A.verdict("I usually weaker than βούλομαι.", "usually weaker than βούλομαι", {"wish"}, "en",
                                   "ἐθέλω")[0], "leak")
        self.assertEqual(A.verdict("[agō].", "[agō]", {"do"}, "en", "agō")[0], "leak")
        self.assertEqual(A.verdict("", "", {"do"}, "en", "agō")[0], "missing")
        self.assertEqual(A.verdict("The boat-house.", "boat house", {"house", "home"}, "en", "x")[0], "ok")

    def test_stems(self):
        self.assertEqual(A.stem_en("walking"), "walk")
        self.assertEqual(A.stem_en("cities"), "city")
        self.assertEqual(A.stem_es("rápidamente"), "rápida")


class ReaderReportTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="vp-c41-test-")

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_read_and_report(self):
        core = os.path.join(self.tmp, "core.tsv")
        with open(core, "w", encoding="utf-8") as f:
            f.write("#key\thead\trank\tdefinition\tpos\tgroup\theadword\tlemma_ids\n")
            f.write("toyus\ttoyus\t2\tlittle toy; plaything\tNoun\tThings\ttoyus -ī m.\t7\n")
            f.write("toyo\ttoyō\t1\tplay with (+abl.)\tVerb: 1st\tPlay\ttoyō -āre\t8,9\n")
        rows = A.read_core(core)
        self.assertEqual([r["head"] for r in rows], ["toyus", "toyō"])
        self.assertEqual(rows[1]["pos"], "verb")
        self.assertEqual(rows[1]["ids"], [8, 9])
        rows.sort(key=lambda r: (r["rank"], r["head"]))
        outs = {"en": [{"target": "I play.", "wbw": "to play", "conf": "ok", "flags": [], "lemmas": [8]},
                       {"target": "The bear.", "wbw": "bear", "conf": "ok", "flags": [], "lemmas": [99]}],
                "es": [{"target": "Juego.", "wbw": "jugar", "conf": "ok", "flags": [], "lemmas": [8]},
                       {"target": "El juguete.", "wbw": "juguete", "conf": "check", "flags": ["x"], "lemmas": [7]}]}
        res = A.audit(rows, outs, None, {"play": {"jugar"}, "toy": {"juguete"}, "plaything": {"juguete"}})
        self.assertEqual(res[0]["en"]["verdict"], "ok")
        self.assertEqual(res[1]["en"]["verdict"], "nosense")
        self.assertTrue(res[1]["en"]["other"])            # the engine chose another lemma than the core list's
        self.assertEqual(res[1]["es"]["verdict"], "ok")
        lines = A.summary(res, ("en", "es"))
        self.assertIn("flagged but rated OK 1", lines[0])
        rep = os.path.join(self.tmp, "audit.tsv")
        A.write_report(rep, res, ("en", "es"), "toy")
        with open(rep, encoding="utf-8") as f:
            text = f.read()
        self.assertIn("toyō", text)
        self.assertNotIn("plaything", text)                # definitions never reach the report


if __name__ == "__main__":
    unittest.main()
