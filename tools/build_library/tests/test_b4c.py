"""B4c (LIB-4): the Greek supplement table (whole lemmas written by hand), the new override kinds (alt / drop, @gender,
@alt-table) and the English glosses of spanish.vpl. Hand-made fixtures only (tests/fixtures/kaikki/grc_b4c.jsonl,
es_b4c.jsonl and the rows written below); the project's curated tables are read as they are in the repository."""
import collections
import os
import shutil
import tempfile
import unittest

import _paths
import build
import esgloss
import features as F
import kaikki
import pack
import resolve
import supplement
import vpl_reader
import vptext

CURATED = os.path.join(_paths.ROOT, "data", "curated")
ATTIC = F.EXTRA["attic"] << 27
CONTR = F.EXTRA["contracted"] << 27
ALT = resolve.FLAG_ALT


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)


def copy_curated(dst, names):
    os.makedirs(dst, exist_ok=True)
    for n in names:
        shutil.copy(os.path.join(CURATED, n), os.path.join(dst, n))


def noun(case, number, extra=ATTIC):
    return F.pack(pos=F.POS["noun"], case=F.CASE[case], number=F.NUMBER[number]) | extra


def verb(tense, person, number, voice, extra=0):
    return F.pack(pos=F.POS["verb"], number=F.NUMBER[number], person=person, tense=F.TENSE[tense],
                  mood=F.MOOD["indicative"], voice=F.VOICE[voice]) | extra


class SupplementFormat(unittest.TestCase):
    ROW = ["διδασκαλεῖον", "διδασκαλεῖον", "noun", "neuter", "2", "2", "school", "escuela",
           "nominative singular=διδασκαλεῖον;genitive singular=διδασκαλείου;dative plural=διδασκαλείοις", "note"]

    def test_parse(self):
        e = supplement.parse_row(list(self.ROW), 3)
        self.assertEqual((e.key, e.pos, e.cls, e.contracted, e.tier), ("διδασκαλεῖον", "noun", 2, False, 2))
        self.assertEqual(e.cells[1], (["genitive", "singular"], "διδασκαλείου"))
        self.assertEqual(supplement.marker(e), "Attic declension-2")
        e = supplement.parse_row(["εὔνουσ", "εὔνους", "adj", "", "2 contracted", "2", "kind", "",
                                  "masculine feminine nominative singular=εὔνουσ"], 4)
        self.assertTrue(e.contracted)
        self.assertEqual(e.cells[0][1], "εὔνους")                       # final sigma written ς
        self.assertEqual(supplement.marker(e), "Attic contracted declension-2")

    def test_errors(self):
        bad = [(0, "λάθοσ"),                                              # key is not greek_key(head)
               (2, "nounish"), (3, "neutral"), (4, "x"), (5, "7"), (6, ""),
               (8, "nominative singlar=διδασκαλεῖον"),                    # unknown tag
               (8, "nominative singular διδασκαλεῖον"),                   # no "="
               (8, "")]
        for col, val in bad:
            row = list(self.ROW)
            row[col] = val
            with self.assertRaises(supplement.SupplementError, msg=(col, val)):
                supplement.parse_row(row, 9)

    def test_repository_table(self):
        """data/curated/lexicon_supplement_grc.tsv parses strictly; full paradigms; the generated nominative is the
        headword; principal parts read like Kaikki head lines."""
        es = supplement.load(CURATED, strict=True)
        heads = [e.head for e in es]
        self.assertEqual(heads[:3], ["διδασκαλεῖον", "λάθος", "εὔνους"])
        self.assertEqual(len(set(e.key for e in es)), len(es))
        by = {e.head: e for e in es}
        for h, n in (("διδασκαλεῖον", 15), ("λάθος", 16), ("εὔνους", 27)):
            self.assertEqual(len(by[h].cells), n, h)
            first = dict((" ".join(t), f) for t, f in reversed(by[h].cells))
            nom = "masculine feminine nominative singular" if by[h].pos == "adj" else "nominative singular"
            self.assertEqual(first[nom], h)
            for _t, f in by[h].cells:
                self.assertFalse(f.endswith("σ"), f)
        cells = dict((" ".join(t), f) for t, f in by["λάθος"].cells)
        self.assertEqual((cells["genitive singular"], cells["genitive plural"], cells["dative plural"]),
                         ("λάθους", "λαθῶν", "λάθεσιν"))       # the later dative plural row (movable ν) is the second
        cells = dict((" ".join(t), f) for t, f in by["εὔνους"].cells)
        self.assertEqual((cells["masculine feminine nominative plural"], cells["neuter nominative plural"]),
                         ("εὖνοι", "εὔνοα"))
        self.assertEqual(supplement.principal(by["λάθος"], []), "λάθος n (genitive λάθους); third declension")
        self.assertEqual(supplement.principal(by["εὔνους"], []),
                         "εὔνους (neuter εὔνουν); second declension, contracted")

    def test_repository_overrides(self):
        """Every `old` value of data/curated/lexicon_overrides_grc.tsv is one the packer knows."""
        import lexdata
        for row in lexdata.read_curated(os.path.join(CURATED, pack.GRC_OVERRIDES)):
            row = (row + [""] * 5)[:5]
            self.assertIsNotNone(pack.parse_old(row[3])[0], row)
        self.assertEqual(pack.parse_old("alt εὗρε εὗρεν"), ("alt", ["εὗρε", "εὗρεν"]))
        self.assertEqual(pack.parse_old(""), ("keep", []))
        self.assertEqual(pack.parse_old("never"), (None, []))


