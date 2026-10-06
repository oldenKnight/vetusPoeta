// Lexical transfer (DESIGN.md §10.2): SemFrame (language neutral, content lemmas in the source language) -> the
// Latin clause structures of realise_la.h (LaClause / LaNP), with lexical selection through the lexicon's reverse
// index (REVX) scored per §10.2, closed classes by tables, valency from valency_la.tsv, names, phrasebook pieces,
// periphrasis, tense / modality mapping (§10.3, order_la.txt RULE tense.* / modal.*). Every choice is recorded with
// its candidate list so the engine can explain it (reasons) and build alternatives. Deterministic: ties by lemma id.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "vp/curated.h"
#include "vp/frame.h"
#include "vp/lex.h"
#include "vp/realise_la.h"
#include "vp/rules.h"

namespace vp::transfer {

constexpr uint32_t kNone = lex::kNoLemma;

struct Candidate { uint32_t lemma = kNone; uint16_t sense = 0; double score = 0; std::string why; };

// One lexical decision (one source word -> one Latin lemma).
struct Choice {
  int token = -1;                  // source token (index into SemSentence::tokens)
  std::string source;              // source lemma
  uint32_t lemma = kNone;          // chosen Latin lemma (kNone: unknown or dropped)
  std::string kind;                // "sense" (REVX), "table" (closed class), "name", "phrasebook", "correction",
                                   // "periphrasis", "unknown"
  std::vector<Candidate> candidates;   // scored, best first (REVX choices only), at most 6
  double margin = 1.0;             // best - second best (1.0 when there was no competitor)
  bool unknown = false;
  bool lowTier = false;            // a tier 3 lemma was chosen while a tier 1/2 candidate of the same sense existed
  std::string note;                // human text for the reason
};

// Discourse memory carried from sentence to sentence (and cue to cue) inside one translate call.
struct Memory {
  uint8_t lastGender = 0, lastNumber = 0;   // last noun mentioned (subject or object): "it", "this one", "red ones"
  bool lastAnimate = false;                 // ... and it was a person / animal (Spanish él / ella keep their gender)
  uint32_t lastVerb = kNone;                // last main verb: "This one does."
  bool lastMotion = false;                  // that verb was a verb of motion (elliptical "where" -> quō)
  bool lastVerbObject = false;
  bool prevFirst = false;                   // the previous sentence began with "first" -> "then" is deinde
  bool addresseePlural = false;             // the previous cue addressed a group (imp.number)
  bool addresseeGuess = false;              // the current plural comes from the previous cue (Check)
  bool answerWe = false;                    // the next sentence answers with "we": "you" here is plural (guess, Check)
  bool sawFirst = false, sawPlural = false; // set while translating the current sentence
};

struct Settings {
  frame::SrcLang lang = frame::SrcLang::En;
  int fidelity = 2;
  char speakerGender = 'm';                 // 'm' | 'f' | 'u'
  bool flipSpeakerGender = false;           // alternative: the other first-person gender
  const rules::Context* context = nullptr;  // glossary and corrections
  std::vector<std::pair<int, int>> overrides;   // (token, candidate rank) forced choices for alternatives
  // Source-language lexicon (C13): for Spanish, the English pivot reads its lemma's one-line English gloss when the
  // Spanish keyword has no Latin candidate and no teacher gloss. May be null.
  const lex::Lexicon* srcLex = nullptr;
};

struct ClauseOut {
  realise::LaClause clause;
  std::vector<Choice> choices;
  std::vector<int> covered;                 // source tokens accounted for
  std::vector<std::string> unknownWords;    // source words without a Latin lemma (kept in brackets)
  std::vector<std::string> flags;           // "name-guessed", "addressee-guess", "speaker-gender", ...
  bool usedPeriphrasis = false;
  void clear() { clause = realise::LaClause{}; choices.clear(); covered.clear(); unknownWords.clear(); flags.clear();
                 usedPeriphrasis = false; }
};

class Transfer {
 public:
  Transfer(const lex::Lexicon& la, const curated::CuratedData& cd);

  // One clause frame -> one Latin clause (subordinates, relative clauses and coordinations included).
  void clause(const frame::SemFrame& f, const frame::SemSentence& s, const Settings& st, Memory& mem,
              ClauseOut& out) const;
  // A bare NP addressed to someone (vocative unit) -> a fragment clause with the NP in the vocative.
  void vocative(const frame::SemNP& np, const frame::SemSentence& s, const Settings& st, Memory& mem,
                ClauseOut& out) const;
  // An NP for a phrasebook slot (case is applied by the caller).
  realise::LaNP np(const frame::SemNP& np, const frame::SemSentence& s, const Settings& st, Memory& mem,
                   ClauseOut& out) const;

  // Lexical selection for one source content word (REVX + scoring). `pos` is a vp::feat::Pos (Noun, Verb, Adj,
  // Adv, Name). `context` = the other content lemmas of the clause (sense-keyword overlap); `hasObject` /
  // `personObject` feed the verb sense tags. Fills `c` (candidates best first) and returns the chosen lemma.
  // `srcGender` (Spanish nouns): a Latin noun of the same gender gets +0.15.
  uint32_t select(const std::string& sourceLemma, uint8_t pos, const std::vector<std::string>& context,
                  bool hasObject, bool personObject, const Settings& st, Choice& c, uint8_t srcGender = 0) const;

  // Closed-class lemma lookups (by Latin headword and part of speech), resolved at construction.
  uint32_t latin(const char* head, uint8_t pos = 0) const;

  const lex::Lexicon& lexicon() const { return la_; }

 private:
  struct Ctx;
  void clauseInto(const frame::SemFrame& f, Ctx& c, realise::LaClause& out) const;
  void npInto(const frame::SemNP& n, Ctx& c, realise::LaNP& out) const;
  bool obliqueInto(const frame::SemOblique& o, Ctx& c, realise::LaClause& cl) const;
  void predicateInto(const frame::SemFrame& f, Ctx& c, realise::LaClause& cl) const;
  uint32_t adverb(const std::string& lemma, int token, Ctx& c, bool motion) const;
  bool pluralOnly(uint32_t lemma) const;
  bool latinVerbPhrase(const std::string& latin, realise::LaClause& rc) const;
  bool nameTableInto(const frame::SemNP& n, Ctx& c, realise::LaNP& o) const;          // C15: names_la.tsv phrases
  bool latinNameNP(const std::string& latin, realise::LaNP& o) const;                 // C15: "Leō Timidus" as an NP
  bool titleNoun(const frame::SemNP& n, Ctx& c, realise::LaNP& o) const;              // C15: "the kind Stork"
  void relativeInto(const frame::SemNP& n, Ctx& c, realise::LaNP& o) const;          // C15: shared by names
  bool deponentActive(const frame::SemFrame& in, Ctx& c, frame::SemFrame& out) const; // C15: deponent passives

  const lex::Lexicon& la_;
  const curated::CuratedData& cd_;
  mutable std::vector<std::pair<std::string, uint32_t>> cache_;   // closed-class lookups (sorted, bounded)
};

// Is the source noun a person (pronouns, names, a small list of animate nouns)? Used for "to" -> dative and
// "with" -> cum.
bool animate(const frame::SemNP& np);

}  // namespace vp::transfer
