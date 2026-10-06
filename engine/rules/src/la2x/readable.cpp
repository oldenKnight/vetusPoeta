// Latin roles -> frame::SemFrame (DESIGN.md §10.1 item 5, §10.6). Works on the disambiguated Sentence: per clause it
// groups noun phrases (prepositions, agreement, genitives, coordination), finds the predicate (finite verb,
// periphrastic passive, modal + infinitive, acc + inf, nōlī + infinitive), assigns roles from the cases (nominative
// subject agreeing with the verb, accusative object, dative indirect object, ablative / prepositional obliques,
// vocatives), reads negation, questions (-ne, num, nōnne, wh words), imperatives and subordinators, and attaches
// relative clauses to their antecedent. Lemma strings in the frame are Latin keys; token indices point into the
// Sentence. The realisers (realise_en.cpp, realise_es.cpp) turn it into text.
#include <algorithm>
#include <functional>

#include "internal.h"
#include "vp/morph.h"
#include "vp/text.h"

namespace vp::la2x::detail {

using namespace vp::feat;
using frame::Kind;
using frame::Relation;
using frame::Role;
using frame::SemFrame;
using frame::SemNP;

uint32_t verbOfParticipleLemma(const lex::Lexicon& la, uint32_t p, std::vector<lex::Analysis>& buf) {
  if (p == lex::kNoLemma || p >= la.lemmaCount()) return lex::kNoLemma;
  const lex::Lemma pl = la.lemma(p);
  if (pl.pos != Participle) return lex::kNoLemma;
  buf.clear();
  la.lookup(pl.key, buf);
  uint32_t best = lex::kNoLemma;
  long bestScore = -1;
  const std::string head = morph::displayForm(pl.head, true);
  for (const lex::Analysis& a : buf) {
    const Features f = unpack(la.feature(a.feat));
    if (f.pos != Verb || f.mood != ParticipleMood) continue;
    const lex::Lemma vl = la.lemma(a.lemma);
    if (vl.pos != Verb) continue;
    long sc = 0;
    if (morph::displayForm(a.display, true) == head) sc += 1L << 20;   // vīsus: videō, not vīsō
    sc += (long)(4 - std::min<uint8_t>(3, vl.tier ? vl.tier : 3)) << 16;
    sc += 65535 - (vl.freqRank ? std::min<long>(65535, (long)vl.freqRank) : 65535);
    if (sc > bestScore || (sc == bestScore && a.lemma < best)) { bestScore = sc; best = a.lemma; }
  }
  return best;
}

void tokInfos(const lex::Lexicon& la, const Sentence& s, std::vector<TokInfo>& out) {
  out.clear();
  std::vector<lex::Analysis> buf;
  out.resize(s.tokens.size());
  for (size_t i = 0; i < s.tokens.size(); ++i) {
    const Token& t = s.tokens[i];
    TokInfo& x = out[i];
    x.text = t.text;
    x.encl = t.encl;
    x.word = t.kind == TokKind::Word;
    x.number = t.kind == TokKind::Number;
    x.punct = t.kind == TokKind::Punct;
    const Reading* r = t.best();
    if (!r) {
      x.key = text::latin_key(t.text);
      continue;
    }
    x.lemma = r->lemma;
    x.f = unpack(r->packed);
    x.name = r->name;
    if (r->lemma != lex::kNoLemma) {
      const lex::Lemma l = la.lemma(r->lemma);
      x.key = std::string(l.key);
      x.lpos = l.pos;
      x.lgender = l.gender;
      x.lflags = l.flags;
    } else {
      x.key = text::latin_key(r->display);
      x.lpos = x.f.pos;
      x.lgender = r->nameGender;
    }
    if (!x.f.gender && r->nameGender) x.f.gender = r->nameGender;
    // participles: the verb they belong to (deponents read actively in periphrases)
    if (x.lpos == Participle) x.verbLemma = verbOfParticipleLemma(la, r->lemma, buf);
    else if (x.lpos == Verb && x.f.mood == ParticipleMood) x.verbLemma = r->lemma;
    if (x.verbLemma != lex::kNoLemma) x.deponent = (la.lemma(x.verbLemma).flags & lex::Deponent) != 0;
    else if (x.lpos == Verb) x.deponent = (x.lflags & lex::Deponent) != 0;
    // comparative lemmas (altior, melior) carry the degree
    if (x.lpos == Adj && x.f.degree == 0 && x.key.size() > 3 && x.key.compare(x.key.size() - 3, 3, "ior") == 0)
      x.f.degree = Comparative;
  }
}

namespace {

bool isKey(const std::string& k, std::initializer_list<const char*> ks) {
  for (const char* x : ks)
    if (k == x) return true;
  return false;
}

struct NPx {                 // a noun phrase under construction
  SemNP np;
  uint8_t case_ = 0, number = 0, gender = 0;
  int first = 0, last = 0;   // token span (for order)
  bool governed = false;     // object of a preposition
  bool adjOnly = false;      // made of modifiers only (substantive adjective or predicate)
  bool used = false;
};

class Builder {
 public:
  Builder(const lex::Lexicon& la, const Tables& tab, const Sentence& s, const std::vector<TokInfo>& ti, Built& out)
      : la_(la), tab_(tab), s_(s), ti_(ti), out_(out) {}

  void run() {
    const size_t n = s_.tokens.size();
    used_.assign(n, 0);
    out_.roles.assign(n, std::string());
    out_.frames.clear();
    out_.joiners.clear();
    phrases();
    // top-level clauses in order
    std::vector<int> top;
    for (size_t c = 0; c < s_.clauses.size(); ++c)
      if (s_.clauses[c].parent < 0 && s_.clauses[c].last >= 0) top.push_back((int)c);
    std::sort(top.begin(), top.end(), [&](int a, int b) { return s_.clauses[(size_t)a].first < s_.clauses[(size_t)b].first; });
    for (size_t k = 0; k < top.size(); ++k) {
      SemFrame f;
      clause(top[k], f);
      if (k > 0) {
        const Clause& c = s_.clauses[(size_t)top[k]];
        std::string j = ",";
        if (c.marker >= 0) {
          j = ti_[(size_t)c.marker].key;
          used_[(size_t)c.marker] = 1;
          // the connector is realised by the joiner, not again by the frame
          f.connectors.erase(std::remove(f.connectors.begin(), f.connectors.end(), j), f.connectors.end());
        } else if (c.first >= 0 && (size_t)c.first < n && ti_[(size_t)c.first].encl == "que") {
          j = "et";
        } else if (c.first > 0 && ti_[(size_t)c.first - 1].punct &&
                   (s_.tokens[(size_t)c.first - 1].text == ";" || s_.tokens[(size_t)c.first - 1].text == ":")) {
          j = s_.tokens[(size_t)c.first - 1].text;
        }
        out_.joiners.push_back(j);
      }
      out_.frames.push_back(std::move(f));
    }
    if (out_.frames.empty()) {
      SemFrame f;
      f.type = Kind::Frag;
      out_.frames.push_back(f);
    }
    // fill
    size_t words = 0, covered = 0;
    for (size_t i = 0; i < n; ++i) {
      if (!ti_[i].word && !ti_[i].number) continue;
      ++words;
      if (used_[i]) ++covered;
    }
    out_.fill = words ? (double)covered / (double)words : 1.0;
  }

 private:
  const lex::Lexicon& la_;
  const Tables& tab_;
  const Sentence& s_;
  const std::vector<TokInfo>& ti_;
  Built& out_;
  std::vector<char> used_;
  int relMarker_ = -1;
  struct Chunk { int first = 0, last = 0; std::string text; bool wh = false; };
  std::vector<Chunk> chunks_;

