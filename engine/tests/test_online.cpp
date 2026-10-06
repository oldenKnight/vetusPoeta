// engine/online: REST definition parsing on hand-made fixtures, verdicts, titles, the settings guard (zero
// transport calls when off), throttle with an injected clock, 429/503 + Retry-After, errors and timeouts, the
// on-disk cache (read, expiry, size cap), stats and the rules-engine hook. No test touches the network except the
// optional live test, which runs only with VP_ONLINE_LIVE=1.
#include <doctest.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#include "vp/fs.h"
#include "vp/online.h"
#include "vp/online_mock.h"
#include "vp/settings.h"
#include "vp/sha256.h"

namespace stdfs = std::filesystem;
using namespace vp::online;

namespace {

std::string fixture(const std::string& name) {
  vp::Result<std::string> r = vp::fs::readFile(std::string(VP_FIXTURES_DIR) + "/online/" + name);
  REQUIRE(r.ok());
  return r.value();
}

std::string freshDir(const std::string& name) {
  const stdfs::path d = stdfs::path(VP_TEST_TMP) / "online" / name;
  std::error_code ec;
  stdfs::remove_all(d, ec);
  stdfs::create_directories(d, ec);
  return d.string();
}

// A settings store in its own folder; `on` turns engines.online and online.wiktionary on.
struct TestSettings {
  vp::Settings s;
  explicit TestSettings(const std::string& name, bool on) : s(freshDir("settings_" + name) + "/settings.json") {
    s.load();
    if (on) set(true, true);
  }
  void set(bool engines, bool wiktionary) {
    REQUIRE(s.set(nlohmann::json{{"engines", {{"online", engines}}}, {"online", {{"wiktionary", wiktionary}}}}).ok());
  }
};

MockTransport::Step ok200(const std::string& body, int delayMs = 0) {
  MockTransport::Step st;
  st.status = 200;
  st.body = body;
  st.delayMs = delayMs;
  return st;
}

MockTransport::Step status(int code, const std::string& retryAfter = std::string()) {
  MockTransport::Step st;
  st.status = code;
  st.body = "too many requests";
  if (!retryAfter.empty()) st.headers["retry-after"] = retryAfter;
  return st;
}

// A client wired to a fake clock, its own throttle and its own cache folder.
struct Rig {
  FakeClock clock;
  Throttle throttle{1000};
  MockTransport mock{&clock};
  TestSettings settings;
  Config cfg;
  std::unique_ptr<Wiktionary> client;
  explicit Rig(const std::string& name, bool on = true) : settings(name, on) {
    cfg.cacheDir = freshDir("cache_" + name);
    cfg.clock = &clock;
    cfg.throttle = &throttle;
    client = std::make_unique<Wiktionary>(mock, settings.s, cfg);
  }
  Evidence check(const std::string& lang, const std::string& lemma, const std::string& gloss) {
    vp::Result<Evidence> r = client->checkLemma(lang, lemma, gloss);
    REQUIRE(r.ok());
    return r.value();
  }
};

uint64_t dirBytes(const std::string& dir) {
  uint64_t t = 0;
  std::error_code ec;
  for (stdfs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) t += stdfs::file_size(it->path(), ec);
  return t;
}

}  // namespace

