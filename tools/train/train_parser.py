#!/usr/bin/env python3
"""Trains the greedy arc-eager dependency parser and exports it as a .vpt file (DESIGN.md section 17).

    python3 tools/train/train_parser.py --lang en --data data/raw/ud --out data/work/nlp/english.dep.vpt \
        --tagger data/work/nlp/english.tag.vpt

Labels: "@SHIFT", "@REDUCE", "@LEFT", "@RIGHT" (transition head), then the universal deprels (label head).
The transition is the best valid transition (lowest index on ties); for an arc, the label is the best deprel
(lowest index on ties) on the configuration features plus label_features(); arcs from the root are "root" and
no other arc may be "root". Training: static oracle on projectivised gold trees, gold UPOS (or jackknifed UPOS
with --jackknife), shuffled with a fixed seed. Evaluation uses the UPOS predicted by --tagger.
"""
import argparse
import os
import random
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import common  # noqa: E402
import conllu  # noqa: E402
from features import ROOT, label_features, norm, parse_features  # noqa: E402
from perceptron import Perceptron, argmax, quantise  # noqa: E402
import vpt  # noqa: E402

SHIFT, REDUCE, LEFT, RIGHT = 0, 1, 2, 3
TRANS = ["@SHIFT", "@REDUCE", "@LEFT", "@RIGHT"]
NT = 4
MIN_LABEL_COUNT = 50


class State:
    """Configuration of the arc-eager system over words 1..n (0 = root)."""
    __slots__ = ("n", "stack", "b", "heads", "labels", "lc1", "lc2", "rc1", "rc2", "nl", "nr")

    def __init__(self, n):
        m = n + 1
        self.n = n
        self.stack = [0]
        self.b = 1
        self.heads = [-1] * m
        self.labels = [""] * m
        self.lc1 = [-1] * m
        self.lc2 = [-1] * m
        self.rc1 = [-1] * m
        self.rc2 = [-1] * m
        self.nl = [0] * m
        self.nr = [0] * m

    def arc(self, h, d, lab):
        self.heads[d] = h
        self.labels[d] = lab
        if d < h:
            self.nl[h] += 1
            if self.lc1[h] < 0 or d < self.lc1[h]:
                self.lc2[h] = self.lc1[h]
                self.lc1[h] = d
            elif self.lc2[h] < 0 or d < self.lc2[h]:
                self.lc2[h] = d
        else:
            self.nr[h] += 1
            if self.rc1[h] < 0 or d > self.rc1[h]:
                self.rc2[h] = self.rc1[h]
                self.rc1[h] = d
            elif self.rc2[h] < 0 or d > self.rc2[h]:
                self.rc2[h] = d

    def valid(self, t):
        st = self.stack
        if t == SHIFT:
            return self.b <= self.n
        if not st:
            return False
        s0 = st[-1]
        if t == REDUCE:
            return s0 != 0 and self.heads[s0] >= 0
        if t == LEFT:
            return s0 != 0 and self.heads[s0] < 0 and self.b <= self.n
        return self.b <= self.n and (s0 != 0 or self.nr[0] == 0)  # RIGHT; one root child

    def apply(self, t, lab):
        if t == SHIFT:
            self.stack.append(self.b)
            self.b += 1
        elif t == REDUCE:
            self.stack.pop()
        elif t == LEFT:
            self.arc(self.b, self.stack.pop(), lab)
        else:
            self.arc(self.stack[-1], self.b, lab)
            self.stack.append(self.b)
            self.b += 1

    def feats(self, lows, tags):
        return parse_features(lows, tags, self.stack, self.b, self.n, self.heads, self.labels,
                              self.lc1, self.lc2, self.rc1, self.rc2, self.nl, self.nr)

    def finish(self, tags):
        """Words left without a head: the first becomes the root if there is none, the others attach to the
        root's child as "punct" (PUNCT) or "dep"."""
        root = self.rc1[0]
        for d in range(1, self.n + 1):
            if self.heads[d] < 0:
                if root < 0:
                    self.arc(0, d, "root")
                    root = d
                else:
                    self.arc(root, d, "punct" if tags[d] == "PUNCT" else "dep")


def oracle(st, gold):
    s0 = st.stack[-1] if st.stack else -1
    b = st.b
    if s0 > 0 and b <= st.n and gold[s0] == b and st.heads[s0] < 0:
        return LEFT
    if s0 >= 0 and b <= st.n and gold[b] == s0 and (s0 != 0 or st.nr[0] == 0):
        return RIGHT
    if s0 > 0 and st.heads[s0] >= 0:
        for k in range(b, st.n + 1):
            if gold[k] == s0:
                return SHIFT
        return REDUCE
    return SHIFT


