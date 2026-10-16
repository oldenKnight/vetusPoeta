// SHA-256 (FIPS 180-4), streaming and one-shot. Used for the project's source hash and the model file check.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace vp {

class Sha256 {
 public:
  Sha256() { reset(); }
  void reset();
  void update(const void* data, size_t size);
  void update(std::string_view s) { update(s.data(), s.size()); }
  // Finishes the hash (the object must be reset() before reuse) and returns 64 lower-case hex digits.
  std::string finishHex();
  void finish(uint8_t out[32]);

 private:
  void block(const uint8_t* p);
  uint32_t h_[8];
  uint8_t buf_[64];
  size_t bufLen_ = 0;
  uint64_t total_ = 0;
};

std::string sha256Hex(std::string_view data);

}  // namespace vp
