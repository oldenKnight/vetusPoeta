// Cue -> sentence mapping (DESIGN.md §10.1 item 1). Speech cues are joined into sentences across cue boundaries when a
// cue ends without terminal punctuation; speaker dashes start sentences; ♪ cues are songs; [..] / (..) are nonverbal.
#include <cstring>

#include "vp/frame.h"
#include "vp/text.h"

namespace vp::frame {
namespace {

bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

std::string trim(std::string_view s) {
  size_t a = 0, b = s.size();
  while (a < b && isSpace(s[a])) ++a;
  while (b > a && isSpace(s[b - 1])) --b;
  return std::string(s.substr(a, b - a));
}

bool hasSongMark(std::string_view s) {
  return s.find("\xE2\x99\xAA") != std::string_view::npos || s.find("\xE2\x99\xAB") != std::string_view::npos;
}

// Closing characters that may follow terminal punctuation.
bool closer(std::string_view s, size_t i, size_t& len) {
  if (i >= s.size()) return false;
  const char c = s[i];
  if (c == '"' || c == '\'' || c == ')' || c == ']' || c == '|') { len = 1; return true; }   // C22: a stray '|'
  if (s.compare(i, 3, "\xE2\x80\x9D") == 0 || s.compare(i, 3, "\xE2\x80\x99") == 0 || s.compare(i, 2, "\xC2\xBB") == 0) {
    len = s[i] == '\xC2' ? 2 : 3;
    return true;
  }
  return false;
}

bool terminalAt(std::string_view s, size_t i, size_t& len) {
  if (i >= s.size()) return false;
  const char c = s[i];
  if (c == '.' || c == '!' || c == '?') { len = 1; return true; }
  if (s.compare(i, 3, "\xE2\x80\xA6") == 0) { len = 3; return true; }   // …
  return false;
}

const char* const kAbbrev[] = {"mr", "mrs", "ms", "dr", "st", "sr", "sra", "srta", "jr", "vs", "prof", "sgt",
                               "capt", "col", "gen", "lt", "mt", "no", "etc"};

bool abbreviationBefore(std::string_view s, size_t dot) {
  size_t b = dot;
  while (b > 0 && ((s[b - 1] >= 'a' && s[b - 1] <= 'z') || (s[b - 1] >= 'A' && s[b - 1] <= 'Z'))) --b;
  if (b == dot) return false;
  if (b > 0 && !isSpace(s[b - 1]) && s[b - 1] != '-' && s[b - 1] != '"') return false;
  std::string w(s.substr(b, dot - b));
  for (char& ch : w) ch = (char)((ch >= 'A' && ch <= 'Z') ? ch + 32 : ch);
  for (const char* a : kAbbrev)
    if (w == a) return w != "no" || dot + 1 < s.size();   // "No." at the end of a line is the answer "no"
  return w.size() == 1 && dot + 1 < s.size() && s[b] >= 'A' && s[b] <= 'Z';   // initials "J. Smith"
}

struct Piece { std::string text; CueKind kind; bool dash = false; std::string open, close; };

// Splits one cue text into speech / nonverbal pieces (bracket groups) and speaker turns.
void splitCue(const std::string& t, std::vector<Piece>& out) {
  out.clear();
  size_t i = 0;
  std::string cur;
  auto flushSpeech = [&]() {
    std::string s = trim(cur);
    cur.clear();
    if (s.empty()) return;
    // speaker dashes: at the start, and " - " after terminal punctuation
    size_t start = 0;
    bool dash = false;
    if (s[0] == '-' && s.size() > 1 && (s[1] == ' ' || (unsigned char)s[1] >= 'A')) {
      dash = true;
      start = 1;
    }
    size_t k = start;
    while (k < s.size()) {
      size_t at = s.find(" -", k);
      if (at == std::string::npos) break;
      size_t back = at;
      size_t tl = 0;
      bool afterTerm = false;
      while (back > start && isSpace(s[back - 1])) --back;
      if (back > start) {
        // last char(s) before the dash are terminal punctuation (or a closing quote after it)
        size_t p = back - 1;
        size_t cl = 0;
        if (closer(s, p, cl) && p > start) --p;
        while (p > start && (s[p] & 0xC0) == 0x80) --p;   // UTF-8 lead byte
        afterTerm = terminalAt(s, p, tl);
      }
      const bool dashWord = at + 2 < s.size() && (s[at + 2] == ' ' || (unsigned char)s[at + 2] >= 'A');
      if (afterTerm && dashWord) {
        out.push_back(Piece{trim(s.substr(start, at - start)), CueKind::Speech, dash, "", ""});
        dash = true;
        start = at + 2;
        k = start;
      } else {
        k = at + 2;
      }
    }
    std::string rest = trim(s.substr(start));
    if (!rest.empty()) out.push_back(Piece{rest, CueKind::Speech, dash, "", ""});
  };
  while (i < t.size()) {
    const char c = t[i];
    if (c == '[' || c == '(') {
      const char close = c == '[' ? ']' : ')';
      const size_t j = t.find(close, i + 1);
      // C17: editorial text in square brackets inside speech ("They are rusted [so badly] that ...") stays in the
      // sentence (translated, brackets kept): an unfinished sentence before it in the cue, lower case, two words or more
      if (c == '[' && j != std::string::npos && !trim(cur).empty() && !endsSentence(trim(cur))) {
        const std::string in = trim(t.substr(i + 1, j - i - 1));
        if (!in.empty() && in[0] >= 'a' && in[0] <= 'z' && in.find(' ') != std::string::npos) {
          cur += t.substr(i, j - i + 1);
          i = j + 1;
          continue;
        }
      }
      if (j != std::string::npos) {
        flushSpeech();
        Piece p;
        p.text = trim(t.substr(i + 1, j - i - 1));
        p.kind = CueKind::Nonverbal;
        p.open = std::string(1, c);
        p.close = std::string(1, close);
        if (!p.text.empty()) out.push_back(std::move(p));
        i = j + 1;
        continue;
      }
    }
    cur += c;
    ++i;
  }
  flushSpeech();
}

// Sentence boundaries inside a speech piece: returns [start,end) chunks; `openEnd` tells whether the last chunk ends
// with terminal punctuation.
void chunks(const std::string& s, std::vector<std::pair<size_t, size_t>>& out, bool& lastClosed) {
  out.clear();
  size_t start = 0, i = 0;
  lastClosed = false;
  while (i < s.size()) {
    size_t tl = 0;
    if (terminalAt(s, i, tl)) {
      size_t j = i + tl;
      size_t more = 0;
      while (terminalAt(s, j, more)) j += more;
      size_t cl = 0;
      while (closer(s, j, cl)) j += cl;
      // C22: closing quotes after a space ("... was... \"") belong to this chunk: a chunk made only of closers
      // would be a sentence of its own that swallows the next cue
      {
        size_t k = j;
        while (k < s.size() && (isSpace(s[k]) || closer(s, k, cl))) k += isSpace(s[k]) ? 1 : cl;
        if (k >= s.size() && k > j && !trim(std::string_view(s).substr(j)).empty()) j = s.size();
      }
      const bool atEnd = j >= s.size();
      const bool spaceAfter = !atEnd && isSpace(s[j]);
      const bool abbr = s[i] == '.' && tl == 1 && j == i + 1 && abbreviationBefore(s, i);
      bool lowerNext = false;
      if (spaceAfter) {
        size_t k = j;
        while (k < s.size() && isSpace(s[k])) ++k;
        lowerNext = k < s.size() && s[k] >= 'a' && s[k] <= 'z';
      }
      const bool ellipsis = tl == 3 || (j - i >= 3 && s[i] == '.');
      if ((atEnd || spaceAfter) && !abbr && !(ellipsis && lowerNext)) {
        out.emplace_back(start, j);
        while (j < s.size() && isSpace(s[j])) ++j;
        start = j;
        i = j;
        if (atEnd) lastClosed = true;
        continue;
      }
      i = j;
      continue;
    }
    ++i;
  }
  if (start < s.size()) {
    std::string rest = trim(s.substr(start));
    if (!rest.empty()) out.emplace_back(start, s.size());
  }
}

// The text ends with clause-level punctuation: , ; : or a dash (— – -).
bool clauseEnd(std::string_view s) {
  size_t n = s.size();
  while (n > 0 && isSpace(s[n - 1])) --n;
  if (n == 0) return false;
  const char c = s[n - 1];
  if (c == ',' || c == ';' || c == ':' || c == '-') return true;
  return n >= 3 && (s.compare(n - 3, 3, "\xE2\x80\x94") == 0 || s.compare(n - 3, 3, "\xE2\x80\x93") == 0);
}

// The cue starts a clause of its own: a capital letter (after quotes) or a clause-initial conjunction.
bool startsClause(std::string_view s) {
  size_t a = 0;
  while (a < s.size() && (s[a] == '"' || s[a] == '\'' || s[a] == '-' || isSpace(s[a]))) ++a;
  if (a < s.size() && s.compare(a, 3, "\xE2\x80\x9C") == 0) a += 3;
  if (a >= s.size()) return false;
  if (s[a] >= 'A' && s[a] <= 'Z') return true;
  if ((unsigned char)s[a] == 0xC2 && a + 1 < s.size() && ((unsigned char)s[a + 1] == 0xBF || (unsigned char)s[a + 1] == 0xA1))
    return true;   // ¿ ¡
  if ((unsigned char)s[a] == 0xC3 && a + 1 < s.size() && (unsigned char)s[a + 1] >= 0x80 && (unsigned char)s[a + 1] <= 0x9E)
    return true;   // capital accented letter
  size_t b = a;
  while (b < s.size() && s[b] >= 'a' && s[b] <= 'z') ++b;
  const std::string_view w = s.substr(a, b - a);
  for (const char* c : {"and", "but", "or", "nor", "for", "so", "yet", "if", "when", "because", "then", "while",
                        "unless", "although", "though", "until", "till", "after", "before", "since", "as", "y", "pero",
                        "o", "ni", "porque", "si", "cuando", "aunque", "pues", "entonces"})
    if (w == c) return true;
  return false;
}

}  // namespace

bool endsSentence(std::string_view text) {
  std::string s = trim(text);
  size_t n = s.size();
  while (n > 0) {
    size_t cl = 0;
    size_t p = n - 1;
    while (p > 0 && (s[p] & 0xC0) == 0x80) --p;
    if (closer(s, p, cl) && p + cl == n) {
      n = p;
      while (n > 0 && isSpace(s[n - 1])) --n;   // C22: "was... \"" (a space before the closing quote)
      continue;
    }
    break;
  }
  if (n == 0) return false;
  size_t p = n - 1;
  while (p > 0 && (s[p] & 0xC0) == 0x80) --p;
  size_t tl = 0;
  return terminalAt(s, p, tl) && p + tl == n;
}

std::vector<SourceSentence> mapSentences(const std::vector<std::string>& cueTexts) {
  std::vector<SourceSentence> out;
  long open = -1;                     // index of a speech sentence waiting for its continuation
  std::vector<Piece> pieces;
  std::vector<std::pair<size_t, size_t>> ch;
  for (size_t ci = 0; ci < cueTexts.size(); ++ci) {
    const std::string t = trim(cueTexts[ci]);
    if (t.empty()) { open = -1; continue; }
    if (hasSongMark(t)) {
      open = -1;
      SourceSentence s;
      s.kind = CueKind::Song;
      size_t a = 0, b = t.size();
      // leading / trailing ♪ ♫ marks and spaces
      auto isMark = [&](size_t at) {
        return t.compare(at, 3, "\xE2\x99\xAA") == 0 || t.compare(at, 3, "\xE2\x99\xAB") == 0;
      };
      while (a < b && (isMark(a) || isSpace(t[a]))) a += isMark(a) ? 3 : 1;
      while (b > a && (isSpace(t[b - 1]) || (b >= 3 && isMark(b - 3)))) b -= isSpace(t[b - 1]) ? 1 : 3;
      s.prefix = t.substr(0, a);
      s.suffix = t.substr(b);
      s.text = t.substr(a, b - a);
      s.parts.push_back(CuePart{ci, 0, (int)s.text.size()});
      out.push_back(std::move(s));
      continue;
    }
    splitCue(t, pieces);
    for (size_t pi = 0; pi < pieces.size(); ++pi) {
      Piece& p = pieces[pi];
      if (p.kind == CueKind::Nonverbal) {
        open = -1;
        SourceSentence s;
        s.kind = CueKind::Nonverbal;
        s.text = p.text;
        s.prefix = p.open;
        s.suffix = p.close;
        s.parts.push_back(CuePart{ci, 0, (int)s.text.size()});
        out.push_back(std::move(s));
        continue;
      }
      if (p.dash) open = -1;
      bool lastClosed = false;
      chunks(p.text, ch, lastClosed);
      for (size_t k = 0; k < ch.size(); ++k) {
        const std::string piece = trim(std::string_view(p.text).substr(ch[k].first, ch[k].second - ch[k].first));
        if (piece.empty()) continue;
        if (k == 0 && open >= 0) {
          SourceSentence& s = out[(size_t)open];
          s.text += ' ';
          const int st = (int)s.text.size();
          s.text += piece;
          s.parts.push_back(CuePart{ci, st, (int)s.text.size()});
        } else {
          SourceSentence s;
          s.kind = CueKind::Speech;
          s.dash = p.dash && k == 0;
          s.text = piece;
          s.parts.push_back(CuePart{ci, 0, (int)s.text.size()});
          out.push_back(std::move(s));
        }
        open = -1;
        const bool last = k + 1 == ch.size();
        if (last && !endsSentence(piece)) open = (long)out.size() - 1;
        // C15: a cue that ends at a clause boundary ("," ";" ":" or a dash) does not continue into a next cue that
        // starts a clause of its own (a capital letter, or a conjunction / subordinator): each cue is rendered as the
        // clause it is, which keeps the cue mapping exact ("..., my aunt and uncle," | "Can you help me ...?")
        if (last && open >= 0 && pi + 1 == pieces.size() && ci + 1 < cueTexts.size() && clauseEnd(piece) &&
            startsClause(trim(cueTexts[ci + 1])))
          open = -1;
        // an ellipsis at the end of a cue continues only when the next cue starts with "..." or lower case
        if (last && open < 0 && pi + 1 == pieces.size() && ci + 1 < cueTexts.size()) {
          const std::string& pc = piece;
          const bool ell = (pc.size() >= 3 && pc.compare(pc.size() - 3, 3, "...") == 0) ||
                           (pc.size() >= 3 && pc.compare(pc.size() - 3, 3, "\xE2\x80\xA6") == 0);
          if (ell) {
            const std::string nx = trim(cueTexts[ci + 1]);
            if (!nx.empty() && (nx.compare(0, 3, "...") == 0 || nx.compare(0, 3, "\xE2\x80\xA6") == 0 ||
                                (nx[0] >= 'a' && nx[0] <= 'z')))
              open = (long)out.size() - 1;
          }
        }
      }
    }
    // a nonverbal piece at the end of a cue closes any open sentence
    if (!pieces.empty() && pieces.back().kind == CueKind::Nonverbal) open = -1;
  }
  return out;
}

}  // namespace vp::frame
