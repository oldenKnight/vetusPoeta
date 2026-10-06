"""Stage `kaikki`: stream the raw Wiktionary extracts and write per-language intermediate files.

Outputs in <out>/<lang>/ (all UTF-8, LF, one record per line, deterministic order = input order):
  lemmas.jsonl      one compact object per lemma record (see README.md for the fields)
  table_forms.tsv   lemma_id, display, key, tags (sorted, space separated), source, marker
  formpages.tsv     word, key, display, target_key, tags, pos, kind (form|alt), target (as written)
  en/translations_{la,grc,es}.tsv   English word, pos, sense text, target word, tags
  es/latin_glosses.tsv              word, pos, gloss, lang (la|grc), sense_index, form_of target
  es/translations_{la,grc}.tsv      Spanish word, pos, sense_index, target word, tags
Entry classification (PREPLAN 1.1): a sense is a form sense when it has form_of / alt_of, the tag form-of /
alt-of, or its gloss reads like "second-person plural present passive indicative of X". An entry is
  lemma      at least one real sense (its form senses still go to formpages.tsv)
  formtable  only form senses but an inflection table of its own (participles like amatus, Attic nous)
  alttable   only alt-of senses and a table of its own (conpello)
  form / alt only form / alt-of senses, no table: a form page (formpages.tsv only)
lemma, formtable and alttable entries become lemma records with sequential ids.
Greek clean-up (B4b, grcfix.py): word-final sigma written as U+03C3 becomes U+03C2 in every display (head, cells,
form pages); "article + word" cells lose the article; romanised cells (and head-line forms) are dropped; a nominal table
without a dialect marker whose forms are Epic/Ionic (-οιο, -ῃσι) gets that dialect prepended to its marker.
"""
import collections
import json
import os
import re
import time

import grcfix
import tagmap
import vptext
from common import TsvWriter, iter_jsonl, log, peak_rss_mb, RAW_FILES

TABLE_SOURCES = frozenset(("declension", "conjugation", "inflection"))
MARKER_TAGS = frozenset(("table-tags", "inflection-template", "class"))
SKIP_HEAD_TAGS = frozenset(("canonical", "romanization", "table-tags", "inflection-template", "class"))
# head-line forms with these tags are other lemmas (amanter is the adverb of amans), not forms of this one
OTHER_LEMMA_TAGS = frozenset(("adverb", "adjective", "noun", "verb", "diminutive", "abbreviation", "agent",
                              "pronoun", "symbol"))
EMPTY_FORMS = frozenset(("", "-", "\u2014", "\u2013", "\u2012", "?"))
FEATURE_TAG_SET = (set(tagmap.CASE_TAGS) | set(tagmap.NUMBER_TAGS) | set(tagmap.GENDER_BITS) |
                   set(tagmap.PERSON_TAGS) | set(tagmap.TENSE_TAGS) | set(tagmap.MOOD_TAGS) |
                   set(tagmap.VOICE_TAGS) | set(tagmap.DEGREE_TAGS) | {"past", "supine", "gerundive", "mediopassive"})
# words allowed before " of " in a form-of gloss
FORM_GLOSS_WORDS = FEATURE_TAG_SET | {
    "inflection", "form", "forms", "the", "and", "or", "degree", "simple", "contracted", "uncontracted",
    "syncopated", "sigmatic", "archaic", "poetic", "epic", "ionic", "attic", "doric", "aeolic", "koine", "homeric",
    "alternative", "obsolete", "rare", "nonstandard", "old", "late", "medieval", "unaugmented", "augmented",
    "apocopated", "elided", "enclitic", "person", "first", "second", "third", "masculine/feminine", "m", "f", "n",
    "nominative/accusative", "genitive/dative", "dative/ablative", "plural/singular", "(", ")", "mediopassive",
    "imperfective", "perfective", "spelling", "variant", "of"}
ALT_GLOSS = re.compile(r"^(?:(?:alternative|obsolete|archaic|rare|nonstandard|dated|old|late|medieval|"
                       r"uncommon|misspelling|eye dialect)\s+)*(?:form|spelling|letter-case form|"
                       r"orthography|misspelling|variant)\s+of\s+(.+)$", re.I)
