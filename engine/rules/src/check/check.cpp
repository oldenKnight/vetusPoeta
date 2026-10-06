// LatinChecker (vp/check.h): tokenise, re-analyse, A1 A2 A3 A4 A6.
#include "vp/check.h"

#include <algorithm>

#include "vp/features.h"
#include "vp/realise_la.h"
#include "vp/text.h"

namespace vp::check {

using namespace vp::feat;

bool Report::ok(std::string_view id) const {
  for (const rules::Check& c : checks)
    if (c.id == id) return c.ok;
  return true;
}
size_t Report::count(std::string_view id, bool warnings) const {
  size_t n = 0;
  for (const Issue& i : issues)
    if (i.id == id && i.warning == warnings) ++n;
  return n;
}

struct LatinChecker::Reading {
  uint32_t lemma = 0;
  Features f;
  uint8_t lpos = 0, lgender = 0, tier = 0, whit = 0;
  uint16_t lflags = 0, aflags = 0;
  std::string_view key;   // lemma key
};

namespace {

bool isWordCp(char32_t c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= 0xC0 && c <= 0x24F && c != 0xD7 && c != 0xF7) ||
         (c >= 0x1E00 && c <= 0x1EFF) || (c >= 0x0300 && c <= 0x036F) || (c >= 0x370 && c <= 0x3FF) ||
         (c >= 0x1F00 && c <= 0x1FFF);
}
bool isBoundaryPunct(char32_t c) {
  return c == ',' || c == ';' || c == ':' || c == '.' || c == '!' || c == '?' || c == '(' || c == ')' ||
         c == 0x2014 || c == 0x2013 || c == '"' || c == 0x201C || c == 0x201D;
}
bool isSentenceEnd(char32_t c) { return c == '.' || c == '!' || c == '?'; }
bool hasMacron(std::string_view s) {
  const std::string d = text::nfd(s);
  return d.find("\xCC\x84") != std::string::npos;   // U+0304
}
uint8_t genderMask(uint8_t g) {
  switch (g) {
    case M: return 1;
    case F: return 2;
    case N: return 4;
    case MF: return 3;
    case MN: return 5;
    case FN: return 6;
    default: return 7;
  }
}
bool genderCompat(uint8_t a, uint8_t b) { return (genderMask(a) & genderMask(b)) != 0; }
bool numberCompat(uint8_t a, uint8_t b) { return a == 0 || b == 0 || a == b; }

}  // namespace

struct LatinChecker::Impl {
  const lex::Lexicon& lx;
  const curated::CuratedData& cd;
  const std::vector<std::string>& nameKeys;
  const Options& opt;
  Report& rep;
  std::vector<std::vector<Reading>> rd;   // readings per token
  std::vector<char> governed, attached, isVerb, boundaryBefore, relStart;

  // ---- reading classes ----
  static bool isFinite(const Reading& r) {
    return (r.lpos == Verb || r.f.pos == Verb) && (r.f.mood == Indicative || r.f.mood == Subjunctive ||
                                                    r.f.mood == Imperative) && r.f.person != 0;
  }
  static bool isInfinitive(const Reading& r) { return r.lpos == Verb && r.f.mood == Infinitive; }
  static bool isHead(const Reading& r) {
    return (r.lpos == Noun || r.lpos == Name) || (r.lpos == Pron && r.f.case_ != 0);
  }
  static bool isModifier(const Reading& r) {
    if (r.f.case_ == 0) return false;
    if (r.lpos == Adj || r.lpos == Det || r.lpos == Num || r.lpos == Participle) return true;
    if (r.lpos == Verb && r.f.mood == ParticipleMood) return true;
    if (r.lpos == Pron && r.f.gender != 0 && r.key != "qui") return true;
    return false;
  }
  static bool isNominal(const Reading& r) { return isHead(r) || isModifier(r); }
  // A citation-only entry (an adjective / noun / participle headword carried without a case) never decides anything.
  static bool citationOnly(const Reading& r) {
    if (r.f.case_ != 0) return false;
    if (r.lpos == Adj || r.lpos == Noun || r.lpos == Name || r.lpos == Participle || r.lpos == Num || r.lpos == feat::Det ||
        r.lpos == Pron)
      return true;
    return r.lpos == Verb && r.f.mood == ParticipleMood;
  }
  static bool isRelative(const Reading& r) { return r.lpos == Pron && r.key == "qui"; }
  uint8_t headGender(const Reading& r) const { return r.f.gender ? r.f.gender : r.lgender; }
  uint8_t personOf(const Reading& r) const {
    if (r.lpos == Pron) {
      if (r.key == "ego" || r.key == "nos") return 1;
      if (r.key == "tu" || r.key == "uos") return 2;
    }
    return 3;
  }

