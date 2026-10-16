import contextlib
import gzip
import io
import json
import os
import shutil
import tempfile
import unittest

import _paths
import build
import common
from common import RAW_FILES


def load(path):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


class DriftTest(unittest.TestCase):
    def test_within_and_beyond_15_percent(self):
        exp = {"la": {"resolve.analyses": 1000, "kaikki.entries": 200}}
        self.assertEqual(build.drift_errors(exp, {"la": {"resolve.analyses": 1150, "kaikki.entries": 170}}), [])
        errs = build.drift_errors(exp, {"la": {"resolve.analyses": 1151, "kaikki.entries": 200}})
        self.assertEqual(len(errs), 1)
        self.assertIn("resolve.analyses", errs[0])
        errs = build.drift_errors(exp, {"la": {"resolve.analyses": 849, "kaikki.entries": 200}})
        self.assertEqual(len(errs), 1)

    def test_missing_key_fails(self):
        exp = {"la": {"resolve.analyses": 1000}, "grc": {"kaikki.entries": 5}}
        errs = build.drift_errors(exp, {"la": {}, "grc": {"kaikki.entries": 5}})
        self.assertEqual(len(errs), 1)
        self.assertIn("missing", errs[0])
        # whole language missing
        self.assertEqual(len(build.drift_errors(exp, {"grc": {"kaikki.entries": 5}})), 1)

    def test_language_filter_and_meta_keys(self):
        exp = {"_rule": "text", "la": {"resolve.analyses": 1000}, "en": {"resolve.analyses": 10}}
        cur = {"la": {"resolve.analyses": 1000}}
        self.assertEqual(build.drift_errors(exp, cur, langs=["la"]), [])
        self.assertEqual(len(build.drift_errors(exp, cur)), 1)

    def test_expected_from_selects_checked_counts(self):
        cur = {"la": {"kaikki.entries": 892320, "kaikki.lines": 892320, "resolve.analyses": 50,
                      "resolve.distinct_keys": 1000}}
        exp = build.expected_from(cur)
        self.assertEqual(exp["la"], {"kaikki.entries": 892320, "resolve.distinct_keys": 1000})
        self.assertIn("_rule", exp)


class StageCacheTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="vp-build-")
        self.raw = os.path.join(self.tmp, "raw")
        self.out = os.path.join(self.tmp, "work")
        os.makedirs(self.raw)
        for lang in ("la", "grc", "en"):
            shutil.copy(os.path.join(_paths.KAIKKI_FIXTURES, lang + ".jsonl"), os.path.join(self.raw, RAW_FILES[lang]))
        with open(os.path.join(_paths.KAIKKI_FIXTURES, "es.jsonl"), "rb") as src:
            with gzip.open(os.path.join(self.raw, RAW_FILES["es"]), "wb") as dst:
                dst.write(src.read())

    def tearDown(self):
        shutil.rmtree(self.tmp)

    def run_build(self, *args):
        err = io.StringIO()
        out = io.StringIO()
        common.QUIET = False
        try:
            with contextlib.redirect_stderr(err), contextlib.redirect_stdout(out):
                rc = build.main(["--raw", self.raw, "--out", self.out] + list(args))
        finally:
            common.QUIET = True
        return rc, out.getvalue(), err.getvalue()

    def test_cache_force_check_report(self):
        rc, _, err = self.run_build("--lang", "la,grc,en,es", "--stage", "kaikki,resolve")
        self.assertEqual(rc, 0)
        meta = load(os.path.join(self.out, "la", "kaikki.json"))
        self.assertEqual(meta["counts"]["entries"], 12)
        self.assertIn("kaikki-Latin.jsonl", meta["inputs"])
        self.assertEqual(len(meta["inputs"]["kaikki-Latin.jsonl"]["sha256"]), 64)
        mtime = os.path.getmtime(os.path.join(self.out, "la", "analyses.tsv"))
        # second run: skipped
        rc, _, err = self.run_build("--lang", "la", "--stage", "kaikki,resolve")
        self.assertIn("[la kaikki] inputs unchanged, skipped", err)
        self.assertIn("[la resolve] inputs unchanged, skipped", err)
        self.assertEqual(os.path.getmtime(os.path.join(self.out, "la", "analyses.tsv")), mtime)
        # --force reruns
        rc, _, err = self.run_build("--lang", "la", "--stage", "resolve", "--force")
        self.assertIn("[la resolve] done", err)
        # changed input reruns kaikki and then resolve
        with open(os.path.join(self.raw, RAW_FILES["la"]), "a", encoding="utf-8") as f:
            f.write('{"word": "rosa", "lang_code": "la", "pos": "noun", "senses": [{"glosses": ["rose"]}]}\n')
        rc, _, err = self.run_build("--lang", "la", "--stage", "kaikki,resolve")
        self.assertIn("[la kaikki] done", err)
        self.assertIn("[la resolve] done", err)
        # expected counts round trip and drift check
        exp_path = os.path.join(self.tmp, "expected.json")
        rc, _, _ = self.run_build("--write-expected", "--expected", exp_path)
        self.assertEqual(rc, 0)
        rc, _, err = self.run_build("--check", "--expected", exp_path)
        self.assertEqual(rc, 0, err)
        exp = load(exp_path)
        exp["la"]["kaikki.missing_count"] = 500
        with open(exp_path, "w") as f:
            json.dump(exp, f)
        rc, _, err = self.run_build("--check", "--expected", exp_path)
        self.assertEqual(rc, 1)
        self.assertIn("missing", err)
        # report
        rc, out, _ = self.run_build("--report")
        rep = json.loads(out)
        la = rep["langs"]["la"]
        for k in ("entries", "lemma_entries", "form_pages", "table_rows", "analyses", "distinct_keys",
                  "analyses_per_key", "unresolved_form_pages", "unknown_tags", "top_pos", "time_s", "peak_rss_mb"):
            self.assertIn(k, la)
        self.assertEqual(la["entries"], 13)
        self.assertEqual(rep["langs"]["en"]["kaikki_counts"]["translations_la"], 3)


if __name__ == "__main__":
    unittest.main()
