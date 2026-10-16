// engine/rules orberg (C14): Orbergise. Needs the real lexicon (data/work/latin.vpl or VP_DATA_WORK); every case
// skips with a message when it is absent.
#include <doctest.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "vp/check.h"
#include "vp/engine_config.h"
#include "vp/nlp.h"
#include "vp/rules.h"
#include "vp/curated.h"
#include "vp/result.h"
#include "vp/features.h"
#include "vp/la2x.h"
#include "vp/morph.h"
#include "vp/lex.h"
#include "vp/orberg.h"
#include "vp/text.h"

namespace stdfs = std::filesystem;
using namespace vp;

namespace {
stdfs::path repoDirO() { return stdfs::path(VP_FIXTURES_DIR).parent_path().parent_path(); }
stdfs::path workDirO() {
  const char* e = std::getenv("VP_DATA_WORK");
  return e && *e ? stdfs::path(e) : repoDirO() / "data" / "work";
}
stdfs::path curatedDirO() {
  const char* e = std::getenv("VP_CURATED_DIR");
  return e && *e ? stdfs::path(e) : repoDirO() / "data" / "curated";
}

struct OWorld {
  lex::Lexicon la;
  std::unique_ptr<curated::CuratedData> cd;
  std::unique_ptr<la2x::Translator> tr;
  std::unique_ptr<check::LatinChecker> checker;
  std::unique_ptr<orberg::Resources> res;
  orberg::EngineContext ctx;
  bool ok = false;
  std::string why;
};
OWorld& oworld() {
  static OWorld w = [] {
    OWorld x;
    auto la = lex::Lexicon::open(workDirO() / "latin.vpl");
    if (!la.ok()) { x.why = la.error().message; return x; }
    x.la = std::move(la.value());
    auto cd = curated::CuratedData::load(curatedDirO());
    if (!cd.ok()) { x.why = cd.error().message; return x; }
    x.cd = std::make_unique<curated::CuratedData>(std::move(cd.value()));
    auto tr = la2x::Translator::create(x.la, *x.cd, {curatedDirO()});
    if (!tr.ok()) { x.why = tr.error().message; return x; }
    x.tr = std::move(tr.value());
    x.checker = std::make_unique<check::LatinChecker>(x.la, *x.cd);
    auto res = orberg::Resources::create(x.la, *x.cd, {curatedDirO()});
    if (!res.ok()) { x.why = res.error().message; return x; }
    x.res = std::move(res.value());
    x.ctx.la = &x.la;
    x.ctx.cd = x.cd.get();
    x.ctx.la2x = x.tr.get();
    x.ctx.checker = x.checker.get();
    x.ctx.resources = x.res.get();
    x.ok = true;
    return x;
  }();
  return w;
}
#define NEED_OWORLD()                                                        \
  OWorld& W = oworld();                                                      \
  if (!W.ok) {                                                               \
    MESSAGE("skipped: real Latin lexicon not available (" << W.why << ")");  \
    return;                                                                  \
  }
// The whole engine (pair la-la, original-language path): Latin, English and Spanish lexicons + NLP models.
struct EWorld {
  lex::Lexicon en, es;
  std::unique_ptr<rules::Engine> engine;
  bool ok = false;
  std::string why;
};
EWorld& eworld() {
  static EWorld w = [] {
    EWorld x;
    OWorld& o = oworld();
    if (!o.ok) { x.why = o.why; return x; }
    auto en = lex::Lexicon::open(workDirO() / "english.vpl");
    if (!en.ok()) { x.why = en.error().message; return x; }
    x.en = std::move(en.value());
    auto es = lex::Lexicon::open(workDirO() / "spanish.vpl");
    if (es.ok()) x.es = std::move(es.value());
    const char* nd = std::getenv("VP_NLP_DIR");
    const stdfs::path nlp = nd && *nd ? stdfs::path(nd) : workDirO() / "nlp";
    std::error_code ec;
    if (!stdfs::is_regular_file(nlp / "english.tag.vpt", ec)) { x.why = "no NLP models in " + nlp.string(); return x; }
    rules::EngineConfig cfg = rules::defaultEngineConfig();
    cfg.curatedDir = curatedDirO().string();
    cfg.dataDir = workDirO().string();
    cfg.nlpDir = nlp.string();
    x.engine = rules::makeEngine(cfg);
    x.engine->setLexicons(&o.la, nullptr, &x.en, es.ok() ? &x.es : nullptr);
    x.ok = true;
    return x;
  }();
  return w;
}
#define NEED_EWORLD()                                                                     \
  EWorld& E = eworld();                                                                   \
  if (!E.ok) {                                                                            \
    MESSAGE("skipped: real lexicons / NLP models not available (" << E.why << ")");       \
    return;                                                                               \
  }

std::vector<std::vector<std::string>> readTsvO(const stdfs::path& p) {
  std::vector<std::vector<std::string>> rows;
  std::ifstream in(p);
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::vector<std::string> c;
    size_t a = 0;
    for (;;) {
      const size_t b = line.find('\t', a);
      c.push_back(line.substr(a, b == std::string::npos ? std::string::npos : b - a));
      if (b == std::string::npos) break;
      a = b + 1;
    }
    rows.push_back(c);
  }
  return rows;
}

