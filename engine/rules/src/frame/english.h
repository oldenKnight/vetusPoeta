// English helpers of the frame builder and the transfer (C17, quality loop 3): word forms the tagger misreads or the
// lexicon does not list are derived from known lemmas before a word is given up as unknown. Internal to
// engine/rules/src. Everything reads english.vpl only; deterministic; nothing throws.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "vp/lex.h"
#include "vp/nlp.h"

namespace vp::frame::en {

// Tags and features from english.vpl (true when a tag changed; the caller re-parses):
//  * an irregular past the tagger read as a present ("She sang", "He swam"): Tense=Past (a participle after have/be);
//  * a past form tagged as a noun in a sentence without a verb ("The bell rang."): a finite verb;
//  * a word the lexicon knows only as an adjective, tagged as a noun ("My sister is braver than me"): an adjective.
bool retagForms(std::vector<nlp::Token>& tk, const lex::Lexicon& lx);
// C24: kinship words and titles are nouns of address (C30b: also used for a one-word cue)
bool addressWord(const std::string& w);

// The verb whose past (or past participle, or -ing form) this form is, when the lexicon lists the form only as such
// under another lemma ("lighted" -> light, "sang" -> sing). `present` is set for -ing forms. Empty when none.
std::string verbOfForm(const lex::Lexicon& lx, const std::string& lower, bool* present = nullptr);

// Base words for a form the lexicon does not list at all, best first: a hyphenated compound joined ("sea-shore" ->
// seashore) and then its last part ("bran-new" -> new); regular endings removed (-ies/-ied -> y, -es, -s, -ed, -ing,
// -er, -est, -ly; doubled consonants: "stopped" -> stop; a dropped e: "hoped" -> hope). Only spellings; the caller
// checks them against a lexicon.
// `upos` (NOUN, VERB, ADJ, ADV; empty = any) keeps only the endings of that part of speech (a noun: -s/-es/-ies; a
// verb: -s/-es/-ies/-ed/-ied/-ing; an adjective: -er/-est/-ier/-iest; an adverb: -ly/-ily).
std::vector<std::string> baseCandidates(const std::string& lower, const std::string& upos = std::string());

// The first base of baseCandidates() the English lexicon knows with a part of speech compatible with `upos`
// (any when empty); empty when none.
std::string knownBase(const lex::Lexicon& lx, const std::string& lower, const std::string& upos);

// C19: a compound noun the lexicon does not list, split into a known first word and a head noun of a closed list
// ("snowman" -> snow + man, "tinsmith" -> tin + smith, "milkmaid" -> milk + maid, "snowmen" -> snow + man). `head` is
// the singular head; false when no split fits (never "kit" + "ten").
bool compoundParts(const lex::Lexicon& lx, const std::string& lower, std::string& first, std::string& head);

// C22: a colour adjective (blue, black, white, red, green, yellow, grey/gray, brown, golden, silver, pink): the first
// part of a compound that is an adjective ("bluebird" -> avis caerulea), not a genitive.
bool colourWord(const std::string& lower);

// C32: the sex an English person noun states by itself (lemma, lower case): 'm' for boy, son, brother, king, man ...,
// 'f' for girl, daughter, sister, queen, woman ...; 0 for words of either sex (child, teacher, friend, cousin).
char nounSex(const std::string& lemma);

}  // namespace vp::frame::en