TEST_CASE("online: parse the six fixture responses") {
  SUBCASE("Latin entry with several senses") {
    vp::Result<Section> s = parseDefinitions(fixture("la_aqua.json"), "la");
    REQUIRE(s.ok());
    CHECK(s->found);
    REQUIRE(s->definitions.size() == 4);   // the empty definition is dropped
    CHECK(s->definitions[0] == "water");
    CHECK(s->definitions[1] == "(in the plural) a spring or bath with healing waters");
    CHECK(s->definitions[2] == "a channel that carries water; conduit");
    CHECK(s->definitions[3] == "plural of aquae");
    // other languages on the page are ignored
    vp::Result<Section> en = parseDefinitions(fixture("la_aqua.json"), "en");
    REQUIRE(en.ok());
    CHECK(en->definitions == std::vector<std::string>{"a light blue-green colour"});
  }
  SUBCASE("Greek entry") {
    vp::Result<Section> s = parseDefinitions(fixture("grc_logos.json"), "grc");
    REQUIRE(s.ok());
    CHECK(s->found);
    REQUIRE(s->definitions.size() == 3);
    CHECK(s->definitions[0] == "word, something said");
    CHECK(s->definitions[2] == "reason, account, explanation");   // the other "other" language is not taken
    CHECK_FALSE(parseDefinitions(fixture("grc_logos.json"), "la")->found);
    // Ancient Greek lives under "other" (live shape); a "grc" key, should the endpoint add one, is read first
    vp::Result<Section> keyed = parseDefinitions(
        R"({"grc":[{"definitions":[{"definition":"keyed"}]}],"other":[{"language":"Ancient Greek","definitions":[{"definition":"named"}]}]})",
        "grc");
    CHECK(keyed->definitions == std::vector<std::string>{"keyed", "named"});
    CHECK_FALSE(parseDefinitions(R"({"other":[{"language":"Latin"}]})", "la")->found);
    CHECK(parseDefinitions(R"({"other":[{"language":"Latin","definitions":[{"definition":"x"}]}]})", "la")->found);
  }
  SUBCASE("page without the language") {
    vp::Result<Section> s = parseDefinitions(fixture("no_latin.json"), "la");
    REQUIRE(s.ok());
    CHECK_FALSE(s->found);
    CHECK(s->definitions.empty());
  }
  SUBCASE("404 body") {
    vp::Result<Section> s = parseDefinitions(fixture("not_found.json"), "la");
    REQUIRE(s.ok());
    CHECK_FALSE(s->found);
  }
  SUBCASE("malformed JSON") {
    vp::Result<Section> s = parseDefinitions(fixture("malformed.json"), "la");
    CHECK_FALSE(s.ok());
    CHECK(s.error().code == vp::ErrorCode::OnlineFailed);
    CHECK_FALSE(parseDefinitions("", "la").ok());
    CHECK_FALSE(parseDefinitions("[1,2]", "la").ok());
    CHECK_FALSE(parseDefinitions("\xff\xfe{", "la").ok());
    CHECK_FALSE(parseDefinitions(R"({"la":5})", "la")->found);
    CHECK_FALSE(parseDefinitions(R"({"la":[{"definitions":7}]})", "la")->found);
    CHECK(parseDefinitions(R"({"la":[{"definitions":[{"definition":3},"x",{"definition":"ok"}]}]})", "la")->definitions ==
          std::vector<std::string>{"ok"});
  }
  SUBCASE("HTML in definitions") {
    vp::Result<Section> s = parseDefinitions(fixture("html_defs.json"), "la");
    REQUIRE(s.ok());
    REQUIRE(s->definitions.size() == 2);   // the whitespace-only one is dropped
    CHECK(s->definitions[0] == "(transitive) to love, be fond of & cherish");
    CHECK(s->definitions[1] == "to like \xE2\x80\x9Cvery much\xE2\x80\x9D \xE2\x80\x94 with infinitive: to be accustomed, tend");
  }
}

