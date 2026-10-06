// Phrasebook pre-pass (DESIGN.md §10.1 item 4): patterns of data/curated/phrasebook_*_la.tsv compiled once (the
// contraction table applied to the pattern words, so "i'm late" matches the expanded "I am late"), matched
// longest-first on the token stream; every pattern word matches a token's lower-case form or its lemma.
#include <algorithm>

#include "vp/frame.h"
#include "vp/text.h"

namespace vp::frame {
namespace {

std::vector<std::string> splitOn(const std::string& s, char sep) {
  std::vector<std::string> out;
  size_t a = 0;
  for (;;) {
    size_t b = s.find(sep, a);
    out.push_back(s.substr(a, b == std::string::npos ? std::string::npos : b - a));
    if (b == std::string::npos) break;
    a = b + 1;
  }
  return out;
}

bool isPunct(const nlp::Token& t) { return t.upos == "PUNCT" || t.upos == "SYM"; }

bool tokenIs(const nlp::Token& t, const std::string& w) {
  if (t.lower == w) return true;
  if (!t.lemma.empty() && text::lower(t.lemma) == w) return true;
  return false;
}

bool npToken(const nlp::Token& t) {
  return t.upos == "DET" || t.upos == "ADJ" || t.upos == "NUM" || t.upos == "NOUN" || t.upos == "PROPN" ||
         t.upos == "PRON" || (t.upos == "ADV" && (t.lower == "very" || t.lower == "too"));
}
bool npEnd(const nlp::Token& t) { return t.upos == "NOUN" || t.upos == "PROPN" || t.upos == "PRON" || t.upos == "NUM"; }
bool capitalised(const nlp::Token& t) { return !t.text.empty() && t.text[0] >= 'A' && t.text[0] <= 'Z'; }

}  // namespace

void Phrasebook::build(const std::vector<curated::PhraseEntry>& entries,
                       const std::vector<curated::PairEntry>& contractions) {
  pats_.clear();
  auto expand = [&](const std::string& w, std::vector<std::string>& out) {
    for (const curated::PairEntry& c : contractions)
      if (c.a == w) {
        for (const std::string& x : splitOn(c.b, ' '))
          if (!x.empty()) out.push_back(x);
        return;
      }
    out.push_back(w);
  };
  for (size_t ei = 0; ei < entries.size(); ++ei) {
    const std::string pat = text::lower(entries[ei].pattern);
    Pattern p;
    p.entry = (int)ei;
    int slotNo = 0;
    size_t i = 0;
    bool bad = false;
    while (i < pat.size()) {
      while (i < pat.size() && pat[i] == ' ') ++i;
      if (i >= pat.size()) break;
      size_t j = i;
      if (pat[i] == '(') {
        j = pat.find(')', i);
        if (j == std::string::npos) { bad = true; break; }
        Elem e;
        e.optional = true;
        for (const std::string& alt : splitOn(pat.substr(i + 1, j - i - 1), '|')) {
          std::vector<std::string> ws;
          expand(alt, ws);
          if (ws.size() == 1) e.words.push_back(ws[0]);
          else if (!ws.empty()) {   // a multi-word optional alternative: keep its first word only
            e.words.push_back(ws[0]);
          }
        }
        p.elems.push_back(std::move(e));
        i = j + 1;
        continue;
      }
      while (j < pat.size() && pat[j] != ' ') ++j;
      const std::string w = pat.substr(i, j - i);
      i = j;
      if (w.size() > 2 && w.front() == '{' && w.back() == '}') {
        Elem e;
        const std::string k = w.substr(1, w.size() - 2);
        e.slot = slotNo++;
        if (k == "np") e.kind = SlotKind::NP;
        else if (k == "name") e.kind = SlotKind::Name;
        else if (k == "vp") e.kind = SlotKind::VP;
        else if (k == "adj") e.kind = SlotKind::Adj;
        else if (k == "num") e.kind = SlotKind::Num;
        else if (k == "wh") e.kind = SlotKind::Wh;
        else { bad = true; break; }
        p.elems.push_back(std::move(e));
        continue;
      }
      std::vector<std::string> alts = splitOn(w, '|');
      if (alts.size() > 1) {
        Elem e;
        for (const std::string& a : alts) e.words.push_back(a);
        p.elems.push_back(std::move(e));
        continue;
      }
      std::vector<std::string> ws;
      expand(w, ws);
      for (const std::string& x : ws) {
        Elem e;
        e.words.push_back(x);
        p.elems.push_back(std::move(e));
      }
    }
    if (!bad && !p.elems.empty()) pats_.push_back(std::move(p));
  }
}

bool Phrasebook::matchFrom(const std::vector<nlp::Token>& toks, const Pattern& p, size_t ei, int at,
                           std::vector<PhraseSlot>& slots, int& end) const {
  if (ei == p.elems.size()) { end = at; return true; }
  const Elem& e = p.elems[ei];
  const int n = (int)toks.size();
  if (e.slot < 0) {
    if (at < n && !isPunct(toks[(size_t)at]))
      for (const std::string& w : e.words)
        if (tokenIs(toks[(size_t)at], w) && matchFrom(toks, p, ei + 1, at + 1, slots, end)) return true;
    if (e.optional) return matchFrom(toks, p, ei + 1, at, slots, end);
    return false;
  }
  // slots: candidate end positions, longest first
  std::vector<int> ends;
  switch (e.kind) {
    case SlotKind::NP: {
      int k = at;
      while (k < n && npToken(toks[(size_t)k])) ++k;
      for (int x = k; x > at; --x) {
        // a noun phrase ends with a noun, or with adjectives after its noun ("la pintura roja") (C13)
        bool nounBefore = false;
        for (int y = at; y < x - 1; ++y) nounBefore = nounBefore || toks[(size_t)y].upos == "NOUN";
        if (npEnd(toks[(size_t)x - 1]) || (nounBefore && toks[(size_t)x - 1].upos == "ADJ")) ends.push_back(x);
      }
      break;
    }
    case SlotKind::Name: {
      int k = at;
      while (k < n && k - at < 3 && (toks[(size_t)k].upos == "PROPN" || (capitalised(toks[(size_t)k]) && k > 0 &&
                                                                       !isPunct(toks[(size_t)k]))))
        ++k;
      for (int x = k; x > at; --x) ends.push_back(x);
      break;
    }
    case SlotKind::Adj: {
      int k = at;
      if (k < n && toks[(size_t)k].upos == "ADV") ++k;
      if (k < n && toks[(size_t)k].upos == "ADJ") ends.push_back(k + 1);
      break;
    }
    case SlotKind::Num:
      if (at < n && toks[(size_t)at].upos == "NUM") ends.push_back(at + 1);
      break;
    case SlotKind::Wh: {   // a wh word, then the rest of the clause up to punctuation
      static const char* const kWh[] = {"where", "what", "which", "who", "whom", "whose", "how", "when", "why",
                                        "whether", "if", "dónde", "qué", "quién", "cómo", "cuándo", "cuál"};
      bool wh = false;
      if (at < n)
        for (const char* w : kWh) wh = wh || toks[(size_t)at].lower == w;
      if (!wh) break;
      int k = at + 1;
      while (k < n && !isPunct(toks[(size_t)k])) ++k;
      ends.push_back(k);
      break;
    }
    case SlotKind::VP: {
      int k = at;
      bool verb = false;
      while (k < n && !isPunct(toks[(size_t)k])) {
        if (toks[(size_t)k].upos == "VERB" || toks[(size_t)k].upos == "AUX") verb = true;
        ++k;
      }
      if (verb)
        for (int x = k; x > at; --x) ends.push_back(x);
      break;
    }
  }
  for (int x : ends) {
    if ((int)slots.size() <= e.slot) slots.resize((size_t)e.slot + 1);
    slots[(size_t)e.slot].kind = e.kind;
    slots[(size_t)e.slot].first = at;
    slots[(size_t)e.slot].last = x - 1;
    if (matchFrom(toks, p, ei + 1, x, slots, end)) return true;
  }
  return false;
}

bool Phrasebook::match(const std::vector<nlp::Token>& toks, int from, PhraseMatch& out) const {
  int bestEnd = -1;
  const Pattern* best = nullptr;
  std::vector<PhraseSlot> slots, bestSlots;
  for (const Pattern& p : pats_) {
    slots.clear();
    int end = -1;
    if (matchFrom(toks, p, 0, from, slots, end) && end > from && end > bestEnd) {
      bestEnd = end;
      best = &p;
      bestSlots = slots;
    }
  }
  if (!best) return false;
  out = PhraseMatch{};
  out.entry = best->entry;
  out.first = from;
  out.last = bestEnd - 1;
  out.slots = std::move(bestSlots);
  return true;
}

}  // namespace vp::frame
