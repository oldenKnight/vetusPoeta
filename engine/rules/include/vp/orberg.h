// Orbergise (DESIGN.md §10.7, PREDESIGN 1.2.3, task C14): rewrites Latin in beginner-reader Latin (first-year
// vocabulary at a tier ceiling, the simple structures of DESIGN §1.2) while keeping the meaning.
//  * Without the original-language text: the Latin is analysed by la2x (constraint-propagation disambiguation) and
//    rewritten in place, clause by clause: structure rewrites from data/curated/simplify_la.tsv (ablative absolute,
//    gerundive of obligation, supine, future participle + sum, historic infinitive, cum + subjunctive, double
//    negatives, relative chains, long sentences), then vocabulary above the ceiling swapped for a core word of the
//    same sense (periphrasis_la.tsv first, then the lexicon's sense keywords). Forms are regenerated with the
//    morphology generator; words that already satisfy the ceiling and the structure rules are kept as written.
//  * With the original (the aligned EN/ES cue, C27): the original is EVIDENCE only. It ranks same-sense swap
//    candidates (a candidate whose gloss names a word of the original first) and enters the meaning check (words of
//    the original the input covers must stay covered); a cue is never replaced by a translation of the original.
//  * Swaps keep the sense (C27): syn rows of simplify_la.tsv (confirmed, may stay OK), periphrasis_la.tsv rows, and
//    lexicon / teacher-gloss candidates only when they share the word's sense (English and Spanish gloss phrases,
//    sense classes: a verb of motion never becomes a verb of change or ceasing, valency). No such word: the word is
//    kept and the cue is Check (flag tier-kept). Fixed phrases (rēs gestae, grātiās agere, valēre iubēre, ad +
//    gerund ...) are never split. Every swap that is not a syn row is at most Check (flag synonym).
//  * Never nonsense (C27): a rewritten sentence whose re-analysis adds an A1/A3/A4 fault or misreads a swapped word
//    is discarded (vocabulary-only, then structure-only, then the sentence as written; flags rewrite-partial /
//    rewrite-discarded, Check).
//  * Every change carries "was -> now" with a reason (vocabulary | structure | order); the meaning check re-analyses
//    the output with la2x and compares the content lemmas a reader reads (best readings) with the input's: a lemma
//    is kept when it is there or was replaced by a curated row or a word whose glosses share its sense; losses are
//    listed in `missing` ("moveō -> mūtō") and make the cue Check (below 0.6 also meaning-low).
//  * Emoji clusters, spacing and the macron convention of the source are kept (no macrons added to a text without).
// Deterministic; nothing here throws across the module boundary (orbergise() and cues() catch).
#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "vp/curated.h"
#include "vp/lex.h"
#include "vp/result.h"
#include "vp/rules.h"

namespace vp::la2x { class Translator; }
namespace vp::check { class LatinChecker; }