TEST_CASE("online: stripHtml, keywords, clipSummary") {
  CHECK(stripHtml("a&lt;b&gt; &amp;&#65;&#x42; &bogus; & x") == "a<b> &AB &bogus; & x");
  CHECK(stripHtml("<b>unterminated <i") == "unterminated");
  CHECK(stripHtml("<STYLE>p{}</STYLE>x<script type=a>y</script >z") == "x z");
  CHECK(stripHtml("&#1114112;").size() == 3);   // out of range -> U+FFFD
  CHECK(stripHtml("") == "");
  CHECK(glossKeywords("to love; be fond of") == std::vector<std::string>{"love", "fond"});
  CHECK(glossKeywords("the Waters (of a river), BANKS") == std::vector<std::string>{"water", "river", "bank"});
  CHECK(glossKeywords("a, the, of 12") .empty());
  CHECK(glossKeywords("cities, boxes, glass") == std::vector<std::string>{"city", "box", "glass"});
  CHECK(clipSummary("short") == "short");
  const std::string longText(400, 'x');
  const std::string c = clipSummary(longText);
  CHECK(c.size() == 159 + 3);   // 159 letters + the ellipsis (3 bytes) = 160 code points
  std::string greek;
  for (int i = 0; i < 200; ++i) greek += "\xCE\xB1";   // alpha x 200
  const std::string cg = clipSummary(greek, 10);
  CHECK(cg.size() == 9 * 2 + 3);
}

TEST_CASE("online: verdicts") {
  const Section aqua = parseDefinitions(fixture("la_aqua.json"), "la").value();
  Evidence e = verdictFor(aqua, "water", "u");
  CHECK(e.verdict == Evidence::Agrees);
  CHECK(e.summary == "Wiktionary: water");
  CHECK(e.url == "u");
  CHECK(verdictFor(aqua, "the waters", "u").verdict == Evidence::Agrees);    // plural stem
  CHECK(verdictFor(aqua, "conduit, pipe", "u").verdict == Evidence::Agrees);  // a later sense counts
  e = verdictFor(aqua, "fire; flame", "u");
  CHECK(e.verdict == Evidence::Disagrees);
  CHECK(e.summary == "Wiktionary: water");   // the first definition
  CHECK(verdictFor(aqua, "of the", "u").verdict == Evidence::Unknown);         // no content word
  e = verdictFor(parseDefinitions(fixture("no_latin.json"), "la").value(), "boat", "u");
  CHECK(e.verdict == Evidence::Unknown);
  CHECK(e.summary == "no Wiktionary entry");
  const Section logos = parseDefinitions(fixture("grc_logos.json"), "grc").value();
  CHECK(verdictFor(logos, "word", "u").verdict == Evidence::Agrees);
  CHECK(verdictFor(logos, "reasons", "u").verdict == Evidence::Agrees);
  CHECK(verdictFor(logos, "horse", "u").verdict == Evidence::Disagrees);
  Section many;
  many.found = true;
  many.definitions.push_back(std::string(500, 'y'));
  CHECK(verdictFor(many, "horse", "u").summary.size() <= 160 * 4);
  CHECK(std::string(verdictName(Evidence::Disagrees)) == "disagrees");
}

TEST_CASE("online: titles and URLs") {
  CHECK(Wiktionary::pageTitle("aqua") == "aqua");
  CHECK(Wiktionary::pageTitle("  \xC4\x81" "qua ") == "aqua");                 // a with macron
  CHECK(Wiktionary::pageTitle("Iu\xCC\x84lius") == "Iulius");                   // combining macron
  CHECK(Wiktionary::pageTitle("\xCE\xBB\xCF\x8C\xCE\xB3\xCE\xBF\xCF\x82") ==     // λόγος keeps its accent
        "\xCE\xBB\xCF\x8C\xCE\xB3\xCE\xBF\xCF\x82");
  CHECK(Wiktionary::pageTitle("\xCF\x87\xCF\x8E\xCF\x81\xE1\xBE\xB1") ==         // χώρᾱ -> χώρα
        "\xCF\x87\xCF\x8E\xCF\x81\xCE\xB1");
  CHECK(Wiktionary::apiUrl("aqua") == "https://en.wiktionary.org/api/rest_v1/page/definition/aqua");
  CHECK(Wiktionary::apiUrl("res publica") == "https://en.wiktionary.org/api/rest_v1/page/definition/res_publica");
  CHECK(Wiktionary::apiUrl("a/b?c") == "https://en.wiktionary.org/api/rest_v1/page/definition/a%2Fb%3Fc");
  CHECK(Wiktionary::apiUrl("\xCE\xBB\xCF\x8C\xCE\xB3\xCE\xBF\xCF\x82") ==
        "https://en.wiktionary.org/api/rest_v1/page/definition/%CE%BB%CF%8C%CE%B3%CE%BF%CF%82");
  CHECK(Wiktionary::pageUrl("la", "aqua") == "https://en.wiktionary.org/wiki/aqua#Latin");
  CHECK(Wiktionary::pageUrl("grc", "x") == "https://en.wiktionary.org/wiki/x#Ancient_Greek");
  CHECK(defaultUserAgent().rfind("vetus-poeta/", 0) == 0);
  CHECK(defaultUserAgent().find("https://github.com/oldenKnight/vetusPoeta") != std::string::npos);
}

