// English realisation of the Latin frames (plain, literal): SVO, articles by definiteness heuristics, tense mapping
// reversed (present -> simple present, imperfect -> was ...ing / simple past for states, perfect -> simple past,
// pluperfect -> had, future -> will), pronouns restored from the verb, do-support for questions and negation,
// imperatives, subordinators from readable_en.tsv.
#include <algorithm>

#include "internal.h"
#include "vp/text.h"

namespace vp::la2x::detail {

using namespace vp::feat;
using frame::Kind;
using frame::Relation;
using frame::Role;
using frame::SemFrame;
using frame::SemNP;

namespace {

enum class R : uint8_t { Subj, Obj, Iobj, Prep, Pred, Voc, Poss };

bool isKey(const std::string& k, std::initializer_list<const char*> ks) {
  for (const char* x : ks)
    if (k == x) return true;
  return false;
}

std::string join(const std::vector<std::string>& w) {
  std::string out;
  for (const std::string& x : w) appendWord(out, x);
  return out;
}

struct Agree { int person = 3, number = 1; uint8_t gender = 0; bool personNoun = true; };

class En {
 public:
  En(const RealiseIn& in, std::vector<std::string>& flags) : in_(in), s_(*in.s), ti_(*in.ti), tab_(*in.tab), flags_(flags) {}

  std::string sentence(const Built& b) {
    std::string out;
    for (size_t k = 0; k < b.frames.size(); ++k) {
      std::string c = clause(b.frames[k], nullptr, nullptr);
      if (k > 0) {
        const std::string& j = b.joiners[k - 1];
        if (j == "," || j == ";" || j == ":") out += j;
        else {
          std::string w = tab_.text("conj", j, {"-"});
          if (w.empty()) w = "and";
          if (!out.empty() && (j == "sed" || j == "at" || j == "autem")) out += ",";
          appendWord(out, w);
        }
      }
      appendWord(out, c);
    }
    std::string punct = b.frames.empty() ? "." : b.frames[0].punct;
    if (punct.empty() || punct == ";") punct = ".";
    if (punct == "..." ) punct = "...";
    out = capitaliseFirst(out);
    while (!out.empty() && (out.back() == ',' || out.back() == ' ')) out.pop_back();
    out += punct;
    return out;
  }

 private:
  const RealiseIn& in_;
  const Sentence& s_;
  const std::vector<TokInfo>& ti_;
  const Tables& tab_;
  std::vector<std::string>& flags_;
  Agree mainSubj_;

  void flag(const std::string& f) {
    if (std::find(flags_.begin(), flags_.end(), f) == flags_.end()) flags_.push_back(f);
  }

  static std::string code(int person, int number, uint8_t gender) {
    std::string c = std::to_string(person < 1 || person > 3 ? 3 : person) + (number == 2 ? "pl" : "sg");
    if (person == 3 && number != 2) c += gender == F ? ".f" : gender == N ? ".n" : ".m";
    return c;
  }
  std::string pron(int person, int number, uint8_t gender, const char* role) const {
    std::string t = tab_.text("pron", code(person, number, gender), {role});
    if (t.empty() && person == 3) t = tab_.text("pron", "3sg.m", {role});
    return t;
  }
  static const char* roleName(R r) {
    switch (r) {
      case R::Subj: return "subj";
      case R::Obj: return "obj";
      case R::Iobj: return "iobj";
      case R::Prep: return "prep";
      case R::Poss: return "poss";
      default: return "subj";
    }
  }

  Lexical lexOf(int tok) const {
    if (tok < 0 || (size_t)tok >= ti_.size()) return Lexical();
    return in_.lex->lexical(ti_[(size_t)tok], Target::En);
  }

  // gender for "he / she / it" of a 3rd-person reference to a noun
  uint8_t refGender(const SemNP& np) const {
    if (np.isPronoun) return np.pron.gender;
    if (np.token < 0) return 0;
    const TokInfo& t = ti_[(size_t)np.token];
    if (t.name) return t.f.gender ? t.f.gender : t.lgender ? t.lgender : (uint8_t)M;
    const Lexical lx = lexOf(np.token);
    if (!lx.person) return N;
    const uint8_t g = t.f.gender ? t.f.gender : t.lgender;
    return g == F ? F : M;
  }

  Agree agreeOf(const SemNP& np) const {
    Agree a;
    if (!np.coord.empty()) {
      // "ego et tū" -> 1st plural; "tū et Mārcus" -> 2nd plural
      a.number = 2;
      a.person = 3;
      const int p0 = np.isPronoun && np.pron.person ? np.pron.person : 3;
      a.person = p0;
      for (const SemNP& c : np.coord) {
        const int pc = c.isPronoun && c.pron.person ? c.pron.person : 3;
        a.person = std::min(a.person, pc);
      }
      return a;
    }
    if (np.isPronoun) {
      a.person = np.pron.person ? np.pron.person : 3;
      a.number = np.pron.number == 2 ? 2 : 1;
      a.gender = np.pron.gender;
      return a;
    }
    a.person = 3;
    a.number = np.number == 2 ? 2 : 1;
    if (np.token >= 0 && !np.head.empty()) {
      const Lexical lx = lexOf(np.token);
      if (lx.mass) a.number = np.number == 2 ? 2 : 1;
      a.personNoun = lx.person || ti_[(size_t)np.token].name;
    }
    a.gender = refGender(np);
    return a;
  }

  // ---- adjectives -------------------------------------------------------------------------------------------------------
  std::string adjective(const frame::SemAdj& a) const {
    std::string w;
    if (a.token >= 0) {
      const TokInfo& t = ti_[(size_t)a.token];
      const Lexical lx = lexOf(a.token);
      w = lx.word;
      const bool participle = (t.f.pos == Participle || t.lpos == Participle) && !lx.adjective;
      if (participle) {
        const bool present = t.key.size() > 2 && t.key.compare(t.key.size() - 2, 2, "ns") == 0;
        w = en::verb(w, present ? en::VForm::Ing : en::VForm::PastPart);
      }
      const uint8_t deg = a.degree ? a.degree : t.f.degree;
      if (deg == Comparative) w = en::comparative(w);
      else if (deg == Superlative) w = "very " + w;
    }
    std::string out;
    for (const std::string& k : a.adverbs) appendWord(out, tab_.text("adv", k, {"-"}));
    appendWord(out, w);
    return out;
  }

