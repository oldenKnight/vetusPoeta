// grc2x analyser: tokens, readings (lexicon through C9's vp::grc::analyse, closed-class tables, names) and the
// disambiguation by constraint propagation (coordinate ascent over per-token choices; deterministic).
#include <algorithm>
#include <cmath>

#include "grc2x/internal.h"
#include "vp/morph.h"
#include "vp/morph_grc.h"
#include "vp/text.h"

namespace vp::grc2x::detail {

using namespace vp::feat;

namespace {

bool isLetter(char32_t c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= 0xC0 && c <= 0x24F && c != 0xD7 && c != 0xF7) ||
         (c >= 0x0370 && c <= 0x03FF && c != 0x037E && c != 0x0387) || (c >= 0x1F00 && c <= 0x1FFF && c != 0x1FBD &&
                                                                        c != 0x1FBF && c != 0x1FFD && c != 0x1FFE) ||
         (c >= 0x300 && c <= 0x36F);
}
bool isElision(char32_t c) { return c == 0x2019 || c == '\'' || c == 0x02BC || c == 0x1FBD || c == 0x1FBF; }

bool isFinite(const Features& f) {
  return f.person && (f.mood == Indicative || f.mood == Subjunctive || f.mood == Imperative || f.mood == Optative);
}
bool isNominal(const Features& f, uint8_t lpos) {
  return f.case_ && f.mood != Infinitive && (lpos == Noun || lpos == Adj || lpos == Pron || lpos == Det ||
                                            lpos == Num || lpos == Name || lpos == Article || lpos == Participle ||
                                            f.mood == ParticipleMood);
}
bool genderOk(uint8_t a, uint8_t b) { return !a || !b || morph::genderAdmits(a, b) || morph::genderAdmits(b, a); }
bool agree(const Features& a, const Features& b) {
  return a.case_ == b.case_ && (!a.number || !b.number || a.number == b.number) && genderOk(a.gender, b.gender);
}

const char* const kModals[] = {"δύναμαι", "βούλομαι", "δεῖ", "ἔξεστι", "ἐθέλω", "μέλλω", "ἐπίσταμαι", "ἔξεστιν"};
const char* const kSubordinators[] = {"ὅτι", "ἐπεί", "εἰ", "ἐάν", "ἵνα", "ὅτε", "ὥστε", "ὅπωσ", "ἐπειδή", "διότι"};

}  // namespace

Analyser::Analyser(const lex::Lexicon& lx, const curated::CuratedData& cd, const grc::GreekData& gd,
                   const grc::GreekTables& gt)
    : lx_(lx), cd_(cd), gd_(gd), gt_(gt) {
  for (const grc::NameEntry& e : gd.names()) {
    if (e.policy == curated::NamePolicy::Translate) continue;
    for (uint8_t c : {Nom, Gen, Dat, Acc, Voc}) {
      std::string f;
      if (grc::declineName(e.nom, e.gen, e.declension, e.gender, c, e.voc, f))
        names_.push_back(NameForm{text::greek_key(f), e.english, c, e.gender});
    }
  }
  std::sort(names_.begin(), names_.end(), [](const NameForm& a, const NameForm& b) {
    return a.key != b.key ? a.key < b.key : a.case_ < b.case_;
  });
}

