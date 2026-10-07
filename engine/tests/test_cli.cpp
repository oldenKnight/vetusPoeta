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
  // interrogative ποῦ keeps its accent; the enclitic indefinite πού "somewhere" loses it (vp::grc::toMonotonic)
  CHECK(greekMonotonic("καὶ ποῦ; ἢ πῶς") == "και πού; ή πώς");
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

// C8: per-cue engine data kept in hidden reasons, and the ReasonView shapes the UI reads (DESIGN 9.2).
TEST_CASE("cli: stored tokens, flags, job facts and UI reason shapes") {
  using vp::rules::TokenView;
  CHECK(lemmaFlagNames(1u << 8) == std::vector<std::string>{"gloss-es-pivot"});
  auto lxr = vp::lex::Lexicon::open(std::filesystem::path(VP_FIXTURES_DIR) / "lex" / "latin.vpl");
  REQUIRE(lxr.ok());
  const vp::lex::Lexicon& lx = lxr.value();
  std::vector<vp::lex::Analysis> an;
  REQUIRE(lx.lookup("puellam", an));
  REQUIRE(!an.empty());
  const uint32_t puella = an[0].lemma;
  an.clear();
  REQUIRE(lx.lookup("amat", an));
  const uint32_t amo = an[0].lemma;
  an.clear();
  REQUIRE(lx.lookup("amor", an));
  uint32_t amor = vp::lex::kNoLemma;
  for (const auto& a : an)
    if (lx.lemma(a.lemma).pos == vp::feat::Noun) amor = a.lemma;
  REQUIRE(amor != vp::lex::kNoLemma);

  vp::rules::CueOutput out;
  out.target = "Puellam amat.";
  TokenView t0, t1;
  t0.text = t0.display = "Puellam";
  t0.start = 0;
  t0.end = 7;
  t0.lemmaId = puella;
  t0.hasLemma = true;
  t0.tier = 1;
  t0.emoji = "\xF0\x9F\x91\xA7";
  t0.features.pos = "noun";
  t0.features.case_ = "accusative";
  t0.features.number = "singular";
  t0.features.gender = "feminine";
  t1.text = t1.display = "amat";
  t1.start = 8;
  t1.end = 12;
  t1.lemmaId = amo;
  t1.hasLemma = true;
  t1.features.pos = "verb";
  t1.features.person = "third";
  t1.features.number = "singular";
  t1.features.tense = "present";
  t1.features.mood = "indicative";
  t1.features.voice = "active";
  out.tokens = {t0, t1};
  out.flags = {"emoji", "cps", "song"};
  out.reasons.push_back({0, "sense", "\"girl\" -> puella (score 0.900)", ""});
  out.reasons.push_back({1, "candidate", "candidates for \"loves\"",
                         "[{\"lemma\":" + std::to_string(amor) + ",\"head\":\"amor\",\"score\":0.2,\"why\":\"base 50\"},"
                         "{\"lemma\":" + std::to_string(amo) + ",\"head\":\"amō\",\"score\":0.9,\"why\":\"base 205\"}]"});
  out.reasons.push_back({1, "form", "order.decl", ""});
  out.reasons.push_back({-1, "evidence", "model: off", ""});
  out.reasons.push_back({-1, "evidence", "online: puella: Wiktionary: girl", "1"});
  out.reasons.push_back({0, "phrasebook", "x", "{\"pattern\":\"p\",\"latin\":\"l\",\"tier\":1}"});

  vp::CueRecord rec;
  applyOutput(out, rec);
  JobFacts jf;
  jf.online = true;
  jf.onlineVerdicts = {{std::string(lx.lemma(puella).head), 1}};
  setHidden(rec, kJobKind, encodeJob(jf));
  // hidden data: tokens round-trip, flags without the recomputed ones, job facts
  REQUIRE(hiddenReason(rec, kTokensKind) != nullptr);
  std::vector<TokenView> back;
  REQUIRE(decodeTokens(hiddenReason(rec, kTokensKind)->data, back));
  REQUIRE(back.size() == 2);
  CHECK(back[0].text == "Puellam");
  CHECK(back[0].lemmaId == puella);
  CHECK(back[0].emoji == t0.emoji);
  CHECK(back[0].features.case_ == "accusative");
  CHECK(back[1].features.tense == "present");
  CHECK(back[1].features.mood == "indicative");
  CHECK(storedFlags(rec) == std::vector<std::string>{"emoji", "song"});
  JobFacts jb;
  REQUIRE(decodeJob(hiddenReason(rec, kJobKind)->data, jb));
  CHECK(jb.online);
  CHECK_FALSE(jb.model);
  CHECK(jb.onlineVerdicts.size() == 1);
  uint32_t packed = 0;
  CHECK(featuresToPacked(back[0].features, packed));
  CHECK(featuresFromPacked(packed).case_ == "accusative");

  // the view never shows hidden reasons; the cue view carries the stored flags
  ReasonViewCtx ctx;
  ctx.lex = &lx;
  ctx.source = "The girl loves love.";
  const json rv = reasonsView(rec, back, ctx);
  int sense = 0, cands = 0, chosen = 0, forms = 0, evidence = 0, onlineYes = 0, modelOff = 0;
  for (const json& r : rv) {
    const std::string k = r["kind"];
    CHECK(k[0] != '_');
    if (k == "sense") {
      ++sense;
      CHECK(r["data"]["source"] == "girl");
      CHECK(r["data"]["sense"] == "girl");
      CHECK(r["data"]["context"] == json::array({"The", "loves", "love"}));
    } else if (k == "candidate") {
      ++cands;
      for (const char* key : {"lemmaId", "head", "form", "tier", "band", "chosen", "gloss"}) CHECK(r["data"].contains(key));
      if (r["data"]["chosen"] == true) {
        ++chosen;
        CHECK(r["data"]["lemmaId"] == amo);
        CHECK(r["data"]["form"] == "amat");
        CHECK(cands == 1);   // the chosen candidate comes first
      }
    } else if (k == "form") {
      ++forms;
      CHECK(r["data"]["features"]["person"] == "third");
    } else if (k == "evidence" && r["tokenIndex"] >= 0) {
      ++evidence;
      if (r["data"]["source"] == "online" && r["data"]["state"] == "yes") ++onlineYes;
      if (r["data"]["source"] == "model" && r["data"]["state"] == "off") ++modelOff;
    } else if (k == "evidence") {
      CHECK((r["data"]["source"] == "model" || r["data"]["source"] == "online"));
    } else if (k == "phrasebook") {
      CHECK(r["data"]["latin"] == "l");
    }
  }
  CHECK(sense == 1);
  CHECK(cands == 2);
  CHECK(chosen == 1);
  CHECK(forms == 1);
  CHECK(evidence == 8);   // four sources x two dictionary tokens
  CHECK(onlineYes == 1);  // the verdict was for puella only
  CHECK(modelOff == 2);
  subs::Document d = parseSrt("1\n00:00:01,000 --> 00:00:03,000\nThe girl loves love.\n");
  CueViewCtx vctx;
  const json v = cueViewJson(d.cues[0], d.cues[0].plainText(), rec, vctx);
  CHECK(v["flags"] == json::array({"emoji", "song"}));

  // after an edit the reasons follow their words
  TokenView n0 = t1, n1 = t0;
  n0.start = 0;
  n1.start = 5;
  const std::vector<vp::CueReason> moved = remapReasons(rec.reasons, back, {n0, n1});
  int onAmat = 0;
  for (const vp::CueReason& r : moved)
    if (r.kind == "candidate") onAmat += r.tokenIndex == 0;
  CHECK(onAmat == 1);
  CHECK(remapReasons(rec.reasons, back, {}).size() == 5);   // two sentence-level and three hidden ones stay

  // Orbergise helpers
  const std::vector<WordChange> ch = wordChanges("Puellam amat.", {t0, [] {
                                                   TokenView x;
                                                   x.text = "dīligit";
                                                   return x;
                                                 }()});
  REQUIRE(ch.size() == 1);
  CHECK(ch[0].was == "amat");
  CHECK(ch[0].now == "dīligit");
  CHECK(ch[0].tokenIndex == 1);
  const Meaning m = meaningCheck(lx, "Puella amat.", "Puella dormit.");
  CHECK(m.percent == 50);
  REQUIRE(m.missing.size() == 1);
  CHECK(m.missing[0].rfind("am", 0) == 0);
}

