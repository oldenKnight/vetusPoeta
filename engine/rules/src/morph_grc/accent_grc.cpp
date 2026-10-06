// Greek text helpers, accent analysis, enclitic rules and the sentence sandhi (vp/morph_grc.h).
#include <algorithm>

#include "vp/morph_grc.h"
#include "vp/text.h"

namespace vp::grc {

namespace {

constexpr char32_t kGrave = 0x0300, kAcute = 0x0301, kCirc = 0x0342, kSmooth = 0x0313, kRough = 0x0314,
                   kIotaSub = 0x0345, kDiaeresis = 0x0308, kMacron = 0x0304, kBreve = 0x0306;

char32_t lowerCp(char32_t c) {
  if (c >= 0x0391 && c <= 0x03A9 && c != 0x03A2) return c + 0x20;
  return c;
}
bool isVowelCp(char32_t c) {
  switch (lowerCp(c)) {
    case U'α': case U'ε': case U'η': case U'ι': case U'ο': case U'υ': case U'ω': return true;
    default: return false;
  }
}
bool isMarkCp(char32_t c) { return c >= 0x0300 && c <= 0x036F; }
bool isAccentCp(char32_t c) { return c == kGrave || c == kAcute || c == kCirc; }
bool isGreekLetterCp(char32_t c) {
  if (c >= 0x0386 && c <= 0x03FF) return c != 0x0387 && c != 0x03F6;
  if (c >= 0x1F00 && c <= 0x1FFF)   // Greek Extended without the spacing accents
    return !(c == 0x1FBD || (c >= 0x1FBF && c <= 0x1FC1) || (c >= 0x1FCD && c <= 0x1FCF) || (c >= 0x1FDD && c <= 0x1FDF) ||
             (c >= 0x1FED && c <= 0x1FEF) || c >= 0x1FFD);
  return false;
}

struct Nucleus {
  size_t first = 0, last = 0;   // indices of the first / last vowel (NFD code points)
  size_t markEnd = 0;           // one past the marks of the last vowel
  Accent accent = Accent::None;
  size_t accentAt = 0;          // index of the accent mark
};

// Syllable nuclei of a decomposed word. Diphthongs: αι ει οι υι αυ ευ ηυ ου ωυ when the first vowel carries no mark
// and the second no diaeresis.
void nuclei(const std::u32string& u, std::vector<Nucleus>& out) {
  out.clear();
  for (size_t i = 0; i < u.size(); ++i) {
    if (!isVowelCp(u[i])) continue;
    size_t j = i + 1;
    bool diaer = false;
    while (j < u.size() && isMarkCp(u[j])) { diaer = diaer || u[j] == kDiaeresis; ++j; }
    const char32_t c = lowerCp(u[i]);
    bool joined = false;
    if (!out.empty() && (c == U'ι' || c == U'υ') && !diaer) {
      Nucleus& p = out.back();
      const char32_t pc = lowerCp(u[p.last]);
      const bool single = p.first == p.last && p.markEnd == p.last + 1 && p.last + 1 == i;
      if (single && ((c == U'ι' && (pc == U'α' || pc == U'ε' || pc == U'ο' || pc == U'υ')) ||
                     (c == U'υ' && (pc == U'α' || pc == U'ε' || pc == U'η' || pc == U'ο' || pc == U'ω')))) {
        p.last = i;
        p.markEnd = j;
        joined = true;
      }
    }
    if (!joined) {
      Nucleus n;
      n.first = n.last = i;
      n.markEnd = j;
      out.push_back(n);
    }
    i = j - 1;
  }
  for (Nucleus& n : out)
    for (size_t k = n.first; k < n.markEnd; ++k) {
      if (u[k] == kAcute) { n.accent = Accent::Acute; n.accentAt = k; }
      else if (u[k] == kGrave) { n.accent = Accent::Grave; n.accentAt = k; }
      else if (u[k] == kCirc) { n.accent = Accent::Circumflex; n.accentAt = k; }
    }
}

std::u32string decompose(std::string_view s) { return text::toUtf32(text::nfd(s)); }
std::string compose(const std::u32string& u) { return text::nfc(text::toUtf8(u)); }

// Insert an accent mark on the last vowel of nucleus n: after its 230-class marks, before an iota subscript.
void insertAccent(std::u32string& u, const Nucleus& n, char32_t mark) {
  size_t at = n.last + 1;
  while (at < u.size() && isMarkCp(u[at]) && u[at] != kIotaSub) ++at;
  u.insert(u.begin() + (long)at, mark);
}

std::string keyNoAccent(std::string_view w) { return text::greek_key(stripAccents(w)); }

bool inList(std::string_view key, std::initializer_list<const char*> list) {
  for (const char* k : list)
    if (key == k) return true;
  return false;
}

}  // namespace

// ---- text -----------------------------------------------------------------------------------------------------------
std::string stripLength(std::string_view s) { return text::display_latin(s, false); }

std::string finalSigma(std::string_view s) {
  std::u32string u = text::toUtf32(s);
  for (size_t i = 0; i < u.size(); ++i) {
    if (u[i] != U'σ') continue;
    size_t j = i + 1;
    while (j < u.size() && isMarkCp(u[j])) ++j;
    const bool end = j >= u.size() || !(isGreekLetterCp(u[j]) || (u[j] >= 'a' && u[j] <= 'z'));
    if (end) u[i] = U'ς';
  }
  return text::toUtf8(u);
}

std::string display(std::string_view s) { return text::nfc(finalSigma(stripLength(s))); }

std::string bare(std::string_view s) { return text::greek_bare(s); }

bool isGreekWord(std::string_view w) {
  size_t i = 0;
  while (i < w.size())
    if (isGreekLetterCp(text::decodeUtf8(w, i))) return true;
  return false;
}

bool startsWithVowel(std::string_view word) {
  const std::u32string u = decompose(word);
  for (char32_t c : u) {
    if (isMarkCp(c)) continue;
    if (c == 0x2019 || c == '\'' || c == 0x1FBD) continue;
    return isVowelCp(c);
  }
  return false;
}

bool startsWithRough(std::string_view word) {
  const std::u32string u = decompose(word);
  size_t i = 0;
  while (i < u.size() && !isVowelCp(u[i]) && lowerCp(u[i]) != U'ρ') ++i;
  if (i >= u.size()) return false;
  // marks of the first syllable (the breathing sits on the second vowel of an initial diphthong)
  for (size_t k = i; k < u.size() && k < i + 6; ++k) {
    if (u[k] == kRough) return true;
    if (!isMarkCp(u[k]) && k > i && !isVowelCp(u[k])) break;
  }
  return false;
}

std::string cleanCell(std::string_view cell, bool* movableNu) {
  if (movableNu) *movableNu = false;
  std::string s(cell);
  while (!s.empty() && s.front() == ' ') s.erase(0, 1);
  while (!s.empty() && s.back() == ' ') s.pop_back();
  const std::string nuMark = "(ν)";
  if (s.size() > nuMark.size() && s.compare(s.size() - nuMark.size(), nuMark.size(), nuMark) == 0) {
    s.resize(s.size() - nuMark.size());
    if (movableNu) *movableNu = true;
  }
  size_t sp = s.find(' ');
  if (sp != std::string::npos) {
    const std::string first = keyNoAccent(stripLength(s.substr(0, sp)));
    static const char* const kArt[] = {"ὁ", "ἡ", "το", "του", "τησ", "τῳ", "τῃ", "τον", "την", "οἱ", "αἱ", "τα",
                                       "των", "τοισ", "ταισ", "τουσ", "τασ", "τω", "τοιν", "ταν", "τᾱσ"};
    bool art = false;
    const std::string fb = text::greek_bare(first);
    for (const char* a : kArt) art = art || fb == text::greek_bare(a);
    if (!art) return std::string();
    s.erase(0, sp + 1);
    if (s.find(' ') != std::string::npos) return std::string();
  }
  if (s.empty() || s.find('(') != std::string::npos || s.find(')') != std::string::npos ||
      s.find('/') != std::string::npos || s.find(',') != std::string::npos)
    return std::string();
  return display(s);
}

// ---- accents --------------------------------------------------------------------------------------------------------
AccentInfo accentOf(std::string_view word) {
  AccentInfo a;
  thread_local std::vector<Nucleus> ns;
  const std::u32string u = decompose(word);
  nuclei(u, ns);
  a.syllables = (int)ns.size();
  for (size_t i = 0; i < ns.size(); ++i) {
    if (ns[i].accent == Accent::None) continue;
    ++a.accents;
    const int pos = (int)(ns.size() - 1 - i);
    if (a.firstPosition < 0) { a.firstPosition = pos; a.firstType = ns[i].accent; }
    a.position = pos;
    a.type = ns[i].accent;
  }
  return a;
}

std::string addUltimaAcute(std::string_view word) {
  thread_local std::vector<Nucleus> ns;
  std::u32string u = decompose(word);
  nuclei(u, ns);
  if (ns.empty() || ns.back().accent != Accent::None) return std::string(word);
  insertAccent(u, ns.back(), kAcute);
  return compose(u);
}

std::string ultimaToGrave(std::string_view word) {
  thread_local std::vector<Nucleus> ns;
  std::u32string u = decompose(word);
  nuclei(u, ns);
  if (ns.empty() || ns.back().accent != Accent::Acute) return std::string(word);
  u[ns.back().accentAt] = kGrave;
  return compose(u);
}

std::string ultimaToAcute(std::string_view word) {
  thread_local std::vector<Nucleus> ns;
  std::u32string u = decompose(word);
  nuclei(u, ns);
  if (ns.empty() || ns.back().accent != Accent::Grave) return std::string(word);
  u[ns.back().accentAt] = kAcute;
  return compose(u);
}

std::string stripAccents(std::string_view word) {
  std::u32string u = decompose(word);
  u.erase(std::remove_if(u.begin(), u.end(), isAccentCp), u.end());
  return compose(u);
}

std::string encliticAccented(std::string_view word) {
  thread_local std::vector<Nucleus> ns;
  std::u32string u = decompose(stripAccents(word));
  nuclei(u, ns);
  if (ns.size() < 2) return compose(u);
  const bool circ = text::greek_bare(word) == "τινων";
  insertAccent(u, ns.back(), circ ? kCirc : kAcute);
  return compose(u);
}

bool isProclitic(std::string_view word) {
  const std::string k = text::greek_key(word);
  return inList(k, {"ὁ", "ἡ", "οἱ", "αἱ", "ἐν", "εἰσ", "ἐσ", "ἐκ", "ἐξ", "εἰ", "ὡσ", "οὐ", "οὐκ", "οὐχ"});
}

bool isEncliticForm(std::string_view word) {
  const AccentInfo a = accentOf(word);
  if (a.accents > 1) return false;
  if (a.accents == 1) {
    // orthotone enclitics keep the accent on the ultima (τινός, ἐστί, εἰμί, ποτέ); monosyllables with an accent
    // are interrogatives or orthotone pronouns (τίς, τί, σοί)
    if (a.position != 0 || a.syllables < 2 || a.type == Accent::Circumflex) {
      if (!(a.position == 0 && a.type == Accent::Circumflex && text::greek_bare(word) == "τινων")) return false;
    }
  }
  const std::string k = keyNoAccent(word);
  return inList(k, {"μου", "μοι", "με", "σου", "σοι", "σε", "τισ", "τι", "τινοσ", "τινι", "τινα", "τινεσ", "τινων",
                    "τισι", "τισιν", "τινασ", "εἰμι", "ἐστι", "ἐστιν", "ἐσμεν", "ἐστε", "εἰσι", "εἰσιν", "φημι",
                    "φησι", "φησιν", "φαμεν", "φατε", "φασι", "φασιν", "γε", "τε", "που", "ποι", "ποθεν", "ποτε",
                    "πωσ", "πω", "τοι"});
}

bool isPostpositive(std::string_view word) {
  const std::string k = keyNoAccent(word);
  return inList(k, {"ἀν", "γαρ", "γε", "δε", "δη", "μεν", "μην", "οὐν", "τε", "τοι", "τοινυν", "δηπου"});
}

// ---- sandhi ---------------------------------------------------------------------------------------------------------
namespace {

bool sentenceEnd(std::string_view p) {
  for (char c : p)
    if (c == '.' || c == ';' || c == '!' || c == '?') return true;
  return p.find("\xC2\xB7") != std::string_view::npos || p.find("\xCE\x87") != std::string_view::npos ||
         p.find("\xCD\xBE") != std::string_view::npos;   // · (U+00B7, U+0387), ; (U+037E)
}

bool endsWithKey(const std::string& keyBare, const char* tail) {
  const std::string_view t(tail);
  return keyBare.size() >= t.size() && keyBare.compare(keyBare.size() - t.size(), t.size(), t) == 0;
}

void elide(SandhiWord& w, const SandhiWord& next) {
  const std::string k = keyNoAccent(w.form);
  static const char* const kElidable[] = {"δε", "ἀλλα", "τε", "οὐδε", "μηδε", "ἀπο", "ἐπι", "κατα", "μετα", "παρα",
                                          "δια", "ὑπο", "ἀντι", "γε"};
  bool ok = false;
  for (const char* e : kElidable) ok = ok || k == e;
  if (!ok) return;
  std::u32string u = decompose(stripAccents(w.form));
  // drop the final vowel and its marks
  while (!u.empty() && isMarkCp(u.back())) u.pop_back();
  if (u.empty() || !isVowelCp(u.back())) return;
  u.pop_back();
  if (startsWithRough(next.form) && !u.empty()) {
    switch (u.back()) {
      case U'π': u.back() = U'φ'; break;
      case U'τ': u.back() = U'θ'; break;
      case U'κ': u.back() = U'χ'; break;
      default: break;
    }
  }
  u.push_back(0x2019);
  w.form = compose(u);
  w.movableNu = false;
}

}  // namespace

void sandhi(std::vector<SandhiWord>& ws, const SandhiOptions& o) {
  const size_t n = ws.size();
  for (SandhiWord& w : ws) {
    w.form = ultimaToAcute(w.form);
    if (o.autoEnclitic && !w.enclitic && !w.existential && isEncliticForm(w.form)) w.enclitic = true;
    if (!w.proclitic && isProclitic(w.form)) w.proclitic = true;
  }
  auto nextOf = [&](size_t i) -> const SandhiWord* {
    if (i + 1 >= n || !ws[i].punctAfter.empty()) return nullptr;
    return &ws[i + 1];
  };
  // 1. οὐ / ἐκ by the next sound
  for (size_t i = 0; i < n; ++i) {
    const std::string k = text::greek_key(ws[i].form);
    const SandhiWord* nx = nextOf(i);
    if (k == "οὐ" || k == "οὐκ" || k == "οὐχ" || k == "οὔ") {
      if (!nx) ws[i].form = "οὔ";
      else if (startsWithVowel(nx->form)) ws[i].form = startsWithRough(nx->form) ? "οὐχ" : "οὐκ";
      else ws[i].form = "οὐ";
      ws[i].proclitic = nx != nullptr;
    } else if (k == "ἐκ" || k == "ἐξ") {
      ws[i].form = nx && startsWithVowel(nx->form) ? "ἐξ" : "ἐκ";
    }
  }
  // 2. movable nu
  for (size_t i = 0; i < n; ++i) {
    SandhiWord& w = ws[i];
    if (!w.movableNu) continue;
    std::string b = text::greek_bare(w.form);
    if (endsWithKey(b, "ν")) {   // the caller gave the form with ν: start from the bare form
      std::u32string u = decompose(w.form);
      while (!u.empty() && isMarkCp(u.back())) u.pop_back();
      if (!u.empty() && u.back() == U'ν') u.pop_back();
      w.form = compose(u);
    }
    const SandhiWord* nx = nextOf(i);
    const bool atEnd = i + 1 >= n || sentenceEnd(w.punctAfter) || w.punctAfter.find(',') != std::string::npos;
    if (atEnd || (nx && startsWithVowel(nx->form))) w.form += "ν";
  }
  // 3. elision (optional)
  if (o.elision)
    for (size_t i = 0; i + 1 < n; ++i)
      if (const SandhiWord* nx = nextOf(i))
        if (startsWithVowel(nx->form)) elide(ws[i], *nx);
  // 4. enclitics
  for (size_t i = 0; i < n; ++i) {
    SandhiWord& w = ws[i];
    if (!w.enclitic) continue;
    SandhiWord* host = (i > 0 && ws[i - 1].punctAfter.empty()) ? &ws[i - 1] : nullptr;
    const std::string kb = text::greek_bare(w.form);
    const bool esti = kb == "εστι" || kb == "εστιν";
    if (esti) {
      bool ortho = w.existential || host == nullptr;
      if (host) {
        const std::string hk = keyNoAccent(host->form);
        ortho = ortho || inList(hk, {"οὐκ", "οὐχ", "οὐ", "μη", "εἰ", "ὡσ", "και", "ἀλλα", "ἀλλ’", "τουτο", "τουτ’"});
      }
      if (ortho) {
        // ἔστι(ν): acute on the first syllable, smooth breathing kept
        std::u32string u = decompose(stripAccents(w.form));
        thread_local std::vector<Nucleus> ns;
        nuclei(u, ns);
        if (!ns.empty()) insertAccent(u, ns.front(), kAcute);
        w.form = compose(u);
        w.enclitic = false;
        if (host && host->enclitic && accentOf(host->form).accents == 0) host->form = addUltimaAcute(host->form);
        continue;
      }
    }
    const AccentInfo ea = accentOf(w.form);
    if (!host) {
      if (ea.syllables >= 2) w.form = encliticAccented(w.form);
      continue;
    }
    const AccentInfo h = accentOf(host->form);
    if (h.accents == 0) {   // proclitic or enclitic host
      host->form = addUltimaAcute(host->form);
      w.form = stripAccents(w.form);
    } else if (h.position == 0) {   // oxytone (keeps the acute) or perispomenon
      host->form = ultimaToAcute(host->form);
      w.form = stripAccents(w.form);
    } else if (h.position == 1 && h.type == Accent::Acute) {   // paroxytone
      w.form = ea.syllables >= 2 ? encliticAccented(w.form) : stripAccents(w.form);
    } else if (h.position == 1 && h.type == Accent::Circumflex) {   // properispomenon
      const std::string hb = text::greek_bare(host->form);
      if (!(endsWithKey(hb, "ξ") || endsWithKey(hb, "ψ"))) host->form = addUltimaAcute(host->form);
      w.form = stripAccents(w.form);
    } else {   // proparoxytone
      host->form = addUltimaAcute(host->form);
      w.form = stripAccents(w.form);
    }
  }
  // 5. grave
  for (size_t i = 0; i < n; ++i) {
    SandhiWord& w = ws[i];
    const SandhiWord* nx = nextOf(i);
    if (!nx || nx->enclitic || w.noGrave || w.interrogative) continue;
    const AccentInfo a = accentOf(w.form);
    if (a.position != 0 || a.type != Accent::Acute) continue;
    if (a.syllables == 1 && inList(text::greek_key(w.form), {"τίσ", "τί"})) continue;   // interrogative
    w.form = ultimaToGrave(w.form);
  }
}

std::string accentuate(std::string_view phrase, const SandhiOptions& o) {
  std::vector<SandhiWord> ws;
  size_t i = 0;
  while (i < phrase.size()) {
    while (i < phrase.size() && phrase[i] == ' ') ++i;
    if (i >= phrase.size()) break;
    size_t e = phrase.find(' ', i);
    if (e == std::string_view::npos) e = phrase.size();
    std::string tok(phrase.substr(i, e - i));
    i = e;
    // trailing punctuation
    std::string punct;
    for (;;) {
      if (tok.empty()) break;
      const unsigned char c = (unsigned char)tok.back();
      if (c == ',' || c == '.' || c == ';' || c == '!' || c == '?' || c == ':') {
        punct.insert(punct.begin(), tok.back());
        tok.pop_back();
        continue;
      }
      if (tok.size() >= 2 && (tok.compare(tok.size() - 2, 2, "\xC2\xB7") == 0 || tok.compare(tok.size() - 2, 2, "\xCE\x87") == 0 ||
                              tok.compare(tok.size() - 2, 2, "\xCD\xBE") == 0)) {
        punct.insert(0, tok.substr(tok.size() - 2));
        tok.resize(tok.size() - 2);
        continue;
      }
      break;
    }
    if (tok.empty()) {
      if (!ws.empty()) ws.back().punctAfter += punct;
      continue;
    }
    SandhiWord w;
    w.form = text::nfc(tok);
    w.punctAfter = punct;
    const std::string b = text::greek_bare(w.form);
    // forms that may take ν by their ending alone (the phrase carries no features): ἐστί / εἰσί and -σι(ν) verbs
    if (b == "εστι" || b == "εστιν" || b == "εισι" || b == "εισιν") w.movableNu = true;
    // -σι (3rd plural, 3rd singular -σι, dative plural): movable ν by the ending alone
    if (b.size() > 4 && b.compare(b.size() - 4, 4, "σι") == 0) w.movableNu = true;
    ws.push_back(std::move(w));
  }
  sandhi(ws, o);
  std::string out;
  for (size_t k = 0; k < ws.size(); ++k) {
    if (k) out += ' ';
    out += ws[k].form;
    out += ws[k].punctAfter;
  }
  return out;
}

bool movableNuCandidate(std::string_view form, const Features& f) {
  const std::string b = text::greek_bare(form);
  auto ends = [&](const char* t) { return endsWithKey(b, t); };
  if (b == "εστι" || b == "εισι" || b == "εστιν" || b == "εισιν") return true;
  if (f.pos == feat::Verb && f.person == feat::P3 && (f.mood == feat::Indicative || f.mood == 0)) {
    if (ends("σι") || ends("σιν")) return true;
    if (f.number == feat::Sg && (ends("ε") || ends("εν")) &&
        (f.tense == feat::Aorist || f.tense == feat::Imperfect || f.tense == feat::Perfect))
      return true;
  }
  if (f.case_ == feat::Dat && f.number == feat::Pl && (ends("σι") || ends("σιν"))) return true;
  return false;
}

}  // namespace vp::grc
