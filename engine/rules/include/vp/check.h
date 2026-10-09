// Latin checker (DESIGN.md §10.4, PREPLAN 6.3): A1 known form, A2 Whitaker cross-check (warning), A3 agreement,
// A4 case government, A6 tier compliance. Works from the text alone (it re-analyses every word with
// morph::analyseLatin and re-derives agreement from the analyses, never from the generator's bookkeeping), so it
// checks user edits too; optional token hints only mark names, paradigm-fallback forms and the chosen lemma (A6).
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "vp/curated.h"
#include "vp/lex.h"
#include "vp/morph.h"
#include "vp/rules.h"

namespace vp::check {

struct TokenHint {                 // byte span in the checked text (e.g. from LaSentence::tokens)
  int start = 0, end = 0;
  uint32_t lemma = lex::kNoLemma;  // A6 uses this lemma's tier
  bool fromRule = false;           // paradigm-fallback form: A1 passes, the cue is Check
  bool name = false;               // a name (A1 exempt)
  bool guess = false;              // C32 addition: a form guess (dictionary form for a missing cell): no reading,
                                   // A1 exempt, outside A3 / A4 (the engine flags the cue form-guess, Check)
};

struct Options {
  uint8_t tierCeiling = 3;                                   // A6: 1, 2 or 3
  const std::vector<rules::GlossaryEntry>* glossary = nullptr;   // names exempt from A1
  const std::vector<TokenHint>* hints = nullptr;
};

struct Issue { std::string id; int token = -1; std::string detail; bool warning = false; };

struct CheckedToken {
  std::string text;            // the word as written (enclitic included)
  int start = 0, end = 0;      // byte offsets
  morph::Token analysis;
  bool name = false, sentenceInitial = false, fromRule = false;
  int segment = 0;             // clause segment index (punctuation, subordinators, relatives)
};

struct Report {
  std::vector<rules::Check> checks;   // A1, A2, A3, A4, A6 in that order
  std::vector<Issue> issues;          // every finding (warnings included), in text order per check
  std::vector<CheckedToken> tokens;
  bool fromRule = false;              // some form came from the paradigm fallback (confidence Check)
  bool ok(std::string_view id) const;
  size_t count(std::string_view id, bool warnings = false) const;
  void clear() { checks.clear(); issues.clear(); tokens.clear(); fromRule = false; }
};

class LatinChecker {
 public:
  LatinChecker(const lex::Lexicon& lx, const curated::CuratedData& cd);
  void check(std::string_view text, const Options&, Report& out);
  Report check(std::string_view text, const Options& o) { Report r; check(text, o, r); return r; }

 private:
  struct Reading;
  struct Impl;
  const lex::Lexicon& lx_;
  const curated::CuratedData& cd_;
  std::vector<std::string> nameKeys_;   // latin_key of every declined form of names_la.tsv (sorted)
};

}  // namespace vp::check
