"""Stage `import_aux`: auxiliary dictionaries -> TSVs joined to the Kaikki lemma ids.

Inputs (downloaded by `fetch.sh aux` into <raw>/aux, see SOURCES.json there) and outputs (<out>/<lang>/):
  la  whitaker/DICTLINE.GEN  -> whitaker.tsv           one row per DICTLINE line (stems, POS, grammar codes,
                                                       age/area/geo/frequency/source letters, meaning, guessed
                                                       citation key, joined Kaikki lemma ids)
      whitaker/INFLECTS.LAT  -> whitaker_inflects.tsv  ending table (pos, grammar, stem key, length, ending,
                                                       age, frequency)
      perseus/lat.ls...xml   -> ls.tsv                 Lewis & Short: key, homograph, type, orth (length marks),
                                                       itype, gen, pos, short <tr> glosses, first italic sense
                                                       text, usg labels, Kaikki ids
      dcc/latin-core-list.csv, latin-core-list-es.csv -> dcc_la.tsv, dcc_la_es.tsv
  grc perseus/grc.lsj...eng1..27.xml -> lsj.tsv        LSJ: Beta Code key and orth converted (betacode.py)
      dcc/greek-core-list.csv -> dcc_grc.tsv
The XML is streamed with xml.etree.iterparse (expat; the external DTD is never fetched) and every finished
entry is cleared. Join rates go into the stage meta (import_aux.json) and report.json.
"""
import collections
import csv
import glob
import io
import os
import re
import time
import xml.etree.ElementTree as ET

import betacode
import lexdata
import vptext
from common import TsvWriter, log, peak_rss_mb

# ---------------------------------------------------------------------------------------------------------------
# Whitaker's Words
# Column layout (0-based) of DICTLINE.GEN: stems at 0, 19, 38, 57 (19 wide); POS at 76 (7 wide); grammar codes
# 83-99; single letters age 100, area 102, geography 104, frequency 106, source 108; meaning from 110.
# Letter meanings: src/latin_utils/latin_utils-inflections_package.ads (Age_Type, Frequency_Type) and
# latin_utils-dictionary_package.ads (Area_Type, Geo_Type, Source_Type) of mk270/whitakers-words, copied
# into tools/build_library/README.md.
WH_POS = {"N": "noun", "V": "verb", "ADJ": "adj", "ADV": "adv", "PREP": "prep", "CONJ": "conj", "INTERJ": "intj",
          "NUM": "num", "PRON": "pron", "PACK": None}
WHITAKER_HEADER = ("#line", "stem1", "stem2", "stem3", "stem4", "pos", "codes", "age", "area", "geo", "freq",
                   "source", "meaning", "cite_key", "lemma_ids")


def parse_dictline(line):
    """One DICTLINE.GEN line -> dict (None for blank lines)."""
    line = line.rstrip("\r\n")
    if not line.strip():
        return None
    line = line.ljust(110)

    def stem(a):
        s = line[a:a + 19].strip()
        return "" if s == "zzz" else s

    return {"stems": [stem(0), stem(19), stem(38), stem(57)], "pos": line[76:83].strip(),
            "codes": line[83:100].split(), "age": line[100], "area": line[102], "geo": line[104],
            "freq": line[106], "source": line[108], "meaning": line[110:].strip()}


def _noun_endings(decl, var, gender):
    neuter = gender == "N"
    if decl == "1":
        return {"6": ["e", "a"], "7": ["es", "a"], "8": ["as", "a"]}.get(var, ["a", "e", "es", "as"])
    if decl == "2":
        if var == "3":
            return ["", "us"]
        if neuter:
            return ["um", "on", "us", ""]
        return ["us", "os", "", "um", "on"]
    if decl == "3":
        return ["", "is", "es", "e", "s"]
    if decl == "4":
        return ["u", "us"] if neuter else ["us", "u"]
    if decl == "5":
        return ["es"]
    return ["", "a", "us", "um", "es"]


