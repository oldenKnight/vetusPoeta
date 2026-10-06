// The sentence rewriter of Orbergise (internal.h): la2x analysis -> output slots (one per input token) -> structure
// rewrites of simplify_la.tsv -> vocabulary swaps above the tier ceiling -> text, changes and meaning bookkeeping.
// Words that already satisfy the ceiling and the structure rules keep their written form; a sentence without any
// change is returned byte for byte. Deterministic (fixed rule order, left to right, ties by lemma id).
#include <algorithm>
#include <cstdlib>

#include "internal.h"
#include "vp/morph.h"
#include "vp/realise_la.h"
#include "vp/text.h"

namespace vp::orberg::detail {

using namespace vp::feat;

namespace {

bool endsWith(std::string_view s, std::string_view e) {
  return s.size() >= e.size() && s.compare(s.size() - e.size(), e.size(), e) == 0;
}

struct Rd {
  uint32_t lemma = kNone;
  Features f;
  uint8_t lpos = 0, lgender = 0;
  uint16_t lflags = 0;
  bool name = false;
  std::string key;          // latin_key of the lemma head ("" without a lemma)
  uint8_t gender() const { return f.gender ? f.gender : lgender; }
};

struct Tk {
  std::string text;
  la2x::TokKind kind = la2x::TokKind::Word;
  std::string encl;
  bool cap = false;
  std::vector<Rd> rd;
  bool word() const { return kind == la2x::TokKind::Word; }
  bool punct() const { return kind == la2x::TokKind::Punct; }
  const Rd* best() const { return rd.empty() ? nullptr : &rd[0]; }
};

struct Slot {
  std::string text;
  int src = -1;
  bool punct = false, isNew = false, del = false, name = false;
  int change = -1;
};

struct Ch { int a = 1 << 30, b = -1; std::string reason, rule, why; };

enum PK { PNone, PPerf, PPres, PFut, PGerundive };

// One gender for generation: common / merged genders count as masculine (FN as feminine).
uint8_t one(uint8_t g) { return g == MF || g == MN || g == MFN ? (uint8_t)M : g == FN ? (uint8_t)F : g; }

bool gok(uint8_t a, uint8_t b) {
  return a == 0 || b == 0 || a == b || morph::genderAdmits(a, b) || morph::genderAdmits(b, a);
}

bool attachLeft(std::string_view t) {
  return t == "," || t == "." || t == ";" || t == ":" || t == "!" || t == "?" || t == ")" || t == "]" ||
         t == "\xC2\xBB" || t == "\xE2\x80\x9D" || t == "\xE2\x80\xA6" || t == "..." || t == "'" || t == "\"";
}
bool opensRight(std::string_view t) {
  return t == "(" || t == "[" || t == "\xC2\xAB" || t == "\xE2\x80\x9C" || t == "\xC2\xBF" || t == "\xC2\xA1";
}
bool endsSent(std::string_view t) { return t == "." || t == "!" || t == "?" || t == "..." || t == "\xE2\x80\xA6"; }

class Rewriter {
 public:
  Rewriter(const SentenceIn& in, Resources::Impl& R, SentenceOut& out)
      : in_(in), R_(R), out_(out), la_(R.la), opt_(*in.opt), ceiling_(std::max(1, std::min(3, in.opt->tierCeiling))) {}

  void run() {
    in_.ctx->la2x->analyser().analyse(in_.text, s_, in_.ctx->glossary);
    // new words carry length marks unless the input is written without them (a word whose reading has a macron
    // the text does not show)
    bool plain = false;
    if (!s_.macrons)
      for (const la2x::Token& t : s_.tokens)
        if (t.kind == la2x::TokKind::Word && t.best() && t.best()->lemma != kNone &&
            text::display_latin(t.best()->display, false) == text::nfc(t.text) &&
            text::nfc(t.best()->display) != text::nfc(t.text) &&
            text::latin_key(t.best()->display) == text::latin_key(t.text))
          plain = true;
    macrons_ = opt_.macrons && (s_.macrons || !plain);
    out_.macrons = macrons_;
    build();
    bookkeeping();
    const bool simplify = opt_.simplify && !in_.simplifyOnly;
    if (simplify) {
      pairs();
      if (R_.on("ablabs.perf") || R_.on("ablabs.pres")) ablativeAbsolute();
      if (R_.on("gerundive")) gerundive();
      if (R_.on("futpart")) futureParticiple();
      if (R_.on("supine")) supine();
      if (R_.on("cumsubj")) cumSubjunctive();
      if (R_.on("histinf")) historicInfinitive();
      if (R_.on("relchain")) relativeChain();
      if (R_.on("split")) splitLong();
      if (R_.on("accinf")) accInfMark();
    }
    vocabulary();
    finish();
  }

 private:
  const SentenceIn& in_;
  Resources::Impl& R_;
  SentenceOut& out_;
  const lex::Lexicon& la_;
  const OrbergOptions& opt_;
  int ceiling_;
  bool macrons_ = false;
  la2x::Sentence s_;
  std::vector<Tk> tk_;
  std::vector<Slot> sl_;
  std::vector<Ch> ch_;
  std::vector<char> touched_;
  int firstWord_ = -1;

  // ---- setup -----------------------------------------------------------------------------------------------------
  void build() {
    tk_.resize(s_.tokens.size());
    for (size_t i = 0; i < s_.tokens.size(); ++i) {
      const la2x::Token& t = s_.tokens[i];
      Tk& x = tk_[i];
      x.text = t.text;
      x.kind = t.kind;
      x.encl = t.encl;
      x.cap = t.capitalised;
      for (const la2x::Reading& r : t.readings) {
        Rd d;
        d.lemma = r.lemma;
        d.f = unpack(r.packed);
        d.name = r.name;
        if (r.lemma != kNone && r.lemma < la_.lemmaCount()) {
          const lex::Lemma l = la_.lemma(r.lemma);
          d.lpos = l.pos;
          d.lgender = l.gender;
          d.lflags = l.flags;
          d.key = std::string(l.key);
          if (l.flags & lex::ProperName) d.name = true;
        } else {
          d.lpos = d.f.pos;
          d.lgender = r.nameGender;
        }
        if (!d.f.gender && r.nameGender) d.f.gender = r.nameGender;
        x.rd.push_back(std::move(d));
      }
      Slot s;
      s.text = t.text;
      s.src = (int)i;
      s.punct = t.kind == la2x::TokKind::Punct;
      s.name = !x.rd.empty() && x.rd[0].name;
      sl_.push_back(s);
      if (firstWord_ < 0 && x.word()) firstWord_ = (int)i;
    }
    touched_.assign(tk_.size(), 0);
  }

  uint32_t contentLemma(const Rd& r) {
    if (r.lemma == kNone) return kNone;
    if (R_.isParticipleLemma(r.lemma)) {
      const uint32_t v = R_.verbOf(r.lemma);
      return v != kNone ? v : r.lemma;
    }
    return r.lemma;
  }

  void bookkeeping() {
    for (size_t i = 0; i < tk_.size(); ++i) {
      const Tk& t = tk_[i];
      if (!t.word() || t.rd.empty()) continue;
      const la2x::Reading& r0 = s_.tokens[i].readings[0];
      if (!contentReading(r0, la_)) continue;
      const Rd& r = t.rd[0];
      if (r.name) {
        out_.content.emplace_back(nameId(text::latin_key(r.lemma != kNone ? std::string(la_.lemma(r.lemma).head) : t.text)),
                                  r.lemma != kNone ? std::string(la_.lemma(r.lemma).head) : t.text);
        continue;
      }
      const uint32_t c = contentLemma(r);
      if (c == kNone) continue;
      out_.content.emplace_back(c, std::string(la_.lemma(c).head));
      out_.inputLemmas.push_back(c);
    }
  }