  bool any(size_t i, bool (*pred)(const Reading&)) const {
    for (const Reading& r : rd[i])
      if (pred(r)) return true;
    return false;
  }
  bool allNominal(size_t i) const {
    bool seen = false;
    for (const Reading& r : rd[i]) {
      if (citationOnly(r)) continue;
      if (!isNominal(r)) return false;
      seen = true;
    }
    return seen;
  }
  bool hasCase(size_t i, uint8_t c) const {
    for (const Reading& r : rd[i])
      if (isNominal(r) && r.f.case_ == c) return true;
    return false;
  }
  bool definiteCase(size_t i, uint8_t c) const {   // every reading nominal and in case c (vocative counts as nom)
    if (!allNominal(i)) return false;
    bool seen = false;
    for (const Reading& r : rd[i]) {
      if (citationOnly(r)) continue;
      if (r.f.case_ == c) seen = true;
      else if (!(c == Nom && r.f.case_ == Voc)) return false;
    }
    return seen;
  }
  bool keyIs(size_t i, std::initializer_list<const char*> keys) const {
    for (const char* k : keys)
      if (rep.tokens[i].analysis.key == k) return true;
    return false;
  }
  bool isCopula(size_t i) const {
    for (const Reading& r : rd[i])
      if (isFinite(r) && r.key == "sum") return true;
    return false;
  }
  void issue(const char* id, int tok, std::string detail, bool warning = false) {
    rep.issues.push_back(Issue{id, tok, std::move(detail), warning});
  }
  const std::string& T(size_t i) const { return rep.tokens[i].text; }

  void tokenise(std::string_view text) {
    size_t i = 0;
    bool boundary = true, sentStart = true;
    while (i < text.size()) {
      size_t j = i;
      const char32_t c = text::decodeUtf8(text, j);
      if (isWordCp(c)) {
        size_t start = i, end = j;
        while (end < text.size()) {
          size_t k = end;
          const char32_t d = text::decodeUtf8(text, k);
          if (!isWordCp(d)) break;
          end = k;
        }
        CheckedToken t;
        t.text = std::string(text.substr(start, end - start));
        t.start = (int)start;
        t.end = (int)end;
        t.sentenceInitial = sentStart;
        rep.tokens.push_back(std::move(t));
        boundaryBefore.push_back(boundary);
        boundary = sentStart = false;
        i = end;
      } else {
        if (isBoundaryPunct(c)) boundary = true;
        if (isSentenceEnd(c)) sentStart = true;
        i = j;
      }
    }
  }

  void analyse(bool macrons) {
    macronMode = macrons;
    rd.resize(rep.tokens.size());
    std::string tokCanon, dispCanon;
    for (size_t i = 0; i < rep.tokens.size(); ++i) {
      CheckedToken& t = rep.tokens[i];
      morph::analyseLatin(lx, t.text, t.analysis);
      std::vector<Reading>& v = rd[i];
      v.clear();
      // the written form without the enclitic, canonical (anceps plain, lower case)
      std::string base = t.text;
      if (t.analysis.enclitic) {
        const std::string low = text::lower(text::nfc(base));
        base = low.substr(0, low.size() >= t.analysis.encliticText.size() ? low.size() - t.analysis.encliticText.size() : 0);
      }
      tokCanon = text::lower(morph::displayForm(base, true));
      auto add = [&](uint32_t lemma, uint32_t packed, std::string_view display, uint16_t aflags) {
        if (strict) {
          dispCanon = text::lower(morph::displayForm(display, true));
          if (dispCanon != tokCanon) return false;
        }
        const lex::Lemma l = lx.lemma(lemma);
        Reading r;
        r.lemma = lemma;
        r.f = unpack(packed);
        r.lpos = l.pos;
        r.lgender = l.gender;
        r.lflags = l.flags;
        r.aflags = aflags;
        r.key = l.key;
        r.tier = l.tier;
        r.whit = l.whitFreq;
        v.push_back(r);
        return true;
      };
      for (int pass = 0; pass < 2 && v.empty(); ++pass) {
        strict = macrons && pass == 0;
        for (const lex::Analysis& a : t.analysis.analyses) add(a.lemma, lx.feature(a.feat), a.display, a.flags);
        for (const morph::RuleAnalysis& a : t.analysis.ruleAnalyses) add(a.lemma, a.packed, a.display, 0);
      }
      // alternative spellings, abbreviations and symbol/affix lemmas only when nothing else reads the word
      // (ā/ab, ē/ex are regular variants of closed-class words and stay)
      auto weak = [](const Reading& r) {
        return ((r.aflags & lex::AltSpelling) && r.lpos != Prep && r.lpos != Conj) || r.lpos == Symbol ||
               r.lpos == Suffix || r.lpos == Prefix || r.lpos == Phrase;
      };
      if (std::any_of(v.begin(), v.end(), [&](const Reading& r) { return !weak(r); }))
        v.erase(std::remove_if(v.begin(), v.end(), weak), v.end());
      // a closed-class word (preposition, conjunction, particle) outranks letter names, interjections and
      // uninflected noun entries spelled the same ("ā", "in", "et")
      if (std::any_of(v.begin(), v.end(), [](const Reading& r) { return r.lpos == Prep || r.lpos == Conj || r.lpos == Particle; }))
        v.erase(std::remove_if(v.begin(), v.end(), [](const Reading& r) {
                  return r.lpos == Intj || ((r.lpos == Noun || r.lpos == Name) && (r.f.case_ == 0 || (r.lflags & lex::Indeclinable)));
                }), v.end());
      if (t.analysis.fromRule) { t.fromRule = true; rep.fromRule = true; }
    }
  }