TOKEN_SPLIT = re.compile(r"[\s/,;:]+")
CLASS_TAG = re.compile(r"^(?:declension|conjugation)-\d$")
CLASS_WORDS = frozenset(("one-termination", "two-termination", "three-termination", "deponent", "semi-deponent",
                         "irregular", "indeclinable", "i-stem", "Greek-type", "impersonal", "no-supine",
                         "no-perfect", "suppletive", "defective", "plural-only", "contracted"))
GREEK_CLASS_ROWS = {"First declension": ["declension-1"], "Second declension": ["declension-2"],
                    "Third declension": ["declension-3"], "First and second declension": ["declension-1",
                                                                                            "declension-2"],
                    "First and third declension": ["declension-1", "declension-3"]}
GENDERS = ("masculine", "feminine", "neuter")
NOMINAL_FPOS = frozenset(("noun", "name", "adj", "pron", "det", "num", "participle", "article"))
ERA_TAGS = tagmap.LATIN_ERAS
ES_GLOSS_RULES = [  # (regex on the lower-cased Spanish gloss, tags)
    (re.compile(r"\bprimera persona\b"), ["first-person"]), (re.compile(r"\bsegunda persona\b"), ["second-person"]),
    (re.compile(r"\btercera persona\b"), ["third-person"]),
    (re.compile(r"\bdel singular\b|\bsingular\b"), ["singular"]), (re.compile(r"\bdel plural\b|\bplural\b"), ["plural"]),
    (re.compile(r"pret\u00e9rito imperfecto"), ["imperfect"]),
    (re.compile(r"pret\u00e9rito perfecto|pret\u00e9rito indefinido"), ["perfect"]),
    (re.compile(r"pluscuamperfecto"), ["pluperfect"]), (re.compile(r"\bpresente\b"), ["present"]),
    (re.compile(r"\bfuturo\b"), ["future"]), (re.compile(r"\bcondicional\b"), ["conditional"]),
    (re.compile(r"\bindicativo\b"), ["indicative"]), (re.compile(r"\bsubjuntivo\b"), ["subjunctive"]),
    (re.compile(r"\bimperativo\b"), ["imperative"]), (re.compile(r"\bparticipio\b"), ["participle"]),
    (re.compile(r"\bgerundio\b"), ["gerund"]), (re.compile(r"\binfinitivo\b"), ["infinitive"]),
    (re.compile(r"\bfemenino\b"), ["feminine"]), (re.compile(r"\bmasculino\b"), ["masculine"]),
]


def es_gloss_tags(gloss):
    g = (gloss or "").lower()
    out = []
    for rx, tags in ES_GLOSS_RULES:
        if rx.search(g):
            out.extend(tags)
    if "pluperfect" in out and "perfect" in out:
        out.remove("perfect")
    if "imperfect" in out and "perfect" in out and "pret\u00e9rito perfecto" not in g:
        out.remove("perfect")
    return out


def first_head_template(e):
    for h in e.get("head_templates") or []:
        if h.get("name") not in ("tlb", "term-label", "head-lite"):
            return h
    hts = e.get("head_templates") or []
    return hts[0] if hts else {}


def target_words(raw):
    """Split 'amatus and amata' style targets; strip parentheses and punctuation."""
    out = []
    for part in re.split(r"\s+and\s+|\s*,\s*", raw or ""):
        part = re.sub(r"\(.*?\)", "", part).strip().strip(":;.,\u201c\u201d\"'")
        if part:
            out.append(part)
    return out


def gloss_form_target(gloss):
    """(kind, tags, target) when a gloss reads like a form-of / alt-of gloss without the structured field."""
    m = ALT_GLOSS.match(gloss)
    if m:
        t = target_words(m.group(1).split(" (")[0])
        return ("alt", [], t[0]) if t else None
    idx = gloss.find(" of ")
    if idx <= 0:
        return None
    prefix = gloss[:idx].lower()
    toks = [t for t in TOKEN_SPLIT.split(prefix) if t]
    if not toks or not all(t in FORM_GLOSS_WORDS for t in toks):
        return None
    tags = [t for t in toks if t in FEATURE_TAG_SET]
    if not tags and "inflection" not in toks:
        return None
    rest = gloss[idx + 4:].strip()
    rest = rest.split(" (")[0].split(":")[0].strip()
    t = target_words(rest)
    if not t:
        return None
    return ("form", tags, t[0])


