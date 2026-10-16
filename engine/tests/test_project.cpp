// engine/core project format (.vpoeta, DESIGN.md section 8): round trip, byte stability, corruption, lock/recovery,
// autosave timing, migration.
#include <doctest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "miniz.h"
#include "vp/fs.h"
#include "vp/project.h"
#include "vp/sha256.h"

#if !defined(_WIN32)
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace stdfs = std::filesystem;
using nlohmann::json;

namespace {

std::string projTmp(const std::string& name) {
  stdfs::path d = stdfs::path(VP_TEST_TMP) / "project" / name;
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

std::string fixture(const std::string& name) { return std::string(VP_FIXTURES_DIR) + "/project/" + name; }

vp::Project sampleProject() {
  // our own sentences; CRLF, BOM and a stray invalid byte must survive byte for byte
  std::string src = "\xEF\xBB\xBF" "1\r\n00:00:01,000 --> 00:00:02,000\r\nThe girl sees the sea.\r\n\r\n"
                    "2\r\n00:00:03,000 --> 00:00:04,000\r\n<i>Good morning\xFF</i>\r\n";
  auto r = vp::Project::create(vp::ProjectKind::Subs, "en-la", src, "sample.srt", json{{"defaultFidelity", 2}});
  REQUIRE(r);
  vp::Project p = std::move(r.value());
  for (int i = 1; i <= 40; ++i) {
    vp::CueRecord c;
    c.index = i;
    c.state = i % 3 ? "translated" : "edited";
    c.target = "Puella mare videt " + std::to_string(i) + ". Iūlia ἀνήρ";
    c.chosen = i % 2;
    c.alternatives = {{"Puella mare videt.", "rules", 0.9}, {"Puella mare spectat.", "synonym", 0.1 * (i % 7)}};
    c.confidence = i % 5 ? "ok" : "fix";
    c.score = 1.0 / static_cast<double>(i);
    c.checks = {{"A1", true, ""}, {"A4", i % 4 != 0, "case after preposition"}};
    c.reasons = {{0, "sense", "see -> videre", "{\"lemma\":\"video\",\"score\":0.25}"}, {2, "form", "acc. sg.", ""}};
    c.edited = i % 3 == 0;
    c.reviewed = i % 7 == 0;
    p.cues.push_back(std::move(c));
  }
  vp::NameEntry n;
  n.name = "Julia";
  n.policy = "decline";
  n.form = "Iūlia";
  n.forms = {"Iūliae", "Iūliam"};
  n.gender = "f";
  n.declension = "1";
  p.glossary.push_back(n);
  p.corrections.push_back({"c1", "runs home", "domum currit", "phrase", 3});
  p.stats = json{{"cues", 40}, {"translated", 27}};
  return p;
}

void checkSame(const vp::Project& a, const vp::Project& b) {
  CHECK(a.manifest.format == b.manifest.format);
  CHECK(a.manifest.created == b.manifest.created);
  CHECK(a.manifest.pair == b.manifest.pair);
  CHECK(a.manifest.kind == b.manifest.kind);
  CHECK(a.manifest.sourceFileName == b.manifest.sourceFileName);
  CHECK(a.manifest.sourceSha256 == b.manifest.sourceSha256);
  CHECK(a.manifest.settingsSnapshot == b.manifest.settingsSnapshot);
  CHECK(a.source == b.source);
  REQUIRE(a.cues.size() == b.cues.size());
  for (size_t i = 0; i < a.cues.size(); ++i) {
    const vp::CueRecord &x = a.cues[i], &y = b.cues[i];
    CHECK(x.index == y.index);
    CHECK(x.state == y.state);
    CHECK(x.target == y.target);
    CHECK(x.chosen == y.chosen);
    REQUIRE(x.alternatives.size() == y.alternatives.size());
    for (size_t k = 0; k < x.alternatives.size(); ++k) {
      CHECK(x.alternatives[k].text == y.alternatives[k].text);
      CHECK(x.alternatives[k].score == y.alternatives[k].score);
    }
    CHECK(x.confidence == y.confidence);
    CHECK(x.score == y.score);
    REQUIRE(x.checks.size() == y.checks.size());
    for (size_t k = 0; k < x.checks.size(); ++k) CHECK(x.checks[k].ok == y.checks[k].ok);
    REQUIRE(x.reasons.size() == y.reasons.size());
    for (size_t k = 0; k < x.reasons.size(); ++k) {
      CHECK(x.reasons[k].kind == y.reasons[k].kind);
      CHECK(x.reasons[k].data == y.reasons[k].data);
    }
    CHECK(x.edited == y.edited);
    CHECK(x.reviewed == y.reviewed);
  }
  REQUIRE(a.glossary.size() == b.glossary.size());
  for (size_t i = 0; i < a.glossary.size(); ++i) {
    CHECK(a.glossary[i].name == b.glossary[i].name);
    CHECK(a.glossary[i].forms == b.glossary[i].forms);
  }
  REQUIRE(a.corrections.size() == b.corrections.size());
  for (size_t i = 0; i < a.corrections.size(); ++i) CHECK(a.corrections[i].count == b.corrections[i].count);
  CHECK(a.stats == b.stats);
}

// Rewrites a zip, replacing (value) or dropping (empty optional = "") named entries.
std::string rebuildZip(const std::string& zip, const std::map<std::string, std::string>& replace,
                       const std::vector<std::string>& drop = {}) {
  mz_zip_archive in;
  mz_zip_zero_struct(&in);
  REQUIRE(mz_zip_reader_init_mem(&in, zip.data(), zip.size(), 0));
  mz_zip_archive out;
  mz_zip_zero_struct(&out);
  REQUIRE(mz_zip_writer_init_heap(&out, 0, 0));
  for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&in); ++i) {
    mz_zip_archive_file_stat st;
    REQUIRE(mz_zip_reader_file_stat(&in, i, &st));
    const std::string name = st.m_filename;
    if (std::find(drop.begin(), drop.end(), name) != drop.end()) continue;
    std::string data;
    auto it = replace.find(name);
    if (it != replace.end()) {
      data = it->second;
    } else {
      size_t n = 0;
      void* p = mz_zip_reader_extract_to_heap(&in, i, &n, 0);
      REQUIRE(p != nullptr);
      data.assign(static_cast<const char*>(p), n);
      mz_free(p);
    }
    REQUIRE(mz_zip_writer_add_mem(&out, name.c_str(), data.data(), data.size(), MZ_DEFAULT_LEVEL));
  }
  void* buf = nullptr;
  size_t size = 0;
  REQUIRE(mz_zip_writer_finalize_heap_archive(&out, &buf, &size));
  std::string result(static_cast<const char*>(buf), size);
  mz_free(buf);
  mz_zip_writer_end(&out);
  mz_zip_reader_end(&in);
  return result;
}

