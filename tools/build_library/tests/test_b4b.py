"""B4b (LIB-3) fixes: Greek particle POS, final sigma, article cells, romanised cells, Epic tables without a marker,
form pages that do not clear bit4, Greek overrides and compounds, deponent flags, Whitaker-only analyses, macron
overrides at build time, the English/Spanish size cut, the Greek gloss_es check, --next builds. Hand-made fixtures
only (tests/fixtures/kaikki/grc_b4b.jsonl and the rows written below)."""
import collections
import contextlib
import io
import json
import os
import shutil
import tempfile
import unittest

import _paths
import build
import common
import features as F
import gloss
import grcfix
import kaikki
import make_vpl_spec
import morphcut
import pack
import resolve
import tagmap
import vptext
import whitaker_gen


def tsv(path):
    with open(path, encoding="utf-8") as f:
        return [line.rstrip("\n").split("\t") for line in f]


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)


class ParticlePos(unittest.TestCase):
    def test_templates(self):
        self.assertEqual(tagmap.pos_name("particle", "grc-particle"), "particle")
        self.assertEqual(tagmap.pos_name("noun", "grc-particle"), "particle")
        self.assertEqual(tagmap.pos_name("particle", "grc-part"), "particle")
        self.assertEqual(tagmap.pos_name("particle", "head", ""), "particle")
        self.assertEqual(tagmap.pos_name("verb", "grc-part-1&3"), "participle")
        self.assertEqual(tagmap.pos_name("verb", "grc-part-1&2"), "participle")
        self.assertEqual(tagmap.pos_name("verb", "grc-part form"), "participle")
        self.assertEqual(tagmap.pos_name("verb", "la-part"), "participle")


class GreekText(unittest.TestCase):
    def test_final_sigma(self):
        fs = grcfix.final_sigma
        self.assertEqual(fs("ἦσ"), "ἦς")
        self.assertEqual(fs("εἴησ, ἦσθα"), "εἴης, ἦσθα")
        self.assertEqual(fs("λόγοσ λόγοσ"), "λόγος λόγος")
        self.assertEqual(fs("-ᾱσ"), "-ᾱς")
        self.assertEqual(fs("ὅσ’"), "ὅσ’")          # elided: the medial letter stays
        self.assertEqual(fs("σοφόσ;"), "σοφός;")
        self.assertEqual(fs("ἐστί"), "ἐστί")
        self.assertEqual(vptext.greek_key(fs("ἦσ")), vptext.greek_key("ἦσ"))   # keys unaffected

    def test_article(self):
        sa = grcfix.strip_article
        self.assertEqual(sa("τῆς ἀνθρώπου"), ("ἀνθρώπου", True))
        self.assertEqual(sa("ὁ κῠ́ων"), ("κῠ́ων", True))
        self.assertEqual(sa("τᾱ̀ς κῠ́νᾰς"), ("κῠ́νᾰς", True))
        self.assertEqual(sa("τῇσῐν θᾰλᾰ́σσῃσῐ"), ("θᾰλᾰ́σσῃσῐ", True))
        self.assertEqual(sa("ὁ/ἡ κῠ́ων"), ("κῠ́ων", True))
        self.assertEqual(sa("γεγενημένοι ὦμεν"), ("γεγενημένοι ὦμεν", False))
        self.assertEqual(sa("ἦ λελυκώς"), ("ἦ λελυκώς", False))   # ἦ is not the article
        self.assertEqual(sa("Μᾰκᾰ́ρων νῆσοι"), ("Μᾰκᾰ́ρων νῆσοι", False))
        self.assertEqual(sa("τοῦ"), ("τοῦ", False))

    def test_latin_script_and_dialect_guess(self):
        self.assertTrue(grcfix.latin_script("taîs kŭsĭ́(n)"))
        self.assertFalse(grcfix.latin_script("τῇσῐν θᾰλᾰ́σσῃσῐ"))
        self.assertEqual(grcfix.dialect_guess([("ᾰ̓νθρώποιο", ["genitive", "singular"])]), "Epic")
        self.assertEqual(grcfix.dialect_guess([("θᾰλᾰ́σσῃσῐ", ["dative", "plural"])]), "Ionic")
        self.assertEqual(grcfix.dialect_guess([("θᾰλᾰ́τταις", ["dative", "plural"]),
                                               ("ᾰ̓νθρώπου", ["genitive", "singular"])]), "")

    def test_compound(self):
        c = grcfix.compound
        self.assertEqual(c("ἀπο", "ἔφῠγον"), ("ἀπέφῠγον", True))
        self.assertEqual(c("ἀπο", "ἐφῠ́γομεν"), ("ἀπεφῠ́γομεν", True))
        self.assertEqual(c("ἀπο", "φῠ́γε"), ("ἀπόφῠγε", True))         # recessive accent onto the prefix
        self.assertEqual(c("ἀπο", "φῠ́γω"), ("ἀποφῠ́γω", True))         # long ultima: stays
        self.assertEqual(c("ἀπο", "φῠ́γοι"), ("ἀποφῠ́γοι", True))
        self.assertEqual(c("ἀπο", "φῠγεῖν", False), ("ἀποφῠγεῖν", True))
        self.assertEqual(c("ἀπο", "ἕλκω"), ("ἀφέλκω", True))           # rough breathing aspirates
        self.assertEqual(c("περι", "ἔβαλον"), ("περιέβαλον", True))     # περι does not elide
        self.assertEqual(c("ἀπο", "πεφευγὼς ἦν"), ("πεφευγὼς ἦν", False))


