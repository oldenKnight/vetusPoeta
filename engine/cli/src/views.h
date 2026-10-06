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
// Polytonic -> monotonic Greek for export greek:"monotonic". With the rules engine linked (VP_HAVE_RULES) this is
// vp::grc::toMonotonic (C12: one tonos per word, breathings / iota subscript / length marks dropped, diaeresis kept,
// monosyllables unaccented except ή and the interrogatives); without it a local approximation of the same rules.
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
// Stores an engine result in a record (target, confidence, score, alternatives, checks, reasons, and the hidden
// tokens and flags).
void applyOutput(const vp::rules::CueOutput& out, vp::CueRecord& r);

// ---- per-cue engine data kept in the project (hidden CueReason kinds; never shown by cue.get) ----
// CueRecord (engine/core) has no fields for the tokens, flags and job facts of a translation, so they travel as
// CueReasons of kind "_tokens" / "_flags" / "_job" with compact data. They are saved with the project, restored
// by undo/redo and filtered out of every view.
extern const char* const kTokensKind;
extern const char* const kFlagsKind;
extern const char* const kJobKind;
bool isHiddenReason(const vp::CueReason& r);
const vp::CueReason* hiddenReason(const vp::CueRecord& r, const char* kind);
void setHidden(vp::CueRecord& r, const char* kind, std::string data);   // empty data removes it
// Tokens as a compact JSON array (features as 9 table indices); decode returns false on damaged data.
std::string encodeTokens(const std::vector<vp::rules::TokenView>& tokens);
bool decodeTokens(const std::string& data, std::vector<vp::rules::TokenView>& out);
// FeatureView strings back to the packed vp::feat form (unknown strings -> false; the rest is still filled).
bool featuresToPacked(const vp::rules::Features& f, uint32_t& packed);
std::vector<std::string> storedFlags(const vp::CueRecord& r);
void setStoredFlags(vp::CueRecord& r, const std::vector<std::string>& flags);
// What a job knew about engines ii/iii and Orbergise when it produced the cue (for the evidence rows).
struct JobFacts {
  bool model = false, online = false, orberg = false;
  std::vector<std::string> modelHeads;                    // headwords the model chose or confirmed
  std::vector<std::pair<std::string, int>> onlineVerdicts;  // headword -> +1 agrees, -1 disagrees, 0 unknown
};
std::string encodeJob(const JobFacts& j);
bool decodeJob(const std::string& data, JobFacts& out);
// Orbergise facts the engine returned for the cue (CueOutput.meaningPercent / meaningMissing / original), hidden kind
// "_orberg"; applyOutput stores them, an edit keeps `original` and drops the meaning (cue.get then measures it).
extern const char* const kOrbergKind;
struct OrbergFacts {
  int percent = -1;                   // -1: not computed by the engine
  std::vector<std::string> missing;
  std::string original;
};
std::string encodeOrberg(const OrbergFacts& f);   // "" when there is nothing to keep (percent < 0, no original)
bool decodeOrberg(const std::string& data, OrbergFacts& out);

// ---- Orbergise: the original-language file ----
// One cue's text and timing (timed = false for a text document or an unparsable timing).
struct TimedText {
  std::string text;
  int64_t start = 0, end = 0;
  bool timed = false;
};
// The original's text for each of `cues` (DESIGN 10.7 "aligned by cue index, then by time overlap"):
//  1. same number of cues, or either side untimed (a .txt): by cue index (position);
//  2. otherwise by time: every original cue that overlaps the cue for at least half of the shorter of the two, in
//     order, joined with a space; a cue that no original cue covers that well gets the original cue with the
//     largest positive overlap; a cue with no overlap at all gets "" (the engine then rewrites from the Latin).
std::vector<std::string> alignOriginal(const std::vector<TimedText>& cues, const std::vector<TimedText>& original);
// Language of an original-language file when orbergise.start gives none: "es" when Spanish markers (¿ ¡ ñ, accented
// vowels, common Spanish function words) outweigh common English function words, else "en".
std::string detectOriginalLang(const std::vector<std::string>& texts);

// ---- ReasonView in the shapes the UI reads (DESIGN 9.2 "Consumed by the UI") ----
// The rules engine's reasons are turned into: sense {source, sense, senseEs, context[]}; one candidate reason per
// candidate {lemmaId, head, form, tier, band, chosen, gloss, score}; form {features, form?}; evidence
// {source: wiktionary|whitaker|model|online, state: yes|no|off|none} (four rows per dictionary token, from the
// lexicon and the job facts); orbergise change {was, now, why}; name {form}; correction {target}. Reasons already
// in a UI shape (objects) pass through.
struct ReasonViewCtx {
  const vp::lex::Lexicon* lex = nullptr;   // the target lexicon (Latin or Greek); null: no lexicon facts
  bool latin = true;                       // candidate forms are generated for Latin only
  std::string source;                      // the cue's source text (context words)
};
json reasonsView(const vp::CueRecord& r, const std::vector<vp::rules::TokenView>& tokens, const ReasonViewCtx& ctx);
// Orbergise: words of `target` that replace words of `latinSource` (aligned by a longest common subsequence on
// latin_key), as {tokenIndex, was, now}.
struct WordChange { int tokenIndex = -1; std::string was, now; };
std::vector<WordChange> wordChanges(const std::string& latinSource, const std::vector<vp::rules::TokenView>& tokens);
// Meaning check (DESIGN 10.7): share of the content lemmas (noun, verb, adjective, adverb) of `before` that are
// still in `after`, and the headwords of the missing ones. percent = 100 when `before` has none.
struct Meaning { int percent = 100; std::vector<std::string> missing; };
Meaning meaningCheck(const vp::lex::Lexicon& la, const std::string& before, const std::string& after);
// Re-attaches reasons to new tokens after an edit: a reason on token k follows the token with the same text (k-th
// occurrence of that text); reasons whose word is gone are dropped, sentence-level ones are kept.
std::vector<vp::CueReason> remapReasons(const std::vector<vp::CueReason>& reasons,
                                        const std::vector<vp::rules::TokenView>& oldTokens,
                                        const std::vector<vp::rules::TokenView>& newTokens);

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
