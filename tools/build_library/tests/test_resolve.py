import os
import shutil
import tempfile
import unittest

import _paths
import features as F
import kaikki
import resolve
import tagmap
import vptext


def analyses(out, lang):
    with open(os.path.join(out, lang, "analyses.tsv"), encoding="utf-8") as f:
        rows = [line.rstrip("\n").split("\t") for line in f]
    return [(k, int(l), int(p), d, int(fl)) for k, l, p, d, fl in rows]


def lemma_ids(out, lang):
    ids = {}
    with open(os.path.join(out, lang, "lemma_index.tsv"), encoding="utf-8") as f:
        for line in f:
            p = line.rstrip("\n").split("\t")
            ids[p[2]] = int(p[0])
    return ids


class ResolveLatinTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.out = tempfile.mkdtemp(prefix="vp-resolve-")
        kaikki.run("la", "", cls.out, input_path=os.path.join(_paths.KAIKKI_FIXTURES, "la.jsonl"))
        cls.res = resolve.run("la", cls.out)
        cls.an = analyses(cls.out, "la")
        cls.ids = lemma_ids(cls.out, "la")

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.out)

    def find(self, key):
        return [a for a in self.an if a[0] == key]

    def test_sorted_and_unique(self):
        keys = [(a[0], a[1], a[2], a[3]) for a in self.an]
        self.assertEqual(keys, sorted(keys))
        self.assertEqual(len(keys), len(set(keys)))

    def test_table_and_formpage_merge(self):
        amo = self.ids["am\u014d"]
        amat = self.find("amat")
        self.assertEqual(len(amat), 1)
        self.assertEqual(amat[0][1], amo)
        self.assertEqual(amat[0][4], resolve.FLAG_TABLE)
        amabit = self.find("amabit")
        self.assertEqual(len(amabit), 1)
        self.assertEqual(amabit[0][3], "am\u0101bit")
        self.assertEqual(amabit[0][4], resolve.FLAG_FORMPAGE)
        self.assertEqual(amabit[0][2], tagmap.tags_to_features(
            ["active", "future", "indicative", "singular", "third-person"], "verb"))
        self.assertEqual(self.find("amabimus")[0][1], amo)   # gloss-only form page

    def test_chain_depth(self):
        amo = self.ids["am\u014d"]
        # amans -> amo (1), amantis -> amans -> amo (2), amantium -> amantis -> amans -> amo (3)
        for key in ("amans", "amantis", "amantium"):
            got = self.find(key)
            self.assertEqual([a[1] for a in got], [amo], key)
            u = F.unpack(got[0][2])
            self.assertEqual(u["pos"], F.POS["verb"])          # participle form of a verb: verb + mood
            self.assertEqual(u["mood"], F.MOOD["participle"])
        self.assertEqual(self.find("amantiumque"), [])           # depth 4: unresolved
        self.assertEqual(self.res["counts"]["unresolved_rows"], 1)
        # depth 1: amabit amabimus amans puela amoo; depth 2: amantis; depth 3: amantium
        self.assertEqual(self.res["chain_depth"], {"1": 5, "2": 1, "3": 1})
        self.assertIn("amantiumque -> amantium", self.res["unresolved_sample"])

    def test_alternative_and_late(self):
        puela = self.find("puela")
        self.assertEqual(len(puela), 1)
        self.assertEqual(puela[0][1], self.ids["puella"])
        self.assertTrue(puela[0][4] & resolve.FLAG_ALT)
        amoat = self.find("amoat")
        self.assertEqual(amoat[0][1], self.ids["amo\u014d"])
        self.assertTrue(amoat[0][4] & resolve.FLAG_ALT and amoat[0][4] & resolve.FLAG_TABLE)
        for a in self.find("birota") + self.find("birotae"):
            self.assertTrue(a[4] & resolve.FLAG_LATE)
        self.assertFalse(any(a[4] & resolve.FLAG_LATE for a in self.find("puella")))

    def test_lemma_self_analysis(self):
        iul = self.find("iulius")
        self.assertEqual(len(iul), 1)
        self.assertEqual(iul[0][2], F.POS["name"])
        self.assertEqual(iul[0][3], "I\u016blius")
        self.assertEqual(iul[0][4], 0)

    def test_report_counts(self):
        c = self.res["counts"]
        self.assertEqual(c["analyses"], len(self.an))
        self.assertEqual(c["distinct_keys"], len({a[0] for a in self.an}))
        self.assertEqual(sum(self.res["analyses_per_key"].values()), c["distinct_keys"])
        self.assertEqual(self.res["tags"]["unknown"], {})
        self.assertIn("noun-from-verb", self.res["tags"]["lossy"])


class ResolveGreekTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.out = tempfile.mkdtemp(prefix="vp-resolve-")
        kaikki.run("grc", "", cls.out, input_path=os.path.join(_paths.KAIKKI_FIXTURES, "grc.jsonl"))
        cls.res = resolve.run("grc", cls.out)
        cls.an = analyses(cls.out, "grc")

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.out)

    def find(self, word):
        k = vptext.greek_key(word)
        return [a for a in self.an if a[0] == k]

    def test_attic_and_ionic_tables(self):
        attic = self.find("\u03c7\u03ce\u03c1\u03b1\u03c2")
        self.assertEqual(len(attic), 1)
        u = F.unpack(attic[0][2])
        self.assertEqual(u["extra"], F.EXTRA["attic"])
        self.assertEqual(attic[0][4], resolve.FLAG_TABLE)
        ionic = self.find("\u03c7\u03ce\u03c1\u03b7\u03c2")
        self.assertEqual(len(ionic), 1)
        self.assertEqual(ionic[0][4], resolve.FLAG_TABLE | resolve.FLAG_DIALECT)
        self.assertEqual(F.unpack(ionic[0][2])["extra"], 0)
        # the head-line alternative Ionic spelling merges with the Ionic table cell: qualifiers are AND-ed
        chore = self.find("\u03c7\u03ce\u03c1\u03b7")
        self.assertTrue(all(a[4] & resolve.FLAG_DIALECT for a in chore))

    def test_contract_verb(self):
        unc = self.find("\u03c4\u03b9\u03bc\u03ac\u03b5\u03b9")
        self.assertEqual(len(unc), 1)
        self.assertTrue(unc[0][4] & resolve.FLAG_DIALECT)          # uncontracted twin of a contracted table
        u = F.unpack(unc[0][2])
        self.assertEqual(u["tense"], F.TENSE["present"])          # tense from the marker row
        con = self.find("\u03c4\u03b9\u03bc\u1fb7")
        self.assertEqual(len(con), 1)
        self.assertFalse(con[0][4] & resolve.FLAG_DIALECT)
        self.assertEqual(F.unpack(con[0][2])["extra"], F.EXTRA["contracted"])
        epic = self.find("\u03c4\u03af\u03bc\u03b1\u03bf\u03bd")
        self.assertTrue(epic[0][4] & resolve.FLAG_DIALECT)
        self.assertEqual(F.unpack(epic[0][2])["tense"], F.TENSE["imperfect"])
        aor = self.find("\u1f10\u03c4\u03af\u03bc\u03b7\u03c3\u03b1")
        self.assertEqual(F.unpack(aor[0][2])["tense"], F.TENSE["aorist"])
        self.assertFalse(aor[0][4] & resolve.FLAG_DIALECT)

    def test_formpage_folds_into_attic_cell(self):
        logou = self.find("\u03bb\u03cc\u03b3\u03bf\u03c5")
        self.assertEqual(len(logou), 1)
        self.assertEqual(logou[0][4], resolve.FLAG_TABLE | resolve.FLAG_FORMPAGE)
        self.assertEqual(F.unpack(logou[0][2])["extra"], F.EXTRA["attic"])
        epic = self.find("\u03bb\u03cc\u03b3\u03bf\u03b9\u03bf")
        self.assertEqual(epic[0][4], resolve.FLAG_FORMPAGE | resolve.FLAG_DIALECT)


class HelpersTest(unittest.TestCase):
    def test_merge_flags(self):
        m = resolve.merge_flags
        self.assertEqual(m(resolve.FLAG_TABLE | resolve.FLAG_DIALECT, resolve.FLAG_TABLE), resolve.FLAG_TABLE)
        self.assertEqual(m(resolve.FLAG_TABLE, resolve.FLAG_FORMPAGE), resolve.FLAG_TABLE | resolve.FLAG_FORMPAGE)
        self.assertEqual(m(resolve.FLAG_ALT | resolve.FLAG_LATE, resolve.FLAG_ALT), resolve.FLAG_ALT)

    def test_parse_marker(self):
        self.assertEqual(resolve.parse_marker("Ionic contracted present")[:3], ("present", frozenset(["Ionic"]), True))
        self.assertEqual(resolve.parse_marker("future perfect")[0], "future-perfect")
        self.assertEqual(resolve.parse_marker("Attic declension-2")[:3], (None, frozenset(["Attic"]), False))
        self.assertEqual(resolve.parse_marker("")[:3], (None, frozenset(), False))

    def test_external_sort_spills(self):
        d = tempfile.mkdtemp(prefix="vp-sort-")
        old = resolve.CHUNK_ROWS
        try:
            resolve.CHUNK_ROWS = 3
            s = resolve.Sorter(d)
            for i in range(10):
                s.add("k%d" % (9 - i), 1, 5, "d", resolve.FLAG_TABLE)
            s.add("k3", 1, 5, "d", resolve.FLAG_FORMPAGE)
            rows = list(s.merged())
            self.assertEqual([r[0][0] for r in rows], ["k%d" % i for i in range(10)])
            self.assertEqual(dict((r[0][0], r[1]) for r in rows)["k3"], resolve.FLAG_TABLE | resolve.FLAG_FORMPAGE)
            self.assertEqual(os.listdir(d), [])
        finally:
            resolve.CHUNK_ROWS = old
            shutil.rmtree(d)


if __name__ == "__main__":
    unittest.main()