TEST_CASE("online: zero transport calls while the settings say off") {
  Rig rig("guard", false);
  CHECK_FALSE(onlineAllowed(rig.settings.s));
  vp::Result<std::unique_ptr<Wiktionary>> c = Wiktionary::create(rig.mock, rig.settings.s, rig.cfg);
  CHECK_FALSE(c.ok());
  CHECK(c.error().code == vp::ErrorCode::OnlineDisabled);
  vp::Result<std::unique_ptr<Transport>> t = makeSystemTransport(rig.settings.s);
  CHECK_FALSE(t.ok());
  CHECK(t.error().code == vp::ErrorCode::OnlineDisabled);
  for (int i = 0; i < 100; ++i) {
    const Evidence e = rig.check("la", "aqua", "water");
    CHECK(e.verdict == Evidence::Error);
    CHECK(e.summary == "online disabled");
  }
  CHECK(rig.client->probe()->summary == "online disabled");
  CHECK(rig.mock.calls() == 0);
  // engines.online alone is not enough, nor online.wiktionary alone
  rig.settings.set(true, false);
  CHECK(rig.check("la", "aqua", "water").summary == "online disabled");
  rig.settings.set(false, true);
  CHECK(rig.check("la", "aqua", "water").summary == "online disabled");
  CHECK_FALSE(makeSystemTransport(rig.settings.s).ok());
  CHECK(rig.mock.calls() == 0);
  const Stats st = rig.client->stats();
  CHECK(st.requests == 0);
  CHECK(st.cacheHits == 0);
  // read at call time: switching on lets the next call through, switching off stops the one after
  rig.settings.set(true, true);
  rig.mock.push(ok200(fixture("la_aqua.json")));
  CHECK(rig.check("la", "aqua", "water").verdict == Evidence::Agrees);
  CHECK(rig.mock.calls() == 1);
  CHECK(Wiktionary::create(rig.mock, rig.settings.s, rig.cfg).ok());
  rig.settings.set(false, false);
  CHECK(rig.check("la", "puella", "girl").summary == "online disabled");
  CHECK(rig.mock.calls() == 1);
  // a settings file with wrong types counts as off
  TestSettings odd("guard_odd", false);
  CHECK_FALSE(odd.s.set(nlohmann::json{{"engines", {{"online", "yes"}}}}).ok());
  CHECK_FALSE(onlineAllowed(odd.s));
}

TEST_CASE("online: request URL and headers") {
  Rig rig("headers");
  rig.mock.push(ok200(fixture("la_aqua.json")));
  CHECK(rig.check("la", "\xC4\x81qua", "water").url == "https://en.wiktionary.org/wiki/aqua#Latin");
  REQUIRE(rig.mock.urls().size() == 1);
  CHECK(rig.mock.urls()[0] == "https://en.wiktionary.org/api/rest_v1/page/definition/aqua");
  const Headers& h = rig.mock.lastHeaders();
  REQUIRE(h.count("User-Agent"));
  CHECK(h.at("User-Agent") == defaultUserAgent());
  CHECK(h.at("Accept") == "application/json");
  CHECK(h.size() == 2);
  CHECK_FALSE(rig.client->checkLemma("en", "water", "water").ok());   // bad_params
  CHECK(rig.client->checkLemma("la", "  ", "x").error().code == vp::ErrorCode::BadParams);
  CHECK(rig.mock.calls() == 1);
}

