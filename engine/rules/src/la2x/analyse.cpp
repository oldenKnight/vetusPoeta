// Analyser: tokenisation, readings and disambiguation by constraint propagation (vp/la2x.h).
// The search is a beam over the readings of the words, left to right. Every step adds the reading's prior and the
// pair terms with the words already chosen (preposition -> case, NP agreement, subject-verb number/person, valency,
// genitive attributes, relative pronoun antecedent, nōlī + infinitive); when the last word of a clause is chosen the
// clause terms are added (one finite verb, verb-final, one subject, modifiers that agree, mood after ut/nē). Clauses
// come from a segmentation of the best readings; the search runs again when the segmentation changes (<= 3 passes).
#include <algorithm>
#include <cmath>
#include <unordered_map>

#include "internal.h"
#include "vp/features.h"
#include "vp/la2x.h"
#include "vp/morph.h"
#include "vp/text.h"

namespace vp::la2x {

using namespace vp::feat;

namespace {

enum SenseTag : uint16_t {
  TagTransitive = 1u << 0, TagIntransitive = 1u << 1, TagRare = 1u << 3, TagArchaic = 1u << 4, TagPoetic = 1u << 5,
  TagMedieval = 1u << 6, TagNewLatin = 1u << 7, TagDat = 1u << 8, TagAbl = 1u << 9, TagGen = 1u << 10,
  TagAcc = 1u << 11, TagInf = 1u << 12
};

bool isLetterCp(char32_t c) {
  if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) return true;
  if (c >= 0xC0 && c <= 0x24F && c != 0xD7 && c != 0xF7) return true;
  if (c >= 0x0300 && c <= 0x036F) return true;   // combining marks (macron, breve)
  if (c >= 0x1E00 && c <= 0x1EFF) return true;
  return false;
}
bool isDigitCp(char32_t c) { return c >= '0' && c <= '9'; }

bool hasLengthMark(std::string_view s) {
  const std::string d = text::nfd(s);
  return d.find("\xCC\x84") != std::string::npos;
}

bool headIsWord(std::string_view head) {
  if (head.empty()) return false;
  size_t i = 0;
  while (i < head.size()) {
    const char32_t c = text::decodeUtf8(head, i);
    if (!isLetterCp(c) && c != ' ' && c != '-' && c != '.') return false;
  }
  return true;
}

uint16_t genderBits(uint8_t g) {
  switch (g) {
    case M: return 1;
    case F: return 2;
    case N: return 4;
    case MF: return 3;
    case MN: return 5;
    case FN: return 6;
    case MFN: return 7;
    default: return 7;   // unknown admits all
  }
}
bool genderCompat(uint8_t a, uint8_t b) { return (genderBits(a) & genderBits(b)) != 0; }

const char* const kSubordinators[] = {"antequam", "cum",     "donec", "dum",  "etsi", "ne",       "nisi",
                                      "postquam", "priusquam", "quamquam", "quia", "quod", "quoniam", "si",
                                      "simul",    "ubi",     "ut"};
bool isSubKey(std::string_view k) {
  for (const char* s : kSubordinators)
    if (k == s) return true;
  return false;
}
const char* const kCoordKeys[] = {"et", "atque", "ac", "sed", "aut", "uel", "neque", "nec", "at", "autem"};
bool isKeyOf(const std::string& k, std::initializer_list<const char*> ks) {
  for (const char* x : ks)
    if (k == x) return true;
  return false;
}
bool isCoordKey(std::string_view k) {
  for (const char* s : kCoordKeys)
    if (k == s) return true;
  return false;
}

// Per reading facts used by the scorer.
struct RI {
  Features f;
  uint8_t lpos = 0, lgender = 0;
  uint16_t lflags = 0;
  std::string key;          // lemma key
  std::string encl;         // enclitic of this reading
  bool finite = false, inf = false, nominal = false, head = false, modifier = false, prep = false, conj = false,
       sub = false, rel = false, copula = false, intjO = false, nolo = false, voc = false, imper = false,
       subj = false, partic = false, coord = false, adv = false;
  uint16_t prepMask = 0;
  uint8_t person = 3;       // person of a nominal head (ego 1, tū 2)
  bool accOK = false, datOK = false, intrOnly = false, infOK = false, accInf = false, valKnown = false;
  bool personNoun = false;  // a noun denoting a person
  uint8_t tier = 0;         // the lemma's tier
  bool motion = false;      // a verb of motion (accusative of place = "to")
  bool perfPart = false;    // perfect participle (amātus, secūtus, ingressus <- ingredior): + sum = one verb
  bool adjToo = false;      // the same token also reads as an adjective lemma (laetus, īrātus): a predicate, not a verb
  bool placeAcc = false;    // domum / rūs: accusative of place to, an adverb in effect
};

struct Hyp {
  std::vector<uint8_t> c;   // reading index per token
  double s = 0;
};

}  // namespace

struct Analyser::Impl {
  const lex::Lexicon& lx;
  const curated::CuratedData& cd;
  struct NameForm { std::string key, display; uint8_t case_ = 0, gender = 0; };
  std::vector<NameForm> nameForms;   // names_la.tsv, every declined form, sorted by key
  mutable std::unordered_map<uint32_t, uint32_t> valCache;   // lemma -> packed valency bits (capped)
  mutable std::vector<lex::Sense> senseBuf;
  mutable std::vector<lex::Analysis> anaBuf;
  std::vector<std::string> personNouns;   // sorted latin keys

  Impl(const lex::Lexicon& l, const curated::CuratedData& c) : lx(l), cd(c) {
    for (const curated::NameEntry& n : cd.names())
      if (n.policy != curated::NamePolicy::Translate) declineName(n.latinNom, n.latinGen, n.declension, n.gender, nameForms);
    std::sort(nameForms.begin(), nameForms.end(), [](const NameForm& a, const NameForm& b) {
      return a.key != b.key ? a.key < b.key : a.case_ < b.case_;
    });
  }

  static bool endsWith(const std::string& s, const char* x) {
    const size_t n = std::char_traits<char>::length(x);
    return s.size() >= n && s.compare(s.size() - n, n, x) == 0;
  }

  // Declined forms of a name (1st, 2nd, 3rd declension; others indeclinable).
  static void declineName(const std::string& nom0, const std::string& gen0, int decl, uint8_t gender,
                          std::vector<NameForm>& out) {
    if (nom0.empty()) return;
    const std::string nom = text::nfc(nom0), gen = text::nfc(gen0);
    auto add = [&](const std::string& form, uint8_t c) {
      out.push_back(NameForm{text::latin_key(form), form, c, gender});
    };
    add(nom, Nom);
    if (decl == 1 && endsWith(nom, "a")) {
      const std::string st = nom.substr(0, nom.size() - 1);
      add(st + "ae", Gen); add(st + "ae", Dat); add(st + "am", Acc); add(st + "ā", Abl); add(nom, Voc);
    } else if (decl == 2) {
      std::string st = !gen.empty() && endsWith(gen, "ī") ? gen.substr(0, gen.size() - 2) : std::string();
      if (st.empty() && endsWith(nom, "us")) st = nom.substr(0, nom.size() - 2);
      if (st.empty()) return;
      add(st + "ī", Gen); add(st + "ō", Dat); add(st + "um", Acc); add(st + "ō", Abl);
      if (endsWith(nom, "ius")) add(nom.substr(0, nom.size() - 3) + "ī", Voc);
      else if (endsWith(nom, "us")) add(st + "e", Voc);
      else add(nom, Voc);
    } else if (decl == 3 && endsWith(gen, "is")) {
      const std::string st = gen.substr(0, gen.size() - 2);
      add(gen, Gen); add(st + "ī", Dat); add(st + "em", Acc); add(st + "e", Abl); add(nom, Voc);
    } else {
      for (uint8_t c : {Gen, Dat, Acc, Abl, Voc}) add(nom, c);
    }
  }

  uint32_t valency(uint32_t lemma, const std::string& key) const {
    auto it = valCache.find(lemma);
    if (it != valCache.end()) return it->second;
    uint32_t v = 0;   // bit0 accOK bit1 datOK bit2 intrOnly bit3 infOK bit4 accInf bit5 copula bit6 known
    if (const curated::Valency* va = cd.valency(key)) {
      bool onlyIntr = !va->frames.empty();
      for (const curated::Frame& fr : va->frames) {
        using K = curated::FrameKind;
        switch (fr.kind) {
          case K::Acc: case K::AccAcc: case K::AccAbl: case K::Refl: v |= 1; onlyIntr = false; break;
          case K::DatAcc: v |= 3; onlyIntr = false; break;
          case K::Dat: v |= 2; onlyIntr = false; break;
          case K::AccInf: v |= 1 | 16 | 8; onlyIntr = false; break;
          case K::Inf: v |= 8; onlyIntr = false; break;
          case K::Copula: v |= 32; onlyIntr = false; break;
          case K::Impers: if (fr.impers.find("dat") != std::string::npos) v |= 2; if (fr.impers.find("acc") != std::string::npos) v |= 1; onlyIntr = false; break;
          case K::Intr: case K::Prep: break;
          default: onlyIntr = false; break;
        }
      }
      if (onlyIntr) v |= 4;
      v |= 64;
    } else {
      senseBuf.clear();
      lx.senses(lemma, senseBuf);
      uint16_t tags = 0;
      bool anyTrans = false;
      for (size_t i = 0; i < senseBuf.size() && i < 4; ++i) {
        tags |= senseBuf[i].tags;
        anyTrans = anyTrans || (senseBuf[i].tags & (TagTransitive | TagAcc));
      }
      if (tags & (TagTransitive | TagAcc)) v |= 1;
      if (tags & TagDat) v |= 2;
      if ((tags & TagIntransitive) && !anyTrans) v |= 4;
      if (tags & TagInf) v |= 8;
      if (tags) v |= 64;
    }
    if (valCache.size() > 4096) valCache.clear();
    valCache.emplace(lemma, v);
    return v;
  }