def _verb_endings(conj, var, kind):
    dep = kind in ("DEP", "SEMIDEP")
    if kind == "IMPERS":
        base = {"1": ["at"], "2": ["et"], "3": ["it"]}.get(conj, []) + ["it", "et", "at"]
    elif conj == "1":
        base = ["o", "or"]
    elif conj == "2":
        base = ["eo", "eor"]
    elif conj == "3" and var == "4":
        base = ["io", "o", "ior", "or"]
    elif conj == "3":
        base = ["o", "io", "or", "ior"]
    elif conj == "5":
        base = ["um", "sum"]
    else:
        base = ["o", "io", "eo", "um", "or", "am", "it"]
    if dep:
        base = [e for e in base if e.endswith("r")] + [e for e in base if not e.endswith("r")]
    return base + ["o", "or", "eo", "io", "um"]


def citation_forms(e):
    """Candidate citation forms (dictionary headwords) for a DICTLINE entry, most likely first."""
    s1 = e["stems"][0]
    pos = e["pos"]
    c = e["codes"] + ["", "", "", ""]
    if pos == "N":
        ends = _noun_endings(c[0], c[1], c[2])
    elif pos == "V":
        ends = _verb_endings(c[0], c[1], c[2])
    elif pos == "ADJ":
        d, v = c[0], c[1]
        if d == "1" and v == "1":
            ends = ["us", "", "er"]
        elif d == "3" and v == "2":
            ends = ["is", ""]
        elif d in ("1", "3", "0", "9"):
            ends = ["", "us", "is", "er"]
        else:
            ends = ["os", "es", "us", "", "is", "on"]
    elif pos == "NUM":
        ends = ["us", "", "o", "es", "i", "a", "ae", "um"]
    elif pos == "PRON":
        if s1 == "qu":  # the relative/interrogative stems: quī (REL, ADJECT), quis (INTERR, INDEF)
            ends = ["i", "is"] if c[2] in ("REL", "ADJECT") else ["is", "i"]
        else:
            ends = ["", "e", "s", "us", "is", "ic", "ud", "a", "i", "o"]
    else:
        ends = [""]
    out = []
    for x in ends:
        f = s1 + x
        if f and f not in out:
            out.append(f)
    return out


def join_whitaker(e, infos, by_key, alt=None):
    """(cite_key, ids): the first citation form whose key is a Kaikki lemma of a compatible POS; then the same
    through alternative-spelling form pages (adcessus -> accessus)."""
    want = WH_POS.get(e["pos"])
    cands = citation_forms(e)
    hints = [s for s in e["stems"][1:] if s]
    for f in cands:
        k = vptext.latin_key(f)
        ids = lexdata.pick_lemmas(by_key.get(k), infos, want, hints, vptext.latin_key)
        if ids:
            return k, ids
    if alt:
        for f in cands:
            k = vptext.latin_key(f)
            ids, how = lexdata.join_key(k, infos, by_key, alt, want, hints, vptext.latin_key)
            if ids:
                return k, ids
    return (vptext.latin_key(cands[0]) if cands else ""), []


def parse_inflect(line):
    """One INFLECTS.LAT line -> (pos, grammar, stem_key, ending_len, ending, age, freq) or None."""
    line = line.split("--", 1)[0].strip()
    if not line:
        return None
    t = line.split()
    if len(t) < 5:
        return None
    age, freq = t[-2], t[-1]
    if t[-3].isdigit() and t[-3] == "0":
        ending, elen, skey, head = "", "0", t[-4], t[:-4]
    else:
        ending, elen, skey, head = t[-3], t[-4], t[-5], t[:-5]
    if not head:
        return None
    return head[0], " ".join(head[1:]), skey, elen, ending, age, freq


