// Internal pieces of the Orbergise module (not a public header): simplify_la.tsv tables, tier and lemma helpers,
// the vocabulary-swap search and the sentence rewriter. Deterministic; nothing throws on bad data.
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "vp/curated.h"
#include "vp/features.h"
#include "vp/la2x.h"
#include "vp/lex.h"
#include "vp/orberg.h"

namespace vp::orberg::detail {

constexpr uint32_t kNone = lex::kNoLemma;

// ---- simplify_la.tsv -------------------------------------------------------------------------------------------------
struct RuleRow { std::string id; bool on = true; std::vector<std::pair<std::string, std::string>> params; std::string note; };
struct PairRow { std::vector<std::string> keys; std::string to; bool agree = false; std::string note; };
struct KeepRow { std::string key, pos; };

// A core word (or a short phrase around one inflected word) that replaces a lemma above the tier ceiling.
struct Swap {
  uint32_t lemma = kNone;          // the inflected word's lemma (kNone: no swap)
  std::string before, after;       // fixed words of a periphrasis around it ("celeriter" eō)
  std::string rule;                // "periphrasis" | "teacher" | "synonym"
  std::string why;                 // "same sense: see, watch"
};

}  // namespace vp::orberg::detail

namespace vp::orberg {

struct Resources::Impl {
  Impl(const lex::Lexicon& l, const curated::CuratedData& c) : la(l), cd(c) {}
  const lex::Lexicon& la;
  const curated::CuratedData& cd;
  std::vector<detail::RuleRow> rules;
  std::vector<detail::PairRow> pairs;
  std::vector<detail::KeepRow> keep;
  std::vector<std::string> warnings;

  // bounded caches (cleared when they reach kCacheCap entries)
  static constexpr size_t kCacheCap = 4096;
  std::map<uint32_t, uint32_t> verbOfPart;               // participle lemma -> verb lemma
  std::map<uint64_t, detail::Swap> swaps;                // (lemma << 8 | ceiling << 1 | keepNames) -> swap
  std::map<std::string, uint32_t> found;                 // "head|pos" -> lemma
  std::vector<lex::Candidate> candBuf;
  std::vector<lex::Sense> senseBuf, senseBuf2;
  std::vector<lex::Analysis> anaBuf;

  // rules
  const detail::RuleRow* rule(std::string_view id) const;
  bool on(std::string_view id) const { const detail::RuleRow* r = rule(id); return r && r->on; }
  std::string param(std::string_view id, std::string_view name, std::string_view def) const;
  bool keepLemma(uint32_t lemma) const;

  // lexicon helpers
  uint32_t find(std::string_view head, uint8_t pos);     // morph::findLemma, cached
  uint8_t tier(uint32_t lemma) const;                     // effective tier (curated wins), 3 when none
  bool isParticipleLemma(uint32_t lemma) const;
  uint32_t verbOf(uint32_t participleLemma);              // kNone when not found
  uint8_t wordTier(uint32_t lemma);                       // participles count with their verb's tier
  // Swap for a lemma above the ceiling (periphrasis_la.tsv, teacher glosses of tiers_la.tsv, then the lexicon's
  // sense keywords with the same part of speech). lemma == kNone when there is none or none is needed.
  detail::Swap swapFor(uint32_t lemma, int ceiling, bool keepNames);
};

}  // namespace vp::orberg

namespace vp::orberg::detail {

// One sentence rewritten in place (rewrite.cpp).
struct SentenceIn {
  std::string text;                 // the sentence as written
  const OrbergOptions* opt = nullptr;
  const EngineContext* ctx = nullptr;
  bool simplifyOnly = false;        // vocabulary only (used on the original's Latin)
};
struct SentenceOut {
  std::string text;
  std::vector<Change> changes;      // tokenIndex: byte offset of the first new word in `text` (-1 none), resolved later
  std::vector<rules::Reason> notes; // marks without a change (acc + inf kept, structure kept)
  std::vector<std::string> flags;
  // meaning bookkeeping: input content lemmas (lemma id, or 0x80000000|hash for names) and the substitutions made
  std::vector<std::pair<uint32_t, std::string>> content;   // (lemma, head shown in "missing")
  std::vector<std::pair<uint32_t, uint32_t>> mapped;       // (input lemma, output lemma) of swaps and pairs
  std::vector<uint32_t> inputLemmas;                       // every content lemma of the input (for "prefer")
  bool changed = false;
  bool macrons = false;             // the input sentence carries length marks
};
void rewriteSentence(const SentenceIn& in, Resources::Impl& R, SentenceOut& out);

// Content lemmas of a Latin text as the analyser reads it (every reading; participles add their verb; names by key).
struct LemmaSet {
  std::vector<uint32_t> lemmas;    // sorted, unique
  std::vector<std::string> names;  // latin keys of names, sorted
  bool has(uint32_t l) const;
  bool hasName(const std::string& k) const;
};
void contentLemmas(const la2x::Sentence& s, Resources::Impl& R, LemmaSet& out);
// True when a reading is a content word (noun, verb other than sum, adjective, adverb, participle, name, numeral).
bool contentReading(const la2x::Reading& r, const lex::Lexicon& la);

// Small text helpers
std::string capitalise(std::string_view w);
std::string decapitalise(std::string_view w);
bool startsUpper(std::string_view w);
std::string jsonEscape(std::string_view s);
uint32_t nameId(std::string_view key);   // 0x80000000 | hash of a name key (content bookkeeping)

}  // namespace vp::orberg::detail