  // ---- fixed phrases (readable_*.tsv "phrase") ----
  void phrases() {
    const size_t n = s_.tokens.size();
    std::vector<std::pair<std::vector<std::string>, const Row*>> pats;
    for (const Row& r : tab_.rows()) {
      if (r.kind != "phrase") continue;
      std::vector<std::string> w;
      size_t a = 0;
      while (a < r.latin.size()) {
        size_t b = r.latin.find(' ', a);
        if (b == std::string::npos) b = r.latin.size();
        if (b > a) w.push_back(r.latin.substr(a, b - a));
        a = b + 1;
      }
      pats.emplace_back(w, &r);
    }
    for (size_t i = 0; i < n; ++i) {
      if (!ti_[i].word || used_[i]) continue;
      size_t bestLen = 0;
      const Row* best = nullptr;
      size_t bestEnd = i;
      for (const auto& p : pats) {
        size_t k = i, w = 0;
        while (w < p.first.size() && k < n) {
          if (!ti_[k].word) break;
          if (text::latin_key(s_.tokens[k].text) != p.first[w] && ti_[k].key != p.first[w]) break;
          ++w;
          ++k;
        }
        if (w == p.first.size() && w > bestLen) { bestLen = w; best = p.second; bestEnd = k - 1; }
      }
      if (best) {
        for (size_t k = i; k <= bestEnd; ++k) { used_[k] = 1; out_.roles[k] = "phrase"; }
        chunks_.push_back(Chunk{(int)i, (int)bestEnd, best->text, noteHas(best->note, "wh")});
        i = bestEnd;
      }
    }
  }

  bool inClause(size_t i, int c) const { return s_.tokens[i].clause == c; }

  static uint8_t genderOf(const TokInfo& t) { return t.f.gender ? t.f.gender : t.lgender; }
  static bool genderOk(uint8_t a, uint8_t b) {
    auto bits = [](uint8_t g) -> int {
      switch (g) { case M: return 1; case F: return 2; case N: return 4; case MF: return 3; case MN: return 5;
                   case FN: return 6; default: return 7; }
    };
    return (bits(a) & bits(b)) != 0;
  }

  bool isNominal(const TokInfo& t) const {
    const uint8_t p = t.f.pos ? t.f.pos : t.lpos;
    if (t.name) return true;
    if (t.f.case_ == 0) return false;
    return p == Noun || p == Adj || p == Pron || p == Det || p == Num || p == Name || p == Participle ||
           (p == Verb && t.f.mood == ParticipleMood);
  }
  bool isHeadTok(const TokInfo& t) const {
    const uint8_t p = t.f.pos ? t.f.pos : t.lpos;
    if (relMarker_ >= 0 && &t == &ti_[(size_t)relMarker_]) return true;
    if (t.name || p == Noun || p == Name) return true;
    if (p == Pron && !isDetKey(t.key)) return true;
    return false;
  }
  bool isDetKey(const std::string& k) const {
    return tab_.has("det", k) || isKey(k, {"suus", "meus", "tuus", "noster", "uester", "hic", "ille", "iste", "is",
                                            "idem", "ipse", "qui"});
  }
  bool isParticiple(const TokInfo& t) const {
    return t.f.pos == Participle || t.lpos == Participle || (t.f.pos == Verb && t.f.mood == ParticipleMood);
  }
  bool isFinite(const TokInfo& t) const {
    return t.f.pos == Verb && (t.f.mood == Indicative || t.f.mood == Subjunctive || t.f.mood == Imperative) && t.f.person;
  }
  bool isInf(const TokInfo& t) const { return t.f.pos == Verb && t.f.mood == Infinitive; }