#if !defined(_WIN32)
uint64_t deadPid() {
  pid_t pid = fork();
  if (pid == 0) _exit(0);
  int status = 0;
  waitpid(pid, &status, 0);
  return static_cast<uint64_t>(pid);
}
#endif

void setMtime(const std::string& path, int secondsFromNow) {
  std::error_code ec;
  stdfs::last_write_time(path, stdfs::file_time_type::clock::now() + std::chrono::seconds(secondsFromNow), ec);
  REQUIRE_FALSE(ec);
}

}  // namespace

TEST_CASE("project: create validates its input") {
  CHECK(vp::Project::create(vp::ProjectKind::Subs, "xx-yy", "abc", "a.srt").error().code == vp::ErrorCode::BadParams);
  auto r = vp::Project::create(vp::ProjectKind::Text, "la-es", "Puella cantat.", "");
  REQUIRE(r);
  CHECK(r.value().manifest.kind == "text");
  CHECK(r.value().manifest.format == vp::Manifest::kFormat);
  CHECK(r.value().manifest.sourceSha256 == vp::sha256Hex("Puella cantat."));
  CHECK(r.value().manifest.created.size() == 20);
  CHECK(std::string(vp::appVersion()).size() >= 5);
}

TEST_CASE("project: round trip is byte identical and keeps every field") {
  const std::string dir = projTmp("roundtrip");
  const std::string a = vp::fs::join(dir, "a.vpoeta");
  const std::string b = vp::fs::join(dir, "b.vpoeta");
  const std::string c = vp::fs::join(dir, "c.vpoeta");
  vp::Project p = sampleProject();
  REQUIRE(p.save(a));
  REQUIRE(p.save(b));
  CHECK_FALSE(vp::fs::fileExists(a + ".tmp"));
  const std::string ha = vp::sha256Hex(slurp(a));
  CHECK(ha == vp::sha256Hex(slurp(b)));

  auto o = vp::Project::open(a);
  REQUIRE(o);
  CHECK(o.value().warnings.empty());
  CHECK_FALSE(o.value().recoverable.available);
  checkSame(p, o.value().project);
  REQUIRE(o.value().project.save(c));
  CHECK(vp::sha256Hex(slurp(c)) == ha);   // open + save reproduces the same bytes

  // the zip really is a zip with the six entries in a fixed order
  const std::string z = slurp(a);
  mz_zip_archive in;
  mz_zip_zero_struct(&in);
  REQUIRE(mz_zip_reader_init_mem(&in, z.data(), z.size(), 0));
  const char* names[] = {"manifest.json", "source.bin", "cues.jsonl", "glossary.json", "corrections.json", "stats.json"};
  REQUIRE(mz_zip_reader_get_num_files(&in) == 6);
  for (mz_uint i = 0; i < 6; ++i) {
    char name[64];
    mz_zip_reader_get_filename(&in, i, name, sizeof name);
    CHECK(std::string(name) == names[i]);
  }
  size_t n = 0;
  void* m = mz_zip_reader_extract_file_to_heap(&in, "manifest.json", &n, 0);
  REQUIRE(m);
  json manifest = json::parse(std::string(static_cast<const char*>(m), n));
  mz_free(m);
  mz_zip_reader_end(&in);
  CHECK(manifest["format"] == 1);
  CHECK(manifest["pair"] == "en-la");
  CHECK(manifest["sourceFileName"] == "sample.srt");
  CHECK(manifest["settings"]["defaultFidelity"] == 2);

  // overwriting an existing project is atomic and leaves no tmp file
  p.cues[0].target = "Mūtātum.";
  REQUIRE(p.save(a));
  CHECK(vp::Project::open(a).value().project.cues[0].target == "Mūtātum.");
  CHECK_FALSE(vp::fs::fileExists(a + ".tmp"));
}

