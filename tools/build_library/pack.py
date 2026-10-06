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
B4b additions (docs/LIBRARY_CHANGES.md):
  * LEMM flag deponent (bit2) also for a Greek verb in -μαι without Attic present active cells and a Latin -or verb
    whose present active cells are all -r forms;
  * Greek: data/curated/lexicon_overrides_grc.tsv sets GENX cells (δεῖ, ἔδει, ἦσθα, βούλει, οἴει) and builds a
    compound's missing tenses from its simplex (ἀποφεύγω aorist from φεύγω); the ANAL rows follow;
  * Latin: data/curated/macron_overrides.tsv applied to the lemma's cells and analyses (narrā -> nārrā; the headword
    stays the dictionary's); Whitaker-only analyses (whitaker_gen.py, ANAL bit2) for keys Kaikki does not know;
  * English / Spanish: only the lemmas morphcut.py selects (UD treebanks + frequency proxy + curated words), ANAL
    display = key (display strings dropped).
B4c additions (docs/LIBRARY_CHANGES.md, "B4c"):
  * Greek: whole lemmas from data/curated/lexicon_supplement_grc.tsv (supplement.py), appended after the Kaikki ids;
  * Greek overrides: `old` = "alt [forms]" / "drop [forms]" (the replaced cell form, or the listed forms, become an
    alternative (ANAL bit3) / lose their analysis of that cell); "@gender" sets the lemma gender; "@alt-table <marker>"
    ranks a whole table of the lemma as an alternative (its cells lose to the other tables, its analyses that no other
    table gives get bit3);
  * Spanish: LEMM.gloss_en from the English edition's translations (inverted) and the Spanish edition's English
    translations (esgloss.py).