  // ---- one clause --------------------------------------------------------------------------------------------------
  void clause(int ci, SemFrame& f) {
    const Clause& C = s_.clauses[(size_t)ci];
    const size_t n = s_.tokens.size();
    std::vector<size_t> toks;
    for (size_t i = 0; i < n; ++i)
      if (inClause(i, ci) && (ti_[i].word || ti_[i].number) && !used_[i]) toks.push_back(i);
    const bool question = s_.finalPunct == "?";
    f.punct = s_.finalPunct;
    if (C.marker >= 0 && C.kind == Clause::Sub) used_[(size_t)C.marker] = 1;
    relMarker_ = C.kind == Clause::Relative ? C.marker : -1;
    // phrase chunks of this clause ride along as discourse items ("phrase:<text>"; position by token)
    for (const Chunk& ch : chunks_) {
      if (s_.tokens[(size_t)ch.first].clause != ci) continue;
      if (ch.wh && s_.finalPunct == "?") {
        f.type = Kind::Wh;
        f.wh.word = "#" + ch.text;
        f.wh.role = Role::Adverb;
        f.wh.token = ch.first;
        continue;
      }
      f.discourse.push_back("phrase:" + ch.text + (ch.first == 0 ? "" : "|end"));
    }

    // the verb group
    int verb = C.verb;
    if (verb >= 0 && used_[(size_t)verb]) verb = -1;
    if (verb >= 0 && isKey(ti_[(size_t)verb].key, {"salueo", "ualeo"}) && ti_[(size_t)verb].f.mood == Imperative &&
        tab_.has("intj", text::latin_key(s_.tokens[(size_t)verb].text)))
      verb = -1;   // salvē / valēte: greetings
    std::vector<size_t> infs, parts;
    for (size_t i : toks) {
      if (isInf(ti_[i])) infs.push_back(i);
      if (isParticiple(ti_[i])) parts.push_back(i);
    }
    // connectors, interjections, adverbs, negation, wh words
    std::vector<int> intensOf(n, -1);
    bool firstWord = true;
    int quamComparative = -1;
    for (size_t idx = 0; idx < toks.size(); ++idx) {
      const size_t i = toks[idx];
      const TokInfo& t = ti_[i];
      const std::string tk = text::latin_key(s_.tokens[i].text);
      const bool clauseFirst = firstWord;
      firstWord = false;
      if ((int)i == verb || !t.word) continue;
      if (isKey(tk, {"domi", "domum", "rus", "ruri", "rure", "humi"}) && tab_.has("adv", tk) &&
          (t.f.case_ == Loc || t.f.case_ == Acc || t.f.case_ == Abl || t.lpos == Adv) &&
          !(idx > 0 && ti_[toks[idx - 1]].lpos == Prep)) {
        SemAdverbPush(f, tk, (int)i, clauseFirst);
        used_[i] = 1;
        out_.roles[i] = "adverb";
        continue;
      }
      // interjections
      const bool answer = isKey(tk, {"ita", "minime", "immo"}) && clauseFirst &&
                          (idx + 1 >= toks.size() || (i + 1 < n && ti_[i + 1].punct && s_.tokens[i + 1].text == ","));
      if (answer || t.lpos == Intj || isKey(tk, {"quaeso"}) ||
          (tab_.has("intj", tk) && (t.lpos == Particle || (t.f.mood == Imperative && t.lpos == Verb &&
                                                             isKey(t.key, {"salueo", "ualeo"}))))) {
        if (tk == "quaeso" && !clauseFirst) {
          SemAdverbPush(f, "quaeso", (int)i, false);
        } else {
          f.interjections.push_back(tk);
        }
        used_[i] = 1;
        out_.roles[i] = "interjection";
        continue;
      }
      bool compBefore = false;
      for (size_t q = 0; q < idx; ++q) compBefore = compBefore || ti_[toks[q]].f.degree == Comparative;
      if (t.lpos == Conj && t.key == "quam" && compBefore) {
        quamComparative = (int)i;
        used_[i] = 1;
        out_.roles[i] = "comparison";
        continue;
      }
      if (t.lpos == Conj) {
        if (tab_.has("conj", t.key) && (clauseFirst || tab_.tagged("conj", t.key, "second"))) {
          f.connectors.push_back(t.key);
          used_[i] = 1;
          out_.roles[i] = "connector";
        }
        continue;   // coordinators between NPs are read by the NP grouping; subordinators are clause markers
      }
      if (t.lpos == Adv || t.lpos == Particle) {
        const std::string k = tab_.has("adv", tk) || tab_.has("wh", tk) ? tk : t.key;
        if (isKey(k, {"non", "haud"})) { f.negative = true; used_[i] = 1; out_.roles[i] = "negation"; continue; }
        if (k == "nonne") { f.expectYes = true; f.type = Kind::Yn; used_[i] = 1; out_.roles[i] = "question"; continue; }
        if (k == "num" && question) { f.discourse.push_back("num"); f.type = Kind::Yn; used_[i] = 1; out_.roles[i] = "question"; continue; }
        if (question && tab_.has("wh", k) && !isKey(k, {"qui"})) {
          f.type = Kind::Wh;
          f.wh.word = k;
          f.wh.token = (int)i;
          f.wh.role = Role::Adverb;
          used_[i] = 1;
          out_.roles[i] = "question word";
          continue;
        }
        if (k == "quam" && s_.finalPunct == "!" && idx + 1 < toks.size() && clauseFirst && t.lpos == Adv) {
          f.exclQuam = true;
          f.type = Kind::Excl;
          used_[i] = 1;
          out_.roles[i] = "exclamation";
          continue;
        }
        if (k == "quam" && compBefore) {
          quamComparative = (int)i;
          used_[i] = 1;
          out_.roles[i] = "comparison";
          continue;
        }
        if (tab_.tagged("adv", k, "intens") && idx + 1 < toks.size()) {
          const TokInfo& nx = ti_[toks[idx + 1]];
          const uint8_t np = nx.f.pos ? nx.f.pos : nx.lpos;
          if (np == Adj || np == Adv || isParticiple(nx)) {
            intensOf[toks[idx + 1]] = (int)i;
            used_[i] = 1;
            out_.roles[i] = "intensifier";
            continue;
          }
        }
        frame::SemAdverb a;
        a.lemma = k;
        a.token = (int)i;
        a.front = clauseFirst;
        f.adverbs.push_back(a);
        used_[i] = 1;
        out_.roles[i] = "adverb";
        continue;
      }
    }
    // -ne on any word: a yes/no question
    for (size_t i : toks)
      if (s_.tokens[i].encl == "ne") f.type = Kind::Yn;
    if (question && f.type == Kind::Decl) f.type = Kind::Yn;

    // predicate
    std::vector<NPx> nps;
    bool passivePeri = false;
    int partTok = -1;
    if (verb >= 0) {
      const TokInfo& v = ti_[(size_t)verb];
      f.hasPred = true;
      f.pred.lemma = v.key;
      f.pred.token = verb;
      used_[(size_t)verb] = 1;
      out_.roles[(size_t)verb] = "verb";
      setTense(v, f.pred);
      if (v.f.mood == Imperative) {
        f.type = Kind::Imp;
        f.imperativePlural = v.f.number == Pl;
      }
      // periphrastic passive: perfect participle (nominative) + sum
      if (v.key == "sum") {
        for (size_t p : parts) {
          const TokInfo& pt = ti_[p];
          if ((pt.f.case_ != Nom && pt.f.case_ != 0) || used_[p]) continue;
          if (pt.f.number && v.f.number && pt.f.number != v.f.number) continue;
          const std::string& hk = pt.key;
          const bool verbPart = pt.f.pos == Verb && pt.f.mood == ParticipleMood && pt.f.tense == Perfect;
          if (tab_.find("lex", text::latin_key(s_.tokens[p].text), "adj") || tab_.find("lex", pt.key, "adj")) continue;
          if (verbPart || (hk.size() > 2 && (hk.compare(hk.size() - 2, 2, "us") == 0))) {
            passivePeri = true;
            partTok = (int)p;
            break;
          }
        }
        if (passivePeri) {
          const TokInfo& pt = ti_[(size_t)partTok];
          f.pred.lemma = pt.verbLemma != lex::kNoLemma ? std::string(la_.lemma(pt.verbLemma).key) : pt.key;
          f.pred.complementToken = partTok;   // the participle token carries the lexical verb
          // deponent perfect (ingressus est, secūta est, locūtī sunt): one active verb
          f.pred.voice = pt.deponent ? frame::Voice::Active : frame::Voice::Passive;
          out_.periphrases.push_back(Periphrasis{partTok, verb, pt.deponent, (int)v.f.tense});
          f.pred.auxTokens.push_back(verb);
          used_[(size_t)partTok] = 1;
          out_.roles[(size_t)partTok] = "verb";
          // sum present -> perfect passive; imperfect -> pluperfect; future -> future perfect
          if (v.f.tense == Present) { f.pred.tense = frame::Tense::Past; f.pred.aspect = frame::Aspect::Simple; }
          else if (v.f.tense == Imperfect) { f.pred.tense = frame::Tense::Past; f.pred.aspect = frame::Aspect::Perfect; }
          else if (v.f.tense == Future) { f.pred.tense = frame::Tense::Future; f.pred.aspect = frame::Aspect::Perfect; }
          f.pred.particle = "peri";
        } else {
          f.copula = true;
        }
      }
      // nōlī / nōlīte + infinitive: a prohibition
      if (v.key == "nolo" && v.f.mood == Imperative && !infs.empty()) {
        const size_t inf = infs[0];
        f.type = Kind::Imp;
        f.negative = true;
        f.imperativePlural = v.f.number == Pl;
        f.pred.lemma = ti_[inf].key;
        f.pred.token = (int)inf;
        f.pred.auxTokens.push_back(verb);
        f.pred.mood = frame::SrcMood::Indicative;
        used_[inf] = 1;
        out_.roles[inf] = "verb";
        out_.roles[(size_t)verb] = "prohibition";
        f.copula = ti_[inf].key == "sum";
        infs.erase(infs.begin());
      }
    } else if (!infs.empty()) {
      // an infinitive alone (fragment, exclamation): the infinitive is the predicate
      const size_t inf = infs[0];
      f.hasPred = true;
      f.pred.lemma = ti_[inf].key;
      f.pred.token = (int)inf;
      f.pred.mood = frame::SrcMood::Indicative;
      f.discourse.push_back("infinitive");
      used_[inf] = 1;
      out_.roles[inf] = "verb";
      infs.erase(infs.begin());
    }

    // noun phrases
    groupNPs(ci, toks, intensOf, nps);

    // acc + inf / modal + inf / verb + inf
    if (f.hasPred && !infs.empty() && verb >= 0 && f.type != Kind::Imp) {
      const TokInfo& v = ti_[(size_t)verb];
      const size_t inf = infs[0];
      const bool modal = isKey(v.key, {"possum", "uolo", "nolo", "malo", "debeo", "cupio", "incipio", "soleo", "audeo",
                                       "conor", "desino", "coepi", "studeo", "nescio", "scio", "disco", "constituo",
                                       "statuo", "paro", "festino", "timeo", "dubito", "opto"});
      const bool sayThink = isKey(v.key, {"dico", "puto", "credo", "nego", "nuntio", "spero", "sentio", "intellego",
                                          "existimo", "arbitror", "respondeo", "narro", "ostendo", "uideo", "audio",
                                          "scio", "nescio", "gaudeo", "promitto", "iuro", "affirmo", "memini",
                                          "cognosco", "intellegeo", "simulo"});
      const bool jussive = isKey(v.key, {"iubeo", "ueto", "cogo", "sino", "patior", "doceo", "impero", "rogo",
                                         "oro", "moneo", "hortor"});
      // an accusative before / after the infinitive that is not the main object of a jussive verb
      int accSubj = -1;
      for (size_t k = 0; k < nps.size(); ++k)
        if (nps[k].case_ == Acc && !nps[k].governed) { accSubj = (int)k; break; }
      if (sayThink && accSubj >= 0 && !(modal && v.key != "scio" && v.key != "nescio" ? true : false)) {
        // complement clause: subject accusative, the infinitive, a second accusative as its object
        SemFrame sub;
        sub.hasPred = true;
        sub.pred.lemma = ti_[inf].key;
        sub.pred.token = (int)inf;
        sub.pred.mood = frame::SrcMood::Indicative;
        setInfTense(ti_[inf], v, sub.pred);
        used_[inf] = 1;
        out_.roles[inf] = "verb (that-clause)";
        NPx& sj = nps[(size_t)accSubj];
        sub.hasSubject = true;
        sub.subject = sj.np;
        sj.used = true;
        markRole(sj, "subject (that-clause)");
        // prepositional phrases and ablatives after the subject accusative belong to the that-clause
        for (NPx& x : nps) {
          if (x.used || x.first <= sj.first || !(x.governed || x.case_ == Abl || x.case_ == Loc)) continue;
          frame::SemOblique o;
          o.np = x.np;
          o.token = x.np.token;
          if (x.governed) {
            const int pt = x.np.tokens.empty() ? -1 : x.np.tokens.front();
            o.prep = pt >= 0 ? ti_[(size_t)pt].key : std::string();
            o.np.tokens.erase(o.np.tokens.begin());
            o.token = pt;
            o.prep += x.case_ == Acc ? ":acc" : x.case_ == Abl ? ":abl" : "";
          } else {
            o.prep = x.case_ == Loc ? "#loc" : "#abl";
          }
          sub.obliques.push_back(o);
          x.used = true;
          markRole(x, "prepositional phrase (that-clause)");
        }
        if (ti_[inf].key == "sum") {
          sub.copula = true;
          for (NPx& x : nps) {
            if (x.used || x.governed || x.case_ != Acc) continue;
            if (x.adjOnly) for (const frame::SemAdj& ad : x.np.adjectives) sub.predAdj.push_back(ad);
            else sub.predicative.push_back(x.np);
            x.used = true;
            markRole(x, "predicate (that-clause)");
          }
          // an adjective that agreed with the subject accusative is its predicate
          if (sub.predAdj.empty() && sub.predicative.empty() && !sub.subject.adjectives.empty()) {
            sub.predAdj.push_back(sub.subject.adjectives.back());
            sub.subject.adjectives.pop_back();
          }
        }
        for (NPx& x : nps) {
          if (x.used || x.governed || x.case_ != Acc) continue;
          sub.hasObject = true;
          sub.object = x.np;
          x.used = true;
          markRole(x, "object (that-clause)");
          break;
        }
        frame::SemSub ss;
        ss.relation = Relation::Complement;
        ss.marker = "acc+inf";
        ss.before = false;
        ss.frame.push_back(std::move(sub));
        f.subordinate.push_back(std::move(ss));
        infs.erase(infs.begin());
      } else if (sayThink && accSubj < 0 && ti_[inf].key == "sum") {
        // "putābam diem Lūnae esse" with the subject inside a fixed phrase: that it was Monday
        std::string chunk;
        for (size_t d = 0; d < f.discourse.size(); ++d)
          if (f.discourse[d].compare(0, 7, "phrase:") == 0) {
            chunk = f.discourse[d].substr(7);
            const size_t bar = chunk.find("|end");
            if (bar != std::string::npos) chunk = chunk.substr(0, bar);
            f.discourse.erase(f.discourse.begin() + (long)d);
            break;
          }
        SemFrame sub;
        sub.hasPred = true;
        sub.copula = true;
        sub.pred.lemma = "sum";
        sub.pred.token = (int)inf;
        setInfTense(ti_[inf], v, sub.pred);
        sub.hasSubject = true;
        sub.implicitSubject = true;
        sub.subject.isPronoun = true;
        sub.subject.pron.person = 3;
        sub.subject.pron.number = 1;
        sub.subject.pron.gender = N;
        if (!chunk.empty()) sub.discourse.push_back("phrase-pred:" + chunk);
        used_[inf] = 1;
        out_.roles[inf] = "verb (that-clause)";
        frame::SemSub ss;
        ss.relation = Relation::Complement;
        ss.marker = "acc+inf";
        ss.frame.push_back(std::move(sub));
        f.subordinate.push_back(std::move(ss));
        infs.erase(infs.begin());
      } else if (jussive && accSubj >= 0) {
        f.pred.complementVerb = ti_[inf].key;
        f.pred.complementToken = (int)inf;
        f.discourse.push_back("jussive");
        used_[inf] = 1;
        out_.roles[inf] = "infinitive";
        infs.erase(infs.begin());
      } else if (modal || !v.key.empty()) {
        f.pred.complementVerb = ti_[inf].key;
        f.pred.complementToken = (int)inf;
        used_[inf] = 1;
        out_.roles[inf] = "infinitive";
        infs.erase(infs.begin());
      }
    }

    // ablative absolute: an ablative participle with an ablative noun, no preposition, a finite verb elsewhere
    for (size_t k = 0; k < nps.size(); ++k) {
      NPx& x = nps[k];
      if (x.used || x.governed || x.case_ != Abl || verb < 0) continue;
      int part = -1;
      for (const frame::SemAdj& a : x.np.adjectives)
        if (a.token >= 0 && isParticiple(ti_[(size_t)a.token])) part = a.token;
      if (part < 0 && x.adjOnly && x.np.token >= 0 && isParticiple(ti_[(size_t)x.np.token])) part = x.np.token;
      if (part < 0) continue;
      // a second ablative noun that agrees (the subject of the participle) or the NP's own head
      SemFrame sub;
      sub.hasPred = true;
      sub.pred.lemma = ti_[(size_t)part].key;
      sub.pred.token = (int)part;
      sub.pred.complementToken = (int)part;
      const std::string& pk = ti_[(size_t)part].key;
      const bool present = pk.size() > 2 && pk.compare(pk.size() - 2, 2, "ns") == 0;
      sub.pred.tense = frame::Tense::Past;
      sub.pred.aspect = present ? frame::Aspect::Progressive : frame::Aspect::Perfect;
      sub.pred.voice = present ? frame::Voice::Active : frame::Voice::Passive;
      sub.pred.particle = present ? "part-pres" : "part-perf";
      SemNP subj = x.np;
      subj.adjectives.erase(std::remove_if(subj.adjectives.begin(), subj.adjectives.end(),
                                           [&](const frame::SemAdj& a) { return a.token == part; }),
                            subj.adjectives.end());
      if (!x.adjOnly) {
        sub.hasSubject = true;
        sub.subject = subj;
      }
      x.used = true;
      markRole(x, "ablative absolute");
      out_.roles[(size_t)part] = "ablative absolute";
      frame::SemSub ss;
      ss.relation = Relation::Time;
      ss.marker = present ? "ablabs-pres" : "ablabs-perf";
      ss.before = x.first < verb;
      ss.frame.push_back(std::move(sub));
      f.subordinate.push_back(std::move(ss));
      out_.flags.push_back("abl-abs");
    }

    assignRoles(ci, f, verb, nps, quamComparative);

    // wh word as a pronoun NP (quis / quid / quem ...)
    if (question) {
      auto checkWh = [&](SemNP& np, Role role) {
        if (np.token < 0) return;
        const TokInfo& t = ti_[(size_t)np.token];
        if (isKey(t.key, {"quis", "quid"}) || (t.key == "qui" && np.adjectives.empty() && np.token == (int)toks.front())) {
          np.interrogative = true;
          np.wh = t.key == "quid" || (t.key == "quis" && genderOf(t) == N) ? "quid" : "quis";
          if (f.wh.word.empty()) {
            f.type = Kind::Wh;
            f.wh.word = np.wh;
            f.wh.role = role;
            f.wh.token = np.token;
          }
        }
        if (!np.determiner.empty() && isKey(np.determiner, {"qui", "quis", "quot", "qualis", "quantus", "uter"})) {
          np.interrogative = true;
          np.wh = isKey(np.determiner, {"quis"}) ? "qui" : np.determiner;
          if (f.wh.word.empty()) {
            f.type = Kind::Wh;
            f.wh.word = np.wh;
            f.wh.role = role;
            f.wh.token = np.token;
          }
        }
      };
      if (f.hasSubject) checkWh(f.subject, Role::Subject);
      if (f.hasObject) checkWh(f.object, Role::Object);
      if (f.hasIndirect) checkWh(f.indirectObject, Role::IndirectObject);
      for (SemNP& p : f.predicative) checkWh(p, Role::Predicate);
      for (frame::SemOblique& o : f.obliques) checkWh(o.np, Role::Oblique);
    }

    // subordinate clauses of this clause
    for (size_t c = 0; c < s_.clauses.size(); ++c) {
      const Clause& K = s_.clauses[c];
      if (K.parent != ci || K.last < 0) continue;
      SemFrame sf;
      clause((int)c, sf);
      if (K.kind == Clause::Relative) {
        // the relative pronoun's role inside its clause
        if (!attachRelative(f, K, sf)) {
          frame::SemSub ss;
          ss.relation = Relation::Relative;
          ss.marker = "qui";
          ss.before = false;
          ss.frame.push_back(std::move(sf));
          f.subordinate.push_back(std::move(ss));
        }
        continue;
      }
      frame::SemSub ss;
      std::string mk = K.marker >= 0 ? ti_[(size_t)K.marker].key : std::string();
      if (K.kind == Clause::Coord) {
        ss.relation = Relation::Coord;
        mk = K.marker >= 0 ? ti_[(size_t)K.marker].key : "et";
      } else {
        ss.relation = relationOf(mk, sf);
      }
      ss.marker = mk;
      int firstParent = -1;
      for (size_t i = 0; i < n; ++i)
        if (s_.tokens[i].clause == ci && ti_[i].word) { firstParent = (int)i; break; }
      ss.before = firstParent < 0 || K.first < firstParent || (verb >= 0 && K.last < verb && K.first <= (firstParent < 0 ? 0 : firstParent));
      if (verb >= 0 && K.first < verb && firstParent >= 0 && K.first < firstParent) ss.before = true;
      ss.frame.push_back(std::move(sf));
      f.subordinate.push_back(std::move(ss));
    }
    // left-over words (unknown words, stray modifiers) are kept as adverb-like items so nothing is lost
    for (size_t i : toks) {
      if (used_[i] || !(ti_[i].word || ti_[i].number)) continue;
      frame::SemAdverb a;
      a.lemma = "#tok";
      a.token = (int)i;
      a.front = false;
      f.adverbs.push_back(a);
      used_[i] = 1;
      if (out_.roles[i].empty()) out_.roles[i] = "other";
    }
    if (!f.hasPred && f.type == Kind::Decl) f.type = Kind::Frag;
    if (!f.hasPred && f.type == Kind::Yn) f.type = Kind::Frag;
  }

