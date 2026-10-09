// GreekPath (engine_grc.h): the EN/ES -> Greek pipeline, the Greek checks and confidence, the Greek -> EN/ES pairs
// through grc2x, check() and inspect() for Greek. Mirrors src/engine/engine.cpp (the Latin path) step by step.
#include "engine_grc/engine_grc.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <functional>
#include <sstream>

#include "frame/english.h"
#include "la2x/internal.h"
#include "vp/check.h"
#include "vp/check_grc.h"
#include "vp/cue.h"
#include "vp/grc2x.h"
#include "vp/morph.h"
#include "vp/morph_grc.h"
#include "vp/realise_grc.h"
#include "vp/subs.h"
#include "vp/text.h"
#include "vp/transfer.h"
#include "vp/transfer_grc.h"

namespace vp::grc {

using rules::Alternative;
using rules::Check;
using rules::Confidence;
using rules::CueInput;
using rules::CueOutput;
using rules::Reason;

namespace {

namespace stdfs = std::filesystem;

std::string jsonEscape(const std::string& s) {
  std::string o;
  for (char c : s) {
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if ((unsigned char)c < 0x20) o += ' ';
    else o += c;
  }
  return o;
}

std::string fmt(double v) {
  std::ostringstream os;
  os.setf(std::ios::fixed);
  os.precision(3);
  os << v;
  return os.str();
}

void addFlag(std::vector<std::string>& f, const std::string& x) {
  if (std::find(f.begin(), f.end(), x) == f.end()) f.push_back(x);
}

// Rebuilds `text` with new token strings (same order); gaps between tokens are kept.
void rewriteTokens(cue::Latin& l, const std::vector<std::string>& texts) {
  std::string out;
  int cursor = 0;
  for (size_t i = 0; i < l.tokens.size(); ++i) {
    rules::TokenView& t = l.tokens[i];
    if (t.start < cursor || t.end > (int)l.text.size() || t.start > t.end) continue;
    out += l.text.substr((size_t)cursor, (size_t)(t.start - cursor));
    const int ns = (int)out.size();
    out += texts[i];
    cursor = t.end;
    t.start = ns;
    t.end = (int)out.size();
    t.text = texts[i];
  }
  out += l.text.substr((size_t)std::min<int>(cursor, (int)l.text.size()));
  l.text = std::move(out);
}

// Trailing sentence punctuation the realiser added (". ; ! , ·" and spaces).
void stripFinal(std::string& t) {
  for (;;) {
    if (t.empty()) return;
    const char b = t.back();
    if (b == '.' || b == ';' || b == '!' || b == ',' || b == ' ' || b == '?') { t.pop_back(); continue; }
    if (t.size() >= 2 && t.compare(t.size() - 2, 2, "\xC2\xB7") == 0) { t.resize(t.size() - 2); continue; }
    return;
  }
}

std::string greekSeparator(const std::string& sep) {
  if (sep == ";" || sep == ":") return "\xC2\xB7";   // ano teleia
  if (sep.empty()) return "";
  return ",";
}

std::vector<std::string> splitWords(const std::string& s) {
  std::vector<std::string> out;
  size_t a = 0;
  while (a < s.size()) {
    while (a < s.size() && s[a] == ' ') ++a;
    size_t b = a;
    while (b < s.size() && s[b] != ' ') ++b;
    if (b > a) out.push_back(s.substr(a, b - a));
    a = b;
  }
  return out;
}

struct Layout { std::string joined; bool overflow = false; };
Layout greekLayout(const std::string& text, int maxLine, int maxLines) {
  Layout l;
  std::string flat = text;
  std::replace(flat.begin(), flat.end(), '\n', ' ');
  const std::vector<std::string> lines = subs::breakLines(flat, greekBreakHints(), maxLine, maxLines, &l.overflow);
  for (size_t i = 0; i < lines.size(); ++i) {
    if (i) l.joined += '\n';
    l.joined += lines[i];
  }
  return l;
}

// C18: token indices of a frame shifted by `d` (a frame analysed on its own and merged into another sentence)
void shiftNP(frame::SemNP& n, int d);
void shiftFrame(frame::SemFrame& f, int d) {
  auto sh = [d](int& t) { if (t >= 0) t += d; };
  auto shv = [d](std::vector<int>& v) { for (int& t : v) if (t >= 0) t += d; };
  sh(f.pred.token);
  sh(f.pred.complementToken);
  shv(f.pred.auxTokens);
  if (f.hasSubject) shiftNP(f.subject, d);
  if (f.hasObject) shiftNP(f.object, d);
  if (f.hasIndirect) shiftNP(f.indirectObject, d);
  for (auto& o : f.obliques) { sh(o.token); shiftNP(o.np, d); }
  for (auto& n : f.predicative) shiftNP(n, d);
  for (auto& a : f.predAdj) { sh(a.token); shv(a.advTokens); }
  for (auto& n : f.vocatives) shiftNP(n, d);
  for (auto& a : f.adverbs) sh(a.token);
  for (auto& sb : f.subordinate)
    for (auto& x : sb.frame) shiftFrame(x, d);
  sh(f.wh.token);
  shv(f.tokens);
  for (auto& n : f.objComplement) shiftNP(n, d);
  for (auto& a : f.objComplementAdj) { sh(a.token); shv(a.advTokens); }
  for (auto& x : f.secondary) shiftFrame(x, d);
}
void shiftNP(frame::SemNP& n, int d) {
  auto sh = [d](int& t) { if (t >= 0) t += d; };
  sh(n.token);
  for (int& t : n.tokens) sh(t);
  for (auto& p : n.possessor) shiftNP(p, d);
  for (auto& a : n.adjectives) { sh(a.token); for (int& t : a.advTokens) sh(t); }
  for (auto& g : n.genitive) shiftNP(g, d);
  for (auto& r : n.relative) shiftFrame(r, d);
  for (auto& k : n.coord) shiftNP(k, d);
}

// C18: "When X, Y." (a statement, not a question). The shared frame builder reads such a sentence as a wh question
// "when" and often takes the verb of X for a noun ("the bell rang" -> rang of bell) and loses Y's subject. X and Y are
// analysed apart (X alone: "the bell rang." is retagged by C17's english helpers) and merged: Y with X as a time
// clause before it. False when the sentence does not have that shape or a part does not give a clause.
bool frontedWhen(const std::string& text, const frame::FrameBuilder& fb, frame::SemSentence& out) {
  std::string low = text::lower(text);
  size_t lead = 0;
  while (lead < low.size() && (low[lead] == ' ' || low[lead] == '-' || low[lead] == '"')) ++lead;
  size_t kw = 0;
  if (low.compare(lead, 5, "when ") == 0) kw = 5;
  else if (low.compare(lead, 7, "cuando ") == 0) kw = 7;
  if (!kw) return false;
  size_t e = text.size();
  while (e > 0 && text[e - 1] == ' ') --e;
  if (e == 0 || text[e - 1] == '?') return false;
  const size_t comma = text.find(',', lead + kw);
  if (comma == std::string::npos || comma + 2 >= e) return false;
  const size_t aStart = lead + kw;
  std::string a = text.substr(aStart, comma - aStart);
  size_t bStart = comma + 1;
  while (bStart < text.size() && text[bStart] == ' ') ++bStart;
  std::string b = text.substr(bStart);
  if (a.empty() || b.empty()) return false;
  frame::SemSentence sa, sb;
  fb.analyse(a + ".", sa);
  fb.analyse(b, sb);
  if (sa.units.size() != 1 || sa.units[0].type != frame::Unit::Clause || !sa.units[0].frame.hasPred ||
      sa.units[0].frame.type != frame::Kind::Decl)
    return false;
  if (sb.units.empty() || sb.units[0].type != frame::Unit::Clause || !sb.units[0].frame.hasPred ||
      (sb.units[0].frame.type != frame::Kind::Decl && sb.units[0].frame.type != frame::Kind::Imp))
    return false;
  out = std::move(sb);
  const int base = (int)out.tokens.size();
  for (nlp::Token& t : out.tokens) { t.start += (int)bStart; t.end += (int)bStart; }
  for (nlp::Token t : sa.tokens) {
    if (t.start >= (int)a.size()) continue;   // the "." added for the analysis
    t.start += (int)aStart;
    t.end += (int)aStart;
    out.tokens.push_back(t);
  }
  for (size_t i = 0; i < sa.drop.size() && out.drop.size() < out.tokens.size(); ++i) out.drop.push_back(sa.drop[i]);
  while (out.drop.size() < out.tokens.size()) out.drop.push_back(frame::Drop::Punct);
  frame::SemFrame tf = sa.units[0].frame;
  shiftFrame(tf, base);
  tf.punct.clear();
  frame::SemSub ts;
  ts.relation = frame::Relation::Time;
  ts.marker = kw == 5 ? "when" : "cuando";
  ts.before = true;
  ts.frame.push_back(std::move(tf));
  out.units[0].frame.subordinate.insert(out.units[0].frame.subordinate.begin(), std::move(ts));
  out.text = text;
  for (const std::string& r : sa.repairs) out.repairs.push_back(r);
  for (const std::string& d : sa.doubts) out.doubt(d.c_str());
  return true;
}

// C21: two parts of a sentence the frame builder misreads, analysed apart and merged. text[a0, a1) is analysed as a
// sentence of its own (with a final "."), text[b0, end) as the rest; the clause of the part that is not `mainIsA`
// becomes a subordinate (relation, marker, before) of the other part's first clause. `mainSubject`: the main clause
// must be a statement with a subject. False when a part does not give a clause.
bool mergeParts(const std::string& text, const frame::FrameBuilder& fb, size_t a0, size_t a1, size_t b0, bool mainIsA,
                frame::Relation rel, const std::string& marker, bool before, bool mainSubject, frame::SemSentence& out) {
  if (a1 <= a0 || b0 >= text.size()) return false;
  const std::string a = text.substr(a0, a1 - a0), b = text.substr(b0);
  frame::SemSentence sa, sb;
  fb.analyse(a + ".", sa);
  fb.analyse(b, sb);
  auto oneClause = [](const frame::SemSentence& x) {
    return x.units.size() == 1 && x.units[0].type == frame::Unit::Clause && x.units[0].frame.hasPred;
  };
  if (!oneClause(sa) || sb.units.empty() || sb.units[0].type != frame::Unit::Clause || !sb.units[0].frame.hasPred)
    return false;
  // the artificial "." of part A
  if (!sa.tokens.empty() && sa.tokens.back().start >= (int)a.size()) {
    sa.tokens.pop_back();
    if (sa.drop.size() > sa.tokens.size()) sa.drop.resize(sa.tokens.size());
    for (frame::Unit& u : sa.units) u.last = std::min(u.last, (int)sa.tokens.size() - 1);
  }
  frame::SemSentence& mainS = mainIsA ? sa : sb;
  frame::SemSentence& subS = mainIsA ? sb : sa;
  const size_t mainOff = mainIsA ? a0 : b0, subOff = mainIsA ? b0 : a0;
  const frame::SemFrame& mf = mainS.units[0].frame;
  if (mf.type != frame::Kind::Decl || (mainSubject && !mf.hasSubject)) return false;
  const frame::SemFrame& sf0 = subS.units[0].frame;
  if (sf0.type == frame::Kind::Yn || sf0.type == frame::Kind::Wh) return false;
  while (mainS.drop.size() < mainS.tokens.size()) mainS.drop.push_back(frame::Drop::No);
  while (subS.drop.size() < subS.tokens.size()) subS.drop.push_back(frame::Drop::No);
  const std::string finalPunct = sb.finalPunct;
  out = std::move(mainS);
  for (nlp::Token& t : out.tokens) { t.start += (int)mainOff; t.end += (int)mainOff; }
  const int base = (int)out.tokens.size();
  for (size_t i = 0; i < subS.tokens.size(); ++i) {
    nlp::Token t = subS.tokens[i];
    t.start += (int)subOff;
    t.end += (int)subOff;
    out.tokens.push_back(t);
    out.drop.push_back(subS.drop[i]);
  }
  frame::SemFrame tf = subS.units[0].frame;
  shiftFrame(tf, base);
  tf.punct.clear();
  tf.type = frame::Kind::Decl;
  frame::SemSub ts;
  ts.relation = rel;
  ts.marker = marker;
  ts.before = before;
  ts.frame.push_back(std::move(tf));
  if (before) out.units[0].frame.subordinate.insert(out.units[0].frame.subordinate.begin(), std::move(ts));
  else out.units[0].frame.subordinate.push_back(std::move(ts));
  out.finalPunct = finalPunct;
  out.text = text;
  for (const std::string& r : subS.repairs) out.repairs.push_back(r);
  for (const std::string& d : subS.doubts) out.doubt(d.c_str());
  return true;
}

// C21: "Sitting under the tree, the girl sang." The frame builder reads the -ing word as an imperative and the
// clause after the comma as a broken noun phrase ("sang of girl"); C18's transfer repair needs that clause intact.
// Analysed as "-ing phrase" + main clause and merged (the transfer makes the participle). English only.
bool frontedIng(const std::string& text, const frame::FrameBuilder& fb, const frame::SemSentence& s,
                frame::SemSentence& out) {
  size_t lead = 0;
  while (lead < text.size() && (text[lead] == ' ' || text[lead] == '-' || text[lead] == '"')) ++lead;
  size_t w = lead;
  while (w < text.size() && ((text[w] >= 'a' && text[w] <= 'z') || (text[w] >= 'A' && text[w] <= 'Z'))) ++w;
  const std::string first = text::lower(text.substr(lead, w - lead));
  if (first.size() < 5 || first.compare(first.size() - 3, 3, "ing") != 0) return false;
  if (first == "nothing" || first == "something" || first == "anything" || first == "everything" || first == "morning" ||
      first == "evening" || first == "during") return false;
  size_t e = text.size();
  while (e > 0 && text[e - 1] == ' ') --e;
  if (e == 0 || text[e - 1] == '?') return false;
  const size_t comma = text.find(',', w);
  if (comma == std::string::npos || comma + 2 >= e) return false;
  // already readable: a statement whose verb comes after the comma, or C18's shape (the -ing clause without a subject
  // and a time / coordinated statement with a subject after it)
  for (const frame::Unit& u : s.units) {
    if (u.type != frame::Unit::Clause) continue;
    const frame::SemFrame& f = u.frame;
    if (f.type == frame::Kind::Decl && f.hasSubject && f.hasPred && f.pred.token >= 0 &&
        (size_t)f.pred.token < s.tokens.size() && s.tokens[(size_t)f.pred.token].start > (int)comma)
      return false;
    if (!f.subordinate.empty()) {
      const frame::SemSub& last = f.subordinate.back();
      if ((last.relation == frame::Relation::Time || last.relation == frame::Relation::Coord) && !last.frame.empty() &&
          last.frame[0].hasSubject && last.frame[0].hasPred && last.frame[0].type == frame::Kind::Decl)
        return false;
    }
    break;
  }
  size_t b0 = comma + 1;
  while (b0 < text.size() && text[b0] == ' ') ++b0;
  return mergeParts(text, fb, lead, comma, b0, false, frame::Relation::Time, "", true, true, out);
}

// C21: "The wind was so strong that the tree fell." / "El viento era tan fuerte que el árbol cayó." with the result
// clause broken by the parser ("that fell of tree"): "X so ADJ" and "Y" analysed apart, Y the "that" clause of X (the
// transfer reads so + that as ὥστε). Only when no clause already has a that / que clause.
bool soThat(const std::string& text, const frame::FrameBuilder& fb, const frame::SemSentence& s, bool es,
            frame::SemSentence& out) {
  for (const frame::Unit& u : s.units)
    if (u.type == frame::Unit::Clause)
      for (const frame::SemSub& sb : u.frame.subordinate)
        if (!sb.frame.empty() && sb.frame[0].hasPred) return false;
  const std::string low = text::lower(text);
  auto word = [&](const std::string& wd, size_t from) {
    for (size_t p = low.find(wd, from); p != std::string::npos; p = low.find(wd, p + 1)) {
      const bool l = p == 0 || !std::isalpha((unsigned char)low[p - 1]);
      const bool r = p + wd.size() >= low.size() || !std::isalpha((unsigned char)low[p + wd.size()]);
      if (l && r) return p;
    }
    return std::string::npos;
  };
  size_t deg = std::string::npos;
  static const char* const kEs[] = {"tan", "tanto", "tanta", "tantos", "tantas", nullptr};
  static const char* const kEn[] = {"so", "such", nullptr};
  for (const char* const* d = es ? kEs : kEn; *d; ++d) {
    const size_t p = word(*d, 0);
    if (p != std::string::npos && (deg == std::string::npos || p < deg)) deg = p;
  }
  if (deg == std::string::npos || deg == 0) return false;
  const std::string conj = es ? "que" : "that";
  const size_t th = word(conj, deg + 2);
  if (th == std::string::npos) return false;
  size_t a1 = th;
  while (a1 > 0 && (text[a1 - 1] == ' ' || text[a1 - 1] == ',')) --a1;
  size_t b0 = th + conj.size();
  while (b0 < text.size() && text[b0] == ' ') ++b0;
  // part A must end right after the degree group (so + one or two words): "so strong", "so very tired"
  size_t words = 0;
  for (size_t i = deg; i < a1; ++i)
    if (text[i] == ' ') ++words;
  if (words < 1 || words > 3) return false;
  return mergeParts(text, fb, 0, a1, b0, true, frame::Relation::Complement, conj, false, true, out);
}

// C25: a question the parser breaks: "Why did the dog run away?" read as a fragment "why", "How many sheep does the
// farmer have?" with "sheep" as the subject and the farmer lost, "Did the dog bark?" with "do" as the main verb and
// "bark of dog" as its object. The part from the auxiliary on is analysed as a yes / no question ("Did the dog run
// away?", "Does the farmer have?"), or, after do / does / did, the statement without the auxiliary ("The dog bark.",
// the tense from the auxiliary), and the wh word (or the "how many" NP, as the object) put back. English only;
// false when the sentence does not have that shape.
bool whAuxiliary(const std::string& text, const frame::FrameBuilder& fb, const frame::SemSentence& s,
                 frame::SemSentence& out) {
  if (s.units.size() != 1 || s.units[0].type != frame::Unit::Clause || s.tokens.size() < 3) return false;
  const frame::SemFrame& f0 = s.units[0].frame;
  static const char* const kAux[] = {"did", "do", "does", "is", "are", "was", "were", "can", "could", "will", "would",
                                     "should", nullptr};
  auto aux = [&](size_t k) {
    for (const char* const* a = kAux; *a; ++a) if (s.tokens[k].lower == *a) return true;
    return false;
  };
  auto doAux = [&](size_t k) { return s.tokens[k].lower == "do" || s.tokens[k].lower == "does" || s.tokens[k].lower == "did"; };
  // C31: "When will the war end?" / "Will the rain stop?" (the verb read as a noun: "the end of the war"): with will /
  // can the statement without the auxiliary is analysed as with do, the tense or the modal taken from the auxiliary
  auto modalAux = [&](size_t k) { return s.tokens[k].lower == "will" || s.tokens[k].lower == "can"; };
  const std::string w0 = s.tokens[0].lower;
  size_t k = 0;
  enum { WhAdverb, HowMany, YesNo } mode;
  if ((w0 == "why" || w0 == "where" || w0 == "when" || w0 == "how") && aux(1) &&
      (f0.type == frame::Kind::Frag || f0.type == frame::Kind::Wh) &&
      (!f0.hasPred || (doAux(1) && text::lower(f0.pred.lemma) == "do" && f0.hasObject && !f0.hasSubject))) {
    k = 1;
    mode = WhAdverb;
  } else if (w0 == "how" && s.tokens[1].lower == "many" && f0.type == frame::Kind::Wh && f0.hasSubject &&
             f0.subject.interrogative && !f0.hasObject) {
    int last = f0.subject.token;
    for (int t : f0.subject.tokens) last = std::max(last, t);
    if (last < 0 || (size_t)last + 1 >= s.tokens.size() || !aux((size_t)last + 1)) return false;
    // the auxiliary must be followed by a subject of its own (not "How many dogs are there?")
    if ((size_t)last + 2 >= s.tokens.size() || s.tokens[(size_t)last + 2].lower == "there") return false;
    k = (size_t)last + 1;
    mode = HowMany;
  } else if (doAux(0) && f0.type == frame::Kind::Yn && text::lower(f0.pred.lemma) == "do" && f0.hasObject &&
             !f0.hasSubject) {
    k = 0;
    mode = YesNo;
  } else if (modalAux(0) && !f0.hasPred && (f0.type == frame::Kind::Frag || f0.type == frame::Kind::Yn) &&
             s.tokens.size() >= 4) {   // C31
    k = 0;
    mode = YesNo;
  } else {
    return false;
  }
  auto cap = [](std::string x) {
    if (!x.empty() && x[0] >= 'a' && x[0] <= 'z') x[0] = (char)(x[0] - 'a' + 'A');
    return x;
  };
  frame::SemSentence sb;
  size_t off = (size_t)s.tokens[k].start, keep = k;
  bool fromStatement = false;
  if (mode != YesNo) {
    fb.analyse(cap(text.substr(off)), sb);
    const bool good = sb.units.size() == 1 && sb.units[0].type == frame::Unit::Clause && sb.units[0].frame.hasPred &&
                      sb.units[0].frame.type == frame::Kind::Yn && sb.units[0].frame.hasSubject &&
                      !(doAux(k) && text::lower(sb.units[0].frame.pred.lemma) == "do");
    if (!good) sb = frame::SemSentence{};
  }
  if (sb.units.empty() && (doAux(k) || modalAux(k)) && k + 1 < s.tokens.size()) {
    // the statement without the auxiliary, its bare verb in the past only so that the tagger reads it as a verb
    // ("Did the dog bark?" -> "The dog barked.", "Why do the dogs bark?" -> "The dogs barked."); the tense comes
    // from the auxiliary. Each word after the first is tried in turn (C11's English inflection, la2x/internal.h).
    off = (size_t)s.tokens[k + 1].start;
    std::string st = text.substr(off);
    while (!st.empty() && (st.back() == '?' || st.back() == ' ')) st.pop_back();
    bool found = false;
    for (size_t j = k + 2; j < s.tokens.size() && !found; ++j) {
      const nlp::Token& vt = s.tokens[j];
      if (vt.upos == "PUNCT" || vt.start < (int)off || (size_t)vt.end > off + st.size()) continue;
      const std::string base = vt.lower;
      const std::string past = la2x::detail::en::verb(base, la2x::detail::en::VForm::Past);
      if (past.empty() || past == base || past.find(' ') != std::string::npos) continue;
      const size_t a = (size_t)vt.start - off, b = (size_t)vt.end - off;
      const std::string trial = cap(st.substr(0, a) + past + st.substr(b));
      frame::SemSentence tr;
      fb.analyse(trial + ".", tr);
      if (tr.units.size() != 1 || tr.units[0].type != frame::Unit::Clause || !tr.units[0].frame.hasPred ||
          tr.units[0].frame.type != frame::Kind::Decl || !tr.units[0].frame.hasSubject ||
          text::lower(tr.units[0].frame.pred.lemma) != base)
        continue;
      if (!tr.tokens.empty() && tr.tokens.back().start >= (int)trial.size()) tr.tokens.pop_back();   // the added "."
      const int delta = (int)past.size() - (int)(b - a);
      for (nlp::Token& t : tr.tokens) {
        if (t.start == (int)a) { t.end = (int)b; t.text = vt.text; t.lower = vt.lower; }
        else if (t.start > (int)a) { t.start -= delta; t.end -= delta; }
      }
      sb = std::move(tr);
      found = true;
    }
    if (!found) return false;
    keep = k + 1;
    fromStatement = true;
  }
  if (sb.units.empty()) return false;
  if (mode == HowMany && sb.units[0].frame.hasObject) return false;
  out = s;
  out.tokens.resize(keep);
  out.drop.resize(std::min(out.drop.size(), keep));
  while (out.drop.size() < keep) out.drop.push_back(frame::Drop::No);
  for (size_t i = 0; i < sb.tokens.size(); ++i) {
    nlp::Token t = sb.tokens[i];
    t.start += (int)off;
    t.end += (int)off;
    out.tokens.push_back(t);
    out.drop.push_back(i < sb.drop.size() ? sb.drop[i] : frame::Drop::No);
  }
  frame::SemFrame nf = sb.units[0].frame;
  shiftFrame(nf, (int)keep);
  for (int t = 0; t < (int)keep; ++t) nf.tokens.insert(nf.tokens.begin() + t, t);
  if (fromStatement) {
    nf.pred.auxTokens.push_back((int)k);
    nf.pred.tense = s.tokens[k].lower == "did" ? frame::Tense::Past
                    : s.tokens[k].lower == "will" ? frame::Tense::Future : frame::Tense::Present;   // C31: will
    if (s.tokens[k].lower == "can") nf.pred.modality = frame::Modality::Can;
    nf.pred.aspect = frame::Aspect::Simple;
  }
  nf.type = mode == YesNo ? frame::Kind::Yn : frame::Kind::Wh;
  if (mode == HowMany) {
    nf.hasObject = true;
    nf.object = f0.subject;
    nf.wh = f0.wh;
    nf.wh.role = frame::Role::Object;
  } else if (mode == WhAdverb) {
    nf.wh = frame::SemWh{};
    nf.wh.word = w0;
    nf.wh.role = frame::Role::Adverb;
    nf.wh.token = 0;
  }
  out.units[0].frame = std::move(nf);
  out.units[0].last = (int)out.tokens.size() - 1;
  out.finalPunct = "?";
  for (const std::string& r : sb.repairs) out.repairs.push_back(r);
  for (const std::string& d : sb.doubts) out.doubt(d.c_str());
  return true;
}

// C25: "My friends and I built a boat.": the address row "my friend" (phrasebook_en_grc.tsv, register voc) matched
// before "and" (a conjunction counts as a phrase boundary in the shared frame builder) and took the subject. The
// sentence is analysed again with "friend(s)" spelled as "father(s)" (same length: offsets kept), which no row
// matches, and the NP head set back to "friend". False when the sentence does not have that shape.
void renameHead(frame::SemNP& n, int tok, const std::string& from, const std::string& to);
void renameHeadF(frame::SemFrame& f, int tok, const std::string& from, const std::string& to) {
  if (f.hasSubject) renameHead(f.subject, tok, from, to);
  if (f.hasObject) renameHead(f.object, tok, from, to);
  if (f.hasIndirect) renameHead(f.indirectObject, tok, from, to);
  for (auto& o : f.obliques) renameHead(o.np, tok, from, to);
  for (auto& n : f.predicative) renameHead(n, tok, from, to);
  for (auto& sb : f.subordinate) for (auto& x : sb.frame) renameHeadF(x, tok, from, to);
}
void renameHead(frame::SemNP& n, int tok, const std::string& from, const std::string& to) {
  if (n.token == tok && text::lower(n.head) == from) { n.head = to; n.surface = to; }
  for (auto& k : n.coord) renameHead(k, tok, from, to);
  for (auto& g : n.genitive) renameHead(g, tok, from, to);
  for (auto& r : n.relative) renameHeadF(r, tok, from, to);
}
void renameNounBack(frame::SemSentence& s, int tok, const std::string& from, const std::string& to) {
  for (frame::Unit& u : s.units)
    if (u.type == frame::Unit::Clause) renameHeadF(u.frame, tok, from, to);
  if (tok >= 0 && (size_t)tok < s.tokens.size()) { s.tokens[(size_t)tok].lemma = to; s.tokens[(size_t)tok].upos = "NOUN"; }
}

bool vocCoordinated(const std::string& text, const frame::FrameBuilder& fb, const frame::SemSentence& s,
                    frame::SemSentence& out) {
  for (size_t ui = 0; ui + 1 < s.units.size(); ++ui) {
    const frame::Unit& u = s.units[ui];
    if (u.type != frame::Unit::Phrase || u.phrase.reg != "voc" || !u.sepAfter.empty()) continue;
    const frame::Unit& nx = s.units[ui + 1];
    bool conj = false;
    if (nx.type == frame::Unit::Clause)
      for (const std::string& k : nx.frame.connectors) conj = conj || text::lower(k) == "and" || text::lower(k) == "or";
    if (!conj || u.last < 0 || (size_t)u.last >= s.tokens.size()) continue;
    const nlp::Token& t = s.tokens[(size_t)u.last];
    const char* sub = t.lower == "friend" ? "father" : t.lower == "friends" ? "fathers" : nullptr;
    if (!sub) continue;
    std::string alt = text;
    for (size_t i = 0; sub[i]; ++i) alt[(size_t)t.start + i] = (t.text[0] == 'F' && i == 0) ? 'F' : sub[i];
    fb.analyse(alt, out);
    bool phrase = false;
    for (const frame::Unit& v : out.units) phrase = phrase || v.type == frame::Unit::Phrase;
    if (phrase || out.units.empty() || out.tokens.size() != s.tokens.size()) return false;
    for (frame::Unit& v : out.units)
      if (v.type == frame::Unit::Clause) renameHeadF(v.frame, u.last, "father", "friend");
    for (size_t i = 0; i < out.tokens.size(); ++i) {
      out.tokens[i].text = s.tokens[i].text;
      out.tokens[i].lower = s.tokens[i].lower;
    }
    out.text = text;
    return true;
  }
  return false;
}

// C25: Spanish "El niño nada en el río." read as a fragment "the boy of nothing in the river" ("nada" = nothing; the
// Greek had no verb and was rated OK): with no verb in the sentence, a "nada" right after the subject NP is the verb
// nadar. The sentence is analysed again with "come" (the same length) in its place and the verb set back to nadar.
bool nadaVerb(const std::string& text, const frame::FrameBuilder& fb, const frame::SemSentence& s,
              frame::SemSentence& out) {
  if (s.units.size() != 1 || s.units[0].type != frame::Unit::Clause) return false;
  const frame::SemFrame& f0 = s.units[0].frame;
  if (f0.hasPred || !f0.hasSubject || f0.subject.isPronoun) return false;
  size_t k = s.tokens.size();
  for (size_t i = 1; i < s.tokens.size(); ++i)
    if (s.tokens[i].lower == "nada" && s.tokens[i - 1].upos != "VERB" && s.tokens[i - 1].upos != "AUX") { k = i; break; }
  if (k == s.tokens.size()) return false;
  for (const nlp::Token& t : s.tokens) if (t.upos == "VERB" || t.upos == "AUX") return false;
  std::string alt = text;
  const char* rep = "come";
  for (size_t i = 0; i < 4; ++i) alt[(size_t)s.tokens[k].start + i] = rep[i];
  fb.analyse(alt, out);
  if (out.units.size() != 1 || out.units[0].type != frame::Unit::Clause || !out.units[0].frame.hasPred ||
      out.units[0].frame.pred.lemma != "comer" || out.units[0].frame.pred.token != (int)k || out.tokens.size() != s.tokens.size())
    return false;
  out.units[0].frame.pred.lemma = "nadar";
  for (size_t i = 0; i < out.tokens.size(); ++i) {
    out.tokens[i].text = s.tokens[i].text;
    out.tokens[i].lower = s.tokens[i].lower;
  }
  out.text = text;
  return true;
}

// C25: Spanish "Mi madre está en casa." / "Los niños están en casa." read as a fragment (the subject NP alone, the
// verb and the rest lost): the part from the copula on is analysed alone ("Está en casa.", the subject implicit) and
// the fragment's NP becomes its subject. False when the sentence does not have that shape.
bool estarFragment(const std::string& text, const frame::FrameBuilder& fb, const frame::SemSentence& s,
                   frame::SemSentence& out) {
  if (s.units.size() != 1 || s.units[0].type != frame::Unit::Clause) return false;
  const frame::SemFrame& f0 = s.units[0].frame;
  if (f0.type != frame::Kind::Frag || f0.hasPred || !f0.hasSubject || f0.subject.isPronoun) return false;
  int last = f0.subject.token;
  for (int t : f0.subject.tokens) last = std::max(last, t);
  const size_t k = (size_t)last + 1;
  if (last < 0 || k >= s.tokens.size()) return false;
  static const char* const kCop[] = {"está", "están", "estaba", "estaban", "estuvo", "estuvieron", "estará", "estarán",
                                     "es", "son", "era", "eran", "fue", "fueron", nullptr};
  bool cop = false;
  for (const char* const* c = kCop; *c; ++c) cop = cop || s.tokens[k].lower == *c;
  if (!cop) return false;
  const size_t off = (size_t)s.tokens[k].start;
  std::string part = text.substr(off);
  if (!part.empty() && part[0] >= 'a' && part[0] <= 'z') part[0] = (char)(part[0] - 'a' + 'A');
  else if (part.size() > 1 && (unsigned char)part[0] == 0xC3 && (unsigned char)part[1] >= 0xA0 && (unsigned char)part[1] <= 0xBF)
    part[1] = (char)((unsigned char)part[1] - 0x20);   // é -> É ...
  frame::SemSentence sb;
  fb.analyse(part, sb);
  if (sb.units.size() != 1 || sb.units[0].type != frame::Unit::Clause || !sb.units[0].frame.hasPred ||
      sb.units[0].frame.type != frame::Kind::Decl)
    return false;
  out = s;
  out.tokens.resize(k);
  out.drop.resize(std::min(out.drop.size(), k));
  while (out.drop.size() < k) out.drop.push_back(frame::Drop::No);
  for (size_t i = 0; i < sb.tokens.size(); ++i) {
    nlp::Token t = sb.tokens[i];
    t.start += (int)off;
    t.end += (int)off;
    out.tokens.push_back(t);
    out.drop.push_back(i < sb.drop.size() ? sb.drop[i] : frame::Drop::No);
  }
  frame::SemFrame nf = sb.units[0].frame;
  shiftFrame(nf, (int)k);
  nf.hasSubject = true;
  nf.implicitSubject = false;
  nf.subject = f0.subject;
  for (int t = 0; t < (int)k; ++t) nf.tokens.insert(nf.tokens.begin() + t, t);
  out.units[0].frame = std::move(nf);
  out.units[0].last = (int)out.tokens.size() - 1;
  out.finalPunct = sb.finalPunct;
  for (const std::string& r : sb.repairs) out.repairs.push_back(r);
  for (const std::string& d : sb.doubts) out.doubt(d.c_str());
  return true;
}

// ---- C29: re-analyses of the shapes the shared frame builder misreads (Greek side only) ----------------------------

// The sentence analysed again with the word of token `tok` replaced by `standIn`, the original spelling and offsets put
// back on the tokens (the later offsets shifted back by the difference in length). False when the token counts differ.
bool substituteWords(const std::string& text, const frame::FrameBuilder& fb, const frame::SemSentence& s,
                     std::vector<std::pair<size_t, std::string>> subs, frame::SemSentence& out) {
  std::sort(subs.begin(), subs.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
  std::string alt = text;
  for (const auto& sb : subs) {   // from the last word back, so the earlier offsets stay valid
    if (sb.first >= s.tokens.size() || sb.second.empty()) return false;
    const nlp::Token& t = s.tokens[sb.first];
    if (t.start < 0 || t.end < t.start || (size_t)t.end > text.size()) return false;
    std::string w = sb.second;
    if (t.text[0] >= 'A' && t.text[0] <= 'Z' && w[0] >= 'a' && w[0] <= 'z') w[0] = (char)(w[0] - 'a' + 'A');
    alt = alt.substr(0, (size_t)t.start) + w + alt.substr((size_t)t.end);
  }
  fb.analyse(alt, out);
  if (out.tokens.size() != s.tokens.size()) return false;
  for (size_t i = 0; i < out.tokens.size(); ++i) {
    out.tokens[i].text = s.tokens[i].text;
    out.tokens[i].lower = s.tokens[i].lower;
    out.tokens[i].start = s.tokens[i].start;
    out.tokens[i].end = s.tokens[i].end;
  }
  out.text = text;
  return true;
}
bool substituteWord(const std::string& text, const frame::FrameBuilder& fb, const frame::SemSentence& s, size_t tok,
                    const std::string& standIn, frame::SemSentence& out) {
  return substituteWords(text, fb, s, {{tok, standIn}}, out);
}

// C29: a common noun after a determiner that the tagger reads as an adjective before a verb ("The soldier/ADJ
// sang/NOUN.": the clause loses its subject and its verb): english.vpl knows the word as a noun and not as an
// adjective. Its token index (and the noun lemma) or false.
bool nounTaggedAdj(const frame::SemSentence& s, const lex::Lexicon& en, size_t k, std::string& lemma, bool& plural) {
  if (k == 0 || k + 1 >= s.tokens.size()) return false;
  const nlp::Token& t = s.tokens[k];
  if (t.upos != "ADJ" || s.tokens[k - 1].upos != "DET" || s.tokens[k + 1].upos == "PUNCT") return false;
  std::vector<lex::Analysis> an;
  en.lookup(text::en_key(t.lower), an);
  bool noun = false, adj = false;
  for (const lex::Analysis& a : an) {
    const lex::Lemma l = en.lemma(a.lemma);
    if (l.pos == feat::Adj || l.pos == feat::Participle) adj = true;
    if (l.pos == feat::Noun && !noun) {
      noun = true;
      lemma = text::lower(std::string(l.head));
      plural = feat::unpack(en.feature(a.feat)).number == feat::Pl;
    }
  }
  return noun && !adj;
}

// C29: "The soldier sang." / "The soldier came because his horse died.": the subject noun read as an adjective and the
// verb as a noun ("[sang]", Fix on both paths). The noun is analysed as "boy(s)" and set back.
bool nounAsAdj(const std::string& text, const frame::FrameBuilder& fb, const frame::SemSentence& s, const lex::Lexicon* en,
               frame::SemSentence& out, std::string& noun) {
  if (!en) return false;
  for (size_t k = 1; k + 1 < s.tokens.size(); ++k) {
    std::string nl;
    bool npl = false;
    if (!nounTaggedAdj(s, *en, k, nl, npl)) continue;
    // only when the analysis lost the noun as a subject (no clause has its subject there)
    bool subj = false;
    std::function<void(const frame::SemFrame&)> look = [&](const frame::SemFrame& f) {
      subj = subj || (f.hasSubject && f.subject.token == (int)k);
      for (const auto& sb : f.subordinate) for (const auto& x : sb.frame) look(x);
    };
    for (const frame::Unit& u : s.units) if (u.type == frame::Unit::Clause) look(u.frame);
    if (subj) continue;
    frame::SemSentence alt;
    if (!substituteWord(text, fb, s, k, npl ? "boys" : "boy", alt)) continue;
    bool pred = false;
    for (const frame::Unit& u : alt.units) pred = pred || (u.type == frame::Unit::Clause && u.frame.hasPred);
    if (!pred) continue;
    renameNounBack(alt, (int)k, "boy", nl);
    out = std::move(alt);
    noun = s.tokens[k].lower;
    return true;
  }
  return false;
}


// ---- C31: modifiers the parser hung on the verb ("The old king died.") -------------------------------------------
// The tokens a frame uses (every NP, adjective, adverb, predicate and wh token), recursively.
void usedNP(const frame::SemNP& n, std::vector<int>& u);
void usedFrame(const frame::SemFrame& f, std::vector<int>& u) {
  auto add = [&u](int t) { if (t >= 0) u.push_back(t); };
  add(f.pred.token);
  add(f.pred.complementToken);
  for (int t : f.pred.auxTokens) add(t);
  if (f.hasSubject) usedNP(f.subject, u);
  if (f.hasObject) usedNP(f.object, u);
  if (f.hasIndirect) usedNP(f.indirectObject, u);
  for (const auto& o : f.obliques) { add(o.token); usedNP(o.np, u); }
  for (const auto& n : f.predicative) usedNP(n, u);
  for (const auto& a : f.predAdj) { add(a.token); for (int t : a.advTokens) add(t); }
  for (const auto& n : f.vocatives) usedNP(n, u);
  for (const auto& a : f.adverbs) add(a.token);
  for (const auto& sb : f.subordinate) for (const auto& x : sb.frame) usedFrame(x, u);
  add(f.wh.token);
  for (const auto& n : f.objComplement) usedNP(n, u);
  for (const auto& a : f.objComplementAdj) { add(a.token); for (int t : a.advTokens) add(t); }
  for (const auto& x : f.secondary) usedFrame(x, u);
}
void usedNP(const frame::SemNP& n, std::vector<int>& u) {
  if (n.token >= 0) u.push_back(n.token);
  for (int t : n.tokens) if (t >= 0) u.push_back(t);
  for (const auto& p : n.possessor) usedNP(p, u);
  for (const auto& a : n.adjectives) {
    if (a.token >= 0) u.push_back(a.token);
    for (int t : a.advTokens) if (t >= 0) u.push_back(t);
  }
  for (const auto& g : n.genitive) usedNP(g, u);
  for (const auto& r : n.relative) usedFrame(r, u);
  for (const auto& k : n.coord) usedNP(k, u);
}

// The adjectives and the article right before an NP's head that no part of the analysis uses (the parser hung them on
// the verb: "The/det>died old/amod>died king died.") are given back to the NP. Also a clause without a subject whose
// noun before the verb was made a compound of it ("The old king died yesterday." lost the king). English only; true
// when something was given back (the analysis is then a repair: Check).
bool lostModifiers(frame::SemSentence& s) {
  if (s.lang != frame::SrcLang::En) return false;
  std::vector<int> used;
  for (const frame::Unit& u : s.units) if (u.type == frame::Unit::Clause) usedFrame(u.frame, used);
  auto isUsed = [&](int t) { return std::find(used.begin(), used.end(), t) != used.end(); };
  bool changed = false;
  auto fixNP = [&](frame::SemNP& n) {
    if (n.isPronoun || n.isName || n.token <= 0 || (size_t)n.token >= s.tokens.size()) return;
    if (s.tokens[(size_t)n.token].upos != "NOUN") return;
    for (int j = n.token - 1; j >= 0; --j) {
      const nlp::Token& t = s.tokens[(size_t)j];
      if (isUsed(j) || t.head - 1 == n.token) break;   // the parser's own NP, or a word in use elsewhere
      if (t.upos == "ADJ") {
        frame::SemAdj a;
        a.lemma = t.lemma.empty() ? t.lower : t.lemma;
        a.token = j;
        n.adjectives.insert(n.adjectives.begin(), a);
      } else if (t.upos == "DET" && (t.lower == "the" || t.lower == "a" || t.lower == "an")) {
        if (n.determiner.empty()) n.determiner = t.lower == "the" ? "the" : "a";
        n.definite = t.lower == "the";
      } else {
        break;
      }
      n.tokens.push_back(j);
      used.push_back(j);
      changed = true;
      if (t.upos == "DET") break;
    }
  };
  std::function<void(frame::SemFrame&)> visit = [&](frame::SemFrame& f) {
    // a subject noun made a compound of the verb
    if (f.hasPred && !f.hasSubject && f.type == frame::Kind::Decl && f.pred.token > 0) {
      for (int j = f.pred.token - 1; j >= 0; --j) {
        const nlp::Token& t = s.tokens[(size_t)j];
        if (t.upos != "NOUN" || isUsed(j) || t.head - 1 != f.pred.token || t.deprel != "compound")
          continue;
        frame::SemNP n;
        n.head = t.lemma.empty() ? t.lower : t.lemma;
        n.surface = t.text;
        n.token = j;
        n.tokens = {j};
        n.number = nlp::morph::get(t.feats, nlp::morph::NumberShift) == nlp::morph::NumPlur ? 2 : 1;
        f.hasSubject = true;
        f.subject = n;
        f.implicitSubject = false;
        used.push_back(j);
        changed = true;
        break;
      }
    }
    if (f.hasSubject) fixNP(f.subject);
    if (f.hasObject) fixNP(f.object);
    if (f.hasIndirect) fixNP(f.indirectObject);
    for (auto& o : f.obliques) fixNP(o.np);
    for (auto& sb : f.subordinate) for (auto& x : sb.frame) visit(x);
  };
  for (frame::Unit& u : s.units) if (u.type == frame::Unit::Clause) visit(u.frame);
  if (changed)
    for (size_t i = 0; i < s.drop.size() && i < s.tokens.size(); ++i)
      if (s.drop[i] != frame::Drop::No && std::find(used.begin(), used.end(), (int)i) != used.end() &&
          s.tokens[i].upos == "ADJ")
        s.drop[i] = frame::Drop::No;
  return changed;
}

// C31: Spanish "Papá está en el jardín." / "Mamá está cansada.": a sentence-initial papá / mamá the tagger reads as a
// verb (papar, mamar: "Ἴσθι ἐν τῷ κήπῳ [papá]", Fix). The rest from the verb on is analysed alone and the word becomes
// its subject (ὁ πατήρ). False when the sentence does not have that shape.
bool familyFirst(const std::string& text, const frame::FrameBuilder& fb, const frame::SemSentence& s,
                 frame::SemSentence& out) {
  if (s.lang != frame::SrcLang::Es || s.tokens.size() < 3 || s.units.empty()) return false;
  const nlp::Token& t0 = s.tokens[0];
  if (!(t0.lower == "papá" || t0.lower == "mamá") || t0.upos != "VERB") return false;
  const nlp::Token& t1 = s.tokens[1];
  if (t1.upos != "VERB" && t1.upos != "AUX") return false;
  const size_t off = (size_t)t1.start;
  std::string part = text.substr(off);
  if (!part.empty() && part[0] >= 'a' && part[0] <= 'z') part[0] = (char)(part[0] - 'a' + 'A');
  else if (part.size() > 1 && (unsigned char)part[0] == 0xC3 && (unsigned char)part[1] >= 0xA0 && (unsigned char)part[1] <= 0xBF)
    part[1] = (char)((unsigned char)part[1] - 0x20);
  frame::SemSentence sb;
  fb.analyse(part, sb);
  if (sb.units.size() != 1 || sb.units[0].type != frame::Unit::Clause || !sb.units[0].frame.hasPred ||
      sb.units[0].frame.type != frame::Kind::Decl)
    return false;
  const frame::SemFrame& g = sb.units[0].frame;
  if (g.hasSubject && !(g.subject.isPronoun && g.subject.token < 0 && g.subject.pron.person == 3)) return false;
  out = s;
  out.tokens.resize(1);
  out.tokens[0].upos = "NOUN";
  out.tokens[0].lemma = t0.lower;
  out.drop.assign(1, frame::Drop::No);
  for (size_t i = 0; i < sb.tokens.size(); ++i) {
    nlp::Token t = sb.tokens[i];
    t.start += (int)off;
    t.end += (int)off;
    out.tokens.push_back(t);
    out.drop.push_back(i < sb.drop.size() ? sb.drop[i] : frame::Drop::No);
  }
  frame::SemFrame nf = g;
  shiftFrame(nf, 1);
  frame::SemNP n;
  n.head = t0.lower;
  n.surface = t0.text;
  n.token = 0;
  n.tokens = {0};
  n.number = 1;
  n.definite = true;
  n.srcGender = t0.lower == "mamá" ? feat::F : feat::M;
  nf.hasSubject = true;
  nf.implicitSubject = false;
  nf.subject = n;
  nf.tokens.insert(nf.tokens.begin(), 0);
  out.units.resize(1);
  out.units[0] = sb.units[0];
  out.units[0].frame = std::move(nf);
  out.units[0].first = 0;
  out.units[0].last = (int)out.tokens.size() - 1;
  out.finalPunct = sb.finalPunct;
  for (const std::string& r : sb.repairs) out.repairs.push_back(r);
  for (const std::string& d : sb.doubts) out.doubt(d.c_str());
  return true;
}

// C31: Spanish clauses joined by a comma without a conjunction ("¡No corras, te vas a caer!", "¡Cierra la puerta, hace
// frío!", "Tengo sed, ¿me das agua?"): the parser hangs one on the other as an adverbial clause and the frame builder
// makes it a time clause ("ὅτε οὐ τρέχεις, πεσεῖ", rated OK). A time clause with no subordinating word in or right
// before it and a comma between it and its main clause is coordinated instead: the first clause is the main one and
// the second follows it (a statement after a command takes γάρ in the transfer); a question after "¿" becomes a unit
// of its own. True when something was changed.
bool esParataxis(frame::SemSentence& s) {
  if (s.lang != frame::SrcLang::Es) return false;
  static const char* const kMark[] = {"cuando", "mientras", "si", "después", "antes", "hasta", "apenas", "desde",
                                      "que", "porque", "aunque", "como", "donde", "mientras", "luego", nullptr};
  bool changed = false;
  for (size_t ui = 0; ui < s.units.size(); ++ui) {
    if (s.units[ui].type != frame::Unit::Clause || s.units[ui].vocative) continue;
    frame::SemFrame& f = s.units[ui].frame;
    for (size_t si = 0; si < f.subordinate.size(); ++si) {
      const frame::SemSub& sb = f.subordinate[si];
      if (sb.relation != frame::Relation::Time || sb.frame.empty() || !sb.frame[0].hasPred || !f.hasPred) continue;
      std::vector<int> toks;
      usedFrame(sb.frame[0], toks);
      if (toks.empty()) continue;
      const int lo = *std::min_element(toks.begin(), toks.end()), hi = *std::max_element(toks.begin(), toks.end());
      bool marked = false;
      for (int t = std::max(0, lo - 2); t <= hi && (size_t)t < s.tokens.size(); ++t)
        for (const char* const* m = kMark; *m; ++m) marked = marked || s.tokens[(size_t)t].lower == *m;
      if (marked) continue;
      // a comma between the two clauses
      const int mp = f.pred.token;
      const int a = sb.before ? hi : mp, b = sb.before ? mp : lo;
      int comma = -1;
      for (int t = a + 1; t < b && (size_t)t < s.tokens.size(); ++t)
        if (s.tokens[(size_t)t].text == ",") comma = t;
      if (comma < 0) continue;
      frame::SemFrame subF = sb.frame[0];
      frame::SemFrame mainF = f;
      mainF.subordinate.erase(mainF.subordinate.begin() + (long)si);
      frame::SemFrame& first = sb.before ? subF : mainF;
      frame::SemFrame& second = sb.before ? mainF : subF;
      // "¡No corras, ...!": "no" + a 2nd-person present subjunctive is a prohibition, not a statement
      if (first.type == frame::Kind::Decl && first.negative && first.pred.token >= 0 &&
          (size_t)first.pred.token < s.tokens.size()) {
        const uint32_t ft = s.tokens[(size_t)first.pred.token].feats;
        if (nlp::morph::get(ft, nlp::morph::MoodShift) == nlp::morph::MoodSub &&
            nlp::morph::get(ft, nlp::morph::PersonShift) == nlp::morph::Pers2) {
          first.type = frame::Kind::Imp;
          first.hasSubject = false;
          first.imperativePlural = nlp::morph::get(ft, nlp::morph::NumberShift) == nlp::morph::NumPlur;
        }
      }
      // "¿" after the comma: the second clause alone is the question
      int inv = -1;
      for (size_t t = 0; t < s.tokens.size(); ++t)
        if (s.tokens[t].text == "\xC2\xBF") inv = (int)t;
      if (inv > comma) {
        if (first.type == frame::Kind::Yn) first.type = frame::Kind::Decl;
        if (second.type == frame::Kind::Decl) second.type = frame::Kind::Yn;
        frame::Unit u1 = s.units[ui], u2 = s.units[ui];
        u1.frame = first;
        u2.frame = second;
        u1.last = comma - 1;
        u1.sepAfter = ",";
        u2.first = comma + 1;
        s.units[ui] = u1;
        s.units.insert(s.units.begin() + (long)ui + 1, u2);
      } else {
        frame::SemSub co;
        co.relation = frame::Relation::Coord;
        co.before = false;
        co.frame.push_back(second);
        first.subordinate.push_back(co);
        s.units[ui].frame = first;
      }
      changed = true;
      break;
    }
  }
  return changed;
}

// The frame (of any unit, subordinate or secondary clause) whose predicate is token `tok`; null when none.
frame::SemFrame* predAt(frame::SemFrame& f, int tok) {
  if (f.hasPred && f.pred.token == tok) return &f;
  for (auto& sb : f.subordinate)
    for (auto& x : sb.frame)
      if (frame::SemFrame* r = predAt(x, tok)) return r;
  for (auto& x : f.secondary)
    if (frame::SemFrame* r = predAt(x, tok)) return r;
  return nullptr;
}
frame::SemFrame* predAt(frame::SemSentence& s, int tok) {
  for (frame::Unit& u : s.units)
    if (u.type == frame::Unit::Clause)
      if (frame::SemFrame* r = predAt(u.frame, tok)) return r;
  return nullptr;
}

// Spanish "¿Qué haces?" / "¿Qué comes, mamá?": "qué" + a 2nd-person verb read as a determiner + a noun ("[haces]", "τίνες
// ζωμοί"). The verb (a present indicative form of spanish.vpl) is replaced by the same person of leer, which the
// tagger reads right, and the verb set back.
bool queVerb(const std::string& text, const frame::FrameBuilder& fb, const frame::SemSentence& s, const lex::Lexicon* es,
             frame::SemSentence& out) {
  if (!es || s.units.empty() || s.units[0].type != frame::Unit::Clause || s.units[0].frame.hasPred) return false;
  for (size_t k = 1; k < s.tokens.size(); ++k) {
    if (s.tokens[k - 1].lower != "qué" || s.tokens[k].upos == "VERB" || s.tokens[k].upos == "AUX") continue;
    std::vector<lex::Analysis> an;
    es->lookup(text::es_key(s.tokens[k].lower), an);
    std::string inf;
    uint8_t pe = 0, nu = 0;
    for (const lex::Analysis& a : an) {
      const lex::Lemma l = es->lemma(a.lemma);
      const feat::Features af = feat::unpack(es->feature(a.feat));
      if (l.pos != feat::Verb || !af.person || af.tense != feat::Present || (af.mood != 0 && af.mood != feat::Indicative))
        continue;
      inf = text::lower(std::string(l.head));
      pe = af.person;
      nu = af.number;
      break;
    }
    if (inf.empty()) continue;
    const bool pl = nu == feat::Pl;
    const char* sw = pe == 1 ? (pl ? "leemos" : "leo") : pe == 2 ? (pl ? "leéis" : "lees") : (pl ? "leen" : "lee");
    frame::SemSentence alt;
    if (!substituteWord(text, fb, s, k, sw, alt)) continue;
    frame::SemFrame* g = predAt(alt, (int)k);
    if (!g || text::lower(g->pred.lemma) != "leer") continue;
    g->pred.lemma = inf;
    out = std::move(alt);
    return true;
  }
  return false;
}

// Spanish "Ven aquí." / "¡Ven pronto!" / "Hijo, ven aquí.": "ven" read as the 3rd plural of ver ("they see here", rated
// OK). A clause-first "ven" with no subject of its own and no object but a pronoun is the imperative of venir: the
// sentence is analysed with "sal" (the same length) in its place and the verb set back to venir.
bool venImperative(const std::string& text, const frame::FrameBuilder& fb, const frame::SemSentence& s,
                   frame::SemSentence& out) {
  for (size_t k = 0; k < s.tokens.size(); ++k) {
    if (s.tokens[k].lower != "ven") continue;
    bool first = true;   // only punctuation (¡, a comma after an address) or an address word before it
    for (size_t j = 0; j < k; ++j) first = first && (s.tokens[j].upos == "PUNCT" || j + 1 < k);
    if (!first || (k > 0 && s.tokens[k - 1].upos != "PUNCT")) continue;
    frame::SemSentence tmp = s;
    frame::SemFrame* f = predAt(tmp, (int)k);
    if (!f || text::lower(f->pred.lemma) != "ver") continue;
    if (f->hasSubject && !f->implicitSubject && f->subject.token >= 0) continue;
    if (f->hasObject && !f->object.isPronoun && !(f->object.isName || transfer::animate(f->object))) continue;
    frame::SemSentence alt;
    if (!substituteWord(text, fb, s, k, "sal", alt)) continue;
    frame::SemFrame* g = predAt(alt, (int)k);
    if (!g || g->type != frame::Kind::Imp || text::lower(g->pred.lemma) != "salir") continue;
    g->pred.lemma = "venir";
    // a person after the comma the parser took as the object ("Ven aquí, niño") is left to the address repair
    out = std::move(alt);
    return true;
  }
  return false;
}

// English irregular pasts the tagger reads as a noun or a name ("Mary wept for her brother." -> the name "Mary wept";
// "The queen wept when she heard the news." -> wept as a noun; Latin and Greek both gave "[wept]"): a word that
// english.vpl knows only as the finite past of another verb (frame::en::verbOfForm, reused), right after a noun or a
// pronoun and given no predicate, is made the verb: the sentence is analysed with a regular past of the same length in
// its place and that verb set back (past tense).
bool pastAsNoun(const std::string& text, const frame::FrameBuilder& fb, const frame::SemSentence& s,
                const lex::Lexicon* en, frame::SemSentence& out, std::string& form) {
  if (!en) return false;
  // regular pasts the tagger reads as verbs in any context (the offsets are restored after the analysis)
  static const char* const kStand[] = {"cried", "called", "played", "looked", "watched"};
  for (size_t k = 1; k < s.tokens.size(); ++k) {
    const nlp::Token& t = s.tokens[k];
    const nlp::Token& pv = s.tokens[k - 1];
    if (t.upos == "VERB" || t.upos == "AUX" || t.upos == "PUNCT" || t.lower.size() < 3) continue;
    // after a noun or a pronoun (the subject's end); the tagger sometimes makes the subject noun an adjective ("The
    // soldier/ADJ wept/NOUN because ...")
    const bool detAdj = pv.upos == "ADJ" && k >= 2 && s.tokens[k - 2].upos == "DET";
    if (pv.upos != "NOUN" && pv.upos != "PROPN" && pv.upos != "PRON" && !detAdj) continue;
    bool present = false;
    const std::string v = frame::en::verbOfForm(*en, t.lower, &present);
    if (v.empty() || present || v == t.lower || v.find(' ') != std::string::npos) continue;
    // only a finite past of that verb, and nothing else (no noun, adjective or participle-only reading)
    std::vector<lex::Analysis> an;
    en->lookup(text::en_key(t.lower), an);
    bool finite = false, other = false;
    for (const lex::Analysis& a : an) {
      const lex::Lemma l = en->lemma(a.lemma);
      const feat::Features af = feat::unpack(en->feature(a.feat));
      if (l.pos != feat::Verb) { other = true; continue; }
      if (text::lower(std::string(l.head)) != v) continue;
      if ((af.tense == feat::Perfect || af.tense == feat::Pluperfect) && (af.mood == 0 || af.mood == feat::Indicative)) finite = true;
    }
    // a noun reading too ("dug" is also a noun) is a misreading when a determiner or a pronoun follows ("The farmer
    // dug a deep hole": a noun is not followed by "a")
    const bool detNext = k + 1 < s.tokens.size() && (s.tokens[k + 1].upos == "DET" || s.tokens[k + 1].upos == "PRON");
    if (!finite || (other && !detNext)) continue;
    frame::SemSentence tmp = s;
    if (predAt(tmp, (int)k)) continue;
    std::string nl;
    bool npl = false;
    const bool subjNoun = detAdj && nounTaggedAdj(s, *en, k - 1, nl, npl);
    for (const char* sw : kStand) {
      frame::SemSentence alt;
      std::vector<std::pair<size_t, std::string>> subs = {{k, sw}};
      if (subjNoun) subs.emplace_back(k - 1, npl ? "boys" : "boy");
      if (!substituteWords(text, fb, s, subs, alt)) continue;
      frame::SemFrame* g = predAt(alt, (int)k);
      if (!g || g->pred.tense != frame::Tense::Past) continue;
      g->pred.lemma = v;
      alt.tokens[k].lemma = v;
      alt.tokens[k].upos = "VERB";
      if (subjNoun) renameNounBack(alt, (int)k - 1, "boy", nl);
      out = std::move(alt);
      form = t.lower;
      return true;
    }
    // the tagger reads the whole subject wrongly in context ("The soldier wept because his horse died." -> soldier an
    // adjective): the clause up to a subordinator after the past is analysed alone (C17's retagging makes the past a
    // verb in a sentence without one) and the rest merged as its dependent clause
    for (size_t j = k + 1; j + 1 < s.tokens.size(); ++j) {
      const std::string& m = s.tokens[j].lower;
      frame::Relation rel;
      if (m == "because" || m == "since") rel = frame::Relation::Cause;
      else if (m == "when" || m == "while" || m == "after" || m == "before" || m == "until") rel = frame::Relation::Time;
      else if (m == "if") rel = frame::Relation::Condition;
      else continue;
      size_t a1 = (size_t)s.tokens[j].start;
      while (a1 > 0 && (text[a1 - 1] == ' ' || text[a1 - 1] == ',')) --a1;
      const size_t b0 = (size_t)s.tokens[j + 1].start;
      frame::SemSentence merged;
      if (!mergeParts(text, fb, 0, a1, b0, true, rel, m, false, true, merged)) break;
      frame::SemFrame* g = predAt(merged, (int)k);
      if (!g || text::lower(g->pred.lemma) != v) break;
      out = std::move(merged);
      form = t.lower;
      return true;
    }
  }
  return false;
}

// Spanish 3rd-person present verbs read as nouns in a sentence without a verb ("El búho caza de noche." -> "the owl of
// hunting of night"; C25 repaired "nada" alone): the word right after the subject NP that spanish.vpl knows as a 3rd
// person present indicative is the verb; analysed with a common verb of the same length in its place, its lemma set
// back. ASCII forms only (the stand-ins are ASCII, so the byte length is the same).
bool esVerbAsNoun(const std::string& text, const frame::FrameBuilder& fb, const frame::SemSentence& s,
                  const lex::Lexicon* es, frame::SemSentence& out) {
  if (!es || s.units.size() != 1 || s.units[0].type != frame::Unit::Clause) return false;
  const frame::SemFrame& f0 = s.units[0].frame;
  if (f0.hasPred || !f0.hasSubject || f0.subject.isPronoun) return false;
  for (const nlp::Token& t : s.tokens) if (t.upos == "VERB" || t.upos == "AUX") return false;
  // the word right after the subject's head (the misread verb is often hung on the subject as an "of" attribute)
  const int head = f0.subject.token;
  const size_t k = (size_t)head + 1;
  if (head < 0 || k >= s.tokens.size() || s.tokens[k].upos == "PUNCT" || s.tokens[k].upos == "ADP") return false;
  const std::string w = s.tokens[k].lower;
  for (char ch : w) if ((unsigned char)ch >= 0x80) return false;
  std::vector<lex::Analysis> an;
  es->lookup(text::es_key(w), an);
  std::string inf;
  uint8_t num = 0;
  for (const lex::Analysis& a : an) {
    const lex::Lemma l = es->lemma(a.lemma);
    const feat::Features af = feat::unpack(es->feature(a.feat));
    if (l.pos != feat::Verb || af.person != 3 || af.tense != feat::Present || (af.mood != 0 && af.mood != feat::Indicative)) continue;
    inf = text::lower(std::string(l.head));
    num = af.number;
    break;
  }
  if (inf.empty()) return false;
  static const char* const kSg[] = {"lee", "come", "corre", "camina", "escribe"};
  static const char* const kPl[] = {"leen", "comen", "corren", "caminan", "escriben"};
  const char* const* list = num == feat::Pl ? kPl : kSg;
  const size_t base = num == feat::Pl ? 4 : 3;
  if (w.size() < base || w.size() > base + 4) return false;
  frame::SemSentence alt;
  if (!substituteWord(text, fb, s, k, list[w.size() - base], alt)) return false;
  frame::SemFrame* g = predAt(alt, (int)k);
  if (!g || g->type != frame::Kind::Decl || !g->hasSubject) return false;
  g->pred.lemma = inf;
  out = std::move(alt);
  return true;
}

// "When one is tired, one sleeps.": the generic "one" before a verb in -s read as a numeral ("one sleep", a noun
// phrase). Every clause-initial "one" is analysed as "she" (the same length) and the subjects made generic again
// (the transfer renders them τις). English only.
bool oneGeneric(const std::string& text, const frame::FrameBuilder& fb, const frame::SemSentence& s,
                frame::SemSentence& out) {
  bool frag = false;
  for (const frame::Unit& u : s.units)
    if (u.type == frame::Unit::Clause && !u.frame.hasPred && u.frame.hasSubject) frag = true;
  if (!frag) return false;
  std::vector<size_t> ones;
  for (size_t k = 0; k + 1 < s.tokens.size(); ++k) {
    if (s.tokens[k].lower != "one") continue;
    const bool start = k == 0 || s.tokens[k - 1].upos == "PUNCT" || s.tokens[k - 1].upos == "SCONJ" ||
                       s.tokens[k - 1].upos == "CCONJ";
    const std::string& nx = s.tokens[k + 1].lower;
    if (start && (nx.size() > 2 && nx.back() == 's')) ones.push_back(k);
    else if (start && (nx == "is" || nx == "was" || nx == "can" || nx == "must" || nx == "should" || nx == "has"))
      ones.push_back(k);
  }
  if (ones.empty()) return false;
  std::string alt = text;
  for (size_t k : ones) {
    const nlp::Token& t = s.tokens[k];
    const char* she = t.text[0] == 'O' ? "She" : "she";
    for (size_t i = 0; i < 3; ++i) alt[(size_t)t.start + i] = she[i];
  }
  fb.analyse(alt, out);
  if (out.tokens.size() != s.tokens.size()) return false;
  for (size_t i = 0; i < out.tokens.size(); ++i) {
    out.tokens[i].text = s.tokens[i].text;
    out.tokens[i].lower = s.tokens[i].lower;
  }
  out.text = text;
  int fixed = 0;
  std::function<void(frame::SemFrame&)> mark = [&](frame::SemFrame& f) {
    if (f.hasSubject && f.subject.isPronoun &&
        std::find(ones.begin(), ones.end(), (size_t)std::max(0, f.subject.token)) != ones.end() && f.subject.token >= 0) {
      f.subject.pronLemma = "one-generic";
      f.subject.head = "one";
      ++fixed;
    }
    for (auto& sb : f.subordinate) for (auto& x : sb.frame) mark(x);
  };
  bool allPred = true;
  for (frame::Unit& u : out.units)
    if (u.type == frame::Unit::Clause) { mark(u.frame); allPred = allPred && u.frame.hasPred; }
  return fixed > 0 && allPred;
}

// A word that addresses someone: kinship and person nouns of children's dialogue, a name of names_grc.tsv that is not
// a place, or a capitalised word the tagger calls a proper name.
bool addressWord(const nlp::Token& t, bool firstWord, const GreekData& gd) {
  static const char* const kWords[] = {
      "boy", "boys", "girl", "girls", "child", "children", "kid", "kids", "son", "daughter", "mother", "father", "mom",
      "mum", "mommy", "mummy", "dad", "daddy", "papa", "mama", "grandmother", "grandfather", "grandma", "grandpa",
      "granny", "brother", "sister", "friend", "friends", "sir", "madam", "teacher", "master", "lady", "uncle", "aunt",
      "everyone", "everybody", "darling", "sweetheart", "dear", "niño", "niña", "niños", "niñas", "hijo", "hija",
      "hijos", "hijas", "mamá", "papá", "madre", "padre", "abuelo", "abuela", "abuelita", "abuelito", "hermano",
      "hermana", "amigo", "amiga", "amigos", "amigas", "señor", "señora", "señorita", "maestro", "maestra", "profesor",
      "profesora", "chico", "chica", "chicos", "chicas", "muchacho", "muchacha", "muchachos", "muchachas", "tío", "tía",
      "cariño", "todos", nullptr};
  for (const char* const* w = kWords; *w; ++w)
    if (t.lower == *w) return true;
  const bool cap = !t.text.empty() && t.text[0] >= 'A' && t.text[0] <= 'Z';
  if (!cap) return false;
  if (const NameEntry* e = gd.nameByEnglish(t.text)) return !e->place && e->policy != curated::NamePolicy::Translate;
  return !firstWord && t.upos == "PROPN";
}
bool addressModifier(const std::string& w) {
  static const char* const kMods[] = {"my", "our", "dear", "little", "poor", "oh", "o", "mi", "mis", "nuestro",
                                      "nuestra", "querido", "querida", "queridos", "queridas", "pequeño", "pequeña",
                                      "pobre", nullptr};
  for (const char* const* m = kMods; *m; ++m)
    if (w == *m) return true;
  return false;
}

// The vocative NP of an address of one to three words (tokens `v` of s; the last is the address word): built from the
// words themselves (the parser reads a lone "Mamá." as a verb): the head's singular lemma, the Spanish gender, "my /
// mi" as the possessor (the transfer leaves it out: ὦ παῖ), "dear / little / querido" as adjectives.
frame::SemNP addressNP(const frame::SemSentence& s, const std::vector<size_t>& v, const GreekData& gd) {
  frame::SemNP n;
  const nlp::Token& h = s.tokens[v.back()];
  static const char* const kPl[][2] = {{"children", "child"}, {"boys", "boy"}, {"girls", "girl"}, {"kids", "kid"},
                                       {"friends", "friend"}, {"niños", "niño"}, {"niñas", "niña"}, {"hijos", "hijo"},
                                       {"hijas", "hija"}, {"amigos", "amigo"}, {"amigas", "amiga"}, {"chicos", "chico"},
                                       {"chicas", "chica"}, {"muchachos", "muchacho"}, {"muchachas", "muchacha"}};
  n.head = h.lower;
  for (const auto& p : kPl)
    if (h.lower == p[0]) { n.head = p[1]; n.number = 2; }
  // a capitalised word that is not one of the address nouns ("Mamá" is) is a name ("Mary")
  nlp::Token low = h;
  low.text = h.lower;
  if (!h.text.empty() && h.text[0] >= 'A' && h.text[0] <= 'Z' && !addressWord(low, false, gd)) {
    n.isName = true;
    n.head = h.text;
  }
  static const char* const kF[] = {"niña", "hija", "mamá", "madre", "abuela", "abuelita", "hermana", "amiga", "señora",
                                   "señorita", "maestra", "profesora", "chica", "muchacha", "tía", nullptr};
  for (const char* const* f = kF; *f; ++f)
    if (n.head == *f) n.srcGender = feat::F;
  n.surface = h.text;
  n.token = (int)v.back();
  for (size_t i : v) n.tokens.push_back((int)i);
  for (size_t i = 0; i + 1 < v.size(); ++i) {
    const std::string& w = s.tokens[v[i]].lower;
    if (w == "my" || w == "our" || w == "mi" || w == "mis" || w == "nuestro" || w == "nuestra") {
      frame::SemNP p;
      p.isPronoun = true;
      p.pron.person = 1;
      p.pron.number = (w == "our" || w == "nuestro" || w == "nuestra") ? 2 : 1;
      p.pronLemma = w;
      p.token = (int)v[i];
      p.tokens.push_back((int)v[i]);
      n.possessor.push_back(p);
    } else if (w != "oh" && w != "o") {
      frame::SemAdj a;
      a.lemma = w == "querido" || w == "querida" || w == "queridos" || w == "queridas" ? "querido"
                : w == "pequeño" || w == "pequeña" ? "pequeño" : w;
      a.token = (int)v[i];
      n.adjectives.push_back(a);
    }
  }
  return n;
}

// "¿Por qué lloras, niña?" (niña read as the object: παῖδα), "Where are you, my son?" (son made the subject, "you"
// lost), "Niña, ¿por qué lloras?" (a broken fragment): an address of one to three words (an address word, optionally
// after my / dear / mi / querido) between a comma and the end of the sentence, or at its start before a comma, is
// analysed apart from the rest and becomes a vocative unit (ὦ παῖ). Only when the analysis has no vocative already;
// a trailing NP after a noun in a statement may be an apposition ("the king, my father") and is left alone.
bool addressSplit(const std::string& text, const frame::SemSentence& s, bool es, const GreekData& gd, const std::function<void(const std::string&, frame::SemSentence&)>& analyse,
                  frame::SemSentence& out) {
  // the words of a span [a, b) of the text: token indices of s
  auto tokensIn = [&](size_t a, size_t b) {
    std::vector<size_t> v;
    for (size_t i = 0; i < s.tokens.size(); ++i)
      if (s.tokens[i].start >= (int)a && s.tokens[i].end <= (int)b && s.tokens[i].upos != "PUNCT") v.push_back(i);
    return v;
  };
  // an address the analysis already has as a vocative (a vocative unit or a frame's vocative on these tokens)
  auto vocAlready = [&](const std::vector<size_t>& v) {
    for (const frame::Unit& u : s.units) {
      if (u.vocative && u.first <= (int)v.front() && u.last >= (int)v.back()) return true;
      if (u.type == frame::Unit::Clause)
        for (const frame::SemNP& n : u.frame.vocatives)
          if (n.token >= (int)v.front() && n.token <= (int)v.back()) return true;
    }
    return false;
  };
  auto isAddress = [&](const std::vector<size_t>& v, bool atStart) {
    if (v.empty() || v.size() > 3 || vocAlready(v)) return false;
    for (size_t i = 0; i + 1 < v.size(); ++i)
      if (!addressModifier(s.tokens[v[i]].lower)) return false;
    return addressWord(s.tokens[v.back()], atStart && v.size() == 1, gd);
  };
  auto capFirst = [](std::string x) {
    for (size_t i = 0; i < x.size(); ++i) {
      const unsigned char c = (unsigned char)x[i];
      if (c >= 'a' && c <= 'z') { x[i] = (char)(c - 'a' + 'A'); break; }
      if ((c >= 'A' && c <= 'Z') || c >= 0x80) {
        if (c == 0xC3 && i + 1 < x.size() && (unsigned char)x[i + 1] >= 0xA0 && (unsigned char)x[i + 1] <= 0xBF)
          x[i + 1] = (char)((unsigned char)x[i + 1] - 0x20);   // á é í ó ú ñ -> upper case
        if (c != 0xC2) break;   // ¿ ¡ (C2 BF, C2 A1) are skipped
        ++i;
      }
    }
    return x;
  };
  // the end of the words: trailing punctuation, quotes and spaces
  size_t e = text.size();
  while (e > 0 && std::strchr(" .?!\"'\xE2\x80\xA6", text[e - 1])) --e;
  while (e >= 3 && (unsigned char)text[e - 1] >= 0x80) {   // … » ” (multibyte closing marks)
    const std::string tail3 = text.substr(e - 3, 3), tail2 = text.substr(e - 2, 2);
    if (tail3 == "\xE2\x80\xA6" || tail3 == "\xE2\x80\x9D") e -= 3;
    else if (tail2 == "\xC2\xBB") e -= 2;
    else break;
    while (e > 0 && std::strchr(" .?!\"'", text[e - 1])) --e;
  }
  const std::string endPunct = text.substr(e);
  bool question = endPunct.find('?') != std::string::npos, bang = endPunct.find('!') != std::string::npos;
  // trailing address
  const size_t lc = text.rfind(',', e);
  if (lc != std::string::npos && lc > 0) {
    const std::vector<size_t> tail = tokensIn(lc + 1, e);
    const std::vector<size_t> head = tokensIn(0, lc);
    if (!head.empty() && isAddress(tail, false)) {
      const nlp::Token& hl = s.tokens[head.back()];
      // C31: not after a command ("Cuéntanos un cuento, papá." was "καὶ πατέρα": the head has an imperative verb)
      bool command = false;
      for (size_t i : head)
        command = command || (s.tokens[i].upos == "VERB" &&
                              nlp::morph::get(s.tokens[i].feats, nlp::morph::MoodShift) == nlp::morph::MoodImp);
      const bool apposition = (hl.upos == "NOUN" || hl.upos == "PROPN") && !question && !bang && !command;
      if (!apposition) {
        std::string ht = text.substr(0, lc);
        while (!ht.empty() && ht.back() == ' ') ht.pop_back();
        frame::SemSentence sh;
        analyse(ht + (endPunct.empty() ? "." : endPunct), sh);
        if (sh.units.empty() || sh.units.back().type != frame::Unit::Clause || !sh.units.back().frame.hasPred) return false;
        out = std::move(sh);
        // the head's own final punctuation (added for the analysis) is not a token of the sentence
        while (!out.tokens.empty() && out.tokens.back().start >= (int)ht.size()) out.tokens.pop_back();
        out.drop.resize(std::min(out.drop.size(), out.tokens.size()));
        while (out.drop.size() < out.tokens.size()) out.drop.push_back(frame::Drop::No);
        for (frame::Unit& u : out.units) u.last = std::min(u.last, (int)out.tokens.size() - 1);
        const int base = (int)out.tokens.size();
        std::vector<size_t> nv;   // the address words, re-indexed after the head's tokens
        for (size_t i : tail) {
          nv.push_back(out.tokens.size());
          out.tokens.push_back(s.tokens[i]);
          out.drop.push_back(frame::Drop::No);
        }
        for (size_t i = 0; i < s.tokens.size(); ++i)   // the sentence's final punctuation
          if (s.tokens[i].start >= (int)e) { out.tokens.push_back(s.tokens[i]); out.drop.push_back(frame::Drop::Punct); }
        frame::Unit vu;
        vu.type = frame::Unit::Clause;
        vu.vocative = true;
        vu.frame.type = frame::Kind::Frag;
        vu.frame.hasSubject = true;
        vu.frame.subject = addressNP(out, nv, gd);
        for (size_t i : nv) vu.frame.tokens.push_back((int)i);
        vu.first = base;
        vu.last = (int)nv.back();
        out.units.back().sepAfter = ",";
        out.units.push_back(std::move(vu));
        out.text = text;
        return true;
      }
    }
  }
  // leading address: "Niña, ¿por qué lloras?", "Hijo, ven aquí."
  size_t ls = 0;
  while (ls < text.size() && (text[ls] == ' ' || text[ls] == '-' || text[ls] == '"' ||
                              (ls + 1 < text.size() && (unsigned char)text[ls] == 0xC2 &&
                               ((unsigned char)text[ls + 1] == 0xA1 || (unsigned char)text[ls + 1] == 0xBF)))) {
    ls += (unsigned char)text[ls] == 0xC2 ? 2 : 1;
  }
  const size_t fc = text.find(',', ls);
  if (!es || fc == std::string::npos || fc + 2 >= e) return false;   // English leading addresses are parsed already
  const std::vector<size_t> lead = tokensIn(ls, fc);
  if (!isAddress(lead, true)) return false;
  size_t r0 = fc + 1;
  while (r0 < text.size() && text[r0] == ' ') ++r0;
  frame::SemSentence sr;
  const std::string rt = capFirst(text.substr(r0));
  analyse(rt, sr);
  if (sr.units.empty() || sr.units[0].type != frame::Unit::Clause || !sr.units[0].frame.hasPred) return false;
  out = frame::SemSentence{};
  out.lang = s.lang;
  out.text = text;
  int nv = 0;
  for (size_t i = 0; i < s.tokens.size(); ++i)   // opening punctuation (¡ ¿) before the address
    if (s.tokens[i].end <= (int)ls) { out.tokens.push_back(s.tokens[i]); out.drop.push_back(frame::Drop::Punct); }
  const int vbase = (int)out.tokens.size();
  std::vector<size_t> nvi;
  for (size_t i : lead) {
    nvi.push_back(out.tokens.size());
    out.tokens.push_back(s.tokens[i]);
    out.drop.push_back(frame::Drop::No);
    ++nv;
  }
  frame::Unit vu;
  vu.type = frame::Unit::Clause;
  vu.vocative = true;
  vu.frame.type = frame::Kind::Frag;
  vu.frame.hasSubject = true;
  vu.frame.subject = addressNP(out, nvi, gd);
  for (size_t i : nvi) vu.frame.tokens.push_back((int)i);
  vu.first = vbase;
  vu.last = vbase + nv - 1;
  vu.sepAfter = ",";
  out.units.push_back(std::move(vu));
  const int rbase = (int)out.tokens.size();
  for (size_t i = 0; i < sr.tokens.size(); ++i) {
    nlp::Token t = sr.tokens[i];
    t.start += (int)r0;
    t.end += (int)r0;
    t.text = text.substr((size_t)t.start, (size_t)(t.end - t.start));
    t.lower = text::lower(t.text);
    out.tokens.push_back(t);
    out.drop.push_back(i < sr.drop.size() ? sr.drop[i] : frame::Drop::No);
  }
  for (frame::Unit u : sr.units) {
    if (u.type == frame::Unit::Clause) shiftFrame(u.frame, rbase);
    u.first += rbase;
    u.last += rbase;
    out.units.push_back(std::move(u));
  }
  out.finalPunct = sr.finalPunct;
  out.question = sr.question || question;
  for (const std::string& r : sr.repairs) out.repairs.push_back(r);
  for (const std::string& d : sr.doubts) out.doubt(d.c_str());
  return true;
}

bool checkOk(const CueOutput& o, const char* id) {
  for (const Check& c : o.checks)
    if (c.id == id) return c.ok;
  return true;
}

}  // namespace

// What one sentence produced.
struct SentOut {
  cue::Latin text;                      // Greek text + token views (cue::Latin is the generic text + tokens holder)
  std::vector<int> srcOffset;
  std::vector<Reason> reasons;
  std::vector<std::string> flags;
  std::vector<transfer::Choice> choices;
  std::vector<std::string> unknown, missing;
  double minMargin = 1.0;
  bool nonverbal = false, song = false, copied = false;
  std::vector<Alternative> alternatives;
};

struct GreekPath::Impl {
  const lex::Lexicon& lx;
  curated::CuratedData cd;
  GreekData gd;
  GreekTables gt;
  PathConfig cfg;
  std::unique_ptr<GreekTransfer> xfer;
  std::unique_ptr<GreekRealiser> real;
  std::unique_ptr<check::GreekChecker> checker;
  std::unique_ptr<grc2x::Translator> back;
  const nlp::Pipeline* pen = nullptr;
  const nlp::Pipeline* pes = nullptr;
  const lex::Lexicon* enLex = nullptr;
  const lex::Lexicon* esLex = nullptr;
  std::unique_ptr<frame::FrameBuilder> fbEn, fbEs;
  const std::vector<rules::GlossaryEntry>* glossary = nullptr;
  transfer::Memory memBefore;
  uint8_t royalGender = 0;
  std::vector<std::string> a9Sources;   // C16: EN/ES content lemmas of the cue being checked (A9)
  bool a9Set = false;   // C16: gender of the last king / queen named in the file (address "your majesty")