  void names() {
    for (size_t i = 0; i < rep.tokens.size(); ++i) {
      CheckedToken& t = rep.tokens[i];
      const std::string& k = t.analysis.key;
      if (rd[i].empty() && std::binary_search(nameKeys.begin(), nameKeys.end(), k)) t.name = true;
      if (rd[i].empty() && opt.glossary)
        for (const rules::GlossaryEntry& g : *opt.glossary)
          if (text::latin_key(g.form.empty() ? g.name : g.form) == k || text::latin_key(g.name) == k) t.name = true;
      if (t.analysis.capitalised && !t.sentenceInitial && rd[i].empty()) t.name = true;
      if (t.analysis.capitalised && !rd[i].empty() &&
          std::all_of(rd[i].begin(), rd[i].end(), [](const Reading& r) { return (r.lflags & lex::ProperName) || r.lpos == Name; }))
        t.name = true;
    }
    if (opt.hints)
      for (const TokenHint& h : *opt.hints)
        for (CheckedToken& t : rep.tokens)
          if (t.start >= h.start && t.end <= h.end) {
            if (h.name) t.name = true;
            if (h.fromRule) { t.fromRule = true; rep.fromRule = true; }
          }
  }

  void segment() {
    const size_t n = rep.tokens.size();
    relStart.assign(n, 0);
    int seg = 0;
    for (size_t i = 0; i < n; ++i) {
      bool cut = boundaryBefore[i] && i > 0;
      if (i == 0 && keyIs(i, {"cum"}) && any(i, [](const Reading& r) { return r.lpos == Conj; })) relStart[i] = 0;
      if (i > 0 && !cut) {
        bool conj = false;
        for (const Reading& r : rd[i])
          if (r.lpos == Conj) conj = true;
        if (conj && keyIs(i, {"quod", "quia", "si", "nisi", "ut", "ne", "antequam", "postquam", "dum", "quamquam"}))
          cut = true;
        if (conj && keyIs(i, {"cum"}) && boundaryBefore[i]) cut = true;   // mid-clause cum is the preposition
        bool rel = false;
        for (const Reading& r : rd[i])
          if (isRelative(r)) rel = true;
        if (rel && any(i - 1, isHead)) { cut = true; relStart[i] = 1; }
      }
      if (cut) ++seg;
      rep.tokens[i].segment = seg;
    }
    // verb tokens: finite readings and either no nominal reading or segment-final
    isVerb.assign(n, 0);
    for (size_t i = 0; i < n; ++i) {
      if (!any(i, isFinite)) continue;
      const bool last = i + 1 == n || rep.tokens[i + 1].segment != rep.tokens[i].segment;
      const bool first = i == 0 || rep.tokens[i - 1].segment != rep.tokens[i].segment;
      bool nominal = false, adv = false, imperative = false;
      for (const Reading& r : rd[i]) {
        nominal = nominal || isNominal(r);
        adv = adv || r.lpos == Adv || r.lpos == Conj || r.lpos == Prep;
        imperative = imperative || (isFinite(r) && r.f.mood == Imperative);
      }
      const bool ne = rep.tokens[i].analysis.enclitic && rep.tokens[i].analysis.encliticText == "ne";
      if ((!nominal && !adv) || last || ne || (imperative && first && !strongHead(i))) isVerb[i] = 1;
    }
    // a segment without a verb takes its only finite candidate
    for (size_t b = 0; b < n;) {
      const size_t e = segEnd(b);
      size_t cands = 0, which = n;
      bool has = false;
      for (size_t j = b; j < e; ++j) {
        has = has || isVerb[j];
        if (any(j, isFinite)) { ++cands; which = j; }
      }
      if (!has && cands == 1) isVerb[which] = 1;
      b = e;
    }
    // coordinated clauses: split a segment with two verbs at et / sed / neque / atque between them
    for (size_t i = 0; i < n; ++i) {
      if (!keyIs(i, {"et", "sed", "neque", "atque", "nec"})) continue;
      bool before = false, after = false;
      for (size_t j = i; j-- > 0 && rep.tokens[j].segment == rep.tokens[i].segment;) before = before || isVerb[j];
      for (size_t j = i + 1; j < n && rep.tokens[j].segment == rep.tokens[i].segment; ++j) after = after || isVerb[j];
      if (before && after)
        for (size_t j = i; j < n; ++j) rep.tokens[j].segment += 1;
    }
  }

  size_t segEnd(size_t i) const {
    size_t j = i;
    while (j < rep.tokens.size() && rep.tokens[j].segment == rep.tokens[i].segment) ++j;
    return j;
  }
  size_t segBegin(size_t i) const {
    size_t j = i;
    while (j > 0 && rep.tokens[j - 1].segment == rep.tokens[i].segment) --j;
    return j;
  }