rules::Options orbergOptions(int tier = 1) {
  rules::Options o;
  o.source = rules::Lang::La;
  o.target = rules::Lang::La;
  o.orbergise = true;
  o.orbergTier = tier;
  return o;
}
}  // namespace

TEST_CASE("orberg: with-original dump (dev)") {
  const char* f = std::getenv("VP_ORBERG_ODUMP");
  if (!f || !*f) return;
  NEED_EWORLD();
  for (const auto& row : readTsvO(f)) {
    if (row.size() < 5) continue;
    rules::CueInput c;
    c.sourceText = row[3];
    c.originalText = row[2];
    c.originalLang = row[1] == "es" ? rules::Lang::Es : rules::Lang::En;
    auto r = E.engine->translate({c}, orbergOptions(), rules::Context{}, nullptr, nullptr);
    REQUIRE(r.ok());
    const rules::CueOutput& o = r.value()[0];
    std::cout << (o.target == row[4] ? "OK " : "XX ") << row[0] << " " << row[2] << "\n   " << row[3] << "\n-> " << o.target
              << "  [conf " << (int)o.confidence << ", meaning " << o.meaningPercent << "]";
    for (const auto& fl : o.flags) std::cout << " " << fl;
    std::cout << "\n";
    for (const auto& x : o.reasons) std::cout << "     " << x.kind << ": " << x.text << "\n";
    for (const auto& x : o.checks) if (!x.ok) std::cout << "     " << x.id << " FAIL " << x.detail << "\n";
  }
}

TEST_CASE("orberg: dump (dev)") {
  const char* f = std::getenv("VP_ORBERG_DUMP");
  if (!f || !*f) return;
  NEED_OWORLD();
  std::ifstream in(f);
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    orberg::OrbergOptions o;
    std::string expected;
    if (line.find('\t') != std::string::npos) {   // fixture row: id ceiling latin expected ...
      std::vector<std::string> c;
      size_t a = 0;
      for (;;) {
        const size_t b = line.find('\t', a);
        c.push_back(line.substr(a, b == std::string::npos ? std::string::npos : b - a));
        if (b == std::string::npos) break;
        a = b + 1;
      }
      if (c.size() < 4) continue;
      o.tierCeiling = std::atoi(c[1].c_str());
      line = c[2];
      expected = c[3];
    }
    const orberg::OrbergResult r = orberg::orbergise(line, nullptr, rules::Lang::En, o, W.ctx);
    if (!expected.empty()) std::cout << (expected == r.text ? "OK " : "XX ") << expected << "\n";
    std::cout << "== " << line << "\n-> " << r.text << "   [meaning " << r.meaning << ", conf " << (int)r.confidence
              << "]";
    for (const auto& m : r.missing) std::cout << " miss:" << m;
    for (const auto& fl : r.flags) std::cout << " flag:" << fl;
    std::cout << "\n";
    for (const auto& c : r.changes)
      std::cout << "   " << c.reason << "/" << c.rule << ": " << c.from << " -> " << c.to << " @" << c.tokenIndex << " ("
                << c.why << ")\n";
    for (const auto& c : r.checks)
      if (!c.ok) std::cout << "   " << c.id << " FAIL " << c.detail << "\n";
    for (const auto& x : r.reasons)
      if (x.data.empty() || x.text.find(" kept: ") != std::string::npos) std::cout << "   note: " << x.text << "\n";
  }
}

namespace {
// Normalisation for the comparison: line breaks and runs of spaces become one space (case, macrons and punctuation
// count).
std::string normO(const std::string& s) {
  std::string o;
  for (char c : s) {
    const char x = c == '\n' || c == '\t' || c == '\r' ? ' ' : c;
    if (x == ' ' && (o.empty() || o.back() == ' ')) continue;
    o += x;
  }
  while (!o.empty() && o.back() == ' ') o.pop_back();
  return o;
}
bool checkOk(const std::vector<rules::Check>& cs, const char* id) {
  for (const rules::Check& c : cs)
    if (c.id == id) return c.ok;
  return true;
}
std::string checkDetail(const std::vector<rules::Check>& cs) {
  std::string d;
  for (const rules::Check& c : cs)
    if (!c.ok) d += " " + c.id + "(" + c.detail + ")";
  return d;
}
long rssAnonKbO() {
  std::ifstream in("/proc/self/status");
  std::string k;
  long v = -1;
  while (in >> k) {
    if (k == "RssAnon:") { in >> v; return v; }
    std::string rest;
    std::getline(in, rest);
  }
  return v;
}
stdfs::path buildDirO() { return stdfs::path(VP_TEST_TMP).parent_path(); }
}  // namespace

TEST_CASE("orberg: simplify_la.tsv loads; broken rows are warnings") {
  NEED_OWORLD();
  CHECK(W.res->warnings().empty());
  for (const std::string& w : W.res->warnings()) MESSAGE(w);
  const stdfs::path tmp = stdfs::path(VP_TEST_TMP) / "orberg_tables";
  std::error_code ec;
  stdfs::create_directories(tmp, ec);
  {
    std::ofstream o(tmp / "simplify_la.tsv");
    o << "# broken rows\nrule\nrule\tablabs.perf\ton\tconj=postquam\tnote\nrule\tablabs.perf\toff\t\tduplicate\n"
         "pair\t\t\tagree\nwhat\tever\tx\nkeep\tcuro\tverb\t-\n";
  }
  auto r = orberg::Resources::create(W.la, *W.cd, {tmp});
  REQUIRE(r.ok());
  CHECK(r.value()->warnings().size() == 4);
  auto missing = orberg::Resources::create(W.la, *W.cd, {tmp / "nowhere"});
  CHECK(!missing.ok());
  CHECK(missing.error().code == ErrorCode::NotFound);
}

