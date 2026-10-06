// Pure parts of the online check: REST definition JSON -> definitions, HTML -> text, gloss keywords, verdict.
// No I/O here. Every function is total: malformed input gives an error Result or an empty value, never a throw.
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <set>

#include "json.hpp"
#include "vp/online.h"
#include "vp/text.h"

namespace vp::online {

namespace {

constexpr size_t kMaxDefinitions = 200;
constexpr size_t kMaxDefinitionBytes = 2000;

bool startsWithCI(std::string_view s, size_t at, std::string_view word) {
  if (at + word.size() > s.size()) return false;
  for (size_t i = 0; i < word.size(); ++i) {
    char c = s[at + i];
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    if (c != word[i]) return false;
  }
  return true;
}

// Decodes the entity starting at s[i] == '&'. Returns the number of bytes consumed (0 = not an entity).
size_t decodeEntity(std::string_view s, size_t i, std::string& out) {
  const size_t semi = s.find(';', i);
  if (semi == std::string_view::npos || semi - i > 10 || semi == i + 1) return 0;
  const std::string_view name = s.substr(i + 1, semi - i - 1);
  if (name[0] == '#') {
    if (name.size() < 2) return 0;
    const bool hex = name[1] == 'x' || name[1] == 'X';
    const std::string digits(name.substr(hex ? 2 : 1));
    if (digits.empty()) return 0;
    for (char c : digits)
      if (!(hex ? std::isxdigit(static_cast<unsigned char>(c)) : std::isdigit(static_cast<unsigned char>(c)))) return 0;
    const unsigned long cp = std::strtoul(digits.c_str(), nullptr, hex ? 16 : 10);
    if (cp == 0xA0) out.push_back(' ');
    else text::appendUtf8(out, cp > 0x10FFFF ? text::kReplacement : static_cast<char32_t>(cp));
    return semi - i + 1;
  }
  static const struct { const char* n; const char* v; } kNamed[] = {
      {"amp", "&"},       {"lt", "<"},        {"gt", ">"},         {"quot", "\""},      {"apos", "'"},
      {"nbsp", " "},      {"ndash", "\xE2\x80\x93"}, {"mdash", "\xE2\x80\x94"}, {"hellip", "\xE2\x80\xA6"},
      {"lsquo", "\xE2\x80\x98"}, {"rsquo", "\xE2\x80\x99"}, {"ldquo", "\xE2\x80\x9C"}, {"rdquo", "\xE2\x80\x9D"},
      {"thinsp", " "},    {"middot", "\xC2\xB7"}};
  for (const auto& e : kNamed)
    if (name == e.n) {
      out += e.v;
      return semi - i + 1;
    }
  return 0;
}

bool isWordByte(unsigned char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c >= 0x80;
}

std::string stem(std::string w) {
  const size_t n = w.size();
  auto ends = [&](const char* suf) {
    const size_t k = std::char_traits<char>::length(suf);
    return n >= k && w.compare(n - k, k, suf) == 0;
  };
  if (n > 4 && ends("ies")) return w.substr(0, n - 3) + "y";
  if (n > 4 && (ends("ses") || ends("xes") || ends("zes") || ends("ches") || ends("shes"))) return w.substr(0, n - 2);
  if (n > 3 && ends("s") && !ends("ss") && !ends("us") && !ends("is")) return w.substr(0, n - 1);
  return w;
}

std::vector<std::string> words(std::string_view s) {
  const std::string low = text::lower(s);
  std::vector<std::string> out;
  std::string cur;
  for (char ch : low) {
    if (isWordByte(static_cast<unsigned char>(ch))) {
      cur.push_back(ch);
    } else if (!cur.empty()) {
      out.push_back(stem(cur));
      cur.clear();
    }
  }
  if (!cur.empty()) out.push_back(stem(cur));
  return out;
}

const std::set<std::string>& stopwords() {
  static const std::set<std::string> k = {
      "a", "an", "the", "to", "of", "in", "on", "at", "by", "for", "with", "from", "into", "onto", "upon", "about",
      "and", "or", "but", "nor", "not", "no", "be", "is", "are", "was", "were", "been", "being", "am", "do", "does",
      "did", "have", "has", "had", "it", "its", "this", "that", "these", "those", "as", "up", "down", "out", "off",
      "over", "under", "than", "then", "so", "such", "some", "any", "one", "ones", "someone", "something", "who",
      "whom", "whose", "which", "what", "where", "when", "why", "how", "me", "my", "we", "us", "our", "you",
      "your", "he", "him", "his", "she", "her", "they", "them", "their", "oneself", "itself", "etc", "sth", "sb",
      "very", "also", "kind", "sort", "thing", "make", "get", "go", "put", "take", "used", "use", "person"};
  return k;
}

}  // namespace

std::string stripHtml(std::string_view s) {
  std::string raw;
  raw.reserve(s.size());
  size_t i = 0;
  while (i < s.size()) {
    const char c = s[i];
    if (c == '<') {
      const size_t close = s.find('>', i);
      if (close == std::string_view::npos) break;   // unterminated tag: drop the rest
      const bool script = startsWithCI(s, i + 1, "script") || startsWithCI(s, i + 1, "style");
      if (script) {
        const std::string_view endTag = startsWithCI(s, i + 1, "script") ? "</script" : "</style";
        size_t j = close + 1;
        while (j < s.size() && !startsWithCI(s, j, endTag)) ++j;
        const size_t gt = j < s.size() ? s.find('>', j) : std::string_view::npos;
        i = gt == std::string_view::npos ? s.size() : gt + 1;
        raw.push_back(' ');
      } else {
        // Block-ish tags separate words.
        raw.push_back(' ');
        i = close + 1;
      }
    } else if (c == '&') {
      const size_t used = decodeEntity(s, i, raw);
      if (used) i += used;
      else raw.push_back(s[i++]);
    } else {
      raw.push_back(c);
      ++i;
    }
  }
  // Collapse ASCII white space and trim.
  std::string out;
  out.reserve(raw.size());
  bool space = false;
  for (char ch : raw) {
    if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f' || ch == '\v') {
      space = true;
    } else {
      if (space && !out.empty()) out.push_back(' ');
      space = false;
      out.push_back(ch);
    }
  }
  // " ," and " ." left behind by removed tags.
  std::string tidy;
  tidy.reserve(out.size());
  for (size_t k = 0; k < out.size(); ++k) {
    if (out[k] == ' ' && k + 1 < out.size() && (out[k + 1] == ',' || out[k + 1] == '.' || out[k + 1] == ';' ||
                                                 out[k + 1] == ':' || out[k + 1] == ')'))
      continue;
    if (out[k] == ' ' && !tidy.empty() && tidy.back() == '(') continue;
    tidy.push_back(out[k]);
  }
  return text::isValidUtf8(tidy) ? tidy : text::toUtf8(text::toUtf32(tidy));
}

