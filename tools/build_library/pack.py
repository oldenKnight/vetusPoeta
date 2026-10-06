"""Stage `pack`: write data/work/{latin,greek,english,spanish}.vpl (DESIGN.md section 5, byte conventions of the
reference encoder engine/tests/lex_fixture_writer.cpp / tests/fixtures/lex/SPEC_CHECK.md).

  header 256 bytes: "VPLX" | u16 major | u16 minor | char[8] lang | u32 section_count | u64 file_size @20 |
                    sha256 of [256, file_size) @28 | zeros
  section table @256: section_count x {char[4] tag, u32 0, u64 offset, u64 length (unpadded)}, file order
  sections 8-byte aligned (zero gap), file ends after the last one:
  NOTE STRS KEYS ANAL LEMM SENS FEAT GENX REVX  (english/spanish: NOTE STRS KEYS ANAL LEMM FEAT)
  STRS: NUL at 0, then every distinct non-empty string sorted bytewise, NUL-terminated.
  FEAT: every packed word used by ANAL or GENX, ascending; feat_id = index.
  KEYS: keys bytewise ascending; ANAL grouped by key (input order inside a key); GENX by feat_id inside a lemma
  (stable); CAND by keyword, score desc, lemma asc, sense asc.
`encode()` is the format; `build_lang()` gathers the inputs from the earlier stages. Reproducible: the bytes depend
only on the inputs (no clock, no hash order), so two runs give the same SHA-256.
"""
import array
import collections
import hashlib
import json
import os
import struct
import sys
import time

import features as F
import lexdata
import resolve as R
import tagmap
import vptext
from common import log, peak_rss_mb, read_json, read_tsv

MAGIC = b"VPLX"
MAJOR, MINOR = 1, 0
HEADER_SIZE = 256
ES_PREFIX = "es:"
FILE_NAMES = {"la": "latin.vpl", "grc": "greek.vpl", "en": "english.vpl", "es": "spanish.vpl"}
LANG_NAMES = {"la": "Latin", "grc": "Ancient Greek", "en": "English", "es": "Spanish"}
# LEMM flags (DESIGN 5); bit8 is an addition recorded in docs/STATUS.md: gloss_es came from the EN->ES pivot
LF_NAME, LF_INDECL, LF_DEPONENT, LF_IMPERS, LF_SHARED_EL, LF_PLURAL, LF_DEFECTIVE, LF_TABLE, LF_ES_PIVOT = (
    1, 2, 4, 8, 16, 32, 64, 128, 256)
ANAL_FIX_MASK = ~(R.FLAG_ALT | R.FLAG_DIALECT) & 0xFFFF


class Lemma(object):
    __slots__ = ("head", "key", "pos", "cls", "gender", "tier", "freq_rank", "whit_freq", "tier_source", "emoji",
                 "gloss_en", "gloss_es", "senses", "flags", "cells", "principal_count", "principal")

    def __init__(self, head="", key="", pos=0, cls=0, gender=0, tier=0, freq_rank=0, whit_freq=0, tier_source=0,
                 emoji="", gloss_en="", gloss_es="", senses=None, flags=0, cells=None, principal_count=0,
                 principal=""):
        self.head, self.key, self.pos, self.cls, self.gender, self.tier = head, key, pos, cls, gender, tier
        self.freq_rank, self.whit_freq, self.tier_source, self.emoji = freq_rank, whit_freq, tier_source, emoji
        self.gloss_en, self.gloss_es = gloss_en, gloss_es
        self.senses = senses or []          # list of (gloss_en, gloss_es, keywords, tags, rank)
        self.flags = flags
        self.cells = cells or []            # list of (packed feature, form)
        self.principal_count, self.principal = principal_count, principal


