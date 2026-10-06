// Spans, plain text, visible width and line breaking.
#include <algorithm>
#include <limits>

#include "internal.h"

namespace vp::subs {
namespace detail {

std::string renderSpans(const std::vector<Span>& spans, const std::string& defaultBreak) {
  std::size_t n = 0;
  for (const Span& s : spans) n += s.raw.empty() ? defaultBreak.size() : s.raw.size();
  std::string out;
  out.reserve(n);
  for (const Span& s : spans) out += (s.kind == Span::Newline && s.raw.empty()) ? defaultBreak : s.raw;
  return out;
}

}  // namespace detail

namespace {

bool isAsciiAlnum(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

// Length of an HTML-like tag starting at s[i] == '<' (0 if it is not a tag). VTT timestamps (<00:01.000>) count.
std::size_t angleTagLen(std::string_view s, std::size_t i) {
  if (i + 1 >= s.size()) return 0;
  char c = s[i + 1];
  if (!(isAsciiAlnum(c) || c == '/')) return 0;
  for (std::size_t j = i + 1; j < s.size(); ++j) {
    if (s[j] == '>') return j - i + 1;
    if (s[j] == '<' || s[j] == '\n' || s[j] == '\r') return 0;
  }
  return 0;
}

// Length of a brace block starting at s[i] == '{' (0 if unterminated). requireBackslash: SRT/VTT accept only {\...}.
std::size_t braceTagLen(std::string_view s, std::size_t i, bool requireBackslash) {
  if (requireBackslash && (i + 1 >= s.size() || s[i + 1] != '\\')) return 0;
  for (std::size_t j = i + 1; j < s.size(); ++j) {
    if (s[j] == '}') return j - i + 1;
    if (s[j] == '{' || s[j] == '\n' || s[j] == '\r') return 0;
  }
  return 0;
}

void pushSpan(std::vector<Span>& out, Span::Kind k, std::string_view raw) {
  if (k == Span::Text && !out.empty() && out.back().kind == Span::Text) {
    out.back().raw.append(raw.data(), raw.size());
    return;
  }
  out.push_back(Span{k, std::string(raw)});
}

}  // namespace

std::vector<Span> splitSpans(const std::string& text, Format format) {
  std::vector<Span> out;
  std::string_view s = text;
  std::size_t i = 0, textStart = 0;
  auto flush = [&](std::size_t end) {
    if (end > textStart) pushSpan(out, Span::Text, s.substr(textStart, end - textStart));
  };
  while (i < s.size()) {
    char c = s[i];
    std::size_t tag = 0, nl = 0;
    bool nlVerbatim = false;
    if (c == '\n') nl = 1;
    else if (c == '\r' && i + 1 < s.size() && s[i + 1] == '\n') nl = 2;
    else if (format == Format::Ass && c == '\\' && i + 1 < s.size() && (s[i + 1] == 'N' || s[i + 1] == 'n')) {
      nl = 2;
      nlVerbatim = true;
    } else if (format != Format::Txt && c == '{') tag = braceTagLen(s, i, format != Format::Ass);
    else if ((format == Format::Srt || format == Format::Vtt) && c == '<') tag = angleTagLen(s, i);
    if (tag) {
      flush(i);
      pushSpan(out, Span::Tag, s.substr(i, tag));
      i += tag;
      textStart = i;
    } else if (nl) {
      flush(i);
      out.push_back(Span{Span::Newline, nlVerbatim ? std::string(s.substr(i, nl)) : std::string()});
      i += nl;
      textStart = i;
    } else {
      ++i;
    }
  }
  flush(s.size());
  return out;
}

std::size_t visibleLength(const std::string& utf8) {
  std::size_t n = 0;
  for (std::size_t i = 0; i < utf8.size();) {
    uint32_t cp;
    i += detail::nextCodePoint(utf8, i, cp);
    if (!detail::isCombining(cp)) ++n;
  }
  return n;
}

std::string Cue::plainText() const {
  std::string joined;
  for (const Span& s : spans) {
    if (s.kind == Span::Text) joined += s.raw;
    else if (s.kind == Span::Newline) joined.push_back(' ');
  }
  std::string out;
  out.reserve(joined.size());
  bool pendingSpace = false;
  auto put = [&](char c) {
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v') {
      pendingSpace = !out.empty();
      return;
    }
    if (pendingSpace) out.push_back(' ');
    pendingSpace = false;
    out.push_back(c);
  };
  static const struct { const char* name; char ch; } kEntities[] = {
      {"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&nbsp;", ' '}};
  for (std::size_t i = 0; i < joined.size(); ++i) {
    if (joined[i] == '&') {
      bool matched = false;
      for (const auto& e : kEntities) {
        std::size_t len = std::char_traits<char>::length(e.name);
        if (joined.compare(i, len, e.name) == 0) {
          put(e.ch);
          i += len - 1;
          matched = true;
          break;
        }
      }
      if (matched) continue;
    }
    put(joined[i]);
  }
  return out;
}

// ---------------------------------------------------------------------------------------------------------------
// Line breaking
namespace {

struct Tok {
  std::string s;    // bytes as written (tags included)
  std::string vis;  // visible text (tags removed)
  int w = 0;        // visible width
  bool dash = false, sentEnd = false, clauseEnd = false, hint = false;
};

bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }

bool isDashCp(uint32_t cp) { return cp == '-' || cp == 0x2010 || cp == 0x2013 || cp == 0x2014; }

std::vector<Tok> rawTokens(std::string_view s) {
  std::vector<Tok> toks;
  Tok cur;
  bool have = false;
  for (std::size_t i = 0; i < s.size();) {
    char c = s[i];
    if (isSpace(c)) {
      if (have) toks.push_back(std::move(cur));
      cur = Tok{};
      have = false;
      ++i;
      continue;
    }
    std::size_t tag = c == '<' ? angleTagLen(s, i) : c == '{' ? braceTagLen(s, i, false) : 0;
    if (tag) {
      cur.s.append(s.substr(i, tag));
      have = true;
      i += tag;
      continue;
    }
    uint32_t cp;
    std::size_t len = detail::nextCodePoint(s, i, cp);
    cur.s.append(s.substr(i, len));
    cur.vis.append(s.substr(i, len));
    if (!detail::isCombining(cp)) ++cur.w;
    have = true;
    i += len;
  }
  if (have) toks.push_back(std::move(cur));
  return toks;
}

// Last visible code point, skipping closing quotes and brackets.
uint32_t lastSignificant(const std::string& vis) {
  uint32_t last = 0;
  for (std::size_t i = 0; i < vis.size();) {
    uint32_t cp;
    i += detail::nextCodePoint(vis, i, cp);
    if (cp == '"' || cp == '\'' || cp == ')' || cp == ']' || cp == 0x201D || cp == 0x2019 || cp == 0x00BB) continue;
    last = cp;
  }
  return last;
}

uint32_t firstCp(const std::string& vis) {
  if (vis.empty()) return 0;
  uint32_t cp;
  detail::nextCodePoint(vis, 0, cp);
  return cp;
}

std::string hintKey(const std::string& vis) {
  std::size_t b = 0;
  while (b < vis.size()) {
    uint32_t cp;
    std::size_t len = detail::nextCodePoint(vis, b, cp);
    if (cp == '"' || cp == '\'' || cp == '(' || cp == '[' || cp == 0x201C || cp == 0x2018 || cp == 0x00AB ||
        cp == 0x00BF || cp == 0x00A1)
      b += len;
    else
      break;
  }
  std::size_t e = vis.size();
  while (e > b && (vis[e - 1] == ',' || vis[e - 1] == '.' || vis[e - 1] == ';' || vis[e - 1] == ':')) --e;
  std::string k = vis.substr(b, e - b);
  for (char& c : k)
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  return k;
}

std::vector<Tok> tokenize(const std::string& text, const BreakHints& hints) {
  std::vector<Tok> raw = rawTokens(text);
  // 1. Tag-only tokens stick to a neighbour: closing tags to the previous word, others to the next word.
  std::vector<Tok> t1;
  std::string pending;
  for (Tok& t : raw) {
    if (t.vis.empty()) {
      bool closing = t.s.compare(0, 2, "</") == 0;
      if (closing && !t1.empty() && pending.empty()) {
        t1.back().s += ' ';
        t1.back().s += t.s;
      } else {
        pending += t.s;
        pending += ' ';
      }
      continue;
    }
    if (!pending.empty()) {
      t.s = pending + t.s;
      pending.clear();
    }
    t1.push_back(std::move(t));
  }
  if (!pending.empty()) {
    pending.pop_back();
    if (t1.empty()) {
      Tok t;
      t.s = pending;
      t1.push_back(std::move(t));
    } else {
      t1.back().s += ' ';
      t1.back().s += pending;
    }
  }
  // 2. A standalone dialogue dash joins the next word; mark dialogue turns.
  std::vector<Tok> out;
  for (std::size_t i = 0; i < t1.size(); ++i) {
    Tok t = std::move(t1[i]);
    bool standalone = t.w == 1 && isDashCp(firstCp(t.vis));
    if (standalone && i + 1 < t1.size()) {
      Tok& n = t1[i + 1];
      n.s = t.s + ' ' + n.s;
      n.vis = t.vis + ' ' + n.vis;
      n.w += t.w + 1;
      n.dash = true;
      continue;
    }
    if (!t.dash && isDashCp(firstCp(t.vis)) && t.w > 1 && (out.empty() || out.back().sentEnd)) t.dash = true;
    uint32_t last = lastSignificant(t.vis);
    t.sentEnd = last == '.' || last == '!' || last == '?' || last == 0x2026 || last == 0x037E;
    t.clauseEnd = last == ',' || last == ';' || last == ':' || last == 0x0387 || last == 0x00B7;
    std::string key = hintKey(t.vis);
    for (const std::string& h : hints.breakBefore) {
      std::string hk = h;
      for (char& c : hk)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
      if (!key.empty() && key == hk) { t.hint = true; break; }
    }
    out.push_back(std::move(t));
  }
  return out;
}

}  // namespace