  // ---- reading predicates --------------------------------------------------------------------------------------------
  static bool finite(const Rd& r) {
    return r.f.pos == Verb && (r.f.mood == Indicative || r.f.mood == Subjunctive || r.f.mood == Imperative);
  }
  bool finiteBest(int i) const { return i >= 0 && i < (int)tk_.size() && tk_[i].best() && finite(*tk_[i].best()); }
  bool isSum(const Rd& r) const { return r.key == "sum" && r.f.pos == Verb; }
  bool isPrep(int i) const {
    const Rd* r = tk_[(size_t)i].best();
    return r && (r->lpos == Prep || r->f.pos == Prep);
  }
  static bool nominal(const Rd& r) {
    return r.name || r.lpos == Noun || r.lpos == Name || r.lpos == Pron || r.f.pos == Noun || r.f.pos == Name ||
           r.f.pos == Pron;
  }
  static bool noun(const Rd& r) { return r.name || r.lpos == Noun || r.lpos == Name || r.f.pos == Noun; }
  static bool modifier(const Rd& r) {
    return r.lpos == Adj || r.lpos == Det || r.lpos == Num || r.lpos == Participle || r.lpos == Pron ||
           r.f.pos == Adj || r.f.pos == Det;
  }
  PK partKind(const Rd& r) const {
    if (r.f.pos == Verb && r.f.mood == ParticipleMood) {
      if (r.f.tense == Present) return PPres;
      if (r.f.tense == Future) return r.f.voice == Passive ? PGerundive : PFut;
      if (r.f.tense == Perfect) return PPerf;
      return PNone;
    }
    if (r.lpos == Participle || r.f.pos == Participle) {
      if (endsWith(r.key, "ndus")) return PGerundive;
      if (endsWith(r.key, "urus")) return PFut;
      if (endsWith(r.key, "ns")) return PPres;
      return PPerf;
    }
    return PNone;
  }
  static uint8_t pcase(const Rd& r) { return r.f.case_ ? r.f.case_ : (uint8_t)Nom; }
  static uint8_t pnum(const Rd& r) { return r.f.number ? r.f.number : (uint8_t)Sg; }
  uint32_t verbOfRd(const Rd& r) {
    if (r.lpos == Verb) return r.lemma;
    return R_.verbOf(r.lemma);
  }
  bool deponent(uint32_t v) const { return v != kNone && (la_.lemma(v).flags & lex::Deponent); }
  bool agree(const Rd& a, const Rd& b) const {
    return a.f.case_ == b.f.case_ && pnum(a) == pnum(b) && gok(a.gender(), b.gender());
  }
  bool boundary(int i) const {   // punctuation, a finite verb or a preposition stops an NP scan
    if (i < 0 || i >= (int)tk_.size()) return true;
    return tk_[(size_t)i].punct() || finiteBest(i) || isPrep(i);
  }
  const std::string& key0(int i) const {
    static const std::string empty;
    const Rd* r = tk_[(size_t)i].best();
    return r ? r->key : empty;
  }
  std::string textKey(int i) const { return text::latin_key(tk_[(size_t)i].text); }

  // ---- generation ------------------------------------------------------------------------------------------------
  bool gen(uint32_t lemma, Features f, std::string& out) {
    if (lemma == kNone) return false;
    std::string form;
    morph::GenInfo info;
    if (!morph::generate(la_, lemma, f, form, true, &info)) return false;
    if (info.fromRule) return false;   // never write a paradigm guess into a reader text
    const lex::Lemma l = la_.lemma(lemma);
    out = realise::Macrons::apply(form, true, R_.cd.macronOverride(l.key));
    if (!macrons_) out = text::display_latin(out, false);
    return !out.empty();
  }
  std::string head(uint32_t lemma) {
    std::string h = morph::displayForm(la_.lemma(lemma).head, true);
    h = realise::Macrons::apply(h, true, R_.cd.macronOverride(la_.lemma(lemma).key));
    return macrons_ ? h : text::display_latin(h, false);
  }
  std::string word(const char* headText, uint8_t pos) {
    const uint32_t id = R_.find(headText, pos);
    if (id == kNone) return macrons_ ? std::string(headText) : text::display_latin(headText, false);
    return head(id);
  }
  // A core lemma for a verb above the ceiling (rules generate verbs through this); the swap is recorded.
  uint32_t pick(uint32_t v, int ch, std::string* why = nullptr) {
    if (v == kNone || R_.tier(v) <= ceiling_) return v;
    const Swap s = R_.swapFor(v, ceiling_, opt_.keepNames);
    if (s.lemma == kNone || !s.before.empty() || !s.after.empty()) return v;
    out_.mapped.emplace_back(v, s.lemma);
    if (why) *why = s.why;
    (void)ch;
    return s.lemma;
  }

  // A rule read token `src` with another reading than the best one: the meaning check follows it.
  void reinterpret(int src, const Rd& used) {
    const Rd* b = tk_[(size_t)src].best();
    if (!b) return;
    const uint32_t from = contentLemma(*b), to = contentLemma(used);
    if (from != kNone && to != kNone && from != to) out_.mapped.emplace_back(from, to);
  }

  // ---- edits -----------------------------------------------------------------------------------------------------
  int slotOf(int src) const {
    for (size_t k = 0; k < sl_.size(); ++k)
      if (sl_[k].src == src) return (int)k;
    return -1;
  }
  int newChange(const char* reason, const char* rule, const std::string& why) {
    Ch c;
    c.reason = reason;
    c.rule = rule;
    c.why = why;
    ch_.push_back(c);
    return (int)ch_.size() - 1;
  }
  void span(int ch, int src) {
    if (src < 0) return;
    ch_[(size_t)ch].a = std::min(ch_[(size_t)ch].a, src);
    ch_[(size_t)ch].b = std::max(ch_[(size_t)ch].b, src);
  }
  void set(int src, const std::string& text, int ch, bool name = false) {
    const int k = slotOf(src);
    if (k < 0) return;
    sl_[(size_t)k].text = text + tk_[(size_t)src].encl;
    sl_[(size_t)k].isNew = true;
    sl_[(size_t)k].change = ch;
    sl_[(size_t)k].name = name;
    touched_[(size_t)src] = 1;
    span(ch, src);
  }
  void del(int src, int ch) {
    const int k = slotOf(src);
    if (k < 0) return;
    sl_[(size_t)k].del = true;
    sl_[(size_t)k].change = ch;
    touched_[(size_t)src] = 1;
    span(ch, src);
  }
  void insert(int src, bool after, const std::vector<std::string>& ws, int ch, bool punct = false) {
    int k = slotOf(src);
    if (k < 0) return;
    if (after) ++k;
    std::vector<Slot> add;
    for (const std::string& w : ws) {
      if (w.empty()) continue;
      Slot s;
      s.text = w;
      s.isNew = true;
      s.change = ch;
      s.punct = punct;
      add.push_back(s);
    }
    sl_.insert(sl_.begin() + k, add.begin(), add.end());
    span(ch, src);
  }
  static std::vector<std::string> split(const std::string& s) {
    std::vector<std::string> o;
    size_t a = 0;
    while (a < s.size()) {
      while (a < s.size() && s[a] == ' ') ++a;
      size_t b = a;
      while (b < s.size() && s[b] != ' ') ++b;
      if (b > a) o.push_back(s.substr(a, b - a));
      a = b;
    }
    return o;
  }
  // Replaces a token with one or more words (the last carries the enclitic).
  void setWords(int src, const std::string& words, int ch, bool name = false) {
    std::vector<std::string> ws = split(words);
    if (ws.empty()) { del(src, ch); return; }
    set(src, ws.back(), ch, name);
    ws.pop_back();
    if (!ws.empty()) insert(src, false, ws, ch);
  }