  // C16: the addressee of a title of address: a glossary entry for the title ("Majesty", "Your Majesty") with a
  // gender, else the last royal noun of the file, else 0 (unknown).
  uint8_t addresseeGender(const std::string& pattern) const {
    if (glossary)
      for (const rules::GlossaryEntry& e : *glossary) {
        const std::string k = text::lower(e.name);
        if ((k == text::lower(pattern) || pattern.find(k) != std::string::npos) && !e.gender.empty())
          return e.gender == "f" ? feat::F : feat::M;
      }
    return royalGender;
  }
  void noteRoyal(const std::vector<transfer::Choice>& choices) {
    for (const transfer::Choice& c : choices) {
      if (c.lemma == kNone) continue;
      const std::string k(lx.lemma(c.lemma).key);
      if (k == "βασίλεια" || k == "δέσποινα" || k == "ἄνασσα") royalGender = feat::F;
      else if (k == "βασιλεύσ" || k == "δεσπότησ" || k == "ἄναξ") royalGender = feat::M;
    }
  }

  Impl(const lex::Lexicon& l, curated::CuratedData c, GreekData g, GreekTables t, PathConfig k)
      : lx(l), cd(std::move(c)), gd(std::move(g)), gt(std::move(t)), cfg(k) {
    cd.replacePhrasebooks(gd.phrasebook(), gt.phrasebookEs());
    xfer = std::make_unique<GreekTransfer>(lx, cd, gd, gt);
    real = std::make_unique<GreekRealiser>(lx, cd, gd);
    checker = std::make_unique<check::GreekChecker>(lx, cd, gd);
    back = std::make_unique<grc2x::Translator>(lx, cd, gd, gt);
  }