TEST_CASE("project: files written by another zip writer (tests/fixtures/project)") {
  auto o = vp::Project::open(fixture("format1_python.vpoeta"));
  REQUIRE(o);
  const vp::Project& p = o.value().project;
  CHECK(p.source == slurp(fixture("sample.srt")));
  REQUIRE(p.cues.size() == 3);
  CHECK(p.cues[0].target == "Puella mare videt.");
  REQUIRE(p.cues[0].alternatives.size() == 2);
  CHECK(p.cues[0].alternatives[1].score == doctest::Approx(0.6));
  REQUIRE(p.cues[0].reasons.size() == 1);
  CHECK(json::parse(p.cues[0].reasons[0].data)["lemma"] == "video");
  CHECK(p.cues[1].edited);
  CHECK(p.cues[1].reviewed);
  REQUIRE(p.glossary.size() == 1);
  CHECK(p.glossary[0].form == "Iūlia");
  REQUIRE(p.corrections.size() == 1);
  CHECK(p.corrections[0].count == 2);
  CHECK(p.stats["cues"] == 3);
  CHECK(o.value().warnings.empty());

  auto nm = vp::Project::open(fixture("no_manifest.vpoeta"));
  CHECK(nm.error().code == vp::ErrorCode::ProjectCorrupt);
  CHECK(nm.error().message.find("manifest.json is missing") != std::string::npos);
}

TEST_CASE("project: migration stub runs for format 0") {
  auto o = vp::Project::open(fixture("format0.vpoeta"));
  REQUIRE(o);
  CHECK(o.value().project.manifest.format == vp::Manifest::kFormat);
  REQUIRE(o.value().warnings.size() == 1);
  CHECK(o.value().warnings[0].find("format 0") != std::string::npos);
  CHECK(o.value().project.cues.size() == 3);

  vp::Project p = sampleProject();
  p.manifest.format = 0;
  CHECK(p.migrate(0) == 1);
  CHECK(p.manifest.format == vp::Manifest::kFormat);
  CHECK(p.migrate(vp::Manifest::kFormat) == 0);
}