class MergeFlags(unittest.TestCase):
    def test_silent_form_page_keeps_bit4(self):
        mf = resolve.merge_flags
        epic_table = resolve.FLAG_TABLE | resolve.FLAG_DIALECT
        silent_fp = resolve.FLAG_FORMPAGE | resolve.FLAG_NODIAL
        self.assertEqual(mf(epic_table, silent_fp), resolve.FLAG_TABLE | resolve.FLAG_FORMPAGE | resolve.FLAG_DIALECT)
        self.assertEqual(mf(silent_fp, epic_table), resolve.FLAG_TABLE | resolve.FLAG_FORMPAGE | resolve.FLAG_DIALECT)
        # an Attic table and an Ionic table: not dialectal (unchanged rule)
        self.assertEqual(mf(resolve.FLAG_TABLE, epic_table), resolve.FLAG_TABLE)
        # a form page that names a dialect still votes
        self.assertEqual(mf(epic_table, resolve.FLAG_FORMPAGE), resolve.FLAG_TABLE | resolve.FLAG_FORMPAGE)
        # two silent pages: the internal bit survives the merge, the writer drops it
        self.assertEqual(mf(silent_fp, silent_fp), silent_fp)


class GreekPipeline(unittest.TestCase):
    """kaikki + resolve on grc_b4b.jsonl, then pack.build_lang with an override table."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp(prefix="vp-b4b-")
        cls.out = os.path.join(cls.tmp, "work")
        cls.res = kaikki.run("grc", "", cls.out, input_path=os.path.join(_paths.KAIKKI_FIXTURES, "grc_b4b.jsonl"))
        resolve.run("grc", cls.out)
        cls.cur = os.path.join(cls.tmp, "curated")
        write(os.path.join(cls.cur, pack.GRC_OVERRIDES),
              "# key\ttags\tform\told\tnote\n"
              "δεῖ\tAttic active indicative present singular third-person\tδεῖ\tdialect\t\n"
              "δεῖ\tAttic active infinitive present\tδεῖν\tdialect\t\n"
              "δεῖ\tAttic active indicative imperfect singular third-person\tἔδει\tkeep\t\n"
              "ἀποφεύγω\t@from aorist\tἀπο+φεύγω\tkeep\t\n"
              "ὀρφανόσ\tAttic active infinitive present\tx\tkeep\tno such lemma\n")
        cls.c = collections.Counter()
        cls.lx = pack.build_lang("grc", cls.out, os.path.join(cls.tmp, "raw"), cls.c, curated=cls.cur)
        with open(os.path.join(cls.out, "grc", "lemmas.jsonl"), encoding="utf-8") as f:
            cls.recs = [json.loads(line) for line in f]
        cls.ids = {r["word"]: r["id"] for r in cls.recs}

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp)

    def cells(self, word):
        return dict(self.lx.lemmas[self.ids[word]].cells)

    def anal(self, key):
        return [a for a in self.lx.analyses if a[0] == key]

    def test_particles(self):
        for w in ("οὐ", "γε"):
            rec = self.recs[self.ids[w]]
            self.assertEqual(rec["fpos"], "particle", w)
            self.assertEqual(self.lx.lemmas[self.ids[w]].pos, F.POS["particle"])

    def test_table_cleanup(self):
        table = tsv(os.path.join(self.out, "grc", "table_forms.tsv"))
        kid = str(self.ids["κύων"])
        forms = [r[1] for r in table if r[0] == kid]
        self.assertIn("κῠ́ων", forms)
        self.assertIn("κῠνός", forms)               # article stripped and final sigma written ς
        self.assertNotIn("τῆς κῠνόσ", forms)
        self.assertFalse(any(grcfix.latin_script(f) for f in forms))   # romanised cell dropped
        c = self.res["counts"]
        self.assertEqual(c["grc_article_cells_stripped"], 3)
        self.assertEqual(c["grc_article_lemmas"], 1)
        self.assertEqual(c["grc_romanised_cells_dropped"], 1)
        self.assertEqual(c["grc_final_sigma_cells"], 2)
        for r in table:
            self.assertFalse(r[1].endswith("σ"), r)
        fp = tsv(os.path.join(self.out, "grc", "formpages.tsv"))
        self.assertEqual(fp[0][2], "εἴης")                                # form-page display
        self.assertEqual(fp[0][1], "εἴησ")                                # key unchanged

    def test_epic_table_without_marker(self):
        table = tsv(os.path.join(self.out, "grc", "table_forms.tsv"))
        pid = str(self.ids["ποταμός"])
        markers = sorted(set(r[5] for r in table if r[0] == pid))
        self.assertEqual(markers, ["Attic declension-2", "Epic declension-2"])
        self.assertEqual(self.res["counts"]["grc_dialect_tables_guessed"], 1)
        epic = [a for a in self.anal("ποταμοῖο") if a[1] == int(pid)]
        self.assertTrue(epic and all(a[3] & resolve.FLAG_DIALECT for a in epic))
        gen = F.pack(pos=F.POS["noun"], case=2, number=1)
        self.assertEqual(self.cells("ποταμός")[gen], "ποτᾰμοῖο")   # only the Epic table fills the plain cell
        self.assertEqual(self.cells("ποταμός")[gen | (F.EXTRA["attic"] << 27)], "ποτᾰμοῦ")

    def test_overrides(self):
        attic = F.EXTRA["attic"] << 27
        v3 = F.pack(pos=F.POS["verb"], number=1, person=3, tense=F.TENSE["present"], mood=1, voice=1) | attic
        inf = F.pack(pos=F.POS["verb"], tense=F.TENSE["present"], mood=F.MOOD["infinitive"], voice=1) | attic
        impf = F.pack(pos=F.POS["verb"], number=1, person=3, tense=F.TENSE["imperfect"], mood=1, voice=1) | attic
        cells = self.cells("δεῖ")
        self.assertEqual((cells[v3], cells[inf], cells[impf]), ("δεῖ", "δεῖν", "ἔδει"))
        lid = self.ids["δεῖ"]
        dee = [a for a in self.anal("δέει") if a[1] == lid and a[2] == v3]
        self.assertTrue(dee and dee[0][3] & resolve.FLAG_DIALECT)            # demoted
        dei = [a for a in self.anal("δεῖ") if a[1] == lid and a[2] == v3]
        self.assertTrue(dei and not dei[0][3] & resolve.FLAG_DIALECT)
        ede = [a for a in self.anal("ἔδεε") if a[1] == lid]
        self.assertTrue(ede and not ede[0][3] & resolve.FLAG_DIALECT)        # "keep"
        self.assertEqual(self.c["override_rows_unmatched"], 1)

    def test_compound(self):
        cells = self.cells("ἀποφεύγω")
        self.assertEqual(sorted(cells.values()), sorted(["ἀποφεύγω", "ἀπέφῠγον", "ἀπόφῠγε", "ἀποφῠγεῖν"]))
        lid = self.ids["ἀποφεύγω"]
        a = self.anal("ἀπέφυγον")
        self.assertEqual([x[1] for x in a], [lid])
        aor = F.pack(pos=F.POS["verb"], number=1, person=1, tense=F.TENSE["aorist"], mood=1, voice=1)
        self.assertEqual(a[0][2], aor)
        self.assertEqual(self.c["compound_cells"], 3)                 # the Epic aorist is not copied

    def test_deponent_flag(self):
        self.assertTrue(self.lx.lemmas[self.ids["ἔρχομαι"]].flags & pack.LF_DEPONENT)
        self.assertFalse(self.lx.lemmas[self.ids["φεύγω"]].flags & pack.LF_DEPONENT)
        self.assertEqual(self.c["deponent_flag_added"], 1)

    def test_analyses_sorted(self):
        keys = [a[0].encode("utf-8") for a in self.lx.analyses]
        self.assertEqual(keys, sorted(keys))
        self.assertEqual(len(self.lx.analyses), len(set((a[0], a[1], a[2], a[4]) for a in self.lx.analyses)))
        data = pack.encode(self.lx)
        import vpl_reader
        r = vpl_reader.VplReader(data=data, check_sha=True)
        r.validate()
        self.assertEqual(r.generate(self.ids["δεῖ"], F.pack(pos=2, number=1, person=3, tense=1, mood=1, voice=1)
                                    | (F.EXTRA["attic"] << 27)), "δεῖ")


class LatinDeponent(unittest.TestCase):
    def rows(self, first):
        return [["0", first, vptext.latin_key(first), "active first-person indicative present singular",
                 "conjugation", "conjugation-3"]]

    def test_signal(self):
        cells = pack.cell_rows("la", self.rows("sequor"), "verb", 0)
        self.assertTrue(pack.deponent_signal("la", "verb", "sequor", "sequor", cells))
        cells = pack.cell_rows("la", self.rows("amō"), "verb", 0)
        self.assertFalse(pack.deponent_signal("la", "verb", "amor", "amor", cells))   # a noun-like -or key, -ō cell
        self.assertFalse(pack.deponent_signal("la", "verb", "sequor", "sequor", []))  # no table: no claim
        self.assertFalse(pack.deponent_signal("la", "noun", "amor", "amor", cells))


class WhitakerOnly(unittest.TestCase):
    INFL = [["N", "1 1 NOM S C", "1", "1", "a", "X", "A"], ["N", "1 1 GEN S C", "2", "2", "ae", "X", "A"],
            ["N", "1 0 GEN P C", "2", "4", "arum", "X", "A"], ["N", "1 0 GEN P C", "2", "2", "um", "X", "C"],
            ["N", "1 1 GEN S C", "2", "2", "ai", "B", "C"], ["N", "1 1 GEN S C", "2", "3", "aes", "A", "C"],
            ["V", "1 1 PRES ACTIVE IND 3 P", "1", "3", "ant", "X", "A"],
            ["V", "0 0 PERF ACTIVE IND 3 P", "3", "5", "erunt", "X", "A"],
            ["V", "0 0 PERF ACTIVE IND 3 P", "3", "3", "ere", "X", "B"],
            ["V", "1 1 PRES PASSIVE IND 3 S", "2", "4", "atur", "X", "A"],
            ["V", "3 1 PRES ACTIVE IMP 2 S", "2", "0", "", "X", "A"]]

    def wrow(self, stems, pos, codes, ids, age="X"):
        return ["1"] + stems + [pos, codes, age, "X", "X", "A", "X", "meaning", "cite", ",".join(map(str, ids))]

    def test_generate(self):
        infl = {}
        for r in self.INFL:
            i = whitaker_gen.Inflection(r)
            if i.age in whitaker_gen.USE_AGES and i.freq in whitaker_gen.USE_FREQS and (i.pos != "V" or i.ending):
                infl.setdefault(i.pos, []).append(i)
        noun = lambda case, num: F.pack(pos=1, case=case, number=num)
        verb = lambda tense, voice, person, num: F.pack(pos=2, tense=tense, voice=voice, mood=1, person=person,
                                                        number=num)
        known = {("puella", 0), ("puellae", 0), ("puellarum", 0), ("amant", 1), ("amauerunt", 1), ("amatur", 1),
                 ("ambulant", 2), ("ambulauerunt", 2), ("xyz", 3)}
        cellset = {(0, noun(1, 1)), (0, noun(2, 1)), (0, noun(2, 2)), (1, verb(1, 1, 3, 2)), (1, verb(4, 1, 3, 2)),
                   (1, verb(1, 2, 3, 1)), (2, verb(1, 1, 3, 2)), (2, verb(4, 1, 3, 2)), (3, noun(1, 1))}
        rows = [self.wrow(["puell", "puell", "", ""], "N", "1 1 F T", [0]),
                self.wrow(["am", "am", "amav", "amat"], "V", "1 1 TRANS", [1]),
                self.wrow(["ambul", "ambul", "ambulav", "ambulat"], "V", "1 1 INTRANS", [2]),
                self.wrow(["mens", "mens", "", ""], "N", "1 1 F T", [3])]      # a wrong join: nothing known
        c = collections.Counter()
        got = whitaker_gen.generate(rows, infl, ["noun", "verb", "verb", "noun"], [True, True, True, True],
                                    lambda k, i: (k, i) in known, c, lambda i, f: (i, f) in cellset)
        flags_rare = whitaker_gen.FLAG_WHITAKER | whitaker_gen.FLAG_RARE
        self.assertEqual([(a[0], a[1], a[3], a[4]) for a in got],
                         [("amauere", 1, flags_rare, "amavere"), ("ambulauere", 2, flags_rare, "ambulavere"),
                          ("puellai", 0, flags_rare, "puellai"), ("puellum", 0, flags_rare, "puellum")])
        self.assertEqual(got[0][2], verb(4, 1, 3, 2))
        self.assertEqual(got[3][2], noun(2, 2))
        self.assertEqual(c["entry_rejected"], 1)               # mensa: 0 of 3 forms known
        # a key Kaikki knows for another lemma gets no Whitaker-only reading
        c3 = collections.Counter()
        got3 = whitaker_gen.generate(rows, infl, ["noun", "verb", "verb", "noun"], [True] * 4,
                                     lambda k, i: (k, i) in known, c3, lambda i, f: (i, f) in cellset,
                                     lambda k: k == "puellum")
        self.assertNotIn("puellum", [a[0] for a in got3])
        self.assertEqual(c3["key_known_for_another_lemma"], 1)
        self.assertEqual(c["feature_not_in_table"], 1)          # ambulatur: the table has no passive
        # no table at all: everything Whitaker accepts that Kaikki lacks, no guard
        c2 = collections.Counter()
        got2 = whitaker_gen.generate(rows[3:], infl, ["noun"] * 4, [False] * 4, lambda k, i: False, c2,
                                     lambda i, f: False)
        self.assertEqual(sorted(a[4] for a in got2), ["mensa", "mensae", "mensai", "mensarum", "mensum"])
        self.assertEqual(c2["lemmas_without_table"], 1)

    def test_deponent_and_impersonal(self):
        infl = {"V": [whitaker_gen.Inflection(r) for r in (
            ["V", "1 1 PRES ACTIVE IND 3 S", "2", "2", "at", "X", "A"],
            ["V", "1 1 PRES PASSIVE IND 3 S", "2", "4", "atur", "X", "A"],
            ["V", "1 1 PRES PASSIVE IND 1 S", "1", "2", "or", "X", "A"])]}
        dep = {"stems": ["hort", "hort", "", "hortat"], "pos": "V", "codes": ["1", "1", "DEP"]}
        forms = whitaker_gen.entry_forms(dep, infl)
        self.assertEqual([(f, t[0]) for f, t, _ in forms],
                         [("hortatur", ["present", "active", "indicative", "third-person", "singular"]),
                          ("hortor", ["present", "active", "indicative", "first-person", "singular"])])
        imp = {"stems": ["plu", "plu", "", ""], "pos": "V", "codes": ["1", "1", "IMPERS"]}
        self.assertEqual([f for f, _, _ in whitaker_gen.entry_forms(imp, infl)], ["pluat", "pluatur"])


class MacronOverride(unittest.TestCase):
    def test_apply(self):
        ma = pack.macron_apply
        self.assertEqual(ma("narrā", "narr", "nārr"), "nārrā")
        self.assertEqual(ma("Narrā", "narr", "nārr"), "Nārrā")
        self.assertEqual(ma("ēnarrā", "narr", "nārr"), "ēnarrā")
        self.assertEqual(ma("narrō, narrāre, narrāvī", "narr", "nārr", words=True), "nārrō, nārrāre, nārrāvī")

    def test_load(self):
        tmp = tempfile.mkdtemp(prefix="vp-mac-")
        try:
            write(os.path.join(tmp, pack.LA_MACRONS), "# key\tstem_from\tstem_to\tnote\nnarro\tnarr\tnārr\t\n")
            lemmas = [pack.Lemma(head="narrō", key="narro"), pack.Lemma(head="nārrō", key="narro"),
                      pack.Lemma(head="amō", key="amo")]
            c = collections.Counter()
            self.assertEqual(pack.load_macron_overrides(tmp, lemmas, c), {0: ("narr", "nārr")})
        finally:
            shutil.rmtree(tmp)


class SizeCut(unittest.TestCase):
    CONLLU = ("# sent_id = 1\n1\tDogs\tdog\tNOUN\t_\t_\t2\tnsubj\t_\t_\n2\tran\trun\tVERB\t_\t_\t0\troot\t_\t_\n"
              "3-4\tdon't\t_\t_\t_\t_\t_\t_\t_\t_\n\n")

    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="vp-cut-")
        self.raw = os.path.join(self.tmp, "raw")
        self.out = os.path.join(self.tmp, "work")
        self.cur = os.path.join(self.tmp, "curated")
        write(os.path.join(self.raw, "ud", "en_ewt-ud-train.conllu"), self.CONLLU)
        write(os.path.join(self.raw, "ud", "es_gsd-ud-train.conllu"), "1\tperros\tperro\tNOUN\t_\t_\t0\troot\t_\t_\n")
        write(os.path.join(self.cur, "phrasebook_en_la.tsv"), "# pattern\tlatin\nhello there\tsalvē\n")
        write(os.path.join(self.out, "en", "translations_la.tsv"), "water\tnoun\t\taqua\t\nwater\tnoun\t\tlympha\t\n")
        write(os.path.join(self.out, "en", "translations_grc.tsv"), "")
        write(os.path.join(self.out, "en", "translations_es.tsv"), "")
        recs = [("dog", 1), ("run", 3), ("water", 0), ("hello", 0), ("zebu", 2), ("quokka", 1), ("ran", 0)]
        lem = "".join(json.dumps({"id": i, "word": k, "key": k, "head": k, "pos": "noun", "fpos": "noun", "ns": ns})
                      + "\n" for i, (k, ns) in enumerate(recs))
        write(os.path.join(self.out, "en", "lemmas.jsonl"), lem)
        anal = sorted([("dog", 0, 1, "dog"), ("dogs", 0, 1, "Dogs"), ("run", 1, 2, "run"), ("ran", 1, 2, "ran"),
                       ("ran", 6, 1, "ran"), ("water", 2, 1, "water"), ("hello", 3, 9, "hello"),
                       ("zebu", 4, 1, "zebu"), ("quokka", 5, 1, "quokka")])
        write(os.path.join(self.out, "en", "analyses.tsv"),
              "".join("%s\t%d\t%d\t%s\t1\n" % a for a in anal))

    def tearDown(self):
        shutil.rmtree(self.tmp)

    def test_select(self):
        records = [(0, "dog", 1), (1, "run", 3), (2, "water", 0), (3, "hello", 0), (4, "zebu", 2), (5, "quokka", 1),
                   (6, "ran", 0)]
        forms = {0: {"dog", "dogs"}, 1: {"run", "ran"}, 6: {"ran"}}
        c = collections.Counter()
        keep = morphcut.select(records, forms, self.out, self.raw, self.cur, "en", c, top_n=2)
        # UD: dog (form Dogs, lemma dog), run (lemma run), ran (form ran); proxy top 2: run (3), water (0 + 2
        # translations) and zebu (2) tie at 2 -> lower id water; curated: hello
        self.assertEqual(sorted(keep), [0, 1, 2, 3, 6])
        self.assertEqual((c["kept_ud"], c["kept_proxy"], c["kept_curated"]), (3, 1, 1))

    def test_pack_display_is_key(self):
        c = collections.Counter()
        old, morphcut.TOP_N = morphcut.TOP_N, 2
        try:
            lx = pack.build_lang("en", self.out, self.raw, c, curated=self.cur)
        finally:
            morphcut.TOP_N = old
        self.assertTrue(lx.morphology_only)
        self.assertEqual([l.key for l in lx.lemmas], ["dog", "run", "water", "hello", "ran"])
        self.assertTrue(all(a[0] == a[4] for a in lx.analyses))
        self.assertIn(("dogs", 0, 1, 1, "dogs"), lx.analyses)
        self.assertEqual(c["lemmas_not_selected"], 2)    # zebu, quokka
        self.assertEqual(c["analyses_not_selected"], 2)


class GreekGlossEs(unittest.TestCase):
    """An es.wiktionary gloss of another sense is replaced by the EN->ES pivot of sense 0; a matching one stays."""

    def test_recheck(self):
        tmp = tempfile.mkdtemp(prefix="vp-gl-")
        try:
            out = os.path.join(tmp, "work")
            recs = [{"id": 0, "word": "λόγος", "key": "λόγοσ", "head": "λόγος", "pos": "noun", "fpos": "noun",
                     "kind": "lemma", "has_table": True, "senses": [{"g": "that which is said: word, speech"},
                                                                    {"g": "computation, reckoning"}]},
                    {"id": 1, "word": "ὕδωρ", "key": "ὕδωρ", "head": "ὕδωρ", "pos": "noun", "fpos": "noun",
                     "kind": "lemma", "has_table": True, "senses": [{"g": "water"}]}]
            write(os.path.join(out, "grc", "lemmas.jsonl"), "".join(json.dumps(r, ensure_ascii=False) + "\n"
                                                                     for r in recs))
            write(os.path.join(out, "es", "latin_glosses.tsv"),
                  "λόγος\tnoun\tCálculo, cómputo.\tgrc\t1\t\nλόγος\tnoun\tDiscurso.\tgrc\t2\t\n"
                  "ὕδωρ\tnoun\tAgua.\tgrc\t1\t\n")
            write(os.path.join(out, "en", "translations_es.tsv"),
                  "word\tnoun\t\tpalabra\t\nspeech\tnoun\t\tdiscurso\t\nwater\tnoun\t\tagua\t\n"
                  "speech\tnoun\t\talocución\t\nspeech\tnoun\t\talocución\t\n"
                  "computation\tnoun\t\tcómputo\t\n")
            res = gloss.run("grc", out, os.path.join(tmp, "curated"))
            rows = {r[0]: r for r in tsv(os.path.join(out, "grc", "gloss.tsv"))}
            # "alocución" is the more frequent translation of "speech"; es.wiktionary's own "Discurso." wins
            self.assertEqual(rows["0"][3:5], ["palabra, discurso", "pivot"])
            self.assertEqual(rows["1"][3:5], ["agua", "eswikt"])
            self.assertEqual(res["counts"]["gloss_es_eswikt_replaced"], 1)
            self.assertEqual(res["counts"]["gloss_es_eswikt_kept"], 1)
        finally:
            shutil.rmtree(tmp)


class CuratedTier2(unittest.TestCase):
    def test_tier2_rows_and_sheet(self):
        import lexdata
        import tiers
        tmp = tempfile.mkdtemp(prefix="vp-t2-")
        try:
            write(os.path.join(tmp, "tiers_la.tsv"), "# key\thead\tpos\ttier\tsource\tnote\n"
                  "aqua\taqua\tnoun\t1\tteacher\t\ncanis\tcanis\tnoun\t2\tteacher\t\n")
            recs = [("aqua", "noun", True), ("canis", "noun", True), ("lupus", "noun", True), ("sol", "noun", True)]
            infos, by_key = [], {}
            for i, (k, p, t) in enumerate(recs):
                infos.append(lexdata.LemmaInfo({"id": i, "key": k, "head": k, "word": k, "fpos": p, "kind": "lemma",
                                                "has_table": t}, True))
                by_key.setdefault(k, []).append(i)
            sig = {"whit": {2: "B", 3: "D"}, "whit_t2": set(), "dcc": {}}
            reasons = {}
            got, missing = tiers.assign("la", infos, by_key, sig, lambda i: True, tmp, reasons)
            self.assertEqual((got[0], got[1], got[2], got[3]), ((1, 2), (2, 2), (3, 0), (3, 0)))
            self.assertEqual(reasons, {0: "teacher", 1: "teacher"})
            c = collections.Counter()
            path = os.path.join(tmp, "sheet.csv")
            tiers.write_teacher_sheet(path, "la", infos, got, reasons, sig, {0: ("water", "agua")}, c)
            with open(path, encoding="utf-8") as f:
                lines = f.read().splitlines()
            self.assertEqual(lines[0], "key,head,pos,tier,source,dcc_rank,whitaker_code,gloss_en,gloss_es,lemma_id")
            self.assertEqual(lines[1:], ["aqua,aqua,noun,1,teacher,,,water,agua,0", "canis,canis,noun,2,teacher,,,,,1",
                                         "lupus,lupus,noun,3,candidate,,B,,,2"])
        finally:
            shutil.rmtree(tmp)


class NextBuild(unittest.TestCase):
    """--next copies what a partial build needs into <out>/next and never writes the live .vpl."""

    def test_next_and_out_vpl(self):
        tmp = tempfile.mkdtemp(prefix="vp-next-")
        try:
            _, base, _ = make_vpl_spec.run_pipeline(make_vpl_spec.SPEC, tmp)
            raw = os.path.join(make_vpl_spec.SPEC, "raw")
            with open(os.path.join(base, "latin.vpl"), "rb") as f:
                live = f.read()
            err = io.StringIO()
            common.QUIET = False
            try:
                with contextlib.redirect_stderr(err):
                    rc = build.main(["--raw", raw, "--out", base, "--next", "--lang", "la", "--stage", "pack"])
            finally:
                common.QUIET = True
                build.VPL_DIR = None
            self.assertEqual(rc, 0)
            self.assertIn("--next: copied", err.getvalue())
            nxt = os.path.join(base, "next")
            self.assertTrue(os.path.exists(os.path.join(nxt, "la", "lemmas.jsonl")))
            self.assertTrue(os.path.exists(os.path.join(nxt, "latin.vpl")))
            self.assertTrue(os.path.exists(os.path.join(nxt, "la", "whitaker_only.tsv")))
            with open(os.path.join(base, "latin.vpl"), "rb") as f:
                self.assertEqual(f.read(), live)
            vdir = os.path.join(tmp, "vpl")
            try:
                rc = build.main(["--raw", raw, "--out", nxt, "--lang", "la", "--stage", "pack", "--force",
                                 "--out-vpl", vdir])
            finally:
                build.VPL_DIR = None
            self.assertEqual(rc, 0)
            with open(os.path.join(vdir, "latin.vpl"), "rb") as f, open(os.path.join(nxt, "latin.vpl"), "rb") as g:
                self.assertEqual(f.read(), g.read())
        finally:
            shutil.rmtree(tmp)


if __name__ == "__main__":
    unittest.main()
