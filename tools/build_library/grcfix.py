"""Greek display clean-up applied by the kaikki stage (B4b, docs/rules_grc_notes.md "Lexicon gaps").

  final_sigma(s)        word-final sigma written as a medial sigma (U+03C3) becomes U+03C2 (ἦσ -> ἦς); a sigma
                        before an apostrophe is left alone (elision keeps the medial letter)
  strip_article(form)   "τῆς ἀνθρώπου" -> "ἀνθρώπου": a leading article (any dialect, any accent) in front of a
                        single word is dropped; anything else is returned unchanged
  latin_script(form)    True when a cell is a romanisation that leaked into a Greek table ("taîs kŭsĭ́(n)")
  dialect_guess(rows)   "Epic" / "Ionic" for a table without a dialect marker whose genitive singular ends in -οιο
                        or whose dative plural ends in -ῃσι(ν) / -ῃς; "" otherwise
Nothing here changes letters other than the final sigma; keys (greek_key) are unaffected by that fix.
"""
import re
import unicodedata

SIGMA, FINAL_SIGMA = "σ", "ς"
# a sigma followed by the end of the string, a space or punctuation (but not an apostrophe / koronis)
_FINAL_RE = re.compile("σ(?=$|[\\s,.;:··!?()\\[\\]—–\"»«/-])")
# accents, breathings, diaeresis, length marks, koronis: removed to compare article shapes (iota subscript kept)
_MARKS = frozenset("̀́̂̄̆̈̓̔͂̓̈́")
_ARTICLES = ("""ο η το του της τω τῳ τη τῃ τον την τω οι αι τα των τοις ταις τους τας τοιν ταιν
τᾳ τᾶς τᾱς τα ταν τοισι τοισιν τῃσι τῃσιν τῃς ταισι ταισιν τηισι τηισιν τηις τοιο""")
_LATIN = re.compile(r"[A-Za-zÀ-ɏḀ-ỿ]")


def plain(s):
    """NFD, drop accents / breathings / length marks (iota subscript kept), lower-case, NFC."""
    d = unicodedata.normalize("NFD", s.lower())
    return unicodedata.normalize("NFC", "".join(ch for ch in d if ch not in _MARKS)).replace(FINAL_SIGMA, SIGMA)


ARTICLE_SHAPES = frozenset(plain(x) for x in _ARTICLES.split())


def final_sigma(s):
    if SIGMA not in s:
        return s
    return _FINAL_RE.sub(FINAL_SIGMA, s)


def is_article(tok):
    tok = tok.strip(",")
    t = plain(tok)
    for part, raw in zip(t.split("/"), tok.split("/")):
        if part and part not in ARTICLE_SHAPES:
            return False
        # ὁ ἡ οἱ αἱ carry a rough breathing (ἦ "I was" does not)
        if part[:1] in "\u03bf\u03b7\u03b1" and "\u0314" not in unicodedata.normalize("NFD", raw):
            return False
    return bool(t)


def strip_article(form):
    """(form without a leading article, True) or (form, False). Only 'article + one word' is stripped."""
    toks = form.split()
    if len(toks) == 2 and is_article(toks[0]):
        return toks[1], True
    return form, False


def latin_script(form):
    return bool(_LATIN.search(unicodedata.normalize("NFD", form)))


def dialect_guess(rows):
    """rows: [(form, tags list)] of one table (same marker). Epic -οιο genitive singular; Ionic/Epic -ῃσι(ν) / -ῃς
    dative plural (written with the iota subscript)."""
    for form, tags in rows:
        p = plain(form.split()[-1]) if form.strip() else ""
        ts = set(tags)
        if "genitive" in ts and "singular" in ts and p.endswith("οιο"):
            return "Epic"
        if "dative" in ts and "plural" in ts and re.search("ῃ(σιν?|σ)$", p):
            return "Ionic"
    return ""


