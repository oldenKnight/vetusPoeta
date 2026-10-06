// engine/rules Greek side (C9): morphology helpers, accents and sandhi, curated Greek tables, realisation and the
// Greek checker. Text helpers, accents, closed tables, names and the paradigm fallback (mini lexicon built with the
// reference encoder) always run; the lexicon tables run on the real library (env VP_GREEK_VPL, default
// data/work/greek.vpl) and skip with a message when it is absent.
#include <doctest.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "lex_fixture_writer.h"
#include "vp/check_grc.h"
#include "vp/curated.h"
#include "vp/lex.h"
#include "vp/morph_grc.h"
#include "vp/realise_grc.h"
#include "vp/text.h"

#if defined(__SANITIZE_ADDRESS__)
#define VP_GRC_TEST_SANITIZED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define VP_GRC_TEST_SANITIZED 1
#endif
#endif

namespace stdfs = std::filesystem;
using namespace vp;
using namespace vp::feat;
using vp::grc::GrcAdj;
using vp::grc::GrcClause;
using vp::grc::GrcNP;
using vp::grc::GrcOblique;
using vp::grc::GrcOptions;
using vp::grc::GrcSentence;
using vp::grc::GrcSub;
using vp::grc::kNone;
using vp::realise::AdvPos;
using vp::realise::ClauseType;
using vp::realise::Polarity;
using vp::realise::Role;
using vp::realise::SubRel;
using vp::realise::YnBias;

namespace {

stdfs::path repoRoot() { return stdfs::path(VP_FIXTURES_DIR).parent_path().parent_path(); }
stdfs::path fixture(const char* name) { return stdfs::path(VP_FIXTURES_DIR) / "rules_grc" / name; }
stdfs::path tmpDir() {
  stdfs::path d = stdfs::path(VP_TEST_TMP) / "rules_grc";
  std::error_code ec;
  stdfs::create_directories(d, ec);
  return d;
}

const curated::CuratedData& cur() {
  static Result<curated::CuratedData> r = curated::CuratedData::load(repoRoot() / "data" / "curated");
  if (!r.ok()) {
    static curated::CuratedData empty;
    return empty;
  }
  return r.value();
}
const grc::GreekData& gdata() {
  static Result<grc::GreekData> r = grc::GreekData::load(repoRoot() / "data" / "curated");
  if (!r.ok()) {
    static grc::GreekData empty;
    return empty;
  }
  return r.value();
}

struct Held { lex::Lexicon lx; bool ok = false; std::string why; };
Held openHeld(const stdfs::path& p) {
  Held h;
  auto r = lex::Lexicon::open(p);
  if (r.ok()) { h.lx = std::move(r.value()); h.ok = true; }
  else h.why = r.error().message;
  return h;
}
const Held& realGrc() {
  static Held h = [] {
    const char* env = std::getenv("VP_GREEK_VPL");
    return openHeld(env && *env ? stdfs::path(env) : repoRoot() / "data" / "work" / "greek.vpl");
  }();
  return h;
}
#define NEED_GRC()                                                                                   \
  if (!realGrc().ok) {                                                                               \
    MESSAGE("real greek.vpl not available (" << realGrc().why << "); set VP_GREEK_VPL. Skipped.");  \
    return;                                                                                          \
  }

std::vector<std::vector<std::string>> readTsv(const stdfs::path& p) {
  std::vector<std::vector<std::string>> rows;
  std::ifstream in(p);
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::vector<std::string> cols;
    size_t s = 0;
    for (;;) {
      size_t t = line.find('\t', s);
      cols.push_back(line.substr(s, t == std::string::npos ? std::string::npos : t - s));
      if (t == std::string::npos) break;
      s = t + 1;
    }
    rows.push_back(cols);
  }
  return rows;
}

uint8_t posOf(const std::string& s) {
  if (s == "noun") return Noun;
  if (s == "verb") return Verb;
  if (s == "adj") return Adj;
  if (s == "adv") return Adv;
  if (s == "pron") return Pron;
  if (s == "num") return Num;
  if (s == "det") return feat::Det;
  if (s == "article") return Article;
  if (s == "name") return Name;
  return 0;
}

Features parseFeats(const std::string& s) {
  Features f;
  std::istringstream is(s);
  std::string w;
  while (is >> w) {
    if (w == "nom") f.case_ = Nom; else if (w == "gen") f.case_ = Gen; else if (w == "dat") f.case_ = Dat;
    else if (w == "acc") f.case_ = Acc; else if (w == "voc") f.case_ = Voc;
    else if (w == "sg") f.number = Sg; else if (w == "pl") f.number = Pl;
    else if (w == "m") f.gender = M; else if (w == "f") f.gender = F; else if (w == "n") f.gender = N;
    else if (w == "1") f.person = P1; else if (w == "2") f.person = P2; else if (w == "3") f.person = P3;
    else if (w == "pres") f.tense = Present; else if (w == "impf") f.tense = Imperfect;
    else if (w == "fut") f.tense = Future; else if (w == "perf") f.tense = Perfect;
    else if (w == "plup") f.tense = Pluperfect; else if (w == "aor") f.tense = Aorist;
    else if (w == "ind") f.mood = Indicative; else if (w == "subj") f.mood = Subjunctive;
    else if (w == "imp") f.mood = Imperative; else if (w == "inf") f.mood = Infinitive;
    else if (w == "opt") f.mood = Optative;
    else if (w == "act") f.voice = Active; else if (w == "mid") f.voice = Middle; else if (w == "pass") f.voice = Passive;
  }
  return f;
}
bool featsMatch(const Features& have, const Features& want) {
  auto eq = [](uint8_t h, uint8_t w) { return w == 0 || h == w; };
  return eq(have.case_, want.case_) && eq(have.number, want.number) && eq(have.person, want.person) &&
         eq(have.tense, want.tense) && eq(have.mood, want.mood) &&
         (want.gender == 0 || have.gender == 0 || morph::genderAdmits(have.gender, want.gender));
}

long rssAnonKb() {
  std::ifstream in("/proc/self/status");
  std::string k;
  long v = -1;
  while (in >> k) {
    if (k == "RssAnon:") { in >> v; return v; }
    std::string rest;
    std::getline(in, rest);
  }
  return -1;
}

// ---- clause-building DSL ----------------------------------------------------------------------------------------
const lex::Lexicon* g_lx = nullptr;
uint32_t L(const char* head, uint8_t pos = 0) {
  uint32_t id = grc::findLemma(*g_lx, head, pos);
  if (id == kNone && pos) id = grc::findLemma(*g_lx, head);
  CHECK_MESSAGE(id != kNone, "lemma not found: " << head);
  return id;
}
GrcNP n(const char* head, uint8_t number = Sg) { GrcNP x; x.head = L(head, Noun); x.number = number; return x; }
GrcNP d(const char* head, uint8_t number = Sg) { GrcNP x = n(head, number); x.definite = true; return x; }
GrcNP sb(const char* head, uint8_t number, uint8_t gender, uint8_t pos = 0) {
  GrcNP x; x.head = L(head, pos); x.number = number; x.gender = gender; return x;
}
GrcNP pr(uint8_t person, uint8_t number = Sg, uint8_t gender = 0, bool emphasis = false) {
  GrcNP x; x.isPronoun = true; x.pron.person = person; x.pron.number = number; x.pron.gender = gender;
  x.number = number; x.gender = gender; x.emphasis = emphasis; return x;
}
GrcNP nm(const char* name, bool definite = true) { GrcNP x; x.isName = true; x.name = name; x.definite = definite; return x; }
GrcAdj A(const char* head, std::initializer_list<const char*> advs = {}) {
  GrcAdj a; a.lemma = L(head, Adj);
  for (const char* v : advs) a.adverbs.push_back(L(v, Adv));
  return a;
}
GrcNP with(GrcNP x, GrcAdj a) { x.adjectives.push_back(a); return x; }
GrcNP poss(GrcNP x, uint8_t person, uint8_t number = Sg, uint8_t gender = 0) {
  x.possPerson = person; x.possNumber = number; x.possGender = gender; return x;
}
GrcNP num(GrcNP x, const char* numeral) { x.numeral = L(numeral); return x; }
GrcNP gen(GrcNP x, GrcNP g) { x.genitive.push_back(g); return x; }
GrcNP et(GrcNP x, GrcNP y) { x.coord.push_back(y); return x; }
GrcNP interrog(GrcNP x, const char* w) { x.interrogative = L(w); return x; }
GrcNP dem(GrcNP x) { x.dem = grc::Demonstrative::Houtos; x.definite = true; return x; }
GrcNP quant(GrcNP x, const char* q) { x.quantifier = L(q); return x; }

