// Spanish (es-MX) realisation of the Latin frames, plain and literal: SVO (wh questions: wh + verb + subject),
// articles el/la/los/las by the gender of the Spanish word (else the Latin gender), presente / imperfecto /
// pretérito / pluscuamperfecto / futuro, tú / ustedes (never vosotros), ¿? and ¡!, "no" before the verb, clitic
// pronouns before the finite verb and attached to affirmative imperatives and infinitives, personal "a", ser/estar.
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

enum class R : uint8_t { Subj, Obj, Iobj, Prep, Pred, Voc };

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

struct Agree { int person = 3, number = 1; uint8_t gender = M; bool personNoun = true; };

// "a el" -> "al", "de el" -> "del" (article only, never the pronoun él)
std::string contract(const std::string& s) {
  std::string out = s;
  for (const char* p : {"a el ", "de el "}) {
    size_t at = 0;
    const std::string pat = p;
    while ((at = out.find(pat, at)) != std::string::npos) {
      const bool start = at == 0 || out[at - 1] == ' ' || out[at - 1] == '\xBF' || out[at - 1] == '\xA1';
      if (!start) { ++at; continue; }
      const std::string rep = pat[0] == 'a' ? "al " : "del ";
      out.replace(at, pat.size(), rep);
      at += rep.size();
    }
  }
  return out;
}

class Es {
 public:
  Es(const RealiseIn& in, std::vector<std::string>& flags) : in_(in), s_(*in.s), ti_(*in.ti), tab_(*in.tab), flags_(flags) {}

  std::string sentence(const Built& b) {
    std::string out;
    for (size_t k = 0; k < b.frames.size(); ++k) {
      std::string c = clause(b.frames[k]);
      if (k > 0) {
        const std::string& j = b.joiners[k - 1];
        if (j == "," || j == ";" || j == ":") out += j;
        else {
          std::string w = tab_.text("conj", j, {"-"});
          if (w.empty()) w = "y";
          if (!out.empty() && (j == "sed" || j == "at" || j == "autem")) out += ",";
          appendWord(out, w);
        }
      }
      appendWord(out, c);
    }
    std::string punct = b.frames.empty() ? "." : b.frames[0].punct;
    if (punct.empty() || punct == ";") punct = ".";
    while (!out.empty() && (out.back() == ',' || out.back() == ' ')) out.pop_back();
    // "y" before i- / hi- -> "e"; "o" before o- / ho- -> "u"
    out = fixConj(out);
    if (punct == "?") out = "\xC2\xBF" + out + "?";
    else if (punct == "!") out = "\xC2\xA1" + out + "!";
    else out += punct;
    out = contract(out);
    return capitaliseFirst(out);
  }

 private:
  const RealiseIn& in_;
  const Sentence& s_;
  const std::vector<TokInfo>& ti_;
  const Tables& tab_;
  std::vector<std::string>& flags_;
  Agree mainSubj_, curSubj_;
  bool mainPast_ = false, curPassive_ = false, inSub_ = false;

  bool reflexiveObject(const SemFrame& f, const Agree& a) const {
    if (!f.hasObject || !f.object.isPronoun || !f.object.coord.empty()) return false;
    if (f.object.pron.reflexive) return true;
    const int p = f.object.pron.person ? f.object.pron.person : 3;
    if (p == 3) return false;
    const int n = f.object.pron.number == 2 ? 2 : 1;
    return p == a.person && n == a.number;
  }

  static std::string fixConj(const std::string& s) {
    std::string out = s;
    for (size_t at = 0; (at = out.find(" y ", at)) != std::string::npos; ++at) {
      const std::string nx = out.substr(at + 3, 2);
      if ((nx.size() && (nx[0] == 'i' || nx == "hi")) && out.compare(at + 3, 3, "hie") != 0) out.replace(at, 3, " e ");
    }
    for (size_t at = 0; (at = out.find(" o ", at)) != std::string::npos; ++at) {
      const std::string nx = out.substr(at + 3, 2);
      if (nx.size() && (nx[0] == 'o' || nx == "ho")) out.replace(at, 3, " u ");
    }
    return out;
  }

  void flag(const std::string& f) {
    if (std::find(flags_.begin(), flags_.end(), f) == flags_.end()) flags_.push_back(f);
  }
  static std::string code(int person, int number, uint8_t gender) {
    std::string c = std::to_string(person < 1 || person > 3 ? 3 : person) + (number == 2 ? "pl" : "sg");
    if (person == 3) c += gender == F ? ".f" : gender == N ? ".n" : ".m";
    else if (number == 2 && gender == F) c += ".f";
    return c;
  }
  std::string pron(int person, int number, uint8_t gender, const char* role) const {
    std::string t = tab_.text("pron", code(person, number, gender), {role});
    if (t.empty()) t = tab_.text("pron", std::to_string(person) + (number == 2 ? "pl" : "sg"), {role});
    if (t.empty()) t = tab_.text("pron", "3sg.m", {role});
    return t;
  }
  Lexical lexOf(int tok) const {
    if (tok < 0 || (size_t)tok >= ti_.size()) return Lexical();
    return in_.lex->lexical(ti_[(size_t)tok], Target::Es);
  }
  static bool hasDisc(const SemFrame& f, const std::string& d) {
    return std::find(f.discourse.begin(), f.discourse.end(), d) != f.discourse.end();
  }

  uint8_t nounGender(const SemNP& x) const {
    if (x.isPronoun) return x.pron.gender == F ? F : M;
    if (x.token < 0) return M;
    const TokInfo& t = ti_[(size_t)x.token];
    if (t.name) return (t.f.gender ? t.f.gender : t.lgender) == F ? F : M;
    const Lexical lx = lexOf(x.token);
    if (lx.gender) return lx.gender;
    const uint8_t lg = t.f.gender ? t.f.gender : t.lgender;
    return es::nounGender(lx.word, lg);
  }