  // The noun phrase around head token h read as `hr`: adjacent modifiers agreeing with it (two on each side at most),
  // no punctuation, finite verb or preposition in between. (token, reading) pairs, head included, in text order.
  std::vector<std::pair<int, const Rd*>> npOf(int h, const Rd& hr) {
    std::vector<std::pair<int, const Rd*>> np;
    np.emplace_back(h, &hr);
    for (int dir = -1; dir <= 1; dir += 2) {
      for (int j = h + dir, n = 0; n < 2; j += dir, ++n) {
        if (boundary(j) || !tk_[(size_t)j].word() || touched_[(size_t)j]) break;
        const Rd* m = nullptr;
        for (const Rd& r : tk_[(size_t)j].rd)
          if (modifier(r) && !noun(r) && r.f.case_ == hr.f.case_ && pnum(r) == pnum(hr) && gok(r.gender(), hr.gender())) {
            m = &r;
            break;
          }
        if (!m) break;
        np.emplace_back(j, m);
      }
    }
    std::sort(np.begin(), np.end(), [](const std::pair<int, const Rd*>& a, const std::pair<int, const Rd*>& b) {
      return a.first < b.first;
    });
    return np;
  }
  // The forms of an NP in another case (same number; `gender` overrides the modifiers' gender when non-zero).
  bool recase(const std::vector<std::pair<int, const Rd*>>& np, uint8_t newCase, std::vector<std::string>& forms,
              uint8_t gender = 0) {
    forms.clear();
    if (!gender)
      for (const auto& m : np)
        if (noun(*m.second)) gender = one(m.second->gender());
    for (const auto& m : np) {
      Features f = m.second->f;
      f.case_ = newCase;
      if (!f.number) f.number = Sg;
      if (modifier(*m.second) && !noun(*m.second)) f.gender = gender ? gender : one(f.gender);
      std::string w;
      if (m.second->lemma == kNone) return false;
      if (!gen(m.second->lemma, f, w)) return false;
      if (m.second->name && !startsUpper(w)) w = capitalise(w);
      forms.push_back(w);
    }
    return true;
  }
  // The reading of a word that the rules trust: the best one, unless another reading of the same part of speech is a
  // core word (vēnit: veniō perfect, not vēneō present).
  const Rd& core(int i) const {
    const Rd& b = tk_[(size_t)i].rd[0];
    if (b.lemma == kNone || R_.tier(b.lemma) <= 1) return b;
    for (const Rd& r : tk_[(size_t)i].rd)
      if (r.lemma != kNone && r.f.pos == b.f.pos && r.f.mood == b.f.mood && R_.tier(r.lemma) == 1) return r;
    return b;
  }
  // A neuter read as nominative next to another nominative subject is the object (nom/acc are one form for neuters;
  // the analyser keeps the first): the case a regenerated noun of another gender needs.
  uint8_t caseOf(int i, const Rd& r) const {
    if (r.f.case_ != Nom || one(r.lgender) != N) return r.f.case_;
    bool acc = false;
    for (const Rd& x : tk_[(size_t)i].rd) acc = acc || (x.lemma == r.lemma && x.f.case_ == Acc && pnum(x) == pnum(r));
    if (!acc) return r.f.case_;
    for (int j = 0; j < (int)tk_.size(); ++j) {
      if (j == i || !tk_[(size_t)j].word() || tk_[(size_t)j].rd.empty()) continue;
      const Rd& o = tk_[(size_t)j].rd[0];
      if (!noun(o) || o.f.case_ != Nom || one(o.lgender) == N) continue;
      return Acc;
    }
    return r.f.case_;
  }
  uint8_t mainPersonOf(int v) const {
    const Rd* r = tk_[(size_t)v].best();
    return r && r->f.person ? r->f.person : (uint8_t)P3;
  }
  // The finite verb of the main clause (la2x clauses), else the first finite verb outside [a, b].
  int mainVerb(int a = -1, int b = -1) const {
    for (const la2x::Clause& c : s_.clauses)
      if (c.parent < 0 && c.verb >= 0 && (c.verb < a || c.verb > b) && finiteBest(c.verb)) return c.verb;
    for (int i = 0; i < (int)tk_.size(); ++i)
      if ((i < a || i > b) && finiteBest(i)) return i;
    return -1;
  }

  // ---- rules: word pairs (double negatives, haud) ------------------------------------------------------------------
  void pairs() {
    for (int i = 0; i < (int)tk_.size(); ++i) {
      if (!tk_[(size_t)i].word() || touched_[(size_t)i]) continue;
      for (const PairRow& p : R_.pairs) {
        std::vector<int> at;
        int j = i;
        bool ok = true;
        for (const std::string& k : p.keys) {
          while (j < (int)tk_.size() && !tk_[(size_t)j].word()) {
            if (!at.empty()) { ok = false; break; }
            ++j;
          }
          if (!ok || j >= (int)tk_.size() || touched_[(size_t)j]) { ok = false; break; }
          if (key0(j) != k && textKey(j) != k) { ok = false; break; }
          at.push_back(j);
          ++j;
        }
        if (!ok || at.size() != p.keys.size()) continue;
        const int last = at.back();
        const Rd* lr = tk_[(size_t)last].best();
        const uint32_t to = R_.find(p.to, 0);
        std::string w;
        if (p.agree && lr && to != kNone) {
          Features f = lr->f;
          if (lr->lpos == Verb || lr->f.pos == Verb) {
            if (!gen(to, f, w)) continue;
          } else {
            if (!gen(to, f, w)) continue;
          }
        } else {
          w = to != kNone ? head(to) : (macrons_ ? p.to : text::display_latin(p.to, false));
        }
        std::string from;
        for (int k : at) from += (from.empty() ? "" : " ") + tk_[(size_t)k].text;
        const int ch = newChange("structure", "pair", from + " -> " + w + (p.note.empty() ? "" : " (" + p.note + ")"));
        for (size_t k = 0; k + 1 < at.size(); ++k) del(at[k], ch);
        set(last, w, ch);
        if (lr && to != kNone) out_.mapped.emplace_back(contentLemma(*lr), to);
        break;
      }
    }
  }