struct CB {
  GrcClause c;
  explicit CB(ClauseType t = ClauseType::Decl) { c.type = t; }
  CB& v(const char* verb, uint8_t tense = Present) { c.pred.lemma = L(verb, Verb); c.pred.tense = tense; return *this; }
  CB& mood(uint8_t m) { c.pred.mood = m; return *this; }
  CB& voice(uint8_t vo) { c.pred.voice = vo; return *this; }
  CB& pers(uint8_t p, uint8_t number = Sg) { c.pred.person = p; c.pred.number = number; return *this; }
  CB& nb(uint8_t number) { c.pred.number = number; return *this; }
  CB& modal(const char* m, uint8_t infTense = Present) { c.pred.modal = L(m, Verb); c.pred.infTense = infTense; return *this; }
  CB& neg() { c.polarity = Polarity::Neg; return *this; }
  CB& s(GrcNP x) { c.hasSubject = true; c.subject = x; return *this; }
  CB& o(GrcNP x) { c.hasObject = true; c.object = x; return *this; }
  CB& io(GrcNP x) { c.hasIndirect = true; c.indirect = x; return *this; }
  CB& obl(const char* prep, GrcNP x, bool front = false, uint8_t cs = 0) {
    GrcOblique ob;
    ob.prep = prep ? L(prep, Prep) : kNone;
    ob.case_ = cs;
    ob.np = x;
    ob.front = front;
    c.obliques.push_back(ob);
    return *this;
  }
  CB& adv(const char* a, AdvPos p = AdvPos::Auto) { c.adverbs.push_back({L(a, Adv), p}); return *this; }
  CB& conn(const char* k) { c.connectors.push_back(L(k)); return *this; }
  CB& polite(const char* k) { c.politeness.push_back(L(k)); return *this; }
  CB& voc(GrcNP x) { c.vocatives.push_back(x); return *this; }
  CB& intj(const char* k) { c.interjections.push_back(L(k)); return *this; }
  CB& pred(GrcNP x) { c.predicative.push_back(x); return *this; }
  CB& padj(GrcAdj a) { c.predAdj.push_back(a); return *this; }
  CB& pg(uint8_t g, uint8_t number = 0) { c.predGender = g; c.predNumber = number; return *this; }
  CB& wh(const char* w, Role r = Role::None, uint8_t g = 0) { c.wh.lemma = L(w); c.wh.role = r; c.wh.gender = g; return *this; }
  CB& exist() { c.existential = true; return *this; }
  CB& punct(const char* p) { c.punct = p; return *this; }
  CB& bias(YnBias b) { c.bias = b; return *this; }
  CB& ara() { c.ara = true; return *this; }
  CB& hos() { c.exclHos = true; return *this; }
  CB& sub(SubRel rel, const CB& sc, bool before = false, const char* conj = nullptr) {
    GrcSub su;
    su.rel = rel;
    su.before = before;
    su.conj = conj ? L(conj, Conj) : kNone;
    su.clause.push_back(sc.c);
    c.subs.push_back(su);
    return *this;
  }
  CB& rel(Role r) { c.relRole = r; return *this; }
};
GrcNP withRel(GrcNP x, const CB& rc) { x.relative.push_back(rc.c); return x; }

}  // namespace

// ================================================================================================================
TEST_CASE("rules-grc: text helpers, accents and closed tables") {
  CHECK(grc::stripLength(text::nfc("ᾰ̓́νθρωπος")) == text::nfc("ἄνθρωπος"));
  CHECK(grc::display("λῡ́ουσῐ") == text::nfc("λύουσι"));
  CHECK(grc::finalSigma("λογοσ") == "λογος");
  CHECK(grc::finalSigma("σοφοσ, καλοσ") == "σοφος, καλος");
  CHECK(grc::finalSigma("σῶμα") == "σῶμα");
  bool nu = false;
  CHECK(grc::cleanCell("ἐστῐ́(ν)", &nu) == text::nfc("ἐστί"));
  CHECK(nu);
  CHECK(grc::cleanCell("τῆς ᾰ̓νθρώπου") == text::nfc("ἀνθρώπου"));
  CHECK(grc::cleanCell("πεφᾰσμένοι εἰσῐ́(ν)").empty());
  CHECK(grc::cleanCell("ἦσ") == "ἦς");
  // accent analysis
  grc::AccentInfo a = grc::accentOf("ἄνθρωπος");
  CHECK(a.syllables == 3);
  CHECK(a.position == 2);
  CHECK(a.type == grc::Accent::Acute);
  a = grc::accentOf("οἶκος");
  CHECK(a.position == 1);
  CHECK(a.type == grc::Accent::Circumflex);
  a = grc::accentOf("ποιεῖ");
  CHECK(a.syllables == 2);
  CHECK(a.position == 0);
  CHECK(grc::accentOf("οὐ").accents == 0);
  CHECK(grc::addUltimaAcute("ἄνθρωπος") == text::nfc("ἄνθρωπός"));
  CHECK(grc::addUltimaAcute("εἰ") == text::nfc("εἴ"));
  CHECK(grc::addUltimaAcute("οὐ") == text::nfc("οὔ"));
  CHECK(grc::ultimaToGrave("καλός") == text::nfc("καλὸς"));
  CHECK(grc::ultimaToAcute("τὸν") == text::nfc("τόν"));
  CHECK(grc::stripAccents("τινός") == text::nfc("τινος"));
  CHECK(grc::encliticAccented("τινων") == text::nfc("τινῶν"));
  CHECK(grc::isEncliticForm("μου"));
  CHECK(grc::isEncliticForm("ἐστί"));
  CHECK(grc::isEncliticForm("τινός"));
  CHECK_FALSE(grc::isEncliticForm("τίς"));
  CHECK_FALSE(grc::isEncliticForm("τίνος"));
  CHECK_FALSE(grc::isEncliticForm("ποῦ"));
  CHECK_FALSE(grc::isEncliticForm("εἶ"));
  CHECK(grc::isProclitic("ἐν"));
  CHECK(grc::isProclitic("οὐκ"));
  CHECK(grc::isPostpositive("γάρ"));
  CHECK(grc::startsWithVowel("ἥκω"));
  CHECK(grc::startsWithRough("ἥκω"));
  CHECK(grc::startsWithRough("ὑμεῖς"));
  CHECK(grc::startsWithRough("ῥόδον"));
  CHECK_FALSE(grc::startsWithRough("εἰμί"));
  CHECK(grc::startsWithRough("οἷς"));
  CHECK(grc::restoreElided("δ’") == "δέ");
  CHECK(grc::restoreElided("ἀφ’") == "ἀπό");
  CHECK(grc::restoreElided("καθ’") == "κατά");
  CHECK(grc::restoreElided("λόγος").empty());
  // elision (optional)
  grc::SandhiOptions el;
  el.elision = true;
  CHECK(grc::accentuate("ἀλλά ἐγώ οὐ οἶδα.", el) == text::nfc("ἀλλ’ ἐγὼ οὐκ οἶδα."));
  CHECK(grc::accentuate("ἀπό ἡμῶν", el) == text::nfc("ἀφ’ ἡμῶν"));
  CHECK(grc::accentuate("ἀλλά ἐγώ οὐ οἶδα.") == text::nfc("ἀλλὰ ἐγὼ οὐκ οἶδα."));   // off by default
  // closed tables
  std::string f;
  CHECK(grc::article(Dat, Pl, F, f));
  CHECK(f == "ταῖς");
  CHECK(grc::closedForm("ἐγώ", Gen, Sg, 0, true, f));
  CHECK(f == "μου");
  CHECK(grc::closedForm("ἐγώ", Gen, Sg, 0, false, f));
  CHECK(f == "ἐμοῦ");
  CHECK(grc::closedForm("ὅσ", Acc, Sg, F, false, f));
  CHECK(f == "ἥν");
  CHECK(grc::closedForm("οὐδείσ", Acc, Sg, F, false, f));
  CHECK(f == "οὐδεμίαν");
  std::vector<grc::ClosedReading> cr;
  grc::closedReadings("τὴν", cr);
  REQUIRE(cr.size() == 1);
  CHECK(cr[0].case_ == Acc);
  cr.clear();
  grc::closedReadings("μου", cr);
  CHECK(!cr.empty());
  CHECK(grc::mapTense(Aorist, false, false, false) == Aorist);
  CHECK(grc::mapTense(Aorist, true, false, false) == Imperfect);
  CHECK(grc::mapTense(Perfect, false, false, true) == Perfect);
  CHECK(grc::mapTense(Perfect, false, false, false) == Aorist);
}

TEST_CASE("rules-grc: 30+ enclitic and sandhi cases (tests/fixtures/rules_grc/enclitic_grc.tsv)") {
  const auto rows = readTsv(fixture("enclitic_grc.tsv"));
  REQUIRE(rows.size() >= 30);
  size_t ok = 0;
  for (const auto& r : rows) {
    REQUIRE(r.size() >= 2);
    const std::string got = grc::accentuate(text::nfc(r[0]));
    CHECK_MESSAGE(got == text::nfc(r[1]), "'" << r[0] << "' -> '" << got << "' expected '" << r[1] << "'");
    ok += got == text::nfc(r[1]);
  }
  MESSAGE("enclitic cases: " << ok << " / " << rows.size());
}

