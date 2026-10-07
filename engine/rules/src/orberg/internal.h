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
// C27: a confirmed same-sense pair (the only swaps that may stay OK): `key` (latin_key of the word's head) -> `to`
// (a head, or a short periphrasis around one inflected head like "celeriter eō"), `pos` "-" or a tiers_la.tsv pos.
struct SynRow { std::string key, pos, to, note; };
// C27: a fixed phrase / idiom that is never split or swapped. Elements are latin keys (alternatives with '|') or GER
// (a gerund or gerundive); `gap` = other words allowed between two elements; anyOrder: the elements in either order.
struct FixedRow { std::vector<std::vector<std::string>> elems; int gap = 0; bool anyOrder = false; std::string name, note; };
// C27: a sense class (English / Spanish gloss phrases); a word of this class is never swapped for a word of a class
// listed in `never` (motion verbs never become change, cease or return verbs).
struct ClassRow { std::string name; std::vector<std::string> phrases; std::vector<std::string> never; };

// Kinds of a substitution recorded for the meaning check.
enum class MapKind : uint8_t {
  Reading,    // the same written word read with another reading by a rule (vēnit: veniō, not vēneō)
  Structure,  // a rule regenerated the word from the same lemma or a rule-made choice
  Phrase,     // a teacher-edited multi-word row (periphrasis "celeriter eō", pair rows): trusted as a whole
  Curated,    // a teacher-edited one-word row (syn, periphrasis_la.tsv): the meaning check still compares the glosses
  Lexicon     // a same-sense swap found in the lexicon (sense phrases, Spanish glosses, class guard)
};
struct MapEntry { uint32_t from = kNone, to = kNone; MapKind kind = MapKind::Structure; };

// A core word (or a short phrase around one inflected word) that replaces a lemma above the tier ceiling.
struct Swap {
  uint32_t lemma = kNone;          // the inflected word's lemma (kNone: no swap)
  std::string before, after;       // fixed words of a periphrasis around it ("celeriter" eō)
  std::string rule;                // "syn" | "periphrasis" | "teacher" | "synonym"
  std::string why;                 // "same sense: see, watch"
  bool confirmed = false;          // a syn row of simplify_la.tsv: the swap may stay OK (C27)
  std::vector<std::string> evidence;   // English words of the swap's gloss (matched against the original)
};

// Normalised gloss phrases ("to go, walk; rush (fast)" -> go | walk | rush); English drops a leading "to "/"a "/"the ".
std::vector<std::string> glossPhrases(std::string_view gloss, bool english, size_t max);

}  // namespace vp::orberg::detail

namespace vp::orberg {

struct Resources::Impl {
  Impl(const lex::Lexicon& l, const curated::CuratedData& c) : la(l), cd(c) {}
  const lex::Lexicon& la;
  const curated::CuratedData& cd;
  std::vector<detail::RuleRow> rules;
  std::vector<detail::PairRow> pairs;
  std::vector<detail::KeepRow> keep;
  std::vector<detail::SynRow> syn;
  std::vector<detail::FixedRow> fixed;
  std::vector<detail::ClassRow> classes;
  std::vector<std::string> warnings;

  // bounded caches (cleared when they reach kCacheCap entries)
  static constexpr size_t kCacheCap = 4096;
  std::map<uint32_t, uint32_t> verbOfPart;               // participle lemma -> verb lemma
  std::map<uint64_t, std::vector<detail::Swap>> swaps;   // (lemma << 8 | ceiling << 1 | keepNames) -> ranked swaps
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
  // Swap for a lemma above the ceiling: syn rows of simplify_la.tsv (confirmed), periphrasis_la.tsv, then teacher
  // glosses of tiers_la.tsv and the lexicon's senses, kept only when they are the SAME SENSE (sameSense()).
  // lemma == kNone when there is none or none is needed. `evidence` (words of the original-language cue, lower case)
  // ranks the candidates whose gloss names one of them first; `tr` gives the curated glosses (may be null).
  detail::Swap swapFor(uint32_t lemma, int ceiling, bool keepNames, const std::vector<std::string>* evidence = nullptr,
                       const la2x::Translator* tr = nullptr);
  // Same sense of two lemmas of one part of speech (C27): an English gloss phrase shared (the candidate's first
  // meaning among the word's first ones or the other way round), the Spanish glosses sharing a phrase when both have
  // real (non-pivot) ones, else both English first meanings shared; no class conflict (sense classes), same
  // transitivity and valency for verbs. `why` gets the shared phrase.
  bool sameSense(uint32_t word, uint32_t cand, const la2x::Translator* tr, std::string* why = nullptr);
  // Meaning check (C27): do the glosses of two lemmas share a phrase (English: la2x curated gloss and the lexicon's
  // first meanings; or real Spanish glosses), without a sense-class conflict? Independent of how the swap was chosen.
  bool glossSame(uint32_t a, uint32_t b, const la2x::Translator* tr);
  // Sense classes of a lemma (class rows matched against its English and Spanish gloss phrases).
  std::vector<std::string> classesOf(uint32_t lemma, const la2x::Translator* tr);
};

}  // namespace vp::orberg

namespace vp::orberg::detail {

// One sentence rewritten in place (rewrite.cpp).
struct SentenceIn {
  std::string text;                 // the sentence as written
  const OrbergOptions* opt = nullptr;
  const EngineContext* ctx = nullptr;
  bool simplifyOnly = false;        // vocabulary only (used on the original's Latin)
  bool noVocab = false;             // C27 never-nonsense fallback: structure rules only
  const std::vector<std::string>* evidence = nullptr;   // words of the original-language cue (sense evidence)
  uint8_t originalPerson = 0, originalNumber = 0;          // subject person / number the original names (0 none)
};
struct SentenceOut {
  std::string text;
  std::vector<Change> changes;      // tokenIndex: byte offset of the first new word in `text` (-1 none), resolved later
  std::vector<rules::Reason> notes; // marks without a change (acc + inf kept, structure kept)
  std::vector<std::string> flags;
  // meaning bookkeeping: input content lemmas (lemma id, or 0x80000000|hash for names) and the substitutions made
  std::vector<std::pair<uint32_t, std::string>> content;   // (lemma, head shown in "missing")
  std::vector<MapEntry> mapped;                            // (input lemma, output lemma, kind) of substitutions
  std::vector<uint32_t> inputLemmas;                       // every content lemma of the input (for "prefer")
  bool changed = false;
  bool macrons = false;             // the input sentence carries length marks
  bool unconfirmed = false;         // a vocabulary swap that is not a syn row (at most Check)
  std::vector<std::string> kept;    // words above the ceiling kept on purpose (no same-sense core word, fixed phrase)
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
// The same with the best reading of each word only (the meaning check: what a reader reads).
void bestContentLemmas(const la2x::Sentence& s, Resources::Impl& R, LemmaSet& out);
// True when a reading is a content word (noun, verb other than sum, adjective, adverb, participle, name, numeral).
bool contentReading(const la2x::Reading& r, const lex::Lexicon& la);

// Small text helpers
std::string capitalise(std::string_view w);
std::string decapitalise(std::string_view w);
bool startsUpper(std::string_view w);
std::string jsonEscape(std::string_view s);
uint32_t nameId(std::string_view key);   // 0x80000000 | hash of a name key (content bookkeeping)

}  // namespace vp::orberg::detail
