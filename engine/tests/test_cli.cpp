// Tests for the pure helpers of engine/cli (views.h) and the stub rules engine. The protocol itself is tested by
// engine/tests/test_server.py.
#include <doctest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "views.h"
#include "vp/lex.h"
#include "vp/rules.h"
#include "vp/subs.h"

using namespace vpcli;
namespace subs = vp::subs;

namespace {
std::vector<uint8_t> fileBytes(const std::string& rel) {
  std::ifstream f(std::filesystem::path(VP_FIXTURES_DIR) / rel, std::ios::binary);
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}
std::string flatSpans(const std::vector<subs::Span>& v) {
  std::string s;
  for (const subs::Span& x : v) s += x.kind == subs::Span::Newline ? std::string("|") : x.raw;
  return s;
}
subs::Document parseSrt(const std::string& text) {
  std::vector<uint8_t> b(text.begin(), text.end());
  return subs::parse(b, subs::Format::Srt).value();
}
}  // namespace

TEST_CASE("cli: feature strings, pairs, formats") {
  using namespace vp::feat;
  Features f;
  f.pos = Noun; f.case_ = Acc; f.number = Sg; f.gender = F;
  vp::rules::Features v = featuresFromPacked(pack(f));
  CHECK(v.pos == "noun");
  CHECK(v.case_ == "accusative");
  CHECK(v.number == "singular");
  CHECK(v.gender == "feminine");
  CHECK(v.tense.empty());
  Features g;
  g.pos = Verb; g.mood = ParticipleMood; g.extra = Gerundive; g.person = P3; g.tense = FuturePerfect;
  v = featuresFromPacked(pack(g));
  CHECK(v.mood == "gerundive");
  CHECK(v.person == "third");
  CHECK(v.tense == "future-perfect");
  CHECK(featureJson(v)["case"] == "");
  CHECK(lemmaFlagNames(vp::lex::ProperName | vp::lex::Deponent) == std::vector<std::string>{"proper-name", "deponent"});

  PairLangs pl;
  CHECK(parsePair("es-grc", pl));
  CHECK(pl.source == vp::rules::Lang::Es);
  CHECK(pl.target == vp::rules::Lang::Grc);
  CHECK_FALSE(parsePair("en-fr", pl));
  CHECK_FALSE(parsePair("enla", pl));

  subs::Format fm;
  CHECK((formatFromPath("C:\\Films\\A.SSA", fm) && fm == subs::Format::Ass));
  CHECK((formatFromPath("/x/y.srt", fm) && fm == subs::Format::Srt));
  CHECK_FALSE(formatFromPath("/x/y.docx", fm));
  CHECK_FALSE(formatFromPath("/x.y/noext", fm));
  CHECK(baseName("a/b\\c.srt") == "c.srt");
  CHECK(looksGreek("λόγος"));
  CHECK(looksGreek("ἄνθρωπος"));
  CHECK_FALSE(looksGreek("Iūlius"));
  CHECK(lexKey("Iūlius", false) == "iulius");
}

TEST_CASE("cli: target text transforms") {
  CHECK(stripAddedEmoji("Canis 🐕 dormit.", "The dog sleeps.") == "Canis dormit.");
  CHECK(stripAddedEmoji("♪ Cantāmus 👨‍👩‍👧 ♪", "♪ We sing ♪") == "♪ Cantāmus ♪");
  CHECK(stripAddedEmoji("Ecce 🙂!", "Look 🙂!") == "Ecce 🙂!");
  CHECK(stripAddedEmoji("Rosa 🌹️ pulchra", "") == "Rosa pulchra");
  CHECK(stripAddedEmoji("plain", "") == "plain");
  CHECK(greekMonotonic("ὁ ἄνθρωπος") == "ο άνθρωπος");
  CHECK(greekMonotonic("τῇ οἰκίᾳ") == "τη οικία");
  CHECK(greekMonotonic("καὶ πού; ἢ πῶς") == "και πού; ή πώς");
  CHECK(greekMonotonic("Ἀθηνᾶ") == "Αθηνά");
  CHECK(flattenLines(" a\nb  c\r\n") == "a b c");
  CHECK(splitLines("a\r\nb") == std::vector<std::string>{"a", "b"});
  CHECK(splitLines("").empty());

  bool over = true;
  CHECK(displayLines("Short one.\nTwo.", {}, 42, 2, &over) == std::vector<std::string>{"Short one.", "Two."});
  CHECK_FALSE(over);
  std::vector<std::string> l = displayLines("Agricola agros suos et parvam casam suam valde amat et colit.", {}, 42, 2, &over);
  CHECK(l.size() == 2);
  for (const std::string& x : l) CHECK(x.size() <= 42);

  CHECK(markupCheck("one\ntwo", 42, 2).ok);
  CHECK_FALSE(markupCheck("one\ntwo\nthree", 42, 2).ok);
  vp::rules::Check c = markupCheck(std::string(50, 'a'), 42, 2);
  CHECK_FALSE(c.ok);
  CHECK(c.id == "A5");
  CHECK(formatTiming(3723456, 3725000, subs::Format::Srt) == "01:02:03,456 --> 01:02:05,000");
  CHECK(formatTiming(3723456, 3725000, subs::Format::Vtt) == "01:02:03.456 --> 01:02:05.000");
  CHECK(formatTiming(3723456, 3725000, subs::Format::Ass) == "1:02:03.45,1:02:05.00");
}

