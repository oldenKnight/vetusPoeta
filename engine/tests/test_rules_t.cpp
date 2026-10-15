// engine/rules, task C43 (LA2X-4): a prepositional phrase that belongs to the subject noun in la2x / grc2x. (a) Latin:
// a phrase right after the subject of a copula clause with a predicate describes the noun when it says where to / from /
// through / about, or where a thing is ("Via ad oppidum angusta est" = the road to the town is narrow); a person with a
// place has two readings (Check, flag pp-attach); verbs of motion and position keep their phrases; mīlle the numeral
// goes with a plural noun; (b) Greek: the attributive position (article + phrase + noun, noun + article + phrase) and
// the same rule for an unbracketed phrase; arrive at, sail to, on the island, κάθημαι = sit; (c) EN / ES -> LA
// "..., not very but enough" = "..., nōn valdē sed satis". Own sentences only: the cases below and
// tests/regression/own_pp*.txt with their gold (written before any C43 change). Cases that need the real data
// (VP_DATA_WORK) skip with a message.
#include <algorithm>
#include <doctest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "vp/engine_config.h"
#include "vp/la2x.h"
#include "vp/lex.h"
#include "vp/rules.h"
#include "vp/text.h"

namespace stdfs = std::filesystem;
using namespace vp;

namespace {

stdfs::path repoT() { return stdfs::path(VP_FIXTURES_DIR).parent_path().parent_path(); }
stdfs::path workT() {
  const char* e = std::getenv("VP_DATA_WORK");
  return e && *e ? stdfs::path(e) : repoT() / "data" / "work";
}
stdfs::path buildT() { return stdfs::path(VP_TEST_TMP).parent_path(); }

struct RealT {
  lex::Lexicon la, grc, en, es;
  bool ok = false;
  std::string why;
};
const RealT& realT() {
  static RealT r = [] {
    RealT x;
    auto la = lex::Lexicon::open(workT() / "latin.vpl");
    auto grc = lex::Lexicon::open(workT() / "greek.vpl");
    auto en = lex::Lexicon::open(workT() / "english.vpl");
    auto es = lex::Lexicon::open(workT() / "spanish.vpl");
    const bool nlp = stdfs::exists(workT() / "nlp" / "english.tag.vpt") && stdfs::exists(workT() / "nlp" / "spanish.tag.vpt");
    if (!la.ok() || !grc.ok() || !en.ok() || !es.ok() || !nlp) {
      x.why = !la.ok() ? la.error().message : !grc.ok() ? grc.error().message : !en.ok() ? en.error().message
              : !es.ok() ? es.error().message : "no nlp/*.tag.vpt";
      return x;
    }
    x.la = std::move(la.value());
    x.grc = std::move(grc.value());
    x.en = std::move(en.value());
    x.es = std::move(es.value());
    x.ok = true;
    return x;
  }();
  return r;
}
#define NEED_REALT()                                                                             \
  if (!realT().ok) {                                                                             \
    MESSAGE("real data not available (" << realT().why << "); set VP_DATA_WORK. Skipped.");      \
    return;                                                                                      \
  }

std::unique_ptr<rules::Engine> engineT() {
  rules::EngineConfig cfg = rules::defaultEngineConfig();
  cfg.curatedDir = (repoT() / "data" / "curated").string();
  cfg.dataDir = workT().string();
  cfg.nlpDir = (workT() / "nlp").string();
  auto e = rules::makeEngine(cfg);
  const RealT& r = realT();
  e->setLexicons(&r.la, &r.grc, &r.en, &r.es);
  return e;
}

rules::Options optT(rules::Lang s, rules::Lang t) {
  rules::Options o;
  o.source = s;
  o.target = t;
  o.fidelity = 2;
  o.speakerGender = 'm';
  o.textProject = true;
  return o;
}

std::vector<rules::CueOutput> runT(rules::Engine& e, const std::vector<std::string>& units, const rules::Options& o) {
  std::vector<rules::CueInput> in(units.size());
  for (size_t i = 0; i < units.size(); ++i) {
    in[i].index = (uint32_t)i;
    in[i].sourceText = units[i];
  }
  auto r = e.translate(in, o, rules::Context{}, nullptr, nullptr);
  REQUIRE(r.ok());
  return r.value();
}
rules::CueOutput oneT(rules::Engine& e, const std::string& s, const rules::Options& o) { return runT(e, {s}, o)[0]; }

std::string confT(rules::Confidence c) {
  return c == rules::Confidence::Ok ? "ok" : c == rules::Confidence::Check ? "check" : "fix";
}
bool hasFlag(const rules::CueOutput& r, const std::string& f) {
  for (const std::string& x : r.flags)
    if (x == f) return true;
  return false;
}
bool failedCheck(const rules::CueOutput& r, const std::string& id) {
  for (const rules::Check& c : r.checks)
    if (c.id == id && !c.ok) return true;
  return false;
}

// the gold comparison of tools/eval/evallib.py (as test_rules_r.cpp)
std::string normT(const std::string& s0) {
  const std::string s = text::lower(text::nfd(s0));
  std::string out;
  size_t i = 0;
  while (i < s.size()) {
    size_t j = i;
    const char32_t c = text::decodeUtf8(s, j);
    if (j == i) j = i + 1;
    const bool mark = c >= 0x300 && c <= 0x36F;
    const bool punct = c == '.' || c == ',' || c == ';' || c == '!' || c == '?' || c == ':' || c == '"' || c == '\'' ||
                       c == '-' || c == '(' || c == ')' || c == 0xB7 || c == 0x387 || c == 0x37E || c == 0xA1 ||
                       c == 0xBF || c == 0xAB || c == 0xBB || c == 0x201C || c == 0x201D || c == 0x2019 || c == '\n';
    if (mark) {
    } else if (punct || c == ' ' || c == '\t') {
      if (!out.empty() && out.back() != ' ') out += ' ';
    } else if (c == 'v') {
      out += 'u';
    } else if (c == 'j') {
      out += 'i';
    } else {
      out.append(s, i, j - i);
    }
    i = j;
  }
  while (!out.empty() && out.back() == ' ') out.pop_back();
  return out;
}
bool matchT(const std::string& got, const std::string& gold) {
  for (size_t a = 0;;) {
    const size_t b = gold.find(" | ", a);
    if (normT(got) == normT(gold.substr(a, b == std::string::npos ? std::string::npos : b - a))) return true;
    if (b == std::string::npos) return false;
    a = b + 3;
  }
}
std::vector<std::string> linesT(const stdfs::path& p) {
  std::vector<std::string> out;
  std::ifstream in(p);
  std::string l;
  while (std::getline(in, l)) {
    if (!l.empty() && l.back() == '\r') l.pop_back();
    if (!l.empty() && l[0] != '#') out.push_back(l);
  }
  return out;
}

struct PairT { const char* src; const char* out; };

void exactT(rules::Engine& e, const std::vector<PairT>& cases, const rules::Options& o) {
  for (const PairT& c : cases) {
    const rules::CueOutput r = oneT(e, c.src, o);
    CHECK_MESSAGE(r.target == c.out, c.src << " -> " << r.target << " (" << confT(r.confidence) << ")");
    CHECK(r.confidence != rules::Confidence::Fix);
  }
}

}  // namespace