TEST_CASE("orberg: 60 own sentences (no original)") {
  NEED_OWORLD();
  const auto rows = readTsvO(repoDirO() / "tests" / "fixtures" / "orberg" / "sentences.tsv");
  REQUIRE(rows.size() == 60);
  int match = 0, exact = 0, faults = 0, tierFaults = 0, meaningLow = 0, changed = 0;
  std::ofstream rep(buildDirO() / "orberg_report.txt");
  rep << "# Orbergise own set (tests/fixtures/orberg/sentences.tsv): id, ok, input, output, expected, changes\n";
  for (const auto& row : rows) {
    REQUIRE(row.size() >= 5);
    orberg::OrbergOptions o;
    o.tierCeiling = std::atoi(row[1].c_str());
    const orberg::OrbergResult r = orberg::orbergise(row[2], nullptr, rules::Lang::En, o, W.ctx);
    const bool m = normO(r.text) == normO(row[3]);
    match += m ? 1 : 0;
    exact += r.text == row[3] ? 1 : 0;
    changed += r.changes.empty() ? 0 : 1;
    rep << row[0] << "\t" << (m ? "ok" : "MISMATCH") << "\t" << row[2] << "\t" << r.text << "\t" << row[3] << "\t";
    for (const orberg::Change& c : r.changes) rep << "[" << c.reason << ": " << c.from << " -> " << c.to << "] ";
    rep << "\n";
    if (!m) MESSAGE("#" << row[0] << " expected: " << row[3] << "\n     got: " << r.text);
    // every output: grammar checks clean, tier ceiling met (names exempt), meaning kept
    const bool g = checkOk(r.checks, "A1") && checkOk(r.checks, "A3") && checkOk(r.checks, "A4");
    if (!g) { ++faults; MESSAGE("#" << row[0] << " " << r.text << ":" << checkDetail(r.checks)); }
    if (!checkOk(r.checks, "A6")) { ++tierFaults; MESSAGE("#" << row[0] << " tier:" << checkDetail(r.checks)); }
    if (r.meaning < 0.6 || !r.missing.empty()) { ++meaningLow; MESSAGE("#" << row[0] << " meaning " << r.meaning); }
    CHECK(r.confidence != rules::Confidence::Fix);
    // the expected rewrite of an unchanged sentence is the input itself, byte for byte
    if (row[2] == row[3]) CHECK(r.text == row[2]);
    for (const orberg::Change& c : r.changes) {
      CHECK((c.reason == "vocabulary" || c.reason == "structure" || c.reason == "order"));
      CHECK(!c.why.empty());
      if (!c.to.empty()) CHECK(c.tokenIndex >= 0);
    }
    size_t orb = 0;
    for (const rules::Reason& x : r.reasons)
      if (x.kind == "orbergise" && x.data.find("\"was\"") != std::string::npos) ++orb;
    CHECK(orb >= r.changes.size());
  }
  MESSAGE("Orbergise own set: " << match << "/60 normalised match (" << exact << " byte-exact), " << changed
                                << " rewritten, grammar faults " << faults << ", tier faults " << tierFaults
                                << ", meaning below 100 % " << meaningLow);
  rep << "# match " << match << "/60, exact " << exact << ", faults " << faults << ", tier " << tierFaults << "\n";
  CHECK(match >= 48);
  CHECK(faults == 0);
  CHECK(tierFaults == 0);
  CHECK(meaningLow == 0);
}

TEST_CASE("orberg: second batch of 20 own sentences") {
  NEED_OWORLD();
  const auto rows = readTsvO(repoDirO() / "tests" / "fixtures" / "orberg" / "sentences_more.tsv");
  REQUIRE(rows.size() == 20);
  int match = 0, faults = 0;
  for (const auto& row : rows) {
    orberg::OrbergOptions o;
    o.tierCeiling = std::atoi(row[1].c_str());
    const orberg::OrbergResult r = orberg::orbergise(row[2], nullptr, rules::Lang::En, o, W.ctx);
    const bool m = normO(r.text) == normO(row[3]);
    match += m ? 1 : 0;
    if (!m) MESSAGE("#" << row[0] << " expected: " << row[3] << "\n     got: " << r.text);
    if (!checkOk(r.checks, "A1") || !checkOk(r.checks, "A3") || !checkOk(r.checks, "A4") || !checkOk(r.checks, "A6")) {
      ++faults;
      MESSAGE("#" << row[0] << " " << r.text << ":" << checkDetail(r.checks));
    }
    CHECK(r.missing.empty());
  }
  MESSAGE("Orbergise second batch: " << match << "/20, faults " << faults);
  CHECK(match >= 16);
  CHECK(faults == 0);
}