void Analyser::readings(const std::string& word, Token& t, const std::vector<rules::GlossaryEntry>* glossary) const {
  std::vector<Reading>& rs = t.readings;
  rs.clear();
  morph::Token mt;
  grc::analyse(lx_, word, mt);
  t.accentDiffers = mt.accentInsensitive;
  const std::string low = text::lower(word);
  // closed-class tables (article, personal / demonstrative / relative / interrogative pronouns, οὐδείς, numerals)
  std::vector<grc::ClosedReading> cr;
  grc::closedReadings(low, cr);
  bool article = false;
  // τίς / τί (accent on the first syllable: interrogative) against the enclitic τις / τι
  {
    const grc::AccentInfo ai = grc::accentOf(low);
    const bool firstAccent = ai.accents > 0 && ai.firstPosition == ai.syllables - 1;
    cr.erase(std::remove_if(cr.begin(), cr.end(), [&](const grc::ClosedReading& c) {
               const std::string_view k = c.lemmaKey;
               if (k == "τισ") return firstAccent && ai.syllables >= 1 && !(ai.syllables == 1 && ai.accents == 0);
               if (k == "τίσ") return !firstAccent;
               return false;
             }), cr.end());
  }
  for (const grc::ClosedReading& c : cr) {
    const bool art = std::string_view(c.lemmaKey) == "ὁ";
    article = article || art;
    Reading r;
    r.lemma = grc::findLemma(lx_, c.lemmaKey, art ? (uint8_t)Article : (uint8_t)0);
    if (r.lemma == kNone) r.lemma = grc::findLemma(lx_, c.lemmaKey);
    Features f;
    const std::string_view ck = c.lemmaKey;
    const bool numeral = ck == "εἷσ" || ck == "δύο" || ck == "τρεῖσ" || ck == "τέτταρεσ";
    const bool quant = ck == "πᾶσ" || ck == "οὐδείσ" || ck == "μηδείσ";
    f.pos = art ? (uint8_t)Article : numeral ? (uint8_t)Num : quant ? (uint8_t)Adj : (uint8_t)Pron;
    f.case_ = c.case_;
    f.number = c.number;
    f.gender = c.gender;
    f.person = (ck == "ἐγώ" || ck == "σύ" || ck == "ἡμεῖσ" || ck == "ὑμεῖσ") ? c.person : 0;
    r.packed = pack(f);
    r.display = word;
    r.closed = true;
    r.prior = 1.0;
    rs.push_back(r);
  }
  for (const lex::Analysis& a : mt.analyses) {
    const lex::Lemma l = lx_.lemma(a.lemma);
    if (l.id == kNone) continue;
    bool sameClosed = false;
    for (const Reading& r : rs) sameClosed = sameClosed || (r.closed && r.lemma == a.lemma);
    if (sameClosed || (article && l.pos != Prep)) continue;   // the tables win (spurious τοῦ / τόν readings)
    Reading r;
    r.lemma = a.lemma;
    r.packed = morph::packedOf(lx_, a);
    r.display = a.display.empty() ? word : grc::display(a.display);
    Features f = unpack(r.packed);
    double p = 0;
    uint8_t tier = l.tier;
    if (const curated::TierEntry* te = cd_.tierGreek(l.key))
      if (te->tier) tier = te->tier;
    p += tier == 1 ? 0.6 : tier == 2 ? 0.3 : 0;
    {
      const char* pn = l.pos == Noun ? "noun" : l.pos == Verb ? "verb" : l.pos == Adj ? "adj" : l.pos == Adv ? "adv"
                     : l.pos == Prep ? "prep" : l.pos == Conj ? "conj" : l.pos == Particle ? "particle" : "";
      if (*pn && gt_.readable(l.key, pn)) p += 0.5;
    }
    if (a.flags & lex::AltSpelling) p -= 0.3;
    if (a.flags & lex::NonAttic) p -= 0.6;
    if (a.flags & lex::PoeticRare) p -= 0.3;
    if (f.extra & Attic) p += 0.1;
    if (f.mood == ParticipleMood || l.pos == Participle) p -= 0.3;
    if (f.mood == Optative) p -= 0.4;
    if (f.number == Dual) p -= 0.8;
    if (f.case_ == Voc) p -= 0.2;
    if ((l.flags & lex::ProperName) && !t.capitalised) p -= 0.5;
    if (l.freqRank) p += 0.2 * (1.0 - std::min<double>(l.freqRank, 5000) / 5000.0);
    if (mt.accentInsensitive) p -= 0.5;
    r.prior = p;
    rs.push_back(r);
  }
  for (const morph::RuleAnalysis& a : mt.ruleAnalyses) {
    Reading r;
    r.lemma = a.lemma;
    r.packed = a.packed;
    r.display = a.display;
    r.fromRule = true;
    r.prior = -0.5;
    rs.push_back(r);
  }
  // names: names_grc.tsv forms, the glossary
  const std::string key = text::greek_key(word);
  auto it = std::lower_bound(names_.begin(), names_.end(), key,
                             [](const NameForm& n, const std::string& k) { return n.key < k; });
  for (; it != names_.end() && it->key == key; ++it) {
    Reading r;
    Features f;
    f.pos = Name;
    f.case_ = it->case_;
    f.number = Sg;
    f.gender = it->gender;
    r.packed = pack(f);
    r.display = word;
    r.name = true;
    r.nameEn = it->english;
    r.prior = 1.5;
    rs.push_back(r);
  }
  if (glossary && rs.empty())
    for (const rules::GlossaryEntry& g : *glossary)
      if (!g.form.empty() && text::greek_bare(g.form) == text::greek_bare(word)) {
        Reading r;
        Features f;
        f.pos = Name;
        f.number = Sg;
        f.gender = g.gender == "f" ? F : g.gender == "n" ? N : M;
        r.packed = pack(f);
        r.display = word;
        r.name = true;
        r.nameEn = g.name;
        rs.push_back(r);
      }
  if (t.capitalised)
    for (Reading& r : rs)
      if (r.lemma != kNone && (lx_.lemma(r.lemma).flags & lex::ProperName)) { r.name = true; r.prior += 0.5; }
  if (rs.empty()) {
    if (t.capitalised) {
      t.nameGuess = true;
      Reading r;
      Features f;
      f.pos = Name;
      r.packed = pack(f);
      r.display = word;
      r.name = true;
      r.nameEn = word;
      rs.push_back(r);
    } else {
      t.unknown = true;
    }
  }
  for (const Reading& r : rs) t.fromRule = t.fromRule || (r.fromRule && rs.size() == 1);
  std::stable_sort(rs.begin(), rs.end(), [](const Reading& a, const Reading& b) {
    if (a.prior != b.prior) return a.prior > b.prior;
    return a.lemma < b.lemma;
  });
}

