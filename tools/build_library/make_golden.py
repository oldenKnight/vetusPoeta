"""Regenerate tests/fixtures/normalisation_golden.tsv and tests/fixtures/features_golden.tsv.

Usage: python3 tools/build_library/make_golden.py [--check]
The inputs are listed here; the expected values are computed with vptext.py / tagmap.py and a sample of them is
pinned by hand in tests/test_vptext.py and tests/test_tagmap.py (independent of this script).
Rows never contain tabs, newlines, leading/trailing spaces or empty fields. No header, no comments.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import vptext  # noqa: E402
import tagmap  # noqa: E402

ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
NORM_PATH = os.path.join(ROOT, "tests", "fixtures", "normalisation_golden.tsv")
FEAT_PATH = os.path.join(ROOT, "tests", "fixtures", "features_golden.tsv")

M, B = "\u0304", "\u0306"            # combining macron, breve
AC, GR, CI = "\u0301", "\u0300", "\u0342"   # acute, grave, perispomeni
SM, RO = "\u0313", "\u0314"          # smooth, rough breathing
DI, YP = "\u0308", "\u0345"          # diaeresis, ypogegrammeni

NFC_IN = [
    "a" + M, "e" + M, "i" + M, "o" + M, "u" + M, "y" + M, "A" + M, "E" + M, "I" + M, "O" + M, "U" + M, "Y" + M,
    "a" + B, "e" + B, "i" + B, "o" + B, "u" + B, "A" + B, "Ro" + M + "ma", "Iu" + M + "lius", "ae" + DI,
    "e" + M + AC, "o" + M + GR, "n" + "\u0303" + "o", "a" + AC + "rbol",
    "\u03b1" + SM, "\u03b1" + SM + AC, "\u03b1" + RO + CI, "\u03c9" + SM + YP, "\u03c9" + RO + CI + YP,
    "\u03b7" + CI + YP, "\u03b1" + YP, "\u0391" + SM, "\u0391" + RO + AC, "\u03c1" + RO, "\u03c5" + DI + AC,
    "\u03b9" + DI, "\u03b5" + AC, "\u1f71", "\u1f73", "\u1f79", "\u03b1" + B, "\u03b9" + M, "\u03c5" + B + AC,
    "\u0391" + B, "\u1f00" + AC, "\u03bf" + SM + GR, "\u03b1\u0313\u0301\u0345",
    "\u03c3\u03ba\u1fe0\u0301\u03bb\u03bf\u03c2", "caf\u00e9", "plain", "Wasser",
]

LATIN_IN = [
    "I\u016blius", "Iu" + M + "lius", "vir", "Vir", "iam", "Iam", "juvenis", "Juvenis", "J\u016bpiter", "S\u012bc",
    "sic", "am\u014d", "amo" + M, "am\u0101re", "\u0101", "\u0113", "\u012b", "\u014d", "\u016b", "\u0233",
    "\u0232", "\u0100", "\u0112", "\u012a", "\u014c", "\u016a", "\u0103", "\u0115", "\u012d", "\u014f",
    "\u016d", "\u0102", "Ro" + M + "ma", "R\u014dma", "R\u014dmae", "aqu\u0101\u012b", "th\u0113saurus",
    "\u00e6s", "\u00c6neas", "c\u00e6lum", "\u0153conomia", "\u0152dipus", "p\u0153na", "\u01e3", "\u01e2",
    "c\u0101ns\u0101", "res publica", "r\u0113s p\u016bblica", "r\u0113s-p\u016bblica", "-que", "ne-", "VENI VIDI VICI",
    "v\u0113n\u012b, v\u012bd\u012b, v\u012bc\u012b", "Ave Maria!", "a.d.", "v\u012br\u012b3", "\u0233dor",
    "Ch\u0233", "Hercle'", "po\u00ebta", "a\u00ebr", "\u0101\u0113r", "\u1e17", "\u1e53", "e" + M + AC,
    "Vulcanus", "Valerius", "jejunus", "ejus", "Cujus", "conjux", "vivus", "uva", "VVLGVS", "Iuppiter",
    "tetig\u012b", "t\u0101ct\u016br\u016bm esse", "\u0101\u012b", "Lucius Annaeus Seneca", "amo" + B,
    "puellas", "Puell\u0101s", "dominus\u2019", "x", "Xerx\u0113s", "qu\u014dque",
]

GREEK_IN = [
    "\u1f04\u03bd\u03b8\u03c1\u03c9\u03c0\u03bf\u03c2", "\u1f60\u03b9\u03b4\u03ae", "\u1fa0\u03b4\u03ae",
    "\u1f08\u03b8\u1fc6\u03bd\u03b1\u03b9", "\u1fe5\u03ae\u03c4\u03c9\u03c1", "\u1f51\u03b9\u03cc\u03c2",
    "\u1f51\u03ca\u03cc\u03c2", "\u03bb\u03cc\u03b3\u03bf\u03c2", "\u039b\u039f\u0393\u039f\u03a3",
    "\u039b\u03cc\u03b3\u03bf\u03c2", "\u03c3\u03ba\u1fe0\u0301\u03bb\u03bf\u03c2", "\u03c3\u03ba\u03cd\u03bb\u03bf\u03c2",
    "\u03b8\u03ac\u03bb\u03b1\u03c4\u03c4\u03b1", "\u03b8\u1f71\u03bb\u03b1\u03c4\u03c4\u03b1",
    "\u03b8\u03ac\u03bb\u1fb0\u03c4\u03c4\u1fb0", "\u03c7\u03ce\u03c1\u1fb1", "\u03c7\u03ce\u03c1\u03b1" + M,
    "\u03bd\u03bf\u1fe6\u03c2", "\u03bd\u1ff7", "\u03b2\u03b1\u03c3\u03b9\u03bb\u03b5\u03cd\u03c2",
    "\u1f04\u03bd\u03b1\u03be", "\u1f10\u03c0\u03bf\u03af\u03b7\u03c3\u03b1", "\u03c0\u03bf\u03b9\u03ad\u03c9",
    "\u03c0\u03bf\u03b9\u1ff6", "\u03c4\u03b9\u03bc\u1ff6", "\u03c4\u03b9\u03bc\u1fb6\u03c2", "\u1f21\u03bb\u03af\u03bf\u03c5",
    "\u1f45\u03c2", "\u1f3a\u03bb\u03b9\u03ac\u03c2", "\u1f28\u03c1\u03b1\u03ba\u03bb\u1fc6\u03c2",
    "\u1fbc\u03b9\u03b4\u03b7\u03c2", "\u1fec\u03cc\u03b4\u03bf\u03c2", "\u0391\u1f30\u03bd\u03b5\u03af\u03b1\u03c2",
    "\u03c0\u03c1\u03b1\u03ca\u0301\u03c2", "\u03ba\u03bb\u03b5\u1fd6\u03b8\u03c1\u03bf\u03bd",
    "\u03c4\u1f78 \u1f00\u03b3\u03b1\u03b8\u03cc\u03bd", "\u03b3\u1fd0\u0301\u03b3\u03bd\u03bf\u03bc\u03b1\u03b9",
    "\u03b3\u1fd0\u03b3\u03bd\u03cc\u03bc\u03b5\u03b8\u1fb0", "\u03c0\u03bf\u03b9\u03ad\u03bf\u03c5\u03c3\u1fd0\u03bd",
    "\u03b1" + SM + AC + YP, "\u03c3\u03bf\u03c6\u03af\u1fb1\u03c2", "\u1f00\u03bb\u03ae\u03b8\u03b5\u03b9\u1fb0",
    "\u03a3\u03a9\u039a\u03a1\u0391\u03a4\u0397\u03a3", "\u03c3\u1f7a", "\u1f41", "\u1f21", "\u03c4\u1ff7",
    "\u03b4\u1fbd", "\u039f\u1f50\u03c1\u03b1\u03bd\u03cc\u03c2", "\u03c3\u03b9\u03c3",
]

EN_IN = [
    "Water", "WATER", "don\u2019t", "Don't", "\u2018quoted\u2019", "\u201cDouble\u201d", "\u201eLow\u201f",
    "\u201aa\u201b", "caf\u00e9", "cafe" + AC, "na\u00efve", "New York", "well-being", "O\u2019Brien", "rock 'n' roll",
    "I", "Alice", "x-ray", "r\u00f4le", "\u00c9clair",
]
ES_IN = [
    "ni\u00f1o", "Ni\u00f1o", "NI\u00d1O", "\u00e1rbol", "\u00c1rbol", "a" + AC + "rbol", "pingu\u0308ino",
    "ping\u00fcino", "coraz\u00f3n", "Espa\u00f1a", "\u00bfQu\u00e9?", "\u00a1Hola!", "camin\u00e9", "\u201cdijo\u201d",
    "l\u2019agua", "ac\u00e1", "r\u00edo", "\u00c9l", "a\u00f1o", "ma\u00f1ana",
]
DISPLAY_IN = [
    "am\u014d", "am\u0101re", "I\u016blius", "Ro" + M + "ma", "th\u0113saurus", "\u0233dor", "\u0232", "Am\u012bcus",
    "amo" + B, "\u0103", "\u016d", "\u01e3", "\u1e17", "puella", "vir", "J\u016bpiter", "r\u0113s p\u016bblica",
    "t\u0101ct\u016br\u016bm esse", "-que", "\u00c6neas",
]


def norm_rows():
    rows = []
    for s in NFC_IN:
        rows.append(("nfc", s, vptext.nfc(s)))
    for s in LATIN_IN:
        rows.append(("latin_key", s, vptext.latin_key(s)))
    for s in GREEK_IN:
        rows.append(("greek_key", s, vptext.greek_key(s)))
    for s in GREEK_IN:
        rows.append(("greek_bare", s, vptext.greek_bare(s)))
    for s in EN_IN:
        rows.append(("en_key", s, vptext.en_key(s)))
    for s in ES_IN:
        rows.append(("es_key", s, vptext.es_key(s)))
        rows.append(("es_bare", s, vptext.es_bare(s)))
    for s in DISPLAY_IN:
        rows.append(("display_latin_plain", s, vptext.display_latin(s, False)))
        rows.append(("display_latin_macrons", s, vptext.display_latin(s, True)))
    for f, i, o in rows:
        assert i and o and "\t" not in i + o and "\n" not in i + o and i == i.strip() and o == o.strip(), (f, i, o)
    return rows


FEAT_IN = [
    # (tags, pos) ; pos is a features.POS name
    ("accusative plural masculine", "adj"), ("accusative plural masculine", "noun"), ("nominative singular", "noun"),
    ("genitive singular", "noun"), ("dative plural", "noun"), ("ablative singular", "noun"), ("singular vocative", "noun"),
    ("locative singular", "noun"), ("dual nominative", "noun"), ("dative dual", "noun"),
    ("ablative feminine masculine neuter plural", "adj"), ("feminine masculine nominative singular", "adj"),
    ("masculine neuter genitive singular", "adj"), ("feminine neuter plural", "adj"),
    ("accusative neuter plural", "adj"), ("comparative", "adj"), ("superlative", "adj"), ("comparative", "adv"),
    ("comparative masculine nominative singular", "adj"), ("positive", "adj"),
    ("active indicative perfect singular third-person", "verb"),
    ("active first-person indicative present singular", "verb"),
    ("active indicative plural present second-person", "verb"),
    ("active imperfect indicative plural third-person", "verb"),
    ("first-person future indicative passive singular", "verb"),
    ("active first-person indicative pluperfect plural", "verb"),
    ("active future-perfect indicative second-person singular", "verb"),
    ("active present singular subjunctive third-person", "verb"),
    ("imperfect passive plural subjunctive third-person", "verb"),
    ("active imperative present second-person singular", "verb"),
    ("active future imperative singular third-person", "verb"),
    ("active infinitive present", "verb"), ("infinitive passive perfect", "verb"), ("active future infinitive", "verb"),
    ("active participle present", "verb"), ("participle passive perfect", "verb"), ("future participle passive", "verb"),
    ("participle perfect passive nominative singular neuter", "verb"),
    ("accusative gerund noun-from-verb", "verb"), ("genitive gerund noun-from-verb", "verb"),
    ("accusative noun-from-verb supine", "verb"), ("ablative noun-from-verb supine", "verb"),
    ("aorist first-person indicative middle plural", "verb"), ("active aorist indicative singular third-person", "verb"),
    ("active dual indicative second-person", "verb"), ("active first-person optative singular", "verb"),
    ("aorist optative passive plural third-person", "verb"), ("active aorist infinitive", "verb"),
    ("active aorist masculine participle", "verb"), ("imperative middle second-person singular", "verb"),
    ("first-person indicative middle perfect singular", "verb"), ("dual optative second-person", "verb"),
    ("first-person plural present subjunctive", "verb"), ("optative", "verb"), ("dual", "noun"),
    ("feminine genitive plural", "participle"), ("masculine nominative singular", "participle"),
    ("plural", "noun"), ("third-person singular present", "verb"), ("participle past", "verb"),
    ("participle present", "verb"), ("past", "verb"), ("imperfect indicative singular third-person", "verb"),
    ("first-person indicative perfect present singular", "verb"), ("gerund", "verb"), ("infinitive", "verb"),
    ("feminine plural", "adj"), ("masculine plural", "noun"),
]


def feat_rows():
    rows = []
    for tags, pos in FEAT_IN:
        t = " ".join(sorted(tags.split()))
        rows.append(("pos=%s %s" % (pos, t), str(tagmap.golden_features(t.split(), pos))))
    return rows


def write(path, rows):
    data = "".join("\t".join(r) + "\n" for r in rows)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(data)
    return data


def main():
    check = "--check" in sys.argv
    ok = True
    for path, rows in ((NORM_PATH, norm_rows()), (FEAT_PATH, feat_rows())):
        data = "".join("\t".join(r) + "\n" for r in rows)
        if check:
            with open(path, encoding="utf-8") as f:
                same = f.read() == data
            print("%s %s" % ("OK  " if same else "DIFF", path))
            ok = ok and same
        else:
            write(path, rows)
            print("wrote %d rows to %s" % (len(rows), path))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
