// The .vpoeta project format: zip via miniz, atomic save, lock/recovery, autosave timing, migration.
// DESIGN.md section 8. Nothing in here throws out of a public function.
#include "vp/project.h"

#include <cstring>
#include <ctime>
#include <exception>
#include <utility>

#include "miniz.h"
#include "vp/sha256.h"

#ifndef VP_APP_VERSION
#define VP_APP_VERSION "0.0.0"
#endif

namespace vp {

namespace {

using json = nlohmann::json;
using ojson = nlohmann::ordered_json;

constexpr uint64_t kMaxProjectBytes = 1ull << 30;     // the whole .vpoeta
constexpr uint64_t kMaxManifestBytes = 16ull << 20;
constexpr uint64_t kMaxSourceBytes = 256ull << 20;
constexpr uint64_t kMaxCuesBytes = 1ull << 30;
constexpr uint64_t kMaxSmallBytes = 64ull << 20;      // glossary, corrections, stats

const char kManifest[] = "manifest.json";
const char kSource[] = "source.bin";
const char kCues[] = "cues.jsonl";
const char kGlossary[] = "glossary.json";
const char kCorrections[] = "corrections.json";
const char kStats[] = "stats.json";

const char* const kPairs[] = {"en-la", "es-la", "la-en", "la-es", "en-grc", "es-grc", "grc-en", "grc-es", "la-la"};

bool validPair(const std::string& p) {
  for (const char* k : kPairs)
    if (p == k) return true;
  return false;
}

std::string dumpJson(const ojson& j, int indent) { return j.dump(indent, ' ', false, ojson::error_handler_t::replace); }

// ---- zip plumbing --------------------------------------------------------------------------------------------------
std::string zipErr(mz_zip_archive& z) {
  const char* s = mz_zip_get_error_string(mz_zip_get_last_error(&z));
  return s ? std::string(s) : std::string("unknown zip error");
}

size_t writeToString(void* opaque, mz_uint64 ofs, const void* buf, size_t n) {
  std::string* s = static_cast<std::string*>(opaque);
  try {
    if (ofs + n > s->size()) s->resize(static_cast<size_t>(ofs + n));
    if (n) std::memcpy(&(*s)[static_cast<size_t>(ofs)], buf, n);
    return n;
  } catch (...) {
    return 0;
  }
}

struct ZipWriter {
  mz_zip_archive z;
  bool open = false;
  ZipWriter() { mz_zip_zero_struct(&z); }
  ~ZipWriter() { if (open) mz_zip_writer_end(&z); }
  ZipWriter(const ZipWriter&) = delete;
  ZipWriter& operator=(const ZipWriter&) = delete;
};

struct ZipReader {
  mz_zip_archive z;
  bool open = false;
  ZipReader() { mz_zip_zero_struct(&z); }
  ~ZipReader() { if (open) mz_zip_reader_end(&z); }
  ZipReader(const ZipReader&) = delete;
  ZipReader& operator=(const ZipReader&) = delete;
};

// A time_t that every time zone renders as 2000-01-01 12:00 local time, so the DOS timestamps miniz derives with
// localtime() are the same bytes on every machine.
MZ_TIME_T fixedStamp() {
  std::tm t{};
  t.tm_year = 100;
  t.tm_mon = 0;
  t.tm_mday = 1;
  t.tm_hour = 12;
  t.tm_isdst = -1;
  std::time_t v = std::mktime(&t);
  if (v == static_cast<std::time_t>(-1)) v = static_cast<std::time_t>(946728000);
  return static_cast<MZ_TIME_T>(v);
}

bool addEntry(ZipWriter& zw, const char* name, const std::string& data, std::string& why) {
  MZ_TIME_T stamp = fixedStamp();
  const void* p = data.empty() ? static_cast<const void*>("") : static_cast<const void*>(data.data());
  if (!mz_zip_writer_add_mem_ex_v2(&zw.z, name, p, data.size(), nullptr, 0, MZ_DEFAULT_LEVEL, 0, 0, &stamp, nullptr, 0,
                                   nullptr, 0)) {
    why = std::string(name) + ": " + zipErr(zw.z);
    return false;
  }
  return true;
}

// 1 = read, 0 = absent, -1 = damaged (why set).
int readEntry(ZipReader& zr, const char* name, uint64_t cap, std::string& out, std::string& why) {
  out.clear();
  const int i = mz_zip_reader_locate_file(&zr.z, name, nullptr, 0);
  if (i < 0) return 0;
  mz_zip_archive_file_stat st;
  if (!mz_zip_reader_file_stat(&zr.z, static_cast<mz_uint>(i), &st)) {
    why = std::string(name) + ": " + zipErr(zr.z);
    return -1;
  }
  if (st.m_uncomp_size > cap) {
    why = std::string(name) + " is too large";
    return -1;
  }
  out.assign(static_cast<size_t>(st.m_uncomp_size), '\0');
  if (!mz_zip_reader_extract_to_mem(&zr.z, static_cast<mz_uint>(i), out.empty() ? nullptr : &out[0], out.size(), 0)) {
    why = std::string(name) + ": " + zipErr(zr.z);
    out.clear();
    return -1;
  }
  return 1;
}

// ---- tolerant typed readers: missing/null keys keep the default, wrong types fail ---------------------------------
struct Rd {
  std::string err;
  bool fail(const std::string& where, const char* key, const char* what) {
    if (err.empty()) err = where + ": '" + key + "' " + what;
    return false;
  }
  static const json* get(const json& o, const char* k) {
    auto it = o.find(k);
    if (it == o.end() || it->is_null()) return nullptr;
    return &*it;
  }
  bool str(const json& o, const char* k, const std::string& w, std::string& out) {
    const json* v = get(o, k);
    if (!v) return true;
    if (!v->is_string()) return fail(w, k, "must be a string");
    out = v->get<std::string>();
    return true;
  }
  bool integer(const json& o, const char* k, const std::string& w, int& out) {
    const json* v = get(o, k);
    if (!v) return true;
    if (!v->is_number_integer()) return fail(w, k, "must be an integer");
    const long long x = v->get<long long>();
    if (x < -2147483647LL || x > 2147483647LL) return fail(w, k, "is out of range");
    out = static_cast<int>(x);
    return true;
  }
  bool num(const json& o, const char* k, const std::string& w, double& out) {
    const json* v = get(o, k);
    if (!v) return true;
    if (!v->is_number()) return fail(w, k, "must be a number");
    out = v->get<double>();
    return true;
  }
  bool boolean(const json& o, const char* k, const std::string& w, bool& out) {
    const json* v = get(o, k);
    if (!v) return true;
    if (!v->is_boolean()) return fail(w, k, "must be true or false");
    out = v->get<bool>();
    return true;
  }
  bool strList(const json& o, const char* k, const std::string& w, std::vector<std::string>& out) {
    const json* v = get(o, k);
    if (!v) return true;
    if (!v->is_array()) return fail(w, k, "must be a list");
    out.clear();
    for (const json& e : *v) {
      if (!e.is_string()) return fail(w, k, "must be a list of strings");
      out.push_back(e.get<std::string>());
    }
    return true;
  }
  const json* list(const json& o, const char* k, const std::string& w, bool& ok) {
    const json* v = get(o, k);
    if (v && !v->is_array()) ok = fail(w, k, "must be a list");
    return ok ? v : nullptr;
  }
};

// ---- serialisation -------------------------------------------------------------------------------------------------
std::string manifestText(const Manifest& m) {
  ojson j;
  j["format"] = m.format;
  j["appVersion"] = m.appVersion;
  j["created"] = m.created;
  j["pair"] = m.pair;
  j["kind"] = m.kind;
  j["sourceFileName"] = m.sourceFileName;
  j["sourceSha256"] = m.sourceSha256;
  j["settings"] = ojson::parse(m.settingsSnapshot.is_object() ? m.settingsSnapshot.dump() : std::string("{}"));
  return dumpJson(j, 2) + "\n";
}

void appendCueLine(const CueRecord& c, std::string& out) {
  ojson j;
  j["index"] = c.index;
  j["state"] = c.state;
  j["target"] = c.target;
  j["chosen"] = c.chosen;
  ojson alts = ojson::array();
  for (const Alternative& a : c.alternatives) alts.push_back(ojson{{"text", a.text}, {"reason", a.reason}, {"score", a.score}});
  j["alternatives"] = std::move(alts);
  j["confidence"] = c.confidence;
  j["score"] = c.score;
  ojson checks = ojson::array();
  for (const CueCheck& k : c.checks) checks.push_back(ojson{{"id", k.id}, {"ok", k.ok}, {"detail", k.detail}});
  j["checks"] = std::move(checks);
  ojson reasons = ojson::array();
  for (const CueReason& r : c.reasons) {
    ojson o{{"tokenIndex", r.tokenIndex}, {"kind", r.kind}, {"text", r.text}};
    if (!r.data.empty()) {
      ojson d = ojson::parse(r.data, nullptr, false);
      o["data"] = d.is_discarded() ? ojson(r.data) : std::move(d);
    } else {
      o["data"] = nullptr;
    }
    reasons.push_back(std::move(o));
  }
  j["reasons"] = std::move(reasons);
  j["edited"] = c.edited;
  j["reviewed"] = c.reviewed;
  out += dumpJson(j, -1);
  out += '\n';
}

std::string glossaryText(const std::vector<NameEntry>& g) {
  ojson a = ojson::array();
  for (const NameEntry& n : g)
    a.push_back(ojson{{"name", n.name}, {"policy", n.policy}, {"form", n.form}, {"forms", n.forms},
                      {"gender", n.gender}, {"declension", n.declension}});
  return dumpJson(a, 2) + "\n";
}

std::string correctionsText(const std::vector<Correction>& cs) {
  ojson a = ojson::array();
  for (const Correction& c : cs)
    a.push_back(ojson{{"id", c.id}, {"key", c.key}, {"target", c.target}, {"scope", c.scope}, {"count", c.count}});
  return dumpJson(a, 2) + "\n";
}

// ---- parsing -------------------------------------------------------------------------------------------------------
bool parseManifest(const std::string& text, Manifest& m, std::string& err) {
  json j = json::parse(text, nullptr, false);
  if (j.is_discarded() || !j.is_object()) {
    err = "manifest.json is not a JSON object";
    return false;
  }
  Rd r;
  const std::string w = "manifest.json";
  if (!j.contains("format")) {
    err = "manifest.json has no format";
    return false;
  }
  m.format = -1;
  if (!r.integer(j, "format", w, m.format) || !r.str(j, "appVersion", w, m.appVersion) ||
      !r.str(j, "created", w, m.created) || !r.str(j, "pair", w, m.pair) || !r.str(j, "kind", w, m.kind) ||
      !r.str(j, "sourceFileName", w, m.sourceFileName) || !r.str(j, "sourceSha256", w, m.sourceSha256)) {
    err = r.err;
    return false;
  }
  if (m.format < 0) {
    err = "manifest.json has an invalid format";
    return false;
  }
  if (const json* s = Rd::get(j, "settings")) {
    if (!s->is_object()) {
      err = "manifest.json: 'settings' must be an object";
      return false;
    }
    m.settingsSnapshot = *s;
  } else {
    m.settingsSnapshot = json::object();
  }
  return true;
}

bool parseCue(const json& j, CueRecord& c, size_t line, std::string& err) {
  const std::string w = "cues.jsonl line " + std::to_string(line);
  if (!j.is_object()) {
    err = w + " is not a JSON object";
    return false;
  }
  Rd r;
  bool ok = r.integer(j, "index", w, c.index) && r.str(j, "state", w, c.state) && r.str(j, "target", w, c.target) &&
            r.integer(j, "chosen", w, c.chosen) && r.str(j, "confidence", w, c.confidence) &&
            r.num(j, "score", w, c.score) && r.boolean(j, "edited", w, c.edited) &&
            r.boolean(j, "reviewed", w, c.reviewed);
  if (ok) {
    if (const json* a = r.list(j, "alternatives", w, ok)) {
      for (const json& e : *a) {
        if (!e.is_object()) { ok = r.fail(w, "alternatives", "must hold objects"); break; }
        Alternative alt;
        if (!(ok = r.str(e, "text", w, alt.text) && r.str(e, "reason", w, alt.reason) && r.num(e, "score", w, alt.score)))
          break;
        c.alternatives.push_back(std::move(alt));
      }
    }
  }
  if (ok) {
    if (const json* a = r.list(j, "checks", w, ok)) {
      for (const json& e : *a) {
        if (!e.is_object()) { ok = r.fail(w, "checks", "must hold objects"); break; }
        CueCheck k;
        if (!(ok = r.str(e, "id", w, k.id) && r.boolean(e, "ok", w, k.ok) && r.str(e, "detail", w, k.detail))) break;
        c.checks.push_back(std::move(k));
      }
    }
  }
  if (ok) {
    if (const json* a = r.list(j, "reasons", w, ok)) {
      for (const json& e : *a) {
        if (!e.is_object()) { ok = r.fail(w, "reasons", "must hold objects"); break; }
        CueReason rs;
        if (!(ok = r.integer(e, "tokenIndex", w, rs.tokenIndex) && r.str(e, "kind", w, rs.kind) &&
                   r.str(e, "text", w, rs.text)))
          break;
        if (const json* d = Rd::get(e, "data")) rs.data = d->dump(-1, ' ', false, json::error_handler_t::replace);
        c.reasons.push_back(std::move(rs));
      }
    }
  }
  if (!ok) err = r.err;
  return ok;
}

bool parseCues(const std::string& text, std::vector<CueRecord>& cues, std::string& err) {
  cues.clear();
  size_t pos = 0, line = 0;
  while (pos < text.size()) {
    size_t nl = text.find('\n', pos);
    if (nl == std::string::npos) nl = text.size();
    ++line;
    if (nl > pos) {
      json j = json::parse(text.begin() + static_cast<std::ptrdiff_t>(pos), text.begin() + static_cast<std::ptrdiff_t>(nl),
                           nullptr, false);
      if (j.is_discarded()) {
        err = "cues.jsonl line " + std::to_string(line) + " is not valid JSON";
        return false;
      }
      CueRecord c;
      if (!parseCue(j, c, line, err)) return false;
      cues.push_back(std::move(c));
    }
    pos = nl + 1;
  }
  return true;
}

bool parseGlossary(const std::string& text, std::vector<NameEntry>& out, std::string& err) {
  json j = json::parse(text, nullptr, false);
  if (j.is_discarded() || !j.is_array()) {
    err = "glossary.json is not a JSON list";
    return false;
  }
  Rd r;
  const std::string w = "glossary.json";
  for (const json& e : j) {
    if (!e.is_object()) { err = "glossary.json must hold objects"; return false; }
    NameEntry n;
    if (!r.str(e, "name", w, n.name) || !r.str(e, "policy", w, n.policy) || !r.str(e, "form", w, n.form) ||
        !r.strList(e, "forms", w, n.forms) || !r.str(e, "gender", w, n.gender) ||
        !r.str(e, "declension", w, n.declension)) {
      err = r.err;
      return false;
    }
    out.push_back(std::move(n));
  }
  return true;
}

bool parseCorrections(const std::string& text, std::vector<Correction>& out, std::string& err) {
  json j = json::parse(text, nullptr, false);
  if (j.is_discarded() || !j.is_array()) {
    err = "corrections.json is not a JSON list";
    return false;
  }
  Rd r;
  const std::string w = "corrections.json";
  for (const json& e : j) {
    if (!e.is_object()) { err = "corrections.json must hold objects"; return false; }
    Correction c;
    if (!r.str(e, "id", w, c.id) || !r.str(e, "key", w, c.key) || !r.str(e, "target", w, c.target) ||
        !r.str(e, "scope", w, c.scope) || !r.integer(e, "count", w, c.count)) {
      err = r.err;
      return false;
    }
    out.push_back(std::move(c));
  }
  return true;
}

const char kCorruptHint[] =
    "The project file is damaged or incomplete. Open its autosave or a backup copy; the original subtitle file is "
    "not affected.";

Result<void> corrupt(const std::string& name, const std::string& why) {
  return Result<void>(ErrorCode::ProjectCorrupt, "the project file '" + name + "' is damaged or incomplete (" + why + ")",
                      kCorruptHint);
}

Result<void> loadZip(const std::string& zip, const std::string& name, Project& p, std::vector<std::string>& warnings) {
  ZipReader zr;
  if (!mz_zip_reader_init_mem(&zr.z, zip.data(), zip.size(), 0)) return corrupt(name, zipErr(zr.z));
  zr.open = true;
  std::string buf, why;

  int st = readEntry(zr, kManifest, kMaxManifestBytes, buf, why);
  if (st == 0) return corrupt(name, "manifest.json is missing");
  if (st < 0) return corrupt(name, why);
  if (!parseManifest(buf, p.manifest, why)) return corrupt(name, why);
  if (p.manifest.format > Manifest::kFormat)
    return Result<void>(ErrorCode::UnsupportedFormat,
                        "'" + name + "' has project format " + std::to_string(p.manifest.format) +
                            ", this program reads up to " + std::to_string(Manifest::kFormat),
                        "The project was saved by a newer version of vetus poeta. Update the program to open it.");

  st = readEntry(zr, kSource, kMaxSourceBytes, p.source, why);
  if (st == 0) return corrupt(name, "source.bin is missing");
  if (st < 0) return corrupt(name, why);
  if (!p.manifest.sourceSha256.empty() && sha256Hex(p.source) != p.manifest.sourceSha256)
    return corrupt(name, "source.bin does not match its checksum");

  st = readEntry(zr, kCues, kMaxCuesBytes, buf, why);
  if (st == 0) return corrupt(name, "cues.jsonl is missing");
  if (st < 0) return corrupt(name, why);
  if (!parseCues(buf, p.cues, why)) return corrupt(name, why);
  std::string().swap(buf);

  st = readEntry(zr, kGlossary, kMaxSmallBytes, buf, why);
  if (st < 0) return corrupt(name, why);
  if (st > 0 && !parseGlossary(buf, p.glossary, why)) return corrupt(name, why);

  st = readEntry(zr, kCorrections, kMaxSmallBytes, buf, why);
  if (st < 0) return corrupt(name, why);
  if (st > 0 && !parseCorrections(buf, p.corrections, why)) return corrupt(name, why);

  st = readEntry(zr, kStats, kMaxSmallBytes, buf, why);
  if (st < 0) return corrupt(name, why);
  if (st > 0) {
    json s = json::parse(buf, nullptr, false);
    if (s.is_discarded() || !s.is_object()) return corrupt(name, "stats.json is not a JSON object");
    p.stats = std::move(s);
  }

  if (p.manifest.format < Manifest::kFormat) {
    const int from = p.manifest.format;
    const int steps = p.migrate(from);
    warnings.push_back("project upgraded from format " + std::to_string(from) + " to " +
                       std::to_string(Manifest::kFormat) + " (" + std::to_string(steps) + " step" +
                       (steps == 1 ? "" : "s") + "); save to keep the new format");
  }
  return Result<void>();
}

Result<Project> loadFile(const std::string& path, std::vector<std::string>& warnings) {
  if (!fs::isRegularFile(path))
    return Result<Project>(ErrorCode::NotFound, "project file not found: '" + path + "'",
                           "The project was moved, renamed or deleted.");
  Result<std::string> bytes = fs::readFile(path, kMaxProjectBytes);
  if (!bytes) return Result<Project>(bytes.error());
  Project p;
  Result<void> r = loadZip(bytes.value(), path, p, warnings);
  if (!r) return Result<Project>(r.error());
  return Result<Project>(std::move(p));
}

}  // namespace

// ---- public API ----------------------------------------------------------------------------------------------------
const char* appVersion() { return VP_APP_VERSION; }

const char* projectKindName(ProjectKind k) { return k == ProjectKind::Text ? "text" : "subs"; }

Result<Project> Project::create(ProjectKind kind, const std::string& pair, std::string sourceBytes,
                                const std::string& sourceName, const nlohmann::json& settingsSnapshot) {
  try {
    if (!validPair(pair))
      return Result<Project>(ErrorCode::BadParams, "unknown language pair '" + pair + "'", "Choose a language pair.");
    if (sourceBytes.size() > kMaxSourceBytes)
      return Result<Project>(ErrorCode::BadParams, "the source is too large",
                             "The file is larger than this program accepts.");
    Project p;
    p.manifest.format = Manifest::kFormat;
    p.manifest.appVersion = appVersion();
    p.manifest.created = fs::isoUtc(fs::nowUnixMs());
    p.manifest.pair = pair;
    p.manifest.kind = projectKindName(kind);
    p.manifest.sourceFileName = sourceName;
    p.manifest.sourceSha256 = sha256Hex(sourceBytes);
    p.manifest.settingsSnapshot = settingsSnapshot.is_object() ? settingsSnapshot : nlohmann::json::object();
    p.source = std::move(sourceBytes);
    return Result<Project>(std::move(p));
  } catch (const std::exception& e) {
    return Result<Project>(ErrorCode::Internal, std::string("cannot create project: ") + e.what(),
                           "The project could not be created.");
  }
}

Result<std::string> Project::toZip() const {
  try {
    std::string out;
    std::string why;
    bool ok = true;
    {
      ZipWriter zw;
      zw.z.m_pWrite = writeToString;
      zw.z.m_pIO_opaque = &out;
      if (!mz_zip_writer_init(&zw.z, 0)) {
        return Result<std::string>(ErrorCode::Internal, "zip: " + zipErr(zw.z), "The project could not be saved.");
      }
      zw.open = true;
      Manifest m = manifest;
      m.format = Manifest::kFormat;
      m.appVersion = appVersion();
      m.sourceSha256 = sha256Hex(source);
      std::string cuesText;
      for (const CueRecord& c : cues) appendCueLine(c, cuesText);
      ok = addEntry(zw, kManifest, manifestText(m), why) && addEntry(zw, kSource, source, why) &&
           addEntry(zw, kCues, cuesText, why) && addEntry(zw, kGlossary, glossaryText(glossary), why) &&
           addEntry(zw, kCorrections, correctionsText(corrections), why) &&
           addEntry(zw, kStats, dumpJson(ojson::parse(stats.is_object() ? stats.dump() : std::string("{}")), 2) + "\n", why);
      if (ok && !mz_zip_writer_finalize_archive(&zw.z)) {
        why = zipErr(zw.z);
        ok = false;
      }
      zw.open = false;
      if (!mz_zip_writer_end(&zw.z)) ok = false;
    }
    if (!ok)
      return Result<std::string>(ErrorCode::Internal, "zip: " + why, "The project could not be saved.");
    return Result<std::string>(std::move(out));
  } catch (const std::exception& e) {
    return Result<std::string>(ErrorCode::Internal, std::string("cannot build the project file: ") + e.what(),
                               "The project could not be saved.");
  }
}

Result<void> Project::save(const std::string& path) const {
  if (path.empty()) return Result<void>(ErrorCode::BadParams, "no project path given", "Choose where to save.");
  Result<std::string> zip = toZip();
  if (!zip) return Result<void>(zip.error());
  return fs::writeFileAtomic(path, zip.value());
}

Result<Project> Project::fromZip(const std::string& zipBytes, const std::string& nameForErrors) {
  try {
    Project p;
    std::vector<std::string> warnings;
    Result<void> r = loadZip(zipBytes, nameForErrors, p, warnings);
    if (!r) return Result<Project>(r.error());
    return Result<Project>(std::move(p));
  } catch (const std::exception& e) {
    return Result<Project>(ErrorCode::ProjectCorrupt, std::string("cannot read '") + nameForErrors + "': " + e.what(),
                           kCorruptHint);
  }
}

Result<Opened> Project::open(const std::string& path) {
  try {
    Opened o;
    Result<Project> p = loadFile(path, o.warnings);
    if (!p) {
      Error e = p.error();
      if (e.code == ErrorCode::ProjectCorrupt) {
        const std::string as = newestReadableAutosave(path);
        if (!as.empty()) e.hint = "The project file is damaged. A readable autosave exists: '" + as + "'. Recover it.";
      }
      return Result<Opened>(std::move(e));
    }
    o.project = std::move(p.value());
    o.recoverable = checkRecovery(path);
    if (o.recoverable.lockedByLiveProcess)
      o.warnings.push_back("the project seems to be open in another window (process " +
                           std::to_string(o.recoverable.lockPid) + ")");
    return Result<Opened>(std::move(o));
  } catch (const std::exception& e) {
    return Result<Opened>(ErrorCode::Internal, std::string("cannot open project: ") + e.what(),
                          "The project could not be opened.");
  }
}

Result<Project> Project::recover(const std::string& path) {
  try {
    std::vector<std::string> warnings;
    const std::string as = autosavePath(path);
    if (!fs::isRegularFile(as))
      return Result<Project>(ErrorCode::NotFound, "no autosave for '" + path + "'", "There is nothing to recover.");
    return loadFile(as, warnings);
  } catch (const std::exception& e) {
    return Result<Project>(ErrorCode::Internal, std::string("cannot recover project: ") + e.what(),
                           "The autosave could not be read.");
  }
}

int Project::migrate(int formatFrom) {
  int steps = 0;
  for (int f = formatFrom; f < Manifest::kFormat; ++f) {
    switch (f) {
      case 0:
        // Format 0 (pre-release drafts) had the same entries; nothing to convert yet. Future steps go here,
        // one case per version, each turning format f into f + 1.
        break;
      default:
        break;
    }
    ++steps;
  }
  manifest.format = Manifest::kFormat;
  return steps;
}

// ---- lock / recovery -----------------------------------------------------------------------------------------------
std::string autosavePath(const std::string& projectPath) { return projectPath + ".autosave"; }
std::string lockPath(const std::string& projectPath) { return projectPath + ".lock"; }

Result<void> writeLockFor(const std::string& projectPath, uint64_t pid, int64_t startMs) {
  try {
    json j = {{"pid", pid}, {"start", startMs}, {"time", fs::isoUtc(fs::nowUnixMs())}};
    return fs::writeFileAtomic(lockPath(projectPath), j.dump() + "\n");
  } catch (...) {
    return Result<void>(ErrorCode::Internal, "cannot write the lock file", "The project folder may be read-only.");
  }
}

Result<void> writeLock(const std::string& projectPath) {
  const uint64_t pid = fs::currentPid();
  int64_t start = fs::processStartMs(pid);
  if (start <= 0) start = 0;
  return writeLockFor(projectPath, pid, start);
}

void removeLock(const std::string& projectPath) { fs::removeQuiet(lockPath(projectPath)); }

Recovery checkRecovery(const std::string& projectPath) {
  Recovery info;
  try {
    const std::string lock = lockPath(projectPath);
    bool deadLock = false;
    if (fs::isRegularFile(lock)) {
      uint64_t pid = 0;
      int64_t start = 0;
      Result<std::string> text = fs::readFile(lock, 4096);
      if (text) {
        json j = json::parse(text.value(), nullptr, false);
        if (j.is_object()) {
          auto ip = j.find("pid");
          if (ip != j.end() && ip->is_number_unsigned()) pid = ip->get<uint64_t>();
          auto is = j.find("start");
          if (is != j.end() && is->is_number_integer()) start = is->get<int64_t>();
        }
      }
      info.lockPid = pid;
      if (pid != 0 && pid != fs::currentPid() && fs::isPidAlive(pid, start)) {
        info.lockedByLiveProcess = true;
        return info;  // the other process owns the autosave
      }
      deadLock = pid != fs::currentPid();  // an unreadable lock counts as left behind by a dead process
    }
    const std::string as = autosavePath(projectPath);
    if (!fs::isRegularFile(as)) return info;
    Result<int64_t> ta = fs::mtime(as);
    if (!ta) return info;
    bool newer = true;
    if (fs::isRegularFile(projectPath)) {
      Result<int64_t> tp = fs::mtime(projectPath);
      newer = tp && ta.value() > tp.value();
    }
    if (deadLock || newer) {
      info.available = true;
      info.autosavePath = as;
      info.atMs = ta.value();
    }
  } catch (...) {
    info = Recovery();
  }
  return info;
}

std::string newestReadableAutosave(const std::string& projectPath) {
  try {
    const std::string as = autosavePath(projectPath);
    if (!fs::isRegularFile(as)) return std::string();
    Result<std::string> bytes = fs::readFile(as, kMaxProjectBytes);
    if (!bytes) return std::string();
    Result<Project> p = Project::fromZip(bytes.value(), as);
    return p ? as : std::string();
  } catch (...) {
    return std::string();
  }
}

// ---- autosave timing -----------------------------------------------------------------------------------------------
void Autosaver::noteChange(int64_t nowMs) {
  if (!dirty_) {
    dirty_ = true;
    dirtySinceMs_ = nowMs;
  }
  lastChangeMs_ = nowMs;
}

bool Autosaver::due(int64_t nowMs) const {
  if (!dirty_) return false;
  return nowMs - lastChangeMs_ >= debounceMs_ || nowMs - dirtySinceMs_ >= maxIntervalMs_;
}

void Autosaver::saved(int64_t nowMs) {
  (void)nowMs;
  dirty_ = false;
}

void Autosaver::saveFailed(int64_t nowMs) {
  dirty_ = true;
  dirtySinceMs_ = nowMs;
  lastChangeMs_ = nowMs;
}

}  // namespace vp
