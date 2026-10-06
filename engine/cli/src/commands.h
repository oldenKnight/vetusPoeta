// vpengine subcommands. Each returns the process exit code and never throws.
#pragma once
#include <string>
#include <vector>

namespace vpcli {
using Args = std::vector<std::string>;
int cmdServe(const Args& args);     // serve [--data <dir>] [--lexicons <dir>] [--stub]
int cmdInspect(const Args& args);   // inspect <file.vpl> <word>
int cmdCheck(const Args& args);     // check <subtitle file> [--cps N]
int cmdLlmGate(const Args& args);   // llm-gate <latin.vpl> <model.gguf> [--pairs N] [--threads T] [--seed S] [--json f]
}  // namespace vpcli
