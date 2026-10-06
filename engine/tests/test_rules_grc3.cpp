// engine/rules Greek quality loop 3 (C18): light verbs and verb + object idioms (lexical_en_grc.tsv kind light),
// noun sense consistency in a batch, purpose and result clauses, circumstantial participles, vocatives and
// particles, the Spanish possessive dative, the frame builder's derived English forms on the Greek path. Every rule
// is checked on our own sentences (at least two each, written before the regression file was run: the
// generalisation guard of docs/rules_grc3_notes.md). Needs the real data (VP_DATA_WORK); skips otherwise.
#include <doctest.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "vp/curated.h"
#include "vp/engine_config.h"
#include "vp/frame.h"
#include "vp/lex.h"
#include "vp/morph_grc.h"
#include "vp/nlp.h"
#include "vp/realise_grc.h"
#include "vp/rules.h"
#include "vp/text.h"
#include "vp/transfer_grc.h"

namespace stdfs = std::filesystem;
using namespace vp;

namespace {

stdfs::path repo3() { return stdfs::path(VP_FIXTURES_DIR).parent_path().parent_path(); }
stdfs::path work3() {
  const char* e = std::getenv("VP_DATA_WORK");
  return e && *e ? stdfs::path(e) : repo3() / "data" / "work";
}

struct Real3 {
  lex::Lexicon grc, en, es;
  nlp::Pipeline pen, pes;
  bool ok = false, esOk = false;
  std::string why;
};
const Real3& real3() {
  static Real3 r = [] {
    Real3 x;
    const char* g = std::getenv("VP_GREEK_VPL");
    auto grc = lex::Lexicon::open(g && *g ? stdfs::path(g) : work3() / "greek.vpl");
    auto en = lex::Lexicon::open(work3() / "english.vpl");
    auto pen = nlp::Pipeline::open(nlp::Lang::En, (work3() / "nlp" / "english.tag.vpt").string(),
                                   (work3() / "nlp" / "english.dep.vpt").string());
    if (!grc.ok() || !en.ok() || !pen.ok()) {
      x.why = !grc.ok() ? grc.error().message : !en.ok() ? en.error().message : pen.error().message;
      return x;
    }
    x.grc = std::move(grc.value());
    x.en = std::move(en.value());
    x.pen = std::move(pen.value());
    x.ok = true;
    auto es = lex::Lexicon::open(work3() / "spanish.vpl");
    auto pes = nlp::Pipeline::open(nlp::Lang::Es, (work3() / "nlp" / "spanish.tag.vpt").string(),
                                   (work3() / "nlp" / "spanish.dep.vpt").string());
    if (es.ok() && pes.ok()) {
      x.es = std::move(es.value());
      x.pes = std::move(pes.value());
      x.esOk = true;
    }
    return x;
  }();
  return r;
}
#define NEED_REAL3()                                                                                 \
  if (!real3().ok) {                                                                                 \
    MESSAGE("real Greek data not available (" << real3().why << "); set VP_DATA_WORK. Skipped.");   \
    return;                                                                                          \
  }

std::unique_ptr<rules::Engine> engine3() {
  rules::EngineConfig cfg = rules::defaultEngineConfig();
  cfg.curatedDir = (repo3() / "data" / "curated").string();
  cfg.dataDir = work3().string();
  cfg.nlpDir = (work3() / "nlp").string();
  auto e = rules::makeEngine(cfg);
  e->setLexicons(nullptr, &real3().grc, &real3().en, real3().esOk ? &real3().es : nullptr);
  return e;
}

std::string flat3(std::string s) {
  std::replace(s.begin(), s.end(), '\n', ' ');
  return s;
}

// As the regression: NFC, lower case, punctuation-insensitive, spaces collapsed; accents and breathings count.
std::string norm3(const std::string& s) {
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

const char* conf3(rules::Confidence c) {
  return c == rules::Confidence::Ok ? "ok" : c == rules::Confidence::Check ? "check" : "fix";
}

rules::Options opts3(bool es) {
  rules::Options o;
  o.source = es ? rules::Lang::Es : rules::Lang::En;
  o.target = rules::Lang::Grc;
  o.fidelity = 2;
  o.speakerGender = 'f';
  return o;
}

// One cue at a time (no discourse memory between cases); `expected` may list alternatives after " | ".
struct Case3 { const char* src; const char* expected; };
int runCases(rules::Engine& e, const std::vector<Case3>& cases, bool es, const char* what) {
  int ok = 0;
  for (const Case3& k : cases) {
    rules::CueInput x;
    x.sourceText = k.src;
    auto r = e.translate({x}, opts3(es), rules::Context{}, nullptr, nullptr);
    REQUIRE(r.ok());
    const std::string got = flat3(r.value()[0].target);
    bool hit = false;
    std::string exp = k.expected;
    size_t a = 0;
    for (;;) {
      const size_t b = exp.find(" | ", a);
      const std::string alt = exp.substr(a, b == std::string::npos ? std::string::npos : b - a);
      hit = hit || norm3(got) == norm3(text::nfc(alt));
      if (b == std::string::npos) break;
      a = b + 3;
    }
    CHECK_MESSAGE(hit, what << ": " << k.src << " -> '" << got << "' expected '" << k.expected << "'");
    CHECK_MESSAGE(r.value()[0].confidence != rules::Confidence::Fix, what << ": " << k.src << " is Fix");
    ok += hit;
  }
  MESSAGE(what << ": " << ok << " / " << cases.size());
  return ok;
}

}  // namespace

TEST_CASE("rules-grc3: debug frames (VP_GRC3_FRAMES=<file>, VP_GRC3_ES=1 for Spanish)") {
  const char* e = std::getenv("VP_GRC3_FRAMES");
  if (!e || !*e) return;
  NEED_REAL3();
  const char* esv = std::getenv("VP_GRC3_ES");
  const bool es = esv && *esv;
  if (es && !real3().esOk) return;
  static curated::CuratedData cg = [] {
    auto r = curated::CuratedData::load(repo3() / "data" / "curated");
    REQUIRE(r.ok());
    return std::move(r.value());
  }();
  auto gd = grc::GreekData::load(repo3() / "data" / "curated");
  REQUIRE(gd.ok());
  auto gt = grc::GreekTables::load(repo3() / "data" / "curated");
  REQUIRE(gt.ok());
  cg.replacePhrasebooks(gd.value().phrasebook(), gt.value().phrasebookEs());
  frame::FrameBuilder fb(es ? frame::SrcLang::Es : frame::SrcLang::En, es ? &real3().pes : &real3().pen,
                         es ? &real3().es : &real3().en, cg);
  std::ifstream in(e);
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty()) continue;
    for (const auto& ss : frame::mapSentences({line})) {
      frame::SemSentence s;
      fb.analyse(ss.text, s);
      std::cout << line << "\n   " << frame::describe(s);
      for (const std::string& d : s.doubts) std::cout << " [" << d << "]";
      std::cout << "\n";
    }
  }
}

