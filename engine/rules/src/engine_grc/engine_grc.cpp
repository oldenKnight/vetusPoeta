// GreekPath (engine_grc.h): the EN/ES -> Greek pipeline, the Greek checks and confidence, the Greek -> EN/ES pairs
// through grc2x, check() and inspect() for Greek. Mirrors src/engine/engine.cpp (the Latin path) step by step.
#include "engine_grc/engine_grc.h"

#include <algorithm>
#include <cmath>
#include <sstream>

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
    if (mem.addresseePlural && greekText.find(' ') == std::string::npos) {   // "plural χαίρετε"
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
          xfer->clause(fr[0], s, st, mem, co);
          if (cs == "subj") co.clause.pred.mood = feat::Subjunctive;
          if (sl.kind == frame::SlotKind::VP) co.clause.hasSubject = false;
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
      if (slash != std::string::npos) {
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
    fb.analyse(text, s);
    if (allowSplit && frame::FrameBuilder::troubled(s)) {
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
    if (!s.repairs.empty()) {
      std::string what;
      for (const std::string& r : s.repairs) what += (what.empty() ? "" : ", ") + r;
      addFlag(flags, "frame-fallback");
      so.reasons.push_back(Reason{-1, "form", "sentence analysis fallback (" + what + "): check the structure", ""});
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
    for (const std::string& f : flags) addFlag(so.flags, f);
    // A7: source coverage
    std::sort(covered.begin(), covered.end());
    for (size_t i = 0; i < s.tokens.size(); ++i) {
      const nlp::Token& t = s.tokens[i];
      const bool content = t.upos == "NOUN" || t.upos == "PROPN" || t.upos == "VERB" || t.upos == "ADJ" ||
                           t.upos == "ADV" || t.upos == "NUM" || t.upos == "PRON";
      if (!content || s.drop[i] != frame::Drop::No) continue;
      if (std::binary_search(covered.begin(), covered.end(), (int)i)) continue;
      if (t.upos == "ADV" && (t.lower == "not" || t.lower == "n't" || t.lower == "no")) continue;
      so.missing.push_back(t.text);
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
    o.checks.push_back(Check{"A9", true, "round trip not run for Greek (see the interlinear view)"});
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
                 !checkOk(o, "A8") || a.minMargin < 0.15 || a.song || a.nonverbal;
      for (const char* f : {"name-guessed", "from-rule", "addressee-guess", "missing-form", "merged", "frame-fallback",
                            "realia", "name-kept"})
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