TEST_CASE("orberg: options (simplify off, tier 2, names)") {
  NEED_OWORLD();
  orberg::OrbergOptions o;
  o.simplify = false;   // vocabulary only: the ablative absolute stays
  orberg::OrbergResult r = orberg::orbergise("Epistulā lēctā, puer lupum cōnspexit.", nullptr, rules::Lang::En, o, W.ctx);
  CHECK(r.text == "Epistulā lēctā, puer lupum vīdit.");
  REQUIRE(r.changes.size() == 1);
  CHECK(r.changes[0].reason == "vocabulary");
  CHECK(r.changes[0].from == "cōnspexit");
  CHECK(r.changes[0].to == "vīdit");
  o.simplify = true;
  o.tierCeiling = 2;    // cōnspiciō is tier 2: kept
  r = orberg::orbergise("Epistulā lēctā, puer lupum cōnspexit.", nullptr, rules::Lang::En, o, W.ctx);
  CHECK(r.text == "Postquam epistula lēcta est, puer lupum cōnspexit.");
  // names are never rewritten, only declined where the structure needs it
  o.tierCeiling = 1;
  r = orberg::orbergise("Mārcō dormiente, Iūlia cantābat.", nullptr, rules::Lang::En, o, W.ctx);
  CHECK(r.text == "Dum Mārcus dormit, Iūlia cantābat.");
  CHECK(r.meaning == doctest::Approx(1.0));
  // a sentence that is already beginner Latin comes back byte for byte, without changes
  r = orberg::orbergise("Puella  rosam amat.", nullptr, rules::Lang::En, o, W.ctx);
  CHECK(r.text == "Puella  rosam amat.");
  CHECK(r.changes.empty());
  CHECK(r.confidence == rules::Confidence::Ok);
  // two sentences in one cue
  r = orberg::orbergise("Puer lupum cōnspexit. Mīles rēgem interfēcit.", nullptr, rules::Lang::En, o, W.ctx);
  CHECK(r.text == "Puer lupum vīdit. Mīles rēgem necāvit.");
  REQUIRE(r.changes.size() == 2);
  REQUIRE(r.changes[1].tokenIndex >= 0);
  CHECK(r.tokens[(size_t)r.changes[1].tokenIndex].text == "necāvit");
}

