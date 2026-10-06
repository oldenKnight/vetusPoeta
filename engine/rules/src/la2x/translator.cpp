// Translator (vp/la2x.h): lexical choice for the readable sentence (readable_*.tsv overrides, gloss_es_la.tsv, the
// lexicon's glosses cleaned to one plain word), names, the interlinear view, the cue-level output for the engine pairs
// la-en / la-es and the A9 round-trip overlap for the EN/ES -> LA engine.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <exception>
#include <sstream>
#include <unordered_map>

#include "internal.h"
#include "vp/morph.h"
#include "vp/realise_la.h"
#include "vp/text.h"

namespace vp::la2x {

using namespace vp::feat;
using detail::Lexical;
using detail::TokInfo;

namespace {

bool startsWith(const std::string& s, const char* p) { return s.compare(0, std::char_traits<char>::length(p), p) == 0; }

std::string trim(std::string s) {
  while (!s.empty() && (s.back() == ' ' || s.back() == '.' || s.back() == '!' || s.back() == '?')) s.pop_back();
  size_t k = 0;
  while (k < s.size() && s[k] == ' ') ++k;
  return s.substr(k);
}

// One plain word (or short phrase) from a dictionary gloss: first item before ';' and ',', no parentheses.
// Wiktionary residue in glosses: "majestad_", "grandeza₁₋₂"
std::string sanitise(std::string_view g) {
  std::string out;
  size_t i = 0;
  while (i < g.size()) {
    size_t j = i;
    const char32_t c = text::decodeUtf8(g, j);
    if (c == '_' || (c >= 0x2080 && c <= 0x209C)) { i = j; continue; }
    out.append(g.data() + i, j - i);
    i = j;
  }
  return out;
}

std::string firstItem(std::string_view g0) {
  const std::string gs = sanitise(g0);
  std::string_view g(gs);
  std::string s;
  int depth = 0;
  for (char c : g) {
    if (c == '(' || c == '[') { ++depth; continue; }
    if (c == ')' || c == ']') { if (depth) --depth; continue; }
    if (depth) continue;
    if (c == ';' || c == ',' || c == ':' || c == '/') break;
    s += c;
  }
  // collapse spaces
  std::string out;
  for (char c : s) {
    if (c == ' ' && (out.empty() || out.back() == ' ')) continue;
    out += c;
  }
  // "mouse or similar rodent" -> "mouse"
  for (const char* o : {" or ", " o "}) {
    const size_t at = out.find(o);
    if (at != std::string::npos && at > 0) out = out.substr(0, at);
  }
  return trim(out);
}

std::vector<std::string> items(std::string_view g0, size_t max) {
  const std::string gs = sanitise(g0);
  std::string_view g(gs);
  std::vector<std::string> out;
  std::string cur;
  int depth = 0;
  for (char c : g) {
    if (c == '(' || c == '[') { ++depth; continue; }
    if (c == ')' || c == ']') { if (depth) --depth; continue; }
    if (depth) continue;
    if (c == ';' || c == ',') {
      cur = trim(cur);
      if (!cur.empty()) out.push_back(cur);
      cur.clear();
      if (out.size() >= max) return out;
      continue;
    }
    cur += c;
  }
  cur = trim(cur);
  if (!cur.empty() && out.size() < max) out.push_back(cur);
  return out;
}

std::string jsonEscape(const std::string& s) {
  std::string o;
  for (char c : s) {
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if ((unsigned char)c < 0x20) o += ' ';
    else o += c;
  }
  return o;
}

const char* posName(uint8_t p) {
  switch (p) {
    case Noun: return "noun";
    case Verb: return "verb";
    case Adj: return "adj";
    case Adv: return "adv";
    case Participle: return "verb";
    case Pron: return "pron";
    case Num: return "adj";
    default: return "";
  }
}

}  // namespace

struct Translator::Impl final : public detail::LexicalSource {
  const lex::Lexicon& lx;
  const curated::CuratedData& cd;
  detail::Tables en, es;
  std::vector<detail::GlossEsRow> glossEs;   // sorted by key
  Analyser analyser;
  detail::Discourse disc;
  mutable std::unordered_map<uint32_t, uint32_t> partVerb;   // participle lemma -> verb lemma (capped)
  mutable std::vector<lex::Candidate> candBuf;
  mutable std::vector<lex::Analysis> anaBuf;
  mutable std::vector<lex::Sense> senseBuf;
  std::vector<std::string> personWordsEn;   // English words of "person" rows (gloss fallback)

  Impl(const lex::Lexicon& l, const curated::CuratedData& c) : lx(l), cd(c), analyser(l, c) {}