TEST_CASE("rules-grc: Greek curated tables load (valency, prepositions, names, particles, phrasebook, order)") {
  auto r = grc::GreekData::load(repoRoot() / "data" / "curated");
  REQUIRE_MESSAGE(r.ok(), r.error().message);
  const grc::GreekData& g = r.value();
  for (const auto& w : g.warnings()) MESSAGE("greek curated warning: " << w.file << ":" << w.line << " " << w.message);
  CHECK(g.warnings().empty());
  CHECK(g.orderRules().size() >= 40);
  CHECK(g.slotTemplate("order.decl") == std::vector<std::string>{"VOC", "CONN", "S", "IO", "O", "OBL", "NEG", "ADV", "V"});
  CHECK(g.slotTemplate("order.exist") == std::vector<std::string>{"CONN", "OBL", "V", "S"});
  const auto second = g.conditionSet("order.conn.second");
  CHECK(std::find(second.begin(), second.end(), text::greek_key("γάρ")) != second.end());
  CHECK(!g.orderingList("order.adv", "time adverbs").empty());
  CHECK(g.valency(text::greek_key("ἕπομαι")) != nullptr);
  CHECK(g.valency(text::greek_key("ἄρχω"))->frames[0].kind == grc::FrameKind::Gen);
  const grc::Valency* pe = g.valency(text::greek_key("πείθω"));
  REQUIRE(pe);
  CHECK(pe->frames[1].middle);
  CHECK(g.prepCases(text::greek_key("παρά")) == ((1u << Gen) | (1u << Dat) | (1u << Acc)));
  CHECK(g.prepCases(text::greek_key("μετά")) == ((1u << Gen) | (1u << Acc)));
  CHECK(g.prepCases(text::greek_key("ἐν")) == (1u << Dat));
  CHECK(g.prepCases(text::greek_key("εἰς")) == (1u << Acc));
  CHECK(g.prepCases(text::greek_key("ἄνευ")) == (1u << Gen));
  CHECK(g.prepCases(text::greek_key("σύν")) == (1u << Dat));
  CHECK(g.prepDefaultCase(text::greek_key("ὑπό")) == Gen);
  REQUIRE(g.nameByEnglish("alice"));
  CHECK(g.nameByEnglish("Alice")->nom == "Ἀλίκη");
  CHECK(g.particle(text::greek_key("δέ"))->second);
  CHECK_FALSE(g.particle(text::greek_key("ἀλλά"))->second);
  CHECK(g.phrasebook().size() >= 40);
  CHECK(!cur().tierGreek(text::greek_key("βαίνω")) == false);
  CHECK(cur().emojiGreek(text::greek_key("ἥλιος")) != nullptr);
  // a missing file is an error with a hint
  auto bad = grc::GreekData::load(tmpDir() / "nowhere");
  CHECK_FALSE(bad.ok());
  CHECK(!bad.error().hint.empty());
}

TEST_CASE("rules-grc: names decline (1st, 2nd, 3rd declension, vocative override)") {
  std::string f;
  auto dn = [&](const char* nom, const char* gen, int decl, uint8_t g, uint8_t c, const char* voc = "") {
    f.clear();
    grc::declineName(nom, gen, decl, g, c, voc, f);
    return f;
  };
  CHECK(dn("Ἀλίκη", "Ἀλίκης", 1, F, Dat) == text::nfc("Ἀλίκῃ"));
  CHECK(dn("Ἀλίκη", "Ἀλίκης", 1, F, Acc) == "Ἀλίκην");
  CHECK(dn("Μαρία", "Μαρίας", 1, F, Dat) == text::nfc("Μαρίᾳ"));
  CHECK(dn("Ἄννα", "Ἄννης", 1, F, Dat) == text::nfc("Ἄννῃ"));
  CHECK(dn("Ἄννα", "Ἄννης", 1, F, Acc) == "Ἄνναν");
  CHECK(dn("Ζωή", "Ζωῆς", 1, F, Dat) == text::nfc("Ζωῇ"));
  CHECK(dn("Ξανθίας", "Ξανθίου", 1, M, Voc) == "Ξανθία");
  CHECK(dn("Ξανθίας", "Ξανθίου", 1, M, Acc) == "Ξανθίαν");
  CHECK(dn("Ἰωάννης", "Ἰωάννου", 1, M, Dat) == text::nfc("Ἰωάννῃ"));
  CHECK(dn("Ἑρμῆς", "Ἑρμοῦ", 1, M, Acc) == "Ἑρμῆν");
  CHECK(dn("Φίλιππος", "Φιλίππου", 2, M, Dat) == text::nfc("Φιλίππῳ"));
  CHECK(dn("Φίλιππος", "Φιλίππου", 2, M, Acc) == "Φίλιππον");
  CHECK(dn("Μᾶρκος", "Μάρκου", 2, M, Dat) == text::nfc("Μάρκῳ"));
  CHECK(dn("Παῦλος", "Παύλου", 2, M, Voc) == "Παῦλε");
  CHECK(dn("Πλάτων", "Πλάτωνος", 3, M, Dat) == "Πλάτωνι");
  CHECK(dn("Πλάτων", "Πλάτωνος", 3, M, Acc) == "Πλάτωνα");
  CHECK(dn("Σωκράτης", "Σωκράτους", 3, M, Dat) == "Σωκράτει");
  CHECK(dn("Σωκράτης", "Σωκράτους", 3, M, Acc) == "Σωκράτη");
  CHECK(dn("Σωκράτης", "Σωκράτους", 3, M, Voc, "Σώκρατες") == "Σώκρατες");
  CHECK(dn("Δικαιόπολις", "Δικαιοπόλιδος", 3, M, Acc) == "Δικαιόπολιν");
  CHECK(dn("Δικαιόπολις", "Δικαιοπόλιδος", 3, M, Dat) == "Δικαιοπόλιδι");
  CHECK(dn("Δανιήλ", "Δανιήλ", 0, M, Dat) == "Δανιήλ");
}

namespace {
// Lemmas without tables (rule paradigm): 2nd-declension nouns and 1st/2nd-class adjectives; εἰμί with a table.
lexfix::LexiconIn miniGreek() {
  lexfix::LexiconIn in;
  in.lang = "grc";
  in.notice = "Greek rules test mini lexicon (own data)";
  auto lemma = [&](const char* head, uint8_t pos, uint8_t cls, uint8_t gender, const char* principal, uint16_t flags = 0) {
    lexfix::LemmaIn l;
    l.head = text::nfc(head);
    l.key = text::greek_key(head);
    l.pos = pos;
    l.cls = cls;
    l.gender = gender;
    l.tier = 1;
    l.flags = flags;
    l.principal = principal;
    l.principalCount = 1;
    in.lemmas.push_back(l);
    const uint32_t id = (uint32_t)(in.lemmas.size() - 1);
    Features f; f.pos = pos; f.case_ = pos == Verb ? 0 : Nom; f.number = Sg; f.gender = pos == Adj ? M : 0;
    in.analyses.push_back({in.lemmas[id].key, id, pack(f), 0, in.lemmas[id].head});
    return id;
  };
  lemma("ἄνθρωπος", Noun, 2, M, "ἄνθρωπος m (genitive ἀνθρώπου); second declension");
  lemma("λόγος", Noun, 2, M, "λόγος m (genitive λόγου); second declension");
  lemma("θεός", Noun, 2, M, "θεός m (genitive θεοῦ); second declension");
  lemma("δῶρον", Noun, 2, N, "δῶρον n (genitive δώρου); second declension");
  lemma("ἀγαθός", Adj, 1, M, "ἀγαθός (feminine ἀγαθή, neuter ἀγαθόν)");
  lemma("δίκαιος", Adj, 1, M, "δίκαιος (feminine δικαία, neuter δίκαιον)");
  lemma("ἄδικος", Adj, 1, M, "ἄδικος (feminine ἄδικος, neuter ἄδικον)");
  return in;
}
const Held& miniGrc() {
  static Held h = [] {
    const auto bytes = lexfix::build(miniGreek());
    const stdfs::path p = tmpDir() / "mini_grc.vpl";
    std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), (std::streamsize)bytes.size());
    return openHeld(p);
  }();
  return h;
}
}  // namespace

TEST_CASE("rules-grc: mini lexicon - rule paradigm for lemmas without a table (fromRule)") {
  REQUIRE(miniGrc().ok);
  const lex::Lexicon& lx = miniGrc().lx;
  struct Row { const char* head; uint8_t pos; Features f; const char* want; };
  const Row rows[] = {
      {"ἄνθρωπος", Noun, grc::nounForm(Gen, Sg), "ἀνθρώπου"}, {"ἄνθρωπος", Noun, grc::nounForm(Nom, Pl), "ἄνθρωποι"},
      {"ἄνθρωπος", Noun, grc::nounForm(Acc, Pl), "ἀνθρώπους"}, {"ἄνθρωπος", Noun, grc::nounForm(Voc, Sg), "ἄνθρωπε"},
      {"λόγος", Noun, grc::nounForm(Dat, Sg), "λόγῳ"},          {"θεός", Noun, grc::nounForm(Gen, Sg), "θεοῦ"},
      {"θεός", Noun, grc::nounForm(Acc, Sg), "θεόν"},           {"θεός", Noun, grc::nounForm(Dat, Pl), "θεοῖς"},
      {"δῶρον", Noun, grc::nounForm(Gen, Sg), "δώρου"},         {"δῶρον", Noun, grc::nounForm(Nom, Pl), "δῶρα"},
      {"ἀγαθός", Adj, grc::adjForm(Nom, Sg, F), "ἀγαθή"},       {"ἀγαθός", Adj, grc::adjForm(Gen, Sg, F), "ἀγαθῆς"},
      {"ἀγαθός", Adj, grc::adjForm(Acc, Pl, M), "ἀγαθούς"},     {"ἀγαθός", Adj, grc::adjForm(Nom, Pl, N), "ἀγαθά"},
      {"δίκαιος", Adj, grc::adjForm(Nom, Sg, F), "δικαία"},     {"δίκαιος", Adj, grc::adjForm(Gen, Pl, M), "δικαίων"},
      {"ἄδικος", Adj, grc::adjForm(Nom, Sg, F), "ἄδικος"},      {"ἄδικος", Adj, grc::adjForm(Gen, Sg, F), "ἀδίκου"},
  };
  for (const Row& r : rows) {
    const uint32_t id = grc::findLemma(lx, r.head, r.pos);
    REQUIRE(id != kNone);
    std::string out;
    grc::GenInfo gi;
    REQUIRE(grc::generate(lx, id, r.f, out, &gi));
    CHECK_MESSAGE(out == text::nfc(r.want), r.head << " -> " << out << " expected " << r.want);
    CHECK(gi.fromRule);
  }
  // analysis of a rule form
  morph::Token t;
  grc::analyse(lx, "ἀνθρώπους", t);
  CHECK(t.fromRule);
  REQUIRE(!t.ruleAnalyses.empty());
  CHECK(unpack(t.ruleAnalyses[0].packed).case_ == Acc);
}

