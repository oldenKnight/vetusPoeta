// Rule tokeniser for English and Spanish (see vp/nlp.h for the contract). Works on code points, keeps byte offsets.
#include <cstring>

#include "vp/nlp.h"
#include "vp/text.h"

namespace vp::nlp {
namespace {

enum class Cls { Space, Word, Punct, Symbol };

bool isAsciiAlnum(char32_t c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); }
bool isDigit(char32_t c) { return c >= '0' && c <= '9'; }

Cls classify(char32_t c) {
  if (c < 0x80) {
    if (c == ' ' || (c >= 0x09 && c <= 0x0D)) return Cls::Space;
    if (c < 0x20 || c == 0x7F) return Cls::Space;
    if (isAsciiAlnum(c) || c == '_') return Cls::Word;
    if (std::strchr("!\"'(),-./:;?[]{}`", static_cast<int>(c)) != nullptr) return Cls::Punct;
    return Cls::Symbol;  // # $ % & * + < = > @ \ ^ | ~
  }
  if (c == 0x85 || c == 0xA0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200B) || c == 0x2028 || c == 0x2029 ||
      c == 0x202F || c == 0x205F || c == 0x3000 || c == 0xFEFF)
    return Cls::Space;
  if (c < 0x100) {
    if (c < 0xA0) return Cls::Space;  // C1 controls
    if (c == 0xA1 || c == 0xAB || c == 0xBB || c == 0xBF || c == 0xB7) return Cls::Punct;
    if ((c >= 0xA2 && c <= 0xA9) || (c >= 0xAC && c <= 0xB1) || c == 0xB4 || c == 0xB6 || c == 0xB8 || c == 0xD7 ||
        c == 0xF7)
      return Cls::Symbol;
    return Cls::Word;
  }
  if (c >= 0x2010 && c <= 0x205E) return Cls::Punct;
  if (c >= 0x20A0 && c <= 0x20CF) return Cls::Symbol;
  if (c >= 0x2100 && c <= 0x214F) return Cls::Symbol;
  if (c >= 0x2190 && c <= 0x2BFF) return Cls::Symbol;
  if (c >= 0x3001 && c <= 0x303F) return Cls::Punct;
  if (c >= 0x1F000 && c <= 0x1FAFF) return Cls::Symbol;
  return Cls::Word;
}

// Code points that continue an emoji / symbol cluster.
bool isClusterExtender(char32_t c) {
  return c == 0x200D || c == 0xFE0E || c == 0xFE0F || (c >= 0x1F3FB && c <= 0x1F3FF) || (c >= 0xE0020 && c <= 0xE007F) ||
         (c >= 0x20D0 && c <= 0x20FF);
}

bool isApostrophe(char32_t c) { return c == '\'' || c == 0x2019; }
bool isHyphen(char32_t c) { return c == '-' || c == 0x2010 || c == 0x2011; }

struct Cp {
  char32_t c;
  size_t off;   // byte offset
};

const char* const kLeadApos[] = {"em", "cause", "cos", "coz", "tis", "twas", "til", "bout", "round"};
const char* const kAbbrevEn[] = {"mr", "mrs", "ms", "dr", "prof", "jr", "sr", "st", "vs", "etc", "inc", "ltd"};
const char* const kAbbrevEs[] = {"sr", "sra", "srta", "dr", "dra", "ud", "uds", "vd", "vds", "etc", "pág", "núm", "lic",
                                 "ing", "dña"};

class Scanner {
 public:
  Scanner(std::string_view text, Lang lang) : text_(text), lang_(lang) {
    size_t i = 0;
    while (i < text.size()) {
      const size_t off = i;
      const char32_t c = vp::text::decodeUtf8(text, i);
      cps_.push_back({c, off});
    }
  }

  void run(std::vector<Token>& out) {
    const size_t n = cps_.size();
    size_t i = 0;
    while (i < n) {
      const char32_t c = cps_[i].c;
      const Cls cls = classify(c);
      if (cls == Cls::Space) {
        ++i;
        continue;
      }
      if (cls == Cls::Word) {
        i = word(i, i, out);
        continue;
      }
      if (cls == Cls::Symbol) {
        if ((c == '#' || c == '@') && i + 1 < n && isWord(i + 1) && (i == 0 || !isWord(i - 1))) {
          i = word(i, i + 1, out);
          continue;
        }
        size_t j = i + 1;
        if (c >= 0x80) {
          while (j < n && (isClusterExtender(cps_[j].c) ||
                           (cps_[j - 1].c == 0x200D && classify(cps_[j].c) == Cls::Symbol)))
            ++j;
        }
        emit(i, j, out);
        i = j;
        continue;
      }
      // punctuation
      if (c == '-' && i + 1 < n && isDigit(cps_[i + 1].c) && (i == 0 || classify(cps_[i - 1].c) == Cls::Space)) {
        i = word(i, i + 1, out);  // negative number
        continue;
      }
      if (isApostrophe(c) && lang_ == Lang::En && (i == 0 || !isWord(i - 1))) {
        const size_t e = letters(i + 1);
        if (e > i + 1 && (e == n || !isWord(e)) && inList(i + 1, e, kLeadApos, sizeof(kLeadApos) / sizeof(*kLeadApos))) {
          emit(i, e, out);
          i = e;
          continue;
        }
      }
      size_t j = i + 1;
      if (c == '.' || c == '-' || c == '`' || c == '\'') {
        while (j < n && cps_[j].c == c) ++j;           // "...", "--", "``", "''"
      } else if (c == '!' || c == '?') {
        while (j < n && (cps_[j].c == '!' || cps_[j].c == '?')) ++j;
      }
      emit(i, j, out);
      i = j;
    }
  }

