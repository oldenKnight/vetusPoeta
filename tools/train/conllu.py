"""CoNLL-U reader for the NLP trainers (DESIGN.md section 17). Python 3.11 stdlib only.

A sentence is a dict of parallel lists (no per-token objects):
    {"id": sent_id, "words": [...], "upos": [...], "feats": [...], "heads": [...], "deprels": [...]}
Index i in each list is word i+1 of the sentence; heads use 1-based word ids, 0 = root.
Multiword token ranges ("1-2") and empty nodes ("1.1") are skipped: the syntactic words are kept.
Deprels are collapsed to the universal relation (the part before ':').
"""
import hashlib


def collapse(deprel):
    i = deprel.find(":")
    return deprel if i < 0 else deprel[:i]


def read(path):
    """Returns a list of sentence dicts read from a CoNLL-U file."""
    sents = []
    words, upos, feats, heads, deps = [], [], [], [], []
    sid = ""
    with open(path, encoding="utf-8") as fh:
        for line in fh:
            line = line.rstrip("\n")
            if not line:
                if words:
                    sents.append({"id": sid, "words": words, "upos": upos, "feats": feats,
                                  "heads": heads, "deprels": deps})
                words, upos, feats, heads, deps = [], [], [], [], []
                sid = ""
                continue
            if line[0] == "#":
                if line.startswith("# sent_id"):
                    sid = line.split("=", 1)[1].strip()
                continue
            cols = line.split("\t")
            if len(cols) != 10:
                raise ValueError("%s: bad CoNLL-U line: %r" % (path, line[:80]))
            tid = cols[0]
            if "-" in tid or "." in tid:
                continue
            words.append(cols[1])
            upos.append(cols[3])
            feats.append(cols[5])
            heads.append(int(cols[6]) if cols[6] != "_" else 0)
            deps.append(collapse(cols[7]))
    if words:
        sents.append({"id": sid, "words": words, "upos": upos, "feats": feats, "heads": heads, "deprels": deps})
    return sents


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for block in iter(lambda: fh.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def _dominates(heads, h, d):
    """True when word h (1-based, 0 = root) is an ancestor of word d (or h == d)."""
    seen = 0
    n = len(heads)
    while d != 0 and seen <= n:
        if d == h:
            return True
        d = heads[d - 1]
        seen += 1
    return h == 0


def _nonprojective_arcs(heads):
    """Dependents d whose arc (heads[d], d) is non-projective, i.e. some word strictly between head and
    dependent is not dominated by the head."""
    out = []
    for d0 in range(len(heads)):
        d = d0 + 1
        h = heads[d0]
        lo, hi = (h, d) if h < d else (d, h)
        for k in range(lo + 1, hi):
            if not _dominates(heads, h, k):
                out.append(d)
                break
    return out


def projectivise(heads):
    """Standard lifting (Nivre and Nilsson 2005, without label encoding): while the tree is non-projective,
    take the shortest non-projective arc (leftmost dependent on ties) and attach its dependent to the head's
    head. Returns (new_heads, number_of_lifts). The input list is not modified."""
    heads = list(heads)
    lifts = 0
    while True:
        bad = _nonprojective_arcs(heads)
        if not bad:
            return heads, lifts
        best = min(bad, key=lambda d: (abs(heads[d - 1] - d), d))
        h = heads[best - 1]
        if h == 0:  # cannot lift above the root; should not happen for a tree
            return heads, lifts
        heads[best - 1] = heads[h - 1]
        lifts += 1


def is_projective(heads):
    return not _nonprojective_arcs(heads)
