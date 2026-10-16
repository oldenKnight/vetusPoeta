"""Which English / Spanish lemmas go into english.vpl / spanish.vpl (B4b size cut, target <= 25 MB each).

A lemma record is kept when
  (a) its key, or the key of one of its forms (an analysis pointing at it), occurs in the UD treebanks the NLP models
      are trained on (data/raw/ud: en_ewt-*.conllu for English; es_ancora-*, es_gsd-* for Spanish; LEMMA and FORM
      columns, lower-cased through en_key / es_key), or
  (b) it is among the TOP_N lemma records by a frequency proxy: number of Wiktionary senses (`ns`) plus the number
      of translation-table rows that mention the word (English: en/translations_{la,grc,es}.tsv source words;
      Spanish: es/translations_{la,grc}.tsv source words and en/translations_es.tsv targets), ties by lemma id, or
  (c) its key or one of its form keys is a word of a data/curated file the engine reads for these languages
      (phrasebooks, contractions, readable_*, states, phrasal and preposition tables, gloss_es_la.tsv).
Everything here only selects; pack.py writes the file (ANAL display strings dropped: display = key).
"""
import collections
import glob
import os
import re

import vptext
from common import read_tsv

TOP_N = 60000
UD_GLOBS = {"en": ("en_ewt-*.conllu",), "es": ("es_ancora-*.conllu", "es_gsd-*.conllu")}
WORD_RE = re.compile(r"[^\W\d_]+(?:['’-][^\W\d_]+)*")
CURATED_FILES = {
    "en": ("phrasebook_en_la.tsv", "phrasebook_en_grc.tsv", "contractions_en.tsv", "nonverbal_en_la.tsv",
           "phrasal_en_la.tsv", "verbprep_en_la.tsv", "states_en_la.tsv", "preps_en_la.tsv", "preps_en_grc.tsv",
           "readable_en.tsv", "names_la.tsv", "names_grc.tsv", "periphrasis_la.tsv", "tiers_la.tsv",
           "tiers_grc.tsv"),
    "es": ("readable_es.tsv", "gloss_es_la.tsv", "states_en_la.tsv", "phrasebook_en_la.tsv", "verbprep_en_la.tsv",
           "phrasal_en_la.tsv", "contractions_es.tsv", "clitics_es.tsv", "phrasal_es_la.tsv", "states_es_la.tsv",
           "verbprep_es_la.tsv"),
}


def key_fn(lang):
    return vptext.en_key if lang == "en" else vptext.es_key


def ud_words(raw, lang):
    """(lemma keys, form keys) of the UD files for a language."""
    k = key_fn(lang)
    lemmas, forms = set(), set()
    files = []
    for pat in UD_GLOBS.get(lang, ()):
        files += sorted(glob.glob(os.path.join(raw, "ud", pat)))
    for path in files:
        with open(path, "r", encoding="utf-8") as f:
            for line in f:
                if not line or line[0] == "#" or line == "\n":
                    continue
                cols = line.rstrip("\n").split("\t")
                if len(cols) < 3 or "-" in cols[0] or "." in cols[0]:
                    continue
                forms.add(k(cols[1]))
                if cols[2] != "_":
                    lemmas.add(k(cols[2]))
    return lemmas, forms, files


def curated_words(curated, lang):
    k = key_fn(lang)
    out = set()
    files = []
    for name in CURATED_FILES.get(lang, ()):
        p = os.path.join(curated, name)
        if not os.path.exists(p):
            continue
        files.append(p)
        with open(p, "r", encoding="utf-8") as f:
            for line in f:
                if line.lstrip().startswith("#"):
                    continue
                for m in WORD_RE.finditer(line):
                    out.add(k(m.group(0)))
    return out, files


def translation_counts(out, lang):
    """key -> number of translation rows naming the word."""
    k = key_fn(lang)
    cnt = collections.Counter()
    files = []
    if lang == "en":
        for code in ("la", "grc", "es"):
            p = os.path.join(out, "en", "translations_%s.tsv" % code)
            if os.path.exists(p):
                files.append(p)
                for r in read_tsv(p):
                    if r and r[0]:
                        cnt[k(r[0])] += 1
    else:
        for code in ("la", "grc"):
            p = os.path.join(out, "es", "translations_%s.tsv" % code)
            if os.path.exists(p):
                files.append(p)
                for r in read_tsv(p):
                    if r and r[0]:
                        cnt[k(r[0])] += 1
        p = os.path.join(out, "en", "translations_es.tsv")
        if os.path.exists(p):
            files.append(p)
            for r in read_tsv(p):
                if len(r) > 3 and r[3]:
                    cnt[k(r[3])] += 1
    return cnt, files


def input_files(out, raw, curated, lang):
    """Every file the selection reads (for the stage cache and --next seeding). The translation tables are always
    listed (the kaikki stage writes them; a missing one must fail the build, not shrink the proxy silently)."""
    files = []
    for pat in UD_GLOBS.get(lang, ()):
        files += sorted(glob.glob(os.path.join(raw, "ud", pat)))
    files += [os.path.join(curated, n) for n in CURATED_FILES.get(lang, ()) if os.path.exists(os.path.join(curated, n))]
    if lang == "en":
        files += [os.path.join(out, "en", "translations_%s.tsv" % c) for c in ("la", "grc", "es")]
    else:
        files += [os.path.join(out, "es", "translations_%s.tsv" % c) for c in ("la", "grc")]
        files.append(os.path.join(out, "en", "translations_es.tsv"))
    return files


def select(records, form_keys_of, out, raw, curated, lang, c, top_n=None):
    """records: list of (id, key, ns) of the single-word lemma records; form_keys_of: id -> set of keys of its
    analyses. Returns the set of kept ids and fills the counter c."""
    ud_lem, ud_form, ud_files = ud_words(raw, lang)
    cur, _ = curated_words(curated, lang)
    tcount, _ = translation_counts(out, lang)
    c["ud_files"] = len(ud_files)
    c["ud_lemma_keys"] = len(ud_lem)
    c["ud_form_keys"] = len(ud_form)
    c["curated_words"] = len(cur)
    proxy = sorted(records, key=lambda r: (-(r[2] + tcount.get(r[1], 0)), r[0]))
    top = set(r[0] for r in proxy[:TOP_N if top_n is None else top_n])
    keep = set()
    for i, key, _ns in records:
        forms = form_keys_of.get(i, ())
        why = None
        if key in ud_lem or key in ud_form or any(f in ud_form for f in forms):
            why = "ud"
        elif i in top:
            why = "proxy"
        elif key in cur or any(f in cur for f in forms):
            why = "curated"
        if why:
            keep.add(i)
            c["kept_" + why] += 1
    c["kept"] = len(keep)
    c["dropped"] = len(records) - len(keep)
    return keep
