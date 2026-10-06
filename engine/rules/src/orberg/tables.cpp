// Orbergise resources: simplify_la.tsv loader, tier / participle / lemma helpers, the vocabulary-swap search and small
// text helpers (internal.h). Deterministic: ties broken by tier, overlap, score, frequency rank, lemma id.
#include <algorithm>
#include <fstream>
#include <sstream>

#include "internal.h"
#include "vp/morph.h"
#include "vp/text.h"

namespace vp::orberg {

using namespace vp::feat;
using detail::kNone;

namespace {

std::vector<std::string> splitTabs(const std::string& line) {
  std::vector<std::string> c;
  size_t a = 0;
  for (;;) {
    const size_t b = line.find('\t', a);
    c.push_back(line.substr(a, b == std::string::npos ? std::string::npos : b - a));
    if (b == std::string::npos) break;
    a = b + 1;
  }
  return c;
}

std::vector<std::string> words(std::string_view s) {
  std::vector<std::string> out;
  size_t i = 0;
  while (i < s.size()) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    size_t j = i;
    while (j < s.size() && s[j] != ' ' && s[j] != '\t') ++j;
    if (j > i) out.emplace_back(s.substr(i, j - i));
    i = j;
  }
  return out;
}

}  // namespace

// ---- Resources -------------------------------------------------------------------------------------------------------
Resources::Resources(Key) {}
Resources::~Resources() = default;
const std::vector<std::string>& Resources::warnings() const { return impl_->warnings; }

Result<std::unique_ptr<Resources>> Resources::create(const lex::Lexicon& la, const curated::CuratedData& cd,
                                                     const std::vector<std::filesystem::path>& dirs) {
  try {
    std::filesystem::path file;
    for (const auto& d : dirs) {
      std::error_code ec;
      if (d.empty()) continue;
      if (std::filesystem::is_regular_file(d / "simplify_la.tsv", ec)) { file = d / "simplify_la.tsv"; break; }
    }
    if (file.empty())
      return Result<std::unique_ptr<Resources>>(ErrorCode::NotFound, "simplify_la.tsv not found",
                                                "The Orbergise table (data/curated/simplify_la.tsv) is missing.");
    std::ifstream in(file);
    if (!in)
      return Result<std::unique_ptr<Resources>>(ErrorCode::Io, "cannot read " + file.string(),
                                                "The Orbergise table could not be read.");
    auto res = std::make_unique<Resources>(Key{});
    res->impl_ = std::make_unique<Impl>(la, cd);
    Impl& R = *res->impl_;
    std::string line;
    int n = 0;
    while (std::getline(in, line)) {
      ++n;
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (line.empty() || line[0] == '#') continue;
      const std::vector<std::string> c = splitTabs(line);
      auto warn = [&](const std::string& m) { R.warnings.push_back("simplify_la.tsv:" + std::to_string(n) + ": " + m); };
      if (c.size() < 3) { warn("fewer than 3 columns"); continue; }
      const std::string& kind = c[0];
      if (kind == "rule") {
        detail::RuleRow r;
        r.id = c[1];
        r.on = c[2] != "off";
        if (c.size() > 3) {
          std::stringstream ss(c[3]);
          std::string kv;
          while (std::getline(ss, kv, ';')) {
            const size_t eq = kv.find('=');
            if (eq == std::string::npos) continue;
            r.params.emplace_back(kv.substr(0, eq), kv.substr(eq + 1));
          }
        }
        if (c.size() > 4) r.note = c[4];
        if (R.rule(r.id)) { warn("duplicate rule " + r.id + " (first row wins)"); continue; }
        R.rules.push_back(std::move(r));
      } else if (kind == "pair") {
        detail::PairRow p;
        for (const std::string& w : words(c[1])) p.keys.push_back(text::latin_key(w));
        p.to = text::nfc(c[2]);
        p.agree = c.size() > 3 && c[3] == "agree";
        if (c.size() > 4) p.note = c[4];
        if (p.keys.empty() || p.to.empty()) { warn("empty pair"); continue; }
        R.pairs.push_back(std::move(p));
      } else if (kind == "keep") {
        R.keep.push_back(detail::KeepRow{text::latin_key(c[1]), c[2]});
      } else {
        warn("unknown kind '" + kind + "'");
      }
    }
    // longest pairs first (stable: file order among equals)
    std::stable_sort(R.pairs.begin(), R.pairs.end(),
                     [](const detail::PairRow& a, const detail::PairRow& b) { return a.keys.size() > b.keys.size(); });
    return Result<std::unique_ptr<Resources>>(std::move(res));
  } catch (const std::exception& e) {
    return Result<std::unique_ptr<Resources>>(ErrorCode::Internal, std::string("simplify_la.tsv: ") + e.what(),
                                              "The Orbergise table could not be loaded.");
  }
}