  // ---- A4 (prepositions) — also marks the tokens a preposition governs ----
  void prepositions() {
    const size_t n = rep.tokens.size();
    governed.assign(n, 0);
    for (size_t i = 0; i + 1 < n; ++i) {
      if (!any(i, [](const Reading& r) { return r.lpos == Prep; })) continue;
      const uint16_t cases = cd.prepCases(rep.tokens[i].analysis.key);
      const size_t x = i + 1;
      if (!cases) continue;
      if (boundaryBefore[x] || rd[x].empty()) continue;
      bool nominal = false;
      for (const Reading& r : rd[x]) nominal = nominal || isNominal(r);
      if (!nominal) continue;
      bool otherUse = false;   // cum + verb (conjunction), post / ante as adverbs before a verb
      for (const Reading& r : rd[i]) otherUse = otherUse || r.lpos == Conj || r.lpos == Adv;
      bool ok = false;
      for (const Reading& r : rd[x])
        if (isNominal(r) && (cases & (1u << r.f.case_))) ok = true;
      if (!ok) {
        // cum / post / ante as conjunction or adverb: at the start of a clause ("Cum māter vēnit")
        if (otherUse && (boundaryBefore[i] || i == segBegin(i))) {
          size_t verbs = 0;
          for (size_t j = 0; j < n; ++j) verbs += isVerb[j];
          if (verbs >= 2) continue;   // "Cum māter vēnit, puer dormiēbat"; one verb: it is the preposition
        }
        issue("A4", (int)x, "preposition '" + T(i) + "' does not govern the case of '" + T(x) + "'");
        continue;
      }
      governed[x] = 1;
      for (size_t j = x + 1; j < n && rep.tokens[j].segment == rep.tokens[x].segment && !boundaryBefore[j]; ++j) {
        bool agrees = false;
        for (const Reading& a : rd[j])
          for (const Reading& b : rd[x])
            if (isModifier(a) && isNominal(b) && (cases & (1u << a.f.case_)) && a.f.case_ == b.f.case_ &&
                numberCompat(a.f.number, b.f.number))
              agrees = true;
        if (!agrees) break;
        governed[j] = 1;
      }
    }
  }

  // ---- A3 ----
  bool agreePair(size_t m, size_t h) const {
    for (const Reading& a : rd[m]) {
      if (!isModifier(a)) continue;
      for (const Reading& b : rd[h]) {
        if (!isHead(b)) continue;
        if (a.f.case_ == b.f.case_ && numberCompat(a.f.number, b.f.number) && genderCompat(a.f.gender, headGender(b)))
          return true;
      }
    }
    return false;
  }

  // Predicate agreement in a copula segment: nominative (accusative with "esse"), number of the copula, gender of
  // the subject when there is a definite nominative noun.
  bool predicateOk(size_t m, size_t b, size_t e) const {
    std::vector<uint8_t> numbers;
    bool esse = false;
    for (size_t j = b; j < e; ++j) {
      if (isVerb[j] && isCopula(j))
        for (const Reading& r : rd[j])
          if (isFinite(r) && r.key == "sum") numbers.push_back(r.f.number);
      for (const Reading& r : rd[j])
        if (isInfinitive(r) && r.key == "sum") esse = true;
    }
    if (numbers.empty() && !esse) return false;
    // (number, gender) of every nominative reading of every possible subject
    std::vector<std::pair<uint8_t, uint8_t>> subjects;
    for (size_t j = b; j < e; ++j) {
      if (j == m || governed[j] || attached[j] || isVerb[j] || !strongHead(j)) continue;
      for (const Reading& r : rd[j])
        if (isHead(r) && (r.f.case_ == Nom || (esse && r.f.case_ == Acc))) subjects.emplace_back(r.f.number, headGender(r));
    }
    for (const Reading& r : rd[m]) {
      if (!isModifier(r)) continue;
      if (!(r.f.case_ == Nom || (esse && r.f.case_ == Acc))) continue;
      bool num = esse;
      for (uint8_t nn : numbers) num = num || numberCompat(r.f.number, nn);
      if (!num) continue;
      if (subjects.empty()) return true;
      for (const auto& sj : subjects)
        if (numberCompat(r.f.number, sj.first) && genderCompat(r.f.gender, sj.second)) return true;
    }
    return false;
  }

