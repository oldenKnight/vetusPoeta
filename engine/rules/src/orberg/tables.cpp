// Orbergise resources: simplify_la.tsv loader, tier / participle / lemma helpers, the vocabulary-swap search and small
// text helpers (internal.h). Deterministic: ties broken by tier, overlap, score, frequency rank, lemma id.
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include "internal.h"
#include "vp/la2x.h"
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
      } else if (kind == "syn") {
        detail::SynRow y;
        y.key = text::latin_key(c[1]);
        y.to = text::nfc(c[2]);
        y.pos = c.size() > 3 && !c[3].empty() ? c[3] : "-";
        if (c.size() > 4) y.note = c[4];
        if (y.key.empty() || words(y.to).empty()) { warn("empty syn row"); continue; }
        R.syn.push_back(std::move(y));
      } else if (kind == "fixed") {
        detail::FixedRow f;
        for (const std::string& w : words(c[1])) {
          std::vector<std::string> alts;
          size_t a = 0;
          for (;;) {
            const size_t b = w.find('|', a);
            const std::string x = w.substr(a, b == std::string::npos ? std::string::npos : b - a);
            if (!x.empty()) alts.push_back(x == "GER" ? x : text::latin_key(x));
            if (b == std::string::npos) break;
            a = b + 1;
          }
          if (!alts.empty()) f.elems.push_back(std::move(alts));
        }
        f.name = c[2];
        if (c.size() > 3) {
          std::stringstream ss(c[3]);
          std::string kv;
          while (std::getline(ss, kv, ';')) {
            if (kv.compare(0, 4, "gap=") == 0) f.gap = std::max(0, std::min(4, std::atoi(kv.c_str() + 4)));
            if (kv == "order=any") f.anyOrder = true;
          }
        }
        if (c.size() > 4) f.note = c[4];
        if (f.elems.size() < 2) { warn("a fixed phrase needs two elements"); continue; }
        R.fixed.push_back(std::move(f));
      } else if (kind == "class") {
        detail::ClassRow k;
        k.name = c[1];
        size_t a = 0;
        const std::string& v = c[2];
        for (;;) {
          const size_t b = v.find('|', a);
          std::string x = v.substr(a, b == std::string::npos ? std::string::npos : b - a);
          for (const std::string& ph : detail::glossPhrases(x, false, 1)) k.phrases.push_back(ph);
          if (b == std::string::npos) break;
          a = b + 1;
        }
        if (c.size() > 3 && c[3].compare(0, 6, "never=") == 0) k.never = words(c[3].substr(6));
        std::sort(k.phrases.begin(), k.phrases.end());
        if (k.name.empty() || k.phrases.empty()) { warn("empty class row"); continue; }
        R.classes.push_back(std::move(k));
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

namespace {

constexpr uint16_t kGlossEsPivot = 1u << 8;   // LEMM flag: the Spanish gloss came through English
constexpr uint16_t kTagTransitive = 1u << 0, kTagIntransitive = 1u << 1;

bool hasPhrase(const std::vector<std::string>& v, const std::string& p) {
  return !p.empty() && std::find(v.begin(), v.end(), p) != v.end();
}
void addAll(std::vector<std::string>& to, const std::vector<std::string>& from) {
  for (const std::string& x : from)
    if (!hasPhrase(to, x)) to.push_back(x);
}

// English words of an original-language cue match a gloss word with a little inflection slack (go: goes, going).
bool evidenceHas(const std::vector<std::string>& ev, const std::string& w) {
  if (w.size() < 2) return false;
  for (const std::string& e : ev) {
    if (e == w) return true;
    if (e.size() > w.size() && e.compare(0, w.size(), w) == 0) {
      const std::string suf = e.substr(w.size());
      if (suf == "s" || suf == "es" || suf == "ed" || suf == "d" || suf == "ing" || suf == "ly") return true;
    }
  }
  return false;
}

}  // namespace

// English (or Spanish) gloss phrases of the lexicon's first sense plus the la2x curated gloss.
struct SenseView {
  std::vector<std::string> en, es;   // en[0] = the lexicon's first meaning
  std::vector<std::string> enCur;    // la2x curated / interlinear English gloss phrases
  bool esReal = false;               // a Spanish gloss that did not come through English
  uint16_t tags = 0;
};

static SenseView senseView(const lex::Lexicon& la, uint32_t id, const la2x::Translator* tr, std::vector<lex::Sense>& buf) {
  SenseView v;
  const lex::Lemma l = la.lemma(id);
  buf.clear();
  la.senses(id, buf);
  const std::string_view en0 = !buf.empty() ? buf[0].glossEn : l.glossEn;
  v.en = detail::glossPhrases(en0, true, 4);
  if (!buf.empty()) v.tags = buf[0].tags;
  if (tr) v.enCur = detail::glossPhrases(tr->gloss(id, la2x::Target::En), true, 3);
  if (!(l.flags & kGlossEsPivot)) {
    v.es = detail::glossPhrases(!buf.empty() && !buf[0].glossEs.empty() ? buf[0].glossEs : l.glossEs, false, 4);
    if (tr) {
      bool pivot = false;
      const std::string g = tr->gloss(id, la2x::Target::Es, &pivot);
      if (!pivot) addAll(v.es, detail::glossPhrases(g, false, 3));
    }
    v.esReal = !v.es.empty();
  }
  return v;
}

std::vector<std::string> Resources::Impl::classesOf(uint32_t lemma, const la2x::Translator* tr) {
  std::vector<std::string> out;
  if (classes.empty() || lemma == kNone || lemma >= la.lemmaCount()) return out;
  const SenseView v = senseView(la, lemma, tr, senseBuf2);
  std::vector<std::string> all = v.en;
  addAll(all, v.enCur);
  addAll(all, v.es);
  if (tr) {   // pivot Spanish glosses still name the class ("cambiar")
    const lex::Lemma l = la.lemma(lemma);
    addAll(all, detail::glossPhrases(l.glossEs, false, 3));
  }
  for (const detail::ClassRow& c : classes)
    for (const std::string& p : all)
      if (std::binary_search(c.phrases.begin(), c.phrases.end(), p)) {
        if (!hasPhrase(out, c.name)) out.push_back(c.name);
        break;
      }
  return out;
}

bool Resources::Impl::sameSense(uint32_t word, uint32_t cand, const la2x::Translator* tr, std::string* why) {
  if (word == kNone || cand == kNone || word >= la.lemmaCount() || cand >= la.lemmaCount()) return false;
  const SenseView w = senseView(la, word, tr, senseBuf);
  const SenseView c = senseView(la, cand, tr, senseBuf2);
  if (w.en.empty() || c.en.empty()) return false;
  std::vector<std::string> ws = w.en, cs = c.en;
  addAll(ws, w.enCur);
  addAll(cs, c.enCur);
  // the candidate's first meaning is one of the word's, or the word's first meaning is one of the candidate's
  const bool a = hasPhrase(ws, c.en[0]) || (!c.enCur.empty() && hasPhrase(ws, c.enCur[0]));
  const bool b = hasPhrase(cs, w.en[0]) || (!w.enCur.empty() && hasPhrase(cs, w.enCur[0]));
  if (!a && !b) return false;
  // Spanish: an independent witness of the sense when both glosses are real
  bool es = false;
  if (w.esReal && c.esReal) {
    for (const std::string& p : w.es) es = es || hasPhrase(c.es, p);
    if (!es) return false;
    // a word whose Spanish gloss has sense groups (";": tempestās "tiempo, época; tempestad, tormenta") is swapped only
    // for a word that covers every group: the first sense alone may not be the one the text means
    const lex::Lemma lw = la.lemma(word);
    senseBuf.clear();
    la.senses(word, senseBuf);
    const std::string_view esw = !senseBuf.empty() && !senseBuf[0].glossEs.empty() ? senseBuf[0].glossEs : lw.glossEs;
    size_t a0 = 0;
    while (a0 < esw.size()) {
      size_t b0 = esw.find(';', a0);
      if (b0 == std::string_view::npos) b0 = esw.size();
      bool covered = false;
      for (const std::string& p : detail::glossPhrases(esw.substr(a0, b0 - a0), false, 6)) covered = covered || hasPhrase(c.es, p);
      if (!covered && b0 > a0 + 1) return false;
      a0 = b0 + 1;
    }
  } else if (!(a && b)) {
    return false;   // English only: the first meanings must name each other
  }
  // sense classes: a verb of motion never becomes a verb of change, ceasing or returning
  const std::vector<std::string> cw = classesOf(word, tr), cc = classesOf(cand, tr);
  for (const detail::ClassRow& k : classes) {
    if (!hasPhrase(cw, k.name)) continue;
    for (const std::string& n : k.never)
      if (hasPhrase(cc, n) && !hasPhrase(cw, n)) return false;
  }
  // transitivity of verbs (sense tags), when both are marked
  if (la.lemma(word).pos == Verb) {
    const uint16_t tw = w.tags & (kTagTransitive | kTagIntransitive), tc = c.tags & (kTagTransitive | kTagIntransitive);
    if ((tw == kTagTransitive && tc == kTagIntransitive) || (tw == kTagIntransitive && tc == kTagTransitive)) return false;
  }
  if (why) {
    std::string shared = a ? (hasPhrase(ws, c.en[0]) ? c.en[0] : c.enCur[0]) : (hasPhrase(cs, w.en[0]) ? w.en[0] : w.enCur[0]);
    *why = shared;
  }
  return true;
}

bool Resources::Impl::glossSame(uint32_t a, uint32_t b, const la2x::Translator* tr) {
  if (a == b) return true;
  if (a == kNone || b == kNone || a >= la.lemmaCount() || b >= la.lemmaCount()) return false;
  const SenseView x = senseView(la, a, tr, senseBuf);
  const SenseView y = senseView(la, b, tr, senseBuf2);
  std::vector<std::string> xs = x.enCur, ys = y.enCur;
  for (size_t k = 0; k < x.en.size() && k < 3; ++k) addAll(xs, {x.en[k]});
  for (size_t k = 0; k < y.en.size() && k < 3; ++k) addAll(ys, {y.en[k]});
  bool en = false;
  for (const std::string& p : xs) en = en || hasPhrase(ys, p);
  bool es = false;   // real Spanish glosses are a second witness ("salir" for ēgredior and exeō)
  if (x.esReal && y.esReal)
    for (const std::string& p : x.es) es = es || hasPhrase(y.es, p);
  if (!en && !es) return false;
  const std::vector<std::string> ca = classesOf(a, tr), cb = classesOf(b, tr);
  for (const detail::ClassRow& k : classes) {
    if (!hasPhrase(ca, k.name)) continue;
    for (const std::string& n : k.never)
      if (hasPhrase(cb, n) && !hasPhrase(ca, n)) return false;
  }
  return true;
}

detail::Swap Resources::Impl::swapFor(uint32_t lemma, int ceiling, bool keepNames, const std::vector<std::string>* evidence,
                                      const la2x::Translator* tr) {
  detail::Swap none;
  if (lemma == kNone || lemma >= la.lemmaCount()) return none;
  if (tier(lemma) <= ceiling) return none;
  const lex::Lemma l = la.lemma(lemma);
  if (keepNames && ((l.flags & lex::ProperName) || l.pos == Name)) return none;
  const uint64_t key = ((uint64_t)lemma << 8) | ((uint64_t)ceiling << 1) | (keepNames ? 1u : 0u);
  auto it = swaps.find(key);
  if (it == swaps.end()) {
    std::vector<detail::Swap> list;
    auto evidenceOf = [&](uint32_t id, detail::Swap& s) {
      const SenseView v = senseView(la, id, tr, senseBuf2);
      for (const std::vector<std::string>* ph : {&v.en, &v.enCur})
        for (const std::string& p : *ph)
          for (const std::string& w : words(p))
            if (w.size() > 2 && !hasPhrase(s.evidence, w)) s.evidence.push_back(w);
    };
    // a row "before main after": the word of the row with the lemma's part of speech is inflected
    auto fromRow = [&](const std::string& value, const char* rule, const std::string& why, bool confirmed) {
      const std::vector<std::string> ws = words(value);
      int main = -1;
      uint32_t mainLemma = kNone;
      for (size_t i = 0; i < ws.size(); ++i) {
        const uint32_t id = morph::findLemma(la, ws[i], l.pos);
        if (id != kNone && la.lemma(id).pos == l.pos) { main = (int)i; mainLemma = id; }
      }
      if (main < 0 || mainLemma == lemma || tier(mainLemma) > ceiling) return;
      detail::Swap s;
      s.lemma = mainLemma;
      for (int i = 0; i < main; ++i) s.before += (s.before.empty() ? "" : " ") + ws[(size_t)i];
      for (size_t i = (size_t)main + 1; i < ws.size(); ++i) s.after += (s.after.empty() ? "" : " ") + ws[i];
      s.rule = rule;
      s.why = why;
      s.confirmed = confirmed;
      evidenceOf(mainLemma, s);
      for (const detail::Swap& x : list)
        if (x.lemma == s.lemma && x.before == s.before && x.after == s.after) return;
      list.push_back(std::move(s));
    };
    // 1. syn rows of simplify_la.tsv: confirmed same-sense pairs
    const char* lp = curated::CuratedData::tierPos(l.pos);
    for (const detail::SynRow& y : syn)
      if (y.key == l.key && (y.pos == "-" || (lp && y.pos == lp)))
        fromRow(y.to, "syn", std::string(l.head) + " -> " + y.to + " (same sense, confirmed pair)", true);
    // 2. periphrasis_la.tsv
    if (const curated::PeriphrasisEntry* pe = cd.periphrasis(l.key))
      fromRow(pe->periphrasis, "periphrasis", std::string(l.head) + " -> " + pe->periphrasis + " (periphrasis table)",
              false);
    // 3. teacher glosses and the lexicon's senses: only the same sense
    if (!keepLemma(lemma)) {
      senseBuf.clear();
      la.senses(lemma, senseBuf);
      std::vector<std::string> kws;
      if (!senseBuf.empty()) kws = words(senseBuf[0].keywords);
      if (kws.size() > 6) kws.resize(6);
      struct Cand { uint8_t tier; int ov; int first; int sameGender; int score; uint16_t rank; uint32_t id; std::string why; bool teacher; };
      std::vector<Cand> cands;
      auto consider = [&](uint32_t id, int score, const std::vector<std::string>& ckws, bool teacher) {
        if (id == kNone || id == lemma) return;
        for (const Cand& x : cands)
          if (x.id == id) return;
        const lex::Lemma cl = la.lemma(id);
        if (cl.pos != l.pos || (cl.flags & lex::ProperName)) return;
        if ((l.pos == Noun || l.pos == Verb || l.pos == Adj) && !(cl.flags & lex::HasTable)) return;
        const uint8_t t = tier(id);
        if (t > ceiling) return;
        // nouns: masculine <-> feminine is a change of sex for persons (magistra is not doctor); a thing may change
        // gender (nāvigium -> nāvis)
        if (l.pos == Noun && ((l.gender == M && cl.gender == F) || (l.gender == F && cl.gender == M))) return;
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
        std::string shared;
        if (!sameSense(lemma, id, tr, &shared)) return;
        int ov = 0;
        for (const std::string& k : kws)
          if (std::find(ckws.begin(), ckws.end(), k) != ckws.end()) ++ov;
        const int first = !kws.empty() && !ckws.empty() && kws[0] == ckws[0] ? 1 : 0;
        const int sameGender = l.pos == Noun && cl.gender == l.gender ? 1 : 0;
        cands.push_back(Cand{t, ov, first, sameGender, score, cl.freqRank ? cl.freqRank : (uint16_t)65535, id,
                             std::string(teacher ? "teacher gloss, " : "") + "same sense: " + shared, teacher});
      };
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
          consider(id, 255, ck, true);
        }
      }
      for (size_t k = 0; k < kws.size() && k < 4; ++k) {
        candBuf.clear();
        la.reverse(kws[k], candBuf);
        const std::vector<lex::Candidate> cb = candBuf;   // consider() reuses the lexicon buffers
        for (const lex::Candidate& c : cb) {
          if (c.sense > 1 || c.score < 100) continue;
          senseBuf2.clear();
          la.senses(c.lemma, senseBuf2);
          if (c.sense >= senseBuf2.size()) continue;
          consider(c.lemma, c.score, words(senseBuf2[c.sense].keywords), false);
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
      for (const Cand& c : cands) {
        detail::Swap s;
        s.lemma = c.id;
        s.rule = c.teacher ? "teacher" : "synonym";
        s.why = std::string(l.head) + " -> " + std::string(la.lemma(c.id).head) + " (" + c.why + ")";
        evidenceOf(c.id, s);
        list.push_back(std::move(s));
      }
    }
    if (swaps.size() >= kCacheCap) swaps.clear();
    it = swaps.emplace(key, std::move(list)).first;
  }
  const std::vector<detail::Swap>& list = it->second;
  if (list.empty()) return none;
  // the original-language cue as evidence: the first candidate whose gloss names one of its words
  if (evidence && !evidence->empty())
    for (const detail::Swap& s : list)
      for (const std::string& w : s.evidence)
        if (evidenceHas(*evidence, w)) return s;
  return list[0];
}

}  // namespace vp::orberg

