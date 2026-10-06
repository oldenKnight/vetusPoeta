// GreekRealiser: clause structure -> Attic sentence (vp/realise_grc.h).
#include <algorithm>
#include <array>

#include "vp/realise_grc.h"
#include "vp/text.h"

namespace vp::grc {

using namespace vp::feat;

namespace {

enum Slot : int { kVOC, kCONN, kS, kIO, kO, kOBL, kADV, kNEG, kV, kPRED, kINF, kWH, kFRONT, kEND, kPRED2, kPTC,
                  kSlotCount };   // kPTC (C18): a circumstantial participle phrase, right after the subject

int slotOf(const std::string& n) {
  static const char* const names[] = {"VOC", "CONN", "S", "IO", "O", "OBL", "ADV", "NEG", "V", "PRED", "INF", "WH"};
  for (int i = 0; i < 12; ++i)
    if (n == names[i]) return i;
  return -1;
}
const int kCanon[] = {kFRONT, kWH, kS, kPTC, kIO, kO, kOBL, kADV, kPRED, kINF, kNEG, kV, kPRED2, kEND};
int canonIndex(int s) {
  for (int i = 0; i < (int)(sizeof kCanon / sizeof kCanon[0]); ++i)
    if (kCanon[i] == s) return i;
  return 99;
}

void append(std::vector<GWord>& dst, std::vector<GWord>& src) {
  for (GWord& w : src) dst.push_back(std::move(w));
  src.clear();
}

uint8_t simpleGender(uint8_t g) {
  switch (g) {
    case M: case F: case N: return g;
    case FN: return F;
    default: return M;
  }
}

bool inKeys(const std::vector<std::string>& v, std::string_view k) { return std::find(v.begin(), v.end(), k) != v.end(); }

// C16: an impersonal modal with accusative + infinitive by valency_grc.tsv (χρή; δεῖ has its own closed id)
bool impersAccInf(const lex::Lexicon& lx, const GreekData& gd, uint32_t verb) {
  if (verb == kNone) return false;
  const lex::Lemma l = lx.lemma(verb);
  if (l.id == kNone) return false;
  if (const Valency* v = gd.valency(l.key))
    for (const Frame& f : v->frames)
      if (f.kind == FrameKind::ImpersAccInf) return true;
  return false;
}

}  // namespace

GreekRealiser::GreekRealiser(const lex::Lexicon& lx, const curated::CuratedData& cd, const GreekData& gd)
    : lx_(lx), cd_(cd), gd_(gd) {
  auto L = [&](const char* h, uint8_t pos) {
    uint32_t id = findLemma(lx, h, pos);
    if (id == kNone && pos) id = findLemma(lx, h, 0);
    return id;
  };
  k_.eimi = L("εἰμί", Verb);
  k_.art = L("ὁ", Article);
  k_.ou = L("οὐ", 0);
  k_.me = L("μή", 0);
  k_.ara = L("ἆρα", 0);
  k_.kai = L("καί", Conj);
  k_.o = L("ὦ", Intj);
  k_.hos = L("ὅς", Pron);
  k_.hosRel = k_.hos;
  k_.ego = L("ἐγώ", Pron);
  k_.su = L("σύ", Pron);
  k_.hemeis = L("ἡμεῖς", Pron);
  k_.humeis = L("ὑμεῖς", Pron);
  k_.autos = L("αὐτός", Pron);
  k_.houtos = L("οὗτος", feat::Det);
  k_.ekeinos = L("ἐκεῖνος", 0);
  k_.tis = L("τίς", Pron);
  k_.tisIndef = L("τις", Pron);
  k_.hoti = L("ὅτι", Conj);
  k_.epei = L("ἐπεί", Conj);
  k_.hote = L("ὅτε", Conj);
  k_.ei = L("εἰ", Conj);
  k_.ean = L("ἐάν", Conj);
  k_.hina = L("ἵνα", Conj);
  k_.hopos = L("ὅπως", Conj);
  k_.hoste = L("ὥστε", Conj);
  k_.hos2 = L("ὡς", Adv);
  k_.emos = L("ἐμός", Adj);
  k_.sos = L("σός", Adj);
  k_.hemeteros = L("ἡμέτερος", Adj);
  k_.humeteros = L("ὑμέτερος", Adj);
  k_.dei = L("δεῖ", Verb);
  k_.exesti = L("ἔξεστι", Verb);
  k_.phemi = L("φημί", Verb);
  k_.oudeis = L("οὐδείς", Pron);
  k_.medeis = L("μηδείς", Pron);
  secondKeys_ = gd.conditionSet("order.conn.second");
  firstKeys_ = gd.conditionSet("order.conn.first");
  timeAdv_ = gd.orderingList("order.adv", "time adverbs");
  beforeNoun_ = gd.orderingList("order.adj", "before it");
  if (secondKeys_.empty()) secondKeys_ = {"δέ", "γάρ", "οὖν", "μέν", "γε", "δή", "τοίνυν", "δήπου", "τε"};
  if (timeAdv_.empty()) timeAdv_ = {"νῦν", "ἤδη", "ἀεί", "τότε", "ἔπειτα"};
  for (const char* k : {"οὐδείσ", "μηδείσ", "οὐδέν", "μηδέν", "οὐδέποτε", "μηδέποτε", "οὐδαμῶσ", "οὐκέτι", "μηκέτι",
                        "οὐδαμοῦ", "οὐδέ", "μηδέ"})
    negKeys_.push_back(k);
}

bool GreekRealiser::middleLemma(uint32_t lemma) const {
  const lex::Lemma l = lx_.lemma(lemma);
  if (l.id == kNone) return false;
  if (l.flags & lex::Deponent) return true;
  const std::string b = text::greek_bare(l.head);
  return b.size() >= 6 && b.compare(b.size() - 6, 6, "μαι") == 0;
}
bool GreekRealiser::timeAdverb(uint32_t lemma) const {
  const lex::Lemma l = lx_.lemma(lemma);
  return l.id != kNone && inKeys(timeAdv_, l.key);
}
bool GreekRealiser::beforeNoun(uint32_t lemma) const {
  const lex::Lemma l = lx_.lemma(lemma);
  return l.id != kNone && (inKeys(beforeNoun_, l.key) || l.pos == Num);
}
bool GreekRealiser::negativeWord(uint32_t lemma) const {
  const lex::Lemma l = lx_.lemma(lemma);
  return l.id != kNone && inKeys(negKeys_, l.key);
}

// ---- case assignment ------------------------------------------------------------------------------------------------
uint8_t GreekRealiser::objectCase(uint32_t verb, bool middle) const {
  const lex::Lemma l = lx_.lemma(verb);
  if (l.id == kNone) return Acc;
  if (const Valency* v = gd_.valency(l.key)) {
    for (int pass = 0; pass < 2; ++pass)
      for (const Frame& f : v->frames) {
        if (pass == 0 && f.middle != middle) continue;
        switch (f.kind) {
          case FrameKind::Acc: case FrameKind::AccAcc: case FrameKind::AccInf: case FrameKind::DatAcc: return Acc;
          case FrameKind::Dat: return Dat;
          case FrameKind::Gen: return Gen;
          default: break;
        }
      }
    return Acc;
  }
  thread_local std::vector<lex::Sense> senses;
  senses.clear();
  lx_.senses(verb, senses);
  if (!senses.empty()) {
    const uint16_t t = senses[0].tags;
    const bool acc = t & (1u << 11);
    if (!acc && (t & (1u << 8))) return Dat;
    if (!acc && (t & (1u << 10))) return Gen;
  }
  return Acc;
}

uint8_t GreekRealiser::prepCase(uint32_t prep, uint8_t override) const {
  if (override) return override;
  const lex::Lemma l = lx_.lemma(prep);
  if (l.id == kNone) return Acc;
  const uint8_t c = gd_.prepDefaultCase(l.key);
  return c ? c : (uint8_t)Acc;
}

// ---- words ------------------------------------------------------------------------------------------------------------
void GreekRealiser::literal(uint32_t lemma, const char* fallback, GWord& w, const char* rule) const {
  w = GWord{};
  const lex::Lemma l = lx_.lemma(lemma);
  if (lemma != kNone && l.id != kNone) {
    w.form = display(l.head);
    w.lemma = lemma;
    Features f; f.pos = l.pos;
    w.packed = pack(f);
  } else {
    w.form = fallback;
  }
  w.rule = rule;
}

void GreekRealiser::form(uint32_t lemma, const Features& f, GWord& w, const char* rule) {
  w.lemma = lemma;
  w.rule = rule;
  const lex::Lemma l = lx_.lemma(lemma);
  if (l.id == kNone) { w.form = "[?]"; w.missing = true; return; }
  switch (l.pos) {
    case Adv: case Conj: case Prep: case Intj: case Particle: case Phrase: case Postp:
      literal(lemma, "?", w, rule);
      return;
    default: break;
  }
  if (l.pos == Participle && !(l.flags & lex::HasTable)) {   // particles mislabelled as participles in the library
    literal(lemma, "?", w, rule);
    return;
  }
  GenInfo gi;
  std::string out;
  if (generate(lx_, lemma, f, out, &gi)) {
    w.form = std::move(out);
    w.packed = gi.packed;
    w.fromRule = gi.fromRule;
    w.movableNu = gi.movableNu;
    if ((lemma == k_.eimi || lemma == k_.phemi) && f.mood == Indicative && (f.tense == Present) && isEncliticForm(w.form))
      w.enclitic = true;
    return;
  }
  if (l.flags & lex::Indeclinable) { literal(lemma, "?", w, rule); return; }
  // C16: cardinals from five up are indeclinable (ἕξ, ἑπτά ...) even when the lexicon lacks the flag
  if (l.pos == Num && !(l.flags & lex::HasTable)) { literal(lemma, "?", w, rule); return; }
  w.form = "[" + display(l.head) + "]";
  w.missing = true;
}

uint8_t GreekRealiser::nounGender(const GrcNP& n) const {
  if (n.gender) return simpleGender(n.gender);
  if (n.isPronoun) return simpleGender(n.pron.gender);
  if (n.isName) {
    if (const NameEntry* e = gd_.nameByEnglish(n.name)) return simpleGender(e->gender);
    if (n.head == kNone) return M;
  }
  if (n.head == kNone) return M;
  return simpleGender(lx_.lemma(n.head).gender);
}

GreekRealiser::Agree GreekRealiser::ofNP(const GrcNP& n) const {
  Agree a;
  a.case_ = n.case_;
  a.gender = nounGender(n);
  a.number = n.number ? n.number : (uint8_t)Sg;
  a.person = n.isPronoun && n.pron.person ? n.pron.person : 3;
  if (n.isPronoun && n.pron.number) a.number = n.pron.number;
  if (!n.coord.empty()) {
    a.number = Pl;
    bool allF = a.gender == F, allN = a.gender == N;
    for (const GrcNP& c : n.coord) {
      const Agree b = ofNP(c);
      if (b.person < a.person) a.person = b.person;
      allF = allF && b.gender == F;
      allN = allN && b.gender == N;
    }
    a.gender = allF ? (uint8_t)F : allN ? (uint8_t)N : (uint8_t)M;
  }
  return a;
}

void GreekRealiser::adjWord(const GrcAdj& ad, const Agree& a, const char* rule, std::vector<GWord>& out) {
  for (uint32_t adv : ad.adverbs) { GWord w; literal(adv, "?", w, "order.adv"); out.push_back(std::move(w)); }
  GWord w;
  form(ad.lemma, adjForm(a.case_, a.number, a.gender, ad.degree), w, rule);
  out.push_back(std::move(w));
}

void GreekRealiser::pronounWord(const GrcNP& n, uint8_t case_, bool afterPrep, GWord& w) {
  const uint8_t person = n.pron.person ? n.pron.person : 3;
  const uint8_t number = n.pron.number ? n.pron.number : n.number;
  uint32_t lemma = kNone;
  const char* key = "αὐτόσ";
  if (person == 1) { lemma = number == Pl ? k_.hemeis : k_.ego; key = number == Pl ? "ἡμεῖσ" : "ἐγώ"; }
  else if (person == 2) { lemma = number == Pl ? k_.humeis : k_.su; key = number == Pl ? "ὑμεῖσ" : "σύ"; }
  else lemma = k_.autos;
  const bool enclitic = person <= 2 && number == Sg && case_ != Nom && case_ != Voc && !n.emphasis && !afterPrep;
  w = GWord{};
  w.lemma = lemma;
  w.rule = "pron.encl";
  std::string f;
  if (!closedForm(key, case_, number, simpleGender(n.pron.gender ? n.pron.gender : n.gender), enclitic, f)) {
    w.form = "[pron]";
    w.missing = true;
    return;
  }
  w.form = f;
  w.enclitic = enclitic;
  Features ft; ft.pos = Pron; ft.case_ = case_; ft.number = number; ft.person = person <= 2 ? person : 0;
  ft.gender = person == 3 ? simpleGender(n.pron.gender ? n.pron.gender : n.gender) : 0;
  w.packed = pack(ft);
}

void GreekRealiser::nameWord(const GrcNP& n, uint8_t case_, const GrcOptions& o, GWord& w) {
  w = GWord{};
  w.name = true;
  w.rule = "name.policy";
  w.lemma = n.head;
  if (n.name.empty() && n.head != kNone) {   // a proper-name lemma of the lexicon
    form(n.head, nounForm(case_, n.number), w, "name.policy");
    w.name = true;
    return;
  }
  // glossary first
  if (o.glossary)
    for (const rules::GlossaryEntry& g : *o.glossary) {
      if (text::en_key(g.name) != text::en_key(n.name)) continue;
      if (g.policy == "keep" || g.form.empty()) { w.form = g.form.empty() ? n.name : g.form; flags_.push_back("name-kept"); return; }
      std::string gen;   // derive the genitive for declensions 1 / 2
      const std::string nb = text::greek_bare(g.form);
      std::string out;
      const uint8_t gg = g.gender == "f" ? F : g.gender == "n" ? N : M;
      if (g.declension == 2 && nb.size() > 2) {
        if (declineName(g.form, "", 2, gg, case_, "", out)) { w.form = out; return; }
      } else if (g.declension == 1) {
        if (declineName(g.form, g.form, 1, gg, case_, "", out) && case_ != Gen) { w.form = out; return; }
      }
      w.form = g.form;
      flags_.push_back("name-kept");
      return;
    }
  if (const NameEntry* e = gd_.nameByEnglish(n.name)) {
    std::string out;
    if (e->policy != curated::NamePolicy::Keep && n.number == Sg &&
        declineName(e->nom, e->gen, e->declension, e->gender, case_, e->voc, out)) {
      w.form = out;
      Features f; f.pos = Name; f.case_ = case_; f.number = Sg; f.gender = e->gender;
      w.packed = pack(f);
      return;
    }
    w.form = e->nom;
    flags_.push_back("name-kept");
    return;
  }
  w.form = n.name;
  flags_.push_back("name-guessed");
}

void GreekRealiser::np(const GrcNP& n, uint8_t case_, bool afterPrep, bool predicate, const GrcOptions& o,
                       std::vector<GWord>& out) {
  Agree a;
  {
    GrcNP single = n;
    single.coord.clear();
    a = ofNP(single);
  }
  a.case_ = case_;
  if (!n.literal.empty()) {
    GWord w;
    w.form = "[" + n.literal + "]";
    w.unknown = true;
    w.rule = "unknown";
    out.push_back(std::move(w));
  } else if (n.isPronoun) {
    GWord w;
    pronounWord(n, case_, afterPrep, w);
    out.push_back(std::move(w));
  } else {
    const bool voc = case_ == Voc;
    const bool definite = (n.definite || n.dem != Demonstrative::None || n.possEmphatic) && !voc &&
                          !(predicate && !n.forceArticle);
    auto articleWord = [&]() {
      GWord w;
      std::string f;
      article(case_, a.number, a.gender, f);
      w.form = f;
      w.lemma = k_.art;
      Features ft; ft.pos = Article; ft.case_ = case_; ft.number = a.number; ft.gender = a.gender;
      w.packed = pack(ft);
      w.rule = "order.art";
      return w;
    };
    if (n.quantifier != kNone) { GrcAdj q; q.lemma = n.quantifier; adjWord(q, a, "order.quant", out); }
    if (n.dem != Demonstrative::None) {
      GrcAdj d; d.lemma = n.dem == Demonstrative::Houtos ? k_.houtos : k_.ekeinos;
      if (d.lemma != kNone) adjWord(d, a, "order.dem", out);
    }
    if (n.interrogative != kNone) { GrcAdj q; q.lemma = n.interrogative; adjWord(q, a, "order.wh", out); }
    if (definite) out.push_back(articleWord());
    if (n.numeral != kNone) { GrcAdj q; q.lemma = n.numeral; adjWord(q, a, "order.num", out); }
    if (n.possEmphatic && n.possPerson) {
      const uint32_t p = n.possPerson == 1 ? (n.possNumber == Pl ? k_.hemeteros : k_.emos)
                                           : (n.possNumber == Pl ? k_.humeteros : k_.sos);
      if (p != kNone) { GrcAdj q; q.lemma = p; adjWord(q, a, "order.poss", out); }
    }
    // attributive adjectives: definite -> between article and noun (one) or repeated article after the noun (two+);
    // indefinite -> after the noun, except quantity / negative / interrogative words
    std::vector<const GrcAdj*> pre, post;
    for (const GrcAdj& ad : n.adjectives) {
      if (beforeNoun(ad.lemma)) pre.push_back(&ad);
      else if (definite && n.adjectives.size() == 1) pre.push_back(&ad);
      else if (!definite && n.adjFirst) pre.push_back(&ad);
      else post.push_back(&ad);
    }
    for (const GrcAdj* ad : pre) adjWord(*ad, a, "order.adj", out);
    if (n.genFirst)   // "Ἄρεως ἡμέρα": the genitive before the noun, without its article
      for (const GrcNP& g : n.genitive) {
        GrcNP x = g;
        x.definite = false;
        np(x, Gen, false, false, o, out);
      }
    // head
    if (n.isName) {
      GWord w;
      nameWord(n, case_, o, w);
      out.push_back(std::move(w));
    } else if (n.head != kNone) {
      const lex::Lemma l = lx_.lemma(n.head);
      Features f;
      if (l.pos == Noun || l.pos == Name) f = nounForm(case_, a.number);
      else f = adjForm(case_, a.number, a.gender);
      GWord w;
      form(n.head, f, w, "order.art");
      if (o.emoji && (l.pos == Noun)) {
        if (const curated::EmojiEntry* e = cd_.emojiGreek(l.key)) w.emoji = e->emoji;
      }
      out.push_back(std::move(w));
    }
    for (const GrcAdj* ad : post) {
      if (definite) out.push_back(articleWord());
      adjWord(*ad, a, "order.adj", out);
    }
    if (n.possPerson && !n.possEmphatic) {
      GrcNP p;
      p.isPronoun = true;
      p.pron.person = n.possPerson;
      p.pron.number = n.possNumber;
      p.pron.gender = n.possGender;
      GWord w;
      pronounWord(p, Gen, false, w);
      w.rule = "order.poss";
      out.push_back(std::move(w));
    }
    if (!n.genFirst)
      for (const GrcNP& g : n.genitive) np(g, Gen, false, false, o, out);
    for (const GrcClause& rc : n.relative) {
      Ctx rctx;
      rctx.main = false;
      rctx.relative = true;
      rctx.ante = a;
      rctx.ante.person = 3;
      std::vector<GWord> rw;
      clause(rc, o, rw, rctx);
      if (!out.empty()) out.back().punctAfter = out.back().punctAfter;   // no comma before a restrictive relative
      append(out, rw);
    }
  }
  for (const GrcNP& c : n.coord) {
    GWord k;
    literal(k_.kai, "καί", k, "order.decl");
    out.push_back(std::move(k));
    np(c, case_, afterPrep, predicate, o, out);
  }
}

// ---- verbs ------------------------------------------------------------------------------------------------------------
void GreekRealiser::verbGroup(const GrcClause& c, const Agree& subj, std::vector<GWord>& fin, std::vector<GWord>& inf,
                              const Ctx& ctx) {
  const GrcPredicate& p = c.pred;
  if (p.lemma == kNone && p.modal == kNone) return;
  const uint8_t voice = p.voice ? p.voice : (uint8_t)Active;
  const uint8_t mood = ctx.forceMood ? ctx.forceMood : p.mood;
  auto aspect = [](uint8_t t) -> uint8_t { return t == Aorist ? (uint8_t)Aorist : t == Perfect ? (uint8_t)Perfect : (uint8_t)Present; };
  auto push = [&](std::vector<GWord>& dst, uint32_t lemma, const Features& f, const char* rule) {
    GWord w;
    if (lemma == kNone) { w.form = "[verb]"; w.missing = true; w.rule = rule; dst.push_back(std::move(w)); return; }
    form(lemma, f, w, rule);
    // C18: a verb whose table has no aorist (ὑλακτέω: the present system only): the imperfect for a past event in the
    // indicative (narrative imperfect), the present stem in the other moods
    if (w.missing && f.tense == Aorist) {
      Features g = f;
      g.tense = f.mood == Indicative ? (uint8_t)Imperfect : (uint8_t)Present;
      GWord w2;
      form(lemma, g, w2, rule);
      if (!w2.missing) w = std::move(w2);
    }
    dst.push_back(std::move(w));
  };
  if (ctx.participle) {   // C18: circumstantial participle agreeing with the main clause's subject (ctx.ante)
    GWord w;
    w.lemma = p.lemma;
    w.rule = "order.ptc";
    GenInfo gi;
    std::string f;
    const uint8_t t = p.tense == Aorist ? (uint8_t)Aorist : p.tense == Perfect ? (uint8_t)Perfect : (uint8_t)Present;
    if (p.lemma != kNone && participle(lx_, p.lemma, t, p.voice, ctx.ante.case_ ? ctx.ante.case_ : (uint8_t)Nom,
                                       ctx.ante.number, ctx.ante.gender, f, &gi)) {
      w.form = f;
      w.packed = gi.packed;
      w.fromRule = gi.fromRule;
    } else {
      w.form = "[verb]";
      w.missing = true;
    }
    fin.push_back(std::move(w));
    return;
  }
  if (c.type == ClauseType::Imp && ctx.main) {
    const uint8_t num = subj.number;
    if (c.polarity == Polarity::Neg && p.tense == Aorist) {   // μή + aorist subjunctive
      push(fin, p.lemma, verbForm(P2, num, Aorist, Subjunctive, voice), "order.prohib");
    } else {
      push(fin, p.lemma, imperative(num, aspect(p.tense), voice), c.polarity == Polarity::Neg ? "order.prohib" : "order.imp");
    }
    return;
  }
  if (ctx.infinitival) {
    const uint8_t t = p.tense == Imperfect ? (uint8_t)Present : p.tense == Pluperfect ? (uint8_t)Perfect : p.tense;
    if (p.modal != kNone) {
      push(fin, p.modal, infinitive(t, Active), "order.acc.inf");
      push(inf, p.lemma, infinitive(p.infTense, p.infVoice ? p.infVoice : (uint8_t)Active), "order.inf");
    } else {
      push(fin, p.lemma, infinitive(t, voice), "order.acc.inf");
    }
    return;
  }
  uint8_t person = p.person ? p.person : subj.person;
  uint8_t number = p.number ? p.number : subj.number;
  uint8_t tense = p.tense;
  if (mood == Subjunctive) tense = aspect(tense) == Perfect ? (uint8_t)Present : aspect(tense);
  if (p.modal != kNone) {
    const bool impersonal = p.modal == k_.dei || p.modal == k_.exesti || impersAccInf(lx_, gd_, p.modal);
    const uint8_t mt = p.modalTense ? p.modalTense : tense;
    if (impersonal) push(fin, p.modal, verbForm(P3, Sg, mt, mood, Active), "order.inf");
    else push(fin, p.modal, verbForm(person, number, mt, mood, Active), "order.inf");
    push(inf, p.lemma, infinitive(p.infTense, p.infVoice ? p.infVoice : (uint8_t)Active), "order.inf");
    return;
  }
  push(fin, p.lemma, verbForm(person, number, tense, mood, voice), "order.decl");
}

// ---- clauses ----------------------------------------------------------------------------------------------------------
void GreekRealiser::clause(const GrcClause& c, const GrcOptions& o, std::vector<GWord>& out, const Ctx& ctx) {
  std::array<std::vector<GWord>, kSlotCount> s;
  std::vector<GWord> intj, conn2, polite;
  const char* orderRule = "order.decl";

  // subject agreement
  Agree subj;
  if (ctx.relative && c.relRole == Role::Subject) subj = ctx.ante;
  else if (c.hasSubject) subj = ofNP(c.subject);
  if (c.type == ClauseType::Imp) {
    subj.person = 2;
    uint8_t num = c.pred.number;
    if (!num) {
      num = Sg;
      for (const GrcNP& v : c.vocatives)
        if (v.number == Pl || !v.coord.empty()) num = Pl;
      if (c.hasSubject && c.subject.isPronoun && c.subject.pron.number == Pl) num = Pl;
    }
    subj.number = num;
  }
  // neuter plural subject: singular verb (agree.neut.pl)
  Agree verbAgree = subj;
  if (c.hasSubject && !(ctx.relative && c.relRole == Role::Subject) && subj.number == Pl && subj.gender == N &&
      subj.person == 3 && c.subject.coord.empty())
    verbAgree.number = Sg;
  if (ctx.relative && c.relRole == Role::Subject && subj.number == Pl && subj.gender == N) verbAgree.number = Sg;
  if (c.pred.person) verbAgree.person = c.pred.person;
  if (c.pred.number) verbAgree.number = c.pred.number;

  const bool impersDei = c.pred.modal != kNone && (c.pred.modal == k_.dei || impersAccInf(lx_, gd_, c.pred.modal));
  const bool impersExesti = c.pred.modal != kNone && c.pred.modal == k_.exesti;
  const uint8_t subjCase = (ctx.infinitival || impersDei) ? (uint8_t)Acc : impersExesti ? (uint8_t)Dat : (uint8_t)Nom;

  // interjections, vocatives, connectors
  for (uint32_t ij : c.interjections) {
    GWord w;
    literal(ij, "?", w, "order.excl");
    intj.push_back(std::move(w));
  }
  for (const GrcNP& v : c.vocatives) {
    const bool bare = v.isPronoun || v.quantifier != kNone ||
                      (v.head != kNone && (lx_.lemma(v.head).key == "πᾶσ"));
    if (!bare) { GWord w; literal(k_.o, "ὦ", w, "order.voc"); s[kVOC].push_back(std::move(w)); }
    np(v, Voc, false, false, o, s[kVOC]);
  }
  for (uint32_t k : c.connectors) {
    GWord w;
    const lex::Lemma l = lx_.lemma(k);
    const bool second = l.id != kNone && inKeys(secondKeys_, l.key);
    literal(k, "?", w, second ? "order.conn.second" : "order.conn.first");
    if (second && isEncliticForm(w.form)) w.enclitic = true;
    (second ? conn2 : s[kCONN]).push_back(std::move(w));
  }
  // subject
  const bool pronDrop = c.hasSubject && c.subject.isPronoun && !c.subject.emphasis && !impersDei && !impersExesti;
  const bool relSubj = ctx.relative && c.relRole == Role::Subject;
  if (c.hasSubject && !pronDrop && !relSubj && !c.exclHos) {
    std::vector<GWord> w;
    np(c.subject, subjCase, false, false, o, w);
    // ἔξεστι: the person is a dative and stands where an indirect object stands (order.exesti)
    append(c.subject.interrogative != kNone ? s[kWH] : impersExesti ? s[kIO] : s[kS], w);
  }
  const bool middle = (c.pred.voice == Middle) || middleLemma(c.pred.lemma);
  // objects
  if (c.hasObject && !(ctx.relative && c.relRole == Role::Object)) {
    const uint8_t oc = c.object.case_ ? c.object.case_ : objectCase(c.pred.lemma, middle);
    std::vector<GWord> w;
    np(c.object, oc, false, false, o, w);
    append(c.object.interrogative != kNone ? s[kWH] : s[kO], w);
  }
  if (c.hasIndirect && !(ctx.relative && c.relRole == Role::IndirectObject))
    np(c.indirect, c.indirect.case_ ? c.indirect.case_ : (uint8_t)Dat, false, false, o, s[kIO]);
  // obliques
  for (const GrcOblique& ob : c.obliques) {
    const uint8_t oc = ob.np.case_ ? ob.np.case_ : ob.prep == kNone ? (ob.case_ ? ob.case_ : (uint8_t)Dat)
                                                                   : prepCase(ob.prep, ob.case_);
    std::vector<GWord> w;
    if (ob.prep != kNone) {
      GWord pw;
      literal(ob.prep, "?", pw, "order.prep");
      w.push_back(std::move(pw));
    }
    np(ob.np, oc, ob.prep != kNone, false, o, w);
    const bool wh = ob.np.interrogative != kNone || (ob.np.head != kNone && ob.np.head == k_.tis);
    append(wh ? s[kWH] : ob.front ? s[kFRONT] : ob.end ? s[kEND] : s[kOBL], w);
  }
  // adverbs
  bool frontGiven = false;
  for (const GrcAdverb& ad : c.adverbs) {
    GWord w;
    const lex::Lemma al = lx_.lemma(ad.lemma);
    if (al.id != kNone && (al.pos == Adj || al.pos == Num)) {   // C16: the adverb of an adjective (πρῶτον, ἡδέως)
      Features af;
      af.pos = Adv;
      form(ad.lemma, af, w, "order.adv");
      if (w.missing) literal(ad.lemma, "?", w, "order.adv");
    } else {
      literal(ad.lemma, "?", w, "order.adv");
    }
    frontGiven = frontGiven || ad.pos == AdvPos::Front;
    if (ad.pos == AdvPos::Front || (ad.pos == AdvPos::Auto && timeAdverb(ad.lemma))) s[kFRONT].push_back(std::move(w));
    else if (ad.pos == AdvPos::End) s[kEND].push_back(std::move(w));
    else s[kADV].push_back(std::move(w));
  }
  // copula predicates
  const bool isEimi = c.pred.lemma != kNone && c.pred.lemma == k_.eimi && c.pred.modal == kNone;
  const bool isCopula = c.pred.lemma != kNone && c.pred.modal == kNone && (!c.predicative.empty() || !c.predAdj.empty());
  const bool exist = c.existential && !isCopula;   // order.exist: V S (location first); orthotone ἔστι for εἰμί
  const uint8_t predCase = ctx.infinitival ? (uint8_t)Acc : (uint8_t)Nom;
  bool whPred = false;
  for (const GrcNP& pn : c.predicative) {
    std::vector<GWord> w;
    whPred = whPred || pn.interrogative != kNone;
    np(pn, predCase, false, true, o, w);
    append(pn.interrogative != kNone ? s[kWH] : s[kPRED], w);
  }
  if (!c.predAdj.empty()) {
    Agree pa = subj;
    pa.case_ = predCase;
    if (c.predGender) pa.gender = c.predGender;
    if (c.predNumber) pa.number = c.predNumber;
    for (size_t i = 0; i < c.predAdj.size(); ++i) {
      if (i) { GWord k; literal(k_.kai, "καί", k, "order.copula"); s[kPRED2].push_back(std::move(k)); }
      adjWord(c.predAdj[i], pa, "order.copula", i ? s[kPRED2] : s[kPRED]);
    }
  }
  // wh word
  if ((c.type == ClauseType::Wh || c.wh.lemma != kNone) && c.wh.lemma != kNone) {
    GWord w;
    const lex::Lemma wl = lx_.lemma(c.wh.lemma);
    if (c.wh.role == Role::None || wl.pos == Adv) literal(c.wh.lemma, "?", w, "order.wh");
    else {
      const uint8_t cs = c.wh.role == Role::Object ? objectCase(c.pred.lemma, middle)
                         : c.wh.role == Role::IndirectObject ? (uint8_t)Dat : (uint8_t)Nom;
      std::string f;
      closedForm("τίσ", cs, c.wh.number ? c.wh.number : (uint8_t)Sg, c.wh.gender ? c.wh.gender : (uint8_t)M, false, f);
      w.form = f;
      w.lemma = k_.tis;
      Features ft; ft.pos = Pron; ft.case_ = cs; ft.number = c.wh.number ? c.wh.number : (uint8_t)Sg;
      ft.gender = c.wh.gender ? c.wh.gender : (uint8_t)M;
      w.packed = pack(ft);
      w.rule = "order.wh";
    }
    w.interrogative = true;
    s[kWH].insert(s[kWH].begin(), std::move(w));
  }
  // relative pronoun
  if (ctx.relative && c.relRole != Role::None) {
    uint8_t rc = Nom;
    if (c.relRole == Role::Object) rc = objectCase(c.pred.lemma, middle);
    else if (c.relRole == Role::IndirectObject) rc = Dat;
    else if (c.relRole == Role::Oblique) rc = prepCase(c.relPrep, 0);
    GWord w;
    std::string f;
    closedForm("ὅσ", rc, ctx.ante.number, ctx.ante.gender, false, f);
    w.form = f;
    w.lemma = k_.hos;
    Features ft; ft.pos = Pron; ft.case_ = rc; ft.number = ctx.ante.number; ft.gender = ctx.ante.gender;
    w.packed = pack(ft);
    w.rule = "order.rel";
    std::vector<GWord> rel;
    if (c.relRole == Role::Oblique && c.relPrep != kNone) { GWord pw; literal(c.relPrep, "?", pw, "order.rel"); rel.push_back(std::move(pw)); }
    rel.push_back(std::move(w));
    for (GWord& x : s[kWH]) rel.push_back(std::move(x));
    s[kWH] = std::move(rel);
  }
  // verb group
  verbGroup(c, verbAgree, s[kV], s[kINF], ctx);
  if (exist && isEimi && c.wh.lemma == kNone)
    for (GWord& w : s[kV]) w.existential = true;
  // negation
  const bool prohib = c.type == ClauseType::Imp && c.polarity == Polarity::Neg;
  bool hasNegWord = false;
  {
    auto scan = [&](const std::vector<GWord>& v) { for (const GWord& w : v) hasNegWord = hasNegWord || negativeWord(w.lemma); };
    for (int sl : {kS, kO, kIO, kOBL, kADV, kFRONT, kPRED, kWH}) scan(s[sl]);
  }
  const bool yesBias = c.type == ClauseType::Yn && c.bias == YnBias::ExpectYes;
  if ((c.polarity == Polarity::Neg && !hasNegWord) || yesBias) {
    const bool useMe = prohib || ctx.negMe || (c.type == ClauseType::Imp && ctx.main);
    GWord w;
    if (useMe) literal(k_.me, "μή", w, prohib ? "order.prohib" : "neg.me");
    else literal(k_.ou, "οὐ", w, yesBias ? "order.yn.ou" : "neg.ou");
    w.proclitic = !useMe;
    s[kNEG].push_back(std::move(w));
  }

  // C18: circumstantial participles: realised here, placed after the subject (or first when the subject is dropped)
  for (const GrcSub& sub : c.subs) {
    if (!sub.participle || sub.clause.empty()) continue;
    Ctx pctx;
    pctx.main = false;
    pctx.participle = true;
    pctx.negMe = false;
    pctx.ante = subj;
    pctx.ante.case_ = subjCase;
    if (pctx.ante.person <= 2 && pctx.ante.number == Pl) pctx.ante.gender = M;   // "we" / "you" (a group): masculine
    std::vector<GWord> body;
    clause(sub.clause[0], o, body, pctx);
    append(s[kPTC], body);
  }
  // ---- linearise ----
  std::vector<int> seq;
  auto addTemplate = [&](const std::vector<std::string>& t) {
    for (const std::string& nm : t) {
      const int sl = slotOf(nm);
      if (sl < 0 || sl == kVOC || sl == kCONN) continue;
      if (std::find(seq.begin(), seq.end(), sl) == seq.end()) seq.push_back(sl);
    }
  };
  auto tmpl = [&](const char* id, std::initializer_list<const char*> def) {
    std::vector<std::string> t = gd_.slotTemplate(id);
    if (t.empty())
      for (const char* d : def) t.emplace_back(d);
    return t;
  };
  std::vector<GWord> content;
  if (c.type == ClauseType::Excl && c.exclHos) {
    GWord lead;
    literal(k_.hos2, "ὡς", lead, "order.excl");
    content.push_back(std::move(lead));
    if (c.hasSubject) {
      Agree sa = ofNP(c.subject);
      sa.case_ = Nom;
      for (const GrcAdj& ad : c.subject.adjectives) adjWord(ad, sa, "order.excl", content);
      GrcNP rest = c.subject;
      rest.adjectives.clear();
      rest.definite = true;
      np(rest, Nom, false, false, o, content);
    }
    append(content, s[kPRED]);
    append(content, s[kV]);
    orderRule = "order.excl";
  } else if (c.type == ClauseType::Frag) {
    for (int sl : {kFRONT, kWH, kS, kPTC, kPRED, kIO, kO, kOBL, kADV, kNEG, kV, kINF, kPRED2, kEND}) append(content, s[sl]);
  } else if (c.type == ClauseType::Imp && ctx.main && frontGiven) {
    // C16 (order.imp): a fronted sequence adverb puts the imperative last ("πρῶτον τὸ ὄνομά σου γράφε")
    orderRule = prohib ? "order.prohib" : "order.imp";
    seq = {kFRONT, kIO, kO, kOBL, kADV, kNEG, kV, kINF, kPRED, kPRED2, kEND};
  } else if (c.type == ClauseType::Imp && ctx.main) {
    orderRule = prohib ? "order.prohib" : "order.imp";
    seq = {kFRONT, kNEG, kV};
    addTemplate(tmpl("order.imp", {"VOC", "V", "IO", "O", "OBL", "ADV"}));
    for (int sl : {kINF, kPRED, kPRED2, kEND}) seq.push_back(sl);
  } else if (c.type == ClauseType::Wh && isCopula && whPred) {
    // C16 (order.wh.cop): an interrogative predicate takes the copula right after it ("τίς ἡμέρα ἐστὶ σήμερον;")
    orderRule = "order.wh";
    seq = {kWH, kNEG, kV, kFRONT, kS, kPRED, kADV, kPRED2, kEND};
  } else {
    const char* id = exist ? "order.exist" : isCopula ? "order.copula" : impersDei ? "order.dei"
                     : impersExesti ? "order.exesti" : c.pred.modal != kNone ? "order.inf" : "order.decl";
    orderRule = id;
    seq.push_back(kWH);
    seq.push_back(kFRONT);
    if (exist || c.verbFirst) addTemplate(tmpl("order.exist", {"CONN", "OBL", "V", "S"}));
    else if (isCopula) addTemplate(tmpl(id, {"VOC", "CONN", "S", "PRED", "NEG", "V"}));
    else if (impersDei) addTemplate(tmpl(id, {"CONN", "NEG", "V", "S", "O", "OBL", "INF"}));
    else if (impersExesti) addTemplate(tmpl(id, {"CONN", "NEG", "V", "IO", "O", "OBL", "INF"}));
    else if (c.pred.modal != kNone && c.type == ClauseType::Yn && c.ara && c.bias == YnBias::Neutral) {
      // C16 (order.yn.inf): ἆρα + the modal first, the infinitive after the subject and the dative pronoun
      // ("ἆρα δύνανται αἱ γαλαῖ μειδιᾶν;", "ἆρα δύνασαί μοι βοηθεῖν περὶ τῆς ἐπιστολῆς;")
      orderRule = "order.yn";
      // an enclitic pronoun object (μοι, σε) goes with the modal, before the infinitive
      if (s[kO].size() == 1 && s[kO][0].enclitic) {
        s[kIO].insert(s[kIO].begin(), std::move(s[kO][0]));
        s[kO].clear();
      }
      addTemplate(tmpl("order.yn.inf", {"VOC", "CONN", "NEG", "V", "S", "IO", "INF", "O", "OBL", "ADV"}));
    } else if (c.pred.modal != kNone) addTemplate(tmpl(id, {"VOC", "CONN", "S", "NEG", "V", "IO", "O", "OBL", "ADV", "INF"}));
    else if (c.type == ClauseType::Yn && c.ara && c.bias == YnBias::Neutral) {   // decision 4: ἆρα + verb first
      orderRule = "order.yn";
      addTemplate(tmpl("order.yn", {"VOC", "CONN", "NEG", "V", "S", "IO", "O", "OBL", "ADV"}));
    } else addTemplate(tmpl(id, {"VOC", "CONN", "S", "IO", "O", "OBL", "ADV", "NEG", "V"}));
  }
  if (content.empty()) {
    for (int sl : {kWH, kFRONT, kS, kPTC, kIO, kO, kOBL, kADV, kPRED, kINF, kNEG, kV, kPRED2}) {
      if (s[sl].empty() || std::find(seq.begin(), seq.end(), sl) != seq.end()) continue;
      auto pos = seq.end();
      if (sl == kPRED2) {
        auto v = std::find(seq.begin(), seq.end(), kV);
        pos = v == seq.end() ? seq.end() : v + 1;
      } else {
        for (auto it = seq.begin(); it != seq.end(); ++it)
          if (canonIndex(*it) > canonIndex(sl)) { pos = it; break; }
      }
      seq.insert(pos, sl);
    }
    if (std::find(seq.begin(), seq.end(), kEND) == seq.end()) seq.push_back(kEND);
    for (int sl : seq)
      if (sl >= 0 && sl < kSlotCount) append(content, s[sl]);
  }
  // yes/no: ἆρα (optional) / ἆρα μή (expects no)
  if (c.type == ClauseType::Yn && (c.ara || c.bias == YnBias::ExpectNo)) {
    std::vector<GWord> lead;
    GWord a;
    literal(k_.ara, "ἆρα", a, "order.yn");
    lead.push_back(std::move(a));
    if (c.bias == YnBias::ExpectNo) { GWord m; literal(k_.me, "μή", m, "order.yn.me"); lead.push_back(std::move(m)); }
    content.insert(content.begin(), lead.begin(), lead.end());
  }
  // C16: the modal particle ἄν after the negation (moved to the front: "οὐκ ἂν ἐνθάδε ἦσθα") or the first word
  if (c.an && !content.empty()) {
    GWord an;
    literal(findLemma(lx_, "ἄν", Particle) != kNone ? findLemma(lx_, "ἄν", Particle) : findLemma(lx_, "ἄν"), "ἄν", an,
            "mood.an");
    std::vector<GWord> neg, rest;
    for (GWord& w : content)
      ((w.lemma == k_.ou && k_.ou != kNone) || (w.lemma == k_.me && k_.me != kNone) ? neg : rest).push_back(std::move(w));
    content.clear();
    if (!neg.empty()) {
      append(content, neg);
      content.push_back(std::move(an));
      append(content, rest);
    } else {
      append(content, rest);
      content.insert(content.begin() + 1, std::move(an));
    }
  }
  // second-position particles: after the first word of the clause (after "διὰ τί" as a whole: "διὰ τί οὖν")
  if (!conn2.empty()) {
    size_t at = content.empty() ? 0 : 1;
    if (content.size() >= 2 && content[1].lemma == k_.tis && k_.tis != kNone && lx_.lemma(content[0].lemma).pos == Prep)
      at = 2;
    for (size_t i = 0; i < conn2.size(); ++i) content.insert(content.begin() + (long)(at + i), std::move(conn2[i]));
  }
  std::vector<GWord> clauseWords;
  for (GWord& w : intj) w.punctAfter = ",";
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
    for (uint32_t pl : c.politeness) {
      GWord w;
      const lex::Lemma l = lx_.lemma(pl);
      if (l.id != kNone && l.pos == Verb) form(pl, verbForm(P1, Sg, Present), w, "polite");
      else literal(pl, "?", w, "polite");
      polite.push_back(std::move(w));
    }
    append(clauseWords, polite);
  }