  // ---- noun phrases ------------------------------------------------------------------------------------------------------
  std::string np(const SemNP& x, R role, bool definite = false) {
    if (!x.coord.empty()) {
      SemNP first = x;
      first.coord.clear();
      std::vector<std::string> parts;
      parts.push_back(np(first, role, definite));
      for (const SemNP& c : x.coord) parts.push_back(np(c, role, definite));
      std::string conj = tab_.text("conj", x.coordConj.empty() ? "et" : x.coordConj, {"-"});
      if (conj.empty() || conj == "and not") conj = "and";
      std::string out;
      for (size_t k = 0; k < parts.size(); ++k) {
        if (k > 0) {
          if (k + 1 < parts.size()) out += ",";
          else appendWord(out, conj);
        }
        appendWord(out, parts[k]);
      }
      return out;
    }
    if (x.interrogative) {
      if (!x.head.empty() && x.wh != "quis" && x.wh != "quid") {
        std::string out = tab_.text("wh", x.wh, {"-"});
        if (out.empty()) out = "which";
        for (const frame::SemAdj& a : x.adjectives) appendWord(out, adjective(a));
        appendWord(out, nounWord(x, role));
        return out;
      }
      const uint8_t c = x.token >= 0 ? ti_[(size_t)x.token].f.case_ : 0;
      const char* ck = c == Acc ? "acc" : c == Dat ? "dat" : c == Gen ? "gen" : c == Abl ? "abl" : "nom";
      std::string w = tab_.text("wh", x.wh, {ck, "nom"});
      if (role == R::Prep && w == "whom") return "whom";
      if (role == R::Prep && x.wh == "quis") return "whom";
      return w.empty() ? "what" : w;
    }
    if (x.isPronoun) {
      const std::string& k = x.pronLemma;
      if (x.pron.reflexive) {
        if (role == R::Subj) return pron(mainSubj_.person, mainSubj_.number, mainSubj_.gender, "subj");
        return pron(curSubj_.person, curSubj_.number, curSubj_.gender, "refl");
      }
      if (isKey(k, {"qui"})) return "who";
      if (isKey(k, {"quis", "quid"})) return tab_.text("wh", k, {"nom"});
      if (tab_.has("det", k) && !isKey(k, {"is"})) return standalone(x, role);
      uint8_t g = x.pron.gender;
      if (x.pron.person == 3 && x.pron.number != 2 && !g) g = M;
      return pron(x.pron.person ? x.pron.person : 3, x.pron.number == 2 ? 2 : 1, g, roleName(role));
    }
    if (x.head.empty() && !x.determiner.empty()) return standalone(x, role);
    if (x.isName && x.token >= 0) {
      std::string out = in_.lex->nameIn(ti_[(size_t)x.token], Target::En);
      for (const frame::SemAdj& a : x.adjectives) out = adjective(a) + " " + out;
      for (const SemNP& g : x.genitive) {
        appendWord(out, "of");
        appendWord(out, np(g, R::Prep, true));
      }
      if (!x.relative.empty()) appendWord(out, relative(x, x.relative[0]));
      return out;
    }
    if (x.token >= 0 && x.adjectives.size() == 1 && x.adjectives[0].token == x.token && x.head == x.adjectives[0].lemma) {
      // a substantive adjective ("bonī" the good ones; predicate handled elsewhere)
      std::string out = definite || role == R::Subj ? "the" : "";
      appendWord(out, adjective(x.adjectives[0]));
      appendWord(out, x.number == 2 ? "ones" : "one");
      return out;
    }
    // a quantifier / demonstrative standing alone as the head ("ab omnibus": by everyone)
    if (x.token >= 0 && x.determiner.empty() && x.adjectives.empty() && x.genitive.empty() && x.relative.empty() &&
        tab_.has("det", ti_[(size_t)x.token].key) &&
        (ti_[(size_t)x.token].lpos == Adj || ti_[(size_t)x.token].lpos == Det)) {
      SemNP y = x;
      y.determiner = ti_[(size_t)x.token].key;
      return standalone(y, role);
    }
    return nounPhrase(x, role, definite);
  }

  Agree curSubj_;

  std::string standalone(const SemNP& x, R role) {
    const uint8_t g = x.token >= 0 ? (ti_[(size_t)x.token].f.gender ? ti_[(size_t)x.token].f.gender : 0) : 0;
    const bool pl = x.number == 2;
    const std::string gk = g == F ? "f" : g == N ? "n" : "m";
    const std::string nk = pl ? "pl" : "sg";
    const std::string k = x.determiner.empty() ? x.pronLemma : x.determiner;
    // compared with a thing after quam ("hic equus celerior est quam ille"): that one, not he
    if (x.token > 0 && ti_[(size_t)x.token - 1].key == "quam" && ti_[(size_t)x.token - 1].lpos == Conj &&
        !curSubj_.personNoun && (k == "ille" || k == "hic" || k == "iste"))
      return k == "hic" ? (pl ? "these" : "this one") : (pl ? "those" : "that one");
    std::string w = tab_.text("det", k, {"pron." + gk + "." + nk, "pron." + nk, "pron"});
    if (w.empty()) w = tab_.text("det", k, {nk});
    if (w == "he" || w == "she" || w == "they" || w == "it") {
      return pron(3, pl ? 2 : 1, w == "she" ? F : w == "it" ? N : M, roleName(role));
    }
    if (w.empty()) w = s_.tokens[(size_t)std::max(0, x.token)].text;
    if (!x.relative.empty()) appendWord(w, relative(x, x.relative[0]));
    return w;
  }

  std::string nounWord(const SemNP& x, R role) {
    (void)role;
    const Lexical lx = lexOf(x.token);
    if (lx.missing) flag("gloss-missing");
    std::string w = lx.word;
    if (x.number == 2 && !lx.mass && !lx.invariable) w = en::plural(w);
    return w;
  }

