// Pure helpers of the CLI. See views.h.
#include "views.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "vp/text.h"

namespace vpcli {

namespace feat = vp::feat;
namespace subs = vp::subs;
using vp::rules::Features;

// ---------------------------------------------------------------------------------------------- features
const char* posName(uint8_t pos) {
  static const char* const k[] = {"",       "noun",   "verb",   "adj",  "adv",    "pron",       "num",
                                  "prep",   "conj",   "intj",   "det",  "name",   "particle",   "participle",
                                  "phrase", "suffix", "prefix", "article", "postp", "symbol",    "punct"};
  if (pos < sizeof k / sizeof k[0]) return k[pos];
  return "other";
}

const char* genderName(uint8_t g) {
  static const char* const k[] = {"",       "masculine",        "feminine",         "neuter",
                                  "masculine-feminine", "masculine-neuter", "feminine-neuter", "common"};
  return g < 8 ? k[g] : "";
}

namespace {
const char* caseName(uint8_t c) {
  static const char* const k[] = {"", "nominative", "genitive", "dative", "accusative", "ablative", "vocative",
                                  "locative"};
  return c < 8 ? k[c] : "";
}
const char* numberName(uint8_t n) {
  static const char* const k[] = {"", "singular", "plural", "dual"};
  return n < 4 ? k[n] : "";
}
const char* personName(uint8_t p) {
  static const char* const k[] = {"", "first", "second", "third"};
  return p < 4 ? k[p] : "";
}
const char* tenseName(uint8_t t) {
  static const char* const k[] = {"", "present", "imperfect", "future", "perfect", "pluperfect", "future-perfect",
                                  "aorist"};
  return t < 8 ? k[t] : "";
}
const char* moodName(uint8_t m) {
  static const char* const k[] = {"", "indicative", "subjunctive", "imperative", "infinitive", "participle",
                                  "gerund", "optative"};
  return m < 8 ? k[m] : "";
}
const char* voiceName(uint8_t v) {
  static const char* const k[] = {"", "active", "passive", "middle"};
  return v < 4 ? k[v] : "";
}
const char* degreeName(uint8_t d) {
  static const char* const k[] = {"", "positive", "comparative", "superlative"};
  return d < 4 ? k[d] : "";
}

std::vector<std::string> bitNames(uint32_t bits, const char* const* names, size_t n) {
  std::vector<std::string> out;
  for (size_t i = 0; i < n; ++i)
    if (bits & (1u << i)) out.emplace_back(names[i]);
  return out;
}
}  // namespace

Features featuresFromPacked(uint32_t packed) {
  const feat::Features f = feat::unpack(packed);
  Features o;
  o.pos = posName(f.pos);
  o.case_ = caseName(f.case_);
  o.number = numberName(f.number);
  o.gender = genderName(f.gender);
  o.person = personName(f.person);
  o.tense = tenseName(f.tense);
  o.mood = moodName(f.mood);
  if (f.extra & feat::Gerundive) o.mood = "gerundive";
  else if (f.extra & feat::Supine) o.mood = "supine";
  o.voice = voiceName(f.voice);
  o.degree = degreeName(f.degree);
  return o;
}

json featureJson(const Features& f) {
  return json{{"pos", f.pos},       {"case", f.case_}, {"number", f.number}, {"gender", f.gender},
              {"person", f.person}, {"tense", f.tense}, {"mood", f.mood},    {"voice", f.voice},
              {"degree", f.degree}};
}

std::vector<std::string> extraNames(uint8_t extra) {
  static const char* const k[] = {"supine", "gerundive", "attic", "alternative", "contracted"};
  return bitNames(extra, k, 5);
}
std::vector<std::string> lemmaFlagNames(uint16_t flags) {
  static const char* const k[] = {"proper-name", "indeclinable", "deponent", "impersonal",
                                  "shared-el",   "plural-only",  "defective", "has-table"};
  return bitNames(flags, k, 8);
}
std::vector<std::string> analFlagNames(uint16_t flags) {
  static const char* const k[] = {"table",       "form-of",    "whitaker-only", "alternative",
                                  "non-attic",   "late-latin", "poetic-rare",   "enclitic-stripped"};
  return bitNames(flags, k, 8);
}
std::vector<std::string> senseTagNames(uint16_t tags) {
  static const char* const k[] = {"transitive", "intransitive", "figurative", "rare",      "archaic",
                                  "poetic",     "medieval",     "new-latin",  "with-dat",  "with-abl",
                                  "with-gen",   "with-acc",     "with-inf",   "impersonal", "reflexive"};
  return bitNames(tags, k, 15);
}

json lemmaJson(const vp::lex::Lexicon& lx, uint32_t id) {
  const vp::lex::Lemma l = lx.lemma(id);
  if (l.id == vp::lex::kNoLemma) return nullptr;
  static const char* const src[] = {"", "derived", "teacher"};
  std::string whit;
  if (l.whitFreq >= 'A' && l.whitFreq <= 'Z') whit.push_back(static_cast<char>(l.whitFreq));
  return json{{"id", l.id},
              {"head", std::string(l.head)},
              {"pos", posName(l.pos)},
              {"gender", genderName(l.gender)},
              {"cls", l.cls},
              {"tier", l.tier},
              {"tierSource", l.tierSource < 3 ? src[l.tierSource] : ""},
              {"freqRank", l.freqRank},
              {"whitFreq", whit},
              {"glossEn", std::string(l.glossEn)},
              {"glossEs", std::string(l.glossEs)},
              {"emoji", std::string(l.emoji)},
              {"principal", std::string(l.principal)},
              {"flags", lemmaFlagNames(l.flags)}};
}

json senseJson(const vp::lex::Sense& s) {
  return json{{"glossEn", std::string(s.glossEn)}, {"glossEs", std::string(s.glossEs)},
              {"keywords", std::string(s.keywords)}, {"tags", senseTagNames(s.tags)}, {"rank", s.rank}};
}

// ---------------------------------------------------------------------------------------------- languages
bool looksGreek(std::string_view w) {
  size_t i = 0;
  while (i < w.size()) {
    const char32_t cp = vp::text::decodeUtf8(w, i);
    if ((cp >= 0x0370 && cp <= 0x03FF) || (cp >= 0x1F00 && cp <= 0x1FFF)) return true;
  }
  return false;
}

std::string lexKey(std::string_view word, bool greek) {
  return greek ? vp::text::greek_key(word) : vp::text::latin_key(word);
}

const char* langCode(vp::rules::Lang l) {
  switch (l) {
    case vp::rules::Lang::En: return "en";
    case vp::rules::Lang::Es: return "es";
    case vp::rules::Lang::La: return "la";
    case vp::rules::Lang::Grc: return "grc";
  }
  return "en";
}

bool langFromCode(const std::string& c, vp::rules::Lang& out) {
  if (c == "en") out = vp::rules::Lang::En;
  else if (c == "es") out = vp::rules::Lang::Es;
  else if (c == "la") out = vp::rules::Lang::La;
  else if (c == "grc") out = vp::rules::Lang::Grc;
  else return false;
  return true;
}

bool parsePair(const std::string& pair, PairLangs& out) {
  const size_t d = pair.find('-');
  if (d == std::string::npos) return false;
  return langFromCode(pair.substr(0, d), out.source) && langFromCode(pair.substr(d + 1), out.target);
}

// ---------------------------------------------------------------------------------------------- formats
std::string baseName(const std::string& p) {
  const size_t k = p.find_last_of("/\\");
  return k == std::string::npos ? p : p.substr(k + 1);
}

bool formatFromPath(const std::string& path, subs::Format& out) {
  const std::string b = baseName(path);
  const size_t d = b.rfind('.');
  if (d == std::string::npos) return false;
  std::string e = b.substr(d + 1);
  for (char& c : e) c = static_cast<char>((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c);
  if (e == "ssa") e = "ass";
  return formatFromName(e, out);
}

bool formatFromName(const std::string& n, subs::Format& out) {
  if (n == "srt") out = subs::Format::Srt;
  else if (n == "vtt") out = subs::Format::Vtt;
  else if (n == "ass") out = subs::Format::Ass;
  else if (n == "txt") out = subs::Format::Txt;
  else return false;
  return true;
}

const char* formatName(subs::Format f) {
  switch (f) {
    case subs::Format::Srt: return "srt";
    case subs::Format::Vtt: return "vtt";
    case subs::Format::Ass: return "ass";
    case subs::Format::Txt: return "txt";
  }
  return "srt";
}

// ---------------------------------------------------------------------------------------------- target text
std::vector<std::string> splitLines(const std::string& text) {
  std::vector<std::string> out;
  if (text.empty()) return out;
  size_t a = 0;
  while (true) {
    const size_t b = text.find('\n', a);
    std::string line = text.substr(a, b == std::string::npos ? std::string::npos : b - a);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    out.push_back(std::move(line));
    if (b == std::string::npos) break;
    a = b + 1;
  }
  return out;
}

std::string flattenLines(const std::string& text) {
  std::string out;
  out.reserve(text.size());
  bool space = false;
  for (char c : text) {
    if (c == '\n' || c == '\r' || c == ' ' || c == '\t') {
      space = !out.empty();
      continue;
    }
    if (space) out.push_back(' ');
    space = false;
    out.push_back(c);
  }
  return out;
}

std::vector<std::string> displayLines(const std::string& target, const subs::BreakHints& hints, int maxLine,
                                      int maxLines, bool* overflow) {
  if (overflow) *overflow = false;
  if (target.empty()) return {};
  std::vector<std::string> lines = splitLines(target);
  bool fits = static_cast<int>(lines.size()) <= maxLines;
  for (const std::string& l : lines)
    if (static_cast<int>(subs::visibleLength(l)) > maxLine) fits = false;
  if (fits) return lines;
  return subs::breakLines(flattenLines(target), hints, maxLine, maxLines, overflow);
}

namespace {
bool isPictographic(char32_t cp) {
  if (cp >= 0x1F000 && cp <= 0x1FAFF) return true;
  if (cp >= 0x2600 && cp <= 0x27BF) return !(cp >= 0x2669 && cp <= 0x266F);   // music notes stay
  switch (cp) {
    case 0x231A: case 0x231B: case 0x2328: case 0x23CF: case 0x2B1B: case 0x2B1C: case 0x2B50: case 0x2B55:
    case 0x3030: case 0x303D: case 0x3297: case 0x3299:
      return true;
    default:
      return (cp >= 0x23E9 && cp <= 0x23F3) || (cp >= 0x23F8 && cp <= 0x23FA);
  }
}
bool isEmojiModifier(char32_t cp) {
  return cp == 0xFE0F || cp == 0xFE0E || cp == 0x200D || cp == 0x20E3 || (cp >= 0x1F3FB && cp <= 0x1F3FF) ||
         (cp >= 0xE0020 && cp <= 0xE007F);
}
std::string collapseSpaces(const std::string& s) {
  std::string out;
  for (const std::string& line : splitLines(s)) {
    if (!out.empty()) out.push_back('\n');
    std::string l;
    bool space = false;
    for (char c : line) {
      if (c == ' ') {
        space = !l.empty();
        continue;
      }
      if (space) l.push_back(' ');
      space = false;
      l.push_back(c);
    }
    out += l;
  }
  return out;
}
}  // namespace

std::string stripAddedEmoji(const std::string& target, const std::string& source) {
  std::vector<char32_t> inSource;
  for (size_t i = 0; i < source.size();) {
    const char32_t cp = vp::text::decodeUtf8(source, i);
    if (isPictographic(cp)) inSource.push_back(cp);
  }
  std::string out;
  out.reserve(target.size());
  bool stripped = false, any = false;
  for (size_t i = 0; i < target.size();) {
    const size_t at = i;
    const char32_t cp = vp::text::decodeUtf8(target, i);
    if (stripped && isEmojiModifier(cp)) continue;
    if (isPictographic(cp) && std::find(inSource.begin(), inSource.end(), cp) == inSource.end()) {
      stripped = any = true;
      continue;
    }
    stripped = false;
    out.append(target, at, i - at);
  }
  return any ? collapseSpaces(out) : out;
}

namespace {
bool isGreekVowel(char32_t c) {
  switch (c) {
    case 0x03B1: case 0x03B5: case 0x03B7: case 0x03B9: case 0x03BF: case 0x03C5: case 0x03C9:
    case 0x0391: case 0x0395: case 0x0397: case 0x0399: case 0x039F: case 0x03A5: case 0x03A9:
      return true;
    default:
      return false;
  }
}
bool isGreekLetterOrMark(char32_t c) { return (c >= 0x0370 && c <= 0x03FF) || (c >= 0x0300 && c <= 0x036F); }

// Monosyllables carry no accent in monotonic Greek (except the usual ή, πού, πώς). A syllable is approximated by a
// run of vowels (diphthongs count once; a diaeresis starts a new syllable).
void dropMonosyllableAccent(std::u32string& w) {
  int groups = 0;
  bool inVowel = false;
  std::u32string base;
  for (size_t i = 0; i < w.size(); ++i) {
    const char32_t c = w[i];
    if (c >= 0x0300 && c <= 0x036F) {
      if (c == 0x0308 && inVowel) ++groups;
      continue;
    }
    base.push_back(c >= 0x0391 && c <= 0x03A9 ? c + 0x20 : c);
    if (isGreekVowel(c)) {
      if (!inVowel) ++groups;
      inVowel = true;
    } else {
      inVowel = false;
    }
  }
  if (groups != 1 || base == U"\u03B7" || base == U"\u03C0\u03BF\u03C5" || base == U"\u03C0\u03C9\u03C2") return;
  w.erase(std::remove(w.begin(), w.end(), static_cast<char32_t>(0x0301)), w.end());
}
}  // namespace

std::string greekMonotonic(const std::string& text) {
  const std::string d = vp::text::nfd(text);
  std::u32string mapped;
  mapped.reserve(d.size());
  for (size_t i = 0; i < d.size();) {
    const char32_t cp = vp::text::decodeUtf8(d, i);
    switch (cp) {
      case 0x0313: case 0x0314: case 0x0343: case 0x0345:
        break;                                      // breathings, koronis, iota subscript
      case 0x0300: case 0x0342:
        mapped.push_back(0x0301);                   // grave, circumflex -> acute (tonos)
        break;
      case 0x0344:
        mapped.push_back(0x0308);
        mapped.push_back(0x0301);
        break;
      default:
        mapped.push_back(cp);
    }
  }
  std::u32string out, word;
  auto flush = [&]() {
    dropMonosyllableAccent(word);
    out += word;
    word.clear();
  };
  for (char32_t c : mapped) {
    if (isGreekLetterOrMark(c)) {
      word.push_back(c);
    } else {
      flush();
      out.push_back(c);
    }
  }
  flush();
  return vp::text::nfc(vp::text::toUtf8(out));
}

vp::rules::Check markupCheck(const std::string& target, int maxLine, int maxLines) {
  vp::rules::Check c;
  c.id = "A5";
  const std::vector<std::string> lines = splitLines(target);
  if (static_cast<int>(lines.size()) > maxLines) {
    c.ok = false;
    c.detail = std::to_string(lines.size()) + " lines (at most " + std::to_string(maxLines) + ")";
    return c;
  }
  for (size_t i = 0; i < lines.size(); ++i) {
    const size_t w = subs::visibleLength(lines[i]);
    if (static_cast<int>(w) > maxLine) {
      c.ok = false;
      c.detail = "line " + std::to_string(i + 1) + " has " + std::to_string(w) + " characters (at most " +
                 std::to_string(maxLine) + ")";
      return c;
    }
  }
  return c;
}

// ---------------------------------------------------------------------------------------------- tags
namespace {
// HTML-like tag name and direction; false for ASS blocks and anything else.
bool htmlTag(const std::string& raw, std::string& name, bool& closing) {
  if (raw.size() < 3 || raw[0] != '<') return false;
  size_t i = 1;
  closing = raw[1] == '/';
  if (closing) ++i;
  name.clear();
  while (i < raw.size() && ((raw[i] >= 'a' && raw[i] <= 'z') || (raw[i] >= 'A' && raw[i] <= 'Z'))) {
    name.push_back(static_cast<char>(raw[i] >= 'A' && raw[i] <= 'Z' ? raw[i] - 'A' + 'a' : raw[i]));
    ++i;
  }
  return !name.empty();
}
bool balanced(const std::string& n) { return n == "i" || n == "b" || n == "u" || n == "s" || n == "font"; }
bool isContent(const subs::Span& s) {
  return s.kind == subs::Span::Text && s.raw.find_first_not_of(" \t") != std::string::npos;
}
}  // namespace

std::vector<subs::Span> retargetSpans(const std::vector<subs::Span>& src, const std::string& text, bool singleLine,
                                      bool* dropped) {
  if (dropped) *dropped = false;
  size_t first = src.size(), last = src.size();
  for (size_t i = 0; i < src.size(); ++i)
    if (isContent(src[i])) {
      if (first == src.size()) first = i;
      last = i;
    }
  std::vector<subs::Span> lead, trail;
  std::vector<std::string> open;
  std::string name;
  bool closing = false;
  for (size_t i = 0; i < src.size(); ++i) {
    const subs::Span& s = src[i];
    if (s.kind != subs::Span::Tag) continue;
    const bool isLead = first == src.size() || i < first;
    const bool isTrail = first != src.size() && i > last;
    if (isLead) {
      lead.push_back(s);
      if (htmlTag(s.raw, name, closing) && balanced(name)) {
        if (!closing) open.push_back(name);
        else {
          auto it = std::find(open.rbegin(), open.rend(), name);
          if (it != open.rend()) open.erase(std::next(it).base());
        }
      }
    } else if (isTrail) {
      if (htmlTag(s.raw, name, closing) && balanced(name) && closing) {
        auto it = std::find(open.rbegin(), open.rend(), name);
        if (it == open.rend()) {
          if (dropped) *dropped = true;
          continue;
        }
        open.erase(std::next(it).base());
      }
      trail.push_back(s);
    } else if (dropped) {
      *dropped = true;
    }
  }
  for (auto it = open.rbegin(); it != open.rend(); ++it) {
    subs::Span c;
    c.kind = subs::Span::Tag;
    c.raw = "</" + *it + ">";
    trail.push_back(c);
  }
  std::vector<subs::Span> out = lead;
  if (singleLine) {
    subs::Span t;
    t.raw = flattenLines(text);
    out.push_back(t);
  } else {
    const std::vector<std::string> lines = splitLines(text);
    for (size_t i = 0; i < lines.size(); ++i) {
      if (i) {
        subs::Span nl;
        nl.kind = subs::Span::Newline;
        out.push_back(nl);
      }
      subs::Span t;
      t.raw = lines[i];
      out.push_back(t);
    }
  }
  out.insert(out.end(), trail.begin(), trail.end());
  return out;
}

// ---------------------------------------------------------------------------------------------- views
const char* confidenceName(vp::rules::Confidence c) {
  switch (c) {
    case vp::rules::Confidence::Ok: return "ok";
    case vp::rules::Confidence::Check: return "check";
    case vp::rules::Confidence::Fix: return "fix";
  }
  return "check";
}

namespace {
double round1(double v) { return std::isfinite(v) ? std::round(v * 10.0) / 10.0 : 0.0; }
double round3(double v) { return std::isfinite(v) ? std::round(v * 1000.0) / 1000.0 : 0.0; }
}  // namespace

json cueViewJson(const subs::Cue& src, const std::string& source, const vp::CueRecord& r, const CueViewCtx& ctx) {
  int64_t s = 0, e = 0;
  const bool timed = ctx.format != subs::Format::Txt && subs::parseTiming(src.timingRaw, ctx.format, s, e);
  if (!timed) s = e = 0;
  const std::string& shown = r.target.empty() ? source : r.target;
  double cps = 0;
  if (timed && e > s)
    cps = round1(static_cast<double>(subs::visibleLength(flattenLines(shown))) / (static_cast<double>(e - s) / 1000.0));
  bool overflow = false;
  std::vector<std::string> lines = ctx.format == subs::Format::Txt
                                       ? splitLines(r.target)
                                       : displayLines(r.target, ctx.hints, ctx.maxLine, ctx.maxLines, &overflow);
  json flags = json::array();
  if (cps > ctx.cpsLimit) flags.push_back("cps");
  if (overflow) flags.push_back("overflow");
  if (r.chosen >= 0) flags.push_back("alternative");
  return json{{"index", r.index},
              {"idRaw", src.idRaw},
              {"timingRaw", src.timingRaw},
              {"start", s},
              {"end", e},
              {"durationMs", e > s ? e - s : 0},
              {"source", source},
              {"target", r.target},
              {"state", r.state},
              {"confidence", r.confidence.empty() ? std::string("check") : r.confidence},
              {"score", round3(r.score)},
              {"cps", cps},
              {"lines", lines},
              {"flags", flags}};
}

json tokenJson(const vp::rules::TokenView& t) {
  json j{{"text", t.text}, {"display", t.display}, {"start", t.start}, {"end", t.end}};
  if (t.hasLemma) j["lemmaId"] = t.lemmaId;
  if (!t.features.pos.empty()) j["features"] = featureJson(t.features);
  if (t.tier) j["tier"] = t.tier;
  if (!t.emoji.empty()) j["emoji"] = t.emoji;
  if (t.unknown) j["unknown"] = true;
  if (t.fromRule) j["fromRule"] = true;
  return j;
}

json checkJson(const vp::CueCheck& c) { return json{{"id", c.id}, {"ok", c.ok}, {"detail", c.detail}}; }

json reasonJson(const vp::CueReason& r) {
  json data = nullptr;
  if (!r.data.empty()) {
    data = json::parse(r.data, nullptr, false);
    if (data.is_discarded()) data = r.data;
  }
  return json{{"tokenIndex", r.tokenIndex}, {"kind", r.kind}, {"text", r.text}, {"data", data}};
}

json altJson(const vp::Alternative& a) {
  return json{{"text", a.text}, {"reason", a.reason}, {"score", round3(a.score)}};
}

void applyOutput(const vp::rules::CueOutput& out, vp::CueRecord& r) {
  r.target = out.target;
  r.confidence = confidenceName(out.confidence);
  r.score = std::isfinite(out.score) ? std::min(1.0, std::max(0.0, out.score)) : 0.0;
  r.chosen = -1;
  r.alternatives.clear();
  for (const auto& a : out.alternatives) r.alternatives.push_back(vp::Alternative{a.text, a.reason, a.score});
  r.checks.clear();
  for (const auto& c : out.checks) r.checks.push_back(vp::CueCheck{c.id, c.ok, c.detail});
  r.reasons.clear();
  for (const auto& x : out.reasons) r.reasons.push_back(vp::CueReason{x.tokenIndex, x.kind, x.text, x.data});
}

// ---------------------------------------------------------------------------------------------- export
std::string formatTiming(int64_t a, int64_t b, subs::Format f) {
  auto one = [f](int64_t ms) {
    if (ms < 0) ms = 0;
    const int64_t h = ms / 3600000, m = ms / 60000 % 60, s = ms / 1000 % 60, x = ms % 1000;
    char buf[48];
    if (f == subs::Format::Ass)
      std::snprintf(buf, sizeof buf, "%lld:%02lld:%02lld.%02lld", static_cast<long long>(h),
                    static_cast<long long>(m), static_cast<long long>(s), static_cast<long long>(x / 10));
    else
      std::snprintf(buf, sizeof buf, "%02lld:%02lld:%02lld%c%03lld", static_cast<long long>(h),
                    static_cast<long long>(m), static_cast<long long>(s), f == subs::Format::Vtt ? '.' : ',',
                    static_cast<long long>(x));
    return std::string(buf);
  };
  return f == subs::Format::Ass ? one(a) + "," + one(b) : one(a) + " --> " + one(b);
}

std::string exportText(const std::string& target, const std::string& source, const ExportOptions& o) {
  if (target.empty()) return std::string();
  std::string t = target;
  if (!o.emoji) t = stripAddedEmoji(t, source);
  if (!o.macrons) t = vp::text::display_latin(t, false);
  if (o.monotonic) t = greekMonotonic(t);
  return t;
}

namespace {
std::vector<std::string> sourceLines(const subs::Cue& c) {
  std::vector<std::string> out(1);
  for (const subs::Span& s : c.spans) {
    if (s.kind == subs::Span::Newline) out.emplace_back();
    else if (s.kind == subs::Span::Text) out.back() += s.raw;
  }
  if (out.size() == 1 && out[0].empty()) out.clear();
  return out;
}

std::string assHeader(const std::string& nl) {
  return "[Script Info]" + nl + "ScriptType: v4.00+" + nl + "WrapStyle: 0" + nl + nl + "[V4+ Styles]" + nl +
         "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, "
         "Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, "
         "MarginR, MarginV, Encoding" + nl +
         "Style: Default,Arial,48,&H00FFFFFF,&H000000FF,&H00000000,&H64000000,0,0,0,0,100,100,0,0,1,2,1,2,20,20,20,1" +
         nl + nl + "[Events]" + nl + "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text" +
         nl;
}
}  // namespace

std::vector<std::string> previewLines(const subs::Cue& src, const std::string& target, const std::string& source,
                                      const ExportOptions& o) {
  const std::string t = exportText(target, source, o);
  if (t.empty()) return sourceLines(src);
  if (o.rebreak && o.format != subs::Format::Txt)
    return subs::breakLines(flattenLines(t), o.hints, o.maxLine, o.maxLines, nullptr);
  return splitLines(t);
}

vp::Result<ExportResult> buildExport(const subs::Document& doc, const std::vector<std::string>& targets,
                                     const std::vector<std::string>& sources, const ExportOptions& o) {
  ExportResult res;
  subs::Document d;
  const bool same = doc.format == o.format;
  if (!same && doc.format == subs::Format::Txt && o.format != subs::Format::Txt)
    return vp::Result<ExportResult>(vp::ErrorCode::UnsupportedFormat, "plain text has no timing",
                                    "A text project can only be exported as plain text.");
  if (same) {
    d = doc;
  } else {
    d.format = o.format;
    d.encoding = doc.encoding;
    d.bom = doc.bom;
    d.newline = doc.newline.empty() ? "\n" : doc.newline;
    if (o.format == subs::Format::Vtt) d.headerRaw = "WEBVTT" + d.newline + d.newline;
    if (o.format == subs::Format::Ass) d.headerRaw = assHeader(d.newline);
    d.cues.reserve(doc.cues.size());
  }
  const bool singleLine = o.rebreak && o.format != subs::Format::Txt;
  for (size_t i = 0; i < doc.cues.size(); ++i) {
    const subs::Cue& c = doc.cues[i];
    const int64_t pos = static_cast<int64_t>(i);
    const std::string& target = i < targets.size() ? targets[i] : std::string();
    const std::string& source = i < sources.size() ? sources[i] : std::string();
    const std::string t = exportText(target, source, o);
    if (t.empty()) res.warnings.push_back({pos, "untranslated"});
    int64_t s = 0, e = 0;
    const bool timed = doc.format != subs::Format::Txt && subs::parseTiming(c.timingRaw, doc.format, s, e);
    if (!t.empty() && timed && e > s) {
      const double cps = static_cast<double>(subs::visibleLength(flattenLines(t))) / (static_cast<double>(e - s) / 1000.0);
      if (cps > o.cpsLimit) res.warnings.push_back({pos, "cps"});
    }
    if (!t.empty() && !singleLine && o.format != subs::Format::Txt) {
      const std::vector<std::string> own = splitLines(t);
      bool fits = static_cast<int>(own.size()) <= o.maxLines;
      for (const std::string& l : own)
        if (static_cast<int>(subs::visibleLength(l)) > o.maxLine) fits = false;
      if (!fits) res.warnings.push_back({pos, "line_overflow"});
    }
    if (same) {
      if (t.empty()) continue;
      bool dropped = false;
      d.cues[i].spans = retargetSpans(c.spans, t, singleLine, &dropped);
      if (dropped) res.warnings.push_back({pos, "tags_dropped"});
    } else {
      subs::Cue n;
      n.index = static_cast<uint32_t>(d.cues.size() + 1);
      if (o.format == subs::Format::Srt || o.format == subs::Format::Vtt) n.idRaw = std::to_string(n.index);
      if (o.format != subs::Format::Txt) {
        if (!timed) res.warnings.push_back({pos, "bad_timing"});
        n.timingRaw = formatTiming(s, e, o.format);
      }
      const std::string text = t.empty() ? c.plainText() : t;
      std::vector<subs::Span> none;
      n.spans = retargetSpans(none, text, singleLine, nullptr);
      d.cues.push_back(std::move(n));
    }
  }
  subs::WriteOptions wo;
  wo.encoding = o.encoding;
  wo.bom = o.bom;
  wo.maxLine = o.maxLine;
  wo.maxLines = o.maxLines;
  wo.rebreak = o.rebreak;
  wo.hints = o.hints;
  std::vector<subs::Warning> ws;
  vp::Result<std::vector<uint8_t>> bytes = subs::write(d, wo, &ws);
  if (!bytes) return vp::Result<ExportResult>(bytes.error());
  for (const subs::Warning& w : ws) res.warnings.push_back({w.index ? static_cast<int64_t>(w.index) - 1 : -1, w.kind});
  res.bytes = std::move(bytes.value());
  return vp::Result<ExportResult>(std::move(res));
}

}  // namespace vpcli