 private:
  bool isWord(size_t k) const { return classify(cps_[k].c) == Cls::Word; }
  bool isLetter(size_t k) const { return isWord(k) && !isDigit(cps_[k].c); }

  size_t letters(size_t k) const {
    while (k < cps_.size() && isLetter(k)) ++k;
    return k;
  }

  // Lower-cased ASCII/UTF-8 text of [a, b) compared with a list of lower-case words.
  bool inList(size_t a, size_t b, const char* const* list, size_t count) const {
    const std::string low = vp::text::lower(slice(a, b));
    for (size_t k = 0; k < count; ++k)
      if (low == list[k]) return true;
    return false;
  }

  std::string_view slice(size_t a, size_t b) const {
    const size_t s = cps_[a].off;
    const size_t e = b < cps_.size() ? cps_[b].off : text_.size();
    return text_.substr(s, e - s);
  }

  // Single letters separated by dots: "U.S", "e.g", "a.m".
  bool dottedInitials(size_t a, size_t b) const {
    if (b - a < 3) return false;
    for (size_t k = a; k < b; ++k) {
      const bool letterPos = ((k - a) % 2) == 0;
      if (letterPos ? !isLetter(k) : cps_[k].c != '.') return false;
    }
    return ((b - a) % 2) == 1;
  }

  size_t word(size_t start, size_t j, std::vector<Token>& out) {
    const size_t n = cps_.size();
    for (;;) {
      while (j < n && isWord(j)) ++j;
      if (j + 3 < n && cps_[j].c == ':' && cps_[j + 1].c == '/' && cps_[j + 2].c == '/' && j > start) {
        j += 3;  // URL: up to the next space, trailing punctuation split off below
        while (j < n && classify(cps_[j].c) != Cls::Space) ++j;
        while (j > start + 1 && std::strchr(".,;:!?)]}\"'", static_cast<int>(cps_[j - 1].c < 0x80 ? cps_[j - 1].c : 1)) != nullptr) --j;
        emit(start, j, out);
        return j;
      }
      if (j + 1 < n && j > start && isWord(j + 1)) {
        const char32_t c = cps_[j].c;
        const bool prevDigit = isDigit(cps_[j - 1].c), nextDigit = isDigit(cps_[j + 1].c);
        if (isHyphen(c) || c == '.' || c == '/' || c == '&' || c == '@' ||
            (isApostrophe(c) && isLetter(j - 1) && isLetter(j + 1)) ||
            ((c == ',' || c == ':') && prevDigit && nextDigit)) {
          ++j;
          continue;
        }
      }
      break;
    }
    if (j < n && cps_[j].c == '.' && !(j + 1 < n && cps_[j + 1].c == '.')) {
      const bool abbrev = dottedInitials(start, j) ||
                          (lang_ == Lang::En ? inList(start, j, kAbbrevEn, sizeof(kAbbrevEn) / sizeof(*kAbbrevEn))
                                             : inList(start, j, kAbbrevEs, sizeof(kAbbrevEs) / sizeof(*kAbbrevEs)));
      if (abbrev) ++j;
    } else if (j < n && lang_ == Lang::En && isApostrophe(cps_[j].c) && (j + 1 == n || !isWord(j + 1)) && j >= start + 3) {
      const std::string low = vp::text::lower(slice(j - 2, j));
      if (low == "in") ++j;  // goin', nothin'
    }
    emit(start, j, out);
    return j;
  }

  void emit(size_t a, size_t b, std::vector<Token>& out) {
    Token t;
    t.start = static_cast<int>(cps_[a].off);
    t.end = static_cast<int>(b < cps_.size() ? cps_[b].off : text_.size());
    t.text.assign(text_.substr(static_cast<size_t>(t.start), static_cast<size_t>(t.end - t.start)));
    t.lower = normalise(t.text);
    out.push_back(std::move(t));
  }

  std::string_view text_;
  Lang lang_;
  std::vector<Cp> cps_;
};

}  // namespace

void Tokenizer::tokenize(std::string_view text, std::vector<Token>& out) const {
  out.clear();
  Scanner(text, lang_).run(out);
}

std::vector<Token> Tokenizer::tokenize(std::string_view text) const {
  std::vector<Token> out;
  tokenize(text, out);
  return out;
}

}  // namespace vp::nlp
