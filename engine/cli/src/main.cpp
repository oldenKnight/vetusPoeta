// vpengine: command-line entry point. `serve` (JSON lines, DESIGN §9), `inspect`, `check`, `version`.
#include <cstdio>
#include <exception>
#include <string>
#include <vector>

#include "commands.h"
#include "vp/project.h"

#if defined(_WIN32)
#include <windows.h>
#include <shellapi.h>
#endif

namespace {

void usage() {
  std::fprintf(stderr,
               "vetus poeta engine %s\n"
               "usage:\n"
               "  vpengine serve [--data <dir>] [--lexicons <dir>]   JSON lines on stdin/stdout (DESIGN section 9)\n"
               "  vpengine inspect <file.vpl> <word>                 analyses and paradigm cells of a word\n"
               "  vpengine check <file.srt|.vtt|.ass|.txt> [--cps N] format, encoding, cues, warnings, fast cues\n"
               "  vpengine version\n"
               "environment: VP_LOG=off|error|warn|info|debug, VP_DATA_DIR, VP_LEXICON_DIR, VP_AUTOSAVE_MS\n",
               vp::appVersion());
}

// Arguments as UTF-8 on every platform (Windows: rebuilt from the wide command line).
std::vector<std::string> utf8Args(int argc, char** argv) {
  std::vector<std::string> out;
#if defined(_WIN32)
  int n = 0;
  LPWSTR* w = CommandLineToArgvW(GetCommandLineW(), &n);
  if (w) {
    for (int i = 0; i < n; ++i) {
      const int len = WideCharToMultiByte(CP_UTF8, 0, w[i], -1, nullptr, 0, nullptr, nullptr);
      std::string s(len > 0 ? static_cast<size_t>(len) : 0, '\0');
      if (len > 0) WideCharToMultiByte(CP_UTF8, 0, w[i], -1, &s[0], len, nullptr, nullptr);
      if (!s.empty() && s.back() == '\0') s.pop_back();
      out.push_back(s);
    }
    LocalFree(w);
    return out;
  }
#endif
  for (int i = 0; i < argc; ++i) out.emplace_back(argv[i]);
  return out;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const std::vector<std::string> args = utf8Args(argc, argv);
    if (args.size() < 2) {
      usage();
      return 2;
    }
    const std::string& cmd = args[1];
    const vpcli::Args rest(args.begin() + 2, args.end());
    if (cmd == "version" || cmd == "--version") {
      std::printf("vetus poeta engine %s\n", vp::appVersion());
      return 0;
    }
    if (cmd == "help" || cmd == "--help" || cmd == "-h") {
      usage();
      return 0;
    }
    if (cmd == "serve") return vpcli::cmdServe(rest);
    if (cmd == "inspect") return vpcli::cmdInspect(rest);
    if (cmd == "check") return vpcli::cmdCheck(rest);
    std::fprintf(stderr, "unknown command: %s\n", cmd.c_str());
    usage();
    return 2;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "fatal: %s\n", e.what());
  } catch (...) {
    std::fprintf(stderr, "fatal: unknown error\n");
  }
  return 1;
}
