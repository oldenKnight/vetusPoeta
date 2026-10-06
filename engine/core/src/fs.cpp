// Portable filesystem / process helpers. See vp/fs.h.
#include "vp/fs.h"

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>
#include <system_error>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <io.h>
#else
#include <fcntl.h>
#include <pwd.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace vp {
namespace fs {

namespace stdfs = std::filesystem;

stdfs::path u8path(const std::string& s) {
  try {
    return stdfs::u8path(s);
  } catch (...) {
    return stdfs::path();
  }
}

std::string toU8(const stdfs::path& p) {
  try {
    auto s = p.u8string();
    return std::string(s.begin(), s.end());
  } catch (...) {
    return std::string();
  }
}

std::string join(const std::string& dir, const std::string& name) {
  if (dir.empty()) return name;
  const char last = dir.back();
  if (last == '/' || last == '\\') return dir + name;
#if defined(_WIN32)
  return dir + "\\" + name;
#else
  return dir + "/" + name;
#endif
}

std::string errnoText(int e) {
  const char* m = std::strerror(e);
  return m ? std::string(m) : std::string("error ") + std::to_string(e);
}

FilePtr openFile(const std::string& path, const char* mode) {
#if defined(_WIN32)
  std::wstring wmode;
  for (const char* c = mode; *c; ++c) wmode.push_back(static_cast<wchar_t>(*c));
  stdfs::path p = u8path(path);
  if (p.empty()) return FilePtr();
  return FilePtr(_wfopen(p.c_str(), wmode.c_str()));
#else
  return FilePtr(std::fopen(path.c_str(), mode));
#endif
}

bool flushAndSync(FILE* f) {
  if (!f) return false;
  if (std::fflush(f) != 0) return false;
#if defined(_WIN32)
  return _commit(_fileno(f)) == 0;
#else
  return fsync(fileno(f)) == 0;
#endif
}

namespace {

bool closeSynced(FilePtr& f) {
  if (!f) return false;
  bool ok = flushAndSync(f.get());
  FILE* raw = f.release();
  if (std::fclose(raw) != 0) ok = false;
  return ok;
}

#if !defined(_WIN32)
void syncParentDir(const std::string& path) {
  stdfs::path parent = u8path(path).parent_path();
  std::string dir = parent.empty() ? std::string(".") : toU8(parent);
  int fd = ::open(dir.c_str(), O_RDONLY);
  if (fd >= 0) {
    (void)fsync(fd);
    ::close(fd);
  }
}
#endif

const char kDiskHint[] = "Check that the folder exists, is not read-only and the disk is not full.";

}  // namespace

Result<void> replaceFile(const std::string& from, const std::string& to) {
#if defined(_WIN32)
  stdfs::path a = u8path(from), b = u8path(to);
  if (a.empty() || b.empty() || !MoveFileExW(a.c_str(), b.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    return Result<void>(ErrorCode::Io, "cannot replace '" + to + "' (Windows error " + std::to_string(GetLastError()) + ")",
                        kDiskHint);
  return Result<void>();
#else
  if (std::rename(from.c_str(), to.c_str()) != 0)
    return Result<void>(ErrorCode::Io, "cannot replace '" + to + "': " + errnoText(errno), kDiskHint);
  syncParentDir(to);
  return Result<void>();
#endif
}

Result<void> writeFileAtomic(const std::string& path, std::string_view data) {
  try {
    if (path.empty()) return Result<void>(ErrorCode::BadParams, "no file path given", "Choose a file name.");
    const std::string tmp = path + ".tmp";
    FilePtr f = openFile(tmp, "wb");
    if (!f) return Result<void>(ErrorCode::Io, "cannot create '" + tmp + "': " + errnoText(errno), kDiskHint);
    bool ok = data.empty() || std::fwrite(data.data(), 1, data.size(), f.get()) == data.size();
    int e = errno;
    if (!closeSynced(f)) {
      if (ok) e = errno;
      ok = false;
    }
    if (!ok) {
      removeQuiet(tmp);
      return Result<void>(ErrorCode::Io, "cannot write '" + tmp + "': " + errnoText(e), kDiskHint);
    }
    Result<void> r = replaceFile(tmp, path);
    if (!r) removeQuiet(tmp);
    return r;
  } catch (...) {
    return Result<void>(ErrorCode::Internal, "cannot write '" + path + "'", kDiskHint);
  }
}

Result<std::string> readFile(const std::string& path, uint64_t maxBytes) {
  try {
    if (!isRegularFile(path))
      return Result<std::string>(ErrorCode::NotFound, "file not found: '" + path + "'",
                                 "The file was moved, renamed or deleted.");
    Result<uint64_t> size = fileSize(path);
    if (!size) return Result<std::string>(size.error());
    if (size.value() > maxBytes)
      return Result<std::string>(ErrorCode::Io, "'" + path + "' is too large (" + std::to_string(size.value()) + " bytes)",
                                 "The file is larger than this program accepts.");
    FilePtr f = openFile(path, "rb");
    if (!f)
      return Result<std::string>(ErrorCode::Io, "cannot open '" + path + "': " + errnoText(errno),
                                 "Check that you may read the file and that no other program locks it.");
    std::string out(static_cast<size_t>(size.value()), '\0');
    const size_t got = out.empty() ? 0 : std::fread(&out[0], 1, out.size(), f.get());
    if (got != out.size())
      return Result<std::string>(ErrorCode::Io, "cannot read '" + path + "'", "The file changed or the disk failed.");
    return Result<std::string>(std::move(out));
  } catch (...) {
    return Result<std::string>(ErrorCode::Io, "out of memory reading '" + path + "'", "The file is too large.");
  }
}

bool fileExists(const std::string& path) {
  std::error_code ec;
  stdfs::path p = u8path(path);
  return !p.empty() && stdfs::exists(p, ec) && !ec;
}

bool isRegularFile(const std::string& path) {
  std::error_code ec;
  stdfs::path p = u8path(path);
  return !p.empty() && stdfs::is_regular_file(p, ec) && !ec;
}

bool isDirectory(const std::string& path) {
  std::error_code ec;
  stdfs::path p = u8path(path);
  return !p.empty() && stdfs::is_directory(p, ec) && !ec;
}

Result<uint64_t> fileSize(const std::string& path) {
  std::error_code ec;
  stdfs::path p = u8path(path);
  if (p.empty() || !stdfs::is_regular_file(p, ec) || ec)
    return Result<uint64_t>(ErrorCode::NotFound, "file not found: '" + path + "'", "The file was moved or deleted.");
  auto s = stdfs::file_size(p, ec);
  if (ec) return Result<uint64_t>(ErrorCode::Io, "cannot stat '" + path + "': " + ec.message(), kDiskHint);
  return Result<uint64_t>(static_cast<uint64_t>(s));
}

Result<int64_t> mtime(const std::string& path) {
#if defined(_WIN32)
  stdfs::path p = u8path(path);
  WIN32_FILE_ATTRIBUTE_DATA a;
  if (p.empty() || !GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &a))
    return Result<int64_t>(ErrorCode::NotFound, "file not found: '" + path + "'", "The file was moved or deleted.");
  const uint64_t t = (static_cast<uint64_t>(a.ftLastWriteTime.dwHighDateTime) << 32) | a.ftLastWriteTime.dwLowDateTime;
  const uint64_t epoch = 116444736000000000ull;  // 1601 -> 1970 in 100 ns units
  if (t < epoch) return Result<int64_t>(int64_t(0));
  return Result<int64_t>(static_cast<int64_t>((t - epoch) / 10000ull));
#else
  struct stat st;
  if (::stat(path.c_str(), &st) != 0)
    return Result<int64_t>(ErrorCode::NotFound, "file not found: '" + path + "'", "The file was moved or deleted.");
#if defined(__APPLE__)
  return Result<int64_t>(static_cast<int64_t>(st.st_mtimespec.tv_sec) * 1000 + st.st_mtimespec.tv_nsec / 1000000);
#else
  return Result<int64_t>(static_cast<int64_t>(st.st_mtim.tv_sec) * 1000 + st.st_mtim.tv_nsec / 1000000);
#endif
#endif
}

Result<void> createDirectories(const std::string& path) {
  std::error_code ec;
  stdfs::path p = u8path(path);
  if (p.empty()) return Result<void>(ErrorCode::BadParams, "invalid folder name '" + path + "'", kDiskHint);
  stdfs::create_directories(p, ec);
  if (ec || !stdfs::is_directory(p, ec))
    return Result<void>(ErrorCode::Io, "cannot create folder '" + path + "': " + ec.message(), kDiskHint);
  return Result<void>();
}

void removeQuiet(const std::string& path) {
  std::error_code ec;
  stdfs::path p = u8path(path);
  if (!p.empty()) stdfs::remove(p, ec);
}

Result<void> removeAll(const std::string& path) {
  std::error_code ec;
  stdfs::path p = u8path(path);
  if (!p.empty()) stdfs::remove_all(p, ec);
  if (ec) return Result<void>(ErrorCode::Io, "cannot delete '" + path + "': " + ec.message(), kDiskHint);
  return Result<void>();
}

namespace {
#if defined(_WIN32)
std::string envU8(const wchar_t* name) {
  wchar_t buf[32768];
  DWORD n = GetEnvironmentVariableW(name, buf, static_cast<DWORD>(sizeof(buf) / sizeof(buf[0])));
  if (n == 0 || n >= sizeof(buf) / sizeof(buf[0])) return std::string();
  return toU8(stdfs::path(std::wstring(buf, n)));
}
#else
std::string envU8(const char* name) {
  const char* v = std::getenv(name);
  return v ? std::string(v) : std::string();
}
#endif
}  // namespace

std::string dataDir() {
  try {
#if defined(_WIN32)
    std::string over = envU8(L"VP_DATA_DIR");
    if (!over.empty()) return over;
    std::string base = envU8(L"LOCALAPPDATA");
    if (base.empty()) base = envU8(L"APPDATA");
    if (base.empty()) base = tempDir();
    return join(base, "vetus-poeta");
#else
    std::string over = envU8("VP_DATA_DIR");
    if (!over.empty()) return over;
    std::string xdg = envU8("XDG_DATA_HOME");
    if (!xdg.empty() && xdg[0] == '/') return join(xdg, "vetus-poeta");
    std::string home = envU8("HOME");
    if (home.empty()) {
      const struct passwd* pw = getpwuid(getuid());
      if (pw && pw->pw_dir) home = pw->pw_dir;
    }
    if (home.empty()) home = tempDir();
    return join(join(join(home, ".local"), "share"), "vetus-poeta");
#endif
  } catch (...) {
    return std::string();
  }
}

std::string tempDir() {
  std::error_code ec;
  stdfs::path p = stdfs::temp_directory_path(ec);
  if (ec || p.empty()) {
#if defined(_WIN32)
    return "C:\\Windows\\Temp";
#else
    return "/tmp";
#endif
  }
  std::string s = toU8(p);
  while (s.size() > 1 && (s.back() == '/' || s.back() == '\\')) s.pop_back();
  return s;
}

uint64_t currentPid() {
#if defined(_WIN32)
  return static_cast<uint64_t>(GetCurrentProcessId());
#else
  return static_cast<uint64_t>(getpid());
#endif
}

#if defined(_WIN32)
namespace {
struct HandleCloser {
  HANDLE h = nullptr;
  explicit HandleCloser(HANDLE x) : h(x) {}
  ~HandleCloser() { if (h) CloseHandle(h); }
  HandleCloser(const HandleCloser&) = delete;
  HandleCloser& operator=(const HandleCloser&) = delete;
};
int64_t filetimeToUnixMs(const FILETIME& ft) {
  uint64_t t = (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
  const uint64_t epoch = 116444736000000000ull;
  if (t < epoch) return -1;
  return static_cast<int64_t>((t - epoch) / 10000ull);
}
}  // namespace
#endif

int64_t processStartMs(uint64_t pid) {
#if defined(_WIN32)
  if (pid == 0 || pid > 0xFFFFFFFFull) return -1;
  HandleCloser h(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid)));
  if (!h.h) return -1;
  FILETIME c, e, k, u;
  if (!GetProcessTimes(h.h, &c, &e, &k, &u)) return -1;
  return filetimeToUnixMs(c);
#elif defined(__linux__)
  try {
    if (pid == 0) return -1;
    std::ifstream st("/proc/" + std::to_string(pid) + "/stat");
    if (!st) return -1;
    std::string line;
    std::getline(st, line);
    size_t rp = line.rfind(')');
    if (rp == std::string::npos) return -1;
    std::istringstream fields(line.substr(rp + 1));
    std::string tok;
    unsigned long long startTicks = 0;
    for (int i = 3; i <= 22; ++i) {
      if (!(fields >> tok)) return -1;
      if (i == 22) startTicks = std::stoull(tok);
    }
    std::ifstream ps("/proc/stat");
    long long btime = -1;
    while (ps >> tok) {
      if (tok == "btime") {
        ps >> btime;
        break;
      }
    }
    long hz = sysconf(_SC_CLK_TCK);
    if (btime < 0 || hz <= 0) return -1;
    return btime * 1000 + static_cast<int64_t>(startTicks * 1000ull / static_cast<unsigned long long>(hz));
  } catch (...) {
    return -1;
  }
#else
  (void)pid;
  return -1;
#endif
}

bool isPidAlive(uint64_t pid, int64_t recordedStartMs) {
  if (pid == 0) return false;
#if defined(_WIN32)
  if (pid > 0xFFFFFFFFull) return false;
  HandleCloser h(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid)));
  if (!h.h) return GetLastError() == ERROR_ACCESS_DENIED;  // exists but not ours to inspect
  DWORD code = 0;
  if (!GetExitCodeProcess(h.h, &code)) return true;
  if (code != STILL_ACTIVE) return false;
#else
  if (pid > 0x7FFFFFFFull) return false;
  if (kill(static_cast<pid_t>(pid), 0) == -1 && errno == ESRCH) return false;
#endif
  if (recordedStartMs > 0) {
    int64_t actual = processStartMs(pid);
    if (actual > 0) {
      int64_t d = actual - recordedStartMs;
      if (d < 0) d = -d;
      if (d > 2000) return false;
    }
  }
  return true;
}

int64_t nowUnixMs() {
  using namespace std::chrono;
  return static_cast<int64_t>(duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
}

std::string isoUtc(int64_t unixMs) {
  std::time_t t = static_cast<std::time_t>(unixMs / 1000);
  std::tm tm{};
#if defined(_WIN32)
  if (gmtime_s(&tm, &t) != 0) return "1970-01-01T00:00:00Z";
#else
  if (gmtime_r(&t, &tm) == nullptr) return "1970-01-01T00:00:00Z";
#endif
  char buf[80];
  std::snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d:%02dZ", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                tm.tm_hour, tm.tm_min, tm.tm_sec);
  return buf;
}

}  // namespace fs
}  // namespace vp