def import_whitaker(aux, d, infos, by_key, c, alt=None):
    path = os.path.join(aux, "whitaker", "DICTLINE.GEN")
    w = TsvWriter(os.path.join(d, "whitaker.tsv"))
    w.write(*WHITAKER_HEADER)
    matched_lemmas = set()
    freq_hist = collections.Counter()
    age_hist = collections.Counter()
    with open(path, "r", encoding="latin-1", newline="") as f:
        for n, line in enumerate(f, 1):
            e = parse_dictline(line)
            if e is None:
                continue
            c["whitaker_entries"] += 1
            freq_hist[e["freq"]] += 1
            age_hist[e["age"]] += 1
            if WH_POS.get(e["pos"], "x") is None:
                c["whitaker_pack"] += 1
                cite, ids = "", []
            else:
                c["whitaker_joinable"] += 1
                cite, ids = join_whitaker(e, infos, by_key, alt)
                if ids:
                    c["whitaker_joined"] += 1
                    matched_lemmas.update(ids)
            w.write(n, *(e["stems"] + [e["pos"], " ".join(e["codes"]), e["age"], e["area"], e["geo"], e["freq"],
                                       e["source"], e["meaning"], cite, lexdata.ids_field(ids)]))
    w.close()
    n_lemma = sum(1 for i in infos if i.kind == "lemma")
    c["whitaker_kaikki_lemmas_covered"] = sum(1 for i in matched_lemmas if infos[i].kind == "lemma")
    w2 = TsvWriter(os.path.join(d, "whitaker_inflects.tsv"))
    w2.write("#pos", "grammar", "stem_key", "ending_len", "ending", "age", "freq")
    with open(os.path.join(aux, "whitaker", "INFLECTS.LAT"), "r", encoding="latin-1", newline="") as f:
        for line in f:
            r = parse_inflect(line.rstrip("\r\n"))
            if r:
                w2.write(*r)
                c["whitaker_inflects"] += 1
    w2.close()
    return {"whitaker_join_rate": _rate(c["whitaker_joined"], c["whitaker_joinable"]),
            "whitaker_kaikki_coverage": _rate(c["whitaker_kaikki_lemmas_covered"], n_lemma),
            "whitaker_freq": dict(sorted(freq_hist.items())), "whitaker_age": dict(sorted(age_hist.items()))}


def _rate(a, b):
    return round(float(a) / b, 4) if b else 0.0


# ---------------------------------------------------------------------------------------------------------------
# Perseus lexica
LS_POS = [("v. a.", "verb"), ("v. n.", "verb"), ("v. dep.", "verb"), ("verb", "verb"), ("adj.", "adj"),
          ("adv.", "adv"), ("prep.", "prep"), ("conj.", "conj"), ("interj.", "intj"), ("pron.", "pron"),
          ("num.", "num"), ("P. a.", "adj")]
MAX_TR = 5


def _text(el):
    return re.sub(r"\s+", " ", "".join(el.itertext())).strip()


def _short(s, n=60):
    s = s.strip().strip(",;:.").strip()
    return s if len(s) <= n else ""


def iter_entries(path):
    """Stream <entryFree> elements of a Perseus TEI P4 file; the caller must not keep references."""
    ctx = ET.iterparse(path, events=("start", "end"))
    root = None
    for ev, el in ctx:
        if root is None and ev == "start":
            root = el
        if ev == "end" and el.tag == "entryFree":
            yield el
            el.clear()
            if root is not None:
                root.clear()