  void SemAdverbPush(SemFrame& f, const std::string& k, int tok, bool front) {
    frame::SemAdverb a;
    a.lemma = k;
    a.token = tok;
    a.front = front;
    f.adverbs.push_back(a);
  }

  static Relation relationOf(const std::string& mk, const SemFrame& sf) {
    if (isKey(mk, {"quod", "quia", "quoniam"})) return Relation::Cause;
    if (isKey(mk, {"si", "nisi"})) return Relation::Condition;
    if (isKey(mk, {"ut", "ne"})) return sf.pred.mood == frame::SrcMood::Subjunctive ? Relation::Purpose : Relation::Manner;
    if (isKey(mk, {"quamquam", "etsi"})) return Relation::Concession;
    return Relation::Time;
  }

  void setTense(const TokInfo& v, frame::SemPredicate& p) const {
    p.voice = v.f.voice == Passive && !(v.lflags & lex::Deponent) ? frame::Voice::Passive : frame::Voice::Active;
    p.mood = v.f.mood == Subjunctive ? frame::SrcMood::Subjunctive : frame::SrcMood::Indicative;
    switch (v.f.tense) {
      case Imperfect: p.tense = frame::Tense::Past; p.aspect = frame::Aspect::Progressive; break;
      case Perfect: p.tense = frame::Tense::Past; p.aspect = frame::Aspect::Simple; break;
      case Pluperfect: p.tense = frame::Tense::Past; p.aspect = frame::Aspect::Perfect; break;
      case Future: p.tense = frame::Tense::Future; p.aspect = frame::Aspect::Simple; break;
      case FuturePerfect: p.tense = frame::Tense::Future; p.aspect = frame::Aspect::Perfect; break;
      default: p.tense = frame::Tense::Present; p.aspect = frame::Aspect::Simple; break;
    }
  }
  // tense of an infinitive relative to the main verb (sequence of tenses, plain)
  void setInfTense(const TokInfo& inf, const TokInfo& main, frame::SemPredicate& p) const {
    const bool past = main.f.tense == Imperfect || main.f.tense == Perfect || main.f.tense == Pluperfect;
    p.voice = inf.f.voice == Passive && !(inf.lflags & lex::Deponent) ? frame::Voice::Passive : frame::Voice::Active;
    if (inf.f.tense == Perfect) {
      p.tense = frame::Tense::Past;
      p.aspect = past ? frame::Aspect::Perfect : frame::Aspect::Simple;
    } else if (inf.f.tense == Future) {
      p.tense = frame::Tense::Future;
      p.habitual = past;   // "would"
    } else {
      p.tense = past ? frame::Tense::Past : frame::Tense::Present;
      p.aspect = past ? frame::Aspect::Progressive : frame::Aspect::Simple;
    }
  }

