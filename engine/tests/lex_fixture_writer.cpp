// Test-only reference encoder for `.vpl` (DESIGN.md §5). See lex_fixture_writer.h.
//
// File layout written here (all integers little-endian, all offsets from the start of the file):
//   [0,256)   header
//               +0   char[4]  magic "VPLX"
//               +4   u16      major (1)
//               +6   u16      minor
//               +8   char[8]  lang, NUL-padded ("la", "grc", "en", "es")
//               +16  u32      section_count
//               +20  u64      file_size (unaligned: the header fields are packed in the order of §5)
//               +28  u8[32]   SHA-256 of bytes [256, file_size)
//               +60  zeros up to 256
//   [256, 256 + 24 * section_count)  section table, one entry per section in file order:
//               +0 char[4] tag | +4 u32 reserved (0) | +8 u64 offset | +16 u64 length (unpadded)
//   sections in the order NOTE STRS KEYS ANAL LEMM SENS FEAT GENX REVX (SENS GENX REVX absent when morphology-only),
//   each starting on an 8-byte boundary; the gap before it is zero-filled. The file ends right after the last
//   section's bytes (no tail padding).
// STRS convention of this encoder (recommended to the Python packer so the bytes can match exactly): one NUL at
// offset 0 (the empty string, so "none" fields are offset 0), then every distinct non-empty string sorted bytewise,
// each NUL-terminated.
#include "lex_fixture_writer.h"

#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>

#include "vp/features.h"
#include "vp/sha256.h"
#include "vp/text.h"

namespace vp::lexfix {

namespace {

using Bytes = std::vector<uint8_t>;

void put8(Bytes& b, uint8_t v) { b.push_back(v); }
void put16(Bytes& b, uint16_t v) { b.push_back(static_cast<uint8_t>(v)); b.push_back(static_cast<uint8_t>(v >> 8)); }
void put32(Bytes& b, uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(v >> (8 * i))); }
void set64(Bytes& b, size_t at, uint64_t v) { for (int i = 0; i < 8; ++i) b[at + i] = static_cast<uint8_t>(v >> (8 * i)); }

struct Strings {
  std::set<std::string> all;
  std::map<std::string, uint32_t> off;
  void add(const std::string& s) { if (!s.empty()) all.insert(s); }
  Bytes finish() {   // NUL at 0, then sorted strings
    Bytes b{0};
    for (const std::string& s : all) {
      off[s] = static_cast<uint32_t>(b.size());
      b.insert(b.end(), s.begin(), s.end());
      b.push_back(0);
    }
    return b;
  }
  uint32_t operator()(const std::string& s) const { return s.empty() ? 0 : off.at(s); }
};

}  // namespace

