// engine/rules Latin side (C1): curated loaders, morphology (analyse / generate / paradigm fallback), realisation
// primitives and the checker. Always run on the committed fixture lexicon (tests/fixtures/lex/latin.vpl) and on a
// mini lexicon built here with the reference encoder (lemmas without tables -> paradigm fallback end to end); the
// big tables run on the real library (env VP_LATIN_VPL, default data/work/latin.vpl) and skip with a message when
// it is absent.
#include <doctest.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "lex_fixture_writer.h"
#include "vp/check.h"
#include "vp/curated.h"
#include "vp/features.h"
#include "vp/lex.h"
#include "vp/morph.h"
#include "vp/realise_la.h"
#include "vp/text.h"

#if defined(__SANITIZE_ADDRESS__)
#define VP_RULES_TEST_SANITIZED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define VP_RULES_TEST_SANITIZED 1
#endif
#endif

namespace stdfs = std::filesystem;
using namespace vp;
using namespace vp::feat;
using vp::realise::AdvPos;
using vp::realise::ClauseType;
using vp::realise::kNone;
using vp::realise::LaAdj;
using vp::realise::LaClause;
using vp::realise::LaNP;
using vp::realise::LaOblique;
using vp::realise::LaSentence;
using vp::realise::LaSub;
using vp::realise::LatinRealiser;
using vp::realise::Polarity;
using vp::realise::RealiseOptions;
using vp::realise::Role;
using vp::realise::SubRel;
using vp::realise::YnBias;