// ---- (a) Latin ---------------------------------------------------------------------------------------------------------
TEST_CASE("rules-t: la2x a prepositional phrase after the subject noun") {
  NEED_REALT();
  auto e = engineT();
  exactT(*e,
         {{"Via ad oppidum angusta est.", "The road to the town is narrow."},
          {"Carmen dē patriā pulchrum est.", "The song about the homeland is beautiful."},
          {"Aqua in flūmine frīgida est.", "The water in the river is cold."},          // the phrase makes it definite
          {"Mīlle nautae in portū sunt.", "A thousand sailors are in the harbor."},     // was "of the mile"
          // unchanged: the place as the predicate, a verb of motion, another verb
          {"Classis Rōmāna in portū est.", "The Roman fleet is in the harbor."},
          {"Nauta ad īnsulam nāvigat.", "The sailor sails to the island."},
          {"Puer in hortō librum legit.", "The boy reads a book in the garden."}},
         optT(rules::Lang::La, rules::Lang::En));
  exactT(*e,
         {{"Via ad oppidum angusta est.", "El camino al pueblo es estrecho."},
          {"Carmen dē patriā pulchrum est.", "La canción sobre la patria es bonita."},
          {"Mīlle nautae in portū sunt.", "Mil marineros están en el puerto."},
          {"Classis Rōmāna in portū est.", "La flota romana está en el puerto."}},
         optT(rules::Lang::La, rules::Lang::Es));
  // a person with a place: two readings, Check with the pp-attach check
  for (rules::Lang t : {rules::Lang::En, rules::Lang::Es}) {
    const rules::CueOutput r = oneT(*e, "Agricola in agrō laetus est.", optT(rules::Lang::La, t));
    CHECK(r.confidence == rules::Confidence::Check);
    CHECK(hasFlag(r, "pp-attach"));
    CHECK(failedCheck(r, "pp-attach"));
  }
}