  void markRole(const NPx& x, const std::string& role) {
    for (int t : x.np.tokens)
      if (t >= 0 && (size_t)t < out_.roles.size() && (out_.roles[(size_t)t].empty() || out_.roles[(size_t)t] == "noun phrase"))
        out_.roles[(size_t)t] = role;
    if (x.np.token >= 0) out_.roles[(size_t)x.np.token] = role;
  }

  // ---- noun phrases ----------------------------------------------------------------------------------------------------
  void initNP(size_t h, NPx& x) {
    const TokInfo& t = ti_[h];
    x.np.token = (int)h;
    x.np.head = t.key;
    x.np.surface = s_.tokens[h].text;
    x.np.number = t.f.number == Pl ? 2 : 1;
    x.np.isName = t.name;
    x.np.srcGender = genderOf(t);
    x.np.tokens.push_back((int)h);
    x.case_ = t.f.case_;
    x.number = t.f.number;
    x.gender = genderOf(t);
    x.first = x.last = (int)h;
    const uint8_t p = t.f.pos ? t.f.pos : t.lpos;
    if (p == Pron && !t.name) {
      x.np.isPronoun = true;
      x.np.pronLemma = t.key;
      x.np.pron.number = t.f.number == Pl ? 2 : t.f.number == Sg ? 1 : 0;
      x.np.pron.gender = genderOf(t);
      x.np.pron.person = 3;
      if (isKey(t.key, {"ego", "nos"})) { x.np.pron.person = 1; if (t.key == "nos") x.np.pron.number = 2; }
      if (isKey(t.key, {"tu", "uos"})) { x.np.pron.person = 2; if (t.key == "uos") x.np.pron.number = 2; }
      if (isKey(t.key, {"sui", "se", "sibi", "sese"})) x.np.pron.reflexive = true;
      if (isKey(t.key, {"nemo", "nihil"})) x.np.negative = true;
    }
    if (t.lpos == Num || t.f.pos == Num) {
      x.np.numeral = s_.tokens[h].text;
    }
  }

  void addMod(NPx& x, size_t m, const std::vector<int>& intensOf) {
    const TokInfo& t = ti_[m];
    x.np.tokens.push_back((int)m);
    x.first = std::min(x.first, (int)m);
    x.last = std::max(x.last, (int)m);
    used_[m] = 1;
    const uint8_t p = t.f.pos ? t.f.pos : t.lpos;
    if (isDetKey(t.key) && (p == Det || p == Pron || p == Adj || p == Num || tab_.has("det", t.key))) {
      if (isKey(t.key, {"nullus"})) x.np.negative = true;
      if (x.np.determiner.empty()) x.np.determiner = t.key;
      else {
        frame::SemAdj a;
        a.lemma = t.key;
        a.token = (int)m;
        x.np.adjectives.push_back(a);
      }
      // determiner token: remember it through `tokens`; the realiser looks the determiner up by key
      return;
    }
    if (p == Num && tab_.has("det", t.key)) {
      x.np.numeral = t.key;
      return;
    }
    frame::SemAdj a;
    a.lemma = t.key;
    a.token = (int)m;
    a.degree = t.f.degree == Comparative ? Comparative : t.f.degree == Superlative ? Superlative : 0;
    if (intensOf[m] >= 0) {
      a.adverbs.push_back(ti_[(size_t)intensOf[m]].key);
      a.advTokens.push_back(intensOf[m]);
    }
    x.np.adjectives.push_back(a);
  }

