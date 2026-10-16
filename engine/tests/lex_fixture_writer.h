// Test-only reference encoder for the `.vpl` format (DESIGN.md §5). Not shipped. Until the Python packer
// (tools/build_library/pack.py) exists this is the executable statement of the layout: the reader in engine/lex is
// tested against its bytes and tests/fixtures/lex/SPEC_CHECK.md lists the offsets it produces.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace vp::lexfix {

struct SenseIn { std::string glossEn, glossEs, keywords; uint16_t tags = 0, rank = 0; };
struct CellIn { uint32_t feat = 0; std::string form; };   // packed features (vp/features.h), surface form

struct LemmaIn {
  std::string head, key;
  uint8_t pos = 0, cls = 0, gender = 0, tier = 0;
  uint16_t freqRank = 0;
  uint8_t whitFreq = 0, tierSource = 0;
  std::string emoji, glossEn, glossEs;   // emoji "" is written as offset 0 (none)
  std::vector<SenseIn> senses;
  uint16_t flags = 0;
  std::vector<CellIn> cells;             // any order; written sorted by feat_id
  uint16_t principalCount = 0;
  std::string principal;
};

struct AnalysisIn { std::string key; uint32_t lemma = 0; uint32_t feat = 0; uint16_t flags = 0; std::string display; };
struct CandidateIn { std::string keyword; uint32_t lemma = 0; uint16_t sense = 0; uint8_t score = 0, pos = 0; };

struct LexiconIn {
  std::string lang;                     // "la", "grc", "en", "es"
  uint16_t major = 1, minor = 0;
  std::string notice;
  std::vector<LemmaIn> lemmas;          // lemma id = index
  std::vector<AnalysisIn> analyses;     // any order; grouped by key (stable) and keys sorted bytewise
  std::vector<CandidateIn> candidates;  // any order; sorted by keyword, then score desc, lemma asc, sense asc
  bool morphologyOnly = false;          // true: no SENS, GENX, REVX (English / Spanish files)
};

struct SectionInfo { std::string tag; uint64_t offset = 0, length = 0; };

// Encodes the lexicon. layout (optional) receives the section table as written.
std::vector<uint8_t> build(const LexiconIn& in, std::vector<SectionInfo>* layout = nullptr);

// The committed fixtures (tests/fixtures/lex/*.vpl) are build() of these.
LexiconIn latinFixture();     // puella, amo, bonus, virgo, diligo, amor; senses, cells, REVX "girl" "love" ...
LexiconIn greekFixture();     // anthropos with Attic and non-Attic forms
LexiconIn englishFixture();   // morphology only
// A generated lexicon with nLemmas lemmas of 8 forms each (timing and RSS tests; not committed).
LexiconIn syntheticFixture(uint32_t nLemmas);

}  // namespace vp::lexfix