  std::string nounPhrase(const SemNP& x, R role, bool definite) {
    const Lexical lx = lexOf(x.token);
    std::vector<std::string> pre;
    bool hasDet = false;
    // determiner / possessive / demonstrative / quantifier
    if (!x.determiner.empty()) {
      const std::string& d = x.determiner;
      std::string w;
      if (d == "suus") w = pron(curSubj_.person, curSubj_.number, curSubj_.gender, "poss");
      else w = tab_.text("det", d, {x.number == 2 ? "pl" : "sg"});
      if (!w.empty()) { pre.push_back(w); hasDet = true; }
    }
    for (const SemNP& p : x.possessor) {
      const int person = p.pron.person ? p.pron.person : 3;
      uint8_t g = p.pron.gender;
      if (person == 3 && !g) g = M;
      pre.push_back(pron(person, p.pron.number == 2 ? 2 : 1, g, "poss"));
      hasDet = true;
    }
    if (!x.numeral.empty()) {
      std::string w = tab_.text("det", x.numeral, {"pl", "sg"});
      pre.push_back(w.empty() ? x.numeral : w);
      hasDet = true;
    }
    std::vector<std::string> adjs;
    for (const frame::SemAdj& a : x.adjectives) {
      if (a.token >= 0 && !tab_.has("det", a.lemma)) adjs.push_back(adjective(a));
      else if (a.token >= 0) {
        std::string w = tab_.text("det", a.lemma, {x.number == 2 ? "pl" : "sg"});
        if (!w.empty()) pre.push_back(w);
      }
    }
    const uint32_t lemma = x.token >= 0 ? ti_[(size_t)x.token].lemma : lex::kNoLemma;
    if (!hasDet && role != R::Voc) {
      const bool mentioned = in_.disc->seen(lemma);
      const bool hasGen = !x.genitive.empty();
      // a restrictive relative clause makes the noun definite ("the book which you gave me")
      const bool hasRel = !x.relative.empty();
      bool def = definite || lx.unique || mentioned || hasGen || hasRel;
      bool indef = false;
      if (!def) {
        if (role == R::Subj || role == R::Iobj || role == R::Prep) def = true;
        else if (x.number != 2 && !lx.mass) indef = true;
      }
      if (lx.mass && !mentioned && !hasGen && (!definite || role == R::Prep) && !x.relative.size()) { def = false; indef = false; }
      if (x.number == 2 && !mentioned && !hasGen && !hasRel && (role == R::Obj || role == R::Pred)) def = false;
      if (role == R::Pred && x.number != 2 && !mentioned && !lx.mass && !lx.unique && !hasGen && !hasRel) { def = false; indef = true; }
      if (def) pre.push_back(tab_.text("article", "def", {"-"}));
      else if (indef) {
        const std::string next = !adjs.empty() ? adjs[0] : nounWord(x, role);
        pre.push_back(en::indefinite(next));
      }
    }
    std::string out = join(pre);
    for (const std::string& a : adjs) appendWord(out, a);
    appendWord(out, nounWord(x, role));
    in_.disc->mention(lemma);
    for (const SemNP& g : x.genitive) {
      appendWord(out, "of");
      appendWord(out, np(g, R::Prep, true));
    }
    if (!x.relative.empty()) appendWord(out, relative(x, x.relative[0]));
    return out;
  }

  // ---- verbs -----------------------------------------------------------------------------------------------------------
  bool stative(const std::string& w) const {
    static const char* const k[] = {"be", "have", "know", "want", "like", "love", "fear", "believe", "think", "live",
                                    "own", "hate", "need", "seem", "understand", "remember", "hope", "wish", "be able",
                                    "must", "not want", "not know", "be glad", "lie", "sit", "stand", "hold"};
    for (const char* x : k)
      if (w == x) return true;
    return false;
  }

  std::string beForm(const Agree& a, bool past) const {
    if (past) return (a.number == 2 || a.person == 2) ? "were" : "was";
    if (a.number == 2 || a.person == 2) return "are";
    return a.person == 1 ? "am" : "is";
  }
  std::string finiteOf(const std::string& base, const Agree& a, bool past) const {
    if (base == "be" || base.compare(0, 3, "be ") == 0) {
      std::string rest = base.size() > 2 ? base.substr(2) : std::string();
      return beForm(a, past) + rest;
    }
    if (past) return en::verb(base, en::VForm::Past);
    if (a.person == 3 && a.number != 2) return en::verb(base, en::VForm::S3);
    return base;
  }

  // The verb chain: words[0] is the finite element. `doSupport` true when words[0] is the bare simple verb (present
  // or past) so that negation / questions need do / does / did.
  struct Chain { std::vector<std::string> w; bool simple = false; bool past = false; std::string base; bool neg = false; };

