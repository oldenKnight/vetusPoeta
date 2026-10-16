// `.vpl` reader. Layout: DESIGN.md §5 (byte offsets spelled out in tests/fixtures/lex/SPEC_CHECK.md).
// Every multi-byte integer is little-endian and may be unaligned (ANAL records start at section offset 4), so all
// reads go through rd16/rd32/rd64 on bytes. Every count and offset taken from the file is checked against the
// section it indexes before it is used.
#include "vp/lex.h"

#include <algorithm>
#include <cstring>
#include <string>

#include "vp/fs.h"
#include "vp/sha256.h"

namespace vp::lex {

namespace {

constexpr size_t kHeaderSize = 256;
constexpr size_t kTableEntry = 24;   // char[4] tag, u32 reserved, u64 offset, u64 length
constexpr size_t kAnalRec = 12, kLemmRec = 48, kSensRec = 16, kGenxRec = 8, kCandRec = 8;
// Header field offsets (DESIGN §5, packed in the order listed there).
constexpr size_t kOffMagic = 0, kOffMajor = 4, kOffMinor = 6, kOffLang = 8, kOffSectionCount = 16, kOffFileSize = 20,
                 kOffSha = 28;

const char kCorruptHint[] = "The dictionary file is damaged. Reinstall the program or rebuild the library.";
const char kVersionHint[] = "The dictionary file was made for another version of the program. Reinstall the program.";
const char kMissingHint[] = "The dictionary file is missing. Reinstall the program or rebuild the library.";

inline uint16_t rd16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
inline uint32_t rd32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}
inline uint64_t rd64(const uint8_t* p) { return static_cast<uint64_t>(rd32(p)) | (static_cast<uint64_t>(rd32(p + 4)) << 32); }

Result<void> corrupt(const std::string& path, const std::string& what) {
  return Result<void>(ErrorCode::LexiconCorrupt, "lexicon '" + path + "': " + what, kCorruptHint);
}

}  // namespace

Lexicon::Lexicon(Lexicon&& o) noexcept : file_(std::move(o.file_)), v_(o.v_) { o.v_ = View{}; }

Lexicon& Lexicon::operator=(Lexicon&& o) noexcept {
  if (this != &o) {
    file_ = std::move(o.file_);
    v_ = o.v_;
    o.v_ = View{};
  }
  return *this;
}

