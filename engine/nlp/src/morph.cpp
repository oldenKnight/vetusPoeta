// Packed morphological features (vp::nlp::morph) and the word normalisation shared with the trainers.
#include <cstring>

#include "vp/nlp.h"
#include "vp/text.h"

namespace vp::nlp {
namespace {

struct Group {
  const char* name;
  morph::Shift shift;
  const char* const* values;   // index = code (0 unused)
  uint32_t count;              // number of entries in values
};

const char* const kNumber[] = {"", "Sing", "Plur", "Dual"};
const char* const kPerson[] = {"", "1", "2", "3"};
const char* const kTense[] = {"", "Pres", "Past", "Fut", "Imp", "Pqp"};
const char* const kVerbForm[] = {"", "Fin", "Inf", "Part", "Ger", "Conv"};
const char* const kMood[] = {"", "Ind", "Sub", "Imp", "Cnd"};
const char* const kPronType[] = {"", "Prs", "Art", "Dem", "Ind", "Int", "Rel", "Neg", "Tot", "Emp", "Rcp", "Exc",
                                 "Int,Rel"};

const Group kGroups[] = {
    {"Number", morph::NumberShift, kNumber, 4},     {"Person", morph::PersonShift, kPerson, 4},
    {"Tense", morph::TenseShift, kTense, 6},        {"VerbForm", morph::VerbFormShift, kVerbForm, 6},
    {"Mood", morph::MoodShift, kMood, 5},           {"PronType", morph::PronTypeShift, kPronType, 13},
};

}  // namespace

namespace morph {

std::string toString(uint32_t feats) {
  std::string out;
  for (const Group& g : kGroups) {
    const uint32_t v = get(feats, g.shift);
    if (v == 0 || v >= g.count) continue;
    if (!out.empty()) out += '|';
    out += g.name;
    out += '=';
    out += g.values[v];
  }
  return out.empty() ? std::string("_") : out;
}

uint32_t fromString(std::string_view ud) {
  uint32_t out = 0;
  size_t pos = 0;
  while (pos <= ud.size()) {
    size_t bar = ud.find('|', pos);
    if (bar == std::string_view::npos) bar = ud.size();
    const std::string_view kv = ud.substr(pos, bar - pos);
    const size_t eq = kv.find('=');
    if (eq != std::string_view::npos) {
      const std::string_view k = kv.substr(0, eq), v = kv.substr(eq + 1);
      for (const Group& g : kGroups) {
        if (k != g.name) continue;
        for (uint32_t c = 1; c < g.count; ++c)
          if (v == g.values[c]) out = (out & ~(get(0xFFFFFFFFu, g.shift) << g.shift)) | (c << g.shift);
      }
    }
    pos = bar + 1;
  }
  return out;
}

}  // namespace morph

std::string normalise(std::string_view word) {
  std::string low = vp::text::lower(word);
  // U+2018..U+201B -> '\'', U+201C..U+201F -> '"' (UTF-8 E2 80 98..9F)
  std::string out;
  out.reserve(low.size());
  for (size_t i = 0; i < low.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(low[i]);
    if (c == 0xE2 && i + 2 < low.size() && static_cast<unsigned char>(low[i + 1]) == 0x80) {
      const unsigned char c3 = static_cast<unsigned char>(low[i + 2]);
      if (c3 >= 0x98 && c3 <= 0x9B) { out += '\''; i += 2; continue; }
      if (c3 >= 0x9C && c3 <= 0x9F) { out += '"'; i += 2; continue; }
    }
    out += static_cast<char>(c);
  }
  return out;
}

uint64_t featureHash(std::string_view feature) {
  uint64_t h = 0xCBF29CE484222325ull;
  for (unsigned char c : feature) {
    h ^= c;
    h *= 0x100000001B3ull;
  }
  return h != 0 ? h : 1;
}

}  // namespace vp::nlp