TEST_CASE("online: throttle 1 request per second, injected clock") {
  SUBCASE("token bucket") {
    Throttle t(1000);
    CHECK(t.reserve(5000) == 0);
    CHECK(t.reserve(5000) == 1000);
    CHECK(t.reserve(5100) == 1900);   // two slots queued
    CHECK(t.reserve(8000) == 0);      // idle long enough: the token is back
    CHECK(t.reserve(8999) == 1);
  }
  SUBCASE("client waits between requests, never on cache hits") {
    Rig rig("throttle");
    rig.mock.setDefault(ok200(fixture("la_aqua.json")));
    rig.check("la", "aqua", "water");
    rig.check("la", "aqua2", "water");
    rig.check("la", "aqua3", "water");
    CHECK(rig.clock.sleeps == std::vector<int64_t>{1000, 1000});
    REQUIRE(rig.mock.callTimes.size() == 3);
    CHECK(rig.mock.callTimes[1] - rig.mock.callTimes[0] >= 1000);
    CHECK(rig.mock.callTimes[2] - rig.mock.callTimes[1] >= 1000);
    rig.check("la", "aqua", "water");   // cache hit: no request, no sleep
    CHECK(rig.mock.calls() == 3);
    CHECK(rig.clock.sleeps.size() == 2);
    CHECK(rig.client->stats().throttled == 2);
  }
  SUBCASE("latency counts toward the interval") {
    Rig rig("throttle_latency");
    rig.mock.setDefault(ok200(fixture("la_aqua.json"), 300));
    rig.check("la", "aqua", "water");
    rig.check("la", "aqua2", "water");
    CHECK(rig.clock.sleeps == std::vector<int64_t>{700});
  }
  SUBCASE("the bucket is shared by every client that uses it") {
    Rig a("throttle_a"), b("throttle_b");
    Config cb = b.cfg;
    cb.throttle = &a.throttle;
    cb.clock = &a.clock;
    MockTransport mb(&a.clock);
    mb.setDefault(ok200(fixture("la_aqua.json")));
    a.mock.setDefault(ok200(fixture("la_aqua.json")));
    Wiktionary second(mb, b.settings.s, cb);
    a.check("la", "aqua", "water");
    CHECK(second.checkLemma("la", "aqua", "water")->verdict == Evidence::Agrees);
    CHECK(a.clock.sleeps == std::vector<int64_t>{1000});
    CHECK(&sharedThrottle() == &sharedThrottle());
  }
}