namespace {
// Resources from simplify_la.tsv with extra rows written FIRST (tests that need a teacher's mistake).
std::unique_ptr<orberg::Resources> resourcesWith(OWorld& W, const std::string& name, const std::string& rows) {
  const stdfs::path tmp = stdfs::path(VP_TEST_TMP) / name;
  std::error_code ec;
  stdfs::create_directories(tmp, ec);
  std::ifstream in(curatedDirO() / "simplify_la.tsv");
  std::string all((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  {
    std::ofstream o(tmp / "simplify_la.tsv");
    o << rows << all;
  }
  auto r = orberg::Resources::create(W.la, *W.cd, {tmp});
  if (!r.ok()) return nullptr;
  return std::move(r.value());
}
bool hasFlag(const std::vector<std::string>& f, const char* x) { return std::find(f.begin(), f.end(), x) != f.end(); }
bool hasNote(const std::vector<rules::Reason>& rs, const std::string& part) {
  for (const rules::Reason& r : rs)
    if (r.text.find(part) != std::string::npos) return true;
  return false;
}
}  // namespace

TEST_CASE("orberg: meaning check") {
  NEED_OWORLD();
  orberg::OrbergOptions o;
  // a confirmed periphrasis counts as the word it replaces; a syn row alone keeps the cue OK
  orberg::OrbergResult r = orberg::orbergise("Mercātor festīnat.", nullptr, rules::Lang::En, o, W.ctx);
  CHECK(r.text == "Mercātor celeriter it.");
  CHECK(r.meaning == doctest::Approx(1.0));
  CHECK(r.missing.empty());
  CHECK(r.confidence == rules::Confidence::Ok);
  // the check is real (C27): a wrong teacher row is reported as a loss, the cue is Check, A7 fails
  auto bad = resourcesWith(W, "orberg_badsyn",
                           "syn\tmoveō\tmūtō\tverb\twrong on purpose\nsyn\tmagnificus\tparvus\tadj\twrong on purpose\n"
                           "syn\tdīligō\tnoceō\tverb\twrong on purpose\n");
  REQUIRE(bad);
  orberg::EngineContext ctx = W.ctx;
  ctx.resources = bad.get();
  r = orberg::orbergise("Folia in arbore moventur.", nullptr, rules::Lang::En, o, ctx);
  CHECK(r.text == "Folia in arbore mūtantur.");
  CHECK(r.meaning < 1.0);
  CHECK(std::find(r.missing.begin(), r.missing.end(), "moveō -> mūtō") != r.missing.end());
  CHECK(r.confidence == rules::Confidence::Check);
  CHECK(hasFlag(r.flags, "meaning-lost"));
  CHECK(!checkOk(r.checks, "A7"));
  r = orberg::orbergise("Templum magnificum est.", nullptr, rules::Lang::En, o, ctx);
  CHECK(r.text == "Templum parvum est.");
  CHECK(std::find(r.missing.begin(), r.missing.end(), "magnificus -> parvus") != r.missing.end());
  CHECK(r.meaning < 0.6);
  CHECK(hasFlag(r.flags, "meaning-low"));
  // with an original: a word of the original the input covered and the output lost is listed too
  const std::string orig = "The temple is magnificent.";
  r = orberg::orbergise("Templum magnificum est.", &orig, rules::Lang::En, o, ctx);
  CHECK(std::find(r.missing.begin(), r.missing.end(), "\"magnificent\" (original)") != r.missing.end());
  // never nonsense: a rewrite the checker rejects (noceō takes a dative) is discarded, the sentence stays as written
  r = orberg::orbergise("Puer puellam dīligit.", nullptr, rules::Lang::En, o, ctx);
  CHECK(r.text == "Puer puellam dīligit.");
  CHECK(hasFlag(r.flags, "rewrite-discarded"));
  CHECK(hasNote(r.reasons, "A4"));
  CHECK(r.confidence == rules::Confidence::Check);
  // the same sentence with the real tables: a lexicon synonym of the same sense, at most Check
  r = orberg::orbergise("Puer puellam dīligit.", nullptr, rules::Lang::En, o, W.ctx);
  CHECK(r.text == "Puer puellam amat.");
  CHECK(hasFlag(r.flags, "synonym"));
  CHECK(r.confidence == rules::Confidence::Check);
  CHECK(r.missing.empty());
}

TEST_CASE("orberg: the original is evidence, never the output (C27)") {
  NEED_OWORLD();
  orberg::OrbergOptions o;
  int calls = 0;
  orberg::EngineContext ctx = W.ctx;
  ctx.fromOriginal = [&](const std::string&, rules::Lang, int, const std::vector<uint32_t>&, orberg::OriginalLatin& out) {
    ++calls;
    out.text = "Nunc domum īre dēbēmus.";
    return true;
  };
  const std::string orig = "We must go home now.";
  orberg::OrbergResult r = orberg::orbergise("Eundum est.", &orig, rules::Lang::En, o, ctx);
  CHECK(calls == 0);
  CHECK(!r.fromOriginal);
  CHECK(r.text == "Īre dēbēmus.");   // the original names the person of dēbeō; nothing else is taken from it
  CHECK(hasFlag(r.flags, "original-evidence"));
  CHECK(!hasFlag(r.flags, "orberg-original"));
  // an original that says something else changes nothing but the evidence
  const std::string other = "The girl reads a book.";
  r = orberg::orbergise("Puella rosam amat.", &other, rules::Lang::En, o, ctx);
  CHECK(r.text == "Puella rosam amat.");
  CHECK(r.changes.empty());
  CHECK(hasFlag(r.flags, "orberg-kept"));
  // without a person in the original the gerund stays (Check)
  const std::string none = "Going is necessary.";
  r = orberg::orbergise("Eundum est.", &none, rules::Lang::En, o, ctx);
  CHECK(r.text == "Eundum est.");
  CHECK(r.confidence == rules::Confidence::Check);
}

TEST_CASE("orberg: real-material fixes (C27): same sense, fixed phrases, never nonsense") {
  NEED_OWORLD();
  orberg::OrbergOptions o;
  // (input, expected): our own sentences of the shapes found on real material; "=" means unchanged byte for byte
  const std::vector<std::pair<std::string, std::string>> cases = {
      // a verb of motion never becomes a verb of change, ceasing or returning; no same-sense core word: kept (Check)
      {"Folia in arbore moventur.", "="},
      {"Hospites discedunt.", "Hospites abeunt."},
      {"Quo vadunt nautae?", "Quo eunt nautae?"},
      {"Pater filium valere iubet et discedit.", "Pater filium valere iubet et abit."},
      {"Testes pro amico testificati sunt.", "="},
      {"Mercator pericula affert.", "="},
      // fixed phrases are never split or swapped
      {"Magister res gestas Romanorum narrat.", "="},
      {"Puella matri gratias agit.", "="},
      {"Deo gratias! Navis advenit.", "Deo gratias! Navis venit."},
      {"Discipuli ad scribendum veniunt.", "="},
      // never nonsense: a nominative with a participle after est is no ablative absolute; cum with an ablative and a
      // purpose clause is the preposition; a form of a core word (meō is meus) is never swapped
      {"Cur est porta clausa 🚪, serve?", "="},
      {"Nunc est via relicta 🛤️, puer.", "="},
      {"Mater venit ad cantandum🎶 cum filia ut eam doceret.", "="},
      {"In horto🌳 meo omnia sunt pulchra.", "="},
      {"Puer in horto meo ludit.", "="},
      // emoji clusters (skin tone, ZWJ, variation selector) stay byte for byte where they were
      {"Puer 👦🏽 et pater 👨‍👦 ad montem ⛰️ vadunt.", "Puer 👦🏽 et pater 👨‍👦 ad montem ⛰️ eunt."},
      {"♪ et vestimenta👗👔 lavat", "♪ et vestes👗👔 lavat"},
  };
  for (const auto& c : cases) {
    const orberg::OrbergResult r = orberg::orbergise(c.first, nullptr, rules::Lang::En, o, W.ctx);
    const std::string want = c.second == "=" ? c.first : c.second;
    CHECK_MESSAGE(r.text == want, c.first << " -> " << r.text);
    for (const char* bad : {"mut", "desin", "dēsin", "rede", "red", "tul", "portat", "postquam", "Postquam", "dum "})
    {
      const bool clean = r.text.find(bad) == std::string::npos || c.first.find(bad) != std::string::npos;
      CHECK_MESSAGE(clean, c.first << " -> " << r.text);
    }
    CHECK(r.missing.empty());
    if (c.second == "=") CHECK(r.changes.empty());
  }
  // kept words are said and the cue is Check
  orberg::OrbergResult r = orberg::orbergise("Folia in arbore moventur.", nullptr, rules::Lang::En, o, W.ctx);
  CHECK(hasFlag(r.flags, "tier-kept"));
  CHECK(hasNote(r.reasons, "moventur kept: no first-year word with the same sense"));
  CHECK(r.confidence == rules::Confidence::Check);
  r = orberg::orbergise("Magister res gestas Romanorum narrat.", nullptr, rules::Lang::En, o, W.ctx);
  CHECK(hasNote(r.reasons, "fixed phrase rēs gestae"));
  // a swap of a confirmed pair may stay OK; a periphrasis row or a lexicon synonym is at most Check
  r = orberg::orbergise("Pueri ad urbem festinant.", nullptr, rules::Lang::En, o, W.ctx);
  CHECK(r.text == "Pueri ad urbem celeriter eunt.");
  CHECK(r.confidence == rules::Confidence::Ok);
  r = orberg::orbergise("Hostis urbem oppugnat.", nullptr, rules::Lang::En, o, W.ctx);   // no same-sense word: kept
  CHECK(r.text == "Hostis urbem oppugnat.");
  CHECK(hasFlag(r.flags, "tier-kept"));
  CHECK(r.confidence == rules::Confidence::Check);
  r = orberg::orbergise("Nauta nāvem in ōceanō vīdit.", nullptr, rules::Lang::En, o, W.ctx);   // periphrasis row only
  CHECK(r.text == "Nauta nāvem in marī vīdit.");
  CHECK(r.confidence == rules::Confidence::Check);
  CHECK(hasFlag(r.flags, "synonym"));
  // nouns of persons keep their sex (magistra is not doctor at tier 2)
  o.tierCeiling = 2;
  r = orberg::orbergise("Magistra puerum laudat.", nullptr, rules::Lang::En, o, W.ctx);
  CHECK(r.text.find("doctor") == std::string::npos);
}

TEST_CASE("orberg: blind check of C27 (20 own sentences, one document)") {
  NEED_EWORLD();
  const auto rows = readTsvO(repoDirO() / "tests" / "fixtures" / "orberg" / "blind_c27.tsv");
  REQUIRE(rows.size() == 20);
  std::vector<rules::CueInput> in;
  for (const auto& row : rows) {
    REQUIRE(row.size() >= 5);
    rules::CueInput c;
    c.index = (uint32_t)std::atoi(row[0].c_str());
    c.sourceText = row[1];
    in.push_back(c);
  }
  auto r = E.engine->translate(in, orbergOptions(), rules::Context{}, nullptr, nullptr);
  REQUIRE(r.ok());
  REQUIRE(r.value().size() == 20);
  int match = 0, firstRun = 0;
  for (size_t i = 0; i < rows.size(); ++i) {
    const rules::CueOutput& o = r.value()[i];
    const bool m = normO(o.target) == rows[i][2];
    match += m ? 1 : 0;
    firstRun += rows[i][4].compare(0, 3, "yes") == 0 ? 1 : 0;
    if (!m) MESSAGE("#" << rows[i][0] << " expected: " << rows[i][2] << "\n     got: " << normO(o.target));
    CHECK(o.meaningMissing.empty());
    CHECK(o.confidence != rules::Confidence::Fix);
  }
  MESSAGE("Orbergise blind check C27: " << match << "/20 (first run before the fixes: " << firstRun << "/20)");
  CHECK(match == 20);
}

TEST_CASE("orberg: macrons follow the source, unchanged cues keep their lines (C27, engine pair la-la)") {
  NEED_EWORLD();
  rules::CueInput a, b, c;
  a.index = 0;
  a.sourceText = "Aspice caelum, puella!";
  b.index = 1;
  b.sourceText = "Pueri ad urbem festinant.";
  c.index = 2;
  c.sourceText = "Puella in horto\nrosam amat.";
  // a document without length marks: new words get none
  auto r = E.engine->translate({a, b, c}, orbergOptions(), rules::Context{}, nullptr, nullptr);
  REQUIRE(r.ok());
  REQUIRE(r.value().size() == 3);
  CHECK(normO(r.value()[0].target) == "Specta caelum, puella!");
  CHECK(normO(r.value()[1].target) == "Pueri ad urbem celeriter eunt.");
  CHECK(r.value()[2].target == "Puella in horto\nrosam amat.");   // byte for byte, line break included
  // the same cue in a document written with macrons
  rules::CueInput m;
  m.index = 3;
  m.sourceText = "Puella in hortō sedet.";
  r = E.engine->translate({a, m}, orbergOptions(), rules::Context{}, nullptr, nullptr);
  REQUIRE(r.ok());
  CHECK(normO(r.value()[0].target) == "Spectā caelum, puella!");
  // emoji clusters through the whole engine path
  rules::CueInput e;
  e.sourceText = "Puer 👦🏽 et pater 👨‍👦 ad montem ⛰️ vadunt.";
  r = E.engine->translate({e}, orbergOptions(), rules::Context{}, nullptr, nullptr);
  REQUIRE(r.ok());
  CHECK(normO(r.value()[0].target) == "Puer 👦🏽 et pater 👨‍👦 ad montem ⛰️ eunt.");
  for (const rules::TokenView& t : r.value()[0].tokens)
    CHECK(r.value()[0].target.compare((size_t)t.start, (size_t)(t.end - t.start), t.text) == 0);
}

TEST_CASE("orberg: with the original (engine pair la-la)") {
  NEED_EWORLD();
  const auto rows = readTsvO(repoDirO() / "tests" / "fixtures" / "orberg" / "with_original.tsv");
  REQUIRE(rows.size() == 22);
  int match = 0, en = 0, enMatch = 0, pathOk = 0;
  for (const auto& row : rows) {
    REQUIRE(row.size() >= 6);
    rules::CueInput c;
    c.index = (uint32_t)std::atoi(row[0].c_str());
    c.sourceText = row[3];
    c.originalText = row[2];
    c.originalLang = row[1] == "es" ? rules::Lang::Es : rules::Lang::En;
    auto r = E.engine->translate({c}, orbergOptions(), rules::Context{}, nullptr, nullptr);
    REQUIRE(r.ok());
    REQUIRE(r.value().size() == 1);
    const rules::CueOutput& o = r.value()[0];
    const bool m = normO(o.target) == normO(row[4]);
    match += m ? 1 : 0;
    if (row[1] == "en") { ++en; enMatch += m ? 1 : 0; }
    if (!m) MESSAGE("#" << row[0] << " (" << row[2] << ") expected: " << row[4] << "\n     got: " << normO(o.target));
    const char* want = row[5] == "kept" ? "orberg-kept" : "orberg-latin";
    const bool p = std::find(o.flags.begin(), o.flags.end(), want) != o.flags.end();
    CHECK(std::find(o.flags.begin(), o.flags.end(), "orberg-original") == o.flags.end());
    CHECK(std::find(o.flags.begin(), o.flags.end(), "original-evidence") != o.flags.end());
    pathOk += p ? 1 : 0;
    CHECK_MESSAGE(p, "#" << row[0] << " path " << row[5]);
    CHECK(o.original == row[2]);
    CHECK(o.meaningPercent >= 60);
    CHECK(o.confidence != rules::Confidence::Fix);
    CHECK(checkOk(o.checks, "A1"));
    CHECK(checkOk(o.checks, "A3"));
    CHECK(checkOk(o.checks, "A4"));
  }
  MESSAGE("Orbergise with the original: " << match << "/" << rows.size() << " (English " << enMatch << "/" << en
                                          << "), path as expected " << pathOk << "/" << rows.size());
  CHECK(match == (int)rows.size());
  CHECK(pathOk == (int)rows.size());
}

TEST_CASE("orberg: engine outputs (fields, reasons, options, empty cue)") {
  NEED_EWORLD();
  rules::CueInput a, b, empty;
  a.index = 7;
  a.sourceText = "Epistulā lēctā, puer domum cucurrit.";
  a.startMs = 0;
  a.endMs = 4000;
  b.index = 8;
  b.sourceText = "Puer lupum cōnspexit.";
  auto r = E.engine->translate({a, b, empty}, orbergOptions(), rules::Context{}, nullptr, nullptr);
  REQUIRE(r.ok());
  REQUIRE(r.value().size() == 3);
  const rules::CueOutput& o = r.value()[0];
  CHECK(o.index == 7);
  CHECK(normO(o.target) == "Postquam epistula lēcta est, puer domum cucurrit.");
  CHECK(o.meaningPercent == 100);
  CHECK(o.meaningMissing.empty());
  CHECK(o.original.empty());
  bool orb = false;
  for (const rules::Reason& x : o.reasons)
    if (x.kind == "orbergise") {
      orb = true;
      CHECK(x.data.find("\"was\":\"Epistulā lēctā\"") != std::string::npos);
      CHECK(x.data.find("\"now\":\"Postquam epistula lēcta est\"") != std::string::npos);
      CHECK(x.data.find("\"why\":") != std::string::npos);
      REQUIRE(x.tokenIndex >= 0);
      CHECK(o.tokens[(size_t)x.tokenIndex].text == "Postquam");
    }
  CHECK(orb);
  for (const char* id : {"A1", "A2", "A3", "A4", "A6", "A7", "A8"}) {
    bool has = false;
    for (const rules::Check& c : o.checks) has = has || c.id == id;
    CHECK_MESSAGE(has, id);
  }
  for (const rules::TokenView& t : o.tokens) CHECK(o.target.compare((size_t)t.start, (size_t)(t.end - t.start), t.text) == 0);
  CHECK(r.value()[1].target == "Puer lupum vīdit.");
  CHECK(r.value()[2].target.empty());
  CHECK(r.value()[2].confidence == rules::Confidence::Ok);
  // Options: simplify off, tier 2, macrons off for new words is the input's convention (no macrons in the input)
  rules::Options opt = orbergOptions(2);
  auto r2 = E.engine->translate({b}, opt, rules::Context{}, nullptr, nullptr);
  REQUIRE(r2.ok());
  CHECK(r2.value()[0].target == "Puer lupum cōnspexit.");
  opt = orbergOptions(1);
  opt.orbergSimplify = false;
  a.endMs = 0;
  auto r3 = E.engine->translate({a}, opt, rules::Context{}, nullptr, nullptr);
  REQUIRE(r3.ok());
  CHECK(normO(r3.value()[0].target) == "Epistulā lēctā, puer domum cucurrit.");
  rules::CueInput plain;
  plain.sourceText = "Puer lupum in silva conspexit.";
  auto r4 = E.engine->translate({plain}, orbergOptions(), rules::Context{}, nullptr, nullptr);
  REQUIRE(r4.ok());
  CHECK(r4.value()[0].target == "Puer lupum in silva vidit.");
  // progress and cancel
  size_t seen = 0;
  int polls = 0;
  auto r5 = E.engine->translate({a, b, b, b}, orbergOptions(), rules::Context{}, [&](size_t d) { seen = d; },
                                [&]() { return ++polls > 2; });
  REQUIRE(r5.ok());
  CHECK(r5.value().size() == 2);
  CHECK(seen == 2);
}

TEST_CASE("orberg: determinism") {
  NEED_OWORLD();
  const auto rows = readTsvO(repoDirO() / "tests" / "fixtures" / "orberg" / "sentences.tsv");
  // a second, independent set of resources and analyser
  auto tr2 = la2x::Translator::create(W.la, *W.cd, {curatedDirO()});
  REQUIRE(tr2.ok());
  auto res2 = orberg::Resources::create(W.la, *W.cd, {curatedDirO()});
  REQUIRE(res2.ok());
  check::LatinChecker ch2(W.la, *W.cd);
  orberg::EngineContext c2 = W.ctx;
  c2.la2x = tr2.value().get();
  c2.resources = res2.value().get();
  c2.checker = &ch2;
  std::string a, b;
  for (int pass = 0; pass < 2; ++pass)
    for (const auto& row : rows) {
      orberg::OrbergOptions o;
      o.tierCeiling = std::atoi(row[1].c_str());
      const orberg::OrbergResult x = orberg::orbergise(row[2], nullptr, rules::Lang::En, o, pass ? c2 : W.ctx);
      std::string& acc = pass ? b : a;
      acc += x.text + "|" + std::to_string(x.meaning) + "|" + std::to_string((int)x.confidence);
      for (const orberg::Change& c : x.changes) acc += "|" + c.from + ">" + c.to + ":" + c.reason;
      acc += "\n";
    }
  CHECK(a == b);
}

TEST_CASE("orberg: RSS flat over 1,000 cues") {
  NEED_EWORLD();
  const auto rows = readTsvO(repoDirO() / "tests" / "fixtures" / "orberg" / "sentences.tsv");
  long after10 = -1;
  int done = 0;
  for (int batch = 0; done < 1000; ++batch) {
    std::vector<rules::CueInput> in;
    const int n = batch == 0 ? 10 : 20;
    for (int k = 0; k < n && done + k < 1000; ++k) {
      rules::CueInput c;
      c.index = (uint32_t)(done + k);
      c.sourceText = rows[(size_t)(done + k) % rows.size()][2];
      in.push_back(c);
    }
    auto r = E.engine->translate(in, orbergOptions(), rules::Context{}, nullptr, nullptr);
    REQUIRE(r.ok());
    done += (int)in.size();
    if (batch == 0) after10 = rssAnonKbO();
  }
  const long after1000 = rssAnonKbO();
  MESSAGE("RssAnon after 10 cues: " << after10 << " kB, after 1,000: " << after1000 << " kB");
#if defined(__SANITIZE_ADDRESS__)
  MESSAGE("AddressSanitizer build: the quarantine holds freed memory, RSS is reported only");
#else
  if (after10 > 0 && after1000 > 0) CHECK((double)after1000 <= (double)after10 * 1.05 + 64.0);
#endif
}

TEST_CASE("orberg: odd input never crashes") {
  NEED_OWORLD();
  std::string longS;
  for (int i = 0; i < 80; ++i) longS += (i % 3 == 0) ? "puella " : (i % 3 == 1) ? "rosam " : "amat et ";
  longS += "cantat.";
  const char* inputs[] = {"", ".", "?!", "   ", "123 456.", "ἄνθρωπος λέγει.", "Puella, , , amat!!!", "-que -ne",
                          "QVIS ES?", "Xyzzy plugh frobnicat.", "[rīdet]", "♪ Puella cantat ♪", "- Quid agis? - Bene.",
                          "Hostibus, , vīsīs", "Cum cum cum.", "est est est sunt.", "Mihi."};
  orberg::OrbergOptions o;
  for (const char* in : inputs) {
    const orberg::OrbergResult r = orberg::orbergise(in, nullptr, rules::Lang::En, o, W.ctx);
    CHECK(std::find(r.flags.begin(), r.flags.end(), "orberg-error") == r.flags.end());
  }
  const orberg::OrbergResult r = orberg::orbergise(longS, nullptr, rules::Lang::En, o, W.ctx);
  CHECK(!r.text.empty());
  // missing context pieces: the input comes back unchanged
  orberg::EngineContext bare;
  const orberg::OrbergResult b = orberg::orbergise("Puer lupum cōnspexit.", nullptr, rules::Lang::En, o, bare);
  CHECK(b.text == "Puer lupum cōnspexit.");
  CHECK(std::find(b.flags.begin(), b.flags.end(), "orberg-unavailable") != b.flags.end());
}
