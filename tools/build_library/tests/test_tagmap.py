import os
import subprocess
import sys
import unittest

import _paths  # noqa: F401
import features as F
import tagmap


class TagmapTest(unittest.TestCase):
    def test_golden_file(self):
        path = os.path.join(_paths.FIXTURES, "features_golden.tsv")
        with open(path, encoding="utf-8") as f:
            rows = [line.rstrip("\n").split("\t") for line in f]
        self.assertGreaterEqual(len(rows), 60)
        for tags, val in rows:
            toks = tags.split()
            self.assertTrue(toks[0].startswith("pos="), tags)
            pos = toks[0][4:]
            self.assertIn(pos, F.POS)
            self.assertEqual(toks[1:], sorted(toks[1:]), "tags must be sorted")
            self.assertEqual(tagmap.tags_to_features(toks[1:], pos), int(val), tags)

    def test_hand_packed(self):
        # computed by hand from the bit layout in features.py
        self.assertEqual(tagmap.tags_to_features(["nominative", "singular"], "noun"), 1 + (1 << 5) + (1 << 9))
        self.assertEqual(tagmap.tags_to_features(["comparative"], "adj"), 3 + (2 << 25))
        self.assertEqual(tagmap.tags_to_features(
            ["active", "indicative", "perfect", "singular", "third-person"], "verb"),
            2 + (1 << 9) + (3 << 14) + (4 << 16) + (1 << 20) + (1 << 23))
        self.assertEqual(tagmap.tags_to_features(["accusative", "supine"], "verb"), 2 + (4 << 5) + (1 << 27))
        self.assertEqual(tagmap.tags_to_features(["ablative", "feminine", "masculine", "neuter", "plural"], "adj"),
                         3 + (5 << 5) + (2 << 9) + (7 << 11))
        self.assertEqual(tagmap.tags_to_features(["dual", "optative", "second-person"], "verb"),
                         2 + (3 << 9) + (2 << 14) + (7 << 20))

    def test_expansion(self):
        vals = tagmap.tags_to_feature_list(["dative", "genitive", "singular"], "noun")
        self.assertEqual(len(vals), 2)
        self.assertEqual({F.unpack(v)["case"] for v in vals}, {F.CASE["genitive"], F.CASE["dative"]})
        vals = tagmap.tags_to_feature_list(["first-person", "indicative", "middle", "passive", "singular"], "verb")
        self.assertEqual({F.unpack(v)["voice"] for v in vals}, {2, 3})
        self.assertEqual(len(tagmap.tags_to_feature_list(["mediopassive"], "verb")), 2)

    def test_tense_rules(self):
        t = lambda tags: F.unpack(tagmap.tags_to_features(tags, "verb"))["tense"]  # noqa: E731
        self.assertEqual(t(["future", "perfect"]), F.TENSE["future-perfect"])
        self.assertEqual(t(["past", "imperfect"]), F.TENSE["imperfect"])
        self.assertEqual(t(["present", "perfect"]), F.TENSE["perfect"])
        self.assertEqual(t(["past"]), F.TENSE["perfect"])
        self.assertEqual(t(["indicative"]), 0)
        self.assertEqual(F.unpack(tagmap.tags_to_features(["indicative"], "verb", tense_hint="aorist"))["tense"],
                         F.TENSE["aorist"])

    def test_unknown_tags_are_counted_not_raised(self):
        st = tagmap.TagStats()
        vals = tagmap.tags_to_feature_list(["nominative", "zzz-new-tag", "conditional", "Medieval-Latin"], "noun",
                                           stats=st)
        self.assertEqual(len(vals), 1)
        self.assertEqual(st.unknown["zzz-new-tag"], 1)
        self.assertEqual(st.lossy["conditional"], 1)
        self.assertNotIn("Medieval-Latin", st.unknown)
        self.assertEqual(tagmap.tags_to_feature_list(None, "noun", stats=st), [F.POS["noun"]])
        self.assertEqual(tagmap.tags_to_feature_list([], "no-such-pos", stats=st), [F.POS["other"]])

    def test_extra_bits(self):
        u = F.unpack(tagmap.tags_to_features(["Attic", "contracted", "nominative"], "noun"))
        self.assertEqual(u["extra"], F.EXTRA["attic"] | F.EXTRA["contracted"])
        self.assertEqual(F.unpack(tagmap.tags_to_features(["adverbial"], "adj"))["pos"], F.POS["adv"])

    def test_pos_name(self):
        self.assertEqual(tagmap.pos_name("verb", "la-part"), "participle")
        self.assertEqual(tagmap.pos_name("verb", "grc-part-1&3"), "participle")
        self.assertEqual(tagmap.pos_name("verb", "head", "participle form"), "participle")
        self.assertEqual(tagmap.pos_name("name", "la-proper noun"), "name")
        self.assertEqual(tagmap.pos_name("weird", ""), "other")

    def test_golden_files_are_current(self):
        r = subprocess.run([sys.executable, os.path.join(_paths.LIB, "make_golden.py"), "--check"],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        self.assertEqual(r.returncode, 0, r.stdout.decode("utf-8", "replace"))


if __name__ == "__main__":
    unittest.main()
