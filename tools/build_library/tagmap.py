"""Kaikki / Wiktextract tag strings -> packed features (features.py, DESIGN.md section 5 FEAT).

tags_to_feature_list(tags, pos) returns every packed value a tag set stands for: a form-of sense tagged
"dative genitive singular" is two analyses. Genders combine (masculine+feminine -> mf) instead of expanding.
Tags without a slot in the enumerations are either IGNORED (known, not morphological for our purpose), LOSSY
(morphological but no enum value, e.g. Spanish "conditional") or unknown; lossy and unknown tags are counted in a
TagStats object and end up in report.json. Nothing here raises on odd input.

Mappings that are a choice, not a copy (documented in tools/build_library/README.md):
  past -> perfect (English/Spanish simple past); past+imperfect -> imperfect; present+perfect -> perfect (Spanish
  preterite, Greek "perfect present" tables); future+perfect -> future-perfect (Latin future perfect);
  mediopassive -> middle and passive (two analyses); adverbial -> pos adv; Attic -> extra attic bit;
  common-gender -> mf.
"""
import collections
import itertools

import features as F

CASE_TAGS = {k: v for k, v in F.CASE.items() if k != "none"}
NUMBER_TAGS = {k: v for k, v in F.NUMBER.items() if k != "none"}
PERSON_TAGS = {k: v for k, v in F.PERSON.items() if k != "none"}
TENSE_TAGS = {k: v for k, v in F.TENSE.items() if k != "none"}
MOOD_TAGS = {k: v for k, v in F.MOOD.items() if k != "none"}
VOICE_TAGS = {k: v for k, v in F.VOICE.items() if k != "none"}
DEGREE_TAGS = {k: v for k, v in F.DEGREE.items() if k != "none"}
GENDER_BITS = {"masculine": 1, "feminine": 2, "neuter": 4}
GENDER_FROM_BITS = {0: 0, 1: F.GENDER["masculine"], 2: F.GENDER["feminine"], 4: F.GENDER["neuter"],
                    3: F.GENDER["mf"], 5: F.GENDER["mn"], 6: F.GENDER["fn"], 7: F.GENDER["mfn"]}
EXTRA_TAGS = {"supine": F.EXTRA["supine"], "gerundive": F.EXTRA["gerundive"], "Attic": F.EXTRA["attic"],
              "alternative": F.EXTRA["alternative"], "contracted": F.EXTRA["contracted"]}

# Greek and Latin dialect / era names. Dialect tags never change the features (except Attic -> extra bit);
# the stages use them for the ANAL flags.
GREEK_DIALECTS = frozenset("""Attic Ionic Epic Doric Aeolic Koine Byzantine Laconian Lesbian Cretan Boeotian Thessalian
Elean Locrian Arcadocypriot Cypriot Old-Attic Corinthian Epirote Macedonian Lyric-Ancient-Greek Tragic-Ancient-Greek
Choral-Doric Mycenaean Homeric Arcadian Pamphylian Argolic Megarian Rhodian Syracusan Euboean Severe-Doric Ionic-Attic
Attic-Ionic Old-Ionic Late-Koine Biblical Ancient-Greek Medieval-Greek""".split())
LATIN_ERAS = frozenset("""Medieval-Latin Late-Latin New-Latin Ecclesiastical Modern-Latin Neo-Latin Scientific-Latin
Anglo-Latin Contemporary-Latin Renaissance-Latin Medieval Mediaeval Late""".split())

LOSSY = frozenset("""hortative singulative conditional conditional-ii potential sigmatic vos-form compound anterior imperfective perfective
iterative preterite multiword-construction noun-from-verb stem oblique instrumental dative-of-advantage
impersonal reflexive pronominal""".split())

