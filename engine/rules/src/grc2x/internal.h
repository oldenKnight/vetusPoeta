// Internal pieces of grc2x (not a public header): the analyser, the frame fill from Greek roles and the English /
// Spanish realisers over frame::SemFrame. Deterministic; nothing throws on bad data.
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "vp/features.h"
#include "vp/frame.h"
#include "vp/grc2x.h"
#include "vp/lex.h"
#include "vp/realise_grc.h"
#include "vp/rules.h"
#include "vp/transfer_grc.h"

namespace vp::grc2x::detail {

constexpr uint32_t kNone = lex::kNoLemma;

// Tokenises, reads and disambiguates a Greek sentence (see vp/grc2x.h).
class Analyser {
 public:
  Analyser(const lex::Lexicon& lx, const curated::CuratedData& cd, const grc::GreekData& gd, const grc::GreekTables& gt);
  void analyse(std::string_view sentence, Sentence& out, const std::vector<rules::GlossaryEntry>* glossary) const;

 private:
  struct NameForm { std::string key, english; uint8_t case_, gender, number = 1; std::string spanish; bool place = false; };
  void readings(const std::string& word, Token& t, const std::vector<rules::GlossaryEntry>* glossary) const;
  void disambiguate(Sentence& s) const;
  const lex::Lexicon& lx_;
  const curated::CuratedData& cd_;
  const grc::GreekData& gd_;
  const grc::GreekTables& gt_;
  std::vector<NameForm> names_;   // declined forms of names_grc.tsv, sorted by key
};

// Facts per token that the frame fill and the realisers share (from the chosen reading).
struct TokInfo {
  uint32_t lemma = lex::kNoLemma;
  std::string key;            // greek_key of the lemma head ("" for names without a lemma)
  feat::Features f;           // features of the chosen reading
  uint8_t lpos = 0;           // the lemma's part of speech (the article: feat::Article)
  uint8_t lgender = 0;
  bool word = false, punct = false, number = false, name = false, closed = false;
  std::string text;           // as written
  std::string nameEn;         // name reading: the English spelling
  std::string nameEs;         // C29: the Spanish spelling of a place name ("Atenas"), "" = as nameEn
  bool place = false;         // C29: a city / country of names_grc.tsv (not a person)
};

// The target-language word of a Greek lemma, with the facts the realisers need.
struct Lexical {
  std::string word;           // "girl", "niña", "look for", "tener miedo"
  bool person = false, animal = false, mass = false, motion = false, missing = false, pivot = false;
  std::string plural;         // irregular English plural ("men"), "" = rule
  uint8_t gender = 0;         // Spanish noun gender (feat::M / F)
};

class LexicalSource {
 public:
  virtual ~LexicalSource() = default;
  virtual Lexical lexical(uint32_t lemma, uint8_t pos, bool middle, Target t) const = 0;
};

// Side information per token for the realisers (indexed by SemNP::token / SemAdj::token / SemPredicate::token).
struct Side {
  std::vector<Lexical> lex;   // per token: the target word of its lemma (empty word for function words)
  std::vector<TokInfo> ti;
};

struct Built {
  std::vector<frame::SemFrame> frames;      // main clauses in order
  std::vector<std::string> joiners;         // between frames[i-1] and frames[i]: "," or "" (connectors live in the frame)
  std::vector<std::string> flags;           // "no-verb", ...
  std::vector<std::string> roles;           // per token
  bool question = false, exclamation = false;
};

// Fills SemFrames from the analysed Greek sentence (target words already chosen through `src`).
void buildFrames(const lex::Lexicon& lx, const Sentence& s, const std::vector<TokInfo>& ti,
                 const LexicalSource& src, Target t, Side& side, Built& out);
std::string realiseEnglish(const Built& b, const Side& side);
std::string realiseSpanish(const Built& b, const Side& side);

void tokInfos(const lex::Lexicon& lx, const Sentence& s, std::vector<TokInfo>& out);
std::string featureText(const feat::Features& f, uint8_t lemmaPos);

}  // namespace vp::grc2x::detail
