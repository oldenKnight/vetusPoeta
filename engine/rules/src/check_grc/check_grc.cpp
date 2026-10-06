// GreekChecker (vp/check_grc.h): tokenise, re-analyse (lexicon + closed tables + names), A1 A1b A3 A4 A6.
#include "vp/check_grc.h"

#include <algorithm>

#include "vp/features.h"
#include "vp/morph_grc.h"
#include "vp/text.h"

namespace vp::check {

using namespace vp::feat;

bool GrcReport::ok(std::string_view id) const {
  for (const rules::Check& c : checks)
    if (c.id == id) return c.ok;
  return true;
}
size_t GrcReport::count(std::string_view id, bool warnings) const {
  size_t n = 0;
  for (const Issue& i : issues)
    if (i.id == id && i.warning == warnings) ++n;
  return n;
}
std::string GrcReport::failures(std::initializer_list<const char*> ids) const {
  std::string s;
  for (const Issue& i : issues)
    if (!i.warning && std::find_if(ids.begin(), ids.end(), [&](const char* x) { return i.id == x; }) != ids.end())
      s += i.id + ": " + i.detail + "; ";
  return s;
}

struct GreekChecker::Reading {
  uint32_t lemma = lex::kNoLemma;
  std::string key;          // lemma key (closed readings: the table key)
  Features f;
  uint8_t lpos = 0, lgender = 0, tier = 0, lexTier = 0;
  uint16_t freqRank = 0;
  bool closed = false, name = false;
  uint8_t person = 0;       // personal pronouns
};

namespace {

bool isWordCp(char32_t c) {
  return (c >= 0x370 && c <= 0x3FF && c != 0x37E && c != 0x387) || (c >= 0x1F00 && c <= 0x1FFF && c != 0x1FBD) ||
         (c >= 0x0300 && c <= 0x036F) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= 0xC0 && c <= 0x24F);
}
bool isApos(char32_t c) { return c == 0x2019 || c == '\'' || c == 0x1FBD || c == 0x02BC; }
bool isBoundaryPunct(char32_t c) {
  return c == ',' || c == ';' || c == ':' || c == '.' || c == '!' || c == '?' || c == 0x387 || c == 0xB7 ||
         c == 0x37E || c == '(' || c == ')' || c == 0x2014 || c == '"' || c == 0x201C || c == 0x201D;
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

uint8_t closedPos(std::string_view key) {
  if (key == "ὁ") return Article;
  if (key == "οὗτοσ" || key == "ἐκεῖνοσ" || key == "πᾶσ") return feat::Det;
  if (key == "εἷσ" || key == "δύο" || key == "τρεῖσ" || key == "τέτταρεσ") return Num;
  return Pron;
}

}  // namespace

struct GreekChecker::Impl {
  const GreekChecker& self;
  const GreekCheckOptions& opt;
  GrcReport& rep;
  std::vector<std::vector<Reading>> rd;
  std::vector<char> boundaryBefore, punctAfter, governed, attached, isVerb, relStart, artCovered;
  std::vector<uint16_t> npMask;   // case mask of the NP a head belongs to (article ∩ head), 0 = not computed
  std::vector<std::string> punctText;

  // ---- reading classes ----
  static bool isFinite(const Reading& r) {
    return r.lpos == Verb && (r.f.mood == Indicative || r.f.mood == Subjunctive || r.f.mood == Imperative ||
                              r.f.mood == Optative) && r.f.person != 0;
  }
  static bool isInfinitive(const Reading& r) { return r.lpos == Verb && r.f.mood == Infinitive; }
  static bool isArticle(const Reading& r) { return r.closed && r.key == "ὁ"; }
  static bool isHead(const Reading& r) {
    if (r.f.case_ == 0) return false;
    if (r.lpos == Noun || r.lpos == Name || r.name) return true;
    return r.lpos == Pron && r.key != "ὅσ";
  }
  static bool isModifier(const Reading& r) {
    if (r.f.case_ == 0 || isArticle(r)) return false;
    return r.lpos == Adj || r.lpos == feat::Det || r.lpos == Num || r.lpos == Participle ||
           (r.lpos == Verb && r.f.mood == ParticipleMood);
  }
  static bool isNominal(const Reading& r) { return isHead(r) || isModifier(r) || isArticle(r); }
  static bool isRelative(const Reading& r) { return r.closed && r.key == "ὅσ"; }
  bool any(size_t i, bool (*p)(const Reading&)) const {
    for (const Reading& r : rd[i])
      if (p(r)) return true;
    return false;
  }
  bool onlyAdverbs(size_t i) const {
    if (rd[i].empty()) return false;
    for (const Reading& r : rd[i])
      if (r.lpos != Adv && r.lpos != Particle) return false;
    return true;
  }
  bool isPrep(size_t i) const {
    for (const Reading& r : rd[i])
      if (r.lpos == Prep) return true;
    return false;
  }
  uint16_t caseMask(size_t i, bool (*p)(const Reading&)) const {
    uint16_t m = 0;
    for (const Reading& r : rd[i])
      if (p(r)) m |= (uint16_t)(1u << r.f.case_);
    return m;
  }
  uint8_t headGender(const Reading& r) const { return r.f.gender ? r.f.gender : r.lgender; }
  bool strongHead(size_t i) const {
    for (const Reading& r : rd[i]) {
      if (!isHead(r)) continue;
      if (r.lpos == Pron || r.name || r.lpos == Name) return true;
      // a noun twin of an adjective reading of the same key (ἀγαθός "a good man") is not a head, unless the noun
      // lemma is the commoner one (ἀδελφός "brother" against ἀδελφός "brotherly")
      bool twin = false;
      auto rank = [](const Reading& x) { return (x.lexTier ? x.lexTier : 4) * 70000 + (x.freqRank ? x.freqRank : 65535); };
      for (const Reading& a : rd[i])
        if (isModifier(a) && a.key == r.key && rank(a) < rank(r)) twin = true;
      if (!twin) return true;
    }
    return false;
  }
  void issue(const char* id, int tok, std::string detail, bool warning = false) {
    rep.issues.push_back(Issue{id, tok, std::move(detail), warning});
  }
  const std::string& T(size_t i) const { return rep.tokens[i].text; }

