#!/usr/bin/env python3
"""Trains the UPOS + morphological-feature tagger and exports it as a .vpt file (DESIGN.md section 17).

    python3 tools/train/train_tagger.py --lang en --data data/raw/ud --out data/work/nlp/english.tag.vpt

Labels: the 17 UPOS tags, then one head per feature group (Number, Person, Tense, VerbForm, Mood, PronType),
each head = "<Group>=_" (absent) followed by the values seen >= 10 times in training. Greedy left to right; the
feature heads use the token's features plus tag_feat_extra(predicted UPOS).
"""
import argparse
import os
import random
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import common  # noqa: E402
from features import BOS, norm, shape, tag_features, tag_feat_extra  # noqa: E402
from perceptron import Perceptron, argmax, quantise  # noqa: E402
import vpt  # noqa: E402

UPOS = ["ADJ", "ADP", "ADV", "AUX", "CCONJ", "DET", "INTJ", "NOUN", "NUM", "PART", "PRON", "PROPN", "PUNCT",
        "SCONJ", "SYM", "VERB", "X"]
GROUPS = ["Number", "Person", "Tense", "VerbForm", "Mood", "PronType"]
VALUE_MAP = {("Number", "Ptan"): "Plur", ("Number", "Coll"): "Sing"}
# Values representable in the packed Token::feats of engine/nlp (vp::nlp::morph); others are not predicted.
KNOWN = {"Number": {"Sing", "Plur", "Dual"}, "Person": {"1", "2", "3"},
         "Tense": {"Pres", "Past", "Fut", "Imp", "Pqp"}, "VerbForm": {"Fin", "Inf", "Part", "Ger", "Conv"},
         "Mood": {"Ind", "Sub", "Imp", "Cnd"},
         "PronType": {"Prs", "Art", "Dem", "Ind", "Int", "Rel", "Neg", "Tot", "Emp", "Rcp", "Exc", "Int,Rel"}}


def feat_values(feats):
    d = {}
    if feats != "_":
        for kv in feats.split("|"):
            k, _, v = kv.partition("=")
            if k in GROUPS:
                d[k] = VALUE_MAP.get((k, v), v)
    return d


def make_labels(train, min_value_count=50):
    counts = {}
    for s in train:
        for f in s["feats"]:
            for k, v in feat_values(f).items():
                counts[(k, v)] = counts.get((k, v), 0) + 1
    labels = list(UPOS)
    heads = []
    for g in GROUPS:
        lo = len(labels)
        labels.append(g + "=_")
        vals = sorted(v for (k, v), c in counts.items() if k == g and c >= min_value_count and v in KNOWN[g])
        labels.extend(g + "=" + v for v in vals)
        heads.append((g, lo, len(labels)))
    return labels, heads


class Layout:
    def __init__(self, labels, heads):
        self.labels = labels
        self.heads = heads
        self.index = {l: i for i, l in enumerate(labels)}

    @staticmethod
    def from_labels(labels):
        """Rebuilds the head ranges from a model's label list (groups are the "<Group>=" runs)."""
        heads = []
        for i, l in enumerate(labels):
            if l.endswith("=_"):
                heads.append([l[:-2], i, i + 1])
            elif "=" in l and heads:
                heads[-1][2] = i + 1
        return Layout(list(labels), [tuple(h) for h in heads])

    def gold(self, s):
        """Gold label indices per token: (upos index, [group label index ...])."""
        up = []
        fe = []
        for u, f in zip(s["upos"], s["feats"]):
            up.append(self.index[u])
            d = feat_values(f)
            row = []
            for g, lo, _hi in self.heads:
                v = d.get(g)
                row.append(self.index.get(g + "=" + v, lo) if v else lo)
            fe.append(row)
        return up, fe


def tag_sentence(words, score_fn, layout):
    """Greedy decoding with any scoring function (float or int model). Returns (upos list, feats strings)."""
    lows = [norm(w) for w in words]
    shapes = [shape(w) for w in words]
    t1, t2 = BOS[0], BOS[1]
    tags = []
    feats_out = []
    nu = len(UPOS)
    for i in range(len(words)):
        f = tag_features(words, lows, shapes, i, t1, t2)
        sc = score_fn(f)
        g = argmax(sc, 0, nu)
        tag = layout.labels[g]
        ex = score_fn(tag_feat_extra(tag, lows[i]))
        parts = []
        for _grp, lo, hi in layout.heads:
            k = argmax([a + b for a, b in zip(sc, ex)], lo, hi)
            lab = layout.labels[k]
            if not lab.endswith("=_"):
                parts.append(lab)
        tags.append(tag)
        feats_out.append("|".join(parts) if parts else "_")
        t2 = t1
        t1 = tag
    return tags, feats_out


def train(sents, layout, epochs, seed, allowed=None, log=print):
    model = Perceptron(len(layout.labels))
    nu = len(UPOS)
    golds = [layout.gold(s) for s in sents]
    pre = [([norm(w) for w in s["words"]], [shape(w) for w in s["words"]]) for s in sents]
    order = list(range(len(sents)))
    rnd = random.Random(seed)
    for ep in range(epochs):
        t0 = time.time()
        rnd.shuffle(order)
        model.counting = ep == 0
        correct = total = 0
        for si in order:
            words = sents[si]["words"]
            lows, shapes = pre[si]
            gup, gfe = golds[si]
            t1, t2 = BOS[0], BOS[1]
            for i in range(len(words)):
                f = tag_features(words, lows, shapes, i, t1, t2)
                if allowed is not None:
                    f = [x for x in f if x in allowed]
                model.count(f)
                sc = model.scores(f)
                g = argmax(sc, 0, nu)
                model.update(gup[i], g, f)
                tag = layout.labels[g]
                ex = tag_feat_extra(tag, lows[i])
                if allowed is not None:
                    ex = [x for x in ex if x in allowed]
                model.count(ex)
                sc = model.scores(ex, sc)
                fx = f + ex
                for (_grp, lo, hi), truth in zip(layout.heads, gfe[i]):
                    model.update(truth, argmax(sc, lo, hi), fx)
                model.tick()
                correct += g == gup[i]
                total += 1
                t2 = t1
                t1 = tag
        log("epoch %d: train acc %.4f (%.1fs)" % (ep + 1, correct / max(total, 1), time.time() - t0))
    return model


