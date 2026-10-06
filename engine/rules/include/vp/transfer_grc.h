// Lexical transfer EN/ES -> Attic Greek (task C12; DESIGN.md §10.2 for Greek, §1.2 Greek style, D12, D13): a
// frame::SemFrame (language neutral) becomes a grc::GrcClause / GrcNP for C9's GreekRealiser. Lexical selection
// through greek.vpl's reverse index (English keyword; Spanish "es:" keyword with an English-pivot fallback) scored as
// the Latin transfer (§10.2) plus the Greek tiers (tiers_grc.tsv, whose note column is the teacher's reverse index)
// and a bonus for lemmas shared with Modern Greek at fidelity 2/3 (LEMM flag shared_el, D12); closed classes by
// tables (articles by the frame's definiteness and rules_grc_notes.md decisions 6 and 9, pronouns, demonstratives,
// quantifiers, prepositions from preps_en_grc.tsv, conjunctions, particles from discourse relations); valency through
// the realiser (valency_grc.tsv); tense / mood / aspect per order_grc.txt tense.* and decision 8; realia (decision 1),
// "please" (decision 2) and the lexical rules of lexical_en_grc.tsv. Also the Greek cue helpers: line-break hints,
// sentence capitals (decision 5) and the polytonic -> monotonic export fallback. Deterministic; nothing throws across
// the module boundary (the engine catches at its entry points).
#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "vp/curated.h"
#include "vp/frame.h"
#include "vp/lex.h"
#include "vp/realise_grc.h"
#include "vp/result.h"
#include "vp/rules.h"
#include "vp/subs.h"
#include "vp/transfer.h"

namespace vp::grc {

// ---- curated tables of the Greek transfer ------------------------------------------------------------------------
// lexical_en_grc.tsv rows (kind, source, greek, frame, note); tiers_grc.tsv notes read as English glosses (the teacher's
// reverse index, as tiers_la.tsv for Latin); phrasebook_es_grc.tsv.
struct LexRow { std::string kind, source, greek, frame, note; };
struct TaughtGloss { std::string key, head, pos; uint8_t tier = 0; };
// readable_grc.tsv (Greek -> English / Spanish glosses for the readable sentence of grc2x): head, pos ("noun", "verb",
// "adj", "adv", "mid" = the middle voice of a verb, ...), English (verbs: base form; nouns: singular), Spanish
// (verbs: infinitive), Spanish gender (m / f / -), tags ("person", "animal", "mass", "motion", "unique").
struct ReadableRow { std::string key, head, pos, en, es, esGender, tags; };

class GreekTables {
 public:
  // Reads lexical_en_grc.tsv, tiers_grc.tsv, phrasebook_es_grc.tsv and readable_grc.tsv from `dir`. A missing lexical_en_grc.tsv is an
  // error with a hint; phrasebook_es_grc.tsv and readable_grc.tsv are optional (a warning). Malformed lines are warnings.
  static Result<GreekTables> load(const std::filesystem::path& dir);
  const std::vector<curated::LoadWarning>& warnings() const { return warnings_; }

  // First row of `kind` whose source equals `source` (lower case); `frame` narrows ("" = any).
  const LexRow* find(std::string_view kind, std::string_view source, std::string_view frame = {}) const;
  bool durative(std::string_view sourceLemma) const;   // kind durative
  // tiers_grc.tsv rows whose note lists `english` (a whole comma / semicolon separated item, lower case; a leading
  // "to", "a", "an", "the" is ignored).
  void taught(std::string_view english, std::vector<const TaughtGloss*>& out) const;
  // readable_grc.tsv read backwards: rows whose English (es = false) or Spanish (es = true) column is `word`
  // (lower case) count as teacher glosses too ("pelota" -> σφαῖρα, "cake" -> πλακοῦς).
  void taughtReadable(std::string_view word, bool es, std::vector<const TaughtGloss*>& out) const;
  const std::vector<curated::PhraseEntry>& phrasebookEs() const { return phrasebookEs_; }
  const std::vector<LexRow>& rows() const { return rows_; }
  // readable_grc.tsv row of a Greek lemma key (greek_key of the head) and part of speech ("" = the first row).
  const ReadableRow* readable(std::string_view greekKey, std::string_view pos = {}) const;

 private:
  std::vector<LexRow> rows_;
  std::vector<TaughtGloss> taught_;
  std::vector<std::pair<std::string, uint32_t>> taughtIndex_;   // gloss item -> index into taught_, sorted
  std::vector<curated::PhraseEntry> phrasebookEs_;
  std::vector<ReadableRow> readable_;   // sorted by key (stable: file order for one key)
  std::vector<TaughtGloss> readableTaught_;                         // one per readable row with a word
  std::vector<std::pair<std::string, uint32_t>> readableIndex_;      // "en:word" / "es:word" -> readableTaught_
  std::vector<curated::LoadWarning> warnings_;
};

// ---- transfer -------------------------------------------------------------------------------------------------------
struct GrcClauseOut {
  GrcClause clause;
  std::vector<transfer::Choice> choices;   // lexical decisions (lemma ids of greek.vpl)
  std::vector<int> covered;                // source tokens accounted for (A7)
  std::vector<std::string> unknownWords;   // source words without a Greek lemma (kept in brackets)
  std::vector<std::string> flags;          // "realia", "addressee-guess", "speaker-gender", "please-dropped" ...
  std::vector<rules::Reason> notes;        // sentence-level reasons (realia hypernym, dropped please ...)
  void clear() { clause = GrcClause{}; choices.clear(); covered.clear(); unknownWords.clear(); flags.clear(); notes.clear(); }
};

class GreekTransfer {
 public:
  GreekTransfer(const lex::Lexicon& grc, const curated::CuratedData& cd, const GreekData& gd, const GreekTables& gt);

