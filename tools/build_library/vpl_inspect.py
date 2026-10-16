#!/usr/bin/env python3
"""Dev tool: print a .vpl file's header and sections, look up a word, show its analyses, lemmas, senses and cells,
and the reverse-index candidates of a keyword. Uses vpl_reader.py (independent of pack.py).

  python3 tools/build_library/vpl_inspect.py data/work/latin.vpl                 # header + section sizes
  python3 tools/build_library/vpl_inspect.py data/work/latin.vpl puellae         # analyses of a form (key derived)
  python3 tools/build_library/vpl_inspect.py data/work/latin.vpl --lemma 983     # one lemma record
  python3 tools/build_library/vpl_inspect.py data/work/latin.vpl --reverse water # candidates (es: prefix for ES)
  python3 tools/build_library/vpl_inspect.py data/work/latin.vpl --validate      # full structural walk
"""
import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import tagmap  # noqa: E402
import vpl_reader  # noqa: E402
import vptext  # noqa: E402

FLAG_NAMES = ["table", "formpage", "whitaker", "alternative", "non-attic", "late", "poetic/rare", "enclitic"]
LFLAG_NAMES = ["name", "indeclinable", "deponent", "impersonal", "shared_el", "plural-only", "defective", "has_table",
               "gloss_es-via-English"]
TAG_NAMES = ["transitive", "intransitive", "figurative", "rare", "archaic", "poetic", "Medieval", "New Latin",
             "with-dat", "with-abl", "with-gen", "with-acc", "with-inf", "impersonal", "reflexive"]


def bits(v, names):
    return ",".join(n for i, n in enumerate(names) if v & (1 << i)) or "-"


def show_lemma(r, i, cells=True, w=sys.stdout):
    l = r.lemma(i)
    w.write("lemma %d %s [key %s] pos %s cls %d gender %d tier %d (source %d) freq_rank %d whit %s flags %s\n" % (
        i, l["head"], l["key"], tagmap.describe(l["pos"]) or l["pos"], l["cls"], l["gender"], l["tier"],
        l["tier_source"], l["freq_rank"], chr(l["whit_freq"]) if l["whit_freq"] else "-", bits(l["flags"], LFLAG_NAMES)))
    w.write("  gloss: %s | %s %s\n" % (l["gloss_en"], l["gloss_es"], l["emoji"]))
    if l["principal"]:
        w.write("  principal (%d): %s\n" % (l["principal_off_count"], l["principal"]))
    for k, s in enumerate(r.senses(i)):
        w.write("  sense %d (rank %d, tags %s): %s | %s  [%s]\n" % (k, s["rank"], bits(s["tags"], TAG_NAMES),
                                                                 s["gloss_en"], s["gloss_es"], s["keywords"]))
    if cells:
        cs = r.cells(i)
        w.write("  %d cells\n" % len(cs))
        for packed, form, fid, _ in cs[:60]:
            w.write("    %-50s %s\n" % (tagmap.describe(packed), form))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("file")
    ap.add_argument("word", nargs="?")
    ap.add_argument("--lemma", type=int)
    ap.add_argument("--reverse")
    ap.add_argument("--validate", action="store_true")
    ap.add_argument("--sha", action="store_true", help="verify the header SHA-256")
    a = ap.parse_args(argv)
    r = vpl_reader.VplReader(a.file, check_sha=a.sha)
    w = sys.stdout
    w.write("%s: lang %s v%d.%d, %d bytes, sha256 %s%s\n" % (a.file, r.lang, r.major, r.minor, r.file_size,
                                                          r.sha256.hex(), " (verified)" if a.sha else ""))
    w.write("keys %d, analyses %d, lemmas %d, senses %d, features %d, cells %d, keywords %d, candidates %d\n" % (
        r.n_keys, r.n_anal, r.n_lemmas, r.n_senses, r.n_feat, r.n_cells, r.n_kw, r.n_cand))
    for tag, off, ln in r.sections:
        w.write("  %s offset %10d length %10d\n" % (tag, off, ln))
    if a.validate:
        r.validate()
        w.write("validate: OK\n")
    if a.word:
        key = vptext.key_for(r.lang)(a.word)
        an = r.lookup(key)
        w.write("word %r key %r: %d analyses\n" % (a.word, key, len(an)))
        seen = []
        for x in an:
            l = r.lemma(x["lemma"])
            w.write("  %s <- %s [%d] %s (%s)\n" % (x["display"], l["head"], x["lemma"], tagmap.describe(x["feat"]),
                                                  bits(x["flags"], FLAG_NAMES)))
            if x["lemma"] not in seen:
                seen.append(x["lemma"])
        for i in seen:
            show_lemma(r, i)
    if a.lemma is not None:
        show_lemma(r, a.lemma)
    if a.reverse:
        cs = r.reverse(a.reverse)
        w.write("reverse %r: %d candidates\n" % (a.reverse, len(cs)))
        for c in cs[:25]:
            l = r.lemma(c["lemma"])
            w.write("  %3d %s [%d] sense %d pos %d  %s\n" % (c["score"], l["head"], c["lemma"], c["sense"], c["pos"],
                                                          l["gloss_en"]))
    r.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