def ls_entry(el):
    """Lewis & Short <entryFree> -> dict."""
    key_attr = el.get("key") or ""
    base, hom = betacode.strip_key_digits(key_attr)
    orths, itype, gen, pos, trs, usg = [], "", "", "", [], []
    first_ital = ""
    for x in el.iter():
        t = x.tag
        if t == "orth":
            orths.append(_text(x))
        elif t == "itype" and not itype:
            itype = _text(x)
        elif t == "gen" and not gen:
            gen = _text(x)
        elif t == "pos" and not pos:
            pos = _text(x)
        elif t == "tr":
            s = _short(_text(x), 40)
            if s and s not in trs and len(trs) < MAX_TR:
                trs.append(s)
        elif t == "usg":
            s = _text(x).strip(".,; ")
            if s and s not in usg:
                usg.append(s)
        elif t == "sense" and not first_ital:
            for h in x.iter("hi"):
                if h.get("rend") == "ital":
                    first_ital = _short(_text(h), 80)
                    if first_ital:
                        break
    orth = orths[0] if orths else base
    orth = orth.replace("^", "").replace("_", "").strip(",;:. ")
    orth = orth.split(",")[0].strip()
    if "-" in orth and "-" not in base:
        orth = orth.replace("-", "")
    return {"key_attr": key_attr, "key": vptext.latin_key(base), "hom": hom, "type": el.get("type") or "",
            "orth": vptext.nfc(orth), "itype": itype, "gen": gen, "pos": pos, "tr": trs, "def": first_ital,
            "usg": usg}


def ls_pos(pos):
    for pat, name in LS_POS:
        if pos.startswith(pat):
            return name
    return None


def import_ls(aux, d, infos, by_key, c, alt=None):
    path = os.path.join(aux, "perseus", "lat.ls.perseus-eng1.xml")
    w = TsvWriter(os.path.join(d, "ls.tsv"))
    w.write("#key", "hom", "type", "orth", "itype", "gen", "pos", "tr", "def", "usg", "lemma_ids")
    covered = set()
    for el in iter_entries(path):
        e = ls_entry(el)
        c["ls_entries"] += 1
        ids, _ = lexdata.join_key(e["key"], infos, by_key, alt or {}, ls_pos(e["pos"]))
        if not ids:
            ids, _ = lexdata.join_key(e["key"], infos, by_key, alt or {})
        if e["type"] == "main":
            c["ls_main"] += 1
            if ids:
                c["ls_main_joined"] += 1
        if ids:
            c["ls_joined"] += 1
            covered.update(ids)
        w.write(e["key"], e["hom"], e["type"], e["orth"], e["itype"], e["gen"], e["pos"], "; ".join(e["tr"]),
                e["def"], ",".join(e["usg"]), lexdata.ids_field(ids))
    w.close()
    n_lemma = sum(1 for i in infos if i.kind == "lemma")
    c["ls_kaikki_lemmas_covered"] = sum(1 for i in covered if infos[i].kind == "lemma")
    return {"ls_join_rate": _rate(c["ls_joined"], c["ls_entries"]),
            "ls_main_join_rate": _rate(c["ls_main_joined"], c["ls_main"]),
            "ls_kaikki_coverage": _rate(c["ls_kaikki_lemmas_covered"], n_lemma)}


def lsj_entry(el):
    key_attr = el.get("key") or ""
    base, hom = betacode.strip_key_digits(key_attr)
    orth, gen, trs = "", "", []
    for x in el.iter():
        t = x.tag
        if t == "orth" and not orth:
            orth = _text(x)
        elif t == "gen" and not gen:
            gen = _text(x)
        elif t == "tr":
            s = _short(_text(x), 40)
            if s and s not in trs and len(trs) < MAX_TR:
                trs.append(s)
    uni = betacode.beta_to_unicode(base)
    return {"beta": key_attr, "key": vptext.greek_key(uni), "bare": vptext.greek_bare(uni), "hom": hom,
            "orth": betacode.beta_to_unicode(orth.strip(",;:. ")) if orth else uni,
            "gen": betacode.beta_to_unicode(gen) if gen else "", "tr": trs}


def lsj_files(aux):
    files = glob.glob(os.path.join(aux, "perseus", "grc.lsj.perseus-eng*.xml"))
    return sorted(files, key=lambda p: int(re.search(r"eng(\d+)\.xml$", p).group(1)))