std::vector<std::string> breakLines(const std::string& text, const BreakHints& hints, int maxLine, int maxLines,
                                    bool* overflow) {
  if (overflow) *overflow = false;
  maxLine = std::max(1, maxLine);
  maxLines = std::max(1, maxLines);
  std::vector<Tok> toks = tokenize(text, hints);
  const int n = static_cast<int>(toks.size());
  if (n == 0) return {};

  std::vector<long long> pre(static_cast<std::size_t>(n) + 1, 0);
  bool dialogue = false;
  for (int i = 0; i < n; ++i) {
    pre[static_cast<std::size_t>(i) + 1] = pre[static_cast<std::size_t>(i)] + toks[static_cast<std::size_t>(i)].w;
    if (i > 0 && toks[static_cast<std::size_t>(i)].dash) dialogue = true;
  }
  auto width = [&](int i, int j) {  // tokens [i, j)
    return pre[static_cast<std::size_t>(j)] - pre[static_cast<std::size_t>(i)] + (j - i - 1);
  };
  auto lineCost = [&](int i, int j) {
    double w = static_cast<double>(width(i, j));
    double over = w - maxLine;
    return w * w + (over > 0 ? 1e7 * over * over : 0.0);  // quadratic: spread unavoidable overflow evenly
  };
  auto breakBonus = [&](int j) {  // break before token j
    const Tok& prev = toks[static_cast<std::size_t>(j) - 1];
    const Tok& next = toks[static_cast<std::size_t>(j)];
    double b = 0;
    if (prev.sentEnd) b += 120;
    else if (prev.clauseEnd) b += 80;
    if (next.hint) b += 60;
    if (!prev.sentEnd && !prev.clauseEnd) {
      if (prev.hint) b -= 60;    // a line should not end on "and", "to", ...
      if (prev.w <= 3) b -= 80;  // nor on a short word (articles, particles) cut off from what follows
    }
    if (next.dash) b += 1e6;
    return b;
  };

  const double kInf = std::numeric_limits<double>::infinity();
  std::vector<int> bestBreaks;
  bool bestOverflow = true;
  int minL = (dialogue && maxLines >= 2) ? 2 : 1;
  int maxL = std::min(maxLines, n);
  // Lower bound on the line count: L lines hold at most L * maxLine visible characters plus L - 1 dropped spaces.
  long long lower = (pre[static_cast<std::size_t>(n)] + n + maxLine) / (static_cast<long long>(maxLine) + 1);
  minL = static_cast<int>(std::min<long long>(std::max<long long>(minL, lower), maxL));
  for (int L = minL; L <= maxL; ++L) {
    // dp[l][j]: best cost for tokens [0, j) in l lines.
    std::vector<std::vector<double>> dp(static_cast<std::size_t>(L) + 1,
                                        std::vector<double>(static_cast<std::size_t>(n) + 1, kInf));
    std::vector<std::vector<int>> from(static_cast<std::size_t>(L) + 1,
                                       std::vector<int>(static_cast<std::size_t>(n) + 1, -1));
    dp[0][0] = 0;
    for (int l = 1; l <= L; ++l) {
      for (int j = l; j <= n; ++j) {
        // Walk line starts backwards; lines far beyond maxLine are not explored once a candidate exists (keeps it linear).
        for (int i = j - 1; i >= l - 1; --i) {
          if (width(i, j) > 4LL * maxLine && dp[static_cast<std::size_t>(l)][static_cast<std::size_t>(j)] < kInf) break;
          double prev = dp[static_cast<std::size_t>(l) - 1][static_cast<std::size_t>(i)];
          if (prev == kInf) continue;
          double c = prev + lineCost(i, j) - (i > 0 ? breakBonus(i) : 0.0);
          if (c < dp[static_cast<std::size_t>(l)][static_cast<std::size_t>(j)]) {
            dp[static_cast<std::size_t>(l)][static_cast<std::size_t>(j)] = c;
            from[static_cast<std::size_t>(l)][static_cast<std::size_t>(j)] = i;
          }
        }
      }
    }
    std::vector<int> breaks;  // start index of each line
    for (int l = L, j = n; l > 0; --l) {
      int i = from[static_cast<std::size_t>(l)][static_cast<std::size_t>(j)];
      breaks.push_back(i);
      j = i;
    }
    std::reverse(breaks.begin(), breaks.end());
    bool over = false;
    for (int l = 0; l < L; ++l) {
      int i = breaks[static_cast<std::size_t>(l)], j = l + 1 < L ? breaks[static_cast<std::size_t>(l) + 1] : n;
      if (width(i, j) > maxLine) over = true;
    }
    if (!over || L == maxL) {
      bestBreaks = breaks;
      bestOverflow = over;
      if (!over) break;
    }
  }
  if (overflow) *overflow = bestOverflow;
  std::vector<std::string> lines;
  for (std::size_t l = 0; l < bestBreaks.size(); ++l) {
    int i = bestBreaks[l], j = l + 1 < bestBreaks.size() ? bestBreaks[l + 1] : n;
    std::string line;
    for (int k = i; k < j; ++k) {
      if (k > i) line.push_back(' ');
      line += toks[static_cast<std::size_t>(k)].s;
    }
    lines.push_back(std::move(line));
  }
  return lines;
}

}  // namespace vp::subs
