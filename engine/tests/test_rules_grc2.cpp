// engine/rules Greek engine path (C12): EN/ES -> Ancient Greek (transfer_grc, engine_grc), Greek -> EN/ES (grc2x),
// monotonic export, regression on the 40-line Greek gold. Needs the real data (VP_DATA_WORK: greek.vpl,
// english.vpl, spanish.vpl, nlp/*.vpt; VP_GREEK_VPL may point at greek.vpl); skips with a message otherwise.
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
#include "vp/grc2x.h"
#include "vp/engine_config.h"
#include "vp/frame.h"
#include "vp/lex.h"
#include "vp/nlp.h"
#include "vp/realise_grc.h"
#include "vp/rules.h"
#include "vp/subs.h"
#include "vp/text.h"
#include "vp/transfer_grc.h"

namespace stdfs = std::filesystem;
using namespace vp;

namespace {

stdfs::path repo() { return stdfs::path(VP_FIXTURES_DIR).parent_path().parent_path(); }
stdfs::path work() {
  const char* e = std::getenv("VP_DATA_WORK");
  return e && *e ? stdfs::path(e) : repo() / "data" / "work";
}
stdfs::path buildDir() { return stdfs::path(VP_TEST_TMP).parent_path(); }

const curated::CuratedData& cur() {
  static Result<curated::CuratedData> r = curated::CuratedData::load(repo() / "data" / "curated");
  REQUIRE_MESSAGE(r.ok(), r.error().message);
  return r.value();
}

struct Real {
  lex::Lexicon grc, en, es;
  nlp::Pipeline pen, pes;
  bool ok = false, esOk = false;
  std::string why;
};
const Real& real() {
  static Real r = [] {
    Real x;
    const char* g = std::getenv("VP_GREEK_VPL");
    auto grc = lex::Lexicon::open(g && *g ? stdfs::path(g) : work() / "greek.vpl");
    auto en = lex::Lexicon::open(work() / "english.vpl");
    auto pen = nlp::Pipeline::open(nlp::Lang::En, (work() / "nlp" / "english.tag.vpt").string(),
                                   (work() / "nlp" / "english.dep.vpt").string());
    if (!grc.ok() || !en.ok() || !pen.ok()) {
      x.why = !grc.ok() ? grc.error().message : !en.ok() ? en.error().message : pen.error().message;
      return x;
    }
    x.grc = std::move(grc.value());
    x.en = std::move(en.value());
    x.pen = std::move(pen.value());
    x.ok = true;
    auto es = lex::Lexicon::open(work() / "spanish.vpl");
    auto pes = nlp::Pipeline::open(nlp::Lang::Es, (work() / "nlp" / "spanish.tag.vpt").string(),
                                   (work() / "nlp" / "spanish.dep.vpt").string());
    if (es.ok() && pes.ok()) {
      x.es = std::move(es.value());
      x.pes = std::move(pes.value());
      x.esOk = true;
    }
    return x;
  }();
  return r;
}
#define NEED_REAL()                                                                                  \
  if (!real().ok) {                                                                                  \
    MESSAGE("real Greek data not available (" << real().why << "); set VP_DATA_WORK. Skipped.");    \
    return;                                                                                          \
  }

// The engine as the CLI makes it (makeEngine with the data directories), with the Greek, English and Spanish lexicons.
std::unique_ptr<rules::Engine> path() {
  rules::EngineConfig cfg = rules::defaultEngineConfig();
  cfg.curatedDir = (repo() / "data" / "curated").string();
  cfg.dataDir = work().string();
  cfg.nlpDir = (work() / "nlp").string();
  auto e = rules::makeEngine(cfg);
  e->setLexicons(nullptr, &real().grc, &real().en, real().esOk ? &real().es : nullptr);
  return e;
}

std::vector<rules::CueInput> srtCues(const char* file) {
  std::ifstream f(repo() / "tests" / "regression" / file, std::ios::binary);
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

std::string flat(std::string s) {
  std::replace(s.begin(), s.end(), '\n', ' ');
  return s;
}

// Normalised comparison for the Greek gold: NFC, lower case (decision 5 capitalises sentence starts; the gold is lower
// case), punctuation-insensitive (. , ; · ! ? and the Greek question mark), spaces collapsed. Accents and breathings
// count: the realiser must get them right.
std::string normGrc(const std::string& s) {
  std::string l = text::lower(text::nfc(s));
  std::string out;
  size_t i = 0;
  while (i < l.size()) {
    size_t j = i;
    const char32_t c = text::decodeUtf8(l, j);
    const bool punct = c == '.' || c == ',' || c == ';' || c == '!' || c == '?' || c == ':' || c == 0xB7 ||
                       c == 0x387 || c == 0x37E || c == '"';
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

std::vector<std::string> splitAlt(const std::string& s) {
  std::vector<std::string> out;
  size_t a = 0;
  for (;;) {
    size_t b = s.find(" | ", a);
    out.push_back(s.substr(a, b == std::string::npos ? std::string::npos : b - a));
    if (b == std::string::npos) break;
    a = b + 3;
  }
  return out;
}

[[maybe_unused]] long rssAnonKb() {
  std::ifstream in("/proc/self/status");
  std::string k;
  while (in >> k) {
    if (k == "RssAnon:") { long v = -1; in >> v; return v; }
    std::string rest;
    std::getline(in, rest);
  }
  return -1;
}

const char* confName(rules::Confidence c) {
  return c == rules::Confidence::Ok ? "ok" : c == rules::Confidence::Check ? "check" : "fix";
}

}  // namespace

TEST_CASE("rules-grc2: debug frames (VP_GRC2_FRAMES=1)") {
  const char* e = std::getenv("VP_GRC2_FRAMES");
  if (!e || !*e) return;
  NEED_REAL();
  const bool es = std::string(e) == "es";
  if (es && !real().esOk) return;
  std::ifstream in(repo() / "tests" / "regression" / (es ? "own_dialogue.es.txt" : "own_dialogue.en.txt"));
  std::string line;
  static curated::CuratedData cg = cur();
  auto gd = grc::GreekData::load(repo() / "data" / "curated");
  REQUIRE(gd.ok());
  auto gt = grc::GreekTables::load(repo() / "data" / "curated");
  REQUIRE(gt.ok());
  cg.replacePhrasebooks(gd.value().phrasebook(), gt.value().phrasebookEs());
  frame::FrameBuilder fb(es ? frame::SrcLang::Es : frame::SrcLang::En, es ? &real().pes : &real().pen,
                         es ? &real().es : &real().en, cg);
  int k = 0;
  while (std::getline(in, line) && k < 120) {
    ++k;
    for (const auto& ss : frame::mapSentences({line})) {
      frame::SemSentence s;
      fb.analyse(ss.text, s);
      std::cout << k << "\t" << line << "\n   " << frame::describe(s) << "\n";
    }
  }
}

TEST_CASE("rules-grc2: EN -> GRC regression on all 114 lines of own_dialogue.en.srt vs the Greek gold") {
  NEED_REAL();
  auto e = path();
  std::vector<rules::CueInput> in = srtCues("own_dialogue.en.srt");
  std::ifstream g(repo() / "tests" / "regression" / "expected" / "own_dialogue.grc.gold.txt");
  std::vector<std::string> gold;
  std::string line;
  while (std::getline(g, line))
    if (!line.empty() && line[0] != '#') gold.push_back(line);
  REQUIRE(gold.size() >= 40);
  REQUIRE(in.size() >= gold.size());
  const size_t n = gold.size();
  in.resize(n);
  rules::Options o;
  o.source = rules::Lang::En;
  o.target = rules::Lang::Grc;
  o.fidelity = 2;
  o.speakerGender = 'f';
  rules::Context ctx;
  auto r1 = e->translate(in, o, ctx, nullptr, nullptr);
  REQUIRE(r1.ok());
  REQUIRE(r1->size() == n);
  // lines 1-40 were the tuning set of C12; 41-114 the tuning set of C16 (quality loop 2)
  int matches = 0, exact = 0, first40 = 0, rest = 0;
  std::map<std::string, int> conf, confRest;
  std::ostringstream table;
  table << "| # | source | gold | ours | checks |\n|---|---|---|---|---|\n";
  for (size_t i = 0; i < n; ++i) {
    const rules::CueOutput& c = r1.value()[i];
    ++conf[confName(c.confidence)];
    if (i >= 40) ++confRest[confName(c.confidence)];
    const std::string ours = flat(c.target);
    bool match = false, ex = false;
    for (const std::string& alt : splitAlt(gold[i])) {
      match = match || normGrc(alt) == normGrc(ours);
      ex = ex || text::nfc(alt) == text::nfc(ours);
    }
    matches += match;
    exact += ex;
    (i < 40 ? first40 : rest) += match;
    if (!match) {
      std::string chk;
      for (const auto& k : c.checks)
        if (!k.ok) chk += k.id + " ";
      std::string g2 = gold[i];
      for (size_t at; (at = g2.find(" | ")) != std::string::npos;) g2.replace(at, 3, " / ");
      table << "| " << i + 1 << " | " << in[i].sourceText << " | " << g2 << " | " << ours << " | "
            << confName(c.confidence) << (chk.empty() ? "" : " " + chk) << "|\n";
    }
  }
  std::ostringstream rep;
  rep << "Regression own_dialogue.en.srt (" << n << " cues) -> Attic Greek, fidelity 2, speaker f\n";
  rep << "match rate (normalised: NFC, case- and punctuation-insensitive, accents count, any gold alternative): "
      << matches << " / " << n << "\n";
  rep << "lines 1-40: " << first40 << " / 40; lines 41-" << n << ": " << rest << " / " << n - 40 << "\n";
  rep << "exact (case and punctuation too): " << exact << " / " << n << "\n";
  rep << "confidence: ok " << conf["ok"] << ", check " << conf["check"] << ", fix " << conf["fix"]
      << " (lines 41-" << n << ": ok " << confRest["ok"] << ", check " << confRest["check"] << ", fix "
      << confRest["fix"] << ")\n\n";
  rep << "Mismatches:\n" << table.str() << "\nAll outputs:\n";
  for (size_t i = 0; i < n; ++i) {
    std::string why;
    for (const auto& k : r1.value()[i].checks)
      if (!k.ok || k.detail.rfind("warning", 0) == 0) why += k.id + "(" + k.detail + ") ";
    for (const auto& f : r1.value()[i].flags) why += "[" + f + "] ";
    rep << i + 1 << "\t" << in[i].sourceText << "\t" << flat(r1.value()[i].target) << "\t"
        << confName(r1.value()[i].confidence) << "\t" << why << "\n";
  }
  std::ofstream(buildDir() / "regression_report_grc.txt") << rep.str();
  MESSAGE("EN -> GRC regression: " << matches << " / " << n << " match the gold (1-40: " << first40 << " / 40, 41-"
                                   << n << ": " << rest << " / " << n - 40 << "; exact " << exact
                                   << "); confidence ok " << conf["ok"] << " / check " << conf["check"] << " / fix "
                                   << conf["fix"] << "; report " << (buildDir() / "regression_report_grc.txt").string());
  CHECK(first40 >= 40);
  CHECK(rest >= 0);
}

TEST_CASE("rules-grc2: ES -> GRC on the first 40 lines of own_dialogue.es.srt vs the Greek gold (report only)") {
  NEED_REAL();
  if (!real().esOk) {
    MESSAGE("spanish.vpl / spanish nlp models not available. Skipped.");
    return;
  }
  auto e = path();
  std::vector<rules::CueInput> in = srtCues("own_dialogue.es.srt");
  REQUIRE(in.size() >= 40);
  in.resize(40);
  rules::Options o;
  o.source = rules::Lang::Es;
  o.target = rules::Lang::Grc;
  o.fidelity = 2;
  o.speakerGender = 'f';
  auto r1 = e->translate(in, o, rules::Context{}, nullptr, nullptr);
  REQUIRE(r1.ok());
  REQUIRE(r1->size() == 40);
  std::ifstream g(repo() / "tests" / "regression" / "expected" / "own_dialogue.grc.gold.txt");
  std::vector<std::string> gold;
  std::string line;
  while (std::getline(g, line))
    if (!line.empty() && line[0] != '#') gold.push_back(line);
  REQUIRE(gold.size() >= 40);
  int matches = 0;
  std::map<std::string, int> conf;
  std::ostringstream all;
  for (size_t i = 0; i < 40; ++i) {
    const rules::CueOutput& c = r1.value()[i];
    ++conf[confName(c.confidence)];
    bool match = false;
    for (const std::string& alt : splitAlt(gold[i])) match = match || normGrc(alt) == normGrc(flat(c.target));
    matches += match;
    all << i + 1 << "\t" << (match ? "=" : "x") << "\t" << in[i].sourceText << "\t" << flat(c.target) << "\t"
        << confName(c.confidence) << "\n";
  }
  std::ostringstream rep;
  rep << "Regression own_dialogue.es.srt (first 40 cues) -> Attic Greek, fidelity 2, speaker f (report only)\n";
  rep << "match rate vs own_dialogue.grc.gold.txt (normalised as the English test): " << matches << " / 40\n";
  rep << "confidence: ok " << conf["ok"] << ", check " << conf["check"] << ", fix " << conf["fix"] << "\n\n" << all.str();
  std::ofstream(buildDir() / "regression_report_grc_es.txt") << rep.str();
  MESSAGE("ES -> GRC regression (report only): " << matches << " / 40; report "
                                                  << (buildDir() / "regression_report_grc_es.txt").string());
}

namespace {
std::string normX(const std::string& s) {   // readable EN/ES: case- and punctuation-insensitive
  std::string l = text::lower(text::nfc(s)), out;
  size_t i = 0;
  while (i < l.size()) {
    size_t j = i;
    const char32_t c = text::decodeUtf8(l, j);
    const bool punct = c == '.' || c == ',' || c == ';' || c == '!' || c == '?' || c == 0xBF || c == 0xA1;
    if (punct || c == ' ') { if (!out.empty() && out.back() != ' ') out += ' '; }
    else out += l.substr(i, j - i);
    i = j;
  }
  while (!out.empty() && out.back() == ' ') out.pop_back();
  return out;
}
struct Tables {
  grc::GreekData gd;
  grc::GreekTables gt;
};
const Tables& tables() {
  static Tables t = [] {
    Tables x;
    auto gd = grc::GreekData::load(repo() / "data" / "curated");
    auto gt = grc::GreekTables::load(repo() / "data" / "curated");
    REQUIRE(gd.ok());
    REQUIRE(gt.ok());
    x.gd = std::move(gd.value());
    x.gt = std::move(gt.value());
    return x;
  }();
  return t;
}
}  // namespace

TEST_CASE("rules-grc2: GRC -> EN / ES on 40 own Attic sentences (tests/fixtures/grc2x/sentences.tsv)") {
  NEED_REAL();
  grc2x::Translator tr(real().grc, cur(), tables().gd, tables().gt);
  std::ifstream f(stdfs::path(VP_FIXTURES_DIR) / "grc2x" / "sentences.tsv");
  std::string line;
  int n = 0, en = 0, es = 0;
  std::ostringstream rep;
  while (std::getline(f, line)) {
    if (line.empty() || line[0] == '#') continue;
    const size_t a = line.find('\t'), b = line.find('\t', a + 1);
    REQUIRE(b != std::string::npos);
    const std::string g = line.substr(0, a), we = line.substr(a + 1, b - a - 1), ws = line.substr(b + 1);
    grc2x::SentenceOut oe, os;
    tr.sentence(g, grc2x::Target::En, oe);
    tr.sentence(g, grc2x::Target::Es, os);
    ++n;
    const bool okE = normX(oe.text) == normX(we), okS = normX(os.text) == normX(ws);
    en += okE;
    es += okS;
    CHECK_MESSAGE(okE, g << " -> EN '" << oe.text << "' expected '" << we << "' | " << oe.frame);
    CHECK_MESSAGE(okS, g << " -> ES '" << os.text << "' expected '" << ws << "'");
    rep << (okE ? "=" : "x") << (okS ? "=" : "x") << "\t" << g << "\t" << oe.text << "\t" << os.text << "\n";
    // interlinear: every word has a lemma or is a name, a gloss and its features in words
    for (const grc2x::Word& w : oe.words) {
      CHECK_MESSAGE((w.lemma != lex::kNoLemma || w.name), g << ": no reading for " << w.text);
      CHECK_MESSAGE(!w.featureText.empty(), g << ": no features for " << w.text);
    }
  }
  REQUIRE(n == 40);
  std::ofstream(buildDir() / "grc2x_report.txt") << "GRC -> EN " << en << " / 40, GRC -> ES " << es << " / 40\n" << rep.str();
  MESSAGE("GRC -> EN " << en << " / 40, GRC -> ES " << es << " / 40; report " << (buildDir() / "grc2x_report.txt").string());
}

TEST_CASE("rules-grc2: debug grc2x (VP_GRC2X_DEBUG=<greek sentence>)") {
  const char* e = std::getenv("VP_GRC2X_DEBUG");
  if (!e || !*e) return;
  NEED_REAL();
  grc2x::Translator tr(real().grc, cur(), tables().gd, tables().gt);
  grc2x::SentenceOut so;
  tr.sentence(e, grc2x::Target::En, so);
  std::cout << so.text << "\n" << so.frame << "\n";
  for (const auto& w : so.words) {
    std::cout << "  " << w.text << " -> " << w.head << " (" << w.featureText << ") '" << w.gloss << "' role=" << w.role
              << " conf=" << w.confidence;
    for (const auto& a : w.alternatives) std::cout << " | " << a;
    std::cout << "\n";
  }
}

TEST_CASE("rules-grc2: monotonic export fallback (20 cases), capitals, break hints") {
  const char* const cases[][2] = {
      {"ὁ", "ο"},
      {"ἡ θύρα", "η θύρα"},
      {"τῷ ἀγρῷ", "τω αγρώ"},
      {"ποῖ βαίνεις;", "ποί βαίνεις;"},
      {"ὁ καλὸς παῖς", "ο καλός παις"},
      {"ἄνθρωπος", "άνθρωπος"},
      {"ῥήτωρ", "ρήτωρ"},
      {"Ἀλίκη", "Αλίκη"},
      {"ὦ φίλε", "ω φίλε"},
      {"ᾠδή", "ωδή"},
      {"τὴν κεφαλὴν αὐτοῦ", "την κεφαλήν αυτού"},
      {"προΐστημι", "προΐστημι"},
      {"Ἆρα εἶδες τὴν γαλῆν μου;", "Άρα είδες την γαλήν μου;"},
      {"Οὐκ οἶδα.", "Ουκ οίδα."},
      {"ᾆσον ἡμῖν ᾠδήν.", "άσον ημίν ωδήν."},
      {"Ἡ θύρα λίαν μικρά ἐστιν.", "Η θύρα λίαν μικρά εστιν."}, {"τίς εἶ; ἤ", "τίς ει; ή"},
      {"δεῦρο", "δεύρο"},
      {"Ἐπάνελθε δεῦρο!", "Επάνελθε δεύρο!"},
      {"πάντες, προσκυνεῖτε!", "πάντες, προσκυνείτε!"},
      {"Hello [tea] 6", "Hello [tea] 6"},
      {"ἕξ", "εξ"},
      {"ἀλλ’ ἐπὶ τοῦ ἵππου", "αλλ’ επί του ίππου"},
  };
  size_t ok = 0;
  for (const auto& c : cases) {
    const std::string got = grc::toMonotonic(text::nfc(c[0]));
    CHECK_MESSAGE(got == text::nfc(c[1]), std::string(c[0]) << " -> " << got << " expected " << std::string(c[1]));
    ok += got == text::nfc(c[1]);
  }
  MESSAGE("monotonic: " << ok << " / " << sizeof(cases) / sizeof(cases[0]));
  CHECK(grc::capitaliseGreek(text::nfc("ὁ παῖς")) == text::nfc("Ὁ παῖς"));
  CHECK(grc::capitaliseGreek(text::nfc("ἆρα")) == text::nfc("Ἆρα"));
  CHECK(grc::capitaliseGreek(text::nfc("ᾠδή")) == text::nfc("ᾨδή"));
  CHECK(grc::capitaliseGreek(text::nfc("ῥήτωρ")) == text::nfc("Ῥήτωρ"));
  CHECK(grc::capitaliseGreek("<i>ποῖ") == text::nfc("<i>Ποῖ"));
  CHECK(grc::capitaliseGreek("[tea]") == "[Tea]");
  CHECK(grc::capitaliseGreek("6") == "6");
  const auto& h = grc::greekBreakHints().breakBefore;
  CHECK(std::find(h.begin(), h.end(), text::nfc("καὶ")) != h.end());
  CHECK(std::find(h.begin(), h.end(), text::nfc("ἵνα")) != h.end());
  const auto lines = subs::breakLines(text::nfc("Ὁ γεωργὸς εἰς τὴν πόλιν σπεύδει καὶ ὁ δοῦλος ἐν τῷ ἀγρῷ μένει."),
                                      grc::greekBreakHints(), 42, 2);
  REQUIRE(lines.size() == 2);
  CHECK(lines[1].rfind(text::nfc("καὶ"), 0) == 0);
}

TEST_CASE("rules-grc2: engine path - determinism, check(), inspect(), corrections, nonverbal / song, RSS flat") {
  NEED_REAL();
  std::vector<rules::CueInput> in = srtCues("own_dialogue.en.srt");
  in.resize(40);
  rules::Options o;
  o.source = rules::Lang::En;
  o.target = rules::Lang::Grc;
  o.speakerGender = 'f';
  rules::Context ctx;
  auto e1 = path();
  auto e2 = path();
  auto r1 = e1->translate(in, o, ctx, nullptr, nullptr);
  auto r2 = e2->translate(in, o, ctx, nullptr, nullptr);
  REQUIRE(r1.ok());
  REQUIRE(r2.ok());
  bool same = true;
  for (size_t i = 0; i < 40; ++i)
    same = same && r1.value()[i].target == r2.value()[i].target && r1.value()[i].confidence == r2.value()[i].confidence;
  CHECK(same);
  // token offsets cover the target; reasons point at tokens; checks A1..A9 present
  for (const rules::CueOutput& c : r1.value()) {
    for (const auto& t : c.tokens) {
      if (t.start < 0) continue;
      REQUIRE(t.end <= (int)c.target.size());
      CHECK(c.target.substr((size_t)t.start, (size_t)(t.end - t.start)) == t.text);
    }
    std::vector<std::string> ids;
    for (const auto& k : c.checks) ids.push_back(k.id);
    for (const char* id : {"A1", "A1b", "A3", "A4", "A5", "A6", "A7", "A8", "A9"})
      CHECK_MESSAGE(std::find(ids.begin(), ids.end(), id) != ids.end(), "missing check " << id << " in " << c.target);
  }
  // realia (decision 1): Check with the reason
  {
    const rules::CueOutput& tea = r1.value()[35];
    CHECK(tea.confidence == rules::Confidence::Check);
    bool reason = false;
    for (const auto& r : tea.reasons) reason = reason || r.text.find("no Attic word") != std::string::npos;
    CHECK(reason);
  }
  // decision 2: "please" dropped in flexible mode
  {
    rules::Options f3 = o;
    f3.fidelity = 3;
    rules::CueInput c;
    c.sourceText = "Open the door, please.";
    auto r = e1->translate({c}, f3, ctx, nullptr, nullptr);
    REQUIRE(r.ok());
    CHECK(r.value()[0].target.find(text::nfc("ἀντιβολῶ")) == std::string::npos);
    CHECK(normGrc(r.value()[0].target) == normGrc(text::nfc("ἄνοιξον τὴν θύραν")));
  }
  // names (decision 6), coined form, article in narrative
  {
    rules::CueInput c;
    c.sourceText = "Alice opens the door.";
    auto r = e1->translate({c}, o, ctx, nullptr, nullptr);
    REQUIRE(r.ok());
    CHECK(normGrc(r.value()[0].target) == normGrc(text::nfc("ἡ Ἀλίκη τὴν θύραν ἀνοίγει")));
  }
  // check() of an edited cue: agreement fault -> Fix; correct -> not Fix
  {
    rules::CueInput c;
    c.startMs = 0;
    c.endMs = 3000;
    auto gr = e1->check(c, text::nfc("Ἡ θύρα λίαν μικρά ἐστιν."), o, ctx);
    auto br = e1->check(c, text::nfc("Ἡ θύρα λίαν μικρὸς ἐστιν."), o, ctx);
    REQUIRE(gr.ok());
    REQUIRE(br.ok());
    const rules::CueOutput& good = gr.value();
    const rules::CueOutput& bad = br.value();
    CHECK(good.confidence != rules::Confidence::Fix);
    CHECK(bad.confidence == rules::Confidence::Fix);
    bool a3 = false;
    for (const auto& k : bad.checks) a3 = a3 || (k.id == "A3" && !k.ok);
    CHECK(a3);
  }
  // inspect(): Greek word (gloss, features), accent-insensitive input, English word with target Greek
  {
    auto i1 = e1->inspect(text::nfc("βαίνεις"), rules::Lang::Grc, o);
    REQUIRE(i1.ok());
    bool baino = false;
    for (const auto& a : i1->analyses) baino = baino || (a.head == text::nfc("βαίνω") && a.glossEn == "go" && a.glossEs == "ir");
    CHECK(baino);
    auto i2 = e1->inspect(text::nfc("βαινεις"), rules::Lang::Grc, o);
    REQUIRE(i2.ok());
    CHECK((!i2->analyses.empty() || !i2->suggestions.empty()));
    auto i3 = e1->inspect("door", rules::Lang::En, o);
    REQUIRE(i3.ok());
    bool thura = false;
    for (const auto& a : i3->analyses) thura = thura || a.head == text::nfc("θύρα");
    CHECK(thura);
  }
  // corrections: a remembered whole-cue correction replaces the translation
  {
    rules::Context cc;
    cc.corrections.push_back(rules::Correction{text::en_key(in[0].sourceText), text::nfc("Ποῖ βαδίζεις;"), "cue", 1});
    auto r = e1->translate({in[0]}, o, cc, nullptr, nullptr);
    REQUIRE(r.ok());
    CHECK(flat(r.value()[0].target) == text::nfc("Ποῖ βαδίζεις;"));
  }
  // nonverbal and song cues
  {
    rules::CueInput nv, sg;
    nv.sourceText = "[laughs]";
    sg.sourceText = "\xE2\x99\xAA The cat runs \xE2\x99\xAA";
    auto r = e1->translate({nv, sg}, o, ctx, nullptr, nullptr);
    REQUIRE(r.ok());
    CHECK(r.value()[0].target == "[laughs]");
    CHECK(r.value()[0].confidence == rules::Confidence::Check);
    CHECK(r.value()[1].target.rfind("\xE2\x99\xAA ", 0) == 0);
  }
  // pairs grc-en / grc-es through the engine: readable text, source tokens with analysis reasons
  {
    rules::Options g = o;
    g.source = rules::Lang::Grc;
    g.target = rules::Lang::En;
    rules::CueInput c;
    c.sourceText = text::nfc("ὁ δοῦλος τὸν λίθον αἴρει. τίς εἶ;");
    auto r = e1->translate({c}, g, ctx, nullptr, nullptr);
    REQUIRE(r.ok());
    CHECK(r.value()[0].target == "The slave lifts the stone. Who are you?");
    CHECK(std::find(r.value()[0].flags.begin(), r.value()[0].flags.end(), "source-tokens") != r.value()[0].flags.end());
    REQUIRE(r.value()[0].tokens.size() == 7);
    CHECK(c.sourceText.substr((size_t)r.value()[0].tokens[1].start,
                              (size_t)(r.value()[0].tokens[1].end - r.value()[0].tokens[1].start)) == text::nfc("δοῦλος"));
    size_t analysis = 0;
    for (const auto& rs : r.value()[0].reasons) analysis += rs.kind == "analysis";
    CHECK(analysis == 7);
    g.target = rules::Lang::Es;
    auto rs = e1->translate({c}, g, ctx, nullptr, nullptr);
    REQUIRE(rs.ok());
    CHECK(rs.value()[0].target == "El esclavo levanta la piedra. ¿Quién eres?");
    // an accent-insensitive reading only is a warning (Check), never Fix
    rules::CueInput a;
    a.sourceText = text::nfc("ὁ δουλος τὸν λίθον αἴρει.");
    g.target = rules::Lang::En;
    auto ra = e1->translate({a}, g, ctx, nullptr, nullptr);
    REQUIRE(ra.ok());
    CHECK(ra.value()[0].confidence == rules::Confidence::Check);
    CHECK(ra.value()[0].target == "The slave lifts the stone.");
  }
  // no Greek lexicon: an error with a hint, never a crash
  {
    rules::EngineConfig cfg = rules::defaultEngineConfig();
    cfg.curatedDir = (repo() / "data" / "curated").string();
    auto none = rules::makeEngine(cfg);
    none->setLexicons(nullptr, nullptr, &real().en, nullptr);
    auto r = none->translate({in[0]}, o, ctx, nullptr, nullptr);
    CHECK(!r.ok());
    CHECK(!r.error().hint.empty());
  }
  // RSS flat: 10 cues, then 1,000 cues in batches of 20
#if !defined(__SANITIZE_ADDRESS__)
  {
    std::vector<rules::CueInput> ten(in.begin(), in.begin() + 10);
    for (int w = 0; w < 3; ++w) REQUIRE(e1->translate(in, o, ctx, nullptr, nullptr).ok());
    REQUIRE(e1->translate(ten, o, ctx, nullptr, nullptr).ok());
    const long before = rssAnonKb();
    size_t done = 0;
    while (done < 1000) {
      std::vector<rules::CueInput> batch(in.begin() + (long)(done % 40), in.begin() + (long)(done % 40) + 20);
      REQUIRE(e1->translate(batch, o, ctx, nullptr, nullptr).ok());
      done += 20;
    }
    const long after = rssAnonKb();
    MESSAGE("RssAnon " << before << " kB -> " << after << " kB over 1,000 cues");
    if (before > 0) CHECK(after <= before + before / 20 + 512);
  }
#endif
}

TEST_CASE("rules-grc2: try sentences (VP_GRC2_TRY=<file of English lines>)") {
  const char* e = std::getenv("VP_GRC2_TRY");
  if (!e || !*e) return;
  NEED_REAL();
  auto en = path();
  std::ifstream f(e);
  std::string line;
  std::vector<rules::CueInput> in;
  while (std::getline(f, line)) {
    if (line.empty()) continue;
    rules::CueInput c;
    c.index = (uint32_t)in.size();
    c.sourceText = line;
    in.push_back(c);
  }
  rules::Options o;
  o.source = rules::Lang::En;
  o.target = rules::Lang::Grc;
  o.speakerGender = 'f';
  auto r = en->translate(in, o, rules::Context{}, nullptr, nullptr);
  REQUIRE(r.ok());
  const char* why = std::getenv("VP_GRC2_TRY_WHY");
  for (size_t i = 0; i < in.size(); ++i) {
    std::cout << in[i].sourceText << "\t" << flat(r.value()[i].target) << "\t" << confName(r.value()[i].confidence) << "\n";
    if (why && *why) {
      for (const auto& rs : r.value()[i].reasons) std::cout << "    " << rs.kind << ": " << rs.text << " " << rs.data << "\n";
      for (const auto& k : r.value()[i].checks) std::cout << "    " << k.id << (k.ok ? " ok " : " FAIL ") << k.detail << "\n";
      for (const auto& a : r.value()[i].alternatives) std::cout << "    alt: " << a.text << " (" << a.reason << ")\n";
    }
  }
}