  const frame::FrameBuilder* builder(frame::SrcLang lang) {
    if (lang == frame::SrcLang::En) {
      if (!pen) return nullptr;
      if (!fbEn) fbEn = std::make_unique<frame::FrameBuilder>(lang, pen, enLex, cd);
      return fbEn.get();
    }
    if (!pes) return nullptr;
    if (!fbEs) fbEs = std::make_unique<frame::FrameBuilder>(lang, pes, esLex, cd);
    return fbEs.get();
  }

  uint8_t tierOf(uint32_t lemma) const {
    const lex::Lemma l = lx.lemma(lemma);
    if (l.id == kNone) return 0;
    uint8_t tier = l.tier;
    if (const curated::TierEntry* te = cd.tierGreek(l.key))
      if (te->tier && (!tier || te->tier < tier)) tier = te->tier;
    return tier;
  }

  void tokenInfo(const std::string& word, rules::TokenView& t) const {
    morph::Token mt;
    analyse(lx, word, mt);
    t.text = t.display = word;
    if (mt.analyses.empty()) return;
    size_t best = 0;
    uint8_t bestTier = 9;
    for (size_t i = 0; i < mt.analyses.size(); ++i) {
      const uint8_t tr = tierOf(mt.analyses[i].lemma);
      if ((tr ? tr : 3) < bestTier) { bestTier = tr ? tr : 3; best = i; }
    }
    const lex::Analysis& a = mt.analyses[best];
    t.lemmaId = a.lemma;
    t.hasLemma = true;
    t.features = realise::featureView(morph::packedOf(lx, a));
    t.tier = tierOf(a.lemma);
  }