  // ---- participles ----
  uint32_t verbOfParticiple(uint32_t p) const {
    auto it = partVerb.find(p);
    if (it != partVerb.end()) return it->second;
    // the lexicon's own link first (the participle's headword as a participle analysis of its verb)
    uint32_t found = detail::verbOfParticipleLemma(lx, p, anaBuf);
    if (found != lex::kNoLemma) {
      if (partVerb.size() > 4096) partVerb.clear();
      partVerb.emplace(p, found);
      return found;
    }
    const lex::Lemma pl = lx.lemma(p);
    const std::string K(pl.key);
    std::string kw = firstItem(pl.glossEn);
    if (startsWith(kw, "to ")) kw = kw.substr(3);
    const size_t sp = kw.find(' ');
    if (sp != std::string::npos) kw = kw.substr(0, sp);
    candBuf.clear();
    if (!kw.empty()) lx.reverse(text::en_key(kw), candBuf);
    auto matches = [&](uint32_t v) {
      const lex::Lemma vl = lx.lemma(v);
      if (vl.pos != Verb) return false;
      const morph::Principal pr = morph::parsePrincipal(vl.head, vl.principal);
      const std::string sup = text::latin_key(pr.supine), inf = text::latin_key(pr.infinitive),
                        first = text::latin_key(pr.first.empty() ? std::string(vl.head) : pr.first);
      if (K.size() > 2 && K.compare(K.size() - 2, 2, "us") == 0) {
        const std::string st = K.substr(0, K.size() - 2);
        if (sup.size() > 2 && sup.compare(sup.size() - 2, 2, "um") == 0 && sup.substr(0, sup.size() - 2) == st) return true;
      }
      if (K.size() > 2 && K.compare(K.size() - 2, 2, "ns") == 0) {
        const std::string st = K.substr(0, K.size() - 2);
        if (inf.size() > 2 && inf.substr(0, inf.size() - 2) == st) return true;
        if (inf.size() > 2 && inf.substr(0, inf.size() - 2) + "e" == st) return true;
        if (first.size() > 1 && first.back() == 'o' && first.substr(0, first.size() - 1) + "e" == st) return true;
      }
      return false;
    };
    for (const lex::Candidate& c : candBuf)
      if (matches(c.lemma)) { found = c.lemma; break; }
    if (found == lex::kNoLemma) {
      // first conjugation by rule: amātus / amāns -> amō
      std::vector<std::string> guesses;
      if (K.size() > 4 && K.compare(K.size() - 4, 4, "atus") == 0) guesses.push_back(K.substr(0, K.size() - 4) + "o");
      if (K.size() > 3 && K.compare(K.size() - 3, 3, "ans") == 0) guesses.push_back(K.substr(0, K.size() - 3) + "o");
      if (K.size() > 4 && K.compare(K.size() - 4, 4, "itus") == 0) guesses.push_back(K.substr(0, K.size() - 4) + "eo");
      if (K.size() > 4 && K.compare(K.size() - 4, 4, "itus") == 0) guesses.push_back(K.substr(0, K.size() - 4) + "io");
      if (K.size() > 3 && K.compare(K.size() - 3, 3, "ens") == 0) guesses.push_back(K.substr(0, K.size() - 3) + "eo");
      if (K.size() > 4 && K.compare(K.size() - 4, 4, "iens") == 0) guesses.push_back(K.substr(0, K.size() - 4) + "io");
      for (const std::string& g : guesses) {
        const uint32_t v = morph::findLemma(lx, g, Verb);
        if (v != lex::kNoLemma && matches(v)) { found = v; break; }
      }
    }
    if (partVerb.size() > 4096) partVerb.clear();
    partVerb.emplace(p, found);
    return found;
  }

  // ---- glosses ----
  // cleaned lexicon gloss; follows "synonym of X" / "alternative form of X" one step
  std::string lexiconGloss(uint32_t lemma, uint8_t pos, Target t, bool* pivot, int depth = 0) const {
    const lex::Lemma l = lx.lemma(lemma);
    std::string g(t == Target::En ? l.glossEn : l.glossEs);
    if (pivot) *pivot = t == Target::Es && (l.flags & (1u << 8));
    for (const char* ref : {"synonym of ", "alternative form of ", "alternative spelling of ", "contraction of ",
                            "sinónimo de ", "forma alternativa de "}) {
      if (startsWith(g, ref) && depth < 2) {
        std::string w = firstItem(g.substr(std::char_traits<char>::length(ref)));
        const size_t sp = w.find(' ');
        if (sp != std::string::npos) w = w.substr(0, sp);
        const uint32_t r = morph::findLemma(lx, w, l.pos);
        if (r != lex::kNoLemma && r != lemma) return glossFor(r, pos, t, pivot, depth + 1);
      }
    }
    std::string w = firstItem(g);
    if (t == Target::En) {
      if (pos == Verb && startsWith(w, "to ")) w = w.substr(3);
      if (pos == Noun) {
        for (const char* a : {"a ", "an ", "the ", "A ", "An ", "The "})
          if (startsWith(w, a)) w = w.substr(std::char_traits<char>::length(a));
        if (!w.empty() && w[0] >= 'A' && w[0] <= 'Z' && w.size() > 1 && w[1] >= 'a' && w[1] <= 'z' && !(l.flags & lex::ProperName))
          w[0] = (char)(w[0] + 32);
      }
    } else {
      if (pos == Noun) {
        for (const char* a : {"el ", "la ", "los ", "las ", "un ", "una "})
          if (startsWith(w, a)) w = w.substr(std::char_traits<char>::length(a));
      }
    }
    if (w.size() > 60) w.clear();
    return w;
  }

  // A readable "lex" row is keyed by the Latin key, which two lemmas can share (volō "want" / volō "fly", occīdō "kill"
  // / occidō "fall"). For such a homograph the row applies only to the lemma whose English senses name the row's word;
  // the other lemma may have a "lexh" row (note = an English word of its senses: "lexh uolo verb fly fly").
  bool sensesName(uint32_t lemma, std::string w) const {
    if (startsWith(w, "be ") && w.size() > 3) w = w.substr(3);
    const size_t sp = w.find(' ');
    if (sp != std::string::npos) w = w.substr(0, sp);
    if (w.empty()) return false;
    auto has = [&](std::string_view g) {
      const std::string low = text::lower(std::string(g));
      size_t at = low.find(w);
      while (at != std::string::npos) {
        const bool b0 = at == 0 || !std::isalpha((unsigned char)low[at - 1]);
        const bool b1 = at + w.size() >= low.size() || !std::isalpha((unsigned char)low[at + w.size()]);
        if (b0 && b1) return true;
        at = low.find(w, at + 1);
      }
      return false;
    };
    if (has(lx.lemma(lemma).glossEn)) return true;
    std::vector<lex::Sense> senses;   // local: callers may be iterating senseBuf
    lx.senses(lemma, senses);
    for (const lex::Sense& se : senses)
      if (has(se.glossEn)) return true;
    return false;
  }
  mutable std::unordered_map<uint32_t, char> rowFit;   // lemma -> fits (capped)
  bool rowFits(uint32_t lemma, const std::string& key, uint8_t pos) const {
    auto it = rowFit.find(lemma);
    if (it != rowFit.end()) return it->second != 0;
    bool fits = true;
    const detail::Row* enRow = en.find("lex", key, posName(pos));
    if (enRow && !enRow->text.empty()) {
      std::vector<lex::Analysis> an;   // local: callers may be iterating anaBuf
      lx.lookup(key, an);
      bool other = false;
      const uint8_t lp = lx.lemma(lemma).pos;
      auto wordHead = [](std::string_view h) {   // "((caelum" and other malformed headwords do not count
        for (char ch : h)
          if (ch == '(' || ch == ')' || ch == '-' || ch == ' ' || (ch >= '0' && ch <= '9')) return false;
        return !h.empty();
      };
      for (const lex::Analysis& a : an) {
        const lex::Lemma ol = lx.lemma(a.lemma);
        if (a.lemma != lemma && ol.key == key && ol.pos == lp && wordHead(ol.head)) other = true;
      }
      if (other) fits = sensesName(lemma, enRow->text);
    }
    if (rowFit.size() > 4096) rowFit.clear();
    rowFit.emplace(lemma, fits ? 1 : 0);
    return fits;
  }
  const detail::Row* lexRow(const detail::Tables& tab, uint32_t lemma, const std::string& key, uint8_t pos) const {
    const detail::Row* r = tab.find("lex", key, posName(pos));
    if (!r || lemma == lex::kNoLemma || rowFits(lemma, key, pos)) return r;
    for (const detail::Row& h : tab.rows())
      if (h.kind == "lexh" && h.latin == key && h.feature == posName(pos) && sensesName(lemma, h.note)) return &h;
    return nullptr;
  }

