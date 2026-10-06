"""B4 stages: Whitaker / Lewis & Short / LSJ / DCC import, glosses and keywords, reverse-index scores, tiers, the
.vpl packer (against an independent reader, B1's reference bytes and itself) and the spot list on the real data."""
import hashlib
import os
import shutil
import tempfile
import unittest

import _paths
import compare_spec
import features as F
import gloss
import import_aux
import lexdata
import make_vpl_spec
import pack
import tiers
import vpl_reader

WORK = os.path.join(_paths.ROOT, "data", "work")


def wline(stems, pos, codes, letters, meaning):
    """A DICTLINE.GEN line typed field by field: 4 stems of 19 columns, POS in 7, codes in 17, 5 letters."""
    return "".join(s.ljust(19) for s in stems) + pos.ljust(7) + codes.ljust(17) + letters + " " + meaning + "\r\n"


class WhitakerParsing(unittest.TestCase):
    LINES = [
        wline(["aqu", "aqu", "", ""], "N", "1 1 F T", "X X X A O", "water; sea, lake;"),
        wline(["am", "am", "amav", "amat"], "V", "1 1 X", "X X X A O", "love, like;"),
        wline(["vid", "vid", "vid", "vis"], "V", "2 1 X", "X X X A X", "see, look at;"),
        wline(["rex", "reg", "", ""], "N", "3 1 M P", "X X X A X", "king;"),
        wline(["sanct", "sanct", "sancti", "sanctissi"], "ADJ", "1 1 X", "E E X B E", "holy;"),
    ]

    def test_columns(self):
        e = import_aux.parse_dictline(self.LINES[0])
        self.assertEqual(e["stems"], ["aqu", "aqu", "", ""])
        self.assertEqual((e["pos"], e["codes"]), ("N", ["1", "1", "F", "T"]))
        self.assertEqual((e["age"], e["area"], e["geo"], e["freq"], e["source"]), ("X", "X", "X", "A", "O"))
        self.assertEqual(e["meaning"], "water; sea, lake;")
        e = import_aux.parse_dictline(self.LINES[4])
        self.assertEqual((e["age"], e["area"], e["freq"], e["source"]), ("E", "E", "B", "E"))
        self.assertEqual(e["stems"][3], "sanctissi")
        self.assertIsNone(import_aux.parse_dictline("   \r\n"))

    def test_zzz_is_empty(self):
        e = import_aux.parse_dictline(wline(["am", "am", "amass", "zzz"], "V", "8 1 X", "B X X E O", "love;"))
        self.assertEqual(e["stems"], ["am", "am", "amass", ""])

    def test_citation_forms(self):
        cites = [import_aux.citation_forms(import_aux.parse_dictline(l))[0] for l in self.LINES]
        self.assertEqual(cites, ["aqua", "amo", "video", "rex", "sanctus"])

    def test_inflects(self):
        self.assertEqual(import_aux.parse_inflect("N 1 1 NOM S C  1 1 a   X A"),
                         ("N", "1 1 NOM S C", "1", "1", "a", "X", "A"))
        self.assertEqual(import_aux.parse_inflect("V 3 1 PRES ACTIVE  IMP 2 S  1 0       X A  -- dic"),
                         ("V", "3 1 PRES ACTIVE IMP 2 S", "1", "0", "", "X", "A"))
        self.assertIsNone(import_aux.parse_inflect("-- a comment"))


