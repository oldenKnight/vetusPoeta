// Timestamps, timing lines and reading speed.
#include "internal.h"

namespace vp::subs {
namespace detail {

std::string_view trim(std::string_view s) {
  std::size_t b = 0, e = s.size();
  while (b < e && (s[b] == ' ' || s[b] == '\t')) ++b;
  while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t')) --e;
  return s.substr(b, e - b);
}

// [h:]mm:ss[(,|.)fff] with any number of hour digits; the fraction is read as a decimal fraction of a second
// (".5" = 500 ms, ".25" = 250 ms as in ASS centiseconds, ",250" = 250 ms). Extra fraction digits are ignored.
bool parseTimestamp(std::string_view s, int64_t& ms) {
  s = trim(s);
  int64_t parts[3] = {0, 0, 0};
  int count = 0;
  std::size_t i = 0;
  while (true) {
    if (count == 3) return false;
    std::size_t start = i;
    int64_t v = 0;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
      if (i - start >= 9) return false;
      v = v * 10 + (s[i] - '0');
      ++i;
    }
    if (i == start) return false;
    parts[count++] = v;
    if (i < s.size() && s[i] == ':') { ++i; continue; }
    break;
  }
  if (count < 2) return false;
  int64_t frac = 0;
  if (i < s.size() && (s[i] == ',' || s[i] == '.')) {
    ++i;
    std::size_t start = i;
    int64_t scale = 100;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
      frac += (s[i] - '0') * scale;
      scale /= 10;
      ++i;
    }
    if (i == start) return false;
  }
  if (i != s.size()) return false;
  int64_t h = count == 3 ? parts[0] : 0, m = parts[count - 2], sec = parts[count - 1];
  if (m >= 60 && count == 3) return false;
  if (sec >= 60) return false;
  ms = ((h * 60 + m) * 60 + sec) * 1000 + frac;
  return true;
}

bool isTimingLine(std::string_view line) {
  std::size_t p = line.find("-->");
  if (p == std::string_view::npos) return false;
  int64_t ms;
  return parseTimestamp(line.substr(0, p), ms);
}

}  // namespace detail

bool parseTiming(const std::string& timingRaw, Format format, int64_t& startMs, int64_t& endMs) {
  std::string_view t = timingRaw;
  if (format == Format::Txt) return false;
  if (format == Format::Ass) {
    std::size_t c = t.find(',');
    if (c == std::string_view::npos) return false;
    return detail::parseTimestamp(t.substr(0, c), startMs) && detail::parseTimestamp(t.substr(c + 1), endMs);
  }
  std::size_t p = t.find("-->");
  if (p == std::string_view::npos) return false;
  std::string_view right = t.substr(p + 3);
  std::size_t b = 0;
  while (b < right.size() && (right[b] == ' ' || right[b] == '\t')) ++b;
  std::size_t e = b;
  while (e < right.size() && right[e] != ' ' && right[e] != '\t') ++e;  // VTT cue settings follow
  return detail::parseTimestamp(t.substr(0, p), startMs) && detail::parseTimestamp(right.substr(b, e - b), endMs);
}

double charsPerSecond(const Cue& cue) {
  int64_t s = 0, e = 0;
  Format f = cue.timingRaw.find("-->") != std::string::npos ? Format::Srt : Format::Ass;
  if (!parseTiming(cue.timingRaw, f, s, e) || e <= s) return 0.0;
  return static_cast<double>(visibleLength(cue.plainText())) * 1000.0 / static_cast<double>(e - s);
}

}  // namespace vp::subs