  std::string glossFor(uint32_t lemma, uint8_t pos, Target t, bool* pivot, int depth = 0) const {
    const lex::Lemma l = lx.lemma(lemma);
    const std::string key(l.key);
    const detail::Tables& tab = t == Target::En ? en : es;
    if (pivot) *pivot = false;
    if (const detail::Row* r = lexRow(tab, lemma, key, pos ? pos : l.pos)) return r->text;
    if (t == Target::Es && rowFits(lemma, key, pos ? pos : l.pos)) {
      if (const detail::GlossEsRow* g = curatedEs(key, l.head)) return firstItem(g->gloss);
    }
    return lexiconGloss(lemma, pos ? pos : l.pos, t, pivot, depth);
  }

  const detail::GlossEsRow* curatedEs(const std::string& key, std::string_view head) const {
    auto it = std::lower_bound(glossEs.begin(), glossEs.end(), key,
                               [](const detail::GlossEsRow& a, const std::string& k) { return a.key < k; });
    const detail::GlossEsRow* first = nullptr;
    const std::string h = text::nfc(head);
    for (; it != glossEs.end() && it->key == key; ++it) {
      if (!first) first = &*it;
      if (it->head == h) return &*it;
    }
    return first;
  }

  // ---- LexicalSource ----
  Lexical lexical(const TokInfo& t, Target lang) const override {
    Lexical out;
    if (t.lemma == lex::kNoLemma) {
      out.word = t.text;
      out.missing = !t.name;
      return out;
    }
    uint32_t lemma = t.lemma;
    uint8_t pos = t.lpos;
    if (pos == Participle || t.f.pos == Participle) {
      // a participle used as an adjective with its own row (īrātus -> angry)
      const detail::Tables& tb = lang == Target::En ? en : es;
      const std::string pk(lx.lemma(t.lemma).key);
      if (const detail::Row* r = tb.find("lex", pk, "adj")) {
        out.word = r->text;
        out.adjective = true;
        return out;
      }
    }
    if (pos == Participle || t.f.pos == Participle) {
      const uint32_t v = verbOfParticiple(t.lemma);
      if (v != lex::kNoLemma) lemma = v;
      pos = Verb;
    }
    if (pos == Num) pos = Adj;
    if (pos == Det) pos = Adj;
    const lex::Lemma l = lx.lemma(lemma);
    const std::string key(l.key);
    const char* pn = posName(pos);
    const detail::Tables& tab = lang == Target::En ? en : es;
    const detail::Row* row = lexRow(tab, lemma, key, pos);
    const detail::Row* enRow = lexRow(en, lemma, key, pos);
    if (!row && pos == Adj && key.size() > 3 && key.compare(key.size() - 3, 3, "ior") == 0) {
      // a comparative lemma: the positive's word ("altior" -> altus -> tall; the realiser adds "-er")
      const std::string st = key.substr(0, key.size() - 3);
      for (const char* e : {"us", "is", "er", ""}) {   // "" : celer -> celerior
        if ((row = tab.find("lex", st + e, pn))) { enRow = en.find("lex", st + e, pn); break; }
      }
    }
    if (row) out.word = row->text;
    else {
      bool pivot = false;
      out.word = glossFor(lemma, pos, lang, &pivot);
      out.pivot = pivot;
      if (out.word.empty() && lang == Target::Es) {
        out.word = "[" + morph::displayForm(l.head, false) + "]";
        out.missing = true;
      } else if (out.word.empty()) {
        out.word = "[" + morph::displayForm(l.head, false) + "]";
        out.missing = true;
      }
    }
    auto tags = [&](const detail::Row* r) {
      if (!r) return;
      out.person = out.person || detail::noteHas(r->note, "person");
      out.mass = out.mass || detail::noteHas(r->note, "mass");
      out.unique = out.unique || detail::noteHas(r->note, "unique");
      out.event = out.event || detail::noteHas(r->note, "event");
      size_t at = r->note.find("time:");
      if (at != std::string::npos && out.timePrep.empty()) {
        size_t e = r->note.find(' ', at);
        out.timePrep = r->note.substr(at + 5, e == std::string::npos ? std::string::npos : e - at - 5);
      }
    };
    tags(row);
    if (lang == Target::Es && !row) {
      // semantics from the English row (person / mass / unique); the time preposition is language specific
      const std::string keep = out.timePrep;
      tags(enRow);
      out.timePrep = keep;
    }
    if (lang == Target::Es && row && !detail::noteHas(row->note, "person") && enRow) out.person = out.person || detail::noteHas(enRow->note, "person");
    if (!out.person && pos == Noun) {
      std::string enWord = enRow ? enRow->text : glossFor(lemma, pos, Target::En, nullptr);
      if (std::binary_search(personWordsEn.begin(), personWordsEn.end(), enWord)) out.person = true;
    }
    if (lang == Target::Es && pos == Noun) {
      const uint8_t lg = l.gender;
      if (out.person && (lg == F || lg == M)) {
        // the Spanish word's own ending wins when it is clear (poeta -> m via the table)
        out.gender = detail::es::nounGender(out.word, lg);
        if (out.word.size() > 1 && out.word.back() != 'a' && out.word.back() != 'o') out.gender = lg;
      } else {
        out.gender = detail::es::nounGender(out.word, lg);
      }
    }
    return out;
  }

