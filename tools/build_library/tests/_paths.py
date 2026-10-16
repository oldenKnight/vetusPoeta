"""Test helper: import path and fixture locations."""
import os
import sys

TESTS = os.path.dirname(os.path.abspath(__file__))
LIB = os.path.dirname(TESTS)
ROOT = os.path.normpath(os.path.join(LIB, "..", ".."))
FIXTURES = os.path.join(ROOT, "tests", "fixtures")
KAIKKI_FIXTURES = os.path.join(FIXTURES, "kaikki")
if LIB not in sys.path:
    sys.path.insert(0, LIB)

import common  # noqa: E402

common.QUIET = True
