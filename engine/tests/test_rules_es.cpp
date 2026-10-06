// engine/rules Spanish source (C13): Spanish phrasebook / contractions / clitics tables, the Spanish branch of the
// frame builder (clitics, usted, ir a + infinitive, perfecto compuesto, estar + gerundio, ser/estar, hay, gustar-type
// verbs, personal "a", double negation, ¿¡, se impersonal, diminutives, subjunctive by conjunction, vocatives), the
// es: keyword path of the transfer with the English pivot, and makeEngine() end to end on
// tests/regression/own_dialogue.es.srt vs the main agent's gold (report in <build>/regression_report_es.txt).
// Tests needing the real data (data/work: latin.vpl, english.vpl, spanish.vpl, nlp/*.vpt; env VP_DATA_WORK) skip
// with a message when it is absent.
#include <doctest.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "vp/curated.h"
#include "vp/engine_config.h"
#include "vp/features.h"
#include "vp/frame.h"
#include "vp/lex.h"
#include "vp/nlp.h"
#include "vp/rules.h"
#include "vp/subs.h"
#include "vp/text.h"
#include "vp/transfer.h"

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
  lex::Lexicon la, en, es;
  nlp::Pipeline pes;
  bool ok = false;
  std::string why;
};
const Real& real() {
  static Real r = [] {
    Real x;
    auto la = lex::Lexicon::open(work() / "latin.vpl");
    auto en = lex::Lexicon::open(work() / "english.vpl");
    auto es = lex::Lexicon::open(work() / "spanish.vpl");
    auto pes = nlp::Pipeline::open(nlp::Lang::Es, (work() / "nlp" / "spanish.tag.vpt").string(),
                                   (work() / "nlp" / "spanish.dep.vpt").string());
    if (!la.ok() || !en.ok() || !es.ok() || !pes.ok()) {
      x.why = !la.ok() ? la.error().message : !en.ok() ? en.error().message : !es.ok() ? es.error().message
                                                                                         : pes.error().message;
      return x;
    }
    x.la = std::move(la.value());
    x.en = std::move(en.value());
    x.es = std::move(es.value());
    x.pes = std::move(pes.value());
    x.ok = true;
    return x;
  }();
  return r;
}
#define NEED_REAL()                                                                                  \
  if (!real().ok) {                                                                                  \
    MESSAGE("real data not available (" << real().why << "); set VP_DATA_WORK. Skipped.");          \
    return;                                                                                          \
  }

std::unique_ptr<rules::Engine> engine() {
  rules::EngineConfig cfg = rules::defaultEngineConfig();
  cfg.curatedDir = (repo() / "data" / "curated").string();
  cfg.dataDir = work().string();
  cfg.nlpDir = (work() / "nlp").string();
  auto e = rules::makeEngine(cfg);
  const Real& r = real();
  e->setLexicons(&r.la, nullptr, &r.en, &r.es);
  return e;
}

std::string norm(const std::string& s) {
  std::string k = text::latin_key(s);
  std::string out;
  for (char c : k) {
    if (c == ' ' && (out.empty() || out.back() == ' ')) continue;
    out += c;
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

const char* confName(rules::Confidence c) {
  return c == rules::Confidence::Ok ? "ok" : c == rules::Confidence::Check ? "check" : "fix";
}

long rssAnonKb() {
  std::ifstream in("/proc/self/status");
  std::string k;
  while (in >> k) {
    if (k == "RssAnon:") { long v = -1; in >> v; return v; }
    std::string rest;
    std::getline(in, rest);
  }
  return -1;
}

std::vector<rules::CueInput> regressionCues(int repeat = 1) {
  std::ifstream f(repo() / "tests" / "regression" / "own_dialogue.es.srt", std::ios::binary);
  std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)), {});
  auto d = subs::parse(b, subs::Format::Srt);
  REQUIRE(d.ok());
  std::vector<rules::CueInput> in;
  for (int r = 0; r < repeat; ++r)
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

rules::Options esOptions(int fidelity = 2, char gender = 'f') {
  rules::Options o;
  o.source = rules::Lang::Es;
  o.target = rules::Lang::La;
  o.fidelity = fidelity;
  o.speakerGender = gender;
  return o;
}

}  // namespace

