"""Feature-string builders for the tagger and the parser (DESIGN.md section 17).

Mirrored exactly by engine/nlp/src/features.cpp; tests/fixtures/nlp/features_golden.tsv pins both.
A feature is a UTF-8 string; its key in the .vpt hash table is fnv1a64(utf8 bytes), with 0 remapped to 1
(0 marks an empty slot). Any change here changes the model files: retrain and regenerate the golden files.
"""

FNV_OFFSET = 0xCBF29CE484222325
FNV_PRIME = 0x100000001B3
MASK64 = 0xFFFFFFFFFFFFFFFF


def fnv1a64(s):
    h = FNV_OFFSET
    for b in s.encode("utf-8"):
        h = ((h ^ b) * FNV_PRIME) & MASK64
    return h if h != 0 else 1


# ---- word normalisation and shape ----------------------------------------------------------------------------
_QUOTES = {0x2018: "'", 0x2019: "'", 0x201A: "'", 0x201B: "'", 0x201C: '"', 0x201D: '"', 0x201E: '"', 0x201F: '"'}


def norm(w):
    """Lower case (Python str.lower = vp::text::lower) and curly quotes to straight ones."""
    return w.lower().translate(_QUOTES)


def shape(w):
    """Word shape: digit -> 'd', a code point whose lower case differs -> 'X', ASCII non-letters and the
    punctuation blocks U+00A1-U+00BF and U+2000-U+206F -> themselves, anything else -> 'x'; runs of the same
    output character collapse to one; at most 6 characters."""
    out = []
    last = ""
    for ch in w:
        o = ord(ch)
        if 48 <= o <= 57:
            c = "d"
        elif ch.lower() != ch:
            c = "X"
        elif 97 <= o <= 122:
            c = "x"
        elif o < 128 or 0xA1 <= o <= 0xBF or 0x2000 <= o <= 0x206F:
            c = ch
        else:
            c = "x"
        if c != last:
            out.append(c)
            last = c
            if len(out) == 6:
                break
    return "".join(out)


BOS = ("-BOS-", "-BOS2-")
EOS = ("-EOS-", "-EOS2-")


# ---- tagger ------------------------------------------------------------------------------------------------------
def tag_features(words, lows, shapes, i, t1, t2):
    """Features of token i for the UPOS head and the feature heads. words: surface forms; lows: norm(words);
    shapes: shape(words); t1, t2: the predicted tags of tokens i-1, i-2 (BOS markers at the start)."""
    n = len(lows)
    w = words[i]
    lw = lows[i]
    lm1 = lows[i - 1] if i >= 1 else BOS[0]
    lm2 = lows[i - 2] if i >= 2 else (BOS[0] if i == 1 else BOS[1])
    lp1 = lows[i + 1] if i + 1 < n else EOS[0]
    lp2 = lows[i + 2] if i + 2 < n else (EOS[0] if i + 1 < n else EOS[1])
    f = [
        "b",
        "lw=" + lw,
        "s1=" + lw[-1:],
        "s2=" + lw[-2:],
        "s3=" + lw[-3:],
        "p1=" + lw[:1],
        "sh=" + shapes[i],
        "lw-1=" + lm1,
        "lw+1=" + lp1,
        "lw-2=" + lm2,
        "lw+2=" + lp2,
        "t-1=" + t1,
        "t-1t-2=" + t1 + " " + t2,
        "lwt-1=" + lw + " " + t1,
        "s3-1=" + (lm1[-3:] if i >= 1 else BOS[0]),
        "s3+1=" + (lp1[-3:] if i + 1 < n else EOS[0]),
        "s4=" + lw[-4:],
        "p2=" + lw[:2],
        "t-1lw+1=" + t1 + " " + lp1,
        "lw-1lw=" + lm1 + " " + lw,
        "lwlw+1=" + lw + " " + lp1,
        "p3=" + lw[:3],
    ]
    if w != lw:
        f.append("w=" + w)
    return f


def tag_feat_extra(upos, lw):
    """Extra features of the morphological-feature heads, given the token's predicted UPOS."""
    return ["u=" + upos, "us2=" + upos + " " + lw[-2:]]


# ---- parser ------------------------------------------------------------------------------------------------------
NONE = "-NONE-"
ROOT = "-ROOT-"


def dist_bucket(d):
    if d < 5:
        return str(d)
    return "5" if d < 10 else "10"


