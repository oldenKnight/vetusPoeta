// Transfer: SemFrame -> LaClause (DESIGN.md §10.2-10.3). Lexical selection through REVX with the §10.2 scoring,
// closed classes by the tables in tables.cpp, prepositions from preps_en_la.tsv, valency through the realiser's
// CaseAssigner (valency_la.tsv), periphrasis_la.tsv for fidelity 2/3, names through the glossary / names_la.tsv.
#include <algorithm>
#include <cmath>

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
    case Adj: return latinPos == Adj || latinPos == Num || latinPos == Participle;
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

uint8_t simpleGender(uint8_t g) {
  switch (g) {
    case M: case F: case N: return g;
    case FN: return F;
    default: return M;
  }
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
        if ((p == "it" || p == "ello" || p == "lo") && c.mem.lastGender && c.frame && !c.frame->copula &&
            c.frame->pred.lemma != "be")
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
    else if (p == "someone" || p == "anyone") { id = latin("aliquis", Pron); gender = M; }
    else if (p == "something" || p == "anything") { id = latin("aliquis", Pron); gender = N; }
    else if (p == "who") { id = latin("quis", Pron); gender = M; }
    else if (p == "what") { id = latin("quis", Pron); gender = N; }
    else if (p == "which") { id = latin("quī", Pron); gender = 0; }
    else if (p == "many") { id = latin("multus", Adj); number = Pl; gender = M; }
    else if (p == "few") { id = latin("paucus", Adj); number = Pl; gender = M; }
    else if (p == "both") { id = latin("ambō"); number = Pl; gender = M; }
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
      if (det == "that") id = latin("ille", Det);
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
    const bool inTable = cd_.nameByEnglish(n.head) != nullptr;
    bool glossary = false;
    if (c.st.context)
      for (const rules::GlossaryEntry& g : c.st.context->glossary)
        if (g.name == n.head) glossary = true;
    if (!inTable && !glossary && (n.number == 2 || n.title)) {
      // a plural title word ("Queen of Hearts" -> Cordium): translate as a capitalised common noun
      Choice ch;
      ch.token = n.token;
      std::string sg = low;
      if (n.number == 2 && sg.size() > 3 && sg.back() == 's') sg.pop_back();
      o.head = select(sg, Noun, c.context, false, false, c.st, ch);
      c.out.choices.push_back(ch);
      if (o.head != kNone) { o.capitalise = true; return; }
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
        return;
      }
    }
    o.isName = true;
    o.name = n.head;
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
    return;
  }
  // common nouns
  {
    Choice ch;
    ch.token = n.token;
    uint32_t id = kNone;
    if (n.head == "hour" && n.ordinal) id = latin("hōra", Noun);
    // the Spanish noun's grammatical gender helps only for persons and animals (hijo -> fīlius, niña -> puella);
    // for things it would leak into the Latin choice (el gato -> fēlēs f, la puerta -> iānua) (C13)
    if (id == kNone) id = select(n.head, Noun, c.context, false, false, c.st, ch, animate(n) ? n.srcGender : 0);
    else { ch.source = n.head; ch.lemma = id; ch.kind = "table"; }
    if (id == kNone && !n.head.empty()) {   // substantive adjective ("the dark")
      Choice c2;
      c2.token = n.token;
      id = select(n.head, Adj, c.context, false, false, c.st, c2);
      if (id != kNone) { ch = c2; o.gender = N; }
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
      if (l.pos == Adj || l.pos == Participle) o.gender = N;   // a taught adjective as a noun: "in īmō"
      if (l.pos == Noun && !(l.flags & lex::PluralOnly) && pluralOnly(id)) o.number = Pl;   // tenebrae
      if (l.pos == Noun) {
        c.mem.lastGender = simpleGender(l.gender);
        c.mem.lastNumber = o.number;
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
  else if (d == "all") { addAdj("omnis", d); o.number = Pl; }
  else if (d == "many") { addAdj("multus", d); o.number = Pl; }
  else if (d == "few") { addAdj("paucus", d); o.number = Pl; }
  else if (d == "much") addAdj("multus", d);
  else if (d == "another" || d == "other") addAdj("alius", d);
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
        const uint8_t pg = p.pron.gender;
        if (c.frame && c.frame->hasSubject && c.subjPerson == 3 && c.frame->subject.token != n.token &&
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
      for (const curated::PhraseEntry& e : cd_.phrasebook())
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
    la.lemma = select(a.lemma, Adj, c.context, false, false, c.st, ch);
    c.out.choices.push_back(ch);
    c.cover(a.token);
    if (la.lemma == kNone) { c.out.unknownWords.push_back(a.lemma); continue; }
    for (size_t i = 0; i < a.adverbs.size(); ++i) {
      const uint32_t av = adverb(a.adverbs[i], i < a.advTokens.size() ? a.advTokens[i] : -1, c, false);
      if (av != kNone) la.adverbs.push_back(av);
    }
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
    }
  }
  // "of" attributes (one genitive)
  for (const SemNP& g : n.genitive) {
    LaNP x;
    npInto(g, c, x);
    o.genitive.push_back(x);
    break;
  }
  // relative clause
  for (const SemFrame& rf : n.relative) {
    LaClause rc;
    clauseInto(rf, c, rc);
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
  // coordination
  for (const SemNP& k : n.coord) {
    LaNP x;
    npInto(k, c, x);
    o.coord.push_back(x);
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
  if ((prep == "in") && n.isName) {
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
  if (prep == "to") {
    if (person && !c.motion) {
      if (!cl.hasIndirect) { cl.hasIndirect = true; npInto(n, c, cl.indirect); return true; }
      return bare(Dat);
    }
    return withPrep("ad", Acc);
  }
  if (prep == "for") {
    if (time) return bare(Acc);
    if (person && !cl.hasIndirect) { cl.hasIndirect = true; npInto(n, c, cl.indirect); return true; }
    return withPrep("prō", Abl);
  }
  if (prep == "with") return person ? withPrep("cum", Abl) : bare(Abl);
  if (prep == "at") {
    if (time) return bare(Abl);
    if (person) return withPrep("apud", Acc);
    return withPrep("in", Abl);
  }
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
  } else if (ph && sp.complementVerb.empty() && (verb = latin(ph->latin.c_str(), Verb)) != kNone) {
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
  } else {
    verb = choose(sp.lemma, sp.token, hasObj, personObj);
    if (!sp.particle.empty()) {
      realise::LaAdverb a;
      a.lemma = adverb(sp.particle, -1, c, c.motion);
      if (a.lemma != kNone) cl.adverbs.push_back(a);
    }
  }
  p.lemma = verb;
  // catenative complement ("know how to play" -> sciō + lūdere)
  if (!sp.complementVerb.empty()) {
    const uint32_t comp = choose(sp.complementVerb, sp.complementToken, hasObj, personObj);
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
  const bool state = tables::stateVerb(la_.lemma(p.modal != kNone ? p.modal : p.lemma).key) || sp.habitual ||
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
    if (tense == Present && !agent) tense = Perfect;   // "the clock is broken": resultant state
  }
  if (sp.deliberative) { tense = Present; p.mood = Subjunctive; }
  if (sp.mood == frame::SrcMood::Conditional) {
    if (c.st.fidelity >= 3) tense = Present;
    else { tense = tense == Perfect || tense == Pluperfect ? Pluperfect : Imperfect; p.mood = Subjunctive; }
  } else if (sp.mood == frame::SrcMood::Subjunctive) {
    p.mood = Subjunctive;
  }
  if (sp.tense == frame::Tense::Future && sp.modality == Modality::Can) tense = Future;
  p.tense = tense;
  if (p.modal != kNone) { p.infTense = Present; p.infVoice = p.voice; p.voice = Active; }
  if (p.lemma != kNone) {
    c.mem.lastVerb = p.lemma;
    c.mem.lastMotion = c.motion;
  }
}

// ---- clauses --------------------------------------------------------------------------------------------------------
void Transfer::clauseInto(const SemFrame& f, Ctx& c, LaClause& cl) const {
  const SemFrame* keepFrame = c.frame;
  const bool keepMotion = c.motion, keepNeg = c.negative;
  const uint8_t keepPerson = c.subjPerson, keepNumber = c.subjNumber, keepGender = c.subjGender;
  c.subjGender = 0;
  const bool keepObjVerb = c.objectInVerb;
  c.objectInVerb = false;
  c.frame = &f;
  c.negative = f.negative;
  c.question = f.type == Kind::Yn || f.type == Kind::Wh;
  c.motion = tables::motionVerb(f.pred.lemma) || tables::motionVerb(f.pred.complementVerb);
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
  cl.existential = f.existential;
  cl.exclQuam = f.exclQuam;
  // the clause's content lemmas (sense-keyword overlap)
  c.context.clear();
  if (f.hasPred) c.context.push_back(f.pred.lemma);
  if (f.hasSubject) c.context.push_back(f.subject.head);
  if (f.hasObject) c.context.push_back(f.object.head);
  for (const frame::SemOblique& o : f.obliques) c.context.push_back(o.np.head);
  for (const frame::SemAdj& a : f.predAdj) c.context.push_back(a.lemma);

  // subject
  if (f.hasSubject && f.type != Kind::Imp) {
    cl.hasSubject = true;
    npInto(f.subject, c, cl.subject);
    if (f.subject.pron.emphatic) cl.subject.emphasis = true;
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
  c.phrasalCase = 0;
  if (f.hasIndirect) {
    cl.hasIndirect = true;
    npInto(f.indirectObject, c, cl.indirect);
  }
  // obliques
  for (const frame::SemOblique& o : f.obliques) {
    if (stateVerb && (o.prep == "of" || o.prep == "about") && !cl.hasObject) {
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
  if (f.copula && !stateVerb) {
    for (const SemNP& pn : f.predicative) {
      if (f.type == Kind::Wh && f.wh.role == frame::Role::Predicate && pn.isPronoun && pn.interrogative) continue;
      LaNP x;
      npInto(pn, c, x);
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
        la.lemma = select(a.lemma, Adj, c.context, false, false, c.st, ch);
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
  // fragment adjectives ("A little.", "Very strange!")
  if (!f.hasPred && !f.predAdj.empty()) {
    for (const frame::SemAdj& a : f.predAdj) {
      Choice ch;
      ch.token = a.token;
      LaAdj la;
      la.lemma = select(a.lemma, Adj, c.context, false, false, c.st, ch);
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
    realise::LaAdverb la;
    la.lemma = adverb(a.lemma, a.token, c, c.motion);
    if (la.lemma == kNone) continue;
    // order.adv: a time adverb opens the clause only when the source puts it first ("Today the teacher ...")
    la.pos = a.front ? realise::AdvPos::Front : realise::AdvPos::BeforeVerb;
    // "never" carries the negation itself (numquam): the realiser drops nōn
    if ((a.lemma == "never" || a.lemma == "nunca") && cl.polarity == realise::Polarity::Neg) {}
    cl.adverbs.push_back(la);
  }
  for (const std::string& d : f.discourse)
    if (d == "please") {
      const uint32_t q = latin("quaesō", Verb);
      if (q != kNone) cl.politeness.push_back(q);
    }
  for (const std::string& k : f.connectors) {
    if (k == "then" && c.mem.prevFirst) {   // "First write ... Then write ...": deinde, an adverb at the front
      realise::LaAdverb a;
      a.lemma = latin("deinde", Adv);
      a.pos = realise::AdvPos::Front;
      if (a.lemma != kNone) { cl.adverbs.insert(cl.adverbs.begin(), a); continue; }
    }
    const char* la = tables::connector(k, c.mem.prevFirst);
    if (!la) continue;
    const uint32_t id = latin(la);
    if (id != kNone) cl.connectors.push_back(id);
  }
  // wh word
  if (f.type == Kind::Wh) {
    const std::string& w = f.wh.word;
    const char* la = nullptr;
    realise::Role role = realise::Role::None;
    uint8_t gender = 0;
    if (w == "where") la = c.motion ? "quō" : "ubi";
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
    clauseInto(sb.frame[0], c, sc);
    c.frame = &f;
    switch (sb.relation) {
      case Relation::Cause: ls.rel = realise::SubRel::Cause; break;
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
      case Relation::Purpose: ls.rel = realise::SubRel::Purpose; break;
      case Relation::Result: ls.rel = realise::SubRel::Result; break;
      case Relation::Concession: ls.rel = realise::SubRel::Concession; break;
      case Relation::Complement:
        ls.rel = sb.frame[0].type == Kind::Wh ? realise::SubRel::IndirectQ : realise::SubRel::AccInf;
        if (ls.rel == realise::SubRel::AccInf && sc.hasSubject && sc.subject.isPronoun &&
            sc.subject.pron.person == c.subjPerson && c.subjPerson == 3 && f.hasSubject && f.subject.isPronoun)
          sc.subject.pron.reflexive = true;
        if (ls.rel == realise::SubRel::AccInf && sc.hasSubject && sc.subject.isPronoun) sc.subject.emphasis = true;
        // "I thought it was Monday" -> Putābam diem Lūnae esse: an "it" subject with a predicate noun is dropped
        if (ls.rel == realise::SubRel::AccInf && sc.hasSubject && sc.subject.isPronoun && sf.hasSubject &&
            sf.subject.pronLemma == "it" && !sc.predicative.empty())
          sc.hasSubject = false;
        if (ls.rel == realise::SubRel::AccInf && !sc.hasSubject && sc.predicative.empty()) {   // implicit "it"
          sc.hasSubject = true;
          sc.subject.isPronoun = true;
          sc.subject.pron.person = 3;
          sc.subject.pron.gender = N;
          sc.subject.gender = N;
        }
        break;
      case Relation::Coord: {
        ls.rel = realise::SubRel::Coord;
        const char* k = sb.marker == "but" ? "sed" : sb.marker == "or" ? "aut" : "et";
        ls.conj = latin(k, Conj);
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