  Chain chain(const SemFrame& f, const Agree& a, Relation rel, bool purpose) {
    Chain c;
    std::string base;
    int lexTok = f.pred.complementToken >= 0 && f.pred.particle == "peri" ? f.pred.complementToken : f.pred.token;
    if (f.pred.particle == "part-pres" || f.pred.particle == "part-perf") lexTok = f.pred.token;
    base = lexOf(lexTok).word;
    if (lexTok >= 0 && f.type == Kind::Wh && f.wh.word == "quid" && tab_.find("lex", ti_[(size_t)lexTok].key, "verb+wh"))
      base = tab_.find("lex", ti_[(size_t)lexTok].key, "verb+wh")->text;   // "quid facis?" what do you do
    if (lexTok >= 0 && reflexiveObject(f, a) && tab_.find("lex", ti_[(size_t)lexTok].key, "verb+refl")) {
      base = tab_.find("lex", ti_[(size_t)lexTok].key, "verb+refl")->text;
      reflDropped_ = true;
    }
    if (f.hasObject && lexTok >= 0) {
      // a verb whose meaning changes with an object ("manet" stays / "mē manet" waits for me)
      const std::string w = tab_.text("lex", ti_[(size_t)lexTok].key, {"verb+obj"});
      if (!w.empty() && tab_.find("lex", ti_[(size_t)lexTok].key, "verb+obj")) base = w;
    }
    if (base.empty()) base = "be";
    bool lexNeg = false;
    if (base.compare(0, 4, "not ") == 0) { base = base.substr(4); lexNeg = true; }   // nesciō, nōlō
    c.neg = lexNeg;
    const bool passive = f.pred.voice == frame::Voice::Passive;
    const frame::Tense t = f.pred.tense;
    const frame::Aspect asp = f.pred.aspect;
    const bool subj = f.pred.mood == frame::SrcMood::Subjunctive;
    c.base = base;
    // modal verbs with an infinitive
    if (!f.pred.complementVerb.empty() && !hasDisc(f, "jussive")) {
      const std::string m = f.pred.lemma;
      const std::string inf = lexOf(f.pred.complementToken).word;
      const bool past = t == frame::Tense::Past;
      if (m == "possum") {
        if (t == frame::Tense::Future) c.w = {"will", "be able to", inf};
        else c.w = {past ? "could" : "can", inf};
        c.past = past;
        return c;
      }
      if (m == "debeo") {
        if (t == frame::Tense::Future) c.w = {"will", "have to", inf};
        else if (past) c.w = {"had to", inf};
        else c.w = {"must", inf};
        return c;
      }
      if (m == "soleo") {
        c.w = {past ? "used to" : "usually", past ? inf : finiteOf(inf, a, false)};
        if (!past) { c.simple = true; c.base = inf; c.w = {finiteOf(inf, a, false)}; c.w.insert(c.w.begin(), "usually"); c.w = {"usually", finiteOf(inf, a, false)}; }
        return c;
      }
      // other verbs: chain of the verb + "to" + infinitive
      SemFrame g = f;
      g.pred.complementVerb.clear();
      Chain v = chain(g, a, rel, purpose);
      v.w.push_back("to");
      v.w.push_back(inf);
      v.neg = v.neg || lexNeg;
      return v;
    }
    if (f.type == Kind::Imp) {
      c.w = {base};
      c.simple = true;
      return c;
    }
    if (hasDisc(f, "infinitive")) {
      c.w = {"to", base};
      return c;
    }
    if (f.pred.particle == "part-pres") {   // ablative absolute, present participle
      c.w = {beForm(a, mainPast_), en::verb(base, en::VForm::Ing)};
      return c;
    }
    if (f.pred.particle == "part-perf") {
      c.w = {beForm(a, true), en::verb(base, en::VForm::PastPart)};
      return c;
    }
    // hortative "intrēmus" let us enter; deliberative "quid faciam?" what shall I do
    if (subj && !purpose && rel == Relation::Cause && t == frame::Tense::Present && f.type == Kind::Decl && a.person == 1 &&
        a.number == 2 && !inSub_) {
      c.w = {"let us", base};
      hortative_ = true;
      return c;
    }
    if (subj && !purpose && t == frame::Tense::Present && (f.type == Kind::Wh || f.type == Kind::Yn) && a.person == 1) {
      c.w = {"shall", base};
      return c;
    }
    if (subj && purpose) {
      if (t == frame::Tense::Past) {
        c.w = {"would", passive ? "be" : base};
        if (passive) c.w.push_back(en::verb(base, en::VForm::PastPart));
        return c;
      }
      if (passive) { c.w = {beForm(a, false), en::verb(base, en::VForm::PastPart)}; return c; }
      c.w = {finiteOf(base, a, false)};
      c.simple = base != "be";
      return c;
    }
    if (subj && rel == Relation::Coord) {}
    const std::string pp = en::verb(base, en::VForm::PastPart);
    if (passive) {
      if (t == frame::Tense::Present) c.w = {beForm(a, false), pp};
      else if (t == frame::Tense::Future) c.w = asp == frame::Aspect::Perfect ? std::vector<std::string>{"will", "have", "been", pp}
                                                                             : std::vector<std::string>{"will", "be", pp};
      else if (asp == frame::Aspect::Perfect) c.w = {"had", "been", pp};
      else c.w = {beForm(a, true), pp};
      return c;
    }
    if (t == frame::Tense::Present) {
      c.w = {finiteOf(base, a, false)};
      c.simple = base != "be" && base.compare(0, 3, "be ") != 0;
      if (!c.simple && base.size() > 3) { c.w = {beForm(a, false), base.substr(3)}; }
      return c;
    }
    if (t == frame::Tense::Future) {
      if (asp == frame::Aspect::Perfect) c.w = {"will", "have", pp};
      else c.w = {"will", base};
      return c;
    }
    // past
    c.past = true;
    if (asp == frame::Aspect::Perfect) { c.w = {"had", pp}; return c; }
    const bool be = base == "be" || base.compare(0, 3, "be ") == 0;
    if (asp == frame::Aspect::Progressive && !be && !stative(base) && !f.pred.habitual) {
      c.w = {beForm(a, true), en::verb(base, en::VForm::Ing)};
      return c;
    }
    if (be) {
      c.w = {beForm(a, true)};
      if (base.size() > 3) c.w.push_back(base.substr(3));
      return c;
    }
    c.w = {en::verb(base, en::VForm::Past)};
    c.simple = true;
    return c;
  }
  bool mainPast_ = false;
  bool reflDropped_ = false, hortative_ = false, inSub_ = false;

  static bool hasDisc(const SemFrame& f, const std::string& d) {
    return std::find(f.discourse.begin(), f.discourse.end(), d) != f.discourse.end();
  }

  // do-support: turns {"loves"} into {"does", "love"}
  void doSupport(Chain& c, const Agree& a) {
    if (!c.simple || c.w.empty()) return;
    std::string head = c.w[0];
    size_t idx = 0;
    if (head == "usually" && c.w.size() > 1) idx = 1;
    const std::string doForm = c.past ? "did" : (a.person == 3 && a.number != 2 ? "does" : "do");
    std::vector<std::string> w;
    for (size_t k = 0; k < idx; ++k) w.push_back(c.w[k]);
    w.push_back(doForm);
    w.push_back(c.base);
    for (size_t k = idx + 1; k < c.w.size(); ++k) w.push_back(c.w[k]);
    c.w = w;
    c.simple = false;
  }

  static std::string negContract(const std::string& aux) {
    static const char* const k[][2] = {{"does", "doesn't"}, {"do", "don't"}, {"did", "didn't"}, {"is", "isn't"},
                                       {"are", "aren't"},   {"was", "wasn't"}, {"were", "weren't"}, {"can", "can't"},
                                       {"could", "couldn't"}, {"will", "won't"}, {"has", "hasn't"}, {"have", "haven't"},
                                       {"had", "hadn't"}, {"must", "mustn't"}};
    for (const auto& x : k)
      if (aux == x[0]) return x[1];
    return aux;
  }

  // ---- relative clause ------------------------------------------------------------------------------------------------
  std::string relative(const SemNP& ante, const SemFrame& rf) {
    const Agree a = agreeOf(ante);
    const bool person = a.personNoun || ante.isName || ante.isPronoun;
    std::string pronoun;
    std::string out;
    SemFrame f = rf;
    switch (f.wh.role) {
      case Role::Subject: pronoun = person ? "who" : "which"; break;
      case Role::Object: pronoun = person ? "whom" : "which"; break;
      case Role::IndirectObject: pronoun = person ? "to whom" : "to which"; break;
      case Role::Oblique: {
        std::string pk = f.wh.word;
        const size_t colon = pk.find(':');
        const std::string prep = pk.substr(0, colon), cs = colon == std::string::npos ? "abl" : pk.substr(colon + 1);
        std::string p = tab_.text("prep", prep, {cs});
        pronoun = (p.empty() ? std::string("in") : p) + (person ? " whom" : " which");
        break;
      }
      default: pronoun = person ? "who" : "which"; break;
    }
    out = pronoun;
    Agree save = curSubj_;
    if (f.wh.role == Role::Subject) {
      f.hasSubject = false;
      appendWord(out, body(f, a, true));
    } else {
      appendWord(out, body(f, Agree(), false));
    }
    curSubj_ = save;
    return out;
  }

