// Transfer: SemFrame -> LaClause (DESIGN.md §10.2-10.3). Lexical selection through REVX with the §10.2 scoring,
// closed classes by the tables in tables.cpp, prepositions from preps_en_la.tsv, valency through the realiser's
// CaseAssigner (valency_la.tsv), periphrasis_la.tsv for fidelity 2/3, names through the glossary / names_la.tsv.
#include <algorithm>
#include <cmath>
#include <cstdlib>

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
  struct Scored { uint32_t lemma; uint16_t sense; double score; std::string why; uint8_t tier; bool kwHit; double base; };
  std::vector<Scored> sc;
  std::vector<lex::Sense> senses;
  // the best reverse-index score of a compatible lemma: fidelity 3 prefers low tiers only among exact senses
  double topBase = 0;
  for (const lex::Candidate& k : raw) {
    const lex::Lemma l = la_.lemma(k.lemma);
    if (l.id != kNone && posCompatible(l.pos, pos)) topBase = std::max(topBase, k.score / 255.0);
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
    double s = k.score / 255.0;
    std::string why = "base " + std::to_string(k.score);
    // tier term (fidelity 1: none; 2: -0.25 per tier above 2; 3: -0.5 per tier above 1); T1 preference. The curated
    // tier (key + part of speech) wins over the lexicon's own.
    const uint8_t tier = cd_.effectiveTier(l.key, l.pos, l.tier);
    s += tierTerm(tier, why, isTaught(l) ? 1.0 : k.score / 255.0);
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
    sc.push_back(Scored{k.lemma, k.sense, s, why, tier, kwHit || isTaught(l), k.score / 255.0});
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
    sc.push_back(Scored{id, 0, s, why, tier, true, 0.5});
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
    sc.push_back(Scored{id, 0, s, why, tier, true, 0.5});
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
    std::vector<char> gone(sc.size(), 0);
    for (size_t i = 0; i < sc.size(); ++i) {
      if (gone[i]) continue;
      const std::string si = sig(sc[i].lemma);
      size_t win = i;
      for (size_t j = i + 1; j < sc.size(); ++j) {
        if (gone[j] || sig(sc[j].lemma) != si) continue;
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
  return select(a.lemma, Adj, c.context, false, false, c.st, ch);
}

// ---- adverbs ---------------------------------------------------------------------------------------------------------
uint32_t Transfer::adverb(const std::string& lemma0, int token, Ctx& c, bool motion) const {
  const std::string lemma = c.st.lang == frame::SrcLang::Es ? tables::spanishAdverb(lemma0) : lemma0;
  Choice ch;
  ch.token = token;
  ch.source = lemma0;
  if (const char* la = tables::adverb(lemma, motion)) {
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
    bool gen = false;
    for (const lex::Analysis& a : mt.analyses) {
      const lex::Lemma l = la_.lemma(a.lemma);
      const Features f = unpack(la_.feature(a.feat));
      if (l.pos == Noun && f.case_ == Nom && noun == kNone && !(l.flags & lex::ProperName)) { noun = a.lemma; num = f.number ? f.number : (uint8_t)Sg; }
      else if ((l.pos == Adj || l.pos == Participle) && f.case_ == Nom && adj == kNone) adj = a.lemma;
      if (f.case_ == Gen) gen = true;
    }
    if (head < 0 && noun != kNone) {
      head = (int)i;
      o.head = noun;
      o.number = num;
      continue;
    }
    if (adj != kNone && !gen) { parts.push_back({(int)i, Part{1, adj, ws[i]}}); continue; }
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
        o.pron.gender = M;
      }
      o.gender = o.pron.gender;
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
        const uint32_t fn = select(cmpFirst, Noun, c.context, false, false, c.st, c3);
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
        if (c.frame && c.frame->hasSubject && c.subjPerson == 3 && c.frame->subject.token != n.token &&
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
    la.lemma = adjectiveInto(a, c, la, ch);
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
    npInto(g, c, x);
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
  const bool time = tables::timeNoun(head) || tables::weekday(head) != nullptr;
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
    npInto(n, c, o.np);
    o.np.case_ = Nom;
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
  if (prep == "of" || prep == "about") return withPrep("dē", Abl);
  if (prep == "by") {
    if (c.frame && c.frame->pred.voice == frame::Voice::Passive) return person ? withPrep("ab", Abl) : bare(Abl);
    if (!person) return bare(Abl);
    return withPrep("apud", Acc);
  }
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
             (verb = latin([&] {   // C15: "arceō|absum": with an object the first verb, without one the second
                       const size_t bar = ph->latin.find('|');
                       if (bar == std::string::npos) return ph->latin;
                       return hasObj ? ph->latin.substr(0, bar) : ph->latin.substr(bar + 1);
                     }().c_str(), Verb)) != kNone) {
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
      realise::LaAdverb a;
      a.lemma = latin("fortasse", Adv);
      if (a.lemma != kNone) cl.adverbs.push_back(a);
      break;
    }
    case Modality::Let:
      p.mood = Subjunctive;
      p.person = 1;
      p.number = Pl;
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
  const bool lightVerb = c.st.lang == frame::SrcLang::En && sne && sne->kind == "verb" && sp.lemma != "be" && sp.lemma != "feel";
  const bool state = (!lightVerb && tables::stateVerb(la_.lemma(p.modal != kNone ? p.modal : p.lemma).key)) || sp.habitual ||
                     cl.pred.lemma == latin("sum", Verb);
  uint8_t tense = Present;
  if (sp.tense == frame::Tense::Future) tense = Future;
  else if (sp.tense == frame::Tense::Past) {
    if (sp.aspect == frame::Aspect::Perfect) tense = Pluperfect;
    else if (sp.aspect == frame::Aspect::Progressive || state || sp.pastModal) tense = Imperfect;
    else tense = Perfect;
  } else {
    if (sp.aspect == frame::Aspect::Perfect) tense = Perfect;
    else if (sp.pastModal) tense = Imperfect;
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
  }
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
      f.object.determiner == "no" && cl.object.adjectives.size() == 1 && cl.object.adjectives[0].lemma == latin("nūllus", Adj)) {
    cl.object.adjectives.clear();
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
    if (!obliqueInto(o, c, cl)) {
      // a place adverb inside a PP ("in here")
      c.cover(o.np.tokens);
    }
  }
  // copula predicate
  if ((f.copula || (f.hasPred && c.st.lang == frame::SrcLang::En && (!f.predicative.empty() || !f.predAdj.empty()))) && !stateVerb) {   // C17: become + predicate
    for (const SemNP& pn : f.predicative) {
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
      if (stateAdj && &a == &f.predAdj[0] && (la.lemma = latin(stateAdj->latin.c_str(), Adj)) != kNone) {
        ch.source = a.lemma;
        ch.lemma = la.lemma;
        ch.kind = "table";
        ch.note = "states_en_la.tsv";
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
      cl.predAdj.push_back(la);
    }
    const bool itSubj = f.hasSubject && f.subject.isPronoun && (f.subject.pronLemma == "it" || f.subject.pronLemma == "ello");
    const bool impersAdj = f.predAdj.size() == 1 && tables::impersonalAdjective(f.predAdj[0].lemma);
    if (!cl.predAdj.empty() && (!f.hasSubject || f.pred.impersonal || (itSubj && (impersAdj || !c.mem.lastGender)))) {
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
    for (const frame::SemOblique& o : sf.obliques) {
      if (sc.hasObject && (o.prep == "of" || o.prep == "about")) continue;
      if (!obliqueInto(o, c, sc)) c.cover(o.np.tokens);
    }
    realise::LaSub sub;
    sub.rel = realise::SubRel::Apposition;
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
  // exclamation "what a strange garden": the NP is the subject
  // vocatives, interjections, adverbs, discourse, connectors
  for (const SemNP& v : f.vocatives) {
    LaNP x;
    npInto(v, c, x);
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
    if (a.lemma == "only" && c.st.lang == frame::SrcLang::En && f.copula && cl.predicative.size() == 1) {
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
    else if ((w == "who" || w == "what") &&
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
    realise::LaSub ls;
    ls.before = sb.before;
    LaClause sc;
    const bool keepSi = c.afterSi;
    c.afterSi = sb.relation == Relation::Condition;
    clauseInto(sb.frame[0], c, sc);
    c.afterSi = keepSi;
    c.frame = &f;
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
        else if (sb.marker == "while" || sb.marker == "until") ls.conj = latin("dum", Conj);
        break;
      case Relation::Condition:
        ls.rel = realise::SubRel::Condition;
        if (sb.marker == "unless") ls.conj = latin("nisi", Conj);
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
        break;
      case Relation::Concession: ls.rel = realise::SubRel::Concession; break;
      case Relation::Complement:
        ls.rel = sb.frame[0].type == Kind::Wh || whatLike(sb.frame[0]) >= 0 ? realise::SubRel::IndirectQ : realise::SubRel::AccInf;
        // C15: "so tired that they cannot walk" -> tam fessī ut ambulāre nōn possint (result: ut + subjunctive)
        if (sb.marker == "that" && sb.frame[0].type != Kind::Wh) {
          bool so = false;
          for (const frame::SemAdj& a : f.predAdj)
            for (const std::string& v : a.adverbs) so = so || v == "so" || v == "such";
          for (const frame::SemAdverb& a : f.adverbs) so = so || a.lemma == "so" || a.lemma == "such" || a.lemma == "so badly";
          if (so) { ls.rel = realise::SubRel::Result; break; }
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
        if (sb.marker == "or" && sf.pred.mood == frame::SrcMood::Conditional) {
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

void Transfer::clause(const SemFrame& f, const SemSentence& s, const Settings& st, Memory& mem, ClauseOut& out) const {
  out.clear();
  Ctx c(s, st, mem, out);
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

void Transfer::vocative(const SemNP& n, const SemSentence& s, const Settings& st, Memory& mem, ClauseOut& out) const {
  out.clear();
  Ctx c(s, st, mem, out);
  out.clause.type = realise::ClauseType::Frag;
  LaNP x;
  const std::string head = text::lower(n.head);
  if ((head == "child" || head == "kid") && n.number != 2 && n.adjectives.empty() && st.lang == frame::SrcLang::En) {
    // "child" in address: puella / puer by the gender of the project's main character (Options.speakerGender: the
    // person speaking or spoken to in most dialogue), else puer; unknown gender -> puer and Check
    const bool girl = st.speakerGender == 'f';
    x.head = latin(girl ? "puella" : "puer", Noun);
    Choice ch;
    ch.token = n.token;
    ch.source = n.head;
    ch.lemma = x.head;
    ch.kind = "table";
    ch.note = "child in address: by the main character's gender";
    out.choices.push_back(ch);
    out.covered.insert(out.covered.end(), n.tokens.begin(), n.tokens.end());
    if (st.speakerGender == 'u') out.flags.push_back("speaker-gender");
  }
  if (x.head == kNone) npInto(n, c, x);
  x.case_ = Voc;
  out.clause.hasObject = true;   // the fragment path realises an object NP in its own case
  out.clause.object = x;
  if (x.number == Pl) mem.sawPlural = true;
  std::sort(out.covered.begin(), out.covered.end());
  out.covered.erase(std::unique(out.covered.begin(), out.covered.end()), out.covered.end());
}

}  // namespace vp::transfer