TEST_CASE("online: 429 and 503 with Retry-After") {
  SUBCASE("Retry-After within 60 s is honoured once") {
    Rig rig("ra_ok");
    rig.mock.push(status(429, "5"));
    rig.mock.push(ok200(fixture("la_aqua.json")));
    CHECK(rig.check("la", "aqua", "water").verdict == Evidence::Agrees);
    CHECK(rig.mock.calls() == 2);
    REQUIRE(!rig.clock.sleeps.empty());
    CHECK(rig.clock.sleeps[0] == 5000);
    CHECK(rig.mock.callTimes[1] - rig.mock.callTimes[0] >= 5000);
    CHECK(rig.client->stats().requests == 2);
    CHECK(rig.client->stats().throttled >= 1);
  }
  SUBCASE("a second 429 gives Unknown and a 60 s back-off") {
    Rig rig("ra_twice");
    rig.mock.push(status(429, "2"));
    rig.mock.push(status(429, "2"));
    rig.mock.setDefault(ok200(fixture("la_aqua.json")));
    Evidence e = rig.check("la", "aqua", "water");
    CHECK(e.verdict == Evidence::Unknown);
    CHECK(e.summary == "rate limited");
    CHECK(rig.mock.calls() == 2);
    rig.clock.advance(30000);
    CHECK(rig.check("la", "puer", "boy").summary == "rate limited");   // inside the back-off: no request
    CHECK(rig.mock.calls() == 2);
    rig.clock.advance(31000);
    CHECK(rig.check("la", "puer", "boy").verdict != Evidence::Error);
    CHECK(rig.mock.calls() == 3);
  }
  SUBCASE("Retry-After above 60 s, missing or a date: no retry") {
    for (const char* ra : {"120", "", "Wed, 21 Oct 2026 07:28:00 GMT"}) {
      Rig rig(std::string("ra_long_") + std::to_string(std::string(ra).size()));
      rig.mock.push(status(429, ra));
      const Evidence e = rig.check("la", "aqua", "water");
      CHECK(e.verdict == Evidence::Unknown);
      CHECK(e.summary == "rate limited");
      CHECK(rig.mock.calls() == 1);
      CHECK(rig.clock.slept() == 0);
    }
  }
  SUBCASE("a long Retry-After lengthens the back-off") {
    Rig rig("ra_backoff");
    rig.mock.push(status(429, "300"));
    rig.mock.setDefault(ok200(fixture("la_aqua.json")));
    rig.check("la", "aqua", "water");
    rig.clock.advance(120000);
    CHECK(rig.check("la", "aqua", "water").summary == "rate limited");
    rig.clock.advance(181000);
    CHECK(rig.check("la", "aqua", "water").verdict == Evidence::Agrees);
  }
  SUBCASE("503 is treated like 429") {
    Rig rig("ra_503");
    rig.mock.push(status(503, "1"));
    rig.mock.push(ok200(fixture("la_aqua.json")));
    CHECK(rig.check("la", "aqua", "water").verdict == Evidence::Agrees);
    CHECK(rig.clock.sleeps.front() == 1000);
  }
  SUBCASE("the settings are re-read after the wait") {
    Rig rig("ra_off");
    rig.mock.push(status(429, "3"));
    rig.settings.set(true, true);
    struct OffClock : FakeClock {
      TestSettings* s = nullptr;
      void sleepMs(int64_t ms) override {
        FakeClock::sleepMs(ms);
        s->set(false, false);
      }
    } clk;
    clk.s = &rig.settings;
    Config c = rig.cfg;
    c.clock = &clk;
    Wiktionary w(rig.mock, rig.settings.s, c);
    CHECK(w.checkLemma("la", "aqua", "water")->summary == "online disabled");
    CHECK(rig.mock.calls() == 1);
  }
}

TEST_CASE("online: network errors, timeouts, bad status, malformed JSON") {
  Rig rig("errors");
  MockTransport::Step down;
  down.fail = true;
  down.error = "no connection (name not resolved)";
  rig.mock.push(down);
  Evidence e = rig.check("la", "aqua", "water");
  CHECK(e.verdict == Evidence::Error);
  CHECK(e.summary == "no connection (name not resolved)");
  CHECK(e.url == "https://en.wiktionary.org/wiki/aqua#Latin");
  rig.mock.push(ok200(fixture("la_aqua.json"), 9000));   // longer than the 8 s timeout
  e = rig.check("la", "aqua", "water");
  CHECK(e.verdict == Evidence::Error);
  CHECK(e.summary == "timed out after 8000 ms");
  rig.mock.push(status(500));
  CHECK(rig.check("la", "aqua", "water").summary == "HTTP 500 from Wiktionary");
  rig.mock.push(ok200(fixture("malformed.json")));
  e = rig.check("la", "aqua", "water");
  CHECK(e.verdict == Evidence::Error);
  CHECK(e.summary == "malformed response from Wiktionary");
  CHECK(rig.client->stats().errors == 4);
  CHECK(rig.client->stats().requests == 4);
  // nothing of that was cached
  CHECK_FALSE(vp::fs::fileExists(rig.client->cachePath(Wiktionary::apiUrl("aqua"))));
  // 404 -> Unknown, no error
  rig.mock.push(MockTransport::Step{404, fixture("not_found.json"), {}, 0, false, {}});
  e = rig.check("la", "zzqqxx", "nothing");
  CHECK(e.verdict == Evidence::Unknown);
  CHECK(e.summary == "no Wiktionary entry");
  CHECK(rig.client->stats().errors == 4);
}

