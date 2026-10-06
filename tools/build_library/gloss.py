"""Stage `gloss` (la, grc): one-line glosses, sense records with keywords and tags, and the reverse index.

Outputs in <out>/<lang>/:
  gloss.tsv    lemma_id, gloss_en, gloss_en_src, gloss_es, gloss_es_src, n_senses
               gloss_en_src: kaikki (first sense) | of (gloss of the lemma a form/alt table points to) |
               ls / whitaker / lsj (fallbacks); gloss_es_src: curated | dcc | eswikt | pivot (EN->ES through the
               English entries' Spanish translations; the UI shows "(via English)": LEMM flags bit8)
  senses.tsv   lemma_id, sense_idx, gloss_en, gloss_es, keywords (space separated en_keys), tags (u16), rank
  revx_en.tsv  keyword, lemma_id, sense_idx, score, pos, components   (sorted: keyword, score desc, lemma, sense)
  revx_es.tsv  same for Spanish keywords (stored in the .vpl with the prefix ES_PREFIX, see README)
Reverse-index score (DESIGN 5.2), computed in integer hundredths so rounding is exact:
  +100 exact Wiktionary translation (EN: English entries' translations; ES: es.wiktionary translations and the
       curated data/curated/gloss_es_<lang>.tsv), +60 head word of the lemma gloss (a head word of a segment of
       the first sense: "to love, like" -> love, like), +35 other content word (or head of a later sense)
       (the larger of the two when both apply), +15 Whitaker frequency A/B, +10 DCC core, +20 tier 1,
  -30 sense tagged rare/archaic/poetic/Medieval/New Latin (or the translation row carries such a tag),
  -20 proper-name lemma and a keyword that was written lower-case; score = clamp(0, 255).
Spanish gloss words from the EN->ES pivot count as "other content word" (+35) only.
One candidate per (keyword, lemma): the best-scoring sense, lowest sense index on ties.
"""
import collections
import os
import re
import time

import lexdata
import tagmap
import tiers as T
import vptext
from common import TsvWriter, log, peak_rss_mb, read_tsv

HERE = os.path.dirname(os.path.abspath(__file__))
ES_PREFIX = "es:"
GLOSS_LEN = 60
SENSE_LEN = 200

# SENS.tags bits (DESIGN 5)
TAG_BITS = {"transitive": 0, "intransitive": 1, "figurative": 2, "rare": 3, "archaic": 4, "poetic": 5,
            "medieval": 6, "newlatin": 7, "with-dat": 8, "with-abl": 9, "with-gen": 10, "with-acc": 11,
            "with-inf": 12, "impersonal": 13, "reflexive": 14}
PENALTY_BITS = sum(1 << TAG_BITS[n] for n in ("rare", "archaic", "poetic", "medieval", "newlatin"))
TAG_WORDS = {
    "transitive": ("transitive",), "ditransitive": ("transitive",), "ambitransitive": ("transitive", "intransitive"),
    "intransitive": ("intransitive",), "figuratively": ("figurative",), "figurative": ("figurative",),
    "metaphorically": ("figurative",), "rare": ("rare",), "uncommon": ("rare",), "archaic": ("archaic",),
    "obsolete": ("archaic",), "dated": ("archaic",), "old latin": ("archaic",), "poetic": ("poetic",),
    "medieval latin": ("medieval",), "late latin": ("medieval",), "ecclesiastical": ("medieval",),
    "ecclesiastical latin": ("medieval",), "medieval": ("medieval",), "vulgar latin": ("medieval",),
    "new latin": ("newlatin",), "renaissance latin": ("newlatin",), "modern latin": ("newlatin",),
    "contemporary latin": ("newlatin",), "neo latin": ("newlatin",), "scientific latin": ("newlatin",),
    "with dative": ("with-dat",), "with ablative": ("with-abl",), "with genitive": ("with-gen",),
    "with accusative": ("with-acc",), "with infinitive": ("with-inf",), "impersonal": ("impersonal",),
    "reflexive": ("reflexive",),
    # Greek registers
    "epic": ("poetic",), "homeric": ("poetic",), "byzantine": ("medieval",), "medieval greek": ("medieval",),
}
QUAL_PATTERNS = [(re.compile(r"\+\s*dat"), "with-dat"), (re.compile(r"\+\s*abl"), "with-abl"),
                 (re.compile(r"\+\s*gen"), "with-gen"), (re.compile(r"\+\s*acc"), "with-acc"),
                 (re.compile(r"\+\s*inf"), "with-inf")]