def evaluate(sents, score_fn, layout):
    up = fe = tot = 0
    for s in sents:
        tags, feats = tag_sentence(s["words"], score_fn, layout)
        for t, f, gt, gf in zip(tags, feats, s["upos"], s["feats"]):
            up += t == gt
            d = feat_values(gf)
            gold = "|".join("%s=%s" % (g, d[g]) for g in GROUPS
                            if g in d and (g + "=" + d[g]) in layout.index) or "_"
            fe += f == gold
            tot += 1
    return {"upos": round(100.0 * up / max(tot, 1), 2), "feats": round(100.0 * fe / max(tot, 1), 2), "tokens": tot}


def int_scorer(rows, n):
    def score(feats):
        s = [0] * n
        for f in feats:
            r = rows.get(f)
            if r is not None:
                s = [x + y for x, y in zip(s, r)]
        return s
    return score


def float_scorer(model, avg):
    n = model.n
    zero = [0.0] * n

    def score(feats):
        s = zero
        for f in feats:
            r = avg.get(f)
            if r is not None:
                s = [a + b for a, b in zip(s, r)]
        return s
    return score


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--lang", required=True, choices=["en", "es"])
    ap.add_argument("--data", default="data/raw/ud")
    ap.add_argument("--out", required=True)
    ap.add_argument("--epochs", type=int, default=10)
    ap.add_argument("--seed", type=int, default=7)
    ap.add_argument("--min-count", type=int, default=2)
    ap.add_argument("--table-bits", type=int, default=16)
    ap.add_argument("--load", type=float, default=0.8, help="max fill of the hash table")
    ap.add_argument("--max-sents", type=int, default=0, help="train on the first N sentences only (tiny models)")
    ap.add_argument("--ancora-fraction", type=float, default=1.0)
    ap.add_argument("--retrain", type=int, default=1, help="1 = second pass restricted to the exported features")
    ap.add_argument("--no-report", action="store_true")
    ap.add_argument("--eval-float", action="store_true", help="also score the unpruned float model on dev")
    a = ap.parse_args()

    t_start = time.time()
    train_s, _r, shas = common.load(a.lang, a.data, "train", a.max_sents, a.ancora_fraction, a.seed)
    dev_s, dev_r, s2 = common.load(a.lang, a.data, "dev")
    test_s, test_r, s3 = common.load(a.lang, a.data, "test")
    shas.update(s2)
    shas.update(s3)
    labels, heads = make_labels(train_s)
    layout = Layout(labels, heads)
    print("%s: %d train sentences, %d labels" % (a.lang, len(train_s), len(labels)))
    model = train(train_s, layout, a.epochs, a.seed)
    avg = model.averaged()
    rep = {}
    if a.eval_float:
        rep["dev_float_unpruned"] = evaluate(dev_s, float_scorer(model, avg), layout)
        print("dev (float, unpruned):", rep["dev_float_unpruned"])
    table = 1 << a.table_bits
    if a.retrain:
        rows, stats = quantise(avg, model.counts, a.min_count, int(table * a.load))
        rep["first_pass_pruning"] = stats
        if a.eval_float:
            rep["dev_first_pass_pruned"] = evaluate(dev_s, int_scorer(rows, len(labels)), layout)
            print("dev (first pass, pruned):", rep["dev_first_pass_pruned"])
        print("retraining on %d features" % len(rows))
        model = train(train_s, layout, a.epochs, a.seed, allowed=set(rows))
        avg = model.averaged()
    t_train = time.time() - t_start
    rows, stats = quantise(avg, model.counts, a.min_count, int(table * a.load))
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    note = common.note_text(a.lang, "tagger", "see tools/train/report.json")
    size, sha = vpt.write(a.out, a.lang, "tag", labels, rows, table, note)
    m = vpt.Model(a.out)
    rep["dev"] = evaluate(dev_s, m.scores, layout)
    rep["test"] = evaluate(test_s, m.scores, layout)
    if len(dev_r) > 1:
        for name, (lo, hi) in dev_r.items():
            rep["dev_" + name] = evaluate(dev_s[lo:hi], m.scores, layout)
        for name, (lo, hi) in test_r.items():
            rep["test_" + name] = evaluate(test_s[lo:hi], m.scores, layout)
    rep.update({
        "file": os.path.basename(a.out), "bytes": size, "sha256": sha, "labels": len(labels),
        "table_size": table, "epochs": a.epochs, "seed": a.seed, "min_count": a.min_count,
        "train_sentences": len(train_s), "train_tokens": sum(len(s["words"]) for s in train_s),
        "ancora_fraction": a.ancora_fraction if a.lang == "es" else None,
        "retrain": a.retrain, "train_seconds": round(t_train, 1), "total_seconds": round(time.time() - t_start, 1),
        "data_sha256": shas, "pruning": stats,
    })
    print(rep)
    if not a.no_report and not a.max_sents:
        common.update_report(a.lang, "tagger", rep)


if __name__ == "__main__":
    main()