LS_SAMPLE = """<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE TEI.2 PUBLIC "-//TEI P4//DTD Main DTD Driver File//EN" "http://example.invalid/tei2.dtd" [
<!ENTITY % PersDict PUBLIC "-//Perseus P4//DTD Perseus Dictionaries//EN" "http://example.invalid/PersDict.dtd">
%PersDict;
]>
<TEI.2><text><body>
<entryFree id="a1" type="main" key="puella"><orth extent="full" lang="la">pŭella</orth>, <itype>ae</itype>, <gen>f.</gen> <sense level="1"><hi rend="ital">a girl</hi> <usg>poet.</usg> <trans><tr>a sweetheart,</tr></trans></sense></entryFree>
<entryFree id="a2" type="main" key="sero4"><orth extent="full" lang="la">sērō^</orth>, <pos>adv.</pos> <sense level="1"><hi rend="ital">late</hi></sense></entryFree>
<entryFree id="a3" type="hapax" key="abduco"><orth extent="full" lang="la">ab-dūco</orth>, <itype>xi, ctum, 3</itype>, <pos>v. a.</pos> <usg>Trop.</usg></entryFree>
</body></text></TEI.2>
"""
LSJ_SAMPLE = """<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE TEI.2 PUBLIC "-//TEI P4//DTD Main DTD Driver File//EN" "http://example.invalid/tei2.dtd" [
<!ENTITY % PersDict PUBLIC "-//Perseus P4//DTD Perseus Dictionaries//EN" "http://example.invalid/PersDict.dtd" >
%PersDict;]>
<TEI.2><text><body><div0><div1 type="alphabetic letter" n="*a">
<entryFree id="b1" key="a)/nqrwpos" type="main"><orth extent="full" lang="greek">a)/nqrwpos</orth>, <gen lang="greek">o(</gen>, <sense n="A" level="1"><tr>man,</tr> generic</sense></entryFree>
<entryFree id="b2" key="lo/gos" type="main"><orth extent="full" lang="greek">lo/gos</orth>, <gen lang="greek">o(</gen>, <sense level="1"><tr>word</tr>; <tr>speech</tr></sense></entryFree>
<entryFree id="b3" key="a)/gw1" type="main"><orth extent="full" lang="greek">a)/gw</orth>, <sense level="1"><tr>lead</tr></sense></entryFree>
</div1></div0></body></text></TEI.2>
"""


class PerseusParsing(unittest.TestCase):
    def _entries(self, text, fn):
        fd, path = tempfile.mkstemp(suffix=".xml")
        with os.fdopen(fd, "w", encoding="utf-8") as f:
            f.write(text)
        try:
            return [fn(el) for el in import_aux.iter_entries(path)]
        finally:
            os.unlink(path)

    def test_lewis_short(self):
        es = self._entries(LS_SAMPLE, import_aux.ls_entry)
        self.assertEqual([e["key"] for e in es], ["puella", "sero", "abduco"])
        self.assertEqual(es[0]["orth"], "pŭella")
        self.assertEqual((es[0]["itype"], es[0]["gen"], es[0]["tr"], es[0]["def"], es[0]["usg"]),
                         ("ae", "f.", ["a sweetheart"], "a girl", ["poet"]))
        self.assertEqual((es[1]["hom"], es[1]["orth"], es[1]["pos"]), ("4", "sērō", "adv."))
        self.assertEqual(import_aux.ls_pos(es[1]["pos"]), "adv")
        self.assertEqual((es[2]["orth"], es[2]["type"], es[2]["usg"]), ("abdūco", "hapax", ["Trop"]))
        self.assertEqual(import_aux.ls_pos(es[2]["pos"]), "verb")

    def test_lsj(self):
        es = self._entries(LSJ_SAMPLE, import_aux.lsj_entry)
        self.assertEqual([e["orth"] for e in es], ["ἄνθρωπος",
                                                    "λόγος", "ἄγω"])
        self.assertEqual(es[0]["key"], "ἄνθρωποσ")  # greek_key: final sigma
        self.assertEqual(es[0]["bare"], "ανθρωποσ")
        self.assertEqual(es[0]["gen"], "ὁ")
        self.assertEqual(es[1]["tr"], ["word", "speech"])
        self.assertEqual((es[2]["hom"], es[2]["beta"]), ("1", "a)/gw1"))

    def test_dcc_headwords(self):
        self.assertEqual(import_aux.dcc_head_tokens("abeō -īre -iī -itum")[0], "abeō")
        self.assertEqual(import_aux.dcc_head_tokens("εἰμί, ἔσομαι, "
                                                    "impf. ἦν"),
                         ["εἰμί", "ἔσομαι", "ἦν"])
        self.assertEqual(import_aux.dcc_head_tokens("deinde/dein"), ["deinde", "dein"])
        self.assertEqual(import_aux.dcc_head_tokens("εἴκοσι(ν)"),
                         ["εἴκοσι"])
        self.assertEqual(import_aux.dcc_key_variants("que", "la"), ["que", "-que"])


