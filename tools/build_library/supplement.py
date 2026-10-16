"""Curated whole lemmas missing from Wiktionary (B4c): data/curated/lexicon_supplement_grc.tsv.

Columns (tab-separated, `#` lines are comments):
  key      greek_key of the headword (final sigma written σ; must equal greek_key(head))
  head     headword as displayed (final ς)
  pos      features.POS name (noun, adj, verb, ...)
  gender   space-separated gender words of the lemma (masculine, feminine, neuter); empty for adjectives
  cls      declension number, optionally followed by "contracted" ("2", "3", "2 contracted")
  tier     1, 2 or 3 (LEMM.tier; tier_source = derived)
  gloss_en one-line English gloss; its comma/semicolon segments give the REVX keywords
  gloss_es one-line Spanish gloss (REVX keywords with the es: prefix)
  cells    `;`-separated `tags=form` (Kaikki-style tags of the features vocabulary, e.g.
           `genitive singular=διδασκαλείου`); a repeated tag set adds an alternative form (the first one is the GENX
           cell, every one gets an ANAL row)
  note     free text (source, remarks); not packed
The cells are read like an Attic Kaikki table with the marker "Attic [contracted ]declension-<cls>" (resolve's rules:
extra bit attic, contracted when the marker says so), so the packer treats a supplement lemma exactly like a Kaikki
lemma with a table: LEMM (has_table, one sense), GENX (one form per feature word), ANAL (key, lemma, packed,
FLAG_TABLE, display) and REVX candidates scored as curated glosses (DESIGN 5.2: exact translation 1.0 + head word 0.6,
other content words 0.35, tier 1 +0.2, -0.3 when the parentheses of gloss_en name a register: "(post-classical)",
"(poetic)", "(rare)" ...; the same words give the sense its SENS tags). New lemma ids are appended after the Kaikki ones; a row whose key is already a
Kaikki lemma is skipped (counted, logged): the supplement only fills gaps.
"""
import os

import features as F
import grcfix
import lexdata
import tagmap
import vptext
from common import log

import re

FILE = "lexicon_supplement_grc.tsv"
N_COLS = 10
QUAL_RE = re.compile(r"\(([^()]*)\)")
# register words of a gloss's parentheses beyond gloss.TAG_WORDS: "(post-classical)" marks a later sense
EXTRA_QUAL = {"post-classical": ("medieval",), "postclassical": ("medieval",), "late": ("medieval",),
              "later": ("medieval",), "koine": ("medieval",)}
DECL_ORDINAL = {1: "first", 2: "second", 3: "third"}
GENDER_LETTER = {"masculine": "m", "feminine": "f", "neuter": "n"}
EXACT, HEAD, OTHER, TIER1, PENALTY = 100, 60, 35, 20, -30


class SupplementError(ValueError):
    pass


class Entry(object):
    __slots__ = ("line", "key", "head", "pos", "gender_words", "cls", "contracted", "tier", "gloss_en", "gloss_es",
                 "cells", "note")


def parse_cells(text, pos, line):
    """`tags=form;...` -> [(tags list, form)] in order; every tag must be known to tagmap."""
    out = []
    for part in text.split(";"):
        part = part.strip()
        if not part:
            continue
        tags, sep, form = part.partition("=")
        tags, form = tags.split(), vptext.nfc(form.strip())
        if not sep or not tags or not form or " " in form:
            raise SupplementError("line %d: bad cell %r" % (line, part))
        st = tagmap.TagStats()
        tagmap.tags_to_feature_list(tags, pos, stats=st)
        bad = sorted(set(st.unknown) | set(st.labels) | set(st.lossy))
        if bad:
            raise SupplementError("line %d: unknown tag(s) %s in %r" % (line, " ".join(bad), part))
        out.append((tags, grcfix.final_sigma(form)))
    if not out:
        raise SupplementError("line %d: no cells" % line)
    return out


def parse_row(row, line):
    row = (row + [""] * N_COLS)[:N_COLS]
    e = Entry()
    e.line = line
    e.head = grcfix.final_sigma(vptext.nfc(row[1]))
    e.key = vptext.greek_key(row[0])
    if not e.head or e.key != vptext.greek_key(e.head):
        raise SupplementError("line %d: key %r is not greek_key(head %r)" % (line, row[0], row[1]))
    e.pos = row[2]
    if e.pos not in F.POS:
        raise SupplementError("line %d: unknown pos %r" % (line, row[2]))
    e.gender_words = row[3].split()
    for g in e.gender_words:
        if g not in tagmap.GENDER_BITS:
            raise SupplementError("line %d: unknown gender %r" % (line, g))
    cl = row[4].split()
    try:
        e.cls = int(cl[0]) if cl else 0
    except ValueError:
        raise SupplementError("line %d: bad cls %r" % (line, row[4]))
    e.contracted = "contracted" in cl[1:]
    try:
        e.tier = int(row[5] or 0)
    except ValueError:
        raise SupplementError("line %d: bad tier %r" % (line, row[5]))
    if e.tier not in (0, 1, 2, 3):
        raise SupplementError("line %d: bad tier %r" % (line, row[5]))
    e.gloss_en, e.gloss_es = row[6], row[7]
    if not e.gloss_en:
        raise SupplementError("line %d: no gloss_en" % line)
    e.cells = parse_cells(row[8], e.pos, line)
    e.note = row[9]
    return e


