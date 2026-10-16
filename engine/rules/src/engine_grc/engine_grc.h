// The Greek engine path (task C12), driven by makeEngine()'s RulesEngine through one delimited block in
// src/engine/engine.cpp: pairs en-grc / es-grc (frame builder with the Greek phrasebooks -> GreekTransfer ->
// GreekRealiser -> GreekChecker A1/A1b/A3/A4/A6 + A5/A7/A8 here -> confidence as the Latin path -> cue assembly with
// Greek line breaks), grc-en / grc-es (grc2x: readable + interlinear), check() of an edited Greek cue and inspect() of
// Greek words (and of English / Spanish words when the target is Greek). Internal header (not public API).
#pragma once
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "vp/curated.h"
#include "vp/frame.h"
#include "vp/lex.h"
#include "vp/nlp.h"
#include "vp/result.h"
#include "vp/rules.h"

namespace vp::grc {

struct PathConfig {
  double cpsLimit = 17.0;
  int maxLine = 42, maxLines = 2;
};

class GreekPath {
  struct Key { explicit Key() = default; };

 public:
  explicit GreekPath(Key);
  ~GreekPath();
  GreekPath(const GreekPath&) = delete;
  GreekPath& operator=(const GreekPath&) = delete;

  // Loads the Greek curated tables (order_grc.txt, valency_grc.tsv, preps_en_grc.tsv, names_grc.tsv,
  // particles_grc.tsv, phrasebook_en_grc.tsv, lexical_en_grc.tsv, phrasebook_es_grc.tsv) from the first directory of
  // `dirs` that holds order_grc.txt. `cd` is the engine's curated data (copied once: its phrasebooks are replaced by
  // the Greek ones for the frame builder). The lexicons must outlive the path.
  static Result<std::unique_ptr<GreekPath>> create(const lex::Lexicon& greek, const curated::CuratedData& cd,
                                                   const std::vector<std::filesystem::path>& dirs, PathConfig cfg);

  // Source analysis: the engine's pipelines and source lexicons (null = not available).
  void setSources(const nlp::Pipeline* en, const lex::Lexicon* enLex, const nlp::Pipeline* es, const lex::Lexicon* esLex);

  static bool toGreek(const rules::Options& o) {
    return o.target == rules::Lang::Grc && (o.source == rules::Lang::En || o.source == rules::Lang::Es);
  }
  static bool fromGreek(const rules::Options& o) {
    return o.source == rules::Lang::Grc && (o.target == rules::Lang::En || o.target == rules::Lang::Es);
  }

  Result<std::vector<rules::CueOutput>> translate(const std::vector<rules::CueInput>& cues, const rules::Options& opt,
                                                  const rules::Context& ctx,
                                                  const std::function<void(size_t)>& progress,
                                                  const std::function<bool()>& cancelled);
  // Re-check a user-edited Greek cue (A1, A1b, A3, A4, A5, A6, A8).
  rules::CueOutput check(const rules::CueInput& in, const std::string& target, const rules::Options& opt,
                         const rules::Context& ctx);
  // Greek words: analyses (sandhi undone; gloss in both languages, the UI shows its own) and accent-insensitive
  // suggestions; English / Spanish words with target Greek: the Greek candidates of the reverse index.
  Result<rules::InspectResult> inspect(const std::string& word, rules::Lang lang, const rules::Options& opt);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace vp::grc