TEST_CASE("online: cache read, expiry, corruption, probe") {
  Rig rig("cache");
  rig.mock.setDefault(ok200(fixture("la_aqua.json")));
  CHECK(rig.check("la", "aqua", "water").verdict == Evidence::Agrees);
  const std::string path = rig.client->cachePath(Wiktionary::apiUrl("aqua"));
  CHECK(path == rig.cfg.cacheDir + "/" + vp::sha256Hex(Wiktionary::apiUrl("aqua")) + ".json");
  REQUIRE(vp::fs::isRegularFile(path));
  const std::string stored = vp::fs::readFile(path).value();
  CHECK(stored.rfind("{\"fetchedAt\":" + std::to_string(rig.clock.unix), 0) == 0);
  const nlohmann::json j = nlohmann::json::parse(stored);
  CHECK(j["body"] == fixture("la_aqua.json"));
  CHECK(j["status"] == 200);
  // a hit: no request, no throttle
  CHECK(rig.check("la", "\xC4\x81qua", "fire").verdict == Evidence::Disagrees);   // the verdict uses the new gloss
  CHECK(rig.mock.calls() == 1);
  CHECK(rig.client->stats().cacheHits == 1);
  // a second client over the same folder reads it too
  Wiktionary other(rig.mock, rig.settings.s, rig.cfg);
  CHECK(other.checkLemma("la", "aqua", "water")->verdict == Evidence::Agrees);
  CHECK(rig.mock.calls() == 1);
  // 29 days: still valid; 31 days: refetched
  rig.clock.advance(29LL * 24 * 3600 * 1000);
  rig.check("la", "aqua", "water");
  CHECK(rig.mock.calls() == 1);
  rig.clock.advance(2LL * 24 * 3600 * 1000);
  rig.check("la", "aqua", "water");
  CHECK(rig.mock.calls() == 2);
  rig.check("la", "aqua", "water");
  CHECK(rig.mock.calls() == 2);   // the refreshed entry is valid again
  // 404 answers are cached as well
  rig.mock.push(MockTransport::Step{404, fixture("not_found.json"), {}, 0, false, {}});
  rig.check("la", "zzqqxx", "x");
  CHECK(rig.check("la", "zzqqxx", "x").summary == "no Wiktionary entry");
  CHECK(rig.mock.calls() == 3);
  // a damaged file is removed and refetched
  REQUIRE(vp::fs::writeFileAtomic(path, "{\"fetchedAt\":1,\"status\":").ok());
  CHECK(rig.check("la", "aqua", "water").verdict == Evidence::Agrees);
  CHECK(rig.mock.calls() == 4);
  // probe() always asks the server (and refreshes the cache)
  CHECK(rig.client->probe()->verdict == Evidence::Agrees);
  CHECK(rig.mock.calls() == 5);
  CHECK(rig.mock.urls().back() == Wiktionary::apiUrl("aqua"));
}

