// engine/rules, task C41 (LEX-AUDIT): core vocabulary audit of the reading directions (la2x, grc2x) and their
// confidence. (a) the gloss calibration: a content word whose gloss comes from no readable row and is not the lexicon's
// own first sense (a Spanish gloss via the English pivot, a followed "synonym of"), or that is not a dictionary word of
// the target language (la2x::glossIsDictionaryWord), makes the cue Check (flag gloss-doubt, check "gloss"); (b) the
// readable rows written for the DCC core lists (readable_en / readable_es / readable_grc), with the small realiser
// fixes the audit sentences needed (pl: notes, longē ā, πολλοί / ὀλίγοι, the possessive adjectives, the genitive of
// comparison, δέδοικα, the generic article only in present statements, -iar verbs, -gión nouns); (c) EN -> LA
// "..., not very but enough" is Check, never OK. Own sentences only: the cases below and tests/regression/own_core*.txt
// with their gold (written before any C41 change). Cases that need the real data (VP_DATA_WORK) skip with a message.
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

stdfs::path repoS() { return stdfs::path(VP_FIXTURES_DIR).parent_path().parent_path(); }
stdfs::path workS() {
  const char* e = std::getenv("VP_DATA_WORK");
  return e && *e ? stdfs::path(e) : repoS() / "data" / "work";
}
stdfs::path buildS() { return stdfs::path(VP_TEST_TMP).parent_path(); }

