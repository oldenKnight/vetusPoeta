// Greek lemma lookup, Attic-first generation, built-in closed-class tables, the rule paradigm for lemmas without a
// table, and analysis with the Attic filter (vp/morph_grc.h).
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "vp/morph_grc.h"
#include "vp/realise_grc.h"
#include "vp/text.h"

namespace vp::grc {

using namespace vp::feat;

// ---- feature builders -------------------------------------------------------------------------------------------
Features verbForm(uint8_t person, uint8_t number, uint8_t tense, uint8_t mood, uint8_t voice) {
  Features f; f.pos = Verb; f.person = person; f.number = number; f.tense = tense; f.mood = mood; f.voice = voice;
  return f;
}
Features infinitive(uint8_t tense, uint8_t voice) {
  Features f; f.pos = Verb; f.mood = Infinitive; f.tense = tense; f.voice = voice; return f;
}
Features imperative(uint8_t number, uint8_t tense, uint8_t voice) {
  Features f; f.pos = Verb; f.mood = Imperative; f.tense = tense; f.person = P2; f.number = number; f.voice = voice;
  return f;
}
Features nounForm(uint8_t case_, uint8_t number) { Features f; f.pos = Noun; f.case_ = case_; f.number = number; return f; }
Features adjForm(uint8_t case_, uint8_t number, uint8_t gender, uint8_t degree) {
  Features f; f.pos = Adj; f.case_ = case_; f.number = number; f.gender = gender; f.degree = degree; return f;
}

// ---- closed-class tables ----------------------------------------------------------------------------------------
namespace {

struct CF { uint8_t c, n, g; const char* form; bool enc; };
struct ClosedTable { const char* key; uint8_t person; const CF* cells; size_t count; };

#define VP_N(x) (sizeof(x) / sizeof((x)[0]))
// Gender 0 = any gender (personal pronouns, numerals with one form for m/f/n where marked).
const CF kArticle[] = {
    {Nom, Sg, M, "ὁ", false},   {Gen, Sg, M, "τοῦ", false}, {Dat, Sg, M, "τῷ", false},  {Acc, Sg, M, "τόν", false},
    {Nom, Pl, M, "οἱ", false},  {Gen, Pl, M, "τῶν", false}, {Dat, Pl, M, "τοῖς", false}, {Acc, Pl, M, "τούς", false},
    {Nom, Sg, F, "ἡ", false},   {Gen, Sg, F, "τῆς", false}, {Dat, Sg, F, "τῇ", false},  {Acc, Sg, F, "τήν", false},
    {Nom, Pl, F, "αἱ", false},  {Gen, Pl, F, "τῶν", false}, {Dat, Pl, F, "ταῖς", false}, {Acc, Pl, F, "τάς", false},
    {Nom, Sg, N, "τό", false},  {Gen, Sg, N, "τοῦ", false}, {Dat, Sg, N, "τῷ", false},  {Acc, Sg, N, "τό", false},
    {Nom, Pl, N, "τά", false},  {Gen, Pl, N, "τῶν", false}, {Dat, Pl, N, "τοῖς", false}, {Acc, Pl, N, "τά", false},
};
const CF kEgo[] = {
    {Nom, Sg, 0, "ἐγώ", false}, {Gen, Sg, 0, "ἐμοῦ", false}, {Dat, Sg, 0, "ἐμοί", false}, {Acc, Sg, 0, "ἐμέ", false},
    {Gen, Sg, 0, "μου", true},  {Dat, Sg, 0, "μοι", true},   {Acc, Sg, 0, "με", true},
};
const CF kSu[] = {
    {Nom, Sg, 0, "σύ", false}, {Gen, Sg, 0, "σοῦ", false}, {Dat, Sg, 0, "σοί", false}, {Acc, Sg, 0, "σέ", false},
    {Voc, Sg, 0, "σύ", false}, {Gen, Sg, 0, "σου", true},  {Dat, Sg, 0, "σοι", true},  {Acc, Sg, 0, "σε", true},
};
const CF kHemeis[] = {{Nom, Pl, 0, "ἡμεῖς", false}, {Gen, Pl, 0, "ἡμῶν", false}, {Dat, Pl, 0, "ἡμῖν", false}, {Acc, Pl, 0, "ἡμᾶς", false}};
const CF kHumeis[] = {{Nom, Pl, 0, "ὑμεῖς", false}, {Gen, Pl, 0, "ὑμῶν", false}, {Dat, Pl, 0, "ὑμῖν", false}, {Acc, Pl, 0, "ὑμᾶς", false},
                      {Voc, Pl, 0, "ὑμεῖς", false}};
const CF kAutos[] = {
    {Nom, Sg, M, "αὐτός", false}, {Gen, Sg, M, "αὐτοῦ", false}, {Dat, Sg, M, "αὐτῷ", false}, {Acc, Sg, M, "αὐτόν", false},
    {Nom, Pl, M, "αὐτοί", false}, {Gen, Pl, M, "αὐτῶν", false}, {Dat, Pl, M, "αὐτοῖς", false}, {Acc, Pl, M, "αὐτούς", false},
    {Nom, Sg, F, "αὐτή", false},  {Gen, Sg, F, "αὐτῆς", false}, {Dat, Sg, F, "αὐτῇ", false},  {Acc, Sg, F, "αὐτήν", false},
    {Nom, Pl, F, "αὐταί", false}, {Gen, Pl, F, "αὐτῶν", false}, {Dat, Pl, F, "αὐταῖς", false}, {Acc, Pl, F, "αὐτάς", false},
    {Nom, Sg, N, "αὐτό", false},  {Gen, Sg, N, "αὐτοῦ", false}, {Dat, Sg, N, "αὐτῷ", false},  {Acc, Sg, N, "αὐτό", false},
    {Nom, Pl, N, "αὐτά", false},  {Gen, Pl, N, "αὐτῶν", false}, {Dat, Pl, N, "αὐτοῖς", false}, {Acc, Pl, N, "αὐτά", false},
};
const CF kHoutos[] = {
    {Nom, Sg, M, "οὗτος", false}, {Gen, Sg, M, "τούτου", false}, {Dat, Sg, M, "τούτῳ", false}, {Acc, Sg, M, "τοῦτον", false},
    {Nom, Pl, M, "οὗτοι", false}, {Gen, Pl, M, "τούτων", false}, {Dat, Pl, M, "τούτοις", false}, {Acc, Pl, M, "τούτους", false},
    {Nom, Sg, F, "αὕτη", false},  {Gen, Sg, F, "ταύτης", false}, {Dat, Sg, F, "ταύτῃ", false},  {Acc, Sg, F, "ταύτην", false},
    {Nom, Pl, F, "αὗται", false}, {Gen, Pl, F, "τούτων", false}, {Dat, Pl, F, "ταύταις", false}, {Acc, Pl, F, "ταύτας", false},
    {Nom, Sg, N, "τοῦτο", false}, {Gen, Sg, N, "τούτου", false}, {Dat, Sg, N, "τούτῳ", false},  {Acc, Sg, N, "τοῦτο", false},
    {Nom, Pl, N, "ταῦτα", false}, {Gen, Pl, N, "τούτων", false}, {Dat, Pl, N, "τούτοις", false}, {Acc, Pl, N, "ταῦτα", false},
};
const CF kEkeinos[] = {
    {Nom, Sg, M, "ἐκεῖνος", false}, {Gen, Sg, M, "ἐκείνου", false}, {Dat, Sg, M, "ἐκείνῳ", false}, {Acc, Sg, M, "ἐκεῖνον", false},
    {Nom, Pl, M, "ἐκεῖνοι", false}, {Gen, Pl, M, "ἐκείνων", false}, {Dat, Pl, M, "ἐκείνοις", false}, {Acc, Pl, M, "ἐκείνους", false},
    {Nom, Sg, F, "ἐκείνη", false},  {Gen, Sg, F, "ἐκείνης", false}, {Dat, Sg, F, "ἐκείνῃ", false},  {Acc, Sg, F, "ἐκείνην", false},
    {Nom, Pl, F, "ἐκεῖναι", false}, {Gen, Pl, F, "ἐκείνων", false}, {Dat, Pl, F, "ἐκείναις", false}, {Acc, Pl, F, "ἐκείνας", false},
    {Nom, Sg, N, "ἐκεῖνο", false},  {Gen, Sg, N, "ἐκείνου", false}, {Dat, Sg, N, "ἐκείνῳ", false},  {Acc, Sg, N, "ἐκεῖνο", false},
    {Nom, Pl, N, "ἐκεῖνα", false},  {Gen, Pl, N, "ἐκείνων", false}, {Dat, Pl, N, "ἐκείνοις", false}, {Acc, Pl, N, "ἐκεῖνα", false},
};
const CF kHos[] = {
    {Nom, Sg, M, "ὅς", false}, {Gen, Sg, M, "οὗ", false}, {Dat, Sg, M, "ᾧ", false},  {Acc, Sg, M, "ὅν", false},
    {Nom, Pl, M, "οἵ", false}, {Gen, Pl, M, "ὧν", false}, {Dat, Pl, M, "οἷς", false}, {Acc, Pl, M, "οὕς", false},
    {Nom, Sg, F, "ἥ", false},  {Gen, Sg, F, "ἧς", false}, {Dat, Sg, F, "ᾗ", false},  {Acc, Sg, F, "ἥν", false},
    {Nom, Pl, F, "αἵ", false}, {Gen, Pl, F, "ὧν", false}, {Dat, Pl, F, "αἷς", false}, {Acc, Pl, F, "ἅς", false},
    {Nom, Sg, N, "ὅ", false},  {Gen, Sg, N, "οὗ", false}, {Dat, Sg, N, "ᾧ", false},  {Acc, Sg, N, "ὅ", false},
    {Nom, Pl, N, "ἅ", false},  {Gen, Pl, N, "ὧν", false}, {Dat, Pl, N, "οἷς", false}, {Acc, Pl, N, "ἅ", false},
};
const CF kTis[] = {   // interrogative: m/f one form, neuter τί
    {Nom, Sg, M, "τίς", false}, {Gen, Sg, M, "τίνος", false}, {Dat, Sg, M, "τίνι", false}, {Acc, Sg, M, "τίνα", false},
    {Nom, Pl, M, "τίνες", false}, {Gen, Pl, M, "τίνων", false}, {Dat, Pl, M, "τίσι", false}, {Acc, Pl, M, "τίνας", false},
    {Nom, Sg, F, "τίς", false}, {Gen, Sg, F, "τίνος", false}, {Dat, Sg, F, "τίνι", false}, {Acc, Sg, F, "τίνα", false},
    {Nom, Pl, F, "τίνες", false}, {Gen, Pl, F, "τίνων", false}, {Dat, Pl, F, "τίσι", false}, {Acc, Pl, F, "τίνας", false},
    {Nom, Sg, N, "τί", false},  {Gen, Sg, N, "τίνος", false}, {Dat, Sg, N, "τίνι", false}, {Acc, Sg, N, "τί", false},
    {Nom, Pl, N, "τίνα", false}, {Gen, Pl, N, "τίνων", false}, {Dat, Pl, N, "τίσι", false}, {Acc, Pl, N, "τίνα", false},
};
const CF kTisIndef[] = {   // indefinite, enclitic (orthotone spellings; the sandhi drops the accent)
    {Nom, Sg, M, "τις", true}, {Gen, Sg, M, "τινός", true}, {Dat, Sg, M, "τινί", true}, {Acc, Sg, M, "τινά", true},
    {Nom, Pl, M, "τινές", true}, {Gen, Pl, M, "τινῶν", true}, {Dat, Pl, M, "τισί", true}, {Acc, Pl, M, "τινάς", true},
    {Nom, Sg, F, "τις", true}, {Gen, Sg, F, "τινός", true}, {Dat, Sg, F, "τινί", true}, {Acc, Sg, F, "τινά", true},
    {Nom, Pl, F, "τινές", true}, {Gen, Pl, F, "τινῶν", true}, {Dat, Pl, F, "τισί", true}, {Acc, Pl, F, "τινάς", true},
    {Nom, Sg, N, "τι", true},  {Gen, Sg, N, "τινός", true}, {Dat, Sg, N, "τινί", true}, {Acc, Sg, N, "τι", true},
    {Nom, Pl, N, "τινά", true}, {Gen, Pl, N, "τινῶν", true}, {Dat, Pl, N, "τισί", true}, {Acc, Pl, N, "τινά", true},
};
const CF kOudeis[] = {
    {Nom, Sg, M, "οὐδείς", false}, {Gen, Sg, M, "οὐδενός", false}, {Dat, Sg, M, "οὐδενί", false}, {Acc, Sg, M, "οὐδένα", false},
    {Nom, Sg, F, "οὐδεμία", false}, {Gen, Sg, F, "οὐδεμιᾶς", false}, {Dat, Sg, F, "οὐδεμιᾷ", false}, {Acc, Sg, F, "οὐδεμίαν", false},
    {Nom, Sg, N, "οὐδέν", false}, {Gen, Sg, N, "οὐδενός", false}, {Dat, Sg, N, "οὐδενί", false}, {Acc, Sg, N, "οὐδέν", false},
};
const CF kMedeis[] = {
    {Nom, Sg, M, "μηδείς", false}, {Gen, Sg, M, "μηδενός", false}, {Dat, Sg, M, "μηδενί", false}, {Acc, Sg, M, "μηδένα", false},
    {Nom, Sg, F, "μηδεμία", false}, {Gen, Sg, F, "μηδεμιᾶς", false}, {Dat, Sg, F, "μηδεμιᾷ", false}, {Acc, Sg, F, "μηδεμίαν", false},
    {Nom, Sg, N, "μηδέν", false}, {Gen, Sg, N, "μηδενός", false}, {Dat, Sg, N, "μηδενί", false}, {Acc, Sg, N, "μηδέν", false},
};
const CF kHeis[] = {
    {Nom, Sg, M, "εἷς", false}, {Gen, Sg, M, "ἑνός", false}, {Dat, Sg, M, "ἑνί", false}, {Acc, Sg, M, "ἕνα", false},
    {Nom, Sg, F, "μία", false}, {Gen, Sg, F, "μιᾶς", false}, {Dat, Sg, F, "μιᾷ", false}, {Acc, Sg, F, "μίαν", false},
    {Nom, Sg, N, "ἕν", false},  {Gen, Sg, N, "ἑνός", false}, {Dat, Sg, N, "ἑνί", false}, {Acc, Sg, N, "ἕν", false},
};
const CF kDuo[] = {{Nom, Pl, 0, "δύο", false}, {Gen, Pl, 0, "δυοῖν", false}, {Dat, Pl, 0, "δυοῖν", false}, {Acc, Pl, 0, "δύο", false}};
const CF kTreis[] = {
    {Nom, Pl, M, "τρεῖς", false}, {Gen, Pl, M, "τριῶν", false}, {Dat, Pl, M, "τρισί", false}, {Acc, Pl, M, "τρεῖς", false},
    {Nom, Pl, F, "τρεῖς", false}, {Gen, Pl, F, "τριῶν", false}, {Dat, Pl, F, "τρισί", false}, {Acc, Pl, F, "τρεῖς", false},
    {Nom, Pl, N, "τρία", false},  {Gen, Pl, N, "τριῶν", false}, {Dat, Pl, N, "τρισί", false}, {Acc, Pl, N, "τρία", false},
};
const CF kTettares[] = {
    {Nom, Pl, M, "τέτταρες", false}, {Gen, Pl, M, "τεττάρων", false}, {Dat, Pl, M, "τέτταρσι", false}, {Acc, Pl, M, "τέτταρας", false},
    {Nom, Pl, F, "τέτταρες", false}, {Gen, Pl, F, "τεττάρων", false}, {Dat, Pl, F, "τέτταρσι", false}, {Acc, Pl, F, "τέτταρας", false},
    {Nom, Pl, N, "τέτταρα", false},  {Gen, Pl, N, "τεττάρων", false}, {Dat, Pl, N, "τέτταρσι", false}, {Acc, Pl, N, "τέτταρα", false},
};
const CF kPas[] = {
    {Nom, Sg, M, "πᾶς", false},   {Gen, Sg, M, "παντός", false}, {Dat, Sg, M, "παντί", false}, {Acc, Sg, M, "πάντα", false},
    {Nom, Pl, M, "πάντες", false}, {Gen, Pl, M, "πάντων", false}, {Dat, Pl, M, "πᾶσι", false}, {Acc, Pl, M, "πάντας", false},
    {Voc, Pl, M, "πάντες", false},
    {Nom, Sg, F, "πᾶσα", false},  {Gen, Sg, F, "πάσης", false},  {Dat, Sg, F, "πάσῃ", false}, {Acc, Sg, F, "πᾶσαν", false},
    {Nom, Pl, F, "πᾶσαι", false}, {Gen, Pl, F, "πασῶν", false},  {Dat, Pl, F, "πάσαις", false}, {Acc, Pl, F, "πάσας", false},
    {Voc, Pl, F, "πᾶσαι", false},
    {Nom, Sg, N, "πᾶν", false},   {Gen, Sg, N, "παντός", false}, {Dat, Sg, N, "παντί", false}, {Acc, Sg, N, "πᾶν", false},
    {Nom, Pl, N, "πάντα", false}, {Gen, Pl, N, "πάντων", false}, {Dat, Pl, N, "πᾶσι", false}, {Acc, Pl, N, "πάντα", false},
};

const ClosedTable kClosed[] = {
    {"ὁ", 3, kArticle, VP_N(kArticle)},        {"ἐγώ", 1, kEgo, VP_N(kEgo)},
    {"σύ", 2, kSu, VP_N(kSu)},                  {"ἡμεῖσ", 1, kHemeis, VP_N(kHemeis)},
    {"ὑμεῖσ", 2, kHumeis, VP_N(kHumeis)},       {"αὐτόσ", 3, kAutos, VP_N(kAutos)},
    {"οὗτοσ", 3, kHoutos, VP_N(kHoutos)},       {"ἐκεῖνοσ", 3, kEkeinos, VP_N(kEkeinos)},
    {"ὅσ", 3, kHos, VP_N(kHos)},                {"τίσ", 3, kTis, VP_N(kTis)},
    {"τισ", 3, kTisIndef, VP_N(kTisIndef)},     {"οὐδείσ", 3, kOudeis, VP_N(kOudeis)},
    {"μηδείσ", 3, kMedeis, VP_N(kMedeis)},      {"εἷσ", 3, kHeis, VP_N(kHeis)},
    {"δύο", 3, kDuo, VP_N(kDuo)},               {"τρεῖσ", 3, kTreis, VP_N(kTreis)},
    {"τέτταρεσ", 3, kTettares, VP_N(kTettares)}, {"πᾶσ", 3, kPas, VP_N(kPas)},
};
#undef VP_N

const ClosedTable* closedTable(std::string_view key) {
  for (const ClosedTable& t : kClosed)
    if (key == t.key) return &t;
  return nullptr;
}

}  // namespace

bool hasClosedTable(std::string_view lemmaKey) { return closedTable(lemmaKey) != nullptr; }

bool closedForm(std::string_view lemmaKey, uint8_t case_, uint8_t number, uint8_t gender, bool enclitic,
                std::string& out) {
  const ClosedTable* t = closedTable(lemmaKey);
  if (!t) return false;
  const uint8_t g = gender == F || gender == N ? gender : (uint8_t)M;
  const CF* best = nullptr;
  int bestScore = -1;
  for (size_t i = 0; i < t->count; ++i) {
    const CF& c = t->cells[i];
    if (c.c != case_) continue;
    if (number && c.n != number) continue;
    if (c.g && c.g != g) continue;
    int s = 1;
    if (c.enc == enclitic) s += 2;
    if (s > bestScore) { bestScore = s; best = &c; }
  }
  if (!best) return false;
  out = best->form;
  return true;
}

bool article(uint8_t case_, uint8_t number, uint8_t gender, std::string& out) {
  return closedForm("ὁ", case_, number ? number : (uint8_t)Sg, gender, false, out);
}

void closedReadings(std::string_view word, std::vector<ClosedReading>& out) {
  const std::string k = text::greek_key(ultimaToAcute(word));
  const std::string kb = text::greek_key(stripAccents(word));
  for (const ClosedTable& t : kClosed)
    for (size_t i = 0; i < t.count; ++i) {
      const CF& c = t.cells[i];
      const std::string ck = text::greek_key(c.form);
      // enclitic forms match with or without their accent
      const bool hit = ck == k || (c.enc && text::greek_key(stripAccents(c.form)) == kb);
      if (!hit) continue;
      bool dup = false;
      for (const ClosedReading& r : out)
        dup = dup || (std::string_view(r.lemmaKey) == t.key && r.case_ == c.c && r.number == c.n && r.gender == c.g);
      if (!dup) out.push_back(ClosedReading{t.key, c.c, c.n, c.g, c.enc, t.person});
    }
}

// ---- findLemma ----------------------------------------------------------------------------------------------------
uint32_t findLemma(const lex::Lexicon& lx, std::string_view head, uint8_t pos) {
  const std::string key = text::greek_key(head);
  if (key.empty()) return lex::kNoLemma;
  thread_local std::vector<lex::Analysis> an;
  an.clear();
  lx.lookup(key, an);
  const std::string want = display(head);
  uint32_t best = lex::kNoLemma;
  long bestScore = -1;
  for (const lex::Analysis& a : an) {
    const lex::Lemma l = lx.lemma(a.lemma);
    if (l.id == lex::kNoLemma || l.key != key) continue;
    if (pos && l.pos != pos) continue;
    long s = 0;
    if (display(l.head) == want) s += 1L << 20;
    if (l.flags & lex::HasTable) s += 1L << 19;
    s += (long)(l.tier ? (4 - l.tier) : 0) << 16;
    s += 65535 - (l.freqRank ? l.freqRank : 65535);
    if (s > bestScore || (s == bestScore && a.lemma < best)) { bestScore = s; best = a.lemma; }
  }
  return best;
}

// ---- rule paradigm ------------------------------------------------------------------------------------------------
namespace {

constexpr char32_t kAcuteM = 0x0301, kCircM = 0x0342, kMacronM = 0x0304, kBreveM = 0x0306, kIotaSubM = 0x0345;

bool vowel(char32_t c) {
  switch (c) {
    case U'α': case U'ε': case U'η': case U'ι': case U'ο': case U'υ': case U'ω': return true;
    default: return false;
  }
}
bool mark(char32_t c) { return c >= 0x0300 && c <= 0x036F; }

struct Syll { size_t last; bool longV; };
// Nuclei of a lower-case NFD word with length marks; `longV` by nature (η ω, diphthong, macron, circumflex, iota
// subscript; α ι υ without a mark count short).
void syllables(const std::u32string& u, std::vector<Syll>& out) {
  out.clear();
  size_t prevVowel = std::u32string::npos;
  bool prevBare = false;
  for (size_t i = 0; i < u.size(); ++i) {
    if (!vowel(u[i])) { if (!mark(u[i])) prevVowel = std::u32string::npos; continue; }
    size_t j = i + 1;
    bool diaer = false, mac = false, brv = false, circ = false, sub = false;
    while (j < u.size() && mark(u[j])) {
      diaer |= u[j] == 0x0308; mac |= u[j] == kMacronM; brv |= u[j] == kBreveM; circ |= u[j] == kCircM;
      sub |= u[j] == kIotaSubM;
      ++j;
    }
    const bool diph = prevVowel != std::u32string::npos && prevBare && prevVowel + 1 == i && !diaer &&
                      ((u[i] == U'ι' && (u[prevVowel] == U'α' || u[prevVowel] == U'ε' || u[prevVowel] == U'ο' ||
                                         u[prevVowel] == U'υ')) ||
                       (u[i] == U'υ' && (u[prevVowel] == U'α' || u[prevVowel] == U'ε' || u[prevVowel] == U'η' ||
                                         u[prevVowel] == U'ο')));
    if (diph && !out.empty()) {
      out.back().last = i;
      out.back().longV = true;
    } else {
      const bool lng = u[i] == U'η' || u[i] == U'ω' || mac || circ || sub;
      (void)brv;
      out.push_back(Syll{i, lng});
    }
    prevVowel = i;
    prevBare = j == i + 1;
    i = j - 1;
  }
}

// Places the accent of a form whose head had its accent on syllable `fromStart` (0-based from the start).
// `ultimaLong`: the ending is long; `oxyCirc`: an oxytone takes the circumflex in this cell (gen / dat).
std::string accentForm(const std::u32string& stem, const char* ending, int fromStart, bool ultimaLong, bool oxyCirc,
                       bool finalShortDiph) {
  std::u32string u = stem + text::toUtf32(text::nfd(ending));
  std::vector<Syll> sy;
  syllables(u, sy);
  if (sy.empty()) return text::nfc(text::toUtf8(u));
  const int n = (int)sy.size();
  int at = std::min(fromStart, n - 1);
  const bool uLong = ultimaLong && !finalShortDiph;
  if (n - 1 - at >= 2 && uLong) at = n - 2;   // antepenult before a long ultima -> penult
  if (n - 1 - at > 2) at = n - 3;
  char32_t mk = kAcuteM;
  if (at == n - 1) mk = oxyCirc ? kCircM : kAcuteM;
  else if (at == n - 2 && sy[(size_t)at].longV && !uLong) mk = kCircM;
  size_t ins = sy[(size_t)at].last + 1;
  while (ins < u.size() && mark(u[ins]) && u[ins] != kIotaSubM) ++ins;
  u.insert(u.begin() + (long)ins, mk);
  return text::nfc(text::toUtf8(u));
}

struct Ending { uint8_t c, n; const char* e; bool lng; bool diphShort; };
const Ending k2M[] = {{Nom, Sg, "ος", false, false}, {Gen, Sg, "ου", true, false}, {Dat, Sg, "ῳ", true, false},
                      {Acc, Sg, "ον", false, false}, {Voc, Sg, "ε", false, false}, {Nom, Pl, "οι", true, true},
                      {Gen, Pl, "ων", true, false}, {Dat, Pl, "οις", true, false}, {Acc, Pl, "ους", true, false},
                      {Voc, Pl, "οι", true, true}};
const Ending k2N[] = {{Nom, Sg, "ον", false, false}, {Gen, Sg, "ου", true, false}, {Dat, Sg, "ῳ", true, false},
                      {Acc, Sg, "ον", false, false}, {Voc, Sg, "ον", false, false}, {Nom, Pl, "α", false, false},
                      {Gen, Pl, "ων", true, false}, {Dat, Pl, "οις", true, false}, {Acc, Pl, "α", false, false},
                      {Voc, Pl, "α", false, false}};
const Ending k1Eta[] = {{Nom, Sg, "η", true, false}, {Gen, Sg, "ης", true, false}, {Dat, Sg, "ῃ", true, false},
                        {Acc, Sg, "ην", true, false}, {Voc, Sg, "η", true, false}, {Nom, Pl, "αι", true, true},
                        {Gen, Pl, "ων", true, false}, {Dat, Pl, "αις", true, false}, {Acc, Pl, "ᾱς", true, false},
                        {Voc, Pl, "αι", true, true}};
const Ending k1Alpha[] = {{Nom, Sg, "ᾱ", true, false}, {Gen, Sg, "ᾱς", true, false}, {Dat, Sg, "ᾳ", true, false},
                          {Acc, Sg, "ᾱν", true, false}, {Voc, Sg, "ᾱ", true, false}, {Nom, Pl, "αι", true, true},
                          {Gen, Pl, "ων", true, false}, {Dat, Pl, "αις", true, false}, {Acc, Pl, "ᾱς", true, false},
                          {Voc, Pl, "αι", true, true}};

// The head without accents (NFD, lower case, length marks kept), its accent syllable from the start, and whether it
// was oxytone.
bool headInfo(std::string_view head, std::u32string& noAcc, int& fromStart, bool& oxy) {
  std::u32string u = text::toUtf32(text::nfd(text::lower(head)));
  std::vector<Syll> sy;
  syllables(u, sy);
  fromStart = -1;
  // the accent mark belongs to the last syllable whose last vowel precedes it
  for (size_t k = 0; k < u.size() && fromStart < 0; ++k) {
    if (!(u[k] == kAcuteM || u[k] == kCircM || u[k] == 0x0300)) continue;
    for (size_t s = sy.size(); s-- > 0;)
      if (sy[s].last < k) { fromStart = (int)s; break; }
  }
  if (fromStart < 0 || sy.empty()) return false;
  oxy = fromStart == (int)sy.size() - 1;
  u.erase(std::remove_if(u.begin(), u.end(), [](char32_t c) { return c == kAcuteM || c == kCircM || c == 0x0300; }),
          u.end());
  noAcc = u;
  return true;
}

bool endsWithU(const std::u32string& u, std::u32string_view t) {
  return u.size() >= t.size() && u.compare(u.size() - t.size(), t.size(), t) == 0;
}

void decline(const std::u32string& stem, int fromStart, bool oxy, const Ending* tab, size_t count, uint8_t pos,
             uint8_t gender, std::vector<RuleCell>& out) {
  for (size_t i = 0; i < count; ++i) {
    const Ending& e = tab[i];
    const bool oxyCirc = oxy && (e.c == Gen || e.c == Dat);
    std::string f = accentForm(stem, e.e, fromStart, e.lng, oxyCirc, e.diphShort);
    Features ft;
    ft.pos = pos;
    ft.case_ = e.c;
    ft.number = e.n;
    ft.gender = pos == Adj ? gender : 0;
    out.push_back(RuleCell{pack(ft), display(f)});
  }
}

}  // namespace

bool paradigmApplies(const lex::Lemma& l) {
  if (l.id == lex::kNoLemma || (l.flags & lex::HasTable) || (l.flags & lex::Indeclinable)) return false;
  const std::string b = text::greek_bare(l.head);
  auto ends = [&](const char* t) {
    const std::string_view s(t);
    return b.size() > s.size() && b.compare(b.size() - s.size(), s.size(), s) == 0;
  };
  if (l.pos == Noun) return (ends("οσ") || ends("ος") || ends("ον")) && (l.cls == 2 || l.cls == 0);
  if (l.pos == Adj) return ends("οσ") || ends("ος");
  return false;
}

void paradigm(const lex::Lemma& l, std::vector<RuleCell>& out) {
  out.clear();
  if (!paradigmApplies(l)) return;
  std::u32string u;
  int fromStart = 0;
  bool oxy = false;
  if (!headInfo(l.head, u, fromStart, oxy)) return;
  // strip the final sigma / nu ending: -ος (ς or σ) or -ον
  std::u32string stem;
  if (endsWithU(u, U"ος") || endsWithU(u, U"οσ")) stem = u.substr(0, u.size() - 2);
  else if (endsWithU(u, U"ον")) stem = u.substr(0, u.size() - 2);
  else return;
  if (l.pos == Noun) {
    const bool neuter = endsWithU(u, U"ον") || l.gender == N;
    if (neuter) decline(stem, fromStart, oxy, k2N, sizeof k2N / sizeof k2N[0], Noun, 0, out);
    else decline(stem, fromStart, oxy, k2M, sizeof k2M / sizeof k2M[0], Noun, 0, out);
    return;
  }
  // adjective: feminine from the principal line ("feminine X"), else by the stem's last letter
  const morph::Principal p = morph::parsePrincipal(l.head, l.principal);
  bool twoTermination = false, alpha = false;
  if (!p.feminine.empty()) {
    const std::string fb = text::greek_bare(p.feminine);
    auto ends = [&](std::string_view t) { return fb.size() >= t.size() && fb.compare(fb.size() - t.size(), t.size(), t) == 0; };
    if (ends("οσ") || ends("ος")) twoTermination = true;
    else if (ends("α")) alpha = true;
  } else if (!stem.empty()) {
    size_t k = stem.size();
    while (k > 0 && mark(stem[k - 1])) --k;
    const char32_t last = k ? stem[k - 1] : 0;
    alpha = last == U'ε' || last == U'ι' || last == U'ρ';
  }
  decline(stem, fromStart, oxy, k2M, sizeof k2M / sizeof k2M[0], Adj, M, out);
  if (twoTermination) decline(stem, fromStart, oxy, k2M, sizeof k2M / sizeof k2M[0], Adj, F, out);
  else if (alpha) decline(stem, fromStart, oxy, k1Alpha, sizeof k1Alpha / sizeof k1Alpha[0], Adj, F, out);
  else decline(stem, fromStart, oxy, k1Eta, sizeof k1Eta / sizeof k1Eta[0], Adj, F, out);
  decline(stem, fromStart, oxy, k2N, sizeof k2N / sizeof k2N[0], Adj, N, out);
}

// ---- generate -----------------------------------------------------------------------------------------------------
namespace {

bool middleForm(const lex::Lemma& l) {
  if (l.flags & lex::Deponent) return true;
  const std::string b = text::greek_bare(l.head);
  return b.size() >= 6 && b.compare(b.size() - 6, 6, "μαι") == 0;
}

// Voice preference list for a request; rank 0 best.
int voiceRank(uint8_t cell, uint8_t want, bool midForm, uint8_t tense) {
  if (want == 0 || want == Active) {
    // middle-form lemmas: the middle in the present system; an active cell wins elsewhere (ἦλθον, διελθεῖν)
    const bool presentSystem = tense == Present || tense == Imperfect || tense == 0;
    if (midForm && presentSystem) return cell == Middle ? 0 : cell == Passive ? 1 : cell == Active ? 2 : cell == 0 ? 3 : -1;
    if (midForm) return cell == Active ? 0 : cell == Middle ? 1 : cell == Passive ? 2 : cell == 0 ? 3 : -1;
    return cell == Active ? 0 : cell == 0 ? 1 : cell == Middle ? 3 : -1;   // middle futures: ἔσομαι, ὄψομαι
  }
  if (want == Middle) return cell == Middle ? 0 : cell == Passive ? 1 : -1;
  if (want == Passive) return cell == Passive ? 0 : cell == Middle ? 1 : -1;
  return cell == want ? 0 : -1;
}

bool allNonAttic(const lex::Lexicon& lx, uint32_t lemma, const std::string& form, const Features& cf) {
  thread_local std::vector<lex::Analysis> an;
  an.clear();
  lx.lookup(text::greek_key(form), an);
  bool any = false, all = true;
  for (const lex::Analysis& a : an) {
    if (a.lemma != lemma) continue;
    const Features f = unpack(lx.feature(a.feat));
    if (f.case_ != cf.case_ || f.number != cf.number || f.person != cf.person || f.tense != cf.tense ||
        f.mood != cf.mood)
      continue;
    any = true;
    all = all && (a.flags & lex::NonAttic);
  }
  return any && all;
}

// Attic forms the lexicon attests through a form-page analysis but whose table cell holds another dialect's form.
// Used only when the lexicon has an analysis of exactly this form for the lemma with these features.
// `anyDialect` (C21): the lexicon lists the form for this lemma and cell but flags it non-Attic by mistake (a root
// aorist taken from an Epic table): accepted all the same.
struct Override {
  const char* lemmaKey; uint8_t tense, mood, person, number; const char* form; bool nu = false; bool anyDialect = false;
};
const Override kOverrides[] = {
    {"εἰμί", Imperfect, Indicative, P2, Sg, "ἦσθα"},   // the Attic table cell has ἦς
    {"πίνω", Aorist, Imperative, P2, Sg, "πῖθι"},      // the table cell has πίε
    {"δεῖ", Present, Indicative, P3, Sg, "δεῖ"},        // the "Attic" table of δεῖ is uncontracted (δέει)
    {"δεῖ", Imperfect, Indicative, P3, Sg, "ἔδει"},
    {"βούλομαι", Present, Indicative, P2, Sg, "βούλει"},   // Attic prose -ει (the cell has βούλῃ)
    {"οἴομαι", Present, Indicative, P2, Sg, "οἴει"},
    // C16: classical Attic augments the readers use where the table's first (or only Attic-flagged) cell differs
    {"εὑρίσκω", Aorist, Indicative, P1, Sg, "ηὗρον"},   // the cell has εὗρον (later Attic)
    {"εὑρίσκω", Aorist, Indicative, P2, Sg, "ηὗρες"},
    {"εὑρίσκω", Aorist, Indicative, P3, Sg, "ηὗρε", true},
    {"εὑρίσκω", Aorist, Indicative, P1, Pl, "ηὕρομεν"},
    {"εὑρίσκω", Aorist, Indicative, P2, Pl, "ηὕρετε"},
    {"εὑρίσκω", Aorist, Indicative, P3, Pl, "ηὗρον"},
    {"βούλομαι", Imperfect, Indicative, P1, Sg, "ἐβουλόμην"},   // the Attic-flagged cells have ἠβουλ- (later)
    {"βούλομαι", Imperfect, Indicative, P2, Sg, "ἐβούλου"},
    {"βούλομαι", Imperfect, Indicative, P3, Sg, "ἐβούλετο"},
    {"βούλομαι", Imperfect, Indicative, P1, Pl, "ἐβουλόμεθα"},
    {"βούλομαι", Imperfect, Indicative, P2, Pl, "ἐβούλεσθε"},
    {"βούλομαι", Imperfect, Indicative, P3, Pl, "ἐβούλοντο"},
    {"οἴομαι", Imperfect, Indicative, P1, Sg, "ᾤμην"},   // the Attic-flagged cell ᾤομην is a table error
    // C21: the intransitive root aorist of δύω "set, sink" (Xen. "ἐπεὶ δὲ ἥλιος ἔδυ"): the lexicon flags the cells
    // non-Attic, so the generator took the middle ἐδύσατο
    {"δύω", Aorist, Indicative, P1, Sg, "ἔδυν", false, true},
    {"δύω", Aorist, Indicative, P2, Sg, "ἔδυς", false, true},
    {"δύω", Aorist, Indicative, P3, Sg, "ἔδυ", false, true},
    {"δύω", Aorist, Indicative, P3, Pl, "ἔδυσαν", false, true},
    // C29: the perfect of ἵστημι "I stand": the table's first cells are Doric (ἕστακα)
    {"ἵστημι", Perfect, Indicative, P1, Sg, "ἕστηκα", false, true},
    {"ἵστημι", Perfect, Indicative, P2, Sg, "ἕστηκας", false, true},
    {"ἵστημι", Perfect, Indicative, P3, Sg, "ἕστηκε", true, true},
    {"ἵστημι", Perfect, Indicative, P3, Pl, "ἑστᾶσι", true, true},
    // C29: ἀνίστημι in "stand up / get up" (lexical_en_grc.tsv, active outside the present system): the intransitive
    // root aorist, not ἀνέστησε "raised up"
    {"ἀνίστημι", Aorist, Indicative, P1, Sg, "ἀνέστην", false, true},
    {"ἀνίστημι", Aorist, Indicative, P2, Sg, "ἀνέστης", false, true},
    {"ἀνίστημι", Aorist, Indicative, P3, Sg, "ἀνέστη", false, true},
    {"ἀνίστημι", Aorist, Indicative, P1, Pl, "ἀνέστημεν", false, true},
    {"ἀνίστημι", Aorist, Indicative, P3, Pl, "ἀνέστησαν", false, true},
    {"ἀνίστημι", Aorist, Imperative, P2, Sg, "ἀνάστηθι", false, true},
    {"ἀνίστημι", Aorist, Imperative, P2, Pl, "ἀνάστητε", false, true},
};

// C21: the simplex verbs with a middle future in Attic (Smyth 805-806; our own selection of the common ones); a
// compound ends in its simplex (ἀναγιγνώσκω, ἀποπλέω)
bool futureMiddle(std::string_view key) {
  static const char* const kList[] = {"ἀκούω", "γιγνώσκω", "ὁράω", "λαμβάνω", "μανθάνω", "φεύγω", "πίνω", "ἐσθίω",
                                      "πάσχω", "βαίνω", "τρέχω", "πλέω", "θαυμάζω", "ᾄδω", "βοάω", "γελάω", "κάμνω",
                                      "πίπτω", "θνῄσκω", "τίκτω", "νέω", "πνέω", "κλαίω", "σιγάω", "σιωπάω"};
  static const char* const kPrefix[] = {"", "ἀνα", "ἀν", "ἀπο", "ἀπ", "ἀφ", "ἐκ", "ἐξ", "εἰσ", "κατα", "κατ", "καθ",
                                        "δια", "δι", "μετα", "μετ", "μεθ", "παρα", "παρ", "περι", "προσ", "συν",
                                        "συμ", "ὑπο", "ὑπ", "ὑφ", "ἐπι", "ἐπ", "ἐφ", "προ", "ἀμφι", "ἐν", "ἐμ"};
  static const std::vector<std::string> list = [] {
    std::vector<std::string> v;
    for (const char* k : kList) v.push_back(text::greek_key(k));
    return v;
  }();
  static const std::vector<std::string> prefixes = [] {
    std::vector<std::string> v;
    for (const char* p : kPrefix) v.push_back(text::greek_key(p));
    return v;
  }();
  for (const std::string& kk : list) {
    if (key.size() < kk.size() || key.compare(key.size() - kk.size(), kk.size(), kk) != 0) continue;
    const std::string_view pre = key.substr(0, key.size() - kk.size());
    for (const std::string& p : prefixes)
      if (pre == p) return true;
  }
  return false;
}

bool attested(const lex::Lexicon& lx, uint32_t lemma, const char* form, const Features& want, bool anyDialect = false) {
  thread_local std::vector<lex::Analysis> an;
  an.clear();
  lx.lookup(text::greek_key(form), an);
  for (const lex::Analysis& a : an) {
    if (a.lemma != lemma || ((a.flags & lex::NonAttic) && !anyDialect)) continue;
    const Features f = unpack(lx.feature(a.feat));
    if (f.tense == want.tense && f.mood == want.mood && f.person == want.person && f.number == want.number) return true;
  }
  return false;
}

}  // namespace

// C25: the first letter of a word with its breathing swapped (rough <-> smooth) when it is an epsilon; empty otherwise
namespace {
std::string swapEpsilonBreathing(const std::string& w, char32_t from, char32_t to) {
  std::u32string u = text::toUtf32(text::nfd(w));
  if (u.size() < 3 || u[0] != U'ε') return std::string();
  size_t i = 1;
  bool hit = false;
  while (i < u.size() && u[i] >= 0x0300 && u[i] <= 0x036F) {
    if (u[i] == from) { u[i] = to; hit = true; }
    ++i;
  }
  return hit ? text::nfc(text::toUtf8(u)) : std::string();
}
std::string smoothAugment(const std::string& form, std::string_view lemmaKey) {
  // the lemma begins with a consonant and the form is that consonant after an augment ε with a rough breathing
  const std::u32string lk = text::toUtf32(text::greek_bare(std::string(lemmaKey)));
  const std::u32string fb = text::toUtf32(text::greek_bare(form));
  if (lk.empty() || fb.size() < 3 || fb[0] != U'ε') return form;
  const std::u32string vowels = U"αεηιουω";
  if (vowels.find(lk[0]) != std::u32string::npos || fb[1] != lk[0]) return form;
  const std::string r = swapEpsilonBreathing(form, 0x0314, 0x0313);
  return r.empty() ? form : r;
}
// C29: a table that writes the syllabic augment before a verb in ε- (ἐνυπνιάζω: ἐἐνυπνίασε) is wrong: ε + ε is the
// temporal augment η (ἠνυπνίασε). `toEta` makes the Attic form from the table's, `!toEta` the table's from the Attic.
std::string doubleAugment(const std::string& form, bool toEta) {
  std::u32string u = text::toUtf32(text::nfd(form));
  if (toEta) {
    if (u.size() < 5 || u[0] != U'ε' || u[1] != 0x0313 || u[2] != U'ε' || (u[3] != 0x0313 && u[3] != 0x0314)) return std::string();
    std::u32string r = U"η";
    r.push_back(u[3]);
    r += u.substr(4);
    return text::nfc(text::toUtf8(r));
  }
  if (u.size() < 4 || u[0] != U'η' || (u[1] != 0x0313 && u[1] != 0x0314) || (u[2] >= 0x0300 && u[2] <= 0x036F))
    return std::string();
  std::u32string r = U"ε";
  r.push_back(0x0313);
  r.push_back(U'ε');
  r.push_back(u[1]);
  r += u.substr(2);
  return text::nfc(text::toUtf8(r));
}
std::string roughAugment(const std::string& form) {
  const std::u32string fb = text::toUtf32(text::greek_bare(form));
  const std::u32string vowels = U"αεηιουω";
  if (fb.size() < 3 || fb[0] != U'ε' || vowels.find(fb[1]) != std::u32string::npos) return std::string();
  return swapEpsilonBreathing(form, 0x0313, 0x0314);
}
}  // namespace

bool generate(const lex::Lexicon& lx, uint32_t lemma, const Features& want0, std::string& out, GenInfo* info) {
  GenInfo gi;
  const lex::Lemma l = lx.lemma(lemma);
  if (l.id == lex::kNoLemma) return false;
  Features want = want0;
  // built-in closed classes
  if (hasClosedTable(l.key) && want.case_ != 0 &&
      (l.pos == Pron || l.pos == feat::Det || l.pos == Article || l.pos == Num || l.pos == Adj)) {
    if (closedForm(l.key, want.case_, want.number, want.gender, false, out)) {
      gi.closed = gi.exact = true;
      Features f = want;
      f.pos = l.pos;
      gi.packed = pack(f);
      gi.movableNu = movableNuCandidate(out, want);
      if (info) *info = gi;
      return true;
    }
  }
  for (const Override& ov : kOverrides)
    if (l.key == ov.lemmaKey && want.tense == ov.tense && want.mood == ov.mood && want.person == ov.person &&
        want.number == ov.number && (want.voice == 0 || want.voice == Active) && attested(lx, lemma, ov.form, want, ov.anyDialect)) {
      out = ov.form;
      gi.exact = gi.attic = true;
      gi.movableNu = ov.nu;
      Features f = want;
      f.voice = Active;
      gi.packed = pack(f);
      if (info) *info = gi;
      return true;
    }
  thread_local std::vector<std::pair<uint32_t, std::string_view>> cells;
  cells.clear();
  lx.cells(lemma, cells);
  const bool midForm = middleForm(l);
  // οἶδα: no present cells but perfect cells -> perfect for the present, pluperfect for the imperfect
  if (want.pos == Verb && (want.tense == Present || want.tense == Imperfect)) {
    bool pres = false, perf = false;
    for (const auto& c : cells) {
      const Features f = unpack(c.first);
      pres = pres || f.tense == Present;
      perf = perf || f.tense == Perfect;
    }
    if (!pres && perf) {
      want.tense = want.tense == Present ? (uint8_t)Perfect : (uint8_t)Pluperfect;
      gi.presentAsPerfect = true;
    }
  }
  // C21: verbs whose Attic future is middle in form (ἀκούσομαι, γνώσομαι, ὄψομαι, λήψομαι, φεύξομαι, πλεύσομαι ...):
  // an active future request takes the middle cells when the table has them (the tables list ἀναγνώσω first)
  if (want.pos == Verb && want.tense == Future && (want.voice == 0 || want.voice == Active) && futureMiddle(l.key)) {
    bool mid = false;
    for (const auto& c : cells) {
      const Features f = unpack(c.first);
      mid = mid || (f.tense == Future && f.voice == Middle && f.mood == want.mood);
    }
    if (mid) want.voice = Middle;
  }
  int best = -1000;
  std::string bestForm;
  uint32_t bestPacked = 0;
  bool bestNu = false;
  for (const auto& c : cells) {
    const Features cf = unpack(c.first);
    if (cf.case_ != want.case_ || cf.person != want.person || cf.tense != want.tense || cf.mood != want.mood) continue;
    if (want.pos == Verb && cf.pos && cf.pos != Verb) continue;
    if (want.pos && want.pos != Verb && cf.pos == Verb) continue;
    int s = 0;
    if (cf.number != want.number) {
      if (want.number == 0) s -= 1;
      else if (cf.number == 0 && cf.case_ == 0 && cf.person == 0) s -= 2;
      else continue;
    }
    if (want.gender && cf.gender) {
      if (cf.gender == want.gender) s += 4;
      else if (morph::genderAdmits(cf.gender, want.gender)) s += 2;
      else continue;
    }
    const int vr = voiceRank(cf.voice, want.voice, midForm, want.tense);
    if (vr < 0) continue;
    s -= 3 * vr;
    const bool cPos = cf.degree <= Positive, wPos = want.degree <= Positive;
    if (cf.degree != want.degree && !(cPos && wPos)) continue;
    if (cf.extra & Alternative) s -= 6;
    if (cf.extra & Attic) s += 8;
    if (cf.extra & Contracted) s += 4;
    bool nu = false;
    std::string f = cleanCell(c.second, &nu);
    if (f.empty()) continue;
    if (!(cf.extra & Attic) && allNonAttic(lx, lemma, text::nfc(c.second), cf)) s -= 10;
    if (s > best || (s == best && f < bestForm)) {
      best = s;
      bestForm = std::move(f);
      bestPacked = c.first;
      bestNu = nu;
    }
  }
  if (best > -1000) {
    // C18: a compound of προσ- whose table writes the augment before the prefix (ἐπροσκύνησα): Attic augments after
    // the preposition (προσεκύνησα); the analysis maps the form back (see analyse)
    if (bestForm.compare(0, std::strlen("ἐπροσ"), "ἐπροσ") == 0 && bestForm.size() > std::strlen("ἐπροσ") + 2)
      bestForm = "προσε" + bestForm.substr(std::strlen("ἐπροσ"));
    // C25: a syllabic augment written with a rough breathing (νίζω: ἕνιψα, ἑνίψατο) is a table error: the augment of
    // a verb that begins with a consonant is always ἐ- (ἔνιψα); the analysis maps the form back (see analyse)
    if (l.pos == Verb) bestForm = smoothAugment(bestForm, l.key);
    if (l.pos == Verb)
      if (const std::string eta = doubleAugment(bestForm, true); !eta.empty()) bestForm = eta;
    // C31: compounds of ἔρχομαι whose aorist indicative cells lack the augment (ἐπανέρχομαι: ἐπάνελθον, ἐπανέλθομεν):
    // Attic augments after the prefix, as ἦλθον: ἐπανῆλθον, ἐπανήλθομεν (the accent cannot go back past the augment)
    if (l.pos == Verb && want.tense == Aorist && want.mood == Indicative) {
      const std::string key(l.key);
      const std::string erx = text::greek_key("έρχομαι");
      const std::string elth = text::nfc("ελθ");
      const std::string b = stripAccents(bestForm);
      const size_t at = b.find(elth);
      if (key.size() > erx.size() && key.compare(key.size() - erx.size(), erx.size(), erx) == 0 && at != std::string::npos &&
          at > 0) {
        const std::string ending = b.substr(at + elth.size());
        const bool shortEnd = ending == text::nfc("ον") || ending == text::nfc("ες") || ending == text::nfc("ε") ||
                              ending == text::nfc("εν");
        bestForm = b.substr(0, at) + text::nfc(shortEnd ? "ῆλθ" : "ήλθ") + ending;
      }
    }
    out = bestForm;
    Features pf = unpack(bestPacked);
    gi.attic = (pf.extra & Attic) != 0;
    gi.contracted = (pf.extra & Contracted) != 0;
    pf.extra &= (uint8_t)~(Attic | Contracted | Alternative);
    gi.packed = pack(pf);
    gi.exact = pf.case_ == want.case_ && pf.number == want.number && pf.person == want.person &&
               pf.tense == want.tense && pf.mood == want.mood;
    gi.movableNu = bestNu || movableNuCandidate(out, pf);
    if (info) *info = gi;
    return true;
  }
  // C31: a comparative in -τερος whose table lists only the citation form (νέος: νεώτερος) is declined by rule like
  // an o / a-stem adjective, the accent on the penult when the ultima is long (νεωτέρου, νεωτέρᾳ, νεωτέρα), else where
  // the citation form has it (νεώτερος, νεώτεροι, νεώτερα). From a rule: Check
  if (want.pos == Adj && want.degree == Comparative && want.case_) {
    for (const auto& c : cells) {
      const Features cf = unpack(c.first);
      if (cf.degree != Comparative || cf.case_ != 0) continue;
      const std::string cit = cleanCell(c.second);
      const std::string tail = text::nfc("ερος");
      if (cit.size() <= tail.size() + 2 || cit.compare(cit.size() - tail.size(), tail.size(), tail) != 0) continue;
      const std::string stemA = cit.substr(0, cit.size() - tail.size());   // νεώτ
      const std::string stemB = stripAccents(stemA);                       // νεωτ
      const uint8_t g = want.gender ? want.gender : (uint8_t)M;
      const bool pl = want.number == Pl;
      const char* shortEnd = nullptr;
      const char* longEnd = nullptr;
      switch (want.case_) {
        case Nom: case Voc:
          if (g == F) { if (pl) shortEnd = "εραι"; else longEnd = "έρα"; }
          else if (g == N) shortEnd = pl ? "ερα" : "ερον";
          else shortEnd = pl ? "εροι" : (want.case_ == Voc ? "ερε" : "ερος");
          break;
        case Acc:
          if (g == F) longEnd = pl ? "έρας" : "έραν";
          else if (g == N) shortEnd = pl ? "ερα" : "ερον";
          else if (pl) longEnd = "έρους";
          else shortEnd = "ερον";
          break;
        case Gen: longEnd = pl ? "έρων" : (g == F ? "έρας" : "έρου"); break;
        case Dat: longEnd = pl ? (g == F ? "έραις" : "έροις") : (g == F ? "έρᾳ" : "έρῳ"); break;
        default: break;
      }
      if (!shortEnd && !longEnd) break;
      out = shortEnd ? stemA + text::nfc(shortEnd) : stemB + text::nfc(longEnd);
      gi.fromRule = true;
      Features pf = want;
      pf.extra = 0;
      gi.packed = pack(pf);
      gi.movableNu = false;
      if (info) *info = gi;
      return true;
    }
  }
  // rule paradigm (lemmas without a table only)
  if (paradigmApplies(l)) {
    thread_local std::vector<RuleCell> rc;
    paradigm(l, rc);
    for (const RuleCell& r : rc) {
      const Features cf = unpack(r.packed);
      if (cf.case_ != want.case_ || cf.number != want.number) continue;
      if (want.gender && cf.gender && cf.gender != want.gender) continue;
      out = r.form;
      gi.fromRule = true;
      gi.packed = r.packed;
      gi.movableNu = false;
      if (info) *info = gi;
      return true;
    }
  }
  return false;
}

// ---- participles (C18) ----------------------------------------------------------------------------------------------
namespace {

bool endsNfc(const std::string& w, const char* e) {
  const std::string t = text::nfc(e);
  return w.size() > t.size() && w.compare(w.size() - t.size(), t.size(), t) == 0;
}
std::string cut(const std::string& w, const char* e) { return w.substr(0, w.size() - text::nfc(e).size()); }

// Nominative plural of a participle from its nominative singular cells (the tables list the participles only as
// masculine / feminine / neuter nominatives without number). The accent stays on the same vowel: τρέχων -> τρέχοντες,
// δραμών -> δραμόντες, ποιῶν (ποιοῦσα) -> ποιοῦντες, λύσας -> λύσαντες, λυθείς -> λυθέντες, λελυκώς -> λελυκότες,
// λυόμενος -> λυόμενοι; feminine: + ι (τρέχουσαι, ἰδοῦσαι), -μένη from the masculine (λυόμεναι); neuter from the
// masculine (τρέχοντα, λυόμενα). Empty when the ending is not one of these.
std::string masculinePlural(const std::string& m, const std::string& f) {
  if (m == text::nfc("ὤν")) return text::nfc("ὄντες");
  if (endsNfc(m, "ῶν")) {   // contracted present: the feminine stem (ποιοῦσα, τιμῶσα, δηλοῦσα)
    if (!endsNfc(f, "σα")) return std::string();
    return cut(f, "σα") + text::nfc("ντες");
  }
  if (endsNfc(m, "ών")) return cut(m, "ών") + text::nfc("όντες");
  if (endsNfc(m, "ων")) return cut(m, "ων") + text::nfc("οντες");
  if (endsNfc(m, "είς")) return cut(m, "είς") + text::nfc("έντες");
  if (endsNfc(m, "ώς")) return cut(m, "ώς") + text::nfc("ότες");
  if (endsNfc(m, "ούς")) return cut(m, "ούς") + text::nfc("όντες");
  if (endsNfc(m, "άς")) return cut(m, "άς") + text::nfc("άντες");
  if (endsNfc(m, "ύς")) return cut(m, "ύς") + text::nfc("ύντες");
  if (endsNfc(m, "ας")) return cut(m, "ας") + text::nfc("αντες");
  if (endsNfc(m, "μενος") || endsNfc(m, "μένος")) return cut(m, "ος") + text::nfc("οι");
  return std::string();
}

// C25: the word with its accents removed and an acute on the penult (the vowel group before the last one; diphthongs
// αι ει οι υι αυ ευ ηυ ου count as one): "κάμνοντων" -> "καμνόντων", "πεινῶσης" -> "πεινώσης"
std::string penultAcute(const std::string& w) {
  std::u32string u = text::toUtf32(text::nfd(stripAccents(w)));
  auto isMark = [](char32_t c) { return c >= 0x0300 && c <= 0x036F; };
  auto vowel = [](char32_t c) {
    const char32_t l = c >= U'Α' && c <= U'Ω' ? c + 0x20 : c;
    return l == U'α' || l == U'ε' || l == U'η' || l == U'ι' || l == U'ο' || l == U'υ' || l == U'ω';
  };
  std::vector<size_t> base;   // indices of base letters
  for (size_t i = 0; i < u.size(); ++i) if (!isMark(u[i])) base.push_back(i);
  auto diaeresis = [&](size_t bi) {
    for (size_t k = base[bi] + 1; k < u.size() && isMark(u[k]); ++k) if (u[k] == 0x0308) return true;
    return false;
  };
  // vowel groups from the end
  std::vector<size_t> groupLast;   // index (into base) of the last letter of each group, from the end
  for (size_t b = base.size(); b-- > 0;) {
    if (!vowel(u[base[b]])) continue;
    size_t last = b;
    if (b > 0 && vowel(u[base[b - 1]]) && !diaeresis(b)) {
      const char32_t a = u[base[b - 1]], c = u[base[b]];
      const bool diph = (c == U'ι' && (a == U'α' || a == U'ε' || a == U'ο' || a == U'υ')) ||
                        (c == U'υ' && (a == U'α' || a == U'ε' || a == U'η' || a == U'ο'));
      if (diph) --b;
    }
    groupLast.push_back(last);
    if (groupLast.size() == 2) break;
  }
  if (groupLast.size() < 2) return w;
  size_t at = base[groupLast[1]] + 1;
  while (at < u.size() && isMark(u[at]) && u[at] != 0x0345) ++at;
  u.insert(u.begin() + (long)at, 0x0301);
  return text::nfc(text::toUtf8(u));
}

// C25: the oblique cases of a participle derived from its nominatives (the tables list only nominatives): the -ντ-
// type (κάμνων κάμνουσα κάμνον; πεινῶν; ἰδών; λύσας; λυθείς; ὤν) and the -μενος type, with the accent rules of the
// third / first / second declension (genitive plural -όντων, feminine -ούσης, -ουσῶν, middle -μένου). Perfect actives
// (-ώς) are not derived. `m`, `f`, `n` are the nominatives singular; empty when no rule fits.
std::string obliqueParticiple(const std::string& m, const std::string& f, const std::string& n, uint8_t case_,
                              uint8_t number, uint8_t gender) {
  const bool pl = number == Pl;
  if (endsNfc(m, "μενος") || endsNfc(m, "μένος")) {
    const std::string st = cut(m, "ος");
    const char* e = nullptr;
    bool longE = true;
    if (gender == F) {
      if (!pl) e = case_ == Gen ? "ης" : case_ == Dat ? "ῃ" : "ην";
      else e = case_ == Gen ? "ων" : case_ == Dat ? "αις" : "ας";
    } else if (!pl) {
      if (case_ == Gen) e = "ου";
      else if (case_ == Dat) e = "ῳ";
      else { e = "ον"; longE = false; }
    } else {
      if (case_ == Gen) e = "ων";
      else if (case_ == Dat) e = "οις";
      else if (gender == N) { e = "α"; longE = false; }
      else e = "ους";
    }
    const std::string w = st + text::nfc(e);
    return longE ? penultAcute(w) : text::nfc(w);
  }
  if (endsNfc(m, "ώς")) return std::string();
  const std::string mp = masculinePlural(m, f);
  if (mp.empty() || !endsNfc(mp, "ντες")) return std::string();
  const std::string S = cut(mp, "ες");   // κάμνοντ-
  if (f.empty() || !endsNfc(f, "α")) return std::string();
  const std::string fs = cut(f, "α");    // κάμνουσ-
  std::string w;
  if (gender == F) {
    if (!pl) w = case_ == Gen ? penultAcute(fs + text::nfc("ης")) : case_ == Dat ? penultAcute(fs + text::nfc("ῃ")) : f + text::nfc("ν");
    else w = case_ == Gen ? stripAccents(fs) + text::nfc("ῶν") : case_ == Dat ? penultAcute(fs + text::nfc("αις"))
                                                                         : penultAcute(fs + text::nfc("ας"));
  } else if (!pl) {
    if (case_ == Gen) w = S + text::nfc("ος");
    else if (case_ == Dat) w = S + text::nfc("ι");
    else w = gender == N ? n : S + text::nfc("α");
  } else {
    if (case_ == Gen) w = penultAcute(S + text::nfc("ων"));
    else if (case_ == Dat) w = fs + text::nfc("ι");
    else w = gender == N ? S + text::nfc("α") : S + text::nfc("ας");
  }
  return w.empty() ? w : text::nfc(w);
}

// C21: a verb whose table files its present as a perfect (κάθημαι: καθήμενος tagged "plural perfect participle",
// the feminine misaccented καθήμενη) while the lexicon has the participle as a lemma of its own with a full table
// (καθήμενος, -η, -ον): that lemma, found through the verb's masculine participle cell. kNoLemma when there is none.
uint32_t participleLemma(const lex::Lexicon& lx, uint32_t lemma) {
  thread_local std::vector<std::pair<uint32_t, std::string_view>> cells;
  cells.clear();
  lx.cells(lemma, cells);
  thread_local std::vector<lex::Analysis> an;
  for (const auto& c : cells) {
    const Features f = unpack(c.first);
    if (f.mood != ParticipleMood || (f.gender && f.gender != M)) continue;
    const std::string form = cleanCell(c.second);
    if (form.empty()) continue;
    an.clear();
    lx.lookup(text::greek_key(form), an);
    for (const lex::Analysis& a : an) {
      const lex::Lemma pl = lx.lemma(a.lemma);
      if (pl.id != lex::kNoLemma && pl.pos == Participle && (pl.flags & lex::HasTable) &&
          pl.key == text::greek_key(form))
        return pl.id;
    }
  }
  return lex::kNoLemma;
}

}  // namespace

bool participle(const lex::Lexicon& lx, uint32_t lemma, uint8_t tense, uint8_t voice, uint8_t case_, uint8_t number,
                uint8_t gender, std::string& out, GenInfo* info) {
  if (case_ != Nom && case_ != Voc) {
    // C25: genitive, dative, accusative derived from the nominatives (obliqueParticiple); a form the lexicon does not
    // list is a rule form (Check)
    if (case_ != Gen && case_ != Dat && case_ != Acc) return false;
    std::string m, f, n;
    if (!participle(lx, lemma, tense, voice, Nom, Sg, M, m, nullptr)) return false;
    participle(lx, lemma, tense, voice, Nom, Sg, F, f, nullptr);
    participle(lx, lemma, tense, voice, Nom, Sg, N, n, nullptr);
    const uint8_t g = gender == F || gender == N ? gender : (uint8_t)M;
    out = obliqueParticiple(m, f, n, case_, number == Pl ? (uint8_t)Pl : (uint8_t)Sg, g);
    if (out.empty()) return false;
    morph::Token t;
    morph::analyseGreek(lx, out, t);
    bool known = false;
    for (const lex::Analysis& a : t.analyses) known = known || (a.lemma == lemma && !t.accentInsensitive);
    Features pf;
    pf.pos = Verb;
    pf.mood = ParticipleMood;
    pf.tense = tense;
    pf.voice = voice;
    pf.case_ = case_;
    pf.number = number == Pl ? (uint8_t)Pl : (uint8_t)Sg;
    pf.gender = g;
    if (info) {
      GenInfo r;
      r.exact = known;
      r.fromRule = !known;
      r.packed = pack(pf);
      r.movableNu = case_ == Dat && number == Pl && endsNfc(out, "σι");   // διψῶσι(ν)
      *info = r;
    }
    return true;
  }
  // C21: the present participle from a participle lemma of its own when the verb's table has no present cell for it
  if (tense == Present) {
    Features pf;
    pf.pos = Verb;
    pf.mood = ParticipleMood;
    pf.tense = Present;
    pf.voice = voice;
    pf.gender = gender == F || gender == N ? gender : (uint8_t)M;
    pf.number = Sg;
    std::string probe;
    bool has = false;
    {
      GenInfo pg;
      has = generate(lx, lemma, pf, probe, &pg) && !probe.empty() && unpack(pg.packed).tense == Present;
    }
    const uint32_t pl = has ? lex::kNoLemma : participleLemma(lx, lemma);
    if (pl != lex::kNoLemma) {
      Features af;
      af.pos = Participle;
      af.case_ = case_;
      af.number = number == Pl ? (uint8_t)Pl : (uint8_t)Sg;
      af.gender = pf.gender;
      GenInfo ag;
      if (generate(lx, pl, af, out, &ag) && !out.empty()) {
        Features rf = unpack(ag.packed);
        if (rf.case_ == case_ && rf.number == af.number && (!rf.gender || rf.gender == af.gender)) {
          if (info) *info = ag;
          return true;
        }
      }
    }
  }
  const uint8_t g = gender == F || gender == N ? gender : (uint8_t)M;
  auto cell = [&](uint8_t gg, std::string& o, GenInfo* gi) {
    Features f;
    f.pos = Verb;
    f.mood = ParticipleMood;
    f.tense = tense;
    f.voice = voice;
    f.gender = gg;
    f.number = Sg;
    if (generate(lx, lemma, f, o, gi) && !o.empty()) return true;
    // C25: tables that tag the singular nominatives of the middle / passive participle as plural (ὀργίζω:
    // ὀργιζόμενος "plural masculine present participle passive"): such a cell with a singular ending serves
    f.number = Pl;
    if (!generate(lx, lemma, f, o, gi) || o.empty()) return false;
    const bool sgShape = (gg == M && (endsNfc(o, "μενος") || endsNfc(o, "μένος"))) ||
                         (gg == F && (endsNfc(o, "μένη") || endsNfc(o, "μενη"))) ||
                         (gg == N && (endsNfc(o, "μενον") || endsNfc(o, "μένον")));
    if (!sgShape) { o.clear(); return false; }
    if (gi) { Features pf = unpack(gi->packed); pf.number = Sg; gi->packed = pack(pf); }
    return true;
  };
  GenInfo gi;
  if (number != Pl) {
    if (!cell(g, out, &gi)) return false;
    Features pf = unpack(gi.packed);
    pf.case_ = Nom;
    pf.number = Sg;
    pf.gender = g;
    gi.packed = pack(pf);
    gi.movableNu = false;
    if (info) *info = gi;
    return true;
  }
  std::string m, f;
  if (!cell(M, m, &gi)) return false;
  GenInfo gf;
  cell(F, f, &gf);
  const std::string mp = masculinePlural(m, f);
  if (mp.empty()) return false;
  if (g == M) out = mp;
  else if (g == N) {
    if (endsNfc(mp, "ντες")) out = cut(mp, "ες") + "α";
    else if (endsNfc(mp, "ότες")) out = cut(mp, "ες") + "α";
    else if (endsNfc(mp, "οι")) out = cut(mp, "οι") + "α";
    else return false;
  } else {
    if (endsNfc(m, "μενος") || endsNfc(m, "μένος")) out = cut(mp, "οι") + "αι";
    else if (endsNfc(f, "α")) out = f + "ι";
    else return false;
  }
  out = text::nfc(out);
  // a form the lexicon does not list (form pages cover only some plurals) is a rule form: Check, never OK
  morph::Token t;
  morph::analyseGreek(lx, out, t);
  bool known = false;
  for (const lex::Analysis& a : t.analyses) known = known || (a.lemma == lemma && !t.accentInsensitive);
  Features pf = unpack(gi.packed);
  pf.case_ = Nom;
  pf.number = Pl;
  pf.gender = g;
  pf.extra = 0;
  GenInfo r;
  r.exact = known;
  r.attic = gi.attic;
  r.fromRule = !known;
  r.packed = pack(pf);
  if (info) *info = r;
  return true;
}

// ---- analysis -----------------------------------------------------------------------------------------------------
namespace {

void atticFilter(std::vector<lex::Analysis>& an) {
  std::vector<uint32_t> keep;   // lemmas with at least one analysis not flagged non-Attic
  for (const lex::Analysis& a : an)
    if (!(a.flags & lex::NonAttic) && std::find(keep.begin(), keep.end(), a.lemma) == keep.end()) keep.push_back(a.lemma);
  an.erase(std::remove_if(an.begin(), an.end(), [&](const lex::Analysis& a) {
             return (a.flags & lex::NonAttic) && std::find(keep.begin(), keep.end(), a.lemma) != keep.end();
           }),
           an.end());
}

// Drops the accent a following enclitic added on the ultima ("ἄνθρωπός" -> "ἄνθρωπος").
std::string dropEncliticAcute(std::string_view w) {
  const AccentInfo a = accentOf(w);
  if (a.accents < 2) return std::string(w);
  std::u32string u = text::toUtf32(text::nfd(w));
  for (size_t i = u.size(); i-- > 0;)
    if (u[i] == kAcuteM || u[i] == 0x0300) { u.erase(u.begin() + (long)i); break; }
  return text::nfc(text::toUtf8(u));
}

}  // namespace

std::string restoreElided(std::string_view word) {
  std::string w = text::nfc(word);
  static const char* const kApos[] = {"\xE2\x80\x99", "'", "\xE1\xBE\xBD", "\xCA\xBC"};
  bool elided = false;
  for (const char* ap : kApos) {
    const std::string_view a(ap);
    if (w.size() > a.size() && w.compare(w.size() - a.size(), a.size(), a) == 0) {
      w.resize(w.size() - a.size());
      elided = true;
      break;
    }
  }
  if (!elided) return std::string();
  static const char* const kFull[] = {"δέ", "ἀλλά", "τε", "οὐδέ", "μηδέ", "ἀπό", "ἐπί", "κατά", "μετά", "παρά",
                                      "διά", "ὑπό", "ἀντί", "γε", "ἆρα"};
  std::u32string b = text::toUtf32(text::greek_bare(w));
  for (const char* f : kFull) {
    std::u32string fb = text::toUtf32(text::greek_bare(f));
    fb.pop_back();   // without the elided vowel
    std::u32string bb = b;
    if (!bb.empty() && fb.size() == bb.size()) {
      const char32_t last = bb.back(), want = fb.back();
      const bool aspirated = (last == U'φ' && want == U'π') || (last == U'θ' && want == U'τ') || (last == U'χ' && want == U'κ');
      if (aspirated) bb.back() = want;
    }
    if (bb == fb) return f;
  }
  return std::string();
}

void analyse(const lex::Lexicon& lx, std::string_view word, morph::Token& out) {
  // exact readings of the canonical spellings first: as written, grave -> acute, without the acute an enclitic
  // added, an orthotone enclitic, without a movable nu, an elided word restored
  std::string cands[12];
  int nc = 0;
  const std::string w0 = text::nfc(word);
  const std::string restored = restoreElided(w0);
  if (!restored.empty()) cands[nc++] = restored;
  {   // orthotone ἔστι(ν) is the 3rd singular of εἰμί
    const std::string b0 = text::greek_bare(w0);
    if ((b0 == "εστι" || b0 == "εστιν") && accentOf(w0).position == 1) cands[nc++] = "ἐστί";
  }
  cands[nc++] = w0;
  // C18: προσε- written by generate() for a table's ἐπροσ- (an augment before the prefix): the table's spelling
  if (w0.compare(0, std::strlen("προσε"), "προσε") == 0 && w0.size() > std::strlen("προσε") + 2)
    cands[nc++] = "ἐπροσ" + w0.substr(std::strlen("προσε"));
  // C25: ἔνιψα written by generate() for a table's ἕνιψα (see smoothAugment): the table's spelling, tried after the
  // word itself; its augmented verb readings are added to the word's own (ἔνιψα is also a form of ἐνίπτω)
  const std::string roughForm = roughAugment(w0);
  if (!roughForm.empty()) cands[nc++] = roughForm;
  // C29: ἠνυπνίασε written by generate() for a table's ἐἐνυπνίασε (see doubleAugment): the table's spelling
  const std::string twoAug = doubleAugment(w0, false);
  if (!twoAug.empty()) cands[nc++] = twoAug;
  // C31: ἐπανῆλθον / ἐπανήλθομεν written by generate() for a table's unaugmented ἐπάνελθον / ἐπανέλθομεν (compounds of
  // ἔρχομαι): the table's spellings (the accent on the stem, or on the prefix's last vowel)
  {
    const std::string b = stripAccents(w0);
    const std::string elth = text::nfc("ηλθ");
    const size_t at = b.find(elth);
    if (at != std::string::npos && at > 0 && (w0.find(text::nfc("ῆλθ")) != std::string::npos ||
                                              w0.find(text::nfc("ήλθ")) != std::string::npos)) {
      const std::string pre = b.substr(0, at), ending = b.substr(at + elth.size());
      cands[nc++] = pre + text::nfc("έλθ") + ending;
      std::u32string u = text::toUtf32(text::nfd(pre));
      for (size_t i = u.size(); i-- > 0;)
        if (vowel(u[i])) {
          size_t j = i + 1;
          while (j < u.size() && u[j] >= 0x0300 && u[j] <= 0x036F) ++j;
          u.insert(u.begin() + (long)j, (char32_t)0x0301);
          break;
        }
      cands[nc++] = text::nfc(text::toUtf8(u)) + text::nfc("ελθ") + ending;
    }
  }
  const std::string w1 = dropEncliticAcute(ultimaToAcute(w0));
  if (w1 != w0) cands[nc++] = w1;
  if (accentOf(w1).accents == 0 && accentOf(w1).syllables >= 2) cands[nc++] = encliticAccented(w1);
  {
    const std::string b = text::greek_bare(w1);
    if (b.size() > 4 && (b.compare(b.size() - 4, 4, "ιν") == 0 || b.compare(b.size() - 4, 4, "εν") == 0)) {
      std::u32string u = text::toUtf32(text::nfd(w1));
      while (!u.empty() && u.back() >= 0x0300 && u.back() <= 0x036F) u.pop_back();
      if (!u.empty() && u.back() == U'ν') { u.pop_back(); cands[nc++] = text::nfc(text::toUtf8(u)); }
    }
  }
  for (int i = 0; i < nc; ++i) {
    morph::analyseGreek(lx, cands[i], out);
    if (!out.analyses.empty() && !out.accentInsensitive) {
      out.text = w0;
      if (!roughForm.empty() && cands[i] != roughForm) {
        morph::Token rt;
        morph::analyseGreek(lx, roughForm, rt);
        if (!rt.accentInsensitive)
          for (const lex::Analysis& a : rt.analyses) {
            const Features af = unpack(lx.feature(a.feat));
            const lex::Lemma rl = lx.lemma(a.lemma);
            if (rl.pos != Verb || smoothAugment(roughForm, rl.key) == roughForm) continue;   // only the augment case
            if (af.tense != Aorist && af.tense != Imperfect && af.tense != Pluperfect) continue;
            bool dup = false;
            for (const lex::Analysis& b : out.analyses) dup = dup || (b.lemma == a.lemma && b.feat == a.feat);
            if (!dup) out.analyses.push_back(a);
          }
      }
      atticFilter(out.analyses);
      return;
    }
  }
  morph::analyseGreek(lx, w0, out);
  if (!out.analyses.empty()) {
    atticFilter(out.analyses);
    return;
  }
  // paradigm fallback: guess the citation form (-ος / -ον) with every accent placement, keep lemmas without a table
  if (!out.unknown) return;
  const std::string b = text::greek_bare(word);
  static const char* const kEnd[] = {"ου", "ω", "ον", "ε", "οι", "ων", "οισ", "ουσ", "α", "η", "ησ", "ην", "αι",
                                     "αισ", "ασ", "αν", "οσ"};
  thread_local std::vector<lex::Analysis> an;
  thread_local std::vector<RuleCell> rc;
  std::vector<uint32_t> tried;
  const std::string bk = text::greek_key(b);
  for (const char* e : kEnd) {
    const std::string_view es(e);
    if (bk.size() <= es.size() + 2 || bk.compare(bk.size() - es.size(), es.size(), es) != 0) continue;
    const std::string stem = bk.substr(0, bk.size() - es.size());
    for (const char* cite : {"οσ", "ον"}) {
      const std::u32string cu = text::toUtf32(text::nfd(stem + cite));
      std::vector<size_t> vowels;
      for (size_t i = 0; i < cu.size(); ++i)
        if (vowel(cu[i])) vowels.push_back(i);
      for (size_t vi = vowels.size() > 3 ? vowels.size() - 3 : 0; vi < vowels.size(); ++vi)
        for (char32_t acc : {kAcuteM, kCircM})
          for (char32_t br : {(char32_t)0, (char32_t)0x0313, (char32_t)0x0314}) {
            if (br && !vowel(cu[0])) continue;
            std::u32string w;
            for (size_t i = 0; i < cu.size(); ++i) {
              w.push_back(cu[i]);
              if (i == 0 && br) w.push_back(br);
              if (i == vowels[vi]) w.push_back(acc);
            }
            an.clear();
            lx.lookup(text::greek_key(text::nfc(text::toUtf8(w))), an);
            for (const lex::Analysis& a : an) {
              if (std::find(tried.begin(), tried.end(), a.lemma) != tried.end()) continue;
              tried.push_back(a.lemma);
              const lex::Lemma l = lx.lemma(a.lemma);
              if (!paradigmApplies(l)) continue;
              paradigm(l, rc);
              for (const RuleCell& r : rc)
                if (text::greek_key(r.form) == out.key)
                  out.ruleAnalyses.push_back(morph::RuleAnalysis{a.lemma, r.packed, r.form});
            }
          }
    }
    if (tried.size() > 64) break;
  }
  if (!out.ruleAnalyses.empty()) {
    out.fromRule = true;
    out.unknown = false;
    out.nameGuess = false;
  }
}

// ---- names ------------------------------------------------------------------------------------------------------------
namespace {

std::string capFirst(const std::string& lowerNfc) {
  std::u32string u = text::toUtf32(text::nfd(lowerNfc));
  if (!u.empty() && u[0] >= 0x03B1 && u[0] <= 0x03C9 && u[0] != 0x03C2) u[0] -= 0x20;
  return text::nfc(text::toUtf8(u));
}

bool endsWith(std::string_view s, std::string_view t) { return s.size() >= t.size() && s.substr(s.size() - t.size()) == t; }

// Replaces the final letter group of `nom` (NFD, accents kept) and appends `add`.
std::string firstDecl(std::string_view nom, std::string_view gen, uint8_t case_, bool masc) {
  std::u32string u = text::toUtf32(text::nfd(nom));
  if (!u.empty() && (u.back() == U'ς' || u.back() == U'σ')) u.pop_back();   // masculine -ης / -ας
  // u ends with the final vowel and its marks
  size_t v = u.size();
  while (v > 0 && u[v - 1] >= 0x0300 && u[v - 1] <= 0x036F) --v;
  if (v == 0) return std::string();
  const std::string gb = text::greek_bare(gen);
  const bool genEta = endsWith(gb, "ησ") || endsWith(gb, "ης");
  bool oxy = false;
  for (size_t k = v; k < u.size(); ++k) oxy = oxy || u[k] == 0x0301;
  switch (case_) {
    case feat::Nom: return std::string(nom);
    case feat::Gen: return std::string(gen);
    case feat::Voc: return masc ? text::nfc(text::toUtf8(u)) : std::string(nom);
    case feat::Acc: { u.push_back(U'ν'); return text::nfc(text::toUtf8(u)); }
    case feat::Dat: {
      if (u[v - 1] == U'α' && genEta && !masc) u[v - 1] = U'η';
      for (size_t k = v; k < u.size(); ++k)
        if (u[k] == 0x0301 && oxy) u[k] = 0x0342;   // oxytone: circumflex in gen / dat
      u.push_back(0x0345);
      return text::nfc(text::toUtf8(u));
    }
    default: return std::string();
  }
}

}  // namespace

bool declineName(std::string_view nom0, std::string_view gen0, int declension, uint8_t gender, uint8_t case_,
                 std::string_view voc, std::string& out) {
  const std::string nom = text::nfc(nom0), gen = text::nfc(gen0);
  if (case_ == feat::Voc && !voc.empty()) { out = text::nfc(voc); return true; }
  if (declension == 0) { out = nom; return true; }
  if (case_ == feat::Nom) { out = nom; return true; }
  if (case_ == feat::Gen && !gen.empty()) { out = gen; return true; }
  if (declension == 1) {
    out = firstDecl(nom, gen, case_, gender == feat::M);
    return !out.empty();
  }
  if (declension == 2) {
    std::u32string u;
    int fromStart = 0;
    bool oxy = false;
    if (!headInfo(nom, u, fromStart, oxy)) return false;
    if (!(endsWithU(u, U"ος") || endsWithU(u, U"οσ"))) return false;
    const std::u32string stem = u.substr(0, u.size() - 2);
    for (const Ending& e : k2M)
      if (e.c == case_ && e.n == feat::Sg) {
        out = capFirst(display(accentForm(stem, e.e, fromStart, e.lng, oxy && (case_ == feat::Gen || case_ == feat::Dat),
                                          e.diphShort)));
        return true;
      }
    return false;
  }
  if (declension == 3) {
    const std::string gb = text::greek_bare(gen);
    std::u32string g = text::toUtf32(text::nfd(gen));
    if (endsWith(gb, "ουσ") || endsWith(gb, "ους")) {   // -ης, -ους (Σωκράτης)
      g.resize(g.size() - 3);
      if (case_ == feat::Dat) g += U"ει";
      else if (case_ == feat::Acc) g += U"η";
      else if (case_ == feat::Voc) { out = nom; return true; }
      out = text::nfc(text::toUtf8(g));
      return true;
    }
    if (!(endsWith(gb, "οσ") || endsWith(gb, "ος"))) return false;
    // accent on the ending (Διός)? then the dative carries it on -ι
    size_t o = g.size();
    while (o > 0 && g[o - 1] != U'ο') --o;
    bool endAcc = false;
    for (size_t k = o; k < g.size(); ++k) endAcc = endAcc || g[k] == 0x0301;
    g.resize(o - 1);
    if (case_ == feat::Voc) { out = nom; return true; }
    if (case_ == feat::Dat) { g.push_back(U'ι'); if (endAcc) g.push_back(0x0301); }
    else if (case_ == feat::Acc) {
      const std::string nb = text::greek_bare(nom);
      const AccentInfo na = accentOf(nom);
      if ((endsWith(nb, "ισ") || endsWith(nb, "ις")) && na.position != 0) {   // barytone -ις: Δικαιόπολιν
        std::u32string n = text::toUtf32(text::nfd(nom));
        n.back() = U'ν';
        out = text::nfc(text::toUtf8(n));
        return true;
      }
      g.push_back(U'α');
      if (endAcc) g.push_back(0x0301);
    } else return false;
    out = text::nfc(text::toUtf8(g));
    return true;
  }
  return false;
}

bool declineName(const NameEntry& e, uint8_t case_, std::string& out) {
  if (e.forms.size() >= 4) {
    int i = -1;
    switch (case_) {
      case feat::Nom: i = 0; break;
      case feat::Gen: i = 1; break;
      case feat::Dat: i = 2; break;
      case feat::Acc: i = 3; break;
      case feat::Voc: i = e.forms.size() > 4 ? 4 : 0; break;
      default: break;
    }
    if (i < 0 || e.forms[(size_t)i].empty()) return false;
    out = e.forms[(size_t)i];
    return true;
  }
  if (e.number != feat::Sg) return false;
  return declineName(e.nom, e.gen, e.declension, e.gender, case_, e.voc, out);
}

uint8_t mapTense(uint8_t englishTense, bool progressive, bool state, bool perfectResult) {
  switch (englishTense) {
    case Present: return Present;
    case Future: return Future;
    case Perfect: return perfectResult ? (uint8_t)Perfect : (uint8_t)Aorist;
    case Pluperfect: return perfectResult ? (uint8_t)Pluperfect : (uint8_t)Aorist;
    case Imperfect: return Imperfect;
    default: break;
  }
  // past simple and friends
  return (progressive || state) ? (uint8_t)Imperfect : (uint8_t)Aorist;
}

}  // namespace vp::grc
