// `vpengine serve`: wires the stdin reader, the protocol output and the server's worker loop.
#include <exception>

#include "commands.h"
#include "locate.h"
#include "serve_io.h"
#include "server.h"
#include "vp/fs.h"

namespace vpcli {

namespace {
constexpr size_t kMaxLine = 16u << 20;   // 16 MiB per request line
}

int cmdServe(const Args& args) {
  try {
    initLogLevel();
    ServeOptions opt;
    const std::string forceStub = envU8("VP_FORCE_STUB");
    opt.stub = !forceStub.empty() && forceStub != "0";
    for (size_t i = 0; i < args.size(); ++i) {
      if (args[i] == "--stub") {
        opt.stub = true;
      } else if ((args[i] == "--data" || args[i] == "--lexicons") && i + 1 < args.size()) {
        (args[i] == "--data" ? opt.dataDir : opt.lexiconDir) = args[i + 1];
        ++i;
      } else {
        logMsg(LogLevel::Error, "unknown argument: " + args[i]);
        return 2;
      }
    }
    if (opt.dataDir.empty()) opt.dataDir = vp::fs::dataDir();
    if (!vp::fs::createDirectories(opt.dataDir)) logMsg(LogLevel::Warn, "cannot create the data folder " + opt.dataDir);
    if (opt.lexiconDir.empty()) opt.lexiconDir = defaultLexiconDir(opt.dataDir);
    setupStdio();
    installStopHandlers();
    Output out;
    Server server(out, opt);
    LineReader reader(
        kMaxLine, [&server](std::string&& line) { server.onLine(std::move(line)); },
        [&server] { server.onLineTooLong(); }, [&server] { server.onEof(); });
    reader.start();
    const int code = server.run();
    reader.stop();
    logMsg(LogLevel::Info, "bye");
    return code;
  } catch (const std::exception& e) {
    logMsg(LogLevel::Error, std::string("fatal: ") + e.what());
  } catch (...) {
    logMsg(LogLevel::Error, "fatal: unknown error");
  }
  return 1;
}

}  // namespace vpcli