TEST_CASE("project: damaged files are project_corrupt, never a crash") {
  const std::string dir = projTmp("corrupt");
  vp::Project p = sampleProject();
  auto zr = p.toZip();
  REQUIRE(zr);
  const std::string zip = zr.value();

  SUBCASE("truncated at 50 random points") {
    std::mt19937 rng(20261006u);
    std::uniform_int_distribution<size_t> cut(0, zip.size() - 1);
    int corrupt = 0, opened = 0;
    for (int i = 0; i < 50; ++i) {
      const std::string path = vp::fs::join(dir, "cut" + std::to_string(i) + ".vpoeta");
      spit(path, zip.substr(0, i == 0 ? 0 : cut(rng)));
      auto o = vp::Project::open(path);
      if (o) {
        ++opened;
        checkSame(p, o.value().project);
      } else {
        ++corrupt;
        CHECK(o.error().code == vp::ErrorCode::ProjectCorrupt);
        CHECK_FALSE(o.error().hint.empty());
      }
    }
    CHECK(corrupt + opened == 50);
    CHECK(corrupt > 0);
  }

  SUBCASE("single flipped bytes") {
    std::mt19937 rng(7u);
    std::uniform_int_distribution<size_t> pos(0, zip.size() - 1);
    for (int i = 0; i < 50; ++i) {
      std::string z = zip;
      z[pos(rng)] ^= static_cast<char>(1 + (i % 255));
      auto r = vp::Project::fromZip(z, "flip");
      if (!r) {
        const bool known = r.error().code == vp::ErrorCode::ProjectCorrupt ||
                           r.error().code == vp::ErrorCode::UnsupportedFormat;
        CHECK(known);
      }
    }
  }

  SUBCASE("structurally valid zips with bad content") {
    auto bad = [&](const std::map<std::string, std::string>& rep, const std::vector<std::string>& drop = {}) {
      return vp::Project::fromZip(rebuildZip(zip, rep, drop), "x").error();
    };
    CHECK(bad({}, {"source.bin"}).code == vp::ErrorCode::ProjectCorrupt);
    CHECK(bad({}, {"cues.jsonl"}).code == vp::ErrorCode::ProjectCorrupt);
    CHECK(bad({{"source.bin", "tampered"}}).message.find("checksum") != std::string::npos);
    CHECK(bad({{"manifest.json", "not json"}}).code == vp::ErrorCode::ProjectCorrupt);
    CHECK(bad({{"manifest.json", "{\"pair\":\"en-la\"}"}}).code == vp::ErrorCode::ProjectCorrupt);
    CHECK(bad({{"manifest.json", "{\"format\":\"one\"}"}}).code == vp::ErrorCode::ProjectCorrupt);
    CHECK(bad({{"manifest.json", "{\"format\":99}"}}).code == vp::ErrorCode::UnsupportedFormat);
    CHECK(bad({{"cues.jsonl", "{\"index\":1}\n{\"index\":\"two\"}\n"}}).message.find("line 2") != std::string::npos);
    CHECK(bad({{"cues.jsonl", "{\"index\":1,\"alternatives\":[1]}\n"}}).code == vp::ErrorCode::ProjectCorrupt);
    CHECK(bad({{"cues.jsonl", "[}\n"}}).code == vp::ErrorCode::ProjectCorrupt);
    CHECK(bad({{"glossary.json", "{}"}}).code == vp::ErrorCode::ProjectCorrupt);
    CHECK(bad({{"stats.json", "[]"}}).code == vp::ErrorCode::ProjectCorrupt);
    // optional entries may be absent
    auto ok = vp::Project::fromZip(rebuildZip(zip, {}, {"glossary.json", "corrections.json", "stats.json"}), "x");
    REQUIRE(ok);
    CHECK(ok.value().glossary.empty());
    CHECK(ok.value().cues.size() == 40);
    // unknown entries are ignored
    CHECK(vp::Project::fromZip(rebuildZip(zip, {{"future.bin", "x"}}), "x").ok());
  }

  SUBCASE("a damaged project names its readable autosave") {
    const std::string path = vp::fs::join(dir, "damaged.vpoeta");
    spit(path, zip.substr(0, zip.size() / 2));
    auto o1 = vp::Project::open(path);
    CHECK(o1.error().code == vp::ErrorCode::ProjectCorrupt);
    CHECK(o1.error().hint.find(".autosave") == std::string::npos);
    REQUIRE(p.save(vp::autosavePath(path)));
    auto o2 = vp::Project::open(path);
    CHECK(o2.error().code == vp::ErrorCode::ProjectCorrupt);
    CHECK(o2.error().hint.find(vp::autosavePath(path)) != std::string::npos);
    auto rec = vp::Project::recover(path);
    REQUIRE(rec);
    checkSame(p, rec.value());
  }

  CHECK(vp::Project::open(vp::fs::join(dir, "absent.vpoeta")).error().code == vp::ErrorCode::NotFound);
  CHECK(vp::Project::recover(vp::fs::join(dir, "absent.vpoeta")).error().code == vp::ErrorCode::NotFound);
}

