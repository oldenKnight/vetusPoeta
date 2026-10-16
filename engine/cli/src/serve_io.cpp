// Platform plumbing for `vpengine serve`. See serve_io.h.
#include "serve_io.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#else
#include <cerrno>
#include <csignal>
#include <poll.h>
#include <unistd.h>
#endif

namespace vpcli {

namespace {
std::mutex gLogMu;
std::atomic<int> gLogLevel{static_cast<int>(LogLevel::Info)};
std::atomic<bool> gStop{false};

const char* levelName(LogLevel l) {
  switch (l) {
    case LogLevel::Error: return "error";
    case LogLevel::Warn: return "warn";
    case LogLevel::Info: return "info";
    case LogLevel::Debug: return "debug";
    default: return "";
  }
}

#if defined(_WIN32)
BOOL WINAPI consoleHandler(DWORD type) {
  switch (type) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
      gStop.store(true);
      Sleep(2000);   // the process ends when this returns: give the main loop time to autosave and unlock
      return TRUE;
    default:
      return FALSE;
  }
}
#else
extern "C" void onStopSignal(int) { gStop.store(true); }
#endif
}  // namespace

void initLogLevel() {
  const std::string v = envU8("VP_LOG");
  LogLevel l = LogLevel::Info;
  if (v == "off" || v == "none") l = LogLevel::Off;
  else if (v == "error") l = LogLevel::Error;
  else if (v == "warn" || v == "warning") l = LogLevel::Warn;
  else if (v == "debug") l = LogLevel::Debug;
  gLogLevel.store(static_cast<int>(l));
}

bool logEnabled(LogLevel l) { return static_cast<int>(l) <= gLogLevel.load() && l != LogLevel::Off; }

void logMsg(LogLevel l, const std::string& msg) {
  if (!logEnabled(l)) return;
  try {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const int ms = static_cast<int>(duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000);
    const std::time_t t = system_clock::to_time_t(now);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char stamp[64];
    std::snprintf(stamp, sizeof stamp, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", tm.tm_year + 1900, tm.tm_mon + 1,
                  tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, ms);
    std::lock_guard<std::mutex> lk(gLogMu);
    std::fprintf(stderr, "%s [vpengine] %s: %s\n", stamp, levelName(l), msg.c_str());
    std::fflush(stderr);
  } catch (...) {
  }
}

int64_t monoMs() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

int hardwareThreads() {
  const unsigned n = std::thread::hardware_concurrency();
  return n == 0 ? 2 : static_cast<int>(std::min(n, 64u));
}

std::string exeDir() {
  try {
#if defined(_WIN32)
    std::wstring buf(32768, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, &buf[0], static_cast<DWORD>(buf.size()));
    if (n == 0 || n >= buf.size()) return std::string();
    buf.resize(n);
    auto u8 = std::filesystem::path(buf).parent_path().u8string();
    return std::string(u8.begin(), u8.end());
#else
    std::error_code ec;
    std::filesystem::path p = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (ec) return std::string();
    return p.parent_path().string();
#endif
  } catch (...) {
    return std::string();
  }
}

std::string envU8(const char* name) {
#if defined(_WIN32)
  std::wstring wn(name, name + std::strlen(name));
  DWORD n = GetEnvironmentVariableW(wn.c_str(), nullptr, 0);
  if (n == 0) return std::string();
  std::wstring w(n, L'\0');
  n = GetEnvironmentVariableW(wn.c_str(), &w[0], n);
  w.resize(n);
  const int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
  std::string s(len > 0 ? static_cast<size_t>(len) : 0, '\0');
  if (len > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), &s[0], len, nullptr, nullptr);
  return s;
#else
  const char* v = std::getenv(name);
  return v ? std::string(v) : std::string();
#endif
}

void setupStdio() {
#if defined(_WIN32)
  _setmode(_fileno(stdout), _O_BINARY);
  _setmode(_fileno(stdin), _O_BINARY);
#else
  std::signal(SIGPIPE, SIG_IGN);
#endif
}

void installStopHandlers() {
#if defined(_WIN32)
  SetConsoleCtrlHandler(consoleHandler, TRUE);
#else
  struct sigaction sa {};
  sa.sa_handler = onStopSignal;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  sigaction(SIGTERM, &sa, nullptr);
  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGHUP, &sa, nullptr);
#endif
}

bool stopRequested() { return gStop.load(); }

