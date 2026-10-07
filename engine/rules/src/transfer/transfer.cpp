// Transfer: SemFrame -> LaClause (DESIGN.md §10.2-10.3). Lexical selection through REVX with the §10.2 scoring,
// closed classes by the tables in tables.cpp, prepositions from preps_en_la.tsv, valency through the realiser's
// CaseAssigner (valency_la.tsv), periphrasis_la.tsv for fidelity 2/3, names through the glossary / names_la.tsv.
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "frame/english.h"
#include "tables.h"
#include "vp/morph.h"
#include "vp/text.h"
#include "vp/transfer.h"

namespace vp::transfer {

using namespace vp::feat;
using frame::Kind;
using frame::Modality;
using frame::Relation;
using frame::SemFrame;
using frame::SemNP;
using frame::SemSentence;
using realise::LaAdj;
using realise::LaClause;
using realise::LaNP;
using realise::LaOblique;

namespace {

bool posCompatible(uint8_t latinPos, uint8_t want) {
  switch (want) {
    case Noun: return latinPos == Noun;
    case Name: return latinPos == Name || latinPos == Noun;
    case Verb: return latinPos == Verb;
    case Adj: return latinPos == Adj || latinPos == Num || latinPos == Participle || latinPos == Det;   // alius (C15)
    case Adv: return latinPos == Adv || latinPos == Particle;
    case Intj: return latinPos == Intj;
    default: return latinPos == want;
  }
}

std::vector<std::string> words(const std::string& s) {
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

// C15: the American spelling of a British word (harbour -> harbor, centre -> center, realise -> realize); empty
// when there is none. The reverse index is keyed by the lexicon's glosses, mostly American.
std::string americanSpelling(const std::string& w) {
  std::string a = w;
  auto rep = [&](const char* from, const char* to) {
    const size_t at = a.find(from);
    if (at != std::string::npos && at > 0) a.replace(at, std::char_traits<char>::length(from), to);
  };
  rep("our", "or");
  if (a.size() > 4 && a.compare(a.size() - 2, 2, "re") == 0 && a.find_first_of("aeiou", a.size() - 4) != std::string::npos &&
      a[a.size() - 3] != 'e' && a[a.size() - 3] != 'u')
    a = a.substr(0, a.size() - 2) + "er";
  rep("ise", "ize");
  rep("yse", "yze");
  if (w == "grey") a = "gray";
  if (w == "plough") a = "plow";
  return a == w ? std::string() : a;
}

// C15: loose identity of an English word with a gloss word (plural -s / -es, British spelling).
bool sameWord(const std::string& a, const std::string& b) {
  if (a == b) return true;
  auto stem = [](std::string x) {
    if (x.size() > 4 && x.compare(x.size() - 3, 3, "ies") == 0) return x.substr(0, x.size() - 3) + "y";
    if (x.size() > 4 && x.compare(x.size() - 2, 2, "es") == 0 &&
        (x[x.size() - 3] == 's' || x[x.size() - 3] == 'x' || x[x.size() - 3] == 'h')) return x.substr(0, x.size() - 2);
    if (x.size() > 3 && x.back() == 's' && x[x.size() - 2] != 's') return x.substr(0, x.size() - 1);
    return x;
  };
  const std::string sa = stem(a), sb = stem(b);
  if (sa == sb) return true;
  const std::string aa = americanSpelling(sa), ab = americanSpelling(sb);
  return (!aa.empty() && aa == sb) || (!ab.empty() && ab == sa) || (!aa.empty() && aa == ab);
}

// C15: sense head-word weighting. Where does `word` stand in the sense gloss? 2 = head of the first gloss item
// ("harbor, port" for harbor), 1 = head of a later item, -1 = only inside an item as a modifier ("port duty ... of
// public harbours" for harbour), 0 = not in the gloss. The head of a noun / adjective item is its last word before a
// preposition or relative word; of a verb item, the first word after "to".
int glossHead(const std::string& gloss0, const std::string& word, uint8_t pos) {
  std::string gloss = text::lower(gloss0);
  // drop parentheses
  std::string g;
  int depth = 0;
  for (char ch : gloss) {
    if (ch == '(' || ch == '[') { ++depth; continue; }
    if (ch == ')' || ch == ']') { if (depth) --depth; continue; }
    if (!depth) g += ch;
  }
  bool seen = false;
  int item = 0;
  size_t a = 0;
  while (a <= g.size()) {
    size_t b = g.find_first_of(",;:", a);
    if (b == std::string::npos) b = g.size();
    std::string it = g.substr(a, b - a);
    a = b + 1;
    std::vector<std::string> ws;
    {
      std::string cur;
      for (char ch : it) {
        if ((ch >= 'a' && ch <= 'z') || ch == '-' || ch == '\'') cur += ch;
        else if (!cur.empty()) { ws.push_back(cur); cur.clear(); }
      }
      if (!cur.empty()) ws.push_back(cur);
    }
    while (!ws.empty() && (ws[0] == "to" || ws[0] == "a" || ws[0] == "an" || ws[0] == "the" || ws[0] == "one's" ||
                           ws[0] == "be" || ws[0] == "become"))
      ws.erase(ws.begin());
    if (ws.empty()) { if (b >= g.size()) break; continue; }
    ++item;
    for (const std::string& w : ws) seen = seen || sameWord(w, word);
    std::string head;
    if (pos == Verb) head = ws[0];
    else {
      static const char* const kStop[] = {"of", "for", "in", "on", "with", "by", "to", "from", "at", "that", "which",
                                          "who", "used", "paid", "made", "as", "into", "between", "having"};
      size_t end = ws.size();
      for (size_t i = 1; i < ws.size(); ++i) {
        bool stop = false;
        for (const char* k : kStop) stop = stop || ws[i] == k;
        if (stop) { end = i; break; }
      }
      head = ws[end - 1];
      // "harbor or port": both words of a two-word "or" item are heads
      if (end < ws.size() && (ws[end] == "or" || ws[end] == "and") && end + 1 < ws.size() && sameWord(ws[end + 1], word))
        head = ws[end + 1];
    }
    if (sameWord(head, word)) return item == 1 ? 2 : 1;
    if (b >= g.size()) break;
  }
  return seen && pos != Verb ? -1 : 0;   // verbs: "take care of" still names "care"
}

// C22: "important" as a genitive of quality: magnī mōmentī, "very important" maximī mōmentī (defined after the class)
uint8_t simpleGender(uint8_t g) {
  switch (g) {
    case M: case F: case N: return g;
    case FN: return F;
    default: return M;
  }
}

}  // namespace

namespace {
// C17: "what X looks like" (an oblique "like" whose noun is the interrogative "what" on look / seem / be)
int whatLike(const SemFrame& f) {
  if (!f.hasPred || !(f.pred.lemma == "look" || f.pred.lemma == "seem" || f.pred.lemma == "be")) return -1;
  for (size_t i = 0; i < f.obliques.size(); ++i)
    if (f.obliques[i].prep == "like" && f.obliques[i].np.isPronoun &&
        (f.obliques[i].np.pronLemma == "what" || f.obliques[i].np.wh == "what"))
      return (int)i;
  return -1;
}
}  // namespace

bool animate(const SemNP& np) {
  if (np.isName) return true;
  if (np.isPronoun) return np.pronLemma != "it" && np.pronLemma != "this" && np.pronLemma != "that" &&
                           np.pronLemma != "something" && np.pronLemma != "everything" && np.pronLemma != "nothing" &&
                           np.pronLemma != "what" && np.pronLemma != "which" && np.pronLemma != "ello";
  return tables::animateNoun(np.head);
}

namespace {
// C20: "child" / "my child" in address (defined with Transfer::vocative)
bool childAddress(const Transfer& t, const SemNP& n, const Settings& st, LaNP& x, ClauseOut& out, uint8_t addressee = 0);

// C22: a child addressed ("my dear child") is puella / puer by the person addressed (a name in the cue), else by
// the main character's gender
void addresseeNoun(const Transfer& t, const SemNP& n, const Settings& st, const Memory& mem, LaNP& x);

// C22: "important" as a genitive of quality: magnī mōmentī ("very / awfully important": maximī mōmentī)
LaNP momentNP(const Transfer& t, bool very) {
  LaNP g;
  g.head = t.latin("mōmentum", Noun);
  if (g.head == kNone) { g.fixed = very ? "maximī mōmentī" : "magnī mōmentī"; return g; }
  LaAdj a;
  a.lemma = t.latin("magnus", Adj);
  a.degree = very ? (uint8_t)Superlative : (uint8_t)0;
  a.before = true;
  if (a.lemma != kNone) g.adjectives.push_back(a);
  return g;
}
}  // namespace

// C24: an invented English word made of a preposition and a known noun ("overcloud" = over + cloud, "underbridge"):
// the Latin preposition, the noun and its case; the choices are recorded and the flag derived-word (Check) is set.
// False when no such reading exists.
bool inventedPrep(const Transfer& t, const std::string& low, const Settings& st, const std::vector<std::string>& context,
                  int token, ClauseOut& out, uint32_t& prep, uint32_t& noun, uint8_t& cs) {
  if (st.lang != frame::SrcLang::En) return false;
  struct Pre { const char* en; const char* la; uint8_t cs; };
  static const Pre kPre[] = {{"under", "sub", Abl}, {"over", "super", Acc}, {"beyond", "ultrā", Acc},
                             {"behind", "post", Acc}, {"inside", "intrā", Acc}, {"outside", "extrā", Acc}};
  for (const Pre& p : kPre) {
    const size_t pl = std::strlen(p.en);
    if (low.size() < pl + 3 || low.compare(0, pl, p.en) != 0) continue;
    const std::string rest = low.substr(pl);
    Choice c2;
    c2.token = token;
    const uint32_t nid = t.select(rest, Noun, context, false, false, st, c2);
    const uint32_t pid = t.latin(p.la, Prep);
    if (nid == kNone || pid == kNone) continue;
    c2.source = low;
    c2.note = std::string("invented word read as \"") + p.en + " " + rest + "\"";
    c2.kind = "table";
    out.choices.push_back(c2);
    Choice c3;
    c3.token = token;
    c3.source = p.en;
    c3.lemma = pid;
    c3.kind = "table";
    out.choices.push_back(c3);
    out.flags.push_back("derived-word");
    prep = pid;
    noun = nid;
    cs = p.cs;
    return true;
  }
  return false;
}

// ---- context ------------------------------------------------------------------------------------------------------
struct Transfer::Ctx {
  const SemSentence& s;
  const Settings& st;
  Memory& mem;
  ClauseOut& out;
  std::vector<std::string> context;   // content lemmas of the clause
  const SemFrame* frame = nullptr;
  bool motion = false;
  bool negative = false;
  bool question = false;
  uint8_t subjPerson = 3, subjNumber = 1, subjGender = 0;
  uint32_t forcedVerb = kNone;          // a verb decided by the caller (state adjectives)
  bool objectInVerb = false;            // "tener miedo" -> timeō: the object noun is part of the verb
  std::string prepOverride;             // a PP whose Latin preposition the verb fixes (depend on -> ex)
  std::string prepOverrideLatin;        // verbprep_en_la.tsv frame prep:<latin>+<case>
  uint8_t prepOverrideCase = 0;
  bool reflObject = false;              // phrasal_en_la.tsv frame refl: "bow" -> inclīnāte vōs
  bool routeObject = false;             // "which way (do I go)": the way is the route (ablative), not an object
  uint8_t phrasalCase = 0;              // phrasal_en_la.tsv frame acc/dat/abl: case of the object
  bool negConsumed = false;             // an NP carries the clause's negation ("neque ... neque")
  bool afterSi = false;                 // C15: inside a sī / nisi clause: "anything" is quid, not aliquid
  // C20: the noun a standalone possessive pronoun stands for ("taller than yours": the compared subject); 0 = the last
  // noun mentioned (Memory::lastGender / lastNumber)
  uint8_t possRefGender = 0, possRefNumber = 0;
  Ctx(const SemSentence& ss, const Settings& t, Memory& m, ClauseOut& o) : s(ss), st(t), mem(m), out(o) {}
  void cover(int tok) { if (tok >= 0) out.covered.push_back(tok); }
  void cover(const std::vector<int>& v) { out.covered.insert(out.covered.end(), v.begin(), v.end()); }
};

Transfer::Transfer(const lex::Lexicon& la, const curated::CuratedData& cd) : la_(la), cd_(cd) {}

// A Latin verb phrase of the phrasebook ("fierī nōn potest", "nōn cūrō") as a clause: nōn -> negation, an
// infinitive -> the verb, a finite verb -> the modal (or the verb when there is no infinitive). False when a word
// is neither.
bool Transfer::latinVerbPhrase(const std::string& latin, LaClause& rc) const {
  rc = LaClause{};
  uint32_t fin = kNone, inf = kNone;
  for (const std::string& w : words(latin)) {
    if (text::latin_key(w) == "non") { rc.polarity = realise::Polarity::Neg; continue; }
    morph::Token mt;
    morph::analyseLatin(la_, w, mt);
    uint32_t f = kNone, i = kNone;
    for (const lex::Analysis& a : mt.analyses) {
      if (la_.lemma(a.lemma).pos != Verb) continue;
      const Features ft = unpack(la_.feature(a.feat));
      if (ft.mood == Infinitive && i == kNone) i = a.lemma;
      else if (ft.person && f == kNone) f = a.lemma;
    }
    if (i != kNone && inf == kNone) inf = i;
    else if (f != kNone && fin == kNone) fin = f;
    else return false;
  }
  if (fin == kNone) return false;
  rc.pred.lemma = inf != kNone ? inf : fin;
  if (inf != kNone) rc.pred.modal = fin;
  return true;
}

// A noun whose lexicon has plural cells only ("tenebrae") although the lemma lacks the plural-only flag.
bool Transfer::pluralOnly(uint32_t lemma) const {
  std::string f;
  return !morph::generate(la_, lemma, morph::nounForm(Nom, Sg), f, false) &&
         morph::generate(la_, lemma, morph::nounForm(Nom, Pl), f, false);
}

uint32_t Transfer::latin(const char* head, uint8_t pos) const {
  std::string key = std::string(head) + "#" + std::to_string((int)pos);
  auto it = std::lower_bound(cache_.begin(), cache_.end(), key,
                             [](const std::pair<std::string, uint32_t>& e, const std::string& k) { return e.first < k; });
  if (it != cache_.end() && it->first == key) return it->second;
  uint32_t id = morph::findLemma(la_, head, pos);
  if (id == kNone && pos) id = morph::findLemma(la_, head);
  if (cache_.size() < 1024) cache_.insert(it, {std::move(key), id});   // bounded: closed-class words only
  return id;
}

// ---- lexical selection ----------------------------------------------------------------------------------------------
uint32_t Transfer::select(const std::string& sourceLemma, uint8_t pos, const std::vector<std::string>& context,
                          bool hasObject, bool personObject, const Settings& st, Choice& c, uint8_t srcGender) const {
  c.source = sourceLemma;
  c.candidates.clear();
  c.lemma = kNone;
  c.kind = "sense";
  c.registerTag.clear();
  const bool es = st.lang == frame::SrcLang::Es;
  auto keyOf = [&](const std::string& w) { return es ? "es:" + text::es_key(w) : text::en_key(w); };
  std::vector<lex::Candidate> raw;
  la_.reverse(keyOf(sourceLemma), raw);
  if (raw.empty() && es) la_.reverse("es:" + text::es_bare(sourceLemma), raw);
  // C15: British spelling -> also the American key (harbour -> harbor); candidates merged, best score per lemma
  if (!es) {
    const std::string us = americanSpelling(text::lower(sourceLemma));
    if (!us.empty()) {
      std::vector<lex::Candidate> more;
      la_.reverse(text::en_key(us), more);
      raw.insert(raw.end(), more.begin(), more.end());
      std::stable_sort(raw.begin(), raw.end(), [](const lex::Candidate& x, const lex::Candidate& y) { return x.score > y.score; });
    }
  }
  // C23: the register tags of a lemma's first sense (taught lemmas take sense 0)
  auto firstSenseTags = [&](uint32_t id) {
    std::vector<lex::Sense> ss;
    la_.senses(id, ss);
    return ss.empty() ? (uint16_t)0 : ss[0].tags;
  };
  // teacher glosses: tier rows whose note lists the source lemma ("hole" -> fovea; data/curated/tiers_la.tsv)
  std::vector<const curated::TierEntry*> taught;
  if (!es) cd_.glossTiers(text::lower(sourceLemma), taught);
  // Spanish (C13): gloss_es_la.tsv rows listing the word are the teacher's Spanish reverse index ("pelota" -> pila)
  std::vector<const curated::GlossEsEntry*> taughtEs;
  if (es) cd_.glossEsLemmas(text::nfc(text::lower(sourceLemma)), taughtEs);
  // English pivot (C13): a Spanish word without a Latin candidate and without a teacher gloss is looked up through
  // the one-line English gloss of its Spanish lexicon entry (when the library carries one); else it stays unknown
  std::string pivotVia;
  if (es && raw.empty() && taughtEs.empty() && st.srcLex) {
    std::vector<lex::Analysis> an;
    st.srcLex->lookup(text::es_key(sourceLemma), an);
    for (const lex::Analysis& a : an) {
      const lex::Lemma sl = st.srcLex->lemma(a.lemma);
      if (sl.id == kNone || sl.glossEn.empty()) continue;
      // the first gloss item: "door; gate (of a city)" -> door
      std::string g = text::lower(std::string(sl.glossEn));
      const size_t cut = g.find_first_of(";,(");
      if (cut != std::string::npos) g = g.substr(0, cut);
      while (!g.empty() && g.back() == ' ') g.pop_back();
      if (g.compare(0, 3, "to ") == 0) g = g.substr(3);
      if (g.empty() || g.find(' ') != std::string::npos) continue;
      la_.reverse(text::en_key(g), raw);
      cd_.glossTiers(g, taught);
      if (!raw.empty() || !taught.empty()) { pivotVia = g; break; }
    }
  }
  auto isTaught = [&](const lex::Lemma& l) {
    for (const curated::TierEntry* te : taught)
      if (te->key == l.key && te->pos == curated::CuratedData::tierPos(l.pos)) return true;
    for (const curated::GlossEsEntry* ge : taughtEs)
      if (ge->key == l.key && (ge->head.empty() || text::nfc(ge->head) == text::nfc(std::string(l.head)))) return true;
    return false;
  };
  // the teacher's gloss outweighs one tier step at every fidelity except the faithful one
  const double taughtBonus = st.fidelity >= 3 ? 1.0 : st.fidelity == 2 ? 0.5 : 0.1;
  struct Scored { uint32_t lemma; uint16_t sense; double score; std::string why; uint8_t tier; bool kwHit; double base;
                  uint16_t tags = 0; };
  std::vector<Scored> sc;
  std::vector<lex::Sense> senses;
  // the best reverse-index score of a compatible lemma: fidelity 3 prefers low tiers only among exact senses
  // C23 (D18): with latinity "wide" a Medieval / Late / ecclesiastical or New Latin sense is no penalty: the
  // build-time -30 of the reverse index (applied once, for any of rare / archaic / poetic / medieval / New Latin) is
  // given back when the late tag is the only register tag of the sense. Integer score units, so ties stay exact.
  std::vector<lex::Sense> lateProbe;
  auto lateBonus = [&](const lex::Candidate& k) {
    if (st.classical) return 0;
    lateProbe.clear();
    la_.senses(k.lemma, lateProbe);
    if (k.sense >= lateProbe.size()) return 0;
    const uint16_t tg = lateProbe[k.sense].tags;
    return (tg & (kSenseMedieval | kSenseNewLatin)) && !(tg & kSenseRareArchaicPoetic) ? 30 : 0;
  };
  double topBase = 0;
  for (const lex::Candidate& k : raw) {
    const lex::Lemma l = la_.lemma(k.lemma);
    if (l.id != kNone && posCompatible(l.pos, pos)) topBase = std::max(topBase, (k.score + lateBonus(k)) / 255.0);
  }
  auto tierTerm = [&](uint8_t tier, std::string& why, double base = 1.0) {
    double tt = 0;
    if (st.fidelity == 2) tt = -0.25 * std::max(0, tier - 2);
    else if (st.fidelity >= 3) {
      // style target 1.2: "T1 whenever the sense is exact": a tier 1 word of a weak sense ("hole" -> ōs) does not
      // beat the word of the sense; weak senses (base < 0.7 of the best) lose one more tier step
      tt = -0.5 * std::max(0, tier - 1);
      if (base < 0.7 * topBase) { tt -= 0.5; why += ", weak sense"; }
    }
    if (st.fidelity >= 2 && tier == 1) tt += 0.1;   // style target: T1 whenever the sense is exact
    if (tt != 0) why += ", tier " + std::to_string(tier);
    return tt;
  };
  auto correction = [&](const lex::Lemma& l, double& s, std::string& why) {   // corrections memory (+1.0)
    if (!st.context) return;
    for (const rules::Correction& cr : st.context->corrections)
      if ((es ? text::es_key(cr.sourceKey) == text::es_key(sourceLemma) : text::en_key(cr.sourceKey) == text::en_key(sourceLemma)) &&
          text::latin_key(cr.target) == std::string(l.key)) {
        s += 1.0;
        why += ", correction";
      }
  };
  for (const lex::Candidate& k : raw) {
    const lex::Lemma l = la_.lemma(k.lemma);
    if (l.id == kNone || !posCompatible(l.pos, pos)) continue;
    if ((l.flags & lex::ProperName) && pos != Name) continue;
    if (l.head.find(' ') != std::string_view::npos) continue;   // multi-word lemmas are phrasebook business
    bool dup = false;
    for (const Scored& x : sc)
      if (x.lemma == k.lemma) dup = true;
    if (dup) continue;   // candidates come best first: keep the best sense of each lemma
    const int late = lateBonus(k);   // C23
    const double kbase = (k.score + late) / 255.0;
    double s = kbase;
    std::string why = "base " + std::to_string(k.score);
    if (late) why += ", late Latin accepted";
    // tier term (fidelity 1: none; 2: -0.25 per tier above 2; 3: -0.5 per tier above 1); T1 preference. The curated
    // tier (key + part of speech) wins over the lexicon's own.
    const uint8_t tier = cd_.effectiveTier(l.key, l.pos, l.tier);
    s += tierTerm(tier, why, isTaught(l) ? 1.0 : kbase);
    if (l.flags & lex::Defective) { s -= 0.2; why += ", defective"; }
    if (pos == Adj && l.pos == Participle) { s -= 0.05; why += ", participle"; }
    if (srcGender && l.pos == Noun && simpleGender(l.gender) == srcGender && l.gender != MF && l.gender != MFN) {
      s += 0.15;   // Spanish hijo -> fīlius, niña -> puella
      why += ", gender";
    }
    if (isTaught(l)) { s += taughtBonus; why += ", teacher gloss"; }
    // sense keywords overlapping the clause's other lemmas
    bool kwHit = false;
    senses.clear();
    la_.senses(k.lemma, senses);
    if (k.sense < senses.size()) {
      const lex::Sense& se = senses[k.sense];
      // C15: head-word weighting: the source word as the head of the gloss outweighs the word as a modifier
      if (!es) {
        const int hw = glossHead(std::string(se.glossEn), text::lower(sourceLemma), pos == Name ? (uint8_t)Noun : pos);
        if (hw == 2) { s += 0.05; why += ", gloss head"; }
        else if (hw == -1) { s -= 0.3; why += ", gloss modifier only"; }
      }
      double ov = 0;
      for (const std::string& kw : words(std::string(se.keywords))) {
        if (kw == sourceLemma) kwHit = true;
        if (es && k.score >= 51) kwHit = true;   // a Spanish keyword's candidates name the word itself (C13)
        for (const std::string& cx : context)
          if (kw == cx && cx != sourceLemma) ov += 0.1;
      }
      ov = std::min(ov, 0.2);
      if (ov > 0) { s += ov; why += ", sense overlap"; }
      if (pos == Verb) {
        const uint16_t tg = se.tags;
        if ((tg & 1u) && hasObject) { s += 0.1; why += ", transitive"; }
        if ((tg & 2u) && !hasObject) { s += 0.05; why += ", intransitive"; }
        // C15: an intransitive sense cannot take the clause's object ("He can't hurt the straw": not doleō)
        if ((tg & 2u) && !(tg & 1u) && hasObject) { s -= 0.25; why += ", intransitive with an object"; }
        if ((tg & (1u << 8)) && personObject) { s += 0.05; why += ", with-dat"; }
      }
    }
    correction(l, s, why);
    sc.push_back(Scored{k.lemma, k.sense, s, why, tier, kwHit || isTaught(l), kbase,
                        k.sense < senses.size() ? senses[k.sense].tags : (uint16_t)0});
  }
  // taught lemmas the reverse index does not list for this word ("smile" -> rīdeō, "bottom" -> īmum): base 0.5
  for (const curated::TierEntry* te : taught) {
    uint8_t lp = 0;
    for (uint8_t p : {Noun, Verb, Adj, Adv, Pron, Num, Prep, Conj, Intj, Det, Particle})
      if (te->pos == curated::CuratedData::tierPos(p)) lp = p;
    if (!lp) continue;
    const bool substantive = pos == Noun && lp == Adj;   // "the bottom" -> īmum (neuter adjective as a noun)
    if (!posCompatible(lp, pos) && !substantive) continue;
    const uint32_t id = morph::findLemma(la_, te->head, lp);
    if (id == kNone) continue;
    bool dup = false;
    for (const Scored& x : sc)
      if (x.lemma == id) dup = true;
    if (dup) continue;
    const lex::Lemma l = la_.lemma(id);
    std::string why = "teacher gloss (tiers_la.tsv: " + te->note + ")";
    const uint8_t tier = cd_.effectiveTier(l.key, l.pos, l.tier);
    double s = 0.5 + taughtBonus + tierTerm(tier, why);
    if (substantive) { s -= 0.05; why += ", adjective as noun"; }
    correction(l, s, why);
    sc.push_back(Scored{id, 0, s, why, tier, true, 0.5, firstSenseTags(id)});
  }
  // Spanish teacher glosses the reverse index does not list for this word (gloss_es_la.tsv has no part-of-speech
  // column: the Latin head is looked up with the part of speech asked for, an adjective may stand for a noun)
  for (const curated::GlossEsEntry* ge : taughtEs) {
    uint32_t id = morph::findLemma(la_, ge->head, pos == Name ? (uint8_t)Noun : pos);
    bool substantive = false;
    if (id == kNone && pos == Adj) id = morph::findLemma(la_, ge->head, Participle);
    if (id == kNone && pos == Adv) id = morph::findLemma(la_, ge->head, Particle);
    if (id == kNone && pos == Noun) { id = morph::findLemma(la_, ge->head, Adj); substantive = id != kNone; }
    if (id == kNone) continue;
    bool dup = false;
    for (const Scored& x : sc)
      if (x.lemma == id) dup = true;
    if (dup) continue;
    const lex::Lemma l = la_.lemma(id);
    std::string why = "teacher gloss (gloss_es_la.tsv: " + ge->glossEs + ")";
    const uint8_t tier = cd_.effectiveTier(l.key, l.pos, l.tier);
    double s = 0.5 + taughtBonus + tierTerm(tier, why);
    if (substantive) { s -= 0.05; why += ", adjective as noun"; }
    correction(l, s, why);
    sc.push_back(Scored{id, 0, s, why, tier, true, 0.5, firstSenseTags(id)});
  }
  if (!pivotVia.empty())
    for (Scored& x : sc) {
      x.score -= 0.05;   // a pivot reading is one step less certain
      x.why += ", via English \"" + pivotVia + "\"";
    }
  // C17 (coordinator add-on): two lemmas with the same cleaned headword, part of speech, genitive / infinitive / perfect
  // inflect alike (the rebuilt latin.vpl lists "caelum" heaven and "caelum" chisel): they give the same Latin words,
  // so they are one candidate. The best score represents the group (its sense is the one that fits); on equal scores
  // the lower tier, then more paradigm cells, then the lower id. The others are dropped and never count as a
  // competitor for the margin. Homographs that inflect differently (volō velle / volō volāre) stay apart.
  if (sc.size() > 1) {
    auto sig = [&](uint32_t id) {
      const lex::Lemma l = la_.lemma(id);
      const std::string head = morph::cleanHead(l.head);
      const morph::Principal pp = morph::parsePrincipal(head, l.principal);
      return text::nfc(head) + "|" + std::to_string((int)l.pos) + "|" + text::nfc(pp.infinitive) + "|" +
             text::nfc(pp.genitive) + "|" + text::nfc(pp.perfect);
    };
    auto cellCount = [&](uint32_t id) {
      std::vector<std::pair<uint32_t, std::string_view>> cl;
      la_.cells(id, cl);
      return cl.size();
    };
    // C20: each signature is computed once (it was recomputed for every pair: the largest cost of a translation)
    std::vector<std::string> sigs;
    sigs.reserve(sc.size());
    for (const Scored& x : sc) sigs.push_back(sig(x.lemma));
    std::vector<char> gone(sc.size(), 0);
    for (size_t i = 0; i < sc.size(); ++i) {
      if (gone[i]) continue;
      const std::string& si = sigs[i];
      size_t win = i;
      for (size_t j = i + 1; j < sc.size(); ++j) {
        if (gone[j] || sigs[j] != si) continue;
        const Scored& w = sc[win];
        const Scored& x = sc[j];
        bool better = x.score > w.score;
        if (x.score == w.score) {
          if (x.tier != w.tier) better = x.tier < w.tier;
          else {
            const size_t cx = cellCount(x.lemma), cw = cellCount(w.lemma);
            better = cx != cw ? cx > cw : x.lemma < w.lemma;
          }
        }
        if (better) { gone[win] = 1; win = j; }
        else gone[j] = 1;
      }
      if (win != i) gone[i] = 1;
    }
    std::vector<Scored> kept;
    for (size_t i = 0; i < sc.size(); ++i)
      if (!gone[i]) kept.push_back(sc[i]);
    sc.swap(kept);
  }
  std::stable_sort(sc.begin(), sc.end(), [](const Scored& a, const Scored& b) {
    if (a.score != b.score) return a.score > b.score;
    return a.lemma < b.lemma;
  });
  // fidelity 2/3: a tier 1/2 lemma whose sense names the source word beats a tier 3 near-synonym (style target 1.2)
  if (st.fidelity >= 2 && !sc.empty() && sc[0].tier >= 3 && sc[0].why.find("correction") == std::string::npos) {
    for (size_t i = 1; i < sc.size(); ++i)
      if (sc[i].tier <= 2 && sc[i].kwHit && sc[i].base >= 0.2 && sc[i].base >= 0.5 * sc[0].base) {
        Scored x = sc[i];
        x.why += ", tier preference";
        sc.erase(sc.begin() + (long)i);
        sc.insert(sc.begin(), x);
        break;
      }
  }
  for (size_t i = 0; i < sc.size() && i < 6; ++i)
    c.candidates.push_back(Candidate{sc[i].lemma, sc[i].sense, std::round(sc[i].score * 1000) / 1000, sc[i].why});
  if (sc.empty() && !es && raw.empty() && taught.empty()) {
    // C17: a form the reverse index does not list is derived from a known word before it is given up: the parts of a
    // hyphenated compound ("sea-shore" -> seashore, shore; "bran-new" -> new), regular endings of the part of speech
    // ("dented" -> dent, "happier" -> happy, "gladly" -> glad)
    const std::string upos = pos == Noun || pos == Name ? "NOUN" : pos == Verb ? "VERB" : pos == Adj ? "ADJ" : pos == Adv ? "ADV" : "";
    const std::string low = text::lower(sourceLemma);
    for (const std::string& b : frame::en::baseCandidates(low, upos)) {
      std::vector<lex::Candidate> probe;
      la_.reverse(text::en_key(b), probe);
      std::vector<const curated::TierEntry*> tp;
      cd_.glossTiers(b, tp);
      if (probe.empty() && tp.empty()) continue;
      Choice c2;
      c2.token = c.token;
      const uint32_t id = select(b, pos, context, hasObject, personObject, st, c2, srcGender);
      if (id == kNone) continue;
      c = c2;
      c.source = sourceLemma;
      c.note = "derived from \"" + b + "\"" + (c.note.empty() ? std::string() : "; " + c.note);
      return id;
    }
  }
  if (sc.empty()) { c.unknown = true; c.kind = "unknown"; return kNone; }
  size_t pick = 0;
  for (const auto& ov : st.overrides)
    if (ov.first == c.token && ov.second >= 0 && (size_t)ov.second < sc.size()) pick = (size_t)ov.second;
  c.lemma = sc[pick].lemma;
  c.registerTag = (sc[pick].tags & kSenseMedieval) ? "medieval" : (sc[pick].tags & kSenseNewLatin) ? "new-latin" : "";
  c.margin = sc.size() > 1 ? std::max(0.0, sc[0].score - sc[1].score) : 1.0;
  if (sc.size() > 1 && sc[0].why.find("tier preference") != std::string::npos) c.margin = std::max(c.margin, 0.15);
  // confidence (DESIGN 10.4, C2b): a tier 3 choice while a tier 1/2 candidate of the same sense existed
  if (sc[pick].tier >= 3)
    for (size_t i = 0; i < sc.size(); ++i)
      if (i != pick && sc[i].tier <= 2 && sc[i].kwHit && sc[i].base >= 0.2) c.lowTier = true;
  if (sc[pick].why.find("correction") != std::string::npos) { c.kind = "correction"; c.lowTier = false; }
  // periphrasis (fidelity 2/3): a simpler single word replaces the chosen lemma
  if (st.fidelity >= 2) {
    const lex::Lemma l = la_.lemma(c.lemma);
    if (const curated::PeriphrasisEntry* pe = cd_.periphrasis(l.key)) {
      const std::vector<std::string> w = words(pe->periphrasis);
      if (w.size() == 1 && st.fidelity >= std::max<int>(2, pe->tier)) {
        const uint32_t alt = morph::findLemma(la_, w[0], l.pos);
        if (alt != kNone && alt != c.lemma) {
          c.note = std::string("periphrasis: ") + std::string(l.head) + " -> " + w[0];
          c.lemma = alt;
          c.registerTag.clear();
          c.kind = "periphrasis";
        }
      }
    }
  }
  return c.lemma;
}

// ---- adjectives (C17) ------------------------------------------------------------------------------------------------
// An adjective of the source: its own Latin adjective; an English participle used as an adjective ("a lighted match",
// "the sleeping dog") takes the Latin adjective of the form when the reverse index lists one ("frightened"), else
// the participle of the Latin verb of its English verb (accēnsus, dormiēns).
uint32_t Transfer::adjectiveInto(const frame::SemAdj& a, Ctx& c, LaAdj& la, Choice& ch) const {
  ch.token = a.token;
  // C22: a greeting used as an adjective ("howdy-do birds", "how-do-you-do frogs"): greeting ones, the present
  // participle of salūtō (avēs salūtantēs), never a bracketed word
  if (c.st.lang == frame::SrcLang::En && a.participle == 0 && a.token >= 0 && (size_t)a.token < c.s.tokens.size()) {
    const std::string sl = c.s.tokens[(size_t)a.token].lower;
    if (sl == "howdy-do" || sl == "how-do-you-do" || sl == "howdy" || sl == "hello") {
      frame::SemAdj g = a;
      g.lemma = "greet";
      g.participle = 2;
      la.participle = Present;
      c.out.flags.push_back("derived-word");
      return adjectiveInto(g, c, la, ch);
    }
  }
  if (a.lemma == "what-like") {   // C17: quālis (see clauseInto)
    ch.source = "what ... like";
    ch.lemma = latin("quālis", Adj);
    if (ch.lemma == kNone) ch.lemma = latin("quālis");
    ch.kind = "table";
    return ch.lemma;
  }
  if (a.participle && a.token >= 0 && (size_t)a.token < c.s.tokens.size()) {
    const std::string surf = c.s.tokens[(size_t)a.token].lower;
    Choice c1;
    c1.token = a.token;
    uint32_t id = surf != a.lemma ? select(surf, Adj, c.context, false, false, c.st, c1) : kNone;
    // the form's own adjective only when it is taught or a strong sense ("stuffed" -> plēnus); else the participle
    // ("covered" is not silvestris "covered with trees")
    const bool strong = !c1.candidates.empty() && (c1.candidates[0].why.find("teacher gloss") != std::string::npos ||
                                                   (c1.candidates[0].why.find("base ") == 0 &&
                                                    std::atoi(c1.candidates[0].why.c_str() + 5) >= 150));
    if (id != kNone && strong) { ch = c1; return id; }
    Choice c2;
    c2.token = a.token;
    id = select(a.lemma, Verb, c.context, a.participle == 1, false, c.st, c2);
    // C20: a deponent's perfect participle is active ("fūrātus" = having stolen): a passive participle ("the stolen
    // crown") takes the next candidate that has a passive one (surreptus), else none
    if (id != kNone && a.participle == 1 && (la_.lemma(id).flags & lex::Deponent)) {
      uint32_t alt = kNone;
      for (const Candidate& cd : c2.candidates)
        if (cd.lemma != id && !(la_.lemma(cd.lemma).flags & lex::Deponent) && cd.score >= c2.candidates[0].score - 0.5) {
          std::string probe;
          if (morph::generate(la_, cd.lemma, morph::participle(Perfect, Passive, Nom, Sg, M), probe, false)) { alt = cd.lemma; break; }
        }
      id = alt;
      if (alt != kNone) { c2.lemma = alt; c2.margin = 0; }
    }
    if (id != kNone) {
      la.participle = a.participle == 2 ? (uint8_t)Present : (uint8_t)Perfect;
      ch = c2;
      ch.source = surf;
      ch.note = "participle of " + std::string(la_.lemma(id).head) + " (" + surf + ")";
      return id;
    }
    ch = c1;
    return kNone;
  }
  const uint32_t id = select(a.lemma, Adj, c.context, false, false, c.st, ch);
  // C20: an adjective that is also a verb's past participle ("the lost kitten", "a broken cup") whose own Latin
  // adjective is only a weak reading (vapidus "stale" for lost: a modifier sense, score below 0.2): the verb's
  // perfect participle (āmissus), as for the participles of C17
  if (c.st.lang == frame::SrcLang::En && c.st.srcLex && a.adverbs.empty() && a.degree == 0 &&
      (id == kNone || ch.candidates.empty() || ch.candidates[0].score < 0.2) && a.token >= 0 &&
      (size_t)a.token < c.s.tokens.size()) {
    std::vector<lex::Analysis> an;
    c.st.srcLex->lookup(text::en_key(c.s.tokens[(size_t)a.token].lower), an);
    std::string verb;
    for (const lex::Analysis& x : an) {
      const lex::Lemma vl = c.st.srcLex->lemma(x.lemma);
      const Features vf = unpack(c.st.srcLex->feature(x.feat));
      if (vl.pos == Verb && vf.mood == ParticipleMood && vf.tense == Perfect) { verb = text::lower(std::string(vl.head)); break; }
    }
    if (!verb.empty()) {
      Choice c2;
      c2.token = a.token;
      const uint32_t v = select(verb, Verb, c.context, true, false, c.st, c2);
      std::string probe;
      if (v != kNone && !c2.candidates.empty() && c2.candidates[0].score >= 0.3 && !(la_.lemma(v).flags & lex::Deponent) &&
          morph::generate(la_, v, morph::participle(Perfect, Passive, Nom, Sg, M), probe, false)) {
        la.participle = Perfect;
        ch = c2;
        ch.source = a.lemma;
        ch.note = "participle of " + std::string(la_.lemma(v).head) + " (" + a.lemma + ")";
        return v;
      }
    }
  }
  return id;
}

// ---- adverbs ---------------------------------------------------------------------------------------------------------
uint32_t Transfer::adverb(const std::string& lemma0, int token, Ctx& c, bool motion) const {
  const std::string lemma = c.st.lang == frame::SrcLang::Es ? tables::spanishAdverb(lemma0) : lemma0;
  Choice ch;
  ch.token = token;
  ch.source = lemma0;
  if (const char* la = tables::adverb(lemma, motion)) {
    // C19: "badly" with a passive ("was badly hurt / damaged") is graviter, like the participle fragments of C17
    if (lemma == "badly" && c.frame && c.frame->pred.voice == frame::Voice::Passive) la = "graviter";
    ch.lemma = latin(la, Adv);
    if (ch.lemma == kNone) ch.lemma = latin(la);
    ch.kind = "table";
    if (ch.lemma != kNone) { c.out.choices.push_back(ch); c.cover(token); return ch.lemma; }
  }
  if (tables::dropAdverb(lemma)) { c.cover(token); return kNone; }
  std::vector<std::string> ctx;
  uint32_t id = select(lemma0, Adv, ctx, false, false, c.st, ch);
  if (id == kNone) {   // "gladly" -> adjective "glad" + -ly is not in REVX: try the adjective-less key
    if (lemma.size() > 4 && lemma.compare(lemma.size() - 2, 2, "ly") == 0) {
      Choice c2;
      c2.token = token;
      id = select(lemma.substr(0, lemma.size() - 2), Adv, ctx, false, false, c.st, c2);
      if (id != kNone) ch = c2;
    }
  }
  // C19: "brightly" (lemma "bright"): the Latin adjective's own adverb when the lexicon lists it (clārus -> clārē,
  // fortis -> fortiter, prūdēns -> prūdenter); never an invented form
  if (id == kNone && c.st.lang == frame::SrcLang::En) {
    std::string base = text::lower(lemma);
    if (base.size() > 4 && base.compare(base.size() - 2, 2, "ly") == 0) {
      const std::string b = frame::en::knownBase(*c.st.srcLex, base, "ADV");
      if (!b.empty()) base = b;
    }
    Choice ca;
    ca.token = token;
    const uint32_t aj0 = c.st.srcLex ? select(base, Adj, ctx, false, false, c.st, ca) : kNone;
    std::vector<uint32_t> adjs;
    if (aj0 != kNone) adjs.push_back(aj0);
    for (const Candidate& k : ca.candidates)
      if (k.lemma != aj0 && adjs.size() < 4) adjs.push_back(k.lemma);
    for (uint32_t aj : adjs) {
      if (id != kNone) break;
      const lex::Lemma al = la_.lemma(aj);
      const std::string head = text::nfc(morph::cleanHead(al.head));
      const morph::Principal pp = morph::parsePrincipal(head, al.principal);
      auto ends = [](const std::string& w, const std::string& e) {
        return w.size() > e.size() && w.compare(w.size() - e.size(), e.size(), e) == 0;
      };
      std::vector<std::string> tries;
      const std::string gen = pp.genitive.empty() ? std::string() : text::nfc(pp.genitive);
      if (ends(head, "us")) tries.push_back(head.substr(0, head.size() - 2) + "ē");
      if (!pp.feminine.empty() && ends(text::nfc(pp.feminine), "a"))
        tries.push_back(text::nfc(pp.feminine).substr(0, text::nfc(pp.feminine).size() - 1) + "ē");   // pulchra -> pulchrē
      if (ends(gen, "is")) {
        const std::string st = gen.substr(0, gen.size() - 2);
        tries.push_back(ends(st, "nt") ? st + "er" : st + "iter");
      }
      if (ends(head, "is")) tries.push_back(head.substr(0, head.size() - 2) + "iter");
      for (const std::string& t : tries) {
        const uint32_t av = morph::findLemma(la_, t, Adv);
        if (av == kNone) continue;
        id = av;
        ch = ca;
        ch.lemma = av;
        ch.source = lemma0;
        ch.note = "adverb of " + head;
        break;
      }
    }
  }
  ch.token = token;
  c.out.choices.push_back(ch);
  c.cover(token);
  if (id == kNone) c.out.unknownWords.push_back(lemma0);
  return id;
}

// ---- names of names_la.tsv made of several words (C15) ------------------------------------------------------------
// "Leō Timidus", "Urbs Smaragdōrum", "Magnus Magus": the nominative noun is the head (declined), nominative adjectives
// agree with it (before or after as written), genitives stay as written. False when a word does not analyse.
bool Transfer::latinNameNP(const std::string& latin, LaNP& o) const {
  const std::vector<std::string> ws = words(latin);
  if (ws.empty()) return false;
  o = LaNP{};
  int head = -1;
  struct Part { int kind; uint32_t lemma; std::string text; };   // 1 adjective, 2 genitive
  std::vector<std::pair<int, Part>> parts;
  for (size_t i = 0; i < ws.size(); ++i) {
    morph::Token mt;
    morph::analyseLatin(la_, ws[i], mt);
    uint32_t noun = kNone, adj = kNone;
    uint8_t num = Sg;
    bool gen = false, adjAgrees = false;
    for (const lex::Analysis& a : mt.analyses) {
      const lex::Lemma l = la_.lemma(a.lemma);
      const Features f = unpack(la_.feature(a.feat));
      if (l.pos == Noun && f.case_ == Nom && noun == kNone && !(l.flags & lex::ProperName)) { noun = a.lemma; num = f.number ? f.number : (uint8_t)Sg; }
      else if ((l.pos == Adj || l.pos == Participle) && f.case_ == Nom && adj == kNone) adj = a.lemma;
      // C22: a nominative adjective reading that agrees with the head noun ("Terra Mīrābilis": mīrābilis is also a
      // genitive) is the adjective; "Potestās Malī" (malī nom. plural) stays a genitive
      if ((l.pos == Adj || l.pos == Participle) && f.case_ == Nom && head >= 0 && o.head != kNone &&
          (f.number == 0 || f.number == o.number) &&
          (f.gender == 0 || simpleGender(f.gender) == simpleGender(la_.lemma(o.head).gender) || f.gender == MF ||
           f.gender == MFN))
        adjAgrees = true;
      if (f.case_ == Gen) gen = true;
    }
    if (head < 0 && noun != kNone) {
      head = (int)i;
      o.head = noun;
      o.number = num;
      continue;
    }
    if (adj != kNone && (!gen || adjAgrees)) { parts.push_back({(int)i, Part{1, adj, ws[i]}}); continue; }
    if (gen || adj != kNone) { parts.push_back({(int)i, Part{2, kNone, ws[i]}}); continue; }
    return false;
  }
  if (head < 0) return false;
  for (const auto& p : parts) {
    if (p.second.kind == 1) {
      LaAdj a;
      a.lemma = p.second.lemma;
      a.before = p.first < head;
      a.capitalise = true;
      o.adjectives.push_back(a);
    } else {
      LaNP g;
      g.fixed = p.second.text;
      o.genitive.push_back(g);
    }
  }
  o.capitalise = true;
  o.nameWords = true;
  return true;
}

// The NP as a name of names_la.tsv: the written words ("Tin Woodman", "the Cowardly Lion", "the City of Emeralds",
// "the Witch of the North") are looked up longest first; a translated multi-word name becomes a Latin NP, a single
// name keeps the realiser's name policy (declined from the table). The parts used are covered.
bool Transfer::nameTableInto(const SemNP& n, Ctx& c, LaNP& o) const {
  if (n.token < 0 || (size_t)n.token >= c.s.tokens.size()) return false;
  auto cap = [](const std::string& w) { return !w.empty() && w[0] >= 'A' && w[0] <= 'Z'; };
  // head words in source order (multi-word names: "Tin Woodman")
  std::vector<int> headToks;
  for (int t : n.tokens) {
    const nlp::Token& x = c.s.tokens[(size_t)t];
    if ((x.upos == "PROPN" || x.upos == "NOUN") && cap(x.text) && t <= n.token) headToks.push_back(t);
  }
  if (headToks.empty() || headToks.back() != n.token) headToks = {n.token};
  std::string head;
  for (int t : headToks) head += (head.empty() ? "" : " ") + c.s.tokens[(size_t)t].text;
  // capitalised adjectives right before the head ("Cowardly Lion", "Wicked Witch", "Great Wizard")
  std::string adjs;
  std::vector<size_t> adjUsed;
  for (size_t i = 0; i < n.adjectives.size(); ++i) {
    const int t = n.adjectives[i].token;
    if (t < 0 || t > headToks.front() || !cap(c.s.tokens[(size_t)t].text)) continue;
    adjs += c.s.tokens[(size_t)t].text + " ";
    adjUsed.push_back(i);
  }
  // "of" genitive ("City of Emeralds", "Witch of the North")
  std::string ofGen, ofGenThe;
  if (!n.genitive.empty() && n.genitive[0].token > n.token) {
    const SemNP& g = n.genitive[0];
    std::string gw;
    for (int t : g.tokens) {
      const nlp::Token& x = c.s.tokens[(size_t)t];
      if (x.upos == "DET" || x.upos == "ADP") continue;
      gw += (gw.empty() ? "" : " ") + x.text;
    }
    ofGen = " of " + gw;
    ofGenThe = " of the " + gw;
  }
  struct Try { std::string s; bool adj, gen; };
  std::vector<Try> tries;
  if (!adjs.empty() && !ofGen.empty()) { tries.push_back({adjs + head + ofGen, true, true}); tries.push_back({adjs + head + ofGenThe, true, true}); }
  if (!ofGen.empty()) { tries.push_back({head + ofGen, false, true}); tries.push_back({head + ofGenThe, false, true}); }
  if (!adjs.empty()) tries.push_back({adjs + head, true, false});
  tries.push_back({head, false, false});
  for (const Try& t : tries) {
    const curated::NameEntry* e = cd_.nameByEnglish(t.s);
    if (!e) continue;
    const bool multi = e->latinNom.find(' ') != std::string::npos;
    if (multi) {
      if (!latinNameNP(e->latinNom, o)) continue;
    } else {
      // a one-word name: the realiser's name policy (names_la.tsv declension); a translated title word with a
      // lexicon lemma ("Cicōnia") is declined from the lexicon
      uint32_t id = e->policy == curated::NamePolicy::Translate && e->declension < 0 ? morph::findLemma(la_, e->latinNom, Noun) : kNone;
      o = LaNP{};
      if (id != kNone) { o.head = id; o.capitalise = true; o.nameWords = true; }
      else { o.isName = true; o.name = t.s; }
    }
    if (n.number == 2 && !multi) o.number = Pl;
    Choice ch;
    ch.token = n.token;
    ch.source = t.s;
    ch.lemma = o.head;
    ch.kind = "name";
    ch.note = "names_la.tsv: " + e->latinNom;
    c.out.choices.push_back(ch);
    c.cover(n.tokens);
    if (!t.gen)   // the "of" attribute was not part of the name: translate it as usual
      for (const SemNP& g : n.genitive) { LaNP x; npInto(g, c, x); o.genitive.push_back(x); break; }
    if (!t.adj)
      for (const frame::SemAdj& a : n.adjectives) {
        Choice ac;
        ac.token = a.token;
        LaAdj la;
        la.lemma = adjectiveInto(a, c, la, ac);
        c.out.choices.push_back(ac);
        // C17: a lower-case epithet before the name stands before it ("the brave Queen" -> fortis Rēgīna)
        if (n.determiner != "a" && n.determiner != "an" && a.token >= 0 && a.token < n.token && (size_t)a.token < c.s.tokens.size() && c.st.lang == frame::SrcLang::En &&
            !c.s.tokens[(size_t)a.token].text.empty() && c.s.tokens[(size_t)a.token].text[0] >= 'a' &&
            c.s.tokens[(size_t)a.token].text[0] <= 'z')
          la.before = true;
        if (la.lemma != kNone) o.adjectives.push_back(la);
        else c.out.unknownWords.push_back(a.lemma);
      }
    return true;
  }
  return false;
}

// A capitalised common noun used as a title inside the sentence ("the kind Stork", "this City", "a great Head",
// "the Golden Cap" when the table lacks it): the noun, capitalised. Not for the first word of the sentence.
bool Transfer::titleNoun(const SemNP& n, Ctx& c, LaNP& o) const {
  if (c.st.lang != frame::SrcLang::En || n.token <= 0 || (size_t)n.token >= c.s.tokens.size()) return false;
  const std::string& surf = c.s.tokens[(size_t)n.token].text;
  if (surf.empty() || surf[0] < 'A' || surf[0] > 'Z') return false;
  bool first = true;
  for (int i = 0; i < n.token; ++i) {
    const nlp::Token& x = c.s.tokens[(size_t)i];
    if (x.upos != "PUNCT" && x.text != "\"" && x.text != "\xE2\x80\x9C") first = false;
  }
  if (first) return false;
  const std::string low = text::lower(surf);
  if (c.st.srcLex) {   // the English lexicon must know the word as a common noun
    std::vector<lex::Analysis> an;
    c.st.srcLex->lookup(text::en_key(low), an);
    bool noun = false;
    for (const lex::Analysis& a : an) noun = noun || c.st.srcLex->lemma(a.lemma).pos == Noun;
    if (!noun) return false;
  }
  Choice ch;
  ch.token = n.token;
  std::string sg = low;
  if (n.number == 2 && sg.size() > 3 && sg.back() == 's') sg.pop_back();
  const uint32_t id = select(sg, Noun, c.context, false, false, c.st, ch);
  if (id == kNone || ch.candidates.empty() || cd_.effectiveTier(la_.lemma(id).key, la_.lemma(id).pos, la_.lemma(id).tier) > 2)
    return false;
  SemNP m = n;
  m.isName = false;
  m.head = sg;
  m.title = true;   // guard: npInto does not come back here
  npInto(m, c, o);
  o.capitalise = true;
  // C17: an epithet written in lower case before a definite title stands before it in Latin too ("the kind Stork" ->
  // benigna Cicōnia; cf. pius Aenēās); "a good Wizard" is a description (magum bonum)
  for (size_t i = 0; i < o.adjectives.size() && i < n.adjectives.size(); ++i) {
    const int at = n.adjectives[i].token;
    if (n.determiner != "a" && n.determiner != "an" && at >= 0 && at < n.token && (size_t)at < c.s.tokens.size() && !c.s.tokens[(size_t)at].text.empty() &&
        c.s.tokens[(size_t)at].text[0] >= 'a' && c.s.tokens[(size_t)at].text[0] <= 'z')
      o.adjectives[i].before = true;
  }
  return true;
}

// The relative clause of an NP (one), with the role of its relative pronoun (C2; stranded prepositions C15).
void Transfer::relativeInto(const SemNP& n, Ctx& c, LaNP& o) const {
  // C20: a to-infinitive on a noun ("a book to read", "water to drink", "a place to sleep") is not a relative clause
  // (quī legit): the gerundive agreeing with the noun when the noun is the verb's object (librum legendum, aquam
  // bibendam), else ad + the gerund (locum ad dormiendum). Only the bare verb; anything else stays as before (Check).
  const bool indefinite = n.isPronoun && (n.pronLemma == "something" || n.pronLemma == "anything" || n.pronLemma == "nothing");
  if (c.st.lang == frame::SrcLang::En && n.relative.size() == 1 && !o.isPronoun && (!n.isPronoun || indefinite) &&
      o.literal.empty()) {
    const SemFrame& rf = n.relative[0];
    int first = -1;
    for (int t : rf.tokens)
      if (t >= 0 && (size_t)t < c.s.tokens.size() && c.s.tokens[(size_t)t].upos != "PUNCT" && (first < 0 || t < first)) first = t;
    if (first >= 0 && c.s.tokens[(size_t)first].lower == "to" && rf.hasPred && !rf.hasSubject && !rf.hasObject &&
        !rf.hasIndirect && rf.obliques.empty() && rf.subordinate.empty() && rf.adverbs.empty() && !rf.negative &&
        rf.pred.modality == Modality::None && rf.pred.voice == frame::Voice::Active && rf.pred.particle.empty() &&
        rf.pred.prepVerb.empty() && rf.predAdj.empty() && rf.predicative.empty() && rf.pred.lemma != "be" &&
        rf.pred.fixedLatin.empty()) {
      Choice ch;
      ch.token = rf.pred.token;
      const uint32_t v = select(rf.pred.lemma, Verb, c.context, true, false, c.st, ch);
      if (v != kNone) {
        // transitive: an accusative object in the sense tags of the chosen sense, or a passive participle cell
        bool trans = false;
        std::vector<lex::Sense> se;
        la_.senses(v, se);
        const uint16_t sn = ch.candidates.empty() ? 0 : ch.candidates[0].sense;
        if (sn < se.size()) trans = (se[sn].tags & 1u) != 0;
        std::string probe;
        const bool gerundive = trans && !indefinite && morph::generate(la_, v, morph::participle(Future, Passive, Acc, Sg, M), probe, false);
        const bool gerund = morph::generate(la_, v, [] { Features g; g.pos = Verb; g.mood = Gerund; g.case_ = Acc; return g; }(),
                                            probe, false);
        if (gerundive || gerund) {
          if (gerundive) {
            LaAdj ga;
            ga.lemma = v;
            ga.gerundive = true;
            o.adjectives.push_back(ga);
            ch.note = "a noun + to-infinitive: the gerundive";
          } else {
            o.adGerund = v;
            ch.note = "a noun + to-infinitive: ad + the gerund";
          }
          c.out.choices.push_back(ch);
          c.cover(rf.tokens);
          return;
        }
      }
    }
    // C22: the to-infinitive has its own object ("a place to build a house", "a strange place to have a party"): the
    // genitive of the gerundive with that object (locus domūs aedificandae), not a relative clause (quī domum aedificat)
    if (first >= 0 && c.s.tokens[(size_t)first].lower == "to" && rf.hasPred && !rf.hasSubject && rf.hasObject &&
        !rf.object.isPronoun && !rf.object.isName && !rf.hasIndirect && rf.obliques.empty() && rf.subordinate.empty() &&
        rf.adverbs.empty() && !rf.negative && rf.pred.modality == Modality::None && rf.pred.voice == frame::Voice::Active &&
        rf.pred.particle.empty() && rf.pred.prepVerb.empty() && rf.pred.lemma != "be" && rf.pred.fixedLatin.empty() &&
        o.genitive.empty()) {
      Choice ch;
      ch.token = rf.pred.token;
      const uint32_t v = select(rf.pred.lemma, Verb, c.context, true, false, c.st, ch);
      std::string probe;
      if (v != kNone && morph::generate(la_, v, morph::participle(Future, Passive, Gen, Sg, M), probe, false)) {
        LaNP g;
        npInto(rf.object, c, g);
        if (g.head != kNone && g.literal.empty()) {
          LaAdj ga;
          ga.lemma = v;
          ga.gerundive = true;
          g.adjectives.push_back(ga);
          g.case_ = Gen;
          o.genitive.push_back(g);
          ch.note = "a noun + to-infinitive with its object: the genitive of the gerundive";
          c.out.choices.push_back(ch);
          c.cover(rf.tokens);
          return;
        }
      }
    }
  }
  // relative clause
  for (const SemFrame& rf00 : n.relative) {
    // C15: a relative pronoun inside a prepositional phrase ("the Great Wizard I told you of" -> dē quō tibi dīxī;
    // "the arts I know of" -> quās sciō when the verb takes the phrase as its object)
    SemFrame strip;
    const SemFrame* rfp = &rf00;
    std::string relPrepWord;
    for (size_t q = 0; q < rf00.obliques.size(); ++q)
      if (rf00.obliques[q].np.isPronoun && rf00.obliques[q].np.pronLemma == "which" && rf00.obliques[q].np.head.empty() &&
          rf00.obliques[q].np.tokens.size() == 1 && rf00.obliques[q].np.token == rf00.obliques[q].token) {
        strip = rf00;
        relPrepWord = rf00.obliques[q].prep;
        strip.obliques.erase(strip.obliques.begin() + (long)q);
        c.cover(rf00.obliques[q].token);
        rfp = &strip;
        break;
      }
    // C17: "the house where I live" -> domus in quā habitō: a relative "where" is in + the relative pronoun (quō for
    // a verb of motion: "the town where we went" -> oppidum quō iimus)
    SemFrame whereRel;
    if (c.st.lang == frame::SrcLang::En && rfp->type == Kind::Wh && rfp->wh.word == "where" && relPrepWord.empty()) {
      whereRel = *rfp;
      whereRel.type = Kind::Decl;
      whereRel.wh = frame::SemWh{};
      if (rfp->wh.token >= 0) c.cover(rfp->wh.token);
      relPrepWord = "in";
      rfp = &whereRel;
    }
    const SemFrame& rf0 = *rfp;
    SemFrame dep;
    const SemFrame& rf = deponentActive(rf0, c, dep) ? dep : rf0;
    LaClause rc;
    const uint32_t keepForced = c.forcedVerb;
    if (rfp == &whereRel)   // "live" + where -> habitō (verbprep_en_la.tsv "live in")
      if (const curated::VerbPrepEntry* vw = cd_.verbPrep(rf.pred.lemma, "in"))
        if (vw->latin != "-" && rf.pred.modality == Modality::None) c.forcedVerb = latin(vw->latin.c_str(), Verb);
    clauseInto(rf, c, rc);
    c.forcedVerb = keepForced;
    if (!relPrepWord.empty()) {
      const curated::VerbPrepEntry* vpe = cd_.verbPrep(rf.pred.lemma, relPrepWord);
      if (vpe && vpe->frame == "obj" && !rc.hasObject) {
        rc.relRole = realise::Role::Object;
      } else {
        const char* lp = relPrepWord == "with" ? "cum" : relPrepWord == "to" ? "ad" : relPrepWord == "from" ? "ab"
                       : relPrepWord == "for" ? "prō" : relPrepWord == "in" || relPrepWord == "on" || relPrepWord == "at" ? "in"
                       : "dē";
        if (vpe && !vpe->latinPrep.empty()) lp = vpe->latinPrep.c_str();
        rc.relRole = realise::Role::Oblique;
        rc.relPrep = latin(lp, Prep);
      }
      o.relative.push_back(rc);
      break;
    }
    // role of the gap: the relative pronoun's role, else object when the clause has a subject
    realise::Role role = realise::Role::Subject;
    if (rf.hasSubject && rf.subject.isPronoun && (rf.subject.pronLemma == "which" || rf.subject.pronLemma == "who" ||
                                                 rf.subject.pronLemma == "that")) {
      role = realise::Role::Subject;
      rc.hasSubject = false;
    } else if (rf.hasObject && rf.object.isPronoun && (rf.object.pronLemma == "which" || rf.object.pronLemma == "who" ||
                                                      rf.object.pronLemma == "that")) {
      role = realise::Role::Object;
      rc.hasObject = false;
    } else if (rf.hasSubject && !rf.hasObject) {
      role = realise::Role::Object;
    }
    rc.relRole = role;
    // C24: "(I could) hear a song that I could understand": a relative clause with "can / could" inside a clause of
    // the same modal is a clause of character (subjunctive: quod intellegere possem), not a second "poteram"
    {
      const uint32_t possum = latin("possum", Verb);
      const bool mainCan = (c.frame && c.frame->pred.modality == Modality::Can) ||
                           (c.mem.songLine && c.mem.prevValid && c.mem.prevModal == possum);
      if (c.st.lang == frame::SrcLang::En && possum != kNone && rc.pred.modal == possum && rc.pred.mood == Indicative &&
          mainCan && c.frame != &rf)
        rc.pred.mood = Subjunctive;
    }
    // C19: a second relative clause coordinated inside the first ("those who are not honest, or who approach him"):
    // its "who" is the relative pronoun again (aut quī ... accēdunt), agreeing with the antecedent
    for (size_t q = 0; q < rc.subs.size() && q < rf.subordinate.size(); ++q) {
      realise::LaSub& sb = rc.subs[q];
      if (sb.rel != realise::SubRel::Coord || sb.clause.empty()) continue;
      LaClause& sc = sb.clause[0];
      if (!sc.hasSubject) continue;
      const frame::SemSub* src = nullptr;
      for (const frame::SemSub& x : rf.subordinate)
        if (x.relation == Relation::Coord && !x.frame.empty() && x.frame[0].hasSubject && x.frame[0].subject.isPronoun &&
            (x.frame[0].subject.pronLemma == "who" || x.frame[0].subject.pronLemma == "which"))
          src = &x;
      if (!src || latin("quī", Pron) == kNone) continue;
      LaNP qn;
      qn.head = latin("quī", Pron);
      qn.number = o.isPronoun ? (o.pron.number ? o.pron.number : o.number) : o.number;
      qn.gender = o.isPronoun ? (o.pron.gender ? o.pron.gender : (uint8_t)M) : o.gender;
      if (!qn.gender && o.head != kNone) {
        const lex::Lemma hl = la_.lemma(o.head);
        qn.gender = (uint8_t)(hl.gender == F || hl.gender == FN ? F : hl.gender == N ? N : M);
      }
      if (!qn.gender) qn.gender = M;
      qn.case_ = Nom;
      sc.subject = qn;
      sc.pred.person = 3;
      sc.pred.number = qn.number;
    }
    o.relative.push_back(rc);
    break;
  }
}

// C17: the feminine of a masculine person noun (servus -> serva, fīlius -> fīlia, puer -> puella, rēx -> rēgīna,
// magister -> magistra), when the lexicon has it as a feminine noun; else the noun itself
uint32_t Transfer::feminineOf(uint32_t noun) const {
  if (noun == kNone) return noun;
  const lex::Lemma l = la_.lemma(noun);
  if (l.pos != Noun || l.gender != M) return noun;
  const std::string k(l.key);
  std::string fem;
  if (k == "puer") fem = "puella";
  else if (k == "rex") fem = "rēgīna";
  else if (k.size() > 2 && k.compare(k.size() - 2, 2, "us") == 0) fem = k.substr(0, k.size() - 2) + "a";
  else if (k.size() > 3 && k.compare(k.size() - 3, 3, "ter") == 0) fem = k.substr(0, k.size() - 2) + "ra";
  if (fem.empty()) return noun;
  const uint32_t f = morph::findLemma(la_, fem, Noun);
  return f != kNone && la_.lemma(f).gender == F ? f : noun;
}

// ---- noun phrases ---------------------------------------------------------------------------------------------------
void Transfer::npInto(const SemNP& n, Ctx& c, LaNP& o) const {
  // C22: appositions ("Robert, the Bishop of London,") in the same case after the noun phrase
  if (!n.apposition.empty()) {
    SemNP m = n;
    m.apposition.clear();
    npInto(m, c, o);
    for (const SemNP& ap : n.apposition) {
      LaNP x;
      npInto(ap, c, x);
      o.apposition.push_back(x);
    }
    return;
  }
  // C22: "a world of my own", "a room of her own" -> mundus meus, cubiculum suum: the possessive of "own" is the noun's
  if (c.st.lang == frame::SrcLang::En && n.possessor.empty())
    for (size_t gi = 0; gi < n.genitive.size(); ++gi)
      if (text::lower(n.genitive[gi].head) == "own" && !n.genitive[gi].possessor.empty()) {
        SemNP m = n;
        m.possessor = n.genitive[gi].possessor;
        m.genitive.erase(m.genitive.begin() + (long)gi);
        npInto(m, c, o);
        return;
      }
  o = LaNP{};
  c.cover(n.tokens);
  o.number = n.number == 2 ? Pl : Sg;
  if (n.literalUnknown) { o.literal = n.surface; c.out.unknownWords.push_back(n.surface); return; }
  auto tableChoice = [&](uint32_t lemma, const std::string& src) {
    Choice ch;
    ch.token = n.token;
    ch.source = src;
    ch.lemma = lemma;
    ch.kind = "table";
    c.out.choices.push_back(ch);
  };
  // C17: what a name keeps of its NP: its adjectives (a lower-case epithet before the name stands before it: "old
  // Grumbo" -> vetus Grumbo), its relative clause and its coordination ("Flimsy and Grub")
  auto nameTail = [&](bool adjectives) {
    if (adjectives)
      for (const frame::SemAdj& a : n.adjectives) {
        Choice ac;
        LaAdj la;
        la.degree = a.degree;
        la.lemma = adjectiveInto(a, c, la, ac);
        c.out.choices.push_back(ac);
        c.cover(a.token);
        if (la.lemma == kNone) { c.out.unknownWords.push_back(a.lemma); continue; }
        la.before = a.token >= 0 && a.token < n.token;
        o.adjectives.push_back(la);
      }
    if (o.relative.empty()) relativeInto(n, c, o);
    for (const SemNP& k : n.coord) {
      LaNP x;
      npInto(k, c, x);
      o.coord.push_back(x);
    }
    if (!o.coord.empty() && n.coordConj == "or") o.coordConj = latin("aut", Conj);
  };
  // C17: "plenty of silk" / "a lot of water" -> multum sēricī (neuter + partitive genitive); "lots of apples" ->
  // multa māla (a plural noun takes multī)
  if (c.st.lang == frame::SrcLang::En && !n.isPronoun && !n.isName && n.genitive.size() == 1 && n.adjectives.empty() &&
      (text::lower(n.head) == "plenty" || text::lower(n.head) == "lot" || text::lower(n.head) == "lots") &&
      (n.determiner.empty() || n.determiner == "a") && !n.genitive[0].isPronoun) {
    const uint32_t multus = latin("multus", Adj);
    if (multus != kNone) {
      LaNP g;
      npInto(n.genitive[0], c, g);
      tableChoice(multus, n.head);
      if (n.genitive[0].number == 2) {
        o = g;
        LaAdj m;
        m.lemma = multus;
        o.adjectives.insert(o.adjectives.begin(), m);
        return;
      }
      o.head = multus;
      o.gender = N;
      o.number = Sg;
      g.case_ = Gen;
      o.genitive.push_back(g);
      return;
    }
  }
  // C20: a standalone possessive pronoun (mine, yours, ours, his, hers, theirs) stands for a noun said before it: the
  // possessive adjective agreeing with that noun with the noun itself left out ("taller than yours" -> quam tuus, "use
  // yours" -> tuō ūtī), the genitive eius / eōrum for the third person ("The ball is his." -> Pila eius est.)
  if (c.st.lang == frame::SrcLang::En && n.possessor.empty() && n.adjectives.empty() && n.genitive.empty() &&
      n.relative.empty() && n.coord.empty() && !n.isName) {
    const std::string w = text::lower(n.surface.empty() ? n.head : n.surface);
    const char* lat = w == "mine" ? "meus" : w == "ours" ? "noster" : w == "yours" ? (c.mem.addresseePlural ? "vester" : "tuus")
                    : nullptr;
    const bool third = w == "his" || w == "hers" || w == "theirs";
    if ((lat && (!n.isPronoun || n.pronLemma == w)) || (third && !n.isPronoun && n.determiner.empty())) {
      o.gender = c.possRefGender ? c.possRefGender : c.mem.lastGender ? c.mem.lastGender : (uint8_t)M;
      o.number = c.possRefNumber ? c.possRefNumber : c.mem.lastNumber ? c.mem.lastNumber : (uint8_t)Sg;
      if (lat) {
        o.possessive = latin(lat, Det);
        if (o.possessive == kNone) o.possessive = latin(lat, Adj);
        if (o.possessive != kNone) { tableChoice(o.possessive, w); return; }
      } else {
        LaNP g;
        g.isPronoun = true;
        g.pron.person = 3;
        g.pron.number = w == "theirs" ? Pl : Sg;
        g.pron.gender = w == "hers" ? F : M;
        g.number = g.pron.number;
        g.case_ = Gen;
        o.genitive.push_back(g);
        tableChoice(latin("is", Pron), w);
        return;
      }
    }
  }
  if (n.isPronoun) {
    const std::string& p = n.pronLemma;
    const bool personal = tables::personalPronoun(p);
    if (personal || (n.pron.person > 0 && p.empty())) {
      o.isPronoun = true;
      o.pron.person = n.pron.person ? n.pron.person : 3;
      uint8_t num = n.pron.number == 2 ? Pl : n.pron.number == 1 ? Sg : 0;
      if (!num) {
        num = (o.pron.person == 2 && (c.mem.addresseePlural || c.mem.answerWe)) ? Pl : Sg;
        if (num == Pl && o.pron.person == 2) c.mem.addresseeGuess = true;
      }
      if (n.determiner == "all") num = Pl;
      o.pron.number = num;
      o.number = num;
      o.pron.reflexive = n.pron.reflexive;
      if (o.pron.person == 1) {
        char g = c.st.speakerGender;
        if (c.st.flipSpeakerGender) g = (g == 'f') ? 'm' : 'f';
        o.pron.gender = g == 'f' ? F : M;
      } else if (o.pron.person == 3) {
        uint8_t g = n.pron.gender;
        // "it" stands for the last noun in verbal clauses ("It ran away" = the ball); with "be" it is neuter
        if ((p == "it" || p == "ello" || p == "lo") && c.mem.lastGender && !(c.mem.lastAnimate && p == "it") &&
            c.frame && !c.frame->copula &&
            c.frame->pred.lemma != "be")
          g = c.mem.lastGender;
        else if (c.st.lang == frame::SrcLang::Es && p == "lo")
          g = N;   // "Dámelo." without an antecedent: it (id)
        // Spanish él / ella / lo / la of a thing take the Latin gender of that noun, not the Spanish one (C13)
        if (c.st.lang == frame::SrcLang::Es && c.mem.lastGender && !c.mem.lastAnimate && n.pron.gender &&
            (p == "él" || p == "ella" || p == "ellos" || p == "ellas" || p == "la" || p == "las" || p == "los"))
          g = c.mem.lastGender;
        o.pron.gender = g ? g : (uint8_t)M;
      } else {
        // C22: "you" takes the gender of the person addressed when a name tells it ("You are very clever, Mary!")
        o.pron.gender = o.pron.person == 2 && c.mem.addresseeGender ? c.mem.addresseeGender : (uint8_t)M;
      }
      o.gender = o.pron.gender;
      // C19: "carry us all", "I saw you all": not the subject: the pronoun stays, omnēs after it ("nōs omnēs")
      const bool isSubject = c.frame && c.frame->hasSubject && c.frame->subject.token == n.token;
      if (n.determiner == "all" && !isSubject && n.pron.person < 3 && c.st.lang == frame::SrcLang::En) {
        LaAdj a;
        a.lemma = latin("omnis", Adj);
        a.after = true;
        if (a.lemma != kNone) { o.adjectives.push_back(a); tableChoice(a.lemma, "all"); }
        o.number = Pl;
        o.pron.number = Pl;
        return;
      }
      if (n.determiner == "all") {   // "you all": omnēs, verb in the pronoun's person
        LaNP q;
        q.head = latin("omnis", Adj);
        q.number = Pl;
        q.gender = M;
        q.emphasis = false;
        q.case_ = 0;
        o = q;
        c.subjPerson = n.pron.person;
        tableChoice(q.head, "all");
      }
      return;
    }
    // C24: a free relative ("what the sky is", "what I see"): the neuter antecedent is understood, quod + the clause
    if (c.st.lang == frame::SrcLang::En && p == "what" && !n.relative.empty() && latin("is", Pron) != kNone) {
      o = LaNP{};
      o.head = latin("is", Pron);
      o.gender = N;
      o.number = Sg;
      o.elideHead = true;
      // fronted in the source ("What you have, you keep."): it stays first (emphasis)
      bool front = n.token >= 0;
      for (int k = 0; k < n.token && (size_t)k < c.s.tokens.size(); ++k) {
        const std::string& u = c.s.tokens[(size_t)k].upos;
        if (u != "PUNCT" && u != "CCONJ" && u != "ADV") front = false;
      }
      o.emphasis = front;
      c.cover(n.token);
      relativeInto(n, c, o);
      // "everything is what it seems": "it" stands for the clause's subject: its number (omnia ... quod videntur)
      const frame::SemNP& rs = n.relative[0].subject;
      if (!o.relative.empty() && n.relative[0].hasSubject && rs.isPronoun &&
          (rs.pronLemma == "it" || text::lower(rs.head) == "it") && c.frame && c.frame->hasSubject &&
          (c.frame->subject.number == 2 || c.frame->subject.pronLemma == "everything" ||
           c.frame->subject.pronLemma == "all"))
        o.relative[0].pred.number = Pl;
      return;
    }
    // other closed-class pronouns
    uint32_t id = kNone;
    uint8_t gender = 0, number = Sg;
    if (p == "everyone" || p == "all") { id = latin("omnis", Adj); number = Pl; gender = M; }
    else if (p == "everything") { id = latin("omnis", Adj); number = Pl; gender = N; }
    else if (p == "nobody" || p == "none") { id = latin("nēmō", Pron); gender = M; }
    else if (p == "nothing") { id = latin("nihil", Pron); gender = N; }
    else if ((p == "someone" || p == "anyone") && c.afterSi) { id = latin("quis", Pron); gender = M; }   // C15: sī quis
    else if ((p == "something" || p == "anything") && c.afterSi) { id = latin("quis", Pron); gender = N; }   // sī quid
    else if (p == "someone" || p == "anyone") { id = latin("aliquis", Pron); gender = M; }
    else if (p == "something" || p == "anything") { id = latin("aliquis", Pron); gender = N; }
    else if (p == "who") { id = latin("quis", Pron); gender = M; }
    else if (p == "what") { id = latin("quis", Pron); gender = N; }
    else if (p == "which") { id = latin("quī", Pron); gender = 0; }
    else if (p == "many") { id = latin("multus", Adj); number = Pl; gender = M; }
    else if (p == "few") { id = latin("paucus", Adj); number = Pl; gender = M; }
    else if (p == "both") { id = latin("ambō"); number = Pl; gender = M; }
    else if (p == "some") { id = latin("quīdam", Pron); number = Pl; gender = M; }   // C15: "some say" -> quīdam dīcunt
    else if (p == "other" || p == "others" || p == "another") { id = latin("alius", Adj); number = p == "others" ? Pl : Sg; }
    else if (p == "one-generic") {   // C22: "How can one read ...?" -> Quōmodo homō legere potest?
      id = latin("homō", Noun);
      if (id == kNone) id = latin("homo", Noun);
      number = Sg;
      gender = M;
      c.cover(n.tokens);
      if (id != kNone) {
        o = LaNP{};
        o.head = id;
        o.number = Sg;
        tableChoice(id, "one");
        return;
      }
    }
    else if ((p == "those" || p == "these") && !n.relative.empty()) {
      // C19: "to those who are poor" -> eīs quī pauperēs sunt
      o.isPronoun = true;
      o.pron.person = 3;
      o.pron.number = Pl;
      o.pron.gender = M;
      o.number = Pl;
      o.gender = M;
      tableChoice(latin("is", Pron), p);
      relativeInto(n, c, o);
      return;
    }
    else if (p == "those") { id = latin("ille", Pron); if (id == kNone) id = latin("ille", Det); number = Pl; gender = M; }
    else if (p == "these") { id = latin("hic", Pron); number = Pl; gender = M; }
    else if (p == "this" || p == "that" || p == "one" || p == "ones") {
      const bool anaphor = p == "one" || p == "ones";
      std::string det = anaphor ? n.determiner : p;
      gender = N;
      if (anaphor && c.mem.lastGender) gender = c.mem.lastGender;
      number = n.number == 2 ? Pl : Sg;
      if (anaphor && !n.adjectives.empty() && det.empty()) {
        // "red ones": the adjective is the head, agreeing with the noun it stands for
        Choice ch;
        ch.token = n.adjectives[0].token;
        id = select(n.adjectives[0].lemma, Adj, c.context, false, false, c.st, ch);
        c.out.choices.push_back(ch);
        if (id == kNone) c.out.unknownWords.push_back(n.adjectives[0].lemma);
        o.head = id;
        o.number = number;
        o.gender = gender ? gender : (uint8_t)M;
        return;
      }
      if (det == "that" && !anaphor) id = latin("is", Pron);   // C15: the pronoun "that" is id ("Id pendet ...")
      else if (det == "that") id = latin("ille", Det);
      else if (det == "this") id = latin("hic", Pron);
      else id = latin("is", Pron);
    } else {
      id = kNone;
    }
    if (id == kNone) {
      o.literal = n.surface;
      c.out.unknownWords.push_back(n.surface);
      return;
    }
    o.head = id;
    o.number = number;
    o.gender = gender;
    tableChoice(id, p);
    // C20: "something to eat" -> aliquid ad edendum (the to-infinitive was dropped)
    if (c.st.lang == frame::SrcLang::En && (p == "something" || p == "anything" || p == "nothing") && n.relative.size() == 1) {
      int first = -1;
      for (int t : n.relative[0].tokens)
        if (t >= 0 && (size_t)t < c.s.tokens.size() && c.s.tokens[(size_t)t].upos != "PUNCT" && (first < 0 || t < first)) first = t;
      if (first >= 0 && c.s.tokens[(size_t)first].lower == "to") relativeInto(n, c, o);
    }
    return;
  }
  // C15: names of names_la.tsv, also several words and "of" phrases; English capitalised common nouns as titles
  if (!n.isPronoun && !n.title && c.st.lang == frame::SrcLang::En && (n.isName || (n.token > 0 && (size_t)n.token < c.s.tokens.size() &&
                                                                     !c.s.tokens[(size_t)n.token].text.empty() &&
                                                                     c.s.tokens[(size_t)n.token].text[0] >= 'A' &&
                                                                     c.s.tokens[(size_t)n.token].text[0] <= 'Z'))) {
    if (nameTableInto(n, c, o)) { relativeInto(n, c, o); if (c.st.lang == frame::SrcLang::En) nameTail(false); return; }
    if (n.isName && !cd_.nameByEnglish(n.head) && titleNoun(n, c, o)) return;
    if (!n.isName && titleNoun(n, c, o)) return;
  }
  // names
  if (n.isName) {
    const std::string low = text::lower(n.head);
    if (const char* day = tables::weekday(low)) {   // "Tuesday" -> diēs Mārtis (RULE time.day)
      o.head = latin("diēs", Noun);
      LaNP g;
      g.head = latin(day, Name);
      if (g.head == kNone) { g.isName = true; g.name = day; }
      o.genitive.push_back(g);
      tableChoice(o.head, n.head);
      return;
    }
    const curated::NameEntry* esName = c.st.lang == frame::SrcLang::Es && !cd_.nameByEnglish(n.head) && !n.title
                                       ? cd_.nameByLatin(text::latin_key(n.head)) : nullptr;   // "Alicia" -> Alīcia
    const bool inTable = cd_.nameByEnglish(n.head) != nullptr || esName != nullptr;
    bool glossary = false;
    if (c.st.context)
      for (const rules::GlossaryEntry& g : c.st.context->glossary)
        if (g.name == n.head) glossary = true;
    if (!inTable && !glossary && (n.number == 2 || n.title)) {
      // a plural title word ("Queen of Hearts" -> Cordium): translate as a capitalised common noun
      Choice ch;
      ch.token = n.token;
      std::string sg = low;
      if (n.number == 2 && sg.size() > 3 && sg.back() == 's' && c.st.lang != frame::SrcLang::Es) sg.pop_back();
      o.head = select(sg, Noun, c.context, false, false, c.st, ch);
      c.out.choices.push_back(ch);
      if (o.head != kNone) {
        o.capitalise = true;
        for (const SemNP& g : n.genitive) {   // "la Reina de Corazones" -> Rēgīna Cordium (C13)
          LaNP x;
          npInto(g, c, x);
          x.capitalise = x.capitalise || g.isName;
          o.genitive.push_back(x);
          break;
        }
        return;
      }
    }
    if (!inTable && !glossary) {
      // a proper name the Latin lexicon knows (Rōma, Iuppiter)
      Choice ch;
      ch.token = n.token;
      std::vector<std::string> none;
      const uint32_t id = select(low, Name, none, false, false, c.st, ch);
      if (id != kNone && (la_.lemma(id).flags & lex::ProperName) && ch.candidates[0].score >= 0.5) {
        ch.kind = "name";
        c.out.choices.push_back(ch);
        o.head = id;
        o.isName = true;
        if (c.st.lang == frame::SrcLang::En) nameTail(true);
        return;
      }
    }
    o.isName = true;
    o.name = esName ? esName->english : n.head;
    Choice ch;
    ch.token = n.token;
    ch.source = n.head;
    ch.kind = "name";
    c.out.choices.push_back(ch);
    if (n.number == 2) o.number = Pl;
    // possessors / genitives of names ("Queen of Hearts")
    for (const SemNP& g : n.genitive) {
      LaNP x;
      npInto(g, c, x);
      x.capitalise = x.capitalise || g.isName;
      o.genitive.push_back(x);
      break;
    }
    if (c.st.lang == frame::SrcLang::En) nameTail(true);
    return;
  }
  // Spanish weekday written as a common noun ("Es martes.") -> diēs Mārtis (C13)
  if (c.st.lang == frame::SrcLang::Es && n.genitive.empty() && n.adjectives.empty())
    if (const char* day = tables::weekday(text::lower(n.head))) {
      o.head = latin("diēs", Noun);
      LaNP g;
      g.head = latin(day, Name);
      if (g.head == kNone) { g.isName = true; g.name = day; }
      o.genitive.push_back(g);
      tableChoice(o.head, n.head);
      return;
    }
  // C19: "a person who ...", "people who ...", "those who": the antecedent is the pronoun is ("eī quem Saga
  // ōsculāta est", "eīs quī pauperēs sunt"); a general noun adds nothing the relative clause does not say
  if (c.st.lang == frame::SrcLang::En && !n.relative.empty() && n.adjectives.empty() && n.possessor.empty() &&
      n.genitive.empty() && n.numeral.empty() && (n.determiner.empty() || n.determiner == "a" || n.determiner == "the") &&
      (text::lower(n.head) == "person" || text::lower(n.head) == "people")) {
    o.isPronoun = true;
    o.pron.person = 3;
    o.number = text::lower(n.head) == "people" || n.number == 2 ? Pl : Sg;
    o.pron.number = o.number;
    o.pron.gender = M;
    o.gender = M;
    Choice ch;
    ch.token = n.token;
    ch.source = n.head;
    ch.lemma = latin("is", Pron);
    ch.kind = "table";
    ch.note = "a person who -> is quī";
    c.out.choices.push_back(ch);
    c.cover(n.token);
    relativeInto(n, c, o);
    return;
  }
  // common nouns
  {
    Choice ch;
    ch.token = n.token;
    uint32_t id = kNone;
    if (n.head == "hour" && n.ordinal) id = latin("hōra", Noun);
    // C17: "men" in general (no article, no possessive, no adjective) are people: hominēs ("as men call me")
    if (c.st.lang == frame::SrcLang::En && text::lower(n.head) == "man" && n.number == 2 && n.determiner.empty() && !n.definite &&
        n.possessor.empty() && n.adjectives.empty() && n.numeral.empty())
      id = latin("homō", Noun);
    // the Spanish noun's grammatical gender helps only for persons and animals (hijo -> fīlius, niña -> puella);
    // for things it would leak into the Latin choice (el gato -> fēlēs f, la puerta -> iānua) (C13)
    // C17: an adjective standing as a noun ("wear white" -> album): the adjective, neuter, before any noun reading
    if (id == kNone && c.st.lang == frame::SrcLang::En && n.token >= 0 && (size_t)n.token < c.s.tokens.size() &&
        c.s.tokens[(size_t)n.token].upos == "ADJ" && n.adjectives.empty()) {
      Choice ca;
      ca.token = n.token;
      id = select(n.head, Adj, c.context, false, false, c.st, ca);
      if (id != kNone) ch = ca;
    }
    if (id == kNone) {
      id = select(n.head, Noun, c.context, false, false, c.st, ch, animate(n) ? n.srcGender : 0);
      // C17: consistency: a noun already rendered in this cue or a cue before keeps its Latin word when that word is
      // one of the candidates here too
      bool forced = false;
      for (const auto& ov : c.st.overrides) forced = forced || ov.first == n.token;
      if (id != kNone && ch.kind == "sense" && ch.candidates.size() > 1 && !forced) {
        const std::string key = text::lower(n.head);
        for (const auto& ns : c.mem.nounSense)
          if (ns.first == key && ns.second != id)
            for (const Candidate& k : ch.candidates)
              if (k.lemma == ns.second) {
                id = ns.second;
                ch.lemma = id;
                ch.note = "the same Latin word as before for \"" + key + "\"";
                break;
              }
      }
      if (id != kNone && ch.kind == "sense") {
        const std::string key = text::lower(n.head);
        auto it = std::find_if(c.mem.nounSense.begin(), c.mem.nounSense.end(),
                               [&](const std::pair<std::string, uint32_t>& p) { return p.first == key; });
        if (it != c.mem.nounSense.end()) it->second = id;
        else {
          if (c.mem.nounSense.size() >= 32) c.mem.nounSense.erase(c.mem.nounSense.begin());
          c.mem.nounSense.emplace_back(key, id);
        }
      }
    } else { ch.source = n.head; ch.lemma = id; ch.kind = "table"; }
    if (id == kNone && !n.head.empty()) {   // substantive adjective ("the dark")
      Choice c2;
      c2.token = n.token;
      id = select(n.head, Adj, c.context, false, false, c.st, c2);
      if (id != kNone) { ch = c2; o.gender = N; }
    }
    // C19: a compound the lexicon does not list ("snowman", "tinsmith", "milkmaid"): the head noun with the first
    // word as a genitive ("homō nivis", "faber stannī") or an adjective; marked as derived (Check through A9/margin)
    std::string cmpFirst, cmpHead;
    if (id == kNone && c.st.lang == frame::SrcLang::En && c.st.srcLex &&
        frame::en::compoundParts(*c.st.srcLex, text::lower(n.surface.empty() ? n.head : n.surface), cmpFirst, cmpHead)) {
      Choice c2;
      c2.token = n.token;
      const uint32_t hid = select(cmpHead, Noun, c.context, false, false, c.st, c2);
      if (hid != kNone) {
        id = hid;
        ch = c2;
        ch.source = n.head;
        ch.note = "compound: \"" + cmpFirst + "\" + \"" + cmpHead + "\"";
        c.out.flags.push_back("derived-word");
        Choice c3;
        c3.token = n.token;
        // C22: an adjective first word ("bluebird", "blackbird") is the adjective (avis caerulea), not a genitive
        const bool firstAdj = frame::en::colourWord(cmpFirst);
        const uint32_t fn = firstAdj ? kNone : select(cmpFirst, Noun, c.context, false, false, c.st, c3);
        if (fn != kNone) {
          LaNP g;
          g.head = fn;
          g.number = la_.lemma(fn).flags & lex::PluralOnly ? (uint8_t)Pl : (uint8_t)Sg;
          if (la_.lemma(fn).pos == Adj || la_.lemma(fn).pos == Participle) g.gender = N;
          o.genitive.push_back(g);
          c3.source = cmpFirst;
          c3.token = -1;
          c.out.choices.push_back(c3);
        } else {
          Choice c4;
          const uint32_t fa = select(cmpFirst, Adj, c.context, false, false, c.st, c4);
          if (fa != kNone) {
            LaAdj a;
            a.lemma = fa;
            o.adjectives.push_back(a);
            c4.source = cmpFirst;
            c4.token = -1;
            c.out.choices.push_back(c4);
          }
        }
      }
    }
    // C24: an invented word made of a preposition and a known noun ("overcloud", "underbridge"): the nearest reading,
    // the preposition with that noun ("sub terrā", "super nūbem"), written as fixed words and marked derived (Check)
    if (id == kNone && c.st.lang == frame::SrcLang::En && n.determiner.empty() && n.adjectives.empty()) {
      uint32_t prep = kNone, noun = kNone;
      uint8_t cs = 0;
      const std::string low = text::lower(n.surface.empty() ? n.head : n.surface);
      std::string form;
      if (inventedPrep(*this, low, c.st, c.context, n.token, c.out, prep, noun, cs) &&
          morph::generate(la_, noun, morph::nounForm(cs, Sg), form, true)) {
        o.fixed = morph::displayForm(la_.lemma(prep).head, true) + " " + form;
        c.cover(n.token);
        return;
      }
    }
    c.out.choices.push_back(ch);
    if (id == kNone) {
      if (!n.head.empty() || n.numeral.empty()) {
        o.literal = n.surface.empty() ? n.head : n.surface;
        c.out.unknownWords.push_back(o.literal);
      }
    } else {
      o.head = id;
      const lex::Lemma l = la_.lemma(id);
      if (l.flags & lex::PluralOnly) o.number = Pl;
      if (text::lower(n.head) == "people" && n.numeral.empty()) o.number = Pl;   // C15: people -> hominēs
      if (l.pos == Adj || l.pos == Participle) o.gender = N;   // a taught adjective as a noun: "in īmō"
      if (l.pos == Noun && !(l.flags & lex::PluralOnly) && pluralOnly(id)) o.number = Pl;   // tenebrae
      if (l.pos == Noun) {
        c.mem.lastGender = simpleGender(l.gender);
        c.mem.lastNumber = o.number;
        c.mem.lastAnimate = animate(n);
      }
    }
  }
  // determiners and quantifiers
  const std::string& d = n.determiner;
  auto addAdj = [&](const char* head, const std::string& src) {
    LaAdj a;
    a.lemma = latin(head, Adj);
    if (a.lemma == kNone) a.lemma = latin(head, Det);
    if (a.lemma == kNone) return;
    o.adjectives.push_back(a);
    tableChoice(a.lemma, src);
  };
  // "no songs" / "not any songs" -> nūllum carmen: nūllus takes the singular (unless the noun has no singular)
  auto nullusSingular = [&]() {
    if (o.head != kNone && !(la_.lemma(o.head).flags & lex::PluralOnly) && !pluralOnly(o.head)) o.number = Sg;
  };
  if (d == "this") o.det = realise::Det::Hic;
  else if (d == "that") o.det = realise::Det::Ille;
  else if (d == "no") { addAdj("nūllus", d); c.negative = true; nullusSingular(); }
  else if (d == "any") {
    if (c.negative) { addAdj("nūllus", d); nullusSingular(); }
    else if (c.question) addAdj("ūllus", d);
  } else if (d == "every") addAdj("omnis", d);
  else if (d == "all" && n.number == 1 && c.st.lang == frame::SrcLang::En && latin("tōtus", Adj) != kNone) addAdj("tōtus", d);   // C17: "all night" -> tōtam noctem
  else if (d == "all") { addAdj("omnis", d); o.number = Pl; }
  else if (d == "many") { addAdj("multus", d); o.number = Pl; }
  else if (d == "few") { addAdj("paucus", d); o.number = Pl; }
  else if (d == "much") addAdj("multus", d);
  else if (d == "another" || d == "other") addAdj("alius", d);
  else if (d == "a little") {   // "un poco de té" -> paulum thēae (partitive genitive) (C13)
    const uint32_t paulum = latin("paulum", Noun);
    if (paulum != kNone && o.head != kNone) {
      LaNP g = o;
      g.case_ = Gen;
      o = LaNP{};
      o.head = paulum;
      o.number = Sg;
      o.gender = N;
      o.genitive.push_back(g);
      tableChoice(paulum, "poco");
      return;
    }
  }
  if (n.interrogative) {
    if (n.wh == "how many") o.interrogative = latin("quot");
    else if (n.wh == "how much") o.interrogative = latin("quantus", Adj);
    else o.interrogative = latin("quī", Pron);
  }
  // possessor
  for (const SemNP& p : n.possessor) {
    if (p.isPronoun && p.pron.person > 0) {
      uint8_t num = p.pron.number == 2 ? Pl : p.pron.number == 1 ? Sg : (c.mem.addresseePlural ? Pl : Sg);
      if (p.pron.person == 3) {
        // reflexive suus when the clause subject is the same 3rd person, else eius / eōrum
        const uint8_t pg = p.pronLemma == "its" ? (uint8_t)0 : p.pron.gender;   // C17: "its" fits any Latin gender
        // C19: not inside the subject itself ("The king and his sons are here": fīliī eius) nor in a verbless fragment
        auto inSubject = [&]() {
          const frame::SemNP& sj = c.frame->subject;
          if (std::find(sj.tokens.begin(), sj.tokens.end(), n.token) != sj.tokens.end()) return true;
          for (const frame::SemNP& k : sj.coord)
            if (k.token == n.token || std::find(k.tokens.begin(), k.tokens.end(), n.token) != k.tokens.end()) return true;
          return false;
        };
        // C24: a coordinated verb without its own subject shares the first clause's subject ("... and said goodbye
        // to his mother" -> mātrī suae)
        const bool sharedSubj = c.frame && !c.frame->hasSubject && c.frame->hasPred && c.frame->type == frame::Kind::Decl &&
                                c.st.lang == frame::SrcLang::En;
        if (c.frame && (c.frame->hasSubject || sharedSubj) && c.subjPerson == 3 && c.frame->subject.token != n.token &&
            !inSubject() && (c.frame->hasPred || c.frame->type != frame::Kind::Frag) &&
            c.subjNumber == (num == Pl ? 2 : 1) && (pg == 0 || c.subjGender == 0 || pg == c.subjGender ||
                                                    num == Pl)) {
          o.possessive = latin("suus", Det);
        } else {
          LaNP g;
          g.isPronoun = true;
          g.pron.person = 3;
          g.pron.number = num;
          g.pron.gender = p.pron.gender ? p.pron.gender : (uint8_t)M;
          g.number = num;
          o.genitive.push_back(g);
        }
      } else {
        o.possessive = latin(p.pron.person == 1 ? (num == Pl ? "noster" : "meus") : (num == Pl ? "vester" : "tuus"), Det);
      }
      tableChoice(o.possessive, p.pronLemma);
    } else {
      LaNP g;
      npInto(p, c, g);
      o.genitive.push_back(g);
    }
    break;
  }
  // adjectives
  for (const frame::SemAdj& a : n.adjectives) {
    // an adjective the phrasebook renders as a verb phrase ("impossible" -> fierī nōn potest): attributive use is a
    // relative clause agreeing with the noun ("sex rēs quae fierī nōn possunt")
    if (n.relative.empty() && o.relative.empty() && a.adverbs.empty()) {
      LaClause rc;
      const curated::PhraseEntry* pe = nullptr;
      for (const curated::PhraseEntry& e : c.st.lang == frame::SrcLang::Es ? cd_.phrasebookEs() : cd_.phrasebook())
        if (e.reg == "state" && text::lower(e.pattern) == text::lower(a.lemma)) { pe = &e; break; }
      if (pe && latinVerbPhrase(pe->latin, rc)) {
        rc.relRole = realise::Role::Subject;
        o.relative.push_back(rc);
        Choice ch;
        ch.token = a.token;
        ch.source = a.lemma;
        ch.lemma = rc.pred.modal != kNone ? rc.pred.modal : rc.pred.lemma;
        ch.kind = "phrasebook";
        ch.note = "phrasebook: " + a.lemma + " -> quī " + pe->latin;
        c.out.choices.push_back(ch);
        c.cover(a.token);
        continue;
      }
    }
    Choice ch;
    ch.token = a.token;
    LaAdj la;
    la.degree = a.degree;
    // C22: "important" is a genitive of quality (rēs magnī mōmentī; "very / awfully important" -> maximī mōmentī)
    if (c.st.lang == frame::SrcLang::En && text::lower(a.lemma) == "important" && a.participle == 0) {
      bool very = false;
      for (const std::string& v : a.adverbs) very = very || v == "very" || v == "awfully" || v == "really" || v == "terribly" || v == "so";
      LaNP g = momentNP(*this, very);
      o.genitive.push_back(g);
      ch.source = a.lemma;
      ch.lemma = latin("mōmentum", Noun);
      ch.kind = "table";
      ch.note = std::string("genitive of quality: ") + (very ? "maximī" : "magnī") + " mōmentī";
      c.out.choices.push_back(ch);
      c.cover(a.token);
      for (int t : a.advTokens) c.cover(t);
      continue;
    }
    la.lemma = adjectiveInto(a, c, la, ch);
    // C22: "all the other animals" -> cētera animālia (not omnia alia); "the other X" keeps alius
    if (c.st.lang == frame::SrcLang::En && text::lower(a.lemma) == "other" && n.number == 2 && n.determiner == "all" &&
        latin("cēterus", Adj) != kNone) {
      la.lemma = latin("cēterus", Adj);
      ch.lemma = la.lemma;
      ch.kind = "table";
      ch.note = "the other (plural) -> cēterī";
      const uint32_t omn = latin("omnis", Adj);
      o.adjectives.erase(std::remove_if(o.adjectives.begin(), o.adjectives.end(),
                                        [&](const LaAdj& x) { return x.lemma == omn; }), o.adjectives.end());
      la.before = true;
    }
    // C22: two English adjectives that give the same Latin word ("very extra special powers") are said once
    if (la.lemma != kNone) {
      bool dup = false;
      for (const LaAdj& x : o.adjectives) dup = dup || x.lemma == la.lemma;
      if (dup) { c.out.choices.push_back(ch); c.cover(a.token); continue; }
    }
    // C19: a modifier with no Latin adjective but a Latin noun ("the poppy bed"): a noun compound, i.e. a genitive
    // (ager papāverum); the "bed" of plants is a plot (ager)
    if (la.lemma == kNone && c.st.lang == frame::SrcLang::En && !a.participle && a.adverbs.empty() && n.genitive.empty() &&
        o.genitive.empty()) {
      Choice cn;
      cn.token = a.token;
      const uint32_t nid = select(a.lemma, Noun, c.context, false, false, c.st, cn);
      if (nid != kNone) {
        LaNP g;
        g.head = nid;
        const bool plant = tables::plantNoun(text::lower(a.lemma));
        g.number = plant ? Pl : Sg;
        if (plant && text::lower(n.head) == "bed" && latin("ager", Noun) != kNone) {
          o.head = latin("ager", Noun);
          for (Choice& x : c.out.choices)
            if (x.token == n.token && x.kind == "sense") { x.lemma = o.head; x.note = "a garden bed of plants"; x.kind = "table"; }
        }
        o.genitive.push_back(g);
        c.out.choices.push_back(cn);
        c.cover(a.token);
        continue;
      }
    }
    c.out.choices.push_back(ch);
    c.cover(a.token);
    if (la.lemma == kNone) { c.out.unknownWords.push_back(a.lemma); continue; }
    for (size_t i = 0; i < a.adverbs.size(); ++i) {
      const uint32_t av = adverb(a.adverbs[i], i < a.advTokens.size() ? a.advTokens[i] : -1, c, false);
      if (av != kNone) la.adverbs.push_back(av);
    }
    // C17: an adjective coordinated with the previous one ("a big and ugly dog" -> canem magnum et foedum)
    la.coord = !o.adjectives.empty() && a.token >= 0 && (size_t)a.token < c.s.tokens.size() &&
               c.s.tokens[(size_t)a.token].deprel == "conj";
    o.adjectives.push_back(la);
  }
  // C20: "such a big dog" -> tantum canem (size: tantus), "such a loud voice" -> tam magnam vōcem, "such a noise" ->
  // tālem strepitum; "so many apples" -> tot māla, "so much water" -> tanta aqua. "such" was dropped silently (OK).
  if (c.st.lang == frame::SrcLang::En && !o.isPronoun && o.literal.empty()) {
    int soTok = -1;
    if (d == "many" || d == "much")
      for (int k : n.tokens)
        if (k > 0 && (size_t)k < c.s.tokens.size() && c.s.tokens[(size_t)k].lower == d &&
            c.s.tokens[(size_t)k - 1].lower == "so")
          soTok = k - 1;
    const uint32_t tantus = latin("tantus", Adj), tam = latin("tam", Adv);
    if (soTok >= 0) {
      const uint32_t multus = latin("multus", Adj), tot = latin("tot");
      for (LaAdj& a : o.adjectives)
        if (a.lemma == multus && multus != kNone) {
          const uint32_t w = d == "many" ? tot : tantus;
          if (w != kNone) {
            a.lemma = w;
            a.before = true;
            tableChoice(w, "so " + d);
            c.cover(soTok);
          }
          break;
        }
    } else if (d == "such") {
      if (!o.adjectives.empty()) {
        LaAdj& a = o.adjectives[0];
        if (a.lemma == latin("magnus", Adj) && a.participle == 0 && tantus != kNone && a.adverbs.empty() && a.degree == 0)
          a.lemma = tantus;
        else if (tam != kNone) a.adverbs.insert(a.adverbs.begin(), tam);
        a.before = true;
        tableChoice(a.lemma == tantus ? tantus : tam, "such");
      } else if (o.adjectives.empty()) {
        LaAdj a;
        a.lemma = latin("tālis", Adj);
        a.before = true;
        if (a.lemma != kNone) { o.adjectives.push_back(a); tableChoice(a.lemma, "such"); }
      }
    }
  }
  // numerals
  if (!n.numeral.empty()) {
    const int v = n.numeralValue;
    if (n.ordinal) {
      if (const char* ord = tables::ordinal(v)) {
        LaAdj a;
        a.lemma = latin(ord, Adj);
        if (a.lemma != kNone) { o.adjectives.push_back(a); tableChoice(a.lemma, n.numeral); }
      }
    } else if (const char* card = tables::cardinal(v)) {
      o.numeral = latin(card, v == 1 ? Adj : Num);
      if (o.numeral == kNone) o.numeral = latin(card);
      tableChoice(o.numeral, n.numeral);
      if (v > 1) o.number = Pl;
    } else if (v > 20 && v < 100 && v % 10 >= 2 && tables::cardinal(v - v % 10) && tables::cardinal(v % 10)) {
      // C19: "twenty-two cows" -> vīgintī duae vaccae (the units word agrees)
      o.numeral = latin(tables::cardinal(v - v % 10), Num);
      if (o.numeral == kNone) o.numeral = latin(tables::cardinal(v - v % 10));
      LaAdj u;
      u.lemma = latin(tables::cardinal(v % 10), Num);
      if (u.lemma == kNone) u.lemma = latin(tables::cardinal(v % 10));
      u.before = true;
      if (u.lemma != kNone) o.adjectives.insert(o.adjectives.begin(), u);
      tableChoice(o.numeral, n.numeral);
      o.number = Pl;
    } else if (v > 1 && v < 4000) {
      // C19: any other number in words: Roman numerals, as Latin writes them ("XXI ovēs"); never dropped
      static const std::pair<int, const char*> kRoman[] = {{1000, "M"}, {900, "CM"}, {500, "D"}, {400, "CD"},
                                                           {100, "C"},  {90, "XC"},  {50, "L"},  {40, "XL"},
                                                           {10, "X"},   {9, "IX"},   {5, "V"},   {4, "IV"}, {1, "I"}};
      int r = v;
      for (const auto& rm : kRoman)
        while (r >= rm.first) { o.numeralLiteral += rm.second; r -= rm.first; }
      o.number = Pl;
      for (int k : n.tokens)
        if (k >= 0 && (size_t)k < c.s.tokens.size() && c.s.tokens[(size_t)k].upos == "NUM") c.cover(k);
    }
  }
  // "of" attributes (one genitive)
  for (const SemNP& g : n.genitive) {
    LaNP x;
    // C24: an -ing word before its noun read as a noun compound ("the babbling water", "a roaring lion"): the present
    // participle of its verb (rīvus murmurāns, leō rugiēns) when English has no noun of that form or Latin only a rare
    // one (stultiloquium); "a dining room" with a common Latin noun stays a genitive
    if (c.st.lang == frame::SrcLang::En && c.st.srcLex && g.token >= 0 && g.token < n.token &&
        (size_t)g.token < c.s.tokens.size() && g.adjectives.empty() && g.possessor.empty() && g.genitive.empty() &&
        g.determiner.empty() && !g.isName && !g.isPronoun && g.relative.empty()) {
      const std::string low = c.s.tokens[(size_t)g.token].lower;
      if (low.size() > 5 && low.compare(low.size() - 3, 3, "ing") == 0) {
        std::vector<lex::Analysis> an;
        c.st.srcLex->lookup(text::en_key(low), an);
        std::string verb;
        bool noun = false;
        for (const lex::Analysis& z : an) {
          const lex::Lemma zl = c.st.srcLex->lemma(z.lemma);
          if (zl.pos == Noun) noun = true;
          if (zl.pos == Verb && verb.empty()) verb = text::lower(std::string(zl.head));
        }
        bool participle = !verb.empty() && !noun;
        if (!verb.empty() && noun) {
          Choice probe;
          probe.token = g.token;
          const uint32_t nid = select(g.head, Noun, c.context, false, false, c.st, probe);
          participle = nid == kNone || cd_.effectiveTier(la_.lemma(nid).key, la_.lemma(nid).pos, la_.lemma(nid).tier) >= 3;
        }
        if (participle) {
          frame::SemAdj a;
          a.lemma = verb;
          a.token = g.token;
          a.participle = 2;
          Choice ch;
          LaAdj la;
          la.lemma = adjectiveInto(a, c, la, ch);
          if (la.lemma != kNone) {
            ch.note += "; an -ing word before its noun";
            c.out.choices.push_back(ch);
            c.cover(g.tokens);
            c.cover(g.token);
            o.adjectives.push_back(la);
            continue;
          }
        }
      }
    }
    // C22: "history" as an attribute ("a history lesson", "the history book") -> rērum gestārum (the teacher's choice)
    if (c.st.lang == frame::SrcLang::En && text::lower(g.head) == "history" && g.adjectives.empty() && g.possessor.empty()) {
      c.cover(g.tokens);
      x.fixed = "rērum gestārum";
      o.genitive.push_back(x);
      Choice ch;
      ch.token = g.token;
      ch.source = "history";
      ch.lemma = latin("rēs", Noun);
      ch.kind = "table";
      ch.note = "history -> rēs gestae (genitive rērum gestārum)";
      c.out.choices.push_back(ch);
      continue;
    }
    npInto(g, c, x);
    // C19: a garden bed of plants ("flower bed", "poppy bed", "cabbage bed") is a plot, ager + genitive plural
    // (ager flōrum / papāverum), not a bed to sleep in
    if (c.st.lang == frame::SrcLang::En && text::lower(n.head) == "bed" && !g.isName && !g.isPronoun &&
        g.determiner.empty() && g.possessor.empty() &&
        tables::plantNoun(text::lower(g.head)) && latin("ager", Noun) != kNone) {
      o.head = latin("ager", Noun);
      if (!x.isName && x.head != kNone && !(la_.lemma(x.head).flags & lex::PluralOnly)) x.number = Pl;
      for (Choice& ch : c.out.choices)
        if (ch.token == n.token && ch.kind == "sense") { ch.lemma = o.head; ch.note = "a garden bed of plants"; ch.kind = "table"; }
    }
    o.genitive.push_back(x);
    break;
  }
  relativeInto(n, c, o);
  // coordination
  for (const SemNP& k : n.coord) {
    LaNP x;
    npInto(k, c, x);
    o.coord.push_back(x);
  }
  // C15: "my aunt and uncle" -> amitam et avunculum meum: a shared possessive goes with the last conjunct
  if (!o.coord.empty() && o.possessive != kNone && o.coord.back().possessive == kNone && !o.coord.back().isPronoun &&
      !o.coord.back().isName) {
    o.coord.back().possessive = o.possessive;
    o.possessive = kNone;
  }
  // "ni perros ni gatos" -> neque canēs neque fēlēs: neque carries the negation (the clause loses its nōn) (C13)
  if (!o.coord.empty() && n.coordConj == "nor") {
    o.coordConj = latin("neque", Conj);
    o.coordBoth = o.coordConj != kNone;
    if (o.coordBoth) c.negConsumed = true;
  } else if (!o.coord.empty() && n.coordConj == "or") {
    o.coordConj = latin("aut", Conj);
  }
}

LaNP Transfer::np(const SemNP& n, const SemSentence& s, const Settings& st, Memory& mem, ClauseOut& out) const {
  Ctx c(s, st, mem, out);
  LaNP o;
  npInto(n, c, o);
  return o;
}

// ---- obliques -------------------------------------------------------------------------------------------------------
bool Transfer::obliqueInto(const frame::SemOblique& ob, Ctx& c, LaClause& cl) const {
  const SemNP& n = ob.np;
  const std::string prep = ob.prep;
  c.cover(ob.token);
  const std::string head = text::lower(n.head);
  // C20: "three times", "ten times" -> ter, deciēns (numeral adverbs); "many times" -> saepe, "several times" ->
  // aliquotiēns ("in iānuā tribus temporibus" was a Check)
  if (c.st.lang == frame::SrcLang::En && (prep.empty() || prep == "-") && (head == "time" || head == "times") &&
      n.number == 2 && !n.ordinal && n.adjectives.empty() && n.genitive.empty() && n.relative.empty()) {
    static const std::pair<int, const char*> kTimes[] = {{2, "bis"},      {3, "ter"},     {4, "quater"},  {5, "quīnquiēs"},
                                                         {6, "sexiēs"},   {7, "septiēs"}, {8, "octiēs"},  {9, "noviēs"},
                                                         {10, "deciēns"}, {100, "centiēns"}, {1000, "mīlliēs"}};
    const char* adv = n.determiner == "many" ? "saepe" : n.determiner == "several" ? "aliquotiēns" : nullptr;
    if (!n.numeral.empty())
      for (const auto& kt : kTimes)
        if (kt.first == n.numeralValue) adv = kt.second;
    const uint32_t id = adv ? latin(adv, Adv) : kNone;
    if (id != kNone) {
      cl.adverbs.push_back(realise::LaAdverb{id, ob.front ? realise::AdvPos::Front : realise::AdvPos::Auto});
      Choice ch;
      ch.token = n.token;
      ch.source = (n.numeral.empty() ? n.determiner : n.numeral) + " times";
      ch.lemma = id;
      ch.kind = "table";
      c.out.choices.push_back(ch);
      c.cover(n.tokens);
      return true;
    }
  }
  const bool time = tables::timeNoun(head) || tables::weekday(head) != nullptr;
  // C24: "they ran the other road": a place noun without a preposition after a verb of motion is the goal (in + acc)
  int npFirst = n.token;
  for (int t : n.tokens) if (t >= 0 && t < npFirst) npFirst = t;
  // a path word before the noun ("ran down the hill", "walked along the road") is its preposition, not a goal
  bool afterParticle = false;
  if (npFirst > 0 && (size_t)npFirst <= c.s.tokens.size()) {
    const std::string pw = c.s.tokens[(size_t)npFirst - 1].lower;
    afterParticle = pw == "down" || pw == "up" || pw == "along" || pw == "across" || pw == "through" || pw == "over" ||
                    pw == "past" || pw == "around" || pw == "round" || pw == "by";
  }
  if (c.st.lang == frame::SrcLang::En && (prep.empty() || prep == "-") && c.motion && !time && !n.isPronoun && !afterParticle &&
      !n.isName && n.numeral.empty() && head != "home" && head != "way" && n.determiner != "every" && n.determiner != "all") {
    frame::SemOblique o2 = ob;
    o2.prep = "into";
    return obliqueInto(o2, c, cl);
  }
  auto bare = [&](uint8_t cs) {
    LaOblique o;
    o.case_ = cs;
    npInto(n, c, o.np);
    o.np.case_ = cs;
    o.front = ob.front;
    cl.obliques.push_back(o);
    return true;
  };
  auto withPrep = [&](const char* latinPrep, uint8_t cs) {
    LaOblique o;
    o.prep = latin(latinPrep, Prep);
    if (o.prep == kNone) return bare(cs);
    o.case_ = cs;
    npInto(n, c, o.np);
    o.np.case_ = cs;
    o.front = ob.front;
    cl.obliques.push_back(o);
    Choice ch;
    ch.token = ob.token;
    ch.source = prep;
    ch.lemma = o.prep;
    ch.kind = "table";
    c.out.choices.push_back(ch);
    return true;
  };
  // C19: "find that out for yourself", "I did it by myself": emphatic ipse in the nominative agreeing with the subject
  // (the person addressed or speaking: gender from the speaker glossary, Check), before the verb ("id ipsa invenīre
  // dēbēs")
  bool forSelf = false;
  if (prep == "for" && c.frame)
    for (const char* v : {"find", "see", "judge", "decide", "learn", "discover", "look", "try"})
      forSelf = forSelf || c.frame->pred.lemma == v;
  if (c.st.lang == frame::SrcLang::En && (forSelf || prep == "by") && n.isPronoun && n.pron.reflexive &&
      n.pron.person >= 1 && latin("ipse", Pron) != kNone) {
    LaOblique o;
    o.case_ = Nom;
    o.np.head = latin("ipse", Pron);
    o.np.number = n.pron.number == 2 ? Pl : Sg;
    char g = c.st.speakerGender;
    if (c.st.flipSpeakerGender) g = g == 'f' ? 'm' : 'f';
    uint8_t gen = n.pron.gender;
    if (n.pron.person < 3 || !gen) {
      gen = g == 'f' ? F : M;
      c.out.flags.push_back("speaker-gender");
    }
    o.np.gender = gen;
    o.np.case_ = Nom;
    cl.obliques.push_back(o);
    Choice ch;
    ch.token = ob.token;
    ch.source = prep + " " + head;
    ch.lemma = o.np.head;
    ch.kind = "table";
    ch.note = "emphatic ipse";
    c.out.choices.push_back(ch);
    return true;
  }
  // C19: an apposition to the object set off by commas ("save our friend, the Cowardly Lion, who is asleep ..."): the
  // object's case, after the verb when it carries a relative clause (the heavy part last)
  if (c.st.lang == frame::SrcLang::En && prep.empty() && c.frame && c.frame->hasObject && !time && ob.token > 0 &&
      (size_t)ob.token < c.s.tokens.size() && c.frame->object.token >= 0 && c.frame->object.token < ob.token) {
    int first = ob.token;
    for (int k : n.tokens) first = std::min(first, k);
    if (first > 0 && c.s.tokens[(size_t)first - 1].text == ",") {
      LaOblique o;
      o.case_ = Acc;
      npInto(n, c, o.np);
      o.np.case_ = Acc;
      o.after = !n.relative.empty();
      cl.obliques.push_back(o);
      return true;
    }
  }
  // C22: "a cat with a bell", "a rabbit with a waistcoat": "with" + a thing describing a noun is cum + ablative after
  // that noun (having it), not the bare ablative of means before the verb
  if (c.st.lang == frame::SrcLang::En && prep == "with" && ob.token >= 0 && (size_t)ob.token < c.s.tokens.size() &&
      c.s.tokens[(size_t)ob.token].deprel == "nmod" && n.determiner != "no" && latin("cum", Prep) != kNone) {
    const int hd = c.s.tokens[(size_t)ob.token].head - 1;
    if (hd >= 0 && (size_t)hd < c.s.tokens.size() && c.s.tokens[(size_t)hd].upos == "NOUN") {
      LaOblique o;
      o.prep = latin("cum", Prep);
      o.case_ = Abl;
      npInto(n, c, o.np);
      o.np.case_ = Abl;
      o.after = true;
      cl.obliques.push_back(o);
      return true;
    }
  }
  // C22: "nothing but bread", "nothing but flowers" -> nihil nisi pānem / flōrēs: "but" (except) takes the case of the
  // negative pronoun it limits
  if (c.st.lang == frame::SrcLang::En && prep == "but" && c.frame) {
    const frame::SemFrame& fr = *c.frame;
    auto negPron = [](const SemNP& x) {
      return x.isPronoun && (x.pronLemma == "nothing" || x.pronLemma == "nobody" || x.pronLemma == "everything" ||
                             x.pronLemma == "everyone" || x.pronLemma == "all" || x.pronLemma == "anything");
    };
    uint8_t cs = 0;
    if (fr.hasObject && negPron(fr.object)) cs = Acc;
    else if (!fr.predicative.empty() && negPron(fr.predicative[0])) cs = Nom;
    else if (fr.hasSubject && negPron(fr.subject)) cs = Nom;
    uint32_t nisi = latin("nisi", Conj);
    if (nisi == kNone) nisi = latin("nisi");
    if (cs && nisi != kNone) {
      LaOblique o;
      o.prep = nisi;
      o.case_ = cs;
      npInto(n, c, o.np);
      o.np.case_ = cs;
      o.after = cs == Nom && !fr.predicative.empty();
      cl.obliques.push_back(o);
      return true;
    }
  }
  // C22: "a book with no pictures in it": "in it" repeats the noun (liber sine pictūrīs): not translated
  if (c.st.lang == frame::SrcLang::En && prep == "in" && n.isPronoun && (n.pronLemma == "it" || n.pronLemma == "them") && c.frame) {
    bool withNo = false;
    for (const frame::SemOblique& o2 : c.frame->obliques) withNo = withNo || (o2.prep == "with" && o2.np.determiner == "no");
    if (withNo) { c.cover(n.tokens); c.cover(n.token); return true; }
  }
  // C22: "with no pictures", "with no shoes" -> sine pictūrīs, sine calceīs (not "nūllīs pictūrīs")
  if (c.st.lang == frame::SrcLang::En && prep == "with" && n.determiner == "no" && latin("sine", Prep) != kNone) {
    SemNP m = n;
    m.determiner.clear();
    m.negative = false;
    LaOblique o;
    o.prep = latin("sine", Prep);
    o.case_ = Abl;
    const bool neg = c.negative;
    npInto(m, c, o.np);
    c.negative = neg;
    o.np.case_ = Abl;
    o.front = ob.front;
    cl.obliques.push_back(o);
    for (int t : n.tokens)
      if (t >= 0 && (size_t)t < c.s.tokens.size() && c.s.tokens[(size_t)t].lower == "no") c.cover(t);
    return true;
  }
  // C22: "like a bird", "just like people" (a comparison) -> sīcut + the nominative of the one compared ("Sīcut avis
  // canit."); the preposition table's bare ablative was "sīcut ave"
  if (c.st.lang == frame::SrcLang::En && prep == "like" && !(n.isPronoun && n.pronLemma == "what") && !time &&
      latin("sīcut", Adv) != kNone) {
    LaOblique o;
    o.prep = latin("sīcut", Adv);
    o.case_ = Nom;
    npInto(n, c, o.np);
    o.np.case_ = Nom;
    o.front = ob.front;
    cl.obliques.push_back(o);
    return true;
  }
  // C17: "as a great Head" (in the role of) -> ut Caput magnum: ut + the case of the subject it describes
  bool asAs = false;   // "as tall as her mother" is a comparison (tam ... quam), not a role
  if (c.frame) {
    for (const frame::SemAdj& a : c.frame->predAdj)
      for (const std::string& v : a.adverbs) asAs = asAs || v == "as";
    for (const frame::SemAdverb& a : c.frame->adverbs) asAs = asAs || a.lemma == "as";
  }
  if (c.st.lang == frame::SrcLang::En && prep == "as" && !n.isPronoun && !time && !asAs) {
    uint32_t ut = latin("ut", Conj);
    if (ut == kNone) ut = latin("ut");
    if (ut != kNone) {
      LaOblique o;
      o.prep = ut;
      o.case_ = Nom;
      npInto(n, c, o.np);
      o.np.case_ = Nom;
      if (c.subjGender == F && animate(n) && o.np.head != kNone && !o.np.isName) o.np.head = feminineOf(o.np.head);
      o.front = ob.front;
      cl.obliques.push_back(o);
      return true;
    }
  }
  // C17: "at home" -> domī, "from home" -> domō (no preposition with domus)
  if (c.st.lang == frame::SrcLang::En && head == "home" && n.adjectives.empty() && n.possessor.empty() &&
      n.determiner.empty() && (prep == "at" || prep == "from") && latin("domus", Noun) != kNone) {
    LaOblique o;
    o.case_ = prep == "at" ? (uint8_t)Loc : (uint8_t)Abl;
    o.np.head = latin("domus", Noun);
    o.np.case_ = o.case_;
    o.front = ob.front;
    cl.obliques.push_back(o);
    c.cover(n.tokens);
    return true;
  }
  // C17: "for the third (and last) time" -> tertium (et ultimum): neuter ordinal adverbs, at the front of the clause
  if (c.st.lang == frame::SrcLang::En && (prep == "for" || prep.empty()) && head == "time" && n.number == 1 &&
      !n.adjectives.empty() && n.genitive.empty() && n.relative.empty()) {
    static const std::pair<const char*, const char*> kOrd[] = {
        {"first", "prīmum"}, {"second", "iterum"}, {"third", "tertium"}, {"fourth", "quārtum"}, {"fifth", "quīntum"},
        {"last", "ultimum"}, {"final", "ultimum"}};
    std::string fixed;
    bool all = true;
    for (size_t i = 0; i < n.adjectives.size() && all; ++i) {
      const char* la = nullptr;
      for (const auto& o2 : kOrd)
        if (n.adjectives[i].lemma == o2.first) la = o2.second;
      if (!la) { all = false; break; }
      if (!fixed.empty()) fixed += " et ";
      fixed += la;
    }
    if (all) {
      LaOblique o;
      o.np.fixed = fixed;
      o.front = true;
      cl.obliques.push_back(o);
      c.cover(n.tokens);
      return true;
    }
  }
  // C17: "into his presence" / "to my presence" -> ad sē / ad mē, "in your presence" -> cōram tē: the presence is the
  // person
  if (c.st.lang == frame::SrcLang::En && head == "presence" && n.possessor.size() == 1 && n.possessor[0].isPronoun &&
      (prep == "to" || prep == "into" || prep == "in" || prep == "before")) {
    SemNP who = n.possessor[0];
    const std::string pl = who.pronLemma;
    who.pronLemma = pl == "my" ? "me" : pl == "your" ? "you" : pl == "his" ? "him" : pl == "our" ? "us"
                  : pl == "their" ? "them" : pl == "its" ? "it" : pl;   // "her" stays
    who.head.clear();
    who.tokens.insert(who.tokens.end(), n.tokens.begin(), n.tokens.end());
    frame::SemOblique o2 = ob;
    o2.np = who;
    c.cover(n.tokens);
    const bool ad = prep == "to" || prep == "into";
    LaOblique o;
    o.prep = latin(ad ? "ad" : "cōram", Prep);
    if (o.prep == kNone) return false;
    o.case_ = ad ? (uint8_t)Acc : (uint8_t)Abl;
    npInto(who, c, o.np);
    o.np.case_ = o.case_;
    if (o.np.isPronoun && o.np.pron.person == 3 && c.subjPerson == 3) o.np.pron.reflexive = true;
    o.front = ob.front;
    cl.obliques.push_back(o);
    return true;
  }
  // C15: "tomorrow morning" -> crās māne, "this evening" -> hodiē vesperī, "in the morning" -> māne
  if ((prep.empty() || prep == "in" || prep == "on") && (head == "morning" || head == "evening" || head == "night") &&
      n.adjectives.empty() && n.possessor.empty()) {
    const char* day = nullptr;
    for (const SemNP& g : n.genitive) {
      const std::string gh = text::lower(g.head);
      day = gh == "tomorrow" ? "crās" : gh == "yesterday" ? "herī" : gh == "today" ? "hodiē" : nullptr;
    }
    if (!day && n.determiner == "this") day = "hodiē";
    if (day || n.determiner.empty() || prep == "in") {
      const char* part = head == "morning" ? "māne" : head == "evening" ? "vesperī" : "nocte";
      for (const char* w : {day, part}) {
        if (!w) continue;
        realise::LaAdverb a;
        a.lemma = latin(w, Adv);
        if (a.lemma == kNone) a.lemma = latin(w);
        if (a.lemma == kNone) continue;
        a.pos = ob.front ? realise::AdvPos::Front : realise::AdvPos::BeforeVerb;
        cl.adverbs.push_back(a);
        Choice ch;
        ch.token = n.token;
        ch.source = head;
        ch.lemma = a.lemma;
        ch.kind = "table";
        c.out.choices.push_back(ch);
      }
      c.cover(n.tokens);
      return true;
    }
  }
  // time adverbs as nouns ("today", "tomorrow")
  if (prep.empty() || ((prep == "on" || prep == "at" || prep == "in") && time)) {
    if (const char* adv = tables::adverb(head, false)) {
      if (n.determiner.empty() && n.adjectives.empty()) {
        realise::LaAdverb a;
        a.lemma = latin(adv, Adv);
        a.pos = ob.front ? realise::AdvPos::Front : realise::AdvPos::BeforeVerb;
        if (a.lemma != kNone) {
          c.cover(n.tokens);
          cl.adverbs.push_back(a);
          Choice ch;
          ch.token = n.token;
          ch.source = head;
          ch.lemma = a.lemma;
          ch.kind = "table";
          c.out.choices.push_back(ch);
          return true;
        }
      }
    }
    if (prep.empty()) return bare(time && head != "night" ? Abl : Acc);
    return bare(Abl);
  }
  // languages: "in Latin" -> Latīnē
  if ((prep == "in") && (n.isName || c.st.lang == frame::SrcLang::Es)) {
    if (const char* adv = tables::languageAdverb(head)) {
      realise::LaAdverb a;
      a.lemma = latin(adv, Adv);
      if (a.lemma != kNone) {
        c.cover(n.tokens);
        cl.adverbs.push_back(a);
        return true;
      }
    }
  }
  // place adverbs inside a PP: "in here"
  if (n.isPronoun && n.pronLemma.empty()) return false;
  const bool person = animate(n);
  const std::string verb = c.frame ? c.frame->pred.lemma : std::string();
  if (!c.prepOverride.empty() && prep == c.prepOverride && !c.prepOverrideLatin.empty())
    return withPrep(c.prepOverrideLatin.c_str(), c.prepOverrideCase ? c.prepOverrideCase : (uint8_t)Abl);
  // prepositional verbs (verbprep_en_la.tsv frame obj): the PP object is the verb's direct object ("wait for me",
  // "look at the sky")
  const curated::VerbPrepEntry* vpe = c.frame ? cd_.verbPrep(verb, prep) : nullptr;
  if (c.frame && !cl.hasObject && vpe && vpe->frame == "obj") {
    cl.hasObject = true;
    npInto(n, c, cl.object);
    return true;
  }
  if (c.frame && vpe && vpe->frame == "dat" && person) return bare(Dat);   // C17: "take X from me" -> mihi auferre
  if (prep == "to" || prep == "into") {   // C15: a country of names_la.tsv ("back to Kansas" -> in Kansiam)
    const curated::NameEntry* ne = n.isName ? cd_.nameByEnglish(n.head) : nullptr;
    if (!ne && !n.genitive.empty()) ne = cd_.nameByEnglish(n.head + " of " + n.genitive[0].head);
    if (ne && ne->note.find("country") != std::string::npos) return withPrep("in", Acc);
    if (ne && ne->note.find("place") != std::string::npos) return withPrep("ad", Acc);   // "to the Emerald City"
  }
  if (prep == "to") {
    // C22: a verb whose valency takes ad + accusative keeps it for persons too ("animum ad magistrum attende")
    if (person && cl.pred.lemma != kNone) {
      if (const curated::Valency* va = cd_.valency(la_.lemma(cl.pred.lemma).key))
        for (const curated::Frame& fr : va->frames)
          if (fr.kind == curated::FrameKind::Prep && fr.prep == "ad" && fr.prepCase == Acc) return withPrep("ad", Acc);
    }
    if (person && !c.motion) {
      if (!cl.hasIndirect) { cl.hasIndirect = true; npInto(n, c, cl.indirect); return true; }
      return bare(Dat);
    }
    return withPrep("ad", Acc);
  }
  if (prep == "for") {
    if (time) return bare(Acc);
    // C15: "It is better for people to ..." -> Melius est hominibus ... (dative after the verb, before the infinitive)
    if (person && c.frame && c.frame->copula && !c.frame->predAdj.empty() &&
        (!c.frame->hasSubject || (c.frame->subject.isPronoun && c.frame->subject.pronLemma == "it"))) {
      LaOblique o;
      o.case_ = Dat;
      npInto(n, c, o.np);
      o.np.case_ = Dat;
      o.after = true;
      cl.obliques.push_back(o);
      return true;
    }
    if (person && !cl.hasIndirect) { cl.hasIndirect = true; npInto(n, c, cl.indirect); return true; }
    return withPrep("prō", Abl);
  }
  // C15: comparison: "more powerful than all of us" -> potentior ... quam nōs omnēs (same case as the subject),
  // after the verb; "as tall as" -> tam ... quam
  if (prep == "than" || (prep == "as" && c.frame && !c.frame->predAdj.empty())) {
    LaOblique o;
    o.prep = latin("quam", Conj);
    o.case_ = Nom;
    // C20: "than yours" stands for the compared noun: the subject of a predicate adjective, else the object ("I like
    // your hat more than mine" -> quam meum, in the object's case)
    const std::string pw = text::lower(n.surface);
    const bool possObj = c.st.lang == frame::SrcLang::En && cl.hasObject && c.frame && c.frame->predAdj.empty() &&
                         (pw == "mine" || pw == "yours" || pw == "ours" || pw == "his" || pw == "hers" || pw == "theirs");
    if (possObj || cl.hasSubject) {
      const LaNP& sj = possObj ? cl.object : cl.subject;
      if (possObj) o.case_ = cl.object.case_ ? cl.object.case_ : (uint8_t)Acc;
      c.possRefNumber = sj.isPronoun && sj.pron.number ? sj.pron.number : sj.number ? sj.number : (uint8_t)Sg;
      if (sj.gender) c.possRefGender = sj.gender;
      else if (sj.isPronoun) c.possRefGender = sj.pron.gender ? sj.pron.gender : (uint8_t)M;
      else if (sj.head != kNone) c.possRefGender = simpleGender(la_.lemma(sj.head).gender);
    }
    npInto(n, c, o.np);
    c.possRefGender = c.possRefNumber = 0;
    o.np.case_ = o.case_;
    // C19: "smaller than the first": an adjective standing for the subject's noun takes its gender and number (quam
    // prīmus), not the neuter of an abstract substantive
    if (c.st.lang == frame::SrcLang::En && n.token >= 0 && (size_t)n.token < c.s.tokens.size() &&
        c.s.tokens[(size_t)n.token].upos == "ADJ" && cl.hasSubject && o.np.head != kNone &&
        (la_.lemma(o.np.head).pos == Adj || la_.lemma(o.np.head).pos == Num)) {
      if (cl.subject.isPronoun) o.np.gender = cl.subject.pron.gender ? cl.subject.pron.gender : (uint8_t)M;
      else if (cl.subject.gender) o.np.gender = cl.subject.gender;
      else if (cl.subject.head != kNone) {
        const lex::Lemma sl = la_.lemma(cl.subject.head);
        o.np.gender = (uint8_t)(sl.gender == F || sl.gender == FN ? F : sl.gender == N ? N : M);
      }
      o.np.number = cl.subject.number;
    }
    o.after = true;
    cl.obliques.push_back(o);
    return true;
  }
  if (prep == "with") return person ? withPrep("cum", Abl) : bare(Abl);
  if (prep == "at") {
    if (time) return bare(Abl);
    if (person) return withPrep("apud", Acc);
    return withPrep("in", Abl);
  }
  // Spanish "en" with a verb of motion is "into" ("Cayó en un hoyo" -> in foveam cecidit) (C13)
  if (prep == "in" && c.motion && !time && c.st.lang == frame::SrcLang::Es) return withPrep("in", Acc);
  if (prep == "in" || prep == "on" || prep == "upon") return time ? bare(Abl) : withPrep("in", Abl);
  if (prep == "into" || prep == "onto") return withPrep("in", Acc);
  // C19: "get out of the bed", "came out of the house": ex + ablative, not dē
  if (prep == "out of" || (prep == "of" && c.frame && c.frame->pred.particle == "out")) return withPrep("ex", Abl);
  if (prep == "of" || prep == "about") return withPrep("dē", Abl);
  if (prep == "by") {
    if (c.frame && c.frame->pred.voice == frame::Voice::Passive) return person ? withPrep("ab", Abl) : bare(Abl);
    if (!person) return bare(Abl);
    return withPrep("apud", Acc);
  }
  // C19: "from curiosity", "from fear", "from hunger": the cause, a bare ablative (cūriōsitāte, timōre, famē)
  if ((prep == "from" || prep == "out of") && c.st.lang == frame::SrcLang::En &&
      [&] { for (const char* w : {"curiosity", "fear", "joy", "hunger", "anger", "shame", "pity", "envy", "pride", "sorrow",
                                  "grief", "thirst", "love", "hate", "fright", "terror", "kindness", "weariness"})
              if (head == w) return true;
            return false; }())
    return bare(Abl);
  if (prep == "from") return withPrep("ab", Abl);
  if (prep == "out of") return withPrep("ex", Abl);
  if (prep == "in front of") return withPrep("ante", Acc);
  // the rest from preps_en_la.tsv (first row of the preposition)
  for (const curated::PrepEntry& pe : cd_.preps()) {
    if (pe.english != prep) continue;
    if (pe.infinitive) break;
    if (pe.latin == "-" || pe.latinKey.empty()) return bare(pe.case_ ? pe.case_ : (uint8_t)Abl);
    std::vector<std::string> lw = words(pe.latin);
    const std::string& last = lw.empty() ? pe.latin : lw.back();
    std::string lp = last == "ā" ? "ab" : last == "ē" ? "ex" : last;
    LaOblique o;
    o.prep = latin(lp.c_str(), Prep);
    if (o.prep == kNone) return bare(pe.case_ ? pe.case_ : (uint8_t)Abl);
    o.case_ = pe.case_;
    npInto(n, c, o.np);
    o.np.case_ = pe.case_;
    o.front = ob.front;
    cl.obliques.push_back(o);
    return true;
  }
  // unknown preposition: keep the NP in the ablative and record it
  c.out.unknownWords.push_back(prep);
  return bare(Abl);
}

// ---- predicate ------------------------------------------------------------------------------------------------------
void Transfer::predicateInto(const SemFrame& f, Ctx& c, LaClause& cl) const {
  realise::LaPredicate& p = cl.pred;
  const frame::SemPredicate& sp = f.pred;
  c.cover(sp.token);
  c.cover(sp.auxTokens);
  auto choose = [&](const std::string& lemma, int token, bool hasObj, bool personObj) {
    Choice ch;
    ch.token = token;
    uint32_t id = kNone;
    if (const char* la = tables::verb(lemma)) {   // closed table: be, can, have ...
      id = latin(la, Verb);
      ch.source = lemma;
      ch.lemma = id;
      ch.kind = "table";
    } else {
      id = select(lemma, Verb, c.context, hasObj, personObj, c.st, ch);
      // C19: a personal passive needs a transitive Latin verb ("The boy was hurt": laedere, not dolēre; "The ship was
      // damaged": not nocēre + dative): the best candidate that takes an accusative object, when one scores close
      bool forced = false;
      for (const auto& ov : c.st.overrides) forced = forced || ov.first == token;
      if (id != kNone && !forced && sp.voice == frame::Voice::Passive && ch.kind == "sense" && ch.candidates.size() > 1) {
        auto transitive = [&](const Candidate& k) {
          const lex::Lemma l = la_.lemma(k.lemma);
          if (l.id == kNone || (l.flags & lex::Deponent)) return false;
          if (const curated::Valency* v = cd_.valency(l.key))
            if (!v->frames.empty() && v->frames[0].kind != curated::FrameKind::Acc &&
                v->frames[0].kind != curated::FrameKind::DatAcc && v->frames[0].kind != curated::FrameKind::AccInf &&
                v->frames[0].kind != curated::FrameKind::AccAbl && v->frames[0].kind != curated::FrameKind::AccAcc)
              return false;
          std::vector<lex::Sense> se;
          la_.senses(k.lemma, se);
          if (k.sense < se.size() && (se[k.sense].tags & 2u) && !(se[k.sense].tags & 1u)) return false;
          std::string probe;
          return morph::generate(la_, k.lemma, morph::verbForm(P3, Pl, Present, Indicative, Passive), probe, false);
        };
        if (!transitive(ch.candidates[0]))
          for (size_t i = 1; i < ch.candidates.size(); ++i)
            if (ch.candidates[i].score >= ch.candidates[0].score - 0.6 && transitive(ch.candidates[i])) {
              id = ch.candidates[i].lemma;
              ch.lemma = id;
              ch.note = "a passive needs a transitive verb";
              ch.margin = std::min(ch.margin, 0.14);
              break;
            }
      }
    }
    c.out.choices.push_back(ch);
    if (id == kNone) c.out.unknownWords.push_back(lemma);
    return id;
  };
  const bool hasObj = (f.hasObject && !c.routeObject) || cl.hasObject;
  const bool personObj = f.hasObject && animate(f.object);
  uint32_t verb = kNone;
  // verb + preposition (data/curated/verbprep_en_la.tsv): the Latin verb, and the Latin preposition it fixes
  const char* withPrep = nullptr;
  for (const frame::SemOblique& o : f.obliques)
    if (const curated::VerbPrepEntry* e = cd_.verbPrep(sp.lemma, o.prep)) {
      if (e->frame == "obj" && f.hasObject && e->latin != "-") continue;   // C24: "put the book on the table" is no "put on"
      if (e->latin != "-") withPrep = e->latin.c_str();
      if (!e->latinPrep.empty()) { c.prepOverride = o.prep; c.prepOverrideLatin = e->latinPrep; c.prepOverrideCase = e->prepCase; }
      if (withPrep || !e->latinPrep.empty()) break;
    }
  // verb + state noun ("tener miedo" -> timeō; data/curated/states_en_la.tsv)
  const curated::StateEntry* sne = f.hasObject ? cd_.state(sp.lemma + " " + text::lower(f.object.head)) : nullptr;
  const char* sn = sne && sne->kind == "verb" && sne->latin.find(' ') == std::string::npos ? sne->latin.c_str() : nullptr;
  // C17: light verbs ("make a visit" -> aliquem vīsere): words before the Latin verb are its fixed object
  std::string snFixed, snVerb;
  if (sne && sne->kind == "verb" && !sn) {
    const std::vector<std::string> lw = words(sne->latin);
    if (lw.size() >= 2 && latin(lw.back().c_str(), Verb) != kNone) {
      snVerb = lw.back();
      for (size_t i = 0; i + 1 < lw.size(); ++i) snFixed += (snFixed.empty() ? "" : " ") + lw[i];
      sn = snVerb.c_str();
    }
  }
  // phrasal verbs (data/curated/phrasal_en_la.tsv); particle "-" = a bare verb with a fixed Latin frame ("bow")
  const curated::PhrasalEntry* ph = cd_.phrasal(sp.lemma, sp.particle.empty() ? std::string("-") : sp.particle);
  // phrasebook "vp" row ("play cards" -> chartīs lūdere): the last Latin word is the infinitive (the verb), the
  // words before it are fixed complements placed with the obliques
  std::vector<std::string> fixedWords = words(sp.fixedLatin);
  uint32_t fixedVerb = kNone;
  if (fixedWords.size() >= 1) {
    morph::Token mt;
    morph::analyseLatin(la_, fixedWords.back(), mt);
    for (const lex::Analysis& a : mt.analyses)
      if (la_.lemma(a.lemma).pos == Verb) { fixedVerb = a.lemma; break; }
  }
  if (fixedVerb != kNone) {
    verb = fixedVerb;
    Choice ch;
    ch.token = sp.token;
    ch.source = sp.lemma;
    ch.lemma = verb;
    ch.kind = "phrasebook";
    ch.note = "phrasebook: " + sp.fixedLatin;
    c.out.choices.push_back(ch);
    fixedWords.pop_back();
    if (!fixedWords.empty()) {
      LaOblique o;
      std::string joined;
      for (const std::string& w : fixedWords) joined += (joined.empty() ? "" : " ") + w;
      o.np.fixed = joined;
      cl.obliques.push_back(o);
    }
  } else if (c.forcedVerb != kNone) {
    verb = c.forcedVerb;
  } else if (sn) {
    verb = latin(sn, Verb);
    c.objectInVerb = true;
    c.cover(f.object.tokens);
    if (!snFixed.empty()) {   // C17: the light verb's fixed object ("aliquem")
      cl.hasObject = true;
      cl.object = LaNP{};
      cl.object.fixed = snFixed;
    }
    Choice ch;
    ch.token = sp.token;
    ch.source = sp.lemma + " " + f.object.head;
    ch.lemma = verb;
    ch.kind = "table";
    c.out.choices.push_back(ch);
  } else if (withPrep && sp.particle.empty()) {
    verb = latin(withPrep, Verb);
    Choice ch;
    ch.token = sp.token;
    ch.source = sp.lemma;
    ch.lemma = verb;
    ch.kind = "table";
    ch.note = "verb with its preposition";
    c.out.choices.push_back(ch);
  } else if (sp.lemma == "have" && f.type == Kind::Imp && sp.complementVerb.empty()) {
    verb = latin("sūmō", Verb);   // "Have some tea." = take
    Choice ch;
    ch.token = sp.token;
    ch.source = sp.lemma;
    ch.lemma = verb;
    ch.kind = "table";
    c.out.choices.push_back(ch);
  } else if (sp.ellipsis) {
    verb = c.mem.lastVerb != kNone ? c.mem.lastVerb : latin("faciō", Verb);
    Choice ch;
    ch.token = sp.token;
    ch.source = sp.lemma;
    ch.lemma = verb;
    ch.kind = "table";
    ch.note = "verb of the previous clause";
    c.out.choices.push_back(ch);
  } else if (ph && sp.complementVerb.empty() &&
             (verb = [&] {   // C15: "arceō|absum": with an object the first verb, without one the second
                const size_t bar = ph->latin.find('|');
                const std::string w = bar == std::string::npos ? ph->latin
                                      : hasObj ? ph->latin.substr(0, bar) : ph->latin.substr(bar + 1);
                return w == "-" || w.empty() ? kNone : latin(w.c_str(), Verb);   // C24: "-" = the verb's own word
              }()) != kNone) {
    Choice ch;
    ch.token = sp.token;
    ch.source = sp.particle.empty() ? sp.lemma : sp.lemma + " " + sp.particle;
    ch.lemma = verb;
    ch.kind = "table";
    ch.note = "phrasal_en_la.tsv";
    c.out.choices.push_back(ch);
    if (ph->frame == "refl" && !f.hasObject) c.reflObject = true;
    else if (ph->frame == "acc") c.phrasalCase = Acc;
    else if (ph->frame == "dat") c.phrasalCase = Dat;
    else if (ph->frame == "abl") c.phrasalCase = Abl;
  } else if (c.st.lang == frame::SrcLang::En && sp.lemma == "live" && !hasObj && latin("habitō", Verb) != kNone &&
             (f.wh.word == "where" || [&] { for (const frame::SemAdverb& a : f.adverbs) if (a.lemma == "where" || a.lemma == "here" || a.lemma == "there") return true; return false; }())) {
    verb = latin("habitō", Verb);   // C19: "where the dragon lives", "we live here": dwell (habitō), not be alive
    Choice ch;
    ch.token = sp.token;
    ch.source = sp.lemma;
    ch.lemma = verb;
    ch.kind = "table";
    ch.note = "live = dwell";
    c.out.choices.push_back(ch);
  } else if (c.st.lang == frame::SrcLang::En && sp.lemma == "bear" && sp.voice == frame::Voice::Passive &&
             latin("nāscor", Verb) != kNone) {
    verb = latin("nāscor", Verb);   // C19: "was born" -> nātus est (nāscor), not a passive of ferō / pariō
    Choice ch;
    ch.token = sp.token;
    ch.source = "born";
    ch.lemma = verb;
    ch.kind = "table";
    ch.note = "be born";
    c.out.choices.push_back(ch);
  } else if (c.st.lang == frame::SrcLang::En && (sp.lemma == "rise" || sp.lemma == "set") && f.hasSubject &&
             !f.subject.isPronoun && tables::celestialNoun(text::lower(f.subject.head)) && !f.hasObject &&
             latin(sp.lemma == "rise" ? "orior" : "occidō", Verb) != kNone) {
    // C19: the sun / moon / stars rise (orior: ortus est) and set (occidō), they do not stand up (surgō)
    verb = latin(sp.lemma == "rise" ? "orior" : "occidō", Verb);
    Choice ch;
    ch.token = sp.token;
    ch.source = sp.lemma;
    ch.lemma = verb;
    ch.kind = "table";
    ch.note = "rise / set of the sun, moon, stars";
    c.out.choices.push_back(ch);
  } else if (c.st.lang == frame::SrcLang::En && sp.lemma == "get" && sp.particle.empty() && hasObj && !personObj &&
             f.hasObject && !f.object.isPronoun && sp.tense != frame::Tense::Past && sp.aspect == frame::Aspect::Simple &&
             [&] { for (const frame::SemOblique& o : f.obliques) if (o.prep == "from" && animate(o.np)) return false; return true; }() &&
             latin("capiō", Verb) != kNone) {
    // C19: "get" + a thing to do or to be done (imperative, future, a modal) is fetching / taking: capiō ("we can get the
    // clothes"); a past "got" with a thing is receiving (accipiō, the reverse index) and "get ... from" keeps accipiō
    verb = latin("capiō", Verb);
    Choice ch;
    ch.token = sp.token;
    ch.source = sp.lemma;
    ch.lemma = verb;
    ch.kind = "table";
    ch.note = "get = fetch, take";
    c.out.choices.push_back(ch);
  } else if (sp.lemma == "tell" && f.hasObject && !f.object.isPronoun &&
             tables::narrativeNoun(text::lower(f.object.head)) && latin("nārrō", Verb) != kNone) {
    verb = latin("nārrō", Verb);   // C15: "tell a story" -> nārrō; otherwise "tell" is dīcō (tiers_la.tsv)
    Choice ch;
    ch.token = sp.token;
    ch.source = sp.lemma;
    ch.lemma = verb;
    ch.kind = "table";
    ch.note = "tell + story";
    c.out.choices.push_back(ch);
  } else {
    verb = choose(sp.lemma, sp.token, hasObj, personObj);
    if (!sp.particle.empty() && sp.particle != "se") {   // Spanish "se" without a row: the verb as it is (C13)
      // C17: a particle without a phrasal row is a direction adverb from the table (away -> procul, back -> retrō);
      // aspect particles ("walk on", "eat up" -> on, up as completion) and words the table lacks are not words of
      // their own (never the reverse index: "on" is not an adverb of place)
      const bool aspect = c.st.lang == frame::SrcLang::En &&
                          (sp.particle == "on" || sp.particle == "off" || sp.particle == "over" || sp.particle == "out" ||
                           sp.particle == "about" || sp.particle == "around" || sp.particle == "along" ||
                           sp.particle == "through");
      if (c.st.lang == frame::SrcLang::En) {
        const char* la = aspect ? nullptr : tables::adverb(sp.particle, c.motion);
        uint32_t id = la ? latin(la, Adv) : kNone;
        if (la && id == kNone) id = latin(la);
        if (id != kNone) {
          realise::LaAdverb a;
          a.lemma = id;
          cl.adverbs.push_back(a);
        }
      } else {
        realise::LaAdverb a;
        a.lemma = adverb(sp.particle, -1, c, c.motion);
        if (a.lemma != kNone) cl.adverbs.push_back(a);
      }
    }
  }
  p.lemma = verb;
  // catenative complement ("know how to play" -> sciō + lūdere)
  if (!sp.complementVerb.empty()) {
    // C15: a phrasal complement ("help to keep away the beasts" -> arcēre adiuvābis)
    const curated::PhrasalEntry* cph = sp.particle.empty() ? nullptr : cd_.phrasal(sp.complementVerb, sp.particle);
    uint32_t comp = kNone;
    if (cph) {
      const size_t bar = cph->latin.find('|');
      const std::string lw = bar == std::string::npos ? cph->latin : hasObj ? cph->latin.substr(0, bar) : cph->latin.substr(bar + 1);
      comp = latin(lw.c_str(), Verb);
      if (comp != kNone) {
        Choice ch;
        ch.token = sp.complementToken;
        ch.source = sp.complementVerb + " " + sp.particle;
        ch.lemma = comp;
        ch.kind = "table";
        ch.note = "phrasal_en_la.tsv";
        c.out.choices.push_back(ch);
        // the particle adverb added by choose() for the main verb is not wanted
        cl.adverbs.erase(std::remove_if(cl.adverbs.begin(), cl.adverbs.end(), [&](const realise::LaAdverb& a) {
                           return a.lemma == latin("procul", Adv) || a.lemma == latin("retrō", Adv); }), cl.adverbs.end());
      }
    }
    if (comp == kNone) comp = choose(sp.complementVerb, sp.complementToken, hasObj, personObj);
    c.cover(sp.complementToken);
    p.modal = verb;
    p.lemma = comp;
  }
  // modality (order_la.txt RULE modal.*)
  switch (sp.modality) {
    case Modality::Can: p.modal = latin("possum", Verb); break;
    case Modality::Must: case Modality::Should: p.modal = latin("dēbeō", Verb); break;
    case Modality::Want:
      if (f.negative) { p.modal = latin("nōlō", Verb); cl.polarity = realise::Polarity::Pos; }
      else p.modal = latin("volō", Verb);
      break;
    case Modality::May: {
      // C19: "May I go out?" / "Mother, may we bake a cake?" asks permission: licetne mihi / nōbīs + infinitive (the
      // subject moves to the dative in Transfer::clause); elsewhere "may" is possibility: fortasse
      if (c.st.lang == frame::SrcLang::En && f.type == Kind::Yn && f.hasSubject && f.subject.isPronoun &&
          f.subject.pron.person == 1 && latin("licet", Verb) != kNone && p.lemma != kNone) {
        p.modal = latin("licet", Verb);
        p.person = 3;
        p.number = Sg;
        break;
      }
      realise::LaAdverb a;
      a.lemma = latin("fortasse", Adv);
      if (a.lemma != kNone) cl.adverbs.push_back(a);
      break;
    }
    case Modality::Let:
      p.mood = Subjunctive;
      // C19: "let us go" -> eāmus; "let him go" / "let the children play" -> jussive of their own person (eat, lūdant)
      if (!f.hasSubject || f.subject.pronLemma == "we" || (f.subject.isPronoun && f.subject.pron.person == 1)) {
        p.person = 1;
        p.number = f.hasSubject && f.subject.isPronoun && f.subject.pron.number == 1 && f.subject.pronLemma != "we" ? Sg : Pl;
      }
      break;
    default: break;
  }
  if (sp.lemma == "can" && sp.complementVerb.empty() && sp.modality == Modality::None) p.lemma = latin("possum", Verb);
  // "You must be." (epistemic must, nothing after be): certē + sum ("Certē es")
  if (sp.lemma == "be" && sp.modality == Modality::Must && !f.copula && f.predicative.empty() && f.predAdj.empty() &&
      f.obliques.empty() && f.adverbs.empty() && !f.hasObject && sp.complementVerb.empty()) {
    const uint32_t certe = latin("certē", Adv);
    if (certe != kNone) {
      p.modal = kNone;
      realise::LaAdverb a;
      a.lemma = certe;
      cl.adverbs.push_back(a);
      Choice ch;
      ch.token = sp.token;
      ch.source = "must";
      ch.lemma = certe;
      ch.kind = "table";
      ch.note = "\"must be\" without a predicate: certē + sum";
      c.out.choices.push_back(ch);
    }
  }
  // "want" without a complement but negated: nōlō + object ("I don't want cake" -> Placentam nōlō)
  if (verb != kNone && verb == latin("volō", Verb) && f.negative && p.modal == kNone) {
    p.lemma = latin("nōlō", Verb);
    cl.polarity = realise::Polarity::Pos;
  }
  // tense / aspect / voice (RULE tense.*)
  // C17: a light verb ("made a mistake" -> errāvistī) is an event, whatever the Latin verb's state reading
  bool lightVerb = c.st.lang == frame::SrcLang::En && sne && sne->kind == "verb" && sp.lemma != "be" && sp.lemma != "feel";
  // C22: an English event verb whose verb + preposition row gives a Latin state verb ("declared for him" -> prō eō
  // stetērunt) stays an event
  if (c.st.lang == frame::SrcLang::En && sp.lemma == "declare")
    for (const frame::SemOblique& o : f.obliques) lightVerb = lightVerb || o.prep == "for";
  const bool state = (!lightVerb && tables::stateVerb(la_.lemma(p.modal != kNone ? p.modal : p.lemma).key)) || sp.habitual ||
                     cl.pred.lemma == latin("sum", Verb);
  // C22: "Every morning he walked to the village": a habit in the past (imperfect)
  bool everyTime = false;
  if (c.st.lang == frame::SrcLang::En)
    for (const frame::SemOblique& o : f.obliques)
      everyTime = everyTime || ((o.np.determiner == "every" || o.np.determiner == "each") && (o.prep.empty() || o.prep == "-"));
  uint8_t tense = Present;
  if (sp.tense == frame::Tense::Future) tense = Future;
  else if (sp.tense == frame::Tense::Past) {
    if (sp.aspect == frame::Aspect::Perfect) tense = Pluperfect;
    else if (sp.aspect == frame::Aspect::Progressive || state || sp.pastModal || everyTime) tense = Imperfect;
    else tense = Perfect;
  } else {
    // C24: "What could it possibly be?": "could" with "possibly" in a question asks about the present (potest)
    bool possibly = false;
    for (const frame::SemAdverb& a : f.adverbs) possibly = possibly || a.lemma == "possibly";
    const bool presentQ = c.st.lang == frame::SrcLang::En && possibly && sp.modality == Modality::Can &&
                          (f.type == Kind::Wh || f.type == Kind::Yn);
    if (sp.aspect == frame::Aspect::Perfect) tense = Perfect;
    else if (sp.pastModal && !presentQ) tense = Imperfect;
  }
  if (sp.voice == frame::Voice::Passive) {
    p.voice = Passive;
    bool agent = false;
    for (const frame::SemOblique& o : f.obliques)
      if (o.prep == "by") agent = true;
    // "the clock is broken": resultant state; Spanish passive "se" is a process ("Se venden casas") (C13)
    // ("it is said that ..." stays present: dīcitur)
    if (tense == Present && !agent && sp.particle != "se" && !(sp.lemma == "say" && !f.subordinate.empty()))
      tense = Perfect;
    // C19: a Latin verb without passive forms (intransitive: liquēscō "melt", nāscor is deponent anyway) says the
    // English passive actively: "I shall be all melted" -> tōta liquēscam; never a missing form
    if (p.lemma != kNone && !agent) {
      const lex::Lemma vl = la_.lemma(p.lemma);
      std::string probe;
      if (vl.id != kNone && !(vl.flags & lex::Deponent) &&
          !morph::generate(la_, p.lemma, morph::verbForm(P3, Pl, Present, Indicative, Passive), probe, false) &&
          morph::generate(la_, p.lemma, morph::verbForm(P3, Sg, Present, Indicative, Active), probe, false)) {
        p.voice = Active;
        if (sp.tense == frame::Tense::Present && sp.aspect == frame::Aspect::Simple) tense = Perfect;
      }
    }
  }
  // C24: "seem" is the passive of videō ("Domus magna vidētur", "quod vidētur"), never the active "see"
  if (c.st.lang == frame::SrcLang::En && sp.lemma == "seem" && p.lemma != kNone && p.lemma == latin("videō", Verb))
    p.voice = Passive;
  if (sp.deliberative) {
    tense = Present;
    p.mood = Subjunctive;
    if (sp.modality == Modality::Should && sp.complementVerb.empty() && p.lemma != kNone) p.modal = kNone;   // "Why should I?" -> cūr faciam
  }
  if (sp.mood == frame::SrcMood::Conditional) {
    if (c.st.fidelity >= 3) tense = Present;
    else { tense = tense == Perfect || tense == Pluperfect ? Pluperfect : Imperfect; p.mood = Subjunctive; }
  } else if (sp.mood == frame::SrcMood::Subjunctive) {
    p.mood = Subjunctive;
  }
  if (sp.tense == frame::Tense::Future && sp.modality == Modality::Can) tense = Future;
  // C15: "how could I kill ...?" (a question about the possible) -> possem; "even if I wanted (to)" -> etiam sī vellem
  bool evenIf = false;
  for (const std::string& k : f.connectors) evenIf = evenIf || k == "if";
  bool even = false;
  for (const frame::SemAdverb& a : f.adverbs) even = even || a.lemma == "even";
  evenIf = evenIf && even;
  if (c.st.lang == frame::SrcLang::En && sp.mood == frame::SrcMood::Indicative && sp.aspect == frame::Aspect::Simple &&
      ((f.type == Kind::Wh && sp.pastModal && sp.modality == Modality::Can && f.hasSubject && f.subject.isPronoun &&
        f.subject.pron.person == 1) || (evenIf && sp.tense == frame::Tense::Past))) {
    tense = Imperfect;
    p.mood = Subjunctive;
  }
  // C15: "Oz was always our friend" -> semper ... fuit (a whole past span, not a background state)
  if (tense == Imperfect && f.copula && sp.aspect == frame::Aspect::Simple && !sp.pastModal)
    for (const frame::SemAdverb& a : f.adverbs)
      if (a.lemma == "always") tense = Perfect;
  p.tense = tense;
  if (p.modal != kNone) { p.infTense = Present; p.infVoice = p.voice; p.voice = Active; }
  if (p.lemma != kNone) {
    c.mem.lastVerb = p.lemma;
    c.mem.lastMotion = c.motion;
  }
}

// C15: the passive of a verb whose Latin is deponent ("a person who has been kissed by the Witch") is said
// actively: the agent is the subject, the patient the object ("quem Saga ōsculāta est"). False: unchanged.
bool Transfer::deponentActive(const SemFrame& in, Ctx& c, SemFrame& out) const {
  if (!in.hasPred || in.pred.voice != frame::Voice::Passive || in.pred.modality != Modality::None ||
      !in.pred.complementVerb.empty() || !in.hasSubject || in.hasObject)
    return false;
  int agent = -1;
  for (size_t i = 0; i < in.obliques.size(); ++i)
    if (in.obliques[i].prep == "by") agent = (int)i;
  if (agent < 0) return false;
  Choice probe;
  const uint32_t v = select(in.pred.lemma, Verb, {}, true, false, c.st, probe);
  if (v == kNone || !(la_.lemma(v).flags & lex::Deponent)) return false;
  out = in;
  out.pred.voice = frame::Voice::Active;
  out.object = in.subject;
  out.hasObject = true;
  out.subject = in.obliques[(size_t)agent].np;
  out.obliques.erase(out.obliques.begin() + agent);
  return true;
}

// ---- clauses --------------------------------------------------------------------------------------------------------
void Transfer::clauseInto(const SemFrame& f0, Ctx& c, LaClause& cl) const {
  // C24: "she likes him" -> is eī placet: the thing or person liked is the Latin subject, the one who likes it the
  // dative (never "eī placet" with the roles turned round); "like to + verb" and questions about the object stay
  if (c.st.lang == frame::SrcLang::En && f0.hasPred && f0.pred.lemma == "like" && f0.hasSubject && f0.hasObject &&
      !f0.hasIndirect && f0.pred.complementVerb.empty() && f0.pred.prepVerb.empty() && !f0.object.interrogative &&
      f0.object.relative.empty() && latin("placeō", Verb) != kNone &&
      [&] {   // only when the dictionary choice for "like" here is placeō (amō keeps the English roles)
        Choice probe;
        probe.token = f0.pred.token;
        return select("like", Verb, c.context, true, animate(f0.object), c.st, probe) == latin("placeō", Verb);
      }()) {
    SemFrame g = f0;
    g.subject = f0.object;
    g.indirectObject = f0.subject;
    g.hasIndirect = true;
    g.hasObject = false;
    g.object = SemNP{};
    g.pred.lemma = "please";
    const uint32_t keep = c.forcedVerb;
    c.forcedVerb = latin("placeō", Verb);
    Choice ch;
    ch.token = f0.pred.token;
    ch.source = "like";
    ch.lemma = c.forcedVerb;
    ch.kind = "table";
    ch.note = "like: X mihi placet (valency_la.tsv)";
    c.out.choices.push_back(ch);
    c.cover(f0.pred.token);
    clauseInto(g, c, cl);
    c.forcedVerb = keep;
    return;
  }
  // C24: "be late for X" with a person or animal as the subject: sērō venīre ad X ("What is the dog late for?" ->
  // Ad quid canis sērō venit?), as the phrasebook's "I am late for X"
  if (c.st.lang == frame::SrcLang::En && f0.copula && f0.predAdj.size() == 1 && f0.predAdj[0].lemma == "late" &&
      f0.predAdj[0].adverbs.empty() && f0.predicative.empty() && f0.hasSubject) {
    bool forPhrase = false;
    for (const frame::SemOblique& o : f0.obliques) forPhrase = forPhrase || o.prep == "for";
    if (forPhrase) {
      SemFrame g = f0;
      g.copula = false;
      g.predAdj.clear();
      g.pred.lemma = "come";
      frame::SemAdverb a;
      a.lemma = "late";
      a.token = f0.predAdj[0].token;
      g.adverbs.push_back(a);
      for (frame::SemOblique& o : g.obliques)
        if (o.prep == "for") o.prep = "to";
      clauseInto(g, c, cl);
      return;
    }
  }
  // C15 (neg.nemo): "I do not want to kill anybody" -> nēminem necāre volō: the negation moves into the pronoun
  SemFrame fneg;
  const SemFrame* fp = &f0;
  if (f0.negative && f0.hasObject && f0.object.isPronoun &&
      (f0.object.pronLemma == "anyone" || f0.object.pronLemma == "anything")) {
    bool never = false;
    for (const frame::SemAdverb& a : f0.adverbs) never = never || a.lemma == "never";
    if (!never) {
      fneg = f0;
      fneg.negative = false;
      fneg.object.pronLemma = f0.object.pronLemma == "anyone" ? "nobody" : "nothing";
      fneg.object.negative = true;
      fp = &fneg;
    }
  }
  SemFrame fdep;
  if (deponentActive(*fp, c, fdep)) fp = &fdep;
  // C15: a present passive without agent whose participle the teacher glosses as an adjective ("My head is stuffed
  // with straw" -> plēnum est, "I am all tired out" -> fessa sum): the adjective, not the verb's passive
  SemFrame fpart;
  if (fp->hasPred && fp->pred.voice == frame::Voice::Passive && fp->pred.modality == Modality::None &&
      fp->pred.aspect == frame::Aspect::Simple && fp->hasSubject && fp->pred.token >= 0 &&
      (size_t)fp->pred.token < c.s.tokens.size() && c.st.lang == frame::SrcLang::En) {
    bool agent = false;
    for (const frame::SemOblique& o : fp->obliques) agent = agent || o.prep == "by";
    const std::string surf = c.s.tokens[(size_t)fp->pred.token].lower;
    std::vector<const curated::TierEntry*> te;
    if (!agent) cd_.glossTiers(surf, te);
    for (const curated::TierEntry* e : te)
      if (e->pos == std::string("adj")) {
        fpart = *fp;
        fpart.pred.voice = frame::Voice::Active;
        fpart.pred.lemma = "be";
        fpart.pred.particle.clear();
        fpart.copula = true;
        frame::SemAdj a;
        a.lemma = surf;
        a.token = fp->pred.token;
        for (size_t q = 0; q < fpart.adverbs.size(); ++q)   // "all tired" -> omnīnō fessa
          if (fpart.adverbs[q].lemma == "all") { a.adverbs.push_back("all"); a.advTokens.push_back(fpart.adverbs[q].token);
                                                   fpart.adverbs.erase(fpart.adverbs.begin() + (long)q); break; }
        fpart.predAdj.push_back(a);
        fp = &fpart;
        break;
      }
  }
  // C15: "he looks like a cat" -> fēlī similis est: look / seem + like = similis + dative
  // C17: "what you looked like" -> quālis essēs, "What does it look like?" -> Quāle est?: quālis is the predicate
  SemFrame fwhat;
  if (c.st.lang == frame::SrcLang::En) {
    const int wl = whatLike(*fp);
    if (wl >= 0) {
      fwhat = *fp;
      c.cover(fwhat.obliques[(size_t)wl].np.tokens);
      c.cover(fwhat.obliques[(size_t)wl].token);
      fwhat.obliques.erase(fwhat.obliques.begin() + wl);
      fwhat.copula = true;
      fwhat.pred.lemma = "be";
      fwhat.pred.particle.clear();
      frame::SemAdj a;
      a.lemma = "what-like";
      a.token = fp->pred.token;
      fwhat.predAdj.push_back(a);
      fwhat.predicative.clear();
      if (fwhat.type == Kind::Wh) fwhat.wh = frame::SemWh{};
      fp = &fwhat;
    }
  }
  SemFrame flike;
  if (fp->hasPred && !fp->copula && (fp->pred.lemma == "look" || fp->pred.lemma == "seem" || fp->pred.lemma == "be") &&
      !fp->hasObject && fp->pred.particle.empty() && c.st.lang == frame::SrcLang::En) {
    for (size_t i = 0; i < fp->obliques.size(); ++i)
      if (fp->obliques[i].prep == "like") {
        flike = *fp;
        flike.copula = true;
        flike.pred.lemma = "be";
        frame::SemAdj a;
        a.lemma = "similar";
        a.token = fp->obliques[i].token;
        flike.predAdj.push_back(a);
        flike.hasIndirect = true;
        flike.indirectObject = fp->obliques[i].np;
        flike.obliques.erase(flike.obliques.begin() + (long)i);
        fp = &flike;
        break;
      }
  }
  // C15: "And we have been told that Oz is a good Wizard" -> nōbīs dictum est Oz magum bonum esse (the person in the
  // dative, impersonal passive); "it is said that he never ..." -> dīcitur nēminem umquam ... (personal: the subject of
  // the that-clause is the subject, the clause an infinitive)
  SemFrame fsaid;
  bool saidImpers = false, saidPersonal = false;
  if (fp->hasPred && fp->pred.voice == frame::Voice::Passive && (fp->pred.lemma == "tell" || fp->pred.lemma == "say") &&
      fp->subordinate.size() >= 1 && fp->subordinate[0].relation == Relation::Complement && !fp->subordinate[0].frame.empty() &&
      fp->subordinate[0].frame[0].type != Kind::Wh && c.st.lang == frame::SrcLang::En) {
    fsaid = *fp;
    fsaid.pred.lemma = "say";
    if (fp->hasSubject && fp->subject.isPronoun && fp->subject.pronLemma == "it" && fp->subordinate[0].frame[0].hasSubject) {
      c.cover(fp->subject.tokens);
      c.cover(fp->subject.token);
      frame::SemFrame& inner = fsaid.subordinate[0].frame[0];
      fsaid.subject = inner.subject;
      inner.hasSubject = false;
      inner.subject = SemNP{};
      fsaid.subordinate[0].marker = "inf";
      saidPersonal = true;
    } else if (fp->hasSubject) {
      fsaid.hasIndirect = true;
      fsaid.indirectObject = fp->subject;
      fsaid.hasSubject = false;
      fsaid.subject = SemNP{};
      saidImpers = true;
    }
    fp = &fsaid;
  }
  const SemFrame& f = *fp;
  const SemFrame* keepFrame = c.frame;
  const bool keepMotion = c.motion, keepNeg = c.negative;
  const uint8_t keepPerson = c.subjPerson, keepNumber = c.subjNumber, keepGender = c.subjGender;
  c.subjGender = 0;
  const bool keepObjVerb = c.objectInVerb;
  c.objectInVerb = false;
  c.frame = &f;
  c.negative = f.negative;
  c.question = f.type == Kind::Yn || f.type == Kind::Wh;
  c.motion = tables::motionVerb(f.pred.lemma) || tables::motionVerb(f.pred.complementVerb) ||
             (f.pred.particle == "back" || f.pred.particle == "away" || f.pred.particle == "out" ||
              f.pred.particle == "in" || f.pred.particle == "down" || f.pred.particle == "up" || f.pred.particle == "on");   // C15: get back to
  const bool prevMotion = c.mem.lastMotion;   // an elliptical "where" asks about the previous clause's motion
  const bool keepRoute = c.routeObject;
  c.routeObject = f.hasObject && !f.object.isPronoun && text::lower(f.object.head) == "way" && c.motion;
  c.subjPerson = f.hasSubject && f.subject.isPronoun ? (f.subject.pron.person ? f.subject.pron.person : 3) : 3;
  c.subjNumber = f.hasSubject ? f.subject.number : 1;
  cl = LaClause{};
  switch (f.type) {
    case Kind::Yn: cl.type = realise::ClauseType::Yn; break;
    case Kind::Wh: cl.type = realise::ClauseType::Wh; break;
    case Kind::Imp: cl.type = realise::ClauseType::Imp; break;
    case Kind::Excl: cl.type = realise::ClauseType::Excl; break;
    case Kind::Frag: cl.type = realise::ClauseType::Frag; break;
    default: cl.type = realise::ClauseType::Decl; break;
  }
  // C22: an exclamation that is only "my / our + noun(s)" ("My hat and gloves!", "My poor cat!") is the accusative of
  // exclamation: Ō pilleum et digitābula mea!
  if (c.st.lang == frame::SrcLang::En && f.type == Kind::Frag && f.punct == "!" && f.hasSubject && !f.hasPred &&
      f.obliques.empty() && f.predAdj.empty() && !f.subject.isPronoun && !f.subject.isName && f.subject.possessor.size() == 1 &&
      f.subject.possessor[0].isPronoun && f.subject.possessor[0].pron.person == 1) {
    cl.type = realise::ClauseType::Excl;
    cl.exclO = true;
  }
  if (f.negative) cl.polarity = realise::Polarity::Neg;
  if (f.expectYes) cl.bias = realise::YnBias::ExpectYes;
  // C15: a negated yes/no question ("Can't you get down?", "Don't you know?") is a statement with a question mark:
  // "Nōn potes dēscendere?", "Nescīs?" (the main agent's gold; nōnne is kept for tag questions)
  if (f.type == Kind::Yn && f.negative && c.st.lang == frame::SrcLang::En) {
    cl.type = realise::ClauseType::Decl;
    cl.bias = realise::YnBias::Neutral;
    cl.punct = "?";
    cl.plainQuestion = true;
  }
  cl.existential = f.existential;
  cl.exclQuam = f.exclQuam;
  // the clause's content lemmas (sense-keyword overlap)
  c.context.clear();
  if (f.hasPred) c.context.push_back(f.pred.lemma);
  if (f.hasSubject) c.context.push_back(f.subject.head);
  if (f.hasObject) c.context.push_back(f.object.head);
  for (const frame::SemOblique& o : f.obliques) c.context.push_back(o.np.head);
  for (const frame::SemAdj& a : f.predAdj) c.context.push_back(a.lemma);

  // subject ("And back to Kansas?": "back" with a goal is the motion, said by the goal alone: Et in Kansiam?)
  const bool backFrag = f.type == Kind::Frag && f.hasSubject && !f.subject.isPronoun && text::lower(f.subject.head) == "back" &&
                        !f.obliques.empty();
  if (backFrag) c.cover(f.subject.tokens);
  if (f.hasSubject && f.type != Kind::Imp && !backFrag) {
    cl.hasSubject = true;
    npInto(f.subject, c, cl.subject);
    if (f.subject.pron.emphatic) cl.subject.emphasis = true;
    // Spanish (C13): a 3rd-person plural question answered with "we" was put to ustedes: 2nd plural (a guess)
    if (c.st.lang == frame::SrcLang::Es && f.implicitSubject && cl.subject.isPronoun && cl.subject.pron.person == 3 &&
        cl.subject.pron.number == Pl && c.mem.answerWe && c.question) {
      cl.subject.pron.person = 2;
      c.subjPerson = 2;
      c.mem.addresseeGuess = true;
    }
    if (f.subject.determiner == "all" && f.subject.isPronoun) cl.pred.person = f.subject.pron.person;
    c.subjGender = cl.subject.isPronoun ? cl.subject.pron.gender
                 : cl.subject.head != kNone ? simpleGender(la_.lemma(cl.subject.head).gender) : 0;
    // "There isn't any tea": a negated existential subject (nūllus) keeps the normal order (Nūlla thēa est)
    if (f.existential && !cl.subject.adjectives.empty() && cl.subject.adjectives[0].lemma == latin("nūllus", Adj))
      cl.existential = false;
  }
  // state adjectives with a person subject become verbs ("I am afraid" -> timeō, "you were wrong" -> errābās)
  // (data/curated/states_en_la.tsv kind verb); kind adj fixes the Latin adjective ("tired" -> fessus)
  const char* stateVerb = nullptr;
  const curated::StateEntry* stateAdj = nullptr;
  if (f.copula && !f.predAdj.empty() && f.predicative.empty()) {
    const bool personSubj = (f.hasSubject && animate(f.subject)) || (!f.hasSubject && f.type == Kind::Imp) ||
                            (f.hasSubject && f.subject.isPronoun && f.subject.pron.person < 3 && f.subject.pron.person > 0);
    const curated::StateEntry* se = personSubj ? cd_.state(text::lower(f.predAdj[0].lemma)) : nullptr;
    if (se && se->kind == "verb" && f.predAdj.size() == 1 && se->latin.find(' ') == std::string::npos &&
        latin(se->latin.c_str(), Verb) != kNone)
      stateVerb = se->latin.c_str();
    else if (se && se->kind == "adj")
      stateAdj = se;
  }
  // predicate
  if (f.hasPred) {
    if (stateVerb) {
      cl.pred.lemma = latin(stateVerb, Verb);
      Choice ch;
      ch.token = f.predAdj[0].token;
      ch.source = f.predAdj[0].lemma;
      ch.lemma = cl.pred.lemma;
      ch.kind = "table";
      ch.note = "be + " + f.predAdj[0].lemma + " -> " + stateVerb;
      c.out.choices.push_back(ch);
      c.cover(f.predAdj[0].token);
      c.cover(f.pred.token);
      c.cover(f.pred.auxTokens);
      SemFrame g = f;
      g.copula = false;
      g.pred.lemma = stateVerb;
      // tense as for "be" (a state): imperfect in the past
      c.forcedVerb = cl.pred.lemma;
      predicateInto(g, c, cl);
      c.forcedVerb = kNone;
      cl.pred.lemma = latin(stateVerb, Verb);
      if (f.pred.tense == frame::Tense::Past && f.pred.aspect == frame::Aspect::Simple) cl.pred.tense = Imperfect;
      // C20: the adjective's degree words stay with the verb ("so hungry" -> tam ēsuriēbāmus, "very afraid" -> valdē
      // timeō); they were dropped (A7 Check)
      if (c.st.lang == frame::SrcLang::En) {
        const frame::SemAdj& sa = f.predAdj[0];
        for (size_t i = 0; i < sa.adverbs.size(); ++i) {
          const uint32_t av = adverb(sa.adverbs[i], i < sa.advTokens.size() ? sa.advTokens[i] : -1, c, false);
          if (av != kNone) cl.adverbs.push_back(realise::LaAdverb{av, realise::AdvPos::BeforeVerb});
        }
      }
      // the "of" PP is the object ("afraid of the dark" -> timēre tenebrās)
    } else {
      predicateInto(f, c, cl);
    }
  }
  if (!f.hasPred && f.type != Kind::Frag && f.type != Kind::Excl) cl.type = realise::ClauseType::Frag;
  // imperative number (RULE imp.number)
  if (f.type == Kind::Imp) {
    bool pl = f.imperativePlural;
    if (!pl && c.mem.addresseePlural) { pl = true; c.mem.addresseeGuess = true; }
    if (pl) cl.pred.number = Pl;
    if (pl) c.mem.sawPlural = true;
  }
  // phrasal frame refl ("Everyone bow!" -> Omnēs, inclīnāte vōs!): the reflexive object of the subject's person
  if (c.reflObject && !f.hasObject) {
    cl.hasObject = true;
    LaNP r;
    r.isPronoun = true;
    r.pron.person = f.type == Kind::Imp ? 2 : c.subjPerson;
    r.pron.number = f.type == Kind::Imp ? (cl.pred.number ? cl.pred.number : (uint8_t)Sg) : (uint8_t)(c.subjNumber == 2 ? Pl : Sg);
    r.pron.reflexive = true;
    r.number = r.pron.number;
    r.pron.gender = M;
    r.case_ = Acc;
    cl.object = r;
  }
  c.reflObject = false;
  if (saidImpers) { cl.predGender = N; cl.pred.person = 3; cl.pred.number = Sg; }
  (void)saidPersonal;
  // object
  if (c.routeObject) {   // "Which way should I go?" -> Quā viā īre dēbeō? (ablative of the route)
    LaOblique o;
    o.case_ = Abl;
    npInto(f.object, c, o.np);
    o.np.case_ = Abl;
    cl.obliques.push_back(o);
  } else if (f.hasObject && !c.objectInVerb &&
             !(f.type == Kind::Wh && f.wh.role == frame::Role::Object && f.object.isPronoun)) {
    cl.hasObject = true;
    npInto(f.object, c, cl.object);
    if (c.phrasalCase) cl.object.case_ = c.phrasalCase;
    // valency "dat;acc": the dative is for persons ("mihi crēde"), a thing is in the accusative ("rēs crēdō")
    if (!cl.object.case_ && cl.pred.lemma != kNone && !animate(f.object))
      if (const curated::Valency* v = cd_.valency(la_.lemma(cl.pred.lemma).key)) {
        bool dat = false, acc = false;
        for (const curated::Frame& fr : v->frames) {
          dat = dat || fr.kind == curated::FrameKind::Dat;
          acc = acc || fr.kind == curated::FrameKind::Acc;
        }
        if (dat && acc) cl.object.case_ = Acc;
      }
  }
  // C17: object complement of a factitive verb ("eum rēgem fēcērunt", "mē fortiōrem nōn facit"); a masculine noun of
  // a pair takes its feminine for a feminine object ("eam servam meam facere")
  if (cl.hasObject) {
    for (const SemNP& pn : f.objComplement) {
      LaNP x;
      npInto(pn, c, x);
      if (x.head != kNone && !x.isPronoun && !x.isName) {
        const uint8_t og = cl.object.gender ? cl.object.gender : cl.object.isPronoun ? cl.object.pron.gender
                         : cl.object.head != kNone ? la_.lemma(cl.object.head).gender : (uint8_t)0;
        if (og == F) x.head = feminineOf(x.head);
      }
      cl.objPredicative.push_back(x);
    }
    for (const frame::SemAdj& a : f.objComplementAdj) {
      Choice ch;
      LaAdj la;
      la.degree = a.degree;
      la.lemma = adjectiveInto(a, c, la, ch);
      c.out.choices.push_back(ch);
      c.cover(a.token);
      if (la.lemma == kNone) { c.out.unknownWords.push_back(a.lemma); continue; }
      for (size_t i = 0; i < a.adverbs.size(); ++i) {
        const uint32_t av = adverb(a.adverbs[i], i < a.advTokens.size() ? a.advTokens[i] : -1, c, false);
        if (av != kNone) la.adverbs.push_back(av);
      }
      cl.objPredAdj.push_back(la);
    }
  }
  // "tener miedo de la oscuridad" -> timēre tenebrās: the "de" attribute of the state noun is the object (C13)
  if (c.objectInVerb && !cl.hasObject && f.hasObject && !f.object.genitive.empty()) {
    cl.hasObject = true;
    npInto(f.object.genitive[0], c, cl.object);
  }
  c.phrasalCase = 0;
  if (f.hasIndirect) {
    cl.hasIndirect = true;
    npInto(f.indirectObject, c, cl.indirect);
    // C15: rogō / interrogō / doceō take the person in the accusative ("Tum mē rogāvit ...")
    if (!cl.hasObject && cl.pred.lemma != kNone &&
        (cl.pred.lemma == latin("rogō", Verb) || cl.pred.lemma == latin("interrogō", Verb) || cl.pred.lemma == latin("doceō", Verb))) {
      cl.hasObject = true;
      cl.object = cl.indirect;
      cl.hasIndirect = false;
      cl.indirect = LaNP{};
    }
  }
  std::vector<int> agentDone;   // C17: agent phrases already placed by the participle-phrase rule below
  // C17: a fragment "or the Tin Woodman badly dented on the rocks" is a noun with a participle phrase: a transitive
  // verb in the past without object, auxiliary or modal, in a clause cut from its sentence (lower-case start) ->
  // the perfect participle agreeing with the subject, no finite verb (... graviter contūsus)
  if (c.st.lang == frame::SrcLang::En && f.hasPred && !f.copula && f.hasSubject && !f.subject.isPronoun && !f.hasObject &&
      !cl.hasObject && f.pred.tense == frame::Tense::Past && f.pred.aspect == frame::Aspect::Simple &&
      f.pred.modality == Modality::None && f.pred.voice == frame::Voice::Active && f.pred.auxTokens.empty() &&
      f.subordinate.empty() && cl.pred.lemma != kNone && cl.pred.modal == kNone && f.type == Kind::Decl &&
      !c.s.text.empty() && c.s.text[0] >= 'a' && c.s.text[0] <= 'z' && f.pred.token >= 0 &&
      (size_t)f.pred.token < c.s.tokens.size()) {
    const std::string& vw = c.s.tokens[(size_t)f.pred.token].lower;
    bool transitive = false;
    if (const curated::Valency* v = cd_.valency(la_.lemma(cl.pred.lemma).key))
      for (const curated::Frame& fr : v->frames) transitive = transitive || fr.kind == curated::FrameKind::Acc;
    std::vector<lex::Sense> ss;
    la_.senses(cl.pred.lemma, ss);
    if (!ss.empty() && (ss[0].tags & 1u) && !(ss[0].tags & 2u)) transitive = true;
    bool agent = false;
    for (const frame::SemOblique& o : f.obliques) agent = agent || o.prep == "by";
    const bool deponent = (la_.lemma(cl.pred.lemma).flags & lex::Deponent) != 0;
    const bool edForm = vw.size() > 3 && (vw.compare(vw.size() - 2, 2, "ed") == 0 || vw.compare(vw.size() - 2, 2, "en") == 0);
    // an English form that is only a past participle ("broken", "written", not "broke") is one
    bool partOnly = false;
    if (c.st.srcLex) {
      std::vector<lex::Analysis> an;
      c.st.srcLex->lookup(text::en_key(vw), an);
      bool part = false, fin = false;
      for (const lex::Analysis& a : an) {
        if (c.st.srcLex->lemma(a.lemma).pos != Verb) continue;
        const Features ff = unpack(c.st.srcLex->feature(a.feat));
        if (ff.mood == ParticipleMood) part = true;
        else if (ff.tense == Perfect) fin = true;
      }
      partOnly = part && !fin;
    }
    if (!deponent && ((transitive && edForm) || agent || partOnly)) {
      LaAdj pa;
      pa.lemma = cl.pred.lemma;
      pa.participle = Perfect;
      cl.predAdj.push_back(pa);
      cl.pred = realise::LaPredicate{};
      cl.type = realise::ClauseType::Frag;
      if (std::find(c.out.flags.begin(), c.out.flags.end(), "participle-phrase") == c.out.flags.end())
        c.out.flags.push_back("participle-phrase");
      // the agent: ā/ab + ablative for persons, the bare ablative for things ("undīs frācta", "ab omnibus amāta")
      for (const frame::SemOblique& o : f.obliques) {
        if (o.prep != "by") continue;
        LaOblique lo;
        const bool person = animate(o.np) || o.np.pronLemma == "everyone" || o.np.pronLemma == "everybody";
        if (person) lo.prep = latin("ab", Prep);   // ā / ab by the next sound (order.prep)
        if (person && lo.prep == kNone) lo.prep = latin("ā", Prep);
        lo.case_ = Abl;
        npInto(o.np, c, lo.np);
        lo.np.case_ = Abl;
        c.cover(o.token);
        cl.obliques.push_back(lo);
        agentDone.push_back(o.token);
      }
    }
  }
  // C17: "tell my brother about the door" -> frātrī meō dē iānuā dīcere: the person told is a dative
  if (c.st.lang == frame::SrcLang::En && cl.hasObject && !cl.hasIndirect && f.hasObject && animate(f.object) &&
      f.pred.lemma == "tell" && (cl.pred.lemma == latin("dīcō", Verb) || cl.pred.lemma == latin("nārrō", Verb))) {
    cl.hasIndirect = true;
    cl.indirect = cl.object;
    cl.indirect.case_ = Dat;
    cl.hasObject = false;
    cl.object = LaNP{};
  }
  // C15: "I have no heart" -> cor nōn habeō (possession denied: the verb's negation, not nūllus)
  if (cl.hasObject && cl.pred.lemma == latin("habeō", Verb) && cl.pred.modal == kNone && f.hasObject &&
      f.object.determiner == "no" && !cl.object.adjectives.empty() && cl.object.adjectives[0].lemma == latin("nūllus", Adj) &&
      (cl.object.adjectives.size() == 1 || (cl.object.adjectives.size() == 2 && cl.object.adjectives[1].gerundive))) {   // C20
    cl.object.adjectives.erase(cl.object.adjectives.begin());
    cl.polarity = realise::Polarity::Neg;
  }
  // obliques
  for (const frame::SemOblique& o : f.obliques) {
    if (std::find(agentDone.begin(), agentDone.end(), o.token) != agentDone.end()) continue;
    if ((stateVerb || c.objectInVerb) && (o.prep == "of" || o.prep == "about") && !cl.hasObject) {
      cl.hasObject = true;
      npInto(o.np, c, cl.object);
      c.cover(o.token);
      continue;
    }
    const size_t before = cl.obliques.size();
    if (!obliqueInto(o, c, cl)) {
      // a place adverb inside a PP ("in here")
      c.cover(o.np.tokens);
    }
    // C22: in a where / what question with "be", a phrase that belongs to the subject noun stays after it ("Where is
    // the road to the city?" -> Ubi est via ad urbem?, not "Ubi ad urbem est via?")
    if (c.st.lang == frame::SrcLang::En && f.type == Kind::Wh && f.hasSubject && f.pred.lemma == "be" &&
             o.token >= 0 && (size_t)o.token < c.s.tokens.size() && c.s.tokens[(size_t)o.token].head - 1 == f.subject.token &&
             f.subject.token >= 0 && !o.front)
      for (size_t q = before; q < cl.obliques.size(); ++q) cl.obliques[q].after = true;
  }
  // copula predicate
  if ((f.copula || (f.hasPred && c.st.lang == frame::SrcLang::En && (!f.predicative.empty() || !f.predAdj.empty()))) && !stateVerb) {   // C17: become + predicate
    for (const SemNP& pn : f.predicative) {
      // C22: "be that way", "be this way" -> ita esse (the manner, not a road)
      if (c.st.lang == frame::SrcLang::En && text::lower(pn.head) == "way" && (pn.determiner == "that" || pn.determiner == "this") &&
          pn.adjectives.empty() && pn.genitive.empty() && latin("ita", Adv) != kNone) {
        cl.adverbs.push_back(realise::LaAdverb{latin("ita", Adv), realise::AdvPos::BeforeVerb});
        Choice ch;
        ch.token = pn.token;
        ch.source = pn.determiner + " way";
        ch.lemma = latin("ita", Adv);
        ch.kind = "table";
        c.out.choices.push_back(ch);
        c.cover(pn.tokens);
        continue;
      }
      if (f.type == Kind::Wh && f.wh.role == frame::Role::Predicate && pn.isPronoun && pn.interrogative) continue;
      // C15: "the Golden Cap is yours" -> tuus est (the possessive agrees with the subject)
      if (pn.isPronoun && (pn.pronLemma == "mine" || pn.pronLemma == "yours" || pn.pronLemma == "ours")) {
        const bool pl2 = pn.pronLemma == "yours" && c.mem.addresseePlural;
        LaAdj pa;
        pa.lemma = latin(pn.pronLemma == "mine" ? "meus" : pn.pronLemma == "ours" ? "noster" : pl2 ? "vester" : "tuus", Det);
        if (pa.lemma == kNone) pa.lemma = latin(pn.pronLemma == "mine" ? "meus" : pn.pronLemma == "ours" ? "noster" : "tuus", Adj);
        if (pa.lemma != kNone) {
          cl.predAdj.push_back(pa);
          Choice ch;
          ch.token = pn.token;
          ch.source = pn.pronLemma;
          ch.lemma = pa.lemma;
          ch.kind = "table";
          c.out.choices.push_back(ch);
          c.cover(pn.tokens);
          continue;
        }
      }
      LaNP x;
      npInto(pn, c, x);
      x.indefinite = pn.determiner == "a" || pn.determiner == "an";   // C17
      // C17: a person noun predicate of a feminine subject takes its feminine ("She is only a child" -> puella)
      if (c.st.lang == frame::SrcLang::En && x.head != kNone && !x.isName && !x.nameWords && !x.capitalise && !x.isPronoun && cl.hasSubject) {
        const uint8_t sg = cl.subject.gender ? cl.subject.gender : cl.subject.isPronoun ? cl.subject.pron.gender
                         : cl.subject.head != kNone ? simpleGender(la_.lemma(cl.subject.head).gender) : (uint8_t)0;
        if (sg == F && animate(pn)) x.head = feminineOf(x.head);
      }
      // C17: an exclamation stresses the quality: the adjective of the predicate noun comes first ("You are a wicked
      // creature!" -> Mala bēstia es!)
      if (c.st.lang == frame::SrcLang::En && f.punct == "!" && !x.isName && !x.nameWords)
        for (LaAdj& xa : x.adjectives) xa.before = true;
      cl.predicative.push_back(x);
    }
    for (const frame::SemAdj& a : f.predAdj) {
      Choice ch;
      ch.token = a.token;
      LaAdj la;
      la.degree = a.degree;
      // C22: "It is (very) important" -> magnī (maximī) mōmentī est: a genitive of quality as the predicate
      if (c.st.lang == frame::SrcLang::En && text::lower(a.lemma) == "important" && a.participle == 0 && f.predAdj.size() == 1) {
        bool very = false;
        for (const std::string& v : a.adverbs) very = very || v == "very" || v == "awfully" || v == "really" || v == "terribly" || v == "so";
        LaNP g = momentNP(*this, very);
        g.case_ = Gen;
        cl.predicative.push_back(g);
        ch.source = a.lemma;
        ch.lemma = latin("mōmentum", Noun);
        ch.kind = "table";
        ch.note = std::string("genitive of quality: ") + (very ? "maximī" : "magnī") + " mōmentī";
        c.out.choices.push_back(ch);
        c.cover(a.token);
        for (int t : a.advTokens) c.cover(t);
        continue;
      }
      if (stateAdj && &a == &f.predAdj[0] && (la.lemma = latin(stateAdj->latin.c_str(), Adj)) != kNone) {
        ch.source = a.lemma;
        ch.lemma = la.lemma;
        ch.kind = "table";
        ch.note = "states_en_la.tsv";
      } else {
        la.lemma = adjectiveInto(a, c, la, ch);
      }
      // C22: a predicate the tagger read as an adjective that Latin has only as a noun ("That is nonsense." -> nūgae)
      if (la.lemma == kNone && f.predAdj.size() == 1 && f.predicative.empty() && a.participle == 0) {
        Choice cn;
        cn.token = a.token;
        const uint32_t nid = select(a.lemma, Noun, c.context, false, false, c.st, cn);
        if (nid != kNone) {
          LaNP x;
          x.head = nid;
          if (pluralOnly(nid)) x.number = Pl;
          cl.predicative.push_back(x);
          c.out.choices.push_back(cn);
          c.cover(a.token);
          continue;
        }
      }
      c.out.choices.push_back(ch);
      c.cover(a.token);
      if (la.lemma == kNone) { c.out.unknownWords.push_back(a.lemma); continue; }
      for (size_t i = 0; i < a.adverbs.size(); ++i) {
        const uint32_t av = adverb(a.adverbs[i], i < a.advTokens.size() ? a.advTokens[i] : -1, c, false);
        if (av != kNone) la.adverbs.push_back(av);
      }
      cl.predAdj.push_back(la);
    }
    // C22: a demonstrative subject agrees with the predicate noun ("This is my cat." -> Haec fēlēs mea est; "These
    // are ...": as before); "it" / "that" pointing at the situation is dropped and the verb agrees with the
    // predicate ("That is nonsense." -> Nūgae sunt.)
    if (c.st.lang == frame::SrcLang::En && f.hasSubject && f.subject.isPronoun && cl.hasSubject && cl.predAdj.empty() &&
        cl.predicative.size() == 1 && cl.predicative[0].head != kNone && !cl.predicative[0].isPronoun &&
        f.type != Kind::Wh && f.subject.relative.empty()) {
      const LaNP& x = cl.predicative[0];
      const std::string sp = f.subject.pronLemma;
      const uint8_t pg = x.gender ? x.gender : simpleGender(la_.lemma(x.head).gender);
      const uint8_t pnum = x.number ? x.number : (uint8_t)Sg;
      if ((sp == "this" || sp == "these") && pg) {
        cl.subject.gender = pg;
        cl.subject.number = pnum;
      } else if ((sp == "that" || sp == "it") && pg) {
        LaNP ps;
        ps.isPronoun = true;
        ps.pron.person = 3;
        ps.pron.number = pnum;
        ps.pron.gender = pg;
        ps.number = pnum;
        ps.gender = pg;
        cl.subject = ps;
      }
    }
    // C22: a predicate adjective of "you" (one person) whose masculine and feminine differ, with no name to tell the
    // gender of the person addressed, is a guess (Check): "You are very kind." -> benignus / benigna
    if (cl.hasSubject && cl.subject.isPronoun && cl.subject.pron.person == 2 && cl.subject.pron.number != Pl &&
        !c.mem.addresseeGender)
      for (const LaAdj& pa : cl.predAdj) {
        std::string fm, ff;
        const bool gm = pa.lemma != kNone && morph::generate(la_, pa.lemma, morph::adjForm(Nom, Sg, M), fm, false);
        const bool gf = pa.lemma != kNone && morph::generate(la_, pa.lemma, morph::adjForm(Nom, Sg, F), ff, false);
        if (gm && gf && fm != ff) { c.out.flags.push_back("addressee-gender"); break; }
      }
    const bool itSubj = f.hasSubject && f.subject.isPronoun && (f.subject.pronLemma == "it" || f.subject.pronLemma == "ello");
    const bool impersAdj = f.predAdj.size() == 1 && tables::impersonalAdjective(f.predAdj[0].lemma);
    // C22: an order "Don't be silly!" / "Be careful!" speaks to a person: the addressee's gender (masculine when no
    // name tells it, a guess: Check), never the neuter of "it is silly"
    if (!cl.predAdj.empty() && f.type == Kind::Imp && !f.hasSubject) {
      cl.predGender = c.mem.addresseeGender ? c.mem.addresseeGender : (uint8_t)M;
      cl.predNumber = f.imperativePlural || c.mem.addresseePlural ? (uint8_t)Pl : (uint8_t)Sg;
      if (!c.mem.addresseeGender)
        for (const LaAdj& pa : cl.predAdj) {
          std::string fm, ff;
          if (pa.lemma != kNone && morph::generate(la_, pa.lemma, morph::adjForm(Nom, Sg, M), fm, false) &&
              morph::generate(la_, pa.lemma, morph::adjForm(Nom, Sg, F), ff, false) && fm != ff) {
            c.out.flags.push_back("addressee-gender");
            break;
          }
        }
    } else if (!cl.predAdj.empty() && (!f.hasSubject || f.pred.impersonal || (itSubj && (impersAdj || !c.mem.lastGender)))) {
      cl.predGender = N;
      if (itSubj) { cl.subject.pron.gender = N; cl.subject.gender = N; }
    }
  }
  // C17: participle / adjective phrases describing the subject (", paleā fartum", ", omnia timēns"): a state adjective
  // of states_en_la.tsv kind verb is that verb's present participle with its "of" phrase as the object
  for (const SemFrame& sf : f.secondary) {
    LaClause sc;
    sc.type = realise::ClauseType::Frag;
    bool any = false;
    for (const frame::SemAdj& a : sf.predAdj) {
      Choice ch;
      ch.token = a.token;
      LaAdj la;
      la.degree = a.degree;
      const curated::StateEntry* se = a.participle ? nullptr : cd_.state(text::lower(a.lemma));
      uint32_t sv = kNone;
      if (se && se->kind == "verb" && se->latin.find(' ') == std::string::npos) sv = latin(se->latin.c_str(), Verb);
      if (sv != kNone) {
        la.lemma = sv;
        la.participle = Present;
        ch.source = a.lemma;
        ch.lemma = sv;
        ch.kind = "table";
        ch.note = "states_en_la.tsv (participle)";
      } else {
        la.lemma = adjectiveInto(a, c, la, ch);
      }
      c.out.choices.push_back(ch);
      c.cover(a.token);
      if (la.lemma == kNone) { c.out.unknownWords.push_back(a.lemma); continue; }
      for (size_t i = 0; i < a.adverbs.size(); ++i) {
        const uint32_t av = adverb(a.adverbs[i], i < a.advTokens.size() ? a.advTokens[i] : -1, c, false);
        if (av != kNone) la.adverbs.push_back(av);
      }
      sc.predAdj.push_back(la);
      any = true;
      for (const frame::SemOblique& o : sf.obliques) {
        if (sv != kNone && (o.prep == "of" || o.prep == "about") && !sc.hasObject) {
          sc.hasObject = true;
          npInto(o.np, c, sc.object);
          sc.object.case_ = Acc;
          c.cover(o.token);
          continue;
        }
      }
    }
    if (!any) continue;
    if (sf.hasObject && !sc.hasObject) {   // C19: "seeing the wolf" -> lupum vidēns
      sc.hasObject = true;
      npInto(sf.object, c, sc.object);
      sc.object.case_ = Acc;
      c.cover(sf.object.tokens);
    }
    for (const frame::SemOblique& o : sf.obliques) {
      if (sc.hasObject && (o.prep == "of" || o.prep == "about")) continue;
      if (!obliqueInto(o, c, sc)) c.cover(o.np.tokens);
    }
    realise::LaSub sub;
    sub.rel = realise::SubRel::Apposition;
    // C19: a phrase between the subject and the verb stays there ("Pāstor, lupum vidēns, fūgit")
    if (!sf.tokens.empty() && f.hasSubject && f.subject.token >= 0 && f.pred.token >= 0) {
      const int a0 = *std::min_element(sf.tokens.begin(), sf.tokens.end());
      sub.afterSubject = a0 > f.subject.token && a0 < f.pred.token;
    }
    sub.clause.push_back(sc);
    cl.subs.push_back(sub);
  }
  // fragment adjectives ("A little.", "Very strange!")
  if (!f.hasPred && !f.predAdj.empty()) {
    for (const frame::SemAdj& a : f.predAdj) {
      Choice ch;
      ch.token = a.token;
      LaAdj la;
      la.lemma = adjectiveInto(a, c, la, ch);
      c.out.choices.push_back(ch);
      c.cover(a.token);
      if (la.lemma == kNone) { c.out.unknownWords.push_back(a.lemma); continue; }
      for (size_t i = 0; i < a.adverbs.size(); ++i) {
        const uint32_t av = adverb(a.adverbs[i], i < a.advTokens.size() ? a.advTokens[i] : -1, c, false);
        if (av != kNone) la.adverbs.push_back(av);
      }
      cl.predAdj.push_back(la);
      cl.predGender = N;
    }
  }
  // C22: "Mr. Rabbit." / "Miss Lucy!" on its own: the person called (vocative), not a nominative fragment
  if (c.st.lang == frame::SrcLang::En && cl.type == realise::ClauseType::Frag && f.hasSubject && cl.hasSubject &&
      !f.hasPred && f.predAdj.empty() && f.obliques.empty() && f.subject.token > 0 && !f.subject.isPronoun) {
    int first = f.subject.token;
    for (int t : f.subject.tokens) first = std::min(first, t);
    std::string tl = first >= 0 && (size_t)first < c.s.tokens.size() ? c.s.tokens[(size_t)first].text : std::string();
    if (tl == "Mr." || tl == "Mrs." || tl == "Ms." || tl == "Miss" || tl == "Mr" || tl == "Mrs" || tl == "Mister") {
      cl.object = cl.subject;
      cl.object.case_ = Voc;
      cl.hasObject = true;
      cl.hasSubject = false;
      cl.subject = LaNP{};
    }
  }
  // exclamation "what a strange garden": the NP is the subject
  // vocatives, interjections, adverbs, discourse, connectors
  for (const SemNP& v : f.vocatives) {
    LaNP x;
    if (!(c.st.lang == frame::SrcLang::En && !v.possessor.empty() && childAddress(*this, v, c.st, x, c.out, c.mem.addresseeGender))) {   // C20
      x = LaNP{};
      npInto(v, c, x);
      addresseeNoun(*this, v, c.st, c.mem, x);
    }
    cl.vocatives.push_back(x);
    if (x.number == Pl) c.mem.sawPlural = true;
  }
  for (const std::string& ij : f.interjections) {
    if (const char* la = tables::interjection(ij)) {
      const uint32_t id = latin(la, Intj);
      if (id != kNone) cl.interjections.push_back(id);
    } else if (!f.hasPred && f.predAdj.empty() && !f.hasSubject && f.adverbs.empty()) {
      c.out.unknownWords.push_back(ij);   // never an empty translation: the unknown word stays in brackets
    }
  }
  for (const frame::SemAdverb& a : f.adverbs) {
    // C19: "I shall be all melted", "The cake was all eaten", "I am all wet": "all" describing the subject is tōtus
    // agreeing with it (nominative, before the verb), not the adverb omnīnō
    if (c.st.lang == frame::SrcLang::En && a.lemma == "all" && f.hasSubject && f.hasPred &&
        (f.pred.voice == frame::Voice::Passive || (f.copula && !f.predAdj.empty())) && latin("tōtus", Adj) != kNone) {
      LaOblique o;
      o.case_ = Nom;
      o.np.head = latin("tōtus", Adj);
      uint8_t g = 0, num = Sg;
      if (f.subject.isPronoun) {
        num = f.subject.pron.number == 2 ? Pl : Sg;
        g = f.subject.pron.gender;
        if (f.subject.pron.person < 3 || !g) {
          char sg = c.st.speakerGender;
          if (c.st.flipSpeakerGender) sg = sg == 'f' ? 'm' : 'f';
          g = f.subject.pron.person < 3 ? (uint8_t)(sg == 'f' ? F : M) : (g ? g : (uint8_t)M);
          if (f.subject.pron.person < 3) c.out.flags.push_back("speaker-gender");
        }
      } else {
        g = cl.hasSubject && cl.subject.gender ? cl.subject.gender : 0;
        if (!g && cl.hasSubject && cl.subject.head != kNone) {
          const lex::Lemma sl = la_.lemma(cl.subject.head);
          g = (uint8_t)(sl.gender == F || sl.gender == FN ? F : sl.gender == N ? N : M);
        }
        num = cl.hasSubject ? cl.subject.number : (uint8_t)Sg;
      }
      o.np.gender = g ? g : (uint8_t)M;
      o.np.number = num;
      o.np.case_ = Nom;
      if (num == Pl && latin("omnis", Adj) != kNone) o.np.head = latin("omnis", Adj);   // "all tired" (they) -> omnēs
      cl.obliques.push_back(o);
      Choice ch;
      ch.token = a.token;
      ch.source = "all";
      ch.lemma = o.np.head;
      ch.kind = "table";
      ch.note = "all (completely) -> tōtus";
      c.out.choices.push_back(ch);
      c.cover(a.token);
      continue;
    }
    if (a.lemma == "first") c.mem.sawFirst = true;
    if (a.ellipticWh) {   // "I don't much care where." -> ... cūrō quō (indirect question without its verb)
      const char* w = a.lemma == "where" ? ((prevMotion || c.motion) ? "quō" : "ubi")
                    : a.lemma == "when" ? "quandō" : a.lemma == "why" ? "cūr" : "quōmodo";
      realise::LaAdverb la;
      la.lemma = latin(w, Adv);
      if (la.lemma == kNone) la.lemma = latin(w);
      la.pos = realise::AdvPos::End;
      c.cover(a.token);
      if (la.lemma == kNone) continue;
      Choice ch;
      ch.token = a.token;
      ch.source = a.lemma;
      ch.lemma = la.lemma;
      ch.kind = "table";
      ch.note = "elliptical indirect question";
      c.out.choices.push_back(ch);
      // C24: "I don't know where." / "She doesn't know where." (a place, no motion): the indirect question with its verb
      // understood, "ubi sit" (subjunctive of sum by the sequence of tenses): "Nesciō ubi sit."
      const uint32_t sumL = latin("sum", Verb);
      if (std::string(w) == "ubi" && sumL != kNone && c.st.lang == frame::SrcLang::En) {
        realise::LaSub sb;
        sb.rel = realise::SubRel::IndirectQ;
        LaClause q;
        q.type = realise::ClauseType::Wh;
        q.wh.lemma = la.lemma;
        q.wh.role = realise::Role::Oblique;
        q.pred.lemma = sumL;
        q.pred.mood = Subjunctive;
        const bool past = f.pred.tense == frame::Tense::Past;
        q.pred.tense = past ? (uint8_t)Imperfect : (uint8_t)Present;
        q.pred.person = 3;
        q.pred.number = Sg;
        sb.clause.push_back(q);
        cl.subs.push_back(sb);
        continue;
      }
      cl.adverbs.push_back(la);
      continue;
    }
    // C17: "go home" -> domum īre, "stay home" -> domī manēre (domus: accusative of goal, locative)
    if (a.lemma == "home" && c.st.lang == frame::SrcLang::En) {
      const uint32_t domus = latin("domus", Noun);
      if (domus != kNone) {
        LaOblique o;
        o.case_ = c.motion ? (uint8_t)Acc : (uint8_t)Loc;
        o.np.head = domus;
        o.np.case_ = o.case_;
        o.front = a.front;
        cl.obliques.push_back(o);
        Choice ch;
        ch.token = a.token;
        ch.source = a.lemma;
        ch.lemma = domus;
        ch.kind = "table";
        ch.note = c.motion ? "home (goal): domum" : "home (place): domī";
        c.out.choices.push_back(ch);
        c.cover(a.token);
        continue;
      }
    }
    // C17: "I am only a Scarecrow" -> Terriculum tantum sum: "only" restricting the predicate noun follows it
    // C22: also "just" ("It's just a rabbit." -> Cunīculus tantum est; "just" was dropped silently)
    if ((a.lemma == "only" || (a.lemma == "just" && cl.predicative.size() == 1 && cl.predicative[0].head != kNone)) &&
        c.st.lang == frame::SrcLang::En && f.copula && cl.predicative.size() == 1) {
      const uint32_t tantum = latin("tantum", Adv);
      if (tantum != kNone) {
        LaAdj t;
        t.lemma = tantum;
        cl.predicative[0].adjectives.push_back(t);
        Choice ch;
        ch.token = a.token;
        ch.source = a.lemma;
        ch.lemma = tantum;
        ch.kind = "table";
        ch.note = "only + predicate noun: tantum after the noun";
        c.out.choices.push_back(ch);
        c.cover(a.token);
        continue;
      }
    }
    // C17: an adverb with its own degree word ("so badly", "very slowly"): both words, the degree word first
    if (a.lemma.find(' ') != std::string::npos && c.st.lang == frame::SrcLang::En) {
      const std::vector<std::string> aw = words(a.lemma);
      bool all = true;
      std::vector<uint32_t> ids;
      for (const std::string& w : aw) {
        const char* la0 = tables::adverb(w, c.motion);
        uint32_t id = la0 ? latin(la0, Adv) : kNone;
        if (id == kNone) {
          Choice probe;
          id = select(w, Adv, c.context, false, false, c.st, probe);
        }
        if (id == kNone) { all = false; break; }
        ids.push_back(id);
      }
      if (all && !ids.empty()) {
        c.cover(a.token);
        for (size_t q = 1; q < aw.size(); ++q) {   // the degree word's own token(s) before the adverb
          const int tq = a.token - (int)q;
          if (tq >= 0 && (size_t)tq < c.s.tokens.size() && c.s.tokens[(size_t)tq].lower == aw[aw.size() - 1 - q]) c.cover(tq);
        }
        for (uint32_t id : ids) {
          realise::LaAdverb x;
          x.lemma = id;
          x.pos = a.front ? realise::AdvPos::Front : realise::AdvPos::BeforeVerb;
          cl.adverbs.push_back(x);
          Choice ch;
          ch.token = a.token;
          ch.source = a.lemma;
          ch.lemma = id;
          ch.kind = "table";
          c.out.choices.push_back(ch);
        }
        continue;
      }
    }
    // C24: an invented adverb made of a preposition and a noun ("They flew overcloud.") -> super nūbem
    if (c.st.lang == frame::SrcLang::En && c.st.srcLex && a.token >= 0 && (size_t)a.token < c.s.tokens.size()) {
      std::vector<lex::Analysis> an;
      const std::string low = c.s.tokens[(size_t)a.token].lower;
      c.st.srcLex->lookup(text::en_key(low), an);
      uint32_t prep = kNone, noun = kNone;
      uint8_t cs = 0;
      if (an.empty() && inventedPrep(*this, low, c.st, c.context, a.token, c.out, prep, noun, cs)) {
        LaOblique o;
        o.prep = prep;
        o.case_ = cs;
        o.np.head = noun;
        o.np.case_ = cs;
        o.front = a.front;
        cl.obliques.push_back(o);
        c.cover(a.token);
        continue;
      }
    }
    realise::LaAdverb la;
    la.lemma = adverb(a.lemma, a.token, c, c.motion);
    if (la.lemma == kNone) continue;
    // order.adv: a time adverb opens the clause only when the source puts it first ("Today the teacher ...")
    la.pos = a.front ? realise::AdvPos::Front : realise::AdvPos::BeforeVerb;
    // "never" carries the negation itself (numquam): the realiser drops nōn
    if ((a.lemma == "never" || a.lemma == "nunca") && cl.polarity == realise::Polarity::Neg) {}
    cl.adverbs.push_back(la);
  }
  // C17: an adverb that belongs to a noun of the clause ("on the rocks below") stays next to the obliques, before
  // the clause's own adverbs ("in saxīs īnfrā graviter contūsus")
  if (c.st.lang == frame::SrcLang::En && cl.adverbs.size() > 1) {
    std::vector<int> nounAdv;
    for (const frame::SemAdverb& a : f.adverbs)
      if (a.token >= 0 && (size_t)a.token < c.s.tokens.size()) {
        const int hd = c.s.tokens[(size_t)a.token].head - 1;
        if (hd >= 0 && (size_t)hd < c.s.tokens.size() && (c.s.tokens[(size_t)hd].upos == "NOUN" || c.s.tokens[(size_t)hd].upos == "PROPN"))
          nounAdv.push_back(a.token);
      }
    if (!nounAdv.empty()) {
      std::vector<uint32_t> first;
      for (int t : nounAdv) {
        const std::string lw = c.s.tokens[(size_t)t].lower;
        const char* la0 = tables::adverb(lw, false);
        uint32_t id = la0 ? latin(la0, Adv) : kNone;
        if (id == kNone)
          for (const Choice& ch : c.out.choices)
            if (ch.token == t) id = ch.lemma;
        if (id != kNone) first.push_back(id);
      }
      std::stable_partition(cl.adverbs.begin(), cl.adverbs.end(), [&](const realise::LaAdverb& a) {
        return std::find(first.begin(), first.end(), a.lemma) != first.end();
      });
    }
  }
  // C17: "badly" with a participle of damage is graviter ("graviter contūsus"; male contūsus is "clumsily dented")
  if (cl.type == realise::ClauseType::Frag && !cl.predAdj.empty() && cl.predAdj.back().participle == Perfect) {
    const uint32_t male = latin("male", Adv), graviter = latin("graviter", Adv);
    for (realise::LaAdverb& a : cl.adverbs)
      if (a.lemma == male && graviter != kNone) a.lemma = graviter;
  }
  for (const std::string& d : f.discourse)
    if (d == "please") {
      const uint32_t q = latin("quaesō", Verb);
      if (q != kNone) cl.politeness.push_back(q);
    }
  std::vector<std::string> conns = f.connectors;   // C17: "so" + "that" read as two connectors is "so that"
  for (size_t i = 0; i + 1 < conns.size(); ++i)
    if (conns[i] == "so" && conns[i + 1] == "that") { conns[i] = "so that"; conns.erase(conns.begin() + (long)i + 1); }
  for (const std::string& k : conns) {
    // C17: a clause cut from its sentence that starts with "so that" is a purpose clause: ut + subjunctive ("so that
    // in reality I may become the King of Beasts" -> ut rē vērā Rēx Bēstiārum fīam)
    if (k == "so that" && c.st.lang == frame::SrcLang::En && f.hasPred) {
      uint32_t ut = latin("ut", Conj);
      if (ut == kNone) ut = latin("ut");
      if (ut != kNone) {
        cl.connectors.push_back(ut);
        cl.pred.mood = Subjunctive;
        if (cl.pred.tense == Future) cl.pred.tense = Present;
        if (cl.pred.tense == Perfect) cl.pred.tense = Imperfect;
        if (f.pred.modality == Modality::May) {
          const uint32_t fort = latin("fortasse", Adv);
          cl.adverbs.erase(std::remove_if(cl.adverbs.begin(), cl.adverbs.end(),
                                          [&](const realise::LaAdverb& a) { return a.lemma == fort; }),
                           cl.adverbs.end());
        }
        continue;
      }
    }
    // C15: "Then you must go ..." (a statement): tum, first; "Then go away." / "Then why ...?": igitur, second
    if (k == "then" && !c.mem.prevFirst && (f.type == Kind::Decl || f.type == Kind::Frag)) {
      const uint32_t tum = latin("tum", Adv);
      if (tum != kNone) { cl.connectors.push_back(tum); continue; }
    }
    if (k == "then" && c.mem.prevFirst) {   // "First write ... Then write ...": deinde, an adverb at the front
      realise::LaAdverb a;
      a.lemma = latin("deinde", Adv);
      a.pos = realise::AdvPos::Front;
      if (a.lemma != kNone) { cl.adverbs.insert(cl.adverbs.begin(), a); continue; }
    }
    const char* la = tables::connector(k, c.mem.prevFirst);
    if (!la) continue;
    const uint32_t id = latin(la);
    if (k == "if") {   // C15: "even if" -> etiam sī (the adverb goes with the conjunction)
      const uint32_t etiam = latin("etiam", Adv);
      auto ev = std::find_if(cl.adverbs.begin(), cl.adverbs.end(), [&](const realise::LaAdverb& a) { return a.lemma == etiam; });
      if (ev != cl.adverbs.end() && etiam != kNone) { cl.adverbs.erase(ev); cl.connectors.push_back(etiam); }
    }
    if (id != kNone) cl.connectors.push_back(id);
  }
  // wh word
  if (f.type == Kind::Wh) {
    const std::string& w = f.wh.word;
    const char* la = nullptr;
    realise::Role role = realise::Role::None;
    uint8_t gender = 0;
    // C15: "Where did you get the shoes?" asks where from: unde
    if (w == "where" && (f.pred.lemma == "get" || f.pred.lemma == "obtain" || f.pred.lemma == "receive" ||
                         f.pred.lemma == "buy" || f.pred.lemma == "take" ||
                         std::any_of(f.obliques.begin(), f.obliques.end(), [](const frame::SemOblique& o) { return o.prep == "from"; })))
      la = "unde";
    else if (w == "where") la = c.motion ? "quō" : "ubi";
    else if (w == "whither") la = "quō";
    else if (w == "why") la = "cūr";
    else if (w == "how") la = "quōmodo";
    else if (w == "when") la = "quandō";
    else if ((w == "who" || w == "what") && f.wh.role != frame::Role::Oblique &&   // C24: the oblique NP says it
             !((f.wh.role == frame::Role::Subject && f.hasSubject && !f.subject.isPronoun) ||
               (f.wh.role == frame::Role::Object && f.hasObject && !f.object.isPronoun) ||
               (f.wh.role == frame::Role::Predicate &&
                (f.predicative.empty() || !f.predicative[0].isPronoun)))) {
      la = "quis";
      gender = w == "what" ? N : M;
      role = f.wh.role == frame::Role::Subject ? realise::Role::Subject
           : f.wh.role == frame::Role::Object ? realise::Role::Object
           : f.wh.role == frame::Role::Predicate ? realise::Role::Predicate : realise::Role::None;
      if (f.wh.role == frame::Role::Subject) cl.hasSubject = false;
    }
    if (la) {
      cl.wh.lemma = latin(la, (w == "who" || w == "what") ? Pron : 0);
      cl.wh.role = role;
      cl.wh.gender = gender;
      c.cover(f.wh.token);
    }
  }
  // subordinate clauses
  for (const frame::SemSub& sb : f.subordinate) {
    if (sb.frame.empty()) continue;
    // "Only if you believe it is.": an elliptical "it is" complement is the object id ("Sī modo id crēdis")
    const SemFrame& sf = sb.frame[0];
    if (sb.relation == Relation::Complement && sf.type != Kind::Wh && sf.pred.lemma == "be" && sf.predicative.empty() &&
        sf.predAdj.empty() && sf.obliques.empty() && sf.adverbs.empty() && !sf.existential && sf.subordinate.empty() &&
        sf.hasSubject && sf.subject.isPronoun && sf.subject.pronLemma == "it" && !cl.hasObject) {
      c.cover(sf.tokens);
      cl.hasObject = true;
      LaNP id;
      id.isPronoun = true;
      id.pron.person = 3;
      id.pron.gender = N;
      id.gender = N;
      id.case_ = Acc;
      cl.object = id;
      continue;
    }
    // C19: "I will meet them as they come" -> eīs venientibus occurram: an "as" clause whose pronoun subject is the
    // main clause's object (same number, 3rd person) and that has nothing else is that object's present participle
    if (c.st.lang == frame::SrcLang::En && sb.marker == "as" && !sb.before && cl.hasObject && f.hasObject &&
        sf.hasPred && sf.hasSubject && sf.subject.isPronoun && sf.subject.pron.person == 3 && !sf.hasObject &&
        sf.obliques.empty() && sf.predAdj.empty() && sf.predicative.empty() && !sf.negative && sf.subordinate.empty() &&
        sf.pred.modality == Modality::None && sf.pred.voice == frame::Voice::Active && sf.pred.lemma != "be" &&
        sf.pred.particle.empty() &&
        ((f.object.isPronoun && f.object.pron.person == 3 && f.object.pron.number == sf.subject.pron.number) ||
         (!f.object.isPronoun && !f.object.isName && (f.object.number == 2) == (sf.subject.pron.number == 2)))) {
      Choice ch;
      ch.token = sf.pred.token;
      const uint32_t v = select(sf.pred.lemma, Verb, c.context, false, false, c.st, ch);
      std::string probe;
      if (v != kNone && morph::generate(la_, v, morph::participle(Present, Active, Nom, Pl, M), probe, false)) {
        LaAdj pa;
        pa.lemma = v;
        pa.participle = Present;
        pa.after = true;
        cl.object.adjectives.push_back(pa);
        ch.note = "as + clause -> present participle of the object";
        c.out.choices.push_back(ch);
        c.cover(sf.tokens);
        c.cover(sf.subject.token);
        continue;
      }
    }
    realise::LaSub ls;
    ls.before = sb.before;
    LaClause sc;
    const bool keepSi = c.afterSi;
    c.afterSi = sb.relation == Relation::Condition;
    clauseInto(sb.frame[0], c, sc);
    c.afterSi = keepSi;
    c.frame = &f;
    // C20: an imperative, a comma and a statement with its own subject ("Run, the dragon is coming!", "Hide, the witch
    // is here!"): two sentences side by side (asyndeton): the comma stays, no et, no accusative + infinitive
    auto asyndeton = [&]() {
      if (f.type != Kind::Imp || !(sb.marker.empty() || (sb.marker == "that" && sb.relation == Relation::Complement)) || sb.before || sf.type != Kind::Decl || !sf.hasSubject ||
          !sc.hasSubject || !sc.connectors.empty() || sc.type != realise::ClauseType::Decl || f.hasObject)
        return false;
      int first = -1;   // the sub's first word (its comma may hang on its verb)
      for (int t : sf.tokens)
        if (t >= 0 && (size_t)t < c.s.tokens.size() && c.s.tokens[(size_t)t].upos != "PUNCT" && (first < 0 || t < first))
          first = t;
      return first > 0 && c.s.tokens[(size_t)first - 1].text == "," && c.s.tokens[(size_t)first].lower != "that";
    };
    // C20: a result clause follows the sequence of tenses: after an imperfect or pluperfect main verb the imperfect
    // subjunctive ("Tam ēsuriēbāmus ut omnia ēderēmus"); after a perfect the perfect subjunctive of an actual result
    // stays ("Tam male cecinit ut rīserīmus") and a present becomes imperfect
    auto resultSequence = [&]() {
      if (c.st.lang != frame::SrcLang::En || sc.pred.modal != kNone) return;
      if ((cl.pred.tense == Imperfect || cl.pred.tense == Pluperfect) && (sc.pred.tense == Perfect || sc.pred.tense == Present))
        sc.pred.tense = Imperfect;
      else if (cl.pred.tense == Perfect && sc.pred.tense == Present)
        sc.pred.tense = Imperfect;
    };
    if ((sb.relation == Relation::Coord || (sb.relation == Relation::Complement && c.st.lang == frame::SrcLang::En)) &&
        asyndeton()) {
      ls.rel = realise::SubRel::Coord;
      ls.asyndeton = true;
      ls.sep = ",";
      ls.clause.push_back(std::move(sc));
      cl.subs.push_back(std::move(ls));
      continue;
    }
    switch (sb.relation) {
      case Relation::Cause: ls.rel = realise::SubRel::Cause; break;
      case Relation::Manner:   // C17: "as men call me" -> ut hominēs mē vocant (ut + indicative)
        ls.rel = realise::SubRel::Cause;
        ls.conj = latin("ut", Conj);
        if (ls.conj == kNone) ls.conj = latin("ut");
        break;
      case Relation::Time:
        ls.rel = realise::SubRel::Time;
        if (sb.marker == "before") {
          ls.conj = latin("antequam", Conj);
          if (f.type == Kind::Imp || f.pred.modality == Modality::Let) sc.pred.mood = Subjunctive;
        } else if (sb.marker == "after") ls.conj = latin("postquam", Conj);
        else if (sb.marker == "as soon as") {   // C19: ubi + perfect ("as soon as the bell rang")
          ls.conj = latin("ubi", Conj);
          if (ls.conj == kNone) ls.conj = latin("ubi");
        }
        // C20: "until" + a past event is dōnec + perfect indicative ("dōnec lūna orta est"); else dum
        else if (sb.marker == "until" && c.st.lang == frame::SrcLang::En && sc.pred.tense == Perfect &&
                 sc.pred.mood == Indicative && latin("dōnec", Conj) != kNone)
          ls.conj = latin("dōnec", Conj);
        else if (sb.marker == "while" || sb.marker == "until") ls.conj = latin("dum", Conj);
        break;
      case Relation::Condition:
        ls.rel = realise::SubRel::Condition;
        if (sb.marker == "unless") ls.conj = latin("nisi", Conj);
        // C22: an unreal condition: "If I had a dog, I would be happy." -> Sī canem habērem, laeta essem: the "if"
        // clause in the past goes into the subjunctive of the main clause's tense (imperfect; pluperfect for "had
        // had" / "would have")
        if (c.st.lang == frame::SrcLang::En && cl.pred.mood == Subjunctive && f.pred.mood == frame::SrcMood::Conditional &&
            (sf.pred.tense == frame::Tense::Past || sf.pred.mood == frame::SrcMood::Subjunctive || sf.pred.pastModal)) {
          sc.pred.mood = Subjunctive;
          sc.pred.tense = sf.pred.aspect == frame::Aspect::Perfect || cl.pred.tense == Pluperfect ? (uint8_t)Pluperfect
                                                                                                 : (uint8_t)Imperfect;
          // C24: a second condition joined by "and" ("If dogs could talk and cats could sing") is unreal too
          for (realise::LaSub& cs2 : sc.subs)
            if (cs2.rel == realise::SubRel::Coord && !cs2.clause.empty() && cs2.clause[0].pred.mood == Indicative) {
              cs2.clause[0].pred.mood = Subjunctive;
              cs2.clause[0].pred.tense = sc.pred.tense;
            }
        }
        break;
      case Relation::Purpose:
        // C17: sequence of tenses: after an imperfect or pluperfect main verb the purpose clause is in the imperfect
        // subjunctive ("nēmō mē adiuvāre poterat ut id quaererem"); after a perfect both sequences occur (vēnī ut
        // videam, as before)
        if (c.st.lang == frame::SrcLang::En && sc.pred.tense == Present &&
            (cl.pred.tense == Imperfect || cl.pred.tense == Pluperfect))
          sc.pred.tense = Imperfect;
        // C17: "so that I may see" -> ut videam: the subjunctive carries "may" (no fortasse)
        if (sf.pred.modality == Modality::May) {
          const uint32_t fort = latin("fortasse", Adv);
          sc.adverbs.erase(std::remove_if(sc.adverbs.begin(), sc.adverbs.end(),
                                          [&](const realise::LaAdverb& a) { return a.lemma == fort; }),
                           sc.adverbs.end());
        }
        // C15: "I am anxious to get back" (a state verb: cupiō) and "It is better for people to keep away" (an
        // impersonal predicate) take the infinitive, not ut
        if (sb.marker == "to" && !sc.hasSubject &&
            (stateVerb || (f.copula && !f.predAdj.empty() && (!f.hasSubject || (f.subject.isPronoun && f.subject.pronLemma == "it"))))) {
          ls.rel = realise::SubRel::AccInf;
          break;
        }
        ls.rel = realise::SubRel::Purpose;
        // C15: the subject of a subject-less purpose clause: the object of help / ask / tell ("help me find" ->
        // ut inveniam), else the main subject ("I came to see you" -> ut tē videam)
        if (!sc.hasSubject && !sc.pred.person) {
          const LaNP* ctl = sb.marker == "to-obj" && cl.hasObject ? &cl.object : cl.hasSubject ? &cl.subject : nullptr;
          if (ctl && ctl->isPronoun) {
            sc.pred.person = ctl->pron.person;
            sc.pred.number = ctl->pron.number ? ctl->pron.number : ctl->number;
          } else if (ctl) {
            sc.pred.person = 3;
            sc.pred.number = ctl->number;
          } else if (f.type == Kind::Imp) {
            sc.pred.person = 2;
            sc.pred.number = cl.pred.number ? cl.pred.number : (uint8_t)Sg;
          }
        }
        break;
      case Relation::Result:
        ls.rel = realise::SubRel::Result;
        resultSequence();
        break;
      case Relation::Concession: ls.rel = realise::SubRel::Concession; break;
      case Relation::Complement:
        ls.rel = sb.frame[0].type == Kind::Wh || whatLike(sb.frame[0]) >= 0 ? realise::SubRel::IndirectQ : realise::SubRel::AccInf;
        // C22: "I wish (that) it was always summer", "I keep wishing it could be so": a wish is optō ut + subjunctive
        // (imperfect for an unreal wish), never an accusative + infinitive ("Volō semper aestātem fuisse" was OK)
        if (c.st.lang == frame::SrcLang::En && f.hasPred && f.pred.lemma == "wish" && sf.type == Kind::Decl && sf.hasSubject &&
            latin("optō", Verb) != kNone && (sb.marker.empty() || sb.marker == "that")) {
          cl.pred.lemma = latin("optō", Verb);
          for (Choice& x : c.out.choices)
            if (x.token == f.pred.token) { x.lemma = cl.pred.lemma; x.kind = "table"; x.note = "wish + clause: optō ut"; }
          ls.rel = realise::SubRel::Purpose;
          ls.conj = latin("ut", Conj);
          sc.pred.mood = Subjunctive;
          // C24: the sequence of tenses follows the verb of wishing: present optō -> present subjunctive ("Optō ut
          // volāre possim", "optō ut ita sit"), a past one (optābam, optāvī) -> imperfect ("Optābat ut venīret")
          const bool mainPast = cl.pred.tense == Perfect || cl.pred.tense == Imperfect || cl.pred.tense == Pluperfect ||
                                f.pred.tense == frame::Tense::Past;
          sc.pred.tense = mainPast ? (uint8_t)Imperfect : (uint8_t)Present;
          break;
        }
        // C15: "so tired that they cannot walk" -> tam fessī ut ambulāre nōn possint (result: ut + subjunctive)
        if (sb.marker == "that" && sb.frame[0].type != Kind::Wh) {
          bool so = false;
          for (const frame::SemAdj& a : f.predAdj)
            for (const std::string& v : a.adverbs) so = so || v == "so" || v == "such";
          for (const frame::SemAdverb& a : f.adverbs)
            so = so || a.lemma == "so" || a.lemma == "such" || a.lemma == "so badly" ||
                 (c.st.lang == frame::SrcLang::En && (a.lemma.compare(0, 3, "so ") == 0 || a.lemma.compare(0, 5, "such ") == 0));   // C20: "so fast"
          // C20: "such a big dog that ...", "so many apples that ...", "so hungry that ..." (a state verb)
          auto suchNP = [](const SemNP& x) { return x.determiner == "such" || x.determiner == "so many" || x.determiner == "so much"; };
          so = so || (c.st.lang == frame::SrcLang::En && ((f.hasSubject && suchNP(f.subject)) || (f.hasObject && suchNP(f.object)) ||
                                                          (!f.predicative.empty() && suchNP(f.predicative[0]))));
          if (so) { ls.rel = realise::SubRel::Result; resultSequence(); break; }
        }
        // C15: "whether" -> num + subjunctive (indirect question)
        if (sb.marker == "whether") {
          ls.rel = realise::SubRel::IndirectQ;
          ls.conj = latin("num", Adv);
          if (ls.conj == kNone) ls.conj = latin("num");
          sc.pred.mood = Subjunctive;
          break;
        }
        // C17: "tell you how to use them" -> quōmodo eīs ūtāris: the person who is to act is the one told (indirect
        // object / object), else the subject
        if (sb.marker == "towh" && !sc.hasSubject) {
          const LaNP* ctl = cl.hasIndirect ? &cl.indirect : cl.hasObject ? &cl.object : cl.hasSubject ? &cl.subject : nullptr;
          if (ctl && ctl->isPronoun) {
            sc.pred.person = ctl->pron.person;
            sc.pred.number = ctl->pron.number ? ctl->pron.number : ctl->number;
          } else if (ctl) {
            sc.pred.person = 3;
            sc.pred.number = ctl->number;
          } else if (f.type == Kind::Imp) {
            sc.pred.person = 2;
          } else {
            sc.pred.person = c.subjPerson;
          }
        }
        // C15: an indirect question is in the subjunctive, tenses by the sequence of tenses ("Vidē quid fēcerīs!",
        // "mē rogāvit quālis essēs")
        if (ls.rel == realise::SubRel::IndirectQ && c.st.lang == frame::SrcLang::En && sc.pred.mood == Indicative) {
          const bool mainPast = cl.pred.tense == Perfect || cl.pred.tense == Imperfect || cl.pred.tense == Pluperfect;
          sc.pred.mood = Subjunctive;
          if (mainPast) sc.pred.tense = sc.pred.tense == Pluperfect || sc.pred.tense == Perfect ?
                                        (sf.pred.aspect == frame::Aspect::Perfect ? (uint8_t)Pluperfect : (uint8_t)Imperfect)
                                        : sc.pred.tense == Future ? (uint8_t)Imperfect : (uint8_t)Imperfect;
          else if (sc.pred.tense == Imperfect || sc.pred.tense == Pluperfect) sc.pred.tense = Perfect;
          else if (sc.pred.tense == Future) sc.pred.tense = Present;
        }
        // C17: "he decided he would admit you" / "he said he would come" -> future infinitive (admissūrum esse)
        if (ls.rel == realise::SubRel::AccInf && c.st.lang == frame::SrcLang::En && sc.pred.modal == kNone &&
            (sf.pred.mood == frame::SrcMood::Conditional || sf.pred.tense == frame::Tense::Future ||
             sf.pred.modality == Modality::Will)) {
          sc.pred.tense = Future;
          sc.pred.mood = Indicative;
        }
        if (ls.rel == realise::SubRel::AccInf && sc.hasSubject && sc.subject.isPronoun &&
            sc.subject.pron.person == c.subjPerson && c.subjPerson == 3 && f.hasSubject && f.subject.isPronoun &&
            tables::personalPronoun(f.subject.pronLemma) && sc.subject.pron.number == cl.subject.pron.number)
          sc.subject.pron.reflexive = true;
        // C17: "The girls said they would come." -> sē ventūrās esse: a 3rd-person pronoun subject of the same number
        // as a noun subject of the verb of saying is that subject (reflexive, its gender)
        if (ls.rel == realise::SubRel::AccInf && sc.hasSubject && sc.subject.isPronoun && !sc.subject.pron.reflexive &&
            sc.subject.pron.person == 3 && f.hasSubject && !f.subject.isPronoun && cl.hasSubject && !cl.subject.isPronoun &&
            cl.subject.coord.empty() && (sc.subject.pron.number == Pl) == (cl.subject.number == Pl) &&
            sf.hasSubject && (sf.subject.pronLemma == "they" || sf.subject.pronLemma == "he" || sf.subject.pronLemma == "she")) {
          const uint8_t g = cl.subject.gender ? cl.subject.gender : cl.subject.head != kNone ? simpleGender(la_.lemma(cl.subject.head).gender) : 0;
          const uint8_t pg = sf.subject.pronLemma == "he" ? (uint8_t)M : sf.subject.pronLemma == "she" ? (uint8_t)F : g;
          if (g && pg == g) {
            sc.subject.pron.reflexive = true;
            sc.subject.pron.gender = g;
            sc.subject.gender = g;
          }
        }
        // C20: "He said that the girl had followed him." -> Dīxit puellam sē secūtam esse: a 3rd-person object pronoun of
        // a reported statement that matches the speaker (person, number, gender) is the reflexive
        if (ls.rel == realise::SubRel::AccInf && c.st.lang == frame::SrcLang::En && sc.hasObject && sc.object.isPronoun &&
            !sc.object.pron.reflexive && sc.object.pron.person == 3 && sf.hasObject && f.hasSubject && cl.hasSubject &&
            c.subjPerson == 3 && cl.subject.coord.empty() &&
            (sf.object.pronLemma == "him" || sf.object.pronLemma == "her" || sf.object.pronLemma == "them") &&
            (sf.object.pronLemma == "them") == (c.subjNumber == 2) &&
            (sf.object.pronLemma == "them" || c.subjGender == (sf.object.pronLemma == "him" ? (uint8_t)M : (uint8_t)F)) &&
            !(sc.hasSubject && sc.subject.isPronoun && sc.subject.pron.reflexive)) {
          sc.object.pron.reflexive = true;
        }
        if (ls.rel == realise::SubRel::AccInf && sc.hasSubject && sc.subject.isPronoun) sc.subject.emphasis = true;
        // "I thought it was Monday" -> Putābam diem Lūnae esse: an "it" subject with a predicate noun is dropped
        if (ls.rel == realise::SubRel::AccInf && sc.hasSubject && sc.subject.isPronoun && sf.hasSubject &&
            (sf.subject.pronLemma == "it" || (sf.implicitSubject && sf.subject.pron.person == 3)) &&
            !sc.predicative.empty())
          sc.hasSubject = false;
        if (ls.rel == realise::SubRel::AccInf && !sc.hasSubject && sc.predicative.empty() && sb.marker != "inf") {   // implicit "it"
          sc.hasSubject = true;
          sc.subject.isPronoun = true;
          sc.subject.pron.person = 3;
          sc.subject.pron.gender = N;
          sc.subject.gender = N;
        }
        break;
      case Relation::Coord: {
        ls.rel = realise::SubRel::Coord;
        const char* k = sb.marker == "but" ? "sed" : sb.marker == "or" ? "aut" : sb.marker == "for" ? "nam"
                      : sb.marker == "so" ? "itaque" : sb.marker == "yet" ? "tamen" : sb.marker == "nor" ? "neque" : "et";
        // C15: a coordinated verb without its own subject has the first clause's person ("curris et salīs")
        if (!sc.hasSubject && sc.type != realise::ClauseType::Imp && cl.hasSubject && !sc.pred.person) {
          if (cl.subject.isPronoun) {
            sc.pred.person = cl.subject.pron.person;
            sc.pred.number = cl.subject.pron.number ? cl.subject.pron.number : cl.subject.number;
          } else {
            sc.pred.person = 3;
            sc.pred.number = cl.subject.coord.empty() ? cl.subject.number : (uint8_t)Pl;
          }
        }
        if (sb.marker == "nor") sc.polarity = realise::Polarity::Pos;
        // C19: "I'll chop it down, and then we can get ..." -> poterimus: "can" after a future clause is future
        if (c.st.lang == frame::SrcLang::En && cl.pred.tense == Future && sc.pred.modal != kNone &&
            sc.pred.modal == latin("possum", Verb) && sc.pred.tense == Present && sf.pred.modality == Modality::Can &&
            !sf.pred.pastModal)
          sc.pred.tense = Future;
        ls.conj = latin(k, Conj);
        // C17: ", so we walked ..." / ", but she ...": the clause's own connector joins it (itaque, not "et itaque")
        if (sb.marker.empty() && !sc.connectors.empty() && c.st.lang == frame::SrcLang::En) {
          const uint32_t first = sc.connectors[0];
          const std::string fk = text::latin_key(std::string(la_.lemma(first).head));
          if (fk == "itaque" || fk == "sed" || fk == "nam" || fk == "tamen" || fk == "aut" || fk == "et") {
            ls.conj = first;
            sc.connectors.erase(sc.connectors.begin());
          }
        }
        // ", or you would not ..." = otherwise: "; aliter ..." with the conditional's subjunctive
        // C24: also after a command or a "must" ("We must run, or the bus will leave." -> ...; aliter ... exībit)
        if (sb.marker == "or" && (sf.pred.mood == frame::SrcMood::Conditional ||
                                  (sf.pred.tense == frame::Tense::Future &&
                                   (f.type == Kind::Imp || f.pred.modality == Modality::Must || f.pred.modality == Modality::Should)))) {
          const uint32_t aliter = latin("aliter", Adv);
          if (aliter != kNone) {
            ls.conj = aliter;
            const uint32_t aut = latin("aut");
            sc.connectors.erase(std::remove(sc.connectors.begin(), sc.connectors.end(), aut), sc.connectors.end());
            ls.sep = ";";
          }
        }
        if (f.type == Kind::Imp && sc.type == realise::ClauseType::Decl && !sc.hasSubject) sc.type = realise::ClauseType::Imp;
        if (sc.type == realise::ClauseType::Imp && cl.pred.number) sc.pred.number = cl.pred.number;
        break;
      }
      default: ls.rel = realise::SubRel::Coord; break;
    }
    ls.clause.push_back(std::move(sc));
    cl.subs.push_back(std::move(ls));
  }
  // clause-level negative words from NPs ("no", "nobody", "nothing", "never")
  if (c.negative && !f.negative) cl.polarity = realise::Polarity::Neg;
  if (c.negConsumed) { cl.polarity = realise::Polarity::Pos; c.negConsumed = false; }
  // C15: "not know" -> nesciō ("Nescīs?", "potestāte suā ūtī nescit") unless a negative word carries the negation
  if (cl.polarity == realise::Polarity::Neg && cl.type != realise::ClauseType::Imp) {
    const uint32_t scio = latin("sciō", Verb), nescio = latin("nesciō", Verb);
    bool negWord = false;
    auto negNP = [&](const LaNP& x) {
      if (x.head != kNone && (x.head == latin("nēmō", Pron) || x.head == latin("nihil", Pron))) negWord = true;
      for (const LaAdj& a : x.adjectives) if (a.lemma == latin("nūllus", Adj)) negWord = true;
    };
    if (cl.hasSubject) negNP(cl.subject);
    if (cl.hasObject) negNP(cl.object);
    for (const realise::LaAdverb& a : cl.adverbs)
      if (a.lemma == latin("numquam", Adv) || a.lemma == latin("nusquam", Adv)) negWord = true;
    if (!negWord && scio != kNone && nescio != kNone) {
      if (cl.pred.modal == scio) { cl.pred.modal = nescio; cl.polarity = realise::Polarity::Pos; }
      else if (cl.pred.modal == kNone && cl.pred.lemma == scio) { cl.pred.lemma = nescio; cl.polarity = realise::Polarity::Pos; }
    }
  }
  c.frame = keepFrame;
  c.motion = keepMotion;
  c.negative = keepNeg;
  c.subjPerson = keepPerson;
  c.subjNumber = keepNumber;
  c.subjGender = keepGender;
  c.objectInVerb = keepObjVerb;
  c.routeObject = keepRoute;
}

void Transfer::clause(const SemFrame& f0, const SemSentence& s, const Settings& st, Memory& mem, ClauseOut& out) const {
  out.clear();
  Ctx c(s, st, mem, out);
  // C19: "The shepherd, seeing the wolf, fled.": the parser takes the -ing word (after a comma, no auxiliary) for the
  // main verb and hangs the real verb on it as a clause without a subject. That clause is the main one; the -ing phrase
  // is a participle agreeing with the subject, placed right after it ("Pāstor, lupum vidēns, fūgit"). Rebuilt here on
  // the Latin side (the Greek transfer rebuilds the same frame its own way), flagged participle-phrase (Check).
  SemFrame rebuilt;
  bool rb = false;
  if (st.lang == frame::SrcLang::En && f0.type == Kind::Decl && f0.hasSubject && f0.hasPred && f0.pred.token > 0 &&
      (size_t)f0.pred.token < s.tokens.size() && f0.pred.auxTokens.empty() && f0.pred.aspect == frame::Aspect::Simple &&
      !f0.subordinate.empty() && s.tokens[(size_t)f0.pred.token - 1].text == ",") {
    const std::string& w = s.tokens[(size_t)f0.pred.token].lower;
    const frame::SemSub& last = f0.subordinate.back();
    if (w.size() > 4 && w.compare(w.size() - 3, 3, "ing") == 0 && f0.subject.token >= 0 && f0.subject.token < f0.pred.token &&
        (last.relation == Relation::Time || last.relation == Relation::Coord) && !last.frame.empty() &&
        !last.frame[0].hasSubject && last.frame[0].type == Kind::Decl && last.frame[0].hasPred) {
      rebuilt = last.frame[0];
      rebuilt.hasSubject = true;
      rebuilt.subject = f0.subject;
      for (const std::string& k : f0.connectors) rebuilt.connectors.push_back(k);
      SemFrame sf;
      sf.type = Kind::Frag;
      frame::SemAdj pa;
      pa.lemma = f0.pred.lemma;
      pa.token = f0.pred.token;
      pa.participle = 2;
      sf.predAdj.push_back(pa);
      sf.hasObject = f0.hasObject;
      sf.object = f0.object;
      sf.obliques = f0.obliques;
      sf.tokens.push_back(f0.pred.token - 1);
      sf.tokens.push_back(f0.pred.token);
      rebuilt.secondary.insert(rebuilt.secondary.begin(), sf);
      for (const SemFrame& x : f0.secondary) rebuilt.secondary.push_back(x);
      rb = true;
      out.flags.push_back("participle-phrase");
    }
  }
  // C20: "so many friends that she is never alone", "such a big dog that everybody ran away": the "that" clause hung on
  // the noun is the result clause of "such / so many" (tam ... ut), not a relative clause ("quōs ... habet" was OK)
  if (st.lang == frame::SrcLang::En) {
    auto soNP = [&](const frame::SemNP& x) {
      if (x.relative.empty() || x.isPronoun) return false;
      bool so = x.determiner == "such";
      if (x.determiner == "many" || x.determiner == "much")
        for (int k : x.tokens)
          if (k > 0 && (size_t)k < s.tokens.size() && s.tokens[(size_t)k].lower == x.determiner && s.tokens[(size_t)k - 1].lower == "so")
            so = true;
      if (!so) return false;
      const SemFrame& rf = x.relative[0];
      int first = -1;
      for (int t : rf.tokens)
        if (t >= 0 && (size_t)t < s.tokens.size() && s.tokens[(size_t)t].upos != "PUNCT" && (first < 0 || t < first)) first = t;
      if (first < 0 || s.tokens[(size_t)first].lower != "that") return false;
      // the relative word must not be the clause's subject ("so many friends that came" stays a relative clause)
      return rf.hasSubject && !(rf.subject.isPronoun && rf.subject.token == first);
    };
    SemFrame& g = rb ? rebuilt : (rebuilt = f0);
    frame::SemNP* hit = (g.hasSubject && soNP(g.subject)) ? &g.subject : (g.hasObject && soNP(g.object)) ? &g.object
                      : (!g.predicative.empty() && soNP(g.predicative[0])) ? &g.predicative[0] : nullptr;
    if (hit) {
      frame::SemSub sub;
      sub.relation = Relation::Result;
      sub.marker = "that";
      sub.frame.push_back(hit->relative[0]);
      hit->relative.clear();
      SemFrame& rf = sub.frame[0];
      if (rf.hasObject && rf.object.isPronoun && (rf.object.pronLemma == "that" || rf.object.pronLemma == "which")) {
        out.covered.push_back(rf.object.token);
        rf.hasObject = false;
        rf.object = frame::SemNP{};
      }
      g.subordinate.push_back(std::move(sub));
      rb = true;
    }
  }
  // C22: "think nothing of X / of doing X" -> X nihil cūrāre (care nothing for it), not "nihil putō" with X lost
  if (st.lang == frame::SrcLang::En) {
    SemFrame& g = rb ? rebuilt : (rebuilt = f0);
    if (g.hasPred && g.pred.lemma == "think" && g.hasObject && g.object.isPronoun && g.object.pronLemma == "nothing" &&
        (!g.object.genitive.empty() || !g.object.relative.empty())) {
      frame::SemAdverb nil;
      nil.lemma = "nothing-adv";
      nil.token = g.object.token;
      SemNP obj = g.object;
      g.pred.lemma = "care";
      if (!obj.genitive.empty()) {
        g.object = obj.genitive[0];
      } else {
        frame::SemSub sub;
        sub.relation = Relation::Complement;
        sub.marker = "to";
        sub.frame.push_back(obj.relative[0]);
        sub.frame[0].type = Kind::Decl;
        g.hasObject = false;
        g.object = SemNP{};
        g.subordinate.push_back(std::move(sub));
      }
      g.adverbs.push_back(nil);
      for (int t : obj.tokens)
        if (t >= 0 && (size_t)t < s.tokens.size() && (s.tokens[(size_t)t].lower == "of" || s.tokens[(size_t)t].lower == "nothing"))
          out.covered.push_back(t);
      rb = true;
    }
  }
  const SemFrame& f = rb ? rebuilt : f0;
  clauseInto(f, c, out.clause);
  // C19: a fragment "and / or + noun phrase" continuing the previous sentence of the same speaker takes the case the
  // noun had there ("I saw the king," + "and the queen." -> Et rēgīnam.)
  LaClause& oc = out.clause;
  if (f.type == frame::Kind::Frag && !f.hasPred && f.hasSubject && !f.subject.isPronoun && mem.contCase &&
      mem.contCase != Nom && oc.type == realise::ClauseType::Frag && oc.hasSubject && !oc.hasObject &&
      f.predAdj.empty() && f.obliques.empty() && !f.connectors.empty() &&
      (f.connectors[0] == "and" || f.connectors[0] == "or" || f.connectors[0] == "nor")) {
    oc.object = oc.subject;
    oc.object.case_ = mem.contCase;
    oc.hasObject = true;
    oc.hasSubject = false;
    oc.subject = realise::LaNP{};
  }
  // C22: a song line "And + bare verb" (no subject) continues the previous line's clause instead of being an order
  // ("And hear a song ..." after "I could listen to the river" -> et carmen audīre possem)
  if (st.lang == frame::SrcLang::En && mem.songLine && mem.prevSong && mem.prevValid &&
      (f.type == Kind::Imp || (f.type == Kind::Decl && f.hasPred && f.pred.tense == frame::Tense::Present &&
                               f.pred.auxTokens.size() <= 1)) &&
      !f.hasSubject && f.vocatives.empty() && !f.connectors.empty() &&
      (f.connectors[0] == "and" || f.connectors[0] == "or") &&
      (oc.type == realise::ClauseType::Imp || oc.type == realise::ClauseType::Decl)) {
    oc.type = realise::ClauseType::Decl;
    oc.pred.person = mem.prevPerson;
    oc.pred.number = mem.prevNumber;
    oc.pred.tense = mem.prevTense;
    oc.pred.mood = mem.prevMood;
    if (mem.prevModal != kNone && oc.pred.modal == kNone) {
      oc.pred.modal = mem.prevModal;
      oc.pred.infTense = Present;
      oc.pred.infVoice = oc.pred.voice;
      oc.pred.voice = Active;
    }
    out.flags.push_back("fragment");   // rebuilt from the line before: Check
  }
  // C22: what the next song line may continue
  if (oc.pred.lemma != kNone && (oc.type == realise::ClauseType::Decl || oc.type == realise::ClauseType::Yn ||
                                 oc.type == realise::ClauseType::Wh)) {
    uint8_t pp = oc.pred.person, pn = oc.pred.number;
    if (!pp) {
      if (oc.hasSubject && oc.subject.isPronoun) { pp = oc.subject.pron.person ? oc.subject.pron.person : 3; pn = oc.subject.pron.number; }
      else { pp = 3; pn = oc.hasSubject ? (oc.subject.coord.empty() ? oc.subject.number : (uint8_t)Pl) : (uint8_t)Sg; }
    }
    mem.prevPerson = pp;
    mem.prevNumber = pn ? pn : (uint8_t)Sg;
    mem.prevTense = oc.pred.tense;
    mem.prevMood = oc.pred.mood;
    mem.prevModal = oc.pred.modal;
    mem.prevValid = true;
    mem.prevSong = mem.songLine;
  }
  // C19: permission (licet): the person allowed is in the dative ("Licetne mihi exīre?")
  if (oc.pred.modal != kNone && oc.pred.modal == latin("licet", Verb) && oc.hasSubject && oc.subject.isPronoun &&
      oc.subject.pron.person == 1 && !oc.hasIndirect) {
    oc.indirect = oc.subject;
    oc.indirect.case_ = Dat;
    oc.hasIndirect = true;
    oc.hasSubject = false;
    oc.subject = realise::LaNP{};
  }
  mem.lastObjCase = 0;
  if (oc.hasObject && oc.type != realise::ClauseType::Frag && oc.pred.lemma != kNone) {
    uint8_t cs = oc.object.case_ ? oc.object.case_ : (uint8_t)Acc;
    const lex::Lemma vl = la_.lemma(oc.pred.lemma);
    if (!oc.object.case_ && vl.id != kNone)
      if (const curated::Valency* v = cd_.valency(vl.key))
        for (const curated::Frame& fr : v->frames) {
          if (fr.kind == curated::FrameKind::Dat) { cs = Dat; break; }
          if (fr.kind == curated::FrameKind::Abl) { cs = Abl; break; }
          if (fr.kind == curated::FrameKind::Gen) { cs = Gen; break; }
          if (fr.kind == curated::FrameKind::Acc || fr.kind == curated::FrameKind::DatAcc) break;
        }
    mem.lastObjCase = cs;
  }
  if (mem.addresseeGuess) out.flags.push_back("addressee-guess");
  std::sort(out.covered.begin(), out.covered.end());
  out.covered.erase(std::unique(out.covered.begin(), out.covered.end()), out.covered.end());
}

// "child" in address: puella / puer by the gender of the project's main character (Options.speakerGender: the person
// speaking or spoken to in most dialogue), else puer; unknown gender -> puer and Check. C20: "my child" is the parent's
// address, mī fīlī / mea fīlia (the possessive first); "my" was dropped silently before.
namespace {
bool childAddress(const Transfer& t, const SemNP& n, const Settings& st, LaNP& x, ClauseOut& out, uint8_t addressee) {
  const std::string head = text::lower(n.head);
  if (!((head == "child" || head == "kid") && n.number != 2 && n.adjectives.empty() && st.lang == frame::SrcLang::En))
    return false;
  bool girl = st.speakerGender == 'f';
  if (addressee) girl = addressee == F;   // C22: a name in the cue tells who is addressed
  bool my = false;
  for (const SemNP& p : n.possessor) my = my || (p.isPronoun && p.pron.person == 1 && p.pron.number == 1);
  if (!n.possessor.empty() && !my) return false;   // "your child": the ordinary noun phrase
  const uint32_t h = t.latin(my ? (girl ? "fīlia" : "fīlius") : (girl ? "puella" : "puer"), Noun);
  uint32_t poss = kNone;
  if (my) {
    poss = t.latin("meus", Det);
    if (poss == kNone) poss = t.latin("meus", Adj);
    if (poss == kNone) return false;
  }
  if (h == kNone) return false;
  x.head = h;
  x.possessive = poss;
  x.possContrast = my;   // mī fīlī: the possessive first
  Choice ch;
  ch.token = n.token;
  ch.source = n.head;
  ch.lemma = x.head;
  ch.kind = "table";
  ch.note = my ? "my child in address: mī fīlī / mea fīlia by the main character's gender"
               : "child in address: by the main character's gender";
  out.choices.push_back(ch);
  out.covered.insert(out.covered.end(), n.tokens.begin(), n.tokens.end());
  if (st.speakerGender == 'u') out.flags.push_back("speaker-gender");
  return true;
}
}  // namespace

void Transfer::vocative(const SemNP& n, const SemSentence& s, const Settings& st, Memory& mem, ClauseOut& out) const {
  out.clear();
  Ctx c(s, st, mem, out);
  out.clause.type = realise::ClauseType::Frag;
  LaNP x;
  if (!childAddress(*this, n, st, x, out, mem.addresseeGender)) x = LaNP{};
  if (x.head == kNone) {
    npInto(n, c, x);
    addresseeNoun(*this, n, st, mem, x);
  }
  x.case_ = Voc;
  out.clause.hasObject = true;   // the fragment path realises an object NP in its own case
  out.clause.object = x;
  if (x.number == Pl) mem.sawPlural = true;
  std::sort(out.covered.begin(), out.covered.end());
  out.covered.erase(std::unique(out.covered.begin(), out.covered.end()), out.covered.end());
}

namespace {
void addresseeNoun(const Transfer& t, const SemNP& n, const Settings& st, const Memory& mem, LaNP& x) {
  const std::string head = text::lower(n.head);
  if (st.lang != frame::SrcLang::En || n.number == 2 || !(head == "child" || head == "kid") || x.head == kNone) return;
  const bool girl = mem.addresseeGender ? mem.addresseeGender == F : st.speakerGender == 'f';
  const uint32_t puer = t.latin("puer", Noun), puella = t.latin("puella", Noun);
  if (girl && x.head == puer && puella != kNone) x.head = puella;
  else if (!girl && x.head == puella && puer != kNone) x.head = puer;
}
}  // namespace

}  // namespace vp::transfer
