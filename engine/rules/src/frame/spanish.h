// Spanish branch of the frame builder (C13; DESIGN.md §10.1 item 6). Internal to engine/rules/src/frame.
// Two passes around the parser: retag() corrects tags and features from spanish.vpl before (re-)parsing (second-person
// verbs the tagger read as nouns, tense / mood / person the tagger left out), normalise() rewrites the dependency tree
// after lemmatisation so the language-neutral clause builder sees the English-like structure: clitic pronouns as
// arguments or as the particle "se" of a pronominal verb, personal "a", "por qué" / "a dónde", multi-word prepositions
// ("detrás de") and adverbs ("a veces"), "un poco de X". Deterministic; nothing throws.
#pragma once
#include <string>
#include <vector>

#include "vp/curated.h"
#include "vp/frame.h"
#include "vp/lex.h"
#include "vp/nlp.h"

namespace vp::frame::es {

// One verb reading of a Spanish form from spanish.vpl (lexicon features, lemma head).
struct VerbReading { std::string lemma; uint8_t person = 0, number = 0, tense = 0, mood = 0; };
void verbReadings(const lex::Lexicon& lex, const std::string& lower, std::vector<VerbReading>& out);
bool hasImperative2(const lex::Lexicon& lex, const std::string& lower);   // a 2nd-person imperative reading

// Tags and features from the lexicon. True when a tag changed (the caller re-parses).
bool retag(std::vector<nlp::Token>& tk, const lex::Lexicon& lex, const curated::CuratedData* cd = nullptr);   // C34: cd skips names
// C34: sure readings from the tables and the lexicon, not a fallback (no "retag" repair): names of names_la.tsv, words
// of address before a comma ("Mamá, ..."), unlisted diminutives, "la vi"; and a sentence-initial "Ven a ..." read as
// venir's imperative, which sets `guess` (a "retag" repair: Check). True when a tag changed (the caller re-parses).
bool retagWords(std::vector<nlp::Token>& tk, const lex::Lexicon& lex, const curated::CuratedData& cd, bool& guess);

// Tree normalisation after lemmas (see the file comment). Records repairs in s.repairs only for real fallbacks.
void normalise(SemSentence& s, const lex::Lexicon* lex, const curated::CuratedData& cd);

// Spanish word lists shared with the clause builder.
bool personNoun(const std::string& lemma);        // persons and animals (personal "a", gender of él / ella)
bool motionVerb(const std::string& lemma);        // ir, venir, caer ... ("en" -> into, "a" -> to)
bool intransitiveVerb(const std::string& lemma);  // vivir, llegar ... (a post-verbal "object" is the subject)
bool dativeVerb(const std::string& lemma);        // gustar, dar, decir ... (me / te / nos -> indirect object)
bool rareLemma(const std::string& lemma);          // dolar (beside doler), vetar (vete) ...: chosen last
bool experiencerVerb(const std::string& lemma);   // gustar, doler ...: "me duele la cabeza" (the thing is the subject)
bool notDiminutive(const std::string& lemma);     // "señorita", "mosquito" ...: not -ito/-ita of another noun
// C34: words of address ("mamá", "maestra", "niños", "abuelita") and the family words whose diminutive is affection,
// not size ("abuelita" -> abuela, not a small grandmother); the base noun of a diminutive ("tortuguita" -> tortuga,
// "amiguito" -> amigo, "perritos" -> perro), "" when the word is none
bool addressNoun(const std::string& lower);
std::string mascSingular(const std::string& lower);   // C34: "cansadas" -> "cansado" (the states_es_la.tsv key)
bool endearment(const std::string& baseLemma);
std::string diminutiveBase(const lex::Lexicon& lex, const std::string& lower);
// Multi-word adverbs / prepositions of the clause builder: "a veces" -> "sometimes"; "" when not listed.
std::string multiwordPrep(const std::string& adv);   // "detrás" (+ de) -> "behind"

}  // namespace vp::frame::es
