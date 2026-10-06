// The project file `.vpoeta` (DESIGN.md section 8): a zip (miniz, deflate, fixed timestamps) holding
//   manifest.json  {format, appVersion, created, pair, kind, sourceFileName, sourceSha256, settings}
//   source.bin     the original file bytes, byte exact
//   cues.jsonl     one CueRecord per line
//   glossary.json  names with policy, form, forms, gender, declension
//   corrections.json  source phrase key -> target phrase, scope, count
//   stats.json     counts for the start screen (free-form object)
// Saving is atomic (`name.vpoeta.tmp`, fsync, rename). Autosave goes to `name.vpoeta.autosave`; the lock file
// `name.vpoeta.lock` holds pid, process start time and the time it was written. Nothing here throws.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "json.hpp"
#include "vp/fs.h"
#include "vp/result.h"

namespace vp {

const char* appVersion();   // the program version written into manifests

enum class ProjectKind { Subs, Text };
const char* projectKindName(ProjectKind k);   // "subs" | "text"

struct Manifest {
  static constexpr int kFormat = 1;   // current format; older files are migrated in memory on open
  int format = kFormat;
  std::string appVersion;
  std::string created;          // ISO UTC, set once by Project::create
  std::string pair;             // "en-la", "la-es", ...
  std::string kind = "subs";    // "subs" | "text"
  std::string sourceFileName;
  std::string sourceSha256;     // of source.bin
  nlohmann::json settingsSnapshot = nlohmann::json::object();
};

struct Alternative { std::string text, reason; double score = 0; };
struct CueCheck { std::string id; bool ok = true; std::string detail; };   // "A1".."A9"
struct CueReason {
  int tokenIndex = -1;
  std::string kind;   // sense | candidate | form | evidence | correction | phrasebook | name
  std::string text;
  std::string data;   // compact JSON text (or empty); kept as a string so no JSON tree lives per cue
};

// One cue's translation state. Strings and numbers only: a project with 10,000 cues holds no JSON trees.
struct CueRecord {
  int index = 0;
  std::string state = "new";        // new | translated | edited | reviewed | stale
  std::string target;
  int chosen = -1;                  // index into alternatives, -1 = none
  std::vector<Alternative> alternatives;
  std::string confidence;           // "" | ok | check | fix
  double score = 0;                 // 0..1
  std::vector<CueCheck> checks;
  std::vector<CueReason> reasons;
  bool edited = false;
  bool reviewed = false;
};

struct NameEntry {
  std::string name;
  std::string policy = "keep";      // keep | decline | translate
  std::string form, gender, declension;
  std::vector<std::string> forms;
};

struct Correction {
  std::string id, key, target;
  std::string scope = "phrase";     // phrase | cue
  int count = 0;
};

// What open() found next to the project.
struct Recovery {
  bool available = false;           // a newer autosave exists and its writer is gone
  std::string autosavePath;
  int64_t atMs = 0;                 // autosave mtime, Unix ms
  bool lockedByLiveProcess = false; // another running process holds the lock
  uint64_t lockPid = 0;
};

class Project;
struct Opened;

class Project {
 public:
  Manifest manifest;
  std::string source;                // source.bin
  std::vector<CueRecord> cues;
  std::vector<NameEntry> glossary;
  std::vector<Correction> corrections;
  nlohmann::json stats = nlohmann::json::object();

  // A new, unsaved project. bad_params for an unknown pair or empty source name.
  static Result<Project> create(ProjectKind kind, const std::string& pair, std::string sourceBytes,
                                const std::string& sourceName,
                                const nlohmann::json& settingsSnapshot = nlohmann::json::object());
  // Opens `path` (not_found, io, project_corrupt with the newest readable autosave named in the hint) and reports
  // whether a recovery is available. Does not write the lock file.
  static Result<Opened> open(const std::string& path);
  // Loads the autosave of `path` (the project file itself may be damaged or missing).
  static Result<Project> recover(const std::string& path);
  // Reads a project from zip bytes already in memory (used by open/recover and the tests).
  static Result<Project> fromZip(const std::string& zipBytes, const std::string& nameForErrors);

  // Atomic save. The same project always produces the same bytes.
  Result<void> save(const std::string& path) const;
  Result<std::string> toZip() const;

  // In-memory upgrade from an older format, one version step at a time (open() calls it and reports it in
  // Opened::warnings). Returns the number of steps applied; manifest.format is Manifest::kFormat afterwards.
  int migrate(int formatFrom);
};

struct Opened {
  Project project;
  Recovery recoverable;
  std::vector<std::string> warnings;
};

std::string autosavePath(const std::string& projectPath);   // path + ".autosave"
std::string lockPath(const std::string& projectPath);       // path + ".lock"

// Writes the lock for this process (pid, process start ms, ISO time). io on failure.
Result<void> writeLock(const std::string& projectPath);
// Writes a lock for an arbitrary pid (tests and tools).
Result<void> writeLockFor(const std::string& projectPath, uint64_t pid, int64_t startMs);
void removeLock(const std::string& projectPath);
// Recovery state of `projectPath` from its lock and autosave (see Recovery).
Recovery checkRecovery(const std::string& projectPath);
// The autosave next to `projectPath` if it opens cleanly, else "".
std::string newestReadableAutosave(const std::string& projectPath);

using fs::isPidAlive;

// Autosave timing (pure logic, no threads, injected clock): a save is due `debounceMs` after the last change, or
// `maxIntervalMs` after the project first became dirty, whichever comes first.
class Autosaver {
 public:
  explicit Autosaver(int64_t debounceMs = 5000, int64_t maxIntervalMs = 60000)
      : debounceMs_(debounceMs), maxIntervalMs_(maxIntervalMs) {}
  void noteChange(int64_t nowMs);
  bool due(int64_t nowMs) const;    // a save should happen now
  void saved(int64_t nowMs);        // the save succeeded: clean again
  void saveFailed(int64_t nowMs);   // retry one debounce interval later
  bool dirty() const { return dirty_; }

 private:
  int64_t debounceMs_, maxIntervalMs_;
  bool dirty_ = false;
  int64_t lastChangeMs_ = 0, dirtySinceMs_ = 0;
};

}  // namespace vp