  // A pure modifier m and a head h "partially agree" when some reading pair shares the case and either the number
  // or the gender: the sign of an agreement slip rather than of two different phrases.
  bool partialPair(size_t m, size_t h) const {
    for (const Reading& a : rd[m]) {
      if (!isModifier(a)) continue;
      for (const Reading& b : rd[h])
        if (isHead(b) && a.f.case_ == b.f.case_) return true;   // same case next to each other: an agreement slip
    }
    return false;
  }
  bool verbish(size_t i) const { return isVerb[i] || any(i, isInfinitive); }
  uint16_t argMask(size_t i) const {
    uint16_t m = 0;
    for (const Reading& r : rd[i])
      if (isNominal(r) && r.f.case_ != Voc) m |= (uint16_t)(1u << r.f.case_);
    return m;
  }
  // A token is a head when it has a noun / pronoun reading that is not just the substantive twin of one of its
  // adjective readings (parvus "a small one", tier 3, next to parvus adj, tier 1).
  bool strongHead(size_t i) const {
    for (const Reading& r : rd[i]) {
      if (!isHead(r)) continue;
      if (r.lpos == Pron) return true;
      const uint8_t nounTier = r.tier ? r.tier : 3;
      bool twin = false;
      for (const Reading& a : rd[i]) {
        if (!isModifier(a) || a.lemma == r.lemma) continue;
        const uint8_t adjTier = a.tier ? a.tier : 3;
        std::string_view ak = a.key;
        if (ak == r.key && adjTier <= nounTier) { twin = true; continue; }   // bonus adj / bonus "a good man"
        if (nounTier >= 3 && adjTier <= 1) { twin = true; continue; }   // magnās "magnate" next to magnus
        if (adjTier >= nounTier) continue;   // a derived key needs the adjective to be clearly the core word
        std::string stem;
        if (ak.size() > 2 && ak.substr(ak.size() - 2) == "us") stem = std::string(ak.substr(0, ak.size() - 2));
        else if (ak.size() > 2 && ak.substr(ak.size() - 2) == "er") stem = std::string(ak.substr(0, ak.size() - 2)) + "r";
        if (!stem.empty() && (r.key == stem + "um" || r.key == stem + "a" || r.key == stem + "us")) twin = true;
      }
      if (!twin) return true;
    }
    return false;
  }
  bool macronMode = false, strict = false;
  // Subject candidate: nominative (vocative counts); without macrons in the text the ablative homograph of the
  // first declension (puella / puellā) does not disqualify.
  bool subjectCase(size_t i) const {
    if (definiteCase(i, Nom)) return true;
    if (macronMode || !allNominal(i)) return false;
    bool nom = false;
    for (const Reading& r : rd[i]) {
      if (citationOnly(r)) continue;
      if (r.f.case_ == Nom) nom = true;
      else if (r.f.case_ != Voc && r.f.case_ != Abl) return false;
    }
    return nom;
  }

