// Pure helpers of the CLI. See views.h.
#include "views.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "vp/text.h"
#if defined(VP_HAVE_RULES)
#include "vp/morph.h"
#include "vp/transfer_grc.h"
#endif

namespace vpcli {

namespace feat = vp::feat;
namespace subs = vp::subs;
using vp::rules::Features;

// ---------------------------------------------------------------------------------------------- features
namespace {
// Name tables in the order of the vp::feat enumerations (the same strings as realise::featureView). kMood carries
// the two extra-bit moods at 8 and 9 so stored tokens round-trip them.
const char* const kPos[] = {"",       "noun",   "verb",   "adj",  "adv",    "pron",       "num",
                            "prep",   "conj",   "intj",   "det",  "name",   "particle",   "participle",
                            "phrase", "suffix", "prefix", "article", "postp", "symbol",    "punct", "other"};
const char* const kGender[] = {"",       "masculine",        "feminine",         "neuter",
                               "masculine-feminine", "masculine-neuter", "feminine-neuter", "common"};
const char* const kCase[] = {"", "nominative", "genitive", "dative", "accusative", "ablative", "vocative", "locative"};
const char* const kNumber[] = {"", "singular", "plural", "dual"};
const char* const kPerson[] = {"", "first", "second", "third"};
const char* const kTense[] = {"", "present", "imperfect", "future", "perfect", "pluperfect", "future-perfect", "aorist"};
const char* const kMood[] = {"", "indicative", "subjunctive", "imperative", "infinitive", "participle", "gerund",
                             "optative", "gerundive", "supine"};
const char* const kVoice[] = {"", "active", "passive", "middle"};
const char* const kDegree[] = {"", "positive", "comparative", "superlative"};
constexpr size_t kPosN = 21;   // kPos without "other"

template <size_t N> const char* nameAt(const char* const (&t)[N], unsigned i) { return i < N ? t[i] : ""; }
template <size_t N> int indexOf(const char* const (&t)[N], const std::string& v) {
  for (size_t i = 0; i < N; ++i)
    if (v == t[i]) return static_cast<int>(i);
  return -1;
}
const char* caseName(uint8_t c) { return nameAt(kCase, c); }
const char* numberName(uint8_t n) { return nameAt(kNumber, n); }
const char* personName(uint8_t p) { return nameAt(kPerson, p); }
const char* tenseName(uint8_t t) { return nameAt(kTense, t); }
const char* moodName(uint8_t m) { return m < 8 ? kMood[m] : ""; }
const char* voiceName(uint8_t v) { return nameAt(kVoice, v); }
const char* degreeName(uint8_t d) { return nameAt(kDegree, d); }

std::vector<std::string> bitNames(uint32_t bits, const char* const* names, size_t n) {
  std::vector<std::string> out;
  for (size_t i = 0; i < n; ++i)
    if (bits & (1u << i)) out.emplace_back(names[i]);
  return out;
}
}  // namespace

const char* posName(uint8_t pos) { return pos < kPosN ? kPos[pos] : "other"; }
const char* genderName(uint8_t g) { return nameAt(kGender, g); }

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
  // bit8: the Spanish gloss came from the English pivot (tools/build_library/pack.py LF_ES_PIVOT; UI "(via English)")
  static const char* const k[] = {"proper-name", "indeclinable", "deponent", "impersonal", "shared-el",
                                  "plural-only", "defective",    "has-table", "gloss-es-pivot"};
  return bitNames(flags, k, 9);
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

#if !defined(VP_HAVE_RULES)
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
#endif

std::string greekMonotonic(const std::string& text) {
#if defined(VP_HAVE_RULES)
  return vp::grc::toMonotonic(text);   // C12's table (the one the Greek engine path is tested against)
#else
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
#endif
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
  for (const std::string& f : storedFlags(r)) flags.push_back(f);   // engine flags: emoji, unknownName, song, ...
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
  for (const auto& x : out.reasons)
    if (x.kind.empty() || x.kind[0] != '_') r.reasons.push_back(vp::CueReason{x.tokenIndex, x.kind, x.text, x.data});
  if (!out.tokens.empty()) setHidden(r, kTokensKind, encodeTokens(out.tokens));
  setStoredFlags(r, out.flags);
  OrbergFacts of;
  of.percent = out.meaningPercent;
  of.missing = out.meaningMissing;
  of.original = out.original;
  setHidden(r, kOrbergKind, encodeOrberg(of));
}

// ---------------------------------------------------------------------------------------------- stored engine data
const char* const kTokensKind = "_tokens";
const char* const kFlagsKind = "_flags";
const char* const kJobKind = "_job";
const char* const kOrbergKind = "_orberg";

bool isHiddenReason(const vp::CueReason& r) { return !r.kind.empty() && r.kind[0] == '_'; }

const vp::CueReason* hiddenReason(const vp::CueRecord& r, const char* kind) {
  for (const vp::CueReason& x : r.reasons)
    if (x.kind == kind) return &x;
  return nullptr;
}

void setHidden(vp::CueRecord& r, const char* kind, std::string data) {
  auto it = std::find_if(r.reasons.begin(), r.reasons.end(), [kind](const vp::CueReason& x) { return x.kind == kind; });
  if (data.empty()) {
    if (it != r.reasons.end()) r.reasons.erase(it);
    return;
  }
  if (it != r.reasons.end()) {
    it->data = std::move(data);
    return;
  }
  vp::CueReason h;
  h.kind = kind;
  h.data = std::move(data);
  r.reasons.push_back(std::move(h));
}

bool featuresToPacked(const Features& f, uint32_t& packed) {
  feat::Features o;
  bool ok = true;
  auto take = [&ok](int i, uint8_t& slot) {
    if (i < 0) ok = false;
    else slot = static_cast<uint8_t>(i);
  };
  const int pos = f.pos == "other" ? static_cast<int>(feat::PosOther) : indexOf(kPos, f.pos);
  take(pos >= static_cast<int>(kPosN) && pos != static_cast<int>(feat::PosOther) ? -1 : pos, o.pos);
  take(indexOf(kCase, f.case_), o.case_);
  take(indexOf(kNumber, f.number), o.number);
  take(indexOf(kGender, f.gender), o.gender);
  take(indexOf(kPerson, f.person), o.person);
  take(indexOf(kTense, f.tense), o.tense);
  if (f.mood == "gerundive") {
    o.mood = feat::ParticipleMood;
    o.extra = feat::Gerundive;
  } else if (f.mood == "supine") {
    o.extra = feat::Supine;
  } else {
    take(indexOf(kMood, f.mood), o.mood);
  }
  take(indexOf(kVoice, f.voice), o.voice);
  take(indexOf(kDegree, f.degree), o.degree);
  packed = feat::pack(o);
  return ok;
}

namespace {
std::string featCode(const Features& f) {
  if (f.pos.empty() && f.case_.empty() && f.number.empty() && f.gender.empty() && f.person.empty() &&
      f.tense.empty() && f.mood.empty() && f.voice.empty() && f.degree.empty())
    return std::string();
  const int idx[9] = {indexOf(kPos, f.pos),       indexOf(kCase, f.case_), indexOf(kNumber, f.number),
                      indexOf(kGender, f.gender), indexOf(kPerson, f.person), indexOf(kTense, f.tense),
                      indexOf(kMood, f.mood),     indexOf(kVoice, f.voice), indexOf(kDegree, f.degree)};
  std::string c(9, 'a');
  for (int i = 0; i < 9; ++i) c[static_cast<size_t>(i)] = idx[i] < 0 ? '?' : static_cast<char>('a' + idx[i]);
  return c;
}
Features featFromCode(const std::string& c) {
  Features f;
  if (c.size() != 9) return f;
  auto at = [&c](int i) { return c[static_cast<size_t>(i)] == '?' ? 99u : static_cast<unsigned>(c[static_cast<size_t>(i)] - 'a'); };
  f.pos = nameAt(kPos, at(0));
  f.case_ = nameAt(kCase, at(1));
  f.number = nameAt(kNumber, at(2));
  f.gender = nameAt(kGender, at(3));
  f.person = nameAt(kPerson, at(4));
  f.tense = nameAt(kTense, at(5));
  f.mood = nameAt(kMood, at(6));
  f.voice = nameAt(kVoice, at(7));
  f.degree = nameAt(kDegree, at(8));
  return f;
}
}  // namespace

std::string encodeTokens(const std::vector<vp::rules::TokenView>& tokens) {
  json a = json::array();
  for (const vp::rules::TokenView& t : tokens) {
    const int bits = (t.unknown ? 1 : 0) | (t.fromRule ? 2 : 0);
    a.push_back(json::array({t.text, t.display == t.text ? json(0) : json(t.display), t.start, t.end,
                             t.hasLemma ? static_cast<int64_t>(t.lemmaId) : int64_t(-1), t.tier, bits,
                             featCode(t.features), t.emoji}));
  }
  return a.dump(-1, ' ', false, json::error_handler_t::replace);
}

bool decodeTokens(const std::string& data, std::vector<vp::rules::TokenView>& out) {
  out.clear();
  const json a = json::parse(data, nullptr, false);
  if (!a.is_array()) return false;
  for (const json& e : a) {
    if (!e.is_array() || e.size() < 9 || !e[0].is_string() || !e[2].is_number_integer() || !e[3].is_number_integer() ||
        !e[4].is_number_integer() || !e[5].is_number_integer() || !e[6].is_number_integer() || !e[7].is_string() ||
        !e[8].is_string())
      return false;
    vp::rules::TokenView t;
    t.text = e[0].get<std::string>();
    t.display = e[1].is_string() ? e[1].get<std::string>() : t.text;
    t.start = e[2].get<int>();
    t.end = e[3].get<int>();
    const int64_t id = e[4].get<int64_t>();
    t.hasLemma = id >= 0;
    t.lemmaId = id >= 0 ? static_cast<uint32_t>(id) : 0;
    t.tier = static_cast<uint8_t>(std::max(0, std::min(255, e[5].get<int>())));
    const int bits = e[6].get<int>();
    t.unknown = (bits & 1) != 0;
    t.fromRule = (bits & 2) != 0;
    t.features = featFromCode(e[7].get<std::string>());
    t.emoji = e[8].get<std::string>();
    out.push_back(std::move(t));
  }
  return true;
}

std::vector<std::string> storedFlags(const vp::CueRecord& r) {
  std::vector<std::string> out;
  const vp::CueReason* h = hiddenReason(r, kFlagsKind);
  if (!h) return out;
  const json a = json::parse(h->data, nullptr, false);
  if (!a.is_array()) return out;
  for (const json& f : a)
    if (f.is_string()) out.push_back(f.get<std::string>());
  return out;
}

void setStoredFlags(vp::CueRecord& r, const std::vector<std::string>& flags) {
  std::vector<std::string> keep;
  for (const std::string& f : flags)
    if (f != "cps" && f != "overflow" && f != "alternative" && std::find(keep.begin(), keep.end(), f) == keep.end())
      keep.push_back(f);   // reading speed, line overflow and alternative are recomputed by every view
  setHidden(r, kFlagsKind, keep.empty() ? std::string() : json(keep).dump());
}

std::string encodeJob(const JobFacts& j) {
  if (!j.model && !j.online && !j.orberg) return std::string();
  json o{{"m", j.model ? 1 : 0}, {"o", j.online ? 1 : 0}, {"r", j.orberg ? 1 : 0}};
  if (!j.modelHeads.empty()) o["mh"] = j.modelHeads;
  if (!j.onlineVerdicts.empty()) {
    json v = json::array();
    for (const auto& p : j.onlineVerdicts) v.push_back(json::array({p.first, p.second}));
    o["ov"] = v;
  }
  return o.dump(-1, ' ', false, json::error_handler_t::replace);
}

bool decodeJob(const std::string& data, JobFacts& out) {
  out = JobFacts();
  const json o = json::parse(data, nullptr, false);
  if (!o.is_object()) return false;
  out.model = o.value("m", 0) != 0;
  out.online = o.value("o", 0) != 0;
  out.orberg = o.value("r", 0) != 0;
  if (o.contains("mh") && o["mh"].is_array())
    for (const json& h : o["mh"])
      if (h.is_string()) out.modelHeads.push_back(h.get<std::string>());
  if (o.contains("ov") && o["ov"].is_array())
    for (const json& p : o["ov"])
      if (p.is_array() && p.size() == 2 && p[0].is_string() && p[1].is_number_integer())
        out.onlineVerdicts.emplace_back(p[0].get<std::string>(), p[1].get<int>());
  return true;
}

std::string encodeOrberg(const OrbergFacts& f) {
  if (f.percent < 0 && f.original.empty()) return std::string();
  json o{{"p", f.percent < 0 ? -1 : std::min(100, f.percent)}};
  if (!f.missing.empty()) o["m"] = f.missing;
  if (!f.original.empty()) o["o"] = f.original;
  return o.dump(-1, ' ', false, json::error_handler_t::replace);
}

bool decodeOrberg(const std::string& data, OrbergFacts& out) {
  out = OrbergFacts();
  const json o = json::parse(data, nullptr, false);
  if (!o.is_object()) return false;
  if (o.contains("p") && o["p"].is_number_integer()) out.percent = std::max(-1, std::min(100, o["p"].get<int>()));
  if (o.contains("m") && o["m"].is_array())
    for (const json& m : o["m"])
      if (m.is_string()) out.missing.push_back(m.get<std::string>());
  if (o.contains("o") && o["o"].is_string()) out.original = o["o"].get<std::string>();
  return true;
}

// ---------------------------------------------------------------------------------------------- Orbergise original
std::vector<std::string> alignOriginal(const std::vector<TimedText>& cues, const std::vector<TimedText>& original) {
  std::vector<std::string> out(cues.size());
  bool timed = !cues.empty() && !original.empty();
  for (const TimedText& t : cues) timed = timed && t.timed;
  for (const TimedText& t : original) timed = timed && t.timed;
  if (cues.size() == original.size() || !timed) {   // 1. by cue index
    for (size_t i = 0; i < out.size() && i < original.size(); ++i) out[i] = original[i].text;
    return out;
  }
  // 2. by time overlap (both lists are in file order; a cue scan over all originals keeps this simple and exact)
  for (size_t i = 0; i < cues.size(); ++i) {
    const int64_t s = cues[i].start, e = cues[i].end;
    int64_t bestOv = 0;
    size_t best = original.size();
    for (size_t j = 0; j < original.size(); ++j) {
      const int64_t ov = std::min(e, original[j].end) - std::max(s, original[j].start);
      if (ov <= 0) continue;
      const int64_t shorter = std::min(e - s, original[j].end - original[j].start);
      if (ov * 2 >= shorter) {
        if (!out[i].empty()) out[i] += ' ';
        out[i] += original[j].text;
      }
      if (ov > bestOv) {
        bestOv = ov;
        best = j;
      }
    }
    if (out[i].empty() && best < original.size()) out[i] = original[best].text;
  }
  return out;
}

std::string detectOriginalLang(const std::vector<std::string>& texts) {
  static const char* const kEs[] = {"el",  "la",  "los", "las",  "que",  "de",   "del",  "y",    "es",   "un",
                                    "una", "por", "con", "para", "pero", "muy",  "su",   "sus",  "lo",   "le",
                                    "se",  "al",  "mi",  "tu",   "yo",   "este", "esta", "como", "cuando", "donde"};
  static const char* const kEn[] = {"the", "and",  "is",   "of",  "to",   "you",  "it",   "that", "he",   "she",
                                    "we",  "they", "are",  "was", "with", "for",  "this", "my",   "your", "have",
                                    "has", "not",  "what", "i",   "in",   "on",   "at",   "be",   "will", "do"};
  int es = 0, en = 0;
  for (const std::string& t : texts) {
    std::string w;
    auto word = [&]() {
      if (w.empty()) return;
      for (const char* x : kEs) es += w == x ? 1 : 0;
      for (const char* x : kEn) en += w == x ? 1 : 0;
      w.clear();
    };
    for (size_t i = 0; i < t.size();) {
      const char32_t c = vp::text::decodeUtf8(t, i);
      switch (c) {
        case 0xBF: case 0xA1: case 0xF1: case 0xD1:   // ¿ ¡ ñ Ñ
          es += 3;
          break;
        case 0xE1: case 0xE9: case 0xED: case 0xF3: case 0xFA: case 0xC1: case 0xC9: case 0xCD: case 0xD3: case 0xDA:
          es += 1;   // á é í ó ú
          break;
        default:
          break;
      }
      if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c >= 0xC0) {
        if (c < 0x80) w.push_back(static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c));
        else w += "#";   // a non-ASCII letter: no function word above contains one
      } else {
        word();
      }
    }
    word();
  }
  return es > en ? "es" : "en";
}