  // ---- clause --------------------------------------------------------------------------------------------------------
  // body: subject + verb + complements (declarative order); `forced` = agreement given by the caller (relative subject)
  std::string body(const SemFrame& f, const Agree& forcedAgree, bool forced, Relation rel = Relation::Cause,
                   bool purpose = false, bool sameSubjectInf = false) {
    Agree a = forced ? forcedAgree : (f.hasSubject ? agreeOf(f.subject) : Agree());
    if (!forced && f.hasSubject && f.implicitSubject) {
      a.person = f.subject.pron.person ? f.subject.pron.person : 3;
      a.number = f.subject.pron.number == 2 ? 2 : 1;
      a.gender = f.subject.pron.gender;
    }
    curSubj_ = a;
    std::vector<std::string> pre, post;
    std::string subj;
    if (f.hasSubject && !forced && !f.existential) subj = subjectText(f);
    Chain c = f.hasPred ? chain(f, a, rel, purpose) : Chain();
    if (sameSubjectInf && f.hasPred) {
      std::string base = lexOf(f.pred.token).word;
      c.w = {f.negative ? "not to" : "to", base};
      subj.clear();
    }
    const bool neg = (f.negative != c.neg) && !(sameSubjectInf);
    std::vector<std::string> preAdv, endAdv;
    adverbs(f, preAdv, endAdv);
    if (neg && f.hasPred) {
      doSupport(c, a);
      if (!c.w.empty()) {
        if (c.w[0] == "can") c.w[0] = "cannot";
        else c.w.insert(c.w.begin() + 1, "not");
      }
    }
    // "pre" adverbs (always, often, never) after the first auxiliary or before a simple verb
    if (!preAdv.empty() && !c.w.empty()) {
      const size_t at = (c.simple || c.w.size() == 1) && !(c.w[0] == "is" || c.w[0] == "are" || c.w[0] == "am" ||
                                                          c.w[0] == "was" || c.w[0] == "were")
                            ? 0
                            : (neg ? 2 : 1);
      c.w.insert(c.w.begin() + (long)std::min(at, c.w.size()), preAdv.begin(), preAdv.end());
    }
    std::string out;
    if (hortative_) { subj.clear(); hortative_ = false; }
    if (std::find(f.discourse.begin(), f.discourse.end(), "all") != f.discourse.end() && !subj.empty()) subj += " all";
    if (f.existential && f.hasSubject && !forced) {
      // "there is / was / will be" + the subject as an indefinite noun phrase
      std::vector<std::string> w = c.w;
      std::string thereSubj = np(f.subject, R::Obj);
      appendWord(out, "there");
      appendWord(out, join(w));
      appendWord(out, thereSubj);
      appendWord(out, complements(f, a));
      for (const std::string& e : endAdv) appendWord(out, e);
      appendWord(out, subordinates(f, false));
      return out;
    }
    if (locInversion(f) && !subj.empty()) {
      // "Hīc est puer" -> "Here is the boy"
      appendWord(out, join(c.w));
      appendWord(out, subj);
    } else {
      appendWord(out, subj);
      appendWord(out, join(c.w));
    }
    appendWord(out, complements(f, a));
    for (const std::string& e : endAdv) appendWord(out, e);
    appendWord(out, subordinates(f, false));
    return out;
  }

  // an adverb the Latin put first stays first when it is a sentence adverb (time, "front") or "hīc est"
  bool frontable(const SemFrame& f, const frame::SemAdverb& a) const {
    if (a.lemma == "#tok") return true;
    if (tab_.tagged("adv", a.lemma, "front") || tab_.tagged("adv", a.lemma, "time")) return true;
    if (!tab_.has("adv", a.lemma)) return true;
    return locInversion(f) && isKey(a.lemma, {"hic", "ibi", "ecce"});
  }

  static bool locInversion(const SemFrame& f) {
    if (!f.copula || !f.hasSubject || f.implicitSubject || f.subject.isPronoun) return false;
    if (!f.predAdj.empty() || !f.predicative.empty()) return false;
    for (const frame::SemAdverb& a : f.adverbs)
      if (a.front && isKey(a.lemma, {"hic", "ibi", "ecce"})) return true;
    return false;
  }

  // an object pronoun of the subject's own person ("inclīnāte vōs", "mē lavō"): reflexive
  bool reflexiveObject(const SemFrame& f, const Agree& a) const {
    if (!f.hasObject || !f.object.isPronoun || !f.object.coord.empty()) return false;
    if (f.object.pron.reflexive) return true;
    const int p = f.object.pron.person ? f.object.pron.person : 3;
    if (p == 3) return false;
    const int n = f.object.pron.number == 2 ? 2 : 1;
    return p == a.person && n == a.number;
  }

  std::string subjectText(const SemFrame& f) {
    if (!f.hasSubject) return std::string();
    if (f.implicitSubject) {
      int person = f.subject.pron.person ? f.subject.pron.person : 3;
      int number = f.subject.pron.number == 2 ? 2 : 1;
      uint8_t g = f.subject.pron.gender;
      if (person == 3 && number == 1) {
        if (!g || g == MFN || g == MF) g = in_.disc->lastSubjGender ? in_.disc->lastSubjGender : (uint8_t)M;
      }
      return pron(person, number, g, "subj");
    }
    return np(f.subject, R::Subj);
  }

  std::string complements(const SemFrame& f, const Agree& a) {
    std::string out;
    const bool jussive = hasDisc(f, "jussive");
    // copula predicate
    for (const std::string& d : f.discourse)
      if (d.compare(0, 12, "phrase-pred:") == 0) appendWord(out, d.substr(12));
    if (f.copula || (!f.hasPred && (!f.predAdj.empty() || !f.predicative.empty()))) {
      std::vector<std::string> pp;
      for (const frame::SemAdj& x : f.predAdj) pp.push_back(adjective(x));
      for (const SemNP& x : f.predicative) pp.push_back(np(x, R::Pred));
      for (size_t k = 0; k < pp.size(); ++k) {
        if (k > 0) appendWord(out, k + 1 == pp.size() ? "and" : ",");
        appendWord(out, pp[k]);
      }
    } else if (!f.predAdj.empty() || !f.predicative.empty()) {
      // secondary predicate ("laeta cantat"): after the verb
      for (const frame::SemAdj& x : f.predAdj) appendWord(out, adjective(x));
      for (const SemNP& x : f.predicative) appendWord(out, np(x, R::Pred));
    }
    const bool giveSay = isKey(f.pred.lemma, {"do", "dico", "narro", "ostendo", "monstro", "trado", "mitto", "porto",
                                              "scribo", "reddo", "dono", "fero", "affero", "praebeo"});
    const bool ioPron = f.hasIndirect && f.indirectObject.isPronoun && f.indirectObject.coord.empty() && f.hasObject;
    if (f.hasIndirect && ((!f.hasObject && !giveSay) || ioPron)) appendWord(out, np(f.indirectObject, R::Iobj));
    if (f.hasObject) {
      if (reflDropped_) reflDropped_ = false;
      else if (reflexiveObject(f, curSubj_)) appendWord(out, pron(curSubj_.person, curSubj_.number, curSubj_.gender, "refl"));
      else appendWord(out, np(f.object, R::Obj));
    }
    if (jussive && f.pred.complementToken >= 0) {
      appendWord(out, "to");
      appendWord(out, lexOf(f.pred.complementToken).word);
    }
    for (const frame::SemOblique& o : f.obliques)
      if (o.prep == "#acc") appendWord(out, np(o.np, R::Obj));
    if (f.hasIndirect && (f.hasObject || giveSay) && !ioPron) {
      appendWord(out, "to");
      appendWord(out, np(f.indirectObject, R::Prep, true));
    }
    for (const frame::SemOblique& o : f.obliques) {
      if (o.prep == "#acc") continue;
      appendWord(out, oblique(o));
    }
    (void)a;
    return out;
  }

