// Parsing: decode, detect the format, split into cues while recording every byte needed to write the file back.
#include <exception>
#include <utility>

#include "internal.h"

namespace vp::subs {
namespace {

using detail::trim;

struct LineRef {
  std::size_t b, e, n;  // content [b, e), line break [e, n)
};

std::vector<LineRef> splitLines(const std::string& s) {
  std::vector<LineRef> lines;
  lines.reserve(s.size() / 24 + 1);
  std::size_t b = 0;
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\n') {
      lines.push_back({b, i, i + 1});
      b = i + 1;
    } else if (s[i] == '\r') {
      std::size_t n = (i + 1 < s.size() && s[i + 1] == '\n') ? i + 2 : i + 1;
      lines.push_back({b, i, n});
      b = n;
      i = n - 1;
    }
  }
  if (b < s.size()) lines.push_back({b, s.size(), s.size()});
  return lines;
}

class Parser {
 public:
  Parser(const std::string& s, Document& d) : s_(s), d_(d), L_(splitLines(s)) {}

  void run() {
    for (const LineRef& l : L_) {
      if (l.n > l.e) {
        d_.newline = s_.compare(l.e, 2, "\r\n") == 0 ? "\r\n" : "\n";
        break;
      }
    }
    switch (d_.format) {
      case Format::Srt:
      case Format::Vtt: parseTimed(); break;
      case Format::Ass: parseAss(); break;
      case Format::Txt: parseTxt(); break;
    }
  }

 private:
  std::string_view content(std::size_t i) const { return std::string_view(s_).substr(L_[i].b, L_[i].e - L_[i].b); }
  std::string eol(std::size_t i) const { return s_.substr(L_[i].e, L_[i].n - L_[i].e); }
  bool blank(std::size_t i) const { return trim(content(i)).empty(); }
  std::string range(std::size_t b, std::size_t e) const { return b < e ? s_.substr(b, e - b) : std::string(); }
  void warn(uint32_t index, const char* kind, std::string detail) {
    d_.warnings.push_back(Warning{index, kind, std::move(detail)});
  }

  // Text lines [from, to) become spans; Newline spans hold each inner line break verbatim.
  void setText(Cue& c, std::size_t from, std::size_t to, Format f) {
    for (std::size_t i = from; i < to; ++i) {
      std::vector<Span> line = splitSpans(std::string(content(i)), f);
      for (Span& sp : line) c.spans.push_back(std::move(sp));
      if (i + 1 < to) c.spans.push_back(Span{Span::Newline, eol(i)});
    }
    c.textEol = to > from ? eol(to - 1) : std::string();
    c.parsedText = detail::renderSpans(c.spans, std::string());
  }