// ---------------------------------------------------------------------------------------------- reasons for the UI
namespace {
// Words of a text (letters, digits, apostrophes inside a word), in order.
std::vector<std::string> words(const std::string& text) {
  std::vector<std::string> out;
  std::string w;
  for (size_t i = 0; i < text.size();) {
    const size_t at = i;
    const char32_t cp = vp::text::decodeUtf8(text, i);
    const bool letter = cp >= 0x80 ? !(cp == 0x2014 || cp == 0x2013 || cp == 0x00AB || cp == 0x00BB || cp == 0x201C ||
                                       cp == 0x201D || cp == 0x00BF || cp == 0x00A1 || cp == 0x2026 || cp == 0x266A ||
                                       cp == 0x00B7 || cp == 0x0387 || cp == 0x00A0)
                                   : ((cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') || (cp >= '0' && cp <= '9') ||
                                      ((cp == '\'' || cp == '-') && !w.empty()));
    if (letter) {
      w.append(text, at, i - at);
    } else if (!w.empty()) {
      while (!w.empty() && (w.back() == '\'' || w.back() == '-')) w.pop_back();
      if (!w.empty()) out.push_back(w);
      w.clear();
    }
  }
  while (!w.empty() && (w.back() == '\'' || w.back() == '-')) w.pop_back();
  if (!w.empty()) out.push_back(w);
  return out;
}

// The first "quoted" part of an engine reason text ("\"girl\" -> puella (score 0.82)" -> girl).
std::string quoted(const std::string& t) {
  const size_t a = t.find('"');
  if (a == std::string::npos) return std::string();
  const size_t b = t.find('"', a + 1);
  return b == std::string::npos ? std::string() : t.substr(a + 1, b - a - 1);
}

json contextWords(const std::string& source, const std::string& word) {
  const std::vector<std::string> ws = words(source);
  const std::vector<std::string> target = words(word);
  const std::string first = target.empty() ? std::string() : vp::text::lower(target[0]);
  json out = json::array();
  size_t at = ws.size();
  for (int pass = 0; pass < 2 && at == ws.size(); ++pass)   // the word itself, then an inflected form ("see" -> "sees")
    for (size_t i = 0; i < ws.size(); ++i) {
      const std::string w = vp::text::lower(ws[i]);
      if (pass == 0 ? w == first : (first.size() >= 3 && w.compare(0, first.size(), first) == 0)) {
        at = i;
        break;
      }
    }
  if (at == ws.size()) return out;
  const size_t span = target.empty() ? 1 : target.size();
  for (size_t i = at >= 2 ? at - 2 : 0; i < std::min(ws.size(), at + span + 2); ++i)
    if (i < at || i >= at + span) out.push_back(ws[i]);
  return out;
}

const char* bandOf(int tier) { return tier == 1 ? "common" : tier == 2 ? "rarer" : "rare"; }

bool lemmaOk(const vp::lex::Lexicon* lx, uint32_t id) { return lx && lx->lemma(id).id != vp::lex::kNoLemma; }

// The form of `lemma` with the features of `tok` (Latin: morph::generate; "" when the cell does not exist).
std::string formLike(const vp::lex::Lexicon& lx, uint32_t lemma, const vp::rules::TokenView& tok, bool latin) {
#if defined(VP_HAVE_RULES)
  uint32_t packed = 0;
  if (!latin || tok.features.pos.empty()) return std::string();
  featuresToPacked(tok.features, packed);
  std::string out;
  if (vp::morph::generate(lx, lemma, feat::unpack(packed), out, true)) return vp::morph::displayForm(out, true);
#else
  (void)lx;
  (void)lemma;
  (void)tok;
  (void)latin;
#endif
  return std::string();
}

json reasonOut(int tokenIndex, const std::string& kind, const std::string& text, json data) {
  return json{{"tokenIndex", tokenIndex}, {"kind", kind}, {"text", text}, {"data", std::move(data)}};
}
}  // namespace

json reasonsView(const vp::CueRecord& r, const std::vector<vp::rules::TokenView>& tokens, const ReasonViewCtx& ctx) {
  json out = json::array();
  JobFacts job;
  if (const vp::CueReason* h = hiddenReason(r, kJobKind)) decodeJob(h->data, job);
  const vp::lex::Lexicon* lx = ctx.lex;
  auto tok = [&tokens](int k) -> const vp::rules::TokenView* {
    return k >= 0 && static_cast<size_t>(k) < tokens.size() ? &tokens[static_cast<size_t>(k)] : nullptr;
  };
  bool hasChange = false;
  for (const vp::CueReason& x : r.reasons) {
    if (isHiddenReason(x)) continue;
    json data = nullptr;
    if (!x.data.empty()) {
      data = json::parse(x.data, nullptr, false);
      if (data.is_discarded()) data = x.data;
    }
    const vp::rules::TokenView* t = tok(x.tokenIndex);
    if (data.is_object()) {   // already a UI shape (phrasebook {pattern, latin, tier}, orbergise {was, now, why})
      if (data.contains("was")) hasChange = true;
      out.push_back(reasonOut(x.tokenIndex, x.kind, x.text, std::move(data)));
      continue;
    }
    if (x.kind == "sense") {
      json d{{"source", quoted(x.text)}};
      if (t && t->hasLemma && lemmaOk(lx, t->lemmaId)) {
        const vp::lex::Lemma l = lx->lemma(t->lemmaId);
        d["sense"] = std::string(l.glossEn);
        d["senseEs"] = std::string(l.glossEs);
      }
      d["context"] = contextWords(ctx.source, d["source"].get<std::string>());
      out.push_back(reasonOut(x.tokenIndex, x.kind, x.text, std::move(d)));
    } else if (x.kind == "candidate" && data.is_array()) {
      const uint32_t chosenId = t && t->hasLemma ? t->lemmaId : vp::lex::kNoLemma;
      size_t chosen = 0;
      for (size_t i = 0; i < data.size(); ++i)
        if (data[i].is_object() && data[i].value("lemma", int64_t(-1)) == static_cast<int64_t>(chosenId)) {
          chosen = i;
          break;
        }
      std::vector<size_t> order{chosen};
      for (size_t i = 0; i < data.size(); ++i)
        if (i != chosen) order.push_back(i);
      for (size_t i : order) {
        const json& c = data[i];
        if (!c.is_object()) continue;
        const int64_t id = c.value("lemma", int64_t(-1));
        const bool isChosen = i == chosen;
        json d{{"lemmaId", id}, {"head", c.value("head", std::string())}, {"chosen", isChosen},
               {"score", c.value("score", 0.0)}};
        int tier = 0;
        std::string form, gloss;
        if (id >= 0 && lemmaOk(lx, static_cast<uint32_t>(id))) {
          const vp::lex::Lemma l = lx->lemma(static_cast<uint32_t>(id));
          d["head"] = std::string(l.head);
          tier = l.tier;
          gloss = std::string(l.glossEn);
          if (!isChosen && t) form = formLike(*lx, static_cast<uint32_t>(id), *t, ctx.latin);
        }
        if (isChosen && t) {
          form = t->text;
          if (t->tier) tier = t->tier;
        }
        d["form"] = form;
        d["tier"] = tier;
        d["band"] = bandOf(tier);
        d["gloss"] = gloss;
        out.push_back(reasonOut(x.tokenIndex, "candidate", c.value("why", std::string()), std::move(d)));
      }
    } else if (x.kind == "form") {
      json d = json::object();
      if (t && !t->features.pos.empty()) d["features"] = featureJson(t->features);
      if (data.is_string() && !data.get<std::string>().empty()) d["form"] = data;
      out.push_back(reasonOut(x.tokenIndex, x.kind, x.text, std::move(d)));
    } else if (x.kind == "name") {
      out.push_back(reasonOut(x.tokenIndex, x.kind, x.text, json{{"form", data.is_string() ? data : json(t ? t->text : "")}}));
    } else if (x.kind == "correction") {
      out.push_back(reasonOut(x.tokenIndex, x.kind, x.text, json{{"target", data.is_string() ? data : json("")}}));
    } else if (x.kind == "evidence" && (x.text.rfind("model:", 0) == 0 || x.text.rfind("online:", 0) == 0)) {
      const bool model = x.text[0] == 'm';
      const bool off = x.text.size() >= 5 && x.text.compare(x.text.size() - 5, 5, ": off") == 0;
      std::string state = off ? "off" : "yes";
      json d{{"source", model ? "model" : "online"}};
      if (!model && !off) {
        const std::string v = data.is_string() ? data.get<std::string>() : data.is_number() ? data.dump() : "0";
        state = v == "1" ? "yes" : v == "-1" ? "no" : "none";
      }
      if (model && x.text.find(": chose ") != std::string::npos) d["preferred"] = true;
      d["state"] = state;
      out.push_back(reasonOut(x.tokenIndex, x.kind, x.text, std::move(d)));
    } else {
      out.push_back(reasonJson(x));
    }
  }
  // Orbergise: words that replaced the input's words, when the engine did not say so itself (the stub; the rules
  // engine reports its changes as reasons of kind "orbergise" {was, now, why} and its meaning facts in "_orberg").
  if (job.orberg && !hasChange && !hiddenReason(r, kOrbergKind)) {
    for (const WordChange& c : wordChanges(ctx.source, tokens)) {
      const vp::rules::TokenView* t = tok(c.tokenIndex);
      json d{{"was", c.was}, {"now", c.now}, {"why", "vocabulary"}, {"chosen", true}};
      if (t && t->hasLemma && lemmaOk(lx, t->lemmaId)) {
        const vp::lex::Lemma l = lx->lemma(t->lemmaId);
        d["lemmaId"] = t->lemmaId;
        d["head"] = std::string(l.head);
        d["tier"] = t->tier ? t->tier : l.tier;
      }
      out.push_back(reasonOut(c.tokenIndex, "candidate", c.was + " -> " + c.now, std::move(d)));
    }
  }
  // Evidence rows for every dictionary word: the offline sources from the lexicon, engines ii/iii from the job.
  if (lx) {
    for (size_t k = 0; k < tokens.size(); ++k) {
      const vp::rules::TokenView& t = tokens[k];
      if (!t.hasLemma || !lemmaOk(lx, t.lemmaId)) continue;
      const vp::lex::Lemma l = lx->lemma(t.lemmaId);
      const std::string head(l.head);
      const int ti = static_cast<int>(k);
      out.push_back(reasonOut(ti, "evidence", "", json{{"source", "wiktionary"}, {"state", l.glossEn.empty() ? "none" : "yes"}}));
      out.push_back(reasonOut(ti, "evidence", "",
                              json{{"source", "whitaker"}, {"state", l.whitFreq >= 'A' && l.whitFreq <= 'Z' ? "yes" : "none"}}));
      std::string ms = "off";
      if (job.model) ms = std::find(job.modelHeads.begin(), job.modelHeads.end(), head) != job.modelHeads.end() ? "yes" : "none";
      out.push_back(reasonOut(ti, "evidence", "", json{{"source", "model"}, {"state", ms}}));
      std::string os = "off";
      if (job.online) {
        os = "none";
        for (const auto& v : job.onlineVerdicts)
          if (v.first == head) os = v.second > 0 ? "yes" : v.second < 0 ? "no" : "none";
      }
      out.push_back(reasonOut(ti, "evidence", "", json{{"source", "online"}, {"state", os}}));
    }
  }
  return out;
}

std::vector<WordChange> wordChanges(const std::string& latinSource, const std::vector<vp::rules::TokenView>& tokens) {
  std::vector<WordChange> out;
  const std::vector<std::string> src = words(latinSource);
  std::vector<std::string> a, b;
  std::vector<int> bTok;
  for (const std::string& w : src) a.push_back(vp::text::latin_key(w));
  for (size_t k = 0; k < tokens.size(); ++k) {
    const std::string key = vp::text::latin_key(tokens[k].text);
    if (key.empty()) continue;
    b.push_back(key);
    bTok.push_back(static_cast<int>(k));
  }
  const size_t n = a.size(), m = b.size();
  if (!n || !m || n * m > 40000) return out;
  std::vector<std::vector<uint16_t>> L(n + 1, std::vector<uint16_t>(m + 1, 0));
  for (size_t i = n; i-- > 0;)
    for (size_t j = m; j-- > 0;)
      L[i][j] = a[i] == b[j] ? static_cast<uint16_t>(L[i + 1][j + 1] + 1) : std::max(L[i + 1][j], L[i][j + 1]);
  size_t i = 0, j = 0;
  std::vector<size_t> gapA, gapB;
  auto flushGap = [&]() {
    for (size_t g = 0; g < std::min(gapA.size(), gapB.size()); ++g)
      out.push_back(WordChange{bTok[gapB[g]], src[gapA[g]], tokens[static_cast<size_t>(bTok[gapB[g]])].text});
    gapA.clear();
    gapB.clear();
  };
  while (i < n && j < m) {
    if (a[i] == b[j]) {
      flushGap();
      ++i;
      ++j;
    } else if (L[i + 1][j] >= L[i][j + 1]) {
      gapA.push_back(i++);
    } else {
      gapB.push_back(j++);
    }
  }
  while (i < n) gapA.push_back(i++);
  while (j < m) gapB.push_back(j++);
  flushGap();
  return out;
}

Meaning meaningCheck(const vp::lex::Lexicon& la, const std::string& before, const std::string& after) {
  auto content = [&la](const std::string& text, std::vector<uint32_t>& ids) {
    std::vector<vp::lex::Analysis> an;
    for (const std::string& w : words(text)) {
      an.clear();
      if (!la.lookup(vp::text::latin_key(w), an) || an.empty()) continue;
      const vp::lex::Lemma l = la.lemma(an[0].lemma);
      if (l.pos == feat::Noun || l.pos == feat::Verb || l.pos == feat::Adj || l.pos == feat::Adv)
        if (std::find(ids.begin(), ids.end(), an[0].lemma) == ids.end()) ids.push_back(an[0].lemma);
    }
  };
  std::vector<uint32_t> a, b;
  content(before, a);
  content(after, b);
  Meaning m;
  for (uint32_t id : a)
    if (std::find(b.begin(), b.end(), id) == b.end()) {
      std::string head(la.lemma(id).head);
      const size_t comma = head.find(',');
      m.missing.push_back(comma == std::string::npos ? head : head.substr(0, comma));
    }
  if (!a.empty())
    m.percent = static_cast<int>(std::lround(100.0 * static_cast<double>(a.size() - m.missing.size()) /
                                             static_cast<double>(a.size())));
  return m;
}

std::vector<vp::CueReason> remapReasons(const std::vector<vp::CueReason>& reasons,
                                        const std::vector<vp::rules::TokenView>& oldTokens,
                                        const std::vector<vp::rules::TokenView>& newTokens) {
  std::vector<vp::CueReason> out;
  for (const vp::CueReason& r : reasons) {
    if (r.tokenIndex < 0 || isHiddenReason(r)) {
      out.push_back(r);
      continue;
    }
    if (static_cast<size_t>(r.tokenIndex) >= oldTokens.size()) continue;
    const std::string& text = oldTokens[static_cast<size_t>(r.tokenIndex)].text;
    int nth = 0;
    for (int k = 0; k < r.tokenIndex; ++k)
      if (oldTokens[static_cast<size_t>(k)].text == text) ++nth;
    for (size_t k = 0; k < newTokens.size(); ++k)
      if (newTokens[k].text == text && nth-- == 0) {
        vp::CueReason x = r;
        x.tokenIndex = static_cast<int>(k);
        out.push_back(std::move(x));
        break;
      }
  }
  return out;
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
