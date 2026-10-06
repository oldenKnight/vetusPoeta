// Data folder discovery. See locate.h.
#include "locate.h"

#include <filesystem>

#include "serve_io.h"
#include "vp/fs.h"

namespace vpcli {

namespace fs = vp::fs;

namespace {
// <exe dir> and up to five parent folders.
std::vector<std::string> exeAncestors() {
  std::vector<std::string> out;
  const std::string exe = exeDir();
  if (exe.empty()) return out;
  std::filesystem::path p = fs::u8path(exe);
  for (int i = 0; i < 6 && !p.empty(); ++i) {
    out.push_back(fs::toU8(p));
    const std::filesystem::path parent = p.parent_path();
    if (parent == p) break;
    p = parent;
  }
  return out;
}

std::string firstWith(const std::vector<std::string>& dirs, const char* file) {
  for (const std::string& d : dirs)
    if (!d.empty() && fs::isRegularFile(fs::join(d, file))) return fs::toU8(fs::u8path(d).lexically_normal());
  return std::string();
}
}  // namespace

bool nlpModelsPresent(const std::string& nlpDir, const char* base) {
  return !nlpDir.empty() && fs::isRegularFile(fs::join(nlpDir, std::string(base) + ".tag.vpt")) &&
         fs::isRegularFile(fs::join(nlpDir, std::string(base) + ".dep.vpt"));
}

std::string defaultLexiconDir(const std::string& dataDir) {
  const std::string env = envU8("VP_LEXICON_DIR");
  if (!env.empty()) return env;
  const std::string exe = exeDir();
  if (!exe.empty()) {
    const std::string data = fs::join(exe, "data");
    if (fs::isRegularFile(fs::join(data, "latin.vpl"))) return data;
    const std::string lex = fs::join(exe, "lexicons");
    if (fs::isDirectory(lex)) return lex;
  }
  return fs::join(dataDir, "lexicons");
}

std::string findNlpDir(const std::string& lexiconDir) {
  const std::string env = envU8("VP_NLP_DIR");
  if (!env.empty()) return env;   // an explicit override is used as given (even when the files are missing)
  std::vector<std::string> dirs;
  if (!lexiconDir.empty()) dirs.push_back(fs::join(lexiconDir, "nlp"));
  if (!exeDir().empty()) dirs.push_back(fs::join(fs::join(exeDir(), "data"), "nlp"));
  for (const std::string& a : exeAncestors()) dirs.push_back(fs::join(fs::join(fs::join(a, "data"), "work"), "nlp"));
  for (const char* base : {"english.tag.vpt", "spanish.tag.vpt"}) {
    const std::string d = firstWith(dirs, base);
    if (!d.empty()) return d;
  }
  return std::string();
}

std::string findCuratedDir(const std::string& lexiconDir) {
  const std::string env = envU8("VP_CURATED_DIR");
  if (!env.empty()) return fs::isRegularFile(fs::join(env, "order_la.txt")) ? env : std::string();
  std::vector<std::string> dirs;
  if (!lexiconDir.empty()) {
    dirs.push_back(fs::join(lexiconDir, "curated"));
    dirs.push_back(fs::join(fs::join(lexiconDir, ".."), "curated"));   // data/work -> data/curated
  }
  if (!exeDir().empty()) dirs.push_back(fs::join(fs::join(exeDir(), "data"), "curated"));
  for (const std::string& a : exeAncestors()) dirs.push_back(fs::join(fs::join(a, "data"), "curated"));
  return firstWith(dirs, "order_la.txt");
}

std::vector<std::string> sampleSourceDirs() {
  std::vector<std::string> dirs, out;
  dirs.push_back(envU8("VP_SAMPLES_DIR"));
  if (!exeDir().empty()) dirs.push_back(fs::join(exeDir(), "samples"));
  for (const std::string& a : exeAncestors()) dirs.push_back(fs::join(fs::join(a, "tests"), "samples"));
  for (const std::string& d : dirs)
    if (!d.empty() && fs::isDirectory(d)) out.push_back(d);
  return out;
}

std::vector<std::pair<std::string, std::string>> installSamples(const std::string& dataDir) {
  std::vector<std::pair<std::string, std::string>> out;
  const std::string dst = fs::join(dataDir, "samples");
  const std::vector<std::string> sources = sampleSourceDirs();
  for (const char* lang : {"en", "es", "la", "grc"}) {
    const std::string name = std::string("sample.") + lang + ".srt";
    const std::string target = fs::join(dst, name);
    if (!fs::isRegularFile(target)) {
      const std::string from = firstWith(sources, name.c_str());
      if (from.empty()) continue;
      vp::Result<std::string> bytes = fs::readFile(fs::join(from, name), 1u << 20);
      if (!bytes || !fs::createDirectories(dst) || !fs::writeFileAtomic(target, bytes.value())) {
        logMsg(LogLevel::Warn, "sample " + name + " could not be copied to " + dst);
        out.emplace_back(lang, fs::join(from, name));
        continue;
      }
    }
    out.emplace_back(lang, target);
  }
  return out;
}

}  // namespace vpcli
