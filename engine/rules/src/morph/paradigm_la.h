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

// UTF-8 helpers on strings with macrons.
bool endsWith(std::string_view s, std::string_view suffix);
std::string dropSuffix(std::string_view s, std::string_view suffix);

}  // namespace vp::morph::detail