# ---- compound verbs from a simplex table (lexicon_overrides_grc.tsv "@from" rows) ------------------------------------
_VOWELS = frozenset("αεηιουω")
_DIPH = frozenset(("αι", "ει", "οι", "υι", "αυ", "ευ",
                   "ου", "ηυ", "ωυ"))
_ASPIRATE = {"π": "φ", "τ": "θ", "κ": "χ"}
NO_ELISION = frozenset(("περι", "προ"))   # περι, προ keep their vowel
ACUTE, SMOOTH, ROUGH = "́", "̓", "̔"


def _nuclei(nfd):
    """Vowel nuclei of an NFD string: list of (start index, end index) of each syllable nucleus."""
    base = [(i, ch) for i, ch in enumerate(nfd) if not unicodedata.combining(ch)]
    out = []
    j = 0
    while j < len(base):
        i, ch = base[j]
        if ch.lower() in _VOWELS:
            if j + 1 < len(base) and (ch + base[j + 1][1]).lower() in _DIPH:
                nxt = base[j + 1][0]
                diaeresis = any(nfd[k] == "̈" for k in range(nxt + 1, base[j + 2][0] if j + 2 < len(base)
                                                                    else len(nfd)))
                if not diaeresis:
                    end = base[j + 2][0] if j + 2 < len(base) else len(nfd)
                    out.append((i, end))
                    j += 2
                    continue
            end = base[j + 1][0] if j + 1 < len(base) else len(nfd)
            out.append((i, end))
        j += 1
    return out


def compound(prefix, form, finite=True):
    """Prefix + a simplex form: ἀπο + ἔφυγον -> ἀπέφυγον, ἀπο + φύγε -> ἀπόφυγε, ἀπο + φυγεῖν -> ἀποφυγεῖν.
    The prefix's final vowel is elided before a vowel (not περι / προ), its final consonant aspirated before a rough
    breathing; the breathing of the simplex goes. Finite forms keep the simplex accent, except a disyllable with the
    acute on its first syllable and a short final ε / ο (φύγε): the recessive accent moves onto the prefix.
    Multi-word cells are returned unchanged with False."""
    if " " in form.strip():
        return form, False
    p = unicodedata.normalize("NFD", prefix)
    f = unicodedata.normalize("NFD", form)
    first_base = next((k for k, ch in enumerate(f) if not unicodedata.combining(ch)), None)
    if first_base is None:
        return form, False
    marks_end = first_base + 1
    while marks_end < len(f) and unicodedata.combining(f[marks_end]):
        marks_end += 1
    starts_vowel = f[first_base].lower() in _VOWELS
    # the breathing may sit on the second vowel of an initial diphthong (εὐ-, οἰ-)
    rough = ROUGH in f[:marks_end + 3]
    f = f[:marks_end + 3].replace(SMOOTH, "").replace(ROUGH, "") + f[marks_end + 3:]
    pb = unicodedata.normalize("NFC", p)
    if starts_vowel and pb not in NO_ELISION and pb[-1:] in _VOWELS:
        p = p[:-1]
        if rough and p[-1:] in _ASPIRATE:
            p = p[:-1] + _ASPIRATE[p[-1]]
    nuc = _nuclei(f)
    if finite and len(nuc) == 2 and ACUTE in f[nuc[0][0]:nuc[0][1]] and not (starts_vowel and p != prefix):
        last = f[nuc[1][0]:nuc[1][1]]
        lb = "".join(ch for ch in last if not unicodedata.combining(ch)).lower()
        if lb in ("ε", "ο") and ACUTE not in last and "͂" not in last:
            a, b = nuc[0]
            f = f[:a] + f[a:b].replace(ACUTE, "", 1) + f[b:]
            pn = _nuclei(p)
            if pn:
                a, b = pn[-1]
                p = p[:b] + ACUTE + p[b:]
    return unicodedata.normalize("NFC", p + f), True