TEST_CASE("cli: TokenView.register (C23): the chosen sense's register survives the stored tokens") {
  using vp::rules::TokenView;
  TokenView a, b, c;
  a.text = a.display = "Alchēmista";
  a.end = 11;
  a.lemmaId = 7;
  a.hasLemma = true;
  a.registerTag = "medieval";
  b.text = b.display = "hīc";
  b.start = 12;
  b.end = 16;
  c.text = c.display = "computātrum";
  c.registerTag = "new-latin";
  c.fromRule = true;
  // the view: "register" only when tagged (additive field, absent otherwise)
  CHECK(tokenJson(a)["register"] == "medieval");
  CHECK_FALSE(tokenJson(b).contains("register"));
  CHECK(tokenJson(c)["register"] == "new-latin");
  CHECK(tokenJson(c)["fromRule"] == true);
  // stored in the project (bits of the flags field) and read back
  std::vector<TokenView> back;
  REQUIRE(decodeTokens(encodeTokens({a, b, c}), back));
  REQUIRE(back.size() == 3);
  CHECK(back[0].registerTag == "medieval");
  CHECK(back[1].registerTag.empty());
  CHECK(back[2].registerTag == "new-latin");
  CHECK(back[2].fromRule);
  CHECK_FALSE(back[2].unknown);
  // a project written before C23 (bits 0..3 only) has no register
  REQUIRE(decodeTokens("[[\"hīc\",0,0,3,-1,0,2,\"\",\"\"]]", back));
  REQUIRE(back.size() == 1);
  CHECK(back[0].registerTag.empty());
  CHECK(back[0].fromRule);
}

