"""Shared loaders for the B4 stages (import_aux, gloss, tiers, pack): Kaikki lemma records and the auxiliary TSVs.

Everything here reads files written by earlier stages; nothing touches data/raw except through import_aux.
"""
import json
import os

import features as F
import vptext
from common import read_tsv

# Kaikki fpos (features.POS names) that a dictionary part of speech may join to
POS_COMPAT = {"noun": ("noun", "name"), "name": ("name", "noun"), "verb": ("verb",), "adj": ("adj", "participle", "num", "det"),
              "adv": ("adv", "conj", "particle"), "prep": ("prep", "postp"), "conj": ("conj",), "intj": ("intj",), "num": ("num", "adj"),
              "pron": ("pron", "det", "adj"), "det": ("det", "pron", "adj"), "particle": ("particle", "adv", "conj"),
              "article": ("article", "det", "pron"), "participle": ("participle", "adj", "verb")}


class LemmaInfo(object):
    __slots__ = ("id", "key", "word", "head", "fpos", "kind", "has_table", "pp", "rec")

    def __init__(self, r, keep_rec):
        self.id = r["id"]
        self.key = r["key"]
        self.word = r.get("word", "")
        self.head = r.get("head", "")
        self.fpos = r.get("fpos", "other")
        self.kind = r.get("kind", "lemma")
        self.has_table = bool(r.get("has_table"))
        self.pp = r.get("pp", "")
        self.rec = r if keep_rec else None


def iter_lemma_records(out, lang):
    with open(os.path.join(out, lang, "lemmas.jsonl"), "r", encoding="utf-8") as f:
        for line in f:
            yield json.loads(line)


def load_lemmas(out, lang, keep_rec=False):
    """(list of LemmaInfo indexed by id, dict key -> [ids])."""
    infos = []
    by_key = {}
    for r in iter_lemma_records(out, lang):
        if r["id"] != len(infos):
            raise ValueError("lemma ids are not sequential at %d" % r["id"])
        infos.append(LemmaInfo(r, keep_rec))
        by_key.setdefault(r["key"], []).append(r["id"])
    return infos, by_key


def pick_lemmas(ids, infos, want_pos=None, hint_tokens=(), key_fn=None):
    """Choose the Kaikki lemma ids a dictionary headword stands for: compatible POS (when given), lemma kind
    before form/alt tables, then the best overlap of the hint tokens (other principal parts) with the Kaikki
    head line. Returns the best ids (several only when they tie), sorted."""
    if not ids:
        return []
    cand = list(ids)
    if want_pos:
        ok = POS_COMPAT.get(want_pos, (want_pos,))
        comp = [i for i in cand if infos[i].fpos in ok]
        if not comp:
            return []
        cand = comp
    lem = [i for i in cand if infos[i].kind == "lemma"]
    if lem:
        cand = lem
    if hint_tokens and len(cand) > 1 and key_fn is not None:
        toks = [t for t in (key_fn(x).strip("-") for x in hint_tokens) if len(t) >= 2]
        if toks:
            def score(i):
                pp = key_fn(infos[i].pp or "")
                return sum(1 for t in toks if t in pp)
            best = max(score(i) for i in cand)
            cand = [i for i in cand if score(i) == best]
    return sorted(cand)


def ids_field(ids):
    return ",".join(str(i) for i in ids)


def parse_ids(s):
    return [int(x) for x in s.split(",") if x] if s else []


def read_rows(path, header=True):
    """Rows of a TSV written by these stages (first line is a `#` header when header is true)."""
    if not os.path.exists(path):
        return
    first = True
    for row in read_tsv(path):
        if first and header:
            first = False
            if row and row[0].startswith("#"):
                continue
        first = False
        yield row


def read_curated(path):
    """Rows of a data/curated TSV: `#` comment lines and blank lines skipped, fields stripped."""
    if not os.path.exists(path):
        return []
    rows = []
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\n").rstrip("\r")
            if not line.strip() or line.lstrip().startswith("#"):
                continue
            rows.append([x.strip() for x in line.split("\t")])
    return rows


def pos_code(fpos):
    return F.POS.get(fpos, F.POS["other"])


def key_fn(lang):
    return vptext.key_for(lang)


def load_alt_targets(out, lang):
    """key -> sorted target keys of alternative-spelling form pages (formpages.tsv rows of kind `alt`), so that a
    dictionary headword written in another spelling (ac -> atque, adcessus -> accessus) still joins."""
    res = {}
    path = os.path.join(out, lang, "formpages.tsv")
    if not os.path.exists(path):
        return res
    for row in read_tsv(path):
        if len(row) > 6 and row[6] == "alt" and row[3] and row[3] != row[1]:
            res.setdefault(row[1], set()).add(row[3])
    return {k: sorted(v) for k, v in res.items()}


def join_key(key, infos, by_key, alt, want_pos=None, hints=(), key_fn=None):
    """pick_lemmas on the key, then on its alternative-spelling targets. Returns (ids, how) with how in
    ('exact', 'alt', '')."""
    ids = pick_lemmas(by_key.get(key), infos, want_pos, hints, key_fn)
    if ids:
        return ids, "exact"
    for t in alt.get(key, ()):
        ids = pick_lemmas(by_key.get(t), infos, want_pos, hints, key_fn)
        if ids:
            return ids, "alt"
    return [], ""