  std::string nameIn(const TokInfo& t, Target lang) const override {
    std::string latin;
    if (t.lemma != lex::kNoLemma) {
      const lex::Lemma l = lx.lemma(t.lemma);
      const std::string g(lang == Target::En ? l.glossEn : l.glossEs);
      const std::string w = firstItem(g);
      // a gloss that is itself a name ("Rome", "Roma")
      if (!w.empty() && w.find(' ') == std::string::npos && w[0] >= 'A' && w[0] <= 'Z') return w;
      latin = morph::displayForm(l.head, false);
    } else {
      const curated::NameEntry* ne = cd.nameByLatin(text::latin_key(t.text));
      if (!ne) {
        // an oblique form of a names_la.tsv name: find the entry whose nominative starts like the form
        for (const curated::NameEntry& n : cd.names()) {
          const std::string nk = text::latin_key(n.latinNom);
          const std::string tk = text::latin_key(t.text);
          if (nk.size() > 2 && tk.size() > 2 && nk.compare(0, nk.size() - 2, tk, 0, nk.size() - 2) == 0 &&
              tk.size() <= nk.size() + 2) { ne = &n; break; }
        }
      }
      latin = ne ? text::display_latin(ne->latinNom, false) : text::display_latin(t.text, false);
    }
    const detail::Tables& tab = lang == Target::En ? en : es;
    const std::string key = text::latin_key(latin);
    if (const detail::Row* r = tab.find("name", key, "-")) return r->text;
    std::string out = latin;
    // initial consonantal I -> J (Iūlia -> Julia)
    if (out.size() > 1 && out[0] == 'I' && std::string("aeiou").find(out[1]) != std::string::npos) out[0] = 'J';
    if (lang == Target::Es) {
      if (out.size() > 3 && out.compare(out.size() - 2, 2, "us") == 0) out = out.substr(0, out.size() - 2) + "o";
    }
    return out;
  }

  // ---- interlinear ----
  static std::string featureText(const rules::Features& f) {
    std::string pos = f.pos == "adj" ? "adjective" : f.pos == "adv" ? "adverb" : f.pos == "pron" ? "pronoun"
                     : f.pos == "prep" ? "preposition" : f.pos == "conj" ? "conjunction" : f.pos == "intj" ? "interjection"
                     : f.pos == "det" ? "determiner" : f.pos == "num" ? "numeral" : f.pos;
    std::string rest;
    auto add = [&](const std::string& x) {
      if (x.empty()) return;
      if (!rest.empty()) rest += ' ';
      rest += x;
    };
    if (!f.person.empty()) add(f.person + " person");
    if (!f.mood.empty() && f.mood != "infinitive" && f.mood != "participle") {
      add(f.number);
      add(f.tense);
      add(f.mood);
      add(f.voice);
    } else {
      add(f.tense);
      add(f.mood);
      add(f.voice);
      add(f.case_);
      add(f.number);
      add(f.gender);
    }
    if (f.mood.empty()) {
      rest.clear();
      add(f.case_);
      add(f.number);
      add(f.gender);
    }
    if (!f.degree.empty() && f.degree != "positive") add(f.degree);
    if (rest.empty()) return pos;
    if (pos.empty()) return rest;
    return pos + ", " + rest;
  }

  std::string interGloss(uint32_t lemma, uint8_t pos, Target t, bool& pivot) const {
    pivot = false;
    if (lemma == lex::kNoLemma) return std::string();
    const lex::Lemma l = lx.lemma(lemma);
    const std::string key(l.key);
    uint8_t p = pos;
    if (p == Participle) {
      const uint32_t v = verbOfParticiple(lemma);
      if (v != lex::kNoLemma) return interGloss(v, Verb, t, pivot);
    }
    const detail::Tables& tab = t == Target::En ? en : es;
    if (t == Target::Es)
      if (rowFits(lemma, key, p ? p : l.pos))
        if (const detail::GlossEsRow* g = curatedEs(key, l.head)) return g->gloss;
    if (const detail::Row* r = lexRow(tab, lemma, key, p ? p : l.pos)) return r->text;
    // closed classes: the readable table's word
    for (const char* kind : {"prep", "conj", "adv", "intj", "det", "sub"}) {
      if (const detail::Row* r = tab.best(kind, key, {"abl", "acc", "sg", "-", "ind"})) {
        if (!r->text.empty()) return r->text;
      }
    }
    std::string g(t == Target::En ? l.glossEn : l.glossEs);
    pivot = t == Target::Es && (l.flags & (1u << 8));
    std::vector<std::string> it = items(g, 3);
    std::string out;
    for (const std::string& x : it) {
      if (!out.empty()) out += ", ";
      out += x;
    }
    if (out.empty() && t == Target::Es) {
      out = interGloss(lemma, pos, Target::En, pivot);
      if (!out.empty()) pivot = true;
    }
    return out;
  }