Result<Lexicon> Lexicon::open(const std::filesystem::path& p) {
  try {
    const std::string path = fs::toU8(p);
    if (!fs::fileExists(path))
      return Result<Lexicon>(ErrorCode::LexiconMissing, "lexicon '" + path + "' not found", kMissingHint);
    Result<MappedFile> mf = MappedFile::open(path);
    if (!mf) return Result<Lexicon>(mf.error());

    Lexicon lx;
    lx.file_ = std::move(mf.value());
    View& v = lx.v_;
    v.base = lx.file_.data();
    v.size = lx.file_.size();
    const uint8_t* b = v.base;
    auto fail = [&](const std::string& what) { return Result<Lexicon>(corrupt(path, what).error()); };

    // ---- header ----
    if (v.size < kHeaderSize) return fail("shorter than the 256-byte header");
    if (std::memcmp(b + kOffMagic, "VPLX", 4) != 0) return fail("bad magic (not a .vpl file)");
    v.major = rd16(b + kOffMajor);
    v.minor = rd16(b + kOffMinor);
    if (v.major != 1)
      return Result<Lexicon>(ErrorCode::LexiconVersion,
                             "lexicon '" + path + "' has format version " + std::to_string(v.major) + "." +
                                 std::to_string(v.minor) + "; this program reads version 1.x",
                             kVersionHint);
    const uint64_t declared = rd64(b + kOffFileSize);
    if (declared != static_cast<uint64_t>(v.size))
      return fail("file size " + std::to_string(v.size) + " differs from the header's " + std::to_string(declared) +
                  " (truncated or padded)");
    size_t langLen = 0;
    while (langLen < 8 && b[kOffLang + langLen] != 0) ++langLen;
    if (langLen == 0) return fail("empty language code");
    for (size_t i = 0; i < 8; ++i) {
      const uint8_t c = b[kOffLang + i];
      if (i < langLen ? !((c >= 'a' && c <= 'z') || c == '-') : c != 0) return fail("bad language code");
    }
    v.lang = std::string_view(reinterpret_cast<const char*>(b + kOffLang), langLen);

    // ---- section table ----
    const uint32_t sectionCount = rd32(b + kOffSectionCount);
    const uint64_t tableEnd = kHeaderSize + static_cast<uint64_t>(sectionCount) * kTableEntry;
    if (tableEnd > v.size) return fail("section table runs past the end of the file");
    struct Want { const char* tag; Span* span; bool required; bool seen; };
    Want wants[] = {{"NOTE", &v.note, true, false}, {"STRS", &v.strs, true, false}, {"KEYS", &v.keys, true, false},
                    {"ANAL", &v.anal, true, false}, {"LEMM", &v.lemm, true, false}, {"SENS", &v.sens, false, false},
                    {"FEAT", &v.feat, true, false}, {"GENX", &v.genx, false, false}, {"REVX", &v.revx, false, false}};
    for (uint32_t s = 0; s < sectionCount; ++s) {
      const uint8_t* e = b + kHeaderSize + static_cast<size_t>(s) * kTableEntry;
      const uint64_t off = rd64(e + 8), len = rd64(e + 16);
      const std::string tag(reinterpret_cast<const char*>(e), 4);
      if (off % 8 != 0) return fail("section " + std::to_string(s) + " is not 8-byte aligned");
      if (off < tableEnd) return fail("section " + std::to_string(s) + " overlaps the header or section table");
      if (off > v.size || len > v.size - off) return fail("section " + std::to_string(s) + " runs past the end");
      for (Want& w : wants) {
        if (std::memcmp(e, w.tag, 4) != 0) continue;
        if (w.seen) return fail("duplicate section " + tag);
        w.seen = true;
        w.span->p = b + off;
        w.span->len = static_cast<size_t>(len);
      }
      // Unknown tags are skipped (a later minor version may add sections).
    }
    for (const Want& w : wants)
      if (w.required && !w.seen) return fail(std::string("missing section ") + w.tag);

    // ---- per-section counts ----
    // STRS: every string NUL-terminated, so the blob must end in NUL; then a scan from any in-range offset stops.
    if (v.strs.len == 0 || v.strs.p[v.strs.len - 1] != 0) return fail("STRS does not end with a NUL byte");
    // A section `u32 n` then n x (recSize + perCountExtra) bytes plus fixedExtra: read n, check it fits.
    auto counted = [](const Span& sp, uint64_t recSize, uint64_t perCountExtra, uint64_t fixedExtra,
                      uint32_t& n) -> bool {
      if (sp.len < 4) return false;
      n = rd32(sp.p);
      return 4 + static_cast<uint64_t>(n) * (recSize + perCountExtra) + fixedExtra <= sp.len;
    };
    // KEYS: n, key_off[n], anal_start[n+1]
    if (!counted(v.keys, 4, 4, 4, v.nKeys)) return fail("KEYS counts exceed the section");
    if (!counted(v.anal, kAnalRec, 0, 0, v.nAnal)) return fail("ANAL count exceeds the section");
    if (!counted(v.lemm, kLemmRec, 0, 0, v.nLemmas)) return fail("LEMM count exceeds the section");
    if (!counted(v.feat, 4, 0, 0, v.nFeat)) return fail("FEAT count exceeds the section");
    if (v.nFeat > 65536u) return fail("FEAT has more entries than a u16 feat_id can address");
    if (v.sens.p && !counted(v.sens, kSensRec, 0, 0, v.nSenses)) return fail("SENS count exceeds the section");
    if (v.genx.p && !counted(v.genx, kGenxRec, 0, 0, v.nCells)) return fail("GENX count exceeds the section");
    if (v.revx.p) {
      // REVX: n_kw, kw_off[n_kw], cand_start[n_kw+1], then cand_start[n_kw] candidates of 8 bytes
      if (!counted(v.revx, 4, 4, 4, v.nKw)) return fail("REVX counts exceed the section");
      const uint64_t candBase = 4 + static_cast<uint64_t>(v.nKw) * 8 + 4;
      v.nCand = rd32(v.revx.p + 4 + static_cast<size_t>(v.nKw) * 8);
      if (candBase + static_cast<uint64_t>(v.nCand) * kCandRec > v.revx.len)
        return fail("REVX candidates exceed the section");
    }
    // FEAT is small (a few thousand words): note whether it is sorted so featId can binary-search.
    v.featSorted = true;
    for (uint32_t i = 1; i < v.nFeat && v.featSorted; ++i)
      if (rd32(v.feat.p + 4 + 4 * static_cast<size_t>(i - 1)) >= rd32(v.feat.p + 4 + 4 * static_cast<size_t>(i)))
        v.featSorted = false;
    return Result<Lexicon>(std::move(lx));
  } catch (...) {
    return Result<Lexicon>(ErrorCode::Internal, "lexicon open failed unexpectedly", kCorruptHint);
  }
}

