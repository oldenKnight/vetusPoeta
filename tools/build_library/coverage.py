#!/usr/bin/env python3
"""Lemma coverage of an English/Spanish text or subtitle file by a morphology lexicon (english.vpl / spanish.vpl).

    python3 tools/build_library/coverage.py data/work/english.vpl tests/regression/own_dialogue.en.txt [more files]

Prints NUMBERS ONLY (token and type counts, unknown counts, coverage %), never a word of the text, so it may be run on
the held-out files (tests/heldout/README.md: only numbers may leave them). A token is a run of letters (with inner
apostrophes / hyphens); it is known when the lexicon has an analysis for its key (en_key / es_key), for its lower-case
key, or, for a token with an apostrophe or a hyphen, for its first part (don't -> don, cat's -> cat). Subtitle files:
cue numbers, timing lines and tags (<i>, {\\an8}) are skipped.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import vptext  # noqa: E402
from vpl_reader import VplReader  # noqa: E402

WORD = re.compile(r"[^\W\d_]+(?:['’-][^\W\d_]+)*")
TAG = re.compile(r"<[^>]*>|\{[^}]*\}")


def text_lines(path):
    srt = path.endswith((".srt", ".vtt", ".ass"))
    with open(path, encoding="utf-8-sig") as f:
        for line in f:
            line = line.rstrip("\r\n")
            if srt and (line.strip().isdigit() or "-->" in line or line.startswith("WEBVTT")):
                continue
            yield TAG.sub(" ", line)


def coverage(reader, path, key):
    tokens = unknown = unknown_cap = 0
    types, unknown_types = set(), set()
    for line in text_lines(path):
        for m in WORD.finditer(line):
            w = m.group(0)
            k = key(w)
            tokens += 1
            types.add(k)
            cands = [k, key(w.lower())]
            parts = re.split(r"['’-]", w)
            if len(parts) > 1:
                cands.append(key(parts[0]))
            if not any(reader.lookup(c) for c in cands if c):
                unknown += 1
                unknown_types.add(k)
                if w[:1].isupper():
                    unknown_cap += 1
    return tokens, unknown, len(types), len(unknown_types), unknown_cap


def main(argv):
    if len(argv) < 2:
        sys.stderr.write(__doc__)
        return 2
    r = VplReader(argv[0])
    key = vptext.en_key if r.lang == "en" else vptext.es_key
    try:
        for path in argv[1:]:
            t, u, ty, uty, ucap = coverage(r, path, key)
            cov = 100.0 * (t - u) / t if t else 0.0
            tcov = 100.0 * (ty - uty) / ty if ty else 0.0
            sys.stdout.write("%s\t%s\ttokens %d\tunknown %d (capitalised %d)\tcoverage %.2f %%\ttypes %d\t"
                             "unknown types %d\ttype coverage %.2f %%\n" % (os.path.basename(argv[0]),
                                                                           os.path.basename(path), t, u, ucap, cov,
                                                                           ty, uty, tcov))
    finally:
        r.close()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