def load(cdir, strict=False, c=None):
    """Entries of data/curated/lexicon_supplement_grc.tsv (in file order). strict: raise on a bad row (tests), else
    log, count `supplement_rows_invalid` and skip it."""
    path = os.path.join(cdir, FILE)
    out = []
    if not os.path.exists(path):
        return out
    n = 0
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            n += 1
            line = line.rstrip("\n").rstrip("\r")
            if not line.strip() or line.lstrip().startswith("#"):
                continue
            row = [x.strip() for x in line.split("\t")]
            try:
                out.append(parse_row(row, n))
            except SupplementError as ex:
                if strict:
                    raise
                log("[grc pack] supplement: %s" % ex)
                if c is not None:
                    c["supplement_rows_invalid"] += 1
    return out


def marker(e):
    return "Attic %sdeclension-%d" % ("contracted " if e.contracted else "", e.cls) if e.cls else "Attic"


def table_rows(e, lid):
    """The entry's cells as table_forms.tsv rows (lemma_id, display, key, tags, source, marker)."""
    m = marker(e)
    return [[str(lid), form, vptext.greek_key(form), " ".join(sorted(tags)), "declension", m] for tags, form in e.cells]


def principal(e, cells):
    """A head line in the style of the Kaikki expansions: "λάθος n (genitive λάθους); third declension"."""
    g = " or ".join(GENDER_LETTER[x] for x in e.gender_words)
    decl = ("; %s declension" % DECL_ORDINAL[e.cls]) if e.cls in DECL_ORDINAL else ""
    if e.contracted:
        decl += ", contracted"
    by_tags = {}
    for tags, form in e.cells:
        by_tags.setdefault(" ".join(sorted(tags)), form)
    if e.pos == "noun":
        gen = by_tags.get("genitive singular")
        return "%s%s%s%s" % (e.head, " " + g if g else "", " (genitive %s)" % gen if gen else "", decl)
    if e.pos == "adj":
        neu = by_tags.get("neuter nominative singular")
        return "%s%s%s" % (e.head, " (neuter %s)" % neu if neu else "", decl)
    return e.head + decl


def keywords(text, es):
    """(heads, others) keyword keys of a gloss, as the gloss stage extracts them (stop words removed, identity
    lemmatiser: the supplement writes base forms)."""
    import gloss
    stop = gloss.load_stopwords("es" if es else "en")
    key = vptext.es_key if es else vptext.en_key
    heads, others, _caps = gloss.extract_keywords(text, lambda w: key(w), stop)
    return heads, others


def sense_tags(e):
    """SENS tag bits from the register words in the parentheses of gloss_en ("mistake (post-classical)" -> Medieval,
    i.e. a later sense; "(poetic)", "(rare)" ... as gloss.TAG_WORDS)."""
    import gloss
    bits = 0
    for q in QUAL_RE.findall(e.gloss_en):
        for w in re.split(r"[,;]", q):
            w = w.strip().lower()
            for name in gloss.TAG_WORDS.get(w, ()) + EXTRA_QUAL.get(w, ()):
                bits |= gloss.tag_bit(name)
    return bits


def candidates(e, lid, pos_code):
    """REVX rows (keyword, lemma, sense, score, pos) for one entry (DESIGN 5.2; -0.3 for a sense tagged rare /
    archaic / poetic / later)."""
    import gloss
    bonus = TIER1 if e.tier == 1 else 0
    if sense_tags(e) & gloss.PENALTY_BITS:
        bonus += PENALTY
    out = []
    for text, prefix in ((e.gloss_en, ""), (e.gloss_es, "es:")):
        if not text:
            continue
        heads, others = keywords(text, bool(prefix))
        for k in heads:
            out.append((prefix + k, lid, 0, max(0, min(255, EXACT + HEAD + bonus)), pos_code))
        for k in others:
            out.append((prefix + k, lid, 0, max(0, min(255, OTHER + bonus)), pos_code))
    return out


def sense_keywords(e):
    heads, others = keywords(e.gloss_en, False)
    return " ".join(heads + others)


def add_to(lemmas, recs, analyses_out, candidates_out, cdir, c, make_lemma, cell_rows, choose_cells, flag_table):
    """Append the supplement lemmas (pack.build_lang, Greek). make_lemma(**fields) -> pack.Lemma; cell_rows /
    choose_cells are pack's; analyses_out / candidates_out receive the new ANAL / REVX rows. Returns the new ids."""
    known = set(l.key for l in lemmas)
    new_ids = []
    for e in load(cdir, c=c):
        if e.key in known:
            c["supplement_skipped_existing"] += 1
            log("[grc pack] supplement row %d: %s is already a lemma, skipped" % (e.line, e.head))
            continue
        lid = len(lemmas)
        rows = table_rows(e, lid)
        cr = cell_rows("grc", rows, e.pos, 0)
        gbits = 0
        for g in e.gender_words:
            gbits |= tagmap.GENDER_BITS[g]
        pos_code = lexdata.pos_code(e.pos)
        l = make_lemma(head=e.head, key=e.key, pos=pos_code, cls=e.cls if e.cls <= 5 else 0,
                       gender=tagmap.GENDER_FROM_BITS[gbits], tier=e.tier, tier_source=1 if e.tier else 0,
                       gloss_en=e.gloss_en, gloss_es=e.gloss_es,
                       senses=[(e.gloss_en, e.gloss_es, sense_keywords(e), sense_tags(e), 1)], flags=flag_table,
                       cells=choose_cells(cr))
        l.principal = principal(e, l.cells)
        l.principal_count = 1
        lemmas.append(l)
        recs.append((e.head, "lemma", 0, e.pos, True))
        known.add(e.key)
        for packed, display, fl, _rank in cr:
            analyses_out.append((vptext.greek_key(display), lid, packed, fl, display))
        candidates_out.extend(candidates(e, lid, pos_code))
        new_ids.append(lid)
        c["supplement_lemmas"] += 1
        c["supplement_cells"] += len(l.cells)
        c["supplement_analyses"] += len(cr)
    return new_ids
