#!/usr/bin/env python3
"""Hand-made inputs for the B4 stages (import_aux, gloss, tiers, pack) in tests/fixtures/vpl_spec/, and the tiny
latin.vpl they produce. Everything here is written for the test suite (no dictionary or dump excerpts).

  python3 tools/build_library/tests/make_vpl_spec.py           # rewrite the inputs and latin.vpl
  python3 tools/build_library/tests/make_vpl_spec.py --check   # exit 1 when latin.vpl would change
Layout: raw/aux/... (Whitaker lines, a 3-entry Lewis & Short file, DCC CSVs), work/{la,en,es}/... (files in the
format of the kaikki/resolve stages), curated/ (tiers, Spanish glosses, valency, emoji), latin.vpl.
"""
import json
import os
import shutil
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import _paths  # noqa: E402
import features as F  # noqa: E402
import vptext  # noqa: E402

SPEC = os.path.join(_paths.FIXTURES, "vpl_spec")
N, V, PART, NAME = F.POS["noun"], F.POS["verb"], F.POS["participle"], F.POS["name"]


def P(pos, case=0, number=0, gender=0, person=0, tense=0, mood=0, voice=0, extra=0):
    return F.pack(pos=pos, case=case, number=number, gender=gender, person=person, tense=tense, mood=mood,
                  voice=voice, extra=extra)


LEMMAS = [
    {"id": 0, "word": "aqua", "key": "aqua", "head": "aqua", "pos": "noun", "fpos": "noun", "ht": "la-noun",
     "kind": "lemma", "has_table": True, "gender": ["feminine"], "class": ["declension-1"],
     "pp": "aqua f (genitive aquae); first declension", "senses": [{"g": "water", "tags": ["declension-1"]}]},
    {"id": 1, "word": "amo", "key": "amo", "head": "amō", "pos": "verb", "fpos": "verb", "ht": "la-verb",
     "kind": "lemma", "has_table": True, "class": ["conjugation-1"],
     "pp": "amō (present infinitive amāre); first conjugation",
     "senses": [{"g": "to love", "tags": ["transitive"]}, {"g": "to like, be fond of"}]},
    {"id": 2, "word": "puella", "key": "puella", "head": "puella", "pos": "noun", "fpos": "noun", "ht": "la-noun",
     "kind": "lemma", "has_table": True, "gender": ["feminine"], "class": ["declension-1"],
     "pp": "puella f (genitive puellae); first declension",
     "senses": [{"g": "girl, maiden"}, {"g": "sweetheart", "tags": ["poetic"]}]},
    {"id": 3, "word": "Roma", "key": "roma", "head": "Rōma", "pos": "name", "fpos": "name", "ht": "la-proper noun",
     "kind": "lemma", "has_table": True, "gender": ["feminine"], "class": ["declension-1"],
     "pp": "Rōma f (genitive Rōmae); first declension", "senses": [{"g": "Rome"}]},
    {"id": 4, "word": "amatus", "key": "amatus", "head": "amātus", "pos": "verb", "fpos": "participle",
     "ht": "la-part", "kind": "formtable", "has_table": True, "senses": [], "of": ["amō"]},
]
# lemma_id, display, tags, source, marker
TABLE = [
    (0, "aqua", "nominative singular"), (0, "aquae", "genitive singular"), (0, "aquae", "dative singular"),
    (0, "aquam", "accusative singular"), (0, "aquā", "ablative singular"), (0, "aqua", "vocative singular"),
    (0, "aquāī", "alternative genitive singular"),
    (1, "amō", "active first-person indicative present singular"),
    (1, "amat", "active indicative present singular third-person"),
    (1, "amāre", "active infinitive present"),
    (2, "puella", "nominative singular"), (2, "puellae", "genitive singular"), (2, "puellae", "nominative plural"),
    (3, "Rōma", "nominative singular"), (3, "Rōmae", "genitive singular"),
    (4, "amātus", "masculine nominative singular"), (4, "amāta", "feminine nominative singular"),
]
FEATS = {
    "nominative singular": dict(case=1, number=1), "genitive singular": dict(case=2, number=1),
    "dative singular": dict(case=3, number=1), "accusative singular": dict(case=4, number=1),
    "ablative singular": dict(case=5, number=1), "vocative singular": dict(case=6, number=1),
    "nominative plural": dict(case=1, number=2), "alternative genitive singular": dict(case=2, number=1, extra=8),
    "active first-person indicative present singular": dict(number=1, person=1, tense=1, mood=1, voice=1),
    "active indicative present singular third-person": dict(number=1, person=3, tense=1, mood=1, voice=1),
    "active infinitive present": dict(tense=1, mood=4, voice=1),
    "masculine nominative singular": dict(case=1, number=1, gender=1),
    "feminine nominative singular": dict(case=1, number=1, gender=2),
}
POS_OF = {0: N, 1: V, 2: N, 3: NAME, 4: PART}
# extra analysis flags: the vocative "aqua" deliberately carries bit3+bit4 (pack must clear them: headword fix);
# "aquāī" keeps bit3 (alternative spelling, not the headword)
FLAGS = {(0, "aqua", "vocative singular"): 1 | 8 | 16, (0, "aquāī", "alternative genitive singular"): 1 | 8 | 64}

