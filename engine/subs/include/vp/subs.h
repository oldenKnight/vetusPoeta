// vp::subs — subtitle I/O: SRT, WebVTT, ASS/SSA and plain text. [CONTRACT, DESIGN.md §7]
// parse() never fails on malformed input; write(parse(x)) is byte-identical to x while the spans are unchanged.
// Numbering and timing lines are never regenerated: the writer copies idRaw and timingRaw verbatim.
// No exception escapes this module: parse() and write() return Result, the other functions do not throw
// (except std::bad_alloc from the other helpers).
#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "vp/result.h"

namespace vp::subs {

enum class Format { Srt, Vtt, Ass, Txt };

// Tag spans are opaque and copied verbatim (<i>, </i>, <v Name>, <c.x>, {\an8}, ASS override blocks {...}).
// Newline spans hold the exact line break found in the file ("\n", "\r\n", or "\\N"/"\\n" inside ASS text);
// an empty Newline raw means "the document's line break" (ASS: "\\N").
struct Span {
  enum Kind { Text, Tag, Newline } kind = Text;
  std::string raw;
};

struct Cue {
  uint32_t index = 0;          // 1-based position in the document (not the number written in the file)
  std::string idRaw;           // numbering / identifier line exactly as in the file ("12"); empty when absent
  std::string timingRaw;       // SRT/VTT: the whole timing line incl. VTT cue settings, byte exact.
                               // ASS: "<Start>,<End>" copied from the Dialogue fields (informative; the writer
                               // writes styleRaw, which already contains them). TXT: empty.
  std::string styleRaw;        // ASS: everything before the Text field ("Dialogue: 0,0:00:01.00,...,,"); else empty
  std::vector<Span> spans;     // text with tags and line breaks as spans
  std::string plainText() const;  // Text spans joined, Tag dropped, Newline -> ' ', entities decoded, spaces collapsed
  std::string sourceText;      // set by rules (not used by this module)

  // Layout recorded by parse() so write() can reproduce the file byte for byte. Callers do not edit these.
  bool fromSource = false;     // true when the cue came from parse(); new cues use the document newline
  std::string idEol;           // line break after idRaw
  std::string timingEol;       // line break after the timing line
  std::string textEol;         // line break after the last text line (ASS/TXT: after the cue's line/paragraph)
  std::string tailRaw;         // bytes between this cue and the next one (blank lines, NOTE blocks, ASS Comment:
                               // lines, stray text); for the last cue they live in Document::trailerRaw
  std::string parsedText;      // the spans' bytes as parsed; write() re-breaks only cues whose text differs
};

struct Warning {
  uint32_t index = 0;          // Cue::index, 0 for document-level warnings
  std::string kind;            // numbering_gap, numbering_duplicate, numbering_order, bad_id, missing_id, bad_timing,
                               // timing_format, overlap, reversed_time, empty_text, missing_blank_line, stray_text,
                               // missing_header, no_cues, no_events, bad_dialogue, format_mismatch,
                               // encoding_fallback, encoding_lossy; write(): unmappable_char, line_overflow
  std::string detail;          // English, for logs
};

struct Document {
  Format format = Format::Srt;
  std::string encoding = "utf-8";  // original encoding: "utf-8", "utf-16le", "utf-16be", "windows-1252"
  bool bom = false;                // original file had a byte-order mark
  std::string newline = "\n";      // "\r\n" or "\n": first line break found in the file
  std::string headerRaw;           // everything before the first cue (VTT header + blocks, ASS sections + Format line)
  std::vector<Cue> cues;
  std::string trailerRaw;          // everything after the last cue's text
  std::vector<Warning> warnings;
};

struct BreakHints {
  std::vector<std::string> breakBefore;  // words a line may start with (conjunctions, prepositions); matched
                                         // ASCII-case-insensitively, ignoring leading punctuation and tags
};

struct WriteOptions {
  std::string encoding;            // empty: the document's encoding. Accepts utf-8, utf-16le, utf-16be, windows-1252
  std::optional<bool> bom;         // unset: the document's bom
  int maxLine = 42;
  int maxLines = 2;
  bool rebreak = true;             // re-break only cues whose spans differ from what parse() read (never TXT)
  BreakHints hints;
};

Result<Document> parse(const std::vector<uint8_t>& bytes, Format hint);
// warnings (optional) receives unmappable_char / line_overflow notes.
Result<std::vector<uint8_t>> write(const Document& doc, const WriteOptions& options = WriteOptions{},
                                   std::vector<Warning>* warnings = nullptr);

// Line breaking: never splits a word, a tag (<...>) or an ASS override block ({...}); prefers breaks after
// , ; : . ! ? and before hint words; balances line lengths; keeps a dialogue dash with its line and puts each
// dialogue turn on its own line. Width is counted in code points, combining marks and tags excluded.
// When the text cannot fit in maxLines x maxLine the best effort is returned and *overflow is set to true.
std::vector<std::string> breakLines(const std::string& text, const BreakHints& hints, int maxLine = 42,
                                    int maxLines = 2, bool* overflow = nullptr);

// Reading speed: visibleLength(plainText()) / duration in seconds, from timingRaw; 0 if unparsable or duration <= 0.
double charsPerSecond(const Cue& cue);
// SRT/VTT: "hh:mm:ss,mmm --> hh:mm:ss,mmm [settings]" (hours optional, ',' or '.'); ASS: "h:mm:ss.cc,h:mm:ss.cc".
bool parseTiming(const std::string& timingRaw, Format format, int64_t& startMs, int64_t& endMs);

// Helpers shared with rules: width in code points without combining marks; tokenise one text field into spans
// (format-specific tag rules; '\n' becomes a Newline span with empty raw).
std::size_t visibleLength(const std::string& utf8);
std::vector<Span> splitSpans(const std::string& text, Format format);

}  // namespace vp::subs
