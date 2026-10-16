// vp::online — engine iii, the optional online check against en.wiktionary.org. [CONTRACT, DESIGN §1.3, §12]
// This is the only module of the engine that talks to the network. Nothing here runs unless the settings say so
// at call time (engines.online && online.wiktionary, both off by default, DECISIONS D3): the client re-reads the
// settings on every call and answers "online disabled" without touching the transport otherwise.
// Threading: checkLemma()/probe() may sleep (1 request/s throttle, Retry-After). Call them from the CLI's worker
// thread, never from the reader thread that answers engine.ping. Nothing here throws across the boundary.
#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "vp/result.h"

namespace vp { class Settings; }

namespace vp::online {

using Headers = std::map<std::string, std::string>;   // response header names are lower case

struct Response {
  int status = 0;
  std::string body;
  Headers headers;
};

// One HTTPS GET. A non-2xx status is a successful Result (the caller reads `status`); DNS/TLS/timeout/spawn
// failures are online_failed errors with a message.
class Transport {
 public:
  virtual ~Transport();
  virtual Result<Response> get(const std::string& url, int timeoutMs, const Headers& requestHeaders) = 0;
};

// engines.online && online.wiktionary, read from the settings now.
bool onlineAllowed(const Settings& settings);

// The real transport: WinHTTP on Windows (system DLL). On other systems a DEV-ONLY transport that runs the `curl`
// program (never shipped: the Linux engine binary exists for tests only); it fails with "dev transport: ..." when
// curl is absent. Returns online_disabled, without creating anything, unless onlineAllowed(settings).
Result<std::unique_ptr<Transport>> makeSystemTransport(const Settings& settings);

// Time source, injectable so tests never sleep.
class Clock {
 public:
  virtual ~Clock();
  virtual int64_t monoMs() = 0;          // steady clock
  virtual int64_t unixMs() = 0;          // wall clock (cache ages)
  virtual void sleepMs(int64_t ms) = 0;
};
Clock& systemClock();

// Token bucket with capacity 1 and one token per `intervalMs`. reserve() takes the next slot and returns how long
// the caller has to wait before using it (0 when a token was available). Thread-safe; it never sleeps itself.
class Throttle {
 public:
  explicit Throttle(int64_t intervalMs = 1000) : interval_(intervalMs) {}
  int64_t reserve(int64_t nowMs);
 private:
  std::mutex mu_;
  int64_t interval_;
  int64_t next_ = 0;
  bool used_ = false;
};
Throttle& sharedThrottle();   // the process-wide 1 request/s bucket every client uses by default

struct Evidence {
  enum Verdict { Agrees, Disagrees, Unknown, Error };
  Verdict verdict = Unknown;
  std::string summary;   // human, English, at most 160 code points
  std::string url;       // human page, e.g. https://en.wiktionary.org/wiki/aqua#Latin
};
const char* verdictName(Evidence::Verdict v);   // "agrees", "disagrees", "unknown", "error"

struct Stats {
  uint64_t requests = 0;    // transport calls made
  uint64_t cacheHits = 0;   // answers served from the cache (no request, no throttle)
  uint64_t throttled = 0;   // waits for the 1/s bucket or a Retry-After, and calls refused during a 429 back-off
  uint64_t errors = 0;      // network errors, unexpected HTTP status, malformed JSON
};

struct Config {
  std::string cacheDir;                       // "" = fs::dataDir()/online-cache
  int timeoutMs = 8000;
  int64_t cacheMaxAgeMs = 30LL * 24 * 3600 * 1000;
  uint64_t cacheMaxBytes = 20ull * 1024 * 1024;   // oldest files are removed down to 90 % of this
  int maxRetryAfterSec = 60;                  // a 429/503 with Retry-After <= this is retried once after waiting
  int64_t backoffMs = 60000;                  // after an unresolved 429/503 no request is sent for this long
  Clock* clock = nullptr;                     // null = systemClock()
  Throttle* throttle = nullptr;               // null = sharedThrottle()
  std::string userAgent;                      // "" = defaultUserAgent()
};

std::string defaultUserAgent();   // vetus-poeta/<version> (offline Latin and Greek tutor; https://github.com/...)

class Wiktionary {
 public:
  // Prefer create(): the constructor itself cannot refuse, but every call re-checks the settings anyway.
  Wiktionary(Transport& transport, const Settings& settings, Config config = {});
  // online_disabled unless onlineAllowed(settings).
  static Result<std::unique_ptr<Wiktionary>> create(Transport& transport, const Settings& settings, Config config = {});

  // lang "la" | "grc"; lemma = dictionary form (macrons allowed, stripped for the title); glossEn = the lexicon's
  // English gloss. bad_params for an unknown lang or an empty lemma; every other outcome is an Evidence.
  Result<Evidence> checkLemma(std::string_view lang, std::string_view lemma, std::string_view glossEn);
  // online.test: fetches "aqua" (gloss "water") without reading the cache (the answer is still cached).
  Result<Evidence> probe();
  Stats stats() const;

  static std::string pageTitle(std::string_view lemma);                     // macrons/breves stripped, NFC
  static std::string apiUrl(std::string_view title);                        // REST definition URL
  static std::string pageUrl(std::string_view lang, std::string_view title);  // human page with #Latin / #Ancient_Greek
  std::string cachePath(const std::string& url) const;                      // <cacheDir>/<sha256(url)>.json

 private:
  Result<Evidence> lookup(std::string_view lang, std::string_view lemma, std::string_view glossEn, bool readCache);
  Result<Response> fetch(const std::string& url);
  bool cacheRead(const std::string& url, Response& out);
  void cacheWrite(const std::string& url, const Response& r);
  void cacheTrim();

  Transport& transport_;
  const Settings& settings_;
  Config cfg_;
  Clock& clock_;
  Throttle& throttle_;
  mutable std::mutex mu_;
  Stats stats_;
  int64_t backoffUntil_ = 0;        // monoMs
  int64_t cacheBytes_ = -1;         // running estimate of the cache folder size (-1 = not scanned yet)
};

// ---- pure helpers (tested directly) ----------------------------------------------------------------------------
struct Section {
  bool found = false;                     // the language has a section on the page
  std::vector<std::string> definitions;   // plain text, HTML removed, in page order
};
// Parses a REST definition response; langCode "la" | "grc". Reads the section keyed by the code and the entries
// under "other" whose "language" is the English name ("Latin", "Ancient Greek": Greek lives there). online_failed
// on malformed JSON.
Result<Section> parseDefinitions(std::string_view json, std::string_view langCode);
std::string stripHtml(std::string_view html);                 // tags removed, entities decoded, spaces collapsed
std::vector<std::string> glossKeywords(std::string_view glossEn);   // lower-case content words, light plural stem
// Agrees: a gloss keyword occurs in the definitions. Disagrees: section present, no overlap. Unknown otherwise.
Evidence verdictFor(const Section& section, std::string_view glossEn, const std::string& pageUrl);
std::string clipSummary(std::string_view s, size_t maxCodePoints = 160);

// The hook the rules engine receives (Advisors.onlineCheck, engine/rules engine_config.h when it lands): evidence
// only, it never changes text; a Disagrees lowers the cue to Check and adds a ReasonView of kind "evidence".
using OnlineCheck = std::function<Evidence(std::string_view lang, std::string_view lemma, std::string_view glossEn)>;
OnlineCheck makeOnlineCheck(Wiktionary& client);   // errors become Evidence::Error; the client must outlive it

}  // namespace vp::online