  // One clause frame -> one Greek clause (subordinates, relative clauses and coordinations included).
  // `subordinate` (C16): the clause is an indirect question or another dependent clause of a phrasebook slot (verb
  // rows of frame "nonfinite" / "sub" apply: "which way you go" -> ποίαν ὁδὸν εἶ).
  void clause(const frame::SemFrame& f, const frame::SemSentence& s, const transfer::Settings& st,
              transfer::Memory& mem, GrcClauseOut& out, bool subordinate = false) const;
  // Weekday names (C16): false = the god's name in the genitive ("Ἄρεως ἡμέρα", the default), true = the ordinal
  // counted from Sunday ("τρίτη ἡμέρα", offered as an alternative by the engine).
  void setWeekdayOrdinal(bool on) const { weekdayOrdinal_ = on; }
  // A bare NP addressed to someone -> a fragment clause with the NP as a vocative.
  void vocative(const frame::SemNP& np, const frame::SemSentence& s, const transfer::Settings& st,
                transfer::Memory& mem, GrcClauseOut& out) const;
  // An NP for a phrasebook slot (the caller sets the case).
  GrcNP np(const frame::SemNP& np, const frame::SemSentence& s, const transfer::Settings& st, transfer::Memory& mem,
           GrcClauseOut& out) const;

  // Lexical selection for one source content word (REVX + scoring); `pos` a vp::feat::Pos (Noun, Verb, Adj, Adv,
  // Name). Fills `c` (candidates best first, at most 6) and returns the chosen lemma (kNone when unknown).
  uint32_t select(const std::string& sourceLemma, uint8_t pos, const std::vector<std::string>& context, bool hasObject,
                  bool personObject, const transfer::Settings& st, transfer::Choice& c, uint8_t srcGender = 0) const;

  // Greek lemma by headword (length marks ignored) and part of speech, cached (closed-class words only).
  uint32_t greek(const char* head, uint8_t pos = 0) const;
  const lex::Lexicon& lexicon() const { return lx_; }
  const GreekTables& tables() const { return gt_; }

 private:
  struct Ctx;
  void clauseInto(const frame::SemFrame& f, Ctx& c, GrcClause& out) const;
  void npInto(const frame::SemNP& n, Ctx& c, GrcNP& out) const;
  void obliqueInto(const frame::SemOblique& o, Ctx& c, GrcClause& cl) const;
  void predicateInto(const frame::SemFrame& f, Ctx& c, GrcClause& cl) const;
  uint32_t adverb(const std::string& lemma, int token, Ctx& c, bool motion, bool* front = nullptr) const;
  uint32_t lexRowLemma(const LexRow* r, uint8_t pos) const;
  uint32_t adjAdverb(const char* form) const;   // the adjective whose adverb cell is `form` (πρῶτον -> πρῶτος)
  bool fixedVerbPhrase(const std::string& greek, Ctx& c, GrcClause& cl) const;
  std::string english(const std::string& sourceLemma, const transfer::Settings& st) const;   // pivot for ES rows
  bool durative(const std::string& sourceLemma, uint32_t greekLemma, const transfer::Settings& st) const;

  const lex::Lexicon& lx_;
  const curated::CuratedData& cd_;
  const GreekData& gd_;
  const GreekTables& gt_;
  mutable std::vector<std::pair<std::string, uint32_t>> cache_;   // closed-class lookups (sorted, bounded)
  mutable bool weekdayOrdinal_ = false;
};

// ---- cue helpers ----------------------------------------------------------------------------------------------------
// Greek break hints: a line may start with καί, ἀλλά, ὅτι, εἰ, ἐάν, ἐπεί, ὅτε, ἵνα, ὥστε, ἐν, εἰς, ἐκ, ἐξ, πρός, ἀπό,
// μετά, διά, περί, ὑπό, παρά, ἐπί, ἤ, οὐδέ (punctuation breaks are found by subs::breakLines itself; `·` and `;`
// count as punctuation there because they end a word).
const subs::BreakHints& greekBreakHints();
// The first letter of `text` (after leading punctuation, quotes, ¿ ¡ and tags) in upper case, breathings and accents
// kept (ὁ -> Ὁ, ἆρα -> Ἆρα, ᾠδή -> ᾨδή). Text without a lower-case Greek or Latin letter first is returned unchanged.
std::string capitaliseGreek(std::string_view text);
// Export fallback (CLI option greek:"monotonic"): polytonic -> monotonic. Every accent (acute, grave, circumflex)
// becomes the acute (tonos, one per word), breathings and the iota subscript / adscript mark are dropped, the diaeresis
// is kept, length marks dropped; monosyllables lose the accent (standard monotonic) except ἤ "or" and the
// interrogatives ποῦ ποῖ πῶς πῇ τίς τί; NFC. Non-Greek text is unchanged.
std::string toMonotonic(std::string_view text);

}  // namespace vp::grc
