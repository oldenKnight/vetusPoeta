// `vpengine serve` plumbing: stderr log, protocol output, stdin line reader and the platform specifics (binary
// pipes, termination signals, executable folder). Everything that differs between Windows and POSIX is in serve_io.cpp.
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "json.hpp"

namespace vpcli {

using json = nlohmann::json;

// ---- log (stderr only; stdout carries nothing but protocol lines) ----
enum class LogLevel { Off = 0, Error = 1, Warn = 2, Info = 3, Debug = 4 };
void initLogLevel();   // from VP_LOG: off | error | warn | info (default) | debug
bool logEnabled(LogLevel l);
void logMsg(LogLevel l, const std::string& msg);   // "2026-10-06T10:00:00.000Z [vpengine] info: msg"

int64_t monoMs();      // steady clock
int hardwareThreads();
std::string exeDir();  // folder of the running executable ("" when unknown)
std::string envU8(const char* name);   // "" when unset

// ---- platform ----
void setupStdio();            // binary stdin/stdout on Windows; SIGPIPE ignored on POSIX
void installStopHandlers();   // SIGTERM/SIGINT/SIGHUP, console close events
bool stopRequested();

// ---- protocol output: one JSON object per line, flushed, thread-safe, never throws ----
class Output {
 public:
  void send(const json& j);
  bool broken() const { return broken_.load(); }
  uint64_t lines() const { return lines_.load(); }

 private:
  std::mutex mu_;
  std::string buf_;
  std::atomic<bool> broken_{false};
  std::atomic<uint64_t> lines_{0};
};

// ---- stdin reader: complete lines go to onLine on the reader's own thread ----
// Lines longer than maxLine are dropped and reported through onTooLong. stop() joins the thread.
class LineReader {
 public:
  LineReader(size_t maxLine, std::function<void(std::string&&)> onLine, std::function<void()> onTooLong,
             std::function<void()> onEof);
  ~LineReader();
  LineReader(const LineReader&) = delete;
  LineReader& operator=(const LineReader&) = delete;
  void start();
  void stop();   // idempotent

 private:
  void loop();
  void deliver(const char* p, size_t n);
  size_t maxLine_;
  std::function<void(std::string&&)> onLine_;
  std::function<void()> onTooLong_, onEof_;
  std::atomic<bool> stop_{false}, done_{false};
  std::string line_;
  bool overflow_ = false;
  std::thread th_;
#if defined(_WIN32)
  std::atomic<void*> threadHandle_{nullptr};
#endif
};

}  // namespace vpcli