class GreekB4c(unittest.TestCase):
    """kaikki + resolve on grc_b4c.jsonl; pack with the repository's override and supplement tables."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp(prefix="vp-b4c-")
        cls.out = os.path.join(cls.tmp, "work")
        kaikki.run("grc", "", cls.out, input_path=os.path.join(_paths.KAIKKI_FIXTURES, "grc_b4c.jsonl"))
        resolve.run("grc", cls.out)
        cls.cur = os.path.join(cls.tmp, "curated")
        copy_curated(cls.cur, [pack.GRC_OVERRIDES, supplement.FILE])
        cls.c = collections.Counter()
        cls.lx = pack.build_lang("grc", cls.out, os.path.join(cls.tmp, "raw"), cls.c, curated=cls.cur)
        cls.ids = {l.key: i for i, l in enumerate(cls.lx.lemmas)}
        cls.data = pack.encode(cls.lx)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp)

    def cells(self, key):
        return dict(self.lx.lemmas[self.ids[vptext.greek_key(key)]].cells)

    def anal(self, form, lemma):
        lid = self.ids[vptext.greek_key(lemma)]
        return {a[2]: a[3] for a in self.lx.analyses if a[0] == vptext.greek_key(form) and a[1] == lid}

    def test_supplement_appended(self):
        self.assertEqual(self.c["supplement_lemmas"], 3)
        n_kaikki = 4
        for k, off in (("διδασκαλεῖον", 0), ("λάθοσ", 1), ("εὔνουσ", 2)):
            self.assertEqual(self.ids[k], n_kaikki + off)
        l = self.lx.lemmas[self.ids["λάθοσ"]]
        self.assertEqual((l.pos, l.cls, l.gender, l.tier, l.tier_source), (F.POS["noun"], 3, F.GENDER["neuter"], 3, 1))
        self.assertTrue(l.flags & pack.LF_TABLE)
        self.assertEqual(l.principal, "λάθος n (genitive λάθους); third declension")
        self.assertEqual(len(l.senses), 1)
        self.assertEqual(l.senses[0][2], "mistake error")
        self.assertEqual(l.senses[0][3], 1 << 6)                           # "(post-classical)" -> SENS Medieval bit
        lc = sorted(c for c in self.lx.candidates if c[1] == self.ids["λάθοσ"])
        self.assertEqual([(c[0], c[3]) for c in lc], [("error", 130), ("es:equivocación", 130), ("es:error", 130),
                                                      ("mistake", 130)])   # 100 + 60 - 30 (later sense)
        cells = self.cells("λάθος")
        self.assertEqual(cells[noun("dative", "plural")], "λάθεσι")       # first form of the repeated tag set
        self.assertEqual(cells[noun("genitive", "singular")], "λάθους")
        a = self.anal("λάθεσιν", "λάθος")
        self.assertEqual(a, {noun("dative", "plural"): resolve.FLAG_TABLE})
        adj = self.cells("εὔνους")
        mf = F.pack(pos=F.POS["adj"], case=1, number=2, gender=F.GENDER["mf"]) | ATTIC | CONTR
        self.assertEqual(adj[mf], "εὖνοι")
        comp = F.pack(pos=F.POS["adj"], degree=F.DEGREE["comparative"]) | ATTIC | CONTR
        self.assertEqual(adj[comp], "εὐνούστερος")

    def test_supplement_candidates(self):
        cand = [c for c in self.lx.candidates if c[1] == self.ids["διδασκαλεῖον"]]
        self.assertEqual(sorted(cand), [("es:escuela", self.ids["διδασκαλεῖον"], 0, 160, F.POS["noun"]),
                                        ("school", self.ids["διδασκαλεῖον"], 0, 160, F.POS["noun"])])
        kws = set(c[0] for c in self.lx.candidates if c[1] == self.ids["εὔνουσ"])
        self.assertTrue({"kind", "well-disposed", "friendly", "well", "disposed", "es:amable", "es:benévolo"} <= kws)

    def test_read_back(self):
        """The packed file opens, validates and gives the supplement lemma by form, by cell and by keyword."""
        r = vpl_reader.VplReader(data=self.data, check_sha=True)
        r.validate()
        lid = self.ids["διδασκαλεῖον"]
        hits = r.lookup("διδασκαλείου")
        self.assertEqual([(a["lemma"], a["feat"]) for a in hits], [(lid, noun("genitive", "singular"))])
        self.assertEqual(r.generate(lid, noun("dative", "singular")), "διδασκαλείῳ")
        lem = r.lemma(lid)
        self.assertEqual((lem["head"], lem["gloss_en"], lem["gloss_es"]), ("διδασκαλεῖον", "school", "escuela"))
        self.assertEqual(r.senses(lid)[0]["keywords"], "school")
        self.assertIn(lid, [c["lemma"] for c in r.reverse("school")])
        self.assertIn(lid, [c["lemma"] for c in r.reverse("es:escuela")])
        self.assertEqual(r.lemma(self.ids["εὔνουσ"])["principal"], "εὔνους (neuter εὔνουν); second declension, contracted")

    def test_drop(self):
        imp = verb("imperfect", 1, "singular", "middle", ATTIC)
        self.assertEqual(self.cells("οἴομαι")[imp], "ᾠόμην")
        self.assertEqual(self.anal("ᾤομην", "οἴομαι"), {})                 # the misaccented cell is gone
        self.assertEqual(self.anal("ᾠόμην", "οἴομαι").get(imp), resolve.FLAG_TABLE)
        self.assertFalse(self.anal("ᾤμην", "οἴομαι")[imp] & (ALT | resolve.FLAG_DIALECT))
        self.assertEqual(self.c["override_dropped_analyses"], 1)

    def test_alt_order(self):
        a1 = verb("aorist", 1, "singular", "active")
        a3 = verb("aorist", 3, "singular", "active")
        cells = self.cells("εὑρίσκω")
        self.assertEqual((cells[a1], cells[a3]), ("ηὗρον", "ηὗρε"))
        self.assertTrue(self.anal("εὗρον", "εὑρίσκω")[a1] & ALT)
        self.assertFalse(self.anal("ηὗρον", "εὑρίσκω")[a1] & ALT)
        self.assertTrue(self.anal("εὗρε", "εὑρίσκω")[a3] & ALT)
        self.assertTrue(self.anal("εὗρεν", "εὑρίσκω")[a3] & ALT)         # listed in `old`
        self.assertFalse(self.anal("ηὗρεν", "εὑρίσκω")[a3] & ALT)
        m1 = verb("aorist", 1, "singular", "middle")
        self.assertEqual(cells[m1], "ηὑρόμην")                            # a cell the fixture lacks is added
        for voice in ("middle", "passive"):
            p = verb("imperfect", 1, "singular", voice, ATTIC)
            self.assertEqual(self.cells("βούλομαι")[p], "ἐβουλόμην")
            self.assertTrue(self.anal("ἠβουλόμην", "βούλομαι")[p] & ALT)
            self.assertFalse(self.anal("ἐβουλόμην", "βούλομαι")[p] & ALT)
            self.assertFalse(self.anal("ἐβουλόμην", "βούλομαι")[p & ~ATTIC] & ALT)

    def test_alt_table_and_gender(self):
        l = self.lx.lemmas[self.ids["σκότοσ"]]
        self.assertEqual(l.gender, F.GENDER["masculine"])
        cells = self.cells("σκότος")
        self.assertEqual((cells[noun("genitive", "singular")], cells[noun("nominative", "plural")],
                          cells[noun("accusative", "singular")]), ("σκότου", "σκότοι", "σκότον"))
        self.assertTrue(self.anal("σκότους", "σκότος")[noun("genitive", "singular")] & ALT)
        self.assertTrue(self.anal("σκότη", "σκότος")[noun("nominative", "plural")] & ALT)
        self.assertFalse(self.anal("σκότου", "σκότος")[noun("genitive", "singular")] & ALT)
        # the neuter accusative is the headword: a lemma's own headword never carries bit3 (DESIGN 5)
        self.assertFalse(self.anal("σκότος", "σκότος")[noun("accusative", "singular")] & ALT)
        self.assertEqual((self.c["override_gender_rows"], self.c["override_alt_table_rows"]), (1, 1))

    def test_existing_key_skipped(self):
        cur = os.path.join(self.tmp, "cur2")
        write(os.path.join(cur, supplement.FILE),
              "# comment\nσκότοσ\tσκότος\tnoun\tneuter\t3\t3\tdarkness\toscuridad\tnominative singular=σκότος\t\n"
              "λάθοσ\tλάθος\tnoun\tneuter\t3\t3\tmistake\terror\tnominative singuler=λάθος\t\n")
        c = collections.Counter()
        lx = pack.build_lang("grc", self.out, os.path.join(self.tmp, "raw"), c, curated=cur)
        self.assertEqual(len(lx.lemmas), 4)
        self.assertEqual((c["supplement_skipped_existing"], c["supplement_rows_invalid"]), (1, 1))


class CleanHead(unittest.TestCase):
    def test_rule(self):
        for raw, want in (("((caelum", "caelum"), ("*dia", "dia"), ("ōh!", "ōh"), ("e.g.", "e.g."), ("lat.", "lat."),
                          ("-arch", "-arch"), ("δ᾽", "δ᾽"), ("ἄν", "ἄν"), (";", ""), ("·", ""), ("caelum", "caelum")):
            self.assertEqual(pack.clean_head(raw), want, raw)

    def test_pack(self):
        """A lemma record whose head carries leading punctuation is packed with the clean head (key unchanged)."""
        tmp = tempfile.mkdtemp(prefix="vp-head-")
        try:
            out = os.path.join(tmp, "work")
            write(os.path.join(out, "la", "lemmas.jsonl"),
                  '{"id": 0, "word": "((caelum", "key": "caelum", "head": "((caelum", "pos": "noun", "fpos": "noun"}\n'
                  '{"id": 1, "word": ";", "key": ";", "head": ";", "pos": "punct", "fpos": "punct"}\n')
            write(os.path.join(out, "la", "analyses.tsv"), "caelum\t0\t1\t((caelum\t1\n")
            for n in ("table_forms.tsv", "gloss.tsv", "senses.tsv", "tiers.tsv", "revx_en.tsv", "revx_es.tsv",
                      "whitaker.tsv"):
                write(os.path.join(out, "la", n), "")
            c = collections.Counter()
            lx = pack.build_lang("la", out, os.path.join(tmp, "raw"), c, curated=os.path.join(tmp, "cur"))
            self.assertEqual([(l.head, l.key) for l in lx.lemmas], [("caelum", "caelum"), ("", ";")])
            self.assertEqual((c["heads_cleaned"], c["heads_dropped"]), (2, 1))
        finally:
            shutil.rmtree(tmp)


class SpanishGlossEn(unittest.TestCase):
    EN_ROWS = ("door\tnoun\tportal\tpuerta\t\n"
               "door\tnoun\tportal\tportón\t\n"
               "gate\tnoun\t\tportón\t\n"
               "gate\tnoun\t\tpuerta\t\n"
               "doorway\tnoun\t\tpuerta\t\n"
               "come\tverb\t\tvenir\t\n"
               "red\tadj\t\trojo\t\n"
               "red\tnoun\t\trojo\t\n"
               "redness\tnoun\t\trojo\t\n"
               "ice cream\tnoun\t\thelado\t\n")

    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="vp-esgl-")
        self.out = os.path.join(self.tmp, "work")
        kaikki.run("es", "", self.out, input_path=os.path.join(_paths.KAIKKI_FIXTURES, "es_b4c.jsonl"))
        resolve.run("es", self.out)
        write(os.path.join(self.out, "en", "translations_es.tsv"), self.EN_ROWS)

    def tearDown(self):
        shutil.rmtree(self.tmp)

    def test_translations_en_written(self):
        with open(os.path.join(self.out, "es", "translations_en.tsv"), encoding="utf-8") as f:
            rows = [line.rstrip("\n").split("\t") for line in f]
        self.assertEqual([(r[0], r[3]) for r in rows], [("puerta", "door"), ("puerta", "gate")])

    def test_gloss_en(self):
        c = collections.Counter()
        lx = pack.build_lang("es", self.out, os.path.join(self.tmp, "raw"), c, curated=os.path.join(self.tmp, "cur"),
                             select=False)
        g = {l.key: l.gloss_en for l in lx.lemmas}
        # puerta: door 2 (first target) + 1 (first es.wikt translation) + 1 (listed there); gate 1 + 1; doorway 2:
        # gate wins the tie on its two English-edition rows (no UD files in this fixture)
        self.assertEqual(g["puerta"], "door, gate, doorway")
        self.assertEqual(g["venir"], "to come")
        self.assertEqual(g["rojo"], "red")          # the noun rows do not count for the adjective
        self.assertEqual(g["zarzo"], "")
        self.assertEqual((c["gloss_en_covered"], c["gloss_en_missing"], c["gloss_en_both"], c["gloss_en_en"]),
                         (3, 1, 1, 2))
        r = vpl_reader.VplReader(data=pack.encode(lx), check_sha=True)
        r.validate()
        ids = {l.key: i for i, l in enumerate(lx.lemmas)}
        self.assertEqual(r.lemma(ids["venir"])["gloss_en"], "to come")
        self.assertEqual(r.lemma(ids["zarzo"])["gloss_en"], "")

    def test_gloss_format(self):
        en = {"x": [("one", "verb", True), ("two", "verb", False), ("three", "verb", False), ("four", "verb", False)]}
        # one: first-listed 2; three: 1 + common in the UD treebank 1, more UD occurrences; two / four: 1, shorter first
        g, src = esgloss.gloss_for("verb", "verb", "x", en, {}, {}, {"three": 25})
        self.assertEqual((g, src), ("to three, one, two", "en"))
        g, _ = esgloss.gloss_for("noun", "noun", "y", {"y": [("a" * 40, "noun", True), ("b" * 30, "noun", False)]},
                                 {}, {})
        self.assertEqual(g, "a" * 40)                 # cut at the last ", " under 60 characters
        # "Son" / "son" compete -> son; the Spanish edition's first translation; "to " and "x/y" words
        es = {("h", "noun"): [esgloss._en_word("to child"), esgloss._en_word("son/daughter")]}
        self.assertEqual(es[("h", "noun")], ["child", ""])
        g, src = esgloss.gloss_for("noun", "noun", "h", {"h": [("Son", "noun", True), ("son", "noun", False)]},
                                   {("h", "noun"): ["son"]}, {})
        self.assertEqual((g, src), ("son", "both"))

    def test_stage_inputs(self):
        self.assertIn("esgloss.py", build.STAGE_CODE["pack"])
        self.assertIn("supplement.py", build.STAGE_CODE["pack"])
        ins = build.stage_inputs("pack", "es", os.path.join(self.tmp, "raw"), self.out)
        self.assertIn(os.path.join(self.out, "es", "translations_en.tsv"), ins)
        self.assertIn(os.path.join(self.out, "en", "translations_es.tsv"), ins)
        self.assertIn("translations_en.tsv", build.KAIKKI_OUTPUTS["es"])


if __name__ == "__main__":
    unittest.main()
