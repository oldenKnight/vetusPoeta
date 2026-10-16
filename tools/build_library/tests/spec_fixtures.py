"""The three lexicons of engine/tests/lex_fixture_writer.cpp (latinFixture, greekFixture, englishFixture), written
again in Python, so pack.encode() can be compared byte for byte with tests/fixtures/lex/*.vpl (B1's reference
encoder). Same lemma order, same analysis order, same candidate list (unsorted, as in the C++ file)."""
import _paths  # noqa: F401
import features as F
import pack
import vptext

NOUN, VERB, ADJ = F.POS["noun"], F.POS["verb"], F.POS["adj"]
NOM, GEN, DAT, ACC, ABL, VOC = 1, 2, 3, 4, 5, 6
SG, PL, DU = 1, 2, 3
M, FEM, N, MFN = 1, 2, 3, 7
PRES, IMPF, FUT, PERF = 1, 2, 3, 4
IND, SUBJ, IMPV, INF, PART = 1, 2, 3, 4, 5
ACT, PASS = 1, 2
ATTIC, ALT = 4, 8
CASES = (NOM, GEN, DAT, ACC, ABL, VOC)


def PK(pos, cs=0, num=0, gen=0, pers=0, tense=0, mood=0, voice=0, deg=0, extra=0):
    return F.pack(pos=pos, case=cs, number=num, gender=gen, person=pers, tense=tense, mood=mood, voice=voice,
                  degree=deg, extra=extra)


def _add(lemmas, analyses, lemma, feat, form, flags, key_fn, cell=True):
    if cell:
        lemmas[lemma].cells.append((feat, form))
    analyses.append((key_fn(form), lemma, feat, flags, form))


