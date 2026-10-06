// Latin -> English / Spanish (DESIGN.md §10.6, task C11). Three layers, each usable on its own:
//  * Analyser: tokenises a Latin sentence (punctuation, enclitics through morph::analyseLatin, capitals, numerals,
//    names from names_la.tsv / the project glossary / capitalised unknowns), gathers every reading of every word and
//    disambiguates by constraint propagation (a beam search over the readings scored by NP agreement, prepositions
//    fixing case, subject-verb number/person, valency, one finite verb per clause with the verb-final heuristic,
//    relative pronoun antecedents, vocatives/imperatives in address) and then by tier, frequency and canonical
//    readings. Every token keeps its chosen reading, the surviving alternatives and a confidence.
//  * Interlinear: per word the lemma head (macrons), the features in words, the gloss in the requested language
//    (Spanish glosses carry the "via English" pivot flag), tier and emoji. This is the didactic view.
//  * Readable: a frame::SemFrame filled from the Latin roles, realised as plain literal English (SVO, articles by
//    definiteness heuristics, tense mapping reversed, pronouns restored from the verb, do-support) or Spanish
//    (es-MX: SVO, articles by gender, pretérito/imperfecto, tú/ustedes, ¿?/¡!, clitic pronouns).
// Deterministic (no randomness; ties broken by lemma id, then by string). Nothing here throws across the module
// boundary: Translator::cues catches and reports per cue.
#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "vp/curated.h"
#include "vp/lex.h"
#include "vp/result.h"
#include "vp/rules.h"

namespace vp::la2x {

enum class Target : uint8_t { En, Es };

// One reading of a Latin word: a lexicon analysis, a paradigm-fallback analysis, a name or a numeral.
struct Reading {
  uint32_t lemma = lex::kNoLemma;   // kNoLemma for a guessed name, a glossary name or a numeral
  uint32_t packed = 0;              // vp::feat packed features of this reading
  std::string display;              // the form with length marks (lexicon display) or the written word
  uint16_t aflags = 0;              // ANAL flags (lex::AnalFlag)
  bool fromRule = false;            // paradigm fallback (lemma without a table)
  bool name = false;                // proper name
  std::string encl;                 // enclitic stripped for this reading ("que" | "ne" | "ue"), "" for none
  uint8_t nameGender = 0;           // vp::feat::Gender of a name reading from names_la.tsv / glossary
  double prior = 0;                 // context-free preference (tier, frequency, canonical readings)
  double score = 0;                 // best total score of a sentence analysis that uses this reading
};

enum class TokKind : uint8_t { Word, Punct, Number };

struct Token {
  std::string text;                 // as written (enclitic included)
  int start = 0, end = 0;           // byte offsets in the sentence text
  TokKind kind = TokKind::Word;
  std::string encl;                 // "que" | "ne" | "ue" when an enclitic was stripped
  bool capitalised = false, initial = false;   // first letter upper case / first word of the sentence
  bool unknown = false;             // no reading at all (kept verbatim)
  bool nameGuess = false;           // capitalised unknown word read as a name (Check)
  bool fromRule = false;
  std::vector<Reading> readings;    // after analyse(): best first
  int clause = 0;                   // clause index (Sentence::clauses)
  double confidence = 1.0;          // 1.0 unambiguous; lower with more surviving readings
  int surviving = 1;                // readings within reach of the best analysis (distinct lemma/case/number/verb form)
  std::vector<std::string> why;     // constraints that decided ("after in", "agrees with puella", "subject of amat")
  const Reading* best() const { return readings.empty() ? nullptr : &readings[0]; }
};

// A clause found by the analyser: tokens [first, last], the subordinator that opens it (token index or -1), the
// clause it depends on (-1 for a main clause) and the finite verb (token index or -1).
struct Clause {
  int first = 0, last = -1;
  int marker = -1;                  // subordinating conjunction / relative pronoun token
  int parent = -1;
  int verb = -1;
  enum Kind : uint8_t { Main, Sub, Relative, Coord } kind = Main;
};

struct Sentence {
  std::string text;
  std::vector<Token> tokens;
  std::vector<Clause> clauses;
  bool macrons = false;             // the text carries length marks: readings must match them
  std::string finalPunct;           // ".", "?", "!", "" ...
  void clear() { text.clear(); tokens.clear(); clauses.clear(); macrons = false; finalPunct.clear(); }
};

class Analyser {
 public:
  Analyser(const lex::Lexicon& la, const curated::CuratedData& cd);
  ~Analyser();
  Analyser(const Analyser&) = delete;
  Analyser& operator=(const Analyser&) = delete;
  // One sentence (or fragment). `glossary` names count as names (any case ending of the declension).
  void analyse(std::string_view sentence, Sentence& out, const std::vector<rules::GlossaryEntry>* glossary = nullptr) const;
  // Latin keys of nouns that denote persons (readable_en.tsv "person" rows): a bare ablative of a person is unlikely.
  void setPersonNouns(std::vector<std::string> keys);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Interlinear word (the didactic view; TokenView-compatible data plus the gloss).
struct Word {
  int token = -1;                   // index into Sentence::tokens
  std::string text, display;        // as written / chosen reading with macrons
  int start = 0, end = 0;           // byte offsets in the sentence text
  uint32_t lemma = lex::kNoLemma;
  std::string head;                 // dictionary form with macrons (or the name)
  rules::Features features;         // FeatureView strings (English keys the UI translates)
  std::string featureText;          // the same in words: "noun, accusative singular" (English)
  std::string gloss;                // in the requested language
  bool glossPivot = false;          // Spanish gloss came through English (UI: "(via English)")
  uint8_t tier = 0;
  std::string emoji;
  bool unknown = false, name = false, nameGuess = false, fromRule = false;
  double confidence = 1.0;
  std::vector<std::string> alternatives;   // other surviving readings: "puellā (puella, noun, ablative singular)"
  std::string role;                 // "subject", "object", "verb", "indirect object", "oblique", ... ("" unknown)
};

struct SentenceOut {
  std::string latin;                // the sentence as given
  std::string text;                 // readable English / Spanish
  std::vector<Word> words;          // interlinear, one per word token (punctuation excluded)
  double confidence = 0;            // 0..1 from the token confidences and the frame fill
  std::vector<std::string> flags;   // "unknown", "name-guessed", "ambiguous", "abl-abs", "no-verb", "gloss-missing", ...
  std::string frame;                // frame::describe() of the main clause (tests, debugging)
  Sentence analysis;
};

class Translator {
  struct Key { explicit Key() = default; };   // only create() can construct