FRAME_BITS = {"acc": ("transitive", "with-acc"), "dat": ("with-dat",), "abl": ("with-abl",), "gen": ("with-gen",),
              "dat+acc": ("transitive", "with-acc", "with-dat"), "acc+inf": ("transitive", "with-acc", "with-inf"),
              "inf": ("with-inf",), "intr": ("intransitive",), "refl": ("reflexive",)}
PEN_ROW_TAGS = frozenset(("poetic", "rare", "archaic", "obsolete", "dated", "Medieval-Latin", "Late-Latin",
                          "New-Latin", "Ecclesiastical", "Renaissance-Latin", "Epic", "uncommon"))
NEGATORS = frozenset(("no", "not", "never", "nunca", "ni", "non"))
WORD_RE = re.compile(r"[^\W\d_]+(?:['’-][^\W\d_]+)*")
PAREN_RE = re.compile(r"\([^()]*\)|\[[^\[\]]*\]")
EN_POS_OF = {"noun": ("noun",), "name": ("name", "noun"), "verb": ("verb",), "adj": ("adj",), "participle": ("adj", "verb"),
             "adv": ("adv",), "prep": ("prep",), "conj": ("conj",), "pron": ("pron",), "num": ("num",),
             "intj": ("intj",), "det": ("det", "pron"), "particle": ("particle", "adv")}
ES_FORM_GLOSS = re.compile(r"^(forma|variante|grafía|abreviatura|plural|femenino|masculino|neutro|participio|"
                           r"primera|segunda|tercera|genitivo|dativo|acusativo|ablativo|vocativo|nominativo)\b", re.I)


def tag_bit(name):
    return 1 << TAG_BITS[name]


def strip_parens(s):
    prev = None
    while prev != s:
        prev = s
        s = PAREN_RE.sub(" ", s)
    s = re.sub(r"\s+", " ", s).strip()
    return re.sub(r" ([,;:.!?])", r"\1", s)


def one_line(g, n=GLOSS_LEN):
    """A gloss for one line: parentheticals removed, cut at ';', then at the last ', ' or space before n chars.
    Verb glosses keep their 'to ' (Wiktionary writes 'to love'); nothing is added or removed at the front."""
    s = strip_parens(g or "") or re.sub(r"\s+", " ", (g or "")).strip()
    s = s.strip(" ,;:.")
    if len(s) <= n:
        return s
    first = s.split(";")[0].strip(" ,;:.")
    if 0 < len(first) <= n:
        return first
    cut = s[:n + 1]
    i = cut.rfind(", ")
    if i >= 20:
        return s[:i].strip(" ,;:.")
    i = cut.rfind(" ")
    if i <= 0:
        i = n - 1
    return s[:i].rstrip(" ,;:.") + "…"


def load_stopwords(lang):
    path = os.path.join(HERE, "stopwords_%s.txt" % lang)
    out = set()
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            w = line.strip()
            if w and not w.startswith("#"):
                out.add(w)
    return frozenset(out)


def gloss_words(text):
    """(word, capitalised) pairs of a gloss after removing parentheticals; hyphenated words are kept whole."""
    return [(m.group(0).lower(), m.group(0)[:1].isupper()) for m in WORD_RE.finditer(strip_parens(text or ""))]


def segment_words(text):
    """Segments (split at , ; :) -> list of lists of (word, capitalised)."""
    t = strip_parens(text or "")
    return [gloss_words(seg) for seg in re.split(r"[,;:/]", t)]