def costs(st, gold):
    """Dynamic-oracle costs (Goldberg and Nivre 2012, arc-eager): gold arcs a transition makes unreachable.
    Returns a list of 4 costs, None for invalid transitions."""
    n = st.n
    b = st.b
    stack = st.stack
    heads = st.heads
    s0 = stack[-1] if stack else -1
    out = [None, None, None, None]
    if b <= n:
        c = 0
        gb = gold[b]
        for k in stack:
            if gb == k:
                c += 1
            if k > 0 and gold[k] == b and heads[k] < 0:
                c += 1
        out[SHIFT] = c
    if st.valid(REDUCE):
        c = 0
        for k in range(b, n + 1):
            if gold[k] == s0:
                c += 1
        out[REDUCE] = c
    if st.valid(LEFT):
        c = 0
        for k in range(b, n + 1):
            if gold[k] == s0:
                c += 1
        if gold[s0] != b and gold[s0] > b:
            c += 1
        out[LEFT] = c
    if st.valid(RIGHT):
        c = 0
        gb = gold[b]
        if gb != s0 and (gb > b or gb in stack):
            c += 1
        for k in stack:
            if k > 0 and gold[k] == b and heads[k] < 0:
                c += 1
        out[RIGHT] = c
    return out


def label_scores(model_scores, sc, lows, tags, st, t):
    if t == LEFT:
        h, d, dirc = st.b, st.stack[-1], "L"
    else:
        h, d, dirc = st.stack[-1], st.b, "R"
    lf = label_features(lows, tags, h, d, dirc, st.labels, st.lc1, st.rc1)
    return h, d, lf, model_scores(lf, list(sc))


def parse_sentence(words, tags_in, score_fn, labels, root_idx):
    """Greedy decoding with any scoring function. tags_in: UPOS per word. Returns (heads, deprels) 1-based."""
    n = len(words)
    lows = [ROOT] + [norm(w) for w in words]
    tags = [ROOT] + list(tags_in)
    st = State(n)
    nlab = len(labels)
    while st.b <= n:
        sc = score_fn(st.feats(lows, tags))
        t = argmax(sc, 0, NT, st.valid)
        lab = ""
        if t == LEFT or t == RIGHT:
            h, _d, lf, sc2 = label_scores(lambda f, base: [a + b for a, b in zip(base, score_fn(f))],
                                          sc, lows, tags, st, t)
            if h == 0:
                lab = "root"
            else:
                lab = labels[argmax(sc2, NT, nlab, lambda k: k != root_idx)]
        st.apply(t, lab)
    st.finish(tags)
    return st.heads[1:], st.labels[1:]


def make_labels(train):
    counts = {}
    for s in train:
        for d in s["deprels"]:
            counts[d] = counts.get(d, 0) + 1
    deps = sorted(d for d, c in counts.items() if c >= MIN_LABEL_COUNT or d == "root")
    return TRANS + deps