TEST_CASE("rules-grc: real lexicon - analyse forms (tests/fixtures/rules_grc/analyse_grc.tsv)") {
  NEED_GRC();
  const lex::Lexicon& lx = realGrc().lx;
  const auto rows = readTsv(fixture("analyse_grc.tsv"));
  REQUIRE(rows.size() >= 60);
  size_t ok = 0;
  morph::Token t;
  std::vector<grc::ClosedReading> cr;
  for (const auto& r : rows) {
    REQUIRE(r.size() >= 4);
    grc::analyse(lx, text::nfc(r[0]), t);
    const std::string wantKey = text::greek_key(r[1]);
    const Features want = parseFeats(r[2]);
    bool hit = false;
    for (const lex::Analysis& a : t.analyses) {
      const lex::Lemma l = lx.lemma(a.lemma);
      if (l.key == wantKey && featsMatch(unpack(lx.feature(a.feat)), want)) hit = true;
    }
    cr.clear();
    grc::closedReadings(text::nfc(r[0]), cr);
    for (const grc::ClosedReading& c : cr)
      if (wantKey == c.lemmaKey && (want.case_ == 0 || want.case_ == c.case_)) hit = true;
    const bool modeOk = r[3] == "any" || (r[3] == "bare") == t.accentInsensitive;
    CHECK_MESSAGE(hit, r[0] << ": no reading of " << r[1] << " " << r[2]);
    CHECK_MESSAGE(modeOk, r[0] << ": accent-insensitive = " << t.accentInsensitive << ", expected " << r[3]);
    ok += hit && modeOk;
  }
  MESSAGE("analyses: " << ok << " / " << rows.size());
}

TEST_CASE("rules-grc: real lexicon - generate cells (tests/fixtures/rules_grc/generate_grc.tsv)") {
  NEED_GRC();
  const lex::Lexicon& lx = realGrc().lx;
  const auto rows = readTsv(fixture("generate_grc.tsv"));
  REQUIRE(rows.size() >= 100);
  size_t ok = 0;
  for (const auto& r : rows) {
    REQUIRE(r.size() >= 4);
    const uint8_t pos = posOf(r[1]);
    uint32_t id = grc::findLemma(lx, r[0], pos);
    if (id == kNone) id = grc::findLemma(lx, r[0]);
    REQUIRE_MESSAGE(id != kNone, r[0]);
    Features f = parseFeats(r[2]);
    f.pos = pos == Article || pos == feat::Det || pos == Pron ? pos : pos == Adj ? (uint8_t)Adj : pos;
    std::string out;
    const bool got = grc::generate(lx, id, f, out);
    CHECK_MESSAGE(got, r[0] << " " << r[2] << ": no form");
    CHECK_MESSAGE(out == text::nfc(r[3]), r[0] << " " << r[2] << " -> " << out << " expected " << r[3]);
    ok += out == text::nfc(r[3]);
  }
  MESSAGE("generated cells: " << ok << " / " << rows.size());
}