class Lexicon(object):
    """Input of encode(). analyses: (key, lemma, packed, flags, display) in any order (stable-sorted by key unless
    analyses_sorted); candidates: (keyword, lemma, sense, score, pos) in any order."""

    def __init__(self, lang, notice, lemmas, analyses, candidates=(), morphology_only=False, major=MAJOR,
                 minor=MINOR, analyses_sorted=False):
        self.lang, self.notice, self.lemmas = lang, notice, lemmas
        self.analyses, self.candidates = analyses, candidates
        self.morphology_only, self.major, self.minor = morphology_only, major, minor
        self.analyses_sorted = analyses_sorted


def _u32s(values):
    a = array.array("I", values)
    if sys.byteorder != "little":
        a.byteswap()
    return a.tobytes()


def _bkey(s):
    return s.encode("utf-8")


def encode(lx, layout=None):
    """Bytes of a .vpl file for a Lexicon. layout (a list) receives (tag, offset, length) per section."""
    morph = lx.morphology_only
    # ---- ordering -------------------------------------------------------------------------------------------
    anal = lx.analyses
    if not lx.analyses_sorted:
        anal = sorted(anal, key=lambda a: _bkey(a[0]))  # stable: input order inside a key
    cand = sorted(lx.candidates, key=lambda c: (_bkey(c[0]), -c[3], c[1], c[2])) if not morph else []
    # ---- FEAT ---------------------------------------------------------------------------------------------------
    feats = set(a[2] for a in anal)
    if not morph:
        for l in lx.lemmas:
            feats.update(f for f, _ in l.cells)
    feats = sorted(feats)
    if len(feats) > 65536:
        raise ValueError("too many feature words: %d" % len(feats))
    fid = {f: i for i, f in enumerate(feats)}
    # ---- STRS ---------------------------------------------------------------------------------------------------
    strings = set()
    for a in anal:
        strings.add(a[0])
        strings.add(a[4])
    for l in lx.lemmas:
        strings.update((l.head, l.key, l.emoji, l.gloss_en, l.gloss_es, l.principal))
        if not morph:
            for s in l.senses:
                strings.update(s[:3])
            for _, form in l.cells:
                strings.add(form)
    for c in cand:
        strings.add(c[0])
    strings.discard("")
    enc = sorted(s.encode("utf-8") for s in strings)
    del strings
    soff = {"": 0}
    parts = [b"\0"]
    pos = 1
    for b in enc:
        if b"\0" in b:
            raise ValueError("string contains NUL: %r" % b[:40])
        soff[b.decode("utf-8")] = pos
        parts.append(b)
        parts.append(b"\0")
        pos += len(b) + 1
    del enc
    strs = b"".join(parts)
    del parts
    if len(strs) > 0xFFFFFFFF:
        raise ValueError("STRS larger than 4 GB")
    S = soff.__getitem__
    # ---- KEYS + ANAL ----------------------------------------------------------------------------------------------
    key_offs, starts = [], []
    arec = bytearray(12 * len(anal))
    prev = None
    for i, a in enumerate(anal):
        if a[0] != prev:
            key_offs.append(S(a[0]))
            starts.append(i)
            prev = a[0]
        struct.pack_into("<IHHI", arec, 12 * i, a[1], fid[a[2]], a[3], S(a[4]))
    starts.append(len(anal))
    keys_b = struct.pack("<I", len(key_offs)) + _u32s(key_offs) + _u32s(starts)
    anal_b = struct.pack("<I", len(anal)) + bytes(arec)
    del arec, key_offs, starts
    # ---- LEMM, SENS, GENX ---------------------------------------------------------------------------------------
    lrec = bytearray(48 * len(lx.lemmas))
    srecs, grecs = bytearray(), bytearray()
    n_s = n_g = 0
    for i, l in enumerate(lx.lemmas):
        sc = 0 if morph else len(l.senses)
        gc = 0 if morph else len(l.cells)
        if sc > 0xFFFF or gc > 0xFFFF:
            raise ValueError("lemma %d has %d senses / %d cells" % (i, sc, gc))
        struct.pack_into("<IIBBBBHBBIIIIHHIHHI", lrec, 48 * i, S(l.head), S(l.key), l.pos, l.cls, l.gender, l.tier,
                         l.freq_rank, l.whit_freq, l.tier_source, S(l.emoji), S(l.gloss_en), S(l.gloss_es), n_s, sc,
                         l.flags, n_g, gc, l.principal_count, S(l.principal))
        if morph:
            continue
        for g_en, g_es, kw, tags, rank in l.senses:
            srecs += struct.pack("<IIIHH", S(g_en), S(g_es), S(kw), tags, rank)
        n_s += sc
        cells = sorted(l.cells, key=lambda c: fid[c[0]])
        for f, form in cells:
            grecs += struct.pack("<HHI", fid[f], 0, S(form))
        n_g += gc
    lemm_b = struct.pack("<I", len(lx.lemmas)) + bytes(lrec)
    del lrec
    sens_b = struct.pack("<I", n_s) + bytes(srecs)
    genx_b = struct.pack("<I", n_g) + bytes(grecs)
    feat_b = struct.pack("<I", len(feats)) + _u32s(feats)
    # ---- REVX -------------------------------------------------------------------------------------------------------
    kw_offs, cstarts = [], []
    crec = bytearray(8 * len(cand))
    prev = None
    for i, c in enumerate(cand):
        if c[0] != prev:
            kw_offs.append(S(c[0]))
            cstarts.append(i)
            prev = c[0]
        struct.pack_into("<IHBB", crec, 8 * i, c[1], c[2], c[3], c[4])
    cstarts.append(len(cand))
    revx_b = struct.pack("<I", len(kw_offs)) + _u32s(kw_offs) + _u32s(cstarts) + bytes(crec)
    del crec
    # ---- assemble ---------------------------------------------------------------------------------------------------
    note_b = lx.notice.encode("utf-8")
    sections = [(b"NOTE", note_b), (b"STRS", strs), (b"KEYS", keys_b), (b"ANAL", anal_b), (b"LEMM", lemm_b)]
    if not morph:
        sections.append((b"SENS", sens_b))
    sections.append((b"FEAT", feat_b))
    if not morph:
        sections += [(b"GENX", genx_b), (b"REVX", revx_b)]
    out = bytearray(HEADER_SIZE)
    out[0:4] = MAGIC
    lang = lx.lang.encode("ascii")[:8]
    struct.pack_into("<HH8sI", out, 4, lx.major, lx.minor, lang, len(sections))
    table_at = len(out)
    out += bytes(24 * len(sections))
    for k, (tag, data) in enumerate(sections):
        while len(out) % 8:
            out.append(0)
        off = len(out)
        out += data
        struct.pack_into("<4sIQQ", out, table_at + 24 * k, tag, 0, off, len(data))
        if layout is not None:
            layout.append((tag.decode(), off, len(data)))
    struct.pack_into("<Q", out, 20, len(out))
    out[28:60] = hashlib.sha256(memoryview(out)[HEADER_SIZE:]).digest()
    return bytes(out)


