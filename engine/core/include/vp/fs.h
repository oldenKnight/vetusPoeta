// Portable filesystem and process helpers. Every path is a UTF-8 std::string; on Windows it is converted to a wide
// path (_wfopen, MoveFileExW, GetFileAttributesExW). Nothing here throws.
#pragma once
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

#include "vp/result.h"

namespace vp {
namespace fs {

std::filesystem::path u8path(const std::string& s);   // empty path on invalid UTF-8
std::string toU8(const std::filesystem::path& p);
std::string join(const std::string& dir, const std::string& name);   // dir + separator + name

struct FileCloser {
  void operator()(FILE* f) const { if (f) std::fclose(f); }
};
using FilePtr = std::unique_ptr<FILE, FileCloser>;

FilePtr openFile(const std::string& path, const char* mode);   // fopen / _wfopen
bool flushAndSync(FILE* f);                                     // fflush + fsync / _commit

constexpr uint64_t kDefaultMaxRead = 1ull << 30;   // 1 GiB
// Whole file into memory. not_found when missing, io when unreadable or larger than maxBytes.
Result<std::string> readFile(const std::string& path, uint64_t maxBytes = kDefaultMaxRead);
// Writes `<path>.tmp`, fsyncs it, renames it over `path` (MoveFileExW REPLACE_EXISTING|WRITE_THROUGH on Windows),
// fsyncs the directory on POSIX. On failure the tmp file is removed, `path` is untouched and the error is io.
Result<void> writeFileAtomic(const std::string& path, std::string_view data);
// Rename `from` over `to` atomically (same rules as above).
Result<void> replaceFile(const std::string& from, const std::string& to);

bool fileExists(const std::string& path);   // anything at that path
bool isRegularFile(const std::string& path);
bool isDirectory(const std::string& path);
Result<uint64_t> fileSize(const std::string& path);
// Last write time in Unix milliseconds.
Result<int64_t> mtime(const std::string& path);
Result<void> createDirectories(const std::string& path);
void removeQuiet(const std::string& path);          // remove a file, ignore errors
Result<void> removeAll(const std::string& path);    // recursive

// Per-user application data directory (not created): %LOCALAPPDATA%\vetus-poeta on Windows, else
// $XDG_DATA_HOME/vetus-poeta or ~/.local/share/vetus-poeta. The environment variable VP_DATA_DIR overrides it
// (tests and portable installs).
std::string dataDir();
std::string tempDir();

// ---- processes and clocks ----
uint64_t currentPid();
int64_t processStartMs(uint64_t pid);   // creation time in Unix ms, -1 when unknown
// True if a process with this pid runs. When recordedStartMs > 0 and the running process' creation time is known
// and differs by more than 2 s, the pid was reused by another process -> false.
bool isPidAlive(uint64_t pid, int64_t recordedStartMs = 0);
int64_t nowUnixMs();
std::string isoUtc(int64_t unixMs);   // "2026-10-06T10:15:00Z"
std::string errnoText(int e);

}  // namespace fs
}  // namespace vp