def train(sents, tag_lists, labels, epochs, seed, explore=0.0, allowed=None, log=print):
    """allowed: optional set of feature strings; all other features are ignored (retraining after pruning)."""
    model = Perceptron(len(labels))
    index = {l: i for i, l in enumerate(labels)}
    root_idx = index["root"]
    nlab = len(labels)
    data = []
    skipped = 0
    for s, tags in zip(sents, tag_lists):
        gold, _ = conllu.projectivise(s["heads"])
        n = len(gold)
        g = [-1] + gold
        # replay the oracle once to make sure it reaches the projectivised tree
        st = State(n)
        while st.b <= n:
            t = oracle(st, g)
            if not st.valid(t):
                break
            st.apply(t, "x")
        if st.b <= n or st.heads[1:] != gold:
            skipped += 1
            continue
        lab = [-1] + [index.get(d, -1) for d in s["deprels"]]
        data.append(([ROOT] + [norm(w) for w in s["words"]], [ROOT] + list(tags), g, lab))
    log("%d training trees, %d skipped (oracle could not reach the projectivised tree)" % (len(data), skipped))
    order = list(range(len(data)))
    rnd = random.Random(seed)
    for ep in range(epochs):
        t0 = time.time()
        rnd.shuffle(order)
        model.counting = ep == 0
        ok = tot = 0
        for di in order:
            lows, tags, g, lab = data[di]
            n = len(g) - 1
            st = State(n)
            while st.b <= n:
                f = st.feats(lows, tags)
                if allowed is not None:
                    f = [x for x in f if x in allowed]
                model.count(f)
                sc = model.scores(f)
                p = argmax(sc, 0, NT, st.valid)
                if explore:
                    cs = costs(st, g)
                    best = min(c for c in cs if c is not None)
                    t = argmax(sc, 0, NT, lambda k: cs[k] == best)
                    if p != t and cs[p] != best and ep >= 1 and rnd.random() < explore:
                        follow = p
                    else:
                        follow = t if cs[p] != best else p
                    if cs[p] == best:
                        t = p
                else:
                    t = oracle(st, g)
                    follow = t
                model.update(t, p, f)
                ok += p == t
                tot += 1
                name = ""
                if follow == LEFT or follow == RIGHT:
                    h, d, lf, sc2 = label_scores(model.scores, sc, lows, tags, st, follow)
                    if allowed is not None:
                        lf = [x for x in lf if x in allowed]
                    if h != 0:
                        model.count(lf)
                        guess = argmax(sc2, NT, nlab, lambda k: k != root_idx)
                        truth = lab[d] if g[d] == h else -2
                        if truth >= 0 and truth != root_idx:
                            model.update(truth, guess, f + lf)
                        name = labels[truth] if truth >= 0 else ("dep" if truth == -1 else labels[guess])
                    else:
                        name = "root"
                model.tick()
                st.apply(follow, name)
        log("epoch %d: transition acc %.4f (%.1fs)" % (ep + 1, ok / max(tot, 1), time.time() - t0))
    return model


def evaluate(sents, tag_lists, score_fn, labels):
    root_idx = labels.index("root")
    uas = las = tot = 0
    for s, tags in zip(sents, tag_lists):
        heads, deps = parse_sentence(s["words"], tags, score_fn, labels, root_idx)
        for h, d, gh, gd in zip(heads, deps, s["heads"], s["deprels"]):
            tot += 1
            if h == gh:
                uas += 1
                las += d == gd
    return {"uas": round(100.0 * uas / max(tot, 1), 2), "las": round(100.0 * las / max(tot, 1), 2), "tokens": tot}


def float_scorer(avg, n):
    def score(feats):
        s = [0.0] * n
        for f in feats:
            r = avg.get(f)
            if r is not None:
                s = [x + y for x, y in zip(s, r)]
        return s
    return score


def vpt_scorer(rows, n):
    """Integer scorer over quantised rows keyed by feature string (same sums as a loaded .vpt)."""
    def score(feats):
        s = [0] * n
        for f in feats:
            r = rows.get(f)
            if r is not None:
                s = [x + y for x, y in zip(s, r)]
        return s
    return score


def predicted_tags(sents, tagger_path):
    import train_tagger
    m = vpt.Model(tagger_path)
    print("tagging %d sentences with %s (sha256 %s)" % (len(sents), tagger_path, conllu.sha256_file(tagger_path)))
    layout = train_tagger.Layout.from_labels(m.labels)
    return [train_tagger.tag_sentence(s["words"], m.scores, layout)[0] for s in sents]


