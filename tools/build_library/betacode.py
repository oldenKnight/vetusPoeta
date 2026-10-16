"""Beta Code (Perseus / TLG convention, as used by the Perseus LSJ XML) -> Unicode Greek (NFC).

Rules implemented (tested by tests/test_betacode.py on a hand-written golden table):
  letters   a b g d e z h q i k l m n c o p r s t u f x y w v  ->  α β γ δ ε ζ η θ ι κ λ μ ν ξ ο π ρ σ τ υ φ χ ψ ω ϝ
            upper-case ASCII letters are read like lower-case ones (TLG files write Greek in capitals);
  capitals  `*` before the letter; the letter's diacritics may stand between `*` and the letter (`*)/a` = Ἄ)
            or after it (`*a)/`);
  marks     `)` smooth U+0313, `(` rough U+0314, `/` acute U+0301, `\\` grave U+0300, `=` circumflex U+0342,
            `+` diaeresis U+0308, `|` iota subscript U+0345, `_` macron U+0304, `^` breve U+0306;
            marks are put in Unicode canonical order (length, diaeresis, breathing, accent, iota) and composed;
  sigma     `s` is final ς at the end of a word (next character not a letter or mark), σ otherwise;
            `s1` = σ, `s2` = ς, `s3` = ϲ (lunate), `j` = ς (final sigma in some files);
  punctuation  `:` -> · (ano teleia), `'` -> ’ (elision), `;` -> ; (Greek question mark is ; after NFC),
            `-` and everything else pass through unchanged. Homograph digits are the caller's business
            (strip_key_digits).
"""
import re
import unicodedata

LETTERS = {"a": "α", "b": "β", "g": "γ", "d": "δ", "e": "ε", "z": "ζ",
           "h": "η", "q": "θ", "i": "ι", "k": "κ", "l": "λ", "m": "μ",
           "n": "ν", "c": "ξ", "o": "ο", "p": "π", "r": "ρ", "s": "σ",
           "t": "τ", "u": "υ", "f": "φ", "x": "χ", "y": "ψ", "w": "ω",
           "v": "ϝ"}
MARKS = {")": "̓", "(": "̔", "/": "́", "\\": "̀", "=": "͂", "+": "̈",
         "|": "ͅ", "_": "̄", "^": "̆"}
# canonical position of each mark: length marks, diaeresis, breathing, accent, iota subscript
MARK_ORDER = {"̄": 0, "̆": 0, "̈": 1, "̓": 2, "̔": 2, "́": 3, "̀": 3,
              "͂": 3, "ͅ": 4}
PUNCT = {":": "·", "'": "’"}
FINAL_SIGMA = "ς"
LUNATE_SIGMA = "ϲ"


def _is_letter(ch):
    return ch.lower() in LETTERS or ch == "*"


def beta_to_unicode(s):
    out = []
    i = 0
    n = len(s)
    while i < n:
        ch = s[i]
        if ch == "*":
            # capital: marks may come before the letter
            j = i + 1
            pre = []
            while j < n and s[j] in MARKS:
                pre.append(MARKS[s[j]])
                j += 1
            if j < n and s[j].lower() in LETTERS:
                base = LETTERS[s[j].lower()].upper()
                j += 1
                post = []
                while j < n and s[j] in MARKS:
                    post.append(MARKS[s[j]])
                    j += 1
                out.append(base + "".join(sorted(pre + post, key=lambda m: MARK_ORDER[m])))
                i = j
                continue
            out.append("".join(pre))  # a stray `*`: keep its marks, drop the asterisk
            i = j
            continue
        lo = ch.lower()
        if lo in LETTERS:
            j = i + 1
            if lo == "s" and j < n and s[j] in "123":
                base = {"1": "σ", "2": FINAL_SIGMA, "3": LUNATE_SIGMA}[s[j]]
                j += 1
            else:
                base = LETTERS[lo]
            marks = []
            while j < n and s[j] in MARKS:
                marks.append(MARKS[s[j]])
                j += 1
            if lo == "s" and base == "σ" and not (i + 1 < n and s[i + 1] in "123"):
                nxt = s[j] if j < n else ""
                if not (nxt and _is_letter(nxt)):
                    base = FINAL_SIGMA
            out.append(base + "".join(sorted(marks, key=lambda m: MARK_ORDER[m])))
            i = j
            continue
        if ch == "j":
            out.append(FINAL_SIGMA)
        elif ch in MARKS:
            out.append(MARKS[ch])  # a mark with no letter (rare): keep it as a combining character
        else:
            out.append(PUNCT.get(ch, ch))
        i += 1
    return unicodedata.normalize("NFC", "".join(out))


_DIGITS = re.compile(r"\d+$")


def strip_key_digits(key):
    """LSJ / LS `key` attributes carry homograph digits at the end ("a)/gw1"); returns (key, digits)."""
    m = _DIGITS.search(key or "")
    if not m:
        return key or "", ""
    return key[:m.start()], m.group(0)