  void realiseClause(const GrcClause& cl0, const rules::Options& opt, cue::Latin& out, std::vector<Reason>& reasons,
                     std::vector<std::string>& flags) {
    GrcClause cl = cl0;
    if (cl.punct.empty()) cl.punct = ".";
    GrcOptions ro;
    ro.emoji = opt.emoji;
    ro.emojiInText = false;
    ro.fidelity = opt.fidelity;
    ro.glossary = glossary;
    GrcSentence gs;
    real->realise(cl, ro, gs);
    stripFinal(gs.text);
    out.text = gs.text;
    out.tokens = gs.tokens;
    for (const Reason& r : gs.reasons) reasons.push_back(r);
    for (const std::string& f : gs.flags) addFlag(flags, f);
  }

  // ---- phrasebook piece ---------------------------------------------------------------------------------------------
  void renderPhrase(const frame::PhraseMatch& m, const frame::SemSentence& s, const transfer::Settings& st,
                    transfer::Memory& mem, const rules::Options& opt, cue::Latin& out, std::vector<Reason>& reasons,
                    std::vector<std::string>& flags, std::vector<transfer::Choice>& choices, std::vector<int>& covered,
                    std::vector<std::string>& unknown) {
    std::string greekText = m.latin;
    // C31: a plural address in the same sentence ("Children, be quiet!" -> ὦ παῖδες, σιγᾶτε; σίγα was produced)
    bool plAddr = mem.addresseePlural;
    for (const frame::Unit& u : s.units) {
      if (u.vocative && u.frame.hasSubject && u.frame.subject.number == 2 && !u.frame.subject.isPronoun) plAddr = true;
      if (u.type == frame::Unit::Clause)
        for (const frame::SemNP& v : u.frame.vocatives) plAddr = plAddr || v.number == 2;
    }
    if (plAddr && greekText.find(' ') == std::string::npos) {   // "plural χαίρετε"
      const size_t p = m.note.find("plural");
      if (p != std::string::npos) {
        size_t a = p + 6;
        while (a < m.note.size() && (m.note[a] == ':' || m.note[a] == ' ')) ++a;
        size_t b = a;
        while (b < m.note.size() && m.note[b] != ' ' && m.note[b] != ',' && m.note[b] != ';') ++b;
        if (b > a) greekText = m.note.substr(a, b - a);
      }
    }
    char g = st.speakerGender;
    if (st.flipSpeakerGender) g = g == 'f' ? 'm' : 'f';
    for (const std::string& w0 : splitWords(greekText)) {
      std::string w = w0;
      if (w.size() > 2 && w[0] == '{') {
        const size_t close = w.find('}');
        const std::string inner = w.substr(1, close == std::string::npos ? std::string::npos : close - 1);
        const std::string tail = close == std::string::npos ? std::string() : w.substr(close + 1);
        const size_t colon = inner.find(':');
        const int k = std::atoi(inner.substr(0, colon).c_str()) - 1;
        const std::string cs = colon == std::string::npos ? std::string() : inner.substr(colon + 1);
        if (k < 0 || (size_t)k >= m.slots.size()) continue;
        const frame::PhraseSlot& sl = m.slots[(size_t)k];
        GrcClauseOut co;
        cue::Latin piece;
        std::vector<Reason> pr;
        if (sl.kind == frame::SlotKind::Wh || sl.kind == frame::SlotKind::VP) {
          const std::vector<frame::SemFrame>& fr = sl.kind == frame::SlotKind::Wh ? sl.wh : sl.vp;
          if (fr.empty()) continue;
          // C16: a {WH} slot is an indirect question (dependent: "which way you go" -> ποίαν ὁδὸν εἶ)
          xfer->clause(fr[0], s, st, mem, co, sl.kind == frame::SlotKind::Wh);
          if (cs == "subj") co.clause.pred.mood = feat::Subjunctive;
          if (sl.kind == frame::SlotKind::VP) co.clause.hasSubject = false;
          if (cs == "inf") {   // C16: {1:inf}: the verb phrase as an infinitive ("ἆρα οἶσθα παίζειν;")
            GrcClause body = co.clause;
            body.type = ClauseType::Decl;
            body.hasSubject = false;
            body.connectors.clear();
            GrcClause wrap;
            wrap.type = ClauseType::Frag;
            GrcSub sub;
            sub.rel = SubRel::AccInf;
            sub.clause.push_back(std::move(body));
            wrap.subs.push_back(std::move(sub));
            co.clause = std::move(wrap);
          }
          realiseClause(co.clause, opt, piece, pr, flags);
        } else if (sl.kind == frame::SlotKind::Adj) {
          transfer::Choice ch;
          ch.token = sl.adj.token;
          const uint32_t id = xfer->select(sl.adj.lemma, feat::Adj, {}, false, false, st, ch);
          co.choices.push_back(ch);
          co.covered.push_back(sl.adj.token);
          if (id != kNone) {
            GrcClause fc;
            fc.type = ClauseType::Frag;
            GrcAdj a;
            a.lemma = id;
            fc.predAdj.push_back(a);
            fc.predGender = feat::N;
            realiseClause(fc, opt, piece, pr, flags);
          } else {
            co.unknownWords.push_back(sl.adj.lemma);
          }
        } else {
          GrcNP x = xfer->np(sl.np, s, st, mem, co);
          if (sl.kind == frame::SlotKind::Name) x.definite = false;   // "ὄνομά μοί ἐστι Ἀλίκη": no article
          x.case_ = cs.empty() ? (uint8_t)feat::Nom : curated::parseCase(cs);
          if (!x.case_) x.case_ = feat::Nom;
          if (x.case_ == feat::Voc) x.definite = false;
          GrcClause fc;
          fc.type = ClauseType::Frag;
          fc.hasObject = true;
          fc.object = x;
          realiseClause(fc, opt, piece, pr, flags);
        }
        choices.insert(choices.end(), co.choices.begin(), co.choices.end());
        covered.insert(covered.end(), co.covered.begin(), co.covered.end());
        unknown.insert(unknown.end(), co.unknownWords.begin(), co.unknownWords.end());
        for (const std::string& f : co.flags) addFlag(flags, f);
        for (const Reason& r : co.notes) reasons.push_back(r);
        const int base = (int)out.tokens.size();
        for (Reason r : pr) { if (r.tokenIndex >= 0) r.tokenIndex += base; reasons.push_back(r); }
        piece.text += tail;
        cue::append(out, piece);
        continue;
      }
      std::string punct;
      while (!w.empty() && (w.back() == ',' || w.back() == '!' || w.back() == '?' || w.back() == ';')) {
        punct.insert(punct.begin(), w.back());
        w.pop_back();
      }
      if (w.size() > 2 && w.front() == '(' && w.back() == ')') w = w.substr(1, w.size() - 2);
      const size_t slash = w.find('/');
      if (slash != std::string::npos && m.note.find("addressee") != std::string::npos) {
        // C16: "your majesty" -> ὦ βασιλεῦ / ὦ βασίλεια by the addressee: the glossary, else the last king or queen
        // named in the file, else masculine with Check (rules_grc2_notes.md, quality loop 2)
        uint8_t ag = addresseeGender(m.pattern);
        if (!ag) { ag = feat::M; addFlag(flags, "addressee-guess"); }
        w = ag == feat::F ? w.substr(slash + 1) : w.substr(0, slash);
      } else if (slash != std::string::npos) {
        w = g == 'f' ? w.substr(slash + 1) : w.substr(0, slash);
        addFlag(flags, "speaker-gender");
      }
      cue::Latin one;
      rules::TokenView t;
      tokenInfo(w, t);
      one.text = t.text + punct;
      t.start = 0;
      t.end = (int)t.text.size();
      one.tokens.push_back(t);
      cue::append(out, one);
    }
    // movable ν between the pieces of the row: "ἐστι" before a vowel -> "ἐστιν"
    {
      std::vector<std::string> tx;
      bool changed = false;
      for (size_t i = 0; i < out.tokens.size(); ++i) {
        tx.push_back(out.tokens[i].text);
        if (i + 1 >= out.tokens.size()) continue;
        const std::string b = text::greek_bare(out.tokens[i].text);
        const bool nuWord = b == "εστι" || b == "εισι" || b == "πασι";
        const bool gap = out.tokens[i].end < (int)out.text.size() && out.text[(size_t)out.tokens[i].end] == ' ';
        if (nuWord && gap && startsWithVowel(out.tokens[i + 1].text)) { tx.back() += "ν"; changed = true; }
        // C18: the grave rule across the pieces ("{1:acc} αὐτοῦ" -> "τὴν κεφαλὴν αὐτοῦ"): an acute on the ultima before
        // another word without punctuation becomes grave, not before an enclitic, never on τίς / τί
        const AccentInfo ai = accentOf(tx.back());
        const std::string k = text::greek_bare(tx.back());
        if (gap && ai.type == Accent::Acute && ai.position == 0 && ai.accents == 1 && k != "τισ" && k != "τι" &&
            isGreekWord(out.tokens[i + 1].text) && !isEncliticForm(out.tokens[i + 1].text)) {
          tx.back() = ultimaToGrave(tx.back());
          changed = true;
        }
      }
      if (changed) rewriteTokens(out, tx);
    }
    for (int k = m.first; k <= m.last; ++k) covered.push_back(k);
    for (const frame::PhraseSlot& sl : m.slots)
      for (int k = sl.first; k <= sl.last; ++k) covered.push_back(k);
    reasons.push_back(Reason{out.tokens.empty() ? -1 : 0, "phrasebook", m.pattern + " -> " + m.latin,
                             "{\"pattern\":\"" + jsonEscape(m.pattern) + "\",\"greek\":\"" + jsonEscape(m.latin) +
                                 "\",\"tier\":" + std::to_string((int)m.tier) + "}"});
  }