def jackknife_tags(sents, folds, epochs, seed, log=print):
    """UPOS for the training sentences predicted by taggers trained on the other folds (float models)."""
    import train_tagger
    out = [None] * len(sents)
    for k in range(folds):
        tr = [s for i, s in enumerate(sents) if i % folds != k]
        labels, heads = train_tagger.make_labels(tr)
        layout = train_tagger.Layout(labels, heads)
        m = train_tagger.train(tr, layout, epochs, seed, log=lambda *_: None)
        sc = train_tagger.float_scorer(m, m.averaged())
        for i in range(k, len(sents), folds):
            out[i] = train_tagger.tag_sentence(sents[i]["words"], sc, layout)[0]
        log("jackknife fold %d done" % (k + 1))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--lang", required=True, choices=["en", "es"])
    ap.add_argument("--data", default="data/raw/ud")
    ap.add_argument("--out", required=True)
    ap.add_argument("--tagger", required=True, help="the .vpt tagger used for evaluation (predicted UPOS)")
    ap.add_argument("--epochs", type=int, default=10)
    ap.add_argument("--seed", type=int, default=7)
    ap.add_argument("--min-count", type=int, default=2)
    ap.add_argument("--table-bits", type=int, default=16)
    ap.add_argument("--load", type=float, default=0.8)
    ap.add_argument("--max-sents", type=int, default=0)
    ap.add_argument("--ancora-fraction", type=float, default=1.0)
    ap.add_argument("--jackknife", type=int, default=4, help="train on UPOS jackknifed over N folds (0 = gold)")
    ap.add_argument("--explore", type=float, default=0.9,
                    help="dynamic oracle; follow wrong predictions with this probability from epoch 2 (0 = static)")
    ap.add_argument("--jk-epochs", type=int, default=5, help="epochs of the jackknife taggers")
    ap.add_argument("--retrain", type=int, default=1, help="1 = second pass restricted to the exported features")
    ap.add_argument("--no-report", action="store_true")
    ap.add_argument("--eval-float", action="store_true")
    a = ap.parse_args()

    t_start = time.time()
    train_s, _r, shas = common.load(a.lang, a.data, "train", a.max_sents, a.ancora_fraction, a.seed)
    dev_s, dev_r, s2 = common.load(a.lang, a.data, "dev")
    test_s, test_r, s3 = common.load(a.lang, a.data, "test")
    shas.update(s2)
    shas.update(s3)
    labels = make_labels(train_s)
    print("%s: %d train sentences, %d labels" % (a.lang, len(train_s), len(labels)))
    if a.jackknife:
        train_tags = jackknife_tags(train_s, a.jackknife, a.jk_epochs, a.seed)
    else:
        train_tags = [s["upos"] for s in train_s]
    model = train(train_s, train_tags, labels, a.epochs, a.seed, a.explore)
    avg = model.averaged()
    table = 1 << a.table_bits
    rep = {}
    if a.eval_float:
        rep["dev_float_unpruned"] = evaluate(dev_s, predicted_tags(dev_s, a.tagger), float_scorer(avg, len(labels)),
                                             labels)
        print("dev (first pass, float, unpruned):", rep["dev_float_unpruned"])
    if a.retrain:
        rows, stats = quantise(avg, model.counts, a.min_count, int(table * a.load))
        rep["first_pass_pruning"] = stats
        if a.eval_float:
            first = vpt_scorer(rows, len(labels))
            rep["dev_first_pass_pruned"] = evaluate(dev_s, predicted_tags(dev_s, a.tagger), first, labels)
            print("dev (first pass, pruned):", rep["dev_first_pass_pruned"])
        print("retraining on %d features" % len(rows))
        model = train(train_s, train_tags, labels, a.epochs, a.seed, a.explore, allowed=set(rows))
        avg = model.averaged()
    t_train = time.time() - t_start
    dev_tags = predicted_tags(dev_s, a.tagger)
    test_tags = predicted_tags(test_s, a.tagger)
    rows, stats = quantise(avg, model.counts, a.min_count, int(table * a.load))
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    note = common.note_text(a.lang, "parser", "see tools/train/report.json")
    size, sha = vpt.write(a.out, a.lang, "dep", labels, rows, table, note)
    m = vpt.Model(a.out)
    rep["dev"] = evaluate(dev_s, dev_tags, m.scores, labels)
    rep["dev_gold_upos"] = evaluate(dev_s, [s["upos"] for s in dev_s], m.scores, labels)
    rep["test"] = evaluate(test_s, test_tags, m.scores, labels)
    if len(dev_r) > 1:
        for name, (lo, hi) in dev_r.items():
            rep["dev_" + name] = evaluate(dev_s[lo:hi], dev_tags[lo:hi], m.scores, labels)
        for name, (lo, hi) in test_r.items():
            rep["test_" + name] = evaluate(test_s[lo:hi], test_tags[lo:hi], m.scores, labels)
    rep.update({
        "file": os.path.basename(a.out), "bytes": size, "sha256": sha, "labels": len(labels),
        "table_size": table, "epochs": a.epochs, "seed": a.seed, "min_count": a.min_count,
        "tagger_sha256": conllu.sha256_file(a.tagger), "jackknife_folds": a.jackknife, "jackknife_epochs": a.jk_epochs if a.jackknife else None,
        "explore": a.explore, "retrain": a.retrain, "tagger": os.path.basename(a.tagger),
        "train_sentences": len(train_s), "train_tokens": sum(len(s["words"]) for s in train_s),
        "ancora_fraction": a.ancora_fraction if a.lang == "es" else None,
        "train_seconds": round(t_train, 1), "total_seconds": round(time.time() - t_start, 1),
        "data_sha256": shas, "pruning": stats,
    })
    print(rep)
    if not a.no_report and not a.max_sents:
        common.update_report(a.lang, "parser", rep)


if __name__ == "__main__":
    main()
