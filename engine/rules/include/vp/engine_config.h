// Construction options of the rule engine (makeEngine) and the hooks through which the local model (engine ii) and
// the online check (engine iii) plug in later. API addition by C2 (recorded in docs/STATUS.md): the CLI keeps calling
// makeEngine() (defaults from the environment) until it adopts makeEngine(const EngineConfig&).
#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "vp/rules.h"

namespace vp::rules {

// Evidence from engine iii for one lemma/gloss: +1 agrees, -1 disagrees, 0 unknown. Never edits text.
struct Evidence { int verdict = 0; std::string source, detail; };

struct Advisors {
  // Engine ii, closed choice: returns the index of the chosen option (or -1 for "no opinion").
  std::function<int(const std::string& prompt, const std::vector<std::string>& options)> chooseSense;
  // Engine iii: evidence for a chosen Latin lemma and its gloss.
  std::function<Evidence(const std::string& lemma, const std::string& gloss)> onlineCheck;
};

struct EngineConfig {
  std::string dataDir;      // holds the lexicons (the CLI hands them in through setLexicons)
  std::string curatedDir;   // data/curated tables
  std::string nlpDir;       // english.tag.vpt, english.dep.vpt, spanish.*.vpt
  Advisors advisors;        // empty: Options.useModel / useOnline only add the reasons "model: off" / "online: off"
  double cpsLimit = 17.0;   // A8 reading speed (characters per second)
  int maxLine = 42, maxLines = 2;
};

// Defaults: dataDir = $VP_DATA_WORK or "data/work"; curatedDir = $VP_CURATED_DIR or "data/curated";
// nlpDir = $VP_NLP_DIR or <dataDir>/nlp.
EngineConfig defaultEngineConfig();
std::unique_ptr<Engine> makeEngine(const EngineConfig& config);

}  // namespace vp::rules
