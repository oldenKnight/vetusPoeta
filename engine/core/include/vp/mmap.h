// Read-only memory-mapped file (RAII). CreateFileMappingW/MapViewOfFile on Windows, mmap(PROT_READ) on POSIX.
// The file and mapping handles are closed as soon as the view exists; the view is unmapped by close() or the
// destructor. Movable, not copyable. An empty file maps to size() == 0 and a non-null data() of zero length.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

#include "vp/result.h"

namespace vp {

class MappedFile {
 public:
  MappedFile() = default;
  ~MappedFile() { close(); }
  MappedFile(MappedFile&& o) noexcept;
  MappedFile& operator=(MappedFile&& o) noexcept;
  MappedFile(const MappedFile&) = delete;
  MappedFile& operator=(const MappedFile&) = delete;

  // io (with a hint) when the file is missing, unreadable or cannot be mapped.
  static Result<MappedFile> open(const std::string& path);

  const uint8_t* data() const { return data_; }
  size_t size() const { return size_; }
  bool isOpen() const { return data_ != nullptr; }
  const std::string& path() const { return path_; }
  void close();

 private:
  const uint8_t* data_ = nullptr;
  size_t size_ = 0;
  bool mapped_ = false;   // false for the empty-file case (data_ points at a static byte)
  std::string path_;
};

}  // namespace vp