namespace {
struct Row { std::vector<GrcClause> c; std::string expect; };

// The gold lines: hand-built clause structures for the first 40 sentences of own_dialogue.en.txt.
std::vector<Row> goldRows() {
  std::vector<Row> t;
  auto add = [&](std::initializer_list<CB> bs) {
    Row r;
    for (const CB& b : bs) r.c.push_back(b.c);
    t.push_back(r);
  };
  add({CB(ClauseType::Wh).wh("ποῖ").v("βαίνω").pers(2)});
  add({CB().v("ἥκω").pers(1).adv("ὀψέ").punct("!"), CB().v("ἥκω").pers(1).adv("ὀψέ").punct("!")});
  add({CB(ClauseType::Wh).intj("οἴμοι").intj("οἴμοι").wh("τίς", Role::Object, N).v("ποιέω", Aorist).mood(Subjunctive).pers(1)});
  add({CB(ClauseType::Imp).v("ἐπανέρχομαι", Aorist).adv("δεῦρο").punct("!")});
  add({CB(ClauseType::Imp).v("μένω").o(pr(1)).polite("ἀντιβολέω")});
  add({CB(ClauseType::Wh).wh("τίς", Role::Predicate).v("εἰμί").pers(2)});
  add({CB().v("οἶδα").pers(1).neg()});
  add({CB(ClauseType::Yn).ara().v("ὁράω", Aorist).pers(2).o(poss(d("γαλῆ"), 1))});
  add({CB().v("εἰμί").pers(3).padj(A("μικρός", {"πάνυ"})).padj(A("λευκός", {"πάνυ"})).pg(F)});
  add({CB(ClauseType::Imp).v("φοβέω").voice(Middle).neg()});
  add({CB(ClauseType::Imp).v("ἀνοίγω", Aorist).o(d("θύρα")).polite("ἀντιβολέω")});
  add({CB().v("εἰμί").s(d("θύρα")).padj(A("μικρός", {"λίαν"}))});
  add({CB().v("διέρχομαι").modal("δύναμαι", Aorist).pers(1).neg()});
  add({CB(ClauseType::Imp).v("πίνω", Aorist).o(sb("οὗτος", Sg, N))});
  add({CB().v("εἰμί").s(pr(1, Sg, F)).padj(A("μέγας", {"λίαν"})).adv("νῦν").punct("!")});
  add({CB().v("εἰμί").s(pr(1, Sg, F)).padj(A("μικρός", {"λίαν"})).adv("νῦν")});
  add({CB(ClauseType::Wh).wh("ποῖ").v("ἀπέρχομαι", Aorist).s(sb("πᾶς", Pl, M))});
  add({CB(ClauseType::Excl).hos().s(with(d("κῆπος"), A("θαυμαστός")))});
  add({CB().v("λαλέω").modal("δύναμαι").s(d("ἄνθος", Pl)).neg()});
  add({CB().v("λαλέω").modal("δύναμαι").pers(1, Pl).conn("δήπου")});
  add({CB(ClauseType::Imp).v("ᾄδω", Aorist).io(pr(1, Pl)).o(n("ᾠδή"))});
  add({CB().v("οἶδα").pers(1).o(with(n("ᾠδή"), A("οὐδείς"))).neg()});
  add({CB(ClauseType::Imp).v("ἀπέρχομαι", Aorist).conn("οὖν")});
  add({CB().v("εἰμί").s(sb("πᾶς", Pl, M)).pers(2, Pl).padj(A("ἄγροικος", {"πάνυ"}))});
  add({CB().v("ἔρχομαι").s(d("βασίλεια")).punct("!")});
  add({CB(ClauseType::Imp).voc(sb("πᾶς", Pl, M)).v("προσκυνέω").punct("!")});
  add({CB(ClauseType::Imp).nb(Pl).v("ἀποτέμνω", Aorist).o(poss(d("κεφαλή"), 3, Sg, M)).punct("!")});
  add({CB().v("εἰμί").pers(3).pred(gen(n("βασίλεια"), d("καρδία", Pl)))});
  add({CB(ClauseType::Yn).ara().v("παίζω").pers(2).obl(nullptr, n("χάρτης", Pl), false, Dat)});
  add({CB().v("παίζω", Aorist).pers(1).adv("οὐδέποτε").adv("πρότερον")});
  add({CB(ClauseType::Imp).v("δίδωμι", Aorist).io(pr(1)).o(d("σφαῖρα"))});
  add({CB(ClauseType::Wh).wh("ποῦ").v("εἰμί").s(d("σφαῖρα")).exist()});
  add({CB().v("φεύγω", Aorist).pers(3)});
  add({CB().v("τρέχω").s(d("σφαῖρα", Pl)).neg()});
  add({CB().v("τρέχω").s(sb("οὗτος", Sg, F)).conn("δέ")});
  add({CB(ClauseType::Imp).v("λαμβάνω", Aorist).o(n("ποτόν"))});
  add({CB().v("εἰμί").s(n("ποτόν")).exist().neg()});
  add({CB(ClauseType::Imp).v("λαμβάνω", Aorist).o(n("πλακοῦς")).conn("οὖν")});
  add({CB().v("βούλομαι").pers(1).o(n("πλακοῦς")).neg()});
  add({CB(ClauseType::Wh).wh("πηνίκα").v("εἰμί").pers(3)});
  return t;
}

// Further clauses in the Athenaze style (own sentences).
std::vector<Row> moreRows() {
  std::vector<Row> t;
  auto add = [&](const CB& b, const char* e) { t.push_back(Row{{b.c}, e}); };
  add(CB().v("εἰμί").s(d("παῖς")).obl("ἐν", d("ἀγρός")), "ὁ παῖς ἐν τῷ ἀγρῷ ἐστιν.");
  add(CB(ClauseType::Yn).ara().v("φιλέω").s(d("κόρη")).o(d("μήτηρ")), "ἆρα φιλεῖ ἡ κόρη τὴν μητέρα;");   // decision 4: verb first
  add(CB().v("οἶδα").s(sb("οὐδείς", Sg, M)), "οὐδεὶς οἶδεν.");
  add(CB().v("οἰκέω", Imperfect).s(d("γαλῆ")).obl("ἐν", d("κῆπος"), true).exist(), "ἐν τῷ κήπῳ ᾤκει ἡ γαλῆ.");
  add(CB(ClauseType::Wh).v("ἔχω").pers(2).o(interrog(n("ἀδελφός", Pl), "πόσος")), "πόσους ἀδελφοὺς ἔχεις;");
  add(CB().v("ἔχω").pers(1).o(et(num(n("ἀδελφός", Pl), "δύο"), num(n("ἀδελφή"), "εἷς"))),
      "δύο ἀδελφοὺς καὶ μίαν ἀδελφὴν ἔχω.");
  add(CB().v("εἰμί").s(poss(d("πατήρ"), 1)).pred(n("γεωργός")), "ὁ πατήρ μου γεωργός ἐστιν.");
  add(CB(ClauseType::Imp).v("φοβέω", Aorist).voice(Passive).neg(), "μὴ φοβηθῇς.");
  add(CB(ClauseType::Wh).voc(n("παῖς")).wh("ποῦ").v("εἰμί").s(poss(d("πατήρ"), 2)).exist(), "ὦ παῖ, ποῦ ἐστιν ὁ πατήρ σου;");
  add(CB().v("αἴρω").s(d("δοῦλος")).o(d("λίθος")), "ὁ δοῦλος τὸν λίθον αἴρει.");
  add(CB().v("δίδωμι").s(d("δεσπότης")).io(d("δοῦλος")).o(d("οἶνος")), "ὁ δεσπότης τῷ δούλῳ τὸν οἶνον δίδωσιν.");
  add(CB().v("ἕπομαι").s(d("παῖς")).o(d("πατήρ")), "ὁ παῖς τῷ πατρὶ ἕπεται.");
  add(CB().v("ἄρχω").s(d("βασιλεύς")).o(d("πόλις")), "ὁ βασιλεὺς τῆς πόλεως ἄρχει.");
  add(CB().v("βαίνω").s(d("δοῦλος", Pl)).obl("εἰς", d("ἀγρός")), "οἱ δοῦλοι εἰς τὸν ἀγρὸν βαίνουσιν.");
  add(CB().v("ἔρχομαι").s(d("γυνή")).obl("ἐκ", d("οἰκία")), "ἡ γυνὴ ἐκ τῆς οἰκίας ἔρχεται.");
  add(CB().v("βαδίζω").s(d("παῖς")).obl("μετά", d("πατήρ")), "ὁ παῖς μετὰ τοῦ πατρὸς βαδίζει.");
  add(CB(ClauseType::Wh).v("λέγω").pers(2).neg().obl("διά", sb("τίς", Sg, N, Pron), false, Acc), "διὰ τί οὐ λέγεις;");
  add(CB().v("ἀκούω").modal("βούλομαι").pers(1), "βούλομαι ἀκούειν.");
  add(CB().v("ἀπέρχομαι").modal("δεῖ", Aorist).s(pr(1, Pl)), "δεῖ ἡμᾶς ἀπελθεῖν.");
  add(CB().v("ἀπέρχομαι").modal("ἔξεστι", Aorist).s(pr(2)), "ἔξεστί σοι ἀπελθεῖν.");
  add(CB().v("εἰμί").s(withRel(d("ἀνήρ"), CB().rel(Role::Object).v("ὁράω").pers(2))).padj(A("σοφός")),
      "ὁ ἀνὴρ ὃν ὁρᾷς σοφός ἐστιν.");
  add(CB().v("τρέχω").s(withRel(d("κόρη"), CB().rel(Role::Subject).v("φιλέω").o(d("γαλῆ")))),
      "ἡ κόρη ἣ τὴν γαλῆν φιλεῖ τρέχει.");
  add(CB(ClauseType::Imp).v("ἀκούω").sub(SubRel::Condition, CB().v("βούλομαι").pers(2), true), "εἰ βούλει, ἄκουε.");
  add(CB().v("καθεύδω", Imperfect).s(d("παῖς")).sub(SubRel::Time, CB().v("ἔρχομαι", Aorist).s(d("μήτηρ")), true),
      "ἐπεὶ ἡ μήτηρ ἦλθεν, ὁ παῖς ἐκάθευδεν.");
  add(CB().v("σπεύδω").s(d("γεωργός")).sub(SubRel::Purpose, CB().v("λύω", Aorist).o(d("βοῦς"))),
      "ὁ γεωργὸς σπεύδει ἵνα τὸν βοῦν λύσῃ.");
  add(CB().v("σπεύδω").s(d("γεωργός")).sub(SubRel::Purpose, CB().v("ἥκω").adv("ὀψέ").neg()),
      "ὁ γεωργὸς σπεύδει ἵνα μὴ ὀψὲ ἥκῃ.");
  add(CB().v("χαίρω").pers(1).sub(SubRel::Cause, CB().v("ἥκω").pers(2)), "χαίρω ὅτι ἥκεις.");
  add(CB().v("χαίρω").pers(1).sub(SubRel::Condition, CB().v("ἔρχομαι", Aorist).mood(Subjunctive).pers(2), true),
      "ἐὰν ἔλθῃς, χαίρω.");
  add(CB().v("τρέχω").s(d("παῖς")).sub(SubRel::Result, CB().v("φεύγω").o(d("λύκος"))),
      "ὁ παῖς τρέχει ὥστε τὸν λύκον φεύγειν.");
  add(CB(ClauseType::Yn).bias(YnBias::ExpectNo).v("ὀργίζω").voice(Middle).pers(2), "ἆρα μὴ ὀργίζῃ;");
  add(CB(ClauseType::Yn).bias(YnBias::ExpectYes).v("φιλέω").pers(2).o(d("μήτηρ")), "τὴν μητέρα οὐ φιλεῖς;");
  add(CB(ClauseType::Imp).voc(n("φίλος")).v("χαίρω"), "ὦ φίλε, χαῖρε.");
  add(CB().v("ζητέω").s(nm("Alice")).o(d("γαλῆ")), "ἡ Ἀλίκη τὴν γαλῆν ζητεῖ.");
  add(CB().v("δίδωμι").s(nm("Mark")).io(nm("Alice")).o(d("σφαῖρα")), "ὁ Μᾶρκος τῇ Ἀλίκῃ τὴν σφαῖραν δίδωσιν.");
  add(CB(ClauseType::Yn).v("ὁράω").pers(2).o(nm("Philip")), "τὸν Φίλιππον ὁρᾷς;");
  add(CB().v("εἰμί").s(dem(n("ἀνήρ"))).padj(A("σοφός")), "οὗτος ὁ ἀνὴρ σοφός ἐστιν.");
  add(CB().v("μένω").s(quant(d("δοῦλος", Pl), "πᾶς")).obl("ἐν", d("ἀγρός")), "πάντες οἱ δοῦλοι ἐν τῷ ἀγρῷ μένουσιν.");
  add(CB().v("τρέχω").s(with(d("παῖς"), A("καλός"))), "ὁ καλὸς παῖς τρέχει.");
  add(CB().v("ἥκω").s(with(n("ἀνήρ"), A("ἀγαθός"))), "ἀνὴρ ἀγαθὸς ἥκει.");
  add(CB().v("εἰμί").s(gen(d("οἶκος"), d("γεωργός"))).padj(A("μικρός")), "ὁ οἶκος τοῦ γεωργοῦ μικρός ἐστιν.");
  add(CB().v("λέγω").s(nm("Dicaeopolis")).o(sb("οὗτος", Pl, N)), "ὁ Δικαιόπολις ταῦτα λέγει.");
  add(CB().v("ὁράω").pers(1).o(nm("Dicaeopolis")), "τὸν Δικαιόπολιν ὁρῶ.");
  add(CB().v("παίζω").s(d("παιδίον", Pl)), "τὰ παιδία παίζει.");
  add(CB().v("εἰμί").s(nm("Socrates")).padj(A("σοφός")), "ὁ Σωκράτης σοφός ἐστιν.");
  add(CB(ClauseType::Wh).voc(nm("Socrates", false)).wh("τίς", Role::Object, N).v("λέγω").pers(2), "ὦ Σώκρατες, τί λέγεις;");
  add(CB().v("βαδίζω").pers(1).obl("μετά", pr(2)), "μετὰ σοῦ βαδίζω.");
  add(CB().v("σπεύδω").s(d("δοῦλος")).conn("καί"), "καὶ ὁ δοῦλος σπεύδει.");
  add(CB().v("σπεύδω").s(d("δοῦλος")).conn("δέ"), "ὁ δὲ δοῦλος σπεύδει.");
  add(CB(ClauseType::Wh).wh("τίς", Role::Object).v("ὁράω").pers(2), "τίνα ὁρᾷς;");
  add(CB(ClauseType::Wh).wh("πῶς").v("ἔχω").pers(2), "πῶς ἔχεις;");
  add(CB(ClauseType::Imp).v("ἀκούω").adv("νῦν"), "νῦν ἄκουε.");
  add(CB().v("λύω", Future).s(d("γεωργός")).o(d("βοῦς", Pl)), "ὁ γεωργὸς τοὺς βοῦς λύσει.");
  add(CB().v("λύω", Aorist).s(d("γεωργός")).o(d("βοῦς", Pl)), "ὁ γεωργὸς τοὺς βοῦς ἔλυσεν.");
  add(CB().v("λύω", Perfect).s(d("γεωργός")).o(d("βοῦς", Pl)), "ὁ γεωργὸς τοὺς βοῦς λέλυκεν.");
  add(CB().v("εἰμί", Imperfect).s(d("ἀγρός")).padj(A("μικρός")), "ὁ ἀγρὸς μικρὸς ἦν.");
  add(CB().v("ποιέω").pers(1).neg().o(sb("οὗτος", Pl, N)), "ταῦτα οὐ ποιῶ.");
  add(CB().v("τιμάω").s(d("ἄνθρωπος", Pl)).o(d("θεός", Pl)), "οἱ ἄνθρωποι τοὺς θεοὺς τιμῶσιν.");
  add(CB().v("εἰμί").s(d("ὕδωρ")).padj(A("καλός")), "τὸ ὕδωρ καλόν ἐστιν.");
  return t;
}

std::string realiseRow(grc::GreekRealiser& R, const Row& r, const GrcOptions& o) {
  std::string out;
  GrcSentence s;
  for (const GrcClause& c : r.c) {
    R.realise(c, o, s);
    if (!out.empty()) out += ' ';
    out += s.text;
  }
  return out;
}

std::vector<std::vector<std::string>> goldLines() {
  std::vector<std::vector<std::string>> out;
  std::ifstream in(repoRoot() / "tests" / "regression" / "expected" / "own_dialogue.grc.gold.txt");
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::vector<std::string> alts;
    size_t p = 0;
    for (;;) {
      size_t q = line.find(" | ", p);
      alts.push_back(text::nfc(line.substr(p, q == std::string::npos ? std::string::npos : q - p)));
      if (q == std::string::npos) break;
      p = q + 3;
    }
    out.push_back(alts);
  }
  return out;
}
}  // namespace

