#!/usr/bin/env python3
"""Core vocabulary audit of the reading directions (task C41): la2x (la-en, la-es) and grc2x (grc-en, grc-es).

Every lemma of a core list (a DCC-style TSV: key, head, rank, definition, pos, group, headword, lemma_ids) is run as
its citation form through `vpengine serve` (project kind text, one unit per lemma), optionally followed by our own
audit sentences (one per line, '#' comments). For each citation form the content words of the readable output are
compared with the core list's definition; each lemma gets one verdict per target language:

  ok        the output word is one of the core senses (light stemming)
  weak      only a secondary word of the output meets the definition ("go away" for "depart, go away" is ok;
            "the boat-house" for "house" is weak)
  nosense   no output word meets the definition
  long      the output word is a multi-word phrase of more than three content words (a definition, not a word)
  leak      Latin / Greek letters or a [bracketed] source word are left in the output
  missing   nothing printed for the word

The definitions are read at run time from the (gitignored) core list and never written into the report beyond the
lemma's head: the report lists lemma heads, verdicts, the engine's output and the confidence, so it can be kept next to
the data (data/work/eval/...). The Spanish side is compared with a Spanish core list when one is given (--dcc-es);
otherwise with the Spanish translations of the definition's English words (--en-es: data/work/en/translations_es.tsv).

Usage:
  core_audit.py --exe build/engine/cli/vpengine --lang la --dcc data/work/la/dcc_la.tsv \
                --dcc-es data/work/la/dcc_la_es.tsv --out data/work/eval/c41-audit/la [--sentences FILE]
  core_audit.py --exe ... --lang grc --dcc data/work/grc/dcc_grc.tsv --en-es data/work/en/translations_es.tsv \
                --out data/work/eval/c41-audit/grc
Python 3 standard library only. Deterministic (sorted output).
"""
import argparse
import os
import re
import shutil
import sys
import tempfile
import unicodedata

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import evallib as L  # noqa: E402

CONTENT_POS = ("noun", "verb", "adjective", "adverb")

STOP_EN = set("""a an the i you he she it we they me him her us them my your his its our their this that these those
is am are was were be been being not no to of and or but will would shall should can could may might must
there here o oh""".split())
STOP_ES = set("""el la los las un una unos unas lo le les me te se nos yo tú él ella nosotros ustedes ellos ellas mi
tu su sus mis tus de del al a y o pero no es son está están ser estar fue era hay he ha han oh""".split())


def fold(s):
    s = unicodedata.normalize("NFKD", s.lower())
    return "".join(c for c in s if not unicodedata.category(c).startswith("M"))


def words(s):
    return [w for w in re.split(r"[^\w'-]+", fold(s)) if w and not w.isdigit()]


def stem_en(w):
    for suf in ("ings", "ing", "ies", "ied", "es", "ed", "s", "ly", "er", "est"):
        if len(w) > len(suf) + 2 and w.endswith(suf):
            base = w[: -len(suf)]
            if suf in ("ies", "ied"):
                base += "y"
            return base
    return w


def stem_es(w):
    for suf in ("mente", "ando", "iendo", "ar", "er", "ir", "os", "as", "es", "o", "a", "e", "s"):
        if len(w) > len(suf) + 2 and w.endswith(suf):
            return w[: -len(suf)]
    return w


IRREGULAR_EN = {"men": "man", "women": "woman", "children": "child", "feet": "foot", "teeth": "tooth", "oxen": "ox",
                "mice": "mouse", "geese": "goose", "went": "go", "gone": "go", "saw": "see", "seen": "see",
                "gave": "give", "given": "give", "took": "take", "taken": "take", "came": "come", "made": "make",
                "said": "say", "told": "tell", "knew": "know", "known": "know", "led": "lead", "fled": "flee",
                "fell": "fall", "fallen": "fall", "ran": "run", "left": "leave", "sent": "send", "held": "hold",
                "brought": "bring", "bore": "bear", "born": "bear", "got": "get", "thought": "think",
                "sought": "seek", "fought": "fight", "taught": "teach", "caught": "catch", "won": "win",
                "began": "begin", "begun": "begin", "wrote": "write", "written": "write", "spoke": "speak",
                "spoken": "speak", "chose": "choose", "chosen": "choose", "rose": "rise", "risen": "rise",
                "lay": "lie", "lain": "lie", "sat": "sit", "stood": "stand", "found": "find", "heard": "hear",
                "lost": "lose", "met": "meet", "paid": "pay", "kept": "keep", "slept": "sleep", "felt": "feel",
                "fed": "feed", "drew": "draw", "drawn": "draw", "grew": "grow", "grown": "grow", "threw": "throw",
                "thrown": "throw", "broke": "break", "broken": "break", "better": "good", "best": "good",
                "worse": "bad", "worst": "bad", "greater": "great", "led": "lead", "dead": "die", "died": "die"}


