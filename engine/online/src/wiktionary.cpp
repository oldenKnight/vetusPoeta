// The Wiktionary client: settings guard, 1 request/s throttle, Retry-After / back-off, on-disk cache, verdict.
// Runs on the CLI's worker thread (it may sleep); never on the reader thread. No exception leaves this file.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <thread>

#include "json.hpp"
#include "vp/fs.h"
#include "vp/online.h"
#include "vp/project.h"
#include "vp/settings.h"
#include "vp/sha256.h"
#include "vp/text.h"

namespace vp::online {

Transport::~Transport() = default;
Clock::~Clock() = default;

namespace {

class SystemClock : public Clock {
 public:
  int64_t monoMs() override {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
  }
  int64_t unixMs() override { return fs::nowUnixMs(); }
  void sleepMs(int64_t ms) override {
    if (ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(ms));
  }
};

constexpr const char kApiBase[] = "https://en.wiktionary.org/api/rest_v1/page/definition/";
constexpr const char kPageBase[] = "https://en.wiktionary.org/wiki/";
constexpr uint64_t kMaxCacheFile = 8ull * 1024 * 1024;
constexpr int64_t kMaxBackoffMs = 3600LL * 1000;

std::string percentEncode(std::string_view s) {
  static const char kHex[] = "0123456789ABCDEF";
  std::string out;
  for (char ch : s) {
    const unsigned char c = static_cast<unsigned char>(ch);
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.' ||
        c == '_' || c == '~') {
      out.push_back(ch);
    } else if (c == ' ') {
      out.push_back('_');   // MediaWiki titles use '_' for spaces
    } else {
      out.push_back('%');
      out.push_back(kHex[c >> 4]);
      out.push_back(kHex[c & 15]);
    }
  }
  return out;
}

bool boolAt(const nlohmann::json& s, const char* group, const char* key) {
  const auto g = s.find(group);
  if (g == s.end() || !g->is_object()) return false;
  const auto v = g->find(key);
  return v != g->end() && v->is_boolean() && v->get<bool>();
}

// Retry-After in seconds (the delta-seconds form); -1 when absent or an HTTP date.
int retryAfterSec(const Headers& h) {
  const auto it = h.find("retry-after");
  if (it == h.end()) return -1;
  std::string v = it->second;
  while (!v.empty() && (v.back() == ' ' || v.back() == '\r')) v.pop_back();
  while (!v.empty() && v.front() == ' ') v.erase(v.begin());
  if (v.empty() || v.size() > 6) return -1;
  for (char c : v)
    if (c < '0' || c > '9') return -1;
  return std::stoi(v);
}

Evidence makeEvidence(Evidence::Verdict v, std::string summary, std::string url) {
  Evidence e;
  e.verdict = v;
  e.summary = clipSummary(summary);
  e.url = std::move(url);
  return e;
}

int64_t leadingFetchedAt(const std::string& path) {
  fs::FilePtr f = fs::openFile(path, "rb");
  if (!f) return -1;
  char buf[48] = {};
  const size_t n = std::fread(buf, 1, sizeof buf - 1, f.get());
  const std::string head(buf, n);
  const std::string key = "{\"fetchedAt\":";
  if (head.compare(0, key.size(), key) != 0) return -1;
  int64_t v = 0;
  size_t i = key.size();
  if (i >= head.size() || head[i] < '0' || head[i] > '9') return -1;
  for (; i < head.size() && head[i] >= '0' && head[i] <= '9'; ++i) v = v * 10 + (head[i] - '0');
  return v;
}

}  // namespace

Clock& systemClock() {
  static SystemClock c;
  return c;
}

int64_t Throttle::reserve(int64_t nowMs) {
  std::lock_guard<std::mutex> lock(mu_);
  if (!used_ || next_ <= nowMs) {
    used_ = true;
    next_ = nowMs + interval_;
    return 0;
  }
  const int64_t wait = next_ - nowMs;
  next_ += interval_;
  return wait;
}

Throttle& sharedThrottle() {
  static Throttle t(1000);
  return t;
}

bool onlineAllowed(const Settings& settings) {
  try {
    const nlohmann::json s = settings.get();
    return boolAt(s, "engines", "online") && boolAt(s, "online", "wiktionary");
  } catch (...) {
    return false;
  }
}

std::string defaultUserAgent() {
  return std::string("vetus-poeta/") + vp::appVersion() +
         " (offline Latin and Greek tutor; https://github.com/oldenKnight/vetusPoeta)";
}

Wiktionary::Wiktionary(Transport& transport, const Settings& settings, Config config)
    : transport_(transport),
      settings_(settings),
      cfg_(std::move(config)),
      clock_(cfg_.clock ? *cfg_.clock : systemClock()),
      throttle_(cfg_.throttle ? *cfg_.throttle : sharedThrottle()) {
  if (cfg_.cacheDir.empty()) cfg_.cacheDir = fs::join(fs::dataDir(), "online-cache");
  if (cfg_.userAgent.empty()) cfg_.userAgent = defaultUserAgent();
}