# =================================================================================================================
# Inputs from the earlier stages
def notice_text(lang, raw):
    src = read_json(os.path.join(raw, "SOURCES.json"), {}) or {}
    kaikki_file = {"la": "kaikki-Latin.jsonl", "grc": "kaikki-AncientGreek.jsonl", "en": "kaikki-English.jsonl",
                   "es": "es-extract.jsonl.gz"}
    lines = ["vetus poeta %s lexicon (%s). Built from:" % (LANG_NAMES[lang], FILE_NAMES[lang]), ""]

    def kaikki(code):
        info = src.get(kaikki_file[code], {}) or {}
        return "%s (dump of %s)" % (info.get("url", "https://kaikki.org/"), info.get("last_modified", "unknown date"))
    if lang in ("la", "grc", "en"):
        lines.append("* Wiktionary (English edition), extracted by Wiktextract / kaikki.org, licensed CC BY-SA 4.0 "
                     "(https://creativecommons.org/licenses/by-sa/4.0/); text by the Wiktionary contributors "
                     "(https://en.wiktionary.org/). Source: " + kaikki(lang) + ".")
        if lang != "en":
            lines.append("  English->%s translations from %s." % (LANG_NAMES[lang], kaikki("en")))
    if lang in ("la", "grc", "es"):
        lines.append("* Wiktionary (Spanish edition), extracted by Wiktextract / kaikki.org, licensed CC BY-SA 4.0. "
                     "Source: " + kaikki("es") + ".")
    if lang == "la":
        lines.append("* Lewis & Short, A Latin Dictionary (Perseus Digital Library, PerseusDL/lexica), licensed CC "
                     "BY-SA 4.0 (https://github.com/PerseusDL/lexica).")
        lines.append("* Dickinson College Commentaries Latin Core Vocabulary, Christopher Francese et al., licensed "
                     "CC BY-SA 3.0 Unported (https://dcc.dickinson.edu/vocab/core-vocabulary); Spanish version "
                     "translated by Francisco Javier Pérez Cartagena.")
        lines.append("* WORDS, a Latin dictionary, by Colonel William Whitaker (USAF, Retired), Copyright William A. "
                     "Whitaker (1936-2010) (https://github.com/mk270/whitakers-words). His permission: \"This is a "
                     "free program, which means it is proper to copy it and pass it on to your friends. Consider it "
                     "a developmental item for which there is no charge. However, just for form, it is Copyrighted "
                     "(c). Permission is hereby freely given for any and all use of program and data. You can sell "
                     "it as your own, but at least tell me.\" \"All parts of the WORDS system, source code and data "
                     "files, are made freely available to anyone who wishes to use them, for whatever purpose.\"")
    if lang == "grc":
        lines.append("* Liddell, Scott, Jones, A Greek-English Lexicon (Perseus Digital Library, PerseusDL/lexica), "
                     "licensed CC BY-SA 4.0 (https://github.com/PerseusDL/lexica).")
        lines.append("* Dickinson College Commentaries Greek Core Vocabulary, licensed CC BY-SA 3.0 Unported "
                     "(https://dcc.dickinson.edu/vocab/core-vocabulary).")
    lines += ["", "This lexicon file is a derived database and is distributed under CC BY-SA 4.0. Vocabulary tiers, "
              "emoji, Spanish seed glosses and the reverse-index scores are the vetus poeta project's own work."]
    return "\n".join(lines) + "\n"


