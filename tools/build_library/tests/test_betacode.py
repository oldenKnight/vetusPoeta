import os
import unicodedata
import unittest

import _paths
import betacode

GOLDEN = os.path.join(_paths.TESTS, "betacode_golden.tsv")


def golden_rows():
    with open(GOLDEN, encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\n")
            if not line or line.startswith("#"):
                continue
            beta, uni, note = (line.split("\t") + ["", ""])[:3]
            yield beta, uni, note


class BetaCodeGolden(unittest.TestCase):
    def test_golden_table(self):
        rows = list(golden_rows())
        self.assertGreaterEqual(len(rows), 100)
        bad = []
        for beta, uni, note in rows:
            got = betacode.beta_to_unicode(beta)
            if got != unicodedata.normalize("NFC", uni):
                bad.append("%s -> %s (expected %s; %s)" % (beta, got, uni, note))
        self.assertEqual(bad, [])

    def test_output_is_nfc(self):
        for beta, _, _ in golden_rows():
            got = betacode.beta_to_unicode(beta)
            self.assertEqual(got, unicodedata.normalize("NFC", got))

    def test_strip_key_digits(self):
        self.assertEqual(betacode.strip_key_digits("a)/gw1"), ("a)/gw", "1"))
        self.assertEqual(betacode.strip_key_digits("lo/gos"), ("lo/gos", ""))
        self.assertEqual(betacode.strip_key_digits("A12"), ("A", "12"))


if __name__ == "__main__":
    unittest.main()