def classify_sense(s, lang, form_head=False):
    """None for a real sense, else (kind, [targets], extra_tags). The gloss pattern is only trusted when the
    sense is tagged form-of / alt-of or the head template is a form head ("verb form", "grc-noun form")."""
    fo = s.get("form_of")
    if fo:
        tg = []
        for x in fo:
            tg.extend(target_words(x.get("word", "")))
        if tg:
            return ("form", tg, [])
    ao = s.get("alt_of")
    if ao:
        tg = []
        for x in ao:
            tg.extend(target_words(x.get("word", "")))
        if tg:
            return ("alt", tg, [])
    tags = s.get("tags") or []
    if not (form_head or "form-of" in tags or "alt-of" in tags):
        return None
    glosses = s.get("glosses") or []
    for g in glosses:
        r = gloss_form_target(g)
        if r:
            kind, gtags, target = r
            return (kind, [target], gtags)
    return None  # no recoverable target: keep it as a real sense


def real_cells(forms):
    """Yield (row, marker, source) for table cells; marker is the text of the last table-tags row."""
    marker = {}
    for r in forms:
        src = r.get("source")
        if not src or src not in TABLE_SOURCES:
            continue
        tags = r.get("tags") or []
        if "table-tags" in tags:
            m = r.get("form", "")
            marker[src] = "" if m == "no-table-tags" else m
            continue
        if "inflection-template" in tags or "class" in tags:
            continue
        form = (r.get("form") or "").strip()
        if form in EMPTY_FORMS or form.startswith("/") or form.startswith("*"):
            continue
        yield r, marker.get(src, ""), src