  void agreement() {
    const size_t n = rep.tokens.size();
    attached.assign(n, 0);
    // (1) + (2): modifiers. A token that can be a noun (or pronoun) on its own is a head, never checked as a
    // modifier; a pure modifier must agree with an adjacent head, or be a predicate of a copula, or stand alone as a
    // substantive (then it may not partially agree with a neighbour and may not follow a preposition's noun).
    for (size_t i = 0; i < n; ++i) {
      if (!any(i, isModifier) || strongHead(i) || verbish(i) || relStart[i] || rep.tokens[i].name) continue;
      bool relOnly = true;
      for (const Reading& r : rd[i]) relOnly = relOnly && isRelative(r);
      if (relOnly) continue;
      const int seg = rep.tokens[i].segment;
      const size_t b = segBegin(i), e = segEnd(i);
      std::vector<size_t> cand;
      for (int dir : {-1, 1}) {
        long j = (long)i + dir;
        while (j >= 0 && j < (long)n && rep.tokens[(size_t)j].segment == seg && !verbish((size_t)j) &&
               !strongHead((size_t)j) && !(dir == 1 && boundaryBefore[(size_t)j]) &&
               !(dir == -1 && boundaryBefore[(size_t)j + 1]))
          j += dir;
        if (j < 0 || j >= (long)n || rep.tokens[(size_t)j].segment != seg) continue;
        if (dir == 1 && boundaryBefore[(size_t)j]) continue;
        if (dir == -1 && boundaryBefore[(size_t)j + 1]) continue;
        if (strongHead((size_t)j) && !verbish((size_t)j)) cand.push_back((size_t)j);
      }
      bool ok = false;
      for (size_t h : cand) ok = ok || agreePair(i, h);
      if (ok) { attached[i] = 1; continue; }
      bool copula = false;
      for (size_t j = b; j < e; ++j) copula = copula || (isVerb[j] && isCopula(j));
      if (copula && predicateOk(i, b, e)) continue;
      if (cand.empty()) {
        if (copula) issue("A3", (int)i, "predicate '" + T(i) + "' does not agree with the subject / verb");
        continue;
      }
      // a quantity noun before a partitive genitive ("paulum thēae", "satis aquae"): the noun reading heads (C13)
      if (any(i, isHead)) {
        bool gen = !cand.empty();
        for (size_t h : cand) {
          bool g = false;
          for (const Reading& r : rd[h]) g = g || (isHead(r) && r.f.case_ == Gen);
          gen = gen && h > i && g;
        }
        if (gen) continue;
      }
      bool partial = false;
      for (size_t h : cand) partial = partial || partialPair(i, h);
      const bool afterGoverned = i > 0 && governed[i - 1] && !boundaryBefore[i] && std::find(cand.begin(), cand.end(), i - 1) != cand.end();
      if (partial || afterGoverned || copula)
        issue("A3", (int)i, "'" + T(i) + "' does not agree with '" + T(cand[0]) + "' (case, number, gender)");
    }
    // (3) subject - verb
    for (size_t b = 0; b < n;) {
      const size_t e = segEnd(b);
      std::vector<size_t> verbs, subj;
      bool copula = false;
      for (size_t j = b; j < e; ++j) {
        if (isVerb[j]) { verbs.push_back(j); copula = copula || isCopula(j); }
      }
      for (size_t j = b; j < e; ++j) {
        if (isVerb[j] || governed[j] || attached[j] || rep.tokens[j].name || relStart[j]) continue;
        if (!subjectCase(j)) continue;
        bool rel = false;
        for (const Reading& r : rd[j]) rel = rel || isRelative(r);
        if (rel) continue;
        subj.push_back(j);
      }
      bool imperativeOnly = !verbs.empty();
      for (size_t v : verbs)
        for (const Reading& r : rd[v])
          if (isFinite(r) && r.f.mood != Imperative) imperativeOnly = false;
      if (!verbs.empty() && !subj.empty() && !imperativeOnly) {
        // groups joined by et / atque / -que are plural
        std::vector<std::pair<std::vector<size_t>, bool>> groups;
        for (size_t k = 0; k < subj.size(); ++k) {
          const size_t j = subj[k];
          bool join = false;
          if (!groups.empty()) {
            const size_t prev = groups.back().first.back();
            bool onlyMods = true, et = false;
            for (size_t x = prev + 1; x < j; ++x) {
              if (keyIs(x, {"et", "atque", "ac"})) et = true;
              else if (!attached[x]) onlyMods = false;
            }
            if ((et && onlyMods) || (rep.tokens[j].analysis.enclitic && rep.tokens[j].analysis.encliticText == "que" && onlyMods))
              join = true;
          }
          if (join) { groups.back().first.push_back(j); groups.back().second = true; }
          else groups.push_back({{j}, false});
        }
        bool anyAgree = false, allAgree = true;
        size_t bad = subj[0];
        for (const auto& g : groups) {
          bool agrees = false;
          for (size_t v : verbs)
            for (const Reading& vr : rd[v]) {
              if (!isFinite(vr)) continue;
              if (g.second) {   // coordinated: plural, lowest person
                uint8_t person = 3;
                for (size_t j : g.first)
                  for (const Reading& r : rd[j])
                    if (isHead(r) && personOf(r) < person) person = personOf(r);
                if (vr.f.number == Pl && (copula || vr.f.person == person)) agrees = true;
                continue;
              }
              for (size_t j : g.first)
                for (const Reading& r : rd[j]) {
                  if (!isNominal(r) || r.f.case_ != Nom) continue;
                  if (!numberCompat(r.f.number, vr.f.number)) continue;
                  if (!copula && isHead(r) && personOf(r) != vr.f.person) continue;
                  if (!copula && !isHead(r) && vr.f.person != 3) continue;
                  agrees = true;
                }
            }
          anyAgree = anyAgree || agrees;
          if (!agrees && allAgree) { allAgree = false; bad = g.first[0]; }
        }
        if (copula ? !anyAgree : !allAgree)
          issue("A3", (int)bad, "subject '" + T(bad) + "' does not agree with the verb '" + T(verbs[0]) + "'");
      }
      b = e;
    }
    // (4) relative pronoun with its antecedent (gender, number). A bare ablative relative ("quī" as the old
    // ablative) counts only after a preposition.
    for (size_t i = 1; i < n; ++i) {
      if (!relStart[i]) continue;
      size_t a = i - 1;
      const bool afterPrep = any(a, [](const Reading& r) { return r.lpos == Prep; });
      auto relOk = [&](const Reading& r) { return isRelative(r) && (r.f.case_ != Abl || afterPrep); };
      bool ok = false;
      for (const Reading& r : rd[i])
        for (const Reading& h : rd[a])
          if (relOk(r) && isHead(h) && numberCompat(r.f.number, h.f.number) && genderCompat(r.f.gender, headGender(h)))
            ok = true;
      // "ex eō quō īre vīs": an ablative relative right after an ablative antecedent (C2b)
      for (const Reading& r : rd[i])
        for (const Reading& h : rd[a])
          if (isRelative(r) && r.f.case_ == Abl && h.f.case_ == Abl && isHead(h) && numberCompat(r.f.number, h.f.number) &&
              genderCompat(r.f.gender, headGender(h)))
            ok = true;
      // the antecedent may carry modifiers after it: look one more word back
      if (!ok && a > 0 && attached[a])
        for (const Reading& r : rd[i])
          for (const Reading& h : rd[a - 1])
            if (relOk(r) && isHead(h) && numberCompat(r.f.number, h.f.number) && genderCompat(r.f.gender, headGender(h)))
              ok = true;
      if (!ok) issue("A3", (int)i, "relative pronoun '" + T(i) + "' does not agree with its antecedent");
    }
  }