  // ---- SRT / WebVTT -------------------------------------------------------------------------------------------
  void parseTimed() {
    const bool vtt = d_.format == Format::Vtt;
    const std::size_t nl = L_.size();
    std::vector<char> timing(nl, 0);
    std::vector<std::size_t> T;
    for (std::size_t i = 0; i < nl; ++i) {
      if (detail::isTimingLine(content(i))) {
        timing[i] = 1;
        T.push_back(i);
      }
    }
    if (vtt && (s_.compare(0, 6, "WEBVTT") != 0 || (s_.size() > 6 && s_[6] != ' ' && s_[6] != '\t' &&
                                                     s_[6] != '\n' && s_[6] != '\r')))
      warn(0, "missing_header", "file does not start with WEBVTT");
    if (T.empty()) {
      d_.headerRaw = s_;
      warn(0, "no_cues", "no timing line found");
      return;
    }
    auto digits = [](std::string_view v) {
      v = trim(v);
      if (v.empty()) return false;
      for (char c : v)
        if (c < '0' || c > '9') return false;
      return true;
    };
    // Where each cue starts: its identifier line when there is one, else its timing line.
    std::vector<std::size_t> start(T.size());
    std::vector<char> hasId(T.size(), 0);
    for (std::size_t k = 0; k < T.size(); ++k) {
      std::size_t minIdx = k == 0 ? (vtt ? 1 : 0) : T[k - 1] + 1;
      std::size_t t = T[k];
      start[k] = t;
      if (t == 0 || t - 1 < minIdx) continue;
      std::size_t j = t - 1;
      if (blank(j) || timing[j]) continue;
      if (digits(content(j)) || j == 0 || blank(j - 1)) {
        start[k] = j;
        hasId[k] = 1;
      }
    }
    d_.headerRaw = range(0, L_[start[0]].b);
    d_.cues.reserve(T.size());
    for (std::size_t k = 0; k < T.size(); ++k) {
      Cue c;
      c.index = static_cast<uint32_t>(k + 1);
      c.fromSource = true;
      if (hasId[k]) {
        c.idRaw = std::string(content(start[k]));
        c.idEol = eol(start[k]);
      }
      c.timingRaw = std::string(content(T[k]));
      c.timingEol = eol(T[k]);
      const bool last = k + 1 == T.size();
      const std::size_t limit = last ? nl : start[k + 1];
      std::size_t j = T[k] + 1;
      while (j < limit && !blank(j)) ++j;
      setText(c, T[k] + 1, j, d_.format);
      std::size_t tailBegin = j > T[k] + 1 ? L_[j - 1].n : L_[T[k]].n;
      std::size_t tailEnd = last ? s_.size() : L_[start[k + 1]].b;
      if (last) d_.trailerRaw = range(tailBegin, tailEnd);
      else c.tailRaw = range(tailBegin, tailEnd);
      if (!last && j == limit) warn(c.index, "missing_blank_line", "next cue starts without a blank line");
      for (std::size_t x = j; x < limit; ++x) {
        if (vtt && blank(x - 1) && (content(x).substr(0, 4) == "NOTE" || content(x).substr(0, 5) == "STYLE" ||
                                    content(x).substr(0, 6) == "REGION")) {
          while (x + 1 < limit && !blank(x + 1)) ++x;  // VTT blocks between cues are legitimate
          continue;
        }
        if (!blank(x)) {
          warn(c.index, "stray_text", "text after the cue's blank line is kept verbatim but not translated");
          break;
        }
      }
      d_.cues.push_back(std::move(c));
    }
    checkTimed();
  }

  static bool canonicalSrtTiming(std::string_view t) {
    static const char kPattern[] = "00:00:00,000 --> 00:00:00,000";
    if (t.size() != sizeof(kPattern) - 1) return false;
    for (std::size_t i = 0; i < t.size(); ++i) {
      if (kPattern[i] == '0' ? (t[i] < '0' || t[i] > '9') : t[i] != kPattern[i]) return false;
    }
    return true;
  }

  void checkTimed() {
    const bool srt = d_.format == Format::Srt;
    long long prevNum = -1;
    int64_t prevEnd = -1;
    for (const Cue& c : d_.cues) {
      if (srt) {
        std::string_view id = trim(c.idRaw);
        if (c.idRaw.empty()) {
          warn(c.index, "missing_id", "cue has no number line");
        } else {
          long long num = 0;
          bool ok = id.size() <= 12;
          for (char ch : id) {
            if (ch < '0' || ch > '9') ok = false;
            else num = num * 10 + (ch - '0');
          }
          if (!ok) {
            warn(c.index, "bad_id", "number line is not a number: " + c.idRaw);
          } else {
            if (id.size() != c.idRaw.size()) warn(c.index, "bad_id", "spaces around the number");
            if (prevNum >= 0 && num == prevNum) warn(c.index, "numbering_duplicate", "number " + std::to_string(num) + " repeats");
            else if (prevNum >= 0 && num < prevNum) warn(c.index, "numbering_order", "number " + std::to_string(num) + " after " + std::to_string(prevNum));
            else if (prevNum >= 0 && num > prevNum + 1) warn(c.index, "numbering_gap", "number " + std::to_string(num) + " after " + std::to_string(prevNum));
            prevNum = num;
          }
        }
        if (!canonicalSrtTiming(c.timingRaw))
          warn(c.index, "timing_format", "timing line differs from hh:mm:ss,mmm --> hh:mm:ss,mmm: " + c.timingRaw);
      }
      int64_t a = 0, b = 0;
      if (!parseTiming(c.timingRaw, d_.format, a, b)) {
        warn(c.index, "bad_timing", "unparsable timing: " + c.timingRaw);
      } else {
        if (b < a) warn(c.index, "reversed_time", "end before start");
        if (prevEnd >= 0 && a < prevEnd) warn(c.index, "overlap", "starts before the previous cue ends");
        prevEnd = b;
      }
      if (c.plainText().empty()) warn(c.index, "empty_text", "cue has no text");
    }
  }