IGNORED = frozenset("""form-of alt-of table-tags inflection-template class canonical romanization no-table-tags
declension-1 declension-2 declension-3 declension-4 declension-5 conjugation-1 conjugation-2 conjugation-3
conjugation-4 conjugation-5 irregular deponent semi-deponent no-supine no-perfect no-genitive no-gloss empty-gloss
one-termination two-termination three-termination indeclinable not-comparable no-comparative no-superlative
comparable plural-only singular-only plural-normally in-plural uncountable countable collective
archaic poetic rare obsolete dated nonstandard proscribed vulgar colloquial informal literary formal slang humorous
derogatory euphemistic figuratively literally metonymically broadly usually often sometimes especially also mainly
specifically possibly uncommon common historical idiomatic rhetoric discourse ellipsis abbreviation initialism
acronym clipping contraction syncope apocopic augmented unaugmented diminutive frequentative causative desiderative
intensifier emphatic enclitic proclitic clitic unstressed stressed before-vowel in-compounds morpheme letter
uppercase lowercase capitalized diacritic symbol numeral cardinal ordinal relative interrogative demonstrative
indefinite definite personal possessive reciprocal anaphoric proximal medial distal negative affirmative
transitive intransitive ambitransitive ditransitive copulative auxiliary modal stative absolute substantive
adjectival adjective adverb noun verb pronoun particle determiner name place person suffix prefix phrase
with-genitive with-dative with-accusative with-ablative with-infinitive with-subjunctive with-nominative
with-locative with-definite-article with-negation not-clause-initial imperative-only no-infinitive
reconstruction hypothetical nonce-word neologism misspelling pronunciation-spelling hypercorrect variant
dialectal regional standard error-unknown-tag error-unrecognized-form error-lua-exec suppletive defective
relational concrete abstract animate inanimate gender-neutral by-personal-gender praenominal Praenominal
demonym patronymic endearing childish familiar polite offensive ironic Internet Judaism Christian Jewish
Ancient-Rome Roman Greek Latin Classical Classical-Latin Old-Latin post-Classical post-Augustan post-Homeric
Proto-Romance Proto-Italo-Western-Romance Proto-Western-Romance Proto-Gallo-Romance Proto-Ibero-Romance
Proto-Balkan-Romance Ibero-Romance Germanic Norse Egyptian Early Middle Classical-Greek Greek-type i-stem
attributive predicative subjective objective conjunctive parenthetic temporal consequential material special
general extended specific superior inclusive mildly weak repeated root agent mediopassive-only
feminine-usually masculine-usually generic polytonic monotonic cardinal-numeral distributive multiplicative
England Britain France Germany Italy Spain Europe US India Western Eastern Southern Northern West East
Argentina Uruguay Mexico Chile Spain Peru Colombia Venezuela Cuba Caribbean Rioplatense Andalusia
archaic-form simple progressive continuous declinable partitive postpositional reduplication unknown
conditional-mood outdated comparative-only definition deliberate dependent derived ethnic exaggerated excessive hard
honorific impolite including independent indirect interjection invariable inversion jargon misconstruction mixed
mnemonic modern no-past no-past-participle no-plural no-present-participle non-scientific onomatopoeic phonetic
physical plural-of proper-noun recently reduced sarcastic secular short-form singular-normally slur surname
term-of-address transliteration universal vernacular with-up without-noun""".split())

# tags that stand for two tags
COMPOUND_TAGS = {"middle-infinitive": ("middle", "infinitive"), "passive-infinitive": ("passive", "infinitive"),
                 "active-infinitive": ("active", "infinitive"), "future-infinitive": ("future", "infinitive")}
# lower-case dialect spellings seen in the data
DIALECT_ALIASES = {"epic": "Epic", "ionic": "Ionic", "attic": "Attic", "doric": "Doric", "aeolic": "Aeolic"}

# tags that change the POS of the analysis
POS_OVERRIDE = {"adverbial": "adv"}

KAIKKI_POS = {"noun": "noun", "verb": "verb", "adj": "adj", "adv": "adv", "pron": "pron", "num": "num",
              "prep": "prep", "conj": "conj", "intj": "intj", "det": "det", "name": "name", "particle": "particle",
              "participle": "participle", "phrase": "phrase", "prep_phrase": "phrase", "proverb": "phrase",
              "suffix": "suffix", "prefix": "prefix", "article": "article", "postp": "postp", "symbol": "symbol",
              "character": "symbol", "punct": "punct", "combining_form": "prefix", "adj_noun": "adj",
              "noun_phrase": "phrase", "verb_phrase": "phrase", "adv_phrase": "phrase", "infix": "other",
              "interfix": "other", "circumfix": "other", "contraction": "other", "affix": "other"}


class TagStats(object):
    def __init__(self):
        self.unknown = collections.Counter()
        self.lossy = collections.Counter()
        self.labels = collections.Counter()

    def as_dict(self, limit=200):
        return {"unknown": dict(self.unknown.most_common(limit)), "lossy": dict(self.lossy.most_common(limit)),
                "labels": dict(self.labels.most_common(40))}


_DEFAULT_STATS = TagStats()


def pos_name(kaikki_pos, head_template="", head_arg2=""):
    """features.POS key for a Kaikki entry; participles are recognised by their head template."""
    ht = head_template or ""
    if ht.startswith("la-part") or ht.startswith("grc-part") or "participle" in (head_arg2 or ""):
        return "participle"
    return KAIKKI_POS.get(kaikki_pos or "", "other")


def _resolve_tense(ts):
    """Set of tense tags -> list of TENSE codes (several = ambiguous, expanded)."""
    if not ts:
        return [0]
    ts = set(ts)
    if "future-perfect" in ts:
        return [F.TENSE["future-perfect"]]
    if "pluperfect" in ts:
        return [F.TENSE["pluperfect"]]
    if "future" in ts and "perfect" in ts:
        return [F.TENSE["future-perfect"]]
    if "perfect" in ts and ts <= {"perfect", "present", "past"}:
        return [F.TENSE["perfect"]]
    if "imperfect" in ts and ts <= {"imperfect", "past"}:
        return [F.TENSE["imperfect"]]
    if ts == {"past"}:
        return [F.TENSE["perfect"]]
    ts.discard("past")
    return sorted(TENSE_TAGS[t] for t in ts if t in TENSE_TAGS) or [0]


