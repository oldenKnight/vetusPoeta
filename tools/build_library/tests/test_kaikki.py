import json
import os
import shutil
import tempfile
import unittest

import _paths
import kaikki


def run_fixture(lang, out):
    return kaikki.run(lang, "", out, input_path=os.path.join(_paths.KAIKKI_FIXTURES, lang + ".jsonl"))


def tsv(path):
    with open(path, encoding="utf-8") as f:
        return [line.rstrip("\n").split("\t") for line in f]


def lemmas(out, lang):
    with open(os.path.join(out, lang, "lemmas.jsonl"), encoding="utf-8") as f:
        return [json.loads(line) for line in f]


class KaikkiLatinTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.out = tempfile.mkdtemp(prefix="vp-kaikki-")
        cls.res = run_fixture("la", cls.out)
        cls.lem = {r["word"]: r for r in lemmas(cls.out, "la")}
        cls.table = tsv(os.path.join(cls.out, "la", "table_forms.tsv"))
        cls.fp = tsv(os.path.join(cls.out, "la", "formpages.tsv"))

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.out)

    def test_kinds(self):
        c = self.res["counts"]
        self.assertEqual(c["entries"], 12)
        self.assertEqual(c["kind_lemma"], 4)          # puella amo birota Iulius
        self.assertEqual(c["kind_alttable"], 1)       # amoo
        self.assertEqual(c["kind_alt"], 1)            # puela
        self.assertEqual(c["kind_form"], 6)
        self.assertEqual(c["lemma_records"], 5)
        self.assertEqual([r["id"] for r in lemmas(self.out, "la")], list(range(5)))

    def test_lemma_record(self):
        amo = self.lem["amo"]
        self.assertEqual(amo["head"], "am\u014d")
        self.assertEqual(amo["key"], "amo")
        self.assertEqual(amo["ht"], "la-verb")
        self.assertEqual(amo["class"], ["conjugation-1"])
        self.assertTrue(amo["has_table"])
        self.assertTrue(amo["pp"].startswith("am\u014d (present infinitive am\u0101re"))
        self.assertEqual([s["g"] for s in amo["senses"]], ["to love", "to like to do"])  # last-level gloss
        self.assertEqual(amo["senses"][1]["q"], "poetic")
        pu = self.lem["puella"]
        self.assertEqual(pu["gender"], ["feminine"])
        self.assertEqual(pu["senses"][0]["ex"], 1)
        self.assertEqual(self.lem["Iulius"]["head"], "I\u016blius")   # from the head template argument
        self.assertEqual(self.lem["Iulius"]["key"], "iulius")
        self.assertEqual(self.lem["birota"].get("late"), 1)
        self.assertNotIn("late", amo)
        self.assertEqual(self.lem["amoo"]["kind"], "alttable")
        self.assertEqual(self.lem["amoo"]["of"], ["am\u014d"])

    def test_table_rows(self):
        amo_id = str(self.lem["amo"]["id"])
        rows = [r for r in self.table if r[0] == amo_id]
        self.assertIn([amo_id, "am\u0101vit", "amauit", "active indicative perfect singular third-person",
                       "conjugation", "conjugation-1"], rows)
        self.assertEqual(len(rows), 5)  # head-line forms are not repeated when a table exists
        pu_rows = [r for r in self.table if r[0] == str(self.lem["puella"]["id"])]
        self.assertEqual(len(pu_rows), 5)  # the "-" cell is skipped
        self.assertTrue(all(r[5] == "" for r in pu_rows))

    def test_formpages(self):
        by = {}
        for r in self.fp:
            by.setdefault(r[0], []).append(r)
        self.assertEqual(by["amabit"][0], ["amabit", "amabit", "am\u0101bit", "amo",
                                           "active future indicative singular third-person", "verb", "form",
                                           "am\u014d"])
        # no form_of field: recognised from the gloss because the head template is a form head
        r = by["amabimus"][0]
        self.assertEqual(r[3], "amo")
        self.assertEqual(r[4], "active first-person future indicative plural")
        self.assertEqual(by["puela"][0][6], "alt")
        self.assertEqual(by["puela"][0][3], "puella")
        self.assertEqual(by["amantis"][0][5], "participle")
        self.assertIn("amoo", by)  # the alttable entry also points at its target


class KaikkiGreekTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.out = tempfile.mkdtemp(prefix="vp-kaikki-")
        cls.res = run_fixture("grc", cls.out)
        cls.lem = {r["word"]: r for r in lemmas(cls.out, "grc")}
        cls.table = tsv(os.path.join(cls.out, "grc", "table_forms.tsv"))

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.out)

    def test_markers_and_el(self):
        chora = self.lem["\u03c7\u03ce\u03c1\u03b1"]
        self.assertEqual(chora["head"], "\u03c7\u03ce\u03c1\u1fb1")
        self.assertEqual(chora["el"], 1)
        self.assertEqual(chora["class"], ["declension-1"])
        self.assertEqual(self.lem["\u03c4\u03b9\u03bc\u03ac\u03c9"]["el"], 0)   # el descendant, other spelling
        self.assertNotIn("el", self.lem["\u03bb\u03cc\u03b3\u03bf\u03c2"])
        cid = str(chora["id"])
        markers = [r[5] for r in self.table if r[0] == cid and r[4] == "declension"]
        self.assertEqual(markers, ["Attic declension-1"] * 3 + ["Ionic declension-1"] * 2)
        heads = [r for r in self.table if r[0] == cid and r[4] == "head"]
        self.assertEqual([h[1] for h in heads], ["\u03c7\u03ce\u03c1\u03b7"])  # the alternative spelling only
        tid = str(self.lem["\u03c4\u03b9\u03bc\u03ac\u03c9"]["id"])
        tm = [r[5] for r in self.table if r[0] == tid]
        self.assertEqual(tm, ["present", "present", "contracted present", "contracted present", "Epic imperfect",
                              "aorist"])
        # keys keep accents, drop length marks and final sigma
        self.assertIn("\u03c7\u03ce\u03c1\u03b1\u03c3", [r[2] for r in self.table])

    def test_counts(self):
        c = self.res["counts"]
        self.assertEqual(c["kind_lemma"], 3)
        self.assertEqual(c["kind_form"], 2)
        self.assertEqual(c["formpage_rows"], 2)


class KaikkiEnglishSpanishTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.out = tempfile.mkdtemp(prefix="vp-kaikki-")
        cls.en = run_fixture("en", cls.out)
        cls.es = run_fixture("es", cls.out)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.out)

    def test_translations_entry_and_sense_level(self):
        la = tsv(os.path.join(self.out, "en", "translations_la.tsv"))
        self.assertEqual(sorted(r[3] for r in la), ["am\u014d", "aqua", "lympha"])
        lympha = [r for r in la if r[3] == "lympha"][0]
        self.assertEqual(lympha[:3], ["water", "noun", "A body of water."])  # sense text from the gloss
        aqua = [r for r in la if r[3] == "aqua"][0]
        self.assertEqual(aqua, ["water", "noun", "clear liquid", "aqua", "feminine"])
        grc = tsv(os.path.join(self.out, "en", "translations_grc.tsv"))
        self.assertEqual([r[3] for r in grc], ["\u1f55\u03b4\u03c9\u03c1"])
        es = tsv(os.path.join(self.out, "en", "translations_es.tsv"))
        self.assertEqual([r[3] for r in es], ["agua"])
        self.assertEqual(self.en["counts"]["translations_la"], 3)

    def test_english_forms(self):
        table = tsv(os.path.join(self.out, "en", "table_forms.tsv"))
        forms = sorted((r[1], r[3]) for r in table)
        self.assertIn(("waters", "plural"), forms)
        self.assertIn(("loved", "participle past"), forms)
        self.assertNotIn("have loved", [f for f, _ in forms])  # multi-word forms are skipped for en/es
        fp = tsv(os.path.join(self.out, "en", "formpages.tsv"))
        self.assertEqual(fp[0][:5], ["waters", "waters", "waters", "water", "plural"])

    def test_spanish(self):
        d = os.path.join(self.out, "es")
        lem = lemmas(self.out, "es")
        self.assertEqual(sorted(r["word"] for r in lem), ["amar", "ni\u00f1a"])
        nina = [r for r in lem if r["word"] == "ni\u00f1a"][0]
        self.assertEqual(nina["gender"], ["feminine"])
        fp = tsv(os.path.join(d, "formpages.tsv"))
        self.assertEqual(fp[0][3], "amar")
        self.assertEqual(fp[0][4], "indicative present second-person singular")  # from the Spanish gloss
        gl = tsv(os.path.join(d, "latin_glosses.tsv"))
        self.assertEqual([(r[0], r[2], r[3]) for r in gl],
                         [("puella", "Ni\u00f1a.", "la"), ("puella", "Muchacha.", "la"),
                          ("puellae", "Forma del genitivo singular de puella.", "la"),
                          ("\u1f55\u03b4\u03c9\u03c1", "Agua.", "grc")])
        self.assertEqual(gl[2][5], "puella")
        tla = tsv(os.path.join(d, "translations_la.tsv"))
        self.assertEqual(tla, [["amar", "verb", "1", "amo", ""]])
        table = tsv(os.path.join(d, "table_forms.tsv"))
        self.assertNotIn("he amado", [r[1] for r in table])
        self.assertEqual(self.es["counts"]["es_entries_la"], 2)


if __name__ == "__main__":
    unittest.main()
