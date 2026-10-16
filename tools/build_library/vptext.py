"""Text normalisation. [CONTRACT, DESIGN.md section 4]

One implementation in Python (this file) and one in C++ (engine/core/include/vp/text.h); both are tested
against tests/fixtures/normalisation_golden.tsv. Keys are compared bytewise on their UTF-8 encoding.
"""
import unicodedata

COMBINING_MACRON = "\u0304"
COMBINING_BREVE = "\u0306"
_LENGTH_MARKS = frozenset((COMBINING_MACRON, COMBINING_BREVE))
# marks removed by greek_bare (after NFD): grave, acute, circumflex, diaeresis, smooth, rough, perispomeni,
# koronis, dialytika tonos, ypogegrammeni
_GREEK_BARE_MARKS = frozenset("\u0300\u0301\u0302\u0308\u0313\u0314\u0342\u0343\u0344\u0345")
_QUOTES = {"\u2018": "'", "\u2019": "'", "\u201a": "'", "\u201b": "'",
           "\u201c": '"', "\u201d": '"', "\u201e": '"', "\u201f": '"'}
_QUOTE_TABLE = str.maketrans(_QUOTES)
_LATIN_FOLD = str.maketrans({"j": "i", "v": "u", "\u00e6": "ae", "\u0153": "oe"})


def nfc(s):
    """Unicode NFC."""
    return unicodedata.normalize("NFC", s)


def _strip_marks(s, marks):
    """NFD, drop the given combining marks, NFC."""
    d = unicodedata.normalize("NFD", s)
    if not any(c in marks for c in d):
        return unicodedata.normalize("NFC", s)
    return unicodedata.normalize("NFC", "".join(c for c in d if c not in marks))


def strip_length_marks(s):
    """Remove macrons and breves (combining or precomposed), keep everything else; result is NFC."""
    return _strip_marks(s, _LENGTH_MARKS)


def latin_key(s):
    """nfc, lower-case, strip macron/breve, j->i, v->u, ae/oe ligatures split, keep only letters, '-' and ' '."""
    s = strip_length_marks(nfc(s).lower())
    s = s.translate(_LATIN_FOLD)
    return "".join(c for c in s if c == "-" or c == " " or unicodedata.category(c).startswith("L"))


def greek_key(s):
    """nfc, lower-case, final sigma to medial sigma, strip length marks (U+0304, U+0306). Accents kept."""
    s = strip_length_marks(nfc(s).lower())
    return s.replace("\u03c2", "\u03c3")


def greek_bare(s):
    """greek_key without accents, breathings, diaeresis and iota subscript."""
    return _strip_marks(greek_key(s), _GREEK_BARE_MARKS)


def en_key(s):
    """nfc, lower-case, curly quotes to straight quotes."""
    return nfc(s).lower().translate(_QUOTE_TABLE)


def es_key(s):
    """Same rule as en_key."""
    return en_key(s)


def es_bare(s):
    """es_key without combining marks U+0300-U+036F (so n-tilde becomes n)."""
    d = unicodedata.normalize("NFD", es_key(s))
    return unicodedata.normalize("NFC", "".join(c for c in d if not ("\u0300" <= c <= "\u036f")))


def display_latin(form, macrons):
    """The macronised form (NFC) when macrons is true, else the same form without length marks."""
    return nfc(form) if macrons else strip_length_marks(form)


KEY_FUNCS = {"la": latin_key, "grc": greek_key, "en": en_key, "es": es_key}


def key_for(lang):
    return KEY_FUNCS[lang]