def _cls(rec):
    nums = []
    for t in rec.get("class") or []:
        if t.startswith("declension-") or t.startswith("conjugation-"):
            try:
                nums.append(int(t.rsplit("-", 1)[1]))
            except ValueError:
                pass
    return min(nums) if nums and min(nums) <= 5 else 0


def _gender(rec):
    bits = 0
    for g in rec.get("gender") or []:
        bits |= tagmap.GENDER_BITS.get(g, 0)
    return tagmap.GENDER_FROM_BITS[bits]


def lemma_flags(rec):
    cl = set(rec.get("class") or [])
    for s in rec.get("senses") or []:
        cl.update(s.get("tags") or [])
    fl = 0
    if rec.get("fpos") == "name":
        fl |= LF_NAME
    if "indeclinable" in cl:
        fl |= LF_INDECL
    if "deponent" in cl or "semi-deponent" in cl:
        fl |= LF_DEPONENT
    if "impersonal" in (rec.get("class") or []):
        fl |= LF_IMPERS
    if rec.get("el") == 1:
        fl |= LF_SHARED_EL
    if "plural-only" in cl:
        fl |= LF_PLURAL
    if "defective" in cl:
        fl |= LF_DEFECTIVE
    if rec.get("has_table"):
        fl |= LF_TABLE
    return fl


