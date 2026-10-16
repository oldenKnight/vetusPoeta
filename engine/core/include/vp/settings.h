// User settings store (DESIGN.md section 9.1): one JSON object at dataDir()/settings.json.
// Dotted keys of the design ("export.macrons", "engines.model", "cps.adult", ...) are nested objects in the file
// and in get(); set() accepts either form. Unknown keys are kept. A damaged file never stops the program: the
// defaults are used, the damaged file is kept as settings.json.corrupt and a warning is reported.
#pragma once
#include <mutex>
#include <string>
#include <vector>

#include "json.hpp"
#include "vp/result.h"

namespace vp {

class Settings {
 public:
  static constexpr size_t kMaxRecentProjects = 20;

  explicit Settings(std::string path = defaultPath());
  static std::string defaultPath();          // fs::dataDir()/settings.json
  static nlohmann::json defaults();

  // Reads the file (missing file = defaults). Never fails; problems end up in warnings().
  void load();
  nlohmann::json get() const;                // the full object (defaults filled in)
  // Merges `patch` (RFC 7386: null resets a key to its default), validates the known keys, saves atomically and
  // returns the new full object. bad_params on a wrong type or range (nothing changes), io when saving fails
  // (nothing changes).
  Result<nlohmann::json> set(const nlohmann::json& patch);
  std::vector<std::string> warnings() const;
  const std::string& path() const { return path_; }

 private:
  std::string path_;
  mutable std::mutex mu_;
  nlohmann::json value_;
  std::vector<std::string> warnings_;
};

}  // namespace vp