std::vector<uint8_t> build(const LexiconIn& in, std::vector<SectionInfo>* layout) {
  // ---- FEAT: every packed word used by ANAL or GENX, sorted ascending; feat_id = index ----
  std::vector<uint32_t> feats;
  for (const AnalysisIn& a : in.analyses) feats.push_back(a.feat);
  if (!in.morphologyOnly)
    for (const LemmaIn& l : in.lemmas)
      for (const CellIn& c : l.cells) feats.push_back(c.feat);
  std::sort(feats.begin(), feats.end());
  feats.erase(std::unique(feats.begin(), feats.end()), feats.end());
  if (feats.size() > 65536) throw std::runtime_error("too many feature words");
  auto fid = [&](uint32_t packed) {
    return static_cast<uint16_t>(std::lower_bound(feats.begin(), feats.end(), packed) - feats.begin());
  };

  // ---- ordering: analyses grouped by key (keys bytewise ascending, stable inside a key) ----
  std::vector<AnalysisIn> anal = in.analyses;
  std::stable_sort(anal.begin(), anal.end(), [](const AnalysisIn& x, const AnalysisIn& y) { return x.key < y.key; });
  std::vector<CandidateIn> cand = in.candidates;
  std::sort(cand.begin(), cand.end(), [](const CandidateIn& x, const CandidateIn& y) {
    if (x.keyword != y.keyword) return x.keyword < y.keyword;
    if (x.score != y.score) return x.score > y.score;
    if (x.lemma != y.lemma) return x.lemma < y.lemma;
    return x.sense < y.sense;
  });

  // ---- STRS ----
  Strings S;
  for (const AnalysisIn& a : anal) { S.add(a.key); S.add(a.display); }
  for (const LemmaIn& l : in.lemmas) {
    S.add(l.head); S.add(l.key); S.add(l.emoji); S.add(l.glossEn); S.add(l.glossEs); S.add(l.principal);
    if (!in.morphologyOnly) {
      for (const SenseIn& s : l.senses) { S.add(s.glossEn); S.add(s.glossEs); S.add(s.keywords); }
      for (const CellIn& c : l.cells) S.add(c.form);
    }
  }
  if (!in.morphologyOnly)
    for (const CandidateIn& c : cand) S.add(c.keyword);
  const Bytes strs = S.finish();

  // ---- KEYS: u32 n_keys | n_keys x u32 key_off | (n_keys+1) x u32 anal_start ----
  std::vector<std::string> keys;
  std::vector<uint32_t> analStart;
  for (size_t i = 0; i < anal.size(); ++i)
    if (i == 0 || anal[i].key != anal[i - 1].key) { keys.push_back(anal[i].key); analStart.push_back(static_cast<uint32_t>(i)); }
  analStart.push_back(static_cast<uint32_t>(anal.size()));
  Bytes keysB;
  put32(keysB, static_cast<uint32_t>(keys.size()));
  for (const std::string& k : keys) put32(keysB, S(k));
  for (uint32_t a : analStart) put32(keysB, a);

  // ---- ANAL: u32 n | n x {u32 lemma_id @0, u16 feat_id @4, u16 flags @6, u32 display_off @8} (12 bytes) ----
  Bytes analB;
  put32(analB, static_cast<uint32_t>(anal.size()));
  for (const AnalysisIn& a : anal) { put32(analB, a.lemma); put16(analB, fid(a.feat)); put16(analB, a.flags); put32(analB, S(a.display)); }

  // ---- LEMM (48 bytes each), SENS (16 bytes each), GENX (8 bytes each) ----
  Bytes lemmB, sensB, genxB;
  uint32_t nSenses = 0, nCells = 0;
  Bytes sensRecs, genxRecs;
  put32(lemmB, static_cast<uint32_t>(in.lemmas.size()));
  for (const LemmaIn& l : in.lemmas) {
    const uint32_t sensesStart = nSenses, genStart = nCells;
    const uint16_t sensesCount = in.morphologyOnly ? 0 : static_cast<uint16_t>(l.senses.size());
    const uint16_t genCount = in.morphologyOnly ? 0 : static_cast<uint16_t>(l.cells.size());
    put32(lemmB, S(l.head));        // +0  head_off
    put32(lemmB, S(l.key));         // +4  key_off
    put8(lemmB, l.pos);             // +8  pos
    put8(lemmB, l.cls);             // +9  cls
    put8(lemmB, l.gender);          // +10 gender
    put8(lemmB, l.tier);            // +11 tier
    put16(lemmB, l.freqRank);       // +12 freq_rank
    put8(lemmB, l.whitFreq);        // +14 whit_freq
    put8(lemmB, l.tierSource);      // +15 tier_source
    put32(lemmB, S(l.emoji));       // +16 emoji_off (0 none)
    put32(lemmB, S(l.glossEn));     // +20 gloss_en_off
    put32(lemmB, S(l.glossEs));     // +24 gloss_es_off
    put32(lemmB, sensesStart);      // +28 senses_start
    put16(lemmB, sensesCount);      // +32 senses_count
    put16(lemmB, l.flags);          // +34 flags
    put32(lemmB, genStart);         // +36 gen_start
    put16(lemmB, genCount);         // +40 gen_count
    put16(lemmB, l.principalCount); // +42 principal_off_count
    put32(lemmB, S(l.principal));   // +44 principal_off
    if (in.morphologyOnly) continue;
    for (const SenseIn& s : l.senses) {
      put32(sensRecs, S(s.glossEn));    // +0  gloss_en_off
      put32(sensRecs, S(s.glossEs));    // +4  gloss_es_off
      put32(sensRecs, S(s.keywords));   // +8  keywords_off
      put16(sensRecs, s.tags);          // +12 tags
      put16(sensRecs, s.rank);          // +14 sense_rank
      ++nSenses;
    }
    std::vector<CellIn> cells = l.cells;
    std::stable_sort(cells.begin(), cells.end(), [&](const CellIn& x, const CellIn& y) { return fid(x.feat) < fid(y.feat); });
    for (const CellIn& c : cells) {
      put16(genxRecs, fid(c.feat));     // +0 feat_id
      put16(genxRecs, 0);               // +2 reserved
      put32(genxRecs, S(c.form));       // +4 form_off
      ++nCells;
    }
  }
  put32(sensB, nSenses); sensB.insert(sensB.end(), sensRecs.begin(), sensRecs.end());
  put32(genxB, nCells); genxB.insert(genxB.end(), genxRecs.begin(), genxRecs.end());

  // ---- FEAT: u32 n | n x u32 packed ----
  Bytes featB;
  put32(featB, static_cast<uint32_t>(feats.size()));
  for (uint32_t f : feats) put32(featB, f);

  // ---- REVX: u32 n_kw | n_kw x u32 kw_off | (n_kw+1) x u32 cand_start | CAND n x {u32 lemma, u16 sense, u8 score, u8 pos} ----
  Bytes revxB;
  {
    std::vector<std::string> kws;
    std::vector<uint32_t> starts;
    for (size_t i = 0; i < cand.size(); ++i)
      if (i == 0 || cand[i].keyword != cand[i - 1].keyword) { kws.push_back(cand[i].keyword); starts.push_back(static_cast<uint32_t>(i)); }
    starts.push_back(static_cast<uint32_t>(cand.size()));
    put32(revxB, static_cast<uint32_t>(kws.size()));
    for (const std::string& k : kws) put32(revxB, S(k));
    for (uint32_t s : starts) put32(revxB, s);
    for (const CandidateIn& c : cand) { put32(revxB, c.lemma); put16(revxB, c.sense); put8(revxB, c.score); put8(revxB, c.pos); }
  }

  // ---- assemble ----
  const Bytes noteB(in.notice.begin(), in.notice.end());
  std::vector<std::pair<const char*, const Bytes*>> sections = {
      {"NOTE", &noteB}, {"STRS", &strs}, {"KEYS", &keysB}, {"ANAL", &analB}, {"LEMM", &lemmB}};
  if (!in.morphologyOnly) sections.push_back({"SENS", &sensB});
  sections.push_back({"FEAT", &featB});
  if (!in.morphologyOnly) { sections.push_back({"GENX", &genxB}); sections.push_back({"REVX", &revxB}); }

  Bytes f(256, 0);
  std::copy_n("VPLX", 4, f.begin());
  f[4] = static_cast<uint8_t>(in.major); f[5] = static_cast<uint8_t>(in.major >> 8);
  f[6] = static_cast<uint8_t>(in.minor); f[7] = static_cast<uint8_t>(in.minor >> 8);
  for (size_t i = 0; i < in.lang.size() && i < 8; ++i) f[8 + i] = static_cast<uint8_t>(in.lang[i]);
  const uint32_t count = static_cast<uint32_t>(sections.size());
  for (int i = 0; i < 4; ++i) f[16 + i] = static_cast<uint8_t>(count >> (8 * i));
  const size_t tableAt = f.size();
  f.resize(tableAt + 24 * sections.size(), 0);
  if (layout) layout->clear();
  for (size_t s = 0; s < sections.size(); ++s) {
    while (f.size() % 8 != 0) f.push_back(0);
    const uint64_t off = f.size(), len = sections[s].second->size();
    f.insert(f.end(), sections[s].second->begin(), sections[s].second->end());
    const size_t e = tableAt + 24 * s;
    std::copy_n(sections[s].first, 4, f.begin() + static_cast<std::ptrdiff_t>(e));   // tag; reserved stays 0
    set64(f, e + 8, off);
    set64(f, e + 16, len);
    if (layout) layout->push_back({sections[s].first, off, len});
  }
  set64(f, 20, f.size());
  Sha256 h;
  h.update(f.data() + 256, f.size() - 256);
  h.finish(f.data() + 28);
  return f;
}