TEST_CASE("rules-grc: real lexicon - gold file (first 40 regression sentences from hand-built clauses)") {
  NEED_GRC();
  g_lx = &realGrc().lx;
  const auto gold = goldLines();
  REQUIRE(gold.size() >= 40);   // the gold file grew to 114 lines (C16); the hand-built clauses cover the first 40
  const std::vector<Row> rows = goldRows();
  REQUIRE(rows.size() == 40);
  grc::GreekRealiser R(realGrc().lx, cur(), gdata());
  check::GreekChecker ck(realGrc().lx, cur(), gdata());
  GrcOptions o;
  size_t match = 0, clean = 0;
  for (size_t i = 0; i < rows.size(); ++i) {
    const std::string got = realiseRow(R, rows[i], o);
    const bool hit = std::find(gold[i].begin(), gold[i].end(), got) != gold[i].end();
    CHECK_MESSAGE(hit, "line " << i + 1 << ": got '" << got << "' gold '" << gold[i][0] << "'");
    match += hit;
    const check::GrcReport rep = ck.check(got, {});
    const std::string f = rep.failures({"A1", "A3", "A4"});
    CHECK_MESSAGE(f.empty(), got << " -> " << f);
    clean += f.empty();
  }
  MESSAGE("gold: " << match << " / 40 match, " << clean << " / 40 clean under A1/A3/A4");
}

TEST_CASE("rules-grc: real lexicon - realisation table (>= 60 clauses) matches and passes the checker") {
  NEED_GRC();
  g_lx = &realGrc().lx;
  std::vector<Row> rows = moreRows();
  const size_t nMore = rows.size();
  {
    const auto gold = goldLines();
    std::vector<Row> g = goldRows();
    for (size_t i = 0; i < g.size() && i < gold.size(); ++i) { g[i].expect = gold[i][0]; }
    // gold rows whose preferred line differs from the realiser's (alternatives) are checked in the gold test
    for (Row& r : g) rows.push_back(r);
  }
  REQUIRE(rows.size() >= 60);
  grc::GreekRealiser R(realGrc().lx, cur(), gdata());
  check::GreekChecker ck(realGrc().lx, cur(), gdata());
  GrcOptions o;
  size_t same = 0, clean = 0, compared = 0;
  GrcSentence s;
  size_t a1b = 0;
  for (size_t ri = 0; ri < rows.size(); ++ri) {
    const Row& r = rows[ri];
    const std::string got = realiseRow(R, r, o);
    if (r.c.size() == 1) {   // token offsets cover the text exactly
      R.realise(r.c[0], o, s);
      for (const auto& tv : s.tokens) {
        REQUIRE(tv.end <= (int)s.text.size());
        CHECK(s.text.substr((size_t)tv.start, (size_t)(tv.end - tv.start)) == tv.text);
      }
    }
    if (ri < nMore) {
      ++compared;
      CHECK_MESSAGE(got == text::nfc(r.expect), "got '" << got << "' expected '" << r.expect << "'");
      same += got == text::nfc(r.expect);
    }
    const check::GrcReport rep = ck.check(got, {});
    const std::string f = rep.failures({"A1", "A3", "A4"});
    CHECK_MESSAGE(f.empty(), got << " -> " << f);
    clean += f.empty();
    // the realiser's sandhi and the checker's accent sanity agree; no accent-insensitive reading
    for (const check::Issue& is : rep.issues)
      if (is.id == "A1b" || is.id == "A1") { ++a1b; MESSAGE(got << " -> warning " << is.id << ": " << is.detail); }
  }
  CHECK(a1b == 0);
  MESSAGE("realisation table: " << rows.size() << " clauses (" << compared << " compared here, 40 in the gold test): "
                                << same << " / " << compared << " exact, " << clean << " / " << rows.size()
                                << " clean under A1/A3/A4");
}


namespace {
std::vector<std::string> splitIds(const std::string& s) {
  std::vector<std::string> out;
  if (s == "-" || s.empty()) return out;
  size_t p = 0;
  for (;;) {
    size_t q = s.find(',', p);
    out.push_back(s.substr(p, q == std::string::npos ? std::string::npos : q - p));
    if (q == std::string::npos) break;
    p = q + 1;
  }
  return out;
}
}  // namespace

TEST_CASE("rules-grc: real lexicon - checker table (tests/fixtures/rules_grc/check_grc.tsv)") {
  NEED_GRC();
  const auto rows = readTsv(fixture("check_grc.tsv"));
  REQUIRE(rows.size() >= 40);
  check::GreekChecker ck(realGrc().lx, cur(), gdata());
  size_t ok = 0;
  for (const auto& r : rows) {
    REQUIRE(r.size() >= 2);
    const check::GrcReport rep = ck.check(text::nfc(r[0]), {});
    const std::vector<std::string> want = splitIds(r[1]);
    bool good = true;
    std::string got;
    for (const check::Issue& is : rep.issues) got += is.id + (is.warning ? "w" : "") + " (" + is.detail + "); ";
    for (const std::string& w : want) {
      const bool warn = w.back() == 'w';
      const std::string id = warn ? w.substr(0, w.size() - 1) : w;
      good = good && rep.count(id, warn) > 0;
    }
    // no unexpected faults (warnings are allowed besides the listed ones)
    for (const check::Issue& is : rep.issues)
      if (!is.warning && std::find(want.begin(), want.end(), is.id) == want.end()) good = false;
    if (want.empty())
      for (const check::Issue& is : rep.issues) good = good && (is.id == "A6" || is.id == "A1b" ? true : is.warning == false ? false : true);
    CHECK_MESSAGE(good, r[0] << " expected " << r[1] << " got " << got);
    ok += good;
  }
  MESSAGE("checker table: " << ok << " / " << rows.size());
  // A6 with a tier ceiling
  check::GreekCheckOptions o;
  o.tierCeiling = 1;
  const check::GrcReport rep = ck.check(text::nfc("ὁ παῖς τὸν πλακοῦντα ἐσθίει."), o);
  CHECK(rep.count("A6") >= 1);
  o.tierCeiling = 3;
  CHECK(ck.check(text::nfc("ὁ παῖς τὸν πλακοῦντα ἐσθίει."), o).count("A6") == 0);
  // the report lists A1, A1b, A3, A4, A6
  REQUIRE(rep.checks.size() == 5);
  CHECK(rep.checks[1].id == "A1b");
}