// ---- Impl helpers ------------------------------------------------------------------------------------------------------
const detail::RuleRow* Resources::Impl::rule(std::string_view id) const {
  for (const detail::RuleRow& r : rules)
    if (r.id == id) return &r;
  return nullptr;
}

std::string Resources::Impl::param(std::string_view id, std::string_view name, std::string_view def) const {
  if (const detail::RuleRow* r = rule(id))
    for (const auto& kv : r->params)
      if (kv.first == name) return kv.second;
  return std::string(def);
}

bool Resources::Impl::keepLemma(uint32_t lemma) const {
  const lex::Lemma l = la.lemma(lemma);
  for (const detail::KeepRow& k : keep)
    if (k.key == l.key && (k.pos.empty() || k.pos == "-" || k.pos == curated::CuratedData::tierPos(l.pos))) return true;
  return false;
}

uint32_t Resources::Impl::find(std::string_view head, uint8_t pos) {
  const std::string k = std::string(head) + "|" + std::to_string(pos);
  auto it = found.find(k);
  if (it != found.end()) return it->second;
  uint32_t id = morph::findLemma(la, head, pos);
  if (id == kNone && pos) id = morph::findLemma(la, head, 0);
  if (found.size() >= kCacheCap) found.clear();
  found.emplace(k, id);
  return id;
}

uint8_t Resources::Impl::tier(uint32_t lemma) const {
  if (lemma == kNone || lemma >= la.lemmaCount()) return 3;
  const lex::Lemma l = la.lemma(lemma);
  const uint8_t t = cd.effectiveTier(l.key, l.pos, l.tier);
  return t ? t : 3;
}

bool Resources::Impl::isParticipleLemma(uint32_t lemma) const {
  return lemma != kNone && lemma < la.lemmaCount() && la.lemma(lemma).pos == Participle;
}

// The verb of a participle lemma: the lexicon lists the nominative masculine singular (the participle's headword) as
// a "verb ... participle" analysis of the verb itself ("vīsus" <- videō perfect participle passive).
uint32_t Resources::Impl::verbOf(uint32_t p) {
  if (!isParticipleLemma(p)) return kNone;
  auto it = verbOfPart.find(p);
  if (it != verbOfPart.end()) return it->second;
  const lex::Lemma pl = la.lemma(p);
  anaBuf.clear();
  la.lookup(pl.key, anaBuf);
  uint32_t best = kNone;
  long bestScore = -1;
  const std::string head = morph::displayForm(pl.head, true);
  for (const lex::Analysis& a : anaBuf) {
    const Features f = unpack(la.feature(a.feat));
    if (f.pos != Verb || f.mood != ParticipleMood) continue;
    const lex::Lemma vl = la.lemma(a.lemma);
    if (vl.pos != Verb) continue;
    long s = 0;
    if (morph::displayForm(a.display, true) == head) s += 1 << 20;   // same vowel quantities (vīsus: videō, not vīsō)
    s += (long)(4 - std::min<uint8_t>(3, tier(a.lemma))) << 16;
    s += 65535 - (vl.freqRank ? vl.freqRank : 65535);
    if (s > bestScore || (s == bestScore && a.lemma < best)) { bestScore = s; best = a.lemma; }
  }
  if (verbOfPart.size() >= kCacheCap) verbOfPart.clear();
  verbOfPart.emplace(p, best);
  return best;
}

