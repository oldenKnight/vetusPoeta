// grc2x readable sentence: SemFrames filled from the Greek roles (buildFrames) and the plain English / es-MX Spanish
// realisers over them. English / Spanish word morphology comes from la2x's helpers (la2x/internal.h: en::verb,
// en::plural, es::verb, es::adjective ...), so both back-translation paths inflect the same way.
#include <algorithm>
#include <functional>

#include "grc2x/internal.h"
#include "la2x/internal.h"
#include "vp/morph.h"
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
  out.spoken.assign(n, 0);
  out.dropped.clear();
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
  // C38: μέν ... δέ: δέ is "but" when the sentence has μέν ("ὁ μὲν πατὴρ πονεῖ, ὁ δὲ παῖς παίζει")
  bool menSentence = false;
  for (size_t i = 0; i < n; ++i) menSentence = menSentence || (ti[i].word && keyIs(ti[i], {"μέν"}));
  bool lastPredSet = false;
  Tense lastTense = Tense::Present;
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
    const bool coordStart = keyIs(ti[idx[0]], {"καί", "ἀλλά"});   // C38: a clause split off at a coordinating word
    Pending p;
    p.sepAfter = sepAfter;
    SemFrame& f = p.f;
    f.punct = "";
    std::vector<NPInfo> nps;
    std::vector<std::pair<std::string, NPInfo>> obl;   // prep word, NP
    size_t finite = n, infinitive = n;
    bool ara = false, me = false, hos = false, frontSet = false;
    bool sawContent = false;
    std::vector<size_t> negTokens;   // C38: spoken when the clause has a verb (the realisers print "not" there)
    size_t allaTok = n;              // C38: a mid-clause ἀλλά ("not X but Y")
    size_t deConn = std::string::npos;   // C38: the connector of δέ in f.connectors
    size_t k = 0;
    auto lexOf = [&](size_t i) -> const Lexical& { return side.lex[i]; };
    // NP starting at idx[k]
    auto readNP = [&](size_t& kk, NPInfo& np) -> bool {
      if (kk >= idx.size()) return false;
      size_t i = idx[kk];
      if (!(nominal(ti[i]) || ti[i].lpos == Article)) return false;
      np = NPInfo{};
      np.first = (int)i;
      int reflexivePossessor = -1;
      uint8_t cs = ti[i].f.case_;
      uint8_t num = ti[i].f.number;
      std::vector<size_t> members;
      std::vector<std::vector<size_t>> advs;   // degree adverbs before each member
      std::vector<size_t> pendingAdv;
      while (kk < idx.size()) {
        const size_t j = idx[kk];
        const TokInfo& t = ti[j];
        if ((t.lpos == Adv || t.lpos == Particle) && !negWord(t) && kk + 1 < idx.size() && nominal(ti[idx[kk + 1]]) &&
            ti[idx[kk + 1]].f.case_ == cs && !members.empty() && ti[idx[kk + 1]].lpos == Adj) {
          pendingAdv.push_back(j);
          ++kk;
          continue;
        }
        if (!(nominal(t) || t.lpos == Article)) break;
        // C38: a reflexive possessor between the article and its noun ("τὴν ἑαυτοῦ οἰκίαν" = his own house)
        if (members.size() == 1 && ti[members[0]].lpos == Article && t.f.case_ == Gen && cs != Gen &&
            (t.key == "ἑαυτοῦ" || t.key == "ἐμαυτοῦ" || t.key == "σεαυτοῦ" || t.key == "ἑαυτοῦσ" ||
             keyIs(t, {"ἑαυτοῦ", "ἑαυτῆς", "ἑαυτῶν", "ἐμαυτοῦ", "σεαυτοῦ", "ἐμαυτῆς", "σεαυτῆς"}))) {
          reflexivePossessor = (int)j;
          ++kk;
          continue;
        }
        if (t.f.case_ != cs) break;
        // predicate position: an adjective after the noun of an articled group without its own article
        // ("ἡ οἰκία μικρά ἐστιν" = the house is small)
        if (!members.empty() && (t.lpos == Adj || t.lpos == Participle) && ti[members.back()].lpos != Article) {
          bool art = false, headSeen = false, nameSeen = false;
          for (size_t m : members) {
            art = art || ti[m].lpos == Article;
            headSeen = headSeen || ti[m].lpos == Noun || ti[m].lpos == Name;
            nameSeen = nameSeen || ti[m].name || ti[m].lpos == Name;
          }
          // C38: also after a name ("Δικαιόπολις Ἀθηναῖός ἐστιν": Athenian was an attribute of the name and lost)
          if (((art && headSeen) || nameSeen) && t.key != "πᾶσ" && t.key != "οὗτοσ" && t.key != "ἐκεῖνοσ") break;
        }
        // C38: a second noun or name without its own article after the group's head is another group (a predicate
        // noun: "ὁ Στέφανος γεωργός ἐστιν" was read "the Stephen farmer")
        if (!members.empty() && (t.lpos == Noun || t.lpos == Name || t.name)) {
          bool headSeen = false;
          for (size_t m : members) headSeen = headSeen || ti[m].lpos == Noun || ti[m].lpos == Name || ti[m].name;
          if (headSeen) break;
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
      // C38: the article alone before δέ / μέν is a pronoun ("ὁ δὲ οὐκ ἀκούει" = but he does not hear; it was "the one")
      if (members.size() == 1 && ti[members[0]].lpos == Article) {
        const size_t a0 = members[0];
        if (a0 + 1 < n && keyIs(ti[a0 + 1], {"δέ", "μέν"})) {
          SemNP& x = np.np;
          x.isPronoun = true;
          x.pron.person = 3;
          x.pron.number = ti[a0].f.number == Pl ? 2 : 1;
          x.pron.gender = ti[a0].f.gender;
          x.number = x.pron.number;
          x.token = (int)a0;
          x.tokens.push_back((int)a0);
          x.pronLemma = "ὁ";   // printed in Spanish too ("y él no escucha": the subject changes)
          return true;
        }
      }
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
        // C41: the possessive adjectives are possessors ("ὁ ἐμὸς πατήρ" = my father, "ἡ ἡμετέρα οἰκία" = our house /
        // nuestra casa; was "the my father", "la casa nuestra", "el padre mina")
        if (t.key == "ἐμόσ" || t.key == "σόσ" || t.key == "ἡμέτεροσ" || t.key == "ὑμέτεροσ") {
          SemNP po;
          po.isPronoun = true;
          po.pron.person = (t.key == "ἐμόσ" || t.key == "ἡμέτεροσ") ? 1 : 2;
          po.pron.number = (t.key == "ἡμέτεροσ" || t.key == "ὑμέτεροσ") ? 2 : 1;
          po.token = (int)m;
          po.tokens.push_back((int)m);
          x.possessor.push_back(po);
          continue;
        }
        // C41: plural πολλοί / ὀλίγοι before or after a noun are quantity words ("πολλοὺς λόγους" = many words, never
        // "much words" / "palabras muchas"); with the article they stay adjectives (οἱ πολλοί, "the many")
        if ((t.key == "πολύσ" || t.key == "ὀλίγοσ") && num == Pl && !np.article) {
          x.determiner = t.key == "πολύσ" ? "many" : "few";
          continue;
        }
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
          } else if (np.adjOnly && np.article && h.lpos == Adj) {
            // C40 (b): an adjective used as a noun with the article: its readable noun ("ὁ αὐτουργός" = the farmer who
            // works his own land; a masculine / feminine one is a person); without one the English plural of an
            // adjective takes no -s ("the self-sufficient", never "the self-sufficients")
            Lexical& hx = side.lex[head];
            if (!hx.noun.empty()) {
              x.head = hx.noun;
              hx.plural = hx.nounPlural;
              if (h.f.gender != N && h.f.gender != 0) hx.person = true;
              if (esT) hx.gender = h.f.gender == F ? F : M;
              x.srcGender = hx.gender;
            } else if (!esT && x.number == 2 && hx.plural.empty() && !x.head.empty() && !hx.person &&
                       !hx.animal && !(x.head[0] >= 'A' && x.head[0] <= 'Z')) {
              // a readable adjective ("the wise"); a person row is a noun ("the allies"). C41: also an adjective
              // without a row ("οἱ ἄδικοι" = the unjust, was "the unjusts")
              hx.plural = x.head;
            }
            // C41: Spanish: a masculine / feminine adjective used as a noun is a person ("castiga a los injustos")
            if (esT && hx.noun.empty() && h.f.gender != N && h.f.gender != 0) hx.person = true;
          }
        }
      }
      if (reflexivePossessor >= 0) {   // C38: "his own" / "her own" / "their own" (Spanish "su propio")
        const TokInfo& r = ti[(size_t)reflexivePossessor];
        SemNP po;
        po.isPronoun = true;
        const std::string rk = bareKey(r.text);
        po.pron.person = rk.rfind(text::greek_key("ἐμαυτ"), 0) == 0 ? 1 : rk.rfind(text::greek_key("σεαυτ"), 0) == 0 ? 2 : 3;
        po.pron.number = r.f.number == Pl ? 2 : 1;
        po.pron.gender = r.f.gender;
        po.pronLemma = "own";
        po.token = reflexivePossessor;
        po.tokens.push_back(reflexivePossessor);
        x.possessor.push_back(po);
        x.tokens.push_back(reflexivePossessor);
        x.definite = false;
      }
      // enclitic / pronoun possessor right after ("ὁ πατήρ μου", "τὴν κεφαλὴν αὐτοῦ")
      // C31: also after a genitive NP when the pronoun is the enclitic μου / σου ("ἐκ τῆς ῥινός μου": "of me" was read)
      if (kk < idx.size() && (cs != Gen || ((ti[idx[kk]].key == "ἐγώ" || ti[idx[kk]].key == "σύ") &&
                                                grc::accentOf(ti[idx[kk]].text).accents == 0))) {
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
        out.roles[i] = "connector";
        // C38: ἀλλά inside a clause after its first words is the "but" of "not X but Y" (οὐκ ἐν Ἀθήναις ἀλλὰ ἐν τοῖς
        // ἀγροῖς; σῖτον οὐ πολὺν ἀλλὰ ἱκανόν), resolved after the walk; it was read "and but ... in the fields"
        if (sawContent && keyIs(t, {"ἀλλά"}) && allaTok == n) { allaTok = i; ++k; continue; }
        std::string cw = lw.word;
        if (keyIs(t, {"δέ"}) && menSentence) cw = esT ? "pero" : "but";
        if (!cw.empty() && cw != "-") {
          if (keyIs(t, {"δέ"})) deConn = f.connectors.size();
          f.connectors.push_back(cw);
          out.spoken[i] = 1;
        }
        ++k;
        continue;
      }
      if (keyIs(t, {"μέν", "γε", "δή", "τοι"})) { out.roles[i] = "particle"; ++k; continue; }
      if (negWord(t)) {
        f.negative = true;
        if (keyIs(t, {"μή"})) me = true;
        out.roles[i] = "negation";
        negTokens.push_back(i);
        ++k;
        continue;
      }
      // subordinator
      if (t.lpos == Conj && keyIs(t, {"ὅτι", "ἐπεί", "ὅτε", "εἰ", "ἐάν", "ἵνα", "ὥστε", "ἐπειδή"}) && !sawContent) {
        p.sub = true;
        p.marker = lw.word;
        if (!lw.word.empty()) out.spoken[i] = 1;
        p.rel = keyIs(t, {"ὅτι"}) ? Relation::Cause : keyIs(t, {"εἰ", "ἐάν"}) ? Relation::Condition
              : keyIs(t, {"ἵνα"}) ? Relation::Purpose : keyIs(t, {"ὥστε"}) ? Relation::Result : Relation::Time;
        out.roles[i] = "subordinator";
        ++k;
        continue;
      }
      if (t.lpos == Conj && keyIs(t, {"καί", "ἤ"})) {   // inside a clause: coordination of NPs
        out.roles[i] = "conjunction";
        ++k;
        // C38: two noun groups of one case joined by καί / ἤ are one coordinated group ("ἡ Χλόη καὶ ἡ μήτηρ ...
        // βαδίζουσιν" = Chloe and the mother walk; the mother was printed as a predicate of "walk")
        if (!nps.empty() && k >= 2 && (size_t)nps.back().last == idx[k - 2] && !nps.back().adjOnly) {
          size_t kk = k;
          NPInfo nx;
          if (readNP(kk, nx) && nx.case_ == nps.back().case_ && !nx.adjOnly && !nx.np.interrogative) {
            nps.back().np.coord.push_back(nx.np);
            nps.back().np.coordConj = keyIs(t, {"ἤ"}) ? (esT ? "o" : "or") : (esT ? "y" : "and");
            for (int tk : nx.np.tokens) nps.back().np.tokens.push_back(tk);
            nps.back().last = nx.last;
            k = kk;
          }
        }
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
      // C33: the Attic times of day read back as one adverb: ἅμα τῇ ἕῳ "at dawn", πρὸς ἑσπέραν "towards evening",
      // μεσημβρίας "at noon", a bare νυκτός "at night", ἑσπέρας "in the evening"
      {
        auto at = [&](size_t d) -> const TokInfo* { return k + d < idx.size() ? &ti[idx[k + d]] : nullptr; };
        const TokInfo* prev = k > 0 ? &ti[idx[k - 1]] : nullptr;
        const bool bare = !prev || (prev->lpos != Article && prev->lpos != Adj && prev->lpos != Det);
        size_t used = 0;
        const char* en = nullptr;
        const char* sp = nullptr;
        if (keyIs(t, {"ἅμα"}) && at(1) && at(1)->lpos == Article && at(2) && at(2)->key == text::greek_key("ἕως") &&
            at(2)->f.case_ == Dat) {
          used = 3, en = "at dawn", sp = "al amanecer";
        } else if (keyIs(t, {"ἅμα"}) && at(1) && at(1)->key == text::greek_key("ἕως") && at(1)->f.case_ == Dat) {
          used = 2, en = "at dawn", sp = "al amanecer";
        } else if (keyIs(t, {"πρός"}) && at(1) && at(1)->key == text::greek_key("ἑσπέρα") && at(1)->f.case_ == Acc) {
          used = 2, en = "towards evening", sp = "al atardecer";
        } else if (bare && t.key == text::greek_key("μεσημβρία") && t.f.case_ == Gen) {
          used = 1, en = "at noon", sp = "a mediodía";
        } else if (bare && t.key == text::greek_key("ἑσπέρα") && t.f.case_ == Gen) {
          used = 1, en = "in the evening", sp = "por la tarde";
        } else if (bare && t.key == text::greek_key("νύξ") && t.f.case_ == Gen && t.f.number != Pl) {
          used = 1, en = "at night", sp = "de noche";
        }
        if (used) {
          frame::SemAdverb a;
          a.lemma = esT ? sp : en;
          a.token = (int)i;
          a.front = !sawContent && !frontSet;
          f.adverbs.push_back(a);
          for (size_t d = 0; d < used; ++d) out.roles[idx[k + d]] = "adverb";
          k += used;
          continue;
        }
      }
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
          // C33: a neuter noun group after πρός read as a nominative is the accusative ("πρὸς τὸ ὄρος" was "near")
          else if (keyIs(t, {"πρός"})) w = np.case_ == Dat ? (esT ? "junto a" : "near") : (esT ? "hacia" : "to");
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
    // C38: "not X but Y" (οὐ / οὐκ right before X, ἀλλά right before Y): two prepositional phrases ("lives not in Athens
    // but in the fields") or two adjectives without a noun after a noun group of their case ("provides food, not much
    // but enough"); the negation then belongs to X, not to the verb. Anything else stays a guess (role-doubt).
    if (allaTok < n) {
      auto posOf = [&](size_t tok) -> long {
        for (size_t q = 0; q < idx.size(); ++q) if (idx[q] == tok) return (long)q;
        return -1;
      };
      auto negBefore = [&](size_t tok) -> long {   // the negation token right before `tok`, else -1
        const long q = posOf(tok);
        if (q <= 0) return -1;
        const size_t pt = idx[(size_t)q - 1];
        return std::find(negTokens.begin(), negTokens.end(), pt) != negTokens.end() ? (long)pt : -1;
      };
      const long qa = posOf(allaTok);
      bool done = false;
      // prepositional phrases
      for (size_t a = 0; a < f.obliques.size() && !done; ++a) {
        frame::SemOblique& o1 = f.obliques[a];
        if (o1.token < 0 || (size_t)o1.token > allaTok) continue;
        const long ng = negBefore((size_t)o1.token);
        if (ng < 0) continue;
        for (size_t b = 0; b < f.obliques.size(); ++b) {
          frame::SemOblique& o2 = f.obliques[b];
          if (o2.token < 0 || posOf((size_t)o2.token) != qa + 1) continue;
          o1.prep = (esT ? "no " : "not ") + o1.prep;
          o2.prep = (esT ? "sino " : "but ") + o2.prep;
          o1.front = o2.front = false;
          f.negative = false;
          negTokens.erase(std::find(negTokens.begin(), negTokens.end(), (size_t)ng));
          out.spoken[(size_t)ng] = out.spoken[allaTok] = 1;
          done = true;
          break;
        }
      }
      // two adjectives without a noun after a noun group of their case
      for (size_t a = 0; a + 1 < nps.size() && !done; ++a) {
        NPInfo& x1 = nps[a];
        NPInfo& x2 = nps[a + 1];
        if (!x1.adjOnly || x1.article || !x2.adjOnly || x2.article || x1.case_ != x2.case_) continue;
        if (posOf((size_t)x2.first) != qa + 1 || x1.first > (int)allaTok) continue;
        const long ng = negBefore((size_t)x1.first);
        if (ng < 0 || x1.np.adjectives.empty() || x2.np.adjectives.empty()) continue;
        const NPInfo* noun = nullptr;
        for (size_t b = 0; b < a; ++b)
          if (nps[b].case_ == x1.case_ && !nps[b].adjOnly) noun = &nps[b];
        if (!noun) continue;
        auto adjWord = [&](const NPInfo& x) {
          const std::string& w = x.np.adjectives[0].lemma;
          if (!esT) return w;
          const uint8_t g = noun->np.token >= 0 && side.lex[(size_t)noun->np.token].gender ? side.lex[(size_t)noun->np.token].gender : (uint8_t)M;
          return es::adjective(w, g == F ? F : M, noun->np.number == 2 ? 2 : 1);
        };
        f.discourse.push_back(std::string("tail:") + (esT ? "no " : "not ") + adjWord(x1) + (esT ? " sino " : " but ") +
                              adjWord(x2));
        for (int tk : x1.np.tokens) out.spoken[(size_t)tk] = 1;
        for (int tk : x2.np.tokens) out.spoken[(size_t)tk] = 1;
        for (const SemAdj& ad : x1.np.adjectives) if (ad.token >= 0) out.spoken[(size_t)ad.token] = 1;
        for (const SemAdj& ad : x2.np.adjectives) if (ad.token >= 0) out.spoken[(size_t)ad.token] = 1;
        negTokens.erase(std::find(negTokens.begin(), negTokens.end(), (size_t)ng));
        out.spoken[(size_t)ng] = out.spoken[allaTok] = 1;
        f.negative = !negTokens.empty();
        nps.erase(nps.begin() + (long)a, nps.begin() + (long)a + 2);
        done = true;
      }
      if (!done) {   // unresolved: the old reading (a clause connector), never OK
        const Lexical& lw = side.lex[allaTok];
        if (!lw.word.empty()) { f.connectors.insert(f.connectors.begin(), lw.word); out.spoken[allaTok] = 1; }
        out.flags.push_back("role-doubt");
      }
    }
    // C38: δέ with ὅμως is "and yet" ("ὅμως δὲ ὁ γεωργὸς χαίρει"; "and nevertheless" was printed)
    if (deConn != std::string::npos && deConn < f.connectors.size())
      for (size_t a = 0; a < f.adverbs.size(); ++a)
        if (f.adverbs[a].token >= 0 && keyIs(ti[(size_t)f.adverbs[a].token], {"ὅμως"})) {
          f.connectors[deConn] = esT ? "y sin embargo" : "and yet";
          out.spoken[(size_t)f.adverbs[a].token] = 1;
          f.adverbs.erase(f.adverbs.begin() + (long)a);
          break;
        }
    // ---- roles from the cases ----
    const TokInfo* vt = finite < n ? &ti[finite] : nullptr;
    const std::string vkey = vt ? vt->key : std::string();
    bool eimi = vkey == "εἰμί";
    // C38: a clause without a verb whose nominative groups are a subject (article, name or pronoun) and a bare
    // adjective or noun is a copula clause with ἐστί left out ("μακρὸς δὲ ὁ πόνος" = and the work is long; "ὁ δὲ
    // Ξανθίας δοῦλος" = and Xanthias is a slave); it was printed without a verb
    bool elliptic = false;
    if (!vt && infinitive == n) {
      int subj = 0, bare = 0, other = 0;
      for (const NPInfo& x : nps) {
        if (x.case_ != Nom) { ++other; continue; }
        if (x.article || x.np.isName || x.np.isPronoun) ++subj;
        else if (x.adjOnly || !x.np.head.empty()) ++bare;
      }
      elliptic = subj == 1 && bare >= 1 && other == 0 && f.type != Kind::Wh && !out.exclamation;
      // C38: "ἡ οἰκία μικρά ἐστιν, καλὴ δέ" / "ὁ ἀγρὸς οὐ μέγας ἐστίν, ἱκανὸς δέ": only bare adjectives after a copula
      // clause: the same subject, "but it is beautiful" ("and beautiful" was printed)
      if (!elliptic && subj == 0 && bare >= 1 && other == 0 && !clauses.empty() && clauses.back().f.copula &&
          clauses.back().f.hasSubject && f.type != Kind::Wh && deConn != std::string::npos) {
        bool allAdj = true;
        for (const NPInfo& x : nps) allAdj = allAdj && x.adjOnly && !x.article;
        if (allAdj) {
          elliptic = true;
          const SemNP& ps = clauses.back().f.subject;
          SemNP pr;
          pr.isPronoun = true;
          pr.pron.person = ps.isPronoun ? (ps.pron.person ? ps.pron.person : 3) : 3;
          pr.pron.number = ps.isPronoun ? ps.pron.number : (ps.number == 2 ? 2 : 1);
          uint8_t g = ps.isPronoun ? ps.pron.gender : (uint8_t)N;
          if (!ps.isPronoun && ps.token >= 0 && (side.lex[(size_t)ps.token].person || ps.isName))
            g = ti[(size_t)ps.token].f.gender ? ti[(size_t)ps.token].f.gender : ti[(size_t)ps.token].lgender;
          if (esT && !ps.isPronoun && ps.token >= 0 && side.lex[(size_t)ps.token].gender)   // "la casa ... es hermosa"
            g = side.lex[(size_t)ps.token].gender;
          pr.pron.gender = g;
          pr.number = pr.pron.number;
          f.hasSubject = true;
          f.subject = pr;
          f.connectors[deConn] = esT ? "pero" : "but";
        }
      }
      if (elliptic) eimi = true;
    }
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
        // C38: a clause split off at καί / ἀλλά shares the previous clause's subject; a neuter group before its 3rd
        // person verb is then read as the object ("... μένει καὶ τὸ δεῖπνον παρασκευάζει" = and prepares the dinner;
        // "and the meal prepares" was printed): a guess (an intransitive verb would want the subject)
        else if (coordStart && !clauses.empty() && clauses.back().sepAfter.empty() && clauses.back().f.hasSubject &&
                 vt && vt->f.person == 3 && noms == 1 && np.first < (int)finite) {
          np.case_ = Acc;
          out.flags.push_back("role-doubt");
        }
      }
    }
    // C31: a neuter accusative of πολύς / ὀλίγος used as an adverb ("πολὺ ἔρρει" = it flowed a lot; it was read as the
    // object "a much") when the verb has no other accusative
    {
      size_t accs = 0;
      for (const NPInfo& m : merged) accs += m.case_ == Acc;
      for (size_t a = 0; a < merged.size() && accs == 1; ++a) {
        const NPInfo& m = merged[a];
        if (m.case_ != Acc || !m.adjOnly || m.article || m.np.adjectives.size() != 1 || m.np.token < 0) continue;
        const TokInfo& h = ti[(size_t)m.np.token];
        const uint8_t g = h.f.gender ? h.f.gender : h.lgender;
        if (g != N || h.f.number == Pl || !(h.key == "πολύσ" || h.key == "ὀλίγοσ")) continue;
        frame::SemAdverb av;
        av.lemma = h.key == "πολύσ" ? (esT ? "mucho" : "a lot") : (esT ? "un poco" : "a little");
        av.token = m.np.token;
        f.adverbs.push_back(av);
        out.roles[(size_t)m.np.token] = "adverb";
        merged.erase(merged.begin() + (long)a);
        break;
      }
    }
    // C31: accusative + infinitive after a verb of hoping, saying or thinking ("ἐλπίζω σε αὔριον ἥξειν" = "I hope that
    // you will come tomorrow"; it was read "I hope you tomorrow"): the infinitive with its accusative subject (the
    // first accusative before it), its object or predicate (a second accusative) and the adverbs and phrases on its
    // side of the finite verb become a that-clause; the tense from the infinitive (future / aorist after hoping:
    // "will"; aorist after saying: the past). Without an accusative subject, a verb of hoping keeps "hope to win" and a
    // verb of saying or thinking repeats its own subject ("νομίζω σοφὸς εἶναι" -> "I think that I am wise").
    if (vt && infinitive < n) {
      static const char* const kHope[] = {"ἐλπίζω", "ὑπισχνέομαι", "ὄμνυμι", nullptr};
      static const char* const kSay[] = {"νομίζω", "οἴομαι", "οἶμαι", "φημί", "λέγω", "ἡγέομαι", "δοκέω", nullptr};
      auto listed = [&](const char* const* ks) {
        for (const char* const* x = ks; *x; ++x) if (vkey == text::greek_key(*x)) return true;
        return false;
      };
      bool dative = false;
      for (const NPInfo& m : merged) dative = dative || m.case_ == Dat;
      const bool hope = listed(kHope), say = !hope && listed(kSay) && !(vkey == text::greek_key("δοκέω") && vt->f.person == 3);
      if (hope || say) {
        const TokInfo& iv = ti[infinitive];
        const bool einai = iv.key == "εἰμί";
        std::vector<size_t> accs;
        for (size_t a = 0; a < merged.size(); ++a)
          if (merged[a].case_ == Acc && !(merged[a].np.interrogative)) accs.push_back(a);
        long subjI = -1, secondI = -1;
        if (accs.size() >= 2) { subjI = (long)accs[0]; secondI = (long)accs[1]; }
        else if (accs.size() == 1 && merged[accs[0]].first < (int)infinitive) subjI = (long)accs[0];
        else if (accs.size() == 1) secondI = (long)accs[0];
        // a nominative predicate with εἶναι and the same subject ("νομίζω σοφὸς εἶναι")
        long nomPred = -1;
        if (einai && subjI < 0)
          for (size_t a = 0; a < merged.size(); ++a)
            if (merged[a].case_ == Nom && merged[a].first > (int)finite && merged[a].np.isPronoun == false) nomPred = (long)a;
        // λέγω / δοκέω + infinitive without an accusative subject is a command or "seem": not a that-clause
        const bool sameSubjectSay = say && subjI < 0 && !dative && vkey != text::greek_key("λέγω") &&
                                    vkey != text::greek_key("δοκέω");
        if (subjI >= 0 || sameSubjectSay) {
          SemFrame sf;
          sf.type = Kind::Decl;
          sf.hasPred = true;
          sf.pred.token = (int)infinitive;
          sf.pred.lemma = einai ? std::string("be") : side.lex[infinitive].word;
          sf.copula = einai;
          switch (iv.f.tense) {
            case Future: sf.pred.tense = Tense::Future; break;
            case Aorist: sf.pred.tense = hope ? Tense::Future : Tense::Past; break;
            case Perfect: sf.pred.tense = Tense::Present; sf.pred.aspect = Aspect::Perfect; break;
            default: sf.pred.tense = hope ? Tense::Future : Tense::Present; break;
          }
          if (hope && esT) sf.pred.mood = frame::SrcMood::Subjunctive;   // "espero que vengas"
          if (subjI >= 0) {
            sf.hasSubject = true;
            sf.subject = merged[(size_t)subjI].np;
            for (int tk : sf.subject.tokens) out.roles[(size_t)tk] = "subject (infinitive)";
            // C41: the generic article is bare only in a present statement ("I think that the war will end")
            if (sf.pred.tense != Tense::Present && sf.subject.token >= 0 && (size_t)sf.subject.token < side.lex.size())
              side.lex[(size_t)sf.subject.token].generic = false;
          } else {   // the same subject as the finite verb
            SemNP pr;
            pr.isPronoun = true;
            pr.pron.person = vt->f.person ? vt->f.person : 3;
            pr.pron.number = vt->f.number == Pl ? 2 : 1;
            pr.number = pr.pron.number;
            for (const NPInfo& m : merged)
              if (m.case_ == Nom && m.first < (int)finite) { pr = m.np; break; }
            sf.hasSubject = true;
            sf.subject = pr;
            sf.implicitSubject = pr.token < 0;
          }
          auto asPred = [&](const NPInfo& np) {
            if (np.adjOnly || (np.np.head.empty() && !np.np.adjectives.empty())) {
              for (const SemAdj& a : np.np.adjectives) sf.predAdj.push_back(a);
            } else {
              sf.predicative.push_back(np.np);
            }
            for (int tk : np.np.tokens) out.roles[(size_t)tk] = "predicate (infinitive)";
          };
          if (secondI >= 0) {
            if (einai) asPred(merged[(size_t)secondI]);
            else {
              sf.hasObject = true;
              sf.object = merged[(size_t)secondI].np;
              for (int tk : sf.object.tokens) out.roles[(size_t)tk] = "object (infinitive)";
            }
          }
          if (nomPred >= 0) asPred(merged[(size_t)nomPred]);
          // adverbs and prepositional phrases on the infinitive's side of the finite verb
          const int lo = infinitive > finite ? (int)finite : -1;
          const int hi = infinitive > finite ? (int)n : (int)finite;
          for (size_t a = 0; a < f.adverbs.size();) {
            const int tk = f.adverbs[a].token;
            if (tk > lo && tk < hi) { frame::SemAdverb av = f.adverbs[a]; av.front = false; sf.adverbs.push_back(av); f.adverbs.erase(f.adverbs.begin() + (long)a); }
            else ++a;
          }
          for (size_t a = 0; a < f.obliques.size();) {
            const int tk = f.obliques[a].token;
            if (tk > lo && tk < hi) { frame::SemOblique ob = f.obliques[a]; ob.front = false; sf.obliques.push_back(ob); f.obliques.erase(f.obliques.begin() + (long)a); }
            else ++a;
          }
          std::vector<NPInfo> keep;
          for (size_t a = 0; a < merged.size(); ++a)
            if ((long)a != subjI && (long)a != secondI && (long)a != nomPred) keep.push_back(merged[a]);
          merged.swap(keep);
          frame::SemSub sb;
          sb.relation = Relation::Complement;
          sb.frame.push_back(sf);
          f.subordinate.push_back(sb);
          out.roles[infinitive] = "infinitive (that-clause)";
          infinitive = n;
        }
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
    // C41: the genitive of comparison after a comparative predicate ("ὁ δοῦλος κρείττων ἐστὶ τοῦ δεσπότου" = stronger
    // than the master / más fuerte que el amo; was "stronger of the master")
    if (eimi) {
      auto comparative = [&](const SemAdj& a) {
        if (a.token < 0 || (size_t)a.token >= ti.size()) return false;
        const TokInfo& t = ti[(size_t)a.token];
        if (t.f.degree == Comparative) return true;
        for (const char* ck : {"κρείττων", "κρείσσων", "ἥττων", "ἥσσων", "βελτίων", "ἀμείνων", "χείρων", "ἐλάττων",
                              "ἐλάσσων", "μείζων", "πλείων", "πλέων"})
          if (t.key == text::greek_key(ck)) return true;
        return false;
      };
      bool comp = false;
      for (const SemNP& pn : f.predicative)
        for (const SemAdj& a : pn.adjectives) comp = comp || (pn.head.empty() && comparative(a));
      for (const SemAdj& a : f.predAdj) comp = comp || comparative(a);
      if (comp)
        for (frame::SemOblique& o : f.obliques)
          if (o.prep == "of" || o.prep == "de") o.prep = esT ? "que" : "than";
    }
    // C31: "αἷμα τῷ παιδὶ ῥεῖ" (blood flows to the boy) is "the boy is bleeding", "αἷμα ἐκ τῆς ῥινὸς ῥεῖ" "the nose is
    // bleeding": the person in the dative (or the body part after ἐκ) is the subject of "bleed" / "sangrar"
    bool bleed = false;
    if (vt && vkey == text::greek_key("ῥέω") && f.hasSubject && f.subject.token >= 0 &&
        ti[(size_t)f.subject.token].key == text::greek_key("αἷμα")) {
      if (f.hasIndirect) {
        f.subject = f.indirectObject;
        f.hasIndirect = false;
        bleed = true;
      } else {
        for (size_t a = 0; a < f.obliques.size(); ++a)
          if (f.obliques[a].token >= 0 && keyIs(ti[(size_t)f.obliques[a].token], {"ἐκ", "ἐξ"})) {
            f.subject = f.obliques[a].np;
            f.obliques.erase(f.obliques.begin() + (long)a);
            bleed = true;
            break;
          }
      }
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
      if (bleed) f.pred.lemma = esT ? "sangrar" : "bleed";
      uint8_t tense = v.f.tense;
      if (vkey == "οἶδα") tense = tense == Pluperfect ? Imperfect : Present;   // perfect with present meaning
      // C41: δέδοικα "I fear" (was "I have feared"), ἔοικα "it seems"
      else if ((vkey == text::greek_key("δείδω") || vkey == text::greek_key("ἔοικα")) && (tense == Perfect || tense == Pluperfect))
        tense = tense == Pluperfect ? Imperfect : Present;
      switch (tense) {
        case Imperfect: f.pred.tense = Tense::Past; f.pred.aspect = Aspect::Progressive; break;
        case Aorist: f.pred.tense = Tense::Past; break;
        case Future: f.pred.tense = Tense::Future; break;
        case Perfect: f.pred.tense = Tense::Present; f.pred.aspect = Aspect::Perfect; break;
        case Pluperfect: f.pred.tense = Tense::Past; f.pred.aspect = Aspect::Perfect; break;
        default: break;
      }
      if (mod != Modality::None && tense == Imperfect) { f.pred.aspect = Aspect::Simple; f.pred.pastModal = true; }
      // C41: the generic article (readable tag generic) is bare in English only in a general statement in the present
      // ("ἡ εἰρήνη καλή ἐστιν" = peace is beautiful, "τὴν ἀρετὴν τιμᾷ" = honours virtue); a past / future clause or a
      // prepositional phrase keeps "the" ("announced the peace", "the war ended", "I think that the war will end")
      {
        auto plain = [&](const SemNP& x) {
          if (x.token >= 0 && (size_t)x.token < side.lex.size()) side.lex[(size_t)x.token].generic = false;
        };
        for (const frame::SemOblique& o : f.obliques) plain(o.np);
        if (tense != Present || mod != Modality::None) {
          if (f.hasObject) plain(f.object);
          if (f.hasIndirect) plain(f.indirectObject);
          if (f.hasSubject) plain(f.subject);
        }
      }
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
    } else if (elliptic) {   // C38: the copula left out; the tense of the sentence's previous clause
      f.hasPred = true;
      f.pred.lemma = "be";
      f.copula = true;
      f.pred.tense = lastPredSet ? lastTense : Tense::Present;
    } else {
      f.type = f.type == Kind::Wh ? Kind::Wh : Kind::Frag;
      if (hos) f.type = Kind::Excl;
    }
    if (f.hasPred) { lastPredSet = true; lastTense = f.pred.tense; }
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
    if (f.hasPred)
      for (size_t ng : negTokens) out.spoken[ng] = 1;
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
  // C33: πάλιν + "come" is "come back" ("αὔριον πάλιν ἥξομεν" -> "We will come back tomorrow", "Volveremos mañana")
  for (Pending& c : clauses) {
    if (!c.f.hasPred) continue;
    const std::string& pl = c.f.pred.lemma;
    if (pl != "have come" && pl != "come" && pl != "haber llegado" && pl != "venir" && pl != "llegar") continue;
    for (size_t a = 0; a < c.f.adverbs.size(); ++a) {
      const int tk = c.f.adverbs[a].token;
      if (tk >= 0 && (size_t)tk < n && side.ti[(size_t)tk].key == text::greek_key("πάλιν")) {
        c.f.pred.lemma = esT ? "volver" : "come back";
        c.f.adverbs.erase(c.f.adverbs.begin() + (long)a);
        break;
      }
    }
  }
  // C33: "say a story" is "tell a story" ("μῦθον λέγει" -> "tells a story", "cuenta un cuento")
  for (Pending& c : clauses)
    if (c.f.hasPred && c.f.hasObject && c.f.object.token >= 0 && (size_t)c.f.object.token < n &&
        (c.f.pred.lemma == "say" || c.f.pred.lemma == "decir")) {
      const std::string& o = side.lex[(size_t)c.f.object.token].word;
      if (o == "story" || o == "cuento") c.f.pred.lemma = esT ? "contar" : "tell";
    }
  // C38: αὐτόν / αὐτήν whose nearest earlier noun of its gender and number in the sentence is a thing is "it" ("ὁ
  // λίθος μέγας ἐστίν· ... αὐτὸν αἴρειν" = lift it; "him" was printed)
  {
    std::vector<const SemNP*> heads;   // non-pronoun noun groups, in frame order
    auto thing = [&](SemNP& p) {
      if (!p.isPronoun || p.pronLemma != "αὐτόσ" || p.pron.number == 2 || p.pron.gender == N || p.token < 0) return;
      const SemNP* best = nullptr;
      for (const SemNP* h : heads)
        if (h->token >= 0 && h->token < p.token && !h->isName && h->number != 2) {
          const TokInfo& ht = ti[(size_t)h->token];
          const uint8_t g = ht.lgender ? ht.lgender : ht.f.gender;
          if ((g == p.pron.gender || (g && morph::genderAdmits(g, p.pron.gender))) && (!best || h->token > best->token)) best = h;
        }
      if (best && !side.lex[(size_t)best->token].person && !side.lex[(size_t)best->token].animal &&
          ti[(size_t)best->token].lpos == Noun)
        p.pron.gender = N;
    };
    for (Pending& c : clauses) {   // the clause's own subject is not the antecedent (that would be ἑαυτόν)
      SemFrame& f = c.f;
      if (f.hasObject) thing(f.object);
      if (f.hasIndirect) thing(f.indirectObject);
      for (frame::SemOblique& o : f.obliques) thing(o.np);
      for (frame::SemSub& sb : f.subordinate)
        for (SemFrame& g : sb.frame)
          if (g.hasObject) thing(g.object);
      if (f.hasSubject && !f.subject.isPronoun && f.subject.token >= 0) heads.push_back(&f.subject);
      if (f.hasObject && !f.object.isPronoun && f.object.token >= 0) heads.push_back(&f.object);
      if (f.hasIndirect && !f.indirectObject.isPronoun && f.indirectObject.token >= 0) heads.push_back(&f.indirectObject);
      for (frame::SemOblique& o : f.obliques)
        if (!o.np.isPronoun && o.np.token >= 0) heads.push_back(&o.np);
    }
  }
  // C38: a clause split at καί / ἀλλά whose subject is only the verb's ending and agrees with the previous clause's
  // subject shares it ("Dicaeopolis always works and often groans and says"): the English realiser leaves it out
  for (size_t a = 1; a < clauses.size(); ++a) {
    SemFrame& f = clauses[a].f;
    const SemFrame& g = clauses[a - 1].f;
    if (!clauses[a - 1].sepAfter.empty() || !f.hasPred || !f.implicitSubject || f.type != Kind::Decl ||
        g.type != Kind::Decl || !g.hasSubject || !g.hasPred)
      continue;
    const int gp = g.subject.isPronoun ? (g.subject.pron.person ? g.subject.pron.person : 3) : 3;
    const int gn = g.subject.isPronoun ? (g.subject.pron.number == 2 ? 2 : 1) : (g.subject.number == 2 ? 2 : 1);
    const int fp = f.subject.pron.person ? f.subject.pron.person : 3;
    const int fn = f.subject.pron.number == 2 ? 2 : 1;
    if (gp == fp && gn == fn) {
      f.discourse.push_back("same-subject");
      f.inheritedSubject.assign(1, g.subject);
      // C38: the shared object of a verb that takes one ("τὸν λίθον αἴρει καὶ ἐκ τοῦ ἀγροῦ φέρει" = lifts the stone and
      // carries it out of the field); a verb without a valency row leaves it open (a guess)
      if (!f.hasObject && g.hasObject && f.pred.token >= 0 && f.pred.modality == Modality::None && !f.copula) {
        const std::string& vk = ti[(size_t)f.pred.token].key;
        const bool saying = vk == text::greek_key("λέγω") || vk == text::greek_key("φημί") || vk == text::greek_key("ἀποκρίνομαι");
        const int acc = saying ? 0 : src.takesAccusative(vk);
        if (acc == 1) {
          SemNP pr;
          pr.isPronoun = true;
          pr.pron.person = g.object.isPronoun ? g.object.pron.person : 3;
          pr.pron.number = g.object.isPronoun ? g.object.pron.number : (g.object.number == 2 ? 2 : 1);
          pr.number = pr.pron.number;
          uint8_t gg = g.object.isPronoun ? g.object.pron.gender : 0;
          if (!g.object.isPronoun && g.object.token >= 0) {
            const Lexical& ol = side.lex[(size_t)g.object.token];
            gg = esT ? (ol.gender ? ol.gender : (uint8_t)M) : (ol.person ? ti[(size_t)g.object.token].f.gender : (uint8_t)N);
          }
          pr.pron.gender = gg;
          f.hasObject = true;
          f.object = pr;
        } else if (acc < 0) {
          out.flags.push_back("role-doubt");
        }
      }
    }
  }
  for (size_t a = 0; a < clauses.size(); ++a) {
    out.frames.push_back(clauses[a].f);
    // C38: no punctuation between two clauses: a clause split at a coordinating καί / ἀλλά (the connector is in the frame)
    out.joiners.push_back(a == 0 ? std::string() : clauses[a - 1].sepAfter == "," ? "," : clauses[a - 1].sepAfter.empty() ? " " : ";");
  }
  bool anyVerb = false;
  for (const SemFrame& f : out.frames) anyVerb = anyVerb || f.hasPred;
  if (!anyVerb) out.flags.push_back("no-verb");
  (void)lowerFirst;
  coverage(s, ti, side, out);
}

// C38: the meaning coverage of the readable sentence (calibration of OK on textbook prose: a dropped predicate
// adjective, a dropped coordinated verb, a lost negation, a dropped connector were rated OK). Every content word of the
// Greek must reach the frames that the realisers print, or be spoken outside them (a connector gloss, a negation of a
// clause with a verb, a subordinator); the function words (article, ὦ, μέν, γε, δή, τοι, τε, καί, ἤ, ἆρα) need not.
// A content word that is lost goes to `dropped` (flag "meaning-coverage"); a nominative group printed as a predicate of
// a verb other than "be" (a second subject or an apposition read wrongly: "Chloe goes the mother") is "role-doubt".
void coverage(const Sentence& s, const std::vector<TokInfo>& ti, const Side& side, Built& out) {
  const size_t n = ti.size();
  std::vector<char> in(n, 0);
  auto mark = [&](int tk) { if (tk >= 0 && (size_t)tk < n) in[(size_t)tk] = 1; };
  auto lexWord = [&](int tk) -> bool {
    return tk >= 0 && (size_t)tk < n && (!side.lex[(size_t)tk].word.empty() || side.lex[(size_t)tk].silent);
  };
  std::function<void(const SemNP&)> np = [&](const SemNP& x) {
    if (x.token >= 0 && (x.isName || x.isPronoun || !x.head.empty() || lexWord(x.token))) mark(x.token);
    if (x.isName || x.isPronoun) {   // the realisers print a name or a pronoun alone (its attributes are lost)
      for (const SemNP& y : x.coord) np(y);
      return;
    }
    for (const SemAdj& a : x.adjectives) {
      if (!a.lemma.empty()) mark(a.token);
      for (int t : a.advTokens) mark(t);
    }
    for (int t : x.tokens) {
      if (t < 0 || (size_t)t >= n) continue;
      const TokInfo& m = ti[(size_t)t];
      if (m.lpos == Article || m.lpos == Num || m.lpos == Det) mark(t);
      for (const char* k : {"οὗτοσ", "ἐκεῖνοσ", "πᾶσ", "οὐδείσ", "μηδείσ", "πόσοσ", "δύο", "τρεῖσ", "εἷσ", "τέτταρεσ",
                            "πολύσ", "ὀλίγοσ"})   // C41: many / few as determiners
        if (m.key == k) mark(t);
    }
    for (const SemNP& y : x.possessor) np(y);
    for (const SemNP& y : x.genitive) np(y);
    for (const SemNP& y : x.coord) np(y);
    for (const SemNP& y : x.apposition) np(y);
  };
  bool roleDoubt = false;
  std::function<void(const SemFrame&)> fr = [&](const SemFrame& f) {
    if (f.hasPred) {
      mark(f.pred.token);
      mark(f.pred.complementToken);
      for (int t : f.pred.auxTokens) mark(t);
    }
    if (f.hasSubject && !f.implicitSubject) np(f.subject);
    if (f.hasObject) np(f.object);
    if (f.hasIndirect) np(f.indirectObject);
    for (const SemNP& x : f.predicative) np(x);
    for (const SemNP& x : f.vocatives) np(x);
    for (const SemAdj& a : f.predAdj) {
      if (!a.lemma.empty()) mark(a.token);
      for (int t : a.advTokens) mark(t);
    }
    for (const frame::SemOblique& o : f.obliques) { mark(o.token); np(o.np); }
    for (const frame::SemAdverb& a : f.adverbs) mark(a.token);
    mark(f.wh.token);
    for (const frame::SemSub& sb : f.subordinate)
      for (const SemFrame& g : sb.frame) fr(g);
    for (const SemFrame& g : f.secondary) fr(g);
    if (f.hasPred && !f.copula && !f.predicative.empty()) roleDoubt = true;
  };
  for (const SemFrame& f : out.frames) fr(f);
  for (size_t i = 0; i < n && i < s.tokens.size(); ++i) {
    const TokInfo& t = ti[i];
    if (!t.word || s.tokens[i].unknown || s.tokens[i].readings.empty()) continue;
    if (t.lpos == Article || t.lpos == Intj) continue;
    if (keyIs(t, {"ὦ", "μέν", "γε", "δή", "τοι", "τε", "καί", "ἤ", "ἆρα", "ἄν"})) continue;
    const std::string& role = out.roles[i];
    if (role == "particle" || role == "conjunction" || role == "wh" || role == "preposition") continue;
    bool ok = in[i] != 0 || out.spoken[i] != 0;
    if (role == "connector" || role == "negation" || role == "subordinator") ok = out.spoken[i] != 0;
    else if (role == "adverb") ok = lexWord((int)i);
    if (!ok) out.dropped.push_back(t.text);
  }
  if (!out.dropped.empty()) out.flags.push_back("meaning-coverage");
  if (roleDoubt) out.flags.push_back("role-doubt");
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
  if (n.pronLemma == "own" && possessive) {   // C38: a reflexive possessor ("τὴν ἑαυτοῦ οἰκίαν" = his own house)
    SemNP x = n;
    x.pronLemma.clear();
    return enPronoun(x, false, true) + " own";
  }
  if (!n.pronLemma.empty() && n.pronLemma.find_first_of("abcdefghijklmnopqrstuvwxyz") == 0) return n.pronLemma;
  if (p == 1) return possessive ? (pl ? "our" : "my") : object ? (pl ? "us" : "me") : (pl ? "we" : "I");
  if (p == 2) return possessive ? (pl ? "your" : "your") : "you";
  if (pl) return possessive ? "their" : object ? "them" : "they";
  if (n.pron.gender == F) return possessive ? "her" : object ? "her" : "she";
  if (n.pron.gender == N) return possessive ? "its" : "it";
  return possessive ? "his" : object ? "him" : "he";
}

std::string enNP(const SemNP& n, const Side& side, bool object) {
  if (n.isPronoun && n.coord.empty()) return enPronoun(n, object, false);
  if (n.isName && n.coord.empty()) return n.head;
  if (n.isName || (n.isPronoun && !n.coord.empty())) {   // C38: a coordinated name / pronoun
    std::string o = n.isName ? n.head : enPronoun(n, object, false);
    for (const SemNP& c : n.coord) o += " " + (n.coordConj.empty() ? std::string("and") : n.coordConj) + " " + enNP(c, side, object);
    return o;
  }
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
  // C38: a generic noun with the Greek article is bare in English ("ὁ βίος χαλεπός ἐστιν" = life is hard)
  else if (n.definite && lx && lx->generic && n.adjectives.empty() && n.genitive.empty() && !pl) {}
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
  for (const SemNP& c : n.coord) { w.push_back(n.coordConj.empty() ? "and" : n.coordConj); w.push_back(enNP(c, side, object)); }
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
  // C38: a gloss "be tired" / "be here" is the copula with its complement ("Stephen is tired", not "exerts oneself")
  if (base.size() > 3 && base.compare(0, 3, "be ") == 0 && f.type != Kind::Imp && p.modality == Modality::None &&
      p.tense != Tense::Future && p.aspect != Aspect::Perfect) {
    v.push_back(beForm(a.person, a.pl, past));
    if (f.negative) v.push_back("not");
    v.push_back(base.substr(3));
    return v;
  }
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
      // C38: "cannot" is one word
      if (f.negative && std::string(m) == "can") v.push_back("cannot");
      else {
        v.push_back(m);
        if (f.negative) v.push_back("not");
      }
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
    // C31: a present-perfect gloss ("have come" for ἥκω) in the future is the plain verb: ἥξειν -> "will come"
    if (base == "have come") base = "come";
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

bool frequencyAdverb(const std::string& a) {
  return a == "always" || a == "often" || a == "never" || a == "sometimes" || a == "usually" || a == "seldom";
}
bool hasDiscourse(const SemFrame& f, const char* d) {
  return std::find(f.discourse.begin(), f.discourse.end(), d) != f.discourse.end();
}

std::string enClause(const SemFrame& f, const Side& side) {
  std::vector<std::string> w;
  for (const std::string& c : f.connectors) w.push_back(c);
  for (const std::string& ij : f.interjections) w.push_back(ij + ",");
  std::vector<std::string> front, end, mid;
  // C38: "always", "often", "never" go before the verb in a statement ("So Dicaeopolis always works"; "So always
  // Dicaeopolis toils often" was printed)
  const bool midOk = f.type == Kind::Decl && f.hasPred && !f.existential;
  // C38: a manner adverb in -ly goes after the verb group ("but they walk slowly"; "but slowly they walk")
  auto manner = [](const std::string& a) { return a.size() > 3 && a.compare(a.size() - 2, 2, "ly") == 0 && a.find(' ') == std::string::npos; };
  // C38: without its subject (shared with the previous clause) a clause-opening adverb goes after the verb ("... and
  // drives them home"; "and home drives them" was printed)
  const bool shared = hasDiscourse(f, "same-subject");
  auto preVerb = [](const std::string& a) {   // "and no longer works", "and still works"
    return a.compare(0, 3, "no ") == 0 || a == "still" || a == "already" || a == "then" || a == "now" || a == "again" || a == "also";
  };
  for (const frame::SemAdverb& a : f.adverbs)
    (midOk && (frequencyAdverb(a.lemma) || (shared && preVerb(a.lemma))) ? mid
     : a.front && !(midOk && manner(a.lemma)) && !shared ? front : end).push_back(a.lemma);
  for (const std::string& x : front) w.push_back(x);
  const bool vocAfter = std::find(f.discourse.begin(), f.discourse.end(), "vocative-after") != f.discourse.end();
  auto voc = [&](const SemNP& v) {
    SemNP x = v;
    x.definite = false;
    std::string np = enNP(x, side, false);
    for (const char* a : {"a ", "an "})
      if (np.compare(0, std::char_traits<char>::length(a), a) == 0) np = np.substr(std::char_traits<char>::length(a));
    // C38: a god addressed with ὦ is "O Zeus" (names_grc.tsv note "god")
    if (x.isName && x.token >= 0 && (size_t)x.token < side.ti.size() && side.ti[(size_t)x.token].god) np = "O " + np;
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
  // C38: a coordinated clause sharing the previous clause's subject leaves it out ("... works and often groans")
  const std::string subj = f.hasSubject && !hasDiscourse(f, "same-subject") ? enNP(f.subject, side, false) : std::string();
  // objects
  std::string obj = f.hasObject ? enNP(f.object, side, true) : std::string();
  std::string io = f.hasIndirect ? enNP(f.indirectObject, side, true) : std::string();
  const bool ioFirst = f.hasIndirect && f.indirectObject.isPronoun && f.hasObject;
  std::vector<std::string> rest;
  if (ioFirst) rest.push_back(io);
  if (!obj.empty()) rest.push_back(obj);
  if (f.hasIndirect && !ioFirst) rest.push_back(f.hasObject || f.type != Kind::Imp ? "to " + io : io);
  // C31: a catenative complement ("ἐλπίζομεν νικήσειν" -> "we hope to win"; it was left out): "to" + the verb, after a
  // person object of a verb of commanding or teaching ("κελεύω σε ἐλθεῖν" -> "I order you to come"), else before the
  // objects (they belong to the infinitive: "we hope to win the war")
  if (f.hasPred && f.pred.modality == Modality::None && !f.pred.complementVerb.empty()) {
    const std::string comp = "to " + f.pred.complementVerb;
    bool after = false;
    if (f.pred.token >= 0 && (size_t)f.pred.token < side.ti.size()) {
      const std::string& vk = side.ti[(size_t)f.pred.token].key;
      for (const char* g : {"κελεύω", "ἐάω", "κωλύω", "διδάσκω", "πείθω", "ἀναγκάζω", "αἰτέω", "λέγω", "παρακαλέω"})
        after = after || vk == text::greek_key(g);
    }
    if (after) rest.push_back(comp);
    else rest.insert(rest.begin(), comp);
  }
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
  for (const std::string& d : f.discourse)   // C38: "provides food, not much but enough"
    if (d.compare(0, 5, "tail:") == 0) { rest.push_back(","); rest.push_back(d.substr(5)); }
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
    // C38: a frequency adverb after "be" / an auxiliary (and "not"), else before the verb
    static const char* const kAux[] = {"is", "are", "am", "was", "were", "will", "can", "could", "must", "may", "does",
                                       "do", "did", "has", "have", "had", nullptr};
    bool aux = false;
    if (!vg.empty() && (vg.size() > 1 || f.pred.lemma == "be"))
      for (const char* const* x = kAux; *x; ++x) aux = aux || vg[0] == *x;
    size_t at = 0;
    if (aux) at = (vg.size() > 1 && vg[1] == "not") ? 2 : 1;
    for (size_t i = 0; i < vg.size(); ++i) {
      if (i == at) for (const std::string& m : mid) w.push_back(m);
      w.push_back(vg[i]);
    }
    if (at >= vg.size()) for (const std::string& m : mid) w.push_back(m);
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
    if (!out.empty()) out += (b.joiners[i] == "," ? ", " : b.joiners[i] == " " ? " " : "; ");
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

std::string esPossessive(const SemNP& p, bool pl, uint8_t g = 0) {
  const int person = p.pron.person ? p.pron.person : 3;
  const bool ppl = p.pron.number == 2;
  if (person == 1 && ppl && g == F) return pl ? "nuestras" : "nuestra";   // C41: nuestra casa
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
  if (!n.coord.empty()) {   // C38: a coordinated group ("Chloe y la madre")
    SemNP first = n;
    first.coord.clear();
    std::string o = esNP(first, side);
    for (const SemNP& c : n.coord) o += " " + (n.coordConj.empty() ? std::string("y") : n.coordConj) + " " + esNP(c, side);
    return o;
  }
  if (n.isPronoun) return esSubjectPronoun(n);
  if (n.isName) return n.head;
  std::vector<std::string> w;
  const uint8_t g = esGender(n, side);
  const bool pl = n.number == 2;
  std::string noun = n.head;
  if (pl && !noun.empty()) {   // C40: a readable plural of its own (noun-pl:) for a multi-word noun
    const Lexical* lx = lexAt(side, n.token);
    noun = lx && !lx->plural.empty() ? lx->plural : es::plural(noun);
  }
  if (!n.possessor.empty()) {
    w.push_back(esPossessive(n.possessor[0], pl, g));
    if (n.possessor[0].pronLemma == "own") w.push_back(es::adjective("propio", g == F ? F : M, pl ? 2 : 1));   // C38
  }
  else if (n.determiner == "all") { w.push_back(g == F ? "todas" : "todos"); w.push_back(esArticle(g, true, true, noun)); }
  else if (n.determiner == "every") w.push_back(g == F ? "cada" : "cada");
  else if (n.determiner == "many") w.push_back(g == F ? "muchas" : "muchos");   // C41
  else if (n.determiner == "few") w.push_back(g == F ? "pocas" : "pocos");
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
  // C31: the that-clause after "esperar" ("espero que vengas"): the subjunctive
  if (p.mood == frame::SrcMood::Subjunctive)
    return p.tense == Tense::Past ? es::VTense::SubjImperfect : es::VTense::SubjPresent;
  if (p.tense == Tense::Future) return es::VTense::Future;
  if (p.tense == Tense::Past) return p.aspect == Aspect::Progressive || p.pastModal ? es::VTense::Imperfect : es::VTense::Preterite;
  return es::VTense::Present;
}

std::string esClause(const SemFrame& f, const Side& side) {
  std::vector<std::string> w;
  for (const std::string& c : f.connectors) w.push_back(c);
  for (const std::string& ij : f.interjections) w.push_back(ij + ",");
  std::vector<std::string> front, end;
  const bool shared = hasDiscourse(f, "same-subject");   // C38: "y lo lleva a casa"
  for (const frame::SemAdverb& a : f.adverbs) (a.front && !shared ? front : end).push_back(a.lemma);
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
    else number = f.subject.number == 2 || !f.subject.coord.empty() ? 2 : 1;
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
  // C31: "haber llegado" (ἥκω) in the future or the subjunctive is the plain verb ("que vengas", "llegarás")
  if ((f.pred.tense == Tense::Future || f.pred.mood == frame::SrcMood::Subjunctive) && verb.compare(0, 6, "haber ") == 0 &&
      verb.size() > 6) {
    const std::string pp = verb.substr(6);
    if (pp.size() > 3 && pp.compare(pp.size() - 3, 3, "ado") == 0) verb = pp.substr(0, pp.size() - 3) + "ar";   // llegado
  }
  // ser / estar: location, "where", no predicate -> estar
  if (f.copula) {
    const bool location = (f.predicative.empty() && f.predAdj.empty() && f.wh.role != frame::Role::Predicate) ||
                          f.wh.word == "where";
    // C33: adjectives of a state take estar ("La comida está lista"; "es lista" is "she is clever")
    bool state = !f.predAdj.empty() && f.predicative.empty();
    for (const SemAdj& a : f.predAdj) {
      bool st = false;
      for (const char* sw : {"listo", "enfermo", "cansado", "contento", "lleno", "vacío", "abierto", "cerrado", "vivo",
                            "muerto", "despierto", "dormido", "sentado", "sano", "solo", "ocupado", "preparado",
                            "tranquilo", "enojado", "sucio", "limpio", "mojado", "seco", "roto"})
        st = st || a.lemma == sw;
      state = state && st;
    }
    verb = location || state ? "estar" : "ser";
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
    // C31: a catenative complement ("ἐλπίζομεν νικήσειν" -> "esperamos ganar")
    if (p.modality == Modality::None && !p.complementVerb.empty()) vg.push_back(p.complementVerb);
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
    // C38: also after "no" / "sino" ("sino al campo")
    const size_t sp = prep.rfind(' ');
    const std::string last = sp == std::string::npos ? prep : prep.substr(sp + 1);
    const std::string lead = sp == std::string::npos ? std::string() : prep.substr(0, sp + 1);
    if ((last == "a" || last == "de") && x.compare(0, 3, "el ") == 0) rest.push_back(lead + (last == "a" ? "al " : "del ") + x.substr(3));
    else rest.push_back(prep + " " + x);
  }
  for (const std::string& x : end) rest.push_back(x);
  for (const std::string& d : f.discourse)   // C38: "da comida, no mucha sino suficiente"
    if (d.compare(0, 5, "tail:") == 0) { rest.push_back(","); rest.push_back(d.substr(5)); }
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
    if (!out.empty()) out += (b.joiners[i] == "," ? ", " : b.joiners[i] == " " ? " " : "; ");
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
