// UTF-8 helpers and the text normalisation contract (DESIGN.md section 4). [CONTRACT]
// The Python twin is tools/build_library/vptext.py; both are tested against tests/fixtures/normalisation_golden.tsv.
// Keys are compared bytewise. Nothing here throws (allocation failure aside) and nothing here allocates once the
// caller-provided output strings and the per-thread scratch buffers have grown to the working size.
#pragma once
#include <cstddef>
#include <string>
#include <string_view>

namespace vp {
namespace text {

// ---- UTF-8 ------------------------------------------------------------------------------------------------------
constexpr char32_t kReplacement = 0xFFFD;
// Decodes one code point at s[i] and advances i. Malformed, overlong, surrogate or truncated sequences decode as
// U+FFFD and advance by one byte. Precondition: i < s.size().
char32_t decodeUtf8(std::string_view s, size_t& i);
// Appends the UTF-8 encoding of cp (invalid code points are written as U+FFFD).
void appendUtf8(std::string& out, char32_t cp);
bool isValidUtf8(std::string_view s);
std::u32string toUtf32(std::string_view s);
std::string toUtf8(std::u32string_view s);

// ---- normalisation (section 4) ----------------------------------------------------------------------------------
// nfc / nfd: Unicode NFC / NFD for Latin-1 Supplement, Latin Extended-A/B, Latin Extended Additional, Greek and
// Coptic, Greek Extended and combining marks U+0300-036F (canonical ordering of those marks included). Every other
// code point passes through unchanged and acts as a starter.
std::string nfc(std::string_view s);
void nfc(std::string_view s, std::string& out);
std::string nfd(std::string_view s);
void nfd(std::string_view s, std::string& out);

// Full Unicode lower case (Python str.lower semantics: one-to-many mappings such as U+0130, and capital sigma
// becomes final sigma at the end of a word by the Unicode Final_Sigma rule). No normalisation.
std::string lower(std::string_view s);
void lower(std::string_view s, std::string& out);

// nfc, lower, strip macron/breve (U+0304/U+0306, also inside precomposed letters), j->i, v->u, the ligatures
// U+00E6 / U+0153 -> "ae" / "oe", then keep only letters (Unicode alphabetic), '-' and ' '. Latin "Iulius" written
// with a macron on the u -> "iulius".
std::string latin_key(std::string_view s);
void latin_key(std::string_view s, std::string& out);
// nfc, lower, final sigma -> sigma, strip U+0304/U+0306. Accents and breathings are kept.
std::string greek_key(std::string_view s);
void greek_key(std::string_view s, std::string& out);
// greek_key, then strip grave, acute, circumflex, diaeresis, smooth, rough, perispomeni, koronis, dialytika tonos
// and ypogegrammeni (NFD, drop, NFC).
std::string greek_bare(std::string_view s);
void greek_bare(std::string_view s, std::string& out);
// nfc, lower, curly quotes to straight (U+2018 U+2019 U+201A U+201B -> ', U+201C U+201D U+201E U+201F -> ").
std::string en_key(std::string_view s);
void en_key(std::string_view s, std::string& out);
std::string es_key(std::string_view s);
void es_key(std::string_view s, std::string& out);
// es_key, then strip every combining mark U+0300-036F (NFD, drop, NFC): "niño" -> "nino".
std::string es_bare(std::string_view s);
void es_bare(std::string_view s, std::string& out);
// macrons=true: the form in NFC. macrons=false: U+0304/U+0306 removed (also from precomposed letters), result
// in NFC. Case and every other letter are kept.
std::string display_latin(std::string_view form, bool macrons);
void display_latin(std::string_view form, bool macrons, std::string& out);

// Unicode version of the generated tables (from the Python that ran engine/core/tools/gen_nfc_table.py).
const char* unicodeVersion();

}  // namespace text
}  // namespace vp