Result<std::unique_ptr<Wiktionary>> Wiktionary::create(Transport& transport, const Settings& settings, Config config) {
  if (!onlineAllowed(settings))
    return Result<std::unique_ptr<Wiktionary>>(ErrorCode::OnlineDisabled, "online disabled",
                                               "Turn on the online check in Settings first.");
  try {
    return std::make_unique<Wiktionary>(transport, settings, std::move(config));
  } catch (...) {
    return Result<std::unique_ptr<Wiktionary>>(ErrorCode::Internal, "could not create the online client",
                                               "Restart the program.");
  }
}

std::string Wiktionary::pageTitle(std::string_view lemma) {
  std::string t = text::display_latin(text::nfc(lemma), false);
  while (!t.empty() && (t.back() == ' ' || t.back() == '\t')) t.pop_back();
  size_t b = 0;
  while (b < t.size() && (t[b] == ' ' || t[b] == '\t')) ++b;
  return t.substr(b);
}

std::string Wiktionary::apiUrl(std::string_view title) { return kApiBase + percentEncode(title); }

std::string Wiktionary::pageUrl(std::string_view lang, std::string_view title) {
  return kPageBase + percentEncode(title) + (lang == "grc" ? "#Ancient_Greek" : "#Latin");
}

std::string Wiktionary::cachePath(const std::string& url) const {
  return fs::join(cfg_.cacheDir, sha256Hex(url) + ".json");
}

Stats Wiktionary::stats() const {
  std::lock_guard<std::mutex> lock(mu_);
  return stats_;
}

Result<Response> Wiktionary::fetch(const std::string& url) {
  const int64_t wait = throttle_.reserve(clock_.monoMs());
  if (wait > 0) {
    {
      std::lock_guard<std::mutex> lock(mu_);
      ++stats_.throttled;
    }
    clock_.sleepMs(wait);
  }
  {
    std::lock_guard<std::mutex> lock(mu_);
    ++stats_.requests;
  }
  const Headers req = {{"User-Agent", cfg_.userAgent}, {"Accept", "application/json"}};
  return transport_.get(url, cfg_.timeoutMs, req);
}

bool Wiktionary::cacheRead(const std::string& url, Response& out) {
  const std::string path = cachePath(url);
  if (!fs::isRegularFile(path)) return false;
  Result<std::string> data = fs::readFile(path, kMaxCacheFile);
  if (!data) return false;
  const nlohmann::json j = nlohmann::json::parse(data.value(), nullptr, false);
  const auto bad = [&]() {
    fs::removeQuiet(path);
    return false;
  };
  if (j.is_discarded() || !j.is_object()) return bad();
  const auto at = j.find("fetchedAt"), st = j.find("status"), u = j.find("url"), body = j.find("body");
  if (at == j.end() || !at->is_number_integer() || st == j.end() || !st->is_number_integer() || u == j.end() ||
      !u->is_string() || body == j.end() || !body->is_string())
    return bad();
  if (u->get_ref<const std::string&>() != url) return false;   // hash collision: treat as a miss
  const int64_t age = clock_.unixMs() - at->get<int64_t>();
  if (age < -24LL * 3600 * 1000 || age > cfg_.cacheMaxAgeMs) return false;   // expired (or from the future)
  out.status = st->get<int>();
  out.body = body->get<std::string>();
  out.headers.clear();
  return true;
}

void Wiktionary::cacheWrite(const std::string& url, const Response& r) {
  if (!fs::createDirectories(cfg_.cacheDir)) return;
  nlohmann::ordered_json j;
  j["fetchedAt"] = clock_.unixMs();   // first key: cacheTrim reads it from the first bytes
  j["status"] = r.status;
  j["url"] = url;
  j["body"] = r.body;
  const std::string data = j.dump(-1, ' ', false, nlohmann::ordered_json::error_handler_t::replace);
  if (data.size() > kMaxCacheFile) return;
  if (!fs::writeFileAtomic(cachePath(url), data)) return;
  bool trim = false;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (cacheBytes_ >= 0) cacheBytes_ += static_cast<int64_t>(data.size());   // overwrites over-count: rescan fixes it
    trim = cacheBytes_ < 0 || cacheBytes_ > static_cast<int64_t>(cfg_.cacheMaxBytes);
  }
  if (trim) cacheTrim();
}

// Rescans the folder; when it is over the cap, removes the oldest entries (by fetch time, then name) down to 90 %.
void Wiktionary::cacheTrim() {
  struct Entry {
    int64_t at;
    std::string name;
    uint64_t size;
  };
  std::vector<Entry> entries;
  uint64_t total = 0;
  std::error_code ec;
  for (std::filesystem::directory_iterator it(fs::u8path(cfg_.cacheDir), ec), end; !ec && it != end; it.increment(ec)) {
    const std::string name = fs::toU8(it->path().filename());
    if (name.size() != 69 || name.compare(64, 5, ".json") != 0) continue;   // only our <sha256>.json files
    const std::string path = fs::join(cfg_.cacheDir, name);
    Result<uint64_t> size = fs::fileSize(path);
    if (!size) continue;
    entries.push_back({leadingFetchedAt(path), name, size.value()});
    total += size.value();
  }
  if (total > cfg_.cacheMaxBytes) {
    std::sort(entries.begin(), entries.end(),
              [](const Entry& a, const Entry& b) { return a.at != b.at ? a.at < b.at : a.name < b.name; });
    const uint64_t target = cfg_.cacheMaxBytes / 10 * 9;
    for (const Entry& e : entries) {
      if (total <= target) break;
      fs::removeQuiet(fs::join(cfg_.cacheDir, e.name));
      total -= e.size;
    }
  }
  std::lock_guard<std::mutex> lock(mu_);
  cacheBytes_ = static_cast<int64_t>(total);
}