Result<void> Lexicon::verifySha256() const {
  if (!v_.base) return Result<void>(ErrorCode::LexiconMissing, "no lexicon is open", kMissingHint);
  Sha256 h;
  h.update(v_.base + kHeaderSize, v_.size - kHeaderSize);
  uint8_t digest[32];
  h.finish(digest);
  if (std::memcmp(digest, v_.base + kOffSha, 32) != 0)
    return corrupt(file_.path(), "SHA-256 of the body does not match the header");
  return Result<void>();
}

// ---- strings ----

bool Lexicon::strChecked(uint32_t off, std::string_view& s) const {
  if (off >= v_.strs.len) return false;   // also covers an absent STRS (len 0)
  const char* p = reinterpret_cast<const char*>(v_.strs.p) + off;
  // The blob ends in NUL (checked at open), so memchr always finds one inside STRS.
  const void* nul = std::memchr(p, 0, v_.strs.len - off);
  s = std::string_view(p, static_cast<size_t>(static_cast<const char*>(nul) - p));
  return true;
}

std::string_view Lexicon::str(uint32_t off) const {
  std::string_view s;
  strChecked(off, s);
  return s;
}

std::string_view Lexicon::keyAt(uint32_t i) const { return str(rd32(v_.keys.p + 4 + 4 * static_cast<size_t>(i))); }

std::string_view Lexicon::kwAt(uint32_t i) const { return str(rd32(v_.revx.p + 4 + 4 * static_cast<size_t>(i))); }

uint32_t Lexicon::keyLowerBound(std::string_view key) const {
  uint32_t lo = 0, hi = v_.nKeys;
  while (lo < hi) {
    const uint32_t mid = lo + (hi - lo) / 2;
    if (keyAt(mid).compare(key) < 0) lo = mid + 1; else hi = mid;
  }
  return lo;
}

// ---- queries ----

std::string_view Lexicon::lang() const { return v_.lang; }

std::string_view Lexicon::notice() const {
  size_t n = v_.note.len;
  while (n > 0 && v_.note.p[n - 1] == 0) --n;   // tolerate NUL padding
  return std::string_view(reinterpret_cast<const char*>(v_.note.p), n);
}

uint32_t Lexicon::lemmaCount() const { return v_.nLemmas; }

Stats Lexicon::stats() const {
  Stats s;
  s.major = v_.major; s.minor = v_.minor; s.fileSize = v_.size;
  s.keys = v_.nKeys; s.analyses = v_.nAnal; s.lemmas = v_.nLemmas; s.senses = v_.nSenses; s.features = v_.nFeat;
  s.cells = v_.nCells; s.keywords = v_.nKw; s.candidates = v_.nCand;
  s.hasSenses = v_.sens.p != nullptr; s.hasCells = v_.genx.p != nullptr; s.hasReverse = v_.revx.p != nullptr;
  return s;
}