void Analyser::analyse(std::string_view sentence, Sentence& out, const std::vector<rules::GlossaryEntry>* glossary) const {
  out.clear();
  out.text = std::string(sentence);
  const std::string& s = out.text;
  size_t i = 0;
  while (i < s.size()) {
    size_t j = i;
    const char32_t c = text::decodeUtf8(s, j);
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == 0xA0) { i = j; continue; }
    if (isLetter(c) && !(c >= 0x300 && c <= 0x36F)) {
      size_t k = j;
      while (k < s.size()) {
        size_t m = k;
        const char32_t d = text::decodeUtf8(s, m);
        if (!isLetter(d)) {
          if (isElision(d)) k = m;   // elided word: the mark belongs to it
          break;
        }
        k = m;
      }
      Token t;
      t.text = s.substr(i, k - i);
      t.start = (int)i;
      t.end = (int)k;
      t.kind = TokKind::Word;
      size_t p = 0;
      const char32_t first = text::decodeUtf8(t.text, p);
      t.capitalised = (first >= 'A' && first <= 'Z') || (first >= 0x0391 && first <= 0x03A9) ||
                      (first >= 0x1F08 && first <= 0x1FFC && ((first & 0xF) >= 8));
      out.tokens.push_back(std::move(t));
      i = k;
      continue;
    }
    if (c >= '0' && c <= '9') {
      size_t k = j;
      while (k < s.size() && s[k] >= '0' && s[k] <= '9') ++k;
      Token t;
      t.text = s.substr(i, k - i);
      t.start = (int)i;
      t.end = (int)k;
      t.kind = TokKind::Number;
      out.tokens.push_back(std::move(t));
      i = k;
      continue;
    }
    Token t;
    t.text = s.substr(i, j - i);
    if (c == 0x037E) t.text = ";";            // Greek question mark (canonically ";")
    if (c == 0x0387) t.text = "\xC2\xB7";     // ano teleia
    t.start = (int)i;
    t.end = (int)j;
    t.kind = TokKind::Punct;
    out.tokens.push_back(std::move(t));
    i = j;
  }
  // final punctuation and question
  for (size_t k = out.tokens.size(); k-- > 0;) {
    const Token& t = out.tokens[k];
    if (t.kind != TokKind::Punct) break;
    if (t.text == "." || t.text == ";" || t.text == "!" || t.text == "?" || t.text == "\xE2\x80\xA6")
      out.finalPunct = t.text + out.finalPunct;
  }
  out.question = out.finalPunct.find(';') != std::string::npos || out.finalPunct.find('?') != std::string::npos;
  // readings; a sentence-initial capital is not a name by itself (decision 5: sentence starts are capitalised)
  bool first = true;
  for (Token& t : out.tokens) {
    if (t.kind != TokKind::Word) continue;
    const bool cap = t.capitalised;
    readings(t.text, t, glossary);
    if (first && cap) {
      bool common = false;
      for (const Reading& r : t.readings) common = common || (!r.name && r.lemma != kNone);
      if (common) {
        t.nameGuess = false;
        for (Reading& r : t.readings)
          if (r.name && r.lemma != kNone && !(lx_.lemma(r.lemma).flags & lex::ProperName)) r.name = false;
      }
    }
    first = false;
  }
  // clauses: punctuation, subordinators, καί / ἀλλά between two finite verbs
  int clause = 0;
  for (size_t k = 0; k < out.tokens.size(); ++k) {
    Token& t = out.tokens[k];
    if (t.kind == TokKind::Punct) { t.clause = clause; if (t.text != "\xE2\x80\x99") ++clause; continue; }
    const std::string key = text::greek_key(grc::ultimaToAcute(t.text));
    bool sub = false;
    for (const char* x : kSubordinators) sub = sub || key == x;
    if (sub && k > 0 && out.tokens[k - 1].kind != TokKind::Punct) ++clause;
    t.clause = clause;
  }
  disambiguate(out);
}