class GlossText(unittest.TestCase):
    def setUp(self):
        self.stop = gloss.load_stopwords("en")
        forms = {"girls": "girl", "loves": "love", "ran": "run"}
        self.lem = lambda w: forms.get(w, w)

    def test_one_line(self):
        self.assertEqual(gloss.one_line("water"), "water")
        self.assertEqual(gloss.one_line("(transitive) to love"), "to love")
        self.assertEqual(gloss.one_line("a water-bucket or pail (especially one for fires), a firebucket"),
                         "a water-bucket or pail, a firebucket")
        long = "to be pleased by or with someone for a particular reason; to derive pleasure from"
        self.assertEqual(gloss.one_line(long), "to be pleased by or with someone for a particular reason")
        g = gloss.one_line("out of favor or kindness, without recompense or compensation, gratuitously, freely")
        self.assertLessEqual(len(g), 60)
        self.assertEqual(g, "out of favor or kindness")
        g = gloss.one_line("x" * 30 + " " + "y" * 40)
        self.assertTrue(g.endswith("…"))

    def test_keywords(self):
        self.assertEqual(gloss.extract_keywords("to love", self.lem, self.stop)[:2], (["love"], []))
        self.assertEqual(gloss.extract_keywords("to be fond of, like, admire", self.lem, self.stop)[:2],
                         (["fond", "like", "admire"], []))
        self.assertEqual(gloss.extract_keywords("girls, young maidens", self.lem, self.stop)[:2],
                         (["girl", "young"], ["maidens"]))
        self.assertEqual(gloss.extract_keywords("to be", self.lem, self.stop)[:2], (["be"], []))
        h, o, caps = gloss.extract_keywords("a water-bucket (for fires) or pail", self.lem, self.stop)
        self.assertEqual((h, o), (["water-bucket"], ["water", "bucket", "pail"]))
        h, o, caps = gloss.extract_keywords("Rome", self.lem, self.stop)
        self.assertEqual((h, caps), (["rome"], {"rome"}))
        es_stop = gloss.load_stopwords("es")
        self.assertEqual(gloss.extract_keywords("no querer", lambda w: w, es_stop)[:2], ([], ["querer"]))

    def test_sense_tags(self):
        b = gloss.tag_bit
        self.assertEqual(gloss.sense_tags({"tags": ["transitive", "figuratively"]}),
                         b("transitive") | b("figurative"))
        self.assertEqual(gloss.sense_tags({"tags": ["Medieval-Latin", "with-dative"]}), b("medieval") | b("with-dat"))
        self.assertEqual(gloss.sense_tags({"raw_tags": ["New Latin"], "q": "+ abl."}), b("newlatin") | b("with-abl"))
        self.assertEqual(gloss.sense_tags({"tags": ["obsolete", "poetic", "impersonal"]}),
                         b("archaic") | b("poetic") | b("impersonal"))
        self.assertEqual(gloss.frame_bits("acc;acc+inf"), b("transitive") | b("with-acc") | b("with-inf"))
        self.assertEqual(gloss.frame_bits("impers:dat+inf"), b("impersonal") | b("with-dat") | b("with-inf"))
        self.assertEqual(gloss.frame_bits("intr;prep:cum+abl"), b("intransitive"))


