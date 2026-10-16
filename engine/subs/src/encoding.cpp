// Encoding detection and conversion: UTF-8 (BOM or not), UTF-16 LE/BE with BOM, Windows-1252 fallback.
#include <array>

#include "internal.h"

namespace vp::subs::detail {
namespace {

// Windows-1252 0x80..0x9F. The five undefined bytes map to the C1 controls of the same value, so every byte
// sequence decodes and re-encodes to itself.
constexpr std::array<uint16_t, 32> kCp1252High = {
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D,
    0x017D, 0x008F, 0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A,
    0x0153, 0x009D, 0x017E, 0x0178};

void appendUtf16(std::vector<uint8_t>& out, uint16_t unit, bool be) {
  uint8_t hi = static_cast<uint8_t>(unit >> 8), lo = static_cast<uint8_t>(unit & 0xFF);
  if (be) { out.push_back(hi); out.push_back(lo); } else { out.push_back(lo); out.push_back(hi); }
}

}  // namespace

bool validUtf8(const uint8_t* p, std::size_t n) {
  std::size_t i = 0;
  while (i < n) {
    uint8_t c = p[i];
    if (c < 0x80) { ++i; continue; }
    std::size_t len;
    uint32_t cp;
    if (c >= 0xC2 && c <= 0xDF) { len = 2; cp = c & 0x1F; }
    else if (c >= 0xE0 && c <= 0xEF) { len = 3; cp = c & 0x0F; }
    else if (c >= 0xF0 && c <= 0xF4) { len = 4; cp = c & 0x07; }
    else return false;
    if (i + len > n) return false;
    for (std::size_t k = 1; k < len; ++k) {
      if ((p[i + k] & 0xC0) != 0x80) return false;
      cp = (cp << 6) | (p[i + k] & 0x3F);
    }
    if ((len == 3 && cp < 0x800) || (len == 4 && (cp < 0x10000 || cp > 0x10FFFF)) || (cp >= 0xD800 && cp <= 0xDFFF))
      return false;
    i += len;
  }
  return true;
}

std::size_t nextCodePoint(std::string_view s, std::size_t i, uint32_t& cp) {
  const auto* p = reinterpret_cast<const uint8_t*>(s.data());
  std::size_t n = s.size();
  uint8_t c = p[i];
  if (c < 0x80) { cp = c; return 1; }
  std::size_t len = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 0;
  if (len == 0 || i + len > n || !validUtf8(p + i, len)) { cp = 0xFFFD; return 1; }
  cp = len == 2 ? (c & 0x1Fu) : len == 3 ? (c & 0x0Fu) : (c & 0x07u);
  for (std::size_t k = 1; k < len; ++k) cp = (cp << 6) | (p[i + k] & 0x3Fu);
  return len;
}

void appendUtf8(std::string& out, uint32_t cp) {
  if (cp < 0x80) {
    out.push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

bool isCombining(uint32_t cp) {
  return (cp >= 0x0300 && cp <= 0x036F) || (cp >= 0x1AB0 && cp <= 0x1AFF) || (cp >= 0x1DC0 && cp <= 0x1DFF) ||
         (cp >= 0x20D0 && cp <= 0x20FF) || (cp >= 0xFE20 && cp <= 0xFE2F) || (cp >= 0x200B && cp <= 0x200D) ||
         cp == 0xFEFF;
}

Decoded decode(const std::vector<uint8_t>& b) {
  Decoded d;
  const std::size_t n = b.size();
  if (n >= 2 && ((b[0] == 0xFF && b[1] == 0xFE) || (b[0] == 0xFE && b[1] == 0xFF))) {
    const bool be = b[0] == 0xFE;
    d.encoding = be ? "utf-16be" : "utf-16le";
    d.bom = true;
    d.text.reserve(n);
    std::size_t i = 2;
    auto unitAt = [&](std::size_t k) -> uint32_t {
      return be ? (static_cast<uint32_t>(b[k]) << 8 | b[k + 1]) : (static_cast<uint32_t>(b[k + 1]) << 8 | b[k]);
    };
    std::size_t bad = 0;
    for (; i + 1 < n; i += 2) {
      uint32_t u = unitAt(i);
      if (u >= 0xD800 && u <= 0xDBFF && i + 3 < n) {
        uint32_t v = unitAt(i + 2);
        if (v >= 0xDC00 && v <= 0xDFFF) {
          appendUtf8(d.text, 0x10000 + ((u - 0xD800) << 10) + (v - 0xDC00));
          i += 2;
          continue;
        }
      }
      if (u >= 0xD800 && u <= 0xDFFF) { ++bad; u = 0xFFFD; }
      appendUtf8(d.text, u);
    }
    if (bad) { d.lossy = true; d.lossDetail = std::to_string(bad) + " unpaired UTF-16 surrogate(s) replaced"; }
    if (i < n) {
      d.lossy = true;
      if (!d.lossDetail.empty()) d.lossDetail += "; ";
      d.lossDetail += "odd trailing byte dropped";
    }
    return d;
  }
  if (n >= 3 && b[0] == 0xEF && b[1] == 0xBB && b[2] == 0xBF && validUtf8(b.data() + 3, n - 3)) {
    d.encoding = "utf-8";
    d.bom = true;
    d.text.assign(reinterpret_cast<const char*>(b.data()) + 3, n - 3);
    return d;
  }
  if (validUtf8(b.data(), n)) {
    d.encoding = "utf-8";
    d.text.assign(reinterpret_cast<const char*>(b.data()), n);
    return d;
  }
  d.encoding = "windows-1252";
  d.text.reserve(n + n / 4);
  for (uint8_t c : b) appendUtf8(d.text, c >= 0x80 && c <= 0x9F ? kCp1252High[c - 0x80] : c);
  return d;
}

std::string canonicalEncoding(const std::string& name) {
  std::string k;
  for (char c : name) {
    if (c == '-' || c == '_' || c == ' ') continue;
    k.push_back(static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c));
  }
  if (k == "utf8") return "utf-8";
  if (k == "utf16le" || k == "utf16") return "utf-16le";
  if (k == "utf16be") return "utf-16be";
  if (k == "windows1252" || k == "cp1252" || k == "latin1" || k == "iso88591") return "windows-1252";
  return {};
}

std::size_t encode(const std::string& s, const std::string& enc, bool bom, std::vector<uint8_t>& out) {
  std::size_t replaced = 0;
  if (enc == "utf-8") {
    out.reserve(out.size() + s.size() + 3);
    if (bom) out.insert(out.end(), {0xEF, 0xBB, 0xBF});
    out.insert(out.end(), s.begin(), s.end());
    return 0;
  }
  if (enc == "utf-16le" || enc == "utf-16be") {
    const bool be = enc == "utf-16be";
    out.reserve(out.size() + 2 * s.size() + 2);
    if (bom) appendUtf16(out, 0xFEFF, be);
    for (std::size_t i = 0; i < s.size();) {
      uint32_t cp;
      i += nextCodePoint(s, i, cp);
      if (cp >= 0x10000) {
        cp -= 0x10000;
        appendUtf16(out, static_cast<uint16_t>(0xD800 + (cp >> 10)), be);
        appendUtf16(out, static_cast<uint16_t>(0xDC00 + (cp & 0x3FF)), be);
      } else {
        appendUtf16(out, static_cast<uint16_t>(cp), be);
      }
    }
    return 0;
  }
  // windows-1252 (a BOM is meaningless here and never written)
  out.reserve(out.size() + s.size());
  for (std::size_t i = 0; i < s.size();) {
    uint32_t cp;
    i += nextCodePoint(s, i, cp);
    if (cp < 0x80 || (cp >= 0xA0 && cp <= 0xFF)) { out.push_back(static_cast<uint8_t>(cp)); continue; }
    int found = -1;
    for (int k = 0; k < 32; ++k)
      if (kCp1252High[static_cast<std::size_t>(k)] == cp) { found = k; break; }
    if (found >= 0) {
      out.push_back(static_cast<uint8_t>(0x80 + found));
    } else {
      out.push_back('?');
      ++replaced;
    }
  }
  return replaced;
}

}  // namespace vp::subs::detail