  // C25 (as C24 (g) for Latin): spans of quoted sound words: one or two words between straight or curly double quotes,
  // each an interjection of the source lexicon or a word it does not know ("woof", "moo", "guau"), not a translatable
  // answer word (yes, no, hello ...). The span (quotes included) becomes the pronoun "it" / "eso" and spaces (offsets
  // kept); the Greek pronoun of that place is replaced by the quoted word as written.
  void quotedSounds(std::string& t, bool es, std::vector<std::pair<size_t, size_t>>& spans) const {
    const lex::Lexicon* src = es ? esLex : enLex;
    if (!src) return;
    auto openAt = [&](size_t i, size_t& len) {
      if (t[i] == '"') { len = 1; return true; }
      if (i + 2 < t.size() && (unsigned char)t[i] == 0xE2 && (unsigned char)t[i + 1] == 0x80 &&
          ((unsigned char)t[i + 2] == 0x9C || (unsigned char)t[i + 2] == 0x9D)) { len = 3; return true; }
      if (i + 1 < t.size() && (unsigned char)t[i] == 0xC2 && ((unsigned char)t[i + 1] == 0xAB || (unsigned char)t[i + 1] == 0xBB)) {
        len = 2;   // « »
        return true;
      }
      return false;
    };
    for (size_t i = 0; i < t.size(); ++i) {
      size_t ol = 0;
      if (!openAt(i, ol)) continue;
      const size_t j = i + ol;
      size_t cl = 0, e = j;
      while (e < t.size() && !openAt(e, cl)) ++e;
      if (e >= t.size()) break;
      std::string inner = t.substr(j, e - j);
      while (!inner.empty() && (inner.back() == ',' || inner.back() == '!' || inner.back() == '.' || inner.back() == '?'))
        inner.pop_back();
      std::vector<std::string> ws;
      bool okWords = !inner.empty() && inner.size() <= 24;
      for (const std::string& w0 : splitWords(inner)) {
        const std::string w = text::lower(w0);
        for (char ch : w) okWords = okWords && (std::isalpha((unsigned char)ch) || ch == '-');
        if (!w.empty()) ws.push_back(w);
      }
      okWords = okWords && !ws.empty() && ws.size() <= 2;
      for (const std::string& w : ws) {
        if (!okWords) break;
        static const char* const kWords[] = {"yes", "no", "hello", "hi", "goodbye", "bye", "oh", "ah", "please",
                                             "thanks", "ok", "okay", "sorry", "help", "hooray", "alas", "well", "why",
                                             "what", "sí", "hola", "adiós", "gracias", "ay", "bueno", "vale", nullptr};
        // a word with a Greek rendering is translated, not kept: the answer words (yes, hello ...) always, others
        // when the reverse index of greek.vpl knows them ("mu", "miau", "hooray" have none: kept as written)
        std::vector<lex::Candidate> gk;
        lx.reverse(es ? "es:" + text::es_key(w) : text::en_key(w), gk);
        bool listed = false;
        for (const char* const* x = kWords; *x; ++x) listed = listed || w == *x;
        if (listed && !gk.empty()) { okWords = false; break; }
        std::vector<lex::Analysis> an;
        src->lookup(es ? text::es_key(w) : text::en_key(w), an);
        bool intj = an.empty() || gk.empty();
        for (const lex::Analysis& a : an) intj = intj || src->lemma(a.lemma).pos == feat::Intj;
        okWords = okWords && intj;
      }
      const size_t end = e + cl;
      const char* pron = es ? "eso" : "it";
      if (okWords && end - i >= std::strlen(pron)) {
        spans.emplace_back(i, end);
        for (size_t k = i; k < end; ++k) t[k] = ' ';
        for (size_t k = 0; pron[k]; ++k) t[i + k] = pron[k];
      }
      i = end - 1;
    }
  }