  // ---- ASS / SSA ----------------------------------------------------------------------------------------------
  static bool startsWithCi(std::string_view v, std::string_view prefix) {
    if (v.size() < prefix.size()) return false;
    for (std::size_t i = 0; i < prefix.size(); ++i) {
      char a = v[i], b = prefix[i];
      if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
      if (a != b) return false;
    }
    return true;
  }

  void parseAss() {
    const std::size_t nl = L_.size();
    std::size_t ev = nl;
    for (std::size_t i = 0; i < nl; ++i) {
      if (startsWithCi(trim(content(i)), "[events]")) { ev = i; break; }
    }
    if (ev == nl) {
      d_.headerRaw = s_;
      warn(0, "no_events", "no [Events] section");
      return;
    }
    std::size_t textField = 9, startField = 1, endField = 2;
    struct Line { std::size_t line; std::size_t prefixLen; };
    std::vector<Line> cues;
    for (std::size_t i = ev + 1; i < nl; ++i) {
      std::string_view v = content(i);
      std::string_view t = trim(v);
      if (!t.empty() && t[0] == '[') break;  // next section: nothing after it is a cue
      if (startsWithCi(t, "format:")) {
        std::size_t idx = 0, pos = v.find(':') + 1;
        while (pos <= v.size()) {
          std::size_t c = v.find(',', pos);
          std::string_view name = trim(v.substr(pos, c == std::string_view::npos ? std::string_view::npos : c - pos));
          if (startsWithCi(name, "text") && name.size() == 4) textField = idx;
          if (startsWithCi(name, "start") && name.size() == 5) startField = idx;
          if (startsWithCi(name, "end") && name.size() == 3) endField = idx;
          ++idx;
          if (c == std::string_view::npos) break;
          pos = c + 1;
        }
        continue;
      }
      if (!startsWithCi(v, "dialogue:")) continue;
      std::size_t pos = v.find(':') + 1, commas = 0;
      while (commas < textField) {
        std::size_t c = v.find(',', pos);
        if (c == std::string_view::npos) break;
        pos = c + 1;
        ++commas;
      }
      if (commas < textField) {
        warn(0, "bad_dialogue", "Dialogue line with too few fields kept verbatim: " + std::string(v.substr(0, 60)));
        continue;
      }
      cues.push_back({i, pos});
    }
    if (cues.empty()) {
      d_.headerRaw = s_;
      warn(0, "no_cues", "no Dialogue lines");
      return;
    }
    d_.headerRaw = range(0, L_[cues[0].line].b);
    d_.cues.reserve(cues.size());
    for (std::size_t k = 0; k < cues.size(); ++k) {
      const std::size_t i = cues[k].line;
      std::string_view v = content(i);
      Cue c;
      c.index = static_cast<uint32_t>(k + 1);
      c.fromSource = true;
      c.styleRaw = std::string(v.substr(0, cues[k].prefixLen));
      // Start and End fields for timingRaw.
      std::vector<std::string_view> fields;
      std::size_t pos = v.find(':') + 1;
      for (std::size_t f = 0; f < textField; ++f) {
        std::size_t cpos = v.find(',', pos);
        fields.push_back(v.substr(pos, cpos - pos));
        pos = cpos + 1;
      }
      if (startField < fields.size() && endField < fields.size()) {
        c.timingRaw = std::string(fields[startField]) + "," + std::string(fields[endField]);
      }
      c.spans = splitSpans(std::string(v.substr(cues[k].prefixLen)), Format::Ass);
      c.parsedText = detail::renderSpans(c.spans, std::string());
      c.textEol = eol(i);
      const bool last = k + 1 == cues.size();
      std::size_t tailEnd = last ? s_.size() : L_[cues[k + 1].line].b;
      if (last) d_.trailerRaw = range(L_[i].n, tailEnd);
      else c.tailRaw = range(L_[i].n, tailEnd);
      int64_t a = 0, b = 0;
      if (!parseTiming(c.timingRaw, Format::Ass, a, b)) warn(c.index, "bad_timing", "unparsable timing: " + c.timingRaw);
      else if (b < a) warn(c.index, "reversed_time", "end before start");
      d_.cues.push_back(std::move(c));
    }
  }

