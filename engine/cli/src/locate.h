// Where the engine's data lives (DESIGN 9: engine.hello; dist layout in engine/cli/README.md):
//   <exe>/data/latin.vpl greek.vpl english.vpl spanish.vpl   lexicons (the shell passes --lexicons <exe>/data)
//   <exe>/data/nlp/{english,spanish}.{tag,dep}.vpt            source-language analysis models
//   <exe>/data/curated/*.tsv, *.txt                           teacher-editable rule tables
//   <exe>/samples/sample.<lang>.srt                           sample files (copied to <user data>/samples)
// In the source tree the same files are found from the executable's folder upwards (data/work, data/curated,
// tests/samples), so a dev build runs without flags. Environment overrides: VP_LEXICON_DIR, VP_NLP_DIR,
// VP_CURATED_DIR, VP_SAMPLES_DIR. Nothing here throws.
#pragma once
#include <string>
#include <vector>

namespace vpcli {

std::string defaultLexiconDir(const std::string& dataDir);   // VP_LEXICON_DIR, <exe>/data, <exe>/lexicons, <data>/lexicons
// The folder holding english/spanish *.tag.vpt (first match; VP_NLP_DIR as given); "" when none is found.
std::string findNlpDir(const std::string& lexiconDir);
// The folder of the curated tables (holds order_la.txt); "" when none is found.
std::string findCuratedDir(const std::string& lexiconDir);
// Folders that may hold sample.<lang>.srt, in order of preference (existing ones only).
std::vector<std::string> sampleSourceDirs();
// Copies each sample.<lang>.srt missing from <dataDir>/samples from the first source folder that has it, so the
// UI finds <dataDir>/samples/sample.<lang>.srt. Returns (lang, path) of every sample available afterwards.
std::vector<std::pair<std::string, std::string>> installSamples(const std::string& dataDir);
bool nlpModelsPresent(const std::string& nlpDir, const char* base);   // <base>.tag.vpt and <base>.dep.vpt

}  // namespace vpcli