// ---- (b) Greek ---------------------------------------------------------------------------------------------------------
TEST_CASE("rules-t: grc2x attributive position and a phrase after the subject noun") {
  NEED_REALT();
  auto e = engineT();
  exactT(*e,
         {{"ὁ ἐν τῷ ἀγρῷ λίθος μέγας ἐστίν.", "The stone in the field is big."},           // article + phrase + noun
          {"ὁ ποταμὸς ὁ διὰ τῆς χώρας μακρός ἐστιν.", "The river through the country is long."},   // repeated article
          {"ἡ ἐπιστολὴ ἐκ τῆς πόλεως μακρά ἐστιν.", "The letter from the city is long."},   // unbracketed
          {"ἡ ναῦς εἰς τὸν λιμένα ἀφικνεῖται.", "The ship arrives at the harbor."},        // was "arrives into"
          {"ὁ γέρων ἐν τῇ οἰκίᾳ κάθηται.", "The old man sits in the house."},             // was "has been seated"
          {"ὁ δοῦλος εἰς τὴν οἰκίαν τρέχει.", "The slave runs into the house."}},          // motion keeps its phrase
         optT(rules::Lang::Grc, rules::Lang::En));
  exactT(*e,
         {{"ὁ ἐν τῷ ἀγρῷ λίθος μέγας ἐστίν.", "La piedra en el campo es grande."},
          {"ἡ ἐπιστολὴ ἐκ τῆς πόλεως μακρά ἐστιν.", "La carta de la ciudad es larga."},
          {"ὁ γέρων ἐν τῇ οἰκίᾳ κάθηται.", "El viejo está sentado en la casa."}},
         optT(rules::Lang::Grc, rules::Lang::Es));
  for (rules::Lang t : {rules::Lang::En, rules::Lang::Es}) {
    const rules::CueOutput r = oneT(*e, "ὁ παῖς ἐν τῷ ἀγρῷ εὐδαίμων ἐστίν.", optT(rules::Lang::Grc, t));
    CHECK(r.confidence == rules::Confidence::Check);
    CHECK(hasFlag(r, "pp-attach"));
  }
}

// ---- (c) EN / ES -> LA "..., not very but enough" -----------------------------------------------------------------------
TEST_CASE("rules-t: EN / ES -> LA a degree tail is nōn X sed Y") {
  NEED_REALT();
  auto e = engineT();
  CHECK(oneT(*e, "The house is big, not very but enough.", optT(rules::Lang::En, rules::Lang::La)).target ==
        "Domus magna est, nōn valdē sed satis.");
  CHECK(oneT(*e, "La casa es grande, no muy, sino bastante.", optT(rules::Lang::Es, rules::Lang::La)).target ==
        "Domus magna est, nōn valdē sed satis.");
}

// ---- own material end to end (text project, one job per file) --------------------------------------------------------
TEST_CASE("rules-t: own material vs gold (tests/regression/own_pp*.txt; report regression_report_t.txt)") {
  NEED_REALT();
  auto e = engineT();
  // checkOnly: units with two readings, right only as Check (an OK there counts as a mismatch among OK)
  struct F { const char* src; const char* gold; rules::Lang s, t; int floor; std::vector<int> checkOnly; };
  // floors: the counts after C43 (first run before any change: 8 / 17, 6 / 17, 2 / 13, 3 / 13, 1 / 2)
  const F files[] = {
      {"own_pp.la.txt", "own_pp.en.gold.txt", rules::Lang::La, rules::Lang::En, 17, {12, 13, 14}},
      {"own_pp.la.txt", "own_pp.es.gold.txt", rules::Lang::La, rules::Lang::Es, 16, {12, 13, 14}},
      {"own_pp_grc.grc.txt", "own_pp_grc.en.gold.txt", rules::Lang::Grc, rules::Lang::En, 13, {10, 11}},
      {"own_pp_grc.grc.txt", "own_pp_grc.es.gold.txt", rules::Lang::Grc, rules::Lang::Es, 13, {10, 11}},
      {"own_pp.en.txt", "own_pp.la.gold.txt", rules::Lang::En, rules::Lang::La, 2, {}},
  };
  std::ostringstream rep;
  for (const F& f : files) {
    const std::vector<std::string> src = linesT(repoT() / "tests" / "regression" / f.src);
    const std::vector<std::string> gold = linesT(repoT() / "tests" / "regression" / "expected" / f.gold);
    REQUIRE(src.size() == gold.size());
    const rules::Options o = optT(f.s, f.t);
    const std::vector<rules::CueOutput> out = runT(*e, src, o);
    REQUIRE(out.size() == src.size());
    int right = 0, ok = 0, okMiss = 0;
    rep << "== " << f.src << " -> " << f.gold << "\n";
    for (size_t i = 0; i < src.size(); ++i) {
      bool m = matchT(out[i].target, gold[i]);
      const bool two = std::find(f.checkOnly.begin(), f.checkOnly.end(), (int)i + 1) != f.checkOnly.end();
      if (two && out[i].confidence == rules::Confidence::Ok) m = false;
      right += m;
      if (out[i].confidence == rules::Confidence::Ok) {
        ++ok;
        okMiss += !m;
      }
      rep << (i + 1) << "\t" << (m ? "=" : "x") << "\t" << out[i].target << "\t" << confT(out[i].confidence) << "\n";
    }
    rep << "right " << right << " / " << src.size() << "; ok " << ok << " (mismatches among OK " << okMiss << ")\n";
    MESSAGE(std::string(f.gold) << ": right " << right << " / " << src.size() << ", ok " << ok << ", mismatches among OK " << okMiss);
    CHECK(right >= f.floor);
    CHECK(okMiss == 0);
    const std::vector<rules::CueOutput> again = runT(*e, src, o);   // deterministic
    for (size_t i = 0; i < src.size(); ++i) CHECK(again[i].target == out[i].target);
  }
  std::ofstream(buildT() / "regression_report_t.txt") << rep.str();
}
