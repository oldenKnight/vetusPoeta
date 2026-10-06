// Cue assembly (DESIGN.md §10.5).
#include "vp/cue.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "vp/text.h"

namespace vp::cue {
namespace {

bool isSpace(char c) { return c == ' ' || c == '\n' || c == '\t' || c == '\r'; }

std::string trim(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && isSpace(s[a])) ++a;
  while (b > a && isSpace(s[b - 1])) --b;
  return s.substr(a, b - a);
}

bool conjunction(const std::string& k) {
  static const char* const kConj[] = {"et", "sed", "aut", "quod", "quia", "cum", "si", "ut", "ne", "quamquam",
                                      "antequam", "postquam", "dum", "nisi", "neque", "nec", "atque", "ac"};
  for (const char* c : kConj)
    if (k == c) return true;
  return false;
}
bool preposition(const std::string& k) {
  static const char* const kPrep[] = {"in", "ad", "ab", "a", "ex", "e", "de", "cum", "per", "pro", "sine", "post",
                                      "ante", "apud", "sub", "inter", "contra", "prope", "propter", "trans", "circum",
                                      "super"};
  for (const char* c : kPrep)
    if (k == c) return true;
  return false;
}
bool nominal(const rules::Features& f) {
  return f.pos == "noun" || f.pos == "adj" || f.pos == "pron" || f.pos == "det" || f.pos == "num" || f.pos == "name" ||
         f.mood == "participle";
}

}  // namespace

const subs::BreakHints& latinBreakHints() {
  static const subs::BreakHints h = [] {
    subs::BreakHints b;
    for (const char* w : {"et", "sed", "aut", "quod", "quia", "cum", "sī", "si", "ut", "nē", "ne", "in", "ad", "ab",
                          "ā", "ex", "ē", "dē", "de", "per", "prō", "pro", "quamquam", "antequam", "postquam", "dum",
                          "nisi", "neque"})
      b.breakBefore.emplace_back(w);
    return b;
  }();
  return h;
}

std::vector<Latin> splitSentence(const frame::SourceSentence& src, const Latin& latin,
                                 const std::vector<int>* srcOffset) {
  std::vector<Latin> out(src.parts.empty() ? 1 : src.parts.size());
  if (out.size() == 1) { out[0] = latin; return out; }
  const size_t nt = latin.tokens.size();
  const double srcLen = std::max<size_t>(1, src.text.size());
  const double laLen = std::max<size_t>(1, latin.text.size());
  // candidate boundaries: before token i (1 <= i < nt), with a quality (0 = forbidden)
  std::vector<double> quality(nt, 0.0);
  for (size_t i = 1; i < nt; ++i) {
    const rules::TokenView& a = latin.tokens[i - 1];
    const rules::TokenView& b = latin.tokens[i];
    const std::string gap = latin.text.substr((size_t)a.end, (size_t)std::max(0, b.start - a.end));
    const std::string ka = text::latin_key(a.text), kb = text::latin_key(b.text);
    double q = 0.5;
    if (gap.find_first_of(",;:") != std::string::npos) q = 3;
    else if (conjunction(kb)) q = 2;
    else if (preposition(kb)) q = 1;
    if (q < 3) {
      if (preposition(ka) && a.features.pos == "prep") q = 0;
      if (nominal(a.features) && nominal(b.features) && !a.features.case_.empty() &&
          a.features.case_ == b.features.case_ && a.features.number == b.features.number)
        q = 0;   // same NP
    }
    quality[i] = q;
  }
  std::vector<size_t> cuts;   // token index where each later part starts
  size_t lo = 1;
  const bool mapped = srcOffset && srcOffset->size() == nt;
  for (size_t p = 1; p < src.parts.size(); ++p) {
    double share = src.parts[p].start / srcLen;
    if (mapped) {   // share of Latin words translating earlier parts (unmapped words count proportionally)
      double before = 0;
      for (size_t i = 0; i < nt; ++i) {
        const int o = (*srcOffset)[i];
        if (o < 0) before += share;
        else if (o < src.parts[p].start) before += 1;
      }
      share = before / std::max<size_t>(1, nt);
    }
    const double target = mapped && nt ? (share * nt < nt ? latin.tokens[std::min(nt - 1, (size_t)std::lround(share * nt))].start
                                                          : laLen)
                                       : share * laLen;
    size_t best = 0;
    double bestCost = 1e9;
    for (size_t i = lo; i < nt; ++i) {
      if (quality[i] <= 0) continue;
      // leave room for the remaining parts
      if (nt - i < src.parts.size() - p) break;
      const double cost = std::fabs(latin.tokens[i].start - target) / laLen - 0.08 * quality[i];
      if (cost < bestCost) { bestCost = cost; best = i; }
    }
    if (best == 0) {
      // no allowed boundary: proportional token position
      size_t i = (size_t)std::lround(share * (double)nt);
      if (i < lo) i = lo;
      if (i >= nt) i = nt;   // empty piece
      best = i;
    }
    cuts.push_back(best);
    lo = best + 1;
  }
  size_t from = 0;
  for (size_t p = 0; p < out.size(); ++p) {
    const size_t to = p < cuts.size() ? std::min(cuts[p], nt) : nt;
    if (from >= to) { from = std::max(from, to); continue; }
    const int bs = latin.tokens[from].start;
    const int be = p + 1 < out.size() && to < nt ? latin.tokens[to].start : (int)latin.text.size();
    std::string piece = latin.text.substr((size_t)bs, (size_t)(be - bs));
    // trailing spaces belong to no piece
    while (!piece.empty() && isSpace(piece.back())) piece.pop_back();
    out[p].text = piece;
    for (size_t i = from; i < to; ++i) {
      rules::TokenView t = latin.tokens[i];
      t.start -= bs;
      t.end -= bs;
      out[p].tokens.push_back(std::move(t));
    }
    from = to;
  }
  // leading text before the first token (rare: none) and empty parts are left empty
  return out;
}