uint8_t Resources::Impl::wordTier(uint32_t lemma) {
  if (isParticipleLemma(lemma)) {
    const uint32_t v = verbOf(lemma);
    if (v != kNone) return std::min(tier(v), tier(lemma));
  }
  return tier(lemma);
}

detail::Swap Resources::Impl::swapFor(uint32_t lemma, int ceiling, bool keepNames) {
  detail::Swap none;
  if (lemma == kNone || lemma >= la.lemmaCount()) return none;
  if (tier(lemma) <= ceiling) return none;
  const lex::Lemma l = la.lemma(lemma);
  if (keepNames && ((l.flags & lex::ProperName) || l.pos == Name)) return none;
  const uint64_t key = ((uint64_t)lemma << 8) | ((uint64_t)ceiling << 1) | (keepNames ? 1u : 0u);
  auto it = swaps.find(key);
  if (it != swaps.end()) return it->second;
  detail::Swap out;
  // 1. periphrasis_la.tsv: the word of the row with the lemma's part of speech is inflected, the others are fixed
  if (const curated::PeriphrasisEntry* pe = cd.periphrasis(l.key)) {
    const std::vector<std::string> ws = words(pe->periphrasis);
    int main = -1;
    uint32_t mainLemma = kNone;
    for (size_t i = 0; i < ws.size(); ++i) {
      const uint32_t id = morph::findLemma(la, ws[i], l.pos);
      if (id != kNone && la.lemma(id).pos == l.pos) { main = (int)i; mainLemma = id; }
    }
    if (main >= 0 && mainLemma != lemma && tier(mainLemma) <= ceiling) {
      out.lemma = mainLemma;
      for (int i = 0; i < main; ++i) out.before += (out.before.empty() ? "" : " ") + ws[(size_t)i];
      for (size_t i = (size_t)main + 1; i < ws.size(); ++i) out.after += (out.after.empty() ? "" : " ") + ws[i];
      out.rule = "periphrasis";
      out.why = std::string(l.head) + " -> " + pe->periphrasis + " (periphrasis table)";
    }
  }
  // 2. the lexicon's sense keywords (and the teacher's tier glosses), same part of speech, a core sense of the
  //    candidate, overlap required
  if (out.lemma == kNone && !keepLemma(lemma)) {
    senseBuf.clear();
    la.senses(lemma, senseBuf);
    std::vector<std::string> kws;
    if (!senseBuf.empty()) kws = words(senseBuf[0].keywords);
    if (kws.size() > 6) kws.resize(6);
    struct Cand { uint8_t tier; int ov; int first; int sameGender; int score; uint16_t rank; uint32_t id; std::string why; };
    std::vector<Cand> cands;
    auto consider = [&](uint32_t id, int score, const std::vector<std::string>& ckws, const char* src) {
      if (id == kNone || id == lemma) return;
      const lex::Lemma cl = la.lemma(id);
      if (cl.pos != l.pos || (cl.flags & lex::ProperName)) return;
      if ((l.pos == Noun || l.pos == Verb || l.pos == Adj) && !(cl.flags & lex::HasTable)) return;
      const uint8_t t = tier(id);
      if (t > ceiling) return;
      int ov = 0;
      std::string shared;
      for (const std::string& k : kws)
        if (std::find(ckws.begin(), ckws.end(), k) != ckws.end()) {
          ++ov;
          if (shared.size() < 40) shared += (shared.empty() ? "" : ", ") + k;
        }
      const int first = !kws.empty() && !ckws.empty() && kws[0] == ckws[0] ? 1 : 0;
      // same sense: the candidate's sense names the word's first meaning, or the candidate's first meaning is one of
      // the word's first two (alloquor "speak, address, greet" is not salūtō "greet")
      const bool head = !kws.empty() && std::find(ckws.begin(), ckws.end(), kws[0]) != ckws.end();
      const bool back = !ckws.empty() && std::find(kws.begin(), kws.begin() + std::min<size_t>(2, kws.size()), ckws[0]) !=
                                             kws.begin() + std::min<size_t>(2, kws.size());
      // nouns and adjectives: the candidate's own first meaning must be the word's (dux "leader" is not magister
      // "teacher", although a teacher may lead)
      const bool nominal = l.pos == Noun || l.pos == Adj;
      if (std::string(src) != "teacher" && (nominal ? !(back && (ov >= 2 || first)) : (!head && !back))) return;
      // valency: a verb that governs another case is not a synonym here
      if (l.pos == Verb) {
        const curated::Valency* va = cd.valency(l.key);
        const curated::Valency* vb = cd.valency(cl.key);
        auto oblique = [](const curated::Valency* v) {
          if (!v || v->frames.empty()) return std::string("acc");
          const curated::FrameKind k = v->frames[0].kind;
          return std::string(k == curated::FrameKind::Dat ? "dat" : k == curated::FrameKind::Abl ? "abl"
                             : k == curated::FrameKind::Gen ? "gen" : k == curated::FrameKind::Prep ? "prep"
                             : k == curated::FrameKind::Intr ? "intr" : "acc");
        };
        if (oblique(va) != oblique(vb)) return;
      }
      const int sameGender = l.pos == Noun && cl.gender == l.gender ? 1 : 0;
      cands.push_back(Cand{t, ov, first, sameGender, score, cl.freqRank ? cl.freqRank : (uint16_t)65535, id,
                           std::string(src) + ": " + (shared.empty() ? std::string("teacher gloss") : shared)});
    };
    // teacher glosses (tiers_la.tsv notes)
    for (size_t k = 0; k < kws.size() && k < 3; ++k) {
      std::vector<const curated::TierEntry*> rows;
      cd.glossTiers(kws[k], rows);
      for (const curated::TierEntry* te : rows) {
        if (te->tier == 0 || te->tier > ceiling) continue;
        const uint32_t id = morph::findLemma(la, te->head, l.pos);
        if (id == kNone) continue;
        std::vector<std::string> ck;
        senseBuf2.clear();
        la.senses(id, senseBuf2);
        if (!senseBuf2.empty()) ck = words(senseBuf2[0].keywords);
        consider(id, 255, ck, "teacher");
      }
    }
    for (size_t k = 0; k < kws.size() && k < 4; ++k) {
      candBuf.clear();
      la.reverse(kws[k], candBuf);
      for (const lex::Candidate& c : candBuf) {
        if (c.sense > 1 || c.score < 100) continue;
        senseBuf2.clear();
        la.senses(c.lemma, senseBuf2);
        if (c.sense >= senseBuf2.size()) continue;
        consider(c.lemma, c.score, words(senseBuf2[c.sense].keywords), "synonym");
      }
    }
    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) {
      if (a.tier != b.tier) return a.tier < b.tier;
      if (a.ov != b.ov) return a.ov > b.ov;
      if (a.first != b.first) return a.first > b.first;
      if (a.sameGender != b.sameGender) return a.sameGender > b.sameGender;
      if (a.score != b.score) return a.score > b.score;
      if (a.rank != b.rank) return a.rank < b.rank;
      return a.id < b.id;
    });
    if (!cands.empty()) {
      out.lemma = cands[0].id;
      out.rule = cands[0].why.compare(0, 7, "teacher") == 0 ? "teacher" : "synonym";
      out.why = std::string(l.head) + " -> " + std::string(la.lemma(cands[0].id).head) + " (" + cands[0].why + ")";
    }
  }
  if (swaps.size() >= kCacheCap) swaps.clear();
  swaps.emplace(key, out);
  return out;
}

}  // namespace vp::orberg