bool Lexicon::lookup(std::string_view key, std::vector<Analysis>& out) const {
  if (v_.nKeys == 0) return false;
  const uint32_t i = keyLowerBound(key);
  if (i >= v_.nKeys || keyAt(i) != key) return false;
  const uint8_t* starts = v_.keys.p + 4 + 4 * static_cast<size_t>(v_.nKeys);
  const uint32_t a = rd32(starts + 4 * static_cast<size_t>(i)), z = rd32(starts + 4 * static_cast<size_t>(i) + 4);
  if (a > z || z > v_.nAnal) return false;   // damaged range
  for (uint32_t k = a; k < z; ++k) {
    const uint8_t* r = v_.anal.p + 4 + kAnalRec * static_cast<size_t>(k);
    Analysis an;
    an.lemma = rd32(r);
    an.feat = rd16(r + 4);
    an.flags = rd16(r + 6);
    an.display = str(rd32(r + 8));
    out.push_back(an);
  }
  return true;
}

void Lexicon::prefix(std::string_view keyPrefix, size_t max, std::vector<std::string_view>& out) const {
  size_t added = 0;
  for (uint32_t i = keyLowerBound(keyPrefix); i < v_.nKeys && added < max; ++i) {
    const std::string_view k = keyAt(i);
    if (k.substr(0, keyPrefix.size()) != keyPrefix) break;
    out.push_back(k);
    ++added;
  }
}

Lemma Lexicon::lemma(uint32_t id) const {
  Lemma l;
  if (id >= v_.nLemmas) return l;
  const uint8_t* r = v_.lemm.p + 4 + kLemmRec * static_cast<size_t>(id);
  l.id = id;
  l.head = str(rd32(r + 0));
  l.key = str(rd32(r + 4));
  l.pos = r[8]; l.cls = r[9]; l.gender = r[10]; l.tier = r[11];
  l.freqRank = rd16(r + 12);
  l.whitFreq = r[14]; l.tierSource = r[15];
  const uint32_t emojiOff = rd32(r + 16);
  if (emojiOff != 0) l.emoji = str(emojiOff);   // 0 = none
  l.glossEn = str(rd32(r + 20));
  l.glossEs = str(rd32(r + 24));
  l.flags = rd16(r + 34);
  l.principal = str(rd32(r + 44));
  return l;
}

void Lexicon::senses(uint32_t id, std::vector<Sense>& out) const {
  if (id >= v_.nLemmas || !v_.sens.p) return;
  const uint8_t* r = v_.lemm.p + 4 + kLemmRec * static_cast<size_t>(id);
  const uint64_t start = rd32(r + 28), count = rd16(r + 32);
  if (start + count > v_.nSenses) return;
  for (uint64_t k = start; k < start + count; ++k) {
    const uint8_t* s = v_.sens.p + 4 + kSensRec * static_cast<size_t>(k);
    Sense se;
    se.glossEn = str(rd32(s));
    se.glossEs = str(rd32(s + 4));
    se.keywords = str(rd32(s + 8));
    se.tags = rd16(s + 12);
    se.rank = rd16(s + 14);
    out.push_back(se);
  }
}

bool Lexicon::generate(uint32_t lemmaId, uint32_t packedFeatures, std::string_view& form) const {
  if (lemmaId >= v_.nLemmas || !v_.genx.p) return false;
  bool found = false;
  const uint16_t fid = featId(packedFeatures, found);
  if (!found) return false;
  const uint8_t* r = v_.lemm.p + 4 + kLemmRec * static_cast<size_t>(lemmaId);
  const uint64_t start = rd32(r + 36), count = rd16(r + 40);
  if (start + count > v_.nCells) return false;
  uint64_t lo = start, hi = start + count;   // lower_bound on feat_id
  while (lo < hi) {
    const uint64_t mid = lo + (hi - lo) / 2;
    if (rd16(v_.genx.p + 4 + kGenxRec * static_cast<size_t>(mid)) < fid) lo = mid + 1; else hi = mid;
  }
  if (lo == start + count) return false;
  const uint8_t* c = v_.genx.p + 4 + kGenxRec * static_cast<size_t>(lo);
  if (rd16(c) != fid) return false;
  return strChecked(rd32(c + 4), form);
}