  // ---- one sentence ---------------------------------------------------------------------------------------------------
  void speechSplit(const std::string& text, const std::vector<size_t>& pts, const frame::FrameBuilder& fb,
                   const rules::Options& opt, transfer::Memory& mem, const transfer::Settings& st, SentOut& so) {
    std::vector<size_t> cuts = pts;
    cuts.push_back(text.size());
    size_t a = 0;
    for (size_t cut : cuts) {
      std::string part = text.substr(a, cut - a);
      while (!part.empty() && (part.back() == ' ' || part.back() == ',' || part.back() == ';')) part.pop_back();
      const size_t off = a;
      a = cut;
      if (part.empty()) continue;
      SentOut po;
      speech(part, fb, opt, mem, st, po, false, false);
      if (po.text.text.empty()) continue;
      if (!so.text.text.empty()) {
        stripFinal(so.text.text);
        so.text.text += "\xC2\xB7";
      }
      const int base = (int)so.text.tokens.size();
      cue::append(so.text, po.text);
      for (int o : po.srcOffset) so.srcOffset.push_back(o >= 0 ? o + (int)off : -1);
      for (Reason r : po.reasons) { if (r.tokenIndex >= 0) r.tokenIndex += base; so.reasons.push_back(r); }
      for (const std::string& f : po.flags) addFlag(so.flags, f);
      so.choices.insert(so.choices.end(), po.choices.begin(), po.choices.end());
      so.unknown.insert(so.unknown.end(), po.unknown.begin(), po.unknown.end());
      so.missing.insert(so.missing.end(), po.missing.begin(), po.missing.end());
      so.minMargin = std::min(so.minMargin, po.minMargin);
    }
    addFlag(so.flags, "frame-fallback");
    so.reasons.push_back(Reason{-1, "form", "the sentence was analysed in pieces (parser fallback): check the structure", ""});
  }

  void speech(const std::string& text, const frame::FrameBuilder& fb, const rules::Options& opt, transfer::Memory& mem,
              const transfer::Settings& st, SentOut& so, bool alternatives, bool allowSplit = true) {
    frame::SemSentence s;
    // C25: quoted sound words ("woof", "moo") are kept as written (see quotedSounds); the parser reads a pronoun there
    std::vector<std::pair<size_t, size_t>> soundSpans;
    std::string parseText = text;
    quotedSounds(parseText, st.lang == frame::SrcLang::Es, soundSpans);
    {
      // a sentence that is only a quoted sound ("Woof!"): copied as it is
      std::string rest = parseText;
      for (const auto& sp : soundSpans) for (size_t k = sp.first; k < sp.second; ++k) rest[k] = ' ';
      bool empty = !soundSpans.empty();
      for (char ch : rest) empty = empty && (ch == ' ' || ch == '.' || ch == '!' || ch == '?' || ch == ',');
      if (empty) {
        cue::Latin one;
        rules::TokenView t;
        std::string w = text;
        while (!w.empty() && w.front() == ' ') w.erase(w.begin());
        while (!w.empty() && w.back() == ' ') w.pop_back();
        t.text = t.display = w;
        t.start = 0;
        t.end = (int)w.size();
        one.text = w;
        one.tokens.push_back(t);
        so.text = one;
        so.srcOffset.push_back(0);
        so.reasons.push_back(Reason{-1, "form", "a sound word in quotes is kept as it is written: " + w, ""});
        return;
      }
    }
    const std::string& atext = soundSpans.empty() ? text : parseText;
    fb.analyse(atext, s);
    // C18: "When X, Y." read as a question or with X broken: X and Y analysed apart and merged (never OK: Check)
    bool whenRepaired = false;
    std::string repairWhat = "\"When ..., ...\" analysed in two parts (time clause + main clause): check it";
    // C29: shapes the shared frame builder misreads, analysed again on the Greek side (never OK: Check): an address
    // after or before a comma ("¿Por qué lloras, niña?"), a clause-first "ven" read as "they see", an irregular past
    // read as a noun or a name ("Mary wept for her brother."), a Spanish verb read as a noun ("El búho caza de noche."),
    // the generic "one" read as a numeral ("one sleeps")
    {
      const bool es = st.lang == frame::SrcLang::Es;
      auto analyseFixed = [&](const std::string& t, frame::SemSentence& o) {
        fb.analyse(t, o);
        frame::SemSentence r;
        std::string pf;
        if (es && venImperative(t, fb, o, r)) o = std::move(r);
        else if (es && queVerb(t, fb, o, esLex, r)) o = std::move(r);
        else if (!es && pastAsNoun(t, fb, o, enLex, r, pf)) { o = std::move(r); o.doubt("past-form"); }
      };
      frame::SemSentence r;
      std::string pf;
      if (addressSplit(atext, s, es, gd, analyseFixed, r)) {
        s = std::move(r);
        whenRepaired = true;
        repairWhat = "an address before or after a comma analysed apart as a vocative (ὦ ...): check it";
      } else if (es && venImperative(atext, fb, s, r)) {
        s = std::move(r);
        whenRepaired = true;
        repairWhat = "\"ven\" read as \"they see\": read as the command of venir: check it";
      } else if (!es && pastAsNoun(atext, fb, s, enLex, r, pf)) {
        s = std::move(r);
        s.doubt("past-form");
        whenRepaired = true;
        repairWhat = "\"" + pf + "\" read as a noun or a name: analysed again as a past verb: check it";
      } else if (!es && nounAsAdj(atext, fb, s, enLex, r, pf)) {
        s = std::move(r);
        whenRepaired = true;
        repairWhat = "\"" + pf + "\" read as an adjective: analysed again as the subject noun: check it";
      } else if (es && queVerb(atext, fb, s, esLex, r)) {
        s = std::move(r);
        whenRepaired = true;
        repairWhat = "\"qué\" + a verb read as a noun phrase: analysed again as a question: check it";
      } else if (es && esVerbAsNoun(atext, fb, s, esLex, r)) {
        s = std::move(r);
        whenRepaired = true;
        repairWhat = "a verb read as a noun in a sentence without a verb: analysed again as the verb: check it";
      } else if (!es && oneGeneric(atext, fb, s, r)) {
        s = std::move(r);
        whenRepaired = true;
        repairWhat = "the generic \"one\" read as a number: analysed again as a subject (τις): check it";
      }
    }
    if (!whenRepaired) {
      bool fine = false;
      for (const frame::Unit& u : s.units)
        if (u.type == frame::Unit::Clause)
          for (const frame::SemSub& sb : u.frame.subordinate)
            fine = fine || (sb.relation == frame::Relation::Time && sb.before && !sb.frame.empty() &&
                            sb.frame[0].hasPred && sb.frame[0].type == frame::Kind::Decl);
      frame::SemSentence merged;
      if (!fine && frontedWhen(atext, fb, merged)) {
        s = std::move(merged);
        whenRepaired = true;
      }
      // C21: "-ing phrase, S V." and "X so ADJ that Y." misread by the parser: two-part analysis (never OK)
      const bool es = st.lang == frame::SrcLang::Es;
      if (!whenRepaired && !es && frontedIng(atext, fb, s, merged)) {
        s = std::move(merged);
        whenRepaired = true;
        repairWhat = "\"-ing phrase, clause\" analysed in two parts (participle + main clause): check it";
      } else if (!whenRepaired && soThat(atext, fb, s, es, merged)) {
        s = std::move(merged);
        whenRepaired = true;
        repairWhat = "\"so ... that\" analysed in two parts (main clause + result clause): check it";
      } else if (!whenRepaired && !es && vocCoordinated(atext, fb, s, merged)) {   // C25
        s = std::move(merged);
        whenRepaired = true;
        repairWhat = "\"my friend(s) and ...\" read as an address: analysed again as a subject: check it";
      } else if (!whenRepaired && !es && whAuxiliary(atext, fb, s, merged)) {   // C25
        s = std::move(merged);
        whenRepaired = true;
        repairWhat = "a wh question analysed from its auxiliary on (yes / no question + the wh word): check it";
      } else if (!whenRepaired && es && nadaVerb(atext, fb, s, merged)) {   // C25
        s = std::move(merged);
        whenRepaired = true;
        repairWhat = "\"nada\" read as \"nothing\" in a sentence without a verb: read as the verb nadar: check it";
      } else if (!whenRepaired && es && familyFirst(atext, fb, s, merged)) {   // C31
        s = std::move(merged);
        whenRepaired = true;
        repairWhat = "\"papá / mamá\" read as a verb at the start: analysed again as the subject: check it";
      } else if (!whenRepaired && es && estarFragment(atext, fb, s, merged)) {   // C25
        s = std::move(merged);
        whenRepaired = true;
        repairWhat = "a subject with \"estar / ser\" read as a fragment: the verb phrase analysed alone: check it";
      }
    }
    // C31: Spanish clauses joined by a comma read as a time clause ("¡No corras, te vas a caer!" -> ὅτε οὐ τρέχεις)
    if (esParataxis(s)) {
      if (!whenRepaired) repairWhat = "two clauses joined by a comma read as a time clause: coordinated: check it";
      whenRepaired = true;
    }
    // C31: the article and the adjectives of a noun hung on the verb ("The old king died." -> βασιλεὺς ἀπέθανεν)
    if ((whenRepaired || !frame::FrameBuilder::troubled(s)) && lostModifiers(s)) {
      if (!whenRepaired) repairWhat = "an article or adjective the parser hung on the verb given back to its noun: check it";
      whenRepaired = true;
    }
    if (!whenRepaired && allowSplit && frame::FrameBuilder::troubled(s)) {
      const std::vector<size_t> pts = frame::FrameBuilder::splitPoints(text);
      if (!pts.empty()) { speechSplit(text, pts, fb, opt, mem, st, so); return; }
      s.repairs.emplace_back("no-verb");
    }
    mem.sawFirst = false;
    std::vector<int> covered;
    std::vector<std::string> flags;
    struct UnitText { cue::Latin text; std::vector<Reason> reasons; std::string sep; int srcStart = -1;
                      size_t choiceFrom = 0, choiceTo = 0; };
    std::vector<UnitText> units;
    std::string pendingParticle;
    for (size_t ui = 0; ui < s.units.size(); ++ui) {
      const frame::Unit& u = s.units[ui];
      UnitText ut;
      ut.sep = u.sepAfter;
      ut.srcStart = u.first < (int)s.tokens.size() ? s.tokens[(size_t)u.first].start : -1;
      ut.choiceFrom = so.choices.size();
      if (u.type == frame::Unit::Phrase) {
        const std::string pat = text::lower(u.phrase.pattern);
        // decision 2: "please" is left out in flexible mode
        if ((pat == "please" || pat == "por favor") && opt.fidelity >= 3) {
          for (int k = u.first; k <= u.last; ++k) covered.push_back(k);
          so.reasons.push_back(Reason{-1, "sense", "\"please\" left out (flexible mode: the command says it)", ""});
          if (!units.empty() && ui + 1 == s.units.size()) units.back().sep.clear();
          continue;
        }
        // a phrase that becomes a particle before a clause ("Of course we can talk" -> δυνάμεθα δήπου λαλεῖν)
        const size_t pp = u.phrase.note.find("before a clause: particle ");
        if (pp != std::string::npos && ui + 1 < s.units.size() && s.units[ui + 1].type == frame::Unit::Clause &&
            u.sepAfter.empty()) {
          std::string w = u.phrase.note.substr(pp + 26);
          const size_t sp = w.find_first_of(" (");
          if (sp != std::string::npos) w = w.substr(0, sp);
          pendingParticle = w;
          for (int k = u.first; k <= u.last; ++k) covered.push_back(k);
          so.reasons.push_back(Reason{-1, "phrasebook", u.phrase.pattern + " -> particle " + w, ""});
          continue;
        }
        renderPhrase(u.phrase, s, st, mem, opt, ut.text, ut.reasons, flags, so.choices, covered, so.unknown);
        for (const std::string& k : u.frame.connectors) {   // "Then go away." when the phrasebook took the clause
          const char* gk = k == "then" || k == "so" ? "οὖν" : k == "and" ? "καί" : k == "but" ? "ἀλλά" : nullptr;
          if (!gk || ut.text.tokens.empty()) continue;
          rules::TokenView t;
          tokenInfo(gk, t);
          const bool second = std::string(gk) == "οὖν";
          std::vector<std::string> ws;
          cue::Latin merged, w;
          w.text = t.text;
          t.start = 0;
          t.end = (int)w.text.size();
          w.tokens.push_back(t);
          if (second) {
            cue::Latin first, tail;
            const rules::TokenView& t0 = ut.text.tokens[0];
            size_t rest = (size_t)t0.end;
            while (rest < ut.text.text.size() && ut.text.text[rest] != ' ') ++rest;
            first.text = ut.text.text.substr(0, rest);
            first.tokens.push_back(t0);
            if (rest < ut.text.text.size()) {
              const size_t b = rest + 1;
              tail.text = ut.text.text.substr(b);
              for (size_t i = 1; i < ut.text.tokens.size(); ++i) {
                rules::TokenView x = ut.text.tokens[i];
                x.start -= (int)b;
                x.end -= (int)b;
                tail.tokens.push_back(x);
              }
            }
            cue::append(merged, first);
            cue::append(merged, w);
            cue::append(merged, tail);
            for (Reason& r : ut.reasons)
              if (r.tokenIndex >= 1) ++r.tokenIndex;
          } else {
            cue::append(merged, w);
            cue::append(merged, ut.text);
            for (Reason& r : ut.reasons)
              if (r.tokenIndex >= 0) ++r.tokenIndex;
          }
          ut.text = merged;
          covered.push_back(u.first);
        }
      } else {
        GrcClauseOut co;
        if (u.vocative) xfer->vocative(u.frame.subject, s, st, mem, co);
        else xfer->clause(u.frame, s, st, mem, co);
        if (!pendingParticle.empty()) {
          uint32_t id = findLemma(lx, pendingParticle, feat::Particle);
          if (id == kNone) id = findLemma(lx, pendingParticle);
          if (id != kNone) co.clause.connectors.push_back(id);
          pendingParticle.clear();
        }
        realiseClause(co.clause, opt, ut.text, ut.reasons, flags);
        so.choices.insert(so.choices.end(), co.choices.begin(), co.choices.end());
        covered.insert(covered.end(), co.covered.begin(), co.covered.end());
        for (const std::string& f : co.flags) addFlag(flags, f);
        for (const Reason& r : co.notes) so.reasons.push_back(r);
        std::vector<std::string> unk = co.unknownWords;
        std::vector<std::string> tx;
        bool changed = false;
        for (rules::TokenView& t : ut.text.tokens) {
          tx.push_back(t.text);
          if (t.text == "[verb]" || t.text == "[?]") {
            for (const transfer::Choice& ch : co.choices)
              if (ch.unknown) { tx.back() = "[" + ch.source + "]"; break; }
            t.unknown = true;
            changed = true;
          }
          if (t.text.size() > 2 && t.text.front() == '[') t.unknown = true;
        }
        if (changed) rewriteTokens(ut.text, tx);
        for (const std::string& w : unk) {
          bool shown = ut.text.text.find("[" + w + "]") != std::string::npos;
          if (shown) continue;
          cue::Latin x;
          rules::TokenView t;
          t.text = t.display = "[" + w + "]";
          t.unknown = true;
          t.start = 0;
          t.end = (int)t.text.size();
          x.text = t.text;
          x.tokens.push_back(t);
          cue::append(ut.text, x);
        }
        so.unknown.insert(so.unknown.end(), unk.begin(), unk.end());
      }
      ut.choiceTo = so.choices.size();
      if (!ut.text.text.empty()) units.push_back(std::move(ut));
    }
    // join the units with the source separators (";" and ":" become the ano teleia)
    cue::Latin& L = so.text;
    for (size_t i = 0; i < units.size(); ++i) {
      UnitText& ut = units[i];
      const int base = (int)L.tokens.size();
      cue::append(L, ut.text);
      std::vector<char> usedChoice(so.choices.size(), 0);
      for (const rules::TokenView& t : ut.text.tokens) {
        int off = ut.srcStart;
        if (t.hasLemma)
          for (size_t k = ut.choiceFrom; k < ut.choiceTo; ++k)
            if (!usedChoice[k] && so.choices[k].lemma == t.lemmaId && so.choices[k].token >= 0 &&
                (size_t)so.choices[k].token < s.tokens.size()) {
              usedChoice[k] = 1;
              off = s.tokens[(size_t)so.choices[k].token].start;
              break;
            }
        so.srcOffset.push_back(off);
      }
      for (Reason r : ut.reasons) {
        if (r.tokenIndex >= 0) r.tokenIndex += base;
        so.reasons.push_back(r);
      }
      if (i + 1 < units.size()) L.text += greekSeparator(ut.sep);
    }
    if (whenRepaired) {
      addFlag(flags, "clause-repair");
      so.reasons.push_back(Reason{-1, "form", repairWhat, ""});
    }
    // C25: a Spanish sentence read as a verbless fragment with a subject and a prepositional phrase or an "of"
    // attribute ("El niño nada en el río." was "the boy of nothing in the river", OK) is a misreading: never OK
    if (!whenRepaired && st.lang == frame::SrcLang::Es && s.units.size() == 1 && s.units[0].type == frame::Unit::Clause) {
      const frame::SemFrame& f0 = s.units[0].frame;
      if (f0.type == frame::Kind::Frag && !f0.hasPred && f0.hasSubject &&
          (!f0.obliques.empty() || !f0.subject.genitive.empty())) {
        addFlag(flags, "fragment");
        so.reasons.push_back(Reason{-1, "form", "a sentence without a verb (subject + phrase): check the analysis", ""});
      }
    }
    // C18: a wh question frame in a sentence that is not a question (an interrogative word in a statement): Check
    if (s.finalPunct.find('?') == std::string::npos)
      for (const frame::Unit& u : s.units)
        if (u.type == frame::Unit::Clause && u.frame.type == frame::Kind::Wh) {
          addFlag(flags, "wh-statement");
          so.reasons.push_back(Reason{-1, "form", "an interrogative word in a statement: check the structure", ""});
          break;
        }
    if (!s.repairs.empty()) {
      std::string what;
      for (const std::string& r : s.repairs) what += (what.empty() ? "" : ", ") + r;
      addFlag(flags, "frame-fallback");
      so.reasons.push_back(Reason{-1, "form", "sentence analysis fallback (" + what + "): check the structure", ""});
    }
    // C18: constructions the frame builder rendered by a rule of thumb (frame.h SemSentence::doubts, as the Latin path):
    // never OK; a light verb or a noun + infinitive that a curated Greek row handled (lexical_en_grc.tsv kind light,
    // valency purp:inf) is not a guess any more
    for (const std::string& d : s.doubts) {
      bool handled = false;
      for (const transfer::Choice& c : so.choices)
        handled = handled || (d == "light-verb" && c.note.find("light verb") != std::string::npos) ||
                  (d == "noun-infinitive" && c.note.find("purp:inf") != std::string::npos);
      if (handled) continue;
      addFlag(flags, d);
      so.reasons.push_back(Reason{-1, "form", "construction rendered by a rule of thumb (" + d + "): check it", ""});
    }
    std::string fp = s.finalPunct;
    if (fp.empty() && frame::endsSentence(text)) fp = ".";
    if (fp == "\xE2\x80\xA6") fp = "...";
    if (fp.find('?') != std::string::npos) fp = ";";   // the Greek question mark
    else if (fp.find('!') != std::string::npos) fp = "!";
    else if (fp == "." && s.units.size() == 1 && s.units[0].type == frame::Unit::Clause &&
             s.units[0].frame.type == frame::Kind::Excl)
      fp = "!";   // order.excl: "ὡς θαυμαστὸς ὁ κῆπος!"
    stripFinal(L.text);
    if (!L.text.empty()) L.text += fp;
    // decision 5: a capital at the start of the sentence
    if (!L.tokens.empty()) {
      std::vector<std::string> tx;
      for (const auto& t : L.tokens) tx.push_back(t.text);
      tx[0] = capitaliseGreek(tx[0]);
      rewriteTokens(L, tx);
      L.tokens[0].display = L.tokens[0].text;
    }
    // C25: the quoted sound words back in place of the pronoun read for them: the n-th placeholder ("it" / "eso") of
    // the parsed text is the n-th Greek token of αὐτός / οὗτος when the counts agree, else the quoted word goes before
    // the final mark
    if (!soundSpans.empty()) {
      const bool es = st.lang == frame::SrcLang::Es;
      const uint32_t pl = es ? findLemma(lx, "οὗτος") : findLemma(lx, "αὐτός");
      const uint32_t pl2 = es ? findLemma(lx, "ἐκεῖνος") : kNone;   // "eso" -> ἐκεῖνο
      const std::string ph = es ? "eso" : "it";
      std::vector<size_t> pronTokens, phWords;
      for (size_t q = 0; q < L.tokens.size(); ++q)
        if (L.tokens[q].hasLemma && ((L.tokens[q].lemmaId == pl && pl != kNone) || (L.tokens[q].lemmaId == pl2 && pl2 != kNone)))
          pronTokens.push_back(q);
      const std::string low = text::lower(atext);
      for (size_t q = low.find(ph); q != std::string::npos; q = low.find(ph, q + 1))
        if ((q == 0 || !std::isalpha((unsigned char)low[q - 1])) &&
            (q + ph.size() >= low.size() || !std::isalpha((unsigned char)low[q + ph.size()])))
          phWords.push_back(q);
      for (const auto& sp : soundSpans) {
        const std::string quoted = text.substr(sp.first, sp.second - sp.first);
        long k = -1;
        if (pronTokens.size() == phWords.size())
          for (size_t w = 0; w < phWords.size(); ++w)
            if (phWords[w] == sp.first) k = (long)pronTokens[w];
        if (k >= 0) {
          std::vector<std::string> tx;
          for (const auto& t : L.tokens) tx.push_back(t.text);
          tx[(size_t)k] = quoted;
          rewriteTokens(L, tx);
          rules::TokenView& t = L.tokens[(size_t)k];
          t.display = quoted;
          t.hasLemma = false;
          t.lemmaId = 0;
          t.features = rules::Features{};
          t.tier = 0;
          t.emoji.clear();
          t.unknown = false;
        } else {
          std::string fpx;
          while (!L.text.empty() && (L.text.back() == '.' || L.text.back() == '!' || L.text.back() == ';' ||
                                     L.text.back() == '?'))
            { fpx.insert(fpx.begin(), L.text.back()); L.text.pop_back(); }
          cue::Latin one;
          rules::TokenView t;
          t.text = t.display = quoted;
          t.start = 0;
          t.end = (int)quoted.size();
          one.text = quoted;
          one.tokens.push_back(t);
          cue::append(L, one);
          so.srcOffset.push_back((int)sp.first);
          L.text += fpx;
        }
        for (size_t i = 0; i < s.tokens.size(); ++i)
          if (s.tokens[i].start >= (int)sp.first && s.tokens[i].start < (int)sp.second) covered.push_back((int)i);
        so.reasons.push_back(Reason{-1, "form", "a sound word in quotes is kept as it is written: " + quoted, ""});
      }
    }
    for (const std::string& f : flags) addFlag(so.flags, f);
    // A7: source coverage
    std::sort(covered.begin(), covered.end());
    // C25: a word between quotes that is not a sound word ("the word "love"") is never dropped silently: missing
    for (size_t i = 0; i < s.tokens.size(); ++i) {
      bool rendered = false;   // a Greek word was chosen for this token (covered alone is not enough: "the word "love"")
      for (const transfer::Choice& ch : so.choices) rendered = rendered || (ch.token == (int)i && ch.lemma != kNone);
      if (rendered) continue;
      const nlp::Token& t = s.tokens[i];
      if (t.upos == "PUNCT" || t.start <= 0 || (size_t)t.end >= atext.size()) continue;
      auto quoteAt = [&](size_t p) { return atext[p] == '"' || (p >= 2 && (unsigned char)atext[p] == 0x9D) ||
                                            (p >= 2 && (unsigned char)atext[p] == 0x9C); };
      if (quoteAt((size_t)t.start - 1) && quoteAt((size_t)t.end) &&
          std::find(so.missing.begin(), so.missing.end(), t.text) == so.missing.end())
        so.missing.push_back(t.text);
    }
    for (size_t i = 0; i < s.tokens.size(); ++i) {
      const nlp::Token& t = s.tokens[i];
      const bool content = t.upos == "NOUN" || t.upos == "PROPN" || t.upos == "VERB" || t.upos == "ADJ" ||
                           t.upos == "ADV" || t.upos == "NUM" || t.upos == "PRON";
      if (!content || s.drop[i] != frame::Drop::No) continue;
      if (std::binary_search(covered.begin(), covered.end(), (int)i)) continue;
      if (t.upos == "ADV" && (t.lower == "not" || t.lower == "n't" || t.lower == "no")) continue;
      so.missing.push_back(t.text);
    }
    // C21: a degree word (such, so / too / very + adjective or adverb) the Greek does not render is never silently
    // dropped: it counts as missing (A7, Check) whatever its tag or the frame builder's drop mark
    for (size_t i = 0; i < s.tokens.size(); ++i) {
      const std::string& w = s.tokens[i].lower;
      bool degree = w == "such" || w == "tal" || w == "semejante";
      if (!degree && i + 1 < s.tokens.size() &&
          (w == "so" || w == "too" || w == "very" || w == "tan" || w == "muy" || w == "demasiado"))
        degree = s.tokens[i + 1].upos == "ADJ" || s.tokens[i + 1].upos == "ADV";
      if (!degree || std::binary_search(covered.begin(), covered.end(), (int)i)) continue;
      if (std::find(so.missing.begin(), so.missing.end(), s.tokens[i].text) == so.missing.end())
        so.missing.push_back(s.tokens[i].text);
    }
    for (const transfer::Choice& c : so.choices)
      if (c.kind == "sense" && c.candidates.size() > 1) so.minMargin = std::min(so.minMargin, c.margin);
    mem.prevFirst = mem.sawFirst;
    if (alternatives) {
      const transfer::Choice* amb = nullptr;
      for (const transfer::Choice& c : so.choices)
        if (c.kind == "sense" && c.candidates.size() > 1 && c.token >= 0 && (!amb || c.margin < amb->margin)) amb = &c;
      if (amb && amb->margin < 0.3) {
        transfer::Memory m2 = memBefore;
        transfer::Settings st2 = st;
        st2.overrides.push_back({amb->token, 1});
        SentOut alt;
        speech(text, fb, opt, m2, st2, alt, false);
        if (alt.text.text != so.text.text) {
          const lex::Lemma l = lx.lemma(amb->candidates[1].lemma);
          so.alternatives.push_back(Alternative{alt.text.text, "second sense of \"" + amb->source + "\": " +
                                                                   display(l.head), std::max(0.0, 1.0 - amb->margin)});
        }
      }
      // C16: weekday names: the ordinal counted from Sunday is the alternative ("τρίτη ἡμέρα" for Ἄρεως ἡμέρα)
      if (std::find(so.flags.begin(), so.flags.end(), "weekday") != so.flags.end()) {
        transfer::Memory m2 = memBefore;
        SentOut alt;
        xfer->setWeekdayOrdinal(true);
        speech(text, fb, opt, m2, st, alt, false);
        xfer->setWeekdayOrdinal(false);
        if (alt.text.text != so.text.text)
          so.alternatives.push_back(Alternative{alt.text.text, "day name as an ordinal (counted from Sunday)", 0.5});
      }
      bool genderWords = std::find(so.flags.begin(), so.flags.end(), "speaker-gender") != so.flags.end();
      for (const auto& t : so.text.tokens)
        genderWords = genderWords || (t.features.pos == "adj" && !t.features.gender.empty() && t.features.case_ == "nom");
      if (opt.speakerGender == 'u' && genderWords) {
        transfer::Memory m2 = memBefore;
        transfer::Settings st2 = st;
        st2.flipSpeakerGender = true;
        SentOut alt;
        speech(text, fb, opt, m2, st2, alt, false);
        if (alt.text.text != so.text.text)
          so.alternatives.insert(so.alternatives.begin(), Alternative{alt.text.text, "speaker: feminine", 0.5});
        addFlag(so.flags, "speaker-gender");
      }
    }
  }

