// Feature-string builders, mirrored line by line from tools/train/features.py. [internal to engine/nlp]
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace vp::nlp::detail {

// A reusable list of feature strings: strings keep their capacity, so steady-state building does not allocate.
struct FeatBuf {
  std::vector<std::string> v;
  size_t n = 0;
  void reset() { n = 0; }
  std::string& next() {
    if (n == v.size()) v.emplace_back();
    std::string& s = v[n++];
    s.clear();
    return s;
  }
  // Appends a string made of the given parts.
  template <class... P> void add(const P&... parts) {
    std::string& s = next();
    (s.append(parts), ...);
  }
};

extern const char* const kBos;
extern const char* const kBos2;
extern const char* const kEos;
extern const char* const kEos2;
extern const char* const kNone;
extern const char* const kRoot;

// Last / first k code points (Python s[-k:] / s[:k]).
std::string_view suffix(std::string_view s, size_t k);
std::string_view prefix(std::string_view s, size_t k);
void shape(std::string_view w, std::string& out);

// Tagger (features.tag_features / tag_feat_extra). words, lows, shapes have the sentence length.
void tagFeatures(const std::vector<std::string_view>& words, const std::vector<std::string_view>& lows,
                 const std::vector<std::string>& shapes, size_t i, std::string_view t1, std::string_view t2,
                 FeatBuf& out);
void tagFeatExtra(std::string_view upos, std::string_view lw, FeatBuf& out);

// Parser configuration (features.parse_features). Index 0 is the root; words are 1..n.
struct ParseState {
  int n = 0;
  std::vector<int> stack;
  int b = 1;
  std::vector<int> heads, lc1, lc2, rc1, rc2, nl, nr;
  std::vector<std::string_view> labels;   // "" = none
  void reset(int words);
  void arc(int h, int d, std::string_view label);
};
void parseFeatures(const std::vector<std::string_view>& lows, const std::vector<std::string_view>& tags,
                   const ParseState& st, FeatBuf& out);
void labelFeatures(const std::vector<std::string_view>& lows, const std::vector<std::string_view>& tags, int h,
                   int d, std::string_view dir, const ParseState& st, FeatBuf& out);
std::string_view distBucket(int d);

}  // namespace vp::nlp::detail