def extract_keywords(text, lemmatise, stop):
    """(heads, others, caps): head = first content word of each segment (none for a negated segment: "no querer"
    is not a gloss of querer); others = remaining content words and the parts of hyphenated words; when the whole gloss has no content word, the first word that is not an article
    or 'to' is the head ("to be" -> be). caps = set of keywords seen capitalised."""
    heads, others, caps = [], [], set()
    allwords = []
    for seg in segment_words(text):
        negated = bool(seg) and seg[0][0] in NEGATORS
        content = []
        for w, cap in seg:
            allwords.append((w, cap))
            if w in stop or len(w) < 2 and w not in ("i",):
                continue
            content.append((w, cap))
        for j, (w, cap) in enumerate(content):
            k = lemmatise(w)
            if cap:
                caps.add(k)
            if j == 0 and not negated:
                if k not in heads:
                    heads.append(k)
            elif k not in heads and k not in others:
                others.append(k)
            if "-" in w:
                for part in w.split("-"):
                    if len(part) >= 2 and part not in stop:
                        pk = lemmatise(part)
                        if pk not in heads and pk not in others:
                            others.append(pk)
    if not heads and not others:
        for w, cap in allwords:
            if w in ("a", "an", "the", "to", "un", "una", "el", "la", "los", "las"):
                continue
            k = lemmatise(w)
            heads.append(k)
            if cap:
                caps.add(k)
            break
    others = [o for o in others if o not in heads]
    return heads, others, caps


# -------------------------------------------------------------------------------------------------------------
class Lemmatiser(object):
    """Form -> lemma key for English (or Spanish) gloss words, from data/work/<lang>/analyses.tsv: a word that is
    itself a lemma key stays; a form maps to the lemma key that is most frequent among the gloss words (then
    shortest, then alphabetical); unknown words go through suffix rules and are kept only when the result is a
    lemma key, else unchanged."""

    def __init__(self, lang, out, words, freq):
        self.lang = lang
        self.map = {}
        d = os.path.join(out, lang)
        words = set(words)
        cand = set(words)
        variants = {}
        for w in words:
            v = self.suffix_variants(w)
            variants[w] = v
            cand.update(v)
        lemma_keys = set()
        idx = os.path.join(d, "lemma_index.tsv")
        anal = os.path.join(d, "analyses.tsv")
        self.available = os.path.exists(idx) and os.path.exists(anal)
        if not self.available:
            self.map = {w: w for w in words}
            return
        for row in read_tsv(idx):
            if len(row) > 1 and row[1] in cand:
                lemma_keys.add(row[1])
        need = {}
        for row in read_tsv(anal):
            k = row[0]
            if k in words and k not in lemma_keys:
                need.setdefault(k, set()).add(int(row[1]))
        ids = set()
        for v in need.values():
            ids.update(v)
        id_key = {}
        for row in read_tsv(idx):
            i = int(row[0])
            if i in ids:
                id_key[i] = row[1]
        for w in words:
            if w in lemma_keys:
                self.map[w] = w
            elif w in need:
                ks = sorted(set(id_key[i] for i in need[w] if i in id_key and id_key[i]))
                ks = [k for k in ks if " " not in k] or ks
                if ks:
                    self.map[w] = min(ks, key=lambda k: (-freq.get(k, 0), len(k), k))
                else:
                    self.map[w] = w
            else:
                hit = [v for v in variants[w] if v in lemma_keys]
                self.map[w] = hit[0] if hit else w

    def suffix_variants(self, w):
        out = []
        if self.lang == "en":
            if w.endswith("ies") and len(w) > 4:
                out.append(w[:-3] + "y")
            if w.endswith("ves") and len(w) > 4:
                out += [w[:-3] + "f", w[:-3] + "fe"]
            if w.endswith("es") and len(w) > 3:
                out.append(w[:-2])
            if w.endswith("s") and not w.endswith("ss") and len(w) > 3:
                out.append(w[:-1])
            if w.endswith("ied") and len(w) > 4:
                out.append(w[:-3] + "y")
            if w.endswith("ed") and len(w) > 4:
                out += [w[:-2], w[:-1]]
                if len(w) > 5 and w[-3] == w[-4]:
                    out.append(w[:-3])
            if w.endswith("ing") and len(w) > 5:
                out += [w[:-3], w[:-3] + "e"]
                if len(w) > 6 and w[-4] == w[-5]:
                    out.append(w[:-4])
        else:
            if w.endswith("es") and len(w) > 4:
                out.append(w[:-2])
            if w.endswith("s") and len(w) > 3:
                out.append(w[:-1])
        return out

    def __call__(self, w):
        return self.map.get(w, w)