// ---------------------------------------------------------------------------------------------------------------
// Fixture contents. Feature helper: pos, case, number, gender, person, tense, mood, voice, degree, extra.
namespace {
using namespace vp::feat;
uint32_t PK(uint8_t pos, uint8_t cs = 0, uint8_t num = 0, uint8_t gen = 0, uint8_t pers = 0, uint8_t tense = 0,
           uint8_t mood = 0, uint8_t voice = 0, uint8_t deg = 0, uint8_t extra = 0) {
  Features f;
  f.pos = pos; f.case_ = cs; f.number = num; f.gender = gen; f.person = pers; f.tense = tense; f.mood = mood;
  f.voice = voice; f.degree = deg; f.extra = extra;
  return pack(f);
}

// Adds a cell to the lemma and the matching analysis (key = keyFn(form)).
void addForm(LexiconIn& lx, uint32_t lemma, uint32_t feat, const std::string& form, uint16_t flags,
             std::string (*keyFn)(std::string_view), bool cell = true) {
  if (cell) lx.lemmas[lemma].cells.push_back({feat, form});
  lx.analyses.push_back({keyFn(form), lemma, feat, flags, form});
}
std::string latinKey(std::string_view s) { return vp::text::latin_key(s); }
std::string greekKey(std::string_view s) { return vp::text::greek_key(s); }
std::string englishKey(std::string_view s) { return vp::text::en_key(s); }

const uint8_t kCases[6] = {Nom, Gen, Dat, Acc, Abl, Voc};
}  // namespace