TEST_CASE("online: cache folder size cap removes the oldest entries") {
  Rig rig("cap");
  rig.cfg.cacheMaxBytes = 6000;
  rig.client = std::make_unique<Wiktionary>(rig.mock, rig.settings.s, rig.cfg);
  // ~1.1 KB per entry: a valid Latin page padded with a long definition
  const std::string body = R"({"la":[{"definitions":[{"definition":"water )" + std::string(1000, 'w') + R"("}]}]})";
  rig.mock.setDefault(ok200(body));
  for (int i = 0; i < 12; ++i) {
    rig.check("la", "lemma" + std::to_string(i), "water");
    rig.clock.advance(10);   // distinct fetch times (sleeps advance it too)
    CHECK(dirBytes(rig.cfg.cacheDir) <= rig.cfg.cacheMaxBytes);
  }
  CHECK_FALSE(vp::fs::fileExists(rig.client->cachePath(Wiktionary::apiUrl("lemma0"))));
  CHECK_FALSE(vp::fs::fileExists(rig.client->cachePath(Wiktionary::apiUrl("lemma1"))));
  CHECK(vp::fs::fileExists(rig.client->cachePath(Wiktionary::apiUrl("lemma11"))));
  CHECK(vp::fs::fileExists(rig.client->cachePath(Wiktionary::apiUrl("lemma10"))));
  // foreign files in the folder are never touched
  const std::string foreign = rig.cfg.cacheDir + "/README.txt";
  REQUIRE(vp::fs::writeFileAtomic(foreign, std::string(100, 'r')).ok());
  for (int i = 12; i < 20; ++i) rig.check("la", "lemma" + std::to_string(i), "water");
  CHECK(vp::fs::fileExists(foreign));
}

TEST_CASE("online: stats and the rules-engine hook") {
  Rig rig("stats");
  rig.mock.push(ok200(fixture("la_aqua.json")));
  rig.mock.push(ok200(fixture("grc_logos.json")));
  MockTransport::Step down;
  down.fail = true;
  rig.mock.push(down);
  OnlineCheck hook = makeOnlineCheck(*rig.client);
  CHECK(hook("la", "aqua", "water").verdict == Evidence::Agrees);
  const Evidence g = hook("grc", "\xCE\xBB\xCF\x8C\xCE\xB3\xCE\xBF\xCF\x82", "word, speech");
  CHECK(g.verdict == Evidence::Agrees);
  CHECK(g.url == "https://en.wiktionary.org/wiki/%CE%BB%CF%8C%CE%B3%CE%BF%CF%82#Ancient_Greek");
  CHECK(hook("la", "puella", "girl").verdict == Evidence::Error);
  CHECK(hook("la", "aqua", "water").verdict == Evidence::Agrees);   // cached
  const Evidence bad = hook("xx", "aqua", "water");
  CHECK(bad.verdict == Evidence::Error);
  CHECK(bad.summary.find("unknown language") != std::string::npos);
  const Stats st = rig.client->stats();
  CHECK(st.requests == 3);
  CHECK(st.cacheHits == 1);
  CHECK(st.errors == 1);
  CHECK(st.throttled == 2);
}

TEST_CASE("online: live Wiktionary (VP_ONLINE_LIVE=1 only)") {
  const char* live = std::getenv("VP_ONLINE_LIVE");
  if (!live || std::string(live) != "1") {
    MESSAGE("skipped: set VP_ONLINE_LIVE=1 to query en.wiktionary.org once");
    return;
  }
  TestSettings settings("live", true);
  vp::Result<std::unique_ptr<Transport>> t = makeSystemTransport(settings.s);
  REQUIRE_MESSAGE(t.ok(), t.error().message);
  Config cfg;
  cfg.cacheDir = freshDir("cache_live");
  vp::Result<std::unique_ptr<Wiktionary>> w = Wiktionary::create(*t.value(), settings.s, cfg);
  REQUIRE(w.ok());
  const auto t0 = std::chrono::steady_clock::now();
  vp::Result<Evidence> e = w.value()->checkLemma("la", "aqua", "water");
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
  REQUIRE(e.ok());
  MESSAGE("live aqua: " << std::string(verdictName(e->verdict)) << " in " << ms << " ms: " << e->summary);
  CHECK(e->verdict == Evidence::Agrees);
  vp::Result<Evidence> g = w.value()->checkLemma("grc", "\xCE\xBB\xCF\x8C\xCE\xB3\xCE\xBF\xCF\x82", "word");
  REQUIRE(g.ok());
  MESSAGE("live logos: " << std::string(verdictName(g->verdict)) << ": " << g->summary);
  CHECK(g->verdict == Evidence::Agrees);
}
