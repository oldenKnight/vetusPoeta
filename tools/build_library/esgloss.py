"""English one-line glosses for spanish.vpl lemmas (B4c), so the engine's Spanish->Latin fallback can pivot through
English when nothing Spanish names a word (engine/rules/src/transfer/transfer.cpp, "English pivot (C13)": it reads the
first item of LEMM.gloss_en before , ; ( and strips a leading "to ").

Sources, both from the earlier kaikki stage:
  <out>/en/translations_es.tsv  English word, pos, sense, Spanish target, tags (English edition; inverted here:
                                Spanish lemma -> the English source words that list it)
  <out>/es/translations_en.tsv  Spanish word, pos, sense_index, English word, tags (Spanish edition, entry level)
Score of an English word W for a Spanish lemma (same key; English-edition rows of a compatible part of speech, the
Spanish-edition rows of the same entry part of speech):
  +2 W lists the lemma as the first Spanish translation of one of its senses (+1 when only later)
  +1 W is the first English translation of the Spanish-edition entry, +1 W is one of its English translations
  +1 W is a common English lemma (>= 20 occurrences in the UD English treebank)
The sum over senses is not used: an English word with many senses that all list the lemma ("gate" for puerta)
must not beat the plain equivalent ("door"). Ties: the UD count, then the number of English-edition rows of W, then the
shorter, then alphabetical. English words are compared case-insensitively when both spellings compete ("Son" / "son"
-> son), a leading "to " is dropped (verbs get it back), words with "/" or digits are skipped. The gloss is the best
three words, ", "-joined; verbs read "to come, arrive". Weights chosen on 115 common Spanish words (the first item
is the expected English word for 103; docs/LIBRARY_CHANGES.md, B4c). Deterministic: no hash order, no clock.
"""
import collections
import glob
import os

import vptext
from common import read_tsv

MAX_WORDS = 3
MAX_LEN = 60
UD_GLOB = "en_ewt-ud-*.conllu"
COMMON = 20
# Spanish lemma fpos (features.POS names) -> English-edition parts of speech whose rows count
# (a Spanish proper name takes only English proper-name rows: "Puerta" the surname is not "door")
EN_POS = {"noun": ("noun", "name"), "name": ("name",), "verb": ("verb",), "adj": ("adj",), "adv": ("adv",),
          "pron": ("pron",), "det": ("det", "pron", "article"), "article": ("article", "det"), "prep": ("prep",),
          "postp": ("postp", "prep"), "conj": ("conj",), "intj": ("intj",), "num": ("num", "adj"),
          "particle": ("particle", "adv"), "participle": ("adj", "verb")}


def input_files(out, raw=None):
    files = [os.path.join(out, "en", "translations_es.tsv"), os.path.join(out, "es", "translations_en.tsv")]
    if raw:
        files += ud_files(raw)
    return files


def ud_files(raw):
    return sorted(glob.glob(os.path.join(raw, "ud", UD_GLOB)))


def ud_counts(raw):
    """English lemma (lower-case) -> occurrences in the UD English treebank files (LEMMA column)."""
    c = collections.Counter()
    for path in ud_files(raw or ""):
        with open(path, "r", encoding="utf-8") as f:
            for line in f:
                if not line[:1].isdigit():
                    continue
                cols = line.split("\t")
                if len(cols) > 3 and "-" not in cols[0] and "." not in cols[0] and cols[2] != "_":
                    c[cols[2].lower()] += 1
    return c


def _en_word(w):
    w = vptext.nfc(w or "").strip()
    if w[:3].lower() == "to " and len(w) > 3:
        w = w[3:].strip()
    if not w or "\t" in w or "/" in w or any(ch.isdigit() for ch in w):
        return ""
    return w