class RevxScoring(unittest.TestCase):
    """DESIGN 5.2 by hand: 1.0 exact, 0.6 head, 0.35 other, +0.15 Whitaker A/B, +0.1 DCC, +0.2 tier 1,
    -0.3 rare/archaic/poetic/Medieval/New Latin sense, -0.2 proper name with a lower-case keyword."""

    def test_arithmetic(self):
        sp = gloss.score_parts
        # aqua "water": exact + head + Whitaker A + DCC = 1.0+0.6+0.15+0.1 = 1.85 -> 185
        self.assertEqual(sp(100, 60, 0, False, 15 + 10, False), 185)
        # amo "love": + tier 1 -> 2.05 -> 205
        self.assertEqual(sp(100, 60, 0, False, 15 + 10 + 20, False), 205)
        # puella "sweetheart" (poetic second sense): 0.35 + 0.25 - 0.3 = 0.30 -> 30
        self.assertEqual(sp(0, 35, -30, False, 25, False), 30)
        # Roma "rome" from a lower-case gloss word: 0.6 + 0.15 - 0.2 = 0.55 -> 55; capitalised keyword: 75
        self.assertEqual(sp(0, 60, 0, False, 15, True), 55)
        self.assertEqual(sp(0, 60, 0, True, 15, True), 75)
        # clamps: never below 0, never above 255
        self.assertEqual(sp(0, 35, -30, False, 0, True), 0)
        self.assertEqual(sp(300, 60, 0, False, 45, False), 255)

    def test_best_sense_and_order(self):
        c = gloss.Cands()
        c.add("love", 7, 0, comp=60)
        c.add("love", 7, 1, exact=100, comp=35)
        c.add("love", 3, 0, comp=60)
        c.add("love", 9, 0, exact=100, comp=35)
        c.add("ama", 3, 2, comp=35, pen=-30)
        rows = gloss.finish_cands(c, {3: 20}, set(), lambda i: 2, lambda i, s: 0)
        # keyword order bytewise; inside: score desc, lemma asc
        self.assertEqual(rows, [("ama", 3, 2, 25, 2), ("love", 7, 1, 135, 2), ("love", 9, 0, 135, 2),
                                ("love", 3, 0, 80, 2)])

    def test_fixture_scores(self):
        """The hand-made fixture (tests/fixtures/vpl_spec) through every stage: the scores computed by hand."""
        tmp = tempfile.mkdtemp(prefix="b4-")
        try:
            data, out, _ = make_vpl_spec.run_pipeline(make_vpl_spec.SPEC, tmp)
            r = vpl_reader.VplReader(data=data)
            got = {r.keyword(i): [(c["lemma"], c["sense"], c["score"]) for c in r.reverse(r.keyword(i))]
                   for i in range(r.n_kw)}
        finally:
            shutil.rmtree(tmp)
        self.assertEqual(got["water"], [(0, 0, 185)])        # 1.0 + 0.6 + 0.15 + 0.1
        self.assertEqual(got["love"], [(1, 0, 205)])         # 1.0 + 0.6 + 0.15 + 0.1 + 0.2
        self.assertEqual(got["girl"], [(2, 0, 185)])
        self.assertEqual(got["maiden"], [(2, 0, 85)])        # 0.6 + 0.25
        self.assertEqual(got["fond"], [(1, 1, 80)])          # 0.35 + 0.45 (later sense)
        self.assertEqual(got["sweetheart"], [(2, 1, 30)])    # 0.35 + 0.25 - 0.3 (poetic)
        self.assertEqual(got["rome"], [(3, 0, 175)])         # 1.0 + 0.6 + 0.15, "Rome" capitalised: no -0.2
        self.assertEqual(got["es:agua"], [(0, 0, 185)])      # es.wiktionary translation + gloss head + 0.25
        self.assertEqual(got["es:niña"], [(2, 0, 185)])  # curated gloss = exact + head


