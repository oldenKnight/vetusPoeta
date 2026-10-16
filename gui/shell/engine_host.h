// Engine process host: spawns vpengine.exe (`serve`) with redirected pipes inside a kill-on-close
// job object, reads its stdout as JSON lines (reader thread -> bounded queue -> PostMessage to the
// UI thread), writes stdin from a writer thread (queue under a mutex), and appends stderr to the
// rotating log <data>\logs\engine.log. Supervision decisions (watchdog, restarts) live in app.cpp
// and host_logic.
#pragma once

#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>

#include "shell_util.h"

namespace vp {
namespace shell {

// <data>\logs\engine.log, rotated at 2 MB (kLogCapBytes), 2 old files kept.
class LogFile {
 public:
  void open(const std::wstring& path);
  void write(const char* data, size_t n);  // thread-safe
  void note(std::string_view text);        // "<timestamp> [shell] text\r\n"
  const std::wstring& path() const { return path_; }

 private:
  void reopenLocked();
  std::mutex mu_;
  UniqueHandle f_;
  uint64_t size_ = 0;
  std::wstring path_;
};

class EngineHost {
 public:
  // Posts msgOutput(wParam = generation) when new stdout lines are queued and
  // msgExited(wParam = generation) when the engine's stdout closes (the process ended).
  EngineHost(HWND notify, UINT msgOutput, UINT msgExited, LogFile& log)
      : hwnd_(notify), msgOutput_(msgOutput), msgExited_(msgExited), log_(log) {}
  ~EngineHost() { kill(); }
  EngineHost(const EngineHost&) = delete;
  EngineHost& operator=(const EngineHost&) = delete;

  // `args` is the rest of the command line, already quoted (quoteWindowsArg).
  bool start(const std::wstring& exePath, const std::wstring& args, DWORD* winError);
  // Sends `shutdownLine`, closes stdin, waits up to waitMs for a clean exit, then kills.
  void shutdown(const std::string& shutdownLine, DWORD waitMs);
  void kill();  // terminates the engine (and anything it started) and joins all threads
  bool send(std::string line);  // queue one line for stdin; false when stopped or full
  void takeOutput(std::deque<std::string>& out);  // UI thread: moves queued stdout lines out
  bool running() const { return proc_.valid(); }
  bool processExited() const;
  unsigned generation() const { return gen_; }

 private:
  void readStdout(unsigned gen);
  void readStderr();
  void writeStdin();
  void stopThreads();

  static constexpr size_t kOutMaxBytes = size_t(256) << 20;
  static constexpr size_t kOutMaxLines = 8192;
  static constexpr size_t kInMaxBytes = size_t(64) << 20;
  static constexpr size_t kInMaxLines = 4096;

  HWND hwnd_;
  UINT msgOutput_, msgExited_;
  LogFile& log_;
  unsigned gen_ = 0;
  UniqueHandle job_, proc_, stdinW_, stdoutR_, stderrR_;
  JoinedThread outThread_, errThread_, inThread_;
  std::atomic<bool> stopping_{false};

  std::mutex outMu_;
  std::condition_variable outCv_;
  std::deque<std::string> outQ_;
  size_t outBytes_ = 0;
  bool posted_ = false;

  std::mutex inMu_;
  std::condition_variable inCv_;
  std::deque<std::string> inQ_;
  size_t inBytes_ = 0;
  bool closeStdin_ = false;
};

}  // namespace shell
}  // namespace vp
