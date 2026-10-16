// Read-only memory-mapped file. See vp/mmap.h.
#include "vp/mmap.h"

#include <utility>

#include "vp/fs.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace vp {

namespace {
const uint8_t kEmpty[1] = {0};
const char kMapHint[] = "The file is missing, locked by another program or unreadable. Reinstall or locate it again.";

#if defined(_WIN32)
struct Handle {
  HANDLE h = nullptr;
  explicit Handle(HANDLE x) : h(x == INVALID_HANDLE_VALUE ? nullptr : x) {}
  ~Handle() { if (h) CloseHandle(h); }
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
};
#else
struct Fd {
  int fd = -1;
  explicit Fd(int x) : fd(x) {}
  ~Fd() { if (fd >= 0) ::close(fd); }
  Fd(const Fd&) = delete;
  Fd& operator=(const Fd&) = delete;
};
#endif
}  // namespace

MappedFile::MappedFile(MappedFile&& o) noexcept
    : data_(o.data_), size_(o.size_), mapped_(o.mapped_), path_(std::move(o.path_)) {
  o.data_ = nullptr;
  o.size_ = 0;
  o.mapped_ = false;
}

MappedFile& MappedFile::operator=(MappedFile&& o) noexcept {
  if (this != &o) {
    close();
    data_ = o.data_;
    size_ = o.size_;
    mapped_ = o.mapped_;
    path_ = std::move(o.path_);
    o.data_ = nullptr;
    o.size_ = 0;
    o.mapped_ = false;
  }
  return *this;
}

void MappedFile::close() {
  if (mapped_ && data_) {
#if defined(_WIN32)
    UnmapViewOfFile(data_);
#else
    ::munmap(const_cast<uint8_t*>(data_), size_);
#endif
  }
  data_ = nullptr;
  size_ = 0;
  mapped_ = false;
}

Result<MappedFile> MappedFile::open(const std::string& path) {
  try {
    MappedFile m;
    m.path_ = path;
#if defined(_WIN32)
    std::filesystem::path wp = fs::u8path(path);
    if (wp.empty()) return Result<MappedFile>(ErrorCode::Io, "invalid file name '" + path + "'", kMapHint);
    Handle file(CreateFileW(wp.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file.h)
      return Result<MappedFile>(ErrorCode::Io,
                                "cannot open '" + path + "' (Windows error " + std::to_string(GetLastError()) + ")",
                                kMapHint);
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(file.h, &sz) || sz.QuadPart < 0)
      return Result<MappedFile>(ErrorCode::Io, "cannot read the size of '" + path + "'", kMapHint);
    if (static_cast<unsigned long long>(sz.QuadPart) > static_cast<unsigned long long>(SIZE_MAX))
      return Result<MappedFile>(ErrorCode::Io, "'" + path + "' is too large to map", kMapHint);
    if (sz.QuadPart == 0) {
      m.data_ = kEmpty;
      return Result<MappedFile>(std::move(m));
    }
    Handle mapping(CreateFileMappingW(file.h, nullptr, PAGE_READONLY, 0, 0, nullptr));
    if (!mapping.h)
      return Result<MappedFile>(ErrorCode::Io,
                                "cannot map '" + path + "' (Windows error " + std::to_string(GetLastError()) + ")",
                                kMapHint);
    void* view = MapViewOfFile(mapping.h, FILE_MAP_READ, 0, 0, 0);
    if (!view)
      return Result<MappedFile>(ErrorCode::Io,
                                "cannot map '" + path + "' (Windows error " + std::to_string(GetLastError()) + ")",
                                kMapHint);
    m.data_ = static_cast<const uint8_t*>(view);
    m.size_ = static_cast<size_t>(sz.QuadPart);
    m.mapped_ = true;
    return Result<MappedFile>(std::move(m));
#else
    Fd fd(::open(path.c_str(), O_RDONLY | O_CLOEXEC));
    if (fd.fd < 0)
      return Result<MappedFile>(ErrorCode::Io, "cannot open '" + path + "': " + fs::errnoText(errno), kMapHint);
    struct stat st;
    if (::fstat(fd.fd, &st) != 0 || !S_ISREG(st.st_mode))
      return Result<MappedFile>(ErrorCode::Io, "'" + path + "' is not a regular file", kMapHint);
    if (static_cast<unsigned long long>(st.st_size) > static_cast<unsigned long long>(SIZE_MAX))
      return Result<MappedFile>(ErrorCode::Io, "'" + path + "' is too large to map", kMapHint);
    if (st.st_size == 0) {
      m.data_ = kEmpty;
      return Result<MappedFile>(std::move(m));
    }
    const size_t size = static_cast<size_t>(st.st_size);
    void* p = ::mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd.fd, 0);
    if (p == MAP_FAILED)
      return Result<MappedFile>(ErrorCode::Io, "cannot map '" + path + "': " + fs::errnoText(errno), kMapHint);
    m.data_ = static_cast<const uint8_t*>(p);
    m.size_ = size;
    m.mapped_ = true;
    return Result<MappedFile>(std::move(m));
#endif
  } catch (...) {
    return Result<MappedFile>(ErrorCode::Io, "cannot map '" + path + "'", kMapHint);
  }
}

}  // namespace vp