class TierRules(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp(prefix="tiers-")
        with open(os.path.join(self.dir, "tiers_la.tsv"), "w", encoding="utf-8") as f:
            f.write("# key\thead\tpos\ttier\tsource\tnote\nvolo\tvolō\tverb\t1\tteacher\t\n"
                    "et\tet\tconj\t1\tderived\t\nnusquam\tnusquam\tadv\t1\tderived\t\n")
        recs = [("uolo", "volō", "verb", True, "lemma"), ("uolo", "volō", "verb", True, "lemma"),
                ("et", "et", "conj", False, "lemma"), ("aqua", "aqua", "noun", True, "lemma"),
                ("roma", "Rōma", "name", True, "lemma"), ("canis", "canis", "noun", False, "lemma"),
                ("zzz", "zzz", "noun", True, "lemma")]
        self.infos = []
        self.by_key = {}
        for i, (k, h, p, t, kind) in enumerate(recs):
            info = lexdata.LemmaInfo({"id": i, "key": k, "head": h, "word": h, "fpos": p, "kind": kind,
                                      "has_table": t}, True)
            self.infos.append(info)
            self.by_key.setdefault(k, []).append(i)
        # volo #1 is in DCC (want), volo #0 only Whitaker C; aqua and roma Whitaker A/X; canis A/X without table
        self.sig = {"whit": {0: "C", 1: "A", 3: "A", 4: "A", 5: "A"}, "whit_t2": {3, 4, 5}, "dcc": {1: 5, 2: 1}}

    def tearDown(self):
        shutil.rmtree(self.dir)

    def test_assign(self):
        got, missing = tiers.assign("la", self.infos, self.by_key, self.sig, lambda i: i != 6, self.dir)
        self.assertEqual(missing, ["nusquam"])
        self.assertEqual(got[1], (1, 2))   # curated, teacher; the DCC homograph wins the tie
        self.assertEqual(got[0], (3, 0))   # the other volō
        self.assertEqual(got[2], (1, 1))   # curated derived (also DCC)
        self.assertEqual(got[3], (2, 1))   # Whitaker A, age X, with a table
        self.assertEqual(got[4], (3, 0))   # proper name: never tier 2 from Whitaker
        self.assertEqual(got[5], (3, 0))   # no table
        self.assertEqual(got[6], (0, 0))   # no gloss

    def test_freq_rank(self):
        self.assertEqual(tiers.freq_rank(1, self.sig), 5)      # DCC rank
        self.assertEqual(tiers.freq_rank(0, self.sig), 3000)   # Whitaker C bucket
        self.assertEqual(tiers.freq_rank(6, self.sig), 0)


def tiny_lexicon():
    """Three lemmas written for this test (not B1's fixture): every LEMM field set to a distinct value."""
    feat_nom = F.pack(pos=1, case=1, number=1)
    feat_gen = F.pack(pos=1, case=2, number=1)
    feat_v = F.pack(pos=2, number=1, person=3, tense=1, mood=1, voice=1)
    a = pack.Lemma(head="rēx", key="rex", pos=1, cls=3, gender=1, tier=1, freq_rank=77, whit_freq=ord("A"),
                   tier_source=2, emoji="\U0001F451", gloss_en="king", gloss_es="rey",
                   senses=[("king", "rey", "king", 0, 1), ("ruler, chief", "", "ruler chief", 1 << 6, 2)],
                   flags=0x80, cells=[(feat_gen, "rēgis"), (feat_nom, "rēx")], principal_count=1,
                   principal="rēgis m.")
    b = pack.Lemma(head="currit", key="currit", pos=2, cls=3, tier=2, freq_rank=65535, whit_freq=ord("B"),
                   tier_source=1, gloss_en="runs", gloss_es="corre", senses=[("he runs", "corre", "run", 1 << 1, 1)],
                   flags=0x80 | 256, cells=[(feat_v, "currit")])
    c = pack.Lemma(head="zz", key="zz")
    analyses = [("rex", 0, feat_nom, 1, "rēx"), ("regis", 0, feat_gen, 1 | 32, "rēgis"),
                ("currit", 1, feat_v, 1, "currit"), ("currit", 2, 0, 2, "currit"), ("zz", 2, 0, 0, "zz")]
    cands = [("run", 1, 0, 160, 2), ("king", 0, 0, 205, 1), ("es:rey", 0, 0, 185, 1), ("king", 2, 0, 205, 0)]
    return pack.Lexicon("la", "tiny é notice", [a, b, c], analyses, cands)


class PackRoundTrip(unittest.TestCase):
    def test_every_field(self):
        lx = tiny_lexicon()
        data = pack.encode(lx)
        r = vpl_reader.VplReader(data=data, check_sha=True)
        self.assertEqual((r.lang, r.major, r.minor, r.file_size), ("la", 1, 0, len(data)))
        self.assertEqual([s[0] for s in r.sections], ["NOTE", "STRS", "KEYS", "ANAL", "LEMM", "SENS", "FEAT", "GENX",
                                                      "REVX"])
        self.assertEqual(r.notice(), "tiny é notice")
        r.validate()
        self.assertEqual([r.key(i) for i in range(r.n_keys)], ["currit", "regis", "rex", "zz"])
        for i, src in enumerate(lx.lemmas):
            l = r.lemma(i)
            for f in ("head", "key", "pos", "cls", "gender", "tier", "freq_rank", "whit_freq", "tier_source", "emoji",
                      "gloss_en", "gloss_es", "flags", "principal"):
                self.assertEqual(l[f], getattr(src, f if f != "principal" else "principal"), (i, f))
            self.assertEqual(l["principal_off_count"], src.principal_count)
            self.assertEqual(l["senses_count"], len(src.senses))
            self.assertEqual(l["gen_count"], len(src.cells))
            self.assertEqual([(s["gloss_en"], s["gloss_es"], s["keywords"], s["tags"], s["rank"])
                              for s in r.senses(i)], src.senses)
            self.assertEqual([(c[0], c[1]) for c in r.cells(i)], sorted(src.cells))
        self.assertEqual(r.lemma(1)["senses_start"], 2)
        self.assertEqual(r.lemma(1)["gen_start"], 2)
        self.assertEqual(r.generate(0, F.pack(pos=1, case=2, number=1)), "rēgis")
        self.assertIsNone(r.generate(0, F.pack(pos=1, case=3, number=1)))
        self.assertEqual([(a["lemma"], a["feat"], a["flags"], a["display"]) for a in r.lookup("currit")],
                         [(1, F.pack(pos=2, number=1, person=3, tense=1, mood=1, voice=1), 1, "currit"), (2, 0, 2, "currit")])
        self.assertEqual(r.lookup("regis")[0]["flags"], 33)
        self.assertEqual(r.lookup("nothing"), [])
        self.assertEqual([(c["lemma"], c["score"], c["pos"]) for c in r.reverse("king")], [(0, 205, 1), (2, 205, 0)])
        self.assertEqual(r.reverse("es:rey")[0]["lemma"], 0)
        self.assertEqual([r.keyword(i) for i in range(r.n_kw)], ["es:rey", "king", "run"])
        feats = [r.feature(i) for i in range(r.n_feat)]
        self.assertEqual(feats, sorted(set(a[2] for a in lx.analyses) | set(f for l in lx.lemmas for f, _ in l.cells)))
        self.assertEqual(r.feat_id(feats[2]), 2)
        # STRS: NUL at 0, then sorted distinct strings
        so, sl = r.sec["STRS"]
        parts = bytes(data[so + 1:so + sl - 1]).split(b"\0")
        self.assertEqual(parts, sorted(set(parts)))
        self.assertEqual(r.lemma(2)["emoji_off"], 0)

    def test_alignment_and_morphology_only(self):
        lx = tiny_lexicon()
        lx.morphology_only = True
        lx.lang = "en"
        data = pack.encode(lx)
        r = vpl_reader.VplReader(data=data, check_sha=True)
        self.assertEqual([s[0] for s in r.sections], ["NOTE", "STRS", "KEYS", "ANAL", "LEMM", "FEAT"])
        for _, off, _ in r.sections:
            self.assertEqual(off % 8, 0)
        self.assertEqual(r.lemma(0)["senses_count"], 0)
        self.assertEqual(r.lemma(0)["gen_count"], 0)
        self.assertEqual(len(data), r.sections[-1][1] + r.sections[-1][2])

    def test_reference_encoder_bytes(self):
        """Same bytes as engine/tests/lex_fixture_writer.cpp for B1's three fixtures (SPEC_CHECK.md)."""
        problems = compare_spec.compare(verbose=False)
        if problems is None:
            self.skipTest("tests/fixtures/lex/SPEC_CHECK.md not present")
        self.assertEqual(problems, [])

    def test_determinism(self):
        a = hashlib.sha256(pack.encode(tiny_lexicon())).hexdigest()
        b = hashlib.sha256(pack.encode(tiny_lexicon())).hexdigest()
        self.assertEqual(a, b)
        shas = []
        for _ in range(2):
            tmp = tempfile.mkdtemp(prefix="b4-")
            try:
                data, _, _ = make_vpl_spec.run_pipeline(make_vpl_spec.SPEC, tmp)
            finally:
                shutil.rmtree(tmp)
            shas.append(hashlib.sha256(data).hexdigest())
        self.assertEqual(shas[0], shas[1])
        with open(os.path.join(make_vpl_spec.SPEC, "latin.vpl"), "rb") as f:
            self.assertEqual(hashlib.sha256(f.read()).hexdigest(), shas[0],
                             "tests/fixtures/vpl_spec/latin.vpl is stale: run tests/make_vpl_spec.py")

    def test_fixture_fields(self):
        r = vpl_reader.VplReader(os.path.join(make_vpl_spec.SPEC, "latin.vpl"), check_sha=True)
        try:
            r.validate()
            aqua, amo, puella, roma, amatus = (r.lemma(i) for i in range(5))
            self.assertEqual((aqua["tier"], aqua["tier_source"], aqua["freq_rank"], aqua["emoji"]),
                             (2, 1, 20, "\U0001F4A7"))
            self.assertEqual((amo["tier"], amo["tier_source"], amo["whit_freq"]), (1, 2, ord("A")))
            self.assertEqual((roma["tier"], roma["flags"] & 1, roma["freq_rank"]), (3, 1, 1000))
            self.assertEqual((puella["gloss_es"], puella["flags"] & pack.LF_ES_PIVOT), ("niña", 0))
            self.assertEqual((amatus["gloss_en"], amatus["gloss_es"], amatus["flags"] & pack.LF_ES_PIVOT),
                             ("to love", "amar", pack.LF_ES_PIVOT))
            self.assertEqual(r.senses(1)[0]["tags"], gloss.tag_bit("transitive") | gloss.tag_bit("with-acc"))
            self.assertEqual(r.senses(2)[1]["tags"], gloss.tag_bit("poetic"))
            # headword fix: the vocative "aqua" came in with bit3+bit4 and leaves without them; aquāī keeps bit3
            self.assertEqual(sorted(a["flags"] for a in r.lookup("aqua")), [1, 1, 1])
            self.assertEqual(r.lookup("aquai")[0]["flags"], 1 | 8 | 64)
            # one display form per cell; the alternative genitive has its own feature word (extra alternative)
            cells = r.cells(0)
            self.assertEqual(len(cells), len(set(c[0] for c in cells)))
            self.assertIn("aquāī", [c[1] for c in cells])
            self.assertIn("Whitaker", r.notice())
            self.assertIn("Pérez Cartagena", r.notice())
        finally:
            r.close()


class GenxChoice(unittest.TestCase):
    def test_one_form_per_cell(self):
        rows = [["0", "λόγοιο", "x", "genitive singular", "declension", "Epic declension-2"],
                ["0", "λόγου", "x", "genitive singular", "declension", "declension-2"],
                ["0", "λόγου", "x", "alternative genitive singular", "head", ""]]
        cells = pack.cell_rows("grc", rows, "noun", 0)
        chosen = dict(pack.choose_cells(cells))
        self.assertEqual(chosen[F.pack(pos=1, case=2, number=1)], "λόγου")
        self.assertEqual(len(chosen), 2)  # the alternative row has its own feature word


SPOT_EN = [("water", ["aqua"]), ("king", ["rēx"]), ("girl", ["puella"]), ("dog", ["canis"]),
           ("rabbit", ["lepus", "cunīculus"]), ("late", ["sērō"]), ("queen", ["rēgīna"]),
           ("run", ["currō"]), ("love", ["amō"]), ("see", ["videō"]), ("say", ["dīcō"]),
           ("big", ["magnus"]), ("small", ["parvus"]), ("cat", ["fēlēs"]), ("door", ["iānua"]),
           ("garden", ["hortus"]), ("time", ["tempus"]), ("know", ["sciō"]), ("want", ["volō"]),
           ("go", ["eō"]), ("come", ["veniō"]),
           ("es:agua", ["aqua"]), ("es:rey", ["rēx"]), ("es:niña", ["puella"]), ("es:perro", ["canis"]),
           ("es:ver", ["videō"]), ("es:querer", ["volō"])]
ALSO_TOP3 = {"late": "sērus"}


@unittest.skipUnless(os.path.exists(os.path.join(WORK, "latin.vpl")), "data/work/latin.vpl not built")
class SpotList(unittest.TestCase):
    def test_spot_list(self):
        r = vpl_reader.VplReader(os.path.join(WORK, "latin.vpl"))
        bad = []
        try:
            for kw, first in SPOT_EN:
                top = [r.lemma(c["lemma"]) for c in r.reverse(kw)[:3]]
                heads = [l["head"] for l in top]
                if not heads or heads[0] not in first:
                    bad.append("%s -> %s (expected first %s)" % (kw, heads, first))
                if kw in ALSO_TOP3 and ALSO_TOP3[kw] not in heads:
                    bad.append("%s -> %s (expected %s in the top 3)" % (kw, heads, ALSO_TOP3[kw]))
                if kw == "want" and top and "velle" not in top[0]["principal"]:
                    bad.append("want -> the wrong volō: %s" % top[0]["principal"])
        finally:
            r.close()
        self.assertEqual(bad, [])


if __name__ == "__main__":
    unittest.main()