void append(Latin& cue, const Latin& piece) {
  if (piece.text.empty()) return;
  int base = (int)cue.text.size();
  if (!cue.text.empty()) { cue.text += ' '; ++base; }
  cue.text += piece.text;
  for (rules::TokenView t : piece.tokens) {
    t.start += base;
    t.end += base;
    cue.tokens.push_back(std::move(t));
  }
}

Layout layout(const std::string& text, int maxLine, int maxLines) {
  Layout l;
  l.lines = subs::breakLines(text, latinBreakHints(), maxLine, maxLines, &l.overflow);
  for (size_t i = 0; i < l.lines.size(); ++i) {
    if (i) l.joined += '\n';
    l.joined += l.lines[i];
  }
  return l;
}

void relocate(const std::string& text, std::vector<rules::TokenView>& tokens) {
  size_t at = 0;
  for (rules::TokenView& t : tokens) {
    if (t.text.empty()) { t.start = t.end = (int)at; continue; }
    const size_t p = text.find(t.text, at);
    if (p == std::string::npos) { t.start = t.end = -1; continue; }
    t.start = (int)p;
    t.end = (int)(p + t.text.size());
    at = p + t.text.size();
  }
}

TagResult applyTags(const std::vector<subs::Span>& source, const std::string& latin) {
  TagResult r;
  // leading position tags ({\an8}) are kept; then one wrapping pair, if the whole text is inside it
  size_t a = 0, b = source.size();
  std::vector<subs::Span> lead;
  while (a < b && source[a].kind == subs::Span::Tag && source[a].raw.size() > 2 && source[a].raw[0] == '{' &&
         source[a].raw.find("\\an") != std::string::npos) {
    lead.push_back(source[a]);
    ++a;
  }
  // skip empty text spans at the edges
  auto blank = [](const subs::Span& s) { return s.kind == subs::Span::Text && trim(s.raw).empty(); };
  while (a < b && blank(source[a])) ++a;
  while (b > a && blank(source[b - 1])) --b;
  bool wrapped = false;
  size_t tags = 0;
  for (size_t i = a; i < b; ++i)
    if (source[i].kind == subs::Span::Tag) ++tags;
  if (b - a >= 2 && source[a].kind == subs::Span::Tag && source[b - 1].kind == subs::Span::Tag && tags == 2) wrapped = true;
  for (const subs::Span& s : lead) r.spans.push_back(s);
  if (wrapped) r.spans.push_back(source[a]);
  subs::Span t;
  t.kind = subs::Span::Text;
  t.raw = latin;
  r.spans.push_back(t);
  if (wrapped) r.spans.push_back(source[b - 1]);
  size_t other = 0;
  for (const subs::Span& s : source)
    if (s.kind == subs::Span::Tag) ++other;
  r.approximated = other > lead.size() + (wrapped ? 2 : 0);
  return r;
}

std::string nonverbal(const std::string& inner, const curated::CuratedData& cd, bool& translated) {
  translated = false;
  const std::string k = text::lower(trim(inner));
  for (const curated::PairEntry& e : cd.nonverbal())
    if (text::lower(e.a) == k) {
      translated = true;
      return e.b;
    }
  return inner;
}

}  // namespace vp::cue