  // ---- tokenise ----
  void tokenise(std::string_view t) {
    size_t i = 0;
    bool boundary = true;
    std::string pend;
    while (i < t.size()) {
      size_t j = i;
      const char32_t c = text::decodeUtf8(t, j);
      if (isWordCp(c)) {
        size_t start = i, end = j;
        while (end < t.size()) {
          size_t k = end;
          const char32_t d = text::decodeUtf8(t, k);
          if (isWordCp(d)) { end = k; continue; }
          if (isApos(d)) { end = k; break; }   // elided word keeps its apostrophe
          break;
        }
        if (!rep.tokens.empty()) punctText.back() = pend;
        pend.clear();
        GrcCheckedToken tok;
        tok.text = std::string(t.substr(start, end - start));
        tok.start = (int)start;
        tok.end = (int)end;
        rep.tokens.push_back(std::move(tok));
        boundaryBefore.push_back(boundary);
        punctText.emplace_back();
        boundary = false;
        i = end;
      } else {
        if (isBoundaryPunct(c)) {
          boundary = true;
          text::appendUtf8(pend, c);
        }
        i = j;
      }
    }
    if (!rep.tokens.empty()) punctText.back() = pend;
    punctAfter.assign(rep.tokens.size(), 0);
    for (size_t k = 0; k + 1 < rep.tokens.size(); ++k) punctAfter[k] = boundaryBefore[k + 1];
    if (!rep.tokens.empty()) punctAfter.back() = 1;
  }

  // ---- analyse ----
  void analyse() {
    const size_t n = rep.tokens.size();
    rd.assign(n, {});
    thread_local std::vector<grc::ClosedReading> cr;
    for (size_t i = 0; i < n; ++i) {
      GrcCheckedToken& t = rep.tokens[i];
      grc::analyse(self.lx_, t.text, t.analysis);
      std::vector<Reading>& v = rd[i];
      // closed classes (on the elided form restored, else the word)
      cr.clear();
      const std::string restored = grc::restoreElided(t.text);
      grc::closedReadings(restored.empty() ? t.text : restored, cr);
      for (const grc::ClosedReading& c : cr) {
        Reading r;
        r.closed = true;
        r.key = c.lemmaKey;
        r.lpos = closedPos(r.key);
        r.f.pos = r.lpos;
        r.f.case_ = c.case_;
        r.f.number = c.number;
        r.f.gender = c.gender;
        r.person = (r.key == "ἐγώ" || r.key == "ἡμεῖσ") ? 1 : (r.key == "σύ" || r.key == "ὑμεῖσ") ? 2 : 0;
        r.tier = 1;
        for (const auto& p : self.closedIds_)
          if (p.first == r.key) r.lemma = p.second;
        v.push_back(std::move(r));
      }
      t.closed = !v.empty();
      auto addLex = [&](uint32_t lemma, uint32_t packed, uint16_t aflags) {
        const lex::Lemma l = self.lx_.lemma(lemma);
        Reading r;
        r.lemma = lemma;
        r.key = std::string(l.key);
        r.f = unpack(packed);
        r.lpos = l.pos;
        if (r.lpos == Participle && r.f.case_ == 0 && r.f.mood == 0) r.lpos = Particle;   // library labels particles so
        r.lgender = l.gender;
        r.tier = r.lexTier = l.tier;
        r.freqRank = l.freqRank;
        if (const curated::TierEntry* te = self.cd_.tierGreek(l.key))
          if (te->tier && (!r.tier || te->tier < r.tier)) r.tier = te->tier;
        (void)aflags;
        if (t.closed && isNominal(r)) return;   // the closed table decides closed-class words
        if ((aflags & lex::AltSpelling) && (r.lpos == Noun || r.lpos == Adj)) return;
        if (r.lpos == Symbol || r.lpos == Suffix || r.lpos == Prefix || r.lpos == Phrase) return;
        v.push_back(std::move(r));
      };
      thread_local std::vector<uint32_t> tabled;
      tabled.clear();
      for (const lex::Analysis& a : t.analysis.analyses)
        if (a.flags & lex::FromTable) tabled.push_back(a.lemma);
      for (const lex::Analysis& a : t.analysis.analyses) {
        if (!(a.flags & lex::FromTable) && std::find(tabled.begin(), tabled.end(), a.lemma) != tabled.end()) continue;
        addLex(a.lemma, self.lx_.feature(a.feat), a.flags);
      }
      for (const morph::RuleAnalysis& a : t.analysis.ruleAnalyses) addLex(a.lemma, a.packed, 0);
      if (v.empty())   // every lexicon reading was a weak one: keep them all rather than none
        for (const lex::Analysis& a : t.analysis.analyses) {
          const lex::Lemma l = self.lx_.lemma(a.lemma);
          Reading r;
          r.lemma = a.lemma;
          r.key = std::string(l.key);
          r.f = unpack(self.lx_.feature(a.feat));
          r.lpos = l.pos;
          r.lgender = l.gender;
          r.tier = l.tier;
          v.push_back(std::move(r));
        }
      // a word that is a preposition / conjunction / particle drops uninflected noun and interjection homographs
      if (std::any_of(v.begin(), v.end(), [](const Reading& r) { return r.lpos == Prep || r.lpos == Conj || r.lpos == Particle; }))
        v.erase(std::remove_if(v.begin(), v.end(), [](const Reading& r) {
                  return r.lpos == Intj || ((r.lpos == Noun || r.lpos == Name) && r.f.case_ == 0);
                }), v.end());
      // names: the declined forms of names_grc.tsv (capitalised)
      if (t.analysis.capitalised) {
        const std::string k = text::greek_key(grc::ultimaToAcute(t.text));
        auto it = std::lower_bound(self.names_.begin(), self.names_.end(), k,
                                   [](const NameForm& a, const std::string& b) { return a.key < b; });
        bool hit = false;
        for (; it != self.names_.end() && it->key == k; ++it) {
          Reading r;
          r.name = true;
          r.lpos = Name;
          r.key = k;
          r.f.pos = Name;
          r.f.case_ = it->case_;
          r.f.number = Sg;
          r.lgender = it->gender;
          r.f.gender = it->gender;
          r.tier = 1;
          v.push_back(std::move(r));
          hit = true;
        }
        if (hit) {
          t.name = true;
          v.erase(std::remove_if(v.begin(), v.end(), [](const Reading& r) { return !r.name && r.lpos != Name; }), v.end());
        }
      }
      if (t.analysis.fromRule) { t.fromRule = true; rep.fromRule = true; }
      t.accentDiffers = t.analysis.accentInsensitive && !t.closed;
    }
    // ὦ before a vocative is the interjection, not the subjunctive of εἰμί
    for (size_t i = 0; i + 1 < n; ++i) {
      if (text::greek_key(rep.tokens[i].text) != "ὦ") continue;
      bool voc = false;
      for (const Reading& r : rd[i + 1]) voc = voc || r.f.case_ == Voc;
      if (voc) rd[i].erase(std::remove_if(rd[i].begin(), rd[i].end(), [](const Reading& r) { return r.lpos == Verb; }), rd[i].end());
    }
  }