// ---- Output ----
void Output::send(const json& j) {
  try {
    std::lock_guard<std::mutex> lk(mu_);
    if (broken_.load()) return;
    buf_ = j.dump(-1, ' ', false, json::error_handler_t::replace);
    buf_.push_back('\n');
    if (std::fwrite(buf_.data(), 1, buf_.size(), stdout) != buf_.size() || std::fflush(stdout) != 0)
      broken_.store(true);
    lines_.fetch_add(1);
    if (buf_.capacity() > (1u << 20) && buf_.size() < (1u << 16)) buf_.shrink_to_fit();
  } catch (...) {
    broken_.store(true);
  }
}

// ---- LineReader ----
LineReader::LineReader(size_t maxLine, std::function<void(std::string&&)> onLine, std::function<void()> onTooLong,
                       std::function<void()> onEof)
    : maxLine_(maxLine), onLine_(std::move(onLine)), onTooLong_(std::move(onTooLong)), onEof_(std::move(onEof)) {}

LineReader::~LineReader() { stop(); }

void LineReader::start() { th_ = std::thread([this] { loop(); }); }

void LineReader::deliver(const char* p, size_t n) {
  while (n > 0) {
    const char* nl = static_cast<const char*>(std::memchr(p, '\n', n));
    const size_t take = nl ? static_cast<size_t>(nl - p) : n;
    if (!overflow_) {
      if (line_.size() + take > maxLine_) {
        overflow_ = true;
        line_.clear();
        line_.shrink_to_fit();
      } else {
        line_.append(p, take);
      }
    }
    if (!nl) return;
    if (overflow_) {
      overflow_ = false;
      if (onTooLong_) onTooLong_();
    } else {
      if (!line_.empty() && line_.back() == '\r') line_.pop_back();
      if (line_.find_first_not_of(" \t") != std::string::npos) {
        std::string l;
        l.swap(line_);
        onLine_(std::move(l));
        if (line_.capacity() > (1u << 20)) line_.shrink_to_fit();
      } else {
        line_.clear();
      }
    }
    p = nl + 1;
    n -= take + 1;
  }
}

void LineReader::loop() {
  try {
    std::vector<char> buf(64 * 1024);
#if defined(_WIN32)
    HANDLE self = nullptr;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &self, 0, FALSE,
                    DUPLICATE_SAME_ACCESS);
    threadHandle_.store(self);
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    while (!stop_.load()) {
      DWORD n = 0;
      if (!ReadFile(in, buf.data(), static_cast<DWORD>(buf.size()), &n, nullptr)) {
        const DWORD e = GetLastError();
        if (e == ERROR_OPERATION_ABORTED && !stop_.load()) continue;
        break;   // broken pipe = EOF, or stop requested
      }
      if (n == 0) {
        if (GetFileType(in) == FILE_TYPE_PIPE) break;
        continue;
      }
      deliver(buf.data(), n);
    }
#else
    while (!stop_.load()) {
      pollfd pf{};
      pf.fd = STDIN_FILENO;
      pf.events = POLLIN;
      const int r = ::poll(&pf, 1, 200);
      if (r < 0) {
        if (errno == EINTR) continue;
        break;
      }
      if (r == 0) continue;
      const ssize_t n = ::read(STDIN_FILENO, buf.data(), buf.size());
      if (n < 0) {
        if (errno == EINTR || errno == EAGAIN) continue;
        break;
      }
      if (n == 0) break;   // EOF
      deliver(buf.data(), static_cast<size_t>(n));
    }
#endif
  } catch (...) {
    logMsg(LogLevel::Error, "stdin reader failed");
  }
  const bool eof = !stop_.load();
  done_.store(true);
  if (eof && onEof_) onEof_();
}

void LineReader::stop() {
  if (!th_.joinable()) return;
  stop_.store(true);
#if defined(_WIN32)
  for (int i = 0; i < 200 && !done_.load(); ++i) {
    HANDLE h = static_cast<HANDLE>(threadHandle_.load());
    if (h) CancelSynchronousIo(h);
    Sleep(10);
  }
  if (!done_.load()) {
    logMsg(LogLevel::Warn, "stdin reader did not stop; exiting");
    std::fflush(stdout);
    ExitProcess(0);
  }
  th_.join();
  HANDLE h = static_cast<HANDLE>(threadHandle_.exchange(nullptr));
  if (h) CloseHandle(h);
#else
  th_.join();
#endif
}

}  // namespace vpcli
