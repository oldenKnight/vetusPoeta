// Closed-class tables of the transfer stage (DESIGN.md §10.2): canonical English words (the frame builder maps
// Spanish closed classes to them) -> Latin headwords, resolved to lemma ids by Transfer::latin(). Small, fixed,
// deterministic; content words never come from here (they go through REVX). Phrasal verbs, verb + preposition
// senses and state adjectives live in data/curated (phrasal_en_la.tsv, verbprep_en_la.tsv, states_en_la.tsv).
#pragma once
#include <string>
#include <string_view>

namespace vp::transfer::tables {

bool animateNoun(const std::string& lemma);           // person / animal nouns (EN + ES)
bool narrativeNoun(const std::string& lemma);   // C15: "tell a story" -> nārrō
bool personalPronoun(const std::string& w);           // i, me, you, he ... yo, tú, él ...
const char* adverb(const std::string& lemma, bool motion);   // here -> hīc (motion: hūc) ...; nullptr if not listed
bool dropAdverb(const std::string& lemma);            // adverbs with no Latin counterpart needed ("just")
std::string spanishAdverb(const std::string& lemma);  // aquí -> here ...; unchanged when not listed
const char* weekday(const std::string& lower);        // tuesday -> Mārs (genitive attribute of diēs)
const char* languageAdverb(const std::string& lower); // latin -> Latīnē
bool timeNoun(const std::string& lower);
bool motionVerb(const std::string& lemma);
bool impersonalAdjective(const std::string& lemma);   // "it is dark / late / impossible": neuter predicate
const char* verb(const std::string& lemma);           // be -> sum, can -> possum, have -> habeō, do -> faciō
bool stateVerb(std::string_view latinKey);            // RULE tense.past.state: imperfect in the past
const char* interjection(const std::string& w);       // oh -> ō, alas -> ēheu ...
const char* connector(const std::string& w, bool afterFirst);   // and -> et, then -> igitur / deinde ...
const char* cardinal(int v);                          // 1 -> ūnus ... 1000 -> mīlle; nullptr if not listed
const char* ordinal(int v);                           // 6 -> sextus

}  // namespace vp::transfer::tables
