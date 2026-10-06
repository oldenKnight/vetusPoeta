// vp::rules::Engine — the one interface the CLI server uses to translate, inspect and check. [CONTRACT, DESIGN §9, §10]
// Implemented by engine/rules (RULES tasks). Until then engine/cli links the stub in engine/cli/src/rules_stub.cpp,
// which echoes the source text and marks every cue Check. Nothing here throws; every failure is a Result.
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "vp/result.h"

namespace vp::lex { class Lexicon; }

namespace vp::rules {

enum class Lang { En, Es, La, Grc };
enum class Confidence { Ok, Check, Fix };

struct Options {
  Lang source = Lang::En, target = Lang::La;
  int fidelity = 2;              // 1 extremely faithful (tier 3 allowed) .. 3 flexible (tier 1, paraphrase)
  bool useModel = false;         // engine ii (closed choices only)
  bool useOnline = false;        // engine iii (evidence only)
  bool emoji = true;             // attach emoji to depictable nouns (display)
  bool macrons = true;           // display forms with length marks
  bool orbergise = false;        // rewrite Latin input with tier ceiling
  int orbergTier = 1;
};

struct Features { std::string pos, case_, number, gender, person, tense, mood, voice, degree; };

struct TokenView {
  std::string text, display;     // surface as written / with macrons
  int start = 0, end = 0;        // byte offsets in the cue target text
  uint32_t lemmaId = 0; bool hasLemma = false;
  Features features;
  uint8_t tier = 0; std::string emoji;
  bool unknown = false, fromRule = false;
};
struct Check  { std::string id; bool ok = true; std::string detail; };       // "A1".."A9"
struct Reason { int tokenIndex = -1; std::string kind, text, data; };         // kind per DESIGN §9.2
struct Alternative { std::string text, reason; double score = 0; };

struct CueInput  { uint32_t index = 0; std::string sourceText; int64_t startMs = 0, endMs = 0; std::string prevSource, nextSource; };
struct CueOutput {
  uint32_t index = 0;
  std::string target;                       // one or more lines joined with '\n'
  Confidence confidence = Confidence::Check; double score = 0;
  std::vector<Alternative> alternatives;
  std::vector<TokenView> tokens;
  std::vector<Check> checks;
  std::vector<Reason> reasons;
  std::vector<std::string> flags;           // "song", "nonverbal", "name-guessed", "cps", ...
};

struct GlossaryEntry { std::string name, policy, form, gender; int declension = 0; };
struct Correction    { std::string sourceKey, target, scope; int count = 0; };

struct Context {                             // per-project data the engine consults; owned by the caller
  std::vector<GlossaryEntry> glossary;
  std::vector<Correction> corrections;
};

struct Analysis { uint32_t lemmaId = 0; std::string head, glossEn, glossEs; Features features; std::string display; uint8_t tier = 0; };
struct InspectResult { std::vector<Analysis> analyses; std::vector<std::string> suggestions; };

class Engine {
 public:
  virtual ~Engine() = default;
  // Lexicons are opened by the caller (CLI) and handed in; null pointers mean "not available" and must not crash.
  virtual Result<void> setLexicons(const lex::Lexicon* latin, const lex::Lexicon* greek,
                                   const lex::Lexicon* english, const lex::Lexicon* spanish) = 0;
  // Translates a batch of cues in order; `progress(done)` may be called after each cue; `cancelled()` is polled.
  virtual Result<std::vector<CueOutput>> translate(const std::vector<CueInput>& cues, const Options&, const Context&,
                                                   const std::function<void(size_t)>& progress,
                                                   const std::function<bool()>& cancelled) = 0;
  // Re-check a user-edited cue (A1..A9) without translating it.
  virtual Result<CueOutput> check(const CueInput&, const std::string& target, const Options&, const Context&) = 0;
  virtual Result<InspectResult> inspect(const std::string& word, Lang lang, const Options&) = 0;
  virtual std::string version() const = 0;
};

std::unique_ptr<Engine> makeStubEngine();   // engine/cli/src/rules_stub.cpp until RULES provides makeEngine()
std::unique_ptr<Engine> makeEngine();       // engine/rules (RULES tasks); the CLI prefers it when VP_HAVE_RULES

}  // namespace vp::rules