  void groupNPs(int ci, const std::vector<size_t>& toks, const std::vector<int>& intensOf, std::vector<NPx>& nps) {
    const size_t n = s_.tokens.size();
    (void)ci;
    auto isFree = [&](size_t i) { return !used_[i] && (ti_[i].word || ti_[i].number); };
    auto agree = [&](size_t a, size_t b) {
      const TokInfo& x = ti_[a];
      const TokInfo& y = ti_[b];
      const bool c = x.f.case_ == y.f.case_ || x.f.case_ == 0 || y.f.case_ == 0;
      const bool nn = x.f.number == y.f.number || x.f.number == 0 || y.f.number == 0;
      return c && nn && genderOk(genderOf(x), genderOf(y));
    };
    std::vector<char> inTok(n, 0);
    for (size_t i : toks) inTok[i] = 1;
    // 1. prepositional phrases
    std::vector<std::pair<size_t, NPx>> pps;   // prep token, object
    for (size_t idx = 0; idx < toks.size(); ++idx) {
      const size_t p = toks[idx];
      if (!isFree(p) || ti_[p].lpos != Prep) continue;
      // the governed words: contiguous nominal tokens of one case (plus genitives and intensifiers)
      size_t k = idx + 1;
      int head = -1;
      uint8_t pc = 0;
      std::vector<size_t> span;
      while (k < toks.size()) {
        const size_t j = toks[k];
        if (!isFree(j)) { if (intensOf[j] >= 0 || used_[j]) { ++k; continue; } break; }
        const TokInfo& t = ti_[j];
        if (!isNominal(t)) break;
        if (pc == 0) pc = t.f.case_;
        if (t.f.case_ != pc && t.f.case_ != Gen && t.f.case_ != 0) break;
        if (t.f.case_ == Gen && head >= 0 && !span.empty()) { span.push_back(j); ++k; continue; }
        span.push_back(j);
        if (head < 0 && isHeadTok(t) && t.f.case_ == pc) head = (int)j;
        ++k;
        if (head >= 0 && k < toks.size()) {
          // stop after the head unless the next word agrees with it (adjective after the noun) or is a genitive
          const size_t nx = toks[k];
          if (!(isFree(nx) && isNominal(ti_[nx]) && ((agree((size_t)head, nx) && !isHeadTok(ti_[nx])) || ti_[nx].f.case_ == Gen))) break;
        }
      }
      if (span.empty()) continue;
      if (head < 0) head = (int)span.back();
      NPx x;
      initNP((size_t)head, x);
      used_[(size_t)head] = 1;
      for (size_t j : span) {
        if ((int)j == head) continue;
        if (ti_[j].f.case_ == Gen && ti_[(size_t)head].f.case_ != Gen) {
          NPx g;
          initNP(j, g);
          used_[j] = 1;
          x.np.genitive.push_back(g.np);
          x.np.tokens.push_back((int)j);
          continue;
        }
        addMod(x, j, intensOf);
      }
      x.governed = true;
      x.case_ = ti_[(size_t)head].f.case_ ? ti_[(size_t)head].f.case_ : pc;
      used_[p] = 1;
      out_.roles[p] = "preposition";
      pps.emplace_back(p, std::move(x));
    }
    // 2. heads and their modifiers
    std::vector<size_t> heads, mods;
    for (size_t i : toks) {
      if (!isFree(i) || !isNominal(ti_[i])) continue;
      if (isHeadTok(ti_[i])) heads.push_back(i);
      else mods.push_back(i);
    }
    std::vector<NPx> local;
    std::vector<int> npOfHead(n, -1);
    for (size_t h : heads) {
      NPx x;
      initNP(h, x);
      used_[h] = 1;
      npOfHead[h] = (int)local.size();
      local.push_back(std::move(x));
    }
    // modifiers: the nearest agreeing head in the clause; else a substantive / predicate of its own
    for (size_t m : mods) {
      int best = -1;
      int bestDist = 1 << 20;
      const bool detMod = isDetKey(ti_[m].key);
      for (size_t h : heads) {
        if (!agree(m, h)) continue;
        if (ti_[m].f.case_ == 0 || ti_[h].f.case_ == 0) continue;
        int d = (int)h > (int)m ? (int)h - (int)m : (int)m - (int)h;
        // a head takes one determiner ("hic puer meus fīlius": meus goes with fīlius)
        if (detMod && npOfHead[h] >= 0 && !local[(size_t)npOfHead[h]].np.determiner.empty()) d += 10;
        // a verb between them weakens the link (predicate adjective after the verb)
        if (d < bestDist) { bestDist = d; best = (int)h; }
      }
      if (best >= 0 && bestDist <= 4) {
        addMod(local[(size_t)npOfHead[(size_t)best]], m, intensOf);
      } else if (isDetKey(ti_[m].key) || ti_[m].lpos == Pron) {
        // a demonstrative / quantifier standing alone: a pronoun ("ille" he, "omnēs" everyone, "is" he)
        NPx x;
        initNP(m, x);
        used_[m] = 1;
        x.np.pronLemma = ti_[m].key;
        if (ti_[m].key == "is") {
          x.np.isPronoun = true;
          x.np.pron.person = 3;
          x.np.pron.number = ti_[m].f.number == Pl ? 2 : 1;
          x.np.pron.gender = genderOf(ti_[m]);
        } else {
          x.np.isPronoun = false;
          x.np.determiner = ti_[m].key;
          x.np.head.clear();
        }
        local.push_back(std::move(x));
      } else {
        NPx x;
        initNP(m, x);
        x.adjOnly = true;
        used_[m] = 1;
        // the token is the head; as an adjective it keeps its degree and intensifier
        frame::SemAdj a;
        a.lemma = ti_[m].key;
        a.token = (int)m;
        a.degree = ti_[m].f.degree == Comparative ? Comparative : ti_[m].f.degree == Superlative ? Superlative : 0;
        if (intensOf[m] >= 0) { a.adverbs.push_back(ti_[(size_t)intensOf[m]].key); a.advTokens.push_back(intensOf[m]); }
        x.np.adjectives.push_back(a);
        local.push_back(std::move(x));
      }
    }
    // interrogative determiners without case (quot): the next head
    for (size_t i : toks) {
      if (!isFree(i) || !(isKey(ti_[i].key, {"quot", "totidem"}) || ((ti_[i].lpos == Num || ti_[i].f.pos == Num) && tab_.has("det", ti_[i].key))))
        continue;
      for (size_t k = 0; k < local.size(); ++k)
        if (local[k].first == (int)i + 1 && !local[k].adjOnly) {
          if (ti_[i].key == "quot" || ti_[i].key == "totidem") {
            local[k].np.determiner = ti_[i].key;
            out_.roles[i] = "question word";
          } else {
            local[k].np.numeral = ti_[i].key;
            out_.roles[i] = "numeral";
          }
          local[k].np.tokens.push_back((int)i);
          local[k].first = (int)i;
          used_[i] = 1;
          break;
        }
    }
    // genitives attach to the nearest non-genitive head (the preceding one first)
    for (size_t k = 0; k < local.size(); ++k) {
      NPx& g = local[k];
      if (g.case_ != Gen || g.used) continue;
      int best = -1, bestD = 1 << 20;
      for (size_t q = 0; q < local.size(); ++q) {
        if (q == k || local[q].used || local[q].case_ == Gen || local[q].adjOnly || local[q].np.isName) continue;
        const int d = g.first > local[q].last ? g.first - local[q].last : local[q].first - g.last;
        const int pen = g.first > local[q].last ? 0 : 1;   // prefer the head before the genitive
        if (d * 2 + pen < bestD) { bestD = d * 2 + pen; best = (int)q; }
      }
      for (auto& pp : pps) {
        const int d = g.first > pp.second.last ? g.first - pp.second.last : 99;
        if (d * 2 < bestD) { bestD = d * 2; best = -2 - (int)(&pp - &pps[0]); }
      }
      if (best >= 0) {
        if (g.np.isPronoun && isKey(g.np.pronLemma, {"is", "ego", "tu", "nos", "uos"})) local[(size_t)best].np.possessor.push_back(g.np);
        else local[(size_t)best].np.genitive.push_back(g.np);
        local[(size_t)best].np.tokens.insert(local[(size_t)best].np.tokens.end(), g.np.tokens.begin(), g.np.tokens.end());
        g.used = true;
      } else if (best <= -2) {
        NPx& h = pps[(size_t)(-2 - best)].second;
        if (g.np.isPronoun && isKey(g.np.pronLemma, {"is", "ego", "tu", "nos", "uos"})) h.np.possessor.push_back(g.np);
        else h.np.genitive.push_back(g.np);
        g.used = true;
      }
    }
    // coordination: NPs of one case joined by et / atque / -que / aut
    std::vector<size_t> order;
    for (size_t k = 0; k < local.size(); ++k)
      if (!local[k].used) order.push_back(k);
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return local[a].first < local[b].first; });
    for (size_t a = 0; a + 1 < order.size(); ++a) {
      NPx& x = local[order[a]];
      if (x.used) continue;
      for (size_t b = a + 1; b < order.size(); ++b) {
        NPx& y = local[order[b]];
        if (y.used) continue;
        if (y.case_ != x.case_ || x.case_ == 0) break;
        // what is between: exactly one coordinator, or y carries -que
        std::string conj;
        int between = 0;
        for (int t = x.last + 1; t < y.first; ++t) {
          if (!ti_[(size_t)t].word) continue;
          ++between;
          if (ti_[(size_t)t].lpos == Conj && isKey(ti_[(size_t)t].key, {"et", "atque", "ac", "aut", "uel", "neque", "nec"}))
            conj = ti_[(size_t)t].key;
        }
        if (s_.tokens[(size_t)y.first].encl == "que" && between == 0) conj = "et";
        if (s_.tokens[(size_t)y.first].encl == "ue" && between == 0) conj = "aut";
        if (conj.empty() || between > 1) break;
        for (int t = x.last + 1; t < y.first; ++t)
          if (ti_[(size_t)t].word && ti_[(size_t)t].lpos == Conj) { used_[(size_t)t] = 1; out_.roles[(size_t)t] = "and"; }
        x.np.coord.push_back(y.np);
        x.np.coordConj = conj;
        x.np.tokens.insert(x.np.tokens.end(), y.np.tokens.begin(), y.np.tokens.end());
        x.last = y.last;
        x.number = Pl;
        y.used = true;
      }
    }
    for (NPx& x : local)
      if (!x.used) nps.push_back(std::move(x));
    for (auto& pp : pps) {
      NPx x = std::move(pp.second);
      x.np.determiner = x.np.determiner;   // unchanged
      x.governed = true;
      x.np.wh = std::string();
      // keep the preposition token in tokens[0] position via a marker in `surface`? no: store separately
      x.np.tokens.insert(x.np.tokens.begin(), (int)pp.first);
      x.first = std::min(x.first, (int)pp.first);
      nps.push_back(std::move(x));
      nps.back().np.pronLemma = nps.back().np.pronLemma;
    }
    std::sort(nps.begin(), nps.end(), [](const NPx& a, const NPx& b) { return a.first < b.first; });
  }