DICTLINE = [  # hand-typed in the DICTLINE.GEN column layout (stems 0/19/38/57, POS 76, codes 83, letters 100..108)
    ("aqu", "aqu", "", "", "N", "1 1 F T", "X X X A O", "water; sea, lake;"),
    ("am", "am", "amav", "amat", "V", "1 1 X", "X X X A O", "love, like; be fond of;"),
    ("puell", "puell", "", "", "N", "1 1 F P", "X X X A O", "girl, (female) child; maiden;"),
    ("Rom", "Rom", "", "", "N", "1 1 F L", "X X X A O", "Rome;"),
    ("zzyzx", "zzyzx", "", "", "N", "2 1 M T", "F X X F W", "made-up word;"),
]


def dictline(stem1, stem2, stem3, stem4, pos, codes, letters, meaning):
    return (stem1.ljust(19) + stem2.ljust(19) + stem3.ljust(19) + stem4.ljust(19) + pos.ljust(7) +
            codes.ljust(17) + letters + " " + meaning)


LS_XML = """<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE TEI.2 PUBLIC "-//TEI P4//DTD Main DTD Driver File//EN" "http://www.tei-c.org/Vault/P4/xml/schema/dtd/tei2.dtd" [
<!ENTITY % PersDict PUBLIC "-//Perseus P4//DTD Perseus Dictionaries//EN" "http://www.perseus.tufts.edu/DTD/1.0/PersDict.dtd">
%PersDict;
]>
<TEI.2><text><body><div0>
<entryFree id="t1" type="main" key="aqua"><orth extent="full" lang="la">ăqua</orth>, <itype>ae</itype>, <gen>f.</gen> <sense id="t1.0" n="I" level="1"><hi rend="ital">water</hi>: <trans><tr>rain-water</tr></trans></sense></entryFree>
<entryFree id="t2" type="main" key="amo1"><orth extent="full" lang="la">ămo</orth>, <itype>āvi, ātum, 1</itype>, <pos>v. a.</pos> <sense id="t2.0" n="I" level="1"><hi rend="ital">to love</hi> <usg type="style">poet.</usg> <trans><tr>love each other</tr></trans></sense></entryFree>
<entryFree id="t3" type="main" key="Zzyzx"><orth extent="full" lang="la">Zzyzx</orth>, <itype>i</itype>, <gen>m.</gen> <sense id="t3.0" n="I" level="1"><hi rend="ital">a word made up for a test</hi></sense></entryFree>
</div0></body></text></TEI.2>
"""
DCC_LA = 'Headword,Definition,"Part of Speech","Semantic Group","Frequency Rank"\n' \
         '"amō -āre","love, like","Verb: 1st Conjugation",Emotions,10\n' \
         '"aqua -ae f.",water,"Noun: 1st Declension",Nature,20\n' \
         '"puella -ae f.",girl,"Noun: 1st Declension",People,30\n'
