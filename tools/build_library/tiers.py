"""Stage `tiers` (la, grc): vocabulary tier, frequency rank, Whitaker frequency letter, emoji and shared_el per lemma.

Rules (DECISIONS D12, PREPLAN 1.8; tools/build_library/README.md has the code letter meanings and their source):
  Latin  tier 1  data/curated/tiers_la.tsv (tier_source as written there: derived = 1, teacher = 2)
         tier 2  DCC Latin core (all rows that join) + Whitaker frequency A/B with age X ("in use throughout the
                 ages") or C ("classical") joined to a Kaikki lemma that has an inflection table, proper names
                 excluded (Kaikki POS name, capitalised headword, Whitaker noun kinds N/L/G); tier_source derived
         tier 3  everything else that has a gloss; 0 when the lemma has no gloss at all
  Greek  tier 1  DCC Greek core lemmas whose Modern Greek descendant has the same spelling (shared_el), plus
                 data/curated/tiers_grc.tsv; tier 2 the rest of DCC Greek; tier 3 the rest (0 without gloss)
  freq_rank      DCC rank when present, else a coarse bucket from the best Whitaker frequency letter
                 (WHIT_BUCKET), else 0
  emoji          data/curated/emoji_<lang>.tsv joined by key (nouns first); unmatched keys are reported
Outputs: tiers.tsv, freq.tsv, review_tier_sheet.csv (for the teacher).
"""
import collections
import csv
import os
import time

import lexdata
import vptext
from common import TsvWriter, log, peak_rss_mb

HERE = os.path.dirname(os.path.abspath(__file__))
CURATED = os.path.normpath(os.path.join(HERE, "..", "..", "data", "curated"))
WHIT_LETTERS = "ABCDEF"
# Whitaker frequency letter -> coarse freq_rank bucket (after the 997 DCC ranks). A "very frequent, in all
# elementary Latin books", B "frequent, top 10 percent", C "top 10,000 words", D "top 20,000", E "2 or 3
# citations", F "single citation"; I (inscription), M (graffiti), N (Pliny) are treated as F.
WHIT_BUCKET = {"A": 1000, "B": 2000, "C": 3000, "D": 4000, "E": 5000, "F": 6000, "I": 6000, "M": 6000, "N": 6000}
TIER2_AGES = frozenset("XC")
NAME_KINDS = frozenset("NLG")
SOURCE_CODE = {"derived": 1, "teacher": 2}


def _better_letter(a, b):
    """The more frequent of two Whitaker letters (A best ... F, then I/M/N, then X/empty)."""
    order = "ABCDEFIMNX"
    if not a:
        return b
    if not b:
        return a
    return a if order.find(a) <= order.find(b) else b


def load_signals(out, lang, infos):
    """Per-lemma data from import_aux: Whitaker best frequency letter and tier-2 eligibility, DCC rank."""
    d = os.path.join(out, lang)
    whit = {}
    whit_t2 = set()
    for r in lexdata.read_rows(os.path.join(d, "whitaker.tsv")):
        ids = lexdata.parse_ids(r[14] if len(r) > 14 else "")
        if not ids:
            continue
        freq, age, pos, codes = r[10], r[7], r[5], r[6].split()
        for i in ids:
            whit[i] = _better_letter(whit.get(i, ""), freq)
        if freq in ("A", "B") and age in TIER2_AGES:
            if pos == "N" and len(codes) >= 4 and codes[3] in NAME_KINDS:
                continue
            whit_t2.update(ids)
    dcc = {}
    dcc_file = "dcc_la.tsv" if lang == "la" else "dcc_grc.tsv"
    for r in lexdata.read_rows(os.path.join(d, dcc_file)):
        try:
            rank = int(r[2])
        except ValueError:
            continue
        for i in lexdata.parse_ids(r[7] if len(r) > 7 else ""):
            if i not in dcc or rank < dcc[i]:
                dcc[i] = rank
    return {"whit": whit, "whit_t2": whit_t2, "dcc": dcc}


def is_name(info):
    head = info.head or info.word
    return info.fpos == "name" or (head[:1].isupper() and head[:1] != head[:1].lower())


def best_single(ids, infos, sig):
    """One lemma for a curated row: DCC member first, then better Whitaker letter, then a table, then lowest id."""
    order = "ABCDEFIMNX"

    def k(i):
        w = sig["whit"].get(i, "X") or "X"
        return (0 if i in sig["dcc"] else 1, order.find(w), 0 if infos[i].has_table else 1, i)
    return min(ids, key=k) if ids else None


def curated_path(lang, name):
    return os.path.join(CURATED, "%s_%s.tsv" % (name, lang))


def tier1(lang, infos, by_key, sig, curated_dir=None):
    """{lemma id: tier_source} for tier 1, and the list of curated rows that did not join."""
    cdir = curated_dir or CURATED
    key_fn = vptext.key_for(lang)
    res = {}
    missing = []
    for row in lexdata.read_curated(os.path.join(cdir, "tiers_%s.tsv" % lang)):
        row = (row + [""] * 6)[:6]
        key, head, pos, tier, source = row[0], row[1], row[2], row[3], row[4]
        if tier != "1":
            continue
        k = key_fn(key or head)  # the curated `key` column is written without the v->u fold
        ids = lexdata.pick_lemmas(by_key.get(k), infos, pos or None) or lexdata.pick_lemmas(by_key.get(k), infos)
        if head and len(ids) > 1:
            same = [i for i in ids if vptext.nfc(infos[i].head) == vptext.nfc(head)]
            ids = same or ids
        i = best_single(ids, infos, sig)
        if i is None:
            missing.append(k)
            continue
        res[i] = max(res.get(i, 0), SOURCE_CODE.get(source, 1))
    if lang == "grc":
        for i in sig["dcc"]:
            if infos[i].rec is not None and infos[i].rec.get("el") == 1 and i not in res:
                res[i] = 1
    return res, missing