  // ---- rules: ablative absolute --------------------------------------------------------------------------------------
  void ablativeAbsolute() {
    for (int p = 0; p < (int)tk_.size(); ++p) {
      if (!tk_[(size_t)p].word() || touched_[(size_t)p]) continue;
      for (const Rd& pr : tk_[(size_t)p].rd) {
        const PK k = partKind(pr);
        if ((k != PPerf && k != PPres) || pr.f.case_ != Abl) continue;
        if (k == PPerf && !R_.on("ablabs.perf")) continue;
        if (k == PPres && !R_.on("ablabs.pres")) continue;
        // the ablative noun the participle agrees with: next to it (one word may stand between)
        int n = -1;
        const Rd* nr = nullptr;
        for (int d : {-1, 1, -2, 2}) {
          const int j = p + d;
          if (j < 0 || j >= (int)tk_.size() || !tk_[(size_t)j].word() || touched_[(size_t)j]) continue;
          if (d == -2 && (boundary(p - 1) || !tk_[(size_t)(p - 1)].word())) continue;
          if (d == 2 && (boundary(p + 1) || !tk_[(size_t)(p + 1)].word())) continue;
          for (const Rd& r : tk_[(size_t)j].rd)
            if (nominal(r) && r.f.case_ == Abl && pnum(r) == pnum(pr) && gok(r.gender(), pr.gender())) { nr = &r; break; }
          if (nr) { n = j; break; }
        }
        // an agent phrase between the noun and the participle ("epistulā ā patre scrīptā")
        int agentFrom = -1;
        if (n < 0 && k == PPerf && p >= 3 && isPrep(p - 2) && tk_[(size_t)(p - 1)].word() &&
            (key0(p - 2) == "a" || key0(p - 2) == "ab") && tk_[(size_t)(p - 3)].word() && !touched_[(size_t)(p - 3)]) {
          for (const Rd& r : tk_[(size_t)(p - 3)].rd)
            if (nominal(r) && r.f.case_ == Abl && pnum(r) == pnum(pr) && gok(r.gender(), pr.gender())) { nr = &r; break; }
          if (nr) { n = p - 3; agentFrom = p - 2; }
        }
        if (n < 0) continue;
        std::vector<std::pair<int, const Rd*>> np = npOf(n, *nr);
        if (agentFrom >= 0)
          np.erase(std::remove_if(np.begin(), np.end(),
                                  [&](const std::pair<int, const Rd*>& x) { return x.first >= agentFrom; }),
                   np.end());
        // the participle is not a modifier of the NP for the rewrite
        np.erase(std::remove_if(np.begin(), np.end(), [&](const std::pair<int, const Rd*>& x) { return x.first == p; }),
                 np.end());
        const int a = std::min(np.front().first, p), b = std::max(np.back().first, p);
        if (a > 0 && isPrep(a - 1)) continue;          // governed by a preposition: not absolute
        const int mv = mainVerb(a, b);
        if (mv < 0) continue;                          // no main clause
        uint32_t v = verbOfRd(pr);
        if (v == kNone) continue;
        std::string swapWhy;
        const uint32_t v2 = pick(v, -1, &swapWhy);
        const uint8_t num = pnum(*nr), gender = nr->gender() ? one(nr->gender()) : (uint8_t)M;
        std::string verbWords;
        std::vector<std::string> npForms;
        std::string conj, why;
        const bool active = k == PPerf && !deponent(v) && R_.param("ablabs.perf", "voice", "passive") == "active";
        if (k == PPerf) {
          conj = R_.param("ablabs.perf", "conj", "postquam");
          Features f = morph::verbForm(active ? mainPersonOf(mv) : (uint8_t)P3,
                                       active ? pnum(*tk_[(size_t)mv].best()) : num, Perfect, Indicative,
                                       deponent(v) || active ? (uint8_t)Active : (uint8_t)Passive);
          f.gender = gender;
          if (!deponent(v) && !active && deponent(v2)) continue;   // a deponent has no passive
          if (!gen(v2, f, verbWords)) continue;
          if (!recase(np, active ? (uint8_t)Acc : (uint8_t)Nom, npForms)) continue;
          why = "ablative absolute -> " + conj + " clause (perfect indicative)";
        } else {
          conj = R_.param("ablabs.pres", "conj", "dum");
          Features f = morph::verbForm(P3, num, Present, Indicative, Active);
          if (!gen(v2, f, verbWords)) continue;
          if (!recase(np, Nom, npForms)) continue;
          why = "ablative absolute -> " + conj + " clause (present indicative)";
        }
        if (!swapWhy.empty()) why += "; " + swapWhy;
        reinterpret(p, pr);
        for (const auto& x : np) reinterpret(x.first, *x.second);
        const int ch = newChange("structure", k == PPerf ? "ablabs.perf" : "ablabs.pres", why);
        for (size_t q = 0; q < np.size(); ++q) set(np[q].first, npForms[q], ch, np[q].second->name);
        if (p > np.back().first) {
          setWords(p, verbWords, ch);
        } else {   // participle first ("lēctā epistulā"): the verb goes after the noun
          del(p, ch);
          insert(np.back().first, true, split(verbWords), ch);
        }
        insert(a, false, {word(conj.c_str(), Conj)}, ch);
        // a fronted clause is closed by a comma before the main clause
        if (b + 1 < (int)tk_.size() && tk_[(size_t)(b + 1)].word() && mv > b) insert(b, true, {","}, ch, true);
        if (active) out_.flags.push_back("agent-guess");
        if (v2 != v) out_.mapped.emplace_back(v, v2);
        break;
      }
    }
  }

