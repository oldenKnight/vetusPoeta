// Settings store. See vp/settings.h and DESIGN.md section 9.1.
#include "vp/settings.h"

#include <utility>

#include "vp/fs.h"

namespace vp {

namespace {

using json = nlohmann::json;

const char* const kPairs[] = {"en-la", "es-la", "la-en", "la-es", "en-grc", "es-grc", "grc-en", "grc-es", "la-la"};

bool isPair(const std::string& s) {
  for (const char* p : kPairs)
    if (s == p) return true;
  return false;
}

bool intIn(const json& v, long long lo, long long hi) {
  if (!v.is_number_integer()) return false;
  const long long x = v.get<long long>();
  return x >= lo && x <= hi;
}

// "" when `v` is a valid value for the known key `key` (dotted), else the reason. Unknown keys are always valid.
std::string check(const std::string& key, const json& v) {
  auto boolean = [&]() { return v.is_boolean() ? std::string() : "must be true or false"; };
  auto str = [&]() { return v.is_string() ? std::string() : "must be a string"; };
  if (key == "lang") return v.is_string() && (v == "en-US" || v == "es-MX") ? "" : "must be \"en-US\" or \"es-MX\"";
  if (key == "theme")
    return v.is_string() && (v == "auto" || v == "light" || v == "dark") ? "" : "must be \"auto\", \"light\" or \"dark\"";
  if (key == "textScale") return intIn(v, 90, 140) ? "" : "must be an integer from 90 to 140";
  if (key == "defaultPair") return v.is_string() && isPair(v.get<std::string>()) ? "" : "must be a language pair";
  if (key == "defaultFidelity") return intIn(v, 1, 3) ? "" : "must be 1, 2 or 3";
  if (key == "latinity") return v.is_string() && (v == "wide" || v == "classical") ? "" : "must be \"wide\" or \"classical\"";   // D18
  if (key == "cps.adult" || key == "cps.child") return intIn(v, 5, 60) ? "" : "must be an integer from 5 to 60";
  if (key == "export.encoding" || key == "modelPath" || key == "tourSeenVersion") return str();
  if (key == "showMacrons" || key == "showEmoji" || key == "grammarColours" || key == "export.emoji" ||
      key == "export.macrons" || key == "export.bom" || key == "export.rebreak" || key == "engines.model" ||
      key == "engines.online" || key == "online.wiktionary" || key == "online.latinitium" || key == "eco" ||
      key == "autosave")
    return boolean();
  if (key == "recentProjects") {
    if (!v.is_array()) return "must be a list of paths";
    for (const json& e : v)
      if (!e.is_string()) return "must be a list of paths";
    return "";
  }
  if (key == "export" || key == "engines" || key == "online" || key == "cps")
    return v.is_object() ? "" : "must be an object";
  return "";
}

// Fills missing keys of `v` from `d` (recursively for objects).
void fillDefaults(json& v, const json& d) {
  for (auto it = d.begin(); it != d.end(); ++it) {
    auto f = v.find(it.key());
    if (f == v.end() || f->is_null()) {
      v[it.key()] = it.value();
    } else if (it.value().is_object() && f->is_object()) {
      fillDefaults(*f, it.value());
    }
  }
}

// Validates every known key of `v` (two levels). With `repair`, invalid values are replaced by defaults and named in
// `problems`; without, the first problem is returned in `problems` and nothing changes.
void validate(json& v, const json& d, bool repair, std::vector<std::string>& problems) {
  for (auto it = d.begin(); it != d.end(); ++it) {
    json& cur = v[it.key()];
    std::string why = check(it.key(), cur);
    if (!why.empty()) {
      problems.push_back("setting '" + it.key() + "' " + why);
      if (!repair) return;
      cur = it.value();
      continue;
    }
    if (!it.value().is_object()) continue;
    for (auto jt = it.value().begin(); jt != it.value().end(); ++jt) {
      json& sub = cur[jt.key()];
      std::string w = check(it.key() + "." + jt.key(), sub);
      if (!w.empty()) {
        problems.push_back("setting '" + it.key() + "." + jt.key() + "' " + w);
        if (!repair) return;
        sub = jt.value();
      }
    }
  }
  json& rp = v["recentProjects"];
  if (rp.is_array() && rp.size() > Settings::kMaxRecentProjects) rp.erase(rp.begin() + Settings::kMaxRecentProjects, rp.end());
}

// {"export.macrons": true} -> {"export": {"macrons": true}} (only for the first dot; deeper keys stay as written).
json expandDotted(const json& patch) {
  json out = json::object();
  for (auto it = patch.begin(); it != patch.end(); ++it) {
    const std::string& k = it.key();
    const size_t dot = k.find('.');
    if (dot == std::string::npos || dot == 0 || dot + 1 == k.size()) {
      if (out.contains(k) && out[k].is_object() && it.value().is_object()) {
        out[k].update(it.value());
      } else {
        out[k] = it.value();
      }
      continue;
    }
    json& parent = out[k.substr(0, dot)];
    if (!parent.is_object()) parent = json::object();
    parent[k.substr(dot + 1)] = it.value();
  }
  return out;
}

}  // namespace

Settings::Settings(std::string path) : path_(std::move(path)), value_(defaults()) {}

std::string Settings::defaultPath() { return fs::join(fs::dataDir(), "settings.json"); }

json Settings::defaults() {
  return json{
      {"lang", "en-US"},
      {"theme", "auto"},
      {"textScale", 100},
      {"showMacrons", true},
      {"showEmoji", true},
      {"grammarColours", true},
      {"defaultPair", "en-la"},
      {"defaultFidelity", 2},
      {"latinity", "wide"},   // D18: medieval and ecclesiastical Latin accepted ("classical" avoids it)
      {"export", {{"emoji", false}, {"macrons", false}, {"encoding", "utf-8"}, {"bom", false}, {"rebreak", true}}},
      {"engines", {{"model", false}, {"online", false}}},
      {"online", {{"wiktionary", false}, {"latinitium", false}}},
      {"modelPath", ""},
      {"eco", false},
      {"autosave", true},
      {"cps", {{"adult", 17}, {"child", 20}}},
      {"tourSeenVersion", ""},
      {"recentProjects", json::array()},
  };
}

void Settings::load() {
  std::lock_guard<std::mutex> lock(mu_);
  warnings_.clear();
  value_ = defaults();
  try {
    if (!fs::fileExists(path_)) return;
    Result<std::string> text = fs::readFile(path_, 16ull << 20);
    if (!text) {
      warnings_.push_back("settings could not be read (" + text.error().message + "); defaults are used");
      return;
    }
    json j = json::parse(text.value(), nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
      warnings_.push_back("the settings file was damaged; defaults are used (kept as settings.json.corrupt)");
      (void)fs::replaceFile(path_, path_ + ".corrupt");
      return;
    }
    const json d = defaults();
    fillDefaults(j, d);
    std::vector<std::string> problems;
    validate(j, d, true, problems);
    for (const std::string& p : problems) warnings_.push_back(p + "; the default is used");
    value_ = std::move(j);
  } catch (const std::exception& e) {
    value_ = defaults();
    warnings_.push_back(std::string("settings could not be read (") + e.what() + "); defaults are used");
  } catch (...) {
    value_ = defaults();
    warnings_.push_back("settings could not be read; defaults are used");
  }
}

json Settings::get() const {
  std::lock_guard<std::mutex> lock(mu_);
  return value_;
}

std::vector<std::string> Settings::warnings() const {
  std::lock_guard<std::mutex> lock(mu_);
  return warnings_;
}

Result<json> Settings::set(const json& patch) {
  std::lock_guard<std::mutex> lock(mu_);
  try {
    if (!patch.is_object())
      return Result<json>(ErrorCode::BadParams, "settings patch must be a JSON object", "This setting cannot be changed.");
    json next = value_;
    next.merge_patch(expandDotted(patch));
    const json d = defaults();
    fillDefaults(next, d);
    std::vector<std::string> problems;
    validate(next, d, false, problems);
    if (!problems.empty())
      return Result<json>(ErrorCode::BadParams, problems.front(), "This value is not allowed for the setting.");
    Result<void> dir = fs::createDirectories(fs::toU8(fs::u8path(path_).parent_path()));
    if (!dir && !fs::u8path(path_).parent_path().empty()) return Result<json>(dir.error());
    Result<void> w = fs::writeFileAtomic(path_, next.dump(2) + "\n");
    if (!w) return Result<json>(w.error());
    value_ = std::move(next);
    return Result<json>(value_);
  } catch (const std::exception& e) {
    return Result<json>(ErrorCode::Internal, std::string("settings: ") + e.what(), "The settings could not be saved.");
  }
}

}  // namespace vp