struct RealS {
  lex::Lexicon la, grc, en, es;
  bool ok = false;
  std::string why;
};
const RealS& realS() {
  static RealS r = [] {
    RealS x;
    auto la = lex::Lexicon::open(workS() / "latin.vpl");
    auto grc = lex::Lexicon::open(workS() / "greek.vpl");
    auto en = lex::Lexicon::open(workS() / "english.vpl");
    auto es = lex::Lexicon::open(workS() / "spanish.vpl");
    const bool nlp = stdfs::exists(workS() / "nlp" / "english.tag.vpt") && stdfs::exists(workS() / "nlp" / "spanish.tag.vpt");
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
#define NEED_REALS()                                                                             \
  if (!realS().ok) {                                                                             \
    MESSAGE("real data not available (" << realS().why << "); set VP_DATA_WORK. Skipped.");      \
    return;                                                                                      \
  }

std::unique_ptr<rules::Engine> engineS() {
  rules::EngineConfig cfg = rules::defaultEngineConfig();
  cfg.curatedDir = (repoS() / "data" / "curated").string();
  cfg.dataDir = workS().string();
  cfg.nlpDir = (workS() / "nlp").string();
  auto e = rules::makeEngine(cfg);
  const RealS& r = realS();
  e->setLexicons(&r.la, &r.grc, &r.en, &r.es);
  return e;
}

rules::Options optS(rules::Lang s, rules::Lang t) {
  rules::Options o;
  o.source = s;
  o.target = t;
  o.fidelity = 2;
  o.speakerGender = 'm';
  o.textProject = true;
  return o;
}

std::vector<rules::CueOutput> runS(rules::Engine& e, const std::vector<std::string>& units, const rules::Options& o) {
  std::vector<rules::CueInput> in(units.size());
  for (size_t i = 0; i < units.size(); ++i) {
    in[i].index = (uint32_t)i;
    in[i].sourceText = units[i];
  }
  auto r = e.translate(in, o, rules::Context{}, nullptr, nullptr);
  REQUIRE(r.ok());
  return r.value();
}
rules::CueOutput oneS(rules::Engine& e, const std::string& s, const rules::Options& o) { return runS(e, {s}, o)[0]; }

std::string confS(rules::Confidence c) {
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
std::string normS(const std::string& s0) {
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
bool matchS(const std::string& got, const std::string& gold) {
  for (size_t a = 0;;) {
    const size_t b = gold.find(" | ", a);
    if (normS(got) == normS(gold.substr(a, b == std::string::npos ? std::string::npos : b - a))) return true;
    if (b == std::string::npos) return false;
    a = b + 3;
  }
}
std::vector<std::string> linesS(const stdfs::path& p) {
  std::vector<std::string> out;
  std::ifstream in(p);
  std::string l;
  while (std::getline(in, l)) {
    if (!l.empty() && l.back() == '\r') l.pop_back();
    if (!l.empty() && l[0] != '#') out.push_back(l);
  }
  return out;
}

struct PairS { const char* src; const char* out; };

void exactS(rules::Engine& e, const std::vector<PairS>& cases, const rules::Options& o) {
  for (const PairS& c : cases) {
    const rules::CueOutput r = oneS(e, c.src, o);
    CHECK_MESSAGE(r.target == c.out, c.src << " -> " << r.target << " (" << confS(r.confidence) << ")");
    CHECK(r.confidence != rules::Confidence::Fix);
  }
}

}  // namespace

// ---- (a) the dictionary-word test of a gloss --------------------------------------------------------------------------
TEST_CASE("rules-s: glossIsDictionaryWord (english.vpl / spanish.vpl)") {
  CHECK(la2x::glossIsDictionaryWord(nullptr, "not a lie", false));   // nothing to check against
  NEED_REALS();
  const lex::Lexicon* en = &realS().en;
  const lex::Lexicon* es = &realS().es;
  for (const char* g : {"truth", "look for", "go away", "to say", "be glad", "the soldier", "well-known", "at last"})
    CHECK_MESSAGE(la2x::glossIsDictionaryWord(en, g, false), g);
  for (const char* g : {"not a lie", "usually weaker than βούλομαι", "cause someone to experience something",
                        "the legal machinery of a state", "help somebody", "[agō]", "xqzt"})
    CHECK_MESSAGE(!la2x::glossIsDictionaryWord(en, g, false), g);
  for (const char* g : {"verdad", "estar lejos", "tener miedo", "por fin", "la ciudad"})
    CHECK_MESSAGE(la2x::glossIsDictionaryWord(es, g, true), g);
  for (const char* g : {"dispuesen", "no es mentira", "hacer algo a alguien", "consuetud bárbara xqzt"})
    CHECK_MESSAGE(!la2x::glossIsDictionaryWord(es, g, true), g);
}

// ---- (a) the calibration on whole cues ------------------------------------------------------------------------------
TEST_CASE("rules-s: a gloss no teacher wrote that is not a first sense or not a word makes the cue Check") {
  NEED_REALS();
  auto e = engineS();
  // Latin -> Spanish: sīmia has no readable row and its Spanish gloss came through the English pivot
  {
    const rules::CueOutput r = oneS(*e, "Puella sīmiam videt.", optS(rules::Lang::La, rules::Lang::Es));
    CHECK_MESSAGE(r.confidence == rules::Confidence::Check, r.target);
    CHECK(hasFlag(r, "gloss-doubt"));
    CHECK(failedCheck(r, "gloss"));
    const rules::CueOutput en = oneS(*e, "Puella sīmiam videt.", optS(rules::Lang::La, rules::Lang::En));
    CHECK(!hasFlag(en, "gloss-doubt"));   // the English gloss is the lexicon's own first sense and a word
  }
  // Greek -> Spanish: πήρα likewise (via English); Greek -> English is fine
  {
    const rules::CueOutput r = oneS(*e, "ἡ γυνὴ τὴν πήραν φέρει.", optS(rules::Lang::Grc, rules::Lang::Es));
    CHECK_MESSAGE(r.confidence == rules::Confidence::Check, r.target);
    CHECK(hasFlag(r, "gloss-doubt"));
    CHECK(failedCheck(r, "gloss"));
    const rules::CueOutput en = oneS(*e, "ἡ γυνὴ τὴν πήραν φέρει.", optS(rules::Lang::Grc, rules::Lang::En));
    CHECK(!hasFlag(en, "gloss-doubt"));
  }
  // a word with a teacher's row is never doubted (κύριος: "castellano" before the row)
  for (bool es : {false, true}) {
    const rules::CueOutput r = oneS(*e, "ὁ κύριος τῷ δούλῳ ἄρτον δίδωσιν.",
                                    optS(rules::Lang::Grc, es ? rules::Lang::Es : rules::Lang::En));
    CHECK_MESSAGE(r.target == (es ? "El señor da pan al esclavo." : "The lord gives bread to the slave."), r.target);
    CHECK(!hasFlag(r, "gloss-doubt"));
    CHECK(r.confidence == rules::Confidence::Ok);
  }
}

// ---- (b) the core rows and the realiser fixes ----------------------------------------------------------------------
TEST_CASE("rules-s: Latin core rows (la2x) and the audit's realiser fixes") {
  NEED_REALS();
  auto e = engineS();
  exactS(*e,
         {{"Lēgēs urbem servant.", "The laws save a city."},                      // was "The legal machineries of a state"
          {"Rōmulus urbem condidit.", "Romulus founded a city."},                  // was "put together"
          {"Templum in monte stat.", "The temple stands on the mountain."},        // note on: in + ablative
          {"Cōpiae in castrīs sunt.", "The troops are in the camp."},              // pl:troops / pl:camp
          {"Legiō longē ā castrīs manet.", "The legion stays far from the camp."}, // longē ā = far from
          {"Procul ā patriā vīvit.", "He lives far from the homeland."},
          {"Mīlitēs ducem verentur.", "The soldiers fear a leader."}},
         optS(rules::Lang::La, rules::Lang::En));
  exactS(*e,
         {{"Cōpiae in castrīs sunt.", "Las tropas están en el campamento."},
          {"Legiō longē ā castrīs manet.", "La legión se queda lejos del campamento."},   // -gión is feminine
          {"Māter fīliōs suōs dīligit.", "La madre ama a sus hijos."}},                    // was "elige"
         optS(rules::Lang::La, rules::Lang::Es));
}

TEST_CASE("rules-s: Greek core rows (grc2x) and the audit's realiser fixes") {
  NEED_REALS();
  auto e = engineS();
  exactS(*e,
         {{"οἱ σοφοὶ τὴν ἀλήθειαν ζητοῦσιν.", "The wise men look for the truth."},   // was "look for the not a lie"
          {"ὁ ἀνὴρ πολλοὺς λόγους λέγει.", "The man says many words."},               // was "much words"
          {"ὁ ἐμὸς πατὴρ τὸν σὸν ἀδελφὸν ὁρᾷ.", "My father sees your brother."},      // was "the my father"
          {"ἡ ἡμετέρα οἰκία μικρά ἐστιν.", "Our house is small."},
          {"ὁ δοῦλος κρείττων ἐστὶ τοῦ δεσπότου.", "The slave is stronger than the master."},
          {"τὸν λύκον δέδοικα.", "I fear the wolf."},                                 // was "I have done the wolf."
          {"ἡ εἰρήνη καλή ἐστιν.", "Peace is beautiful."},                            // generic in a present statement
          {"ὁ κῆρυξ τὴν εἰρήνην ἤγγειλεν.", "The herald announced the peace."},       // a past event keeps "the"
          {"ἡ δίκη τοὺς ἀδίκους κολάζει.", "Justice punishes the unjust."},           // never "the unjusts"
          {"ἡ ψυχὴ ἀθάνατός ἐστιν.", "The soul is immortal."}},
         optS(rules::Lang::Grc, rules::Lang::En));
  exactS(*e,
         {{"ὁ ἀνὴρ πολλοὺς λόγους λέγει.", "El hombre dice muchas palabras."},          // was "palabras muchas"
          {"ὁ ἐμὸς πατὴρ τὸν σὸν ἀδελφὸν ὁρᾷ.", "Mi padre ve a tu hermano."},          // was "el padre mina"
          {"ἡ ἡμετέρα οἰκία μικρά ἐστιν.", "Nuestra casa es pequeña."},
          {"ὁ δοῦλος κρείττων ἐστὶ τοῦ δεσπότου.", "El esclavo es más fuerte que el amo."},
          {"ἡ δίκη τοὺς ἀδίκους κολάζει.", "La justicia castiga a los injustos."},     // a person: personal "a"
          {"ὁ ἀδελφὸς τὴν ἐπιστολὴν τῇ μητρὶ πέμπει.", "El hermano envía la carta a la madre."},   // was "envia"
          {"ὁ σοφὸς τῇ ἀληθείᾳ πιστεύει.", "El sabio confía en la verdad."}},
         optS(rules::Lang::Grc, rules::Lang::Es));
}

// ---- (c) EN -> LA "..., not very but enough" ----------------------------------------------------------------------------
// C43 changed this expectation: the Latin itself is now right ("Domus magna est, nōn valdē sed satis."), so the unit may
// be OK; it is never OK with a wrong Latin (valdē without nōn said the opposite in C41).
TEST_CASE("rules-s: EN -> LA \"not very but enough\" is OK only when the Latin says nōn valdē sed satis") {
  NEED_REALS();
  auto e = engineS();
  for (const char* s : {"The house is big, not very but enough.", "The road is long, not very but enough.",
                        "The farmer is rich, not very but enough."}) {
    const rules::CueOutput r = oneS(*e, s, optS(rules::Lang::En, rules::Lang::La));
    CHECK_MESSAGE((r.confidence != rules::Confidence::Ok || r.target.find(", nōn valdē sed satis.") != std::string::npos),
                  s << " -> " << r.target);
    CHECK_MESSAGE(r.target.find("nōn valdē sed satis") != std::string::npos, s << " -> " << r.target);
  }
  const rules::CueOutput plain = oneS(*e, "The house is big.", optS(rules::Lang::En, rules::Lang::La));
  CHECK(plain.confidence == rules::Confidence::Ok);
}

// ---- own material end to end (text project, one job per file) --------------------------------------------------------
TEST_CASE("rules-s: own material vs gold (tests/regression/own_core*.txt; report regression_report_s.txt)") {
  NEED_REALS();
  auto e = engineS();
  struct F { const char* src; const char* gold; rules::Lang s, t; int floor; };
  // floors: the counts after C41 (first run before any change: 8 / 15, 11 / 15, 8 / 15, 8 / 15, 0 / 2)
  const F files[] = {
      {"own_core.la.txt", "own_core.en.gold.txt", rules::Lang::La, rules::Lang::En, 13},
      {"own_core.la.txt", "own_core.es.gold.txt", rules::Lang::La, rules::Lang::Es, 13},
      {"own_core_grc.grc.txt", "own_core_grc.en.gold.txt", rules::Lang::Grc, rules::Lang::En, 15},
      {"own_core_grc.grc.txt", "own_core_grc.es.gold.txt", rules::Lang::Grc, rules::Lang::Es, 15},
      {"own_core.en.txt", "own_core.la.gold.txt", rules::Lang::En, rules::Lang::La, 0},
  };
  std::ostringstream rep;
  for (const F& f : files) {
    const std::vector<std::string> src = linesS(repoS() / "tests" / "regression" / f.src);
    const std::vector<std::string> gold = linesS(repoS() / "tests" / "regression" / "expected" / f.gold);
    REQUIRE(src.size() == gold.size());
    const rules::Options o = optS(f.s, f.t);
    const std::vector<rules::CueOutput> out = runS(*e, src, o);
    REQUIRE(out.size() == src.size());
    int right = 0, ok = 0, okMiss = 0;
    rep << "== " << f.src << " -> " << f.gold << "\n";
    for (size_t i = 0; i < src.size(); ++i) {
      const bool m = matchS(out[i].target, gold[i]);
      right += m;
      if (out[i].confidence == rules::Confidence::Ok) {
        ++ok;
        okMiss += !m;
      }
      rep << (i + 1) << "\t" << (m ? "=" : "x") << "\t" << out[i].target << "\t" << confS(out[i].confidence) << "\n";
    }
    rep << "right " << right << " / " << src.size() << "; ok " << ok << " (mismatches among OK " << okMiss << ")\n";
    MESSAGE(std::string(f.gold) << ": right " << right << " / " << src.size() << ", ok " << ok << ", mismatches among OK " << okMiss);
    CHECK(right >= f.floor);
    CHECK(okMiss == 0);
    const std::vector<rules::CueOutput> again = runS(*e, src, o);   // deterministic
    for (size_t i = 0; i < src.size(); ++i) CHECK(again[i].target == out[i].target);
  }
  std::ofstream(buildS() / "regression_report_s.txt") << rep.str();
}
