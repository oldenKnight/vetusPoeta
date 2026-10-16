#!/usr/bin/env python3
"""Compare pack.encode() with B1's reference encoder: tests/fixtures/lex/SPEC_CHECK.md (offsets, sizes, SHA-256)
and, when present, the committed fixture bytes tests/fixtures/lex/{latin,greek,english}.vpl.

  python3 tools/build_library/tests/compare_spec.py        # exit 0 when everything matches
Prints one line per file and the first differing byte when the bytes differ.
"""
import hashlib
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import _paths  # noqa: E402
import pack  # noqa: E402
import spec_fixtures  # noqa: E402

LEX = os.path.join(_paths.FIXTURES, "lex")


def parse_spec_check(path):
    """{file: {"size": int, "sha256": str, "sections": [(tag, offset, length)]}}"""
    res = {}
    cur = None
    with open(path, encoding="utf-8") as f:
        for line in f:
            m = re.match(r"^## (\S+\.vpl)", line)
            if m:
                cur = res.setdefault(m.group(1), {"sections": []})
                continue
            if cur is None:
                continue
            m = re.match(r"^- file size: (\d+) bytes", line)
            if m:
                cur["size"] = int(m.group(1))
            m = re.match(r"^- SHA-256 of the whole file: `([0-9a-f]{64})`", line)
            if m:
                cur["sha256"] = m.group(1)
            m = re.match(r"^\| ([A-Z]{4}) \| (\d+) \| (\d+) \|", line)
            if m:
                cur["sections"].append((m.group(1), int(m.group(2)), int(m.group(3))))
    return res


def compare(verbose=True):
    spec_path = os.path.join(LEX, "SPEC_CHECK.md")
    if not os.path.exists(spec_path):
        if verbose:
            print("no %s: nothing to compare" % spec_path)
        return None
    spec = parse_spec_check(spec_path)
    problems = []
    for name, make in sorted(spec_fixtures.ALL.items()):
        layout = []
        data = pack.encode(make(), layout)
        exp = spec.get(name)
        if exp is None:
            problems.append("%s: not in SPEC_CHECK.md" % name)
            continue
        sha = hashlib.sha256(data).hexdigest()
        line = "%s: size %d (spec %s), sha256 %s" % (name, len(data), exp.get("size"),
                                                      "match" if sha == exp.get("sha256") else "DIFFERS")
        if layout != exp["sections"]:
            problems.append("%s: sections %s != spec %s" % (name, layout, exp["sections"]))
        if len(data) != exp.get("size") or sha != exp.get("sha256"):
            problems.append(line)
        ref = os.path.join(LEX, name)
        if os.path.exists(ref):
            with open(ref, "rb") as f:
                rb = f.read()
            if rb != data:
                i = next((k for k in range(min(len(rb), len(data))) if rb[k] != data[k]), min(len(rb), len(data)))
                problems.append("%s: bytes differ from the committed fixture at offset %d" % (name, i))
            else:
                line += ", bytes identical to tests/fixtures/lex/%s" % name
        if verbose:
            print(line)
    return problems


if __name__ == "__main__":
    p = compare()
    if p is None:
        sys.exit(0)
    for x in p:
        print("MISMATCH " + x)
    sys.exit(1 if p else 0)