DCC_LA_ES = 'Headword,Definiciones,"Clase de palabra","Campo semántico","Rango de frecuencia"\n' \
            '"amō -āre","amar, querer","Verbo: 1ª conjugación",Emociones,10\n' \
            '"puella -ae f.","niña, muchacha","Sustantivo: 1ª declinación",Personas,30\n'
DCC_GRC = 'Headword,DEFINITION,"Part of Speech","SEMANTIC GROUP","FREQUENCY RANK"\n' \
          '"καί",and,conjunction,Conjunctions,3\n'
INFLECTS = ["N 1 1 NOM S C  1 1 a   X A", "N 1 1 GEN S C  1 2 ae  X A", "V 1 1 PRES ACTIVE  IND  1 S  1 1 o X A"]
EN_TRANS_LA = [("water", "noun", "H2O", "aqua", "feminine"), ("love", "verb", "to have affection", "amō", ""),
               ("girl", "noun", "young female", "puella", "feminine"), ("Rome", "name", "city", "Rōma", "")]
EN_TRANS_ES = [("water", "noun", "H2O", "agua", ""), ("water", "noun", "liquid", "agua", ""),
               ("water", "noun", "H2O", "linfa", ""), ("love", "verb", "affection", "amar", ""),
               ("girl", "noun", "young female", "niña", "")]
EN_LEMMAS = ["water", "love", "girl", "like", "maiden", "sweetheart", "rome", "fond", "sea"]
EN_FORMS = [("girls", "girl"), ("loves", "love"), ("loved", "love")]
ES_LEMMAS = ["agua", "amar", "niña", "muchacha", "querer"]
ES_TRANS_LA = [("agua", "noun", "1", "aqua", "")]
ES_GLOSSES = [("aqua", "noun", "Agua.", "la", "1", "")]
CURATED = {
    "tiers_la.tsv": "# key\thead\tpos\ttier\tsource\tnote\namo\tamō\tverb\t1\tteacher\t\n",
    "gloss_es_la.tsv": "# key\thead\tgloss_es\npuella\tpuella\tniña\n",
    "valency_la.tsv": "# key\tframe\texample\tnote\namo\tacc\tpuellam amat\t\n",
    "emoji_la.tsv": "# key\thead\temoji\tnote\naqua\taqua\t\U0001F4A7\t\nnix\tnix\t❄️\tnot in the fixture\n",
}


def tsv(path, rows):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        for r in rows:
            f.write("\t".join(str(x) for x in r) + "\n")


def text(path, s):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(s)