  // ---- checks -------------------------------------------------------------------------------------------------------
  void runChecks(const std::string& target, const std::vector<rules::TokenView>& tokens, const rules::Options& opt,
                 const CueInput& in, bool greekText, CueOutput& o, bool overflow, bool tagsApprox, bool& warn) {
    o.checks.clear();
    warn = false;
    if (greekText) {
      check::GreekCheckOptions co;
      co.tierCeiling = (uint8_t)(opt.fidelity >= 3 ? 1 : opt.fidelity == 2 ? 2 : 3);
      co.glossary = glossary;
      std::vector<check::TokenHint> hints;
      for (const rules::TokenView& t : tokens) {
        if (t.start < 0) continue;
        check::TokenHint h;
        h.start = t.start;
        h.end = t.end;
        h.lemma = t.hasLemma ? t.lemmaId : lex::kNoLemma;
        h.fromRule = t.fromRule;
        h.name = !t.hasLemma && !t.unknown && !t.text.empty() && t.text[0] != '[';
        if (t.hasLemma && (lx.lemma(t.lemmaId).flags & lex::ProperName)) h.name = true;
        hints.push_back(h);
      }
      for (const Reason& r : o.reasons)
        if (r.kind == "name" && r.tokenIndex >= 0 && (size_t)r.tokenIndex < tokens.size())
          for (check::TokenHint& h : hints)
            if (h.start == tokens[(size_t)r.tokenIndex].start) h.name = true;
      co.hints = &hints;
      check::GrcReport rep;
      std::string flat = target;
      std::replace(flat.begin(), flat.end(), '\n', ' ');
      checker->check(flat, co, rep);
      for (const Check& c : rep.checks) {
        o.checks.push_back(c);
        if (c.ok && c.detail.compare(0, 8, "warning:") == 0 && (c.id == "A1" || c.id == "A1b")) warn = true;
      }
      if (rep.fromRule) addFlag(o.flags, "from-rule");
    } else {
      for (const char* id : {"A1", "A1b", "A3", "A4", "A6"}) o.checks.push_back(Check{id, true, "not Greek text (copied)"});
    }
    {
      Check a5{"A5", true, ""};
      int sq = 0, rd = 0;
      for (char ch : target) {
        if (ch == '<' || ch == '{') { a5.ok = false; a5.detail = "stray markup in the text"; }
        if (ch == '[') ++sq;
        if (ch == ']') --sq;
        if (ch == '(') ++rd;
        if (ch == ')') --rd;
      }
      if (sq != 0 || rd != 0) { a5.ok = false; a5.detail = "unbalanced brackets"; }
      if (tagsApprox) {
        a5.detail += std::string(a5.ok ? "" : "; ") + "tag position approximated";
        a5.ok = false;
      }
      o.checks.push_back(a5);
    }
    {
      Check a8{"A8", true, ""};
      double cps = 0;
      if (in.endMs > in.startMs) {
        std::string flat = target;
        std::replace(flat.begin(), flat.end(), '\n', ' ');
        cps = (double)subs::visibleLength(flat) / ((double)(in.endMs - in.startMs) / 1000.0);
      }
      if (cps > cfg.cpsLimit) {
        a8.ok = false;
        a8.detail = "reading speed " + fmt(cps) + " cps > " + fmt(cfg.cpsLimit);
        addFlag(o.flags, "cps");
      }
      if (overflow) {
        a8.ok = false;
        a8.detail += std::string(a8.detail.empty() ? "" : "; ") + "more than " + std::to_string(cfg.maxLines) +
                     " lines of " + std::to_string(cfg.maxLine);
        addFlag(o.flags, "overflow");
      }
      o.checks.push_back(a8);
    }
    // A9 (C16, decision 1): round trip through grc2x, overlap of the source's content lemmas with the glosses of the
    // Greek readings; < 0.5 -> Check
    {
      std::vector<std::string> src;
      src.swap(a9Sources);
      const bool set = a9Set;
      a9Set = false;
      if (!set) o.checks.push_back(Check{"A9", true, "not run for edited cues (no source lemmas)"});
      else if (!greekText) o.checks.push_back(Check{"A9", true, "not Greek text"});
      else if (src.empty()) o.checks.push_back(Check{"A9", true, "no source content words"});
      else {
        std::string flat = target;
        std::replace(flat.begin(), flat.end(), '\n', ' ');
        const double ov = back->roundTripOverlap(flat, src, opt.source == rules::Lang::Es ? grc2x::Target::Es
                                                                                          : grc2x::Target::En);
        o.checks.push_back(Check{"A9", ov >= 0.5, "round-trip overlap " + fmt(ov)});
      }
    }
    std::stable_sort(o.checks.begin(), o.checks.end(), [](const Check& x, const Check& y) { return x.id < y.id; });
  }