  Agree agreeOf(const SemNP& np) const {
    Agree a;
    if (!np.coord.empty()) {
      a.number = 2;
      a.person = np.isPronoun && np.pron.person ? np.pron.person : 3;
      for (const SemNP& c : np.coord) a.person = std::min(a.person, c.isPronoun && c.pron.person ? (int)c.pron.person : 3);
      a.gender = nounGender(np);
      for (const SemNP& c : np.coord)
        if (nounGender(c) == M) a.gender = M;
      return a;
    }
    if (np.isPronoun) {
      a.person = np.pron.person ? np.pron.person : 3;
      a.number = np.pron.number == 2 ? 2 : 1;
      a.gender = np.pron.gender == F ? F : M;
      return a;
    }
    a.number = np.number == 2 ? 2 : 1;
    a.gender = nounGender(np);
    if (np.token >= 0 && !np.head.empty()) a.personNoun = lexOf(np.token).person || ti_[(size_t)np.token].name;
    return a;
  }

  // ---- adjectives ----
  std::string adjective(const frame::SemAdj& a, uint8_t gender, int number) const {
    std::string w;
    if (a.token >= 0) {
      const TokInfo& t = ti_[(size_t)a.token];
      const Lexical lx = lexOf(a.token);
      w = lx.word;
      const bool participle = (t.f.pos == Participle || t.lpos == Participle) && !lx.adjective;
      if (participle) {
        const bool present = t.key.size() > 2 && t.key.compare(t.key.size() - 2, 2, "ns") == 0;
        w = present ? es::verb(w, es::VTense::Gerund, 3, 1) : es::verb(w, es::VTense::PastPart, 3, 1);
        if (present) return w;
      }
      w = es::adjective(w, gender, (uint8_t)number);
      const uint8_t deg = a.degree ? a.degree : t.f.degree;
      if (deg == Comparative) w = "más " + w;
      else if (deg == Superlative) w = "muy " + w;
    }
    std::string out;
    for (const std::string& k : a.adverbs) appendWord(out, tab_.text("adv", k, {"-"}));
    appendWord(out, w);
    return out;
  }

  std::string det(const std::string& k, uint8_t g, int number) const {
    const std::string gk = g == F ? "f" : "m";
    const std::string nk = number == 2 ? "pl" : "sg";
    return tab_.text("det", k, {gk + "." + nk, nk});
  }

  std::string article(bool def, uint8_t g, int number, const std::string& next) const {
    const std::string gk = g == F ? "f" : "m";
    const std::string nk = number == 2 ? "pl" : "sg";
    if (g == F && number != 2 && es::startsWithStressedA(next)) return def ? "el" : "un";
    return tab_.text("article", def ? "def" : "indef", {gk + "." + nk});
  }