namespace {

stdfs::path repoRoot() { return stdfs::path(VP_FIXTURES_DIR).parent_path().parent_path(); }
stdfs::path rulesFixture(const char* name) { return stdfs::path(VP_FIXTURES_DIR) / "rules" / name; }
stdfs::path tmpDir() {
  stdfs::path d = stdfs::path(VP_TEST_TMP) / "rules";
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

struct Held { lex::Lexicon lx; bool ok = false; std::string why; };
Held openHeld(const stdfs::path& p) {
  Held h;
  auto r = lex::Lexicon::open(p);
  if (r.ok()) { h.lx = std::move(r.value()); h.ok = true; }
  else h.why = r.error().message;
  return h;
}
const Held& real() {
  static Held h = [] {
    const char* env = std::getenv("VP_LATIN_VPL");
    return openHeld(env && *env ? stdfs::path(env) : repoRoot() / "data" / "work" / "latin.vpl");
  }();
  return h;
}
const Held& fixture() {
  static Held h = openHeld(stdfs::path(VP_FIXTURES_DIR) / "lex" / "latin.vpl");
  return h;
}
#define NEED_REAL()                                                                                  \
  if (!real().ok) {                                                                                  \
    MESSAGE("real latin.vpl not available (" << real().why << "); set VP_LATIN_VPL. Skipped.");     \
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
  if (s == "name") return Name;
  if (s == "particle") return Particle;
  if (s == "prep") return Prep;
  if (s == "conj") return Conj;
  return 0;
}

// "3 sg pres ind act", "acc pl f comp", "perf ptc pass acc sg f" -> Features (pos left to the caller)
Features parseFeats(const std::string& s) {
  Features f;
  std::istringstream is(s);
  std::string w;
  while (is >> w) {
    if (w == "nom") f.case_ = Nom; else if (w == "gen") f.case_ = Gen; else if (w == "dat") f.case_ = Dat;
    else if (w == "acc") f.case_ = Acc; else if (w == "abl") f.case_ = Abl; else if (w == "voc") f.case_ = Voc;
    else if (w == "sg") f.number = Sg; else if (w == "pl") f.number = Pl;
    else if (w == "m") f.gender = M; else if (w == "f") f.gender = F; else if (w == "n") f.gender = N;
    else if (w == "1") f.person = P1; else if (w == "2") f.person = P2; else if (w == "3") f.person = P3;
    else if (w == "pres") f.tense = Present; else if (w == "impf") f.tense = Imperfect;
    else if (w == "fut") f.tense = Future; else if (w == "perf") f.tense = Perfect;
    else if (w == "plup") f.tense = Pluperfect; else if (w == "futp") f.tense = FuturePerfect;
    else if (w == "ind") f.mood = Indicative; else if (w == "subj") f.mood = Subjunctive;
    else if (w == "imp") f.mood = Imperative; else if (w == "inf") f.mood = Infinitive;
    else if (w == "ptc") f.mood = ParticipleMood;
    else if (w == "act") f.voice = Active; else if (w == "pass") f.voice = Passive;
    else if (w == "comp") f.degree = Comparative; else if (w == "sup") f.degree = Superlative;
  }
  return f;
}
bool featsMatch(const Features& have, const Features& want) {
  auto eq = [](uint8_t h, uint8_t w) { return w == 0 || h == w; };
  return eq(have.case_, want.case_) && eq(have.number, want.number) && eq(have.person, want.person) &&
         eq(have.tense, want.tense) && eq(have.mood, want.mood) && eq(have.voice, want.voice) &&
         eq(have.degree, want.degree) && (want.gender == 0 || have.gender == 0 || morph::genderAdmits(have.gender, want.gender));
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

// ---- clause-building DSL over one lexicon -------------------------------------------------------------------------
const lex::Lexicon* g_lx = nullptr;
uint32_t L(const char* head, uint8_t pos = 0) {
  uint32_t id = morph::findLemma(*g_lx, head, pos);
  if (id == kNone && pos) id = morph::findLemma(*g_lx, head);
  CHECK_MESSAGE(id != kNone, "lemma not found: " << head);
  return id;
}
LaNP n(const char* head, uint8_t number = Sg) {
  LaNP x;
  x.head = L(head, Noun);
  x.number = number;
  return x;
}
LaNP sb(const char* head, uint8_t number, uint8_t gender, uint8_t pos = 0) {   // substantive adj / pronoun
  LaNP x;
  x.head = L(head, pos);
  x.number = number;
  x.gender = gender;
  return x;
}
LaNP pr(uint8_t person, uint8_t number = Sg, uint8_t gender = 0, bool refl = false) {
  LaNP x;
  x.isPronoun = true;
  x.pron.person = person;
  x.pron.number = number;
  x.pron.gender = gender;
  x.pron.reflexive = refl;
  x.number = number;
  x.gender = gender;
  return x;
}
LaNP nm(const char* name) {
  LaNP x;
  x.isName = true;
  x.name = name;
  return x;
}
LaAdj A(const char* head, std::initializer_list<const char*> advs = {}, uint8_t degree = 0) {
  LaAdj a;
  a.lemma = L(head, Adj);
  for (const char* v : advs) a.adverbs.push_back(L(v, Adv));
  a.degree = degree;
  return a;
}
LaNP with(LaNP x, LaAdj a) { x.adjectives.push_back(a); return x; }
LaNP withPoss(LaNP x, const char* poss) { x.possessive = L(poss); return x; }
LaNP withNum(LaNP x, const char* num) { x.numeral = L(num); return x; }
LaNP withGen(LaNP x, LaNP g) { x.genitive.push_back(g); return x; }
LaNP withDet(LaNP x, realise::Det d) { x.det = d; return x; }
LaNP cap(LaNP x) { x.capitalise = true; return x; }
LaNP et(LaNP x, LaNP y) { x.coord.push_back(y); return x; }
LaNP emph(LaNP x) { x.emphasis = true; return x; }

struct CB {
  LaClause c;
  explicit CB(ClauseType t = ClauseType::Decl) { c.type = t; }
  CB& v(const char* verb, uint8_t tense = Present) { c.pred.lemma = L(verb, Verb); c.pred.tense = tense; return *this; }
  CB& mood(uint8_t m) { c.pred.mood = m; return *this; }
  CB& passive() { c.pred.voice = Passive; return *this; }
  CB& pers(uint8_t p, uint8_t num = Sg) { c.pred.person = p; c.pred.number = num; return *this; }
  CB& num(uint8_t num) { c.pred.number = num; return *this; }
  CB& modal(const char* m) { c.pred.modal = L(m, Verb); return *this; }
  CB& neg() { c.polarity = Polarity::Neg; return *this; }
  CB& s(LaNP x) { c.hasSubject = true; c.subject = x; return *this; }
  CB& o(LaNP x) { c.hasObject = true; c.object = x; return *this; }
  CB& io(LaNP x) { c.hasIndirect = true; c.indirect = x; return *this; }
  CB& obl(const char* prep, LaNP x, bool front = false, uint8_t cs = 0) {
    LaOblique ob;
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
  CB& voc(LaNP x) { c.vocatives.push_back(x); return *this; }
  CB& intj(const char* k) { c.interjections.push_back(L(k)); return *this; }
  CB& pred(LaNP x) { c.predicative.push_back(x); return *this; }
  CB& padj(LaAdj a) { c.predAdj.push_back(a); return *this; }
  CB& pg(uint8_t g, uint8_t num = 0) { c.predGender = g; c.predNumber = num; return *this; }
  CB& wh(const char* w, Role r = Role::None, uint8_t g = 0) { c.wh.lemma = L(w); c.wh.role = r; c.wh.gender = g; return *this; }
  CB& exist() { c.existential = true; return *this; }
  CB& punct(const char* p) { c.punct = p; return *this; }
  CB& bias(YnBias b) { c.bias = b; return *this; }
  CB& quam() { c.exclQuam = true; return *this; }
  CB& exclO() { c.exclO = true; return *this; }
  CB& sub(SubRel rel, const CB& sc, bool before = false, const char* conj = nullptr) {
    LaSub su;
    su.rel = rel;
    su.before = before;
    su.conj = conj ? L(conj, Conj) : kNone;
    su.clause.push_back(sc.c);
    c.subs.push_back(su);
    return *this;
  }
  CB& rel(Role r) { c.relRole = r; return *this; }
};
LaNP withRel(LaNP x, const CB& rc) { x.relative.push_back(rc.c); return x; }

std::string failures(const check::Report& r, std::initializer_list<const char*> ids) {
  std::string s;
  for (const check::Issue& i : r.issues)
    if (!i.warning && std::find_if(ids.begin(), ids.end(), [&](const char* x) { return i.id == x; }) != ids.end())
      s += i.id + ": " + i.detail + "; ";
  return s;
}

// The mini lexicon: lemmas without inflection tables (paradigm fallback), sum with a table, a few particles.
lexfix::LexiconIn miniLexicon() {
  lexfix::LexiconIn in;
  in.lang = "la";
  in.notice = "rules test mini lexicon (own data)";
  auto lemma = [&](const char* head, uint8_t pos, uint8_t cls, uint8_t gender, const char* principal, uint16_t flags = 0) {
    lexfix::LemmaIn l;
    l.head = text::nfc(head);
    l.key = text::latin_key(head);
    l.pos = pos;
    l.cls = cls;
    l.gender = gender;
    l.tier = 1;
    l.flags = flags;
    l.principal = principal;
    l.principalCount = 1;
    in.lemmas.push_back(l);
    return (uint32_t)(in.lemmas.size() - 1);
  };
  auto head = [&](uint32_t id, uint32_t packed) {
    in.analyses.push_back({in.lemmas[id].key, id, packed, 0, in.lemmas[id].head});
  };
  auto nf = [](uint8_t pos, uint8_t c, uint8_t num) { Features f; f.pos = pos; f.case_ = c; f.number = num; return pack(f); };
  uint32_t id;
  id = lemma("rosa", Noun, 1, F, "rosa f (genitive rosae); first declension"); head(id, nf(Noun, Nom, Sg));
  id = lemma("puella", Noun, 1, F, "puella f (genitive puellae); first declension"); head(id, nf(Noun, Nom, Sg));
  id = lemma("hortus", Noun, 2, M, "hortus m (genitive hortī); second declension"); head(id, nf(Noun, Nom, Sg));
  id = lemma("dōnum", Noun, 2, N, "dōnum n (genitive dōnī); second declension"); head(id, nf(Noun, Nom, Sg));
  id = lemma("ager", Noun, 2, M, "ager m (genitive agrī); second declension"); head(id, nf(Noun, Nom, Sg));
  id = lemma("fīlius", Noun, 2, M, "fīlius m (genitive fīliī); second declension"); head(id, nf(Noun, Nom, Sg));
  id = lemma("laudō", Verb, 1, 0, "laudō (present infinitive laudāre, perfect active laudāvī, supine laudātum); first conjugation");
  head(id, pack(morph::verbForm(1, Sg, Present)));
  id = lemma("moneō", Verb, 2, 0, "moneō (present infinitive monēre, perfect active monuī, supine monitum); second conjugation");
  head(id, pack(morph::verbForm(1, Sg, Present)));
  id = lemma("altus", Adj, 1, 0, "altus (feminine alta, neuter altum, comparative altior, superlative altissimus)");
  head(id, pack(morph::adjForm(Nom, Sg, M, 0)));
  id = lemma("pulcher", Adj, 1, 0, "pulcher (feminine pulchra, neuter pulchrum)");
  head(id, pack(morph::adjForm(Nom, Sg, M, 0)));
  // sum, with a (partial) table
  id = lemma("sum", Verb, 0, 0, "sum (present infinitive esse, perfect active fuī)", lex::HasTable);
  const char* pres[6] = {"sum", "es", "est", "sumus", "estis", "sunt"};
  const char* impf[6] = {"eram", "erās", "erat", "erāmus", "erātis", "erant"};
  const char* fut[6] = {"erō", "eris", "erit", "erimus", "eritis", "erunt"};
  for (int i = 0; i < 6; ++i) {
    const uint8_t p = (uint8_t)(i % 3 + 1), num = i < 3 ? Sg : Pl;
    for (auto [t, forms] : {std::pair<uint8_t, const char**>{Present, pres}, {Imperfect, impf}, {Future, fut}}) {
      const uint32_t pk = pack(morph::verbForm(p, num, t));
      in.lemmas[id].cells.push_back({pk, text::nfc(forms[i])});
      in.analyses.push_back({text::latin_key(forms[i]), id, pk, lex::FromTable, text::nfc(forms[i])});
    }
  }
  in.lemmas[id].cells.push_back({pack(morph::infinitive(Present)), "esse"});
  in.analyses.push_back({"esse", id, pack(morph::infinitive(Present)), lex::FromTable, "esse"});
  id = lemma("nōn", Particle, 0, 0, "nōn"); head(id, 0);
  id = lemma("in", Prep, 0, 0, "in (+ ablative)"); head(id, 0);
  id = lemma("et", Conj, 0, 0, "et"); head(id, 0);
  return in;
}
const Held& mini() {
  static Held h = [] {
    const auto bytes = lexfix::build(miniLexicon());
    const stdfs::path p = tmpDir() / "mini_la.vpl";
    std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), (std::streamsize)bytes.size());
    return openHeld(p);
  }();
  return h;
}

}  // namespace

// ================================================================================================================
TEST_CASE("rules-la: curated data loads; missing file is an error with a hint; malformed lines are warnings") {
  auto r = curated::CuratedData::load(repoRoot() / "data" / "curated");
  REQUIRE_MESSAGE(r.ok(), r.error().message);
  const curated::CuratedData& d = r.value();
  for (const auto& w : d.warnings()) MESSAGE("curated warning: " << w.file << ":" << w.line << " " << w.message);
  CHECK(d.orderRules().size() >= 60);
  REQUIRE(d.valency("amo"));
  CHECK(d.valency("amo")->frames[0].kind == curated::FrameKind::Acc);
  REQUIRE(d.valency("do"));
  CHECK(d.valency("do")->frames[0].kind == curated::FrameKind::DatAcc);
  REQUIRE(d.valency("loquor"));
  CHECK(d.valency("loquor")->frames[1].kind == curated::FrameKind::Prep);
  CHECK(d.valency("loquor")->frames[1].prep == "cum");
  CHECK(d.valency("loquor")->frames[1].prepCase == Abl);
  REQUIRE(d.valency("licet"));
  CHECK(d.valency("licet")->frames[0].kind == curated::FrameKind::Impers);
  CHECK(d.valency("licet")->frames[0].impers == "dat+inf");
  CHECK(d.valency("uolo2") != nullptr);   // homograph marker kept
  CHECK(d.valency("uolo")->frames[0].kind == curated::FrameKind::Inf);
  REQUIRE(d.nameByEnglish("alice"));
  CHECK(d.nameByEnglish("Alice")->latinNom == "Alīcia");
  CHECK(d.nameByEnglish("Alice")->policy == curated::NamePolicy::Decline);
  CHECK(d.nameByEnglish("David")->policy == curated::NamePolicy::Keep);
  CHECK(d.nameByEnglish("David")->declension == 0);
  CHECK(d.nameByLatin("alicia") != nullptr);
  CHECK(d.prepCases("in") == ((1u << Abl) | (1u << Acc)));
  CHECK(d.prepCases("ad") == (1u << Acc));
  CHECK(d.prepCases("ab") == (1u << Abl));
  CHECK(d.prepCases("cum") == (1u << Abl));
  CHECK(d.prepCases("hortus") == 0);
  REQUIRE(d.tier("sum"));
  CHECK(d.tier("sum")->tier == 1);
  CHECK(d.tier("eo", "adv")->note == "thither");
  CHECK(d.emoji("feles") != nullptr);
  CHECK(d.periphrasis("oportet")->periphrasis == "dēbeō");
  CHECK(!d.phrasebook().empty());
  CHECK(!d.contractions().empty());
  CHECK(!d.nonverbal().empty());
  CHECK(d.glossEs("amo") != nullptr);
  // order rules: templates and lists
  CHECK(d.slotTemplate("order.decl") == std::vector<std::string>{"VOC", "CONN", "S", "IO", "O", "OBL", "ADV", "NEG", "V"});
  CHECK(d.slotTemplate("order.exist") == std::vector<std::string>{"CONN", "OBL", "V", "S"});
  CHECK(d.slotTemplate("order.inf") == std::vector<std::string>{"O", "OBL", "INF", "V"});
  const auto conn = d.conditionSet("order.conn");
  CHECK(std::find(conn.begin(), conn.end(), "igitur") != conn.end());
  const auto quant = d.orderingList("order.adj", "quantity");
  CHECK(quant == std::vector<std::string>{"multi", "omnes", "pauci", "nullus", "totus", "alius", "ullus"});   // C15
  const auto cumList = d.orderingList("order.prep", "enclitic");
  CHECK(std::find(cumList.begin(), cumList.end(), "nobiscum") != cumList.end());

  // a copy with damaged lines and then with a file missing
  const stdfs::path dir = tmpDir() / "curated_copy";
  std::error_code ec;
  stdfs::remove_all(dir, ec);
  stdfs::create_directories(dir, ec);
  for (const auto& e : stdfs::directory_iterator(repoRoot() / "data" / "curated"))
    stdfs::copy_file(e.path(), dir / e.path().filename(), stdfs::copy_options::overwrite_existing, ec);
  {
    std::ofstream(dir / "valency_la.tsv", std::ios::app) << "brokenline_without_frame\nfoo\tbogusframe\n";
    std::ofstream(dir / "names_la.tsv", std::ios::app) << "Bob\tBobus\tBobī\tx\t9\tmaybe\n";
    std::ofstream(dir / "order_la.txt", std::ios::app) << "this is not a rule\nRULE broken without arrow\n";
    std::ofstream(dir / "tiers_la.tsv", std::ios::app) << "puella\tpuella\tnoun\t7\tderived\t\n";
    std::ofstream(dir / "preps_en_la.tsv", std::ios::app) << "with\tx\tcum\tinstrumental\t\n";
  }
  auto r2 = curated::CuratedData::load(dir);
  REQUIRE(r2.ok());
  size_t mine = 0;
  for (const auto& w : r2.value().warnings())
    if (w.line > 0) ++mine;
  CHECK(mine >= 7);
  CHECK(r2.value().valency("amo") != nullptr);
  stdfs::remove(dir / "emoji_la.tsv", ec);
  auto r3 = curated::CuratedData::load(dir);
  REQUIRE(!r3.ok());
  CHECK(r3.error().code == ErrorCode::NotFound);
  CHECK(r3.error().hint.find("emoji_la.tsv") != std::string::npos);
  auto r4 = curated::CuratedData::load(tmpDir() / "no_such_dir");
  CHECK(!r4.ok());
}

TEST_CASE("rules-la: frame and case parsing") {
  CHECK(curated::parseFrame("acc+inf").kind == curated::FrameKind::AccInf);
  CHECK(curated::parseFrame("nē").kind == curated::FrameKind::Ne);
  const auto f = curated::parseFrame("prep:ā+abl");
  CHECK(f.kind == curated::FrameKind::Prep);
  CHECK(f.prep == "a");
  CHECK(f.prepCase == Abl);
  CHECK(curated::parseFrame("zzz").kind == curated::FrameKind::Other);
  CHECK(curated::parseCase("loc") == Loc);
  CHECK(curated::parseCase("-") == 0);
}

TEST_CASE("rules-la: principal parts and display helpers") {
  auto p = morph::parsePrincipal("amō", "amō (present infinitive amāre, perfect active amāvī, supine amātum); first conjugation");
  CHECK(p.infinitive == "amāre");
  CHECK(p.perfect == "amāvī");
  CHECK(p.supine == "amātum");
  p = morph::parsePrincipal("eō", "eō (present infinitive īre, perfect active iī or īvī, supine itum); irregular");
  CHECK(p.perfect == "iī");
  p = morph::parsePrincipal("bonus", "bonus (feminine bona, neuter bonum, comparative melior, superlative optimus or optumus, adverb bene)");
  CHECK(p.feminine == "bona");
  CHECK(p.comparative == "melior");
  CHECK(p.superlative == "optimus");
  p = morph::parsePrincipal("lava", "lava f (genitive lavae); first declension");
  CHECK(p.genitive == "lavae");
  p = morph::parsePrincipal("amō", "amō, amāre, amāvī, amātum");
  CHECK(p.infinitive == "amāre");
  CHECK(p.supine == "amātum");
  p = morph::parsePrincipal("bonus", "bonus, bona, bonum");
  CHECK(p.feminine == "bona");
  p = morph::parsePrincipal("puella", "puellae, f.");
  CHECK(p.genitive == "puellae");
  p = morph::parsePrincipal("bastō", "*bastō (present infinitive *bastāre, perfect active *bastāvī, supine *bastātum)");
  CHECK(p.infinitive == "bastāre");
  CHECK(morph::displayForm("egō̆", true) == "ego");
  CHECK(morph::displayForm("mihī̆", true) == "mihi");
  CHECK(morph::displayForm("puellā", true) == "puellā");
  CHECK(morph::displayForm("puellā", false) == "puella");
  CHECK(morph::genderAdmits(MFN, F));
  CHECK(morph::genderAdmits(MF, M));
  CHECK(!morph::genderAdmits(MF, N));
}

TEST_CASE("rules-la: names decline from the table and follow glossary policies") {
  std::string out;
  CHECK(realise::Names::decline("Alīcia", "Alīciae", 1, F, Acc, Sg, out));
  CHECK(out == "Alīciam");
  CHECK(realise::Names::decline("Marcus", "Marcī", 2, M, Voc, Sg, out));
  CHECK(out == "Marce");
  CHECK(realise::Names::decline("Iūlius", "Iūliī", 2, M, Voc, Sg, out));
  CHECK(out == "Iūlī");
  CHECK(realise::Names::decline("Iōannēs", "Iōannis", 3, M, Acc, Sg, out));
  CHECK(out == "Iōannem");
  CHECK(realise::Names::decline("Londinium", "Londiniī", 2, N, Abl, Sg, out));
  CHECK(out == "Londiniō");
  CHECK(realise::Names::decline("Athēnae", "Athēnārum", 1, F, Abl, Pl, out));
  CHECK(out == "Athēnīs");
  CHECK(realise::Names::decline("Thomās", "Thomae", 1, M, Acc, Sg, out));
  CHECK(out == "Thomam");
  CHECK(realise::Names::decline("Alexander", "Alexandrī", 2, M, Gen, Sg, out));
  CHECK(out == "Alexandrī");
  CHECK(realise::Names::decline("Dāvīd", "Dāvīd", 0, M, Dat, Sg, out));
  CHECK(out == "Dāvīd");
  realise::Names names(cur());
  auto f = names.form("Alice", Dat, Sg, nullptr);
  CHECK(f.form == "Alīciae");
  CHECK(!f.kept);
  f = names.form("David", Acc, Sg, nullptr);
  CHECK(f.form == "Dāvīd");
  f = names.form("Zork", Acc, Sg, nullptr);
  CHECK(f.form == "Zork");
  CHECK(f.guessed);
  std::vector<rules::GlossaryEntry> gl = {{"Zork", "decline", "Zorcus", "m", 2}, {"Alice", "keep", "", "f", 0}};
  f = names.form("Zork", Acc, Sg, &gl);
  CHECK(f.form == "Zorcum");
  f = names.form("Alice", Acc, Sg, &gl);
  CHECK(f.form == "Alice");
  CHECK(f.kept);
}

TEST_CASE("rules-la: fixture lexicon - analyse, enclitics, capitals, generate") {
  REQUIRE(fixture().ok);
  const lex::Lexicon& lx = fixture().lx;
  morph::Token t;
  morph::analyseLatin(lx, "puellam", t);
  CHECK(!t.unknown);
  CHECK(!t.analyses.empty());
  morph::analyseLatin(lx, "puellamque", t);
  CHECK(t.enclitic);
  CHECK(t.encliticText == "que");
  CHECK(t.key == "puellam");
  morph::analyseLatin(lx, "amāsne", t);
  CHECK(t.enclitic);
  CHECK(t.encliticText == "ne");
  morph::analyseLatin(lx, "Puella", t);
  CHECK(t.capitalised);
  CHECK(!t.unknown);
  morph::analyseLatin(lx, "puellave", t);
  CHECK(t.enclitic);
  CHECK(t.encliticText == "ve");
  morph::analyseLatin(lx, "xyzzy", t);
  CHECK(t.unknown);
  CHECK(!t.nameGuess);
  morph::analyseLatin(lx, "Iūlius", t);
  CHECK(t.unknown);
  CHECK(t.nameGuess);
  const uint32_t puella = morph::findLemma(lx, "puella", Noun);
  const uint32_t amo = morph::findLemma(lx, "amō", Verb);
  const uint32_t bonus = morph::findLemma(lx, "bonus", Adj);
  REQUIRE(puella != kNone);
  REQUIRE(amo != kNone);
  REQUIRE(bonus != kNone);
  std::string out;
  CHECK(morph::generate(lx, puella, morph::nounForm(Acc, Sg), out, true));
  CHECK(out == "puellam");
  CHECK(morph::generate(lx, puella, morph::nounForm(Abl, Sg), out, false));
  CHECK(out == "puella");
  CHECK(morph::generate(lx, amo, morph::verbForm(3, Sg, Present), out, true));
  CHECK(out == "amat");
  CHECK(morph::generate(lx, bonus, morph::adjForm(Nom, Pl, F), out, true));
  CHECK(out == "bonae");
  CHECK(!morph::generate(lx, puella, morph::verbForm(3, Sg, Present), out, true));   // never invents a cell
}

TEST_CASE("rules-la: mini lexicon - paradigm fallback generates and analyses (fromRule)") {
  REQUIRE_MESSAGE(mini().ok, mini().why);
  const lex::Lexicon& lx = mini().lx;
  struct G { const char* lemma; uint8_t pos; const char* feats; const char* form; };
  const G gens[] = {
      {"rosa", Noun, "acc pl", "rosās"},          {"rosa", Noun, "gen pl", "rosārum"},
      {"hortus", Noun, "voc sg", "horte"},        {"hortus", Noun, "abl pl", "hortīs"},
      {"fīlius", Noun, "voc sg", "fīlī"},         {"ager", Noun, "gen sg", "agrī"},
      {"ager", Noun, "nom sg", "ager"},           {"dōnum", Noun, "nom pl", "dōna"},
      {"laudō", Verb, "3 sg pres ind act", "laudat"}, {"laudō", Verb, "3 pl impf ind act", "laudābant"},
      {"laudō", Verb, "1 sg fut ind act", "laudābō"}, {"laudō", Verb, "3 sg perf ind act", "laudāvit"},
      {"laudō", Verb, "3 pl plup ind act", "laudāverant"}, {"laudō", Verb, "3 sg pres subj act", "laudet"},
      {"laudō", Verb, "3 sg impf subj act", "laudāret"}, {"laudō", Verb, "2 sg pres imp act", "laudā"},
      {"laudō", Verb, "2 pl pres imp act", "laudāte"}, {"laudō", Verb, "pres inf act", "laudāre"},
      {"laudō", Verb, "3 sg pres ind pass", "laudātur"}, {"laudō", Verb, "3 sg perf ind pass f", "laudāta est"},
      {"laudō", Verb, "perf inf act", "laudāvisse"},
      {"moneō", Verb, "3 sg pres ind act", "monet"},   {"moneō", Verb, "3 sg impf ind act", "monēbat"},
      {"moneō", Verb, "3 sg pres subj act", "moneat"}, {"moneō", Verb, "3 sg perf ind act", "monuit"},
      {"moneō", Verb, "pres inf pass", "monērī"},
      {"altus", Adj, "nom sg f", "alta"},             {"altus", Adj, "acc pl n", "alta"},
      {"altus", Adj, "nom sg m comp", "altior"},      {"altus", Adj, "acc sg n comp", "altius"},
      {"altus", Adj, "gen sg f comp", "altiōris"},    {"altus", Adj, "nom sg f sup", "altissima"},
      {"pulcher", Adj, "nom sg f", "pulchra"},        {"pulcher", Adj, "acc sg m", "pulchrum"},
      {"pulcher", Adj, "nom sg m sup", "pulcherrimus"},
  };
  for (const G& g : gens) {
    const uint32_t id = morph::findLemma(lx, g.lemma, g.pos);
    REQUIRE_MESSAGE(id != kNone, g.lemma);
    Features f = parseFeats(g.feats);
    f.pos = g.pos;
    std::string out;
    morph::GenInfo gi;
    CHECK_MESSAGE(morph::generate(lx, id, f, out, true, &gi), g.lemma << " " << g.feats);
    CHECK_MESSAGE(out == g.form, g.lemma << " " << g.feats << ": " << out << " != " << g.form);
    CHECK_MESSAGE(gi.fromRule, g.lemma << " " << g.feats);
  }
  morph::Token t;
  morph::analyseLatin(lx, "rosās", t);
  CHECK(t.fromRule);
  CHECK(!t.unknown);
  REQUIRE(!t.ruleAnalyses.empty());
  CHECK(unpack(t.ruleAnalyses[0].packed).case_ == Acc);
  morph::analyseLatin(lx, "laudāvērunt", t);
  CHECK(t.fromRule);
  morph::analyseLatin(lx, "puellīs", t);
  CHECK(t.fromRule);
  morph::analyseLatin(lx, "xyzzy", t);
  CHECK(t.unknown);

  // realisation and checking end to end through the fallback
  g_lx = &lx;
  LatinRealiser R(lx, cur());
  RealiseOptions o;
  LaSentence s = R.realise(CB().v("laudō").s(n("puella")).o(with(n("rosa", Pl), A("pulcher"))).c, o);
  CHECK(s.text == "Puella rosās pulchrās laudat.");
  CHECK(std::find(s.flags.begin(), s.flags.end(), "from-rule") != s.flags.end());
  CHECK(std::all_of(s.tokens.begin(), s.tokens.end(), [](const rules::TokenView& tv) { return tv.fromRule; }));
  check::LatinChecker ck(lx, cur());
  auto rep = ck.check(s.text, {});
  CHECK(rep.ok("A1"));
  CHECK(rep.ok("A3"));
  CHECK(rep.ok("A4"));
  CHECK(rep.fromRule);
  rep = ck.check("Puella rosās pulchram laudat.", {});
  CHECK(!rep.ok("A3"));
  s = R.realise(CB().v("laudō", Perfect).passive().s(n("puella")).c, o);
  CHECK(s.text == "Puella laudāta est.");
  s = R.realise(CB().v("sum").s(n("hortus")).padj(A("altus")).neg().c, o);
  CHECK(s.text == "Hortus altus nōn est.");
  s = R.realise(CB().v("moneō").s(n("fīlius", Pl)).obl("in", n("ager")).c, o);
  CHECK(s.text == "Fīliī in agrō monent.");
  o.macrons = false;
  s = R.realise(CB().v("moneō").s(n("fīlius", Pl)).obl("in", n("ager")).c, o);
  CHECK(s.text == "Filii in agro monent.");
  CHECK(s.tokens[0].display == "Fīliī");
  CHECK(s.tokens[0].text == "Filii");
}

TEST_CASE("rules-la: real lexicon - analyse 61 forms (tests/fixtures/rules/analyse_la.tsv)") {
  NEED_REAL();
  const lex::Lexicon& lx = real().lx;
  const auto rows = readTsv(rulesFixture("analyse_la.tsv"));
  REQUIRE(rows.size() >= 60);
  size_t ok = 0;
  for (const auto& r : rows) {
    REQUIRE(r.size() >= 4);
    const std::string flags = r.size() > 4 ? r[4] : "";
    morph::Token t;
    morph::analyseLatin(lx, r[0], t);
    bool good = true;
    if (flags.find("unknown") != std::string::npos) good = good && t.unknown;
    if (flags.find("name") != std::string::npos) good = good && t.nameGuess;
    if (flags.find("capital") != std::string::npos) good = good && t.capitalised;
    if (flags.find("rule") != std::string::npos) good = good && t.fromRule;
    const size_t e = flags.find("enclitic=");
    if (e != std::string::npos) good = good && t.enclitic && t.encliticText == flags.substr(e + 9, flags.find(',', e) - e - 9);
    if (r[1] != "-") {
      Features want = parseFeats(r[3]);
      const uint8_t pos = posOf(r[2]);
      bool found = false;
      auto test = [&](uint32_t lemma, uint32_t packed) {
        const lex::Lemma l = lx.lemma(lemma);
        if (morph::displayForm(l.head, true) != morph::displayForm(r[1], true)) return;
        if (pos && l.pos != pos) return;
        if (featsMatch(unpack(packed), want)) found = true;
      };
      for (const lex::Analysis& a : t.analyses) test(a.lemma, lx.feature(a.feat));
      for (const morph::RuleAnalysis& a : t.ruleAnalyses) test(a.lemma, a.packed);
      good = good && found;
    }
    CHECK_MESSAGE(good, "analyse " << r[0] << " expected " << r[1] << " " << r[3] << " [" << flags << "]");
    ok += good;
  }
  MESSAGE("analysed " << ok << " / " << rows.size() << " forms as expected");
}

TEST_CASE("rules-la: real lexicon - generate 114 cells (tests/fixtures/rules/generate_la.tsv)") {
  NEED_REAL();
  const lex::Lexicon& lx = real().lx;
  const auto rows = readTsv(rulesFixture("generate_la.tsv"));
  REQUIRE(rows.size() >= 100);
  size_t ok = 0;
  for (const auto& r : rows) {
    REQUIRE(r.size() >= 4);
    const uint8_t pos = posOf(r[1]);
    const uint32_t id = morph::findLemma(lx, r[0], pos);
    REQUIRE_MESSAGE(id != kNone, r[0]);
    Features f = parseFeats(r[2]);
    f.pos = pos;
    std::string out;
    morph::GenInfo gi;
    const bool g = morph::generate(lx, id, f, out, true, &gi);
    const bool rule = r.size() > 4 && r[4] == "rule";
    const bool good = g && out == r[3] && gi.fromRule == rule;
    CHECK_MESSAGE(good, r[0] << " " << r[2] << ": got '" << out << "' fromRule " << gi.fromRule << ", expected '" << r[3] << "'");
    ok += good;
  }
  MESSAGE("generated " << ok << " / " << rows.size() << " cells as expected");
}

TEST_CASE("rules-la: real greek.vpl / fixture - analyseGreek exact and accent-insensitive") {
  {
    const Held g = openHeld(stdfs::path(VP_FIXTURES_DIR) / "lex" / "greek.vpl");
    REQUIRE(g.ok);
    morph::Token t;
    morph::analyseGreek(g.lx, "ἄνθρωπος", t);
    CHECK(!t.unknown);
    CHECK(!t.accentInsensitive);
    morph::analyseGreek(g.lx, "ανθρωπος", t);
    CHECK(!t.unknown);
    CHECK(t.accentInsensitive);
    morph::analyseGreek(g.lx, "Ἄνθρωπος", t);
    CHECK(!t.unknown);
    CHECK(t.capitalised);
  }
  const char* env = std::getenv("VP_GREEK_VPL");
  const Held g = openHeld(env && *env ? stdfs::path(env) : repoRoot() / "data" / "work" / "greek.vpl");
  if (!g.ok) { MESSAGE("real greek.vpl not available; skipped"); return; }
  morph::Token t;
  morph::analyseGreek(g.lx, "λόγος", t);
  CHECK(!t.unknown);
  morph::analyseGreek(g.lx, "λογος", t);
  CHECK(!t.unknown);
  CHECK(t.accentInsensitive);
  morph::analyseGreek(g.lx, "ανθρωπου", t);
  CHECK(!t.unknown);
  morph::analyseGreek(g.lx, "Ἀθῆναι", t);
  CHECK(!t.unknown);
}

namespace {

struct Row { LaClause c; const char* expect; };

std::vector<Row> realisationTable() {
  using R = Row;
  std::vector<Row> t;
  auto add = [&](const CB& b, const char* e) { t.push_back(R{b.c, e}); };
  // style of tests/regression/expected/own_dialogue.la.gold.txt
  add(CB(ClauseType::Wh).wh("quō").v("eō").pers(2), "Quō īs?");
  add(CB().v("veniō").pers(1).adv("sērō").punct("!"), "Sērō veniō!");
  add(CB(ClauseType::Imp).v("aperiō").o(n("iānua")).polite("quaesō"), "Aperī iānuam, quaesō.");
  add(CB().v("sum").s(n("iānua")).padj(A("parvus", {"nimis"})), "Iānua nimis parva est.");
  add(CB(ClauseType::Imp).v("timeō").neg(), "Nōlī timēre.");
  add(CB(ClauseType::Yn).v("rīdeō").modal("possum").s(n("fēlēs", Pl)), "Possuntne fēlēs rīdēre?");
  add(CB().v("sum").pred(withGen(cap(n("rēgīna")), cap(n("cor", Pl)))), "Rēgīna Cordium est.");
  add(CB(ClauseType::Imp).v("dō").io(pr(1)).o(n("pila")), "Dā mihi pilam.");
  add(CB().v("sciō").s(sb("nēmō", Sg, 0, Pron)), "Nēmō scit.");
  add(CB().v("habitō", Imperfect).s(n("fēlēs")).obl("in", n("hortus"), true), "In hortō fēlēs habitābat.");
  add(CB().v("currō").pers(1, Pl).mood(Subjunctive).sub(SubRel::Time, CB().v("veniō").mood(Subjunctive), false, "antequam"),
      "Currāmus antequam veniat.");
  add(CB(ClauseType::Wh).v("habeō").pers(2).o([] { LaNP x = n("frāter", Pl); x.interrogative = L("quot"); return x; }()),
      "Quot frātrēs habēs?");
  add(CB().v("habeō").pers(1).o(et(withNum(n("frāter", Pl), "duo"), withNum(n("soror"), "ūnus"))),
      "Duōs frātrēs et ūnam sorōrem habeō.");
  add(CB().v("sum").s(withPoss(n("pater"), "meus")).pred(n("agricola")), "Pater meus agricola est.");
  add(CB().v("habitō").pers(1, Pl).obl("in", with(n("casa"), A("parvus"))).obl("prope", n("flūmen")),
      "In casā parvā prope flūmen habitāmus.");
  add(CB(ClauseType::Wh).wh("quis", Role::Predicate).v("sum").pers(2), "Quis es?");
  add(CB().v("nesciō").pers(1), "Nesciō.");
  add(CB(ClauseType::Yn).v("videō", Perfect).pers(2).o(withPoss(n("fēlēs"), "meus")), "Vīdistīne fēlem meam?");
  add(CB(ClauseType::Imp).v("redeō").adv("hūc").punct("!"), "Redī hūc!");
  add(CB().v("trānseō").modal("possum").pers(1).neg(), "Trānsīre nōn possum.");
  add(CB(ClauseType::Imp).v("bibō").o(sb("hic", Sg, N, Pron)), "Bibe hoc.");
  add(CB().v("sum").s(pr(1, Sg, F)).padj(A("magnus", {"nimis"})).adv("nunc").punct("!"), "Nunc nimis magna sum!");
  add(CB(ClauseType::Wh).wh("quō").v("abeō", Perfect).s(sb("omnis", Pl, M, Adj)), "Quō omnēs abiērunt?");
  add(CB(ClauseType::Excl).quam().s(with(n("hortus"), A("mīrus"))), "Quam mīrus hortus!");
  add(CB().v("loquor").modal("possum").s(n("flōs", Pl)).neg(), "Flōrēs loquī nōn possunt.");
  add(CB().v("loquor").modal("possum").pers(1, Pl).adv("certē"), "Certē loquī possumus.");
  add(CB(ClauseType::Imp).v("canō").io(pr(1, Pl)).o(n("carmen")), "Cane nōbīs carmen.");
  add(CB().v("sciō").pers(1).o(with(n("carmen"), A("nūllus"))).neg(), "Nūllum carmen sciō.");
  add(CB(ClauseType::Imp).v("abeō").conn("igitur"), "Abī igitur.");
  add(CB().v("veniō").s(n("rēgīna")).punct("!"), "Rēgīna venit!");
  add(CB(ClauseType::Imp).v("inclīnō").voc(sb("omnis", Pl, M, Adj)).o(pr(2, Pl, 0, true)).punct("!"),
      "Omnēs, inclīnāte vōs!");
  add(CB(ClauseType::Yn).v("lūdō").pers(2).obl(nullptr, n("charta", Pl), false, Abl), "Lūdisne chartīs?");
  add(CB().v("lūdō", Perfect).pers(1).adv("numquam").adv("anteā").neg(), "Numquam anteā lūsī.");
  add(CB(ClauseType::Wh).wh("ubi").v("sum").s(n("pila")).exist(), "Ubi est pila?");
  add(CB().v("currō").s(n("pila", Pl)).neg(), "Pilae nōn currunt.");
  add(CB().v("currō").s(sb("hic", Sg, F, Pron)), "Haec currit.");
  add(CB(ClauseType::Imp).v("sūmō").o(n("placenta")).conn("igitur"), "Sūme igitur placentam.");
  add(CB().v("nōlō").pers(1).o(n("placenta")), "Placentam nōlō.");
  add(CB(ClauseType::Wh).wh("quis", Role::Subject).v("frangō", Perfect).o(n("hōrologium")), "Quis hōrologium frēgit?");
  add(CB(ClauseType::Imp).v("sedeō").sub(SubRel::Coord, CB(ClauseType::Imp).v("nārrō").io(pr(1)).o(n("fābula"))),
      "Sedē et nārrā mihi fābulam.");   // macron_overrides.tsv in the Macrons primitive (decision 5)
  add(CB().v("sum", Imperfect).s(with(n("puella"), A("parvus"))).exist().adv("ōlim", AdvPos::Front),
      "Ōlim erat puella parva.");
  add(CB().v("cadō", Perfect).obl("in", with(n("fovea"), A("altus")), false, Acc), "In foveam altam cecidit.");
  add(CB().v("sum", Imperfect).s(n("hortus")).obl("post", n("iānua"), true), "Post iānuam hortus erat.");
  add(CB().v("rīdeō", Imperfect).modal("possum").s(n("fēlēs")), "Fēlēs rīdēre poterat.");
  add(CB().v("possum").s(sb("hic", Sg, F, Pron)), "Haec potest.");
  add(CB().v("sum").s(sb("omnis", Pl, M, Adj)).padj(A("īnsānus")).adv("hīc"), "Omnēs hīc īnsānī sunt.");
  add(CB(ClauseType::Wh).wh("cūr").v("pingō").pers(2, Pl).o(n("rosa", Pl)), "Cūr rosās pingitis?");
  add(CB().v("serō", Perfect).pers(1, Pl).o(with(n("rosa", Pl), A("albus"))).obl(nullptr, n("error"), false, Abl),
      "Rosās albās errōre sēvimus.");
  add(CB().v("volō", Perfect).s(n("rēgīna")).o(sb("ruber", Pl, F, Adj)), "Rēgīna rubrās voluit.");
  add(CB().v("abscīdō", Future).o(withPoss(n("caput", Pl), "noster"))
          .sub(SubRel::Condition, CB().v("videō").o(with(n("rosa", Pl), A("albus"))), true),
      "Sī rosās albās videt, capita nostra abscīdet.");
  add(CB(ClauseType::Imp).v("adiuvō").num(Pl).o(pr(1, Pl)).polite("quaesō").punct("!"), "Adiuvāte nōs, quaesō!");
  add(CB(ClauseType::Imp).v("dō").io(pr(1)).o(with(n("color"), A("ruber"))), "Dā mihi colōrem rubrum.");
  add(CB().v("fīō").modal("possum").s(sb("hic", Sg, N, Pron)).neg(), "Hoc fierī nōn potest.");
  add(CB().v("doceō").s(withPoss(n("māter"), "meus")).o(n("puer", Pl)), "Māter mea puerōs docet.");
  add(CB().v("sum", Imperfect).s(n("magister")).padj(A("īrātus")).adv("hodiē"), "Hodiē magister īrātus erat.");
  add(CB().v("faciō", Pluperfect).s(sb("nēmō", Sg, 0, Pron)).o(n("pēnsum")), "Nēmō pēnsum fēcerat.");
  add(CB().v("scrībō", Future).pers(1, Pl).o(n("epistula")).adv("crās", AdvPos::Front).adv("Latīnē"),
      "Crās epistulam Latīnē scrībēmus.");
  add(CB(ClauseType::Imp).v("scrībō").o(withPoss(n("nōmen"), "tuus")).adv("prīmum", AdvPos::Front),
      "Prīmum nōmen tuum scrībe.");
  add(CB().v("putō", Imperfect).pers(1)
          .sub(SubRel::AccInf, CB().v("sum").pred(withGen(n("diēs"), [] { LaNP x; x.head = L("Lūna", Name); return x; }()))),
      "Putābam diem Lūnae esse.");
  add(CB().v("errō").pers(1).adv("numquam").neg(), "Numquam errō.");
  add(CB().v("errō").s(sb("omnis", Pl, M, Adj)).adv("aliquandō"), "Omnēs aliquandō errant.");
  add(CB(ClauseType::Imp).v("spectō").o(n("caelum")).punct("!"), "Spectā caelum!");
  add(CB(ClauseType::Imp).v("claudō").o(n("fenestra")), "Claude fenestram.");
  add(CB(ClauseType::Imp).v("accendō").o(n("candēla")), "Accende candēlam.");
  add(CB().v("sum").padj(A("obscūrus")).pg(N).adv("hīc", AdvPos::Front), "Hīc obscūrum est.");
  add(CB(ClauseType::Yn).v("timeō").pers(2).o(n("tenebrae", Pl)), "Timēsne tenebrās?");
  add(CB(ClauseType::Frag).adv("minimē"), "Minimē.");
  add(CB(ClauseType::Wh).wh("cūr").v("tremō").pers(2).conn("igitur"), "Cūr igitur tremis?");
  add(CB().v("frīgeō").pers(1), "Frīgeō.");
  add(CB(ClauseType::Imp).v("sūmō").o(withPoss(n("pallium"), "meus")), "Sūme pallium meum.");
  add(CB().v("sum").s(pr(2, Sg, M)).padj(A("benignus", {"valdē"})), "Valdē benignus es.");
  add(CB().v("videō", Future).pers(1).o(pr(2)).adv("crās", AdvPos::Front), "Crās tē vidēbō.");
  // further constructions of order_la.txt
  add(CB(ClauseType::Imp).v("veniō").obl("cum", pr(1)).punct("!"), "Venī mēcum!");
  add(CB().v("ambulō").pers(1, Pl).obl("cum", pr(2, Pl)), "Vōbīscum ambulāmus.");
  add(CB(ClauseType::Yn).bias(YnBias::ExpectYes).v("videō", Perfect).pers(2).o(n("fēlēs")), "Nōnne fēlem vīdistī?");
  add(CB(ClauseType::Yn).bias(YnBias::ExpectNo).v("natō").s(n("fēlēs", Pl)), "Num fēlēs natant?");
  add(CB().v("ambulō").s(withRel(n("puella"), CB().rel(Role::Subject).v("amō").o(n("rosa", Pl)))).obl("in", n("hortus")),
      "Puella quae rosās amat in hortō ambulat.");
  add(CB().v("habeō").pers(1).o(withRel(n("liber"), CB().rel(Role::Object).v("legō").pers(2))), "Librum quem legis habeō.");
  add(CB().v("vocō", Perfect).passive().s(n("puella")).obl("ab", n("rēgīna")), "Puella ā rēgīnā vocāta est.");
  add(CB().v("vocō", Perfect).passive().s(n("puer")).obl("ab", n("agricola")), "Puer ab agricolā vocātus est.");
  add(CB().v("videō").s(nm("Mark")).o(nm("Alice")), "Marcus Alīciam videt.");
  add(CB().v("ambulō").s(nm("Alice")).obl("cum", nm("Mark")), "Alīcia cum Marcō ambulat.");
  add(CB().v("sum").s(pr(1, Sg, F)).pred(nm("Alice")), "Ego Alīcia sum.");
  add(CB().v("currō").s(et(with(n("puer"), A("parvus")), with(n("puella"), A("parvus")))).obl("in", n("hortus")),
      "Puer parvus et puella parva in hortō currunt.");
  add(CB().v("sciō").pers(1).sub(SubRel::AccInf, CB().v("amō").s(n("puella")).o(n("rosa", Pl))),
      "Sciō puellam rosās amāre.");
  add(CB().v("pāreō").modal("dēbeō").pers(1, Pl).io(n("magister")), "Magistrō pārēre dēbēmus.");
  add(CB().v("ūtor").s(n("mīles")).o(n("gladius")), "Mīles gladiō ūtitur.");
  add(CB().v("placeō").s(n("rosa", Pl)).io(pr(1)), "Rosae mihi placent.");
  add(CB(ClauseType::Excl).exclO().s(with(pr(1, Sg, F), A("miser"))), "Ō mē miseram!");
  add(CB().v("veniō").sub(SubRel::Purpose, CB().v("videō").o(n("puella"))), "Venit ut puellam videat.");
  add(CB().v("veniō").sub(SubRel::Purpose, CB().v("videō").o(n("puella")).neg()), "Venit nē puellam videat.");
  add(CB(ClauseType::Imp).v("clāmō").num(Pl).neg().punct("!"), "Nōlīte clāmāre!");
  add(CB(ClauseType::Imp).v("carpō").o(n("rosa", Pl)).obl("in", n("hortus")).neg(), "Rosās in hortō nōlī carpere.");
  add(CB().v("dō").s(n("puer")).io(n("puella")).o(n("rosa")).adv("hodiē"), "Hodiē puer puellae rosam dat.");
  add(CB().v("habitō").s(n("fēlēs")).obl("in", n("hortus")).conn("autem"), "Fēlēs autem in hortō habitat.");
  add(CB().v("habitō").s(n("fēlēs")).obl("in", n("hortus"), true).conn("autem"), "In hortō autem fēlēs habitat.");
  add(CB().v("legō").s(n("puer")).o(with(n("liber"), A("magnus"))).adv("celeriter"), "Puer librum magnum celeriter legit.");
  add(CB().v("videō").s(withDet(n("puella"), realise::Det::Hic)).o(withDet(n("hortus"), realise::Det::Ille)),
      "Haec puella illum hortum videt.");
  add(CB().v("sum").s(withGen(n("liber"), n("puer"))).padj(A("novus")), "Liber puerī novus est.");
  add(CB().v("veniō", Perfect).s(n("puella")).sub(SubRel::Cause, CB().v("amō").o(n("hortus"))), "Puella vēnit quia hortum amat.");
  add(CB().v("dormiō", Imperfect).s(n("puer")).sub(SubRel::Time, CB().v("veniō", Perfect).s(n("māter")), true),
      "Cum māter vēnit, puer dormiēbat.");
  add(CB().v("sum").s(n("puella")).padj(A("fessus")).padj(A("laetus")), "Puella fessa et laeta est.");
  add(CB().v("amō").s(pr(1, Sg, M, false)).o(pr(2)).neg(), "Tē nōn amō.");
  add(CB().v("amō").s(emph(pr(1))).o(pr(2)), "Ego tē amō.");
  add(CB().v("sequor", Perfect).s(n("puella")).o(n("puer")), "Puella puerum secūta est.");
  add(CB().v("vīvō", Perfect).s(n("rēx")).adv("diū"), "Rēx diū vīxit.");
  return t;
}

}  // namespace

TEST_CASE("rules-la: real lexicon - realisation table matches the expected Latin and passes the checker") {
  NEED_REAL();
  g_lx = &real().lx;
  const std::vector<Row> table = realisationTable();
  REQUIRE(table.size() >= 60);
  LatinRealiser R(real().lx, cur());
  check::LatinChecker ck(real().lx, cur());
  RealiseOptions o;
  size_t same = 0, clean = 0;
  LaSentence s;
  for (const Row& r : table) {
    R.realise(r.c, o, s);
    CHECK_MESSAGE(s.text == std::string(r.expect), "got '" << s.text << "' expected '" << r.expect << "'");
    same += s.text == r.expect;
    // token offsets cover the text exactly
    for (const auto& tv : s.tokens) {
      REQUIRE(tv.end <= (int)s.text.size());
      CHECK(s.text.substr((size_t)tv.start, (size_t)(tv.end - tv.start)) == tv.text);
    }
    const check::Report rep = ck.check(s.text, {});
    const std::string f = failures(rep, {"A1", "A3", "A4"});
    CHECK_MESSAGE(f.empty(), s.text << " -> " << f);
    clean += f.empty();
  }
  MESSAGE("realisation table: " << same << " / " << table.size() << " exact, " << clean << " clean under A1/A3/A4");
}

TEST_CASE("rules-la: real lexicon - token views, reasons, emoji, macrons off, names flags") {
  NEED_REAL();
  g_lx = &real().lx;
  LatinRealiser R(real().lx, cur());
  RealiseOptions o;
  LaSentence s = R.realise(CB().v("habitō", Imperfect).s(n("fēlēs")).obl("in", n("hortus"), true).c, o);
  REQUIRE(s.tokens.size() == 4);
  CHECK(s.tokens[2].emoji == "🐈");
  CHECK(s.tokens[2].features.case_ == "nominative");
  CHECK(s.tokens[3].features.tense == "imperfect");
  CHECK(s.tokens[3].features.person == "third");
  CHECK(s.tokens[0].hasLemma);
  CHECK(!s.reasons.empty());
  o.emojiInText = true;
  s = R.realise(CB().v("habitō", Imperfect).s(n("fēlēs")).obl("in", n("hortus"), true).c, o);
  CHECK(s.text == "In hortō 🏡 fēlēs 🐈 habitābat.");
  o.emojiInText = false;
  o.emoji = false;
  o.macrons = false;
  s = R.realise(CB().v("habitō", Imperfect).s(n("fēlēs")).obl("in", n("hortus"), true).c, o);
  CHECK(s.text == "In horto feles habitabat.");
  CHECK(s.tokens[2].emoji.empty());
  CHECK(s.tokens[2].display == "fēlēs");
  o = RealiseOptions{};
  s = R.realise(CB().v("videō").s(nm("Zork")).o(n("rosa")).c, o);
  CHECK(s.text == "Zork rosam videt.");
  CHECK(std::find(s.flags.begin(), s.flags.end(), "name-guessed") != s.flags.end());
  std::vector<rules::GlossaryEntry> gl = {{"Zork", "decline", "Zorcus", "m", 2}};
  o.glossary = &gl;
  s = R.realise(CB().v("videō").s(n("puella")).o(nm("Zork")).c, o);
  CHECK(s.text == "Puella Zorcum videt.");
  o.glossary = nullptr;
  // enclitic -que only in flexible mode
  s = R.realise(CB().v("amō").s(et(n("pater"), n("māter"))).o(pr(1)).c, o);
  CHECK(s.text == "Pater et māter mē amant.");
  LaNP pm = et(n("pater"), n("māter"));
  pm.coordQue = true;
  o.fidelity = 3;
  s = R.realise(CB().v("amō").s(pm).o(pr(1)).c, o);
  CHECK(s.text == "Pater māterque mē amant.");
  o.fidelity = 2;
  s = R.realise(CB().v("amō").s(pm).o(pr(1)).c, o);
  CHECK(s.text == "Pater et māter mē amant.");
  // unknown source word kept in brackets
  LaNP unk;
  unk.literal = "smartphone";
  s = R.realise(CB().v("habeō").pers(1).o(unk).c, o);
  CHECK(s.text == "[smartphone] habeō.");
  CHECK(s.tokens[0].unknown);
  CHECK(std::find(s.flags.begin(), s.flags.end(), "unknown") != s.flags.end());
  // a paradigm-fallback noun is flagged
  s = R.realise(CB().v("videō").pers(1).o(n("lava")).c, o);
  CHECK(s.text == "Lavam videō.");
  CHECK(s.tokens[0].fromRule);
  CHECK(std::find(s.flags.begin(), s.flags.end(), "from-rule") != s.flags.end());
}

TEST_CASE("rules-la: real lexicon - checker table (tests/fixtures/rules/check_la.tsv)") {
  NEED_REAL();
  check::LatinChecker ck(real().lx, cur());
  const auto rows = readTsv(rulesFixture("check_la.tsv"));
  REQUIRE(rows.size() >= 30);
  for (const auto& r : rows) {
    REQUIRE(r.size() >= 3);
    check::Options o;
    o.tierCeiling = (uint8_t)std::atoi(r[2].c_str());
    const check::Report rep = ck.check(r[0], o);
    std::string failed;
    for (const auto& c : rep.checks)
      if (!c.ok) failed += (failed.empty() ? "" : ",") + c.id;
    if (failed.empty()) failed = "-";
    std::string details;
    for (const auto& c : rep.checks)
      if (!c.detail.empty()) details += c.id + ": " + c.detail + "; ";
    CHECK_MESSAGE(failed == r[1], r[0] << " -> failed " << failed << " expected " << r[1] << " | " << details);
  }
  // hints: a token hinted as a name passes A1, a fromRule hint marks the report
  std::vector<check::TokenHint> hints = {{0, 6, kNone, false, true}};
  check::Options o;
  o.hints = &hints;
  CHECK(ck.check("Qwerty rosam amat.", o).ok("A1"));
  CHECK(!ck.check("Qwerty rosam amat.", {}).ok("A1"));
}

namespace {

// Template generator for the 2,000-sentence test (DESIGN 10.3): subjects x objects x verbs x tenses x types.
struct GenItem { LaClause c; int kind; };
std::vector<GenItem> generated(size_t count) {
  struct NounSpec { const char* head; };
  static const char* subjects[] = {"puella", "puer", "fēmina", "agricola", "servus", "dominus", "rēgīna", "magister",
                                   "nauta", "fīlia", "amīcus", "mīles", "māter", "pater", "rēx"};
  static const char* subjAdj[] = {nullptr, "parvus", "magnus", "bonus", "laetus", "īrātus", "fessus", "pulcher", nullptr};
  static const char* accVerbs[] = {"amō", "videō", "habeō", "timeō", "laudō", "portō", "spectō", "vocō", "quaerō", "audiō"};
  static const char* objects[] = {"rosa", "liber", "epistula", "pila", "equus", "templum", "dōnum", "canis", "oppidum",
                                  "mālum", "fenestra", "gladius"};
  static const char* objAdj[] = {nullptr, "albus", "novus", "magnus", "parvus", "pulcher", "longus", nullptr};
  static const char* datVerbs[] = {"pāreō", "crēdō", "faveō", "noceō"};
  static const char* datNouns[] = {"magister", "rēgīna", "dominus", "amīcus", "puella", "rēx"};
  static const char* intrVerbs[] = {"veniō", "currō", "ambulō", "sedeō", "habitō"};
  static const char* places[] = {"hortus", "vīlla", "casa", "silva", "oppidum", "templum"};
  static const char* predAdjs[] = {"laetus", "fessus", "parvus", "magnus", "bonus", "īrātus", "pulcher"};
  static const uint8_t tenses[] = {Present, Imperfect, Future, Perfect, Pluperfect};
  std::vector<GenItem> out;
  uint32_t x = 12345;
  auto rnd = [&](uint32_t m) { x = x * 1103515245u + 12345u; return (x >> 8) % m; };
  auto subjNP = [&](bool allowPron) {
    const uint32_t k = rnd(10);
    if (allowPron && k == 0) return pr((uint8_t)(1 + rnd(2)), rnd(2) ? Pl : Sg, rnd(2) ? F : M);
    LaNP s = n(subjects[rnd(sizeof subjects / sizeof *subjects)], rnd(3) == 0 ? Pl : Sg);
    if (const char* a = subjAdj[rnd(sizeof subjAdj / sizeof *subjAdj)]) s = with(s, A(a));
    return s;
  };
  for (size_t i = 0; i < count; ++i) {
    CB b;
    const uint32_t kind = rnd(6);
    const uint8_t tense = tenses[rnd(5)];
    if (kind <= 1) {   // transitive (accusative)
      b.v(accVerbs[rnd(10)], tense).s(subjNP(true));
      LaNP o = n(objects[rnd(sizeof objects / sizeof *objects)], rnd(2) ? Pl : Sg);
      if (const char* a = objAdj[rnd(sizeof objAdj / sizeof *objAdj)]) o = with(o, A(a));
      b.o(o);
    } else if (kind == 2) {   // dative verb
      b.v(datVerbs[rnd(4)], tense).s(subjNP(true)).io(n(datNouns[rnd(6)], rnd(3) == 0 ? Pl : Sg));
    } else if (kind == 3) {   // intransitive + prepositional phrase
      b.v(intrVerbs[rnd(5)], tense).s(subjNP(true));
      const uint32_t p = rnd(3);
      if (p == 0) b.obl("in", n(places[rnd(6)]));
      else if (p == 1) b.obl("ad", n(places[rnd(6)]));
      else b.obl("cum", n(subjects[rnd(sizeof subjects / sizeof *subjects)], rnd(2) ? Pl : Sg));
    } else if (kind == 4) {   // copula + predicate adjective
      b.v("sum", tense == Perfect || tense == Pluperfect ? (uint8_t)Imperfect : tense).s(subjNP(true)).padj(A(predAdjs[rnd(7)]));
    } else {   // passive perfect with agent ā/ab, or ūtor + ablative
      if (rnd(2)) {
        b.v(accVerbs[rnd(10) % 3 == 0 ? 0 : 7], Perfect).passive().s(subjNP(false)).obl("ab", n(subjects[rnd(15)]));
      } else {
        b.v("ūtor", tense).s(subjNP(true)).o(n(objects[rnd(2) ? 11 : 3], rnd(2) ? Pl : Sg));
      }
    }
    // sentence type
    const uint32_t ty = rnd(10);
    if (ty == 0) b.neg();
    else if (ty == 1) b.c.type = ClauseType::Yn;
    else if (ty == 2) b.wh("cūr").c.type = ClauseType::Wh;
    else if (ty == 3 && kind <= 1) {   // imperative: drop the subject
      b.c.type = ClauseType::Imp;
      b.c.hasSubject = false;
      b.c.pred.tense = Present;
      b.c.pred.number = rnd(2) ? Pl : Sg;
    }
    out.push_back(GenItem{b.c, (int)kind});
  }
  return out;
}

// Replaces token `ti` of a sentence with `form` (macrons as in the text).
std::string replaceToken(const LaSentence& s, size_t ti, const std::string& form) {
  std::string t = s.text;
  const auto& tv = s.tokens[ti];
  std::string f = form;
  if (tv.start == 0) realise::Punctuation::capitaliseFirst(f);
  t.replace((size_t)tv.start, (size_t)(tv.end - tv.start), f);
  return t;
}

// Words of a sentence (punctuation dropped) and the lexicon readings of each (display must match the spelling).
struct Rd { Features f; uint8_t lgender = 0, pos = 0; };
void splitWords(const std::string& t, std::vector<std::string>& o) {
  std::string w;
  for (size_t i = 0; i < t.size();) {
    size_t j = i;
    const char32_t c = text::decodeUtf8(t, j);
    if (c == ' ' || c == '.' || c == ',' || c == '?' || c == '!') { if (!w.empty()) o.push_back(w); w.clear(); }
    else w += t.substr(i, j - i);
    i = j;
  }
  if (!w.empty()) o.push_back(w);
}
std::vector<Rd> readingsOf(const lex::Lexicon& lx, const std::string& w) {
  std::vector<Rd> v;
  morph::Token t;
  morph::analyseLatin(lx, w, t);
  const std::string canon = text::lower(morph::displayForm(w, true));
  for (const auto& a : t.analyses) {
    if (text::lower(morph::displayForm(a.display, true)) != canon) continue;
    const lex::Lemma l = lx.lemma(a.lemma);
    v.push_back(Rd{unpack(lx.feature(a.feat)), l.gender, l.pos});
  }
  return v;
}
// A corruption is a real fault only when the changed word, by the lexicon's own readings, agrees with no noun of
// the sentence (an adjective that now fits another noun, or a verb that now matches another nominative, is Latin).
bool realFault(const lex::Lexicon& lx, const LaSentence& orig, const std::string& bad) {
  std::vector<std::string> wa, wb;
  splitWords(orig.text, wa);
  splitWords(bad, wb);
  if (wa.size() != wb.size()) return true;
  size_t changed = wa.size();
  for (size_t i = 0; i < wa.size(); ++i)
    if (wa[i] != wb[i]) { changed = i; break; }
  if (changed == wa.size()) return false;
  const std::vector<Rd> cr = readingsOf(lx, wb[changed]);
  if (cr.empty()) return true;   // unknown form: A1 catches it
  const auto gm = [](uint8_t g) -> uint8_t {
    return g == M ? 1 : g == F ? 2 : g == N ? 4 : g == MF ? 3 : g == MN ? 5 : g == FN ? 6 : 7;
  };
  // the nearest words with a noun reading on either side (an adjective agrees with a neighbour, not across the clause)
  auto hasHead = [&](size_t i) {
    for (const Rd& h : readingsOf(lx, wb[i]))
      if ((h.pos == Noun || h.pos == Name || h.pos == Pron) && h.f.case_) return true;
    return false;
  };
  std::vector<size_t> near;
  for (size_t i = changed; i-- > 0;) if (hasHead(i)) { near.push_back(i); break; }
  for (size_t i = changed + 1; i < wb.size(); ++i) if (hasHead(i)) { near.push_back(i); break; }
  bool sharedCase = false, anyNominal = false;
  for (size_t i = 0; i < wb.size(); ++i) {
    if (i == changed) continue;
    const bool adjacent = std::find(near.begin(), near.end(), i) != near.end();
    for (const Rd& h : readingsOf(lx, wb[i])) {
      if (adjacent && (h.pos == Noun || h.pos == Name || h.pos == Pron) && h.f.case_)
        for (const Rd& r : cr)
          if (r.pos != Noun && r.pos != Verb && r.f.case_) { anyNominal = true; sharedCase = sharedCase || r.f.case_ == h.f.case_; }
      const bool head = (h.pos == Noun || h.pos == Name || h.pos == Pron) && h.f.case_;
      if (!head) continue;
      const uint8_t hg = h.f.gender ? h.f.gender : h.lgender;
      for (const Rd& r : cr) {
        const bool nominal = r.f.case_ != 0 && (r.pos == Adj || r.pos == Participle || r.pos == Num ||
                                                r.pos == feat::Det || r.pos == Pron || (r.pos == Verb && r.f.mood == ParticipleMood));
        if (nominal && r.f.case_ == h.f.case_ && (!r.f.number || !h.f.number || r.f.number == h.f.number) &&
            (gm(r.f.gender) & gm(hg)))
          return false;
        const bool verb = r.pos == Verb && r.f.person != 0;
        if (verb && (h.f.case_ == Nom || h.f.case_ == Voc) && r.f.number == h.f.number &&
            (r.f.person == 3 || h.pos == Pron))
          return false;
      }
    }
  }
  // an adjective moved to a case no noun of the sentence carries is a substantive, not an agreement slip
  if (anyNominal && !sharedCase) return false;
  return true;
}

}  // namespace

TEST_CASE("rules-la: real lexicon - 2,000 generated clauses pass A1/A3/A4; checker catches 200 corruptions") {
  NEED_REAL();
  const lex::Lexicon& lx = real().lx;
  g_lx = &lx;
  const std::vector<GenItem> items = generated(2000);
  LatinRealiser R(lx, cur());
  check::LatinChecker ck(lx, cur());
  RealiseOptions o;
  LaSentence s;
  size_t faults = 0, missing = 0;
  std::vector<LaSentence> sentences;
  sentences.reserve(items.size());
  for (const GenItem& it : items) {
    R.realise(it.c, o, s);
    if (std::find(s.flags.begin(), s.flags.end(), "missing-form") != s.flags.end()) {
      ++missing;
      MESSAGE("missing form: " << s.text);
    }
    const check::Report rep = ck.check(s.text, {});
    const std::string f = failures(rep, {"A1", "A3", "A4"});
    if (!f.empty()) {
      ++faults;
      if (faults <= 30) MESSAGE("fault: " << s.text << " -> " << f);
    }
    sentences.push_back(s);
  }
  CHECK(faults == 0);
  CHECK(missing == 0);
  MESSAGE("generated 2000 clauses: " << faults << " checker faults, " << missing << " missing forms; e.g. '"
                                     << sentences[0].text << "', '" << sentences[1].text << "', '" << sentences[7].text << "'");

  // ---- 200 deliberate corruptions (wrong case / number / gender / person), each must be caught ----
  size_t made = 0, caught = 0;
  uint32_t x = 777;
  auto rnd = [&](uint32_t m) { x = x * 1103515245u + 12345u; return (x >> 8) % m; };
  std::string form;
  morph::Token tok;
  for (size_t i = 0; i < items.size() && made < 200; ++i) {
    const LaClause& c = items[i].c;
    const LaSentence& st = sentences[i];
    if (c.type != ClauseType::Decl || c.polarity != Polarity::Pos) continue;
    std::string bad, what;
    const int k = (int)rnd(5);
    // the token of a lemma: first occurrence (subject side) or last (object / oblique side)
    auto tokenOf = [&](uint32_t lemma, bool last = false) -> long {
      long found = -1;
      for (size_t t = 0; t < st.tokens.size(); ++t)
        if (st.tokens[t].hasLemma && st.tokens[t].lemmaId == lemma) { found = (long)t; if (!last) break; }
      return found;
    };
    auto differs = [&](const std::string& a, const std::string& b) { return text::latin_key(a) != text::latin_key(b); };
    if (k == 0 && c.hasSubject && !c.subject.isPronoun && !c.subject.adjectives.empty()) {   // subject adjective
      const uint32_t adj = c.subject.adjectives[0].lemma;
      const long t = tokenOf(adj);
      const lex::Lemma hl = lx.lemma(c.subject.head);
      const uint8_t g = hl.gender == F ? F : hl.gender == N ? N : M;
      const uint8_t cases[] = {Gen, Dat, Acc, Abl};
      Features f = morph::adjForm(cases[rnd(4)], c.subject.number, g);
      if (rnd(2)) { f.case_ = Nom; f.number = c.subject.number == Sg ? Pl : Sg; }
      if (t >= 0 && morph::generate(lx, adj, f, form, true) && differs(form, st.tokens[(size_t)t].text)) {
        bad = replaceToken(st, (size_t)t, form);
        what = "subject adjective case/number";
      }
    } else if (k == 1 && c.hasObject && !c.object.adjectives.empty()) {   // object adjective gender / case
      const uint32_t adj = c.object.adjectives[0].lemma;
      const long t = tokenOf(adj, true);
      const lex::Lemma hl = lx.lemma(c.object.head);
      const uint8_t g = hl.gender == F ? F : hl.gender == N ? N : M;
      const uint8_t oc = c.pred.lemma != kNone && lx.lemma(c.pred.lemma).key == "utor" ? Abl : Acc;
      Features f = morph::adjForm(oc, c.object.number, g == F ? M : F);
      if (rnd(2)) f = morph::adjForm(rnd(2) ? Gen : Nom, c.object.number == Sg ? Pl : Sg, g);
      if (t >= 0 && morph::generate(lx, adj, f, form, true) && differs(form, st.tokens[(size_t)t].text)) {
        bad = replaceToken(st, (size_t)t, form);
        what = "object adjective gender/case";
      }
    } else if (k == 2 && c.hasSubject && !c.subject.isPronoun && c.subject.coord.empty() && c.pred.modal == kNone &&
               c.pred.voice == Active && c.subject.number == Sg) {   // verb person / number with a noun subject
      const long t = tokenOf(c.pred.lemma);
      Features f = morph::verbForm(rnd(2) ? P1 : P2, rnd(2) ? Sg : Pl, c.pred.tense, Indicative, Active);
      if (rnd(3) == 0 || lx.lemma(c.pred.lemma).key == "sum") f.person = P3, f.number = Pl;   // "Puella es" is Latin
      if (t >= 0 && morph::generate(lx, c.pred.lemma, f, form, true) && form.find(' ') == std::string::npos &&
          differs(form, st.tokens[(size_t)t].text)) {
        bad = replaceToken(st, (size_t)t, form);
        what = "verb person/number";
      }
    } else if (k == 3 && c.hasObject && c.object.adjectives.empty() && items[i].kind <= 1) {   // object case
      const long t = tokenOf(c.object.head, true);
      if (t >= 0 && morph::generate(lx, c.object.head, morph::nounForm(rnd(2) ? Abl : Dat, c.object.number), form, true) &&
          differs(form, st.tokens[(size_t)t].text)) {
        // only a real case error: the new form must have no accusative / nominative reading
        morph::analyseLatin(lx, form, tok);
        bool accNom = false;
        for (const auto& a : tok.analyses) {
          const Features ff = unpack(lx.feature(a.feat));
          if (morph::displayForm(a.display, true) == form && (ff.case_ == Acc || ff.case_ == Nom || ff.case_ == Gen)) accNom = true;
        }
        if (!accNom) { bad = replaceToken(st, (size_t)t, form); what = "object case"; }
      }
    } else if (k == 4 && !c.obliques.empty() && c.obliques[0].prep != kNone && c.obliques[0].np.adjectives.empty()) {
      const LaNP& np = c.obliques[0].np;   // case after a preposition
      const long t = tokenOf(np.head, true);
      const uint8_t wrong = lx.lemma(c.obliques[0].prep).key == "ad" ? (rnd(2) ? Abl : Dat) : (rnd(2) ? Acc : Gen);
      if (t >= 0 && morph::generate(lx, np.head, morph::nounForm(wrong, np.number), form, true) &&
          differs(form, st.tokens[(size_t)t].text)) {
        morph::analyseLatin(lx, form, tok);
        const uint16_t allowed = cur().prepCases(lx.lemma(c.obliques[0].prep).key);
        bool ok = false;
        for (const auto& a : tok.analyses) {
          const Features ff = unpack(lx.feature(a.feat));
          if (morph::displayForm(a.display, true) == form && (allowed & (1u << ff.case_))) ok = true;
        }
        if (!ok) { bad = replaceToken(st, (size_t)t, form); what = "case after preposition"; }
      }
    }
    if (bad.empty() || bad == st.text) continue;
    // Only a real fault counts: the corrupted word must not agree, by the lexicon's own readings, with any noun of
    // the sentence (an adjective moved onto another noun, or a verb matching another nominative, is still Latin).
    if (!realFault(lx, st, bad)) continue;
    ++made;
    const check::Report rep = ck.check(bad, {});
    const bool hit = !rep.ok("A1") || !rep.ok("A3") || !rep.ok("A4");
    caught += hit;
    if (!hit) MESSAGE("missed corruption (" << what << "): " << st.text << " => " << bad);
  }
  CHECK(made == 200);
  CHECK(caught == made);
  MESSAGE("corruptions: " << caught << " / " << made << " caught");
}

TEST_CASE("rules-la: real lexicon - determinism (two realisers, byte-identical output and checks)") {
  NEED_REAL();
  g_lx = &real().lx;
  auto run = [&]() {
    LatinRealiser R(real().lx, cur());
    check::LatinChecker ck(real().lx, cur());
    std::string all;
    LaSentence s;
    for (const Row& r : realisationTable()) {
      R.realise(r.c, RealiseOptions{}, s);
      all += s.text + "\n";
      for (const auto& tv : s.tokens) all += std::to_string(tv.start) + ":" + tv.display + ":" + std::to_string(tv.lemmaId) + " ";
      for (const auto& c : ck.check(s.text, {}).checks) all += c.id + (c.ok ? "+" : "-") + c.detail;
    }
    for (const GenItem& it : generated(500)) {
      R.realise(it.c, RealiseOptions{}, s);
      all += s.text + "\n";
    }
    return all;
  };
  const std::string a = run(), b = run();
  CHECK(a.size() > 10000);
  CHECK(a == b);
}

TEST_CASE("rules-la: real lexicon - RSS flat over 10,000 realisations") {
  NEED_REAL();
  g_lx = &real().lx;
  const std::vector<GenItem> items = generated(200);
  LatinRealiser R(real().lx, cur());
  check::LatinChecker ck(real().lx, cur());
  LaSentence s;
  check::Report rep;
  long after1k = -1;
  for (size_t i = 0; i < 10000; ++i) {
    R.realise(items[i % items.size()].c, RealiseOptions{}, s);
    if (i % 10 == 0) ck.check(s.text, {}, rep);
    if (i == 999) after1k = rssAnonKb();
  }
  const long after10k = rssAnonKb();
  if (after1k < 0 || after10k < 0) { MESSAGE("RSS check skipped (not Linux)"); return; }
  MESSAGE("RssAnon after 1,000 realisations: " << after1k << " kB, after 10,000: " << after10k << " kB");
#if defined(VP_RULES_TEST_SANITIZED)
  MESSAGE("RSS growth not asserted under ASan (allocator quarantine); the release build asserts <= 5 %");
#else
  CHECK(after10k <= after1k + after1k / 20);
#endif
}

TEST_CASE("rules-la: debug dump (VP_RULES_DEBUG=<sentence>)") {
  const char* env = std::getenv("VP_RULES_DEBUG");
  if (!env || !*env) return;
  NEED_REAL();
  check::LatinChecker ck(real().lx, cur());
  const check::Report rep = ck.check(env, {});
  for (size_t i = 0; i < rep.tokens.size(); ++i) {
    const auto& t = rep.tokens[i];
    std::string line = t.text + " seg=" + std::to_string(t.segment) + (t.name ? " NAME" : "") + " :";
    for (const auto& a : t.analysis.analyses) {
      const lex::Lemma l = real().lx.lemma(a.lemma);
      const auto f = realise::featureView(real().lx.feature(a.feat));
      line += " [" + std::string(a.display) + "<" + std::string(l.head) + " " + f.pos + " " + f.case_ + " " + f.number + " " + f.gender + " " + f.person + " " + f.mood + "]";
    }
    MESSAGE(line);
  }
  for (const auto& is : rep.issues) MESSAGE(is.id << (is.warning ? "~" : "!") << " tok " << is.token << ": " << is.detail);
}