  void words(const Sentence& s, const std::vector<TokInfo>& ti, const detail::Built& b, Target t, std::vector<Word>& out) const {
    out.clear();
    for (size_t i = 0; i < s.tokens.size(); ++i) {
      const Token& tk = s.tokens[i];
      if (tk.kind == TokKind::Punct) continue;
      Word w;
      w.token = (int)i;
      w.text = tk.text;
      w.start = tk.start;
      w.end = tk.end;
      w.unknown = tk.unknown;
      w.nameGuess = tk.nameGuess;
      w.fromRule = tk.fromRule;
      w.confidence = tk.confidence;
      w.role = i < b.roles.size() ? b.roles[i] : std::string();
      const Reading* r = tk.best();
      if (r) {
        w.display = morph::displayForm(r->display.empty() ? tk.text : r->display, true);
        if (tk.capitalised && !w.display.empty()) w.display = detail::capitaliseFirst(w.display);
        w.lemma = r->lemma;
        w.name = r->name;
        w.features = realise::featureView(r->packed);
        w.featureText = featureText(w.features);
        if (r->lemma != lex::kNoLemma) {
          const lex::Lemma l = lx.lemma(r->lemma);
          w.head = morph::displayForm(l.head, true);
          w.tier = cd.effectiveTier(l.key, l.pos, l.tier);
          w.emoji = std::string(l.emoji);
          if (w.emoji.empty()) if (const curated::EmojiEntry* e = cd.emoji(l.key)) w.emoji = e->emoji;
          if (r->name) w.gloss = nameIn(ti[i], t);
          else w.gloss = interGloss(r->lemma, l.pos, t, w.glossPivot);
        } else {
          w.head = nameIn(ti[i], Target::En);
          if (r->name) w.gloss = nameIn(ti[i], t);
          if (tk.kind == TokKind::Number) { w.head = tk.text; w.gloss = tk.text; }
        }
        // the enclitic in words
        if (tk.encl == "que") w.gloss += t == Target::En ? " (+ and)" : " (+ y)";
        else if (tk.encl == "ne") w.gloss += t == Target::En ? " (+ question)" : " (+ pregunta)";
        else if (tk.encl == "ue") w.gloss += t == Target::En ? " (+ or)" : " (+ o)";
        // other surviving readings
        for (size_t k = 1; k < tk.readings.size() && w.alternatives.size() < 4; ++k) {
          const Reading& a = tk.readings[k];
          if (a.score < r->score - 1.5 || a.score < -1e8) continue;
          const rules::Features fv = realise::featureView(a.packed);
          std::string head = a.lemma != lex::kNoLemma ? morph::displayForm(lx.lemma(a.lemma).head, true) : a.display;
          std::string d = morph::displayForm(a.display, true) + " (" + head + ", " + featureText(fv) + ")";
          if (std::find(w.alternatives.begin(), w.alternatives.end(), d) == w.alternatives.end()) w.alternatives.push_back(d);
        }
      } else {
        w.display = tk.text;
        w.head = tk.text;
      }
      out.push_back(std::move(w));
    }
    // periphrases (participle + sum = one verb): both words carry the note
    for (const detail::Periphrasis& pp : b.periphrases) {
      const std::string note = periphrasisNote(s, ti, pp, t);
      for (Word& w : out)
        if (w.token == pp.participle || w.token == pp.aux) w.note = note;
    }
  }

  // "amātus erat = had been loved (one verb: pluperfect passive of amō)" / "secūta est = siguió (un solo verbo:
  // perfecto del deponente sequor)"
  std::string periphrasisNote(const Sentence& s, const std::vector<TokInfo>& ti, const detail::Periphrasis& pp,
                              Target t) const {
    if (pp.participle < 0 || pp.aux < 0 || (size_t)pp.participle >= ti.size() || (size_t)pp.aux >= ti.size()) return {};
    const TokInfo& part = ti[(size_t)pp.participle];
    const TokInfo& aux = ti[(size_t)pp.aux];
    const uint32_t verbL = part.verbLemma != lex::kNoLemma ? part.verbLemma : part.lemma;
    if (verbL == lex::kNoLemma) return {};
    const std::string base = lexical(part, t).word;
    const int person = aux.f.person ? aux.f.person : 3, number = aux.f.number == Pl ? 2 : 1;
    const int tense = pp.auxTense;
    std::string form;
    if (t == Target::En) {
      using detail::en::VForm;
      const std::string ppart = detail::en::verb(base, VForm::PastPart);
      if (pp.deponent) form = tense == Imperfect ? "had " + ppart : tense == Future ? "will have " + ppart
                                                                                    : detail::en::verb(base, VForm::Past);
      else form = tense == Imperfect ? "had been " + ppart : tense == Future ? "will have been " + ppart
                                                                             : (number == 2 ? "were " : person == 2 ? "were " : "was ") + ppart;
    } else {
      using detail::es::VTense;
      const std::string inf = detail::es::unreflexive(base);
      const uint8_t g = part.f.gender ? part.f.gender : (uint8_t)M;
      const std::string ppart = detail::es::verb(inf, VTense::PastPart, 3, 1);
      if (pp.deponent) form = tense == Imperfect ? detail::es::verb("haber", VTense::Imperfect, person, number) + " " + ppart
                            : tense == Future ? detail::es::verb("haber", VTense::Future, person, number) + " " + ppart
                                              : detail::es::verb(inf, VTense::Preterite, person, number);
      else {
        const std::string agr = detail::es::adjective(ppart, g == F ? (uint8_t)F : (uint8_t)M, (uint8_t)number);
        form = tense == Imperfect ? detail::es::verb("haber", VTense::Imperfect, person, number) + " sido " + agr
             : tense == Future ? detail::es::verb("haber", VTense::Future, person, number) + " sido " + agr
                               : detail::es::verb("ser", VTense::Preterite, person, number) + " " + agr;
      }
    }
    const std::string words = s.tokens[(size_t)pp.participle].text + " " + s.tokens[(size_t)pp.aux].text;
    const std::string head = morph::displayForm(lx.lemma(verbL).head, true);
    std::string what;
    if (t == Target::En) {
      what = tense == Imperfect ? "pluperfect" : tense == Future ? "future perfect" : "perfect";
      what = pp.deponent ? what + " of the deponent " + head : what + " passive of " + head;
      return words + " = " + form + " (one verb: " + what + ")";
    }
    what = tense == Imperfect ? "pluscuamperfecto" : tense == Future ? "futuro perfecto" : "perfecto";
    what = pp.deponent ? what + " del deponente " + head : what + " pasivo de " + head;
    return words + " = " + form + " (un solo verbo: " + what + ")";
  }