  // ---- tokenisation ----------------------------------------------------------------------------------------------
  void tokenise(std::string_view s, std::vector<Token>& out) const {
    size_t i = 0;
    while (i < s.size()) {
      size_t j = i;
      const char32_t c = text::decodeUtf8(s, j);
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == 0xA0) { i = j; continue; }
      Token t;
      t.start = (int)i;
      if (isLetterCp(c)) {
        size_t k = j;
        while (k < s.size()) {
          size_t m = k;
          const char32_t d = text::decodeUtf8(s, m);
          if (!isLetterCp(d)) break;
          k = m;
        }
        t.kind = TokKind::Word;
        j = k;
      } else if (isDigitCp(c)) {
        size_t k = j;
        while (k < s.size() && (isDigitCp((unsigned char)s[k]) || s[k] == '.' || s[k] == ',') &&
               !(k + 1 >= s.size() && (s[k] == '.' || s[k] == ',')) &&
               !((s[k] == '.' || s[k] == ',') && (k + 1 >= s.size() || !isDigitCp((unsigned char)s[k + 1]))))
          ++k;
        t.kind = TokKind::Number;
        j = k;
      } else {
        t.kind = TokKind::Punct;
        if (c == '.') while (j < s.size() && s[j] == '.') ++j;   // "..."
      }
      t.end = (int)j;
      t.text = std::string(s.substr(i, j - i));
      out.push_back(std::move(t));
      i = j;
    }
  }

  // ---- readings ----------------------------------------------------------------------------------------------------
  void readings(Token& t, bool macrons, const std::vector<rules::GlossaryEntry>* glossary, const Sentence& s,
                size_t idx) const {
    morph::Token mt;
    morph::analyseLatin(lx, t.text, mt);
    t.capitalised = mt.capitalised;
    if (mt.enclitic) t.encl = mt.encliticText == "ve" ? "ue" : mt.encliticText;
    t.fromRule = mt.fromRule;
    std::string base = t.text;
    if (mt.enclitic) {
      const std::string low = text::lower(text::nfc(base));
      base = low.substr(0, low.size() >= mt.encliticText.size() ? low.size() - mt.encliticText.size() : 0);
    }
    const std::string tokCanon = text::lower(morph::displayForm(base, true));
    std::vector<Reading>& v = t.readings;
    const std::string enclAll = t.encl;
    for (int pass = 0; pass < 2 && v.empty(); ++pass) {
      const bool strict = macrons && pass == 0;
      for (const lex::Analysis& a : mt.analyses) {
        if (strict && text::lower(morph::displayForm(a.display, true)) != tokCanon &&
            text::nfd(a.display).find("\xCC\x86") == std::string::npos)
          continue;
        Reading r;
        r.lemma = a.lemma;
        r.packed = lx.feature(a.feat);
        r.display = std::string(a.display);
        r.aflags = a.flags;
        r.encl = enclAll;
        v.push_back(std::move(r));
      }
      for (const morph::RuleAnalysis& a : mt.ruleAnalyses) {
        if (strict && text::lower(morph::displayForm(a.display, true)) != tokCanon) continue;
        Reading r;
        r.lemma = a.lemma;
        r.packed = a.packed;
        r.display = a.display;
        r.fromRule = true;
        r.encl = enclAll;
        v.push_back(std::move(r));
      }
    }
    // the whole word is known, but an enclitic split reads it as a core word ("vidēsne" = vidēs + ne, not viden)
    if (!mt.enclitic && !mt.analyses.empty()) {
      const std::string k0 = text::latin_key(t.text);
      for (const char* enc : {"que", "ne", "ue"}) {
        const size_t el = std::char_traits<char>::length(enc);
        if (k0.size() <= el + 1 || k0.compare(k0.size() - el, el, enc) != 0) continue;
        const std::string low = text::lower(text::nfc(t.text));
        const std::string baseText = low.substr(0, low.size() - el);
        morph::Token bt;
        morph::analyseLatin(lx, baseText, bt);
        if (bt.analyses.empty() || bt.enclitic) continue;
        const std::string baseCanon = text::lower(morph::displayForm(baseText, true));
        for (const lex::Analysis& a : bt.analyses) {
          const lex::Lemma bl = lx.lemma(a.lemma);
          const Features bf = unpack(lx.feature(a.feat));
          if (bl.tier != 1 && bl.tier != 2) continue;
          if (std::string(enc) == "ne" && !(bf.pos == Verb || bf.pos == Noun || bf.pos == Pron || bf.pos == Adj || bf.pos == Adv)) continue;
          if (macrons && text::lower(morph::displayForm(a.display, true)) != baseCanon) continue;
          Reading r;
          r.lemma = a.lemma;
          r.packed = lx.feature(a.feat);
          r.display = std::string(a.display);
          r.aflags = a.flags;
          r.encl = enc;
          v.push_back(std::move(r));
        }
        break;
      }
    }
    // weak readings only when nothing else reads the word: alternative spellings (not of closed-class words),
    // symbol/affix lemmas, phrases, citation-only nominal entries (no case)
    auto weak = [&](const Reading& r) {
      const lex::Lemma l = lx.lemma(r.lemma);
      const Features f = unpack(r.packed);
      if (!headIsWord(l.head)) return true;
      if (l.glossEn.substr(0, 15) == "The name of the" || l.glossEn.substr(0, 13) == "A letter of t") return true;
      if ((r.aflags & lex::AltSpelling) && l.pos != Prep && l.pos != Conj) return true;
      if (l.pos == Symbol || l.pos == Suffix || l.pos == Prefix || l.pos == Phrase) return true;
      if ((f.pos == Noun || f.pos == Adj || f.pos == Name || f.pos == Participle || f.pos == Det) && f.case_ == 0 &&
          !(l.flags & lex::Indeclinable))
        return true;
      if (f.pos == Pron && f.case_ == 0 && l.tier != 1) return true;
      return false;
    };
    if (std::any_of(v.begin(), v.end(), [&](const Reading& r) { return !weak(r); }))
      v.erase(std::remove_if(v.begin(), v.end(), weak), v.end());
    // a closed-class word outranks interjections and caseless nouns spelled the same
    if (std::any_of(v.begin(), v.end(), [&](const Reading& r) {
          const uint8_t p = lx.lemma(r.lemma).pos;
          return p == Prep || p == Conj || p == Particle;
        }))
      v.erase(std::remove_if(v.begin(), v.end(), [&](const Reading& r) {
                const Features f = unpack(r.packed);
                const lex::Lemma l = lx.lemma(r.lemma);
                return (l.pos == Intj && text::latin_key(l.head) != "o") ||
                       ((f.pos == Noun || f.pos == Name) && (f.case_ == 0 || (l.flags & lex::Indeclinable)));
              }), v.end());
    // duplicates (lemma, features)
    std::stable_sort(v.begin(), v.end(), [](const Reading& a, const Reading& b) {
      if (a.lemma != b.lemma) return a.lemma < b.lemma;
      if (a.packed != b.packed) return a.packed < b.packed;
      return a.encl < b.encl;
    });
    v.erase(std::unique(v.begin(), v.end(), [](const Reading& a, const Reading& b) {
              return a.lemma == b.lemma && a.packed == b.packed && a.encl == b.encl;
            }), v.end());
    // names: names_la.tsv, the project glossary
    const std::string key = mt.key.empty() ? text::latin_key(t.text) : text::latin_key(t.text);
    auto addName = [&](const std::string& display, uint8_t c, uint8_t g) {
      for (const Reading& r : v) {
        const Features f = unpack(r.packed);
        if ((f.pos == Name || (lx.lemma(r.lemma).flags & lex::ProperName)) && f.case_ == c) return;
      }
      Reading r;
      Features f;
      f.pos = Name;
      f.case_ = c;
      f.number = Sg;
      f.gender = g;
      r.packed = pack(f);
      r.display = display;
      r.name = true;
      r.nameGender = g;
      v.push_back(r);
    };
    {
      auto it = std::lower_bound(nameForms.begin(), nameForms.end(), key,
                                 [](const NameForm& a, const std::string& k) { return a.key < k; });
      for (; it != nameForms.end() && it->key == key; ++it) addName(it->display, it->case_, it->gender);
    }
    if (glossary) {
      for (const rules::GlossaryEntry& g : *glossary) {
        const std::string form = g.form.empty() ? g.name : g.form;
        if (form.empty()) continue;
        const uint8_t gg = g.gender == "f" ? F : g.gender == "m" ? M : g.gender == "n" ? N : 0;
        std::vector<NameForm> forms;
        declineName(form, std::string(), g.declension, gg, forms);
        for (const NameForm& nf : forms)
          if (nf.key == key) addName(nf.display, nf.case_, gg);
      }
    }
    for (Reading& r : v) {
      const lex::Lemma l = lx.lemma(r.lemma);
      if (r.lemma != lex::kNoLemma && ((l.flags & lex::ProperName) || unpack(r.packed).pos == Name)) r.name = true;
    }
    if (v.empty()) {
      if (t.capitalised && (!t.initial || idx + 1 < s.tokens.size())) {
        // capitalised unknown word: a name (case open), Check
        Reading r;
        Features f;
        f.pos = Name;
        f.number = Sg;
        r.packed = pack(f);
        r.display = t.text;
        r.name = true;
        v.push_back(r);
        t.nameGuess = true;
      } else {
        t.unknown = true;
      }
    }
  }

  // ---- priors --------------------------------------------------------------------------------------------------------
  // Tier of a reading's lemma; a participle lemma counts at its verb's tier (lēctus -> legō, tier 1).
  uint8_t readingTier(const Reading& r) const {
    if (r.lemma == lex::kNoLemma) return 0;
    const lex::Lemma l = lx.lemma(r.lemma);
    if (l.pos == Participle) {
      const uint32_t v = detail::verbOfParticipleLemma(lx, r.lemma, anaBuf);
      if (v != lex::kNoLemma) {
        const lex::Lemma vl = lx.lemma(v);
        return cd.effectiveTier(vl.key, vl.pos, vl.tier);
      }
    }
    return cd.effectiveTier(l.key, l.pos, l.tier);
  }

  // pastCtx: another word of the sentence is a past-only finite verb or a past adverb (heri, ōlim): a form spelled
  // like both the present and the perfect of one lemma (legit, venit) is read as the perfect.
  double prior(const Token& t, const Reading& r, const std::vector<Reading>& all, bool pastCtx = false) const {
    const Features f = unpack(r.packed);
    double p = 0;
    bool coreCommon = false, properAdj = false;
    for (const Reading& o : all) {
      if (o.name || o.lemma == lex::kNoLemma) continue;
      const lex::Lemma ol = lx.lemma(o.lemma);
      if (ol.tier == 1 || ol.tier == 2) coreCommon = true;
      if (!ol.head.empty() && ol.head[0] >= 'A' && ol.head[0] <= 'Z') properAdj = true;
    }
    if (r.lemma != lex::kNoLemma) {
      const lex::Lemma l = lx.lemma(r.lemma);
      const uint8_t tier = r.name ? l.tier : cd.effectiveTier(l.key, l.pos, l.tier);
      p += tier == 1 ? 1.0 : tier == 2 ? 0.5 : 0.0;
      if (l.freqRank > 0) p += 0.8 * std::max(0.0, 1.0 - std::log10((double)l.freqRank) / 4.5);
      p += l.whitFreq == 'A' ? 0.3 : l.whitFreq == 'B' ? 0.2 : l.whitFreq == 'C' ? 0.1 : 0.0;
      if (!headIsWord(l.head)) p -= 3.0;
      if (r.aflags & lex::LateLatin) p -= 0.5;
      if (r.aflags & lex::PoeticRare) p -= 0.5;
      if (r.aflags & lex::AltSpelling) p -= 0.4;
      if (r.aflags & lex::FromTable) p += 0.1;
      if ((f.pos == Verb || f.pos == Participle) && f.voice == Passive && !(l.flags & lex::Deponent)) p -= 0.25;
      if (f.pos == Participle || (f.pos == Verb && f.mood == ParticipleMood)) p -= 0.4;
      // the same form read as the perfect and as the present of one lemma (venit / vēnit): present first, unless the
      // sentence is set in the past (another past-only verb, heri, ōlim)
      if (f.pos == Verb && f.tense == Perfect && !t.text.empty() && !hasLengthMark(t.text)) {
        for (const Reading& o : all) {
          const Features g = unpack(o.packed);
          if (o.lemma == r.lemma && g.pos == Verb && g.tense == Present && g.mood == f.mood) {
            // syncopated perfects spelled like the present (amāt) are rarer still
            const bool syncopated = r.display.size() > 3 && r.display.compare(r.display.size() - 3, 3, "\xC4\x81t") == 0;
            p -= syncopated ? 1.2 : pastCtx ? 0.0 : 0.5;
            break;
          }
        }
      }
      if (pastCtx && f.pos == Verb && f.tense == Present && f.mood == Indicative && !t.text.empty() &&
          !hasLengthMark(t.text)) {
        for (const Reading& o : all) {
          const Features g = unpack(o.packed);
          const bool syncopated = o.display.size() > 3 && o.display.compare(o.display.size() - 3, 3, "\xC4\x81t") == 0;
          if (o.lemma == r.lemma && g.pos == Verb && g.tense == Perfect && g.mood == f.mood && g.person == f.person &&
              g.number == f.number && !syncopated) {
            p -= 0.4;
            break;
          }
        }
      }
      // a pluperfect spelled without -erā- (pārat "syncopated" from paveō) is a present of another verb
      if (f.pos == Verb && f.tense == Pluperfect && f.mood == Indicative && !hasLengthMark(t.text)) {
        const std::string k = text::latin_key(t.text);
        bool er = false;
        for (const char* e : {"eram", "eras", "erat", "eramus", "eratis", "erant"}) {
          const size_t el = std::char_traits<char>::length(e);
          er = er || (k.size() > el && k.compare(k.size() - el, el, e) == 0);
        }
        if (!er) p -= 1.0;
      }
      // a form found only on a form page of this lemma while another lemma's own table has it (volat: volō "fly",
      // not volō "want"; edit: edō "eat", not ēdō)
      if (!(r.aflags & lex::FromTable)) {
        for (const Reading& o : all)
          if (o.lemma != r.lemma && o.lemma != lex::kNoLemma && (o.aflags & lex::FromTable) && o.packed == r.packed) {
            p -= 0.8;
            break;
          }
      }
      // homographs under several lemmas of one kind (verb / verb, noun / noun): the core lemma first (tier 1 before
      // tier 2 before tier 3); across kinds the clause decides (lacrimās: a verb where the clause needs one)
      if (!r.name && (l.pos == Verb || l.pos == Participle || l.pos == Noun || l.pos == Adj)) {
        auto kind = [&](uint32_t lm) {   // verbs with participles; nouns with adjectives (laetī: laetus adj, not the noun)
          const uint8_t k = lx.lemma(lm).pos;
          return k == Participle ? (uint8_t)Verb : k == Adj ? (uint8_t)Noun : k;
        };
        const uint8_t mine = readingTier(r), myKind = kind(r.lemma);
        uint8_t bestOther = 9;
        for (const Reading& o : all)
          if (o.lemma != r.lemma && o.lemma != lex::kNoLemma && !o.name && kind(o.lemma) == myKind)
            bestOther = std::min(bestOther, readingTier(o));
        if (bestOther <= 2 && mine >= 3) p -= 0.5;
        else if (bestOther == 1 && mine == 2) p -= 0.2;
      }
      // est / sunt / erat ... are sum (never edō "eat" or a noun)
      if (l.key != "sum") {
        const std::string k = text::latin_key(t.text);
        if (k == "est" || k == "estis" || k == "es" || k == "sunt" || k == "erat" || k == "erant") p -= 2.0;
      }
    } else if (r.name) {
      p += 0.6;
    }
    if (r.name) {
      if (!t.capitalised) p -= 2.5;
      else if (t.initial) p += coreCommon ? -1.0 : 0.8;
      else p += properAdj ? 0.5 : 1.5;
    } else if (t.capitalised && !t.initial && r.lemma != lex::kNoLemma) {
      const lex::Lemma l = lx.lemma(r.lemma);
      if (!(l.head.size() && l.head[0] >= 'A' && l.head[0] <= 'Z')) p -= 0.6;
    }
    if (!r.encl.empty() && t.encl.empty()) {
      // a split of a known word: only when the whole word is rare
      bool wholeCore = false;
      for (const Reading& o : all)
        if (o.encl.empty() && o.lemma != lex::kNoLemma) {
          const uint8_t ot = lx.lemma(o.lemma).tier;
          wholeCore = wholeCore || ot == 1 || ot == 2;
        }
      p -= wholeCore ? 1.5 : 0.2;
    }
    if (f.case_ == Voc) p -= 0.8;
    if (f.case_ == Loc) p -= 0.3;
    if (f.mood == Subjunctive) p -= 0.4;
    if (f.mood == Imperative) p -= 0.5;
    if (f.mood == Gerund || (f.extra & (Supine | Gerundive))) p -= 0.6;
    if (f.tense == FuturePerfect) p -= 0.5;
    if (r.fromRule) p -= 0.3;
    return p;
  }

  // ---- reading facts -----------------------------------------------------------------------------------------------
  void info(const Reading& r, RI& o) const {
    o = RI();
    o.encl = r.encl;
    o.f = unpack(r.packed);
    if (r.lemma != lex::kNoLemma) {
      const lex::Lemma l = lx.lemma(r.lemma);
      o.tier = l.tier;
      o.lpos = l.pos;
      o.lgender = l.gender;
      o.lflags = l.flags;
      o.key = std::string(l.key);
    } else {
      o.lpos = o.f.pos;
      o.key = text::latin_key(r.display);
    }
    const Features& f = o.f;
    const uint8_t pos = f.pos ? f.pos : o.lpos;
    o.finite = pos == Verb && (f.mood == Indicative || f.mood == Subjunctive || f.mood == Imperative) && f.person != 0 &&
               !(o.key == "quaeso" || o.key == "amabo");
    o.inf = pos == Verb && f.mood == Infinitive;
    o.partic = pos == Participle || (pos == Verb && f.mood == ParticipleMood);
    o.nominal = (f.case_ != 0 && (pos == Noun || pos == Adj || pos == Pron || pos == Det || pos == Num ||
                                  pos == Name || o.partic)) ||
                (r.name && f.case_ == 0);
    o.head = o.nominal && (pos == Noun || pos == Name || r.name ||
                           (pos == Pron && o.key != "meus" && o.key != "tuus" && o.key != "suus" && o.key != "noster" &&
                            o.key != "uester"));
    o.modifier = o.nominal && !o.head;
    if (pos == Pron && (o.key == "hic" || o.key == "ille" || o.key == "iste" || o.key == "ipse" || o.key == "is" ||
                        o.key == "idem" || o.key == "qui"))
      o.modifier = o.nominal;   // demonstratives agree with a noun when one is there
    o.prep = o.lpos == Prep;
    o.adv = o.lpos == Adv || o.lpos == Particle;
    o.conj = o.lpos == Conj;
    o.sub = o.conj && isSubKey(o.key);
    o.coord = o.conj && isCoordKey(o.key);
    o.rel = pos == Pron && o.key == "qui";
    o.copula = pos == Verb && o.key == "sum";
    o.intjO = o.key == "o" && (o.lpos == Intj || o.lpos == Particle);
    o.nolo = pos == Verb && o.key == "nolo" && f.mood == Imperative;
    o.voc = f.case_ == Voc;
    o.imper = o.finite && f.mood == Imperative;
    o.subj = o.finite && f.mood == Subjunctive;
    if (o.prep) {
      o.prepMask = cd.prepCases(o.key);
      if (!o.prepMask && r.lemma != lex::kNoLemma) {
        senseBuf.clear();
        lx.senses(r.lemma, senseBuf);
        for (const lex::Sense& s : senseBuf) {
          if (s.tags & TagAbl) o.prepMask |= (uint16_t)(1u << Abl);
          if (s.tags & TagAcc) o.prepMask |= (uint16_t)(1u << Acc);
          if (s.tags & TagGen) o.prepMask |= (uint16_t)(1u << Gen);
        }
      }
      if (!o.prepMask) o.prepMask = (uint16_t)((1u << Abl) | (1u << Acc));
    }
    if (pos == Noun && std::binary_search(personNouns.begin(), personNouns.end(), o.key)) o.personNoun = true;
    if (o.head && pos == Pron) {
      if (o.key == "ego" || o.key == "nos") o.person = 1;
      else if (o.key == "tu" || o.key == "uos") o.person = 2;
    }
    if ((o.finite || o.inf || o.partic) && r.lemma != lex::kNoLemma) {
      const uint32_t v = valency(r.lemma, o.key);
      o.accOK = v & 1;
      o.datOK = v & 2;
      o.intrOnly = v & 4;
      o.infOK = v & 8;
      o.accInf = v & 16;
      o.copula = o.copula || (v & 32);
      o.valKnown = v & 64;
      // verbs of giving / saying / showing take a dative; verbs of motion an accusative of place
      static const char* const kDat[] = {"do", "dico", "narro", "monstro", "ostendo", "trado", "mitto", "scribo", "reddo",
                                         "dono", "praebeo", "nuntio", "respondeo", "porto", "fero", "affero", "credo",
                                         "pareo", "placeo", "noceo", "faueo", "impero", "persuadeo", "ignosco", "studeo"};
      for (const char* k : kDat)
        if (o.key == k) o.datOK = true;
      static const char* const kMotion[] = {"eo", "uenio", "redeo", "curro", "ambulo", "nauigo", "festino", "propero",
                                            "fugio", "abeo", "adeo", "peruenio", "mitto", "duco", "porto", "proficiscor"};
      for (const char* k : kMotion)
        if (o.key == k) o.motion = true;
    }
    // perfect participle: a participle lemma in -us (not the future -ūrus), or a verb's perfect participle cell
    if (o.partic) {
      const bool usKey = o.key.size() > 2 && o.key.compare(o.key.size() - 2, 2, "us") == 0 &&
                         !(o.key.size() > 4 && o.key.compare(o.key.size() - 4, 4, "urus") == 0);
      o.perfPart = (pos == Verb && f.tense == Perfect) || (o.lpos == Participle && usKey);
    }
    o.placeAcc = f.case_ == Acc && (o.key == "domus" || o.key == "rus");
  }

  // ---- segmentation -----------------------------------------------------------------------------------------------------
  void segment(Sentence& s, const std::vector<std::vector<RI>>& ri, const std::vector<int>& pick,
               std::vector<int>& clauseOf) const {
    const size_t n = s.tokens.size();
    s.clauses.clear();
    clauseOf.assign(n, 0);
    auto R = [&](size_t i) -> const RI* {
      if (pick[i] < 0 || ri[i].empty()) return nullptr;
      return &ri[i][(size_t)pick[i]];
    };
    auto hasFiniteReading = [&](size_t i) {
      for (const RI& x : ri[i])
        if (x.finite) return true;
      return false;
    };
    const bool question = s.finalPunct == "?";
    Clause main;
    main.first = 0;
    s.clauses.push_back(main);
    std::vector<int> stack = {0};
    std::vector<char> verbSeen = {0};
    int clauseWords = 0;
    auto hasNominalReading = [&](int i) {
      if (i < 0) return false;
      for (const RI& x : ri[(size_t)i])
        if (x.nominal) return true;
      return false;
    };
    for (size_t i = 0; i < n; ++i) {
      const Token& t = s.tokens[i];
      int cur = stack.back();
      if (t.kind == TokKind::Punct) {
        clauseOf[i] = cur;
        // a comma / semicolon closes a subordinate clause whose verb was seen
        if ((t.text == "," || t.text == ";" || t.text == ":") && stack.size() > 1 && verbSeen.back()) {
          stack.pop_back();
          verbSeen.pop_back();
        }
        // a semicolon / colon after a complete main clause starts the next one
        if ((t.text == ";" || t.text == ":") && stack.size() == 1 && verbSeen.back() && i + 1 < n) {
          Clause c;
          c.first = (int)i + 1;
          c.marker = -1;
          c.parent = -1;
          c.kind = Clause::Coord;
          s.clauses.push_back(c);
          stack.back() = (int)s.clauses.size() - 1;
          verbSeen.back() = 0;
        }
        continue;
      }
      const RI* r = R(i);
      // opening a subordinate / relative clause
      bool open = false;
      Clause::Kind kind = Clause::Sub;
      if (r && r->sub) {
        open = true;
        if (r->key == "ubi" && question && i == 0) open = false;
        if (r->key == "cum" && !r->conj) open = false;
      } else if (r && r->key == "ubi" && !(question && i == 0) && r->lpos == Adv) {
        open = true;   // ubi as "when" / relative "where"
      } else if (r && r->rel && i > 0 && s.clauses[(size_t)cur].marker != (int)i) {
        // (not when the preposition before it already opened this relative clause: "in quā habitāmus")
        // relative pronoun after a noun (or after a comma that follows a noun)
        size_t j = i;
        while (j > 0 && s.tokens[j - 1].kind == TokKind::Punct && s.tokens[j - 1].text == ",") --j;
        if (j > 0 && R(j - 1) && R(j - 1)->nominal) { open = true; kind = Clause::Relative; }
        if (j > 0 && s.tokens[j - 1].kind == TokKind::Word && R(j - 1) && R(j - 1)->prep && j > 1 && R(j - 2) && R(j - 2)->nominal) {
          open = true;
          kind = Clause::Relative;
        }
      }
      if (r && r->prep && i + 1 < n && R(i + 1) && R(i + 1)->rel) {
        // "in quō": the preposition belongs to the relative clause
        size_t j = i;
        if (j > 0 && R(j - 1) && R(j - 1)->nominal) { open = true; kind = Clause::Relative; }
      }
      if (open) {
        Clause c;
        c.first = (int)i;
        c.marker = (int)i;
        c.parent = cur;
        c.kind = kind;
        if (r && r->prep && kind == Clause::Relative) c.marker = (int)i + 1;
        s.clauses.push_back(c);
        stack.push_back((int)s.clauses.size() - 1);
        verbSeen.push_back(0);
        cur = stack.back();
      } else if (r && (r->coord || r->encl == "que") && verbSeen.back() &&
                 (clauseWords >= 2 || !hasNominalReading(s.clauses[(size_t)cur].verb))) {
        // et / sed between two finite verbs: a coordinated clause at the same level
        bool laterVerb = false;
        for (size_t j = i + (r->coord ? 1 : 0); j < n; ++j) {
          if (s.tokens[j].kind == TokKind::Punct && s.tokens[j].text != ",") break;
          if (R(j) && R(j)->finite && !(R(j)->rel)) { laterVerb = true; break; }
          if (R(j) && (R(j)->sub || (R(j)->rel && j > i))) break;
        }
        if (laterVerb && !(r->encl == "que" && r->finite)) {
          Clause c;
          c.first = (int)i;
          c.marker = r->coord ? (int)i : -1;
          c.parent = s.clauses[(size_t)cur].parent;
          c.kind = Clause::Coord;
          s.clauses.push_back(c);
          stack.back() = (int)s.clauses.size() - 1;
          verbSeen.back() = 0;
          cur = stack.back();
        }
      }
      clauseOf[i] = cur;
      clauseWords = 0;
      for (size_t q = 0; q <= i; ++q)
        if (clauseOf[q] == cur && s.tokens[q].kind == TokKind::Word) ++clauseWords;
      if (r && r->finite) {
        if (!verbSeen.back()) s.clauses[(size_t)cur].verb = (int)i;
        verbSeen.back() = 1;
        // a subordinate clause ends with its verb (verb-final) unless more of it follows before a comma
        if (stack.size() > 1) {
          bool more = false;
          for (size_t j = i + 1; j < n; ++j) {
            if (s.tokens[j].kind == TokKind::Punct) break;
            const RI* q = R(j);
            if (!q) continue;
            // a following word that only this clause can take (an object of this verb before the next verb)
            if (q->finite || q->sub || q->rel || q->coord) break;
            bool laterVerb = false;
            for (size_t k = j + 1; k < n && s.tokens[k].kind != TokKind::Punct; ++k)
              if (R(k) && R(k)->finite) laterVerb = true;
            if (!laterVerb) { more = true; }
            break;
          }
          if (!more) {
            stack.pop_back();
            verbSeen.pop_back();
          }
        }
      }
      (void)hasFiniteReading;
    }
    // clause extents
    for (Clause& c : s.clauses) { c.first = (int)n; c.last = -1; }
    for (size_t i = 0; i < n; ++i) {
      Clause& c = s.clauses[(size_t)clauseOf[i]];
      c.first = std::min(c.first, (int)i);
      c.last = std::max(c.last, (int)i);
    }
    for (Clause& c : s.clauses)
      if (c.last < 0) c.first = 0;
  }

  // ---- scoring -----------------------------------------------------------------------------------------------------------
  struct Ctx {
    const Sentence* s = nullptr;
    const std::vector<std::vector<RI>>* ri = nullptr;
    const std::vector<std::vector<double>>* pri = nullptr;
    const std::vector<int>* clauseOf = nullptr;
    std::vector<int> lastOfClause;          // per clause: last token index
    std::vector<char> address;              // per token: set off as an address (vocative context)
    bool exclam = false, question = false, anyImper = false, anySecond = false;
    std::vector<char> coordBefore;          // token preceded by et/atque/-que joiner (coordination)
    std::vector<std::string> keyTok;        // latin key of the written token
  };

  static bool governedBy(const Ctx& c, const std::vector<uint8_t>& ch, size_t i, uint16_t& mask) {
    // token i is the first or second word after a preposition (chosen readings)
    const auto& ri = *c.ri;
    for (size_t back = 1; back <= 2 && back <= i; ++back) {
      const size_t j = i - back;
      if (c.s->tokens[j].kind != TokKind::Word || ri[j].empty()) return false;
      const RI& x = ri[j][ch[j]];
      if (x.prep) { mask = x.prepMask; return true; }
      if (!x.nominal) return false;
    }
    return false;
  }

  double pair(const Ctx& c, const std::vector<uint8_t>& ch, size_t j, const RI& a, size_t i, const RI& b,
              std::vector<std::string>* why) const {
    const auto& co = *c.clauseOf;
    double d = 0;
    const bool same = co[i] == co[j];
    const size_t dist = i - j;
    // 1. preposition fixes the case of the next word
    if (dist == 1 && a.prep) {
      if (b.nominal) {
        if (b.f.case_ == 0 || (a.prepMask & (1u << b.f.case_))) {
          d += 2.0;
          if (why) why->push_back("after " + a.key);
        } else {
          d -= 2.5;
        }
      } else if (!b.rel) {
        d -= 1.0;
      }
    }
    // 2. agreement inside the noun phrase
    if (dist <= 3 && (same || dist == 1) && a.nominal && b.nominal && (a.modifier || b.modifier)) {
      bool between = false;   // a verb or a preposition between them breaks the phrase
      for (size_t k = j + 1; k < i; ++k) {
        const auto& rk = (*c.ri)[k];
        if (c.s->tokens[k].kind == TokKind::Punct) between = true;
        else if (!rk.empty() && ((rk[ch[k]].finite) || rk[ch[k]].prep)) between = true;
      }
      if (!between) {
        const bool caseOk = a.f.case_ == b.f.case_ || a.f.case_ == 0 || b.f.case_ == 0;
        const bool numOk = a.f.number == b.f.number || a.f.number == 0 || b.f.number == 0;
        const uint8_t ga = a.f.gender ? a.f.gender : a.lgender, gb = b.f.gender ? b.f.gender : b.lgender;
        const bool genOk = genderCompat(ga, gb);
        if (caseOk && numOk && genOk) {
          d += dist == 1 ? 1.2 : dist == 2 ? 0.8 : 0.5;
          if (why) why->push_back("agrees with " + c.s->tokens[j].text);
        } else if (dist == 1 && caseOk && (a.head != b.head)) {
          d -= 0.6;
        }
      }
    }
    // 3. subject - verb; 4. valency
    if (same) {
      const RI* verb = a.finite ? &a : b.finite ? &b : nullptr;
      const RI* nom = (verb == &a) ? &b : (verb == &b) ? &a : nullptr;
      const size_t ni = verb == &a ? i : j;
      if (verb && nom && nom->head && !nom->rel) {
        uint16_t mask = 0;
        const bool gov = governedBy(c, ch, ni, mask) || (ni > 0 && (*c.ri)[ni - 1].size() && (*c.ri)[ni - 1][ch[ni - 1]].prep);
        if (nom->f.case_ == Nom || (nom->f.case_ == 0 && !gov)) {
          const bool numOk = nom->f.number == 0 || nom->f.number == verb->f.number;
          const bool persOk = nom->person == verb->f.person;
          if (verb->imper) {
            // an imperative takes no nominative subject (a vocative instead)
            if (nom->f.case_ == Nom && nom->person != 2) d -= 0.4;
          } else if (numOk && persOk) {
            d += 1.5;
            if (why && nom == &b) why->push_back("subject of " + c.s->tokens[j].text);
            if (why && nom == &a) why->push_back("verb of " + c.s->tokens[j].text);
          } else if (verb->f.number == Pl && nom->f.number == Sg && (c.coordBefore[ni] || coordinatedNext(c, ni))) {
            d += 1.5;
          } else if (verb->copula) {
            // a predicate nominative ("Quis es?", "amīcī sumus")
            d += (nom->f.number == verb->f.number && nom->person == 3 && verb->f.person != 3) ? 0.6 : -0.2;
          } else {
            d -= 1.5;
          }
        } else if (!gov && nom->placeAcc) {
          // domum / rūs: "home", "to the country" (an adverb of place, not an object)
          d += verb->motion ? 0.6 : verb->accOK ? 0.4 : 0.0;
        } else if (!gov && nom->f.case_ == Acc) {
          if (verb->accOK) { d += 0.8; if (why && nom == &b) why->push_back("object of " + c.s->tokens[j].text); }
          else if (verb->copula || verb->intrOnly) d -= 0.8;
          else if (!verb->valKnown) d += 0.2;
        } else if (!gov && nom->f.case_ == Dat) {
          if (verb->datOK) { d += 0.8; if (why && nom == &b) why->push_back("dative with " + c.s->tokens[j].text); }
          else if (verb->copula) d += 0.1;
          else d -= 0.3;
        } else if (!gov && nom->f.case_ == Abl) {
          d -= (nom->personNoun || (nom->lpos == Pron && isKeyOf(nom->key, {"ego", "tu", "nos", "uos", "is"}))) ? 0.5 : 0.1;
        } else if (!gov && nom->f.case_ == Loc) {
          d += 0.5;
        }
      }
      // infinitive objects / acc + inf
      const RI* infv = a.inf ? &a : b.inf ? &b : nullptr;
      const RI* other = infv == &a ? &b : infv == &b ? &a : nullptr;
      if (infv && other) {
        if (other->finite && (other->infOK || other->accInf || other->nolo)) {
          d += 1.0;
          if (why && other == &a) why->push_back("infinitive with " + c.s->tokens[j].text);
        }
        if (other->head && other->f.case_ == Acc && infv->accOK) d += 0.4;
      }
    }
    // an adjective next to a form of sum, agreeing in number: the predicate (laetī sunt), not a noun subject
    if (same && dist <= 2) {
      const RI* adj = (a.lpos == Adj && a.modifier && a.f.case_ == Nom) ? &a : (b.lpos == Adj && b.modifier && b.f.case_ == Nom) ? &b : nullptr;
      const RI* cop = adj == &a ? &b : adj == &b ? &a : nullptr;
      if (adj && cop && cop->finite && cop->key == "sum" && !cop->imper && adj->f.number == cop->f.number) d += 0.6;
    }
    // perfect participle + sum: one verb (amātus erat, ingressus est, secūtae sunt)
    if (same && dist <= 3) {
      const RI* part = a.perfPart ? &a : b.perfPart ? &b : nullptr;
      const RI* aux = part == &a ? &b : part == &b ? &a : nullptr;
      if (part && aux && aux->finite && !aux->imper && aux->key == "sum" && !part->adjToo &&
          (part->f.case_ == Nom || part->f.case_ == 0) && (part->f.number == 0 || part->f.number == aux->f.number)) {
        d += dist == 1 ? 1.8 : 1.2;
        if (why) why->push_back(part == &b ? "with " + c.s->tokens[j].text + ": one verb" : "with " + c.s->tokens[j].text + ": one verb");
      }
    }
    // coordinated nominals (X et Y) prefer one number
    if (dist == 2 && a.head && b.head && a.f.case_ == b.f.case_ && c.coordBefore[i] && a.f.number == b.f.number) d += 0.3;
    // ... and one case (gladium et scūtum: both objects)
    if (dist == 2 && a.head && b.head && a.f.case_ && a.f.case_ == b.f.case_ && c.coordBefore[i] && same) d += 1.2;
    // nōlī + infinitive
    if (a.nolo && b.inf) d += 1.0;
    // genitive attribute next to a noun
    if (dist == 1 && a.head && b.head && a.f.case_ != b.f.case_ && a.lpos == Noun && b.lpos == Noun) {
      if (b.f.case_ == Gen) d += 0.4;   // liber puerī
      else if (a.f.case_ == Gen) {      // puerī liber, unless the genitive already follows a noun
        bool afterNoun = false;
        if (j > 0 && !(*c.ri)[j - 1].empty()) {
          const RI& p = (*c.ri)[j - 1][ch[j - 1]];
          afterNoun = p.head && p.lpos == Noun && p.f.case_ != Gen;
        }
        if (!afterNoun) d += 0.25;
      }
    }
    // ō + vocative
    if (dist == 1 && a.intjO && b.voc) d += 2.2;
    return d;
  }

  static bool coordinatedNext(const Ctx& c, size_t i) {
    const size_t n = c.s->tokens.size();
    if (i + 1 < n && c.coordBefore[i + 1]) return true;
    if (i + 2 < n && c.s->tokens[i + 1].kind == TokKind::Word && c.coordBefore[i + 2]) return true;
    return false;
  }

  double unary(const Ctx& c, size_t i, const RI& r) const {
    double d = 0;
    if (r.voc && c.address[i]) d += 2.2;
    if (r.f.case_ == Nom && c.address[i] && (c.anyImper || c.anySecond)) d -= 0.3;
    if (r.imper) {
      const Clause& cl = c.s->clauses[(size_t)(*c.clauseOf)[i]];
      if (c.exclam) d += 0.6;
      if ((int)i == cl.first || (i > 0 && c.address[i - 1]) || (i > 1 && c.s->tokens[i - 1].kind == TokKind::Punct)) d += 0.4;
      if (c.question) d -= 0.8;
    }
    // interrogative quis/quid first in a question; relative quī after a noun
    if (c.question && i == 0 && (r.key == "quis" || r.key == "quid") && (r.f.case_ == Nom || r.f.case_ == Acc)) d += 0.8;
    // "quid faciam?" deliberative: a 1st person present subjunctive in a question
    if (c.question && r.subj && r.f.person == 1 && r.f.tense == Present) d += 0.6;
    return d;
  }

  double clauseTerm(const Ctx& c, const std::vector<uint8_t>& ch, int cl, std::vector<std::string>* /*why*/) const {
    const Sentence& s = *c.s;
    const auto& ri = *c.ri;
    const auto& co = *c.clauseOf;
    const Clause& C = s.clauses[(size_t)cl];
    double d = 0;
    int finite = 0, words = 0, finiteReadings = 0, lastWord = -1, lastFinite = -1, noms = 0;
    bool coord = false, copula = false, subj = false, inf = false, transVerb = false, accSeen = false;
    for (int i = C.first; i <= C.last; ++i) {
      if (co[(size_t)i] != cl || s.tokens[(size_t)i].kind != TokKind::Word || ri[(size_t)i].empty()) continue;
      ++words;
      lastWord = i;
      const RI& r = ri[(size_t)i][ch[(size_t)i]];
      for (const RI& x : ri[(size_t)i])
        if (x.finite && (x.tier == 1 || x.tier == 2)) { ++finiteReadings; break; }
      if (r.finite) {
        if (finite && (c.coordBefore[(size_t)i] || coord)) {
        } else {
          ++finite;
        }
        lastFinite = i;
        copula = copula || r.copula;
        subj = subj || r.subj;
      }
      if (r.inf) inf = true;
      if (r.coord) coord = true;
      if (r.head && r.f.case_ == Nom && !r.rel && !c.coordBefore[(size_t)i]) ++noms;
      if (r.finite && (r.accOK || !r.intrOnly) && !r.copula) transVerb = true;   // valency: not intransitive-only
      if (r.nominal && r.f.case_ == Acc && !r.placeAcc &&
          !(i > C.first && !ri[(size_t)(i - 1)].empty() && ri[(size_t)(i - 1)][ch[(size_t)(i - 1)]].prep))
        accSeen = true;
    }
    if (finite == 1) d += 1.0;
    else if (finite == 0 && words >= 2 && finiteReadings > 0) d -= 0.5;
    else if (finite >= 2) d -= 1.6 * (finite - 1);
    if (finite >= 1 && lastFinite == lastWord && words >= 2) d += 0.4;
    // two nominatives with a transitive verb and no object: the second is the object (Puer dōnum habet)
    if (noms > 1 && !copula) d -= (transVerb && !accSeen ? 1.5 : 0.6) * (noms - 1);
    // a genitive with no noun next to it to depend on
    for (int i = C.first; i <= C.last; ++i) {
      if (co[(size_t)i] != cl || s.tokens[(size_t)i].kind != TokKind::Word || ri[(size_t)i].empty()) continue;
      const RI& r = ri[(size_t)i][ch[(size_t)i]];
      if (!r.head || r.f.case_ != Gen || r.lpos == Pron) continue;
      bool host = false;
      for (int j : {i - 1, i + 1, i - 2, i + 2}) {
        if (j < C.first || j > C.last || co[(size_t)j] != cl || ri[(size_t)j].empty()) continue;
        const RI& h = ri[(size_t)j][ch[(size_t)j]];
        if (h.head && h.lpos == Noun && h.f.case_ != Gen) host = true;
      }
      if (!host) d -= 0.4;
    }
    // modifiers that agree with nothing (unless a predicate of the copula)
    for (int i = C.first; i <= C.last; ++i) {
      if (co[(size_t)i] != cl || s.tokens[(size_t)i].kind != TokKind::Word || ri[(size_t)i].empty()) continue;
      const RI& r = ri[(size_t)i][ch[(size_t)i]];
      if (!r.modifier || r.head || r.rel) continue;
      if (copula && r.f.case_ == Nom) continue;
      bool agrees = false;
      for (int j = std::max(C.first, i - 3); j <= std::min(C.last, i + 3); ++j) {
        if (j == i || co[(size_t)j] != cl || ri[(size_t)j].empty() || s.tokens[(size_t)j].kind != TokKind::Word) continue;
        const RI& h = ri[(size_t)j][ch[(size_t)j]];
        if (!h.nominal || (h.modifier && !h.head)) continue;
        bool blocked = false;   // a preposition or a verb between them: another phrase
        for (int k = std::min(i, j) + 1; k < std::max(i, j); ++k)
          if (!ri[(size_t)k].empty() && (ri[(size_t)k][ch[(size_t)k]].prep || ri[(size_t)k][ch[(size_t)k]].finite)) blocked = true;
        if (i > 0 && !ri[(size_t)(i - 1)].empty() && ri[(size_t)(i - 1)][ch[(size_t)(i - 1)]].prep && j < i) blocked = true;
        if (blocked) continue;
        const uint8_t ga = r.f.gender ? r.f.gender : r.lgender, gb = h.f.gender ? h.f.gender : h.lgender;
        if ((h.f.case_ == r.f.case_ || h.f.case_ == 0) && (h.f.number == r.f.number || h.f.number == 0) && genderCompat(ga, gb))
          agrees = true;
      }
      if (!agrees) d -= (i > C.first && !ri[(size_t)(i - 1)].empty() && ri[(size_t)(i - 1)][ch[(size_t)(i - 1)]].prep) ? 1.0 : 0.6;
    }
    // mood after the subordinator
    if (C.marker >= 0 && !ri[(size_t)C.marker].empty()) {
      const RI& m = ri[(size_t)C.marker][ch[(size_t)C.marker]];
      if ((m.key == "ut" || m.key == "ne") && m.conj) d += subj ? 1.0 : -0.3;
      if ((m.key == "quod" || m.key == "quia") && m.conj && finite && !subj) d += 0.3;
    }
    if (C.kind == Clause::Main && subj && !c.question) {
      // a main-clause subjunctive is rare except the hortative 1st plural ("eāmus")
      bool hortative = false;
      for (int i = C.first; i <= C.last; ++i)
        if (co[(size_t)i] == cl && !ri[(size_t)i].empty() && ri[(size_t)i][ch[(size_t)i]].subj &&
            ri[(size_t)i][ch[(size_t)i]].f.person == 1 && ri[(size_t)i][ch[(size_t)i]].f.number == Pl)
          hortative = true;
      if (!hortative) d -= 0.6;
    }
    if (inf && finite == 0 && words >= 2) d += 0.2;
    return d;
  }

  // ---- the search -------------------------------------------------------------------------------------------------------
  void search(Sentence& s, const std::vector<std::vector<RI>>& ri, const std::vector<std::vector<double>>& pri,
              const std::vector<int>& clauseOf, std::vector<Hyp>& finalBeam) const {
    const size_t n = s.tokens.size();
    Ctx c;
    c.s = &s;
    c.ri = &ri;
    c.pri = &pri;
    c.clauseOf = &clauseOf;
    c.lastOfClause.assign(s.clauses.size(), -1);
    for (size_t i = 0; i < n; ++i) c.lastOfClause[(size_t)clauseOf[i]] = (int)i;
    fillContext(s, ri, c);
    const size_t kBeam = 96;
    std::vector<Hyp> beam(1), next;
    beam[0].c.reserve(n);
    for (size_t i = 0; i < n; ++i) {
      next.clear();
      const size_t nr = ri[i].empty() ? 1 : ri[i].size();
      for (const Hyp& h : beam) {
        for (size_t k = 0; k < nr; ++k) {
          Hyp x;
          x.c = h.c;
          x.c.push_back((uint8_t)k);
          double d = 0;
          if (!ri[i].empty()) {
            const RI& b = ri[i][k];
            d += pri[i][k] + unary(c, i, b);
            for (size_t j = (i > 12 ? i - 12 : 0); j < i; ++j) {
              if (ri[j].empty()) continue;
              d += pair(c, x.c, j, ri[j][x.c[j]], i, b, nullptr);
            }
            if (b.rel) d += relative(c, x.c, i, b, nullptr);
          }
          for (size_t cl = 0; cl < s.clauses.size(); ++cl)
            if (c.lastOfClause[cl] == (int)i) d += clauseTerm(c, x.c, (int)cl, nullptr);
          x.s = h.s + d;
          next.push_back(std::move(x));
        }
      }
      std::stable_sort(next.begin(), next.end(), [](const Hyp& a, const Hyp& b) {
        if (a.s != b.s) return a.s > b.s;
        return a.c < b.c;
      });
      if (next.size() > kBeam) next.resize(kBeam);
      beam.swap(next);
    }
    finalBeam = beam;
  }

  double relative(const Ctx& c, const std::vector<uint8_t>& ch, size_t i, const RI& b, std::vector<std::string>* why) const {
    const auto& co = *c.clauseOf;
    for (size_t j = i; j-- > 0;) {
      if (co[j] == co[i]) continue;
      if (c.s->tokens[j].kind != TokKind::Word || (*c.ri)[j].empty()) continue;
      const RI& a = (*c.ri)[j][ch[j]];
      if (!a.head) continue;
      const uint8_t ga = a.f.gender ? a.f.gender : a.lgender;
      const bool ok = genderCompat(ga, b.f.gender) && (a.f.number == b.f.number || a.f.number == 0 || b.f.number == 0);
      if (ok && why) why->push_back("refers to " + c.s->tokens[j].text);
      return ok ? 1.2 : -1.2;
    }
    return 0;
  }

  void fillContext(const Sentence& s, const std::vector<std::vector<RI>>& ri, Ctx& c) const {
    const size_t n = s.tokens.size();
    c.exclam = s.finalPunct == "!";
    c.question = s.finalPunct == "?";
    c.address.assign(n, 0);
    c.coordBefore.assign(n, 0);
    for (size_t i = 0; i < n; ++i) {
      for (const RI& x : ri[i]) {
        if (x.imper) c.anyImper = true;
        if (x.finite && x.f.person == 2) c.anySecond = true;
        if (x.lpos == Pron && (x.key == "tu" || x.key == "uos")) c.anySecond = true;
      }
      if (s.tokens[i].encl == "que" || s.tokens[i].encl == "ue") c.coordBefore[i] = 1;
      for (const RI& x : ri[i])
        if (x.encl == "que" || x.encl == "ue") c.coordBefore[i] = 1;
      if (i > 0 && s.tokens[i - 1].kind == TokKind::Word && !ri[i - 1].empty()) {
        bool co = false;
        for (const RI& x : ri[i - 1])
          if (x.coord && (x.key == "et" || x.key == "atque" || x.key == "ac" || x.key == "aut" || x.key == "uel" ||
                          x.key == "neque" || x.key == "nec"))
            co = true;
        if (co) c.coordBefore[i] = 1;
      }
    }
    // address: a word (or two) set off by commas / sentence edges, or after ō
    for (size_t i = 0; i < n; ++i) {
      if (s.tokens[i].kind != TokKind::Word) continue;
      const bool startEdge = i == 0 || s.tokens[i - 1].kind == TokKind::Punct ||
                             (i > 0 && !ri[i - 1].empty() && std::any_of(ri[i - 1].begin(), ri[i - 1].end(), [](const RI& x) { return x.intjO; }));
      size_t e = i + 1;
      if (e < n && s.tokens[e].kind == TokKind::Word && !ri[e].empty() &&
          std::any_of(ri[e].begin(), ri[e].end(), [](const RI& x) { return x.modifier && x.voc; }))
        ++e;
      const bool endEdge = e >= n || (s.tokens[e].kind == TokKind::Punct && s.tokens[e].text != "-");
      bool afterO = i > 0 && !ri[i - 1].empty() && std::any_of(ri[i - 1].begin(), ri[i - 1].end(), [](const RI& x) { return x.intjO; });
      bool finalSetOff = false;
      if (i > 0 && s.tokens[i - 1].kind == TokKind::Punct && s.tokens[i - 1].text == "," && endEdge) {
        size_t k = e;
        while (k < n && s.tokens[k].kind == TokKind::Punct) ++k;
        bool ecce = false;   // "Ecce, puella!" presents, it does not address
        for (size_t q = 0; q < n; ++q) ecce = ecce || text::latin_key(s.tokens[q].text) == "ecce";
        if (k >= n && (!ecce || c.anyImper || c.anySecond))
          finalSetOff = c.question || s.tokens[i].capitalised ||
                        std::any_of(ri[i].begin(), ri[i].end(), [](const RI& x) { return x.personNoun; });
      }
      if ((startEdge && endEdge && (c.anyImper || c.anySecond || afterO)) || afterO || finalSetOff) {
        for (size_t k = i; k < e; ++k) c.address[k] = 1;
      }
    }
  }

  void run(Sentence& s, const std::vector<rules::GlossaryEntry>* glossary) const {
    const size_t n = s.tokens.size();
    // final punctuation
    for (size_t i = n; i-- > 0;) {
      if (s.tokens[i].kind != TokKind::Punct) break;
      const std::string& p = s.tokens[i].text;
      if (p == "?" || p == "!" || p == "." || p == "..." || p == ";") { s.finalPunct = p; break; }
    }
    bool first = true;
    for (size_t i = 0; i < n; ++i) {
      Token& t = s.tokens[i];
      if (t.kind != TokKind::Word) continue;
      t.initial = first;
      first = false;
    }
    for (size_t i = 0; i < n; ++i) {
      Token& t = s.tokens[i];
      if (t.kind == TokKind::Word) readings(t, s.macrons, glossary, s, i);
      if (t.kind == TokKind::Number) {
        Reading r;
        Features f;
        f.pos = Num;
        r.packed = pack(f);
        r.display = t.text;
        t.readings.push_back(r);
      }
    }
    // cum: the conjunction at a clause start in a sentence with two finite verbs, else the preposition
    {
      int finiteTokens = 0, strongFinite = 0;
      bool comma = false;
      for (const Token& t : s.tokens) {
        if (t.kind == TokKind::Punct && t.text == ",") comma = true;
        bool fin = false, other = false;
        for (const Reading& r : t.readings) {
          const Features f = unpack(r.packed);
          if (f.pos == Verb && (f.mood == Indicative || f.mood == Subjunctive) && f.person) fin = true;
          else other = true;
        }
        if (fin) ++finiteTokens;
        if (fin && !other) ++strongFinite;
      }
      for (size_t i = 0; i < n; ++i) {
        Token& t = s.tokens[i];
        if (text::latin_key(t.text) != "cum" || t.readings.empty()) continue;
        const bool clauseStart = t.initial || (i > 0 && s.tokens[i - 1].kind == TokKind::Punct);
        bool nextAbl = false;
        if (i + 1 < n)
          for (const Reading& r : s.tokens[i + 1].readings)
            if (unpack(r.packed).case_ == Abl || r.name) nextAbl = true;
        const bool conj = clauseStart && (!nextAbl ? finiteTokens >= 2 : (strongFinite >= 2 || (finiteTokens >= 2 && comma)));
        std::vector<Reading> keep;
        for (const Reading& r : t.readings) {
          const uint8_t p = lx.lemma(r.lemma).pos;
          if ((conj && p == Conj) || (!conj && p == Prep)) keep.push_back(r);
        }
        if (!keep.empty()) t.readings = keep;
      }
    }
    // quod: the conjunction unless a neuter singular noun precedes; quam: exclamative adverb first in an
    // exclamation, "than" after a comparative
    for (size_t i = 0; i < n; ++i) {
      Token& t = s.tokens[i];
      const std::string k = text::latin_key(t.text);
      if (t.readings.empty() || (k != "quod" && k != "quam")) continue;
      size_t p = i;
      while (p > 0 && s.tokens[p - 1].kind != TokKind::Word) --p;
      const bool afterPunct = i == 0 || s.tokens[i - 1].kind == TokKind::Punct;
      const Token* prev = (p > 0 && !afterPunct) ? &s.tokens[p - 1] : nullptr;
      if (i > 0 && s.tokens[i - 1].kind == TokKind::Word) prev = &s.tokens[i - 1];
      // "altior est quam": look one word further back past a form of sum
      if (k == "quam" && prev && i > 1 && text::latin_key(prev->text) == "est" && s.tokens[i - 2].kind == TokKind::Word)
        prev = &s.tokens[i - 2];
      std::vector<Reading> keep;
      if (k == "quod") {
        bool neuterNoun = false;
        if (prev)
          for (const Reading& r : prev->readings) {
            const Features f = unpack(r.packed);
            const uint8_t g = f.gender ? f.gender : lx.lemma(r.lemma).gender;
            if ((f.pos == Noun || f.pos == Pron) && f.number != Pl && (g == N || g == MN || g == FN || g == MFN)) neuterNoun = true;
          }
        if (!neuterNoun)
          for (const Reading& r : t.readings)
            if (lx.lemma(r.lemma).pos == Conj) keep.push_back(r);
      } else {
        bool comparative = false;
        if (prev)
          for (const Reading& r : prev->readings) {
            const std::string pk(lx.lemma(r.lemma).key);
            if (unpack(r.packed).degree == Comparative ||
                (unpack(r.packed).pos == Adj && pk.size() > 3 && pk.compare(pk.size() - 3, 3, "ior") == 0))
              comparative = true;
          }
        const bool exclamFirst = t.initial && s.finalPunct == "!";
        for (const Reading& r : t.readings) {
          const uint8_t lp = lx.lemma(r.lemma).pos;
          if (comparative && lp == Conj) keep.push_back(r);
          if (!comparative && exclamFirst && lp == Adv) keep.push_back(r);
        }
      }
      if (!keep.empty()) t.readings = keep;
    }
    for (size_t i = 0; i < n; ++i) {
      Token& t = s.tokens[i];
      if (t.readings.empty() || t.kind != TokKind::Word) continue;
      const std::string k = text::latin_key(t.text);
      std::vector<Reading> keep;
      if (s.finalPunct == "?" && t.initial &&
          (k == "quo" || k == "ubi" || k == "unde" || k == "cur" || k == "quando" || k == "quomodo" || k == "quare" || k == "qua")) {
        for (const Reading& r : t.readings)
          if (lx.lemma(r.lemma).pos == Adv) keep.push_back(r);
      }
      if (k == "domi" || k == "ruri" || k == "humi") {
        for (const Reading& r : t.readings)
          if (unpack(r.packed).case_ == Loc || lx.lemma(r.lemma).pos == Adv) keep.push_back(r);
      }
      if (!keep.empty()) t.readings = keep;
    }
    // past context: words that are past-only finite verbs (dormīvit, gaudēbat) or past adverbs (heri, ōlim)
    std::vector<char> pastWord(n, 0);
    for (size_t i = 0; i < n; ++i) {
      const Token& t = s.tokens[i];
      if (t.kind != TokKind::Word) continue;
      const std::string k = text::latin_key(t.text);
      if (k == "heri" || k == "olim" || k == "nuper" || k == "pridie") { pastWord[i] = 1; continue; }
      bool fin = false, nonPast = false;
      for (const Reading& r : t.readings) {
        const Features f = unpack(r.packed);
        if (f.pos != Verb || !f.person || (f.mood != Indicative && f.mood != Subjunctive)) continue;
        fin = true;
        if (f.tense != Perfect && f.tense != Imperfect && f.tense != Pluperfect) nonPast = true;
      }
      pastWord[i] = fin && !nonPast;
    }
    // priors, ordering, cap
    std::vector<std::vector<RI>> ri(n);
    std::vector<std::vector<double>> pri(n);
    for (size_t i = 0; i < n; ++i) {
      Token& t = s.tokens[i];
      bool pastCtx = false;
      for (size_t j = 0; j < n; ++j) pastCtx = pastCtx || (j != i && pastWord[j]);
      for (Reading& r : t.readings) r.prior = prior(t, r, t.readings, pastCtx);
      std::stable_sort(t.readings.begin(), t.readings.end(), [](const Reading& a, const Reading& b) {
        if (a.prior != b.prior) return a.prior > b.prior;
        if (a.lemma != b.lemma) return a.lemma < b.lemma;
        return a.packed < b.packed;
      });
      if (t.readings.size() > 14) t.readings.resize(14);
      ri[i].resize(t.readings.size());
      pri[i].resize(t.readings.size());
      bool adj = false;
      for (size_t k = 0; k < t.readings.size(); ++k) {
        info(t.readings[k], ri[i][k]);
        pri[i][k] = t.readings[k].prior;
        adj = adj || ri[i][k].lpos == Adj;
      }
      for (RI& x : ri[i]) x.adjToo = adj;
    }
    // segmentation <-> search until stable
    std::vector<int> pick(n, 0), clauseOf, prevClauses;
    for (size_t i = 0; i < n; ++i) pick[i] = s.tokens[i].readings.empty() ? -1 : 0;
    std::vector<Hyp> beam;
    for (int pass = 0; pass < 3; ++pass) {
      segment(s, ri, pick, clauseOf);
      if (pass > 0 && clauseOf == prevClauses) break;
      prevClauses = clauseOf;
      search(s, ri, pri, clauseOf, beam);
      if (beam.empty()) break;
      for (size_t i = 0; i < n; ++i) pick[i] = s.tokens[i].readings.empty() ? -1 : (int)beam[0].c[i];
    }
    if (beam.empty()) return;
    // confidence and alternatives from the final beam: best total per reading
    const double best = beam[0].s;
    for (size_t i = 0; i < n; ++i) {
      Token& t = s.tokens[i];
      t.clause = clauseOf[i];
      if (t.readings.empty()) continue;
      std::vector<double> sc(t.readings.size(), -1e9);
      for (const Hyp& h : beam) sc[h.c[i]] = std::max(sc[h.c[i]], h.s);
      for (size_t k = 0; k < t.readings.size(); ++k) t.readings[k].score = sc[k];
      const size_t chosen = beam[0].c[i];
      // order: chosen first, then by score, prior
      std::vector<size_t> order(t.readings.size());
      for (size_t k = 0; k < order.size(); ++k) order[k] = k;
      std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        if ((a == chosen) != (b == chosen)) return a == chosen;
        if (sc[a] != sc[b]) return sc[a] > sc[b];
        return t.readings[a].prior > t.readings[b].prior;
      });
      std::vector<Reading> sorted;
      std::vector<RI> sri;
      for (size_t k : order) { sorted.push_back(t.readings[k]); sri.push_back(ri[i][k]); }
      t.readings.swap(sorted);
      ri[i].swap(sri);
      if (!t.readings.empty()) t.encl = t.readings[0].encl;
      // surviving distinct readings within 1.5 of the best analysis
      double z = 0;
      int surv = 0;
      std::vector<std::pair<uint32_t, uint32_t>> seen;
      for (size_t k = 0; k < t.readings.size(); ++k) {
        const Reading& r = t.readings[k];
        if (r.score < best - 1.5) continue;
        const Features f = unpack(r.packed);
        Features g;
        g.case_ = f.case_; g.number = f.number; g.person = f.person; g.tense = f.tense; g.mood = f.mood; g.voice = f.voice;
        g.pos = f.pos;
        const std::pair<uint32_t, uint32_t> id{r.lemma, pack(g)};
        if (std::find(seen.begin(), seen.end(), id) != seen.end()) continue;
        seen.push_back(id);
        ++surv;
        z += std::exp(r.score - best);
      }
      t.surviving = std::max(1, surv);
      t.confidence = surv <= 1 ? 1.0 : std::max(0.05, std::min(1.0, 1.0 / std::max(1.0, z)));
      t.confidence = std::round(t.confidence * 1000) / 1000;
    }
    // explanations for the chosen analysis
    {
      Ctx c;
      c.s = &s;
      c.ri = &ri;
      c.pri = &pri;
      c.clauseOf = &clauseOf;
      fillContext(s, ri, c);
      std::vector<uint8_t> ch(n, 0);
      for (size_t i = 0; i < n; ++i) {
        if (ri[i].empty()) continue;
        std::vector<std::string> why;
        for (size_t j = (i > 12 ? i - 12 : 0); j < i; ++j)
          if (!ri[j].empty()) pair(c, ch, j, ri[j][0], i, ri[i][0], &why);
        if (ri[i][0].rel) relative(c, ch, i, ri[i][0], &why);
        for (std::string& w : why)
          if (std::find(s.tokens[i].why.begin(), s.tokens[i].why.end(), w) == s.tokens[i].why.end())
            s.tokens[i].why.push_back(w);
      }
    }
    // clause verbs from the final analysis
    for (Clause& cl : s.clauses) cl.verb = -1;
    for (size_t i = 0; i < n; ++i) {
      if (ri[i].empty() || !ri[i][0].finite) continue;
      Clause& cl = s.clauses[(size_t)clauseOf[i]];
      if (cl.verb < 0) cl.verb = (int)i;
    }
  }
};

