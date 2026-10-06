#include "model.h"

#include "vp/sha256.h"

namespace vp::nlp {
namespace {

constexpr size_t kHeader = 256;
constexpr uint32_t kMaxLabels = 4096;

template <class T> T rd(const uint8_t* p) {
  T v;
  std::memcpy(&v, p, sizeof(T));
  return v;
}

Error damaged(const std::string& path, const std::string& why) {
  return Error{ErrorCode::Internal, "model " + path + ": " + why, "model file damaged"};
}

bool hostLittleEndian() {
  const uint16_t x = 1;
  uint8_t b;
  std::memcpy(&b, &x, 1);
  return b == 1;
}

// A NUL-padded char[8] field of printable ASCII.
bool field8(const uint8_t* p, std::string_view& out) {
  size_t len = 0;
  while (len < 8 && p[len] != 0) {
    if (p[len] < 0x21 || p[len] > 0x7E) return false;
    ++len;
  }
  for (size_t k = len; k < 8; ++k)
    if (p[k] != 0) return false;
  out = std::string_view(reinterpret_cast<const char*>(p), len);
  return len > 0;
}

}  // namespace

Result<Model> Model::open(const std::string& path, std::string_view expectKind) {
  if (!hostLittleEndian()) return Error{ErrorCode::Internal, "big-endian host", "unsupported platform"};
  auto mf = MappedFile::open(path);
  if (!mf.ok()) return mf.error();
  Model m;
  m.file = std::move(mf.value());
  const uint8_t* d = m.file.data();
  const size_t size = m.file.size();
  if (size < kHeader) return damaged(path, "shorter than the header");
  if (std::memcmp(d, "VPTX", 4) != 0) return damaged(path, "bad magic");
  if (rd<uint16_t>(d + 4) != 1) return damaged(path, "unsupported major version");
  if (!field8(d + 8, m.lang) || !field8(d + 16, m.kind)) return damaged(path, "bad lang/kind field");
  if (m.kind != expectKind) return damaged(path, "model kind is not " + std::string(expectKind));
  m.nLabels = rd<uint32_t>(d + 24);
  m.nFeats = rd<uint32_t>(d + 28);
  m.tableSize = rd<uint32_t>(d + 32);
  const uint32_t reserved = rd<uint32_t>(d + 36);
  const uint64_t fileSize = rd<uint64_t>(d + 40);
  if (reserved != 0) return damaged(path, "reserved field not zero");
  for (size_t k = 80; k < kHeader; ++k)
    if (d[k] != 0) return damaged(path, "header padding not zero");
  if (fileSize != size) return damaged(path, "size mismatch (truncated or extended)");
  if (m.nLabels == 0 || m.nLabels > kMaxLabels) return damaged(path, "bad label count");
  if (m.tableSize == 0 || (m.tableSize & (m.tableSize - 1)) != 0 || m.tableSize > (1u << 26))
    return damaged(path, "table size is not a power of two");
  if (m.nFeats >= m.tableSize) return damaged(path, "feature count exceeds the table");
  // labels
  size_t pos = kHeader;
  m.labels.reserve(m.nLabels);
  for (uint32_t i = 0; i < m.nLabels; ++i) {
    const void* z = pos < size ? std::memchr(d + pos, 0, size - pos) : nullptr;
    if (z == nullptr) return damaged(path, "label table runs past the end");
    const size_t e = static_cast<size_t>(static_cast<const uint8_t*>(z) - d);
    if (e == pos) return damaged(path, "empty label");
    m.labels.emplace_back(reinterpret_cast<const char*>(d + pos), e - pos);
    pos = e + 1;
  }
  pos = (pos + 7) & ~static_cast<size_t>(7);
  const uint64_t keyBytes = static_cast<uint64_t>(m.tableSize) * 8;
  const uint64_t wBytes = static_cast<uint64_t>(m.tableSize) * m.nLabels * 2;
  const uint64_t wPadded = (wBytes + 7) & ~static_cast<uint64_t>(7);
  if (pos > size || keyBytes + wPadded > size - pos) return damaged(path, "tables run past the end");
  m.keys = d + pos;
  m.weights = d + pos + keyBytes;
  const size_t notePos = pos + static_cast<size_t>(keyBytes + wPadded);
  m.note = std::string_view(reinterpret_cast<const char*>(d + notePos), size - notePos);
  // checksum of the body
  Sha256 sha;
  sha.update(d + kHeader, size - kHeader);
  uint8_t dig[32];
  sha.finish(dig);
  if (std::memcmp(dig, d + 48, 32) != 0) return damaged(path, "SHA-256 mismatch");
  // occupied slots must match n_feats
  uint32_t used = 0;
  for (uint32_t i = 0; i < m.tableSize; ++i)
    if (rd<uint64_t>(m.keys + static_cast<size_t>(i) * 8) != 0) ++used;
  if (used != m.nFeats) return damaged(path, "feature count does not match the table");
  return m;
}

}  // namespace vp::nlp