  // ---- roles -------------------------------------------------------------------------------------------------------------
  void assignRoles(int ci, SemFrame& f, int verb, std::vector<NPx>& nps, int quamComp) {
    (void)ci;
    const TokInfo* v = verb >= 0 ? &ti_[(size_t)verb] : nullptr;
    const uint8_t vnum = v ? v->f.number : 0, vper = v ? v->f.person : 0;
    const bool copula = f.copula;
    // vocatives
    for (NPx& x : nps) {
      if (x.used || x.governed) continue;
      if (x.case_ == Voc || (x.np.isName && x.case_ == 0 && f.type == Kind::Imp)) {
        f.vocatives.push_back(x.np);
        x.used = true;
        markRole(x, "vocative");
      }
    }
    // after a comparative + quam: "than X" (same case as the compared word)
    if (quamComp >= 0)
      for (NPx& x : nps) {
        if (x.used || x.governed || x.first <= quamComp) continue;
        frame::SemOblique o;
        o.prep = "#than";
        o.np = x.np;
        o.token = x.np.token;
        f.obliques.push_back(o);
        x.used = true;
        markRole(x, "comparison");
        break;
      }
    // nominatives: subject (agreeing with the verb), predicate
    std::vector<NPx*> noms;
    for (NPx& x : nps)
      if (!x.used && !x.governed && (x.case_ == Nom || (x.case_ == 0 && x.np.isName))) noms.push_back(&x);
    NPx* subj = nullptr;
    auto personOf = [](const NPx& x) { return x.np.isPronoun ? x.np.pron.person : 3; };
    auto numberOf = [](const NPx& x) -> uint8_t { return x.np.coord.empty() ? (x.number ? x.number : (uint8_t)Sg) : (uint8_t)Pl; };
    auto interrogativeNP = [&](const NPx* x) {
      return x->np.isPronoun && isKey(x->np.pronLemma, {"quis", "quid"});
    };
    bool otherNom = false;
    for (NPx* x : noms) otherNom = otherNom || (!x->adjOnly && !interrogativeNP(x));
    if (v && f.type != Kind::Imp) {
      for (NPx* x : noms) {
        if (x->adjOnly && copula) continue;
        if (copula && otherNom && interrogativeNP(x)) continue;   // "Quid est nōmen?": nōmen is the subject
        if (personOf(*x) == vper && (numberOf(*x) == vnum || x->number == 0)) { subj = x; break; }
      }
      if (!subj)
        for (NPx* x : noms)
          if (!x->adjOnly && personOf(*x) == vper) { subj = x; break; }
      if (!subj && !copula)
        for (NPx* x : noms)
          if (personOf(*x) == vper) { subj = x; break; }
    } else if (!v) {
      // fragment: the first nominative is the "subject" of a verbless exclamation / answer
      for (NPx* x : noms) { subj = x; break; }
    }
    if (subj) {
      f.hasSubject = true;
      f.subject = subj->np;
      subj->used = true;
      markRole(*subj, "subject");
    } else if (v && f.type != Kind::Imp) {
      // subject restored from the verb's person and number
      f.hasSubject = true;
      f.implicitSubject = true;
      f.subject = SemNP();
      f.subject.isPronoun = true;
      f.subject.pron.person = vper ? vper : 3;
      f.subject.pron.number = vnum == Pl ? 2 : 1;
      f.subject.number = vnum == Pl ? 2 : 1;
      if (isKey(v->key, {"pluo", "pluit", "ningit", "tonat", "fulget", "ningo", "tono", "fulgeo", "fulmino", "grandino", "licet", "oportet", "decet", "paenitet",
                         "piget", "pudet", "taedet", "lucesco", "aduesperascit", "aduesperasco"}))
        f.subject.pron.gender = N;   // "it rains"
      // a predicate adjective / participle gives the gender ("laeta sum", "amātus est")
      for (NPx* x : noms)
        if (x->adjOnly && x->gender && x->gender != MFN) { f.subject.pron.gender = x->gender; break; }
      if (f.pred.complementToken >= 0 && f.pred.particle == "peri") f.subject.pron.gender = genderOf(ti_[(size_t)f.pred.complementToken]);
    }
    // copula predicate (nominatives left) / extra nominatives
    for (NPx* x : noms) {
      if (x->used) continue;
      if (x->adjOnly) {
        for (const frame::SemAdj& a : x->np.adjectives) f.predAdj.push_back(a);
        if (x->np.adjectives.empty()) f.predicative.push_back(x->np);
        if (copula || !v) markRole(*x, "predicate");
        else markRole(*x, "predicate");
        // participle predicate adjectives carry their gender into the subject
        if (f.hasSubject && f.implicitSubject && !f.subject.pron.gender) f.subject.pron.gender = x->gender;
      } else {
        f.predicative.push_back(x->np);
        markRole(*x, "predicate");
      }
      x->used = true;
    }
    // floating omnēs / tōtī with a 1st / 2nd person verb ("Omnēs inurbānī estis": you are all rude)
    for (size_t k = 0; k < f.predicative.size(); ++k) {
      const SemNP& p = f.predicative[k];
      if (p.head.empty() && isKey(p.determiner, {"omnis", "totus"}) && vper && vper != 3) {
        f.discourse.push_back("all");
        f.predicative.erase(f.predicative.begin() + (long)k);
        break;
      }
    }
    // existential: the copula before its subject, or a subject without a predicate ("erat puella parva",
    // "nūlla thēa est" -> there was a little girl / there is no tea)
    const bool verbFirst = f.hasSubject && !f.implicitSubject && f.subject.token >= 0 && verb >= 0 &&
                           verb < f.subject.token && f.type == Kind::Decl;
    if (copula && f.hasSubject && !f.implicitSubject && !f.subject.isPronoun && !f.subject.isName &&
        f.predAdj.empty() && f.predicative.empty() &&
        (verbFirst || isKey(f.subject.determiner, {"nullus", "multus", "paucus", "aliquis", "ullus", "nemo"}))) {
      bool place = false;
      for (const frame::SemAdverb& a : f.adverbs) place = place || isKey(a.lemma, {"hic", "ibi"});
      if (!place) f.existential = true;
    }
    if (copula && f.predAdj.empty() && f.predicative.empty() && (!f.hasSubject || f.implicitSubject)) {
      for (std::string& d : f.discourse)
        if (d.compare(0, 7, "phrase:") == 0) {
          std::string t = d.substr(7);
          const size_t bar = t.find("|end");
          if (bar != std::string::npos) t = t.substr(0, bar);
          d = "phrase-pred:" + t;
          f.subject.pron.gender = N;   // "it is ..."
          break;
        }
    }
    // copula without a predicate yet: the subject's last adjective is the predicate ("Iūlia laeta est")
    if (copula && !f.existential && f.predAdj.empty() && f.predicative.empty() && f.hasSubject && !f.subject.adjectives.empty()) {
      f.predAdj.push_back(f.subject.adjectives.back());
      f.subject.adjectives.pop_back();
      out_.roles[(size_t)f.predAdj.back().token] = "predicate";
    }
    if (!f.predAdj.empty() || !f.predicative.empty()) {
      if (!copula && v) f.discourse.push_back("secondary-predicate");
    }
    // accusative of place (towns) with a verb of motion: "Rōmam īmus" -> to Rome
    const bool motion = v && isKey(v->key, {"eo", "uenio", "redeo", "curro", "ambulo", "nauigo", "festino", "propero",
                                            "fugio", "abeo", "adeo", "peruenio", "mitto", "duco", "proficiscor"});
    for (NPx& x : nps) {
      if (x.used || x.governed || x.case_ != Acc || !motion || !x.np.isName) continue;
      frame::SemOblique o;
      o.prep = "ad:acc";
      o.np = x.np;
      o.token = -1;
      f.obliques.push_back(o);
      x.used = true;
      markRole(x, "place (to)");
    }
    // accusative object(s)
    for (NPx& x : nps) {
      if (x.used || x.governed || x.case_ != Acc) continue;
      if (!f.hasObject) {
        f.hasObject = true;
        f.object = x.np;
        markRole(x, "object");
      } else {
        // a second accusative: double object / time / coordination without a conjunction
        frame::SemOblique o;
        o.prep = "#acc";
        o.np = x.np;
        o.token = x.np.token;
        f.obliques.push_back(o);
        markRole(x, "object");
      }
      x.used = true;
    }
    // dative
    for (NPx& x : nps) {
      if (x.used || x.governed || x.case_ != Dat) continue;
      if (!f.hasIndirect) {
        f.hasIndirect = true;
        f.indirectObject = x.np;
        markRole(x, "indirect object");
      } else {
        frame::SemOblique o;
        o.prep = "#dat";
        o.np = x.np;
        o.token = x.np.token;
        f.obliques.push_back(o);
        markRole(x, "indirect object");
      }
      x.used = true;
    }
    // dative of possession: "nōmen mihi est" -> my name is
    if (copula && f.hasIndirect && f.indirectObject.isPronoun && f.indirectObject.coord.empty() && f.hasSubject &&
        !f.subject.isPronoun && f.subject.possessor.empty() && f.subject.determiner.empty()) {
      f.subject.possessor.push_back(f.indirectObject);
      f.hasIndirect = false;
    }
    // prepositional phrases, ablatives, locatives, genitives left, "than" after a comparative
    for (NPx& x : nps) {
      if (x.used) continue;
      frame::SemOblique o;
      o.np = x.np;
      o.token = x.np.token;
      if (x.governed) {
        const int pt = x.np.tokens.empty() ? -1 : x.np.tokens.front();
        o.prep = pt >= 0 ? ti_[(size_t)pt].key : std::string();
        o.np.tokens.erase(o.np.tokens.begin());
        o.token = pt;
        // the case the preposition took, for "in" + acc / abl
        o.prep += x.case_ == Acc ? ":acc" : x.case_ == Abl ? ":abl" : x.case_ == Gen ? ":gen" : "";
        markRole(x, "prepositional phrase");
      } else if (quamComp >= 0 && x.first > quamComp) {
        o.prep = "#than";
        markRole(x, "comparison");
      } else if (x.case_ == Abl) {
        o.prep = "#abl";
        markRole(x, "ablative");
      } else if (x.case_ == Loc) {
        o.prep = "#loc";
        markRole(x, "place");
      } else if (x.case_ == Gen) {
        o.prep = "#gen";
        markRole(x, "genitive");
      } else {
        o.prep = "#" + std::to_string((int)x.case_);
        markRole(x, "other");
      }
      o.front = verb >= 0 && x.first < verb && x.first == firstWordOf(f, x.first);
      f.obliques.push_back(o);
      x.used = true;
    }
  }
  int firstWordOf(const SemFrame&, int t) const { return t; }