LexiconIn latinFixture() {
  LexiconIn lx;
  lx.lang = "la";
  lx.minor = 0;
  lx.notice = "Test fixture for engine/lex. Forms and glosses written for the test suite.";
  auto lemma = [&](const char* head, Pos pos, uint8_t cls, uint8_t gender, const char* principal, const char* en,
                   const char* es) {
    LemmaIn l;
    l.head = head; l.key = vp::text::latin_key(head); l.pos = pos; l.cls = cls; l.gender = gender;
    l.principal = principal; l.principalCount = 1; l.glossEn = en; l.glossEs = es;
    lx.lemmas.push_back(l);
    return static_cast<uint32_t>(lx.lemmas.size() - 1);
  };
  const uint32_t puella = lemma("puella", Noun, 1, F, "puellae, f.", "girl", "niña");
  const uint32_t amo = lemma("amō", Verb, 1, GenNone, "amō, amāre, amāvī, amātum", "to love", "amar");
  const uint32_t bonus = lemma("bonus", Adj, 1, GenNone, "bonus, bona, bonum", "good", "bueno");
  const uint32_t virgo = lemma("virgō", Noun, 3, F, "virginis, f.", "maiden", "doncella");
  const uint32_t diligo = lemma("dīligō", Verb, 3, GenNone, "dīligō, dīligere, dīlēxī, dīlēctum", "to esteem", "apreciar");
  const uint32_t amor = lemma("amor", Noun, 3, M, "amōris, m.", "love", "amor");
  for (LemmaIn& l : lx.lemmas) { l.flags = 0x80 /* has_table */; l.tier = 1; l.tierSource = 2; l.whitFreq = 'A'; }
  lx.lemmas[puella].emoji = "\xF0\x9F\x91\xA7";   // U+1F467 girl
  lx.lemmas[puella].freqRank = 412;
  lx.lemmas[amo].emoji = "\xE2\x9D\xA4";           // U+2764 heart
  lx.lemmas[amo].freqRank = 128;
  lx.lemmas[virgo].tier = 2; lx.lemmas[virgo].tierSource = 1; lx.lemmas[virgo].whitFreq = 'B';
  lx.lemmas[diligo].tier = 2; lx.lemmas[diligo].tierSource = 1;

  // puella: 12 case/number cells and the archaic genitive in -āī (Alternative).
  const char* puellaSg[6] = {"puella", "puellae", "puellae", "puellam", "puellā", "puella"};
  const char* puellaPl[6] = {"puellae", "puellārum", "puellīs", "puellās", "puellīs", "puellae"};
  for (int c = 0; c < 6; ++c) addForm(lx, puella, PK(Noun, kCases[c], Sg), puellaSg[c], 1, latinKey);
  for (int c = 0; c < 6; ++c) addForm(lx, puella, PK(Noun, kCases[c], Pl), puellaPl[c], 1, latinKey);
  addForm(lx, puella, PK(Noun, Gen, Sg, 0, 0, 0, 0, 0, 0, Alternative), "puellāī", 1 | 64, latinKey);
  lx.lemmas[puella].senses = {{"girl", "niña", "girl", 0, 1},
                              {"sweetheart, beloved", "amada, novia", "sweetheart beloved girl", 1u << 5, 2}};

  // amō: 42 cells.
  const char* tenses[5][6] = {{"amō", "amās", "amat", "amāmus", "amātis", "amant"},
                              {"amābam", "amābās", "amābat", "amābāmus", "amābātis", "amābant"},
                              {"amābō", "amābis", "amābit", "amābimus", "amābitis", "amābunt"},
                              {"amāvī", "amāvistī", "amāvit", "amāvimus", "amāvistis", "amāvērunt"},
                              {"amor", "amāris", "amātur", "amāmur", "amāminī", "amantur"}};
  const uint8_t tenseOf[5] = {Present, Imperfect, Future, Perfect, Present};
  const uint8_t voiceOf[5] = {Active, Active, Active, Active, Passive};
  for (int t = 0; t < 5; ++t)
    for (int k = 0; k < 6; ++k)
      addForm(lx, amo, PK(Verb, 0, k < 3 ? Sg : Pl, 0, static_cast<uint8_t>(k % 3 + 1), tenseOf[t], Indicative, voiceOf[t]),
              tenses[t][k], 1, latinKey);
  const char* subj[6] = {"amem", "amēs", "amet", "amēmus", "amētis", "ament"};
  for (int k = 0; k < 6; ++k)
    addForm(lx, amo, PK(Verb, 0, k < 3 ? Sg : Pl, 0, static_cast<uint8_t>(k % 3 + 1), Present, Subjunctive, Active),
            subj[k], 1, latinKey);
  addForm(lx, amo, PK(Verb, 0, 0, 0, 0, Present, Infinitive, Active), "amāre", 1, latinKey);
  addForm(lx, amo, PK(Verb, 0, 0, 0, 0, Present, Infinitive, Passive), "amārī", 1, latinKey);
  addForm(lx, amo, PK(Verb, 0, 0, 0, 0, Perfect, Infinitive, Active), "amāvisse", 1, latinKey);
  addForm(lx, amo, PK(Verb, 0, Sg, 0, P2, Present, Imperative, Active), "amā", 1, latinKey);
  addForm(lx, amo, PK(Verb, 0, Pl, 0, P2, Present, Imperative, Active), "amāte", 1, latinKey);
  addForm(lx, amo, PK(Verb, Nom, Sg, MFN, 0, Present, ParticipleMood, Active), "amāns", 1, latinKey);
  // Form-of page analysis (syncopated perfect), not a table cell.
  addForm(lx, amo, PK(Verb, 0, Sg, 0, P2, Perfect, Indicative, Active), "amāstī", 2 | 8, latinKey, false);
  lx.lemmas[amo].senses = {{"to love", "amar", "love", 1, 1}, {"to like, be fond of", "gustar, apreciar", "like fond", 1, 2}};

  // bonus: 36 cells (3 genders x 6 cases x 2 numbers).
  const char* bon[3][2][6] = {
      {{"bonus", "bonī", "bonō", "bonum", "bonō", "bone"}, {"bonī", "bonōrum", "bonīs", "bonōs", "bonīs", "bonī"}},
      {{"bona", "bonae", "bonae", "bonam", "bonā", "bona"}, {"bonae", "bonārum", "bonīs", "bonās", "bonīs", "bonae"}},
      {{"bonum", "bonī", "bonō", "bonum", "bonō", "bonum"}, {"bona", "bonōrum", "bonīs", "bona", "bonīs", "bona"}}};
  const uint8_t gens[3] = {M, F, N};
  for (int g = 0; g < 3; ++g)
    for (int n = 0; n < 2; ++n)
      for (int c = 0; c < 6; ++c) addForm(lx, bonus, PK(Adj, kCases[c], n == 0 ? Sg : Pl, gens[g]), bon[g][n][c], 1, latinKey);
  lx.lemmas[bonus].senses = {{"good", "bueno", "good", 0, 1}};

  addForm(lx, virgo, PK(Noun, Nom, Sg), "virgō", 1, latinKey);
  addForm(lx, virgo, PK(Noun, Gen, Sg), "virginis", 1, latinKey);
  lx.lemmas[virgo].senses = {{"maiden, young woman", "doncella", "maiden girl", 0, 1}};
  addForm(lx, diligo, PK(Verb, 0, Sg, 0, P1, Present, Indicative, Active), "dīligō", 1, latinKey);
  lx.lemmas[diligo].senses = {{"to esteem, to love", "apreciar, amar", "esteem love", 1, 1}};
  addForm(lx, amor, PK(Noun, Nom, Sg), "amor", 1, latinKey);
  addForm(lx, amor, PK(Noun, Gen, Sg), "amōris", 1, latinKey);
  lx.lemmas[amor].senses = {{"love", "amor", "love", 0, 1}};

  // Reverse index, deliberately unsorted (the encoder sorts: keyword, score desc, lemma asc, sense asc).
  lx.candidates = {{"love", amor, 0, 95, Noun},     {"girl", virgo, 0, 60, Noun},   {"love", amo, 0, 175, Verb},
                   {"girl", puella, 1, 60, Noun},   {"love", diligo, 0, 95, Verb},  {"girl", puella, 0, 160, Noun},
                   {"good", bonus, 0, 160, Adj},    {"maiden", virgo, 0, 160, Noun}, {"sweetheart", puella, 1, 95, Noun},
                   {"beloved", puella, 1, 60, Noun}, {"esteem", diligo, 0, 160, Verb}, {"like", amo, 1, 95, Verb},
                   {"fond", amo, 1, 60, Verb}};
  return lx;
}