void Analyser::disambiguate(Sentence& s) const {
  const size_t n = s.tokens.size();
  std::vector<size_t> pick(n, 0);
  auto feats = [&](size_t i, size_t r) { return unpack(s.tokens[i].readings[r].packed); };
  auto lpos = [&](size_t i, size_t r) -> uint8_t {
    const Reading& rd = s.tokens[i].readings[r];
    if (rd.closed) return unpack(rd.packed).pos;
    if (rd.name) return Name;
    return rd.lemma != kNone ? lx_.lemma(rd.lemma).pos : 0;
  };
  auto keyOf = [&](size_t i, size_t r) -> std::string {
    const Reading& rd = s.tokens[i].readings[r];
    return rd.lemma != kNone ? std::string(lx_.lemma(rd.lemma).key) : std::string();
  };
  auto word = [&](size_t i) { return s.tokens[i].kind == TokKind::Word && !s.tokens[i].readings.empty(); };
  auto chosen = [&](size_t i) { return feats(i, pick[i]); };
  auto chosenPos = [&](size_t i) { return lpos(i, pick[i]); };
  auto nextWord = [&](size_t i) -> long {
    for (size_t k = i + 1; k < n; ++k) {
      if (s.tokens[k].kind == TokKind::Punct) return -1;
      if (word(k)) return (long)k;
    }
    return -1;
  };
  auto prevWord = [&](size_t i) -> long {
    for (size_t k = i; k-- > 0;) {
      if (s.tokens[k].kind == TokKind::Punct) return -1;
      if (word(k)) return (long)k;
    }
    return -1;
  };
  auto isArticle = [&](size_t i, size_t r) { return lpos(i, r) == Article; };
  auto isPrep = [&](size_t i, size_t r) { return lpos(i, r) == Prep; };
  auto adverbish = [&](size_t i) {
    const uint8_t p = chosenPos(i);
    return p == Adv || p == Particle;
  };
  auto local = [&](size_t i, size_t r, std::vector<std::string>* why) {
    const Reading& rd = s.tokens[i].readings[r];
    const Features f = feats(i, r);
    const uint8_t lp = lpos(i, r);
    double sc = rd.prior;
    const long pw = prevWord(i), nw = nextWord(i);
    // (a) article: the strongest clue for the next nominal (skipping degree adverbs) and for itself
    if (lp == Article) {
      long j = nw;
      while (j >= 0 && adverbish((size_t)j)) j = nextWord((size_t)j);
      if (j >= 0) {
        const Features g = chosen((size_t)j);
        if (isNominal(g, chosenPos((size_t)j))) sc += agree(f, g) ? 3.0 : -2.0;
        else sc -= 1.0;
      }
    } else if (isNominal(f, lp)) {
      long a = pw;
      while (a >= 0 && adverbish((size_t)a)) a = prevWord((size_t)a);
      // an article or an agreeing adjective right before
      if (a >= 0 && isArticle((size_t)a, pick[(size_t)a])) {
        const bool ok = agree(f, chosen((size_t)a));
        sc += ok ? 3.0 : -2.0;
        if (why && ok) why->push_back("article " + s.tokens[(size_t)a].text);
      } else if (a >= 0 && isNominal(chosen((size_t)a), chosenPos((size_t)a)) && chosenPos((size_t)a) != Pron &&
                 (lp == Noun || chosenPos((size_t)a) == Noun) && lp != Pron) {
        if (agree(f, chosen((size_t)a))) sc += 1.0;
      }
      // an adjective / numeral / quantity word before its noun agrees with it
      if ((lp == Adj || lp == Num || lp == Det) && nw >= 0 && chosenPos((size_t)nw) == Noun)
        sc += agree(f, chosen((size_t)nw)) ? 1.5 : -0.5;
      // a preposition before the group fixes its case
      long p = pw;
      while (p >= 0 && (isArticle((size_t)p, pick[(size_t)p]) || adverbish((size_t)p) ||
                        (isNominal(chosen((size_t)p), chosenPos((size_t)p)) && chosen((size_t)p).case_ == f.case_ &&
                         chosenPos((size_t)p) != Pron)))
        p = prevWord((size_t)p);
      if (p >= 0 && isPrep((size_t)p, pick[(size_t)p])) {
        const uint16_t cases = gd_.prepCases(keyOf((size_t)p, pick[(size_t)p]));
        if (cases) {
          const bool ok = cases & (1u << f.case_);
          sc += ok ? 2.0 : -2.0;
          if (why && ok) why->push_back("after " + s.tokens[(size_t)p].text);
        }
      }
      // ὦ + vocative
      if (pw >= 0 && text::greek_key(s.tokens[(size_t)pw].text) == "ὦ") sc += f.case_ == Voc ? 3.0 : -1.0;
      // enclitic possessor after a noun
      if (rd.closed && lp == Pron && f.case_ == Gen && pw >= 0 && isNominal(chosen((size_t)pw), chosenPos((size_t)pw)))
        sc += 1.0;
      // nominative subject agreeing with the clause's verb
      if (f.case_ == Nom)
        for (size_t k = 0; k < n; ++k)
          if (k != i && word(k) && s.tokens[k].clause == s.tokens[i].clause && isFinite(chosen(k))) {
            const Features v = chosen(k);
            const bool neutPl = f.number == Pl && f.gender == N;
            if ((v.number == f.number || (neutPl && v.number == Sg)) && (v.person == P3 || rd.closed)) sc += 0.5;
            break;
          }
    } else if (lp == Prep) {
      if (nw >= 0) sc += 0.5;
    }
    // finite verb: one per clause; agreement with a nominative
    if (isFinite(f)) {
      bool other = false;
      for (size_t k = 0; k < n; ++k)
        if (k != i && word(k) && s.tokens[k].clause == s.tokens[i].clause && isFinite(chosen(k))) other = true;
      sc += other ? -1.0 : 1.0;
      for (size_t k = 0; k < n; ++k) {
        if (k == i || !word(k) || s.tokens[k].clause != s.tokens[i].clause) continue;
        const Features g = chosen(k);
        if (g.case_ != Nom || !isNominal(g, chosenPos(k)) || chosenPos(k) == Article) continue;
        const bool neutPl = g.number == Pl && g.gender == N;
        const uint8_t person = g.person ? g.person : 3;
        if ((g.number == f.number || (neutPl && f.number == Sg)) && person == f.person) {
          sc += 1.0;
          if (why) why->push_back("subject " + s.tokens[k].text);
          break;
        }
      }
      if (f.mood == Imperative && s.question) sc -= 0.5;
    }
    // infinitive after a modal verb of its clause
    if (f.mood == Infinitive)
      for (size_t k = 0; k < n; ++k)
        if (k != i && word(k) && s.tokens[k].clause == s.tokens[i].clause && isFinite(chosen(k))) {
          const std::string mk = keyOf(k, pick[k]);
          for (const char* m : kModals)
            if (mk == text::greek_key(m)) sc += 1.5;
        }
    return sc;
  };
  // initial: best prior; then coordinate ascent
  for (int pass = 0; pass < 4; ++pass) {
    bool changed = false;
    for (size_t i = 0; i < n; ++i) {
      if (!word(i) || s.tokens[i].readings.size() < 2) continue;
      size_t best = pick[i];
      double bs = local(i, best, nullptr);
      for (size_t r = 0; r < s.tokens[i].readings.size(); ++r) {
        const double v = local(i, r, nullptr);
        if (v > bs + 1e-9) { bs = v; best = r; }
      }
      if (best != pick[i]) { pick[i] = best; changed = true; }
    }
    if (!changed) break;
  }
  // chosen reading first; scores, surviving readings, confidence, why
  for (size_t i = 0; i < n; ++i) {
    Token& t = s.tokens[i];
    if (!word(i)) continue;
    std::vector<double> sc(t.readings.size());
    for (size_t r = 0; r < t.readings.size(); ++r) sc[r] = local(i, r, nullptr);
    t.why.clear();
    local(i, pick[i], &t.why);
    const double top = sc[pick[i]];
    for (size_t r = 0; r < t.readings.size(); ++r) t.readings[r].prior = sc[r];
    std::vector<std::string> seen;
    int surv = 0;
    for (size_t r = 0; r < t.readings.size(); ++r) {
      if (sc[r] < top - 0.5) continue;
      const Features f = unpack(t.readings[r].packed);
      const std::string sig = std::to_string(t.readings[r].lemma) + ":" + std::to_string(f.case_) + ":" +
                              std::to_string(f.number) + ":" + std::to_string(f.person) + ":" + std::to_string(f.tense) +
                              ":" + std::to_string(f.mood);
      if (std::find(seen.begin(), seen.end(), sig) == seen.end()) { seen.push_back(sig); ++surv; }
    }
    t.surviving = std::max(1, surv);
    t.confidence = 1.0 / t.surviving;
    if (pick[i] != 0) std::swap(t.readings[0], t.readings[pick[i]]);
    std::stable_sort(t.readings.begin() + 1, t.readings.end(),
                     [](const Reading& a, const Reading& b) { return a.prior > b.prior; });
  }
}