  // relative clause: attach to the antecedent NP in `f` (searching subject, object, IO, obliques, predicatives)
  bool attachRelative(SemFrame& f, const Clause& K, SemFrame& sf) {
    if (K.marker < 0) return false;
    const TokInfo& rel = ti_[(size_t)K.marker];
    // the relative pronoun's role inside its clause: it is one of sf's NPs (subject/object/...) with key qui
    Role role = Role::None;
    auto isRel = [&](const SemNP& np) { return np.token == K.marker; };
    if (sf.hasSubject && isRel(sf.subject)) { role = Role::Subject; sf.hasSubject = false; }
    else if (sf.hasObject && isRel(sf.object)) { role = Role::Object; sf.hasObject = false; }
    else if (sf.hasIndirect && isRel(sf.indirectObject)) { role = Role::IndirectObject; sf.hasIndirect = false; }
    else {
      for (size_t k = 0; k < sf.obliques.size(); ++k)
        if (sf.obliques[k].np.token == K.marker) {
          role = Role::Oblique;
          sf.wh.word = sf.obliques[k].prep;   // "in:abl" -> "in which"
          sf.obliques.erase(sf.obliques.begin() + (long)k);
          break;
        }
    }
    if (role == Role::None && rel.f.case_ == Nom) role = Role::Subject;
    if (role == Role::None && rel.f.case_ == Acc) role = Role::Object;
    if (role == Role::Subject && sf.implicitSubject) sf.hasSubject = false;
    if (role == Role::Subject) {
      sf.hasSubject = false;
      sf.implicitSubject = false;
    }
    if (role == Role::Object && sf.hasSubject && sf.implicitSubject && false) {}
    sf.wh.role = role;
    sf.wh.token = K.marker;
    if (sf.wh.word.empty()) sf.wh.word = "qui";
    sf.type = Kind::Decl;
    // antecedent: the nearest head token before the relative clause
    int ante = -1;
    for (int i = K.first - 1; i >= 0; --i) {
      if (!ti_[(size_t)i].word) continue;
      if (isNominal(ti_[(size_t)i]) && isHeadTok(ti_[(size_t)i])) { ante = i; break; }
      if (isNominal(ti_[(size_t)i])) continue;
      if (ti_[(size_t)i].lpos == Prep) continue;
      break;
    }
    if (ante < 0) return false;
    std::function<bool(SemNP&)> put = [&](SemNP& np) -> bool {
      if (np.token == ante) {
        np.relative.push_back(sf);
        return true;
      }
      for (SemNP& c : np.coord)
        if (put(c)) return true;
      for (SemNP& g : np.genitive)
        if (put(g)) return true;
      return false;
    };
    if (f.hasSubject && put(f.subject)) return true;
    if (f.hasObject && put(f.object)) return true;
    if (f.hasIndirect && put(f.indirectObject)) return true;
    for (frame::SemOblique& o : f.obliques)
      if (put(o.np)) return true;
    for (SemNP& p : f.predicative)
      if (put(p)) return true;
    for (SemNP& p : f.vocatives)
      if (put(p)) return true;
    return false;
  }
};

}  // namespace

void buildFrames(const lex::Lexicon& la, const Tables& tab, const Sentence& s, const std::vector<TokInfo>& ti,
                 Built& out) {
  out.frames.clear();
  out.joiners.clear();
  out.flags.clear();
  out.roles.clear();
  Builder b(la, tab, s, ti, out);
  b.run();
}

}  // namespace vp::la2x::detail
