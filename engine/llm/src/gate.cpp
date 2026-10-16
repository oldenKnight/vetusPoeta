// Minimal-pair gate (PREPLAN 2.2): pair generation from the Latin lexicon, a generous clause checker, scoring.
#include <algorithm>
#include <chrono>
#include <set>
#include <string_view>

#include "prompts.h"
#include "vp/features.h"
#include "vp/llm_gate.h"
#include "vp/text.h"

namespace vp::llm {

namespace {

using vp::feat::Features;

// ---------------------------------------------------------------------------------------------- PRNG (splitmix64)
class Rng {
 public:
  explicit Rng(uint64_t seed) : s_(seed) {}
  uint64_t next() {
    uint64_t z = (s_ += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
  }
  size_t below(size_t n) { return n == 0 ? 0 : static_cast<size_t>(next() % n); }

 private:
  uint64_t s_;
};

// ---------------------------------------------------------------------------------------------- lexicon access
struct Cell {
  Features f;
  std::string form;   // without macrons, lower case
};

struct Word {
  uint32_t lemma = 0;
  uint8_t gender = 0;   // lemma gender (nouns)
  std::vector<Cell> cells;
};

bool plainLatin(const std::string& s) {
  if (s.empty() || s.size() > 24) return false;
  for (char c : s)
    if (c < 'a' || c > 'z') return false;
  return true;
}

// Every non-alternative cell of a lemma whose form is one plain word.
void loadCells(const lex::Lexicon& lx, uint32_t id, Word& w) {
  std::vector<std::pair<uint32_t, std::string_view>> raw;
  lx.cells(id, raw);
  for (const auto& c : raw) {
    Features f = vp::feat::unpack(c.first);
    if (f.extra != 0) continue;
    std::string form = vp::text::lower(vp::text::display_latin(c.second, false));
    if (!plainLatin(form)) continue;
    w.cells.push_back(Cell{f, std::move(form)});
  }
}

const Cell* findCell(const Word& w, uint8_t case_, uint8_t number, uint8_t gender = 0) {
  for (const Cell& c : w.cells)
    if (c.f.case_ == case_ && c.f.number == number && (gender == 0 || c.f.gender == gender) &&
        (c.f.degree == 0 || c.f.degree == vp::feat::Positive))
      return &c;
  return nullptr;
}

const Cell* findVerb(const Word& w, uint8_t person, uint8_t number) {
  for (const Cell& c : w.cells)
    if (c.f.person == person && c.f.number == number && c.f.tense == vp::feat::Present &&
        c.f.mood == vp::feat::Indicative && c.f.voice == vp::feat::Active)
      return &c;
  return nullptr;
}

// ---------------------------------------------------------------------------------------------- clause checker
struct Reading {
  uint8_t pos = 0, case_ = 0, number = 0, gender = 0, person = 0, mood = 0;
};

uint8_t genderBits(uint8_t g) {
  switch (g) {
    case vp::feat::M: return 1;
    case vp::feat::F: return 2;
    case vp::feat::N: return 4;
    case vp::feat::MF: return 3;
    case vp::feat::MN: return 5;
    case vp::feat::FN: return 6;
    case vp::feat::MFN: return 7;
    default: return 0;
  }
}
bool genderCompat(uint8_t a, uint8_t b) {
  const uint8_t x = genderBits(a), y = genderBits(b);
  return x == 0 || y == 0 || (x & y) != 0;
}

class Readings {
 public:
  explicit Readings(const lex::Lexicon& lx) : lx_(lx) {}
  // Every analysis of the (macron-less) word in the whole lexicon.
  std::vector<Reading> of(const std::string& word) {
    std::vector<Reading> out;
    an_.clear();
    lx_.lookup(vp::text::latin_key(word), an_);
    for (const lex::Analysis& a : an_) {
      const Features f = vp::feat::unpack(lx_.feature(a.feat));
      Reading r;
      r.pos = f.pos;
      r.case_ = f.case_;
      r.number = f.number;
      r.gender = f.gender ? f.gender : lx_.lemma(a.lemma).gender;
      r.person = f.person;
      r.mood = f.mood;
      out.push_back(r);
    }
    return out;
  }