LexiconIn greekFixture() {
  LexiconIn lx;
  lx.lang = "grc";
  lx.notice = "Test fixture for engine/lex (Ancient Greek). Forms written for the test suite.";
  LemmaIn l;
  l.head = "ἄνθρωπος"; l.key = vp::text::greek_key(l.head); l.pos = Noun; l.cls = 2; l.gender = M; l.tier = 1;
  l.tierSource = 2; l.glossEn = "human being, person"; l.glossEs = "ser humano, persona"; l.principal = "ἀνθρώπου, ὁ";
  l.principalCount = 1; l.flags = 0x80;   // has_table
  l.senses = {{"human being, person", "ser humano, persona", "human person man", 0, 1}};
  lx.lemmas.push_back(l);
  const char* sg[5] = {"ἄνθρωπος", "ἀνθρώπου", "ἀνθρώπῳ", "ἄνθρωπον", "ἄνθρωπε"};
  const char* du[5] = {"ἀνθρώπω", "ἀνθρώποιν", "ἀνθρώποιν", "ἀνθρώπω", "ἀνθρώπω"};
  const char* pl[5] = {"ἄνθρωποι", "ἀνθρώπων", "ἀνθρώποις", "ἀνθρώπους", "ἄνθρωποι"};
  const uint8_t cases[5] = {Nom, Gen, Dat, Acc, Voc};
  for (int c = 0; c < 5; ++c) addForm(lx, 0, PK(Noun, cases[c], Sg, 0, 0, 0, 0, 0, 0, Attic), sg[c], 1, greekKey);
  for (int c = 0; c < 5; ++c) addForm(lx, 0, PK(Noun, cases[c], Dual, 0, 0, 0, 0, 0, 0, Attic), du[c], 1, greekKey);
  for (int c = 0; c < 5; ++c) addForm(lx, 0, PK(Noun, cases[c], Pl, 0, 0, 0, 0, 0, 0, Attic), pl[c], 1, greekKey);
  // Epic genitive and Ionic/Epic dative plural: no Attic bit, ANAL flag bit4 (non-Attic).
  addForm(lx, 0, PK(Noun, Gen, Sg), "ἀνθρώποιο", 1 | 16, greekKey);
  addForm(lx, 0, PK(Noun, Dat, Pl), "ἀνθρώποισι", 1 | 16, greekKey);
  lx.candidates = {{"human", 0, 0, 160, Noun}, {"person", 0, 0, 160, Noun}, {"man", 0, 0, 95, Noun}};
  return lx;
}