def keys_en(ws):
    out = set()
    for w in ws:
        w = IRREGULAR_EN.get(w, w)
        out.add(w)
        out.add(stem_en(w))
    return out


def keys_es(ws):
    out = set()
    for w in ws:
        out.add(w)
        out.add(stem_es(w))
    return out


def read_core(path):
    rows = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            if line.startswith("#") or not line.strip():
                continue
            c = line.rstrip("\n").split("\t")
            while len(c) < 8:
                c.append("")
            rows.append({"key": c[0], "head": c[1], "rank": int(c[2]) if c[2].isdigit() else 0, "def": c[3],
                         "pos": c[4].split(":")[0].strip().lower(), "group": c[5], "headword": c[6],
                         "ids": [int(x) for x in c[7].split(",") if x.strip().isdigit()]})
    return rows


def read_en_es(path, wanted):
    """English word -> set of Spanish translations (only the words in `wanted`)."""
    out = {}
    if not path or not os.path.exists(path):
        return out
    with open(path, encoding="utf-8") as f:
        for line in f:
            c = line.rstrip("\n").split("\t")
            if len(c) < 4:
                continue
            w = c[0].lower()
            if w in wanted:
                out.setdefault(w, set()).update(words(c[3]))
    return out


def citation(row, lang):
    """The citation form fed to the engine: the head; a Latin verb head keeps its macrons."""
    h = row["head"].strip()
    return h.split(",")[0].split(" ")[0] if lang == "grc" else h


def content_of(output, lang_out):
    stop = STOP_EN if lang_out == "en" else STOP_ES
    return [w for w in words(output) if w not in stop]


def def_words(defn):
    # the definition's words, parentheses opened ("(+abl.)" adds nothing that matters)
    return words(re.sub(r"[()\[\]]", " ", defn))


GREEK = re.compile("[Ͱ-Ͽἀ-῿]")


def verdict(output, word, defn_words, lang_out, source_text):
    """output: the readable sentence; word: the lexical word of the lemma (the word-by-word line)."""
    out = output.strip()
    if not out or not word.strip():
        return "missing", []
    both = out + " " + word
    if GREEK.search(both) or re.search(r"\[[^\]]+\]", both):
        return "leak", []
    cw = content_of(word, lang_out)
    if not cw:
        return "missing", []
    src = fold(source_text)
    if lang_out in ("en", "es") and any(w == src for w in cw) and src not in defn_words:
        return "leak", cw
    if len(cw) > 3:
        return "long", cw
    keys = keys_en if lang_out == "en" else keys_es
    dk = keys(defn_words)
    head_hit = bool(keys([cw[0]]) & dk) or bool(keys([cw[-1]]) & dk)
    if head_hit:
        return "ok", cw
    if keys(cw) & dk:
        return "weak", cw
    return "nosense", cw


def run_engine(exe, lexdir, curated, nlp, pair, units, timeout=1800):
    data = tempfile.mkdtemp(prefix="vp-c41-")
    env = {}
    if curated:
        env["VP_CURATED_DIR"] = curated
    if nlp:
        env["VP_NLP_DIR"] = nlp
    try:
        with L.Engine(exe, data, lexdir, env=env, timeout=timeout) as eng:
            eng.request("settings.set", {"patch": {"speakerGender": "m", "defaultFidelity": 2, "engines.model": False,
                                                   "engines.online": False, "latinity": "wide"}})
            eng.request("project.new", {"kind": "text", "pair": pair, "text": "\n\n".join(units) + "\n"})
            res = L.translate(eng, {"rules": True, "model": False, "online": False}, 2, timeout=timeout)
            cues = res["cues"]
            out = []
            for i in range(len(cues)):
                d = eng.request("cue.get", {"index": i})
                wbw = ""
                for a in d.get("alternatives", []):
                    if a.get("reason") == "word by word":
                        wbw = a.get("text", "")
                lemmas = [r["data"].get("lemmaId") for r in d.get("reasons", [])
                          if r.get("kind") == "analysis" and isinstance(r.get("data"), dict)]
                out.append({"target": cues[i].get("target", ""), "conf": cues[i].get("confidence", "?"),
                            "flags": cues[i].get("flags", []), "wbw": wbw, "lemmas": lemmas})
            return out
    finally:
        shutil.rmtree(data, ignore_errors=True)