namespace vp::orberg::detail {

using namespace vp::feat;

bool LemmaSet::has(uint32_t l) const { return std::binary_search(lemmas.begin(), lemmas.end(), l); }
bool LemmaSet::hasName(const std::string& k) const { return std::binary_search(names.begin(), names.end(), k); }

uint32_t nameId(std::string_view key) {
  uint32_t h = 2166136261u;
  for (char c : key) { h ^= (uint8_t)c; h *= 16777619u; }
  return 0x80000000u | (h & 0x7FFFFFFFu);
}

bool contentReading(const la2x::Reading& r, const lex::Lexicon& la) {
  if (r.name) return true;
  if (r.lemma == kNone) return false;
  const lex::Lemma l = la.lemma(r.lemma);
  const Features f = unpack(r.packed);
  const uint8_t p = l.pos ? l.pos : f.pos;
  if (p == Verb) return l.key != "sum";
  return p == Noun || p == Adj || p == Adv || p == Participle || p == Name || p == Num;
}

void contentLemmas(const la2x::Sentence& s, Resources::Impl& R, LemmaSet& out) {
  for (const la2x::Token& t : s.tokens) {
    if (t.kind != la2x::TokKind::Word) continue;
    for (const la2x::Reading& r : t.readings) {
      if (r.name || (r.lemma != kNone && (R.la.lemma(r.lemma).flags & lex::ProperName))) {
        out.names.push_back(text::latin_key(r.lemma != kNone ? R.la.lemma(r.lemma).head : std::string_view(r.display)));
        out.names.push_back(text::latin_key(t.text));
      }
      if (r.lemma == kNone) continue;
      out.lemmas.push_back(r.lemma);
      if (R.isParticipleLemma(r.lemma)) {
        const uint32_t v = R.verbOf(r.lemma);
        if (v != kNone) out.lemmas.push_back(v);
      }
    }
  }
  std::sort(out.lemmas.begin(), out.lemmas.end());
  out.lemmas.erase(std::unique(out.lemmas.begin(), out.lemmas.end()), out.lemmas.end());
  std::sort(out.names.begin(), out.names.end());
  out.names.erase(std::unique(out.names.begin(), out.names.end()), out.names.end());
}

// ---- text helpers ----------------------------------------------------------------------------------------------------
std::string capitalise(std::string_view w) {
  if (w.empty()) return std::string();
  size_t j = 0;
  text::decodeUtf8(w, j);
  std::string first(w.substr(0, j));
  // upper case of the first letter: ASCII and the macron vowels (ā ē ī ō ū ȳ -> Ā Ē Ī Ō Ū Ȳ)
  static const char* pairs[][2] = {{"ā", "Ā"}, {"ē", "Ē"}, {"ī", "Ī"}, {"ō", "Ō"}, {"ū", "Ū"}, {"ȳ", "Ȳ"},
                                   {"æ", "Æ"}, {"œ", "Œ"}};
  if (first.size() == 1 && first[0] >= 'a' && first[0] <= 'z') first[0] = (char)(first[0] - 'a' + 'A');
  else
    for (auto& p : pairs)
      if (first == p[0]) { first = p[1]; break; }
  return first + std::string(w.substr(j));
}

std::string decapitalise(std::string_view w) {
  if (w.empty()) return std::string();
  size_t j = 0;
  text::decodeUtf8(w, j);
  return text::lower(w.substr(0, j)) + std::string(w.substr(j));
}

bool startsUpper(std::string_view w) {
  if (w.empty()) return false;
  size_t j = 0;
  text::decodeUtf8(w, j);
  const std::string f(w.substr(0, j));
  return text::lower(f) != f;
}

std::string jsonEscape(std::string_view s) {
  std::string o;
  for (char c : s) {
    switch (c) {
      case '"': o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n"; break;
      case '\t': o += "\\t"; break;
      default:
        if ((unsigned char)c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += c;
    }
  }
  return o;
}

}  // namespace vp::orberg::detail