  // ---- rules: gerundive of obligation / passive periphrastic -----------------------------------------------------------
  void gerundive() {
    for (int g = 0; g < (int)tk_.size(); ++g) {
      if (!tk_[(size_t)g].word() || touched_[(size_t)g]) continue;
      const Rd* gr = nullptr;
      bool gerund = false;
      for (const Rd& r : tk_[(size_t)g].rd) {
        if (partKind(r) == PGerundive && (pcase(r) == Nom)) { gr = &r; break; }
        if (r.f.pos == Verb && r.f.mood == Gerund && (r.f.case_ == Acc || r.f.case_ == Nom || r.f.case_ == 0)) {
          gr = &r;
          gerund = true;
          break;
        }
      }
      if (!gr) continue;
      // a finite form of sum next to it
      int s = -1;
      for (int d : {1, -1, 2}) {
        const int j = g + d;
        if (j < 0 || j >= (int)tk_.size() || touched_[(size_t)j]) continue;
        const Rd* r = tk_[(size_t)j].best();
        if (r && isSum(*r) && finite(*r)) { s = j; break; }
        if (!tk_[(size_t)j].word()) break;
      }
      if (s < 0) continue;
      const Rd& sr = *tk_[(size_t)s].best();
      // clause span: between punctuation marks
      int c0 = std::min(g, s), c1 = std::max(g, s);
      while (c0 > 0 && !tk_[(size_t)(c0 - 1)].punct()) --c0;
      while (c1 + 1 < (int)tk_.size() && !tk_[(size_t)(c1 + 1)].punct()) ++c1;
      const uint32_t v = verbOfRd(*gr);
      if (v == kNone) continue;
      // dative agent (not with a verb that takes a dative object)
      bool datVerb = false;
      if (const curated::Valency* va = R_.cd.valency(la_.lemma(v).key))
        if (!va->frames.empty())
          datVerb = va->frames[0].kind == curated::FrameKind::Dat || va->frames[0].kind == curated::FrameKind::DatAcc;
      int ag = -1;
      const Rd* ar = nullptr;
      if (!datVerb)
        for (int j = c0; j <= c1; ++j) {
          if (j == g || j == s || !tk_[(size_t)j].word() || touched_[(size_t)j]) continue;
          if (j > 0 && isPrep(j - 1)) continue;
          for (const Rd& r : tk_[(size_t)j].rd)
            if (nominal(r) && r.f.case_ == Dat && !modifier(r)) { ar = &r; break; }
            else if (nominal(r) && r.f.case_ == Dat && r.lpos == Pron) { ar = &r; break; }
          if (ar) { ag = j; break; }
        }
      // the gerundive's noun (nominative, agreeing)
      int sj = -1;
      const Rd* snr = nullptr;
      if (!gerund)
        for (int j = c0; j <= c1; ++j) {
          if (j == g || j == s || j == ag || !tk_[(size_t)j].word() || touched_[(size_t)j]) continue;
          for (const Rd& r : tk_[(size_t)j].rd)
            if (nominal(r) && !modifier(r) && r.f.case_ == Nom && pnum(r) == pnum(*gr) && gok(r.gender(), gr->gender())) {
              snr = &r;
              break;
            }
          if (snr) { sj = j; break; }
        }
      std::string swapWhy;
      const uint32_t v2 = pick(v, -1, &swapWhy);
      const uint32_t debeo = R_.find(R_.param("gerundive", "modal", "dēbeō"), Verb);
      if (debeo == kNone) continue;
      std::string inf, modal, why;
      std::vector<std::pair<int, const Rd*>> subjNP;
      std::vector<std::string> subjForms, agentForms;
      bool dropAgent = false;
      if (ag >= 0) {
        uint8_t person = ar->f.person ? ar->f.person : (uint8_t)P3;
        const std::string& k = ar->key;
        if (k == "ego" || k == "nos") person = P1;
        else if (k == "tu" || k == "uos") person = P2;
        dropAgent = person != P3 && ar->lpos == Pron;
        if (!gen(v2, morph::infinitive(Present, Active), inf)) continue;
        Features mf = morph::verbForm(person, pnum(*ar), sr.f.tense, sr.f.mood, Active);
        if (!gen(debeo, mf, modal)) continue;
        if (!dropAgent) {
          std::vector<std::pair<int, const Rd*>> anp = npOf(ag, *ar);
          if (!recase(anp, Nom, agentForms)) continue;
          for (size_t q = 0; q < anp.size(); ++q) subjNP.push_back(anp[q]);   // reuse as agent list below
        }
        if (sj >= 0) {
          std::vector<std::pair<int, const Rd*>> onp = npOf(sj, *snr);
          std::vector<std::string> of;
          if (!recase(onp, Acc, of)) continue;
          // object NP and agent NP never overlap (different cases)
          for (size_t q = 0; q < onp.size(); ++q) { subjNP.push_back(onp[q]); agentForms.push_back(of[q]); }
        }
        why = "gerundive of obligation -> dēbeō + infinitive (the dative agent is the subject)";
      } else if (sj >= 0 && !gerund && !deponent(v) && !deponent(v2)) {
        if (!gen(v2, morph::infinitive(Present, Passive), inf)) continue;
        Features mf = morph::verbForm(P3, pnum(*snr), sr.f.tense, sr.f.mood, Active);
        if (!gen(debeo, mf, modal)) continue;
        why = "gerundive of obligation -> dēbeō + passive infinitive (no agent named)";
      } else {
        if (std::find(out_.flags.begin(), out_.flags.end(), "structure-kept") == out_.flags.end())
          out_.flags.push_back("structure-kept");
        out_.notes.push_back(rules::Reason{-1, "orbergise", "gerundive kept: no agent to make the subject",
                                           "{\"was\":\"" + jsonEscape(tk_[(size_t)g].text) + "\",\"now\":\"" +
                                               jsonEscape(tk_[(size_t)g].text) +
                                               "\",\"why\":\"structure kept (gerundive without an agent)\"}"});
        continue;
      }
      if (!swapWhy.empty()) why += "; " + swapWhy;
      reinterpret(g, *gr);
      const int ch = newChange("structure", "gerundive", why);
      if (dropAgent) del(ag, ch);
      for (size_t q = 0; q < subjNP.size(); ++q) set(subjNP[q].first, agentForms[q], ch, subjNP[q].second->name);
      set(g, inf, ch);
      set(s, modal, ch);
      if (v2 != v) out_.mapped.emplace_back(v, v2);
    }
  }

  // ---- rules: future participle + sum ----------------------------------------------------------------------------------
  void futureParticiple() {
    for (int f = 0; f < (int)tk_.size(); ++f) {
      if (!tk_[(size_t)f].word() || touched_[(size_t)f]) continue;
      const Rd* fr = nullptr;
      for (const Rd& r : tk_[(size_t)f].rd)
        if (partKind(r) == PFut && pcase(r) == Nom) { fr = &r; break; }
      if (!fr) continue;
      int s = -1;
      for (int d : {1, -1}) {
        const int j = f + d;
        if (j < 0 || j >= (int)tk_.size() || touched_[(size_t)j]) continue;
        const Rd* r = tk_[(size_t)j].best();
        if (r && isSum(*r) && finite(*r) && r->f.mood == Indicative) { s = j; break; }
      }
      if (s < 0) continue;
      const Rd& sr = *tk_[(size_t)s].best();
      const uint32_t v = verbOfRd(*fr);
      if (v == kNone) continue;
      std::string swapWhy;
      const uint32_t v2 = pick(v, -1, &swapWhy);
      const uint8_t person = sr.f.person ? sr.f.person : (uint8_t)P3;
      reinterpret(f, *fr);
      if (sr.f.tense == Present || sr.f.tense == Future) {
        std::string w;
        if (!gen(v2, morph::verbForm(person, pnum(sr), Future, Indicative, Active), w)) continue;
        const int ch = newChange("structure", "futpart", "future participle + sum -> future tense" +
                                                             (swapWhy.empty() ? std::string() : "; " + swapWhy));
        setWords(f, w, ch);
        del(s, ch);
      } else {
        const uint32_t volo = R_.find(R_.param("futpart", "past", "volō"), Verb);
        std::string inf, m;
        if (volo == kNone || !gen(v2, morph::infinitive(Present, Active), inf) ||
            !gen(volo, morph::verbForm(person, pnum(sr), Imperfect, Indicative, Active), m))
          continue;
        const int ch = newChange("structure", "futpart", "future participle + past of sum -> volō + infinitive" +
                                                             (swapWhy.empty() ? std::string() : "; " + swapWhy));
        set(f, inf, ch);
        set(s, m, ch);
      }
      if (v2 != v) out_.mapped.emplace_back(v, v2);
    }
  }

  // ---- rules: supine of purpose ----------------------------------------------------------------------------------------
  void supine() {
    for (int u = 0; u < (int)tk_.size(); ++u) {
      if (!tk_[(size_t)u].word() || touched_[(size_t)u]) continue;
      const Rd* ur = nullptr;
      for (const Rd& r : tk_[(size_t)u].rd)
        if (r.f.pos == Verb && (r.f.extra & Supine) && (r.f.case_ == Acc || r.f.case_ == 0)) { ur = &r; break; }
      if (!ur) continue;
      // the main verb: the nearest finite verb of the same stretch (no punctuation between)
      int m = -1;
      for (int j = u + 1; j < (int)tk_.size() && !tk_[(size_t)j].punct(); ++j)
        if (finiteBest(j)) { m = j; break; }
      if (m < 0)
        for (int j = u - 1; j >= 0 && !tk_[(size_t)j].punct(); --j)
          if (finiteBest(j)) { m = j; break; }
      if (m < 0) continue;
      const Rd& mr = core(m);
      // the supine's object: an accusative noun right before it (not a place to which)
      std::vector<int> obj;
      if (u > 0 && tk_[(size_t)(u - 1)].word() && !touched_[(size_t)(u - 1)] && !(u > 1 && isPrep(u - 2))) {
        const Rd* o = nullptr;
        for (const Rd& r : tk_[(size_t)(u - 1)].rd)
          if (nominal(r) && r.f.case_ == Acc && !r.name && r.key != "domus" && r.key != "rus") { o = &r; break; }
        if (o) {
          for (const auto& x : npOf(u - 1, *o))
            if (x.first < u) obj.push_back(x.first);
        }
      }
      const uint32_t v = ur->lemma;
      std::string swapWhy;
      const uint32_t v2 = pick(v, -1, &swapWhy);
      const bool primary = mr.f.mood == Imperative || mr.f.tense == Present || mr.f.tense == Future ||
                           mr.f.tense == FuturePerfect;
      std::string w;
      if (!gen(v2, morph::verbForm(mr.f.person ? mr.f.person : (uint8_t)P3, pnum(mr), primary ? Present : Imperfect,
                                   Subjunctive, Active),
               w))
        continue;
      reinterpret(u, *ur);
      const std::string conj = R_.param("supine", "conj", "ut");
      const int ch = newChange("structure", "supine", "supine of purpose -> " + conj + " + subjunctive" +
                                                          (swapWhy.empty() ? std::string() : "; " + swapWhy));
      std::vector<std::string> clause{word(conj.c_str(), Conj)};
      for (int o : obj) { clause.push_back(tk_[(size_t)o].text); del(o, ch); }
      for (const std::string& x : split(w)) clause.push_back(x);
      del(u, ch);
      insert(m, true, clause, ch);
      if (v2 != v) out_.mapped.emplace_back(v, v2);
    }
  }

