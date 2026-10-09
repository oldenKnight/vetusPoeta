// Greek checker (DESIGN.md §10.4 for Greek): A1 known form (exact greek_key reading after undoing the sentence
// sandhi; an accent-insensitive reading only is a warning "accent differs" -> Check, never Fix), A1b accent sanity
// (warning: one accent per word except proclitics / enclitics; the enclitic, grave, movable-nu and οὐ/οὐκ/οὐχ rules
// re-applied to the text must reproduce it), A3 agreement (article - adjective - noun, adjective - noun, predicate,
// subject - verb with the neuter-plural rule, relative pronoun - antecedent), A4 case government (prepositions from
// preps_en_grc.tsv, verb valency from valency_grc.tsv), A6 tier ceiling. Works from the text alone (re-analysis with
// vp::grc::analyse plus the closed-class tables); optional hints mark names, rule forms and the chosen lemma.
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "vp/check.h"
#include "vp/curated.h"
#include "vp/lex.h"
#include "vp/morph.h"
#include "vp/realise_grc.h"
#include "vp/rules.h"

namespace vp::check {

struct GreekCheckOptions {
  uint8_t tierCeiling = 3;
  const std::vector<rules::GlossaryEntry>* glossary = nullptr;   // names exempt from A1
  const std::vector<TokenHint>* hints = nullptr;
};

struct GrcCheckedToken {
  std::string text;
  int start = 0, end = 0;
  morph::Token analysis;
  bool name = false, fromRule = false, accentDiffers = false, closed = false;
  int segment = 0;
};

struct GrcReport {
  std::vector<rules::Check> checks;   // A1, A1b, A3, A4, A6 in that order
  std::vector<Issue> issues;
  std::vector<GrcCheckedToken> tokens;
  bool fromRule = false;
  bool ok(std::string_view id) const;
  size_t count(std::string_view id, bool warnings = false) const;
  // "A3: ...; A4: ...; " for the non-warning issues of the given checks (empty = clean).
  std::string failures(std::initializer_list<const char*> ids) const;
  void clear() { checks.clear(); issues.clear(); tokens.clear(); fromRule = false; }
};

class GreekChecker {
 public:
  GreekChecker(const lex::Lexicon& lx, const curated::CuratedData& cd, const grc::GreekData& gd);
  void check(std::string_view text, const GreekCheckOptions&, GrcReport& out);
  GrcReport check(std::string_view text, const GreekCheckOptions& o) { GrcReport r; check(text, o, r); return r; }

 private:
  struct Reading;
  struct Impl;
  const lex::Lexicon& lx_;
  const curated::CuratedData& cd_;
  const grc::GreekData& gd_;
  struct NameForm { std::string key; uint8_t case_, gender, number = 1; };   // C29: number (plural place names)
  std::vector<NameForm> names_;                               // declined forms of names_grc.tsv (sorted by key)
  std::vector<std::pair<std::string, uint32_t>> closedIds_;   // closed-table key -> lemma id
};

}  // namespace vp::check