void Lexicon::cells(uint32_t lemmaId, std::vector<std::pair<uint32_t, std::string_view>>& out) const {
  if (lemmaId >= v_.nLemmas || !v_.genx.p) return;
  const uint8_t* r = v_.lemm.p + 4 + kLemmRec * static_cast<size_t>(lemmaId);
  const uint64_t start = rd32(r + 36), count = rd16(r + 40);
  if (start + count > v_.nCells) return;
  for (uint64_t k = start; k < start + count; ++k) {
    const uint8_t* c = v_.genx.p + 4 + kGenxRec * static_cast<size_t>(k);
    out.emplace_back(feature(rd16(c)), str(rd32(c + 4)));
  }
}

void Lexicon::reverse(std::string_view keyword, std::vector<Candidate>& out) const {
  if (!v_.revx.p || v_.nKw == 0) return;
  uint32_t lo = 0, hi = v_.nKw;
  while (lo < hi) {
    const uint32_t mid = lo + (hi - lo) / 2;
    if (kwAt(mid).compare(keyword) < 0) lo = mid + 1; else hi = mid;
  }
  if (lo >= v_.nKw || kwAt(lo) != keyword) return;
  const uint8_t* starts = v_.revx.p + 4 + 4 * static_cast<size_t>(v_.nKw);
  const uint32_t a = rd32(starts + 4 * static_cast<size_t>(lo)), z = rd32(starts + 4 * static_cast<size_t>(lo) + 4);
  if (a > z || z > v_.nCand) return;
  const uint8_t* cand = v_.revx.p + 4 + 8 * static_cast<size_t>(v_.nKw) + 4;
  const size_t first = out.size();
  for (uint32_t k = a; k < z; ++k) {
    const uint8_t* c = cand + kCandRec * static_cast<size_t>(k);
    Candidate cd;
    cd.lemma = rd32(c);
    cd.sense = rd16(c + 4);
    cd.score = c[6];
    cd.pos = c[7];
    out.push_back(cd);
  }
  // The packer already writes this order (§5.2); sorting the appended range keeps it deterministic regardless.
  std::sort(out.begin() + static_cast<std::ptrdiff_t>(first), out.end(), [](const Candidate& x, const Candidate& y) {
    if (x.score != y.score) return x.score > y.score;
    if (x.lemma != y.lemma) return x.lemma < y.lemma;
    return x.sense < y.sense;
  });
}

uint32_t Lexicon::feature(uint16_t fid) const {
  if (fid >= v_.nFeat) return 0;
  return rd32(v_.feat.p + 4 + 4 * static_cast<size_t>(fid));
}

uint16_t Lexicon::featId(uint32_t packed, bool& found) const {
  found = false;
  if (v_.featSorted) {
    uint32_t lo = 0, hi = v_.nFeat;
    while (lo < hi) {
      const uint32_t mid = lo + (hi - lo) / 2;
      if (rd32(v_.feat.p + 4 + 4 * static_cast<size_t>(mid)) < packed) lo = mid + 1; else hi = mid;
    }
    if (lo < v_.nFeat && rd32(v_.feat.p + 4 + 4 * static_cast<size_t>(lo)) == packed) {
      found = true;
      return static_cast<uint16_t>(lo);
    }
    return 0;
  }
  for (uint32_t i = 0; i < v_.nFeat; ++i)
    if (rd32(v_.feat.p + 4 + 4 * static_cast<size_t>(i)) == packed) {
      found = true;
      return static_cast<uint16_t>(i);
    }
  return 0;
}

}  // namespace vp::lex