TEST_CASE("cli: tags re-attached around new text") {
  bool dropped = true;
  auto spans = subs::splitSpans("<i>Hello there.</i>", subs::Format::Srt);
  CHECK(flatSpans(retargetSpans(spans, "Salvē.", true, &dropped)) == "<i>Salvē.</i>");
  CHECK_FALSE(dropped);
  spans = subs::splitSpans("{\\an8}<i>One\ntwo</i>", subs::Format::Srt);
  CHECK(flatSpans(retargetSpans(spans, "Ūnus\nduo", false, &dropped)) == "{\\an8}<i>Ūnus|duo</i>");
  spans = subs::splitSpans("<i>One</i>\n<i>two</i>", subs::Format::Srt);
  CHECK(flatSpans(retargetSpans(spans, "Ūnus duo", true, &dropped)) == "<i>Ūnus duo</i>");
  CHECK(dropped);
  spans = subs::splitSpans("Partly <i>italic</i>", subs::Format::Srt);
  CHECK(flatSpans(retargetSpans(spans, "Ex parte", true, &dropped)) == "Ex parte");
  CHECK(dropped);
  spans = subs::splitSpans("<b><i>Both", subs::Format::Srt);
  CHECK(flatSpans(retargetSpans(spans, "Ambo", true, &dropped)) == "<b><i>Ambo</i></b>");
  spans = subs::splitSpans("{\\i1}Ass text{\\i0}", subs::Format::Ass);
  CHECK(flatSpans(retargetSpans(spans, "Textus", true, &dropped)) == "{\\i1}Textus{\\i0}");
}

TEST_CASE("cli: export keeps numbering, timing and untouched cues byte for byte") {
  const std::vector<uint8_t> src = fileBytes("cli/thirty_cues.srt");
  REQUIRE(src.size() > 100);
  subs::Document d = subs::parse(src, subs::Format::Srt).value();
  REQUIRE(d.cues.size() == 30);
  std::vector<std::string> sources, targets;
  for (const subs::Cue& c : d.cues) sources.push_back(c.plainText());
  ExportOptions o;
  o.macrons = true;
  ExportResult same = buildExport(d, targets, sources, o).value();   // nothing translated: identical file
  CHECK(same.bytes == src);
  CHECK(same.warnings.size() == 30);
  CHECK(same.warnings[29].index == 29);
  targets = sources;   // echo: single-line cues are unchanged, two-line cues re-broken the same way or joined
  ExportResult echo = buildExport(d, targets, sources, o).value();
  const std::string out(echo.bytes.begin(), echo.bytes.end());
  CHECK(out.find("4\n00:00:10,000 --> 00:00:12,400\n{\\an8}Rome, many years ago.\n") != std::string::npos);
  CHECK(out.find("2\n00:00:04,000 --> 00:00:06,400\n<i>The old poet walks into the garden.</i>\n") != std::string::npos);
  CHECK(out.find("6\n00:00:16,000 --> 00:00:18,400\n- Are you ready?\n- Yes, I am ready.\n") != std::string::npos);
  targets[3] = "Rōma, multīs abhinc annīs.";
  o.macrons = false;
  const std::string out2 = [&] {
    ExportResult r = buildExport(d, targets, sources, o).value();
    return std::string(r.bytes.begin(), r.bytes.end());
  }();
  CHECK(out2.find("{\\an8}Roma, multis abhinc annis.\n") != std::string::npos);
  o.format = subs::Format::Ass;
  const std::string ass = [&] {
    ExportResult r = buildExport(d, targets, sources, o).value();
    return std::string(r.bytes.begin(), r.bytes.end());
  }();
  CHECK(ass.find("Dialogue: 0,0:00:10.00,0:00:12.40,Default,,0,0,0,,Roma, multis abhinc annis.\n") != std::string::npos);
  subs::Document txt = subs::parse(std::vector<uint8_t>{'a', '\n'}, subs::Format::Txt).value();
  o.format = subs::Format::Srt;
  CHECK(buildExport(txt, {"b"}, {"a"}, o).error().code == vp::ErrorCode::UnsupportedFormat);
}