  // ---- names (glossary, hints, unknown capitalised words) ----
  void names() {
    for (size_t i = 0; i < rep.tokens.size(); ++i) {
      GrcCheckedToken& t = rep.tokens[i];
      if (opt.glossary && rd[i].empty())
        for (const rules::GlossaryEntry& g : *opt.glossary) {
          const std::string k = text::greek_bare(t.text);
          if ((!g.form.empty() && text::greek_bare(g.form) == k) || text::en_key(g.name) == text::en_key(t.text)) t.name = true;
        }
      if (t.analysis.capitalised && rd[i].empty()) t.name = true;
      if (t.analysis.capitalised && !rd[i].empty() &&
          std::all_of(rd[i].begin(), rd[i].end(), [](const Reading& r) { return r.lpos == Name || r.name; }))
        t.name = true;
    }
    if (opt.hints)
      for (const TokenHint& h : *opt.hints)
        for (GrcCheckedToken& t : rep.tokens)
          if (t.start >= h.start && t.end <= h.end) {
            if (h.name) t.name = true;
            if (h.fromRule) { t.fromRule = true; rep.fromRule = true; }
          }
  }

  // ---- segments and verbs ----
  void segment() {
    const size_t n = rep.tokens.size();
    relStart.assign(n, 0);
    int seg = 0;
    for (size_t i = 0; i < n; ++i) {
      bool cut = boundaryBefore[i] && i > 0;
      if (i > 0 && !cut) {
        const std::string k = text::greek_key(grc::ultimaToAcute(T(i)));
        bool conj = false;
        for (const Reading& r : rd[i]) conj = conj || r.lpos == Conj;
        if (conj && (k == "ὅτι" || k == "ἐπεί" || k == "ὅτε" || k == "εἰ" || k == "ἐάν" || k == "ἵνα" || k == "ὅπωσ" ||
                     k == "ὥστε" || k == "ἐπειδή" || k == "διότι"))
          cut = true;
        if (any(i, isRelative) && i > 0 && (any(i - 1, isHead) || attachedHeadBefore(i))) { cut = true; relStart[i] = 1; }
      }
      if (cut) ++seg;
      rep.tokens[i].segment = seg;
    }
    isVerb.assign(n, 0);
    for (size_t i = 0; i < n; ++i) {
      if (!any(i, isFinite)) continue;
      bool nominal = false, onlyParticiple = true;
      for (const Reading& r : rd[i]) {
        if (!isNominal(r)) continue;
        nominal = true;
        onlyParticiple = onlyParticiple && (r.lpos == Verb || r.lpos == Participle);
      }
      // a finite reading wins over participle homographs (βοηθοῦσι) and at the end of its clause (verb-final order)
      const bool last = i + 1 == n || rep.tokens[i + 1].segment != rep.tokens[i].segment || boundaryBefore[i + 1];
      if (!nominal || onlyParticiple || (last && !strongHead(i))) isVerb[i] = 1;
    }
    for (size_t b = 0; b < n;) {   // an infinitive homograph (καλέσαι, optative too) next to a sure finite verb
      const size_t e = segEnd(b);
      bool sure = false;
      for (size_t j = b; j < e; ++j) sure = sure || (isVerb[j] && !any(j, isInfinitive));
      if (sure)
        for (size_t j = b; j < e; ++j)
          if (isVerb[j] && any(j, isInfinitive)) isVerb[j] = 0;
      b = e;
    }
    for (size_t b = 0; b < n;) {   // a segment without a verb takes its only finite candidate
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
    // coordinated clauses: split at καί / ἀλλά / οὐδέ between two verbs
    for (size_t i = 0; i < n; ++i) {
      const std::string k = text::greek_key(grc::ultimaToAcute(T(i)));
      if (!(k == "καί" || k == "ἀλλά" || k == "οὐδέ" || k == "μηδέ")) continue;
      bool before = false, after = false;
      for (size_t j = i; j-- > 0 && rep.tokens[j].segment == rep.tokens[i].segment;) before = before || isVerb[j];
      for (size_t j = i + 1; j < n && rep.tokens[j].segment == rep.tokens[i].segment; ++j) after = after || isVerb[j];
      if (before && after)
        for (size_t j = i; j < n; ++j) rep.tokens[j].segment += 1;
    }
  }
  bool attachedHeadBefore(size_t i) const {   // "ὁ ἀνὴρ ὃν": the head may carry an enclitic possessor after it
    return i >= 2 && any(i - 2, isHead) && rep.tokens[i - 1].closed;
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
  bool sameGroup(size_t a, size_t b) const {   // no punctuation and no segment change between a and b (a < b)
    for (size_t k = a + 1; k <= b; ++k)
      if (boundaryBefore[k] || rep.tokens[k].segment != rep.tokens[a].segment) return false;
    return true;
  }

  // ---- A1 / A1b ----
  void known() {
    for (size_t i = 0; i < rep.tokens.size(); ++i) {
      const GrcCheckedToken& t = rep.tokens[i];
      if (t.name) continue;
      if (rd[i].empty()) {
        if (!t.fromRule) issue("A1", (int)i, "unknown form '" + t.text + "'");
        continue;
      }
      if (t.accentDiffers) issue("A1", (int)i, "'" + t.text + "' is known only with other accents or breathings (accent differs)", true);
      if (t.analysis.fromRule) issue("A1", (int)i, "'" + t.text + "' is a paradigm-fallback form (check)", true);
    }
  }
  void accents() {
    const size_t n = rep.tokens.size();
    std::vector<grc::SandhiWord> ws(n);
    for (size_t i = 0; i < n; ++i) {
      std::string w = grc::ultimaToAcute(T(i));
      const grc::AccentInfo a = grc::accentOf(w);
      if (a.accents >= 2) {   // the acute a following enclitic added: back to the lexical form
        std::u32string u = text::toUtf32(text::nfd(w));
        for (size_t k = u.size(); k-- > 0;)
          if (u[k] == 0x0301) { u.erase(u.begin() + (long)k); break; }
        w = text::nfc(text::toUtf8(u));
      }
      ws[i].form = w;
      const std::string b = text::greek_bare(w);
      if ((b == "εστι" || b == "εστιν") && grc::accentOf(w).position == 1) ws[i].existential = true;
      ws[i].punctAfter = punctText[i];
      if (i + 1 == n && ws[i].punctAfter.empty()) ws[i].punctAfter = ".";
    }
    grc::SandhiOptions so;
    so.autoEnclitic = true;
    grc::sandhi(ws, so);
    for (size_t i = 0; i < n; ++i) {
      const GrcCheckedToken& t = rep.tokens[i];
      if (t.name || !grc::isGreekWord(t.text)) continue;
      const bool elided = !grc::restoreElided(t.text).empty();
      const grc::AccentInfo a = grc::accentOf(t.text);
      const bool clitic = grc::isProclitic(t.text) || grc::isEncliticForm(t.text) || ws[i].enclitic || ws[i].proclitic;
      if (!elided && !clitic && a.accents == 0) {
        issue("A1b", (int)i, "'" + t.text + "' has no accent", true);
        continue;
      }
      if (a.accents > 2) { issue("A1b", (int)i, "'" + t.text + "' has more than two accents", true); continue; }
      if (elided) continue;
      // movable ν is the realiser's choice: compare without it
      std::string want = ws[i].form, have = text::nfc(t.text);
      auto noNu = [](std::string s) {
        const std::string b = text::greek_bare(s);
        if (b.size() >= 2 && b.compare(b.size() - 2, 2, "ν") == 0) {
          std::u32string u = text::toUtf32(text::nfd(s));
          if (!u.empty() && u.back() == U'ν') { u.pop_back(); s = text::nfc(text::toUtf8(u)); }
        }
        return s;
      };
      if (want != have && noNu(want) != noNu(have))
        issue("A1b", (int)i, "accent of '" + t.text + "': expected '" + want + "'", true);
    }
  }

  // ---- A4 prepositions (also marks the governed NP) ----
  uint16_t prepCasesOf(size_t i) const {
    std::string k = grc::restoreElided(T(i));
    k = text::greek_key(grc::ultimaToAcute(k.empty() ? T(i) : k));
    if (k == "ἐξ") k = "ἐκ";
    if (k == "ἐσ") k = "εἰσ";
    return self.gd_.prepCases(k);
  }
  // Case mask of the NP that starts at i: article ∩ the first head after it (modifiers skipped), else the token.
  uint16_t npStartMask(size_t i, size_t* head) const {
    if (head) *head = i;
    if (any(i, isArticle)) {
      uint16_t m = caseMask(i, isArticle);
      for (size_t j = i + 1; j < rep.tokens.size() && sameGroup(i, j); ++j) {
        if (onlyAdverbs(j)) continue;
        const uint16_t hm = caseMask(j, isNominal);
        if (!hm) break;
        if (strongHead(j)) { if (head) *head = j; return m & hm ? (uint16_t)(m & hm) : m; }
        if (hm & m) m &= hm;
        if (head) *head = j;
      }
      return m;
    }
    return caseMask(i, isNominal);
  }
  void prepositions() {
    const size_t n = rep.tokens.size();
    governed.assign(n, 0);
    for (size_t i = 0; i + 1 < n; ++i) {
      if (!isPrep(i) || punctAfter[i]) continue;
      const uint16_t cases = prepCasesOf(i);
      if (!cases) continue;
      const size_t x = i + 1;
      if (!any(x, isNominal)) continue;
      size_t head = x;
      const uint16_t m = npStartMask(x, &head);
      if (!(m & cases)) {
        issue("A4", (int)x, "preposition '" + T(i) + "' does not govern the case of '" + T(head) + "'");
        continue;
      }
      for (size_t j = x; j <= head; ++j) governed[j] = 1;
      // a following agreeing modifier belongs to the phrase too
      for (size_t j = head + 1; j < n && sameGroup(head, j); ++j) {
        if (!(caseMask(j, isModifier) & m) || strongHead(j)) break;
        governed[j] = 1;
      }
    }
  }

  // ---- A3 ----
  bool agreeReading(const Reading& a, const Reading& b) const {   // a modifier / article with a head or modifier
    return a.f.case_ == b.f.case_ && numberCompat(a.f.number, b.f.number) &&
           genderCompat(a.f.gender ? a.f.gender : (uint8_t)7, isHead(b) ? headGender(b) : (b.f.gender ? b.f.gender : (uint8_t)7));
  }
  bool agreeTokens(size_t m, size_t h, bool (*mp)(const Reading&)) const {
    for (const Reading& a : rd[m]) {
      if (!mp(a)) continue;
      for (const Reading& b : rd[h])
        if (isNominal(b) && !isArticle(b) && agreeReading(a, b)) return true;
    }
    return false;
  }
  void articles() {
    const size_t n = rep.tokens.size();
    artCovered.assign(n, 0);
    for (size_t i = 0; i < n; ++i) {
      if (!any(i, isArticle)) continue;
      // relative / demonstrative homographs are not articles when accented (ὅ, ἥ, οἵ, αἵ): the closed table splits them
      for (size_t j = i + 1; j < n && sameGroup(i, j); ++j) {
        if (onlyAdverbs(j)) continue;
        if (any(j, isArticle) || isPrep(j) || isVerb[j] || any(j, isInfinitive)) break;
        if (!any(j, isNominal)) break;
        const bool head = strongHead(j);
        if (!agreeTokens(i, j, isArticle)) {
          issue("A3", (int)j, "article '" + T(i) + "' does not agree with '" + T(j) + "' (case, number, gender)");
          artCovered[j] = 1;
          break;
        }
        artCovered[j] = 1;
        if (head || any(j, isHead)) break;
      }
    }
  }
  bool closedDet(size_t i) const {
    bool det = false;
    for (const Reading& r : rd[i]) {
      if (!isModifier(r)) continue;
      if (!(r.closed && (r.key == "οὗτοσ" || r.key == "ἐκεῖνοσ" || r.key == "πᾶσ" || r.key == "οὐδείσ" || r.key == "μηδείσ")))
        return false;
      det = true;
    }
    return det;
  }
  bool agreeArticle(size_t m, size_t art) const {
    for (const Reading& a : rd[m])
      for (const Reading& b : rd[art])
        if (isModifier(a) && isArticle(b) && a.f.case_ == b.f.case_ && numberCompat(a.f.number, b.f.number) &&
            genderCompat(a.f.gender, b.f.gender))
          return true;
    return false;
  }
  bool predicateOk(size_t m, size_t b, size_t e) const {
    std::vector<uint8_t> numbers;
    for (size_t j = b; j < e; ++j)
      if (isVerb[j])
        for (const Reading& r : rd[j])
          if (isFinite(r) && (r.key == "εἰμί" || r.key == "γίγνομαι")) numbers.push_back(r.f.number);
    bool inf = false;
    for (size_t j = b; j < e; ++j)
      for (const Reading& r : rd[j])
        if (isInfinitive(r) && r.key == "εἰμί") inf = true;
    if (numbers.empty() && !inf) return false;
    std::vector<std::pair<uint8_t, uint8_t>> subjects;   // (number, gender) of nominative heads
    for (size_t j = b; j < e; ++j) {
      if (j == m || governed[j] || isVerb[j] || !strongHead(j)) continue;
      for (const Reading& r : rd[j])
        if (isHead(r) && (r.f.case_ == Nom || (inf && r.f.case_ == Acc))) subjects.emplace_back(r.f.number, headGender(r));
    }
    for (const Reading& r : rd[m]) {
      if (!isModifier(r) || !(r.f.case_ == Nom || (inf && r.f.case_ == Acc))) continue;
      bool numOk = inf;
      for (uint8_t nn : numbers)
        numOk = numOk || numberCompat(r.f.number, nn) || (r.f.number == Pl && r.f.gender == N && nn == Sg);
      if (!numOk) continue;
      if (subjects.empty()) return true;
      for (const auto& sj : subjects)
        if (numberCompat(r.f.number, sj.first) && genderCompat(r.f.gender, sj.second)) return true;
    }
    return false;
  }
  void agreement() {
    const size_t n = rep.tokens.size();
    attached.assign(n, 0);
    // (1) pure modifiers outside article groups: agree with an adjacent head, or be a valid predicate
    for (size_t i = 0; i < n; ++i) {
      if (artCovered[i] || governed[i] || !any(i, isModifier) || any(i, isHead) || isVerb[i] || rep.tokens[i].name ||
          any(i, isArticle) || relStart[i])
        continue;
      if (closedDet(i)) {
        if (i + 1 < n && sameGroup(i, i + 1) && any(i + 1, isArticle)) {
          if (!agreeArticle(i, i + 1)) issue("A3", (int)i, "'" + T(i) + "' does not agree with the article '" + T(i + 1) + "'");
          attached[i] = 1;
        }
        continue;   // a demonstrative / quantifier standing alone is a pronoun ("ταῦτα λέγει")
      }
      const int seg = rep.tokens[i].segment;
      const size_t b = segBegin(i), e = segEnd(i);
      std::vector<size_t> cand;
      for (int dir : {-1, 1}) {
        long j = (long)i + dir;
        while (j >= 0 && j < (long)n && rep.tokens[(size_t)j].segment == seg && !isVerb[(size_t)j] &&
               !strongHead((size_t)j) && !(dir == 1 && boundaryBefore[(size_t)j]) && !(dir == -1 && boundaryBefore[(size_t)j + 1]))
          j += dir;
        if (j < 0 || j >= (long)n || rep.tokens[(size_t)j].segment != seg) continue;
        if (dir == 1 && boundaryBefore[(size_t)j]) continue;
        if (dir == -1 && boundaryBefore[(size_t)j + 1]) continue;
        if (strongHead((size_t)j) && !isVerb[(size_t)j]) cand.push_back((size_t)j);
      }
      bool ok = false;
      for (size_t h : cand) ok = ok || agreeTokens(i, h, isModifier);
      if (ok) { attached[i] = 1; continue; }
      bool copula = false;
      for (size_t j = b; j < e; ++j)
        if (isVerb[j])
          for (const Reading& r : rd[j]) copula = copula || (isFinite(r) && (r.key == "εἰμί" || r.key == "γίγνομαι"));
      for (size_t j = b; j < e; ++j)
        for (const Reading& r : rd[j]) copula = copula || (isInfinitive(r) && r.key == "εἰμί");
      if (copula && predicateOk(i, b, e)) { attached[i] = 1; continue; }
      if (cand.empty()) {
        if (copula) issue("A3", (int)i, "predicate '" + T(i) + "' does not agree with the subject / verb");
        continue;
      }
      bool partial = false;
      for (size_t h : cand)
        for (const Reading& a : rd[i])
          for (const Reading& r : rd[h])
            if (isModifier(a) && isHead(r) && a.f.case_ == r.f.case_) partial = true;
      if (partial || copula)
        issue("A3", (int)i, "'" + T(i) + "' does not agree with '" + T(cand[0]) + "' (case, number, gender)");
    }
    // (2) subject - verb, per segment
    for (size_t b = 0; b < n;) {
      const size_t e = segEnd(b);
      std::vector<size_t> verbs;
      bool copula = false;
      for (size_t j = b; j < e; ++j)
        if (isVerb[j]) {
          verbs.push_back(j);
          for (const Reading& r : rd[j]) copula = copula || (isFinite(r) && r.key == "εἰμί");
        }
      // nominative heads: the NP case (article ∩ head) must be nominative only; neuter nom/acc ambiguity kept apart
      std::vector<size_t> subj, ambiguous;
      for (size_t j = b; j < e; ++j) {
        if (isVerb[j] || governed[j] || attached[j] || relStart[j] || !any(j, isHead)) continue;
        if (any(j, isRelative)) continue;
        uint16_t m = caseMask(j, isHead);
        if (j > b && any(j - 1, isArticle) && sameGroup(j - 1, j)) m &= caseMask(j - 1, isArticle) | 0;
        else
          for (size_t k = j; k-- > b;) {   // article further left (article + modifiers + head)
            if (!sameGroup(k, j)) break;
            if (any(k, isArticle)) { m &= caseMask(k, isArticle); break; }
            if ((!artCovered[k] && !onlyAdverbs(k)) || any(k, isHead)) break;
          }
        m &= (uint16_t)~(1u << Voc);
        if (m == (1u << Nom)) subj.push_back(j);
        else if (m == ((1u << Nom) | (1u << Acc))) ambiguous.push_back(j);
      }
      bool imperativeOnly = !verbs.empty();
      for (size_t v : verbs)
        for (const Reading& r : rd[v])
          if (isFinite(r) && r.f.mood != Imperative) imperativeOnly = false;
      bool third = false;
      for (size_t v : verbs)
        for (const Reading& r : rd[v]) third = third || (isFinite(r) && r.f.person == P3);
      ambiguous.erase(std::remove_if(ambiguous.begin(), ambiguous.end(), [&](size_t j) {
                        for (const Reading& r : rd[j])
                          if (r.closed && (r.key == "τίσ" || r.key == "τισ")) return true;
                        return false;
                      }), ambiguous.end());
      bool nomElsewhere = false;   // a nominative-only article or modifier: the subject is there, not in a neuter
      for (size_t j = b; j < e; ++j) {
        if (isVerb[j] || governed[j] || std::find(ambiguous.begin(), ambiguous.end(), j) != ambiguous.end()) continue;
        const uint16_t m = (uint16_t)(caseMask(j, isNominal) & ~(1u << Voc));
        if (m == (1u << Nom)) nomElsewhere = true;
      }
      if (subj.empty() && verbs.size() == 1 && !copula && third && !nomElsewhere) subj = ambiguous;
      if (!verbs.empty() && !subj.empty() && !imperativeOnly) {
        // subjects joined by καί are plural
        bool coord = false;
        for (size_t k = 1; k < subj.size(); ++k)
          for (size_t x = subj[k - 1] + 1; x < subj[k]; ++x)
            if (text::greek_key(grc::ultimaToAcute(T(x))) == "καί") coord = true;
        bool anyAgree = false, allAgree = true;
        size_t bad = subj[0];
        for (size_t j : subj) {
          bool agrees = false;
          for (size_t v : verbs)
            for (const Reading& vr : rd[v]) {
              if (!isFinite(vr)) continue;
              if (coord) { agrees = agrees || vr.f.number == Pl; continue; }
              for (const Reading& r : rd[j]) {
                if (!isHead(r) || (r.f.case_ != Nom && r.f.case_ != Acc)) continue;
                const uint8_t person = r.person ? r.person : 3;
                const bool neutPl = r.f.number == Pl && headGender(r) == N;
                const bool numOk = numberCompat(r.f.number, vr.f.number) || (neutPl && vr.f.number == Sg);
                if (!numOk) continue;
                if (!copula && person != vr.f.person) continue;
                agrees = true;
              }
            }
          anyAgree = anyAgree || agrees;
          if (!agrees && allAgree) { allAgree = false; bad = j; }
          if (coord) break;
        }
        if (copula ? !anyAgree : !allAgree)
          issue("A3", (int)bad, "subject '" + T(bad) + "' does not agree with the verb '" + T(verbs[0]) + "'");
      }
      b = e;
    }
    // (3) relative pronoun with its antecedent (gender, number)
    for (size_t i = 1; i < n; ++i) {
      if (!relStart[i]) continue;
      size_t a = any(i - 1, isHead) ? i - 1 : i - 2;
      bool ok = false;
      for (const Reading& r : rd[i])
        for (const Reading& h : rd[a])
          if (isRelative(r) && isHead(h) && numberCompat(r.f.number, h.f.number) && genderCompat(r.f.gender, headGender(h)))
            ok = true;
      if (!ok) issue("A3", (int)i, "relative pronoun '" + T(i) + "' does not agree with its antecedent");
    }
  }

  // ---- A4 valency ----
  void government() {
    const size_t n = rep.tokens.size();
    // attributive genitives (after a head or its enclitic possessor) and enclitic possessors are not objects
    std::vector<char> attrib(n, 0);
    for (size_t i = 1; i < n; ++i) {
      if (!sameGroup(i - 1, i)) continue;
      size_t start = i;
      uint16_t m = npStartMask(i, nullptr);
      if (m == (1u << Gen) && (strongHead(i - 1) || artCovered[i - 1] || attrib[i - 1] || governed[i - 1])) {
        size_t head = start;
        npStartMask(i, &head);
        for (size_t j = start; j <= head; ++j) attrib[j] = 1;
      }
    }
    for (size_t b = 0; b < n;) {
      const size_t e = segEnd(b);
      std::vector<std::pair<size_t, const grc::Valency*>> vv;
      bool middle = false;
      for (size_t j = b; j < e; ++j) {
        const grc::Valency* found = nullptr;
        for (const Reading& r : rd[j]) {
          if (!(isFinite(r) || isInfinitive(r))) continue;
          if (!isVerb[j] && !isInfinitive(r)) continue;
          if (const grc::Valency* v = self.gd_.valency(r.key)) {
            found = v;
            middle = middle || r.f.voice == Middle || r.f.voice == Passive;
          }
        }
        if (found) vv.push_back({j, found});
      }
      if (vv.size() == 1) {
        const grc::Valency* v = vv[0].second;
        bool licAcc = false, licDat = false, licGen = false, impers = false, any = false;
        auto scan = [&](bool wantMiddle, bool strictVoice) {
          for (const grc::Frame& f : v->frames) {
            if (strictVoice && f.middle != wantMiddle) continue;
            any = true;
            switch (f.kind) {
              case grc::FrameKind::Acc: case grc::FrameKind::AccAcc: case grc::FrameKind::AccInf: licAcc = true; break;
              case grc::FrameKind::DatAcc: licAcc = licDat = true; break;
              case grc::FrameKind::Dat: licDat = true; break;
              case grc::FrameKind::Gen: licGen = true; break;
              case grc::FrameKind::ImpersAccInf: case grc::FrameKind::ImpersDatInf: impers = true; break;
              default: break;
            }
          }
        };
        scan(middle, true);
        if (!any) scan(middle, false);
        bool accPresent = false, datPresent = false, genPresent = false;
        size_t defDat = n, defGen = n, defAcc = n;
        for (size_t j = b; j < e; ++j) {
          if (governed[j] || isVerb[j] || attrib[j] || attached[j] || relStart[j]) continue;
          if (any_(j)) continue;
          if (artCovered[j] && !strongHead(j)) continue;
          if (any_article(j)) continue;   // the head after the article carries the NP
          bool adverb = false;
          for (const Reading& r : rd[j]) adverb = adverb || r.lpos == Adv;
          if (adverb) continue;
          uint16_t m = npMaskOf(j);
          if (!m) continue;
          if (m & (1u << Acc)) accPresent = true;
          if (m & (1u << Dat)) datPresent = true;
          if (m & (1u << Gen)) genPresent = true;
          if (m == (1u << Dat) && defDat == n) defDat = j;
          if (m == (1u << Gen) && defGen == n) defGen = j;
          if (m == (1u << Acc) && defAcc == n) defAcc = j;
        }
        const std::string& vt = T(vv[0].first);
        const bool strictAcc = licAcc && !licDat && !licGen;
        if (impers) {
        } else if (strictAcc && !accPresent && (defDat < n || defGen < n)) {
          const size_t j = defDat < n ? defDat : defGen;
          issue("A4", (int)j, "'" + vt + "' takes an accusative object, not '" + T(j) + "'");
        } else if (licDat && !licAcc && !licGen && defAcc < n && !datPresent) {
          issue("A4", (int)defAcc, "'" + vt + "' takes a dative, not the accusative '" + T(defAcc) + "'");
        } else if (licGen && !licAcc && !licDat && defAcc < n && !genPresent) {
          issue("A4", (int)defAcc, "'" + vt + "' takes a genitive, not the accusative '" + T(defAcc) + "'");
        }
      }
      b = e;
    }
  }
  bool any_(size_t j) const {   // relative pronouns and closed interrogatives are not objects here
    for (const Reading& r : rd[j])
      if (isRelative(r) || (r.closed && r.key == "τίσ")) return true;
    return false;
  }
  bool any_article(size_t j) const { return any(j, isArticle) && !strongHead(j); }
  // Case mask of the NP whose head is j (article to the left intersected), nominative excluded.
  uint16_t npMaskOf(size_t j) const {
    uint16_t m = caseMask(j, isNominal);
    for (size_t k = j; k-- > 0;) {
      if (!sameGroup(k, j)) break;
      if (any(k, isArticle)) { const uint16_t am = caseMask(k, isArticle); if (m & am) m &= am; break; }
      if ((!artCovered[k] && !onlyAdverbs(k)) || any(k, isHead)) break;
    }
    m &= (uint16_t)~(1u << Voc);
    return m;
  }

  // ---- A6 ----
  void tiers() {
    for (size_t i = 0; i < rep.tokens.size(); ++i) {
      if (rd[i].empty() || rep.tokens[i].name) continue;
      uint32_t hinted = lex::kNoLemma;
      if (opt.hints)
        for (const TokenHint& h : *opt.hints)
          if (rep.tokens[i].start >= h.start && rep.tokens[i].end <= h.end) hinted = h.lemma;
      uint8_t best = 9;
      for (const Reading& r : rd[i]) {
        if (hinted != lex::kNoLemma && r.lemma != hinted) continue;
        best = std::min<uint8_t>(best, r.tier ? r.tier : 3);
      }
      if (best == 9)
        for (const Reading& r : rd[i]) best = std::min<uint8_t>(best, r.tier ? r.tier : 3);
      if (best != 9 && best > opt.tierCeiling)
        issue("A6", (int)i, "'" + T(i) + "' is tier " + std::to_string(best) + " (ceiling " + std::to_string(opt.tierCeiling) + ")");
    }
  }

  void summarise() {
    for (const char* id : {"A1", "A1b", "A3", "A4", "A6"}) {
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

GreekChecker::GreekChecker(const lex::Lexicon& lx, const curated::CuratedData& cd, const grc::GreekData& gd)
    : lx_(lx), cd_(cd), gd_(gd) {
  for (const grc::NameEntry& e : gd.names()) {
    if (e.policy == curated::NamePolicy::Translate) continue;   // titles are ordinary nouns
    for (uint8_t c : {Nom, Gen, Dat, Acc, Voc}) {
      std::string f;
      if (grc::declineName(e.nom, e.gen, e.declension, e.gender, c, e.voc, f))
        names_.push_back(NameForm{text::greek_key(f), c, e.gender});
    }
  }
  std::sort(names_.begin(), names_.end(), [](const NameForm& a, const NameForm& b) {
    return a.key != b.key ? a.key < b.key : a.case_ < b.case_;
  });
  static const char* const kClosed[][2] = {{"ὁ", "ὁ"}, {"ἐγώ", "ἐγώ"}, {"σύ", "σύ"}, {"ἡμεῖσ", "ἡμεῖς"}, {"ὑμεῖσ", "ὑμεῖς"},
                                           {"αὐτόσ", "αὐτός"}, {"οὗτοσ", "οὗτος"}, {"ἐκεῖνοσ", "ἐκεῖνος"}, {"ὅσ", "ὅς"},
                                           {"τίσ", "τίς"}, {"τισ", "τις"}, {"οὐδείσ", "οὐδείς"}, {"μηδείσ", "μηδείς"},
                                           {"εἷσ", "εἷς"}, {"δύο", "δύο"}, {"τρεῖσ", "τρεῖς"}, {"τέτταρεσ", "τέτταρες"},
                                           {"πᾶσ", "πᾶς"}};
  for (const auto& k : kClosed) {
    uint32_t id = grc::findLemma(lx, k[1], k[0] == std::string("ὁ") ? (uint8_t)Article : (uint8_t)0);
    if (id == lex::kNoLemma) id = grc::findLemma(lx, k[1]);
    closedIds_.push_back({k[0], id});
  }
}

void GreekChecker::check(std::string_view text, const GreekCheckOptions& o, GrcReport& out) {
  out.clear();
  Impl im{*this, o, out, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}};
  im.tokenise(text);
  im.analyse();
  im.names();
  im.segment();
  im.known();
  im.accents();
  im.prepositions();
  im.articles();
  im.agreement();
  im.government();
  im.tiers();
  im.summarise();
}

}  // namespace vp::check
