import os
import unittest

import _paths  # noqa: F401
import vptext

FUNCS = {
    "nfc": vptext.nfc, "latin_key": vptext.latin_key, "greek_key": vptext.greek_key, "greek_bare": vptext.greek_bare,
    "en_key": vptext.en_key, "es_key": vptext.es_key, "es_bare": vptext.es_bare,
    "display_latin_plain": lambda s: vptext.display_latin(s, False),
    "display_latin_macrons": lambda s: vptext.display_latin(s, True),
}

# pinned by hand, independent of make_golden.py
HAND = [
    ("latin_key", "I\u016blius", "iulius"), ("latin_key", "vir", "uir"), ("latin_key", "iam", "iam"),
    ("latin_key", "juvenis", "iuuenis"), ("latin_key", "S\u012bc", "sic"), ("latin_key", "\u0233", "y"),
    ("latin_key", "c\u00e6lum", "caelum"), ("latin_key", "p\u0153na", "poena"), ("latin_key", "Ro\u0304ma", "roma"),
    ("latin_key", "r\u0113s-p\u016bblica", "res-publica"), ("latin_key", "a.d.", "ad"),
    ("greek_key", "\u03bb\u03cc\u03b3\u03bf\u03c2", "\u03bb\u03cc\u03b3\u03bf\u03c3"),
    ("greek_key", "\u03c3\u03ba\u1fe0\u0301\u03bb\u03bf\u03c2", "\u03c3\u03ba\u03cd\u03bb\u03bf\u03c3"),
    ("greek_key", "\u1f08\u03b8\u1fc6\u03bd\u03b1\u03b9", "\u1f00\u03b8\u1fc6\u03bd\u03b1\u03b9"),
    ("greek_bare", "\u1fa0\u03b4\u03ae", "\u03c9\u03b4\u03b7"),
    ("greek_bare", "\u1f04\u03bd\u03b8\u03c1\u03c9\u03c0\u03bf\u03c2", "\u03b1\u03bd\u03b8\u03c1\u03c9\u03c0\u03bf\u03c3"),
    ("greek_bare", "\u1fe5\u03ae\u03c4\u03c9\u03c1", "\u03c1\u03b7\u03c4\u03c9\u03c1"),
    ("greek_bare", "\u1f51\u03ca\u03cc\u03c2", "\u03c5\u03b9\u03bf\u03c3"),
    ("en_key", "Don\u2019t", "don't"), ("en_key", "\u201cA\u201d", '"a"'),
    ("es_key", "Ni\u00f1o", "ni\u00f1o"), ("es_bare", "ni\u00f1o", "nino"), ("es_bare", "\u00c1rbol", "arbol"),
    ("display_latin_plain", "am\u014d", "amo"), ("display_latin_macrons", "am\u014d", "am\u014d"),
    ("nfc", "\u03c9\u0314\u0342\u0345", "\u1fa7"), ("nfc", "\u1f71", "\u03ac"),
]


class GoldenTest(unittest.TestCase):
    def rows(self):
        path = os.path.join(_paths.FIXTURES, "normalisation_golden.tsv")
        with open(path, encoding="utf-8", newline="\n") as f:
            return [line.rstrip("\n").split("\t") for line in f]

    def test_golden_file(self):
        rows = self.rows()
        self.assertGreaterEqual(len(rows), 200)
        for row in rows:
            self.assertEqual(len(row), 3, row)
            fn, inp, exp = row
            self.assertIn(fn, FUNCS)
            self.assertEqual(FUNCS[fn](inp), exp, row)

    def test_every_function_covered(self):
        seen = {r[0] for r in self.rows()}
        self.assertEqual(seen, set(FUNCS))

    def test_hand_pinned(self):
        for fn, inp, exp in HAND:
            self.assertEqual(FUNCS[fn](inp), exp, (fn, inp))

    def test_hand_pinned_rows_are_in_golden(self):
        golden = {tuple(r) for r in self.rows()}
        present = [h for h in HAND if h in golden]
        self.assertGreaterEqual(len(present), 15)

    def test_keys_are_nfc_and_idempotent(self):
        for fn in ("latin_key", "greek_key", "greek_bare", "en_key", "es_key", "es_bare"):
            for _, inp, _ in self.rows():
                out = FUNCS[fn](inp)
                self.assertEqual(vptext.nfc(out), out)
                self.assertEqual(FUNCS[fn](out), out, (fn, inp))


if __name__ == "__main__":
    unittest.main()