def cell_rows(lang, rows, pos, base):
    """Table rows of one lemma -> [(packed, display, flags, rank)] exactly as the resolve stage derives them."""
    parsed = {}
    contracted_twins = set()
    for row in rows:
        m = row[5] if len(row) > 5 else ""
        if m not in parsed:
            parsed[m] = R.parse_marker(m)
            if parsed[m][2]:
                contracted_twins.add(parsed[m][3] - {"contracted"})
    out = []
    stats = tagmap.TagStats()
    for n, row in enumerate(rows):
        _, display, key, tags, source, marker = (row + [""] * 6)[:6]
        tl = tags.split() if tags else []
        tense, dialects, contracted, toks = parsed[marker]
        fl = base | R.tag_flags(tl, lang)
        fl |= R.FLAG_TABLE if source != "head" else 0
        extra = 0
        if lang == "grc":
            dfl, dex = R.dialect_flags(dialects)
            fl |= dfl
            extra |= dex
            if contracted:
                extra |= F.EXTRA["contracted"]
            elif toks in contracted_twins and "Attic" not in dialects:
                fl |= R.FLAG_DIALECT
        hint = tense if lang == "grc" else None
        for packed in tagmap.tags_to_feature_list(tl, pos, tense_hint=hint, extra=extra, stats=stats):
            rank = (1 if fl & R.FLAG_ALT else 0, 1 if fl & R.FLAG_DIALECT else 0,
                    1 if fl & (R.FLAG_LATE | R.FLAG_RARE) else 0, 1 if source == "head" else 0, n)
            out.append((packed, display, fl, rank))
    return out


def choose_cells(cells):
    """One display form per packed feature: the best rank (non-alternative, Attic, classical, table row, first)."""
    best = {}
    for packed, display, fl, rank in cells:
        b = best.get(packed)
        if b is None or rank < b[1]:
            best[packed] = (display, rank)
    return [(p, best[p][0]) for p in sorted(best)]


def iter_table_groups(path):
    cur, rows = None, []
    for row in read_tsv(path):
        lid = int(row[0])
        if lid != cur:
            if cur is not None:
                yield cur, rows
            cur, rows = lid, []
        rows.append(row)
    if cur is not None:
        yield cur, rows