LexiconIn englishFixture() {
  LexiconIn lx;
  lx.lang = "en";
  lx.notice = "Test fixture for engine/lex (English morphology only).";
  lx.morphologyOnly = true;
  auto lemma = [&](const char* head, Pos pos) {
    LemmaIn l;
    l.head = head; l.key = vp::text::en_key(head); l.pos = pos;
    lx.lemmas.push_back(l);
    return static_cast<uint32_t>(lx.lemmas.size() - 1);
  };
  const uint32_t go = lemma("go", Verb), see = lemma("see", Verb), saw = lemma("saw", Noun), child = lemma("child", Noun);
  const uint32_t pres3 = PK(Verb, 0, Sg, 0, P3, Present), past = PK(Verb, 0, 0, 0, 0, Perfect);
  const uint32_t pastPart = PK(Verb, 0, 0, 0, 0, Perfect, ParticipleMood), presPart = PK(Verb, 0, 0, 0, 0, Present, ParticipleMood);
  const uint32_t inf = PK(Verb, 0, 0, 0, 0, 0, Infinitive), nsg = PK(Noun, 0, Sg), npl = PK(Noun, 0, Pl);
  addForm(lx, go, inf, "go", 1, englishKey, false);
  addForm(lx, go, pres3, "goes", 1, englishKey, false);
  addForm(lx, go, past, "went", 1, englishKey, false);
  addForm(lx, go, pastPart, "gone", 1, englishKey, false);
  addForm(lx, go, presPart, "going", 1, englishKey, false);
  addForm(lx, see, inf, "see", 1, englishKey, false);
  addForm(lx, see, pres3, "sees", 1, englishKey, false);
  addForm(lx, see, past, "saw", 1, englishKey, false);
  addForm(lx, see, pastPart, "seen", 1, englishKey, false);
  addForm(lx, saw, nsg, "saw", 1, englishKey, false);
  addForm(lx, saw, npl, "saws", 1, englishKey, false);
  addForm(lx, child, nsg, "child", 1, englishKey, false);
  addForm(lx, child, npl, "children", 1, englishKey, false);
  return lx;
}