TEST_CASE("project: lock file and recovery detection") {
  const std::string dir = projTmp("lock");
  const std::string path = vp::fs::join(dir, "p.vpoeta");
  vp::Project p = sampleProject();
  REQUIRE(p.save(path));
  CHECK(vp::lockPath(path) == path + ".lock");
  CHECK(vp::autosavePath(path) == path + ".autosave");

  // no lock, no autosave
  vp::Recovery r = vp::checkRecovery(path);
  CHECK_FALSE(r.available);
  CHECK_FALSE(r.lockedByLiveProcess);

  // our own lock is never "another window"
  REQUIRE(vp::writeLock(path));
  json lock = json::parse(slurp(vp::lockPath(path)));
  CHECK(lock["pid"] == vp::fs::currentPid());
  CHECK(lock.contains("time"));
  CHECK_FALSE(vp::checkRecovery(path).lockedByLiveProcess);

  // autosave older than the project, own lock: nothing to recover
  vp::Project newer = p;
  newer.cues[0].target = "Autosalvātum.";
  REQUIRE(newer.save(vp::autosavePath(path)));
  setMtime(vp::autosavePath(path), -120);
  CHECK_FALSE(vp::checkRecovery(path).available);

  // autosave newer than the project: recoverable
  setMtime(vp::autosavePath(path), 120);
  r = vp::checkRecovery(path);
  CHECK(r.available);
  CHECK(r.autosavePath == vp::autosavePath(path));
  CHECK(r.atMs > 0);
  vp::removeLock(path);
  CHECK_FALSE(vp::fs::fileExists(vp::lockPath(path)));
  CHECK(vp::checkRecovery(path).available);   // no lock at all, newer autosave

#if !defined(_WIN32)
  // a lock left by a dead process with an older autosave: recoverable (the writer crashed)
  setMtime(vp::autosavePath(path), -120);
  const uint64_t dead = deadPid();
  CHECK_FALSE(vp::isPidAlive(dead));
  REQUIRE(vp::writeLockFor(path, dead, 0));
  r = vp::checkRecovery(path);
  CHECK(r.available);
  CHECK_FALSE(r.lockedByLiveProcess);
  CHECK(r.lockPid == dead);
  auto o = vp::Project::open(path);
  REQUIRE(o);
  CHECK(o.value().recoverable.available);
  CHECK(o.value().project.cues[0].target == p.cues[0].target);   // open() reads the project, not the autosave
  auto rec = vp::Project::recover(path);
  REQUIRE(rec);
  CHECK(rec.value().cues[0].target == "Autosalvātum.");

  // a lock held by another live process: not recoverable, reported as open elsewhere
  setMtime(vp::autosavePath(path), 120);
  REQUIRE(vp::writeLockFor(path, static_cast<uint64_t>(getppid()), 0));
  r = vp::checkRecovery(path);
  CHECK_FALSE(r.available);
  CHECK(r.lockedByLiveProcess);
  auto o2 = vp::Project::open(path);
  REQUIRE(o2);
  CHECK_FALSE(o2.value().recoverable.available);
  CHECK(o2.value().warnings.size() == 1);
#endif

  // an unreadable lock counts as left behind by a dead process
  spit(vp::lockPath(path), "garbage");
  setMtime(vp::autosavePath(path), -120);
  CHECK(vp::checkRecovery(path).available);
  vp::removeLock(path);

  // the autosave of a project that was never saved
  const std::string fresh = vp::fs::join(dir, "never_saved.vpoeta");
  REQUIRE(p.save(vp::autosavePath(fresh)));
  CHECK(vp::checkRecovery(fresh).available);
}

TEST_CASE("project: autosave debounce with an injected clock") {
  vp::Autosaver a;   // 5 s debounce, 60 s max
  CHECK_FALSE(a.dirty());
  CHECK_FALSE(a.due(1000000));

  a.noteChange(1000);
  CHECK(a.dirty());
  CHECK_FALSE(a.due(1000));
  CHECK_FALSE(a.due(5999));
  CHECK(a.due(6000));
  a.saved(6000);
  CHECK_FALSE(a.dirty());
  CHECK_FALSE(a.due(100000));

  // a change every second keeps postponing the debounce, but the 60 s cap forces a save
  int64_t t = 100000;
  a.noteChange(t);
  bool forced = false;
  for (int i = 1; i <= 70; ++i) {
    t = 100000 + i * 1000;
    if (a.due(t)) {
      forced = true;
      break;
    }
    a.noteChange(t);
  }
  CHECK(forced);
  CHECK(t == 160000);
  a.saved(t);

  // a failed save retries one debounce later
  a.noteChange(200000);
  REQUIRE(a.due(205000));
  a.saveFailed(205000);
  CHECK(a.dirty());
  CHECK_FALSE(a.due(209999));
  CHECK(a.due(210000));

  vp::Autosaver quick(100, 1000);
  quick.noteChange(0);
  CHECK(quick.due(100));
}