def latin():
    lemmas, analyses = [], []

    def lemma(head, pos, cls, gender, principal, en, es):
        lemmas.append(pack.Lemma(head=head, key=vptext.latin_key(head), pos=pos, cls=cls, gender=gender,
                                 principal=principal, principal_count=1, gloss_en=en, gloss_es=es))
        return len(lemmas) - 1
    puella = lemma("puella", NOUN, 1, FEM, "puellae, f.", "girl", "niña")
    amo = lemma("amō", VERB, 1, 0, "amō, amāre, amāvī, amātum", "to love", "amar")
    bonus = lemma("bonus", ADJ, 1, 0, "bonus, bona, bonum", "good", "bueno")
    virgo = lemma("virgō", NOUN, 3, FEM, "virginis, f.", "maiden", "doncella")
    diligo = lemma("dīligō", VERB, 3, 0, "dīligō, dīligere, dīlēxī, "
                   "dīlēctum", "to esteem", "apreciar")
    amor = lemma("amor", NOUN, 3, M, "amōris, m.", "love", "amor")
    for l in lemmas:
        l.flags, l.tier, l.tier_source, l.whit_freq = 0x80, 1, 2, ord("A")
    lemmas[puella].emoji = "\U0001F467"
    lemmas[puella].freq_rank = 412
    lemmas[amo].emoji = "❤"
    lemmas[amo].freq_rank = 128
    lemmas[virgo].tier, lemmas[virgo].tier_source, lemmas[virgo].whit_freq = 2, 1, ord("B")
    lemmas[diligo].tier, lemmas[diligo].tier_source = 2, 1
    lk = vptext.latin_key
    sg = ["puella", "puellae", "puellae", "puellam", "puellā", "puella"]
    pl = ["puellae", "puellārum", "puellīs", "puellās", "puellīs", "puellae"]
    for c in range(6):
        _add(lemmas, analyses, puella, PK(NOUN, CASES[c], SG), sg[c], 1, lk)
    for c in range(6):
        _add(lemmas, analyses, puella, PK(NOUN, CASES[c], PL), pl[c], 1, lk)
    _add(lemmas, analyses, puella, PK(NOUN, GEN, SG, extra=ALT), "puellāī", 1 | 64, lk)
    lemmas[puella].senses = [("girl", "niña", "girl", 0, 1),
                             ("sweetheart, beloved", "amada, novia", "sweetheart beloved girl", 1 << 5, 2)]
    tenses = [["amō", "amās", "amat", "amāmus", "amātis", "amant"],
              ["amābam", "amābās", "amābat", "amābāmus", "amābātis",
               "amābant"],
              ["amābō", "amābis", "amābit", "amābimus", "amābitis", "amābunt"],
              ["amāvī", "amāvistī", "amāvit", "amāvimus", "amāvistis",
               "amāvērunt"],
              ["amor", "amāris", "amātur", "amāmur", "amāminī", "amantur"]]
    tense_of = [PRES, IMPF, FUT, PERF, PRES]
    voice_of = [ACT, ACT, ACT, ACT, PASS]
    for t in range(5):
        for k in range(6):
            _add(lemmas, analyses, amo, PK(VERB, 0, SG if k < 3 else PL, 0, k % 3 + 1, tense_of[t], IND, voice_of[t]),
                 tenses[t][k], 1, lk)
    subj = ["amem", "amēs", "amet", "amēmus", "amētis", "ament"]
    for k in range(6):
        _add(lemmas, analyses, amo, PK(VERB, 0, SG if k < 3 else PL, 0, k % 3 + 1, PRES, SUBJ, ACT), subj[k], 1, lk)
    _add(lemmas, analyses, amo, PK(VERB, 0, 0, 0, 0, PRES, INF, ACT), "amāre", 1, lk)
    _add(lemmas, analyses, amo, PK(VERB, 0, 0, 0, 0, PRES, INF, PASS), "amārī", 1, lk)
    _add(lemmas, analyses, amo, PK(VERB, 0, 0, 0, 0, PERF, INF, ACT), "amāvisse", 1, lk)
    _add(lemmas, analyses, amo, PK(VERB, 0, SG, 0, 2, PRES, IMPV, ACT), "amā", 1, lk)
    _add(lemmas, analyses, amo, PK(VERB, 0, PL, 0, 2, PRES, IMPV, ACT), "amāte", 1, lk)
    _add(lemmas, analyses, amo, PK(VERB, NOM, SG, MFN, 0, PRES, PART, ACT), "amāns", 1, lk)
    _add(lemmas, analyses, amo, PK(VERB, 0, SG, 0, 2, PERF, IND, ACT), "amāstī", 2 | 8, lk, cell=False)
    lemmas[amo].senses = [("to love", "amar", "love", 1, 1), ("to like, be fond of", "gustar, apreciar", "like fond", 1, 2)]
    bon = [[["bonus", "bonī", "bonō", "bonum", "bonō", "bone"],
            ["bonī", "bonōrum", "bonīs", "bonōs", "bonīs", "bonī"]],
           [["bona", "bonae", "bonae", "bonam", "bonā", "bona"],
            ["bonae", "bonārum", "bonīs", "bonās", "bonīs", "bonae"]],
           [["bonum", "bonī", "bonō", "bonum", "bonō", "bonum"],
            ["bona", "bonōrum", "bonīs", "bona", "bonīs", "bona"]]]
    for g, gender in enumerate((M, FEM, N)):
        for nn in range(2):
            for c in range(6):
                _add(lemmas, analyses, bonus, PK(ADJ, CASES[c], SG if nn == 0 else PL, gender), bon[g][nn][c], 1, lk)
    lemmas[bonus].senses = [("good", "bueno", "good", 0, 1)]
    _add(lemmas, analyses, virgo, PK(NOUN, NOM, SG), "virgō", 1, lk)
    _add(lemmas, analyses, virgo, PK(NOUN, GEN, SG), "virginis", 1, lk)
    lemmas[virgo].senses = [("maiden, young woman", "doncella", "maiden girl", 0, 1)]
    _add(lemmas, analyses, diligo, PK(VERB, 0, SG, 0, 1, PRES, IND, ACT), "dīligō", 1, lk)
    lemmas[diligo].senses = [("to esteem, to love", "apreciar, amar", "esteem love", 1, 1)]
    _add(lemmas, analyses, amor, PK(NOUN, NOM, SG), "amor", 1, lk)
    _add(lemmas, analyses, amor, PK(NOUN, GEN, SG), "amōris", 1, lk)
    lemmas[amor].senses = [("love", "amor", "love", 0, 1)]
    cands = [("love", amor, 0, 95, NOUN), ("girl", virgo, 0, 60, NOUN), ("love", amo, 0, 175, VERB),
             ("girl", puella, 1, 60, NOUN), ("love", diligo, 0, 95, VERB), ("girl", puella, 0, 160, NOUN),
             ("good", bonus, 0, 160, ADJ), ("maiden", virgo, 0, 160, NOUN), ("sweetheart", puella, 1, 95, NOUN),
             ("beloved", puella, 1, 60, NOUN), ("esteem", diligo, 0, 160, VERB), ("like", amo, 1, 95, VERB),
             ("fond", amo, 1, 60, VERB)]
    return pack.Lexicon("la", "Test fixture for engine/lex. Forms and glosses written for the test suite.",
                        lemmas, analyses, cands)


