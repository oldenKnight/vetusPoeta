// Attic Greek -> English / Spanish (DESIGN.md §10.6, task C12), the Greek analogue of la2x (vp/la2x.h), same layers:
//  * Analysis: tokenises a Greek sentence (punctuation incl. the Greek question mark ";" and the ano teleia "·",
//    elided words, capitals), gathers every reading of every word (C9's vp::grc::analyse with the sandhi undone, the
//    closed-class tables for the article and the pronouns, names from names_grc.tsv / the project glossary; an
//    accent-insensitive reading only is flagged "accent differs" = Check), then disambiguates by constraint
//    propagation: the article is the strongest clue (article + adjective + noun agree in case, number and gender),
//    prepositions fix the case of their noun group (preps_en_grc.tsv), subject - verb number and person (a neuter
//    plural subject takes a singular verb), one finite verb per clause, valency (valency_grc.tsv), ὦ + vocative,
//    then tier and canonical readings. Every token keeps its chosen reading, the surviving alternatives and a
//    confidence.
//  * Interlinear: per word the lemma head, the features in words, the gloss in the requested language (Spanish
//    glosses through English carry the pivot flag), tier and emoji.
//  * Readable: a frame::SemFrame filled from the Greek roles (subject, object, indirect object, predicate, obliques,
//    adverbs, vocatives, wh word, negation οὐ / μή, connectors from the particles: δέ -> "and" or nothing, γάρ ->
//    "for", οὖν -> "so"), realised as plain English (SVO, articles from the Greek article, tense mapping reversed:
//    aorist -> simple past, imperfect -> "was ...ing", perfect -> present perfect, do-support, questions from ";" and
//    the interrogatives) or es-MX Spanish (pretérito / imperfecto, ser / estar, clitic pronouns, personal "a", ¿?).
// Glosses come from data/curated/readable_grc.tsv first, then the teacher's tier notes, then the lexicon. Deterministic;
// nothing here throws across the module boundary (Translator::cues reports per cue).
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "vp/curated.h"
#include "vp/frame.h"
#include "vp/lex.h"
#include "vp/realise_grc.h"
#include "vp/rules.h"
#include "vp/transfer_grc.h"

namespace vp::grc2x {

enum class Target : uint8_t { En, Es };

// One reading of a Greek word.
struct Reading {
  uint32_t lemma = lex::kNoLemma;   // kNoLemma for a name without a lemma
  uint32_t packed = 0;              // vp::feat packed features
  std::string display;              // dictionary-side form (lexicon display or the written word)
  bool closed = false;              // from the built-in closed-class tables (article, pronouns)
  bool name = false;                // a name (names_grc.tsv, glossary, lexicon proper name)
  bool fromRule = false;            // paradigm fallback (lemma without a table)
  std::string nameEn;               // the English spelling of a name reading ("Alice")
  double prior = 0;                 // context-free preference (closed tables, tier, canonical, Attic)
};

enum class TokKind : uint8_t { Word, Punct, Number };

struct Token {
  std::string text;                 // as written
  int start = 0, end = 0;           // byte offsets in the sentence
  TokKind kind = TokKind::Word;
  bool capitalised = false, unknown = false, nameGuess = false, accentDiffers = false, fromRule = false;
  std::vector<Reading> readings;    // after analyse(): the chosen reading first
  int clause = 0;
  double confidence = 1.0;
  int surviving = 1;
  std::vector<std::string> why;     // constraints that decided ("article τόν", "after ἐν", "subject of τρέχει")
  std::string role;                 // "subject", "object", "verb", ... after the frame fill
  const Reading* best() const { return readings.empty() ? nullptr : &readings[0]; }
};

struct Sentence {
  std::string text;
  std::vector<Token> tokens;
  std::string finalPunct;           // ".", ";", "!", "" ...
  bool question = false;
  void clear() { text.clear(); tokens.clear(); finalPunct.clear(); question = false; }
};

// Interlinear word.
struct Word {
  int token = -1;
  std::string text;                 // as written
  int start = 0, end = 0;
  uint32_t lemma = lex::kNoLemma;
  std::string head;                 // dictionary form (or the name)
  rules::Features features;
  std::string featureText;          // "noun, accusative singular feminine" (English)
  std::string gloss;                // in the requested language
  bool glossPivot = false;          // Spanish gloss came through English
  uint8_t tier = 0;
  std::string emoji;
  bool unknown = false, name = false, nameGuess = false, accentDiffers = false, fromRule = false;
  double confidence = 1.0;
  std::vector<std::string> alternatives;   // other surviving readings: "ἔχεις (ἔχις, noun, nominative plural)"
  std::string role;
};

struct SentenceOut {
  std::string greek;                // the sentence as given
  std::string text;                 // readable English / Spanish
  std::vector<Word> words;          // one per word token
  double confidence = 0;
  std::vector<std::string> flags;   // "unknown", "name-guessed", "accent-differs", "ambiguous", "no-verb", ...
  std::string frame;                // frame::describe() of the clauses (tests, debugging)
  Sentence analysis;
};

class Translator {
 public:
  // The tables must outlive the translator (GreekPath owns them).
  Translator(const lex::Lexicon& grc, const curated::CuratedData& cd, const grc::GreekData& gd,
             const grc::GreekTables& gt);
  ~Translator();
  Translator(const Translator&) = delete;
  Translator& operator=(const Translator&) = delete;

  // Analysis only (tokens, readings, disambiguation).
  void analyse(std::string_view greek, Sentence& out, const std::vector<rules::GlossaryEntry>* glossary = nullptr) const;
  // One sentence: analysis, interlinear and readable text. Discourse memory (the last subject's gender for restored
  // pronouns) carries over until resetDiscourse().
  void sentence(std::string_view greek, Target, SentenceOut& out,
                const std::vector<rules::GlossaryEntry>* glossary = nullptr);
  void resetDiscourse();

  // Engine pairs grc-en / grc-es: each cue's source text is one or more Greek sentences. target = readable text;
  // tokens = the Greek words (offsets in the cue's SOURCE text; flag "source-tokens"); one "analysis" reason per word
  // (data: lemma, head, gloss, pivot, features, confidence, alternatives); checks A1 (unknown forms; accent differs
  // = warning) and "ambiguity". Never throws.
  std::vector<rules::CueOutput> cues(const std::vector<rules::CueInput>&, const rules::Options&, const rules::Context&,
                                     const std::function<void(size_t)>& progress,
                                     const std::function<bool()>& cancelled);

  // Gloss of a Greek lemma in the language (readable_grc.tsv, the teacher's tier notes, the lexicon); `pivot` = the
  // Spanish gloss came through English.
  std::string gloss(uint32_t lemma, Target, bool* pivot = nullptr) const;

  // A9 round trip (C16, decision 1 of rules_grc2_notes.md, as la2x serves Latin): the share of the source's content
  // lemmas (English or Spanish, stop words left out) found among the glosses of the Greek text's chosen readings
  // (readable_grc.tsv, the teacher's tier notes, the lexicon's senses and keywords) or among its names. 1.0 when the
  // source has no content word. Never throws (an internal error counts as 1.0).
  double roundTripOverlap(std::string_view greekText, const std::vector<std::string>& sourceLemmas,
                          Target sourceLang) const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Byte ranges [first, second) of the sentences of a Greek text (split after . ; ! ? and the ano teleia is NOT a
// sentence end).
std::vector<std::pair<size_t, size_t>> splitSentences(std::string_view text);

}  // namespace vp::grc2x
