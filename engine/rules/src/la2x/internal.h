// Internal pieces of la2x (not a public header): readable tables, English/Spanish word morphology, the Latin frame
// builder and the two realisers. Everything deterministic; nothing throws on bad data (empty strings instead).
#pragma once
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

#include "vp/features.h"
#include "vp/frame.h"
#include "vp/la2x.h"
#include "vp/lex.h"

namespace vp::la2x::detail {

// ---- readable_en.tsv / readable_es.tsv ------------------------------------------------------------------------------
// Columns: kind, latin, feature, text, note. `latin` is a latin_key (or a person code for pronoun rows); `feature`
// narrows the row ("abl", "nom.m.sg", "subj", "-" = any). Lookups: exact (kind, latin, feature), in the order the
// caller lists the features, then feature "-".
struct Row { std::string kind, latin, feature, text, note; };

class Tables {
 public:
  bool load(const std::filesystem::path& file, std::string& error);
  const Row* find(std::string_view kind, std::string_view latin, std::string_view feature) const;
  const Row* best(std::string_view kind, std::string_view latin, std::initializer_list<std::string_view> features) const;
  bool has(std::string_view kind, std::string_view latin) const;
  // text of best(), or "" when there is none
  std::string text(std::string_view kind, std::string_view latin, std::initializer_list<std::string_view> features) const;
  // note contains the whole word `tag` (space or comma separated)
  bool tagged(std::string_view kind, std::string_view latin, std::string_view tag) const;
  const std::vector<Row>& rows() const { return rows_; }

