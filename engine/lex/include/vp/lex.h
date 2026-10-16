// Read-only reader of the `.vpl` lexicon files. [CONTRACT, DESIGN.md §5 and §5.1]
// One Lexicon owns one read-only memory map of one file. Every string_view returned by it points into that map and
// dies with the Lexicon (or when it is moved from). Lookups never allocate except by growing the caller's vectors;
// every offset read from the file is bounds-checked before use, so a damaged file gives lexicon_corrupt at open or
// empty / false results later, never undefined behaviour. Nothing here throws.
#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string_view>
#include <utility>
#include <vector>

#include "vp/mmap.h"
#include "vp/result.h"

namespace vp::lex {

constexpr uint32_t kNoLemma = 0xFFFFFFFFu;   // Lemma::id of the record returned for an out-of-range id

// ANAL flags (DESIGN §5).
enum AnalFlag : uint16_t {
  FromTable = 1u << 0, FromFormOf = 1u << 1, WhitakerOnly = 1u << 2, AltSpelling = 1u << 3, NonAttic = 1u << 4,
  LateLatin = 1u << 5, PoeticRare = 1u << 6, EncliticStripped = 1u << 7
};
// LEMM flags (DESIGN §5).
enum LemmaFlag : uint16_t {
  ProperName = 1u << 0, Indeclinable = 1u << 1, Deponent = 1u << 2, Impersonal = 1u << 3, SharedEl = 1u << 4,
  PluralOnly = 1u << 5, Defective = 1u << 6, HasTable = 1u << 7
};

struct Analysis { uint32_t lemma = 0; uint16_t feat = 0; uint16_t flags = 0; std::string_view display; };
struct Lemma    { std::string_view head, key, glossEn, glossEs, emoji, principal; uint8_t pos = 0, cls = 0, gender = 0,
                  tier = 0, whitFreq = 0, tierSource = 0; uint16_t freqRank = 0, flags = 0; uint32_t id = kNoLemma; };
struct Sense    { std::string_view glossEn, glossEs, keywords; uint16_t tags = 0, rank = 0; };
struct Candidate{ uint32_t lemma = 0; uint16_t sense = 0; uint8_t score = 0, pos = 0; };

// Record counts and format version of an open file (dev tools, `inspect`, tests).
struct Stats {
  uint16_t major = 0, minor = 0;
  uint64_t fileSize = 0;
  uint32_t keys = 0, analyses = 0, lemmas = 0, senses = 0, features = 0, cells = 0, keywords = 0, candidates = 0;
  bool hasSenses = false, hasCells = false, hasReverse = false;   // morphology-only files (en, es) have none
};

class Lexicon {                      // non-copyable, movable; holds one mmap; all views die with it
 public:
  Lexicon() = default;               // empty: every query returns nothing
  Lexicon(Lexicon&& o) noexcept;
  Lexicon& operator=(Lexicon&& o) noexcept;
  Lexicon(const Lexicon&) = delete;
  Lexicon& operator=(const Lexicon&) = delete;
  ~Lexicon() = default;

  // Maps the file and validates the header (magic, major version, file size), the section table (every section
  // inside the file, 8-byte aligned, after the table, no duplicate tags, the six required ones present) and that
  // every section's record count fits its length. Errors: lexicon_missing (no such file), io (unreadable),
  // lexicon_version (major != 1), lexicon_corrupt (anything else). Does not hash the body: see verifySha256().
  static Result<Lexicon> open(const std::filesystem::path&);
  // SHA-256 of bytes [256, file_size) against the header (touches every page once). lexicon_corrupt on mismatch.
  Result<void> verifySha256() const;

  std::string_view lang() const; std::string_view notice() const; uint32_t lemmaCount() const;
  Stats stats() const;
  bool lookup(std::string_view key, std::vector<Analysis>& out) const;        // exact key; out appended
  // Up to `max` keys starting with keyPrefix, in key order (appended).
  void prefix(std::string_view keyPrefix, size_t max, std::vector<std::string_view>& out) const;
  Lemma lemma(uint32_t id) const; void senses(uint32_t id, std::vector<Sense>& out) const;
  bool generate(uint32_t lemma, uint32_t packedFeatures, std::string_view& form) const;  // exact cell
  // (packed features, form) of every GENX cell of the lemma, in feat_id order (appended).
  void cells(uint32_t lemma, std::vector<std::pair<uint32_t,std::string_view>>& out) const;
  // Candidates of an exact keyword, appended sorted by score desc, lemma id asc, sense asc.
  void reverse(std::string_view keyword, std::vector<Candidate>& out) const;
  // feature: packed word of a feat_id (0 when out of range). featId: inverse; found=false when absent.
  uint32_t feature(uint16_t featId) const; uint16_t featId(uint32_t packed, bool& found) const;

 private:
  struct Span { const uint8_t* p = nullptr; size_t len = 0; };
  struct View {
    const uint8_t* base = nullptr;
    size_t size = 0;
    uint16_t major = 0, minor = 0;
    std::string_view lang;
    Span note, strs, keys, anal, lemm, sens, feat, genx, revx;
    uint32_t nKeys = 0, nAnal = 0, nLemmas = 0, nSenses = 0, nFeat = 0, nCells = 0, nKw = 0, nCand = 0;
    bool featSorted = false;
  };
  std::string_view str(uint32_t off) const;              // empty view for an out-of-range offset
  bool strChecked(uint32_t off, std::string_view& s) const;
  std::string_view keyAt(uint32_t i) const;
  uint32_t keyLowerBound(std::string_view key) const;
  std::string_view kwAt(uint32_t i) const;

  MappedFile file_;
  View v_;
};

}  // namespace vp::lex
