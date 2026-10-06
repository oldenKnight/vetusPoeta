// The Latin minimal-pair gate of PREPLAN 2.2 (used by `vpengine llm-gate` and tools/eval/llm_gate.py).
// Pairs are generated from the lexicon itself: short correct clauses from four templates, and the same clause with
// one word replaced by another cell of the same lemma. A corrupted clause is kept only if a small, deliberately
// generous clause checker finds no grammatical reading of it (every analysis of every word in the lexicon is
// tried: vocative excepted, since the clauses carry no commas); the correct clause must pass the same checker.
// Each clause is scored by the model as raw text after prompts::kGatePrefix; a pair counts as right when the
// correct clause has the strictly higher summed log-probability (primary metric, fixed before the first run).
#pragma once
#include <cmath>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "vp/lex.h"
#include "vp/llm.h"
#include "vp/result.h"

namespace vp::llm {

struct GatePair {
  std::string templ;        // "N-A-est", "S-O-V", "Npl-Vpl", "in-N-V"
  std::string corruption;   // "case", "number", "gender", "person"
  std::string good, bad;    // clauses without macrons, capitalised, with a final full stop
  double goodLp = 0, badLp = 0;
  int goodTokens = 0, badTokens = 0;
};

struct Interval {
  double p = 0, lo = 0, hi = 0;
};
// Wilson score interval of a proportion (z = 1.96 for 95 %).
inline Interval wilson(int correct, int n, double z = 1.96) {
  Interval r;
  if (n <= 0) return r;
  const double nn = static_cast<double>(n), p = static_cast<double>(correct) / nn, z2 = z * z;
  const double denom = 1.0 + z2 / nn;
  const double centre = (p + z2 / (2.0 * nn)) / denom;
  const double half = z * std::sqrt(p * (1.0 - p) / nn + z2 / (4.0 * nn * nn)) / denom;
  r.p = p;
  r.lo = centre - half < 0.0 ? 0.0 : centre - half;
  r.hi = centre + half > 1.0 ? 1.0 : centre + half;
  return r;
}

// Deterministic for a given lexicon file, n and seed. Fewer than n pairs only if the lexicon runs out.
Result<std::vector<GatePair>> buildGatePairs(const lex::Lexicon& latin, int n, uint64_t seed);

struct GateOptions {
  int pairs = 600;
  uint64_t seed = 1;
  Config model;                                   // path, threads
  std::function<void(int done, int total)> progress;
};

struct GateResult {
  std::vector<GatePair> pairs;
  int correct = 0, correctMean = 0;               // summed (primary) and per-token mean log-probability
  Interval sum, mean;
  double msPerPair = 0;
  int loadMs = 0;
  bool passed = false;                            // sum.p >= 0.75 and sum.lo > 0.70
};

#if defined(VP_LLM_REAL) && VP_LLM_REAL
Result<GateResult> runGate(const lex::Lexicon& latin, const GateOptions& opt);
#else
inline Result<std::vector<GatePair>> buildGatePairs(const lex::Lexicon&, int, uint64_t) {
  return Error{ErrorCode::ModelMissing, "the local model engine is not part of this build",
               "This version of vetus poeta was built without the local model."};
}
inline Result<GateResult> runGate(const lex::Lexicon&, const GateOptions&) {
  return Error{ErrorCode::ModelMissing, "the local model engine is not part of this build",
               "This version of vetus poeta was built without the local model."};
}
#endif

}  // namespace vp::llm
