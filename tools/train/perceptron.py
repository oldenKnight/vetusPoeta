"""Averaged perceptron with label "heads", feature counting, pruning and int16 quantisation (DESIGN.md 17).

Weights: dict feature -> dict label index -> float (sparse, no per-update objects besides the averaging keys).
A head is a contiguous range of label indices; each head is an independent multi-class perceptron sharing the
feature strings. Averaging is lazy (totals and timestamps per (feature, label)).
"""
import math


class Perceptron:
    def __init__(self, n_labels):
        self.n = n_labels
        self.w = {}          # feat -> {label: weight}
        self.totals = {}     # (feat, label) -> accumulated weight * time
        self.stamps = {}     # (feat, label) -> time of the last change
        self.i = 0           # instances seen
        self.counts = {}     # feat -> occurrences (counted while counting is on)
        self.counting = True

    def count(self, feats):
        if self.counting:
            c = self.counts
            for f in feats:
                c[f] = c.get(f, 0) + 1

    def scores(self, feats, out=None):
        s = out if out is not None else [0.0] * self.n
        w = self.w
        for f in feats:
            row = w.get(f)
            if row:
                for lab, v in row.items():
                    s[lab] += v
        return s

    def tick(self):
        self.i += 1

    def update(self, truth, guess, feats):
        if truth == guess:
            return
        w = self.w
        tot = self.totals
        st = self.stamps
        i = self.i
        for f in feats:
            row = w.get(f)
            if row is None:
                row = w[f] = {}
            for lab, delta in ((truth, 1.0), (guess, -1.0)):
                key = (f, lab)
                cur = row.get(lab, 0.0)
                tot[key] = tot.get(key, 0.0) + (i - st.get(key, 0)) * cur
                st[key] = i
                row[lab] = cur + delta

    def averaged(self):
        """Returns feat -> list of n averaged float weights (dense rows)."""
        out = {}
        i = max(self.i, 1)
        tot = self.totals
        st = self.stamps
        for f, row in self.w.items():
            dense = [0.0] * self.n
            for lab, cur in row.items():
                key = (f, lab)
                t = tot.get(key, 0.0) + (i - st.get(key, 0)) * cur
                dense[lab] = t / i
            out[f] = dense
        return out


def argmax(scores, lo, hi, allowed=None):
    """Index of the highest score in [lo, hi); lowest index on ties; allowed: optional predicate on index."""
    best = -1
    bv = None
    for k in range(lo, hi):
        if allowed is not None and not allowed(k):
            continue
        v = scores[k]
        if bv is None or v > bv:
            bv = v
            best = k
    return best


def quantise(avg, counts, min_count, capacity):
    """Prunes and quantises averaged weights.

    1. features seen fewer than min_count times are dropped;
    2. weights are scaled so the largest |w| maps into int16 (w_q = round(w * 256) on the rescaled weight;
       argmax is scale invariant), rows that quantise to all zeros are dropped;
    3. if more than `capacity` rows remain, the rows with the largest L1 norm are kept (ties: smaller hash).
    Returns (rows: dict feat -> list of ints, stats dict).
    """
    from features import fnv1a64
    kept = {f: r for f, r in avg.items() if counts.get(f, 0) >= min_count}
    stats = {"features_trained": len(avg), "features_count_ok": len(kept)}
    mx = 0.0
    for r in kept.values():
        for v in r:
            a = abs(v)
            if a > mx:
                mx = a
    scale = 256.0
    if mx * scale > 32767.0:
        scale = 32767.0 / mx
    stats["scale"] = scale
    rows = {}
    for f, r in kept.items():
        q = [int(math.floor(v * scale + 0.5)) for v in r]
        q = [32767 if x > 32767 else -32768 if x < -32768 else x for x in q]
        if any(q):
            rows[f] = q
    stats["features_nonzero"] = len(rows)
    if len(rows) > capacity:
        ranked = sorted(rows.items(), key=lambda kv: (-sum(abs(x) for x in kv[1]), fnv1a64(kv[0])))
        rows = dict(ranked[:capacity])
    stats["features_exported"] = len(rows)
    return rows, stats
