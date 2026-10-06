// Pure helpers of the CLI: protocol views (DESIGN §9.2), feature strings, language pairs, subtitle formats, target
// text transforms for export (macrons, emoji, monotonic Greek), tag re-attachment and the export builder.
// No I/O, no global state; unit-tested in engine/tests/test_cli.cpp. Nothing here throws except std::bad_alloc
// and nlohmann::json errors on misuse (the server catches at its boundary).
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "json.hpp"
#include "vp/features.h"
#include "vp/lex.h"
#include "vp/project.h"
#include "vp/result.h"
#include "vp/rules.h"
#include "vp/subs.h"

namespace vpcli {

using json = nlohmann::json;

// ---- features and lexicon views ----
// FeatureView strings ("noun", "nominative", "singular", "feminine", "first", "present", "indicative", "active",
// "positive"; "" when absent). Gerundive and supine (extra bits) are reported as the mood.
vp::rules::Features featuresFromPacked(uint32_t packed);
json featureJson(const vp::rules::Features& f);
const char* posName(uint8_t pos);
const char* genderName(uint8_t gender);
std::vector<std::string> extraNames(uint8_t extra);
std::vector<std::string> lemmaFlagNames(uint16_t flags);
std::vector<std::string> analFlagNames(uint16_t flags);
std::vector<std::string> senseTagNames(uint16_t tags);
json lemmaJson(const vp::lex::Lexicon& lx, uint32_t id);   // LemmaView; null when the id is out of range
json senseJson(const vp::lex::Sense& s);

// ---- scripts, languages, pairs ----
bool looksGreek(std::string_view word);                    // any Greek or Greek Extended code point
std::string lexKey(std::string_view word, bool greek);     // latin_key or greek_key
const char* langCode(vp::rules::Lang l);                   // "en" "es" "la" "grc"
bool langFromCode(const std::string& code, vp::rules::Lang& out);
struct PairLangs { vp::rules::Lang source = vp::rules::Lang::En, target = vp::rules::Lang::La; };
bool parsePair(const std::string& pair, PairLangs& out);   // "en-la" -> En, La

// ---- subtitle formats ----
bool formatFromPath(const std::string& path, vp::subs::Format& out);   // .srt .vtt .ass .ssa .txt
bool formatFromName(const std::string& name, vp::subs::Format& out);   // "srt" "vtt" "ass" "txt"
const char* formatName(vp::subs::Format f);
std::string baseName(const std::string& path);

// ---- target text ----
std::vector<std::string> splitLines(const std::string& text);   // on '\n' ('\r' dropped)
std::string flattenLines(const std::string& text);              // '\n' -> ' ', spaces collapsed, trimmed
// Display lines of a target: its own lines when they fit (<= maxLines lines of <= maxLine width), else breakLines.
std::vector<std::string> displayLines(const std::string& target, const vp::subs::BreakHints& hints, int maxLine,
                                      int maxLines, bool* overflow);
// Removes emoji that the engine added: pictographic code points (music notes excluded) that do not occur in the
// source, with their variation selectors, joiners and skin tones; spaces left behind are collapsed.
std::string stripAddedEmoji(const std::string& target, const std::string& source);
// Polytonic -> monotonic Greek (breathings, iota subscript and koronis dropped; grave and circumflex -> acute).
std::string greekMonotonic(const std::string& text);
// A5 (markup/line layout) on a target as the engine produced it.
vp::rules::Check markupCheck(const std::string& target, int maxLine, int maxLines);

// The source cue's tags re-attached around a new text: tags before the first and after the last text are kept, tags
// inside the text are dropped (*dropped = true), opened <i>/<b>/<u>/<font> are closed again, unmatched closers are
// dropped. Lines of `text` ('\n') become Newline spans unless `singleLine` (then one Text span with the flat text).
std::vector<vp::subs::Span> retargetSpans(const std::vector<vp::subs::Span>& source, const std::string& text,
                                          bool singleLine, bool* dropped);

// ---- views ----
struct CueViewCtx {
  vp::subs::Format format = vp::subs::Format::Srt;
  int maxLine = 42, maxLines = 2;
  double cpsLimit = 17;
  vp::subs::BreakHints hints;
};
json cueViewJson(const vp::subs::Cue& src, const std::string& source, const vp::CueRecord& r, const CueViewCtx& ctx);
json tokenJson(const vp::rules::TokenView& t);
json checkJson(const vp::CueCheck& c);
json reasonJson(const vp::CueReason& r);
json altJson(const vp::Alternative& a);
const char* confidenceName(vp::rules::Confidence c);
// Stores an engine result in a record (target, confidence, score, alternatives, checks, reasons).
void applyOutput(const vp::rules::CueOutput& out, vp::CueRecord& r);

// ---- export ----
struct ExportOptions {
  vp::subs::Format format = vp::subs::Format::Srt;
  bool emoji = false, macrons = false, monotonic = false, rebreak = true;
  std::string encoding;                 // "" = the source's
  std::optional<bool> bom;              // unset = the source's
  int maxLine = 42, maxLines = 2;
  double cpsLimit = 17;
  vp::subs::BreakHints hints;
};
struct ExportWarning { int64_t index = -1; std::string kind; };   // 0-based cue position, -1 = whole file
struct ExportResult { std::vector<uint8_t> bytes; std::vector<ExportWarning> warnings; };
// The text a cue exports: target (or the source text when the target is empty), with macrons/emoji/Greek options.
std::string exportText(const std::string& target, const std::string& source, const ExportOptions& o);
// Rebuilds the document: same format = the source document with each cue's text spans replaced (numbering,
// timing, header, tags and layout byte for byte); other format = a new document (timing converted).
// `targets[i]` belongs to doc.cues[i]; an empty target keeps the source text (warning "untranslated").
// Warnings carry the 0-based cue position.
vp::Result<ExportResult> buildExport(const vp::subs::Document& doc, const std::vector<std::string>& targets,
                                     const std::vector<std::string>& sources, const ExportOptions& o);
// Lines a cue would get in the export (for export.preview).
std::vector<std::string> previewLines(const vp::subs::Cue& src, const std::string& target, const std::string& source,
                                      const ExportOptions& o);
std::string formatTiming(int64_t startMs, int64_t endMs, vp::subs::Format f);

}  // namespace vpcli