"""
import array
import collections
import hashlib
import json
import os
import re
import struct
import sys
import time
import unicodedata

import esgloss
import features as F
import grcfix
import lexdata
import morphcut
import resolve as R
import supplement
import tagmap
import vptext
import whitaker_gen
from common import log, peak_rss_mb, read_json, read_tsv

HERE = os.path.dirname(os.path.abspath(__file__))
CURATED = os.path.normpath(os.path.join(HERE, "..", "..", "data", "curated"))
GRC_OVERRIDES = "lexicon_overrides_grc.tsv"
LA_MACRONS = "macron_overrides.tsv"

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


HEAD_KEEP = frozenset("'\u2019\u02bc\u1fbd\u1fbf-\u2010")  # apostrophes (also Greek koronis / psili), hyphens


def _head_char(ch):
    return unicodedata.category(ch)[0] in "LNM" or ch in HEAD_KEEP


def clean_head(s):
    """Headword without leading / trailing characters that are not letters, digits, combining marks, apostrophes or
    hyphens ("((caelum" -> caelum, "*dia" -> dia, "ōh!" -> ōh); a final period right after a letter or digit stays (an
    abbreviation: "e.g.", "lat."). A head of punctuation only becomes "" (dropped). B4c."""
    i, j = 0, len(s)
    while i < j and not _head_char(s[i]):
        i += 1
    while j > i and not _head_char(s[j - 1]):
        if s[j - 1] == "." and j - 1 > i and unicodedata.category(s[j - 2])[0] in "LN":
            break
        j -= 1
    return s[i:j]


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


def cell_rows(lang, rows, pos, base, alt_markers=frozenset()):
    """Table rows of one lemma -> [(packed, display, flags, rank)] exactly as the resolve stage derives them.
    alt_markers: table markers whose rows count as alternative (FLAG_ALT, so they lose the GENX choice; B4c)."""
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
        if marker in alt_markers:
            fl |= R.FLAG_ALT
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


# ---- B4b helpers ---------------------------------------------------------------------------------------------------
ACTIVE, PRESENT, INDIC = F.VOICE["active"], F.TENSE["present"], F.MOOD["indicative"]
FPOS_NAME = {v: k for k, v in F.POS.items()}


def deponent_signal(lang, fpos, head, key, cells):
    """Greek: a verb in -μαι without an Attic (or unmarked) present active cell. Latin: a verb in -or whose present
    active 1st singular indicative cells are all -r forms (the table is a deponent's)."""
    if fpos != "verb":
        return False
    if lang == "grc":
        if not grcfix.plain(head).endswith("\u03bc\u03b1\u03b9"):
            return False
        for packed, _display, fl, rank in cells:
            u = F.unpack(packed)
            if rank[3] == 0 and u["voice"] == ACTIVE and u["tense"] == PRESENT and not fl & R.FLAG_DIALECT:
                return False
        return True
    if lang == "la":
        if not key.endswith("or"):
            return False
        found = False
        for packed, display, _fl, rank in cells:
            u = F.unpack(packed)
            if (rank[3] == 0 and u["voice"] == ACTIVE and u["tense"] == PRESENT and u["mood"] == INDIC
                    and u["person"] == 1 and u["number"] == 1):
                if not vptext.latin_key(display).endswith("r"):
                    return False
                found = True
        return found
    return False


def fill_cells(lang, lid, rows, lemmas, recs, c, displays=None, alt_tables=None, alt_only=None):
    """GENX cells of one lemma from its table rows; the deponent flag from the same rows. alt_tables {lid: markers}
    (override "@alt-table"): those tables' rows rank as alternatives; alt_only[lid] receives the (key, packed) pairs
    that only those tables give (their analyses get bit3)."""
    base = (R.FLAG_ALT if recs[lid][1] == "alttable" else 0) | (R.FLAG_LATE if recs[lid][2] else 0)
    l = lemmas[lid]
    fpos = FPOS_NAME.get(l.pos, "other")
    alt_m = (alt_tables or {}).get(lid, frozenset())
    cr = cell_rows(lang, rows, fpos, base, alt_m)
    if alt_m and alt_only is not None:
        main, alt = set(), set()
        for row in rows:
            m = row[5] if len(row) > 5 else ""
            for packed, _d, _f, _r in cell_rows(lang, [row], fpos, base):
                (alt if m in alt_m else main).add((row[2], packed))
        alt_only[lid] = alt - main
        c["override_alt_table_pairs"] += len(alt_only[lid])
    l.cells = choose_cells(cr)
    c["genx_cells"] += len(l.cells)
    if not l.flags & LF_DEPONENT and deponent_signal(lang, fpos, l.head, l.key, cr):
        l.flags |= LF_DEPONENT
        c["deponent_flag_added"] += 1
    if displays is not None and lid in displays:
        for row in rows:
            displays[lid].setdefault(row[2], row[1])


def stash_rows(path, ids):
    """Table rows of the given lemma ids (the simplex verbs of compound overrides)."""
    out = {}
    if not ids:
        return out
    for lid, rows in iter_table_groups(path):
        if lid in ids:
            out[lid] = rows
    return out


def parse_old(text):
    """`old` column -> (mode, [form keys]): keep | dialect | alt [forms] | drop [forms] (B4c: alt / drop)."""
    parts = (text or "keep").split()
    mode = parts[0] if parts and parts[0] in ("keep", "dialect", "alt", "drop") else None
    if mode is None:
        return None, []
    return mode, [vptext.greek_key(x) for x in parts[1:]]


def load_grc_overrides(cdir, lemmas, c):
    """data/curated/lexicon_overrides_grc.tsv -> {"cells": [(lid, [packed], form, (mode, forms))], "compounds": {lid:
    (simplex lid, prefix, tenses)}, "gender": {lid: gender}, "alt_tables": {lid: frozenset(markers)}}. Columns: key
    (greek_key of the lemma headword), tags (Kaikki-style, with the tense and "Attic" when the table cell is
    Attic-marked) or "@from <tense> ...", "@gender", "@alt-table", form (or "<prefix>+<simplex key>", the gender words,
    the table marker), old ("dialect": the other forms' analyses of this cell get ANAL bit4; "alt [forms]": the replaced
    form, or the listed forms, get bit3 for this cell; "drop [forms]": their analyses of this cell are removed;
    "keep"), note."""
    res = {"cells": [], "compounds": {}, "gender": {}, "alt_tables": {}}
    by_key = {}
    for i, l in enumerate(lemmas):
        if l.pos == F.POS["verb"] or l.flags & LF_TABLE:
            by_key.setdefault(l.key, []).append(i)
    for row in lexdata.read_curated(os.path.join(cdir, GRC_OVERRIDES)):
        row = (row + [""] * 5)[:5]
        key, tags, form = vptext.greek_key(row[0]), row[1], vptext.nfc(row[2])
        old = parse_old(row[3])
        ids = [i for i in by_key.get(key, []) if lemmas[i].flags & LF_TABLE or tags.startswith("@from")]
        if not ids or not form or old[0] is None:
            c["override_rows_unmatched"] += 1
            log("[grc pack] override row without a lemma (or a bad `old`): %s" % row[0])
            continue
        if tags == "@gender":
            bits = 0
            for g in form.split():
                bits |= tagmap.GENDER_BITS.get(g, 0)
            if not bits:
                c["override_rows_unmatched"] += 1
                continue
            for i in ids:
                res["gender"][i] = tagmap.GENDER_FROM_BITS[bits]
            c["override_gender_rows"] += 1
            continue
        if tags == "@alt-table":
            for i in ids:
                res["alt_tables"][i] = res["alt_tables"].get(i, frozenset()) | frozenset([form])
            c["override_alt_table_rows"] += 1
            continue
        if tags.startswith("@from"):
            tenses = frozenset(tags.split()[1:])
            prefix, _, simplex = form.partition("+")
            sids = [i for i in by_key.get(vptext.greek_key(simplex), []) if lemmas[i].flags & LF_TABLE]
            if not sids or not prefix:
                c["override_rows_unmatched"] += 1
                continue
            for i in ids:
                res["compounds"][i] = (min(sids), prefix, tenses)
            c["override_compound_rows"] += 1
            continue
        for i in ids:
            fpos = FPOS_NAME.get(lemmas[i].pos, "other")
            packs = tagmap.tags_to_feature_list(tags.split(), fpos, stats=tagmap.TagStats())
            res["cells"].append((i, packs, form, old))
        c["override_cell_rows"] += 1
    return res


def compound_rows(lid, rows, compounds, simplex_rows, extra_anal, c):
    """Table rows for the tenses a compound lacks, built from its simplex (grcfix.compound); their ANAL rows go to
    extra_anal."""
    if lid not in compounds:
        return []
    sid, prefix, tenses = compounds[lid]
    have = set(R.parse_marker(r[5] if len(r) > 5 else "")[0] for r in rows if len(r) > 4 and r[4] != "head")
    out = []
    for r in simplex_rows.get(sid, []):
        r = (r + [""] * 6)[:6]
        tense, dialects, _contracted, _toks = R.parse_marker(r[5])
        if r[4] == "head" or tense not in tenses or tense in have:
            continue
        if dialects and "Attic" not in dialects:
            continue
        tags = r[3].split()
        disp, ok = grcfix.compound(prefix, r[1], not ("infinitive" in tags or "participle" in tags))
        if not ok:
            c["compound_multiword_skipped"] += 1
            continue
        out.append([str(lid), disp, vptext.greek_key(disp), r[3], r[4], r[5]])
    if out:
        for packed, display, fl, _rank in cell_rows("grc", out, "verb", 0):
            extra_anal.append((vptext.greek_key(display), lid, packed, fl, display))
        c["compound_cells"] += len(out)
    return out


def apply_cell_overrides(cells_ov, lemmas, extra_anal, displays, c):
    """Set the overridden GENX cells; ANAL rows for the new forms. Returns (demote, marks): demote {(lid, packed): new
    key} for the cells whose other forms are demoted to non-Attic ("dialect"); marks {(lid, packed, form key): "alt" |
    "drop"} for the replaced (or listed) forms."""
    demote, marks = {}, {}
    for lid, packs, form, (mode, forms) in cells_ov:
        l = lemmas[lid]
        key = vptext.greek_key(form)
        disp = displays.get(lid, {}).get(key, form)
        cells = dict(l.cells)
        for packed in packs:
            prev = cells.get(packed)
            cells[packed] = disp
            c["override_cells_set" if prev is not None else "override_cells_added"] += 1
            if prev == disp:
                c["override_cells_unchanged"] += 1
            extra_anal.append((key, lid, packed, R.FLAG_TABLE, disp))
            if mode == "dialect":
                demote[(lid, packed)] = key
            elif mode in ("alt", "drop"):
                for k in (forms or ([vptext.greek_key(prev)] if prev is not None else [])):
                    if k != key:
                        marks[(lid, packed, k)] = mode
        l.cells = sorted(cells.items())
    return demote, marks


def load_macron_overrides(cdir, lemmas, c):
    """{lid: (stem_from, stem_to)} from data/curated/macron_overrides.tsv (key, stem_from, stem_to, note)."""
    rows = {}
    for row in lexdata.read_curated(os.path.join(cdir, LA_MACRONS)):
        if len(row) >= 3 and row[0] and row[1] and row[2]:
            rows[vptext.latin_key(row[0])] = (vptext.nfc(row[1]), vptext.nfc(row[2]))
    res = {}
    for i, l in enumerate(lemmas):
        if l.key in rows:
            frm, to = rows[l.key]
            if macron_apply(l.head, frm, to) != l.head:
                res[i] = (frm, to)
                c["macron_override_lemmas"] += 1
    return res


def macron_apply(s, frm, to, words=False):
    """Replace the stem prefix (also capitalised); words=True: at the start of every word (principal parts)."""
    if not s or not frm:
        return s
    cap_f, cap_t = frm[:1].upper() + frm[1:], to[:1].upper() + to[1:]
    if words:
        s = re.sub(r"(?<!\w)" + re.escape(frm), to, s)
        return re.sub(r"(?<!\w)" + re.escape(cap_f), cap_t, s)
    if s.startswith(frm):
        return to + s[len(frm):]
    if s.startswith(cap_f):
        return cap_t + s[len(cap_f):]
    return s


def whitaker_rows(out, lemmas, recs, analyses, c):
    """Whitaker-only ANAL rows (whitaker_gen); also written to <out>/la/whitaker_only.tsv for review."""
    need = set()
    for r in lexdata.read_rows(os.path.join(out, "la", "whitaker.tsv")):
        if len(r) > 14:
            need.update(lexdata.parse_ids(r[14]))
    known = set((a[0], a[1]) for a in analyses if a[1] in need)
    all_keys = set(a[0] for a in analyses)
    cellset = set((i, f) for i in need for f, _ in lemmas[i].cells)
    wc = collections.Counter()
    rows = whitaker_gen.run_for_pack(out, [r[3] for r in recs], [r[4] for r in recs],
                                     lambda k, i: (k, i) in known, wc, lambda i, f: (i, f) in cellset,
                                     all_keys.__contains__)
    for k, v in wc.items():
        c["whitaker_" + k] = v
    with open(os.path.join(out, "la", "whitaker_only.tsv"), "w", encoding="utf-8", newline="\n") as f:
        f.write("#key\tlemma_id\thead\tpacked\tfeatures\tflags\tdisplay\n")
        for key, lid, packed, fl, display in rows:
            f.write("%s\t%d\t%s\t%d\t%s\t%d\t%s\n" % (key, lid, lemmas[lid].head, packed, tagmap.describe(packed),
                                                         fl, display))
    return rows


def merge_analyses(analyses, extra, c):
    """analyses (sorted like analyses.tsv) + extra rows -> one list sorted by key, lemma, features, display;
    duplicates merged with resolve.merge_flags."""
    if not extra:
        return analyses
    d = {}
    for key, lid, packed, fl, display in analyses:
        d[(key, lid, packed, display)] = fl
    for key, lid, packed, fl, display in extra:
        k = (key, lid, packed, display)
        if k in d:
            d[k] = R.merge_flags(d[k], fl)
            c["extra_analyses_merged"] += 1
        else:
            d[k] = fl
            c["extra_analyses_added"] += 1
    return [(k[0], k[1], k[2], fl, k[3]) for k, fl in sorted(d.items())]


def morph_selection(lang, out, raw, curated, c):
    """English / Spanish: ids of the lemma records that go into the file (morphcut.select)."""
    records = []
    for r in lexdata.iter_lemma_records(out, lang):
        if " " not in r["key"]:
            records.append((r["id"], r["key"], int(r.get("ns", 0) or 0)))
    want = set(i for i, _, _ in records)
    form_keys = {}
    for r in read_tsv(os.path.join(out, lang, "analyses.tsv")):
        i = int(r[1])
        if i in want and r[0] != "" and " " not in r[0]:
            form_keys.setdefault(i, set()).add(r[0])
    sc = collections.Counter()
    keep = morphcut.select(records, form_keys, out, raw, curated, lang, sc)
    for k, v in sc.items():
        c["select_" + k] = v
    return keep


def build_lang(lang, out, raw, c, curated=None, select=True):
    """Lexicon for one language from <out>/<lang>/ files. curated: data/curated (default the repository's);
    select: apply the English/Spanish lemma selection (morphcut)."""
    d = os.path.join(out, lang)
    cdir = curated or CURATED
    morph = lang in ("en", "es")
    recs = []
    lemmas = []
    keep = morph_selection(lang, out, raw, cdir, c) if (morph and select) else None
    # English / Spanish: multi-word lemmas (phrases, "kick the bucket") are left out; the NLP module looks up single
    # tokens and phrases go through the phrasebook. Their ids are renumbered densely (new_id).
    new_id = {}
    n_in = 0
    es_recs = []  # Spanish: (index, key, Kaikki pos, fpos) for the English glosses (esgloss)
    for r in lexdata.iter_lemma_records(out, lang):
        if r["id"] != n_in:
            raise ValueError("lemma ids are not sequential at %d" % r["id"])
        n_in += 1
        if morph and " " in r["key"]:
            c["lemmas_multiword_dropped"] += 1
            continue
        if keep is not None and r["id"] not in keep:
            c["lemmas_not_selected"] += 1
            continue
        new_id[r["id"]] = len(lemmas)
        head = r.get("head", "")
        ch = clean_head(head)
        if ch != head:
            c["heads_cleaned"] += 1
            if not ch:
                c["heads_dropped"] += 1
        l = Lemma(head=ch, key=r["key"], pos=lexdata.pos_code(r.get("fpos", "other")),
                  gender=_gender(r), flags=lemma_flags(r))
        if not morph:
            l.cls = _cls(r)
            pp = r.get("pp", "")
            l.principal, l.principal_count = pp, (1 if pp else 0)
            recs.append((r.get("word", ""), r.get("kind", ""), r.get("late", 0), r.get("fpos", "other"),
                         bool(r.get("has_table"))))
        else:
            recs.append((r.get("word", ""), "", 0, r.get("fpos", "other"), False))
            if lang == "es":
                es_recs.append((len(lemmas), r["key"], r.get("pos", ""), r.get("fpos", "other")))
        lemmas.append(l)
    log("[%s pack] %d lemmas" % (lang, len(lemmas)))
    if lang == "es":
        esgloss.apply(out, es_recs, lemmas, c, raw)
        log("[es pack] gloss_en for %d of %d lemmas" % (c["gloss_en_covered"], len(lemmas)))
    del es_recs
    candidates = []
    demote, marks, alt_only = {}, {}, {}
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
        # Greek supplement lemmas (B4c): appended after the Kaikki ids, with their cells, analyses and candidates
        extra_anal = []
        if lang == "grc":
            supplement.add_to(lemmas, recs, extra_anal, candidates, cdir, c, Lemma, cell_rows, choose_cells, LF_TABLE)
        # GENX cells (+ compound tenses built from a simplex, Greek overrides)
        ov = load_grc_overrides(cdir, lemmas, c) if lang == "grc" else {"cells": [], "compounds": {}, "gender": {},
                                                                         "alt_tables": {}}
        for lid, g in sorted(ov["gender"].items()):
            if lemmas[lid].gender != g:
                lemmas[lid].gender = g
                c["override_gender_set"] += 1
        table_path = os.path.join(d, "table_forms.tsv")
        simplex_rows = stash_rows(table_path, set(s for s, _, _ in ov["compounds"].values()))
        seen = set()
        displays = {lid: {} for lid, _p, _f, _o in ov["cells"]}
        for lid, rows in iter_table_groups(table_path):
            seen.add(lid)
            rows = rows + compound_rows(lid, rows, ov["compounds"], simplex_rows, extra_anal, c)
            fill_cells(lang, lid, rows, lemmas, recs, c, displays, ov["alt_tables"], alt_only)
        for lid in sorted(ov["compounds"]):
            if lid not in seen:
                rows = compound_rows(lid, [], ov["compounds"], simplex_rows, extra_anal, c)
                if rows:
                    fill_cells(lang, lid, rows, lemmas, recs, c, displays, ov["alt_tables"], alt_only)
        demote, marks = apply_cell_overrides(ov["cells"], lemmas, extra_anal, displays, c)
        if lang == "la":
            macron = load_macron_overrides(cdir, lemmas, c)
            for lid, (frm, to) in sorted(macron.items()):
                # cells and analyses only: the headword stays the dictionary's (the engines and tests name the
                # lemma by it); the readers' quantities are what the realiser writes
                l = lemmas[lid]
                l.cells = [(f, macron_apply(form, frm, to)) for f, form in l.cells]
        else:
            macron = {}
    log("[%s pack] lemma data loaded, %d candidates" % (lang, len(candidates)))
    # ANAL from analyses.tsv (sorted by key, lemma, packed, display); the headword fix
    analyses = []
    for r in read_tsv(os.path.join(d, "analyses.tsv")):
        key, lid, packed, display, fl = r[0], int(r[1]), int(r[2]), r[3], int(r[4])
        if morph:
            if " " in key:
                c["analyses_multiword_dropped"] += 1
                continue
            lid = new_id.get(lid)
            if lid is None:
                c["analyses_multiword_dropped" if keep is None else "analyses_not_selected"] += 1
                continue
            if keep is not None:
                display = key
        elif (lid, packed) in demote and key != demote[(lid, packed)]:
            fl |= R.FLAG_DIALECT
            c["override_demoted_analyses"] += 1
        if marks or alt_only:
            mk = marks.get((lid, packed, key))
            if mk == "drop":
                c["override_dropped_analyses"] += 1
                continue
            if mk == "alt":
                fl |= R.FLAG_ALT
                c["override_alt_analyses"] += 1
            if lid in alt_only and (key, packed) in alt_only[lid]:
                fl |= R.FLAG_ALT
                c["override_alt_table_analyses"] += 1
        if fl & (R.FLAG_ALT | R.FLAG_DIALECT):
            l = lemmas[lid]
            if display == l.head or display == recs[lid][0] or vptext.nfc(display) == vptext.nfc(l.head):
                fl &= ANAL_FIX_MASK
                c["anal_headword_fixed"] += 1
        if not morph and lid in macron:  # after the headword fix, which compares with the dictionary's spelling
            nd = macron_apply(display, *macron[lid])
            if nd != display:
                display = nd
                c["macron_override_displays"] += 1
        analyses.append((key, lid, packed, fl, display))
    if not morph:
        if lang == "la":
            extra_anal += whitaker_rows(out, lemmas, recs, analyses, c)
        analyses = merge_analyses(analyses, extra_anal, c)
    c["analyses"] = len(analyses)
    for i in range(1, len(analyses)):
        if _bkey(analyses[i - 1][0]) > _bkey(analyses[i][0]):
            raise ValueError("analyses.tsv is not sorted bytewise at row %d" % i)
    c["lemmas"] = len(lemmas)
    c["candidates"] = len(candidates)
    c["senses"] = sum(len(l.senses) for l in lemmas)
    return Lexicon(lang, notice_text(lang, raw), lemmas, analyses, candidates, morphology_only=morph,
                   analyses_sorted=True)


def run(lang, out, raw, vpl_dir=None, curated=None):
    """Pack one language; the file goes to vpl_dir (default: out)."""
    t0 = time.time()
    c = collections.Counter()
    lx = build_lang(lang, out, raw, c, curated)
    t1 = time.time()
    layout = []
    data = encode(lx, layout)
    del lx
    path = os.path.join(vpl_dir or out, FILE_NAMES[lang])
    tmp = path + ".tmp"
    with open(tmp, "wb") as f:
        f.write(data)
    os.replace(tmp, path)
    sha = hashlib.sha256(data).hexdigest()
    c["size"] = len(data)
    res = {"counts": dict(c), "file": FILE_NAMES[lang], "path": path, "size": len(data), "sha256": sha,
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
    ap.add_argument("--vpl-dir", default=None, help="where the .vpl goes (default: --out)")
    ap.add_argument("lang")
    a = ap.parse_args()
    print(json.dumps(run(a.lang, a.out, a.raw, a.vpl_dir), indent=1))