def audit(rows, outputs_by_lang, es_defs, en_es, lang="la"):
    """rows: core rows (content parts of speech); outputs_by_lang: {"en": [(target, conf, flags)], "es": [...]}.
    Returns a list of result dicts (one per row) in rank order."""
    results = []
    for i, r in enumerate(rows):
        res = {"head": r["head"], "rank": r["rank"], "pos": r["pos"]}
        dw_en = def_words(r["def"])
        for lang_out, outs in sorted(outputs_by_lang.items()):
            o = outs[i]
            if lang_out == "en":
                dw = dw_en
            else:
                if es_defs is not None:
                    dw = def_words(es_defs.get(r["key"], ""))
                else:
                    dw = []
                    for w in dw_en:
                        dw.extend(sorted(en_es.get(w, ())))
            v, cw = verdict(o["target"], o["wbw"], set(dw), lang_out, citation(r, lang))
            other = bool(r["ids"]) and bool(o["lemmas"]) and not (set(o["lemmas"]) & set(r["ids"]))
            res[lang_out] = {"verdict": v, "out": o["target"], "word": o["wbw"], "conf": o["conf"],
                             "flags": ",".join(o["flags"]), "other": other}
        results.append(res)
    return results


def summary(results, langs):
    lines = []
    for lang_out in langs:
        counts = {}
        okw = 0
        for r in results:
            v = r[lang_out]["verdict"]
            counts[v] = counts.get(v, 0) + 1
            if v != "ok" and r[lang_out]["conf"] == "ok":
                okw += 1
        parts = " ".join("%s %d" % (k, counts[k]) for k in sorted(counts))
        lines.append("%s: %d lemmas; %s; flagged but rated OK %d" % (lang_out, len(results), parts, okw))
    return lines


def write_report(path, results, langs, header):
    with open(path, "w", encoding="utf-8") as f:
        f.write("# " + header + "\n")
        for line in summary(results, langs):
            f.write("# " + line + "\n")
        f.write("# rank\thead\tpos\t" + "\t".join("%s_verdict\t%s_lemma\t%s_conf\t%s_word\t%s_output\t%s_flags"
                                                  % (l, l, l, l, l, l) for l in langs) + "\n")
        for r in results:
            cols = [str(r["rank"]), r["head"], r["pos"]]
            for l in langs:
                x = r[l]
                cols += [x["verdict"], "other" if x["other"] else "core", x["conf"], x["word"],
                         x["out"].replace("\n", " / "), x["flags"]]
            f.write("\t".join(cols) + "\n")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--exe", required=True)
    ap.add_argument("--lang", choices=("la", "grc"), required=True)
    ap.add_argument("--dcc", required=True)
    ap.add_argument("--dcc-es")
    ap.add_argument("--en-es")
    ap.add_argument("--lexicons", default=None)
    ap.add_argument("--curated", default=None)
    ap.add_argument("--nlp", default=None)
    ap.add_argument("--sentences", default=None, help="own audit sentences (one per line), run after the lemmas")
    ap.add_argument("--out", required=True)
    ap.add_argument("--all-pos", action="store_true", help="also function words (pronouns, prepositions ...)")
    a = ap.parse_args(argv)
    rows = [r for r in read_core(a.dcc) if a.all_pos or r["pos"] in CONTENT_POS]
    rows.sort(key=lambda r: (r["rank"], r["head"]))
    es_defs = None
    if a.dcc_es:
        es_defs = {r["key"]: r["def"] for r in read_core(a.dcc_es)}
    wanted = set()
    for r in rows:
        wanted.update(def_words(r["def"]))
    en_es = read_en_es(a.en_es, wanted) if es_defs is None else {}
    units = [citation(r, a.lang) for r in rows]
    sents = []
    if a.sentences:
        with open(a.sentences, encoding="utf-8") as f:
            sents = [l.strip() for l in f if l.strip() and not l.startswith("#")]
    os.makedirs(a.out, exist_ok=True)
    outs = {}
    for lang_out in ("en", "es"):
        got = run_engine(a.exe, a.lexicons, a.curated, a.nlp, "%s-%s" % (a.lang, lang_out), units + sents)
        if len(got) != len(units) + len(sents):
            print("unit count mismatch: %d outputs for %d units" % (len(got), len(units) + len(sents)), file=sys.stderr)
            return 2
        outs[lang_out] = got[: len(units)]
        if sents:
            with open(os.path.join(a.out, "sentences.%s.txt" % lang_out), "w", encoding="utf-8") as f:
                for s, o in zip(sents, got[len(units):]):
                    f.write("%s\t[%s]\t%s\t%s\t{%s}\n" % (s, o["conf"], o["target"].replace("\n", " / "), o["wbw"],
                                                         ",".join(o["flags"])))
    results = audit(rows, outs, es_defs, en_es, a.lang)
    write_report(os.path.join(a.out, "audit.tsv"), results, ("en", "es"),
                 "core audit %s: %s" % (a.lang, os.path.basename(a.dcc)))
    for line in summary(results, ("en", "es")):
        print(line)
    return 0


if __name__ == "__main__":
    sys.exit(main())