  std::string oblique(const frame::SemOblique& o) {
    std::string p = o.prep;
    if (p == "#dat") return "to " + np(o.np, R::Prep, true);
    if (p == "#than") return "than " + np(o.np, R::Subj, true);
    if (p == "#gen") return "of " + np(o.np, R::Prep, true);
    if (p == "#loc") {
      std::string w = in_.lex->nameIn(ti_[(size_t)std::max(0, o.np.token)], Target::En);
      return "in " + (o.np.isName ? w : np(o.np, R::Prep, true));
    }
    if (p == "#abl") {
      const Lexical lx = lexOf(o.np.token);
      if (!lx.timePrep.empty()) {
        // "at night", "on the third day", "in the first hour"
        SemNP x = o.np;
        const bool bare = x.adjectives.empty() && x.determiner.empty() && x.number != 2 && lx.timePrep == "at";
        std::string out = lx.timePrep;
        if (bare) { appendWord(out, nounWord(x, R::Prep)); return out; }
        appendWord(out, np(x, R::Prep, true));
        return out;
      }
      return "with " + np(o.np, R::Obj, false);
    }
    if (p.size() > 1 && p[0] == '#') return np(o.np, R::Obj);
    const size_t colon = p.find(':');
    const std::string prep = p.substr(0, colon), cs = colon == std::string::npos ? "-" : p.substr(colon + 1);
    std::string w;
    const bool agent = (prep == "a" || prep == "ab") && curPassive_;
    if (agent) w = tab_.text("prep", prep, {"agent"});
    if (w.empty() && (prep == "post" || prep == "ante")) {
      const Lexical lx = lexOf(o.np.token);
      if (lx.timePrep.empty() && !lx.event) w = tab_.text("prep", prep, {cs + ".place"});
    }
    // "enter the temple", not "enter into"; "leave the house", not "leave out of": verbs tagged "direct-in" take
    // in + accusative, "direct-ex" ē/ex + ablative as a plain object
    if (!curVerbKey_.empty() && ((prep == "in" && cs == "acc" && tab_.tagged("lex", curVerbKey_, "direct-in")) ||
                                 (prep == "ex" && tab_.tagged("lex", curVerbKey_, "direct-ex"))))
      return np(o.np, R::Prep, true);
    if (w.empty() && prep == "ex" && !curVerbKey_.empty() && tab_.tagged("lex", curVerbKey_, "from-ex")) w = "from";
    if (w.empty()) w = tab_.text("prep", prep, {cs});
    if (w.empty()) w = s_.tokens[(size_t)std::max(0, o.token)].text;
    std::string out = w;
    appendWord(out, np(o.np, R::Prep, true));
    return out;
  }
  bool curPassive_ = false;
  std::string curVerbKey_;   // Latin key of the clause's lexical verb (the participle's verb in a periphrasis)

  void adverbs(const SemFrame& f, std::vector<std::string>& pre, std::vector<std::string>& end) {
    for (const frame::SemAdverb& a : f.adverbs) {
      if (a.front && !(tab_.tagged("adv", a.lemma, "pre") && f.hasPred) && frontable(f, a)) continue;
      if (a.lemma == "#tok") {
        end.push_back(leftover(a.token));
        continue;
      }
      std::string w = tab_.text("adv", a.lemma, {"-"});
      if (w.empty()) {
        w = lexOf(a.token).word;
        if (w.empty()) w = "[" + s_.tokens[(size_t)a.token].text + "]";
      }
      if (a.lemma == "quaeso") w = ", " + w;
      if (a.lemma == "ualde" && f.hasPred) w = "very much";
      // an adverb compared after quam ("clārius quam herī": than yesterday)
      if (a.token > 0 && ti_[(size_t)a.token - 1].key == "quam" && ti_[(size_t)a.token - 1].lpos == Conj) {
        end.push_back("than " + w);
        continue;
      }
      if (tab_.tagged("adv", a.lemma, "pre")) pre.push_back(w);
      else end.push_back(w);
    }
  }

  std::string leftover(int tok) {
    if (tok < 0) return std::string();
    const Token& t = s_.tokens[(size_t)tok];
    if (t.unknown) { flag("unknown"); return "[" + t.text + "]"; }
    if (t.kind == TokKind::Number) return t.text;
    const Lexical lx = lexOf(tok);
    if (!lx.word.empty()) return lx.word;
    return "[" + t.text + "]";
  }

  std::string frontAdverbs(const SemFrame& f) {
    std::string out;
    for (const frame::SemAdverb& a : f.adverbs) {
      if (!a.front) continue;
      if (a.lemma == "#tok") { appendWord(out, leftover(a.token)); continue; }
      if (tab_.tagged("adv", a.lemma, "pre") && f.hasPred) continue;   // with the verb ("never", "always")
      if (!frontable(f, a)) continue;
      std::string w = tab_.text("adv", a.lemma, {"-"});
      if (w.empty()) w = lexOf(a.token).word;
      appendWord(out, w);
    }
    return out;
  }

  std::string subordinates(const SemFrame& f, bool before) {
    std::string out;
    for (const frame::SemSub& ss : f.subordinate) {
      if (ss.before != before || ss.frame.empty()) continue;
      std::string c = subordinate(f, ss);
      if (before) {
        appendWord(out, c);
        out += ",";
      } else {
        if (ss.relation != Relation::Complement && ss.relation != Relation::Purpose && ss.relation != Relation::Relative &&
            ss.relation != Relation::Coord && !out.empty())
          out += "";
        appendWord(out, c);
      }
    }
    return out;
  }

