// Rule-based Latin paradigm fallback (DESIGN.md §6). Internal to engine/rules.
// Scope (and nothing else): regular 1st/2nd declension nouns, 1st/2nd class adjectives (and -us participles),
// 1st and 2nd conjugation regular (non-deponent) verbs, regular comparatives (-ior) and superlatives (-issimus,
// -errimus). Forms carry macrons where the stems and endings have them.
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "vp/lex.h"
#include "vp/morph.h"

namespace vp::morph::detail {

struct Cell { uint32_t packed = 0; std::string form; };

// True when the lemma has no inflection table and its class is in the fallback scope.
bool paradigmApplies(const lex::Lemma&);
// The full fallback paradigm of a lemma (empty when out of scope or the principal parts cannot be parsed).
void paradigm(const lex::Lemma&, std::vector<Cell>& out);
// Building blocks (also used for participles of periphrastic forms and for comparison).
void adjective12(std::string_view nomM, std::string_view stem, uint8_t pos, uint8_t degree, std::vector<Cell>& out);
void comparative(std::string_view stem, uint8_t pos, std::vector<Cell>& out);
// Comparative / superlative stems of a 1st/2nd class adjective ("altus" -> "alt", "pulcher" + "pulchra" -> "pulchr").
std::string adjStem(std::string_view nomM, std::string_view feminine);
std::string superlativeNom(std::string_view nomM, std::string_view stem);   // altissimus, pulcherrimus

// C32: a cell the lemma's own table lacks, derived from its other cells (or its principal parts) by the rules of its
// class: case syncretism (vocative = nominative, dative = ablative plural, neuter nominative = accusative), the
// declension endings on the genitive stem (1st-5th declension nouns, 1st/2nd and 3rd class adjectives), the
// conjugation endings on the present / perfect stem (1st-4th and -iō verbs, active and personal passive), perfect-only
// verbs (meminī: perfect = present form), the future imperative for a missing present one. Never a person, number
// or voice the lemma does not have (impersonal verbs, verbs without a personal passive, plural-only nouns).
// `attested`: the form is another cell of the lemma that Latin grammar makes identical (syncretism, the perfect-only
// verbs, the future imperative), not built from endings.
bool gapCell(const lex::Lexicon&, const lex::Lemma&, const Features& want, std::string& form, bool& attested);

// UTF-8 helpers on strings with macrons.
bool endsWith(std::string_view s, std::string_view suffix);
std::string dropSuffix(std::string_view s, std::string_view suffix);

}  // namespace vp::morph::detail