def parse_features(lows, tags, stack, b, n, heads, labels, lc1, lc2, rc1, rc2, nl, nr):
    """Features of the configuration. Index 0 is the artificial root; words are 1..n.
    lows[0] = tags[0] = ROOT. stack: list of word indices (top last); b: index of the first buffer word
    (b > n means the buffer is empty). heads[i] (-1 = none), labels[i] ("" = none), lc1/lc2 leftmost and second
    leftmost child, rc1/rc2 rightmost and second rightmost (-1 = none), nl/nr number of left/right children."""
    if stack:
        s0 = stack[-1]
        s0w = lows[s0]
        s0p = tags[s0]
        s0h = heads[s0]
        s0hp = tags[s0h] if s0h >= 0 else NONE
        s0hw = lows[s0h] if s0h >= 0 else NONE
        s0l = labels[s0] or NONE
        x = lc1[s0]
        s0lp = tags[x] if x >= 0 else NONE
        s0ll = labels[x] if x >= 0 else NONE
        x = rc1[s0]
        s0rp = tags[x] if x >= 0 else NONE
        s0rl = labels[x] if x >= 0 else NONE
        x = lc2[s0]
        s0l2p = tags[x] if x >= 0 else NONE
        x = rc2[s0]
        s0r2p = tags[x] if x >= 0 else NONE
        s0vl = str(nl[s0])
        s0vr = str(nr[s0])
    else:
        s0 = -1
        s0w = s0p = s0hp = s0hw = s0l = s0lp = s0ll = s0rp = s0rl = s0l2p = s0r2p = NONE
        s0vl = s0vr = "0"
    if len(stack) >= 2:
        s1 = stack[-2]
        s1w = lows[s1]
        s1p = tags[s1]
    else:
        s1w = s1p = NONE
    if b <= n:
        n0w = lows[b]
        n0p = tags[b]
        x = lc1[b]
        n0lp = tags[x] if x >= 0 else NONE
        n0ll = labels[x] if x >= 0 else NONE
        x = lc2[b]
        n0l2p = tags[x] if x >= 0 else NONE
        n0vl = str(nl[b])
    else:
        n0w = n0p = n0lp = n0ll = n0l2p = NONE
        n0vl = "0"
    n1w = lows[b + 1] if b + 1 <= n else NONE
    n1p = tags[b + 1] if b + 1 <= n else NONE
    n2p = tags[b + 2] if b + 2 <= n else NONE
    d = dist_bucket(b - s0) if (s0 >= 0 and b <= n) else "0"
    return [
        "b",
        "s0w=" + s0w,
        "s0p=" + s0p,
        "s0wp=" + s0w + " " + s0p,
        "n0w=" + n0w,
        "n0p=" + n0p,
        "n0wp=" + n0w + " " + n0p,
        "n1w=" + n1w,
        "n1p=" + n1p,
        "n2p=" + n2p,
        "s1p=" + s1p,
        "s1w=" + s1w,
        "s0wp,n0p=" + s0w + " " + s0p + " " + n0p,
        "s0p,n0wp=" + s0p + " " + n0w + " " + n0p,
        "s0w,n0w=" + s0w + " " + n0w,
        "s0p,n0p=" + s0p + " " + n0p,
        "n0p,n1p=" + n0p + " " + n1p,
        "n0p,n1p,n2p=" + n0p + " " + n1p + " " + n2p,
        "s0p,n0p,n1p=" + s0p + " " + n0p + " " + n1p,
        "s1p,s0p,n0p=" + s1p + " " + s0p + " " + n0p,
        "s0hp,s0p,n0p=" + s0hp + " " + s0p + " " + n0p,
        "s0p,s0lp,n0p=" + s0p + " " + s0lp + " " + n0p,
        "s0p,s0rp,n0p=" + s0p + " " + s0rp + " " + n0p,
        "s0p,n0p,n0lp=" + s0p + " " + n0p + " " + n0lp,
        "s0p,s0lp,s0l2p=" + s0p + " " + s0lp + " " + s0l2p,
        "s0p,s0rp,s0r2p=" + s0p + " " + s0rp + " " + s0r2p,
        "n0p,n0lp,n0l2p=" + n0p + " " + n0lp + " " + n0l2p,
        "s0w,d=" + s0w + " " + d,
        "n0w,d=" + n0w + " " + d,
        "s0p,n0p,d=" + s0p + " " + n0p + " " + d,
        "s0p,vl=" + s0p + " " + s0vl,
        "s0p,vr=" + s0p + " " + s0vr,
        "n0p,vl=" + n0p + " " + n0vl,
        "s0hw=" + s0hw,
        "s0l=" + s0l,
        "s0ll,s0p=" + s0ll + " " + s0p,
        "s0rl,s0p=" + s0rl + " " + s0p,
        "n0ll,n0p=" + n0ll + " " + n0p,
    ]


def label_features(lows, tags, h, dpt, dirc, labels, lc1, rc1):
    """Extra features of the label head for a candidate arc h -> dpt; dirc is "L" (left-arc, the dependent is
    s0 and the head b0) or "R" (right-arc, the dependent is b0)."""
    hw = lows[h]
    hp = tags[h]
    dw = lows[dpt]
    dp = tags[dpt]
    x = lc1[dpt]
    dll = labels[x] if x >= 0 else NONE
    x = rc1[dpt]
    drl = labels[x] if x >= 0 else NONE
    dist = dist_bucket(abs(h - dpt)) if h != 0 else "r"
    return [
        "Lb=" + dirc,
        "Ldp=" + dirc + " " + dp,
        "Lhp,dp=" + dirc + " " + hp + " " + dp,
        "Ldw=" + dirc + " " + dw,
        "Lhw,dp=" + dirc + " " + hw + " " + dp,
        "Lhp,dw=" + dirc + " " + hp + " " + dw,
        "Ldp,dist=" + dirc + " " + dp + " " + dist,
        "Ldp,dll,drl=" + dirc + " " + dp + " " + dll + " " + drl,
    ]