  void sentence(std::string_view latin, Target t, SentenceOut& out, const std::vector<rules::GlossaryEntry>* glossary) {
    out = SentenceOut();
    out.latin = std::string(latin);
    analyser.analyse(latin, out.analysis, glossary);
    std::vector<TokInfo> ti;
    detail::tokInfos(lx, out.analysis, ti);
    detail::Built b;
    detail::buildFrames(lx, t == Target::En ? en : es, out.analysis, ti, b);
    detail::RealiseIn in;
    in.s = &out.analysis;
    in.ti = &ti;
    in.tab = t == Target::En ? &en : &es;
    in.lex = this;
    in.disc = &disc;
    out.flags = b.flags;
    out.text = t == Target::En ? detail::realiseEnglish(b, in, out.flags) : detail::realiseSpanish(b, in, out.flags);
    out.frame = b.frames.empty() ? std::string() : frame::describe(b.frames[0]);
    words(out.analysis, ti, b, t, out.words);
    for (const Word& w : out.words)   // the periphrasis note also explains the reading ("Why this reading?")
      if (!w.note.empty() && w.token >= 0) {
        std::vector<std::string>& why = out.analysis.tokens[(size_t)w.token].why;
        if (std::find(why.begin(), why.end(), w.note) == why.end()) why.push_back(w.note);
      }
    // confidence: token confidences x frame fill x penalties
    double c = 1.0;
    size_t n = 0;
    bool unknown = false, guess = false, ambiguous = false;
    for (const Word& w : out.words) {
      c *= std::max(0.3, w.confidence);
      ++n;
      unknown = unknown || w.unknown;
      guess = guess || w.nameGuess;
      ambiguous = ambiguous || w.confidence < 0.6;
    }
    (void)n;
    c *= std::max(0.3, b.fill);
    auto flag = [&](const char* f) {
      if (std::find(out.flags.begin(), out.flags.end(), f) == out.flags.end()) out.flags.push_back(f);
    };
    if (unknown) { c *= 0.5; flag("unknown"); }
    if (guess) { c *= 0.8; flag("name-guessed"); }
    if (ambiguous) flag("ambiguous");
    if (std::find(out.flags.begin(), out.flags.end(), "abl-abs") != out.flags.end()) c *= 0.8;
    if (std::find(out.flags.begin(), out.flags.end(), "gloss-missing") != out.flags.end()) c *= 0.8;
    bool verb = false;
    for (const detail::TokInfo& x : ti) verb = verb || (x.f.pos == Verb);
    if (!verb && out.words.size() > 3) { c *= 0.8; flag("no-verb"); }
    out.confidence = std::round(std::max(0.0, std::min(1.0, c)) * 1000) / 1000;
  }

  // ---- A9 ----
  static bool stopEn(const std::string& w) {
    static const char* const k[] = {"a", "about", "all", "an", "and", "any", "are", "as", "at", "be", "been", "but",
                                    "by", "can", "could", "did", "do", "does", "for", "from", "get", "go", "have", "he",
                                    "her", "here", "him", "his", "i", "if", "in", "into", "is", "it", "its", "just", "me",
                                    "my", "no", "not", "now", "of", "off", "oh", "on", "or", "our", "out", "please",
                                    "she", "so", "some", "than", "that", "the", "their", "them", "then", "there",
                                    "these", "they", "this", "those", "to", "too", "up", "us", "very", "was", "we",
                                    "were", "what", "when", "where", "which", "who", "why", "will", "with", "would",
                                    "you", "your", "yes", "how", "shall", "should", "may", "might", "must", "let",
                                    "dear", "o", "well", "everyone", "everybody", "one", "ones", "much", "many"};
    for (const char* x : k)
      if (w == x) return true;
    return false;
  }
  static bool stopEs(const std::string& w) {
    static const char* const k[] = {"a", "al", "de", "del", "el", "la", "los", "las", "un", "una", "unos", "unas", "y",
                                    "o", "que", "en", "con", "por", "para", "no", "sí", "si", "ser", "estar", "haber",
                                    "yo", "tú", "él", "ella", "nosotros", "ustedes", "ellos", "ellas", "me", "te", "se",
                                    "nos", "le", "les", "lo", "mi", "tu", "su", "muy", "ya", "más", "pero", "como",
                                    "este", "esta", "ese", "esa", "eso", "esto", "qué", "quién", "dónde", "cuándo",
                                    "cómo", "por favor", "usted", "ir", "hacer", "tener", "todo", "todos", "uno"};
    for (const char* x : k)
      if (w == x) return true;
    return false;
  }
  static void wordsOf(const std::string& s, std::vector<std::string>& out) {
    std::string cur;
    auto flush = [&]() {
      if (!cur.empty()) out.push_back(cur);
      cur.clear();
    };
    const std::string low = text::lower(s);
    for (size_t i = 0; i < low.size(); ++i) {
      const unsigned char c = (unsigned char)low[i];
      if ((c >= 'a' && c <= 'z') || c >= 0x80 || c == '\'') cur += (char)c;
      else flush();
    }
    flush();
  }