TEST_CASE("cli: CueView") {
  subs::Document d = parseSrt("1\n00:00:01,000 --> 00:00:02,000\nA rather long line that is read too fast here.\n");
  vp::CueRecord r;
  r.index = 0;
  CueViewCtx ctx;
  json v = cueViewJson(d.cues[0], d.cues[0].plainText(), r, ctx);
  CHECK(v["start"] == 1000);
  CHECK(v["durationMs"] == 1000);
  CHECK(v["index"] == 0);
  CHECK(v["confidence"] == "check");
  CHECK(v["flags"][0] == "cps");
  CHECK(v["lines"].empty());
  r.target = "Brevis.";
  r.confidence = "ok";
  v = cueViewJson(d.cues[0], d.cues[0].plainText(), r, ctx);
  CHECK(v["cps"] == 7.0);
  CHECK(v["flags"].empty());
  CHECK(v["lines"][0] == "Brevis.");
}

TEST_CASE("cli: stub engine") {
  std::unique_ptr<vp::rules::Engine> e = vp::rules::makeStubEngine();
  REQUIRE(e->setLexicons(nullptr, nullptr, nullptr, nullptr).ok());
  std::vector<vp::rules::CueInput> in(3);
  for (size_t i = 0; i < 3; ++i) {
    in[i].index = static_cast<uint32_t>(i + 1);
    in[i].sourceText = "cue number " + std::to_string(i + 1);
  }
  size_t seen = 0;
  auto r = e->translate(in, {}, {}, [&](size_t d) { seen = d; }, [] { return false; });
  REQUIRE(r.ok());
  REQUIRE(r.value().size() == 3);
  CHECK(seen == 3);
  CHECK(r.value()[2].target == "cue number 3");
  CHECK(r.value()[2].confidence == vp::rules::Confidence::Check);
  CHECK(r.value()[2].tokens.size() == 3);
  CHECK(r.value()[2].tokens[1].start == 4);
  CHECK(r.value()[2].reasons[0].text == "stub engine");
  int calls = 0;
  auto c = e->translate(in, {}, {}, nullptr, [&] { return ++calls > 1; });
  CHECK(c.value().size() == 1);
  CHECK(e->inspect("puella", vp::rules::Lang::La, {}).error().code == vp::ErrorCode::LexiconMissing);

  auto lx = vp::lex::Lexicon::open(std::filesystem::path(VP_FIXTURES_DIR) / "lex" / "latin.vpl");
  REQUIRE(lx.ok());
  REQUIRE(e->setLexicons(&lx.value(), nullptr, nullptr, nullptr).ok());
  auto ins = e->inspect("PUELLĀS", vp::rules::Lang::La, {});
  REQUIRE(ins.ok());
  REQUIRE(!ins.value().analyses.empty());
  CHECK(ins.value().analyses[0].head == "puella");
  CHECK(ins.value().analyses[0].features.case_ == "accusative");
  CHECK(ins.value().analyses[0].features.number == "plural");
  auto chk = e->check({}, std::string(60, 'x'), {}, {});
  REQUIRE(chk.ok());
  CHECK_FALSE(chk.value().checks[0].ok);
}