class KaikkiStage(object):
    def __init__(self, lang, raw_dir, out_dir):
        self.lang = lang
        self.raw_dir = raw_dir
        self.dir = os.path.join(out_dir, lang)
        os.makedirs(self.dir, exist_ok=True)
        self.key = vptext.key_for(lang)
        self.c = collections.Counter()
        self.pos = collections.Counter()
        self.ht = collections.Counter()
        self.next_id = 0
        self.lemmas = open(os.path.join(self.dir, "lemmas.jsonl"), "w", encoding="utf-8", newline="\n")
        self.table = TsvWriter(os.path.join(self.dir, "table_forms.tsv"))
        self.formpages = TsvWriter(os.path.join(self.dir, "formpages.tsv"))
        self.extra_writers = {}
        self.article_lemmas = set()

    def writer(self, name):
        w = self.extra_writers.get(name)
        if w is None:
            w = self.extra_writers[name] = TsvWriter(os.path.join(self.dir, name))
        return w

    # -- display helpers -------------------------------------------------------------------------
    def head_display(self, e, word, key):
        cands = []
        for r in e.get("forms") or []:
            if "canonical" in (r.get("tags") or []) and not r.get("source"):
                cands.append(r.get("form", ""))
        h = first_head_template(e)
        args = h.get("args") or {}
        if args.get("head"):
            cands.append(args["head"])
        for k in ("1", "2"):
            v = args.get(k)
            if v and "<" in v:
                v = v.split("<")[0]
            if v:
                cands.append(v)
        exp = h.get("expansion") or ""
        if exp:
            nw = len(word.split())
            cands.append(" ".join(exp.split()[:nw]))
        for c in cands:
            c = vptext.nfc(c.strip())
            if c and self.key(c) == key:
                return self.fix_display(c)
        return self.fix_display(vptext.nfc(word))

    def fix_display(self, s):
        if self.lang != "grc":
            return s
        f = grcfix.final_sigma(s)
        if f != s:
            self.c["grc_final_sigma_fixed"] += 1
        return f

    # -- per entry -----------------------------------------------------------------------------
    def entry(self, e):
        lang = self.lang
        c = self.c
        word = e.get("word") or ""
        if not word:
            c["no_word"] += 1
            return
        word = vptext.nfc(word)
        key = self.key(word)
        kpos = e.get("pos") or ""
        h = first_head_template(e)
        ht = h.get("name", "") if h else (e.get("pos_title") or "")
        harg2 = (h.get("args") or {}).get("2", "") if h else ""
        fpos = tagmap.pos_name(kpos, ht, harg2 if ht == "head" else "")
        self.pos[kpos] += 1
        self.ht[ht or "-"] += 1
        c["entries"] += 1
        forms = e.get("forms") or []
        cells = list(real_cells(forms))
        has_table = bool(cells)
        senses = e.get("senses") or []
        real, formsenses = [], []
        form_head = ht.endswith(" form") or (ht == "head" and (harg2.endswith(" form") or harg2.endswith(" forms")))
        for s in senses:
            r = classify_sense(s, lang, form_head)
            if r is None:
                real.append(s)
            else:
                formsenses.append((r, s))
        if real or not senses:
            kind = "lemma"
        elif has_table:
            kind = "alttable" if all(r[0] == "alt" for r, _ in formsenses) else "formtable"
        else:
            kind = "alt" if all(r[0] == "alt" for r, _ in formsenses) else "form"
        c["kind_" + kind] += 1
        display = self.head_display(e, word, key) if lang in ("la", "grc") else word
        lemma_id = None
        if kind in ("lemma", "formtable", "alttable"):
            lemma_id = self.next_id
            self.next_id += 1
            self.write_lemma(e, lemma_id, word, key, display, kpos, fpos, ht, kind, real, formsenses, cells)
            self.write_table(e, lemma_id, cells, forms, has_table, fpos)
        for (fkind, targets, gtags), s in formsenses:
            tags = set(s.get("tags") or [])
            tags.update(gtags)
            if lang == "es":
                tags.update(es_gloss_tags((s.get("glosses") or [""])[-1]))
            tags.discard("form-of")
            tags.discard("alt-of")
            tagstr = " ".join(sorted(tags))
            for t in targets:
                t = vptext.nfc(t)
                if lang == "grc":
                    t = grcfix.final_sigma(t)
                self.formpages.write(word, key, display, self.key(t), tagstr, fpos, fkind, t)
                c["formpage_rows"] += 1
        if lang == "en":
            self.translations(e, word, kpos)
        if lang == "es":
            self.es_translations(e, word, kpos)

    def write_lemma(self, e, lemma_id, word, key, display, kpos, fpos, ht, kind, real, formsenses, cells):
        lang = self.lang
        rec = {"id": lemma_id, "word": word, "key": key, "head": display, "pos": kpos, "fpos": fpos, "ht": ht,
               "kind": kind, "has_table": bool(cells)}
        genders = set()
        classes = set()
        for r in e.get("forms") or []:
            if "canonical" in (r.get("tags") or []):
                genders.update(t for t in r.get("tags") if t in GENDERS)
            if "class" in (r.get("tags") or []):
                classes.update(GREEK_CLASS_ROWS.get(r.get("form", ""), []))
            if "table-tags" in (r.get("tags") or []):
                for t in (r.get("form") or "").split():
                    if CLASS_TAG.match(t) or t in CLASS_WORDS:
                        classes.add(t)
        sense_src = real if real else [s for _, s in formsenses]
        for s in sense_src:
            for t in s.get("tags") or []:
                if t in GENDERS:
                    genders.add(t)
                elif CLASS_TAG.match(t) or t in CLASS_WORDS:
                    classes.add(t)
        for t in e.get("tags") or []:  # es-extract puts gender on the entry
            if t in GENDERS:
                genders.add(t)
        if genders:
            rec["gender"] = sorted(genders)
        if classes:
            rec["class"] = sorted(classes)
        h = first_head_template(e)
        if h and h.get("expansion"):
            rec["pp"] = h["expansion"]
        if lang in ("la", "grc"):
            out = []
            for s in real:
                gl = s.get("glosses") or []
                d = {"g": gl[-1] if gl else ""}
                for k_in, k_out in (("tags", "tags"), ("raw_tags", "raw_tags"), ("topics", "topics")):
                    if s.get(k_in):
                        d[k_out] = s[k_in]
                if s.get("qualifier"):
                    d["q"] = s["qualifier"]
                if s.get("examples"):
                    d["ex"] = len(s["examples"])
                out.append(d)
            rec["senses"] = out
            if lang == "la" and real and all(any(t in ERA_TAGS for t in (s.get("tags") or [])) for s in real):
                rec["late"] = 1
        else:
            rec["ns"] = len(real)
        if kind != "lemma":
            tg = []
            for (fk, targets, _), _s in formsenses:
                for t in targets:
                    if t not in tg:
                        tg.append(vptext.nfc(t))
            rec["of"] = tg
        if lang == "grc":
            el = self.el_descendant(e.get("descendants") or [], vptext.greek_bare(word))
            if el is not None:
                rec["el"] = el
                self.c["el_desc"] += 1
                self.c["el_same"] += el
        self.lemmas.write(_json_compact(rec))
        self.lemmas.write("\n")
        self.c["lemma_records"] += 1
        if cells:
            self.c["lemmas_with_table"] += 1

    def el_descendant(self, items, bare):
        found = None
        stack = list(items)
        while stack:
            d = stack.pop()
            if not isinstance(d, dict):
                continue
            if d.get("lang_code") == "el" and d.get("word"):
                same = 1 if vptext.greek_bare(d["word"]) == bare else 0
                found = max(found or 0, same)
            stack.extend(d.get("descendants") or [])
        return found

    def greek_cells(self, lemma_id, cells, fpos):
        """[(form, tags, src, marker)] after the Greek clean-up (see the module docstring)."""
        nominal = fpos in NOMINAL_FPOS
        guess = {}
        if nominal:
            groups = collections.OrderedDict()
            for r, marker, src in cells:
                groups.setdefault((src, marker), []).append((r.get("form") or "", r.get("tags") or []))
            for (src, marker), rows in groups.items():
                if any(t[:1].isupper() for t in marker.split()):
                    continue
                g = grcfix.dialect_guess(rows)
                if g:
                    guess[(src, marker)] = (g + " " + marker).strip()
                    self.c["grc_dialect_tables_guessed"] += 1
        out = []
        for r, marker, src in cells:
            form = vptext.nfc(r["form"].strip())
            if grcfix.latin_script(form):
                self.c["grc_romanised_cells_dropped"] += 1
                continue
            form, stripped = grcfix.strip_article(form)  # any POS: suffix tables, mis-tagged participles
            if stripped:
                self.c["grc_article_cells_stripped"] += 1
                if lemma_id not in self.article_lemmas:
                    self.article_lemmas.add(lemma_id)
                    self.c["grc_article_lemmas"] += 1
            fixed = grcfix.final_sigma(form)
            if fixed != form:
                self.c["grc_final_sigma_cells"] += 1
                form = fixed
            out.append((form, r.get("tags") or [], src, guess.get((src, marker), marker)))
        return out

    def write_table(self, e, lemma_id, cells, forms, has_table, fpos=""):
        lang = self.lang
        multi_ok = lang in ("la", "grc")
        if lang == "grc":
            rows = self.greek_cells(lemma_id, cells, fpos)
        else:
            rows = [(vptext.nfc(r["form"].strip()), r.get("tags") or [], src, marker) for r, marker, src in cells]
        for form, rtags, src, marker in rows:
            if not multi_ok and " " in form:
                self.c["skipped_multiword"] += 1
                continue
            tags = " ".join(sorted(set(rtags)))
            self.table.write(lemma_id, form, self.key(form), tags, src, marker)
            self.c["table_rows"] += 1
        # head-line forms: all of them for en/es (their "tables" are mostly head-line forms), only alternative
        # spellings for la/grc when a table exists, otherwise the morphological ones
        for r in forms:
            if r.get("source"):
                continue
            tags = set(r.get("tags") or [])
            if tags & SKIP_HEAD_TAGS or tags & OTHER_LEMMA_TAGS:
                continue
            form = (r.get("form") or "").strip()
            if form in EMPTY_FORMS or form.startswith("/") or form.startswith("*"):
                continue
            alt = "alternative" in tags
            if lang in ("la", "grc") and has_table and not alt:
                continue
            if not alt and not (tags & FEATURE_TAG_SET):
                continue
            form = vptext.nfc(form)
            if lang == "grc":
                if grcfix.latin_script(form):
                    self.c["grc_romanised_head_forms_dropped"] += 1
                    continue
                fixed = grcfix.final_sigma(form)
                if fixed != form:
                    self.c["grc_final_sigma_head_forms"] += 1
                    form = fixed
            if not multi_ok and " " in form:
                self.c["skipped_multiword"] += 1
                continue
            self.table.write(lemma_id, form, self.key(form), " ".join(sorted(tags)), "head", "")
            self.c["head_rows"] += 1

    # -- translations --------------------------------------------------------------------------
    def translations(self, e, word, kpos):
        rows = list(e.get("translations") or [])
        for s in e.get("senses") or []:
            st = s.get("translations")
            if st:
                gl = (s.get("glosses") or [""])[-1]
                for t in st:
                    rows.append(dict(t, _gloss=gl))
        for t in rows:
            code = t.get("lang_code") or t.get("code")
            if code not in ("la", "grc", "es"):
                continue
            tw = t.get("word")
            if not tw:
                continue
            sense = t.get("sense") or t.get("_gloss") or ""
            self.writer("translations_%s.tsv" % code).write(
                word, kpos, sense, vptext.nfc(tw), " ".join(sorted(t.get("tags") or [])))
            self.c["translations_" + code] += 1

    def es_translations(self, e, word, kpos):
        for t in e.get("translations") or []:
            code = t.get("lang_code")
            if code in ("la", "grc") and t.get("word"):
                self.writer("translations_%s.tsv" % code).write(
                    word, kpos, t.get("sense_index", ""), vptext.nfc(t["word"]), " ".join(sorted(t.get("tags") or [])))
                self.c["es_translations_" + code] += 1

    def es_foreign(self, e):
        """es.wiktionary Latin / Ancient Greek entries: Spanish glosses."""
        w = self.writer("latin_glosses.tsv")
        lc = e.get("lang_code")
        word = vptext.nfc(e.get("word") or "")
        for s in e.get("senses") or []:
            gl = s.get("glosses") or []
            if not gl:
                continue
            fo = " ".join(x.get("word", "") for x in (s.get("form_of") or []))
            w.write(word, e.get("pos", ""), gl[-1], lc, s.get("sense_index", ""), fo)
            self.c["es_glosses_" + lc] += 1
        self.c["es_entries_" + lc] += 1

    def close(self):
        self.lemmas.close()
        self.table.close()
        self.formpages.close()
        for w in self.extra_writers.values():
            w.close()