 private:
  std::vector<Row> rows_;   // sorted by (kind, latin, feature); duplicates: first row of the file wins
};

struct GlossEsRow { std::string key, head, gloss; };
bool loadGlossEs(const std::filesystem::path& file, std::vector<GlossEsRow>& out);

bool noteHas(std::string_view note, std::string_view tag);

// ---- English morphology ------------------------------------------------------------------------------------------------
namespace en {
enum class VForm : uint8_t { Base, S3, Past, PastPart, Ing };
std::string verb(std::string_view base, VForm f);           // multi-word: the first word inflects ("look at")
std::string plural(std::string_view noun);                   // multi-word: the last word inflects
std::string comparative(std::string_view adj);               // "taller", "more beautiful"
std::string superlative(std::string_view adj);               // "tallest", "most beautiful"
std::string indefinite(std::string_view nextWord);           // "a" | "an"
}  // namespace en

// ---- Spanish morphology ------------------------------------------------------------------------------------------------
namespace es {
enum class VTense : uint8_t { Present, Imperfect, Preterite, Future, Conditional, SubjPresent, SubjImperfect,
                              Imperative, PastPart, Gerund, Infinitive };
// person 1..3, number 1 sg / 2 pl. Imperative: person 2 (tú, ustedes = 3rd plural subjunctive). Multi-word glosses
// ("estar sentado") inflect the first word; a reflexive infinitive ("sentarse") gives the bare form (the caller
// places the clitic).
std::string verb(std::string_view infinitive, VTense t, int person, int number);
bool reflexive(std::string_view infinitive);                 // ends in -se ("levantarse")
std::string unreflexive(std::string_view infinitive);        // "levantarse" -> "levantar"
std::string plural(std::string_view noun);
std::string adjective(std::string_view masculine, uint8_t gender, uint8_t number);   // gender vp::feat M/F
uint8_t nounGender(std::string_view noun, uint8_t latinGender);   // M or F
// Adds the written accent a word needs when `syllablesAdded` clitic syllables are attached ("mira" + "me").
std::string withClitics(std::string_view verbForm, std::string_view clitics);
bool startsWithStressedA(std::string_view noun);             // "agua", "águila", "hambre": "el agua"
}  // namespace es

// ---- per-token facts the frame builder and the realisers share ------------------------------------------------------
struct TokInfo {
  uint32_t lemma = lex::kNoLemma;
  std::string key;          // latin_key of the lemma head ("" for names without a lemma: then the written key)
  feat::Features f;         // features of the chosen reading
  uint8_t lpos = 0;         // the lemma's part of speech
  uint8_t lgender = 0;      // the lemma's gender (nouns)
  uint16_t lflags = 0;
  bool name = false, word = false, number = false, punct = false;
  std::string text;         // as written
  std::string encl;
  uint32_t verbLemma = lex::kNoLemma;   // participles: the verb they belong to (secūtus -> sequor), else kNoLemma
  bool deponent = false;    // verbLemma (or the verb itself) is deponent
};
void tokInfos(const lex::Lexicon& la, const Sentence& s, std::vector<TokInfo>& out);

// The verb of a participle lemma: the lexicon lists the participle's headword as a "verb ... participle" analysis of
// the verb itself (lēctus <- legō perfect participle passive; secūtus <- sequor). Same vowel quantities first, then
// the verb's tier and frequency. kNoLemma when `p` is not a participle lemma or no verb is found. `buf` is scratch.
uint32_t verbOfParticipleLemma(const lex::Lexicon& la, uint32_t p, std::vector<lex::Analysis>& buf);

// ---- Latin frame builder (readable.cpp) ------------------------------------------------------------------------------
// Perfect participle + a form of sum read as one verb (amātus erat, ingressus est): both tokens stay in the interlinear,
// the note names the periphrasis.
struct Periphrasis {
  int participle = -1, aux = -1;           // token indices
  bool deponent = false;                   // active meaning (ingressus est = entered)
  int auxTense = 0;                        // vp::feat tense of the sum form (Present -> perfect, Imperfect -> pluperfect)
};

struct Built {
  std::vector<frame::SemFrame> frames;     // main clauses in order (coordinated main clauses: several)
  std::vector<std::string> joiners;        // between frames[i-1] and frames[i]: Latin key of the connector or ","
  std::vector<std::string> flags;          // "abl-abs", "no-verb", ...
  std::vector<std::string> roles;          // per token: "subject", "object", ...
  double fill = 1.0;                       // share of word tokens the frame accounts for
  std::vector<Periphrasis> periphrases;
};
void buildFrames(const lex::Lexicon& la, const Tables& tab, const Sentence& s, const std::vector<TokInfo>& ti,
                 Built& out);

// ---- realisers -------------------------------------------------------------------------------------------------------
struct Lexical {                // the target-language word for a Latin content lemma
  std::string word;             // "girl", "love", "niña", "amar"
  bool person = false, mass = false, unique = false, missing = false, pivot = false, invariable = false;
  bool event = false;           // a time-like noun for post / ante ("after dinner" vs "behind the door")
  bool adjective = false;       // a participle with its own adjective row (īrātus -> angry)
  std::string timePrep;         // bare ablative of time: "at", "on", "in" (EN) / "en", "por la" (ES)
  uint8_t gender = 0;           // Spanish noun gender (M/F) when known
};

struct Discourse {              // memory across sentences of one text (articles, restored pronouns)
  std::vector<uint32_t> mentioned;    // lemmas mentioned so far (bounded: last 64)
  uint8_t lastSubjGender = 0;   // vp::feat::Gender of the last 3rd-person subject (M/F/N)
  bool lastSubjPerson = true;
  uint8_t lastSubjNumber = 1;
  void clear() { mentioned.clear(); lastSubjGender = 0; lastSubjPerson = true; lastSubjNumber = 1; }
  bool seen(uint32_t l) const;
  void mention(uint32_t l);
};

class LexicalSource {           // implemented by the Translator
 public:
  virtual ~LexicalSource() = default;
  virtual Lexical lexical(const TokInfo& t, Target lang) const = 0;
  virtual std::string nameIn(const TokInfo& t, Target lang) const = 0;
};

struct RealiseIn {
  const Sentence* s = nullptr;
  const std::vector<TokInfo>* ti = nullptr;
  const Tables* tab = nullptr;
  const LexicalSource* lex = nullptr;
  Discourse* disc = nullptr;
};
std::string realiseEnglish(const Built& b, const RealiseIn& in, std::vector<std::string>& flags);
std::string realiseSpanish(const Built& b, const RealiseIn& in, std::vector<std::string>& flags);

// Small string helpers shared by the realisers.
void appendWord(std::string& out, std::string_view w);
std::string capitaliseFirst(std::string_view s);
std::string stripMacrons(std::string_view s);

}  // namespace vp::la2x::detail