  std::string subordinate(const SemFrame& main, const frame::SemSub& ss) {
    struct Flag { bool& b; bool old; Flag(bool& x) : b(x), old(x) { b = true; } ~Flag() { b = old; } } guard(inSub_);
    const SemFrame& sf = ss.frame[0];
    std::string marker;
    bool purpose = false, sameSubj = false;
    const bool subjMood = sf.pred.mood == frame::SrcMood::Subjunctive;
    Agree saveCur = curSubj_;
    if (ss.marker == "acc+inf") {
      marker = "that";
      // sē as the subject of the that-clause refers back to the main subject
      SemFrame g = sf;
      if (g.hasSubject && g.subject.isPronoun && g.subject.pron.reflexive) {
        g.subject.pron.reflexive = false;
        g.subject.pron.person = (uint8_t)mainSubj_.person;
        g.subject.pron.number = (uint8_t)mainSubj_.number;
        g.subject.pron.gender = mainSubj_.gender ? mainSubj_.gender : (uint8_t)M;
      }
      std::string out = marker;
      appendWord(out, body(g, Agree(), false));
      curSubj_ = saveCur;
      return out;
    }
    if (ss.marker == "ablabs-perf" || ss.marker == "ablabs-pres") {
      flag("abl-abs");
      std::string out = ss.marker == "ablabs-perf" ? "after" : "while";
      appendWord(out, body(sf, Agree(), false));
      curSubj_ = saveCur;
      return out;
    }
    if (ss.relation == Relation::Relative) {
      std::string out = sf.wh.role == Role::Object ? "what" : "who";
      SemFrame g = sf;
      appendWord(out, body(g, Agree(), false));
      return out;
    }
    if (ss.relation == Relation::Coord) {
      std::string out = tab_.text("conj", ss.marker, {"-"});
      if (out.empty()) out = "and";
      appendWord(out, body(sf, Agree(), false));
      curSubj_ = saveCur;
      return out;
    }
    if (ss.relation == Relation::Purpose) {
      purpose = true;
      // same subject (restored pronoun of the same person/number): "to" + infinitive
      if (sf.implicitSubject && main.hasSubject) {
        const Agree ma = mainSubj_;
        if ((sf.subject.pron.person ? sf.subject.pron.person : 3) == ma.person &&
            (sf.subject.pron.number == 2 ? 2 : 1) == ma.number)
          sameSubj = true;
      }
      if (sameSubj) {
        std::string out = sf.negative || ss.marker == "ne" ? "in order not to" : "to";
        SemFrame g = sf;
        g.negative = false;
        std::string b = lexOf(g.pred.token).word;
        appendWord(out, b);
        appendWord(out, complements(g, Agree()));
        curSubj_ = saveCur;
        return out;
      }
      marker = tab_.text("sub", ss.marker, {"subj", "-"});
      if (ss.marker == "ne") marker = "so that";
    } else {
      marker = tab_.text("sub", ss.marker, {subjMood ? "subj" : "ind", "-"});
    }
    if (marker.empty()) marker = ss.marker;
    SemFrame g = sf;
    if (ss.marker == "ne") g.negative = true;
    std::string out = marker;
    appendWord(out, body(g, Agree(), false, ss.relation, purpose));
    curSubj_ = saveCur;
    return out;
  }

  std::string clause(const SemFrame& f, const SemFrame* parent, const frame::SemSub* how) {
    (void)parent;
    (void)how;
    std::string out;
    curPassive_ = f.pred.voice == frame::Voice::Passive;
    {
      const int lt = f.pred.complementToken >= 0 && f.pred.particle == "peri" ? f.pred.complementToken : f.pred.token;
      curVerbKey_.clear();
      if (lt >= 0 && (size_t)lt < ti_.size())
        curVerbKey_ = ti_[(size_t)lt].verbLemma != lex::kNoLemma ? std::string(f.pred.lemma) : ti_[(size_t)lt].key;
    }
    mainPast_ = f.pred.tense == frame::Tense::Past;
    // fixed phrase covering the clause
    std::string phraseFront, phraseEnd;
    for (const std::string& d : f.discourse)
      if (d.compare(0, 7, "phrase:") == 0) {
        std::string t = d.substr(7);
        const size_t bar = t.find("|end");
        if (bar != std::string::npos) { t = t.substr(0, bar); appendWord(phraseEnd, t); }
        else appendWord(phraseFront, t);
      }
    // interjections + vocatives in front
    std::string head;
    for (const std::string& i : f.interjections) {
      std::string w = tab_.text("intj", i, {"-"});
      if (w.empty()) w = i;
      if (i == "o" && !f.vocatives.empty()) w = "O";
      appendWord(head, w);
      if (i != "o" || f.vocatives.empty()) head += ",";
    }
    std::string vocFront, vocEnd;
    int firstTok = 1 << 20;
    for (int t : f.tokens) firstTok = std::min(firstTok, t);
    const int predTok = f.pred.token >= 0 ? f.pred.token : (1 << 20);
    for (const SemNP& v : f.vocatives) {
      std::string w = np(v, R::Voc);
      if (v.token >= 0 && v.token > predTok) { if (!vocEnd.empty()) vocEnd += ","; appendWord(vocEnd, w); }
      else { appendWord(vocFront, w); vocFront += ","; }
    }
    appendWord(out, head);
    appendWord(out, vocFront);
    if (!phraseFront.empty()) {
      appendWord(out, phraseFront);
      if (f.hasPred || f.hasSubject || f.hasObject) out += ",";
    }
    // connectors
    for (const std::string& k : f.connectors) {
      std::string w = tab_.text("conj", k, {"-"});
      if (!w.empty()) appendWord(out, w);
    }
    appendWord(out, frontAdverbs(f));
    // subordinate clauses that come first
    // the main subject (for sē and same-subject purpose clauses)
    Agree ma = f.hasSubject ? agreeOf(f.subject) : Agree();
    if (f.hasSubject && f.implicitSubject) {
      ma.person = f.subject.pron.person ? f.subject.pron.person : 3;
      ma.number = f.subject.pron.number == 2 ? 2 : 1;
      ma.gender = f.subject.pron.gender;
    }
    mainSubj_ = ma;
    appendWord(out, subordinates(f, true));
    // the clause itself by type
    std::string core;
    if (f.type == Kind::Frag || (!f.hasPred && f.type != Kind::Imp)) {
      core = fragment(f);
    } else if (f.type == Kind::Imp) {
      Chain c = chain(f, Agree{2, f.imperativePlural ? 2 : 1, 0, true}, Relation::Cause, false);
      if (f.negative) c.w.insert(c.w.begin(), "do not");
      appendWord(core, join(c.w));
      appendWord(core, complements(f, Agree()));
      std::vector<std::string> pre, end;
      adverbs(f, pre, end);
      for (const std::string& e : pre) appendWord(core, e);
      for (const std::string& e : end) appendWord(core, e);
      appendWord(core, subordinates(f, false));
    } else if (f.type == Kind::Yn || f.type == Kind::Wh) {
      core = question(f);
    } else if (f.type == Kind::Excl && f.exclQuam) {
      core = exclamation(f);
    } else {
      core = body(f, Agree(), false);
    }
    appendWord(out, core);
    if (!phraseEnd.empty()) {
      if (!core.empty()) out += ",";
      appendWord(out, phraseEnd);
    }
    if (!vocEnd.empty()) {
      out += ",";
      appendWord(out, vocEnd);
    }
    // discourse memory: the subject of this clause
    if (f.hasSubject && !f.implicitSubject && !f.subject.isPronoun && f.subject.token >= 0) {
      const Agree a = agreeOf(f.subject);
      in_.disc->lastSubjGender = a.gender;
      in_.disc->lastSubjPerson = a.personNoun;
      in_.disc->lastSubjNumber = (uint8_t)a.number;
    }
    return out;
  }

