#!/usr/bin/env python3
"""Lemma coverage of an English/Spanish text or subtitle file by a morphology lexicon (english.vpl / spanish.vpl).

    python3 tools/build_library/coverage.py data/work/english.vpl tests/regression/own_dialogue.en.txt [more files]
    python3 tools/build_library/coverage.py --gloss-en data/work/spanish.vpl tests/regression/own_dialogue.es.txt

Prints NUMBERS ONLY (token and type counts, unknown counts, coverage %), never a word of the text, so it may be run on
the held-out files (tests/heldout/README.md: only numbers may leave them). A token is a run of letters (with inner
apostrophes / hyphens); it is known when the lexicon has an analysis for its key (en_key / es_key), for its lower-case
key, or, for a token with an apostrophe or a hyphen, for its first part (don't -> don, cat's -> cat). Subtitle files:
cue numbers, timing lines and tags (<i>, {\\an8}) are skipped.
--gloss-en (B4c) also counts the known tokens / types of which at least one analysed lemma carries an English gloss
(LEMM.gloss_en; the Spanish->Latin English pivot needs one) and the share of all lemmas in the file that have one.
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


def has_gloss_en(reader, analyses):
    return any(reader.lemma(a["lemma"])["gloss_en"] for a in analyses)


def coverage(reader, path, key, gloss=None):
    """gloss: a dict that receives the --gloss-en counts (tokens, types)."""
    tokens = unknown = unknown_cap = 0
    types, unknown_types = set(), set()
    gl_types = set()
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
            found = [a for c in cands if c for a in reader.lookup(c)]
            if not found:
                unknown += 1
                unknown_types.add(k)
                if w[:1].isupper():
                    unknown_cap += 1
            elif gloss is not None and has_gloss_en(reader, found):
                gloss["tokens"] = gloss.get("tokens", 0) + 1
                gl_types.add(k)
    if gloss is not None:
        gloss["types"] = len(gl_types)
    return tokens, unknown, len(types), len(unknown_types), unknown_cap


def main(argv):
    gl = "--gloss-en" in argv
    argv = [a for a in argv if a != "--gloss-en"]
    if len(argv) < 2:
        sys.stderr.write(__doc__)
        return 2
    r = VplReader(argv[0])
    if gl:
        n = sum(1 for i in range(r.n_lemmas) if r.lemma(i)["gloss_en"])
        sys.stdout.write("%s\tlemmas %d\twith gloss_en %d (%.2f %%)\n" % (os.path.basename(argv[0]), r.n_lemmas, n,
                                                                       100.0 * n / max(1, r.n_lemmas)))
    key = vptext.en_key if r.lang == "en" else vptext.es_key
    try:
        for path in argv[1:]:
            g = {} if gl else None
            t, u, ty, uty, ucap = coverage(r, path, key, g)
            cov = 100.0 * (t - u) / t if t else 0.0
            tcov = 100.0 * (ty - uty) / ty if ty else 0.0
            sys.stdout.write("%s\t%s\ttokens %d\tunknown %d (capitalised %d)\tcoverage %.2f %%\ttypes %d\t"
                             "unknown types %d\ttype coverage %.2f %%\n" % (os.path.basename(argv[0]),
                                                                           os.path.basename(path), t, u, ucap, cov,
                                                                           ty, uty, tcov))
            if gl:
                gt, gty = g.get("tokens", 0), g.get("types", 0)
                sys.stdout.write("%s\t%s\tgloss_en: tokens %d of %d known (%.2f %%)\ttypes %d of %d known "
                                 "(%.2f %%)\n" % (os.path.basename(argv[0]), os.path.basename(path), gt, t - u,
                                                 100.0 * gt / max(1, t - u), gty, ty - uty,
                                                 100.0 * gty / max(1, ty - uty)))
    finally:
        r.close()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