def _json_compact(obj):
    return json.dumps(obj, ensure_ascii=False, separators=(",", ":"), sort_keys=False)


def run(lang, raw_dir, out_dir, input_path=None):
    """Run the stage for one language. Returns the counts dict."""
    t0 = time.time()
    path = input_path or os.path.join(raw_dir, RAW_FILES[lang])
    st = KaikkiStage(lang, raw_dir, out_dir)
    progress = st.c
    try:
        for e in iter_jsonl(path, label="%s kaikki" % lang, counters=progress):
            lc = e.get("lang_code")
            if lang == "es":
                if lc in ("la", "grc"):
                    st.es_foreign(e)
                    continue
                if lc != "es":
                    continue
            elif lc and lc != lang:
                st.c["other_lang"] += 1
                continue
            st.entry(e)
        for name in ("translations_la.tsv", "translations_grc.tsv", "translations_es.tsv", "latin_glosses.tsv"):
            if (lang == "en" and name.startswith("translations")) or (lang == "es" and name in (
                    "translations_la.tsv", "translations_grc.tsv", "latin_glosses.tsv")):
                st.writer(name)  # create even when empty
    finally:
        st.close()
    counts = dict(st.c)
    counts["form_pages"] = counts.get("kind_form", 0) + counts.get("kind_alt", 0)
    res = {"counts": counts, "pos": dict(st.pos.most_common()), "head_templates": dict(st.ht.most_common(60)),
           "duration_s": round(time.time() - t0, 1), "peak_rss_mb": peak_rss_mb()}
    log("[%s kaikki] done: %s" % (lang, " ".join("%s=%s" % kv for kv in sorted(counts.items()))))
    return res
