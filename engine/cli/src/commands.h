// vpengine subcommands. Each returns the process exit code and never throws.
#pragma once
#include <string>
#include <vector>

namespace vpcli {
using Args = std::vector<std::string>;
int cmdServe(const Args& args);     // serve [--data <dir>] [--lexicons <dir>]
int cmdInspect(const Args& args);   // inspect <file.vpl> <word>
int cmdCheck(const Args& args);     // check <subtitle file> [--cps N]
}  // namespace vpcli