  // ---- rules: cum + subjunctive ------------------------------------------------------------------------------------------
  void cumSubjunctive() {
    for (int c = 0; c < (int)tk_.size(); ++c) {
      if (!tk_[(size_t)c].word() || touched_[(size_t)c]) continue;
      const Rd* cr = tk_[(size_t)c].best();
      if (!cr || cr->key != "cum" || (cr->lpos != Conj && cr->f.pos != Conj)) continue;
      int v = -1;
      for (int j = c + 1; j < (int)tk_.size() && !tk_[(size_t)j].punct(); ++j)
        if (finiteBest(j)) { v = j; break; }
      if (v < 0 || touched_[(size_t)v]) continue;
      const Rd& vr = *tk_[(size_t)v].best();
      if (vr.f.mood != Subjunctive) continue;
      const uint8_t person = vr.f.person ? vr.f.person : (uint8_t)P3;
      std::string conj, w, why;
      // periphrastic perfect passive / deponent: participle + esset -> participle + est
      const bool peri = isSum(vr) && v > 0 && [&] {
        for (const Rd& r : tk_[(size_t)(v - 1)].rd)
          if (partKind(r) == PPerf && pcase(r) == Nom) return true;
        return false;
      }();
      if (vr.f.tense == Pluperfect || (peri && vr.f.tense == Imperfect && false)) {
        conj = R_.param("cumsubj", "plup", "postquam");
        if (!gen(vr.lemma, morph::verbForm(person, pnum(vr), Perfect, Indicative, vr.f.voice ? vr.f.voice : (uint8_t)Active), w))
          continue;
        why = "cum + pluperfect subjunctive -> " + conj + " + perfect indicative";
      } else if (vr.f.tense == Imperfect && peri) {
        // "cum missus esset" (pluperfect passive): esset -> est
        conj = R_.param("cumsubj", "plup", "postquam");
        if (!gen(vr.lemma, morph::verbForm(person, pnum(vr), Present, Indicative, Active), w)) continue;
        why = "cum + pluperfect subjunctive -> " + conj + " + perfect indicative";
      } else if (vr.f.tense == Imperfect) {
        conj = R_.param("cumsubj", "impf", "dum");
        if (!gen(vr.lemma, morph::verbForm(person, pnum(vr), Present, Indicative, vr.f.voice ? vr.f.voice : (uint8_t)Active), w))
          continue;
        why = "cum + imperfect subjunctive -> " + conj + " + present indicative";
      } else {
        continue;
      }
      const int ch = newChange("structure", "cumsubj", why);
      set(c, word(conj.c_str(), Conj), ch);
      setWords(v, w, ch);
    }
  }

  // ---- rules: historic infinitive ------------------------------------------------------------------------------------------
  void historicInfinitive() {
    std::vector<int> infs;
    for (int i = 0; i < (int)tk_.size(); ++i) {
      if (!tk_[(size_t)i].word() || touched_[(size_t)i]) continue;
      bool inf = false;
      for (const Rd& r : tk_[(size_t)i].rd)
        if (r.f.pos == Verb && r.f.mood == Infinitive && r.f.tense == Present && r.f.voice != Passive) inf = true;
      if (finiteBest(i) && !inf) return;   // a real finite verb: not a historic infinitive sentence
      if (inf) infs.push_back(i);
    }
    if (infs.empty() || s_.finalPunct == "?") return;
    // subjects: the nominative of each stretch (commas / et split stretches)
    int subj = -1;
    const Rd* subjR = nullptr;
    int start = 0;
    std::vector<std::pair<int, std::pair<uint8_t, uint8_t>>> plan;   // (inf, (person, number))
    for (int inf : infs) {
      int a = inf;
      while (a > start && !tk_[(size_t)(a - 1)].punct() && key0(a - 1) != "et") --a;
      for (int j = a; j < inf; ++j)
        for (const Rd& r : tk_[(size_t)j].rd)
          if ((nominal(r) || r.key == "omnis") && r.f.case_ == Nom) { subj = j; subjR = &r; break; }
      if (!subjR) return;   // no subject: not historic
      uint8_t person = subjR->f.person ? subjR->f.person : (uint8_t)P3;
      if (subjR->key == "ego" || subjR->key == "nos") person = P1;
      if (subjR->key == "tu" || subjR->key == "uos") person = P2;
      plan.push_back({inf, {person, pnum(*subjR)}});
      start = inf + 1;
    }
    (void)subj;
    std::vector<std::string> forms;
    for (const auto& p : plan) {
      const Rd* r = nullptr;
      for (const Rd& x : tk_[(size_t)p.first].rd)
        if (x.f.pos == Verb && x.f.mood == Infinitive) { r = &x; break; }
      std::string w;
      reinterpret(p.first, *r);
      const uint32_t v2 = pick(r->lemma, -1);
      if (!gen(v2, morph::verbForm(p.second.first, p.second.second, Imperfect, Indicative, Active), w)) return;
      forms.push_back(w);
    }
    const int ch = newChange("structure", "histinf", "historic infinitive -> imperfect indicative");
    for (size_t k = 0; k < plan.size(); ++k) setWords(plan[k].first, forms[k], ch);
  }