std::vector<std::string> glossKeywords(std::string_view glossEn) {
  std::vector<std::string> out;
  std::set<std::string> seen;
  for (std::string& w : words(glossEn)) {
    if (w.size() < 2 || stopwords().count(w)) continue;
    bool digits = true;
    for (char ch : w) digits = digits && ch >= '0' && ch <= '9';
    if (digits) continue;
    if (seen.insert(w).second) out.push_back(std::move(w));
  }
  return out;
}

Result<Section> parseDefinitions(std::string_view body, std::string_view langCode) {
  const nlohmann::json j = nlohmann::json::parse(body.begin(), body.end(), nullptr, false);
  if (j.is_discarded() || !j.is_object())
    return Result<Section>(ErrorCode::OnlineFailed, "malformed response from Wiktionary",
                           "Wiktionary sent an answer this version cannot read.");
  // The endpoint keys some languages by code ("la") and puts the rest under "other" with their English name
  // (Ancient Greek, checked live 2026-10-06). Both places are read, in page order: the code first.
  const std::string name = langCode == "la" ? "Latin" : langCode == "grc" ? "Ancient Greek" : std::string();
  Section sec;
  auto takeUsage = [&](const nlohmann::json& usage) {
    const auto defs = usage.find("definitions");
    if (defs == usage.end() || !defs->is_array()) return;
    sec.found = true;
    for (const nlohmann::json& d : *defs) {
      if (sec.definitions.size() >= kMaxDefinitions) break;
      if (!d.is_object()) continue;
      const auto t = d.find("definition");
      if (t == d.end() || !t->is_string()) continue;
      const std::string& html = t->get_ref<const std::string&>();
      std::string plain = stripHtml(std::string_view(html).substr(0, kMaxDefinitionBytes * 4));
      if (plain.size() > kMaxDefinitionBytes) plain = clipSummary(plain, kMaxDefinitionBytes / 2);
      if (!plain.empty()) sec.definitions.push_back(std::move(plain));
    }
  };
  const auto it = j.find(std::string(langCode));
  if (it != j.end() && it->is_array())
    for (const nlohmann::json& usage : *it)
      if (usage.is_object()) takeUsage(usage);
  const auto other = j.find("other");
  if (!name.empty() && langCode != "other" && other != j.end() && other->is_array())
    for (const nlohmann::json& usage : *other) {
      if (!usage.is_object()) continue;
      const auto lang = usage.find("language");
      if (lang != usage.end() && lang->is_string() && lang->get_ref<const std::string&>() == name) takeUsage(usage);
    }
  return sec;
}

std::string clipSummary(std::string_view s, size_t maxCodePoints) {
  if (maxCodePoints == 0) return std::string();
  size_t count = 0, i = 0, cut = std::string_view::npos;
  while (i < s.size()) {
    if (count == maxCodePoints - 1) cut = i;
    text::decodeUtf8(s, i);
    ++count;
    if (count > maxCodePoints) {
      std::string out(s.substr(0, cut));
      while (!out.empty() && out.back() == ' ') out.pop_back();
      return out + "\xE2\x80\xA6";   // U+2026
    }
  }
  return std::string(s);
}

Evidence verdictFor(const Section& sec, std::string_view glossEn, const std::string& pageUrl) {
  Evidence ev;
  ev.url = pageUrl;
  if (!sec.found || sec.definitions.empty()) {
    ev.verdict = Evidence::Unknown;
    ev.summary = "no Wiktionary entry";
    return ev;
  }
  const std::vector<std::string> kws = glossKeywords(glossEn);
  if (kws.empty()) {
    ev.verdict = Evidence::Unknown;
    ev.summary = clipSummary("no gloss to compare; Wiktionary: " + sec.definitions.front());
    return ev;
  }
  for (const std::string& def : sec.definitions) {
    const std::vector<std::string> dw = words(def);
    const std::set<std::string> have(dw.begin(), dw.end());
    for (const std::string& k : kws)
      if (have.count(k)) {
        ev.verdict = Evidence::Agrees;
        ev.summary = clipSummary("Wiktionary: " + def);
        return ev;
      }
  }
  ev.verdict = Evidence::Disagrees;
  ev.summary = clipSummary("Wiktionary: " + sec.definitions.front());
  return ev;
}

const char* verdictName(Evidence::Verdict v) {
  switch (v) {
    case Evidence::Agrees: return "agrees";
    case Evidence::Disagrees: return "disagrees";
    case Evidence::Unknown: return "unknown";
    case Evidence::Error: return "error";
  }
  return "unknown";
}

}  // namespace vp::online
