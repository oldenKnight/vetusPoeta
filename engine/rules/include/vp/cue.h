// Cue assembly (DESIGN.md §10.5): a translated sentence goes back onto the source cues it came from, split at clause or
// phrase boundaries nearest the source split points (never inside an NP; proportional by character share when no
// boundary is near); per-cue line breaking through vp::subs::breakLines with Latin break hints; tag policy; songs and
// nonverbal cues. Pure functions over strings and token views; deterministic; never changes the number of cues.
#pragma once
#include <string>
#include <vector>

#include "vp/curated.h"
#include "vp/frame.h"
#include "vp/rules.h"
#include "vp/subs.h"

namespace vp::cue {

// Latin text with token views whose [start, end) are byte offsets into `text`.
struct Latin {
  std::string text;
  std::vector<rules::TokenView> tokens;
};

// Splits the Latin of one source sentence into src.parts.size() pieces (one per source cue part). Pieces may be
// empty when the Latin has fewer words than the sentence has cue parts. Token offsets are rebased to each piece.
// `srcOffset` (optional, one per Latin token): the byte offset in src.text of the source word the token translates
// (-1 unknown); with it the split target is the share of Latin words that translate earlier cue parts (Latin order
// differs from the source), else the character share.
std::vector<Latin> splitSentence(const frame::SourceSentence& src, const Latin& latin,
                                 const std::vector<int>* srcOffset = nullptr);

// Appends `piece` to `cue` (one space between non-empty texts), shifting token offsets.
void append(Latin& cue, const Latin& piece);

// Latin break hints: line breaks preferred before et, sed, aut, quod, quia, cum, sī, ut, nē, in, ad, ab, ex, dē, per,
// prō (and after punctuation, which breakLines does itself).
const subs::BreakHints& latinBreakHints();

struct Layout { std::vector<std::string> lines; bool overflow = false; std::string joined; };
// Lines of one cue (maxLine code points, maxLines lines); `joined` is the lines joined with '\n'.
Layout layout(const std::string& text, int maxLine = 42, int maxLines = 2);
// Re-locates every token's offsets inside `text` (after line breaking), in order. Tokens not found keep -1/-1.
void relocate(const std::string& text, std::vector<rules::TokenView>& tokens);

// Tag policy: a cue whose text is entirely wrapped in one tag pair (<i>...</i>, {\i1}...{\i0}) keeps the pair around
// the Latin; position tags at the very start ({\an8}) are kept; any other tag is dropped and `approximated` is set
// (A5 "tag position approximated", confidence Check).
struct TagResult { std::vector<subs::Span> spans; bool approximated = false; };
TagResult applyTags(const std::vector<subs::Span>& source, const std::string& latin);

// Nonverbal cue text ("laughs" inside brackets): the Latin from nonverbal_en_la.tsv, or the input unchanged.
std::string nonverbal(const std::string& inner, const curated::CuratedData& cd, bool& translated);

}  // namespace vp::cue