void tokInfos(const lex::Lexicon& lx, const Sentence& s, std::vector<TokInfo>& out) {
  out.clear();
  out.resize(s.tokens.size());
  for (size_t i = 0; i < s.tokens.size(); ++i) {
    const Token& t = s.tokens[i];
    TokInfo& x = out[i];
    x.text = t.text;
    x.punct = t.kind == TokKind::Punct;
    x.number = t.kind == TokKind::Number;
    x.word = t.kind == TokKind::Word;
    const Reading* r = t.best();
    if (!r) continue;
    x.lemma = r->lemma;
    x.f = unpack(r->packed);
    x.name = r->name;
    x.nameEn = r->nameEn;
    x.closed = r->closed;
    if (r->lemma != kNone) {
      const lex::Lemma l = lx.lemma(r->lemma);
      x.key = std::string(l.key);
      x.lpos = r->closed ? x.f.pos : l.pos;
      x.lgender = l.gender;
    } else {
      x.lpos = x.f.pos;
    }
    if (r->name && r->lemma == kNone) x.lpos = Name;
  }
}

std::string featureText(const Features& f, uint8_t lemmaPos) {
  static const char* const kPos[] = {"", "noun", "verb", "adjective", "adverb", "pronoun", "numeral", "preposition",
                                     "conjunction", "interjection", "determiner", "name", "particle", "participle"};
  static const char* const kCase[] = {"", "nominative", "genitive", "dative", "accusative", "ablative", "vocative", "locative"};
  static const char* const kNum[] = {"", "singular", "plural", "dual"};
  static const char* const kGen[] = {"", "masculine", "feminine", "neuter", "masc./fem.", "masc./neut.", "fem./neut.", "any gender"};
  static const char* const kTense[] = {"", "present", "imperfect", "future", "perfect", "pluperfect", "future perfect", "aorist"};
  static const char* const kMood[] = {"", "indicative", "subjunctive", "imperative", "infinitive", "participle", "gerund", "optative"};
  static const char* const kVoice[] = {"", "active", "passive", "middle"};
  std::string o;
  const uint8_t p = lemmaPos ? lemmaPos : f.pos;
  if (p == Article) o = "article";
  else if (p < sizeof(kPos) / sizeof(kPos[0])) o = kPos[p];
  auto add = [&](const char* w) {
    if (!w || !*w) return;
    if (!o.empty()) o += o.find(',') == std::string::npos ? ", " : " ";
    o += w;
  };
  if (f.person) add(f.person == 1 ? "1st person" : f.person == 2 ? "2nd person" : "3rd person");
  if (f.case_ < 8) add(kCase[f.case_]);
  if (f.number < 4) add(kNum[f.number]);
  if (f.gender < 8 && (f.case_ || f.mood == ParticipleMood)) add(kGen[f.gender]);
  if (f.tense < 8) add(kTense[f.tense]);
  if (f.mood < 8) add(kMood[f.mood]);
  if (f.voice < 4 && f.mood) add(kVoice[f.voice]);
  return o;
}

}  // namespace vp::grc2x::detail