 private:
  const lex::Lexicon& lx_;
  std::vector<lex::Analysis> an_;
};

bool nominal(const Reading& r) { return r.case_ != 0 && r.case_ != vp::feat::Voc; }
bool adjLike(const Reading& r) {
  return r.pos == vp::feat::Adj || r.pos == vp::feat::Participle || r.pos == vp::feat::Det ||
         (r.pos == vp::feat::Verb && r.mood == vp::feat::ParticipleMood);
}
bool finite(const Reading& r) {
  return r.person != 0 && r.number != 0 && (r.mood == vp::feat::Indicative || r.mood == vp::feat::Subjunctive);
}
bool agrees(const Reading& subj, const Reading& v) {
  return v.person == vp::feat::P3 && (subj.number == 0 || subj.number == v.number);
}
bool adjunct(uint8_t c) { return c == vp::feat::Dat || c == vp::feat::Abl || c == vp::feat::Loc; }

// "N A est": N nominative singular; A a predicate nominative (adjectives agree in gender; nouns need not), a
// predicate genitive, a dative of possession or an ablative of description. Also N dative + A nominative
// (possessive dative with A as the subject).
bool okNAEst(const std::vector<Reading>& n, const std::vector<Reading>& a) {
  for (const Reading& x : n) {
    if (!nominal(x)) continue;
    for (const Reading& y : a) {
      if (!nominal(y)) continue;
      if (x.case_ == vp::feat::Nom && x.number == vp::feat::Sg) {
        if (y.case_ == vp::feat::Nom && y.number == vp::feat::Sg && (!adjLike(y) || genderCompat(x.gender, y.gender)))
          return true;
        if (y.case_ == vp::feat::Gen || adjunct(y.case_)) return true;
      }
      if (x.case_ == vp::feat::Dat && y.case_ == vp::feat::Nom && y.number == vp::feat::Sg) return true;
    }
  }
  return false;
}

// "S O V" with any reading order: at most one nominative (agreeing with V) and at most one accusative; other
// nouns are adjuncts (dative, ablative, locative) or a genitive depending on the other noun; no nominative =
// pronoun subject.
bool okSOV(const std::vector<Reading>& s, const std::vector<Reading>& o, const std::vector<Reading>& v) {
  for (const Reading& vv : v) {
    if (!finite(vv)) continue;
    for (const Reading& x : s) {
      if (!nominal(x)) continue;
      for (const Reading& y : o) {
        if (!nominal(y)) continue;
        const Reading* pair[2] = {&x, &y};
        int noms = 0, accs = 0, gens = 0;
        bool ok = true;
        for (const Reading* r : pair) {
          if (r->case_ == vp::feat::Nom) {
            ++noms;
            if (!agrees(*r, vv)) ok = false;
          } else if (r->case_ == vp::feat::Acc) {
            ++accs;
          } else if (r->case_ == vp::feat::Gen) {
            ++gens;
          } else if (!adjunct(r->case_)) {
            ok = false;
          }
        }
        if (ok && noms <= 1 && accs <= 1 && gens <= 1) return true;
      }
    }
  }
  return false;
}

// "N V": N nominative agreeing with V, or an accusative/adjunct with a pronoun subject. A lone genitive has no
// head and does not count.
bool okNV(const std::vector<Reading>& n, const std::vector<Reading>& v) {
  for (const Reading& vv : v) {
    if (!finite(vv)) continue;
    for (const Reading& x : n) {
      if (!nominal(x)) continue;
      if (x.case_ == vp::feat::Nom && agrees(x, vv)) return true;
      if (x.case_ == vp::feat::Acc || adjunct(x.case_)) return true;
    }
  }
  return false;
}

// "in N V": the preposition takes the ablative (place) or the accusative (motion); any finite verb.
bool okInNV(const std::vector<Reading>& n, const std::vector<Reading>& v) {
  bool fin = false;
  for (const Reading& vv : v) fin = fin || finite(vv);
  if (!fin) return false;
  for (const Reading& x : n)
    if (x.case_ == vp::feat::Abl || x.case_ == vp::feat::Acc) return true;
  return false;
}

std::string clause(const std::vector<std::string>& words) {
  std::string out;
  for (const std::string& w : words) {
    if (!out.empty()) out += ' ';
    out += w;
  }
  if (!out.empty() && out[0] >= 'a' && out[0] <= 'z') out[0] = static_cast<char>(out[0] - 'a' + 'A');
  out += '.';
  return out;
}

const char* corruptionKind(const Features& a, const Features& b) {
  if (a.person != b.person) return "person";
  if (a.case_ != b.case_) return "case";
  if (a.number != b.number) return "number";
  if (a.gender != b.gender) return "gender";
  return "other";
}

bool sameSlot(const Features& a, const Features& b) {
  return a.case_ == b.case_ && a.number == b.number && a.gender == b.gender && a.person == b.person &&
         a.tense == b.tense && a.mood == b.mood && a.voice == b.voice && a.degree == b.degree;
}

// A different cell of the same lemma (same tense/mood/voice for verbs; same degree for adjectives).
const Cell* otherCell(const Word& w, const Cell& orig, bool verb, Rng& rng) {
  std::vector<const Cell*> cands;
  for (const Cell& c : w.cells) {
    if (sameSlot(c.f, orig.f) || c.form == orig.form) continue;
    if (verb) {
      if (c.f.tense != orig.f.tense || c.f.mood != orig.f.mood || c.f.voice != orig.f.voice || c.f.person == 0) continue;
    } else {
      if (c.f.case_ == 0 || c.f.case_ == vp::feat::Voc) continue;
      if (c.f.degree != orig.f.degree && !(c.f.degree <= 1 && orig.f.degree <= 1)) continue;
      if (c.f.mood != 0) continue;   // gerunds and the like
    }
    cands.push_back(&c);
  }
  return cands.empty() ? nullptr : cands[rng.below(cands.size())];
}

}  // namespace

Result<std::vector<GatePair>> buildGatePairs(const lex::Lexicon& lx, int n, uint64_t seed) {
  try {
    if (n <= 0 || n > 100000) return Error{ErrorCode::BadParams, "pairs must be 1..100000", "Choose a pair count."};
    // Pools: tier-1/2 lemmas with tables.
    std::vector<Word> nouns, adjs, verbs, transitive;
    std::vector<lex::Sense> senses;
    const uint32_t count = lx.lemmaCount();
    for (uint32_t id = 0; id < count; ++id) {
      const lex::Lemma l = lx.lemma(id);
      if (l.tier != 1 && l.tier != 2) continue;
      if (!(l.flags & lex::HasTable) || (l.flags & (lex::ProperName | lex::Indeclinable | lex::Defective))) continue;
      Word w;
      w.lemma = id;
      w.gender = l.gender;
      if (l.pos == vp::feat::Noun) {
        if ((l.flags & lex::PluralOnly) || l.gender < vp::feat::M || l.gender > vp::feat::N) continue;
        loadCells(lx, id, w);
        if (findCell(w, vp::feat::Nom, vp::feat::Sg) && findCell(w, vp::feat::Acc, vp::feat::Sg) &&
            findCell(w, vp::feat::Abl, vp::feat::Sg) && findCell(w, vp::feat::Nom, vp::feat::Pl))
          nouns.push_back(std::move(w));
      } else if (l.pos == vp::feat::Adj) {
        loadCells(lx, id, w);
        if (findCell(w, vp::feat::Nom, vp::feat::Sg, vp::feat::M) && findCell(w, vp::feat::Nom, vp::feat::Sg, vp::feat::F) &&
            findCell(w, vp::feat::Nom, vp::feat::Sg, vp::feat::N))
          adjs.push_back(std::move(w));
      } else if (l.pos == vp::feat::Verb) {
        if (l.flags & (lex::Deponent | lex::Impersonal)) continue;
        loadCells(lx, id, w);
        bool all = true;
        for (uint8_t p = 1; p <= 3; ++p)
          for (uint8_t num = 1; num <= 2; ++num) all = all && findVerb(w, p, num) != nullptr;
        if (!all) continue;
        senses.clear();
        lx.senses(id, senses);
        bool trans = false;
        for (const lex::Sense& s : senses) trans = trans || (s.tags & 1u);
        if (trans) transitive.push_back(w);
        verbs.push_back(std::move(w));
      }
    }
    if (nouns.size() < 2 || adjs.empty() || verbs.empty())
      return Error{ErrorCode::LexiconCorrupt, "the lexicon has too few tier-1/2 nouns, adjectives or verbs with tables",
                   "Use the full Latin lexicon (latin.vpl) for the gate."};
    if (transitive.empty()) transitive = verbs;

    Rng rng(seed);
    Readings rd(lx);
    std::set<std::string> seen;
    std::vector<GatePair> out;
    const int maxAttempts = n * 400;
    for (int attempt = 0; attempt < maxAttempts && static_cast<int>(out.size()) < n; ++attempt) {
      const int t = attempt % 4;
      GatePair p;
      std::vector<std::string> good, bad;
      bool okGood = false, okBad = true;
      if (t == 0) {   // N A est
        const Word& nw = nouns[rng.below(nouns.size())];
        const Word& aw = adjs[rng.below(adjs.size())];
        const Cell* nc = findCell(nw, vp::feat::Nom, vp::feat::Sg);
        const Cell* ac = findCell(aw, vp::feat::Nom, vp::feat::Sg, nw.gender);
        if (!nc || !ac) continue;
        const Cell* bc = otherCell(aw, *ac, false, rng);
        if (!bc) continue;
        p.templ = "N-A-est";
        p.corruption = corruptionKind(ac->f, bc->f);
        good = {nc->form, ac->form, "est"};
        bad = {nc->form, bc->form, "est"};
        const std::vector<Reading> rn = rd.of(nc->form);
        okGood = okNAEst(rn, rd.of(ac->form));
        okBad = okNAEst(rn, rd.of(bc->form));
      } else if (t == 1) {   // S O V
        const Word& sw = nouns[rng.below(nouns.size())];
        const Word& ow = nouns[rng.below(nouns.size())];
        const Word& vw = transitive[rng.below(transitive.size())];
        if (sw.lemma == ow.lemma) continue;
        const Cell* sc = findCell(sw, vp::feat::Nom, vp::feat::Sg);
        const Cell* oc = findCell(ow, vp::feat::Acc, vp::feat::Sg);
        const Cell* vc = findVerb(vw, vp::feat::P3, vp::feat::Sg);
        if (!sc || !oc || !vc) continue;
        p.templ = "S-O-V";
        good = {sc->form, oc->form, vc->form};
        const std::vector<Reading> rs = rd.of(sc->form), ro = rd.of(oc->form), rv = rd.of(vc->form);
        okGood = okSOV(rs, ro, rv);
        if (rng.below(2) == 0) {
          const Cell* bc = otherCell(ow, *oc, false, rng);
          if (!bc) continue;
          p.corruption = corruptionKind(oc->f, bc->f);
          bad = {sc->form, bc->form, vc->form};
          okBad = okSOV(rs, rd.of(bc->form), rv);
        } else {
          const Cell* bc = otherCell(vw, *vc, true, rng);
          if (!bc) continue;
          p.corruption = corruptionKind(vc->f, bc->f);
          bad = {sc->form, oc->form, bc->form};
          okBad = okSOV(rs, ro, rd.of(bc->form));
        }
      } else if (t == 2) {   // Npl Vpl
        const Word& nw = nouns[rng.below(nouns.size())];
        const Word& vw = verbs[rng.below(verbs.size())];
        const Cell* nc = findCell(nw, vp::feat::Nom, vp::feat::Pl);
        const Cell* vc = findVerb(vw, vp::feat::P3, vp::feat::Pl);
        if (!nc || !vc) continue;
        p.templ = "Npl-Vpl";
        good = {nc->form, vc->form};
        const std::vector<Reading> rn = rd.of(nc->form), rv = rd.of(vc->form);
        okGood = okNV(rn, rv);
        if (rng.below(2) == 0) {
          const Cell* bc = otherCell(nw, *nc, false, rng);
          if (!bc) continue;
          p.corruption = corruptionKind(nc->f, bc->f);
          bad = {bc->form, vc->form};
          okBad = okNV(rd.of(bc->form), rv);
        } else {
          const Cell* bc = otherCell(vw, *vc, true, rng);
          if (!bc) continue;
          p.corruption = corruptionKind(vc->f, bc->f);
          bad = {nc->form, bc->form};
          okBad = okNV(rn, rd.of(bc->form));
        }
      } else {   // in N V
        const Word& nw = nouns[rng.below(nouns.size())];
        const Word& vw = verbs[rng.below(verbs.size())];
        const Cell* nc = findCell(nw, vp::feat::Abl, vp::feat::Sg);
        const Cell* vc = findVerb(vw, vp::feat::P3, vp::feat::Sg);
        if (!nc || !vc) continue;
        const Cell* bc = otherCell(nw, *nc, false, rng);
        if (!bc) continue;
        p.templ = "in-N-V";
        p.corruption = corruptionKind(nc->f, bc->f);
        good = {"in", nc->form, vc->form};
        bad = {"in", bc->form, vc->form};
        const std::vector<Reading> rv = rd.of(vc->form);
        okGood = okInNV(rd.of(nc->form), rv);
        okBad = okInNV(rd.of(bc->form), rv);
      }
      if (!okGood || okBad) continue;
      p.good = clause(good);
      p.bad = clause(bad);
      if (p.good == p.bad || !seen.insert(p.bad).second) continue;
      out.push_back(std::move(p));
    }
    return out;
  } catch (const std::exception& e) {
    return Error{ErrorCode::Internal, std::string("buildGatePairs: ") + e.what(), "The gate could not be built."};
  } catch (...) {
    return Error{ErrorCode::Internal, "buildGatePairs: unknown error", "The gate could not be built."};
  }
}

Result<GateResult> runGate(const lex::Lexicon& lx, const GateOptions& opt) {
  try {
    Result<std::vector<GatePair>> pairs = buildGatePairs(lx, opt.pairs, opt.seed);
    if (!pairs) return pairs.error();
    GateResult res;
    res.pairs = std::move(pairs.value());
    Model model;
    Result<void> ld = model.load(opt.model);
    if (!ld) return ld.error();
    res.loadMs = model.status().lastLoadMs;
    const auto t0 = std::chrono::steady_clock::now();
    const int total = static_cast<int>(res.pairs.size());
    int done = 0;
    for (GatePair& p : res.pairs) {
      Result<TextScore> g = model.scoreText(prompts::kGatePrefix, " " + p.good);
      if (!g) return g.error();
      Result<TextScore> b = model.scoreText(prompts::kGatePrefix, " " + p.bad);
      if (!b) return b.error();
      p.goodLp = g->logProb;
      p.goodTokens = g->tokens;
      p.badLp = b->logProb;
      p.badTokens = b->tokens;
      if (p.goodLp > p.badLp) ++res.correct;
      if (p.goodTokens > 0 && p.badTokens > 0 && p.goodLp / p.goodTokens > p.badLp / p.badTokens) ++res.correctMean;
      ++done;
      if (opt.progress) opt.progress(done, total);
    }
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    model.unload();
    res.msPerPair = total > 0 ? ms / total : 0.0;
    res.sum = wilson(res.correct, total);
    res.mean = wilson(res.correctMean, total);
    res.passed = total > 0 && res.sum.p >= 0.75 && res.sum.lo > 0.70;
    return res;
  } catch (const std::exception& e) {
    return Error{ErrorCode::Internal, std::string("runGate: ") + e.what(), "The gate could not run."};
  } catch (...) {
    return Error{ErrorCode::Internal, "runGate: unknown error", "The gate could not run."};
  }
}

}  // namespace vp::llm