  // ---- rules: relative clause chains -----------------------------------------------------------------------------------------
  bool relPron(int i) const {
    if (i <= 0 || !tk_[(size_t)i].word()) return false;
    for (const Rd& r : tk_[(size_t)i].rd)
      if (r.key == "qui" && (r.lpos == Pron || r.lpos == Det)) return true;
    return false;
  }
  void relativeChain() {
    if (s_.finalPunct == "?") return;
    std::vector<int> rel;
    for (int i = 0; i < (int)tk_.size(); ++i)
      if (relPron(i) && !touched_[(size_t)i]) rel.push_back(i);
    if (rel.size() < 2) return;
    const int r1 = rel[rel.size() - 2], r2 = rel.back();
    // a finite verb before r1 (the main clause), one between r1 and r2 (the first relative clause), exactly one after r2
    auto finites = [&](int a, int b) {
      int n = 0;
      for (int j = a; j < b; ++j) n += finiteBest(j) ? 1 : 0;
      return n;
    };
    if (finites(0, r1) < 1 || finites(r1 + 1, r2) < 1 || finites(r2 + 1, (int)tk_.size()) != 1) return;
    // antecedent: the nearest noun between r1 and r2 agreeing with a reading of r2
    for (int j = r2 - 1; j > r1; --j) {
      if (!tk_[(size_t)j].word()) continue;
      for (const Rd& nr : tk_[(size_t)j].rd) {
        if (!noun(nr)) continue;
        for (const Rd& rr : tk_[(size_t)r2].rd) {
          if (rr.key != "qui" || !rr.f.case_) continue;
          if (pnum(rr) != pnum(nr) || !gok(rr.gender(), nr.gender())) continue;
          Features f = nr.f;
          f.case_ = rr.f.case_;
          std::string w;
          if (nr.lemma == kNone || !gen(nr.lemma, f, w)) continue;
          if (nr.name && !startsUpper(w)) w = capitalise(w);
          const int ch = newChange("structure", "relchain", "relative clause inside a relative clause -> new sentence");
          if (r2 > 0 && tk_[(size_t)(r2 - 1)].punct() && tk_[(size_t)(r2 - 1)].text == ",") del(r2 - 1, ch);
          set(r2, w, ch, nr.name);
          insert(r2, false, {"."}, ch, true);
          if (nr.lemma != kNone) out_.mapped.emplace_back(nr.lemma, nr.lemma);
          return;
        }
      }
    }
  }

  // ---- rules: long sentences ---------------------------------------------------------------------------------------------------
  void splitLong() {
    int words = 0;
    for (const Tk& t : tk_) words += t.word() ? 1 : 0;
    const int limit = std::atoi(R_.param("split", "words", "12").c_str());
    if (words <= limit || s_.finalPunct == "?") return;
    const std::vector<std::string> conj = split(R_.param("split", "conj", "et sed nam atque"));
    std::vector<int> cands;
    for (int i = 1; i < (int)tk_.size(); ++i)
      if (tk_[(size_t)i].word() && !touched_[(size_t)i] &&
          std::find(conj.begin(), conj.end(), key0(i)) != conj.end() && !s_.tokens[(size_t)i].encl.size())
        cands.push_back(i);
    int from = 0;
    for (size_t k = 0; k < cands.size(); ++k) {
      const int c = cands[k];
      const int to = k + 1 < cands.size() ? cands[k + 1] : (int)tk_.size();
      bool left = false, right = false;
      for (int j = from; j < c; ++j) left = left || finiteBest(j);
      for (int j = c + 1; j < to; ++j) right = right || finiteBest(j);
      // not inside a subordinate clause: no subordinator between `from` and c without its verb
      bool sub = false;
      for (const la2x::Clause& cl : s_.clauses)
        if (cl.parent >= 0 && cl.first < c && cl.last > c) sub = true;
      if (!left || !right || sub) continue;
      const std::string& kk = key0(c);
      const int ch = newChange("structure", "split", "long sentence split at \"" + tk_[(size_t)c].text + "\"");
      if (tk_[(size_t)(c - 1)].punct() && tk_[(size_t)(c - 1)].text == ",") del(c - 1, ch);
      if (kk == "et" || kk == "atque") {
        del(c, ch);
        const int nx = c + 1;
        if (nx < (int)tk_.size()) {
          insert(nx, false, {"."}, ch, true);
          span(ch, nx);
        }
      } else {
        insert(c, false, {"."}, ch, true);
        set(c, tk_[(size_t)c].text, ch);
      }
      from = c + 1;
    }
  }

  // ---- marks: accusative + infinitive kept ---------------------------------------------------------------------------------
  void accInfMark() {
    static const char* sayThink[] = {"dico", "puto", "scio", "credo", "nuntio", "audio", "video", "narro",
                                     "respondeo", "cogito", "intellego", "nego", "existimo", "arbitror", "sentio"};
    for (int v = 0; v < (int)tk_.size(); ++v) {
      if (!finiteBest(v)) continue;
      const std::string& k = key0(v);
      bool st = false;
      for (const char* x : sayThink) st = st || k == x;
      if (!st) continue;
      int acc = -1, inf = -1;
      for (int j = 0; j < (int)tk_.size(); ++j) {
        if (j == v || !tk_[(size_t)j].word()) continue;
        const Rd* r = tk_[(size_t)j].best();
        if (!r) continue;
        if (acc < 0 && nominal(*r) && r->f.case_ == Acc) acc = j;
        if (r->f.pos == Verb && r->f.mood == Infinitive) inf = j;
      }
      if (acc < 0 || inf < 0) continue;
      out_.notes.push_back(rules::Reason{-1, "orbergise", "accusative + infinitive kept after " + tk_[(size_t)v].text,
                                         "{\"was\":\"" + jsonEscape(tk_[(size_t)v].text) + "\",\"now\":\"" +
                                             jsonEscape(tk_[(size_t)v].text) +
                                             "\",\"why\":\"structure kept: accusative + infinitive (a first-year "
                                             "structure)\"}"});
      return;
    }
  }

  // ---- vocabulary --------------------------------------------------------------------------------------------------------------
  void vocabulary() {
    for (int i = 0; i < (int)tk_.size(); ++i) {
      if (!tk_[(size_t)i].word() || touched_[(size_t)i] || tk_[(size_t)i].rd.empty()) continue;
      if (periphrastic(i)) continue;
      const Rd& r = tk_[(size_t)i].rd[0];
      if (r.lemma == kNone) continue;
      if (r.name && opt_.keepNames) continue;
      if (R_.wordTier(r.lemma) <= ceiling_) continue;
      // another reading of the same word is within the ceiling (the checker accepts the word): leave it
      bool other = false;
      for (const Rd& x : tk_[(size_t)i].rd)
        if (x.lemma != kNone && R_.wordTier(x.lemma) <= ceiling_ && x.lpos == r.lpos) other = true;
      if (other) continue;
      if (R_.isParticipleLemma(r.lemma)) { participleSwap(i, r); continue; }
      const Swap s = R_.swapFor(r.lemma, ceiling_, opt_.keepNames);
      if (s.lemma == kNone) continue;
      Features f = r.f;
      const bool depFrom = deponent(r.lemma), depTo = deponent(s.lemma);
      std::string w;
      if (r.f.pos == Verb || r.lpos == Verb) {
        if (depFrom) f.voice = Active;
        else if (f.voice == Passive && depTo) continue;
        if (f.mood == ParticipleMood) continue;   // handled with the participle lemma
        if (!gen(s.lemma, f, w)) continue;
      } else if (r.lpos == Adv || r.f.pos == Adv || r.lpos == Conj || r.lpos == Prep || r.lpos == Intj ||
                 r.lpos == Particle) {
        w = head(s.lemma);
      } else {
        if (!f.number) f.number = Sg;
        f.case_ = caseOf(i, r);
        if (!gen(s.lemma, f, w)) continue;
      }
      std::vector<std::pair<int, std::string>> mods;
      if (noun(r)) {
        const uint8_t g0 = r.lgender, g1 = la_.lemma(s.lemma).gender;
        if (g0 && g1 && g0 != g1) {
          bool ok = true;
          for (const auto& m : npOf(i, r)) {
            if (m.first == i) continue;
            Features mf = m.second->f;
            mf.gender = one(g1);
            std::string mw;
            if (m.second->lemma == kNone || !gen(m.second->lemma, mf, mw)) { ok = false; break; }
            mods.emplace_back(m.first, mw);
          }
          if (!ok) continue;
        }
      }
      std::string all = s.before.empty() ? w : s.before + " " + w;
      if (!s.after.empty()) all += " " + s.after;
      const int ch = newChange("vocabulary", s.rule.c_str(), s.why);
      if (s.rule == "synonym" && std::find(out_.flags.begin(), out_.flags.end(), "synonym") == out_.flags.end())
        out_.flags.push_back("synonym");   // a lexicon synonym (not a curated row): the teacher confirms (Check)
      setWords(i, all, ch);
      for (const auto& m : mods) set(m.first, m.second, ch);
      out_.mapped.emplace_back(r.lemma, s.lemma);
    }
  }