TEST_CASE("rules-grc: real lexicon - token views, reasons, emoji, flags, names, elision option") {
  NEED_GRC();
  g_lx = &realGrc().lx;
  grc::GreekRealiser R(realGrc().lx, cur(), gdata());
  GrcOptions o;
  GrcSentence s = R.realise(CB().v("ὁράω").s(d("παῖς")).o(d("ἥλιος")).c, o);
  CHECK(s.text == text::nfc("ὁ παῖς τὸν ἥλιον ὁρᾷ."));
  REQUIRE(s.tokens.size() == 5);
  CHECK(s.tokens[3].emoji == "☀️");
  CHECK(s.tokens[3].features.case_ == "accusative");
  CHECK(s.tokens[4].features.person == "third");
  CHECK(s.tokens[0].features.pos == "article");
  CHECK(s.tokens[1].tier == 1);
  CHECK(!s.reasons.empty());
  o.emojiInText = true;
  CHECK(R.realise(CB().v("ὁράω").s(d("παῖς")).o(d("ἥλιος")).c, o).text == text::nfc("ὁ παῖς τὸν ἥλιον ☀️ ὁρᾷ."));
  o.emojiInText = false;
  // unknown names are kept and flagged; unknown source words are bracketed
  s = R.realise(CB().v("τρέχω").s(nm("Bartholomew")).c, o);
  CHECK(std::find(s.flags.begin(), s.flags.end(), "name-guessed") != s.flags.end());
  GrcNP lit;
  lit.literal = "tea";
  s = R.realise(CB(ClauseType::Imp).v("πίνω").o(lit).c, o);
  CHECK(s.text == "πῖνε [tea].");
  CHECK(std::find(s.flags.begin(), s.flags.end(), "unknown") != s.flags.end());
  // glossary: a decline policy with a Greek form
  std::vector<rules::GlossaryEntry> gl{{"Bartholomew", "decline", "Βαρθολομαῖος", "m", 2}};
  o.glossary = &gl;
  CHECK(R.realise(CB().v("ὁράω").pers(1).o(nm("Bartholomew")).c, o).text == text::nfc("τὸν Βαρθολομαῖον ὁρῶ."));
  o.glossary = nullptr;
  // elision (option, off by default)
  GrcClause c = CB().v("μένω").pers(1).conn("ἀλλά").obl("ἐπί", d("ἵππος"), false, Gen).c;
  CHECK(R.realise(c, o).text == text::nfc("ἀλλὰ ἐπὶ τοῦ ἵππου μένω."));
  o.elision = true;
  CHECK(R.realise(c, o).text == text::nfc("ἀλλ’ ἐπὶ τοῦ ἵππου μένω."));
  o.elision = false;
  // ἀποφεύγω has its aorist cells since the lexicon follow-up (B4b): no missing form any more
  s = R.realise(CB().v("ἀποφεύγω", Aorist).pers(3).c, o);
  CHECK(s.text == text::nfc("ἀπέφυγεν."));
  CHECK(std::find(s.flags.begin(), s.flags.end(), "missing-form") == s.flags.end());
  // a missing verb is reported, never invented
  {
    GrcClause mc = CB().modal("δύναμαι").pers(1).c;
    s = R.realise(mc, o);
    CHECK(std::find(s.flags.begin(), s.flags.end(), "missing-form") != s.flags.end());
  }
}

namespace {
// Deterministic generator of template clauses over a small Athenaze-style vocabulary.
struct ClauseGen {
  uint64_t st = 0x9E3779B97F4A7C15ull;
  uint32_t next() { st = st * 6364136223846793005ull + 1442695040888963407ull; return (uint32_t)(st >> 33); }
  size_t pick(size_t n) { return next() % n; }
};
const char* const kPersons[] = {"παῖς", "κόρη", "δοῦλος", "γεωργός", "γυνή", "ἀνήρ", "πατήρ", "μήτηρ", "ἀδελφός",
                                "ἀδελφή", "θυγάτηρ", "δεσπότης", "βασιλεύς", "φίλος"};
const char* const kAnimals[] = {"γαλῆ", "κύων", "ἵππος", "βοῦς", "λύκος"};
const char* const kThings[] = {"λίθος", "οἶνος", "ἄρτος", "σφαῖρα", "βιβλίον", "δῶρον", "θύρα", "ἐπιστολή", "ἵππος"};
const char* const kAdjs[] = {"μικρός", "μέγας", "καλός", "ἀγαθός", "σοφός", "νέος", "κακός"};
const char* const kAccVerbs[] = {"ὁράω", "φιλέω", "λύω", "ἄγω", "φέρω", "ζητέω", "λαμβάνω", "ἔχω", "καλέω", "τιμάω", "διώκω"};
const char* const kDatVerbs[] = {"ἕπομαι", "βοηθέω", "πιστεύω"};
const char* const kGenVerbs[] = {"ἄρχω", "ἐπιθυμέω", "κρατέω"};
struct Motion { const char* verb; const char* prep; uint8_t cs; };
const Motion kMotion[] = {{"βαίνω", "εἰς", Acc}, {"ἔρχομαι", "ἐκ", Gen}, {"μένω", "ἐν", Dat}, {"τρέχω", "πρός", Acc},
                          {"βαδίζω", "μετά", Gen}, {"οἰκέω", "ἐν", Dat}, {"ἔρχομαι", "ἀπό", Gen}, {"βαίνω", "παρά", Acc}};
const char* const kPlaces[] = {"ἀγρός", "οἰκία", "κῆπος", "πόλις", "ὁδός", "θάλαττα"};

GrcNP genNP(ClauseGen& g, const char* head, bool allowPl = true) {
  GrcNP x = n(head, allowPl && g.pick(3) == 0 ? Pl : Sg);
  x.definite = g.pick(4) != 0;
  if (g.pick(3) == 0) x.adjectives.push_back(A(kAdjs[g.pick(7)]));
  return x;
}
uint8_t genTense(ClauseGen& g) {
  static const uint8_t t[] = {Present, Present, Aorist, Imperfect, Future};
  return t[g.pick(5)];
}

GrcClause genClause(ClauseGen& g) {
  CB b;
  switch (g.pick(10)) {
    case 0: case 1:   // S O V (accusative verbs)
      b.v(kAccVerbs[g.pick(11)], genTense(g)).s(genNP(g, kPersons[g.pick(14)])).o(genNP(g, kThings[g.pick(9)]));
      break;
    case 2:   // dative verbs
      b.v(kDatVerbs[g.pick(3)], genTense(g)).s(genNP(g, kPersons[g.pick(14)])).o(genNP(g, kPersons[g.pick(14)]));
      break;
    case 3:   // genitive verbs
      b.v(kGenVerbs[g.pick(3)]).s(genNP(g, kPersons[g.pick(14)])).o(genNP(g, g.pick(2) ? kThings[g.pick(9)] : kPlaces[g.pick(6)]));
      break;
    case 4: {   // motion with a preposition
      const Motion& m = kMotion[g.pick(8)];
      GrcNP pl = genNP(g, kPlaces[g.pick(6)], false);
      pl.definite = true;
      b.v(m.verb, genTense(g)).s(genNP(g, g.pick(2) ? kPersons[g.pick(14)] : kAnimals[g.pick(5)])).obl(m.prep, pl, g.pick(3) == 0, m.cs);
      break;
    }
    case 5: {   // copula with a predicate adjective
      GrcNP sj = genNP(g, g.pick(2) ? kPersons[g.pick(14)] : kThings[g.pick(9)]);
      sj.definite = true;
      sj.adjectives.clear();
      b.v("εἰμί", g.pick(3) == 0 ? Imperfect : Present).s(sj).padj(A(kAdjs[g.pick(7)]));
      break;
    }
    case 6: {   // imperatives with an enclitic indirect object
      CB i(ClauseType::Imp);
      i.v(g.pick(2) ? "δίδωμι" : "φέρω", g.pick(2) ? Aorist : Present).o(genNP(g, kThings[g.pick(9)]));
      if (g.pick(2)) i.io(pr(1));
      if (g.pick(4) == 0) i.nb(Pl);
      return i.c;
    }
    case 7: {   // negative questions and statements with pronoun subjects
      CB q(g.pick(2) ? ClauseType::Yn : ClauseType::Decl);
      if (q.c.type == ClauseType::Yn) q.ara();
      q.v(kAccVerbs[g.pick(11)], genTense(g)).pers((uint8_t)(1 + g.pick(3)), g.pick(2) ? Sg : Pl).o(genNP(g, kAnimals[g.pick(5)]));
      if (g.pick(2)) q.neg();
      return q.c;
    }
    case 8:   // modal + infinitive
      b.v(kAccVerbs[g.pick(11)]).modal(g.pick(2) ? "δύναμαι" : "βούλομαι", g.pick(2) ? Aorist : Present)
          .s(genNP(g, kPersons[g.pick(14)])).o(genNP(g, kThings[g.pick(9)]));
      if (g.pick(3) == 0) b.neg();
      break;
    default: {   // neuter plural subjects (singular verb) and relative clauses
      if (g.pick(2)) {
        GrcNP sj = d(g.pick(2) ? "παιδίον" : "τέκνον", Pl);
        b.v(g.pick(2) ? "τρέχω" : "παίζω").s(sj);
      } else {
        GrcNP sj = d(kPersons[g.pick(14)]);
        sj.relative.push_back(CB().rel(Role::Object).v(kAccVerbs[g.pick(11)]).pers(2).c);
        b.v("τρέχω").s(sj);
      }
      break;
    }
  }
  return b.c;
}

uint8_t caseOfName(const std::string& s) {
  if (s == "nominative") return Nom;
  if (s == "genitive") return Gen;
  if (s == "dative") return Dat;
  if (s == "accusative") return Acc;
  if (s == "vocative") return Voc;
  return 0;
}
uint8_t numOfName(const std::string& s) { return s == "singular" ? Sg : s == "plural" ? Pl : 0; }
uint8_t genOfName(const std::string& s) { return s == "masculine" ? M : s == "feminine" ? F : s == "neuter" ? N : 0; }
uint8_t tenseOfName(const std::string& s) {
  static const char* const t[] = {"", "present", "imperfect", "future", "perfect", "pluperfect", "future-perfect", "aorist"};
  for (uint8_t i = 1; i < 8; ++i)
    if (s == t[i]) return i;
  return 0;
}

// One corruption of a realised sentence: an article's gender, a noun's case, an adjective's gender or a finite
// verb's number. Returns the corrupted text, or "" when the sentence offers no corruption of that kind.
std::string corrupt(const lex::Lexicon& lx, const GrcSentence& s, int kind, const grc::GreekRealiser& R) {
  for (size_t i = 0; i < s.tokens.size(); ++i) {
    const rules::TokenView& t = s.tokens[i];
    std::string repl;
    const uint8_t cs = caseOfName(t.features.case_), nb = numOfName(t.features.number), gd = genOfName(t.features.gender);
    if (kind == 0 && t.hasLemma && t.lemmaId == R.closed().art && cs && cs != Gen && i + 1 < s.tokens.size() &&
        s.tokens[i + 1].hasLemma) {   // article gender: one the next word's lemma does not admit (not ἡ λίθος)
      // a gender no reading of the next word admits in this case (not ἡ λίθος, not ἡ γεωργός "the farming one")
      morph::Token nt;
      grc::analyse(lx, grc::ultimaToAcute(s.tokens[i + 1].text), nt);
      for (uint8_t g2 : {M, F, N}) {
        if (g2 == gd) continue;
        bool admitted = false;
        for (const lex::Analysis& a : nt.analyses) {
          const Features f = unpack(lx.feature(a.feat));
          const uint8_t g = f.gender ? f.gender : lx.lemma(a.lemma).gender;
          if (f.case_ == cs && (g == 0 || g == g2 || morph::genderAdmits(g, g2))) admitted = true;
        }
        if (!admitted) { grc::article(cs, nb, g2, repl); break; }
      }
    } else if (kind == 1 && t.features.pos == "noun" && cs && i > 0 && s.tokens[i - 1].hasLemma &&
               s.tokens[i - 1].lemmaId == R.closed().art) {   // noun case inside an article phrase
      grc::generate(lx, t.lemmaId, grc::nounForm(cs == Dat ? Acc : Dat, nb), repl);
    } else if (kind == 2 && t.features.pos == "adjective" && cs && gd && gd != N) {   // adjective gender
      grc::generate(lx, t.lemmaId, grc::adjForm(cs, nb, N), repl);
    } else if (kind == 3 && t.features.pos == "verb" && t.features.person == "third" && t.features.mood == "indicative") {
      grc::generate(lx, t.lemmaId, grc::verbForm(P3, nb == Sg ? Pl : Sg, tenseOfName(t.features.tense), Indicative,
                                                 t.features.voice == "middle" ? Middle : t.features.voice == "passive" ? Passive : Active), repl);
    } else continue;
    if (repl.empty() || text::greek_bare(repl) == text::greek_bare(t.text)) continue;
    return s.text.substr(0, (size_t)t.start) + repl + s.text.substr((size_t)t.end);
  }
  return std::string();
}
}  // namespace