def build_lang(lang, out, raw, c):
    """Lexicon for one language from <out>/<lang>/ files."""
    d = os.path.join(out, lang)
    morph = lang in ("en", "es")
    recs = []
    lemmas = []
    # English / Spanish: multi-word lemmas (phrases, "kick the bucket") are left out; the NLP module looks up single
    # tokens and phrases go through the phrasebook. Their ids are renumbered densely (new_id).
    new_id = {}
    n_in = 0
    for r in lexdata.iter_lemma_records(out, lang):
        if r["id"] != n_in:
            raise ValueError("lemma ids are not sequential at %d" % r["id"])
        n_in += 1
        if morph and " " in r["key"]:
            c["lemmas_multiword_dropped"] += 1
            continue
        new_id[r["id"]] = len(lemmas)
        l = Lemma(head=r.get("head", ""), key=r["key"], pos=lexdata.pos_code(r.get("fpos", "other")),
                  gender=_gender(r), flags=lemma_flags(r))
        if not morph:
            l.cls = _cls(r)
            pp = r.get("pp", "")
            l.principal, l.principal_count = pp, (1 if pp else 0)
            recs.append((r.get("word", ""), r.get("kind", ""), r.get("late", 0)))
        else:
            recs.append((r.get("word", ""), "", 0))
        lemmas.append(l)
    log("[%s pack] %d lemmas" % (lang, len(lemmas)))
    candidates = []
    if not morph:
        for r in lexdata.read_rows(os.path.join(d, "gloss.tsv")):
            l = lemmas[int(r[0])]
            l.gloss_en, l.gloss_es = r[1], r[3]
            if r[4] == "pivot":
                l.flags |= LF_ES_PIVOT
        for r in lexdata.read_rows(os.path.join(d, "senses.tsv")):
            lemmas[int(r[0])].senses.append((r[2], r[3], r[4], int(r[5]), int(r[6])))
        for r in lexdata.read_rows(os.path.join(d, "tiers.tsv")):
            l = lemmas[int(r[0])]
            l.tier, l.tier_source, l.freq_rank = int(r[4]), int(r[5]), min(int(r[6]), 0xFFFF)
            l.whit_freq = ord(r[7]) if r[7] in ("A", "B", "C", "D", "E", "F") else 0
            l.emoji = r[9]
        for name, prefix in (("revx_en.tsv", ""), ("revx_es.tsv", ES_PREFIX)):
            for r in lexdata.read_rows(os.path.join(d, name)):
                candidates.append((prefix + r[0], int(r[1]), int(r[2]), int(r[3]), int(r[4])))
        # GENX cells
        for lid, rows in iter_table_groups(os.path.join(d, "table_forms.tsv")):
            base = (R.FLAG_ALT if recs[lid][1] == "alttable" else 0) | (R.FLAG_LATE if recs[lid][2] else 0)
            fpos = {v: k for k, v in F.POS.items()}.get(lemmas[lid].pos, "other")
            lemmas[lid].cells = choose_cells(cell_rows(lang, rows, fpos, base))
            c["genx_cells"] += len(lemmas[lid].cells)
    log("[%s pack] lemma data loaded, %d candidates" % (lang, len(candidates)))
    # ANAL from analyses.tsv (sorted by key, lemma, packed, display); the headword fix
    analyses = []
    for r in read_tsv(os.path.join(d, "analyses.tsv")):
        key, lid, packed, display, fl = r[0], int(r[1]), int(r[2]), r[3], int(r[4])
        if morph:
            lid = new_id.get(lid)
            if lid is None or " " in key:
                c["analyses_multiword_dropped"] += 1
                continue
        if fl & (R.FLAG_ALT | R.FLAG_DIALECT):
            l = lemmas[lid]
            if display == l.head or display == recs[lid][0] or vptext.nfc(display) == vptext.nfc(l.head):
                fl &= ANAL_FIX_MASK
                c["anal_headword_fixed"] += 1
        analyses.append((key, lid, packed, fl, display))
    c["analyses"] = len(analyses)
    for i in range(1, len(analyses)):
        if _bkey(analyses[i - 1][0]) > _bkey(analyses[i][0]):
            raise ValueError("analyses.tsv is not sorted bytewise at row %d" % i)
    c["lemmas"] = len(lemmas)
    c["candidates"] = len(candidates)
    c["senses"] = sum(len(l.senses) for l in lemmas)
    return Lexicon(lang, notice_text(lang, raw), lemmas, analyses, candidates, morphology_only=morph,
                   analyses_sorted=True)


def run(lang, out, raw):
    t0 = time.time()
    c = collections.Counter()
    lx = build_lang(lang, out, raw, c)
    t1 = time.time()
    layout = []
    data = encode(lx, layout)
    del lx
    path = os.path.join(out, FILE_NAMES[lang])
    tmp = path + ".tmp"
    with open(tmp, "wb") as f:
        f.write(data)
    os.replace(tmp, path)
    sha = hashlib.sha256(data).hexdigest()
    c["size"] = len(data)
    res = {"counts": dict(c), "file": FILE_NAMES[lang], "size": len(data), "sha256": sha,
           "sections": {t: {"offset": o, "length": n} for t, o, n in layout},
           "load_s": round(t1 - t0, 1), "encode_s": round(time.time() - t1, 1),
           "duration_s": round(time.time() - t0, 1), "peak_rss_mb": peak_rss_mb()}
    log("[%s pack] wrote %s: %d bytes, sha256 %s" % (lang, path, len(data), sha))
    return res


def outputs(lang):
    return []


def write_file(path, lx):
    data = encode(lx)
    with open(path, "wb") as f:
        f.write(data)
    return data


if __name__ == "__main__":
    import argparse
    ap = argparse.ArgumentParser(description="pack one language (normally run through build.py --stage pack)")
    ap.add_argument("--out", default="data/work")
    ap.add_argument("--raw", default="data/raw")
    ap.add_argument("lang")
    a = ap.parse_args()
    print(json.dumps(run(a.lang, a.out, a.raw), indent=1))