def greek():
    gk = vptext.greek_key
    l = pack.Lemma(head="ἄνθρωπος", pos=NOUN, cls=2, gender=M, tier=1,
                   tier_source=2, gloss_en="human being, person", gloss_es="ser humano, persona",
                   principal="ἀνθρώπου, ὁ", principal_count=1, flags=0x80)
    l.key = gk(l.head)
    l.senses = [("human being, person", "ser humano, persona", "human person man", 0, 1)]
    lemmas, analyses = [l], []
    sg = ["ἄνθρωπος", "ἀνθρώπου",
          "ἀνθρώπῳ", "ἄνθρωπον",
          "ἄνθρωπε"]
    du = ["ἀνθρώπω", "ἀνθρώποιν",
          "ἀνθρώποιν", "ἀνθρώπω",
          "ἀνθρώπω"]
    pl = ["ἄνθρωποι", "ἀνθρώπων",
          "ἀνθρώποις", "ἀνθρώπους",
          "ἄνθρωποι"]
    cases = (NOM, GEN, DAT, ACC, VOC)
    for forms, num in ((sg, SG), (du, DU), (pl, PL)):
        for c in range(5):
            _add(lemmas, analyses, 0, PK(NOUN, cases[c], num, extra=ATTIC), forms[c], 1, gk)
    _add(lemmas, analyses, 0, PK(NOUN, GEN, SG), "ἀνθρώποιο", 1 | 16, gk)
    _add(lemmas, analyses, 0, PK(NOUN, DAT, PL), "ἀνθρώποισι",
         1 | 16, gk)
    cands = [("human", 0, 0, 160, NOUN), ("person", 0, 0, 160, NOUN), ("man", 0, 0, 95, NOUN)]
    return pack.Lexicon("grc", "Test fixture for engine/lex (Ancient Greek). Forms written for the test suite.",
                        lemmas, analyses, cands)


def english():
    ek = vptext.en_key
    lemmas, analyses = [], []
    for head, pos in (("go", VERB), ("see", VERB), ("saw", NOUN), ("child", NOUN)):
        lemmas.append(pack.Lemma(head=head, key=ek(head), pos=pos))
    go, see, saw, child = 0, 1, 2, 3
    pres3, past = PK(VERB, 0, SG, 0, 3, PRES), PK(VERB, 0, 0, 0, 0, PERF)
    past_part, pres_part = PK(VERB, 0, 0, 0, 0, PERF, PART), PK(VERB, 0, 0, 0, 0, PRES, PART)
    inf, nsg, npl = PK(VERB, 0, 0, 0, 0, 0, INF), PK(NOUN, 0, SG), PK(NOUN, 0, PL)
    for lem, feat, form in ((go, inf, "go"), (go, pres3, "goes"), (go, past, "went"), (go, past_part, "gone"),
                            (go, pres_part, "going"), (see, inf, "see"), (see, pres3, "sees"), (see, past, "saw"),
                            (see, past_part, "seen"), (saw, nsg, "saw"), (saw, npl, "saws"), (child, nsg, "child"),
                            (child, npl, "children")):
        _add(lemmas, analyses, lem, feat, form, 1, ek, cell=False)
    return pack.Lexicon("en", "Test fixture for engine/lex (English morphology only).", lemmas, analyses,
                        morphology_only=True)


ALL = {"latin.vpl": latin, "greek.vpl": greek, "english.vpl": english}