// ================================================================================================================
TEST_CASE("rules-es: end to end on own_dialogue.es.srt vs the gold Latin (report; determinism)") {
  NEED_REAL();
  auto e = engine();
  std::vector<rules::CueInput> in = regressionCues();
  REQUIRE(in.size() == 100);
  const rules::Options o = esOptions();   // the gold's speaker is Alicia (header of the gold file)
  rules::Context ctx;
  auto r1 = e->translate(in, o, ctx, nullptr, nullptr);
  REQUIRE(r1.ok());
  auto r2 = engine()->translate(in, o, ctx, nullptr, nullptr);   // a fresh engine: byte-identical
  REQUIRE(r2.ok());
  REQUIRE(r1->size() == 100);
  REQUIRE(r2->size() == 100);
  bool same = true;
  for (size_t i = 0; i < 100; ++i) {
    same = same && r1.value()[i].target == r2.value()[i].target;
    same = same && r1.value()[i].confidence == r2.value()[i].confidence;
  }
  CHECK(same);
  std::ifstream g(repo() / "tests" / "regression" / "expected" / "own_dialogue.es.la.gold.txt");
  std::vector<std::string> gold;
  std::string line;
  while (std::getline(g, line))
    if (!line.empty() && line[0] != '#') gold.push_back(line);
  REQUIRE(gold.size() == 100);
  frame::FrameBuilder fb(frame::SrcLang::Es, &real().pes, &real().es, cur());
  int matches = 0, exact = 0;
  std::map<std::string, int> conf, checkWhy;
  std::ostringstream table, frames;
  table << "| # | source | gold | ours | checks |\n|---|---|---|---|---|\n";
  int shown = 0;
  for (size_t i = 0; i < 100; ++i) {
    const rules::CueOutput& c = r1.value()[i];
    CHECK_MESSAGE(!c.target.empty(), "empty target for cue " << i + 1);
    ++conf[confName(c.confidence)];
    const std::string ours = flat(c.target);
    bool match = false, exactMatch = false;
    for (const std::string& alt : splitAlt(gold[i])) {
      match = match || norm(alt) == norm(ours);
      exactMatch = exactMatch || text::nfc(alt) == text::nfc(ours);
    }
    matches += match;
    exact += exactMatch;
    if (c.confidence == rules::Confidence::Check) {
      bool why = false;
      for (const char* f : {"frame-fallback", "addressee-guess", "low-tier", "speaker-gender", "name-guessed",
                            "from-rule", "missing-form", "merged", "tags-approximated", "song", "nonverbal", "pivot"})
        if (std::find(c.flags.begin(), c.flags.end(), f) != c.flags.end()) { ++checkWhy[f]; why = true; }
      for (const auto& k : c.checks)
        if (!k.ok) { ++checkWhy[k.id]; why = true; }
      if (!why) ++checkWhy["margin < 0.15"];
    }
    if (!match) {
      frame::SemSentence s;
      frames << i + 1 << "\t" << in[i].sourceText << "\n";
      for (const auto& ss : frame::mapSentences({in[i].sourceText})) {
        fb.analyse(ss.text, s);
        frames << "  " << frame::describe(s) << "\n  ";
        for (const nlp::Token& t : s.tokens)
          frames << t.text << "/" << t.lemma << "/" << t.upos << "/" << t.deprel << ">" << t.head << " ";
        frames << "\n";
      }
    }
    if (!match && shown < 40) {
      ++shown;
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
  rep << "Regression own_dialogue.es.srt -> Latin, fidelity 2, speaker f\n";
  rep << "match rate (normalised: NFC, macron/punctuation/case-insensitive, any gold alternative): " << matches
      << " / 100\n";
  rep << "exact (macrons and punctuation too): " << exact << " / 100\n";
  rep << "confidence: ok " << conf["ok"] << ", check " << conf["check"] << ", fix " << conf["fix"] << "\n";
  rep << "check because:";
  for (const auto& w : checkWhy) rep << " " << w.first << " " << w.second << ";";
  rep << "\n\n";
  rep << "First " << shown << " mismatches:\n" << table.str() << "\nAll outputs:\n";
  for (size_t i = 0; i < 100; ++i)
    rep << i + 1 << "\t" << in[i].sourceText << "\t" << flat(r1.value()[i].target) << "\t"
        << confName(r1.value()[i].confidence) << "\n";
  rep << "\nFrames of the mismatches:\n" << frames.str();
  std::ofstream(buildDir() / "regression_report_es.txt") << rep.str();
  MESSAGE("regression es: " << matches << " / 100 match the gold; confidence ok " << conf["ok"] << " / check "
                            << conf["check"] << " / fix " << conf["fix"] << "; report "
                            << (buildDir() / "regression_report_es.txt").string());
}
