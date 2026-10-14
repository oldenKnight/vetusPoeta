// engine/rules Greek loop on textbook prose (C38): the calibration of OK in both directions (grc2x meaning coverage: a
// dropped predicate adjective, a dropped coordinated verb, a vocative left in Greek letters, a dropped connector make
// the cue Check; EN -> GRC: a name left in Latin letters, a dropped vocative or connector make it Check), the regression
// on our own 50 beginner-reader sentences (tests/regression/own_athen.grc.txt with the English gold
// expected/own_athen.en.gold.txt, and the reverse direction own_athen.en.txt -> expected/own_athen.grc.gold.txt, both
// written before any C38 rule) and the constructions of the loop (two or more own sentences per rule:
// docs/rules_grc3_notes.md "Athenaze prose (C38)"). Needs the real data (VP_DATA_WORK); skips otherwise.
#include <doctest.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "vp/curated.h"
#include "vp/engine_config.h"
#include "vp/grc2x.h"
#include "vp/lex.h"
#include "vp/rules.h"
#include "vp/text.h"
#include "vp/transfer_grc.h"

namespace stdfs = std::filesystem;
using namespace vp;

namespace {

stdfs::path repo10() { return stdfs::path(VP_FIXTURES_DIR).parent_path().parent_path(); }
stdfs::path work10() {
  const char* e = std::getenv("VP_DATA_WORK");
  return e && *e ? stdfs::path(e) : repo10() / "data" / "work";
}
stdfs::path buildDir10() { return stdfs::path(VP_TEST_TMP).parent_path(); }

struct Real10 {
  lex::Lexicon grc, en, es;
  bool ok = false;
  std::string why;
};
const Real10& real10() {
  static Real10 r = [] {
    Real10 x;
    const char* g = std::getenv("VP_GREEK_VPL");
    auto grc = lex::Lexicon::open(g && *g ? stdfs::path(g) : work10() / "greek.vpl");
    auto en = lex::Lexicon::open(work10() / "english.vpl");
    auto es = lex::Lexicon::open(work10() / "spanish.vpl");
    const bool nlp = stdfs::exists(work10() / "nlp" / "english.tag.vpt");
    if (!grc.ok() || !en.ok() || !es.ok() || !nlp) {
      x.why = !grc.ok() ? grc.error().message : !en.ok() ? en.error().message : !es.ok() ? es.error().message
                                                                                         : "no english.tag.vpt";
      return x;
    }
    x.grc = std::move(grc.value());
    x.en = std::move(en.value());
    x.es = std::move(es.value());
    x.ok = true;
    return x;
  }();
  return r;
}
#define NEED_REAL10()                                                                                \
  if (!real10().ok) {                                                                                \
    MESSAGE("real Greek data not available (" << real10().why << "); set VP_DATA_WORK. Skipped.");  \
    return;                                                                                          \
  }

std::unique_ptr<rules::Engine> engine10() {
  rules::EngineConfig cfg = rules::defaultEngineConfig();
  cfg.curatedDir = (repo10() / "data" / "curated").string();
  cfg.dataDir = work10().string();
  cfg.nlpDir = (work10() / "nlp").string();
  auto e = rules::makeEngine(cfg);
  e->setLexicons(nullptr, &real10().grc, &real10().en, &real10().es);
  return e;
}

// NFC, lower case, punctuation-insensitive (the Greek and the English marks, quotes), spaces collapsed; accents and
// breathings count.
std::string norm10(const std::string& s) {
  std::string l = text::lower(text::nfc(s));
  std::string out;
  size_t i = 0;
  while (i < l.size()) {
    size_t j = i;
    const char32_t c = text::decodeUtf8(l, j);
    const bool punct = c == '.' || c == ',' || c == ';' || c == '!' || c == '?' || c == ':' || c == 0xB7 ||
                       c == 0x387 || c == 0x37E || c == '"' || c == 0xA1 || c == 0xBF || c == 0xAB || c == 0xBB ||
                       c == 0x201C || c == 0x201D;
    if (punct || c == ' ' || c == '\n') {
      if (!out.empty() && out.back() != ' ') out += ' ';
    } else {
      out += l.substr(i, j - i);
    }
    i = j;
  }
  while (!out.empty() && out.back() == ' ') out.pop_back();
  return out;
}

bool matches10(const std::string& got, const std::string& expected) {
  for (size_t a = 0;;) {
    const size_t b = expected.find(" | ", a);
    if (norm10(got) == norm10(text::nfc(expected.substr(a, b == std::string::npos ? std::string::npos : b - a))))
      return true;
    if (b == std::string::npos) return false;
    a = b + 3;
  }
}

enum class Dir { EnGrc, GrcEn, GrcEs, EsGrc };
rules::Options opts10(Dir d) {
  rules::Options o;
  o.source = d == Dir::EnGrc ? rules::Lang::En : d == Dir::EsGrc ? rules::Lang::Es : rules::Lang::Grc;
  o.target = d == Dir::GrcEn ? rules::Lang::En : d == Dir::GrcEs ? rules::Lang::Es : rules::Lang::Grc;
  o.fidelity = 2;
  o.speakerGender = 'm';
  return o;
}

const char* confName10(rules::Confidence c) {
  return c == rules::Confidence::Ok ? "ok" : c == rules::Confidence::Check ? "check" : "fix";
}
std::string flat10(std::string s) {
  std::replace(s.begin(), s.end(), '\n', ' ');
  return s;
}
bool hasFlag10(const rules::CueOutput& o, const char* f) {
  return std::find(o.flags.begin(), o.flags.end(), f) != o.flags.end();
}

std::vector<std::string> lines10(const stdfs::path& p) {
  std::ifstream f(p);
  std::vector<std::string> out;
  std::string line;
  while (std::getline(f, line))
    if (!line.empty() && line[0] != '#') out.push_back(line);
  return out;
}

rules::CueOutput one10(rules::Engine& e, const std::string& src, Dir d) {
  rules::CueInput x;
  x.sourceText = src;
  auto r = e.translate({x}, opts10(d), rules::Context{}, nullptr, nullptr);
  REQUIRE(r.ok());
  return r.value()[0];
}

// One sentence at a time; `expected` may list alternatives after " | ". `mustCheck`: the cue must not be OK (a rebuilt
// structure, a guess, a meaning the engine knows it lost). A cue with a rebuilt / guessed flag is never OK.
struct Case10 { const char* src; const char* expected; bool mustCheck; };
int run10(rules::Engine& e, const std::vector<Case10>& cases, Dir d, const char* what) {
  int ok = 0;
  for (const Case10& k : cases) {
    const rules::CueOutput o = one10(e, k.src, d);
    const std::string got = flat10(o.target);
    const bool hit = matches10(got, k.expected);
    CHECK_MESSAGE(hit, what << ": " << k.src << " -> '" << got << "' expected '" << k.expected << "'");
    CHECK_MESSAGE(o.confidence != rules::Confidence::Fix, what << ": " << k.src << " is Fix");
    bool rebuilt = false;
    for (const char* f : {"clause-repair", "participle-phrase", "realia", "past-form", "from-rule", "det-adverb",
                          "light-verb", "subject-guess", "speech-inversion", "idiom", "lexicon-gap", "addressee-guess",
                          "meaning-coverage", "greek-left", "latin-left"})
      rebuilt = rebuilt || hasFlag10(o, f);
    if (rebuilt || k.mustCheck)
      CHECK_MESSAGE(o.confidence != rules::Confidence::Ok, what << ": " << k.src << " was rebuilt / guessed but is OK");
    ok += hit;
  }
  MESSAGE(std::string(what) << ": " << ok << " / " << cases.size());
  return ok;
}

struct RegResult { int match = 0, ok = 0, okMiss = 0, check = 0, fix = 0; };
RegResult regression10(Dir d, const char* srcFile, const char* goldFile, const char* report) {
  auto e = engine10();
  const std::vector<std::string> src = lines10(repo10() / "tests" / "regression" / srcFile);
  const std::vector<std::string> gold = lines10(repo10() / "tests" / "regression" / "expected" / goldFile);
  REQUIRE(src.size() == 50);
  REQUIRE(gold.size() == 50);
  RegResult rr;
  std::ostringstream all;
  for (size_t i = 0; i < src.size(); ++i) {
    const rules::CueOutput o = one10(*e, src[i], d);
    const std::string got = flat10(o.target);
    const bool m = matches10(got, gold[i]);
    rr.match += m;
    if (o.confidence == rules::Confidence::Ok) { ++rr.ok; rr.okMiss += !m; }
    else if (o.confidence == rules::Confidence::Check) ++rr.check;
    else ++rr.fix;
    std::string why;
    for (const auto& k : o.checks)
      if (!k.ok) why += k.id + "(" + k.detail + ") ";
    for (const auto& f : o.flags)
      if (f != "source-tokens") why += "[" + f + "] ";
    all << i + 1 << "\t" << (m ? "=" : "x") << "\t" << src[i] << "\t" << got << "\t" << confName10(o.confidence) << "\t"
        << why << "\n";
  }
  std::ofstream(buildDir10() / report) << "match " << rr.match << " / 50; ok " << rr.ok << " (mismatches among OK "
                                       << rr.okMiss << "), check " << rr.check << ", fix " << rr.fix << "\n"
                                       << all.str();
  MESSAGE(srcFile << ": " << rr.match << " / 50; ok " << rr.ok << " (mismatches among OK " << rr.okMiss << "), check "
                  << rr.check << ", fix " << rr.fix << "; report " << (buildDir10() / report).string());
  return rr;
}

}  // namespace