def collect(out, want_keys):
    """-> (en_rows {es_key: [(en_word, en_pos, first)]}, es_rows {(es_key, es_pos): [en_word, ...]} (entry order),
    en_freq {en_word: English-edition rows}) for the Spanish keys in want_keys."""
    en_rows = collections.defaultdict(list)
    en_freq = collections.Counter()
    path = os.path.join(out, "en", "translations_es.tsv")
    if os.path.exists(path):
        group = None
        for r in read_tsv(path):
            if len(r) < 4:
                continue
            ew = _en_word(r[0])
            if not ew:
                continue
            en_freq[ew] += 1
            g = (r[0], r[1], r[2])
            first = g != group
            group = g
            t = vptext.nfc(r[3]).strip()
            if not t or " " in t:
                continue
            k = vptext.es_key(t)
            if k in want_keys:
                en_rows[k].append((ew, r[1], first))
    es_rows = collections.defaultdict(list)
    path = os.path.join(out, "es", "translations_en.tsv")
    if os.path.exists(path):
        for r in read_tsv(path):
            if len(r) < 4:
                continue
            ew = _en_word(r[3])
            k = vptext.es_key(r[0])
            if ew and k in want_keys:
                es_rows[(k, r[1])].append(ew)
    return en_rows, es_rows, en_freq


def gloss_for(fpos, kpos, key, en_rows, es_rows, en_freq, ud=None):
    """(gloss, source) for one Spanish lemma record; source in ("both", "en", "es", "")."""
    ud = ud or {}
    ok = EN_POS.get(fpos, (fpos,))
    first_any = {}
    src = set()
    for ew, pos, first in en_rows.get(key, ()):
        if pos in ok:
            first_any[ew] = max(first_any.get(ew, 0), 2 if first else 1)
            src.add("en")
    esl = es_rows.get((key, kpos), [])
    if esl:
        src.add("es")
    cands = set(first_any) | set(esl)
    if not cands:
        return "", ""
    # case-insensitive merge when both spellings compete
    lower = {}
    for w in sorted(cands):
        lower.setdefault(w.lower(), []).append(w)
    canon = {}
    for lw, ws in lower.items():
        pick = lw if lw in ws else ws[0]
        for w in ws:
            canon[w] = pick
    score = collections.Counter()
    first_c = {}
    for w, v in first_any.items():
        cw = canon[w]
        first_c[cw] = max(first_c.get(cw, 0), v)
    es_first = canon[esl[0]] if esl else None
    es_set = set(canon[w] for w in esl)
    for cw in set(canon.values()):
        n = ud.get(cw.lower(), 0)
        score[cw] = (first_c.get(cw, 0) + (1 if cw == es_first else 0) + (1 if cw in es_set else 0)
                     + (1 if n >= COMMON else 0))
    best = sorted(score, key=lambda w: (-score[w], -ud.get(w.lower(), 0), -en_freq.get(w, 0), len(w), w))
    best = best[:MAX_WORDS]
    if fpos == "verb":
        text = "to " + ", ".join(best)
    else:
        text = ", ".join(best)
    while len(text) > MAX_LEN and ", " in text:
        text = text[:text.rfind(", ")]
    return text, ("both" if len(src) == 2 else src.pop())


def apply(out, records, lemmas, c, raw=None):
    """records: [(lemma index, key, Kaikki pos, fpos)] of the lemmas in the file; lemmas: pack.Lemma list; raw: the
    raw directory (UD English counts). Sets gloss_en; counts in c."""
    want = set(k for _, k, _, _ in records)
    en_rows, es_rows, en_freq = collect(out, want)
    ud = ud_counts(raw)
    c["gloss_en_ud_lemmas"] = len(ud)
    for i, key, kpos, fpos in records:
        g, src = gloss_for(fpos, kpos, key, en_rows, es_rows, en_freq, ud)
        if g:
            lemmas[i].gloss_en = g
            c["gloss_en_" + src] += 1
            c["gloss_en_covered"] += 1
        else:
            c["gloss_en_missing"] += 1