def import_lsj(aux, d, infos, by_key, c):
    by_bare = {}
    for i in infos:
        by_bare.setdefault(vptext.greek_bare(i.key), []).append(i.id)
    w = TsvWriter(os.path.join(d, "lsj.tsv"))
    w.write("#key", "bare", "hom", "beta", "orth", "gen", "tr", "lemma_ids", "match")
    covered = set()
    files = lsj_files(aux)
    c["lsj_files"] = len(files)
    for p in files:
        for el in iter_entries(p):
            e = lsj_entry(el)
            c["lsj_entries"] += 1
            ids = lexdata.pick_lemmas(by_key.get(e["key"]), infos)
            how = "exact" if ids else ""
            if not ids:
                bare = by_bare.get(e["bare"], [])
                if len(set(infos[i].key for i in bare)) == 1:  # unambiguous without accents
                    ids = lexdata.pick_lemmas(bare, infos)
                    how = "bare" if ids else ""
            if ids:
                c["lsj_joined_" + how] += 1
                covered.update(ids)
            w.write(e["key"], e["bare"], e["hom"], e["beta"], e["orth"], e["gen"], "; ".join(e["tr"]),
                    lexdata.ids_field(ids), how)
        log("[grc import_aux] %s: %d LSJ entries so far" % (os.path.basename(p), c["lsj_entries"]))
    w.close()
    n_lemma = sum(1 for i in infos if i.kind == "lemma")
    c["lsj_kaikki_lemmas_covered"] = sum(1 for i in covered if infos[i].kind == "lemma")
    joined = c["lsj_joined_exact"] + c["lsj_joined_bare"]
    return {"lsj_join_rate": _rate(joined, c["lsj_entries"]),
            "lsj_join_rate_exact": _rate(c["lsj_joined_exact"], c["lsj_entries"]),
            "lsj_kaikki_coverage": _rate(c["lsj_kaikki_lemmas_covered"], n_lemma)}


# ---------------------------------------------------------------------------------------------------------------
# Dickinson College Commentaries core lists
DCC_POS = [("noun", "noun"), ("sustantivo", "noun"), ("verb", "verb"), ("verbo", "verb"), ("adjective", "adj"),
           ("adjetivo", "adj"), ("adverb", "adv"), ("adverbio", "adv"), ("preposition", "prep"),
           ("preposici", "prep"), ("conjunc", "conj"), ("conjunction", "conj"), ("pronoun", "pron"),
           ("pronombre", "pron"), ("interjec", "intj"), ("numer", "num"), ("número", "num"),
           ("definite article", "article"), ("particle", "particle"), ("partícula", "particle")]
DCC_HEADER = ("#key", "head", "rank", "definition", "pos", "group", "headword", "lemma_ids")


def dcc_pos(s):
    s = (s or "").lower()
    for pat, name in DCC_POS:
        if s.startswith(pat):
            return name
    return None


def dcc_head_tokens(headword):
    """'abeō -īre -iī -itum' -> ['abeō', '-īre', ...]; 'εἰμί, ἔσομαι, impf. ἦν' -> ['εἰμί', 'ἔσομαι', ...];
    'μέν...δέ' -> ['μέν', 'δέ']; 'εἴκοσι(ν)' -> ['εἴκοσι']."""
    h = re.sub(r"\(.*?\)", "", headword or "")
    h = h.replace("\u2026", " ").replace("...", " ").replace("/", " ")
    return [t for t in re.split(r"[\s,;]+", h) if t and t not in ("impf.", "fut.", "aor.", "pf.")]


def dcc_key_variants(k, lang):
    """The key itself, then the enclitic spelling (que -> -que), then for Latin a plural headword's singular
    (singuli -> singulus, maiores -> maior)."""
    out = [k]
    if not k.startswith("-") and lang == "la" and k in ("que", "ue", "ne", "ce"):
        out.append("-" + k)
    if lang == "la":
        if k.endswith("i"):
            out.append(k[:-1] + "us")
        if k.endswith("es"):
            out.append(k[:-2])
    return out


def read_dcc(path):
    with open(path, "r", encoding="utf-8-sig", newline="") as f:
        data = f.read()
    rows = list(csv.reader(io.StringIO(data)))
    return rows[0], rows[1:]