  // subordinate clauses
  std::vector<GWord> before, after;
  for (const GrcSub& sub : c.subs) {
    if (sub.clause.empty() || sub.participle) continue;
    const GrcClause& sc = sub.clause[0];
    std::vector<GWord> sw;
    uint32_t conj = sub.conj;
    if (conj == kNone) {
      switch (sub.rel) {
        case SubRel::Cause: conj = k_.hoti; break;
        case SubRel::Time: conj = k_.epei; break;
        case SubRel::Condition: conj = sc.pred.mood == Subjunctive ? k_.ean : k_.ei; break;
        case SubRel::Purpose: conj = k_.hina; break;
        case SubRel::Result: conj = k_.hoste; break;
        case SubRel::Coord: conj = sub.noConj ? kNone : k_.kai; break;
        default: break;
      }
    }
    if (sub.noConj && !sub.before) {   // C18: "..., ὀψὲ γάρ ἐστιν": a comma before the clause with its own particle
      std::vector<GWord>& prev = after.empty() ? clauseWords : after;
      if (!prev.empty() && prev.back().punctAfter.empty()) prev.back().punctAfter = ",";
    }
    Ctx sctx;
    sctx.main = false;
    if (sub.otherwise) {   // C16: "εἰ δὲ μή," + the clause (an "or else" before a counterfactual)
      for (const char* h : {"εἰ", "δέ", "μή"}) {
        GWord w;
        const uint32_t id = std::string(h) == "εἰ" ? k_.ei : std::string(h) == "μή" ? k_.me
                                                                       : findLemma(lx_, h, Particle);
        literal(id, h, w, "order.sub.otherwise");
        if (std::string(h) == "δέ") w.enclitic = false;
        sw.push_back(std::move(w));
      }
      sw.back().punctAfter = ",";
      conj = kNone;
    }
    if (conj != kNone) {
      GWord w;
      literal(conj, "?", w, sub.before ? "order.sub.pre" : sub.rel == SubRel::Purpose ? "order.sub.purp"
                                                         : sub.rel == SubRel::Result ? "order.result" : "order.sub.cause");
      sw.push_back(std::move(w));
    }
    if (sub.rel == SubRel::Purpose || conj == k_.ean || conj == k_.hopos) {
      sctx.forceMood = Subjunctive;
      sctx.negMe = true;
    }
    if (sub.rel == SubRel::Condition) sctx.negMe = true;
    if (sub.rel == SubRel::Result && !sub.finite) { sctx.infinitival = true; sctx.negMe = true; }   // C18: finite = οὐ
    if (sub.rel == SubRel::AccInf) sctx.infinitival = true;
    if (sub.rel == SubRel::Coord && sc.type == ClauseType::Imp) sctx.main = true;
    std::vector<GWord> body;
    clause(sc, o, body, sctx);
    append(sw, body);
    if (sub.before) {
      if (!sw.empty()) sw.back().punctAfter = ",";
      append(before, sw);
    } else append(after, sw);
  }
  append(out, before);
  append(out, clauseWords);
  append(out, after);
}