// Debug: sentences of a file, one cue each (VP_GRC10_TRY=<file>, VP_GRC10_DIR=en-grc | grc-en | grc-es | es-grc;
// VP_GRC10_WHY=1 adds the failed checks and the reasons).
TEST_CASE("rules-grc10: try (VP_GRC10_TRY=<file>)") {
  const char* env = std::getenv("VP_GRC10_TRY");
  if (!env || !*env) return;
  NEED_REAL10();
  const char* dv = std::getenv("VP_GRC10_DIR");
  const std::string ds = dv ? dv : "en-grc";
  const Dir d = ds == "grc-en" ? Dir::GrcEn : ds == "grc-es" ? Dir::GrcEs : ds == "es-grc" ? Dir::EsGrc : Dir::EnGrc;
  auto e = engine10();
  for (const std::string& line : lines10(env)) {
    const rules::CueOutput o = one10(*e, line, d);
    std::string extra;
    for (const auto& fl : o.flags)
      if (fl != "source-tokens") extra += " " + fl;
    std::cout << line << "\t" << flat10(o.target) << "\t" << confName10(o.confidence) << " |" << extra << "\n";
    if (std::getenv("VP_GRC10_WHY")) {
      for (const auto& k : o.checks)
        if (!k.ok || k.detail.rfind("warning", 0) == 0) std::cout << "      " << k.id << ": " << k.detail << "\n";
      if (d == Dir::EnGrc || d == Dir::EsGrc)
        for (const auto& w : o.reasons) std::cout << "      reason " << w.kind << ": " << w.text << "\n";
    }
  }
}

TEST_CASE("rules-grc10: GRC -> EN regression on own_athen.grc.txt (50 own sentences) vs the English gold") {
  NEED_REAL10();
  const RegResult r = regression10(Dir::GrcEn, "own_athen.grc.txt", "own_athen.en.gold.txt", "regression_report_athen_grc_en.txt");
  CHECK(r.okMiss == 0);
}

TEST_CASE("rules-grc10: EN -> GRC regression on own_athen.en.txt (50 own sentences) vs the Greek gold") {
  NEED_REAL10();
  const RegResult r = regression10(Dir::EnGrc, "own_athen.en.txt", "own_athen.grc.gold.txt", "regression_report_athen_en_grc.txt");
  CHECK(r.okMiss == 0);
}
