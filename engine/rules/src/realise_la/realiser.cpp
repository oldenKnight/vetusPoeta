// LatinRealiser: wires the components of realise_la.h into one clause -> sentence pass (DESIGN.md §10.3).
#include <algorithm>
#include <array>

#include "vp/realise_la.h"
#include "vp/text.h"

namespace vp::realise {

using namespace vp::feat;

namespace {

enum Slot : int { kVOC, kCONN, kS, kIO, kO, kOBL, kADV, kNEG, kV, kPRED, kINF, kWH, kFRONT, kEND, kSlotCount };

int slotOf(const std::string& n) {
  if (n == "VOC") return kVOC;
  if (n == "CONN") return kCONN;
  if (n == "S") return kS;
  if (n == "IO") return kIO;
  if (n == "O") return kO;
  if (n == "OBL") return kOBL;
  if (n == "ADV") return kADV;
  if (n == "NEG") return kNEG;
  if (n == "V") return kV;
  if (n == "PRED") return kPRED;
  if (n == "INF") return kINF;
  if (n == "WH") return kWH;
  return -1;
}

// Canonical order used to insert slots a template does not name.
const int kCanon[] = {kFRONT, kWH, kS, kIO, kO, kOBL, kADV, kPRED, kINF, kNEG, kV, kEND};
int canonIndex(int s) {
  for (int i = 0; i < (int)(sizeof kCanon / sizeof kCanon[0]); ++i)
    if (kCanon[i] == s) return i;
  return 99;
}

void append(std::vector<Word>& dst, std::vector<Word>& src) {
  for (Word& w : src) dst.push_back(std::move(w));
  src.clear();
}

bool startsWithVowelOrH(std::string_view s) {
  const std::string k = text::latin_key(s);
  if (k.empty()) return false;
  const char c = k[0];
  return c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u' || c == 'h';
}

}  // namespace

struct LatinRealiser::Slots {
  std::array<std::vector<Word>, kSlotCount> s;
};

LatinRealiser::LatinRealiser(const lex::Lexicon& lx, const curated::CuratedData& cd)
    : lx_(lx), cd_(cd), forms_(lx), agree_(lx), cases_(lx, cd), names_(cd), order_(lx, cd), neg_(lx), pron_(lx),
      emoji_(lx, cd) {
  k_.sum = morph::findLemma(lx, "sum", Verb);
  k_.nolo = morph::findLemma(lx, "nōlō", Verb);
  k_.non = morph::findLemma(lx, "nōn");
  k_.nonne = morph::findLemma(lx, "nōnne", Adv);
  k_.num = morph::findLemma(lx, "num", Adv);
  k_.quam = morph::findLemma(lx, "quam", Adv);
  k_.o = morph::findLemma(lx, "ō", Intj);
  k_.et = morph::findLemma(lx, "et", Conj);
  k_.cum = morph::findLemma(lx, "cum", Prep);
  k_.ab = morph::findLemma(lx, "ab", Prep);
  k_.ex = morph::findLemma(lx, "ex", Prep);
  k_.qui = morph::findLemma(lx, "quī", Pron);
  k_.quis = morph::findLemma(lx, "quis", Pron);
  k_.hic = morph::findLemma(lx, "hic", Pron);
  k_.hicDet = morph::findLemma(lx, "hic", feat::Det);
  k_.ille = morph::findLemma(lx, "ille", feat::Det);
  if (k_.ille == kNone) k_.ille = morph::findLemma(lx, "ille", Pron);
  k_.iste = morph::findLemma(lx, "iste", feat::Det);
  if (k_.iste == kNone) k_.iste = morph::findLemma(lx, "iste", Pron);
  k_.is = morph::findLemma(lx, "is", Pron);
  k_.quia = morph::findLemma(lx, "quia", Conj);
  k_.cumConj = morph::findLemma(lx, "cum", Conj);
  k_.si = morph::findLemma(lx, "sī", Conj);
  k_.ut = morph::findLemma(lx, "ut", Conj);
  k_.ne = morph::findLemma(lx, "nē", Conj);
  k_.quamquam = morph::findLemma(lx, "quamquam", Conj);
}

void LatinRealiser::literal(uint32_t lemma, const char* fallback, Word& w, const char* rule) const {
  w = Word{};
  if (lemma != kNone) forms_.invariable(lemma, w);
  if (lemma == kNone || w.missing) { w = Word{}; w.form = fallback; w.lemma = kNone; }
  w.rule = rule;
}

void LatinRealiser::nameWord(const LaNP& n, uint8_t case_, const RealiseOptions& o, Word& w) {
  w = Word{};
  w.name = true;
  w.lemma = n.head;
  if (n.name.empty() && n.head != kNone) {   // a proper-name lemma of the lexicon
    Features f = morph::nounForm(case_, n.number);
    forms_.select(n.head, f, w);
    w.name = true;
    w.rule = "name.policy";
    return;
  }
  const NameForm nf = names_.form(n.name, case_, n.number, o.glossary);
  w.form = nf.form;
  Features f; f.pos = Name; f.case_ = nf.kept ? 0 : case_; f.number = n.number; f.gender = nf.gender;
  w.packed = pack(f);
  w.rule = "name.policy";
  if (nf.guessed) flags_.push_back("name-guessed");
  else if (nf.kept) flags_.push_back("name-kept");
}

void LatinRealiser::np(const LaNP& n, uint8_t case_, const LaClause* owner, const RealiseOptions& o,
                       std::vector<Word>& out) {
  const bool exclFirst = owner && owner->exclQuam;
  const size_t start0 = out.size();
  AgreeInfo a = agree_.ofNP(n, cd_);
  a.case_ = case_;
  if (!n.coord.empty()) {   // the head NP alone agrees with its own modifiers
    LaNP single = n;
    single.coord.clear();
    a = agree_.ofNP(single, cd_);
    a.case_ = case_;
  }
  if (!n.literal.empty()) {
    Word w;
    w.form = "[" + n.literal + "]";
    w.unknown = true;
    w.rule = "unknown";
    out.push_back(std::move(w));
  } else if (!n.fixed.empty()) {   // phrasebook Latin, written as it is ("chartīs lūdere": chartīs)
    size_t fa = 0;
    while (fa < n.fixed.size()) {
      size_t fb = n.fixed.find(' ', fa);
      if (fb == std::string::npos) fb = n.fixed.size();
      if (fb > fa) {
        Word w;
        w.form = n.fixed.substr(fa, fb - fa);
        morph::Token mt;
        morph::analyseLatin(lx_, w.form, mt);
        if (!mt.analyses.empty()) {
          w.lemma = mt.analyses[0].lemma;
          w.packed = morph::packedOf(lx_, mt.analyses[0]);
        }
        w.rule = "phrasebook";
        out.push_back(std::move(w));
      }
      fa = fb + 1;
    }
  } else {
    // pre-head: determiner, interrogative, numeral, quantity / demonstrative adjectives, contrastive possessive
    auto modifierWord = [&](uint32_t lemma, uint8_t degree, const char* rule) {
      Word w;
      forms_.select(lemma, agree_.modifier(a, degree), w);
      w.rule = rule;
      return w;
    };
    if (n.det != Det::None) {
      uint32_t d = n.det == Det::Hic ? k_.hicDet : n.det == Det::Ille ? k_.ille : n.det == Det::Iste ? k_.iste : k_.is;
      Word w;
      if (d == kNone || !forms_.select(d, agree_.modifier(a), w)) {
        const uint32_t alt = n.det == Det::Hic ? k_.hic : d;
        if (alt != kNone) forms_.select(alt, agree_.modifier(a), w);
      }
      w.rule = "order.adj";
      out.push_back(std::move(w));
    }
    if (n.interrogative != kNone) out.push_back(modifierWord(n.interrogative, 0, "order.wh"));
    if (n.numeral != kNone) out.push_back(modifierWord(n.numeral, 0, "order.num"));
    for (const LaAdj& ad : n.adjectives) {
      if (!(order_.adjectiveBefore(ad.lemma) || exclFirst)) continue;
      for (uint32_t adv : ad.adverbs) { Word w; literal(adv, "?", w, "order.adv"); out.push_back(std::move(w)); }
      out.push_back(modifierWord(ad.lemma, ad.degree, exclFirst ? "order.excl" : "order.adj"));
    }
    if (n.possessive != kNone && n.possContrast) out.push_back(modifierWord(n.possessive, 0, "order.poss"));
    // head
    if (n.isPronoun) {
      const uint32_t p = pron_.personal(n.pron.person, n.pron.number ? n.pron.number : n.number, n.pron.reflexive);
      Features f; f.pos = Pron; f.case_ = case_; f.number = a.number; f.gender = a.gender;
      Word w;
      if (p == kNone) { w.form = "[pron]"; w.missing = true; }
      else forms_.select(p, f, w);
      w.rule = "pron.drop";
      out.push_back(std::move(w));
    } else if (n.isName) {
      Word w;
      nameWord(n, case_, o, w);
      if (n.capitalise) Punctuation::capitaliseFirst(w.form);
      out.push_back(std::move(w));
    } else if (n.head != kNone) {
      const lex::Lemma l = lx_.lemma(n.head);
      Features f;
      if (l.pos == Noun || l.pos == Name) f = morph::nounForm(case_, a.number);
      else { f.pos = l.pos; f.case_ = case_; f.number = a.number; f.gender = a.gender; }
      Word w;
      forms_.select(n.head, f, w);
      if (o.emoji) w.emoji = std::string(emoji_.forLemma(n.head));
      if (n.capitalise) { Punctuation::capitaliseFirst(w.form); w.title = true; }
      w.rule = "order.adj";
      out.push_back(std::move(w));
    }
    // post-head: adjectives, possessive, genitive, relative clause
    for (const LaAdj& ad : n.adjectives) {
      if (order_.adjectiveBefore(ad.lemma) || exclFirst) continue;
      for (uint32_t adv : ad.adverbs) { Word w; literal(adv, "?", w, "order.adv"); out.push_back(std::move(w)); }
      out.push_back(modifierWord(ad.lemma, ad.degree, "order.adj"));
    }
    if (n.possessive != kNone && !n.possContrast) out.push_back(modifierWord(n.possessive, 0, "order.poss"));
    for (const LaNP& g : n.genitive) np(g, Gen, nullptr, o, out);
    for (const LaClause& rc : n.relative) {
      ClauseCtx rctx;
      rctx.main = false;
      rctx.relative = true;
      rctx.ante = a;
      rctx.ante.person = 3;
      std::vector<Word> rw;
      clause(rc, o, rw, rctx);
      append(out, rw);
    }
  }
  for (size_t i = 0; i < n.coord.size(); ++i) {
    std::vector<Word> cw;
    np(n.coord[i], case_, owner, o, cw);
    const bool que = n.coordQue && o.fidelity >= 3 && cw.size() == 1 && i + 1 == n.coord.size();
    if (que) {
      cw[0].form += "que";
      cw[0].rule = "enclitic.que";
    } else {
      Word et;
      literal(n.coordConj != kNone ? n.coordConj : k_.et, "et", et, "order.decl");
      out.push_back(std::move(et));
    }
    append(out, cw);
  }
  if (n.coordBoth && n.coordConj != kNone && !n.coord.empty()) {   // "neque canēs neque fēlēs"
    Word w;
    literal(n.coordConj, "et", w, "order.decl");
    out.insert(out.begin() + (long)start0, std::move(w));
  }
}

void LatinRealiser::verbGroup(const LaClause& c, const AgreeInfo& subj, std::vector<Word>& fin,
                              std::vector<Word>& inf, const ClauseCtx& ctx) {
  const LaPredicate& p = c.pred;
  const bool accInf = ctx.accInf;
  const uint8_t mood = ctx.forceMood ? ctx.forceMood : p.mood;
  if (p.lemma == kNone && p.modal == kNone && !(c.type == ClauseType::Imp && c.polarity == Polarity::Neg)) return;   // verbless
  auto push = [&](std::vector<Word>& dst, uint32_t lemma, const Features& f, const char* rule) {
    Word w;
    if (lemma == kNone) { w.form = "[verb]"; w.missing = true; w.rule = rule; dst.push_back(std::move(w)); return; }
    morph::GenInfo gi;
    std::string form;
    const lex::Lemma l = lx_.lemma(lemma);
    if (l.id != kNone && morph::generate(lx_, lemma, f, form, true, &gi)) {
      // split multi-word cells: periphrastic perfect (participle + sum), "nōn vīs", "amātūrum esse"
      size_t start = 0;
      int part = 0;
      while (start <= form.size()) {
        size_t sp = form.find(' ', start);
        Word w2;
        w2.form = form.substr(start, sp == std::string::npos ? std::string::npos : sp - start);
        w2.rule = rule;
        w2.fromRule = gi.fromRule;
        if (part == 0) {
          w2.lemma = lemma;
          w2.packed = gi.periphrastic ? pack(morph::participle(Perfect, l.flags & lex::Deponent ? Active : Passive,
                                                               Nom, f.number, f.gender ? f.gender : (uint8_t)M))
                                      : gi.packed;
        } else {
          const std::string k = text::latin_key(w2.form);
          if (k == "non") { w2.lemma = k_.non; w2.packed = 0; }
          else if (gi.periphrastic || k == "esse" || k == "fore") {
            w2.lemma = k_.sum;
            w2.packed = gi.periphrastic ? pack(morph::verbForm(f.person, f.number,
                                                               f.tense == Perfect ? (uint8_t)Present
                                                               : f.tense == Pluperfect ? (uint8_t)Imperfect
                                                                                       : (uint8_t)Future,
                                                               f.mood, Active))
                                        : 0;
          } else { w2.lemma = lemma; w2.packed = gi.packed; }
        }
        dst.push_back(std::move(w2));
        ++part;
        if (sp == std::string::npos) break;
        start = sp + 1;
      }
      return;
    }
    w.lemma = lemma;
    w.form = "[" + morph::displayForm(l.head, true) + "]";
    w.missing = true;
    w.rule = rule;
    dst.push_back(std::move(w));
  };
  auto infTense = [](uint8_t t) -> uint8_t {
    if (t == Perfect || t == Pluperfect) return Perfect;
    if (t == Future || t == FuturePerfect) return Future;
    return Present;
  };
  if (c.type == ClauseType::Imp) {
    const uint8_t num = subj.number;
    if (c.polarity == Polarity::Neg) {
      push(fin, k_.nolo, morph::imperative(num), "order.prohib");
      push(inf, p.lemma, morph::infinitive(Present, p.voice), "order.prohib");
    } else if (p.modal != kNone) {
      push(fin, p.modal, morph::imperative(num), "order.imp");
      push(inf, p.lemma, morph::infinitive(p.infTense, p.infVoice), "order.imp");
    } else {
      push(fin, p.lemma, morph::imperative(num, p.voice), "order.imp");
    }
    return;
  }
  if (accInf) {
    if (p.modal != kNone) {
      push(fin, p.modal, morph::infinitive(infTense(p.tense), Active), "order.acc.inf");
      push(inf, p.lemma, morph::infinitive(p.infTense, p.infVoice), "order.inf");
    } else {
      push(fin, p.lemma, morph::infinitive(infTense(p.tense), p.voice), "order.acc.inf");
    }
    return;
  }
  LaPredicate q = p;
  q.mood = mood;
  if (p.modal != kNone) {
    push(fin, p.modal, agree_.finiteVerb(subj, q), "order.inf");
    push(inf, p.lemma, morph::infinitive(p.infTense, p.infVoice), "order.inf");
  } else {
    push(fin, p.lemma, agree_.finiteVerb(subj, q), "order.decl");
  }
}

void LatinRealiser::clause(const LaClause& c, const RealiseOptions& o, std::vector<Word>& out,
                           const ClauseCtx& ctx) {
  Slots S;
  auto& s = S.s;
  std::vector<Word> intj, conn2, polite;
  int focus = -1;
  const char* orderRule = "order.decl";

  // Subject agreement (person, number, gender)
  AgreeInfo subj;
  if (ctx.relative && c.relRole == Role::Subject) subj = ctx.ante;
  else if (c.hasSubject) subj = agree_.ofNP(c.subject, cd_);
  if (c.type == ClauseType::Imp) {
    subj.person = 2;
    uint8_t num = c.pred.number;
    if (!num) {
      num = Sg;
      for (const LaNP& v : c.vocatives)
        if (v.number == Pl || !v.coord.empty()) num = Pl;
      if (c.hasSubject && c.subject.isPronoun && c.subject.pron.number == Pl) num = Pl;
    }
    subj.number = num;
  }
  if (c.pred.person) subj.person = c.pred.person;
  if (c.pred.number) subj.number = c.pred.number;
  const uint8_t subjCase = (c.exclO || ctx.accInf) ? (uint8_t)Acc : (uint8_t)Nom;

  // Interjections, vocatives, connectors
  for (uint32_t ij : c.interjections) {
    Word w;
    literal(ij, "ō", w, "order.voc");
    if (ij == k_.o) w.form = "ō";
    intj.push_back(std::move(w));
  }
  for (const LaNP& v : c.vocatives) np(v, Voc, &c, o, s[kVOC]);
  for (uint32_t k : c.connectors) {
    Word w;
    literal(k, "?", w, order_.connectorSecond(k) ? "order.conn" : "order.conn.first");
    (order_.connectorSecond(k) ? conn2 : s[kCONN]).push_back(std::move(w));
  }
  // Subject
  const bool drop = pron_.dropSubject(c) || (ctx.relative && c.relRole == Role::Subject);
  if (c.hasSubject && !drop && !c.exclQuam && !c.exclO) {
    np(c.subject, subjCase, &c, o, s[kS]);
    if (c.subject.emphasis) focus = kS;
    if (c.subject.interrogative != kNone) append(s[kWH], s[kS]);
  }
  // Objects
  if (c.hasObject && !(ctx.relative && c.relRole == Role::Object)) {
    const uint8_t oc = c.object.case_ ? c.object.case_ : cases_.objectCase(c.pred.lemma);
    np(c.object, oc, &c, o, s[kO]);
    if (c.object.emphasis) focus = kO;
    if (c.object.interrogative != kNone) append(s[kWH], s[kO]);
  }
  if (c.hasIndirect && !(ctx.relative && c.relRole == Role::IndirectObject)) {
    np(c.indirect, c.indirect.case_ ? c.indirect.case_ : (uint8_t)Dat, &c, o, s[kIO]);
    if (c.indirect.emphasis) focus = kIO;
  }
  // Obliques
  for (const LaOblique& ob : c.obliques) {
    const uint8_t oc = ob.np.case_ ? ob.np.case_ : ob.prep == kNone ? (ob.case_ ? ob.case_ : (uint8_t)Abl)
                                                                   : cases_.prepCase(ob.prep, ob.case_);
    std::vector<Word> w;
    np(ob.np, oc, &c, o, w);
    if (ob.prep != kNone) {
      Word pw;
      bool enclitic = false;
      if (ob.prep == k_.cum && ob.np.isPronoun && w.size() == 1) {
        const std::string joined = text::latin_key(w[0].form) + "cum";
        if (order_.encliticCum(joined)) {
          w[0].form += "cum";
          w[0].rule = "order.prep";
          enclitic = true;
        }
      }
      if (!enclitic) {
        literal(ob.prep, "?", pw, "order.prep");
        const bool vowel = !w.empty() && startsWithVowelOrH(w[0].form);
        if (ob.prep == k_.ab) pw.form = vowel ? "ab" : "ā";
        if (ob.prep == k_.ex) pw.form = vowel ? "ex" : "ē";
        w.insert(w.begin(), std::move(pw));
      }
    }
    const bool wh = ob.np.interrogative != kNone;
    std::vector<Word>& dst = wh ? s[kWH] : ob.front ? s[kFRONT] : s[kOBL];
    if (ob.np.emphasis && !wh) focus = ob.front ? kFRONT : kOBL;
    append(dst, w);
  }
  // Adverbs
  for (const LaAdverb& ad : c.adverbs) {
    Word w;
    literal(ad.lemma, "?", w, "order.adv");
    if (ad.pos == AdvPos::Front || (ad.pos == AdvPos::Auto && order_.timeAdverb(ad.lemma))) s[kFRONT].push_back(std::move(w));
    else if (ad.pos == AdvPos::End) s[kEND].push_back(std::move(w));
    else s[kADV].push_back(std::move(w));
  }
  // Predicate (copula)
  const bool isSum = c.pred.lemma != kNone && c.pred.lemma == k_.sum && c.pred.modal == kNone;
  const bool isCopula = isSum && (!c.predicative.empty() || !c.predAdj.empty());
  const bool exist = isSum && c.existential && !isCopula;
  for (const LaNP& pn : c.predicative) {
    np(pn, subjCase, &c, o, s[kPRED]);
    if (pn.interrogative != kNone) append(s[kWH], s[kPRED]);
  }
  if (!c.predAdj.empty()) {
    AgreeInfo pa = subj;
    pa.case_ = subjCase;
    if (c.predGender) pa.gender = c.predGender;
    if (c.predNumber) pa.number = c.predNumber;
    if (!c.hasSubject && !c.predGender && subj.gender == 0) pa.gender = M;
    for (size_t i = 0; i < c.predAdj.size(); ++i) {
      if (i) { Word et; literal(k_.et, "et", et, "order.copula"); s[kPRED].push_back(std::move(et)); }
      for (uint32_t adv : c.predAdj[i].adverbs) { Word w; literal(adv, "?", w, "order.adv"); s[kPRED].push_back(std::move(w)); }
      Word w;
      forms_.select(c.predAdj[i].lemma, agree_.modifier(pa, c.predAdj[i].degree), w);
      w.rule = "order.copula";
      s[kPRED].push_back(std::move(w));
    }
  }
  // Wh word
  if (c.type == ClauseType::Wh && c.wh.lemma != kNone) {
    Word w;
    const lex::Lemma wl = lx_.lemma(c.wh.lemma);
    if (c.wh.role == Role::None || wl.pos == Adv) literal(c.wh.lemma, "?", w, "order.wh");
    else {
      Features f; f.pos = Pron; f.number = Sg; f.gender = c.wh.gender ? c.wh.gender : (uint8_t)M;
      f.case_ = c.wh.role == Role::Object ? cases_.objectCase(c.pred.lemma) : c.wh.role == Role::IndirectObject ? (uint8_t)Dat
                                                                              : (uint8_t)Nom;
      forms_.select(c.wh.lemma, f, w);
      w.rule = "order.wh";
    }
    s[kWH].insert(s[kWH].begin(), std::move(w));
  }
  // Relative pronoun (first in its clause)
  if (ctx.relative && c.relRole != Role::None && k_.qui != kNone) {
    uint8_t rc = Nom;
    if (c.relRole == Role::Object) rc = cases_.objectCase(c.pred.lemma);
    else if (c.relRole == Role::IndirectObject) rc = Dat;
    else if (c.relRole == Role::Oblique) rc = cases_.prepCase(c.relPrep, 0);
    Word w;
    forms_.select(k_.qui, agree_.relative(ctx.ante, rc), w);
    w.rule = "order.rel";
    std::vector<Word> rel;
    if (c.relRole == Role::Oblique && c.relPrep != kNone) {
      if (c.relPrep == k_.cum && order_.encliticCum(text::latin_key(w.form) + "cum")) {
        w.form += "cum";
        rel.push_back(std::move(w));
      } else {
        Word pw;
        literal(c.relPrep, "?", pw, "order.rel");
        rel.push_back(std::move(pw));
        rel.push_back(std::move(w));
      }
    } else rel.push_back(std::move(w));
    for (Word& x : s[kWH]) rel.push_back(std::move(x));
    s[kWH] = std::move(rel);
  }
  // Verb group
  verbGroup(c, subj, s[kV], s[kINF], ctx);
  // Negation
  const bool prohib = c.type == ClauseType::Imp && c.polarity == Polarity::Neg;
  const bool nonne = c.type == ClauseType::Yn && (c.bias == YnBias::ExpectYes || c.polarity == Polarity::Neg);
  if (c.polarity == Polarity::Neg && !prohib && !nonne && !ctx.suppressNon && !neg_.clauseHasNegativeWord(c)) {
    Word w;
    literal(k_.non, "nōn", w, "order.decl");
    s[kNEG].push_back(std::move(w));
  }

  // ---- linearise ----
  std::vector<int> seq;
  auto addTemplate = [&](const std::vector<std::string>& t) {
    for (const std::string& n : t) {
      const int sl = slotOf(n);
      if (sl < 0 || sl == kVOC || sl == kCONN) continue;
      if (std::find(seq.begin(), seq.end(), sl) == seq.end()) seq.push_back(sl);
    }
  };
  std::vector<Word> content;
  if (c.type == ClauseType::Excl && (c.exclO || c.exclQuam)) {
    Word lead;
    if (c.exclO) { literal(k_.o, "ō", lead, "order.excl"); lead.form = "ō"; }
    else literal(k_.quam, "quam", lead, "order.excl");
    content.push_back(std::move(lead));
    std::vector<Word> subjWords;
    if (c.hasSubject) np(c.subject, subjCase, &c, o, subjWords);
    if (c.exclO) {
      append(content, subjWords);
      append(content, s[kPRED]);
    } else if (!s[kPRED].empty()) {
      append(content, s[kPRED]);
      append(content, s[kV]);
      append(content, subjWords);
    } else {
      append(content, subjWords);
      append(content, s[kV]);
    }
    orderRule = "order.excl";
  } else if (c.type == ClauseType::Frag) {
    for (int sl : {kFRONT, kWH, kS, kPRED, kIO, kO, kOBL, kADV, kNEG, kV, kINF, kEND}) append(content, s[sl]);
    orderRule = "order.decl";
  } else if (c.type == ClauseType::Imp) {
    // order.imp: verb first when the clause is short. "words" counts the complements (the verb itself and the
    // vocative excluded): "Dā mihi colōrem rubrum" is short, "Rosās in hortō nōlī carpere" is long.
    size_t words = 0;
    for (int sl : {kFRONT, kIO, kO, kOBL, kADV, kPRED}) words += s[sl].size();
    const bool shortImp = words <= 3 && s[kFRONT].empty();   // "Prīmum nōmen tuum scrībe": a fronted adverb keeps V last
    if (prohib) {
      orderRule = "order.prohib";
      if (words <= 1) seq = {kFRONT, kV, kIO, kO, kOBL, kADV, kINF};
      else seq = {kFRONT, kIO, kO, kOBL, kADV, kV, kINF};
    } else if (shortImp) {
      orderRule = "order.imp";
      seq = {kFRONT, kV, kIO, kO, kOBL, kADV, kINF, kPRED};
    } else {
      orderRule = "order.imp.long";
      seq.push_back(kFRONT);
      addTemplate(order_.slots("order.imp.long"));
    }
  } else {
    orderRule = exist ? "order.exist" : isCopula ? "order.copula" : c.pred.modal != kNone ? "order.inf" : "order.decl";
    if (ctx.accInf) orderRule = "order.acc.inf";
    seq.push_back(kWH);
    seq.push_back(kFRONT);
    addTemplate(order_.slots(exist ? "order.exist" : isCopula ? "order.copula" : "order.decl"));
  }
  if (content.empty()) {
    // slots the template does not name go before the first template slot that follows them canonically
    for (int sl : {kWH, kFRONT, kS, kIO, kO, kOBL, kADV, kPRED, kINF, kNEG, kV}) {
      if (s[sl].empty() || std::find(seq.begin(), seq.end(), sl) != seq.end()) continue;
      auto pos = seq.end();
      for (auto it = seq.begin(); it != seq.end(); ++it)
        if (canonIndex(*it) > canonIndex(sl)) { pos = it; break; }
      // the infinitive goes before nōn: "venīre nōn potest"
      if (sl == kINF) {
        auto neg = std::find(seq.begin(), seq.end(), kNEG);
        auto v = std::find(seq.begin(), seq.end(), kV);
        pos = neg != seq.end() ? neg : v;
      }
      seq.insert(pos, sl);
    }
    seq.push_back(kEND);
    // order.wh.cop: the copula comes second after an interrogative predicate ("Quī diēs est hodiē?", "Quis es?")
    bool whPred = c.type == ClauseType::Wh && isSum && c.wh.role == Role::Predicate;
    for (const LaNP& pn : c.predicative) whPred = whPred || (c.type == ClauseType::Wh && isSum && pn.interrogative != kNone);
    if (whPred && !s[kV].empty() && !ctx.accInf) {
      seq.erase(std::remove(seq.begin(), seq.end(), kV), seq.end());
      const bool neg = std::find(seq.begin(), seq.end(), kNEG) != seq.end();
      if (neg) seq.erase(std::remove(seq.begin(), seq.end(), kNEG), seq.end());
      auto at = std::find(seq.begin(), seq.end(), kWH);
      at = at == seq.end() ? seq.begin() : at + 1;
      at = seq.insert(at, kV);
      if (neg) seq.insert(at, kNEG);
      orderRule = "order.wh.cop";
    }
    // order.neg.degree: nōn before a degree adverb that the negation scopes over ("Nōn multum cūrō")
    if (!s[kNEG].empty() && !s[kADV].empty()) {
      bool allDegree = true;
      for (const Word& w : s[kADV]) allDegree = allDegree && w.lemma != kNone && order_.degreeAdverb(w.lemma);
      if (allDegree) {
        seq.erase(std::remove(seq.begin(), seq.end(), kNEG), seq.end());
        auto adv = std::find(seq.begin(), seq.end(), kADV);
        if (adv != seq.end()) seq.insert(adv, kNEG);
        else seq.insert(std::find(seq.begin(), seq.end(), kV), kNEG);
        orderRule = "order.neg.degree";
      }
    }
    // yes/no questions: nōnne / num first; otherwise -ne on the verb (or the focused word), moved first
    if (c.type == ClauseType::Yn && !nonne && c.bias != YnBias::ExpectNo) {
      const int host = focus >= 0 && !s[focus].empty() ? focus : !s[kV].empty() ? kV : -1;
      if (host >= 0) {
        auto it = std::find(seq.begin(), seq.end(), host);
        if (it != seq.end()) seq.erase(it);
        seq.insert(seq.begin(), host);
        s[host][0].form += "ne";
        s[host][0].rule = "order.yn";
      }
    }
    for (int sl : seq)
      if (sl >= 0 && sl < kSlotCount) append(content, s[sl]);
  }
  if (c.type == ClauseType::Yn && (nonne || c.bias == YnBias::ExpectNo)) {
    Word w;
    if (nonne) literal(k_.nonne, "nōnne", w, "order.yn.nonne");
    else literal(k_.num, "num", w, "order.yn.num");
    content.insert(content.begin(), std::move(w));
  }
  // second-position connectors (after the first word; a preposition keeps its noun)
  if (!conn2.empty()) {
    size_t at = content.empty() ? 0 : 1;
    if (!content.empty() && content[0].lemma != kNone && lx_.lemma(content[0].lemma).pos == Prep && content.size() > 1) at = 2;
    for (size_t i = 0; i < conn2.size(); ++i) content.insert(content.begin() + (long)(at + i), std::move(conn2[i]));
  }
  // assemble: interjections, vocatives, first-position connectors, content, politeness
  std::vector<Word> clauseWords;
  append(clauseWords, intj);
  if (!s[kVOC].empty()) {
    s[kVOC].back().punctAfter = ",";
    append(clauseWords, s[kVOC]);
  }
  append(clauseWords, s[kCONN]);
  append(clauseWords, content);
  if (!clauseWords.empty() && !clauseWords.back().rule[0]) clauseWords.back().rule = orderRule;
  if (!c.politeness.empty() && !clauseWords.empty()) {
    clauseWords.back().punctAfter = ",";
    for (uint32_t pl : c.politeness) { Word w; literal(pl, "?", w, "polite"); polite.push_back(std::move(w)); }
    append(clauseWords, polite);
  }

  // Subordinate clauses
  std::vector<Word> before, after;
  for (const LaSub& sub : c.subs) {
    if (sub.clause.empty()) continue;
    const LaClause& sc = sub.clause[0];
    std::vector<Word> sw;
    uint32_t conj = sub.conj;
    const bool neg = sc.polarity == Polarity::Neg;
    if (conj == kNone) {
      switch (sub.rel) {
        case SubRel::Cause: conj = k_.quia; break;
        case SubRel::Time: conj = k_.cumConj; break;
        case SubRel::Condition: conj = k_.si; break;
        case SubRel::Purpose: conj = neg ? k_.ne : k_.ut; break;
        case SubRel::Result: conj = k_.ut; break;
        case SubRel::Concession: conj = k_.quamquam; break;
        case SubRel::Coord: conj = k_.et; break;
        default: break;
      }
    }
    if (conj != kNone) {
      Word w;
      literal(conj, "?", w, sub.before ? "order.sub.pre" : sub.rel == SubRel::Purpose ? "order.sub.purp" : "order.sub.pre");
      sw.push_back(std::move(w));
    }
    ClauseCtx sctx;
    sctx.main = false;
    sctx.accInf = sub.rel == SubRel::AccInf;
    if (sub.rel == SubRel::Purpose) { sctx.forceMood = Subjunctive; sctx.suppressNon = neg; }
    std::vector<Word> body;
    clause(sc, o, body, sctx);
    append(sw, body);
    if (sub.before) {
      if (!sw.empty()) sw.back().punctAfter = ",";
      append(before, sw);
    } else {
      if (!sub.sep.empty()) {   // "Certē es; aliter hīc nōn essēs"
        if (!after.empty()) after.back().punctAfter = sub.sep;
        else if (!clauseWords.empty()) clauseWords.back().punctAfter = sub.sep;
      }
      append(after, sw);
    }
  }
  append(out, before);
  append(out, clauseWords);
  append(out, after);
}

void LatinRealiser::finish(const RealiseOptions& o, std::vector<Word>& words, LaSentence& out) {
  if (!words.empty()) Punctuation::capitaliseFirst(words[0].form);
  for (size_t i = 0; i < words.size(); ++i) {
    Word& w = words[i];
    if (i) out.text += ' ';
    rules::TokenView t;
    const curated::MacronOverride* ov = w.lemma != kNone ? cd_.macronOverride(lx_.lemma(w.lemma).key) : nullptr;
    t.display = Macrons::apply(w.form, true, ov);
    t.text = Macrons::apply(w.form, o.macrons, ov);
    t.start = (int)out.text.size();
    out.text += t.text;
    t.end = (int)out.text.size();
    if (w.lemma != kNone) {
      t.lemmaId = w.lemma;
      t.hasLemma = true;
      const lex::Lemma l = lx_.lemma(w.lemma);
      t.tier = cd_.effectiveTier(l.key, l.pos, l.tier);
    }
    t.features = featureView(w.packed);
    t.emoji = w.emoji;
    t.unknown = w.unknown;
    t.fromRule = w.fromRule;
    if (o.emojiInText && !w.emoji.empty()) { out.text += ' '; out.text += w.emoji; }
    out.text += w.punctAfter;
    const int idx = (int)out.tokens.size();
    out.tokens.push_back(std::move(t));
    if (w.fromRule) {
      out.reasons.push_back(rules::Reason{idx, "form", "paradigm fallback (no inflection table): check this form", w.form});
      flags_.push_back("from-rule");
    }
    if (w.missing) {
      out.reasons.push_back(rules::Reason{idx, "form", "no lexicon cell for these features", w.form});
      flags_.push_back("missing-form");
    }
    if (w.unknown) flags_.push_back("unknown");
    if (w.name) out.reasons.push_back(rules::Reason{idx, "name", "name policy", w.form});
    if (w.rule && w.rule[0]) out.reasons.push_back(rules::Reason{idx, "form", w.rule, ""});
  }
}

void LatinRealiser::realise(const LaClause& c, const RealiseOptions& o, LaSentence& out) {
  out.clear();
  words_.clear();
  flags_.clear();
  clause(c, o, words_, ClauseCtx{});
  finish(o, words_, out);
  out.text += Punctuation::finalMark(c);
  for (const std::string& f : flags_)
    if (std::find(out.flags.begin(), out.flags.end(), f) == out.flags.end()) out.flags.push_back(f);
  words_.clear();
}

}  // namespace vp::realise