// ---- output -----------------------------------------------------------------------------------------------------------
void GreekRealiser::finish(const GrcOptions& o, std::vector<GWord>& words, const std::string& finalPunct,
                           GrcSentence& out) {
  sw_.clear();
  for (const GWord& w : words) {
    SandhiWord s;
    s.form = w.form;
    s.enclitic = w.enclitic;
    s.proclitic = w.proclitic;
    s.movableNu = w.movableNu;
    s.existential = w.existential;
    s.interrogative = w.interrogative;
    s.punctAfter = w.punctAfter;
    sw_.push_back(std::move(s));
  }
  if (!sw_.empty()) sw_.back().punctAfter += finalPunct;
  SandhiOptions so;
  so.elision = o.elision;
  so.autoEnclitic = false;
  sandhi(sw_, so);
  for (size_t i = 0; i < words.size(); ++i) {
    GWord& w = words[i];
    if (i) out.text += ' ';
    rules::TokenView t;
    t.text = t.display = sw_[i].form;
    t.start = (int)out.text.size();
    out.text += t.text;
    t.end = (int)out.text.size();
    if (w.lemma != kNone) {
      t.lemmaId = w.lemma;
      t.hasLemma = true;
      const lex::Lemma l = lx_.lemma(w.lemma);
      uint8_t tier = l.tier;
      if (const curated::TierEntry* te = cd_.tierGreek(l.key))
        if (te->tier && (!tier || te->tier < tier)) tier = te->tier;
      t.tier = tier;
    }
    t.features = realise::featureView(w.packed);
    t.emoji = w.emoji;
    t.unknown = w.unknown;
    t.fromRule = w.fromRule;
    if (o.emojiInText && !w.emoji.empty()) { out.text += ' '; out.text += w.emoji; }
    out.text += i + 1 < words.size() ? w.punctAfter : sw_[i].punctAfter;
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

void GreekRealiser::realise(const GrcClause& c, const GrcOptions& o, GrcSentence& out) {
  out.clear();
  words_.clear();
  flags_.clear();
  clause(c, o, words_, Ctx{});
  std::string p = c.punct;
  if (p == "?") p = ";";
  else if (p == ":") p = "\xC2\xB7";
  if (p.empty()) p = (c.type == ClauseType::Yn || c.type == ClauseType::Wh) ? ";" : c.type == ClauseType::Excl ? "!" : ".";
  finish(o, words_, p, out);
  for (const std::string& f : flags_)
    if (std::find(out.flags.begin(), out.flags.end(), f) == out.flags.end()) out.flags.push_back(f);
  words_.clear();
}

}  // namespace vp::grc