def write_inputs(root):
    """Writes raw/, work/ and curated/ under root."""
    raw, work, cur = os.path.join(root, "raw"), os.path.join(root, "work"), os.path.join(root, "curated")
    for d in (raw, work, cur):
        if os.path.isdir(d):
            shutil.rmtree(d)
    text(os.path.join(raw, "aux", "whitaker", "DICTLINE.GEN"), "".join(dictline(*r) + "\r\n" for r in DICTLINE))
    text(os.path.join(raw, "aux", "whitaker", "INFLECTS.LAT"), "".join(x + "\r\n" for x in INFLECTS))
    text(os.path.join(raw, "aux", "perseus", "lat.ls.perseus-eng1.xml"), LS_XML)
    text(os.path.join(raw, "aux", "dcc", "latin-core-list.csv"), DCC_LA)
    text(os.path.join(raw, "aux", "dcc", "latin-core-list-es.csv"), DCC_LA_ES)
    text(os.path.join(raw, "aux", "dcc", "greek-core-list.csv"), DCC_GRC)
    la = os.path.join(work, "la")
    text(os.path.join(la, "lemmas.jsonl"),
         "".join(json.dumps(r, ensure_ascii=False, separators=(",", ":")) + "\n" for r in LEMMAS))
    rows = [(lid, disp, vptext.latin_key(disp), tags, "declension" if POS_OF[lid] != V else "conjugation", "")
            for lid, disp, tags in TABLE]
    tsv(os.path.join(la, "table_forms.tsv"), rows)
    tsv(os.path.join(la, "formpages.tsv"), [])
    anal = set()
    for lid, disp, tags in TABLE:
        anal.add((vptext.latin_key(disp), lid, P(POS_OF[lid], **FEATS[tags]), disp, FLAGS.get((lid, disp, tags), 1)))
    tsv(os.path.join(la, "analyses.tsv"), sorted(anal, key=lambda r: (r[0].encode("utf-8"), r[1], r[2], r[3])))
    tsv(os.path.join(la, "lemma_index.tsv"), [(r["id"], r["key"], r["head"], r["pos"], 1, r["kind"], r["ht"])
                                              for r in LEMMAS])
    en = os.path.join(work, "en")
    tsv(os.path.join(en, "translations_la.tsv"), EN_TRANS_LA)
    tsv(os.path.join(en, "translations_es.tsv"), EN_TRANS_ES)
    tsv(os.path.join(en, "lemma_index.tsv"), [(i, w, w, "noun", 0, "lemma", "") for i, w in enumerate(EN_LEMMAS)])
    tsv(os.path.join(en, "analyses.tsv"), sorted((f, EN_LEMMAS.index(l), 1, f, 2) for f, l in EN_FORMS))
    es = os.path.join(work, "es")
    tsv(os.path.join(es, "translations_la.tsv"), ES_TRANS_LA)
    tsv(os.path.join(es, "latin_glosses.tsv"), ES_GLOSSES)
    tsv(os.path.join(es, "lemma_index.tsv"), [(i, w, w, "noun", 0, "lemma", "") for i, w in enumerate(ES_LEMMAS)])
    tsv(os.path.join(es, "analyses.tsv"), [])
    for name, body in CURATED.items():
        text(os.path.join(cur, name), body)


def run_pipeline(root, out_root):
    """Copies root/work to out_root and runs import_aux, gloss, tiers and pack for la. Returns the .vpl bytes."""
    import gloss
    import import_aux
    import pack
    import tiers
    out = os.path.join(out_root, "work")
    shutil.copytree(os.path.join(root, "work"), out)
    raw = os.path.join(root, "raw")
    cur = os.path.join(root, "curated")
    res = {"import_aux": import_aux.run("la", os.path.join(raw, "aux"), out),
           "gloss": gloss.run("la", out, cur), "tiers": tiers.run("la", out, cur),
           "pack": pack.run("la", out, raw, curated=cur)}
    with open(os.path.join(out, "latin.vpl"), "rb") as f:
        return f.read(), out, res


def main(argv):
    check = "--check" in argv
    if not check:
        write_inputs(SPEC)
    tmp = tempfile.mkdtemp(prefix="vpl_spec-")
    try:
        data, _, _ = run_pipeline(SPEC, tmp)
    finally:
        shutil.rmtree(tmp)
    target = os.path.join(SPEC, "latin.vpl")
    if check:
        with open(target, "rb") as f:
            same = f.read() == data
        print("latin.vpl %s" % ("unchanged" if same else "WOULD CHANGE"))
        return 0 if same else 1
    with open(target, "wb") as f:
        f.write(data)
    print("wrote %s (%d bytes)" % (target, len(data)))
    return 0


if __name__ == "__main__":
    import common
    common.QUIET = True
    sys.exit(main(sys.argv[1:]))
