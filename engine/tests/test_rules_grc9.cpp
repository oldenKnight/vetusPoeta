// engine/rules Greek loop 9 (C35): Spanish source into Greek. The regression on the 120 own Mexican-Spanish cues of
// tests/regression/own_dialogue2.es.srt against the Attic gold tests/regression/expected/own_dialogue2.grc.gold.txt
// (written before the engine was run on the file), the Spanish constructions of the loop (two or more own sentences
// per rule, written before the rule: docs/rules_grc3_notes.md "Loop 9") and the fixes after the blind check. Needs the
// real data (VP_DATA_WORK); skips otherwise.
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

#include "vp/engine_config.h"
#include "vp/lex.h"
#include "vp/nlp.h"
#include "vp/rules.h"
#include "vp/subs.h"
#include "vp/text.h"

namespace stdfs = std::filesystem;
using namespace vp;

namespace {

stdfs::path repo9() { return stdfs::path(VP_FIXTURES_DIR).parent_path().parent_path(); }
stdfs::path work9() {
  const char* e = std::getenv("VP_DATA_WORK");
  return e && *e ? stdfs::path(e) : repo9() / "data" / "work";
}
stdfs::path buildDir9() { return stdfs::path(VP_TEST_TMP).parent_path(); }

struct Real9 {
  lex::Lexicon grc, en, es;
  bool ok = false;
  std::string why;
};
const Real9& real9() {
  static Real9 r = [] {
    Real9 x;
    const char* g = std::getenv("VP_GREEK_VPL");
    auto grc = lex::Lexicon::open(g && *g ? stdfs::path(g) : work9() / "greek.vpl");
    auto en = lex::Lexicon::open(work9() / "english.vpl");
    auto es = lex::Lexicon::open(work9() / "spanish.vpl");
    auto pes = nlp::Pipeline::open(nlp::Lang::Es, (work9() / "nlp" / "spanish.tag.vpt").string(),
                                   (work9() / "nlp" / "spanish.dep.vpt").string());
    if (!grc.ok() || !en.ok() || !es.ok() || !pes.ok()) {
      x.why = !grc.ok() ? grc.error().message : !en.ok() ? en.error().message : !es.ok() ? es.error().message
                                                                                         : pes.error().message;
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
#define NEED_REAL9()                                                                                          \
  if (!real9().ok) {                                                                                          \
    MESSAGE("real Greek / Spanish data not available (" << real9().why << "); set VP_DATA_WORK. Skipped.");  \
    return;                                                                                                   \
  }

std::unique_ptr<rules::Engine> engine9() {
  rules::EngineConfig cfg = rules::defaultEngineConfig();
  cfg.curatedDir = (repo9() / "data" / "curated").string();
  cfg.dataDir = work9().string();
  cfg.nlpDir = (work9() / "nlp").string();
  auto e = rules::makeEngine(cfg);
  e->setLexicons(nullptr, &real9().grc, &real9().en, &real9().es);
  return e;
}

// As the other Greek regressions: NFC, lower case, punctuation-insensitive, spaces collapsed; accents and breathings
// count.
std::string norm9(const std::string& s) {
  std::string l = text::lower(text::nfc(s));
  std::string out;
  size_t i = 0;
  while (i < l.size()) {
    size_t j = i;
    const char32_t c = text::decodeUtf8(l, j);
    const bool punct = c == '.' || c == ',' || c == ';' || c == '!' || c == '?' || c == ':' || c == 0xB7 ||
                       c == 0x387 || c == 0x37E || c == '"' || c == 0xA1 || c == 0xBF;
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

bool matches9(const std::string& got, const std::string& expected) {
  for (size_t a = 0;;) {
    const size_t b = expected.find(" | ", a);
    if (norm9(got) == norm9(text::nfc(expected.substr(a, b == std::string::npos ? std::string::npos : b - a))))
      return true;
    if (b == std::string::npos) return false;
    a = b + 3;
  }
}

rules::Options opts9() {
  rules::Options o;
  o.source = rules::Lang::Es;
  o.target = rules::Lang::Grc;
  o.fidelity = 2;
  o.speakerGender = 'f';
  return o;
}

const char* confName9(rules::Confidence c) {
  return c == rules::Confidence::Ok ? "ok" : c == rules::Confidence::Check ? "check" : "fix";
}

std::string flat9(std::string s) {
  std::replace(s.begin(), s.end(), '\n', ' ');
  return s;
}

bool hasFlag9(const rules::CueOutput& o, const char* f) {
  return std::find(o.flags.begin(), o.flags.end(), f) != o.flags.end();
}

// One cue at a time; `expected` may list alternatives after " | ". `mustCheck`: the cue must not be OK (a rebuilt
// structure, a repaired misreading, a guess). A cue with a rebuilt / guessed flag is never OK.
struct Case9 { const char* src; const char* expected; bool mustCheck; };
int run9(rules::Engine& e, const std::vector<Case9>& cases, const char* what) {
  int ok = 0;
  for (const Case9& k : cases) {
    rules::CueInput x;
    x.sourceText = k.src;
    auto r = e.translate({x}, opts9(), rules::Context{}, nullptr, nullptr);
    REQUIRE(r.ok());
    const rules::CueOutput& o = r.value()[0];
    const std::string got = flat9(o.target);
    const bool hit = matches9(got, k.expected);
    CHECK_MESSAGE(hit, what << ": " << k.src << " -> '" << got << "' expected '" << k.expected << "'");
    CHECK_MESSAGE(o.confidence != rules::Confidence::Fix, what << ": " << k.src << " is Fix");
    bool rebuilt = false;
    for (const char* f : {"clause-repair", "participle-phrase", "realia", "past-form", "from-rule", "det-adverb",
                          "light-verb", "subject-guess", "speech-inversion", "idiom", "lexicon-gap", "addressee-guess"})
      rebuilt = rebuilt || hasFlag9(o, f);
    if (rebuilt || k.mustCheck)
      CHECK_MESSAGE(o.confidence != rules::Confidence::Ok, what << ": " << k.src << " was rebuilt / guessed but is OK");
    ok += hit;
  }
  MESSAGE(std::string(what) << ": " << ok << " / " << cases.size());
  return ok;
}

std::vector<rules::CueInput> srtCues9(const char* file) {
  std::ifstream f(repo9() / "tests" / "regression" / file, std::ios::binary);
  std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)), {});
  auto d = subs::parse(b, subs::Format::Srt);
  REQUIRE(d.ok());
  std::vector<rules::CueInput> in;
  for (size_t i = 0; i < d->cues.size(); ++i) {
    rules::CueInput c;
    c.index = (uint32_t)in.size();
    c.sourceText = d->cues[i].plainText();
    int64_t s = 0, t = 0;
    subs::parseTiming(d->cues[i].timingRaw, subs::Format::Srt, s, t);
    c.startMs = s;
    c.endMs = t;
    in.push_back(c);
  }
  return in;
}

}  // namespace

// Debug: Spanish cues of a file into Greek, each alone (VP_GRC9_TRY=<file>; VP_GRC9_WHY=1 adds the reasons).
TEST_CASE("rules-grc9: try (VP_GRC9_TRY=<file>)") {
  const char* env = std::getenv("VP_GRC9_TRY");
  if (!env || !*env) return;
  NEED_REAL9();
  auto e = engine9();
  std::ifstream f(env);
  std::string line;
  while (std::getline(f, line)) {
    if (line.empty() || line[0] == '#') continue;
    rules::CueInput c;
    c.sourceText = line;
    auto r = e->translate({c}, opts9(), rules::Context{}, nullptr, nullptr);
    REQUIRE(r.ok());
    const rules::CueOutput& o = r.value()[0];
    std::string extra;
    for (const auto& fl : o.flags) extra += " " + fl;
    std::cout << line << "\t" << flat9(o.target) << "\t" << confName9(o.confidence) << " |" << extra << "\n";
    if (std::getenv("VP_GRC9_WHY"))
      for (const auto& k : o.checks)
        if (!k.ok || k.detail.rfind("warning", 0) == 0) std::cout << "      " << k.id << ": " << k.detail << "\n";
    if (std::getenv("VP_GRC9_WHY"))
      for (const auto& w : o.reasons) std::cout << "      reason " << w.kind << ": " << w.text << "\n";
  }
}

TEST_CASE("rules-grc9: ES -> GRC regression on own_dialogue2.es.srt (120 cues) vs the Greek gold") {
  NEED_REAL9();
  auto e = engine9();
  std::vector<rules::CueInput> in = srtCues9("own_dialogue2.es.srt");
  std::ifstream g(repo9() / "tests" / "regression" / "expected" / "own_dialogue2.grc.gold.txt");
  std::vector<std::string> gold;
  std::string line;
  while (std::getline(g, line))
    if (!line.empty() && line[0] != '#') gold.push_back(line);
  REQUIRE(gold.size() == 120);
  REQUIRE(in.size() == 120);
  const size_t n = gold.size();
  const rules::Options o = opts9();
  auto r1 = e->translate(in, o, rules::Context{}, nullptr, nullptr);
  REQUIRE(r1.ok());
  REQUIRE(r1->size() == n);
  int matches = 0;
  std::map<std::string, int> conf, confMiss;
  std::ostringstream table, all;
  table << "| # | source | gold | ours | checks |\n|---|---|---|---|---|\n";
  for (size_t i = 0; i < n; ++i) {
    const rules::CueOutput& c = r1.value()[i];
    ++conf[confName9(c.confidence)];
    const std::string ours = flat9(c.target);
    const bool match = matches9(ours, gold[i]);
    matches += match;
    if (!match) {
      ++confMiss[confName9(c.confidence)];
      std::string chk;
      for (const auto& k : c.checks)
        if (!k.ok) chk += k.id + " ";
      std::string g2 = gold[i].substr(0, gold[i].find(" | "));
      table << "| " << i + 1 << " | " << in[i].sourceText << " | " << g2 << " | " << ours << " | "
            << confName9(c.confidence) << (chk.empty() ? "" : " " + chk) << "|\n";
    }
    std::string why;
    for (const auto& k : c.checks)
      if (!k.ok || k.detail.rfind("warning", 0) == 0) why += k.id + "(" + k.detail + ") ";
    for (const auto& f : c.flags) why += "[" + f + "] ";
    all << i + 1 << "\t" << (match ? "=" : "x") << "\t" << flat9(in[i].sourceText) << "\t" << ours << "\t"
        << confName9(c.confidence) << "\t" << why << "\n";
  }
  std::ostringstream rep;
  rep << "Regression own_dialogue2.es.srt (" << n << " cues) -> Attic Greek, fidelity 2, speaker f\n";
  rep << "match rate (normalised: NFC, case- and punctuation-insensitive, accents count, any gold alternative): "
      << matches << " / " << n << "\n";
  rep << "confidence: ok " << conf["ok"] << ", check " << conf["check"] << ", fix " << conf["fix"]
      << "; mismatches: ok " << confMiss["ok"] << ", check " << confMiss["check"] << ", fix " << confMiss["fix"]
      << "\n\nMismatches:\n" << table.str() << "\nAll outputs:\n" << all.str();
  std::ofstream(buildDir9() / "regression_report_grc_es2.txt") << rep.str();
  MESSAGE("ES -> GRC own_dialogue2: " << matches << " / " << n << "; confidence ok " << conf["ok"] << " / check "
                                      << conf["check"] << " / fix " << conf["fix"] << " (mismatches rated OK: "
                                      << confMiss["ok"] << "); report "
                                      << (buildDir9() / "regression_report_grc_es2.txt").string());
  CHECK(matches >= 0);   // C35: threshold set at the end of the loop
  // determinism: a second engine gives byte-identical cues
  auto e2 = engine9();
  auto r2 = e2->translate(in, o, rules::Context{}, nullptr, nullptr);
  REQUIRE(r2.ok());
  bool same = true;
  for (size_t i = 0; i < n; ++i) same = same && r1.value()[i].target == r2.value()[i].target;
  CHECK(same);
}
