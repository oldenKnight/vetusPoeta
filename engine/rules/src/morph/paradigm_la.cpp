// Rule-based Latin paradigm fallback (see paradigm_la.h). Every form produced here is reported fromRule = true by
// the callers; the checker turns that into Check, never OK.
#include "paradigm_la.h"

#include "vp/text.h"

namespace vp::morph::detail {

using namespace vp::feat;

bool endsWith(std::string_view s, std::string_view suffix) {
  return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
}
std::string dropSuffix(std::string_view s, std::string_view suffix) {
  if (!endsWith(s, suffix)) return std::string(s);
  return std::string(s.substr(0, s.size() - suffix.size()));
}

namespace {

uint32_t pk(uint8_t pos, uint8_t c, uint8_t n, uint8_t g, uint8_t degree = 0) {
  Features f;
  f.pos = pos; f.case_ = c; f.number = n; f.gender = g; f.degree = degree;
  return pack(f);
}
uint32_t vk(uint8_t person, uint8_t number, uint8_t tense, uint8_t mood, uint8_t voice) {
  Features f;
  f.pos = Verb; f.person = person; f.number = number; f.tense = tense; f.mood = mood; f.voice = voice;
  return pack(f);
}

const uint8_t kCases[6] = {Nom, Gen, Dat, Acc, Abl, Voc};

// 12 endings: sg nom gen dat acc abl voc, pl nom gen dat acc abl voc. "@" = the nominative singular as given.
void decline(std::string_view stem, std::string_view nom, const char* const end[12], uint8_t pos, uint8_t gender,
             uint8_t degree, std::vector<Cell>& out) {
  for (int i = 0; i < 12; ++i) {
    std::string form;
    if (std::string_view(end[i]) == "@") form = std::string(nom);
    else form = std::string(stem) + end[i];
    out.push_back(Cell{pk(pos, kCases[i % 6], i < 6 ? Sg : Pl, gender, degree), std::move(form)});
  }
}

const char* const kDecl1[12] = {"a", "ae", "ae", "am", "ā", "a", "ae", "ārum", "īs", "ās", "īs", "ae"};
const char* const kDecl2M[12] = {"us", "ī", "ō", "um", "ō", "e", "ī", "ōrum", "īs", "ōs", "īs", "ī"};
const char* const kDecl2Er[12] = {"@", "ī", "ō", "um", "ō", "@", "ī", "ōrum", "īs", "ōs", "īs", "ī"};
const char* const kDecl2N[12] = {"um", "ī", "ō", "um", "ō", "um", "a", "ōrum", "īs", "a", "īs", "a"};
const char* const kCompMF[12] = {"ior", "iōris", "iōrī", "iōrem", "iōre", "ior",
                                 "iōrēs", "iōrum", "iōribus", "iōrēs", "iōribus", "iōrēs"};
const char* const kCompN[12] = {"ius", "iōris", "iōrī", "ius", "iōre", "ius",
                                "iōra", "iōrum", "iōribus", "iōra", "iōribus", "iōra"};

// Verb endings: 6 per tense (1sg 2sg 3sg 1pl 2pl 3pl), appended to the root.
struct TenseRow { uint8_t tense, mood, voice; const char* e[6]; };
const TenseRow kConj1[] = {
    {Present, Indicative, Active, {"ō", "ās", "at", "āmus", "ātis", "ant"}},
    {Imperfect, Indicative, Active, {"ābam", "ābās", "ābat", "ābāmus", "ābātis", "ābant"}},
    {Future, Indicative, Active, {"ābō", "ābis", "ābit", "ābimus", "ābitis", "ābunt"}},
    {Present, Subjunctive, Active, {"em", "ēs", "et", "ēmus", "ētis", "ent"}},
    {Imperfect, Subjunctive, Active, {"ārem", "ārēs", "āret", "ārēmus", "ārētis", "ārent"}},
    {Present, Indicative, Passive, {"or", "āris", "ātur", "āmur", "āminī", "antur"}},
    {Imperfect, Indicative, Passive, {"ābar", "ābāris", "ābātur", "ābāmur", "ābāminī", "ābantur"}},
    {Future, Indicative, Passive, {"ābor", "āberis", "ābitur", "ābimur", "ābiminī", "ābuntur"}},
    {Present, Subjunctive, Passive, {"er", "ēris", "ētur", "ēmur", "ēminī", "entur"}},
    {Imperfect, Subjunctive, Passive, {"ārer", "ārēris", "ārētur", "ārēmur", "ārēminī", "ārentur"}},
};
const TenseRow kConj2[] = {
    {Present, Indicative, Active, {"eō", "ēs", "et", "ēmus", "ētis", "ent"}},
    {Imperfect, Indicative, Active, {"ēbam", "ēbās", "ēbat", "ēbāmus", "ēbātis", "ēbant"}},
    {Future, Indicative, Active, {"ēbō", "ēbis", "ēbit", "ēbimus", "ēbitis", "ēbunt"}},
    {Present, Subjunctive, Active, {"eam", "eās", "eat", "eāmus", "eātis", "eant"}},
    {Imperfect, Subjunctive, Active, {"ērem", "ērēs", "ēret", "ērēmus", "ērētis", "ērent"}},
    {Present, Indicative, Passive, {"eor", "ēris", "ētur", "ēmur", "ēminī", "entur"}},
    {Imperfect, Indicative, Passive, {"ēbar", "ēbāris", "ēbātur", "ēbāmur", "ēbāminī", "ēbantur"}},
    {Future, Indicative, Passive, {"ēbor", "ēberis", "ēbitur", "ēbimur", "ēbiminī", "ēbuntur"}},
    {Present, Subjunctive, Passive, {"ear", "eāris", "eātur", "eāmur", "eāminī", "eantur"}},
    {Imperfect, Subjunctive, Passive, {"ērer", "ērēris", "ērētur", "ērēmur", "ērēminī", "ērentur"}},
};
const TenseRow kPerfect[] = {
    {Perfect, Indicative, Active, {"ī", "istī", "it", "imus", "istis", "ērunt"}},
    {Pluperfect, Indicative, Active, {"eram", "erās", "erat", "erāmus", "erātis", "erant"}},
    {FuturePerfect, Indicative, Active, {"erō", "eris", "erit", "erimus", "eritis", "erint"}},
    {Perfect, Subjunctive, Active, {"erim", "erīs", "erit", "erīmus", "erītis", "erint"}},
    {Pluperfect, Subjunctive, Active, {"issem", "issēs", "isset", "issēmus", "issētis", "issent"}},
};

void conjugate(std::string_view root, const TenseRow* rows, size_t n, std::vector<Cell>& out) {
  for (size_t r = 0; r < n; ++r)
    for (int i = 0; i < 6; ++i)
      out.push_back(Cell{vk((uint8_t)(i % 3 + 1), i < 3 ? Sg : Pl, rows[r].tense, rows[r].mood, rows[r].voice),
                         std::string(root) + rows[r].e[i]});
}

std::string stripStar(std::string_view s) {
  while (!s.empty() && s.front() == '*') s.remove_prefix(1);
  return std::string(s);
}

bool isDeponentHead(const lex::Lemma& l) { return (l.flags & lex::Deponent) || endsWith(l.head, "or"); }

}  // namespace

std::string adjStem(std::string_view nomM, std::string_view feminine) {
  if (endsWith(nomM, "us")) return dropSuffix(nomM, "us");
  if (!feminine.empty() && endsWith(feminine, "a")) return dropSuffix(feminine, "a");
  if (endsWith(nomM, "er")) return std::string(nomM);
  return std::string();
}

std::string superlativeNom(std::string_view nomM, std::string_view stem) {
  if (endsWith(nomM, "er")) return std::string(nomM) + "rimus";
  return std::string(stem) + "issimus";
}

void adjective12(std::string_view nomM, std::string_view stem, uint8_t pos, uint8_t degree, std::vector<Cell>& out) {
  if (stem.empty()) return;
  if (endsWith(nomM, "us")) {
    decline(stem, nomM, kDecl2M, pos, M, degree, out);
    if (endsWith(nomM, "ius") && !out.empty()) out[out.size() - 7].form = dropSuffix(nomM, "ius") + "ī";   // voc sg
  } else {
    decline(stem, nomM, kDecl2Er, pos, M, degree, out);
  }
  decline(stem, nomM, kDecl1, pos, F, degree, out);
  decline(stem, nomM, kDecl2N, pos, N, degree, out);
}

void comparative(std::string_view stem, uint8_t pos, std::vector<Cell>& out) {
  if (stem.empty()) return;
  const std::string nom = std::string(stem) + "ior";
  decline(stem, nom, kCompMF, pos, M, Comparative, out);
  decline(stem, nom, kCompMF, pos, F, Comparative, out);
  decline(stem, nom, kCompN, pos, N, Comparative, out);
}

bool paradigmApplies(const lex::Lemma& l) {
  if (l.id == lex::kNoLemma || (l.flags & lex::HasTable) || (l.flags & lex::Indeclinable)) return false;
  if (l.head.find(' ') != std::string_view::npos) return false;
  switch (l.pos) {
    case Noun: case Name: return l.cls == 1 || l.cls == 2;
    case Adj: case Participle: return l.cls == 1;
    case Verb: return (l.cls == 1 || l.cls == 2) && !isDeponentHead(l);
    default: return false;
  }
}

void paradigm(const lex::Lemma& l, std::vector<Cell>& out) {
  if (!paradigmApplies(l)) return;
  const std::string head = stripStar(text::nfc(l.head));
  const Principal p = parsePrincipal(head, l.principal);
  const bool pluralOnly = (l.flags & lex::PluralOnly) != 0;
  const size_t before = out.size();
  if (l.pos == Noun || l.pos == Name) {
    if (l.cls == 1) {
      std::string stem = endsWith(head, "a") ? dropSuffix(head, "a") : pluralOnly ? dropSuffix(head, "ae") : "";
      if (stem.empty() && endsWith(p.genitive, "ae")) stem = dropSuffix(p.genitive, "ae");
      if (stem.empty()) return;
      decline(stem, head, kDecl1, l.pos, 0, 0, out);
    } else {
      std::string stem;
      if (endsWith(p.genitive, "ī")) stem = dropSuffix(p.genitive, "ī");
      else if (endsWith(p.genitive, "ōrum")) stem = dropSuffix(p.genitive, "ōrum");
      else if (endsWith(head, "us")) stem = dropSuffix(head, "us");
      else if (endsWith(head, "um")) stem = dropSuffix(head, "um");
      if (stem.empty()) return;
      const bool neuter = l.gender == N || endsWith(head, "um") || (pluralOnly && endsWith(head, "a"));
      if (neuter) decline(stem, head, kDecl2N, l.pos, 0, 0, out);
      else if (endsWith(head, "us") || pluralOnly) {
        decline(stem, head, kDecl2M, l.pos, 0, 0, out);
        if (endsWith(head, "ius")) out[before + 5].form = dropSuffix(head, "ius") + "ī";
      } else decline(stem, head, kDecl2Er, l.pos, 0, 0, out);
    }
    if (pluralOnly) {   // keep the plural cells only
      std::vector<Cell> pl;
      for (size_t i = before; i < out.size(); ++i)
        if (unpack(out[i].packed).number == Pl) pl.push_back(std::move(out[i]));
      out.resize(before);
      for (Cell& c : pl) out.push_back(std::move(c));
    }
    return;
  }
  if (l.pos == Adj || l.pos == Participle) {
    const std::string stem = adjStem(head, p.feminine);
    adjective12(head, stem, l.pos, 0, out);
    if (l.pos == Adj && !stem.empty()) {
      comparative(stem, Adj, out);
      const std::string sup = superlativeNom(head, stem);
      adjective12(sup, dropSuffix(sup, "us"), Adj, Superlative, out);
    }
    return;
  }
  // Verbs, 1st and 2nd conjugation.
  std::string inf = stripStar(p.infinitive);
  std::string root;
  const TenseRow* rows = nullptr;
  if (l.cls == 1 && endsWith(inf, "āre")) { root = dropSuffix(inf, "āre"); rows = kConj1; }
  else if (l.cls == 2 && endsWith(inf, "ēre")) { root = dropSuffix(inf, "ēre"); rows = kConj2; }
  else if (l.cls == 1 && endsWith(head, "ō") && inf.empty()) { root = dropSuffix(head, "ō"); rows = kConj1; inf = root + "āre"; }
  else if (l.cls == 2 && endsWith(head, "eō") && inf.empty()) { root = dropSuffix(head, "eō"); rows = kConj2; inf = root + "ēre"; }
  if (!rows || root.empty()) return;
  conjugate(root, rows, 10, out);
  const char* vowel = l.cls == 1 ? "ā" : "ē";
  out.push_back(Cell{vk(P2, Sg, Present, Imperative, Active), root + vowel});
  out.push_back(Cell{vk(P2, Pl, Present, Imperative, Active), root + vowel + "te"});
  out.push_back(Cell{vk(P2, Sg, Present, Imperative, Passive), root + vowel + "re"});
  out.push_back(Cell{vk(P2, Pl, Present, Imperative, Passive), root + vowel + "minī"});
  out.push_back(Cell{vk(0, 0, Present, Infinitive, Active), inf});
  out.push_back(Cell{vk(0, 0, Present, Infinitive, Passive), root + vowel + "rī"});
  out.push_back(Cell{vk(0, 0, Present, ParticipleMood, Active), root + vowel + "ns"});
  const std::string perf = stripStar(p.perfect);
  if (endsWith(perf, "ī")) {
    const std::string pstem = dropSuffix(perf, "ī");
    conjugate(pstem, kPerfect, 5, out);
    out.push_back(Cell{vk(0, 0, Perfect, Infinitive, Active), pstem + "isse"});
  }
  const std::string sup = stripStar(p.supine);
  if (endsWith(sup, "um")) {
    const std::string sstem = dropSuffix(sup, "um");
    out.push_back(Cell{vk(0, 0, Perfect, ParticipleMood, Passive), sstem + "us"});
    out.push_back(Cell{vk(0, 0, Future, ParticipleMood, Active), sstem + "ūrus"});
  }
}


// ---- C32: gap cells -------------------------------------------------------------------------------------------------
namespace {

const TenseRow kConj3[] = {
    {Present, Indicative, Active, {"ō", "is", "it", "imus", "itis", "unt"}},
    {Imperfect, Indicative, Active, {"ēbam", "ēbās", "ēbat", "ēbāmus", "ēbātis", "ēbant"}},
    {Future, Indicative, Active, {"am", "ēs", "et", "ēmus", "ētis", "ent"}},
    {Present, Subjunctive, Active, {"am", "ās", "at", "āmus", "ātis", "ant"}},
    {Imperfect, Subjunctive, Active, {"erem", "erēs", "eret", "erēmus", "erētis", "erent"}},
    {Present, Indicative, Passive, {"or", "eris", "itur", "imur", "iminī", "untur"}},
    {Imperfect, Indicative, Passive, {"ēbar", "ēbāris", "ēbātur", "ēbāmur", "ēbāminī", "ēbantur"}},
    {Future, Indicative, Passive, {"ar", "ēris", "ētur", "ēmur", "ēminī", "entur"}},
    {Present, Subjunctive, Passive, {"ar", "āris", "ātur", "āmur", "āminī", "antur"}},
    {Imperfect, Subjunctive, Passive, {"erer", "erēris", "erētur", "erēmur", "erēminī", "erentur"}},
};
const TenseRow kConj3io[] = {
    {Present, Indicative, Active, {"iō", "is", "it", "imus", "itis", "iunt"}},
    {Imperfect, Indicative, Active, {"iēbam", "iēbās", "iēbat", "iēbāmus", "iēbātis", "iēbant"}},
    {Future, Indicative, Active, {"iam", "iēs", "iet", "iēmus", "iētis", "ient"}},
    {Present, Subjunctive, Active, {"iam", "iās", "iat", "iāmus", "iātis", "iant"}},
    {Imperfect, Subjunctive, Active, {"erem", "erēs", "eret", "erēmus", "erētis", "erent"}},
    {Present, Indicative, Passive, {"ior", "eris", "itur", "imur", "iminī", "iuntur"}},
    {Imperfect, Indicative, Passive, {"iēbar", "iēbāris", "iēbātur", "iēbāmur", "iēbāminī", "iēbantur"}},
    {Future, Indicative, Passive, {"iar", "iēris", "iētur", "iēmur", "iēminī", "ientur"}},
    {Present, Subjunctive, Passive, {"iar", "iāris", "iātur", "iāmur", "iāminī", "iantur"}},
    {Imperfect, Subjunctive, Passive, {"erer", "erēris", "erētur", "erēmur", "erēminī", "erentur"}},
};
const TenseRow kConj4[] = {
    {Present, Indicative, Active, {"iō", "īs", "it", "īmus", "ītis", "iunt"}},
    {Imperfect, Indicative, Active, {"iēbam", "iēbās", "iēbat", "iēbāmus", "iēbātis", "iēbant"}},
    {Future, Indicative, Active, {"iam", "iēs", "iet", "iēmus", "iētis", "ient"}},
    {Present, Subjunctive, Active, {"iam", "iās", "iat", "iāmus", "iātis", "iant"}},
    {Imperfect, Subjunctive, Active, {"īrem", "īrēs", "īret", "īrēmus", "īrētis", "īrent"}},
    {Present, Indicative, Passive, {"ior", "īris", "ītur", "īmur", "īminī", "iuntur"}},
    {Imperfect, Indicative, Passive, {"iēbar", "iēbāris", "iēbātur", "iēbāmur", "iēbāminī", "iēbantur"}},
    {Future, Indicative, Passive, {"iar", "iēris", "iētur", "iēmur", "iēminī", "ientur"}},
    {Present, Subjunctive, Passive, {"iar", "iāris", "iātur", "iāmur", "iāminī", "iantur"}},
    {Imperfect, Subjunctive, Passive, {"īrer", "īrēris", "īrētur", "īrēmur", "īrēminī", "īrentur"}},
};

struct OwnCell { Features f; std::string form; };

bool hardExtra(const Features& f) { return (f.extra & (Supine | Gerundive)) != 0; }

// The lemma's own cell for exact features (the gender admitted by a merged cell gender), the plain spelling first.
const OwnCell* own(const std::vector<OwnCell>& cells, uint8_t c, uint8_t n, uint8_t g, uint8_t person, uint8_t tense,
                   uint8_t mood, uint8_t voice) {
  const OwnCell* best = nullptr;
  int bestScore = -1;
  for (const OwnCell& x : cells) {
    const Features& f = x.f;
    if (hardExtra(f) || f.degree > Positive) continue;
    if (f.case_ != c || f.number != n || f.person != person || f.tense != tense || f.mood != mood) continue;
    if (voice && f.voice != voice && !(voice == Active && f.voice == 0)) continue;
    int s = 0;
    if (g) {
      if (f.gender == g) s += 4;
      else if (f.gender == 0) s += 1;
      else if (genderAdmits(f.gender, g)) s += 2;
      else continue;
    }
    if (!(f.extra & Alternative)) s += 8;
    if (s > bestScore) { bestScore = s; best = &x; }
  }
  return best;
}

// Ending i of the 12 (sg nom gen dat acc abl voc, pl nom gen dat acc abl voc).
int slot(uint8_t c, uint8_t n) {
  int k = -1;
  for (int i = 0; i < 6; ++i)
    if (kCases[i] == c) k = i;
  if (k < 0) return -1;
  return n == Pl ? 6 + k : k;
}

bool nominalGap(const std::vector<OwnCell>& cells, const lex::Lemma& l, const Features& want, std::string& form,
                bool& attested) {
  const uint8_t c = want.case_, n = want.number ? want.number : (uint8_t)Sg;
  if (c == 0 || c == Loc || want.degree > Positive) return false;
  bool hasSg = false, hasPl = false;
  for (const OwnCell& x : cells) {
    hasSg = hasSg || x.f.number == Sg;
    hasPl = hasPl || x.f.number == Pl;
  }
  const bool nominal = l.pos == Noun || l.pos == Name;
  if (n == Sg && ((l.flags & lex::PluralOnly) || (!hasSg && hasPl))) return false;   // tenebrae: no singular
  if (l.pos == Name && n == Pl && !hasPl) return false;                         // a name has no plural it does not list
  uint8_t g = nominal ? (uint8_t)0 : want.gender;
  const uint8_t lg = nominal ? l.gender : g;
  const bool neuter = lg == N;
  auto cell = [&](uint8_t cc, uint8_t nn) -> const OwnCell* { return own(cells, cc, nn, g, 0, 0, 0, 0); };
  // 1. syncretism inside the lemma's own cells
  if (c == Voc || (c == Nom && n == Pl)) {
    if (const OwnCell* x = cell(c == Voc ? Nom : Voc, n)) {
      const OwnCell* gen = cell(Gen, Sg);
      if (c == Voc && n == Sg && endsWith(x->form, "us") && gen && endsWith(gen->form, "ī") && (nominal || g == M)) {
        form = endsWith(x->form, "ius") && nominal ? dropSuffix(x->form, "ius") + "ī" : dropSuffix(x->form, "us") + "e";
        return true;
      }
      form = x->form;
      attested = true;
      return true;
    }
  }
  if ((c == Dat || c == Abl) && n == Pl)
    if (const OwnCell* x = cell(c == Dat ? Abl : Dat, Pl)) { form = x->form; attested = true; return true; }
  if (neuter && (c == Nom || c == Acc || c == Voc))
    for (uint8_t cc : {Nom, Acc, Voc})
      if (const OwnCell* x = cell(cc, n)) { form = x->form; attested = true; return true; }
  // 2. the declension endings on the stem of the genitive (never for a defective word: vicis, aiō)
  if (l.flags & lex::Defective) return false;
  const Principal p = parsePrincipal(stripStar(text::nfc(l.head)), l.principal);
  std::string nomSg, genSg, genPl;
  if (const OwnCell* x = cell(Nom, Sg)) nomSg = x->form;
  if (const OwnCell* x = own(cells, Gen, Sg, nominal ? (uint8_t)0 : (uint8_t)M, 0, 0, 0, 0)) genSg = x->form;
  if (const OwnCell* x = own(cells, Gen, Pl, nominal ? (uint8_t)0 : (uint8_t)M, 0, 0, 0, 0)) genPl = x->form;
  if (genSg.empty()) genSg = stripStar(p.genitive);
  if (nomSg.empty() && nominal && hasSg) nomSg = stripStar(text::nfc(l.head));
  if (!nominal) {   // adjectives: the nominative of the wanted gender, else the masculine one
    if (nomSg.empty())
      if (const OwnCell* x = own(cells, Nom, Sg, M, 0, 0, 0, 0)) nomSg = x->form;
    if (nomSg.empty()) nomSg = stripStar(text::nfc(l.head));
    std::string nomM = nomSg;
    if (const OwnCell* x = own(cells, Nom, Sg, M, 0, 0, 0, 0)) nomM = x->form;
    if (endsWith(genSg, "ī") || (genSg.empty() && endsWith(nomM, "us"))) {   // 1st/2nd class
      std::string fem = p.feminine;
      if (const OwnCell* x = own(cells, Nom, Sg, F, 0, 0, 0, 0)) fem = x->form;
      const std::string stem = adjStem(nomM, fem);
      std::vector<Cell> out;
      adjective12(nomM, stem, l.pos, 0, out);
      for (const Cell& x : out) {
        const Features f = unpack(x.packed);
        if (f.case_ == c && f.number == n && f.gender == g) { form = x.form; return true; }
      }
      return false;
    }
    if (!endsWith(genSg, "is")) return false;
    const std::string stem = dropSuffix(genSg, "is");
    const bool iStem = genPl.empty() ? true : endsWith(genPl, "ium");
    std::string nomN = nomSg;
    if (const OwnCell* x = own(cells, Nom, Sg, N, 0, 0, 0, 0)) nomN = x->form;
    else if (endsWith(nomM, "is") && iStem) nomN = dropSuffix(nomM, "is") + "e";   // fortis, forte (iuvenis stays)
    else nomN = nomM;
    const bool nt = g == N;
    const char* const endMF[12] = {"@", "is", "ī", "em", iStem ? "ī" : "e", "@",
                                   "ēs", iStem ? "ium" : "um", "ibus", "ēs", "ibus", "ēs"};
    const char* const endN[12] = {"@", "is", "ī", "@", iStem ? "ī" : "e", "@",
                                  iStem ? "ia" : "a", iStem ? "ium" : "um", "ibus", iStem ? "ia" : "a", "ibus",
                                  iStem ? "ia" : "a"};
    const int k = slot(c, n);
    if (k < 0) return false;
    const char* e = nt ? endN[k] : endMF[k];
    form = std::string_view(e) == "@" ? (nt ? nomN : nomSg) : stem + e;
    return true;
  }
  // nouns: the declension from the genitive singular (or plural for a plural-only noun)
  std::string stem;
  int decl = 0;
  if (endsWith(genSg, "ae")) { decl = 1; stem = dropSuffix(genSg, "ae"); }
  else if (endsWith(genSg, "ī") && !endsWith(genSg, "eī") && !endsWith(genSg, "ēī")) { decl = 2; stem = dropSuffix(genSg, "ī"); }
  else if (endsWith(genSg, "is")) { decl = 3; stem = dropSuffix(genSg, "is"); }
  else if (endsWith(genSg, "ūs")) { decl = 4; stem = dropSuffix(genSg, "ūs"); }
  else if (endsWith(genSg, "ēī")) { decl = 5; stem = dropSuffix(genSg, "ēī"); }
  else if (endsWith(genSg, "eī")) { decl = 5; stem = dropSuffix(genSg, "eī"); }
  else if (genSg.empty() && !genPl.empty()) {
    if (endsWith(genPl, "ārum")) { decl = 1; stem = dropSuffix(genPl, "ārum"); }
    else if (endsWith(genPl, "ōrum")) { decl = 2; stem = dropSuffix(genPl, "ōrum"); }
    else if (endsWith(genPl, "uum")) { decl = 4; stem = dropSuffix(genPl, "uum"); }
    else if (endsWith(genPl, "ērum")) { decl = 5; stem = dropSuffix(genPl, "ērum"); }
    else if (endsWith(genPl, "ium")) { decl = 3; stem = dropSuffix(genPl, "ium"); }
    else if (endsWith(genPl, "um")) { decl = 3; stem = dropSuffix(genPl, "um"); }
  }
  if (!decl || stem.empty()) return false;
  const int k = slot(c, n);
  if (k < 0) return false;
  const char* e = nullptr;
  if (decl == 1) e = kDecl1[k];
  else if (decl == 2) e = neuter ? kDecl2N[k] : endsWith(nomSg, "us") ? kDecl2M[k] : kDecl2Er[k];
  else if (decl == 3) {
    const bool iStem = endsWith(genPl, "ium") ||
                       (genPl.empty() && neuter && (endsWith(nomSg, "e") || endsWith(nomSg, "al") || endsWith(nomSg, "ar")));
    static const char* const d3c[12] = {"@", "is", "ī", "em", "e", "@", "ēs", "um", "ibus", "ēs", "ibus", "ēs"};
    static const char* const d3i[12] = {"@", "is", "ī", "em", "e", "@", "ēs", "ium", "ibus", "ēs", "ibus", "ēs"};
    static const char* const d3n[12] = {"@", "is", "ī", "@", "e", "@", "a", "um", "ibus", "a", "ibus", "a"};
    static const char* const d3ni[12] = {"@", "is", "ī", "@", "ī", "@", "ia", "ium", "ibus", "ia", "ibus", "ia"};
    e = neuter ? (iStem ? d3ni[k] : d3n[k]) : (iStem ? d3i[k] : d3c[k]);
  } else if (decl == 4) {
    static const char* const d4[12] = {"us", "ūs", "uī", "um", "ū", "us", "ūs", "uum", "ibus", "ūs", "ibus", "ūs"};
    static const char* const d4n[12] = {"ū", "ūs", "ū", "ū", "ū", "ū", "ua", "uum", "ibus", "ua", "ibus", "ua"};
    e = neuter ? d4n[k] : d4[k];
  } else {
    static const char* const d5[12] = {"ēs", "eī", "eī", "em", "ē", "ēs", "ēs", "ērum", "ēbus", "ēs", "ēbus", "ēs"};
    e = d5[k];
    if ((k == 1 || k == 2) && endsWith(genSg, "ēī")) e = "ēī";
  }
  if (std::string_view(e) == "@") {
    if (nomSg.empty()) return false;
    form = nomSg;
    return true;
  }
  form = stem + e;
  return true;
}

bool verbGap(const std::vector<OwnCell>& cells, const lex::Lemma& l, const Features& want0, std::string& form,
             bool& attested) {
  Features want = want0;
  const bool dep = (l.flags & lex::Deponent) != 0;
  if (dep) return false;   // deponents: periphrasis and their own cells only
  const bool defective = (l.flags & lex::Defective) != 0;
  if (want.mood != Indicative && want.mood != Subjunctive && want.mood != Imperative && want.mood != Infinitive &&
      want.mood != ParticipleMood)
    return false;
  bool personal = false, personalPassive = false, anyPassive = false, perfectCells = false;
  for (const OwnCell& x : cells) {
    const bool p12 = x.f.person == P1 || x.f.person == P2;
    personal = personal || p12;
    personalPassive = personalPassive || (p12 && x.f.voice == Passive);
    anyPassive = anyPassive || x.f.voice == Passive;
    perfectCells = perfectCells || (x.f.tense == Perfect && x.f.mood == Indicative && x.f.person);
  }
  const bool table = !cells.empty();
  if (table && !personal && (want.person == P1 || want.person == P2 || want.number == Pl || want.mood == Imperative))
    return false;   // impersonal (pluit, licet): 3rd singular only
  if (want.voice == Passive && table && !(personalPassive || (!personal && anyPassive))) return false;
  const uint8_t voice = want.voice == Passive ? (uint8_t)Passive : (uint8_t)Active;
  auto vc = [&](uint8_t person, uint8_t number, uint8_t tense, uint8_t mood) -> const OwnCell* {
    return own(cells, 0, number, 0, person, tense, mood, voice);
  };
  // perfect-only verbs (meminī, ōdī): the "present" cells are perfect forms with a present meaning
  const OwnCell* p1 = own(cells, 0, Sg, 0, P1, Present, Indicative, Active);
  if (!perfectCells && p1 && endsWith(p1->form, "ī") && voice == Active) {
    uint8_t t = want.tense;
    uint8_t m = want.mood;
    // the perfect form means the present: a past event takes the pluperfect form (memineram, the cell the lexicon
    // labels imperfect), the future perfect form stands for the future (meminerō)
    if (t == Perfect || t == Pluperfect) t = Imperfect;
    else if (t == FuturePerfect) t = Future;
    if (m == Imperative) t = Future;
    if (m == Infinitive) t = Present;   // meminisse
    if (m == ParticipleMood) return false;
    attested = true;
    if (m == Infinitive) {
      if (const OwnCell* x = own(cells, 0, 0, 0, 0, Present, Infinitive, Active)) { form = x->form; return true; }
      return false;
    }
    if (const OwnCell* x = vc(want.person, want.number, t, m)) { form = x->form; return true; }
    return false;
  }
  // a defective verb without a present imperative: the future imperative it has (mementō, scītō)
  if (defective && want.mood == Imperative && want.tense <= Present)
    if (const OwnCell* x = vc(P2, want.number ? want.number : (uint8_t)Sg, Future, Imperative)) {
      form = x->form;
      attested = true;
      return true;
    }
  if (defective) return false;   // aiō, inquam: only the forms they have
  const Principal p = parsePrincipal(stripStar(text::nfc(l.head)), l.principal);
  // the perfect active system on the perfect stem
  if (voice == Active && (want.tense == Perfect || want.tense == Pluperfect || want.tense == FuturePerfect)) {
    std::string perf;
    if (const OwnCell* x = vc(P1, Sg, Perfect, Indicative)) perf = x->form;
    if (perf.empty()) perf = stripStar(p.perfect);
    if (!endsWith(perf, "ī")) return false;
    const std::string pstem = dropSuffix(perf, "ī");
    if (want.mood == Infinitive && want.tense == Perfect) { form = pstem + "isse"; return true; }
    std::vector<Cell> out;
    conjugate(pstem, kPerfect, 5, out);
    for (const Cell& x : out) {
      const Features f = unpack(x.packed);
      if (f.person == want.person && f.number == want.number && f.tense == want.tense && f.mood == want.mood) {
        form = x.form;
        return true;
      }
    }
    return false;
  }
  if (want.tense != Present && want.tense != Imperfect && want.tense != Future && want.tense != 0) return false;
  // the present system on the present stem, by conjugation (from the infinitive and the 1st singular)
  std::string inf, first;
  if (const OwnCell* x = own(cells, 0, 0, 0, 0, Present, Infinitive, Active)) inf = x->form;
  if (inf.empty()) inf = stripStar(p.infinitive);
  if (p1) first = p1->form;
  if (first.empty()) first = stripStar(text::nfc(l.head));
  const TenseRow* rows = nullptr;
  std::string root, vowel, part;
  if (endsWith(inf, "āre")) { rows = kConj1; root = dropSuffix(inf, "āre"); vowel = "ā"; part = "āns"; }
  else if (endsWith(inf, "ēre")) { rows = kConj2; root = dropSuffix(inf, "ēre"); vowel = "ē"; part = "ēns"; }
  else if (endsWith(inf, "īre")) { rows = kConj4; root = dropSuffix(inf, "īre"); vowel = "ī"; part = "iēns"; }
  else if (endsWith(inf, "ere") && endsWith(first, "iō")) { rows = kConj3io; root = dropSuffix(inf, "ere"); vowel = "e"; part = "iēns"; }
  else if (endsWith(inf, "ere")) { rows = kConj3; root = dropSuffix(inf, "ere"); vowel = "e"; part = "ēns"; }
  if (!rows || root.empty()) return false;
  if (want.mood == Infinitive) {
    if (voice == Active) form = inf;
    else form = rows == kConj3 || rows == kConj3io ? root + "ī" : root + vowel + "rī";
    return true;
  }
  if (want.mood == Imperative) {
    const bool pl = want.number == Pl;
    if (voice == Passive) return false;
    if (rows == kConj3 || rows == kConj3io) form = root + (pl ? "ite" : "e");
    else form = root + vowel + (pl ? "te" : "");
    return true;
  }
  if (want.mood == ParticipleMood) {
    if (want.tense != Present || voice != Active || (want.case_ && !(want.case_ == Nom && want.number <= Sg))) return false;
    form = root + part;
    return true;
  }
  std::vector<Cell> out;
  conjugate(root, rows, 10, out);
  for (const Cell& x : out) {
    const Features f = unpack(x.packed);
    if (f.person == want.person && f.number == want.number && f.tense == want.tense && f.mood == want.mood &&
        f.voice == voice) {
      form = x.form;
      return true;
    }
  }
  return false;
}

}  // namespace

bool gapCell(const lex::Lexicon& lx, const lex::Lemma& l, const Features& want, std::string& form, bool& attested) {
  if (l.id == lex::kNoLemma || (l.flags & lex::Indeclinable) || l.head.find(' ') != std::string_view::npos) return false;
  thread_local std::vector<std::pair<uint32_t, std::string_view>> raw;
  raw.clear();
  lx.cells(l.id, raw);
  std::vector<OwnCell> cells;
  cells.reserve(raw.size());
  for (const auto& c : raw)
    if (!c.second.empty() && c.second.find(' ') == std::string_view::npos && c.second.find('-') == std::string_view::npos)
      cells.push_back(OwnCell{unpack(c.first), text::nfc(stripStar(c.second))});
  form.clear();
  attested = false;
  bool ok = false;
  switch (l.pos) {
    case Noun: case Name: case Adj: case Participle: ok = nominalGap(cells, l, want, form, attested); break;
    case Verb: ok = verbGap(cells, l, want, form, attested); break;
    default: return false;
  }
  return ok && !form.empty();
}

}  // namespace vp::morph::detail