  // ---- translate -----------------------------------------------------------------------------------------------------
  Result<std::vector<CueOutput>> translate(const std::vector<CueInput>& cues, const rules::Options& opt,
                                           const rules::Context& ctx, const std::function<void(size_t)>& progress,
                                           const std::function<bool()>& cancelled) {
    std::vector<CueOutput> outs;
    if (cues.empty()) return Result<std::vector<CueOutput>>(std::move(outs));
    const frame::SrcLang lang = opt.source == rules::Lang::Es ? frame::SrcLang::Es : frame::SrcLang::En;
    const frame::FrameBuilder* fbp = builder(lang);
    if (!fbp)
      return Result<std::vector<CueOutput>>(ErrorCode::LexiconMissing, "nlp models not available for the source",
                                            "The language analysis files (english/spanish .tag.vpt and .dep.vpt) are "
                                            "missing from the data folder. Reinstall the app or point VP_NLP_DIR at them.");
    const frame::FrameBuilder& fb = *fbp;
    glossary = &ctx.glossary;
    std::vector<std::string> texts;
    const bool prev = !cues.front().prevSource.empty();
    const bool next = !cues.back().nextSource.empty() && !frame::endsSentence(cues.back().sourceText);
    if (prev) texts.push_back(cues.front().prevSource);
    for (const CueInput& c : cues) texts.push_back(c.sourceText);
    if (next) texts.push_back(cues.back().nextSource);
    const size_t off = prev ? 1 : 0;
    const std::vector<frame::SourceSentence> sents = frame::mapSentences(texts);
    std::vector<long> lastSentence(texts.size(), -1);
    for (size_t si = 0; si < sents.size(); ++si)
      for (const frame::CuePart& p : sents[si].parts) lastSentence[p.cue] = (long)si;
    struct CueAcc {
      cue::Latin text;
      std::vector<Reason> reasons;
      std::vector<std::string> flags, unknown, missing;
      std::vector<transfer::Choice> choices;
      double minMargin = 1.0;
      bool nonverbal = false, song = false, copied = false, greekText = false;
      int sentences = 0, wholeSentences = 0;
      std::vector<Alternative> alternatives;
    };
    std::vector<CueAcc> acc(texts.size());
    transfer::Memory mem;
    royalGender = 0;
    transfer::Settings st;
    st.lang = lang;
    st.fidelity = std::max(1, std::min(3, opt.fidelity));
    st.speakerGender = opt.speakerGender == 'f' ? 'f' : opt.speakerGender == 'u' ? 'u' : 'm';
    st.context = &ctx;
    st.srcLex = lang == frame::SrcLang::Es ? esLex : enLex;
    size_t reported = 0, doneCues = 0;
    bool stopped = false;
    long lastCueSeen = -1;
    for (size_t si = 0; si < sents.size(); ++si) {
      if (cancelled && cancelled()) { stopped = true; break; }
      const frame::SourceSentence& ss = sents[si];
      const long firstCue = ss.parts.empty() ? 0 : (long)ss.parts.front().cue;
      if (firstCue != lastCueSeen) {
        if (lastCueSeen >= 0) { mem.addresseePlural = mem.sawPlural; mem.sawPlural = false; }
        mem.addresseeGuess = false;
        lastCueSeen = firstCue;
      }
      mem.answerWe = false;
      if (ss.kind == frame::CueKind::Speech && si + 1 < sents.size() && sents[si + 1].kind == frame::CueKind::Speech) {
        const std::string low = " " + text::lower(ss.text) + " ";
        const std::string nx = text::lower(sents[si + 1].text);
        bool you = false;
        for (const char* w : {" you ", " you?", " you.", " you!", " you,", " your "}) you = you || low.find(w) != std::string::npos;
        mem.answerWe = you && (nx.rfind("we ", 0) == 0 || nx.rfind("we'", 0) == 0) && lang == frame::SrcLang::En;
      }
      SentOut so;
      memBefore = mem;
      if (ss.kind == frame::CueKind::Nonverbal) {
        so.nonverbal = true;
        so.copied = true;
        so.text.text = ss.prefix + ss.text + ss.suffix;
        addFlag(so.flags, "nonverbal");
      } else {
        speech(ss.text, fb, opt, mem, st, so, true);
        noteRoyal(so.choices);
        if (ss.kind == frame::CueKind::Song) {
          so.song = true;
          addFlag(so.flags, "song");
          const int shift = (int)ss.prefix.size();
          so.text.text = ss.prefix + so.text.text + ss.suffix;
          for (auto& t : so.text.tokens) { t.start += shift; t.end += shift; }
        }
      }
      if (so.srcOffset.size() != so.text.tokens.size()) so.srcOffset.assign(so.text.tokens.size(), -1);
      if (ss.kind == frame::CueKind::Song) so.srcOffset.assign(so.text.tokens.size(), -1);
      const std::vector<cue::Latin> pieces = cue::splitSentence(ss, so.text, &so.srcOffset);
      size_t tokBase = 0;
      for (size_t p = 0; p < pieces.size() && p < ss.parts.size(); ++p) {
        CueAcc& a = acc[ss.parts[p].cue];
        const int cueBase = (int)a.text.tokens.size();
        cue::append(a.text, pieces[p]);
        for (const Reason& rr : so.reasons) {
          if (rr.tokenIndex < 0) { if (p == 0) a.reasons.push_back(rr); continue; }
          if ((size_t)rr.tokenIndex >= tokBase && (size_t)rr.tokenIndex < tokBase + pieces[p].tokens.size()) {
            Reason x = rr;
            x.tokenIndex = cueBase + (int)((size_t)rr.tokenIndex - tokBase);
            a.reasons.push_back(x);
          }
        }
        tokBase += pieces[p].tokens.size();
        for (const std::string& f : so.flags) addFlag(a.flags, f);
        a.unknown.insert(a.unknown.end(), so.unknown.begin(), so.unknown.end());
        a.missing.insert(a.missing.end(), so.missing.begin(), so.missing.end());
        a.choices.insert(a.choices.end(), so.choices.begin(), so.choices.end());
        a.minMargin = std::min(a.minMargin, so.minMargin);
        a.nonverbal = a.nonverbal || so.nonverbal;
        a.song = a.song || so.song;
        a.copied = a.copied || so.copied;
        a.greekText = a.greekText || !so.copied;
        ++a.sentences;
        if (ss.parts.size() == 1) {
          ++a.wholeSentences;
          for (const Alternative& alt : so.alternatives) a.alternatives.push_back(alt);
        }
        if (mem.addresseeGuess) addFlag(a.flags, "addressee-guess");
      }
      while (doneCues < cues.size() && lastSentence[off + doneCues] <= (long)si) ++doneCues;
      if (progress && doneCues > reported) {
        reported = doneCues;
        progress(doneCues);
      }
    }
    if (stopped) {
      size_t complete = 0;
      while (complete < cues.size() && lastSentence[off + complete] >= 0 &&
             (size_t)lastSentence[off + complete] < sents.size() && complete < doneCues)
        ++complete;
      doneCues = complete;
    } else {
      doneCues = cues.size();
    }
    outs.reserve(doneCues);
    for (size_t i = 0; i < doneCues; ++i) {
      CueAcc& a = acc[off + i];
      const std::string ckey = lang == frame::SrcLang::Es ? text::es_key(cues[i].sourceText) : text::en_key(cues[i].sourceText);
      const rules::Correction* corr = nullptr;
      for (const rules::Correction& c : ctx.corrections)
        if (c.sourceKey == ckey && !c.target.empty()) corr = &c;
      if (corr) {
        CueOutput o = check(cues[i], corr->target, opt, ctx);
        glossary = &ctx.glossary;
        o.index = cues[i].index;
        o.target = greekLayout(corr->target, cfg.maxLine, cfg.maxLines).joined;
        cue::relocate(o.target, o.tokens);
        o.reasons.push_back(Reason{-1, "correction", "your correction (" + corr->scope + ", used " +
                                                         std::to_string(corr->count) + " times)", corr->target});
        addFlag(o.flags, "correction");
        outs.push_back(std::move(o));
        continue;
      }
      CueOutput o;
      o.index = cues[i].index;
      const Layout lay = greekLayout(a.text.text, cfg.maxLine, cfg.maxLines);
      o.target = lay.joined;
      o.tokens = a.text.tokens;
      cue::relocate(o.target, o.tokens);
      o.reasons = a.reasons;
      o.flags = a.flags;
      if (a.text.text.empty() && !cues[i].sourceText.empty()) addFlag(o.flags, "merged");
      for (const auto& t : o.tokens)
        if (!t.emoji.empty()) { addFlag(o.flags, "emoji"); break; }
      if (std::find(o.flags.begin(), o.flags.end(), "name-guessed") != o.flags.end()) addFlag(o.flags, "unknownName");
      std::vector<char> used(o.tokens.size(), 0);
      for (const transfer::Choice& c : a.choices) {
        int ti = -1;
        for (size_t k = 0; k < o.tokens.size(); ++k)
          if (!used[k] && o.tokens[k].hasLemma && o.tokens[k].lemmaId == c.lemma && c.lemma != kNone) {
            ti = (int)k;
            used[k] = 1;
            break;
          }
        if (c.lemma == kNone && !c.unknown) continue;
        if (c.unknown) {
          o.reasons.push_back(Reason{ti, "sense", "\"" + c.source + "\": no Greek word in the dictionary (kept in brackets)", ""});
          continue;
        }
        const std::string head = display(lx.lemma(c.lemma).head);
        const std::string kind = c.kind == "table" || c.kind == "realia" ? "sense" : c.kind;
        std::string txt = "\"" + c.source + "\" -> " + head;
        if (c.kind == "table") txt += " (closed-class table)";
        if (!c.note.empty()) txt += " (" + c.note + ")";
        if (c.kind == "sense" && !c.candidates.empty()) txt += " (score " + fmt(c.candidates[0].score) + ")";
        o.reasons.push_back(Reason{ti, kind, txt, ""});
        if (!c.candidates.empty()) {
          std::string data = "[";
          for (size_t k = 0; k < c.candidates.size(); ++k) {
            const lex::Lemma l = lx.lemma(c.candidates[k].lemma);
            if (k) data += ",";
            data += "{\"lemma\":" + std::to_string(c.candidates[k].lemma) + ",\"head\":\"" +
                    jsonEscape(display(l.head)) + "\",\"score\":" + fmt(c.candidates[k].score) + ",\"why\":\"" +
                    jsonEscape(c.candidates[k].why) + "\"}";
          }
          data += "]";
          o.reasons.push_back(Reason{ti, "candidate", "candidates for \"" + c.source + "\"", data});
        }
      }
      if (opt.useModel) o.reasons.push_back(Reason{-1, "evidence", "model: not used for Greek", ""});
      if (opt.useOnline) o.reasons.push_back(Reason{-1, "evidence", "online: not used for Greek", ""});
      bool tagsApprox = false;
      if (!cues[i].spans.empty()) {
        std::vector<subs::Span> sp;
        bool anyTag = false;
        for (const rules::SpanIn& x : cues[i].spans) {
          subs::Span y;
          y.kind = x.tag ? subs::Span::Tag : subs::Span::Text;
          y.raw = x.raw;
          anyTag = anyTag || x.tag;
          sp.push_back(std::move(y));
        }
        const cue::TagResult tr = cue::applyTags(sp, o.target);
        tagsApprox = tr.approximated;
        if (tagsApprox) {
          addFlag(o.flags, "tags-approximated");
          o.reasons.push_back(Reason{-1, "form", "a tag inside the source text could not be placed in the Greek: check it", ""});
        } else if (anyTag) {
          addFlag(o.flags, "tags");
        }
      }
      bool warn = false;
      a9Sources.clear();
      for (const transfer::Choice& c : a.choices)
        if (c.kind != "table" && !c.source.empty()) a9Sources.push_back(c.source);
      a9Set = true;
      runChecks(o.target, o.tokens, opt, cues[i], a.greekText && !a.copied, o, lay.overflow, tagsApprox, warn);
      {
        Check a7{"A7", true, ""};
        std::vector<std::string> miss = a.missing;
        std::sort(miss.begin(), miss.end());
        miss.erase(std::unique(miss.begin(), miss.end()), miss.end());
        if (!miss.empty()) {
          a7.ok = false;
          a7.detail = "not accounted for:";
          for (const std::string& w : miss) a7.detail += " " + w;
        }
        o.checks.push_back(a7);
        std::stable_sort(o.checks.begin(), o.checks.end(), [](const Check& x, const Check& y) { return x.id < y.id; });
      }
      bool unknown = !a.unknown.empty();
      for (const auto& t : o.tokens) unknown = unknown || t.unknown;
      bool a5fix = false;
      for (const Check& k : o.checks)
        if (k.id == "A5" && !k.ok && k.detail != "tag position approximated") a5fix = true;
      const bool fix = !checkOk(o, "A1") || !checkOk(o, "A3") || !checkOk(o, "A4") || unknown || a5fix;
      bool chk = warn || !checkOk(o, "A1b") || !checkOk(o, "A5") || !checkOk(o, "A6") || !checkOk(o, "A7") ||
                 !checkOk(o, "A8") || !checkOk(o, "A9") || a.minMargin < 0.15 || a.song || a.nonverbal;
      for (const char* f : {"name-guessed", "from-rule", "addressee-guess", "missing-form", "merged", "frame-fallback",
                            "realia", "name-kept", "contact-relative", "noun-infinitive", "purpose-guess", "light-verb",
                            "phrase-order", "participle-phrase", "ellipsis", "clause-repair", "wh-statement", "det-adverb",
                            "past-form", "subject-guess", "fragment", "free-relative", "speech-inversion", "idiom"})
        if (std::find(o.flags.begin(), o.flags.end(), f) != o.flags.end()) chk = true;
      for (const transfer::Choice& c : a.choices)
        if (c.lowTier) {
          chk = true;
          addFlag(o.flags, "low-tier");
          o.reasons.push_back(Reason{-1, "sense", "\"" + c.source + "\": a rarer word was chosen although a core word "
                                                  "of the same sense exists", ""});
          break;
        }
      if (opt.speakerGender == 'u' && std::find(o.flags.begin(), o.flags.end(), "speaker-gender") != o.flags.end())
        chk = true;
      o.confidence = fix ? Confidence::Fix : chk ? Confidence::Check : Confidence::Ok;
      double score = 1.0;
      for (const transfer::Choice& c : a.choices)
        if (c.kind == "sense") score *= std::min(1.0, 0.6 + std::max(0.0, c.margin));
      if (fix) score *= 0.3;
      else if (chk) score *= 0.7;
      o.score = std::max(0.0, std::min(1.0, std::round(score * 1000) / 1000));
      if (a.wholeSentences == a.sentences && a.sentences == 1)
        for (const Alternative& alt : a.alternatives) {
          if (o.alternatives.size() >= 3) break;
          Alternative x = alt;
          x.score = std::round(x.score * 1000) / 1000;
          o.alternatives.push_back(x);
        }
      outs.push_back(std::move(o));
    }
    if (progress && outs.size() > reported) progress(outs.size());
    glossary = nullptr;
    return Result<std::vector<CueOutput>>(std::move(outs));
  }

  // ---- check ---------------------------------------------------------------------------------------------------------
  CueOutput check(const CueInput& in, const std::string& target, const rules::Options& opt, const rules::Context& ctx) {
    glossary = &ctx.glossary;
    CueOutput o;
    o.index = in.index;
    o.target = target;
    check::GreekCheckOptions co;
    co.tierCeiling = (uint8_t)(opt.fidelity >= 3 ? 1 : opt.fidelity == 2 ? 2 : 3);
    co.glossary = glossary;
    check::GrcReport rep;
    checker->check(target, co, rep);
    for (const check::GrcCheckedToken& ct : rep.tokens) {
      rules::TokenView t;
      t.text = t.display = ct.text;
      t.start = ct.start;
      t.end = ct.end;
      t.fromRule = ct.fromRule;
      if (!ct.analysis.analyses.empty()) {
        const lex::Analysis& a = ct.analysis.analyses[0];
        t.lemmaId = a.lemma;
        t.hasLemma = true;
        t.features = realise::featureView(morph::packedOf(lx, a));
        t.tier = tierOf(a.lemma);
      } else if (!ct.name) {
        t.unknown = ct.analysis.unknown || ct.analysis.analyses.empty();
      }
      o.tokens.push_back(std::move(t));
    }
    bool overflow = false;
    {
      std::string flat = target;
      std::replace(flat.begin(), flat.end(), '\n', ' ');
      subs::breakLines(flat, greekBreakHints(), cfg.maxLine, cfg.maxLines, &overflow);
      if (1 + (size_t)std::count(target.begin(), target.end(), '\n') > (size_t)cfg.maxLines) overflow = true;
    }
    bool warn = false;
    runChecks(target, o.tokens, opt, in, true, o, overflow, false, warn);
    bool unknown = false;
    for (const auto& t : o.tokens) unknown = unknown || t.unknown;
    const bool fix = !checkOk(o, "A1") || !checkOk(o, "A3") || !checkOk(o, "A4") || !checkOk(o, "A5") || unknown;
    const bool chk = warn || !checkOk(o, "A1b") || !checkOk(o, "A6") || !checkOk(o, "A8") || rep.fromRule;
    o.confidence = fix ? Confidence::Fix : chk ? Confidence::Check : Confidence::Ok;
    o.score = fix ? 0.3 : chk ? 0.7 : 1.0;
    glossary = nullptr;
    return o;
  }

  // ---- inspect -------------------------------------------------------------------------------------------------------
  rules::Analysis fromLemma(uint32_t id, uint32_t packed, const std::string& disp) const {
    rules::Analysis a;
    const lex::Lemma l = lx.lemma(id);
    a.lemmaId = id;
    a.head = display(l.head);
    a.glossEn = back ? back->gloss(id, grc2x::Target::En) : std::string(l.glossEn);
    a.glossEs = back ? back->gloss(id, grc2x::Target::Es) : std::string(l.glossEs);
    a.features = realise::featureView(packed);
    a.display = disp;
    a.tier = tierOf(id);
    return a;
  }

  Result<rules::InspectResult> inspect(const std::string& word, rules::Lang lang, const rules::Options& opt) {
    rules::InspectResult res;
    if (lang == rules::Lang::Grc) {
      morph::Token t;
      analyse(lx, word, t);
      for (const lex::Analysis& a : t.analyses)
        res.analyses.push_back(fromLemma(a.lemma, morph::packedOf(lx, a), a.display.empty() ? word : display(a.display)));
      for (const morph::RuleAnalysis& a : t.ruleAnalyses) res.analyses.push_back(fromLemma(a.lemma, a.packed, a.display));
      // accent-insensitive suggestions: other spellings of the same letters, or keys that start like the word
      if (res.analyses.empty() || t.accentInsensitive) {
        const std::string key = text::greek_key(word);
        std::vector<std::string_view> keys;
        for (size_t n = key.size(); n >= 2 && keys.empty(); --n) {
          if (n < key.size() && ((unsigned char)key[n] & 0xC0) == 0x80) continue;
          lx.prefix(std::string_view(key).substr(0, n), 5, keys);
        }
        for (std::string_view k : keys) res.suggestions.emplace_back(k);
      }
      return Result<rules::InspectResult>(std::move(res));
    }
    // English / Spanish word, target Greek: the Greek candidates
    transfer::Settings st;
    st.lang = lang == rules::Lang::Es ? frame::SrcLang::Es : frame::SrcLang::En;
    st.fidelity = std::max(1, std::min(3, opt.fidelity));
    st.srcLex = lang == rules::Lang::Es ? esLex : enLex;
    const std::string low = text::lower(word);
    std::vector<std::string> lemmas;
    if (const lex::Lexicon* src = lang == rules::Lang::Es ? esLex : enLex) {
      std::vector<lex::Analysis> an;
      src->lookup(lang == rules::Lang::En ? text::en_key(low) : text::es_key(low), an);
      for (const lex::Analysis& a : an) {
        const std::string h = text::lower(src->lemma(a.lemma).head);
        if (std::find(lemmas.begin(), lemmas.end(), h) == lemmas.end()) lemmas.push_back(h);
      }
    }
    if (lemmas.empty()) lemmas.push_back(low);
    size_t added = 0;
    for (const std::string& lm : lemmas)
      for (uint8_t pos : {feat::Noun, feat::Verb, feat::Adj, feat::Adv}) {
        transfer::Choice ch;
        xfer->select(lm, pos, {}, false, false, st, ch);
        for (const transfer::Candidate& c : ch.candidates) {
          if (added >= 12) break;
          bool dup = false;
          for (const rules::Analysis& x : res.analyses) dup = dup || x.lemmaId == c.lemma;
          if (dup) continue;
          feat::Features f;
          f.pos = lx.lemma(c.lemma).pos;
          res.analyses.push_back(fromLemma(c.lemma, feat::pack(f), display(lx.lemma(c.lemma).head)));
          ++added;
        }
      }
    return Result<rules::InspectResult>(std::move(res));
  }
};

GreekPath::GreekPath(Key) {}
GreekPath::~GreekPath() = default;

Result<std::unique_ptr<GreekPath>> GreekPath::create(const lex::Lexicon& greek, const curated::CuratedData& cd,
                                                     const std::vector<std::filesystem::path>& dirs, PathConfig cfg) {
  Error last{ErrorCode::NotFound, "Greek curated tables not found", "The Greek rule tables (data/curated) are missing."};
  for (const stdfs::path& d : dirs) {
    std::error_code ec;
    if (d.empty() || !stdfs::exists(d / "order_grc.txt", ec)) continue;
    Result<GreekData> gd = GreekData::load(d);
    if (!gd.ok()) { last = gd.error(); continue; }
    Result<GreekTables> gt = GreekTables::load(d);
    if (!gt.ok()) { last = gt.error(); continue; }
    std::unique_ptr<GreekPath> p = std::make_unique<GreekPath>(Key{});
    p->impl_ = std::make_unique<Impl>(greek, cd, std::move(gd.value()), std::move(gt.value()), cfg);
    return Result<std::unique_ptr<GreekPath>>(std::move(p));
  }
  return Result<std::unique_ptr<GreekPath>>(last);
}

void GreekPath::setSources(const nlp::Pipeline* en, const lex::Lexicon* enLex, const nlp::Pipeline* es,
                           const lex::Lexicon* esLex) {
  if (en != impl_->pen || enLex != impl_->enLex) impl_->fbEn.reset();
  if (es != impl_->pes || esLex != impl_->esLex) impl_->fbEs.reset();
  impl_->pen = en;
  impl_->enLex = enLex;
  impl_->pes = es;
  impl_->esLex = esLex;
}

Result<std::vector<CueOutput>> GreekPath::translate(const std::vector<CueInput>& cues, const rules::Options& opt,
                                                    const rules::Context& ctx,
                                                    const std::function<void(size_t)>& progress,
                                                    const std::function<bool()>& cancelled) {
  if (fromGreek(opt)) return Result<std::vector<CueOutput>>(impl_->back->cues(cues, opt, ctx, progress, cancelled));
  return impl_->translate(cues, opt, ctx, progress, cancelled);
}

CueOutput GreekPath::check(const CueInput& in, const std::string& target, const rules::Options& opt,
                           const rules::Context& ctx) {
  return impl_->check(in, target, opt, ctx);
}

Result<rules::InspectResult> GreekPath::inspect(const std::string& word, rules::Lang lang, const rules::Options& opt) {
  return impl_->inspect(word, lang, opt);
}

}  // namespace vp::grc
