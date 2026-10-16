// Private helpers of vp_subs (not a public header).
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "vp/subs.h"

namespace vp::subs::detail {

struct Decoded {
  std::string text;          // UTF-8
  std::string encoding;      // canonical name
  bool bom = false;
  bool lossy = false;        // invalid UTF-16 was repaired: round trip is not exact
  std::string lossDetail;
};

Decoded decode(const std::vector<uint8_t>& bytes);
std::string canonicalEncoding(const std::string& name);  // "" when unknown
// Appends the encoded bytes; returns the number of code points that had to be replaced.
std::size_t encode(const std::string& utf8, const std::string& canonical, bool bom, std::vector<uint8_t>& out);

bool validUtf8(const uint8_t* p, std::size_t n);
// Decodes one code point at s[i]; returns bytes consumed (>= 1). Invalid sequences yield U+FFFD and length 1.
std::size_t nextCodePoint(std::string_view s, std::size_t i, uint32_t& cp);
void appendUtf8(std::string& out, uint32_t cp);
bool isCombining(uint32_t cp);

bool parseTimestamp(std::string_view s, int64_t& ms);
bool isTimingLine(std::string_view line);
std::string_view trim(std::string_view s);

std::string renderSpans(const std::vector<Span>& spans, const std::string& defaultBreak);

}  // namespace vp::subs::detail
