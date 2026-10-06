// DEV-ONLY transport for Linux/macOS builds: runs the `curl` program and parses its output. It exists so the real
// Wiktionary client can be exercised on the build box; it is never shipped (the product is the Windows build, which
// uses transport_winhttp.cpp). No socket code is linked: the network is curl's business.
#ifndef _WIN32

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "vp/fs.h"
#include "vp/online.h"

extern char** environ;

namespace vp::online {

namespace {

constexpr size_t kMaxOutput = 6u * 1024 * 1024;

struct Fd {
  int fd = -1;
  Fd() = default;
  explicit Fd(int f) : fd(f) {}
  Fd(const Fd&) = delete;
  Fd& operator=(const Fd&) = delete;
  ~Fd() { reset(); }
  void reset() {
    if (fd >= 0) ::close(fd);
    fd = -1;
  }
};

struct Pipe {
  Fd r, w;
  bool open() {
    int p[2];
    if (::pipe(p) != 0) return false;
    r.fd = p[0];
    w.fd = p[1];
    ::fcntl(r.fd, F_SETFD, FD_CLOEXEC);
    ::fcntl(w.fd, F_SETFD, FD_CLOEXEC);
    return true;
  }
};

struct FileActions {
  posix_spawn_file_actions_t fa;
  bool ok;
  FileActions() { ok = posix_spawn_file_actions_init(&fa) == 0; }
  ~FileActions() {
    if (ok) posix_spawn_file_actions_destroy(&fa);
  }
};

// The child is killed and reaped on every path out of get().
struct Child {
  pid_t pid = -1;
  int status = 0;
  ~Child() {
    if (pid > 0) {
      ::kill(pid, SIGKILL);
      wait();
    }
  }
  void wait() {
    while (pid > 0 && ::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    pid = -1;
  }
};

int64_t nowMs() { return fs::nowUnixMs(); }

std::string lower(std::string s) {
  for (char& c : s)
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  return s;
}

// curl -i output: one or more header blocks ("HTTP/x 1xx" interim, redirects are not followed) then the body.
bool parseCurlOutput(const std::string& out, Response& r) {
  size_t pos = 0;
  for (;;) {
    if (out.compare(pos, 5, "HTTP/") != 0) return false;
    size_t end = out.find("\r\n\r\n", pos);
    size_t sepLen = 4;
    if (end == std::string::npos) {
      end = out.find("\n\n", pos);
      sepLen = 2;
    }
    if (end == std::string::npos) end = out.size(), sepLen = 0;
    const std::string block = out.substr(pos, end - pos);
    const size_t sp = block.find(' ');
    if (sp == std::string::npos) return false;
    r.status = std::atoi(block.c_str() + sp + 1);
    r.headers.clear();
    size_t line = block.find('\n');
    while (line != std::string::npos) {
      const size_t next = block.find('\n', line + 1);
      std::string h = block.substr(line + 1, next == std::string::npos ? std::string::npos : next - line - 1);
      if (!h.empty() && h.back() == '\r') h.pop_back();
      const size_t colon = h.find(':');
      if (colon != std::string::npos) {
        size_t v = colon + 1;
        while (v < h.size() && h[v] == ' ') ++v;
        r.headers[lower(h.substr(0, colon))] = h.substr(v);
      }
      line = next;
    }
    pos = end + sepLen;
    if (r.status >= 100 && r.status < 200 && pos < out.size()) continue;
    r.body = pos <= out.size() ? out.substr(pos) : std::string();
    return r.status > 0;
  }
}

class CurlDevTransport : public Transport {
 public:
  Result<Response> get(const std::string& url, int timeoutMs, const Headers& requestHeaders) override {
    try {
      return run(url, timeoutMs, requestHeaders);
    } catch (...) {
      return Result<Response>(ErrorCode::OnlineFailed, "dev transport: unexpected failure");
    }
  }

