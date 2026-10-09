// grc2x readable sentence: SemFrames filled from the Greek roles (buildFrames) and the plain English / es-MX Spanish
// realisers over them. English / Spanish word morphology comes from la2x's helpers (la2x/internal.h: en::verb,
// en::plural, es::verb, es::adjective ...), so both back-translation paths inflect the same way.
#include <algorithm>

#include "grc2x/internal.h"
#include "la2x/internal.h"
#include "vp/morph_grc.h"
#include "vp/text.h"

namespace vp::grc2x::detail {

using namespace vp::feat;
using frame::Aspect;
using frame::Kind;
using frame::Modality;
using frame::Relation;
using frame::SemAdj;
using frame::SemFrame;
using frame::SemNP;
using frame::Tense;
namespace en = vp::la2x::detail::en;
namespace es = vp::la2x::detail::es;

namespace {

bool isFinite(const Features& f) {
  return f.person && (f.mood == Indicative || f.mood == Subjunctive || f.mood == Imperative || f.mood == Optative);
}
bool nominal(const TokInfo& t) {
  if (!t.word || !t.f.case_ || t.f.mood == Infinitive) return false;
  return t.lpos == Noun || t.lpos == Adj || t.lpos == Pron || t.lpos == Det || t.lpos == Num || t.lpos == Name ||
         t.lpos == Article || t.lpos == Participle || t.f.mood == ParticipleMood || t.name;
}
std::string bareKey(const std::string& w) { return text::greek_key(grc::ultimaToAcute(w)); }
bool keyIs(const TokInfo& t, std::initializer_list<const char*> ks) {
  const std::string k = bareKey(t.text);
  for (const char* x : ks)
    if (k == text::greek_key(x) || t.key == text::greek_key(x)) return true;
  return false;
}
bool negWord(const TokInfo& t) { return keyIs(t, {"οὐ", "οὐκ", "οὐχ", "οὔ", "μή"}); }
bool interrogativePron(const TokInfo& t) {   // τίς / τί with its own accent (not the enclitic τις)
  if (t.key != "τίσ") return false;
  const grc::AccentInfo a = grc::accentOf(t.text);
  return a.accents > 0;
}
std::string lowerFirst(const std::string& s) { return s; }

struct NPInfo { SemNP np; uint8_t case_ = 0; bool article = false; int first = 0, last = 0; bool adjOnly = false; };

}  // namespace

// ---- frame fill ------------------------------------------------------------------------------------------------------
void buildFrames(const lex::Lexicon& lx, const Sentence& s, const std::vector<TokInfo>& ti, const LexicalSource& src,
                 Target tg, Side& side, Built& out) {
  (void)lx;
  const bool esT = tg == Target::Es;
  const size_t n = ti.size();
  side.ti = ti;
  side.lex.assign(n, Lexical{});
  out.roles.assign(n, std::string());
  for (size_t i = 0; i < n; ++i) {
    const TokInfo& t = ti[i];
    if (!t.word) continue;
    if (t.name) {
      side.lex[i].word = esT && !t.nameEs.empty() ? t.nameEs : t.nameEn.empty() ? t.text : t.nameEn;
      side.lex[i].person = !t.place;   // C29: a city is not a person ("a Atenas" only after a verb of motion)
      continue;
    }
    if (t.lemma == kNone) continue;
    const bool middle = t.f.voice == Middle || t.f.voice == Passive;
    side.lex[i] = src.lexical(t.lemma, t.lpos, middle, tg);
  }
  out.question = s.question;
  out.exclamation = s.finalPunct.find('!') != std::string::npos;
  int maxClause = 0;
  for (const Token& t : s.tokens) maxClause = std::max(maxClause, t.clause);
  struct Pending { SemFrame f; bool sub = false; Relation rel = Relation::Cause; std::string marker; bool hasContent = false;
                   bool onlyVocative = false; std::string sepAfter; };
  std::vector<Pending> clauses;
  for (int cl = 0; cl <= maxClause; ++cl) {
    std::vector<size_t> idx;
    std::string sepAfter;
    for (size_t i = 0; i < n; ++i) {
      if (s.tokens[i].clause != cl) continue;
      if (ti[i].word) idx.push_back(i);
      else if (ti[i].punct) sepAfter = ti[i].text;
    }
    if (idx.empty()) continue;
    // a postpositive particle after the first word ("ὁ δὲ δοῦλος", "ἐν δὲ τῷ ἀγρῷ") is read before it
    for (size_t a = 0; a + 1 < idx.size(); ++a)
      if ((ti[idx[a]].lpos == Article || ti[idx[a]].lpos == Prep) &&
          keyIs(ti[idx[a + 1]], {"δέ", "γάρ", "οὖν", "μέν", "γε", "δή"})) {
        std::swap(idx[a], idx[a + 1]);
        break;
      }
    Pending p;
    p.sepAfter = sepAfter;
    SemFrame& f = p.f;
    f.punct = "";
    std::vector<NPInfo> nps;
    std::vector<std::pair<std::string, NPInfo>> obl;   // prep word, NP
    size_t finite = n, infinitive = n;
    bool ara = false, me = false, hos = false, frontSet = false;
    bool sawContent = false;
    size_t k = 0;
    auto lexOf = [&](size_t i) -> const Lexical& { return side.lex[i]; };
    // NP starting at idx[k]
    auto readNP = [&](size_t& kk, NPInfo& np) -> bool {
      if (kk >= idx.size()) return false;
      size_t i = idx[kk];
      if (!(nominal(ti[i]) || ti[i].lpos == Article)) return false;
      np = NPInfo{};
      np.first = (int)i;
      uint8_t cs = ti[i].f.case_;
      uint8_t num = ti[i].f.number;
      std::vector<size_t> members;
      std::vector<std::vector<size_t>> advs;   // degree adverbs before each member
      std::vector<size_t> pendingAdv;
      while (kk < idx.size()) {
        const size_t j = idx[kk];
        const TokInfo& t = ti[j];
        if ((t.lpos == Adv || t.lpos == Particle) && kk + 1 < idx.size() && nominal(ti[idx[kk + 1]]) &&
            ti[idx[kk + 1]].f.case_ == cs && !members.empty() && ti[idx[kk + 1]].lpos == Adj) {
          pendingAdv.push_back(j);
          ++kk;
          continue;
        }
        if (!(nominal(t) || t.lpos == Article)) break;
        if (t.f.case_ != cs) break;
        // predicate position: an adjective after the noun of an articled group without its own article
        // ("ἡ οἰκία μικρά ἐστιν" = the house is small)
        if (!members.empty() && (t.lpos == Adj || t.lpos == Participle) && ti[members.back()].lpos != Article) {
          bool art = false, headSeen = false;
          for (size_t m : members) { art = art || ti[m].lpos == Article; headSeen = headSeen || ti[m].lpos == Noun || ti[m].lpos == Name; }
          if (art && headSeen && t.key != "πᾶσ" && t.key != "οὗτοσ" && t.key != "ἐκεῖνοσ") break;
        }
        if (num && t.f.number && t.f.number != num && t.lpos != Article) break;
        // a second article starts a new NP unless the group's head is not reached yet ("ὁ παῖς ὁ καλός")
        if (t.lpos == Article && !members.empty()) {
          bool headSeen = false;
          for (size_t m : members) headSeen = headSeen || ti[m].lpos == Noun || ti[m].lpos == Name || ti[m].lpos == Pron;
          if (headSeen && !(kk + 1 < idx.size() && ti[idx[kk + 1]].lpos == Adj && ti[idx[kk + 1]].f.case_ == cs))
            break;
        }
        // a personal pronoun is a group of its own (enclitic possessors are attached below)
        if (!members.empty() && t.closed && t.lpos == Pron && t.key != "οὗτοσ" && t.key != "ἐκεῖνοσ" && t.key != "πᾶσ")
          break;
        if (!members.empty() && (ti[members.back()].lpos == Pron && ti[members.back()].closed &&
                                 ti[members.back()].key != "οὗτοσ" && ti[members.back()].key != "πᾶσ" &&
                                 ti[members.back()].key != "ἐκεῖνοσ"))
          break;
        if (!num) num = t.f.number;
        members.push_back(j);
        advs.push_back(pendingAdv);
        pendingAdv.clear();
        ++kk;
        // a name or a noun closes the group unless an agreeing adjective follows with a repeated article
      }
      if (members.empty()) return false;
      np.case_ = cs;
      np.last = (int)members.back();
      size_t head = n;
      for (size_t m : members)
        if (ti[m].lpos == Noun || ti[m].lpos == Name || ti[m].name) head = m;
      if (head == n)
        for (size_t m : members)
          if (ti[m].lpos == Pron && ti[m].key != "τίσ") head = m;
      if (head == n)
        for (size_t m : members)
          if (ti[m].lpos != Article) head = m;
      SemNP& x = np.np;
      x.number = num == Pl ? 2 : 1;
      for (size_t mi = 0; mi < members.size(); ++mi) {
        const size_t m = members[mi];
        x.tokens.push_back((int)m);
        const TokInfo& t = ti[m];
        if (t.lpos == Article) { np.article = true; x.definite = true; continue; }
        if (m == head) continue;
        if (t.key == "οὗτοσ") { x.determiner = num == Pl ? "these" : "this"; x.definite = true; continue; }
        if (t.key == "ἐκεῖνοσ") { x.determiner = num == Pl ? "those" : "that"; x.definite = true; continue; }
        if (t.key == "πᾶσ") { x.determiner = num == Pl ? "all" : "every"; continue; }
        if (t.key == "οὐδείσ" || t.key == "μηδείσ") { x.determiner = "no"; x.negative = true; continue; }
        if (t.key == "πόσοσ") { x.interrogative = true; x.wh = "how many"; continue; }
        if (t.lpos == Num || t.key == "δύο" || t.key == "τρεῖσ" || t.key == "εἷσ" || t.key == "τέτταρεσ") {
          x.numeral = lexOf(m).word;
          continue;
        }
        SemAdj a;
        a.lemma = lexOf(m).word;
        a.token = (int)m;
        for (size_t av : advs[mi]) { a.adverbs.push_back(lexOf(av).word); a.advTokens.push_back((int)av); }
        if (!a.lemma.empty()) x.adjectives.push_back(a);
      }
      if (head != n) {
        const TokInfo& h = ti[head];
        x.token = (int)head;
        x.surface = h.text;
        if (h.name) { x.isName = true; x.head = lexOf(head).word; x.definite = false; }
        else if (h.closed && h.lpos == Pron && h.key != "οὗτοσ" && h.key != "ἐκεῖνοσ" && h.key != "πᾶσ" &&
                 h.key != "οὐδείσ") {
          x.isPronoun = true;
          x.pron.person = h.f.person ? h.f.person : 3;
          x.pron.number = h.f.number == Pl ? 2 : 1;
          x.pron.gender = h.f.gender;
          x.pronLemma = h.key;
          if (h.key == "τίσ" && interrogativePron(h)) { x.interrogative = true; x.wh = h.f.gender == N ? "what" : "who"; }
          if (h.key == "τισ") { x.isPronoun = true; x.pronLemma = h.f.gender == N ? "something" : "someone"; }
        } else if (h.key == "οὗτοσ" || h.key == "ἐκεῖνοσ") {
          x.isPronoun = true;
          x.pron.person = 3;
          x.pron.number = h.f.number == Pl ? 2 : 1;
          x.pron.gender = h.f.gender;
          x.pronLemma = h.key == "οὗτοσ" ? (h.f.number == Pl ? "these" : "this") : (h.f.number == Pl ? "those" : "that");
        } else if (h.key == "πᾶσ") {
          x.isPronoun = true;
          x.pron.person = 3;
          x.pron.number = 2;
          x.pronLemma = h.f.gender == N ? "everything" : "everyone";
          x.number = h.f.gender == N ? 1 : 2;
        } else if (h.key == "οὐδείσ" || h.key == "μηδείσ") {
          x.isPronoun = true;
          x.pron.person = 3;
          x.pronLemma = h.f.gender == N ? "nothing" : "nobody";
          x.negative = true;
        } else {
          x.head = lexOf(head).word;
          x.srcGender = lexOf(head).gender;
          if (h.lpos == Adj && np.article && x.head.empty()) x.head = lexOf(head).word;
          np.adjOnly = h.lpos == Adj || h.lpos == Participle || h.f.mood == ParticipleMood;
          if (np.adjOnly && !np.article) {   // a predicate adjective, not a noun
            SemAdj a;
            a.lemma = lexOf(head).word;
            a.token = (int)head;
            for (size_t mi = 0; mi < members.size(); ++mi)
              if (members[mi] == head)
                for (size_t av : advs[mi]) { a.adverbs.push_back(lexOf(av).word); a.advTokens.push_back((int)av); }
            x.adjectives.push_back(a);
            x.head.clear();
          }
        }
      }
      // enclitic / pronoun possessor right after ("ὁ πατήρ μου", "τὴν κεφαλὴν αὐτοῦ")
      if (kk < idx.size() && cs != Gen) {
        const TokInfo& t = ti[idx[kk]];
        if (t.closed && t.lpos == Pron && t.f.case_ == Gen && (t.key == "ἐγώ" || t.key == "σύ" || t.key == "αὐτόσ" ||
                                                               t.key == "ἡμεῖσ" || t.key == "ὑμεῖσ")) {
          SemNP po;
          po.isPronoun = true;
          po.pron.person = t.f.person ? t.f.person : 3;
          po.pron.number = t.f.number == Pl ? 2 : 1;
          po.pron.gender = t.f.gender;
          po.token = (int)idx[kk];
          po.tokens.push_back((int)idx[kk]);
          x.possessor.push_back(po);
          x.definite = false;
          np.last = (int)idx[kk];
          ++kk;
        }
      }
      return true;
    };
    // walk the clause
    while (k < idx.size()) {
      const size_t i = idx[k];
      const TokInfo& t = ti[i];
      const Lexical& lw = lexOf(i);
      // particles and connectors
      if (keyIs(t, {"ἆρα"})) { ara = true; ++k; continue; }
      if (keyIs(t, {"δέ", "γάρ", "οὖν", "ἀλλά", "δήπου"}) || (keyIs(t, {"καί"}) && !sawContent && k == 0)) {
        if (!lw.word.empty() && lw.word != "-") f.connectors.push_back(lw.word);
        out.roles[i] = "connector";
        ++k;
        continue;
      }
      if (keyIs(t, {"μέν", "γε", "δή", "τοι"})) { out.roles[i] = "particle"; ++k; continue; }
      if (negWord(t)) {
        f.negative = true;
        if (keyIs(t, {"μή"})) me = true;
        out.roles[i] = "negation";
        ++k;
        continue;
      }
      // subordinator
      if (t.lpos == Conj && keyIs(t, {"ὅτι", "ἐπεί", "ὅτε", "εἰ", "ἐάν", "ἵνα", "ὥστε", "ἐπειδή"}) && !sawContent) {
        p.sub = true;
        p.marker = lw.word;
        p.rel = keyIs(t, {"ὅτι"}) ? Relation::Cause : keyIs(t, {"εἰ", "ἐάν"}) ? Relation::Condition
              : keyIs(t, {"ἵνα"}) ? Relation::Purpose : keyIs(t, {"ὥστε"}) ? Relation::Result : Relation::Time;
        out.roles[i] = "subordinator";
        ++k;
        continue;
      }
      if (t.lpos == Conj && keyIs(t, {"καί", "ἤ"})) {   // inside a clause: coordination of NPs
        out.roles[i] = "conjunction";
        ++k;
        continue;
      }
      // interjections and ὦ
      if (keyIs(t, {"ὦ"})) { ++k; continue; }
      if (t.lpos == Intj) {
        if (!lw.word.empty()) f.interjections.push_back(lw.word);
        out.roles[i] = "interjection";
        ++k;
        continue;
      }
      if (keyIs(t, {"ὡς"}) && !sawContent && out.exclamation) { hos = true; ++k; continue; }
      // wh adverbs; διὰ τί
      if (keyIs(t, {"ποῦ", "ποῖ", "πῶς", "πότε", "πόθεν", "πηνίκα"})) {
        f.type = Kind::Wh;
        const std::string k2 = bareKey(t.text);
        f.wh.word = k2 == text::greek_key("πῶς") ? "how" : k2 == text::greek_key("πότε") ? "when"
                  : k2 == text::greek_key("πόθεν") ? "where from" : k2 == text::greek_key("πηνίκα") ? "what time" : "where";
        f.wh.token = (int)i;
        out.roles[i] = "wh";
        sawContent = true;
        ++k;
        continue;
      }
      if (t.lpos == Prep) {
        if (k + 1 < idx.size() && interrogativePron(ti[idx[k + 1]]) && keyIs(t, {"διά"})) {
          f.type = Kind::Wh;
          f.wh.word = "why";
          f.wh.token = (int)i;
          out.roles[i] = out.roles[idx[k + 1]] = "wh";
          k += 2;
          sawContent = true;
          continue;
        }
        size_t kk = k + 1;
        NPInfo np;
        if (readNP(kk, np)) {
          std::string w = lw.word;
          // case-dependent senses
          if (keyIs(t, {"μετά"})) w = np.case_ == Acc ? (esT ? "después de" : "after") : (esT ? "con" : "with");
          else if (keyIs(t, {"παρά"})) w = np.case_ == Gen ? (esT ? "de" : "from") : np.case_ == Acc ? (esT ? "a" : "to") : (esT ? "junto a" : "beside");
          else if (keyIs(t, {"διά"})) w = np.case_ == Acc ? (esT ? "por" : "because of") : (esT ? "por" : "through");
          else if (keyIs(t, {"ἐπί"})) w = np.case_ == Acc ? (esT ? "contra" : "against") : (esT ? "sobre" : "on");
          else if (keyIs(t, {"πρός"})) w = np.case_ == Acc ? (esT ? "hacia" : "to") : (esT ? "junto a" : "near");
          else if (keyIs(t, {"ὑπό"})) w = np.case_ == Gen ? (esT ? "por" : "by") : (esT ? "bajo" : "under");
          if (w.empty()) w = esT ? "en" : "in";
          out.roles[i] = "preposition";
          for (int tk : np.np.tokens) out.roles[(size_t)tk] = "oblique";
          frame::SemOblique o;
          o.prep = w;
          o.np = np.np;
          o.token = (int)i;
          o.front = !sawContent;
          f.obliques.push_back(o);
          k = kk;
          sawContent = true;
          continue;
        }
        ++k;
        continue;
      }
      if (t.lpos == Adv || t.lpos == Particle || (t.lpos == Participle && !t.f.case_)) {
        if (!lw.word.empty() && lw.word != "-") {
          frame::SemAdverb a;
          a.lemma = lw.word;
          a.token = (int)i;
          a.front = !sawContent && !frontSet;
          f.adverbs.push_back(a);
        }
        out.roles[i] = "adverb";
        ++k;
        continue;
      }
      if (t.f.mood == Infinitive) { if (infinitive == n) infinitive = i; out.roles[i] = "infinitive"; ++k; sawContent = true; continue; }
      if (isFinite(t.f) && t.lpos == Verb) {
        if (finite == n) finite = i;
        out.roles[i] = "verb";
        ++k;
        sawContent = true;
        continue;
      }
      NPInfo np;
      size_t kk = k;
      if (readNP(kk, np)) {
        nps.push_back(np);
        k = kk;
        sawContent = true;
        continue;
      }
      ++k;   // anything else is skipped (it stays in the interlinear view)
    }
    // ---- roles from the cases ----
    const TokInfo* vt = finite < n ? &ti[finite] : nullptr;
    const std::string vkey = vt ? vt->key : std::string();
    const bool eimi = vkey == "εἰμί";
    // genitive NP right after another NP: an attribute ("ὁ οἶκος τοῦ γεωργοῦ"), unless the verb takes a genitive
    bool genVerb = false;
    for (const char* g : {"ἀκούω", "ἄρχω", "ἅπτω", "ἐπιθυμέω", "μέμνημαι", "δέομαι"})
      genVerb = genVerb || vkey == text::greek_key(g);
    std::vector<NPInfo> merged;
    for (size_t a = 0; a < nps.size(); ++a) {
      if (nps[a].case_ == Gen && !merged.empty() && merged.back().case_ != Gen && merged.back().last + 1 == nps[a].first &&
          !(genVerb && merged.back().case_ == Nom)) {
        merged.back().np.genitive.push_back(nps[a].np);
        merged.back().last = nps[a].last;
        continue;
      }
      merged.push_back(nps[a]);
    }
    bool dativeObject = false;
    for (const char* g : {"ἕπομαι", "πείθομαι", "βοηθέω", "πιστεύω", "χράομαι", "ἀποκρίνομαι", "μάχομαι"})
      dativeObject = dativeObject || vkey == text::greek_key(g);
    {
      // neuter nominative / accusative: an object when the verb cannot take it as its subject (imperative, 1st or
      // 2nd person, or another nominative subject is there)
      size_t noms = 0;
      for (const NPInfo& np : merged) noms += np.case_ == Nom;
      for (NPInfo& np : merged) {
        if (np.case_ != Nom || np.np.token < 0 || eimi) continue;
        const TokInfo& h = ti[(size_t)np.np.token];
        const uint8_t hg = h.f.gender ? h.f.gender : h.lgender;
        if (hg != N || h.lpos == Pron) continue;
        const bool notThird = vt && (vt->f.mood == Imperative || (vt->f.person && vt->f.person != 3));
        if (notThird || (noms > 1 && &np != &merged.front())) np.case_ = Acc;
      }
    }
    for (NPInfo& np : merged) {
      SemNP& x = np.np;
      auto mark = [&](const char* r) { for (int tk : x.tokens) out.roles[(size_t)tk] = r; };
      if (x.interrogative && (x.wh == "who" || x.wh == "what")) {
        f.type = Kind::Wh;
        f.wh.word = x.wh;
        f.wh.token = x.token;
        if (np.case_ == Nom) {
          if (eimi) { f.wh.role = frame::Role::Predicate; mark("wh"); continue; }
          f.wh.role = frame::Role::Subject;
          f.hasSubject = true;
          f.subject = x;
          mark("subject");
          continue;
        }
        f.wh.role = frame::Role::Object;
        f.hasObject = true;
        f.object = x;
        mark("object");
        continue;
      }
      if (x.interrogative && x.wh == "how many") { f.type = Kind::Wh; f.wh.word = "how many"; f.wh.token = x.token; }
      switch (np.case_) {
        case Nom:
          if (!f.hasSubject && !(eimi && np.adjOnly && !np.article) && !(eimi && !np.article && !x.isName &&
                                                                         !x.isPronoun && f.hasSubject)) {
            // with εἰμί the NP with the article (or a pronoun / name) is the subject, the bare one the predicate
            bool otherArticled = false;
            for (const NPInfo& o : merged)
              if (&o != &np && o.case_ == Nom && (o.article || o.np.isName || o.np.isPronoun)) otherArticled = true;
            if (eimi && !np.article && !x.isName && !x.isPronoun && otherArticled) {
              f.predicative.push_back(x);
              mark("predicate");
              break;
            }
            f.hasSubject = true;
            f.subject = x;
            mark("subject");
          } else if (eimi && (np.adjOnly || !x.adjectives.empty()) && x.head.empty()) {
            for (const SemAdj& a : x.adjectives) f.predAdj.push_back(a);
            mark("predicate");
          } else {
            f.predicative.push_back(x);
            mark("predicate");
          }
          break;
        case Acc:
          if (!f.hasObject) { f.hasObject = true; f.object = x; mark("object"); }
          else { frame::SemOblique o; o.np = x; f.obliques.push_back(o); mark("oblique"); }
          break;
        case Dat:
          if (dativeObject && !f.hasObject) { f.hasObject = true; f.object = x; mark("object"); }
          else if (!f.hasIndirect) { f.hasIndirect = true; f.indirectObject = x; mark("indirect object"); }
          else { frame::SemOblique o; o.prep = esT ? "con" : "with"; o.np = x; f.obliques.push_back(o); mark("oblique"); }
          break;
        case Gen:
          if (genVerb && !f.hasObject) { f.hasObject = true; f.object = x; mark("object"); }
          else { frame::SemOblique o; o.prep = esT ? "de" : "of"; o.np = x; f.obliques.push_back(o); mark("oblique"); }
          break;
        case Voc:
          f.vocatives.push_back(x);
          mark("vocative");
          break;
        default: break;
      }
    }
    // predicate adjective with εἰμί and no explicit subject NP besides it
    if (eimi && !f.hasSubject && !f.predicative.empty() && f.predicative[0].head.empty() && !f.predicative[0].adjectives.empty()) {
      for (const SemAdj& a : f.predicative[0].adjectives) f.predAdj.push_back(a);
      f.predicative.clear();
    }
    // ---- verb ----
    p.hasContent = vt || infinitive < n || !merged.empty() || !f.obliques.empty() || !f.adverbs.empty() ||
                   !f.connectors.empty() || f.type == Kind::Wh;
    p.onlyVocative = !vt && infinitive == n && f.hasSubject == false && f.vocatives.size() == merged.size() &&
                     !f.vocatives.empty() && f.obliques.empty() && f.adverbs.empty();
    const TokInfo* main = vt;
    if (vt || infinitive < n) {
      f.hasPred = true;
      const size_t vi = vt ? finite : infinitive;
      const TokInfo& v = ti[vi];
      f.pred.token = (int)vi;
      Modality mod = Modality::None;
      if (vt && infinitive < n) {
        if (vkey == "δύναμαι") mod = Modality::Can;
        else if (vkey == "βούλομαι" || vkey == "ἐθέλω") mod = Modality::Want;
        else if (vkey == "δεῖ") mod = Modality::Must;
        else if (vkey == "ἔξεστι" || vkey == "ἔξεστιν") mod = Modality::May;
        else if (vkey == "μέλλω") mod = Modality::Will;
      }
      if (mod != Modality::None) {
        f.pred.modality = mod;
        f.pred.lemma = side.lex[infinitive].word;
        f.pred.complementToken = (int)infinitive;
        f.pred.auxTokens.push_back((int)vi);
        f.pred.token = (int)infinitive;
      } else if (vt && infinitive < n) {
        f.pred.lemma = side.lex[vi].word;
        f.pred.complementVerb = side.lex[infinitive].word;
        f.pred.complementToken = (int)infinitive;
      } else {
        f.pred.lemma = side.lex[vi].word;
      }
      if (eimi && mod == Modality::None) { f.pred.lemma = "be"; f.copula = true; }
      uint8_t tense = v.f.tense;
      if (vkey == "οἶδα") tense = tense == Pluperfect ? Imperfect : Present;   // perfect with present meaning
      switch (tense) {
        case Imperfect: f.pred.tense = Tense::Past; f.pred.aspect = Aspect::Progressive; break;
        case Aorist: f.pred.tense = Tense::Past; break;
        case Future: f.pred.tense = Tense::Future; break;
        case Perfect: f.pred.tense = Tense::Present; f.pred.aspect = Aspect::Perfect; break;
        case Pluperfect: f.pred.tense = Tense::Past; f.pred.aspect = Aspect::Perfect; break;
        default: break;
      }
      if (mod != Modality::None && tense == Imperfect) { f.pred.aspect = Aspect::Simple; f.pred.pastModal = true; }
      if (tense == Present && !eimi && mod == Modality::None && side.lex[vi].motion) f.pred.aspect = Aspect::Progressive;
      if (v.f.mood == Imperative) f.type = Kind::Imp;
      if (v.f.mood == Subjunctive && me && !p.sub) f.type = Kind::Imp;   // μή + aorist subjunctive: prohibition
      if (v.f.mood == Subjunctive && !p.sub && !me && f.type == Kind::Wh) f.pred.deliberative = true;
      if (f.type == Kind::Imp && (v.f.number == Pl)) f.imperativePlural = true;
      // the subject from the verb when there is none
      if (!f.hasSubject && f.type != Kind::Imp && !(f.type == Kind::Wh && f.wh.role == frame::Role::Subject) &&
          v.f.person && mod != Modality::Must && mod != Modality::May) {
        SemNP pr;
        pr.isPronoun = true;
        pr.pron.person = v.f.person;
        pr.pron.number = v.f.number == Pl ? 2 : 1;
        pr.number = pr.pron.number;
        pr.token = -1;
        f.hasSubject = true;
        f.subject = pr;
        f.implicitSubject = true;
      }
      if (mod == Modality::Must || mod == Modality::May) {   // δεῖ / ἔξεστι: the person is the accusative / dative
        if (f.hasObject && f.object.isPronoun) { f.hasSubject = true; f.subject = f.object; f.hasObject = false; }
        else if (f.hasIndirect && f.indirectObject.isPronoun) { f.hasSubject = true; f.subject = f.indirectObject; f.hasIndirect = false; }
        if (!f.hasSubject) {
          SemNP pr;
          pr.isPronoun = true;
          pr.pron.person = 1;
          pr.pron.number = 1;
          f.hasSubject = true;
          f.subject = pr;
          f.implicitSubject = true;
        }
      }
      // existential "ἔστι ποτόν" / "οὐκ ἔστι ποτόν": the verb before an indefinite subject without a predicate
      if (eimi && f.hasSubject && !f.implicitSubject && f.predicative.empty() && f.predAdj.empty() && f.subject.token > (int)vi &&
          !f.subject.definite && !f.subject.isName && !f.subject.isPronoun && f.type != Kind::Wh)
        f.existential = true;
    } else {
      f.type = f.type == Kind::Wh ? Kind::Wh : Kind::Frag;
      if (hos) f.type = Kind::Excl;
    }
    (void)main;
    if (hos) { f.type = Kind::Excl; f.exclQuam = true; }
    if (ara) {
      if (f.type == Kind::Decl) f.type = Kind::Yn;
      if (me) { f.negative = false; f.expectYes = false; }
    } else if (out.question && f.type == Kind::Decl && cl == maxClause) {
      f.type = Kind::Yn;
    }
    if (f.type == Kind::Imp) f.hasSubject = false;
    if (f.hasPred && !p.sub && f.type == Kind::Decl && out.question && cl != maxClause) {}
    clauses.push_back(std::move(p));
  }
  // vocative-only clauses join the next clause (or the previous one)
  for (size_t a = 0; a < clauses.size(); ++a) {
    if (!clauses[a].onlyVocative) continue;
    const size_t to = a + 1 < clauses.size() ? a + 1 : (a > 0 ? a - 1 : a);
    if (to == a) continue;
    for (const SemNP& v : clauses[a].f.vocatives) clauses[to].f.vocatives.push_back(v);
    if (to < a) clauses[to].f.discourse.push_back("vocative-after");
    clauses.erase(clauses.begin() + (long)a);
    --a;
  }
  // subordinate clauses attach to the main clause before them (or after them when they come first)
  for (size_t a = 0; a < clauses.size(); ++a) {
    if (!clauses[a].sub) continue;
    frame::SemSub sb;
    sb.relation = clauses[a].rel;
    sb.marker = clauses[a].marker;
    sb.frame.push_back(clauses[a].f);
    long host = -1;
    for (size_t b = a; b-- > 0;)
      if (!clauses[b].sub) { host = (long)b; break; }
    if (host < 0)
      for (size_t b = a + 1; b < clauses.size(); ++b)
        if (!clauses[b].sub) { host = (long)b; sb.before = true; break; }
    if (host < 0) { clauses[a].sub = false; continue; }
    clauses[(size_t)host].f.subordinate.push_back(sb);
    clauses.erase(clauses.begin() + (long)a);
    --a;
  }
  for (size_t a = 0; a < clauses.size(); ++a) {
    out.frames.push_back(clauses[a].f);
    out.joiners.push_back(a == 0 ? std::string() : (clauses[a - 1].sepAfter == "," ? "," : ";"));
  }
  bool anyVerb = false;
  for (const SemFrame& f : out.frames) anyVerb = anyVerb || f.hasPred;
  if (!anyVerb) out.flags.push_back("no-verb");
  (void)lowerFirst;
}

// ---- English ----------------------------------------------------------------------------------------------------------
namespace {

std::string join(const std::vector<std::string>& w) {
  std::string o;
  for (const std::string& x : w) {
    if (x.empty()) continue;
    if (!o.empty() && x != "," && x != ";") o += ' ';
    o += x;
  }
  return o;
}

const Lexical* lexAt(const Side& side, int tok) {
  return tok >= 0 && (size_t)tok < side.lex.size() ? &side.lex[(size_t)tok] : nullptr;
}

std::string enPronoun(const SemNP& n, bool object, bool possessive) {
  const int p = n.pron.person ? n.pron.person : 3;
  const bool pl = n.pron.number == 2;
  if (!n.pronLemma.empty() && n.pronLemma.find_first_of("abcdefghijklmnopqrstuvwxyz") == 0) return n.pronLemma;
  if (p == 1) return possessive ? (pl ? "our" : "my") : object ? (pl ? "us" : "me") : (pl ? "we" : "I");
  if (p == 2) return possessive ? (pl ? "your" : "your") : "you";
  if (pl) return possessive ? "their" : object ? "them" : "they";
  if (n.pron.gender == F) return possessive ? "her" : object ? "her" : "she";
  if (n.pron.gender == N) return possessive ? "its" : "it";
  return possessive ? "his" : object ? "him" : "he";
}

std::string enNP(const SemNP& n, const Side& side, bool object) {
  if (n.isPronoun) return enPronoun(n, object, false);
  if (n.isName) return n.head;
  std::vector<std::string> w;
  const Lexical* lx = lexAt(side, n.token);
  const bool pl = n.number == 2;
  std::string noun = n.head;
  if (pl && !noun.empty()) noun = lx && !lx->plural.empty() ? lx->plural : en::plural(noun);
  if (!n.possessor.empty()) w.push_back(enPronoun(n.possessor[0], false, true));
  else if (n.determiner == "all") { w.push_back("all"); if (n.definite) w.push_back("the"); }
  else if (n.determiner == "every") w.push_back("every");
  else if (n.determiner == "no") w.push_back("no");
  else if (!n.determiner.empty()) w.push_back(n.determiner);
  else if (n.interrogative && n.wh == "how many") w.push_back("how many");
  else if (n.definite) w.push_back("the");
  else if (!pl && n.numeral.empty() && !(lx && lx->mass) && !noun.empty()) {
    std::string next = !n.adjectives.empty() ? n.adjectives[0].lemma : noun;
    if (!n.adjectives.empty() && !n.adjectives[0].adverbs.empty()) next = n.adjectives[0].adverbs[0];
    w.push_back(en::indefinite(next));
  }
  if (!n.numeral.empty()) w.push_back(n.numeral);
  for (const SemAdj& a : n.adjectives) {
    for (const std::string& av : a.adverbs) w.push_back(av);
    w.push_back(a.lemma);
  }
  if (!noun.empty()) w.push_back(noun);
  else if (n.adjectives.empty()) w.push_back(pl ? "ones" : "one");
  for (const SemNP& g : n.genitive) w.push_back("of " + enNP(g, side, true));
  return join(w);
}

std::string beForm(int person, bool pl, bool past) {
  if (past) return (pl || person == 2) ? "were" : "was";
  if (pl || person == 2) return "are";
  return person == 1 ? "am" : "is";
}

struct Agr { int person = 3; bool pl = false; };

// The verb group: returns {auxiliary-or-finite, rest}; question inversion moves the first word.
std::vector<std::string> enVerb(const SemFrame& f, const Agr& a, bool question, bool& needsDo) {
  const frame::SemPredicate& p = f.pred;
  const bool third = a.person == 3 && !a.pl;
  const bool past = p.tense == Tense::Past;
  std::vector<std::string> v;
  needsDo = false;
  std::string base = p.lemma.empty() ? "do" : p.lemma;
  if (!f.hasObject) {   // "listen to", "look for" without an object: the bare verb
    for (const char* tail : {" to", " for", " at"}) {
      const size_t L = std::char_traits<char>::length(tail);
      if (base.size() > L && base.compare(base.size() - L, L, tail) == 0) base.resize(base.size() - L);
    }
  }
  if (f.type == Kind::Imp) {
    if (f.negative) { v.push_back("do"); v.push_back("not"); }
    v.push_back(base == "be" ? "be" : base);
    return v;
  }
  if (p.modality != Modality::None) {
    const char* m = nullptr;
    switch (p.modality) {
      case Modality::Can: m = (past || p.pastModal) ? "could" : "can"; break;
      case Modality::Must: m = "must"; break;
      case Modality::May: m = "may"; break;
      case Modality::Will: m = "will"; break;
      default: break;
    }
    if (m) {
      v.push_back(m);
      if (f.negative) v.push_back(std::string(m) == "can" ? "not" : "not");
      v.push_back(base);
      return v;
    }
    // want to
    if (f.negative || question) {
      v.push_back(past ? "did" : third ? "does" : "do");
      if (f.negative) v.push_back("not");
      v.push_back("want");
      needsDo = true;
    } else {
      v.push_back(past ? "wanted" : third ? "wants" : "want");
    }
    v.push_back("to");
    v.push_back(base);
    return v;
  }
  if (p.tense == Tense::Future) {
    v.push_back("will");
    if (f.negative) v.push_back("not");
    v.push_back(base);
    return v;
  }
  if (p.aspect == Aspect::Perfect) {
    v.push_back(past ? "had" : third ? "has" : "have");
    if (f.negative) v.push_back("not");
    v.push_back(base == "be" ? "been" : en::verb(base, en::VForm::PastPart));
    return v;
  }
  if (base == "be") {
    v.push_back(beForm(a.person, a.pl, past));
    if (f.negative) v.push_back("not");
    return v;
  }
  if (p.aspect == Aspect::Progressive) {
    v.push_back(beForm(a.person, a.pl, past));
    if (f.negative) v.push_back("not");
    v.push_back(en::verb(base, en::VForm::Ing));
    return v;
  }
  if (f.negative || question) {
    v.push_back(past ? "did" : third ? "does" : "do");
    if (f.negative) v.push_back("not");
    v.push_back(base);
    needsDo = true;
    return v;
  }
  v.push_back(past ? en::verb(base, en::VForm::Past) : third ? en::verb(base, en::VForm::S3) : base);
  return v;
}

std::string enClause(const SemFrame& f, const Side& side) {
  std::vector<std::string> w;
  for (const std::string& c : f.connectors) w.push_back(c);
  for (const std::string& ij : f.interjections) w.push_back(ij + ",");
  std::vector<std::string> front, end;
  for (const frame::SemAdverb& a : f.adverbs) (a.front ? front : end).push_back(a.lemma);
  for (const std::string& x : front) w.push_back(x);
  const bool vocAfter = std::find(f.discourse.begin(), f.discourse.end(), "vocative-after") != f.discourse.end();
  auto voc = [&](const SemNP& v) {
    SemNP x = v;
    x.definite = false;
    std::string np = enNP(x, side, false);
    for (const char* a : {"a ", "an "})
      if (np.compare(0, std::char_traits<char>::length(a), a) == 0) np = np.substr(std::char_traits<char>::length(a));
    return np;
  };
  if (!vocAfter)
    for (const SemNP& v : f.vocatives) w.push_back(voc(v) + ",");
  Agr a;
  if (f.hasSubject) {
    if (f.subject.isPronoun) { a.person = f.subject.pron.person ? f.subject.pron.person : 3; a.pl = f.subject.pron.number == 2; }
    else a.pl = f.subject.number == 2 || !f.subject.coord.empty();
    if (f.subject.isPronoun && (f.subject.pronLemma == "everyone" || f.subject.pronLemma == "nobody" ||
                                f.subject.pronLemma == "everything" || f.subject.pronLemma == "nothing" ||
                                f.subject.pronLemma == "this" || f.subject.pronLemma == "that" ||
                                f.subject.pronLemma == "someone" || f.subject.pronLemma == "something")) {
      a.person = 3;
      a.pl = false;
    }
  }
  const std::string subj = f.hasSubject ? enNP(f.subject, side, false) : std::string();
  // objects
  std::string obj = f.hasObject ? enNP(f.object, side, true) : std::string();
  std::string io = f.hasIndirect ? enNP(f.indirectObject, side, true) : std::string();
  const bool ioFirst = f.hasIndirect && f.indirectObject.isPronoun && f.hasObject;
  std::vector<std::string> rest;
  if (ioFirst) rest.push_back(io);
  if (!obj.empty()) rest.push_back(obj);
  if (f.hasIndirect && !ioFirst) rest.push_back(f.hasObject || f.type != Kind::Imp ? "to " + io : io);
  for (const SemNP& pn : f.predicative) rest.push_back(enNP(pn, side, false));
  {
    std::vector<std::string> adjs;
    for (const SemAdj& ad : f.predAdj) {
      std::string x;
      for (const std::string& av : ad.adverbs) x += av + " ";
      adjs.push_back(x + ad.lemma);
    }
    for (size_t i = 0; i < adjs.size(); ++i) {
      if (i) rest.push_back("and");
      rest.push_back(adjs[i]);
    }
  }
  for (const frame::SemOblique& o : f.obliques) rest.push_back((o.prep.empty() ? "" : o.prep + " ") + enNP(o.np, side, true));
  for (const std::string& x : end) rest.push_back(x);
  if (vocAfter)
    for (const SemNP& v : f.vocatives) rest.push_back(", " + voc(v));
  std::string sentence;
  if (f.type == Kind::Excl && f.exclQuam) {
    std::vector<std::string> x = {"how"};
    for (const SemAdj& ad : f.predAdj) x.push_back(ad.lemma);
    if (f.hasSubject) {
      SemNP sb = f.subject;
      std::vector<std::string> adj;
      for (const SemAdj& ad : sb.adjectives) adj.push_back(ad.lemma);
      sb.adjectives.clear();
      sb.definite = true;
      for (const std::string& s2 : adj) x.push_back(s2);
      x.push_back(enNP(sb, side, false));
      x.push_back(beForm(3, sb.number == 2, false));
    }
    for (const std::string& s2 : x) w.push_back(s2);
    return join(w);
  }
  if (!f.hasPred) {
    if (!subj.empty()) w.push_back(subj);
    for (const std::string& r : rest) w.push_back(r);
    return join(w);
  }
  const bool question = f.type == Kind::Yn || f.type == Kind::Wh;
  bool needsDo = false;
  std::vector<std::string> vg = enVerb(f, a, question, needsDo);
  if (f.existential && !question) {
    w.push_back("there");
    w.push_back(beForm(3, f.subject.number == 2, f.pred.tense == Tense::Past));
    if (f.negative) w.push_back("no");
    SemNP sb = f.subject;
    if (f.negative) { sb.definite = false; std::string np = enNP(sb, side, false); if (np.compare(0, 2, "a ") == 0) np = np.substr(2); else if (np.compare(0, 3, "an ") == 0) np = np.substr(3); w.push_back(np); }
    else w.push_back(enNP(sb, side, false));
    for (const std::string& r : rest) w.push_back(r);
    return join(w);
  }
  if (f.type == Kind::Imp) {
    for (const std::string& x : vg) w.push_back(x);
    for (const std::string& r : rest) w.push_back(r);
    return join(w);
  }
  if (f.type == Kind::Wh) {
    std::string wh = f.wh.word;
    if (wh == "what time") wh = "at what time";
    if (wh == "how many" && f.hasObject && f.object.interrogative) {
      wh = enNP(f.object, side, true);
      rest.erase(std::find(rest.begin(), rest.end(), obj));
    }
    if (f.wh.role == frame::Role::Object && f.hasObject && f.object.interrogative && wh != obj) {
      rest.erase(std::find(rest.begin(), rest.end(), obj));
    }
    w.push_back(wh);
    if (f.wh.role == frame::Role::Subject) {   // "Who sees the cat?": no inversion
      bool nd = false;
      SemFrame g = f;
      std::vector<std::string> v2 = enVerb(g, Agr{3, false}, false, nd);
      for (const std::string& x : v2) w.push_back(x);
      for (const std::string& r : rest) w.push_back(r);
      return join(w);
    }
    if (f.wh.role == frame::Role::Predicate) {   // "Who are you?"
      w.push_back(vg.front());
      w.push_back(subj);
      for (size_t i = 1; i < vg.size(); ++i) w.push_back(vg[i]);
      for (const std::string& r : rest) w.push_back(r);
      return join(w);
    }
  }
  if (question) {
    if (f.type == Kind::Yn && !(f.wh.word.size())) {}
    w.push_back(vg.front());
    w.push_back(subj);
    for (size_t i = 1; i < vg.size(); ++i) w.push_back(vg[i]);
  } else {
    w.push_back(subj);
    for (const std::string& x : vg) w.push_back(x);
  }
  for (const std::string& r : rest) w.push_back(r);
  return join(w);
}

std::string capFirst(std::string s) {
  if (!s.empty() && s[0] >= 'a' && s[0] <= 'z') s[0] = (char)(s[0] - 32);
  return s;
}

}  // namespace

std::string realiseEnglish(const Built& b, const Side& side) {
  std::string out;
  for (size_t i = 0; i < b.frames.size(); ++i) {
    const SemFrame& f = b.frames[i];
    std::string c = enClause(f, side);
    for (const frame::SemSub& sb : f.subordinate) {
      if (sb.frame.empty()) continue;
      std::string sc = enClause(sb.frame[0], side);
      std::string marker = sb.marker.empty() ? "that" : sb.marker;
      if (sb.before) c = marker + " " + sc + ", " + c;
      else c += " " + marker + " " + sc;
    }
    if (c.empty()) continue;
    if (!out.empty()) out += (b.joiners[i] == "," ? ", " : "; ");
    out += c;
  }
  // tidy: "a" / "an", spaces before commas
  std::string t;
  for (size_t i = 0; i < out.size(); ++i) {
    if (out[i] == ' ' && i + 1 < out.size() && (out[i + 1] == ',' || out[i + 1] == ' ')) continue;
    t += out[i];
  }
  t = capFirst(t);
  bool question = b.question;
  for (const SemFrame& f : b.frames) question = question || f.type == Kind::Yn || f.type == Kind::Wh;
  if (!t.empty()) t += question ? "?" : b.exclamation ? "!" : ".";
  return t;
}

// ---- Spanish ----------------------------------------------------------------------------------------------------------
namespace {

uint8_t esGender(const SemNP& n, const Side& side) {
  const Lexical* lx = lexAt(side, n.token);
  if (lx && lx->gender) return lx->gender;
  if (n.srcGender) return n.srcGender;
  return es::nounGender(n.head, M);
}

std::string esArticle(uint8_t g, bool pl, bool definite, const std::string& noun) {
  if (definite) {
    if (pl) return g == F ? "las" : "los";
    if (g == F && !es::startsWithStressedA(noun)) return "la";
    return "el";
  }
  if (pl) return "";
  if (g == F && !es::startsWithStressedA(noun)) return "una";
  return "un";
}

std::string esPossessive(const SemNP& p, bool pl) {
  const int person = p.pron.person ? p.pron.person : 3;
  const bool ppl = p.pron.number == 2;
  if (person == 1) return ppl ? (pl ? "nuestros" : "nuestro") : (pl ? "mis" : "mi");
  if (person == 2) return ppl ? (pl ? "sus" : "su") : (pl ? "tus" : "tu");
  return pl ? "sus" : "su";
}

std::string esSubjectPronoun(const SemNP& n) {
  const int p = n.pron.person ? n.pron.person : 3;
  const bool pl = n.pron.number == 2;
  if (!n.pronLemma.empty()) {
    if (n.pronLemma == "everyone") return "todos";
    if (n.pronLemma == "everything") return "todo";
    if (n.pronLemma == "nobody") return "nadie";
    if (n.pronLemma == "nothing") return "nada";
    if (n.pronLemma == "someone") return "alguien";
    if (n.pronLemma == "something") return "algo";
    if (n.pronLemma == "this") return n.pron.gender == F ? "esta" : n.pron.gender == M ? "este" : "esto";
    if (n.pronLemma == "these") return n.pron.gender == F ? "estas" : "estos";
    if (n.pronLemma == "that") return n.pron.gender == F ? "aquella" : n.pron.gender == M ? "aquel" : "aquello";
    if (n.pronLemma == "those") return n.pron.gender == F ? "aquellas" : "aquellos";
  }
  if (p == 1) return pl ? "nosotros" : "yo";
  if (p == 2) return pl ? "ustedes" : "tú";
  if (pl) return n.pron.gender == F ? "ellas" : "ellos";
  return n.pron.gender == F ? "ella" : "él";
}

// Clitic of a pronoun object: me te lo la nos los las; dative le les.
std::string esClitic(const SemNP& n, bool dative) {
  const int p = n.pron.person ? n.pron.person : 3;
  const bool pl = n.pron.number == 2;
  if (p == 1) return pl ? "nos" : "me";
  if (p == 2) return pl ? "los" : "te";
  if (dative) return pl ? "les" : "le";
  if (pl) return n.pron.gender == F ? "las" : "los";
  return n.pron.gender == F ? "la" : "lo";
}

std::string esNP(const SemNP& n, const Side& side) {
  if (n.isPronoun) return esSubjectPronoun(n);
  if (n.isName) return n.head;
  std::vector<std::string> w;
  const uint8_t g = esGender(n, side);
  const bool pl = n.number == 2;
  std::string noun = n.head;
  if (pl && !noun.empty()) noun = es::plural(noun);
  if (!n.possessor.empty()) w.push_back(esPossessive(n.possessor[0], pl));
  else if (n.determiner == "all") { w.push_back(g == F ? "todas" : "todos"); w.push_back(esArticle(g, true, true, noun)); }
  else if (n.determiner == "every") w.push_back(g == F ? "cada" : "cada");
  else if (n.determiner == "no") w.push_back(g == F ? "ninguna" : "ningún");
  else if (n.determiner == "this") w.push_back(g == F ? "esta" : "este");
  else if (n.determiner == "these") w.push_back(g == F ? "estas" : "estos");
  else if (n.determiner == "that") w.push_back(g == F ? "aquella" : "aquel");
  else if (n.determiner == "those") w.push_back(g == F ? "aquellas" : "aquellos");
  else if (n.interrogative && n.wh == "how many") w.push_back(g == F ? "cuántas" : "cuántos");
  else if (n.numeral.empty()) {
    const Lexical* lx = lexAt(side, n.token);
    if (n.definite || !(lx && lx->mass)) w.push_back(esArticle(g, pl, n.definite, noun));
  }
  if (!n.numeral.empty()) w.push_back(n.numeral);
  if (!noun.empty()) w.push_back(noun);
  for (const SemAdj& a : n.adjectives) {
    for (const std::string& av : a.adverbs) w.push_back(av);
    w.push_back(es::adjective(a.lemma, g == F ? F : M, pl ? 2 : 1));
  }
  for (const SemNP& gg : n.genitive) {
    std::string x = esNP(gg, side);
    if (x.compare(0, 3, "el ") == 0) w.push_back("del " + x.substr(3));
    else w.push_back("de " + x);
  }
  return join(w);
}

std::string withA(const std::string& np) {
  if (np.compare(0, 3, "el ") == 0) return "al " + np.substr(3);
  return "a " + np;
}

es::VTense esTense(const SemFrame& f) {
  const frame::SemPredicate& p = f.pred;
  if (p.tense == Tense::Future) return es::VTense::Future;
  if (p.tense == Tense::Past) return p.aspect == Aspect::Progressive || p.pastModal ? es::VTense::Imperfect : es::VTense::Preterite;
  return es::VTense::Present;
}

std::string esClause(const SemFrame& f, const Side& side) {
  std::vector<std::string> w;
  for (const std::string& c : f.connectors) w.push_back(c);
  for (const std::string& ij : f.interjections) w.push_back(ij + ",");
  std::vector<std::string> front, end;
  for (const frame::SemAdverb& a : f.adverbs) (a.front ? front : end).push_back(a.lemma);
  for (const std::string& x : front) w.push_back(x);
  const bool vocAfter = std::find(f.discourse.begin(), f.discourse.end(), "vocative-after") != f.discourse.end();
  auto voc = [&](const SemNP& v) {
    SemNP x = v;
    if (x.isPronoun || x.isName) return esNP(x, side);
    std::string noun = x.number == 2 ? es::plural(x.head) : x.head;
    for (const SemAdj& a : x.adjectives) noun += " " + es::adjective(a.lemma, esGender(x, side), x.number == 2 ? 2 : 1);
    return noun;
  };
  if (!vocAfter)
    for (const SemNP& v : f.vocatives) w.push_back(voc(v) + ",");
  int person = 3, number = 1;
  if (f.hasSubject) {
    if (f.subject.isPronoun) { person = f.subject.pron.person ? f.subject.pron.person : 3; number = f.subject.pron.number == 2 ? 2 : 1; }
    else number = f.subject.number == 2 ? 2 : 1;
    if (f.subject.isPronoun && f.subject.pron.person == 2 && f.subject.pron.number == 2) person = 3;   // ustedes
    if (f.subject.isPronoun && (f.subject.pronLemma == "everyone")) { person = 3; number = 2; }
    if (f.subject.isPronoun && (f.subject.pronLemma == "nobody" || f.subject.pronLemma == "nothing" ||
                                f.subject.pronLemma == "this" || f.subject.pronLemma == "that")) { person = 3; number = 1; }
  }
  if (f.type == Kind::Imp) { person = 2; number = f.imperativePlural ? 2 : 1; }
  // subject: pronouns from the verb are dropped (pro-drop)
  std::string subj;
  if (f.hasSubject && !(f.subject.isPronoun && (f.implicitSubject || f.subject.pronLemma.empty() ||
                                                 f.subject.pronLemma == "ἐγώ" || f.subject.pronLemma == "σύ" ||
                                                 f.subject.pronLemma == "αὐτόσ")))
    subj = esNP(f.subject, side);
  // clitics and objects
  std::vector<std::string> clitics;
  std::string obj, io;
  if (f.hasIndirect) {
    if (f.indirectObject.isPronoun) clitics.push_back(esClitic(f.indirectObject, true));
    else io = withA(esNP(f.indirectObject, side));
  }
  if (f.hasObject) {
    if (f.object.isPronoun && f.object.pronLemma != "this" && f.object.pronLemma != "these" &&
        f.object.pronLemma != "that" && f.object.pronLemma != "everything" && f.object.pronLemma != "nothing" &&
        !f.object.interrogative)
      clitics.push_back(esClitic(f.object, false));
    else if (!f.object.interrogative) {
      const Lexical* lx = lexAt(side, f.object.token);
      std::string x = esNP(f.object, side);
      const bool specific = f.object.definite || f.object.isName || !f.object.possessor.empty() || !f.object.determiner.empty();
      obj = ((f.object.isName || (lx && (lx->person || lx->animal))) && specific && !f.object.isPronoun) ? withA(x) : x;
    }
  }
  std::string verb = f.pred.lemma.empty() ? "hacer" : f.pred.lemma;
  // ser / estar: location, "where", no predicate -> estar
  if (f.copula) {
    const bool location = (f.predicative.empty() && f.predAdj.empty() && f.wh.role != frame::Role::Predicate) ||
                          f.wh.word == "where";
    verb = location ? "estar" : "ser";
  }
  bool refl = es::reflexive(verb);
  if (refl) {
    verb = es::unreflexive(verb);
    const char* se = person == 1 ? (number == 2 ? "nos" : "me") : person == 2 && number == 1 ? "te" : "se";
    clitics.insert(clitics.begin(), se);
  }
  std::vector<std::string> vg;
  const frame::SemPredicate& p = f.pred;
  auto conj = [&](const std::string& inf, es::VTense t) { return es::verb(inf, t, person, number); };
  std::string cl;
  for (const std::string& c : clitics) cl += (cl.empty() ? "" : " ") + c;
  if (f.type == Kind::Imp) {
    if (f.negative) {
      vg.push_back("no");
      if (!cl.empty()) vg.push_back(cl);
      vg.push_back(conj(verb, es::VTense::SubjPresent));
    } else {
      std::string im = es::verb(verb, es::VTense::Imperative, 2, number);
      std::string attached;
      for (const std::string& c : clitics) attached += c;
      vg.push_back(attached.empty() ? im : es::withClitics(im, attached));
    }
  } else {
    if (f.negative) vg.push_back("no");
    if (!cl.empty()) vg.push_back(cl);
    if (p.modality != Modality::None) {
      const char* m = p.modality == Modality::Can ? "poder" : p.modality == Modality::Want ? "querer"
                    : p.modality == Modality::Must ? "deber" : p.modality == Modality::May ? "poder" : "ir";
      vg.push_back(conj(m, esTense(f)));
      if (p.modality == Modality::Will) vg.push_back("a");
      vg.push_back(verb);
    } else if (p.aspect == Aspect::Perfect) {
      vg.push_back(conj("haber", p.tense == Tense::Past ? es::VTense::Imperfect : es::VTense::Present));
      vg.push_back(es::verb(verb, es::VTense::PastPart, 3, 1));
    } else {
      vg.push_back(conj(verb, esTense(f)));
    }
  }
  std::vector<std::string> rest;
  if (!obj.empty()) rest.push_back(obj);
  if (!io.empty()) rest.push_back(io);
  for (const SemNP& pn : f.predicative) {
    SemNP x = pn;
    if (!x.definite && x.possessor.empty() && !x.isPronoun && !x.isName && x.adjectives.empty()) {
      // "Mi padre es campesino": a bare predicate noun
      std::string noun = x.number == 2 ? es::plural(x.head) : x.head;
      rest.push_back(noun);
    } else {
      rest.push_back(esNP(x, side));
    }
  }
  {
    uint8_t g = M;
    bool pl = number == 2;
    if (f.hasSubject && !f.subject.isPronoun) g = esGender(f.subject, side);
    else if (f.hasSubject && f.subject.isPronoun && f.subject.pron.gender) g = f.subject.pron.gender == F ? F : M;
    for (size_t i = 0; i < f.predAdj.size(); ++i) {
      if (i) rest.push_back("y");
      for (const std::string& av : f.predAdj[i].adverbs) rest.push_back(av);
      rest.push_back(es::adjective(f.predAdj[i].lemma, g, pl ? 2 : 1));
    }
  }
  for (const frame::SemOblique& o : f.obliques) {
    std::string x = esNP(o.np, side);
    std::string prep = o.prep.empty() ? "en" : o.prep;
    if ((prep == "a" || prep == "de") && x.compare(0, 3, "el ") == 0) rest.push_back((prep == "a" ? "al " : "del ") + x.substr(3));
    else rest.push_back(prep + " " + x);
  }
  for (const std::string& x : end) rest.push_back(x);
  if (vocAfter)
    for (const SemNP& v : f.vocatives) rest.push_back(", " + voc(v));
  if (f.type == Kind::Excl && f.exclQuam) {
    std::vector<std::string> x = {"qué"};
    if (f.hasSubject) {
      SemNP sb = f.subject;
      std::vector<SemAdj> adj = sb.adjectives;
      sb.adjectives.clear();
      std::string noun = sb.number == 2 ? es::plural(sb.head) : sb.head;
      x.push_back(noun);
      const uint8_t g = esGender(sb, side);
      for (const SemAdj& a : adj) { x.push_back("tan"); x.push_back(es::adjective(a.lemma, g, sb.number == 2 ? 2 : 1)); }
    }
    for (const SemAdj& a : f.predAdj) x.push_back(es::adjective(a.lemma, M, 1));
    for (const std::string& s2 : x) w.push_back(s2);
    return join(w);
  }
  if (!f.hasPred) {
    if (!subj.empty()) w.push_back(subj);
    for (const std::string& r : rest) w.push_back(r);
    return join(w);
  }
  if (f.existential) {
    if (f.negative) w.push_back("no");
    w.push_back(f.pred.tense == Tense::Past ? "había" : "hay");
    SemNP sb = f.subject;
    std::string np = esNP(sb, side);
    if (f.negative) {
      for (const char* a : {"un ", "una "})
        if (np.compare(0, std::char_traits<char>::length(a), a) == 0) np = np.substr(std::char_traits<char>::length(a));
    }
    w.push_back(np);
    for (const std::string& r : rest) w.push_back(r);
    return join(w);
  }
  if (f.type == Kind::Wh) {
    std::string wh = f.wh.word == "where" ? (f.pred.aspect == Aspect::Progressive || (lexAt(side, f.pred.token) && lexAt(side, f.pred.token)->motion) ? "adónde" : "dónde")
                   : f.wh.word == "who" ? "quién" : f.wh.word == "what" ? "qué" : f.wh.word == "how" ? "cómo"
                   : f.wh.word == "when" ? "cuándo" : f.wh.word == "why" ? "por qué" : f.wh.word == "where from" ? "de dónde"
                   : f.wh.word == "what time" ? "a qué hora" : f.wh.word;
    if (f.wh.word == "how many" && f.hasObject && f.object.interrogative) wh = esNP(f.object, side);
    w.push_back(wh);
    for (const std::string& x : vg) w.push_back(x);
    if (!subj.empty()) w.push_back(subj);
    for (const std::string& r : rest) w.push_back(r);
    return join(w);
  }
  if (!subj.empty()) w.push_back(subj);
  for (const std::string& x : vg) w.push_back(x);
  for (const std::string& r : rest) w.push_back(r);
  return join(w);
}

std::string capFirstUtf8(const std::string& s) {
  if (s.empty()) return s;
  size_t i = 0;
  if (s.compare(0, 2, "\xC2\xBF") == 0 || s.compare(0, 2, "\xC2\xA1") == 0) i = 2;
  std::string head = s.substr(0, i), rest = s.substr(i);
  if (!rest.empty() && rest[0] >= 'a' && rest[0] <= 'z') { rest[0] = (char)(rest[0] - 32); return head + rest; }
  // á é í ó ú ñ
  static const char* const lo[] = {"á", "é", "í", "ó", "ú", "ñ"};
  static const char* const up[] = {"Á", "É", "Í", "Ó", "Ú", "Ñ"};
  for (int k = 0; k < 6; ++k)
    if (rest.compare(0, 2, lo[k]) == 0) return head + up[k] + rest.substr(2);
  return s;
}

}  // namespace

std::string realiseSpanish(const Built& b, const Side& side) {
  std::string out;
  bool question = b.question;
  for (const SemFrame& f : b.frames) question = question || f.type == Kind::Yn || f.type == Kind::Wh;
  for (size_t i = 0; i < b.frames.size(); ++i) {
    const SemFrame& f = b.frames[i];
    std::string c = esClause(f, side);
    for (const frame::SemSub& sb : f.subordinate) {
      if (sb.frame.empty()) continue;
      std::string sc = esClause(sb.frame[0], side);
      std::string marker = sb.marker.empty() ? "que" : sb.marker;
      if (sb.before) c = marker + " " + sc + ", " + c;
      else c += " " + marker + " " + sc;
    }
    if (c.empty()) continue;
    if (!out.empty()) out += (b.joiners[i] == "," ? ", " : "; ");
    out += c;
  }
  std::string t;
  for (size_t i = 0; i < out.size(); ++i) {
    if (out[i] == ' ' && i + 1 < out.size() && (out[i + 1] == ',' || out[i + 1] == ' ')) continue;
    t += out[i];
  }
  if (t.empty()) return t;
  if (question) t = "\xC2\xBF" + t + "?";
  else if (b.exclamation) t = "\xC2\xA1" + t + "!";
  else t += ".";
  return capFirstUtf8(t);
}

}  // namespace vp::grc2x::detail
