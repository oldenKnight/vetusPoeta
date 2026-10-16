// `vpengine llm-gate`: the Latin minimal-pair gate of PREPLAN 2.2 (engine/llm/include/vp/llm_gate.h).
// Prints a summary on stdout, progress on stderr, and with --json writes every pair and its scores.
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <map>
#include <string>

#include "commands.h"
#include "serve_io.h"
#include "vp/fs.h"
#include "vp/lex.h"
#include "vp/llm.h"
#include "vp/llm_gate.h"

namespace vpcli {

namespace {
json intervalJson(const vp::llm::Interval& i) { return json{{"p", i.p}, {"lo", i.lo}, {"hi", i.hi}}; }
}  // namespace

int cmdLlmGate(const Args& args) {
  try {
    std::string lexPath, modelPath, jsonPath;
    vp::llm::GateOptions opt;
    for (size_t i = 0; i < args.size(); ++i) {
      const std::string& a = args[i];
      const bool hasValue = i + 1 < args.size();
      if (a == "--pairs" && hasValue) opt.pairs = std::atoi(args[++i].c_str());
      else if (a == "--threads" && hasValue) opt.model.threads = std::atoi(args[++i].c_str());
      else if (a == "--seed" && hasValue) opt.seed = std::strtoull(args[++i].c_str(), nullptr, 10);
      else if (a == "--json" && hasValue) jsonPath = args[++i];
      else if (lexPath.empty()) lexPath = a;
      else if (modelPath.empty()) modelPath = a;
      else {
        std::fprintf(stderr, "llm-gate: unexpected argument %s\n", a.c_str());
        return 2;
      }
    }
    if (lexPath.empty() || modelPath.empty() || opt.pairs <= 0) {
      std::fprintf(stderr, "usage: vpengine llm-gate <latin.vpl> <model.gguf> [--pairs N] [--threads T] [--seed S] "
                           "[--json out.json]\n");
      return 2;
    }
    vp::Result<vp::lex::Lexicon> lx = vp::lex::Lexicon::open(vp::fs::u8path(lexPath));
    if (!lx) {
      std::fprintf(stderr, "llm-gate: %s: %s\n", vp::errorCodeName(lx.error().code), lx.error().message.c_str());
      return 1;
    }
    opt.model.path = modelPath;
    opt.progress = [](int done, int total) {
      if (done % 100 == 0 || done == total) std::fprintf(stderr, "llm-gate: %d/%d pairs scored\n", done, total);
    };
    vp::Result<vp::llm::GateResult> r = vp::llm::runGate(lx.value(), opt);
    if (!r) {
      std::fprintf(stderr, "llm-gate: %s: %s (%s)\n", vp::errorCodeName(r.error().code), r.error().message.c_str(),
                   r.error().hint.c_str());
      return 1;
    }
    const vp::llm::GateResult& g = r.value();
    const int n = static_cast<int>(g.pairs.size());
    std::map<std::string, std::pair<int, int>> byTemplate, byKind;   // name -> (right, total)
    for (const vp::llm::GatePair& p : g.pairs) {
      const int right = p.goodLp > p.badLp ? 1 : 0;
      byTemplate[p.templ].first += right;
      byTemplate[p.templ].second += 1;
      byKind[p.corruption].first += right;
      byKind[p.corruption].second += 1;
    }
    std::printf("pairs %d (requested %d, seed %llu)\n", n, opt.pairs, static_cast<unsigned long long>(opt.seed));
    std::printf("summed log-prob (primary): %d/%d = %.1f %% (95 %% Wilson %.1f-%.1f %%)\n", g.correct, n,
                100.0 * g.sum.p, 100.0 * g.sum.lo, 100.0 * g.sum.hi);
    std::printf("mean log-prob per token:   %d/%d = %.1f %% (95 %% Wilson %.1f-%.1f %%)\n", g.correctMean, n,
                100.0 * g.mean.p, 100.0 * g.mean.lo, 100.0 * g.mean.hi);
    for (const auto& t : byTemplate)
      std::printf("  template %-8s %d/%d\n", t.first.c_str(), t.second.first, t.second.second);
    for (const auto& k : byKind) std::printf("  corruption %-7s %d/%d\n", k.first.c_str(), k.second.first, k.second.second);
    std::printf("load %d ms, %.1f ms per pair\n", g.loadMs, g.msPerPair);
    std::printf("gate (>= 75 %% and lower bound > 70 %%): %s\n", g.passed ? "PASSED" : "NOT PASSED");
    if (!jsonPath.empty()) {
      json pairs = json::array();
      for (const vp::llm::GatePair& p : g.pairs)
        pairs.push_back(json{{"template", p.templ}, {"corruption", p.corruption}, {"good", p.good}, {"bad", p.bad},
                             {"goodLp", p.goodLp}, {"badLp", p.badLp}, {"goodTokens", p.goodTokens},
                             {"badTokens", p.badTokens}});
      json out{{"requested", opt.pairs}, {"seed", opt.seed}, {"threads", opt.model.threads}, {"pairs", n},
               {"correct", g.correct}, {"correctMean", g.correctMean}, {"sum", intervalJson(g.sum)},
               {"mean", intervalJson(g.mean)}, {"msPerPair", g.msPerPair}, {"loadMs", g.loadMs}, {"passed", g.passed},
               {"items", std::move(pairs)}};
      vp::Result<void> w = vp::fs::writeFileAtomic(jsonPath, out.dump(1));
      if (!w) {
        std::fprintf(stderr, "llm-gate: %s\n", w.error().message.c_str());
        return 1;
      }
    }
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "llm-gate: %s\n", e.what());
  } catch (...) {
    std::fprintf(stderr, "llm-gate: unknown error\n");
  }
  return 1;
}

}  // namespace vpcli