  // ---- Plain text: one cue per paragraph ----------------------------------------------------------------------
  void parseTxt() {
    const std::size_t nl = L_.size();
    std::size_t i = 0;
    while (i < nl && blank(i)) ++i;
    if (i == nl) {
      d_.headerRaw = s_;
      return;
    }
    d_.headerRaw = range(0, L_[i].b);
    uint32_t index = 0;
    while (i < nl) {
      std::size_t j = i;
      while (j < nl && !blank(j)) ++j;
      Cue c;
      c.index = ++index;
      c.fromSource = true;
      setText(c, i, j, Format::Txt);
      std::size_t next = j;
      while (next < nl && blank(next)) ++next;
      std::string tail = range(L_[j - 1].n, next < nl ? L_[next].b : s_.size());
      if (next < nl) c.tailRaw = std::move(tail);
      else d_.trailerRaw = std::move(tail);
      d_.cues.push_back(std::move(c));
      i = next;
    }
  }

  const std::string& s_;
  Document& d_;
  std::vector<LineRef> L_;
};

bool containsCi(std::string_view hay, std::string_view needle) {
  if (needle.size() > hay.size()) return false;
  for (std::size_t i = 0; i + needle.size() <= hay.size(); ++i) {
    bool ok = true;
    for (std::size_t k = 0; k < needle.size() && ok; ++k) {
      char a = hay[i + k];
      if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
      ok = a == needle[k];
    }
    if (ok) return true;
  }
  return false;
}

Format sniff(const std::string& s, Format hint) {
  if (hint == Format::Txt) return hint;
  if (s.compare(0, 6, "WEBVTT") == 0) return Format::Vtt;
  std::string_view head = std::string_view(s).substr(0, 65536);
  if (containsCi(head, "[script info]") || containsCi(head, "[events]")) return Format::Ass;
  if (hint == Format::Ass && s.find("-->") != std::string::npos) return Format::Srt;
  return hint;
}

const char* formatName(Format f) {
  switch (f) {
    case Format::Srt: return "srt";
    case Format::Vtt: return "vtt";
    case Format::Ass: return "ass";
    case Format::Txt: return "txt";
  }
  return "?";
}

}  // namespace

Result<Document> parse(const std::vector<uint8_t>& bytes, Format hint) {
  try {
    if (hint != Format::Srt && hint != Format::Vtt && hint != Format::Ass && hint != Format::Txt)
      return Result<Document>(ErrorCode::BadParams, "unknown subtitle format", "Choose an .srt, .vtt, .ass or .txt file.");
    detail::Decoded dec = detail::decode(bytes);
    Document d;
    d.encoding = dec.encoding;
    d.bom = dec.bom;
    if (dec.encoding == "windows-1252")
      d.warnings.push_back(Warning{0, "encoding_fallback", "not valid UTF-8; read as Windows-1252"});
    if (dec.lossy) d.warnings.push_back(Warning{0, "encoding_lossy", dec.lossDetail});
    d.format = sniff(dec.text, hint);
    if (d.format != hint)
      d.warnings.push_back(Warning{0, "format_mismatch", std::string("content looks like ") + formatName(d.format) +
                                                             ", not " + formatName(hint)});
    Parser(dec.text, d).run();
    return Result<Document>(std::move(d));
  } catch (const std::exception& e) {
    return Result<Document>(ErrorCode::Internal, std::string("subtitle parse failed: ") + e.what(),
                            "The subtitle file could not be read. Try saving it again as UTF-8.");
  } catch (...) {
    return Result<Document>(ErrorCode::Internal, "subtitle parse failed", "The subtitle file could not be read.");
  }
}

}  // namespace vp::subs