  double overlap(std::string_view latinText, const std::vector<std::string>& src, Target lang) {
    std::vector<std::string> content;
    for (const std::string& w0 : src) {
      const std::string w = text::lower(w0);
      if (w.empty() || (lang == Target::En ? stopEn(w) : stopEs(w))) continue;
      if (std::find(content.begin(), content.end(), w) == content.end()) content.push_back(w);
    }
    if (content.empty()) return 1.0;
    // the Latin side: every reading-chosen lemma's gloss words (all senses' first items + keywords)
    std::vector<std::string> bag, latinKeys;
    for (const auto& r : splitSentences(latinText)) {
      Sentence s;
      analyser.analyse(latinText.substr(r.first, r.second - r.first), s, nullptr);
      for (const Token& t : s.tokens) {
        const Reading* rd = t.best();
        if (!rd) continue;
        latinKeys.push_back(text::latin_key(t.text));
        if (rd->lemma == lex::kNoLemma) continue;
        uint32_t lemma = rd->lemma;
        const lex::Lemma l0 = lx.lemma(lemma);
        if (l0.pos == Participle) {
          const uint32_t v = verbOfParticiple(lemma);
          if (v != lex::kNoLemma) lemma = v;
        }
        const lex::Lemma l = lx.lemma(lemma);
        const detail::Tables& tab = lang == Target::En ? en : es;
        for (const detail::Row& row : tab.rows())
          if (row.latin == l.key && (row.kind == "lex" || row.kind == "adv" || row.kind == "det" || row.kind == "prep" ||
                                     row.kind == "intj" || row.kind == "wh"))
            wordsOf(row.text, bag);
        wordsOf(std::string(lang == Target::En ? l.glossEn : l.glossEs), bag);
        if (lang == Target::Es)
          if (const detail::GlossEsRow* g = curatedEs(std::string(l.key), l.head)) wordsOf(g->gloss, bag);
        senseBuf.clear();
        lx.senses(lemma, senseBuf);
        for (size_t k = 0; k < senseBuf.size() && k < 4; ++k) {
          wordsOf(std::string(lang == Target::En ? senseBuf[k].glossEn : senseBuf[k].glossEs), bag);
          if (lang == Target::En) wordsOf(std::string(senseBuf[k].keywords), bag);
        }
      }
    }
    std::sort(bag.begin(), bag.end());
    bag.erase(std::unique(bag.begin(), bag.end()), bag.end());
    auto has = [&](const std::string& w) { return std::binary_search(bag.begin(), bag.end(), w); };
    size_t hit = 0;
    for (const std::string& w : content) {
      bool ok = has(w);
      // simple inflection slack: plural / 3sg / past / -ing of the source lemma in the gloss words
      if (!ok && lang == Target::En)
        for (const char* suf : {"s", "es", "ed", "d", "ing"}) ok = ok || has(w + suf);
      if (!ok && w.size() > 3 && lang == Target::En && w.back() == 'e') ok = has(w.substr(0, w.size() - 1) + "ing");
      if (!ok && lang == Target::Es && w.size() > 2) ok = has(w + "s") || has(w + "es");
      // names: the Latin form of an English name (names_la.tsv) or the same spelling
      if (!ok) {
        if (const curated::NameEntry* ne = cd.nameByEnglish(w)) {
          const std::string k = text::latin_key(ne->latinNom);
          const std::string stem = k.size() > 3 ? k.substr(0, k.size() - 2) : k;
          for (const std::string& lk : latinKeys) ok = ok || lk.compare(0, stem.size(), stem) == 0;
        }
        for (const std::string& lk : latinKeys) ok = ok || lk == text::latin_key(w);
      }
      if (ok) ++hit;
    }
    return (double)hit / (double)content.size();
  }
};

Translator::Translator(Key) {}
Translator::~Translator() = default;

Result<std::unique_ptr<Translator>> Translator::create(const lex::Lexicon& la, const curated::CuratedData& cd,
                                                       const std::vector<std::filesystem::path>& dirs) {
  try {
    std::unique_ptr<Translator> t = std::make_unique<Translator>(Key{});
    t->impl_ = std::make_unique<Impl>(la, cd);
    std::string err;
    for (const std::filesystem::path& d : dirs) {
      std::error_code ec;
      if (d.empty() || !std::filesystem::exists(d / "readable_en.tsv", ec)) continue;
      if (!t->impl_->en.load(d / "readable_en.tsv", err) || !t->impl_->es.load(d / "readable_es.tsv", err))
        return Result<std::unique_ptr<Translator>>(ErrorCode::Io, err, "The Latin reading tables could not be read.");
      detail::loadGlossEs(d / "gloss_es_la.tsv", t->impl_->glossEs);
      std::stable_sort(t->impl_->glossEs.begin(), t->impl_->glossEs.end(),
                       [](const detail::GlossEsRow& a, const detail::GlossEsRow& b) { return a.key < b.key; });
      for (const detail::Row& r : t->impl_->en.rows())
        if (r.kind == "lex" && detail::noteHas(r.note, "person")) t->impl_->personWordsEn.push_back(r.text);
      std::sort(t->impl_->personWordsEn.begin(), t->impl_->personWordsEn.end());
      std::vector<std::string> keys;
      for (const detail::Row& r : t->impl_->en.rows())
        if (r.kind == "lex" && detail::noteHas(r.note, "person")) keys.push_back(r.latin);
      t->impl_->analyser.setPersonNouns(std::move(keys));
      return Result<std::unique_ptr<Translator>>(std::move(t));
    }
    return Result<std::unique_ptr<Translator>>(ErrorCode::NotFound, "readable_en.tsv not found",
                                               "The Latin reading tables (data/curated/readable_en.tsv) are missing.");
  } catch (const std::exception& e) {
    return Result<std::unique_ptr<Translator>>(ErrorCode::Internal, std::string("la2x: ") + e.what(),
                                               "The Latin reader could not start.");
  }
}

const Analyser& Translator::analyser() const { return impl_->analyser; }
void Translator::resetDiscourse() { impl_->disc.clear(); }

std::string Translator::gloss(uint32_t lemma, Target t, bool* pivot) const {
  bool p = false;
  const lex::Lemma l = impl_->lx.lemma(lemma);
  std::string g = impl_->interGloss(lemma, l.pos, t, p);
  if (pivot) *pivot = p;
  return g;
}

void Translator::sentence(std::string_view latin, Target t, SentenceOut& out,
                          const std::vector<rules::GlossaryEntry>* glossary) {
  impl_->sentence(latin, t, out, glossary);
}

void Translator::text(std::string_view latin, Target t, std::vector<SentenceOut>& out,
                      const std::vector<rules::GlossaryEntry>* glossary) {
  out.clear();
  for (const auto& r : splitSentences(latin)) {
    out.emplace_back();
    impl_->sentence(latin.substr(r.first, r.second - r.first), t, out.back(), glossary);
  }
}

double Translator::roundTripOverlap(std::string_view latinText, const std::vector<std::string>& sourceLemmas,
                                    Target sourceLang) {
  try {
    return impl_->overlap(latinText, sourceLemmas, sourceLang);
  } catch (...) {
    return 1.0;
  }
}

std::vector<rules::CueOutput> Translator::cues(const std::vector<rules::CueInput>& cues, const rules::Options& opt,
                                               const rules::Context& ctx, const std::function<void(size_t)>& progress,
                                               const std::function<bool()>& cancelled) {
  std::vector<rules::CueOutput> outs;
  outs.reserve(cues.size());
  const Target t = opt.target == rules::Lang::Es ? Target::Es : Target::En;
  impl_->disc.clear();
  // the previous cue seeds the discourse memory (articles, pronouns)
  if (!cues.empty() && !cues.front().prevSource.empty()) {
    std::vector<SentenceOut> tmp;
    text(cues.front().prevSource, t, tmp, &ctx.glossary);
  }
  for (size_t ci = 0; ci < cues.size(); ++ci) {
    if (cancelled && cancelled()) break;
    const rules::CueInput& in = cues[ci];
    rules::CueOutput o;
    o.index = in.index;
    try {
      std::vector<SentenceOut> sents;
      const auto ranges = splitSentences(in.sourceText);
      double conf = 0;
      bool unknown = false, ambiguous = false, check = false;
      std::vector<std::string> unknownWords, ambiguousWords;
      std::string wordByWord;
      for (const auto& r : ranges) {
        SentenceOut so;
        impl_->sentence(std::string_view(in.sourceText).substr(r.first, r.second - r.first), t, so, &ctx.glossary);
        if (!o.target.empty()) o.target += ' ';
        o.target += so.text;
        conf += so.confidence;
        for (const std::string& f : so.flags)
          if (std::find(o.flags.begin(), o.flags.end(), f) == o.flags.end()) o.flags.push_back(f);
        for (const Word& w : so.words) {
          rules::TokenView tv;
          tv.text = w.text;
          tv.display = opt.macrons ? w.display : text::display_latin(w.display, false);
          tv.start = (int)r.first + w.start;
          tv.end = (int)r.first + w.end;
          tv.lemmaId = w.lemma;
          tv.hasLemma = w.lemma != lex::kNoLemma;
          tv.features = w.features;
          tv.tier = w.tier;
          if (opt.emoji) tv.emoji = w.emoji;
          tv.unknown = w.unknown;
          tv.fromRule = w.fromRule;
          const int ti = (int)o.tokens.size();
          o.tokens.push_back(tv);
          std::string data = "{\"lemmaId\":" + (tv.hasLemma ? std::to_string(w.lemma) : std::string("null")) +
                             ",\"head\":\"" + jsonEscape(w.head) + "\",\"gloss\":\"" + jsonEscape(w.gloss) +
                             "\",\"glossLang\":\"" + (t == Target::En ? "en" : "es") + "\",\"pivot\":" +
                             (w.glossPivot ? "true" : "false") + ",\"form\":\"" + jsonEscape(w.featureText) +
                             "\",\"role\":\"" + jsonEscape(w.role) + "\",\"confidence\":" +
                             std::to_string(w.confidence).substr(0, 5) + ",\"alternatives\":[";
          for (size_t k = 0; k < w.alternatives.size(); ++k)
            data += (k ? ",\"" : "\"") + jsonEscape(w.alternatives[k]) + "\"";
          data += "],\"note\":\"" + jsonEscape(w.note) + "\",\"why\":[";
          const Token& tk = so.analysis.tokens[(size_t)w.token];
          for (size_t k = 0; k < tk.why.size(); ++k) data += (k ? ",\"" : "\"") + jsonEscape(tk.why[k]) + "\"";
          data += "]}";
          std::string txt = w.head.empty() ? w.text : w.head;
          if (!w.featureText.empty()) txt += " (" + w.featureText + ")";
          if (!w.gloss.empty()) txt += ": " + w.gloss + (w.glossPivot ? (t == Target::Es ? " (vía inglés)" : " (via English)") : "");
          if (!w.note.empty()) txt += "; " + w.note;
          o.reasons.push_back(rules::Reason{ti, "analysis", txt, data});
          if (w.unknown) { unknown = true; unknownWords.push_back(w.text); }
          if (w.confidence < 0.6) { ambiguous = true; ambiguousWords.push_back(w.text); }
          if (w.nameGuess) check = true;
          std::string g = w.gloss.empty() ? w.text : w.gloss;
          const size_t comma = g.find(',');
          if (comma != std::string::npos) g = g.substr(0, comma);
          // a periphrasis: the verb form once, on the participle ("had been loved"); the form of sum adds nothing
          if (!w.note.empty()) {
            const size_t eq = w.note.find(" = "), par = w.note.find(" (", eq == std::string::npos ? 0 : eq);
            const bool isAux = w.role == "verb" && w.lemma != lex::kNoLemma && std::string(impl_->lx.lemma(w.lemma).key) == "sum";
            if (isAux) g.clear();
            else if (eq != std::string::npos && par != std::string::npos) g = w.note.substr(eq + 3, par - eq - 3);
          }
          if (!g.empty() && !wordByWord.empty()) wordByWord += ' ';
          wordByWord += g;
        }
        sents.push_back(std::move(so));
      }
      if (std::find(o.flags.begin(), o.flags.end(), "source-tokens") == o.flags.end()) o.flags.push_back("source-tokens");
      rules::Check a1{"A1", !unknown, unknown ? "unknown Latin words:" : ""};
      for (const std::string& w : unknownWords) a1.detail += " " + w;
      rules::Check amb{"ambiguity", !ambiguous, ambiguous ? "several readings fit:" : ""};
      for (const std::string& w : ambiguousWords) amb.detail += " " + w;
      o.checks.push_back(a1);
      o.checks.push_back(amb);
      const double mean = sents.empty() ? 1.0 : conf / (double)sents.size();
      for (const char* f : {"abl-abs", "gloss-missing", "name-guessed", "no-verb"})
        if (std::find(o.flags.begin(), o.flags.end(), f) != o.flags.end()) check = true;
      o.confidence = unknown ? rules::Confidence::Fix
                             : (ambiguous || check || mean < 0.7) ? rules::Confidence::Check : rules::Confidence::Ok;
      o.score = std::round(mean * 1000) / 1000;
      if (!wordByWord.empty()) o.alternatives.push_back(rules::Alternative{wordByWord, "word by word", 0.5});
    } catch (const std::exception& e) {
      o.target = in.sourceText;
      o.confidence = rules::Confidence::Fix;
      o.reasons.push_back(rules::Reason{-1, "analysis", std::string("could not analyse: ") + e.what(), ""});
    } catch (...) {
      o.target = in.sourceText;
      o.confidence = rules::Confidence::Fix;
    }
    outs.push_back(std::move(o));
    if (progress) progress(outs.size());
  }
  return outs;
}

}  // namespace vp::la2x