  std::string fragment(const SemFrame& f) {
    std::string out;
    if (f.exclQuam) {
      // "What a strange garden!" / "How strange!"
      if (f.hasSubject && !f.subject.head.empty() && !f.subject.isPronoun) {
        SemNP x = f.subject;
        out = "what";
        if (x.number != 2) appendWord(out, en::indefinite(x.adjectives.empty() ? nounWord(x, R::Obj) : adjective(x.adjectives[0])));
        for (const frame::SemAdj& a : x.adjectives) appendWord(out, adjective(a));
        appendWord(out, nounWord(x, R::Obj));
        return out;
      }
      out = "how";
      for (const frame::SemAdj& a : f.predAdj) appendWord(out, adjective(a));
      return out;
    }
    const bool ecce = std::find(f.interjections.begin(), f.interjections.end(), "ecce") != f.interjections.end();
    if (f.hasSubject) appendWord(out, np(f.subject, ecce ? R::Obj : R::Subj));
    for (const frame::SemAdj& a : f.predAdj) appendWord(out, adjective(a));
    for (const SemNP& x : f.predicative) appendWord(out, np(x, R::Pred));
    if (f.hasObject) appendWord(out, np(f.object, R::Obj));
    if (f.hasIndirect) appendWord(out, "to " + np(f.indirectObject, R::Prep, true));
    for (const frame::SemOblique& o : f.obliques) appendWord(out, oblique(o));
    std::vector<std::string> pre, end;
    adverbs(f, pre, end);
    for (const std::string& e : pre) appendWord(out, e);
    for (const std::string& e : end) appendWord(out, e);
    if (f.hasPred) appendWord(out, join(chain(f, Agree(), Relation::Cause, false).w));
    if (f.negative) out = "not " + out;
    appendWord(out, subordinates(f, false));
    return out;
  }

  std::string exclamation(const SemFrame& f) {
    // "Quam pulchra est rosa!" -> "How beautiful the rose is!"
    std::string out = "how";
    for (const frame::SemAdj& a : f.predAdj) appendWord(out, adjective(a));
    const Agree a = f.hasSubject ? agreeOf(f.subject) : Agree();
    appendWord(out, subjectText(f));
    Chain c = chain(f, a, Relation::Cause, false);
    appendWord(out, join(c.w));
    return out;
  }

  std::string question(const SemFrame& f) {
    Agree a = f.hasSubject ? agreeOf(f.subject) : Agree();
    if (f.hasSubject && f.implicitSubject) {
      a.person = f.subject.pron.person ? f.subject.pron.person : 3;
      a.number = f.subject.pron.number == 2 ? 2 : 1;
      a.gender = f.subject.pron.gender;
    }
    curSubj_ = a;
    const bool whSubject = f.type == Kind::Wh && f.wh.role == Role::Subject;
    std::string whText;
    SemFrame g = f;
    if (f.type == Kind::Wh) {
      if (f.wh.role == Role::Adverb) {
        whText = f.wh.word.size() > 1 && f.wh.word[0] == '#' ? f.wh.word.substr(1) : tab_.text("wh", f.wh.word, {"-"});
        const size_t bar = whText.find('|');
        if (bar != std::string::npos) { whTail_ = whText.substr(bar + 1); whText = whText.substr(0, bar); }
      } else if (f.wh.role == Role::Subject) {
        whText = np(f.subject, R::Subj);
        g.hasSubject = false;
      } else if (f.wh.role == Role::Object) {
        whText = np(f.object, R::Obj);
        g.hasObject = false;
      } else if (f.wh.role == Role::IndirectObject) {
        whText = "to " + np(f.indirectObject, R::Prep);
        g.hasIndirect = false;
      } else if (f.wh.role == Role::Predicate) {
        for (size_t k = 0; k < g.predicative.size(); ++k)
          if (g.predicative[k].interrogative) {
            whText = np(g.predicative[k], R::Subj);
            g.predicative.erase(g.predicative.begin() + (long)k);
            break;
          }
      } else if (f.wh.role == Role::Oblique) {
        for (size_t k = 0; k < g.obliques.size(); ++k)
          if (g.obliques[k].np.interrogative) {
            whText = oblique(g.obliques[k]);
            g.obliques.erase(g.obliques.begin() + (long)k);
            break;
          }
      }
    }
    // "Quis es?" -> "Who are you?": a wh predicate of the copula
    Chain c = chain(g, a, Relation::Cause, false);
    const bool expectNo = hasDisc(f, "num");
    std::string out;
    if (expectNo) {
      // "Surely ... not ...?"
      g.negative = true;
      out = "surely";
      appendWord(out, body(g, Agree(), false));
      return out;
    }
    if (whSubject) {
      // no inversion: "Who loves the girl?"
      out = whText;
      if (f.negative) { doSupport(c, a); c.w.insert(c.w.begin() + 1, "not"); }
      appendWord(out, join(c.w));
      appendWord(out, complements(g, a));
      std::vector<std::string> pre, end;
      adverbs(g, pre, end);
      for (const std::string& e : end) appendWord(out, e);
      return out;
    }
    doSupport(c, a);
    std::string aux = c.w.empty() ? std::string() : c.w[0];
    std::vector<std::string> rest(c.w.begin() + (c.w.empty() ? 0 : 1), c.w.end());
    if ((f.negative != c.neg) && !f.expectYes) rest.insert(rest.begin(), "not");
    if (f.expectYes) aux = negContract(aux);
    appendWord(out, whText);
    appendWord(out, aux);
    appendWord(out, subjectText(g));
    std::vector<std::string> pre, end;
    adverbs(g, pre, end);
    for (const std::string& p : pre) appendWord(out, p);
    appendWord(out, join(rest));
    appendWord(out, complements(g, a));
    for (const std::string& e : end) appendWord(out, e);
    appendWord(out, whTail_);
    whTail_.clear();
    appendWord(out, subordinates(g, false));
    return out;
  }
  std::string whTail_;
};

}  // namespace

std::string realiseEnglish(const Built& b, const RealiseIn& in, std::vector<std::string>& flags) {
  En e(in, flags);
  return e.sentence(b);
}

}  // namespace vp::la2x::detail