namespace vp::orberg::detail {

using namespace vp::feat;

std::vector<std::string> glossPhrases(std::string_view gloss, bool english, size_t max) {
  std::vector<std::string> out;
  std::string g;
  int depth = 0;
  for (char c : gloss) {   // parenthesised remarks out
    if (c == '(' || c == '[') { ++depth; continue; }
    if (c == ')' || c == ']') { if (depth) --depth; continue; }
    if (!depth) g += c;
  }
  size_t a = 0;
  while (a <= g.size() && out.size() < max) {
    size_t b = g.find_first_of(",;", a);
    if (b == std::string::npos) b = g.size();
    std::string p = text::lower(g.substr(a, b - a));
    std::string q;
    for (char c : p) {   // collapse spaces, drop dots and quotes
      if (c == '.' || c == '"') continue;
      if (c == ' ' || c == '\t') {
        if (!q.empty() && q.back() != ' ') q += ' ';
      } else {
        q += c;
      }
    }
    while (!q.empty() && q.back() == ' ') q.pop_back();
    if (english) {
      for (const char* art : {"to ", "a ", "an ", "the "}) {
        const size_t n = std::char_traits<char>::length(art);
        if (q.size() > n && q.compare(0, n, art) == 0) { q = q.substr(n); break; }
      }
      // "look at", "come to": the verb is the sense ("leave off", "give up" keep their particle)
      for (const char* tail : {" at", " to", " upon", " towards"}) {
        const size_t n = std::char_traits<char>::length(tail);
        if (q.size() > n + 1 && q.compare(q.size() - n, n, tail) == 0) { q = q.substr(0, q.size() - n); break; }
      }
    }
    if (!q.empty() && std::find(out.begin(), out.end(), q) == out.end()) out.push_back(q);
    a = b + 1;
  }
  return out;
}

void bestContentLemmas(const la2x::Sentence& s, Resources::Impl& R, LemmaSet& out) {
  for (const la2x::Token& t : s.tokens) {
    if (t.kind != la2x::TokKind::Word || t.readings.empty()) continue;
    const la2x::Reading& r = t.readings[0];
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
  std::sort(out.lemmas.begin(), out.lemmas.end());
  out.lemmas.erase(std::unique(out.lemmas.begin(), out.lemmas.end()), out.lemmas.end());
  std::sort(out.names.begin(), out.names.end());
  out.names.erase(std::unique(out.names.begin(), out.names.end()), out.names.end());
}

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