def import_dcc(path, out_path, lang, infos, by_key, c, label, alt=None):
    key_fn = vptext.key_for(lang)
    _, rows = read_dcc(path)
    w = TsvWriter(out_path)
    w.write(*DCC_HEADER)
    for r in rows:
        if len(r) < 5 or not r[0].strip():
            continue
        head, definition, pos, group, rank = r[0].strip(), r[1].strip(), r[2].strip(), r[3].strip(), r[4].strip()
        toks = dcc_head_tokens(head)
        if not toks:
            continue
        c[label + "_rows"] += 1
        first = vptext.nfc(toks[0])
        k = key_fn(first)
        ids = []
        for kk in dcc_key_variants(k, lang):
            ids, _ = lexdata.join_key(kk, infos, by_key, alt or {}, dcc_pos(pos), toks[1:], key_fn)
            if not ids:
                ids, _ = lexdata.join_key(kk, infos, by_key, alt or {}, None, toks[1:], key_fn)
            if ids:
                break
        if ids:
            c[label + "_joined"] += 1
        w.write(k, first, rank, definition, pos, group, head, lexdata.ids_field(ids))
    w.close()
    return {label + "_join_rate": _rate(c[label + "_joined"], c[label + "_rows"])}


# ---------------------------------------------------------------------------------------------------------------
def run(lang, aux, out):
    t0 = time.time()
    d = os.path.join(out, lang)
    c = collections.Counter()
    res = {}
    if lang not in ("la", "grc"):
        return {"counts": {}, "skipped": "no auxiliary sources for %s" % lang}
    infos, by_key = lexdata.load_lemmas(out, lang)
    alt = lexdata.load_alt_targets(out, lang)
    if lang == "la":
        res.update(import_whitaker(aux, d, infos, by_key, c, alt))
        log("[la import_aux] whitaker: %d entries, %d joined" % (c["whitaker_entries"], c["whitaker_joined"]))
        res.update(import_ls(aux, d, infos, by_key, c, alt))
        log("[la import_aux] Lewis & Short: %d entries, %d joined" % (c["ls_entries"], c["ls_joined"]))
        res.update(import_dcc(os.path.join(aux, "dcc", "latin-core-list.csv"), os.path.join(d, "dcc_la.tsv"), "la",
                              infos, by_key, c, "dcc_la", alt))
        res.update(import_dcc(os.path.join(aux, "dcc", "latin-core-list-es.csv"), os.path.join(d, "dcc_la_es.tsv"),
                              "la", infos, by_key, c, "dcc_la_es", alt))
    else:
        res.update(import_lsj(aux, d, infos, by_key, c))
        res.update(import_dcc(os.path.join(aux, "dcc", "greek-core-list.csv"), os.path.join(d, "dcc_grc.tsv"), "grc",
                              infos, by_key, c, "dcc_grc", alt))
    res = {"counts": dict(c), "join": res, "duration_s": round(time.time() - t0, 1), "peak_rss_mb": peak_rss_mb()}
    log("[%s import_aux] done: %s" % (lang, " ".join("%s=%s" % kv for kv in sorted(res["join"].items())
                                                    if not isinstance(kv[1], dict))))
    return res


def outputs(lang):
    return {"la": ["whitaker.tsv", "whitaker_inflects.tsv", "ls.tsv", "dcc_la.tsv", "dcc_la_es.tsv"],
            "grc": ["lsj.tsv", "dcc_grc.tsv"]}.get(lang, [])


def inputs(lang, aux):
    if lang == "la":
        return [os.path.join(aux, "whitaker", "DICTLINE.GEN"), os.path.join(aux, "whitaker", "INFLECTS.LAT"),
                os.path.join(aux, "perseus", "lat.ls.perseus-eng1.xml"),
                os.path.join(aux, "dcc", "latin-core-list.csv"), os.path.join(aux, "dcc", "latin-core-list-es.csv")]
    if lang == "grc":
        return lsj_files(aux) + [os.path.join(aux, "dcc", "greek-core-list.csv")]
    return []
