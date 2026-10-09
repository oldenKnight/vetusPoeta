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
      // C24: when most words are mapped, an unmapped word goes with its mapped neighbour (a preposition with its noun:
      // "cum Henrīcō", a connector with the clause after it) instead of counting proportionally
      std::vector<int> off(srcOffset->begin(), srcOffset->end());
      size_t nm = 0;
      for (int o : off) nm += o >= 0;
      if (nm * 3 >= nt * 2)
        for (size_t i = 0; i < nt; ++i) {
          if (off[i] >= 0) continue;
          for (size_t j = i + 1; j < nt && off[i] < 0; ++j)
            if ((*srcOffset)[j] >= 0) off[i] = (*srcOffset)[j];
          for (size_t j = i; j > 0 && off[i] < 0; --j)
            if ((*srcOffset)[j - 1] >= 0) off[i] = (*srcOffset)[j - 1];
        }
      double before = 0;
      for (size_t i = 0; i < nt; ++i) {
        const int o = off[i];
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

namespace {
bool relativeWord(const std::string& k) {
  static const char* const kRel[] = {"qui",  "quae",  "quod", "quem",  "quam",   "cuius",  "cui",
                                     "quo",  "qua",   "quibus", "quos", "quas", "quorum", "quarum"};
  for (const char* r : kRel)
    if (k == r) return true;
  return false;
}
bool letters(const std::string& s) {
  for (char c : s)
    if ((unsigned char)c >= 0x80 || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) return true;
  return false;
}
// The last word of a source part when nothing (no punctuation) follows it: [start, end) in src.text, else {-1, -1}.
std::pair<int, int> danglingWord(const std::string& t, int a, int b) {
  while (b > a && isSpace(t[(size_t)b - 1])) --b;
  int e = b;
  while (b > a && ((t[(size_t)b - 1] >= 'a' && t[(size_t)b - 1] <= 'z') || (t[(size_t)b - 1] >= 'A' && t[(size_t)b - 1] <= 'Z') ||
                   t[(size_t)b - 1] == '\''))
    --b;
  if (b == e) return {-1, -1};
  return {b, e};
}
}  // namespace

bool regroupSentence(const frame::SourceSentence& src, const Latin& latin, const std::vector<int>& srcOffset,
                     std::vector<Latin>& out, std::vector<std::vector<size_t>>* order) {
  const size_t np = src.parts.size(), nt = latin.tokens.size();
  if (np < 2 || nt == 0 || srcOffset.size() != nt) return false;
  size_t mapped = 0;
  for (int o : srcOffset) mapped += o >= 0;
  if (mapped * 3 < nt * 2) return false;
  auto partOf = [&](int off) {
    size_t p = 0;
    for (size_t q = 1; q < np; ++q)
      if (off >= src.parts[q].start) p = q;
    return p;
  };
  std::vector<long> part(nt, -1);
  for (size_t i = 0; i < nt; ++i)
    if (srcOffset[i] >= 0) part[i] = (long)partOf(srcOffset[i]);
  for (size_t i = 0; i < nt; ++i) {
    if (srcOffset[i] >= 0) continue;
    for (size_t j = i + 1; j < nt && part[i] < 0; ++j)
      if (srcOffset[j] >= 0) part[i] = (long)partOf(srcOffset[j]);
    for (size_t j = i; j > 0 && part[i] < 0; --j)
      if (srcOffset[j - 1] >= 0) part[i] = (long)partOf(srcOffset[j - 1]);
    if (part[i] < 0) part[i] = 0;
  }
  // the Latin verb of an auxiliary or copula left at the end of a part goes to the next part
  static const char* const kAux[] = {"is",    "are",    "was",  "were",  "am",    "be",  "been", "will",
                                     "would", "can",    "could", "shall", "should", "must", "may", "might",
                                     "has",   "have",   "had",  "do",    "does",  "did",  "'s",  "'re"};
  for (size_t p = 0; p + 1 < np; ++p) {
    const std::pair<int, int> w = danglingWord(src.text, src.parts[p].start, src.parts[p].end);
    if (w.first < 0) continue;
    const std::string low = text::lower(src.text.substr((size_t)w.first, (size_t)(w.second - w.first)));
    bool aux = false;
    for (const char* a : kAux) aux = aux || low == a;
    if (!aux) continue;
    for (size_t i = 0; i < nt; ++i)
      if (srcOffset[i] == w.first && latin.tokens[i].features.pos == "verb") part[i] = (long)p + 1;
  }
  // the auxiliary of a perfect participle stays with it ("Cum sōl ortus est," | "avēs ...")
  for (size_t i = 1; i < nt; ++i) {
    const std::string k = text::latin_key(latin.tokens[i].text);
    if ((k == "est" || k == "sunt" || k == "es" || k == "sum" || k == "sumus" || k == "estis" || k == "erat" || k == "erant" ||
         k == "esset" || k == "essent" || k == "sit" || k == "sint" || k == "erit" || k == "erunt" || k == "esse") &&
        latin.tokens[i - 1].features.mood == "participle")
      part[i] = part[i - 1];
  }
  // C30: a form of esse goes with the predicate or place phrase right before it ("Capra aviae meae" | "trāns flūmen
  // est," -- not "... meae est,"); an adjective goes with the noun it agrees with ("capra stulta?" stays one phrase)
  for (size_t i = 1; i < nt; ++i) {
    const std::string k = text::latin_key(latin.tokens[i].text);
    static const char* const kEsse[] = {"est", "sunt", "es", "sum", "sumus", "estis", "erat", "erant", "eram", "eras",
                                        "erit", "erunt", "fuit", "fuerunt", "esset", "essent", "sit", "sint"};
    bool esse = false;
    for (const char* e : kEsse) esse = esse || k == e;
    if (esse && part[i - 1] > part[i] && latin.tokens[i - 1].features.mood != "participle") part[i] = part[i - 1];
  }
  {
    auto agree = [&](size_t a, size_t n) {
      const rules::Features& fa = latin.tokens[a].features;
      const rules::Features& fn = latin.tokens[n].features;
      return (fa.pos == "adj" || fa.pos == "det" || fa.pos == "num" || (fa.pos == "pron" && !fa.case_.empty())) &&
             fn.pos == "noun" && !fa.case_.empty() && fa.case_ == fn.case_ && fa.number == fn.number &&
             (fa.gender.empty() || fn.gender.empty() || fa.gender == fn.gender ||
              fa.gender.find(fn.gender) != std::string::npos || fn.gender.find(fa.gender) != std::string::npos);
    };
    for (size_t i = 0; i < nt; ++i) {
      if (srcOffset[i] < 0) continue;
      long np2 = -1;
      // (the adjective's English word before the noun's, close by: an attribute, not a predicate "via angusta facta est")
      auto attr = [&](size_t a, size_t n2) {
        return srcOffset[n2] >= 0 && srcOffset[a] < srcOffset[n2] && srcOffset[n2] - srcOffset[a] <= 24;
      };
      if (i + 1 < nt && agree(i, i + 1) && attr(i, i + 1)) np2 = part[i + 1];
      else if (i > 0 && agree(i, i - 1) && attr(i, i - 1)) np2 = part[i - 1];
      // a genitive pronoun after its noun ("avia eius": "her" | "grandmother")
      else if (i > 0 && latin.tokens[i].features.pos == "pron" && latin.tokens[i].features.case_ == "genitive" &&
               latin.tokens[i - 1].features.pos == "noun")
        np2 = part[i - 1];
      if (np2 >= 0 && np2 != part[i]) part[i] = np2;
    }
  }
  // linkers: right to left, so "et nōn" moves together
  for (size_t i = nt - 1; i-- > 0;) {
    const rules::TokenView& t = latin.tokens[i];
    const std::string k = text::latin_key(t.text);
    if (part[i + 1] <= part[i]) continue;
    if (t.features.pos == "prep" && preposition(k)) { part[i] = part[i + 1]; continue; }
    if (conjunction(k) || relativeWord(k) || k == "non" || k == "donec" || k == "antequam") {
      // with the verb of the clause it opens (the first verb after it), else the nearest later part
      long best = -1;
      bool inRel = false;   // C30: a relative clause on the way has its own verb ("et avem ... quae sedēbat lātrāre coepit")
      for (size_t j = i + 1; j < nt && best < 0; ++j) {
        if (relativeWord(text::latin_key(latin.tokens[j].text)) && !relativeWord(k)) { inRel = true; continue; }
        if (latin.tokens[j].features.pos != "verb") continue;
        if (inRel) { inRel = false; continue; }
        best = part[j];
      }
      if (best < 0) {
        best = part[i + 1];
        for (size_t j = i + 1; j < nt; ++j)
          if (part[j] > part[i] && part[j] < best) best = part[j];
      }
      if (best > part[i]) part[i] = best;
    }
  }
  // every part with source letters gets a Latin word
  for (size_t p = 0; p < np; ++p) {
    const frame::CuePart& cp = src.parts[p];
    const std::string s = src.text.substr((size_t)cp.start, (size_t)std::max(0, cp.end - cp.start));
    bool any = false;
    for (size_t i = 0; i < nt && !any; ++i) any = part[i] == (long)p && letters(latin.tokens[i].text);
    if (letters(s) && !any) return false;
  }
  // the text: a prefix before the first token ("- "), each token with the marks after it, the final mark at the end;
  // C30: marks after the last space of a gap open the next word (an opening quotation mark: inquit. "Capram ...)
  const std::string prefix = latin.text.substr(0, (size_t)std::max(0, latin.tokens[0].start));
  std::vector<std::string> after(nt), before(nt);
  for (size_t i = 0; i < nt; ++i) {
    const size_t a = (size_t)latin.tokens[i].end;
    const size_t b = i + 1 < nt ? (size_t)latin.tokens[i + 1].start : latin.text.size();
    std::string g = a < b ? latin.text.substr(a, b - a) : std::string();
    const size_t sp = g.find_last_of(" \n\t");
    std::string m, n;
    for (size_t q = 0; q < g.size(); ++q)
      if (!isSpace(g[q])) (sp != std::string::npos && q > sp && i + 1 < nt ? n : m) += g[q];
    after[i] = m;
    if (i + 1 < nt) before[i + 1] = n;
  }
  const std::string finalMark = after[nt - 1];
  after[nt - 1].clear();
  out.assign(np, Latin{});
  if (order) order->assign(np, {});
  for (size_t p = 0; p < np; ++p) {
    Latin& L = out[p];
    if (p == 0) L.text = prefix;
    for (size_t i = 0; i < nt; ++i) {
      if (part[i] != (long)p) continue;
      const bool openQuote = p == 0 && L.text == prefix && !prefix.empty() && prefix.back() == '"';
      if (!L.text.empty() && !isSpace(L.text.back()) && !openQuote) L.text += ' ';
      rules::TokenView t = latin.tokens[i];
      L.text += before[i];
      t.start = (int)L.text.size();
      L.text += t.text;
      t.end = (int)L.text.size();
      L.text += after[i];
      L.tokens.push_back(std::move(t));
      if (order) (*order)[p].push_back(i);
    }
    if (p + 1 == np) L.text += finalMark;
  }
  return true;
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