 private:
  Result<Response> run(const std::string& url, int timeoutMs, const Headers& requestHeaders) {
    if (url.compare(0, 8, "https://") != 0) return Result<Response>(ErrorCode::BadParams, "dev transport: https only");
    const int secs = std::max(1, (timeoutMs + 999) / 1000);
    std::vector<std::string> args = {"curl", "-sS", "-i", "--suppress-connect-headers", "--proto", "=https",
                                     "--max-time", std::to_string(secs), "--max-filesize", std::to_string(kMaxOutput)};
    for (const auto& h : requestHeaders) {
      args.push_back("-H");
      args.push_back(h.first + ": " + h.second);
    }
    args.push_back("--url");
    args.push_back(url);
    std::vector<char*> argv;
    for (std::string& a : args) argv.push_back(&a[0]);
    argv.push_back(nullptr);

    Pipe out, err;
    if (!out.open() || !err.open()) return Result<Response>(ErrorCode::OnlineFailed, "dev transport: pipe failed");
    FileActions fa;
    if (!fa.ok) return Result<Response>(ErrorCode::OnlineFailed, "dev transport: spawn setup failed");
    posix_spawn_file_actions_addopen(&fa.fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&fa.fa, out.w.fd, 1);
    posix_spawn_file_actions_adddup2(&fa.fa, err.w.fd, 2);
    Child child;
    const int rc = posix_spawnp(&child.pid, "curl", &fa.fa, nullptr, argv.data(), environ);
    if (rc != 0) {
      child.pid = -1;
      return Result<Response>(ErrorCode::OnlineFailed,
                              rc == ENOENT ? "dev transport: curl not found" : "dev transport: cannot run curl: " +
                                                                                   std::string(std::strerror(rc)));
    }
    out.w.reset();
    err.w.reset();

    std::string o, e;
    const int64_t deadline = nowMs() + timeoutMs + 3000;   // curl's own --max-time fires first
    bool outOpen = true, errOpen = true;
    char buf[16384];
    while (outOpen || errOpen) {
      const int64_t left = deadline - nowMs();
      if (left <= 0) return Result<Response>(ErrorCode::OnlineFailed, "timed out after " + std::to_string(timeoutMs) + " ms");
      pollfd p[2];
      int n = 0;
      if (outOpen) p[n++] = {out.r.fd, POLLIN, 0};
      if (errOpen) p[n++] = {err.r.fd, POLLIN, 0};
      const int pr = ::poll(p, static_cast<nfds_t>(n), static_cast<int>(std::min<int64_t>(left, 1000)));
      if (pr < 0 && errno != EINTR) return Result<Response>(ErrorCode::OnlineFailed, "dev transport: poll failed");
      for (int k = 0; k < n && pr > 0; ++k) {
        if (!(p[k].revents & (POLLIN | POLLHUP | POLLERR))) continue;
        const ssize_t got = ::read(p[k].fd, buf, sizeof buf);
        const bool isOut = p[k].fd == out.r.fd;
        if (got <= 0) {
          if (got < 0 && errno == EINTR) continue;
          (isOut ? outOpen : errOpen) = false;
        } else {
          std::string& dst = isOut ? o : e;
          dst.append(buf, static_cast<size_t>(got));
          if (dst.size() > kMaxOutput + 65536)
            return Result<Response>(ErrorCode::OnlineFailed, "dev transport: response too large");
        }
      }
    }
    child.wait();
    const int code = WIFEXITED(child.status) ? WEXITSTATUS(child.status) : -1;
    if (code != 0) {
      while (!e.empty() && (e.back() == '\n' || e.back() == '\r')) e.pop_back();
      if (code == 28) return Result<Response>(ErrorCode::OnlineFailed, "timed out after " + std::to_string(timeoutMs) + " ms");
      return Result<Response>(ErrorCode::OnlineFailed,
                              "dev transport: curl exit " + std::to_string(code) + (e.empty() ? "" : ": " + e.substr(0, 200)));
    }
    Response r;
    if (!parseCurlOutput(o, r)) return Result<Response>(ErrorCode::OnlineFailed, "dev transport: unreadable curl output");
    return r;
  }
};

bool curlOnPath() {
  const char* path = std::getenv("PATH");
  if (!path) return false;
  std::string p(path);
  size_t start = 0;
  while (start <= p.size()) {
    size_t end = p.find(':', start);
    if (end == std::string::npos) end = p.size();
    const std::string dir = end > start ? p.substr(start, end - start) : std::string(".");
    if (::access((dir + "/curl").c_str(), X_OK) == 0) return true;
    start = end + 1;
  }
  return false;
}

}  // namespace

Result<std::unique_ptr<Transport>> makeSystemTransport(const Settings& settings) {
  if (!onlineAllowed(settings))
    return Result<std::unique_ptr<Transport>>(ErrorCode::OnlineDisabled, "online disabled",
                                              "Turn on the online check in Settings first.");
  if (!curlOnPath())
    return Result<std::unique_ptr<Transport>>(ErrorCode::OnlineFailed, "dev transport: curl not found",
                                              "This development build needs the curl program for the online check.");
  try {
    return std::unique_ptr<Transport>(std::make_unique<CurlDevTransport>());
  } catch (...) {
    return Result<std::unique_ptr<Transport>>(ErrorCode::Internal, "dev transport: out of memory", "Restart the program.");
  }
}

}  // namespace vp::online

#endif  // !_WIN32