# -------------------------------------------------------------------------------------------------------------
def sense_tags(s):
    """SENS tag bits from Kaikki tags, raw_tags and the qualifier."""
    bits = 0
    words = []
    for t in (s.get("tags") or []) + (s.get("raw_tags") or []):
        words.append(t.lower().replace("-", " "))
    q = (s.get("q") or "").lower()
    if q:
        words.extend(x.strip() for x in re.split(r"[,;]", q))
    for w in words:
        for name in TAG_WORDS.get(w, ()):
            bits |= tag_bit(name)
        if w.startswith("with ") or w.startswith("+"):
            for rx, name in QUAL_PATTERNS:
                if rx.search(w.replace("with ", "+")):
                    bits |= tag_bit(name)
    for rx, name in QUAL_PATTERNS:
        if rx.search(q):
            bits |= tag_bit(name)
    return bits


def frame_bits(frames):
    bits = 0
    for fr in frames.split(";"):
        fr = fr.strip()
        if fr.startswith("impers:"):
            bits |= tag_bit("impersonal")
            fr = fr[len("impers:"):]
            for part in fr.split("+"):
                bits |= sum(tag_bit(n) for n in FRAME_BITS.get(part, ()) if n != "transitive")
            continue
        for n in FRAME_BITS.get(fr, ()):
            bits |= tag_bit(n)
    return bits


def es_clean(g, lemma_is_name=False):
    s = strip_parens(g or "").replace("→", " ").strip(" .;:,")
    s = re.sub(r"\s+", " ", s)
    if not lemma_is_name and len(s) > 1 and s[0].isupper() and s[1:2].islower():
        s = s[0].lower() + s[1:]
    return s


def read_translations(path, key_fn, keep):
    """Rows (source word, pos, sense, target_key, tags) of a translations TSV whose target key is in keep."""
    rows = []
    if not os.path.exists(path):
        return rows
    for r in read_tsv(path):
        if len(r) < 4:
            continue
        tk = key_fn(r[3])
        if tk in keep:
            rows.append((r[0], r[1], r[2], tk, r[4] if len(r) > 4 else ""))
    return rows


class Cands(object):
    """(keyword, lemma) -> {sense: [exact, gloss_component, penalty, caps]}"""

    def __init__(self):
        self.d = {}

    def add(self, kw, lemma, sense, exact=0, comp=0, pen=0, cap=False):
        if not kw:
            return
        e = self.d.setdefault((kw, lemma), {}).setdefault(sense, [0, 0, 0, False])
        e[0] = max(e[0], exact)
        e[1] = max(e[1], comp)
        e[2] = min(e[2], pen)
        e[3] = e[3] or cap


def score_parts(exact, comp, pen, cap, bonus, is_name):
    w = exact + comp + bonus + pen
    if is_name and not cap:
        w -= 20
    return max(0, min(255, w))


def finish_cands(cands, bonus, names, pos_of, sense_pen):
    """-> sorted list of (kw, lemma, sense, score, pos)."""
    out = []
    for (kw, lemma), senses in cands.d.items():
        best = None
        for sense in sorted(senses):
            exact, comp, pen, cap = senses[sense]
            p = min(pen, sense_pen(lemma, sense))
            sc = score_parts(exact, comp, p, cap, bonus.get(lemma, 0), lemma in names)
            if best is None or sc > best[0]:
                best = (sc, sense)
        out.append((kw, lemma, best[1], best[0], pos_of(lemma)))
    out.sort(key=lambda r: (r[0].encode("utf-8"), -r[3], r[1], r[2]))
    return out


