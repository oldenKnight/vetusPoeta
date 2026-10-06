#!/usr/bin/env python3
"""Writes the golden files the C++ decoders are tested against (tests/fixtures/nlp/).

    python3 tools/train/make_golden.py --lang en --tag data/work/nlp/english.tag.vpt \
        --dep data/work/nlp/english.dep.vpt --out tests/fixtures/nlp/en.golden.tsv
    python3 tools/train/make_golden.py --lang en --tag tests/fixtures/nlp/tiny.tag.vpt \
        --dep tests/fixtures/nlp/tiny.dep.vpt --out tests/fixtures/nlp/tiny.golden.tsv \
        --features tests/fixtures/nlp/features_golden.tsv

Golden format: header lines start with "## " (model SHA-256s); then one token per line
"word<TAB>upos<TAB>feats<TAB>head<TAB>deprel", a blank line after each sentence. Sentences: the first 200 dev
sentences (Spanish: the first 100 of GSD dev and the first 100 of AnCora dev).
"""
import argparse
import hashlib
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import common  # noqa: E402
import features as F  # noqa: E402
import train_parser  # noqa: E402
import train_tagger  # noqa: E402
import vpt  # noqa: E402


def sha(path):
    with open(path, "rb") as fh:
        return hashlib.sha256(fh.read()).hexdigest()


def golden_sentences(lang, data, n):
    dev, ranges, _ = common.load(lang, data, "dev")
    if len(ranges) == 1:
        return dev[:n]
    per = n // len(ranges)
    out = []
    for _name, (lo, hi) in ranges.items():
        out.extend(dev[lo:min(hi, lo + per)])
    return out


def feature_rows(sents, tagged):
    """~440 feature strings from real contexts + edge-case strings for the hash, 500 rows in all."""
    rows = []
    for si, (s, (tags, _feats)) in enumerate(zip(sents, tagged)):
        words = s["words"]
        lows = [F.norm(w) for w in words]
        shapes = [F.shape(w) for w in words]
        for i in range(len(words)):
            t1 = tags[i - 1] if i >= 1 else F.BOS[0]
            t2 = tags[i - 2] if i >= 2 else (F.BOS[0] if i == 1 else F.BOS[1])
            for f in F.tag_features(words, lows, shapes, i, t1, t2):
                rows.append(("tag:%d:%d" % (si, i), f))
            for f in F.tag_feat_extra(tags[i], lows[i]):
                rows.append(("tagx:%d:%d" % (si, i), f))
            if len(rows) >= 300:
                break
        if len(rows) >= 300:
            break
    for si in range(3):
        s = sents[si]
        tags = tagged[si][0]
        n = len(s["words"])
        lows = [F.ROOT] + [F.norm(w) for w in s["words"]]
        tg = [F.ROOT] + list(tags)
        st = train_parser.State(n)
        for f in st.feats(lows, tg):
            rows.append(("dep:%d" % si, f))
        if n >= 2:
            for f in F.label_features(lows, tg, 2, 1, "L", st.labels, st.lc1, st.rc1):
                rows.append(("lab:%d" % si, f))
    extra = ["", "b", "a", "é", "É", "lw=ÆSIR", "lw=straße", "lw=ﬁne", "sh=Xx", "t-1t-2=-BOS- -BOS2-",
             "lw=" + "x" * 300, "lw=日本語", "lw=😀", "lw=ǅ", "lw=İstanbul", "w=ΟΔΟΣ", "s3=ς", "p1=¿", "lw=¡hola!",
             "lw=don't", "lw=“quoted”", "lw=…", "lw=á", "n0w=-NONE-", "s0w=-ROOT-", "lw=tab\x01",
             "lw=multi word", "sh=d,d", "sh=Xx-Xx", "lw=ñandú"]
    i = 0
    while len(rows) + len(extra) < 500:
        extra.append("x%d=%s" % (i, "ab" * (i % 7)))
        i += 1
    for e in extra[:500 - len(rows)]:
        rows.append(("hash", e))
    return rows[:500]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--lang", required=True, choices=["en", "es"])
    ap.add_argument("--data", default="data/raw/ud")
    ap.add_argument("--tag", required=True)
    ap.add_argument("--dep", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--n", type=int, default=200)
    ap.add_argument("--features", default="")
    a = ap.parse_args()
    sents = golden_sentences(a.lang, a.data, a.n)
    tm = vpt.Model(a.tag)
    dm = vpt.Model(a.dep)
    layout = train_tagger.Layout.from_labels(tm.labels)
    root_idx = dm.labels.index("root")
    tagged = [train_tagger.tag_sentence(s["words"], tm.scores, layout) for s in sents]
    with open(a.out, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("## lang=%s sentences=%d\n" % (a.lang, len(sents)))
        names = ", ".join("%s (%s)" % (name, lic) for _k, name, lic, _att in common.TREEBANKS[a.lang])
        fh.write("## attribution: words and tokenisation from the dev sets of %s, "
                 "https://universaldependencies.org; tags, features and trees predicted by the model\n" % names)
        fh.write("## tag_sha256=%s\n" % sha(a.tag))
        fh.write("## dep_sha256=%s\n" % sha(a.dep))
        for s, (tags, feats) in zip(sents, tagged):
            heads, deps = train_parser.parse_sentence(s["words"], tags, dm.scores, dm.labels, root_idx)
            for w, t, f, h, d in zip(s["words"], tags, feats, heads, deps):
                assert "\t" not in w and "\n" not in w
                fh.write("%s\t%s\t%s\t%d\t%s\n" % (w, t, f, h, d))
            fh.write("\n")
    if a.features:
        with open(a.features, "w", encoding="utf-8", newline="\n") as fh:
            for ctx, f in feature_rows(sents, tagged):
                assert "\t" not in f and "\n" not in f
                fh.write("%s\t%s\t%016x\n" % (ctx, f, F.fnv1a64(f)))
    print("wrote", a.out)


if __name__ == "__main__":
    main()