 public:
  explicit Translator(Key);
  // Loads readable_en.tsv, readable_es.tsv and gloss_es_la.tsv from the first directory of `dirs` that holds
  // readable_en.tsv. Error not_found with a hint when none does.
  static Result<std::unique_ptr<Translator>> create(const lex::Lexicon& la, const curated::CuratedData& cd,
                                                    const std::vector<std::filesystem::path>& dirs);
  ~Translator();
  Translator(const Translator&) = delete;
  Translator& operator=(const Translator&) = delete;

  // One sentence. Discourse memory (previous mentions for articles, the last subject for restored pronouns) carries
  // over between calls until resetDiscourse().
  void sentence(std::string_view latin, Target, SentenceOut& out,
                const std::vector<rules::GlossaryEntry>* glossary = nullptr);
  // A text of one or more sentences (split at . ! ? ; keeps the order).
  void text(std::string_view latin, Target, std::vector<SentenceOut>& out,
            const std::vector<rules::GlossaryEntry>* glossary = nullptr);
  void resetDiscourse();

  // Engine pairs la-en / la-es: each cue's source text is one or more Latin sentences. target = readable text;
  // tokens = the Latin words (start/end are byte offsets in the cue's SOURCE text; flag "source-tokens"); reasons =
  // one "analysis" reason per word (data: lemma, head, gloss, pivot, features, confidence, alternatives); checks:
  // A1 (unknown forms) and "ambiguity". Never throws.
  std::vector<rules::CueOutput> cues(const std::vector<rules::CueInput>&, const rules::Options&, const rules::Context&,
                                     const std::function<void(size_t)>& progress,
                                     const std::function<bool()>& cancelled);

  // A9 (EN/ES -> LA round trip): analyses the Latin text and returns the share of the source content lemmas
  // (English or Spanish, lower case; function words are ignored) that one of the Latin content lemmas glosses, in
  // [0,1]. 1.0 when the source has no content lemma.
  double roundTripOverlap(std::string_view latinText, const std::vector<std::string>& sourceLemmas,
                          Target sourceLang = Target::En);

  // Gloss of a Latin lemma in the language (curated tables first, then the lexicon); pivot = Spanish via English.
  std::string gloss(uint32_t lemma, Target, bool* pivot = nullptr) const;
  const Analyser& analyser() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Byte ranges [first, second) of the sentences of a Latin text (split after . ! ? ; and closing quotes).
std::vector<std::pair<size_t, size_t>> splitSentences(std::string_view text);

}  // namespace vp::la2x