Result<Evidence> Wiktionary::lookup(std::string_view lang, std::string_view lemma, std::string_view glossEn,
                                    bool readCache) {
  if (!onlineAllowed(settings_)) return makeEvidence(Evidence::Error, "online disabled", std::string());
  if (lang != "la" && lang != "grc")
    return Result<Evidence>(ErrorCode::BadParams, "online check: unknown language '" + std::string(lang) + "'",
                            "The online check knows Latin and Ancient Greek only.");
  const std::string title = pageTitle(lemma);
  if (title.empty() || title.size() > 200)
    return Result<Evidence>(ErrorCode::BadParams, "online check: empty or overlong lemma", "No word to look up.");
  const std::string url = apiUrl(title);
  const std::string page = pageUrl(lang, title);
  auto failWith = [&](const std::string& message) {
    std::lock_guard<std::mutex> lock(mu_);
    ++stats_.errors;
    return makeEvidence(Evidence::Error, message, page);
  };

  Response r;
  bool fromCache = false;
  if (readCache && cacheRead(url, r)) {
    fromCache = true;
    std::lock_guard<std::mutex> lock(mu_);
    ++stats_.cacheHits;
  }
  if (!fromCache) {
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (clock_.monoMs() < backoffUntil_) {
        ++stats_.throttled;
        return makeEvidence(Evidence::Unknown, "rate limited", page);
      }
    }
    Result<Response> res = fetch(url);
    if (!res) return failWith(res.error().message);
    r = std::move(res.value());
    if (r.status == 429 || r.status == 503) {
      const int ra = retryAfterSec(r.headers);
      if (ra >= 0 && ra <= cfg_.maxRetryAfterSec) {
        {
          std::lock_guard<std::mutex> lock(mu_);
          ++stats_.throttled;
        }
        clock_.sleepMs(static_cast<int64_t>(ra) * 1000);
        if (!onlineAllowed(settings_)) return makeEvidence(Evidence::Error, "online disabled", std::string());
        res = fetch(url);
        if (!res) return failWith(res.error().message);
        r = std::move(res.value());
      }
      if (r.status == 429 || r.status == 503) {
        const int ra2 = retryAfterSec(r.headers);
        const int64_t wait = std::min(kMaxBackoffMs, std::max<int64_t>(cfg_.backoffMs, static_cast<int64_t>(ra2) * 1000));
        std::lock_guard<std::mutex> lock(mu_);
        backoffUntil_ = clock_.monoMs() + wait;
        return makeEvidence(Evidence::Unknown, "rate limited", page);
      }
    }
  }
  if (r.status == 404) {
    if (!fromCache) cacheWrite(url, r);
    return makeEvidence(Evidence::Unknown, "no Wiktionary entry", page);
  }
  if (r.status != 200) return failWith("HTTP " + std::to_string(r.status) + " from Wiktionary");
  Result<Section> sec = parseDefinitions(r.body, lang);
  if (!sec) {
    if (fromCache) fs::removeQuiet(cachePath(url));
    return failWith(sec.error().message);
  }
  if (!fromCache) cacheWrite(url, r);
  return verdictFor(sec.value(), glossEn, page);
}

Result<Evidence> Wiktionary::checkLemma(std::string_view lang, std::string_view lemma, std::string_view glossEn) {
  try {
    return lookup(lang, lemma, glossEn, true);
  } catch (const std::exception& e) {
    return Result<Evidence>(ErrorCode::Internal, std::string("online check: ") + e.what(), "Try again.");
  } catch (...) {
    return Result<Evidence>(ErrorCode::Internal, "online check: unknown failure", "Try again.");
  }
}

Result<Evidence> Wiktionary::probe() {
  try {
    return lookup("la", "aqua", "water", false);
  } catch (const std::exception& e) {
    return Result<Evidence>(ErrorCode::Internal, std::string("online test: ") + e.what(), "Try again.");
  } catch (...) {
    return Result<Evidence>(ErrorCode::Internal, "online test: unknown failure", "Try again.");
  }
}

OnlineCheck makeOnlineCheck(Wiktionary& client) {
  return [&client](std::string_view lang, std::string_view lemma, std::string_view glossEn) {
    Result<Evidence> r = client.checkLemma(lang, lemma, glossEn);
    if (r) return r.value();
    Evidence e;
    e.verdict = Evidence::Error;
    e.summary = clipSummary(r.error().message);
    return e;
  };
}

}  // namespace vp::online