  // A participle lemma above the ceiling: its verb swapped, the participle regenerated (same deponency only).
  void participleSwap(int i, const Rd& r) {
    const uint32_t v = R_.verbOf(r.lemma);
    if (v == kNone || R_.tier(v) <= ceiling_) return;
    const Swap s = R_.swapFor(v, ceiling_, opt_.keepNames);
    if (s.lemma == kNone || !s.before.empty() || !s.after.empty() || deponent(v) != deponent(s.lemma)) return;
    const PK k = partKind(r);
    Features f = morph::participle(k == PPres ? Present : k == PFut || k == PGerundive ? Future : Perfect,
                                   k == PGerundive || (k == PPerf && !deponent(v)) ? Passive : Active, pcase(r), pnum(r),
                                   r.gender() ? one(r.gender()) : (uint8_t)M);
    std::string w;
    if (!gen(s.lemma, f, w)) return;
    const int ch = newChange("vocabulary", s.rule.c_str(), s.why);
    set(i, w, ch);
    out_.mapped.emplace_back(v, s.lemma);
  }

  // Perfect participle + sum (perfect passive, deponent perfect) with a verb above the ceiling: regenerated from the
  // swapped verb ("ingressus est" -> "intrāvit"). True when the pair was handled (or must be left alone).
  bool periphrastic(int i) {
    if (i + 1 >= (int)tk_.size() || touched_[(size_t)(i + 1)]) return false;
    const Rd* sr = tk_[(size_t)(i + 1)].best();
    if (!sr || !isSum(*sr) || !finite(*sr)) return false;
    const Rd* pr = nullptr;
    for (const Rd& r : tk_[(size_t)i].rd)
      if (partKind(r) == PPerf && pcase(r) == Nom) { pr = &r; break; }
    if (!pr) return false;
    const uint32_t v = verbOfRd(*pr);
    if (v == kNone) return false;
    if (R_.tier(v) <= ceiling_) return true;   // fine as it is (the participle counts with its verb)
    const Swap s = R_.swapFor(v, ceiling_, opt_.keepNames);
    if (s.lemma == kNone || !s.before.empty() || !s.after.empty()) return true;
    const bool active = deponent(v);
    if (!active && deponent(s.lemma)) return true;
    const uint8_t t = sr->f.tense == Present ? Perfect : sr->f.tense == Imperfect ? Pluperfect
                      : sr->f.tense == Future ? FuturePerfect : 0;
    if (!t) return true;
    Features f = morph::verbForm(sr->f.person ? sr->f.person : (uint8_t)P3, pnum(*sr), t, sr->f.mood,
                                 active ? (uint8_t)Active : (uint8_t)Passive);
    f.gender = pr->gender() ? one(pr->gender()) : (uint8_t)M;
    std::string w;
    if (!gen(s.lemma, f, w)) return true;
    reinterpret(i, *pr);
    const int ch = newChange("vocabulary", s.rule.c_str(), s.why);
    setWords(i, w, ch);
    del(i + 1, ch);
    out_.mapped.emplace_back(v, s.lemma);
    return true;
  }

  // ---- output --------------------------------------------------------------------------------------------------------------
  std::string joinSource(int a, int b) const {
    std::string o;
    bool open = false;
    for (int i = a; i <= b && i < (int)tk_.size(); ++i) {
      const std::string& t = tk_[(size_t)i].text;
      if (!o.empty() && !open && !(tk_[(size_t)i].punct() && attachLeft(t))) o += ' ';
      o += t;
      open = tk_[(size_t)i].punct() && opensRight(t);
    }
    return o;
  }

  void finish() {
    if (ch_.empty()) {
      out_.text = in_.text;
      out_.changed = false;
      return;
    }
    out_.changed = true;
    std::string text;
    std::vector<int> startOf(sl_.size(), -1), endOf(sl_.size(), -1);
    bool sentenceStart = true, open = false;
    for (size_t k = 0; k < sl_.size(); ++k) {
      Slot& s = sl_[k];
      if (s.del) continue;
      if (s.punct) {
        if (!text.empty() && !open && !attachLeft(s.text)) text += ' ';
        startOf[k] = (int)text.size();
        text += s.text;
        endOf[k] = (int)text.size();
        if (endsSent(s.text)) sentenceStart = true;
        open = opensRight(s.text);
        continue;
      }
      std::string w = s.text;
      if (sentenceStart) w = capitalise(w);
      else if (!s.name && (s.isNew || s.src == firstWord_)) {
        bool keepCap = false;
        if (s.src >= 0 && s.src != firstWord_) keepCap = tk_[(size_t)s.src].cap;
        if (!keepCap) w = decapitalise(w);
      }
      if (!text.empty() && !open) text += ' ';
      startOf[k] = (int)text.size();
      text += w;
      endOf[k] = (int)text.size();
      sentenceStart = false;
      open = false;
    }
    out_.text = text;
    // changes: from = the source words of the span, to = the output between the first and last slot of the span
    for (size_t c = 0; c < ch_.size(); ++c) {
      const Ch& h = ch_[c];
      Change o;
      o.reason = h.reason;
      o.rule = h.rule;
      o.why = h.why;
      o.from = h.b >= 0 ? joinSource(h.a, h.b) : std::string();
      int first = -1, last = -1, firstNew = -1;
      for (size_t k = 0; k < sl_.size(); ++k) {
        if (sl_[k].del) continue;
        const bool in = sl_[k].change == (int)c || (sl_[k].src >= h.a && sl_[k].src <= h.b);
        if (!in) continue;
        if (first < 0) first = (int)k;
        last = (int)k;
        if (firstNew < 0 && sl_[k].change == (int)c && !sl_[k].punct) firstNew = (int)k;
      }
      if (first >= 0) {
        // trim punctuation at the ends of "to"
        while (first <= last && sl_[(size_t)first].punct) ++first;
        while (last >= first && sl_[(size_t)last].punct && !endsSent(sl_[(size_t)last].text)) --last;
        while (last >= first && sl_[(size_t)last].del) --last;
        if (first <= last && startOf[(size_t)first] >= 0 && endOf[(size_t)last] >= 0)
          o.to = text.substr((size_t)startOf[(size_t)first], (size_t)(endOf[(size_t)last] - startOf[(size_t)first]));
      }
      if (firstNew < 0 && first >= 0)   // only punctuation was inserted (a split): the first word after it
        for (int k = first; k <= last && k < (int)sl_.size(); ++k)
          if (!sl_[(size_t)k].del && !sl_[(size_t)k].punct && startOf[(size_t)k] >= 0) { firstNew = k; break; }
      o.tokenIndex = firstNew >= 0 ? startOf[(size_t)firstNew] : -1;
      out_.changes.push_back(std::move(o));
    }
  }
};

}  // namespace

void rewriteSentence(const SentenceIn& in, Resources::Impl& R, SentenceOut& out) {
  Rewriter rw(in, R, out);
  rw.run();
}

}  // namespace vp::orberg::detail