TEST_CASE("cli: Orbergise original alignment, language detection, stored facts, monotonic export") {
  auto t = [](const char* s, int64_t a, int64_t b) {
    TimedText x;
    x.text = s;
    x.start = a;
    x.end = b;
    x.timed = true;
    return x;
  };
  const std::vector<TimedText> cues = {t("Puella rosam videt.", 1000, 3500), t("Nauta habitat.", 4000, 6500),
                                       t("Agricola aquam portat.", 7000, 9500)};
  // 1. same number of cues: by index, whatever the timing says
  std::vector<TimedText> same = {t("A", 0, 10), t("B", 20, 30), t("C", 40, 50)};
  CHECK(alignOriginal(cues, same) == std::vector<std::string>{"A", "B", "C"});
  // untimed original (a .txt): by index, missing ones empty
  std::vector<TimedText> txt(2);
  txt[0].text = "one";
  txt[1].text = "two";
  CHECK(alignOriginal(cues, txt) == std::vector<std::string>{"one", "two", ""});
  // 2. different counts: by time overlap (two halves of cue 1 joined; cue 2 gets the best partial overlap)
  std::vector<TimedText> split = {t("The girl sees the rose.", 1000, 3400), t("The sailor", 4000, 5200),
                                  t("lives there.", 5200, 6500), t("The farmer", 9300, 12000)};
  CHECK(alignOriginal(cues, split) ==
        std::vector<std::string>{"The girl sees the rose.", "The sailor lives there.", "The farmer"});
  std::vector<TimedText> far = {t("x", 50000, 51000), t("y", 52000, 53000)};
  CHECK(alignOriginal(cues, far) == std::vector<std::string>{"", "", ""});
  CHECK(alignOriginal(cues, {}) == std::vector<std::string>{"", "", ""});

  CHECK(detectOriginalLang({"The girl sees the rose.", "The sailor lives on the island."}) == "en");
  CHECK(detectOriginalLang({"La niña ve la rosa.", "El marinero vive en la isla."}) == "es");
  CHECK(detectOriginalLang({"¿Dónde está?"}) == "es");
  CHECK(detectOriginalLang({}) == "en");
  CHECK(detectOriginalLang({"OK"}) == "en");

  OrbergFacts f;
  CHECK(encodeOrberg(f).empty());
  f.percent = 67;
  f.missing = {"urbs"};
  f.original = "The city is taken.";
  OrbergFacts g;
  REQUIRE(decodeOrberg(encodeOrberg(f), g));
  CHECK(g.percent == 67);
  CHECK(g.missing == std::vector<std::string>{"urbs"});
  CHECK(g.original == "The city is taken.");
  CHECK_FALSE(decodeOrberg("[1]", g));

  // applyOutput keeps the engine's meaning facts hidden; the stub path (meaningPercent -1, no original) keeps none
  vp::rules::CueOutput out;
  out.target = "Puella rosam videt.";
  out.meaningPercent = 100;
  out.original = "The girl sees the rose.";
  out.reasons.push_back(vp::rules::Reason{0, "orbergise", "spectat -> videt (vocabulary)",
                                          "{\"was\":\"spectat\",\"now\":\"videt\",\"why\":\"vocabulary\"}"});
  vp::CueRecord r;
  applyOutput(out, r);
  const vp::CueReason* h = hiddenReason(r, kOrbergKind);
  REQUIRE(h != nullptr);
  REQUIRE(decodeOrberg(h->data, g));
  CHECK(g.percent == 100);
  CHECK(g.original == "The girl sees the rose.");
  vp::CueRecord r2;
  applyOutput(vp::rules::CueOutput(), r2);
  CHECK(hiddenReason(r2, kOrbergKind) == nullptr);

  // export greek:"monotonic" on a Greek target: no breathing, circumflex, grave or iota subscript left
  const std::string mono = greekMonotonic("Ἡ κόρη τὸ ῥόδον ὁρᾷ. Ὁ ναύτης ἐν τῇ νήσῳ οἰκεῖ.");
  CHECK(mono == "Η κόρη το ρόδον ορά. Ο ναύτης εν τη νήσω οικεί.");
  ExportOptions eo;
  eo.monotonic = true;
  CHECK(exportText("Ὁ γεωργὸς ὕδωρ φέρει.", "The farmer carries water.", eo) == "Ο γεωργός ύδωρ φέρει.");
}