Analyser::Analyser(const lex::Lexicon& la, const curated::CuratedData& cd) : impl_(std::make_unique<Impl>(la, cd)) {}
Analyser::~Analyser() = default;

void Analyser::setPersonNouns(std::vector<std::string> keys) {
  std::sort(keys.begin(), keys.end());
  impl_->personNouns = std::move(keys);
}

void Analyser::analyse(std::string_view sentence, Sentence& out, const std::vector<rules::GlossaryEntry>* glossary) const {
  out.clear();
  out.text = text::nfc(sentence);
  out.macrons = hasLengthMark(out.text);
  impl_->tokenise(out.text, out.tokens);
  impl_->run(out, glossary);
}

std::vector<std::pair<size_t, size_t>> splitSentences(std::string_view t) {
  std::vector<std::pair<size_t, size_t>> out;
  size_t start = 0;
  size_t i = 0;
  while (i < t.size()) {
    const char c = t[i];
    if (c == '.' || c == '!' || c == '?' || c == ';') {
      size_t j = i + 1;
      while (j < t.size() && (t[j] == '.' || t[j] == '!' || t[j] == '?' || t[j] == '"' || t[j] == '\'' || t[j] == ')'))
        ++j;
      // closing quotes (UTF-8 ” ’ »)
      while (j + 1 < t.size() && (unsigned char)t[j] == 0xE2 && (unsigned char)t[j + 1] == 0x80 && j + 2 < t.size() &&
             ((unsigned char)t[j + 2] == 0x9D || (unsigned char)t[j + 2] == 0x99))
        j += 3;
      if (c == ';') {
        // a semicolon ends a sentence only when a capital follows
        size_t k = j;
        while (k < t.size() && t[k] == ' ') ++k;
        if (k >= t.size() || !(t[k] >= 'A' && t[k] <= 'Z')) { i = j; continue; }
      }
      out.emplace_back(start, j);
      while (j < t.size() && (t[j] == ' ' || t[j] == '\n' || t[j] == '\r' || t[j] == '\t')) ++j;
      start = j;
      i = j;
      continue;
    }
    ++i;
  }
  size_t e = t.size();
  while (e > start && (t[e - 1] == ' ' || t[e - 1] == '\n' || t[e - 1] == '\r')) --e;
  if (e > start) out.emplace_back(start, e);
  return out;
}

}  // namespace vp::la2x
