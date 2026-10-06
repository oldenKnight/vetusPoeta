// FormSelector, Agreement, CaseAssigner, Negation, Pronouns, Macrons, Punctuation, Emoji, featureView.
#include <algorithm>

#include "vp/realise_la.h"
#include "vp/text.h"

namespace vp::realise {

using namespace vp::feat;

namespace {
bool invariablePos(uint8_t pos) {
  switch (pos) {
    case Adv: case Conj: case Prep: case Intj: case Particle: case Phrase: case Symbol: case Punct: case Postp:
      return true;
    default: return false;
  }
}
}  // namespace

// ---- FormSelector -------------------------------------------------------------------------------------------------
void FormSelector::invariable(uint32_t lemma, Word& out) const {
  const lex::Lemma l = lx_.lemma(lemma);
  out.lemma = lemma;
  out.packed = 0;
  if (l.id == lex::kNoLemma) { out.form = "[?]"; out.missing = true; return; }
  out.form = morph::displayForm(l.head, true);
  Features f; f.pos = l.pos;
  out.packed = pack(f);
}

bool FormSelector::select(uint32_t lemma, const Features& f, Word& out) const {
  out.lemma = lemma;
  out.missing = false;
  out.fromRule = false;
  const lex::Lemma l = lx_.lemma(lemma);
  if (l.id == lex::kNoLemma) { out.form = "[?]"; out.missing = true; return false; }
  if (invariablePos(l.pos)) { invariable(lemma, out); return true; }
  morph::GenInfo gi;
  std::string form;
  if (morph::generate(lx_, lemma, f, form, true, &gi)) {
    // domus: the lexicon lists the 4th-declension ablative domū first; the readers use domō (C2b)
    if (l.key == "domus" && f.case_ == Abl && f.number == Sg && text::latin_key(form) == "domu") form = "domō";
    out.form = std::move(form);
    out.packed = gi.packed ? gi.packed : pack(f);
    out.fromRule = gi.fromRule;
    return true;
  }
  // Indeclinables (quattuor, quot) and words without cells keep their headword.
  thread_local std::vector<std::pair<uint32_t, std::string_view>> cells;
  cells.clear();
  lx_.cells(lemma, cells);
  if ((l.flags & lex::Indeclinable) || cells.empty()) {
    if (cells.empty() && (l.pos == Noun || l.pos == Adj || l.pos == Verb) && !(l.flags & lex::Indeclinable)) {
      out.form = "[" + morph::displayForm(l.head, true) + "]";
      out.missing = true;
      return false;
    }
    invariable(lemma, out);
    return true;
  }
  out.form = "[" + morph::displayForm(l.head, true) + "]";
  out.missing = true;
  return false;
}

// ---- Agreement ----------------------------------------------------------------------------------------------------
namespace {
uint8_t simpleGender(uint8_t g) {
  switch (g) {
    case M: case F: case N: return g;
    case FN: return F;
    case 0: return M;
    default: return M;   // MF, MN, MFN: masculine by default
  }
}
}  // namespace

uint8_t Agreement::nounGender(const LaNP& np, const curated::CuratedData& cd) const {
  if (np.gender) return simpleGender(np.gender);
  if (np.isPronoun) return simpleGender(np.pron.gender);
  if (np.isName) {
    if (const curated::NameEntry* n = cd.nameByEnglish(np.name)) return simpleGender(n->gender);
    if (np.head == kNone) return M;
  }
  if (np.head == kNone) return M;
  // C17: a noun of two genders by number (caelum, caelī; locus, loca): the singular follows the nominative ending
  const lex::Lemma l = lx_.lemma(np.head);
  if (l.gender == MN && l.key.size() > 2) {
    const bool um = l.key.compare(l.key.size() - 2, 2, "um") == 0;
    if (np.number == Pl) return M;   // caelī, locī: the lexicon's plural cells are the masculine ones
    return um ? N : M;
  }
  return simpleGender(l.gender);
}

AgreeInfo Agreement::ofNP(const LaNP& np, const curated::CuratedData& cd) const {
  AgreeInfo a;
  a.case_ = np.case_;
  a.gender = nounGender(np, cd);
  a.number = np.number ? np.number : (uint8_t)Sg;
  a.person = np.isPronoun && np.pron.person ? np.pron.person : 3;
  if (np.isPronoun && np.pron.number) a.number = np.pron.number;
  if (!np.coord.empty()) {
    a.number = Pl;
    bool allF = a.gender == F, allN = a.gender == N;
    for (const LaNP& c : np.coord) {
      const AgreeInfo b = ofNP(c, cd);
      if (b.person < a.person) a.person = b.person;
      allF = allF && b.gender == F;
      allN = allN && b.gender == N;
    }
    a.gender = allF ? (uint8_t)F : allN ? (uint8_t)N : (uint8_t)M;
  }
  return a;
}

Features Agreement::modifier(const AgreeInfo& h, uint8_t degree) const {
  return morph::adjForm(h.case_, h.number, h.gender, degree);
}

Features Agreement::finiteVerb(const AgreeInfo& s, const LaPredicate& p) const {
  Features f = morph::verbForm(p.person ? p.person : s.person, p.number ? p.number : s.number, p.tense, p.mood,
                               p.voice);
  f.gender = s.gender;   // for the participle of periphrastic forms
  return f;
}

Features Agreement::relative(const AgreeInfo& a, uint8_t ownCase) const {
  Features f; f.pos = Pron; f.case_ = ownCase; f.number = a.number; f.gender = a.gender;
  return f;
}

// ---- CaseAssigner ---------------------------------------------------------------------------------------------------
uint8_t CaseAssigner::objectCase(uint32_t verb) const {
  const lex::Lemma l = lx_.lemma(verb);
  if (l.id == lex::kNoLemma) return Acc;
  if (const curated::Valency* v = cd_.valency(l.key)) {
    for (const curated::Frame& f : v->frames) {
      switch (f.kind) {
        case curated::FrameKind::Acc: case curated::FrameKind::AccInf: case curated::FrameKind::AccAcc:
        case curated::FrameKind::AccAbl: case curated::FrameKind::DatAcc: return Acc;
        case curated::FrameKind::Dat: return Dat;
        case curated::FrameKind::Abl: return Abl;
        case curated::FrameKind::Gen: return Gen;
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
    if (!acc && (t & (1u << 9))) return Abl;
    if (!acc && (t & (1u << 10))) return Gen;
  }
  return Acc;
}

uint8_t CaseAssigner::prepCase(uint32_t prep, uint8_t override) const {
  if (override) return override;
  const lex::Lemma l = lx_.lemma(prep);
  if (l.id == lex::kNoLemma) return Abl;
  const uint16_t bits = cd_.prepCases(l.key);
  if (bits & (1u << Abl)) return Abl;   // in, sub: static (ablative) unless the caller asks for motion
  for (uint8_t c = Nom; c <= Loc; ++c)
    if (bits & (1u << c)) return c;
  return Abl;
}

void CaseAssigner::assign(LaClause& c, bool accInf) const {
  const uint8_t subj = c.exclO || accInf ? (uint8_t)Acc : (uint8_t)Nom;
  if (c.hasSubject && !c.subject.case_) c.subject.case_ = subj;
  for (LaNP& p : c.predicative)
    if (!p.case_) p.case_ = subj;
  if (c.hasObject && !c.object.case_) c.object.case_ = objectCase(c.pred.lemma);
  if (c.hasIndirect && !c.indirect.case_) c.indirect.case_ = Dat;
  for (LaOblique& o : c.obliques)
    if (!o.np.case_) o.np.case_ = o.prep == kNone ? (o.case_ ? o.case_ : (uint8_t)Abl) : prepCase(o.prep, o.case_);
  for (LaNP& v : c.vocatives)
    if (!v.case_) v.case_ = Voc;
  for (LaSub& s : c.subs)
    for (LaClause& sc : s.clause) assign(sc, s.rel == SubRel::AccInf);
}

// ---- Negation -----------------------------------------------------------------------------------------------------
Negation::Negation(const lex::Lexicon& lx) {
  for (const char* w : {"nūllus", "nēmō", "nihil", "numquam", "nusquam"}) {
    const uint32_t id = morph::findLemma(lx, w);
    if (id != kNone) neg_.push_back(id);
  }
  std::sort(neg_.begin(), neg_.end());
}
bool Negation::negativeWord(uint32_t lemma) const { return std::binary_search(neg_.begin(), neg_.end(), lemma); }
bool Negation::clauseHasNegativeWord(const LaClause& c) const {
  auto inNP = [&](const LaNP& np) {
    if (negativeWord(np.head)) return true;
    for (const LaAdj& a : np.adjectives)
      if (negativeWord(a.lemma)) return true;
    return false;
  };
  if (c.hasSubject && inNP(c.subject)) return true;
  if (c.hasObject && inNP(c.object)) return true;
  if (c.hasIndirect && inNP(c.indirect)) return true;
  for (const LaAdverb& a : c.adverbs)
    if (negativeWord(a.lemma)) return true;
  for (const LaOblique& o : c.obliques)
    if (inNP(o.np)) return true;
  return false;
}

// ---- Pronouns -----------------------------------------------------------------------------------------------------
Pronouns::Pronouns(const lex::Lexicon& lx) {
  ego_ = morph::findLemma(lx, "ego", Pron);
  tu_ = morph::findLemma(lx, "tū", Pron);
  nos_ = morph::findLemma(lx, "nōs", Pron);
  vos_ = morph::findLemma(lx, "vōs", Pron);
  is_ = morph::findLemma(lx, "is", Pron);
  se_ = morph::findLemma(lx, "sē", Pron);
  meus_ = morph::findLemma(lx, "meus", feat::Det);
  tuus_ = morph::findLemma(lx, "tuus", feat::Det);
  noster_ = morph::findLemma(lx, "noster", feat::Det);
  vester_ = morph::findLemma(lx, "vester", feat::Det);
  suus_ = morph::findLemma(lx, "suus", feat::Det);
  if (meus_ == kNone) meus_ = morph::findLemma(lx, "meus");
  if (tuus_ == kNone) tuus_ = morph::findLemma(lx, "tuus");
  if (noster_ == kNone) noster_ = morph::findLemma(lx, "noster");
  if (vester_ == kNone) vester_ = morph::findLemma(lx, "vester");
  if (suus_ == kNone) suus_ = morph::findLemma(lx, "suus");
}
uint32_t Pronouns::personal(uint8_t person, uint8_t number, bool reflexive) const {
  if (person == 1) return number == Pl ? nos_ : ego_;
  if (person == 2) return number == Pl ? vos_ : tu_;
  return reflexive ? se_ : is_;
}
uint32_t Pronouns::possessive(uint8_t person, uint8_t number, bool reflexive) const {
  if (person == 1) return number == Pl ? noster_ : meus_;
  if (person == 2) return number == Pl ? vester_ : tuus_;
  return reflexive ? suus_ : kNone;   // non-reflexive 3rd person: genitive eius / eōrum (caller)
}
bool Pronouns::dropSubject(const LaClause& c) const {
  if (c.type == ClauseType::Imp) return true;
  if (!c.hasSubject || !c.subject.isPronoun || c.subject.emphasis || !c.subject.coord.empty()) return false;
  // pron.drop (c): "Ego sum Alīcia" keeps ego when a copula introduces the speaker by name
  // (rule pron.is: a 3rd-person pronoun is dropped before a name too: "Rēgīna Cordium est")
  if (!c.predicative.empty() && c.predicative[0].isName && !c.predicative[0].indefinite && c.subject.pron.person != 3)
    return false;
  return true;   // pron.drop (ego/tū/nōs/vōs) and pron.is (is/ea/id)
}

// ---- Macrons / Punctuation / Emoji ----------------------------------------------------------------------------------
std::string Macrons::apply(std::string_view form, bool macrons) { return morph::displayForm(form, macrons); }
std::string Macrons::apply(std::string_view form, bool macrons, const curated::MacronOverride* ov) {
  if (!ov || ov->from.empty()) return morph::displayForm(form, macrons);
  std::string f = text::nfc(form);
  std::string from = ov->from, to = ov->to;
  if (f.compare(0, from.size(), from) != 0) {   // a capitalised form ("Narrā" at the start of a sentence)
    Punctuation::capitaliseFirst(from);
    Punctuation::capitaliseFirst(to);
  }
  if (f.compare(0, from.size(), from) == 0) f = to + f.substr(from.size());
  return morph::displayForm(f, macrons);
}

std::string Punctuation::finalMark(const LaClause& c) {
  if (!c.punct.empty()) return c.punct;
  switch (c.type) {
    case ClauseType::Yn: case ClauseType::Wh: return "?";
    case ClauseType::Excl: return "!";
    default: return ".";
  }
}

void Punctuation::capitaliseFirst(std::string& t) {
  if (t.empty()) return;
  size_t i = 0;
  const char32_t c = text::decodeUtf8(t, i);
  char32_t u = c;
  if (c >= U'a' && c <= U'z') u = c - 32;
  else if (c == 0x0101 || c == 0x0113 || c == 0x012B || c == 0x014D || c == 0x016B || c == 0x0233) u = c - 1;
  else if (c >= 0xE0 && c <= 0xFE && c != 0xF7) u = c - 32;
  if (u == c) return;
  std::string up;
  text::appendUtf8(up, u);
  t.replace(0, i, up);
}

std::string_view Emoji::forLemma(uint32_t lemma) const {
  const lex::Lemma l = lx_.lemma(lemma);
  if (l.id == lex::kNoLemma || l.pos != Noun || (l.flags & lex::ProperName)) return {};
  const curated::EmojiEntry* e = cd_.emoji(l.key);
  if (!e) return {};
  // The headword (with macrons) must match: mālum (apple) has one, malum (evil) not; ōs (mouth) not os (bone).
  if (morph::displayForm(e->head, true) != morph::displayForm(l.head, true)) return {};
  // "old woman only when noun fem." (anus)
  if (e->note.find("fem") != std::string::npos && l.gender != F) return {};
  return e->emoji;
}

// ---- featureView --------------------------------------------------------------------------------------------------
rules::Features featureView(uint32_t packed) {
  static const char* const kPos[] = {"",         "noun",     "verb",   "adj",    "adv",     "pron",   "num",
                                     "prep",     "conj",     "intj",   "det",    "name",    "particle", "participle",
                                     "phrase",   "suffix",   "prefix", "article", "postp",  "symbol", "punct"};
  static const char* const kCase[] = {"", "nominative", "genitive", "dative", "accusative", "ablative", "vocative",
                                      "locative"};
  static const char* const kNum[] = {"", "singular", "plural", "dual"};
  static const char* const kGen[] = {"", "masculine", "feminine", "neuter", "masculine-feminine", "masculine-neuter",
                                     "feminine-neuter", "common"};
  static const char* const kPers[] = {"", "first", "second", "third"};
  static const char* const kTense[] = {"", "present", "imperfect", "future", "perfect", "pluperfect",
                                       "future-perfect", "aorist"};
  static const char* const kMood[] = {"", "indicative", "subjunctive", "imperative", "infinitive", "participle",
                                      "gerund", "optative"};
  static const char* const kVoice[] = {"", "active", "passive", "middle"};
  static const char* const kDeg[] = {"", "positive", "comparative", "superlative"};
  const Features f = unpack(packed);
  rules::Features o;
  o.pos = f.pos < 21 ? kPos[f.pos] : "other";
  o.case_ = f.case_ < 8 ? kCase[f.case_] : "";
  o.number = kNum[f.number & 3];
  o.gender = kGen[f.gender & 7];
  o.person = kPers[f.person & 3];
  o.tense = f.tense < 8 ? kTense[f.tense] : "";
  o.mood = kMood[f.mood & 7];
  if (f.extra & Gerundive) o.mood = "gerundive";
  else if (f.extra & Supine) o.mood = "supine";
  o.voice = kVoice[f.voice & 3];
  o.degree = kDeg[f.degree & 3];
  return o;
}

}  // namespace vp::realise