def tags_to_feature_list(tags, pos="none", tense_hint=None, extra=0, stats=None):
    """All packed feature values for one tag set. pos is a features.POS key (or a Kaikki pos string).
    tense_hint (a TENSE key) is used when the tags carry no tense (Greek verb tables carry it in the table
    marker). extra is OR-ed into the extra field (e.g. contracted, attic from the table marker)."""
    st = stats if stats is not None else _DEFAULT_STATS
    pos = pos if pos in F.POS else KAIKKI_POS.get(pos, "other")
    cases, numbers, persons, tenses, moods, voices, degrees = [], [], [], [], [], [], []
    gbits = 0
    ex = extra
    expanded = []
    for t in tags or ():
        t = DIALECT_ALIASES.get(t, t)
        expanded.extend(COMPOUND_TAGS.get(t, (t,)))
    for t in expanded:
        if t in CASE_TAGS:
            cases.append(CASE_TAGS[t])
        elif t in NUMBER_TAGS:
            numbers.append(NUMBER_TAGS[t])
        elif t in GENDER_BITS:
            gbits |= GENDER_BITS[t]
        elif t == "common-gender":
            gbits |= 3
        elif t in PERSON_TAGS:
            persons.append(PERSON_TAGS[t])
        elif t in TENSE_TAGS or t == "past":
            tenses.append(t)
        elif t in MOOD_TAGS:
            moods.append(MOOD_TAGS[t])
        elif t in VOICE_TAGS:
            voices.append(VOICE_TAGS[t])
        elif t == "mediopassive":
            voices.extend((VOICE_TAGS["middle"], VOICE_TAGS["passive"]))
        elif t in DEGREE_TAGS:
            degrees.append(DEGREE_TAGS[t])
        elif t in EXTRA_TAGS:
            ex |= EXTRA_TAGS[t]
        elif t in POS_OVERRIDE:
            pos = POS_OVERRIDE[t]
        elif t in LOSSY:
            st.lossy[t] += 1
        elif t in IGNORED or t in GREEK_DIALECTS or t in LATIN_ERAS:
            pass
        elif t[:1].isupper():
            st.labels[t] += 1  # capitalised tags are region / variety labels (UK, Scotland, Singlish)
        else:
            st.unknown[t] += 1
    if not tenses and tense_hint:
        tenses = [tense_hint]
    pos_v = F.POS[pos]
    gender = GENDER_FROM_BITS[gbits]
    out = []
    for c, n, p, te, mo, vo, de in itertools.product(sorted(set(cases)) or [0], sorted(set(numbers)) or [0],
                                                     sorted(set(persons)) or [0], _resolve_tense(tenses),
                                                     sorted(set(moods)) or [0], sorted(set(voices)) or [0],
                                                     sorted(set(degrees)) or [0]):
        out.append(F.pack(pos=pos_v, case=c, number=n, gender=gender, person=p, tense=te, mood=mo, voice=vo,
                          degree=de, extra=ex))
    return out


def align_pos(packed, lemma_pos):
    """Make a form-page analysis agree with its lemma's POS: a participle form of a verb is a verb in mood
    participle (as in the verb's own table); a form of a participle lemma is pos participle without a mood."""
    u = F.unpack(packed)
    if lemma_pos == "verb" and u["pos"] == F.POS["participle"]:
        u["pos"] = F.POS["verb"]
        u["mood"] = F.MOOD["participle"]
    elif lemma_pos == "participle" and u["pos"] in (F.POS["participle"], F.POS["verb"]):
        u["pos"] = F.POS["participle"]
        if u["mood"] == F.MOOD["participle"]:
            u["mood"] = 0
    else:
        return packed
    return F.pack(**u)


def tags_to_features(tags, pos="none", **kw):
    """The first (lowest) packed value for a tag set; use tags_to_feature_list for ambiguous sets."""
    return tags_to_feature_list(tags, pos, **kw)[0]


def golden_features(tags, pos):
    """Used by make_golden.py: the tag set must be unambiguous."""
    vals = tags_to_feature_list(tags, pos, stats=TagStats())
    if len(vals) != 1:
        raise ValueError("ambiguous tag set for the golden file: %r" % (tags,))
    return vals[0]


def describe(packed):
    """Human-readable form of a packed value (for reports and debugging)."""
    u = F.unpack(packed)
    names = []
    for field, table in (("pos", F.POS), ("case", F.CASE), ("number", F.NUMBER), ("gender", F.GENDER),
                         ("person", F.PERSON), ("tense", F.TENSE), ("mood", F.MOOD), ("voice", F.VOICE),
                         ("degree", F.DEGREE)):
        if u[field]:
            inv = {v: k for k, v in table.items()}
            names.append(inv.get(u[field], "%s?%d" % (field, u[field])))
    for k, bit in sorted(F.EXTRA.items(), key=lambda kv: kv[1]):
        if u["extra"] & bit:
            names.append(k)
    return " ".join(names)