def assign(lang, infos, by_key, sig, has_gloss, curated_dir=None):
    """{id: (tier, tier_source)} for every lemma, plus stats."""
    t1, missing = tier1(lang, infos, by_key, sig, curated_dir)
    out = {}
    for info in infos:
        i = info.id
        if not has_gloss(i):
            out[i] = (0, 0)
        elif i in t1:
            out[i] = (1, t1[i])
        elif i in sig["dcc"]:
            out[i] = (2, 1)
        elif lang == "la" and i in sig["whit_t2"] and info.has_table and not is_name(info):
            out[i] = (2, 1)
        else:
            out[i] = (3, 0)
    return out, missing


def freq_rank(i, sig):
    if i in sig["dcc"]:
        return sig["dcc"][i]
    w = sig["whit"].get(i, "")
    return WHIT_BUCKET.get(w, 0)


def load_emoji(lang, infos, by_key, curated_dir=None):
    """{lemma id: emoji}, unmatched keys."""
    cdir = curated_dir or CURATED
    key_fn = vptext.key_for(lang)
    res = {}
    unmatched = []
    for row in lexdata.read_curated(os.path.join(cdir, "emoji_%s.tsv" % lang)):
        row = (row + [""] * 4)[:4]
        key, head, emo = row[0], row[1], row[2]
        if not emo:
            continue
        k = key_fn(key or head)
        ids = lexdata.pick_lemmas(by_key.get(k), infos, "noun")
        if head and len(ids) > 1:
            ids = [i for i in ids if vptext.nfc(infos[i].head) == vptext.nfc(head)] or ids
        if not ids:
            unmatched.append(k)
            continue
        res[min(ids)] = emo
    return res, unmatched


def read_gloss_presence(out, lang):
    has = set()
    for r in lexdata.read_rows(os.path.join(out, lang, "gloss.tsv")):
        if len(r) > 1 and r[1]:
            has.add(int(r[0]))
    return has


def run(lang, out, curated_dir=None):
    t0 = time.time()
    if lang not in ("la", "grc"):
        return {"counts": {}, "skipped": "no tiers for %s" % lang}
    d = os.path.join(out, lang)
    infos, by_key = lexdata.load_lemmas(out, lang, keep_rec=(lang == "grc"))
    sig = load_signals(out, lang, infos)
    glossed = read_gloss_presence(out, lang)
    tiers, missing = assign(lang, infos, by_key, sig, lambda i: i in glossed, curated_dir)
    emoji, unmatched = load_emoji(lang, infos, by_key, curated_dir)
    gl = {}
    for r in lexdata.read_rows(os.path.join(d, "gloss.tsv")):
        gl[int(r[0])] = (r[1], r[3] if len(r) > 3 else "")
    c = collections.Counter()
    w = TsvWriter(os.path.join(d, "tiers.tsv"))
    w.write("#lemma_id", "key", "head", "pos", "tier", "tier_source", "freq_rank", "whit_freq", "dcc_rank", "emoji",
            "shared_el")
    wf = TsvWriter(os.path.join(d, "freq.tsv"))
    wf.write("#lemma_id", "freq_rank", "source", "dcc_rank", "whit_freq")
    sheet_rows = []
    for info in infos:
        i = info.id
        tier, src = tiers[i]
        fr = freq_rank(i, sig)
        wl = sig["whit"].get(i, "")
        dr = sig["dcc"].get(i, 0)
        el = 1 if (info.rec is not None and info.rec.get("el") == 1) else 0
        w.write(i, info.key, info.head, info.fpos, tier, src, fr, wl, dr or "", emoji.get(i, ""), el)
        wf.write(i, fr, "dcc" if dr else ("whitaker" if fr else ""), dr or "", wl)
        c["tier%d" % tier] += 1
        if el:
            c["shared_el"] += 1
        if fr:
            c["freq_rank_dcc" if dr else "freq_rank_whitaker"] += 1
        if tier in (1, 2):
            g = gl.get(i, ("", ""))
            sheet_rows.append((tier, dr or 99999, info.key, i, info.head, info.fpos, src, wl, g[0], g[1]))
    w.close()
    wf.close()
    sheet_rows.sort()
    with open(os.path.join(d, "review_tier_sheet.csv"), "w", encoding="utf-8", newline="") as f:
        cw = csv.writer(f, lineterminator="\n")
        cw.writerow(["key", "head", "pos", "tier", "tier_source", "dcc_rank", "whitaker_freq", "gloss_en", "gloss_es",
                     "lemma_id"])
        for tier, dr, key, i, head, pos, src, wl, ge, gs in sheet_rows:
            cw.writerow([key, head, pos, tier, {0: "", 1: "derived", 2: "teacher"}[src],
                         "" if dr == 99999 else dr, wl, ge, gs, i])
    c["emoji_matched"] = len(emoji)
    c["emoji_unmatched"] = len(unmatched)
    c["curated_tier1_unmatched"] = len(missing)
    res = {"counts": dict(c), "emoji_unmatched": sorted(unmatched), "curated_unmatched": sorted(missing),
           "duration_s": round(time.time() - t0, 1), "peak_rss_mb": peak_rss_mb()}
    log("[%s tiers] done: %s" % (lang, " ".join("%s=%s" % kv for kv in sorted(c.items()))))
    return res


def outputs(lang):
    return ["tiers.tsv", "freq.tsv", "review_tier_sheet.csv"] if lang in ("la", "grc") else []