  // ---- A4 (valency, agent) ----
  void government() {
    const size_t n = rep.tokens.size();
    for (size_t b = 0; b < n;) {
      const size_t e = segEnd(b);
      // verbs of the segment with a valency entry (finite or infinitive)
      std::vector<std::pair<size_t, const curated::Valency*>> vv;
      bool passive = false, active = false, sumFinite = false, perfPart = false;
      for (size_t j = b; j < e; ++j) {
        const curated::Valency* found = nullptr;
        for (const Reading& r : rd[j]) {
          if (r.f.mood == ParticipleMood && r.f.tense == Perfect && r.f.voice == Passive) perfPart = true;
          if (r.lpos == Participle && !verbish(j)) perfPart = true;
          if (!(isFinite(r) || isInfinitive(r)) || (!isVerb[j] && !isInfinitive(r))) continue;
          if (isFinite(r)) {
            if (r.f.voice == Passive) passive = true;
            else if (r.key == "sum") sumFinite = true;
            else active = true;
          }
          if (const curated::Valency* v = cd.valency(r.key)) found = v;
        }
        if (found && (isVerb[j] || any(j, isInfinitive))) vv.push_back({j, found});
      }
      if (sumFinite && perfPart) passive = true;   // periphrastic perfect passive: amāta est
      if (vv.size() == 1) {
        const curated::Valency* v = vv[0].second;
        bool firstDat = false, firstAbl = false, licAcc = false, licDat = false, licAbl = false, licGen = false;
        for (const curated::Frame& f : v->frames) {
          using K = curated::FrameKind;
          switch (f.kind) {
            case K::Acc: case K::AccInf: case K::AccAcc: licAcc = true; break;
            case K::AccAbl: licAcc = licAbl = true; break;
            case K::DatAcc: licAcc = licDat = true; break;
            case K::Dat: licDat = true; break;
            case K::Abl: licAbl = true; break;
            case K::Gen: licGen = true; break;
            case K::Impers:
              if (f.impers.find("acc") != std::string::npos) licAcc = true;
              if (f.impers.find("dat") != std::string::npos) licDat = true;
              if (f.impers.find("abl") != std::string::npos) licAbl = true;
              if (f.impers.find("gen") != std::string::npos) licGen = true;
              break;
            default: break;
          }
        }
        // the verb takes only accusative objects (prepositional, infinitive and clause frames do not license a
        // bare dative or ablative noun)
        const bool strictAcc = licAcc && !licDat && !licAbl && !licGen;
        if (!v->frames.empty()) {
          firstDat = v->frames[0].kind == curated::FrameKind::Dat;
          firstAbl = v->frames[0].kind == curated::FrameKind::Abl;
        }
        bool accPresent = false, datPresent = false, ablPresent = false;
        size_t defDat = n, defAbl = n, defAcc = n;
        for (size_t j = b; j < e; ++j) {
          if (governed[j] || isVerb[j]) continue;
          const uint16_t m = argMask(j);   // cases of the nominal readings (a non-verb token with a verb homograph is nominal)
          if (!m) continue;
          if ((m & (1u << Acc)) && !attached[j]) accPresent = true;
          if (m & (1u << Dat)) datPresent = true;
          if (m & (1u << Abl)) ablPresent = true;
          if (attached[j]) continue;
          const uint16_t da = (1u << Dat) | (1u << Abl);
          if (m == (1u << Dat) && defDat == n) defDat = j;
          if (m == (1u << Abl) && defAbl == n) defAbl = j;
          if (m == (1u << Acc) && defAcc == n) defAcc = j;
          if ((m & ~da) == 0 && defDat == n && defAbl == n) defDat = j;   // dative-or-ablative only (-ō, -īs)
        }
        const std::string& vt = T(vv[0].first);
        if (strictAcc && licAcc && !accPresent && !passive && (defDat < n || defAbl < n)) {
          const size_t j = defDat < n ? defDat : defAbl;
          issue("A4", (int)j, "'" + vt + "' takes an accusative object, not '" + T(j) + "'");
        } else if (firstDat && !licAcc && defAcc < n && !datPresent) {
          issue("A4", (int)defAcc, "'" + vt + "' takes a dative, not the accusative '" + T(defAcc) + "'");
        } else if (firstAbl && !licAcc && defAcc < n && !ablPresent) {
          issue("A4", (int)defAcc, "'" + vt + "' takes an ablative, not the accusative '" + T(defAcc) + "'");
        }
        (void)licDat; (void)licAbl; (void)licGen;
      }
      // ablative of agent: a passive verb with a bare ablative person (pronoun or name) needs ā/ab
      if (passive && !active) {
        for (size_t j = b; j < e; ++j) {
          // a bare oblique person (ablative reading, no nominative: mē, tē, nōbīs, eō, Marcō) with a passive verb
          if (governed[j] || !hasCase(j, Abl) || hasCase(j, Nom) || !allNominal(j)) continue;
          bool person = rep.tokens[j].name;
          for (const Reading& r : rd[j])
            if (r.lpos == Pron && (r.key == "ego" || r.key == "tu" || r.key == "nos" || r.key == "uos" || r.key == "is"))
              person = true;
            else if (r.lpos == Name || (r.lflags & lex::ProperName))
              person = true;
          if (person) issue("A4", (int)j, "agent '" + T(j) + "' of a passive verb needs ā/ab");
        }
      }
      b = e;
    }
  }