  // ---- noun phrases ----
  std::string np(const SemNP& x, R role, bool definite = false) {
    if (!x.coord.empty()) {
      SemNP first = x;
      first.coord.clear();
      std::vector<std::string> parts;
      parts.push_back(np(first, role, definite));
      for (const SemNP& c : x.coord) parts.push_back(np(c, role == R::Obj ? R::Obj : role, definite));
      std::string conj = tab_.text("conj", x.coordConj.empty() ? "et" : x.coordConj, {"-"});
      if (conj.empty() || conj == "y no") conj = "y";
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
        if (out.empty()) out = "qué";
        const uint8_t g = nounGender(x);
        if (g == F && out.size() > 2 && out.compare(out.size() - 2, 2, "os") == 0) out = out.substr(0, out.size() - 2) + "as";
        appendWord(out, nounWord(x));
        for (const frame::SemAdj& a : x.adjectives) appendWord(out, adjective(a, g, x.number));
        return out;
      }
      const uint8_t c = x.token >= 0 ? ti_[(size_t)x.token].f.case_ : 0;
      const char* ck = c == Acc ? "acc" : c == Dat ? "dat" : c == Gen ? "gen" : c == Abl ? "abl" : "nom";
      if (role == R::Prep) return x.wh == "quid" ? "qué" : "quién";
      std::string w = tab_.text("wh", x.wh, {ck, "nom"});
      return w.empty() ? "qué" : w;
    }
    if (x.isPronoun) {
      const std::string& k = x.pronLemma;
      if (x.pron.reflexive) {
        if (role == R::Subj) return pron(mainSubj_.person, mainSubj_.number, mainSubj_.gender, "subj");
        return pron(curSubj_.person, curSubj_.number, curSubj_.gender, "prep");
      }
      if (isKey(k, {"qui"})) return "que";
      if (isKey(k, {"quis", "quid"})) return tab_.text("wh", k, {"nom"});
      if (tab_.has("det", k) && !isKey(k, {"is"})) return standalone(x, role);
      const uint8_t g = x.pron.gender;
      const char* rk = role == R::Subj ? "subj" : "prep";
      return pron(x.pron.person ? x.pron.person : 3, x.pron.number == 2 ? 2 : 1, g, rk);
    }
    if (x.head.empty() && !x.determiner.empty()) return standalone(x, role);
    if (x.isName && x.token >= 0) {
      std::string out = in_.lex->nameIn(ti_[(size_t)x.token], Target::Es);
      if (!x.relative.empty()) appendWord(out, relative(x, x.relative[0]));
      return out;
    }
    if (x.token >= 0 && x.adjectives.size() == 1 && x.adjectives[0].token == x.token && x.head == x.adjectives[0].lemma) {
      const uint8_t g = (ti_[(size_t)x.token].f.gender == F) ? F : M;
      std::string out = article(true, g, x.number, "");
      appendWord(out, adjective(x.adjectives[0], g, x.number));
      return out;
    }
    return nounPhrase(x, role, definite);
  }

  std::string standalone(const SemNP& x, R role) {
    const uint8_t g0 = x.token >= 0 ? ti_[(size_t)x.token].f.gender : 0;
    const bool pl = x.number == 2;
    const std::string gk = g0 == F ? "f" : g0 == N ? "n" : "m";
    const std::string nk = pl ? "pl" : "sg";
    const std::string k = x.determiner.empty() ? x.pronLemma : x.determiner;
    std::string w = tab_.text("det", k, {"pron." + gk + "." + nk, "pron." + nk, "pron"});
    if (w.empty()) w = tab_.text("det", k, {(gk == "n" ? std::string("m") : gk) + "." + nk, nk});
    if (w == "él" || w == "ella" || w == "ellos" || w == "ellas") {
      const char* rk = role == R::Subj ? "subj" : "prep";
      return pron(3, pl ? 2 : 1, w == "ella" || w == "ellas" ? F : M, rk);
    }
    if (w.empty()) w = s_.tokens[(size_t)std::max(0, x.token)].text;
    if (!x.relative.empty()) appendWord(w, relative(x, x.relative[0]));
    return w;
  }

  std::string nounWord(const SemNP& x) {
    const Lexical lx = lexOf(x.token);
    if (lx.missing) flag("gloss-missing");
    std::string w = lx.word;
    if (x.number == 2 && !lx.invariable) w = es::plural(w);
    return w;
  }

  std::string nounPhrase(const SemNP& x, R role, bool definite) {
    const Lexical lx = lexOf(x.token);
    const uint8_t g = nounGender(x);
    const int num = x.number == 2 ? 2 : 1;
    std::vector<std::string> pre;
    bool hasDet = false;
    if (!x.determiner.empty()) {
      std::string w;
      if (x.determiner == "suus") w = num == 2 ? "sus" : "su";
      else w = det(x.determiner, g, num);
      if (!w.empty()) { pre.push_back(w); hasDet = true; }
    }
    for (const SemNP& p : x.possessor) {
      const int person = p.pron.person ? p.pron.person : 3;
      std::string w = pron(person, p.pron.number == 2 ? 2 : 1, p.pron.gender, "poss");
      if (w == "nuestro") w = es::adjective(w, g, (uint8_t)num);
      else if (num == 2) w += "s";
      pre.push_back(w);
      hasDet = true;
    }
    if (!x.numeral.empty()) {
      std::string w = tab_.text("det", x.numeral, {std::string(g == F ? "f" : "m") + "." + (num == 2 ? "pl" : "sg"), "pl", "sg"});
      pre.push_back(w.empty() ? x.numeral : w);
      hasDet = true;
    }
    std::vector<std::string> adjs;
    for (const frame::SemAdj& a : x.adjectives) {
      if (a.token >= 0 && tab_.has("det", a.lemma)) {
        std::string w = det(a.lemma, g, num);
        if (!w.empty()) pre.push_back(w);
      } else {
        adjs.push_back(adjective(a, g, num));
      }
    }
    const uint32_t lemma = x.token >= 0 ? ti_[(size_t)x.token].lemma : lex::kNoLemma;
    const std::string word = nounWord(x);
    if (!hasDet && role != R::Voc) {
      const bool mentioned = in_.disc->seen(lemma);
      const bool hasGen = !x.genitive.empty();
      bool def = definite || lx.unique || mentioned || hasGen;
      bool indef = false;
      if (!def) {
        if (role == R::Subj || role == R::Iobj || role == R::Prep) def = true;
        else if (num != 2 && !lx.mass) indef = true;
      }
      if (lx.mass && !mentioned && !hasGen && (!definite || role == R::Prep) && role != R::Subj) { def = false; indef = false; }
      if (lx.mass && role == R::Subj) def = true;
      if (num == 2 && !mentioned && !hasGen && (role == R::Obj || role == R::Pred)) def = false;
      if (role == R::Pred && num != 2 && !mentioned && !lx.mass && !lx.unique) { def = false; indef = true; }
      if (def) pre.push_back(article(true, g, num, word));
      else if (indef) pre.push_back(article(false, g, num, word));
    }
    std::string out = join(pre);
    appendWord(out, word);
    for (const std::string& a : adjs) appendWord(out, a);
    in_.disc->mention(lemma);
    for (const SemNP& gn : x.genitive) {
      appendWord(out, "de");
      appendWord(out, np(gn, R::Prep, true));
    }
    if (!x.relative.empty()) appendWord(out, relative(x, x.relative[0]));
    return out;
  }

  // personal "a" before a person object
  bool personObject(const SemNP& x) const {
    if (x.isName) return true;
    if (x.isPronoun) return true;
    if (x.head.empty() && !x.determiner.empty()) return isKey(x.determiner, {"omnis", "nemo", "aliquis", "ille", "hic", "multus", "paucus", "quisque"});
    if (x.token >= 0 && lexOf(x.token).person) return true;
    if (x.interrogative && x.wh == "quis") return true;
    return false;
  }

  // ---- verbs ----
  struct Chain {
    std::vector<std::string> w;     // [haber/ser/modal] ... main
    size_t finite = 0;              // index of the finite word
    bool imperativeAff = false;     // clitics attach to w[finite]
    int attachTo = -1;              // infinitive index where clitics attach (modal + inf)
    bool reflexive = false;
  };

  es::VTense tenseOf(const SemFrame& f, bool& haber) const {
    haber = false;
    const frame::Tense t = f.pred.tense;
    const frame::Aspect a = f.pred.aspect;
    if (t == frame::Tense::Present) return es::VTense::Present;
    if (t == frame::Tense::Future) { haber = a == frame::Aspect::Perfect; return es::VTense::Future; }
    if (a == frame::Aspect::Perfect) { haber = true; return es::VTense::Imperfect; }
    if (a == frame::Aspect::Progressive) return es::VTense::Imperfect;
    return es::VTense::Preterite;
  }

  std::string verbWord(int tok) const {
    std::string w = lexOf(tok).word;
    return w.empty() ? std::string("ser") : w;
  }

  // ser or estar for the copula
  std::string copulaVerb(const SemFrame& f) const {
    if (f.type == Kind::Wh && isKey(f.wh.word, {"ubi", "quo", "unde"})) return "estar";
    for (const frame::SemAdj& a : f.predAdj)
      if (a.token >= 0 && tab_.tagged("lex", ti_[(size_t)a.token].key, "estar")) return "estar";
    if (f.predAdj.empty() && f.predicative.empty()) {
      for (const frame::SemOblique& o : f.obliques) {
        const std::string p = o.prep.substr(0, o.prep.find(':'));
        if (isKey(p, {"in", "apud", "sub", "inter", "prope", "#loc"})) return "estar";
      }
      for (const frame::SemAdverb& a : f.adverbs)
        if (isKey(a.lemma, {"hic", "ibi", "domi", "ruri", "foris", "intus", "procul", "bene", "male"})) return "estar";
    }
    return "ser";
  }

  Chain chain(const SemFrame& f, const Agree& a, bool purposeSubj, bool& negOut) {
    Chain c;
    negOut = false;
    int lexTok = f.pred.complementToken >= 0 && f.pred.particle == "peri" ? f.pred.complementToken : f.pred.token;
    std::string base = f.copula ? copulaVerb(f) : verbWord(lexTok);
    if (f.hasObject && lexTok >= 0 && tab_.find("lex", ti_[(size_t)lexTok].key, "verb+obj"))
      base = tab_.find("lex", ti_[(size_t)lexTok].key, "verb+obj")->text;
    if (lexTok >= 0 && f.type == Kind::Wh && f.wh.word == "quid" && tab_.find("lex", ti_[(size_t)lexTok].key, "verb+wh"))
      base = tab_.find("lex", ti_[(size_t)lexTok].key, "verb+wh")->text;
    if (lexTok >= 0 && reflexiveObject(f, a) && tab_.find("lex", ti_[(size_t)lexTok].key, "verb+refl"))
      base = tab_.find("lex", ti_[(size_t)lexTok].key, "verb+refl")->text;
    if (f.existential && f.copula) {
      // hay / había / hubo / habrá
      bool hb = false;
      const es::VTense vt0 = tenseOf(f, hb);
      c.w = {vt0 == es::VTense::Present ? std::string("hay") : es::verb("haber", vt0, 3, 1)};
      return c;
    }
    if (base.compare(0, 3, "no ") == 0) { base = base.substr(3); negOut = true; }
    const bool passive = f.pred.voice == frame::Voice::Passive;
    const bool subjMood = f.pred.mood == frame::SrcMood::Subjunctive;
    const int P = a.person, N = a.number;
    bool haber = false;
    es::VTense vt = tenseOf(f, haber);
    c.reflexive = es::reflexive(base);
    // modal + infinitive
    if (!f.pred.complementVerb.empty() && !hasDisc(f, "jussive")) {
      std::string inf = verbWord(f.pred.complementToken);
      bool neg2 = false;
      if (inf.compare(0, 3, "no ") == 0) { inf = inf.substr(3); neg2 = true; }
      (void)neg2;
      SemFrame g = f;
      g.pred.complementVerb.clear();
      g.copula = false;
      Chain v = chain(g, a, purposeSubj, negOut);
      if (es::reflexive(inf)) v.reflexive = true;
      v.w.push_back(es::unreflexive(inf));
      v.attachTo = (int)v.w.size() - 1;
      return v;
    }
    if (f.type == Kind::Imp) {
      if (f.negative) {
        c.w = {es::verb(base, es::VTense::SubjPresent, 2, f.imperativePlural ? 2 : 1)};
      } else {
        c.w = {es::verb(base, es::VTense::Imperative, 2, f.imperativePlural ? 2 : 1)};
        c.imperativeAff = true;
      }
      return c;
    }
    if (hasDisc(f, "infinitive")) {
      c.w = {es::unreflexive(base)};
      c.attachTo = 0;
      return c;
    }
    if (f.pred.particle == "part-pres") {
      c.w = {es::verb(base, mainPast_ ? es::VTense::Imperfect : es::VTense::Present, P, N)};
      return c;
    }
    if (f.pred.particle == "part-perf") {
      c.w = {es::verb("ser", mainPast_ ? es::VTense::Preterite : es::VTense::Present, P, N),
             es::adjective(es::verb(base, es::VTense::PastPart, 3, 1), a.gender, (uint8_t)N)};
      return c;
    }
    if (subjMood && purposeSubj) {
      vt = mainPast_ ? es::VTense::SubjImperfect : es::VTense::SubjPresent;
      haber = false;
    }
    // hortative "intrēmus" -> entremos (main clause, 1st plural present subjunctive)
    if (subjMood && !purposeSubj && !inSub_ && f.type == Kind::Decl && P == 1 && N == 2 && f.pred.tense == frame::Tense::Present) {
      vt = es::VTense::SubjPresent;
      haber = false;
    }
    if (passive) {
      const std::string part = es::adjective(es::verb(base, es::VTense::PastPart, 3, 1), a.gender, (uint8_t)N);
      if (haber) c.w = {es::verb("haber", vt, P, N), "sido", part};
      else {
        es::VTense st = vt;
        if (f.pred.particle == "peri" && f.pred.tense == frame::Tense::Past && f.pred.aspect == frame::Aspect::Simple)
          st = es::VTense::Preterite;
        c.w = {es::verb("ser", st, P, N), part};
      }
      return c;
    }
    if (haber) {
      c.w = {es::verb("haber", vt, P, N), es::verb(base, es::VTense::PastPart, 3, 1)};
      // "estar sentado": only the verb takes the participle
      return c;
    }
    c.w = {es::verb(base, vt, P, N)};
    // "estar sentado": the participle agrees with the subject
    if (base.compare(0, 6, "estar ") == 0 && !c.w.empty()) {
      const size_t sp = c.w[0].rfind(' ');
      if (sp != std::string::npos) {
        const std::string last = c.w[0].substr(sp + 1);
        if (last.size() > 2 && last.back() == 'o') c.w[0] = c.w[0].substr(0, sp + 1) + es::adjective(last, a.gender, (uint8_t)N);
      }
    }
    return c;
  }

  // clitic string for the objects that are personal pronouns (removed from the complements)
  std::string clitics(const SemFrame& f, const Agree& a, bool reflexive, bool& usedObj, bool& usedIo) {
    usedObj = usedIo = false;
    std::vector<std::string> cl;
    if (reflexive && !reflexiveObject(f, a)) cl.push_back(pron(a.person, a.number, a.gender, "refl"));
    std::string io, dobj;
    bool io3 = false;
    if (f.hasIndirect && f.indirectObject.isPronoun && f.indirectObject.coord.empty() &&
        !isKey(f.indirectObject.pronLemma, {"qui", "quis", "quid", "nemo", "nihil"}) &&
        !(tab_.has("det", f.indirectObject.pronLemma) && f.indirectObject.pronLemma != "is")) {
      const SemNP& p = f.indirectObject;
      if (p.pron.reflexive) io = pron(curSubj_.person, curSubj_.number, curSubj_.gender, "refl");
      else io = pron(p.pron.person ? p.pron.person : 3, p.pron.number == 2 ? 2 : 1, p.pron.gender, "iobj");
      io3 = !p.pron.reflexive && (p.pron.person == 3 || !p.pron.person);
      usedIo = true;
    }
    if (f.hasObject && f.object.isPronoun && f.object.coord.empty() &&
        !isKey(f.object.pronLemma, {"qui", "quis", "quid", "nemo", "nihil"}) &&
        !(tab_.has("det", f.object.pronLemma) && f.object.pronLemma != "is")) {
      const SemNP& p = f.object;
      if (p.pron.reflexive || reflexiveObject(f, a)) dobj = pron(a.person, a.number, a.gender, "refl");
      else {
        uint8_t g = p.pron.gender;
        dobj = pron(p.pron.person ? p.pron.person : 3, p.pron.number == 2 ? 2 : 1, g, "obj");
      }
      usedObj = true;
    }
    if (!io.empty() && !dobj.empty() && io3 && (dobj[0] == 'l')) io = "se";
    if (!io.empty()) cl.push_back(io);
    if (!dobj.empty()) cl.push_back(dobj);
    std::string out;
    for (const std::string& x : cl) {
      if (std::find(cl.begin(), cl.end(), x) != cl.begin() + (&x - &cl[0])) continue;
      appendWord(out, x);
    }
    return out;
  }

  // the verb group with negation and clitics placed
  std::string verbGroup(const SemFrame& f, const Agree& a, bool purposeSubj, bool& usedObj, bool& usedIo,
                        const std::vector<std::string>& preAdv) {
    bool lexNeg = false;
    Chain c = chain(f, a, purposeSubj, lexNeg);
    std::string cl = clitics(f, a, c.reflexive, usedObj, usedIo);
    const bool neg = f.negative != lexNeg;   // "nōn vult" -> "no quiere"; nōlō alone -> "no quiere"
    bool negWord = neg;
    // nada / nadie / ningún after the verb: "no ... nada" (a negative subject before the verb needs no "no")
    if (!negWord && ((f.hasObject && f.object.negative) || (f.hasIndirect && f.indirectObject.negative))) negWord = true;
    for (const std::string& p : preAdv)
      if (p == "nunca" || p == "todavía no") negWord = false;
    // nēmō / nihil / numquam carry the negation themselves when they come first
    std::string out;
    if (c.imperativeAff && !cl.empty() && !c.w.empty()) {
      std::string attached = cl;
      attached.erase(std::remove(attached.begin(), attached.end(), ' '), attached.end());
      c.w[0] = es::withClitics(c.w[0], attached);
      cl.clear();
    } else if (c.attachTo >= 0 && !cl.empty() && (size_t)c.attachTo < c.w.size()) {
      std::string attached = cl;
      attached.erase(std::remove(attached.begin(), attached.end(), ' '), attached.end());
      // the reflexive of a modal + reflexive infinitive goes on the infinitive too
      c.w[(size_t)c.attachTo] = es::withClitics(c.w[(size_t)c.attachTo], attached);
      cl.clear();
    }
    if (negWord) appendWord(out, "no");
    for (const std::string& p : preAdv) appendWord(out, p);
    appendWord(out, cl);
    appendWord(out, join(c.w));
    return out;
  }

  // ---- relative clauses ----
  std::string relative(const SemNP& ante, const SemFrame& rf) {
    const Agree a = agreeOf(ante);
    const bool person = a.personNoun || ante.isName;
    SemFrame f = rf;
    std::string pronoun = "que";
    switch (f.wh.role) {
      case Role::Subject:
      case Role::Object: pronoun = "que"; break;
      case Role::IndirectObject: pronoun = person ? "a quien" : "al que"; break;
      case Role::Oblique: {
        std::string pk = f.wh.word;
        const size_t colon = pk.find(':');
        const std::string prep = pk.substr(0, colon), cs = colon == std::string::npos ? "abl" : pk.substr(colon + 1);
        std::string p = tab_.text("prep", prep, {cs});
        if (p.empty()) p = "en";
        if (person) pronoun = p + (a.number == 2 ? " quienes" : " quien");
        else pronoun = p + (a.gender == F ? (a.number == 2 ? " las que" : " la que") : (a.number == 2 ? " los que" : " el que"));
        break;
      }
      default: break;
    }
    std::string out = pronoun;
    const Agree save = curSubj_;
    if (f.wh.role == Role::Subject) {
      f.hasSubject = false;
      appendWord(out, body(f, a, true));
    } else {
      appendWord(out, body(f, Agree(), false));
    }
    curSubj_ = save;
    return out;
  }

  // ---- clause body (declarative order) ----
  std::string body(const SemFrame& f, const Agree& forcedAgree, bool forced, bool purposeSubj = false) {
    Agree a = forced ? forcedAgree : (f.hasSubject ? agreeOf(f.subject) : Agree());
    if (!forced && f.hasSubject && f.implicitSubject) {
      a.person = f.subject.pron.person ? f.subject.pron.person : 3;
      a.number = f.subject.pron.number == 2 ? 2 : 1;
      a.gender = f.subject.pron.gender == F ? F : M;
    }
    curSubj_ = a;
    std::string out;
    std::string subj = (f.hasSubject && !forced && !f.existential) ? subjectText(f) : std::string();
    std::vector<std::string> preAdv, endAdv;
    adverbs(f, preAdv, endAdv);
    bool usedObj = false, usedIo = false;
    if (f.existential && f.hasSubject && !forced) {
      // "hay una niña", "no hay té"
      SemFrame g = f;
      bool none = false;
      if (g.subject.determiner == "nullus") { none = true; g.subject.determiner.clear(); g.negative = true; }
      std::string sj = np(g.subject, R::Obj);
      if (none) {
        // "no hay té": no article after a negated "hay"
        for (const char* art : {"un ", "una ", "unos ", "unas "})
          if (sj.compare(0, std::char_traits<char>::length(art), art) == 0) sj = sj.substr(std::char_traits<char>::length(art));
      }
      appendWord(out, verbGroup(g, a, purposeSubj, usedObj, usedIo, preAdv));
      appendWord(out, sj);
      appendWord(out, complements(g, a, usedObj, usedIo));
      for (const std::string& e : endAdv) appendWord(out, e);
      appendWord(out, subordinates(f, false));
      return out;
    }
    if (std::find(f.discourse.begin(), f.discourse.end(), "all") != f.discourse.end()) {
      // "todos ustedes son ..."
      const char* t = a.gender == F ? "todas" : "todos";
      if (subj.empty() && f.hasSubject) subj = pron(a.person, a.number, a.gender, "subj");
      subj = std::string(t) + " " + subj;
    }
    bool inverted = false;
    if (f.copula && f.hasSubject && !f.implicitSubject && !f.subject.isPronoun && f.predAdj.empty() && f.predicative.empty())
      for (const frame::SemAdverb& x : f.adverbs)
        if (x.front && isKey(x.lemma, {"hic", "ibi", "ecce"})) inverted = true;
    if (!inverted) appendWord(out, subj);
    if (f.hasPred) appendWord(out, verbGroup(f, a, purposeSubj, usedObj, usedIo, preAdv));
    if (inverted) appendWord(out, subj);
    appendWord(out, complements(f, a, usedObj, usedIo));
    for (const std::string& e : endAdv) appendWord(out, e);
    appendWord(out, subordinates(f, false));
    return out;
  }

  std::string subjectText(const SemFrame& f) {
    if (!f.hasSubject || f.implicitSubject) return std::string();   // pro-drop
    return np(f.subject, R::Subj);
  }

  std::string complements(const SemFrame& f, const Agree& a, bool usedObj, bool usedIo) {
    std::string out;
    for (const std::string& d : f.discourse)
      if (d.compare(0, 12, "phrase-pred:") == 0) appendWord(out, d.substr(12));
    if (f.copula || (!f.hasPred && (!f.predAdj.empty() || !f.predicative.empty()))) {
      std::vector<std::string> pp;
      for (const frame::SemAdj& x : f.predAdj) {
        uint8_t g = a.gender;
        if (f.hasSubject && f.implicitSubject && x.token >= 0) {
          const uint8_t lg = ti_[(size_t)x.token].f.gender;
          if (lg == F || lg == M) g = lg;
        }
        pp.push_back(adjective(x, g, a.number));
      }
      for (const SemNP& x : f.predicative) pp.push_back(np(x, R::Pred));
      for (size_t k = 0; k < pp.size(); ++k) {
        if (k > 0) appendWord(out, k + 1 == pp.size() ? "y" : ",");
        appendWord(out, pp[k]);
      }
    } else if (!f.predAdj.empty() || !f.predicative.empty()) {
      for (const frame::SemAdj& x : f.predAdj) appendWord(out, adjective(x, a.gender, a.number));
      for (const SemNP& x : f.predicative) appendWord(out, np(x, R::Pred));
    }
    const bool jussive = hasDisc(f, "jussive");
    if (f.hasObject && !usedObj) {
      std::string o = np(f.object, R::Obj);
      if (personObject(f.object) && !(f.object.isPronoun && false)) o = "a " + o;
      appendWord(out, o);
    }
    if (jussive && f.pred.complementToken >= 0) appendWord(out, es::unreflexive(verbWord(f.pred.complementToken)));
    for (const frame::SemOblique& o : f.obliques)
      if (o.prep == "#acc") appendWord(out, np(o.np, R::Obj));
    if (f.hasIndirect && !usedIo) appendWord(out, "a " + np(f.indirectObject, R::Prep, true));
    for (const frame::SemOblique& o : f.obliques) {
      if (o.prep == "#acc") continue;
      appendWord(out, oblique(o));
    }
    return out;
  }

  std::string oblique(const frame::SemOblique& o) {
    const std::string p = o.prep;
    if (p == "#dat") return "a " + np(o.np, R::Prep, true);
    if (p == "#than") return "que " + np(o.np, R::Subj, true);
    if (p == "#gen") return "de " + np(o.np, R::Prep, true);
    if (p == "#loc") return "en " + (o.np.isName ? in_.lex->nameIn(ti_[(size_t)std::max(0, o.np.token)], Target::Es) : np(o.np, R::Prep, true));
    if (p == "#abl") {
      const Lexical lx = lexOf(o.np.token);
      if (!lx.timePrep.empty()) {
        std::string out = lx.timePrep;
        if (lx.timePrep == "de" && o.np.adjectives.empty() && o.np.number != 2) { appendWord(out, nounWord(o.np)); return out; }
        appendWord(out, np(o.np, R::Prep, true));
        return out;
      }
      return "con " + np(o.np, R::Obj, false);
    }
    if (p.size() > 1 && p[0] == '#') return np(o.np, R::Obj);
    const size_t colon = p.find(':');
    const std::string prep = p.substr(0, colon), cs = colon == std::string::npos ? "-" : p.substr(colon + 1);
    std::string w;
    if ((prep == "a" || prep == "ab") && curPassive_) w = tab_.text("prep", prep, {"agent"});
    if (w.empty() && (prep == "post" || prep == "ante")) {
      const Lexical lx = lexOf(o.np.token);
      if (lx.timePrep.empty() && !lx.event) w = tab_.text("prep", prep, {cs + ".place"});
    }
    if (w.empty()) w = tab_.text("prep", prep, {cs});
    if (w.empty()) w = s_.tokens[(size_t)std::max(0, o.token)].text;
    // con + mí / ti -> conmigo / contigo
    if (w == "con" && o.np.isPronoun && o.np.coord.empty() && (o.np.pron.person == 1 || o.np.pron.person == 2) && o.np.pron.number != 2)
      return o.np.pron.person == 1 ? "conmigo" : "contigo";
    std::string out = w;
    appendWord(out, np(o.np, R::Prep, true));
    return out;
  }

  void adverbs(const SemFrame& f, std::vector<std::string>& pre, std::vector<std::string>& end) {
    for (const frame::SemAdverb& a : f.adverbs) {
      if (a.front && !(tab_.tagged("adv", a.lemma, "pre") && f.hasPred) && frontable(f, a)) continue;
      if (a.lemma == "#tok") { end.push_back(leftover(a.token)); continue; }
      std::string w = tab_.text("adv", a.lemma, {"-"});
      if (w.empty()) {
        w = lexOf(a.token).word;
        if (w.empty()) w = "[" + s_.tokens[(size_t)a.token].text + "]";
      }
      if (a.lemma == "quaeso") w = ", " + w;
      if (a.lemma == "ualde" && f.hasPred) w = "mucho";
      if (tab_.tagged("adv", a.lemma, "pre")) pre.push_back(w);
      else end.push_back(w);
    }
  }
  bool frontable(const SemFrame& f, const frame::SemAdverb& a) const {
    if (a.lemma == "#tok") return true;
    if (tab_.tagged("adv", a.lemma, "front") || tab_.tagged("adv", a.lemma, "time")) return true;
    if (!tab_.has("adv", a.lemma)) return true;
    if (!isKey(a.lemma, {"hic", "ibi", "ecce"})) return false;
    return f.copula && f.hasSubject && !f.implicitSubject && !f.subject.isPronoun && f.predAdj.empty() && f.predicative.empty();
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
      std::string w = tab_.text("adv", a.lemma, {"-"});
      if (w.empty()) w = lexOf(a.token).word;
      if (tab_.tagged("adv", a.lemma, "pre") && f.hasPred) continue;   // handled with the verb ("nunca", "ya")
      if (!frontable(f, a)) continue;
      appendWord(out, w);
    }
    return out;
  }

  std::string subordinates(const SemFrame& f, bool before) {
    std::string out;
    for (const frame::SemSub& ss : f.subordinate) {
      if (ss.before != before || ss.frame.empty()) continue;
      appendWord(out, subordinate(f, ss));
      if (before) out += ",";
    }
    return out;
  }

  std::string subordinate(const SemFrame& main, const frame::SemSub& ss) {
    struct Flag { bool& b; bool old; Flag(bool& x) : b(x), old(x) { b = true; } ~Flag() { b = old; } } guard(inSub_);
    const SemFrame& sf = ss.frame[0];
    const Agree save = curSubj_;
    std::string out;
    if (ss.marker == "acc+inf") {
      SemFrame g = sf;
      if (g.hasSubject && g.subject.isPronoun && g.subject.pron.reflexive) {
        // sē: the main subject; Spanish drops it
        g.implicitSubject = true;
        g.subject.pron.reflexive = false;
        g.subject.pron.person = (uint8_t)mainSubj_.person;
        g.subject.pron.number = (uint8_t)mainSubj_.number;
        g.subject.pron.gender = mainSubj_.gender;
      }
      out = "que";
      appendWord(out, body(g, Agree(), false));
      curSubj_ = save;
      return out;
    }
    if (ss.marker == "ablabs-perf" || ss.marker == "ablabs-pres") {
      flag("abl-abs");
      out = ss.marker == "ablabs-perf" ? "después de que" : "mientras";
      appendWord(out, body(sf, Agree(), false));
      curSubj_ = save;
      return out;
    }
    if (ss.relation == Relation::Relative) {
      out = "el que";
      appendWord(out, body(sf, Agree(), false));
      return out;
    }
    if (ss.relation == Relation::Coord) {
      out = tab_.text("conj", ss.marker, {"-"});
      if (out.empty()) out = "y";
      appendWord(out, body(sf, Agree(), false));
      curSubj_ = save;
      return out;
    }
    if (ss.relation == Relation::Purpose) {
      bool same = false;
      if (sf.implicitSubject && main.hasSubject)
        same = (sf.subject.pron.person ? sf.subject.pron.person : 3) == mainSubj_.person &&
               (sf.subject.pron.number == 2 ? 2 : 1) == mainSubj_.number;
      if (same) {
        out = ss.marker == "ne" ? "para no" : "para";
        SemFrame g = sf;
        g.negative = false;
        g.discourse.push_back("infinitive");
        g.hasSubject = false;
        bool uo = false, ui = false;
        Agree a = mainSubj_;
        curSubj_ = a;
        appendWord(out, verbGroup(g, a, false, uo, ui, {}));
        appendWord(out, complements(g, a, uo, ui));
        curSubj_ = save;
        return out;
      }
      out = tab_.text("sub", ss.marker, {"subj", "-"});
      SemFrame g = sf;
      if (ss.marker == "ne") { out = "para que"; g.negative = true; }
      appendWord(out, body(g, Agree(), false, true));
      curSubj_ = save;
      return out;
    }
    const bool subjMood = sf.pred.mood == frame::SrcMood::Subjunctive;
    out = tab_.text("sub", ss.marker, {subjMood ? "subj" : "ind", "-"});
    if (out.empty()) out = ss.marker;
    SemFrame g = sf;
    const bool needSubj = tab_.tagged("sub", ss.marker, "subj");
    if (!needSubj) g.pred.mood = frame::SrcMood::Indicative;
    appendWord(out, body(g, Agree(), false, needSubj));
    curSubj_ = save;
    return out;
  }

  // ---- clause ----
  std::string clause(const SemFrame& f) {
    std::string out;
    curPassive_ = f.pred.voice == frame::Voice::Passive;
    mainPast_ = f.pred.tense == frame::Tense::Past;
    std::string phraseFront, phraseEnd;
    for (const std::string& d : f.discourse)
      if (d.compare(0, 7, "phrase:") == 0) {
        std::string t = d.substr(7);
        const size_t bar = t.find("|end");
        if (bar != std::string::npos) { t = t.substr(0, bar); appendWord(phraseEnd, t); }
        else appendWord(phraseFront, t);
      }
    std::string head;
    for (const std::string& i : f.interjections) {
      std::string w = tab_.text("intj", i, {"-"});
      if (w.empty()) w = i;
      appendWord(head, w);
      if (i != "o" || f.vocatives.empty()) head += ",";
    }
    std::string vocFront, vocEnd;
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
    for (const std::string& k : f.connectors) {
      std::string w = tab_.text("conj", k, {"-"});
      if (!w.empty()) appendWord(out, w);
    }
    appendWord(out, frontAdverbs(f));
    Agree ma = f.hasSubject ? agreeOf(f.subject) : Agree();
    if (f.hasSubject && f.implicitSubject) {
      ma.person = f.subject.pron.person ? f.subject.pron.person : 3;
      ma.number = f.subject.pron.number == 2 ? 2 : 1;
      ma.gender = f.subject.pron.gender == F ? F : M;
    }
    mainSubj_ = ma;
    appendWord(out, subordinates(f, true));
    std::string core;
    if (f.type == Kind::Frag || (!f.hasPred && f.type != Kind::Imp)) {
      core = fragment(f);
    } else if (f.type == Kind::Imp) {
      Agree a{2, f.imperativePlural ? 2 : 1, M, true};
      curSubj_ = a;
      bool uo = false, ui = false;
      std::vector<std::string> pre, end;
      adverbs(f, pre, end);
      SemFrame g = f;
      appendWord(core, verbGroup(g, a, false, uo, ui, pre));
      appendWord(core, complements(g, a, uo, ui));
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
    return out;
  }

  std::string fragment(const SemFrame& f) {
    std::string out;
    if (f.exclQuam) {
      if (f.hasSubject && !f.subject.head.empty() && !f.subject.isPronoun) {
        // "¡Qué jardín tan extraño!"
        SemNP x = f.subject;
        const uint8_t g = nounGender(x);
        out = "qué";
        appendWord(out, nounWord(x));
        for (const frame::SemAdj& a : x.adjectives) { appendWord(out, "tan"); appendWord(out, adjective(a, g, x.number)); }
        return out;
      }
      out = "qué";
      for (const frame::SemAdj& a : f.predAdj) appendWord(out, adjective(a, M, 1));
      return out;
    }
    const bool ecce = std::find(f.interjections.begin(), f.interjections.end(), "ecce") != f.interjections.end();
    if (f.hasSubject) appendWord(out, np(f.subject, ecce ? R::Obj : R::Subj));
    const Agree a = f.hasSubject ? agreeOf(f.subject) : Agree();
    for (const frame::SemAdj& x : f.predAdj) appendWord(out, adjective(x, a.gender, a.number));
    for (const SemNP& x : f.predicative) appendWord(out, np(x, R::Pred));
    if (f.hasObject) appendWord(out, np(f.object, R::Obj));
    if (f.hasIndirect) appendWord(out, "a " + np(f.indirectObject, R::Prep, true));
    for (const frame::SemOblique& o : f.obliques) appendWord(out, oblique(o));
    std::vector<std::string> pre, end;
    adverbs(f, pre, end);
    for (const std::string& e : pre) appendWord(out, e);
    for (const std::string& e : end) appendWord(out, e);
    if (f.hasPred) {
      bool uo = false, ui = false;
      appendWord(out, verbGroup(f, a, false, uo, ui, {}));
    }
    if (f.negative) out = "no " + out;
    appendWord(out, subordinates(f, false));
    return out;
  }

  std::string exclamation(const SemFrame& f) {
    // "Quam pulchra est rosa!" -> "¡Qué bonita es la rosa!"
    const Agree a = f.hasSubject ? agreeOf(f.subject) : Agree();
    std::string out = "qué";
    for (const frame::SemAdj& x : f.predAdj) appendWord(out, adjective(x, a.gender, a.number));
    bool uo = false, ui = false;
    curSubj_ = a;
    appendWord(out, verbGroup(f, a, false, uo, ui, {}));
    appendWord(out, subjectText(f));
    return out;
  }

  std::string question(const SemFrame& f) {
    Agree a = f.hasSubject ? agreeOf(f.subject) : Agree();
    if (f.hasSubject && f.implicitSubject) {
      a.person = f.subject.pron.person ? f.subject.pron.person : 3;
      a.number = f.subject.pron.number == 2 ? 2 : 1;
      a.gender = f.subject.pron.gender == F ? F : M;
    }
    curSubj_ = a;
    SemFrame g = f;
    std::string whText;
    bool whSubject = false;
    if (f.type == Kind::Wh) {
      if (f.wh.role == Role::Adverb)
        whText = f.wh.word.size() > 1 && f.wh.word[0] == '#' ? f.wh.word.substr(1) : tab_.text("wh", f.wh.word, {"-"});
      else if (f.wh.role == Role::Subject) { whText = np(f.subject, R::Subj); g.hasSubject = false; whSubject = true; }
      else if (f.wh.role == Role::Object) {
        whText = np(f.object, R::Obj);
        g.hasObject = false;
      } else if (f.wh.role == Role::IndirectObject) { whText = "a " + np(f.indirectObject, R::Prep); g.hasIndirect = false; }
      else if (f.wh.role == Role::Predicate) {
        for (size_t k = 0; k < g.predicative.size(); ++k)
          if (g.predicative[k].interrogative) {
            whText = np(g.predicative[k], R::Subj);
            // "Quid est nōmen tuum?" -> ¿Cuál es tu nombre?
            if (whText == "qué" && f.copula && f.hasSubject && !f.subject.isPronoun) whText = "cuál";
            g.predicative.erase(g.predicative.begin() + (long)k);
            break;
          }
      } else if (f.wh.role == Role::Oblique) {
        for (size_t k = 0; k < g.obliques.size(); ++k)
          if (g.obliques[k].np.interrogative) { whText = oblique(g.obliques[k]); g.obliques.erase(g.obliques.begin() + (long)k); break; }
      }
    }
    if (hasDisc(f, "num")) {
      std::string out = "acaso";
      appendWord(out, body(g, Agree(), false));
      return out;
    }
    if (f.type == Kind::Yn) {
      SemFrame h = g;
      if (f.expectYes) h.negative = true;
      return body(h, Agree(), false);
    }
    // wh: wh + [no] [clitics] verb + subject + complements
    std::string out = whText;
    std::vector<std::string> pre, end;
    adverbs(g, pre, end);
    bool uo = false, ui = false;
    if (g.hasPred) appendWord(out, verbGroup(g, a, false, uo, ui, pre));
    if (!whSubject) appendWord(out, subjectText(g));
    appendWord(out, complements(g, a, uo, ui));
    for (const std::string& e : end) appendWord(out, e);
    appendWord(out, subordinates(g, false));
    return out;
  }
};

}  // namespace

std::string realiseSpanish(const Built& b, const RealiseIn& in, std::vector<std::string>& flags) {
  Es e(in, flags);
  return e.sentence(b);
}

}  // namespace vp::la2x::detail
