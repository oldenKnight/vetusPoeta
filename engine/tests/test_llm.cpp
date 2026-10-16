// engine/llm tests (DESIGN §11). Without a model file (CI) they check the error paths, the CPUID gate, the file
// checks, the Wilson interval and the gate's pair generation. With a model (env VP_MODEL_GGUF, or a *.gguf in the
// repository's models/ folder) they also load it and check choose/simplify/scoreText, determinism, RSS and the idle
// unload. Sanitizer builds run the model part only when VP_MODEL_GGUF is set explicitly (llama under ASan is slow).
#include <doctest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "vp/fs.h"
#include "vp/lex.h"
#include "vp/llm.h"
#include "vp/llm_gate.h"

namespace stdfs = std::filesystem;
using vp::ErrorCode;

#if defined(__SANITIZE_ADDRESS__)
#define VP_LLM_TEST_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define VP_LLM_TEST_ASAN 1
#endif
#endif

namespace {

std::string repoDir() { return (stdfs::path(VP_FIXTURES_DIR) / ".." / "..").lexically_normal().string(); }

std::string modelForTests() {
  const char* env = std::getenv("VP_MODEL_GGUF");
  if (env && *env) return env;
#if defined(VP_LLM_TEST_ASAN)
  return std::string();
#else
  const char* skip = std::getenv("VP_LLM_SKIP_MODEL");
  if (skip && *skip == '1') return std::string();
  std::error_code ec;
  const stdfs::path dir = stdfs::path(repoDir()) / "models";
  std::string best;
  for (stdfs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
    if (it->path().extension() == ".gguf" && (best.empty() || it->path().string() < best)) best = it->path().string();
  return best;
#endif
}

long statusKb(const char* field) {
  std::ifstream in("/proc/self/status");
  std::string line;
  const size_t n = std::char_traits<char>::length(field);
  while (std::getline(in, line))
    if (line.compare(0, n, field) == 0) return std::atol(line.c_str() + n);
  return -1;
}

int64_t g_fakeNow = 0;
int64_t fakeClock() { return g_fakeNow; }

struct SenseItem {
  const char* question;
  std::vector<std::string> options;
  int expected;
};

// Our own English word-sense questions (closed choice).
const std::vector<SenseItem>& senseItems() {
  static const std::vector<SenseItem> items = {
      {"In 'She sat on the bank of the river', what does 'bank' mean?", {"the side of a river", "a place that keeps money"}, 0},
      {"In 'He swung the bat and hit the ball', what is a 'bat'?", {"a flying animal", "a club used in games"}, 1},
      {"In 'The light was too bright for my eyes', what does 'light' mean?", {"not heavy", "brightness that lets us see"}, 1},
      {"In 'Please leave the room now', what does 'leave' mean?", {"go away from", "permission to be absent"}, 0},
      {"In 'The girl wrote a letter to her mother', what is a 'letter'?", {"a sign of the alphabet", "a written message"}, 1},
      {"In 'The soldiers made camp near the hill', what is a 'camp'?", {"a place with tents", "an exaggerated style"}, 0},
      {"In 'He can run very fast', what does 'fast' mean?", {"quickly", "to go without food"}, 0},
      {"In 'The duck swam across the pond', what is a 'duck'?", {"a water bird", "to lower the head quickly"}, 0},
      {"In 'She will book a table for two', what does 'book' mean?", {"a set of printed pages", "to reserve"}, 1},
      {"In 'The children played in the park', what is a 'park'?", {"a public garden", "to leave a car somewhere"}, 0},
  };
  return items;
}

}  // namespace

TEST_CASE("llm: CPUID gate is consistent") {
  const vp::llm::CpuFeatures a = vp::llm::cpuFeatures(), b = vp::llm::cpuFeatures();
  CHECK(a.x86 == b.x86);
  CHECK(a.avx2 == b.avx2);
  CHECK(a.fma == b.fma);
  CHECK(a.f16c == b.f16c);
  CHECK(a.bmi2 == b.bmi2);
  CHECK(a.osAvx == b.osAvx);
  CHECK(vp::llm::cpuSupported() == (a.x86 && a.osAvx && a.avx && a.avx2 && a.fma && a.f16c && a.bmi2));
  if (vp::llm::built()) {
#if defined(__x86_64__) || defined(_M_X64)
    CHECK(a.x86);
#endif
    MESSAGE("cpu: avx " << a.avx << " avx2 " << a.avx2 << " fma " << a.fma << " f16c " << a.f16c << " bmi2 " << a.bmi2
                        << " osAvx " << a.osAvx << " -> supported " << vp::llm::cpuSupported());
  } else {
    CHECK_FALSE(vp::llm::cpuSupported());
  }
}

TEST_CASE("llm: without a model file every call fails with a hint") {
  vp::llm::Model m;
  const vp::llm::Status st = m.status();
  CHECK_FALSE(st.available);
  CHECK_FALSE(st.loaded);
  CHECK_FALSE(m.loaded());
  vp::llm::Config cfg;
  cfg.path = (stdfs::path(VP_TEST_TMP) / "llm" / "no-such-model.gguf").string();
  vp::Result<void> r = m.load(cfg);
  REQUIRE_FALSE(r.ok());
  if (vp::llm::built() && vp::llm::cpuSupported()) CHECK(r.error().code == ErrorCode::ModelMissing);
  if (!vp::llm::built()) CHECK(r.error().code == ErrorCode::ModelMissing);
  CHECK_FALSE(r.error().hint.empty());
  CHECK_FALSE(m.loaded());

  vp::Result<int> c = m.choose("Which word means 'girl'?", {"puella", "puer"}, nullptr);
  REQUIRE_FALSE(c.ok());
  CHECK((c.error().code == ErrorCode::ModelLoadFailed || c.error().code == ErrorCode::ModelMissing));
  CHECK_FALSE(c.error().hint.empty());
  vp::Result<std::string> s = m.simplify("The cat sat on the mat.", "en");
  REQUIRE_FALSE(s.ok());
  CHECK((s.error().code == ErrorCode::ModelLoadFailed || s.error().code == ErrorCode::ModelMissing));
  CHECK_FALSE(s.error().hint.empty());
  vp::Result<vp::llm::TextScore> t = m.scoreText("Latin:", " Puella cantat.");
  CHECK_FALSE(t.ok());
  CHECK_FALSE(m.maybeUnload(1ll << 40));

  vp::Result<vp::llm::FileCheck> fc = vp::llm::checkModelFile(cfg.path, true);
  REQUIRE_FALSE(fc.ok());
  CHECK(fc.error().code == ErrorCode::ModelMissing);

  vp::llm::Advisors adv = vp::llm::makeAdvisors(m, cfg);
  if (vp::llm::built()) {
    REQUIRE(static_cast<bool>(adv));
    vp::Result<int> a = adv.chooseSense("q", {"a", "b"}, nullptr);
    CHECK_FALSE(a.ok());   // the lazy load fails: no file
    CHECK(static_cast<bool>(adv.rerankLatin) == vp::llm::rerankEnabled());
  } else {
    CHECK_FALSE(static_cast<bool>(adv));
  }
}

TEST_CASE("llm: model file checks reject what is not a usable GGUF") {
  if (!vp::llm::built()) return;
  const stdfs::path dir = stdfs::path(VP_TEST_TMP) / "llm";
  std::error_code ec;
  stdfs::create_directories(dir, ec);
  const std::string junk = (dir / "junk.gguf").string(), tiny = (dir / "tiny.gguf").string();
  {
    std::ofstream o(junk, std::ios::binary);
    o << std::string(4096, 'x');
  }
  {
    std::ofstream o(tiny, std::ios::binary);
    const unsigned char head[24] = {'G', 'G', 'U', 'F', 3, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0};
    o.write(reinterpret_cast<const char*>(head), sizeof head);
    o << std::string(1000, '\0');
  }
  vp::Result<vp::llm::FileCheck> a = vp::llm::checkModelFile(junk, true);
  REQUIRE_FALSE(a.ok());
  CHECK(a.error().code == ErrorCode::ModelLoadFailed);
  CHECK_FALSE(a.error().hint.empty());
  vp::Result<vp::llm::FileCheck> b = vp::llm::checkModelFile(tiny, true);
  REQUIRE_FALSE(b.ok());
  CHECK(b.error().code == ErrorCode::ModelLoadFailed);
  vp::llm::Model m;
  vp::llm::Config cfg;
  cfg.path = junk;
  vp::Result<void> r = m.load(cfg);
  REQUIRE_FALSE(r.ok());
  if (vp::llm::cpuSupported()) CHECK(r.error().code == ErrorCode::ModelLoadFailed);
  CHECK_FALSE(m.loaded());
}

TEST_CASE("llm: models/README.md records the expected size and SHA-256") {
  std::ifstream in(stdfs::path(repoDir()) / "models" / "README.md");
  REQUIRE(in.good());
  std::stringstream ss;
  ss << in.rdbuf();
  const std::string text = ss.str();
  CHECK(text.find(vp::llm::kModelSha256) != std::string::npos);
  CHECK(text.find("397,808,192") != std::string::npos);
  CHECK(vp::llm::kModelSizeBytes == 397808192ull);
}

TEST_CASE("llm: Wilson interval") {
  const vp::llm::Interval w = vp::llm::wilson(75, 100);
  CHECK(w.p == doctest::Approx(0.75));
  CHECK(w.lo == doctest::Approx(0.6570).epsilon(0.001));
  CHECK(w.hi == doctest::Approx(0.8246).epsilon(0.001));
  const vp::llm::Interval all = vp::llm::wilson(10, 10);
  CHECK(all.hi == doctest::Approx(1.0));
  CHECK(all.lo > 0.69);
  const vp::llm::Interval none = vp::llm::wilson(0, 0);
  CHECK(none.p == 0.0);
}

TEST_CASE("llm: gate pairs from the Latin lexicon") {
  if (!vp::llm::built()) return;
  const stdfs::path full = stdfs::path(repoDir()) / "data" / "work" / "latin.vpl";
  if (!stdfs::exists(full)) {
    MESSAGE("data/work/latin.vpl absent: gate pair generation skipped");
    return;
  }
  vp::Result<vp::lex::Lexicon> lx = vp::lex::Lexicon::open(full);
  REQUIRE(lx.ok());
  vp::Result<std::vector<vp::llm::GatePair>> a = vp::llm::buildGatePairs(lx.value(), 200, 7);
  vp::Result<std::vector<vp::llm::GatePair>> b = vp::llm::buildGatePairs(lx.value(), 200, 7);
  REQUIRE(a.ok());
  REQUIRE(b.ok());
  REQUIRE(a->size() == 200);
  REQUIRE(b->size() == 200);
  std::set<std::string> templates, bads;
  for (size_t i = 0; i < a->size(); ++i) {
    const vp::llm::GatePair& p = a.value()[i];
    CHECK(p.good == b.value()[i].good);
    CHECK(p.bad == b.value()[i].bad);
    CHECK(p.good != p.bad);
    CHECK(p.good.back() == '.');
    templates.insert(p.templ);
    bads.insert(p.bad);
  }
  CHECK(templates.size() == 4);
  CHECK(bads.size() == 200);
  MESSAGE("example pair: '" << a.value()[0].good << "' vs '" << a.value()[0].bad << "'");
}

TEST_CASE("llm: with the model file (load, choose, simplify, RSS, idle unload)") {
  if (!vp::llm::built()) return;
  const std::string path = modelForTests();
  if (path.empty()) {
    MESSAGE("no model file (set VP_MODEL_GGUF or put a .gguf in models/): model tests skipped");
    return;
  }
  if (!vp::llm::cpuSupported()) {
    MESSAGE("CPU without AVX2: model tests skipped");
    return;
  }
  const auto h0 = std::chrono::steady_clock::now();
  vp::Result<vp::llm::FileCheck> fc = vp::llm::checkModelFile(path, true);
  const auto h1 = std::chrono::steady_clock::now();
  REQUIRE(fc.ok());
  MESSAGE("model file " << path << ": " << fc->sizeBytes << " bytes, sha256ok " << fc->sha256ok << ", hashed in "
                        << std::chrono::duration_cast<std::chrono::milliseconds>(h1 - h0).count() << " ms");

  const long anon0 = statusKb("RssAnon:");
  vp::llm::Model m;
  vp::llm::Config cfg;
  cfg.path = path;
  cfg.clock = fakeClock;
  g_fakeNow = 1000000;
  const auto t0 = std::chrono::steady_clock::now();
  vp::Result<void> r = m.load(cfg);
  const auto t1 = std::chrono::steady_clock::now();
  REQUIRE_MESSAGE(r.ok(), r.error().message);
  const long loadMs = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
  CHECK(loadMs < 3000);
  CHECK(m.loaded());
  CHECK(m.status().loaded);
  CHECK(m.load(cfg).ok());   // same file: no-op

  // 10 English sense questions: report the score, assert determinism.
  int right = 0;
  for (const SenseItem& it : senseItems()) {
    std::vector<float> s1, s2;
    vp::Result<int> a = m.choose(it.question, it.options, &s1);
    vp::Result<int> b = m.choose(it.question, it.options, &s2);
    REQUIRE(a.ok());
    REQUIRE(b.ok());
    CHECK(a.value() == b.value());
    CHECK(s1 == s2);
    REQUIRE(s1.size() == it.options.size());
    if (a.value() == it.expected) ++right;
  }
  MESSAGE("English sense questions: " << right << "/" << senseItems().size() << " right; load " << loadMs << " ms");
  std::vector<float> ts;
  vp::Result<int> girl = m.choose("Which word means 'girl'?", {"puella", "puer", "aqua"}, &ts);
  REQUIRE(girl.ok());
  MESSAGE("'girl' -> option " << girl.value() << " (scores " << ts[0] << ", " << ts[1] << ", " << ts[2] << ")");
  CHECK_FALSE(m.choose("q", {}, nullptr).ok());

  vp::Result<vp::llm::TextScore> sc = m.scoreText("Latin sentence:", " Puella rosam amat.");
  REQUIRE(sc.ok());
  CHECK(sc->tokens > 0);
  CHECK(sc->logProb < 0);
  vp::Result<vp::llm::TextScore> sc2 = m.scoreText("Latin sentence:", " Puella rosam amat.");
  REQUIRE(sc2.ok());
  CHECK(sc->logProb == sc2->logProb);

  for (const char* lang : {"en", "es"}) {
    const char* in = std::string(lang) == "en" ? "Notwithstanding the inclement weather, the expedition proceeded northward."
                                               : "A pesar del mal tiempo, la expedición siguió hacia el norte.";
    vp::Result<std::string> s = m.simplify(in, lang);
    REQUIRE_MESSAGE(s.ok(), s.error().message);
    CHECK_FALSE(s->empty());
    CHECK(s->find('\n') == std::string::npos);
    for (char ch : s.value()) CHECK((ch >= 0x20 && ch < 0x7f));
    vp::Result<std::string> again = m.simplify(in, lang);
    REQUIRE(again.ok());
    CHECK(again.value() == s.value());
    MESSAGE("simplify(" << std::string(lang) << "): " << s.value());
  }
  CHECK_FALSE(m.simplify("", "en").ok());
  CHECK_FALSE(m.simplify("Hola.", "fr").ok());

  // RSS: 50 more choose calls must not grow anonymous memory by 5 %.
  const long anon1 = statusKb("RssAnon:");
  for (int i = 0; i < 50; ++i) {
    const SenseItem& it = senseItems()[static_cast<size_t>(i) % senseItems().size()];
    REQUIRE(m.choose(it.question, it.options, nullptr).ok());
  }
  const long anon2 = statusKb("RssAnon:");
  MESSAGE("RssAnon kB: before load " << anon0 << ", after load + 23 calls " << anon1 << ", after 50 more " << anon2
                                     << "; VmRSS " << statusKb("VmRSS:"));
  if (anon1 > 0) CHECK(anon2 <= anon1 + anon1 / 20);

  // Idle unload with the injected clock.
  m.touch();
  CHECK_FALSE(m.maybeUnload(g_fakeNow + cfg.idleUnloadMs - 1));
  CHECK(m.loaded());
  CHECK(m.maybeUnload(g_fakeNow + cfg.idleUnloadMs));
  CHECK_FALSE(m.loaded());
  const long anon3 = statusKb("RssAnon:");
  MESSAGE("RssAnon kB after unload: " << anon3);
  if (anon0 > 0) CHECK(anon3 <= anon0 + 50 * 1024);

  // Advisors load lazily and leave the model loaded for the job's caller to unload.
  vp::llm::Advisors adv = vp::llm::makeAdvisors(m, cfg);
  vp::Result<int> av = adv.chooseSense(senseItems()[0].question, senseItems()[0].options, nullptr);
  REQUIRE(av.ok());
  CHECK(m.loaded());
  m.unload();
  CHECK_FALSE(m.loaded());
}