  // ---- A1, A2, A6 ----
  void known() {
    for (size_t i = 0; i < rep.tokens.size(); ++i) {
      const CheckedToken& t = rep.tokens[i];
      if (!rd[i].empty() || t.name) continue;
      if (t.fromRule) continue;
      issue("A1", (int)i, "unknown form '" + t.text + "'");
    }
    for (size_t i = 0; i < rep.tokens.size(); ++i)
      if (rep.tokens[i].fromRule && rd[i].empty() == false && rep.tokens[i].analysis.fromRule)
        issue("A1", (int)i, "'" + rep.tokens[i].text + "' is a paradigm-fallback form (check)", true);
  }
  void whitaker() {
    for (size_t i = 0; i < rep.tokens.size(); ++i) {
      if (rd[i].empty() || rep.tokens[i].name) continue;
      bool agree = false;
      // the core vocabulary (tiers 1-2, teacher-reviewed lists) counts as attested; Whitaker's join misses some
      // irregular verbs (sum) and pronouns (nēmō) in the current library
      for (const Reading& r : rd[i])
        agree = agree || r.whit != 0 || (r.aflags & lex::WhitakerOnly) || (r.tier >= 1 && r.tier <= 2) ||
                cd.tier(r.key) != nullptr;
      if (!agree) issue("A2", (int)i, "'" + T(i) + "' has no Whitaker entry", true);
    }
  }
  void tiers() {
    for (size_t i = 0; i < rep.tokens.size(); ++i) {
      if (rd[i].empty() || rep.tokens[i].name) continue;
      uint8_t best = 9;
      uint32_t hinted = lex::kNoLemma;
      if (opt.hints)
        for (const TokenHint& h : *opt.hints)
          if (rep.tokens[i].start >= h.start && rep.tokens[i].end <= h.end) hinted = h.lemma;
      for (const Reading& r : rd[i]) {
        if (hinted != lex::kNoLemma && r.lemma != hinted) continue;
        const uint8_t t = cd.effectiveTier(r.key, r.lpos, r.tier);
        best = std::min(best, t);
      }
      if (best == 9 && hinted != lex::kNoLemma) {   // C15: the realiser generated the form from the hinted lemma
        const lex::Lemma hl = lx.lemma(hinted);       // ("ōsculāta" from ōsculor; the reading is a participle lemma)
        if (hl.id != lex::kNoLemma) best = cd.effectiveTier(hl.key, hl.pos, hl.tier);
      }
      if (best == 9) {   // hinted lemma not among the readings: the best tier of any reading
        for (const Reading& r : rd[i]) best = std::min<uint8_t>(best, cd.effectiveTier(r.key, r.lpos, r.tier));
      }
      if (best != 9 && best > opt.tierCeiling)
        issue("A6", (int)i, "'" + T(i) + "' is tier " + std::to_string(best) + " (ceiling " +
                                std::to_string(opt.tierCeiling) + ")");
    }
  }

  void summarise() {
    for (const char* id : {"A1", "A2", "A3", "A4", "A6"}) {
      rules::Check c;
      c.id = id;
      size_t faults = 0, warns = 0;
      std::string first, firstWarn;
      for (const Issue& is : rep.issues) {
        if (is.id != id) continue;
        if (is.warning) { if (!warns++) firstWarn = is.detail; }
        else if (!faults++) first = is.detail;
      }
      c.ok = faults == 0;
      if (faults) c.detail = first + (faults > 1 ? " (+" + std::to_string(faults - 1) + " more)" : "");
      else if (warns) c.detail = "warning: " + firstWarn + (warns > 1 ? " (+" + std::to_string(warns - 1) + " more)" : "");
      rep.checks.push_back(std::move(c));
    }
  }
};

LatinChecker::LatinChecker(const lex::Lexicon& lx, const curated::CuratedData& cd) : lx_(lx), cd_(cd) {
  // every declined form of the names table (A1 exempt)
  for (const curated::NameEntry& n : cd.names()) {
    if (n.policy == curated::NamePolicy::Translate) continue;   // rēgīna, dominus, rēx are ordinary nouns
    nameKeys_.push_back(text::latin_key(n.latinNom));
    for (uint8_t c : {Nom, Gen, Dat, Acc, Abl, Voc})
      for (uint8_t num : {Sg, Pl}) {
        std::string f;
        if (realise::Names::decline(n.latinNom, n.latinGen, n.declension, n.gender, c, num, f))
          nameKeys_.push_back(text::latin_key(f));
      }
  }
  std::sort(nameKeys_.begin(), nameKeys_.end());
  nameKeys_.erase(std::unique(nameKeys_.begin(), nameKeys_.end()), nameKeys_.end());
}

void LatinChecker::check(std::string_view text, const Options& o, Report& out) {
  out.clear();
  Impl im{lx_, cd_, nameKeys_, o, out, {}, {}, {}, {}, {}, {}};
  im.tokenise(text);
  im.analyse(hasMacron(text));
  im.names();
  im.segment();
  im.known();
  im.whitaker();
  im.prepositions();
  im.agreement();
  im.government();
  im.tiers();
  im.summarise();
}

}  // namespace vp::check