# -------------------------------------------------------------------------------------------------------------
def run(lang, out, curated_dir=None):
    t0 = time.time()
    if lang not in ("la", "grc"):
        return {"counts": {}, "skipped": "no glosses for %s" % lang}
    d = os.path.join(out, lang)
    cdir = curated_dir or T.CURATED
    key_fn = vptext.key_for(lang)
    c = collections.Counter()
    infos, by_key = lexdata.load_lemmas(out, lang, keep_rec=True)
    n = len(infos)
    sig = T.load_signals(out, lang, infos)
    t1, _missing = T.tier1(lang, infos, by_key, sig, cdir)
    names = set(i.id for i in infos if T.is_name(i))
    log("[%s gloss] %d lemmas, tier1 %d, dcc %d, whitaker %d" % (lang, n, len(t1), len(sig["dcc"]), len(sig["whit"])))

    # ---- English gloss words and lemmatiser --------------------------------------------------------------
    stop_en = load_stopwords("en")
    stop_es = load_stopwords("es")
    freq_en = collections.Counter()
    for info in infos:
        for s in info.rec.get("senses") or []:
            for w, _ in gloss_words(s.get("g", "")):
                freq_en[w] += 1
    en_trans = read_translations(os.path.join(out, "en", "translations_%s.tsv" % lang), key_fn, by_key)
    extra_en = collections.defaultdict(list)
    if lang == "la":
        for r in lexdata.read_rows(os.path.join(d, "whitaker.tsv")):
            ids = lexdata.parse_ids(r[14] if len(r) > 14 else "")
            if ids and r[12]:
                text = re.sub(r"\[.*?\]", " ", r[12]).split(";")[0]
                for j in ids:
                    extra_en[j].append(text)
    for r in lexdata.read_rows(os.path.join(d, "dcc_la.tsv" if lang == "la" else "dcc_grc.tsv")):
        for j in lexdata.parse_ids(r[7] if len(r) > 7 else ""):
            extra_en[j].append(r[3].split(";")[0])
    for texts in extra_en.values():
        for t in texts:
            for w2, _ in gloss_words(t):
                freq_en[w2] += 1
    lem_en = Lemmatiser("en", out, set(freq_en), freq_en)
    log("[%s gloss] English lemmatiser: %d gloss words, %d mapped to another key" % (
        lang, len(freq_en), sum(1 for k, v in lem_en.map.items() if k != v)))

    # ---- senses --------------------------------------------------------------------------------------------
    valency = {}
    if lang == "la":
        for row in lexdata.read_curated(os.path.join(cdir, "valency_la.tsv")):
            if len(row) >= 2 and row[0] != "key":
                valency[row[0]] = frame_bits(row[1])
    valency_ids = {}
    for k, bits in valency.items():
        k = key_fn(k)
        ids = lexdata.pick_lemmas(by_key.get(k), infos, "verb") or lexdata.pick_lemmas(by_key.get(k), infos)
        i = T.best_single(ids, infos, sig)
        if i is not None:
            valency_ids[i] = bits
    sense_kw = [None] * n       # per lemma: list of (heads, others, caps)
    sense_tagv = [None] * n
    gloss_en = [""] * n
    gloss_en_src = [""] * n
    for info in infos:
        i = info.id
        senses = info.rec.get("senses") or []
        kws, tg = [], []
        for s in senses:
            g = s.get("g", "")
            kws.append(extract_keywords(g, lem_en, stop_en))
            tg.append(sense_tags(s) | valency_ids.get(i, 0))
        sense_kw[i] = kws
        sense_tagv[i] = tg
        for s in senses:
            if s.get("g"):
                gloss_en[i] = one_line(s["g"])
                gloss_en_src[i] = "kaikki"
                break
    # lemmas without senses: the gloss of the lemma their form/alt table points to, then LS / Whitaker / LSJ
    for info in infos:
        i = info.id
        if gloss_en[i]:
            continue
        for t in info.rec.get("of") or []:
            ids = lexdata.pick_lemmas(by_key.get(key_fn(t)), infos)
            ids = [j for j in ids if gloss_en[j] and gloss_en_src[j] == "kaikki"]
            if ids:
                gloss_en[i] = gloss_en[ids[0]]
                gloss_en_src[i] = "of"
                break
    fallback = {}
    if lang == "la":
        for r in lexdata.read_rows(os.path.join(d, "ls.tsv")):
            ids = lexdata.parse_ids(r[10] if len(r) > 10 else "")
            g = (r[8] if len(r) > 8 else "") or (r[7].split("; ")[0] if len(r) > 7 else "")
            for j in ids:
                if g and j not in fallback:
                    fallback[j] = (one_line(g), "ls")
        for r in lexdata.read_rows(os.path.join(d, "whitaker.tsv")):
            for j in lexdata.parse_ids(r[14] if len(r) > 14 else ""):
                if j not in fallback and r[12]:
                    fallback[j] = (one_line(r[12].split(";")[0]), "whitaker")
    else:
        for r in lexdata.read_rows(os.path.join(d, "lsj.tsv")):
            ids = lexdata.parse_ids(r[7] if len(r) > 7 else "")
            g = r[6].split("; ")[0] if len(r) > 6 else ""
            for j in ids:
                if g and j not in fallback:
                    fallback[j] = (one_line(g), "lsj")
    for i in range(n):
        if not gloss_en[i] and i in fallback:
            gloss_en[i], gloss_en_src[i] = fallback[i]
    for i in range(n):
        c["gloss_en_" + (gloss_en_src[i] or "none")] += 1

    # ---- Spanish glosses -------------------------------------------------------------------------------------
    gloss_es = [""] * n
    gloss_es_src = [""] * n
    es_kw = collections.defaultdict(list)    # lemma -> [(text, kind)] kind: exact, gloss, pivot
    if lang == "la":
        for row in lexdata.read_curated(os.path.join(cdir, "gloss_es_la.tsv")):
            row = (row + ["", "", ""])[:3]
            k, head, ges = row
            if not ges or k == "key":
                continue
            ids = lexdata.pick_lemmas(by_key.get(key_fn(k or head)), infos)
            if head and len(ids) > 1:
                ids = [j for j in ids if vptext.nfc(infos[j].head) == vptext.nfc(head)] or ids
            j = T.best_single(ids, infos, sig)
            if j is None:
                c["curated_es_unmatched"] += 1
                continue
            if not gloss_es[j]:
                gloss_es[j], gloss_es_src[j] = one_line(ges), "curated"
            es_kw[j].append((ges, "curated"))
        for r in lexdata.read_rows(os.path.join(d, "dcc_la_es.tsv")):
            ids = lexdata.parse_ids(r[7] if len(r) > 7 else "")
            g = es_clean(r[3])
            for j in ids:
                if g and not gloss_es[j]:
                    gloss_es[j], gloss_es_src[j] = one_line(g), "dcc"
                if g:
                    es_kw[j].append((g, "gloss"))
    eswikt = os.path.join(out, "es", "latin_glosses.tsv")
    if os.path.exists(eswikt):
        for r in read_tsv(eswikt):
            if len(r) < 4 or r[3] != lang or (len(r) > 5 and r[5]) or ES_FORM_GLOSS.match(r[2] or ""):
                continue
            k = key_fn(r[0])
            ids = lexdata.pick_lemmas(by_key.get(k), infos, tagmap.KAIKKI_POS.get(r[1])) or \
                lexdata.pick_lemmas(by_key.get(k), infos)
            for j in ids:
                g = es_clean(r[2], j in names)
                if not g:
                    continue
                if not gloss_es[j]:
                    gloss_es[j], gloss_es_src[j] = one_line(g), "eswikt"
                es_kw[j].append((g, "gloss"))
    # EN -> ES pivot: English head words of gloss_en -> their most common Spanish translation
    heads_needed = collections.defaultdict(set)
    for i in range(n):
        if not gloss_es[i] and gloss_en[i]:
            kws = sense_kw[i][0] if sense_kw[i] else extract_keywords(gloss_en[i], lem_en, stop_en)
            for h in kws[0][:3]:
                heads_needed[h].add(i)
    es_counts = collections.defaultdict(collections.Counter)  # (en word, pos) -> Counter(es word)
    tpath = os.path.join(out, "en", "translations_es.tsv")
    if os.path.exists(tpath):
        for r in read_tsv(tpath):
            if len(r) >= 4 and vptext.en_key(r[0]) in heads_needed and r[3]:
                es_counts[(vptext.en_key(r[0]), tagmap.KAIKKI_POS.get(r[1], "other"))][vptext.nfc(r[3])] += 1
    for i in range(n):
        if gloss_es[i] or not gloss_en[i]:
            continue
        kws = sense_kw[i][0] if sense_kw[i] else extract_keywords(gloss_en[i], lem_en, stop_en)
        want = EN_POS_OF.get(infos[i].fpos, ())
        got = []
        for h in kws[0][:3]:
            cnt = collections.Counter()
            for p in want:
                cnt.update(es_counts.get((h, p), {}))
            if not cnt:
                for (w, p), cc in es_counts.items():
                    if w == h:
                        cnt.update(cc)
            if cnt:
                best = min(cnt.items(), key=lambda kv: (-kv[1], kv[0]))[0]
                if best not in got:
                    got.append(best)
        if got:
            gloss_es[i], gloss_es_src[i] = one_line(", ".join(got)), "pivot"
            es_kw[i].append((", ".join(got), "pivot"))
    for i in range(n):
        if gloss_en[i] or gloss_es[i]:
            c["gloss_es_" + (gloss_es_src[i] or "none")] += 1
    c["gloss_es_covered"] = sum(1 for i in range(n) if gloss_es[i])
    c["gloss_en_covered"] = sum(1 for i in range(n) if gloss_en[i])

    # ---- scoring helpers ---------------------------------------------------------------------------------------
    bonus = {}
    for i in range(n):
        b = 0
        if sig["whit"].get(i) in ("A", "B"):
            b += 15
        if i in sig["dcc"]:
            b += 10
        if i in t1:
            b += 20
        if b:
            bonus[i] = b

    def pos_of(i):
        return lexdata.pos_code(infos[i].fpos)

    def sense_pen(i, s):
        tv = sense_tagv[i]
        if tv and s < len(tv) and tv[s] & PENALTY_BITS:
            return -30
        return 0

    # ---- English reverse index -----------------------------------------------------------------------------------
    cands = Cands()
    for i in range(n):
        for s, (heads, others, caps) in enumerate(sense_kw[i]):
            for h in heads:  # head of the lemma gloss (sense 0) +60; heads of later senses count as content words
                cands.add(h, i, s, comp=60 if s == 0 else 35, cap=h in caps)
            for o in others:
                cands.add(o, i, s, comp=35, cap=o in caps)
    # other lemma glosses (Whitaker meanings, DCC definitions) count like the first sense
    for i, texts in extra_en.items():
        if not sense_kw[i]:
            continue
        for text in texts:
            heads, others, caps = extract_keywords(text, lem_en, stop_en)
            for h in heads:
                cands.add(h, i, 0, comp=60, cap=h in caps)
            for o in others:
                cands.add(o, i, 0, comp=35, cap=o in caps)
    for en_word, en_pos, _sense, tk, tags in en_trans:
        want = tagmap.KAIKKI_POS.get(en_pos, None)
        ids = lexdata.pick_lemmas(by_key.get(tk), infos, want if want in lexdata.POS_COMPAT else None)
        if not ids:
            ids = lexdata.pick_lemmas(by_key.get(tk), infos)
        ids = [j for j in ids if sense_kw[j]]
        if not ids:
            c["pivot_rows_unjoined"] += 1
            continue
        kw = vptext.en_key(en_word).strip()
        if len(ids) > 1:  # homographs (volō "want" / volō "fly"): keep those whose glosses contain the word
            hit = [j for j in ids if any(kw in h or kw in o for h, o, _c in sense_kw[j])]
            ids = hit or ids
        pen = -30 if set(tags.split()) & PEN_ROW_TAGS else 0
        cap = en_word[:1].isupper()
        for j in ids:
            s = 0
            for si, (heads, others, _caps) in enumerate(sense_kw[j]):
                if kw in heads:
                    s = si
                    break
            else:
                for si, (heads, others, _caps) in enumerate(sense_kw[j]):
                    if kw in others:
                        s = si
                        break
            cands.add(kw, j, s, exact=100, pen=pen, cap=cap)
        c["pivot_rows_joined"] += 1
    rev_en = finish_cands(cands, bonus, names, pos_of, sense_pen)
    del cands

    # ---- Spanish reverse index ------------------------------------------------------------------------------------
    freq_es = collections.Counter()
    for i, lst in es_kw.items():
        for text, _kind in lst:
            for w, _ in gloss_words(text):
                freq_es[w] += 1
    es_trans = read_translations(os.path.join(out, "es", "translations_%s.tsv" % lang), key_fn, by_key)
    for r in es_trans:
        freq_es[vptext.es_key(r[0])] += 0
    lem_es = Lemmatiser("es", out, set(freq_es), freq_es)
    cands = Cands()
    for i, lst in es_kw.items():
        if not sense_kw[i]:
            continue
        for text, kind in lst:
            heads, others, caps = extract_keywords(text, lem_es, stop_es)
            for h in heads:
                if kind == "curated":
                    cands.add(h, i, 0, exact=100, comp=60, cap=h in caps)
                elif kind == "pivot":
                    cands.add(h, i, 0, comp=35, cap=h in caps)
                else:
                    cands.add(h, i, 0, comp=60, cap=h in caps)
            if kind != "pivot":
                for o in others:
                    cands.add(o, i, 0, comp=35, cap=o in caps)
    for es_word, es_pos, _si, tk, tags in es_trans:
        ids = lexdata.pick_lemmas(by_key.get(tk), infos, tagmap.KAIKKI_POS.get(es_pos)) or \
            lexdata.pick_lemmas(by_key.get(tk), infos)
        ids = [j for j in ids if sense_kw[j]]
        if not ids:
            c["es_translation_rows_unjoined"] += 1
            continue
        kw = vptext.es_key(es_word).strip()
        pen = -30 if set(tags.split()) & PEN_ROW_TAGS else 0
        for j in ids:
            cands.add(kw, j, 0, exact=100, pen=pen, cap=es_word[:1].isupper())
        c["es_translation_rows_joined"] += 1
    rev_es = finish_cands(cands, bonus, names, pos_of, sense_pen)
    del cands

    # ---- write -------------------------------------------------------------------------------------------------
    w = TsvWriter(os.path.join(d, "gloss.tsv"))
    w.write("#lemma_id", "gloss_en", "gloss_en_src", "gloss_es", "gloss_es_src", "n_senses")
    ws = TsvWriter(os.path.join(d, "senses.tsv"))
    ws.write("#lemma_id", "sense_idx", "gloss_en", "gloss_es", "keywords", "tags", "rank")
    for info in infos:
        i = info.id
        w.write(i, gloss_en[i], gloss_en_src[i], gloss_es[i], gloss_es_src[i], len(sense_kw[i]))
        for s, sv in enumerate(info.rec.get("senses") or []):
            heads, others, _caps = sense_kw[i][s]
            g = re.sub(r"\s+", " ", sv.get("g", "")).strip()
            if len(g) > SENSE_LEN:
                g = g[:SENSE_LEN - 1].rstrip() + "…"
            ws.write(i, s, g, gloss_es[i] if s == 0 else "", " ".join(heads + others), sense_tagv[i][s], s + 1)
            c["senses"] += 1
            if sense_tagv[i][s]:
                c["senses_tagged"] += 1
    w.close()
    ws.close()
    for name, rows in (("revx_en.tsv", rev_en), ("revx_es.tsv", rev_es)):
        wr = TsvWriter(os.path.join(d, name))
        wr.write("#keyword", "lemma_id", "sense_idx", "score", "pos")
        for kw, lemma, sense, score, pos in rows:
            wr.write(kw, lemma, sense, score, pos)
        wr.close()
    c["revx_en_keywords"] = len(set(r[0] for r in rev_en))
    c["revx_en_candidates"] = len(rev_en)
    c["revx_es_keywords"] = len(set(r[0] for r in rev_es))
    c["revx_es_candidates"] = len(rev_es)
    res = {"counts": dict(c), "duration_s": round(time.time() - t0, 1), "peak_rss_mb": peak_rss_mb()}
    log("[%s gloss] done: %s" % (lang, " ".join("%s=%s" % kv for kv in sorted(c.items()))))
    return res


def outputs(lang):
    return ["gloss.tsv", "senses.tsv", "revx_en.tsv", "revx_es.tsv"] if lang in ("la", "grc") else []
