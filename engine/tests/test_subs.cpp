// Tests for vp_subs (DESIGN.md §7): byte-exact round trip, encodings, spans, warnings, line breaking, reading speed.
#include <doctest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <vector>

#include "vp/subs.h"

namespace fs = std::filesystem;
using namespace vp::subs;

namespace {

std::vector<uint8_t> readBytes(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

std::vector<uint8_t> bytesOf(const std::string& s) { return std::vector<uint8_t>(s.begin(), s.end()); }
std::string strOf(const std::vector<uint8_t>& b) { return std::string(b.begin(), b.end()); }

fs::path fixtureDir() { return fs::path(VP_FIXTURES_DIR) / "subs"; }

Format formatFor(const fs::path& p) {
  std::string e = p.extension().string();
  if (e == ".vtt") return Format::Vtt;
  if (e == ".ass" || e == ".ssa") return Format::Ass;
  if (e == ".txt") return Format::Txt;
  return Format::Srt;
}

std::vector<fs::path> fixtures() {
  std::vector<fs::path> out;
  for (const auto& e : fs::directory_iterator(fixtureDir()))
    if (e.is_regular_file() && e.path().filename().string()[0] != '.') out.push_back(e.path());  // skip .gitattributes
  std::sort(out.begin(), out.end());
  return out;
}

Document load(const std::string& name) {
  auto r = parse(readBytes(fixtureDir() / name), formatFor(name));
  REQUIRE(r.ok());
  return r.value();
}

bool hasWarning(const Document& d, const std::string& kind, uint32_t index = 0) {
  for (const Warning& w : d.warnings)
    if (w.kind == kind && (index == 0 || w.index == index)) return true;
  return false;
}

std::vector<uint8_t> writeOrFail(const Document& d, const WriteOptions& o = WriteOptions{}) {
  auto r = write(d, o);
  REQUIRE(r.ok());
  return r.value();
}

std::string collapse(const std::string& s) {
  std::string out;
  bool sp = false;
  for (char c : s) {
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { sp = !out.empty(); continue; }
    if (sp) out.push_back(' ');
    sp = false;
    out.push_back(c);
  }
  return out;
}

std::string stripTags(const std::string& s) {
  std::string out;
  for (std::size_t i = 0; i < s.size(); ++i) {
    char close = s[i] == '<' ? '>' : s[i] == '{' ? '}' : 0;
    std::size_t j = close ? s.find(close, i) : std::string::npos;
    if (j != std::string::npos) { i = j; continue; }
    out.push_back(s[i]);
  }
  return out;
}

std::vector<std::string> tagsOf(const Document& d) {
  std::vector<std::string> t;
  for (const Cue& c : d.cues)
    for (const Span& s : c.spans)
      if (s.kind == Span::Tag) t.push_back(s.raw);
  return t;
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------------
TEST_CASE("subs: fixture set covers the required formats") {
  int srt = 0, vtt = 0, ass = 0;
  for (const auto& p : fixtures()) {
    Format f = formatFor(p);
    srt += f == Format::Srt;
    vtt += f == Format::Vtt;
    ass += f == Format::Ass;
  }
  CHECK(srt >= 12);
  CHECK(vtt >= 6);
  CHECK(ass >= 6);
}

TEST_CASE("subs: write(parse(x)) is byte-identical for every fixture") {
  for (const auto& p : fixtures()) {
    CAPTURE(p.filename().string());
    std::vector<uint8_t> in = readBytes(p);
    auto r = parse(in, formatFor(p));
    REQUIRE(r.ok());
    CHECK(!r->cues.empty());
    auto out = write(r.value());
    REQUIRE(out.ok());
    CHECK(out.value() == in);
    bool malformed = p.filename().string().rfind("malformed_", 0) == 0;
    if (!malformed) {
      for (const Warning& w : r->warnings) {
        CAPTURE(w.kind);
        CAPTURE(w.detail);
        CHECK((w.kind == "encoding_fallback"));
      }
    }
  }
}

TEST_CASE("subs: encodings, BOM and newline detection") {
  Document d = load("srt_utf16le.srt");
  CHECK(d.encoding == "utf-16le");
  CHECK(d.bom);
  CHECK(d.newline == "\r\n");
  CHECK(d.cues[1].plainText() == "Servus librōs in mēnsā pōnit.");

  d = load("srt_utf16be.srt");
  CHECK(d.encoding == "utf-16be");
  CHECK(d.newline == "\n");
  CHECK(d.cues[1].plainText() == "ὁ ναύτης τὴν ναῦν ὁρᾷ.");

  d = load("srt_cp1252.srt");
  CHECK(d.encoding == "windows-1252");
  CHECK_FALSE(d.bom);
  CHECK(hasWarning(d, "encoding_fallback"));
  CHECK(d.cues[0].plainText() == "El café está frío, pero la señora sonríe.");
  CHECK(d.cues[1].plainText() == "“¿Dónde está el niño?” — preguntó.");
  CHECK(d.cues[2].plainText() == "It’s raining… again.");

  d = load("srt_bom_utf8.srt");
  CHECK(d.encoding == "utf-8");
  CHECK(d.bom);
  CHECK(d.cues[0].idRaw == "1");  // BOM is not part of the first line

  d = load("srt_basic_lf.srt");
  CHECK(d.encoding == "utf-8");
  CHECK_FALSE(d.bom);
  CHECK(d.newline == "\n");
}

TEST_CASE("subs: write options re-encode") {
  Document d = load("srt_cp1252.srt");
  WriteOptions o;
  o.encoding = "utf-8";
  std::string utf8 = strOf(writeOrFail(d, o));
  CHECK(utf8.find("señora") != std::string::npos);
  CHECK(utf8.find("“¿Dónde") != std::string::npos);
  auto back = parse(bytesOf(utf8), Format::Srt);
  REQUIRE(back.ok());
  CHECK(back->encoding == "utf-8");
  CHECK(back->cues[1].plainText() == d.cues[1].plainText());

  o.bom = true;
  auto withBom = writeOrFail(d, o);
  REQUIRE(withBom.size() > 3);
  CHECK(withBom[0] == 0xEF);
  CHECK(withBom[1] == 0xBB);
  CHECK(withBom[2] == 0xBF);

  Document g = load("srt_greek_polytonic.srt");
  WriteOptions toLe;
  toLe.encoding = "utf-16le";
  toLe.bom = true;
  auto le = writeOrFail(g, toLe);
  auto g2 = parse(le, Format::Srt);
  REQUIRE(g2.ok());
  CHECK(g2->encoding == "utf-16le");
  CHECK(strOf(writeOrFail(g2.value(), WriteOptions{})) == strOf(le));
  WriteOptions backToUtf8;
  backToUtf8.encoding = "utf-8";
  backToUtf8.bom = false;
  CHECK(writeOrFail(g2.value(), backToUtf8) == readBytes(fixtureDir() / "srt_greek_polytonic.srt"));

  WriteOptions cp;
  cp.encoding = "windows-1252";
  std::vector<Warning> ws;
  auto r = write(g, cp, &ws);
  REQUIRE(r.ok());
  bool unmappable = false;
  for (const Warning& w : ws) unmappable |= w.kind == "unmappable_char";
  CHECK(unmappable);

  WriteOptions bad;
  bad.encoding = "ebcdic";
  auto e = write(g, bad);
  CHECK_FALSE(e.ok());
  CHECK(e.error().code == vp::ErrorCode::BadParams);
}

TEST_CASE("subs: SRT structure and spans") {
  Document d = load("srt_basic_crlf.srt");
  REQUIRE(d.cues.size() == 4);
  CHECK(d.format == Format::Srt);
  CHECK(d.cues[1].index == 2);
  CHECK(d.cues[1].idRaw == "2");
  CHECK(d.cues[1].timingRaw == "00:00:04,800 --> 00:00:07,300");
  REQUIRE(d.cues[1].spans.size() == 3);
  CHECK(d.cues[1].spans[1].kind == Span::Newline);
  CHECK(d.cues[1].spans[1].raw == "\r\n");
  CHECK(d.cues[1].plainText() == "Each tin has a label written in green ink.");

  d = load("srt_italics_tags.srt");
  REQUIRE(d.cues.size() == 4);
  const auto& sp = d.cues[0].spans;
  REQUIRE(sp.size() == 3);
  CHECK(sp[0].kind == Span::Tag);
  CHECK(sp[0].raw == "<i>");
  CHECK(sp[1].raw == "Somewhere a radio is playing.");
  CHECK(sp[2].raw == "</i>");
  CHECK(d.cues[2].spans[0].raw == "<font color=\"#ffcc00\">");
  CHECK(d.cues[3].plainText() == "Wait, I said, not yet.");

  d = load("srt_an8_position.srt");
  CHECK(d.cues[0].spans[0].kind == Span::Tag);
  CHECK(d.cues[0].spans[0].raw == "{\\an8}");
  CHECK(d.cues[2].plainText() == "Closed for repairs");

  d = load("srt_no_trailing_newline.srt");
  CHECK(d.cues.back().textEol.empty());
  CHECK(d.trailerRaw.empty());

  d = load("srt_extra_blank_lines.srt");
  CHECK(d.headerRaw == "\n\n");
  CHECK(d.cues[0].tailRaw == "\n\n\n");
  CHECK(d.trailerRaw == "\n\n");

  d = load("srt_entities.srt");
  CHECK(d.cues[0].plainText() == "Salt & pepper, <please>.");
  CHECK(d.cues[1].plainText() == "Ten o'clock sharp.");

  d = load("srt_music_notes.srt");
  CHECK(d.cues[0].plainText() == "♪ The wind is soft upon the hill ♪");
}

TEST_CASE("subs: VTT header, identifiers, settings, blocks and tags") {
  Document d = load("vtt_identifiers_settings.vtt");
  REQUIRE(d.cues.size() == 3);
  CHECK(d.headerRaw == "WEBVTT - with identifiers\n\n");
  CHECK(d.cues[0].idRaw == "intro");
  CHECK(d.cues[0].timingRaw == "00:00:01.000 --> 00:00:03.000 align:start position:10%");
  CHECK(d.cues[2].idRaw == "closing-remark");

  d = load("vtt_note_style_region.vtt");
  REQUIRE(d.cues.size() == 2);
  CHECK(d.headerRaw.find("STYLE\n::cue {") != std::string::npos);
  CHECK(d.headerRaw.find("REGION\nid:top") != std::string::npos);
  CHECK(d.headerRaw.find("NOTE This file was written for the tests.") != std::string::npos);
  CHECK(d.cues[0].idRaw.empty());
  CHECK(d.cues[0].timingRaw == "00:01.000 --> 00:04.000 region:top");
  CHECK(d.cues[0].tailRaw == "\nNOTE a note between cues\n\n");

  d = load("vtt_voice_class_tags.vtt");
  REQUIRE(d.cues.size() == 5);
  CHECK(d.cues[0].spans[0].raw == "<v Marcus>");
  CHECK(d.cues[0].plainText() == "Salvē, amīce!");
  CHECK(d.cues[1].spans[0].raw == "<v.loud Julia>");
  CHECK(d.cues[2].plainText() == "The lamp is under the table, surely.");
  CHECK(d.cues[3].spans[0].raw == "<ruby>");
  CHECK(d.cues[4].plainText() == "One word at a time.");
  std::vector<std::string> tags = tagsOf(d);
  CHECK(std::count(tags.begin(), tags.end(), "<00:00:14.000>") == 1);

  d = load("vtt_crlf_bom.vtt");
  CHECK(d.bom);
  CHECK(d.newline == "\r\n");
  CHECK(d.headerRaw == "WEBVTT\r\n\r\n");
}

TEST_CASE("subs: ASS fields, overrides, newlines and comments") {
  Document d = load("ass_overrides.ass");
  REQUIRE(d.cues.size() == 5);
  CHECK(d.headerRaw.find("[V4+ Styles]") != std::string::npos);
  CHECK(d.headerRaw.size() > 0);
  CHECK(d.cues[0].styleRaw == "Dialogue: 0,0:00:01.00,0:00:04.00,Top,,0,0,0,,");
  CHECK(d.cues[0].timingRaw == "0:00:01.00,0:00:04.00");
  CHECK(d.cues[0].spans[0].kind == Span::Tag);
  CHECK(d.cues[0].spans[0].raw == "{\\an8}");
  const auto& sp = d.cues[1].spans;
  REQUIRE(sp.size() == 6);
  CHECK(sp[0].raw == "{\\i1}");
  CHECK(sp[4].kind == Span::Newline);
  CHECK(sp[4].raw == "\\N");
  CHECK(d.cues[1].plainText() == "Somebody forgot to wind it, or nobody cared.");
  CHECK(d.cues[2].spans[0].raw == "{\\pos(960,200)\\fad(200,200)}");
  CHECK(d.cues[2].spans[2].raw == "\\n");
  CHECK(d.cues[3].plainText().find("a comma, inside") != std::string::npos);
  CHECK(d.cues[4].spans[0].kind == Span::Tag);

  d = load("ass_comments.ass");
  REQUIRE(d.cues.size() == 3);
  CHECK(d.headerRaw.find("Comment: 0,0:00:00.00") != std::string::npos);
  CHECK(d.cues[0].tailRaw == "Comment: 0,0:00:03.00,0:00:03.00,Default,,0,0,0,,timing checked by hand\n\n");
  CHECK(d.trailerRaw == "\n[Fonts]\n");
  CHECK(d.cues[1].styleRaw == "Dialogue: 0,0:00:03.50,0:00:06.00,Default,Ben,0,0,0,,");

  d = load("ass_ssa_v4.ssa");
  REQUIRE(d.cues.size() == 2);
  CHECK(d.cues[0].timingRaw == "0:00:01.00,0:00:03.00");
  CHECK(d.cues[1].plainText() == "Mice keep very still until the morning.");

  d = load("ass_greek_latin.ass");
  REQUIRE(d.cues.size() == 3);
  CHECK(d.cues[0].styleRaw == "Dialogue:0,0:00:01.00,0:00:03.00,Default,,0,0,0,,");
  CHECK(d.cues[2].plainText() == "Discipulī gaudent, οἱ μαθηταὶ χαίρουσιν.");
  CHECK(d.cues[2].textEol.empty());
}

TEST_CASE("subs: TXT paragraphs") {
  Document d = load("txt_paragraphs.txt");
  REQUIRE(d.cues.size() == 3);
  CHECK(d.cues[0].timingRaw.empty());
  CHECK(d.cues[0].plainText() == "The village has one well and two bridges. Children cross the small bridge to school.");
  CHECK(d.cues[1].tailRaw == "\n\n");
  CHECK(d.cues[2].plainText() == "ὁ γεωργὸς ἐν τῷ ἀγρῷ πονεῖ.");
  d = load("txt_crlf_no_trailing.txt");
  REQUIRE(d.cues.size() == 2);
  CHECK(d.newline == "\r\n");
}

TEST_CASE("subs: malformed files parse with warnings and keep their bytes") {
  Document d = load("malformed_srt_numbering.srt");
  REQUIRE(d.cues.size() == 4);
  CHECK(hasWarning(d, "numbering_gap", 2));
  CHECK(hasWarning(d, "numbering_duplicate", 3));
  CHECK(hasWarning(d, "numbering_order", 4));
  CHECK(d.cues[3].idRaw == "2");

  d = load("malformed_srt_overlap.srt");
  REQUIRE(d.cues.size() == 3);
  CHECK(hasWarning(d, "overlap", 2));
  CHECK(hasWarning(d, "reversed_time", 3));

  d = load("malformed_srt_mixed.srt");
  REQUIRE(d.cues.size() == 5);
  CHECK(hasWarning(d, "timing_format", 1));   // dots instead of commas
  CHECK(hasWarning(d, "missing_blank_line", 1));
  CHECK(hasWarning(d, "timing_format", 2));   // extra spaces
  CHECK(d.cues[1].timingRaw == "00:00:03,500  -->  00:00:05,000");
  CHECK(hasWarning(d, "empty_text", 3));
  CHECK(d.cues[2].spans.empty());
  CHECK(hasWarning(d, "stray_text", 4));
  CHECK(d.cues[4].idRaw == " 5 ");
  CHECK(hasWarning(d, "bad_id", 5));
  CHECK(d.cues[0].plainText() == "Dots instead of commas.");
  int64_t a = 0, b = 0;
  CHECK(parseTiming(d.cues[0].timingRaw, Format::Srt, a, b));
  CHECK(a == 1000);
  CHECK(b == 3000);

  // No timing at all: everything is kept as header.
  auto r = parse(bytesOf("just some words\nand more\n"), Format::Srt);
  REQUIRE(r.ok());
  CHECK(r->cues.empty());
  CHECK(hasWarning(r.value(), "no_cues"));
  CHECK(strOf(writeOrFail(r.value())) == "just some words\nand more\n");

  // Content sniffing overrides a wrong hint, with a warning.
  r = parse(readBytes(fixtureDir() / "vtt_basic.vtt"), Format::Srt);
  REQUIRE(r.ok());
  CHECK(r->format == Format::Vtt);
  CHECK(hasWarning(r.value(), "format_mismatch"));

  // Old Mac line breaks (CR only) are line breaks too, and round-trip.
  const std::string cr = "1\r00:00:01,000 --> 00:00:02,000\rHi there.\r\r2\r00:00:03,000 --> 00:00:04,000\rBye.\r";
  r = parse(bytesOf(cr), Format::Srt);
  REQUIRE(r.ok());
  REQUIRE(r->cues.size() == 2);
  CHECK(r->cues[1].plainText() == "Bye.");
  CHECK(strOf(writeOrFail(r.value())) == cr);

  // Broken UTF-16 (odd byte count) is the documented round-trip exception: repaired and reported.
  r = parse({0xFF, 0xFE, '1', 0, '\n', 0, 'x'}, Format::Srt);
  REQUIRE(r.ok());
  CHECK(r->encoding == "utf-16le");
  CHECK(hasWarning(r.value(), "encoding_lossy"));
  CHECK(writeOrFail(r.value()) == std::vector<uint8_t>{0xFF, 0xFE, '1', 0, '\n', 0});

  r = parse({}, Format::Srt);
  REQUIRE(r.ok());
  CHECK(r->cues.empty());
  CHECK(writeOrFail(r.value()).empty());
}

TEST_CASE("subs: non-text bytes are untouched when text changes") {
  for (const auto& p : fixtures()) {
    CAPTURE(p.filename().string());
    std::vector<uint8_t> in = readBytes(p);
    auto r = parse(in, formatFor(p));
    REQUIRE(r.ok());
    Document d = r.value();
    std::vector<std::vector<Span>> saved;
    for (Cue& c : d.cues) {
      saved.push_back(c.spans);
      for (Span& s : c.spans)
        if (s.kind == Span::Text) s.raw = "Lorem ipsum translated text";
    }
    auto changed = write(d, WriteOptions{});
    REQUIRE(changed.ok());
    auto reparsed = parse(changed.value(), formatFor(p));
    REQUIRE(reparsed.ok());
    REQUIRE(reparsed->cues.size() == r->cues.size());
    for (std::size_t i = 0; i < d.cues.size(); ++i) {
      CHECK(reparsed->cues[i].idRaw == r->cues[i].idRaw);
      CHECK(reparsed->cues[i].timingRaw == r->cues[i].timingRaw);
      CHECK(reparsed->cues[i].styleRaw == r->cues[i].styleRaw);
    }
    CHECK(reparsed->headerRaw == r->headerRaw);
    CHECK(tagsOf(reparsed.value()) == tagsOf(r.value()));
    // Restoring the original text spans restores the original bytes exactly.
    for (std::size_t i = 0; i < d.cues.size(); ++i) d.cues[i].spans = saved[i];
    CHECK(writeOrFail(d) == in);
  }
}

TEST_CASE("subs: changed cues are re-broken, unchanged cues are not") {
  Document d = load("srt_basic_lf.srt");
  d.cues[0].spans = {Span{Span::Tag, "<i>"},
                     Span{Span::Text, "Custos phari cotidie vesperi naves numerat et lucernam accendit."},
                     Span{Span::Tag, "</i>"}};
  WriteOptions o;
  o.hints.breakBefore = {"et", "sed"};
  std::string out = strOf(writeOrFail(d, o));
  CHECK(out.find("1\n00:00:01,000 --> 00:00:03,200\n<i>Custos phari cotidie vesperi naves\nnumerat et lucernam accendit.</i>\n\n2\n") == 0);
  CHECK(out.find("Tonight only three came home,\nand the fog is getting thicker.") != std::string::npos);
  o.rebreak = false;
  out = strOf(writeOrFail(d, o));
  CHECK(out.find("<i>Custos phari cotidie vesperi naves numerat et lucernam accendit.</i>\n\n2\n") != std::string::npos);

  Document a = load("ass_basic.ass");
  a.cues[2].spans = splitSpans("We built a path to the woodshed, one shovel at a time, all morning long.", Format::Ass);
  std::string ass = strOf(writeOrFail(a, WriteOptions{}));
  CHECK(ass.find(",,We built a path to the woodshed,\\None shovel at a time, all morning long.\n") != std::string::npos);

  // A cue that was empty in the source gains a line break before its blank line.
  Document m = load("malformed_srt_mixed.srt");
  m.cues[2].spans = {Span{Span::Text, "Now I have words."}};
  std::string ms = strOf(writeOrFail(m));
  CHECK(ms.find("3\n00:00:05,500 --> 00:00:07,000\nNow I have words.\n\n4\n") != std::string::npos);

  // New cues (not from the source) are written with the document newline.
  Document n;
  n.newline = "\r\n";
  Cue c;
  c.idRaw = "1";
  c.timingRaw = "00:00:01,000 --> 00:00:02,000";
  c.spans = {Span{Span::Text, "Hello."}};
  n.cues.push_back(c);
  c.idRaw = "2";
  n.cues.push_back(c);
  CHECK(strOf(writeOrFail(n)) ==
        "1\r\n00:00:01,000 --> 00:00:02,000\r\nHello.\r\n\r\n2\r\n00:00:01,000 --> 00:00:02,000\r\nHello.\r\n");
}

TEST_CASE("subs: plainText") {
  Cue c;
  c.spans = {Span{Span::Tag, "<i>"}, Span{Span::Text, "  Salt &amp; "}, Span{Span::Newline, "\n"},
             Span{Span::Text, "pepper&nbsp;&lt;3&gt;\t "}, Span{Span::Tag, "</i>"}, Span{Span::Text, "&quot;"}};
  CHECK(c.plainText() == "Salt & pepper <3> &quot;");
  Cue empty;
  CHECK(empty.plainText().empty());
  Cue tagsOnly;
  tagsOnly.spans = {Span{Span::Tag, "{\\an8}"}, Span{Span::Newline, ""}};
  CHECK(tagsOnly.plainText().empty());
}

TEST_CASE("subs: splitSpans per format") {
  auto s = splitSpans("{\\an8}<i>Hi</i>\nthere", Format::Srt);
  REQUIRE(s.size() == 6);
  CHECK(s[0].raw == "{\\an8}");
  CHECK(s[4].kind == Span::Newline);
  CHECK(s[4].raw.empty());
  s = splitSpans("{comment}a\\Nb <i>c</i>", Format::Ass);
  REQUIRE(s.size() == 4);
  CHECK(s[0].kind == Span::Tag);
  CHECK(s[2].raw == "\\N");
  CHECK(s[3].raw == "b <i>c</i>");  // angle brackets are text in ASS
  s = splitSpans("a <b> {c}", Format::Txt);
  REQUIRE(s.size() == 1);
  s = splitSpans("I <3 you, 2 < 3 and {not a tag}", Format::Srt);
  REQUIRE(s.size() == 1);
  CHECK(s[0].kind == Span::Text);
}

// ---------------------------------------------------------------------------------------------------------------
namespace {
struct BreakCase {
  const char* text;
  int maxLine, maxLines;
  std::vector<std::string> expect;
  bool overflow;
};
}  // namespace

TEST_CASE("subs: breakLines table") {
  BreakHints hints;
  hints.breakBefore = {"and", "but", "or", "because", "to", "with", "in", "at", "et", "sed", "ad", "cum", "quod",
                       "y", "pero", "en", "καὶ", "ἀλλὰ"};
  const std::string decomposedMarcus = "Ma\xCC\x84rcus et Iu\xCC\x84lia in horto\xCC\x84 ambulant cum cane";
  const std::vector<BreakCase> cases = {
      {"Short line.", 42, 2, {"Short line."}, false},
      {"", 42, 2, {}, false},
      {"   \t ", 42, 2, {}, false},
      {"abcdefghij abcdefghij abcdefghij abcdefghi", 42, 2, {"abcdefghij abcdefghij abcdefghij abcdefghi"}, false},
      {"abcdefghij abcdefghij abcdefghij abcdefghij", 42, 2, {"abcdefghij abcdefghij", "abcdefghij abcdefghij"}, false},
      {"I waited for you at the station, but the train never came.", 42, 2,
       {"I waited for you at the station,", "but the train never came."}, false},
      {"- Where are you going? - To the market.", 42, 2, {"- Where are you going?", "- To the market."}, false},
      {"-Did you bring it? -It is by the door.", 42, 2, {"-Did you bring it?", "-It is by the door."}, false},
      {"– Quō vādis? – Ad forum.", 42, 2, {"– Quō vādis?", "– Ad forum."}, false},
      {"<i>I waited for you at the station, but the train never came.</i>", 42, 2,
       {"<i>I waited for you at the station,", "but the train never came.</i>"}, false},
      {"{\\an8}The station clock has stopped working since the storm.", 42, 2,
       {"{\\an8}The station clock has stopped", "working since the storm."}, false},
      {"<font color=\"#ffcc00\">The kettle whistles</font> and then the whole house wakes up.", 42, 2,
       {"<font color=\"#ffcc00\">The kettle whistles</font> and then", "the whole house wakes up."}, false},
      {"Agricola equum ad flūmen dūcit et puella rosās in hortō spectat.", 42, 2,
       {"Agricola equum ad flūmen dūcit", "et puella rosās in hortō spectat."}, false},
      {"Mārcus et Iūlia in hortō ambulant cum cane", 42, 2, {"Mārcus et Iūlia in hortō ambulant cum cane"}, false},
      {decomposedMarcus.c_str(), 42, 2, {decomposedMarcus}, false},
      {"ὁ παῖς τὸν ἵππον πρὸς τὸν ποταμὸν ἄγει καὶ ἡ μήτηρ ὁρᾷ.", 42, 2,
       {"ὁ παῖς τὸν ἵππον πρὸς τὸν ποταμὸν", "ἄγει καὶ ἡ μήτηρ ὁρᾷ."}, false},
      {"ὁ διδάσκαλος λέγει, ἀλλὰ οἱ μαθηταὶ οὐκ ἀκούουσιν.", 42, 2,
       {"ὁ διδάσκαλος λέγει,", "ἀλλὰ οἱ μαθηταὶ οὐκ ἀκούουσιν."}, false},
      {"The old fisherman mends his nets every morning and sells his fish at the market before the bells ring for noon.",
       42, 2, {"The old fisherman mends his nets every morning and sells", "his fish at the market before the bells ring for noon."},
       true},
      {"Pneumonoultramicroscopicsilicovolcanoconiosis is long", 42, 2,
       {"Pneumonoultramicroscopicsilicovolcanoconiosis", "is long"}, true},
      {"The old fisherman mends his nets every morning and sells his fish at the market before noon.", 42, 3,
       {"The old fisherman mends his nets", "every morning and sells his fish", "at the market before noon."}, false},
      {"We missed the bus again, so we walked home.", 20, 3, {"We missed", "the bus again,", "so we walked home."}, false},
      {"♪ The wind is soft upon the hill and the sheep are counting stars ♪", 42, 2,
       {"♪ The wind is soft upon the hill", "and the sheep are counting stars ♪"}, false},
      {"We missed the bus. Now we walk home slowly.", 42, 2, {"We missed the bus.", "Now we walk home slowly."}, false},
      {"El café está frío pero la señora sonríe en la ventana.", 42, 2,
       {"El café está frío pero la señora", "sonríe en la ventana."}, false},
      {"<i> Somewhere a radio is playing, and nobody listens to it.</i>", 42, 2,
       {"<i> Somewhere a radio is playing,", "and nobody listens to it.</i>"}, false},
      {"One\ntwo   three", 42, 2, {"One two three"}, false},
      {"This sentence is a little too long for one single line here", 42, 1,
       {"This sentence is a little too long for one single line here"}, true},
      {"- Hi. - Hello.", 42, 1, {"- Hi. - Hello."}, false},
      {"\"Wait,\" she said, \"the bridge stays closed until the spring floods are gone.\"", 42, 2,
       {"\"Wait,\" she said, \"the bridge stays", "closed until the spring floods are gone.\""}, false},
      {"The lamp is under the table <i>surely</i> and the key is in the drawer.", 42, 2,
       {"The lamp is under the table <i>surely</i>", "and the key is in the drawer."}, false},
  };
  REQUIRE(cases.size() >= 30);
  for (const BreakCase& bc : cases) {
    std::string text = bc.text;
    CAPTURE(text);
    bool overflow = false;
    std::vector<std::string> got = breakLines(bc.text, hints, bc.maxLine, bc.maxLines, &overflow);
    CHECK(got == bc.expect);
    CHECK(overflow == bc.overflow);
    // Properties: nothing lost or split, line count and width respected unless overflow is reported.
    std::string joined;
    for (const auto& l : got) joined += (joined.empty() ? "" : " ") + l;
    CHECK(joined == collapse(bc.text));
    CHECK(static_cast<int>(got.size()) <= bc.maxLines);
    if (!overflow)
      for (const auto& l : got) CHECK(static_cast<int>(visibleLength(stripTags(l))) <= bc.maxLine);
  }
}

TEST_CASE("subs: visible length counts code points without combining marks") {
  CHECK(visibleLength("abc") == 3);
  CHECK(visibleLength("ā") == 1);
  CHECK(visibleLength("a\xCC\x84") == 1);
  CHECK(visibleLength("ὁρᾷ") == 3);
  CHECK(visibleLength("\xFF\xFE") == 2);  // invalid bytes count once each, never crash
}

TEST_CASE("subs: timing and reading speed") {
  int64_t a = 0, b = 0;
  CHECK(parseTiming("00:00:01,000 --> 00:00:03,500", Format::Srt, a, b));
  CHECK(a == 1000);
  CHECK(b == 3500);
  CHECK(parseTiming("01:02:03.004 --> 01:02:04.000 align:start", Format::Vtt, a, b));
  CHECK(a == 3723004);
  CHECK(b == 3724000);
  CHECK(parseTiming("00:01.500 --> 00:04.000", Format::Vtt, a, b));
  CHECK(a == 1500);
  CHECK(b == 4000);
  CHECK(parseTiming("0:00:01.25,0:00:03.00", Format::Ass, a, b));
  CHECK(a == 1250);
  CHECK(b == 3000);
  CHECK_FALSE(parseTiming("garbage", Format::Srt, a, b));
  CHECK_FALSE(parseTiming("00:00:01,000 --> soon", Format::Srt, a, b));
  CHECK_FALSE(parseTiming("00:00:61,000 --> 00:01:00,000", Format::Srt, a, b));
  CHECK_FALSE(parseTiming("", Format::Ass, a, b));
  CHECK_FALSE(parseTiming("00:00:01,000 --> 00:00:02,000", Format::Txt, a, b));

  Cue c;
  c.timingRaw = "00:00:01,000 --> 00:00:03,000";
  c.spans = {Span{Span::Tag, "<i>"}, Span{Span::Text, "Twenty characters!!"}, Span{Span::Tag, "</i>"}};
  CHECK(charsPerSecond(c) == doctest::Approx(9.5));  // 19 chars / 2 s
  c.spans = {Span{Span::Text, "Puella in hortō"}, Span{Span::Newline, "\n"}, Span{Span::Text, "ambulat."}};
  CHECK(charsPerSecond(c) == doctest::Approx(12.0));  // "Puella in hortō ambulat." = 24 / 2 s
  c.spans = {Span{Span::Text, "Puella in horto\xCC\x84 ambulat."}};
  CHECK(charsPerSecond(c) == doctest::Approx(12.0));
  c.timingRaw = "0:00:01.00,0:00:05.00";
  CHECK(charsPerSecond(c) == doctest::Approx(6.0));
  c.timingRaw = "00:00:05,000 --> 00:00:05,000";
  CHECK(charsPerSecond(c) == 0.0);
  c.timingRaw = "00:00:09,000 --> 00:00:07,000";
  CHECK(charsPerSecond(c) == 0.0);
  c.timingRaw = "not a timing";
  CHECK(charsPerSecond(c) == 0.0);

  Document d = load("ass_basic.ass");
  CHECK(charsPerSecond(d.cues[0]) == doctest::Approx(32.0 / 2.0));
}

TEST_CASE("subs: fuzzed inputs never crash and still round-trip") {
  std::vector<std::vector<uint8_t>> seeds;
  std::vector<Format> formats;
  for (const auto& p : fixtures()) {
    seeds.push_back(readBytes(p));
    formats.push_back(formatFor(p));
  }
  REQUIRE(!seeds.empty());
  std::mt19937 rng(20261006u);
  auto pick = [&](std::size_t n) { return n == 0 ? std::size_t{0} : static_cast<std::size_t>(rng() % n); };
  const Format all[] = {Format::Srt, Format::Vtt, Format::Ass, Format::Txt};
  int roundTrips = 0;
  for (int iter = 0; iter < 500; ++iter) {
    std::size_t s = pick(seeds.size());
    std::vector<uint8_t> b = seeds[s];
    int mutations = 1 + static_cast<int>(pick(3));
    for (int m = 0; m < mutations; ++m) {
      switch (pick(5)) {
        case 0: b.resize(pick(b.size() + 1)); break;  // truncate
        case 1: {                                       // insert random bytes
          std::size_t at = pick(b.size() + 1), n = 1 + pick(8);
          std::vector<uint8_t> ins(n);
          for (auto& x : ins) x = static_cast<uint8_t>(rng());
          b.insert(b.begin() + static_cast<std::ptrdiff_t>(at), ins.begin(), ins.end());
          break;
        }
        case 2: {  // drop a line
          std::vector<std::size_t> starts{0};
          for (std::size_t i = 0; i < b.size(); ++i)
            if (b[i] == '\n') starts.push_back(i + 1);
          std::size_t li = pick(starts.size());
          std::size_t from = starts[li], to = li + 1 < starts.size() ? starts[li + 1] : b.size();
          b.erase(b.begin() + static_cast<std::ptrdiff_t>(from), b.begin() + static_cast<std::ptrdiff_t>(to));
          break;
        }
        case 3: {  // insert structural noise
          static const char* kNoise[] = {"\n\n", "-->", "\r", "{", "<i", "Dialogue: 0,", "\n12\n", "[Events]\n", "WEBVTT"};
          std::string n = kNoise[pick(9)];
          std::size_t at = pick(b.size() + 1);
          b.insert(b.begin() + static_cast<std::ptrdiff_t>(at), n.begin(), n.end());
          break;
        }
        default:
          if (!b.empty()) b[pick(b.size())] = static_cast<uint8_t>(rng());  // flip a byte
      }
    }
    Format f = pick(4) == 0 ? all[pick(4)] : formats[s];
    CAPTURE(iter);
    vp::Result<Document> r = vp::Result<Document>(vp::ErrorCode::Internal, "not run");
    CHECK_NOTHROW(r = parse(b, f));
    REQUIRE(r.ok());
    for (const Cue& c : r->cues) {
      CHECK_NOTHROW((void)c.plainText());
      CHECK_NOTHROW((void)charsPerSecond(c));
      CHECK_NOTHROW((void)breakLines(c.plainText(), BreakHints{}, 42, 2));
    }
    auto w = write(r.value());
    REQUIRE(w.ok());
    bool utf16 = b.size() >= 2 && ((b[0] == 0xFF && b[1] == 0xFE) || (b[0] == 0xFE && b[1] == 0xFF));
    if (!utf16) {
      CHECK(w.value() == b);
      ++roundTrips;
    }
  }
  CHECK(roundTrips > 300);
}

TEST_CASE("subs: 50,000-cue file parses fast and linearly") {
  auto make = [](int n) {
    std::string s;
    s.reserve(static_cast<std::size_t>(n) * 80);
    for (int i = 0; i < n; ++i) {
      int t = i * 2000;
      char timing[64];
      std::snprintf(timing, sizeof timing, "%02d:%02d:%02d,%03d --> %02d:%02d:%02d,%03d", t / 3600000,
                    t / 60000 % 60, t / 1000 % 60, t % 1000, (t + 1500) / 3600000, (t + 1500) / 60000 % 60,
                    (t + 1500) / 1000 % 60, (t + 1500) % 1000);
      s += std::to_string(i + 1) + "\n" + timing + "\nLine number " + std::to_string(i) + " is here.\n<i>Second line.</i>\n\n";
    }
    return bytesOf(s);
  };
  auto timeParse = [](const std::vector<uint8_t>& b, std::size_t expectCues) {
    auto t0 = std::chrono::steady_clock::now();
    auto r = parse(b, Format::Srt);
    auto t1 = std::chrono::steady_clock::now();
    REQUIRE(r.ok());
    CHECK(r->cues.size() == expectCues);
    CHECK(r->warnings.empty());
    auto w = write(r.value());
    REQUIRE(w.ok());
    CHECK(w.value() == b);
    return std::chrono::duration<double>(t1 - t0).count();
  };
  auto small = make(5000), big = make(50000);
  double ts = timeParse(small, 5000);
  double tb = timeParse(big, 50000);
  MESSAGE("parse 5k cues: " << ts << " s, 50k cues: " << tb << " s");
  CHECK(tb < 2.0);
  CHECK(tb < ts * 25 + 0.05);  // linear: 10x the input, well under 25x the time
}