TEST_CASE("rules-grc: real lexicon - 2,000 generated clauses pass A1/A3/A4; checker catches 200 corruptions") {
  NEED_GRC();
  g_lx = &realGrc().lx;
  grc::GreekRealiser R(realGrc().lx, cur(), gdata());
  check::GreekChecker ck(realGrc().lx, cur(), gdata());
  GrcOptions o;
  ClauseGen g;
  GrcSentence s;
  check::GrcReport rep;
  size_t faults = 0, missing = 0, accentWarn = 0, corrupted = 0, caught = 0;
  int kind = 0;
  for (int i = 0; i < 2000; ++i) {
    const GrcClause c = genClause(g);
    R.realise(c, o, s);
    if (!s.flags.empty()) { ++missing; MESSAGE("flags on: " << s.text); }
    ck.check(s.text, {}, rep);
    const std::string f = rep.failures({"A1", "A3", "A4"});
    if (!f.empty()) { ++faults; MESSAGE(s.text << " -> " << f); }
    accentWarn += rep.count("A1b", true) + rep.count("A1", true);
    if (corrupted < 200 && f.empty() && i % 3 == 0) {
      // the corrupting subject must not be a neuter plural (its verb may be singular or plural)
      bool neutPl = false;
      if (c.hasSubject && c.subject.number == Pl && !c.subject.isPronoun) {
        const lex::Lemma l = realGrc().lx.lemma(c.subject.head);
        neutPl = l.gender == N;
      }
      for (int k = 0; k < 4; ++k) {
        const int kk = (kind + k) % 4;
        if (kk == 3 && (neutPl || !c.hasSubject || c.subject.isPronoun || !c.subs.empty() || !c.subject.relative.empty()))
          continue;   // a verb's number is wrong only against an expressed subject
        const std::string bad = corrupt(realGrc().lx, s, kk, R);
        if (bad.empty()) continue;
        ++corrupted;
        ck.check(bad, {}, rep);
        const bool hit = !rep.failures({"A1", "A3", "A4"}).empty();
        caught += hit;
        CHECK_MESSAGE(hit, "corruption not caught: '" << bad << "' (from '" << s.text << "')");
        kind = kk + 1;
        break;
      }
    }
  }
  CHECK(faults == 0);
  CHECK(missing == 0);
  CHECK(corrupted == 200);
  CHECK(caught == corrupted);
  CHECK(accentWarn == 0);
  MESSAGE("generator: 2000 clauses, " << faults << " A1/A3/A4 faults, " << missing << " with flags, " << accentWarn
                                      << " accent warnings; corruptions caught " << caught << " / " << corrupted);
}

TEST_CASE("rules-grc: real lexicon - determinism (two realisers, byte-identical output and checks)") {
  NEED_GRC();
  g_lx = &realGrc().lx;
  grc::GreekRealiser R1(realGrc().lx, cur(), gdata()), R2(realGrc().lx, cur(), gdata());
  check::GreekChecker c1(realGrc().lx, cur(), gdata()), c2(realGrc().lx, cur(), gdata());
  ClauseGen g1, g2;
  GrcOptions o;
  for (int i = 0; i < 300; ++i) {
    const GrcSentence a = R1.realise(genClause(g1), o), b = R2.realise(genClause(g2), o);
    REQUIRE(a.text == b.text);
    CHECK(a.tokens.size() == b.tokens.size());
    CHECK(a.reasons.size() == b.reasons.size());
    const check::GrcReport ra = c1.check(a.text, {}), rb = c2.check(b.text, {});
    CHECK(ra.issues.size() == rb.issues.size());
  }
}

TEST_CASE("rules-grc: real lexicon - RSS flat over 10,000 realisations and checks") {
  NEED_GRC();
  g_lx = &realGrc().lx;
  grc::GreekRealiser R(realGrc().lx, cur(), gdata());
  check::GreekChecker ck(realGrc().lx, cur(), gdata());
  GrcOptions o;
  ClauseGen g;
  std::vector<GrcClause> pool;
  for (int i = 0; i < 200; ++i) pool.push_back(genClause(g));
  GrcSentence s;
  check::GrcReport rep;
  for (int i = 0; i < 200; ++i) { R.realise(pool[(size_t)i], o, s); ck.check(s.text, {}, rep); }
  const long before = rssAnonKb();
  for (int i = 0; i < 10000; ++i) { R.realise(pool[(size_t)i % pool.size()], o, s); ck.check(s.text, {}, rep); }
  const long after = rssAnonKb();
  MESSAGE("RssAnon " << before << " kB -> " << after << " kB over 10,000 realisations + checks");
#ifndef VP_GRC_TEST_SANITIZED
  if (before > 0) CHECK(after <= before + before / 20 + 256);
#endif
}

TEST_CASE("rules-grc: debug dump (VP_GRC_DEBUG=<sentence>)") {
  const char* s = std::getenv("VP_GRC_DEBUG");
  if (!s || !realGrc().ok) return;
  check::GreekChecker ck(realGrc().lx, cur(), gdata());
  const check::GrcReport rep = ck.check(text::nfc(s), {});
  for (const auto& t : rep.tokens) {
    std::cout << t.text << " seg " << t.segment << (t.name ? " name" : "") << (t.closed ? " closed" : "")
              << (t.accentDiffers ? " accent-differs" : "") << "\n";
    for (const auto& a : t.analysis.analyses)
    {
      const rules::Features fv = realise::featureView(realGrc().lx.feature(a.feat));
      std::cout << "   " << realGrc().lx.lemma(a.lemma).head << " " << fv.pos << " " << fv.case_ << " " << fv.number << " "
                << fv.gender << " " << fv.person << " " << fv.tense << " " << fv.mood << " " << fv.voice << " flags "
                << a.flags << "\n";
    }
  }
  for (const auto& i : rep.issues) std::cout << i.id << (i.warning ? "w" : "") << " " << i.detail << "\n";
}

TEST_CASE("rules-grc: scratch (VP_GRC_SCRATCH=1 prints the gold and table realisations)") {
  if (!std::getenv("VP_GRC_SCRATCH") || !realGrc().ok) return;
  g_lx = &realGrc().lx;
  grc::GreekRealiser R(realGrc().lx, cur(), gdata());
  GrcOptions o;
  for (const Row& r : goldRows()) std::cout << realiseRow(R, r, o) << "\n";
  std::cout << "----\n";
  for (const Row& r : moreRows()) std::cout << realiseRow(R, r, o) << "   [" << r.expect << "]\n";
  std::cout << "----\n";
  ClauseGen g;
  GrcSentence s;
  for (int i = 0; i < 60; ++i) { R.realise(genClause(g), o, s); std::cout << s.text << "\n"; }
}
