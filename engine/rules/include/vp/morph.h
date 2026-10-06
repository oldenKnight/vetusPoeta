// Morphology on top of the lexicon. [CONTRACT, DESIGN.md §6] (additions marked "C1 addition" are additive)
// analyseLatin / analyseGreek turn a written word into lexicon analyses; generate() turns (lemma, features) into a
// surface form through Lexicon::generate (exact FEAT id), a tolerant cell match (merged genders such as "mfn",
// degree "none" = positive, deponents), periphrastic perfect passives (participle + sum) and, only for lemmas
// without an inflection table, the rule-based paradigm fallback of paradigm_la.cpp (fromRule = true).
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "vp/features.h"
#include "vp/lex.h"

namespace vp::morph {

using Features = feat::Features;   // pos, case_, number, gender, person, tense, mood, voice, degree, extra

// C1 addition: an analysis that the lexicon does not hold but the paradigm fallback produces for a lemma without a
// table (packed features instead of a FEAT id, because the word may not exist in FEAT).
struct RuleAnalysis { uint32_t lemma = 0; uint32_t packed = 0; std::string display; };

struct Token {
  std::string text; std::string key; std::vector<lex::Analysis> analyses; bool enclitic = false;
  std::string encliticText;   /* "que","ne","ue" */ bool unknown = false;
  // C1 additions
  std::vector<RuleAnalysis> ruleAnalyses;   // paradigm-fallback analyses (only when `analyses` is empty)
  bool fromRule = false;                    // the analyses came from the paradigm fallback
  bool whitakerOnly = false;                // every analysis carries ANAL flag bit2 (Whitaker-only)
  bool capitalised = false;                 // first letter upper case in the text
  bool nameGuess = false;                   // capitalised and unknown: kept as a name (Check)
  bool accentInsensitive = false;           // Greek: matched through greek_bare
};

// Latin: strips -que -ne -ue(-ve) when the base is known and the whole is not; u/v, i/j and macrons are folded by
// latin_key; falls back to Whitaker stem+ending (ANAL flag bit2) through the lexicon's Whitaker-only analyses, then
// to the paradigm fallback (lemmas without a table; Token::fromRule). Capitalised unknown words set nameGuess.
void analyseLatin(const lex::Lexicon&, std::string_view word, Token& out);
// Greek: exact greek_key, then greek_bare match (flag "accent-insensitive" on the Token), then capital-initial fold.
void analyseGreek(const lex::Lexicon&, std::string_view word, Token& out);

// C1 addition: what generate() did.
struct GenInfo {
  bool fromRule = false;       // paradigm fallback (no table): the checker reports Check, never OK
  bool periphrastic = false;   // composed of two words (perfect passive / deponent perfect: participle + sum)
  bool exact = false;          // the exact FEAT cell existed
  uint32_t lemmaUsed = 0;      // the lemma whose cell was used (comparative lemma, participle lemma ...)
  uint32_t packed = 0;         // the features of the cell used
};

// Generation: feature packing helpers and the one entry point. Returns false if the cell does not exist; the caller
// decides (periphrasis or another lemma). Never invents a form (the paradigm fallback is the documented exception
// for lemmas without has_table).
bool generate(const lex::Lexicon&, uint32_t lemma, const Features&, std::string& out, bool macrons);
bool generate(const lex::Lexicon&, uint32_t lemma, const Features&, std::string& out, bool macrons, GenInfo* info);

// Feature builders (vp::feat enumerations).
Features nounForm(uint8_t case_, uint8_t number);
Features adjForm(uint8_t case_, uint8_t number, uint8_t gender, uint8_t degree = feat::Positive);
Features verbForm(uint8_t person, uint8_t number, uint8_t tense, uint8_t mood = feat::Indicative,
                  uint8_t voice = feat::Active);
Features participle(uint8_t tense, uint8_t voice, uint8_t case_ = feat::Nom, uint8_t number = feat::Sg,
                    uint8_t gender = feat::M);
Features infinitive(uint8_t tense = feat::Present, uint8_t voice = feat::Active);
Features imperative(uint8_t number = feat::Sg, uint8_t voice = feat::Active);

// C1 additions -----------------------------------------------------------------------------------------------------
// The lemma whose key is latin_key(head) (pos 0 = any), preferring: exact headword (macrons), has_table, the
// requested pos, a tier (1 < 2 < 3 < none), frequency rank, lower id. lex::kNoLemma when none.
uint32_t findLemma(const lex::Lexicon&, std::string_view head, uint8_t pos = 0);
// Every feature tuple of an analysis (lexicon or rule).
uint32_t packedOf(const lex::Lexicon&, const lex::Analysis&);
// True when gender `have` (a cell or lemma gender, possibly merged: MF, MN, FN, MFN, or 0) admits `want` (M/F/N).
bool genderAdmits(uint8_t have, uint8_t want);
// Display cleanup used everywhere a form is shown: NFC, anceps vowels (macron + breve on one vowel, "egō̆") shown
// plain, combining tie bars / double breves (U+035C-0362, "de͡inde") dropped, and display_latin(macrons).
// C15: a headword or form without editorial marks at its edges ("((caelum" -> "caelum"); displayForm applies it.
std::string cleanHead(std::string_view word);
std::string displayForm(std::string_view form, bool macrons);
// Principal parts parsed from a lemma's `principal` line (both the Wiktionary head line "amō (present infinitive
// amāre, perfect active amāvī, supine amātum); first conjugation" and the short "amō, amāre, amāvī, amātum").
struct Principal { std::string first, infinitive, perfect, supine, genitive, feminine, neuter, comparative,
                   superlative; };
Principal parsePrincipal(std::string_view head, std::string_view principal);

}  // namespace vp::morph
