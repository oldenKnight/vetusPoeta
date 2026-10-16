// analyseLatin / analyseGreek (vp/morph.h).
#include <algorithm>

#include "paradigm_la.h"
#include "vp/morph.h"
#include "vp/text.h"

namespace vp::morph {

using namespace vp::feat;

namespace {

bool startsUpper(std::string_view w) {
  if (w.empty()) return false;
  size_t i = 0;
  const char32_t c = text::decodeUtf8(w, i);
  std::string one;
  text::appendUtf8(one, c);
  return text::lower(one) != one;
}

// Clears a Token keeping its buffers (callers reuse one Token per word slot).
void reset(Token& t, std::string_view word) {
  t.text.assign(word.data(), word.size());
  t.key.clear();
  t.analyses.clear();
  t.enclitic = false;
  t.encliticText.clear();
  t.unknown = false;
  t.ruleAnalyses.clear();
  t.fromRule = t.whitakerOnly = t.capitalised = t.nameGuess = t.accentInsensitive = false;
}

void flagWhitaker(Token& t) {
  if (t.analyses.empty()) return;
  bool all = true;
  for (const lex::Analysis& a : t.analyses) all = all && (a.flags & lex::WhitakerOnly);
  t.whitakerOnly = all;
}

// Citation-form guesses for the paradigm analysis: (ending of the form key, ending of the citation key).
struct Guess { const char* formEnd; const char* citeEnd; };
const Guess kGuesses[] = {
    // 1st declension nouns / feminine adjectives
    {"a", "a"}, {"ae", "a"}, {"am", "a"}, {"arum", "a"}, {"is", "a"}, {"as", "a"},
    // 2nd declension
    {"us", "us"}, {"i", "us"}, {"o", "us"}, {"um", "us"}, {"orum", "us"}, {"is", "us"}, {"os", "us"}, {"e", "us"},
    {"um", "um"}, {"i", "um"}, {"o", "um"}, {"a", "um"}, {"orum", "um"}, {"is", "um"},
    {"a", "us"}, {"ae", "us"}, {"am", "us"}, {"arum", "us"}, {"as", "us"},
    // comparatives / superlatives of 1st/2nd adjectives
    {"ior", "us"}, {"ius", "us"}, {"iorem", "us"}, {"ioris", "us"}, {"iori", "us"}, {"iore", "us"}, {"iores", "us"},
    {"iora", "us"}, {"iorum", "us"}, {"ioribus", "us"}, {"issimus", "us"}, {"issima", "us"}, {"issimum", "us"},
    // 1st conjugation (root + a...) and 2nd conjugation (root + e...)
    {"o", "o"}, {"as", "o"}, {"at", "o"}, {"amus", "o"}, {"atis", "o"}, {"ant", "o"}, {"abam", "o"}, {"abat", "o"},
    {"abant", "o"}, {"abo", "o"}, {"abit", "o"}, {"abunt", "o"}, {"em", "o"}, {"et", "o"}, {"ent", "o"},
    {"arem", "o"}, {"aret", "o"}, {"are", "o"}, {"ari", "o"}, {"atur", "o"}, {"antur", "o"}, {"aui", "o"},
    {"auit", "o"}, {"auerunt", "o"}, {"auerat", "o"}, {"auisse", "o"}, {"atus", "o"}, {"ate", "o"}, {"a", "o"},
    {"eo", "eo"}, {"es", "eo"}, {"et", "eo"}, {"emus", "eo"}, {"etis", "eo"}, {"ent", "eo"}, {"ebam", "eo"},
    {"ebat", "eo"}, {"ebant", "eo"}, {"ebo", "eo"}, {"ebit", "eo"}, {"eam", "eo"}, {"eat", "eo"}, {"eant", "eo"},
    {"erem", "eo"}, {"eret", "eo"}, {"ere", "eo"}, {"eri", "eo"}, {"etur", "eo"}, {"entur", "eo"}, {"e", "eo"},
    {"ete", "eo"}, {"ui", "eo"}, {"uit", "eo"}, {"uerunt", "eo"},
};

void paradigmAnalyse(const lex::Lexicon& lx, const std::string& key, Token& out) {
  thread_local std::vector<lex::Analysis> an;
  thread_local std::vector<detail::Cell> cells;
  thread_local std::vector<uint32_t> tried;
  tried.clear();
  for (const Guess& g : kGuesses) {
    std::string_view fe(g.formEnd);
    if (key.size() <= fe.size() + 1 || key.compare(key.size() - fe.size(), fe.size(), fe) != 0) continue;
    const std::string stem = key.substr(0, key.size() - fe.size());
    std::string cite = stem + g.citeEnd;
    an.clear();
    lx.lookup(cite, an);
    if (an.empty() && std::string_view(g.citeEnd) == "us" && !stem.empty() && stem.back() == 'r') {
      cite = stem.substr(0, stem.size() - 1) + "er";   // pulchr-ās -> pulcher, agr-ī -> ager
      lx.lookup(cite, an);
    }
    for (const lex::Analysis& a : an) {
      if (std::find(tried.begin(), tried.end(), a.lemma) != tried.end()) continue;
      tried.push_back(a.lemma);
      const lex::Lemma l = lx.lemma(a.lemma);
      if (l.key != cite || !detail::paradigmApplies(l)) continue;
      cells.clear();
      detail::paradigm(l, cells);
      for (const detail::Cell& c : cells)
        if (text::latin_key(c.form) == key) out.ruleAnalyses.push_back(RuleAnalysis{a.lemma, c.packed, c.form});
    }
    if (tried.size() > 64) break;
  }
  out.fromRule = !out.ruleAnalyses.empty();
}

}  // namespace

void analyseLatin(const lex::Lexicon& lx, std::string_view word, Token& out) {
  reset(out, word);
  text::latin_key(word, out.key);
  out.capitalised = startsUpper(word);
  if (out.key.empty()) { out.unknown = true; return; }
  lx.lookup(out.key, out.analyses);
  if (!out.analyses.empty()) { flagWhitaker(out); return; }
  // Enclitics: only when the whole is unknown and the base is known.
  static const char* const kEnclitics[] = {"que", "ne", "ue"};
  for (const char* enc : kEnclitics) {
    std::string_view e(enc);
    if (out.key.size() <= e.size() + 1 || out.key.compare(out.key.size() - e.size(), e.size(), e) != 0) continue;
    const std::string base = out.key.substr(0, out.key.size() - e.size());
    lx.lookup(base, out.analyses);
    if (!out.analyses.empty()) {
      out.enclitic = true;
      // The written enclitic as in the text ("ve" stays "ve").
      std::string lowerText = text::lower(text::nfc(word));
      out.encliticText = lowerText.size() >= e.size() ? lowerText.substr(lowerText.size() - e.size()) : std::string(e);
      out.key = base;
      flagWhitaker(out);
      return;
    }
  }
  // Paradigm fallback (lemmas without a table).
  paradigmAnalyse(lx, out.key, out);
  if (out.fromRule) return;
  out.unknown = true;
  out.nameGuess = out.capitalised;
}

// ---- Greek -------------------------------------------------------------------------------------------------------
namespace {

bool isGreekVowel(char32_t c) {
  switch (c) {
    case U'α': case U'ε': case U'η': case U'ι': case U'ο': case U'υ': case U'ω': return true;
    default: return false;
  }
}

// Tries accent placements on the bare word (NFD letters): an initial breathing on a word-initial vowel (or rho), one
// accent (acute, grave, circumflex) on one vowel. Bounded to 400 lookups. Keeps analyses whose bare display equals
// the bare word.
void greekBareSearch(const lex::Lexicon& lx, const std::string& bare, Token& out) {
  std::u32string b = text::toUtf32(text::nfd(bare));
  if (b.empty() || b.size() > 24) return;
  std::vector<size_t> vowels;
  for (size_t i = 0; i < b.size(); ++i)
    if (isGreekVowel(b[i])) vowels.push_back(i);
  if (vowels.empty()) return;
  const bool initVowel = isGreekVowel(b[0]);
  // initial diphthong: breathing goes on the second vowel (αι, ει, οι, υι, αυ, ευ, ου, ηυ)
  size_t breathAt = 0;
  if (initVowel && b.size() > 1 && isGreekVowel(b[1]) && (b[1] == U'ι' || b[1] == U'υ')) breathAt = 1;
  std::vector<char32_t> breaths;
  if (initVowel) breaths = {0x0313, 0x0314};
  else if (b[0] == U'ρ') breaths = {0, 0x0314};
  else breaths = {0};
  static const char32_t accents[] = {0x0301, 0x0300, 0x0342};
  thread_local std::vector<lex::Analysis> an;
  int lookups = 0;
  std::string cand, key, displayBare;
  for (char32_t br : breaths) {
    for (size_t vi = 0; vi < vowels.size(); ++vi) {
      for (char32_t ac : accents) {
        if (++lookups > 400) return;
        std::u32string w;
        for (size_t i = 0; i < b.size(); ++i) {
          w.push_back(b[i]);
          if (i == breathAt && br) w.push_back(br);
          if (i == vowels[vi]) w.push_back(ac);
        }
        cand = text::nfc(text::toUtf8(w));
        text::greek_key(cand, key);
        an.clear();
        lx.lookup(key, an);
        for (const lex::Analysis& a : an) {
          text::greek_bare(a.display, displayBare);
          if (displayBare == bare) out.analyses.push_back(a);
        }
      }
    }
  }
}

}  // namespace

void analyseGreek(const lex::Lexicon& lx, std::string_view word, Token& out) {
  reset(out, word);
  text::greek_key(word, out.key);
  out.capitalised = startsUpper(word);
  if (out.key.empty()) { out.unknown = true; return; }
  lx.lookup(out.key, out.analyses);
  if (!out.analyses.empty()) { flagWhitaker(out); return; }
  const std::string bare = text::greek_bare(word);
  lx.lookup(bare, out.analyses);   // a file that carries bare keys answers directly
  if (out.analyses.empty()) greekBareSearch(lx, bare, out);
  if (!out.analyses.empty()) {
    out.accentInsensitive = true;
    // de-duplicate (lemma, feat)
    std::stable_sort(out.analyses.begin(), out.analyses.end(), [](const lex::Analysis& a, const lex::Analysis& b) {
      return a.lemma != b.lemma ? a.lemma < b.lemma : a.feat < b.feat;
    });
    out.analyses.erase(std::unique(out.analyses.begin(), out.analyses.end(),
                                   [](const lex::Analysis& a, const lex::Analysis& b) {
                                     return a.lemma == b.lemma && a.feat == b.feat && a.display == b.display;
                                   }),
                       out.analyses.end());
    return;
  }
  out.unknown = true;
  out.nameGuess = out.capitalised;
}

}  // namespace vp::morph
