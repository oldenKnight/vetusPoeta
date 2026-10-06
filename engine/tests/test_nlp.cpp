// engine/nlp tests: feature hashes and strings against the Python golden list, the tiny models reproduce the
// Python outputs exactly, the big models too when present, tokeniser table, corrupt files, lemma rules, RSS.
#include <doctest.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "../nlp/src/feature_strings.h"
#include "vp/nlp.h"
#include "vp/sha256.h"

#if defined(__SANITIZE_ADDRESS__)
#define VP_NLP_SANITIZED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define VP_NLP_SANITIZED 1
#endif
#endif

namespace stdfs = std::filesystem;
using vp::nlp::Token;

namespace {

const std::string kFix = std::string(VP_FIXTURES_DIR) + "/nlp/";
const std::string kWork = std::string(VP_FIXTURES_DIR) + "/../../data/work/nlp/";

std::string readAll(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

void writeAll(const std::string& path, const std::string& data) {
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  f.write(data.data(), static_cast<std::streamsize>(data.size()));
}

std::string nlpTmp() {
  stdfs::path d = stdfs::path(VP_TEST_TMP) / "nlp";
  std::error_code ec;
  stdfs::create_directories(d, ec);
  return stdfs::absolute(d).string();
}

std::vector<std::string> splitTab(const std::string& line) {
  std::vector<std::string> out;
  size_t pos = 0;
  for (;;) {
    const size_t t = line.find('\t', pos);
    out.push_back(line.substr(pos, t == std::string::npos ? std::string::npos : t - pos));
    if (t == std::string::npos) break;
    pos = t + 1;
  }
  return out;
}

struct GoldTok {
  std::string word, upos, feats, deprel;
  int head = 0;
};
struct Golden {
  std::map<std::string, std::string> meta;
  std::vector<std::vector<GoldTok>> sents;
};

Golden readGolden(const std::string& path) {
  Golden g;
  std::ifstream f(path, std::ios::binary);
  std::string line;
  std::vector<GoldTok> cur;
  while (std::getline(f, line)) {
    if (line.compare(0, 3, "## ") == 0) {
      std::istringstream ss(line.substr(3));
      std::string kv;
      while (ss >> kv) {
        const size_t eq = kv.find('=');
        if (eq != std::string::npos) g.meta[kv.substr(0, eq)] = kv.substr(eq + 1);
      }
      continue;
    }
    if (line.empty()) {
      if (!cur.empty()) g.sents.push_back(std::move(cur));
      cur.clear();
      continue;
    }
    const auto c = splitTab(line);
    REQUIRE(c.size() == 5);
    cur.push_back({c[0], c[1], c[2], c[4], std::atoi(c[3].c_str())});
  }
  if (!cur.empty()) g.sents.push_back(std::move(cur));
  return g;
}

std::string fileSha(const std::string& path) { return vp::sha256Hex(readAll(path)); }

// Runs tagger + parser on every golden sentence; returns the number of token-level mismatches.
size_t compareGolden(const Golden& g, const vp::nlp::Tagger& tagger, const vp::nlp::Parser& parser, size_t& tokens) {
  size_t bad = 0;
  tokens = 0;
  for (const auto& s : g.sents) {
    std::vector<Token> toks(s.size());
    for (size_t i = 0; i < s.size(); ++i) toks[i].text = s[i].word;
    tagger.tag(toks);
    parser.parse(toks);
    for (size_t i = 0; i < s.size(); ++i) {
      ++tokens;
      const std::string feats = vp::nlp::morph::toString(toks[i].feats);
      if (toks[i].upos != s[i].upos || feats != s[i].feats || toks[i].head != s[i].head ||
          toks[i].deprel != s[i].deprel) {
        if (bad < 5)
          MESSAGE("mismatch at '" << s[i].word << "': got " << toks[i].upos << " " << feats << " " << toks[i].head
                                  << " " << toks[i].deprel << ", want " << s[i].upos << " " << s[i].feats << " "
                                  << s[i].head << " " << s[i].deprel);
        ++bad;
      }
    }
  }
  return bad;
}

std::string joinTokens(const std::vector<Token>& t) {
  std::string out;
  for (size_t i = 0; i < t.size(); ++i) {
    if (i) out += " | ";
    out += t[i].text;
  }
  return out;
}

#if defined(__linux__)
long statusKb(const char* field) {
  std::ifstream f("/proc/self/status");
  std::string line;
  const size_t n = std::char_traits<char>::length(field);
  while (std::getline(f, line))
    if (line.compare(0, n, field) == 0) return std::atol(line.c_str() + n);
  return -1;
}
#endif

}  // namespace

TEST_CASE("nlp: feature hashes and feature strings match the Python golden list") {
  std::ifstream f(kFix + "features_golden.tsv", std::ios::binary);
  REQUIRE(f.good());
  const Golden tiny = readGolden(kFix + "tiny.golden.tsv");
  REQUIRE(tiny.sents.size() == 200);
  std::string line;
  size_t rows = 0, ctxChecked = 0;
  std::map<std::string, std::vector<std::string>> byCtx;
  std::vector<std::string> order;
  while (std::getline(f, line)) {
    const auto c = splitTab(line);
    REQUIRE(c.size() == 3);
    ++rows;
    char hex[17];
    std::snprintf(hex, sizeof hex, "%016llx", static_cast<unsigned long long>(vp::nlp::featureHash(c[1])));
    CHECK_MESSAGE(c[2] == hex, "hash of '" << c[1] << "'");
    if (c[0] != "hash") {
      if (!byCtx.count(c[0])) order.push_back(c[0]);
      byCtx[c[0]].push_back(c[1]);
    }
  }
  CHECK(rows == 500);
  using namespace vp::nlp::detail;
  for (const auto& ctx : order) {
    const auto& want = byCtx[ctx];
    const auto parts = [&] {
      std::vector<std::string> p;
      std::stringstream ss(ctx);
      std::string x;
      while (std::getline(ss, x, ':')) p.push_back(x);
      return p;
    }();
    const auto& s = tiny.sents[static_cast<size_t>(std::atoi(parts[1].c_str()))];
    std::vector<std::string> wordsS, lowsS, shapes(s.size());
    for (const auto& t : s) {
      wordsS.push_back(t.word);
      lowsS.push_back(vp::nlp::normalise(t.word));
    }
    std::vector<std::string_view> words(wordsS.begin(), wordsS.end()), lows(lowsS.begin(), lowsS.end());
    for (size_t i = 0; i < s.size(); ++i) shape(s[i].word, shapes[i]);
    FeatBuf fb;
    if (parts[0] == "tag" || parts[0] == "tagx") {
      const size_t i = static_cast<size_t>(std::atoi(parts[2].c_str()));
      if (parts[0] == "tag") {
        const std::string_view t1 = i >= 1 ? std::string_view(s[i - 1].upos) : std::string_view(kBos);
        const std::string_view t2 = i >= 2 ? std::string_view(s[i - 2].upos) : std::string_view(i == 1 ? kBos : kBos2);
        tagFeatures(words, lows, shapes, i, t1, t2, fb);
      } else {
        tagFeatExtra(s[i].upos, lows[i], fb);
      }
    } else {
      std::vector<std::string_view> l2{kRoot}, t2{kRoot};
      for (size_t i = 0; i < s.size(); ++i) {
        l2.push_back(lows[i]);
        t2.push_back(s[i].upos);
      }
      ParseState st;
      st.reset(static_cast<int>(s.size()));
      if (parts[0] == "dep")
        parseFeatures(l2, t2, st, fb);
      else
        labelFeatures(l2, t2, 2, 1, "L", st, fb);
    }
    REQUIRE(fb.n >= want.size());   // the golden list may cut the last context short
    for (size_t k = 0; k < want.size(); ++k) CHECK_MESSAGE(fb.v[k] == want[k], ctx << " #" << k);
    ++ctxChecked;
  }
  CHECK(ctxChecked > 15);
}

TEST_CASE("nlp: tiny models reproduce the Python outputs on 200 golden sentences") {
  auto tg = vp::nlp::Tagger::open(kFix + "tiny.tag.vpt");
  REQUIRE_MESSAGE(tg.ok(), tg.error().message);
  auto dp = vp::nlp::Parser::open(kFix + "tiny.dep.vpt");
  REQUIRE_MESSAGE(dp.ok(), dp.error().message);
  CHECK(tg.value().lang() == "en");
  CHECK(tg.value().note().find("CC BY-SA 4.0") != std::string_view::npos);
  const Golden g = readGolden(kFix + "tiny.golden.tsv");
  REQUIRE(g.sents.size() == 200);
  CHECK(g.meta.at("tag_sha256") == fileSha(kFix + "tiny.tag.vpt"));
  size_t tokens = 0;
  CHECK(compareGolden(g, tg.value(), dp.value(), tokens) == 0);
  CHECK(tokens > 2000);
}

TEST_CASE("nlp: full models reproduce the Python outputs (skipped when data/work/nlp is absent)") {
  const std::vector<std::vector<std::string>> names{{"english", "en.golden.tsv", "en"},
                                                    {"spanish", "es.golden.tsv", "es"}};
  for (const auto& nm : names) {
    const std::string tagPath = kWork + nm[0] + ".tag.vpt", depPath = kWork + nm[0] + ".dep.vpt";
    if (!stdfs::exists(tagPath) || !stdfs::exists(depPath)) {
      MESSAGE("skipped " << nm[0] << ": " << tagPath << " or " << depPath << " not built (see tools/train/README.md)");
      continue;
    }
    Golden g = readGolden(kFix + nm[1]);
    REQUIRE(g.sents.size() == 200);
    if (g.meta["tag_sha256"] != fileSha(tagPath) || g.meta["dep_sha256"] != fileSha(depPath)) {
      MESSAGE("skipped " << nm[0] << ": the models on disk differ from the ones the golden file was made with");
      continue;
    }
    auto tg = vp::nlp::Tagger::open(tagPath);
    auto dp = vp::nlp::Parser::open(depPath);
    REQUIRE(tg.ok());
    REQUIRE(dp.ok());
    CHECK(tg.value().lang() == nm[2]);
    size_t tokens = 0;
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(compareGolden(g, tg.value(), dp.value(), tokens) == 0);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    MESSAGE(nm[0] << ": " << tokens << " golden tokens compared, tag + parse " << ms << " ms");
  }
}

TEST_CASE("nlp: tokeniser table") {
  using vp::nlp::Lang;
  struct Case {
    Lang lang;
    const char* in;
    const char* want;
  };
  const Case cases[] = {
      {Lang::En, "Hello, world!", "Hello | , | world | !"},
      {Lang::En, "I don't know.", "I | don't | know | ."},
      {Lang::En, "I'm here, aren't you?", "I'm | here | , | aren't | you | ?"},
      {Lang::En, "John's car is red.", "John's | car | is | red | ."},
      {Lang::En, "Wait...", "Wait | ..."},
      {Lang::En, "Wait\xE2\x80\xA6 what?", "Wait | \xE2\x80\xA6 | what | ?"},
      {Lang::En, "\xE2\x80\x9CHello,\xE2\x80\x9D she said.", "\xE2\x80\x9C | Hello | , | \xE2\x80\x9D | she | said | ."},
      {Lang::En, "Don\xE2\x80\x99t stop", "Don\xE2\x80\x99t | stop"},
      {Lang::En, "'Twas the night", "'Twas | the | night"},
      {Lang::En, "Tell 'em now.", "Tell | 'em | now | ."},
      {Lang::En, "I'm goin' home.", "I'm | goin' | home | ."},
      {Lang::En, "It costs $1,000.50 today.", "It | costs | $ | 1,000.50 | today | ."},
      {Lang::En, "Meet at 3:30 on 10/12.", "Meet | at | 3:30 | on | 10/12 | ."},
      {Lang::En, "A well-known fact.", "A | well-known | fact | ."},
      {Lang::En, "Mr. Smith went to Washington.", "Mr. | Smith | went | to | Washington | ."},
      {Lang::En, "The U.S. army.", "The | U.S. | army | ."},
      {Lang::En, "e.g. this", "e.g. | this"},
      {Lang::En, "Visit https://example.com/a?b=1.", "Visit | https://example.com/a?b=1 | ."},
      {Lang::En, "Mail me at bob@example.com!", "Mail | me | at | bob@example.com | !"},
      {Lang::En, "What?!", "What | ?!"},
      {Lang::En, "- Hello there.", "- | Hello | there | ."},
      {Lang::En, "Yes -- no.", "Yes | -- | no | ."},
      {Lang::En, "It's 5% off", "It's | 5 | % | off"},
      {Lang::En, "(maybe)", "( | maybe | )"},
      {Lang::En, "I can't\xE2\x80\x94really.", "I | can't | \xE2\x80\x94 | really | ."},
      {Lang::En, "We're 100% sure \xF0\x9F\x98\x80!", "We're | 100 | % | sure | \xF0\x9F\x98\x80 | !"},
      {Lang::En, "rock'n'roll", "rock'n'roll"},
      {Lang::En, "It was -5 degrees", "It | was | -5 | degrees"},
      {Lang::En, "the end.\"", "the | end | . | \""},
      {Lang::En, "Ice-cream, please.", "Ice-cream | , | please | ."},
      {Lang::En, "A tab\there\n", "A | tab | here"},
      {Lang::En, "", ""},
      {Lang::En, "Thumbs \xF0\x9F\x91\x8D\xF0\x9F\x8F\xBD up", "Thumbs | \xF0\x9F\x91\x8D\xF0\x9F\x8F\xBD | up"},
      {Lang::Es, "\xC2\xBF" "D\xC3\xB3nde est\xC3\xA1?", "\xC2\xBF | D\xC3\xB3nde | est\xC3\xA1 | ?"},
      {Lang::Es, "\xC2\xA1Hola, amigo!", "\xC2\xA1 | Hola | , | amigo | !"},
      {Lang::Es, "La Sra. Garc\xC3\xAD" "a lleg\xC3\xB3.", "La | Sra. | Garc\xC3\xAD" "a | lleg\xC3\xB3 | ."},
      {Lang::Es, "\xC2\xABYa voy\xC2\xBB, dijo.", "\xC2\xAB | Ya | voy | \xC2\xBB | , | dijo | ."},
      {Lang::Es, "Cuesta 1.000,50 euros.", "Cuesta | 1.000,50 | euros | ."},
      {Lang::Es, "Vamos al cine del barrio.", "Vamos | al | cine | del | barrio | ."},
      {Lang::Es, "Es el 1\xC2\xBA de mayo.", "Es | el | 1\xC2\xBA | de | mayo | ."},
      {Lang::Es, "D\xC3\xA1melo ya...", "D\xC3\xA1melo | ya | ..."},
  };
  CHECK(sizeof(cases) / sizeof(*cases) >= 40);
  for (const Case& c : cases) {
    const vp::nlp::Tokenizer tok(c.lang);
    CHECK_MESSAGE(joinTokens(tok.tokenize(c.in)) == c.want, "input: " << c.in);
  }
  // offsets and lower case
  const auto t = vp::nlp::Tokenizer(vp::nlp::Lang::Es).tokenize("\xC2\xBF" "D\xC3\xB3nde?");
  REQUIRE(t.size() == 3);
  CHECK(t[0].start == 0);
  CHECK(t[0].end == 2);
  CHECK(t[1].start == 2);
  CHECK(t[1].end == 8);
  CHECK(t[1].lower == "d\xC3\xB3nde");
  const auto u = vp::nlp::Tokenizer().tokenize("Don\xE2\x80\x99t");
  REQUIRE(u.size() == 1);
  CHECK(u[0].lower == "don't");
  // invalid UTF-8 never breaks it
  const auto bad = vp::nlp::Tokenizer().tokenize(std::string("ab\xFF\xC3 cd\xE2\x80", 9));
  CHECK(bad.size() >= 2);
}

TEST_CASE("nlp: corrupt or truncated .vpt files are rejected and never crash") {
  const std::string dir = nlpTmp();
  const std::string good = readAll(kFix + "tiny.tag.vpt");
  REQUIRE(good.size() > 1000);
  const std::string p = dir + "/bad.vpt";
  std::vector<Token> probe(3);
  probe[0].text = "The";
  probe[1].text = "dog";
  probe[2].text = "barks";
  // truncations
  std::vector<size_t> cuts{0, 1, 3, 4, 8, 40, 80, 255, 256, 257, 300, good.size() / 2, good.size() - 1};
  for (size_t c = 7; c < good.size(); c += 4093) cuts.push_back(c);
  for (size_t c : cuts) {
    writeAll(p, good.substr(0, c));
    auto r = vp::nlp::Tagger::open(p);
    CHECK_MESSAGE(!r.ok(), "truncated at " << c);
    if (!r.ok() && c > 0) CHECK(r.error().hint == "model file damaged");
  }
  // one extra byte
  writeAll(p, good + "x");
  CHECK(!vp::nlp::Tagger::open(p).ok());
  // every header byte flipped: never crashes; a model that still opens still tags
  for (size_t i = 0; i < 256; ++i) {
    std::string b = good;
    b[i] = static_cast<char>(b[i] ^ 0x5A);
    writeAll(p, b);
    auto r = vp::nlp::Tagger::open(p);
    if (r.ok()) r.value().tag(probe);
  }
  // body flips are caught by the SHA-256
  uint32_t x = 12345;
  for (int k = 0; k < 150; ++k) {
    x = x * 1664525u + 1013904223u;
    const size_t pos = 256 + x % (good.size() - 256);
    std::string b = good;
    b[pos] = static_cast<char>(b[pos] ^ (1 << (k % 8)));
    writeAll(p, b);
    CHECK_MESSAGE(!vp::nlp::Tagger::open(p).ok(), "flip at " << pos);
  }
  // wrong kind, missing file
  auto wrong = vp::nlp::Tagger::open(kFix + "tiny.dep.vpt");
  CHECK(!wrong.ok());
  auto wrong2 = vp::nlp::Parser::open(kFix + "tiny.tag.vpt");
  CHECK(!wrong2.ok());
  auto missing = vp::nlp::Parser::open(dir + "/does-not-exist.vpt");
  CHECK(!missing.ok());
  CHECK(missing.error().code == vp::ErrorCode::Io);
  // closed objects are no-ops
  vp::nlp::Tagger closed;
  closed.tag(probe);
  vp::nlp::Parser closedP;
  closedP.parse(probe);
  CHECK(!closed.isOpen());
}

TEST_CASE("nlp: pipeline, lemmatiser hook and rules, packed features") {
  auto pl = vp::nlp::Pipeline::open(vp::nlp::Lang::En, kFix + "tiny.tag.vpt", kFix + "tiny.dep.vpt");
  REQUIRE(pl.ok());
  auto toks = pl.value().analyse("The children walked to the stores.");
  REQUIRE(toks.size() == 7);
  int roots = 0;
  for (const auto& t : toks) {
    CHECK(!t.upos.empty());
    CHECK(!t.deprel.empty());
    CHECK(t.head >= 0);
    CHECK(t.head <= 7);
    roots += t.head == 0;
  }
  CHECK(roots == 1);
  CHECK(toks[1].lemma == "child");
  pl.value().setLemmatizer([](const std::string& lower, const std::string&) {
    if (lower == "walked") return std::string("WALK");
    if (lower == "stores") throw 42;
    return std::string();
  });
  toks = pl.value().analyse("The children walked to the stores.");
  CHECK(toks[2].lemma == "WALK");
  CHECK(toks[1].lemma == "child");
  // rules
  auto lem = [](const char* w, const char* upos) {
    Token t;
    t.text = w;
    t.lower = vp::nlp::normalise(w);
    t.upos = upos;
    return vp::nlp::ruleLemma(vp::nlp::Lang::En, t);
  };
  CHECK(lem("cats", "NOUN") == "cat");
  CHECK(lem("boxes", "NOUN") == "box");
  CHECK(lem("cities", "NOUN") == "city");
  CHECK(lem("glass", "NOUN") == "glass");
  CHECK(lem("stopped", "VERB") == "stop");
  CHECK(lem("liked", "VERB") == "like");
  CHECK(lem("walking", "VERB") == "walk");
  CHECK(lem("making", "VERB") == "make");
  CHECK(lem("tried", "VERB") == "try");
  CHECK(lem("called", "VERB") == "call");
  CHECK(lem("was", "AUX") == "be");
  CHECK(lem("went", "VERB") == "go");
  CHECK(lem("happier", "ADJ") == "happy");
  CHECK(lem("taller", "ADJ") == "tall");
  CHECK(lem("London", "PROPN") == "London");
  Token es;
  es.text = "Cantaban";
  es.lower = "cantaban";
  es.upos = "VERB";
  CHECK(vp::nlp::ruleLemma(vp::nlp::Lang::Es, es) == "cantaban");
  // packed features
  using namespace vp::nlp::morph;
  const uint32_t f = fromString("Number=Sing|Person=3|Tense=Past|VerbForm=Fin|Mood=Ind|PronType=Int,Rel");
  CHECK(get(f, NumberShift) == NumSing);
  CHECK(get(f, PersonShift) == Pers3);
  CHECK(get(f, TenseShift) == TensePast);
  CHECK(get(f, VerbFormShift) == VfFin);
  CHECK(get(f, MoodShift) == MoodInd);
  CHECK(get(f, PronTypeShift) == PtIntRel);
  CHECK(toString(f) == "Number=Sing|Person=3|Tense=Past|VerbForm=Fin|Mood=Ind|PronType=Int,Rel");
  CHECK(toString(0) == "_");
  CHECK(fromString("Case=Nom|Number=Plur|Bogus") == static_cast<uint32_t>(NumPlur));
}

TEST_CASE("nlp: RSS stays flat over 10,000 sentences") {
  auto pl = vp::nlp::Pipeline::open(vp::nlp::Lang::En, kFix + "tiny.tag.vpt", kFix + "tiny.dep.vpt");
  REQUIRE(pl.ok());
  const Golden g = readGolden(kFix + "tiny.golden.tsv");
  std::vector<std::string> sents;
  for (const auto& s : g.sents) {
    std::string line;
    for (const auto& t : s) line += t.word + " ";
    sents.push_back(line);
  }
#if defined(__linux__)
  long rss1k = -1;
  size_t sink = 0;
  for (int i = 1; i <= 10000; ++i) {
    sink += pl.value().analyse(sents[static_cast<size_t>(i) % sents.size()]).size();
    if (i == 1000) rss1k = statusKb("VmRSS:");
  }
  const long rss10k = statusKb("VmRSS:");
  MESSAGE("VmRSS after 1,000 sentences: " << rss1k << " kB, after 10,000: " << rss10k << " kB (" << sink << " tokens)");
  REQUIRE(rss1k > 0);
#if !defined(VP_NLP_SANITIZED)
  CHECK(static_cast<double>(rss10k) <= static_cast<double>(rss1k) * 1.05);
#endif
#else
  MESSAGE("RSS check skipped (not Linux)");
#endif
}