namespace vp::orberg {

using rules::Confidence;
using rules::Lang;

struct OrbergOptions {
  int tierCeiling = 1;       // 1 (Familia Romana core) or 2
  bool keepNames = true;     // names are never touched (false: names with a periphrasis row may be replaced)
  bool simplify = true;      // structure rewrites; false = vocabulary swaps only
  bool hasOriginal = false;  // set by orbergise() when an original text is given
  bool macrons = true;       // new words carry length marks when the input does (never added to kept words)
  int sourceMacrons = -1;    // C27: the document's convention: 1 written with macrons, 0 without (new words get none),
                             // -1 unknown (decided per sentence from the words that show or omit their length marks)
};

// One "was -> now". tokenIndex: index of the first new word in OrbergResult::tokens (-1 for a pure deletion).
struct Change {
  std::string from, to;
  std::string reason;        // "vocabulary" | "structure" | "order"
  int tokenIndex = -1;
  std::string rule;          // simplify_la.tsv rule id, "periphrasis", "synonym", "pair", "original"
  std::string why;           // human text ("ablative absolute -> postquam clause")
};

struct OrbergResult {
  std::string text;
  std::vector<Change> changes;
  double meaning = 1.0;                      // content-lemma overlap in [0,1]
  std::vector<std::string> missing;          // input content lemmas (heads) not accounted for in the output
  Confidence confidence = Confidence::Check;
  std::vector<rules::TokenView> tokens;      // the output re-analysed (offsets into text)
  std::vector<rules::Reason> reasons;        // kind "orbergise" {was, now, why} per change, plus notes
  std::vector<rules::Check> checks;          // A1 A2 A3 A4 A6 (participles count with their verb's tier), A7 meaning
  std::vector<std::string> flags;            // "orberg-original", "orberg-kept", "tier-exceeded", "meaning-low", ...
  bool fromOriginal = false;                 // always false since C27 (the original is evidence, never the output)
};

// Latin made from the original-language text by the EN/ES -> LA pipeline (filled by the engine).
struct OriginalLatin {
  std::string text;                          // display form (macrons per options)
  std::vector<uint32_t> lemmas;              // Latin content lemmas the transfer chose (meaning-check reference)
  bool unknown = false;                      // some source word had no Latin lemma (bracketed)
  bool fallback = false;                     // the frame builder needed a fallback (structure doubtful)
};
// (original text, its language, tier ceiling, Latin lemmas of the input to prefer when they are candidates) -> Latin.
using OriginalFn = std::function<bool(const std::string& original, Lang lang, int tierCeiling,
                                      const std::vector<uint32_t>& prefer, OriginalLatin& out)>;

// Loaded tables (simplify_la.tsv) and bounded caches; one per engine, reused across calls. Not thread-safe (the
// engine serialises calls).
class Resources {
  struct Key { explicit Key() = default; };

 public:
  explicit Resources(Key);
  ~Resources();
  Resources(const Resources&) = delete;
  Resources& operator=(const Resources&) = delete;
  // Reads simplify_la.tsv from the first directory of `dirs` that holds it. not_found with a hint when none does;
  // malformed rows are skipped with a warning.
  static Result<std::unique_ptr<Resources>> create(const lex::Lexicon& la, const curated::CuratedData& cd,
                                                   const std::vector<std::filesystem::path>& dirs);
  const std::vector<std::string>& warnings() const;
  struct Impl;
  Impl& impl() { return *impl_; }

 private:
  std::unique_ptr<Impl> impl_;
};

struct EngineContext {
  const lex::Lexicon* la = nullptr;
  const curated::CuratedData* cd = nullptr;
  la2x::Translator* la2x = nullptr;          // analysis (constraint propagation)
  check::LatinChecker* checker = nullptr;    // A1-A4 on the output
  Resources* resources = nullptr;
  const std::vector<rules::GlossaryEntry>* glossary = nullptr;
  OriginalFn fromOriginal;                   // unused since C27 (kept for source compatibility)
  double cpsLimit = 17.0;                    // A8 for cues
  int maxLine = 42, maxLines = 2;
};

// One cue or text (one or more sentences). `original` may be null.
OrbergResult orbergise(const std::string& latin, const std::string* original, Lang originalLang,
                       const OrbergOptions&, const EngineContext&);

// Engine pair la-la with Options.orbergise: one CueOutput per cue (CueInput.sourceText = the Latin, CueInput.originalText
// / originalLang = the aligned original when present). target = the rewrite laid out in lines; reasons kind
// "orbergise" with data {was, now, why}; meaningPercent / meaningMissing / original filled. Never throws.
std::vector<rules::CueOutput> cues(const std::vector<rules::CueInput>& in, const rules::Options&, const rules::Context&,
                                   const EngineContext&, const std::function<void(size_t)>& progress,
                                   const std::function<bool()>& cancelled);

// Data of an "orbergise" reason: {"was":..., "now":..., "why":...} (JSON object text).
std::string reasonData(const Change& c);

}  // namespace vp::orberg
