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

}  // namespace vp::morph::detail
