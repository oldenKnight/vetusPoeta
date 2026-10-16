// engine/core: error names, UTF-8 + normalisation (DESIGN.md section 4), fs utils, MappedFile, settings store.
#include <doctest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "vp/fs.h"
#include "vp/mmap.h"
#include "vp/result.h"
#include "vp/settings.h"
#include "vp/sha256.h"
#include "vp/text.h"

#if !defined(_WIN32)
#include <sys/stat.h>
#include <unistd.h>
#endif

#if defined(__SANITIZE_ADDRESS__)
#define VP_TEST_SANITIZED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define VP_TEST_SANITIZED 1
#endif
#endif

namespace stdfs = std::filesystem;
using nlohmann::json;

namespace {

std::string coreTmp(const std::string& name) {
  stdfs::path d = stdfs::path(VP_TEST_TMP) / "core" / name;
  std::error_code ec;
  stdfs::remove_all(d, ec);
  stdfs::create_directories(d);
  return stdfs::absolute(d).string();
}

std::string slurp(const std::string& p) {
  std::ifstream f(p, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

void spit(const std::string& p, const std::string& s) {
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  f.write(s.data(), static_cast<std::streamsize>(s.size()));
}

struct Row {
  std::string fn, input, expected;
  int line;
};

// function \t input \t expected; '#' comments; an optional "function\tinput\texpected" header.
std::vector<Row> loadTsv(const std::string& path) {
  std::vector<Row> rows;
  std::ifstream f(path, std::ios::binary);
  std::string line;
  int n = 0;
  while (std::getline(f, line)) {
    ++n;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || line[0] == '#' || line.rfind("function\t", 0) == 0) continue;
    const size_t a = line.find('\t');
    const size_t b = a == std::string::npos ? a : line.find('\t', a + 1);
    if (b == std::string::npos) continue;
    rows.push_back(Row{line.substr(0, a), line.substr(a + 1, b - a - 1), line.substr(b + 1), n});
  }
  return rows;
}

using Fn = std::function<std::string(const std::string&)>;
const std::map<std::string, Fn>& functions() {
  using namespace vp::text;
  static const std::map<std::string, Fn> m = {
      {"nfc", [](const std::string& s) { return nfc(s); }},
      {"nfd", [](const std::string& s) { return nfd(s); }},
      {"lower", [](const std::string& s) { return lower(s); }},
      {"latin_key", [](const std::string& s) { return latin_key(s); }},
      {"greek_key", [](const std::string& s) { return greek_key(s); }},
      {"greek_bare", [](const std::string& s) { return greek_bare(s); }},
      {"en_key", [](const std::string& s) { return en_key(s); }},
      {"es_key", [](const std::string& s) { return es_key(s); }},
      {"es_bare", [](const std::string& s) { return es_bare(s); }},
      {"display_latin", [](const std::string& s) { return display_latin(s, false); }},
      {"display_latin_plain", [](const std::string& s) { return display_latin(s, false); }},
      {"display_latin_macrons", [](const std::string& s) { return display_latin(s, true); }},
  };
  return m;
}

// Runs every row; returns the number checked. Unknown function names are reported, not failed.
size_t runRows(const std::vector<Row>& rows, const std::string& file) {
  size_t checked = 0;
  for (const Row& r : rows) {
    auto it = functions().find(r.fn);
    if (it == functions().end()) {
      MESSAGE(file << ":" << r.line << " unknown function '" << r.fn << "' skipped");
      continue;
    }
    INFO(file << ":" << r.line << " " << r.fn << "(" << r.input << ")");
    CHECK(it->second(r.input) == r.expected);
    // the caller-provided-output overloads agree with the value overloads, also on reused buffers
    ++checked;
  }
  return checked;
}

}  // namespace

TEST_CASE("core: error code wire names") {
  using vp::ErrorCode;
  CHECK(std::string(vp::errorCodeName(ErrorCode::Ok)) == "ok");
  const std::pair<ErrorCode, const char*> names[] = {
      {ErrorCode::BadParams, "bad_params"},       {ErrorCode::NotFound, "not_found"},
      {ErrorCode::Io, "io"},                      {ErrorCode::UnsupportedFormat, "unsupported_format"},
      {ErrorCode::LexiconMissing, "lexicon_missing"}, {ErrorCode::LexiconCorrupt, "lexicon_corrupt"},
      {ErrorCode::LexiconVersion, "lexicon_version"}, {ErrorCode::ProjectCorrupt, "project_corrupt"},
      {ErrorCode::ModelMissing, "model_missing"}, {ErrorCode::ModelLoadFailed, "model_load_failed"},
      {ErrorCode::ModelUnsupportedCpu, "model_unsupported_cpu"}, {ErrorCode::OnlineDisabled, "online_disabled"},
      {ErrorCode::OnlineFailed, "online_failed"}, {ErrorCode::Busy, "busy"},
      {ErrorCode::Internal, "internal"}};
  for (const auto& p : names) CHECK(std::string(vp::errorCodeName(p.first)) == p.second);
}

TEST_CASE("core: sha256 known answers") {
  CHECK(vp::sha256Hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  CHECK(vp::sha256Hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  CHECK(vp::sha256Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
  std::string million(1000000, 'a');
  vp::Sha256 h;
  for (size_t i = 0; i < million.size(); i += 777) h.update(million.data() + i, std::min<size_t>(777, million.size() - i));
  CHECK(h.finishHex() == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST_CASE("text: utf8 decode/encode") {
  using namespace vp::text;
  CHECK(toUtf8(toUtf32("Iūlius ἀνήρ 😀")) == "Iūlius ἀνήρ 😀");
  CHECK(toUtf32("ā").size() == 1);
  CHECK(isValidUtf8("ā ἀ \xEF\xBF\xBD"));
  CHECK_FALSE(isValidUtf8("\xC0\xAF"));           // overlong
  CHECK_FALSE(isValidUtf8("\xED\xA0\x80"));       // surrogate
  CHECK_FALSE(isValidUtf8("ab\xE2\x82"));         // truncated
  CHECK_FALSE(isValidUtf8("\xF4\x90\x80\x80"));   // > U+10FFFF
  std::u32string u = toUtf32("a\xFF" "b");
  REQUIRE(u.size() == 3);
  CHECK(u[1] == kReplacement);
  std::string out;
  appendUtf8(out, 0xD800);
  CHECK(out == "\xEF\xBF\xBD");
  // malformed input never crashes the normalisers
  CHECK(latin_key("ab\xFF\xFE" "cd") == "abcd");
  CHECK(nfc("\xE2\x82") == "\xEF\xBF\xBD\xEF\xBF\xBD");
}

TEST_CASE("text: contract examples of DESIGN section 4") {
  using namespace vp::text;
  CHECK(latin_key("Iūlius") == "iulius");
  CHECK(latin_key("vir") == "uir");
  CHECK(latin_key("Sīc") == "sic");
  CHECK(latin_key("Iu\xCC\x84lius") == "iulius");   // combining macron
  CHECK(greek_key("ΛΌΓΟΣ") == "λόγοσ");
  CHECK(greek_bare("ἀνήρ") == "ανηρ");
  CHECK(greek_bare("ᾠδῇ") == "ωδη");
  CHECK(en_key("\xE2\x80\x9C" "Don\xE2\x80\x99" "t\xE2\x80\x9D") == "\"don't\"");
  CHECK(es_bare("NIÑO") == "nino");
  CHECK(display_latin("Iūlius", true) == "Iūlius");
  CHECK(display_latin("Iūlius", false) == "Iulius");
  CHECK(display_latin("ROMA", false) == "ROMA");
  CHECK(lower("ΟΔΟΣ ΟΔΟΣ") == "οδος οδος");
  CHECK(nfc("e\xCC\x81") == "é");
  CHECK(nfd("é") == "e\xCC\x81");
  CHECK(std::string(unicodeVersion()).size() >= 5);
}

TEST_CASE("text: core normalisation cases (tests/fixtures/normalisation_core_cases.tsv)") {
  const std::string path = std::string(VP_FIXTURES_DIR) + "/normalisation_core_cases.tsv";
  std::vector<Row> rows = loadTsv(path);
  REQUIRE(rows.size() >= 120);
  CHECK(runRows(rows, "normalisation_core_cases.tsv") == rows.size());
}

TEST_CASE("text: shared golden file (tests/fixtures/normalisation_golden.tsv), when present") {
  const std::string path = std::string(VP_FIXTURES_DIR) + "/normalisation_golden.tsv";
  if (!vp::fs::isRegularFile(path)) {
    MESSAGE("normalisation_golden.tsv absent; skipped");
    return;
  }
  std::vector<Row> rows = loadTsv(path);
  CHECK(rows.size() >= 200);
  const size_t n = runRows(rows, "normalisation_golden.tsv");
  MESSAGE("golden rows checked: " << n << " of " << rows.size());
  CHECK(n == rows.size());
}

TEST_CASE("text: nfc/nfd are idempotent and inverse over the supported ranges") {
  using namespace vp::text;
  const std::pair<char32_t, char32_t> ranges[] = {{0x80, 0x24F}, {0x300, 0x36F}, {0x370, 0x3FF}, {0x1E00, 0x1FFF}};
  size_t n = 0;
  for (const auto& r : ranges) {
    for (char32_t c = r.first; c <= r.second; ++c) {
      std::string s;
      appendUtf8(s, c);
      const std::string d = nfd(s), k = nfc(s);
      if (nfc(k) != k || nfd(d) != d || nfc(d) != k || nfd(k) != d) {
        FAIL_CHECK("U+" << std::hex << static_cast<unsigned>(c));
      }
      ++n;
    }
  }
  CHECK(n == 1232);
  // canonical ordering: the order of marks with different classes does not matter, equal classes keep their order
  CHECK(nfc("a\xCC\xA3\xCC\x82") == nfc("a\xCC\x82\xCC\xA3"));   // dot below (220) + circumflex (230)
  CHECK(nfc("a\xCC\x81\xCC\x80") != nfc("a\xCC\x80\xCC\x81"));   // acute and grave are both 230
}

TEST_CASE("text: out-parameter overloads reuse the buffer") {
  using namespace vp::text;
  std::string out;
  out.reserve(64);
  latin_key("Iūliusque", out);
  const char* buf = out.data();
  const size_t cap = out.capacity();
  for (int i = 0; i < 1000; ++i) {
    latin_key(i % 2 ? "Iūliusque" : "Mārcus", out);
    greek_key("λόγος", out);
  }
  CHECK(out.data() == buf);
  CHECK(out.capacity() == cap);
  latin_key("Iūliusque", out);
  CHECK(out == "iuliusque");
}

TEST_CASE("text: latin_key speed (10-character words, amortised)") {
  using namespace vp::text;
  const char* words[] = {"Iūliusque", "agricolae", "Rōmānōrum", "puellārum", "VIRTVTEM", "fēminīsque"};
  std::string out;
  out.reserve(64);
  size_t sink = 0;
  for (int i = 0; i < 1000; ++i) latin_key(words[i % 6], out);   // warm-up
  const int n = 300000;
  auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < n; ++i) {
    latin_key(words[i % 6], out);
    sink += out.size();
  }
  auto t1 = std::chrono::steady_clock::now();
  const double ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / n;
  MESSAGE("latin_key: " << ns << " ns per word (sink " << sink << ")");
#if !defined(VP_TEST_SANITIZED) && defined(NDEBUG)
  CHECK(ns < 1000.0);
#endif
}

// ---- fs ----------------------------------------------------------------------------------------------------------
TEST_CASE("fs: atomic write, read, size, mtime, utf-8 names") {
  const std::string dir = coreTmp("fs");
  const std::string p = vp::fs::join(dir, "t\xC3\xA9st_\xC4\x81.txt");   // "tést_ā.txt"
  REQUIRE(vp::fs::writeFileAtomic(p, "hello"));
  CHECK(vp::fs::fileExists(p));
  CHECK(vp::fs::isRegularFile(p));
  CHECK_FALSE(vp::fs::isDirectory(p));
  CHECK_FALSE(vp::fs::fileExists(p + ".tmp"));
  auto r = vp::fs::readFile(p);
  REQUIRE(r);
  CHECK(r.value() == "hello");
  REQUIRE(vp::fs::writeFileAtomic(p, std::string("a\0b", 3)));
  CHECK(vp::fs::readFile(p).value() == std::string("a\0b", 3));
  CHECK(vp::fs::fileSize(p).value() == 3);
  auto m = vp::fs::mtime(p);
  REQUIRE(m);
  CHECK(std::llabs(m.value() - vp::fs::nowUnixMs()) < 60000);
  REQUIRE(vp::fs::writeFileAtomic(p, ""));
  CHECK(vp::fs::readFile(p).value().empty());

  auto missing = vp::fs::readFile(vp::fs::join(dir, "nope"));
  CHECK(missing.error().code == vp::ErrorCode::NotFound);
  CHECK_FALSE(missing.error().hint.empty());
  CHECK(vp::fs::fileSize(vp::fs::join(dir, "nope")).error().code == vp::ErrorCode::NotFound);
  CHECK_FALSE(vp::fs::mtime(vp::fs::join(dir, "nope")).ok());
  spit(vp::fs::join(dir, "big"), std::string(100, 'x'));
  CHECK(vp::fs::readFile(vp::fs::join(dir, "big"), 10).error().code == vp::ErrorCode::Io);
}

TEST_CASE("fs: a failed atomic write leaves the target intact and reports io") {
  const std::string dir = coreTmp("fsfail");
  const std::string target = vp::fs::join(dir, "keep.txt");
  REQUIRE(vp::fs::writeFileAtomic(target, "original"));

  // 1) the tmp file cannot be created (a directory sits where it would go)
  stdfs::create_directories(target + ".tmp");
  auto r = vp::fs::writeFileAtomic(target, "new content");
  CHECK(r.error().code == vp::ErrorCode::Io);
  CHECK_FALSE(r.error().hint.empty());
  CHECK(slurp(target) == "original");
  stdfs::remove(target + ".tmp");

  // 2) the parent folder does not exist
  CHECK(vp::fs::writeFileAtomic(vp::fs::join(dir, "no/such/dir/x.txt"), "x").error().code == vp::ErrorCode::Io);

  // 3) a read-only folder (only meaningful when not running as root)
#if !defined(_WIN32)
  const std::string ro = vp::fs::join(dir, "ro");
  stdfs::create_directories(ro);
  REQUIRE(vp::fs::writeFileAtomic(vp::fs::join(ro, "f.txt"), "before"));
  chmod(ro.c_str(), 0555);
  if (geteuid() != 0) {
    auto w = vp::fs::writeFileAtomic(vp::fs::join(ro, "f.txt"), "after");
    CHECK(w.error().code == vp::ErrorCode::Io);
    CHECK(slurp(vp::fs::join(ro, "f.txt")) == "before");
    CHECK_FALSE(vp::fs::fileExists(vp::fs::join(ro, "f.txt.tmp")));
  } else {
#if defined(__linux__)
    // root ignores folder permissions; /proc is a read-only file system even for root
    auto w = vp::fs::writeFileAtomic("/proc/vp_core_test.txt", "x");
    CHECK(w.error().code == vp::ErrorCode::Io);
#else
    MESSAGE("running as root: read-only folder case skipped (covered by cases 1 and 2)");
#endif
  }
  chmod(ro.c_str(), 0755);
#endif
}

TEST_CASE("fs: data and temp folders, pids, clocks") {
  CHECK_FALSE(vp::fs::tempDir().empty());
  CHECK_FALSE(vp::fs::dataDir().empty());
#if !defined(_WIN32)
  const char* old = std::getenv("VP_DATA_DIR");
  std::string saved = old ? old : "";
  setenv("VP_DATA_DIR", "/x/y", 1);
  CHECK(vp::fs::dataDir() == "/x/y");
  unsetenv("VP_DATA_DIR");
  const char* oldXdg = std::getenv("XDG_DATA_HOME");
  std::string savedXdg = oldXdg ? oldXdg : "";
  setenv("XDG_DATA_HOME", "/data/home", 1);
  CHECK(vp::fs::dataDir() == "/data/home/vetus-poeta");
  unsetenv("XDG_DATA_HOME");
  CHECK(vp::fs::dataDir().find("/.local/share/vetus-poeta") != std::string::npos);
  if (!savedXdg.empty()) setenv("XDG_DATA_HOME", savedXdg.c_str(), 1);
  if (!saved.empty()) setenv("VP_DATA_DIR", saved.c_str(), 1);
#endif
  CHECK(vp::fs::isPidAlive(vp::fs::currentPid()));
  CHECK_FALSE(vp::fs::isPidAlive(0));
  const int64_t start = vp::fs::processStartMs(vp::fs::currentPid());
#if defined(__linux__)
  CHECK(start > 0);
  CHECK(vp::fs::isPidAlive(vp::fs::currentPid(), start));
  CHECK_FALSE(vp::fs::isPidAlive(vp::fs::currentPid(), start - 3600000));   // pid reused by another process
#else
  (void)start;
#endif
  CHECK(vp::fs::isoUtc(0) == "1970-01-01T00:00:00Z");
  CHECK(vp::fs::isoUtc(1791280800000LL) == "2026-10-06T10:00:00Z");
}

// ---- mmap --------------------------------------------------------------------------------------------------------
TEST_CASE("mmap: map, read, move, close") {
  const std::string dir = coreTmp("mmap");
  const std::string p = vp::fs::join(dir, "blob.bin");
  std::string data(70000, '\0');
  for (size_t i = 0; i < data.size(); ++i) data[i] = static_cast<char>((i * 31) & 0xFF);
  spit(p, data);

  auto r = vp::MappedFile::open(p);
  REQUIRE(r);
  vp::MappedFile a = std::move(r.value());
  REQUIRE(a.isOpen());
  CHECK(a.size() == data.size());
  CHECK(std::string(reinterpret_cast<const char*>(a.data()), a.size()) == data);

  vp::MappedFile b(std::move(a));
  CHECK_FALSE(a.isOpen());
  CHECK(a.size() == 0);
  CHECK(b.isOpen());
  CHECK(b.data()[12345] == static_cast<uint8_t>((12345 * 31) & 0xFF));

  vp::MappedFile c;
  c = std::move(b);
  CHECK_FALSE(b.isOpen());
  CHECK(c.size() == data.size());
  auto r2 = vp::MappedFile::open(p);
  REQUIRE(r2);
  c = std::move(r2.value());   // move-assign over an open mapping unmaps the old one
  CHECK(c.size() == data.size());
  c.close();
  CHECK_FALSE(c.isOpen());
  c.close();   // idempotent

  spit(vp::fs::join(dir, "empty.bin"), "");
  auto e = vp::MappedFile::open(vp::fs::join(dir, "empty.bin"));
  REQUIRE(e);
  CHECK(e.value().size() == 0);
}

TEST_CASE("mmap: a missing file is io with a hint") {
  auto r = vp::MappedFile::open(vp::fs::join(coreTmp("mmap_missing"), "missing.vpl"));
  CHECK_FALSE(r.ok());
  CHECK(r.error().code == vp::ErrorCode::Io);
  CHECK_FALSE(r.error().hint.empty());
  CHECK(r.error().message.find("missing.vpl") != std::string::npos);
  CHECK(vp::MappedFile::open(coreTmp("mmap_dir")).error().code == vp::ErrorCode::Io);
}

// ---- settings ----------------------------------------------------------------------------------------------------
TEST_CASE("settings: defaults, merge, save, reload, unknown keys") {
  const std::string dir = coreTmp("settings");
  const std::string p = vp::fs::join(dir, "settings.json");
  vp::Settings s(p);
  s.load();
  CHECK(s.warnings().empty());
  json g = s.get();
  CHECK(g["lang"] == "en-US");
  CHECK(g["textScale"] == 100);
  CHECK(g["export"]["macrons"] == false);
  CHECK(g["cps"]["adult"] == 17);
  CHECK(g["cps"]["child"] == 20);
  CHECK(g["autosave"] == true);
  CHECK(g["recentProjects"].is_array());
  CHECK_FALSE(vp::fs::fileExists(p));   // nothing written until set()

  auto r = s.set(json{{"lang", "es-MX"}, {"export", {{"macrons", true}}}, {"engines.model", true}, {"futureKey", 7}});
  REQUIRE(r);
  CHECK(r.value()["lang"] == "es-MX");
  CHECK(r.value()["export"]["macrons"] == true);
  CHECK(r.value()["export"]["encoding"] == "utf-8");   // sibling keys survive a nested patch
  CHECK(r.value()["engines"]["model"] == true);
  CHECK(r.value()["engines"]["online"] == false);
  CHECK(r.value()["futureKey"] == 7);
  CHECK(vp::fs::fileExists(p));

  vp::Settings s2(p);
  s2.load();
  CHECK(s2.warnings().empty());
  CHECK(s2.get() == s.get());

  // null resets to the default; bad types and ranges change nothing
  REQUIRE(s2.set(json{{"lang", nullptr}}));
  CHECK(s2.get()["lang"] == "en-US");
  auto bad = s2.set(json{{"textScale", 300}});
  CHECK(bad.error().code == vp::ErrorCode::BadParams);
  CHECK(s2.set(json{{"cps", {{"adult", "fast"}}}}).error().code == vp::ErrorCode::BadParams);
  CHECK(s2.set(json::array()).error().code == vp::ErrorCode::BadParams);
  CHECK(s2.get()["textScale"] == 100);

  json recent = json::array();
  for (int i = 0; i < 30; ++i) recent.push_back("p" + std::to_string(i) + ".vpoeta");
  auto rr = s2.set(json{{"recentProjects", recent}});
  REQUIRE(rr);
  CHECK(rr.value()["recentProjects"].size() == vp::Settings::kMaxRecentProjects);

  // unknown keys written by a newer version are kept
  json onDisk = json::parse(slurp(p));
  onDisk["fromTheFuture"] = {{"x", 1}};
  spit(p, onDisk.dump());
  vp::Settings s3(p);
  s3.load();
  CHECK(s3.get()["fromTheFuture"]["x"] == 1);
  REQUIRE(s3.set(json{{"eco", true}}));
  CHECK(json::parse(slurp(p))["fromTheFuture"]["x"] == 1);
}

TEST_CASE("settings: latinity is \"wide\" by default and only \"wide\" or \"classical\" (D18, C23)") {
  const std::string dir = coreTmp("settings_latinity");
  const std::string p = vp::fs::join(dir, "settings.json");
  vp::Settings s(p);
  s.load();
  CHECK(s.get()["latinity"] == "wide");
  auto r = s.set(json{{"latinity", "classical"}});
  REQUIRE(r);
  CHECK(r.value()["latinity"] == "classical");
  for (const json& bad : {json("medieval"), json(true), json(1), json("Wide")}) {
    auto b = s.set(json{{"latinity", bad}});
    REQUIRE_FALSE(b);
    CHECK(b.error().code == vp::ErrorCode::BadParams);
    CHECK(b.error().message.find("latinity") != std::string::npos);
  }
  CHECK(s.get()["latinity"] == "classical");   // a refused patch changes nothing
  REQUIRE(s.set(json{{"latinity", nullptr}}));  // null resets to the default
  CHECK(s.get()["latinity"] == "wide");
  // an invalid value on disk is repaired to the default with a warning
  spit(p, "{\"latinity\": \"vulgar\"}");
  s.load();
  CHECK(s.get()["latinity"] == "wide");
  CHECK(s.warnings().size() == 1);
}

TEST_CASE("settings: a corrupt file falls back to defaults with a warning") {
  const std::string dir = coreTmp("settings_corrupt");
  const std::string p = vp::fs::join(dir, "settings.json");
  spit(p, "{\"lang\": \"es-MX\", ");   // truncated
  vp::Settings s(p);
  s.load();
  CHECK(s.get() == vp::Settings::defaults());
  REQUIRE(s.warnings().size() == 1);
  CHECK(vp::fs::fileExists(p + ".corrupt"));
  REQUIRE(s.set(json{{"theme", "dark"}}));

  spit(p, "[1,2,3]");
  s.load();
  CHECK(s.get() == vp::Settings::defaults());
  CHECK_FALSE(s.warnings().empty());

  spit(p, "{\"lang\": \"fr-FR\", \"textScale\": 120, \"export\": 5}");   // valid JSON, invalid values
  s.load();
  CHECK(s.get()["lang"] == "en-US");
  CHECK(s.get()["textScale"] == 120);
  CHECK(s.get()["export"] == vp::Settings::defaults()["export"]);
  CHECK(s.warnings().size() == 2);

  spit(p, std::string("\x00\xFF\xFE garbage", 11));
  s.load();
  CHECK(s.get() == vp::Settings::defaults());
}