TEST_CASE("rules-grc3: try sentences (VP_GRC3_TRY=<file>, VP_GRC3_ES=1 for Spanish, VP_GRC3_WHY=1)") {
  const char* e = std::getenv("VP_GRC3_TRY");
  if (!e || !*e) return;
  NEED_REAL3();
  const char* esv = std::getenv("VP_GRC3_ES");
  const bool es = esv && *esv;
  if (es && !real3().esOk) return;
  auto en = engine3();
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
  const char* why = std::getenv("VP_GRC3_WHY");
  for (size_t i = 0; i < in.size(); ++i) {   // one cue at a time, as the unit cases
    auto r = en->translate({in[i]}, opts3(es), rules::Context{}, nullptr, nullptr);
    REQUIRE(r.ok());
    const rules::CueOutput& o = r.value()[0];
    std::cout << in[i].sourceText << "\t" << flat3(o.target) << "\t" << conf3(o.confidence);
    for (const auto& fl : o.flags) std::cout << " [" << fl << "]";
    std::cout << "\n";
    if (why && *why) {
      for (const auto& rs : o.reasons) std::cout << "    " << rs.kind << ": " << rs.text << "\n";
      for (const auto& k : o.checks)
        if (!k.ok || k.detail.rfind("warning", 0) == 0) std::cout << "    " << k.id << (k.ok ? " ok " : " FAIL ") << k.detail << "\n";
    }
  }
}