LexiconIn syntheticFixture(uint32_t nLemmas) {
  LexiconIn lx;
  lx.lang = "la";
  lx.notice = "synthetic";
  static const char* suffix[8] = {"", "a", "ae", "am", "is", "os", "um", "ibus"};
  lx.lemmas.resize(nLemmas);
  lx.analyses.reserve(static_cast<size_t>(nLemmas) * 8);
  for (uint32_t i = 0; i < nLemmas; ++i) {
    std::string base(5, 'a');   // fixed width, so no key is a prefix-collision of another lemma's key
    for (uint32_t x = i, k = 0; k < 5; ++k, x /= 26) base[4 - k] = static_cast<char>('a' + x % 26);
    LemmaIn& l = lx.lemmas[i];
    l.head = base; l.key = base; l.pos = Noun; l.glossEn = "gloss " + base;
    l.senses = {{"gloss " + base, "", base, 0, 1}};
    for (int s = 0; s < 8; ++s) {
      const uint32_t feat = PK(Noun, static_cast<uint8_t>(s % 6 + 1), s < 4 ? Sg : Pl);
      l.cells.push_back({feat, base + suffix[s]});
      lx.analyses.push_back({base + suffix[s], i, feat, 1, base + suffix[s]});
    }
    if (i % 7 == 0) lx.candidates.push_back({"kw" + base.substr(0, 3), i, 0, static_cast<uint8_t>(i % 256), Noun});
  }
  return lx;
}

}  // namespace vp::lexfix
