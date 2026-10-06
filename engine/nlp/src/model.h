// One memory-mapped, validated .vpt model (DESIGN.md section 17; byte layout in tools/train/vpt.py).
#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "vp/mmap.h"
#include "vp/result.h"

namespace vp::nlp {

struct Model {
  vp::MappedFile file;
  std::string_view lang, kind, note;
  uint32_t nLabels = 0, nFeats = 0, tableSize = 0;
  const uint8_t* keys = nullptr;      // tableSize x u64
  const uint8_t* weights = nullptr;   // tableSize x nLabels x int16
  std::vector<std::string_view> labels;

  // Opens and validates; expectKind is "tag" or "dep".
  static Result<Model> open(const std::string& path, std::string_view expectKind);

  // Slot of a feature hash, or -1 when absent (linear probing, at most tableSize probes).
  int64_t find(uint64_t h) const {
    const uint64_t mask = tableSize - 1;
    uint64_t i = h & mask;
    for (uint32_t probes = 0; probes < tableSize; ++probes) {
      uint64_t k;
      std::memcpy(&k, keys + i * 8, 8);
      if (k == h) return static_cast<int64_t>(i);
      if (k == 0) return -1;
      i = (i + 1) & mask;
    }
    return -1;
  }
  // scores[k] += weight(slot, k) for every label.
  void addRow(int64_t slot, int32_t* scores) const {
    const uint8_t* p = weights + static_cast<size_t>(slot) * nLabels * 2;
    for (uint32_t k = 0; k < nLabels; ++k) {
      int16_t v;
      std::memcpy(&v, p + 2 * k, 2);
      scores[k] += v;
    }
  }
  void add(uint64_t h, int32_t* scores) const {
    const int64_t s = find(h);
    if (s >= 0) addRow(s, scores);
  }
  int label(std::string_view name) const {
    for (size_t i = 0; i < labels.size(); ++i)
      if (labels[i] == name) return static_cast<int>(i);
    return -1;
  }
};

}  // namespace vp::nlp
