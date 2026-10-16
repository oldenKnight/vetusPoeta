// Built-in lemma rules: the fallback when no lexicon lemmatiser is set or it returns "" (DESIGN.md section 17).
#include <cstring>

#include "vp/nlp.h"

namespace vp::nlp {
namespace {

bool endsWith(const std::string& s, const char* suf) {
  const size_t n = std::strlen(suf);
  return s.size() >= n && s.compare(s.size() - n, n, suf) == 0;
}
bool isVowel(char c) { return c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u'; }
bool isAsciiWord(const std::string& s) {
  if (s.empty()) return false;
  for (char c : s)
    if (!((c >= 'a' && c <= 'z') || c == '-' || c == '\'')) return false;
  return true;
}

struct Irregular {
  const char* form;
  const char* lemma;
};
const Irregular kIrregular[] = {
    {"am", "be"}, {"is", "be"}, {"are", "be"}, {"was", "be"}, {"were", "be"}, {"been", "be"}, {"being", "be"},
    {"'m", "be"}, {"'re", "be"}, {"has", "have"}, {"had", "have"}, {"having", "have"}, {"'ve", "have"},
    {"does", "do"}, {"did", "do"}, {"done", "do"}, {"doing", "do"}, {"went", "go"}, {"gone", "go"}, {"goes", "go"},
    {"said", "say"}, {"says", "say"}, {"got", "get"}, {"gotten", "get"}, {"made", "make"}, {"knew", "know"},
    {"known", "know"}, {"thought", "think"}, {"took", "take"}, {"taken", "take"}, {"saw", "see"}, {"seen", "see"},
    {"came", "come"}, {"gave", "give"}, {"given", "give"}, {"found", "find"}, {"told", "tell"}, {"felt", "feel"},
    {"left", "leave"}, {"brought", "bring"}, {"bought", "buy"}, {"ran", "run"}, {"sat", "sit"}, {"stood", "stand"},
    {"wrote", "write"}, {"written", "write"}, {"ate", "eat"}, {"eaten", "eat"}, {"began", "begin"},
    {"begun", "begin"}, {"kept", "keep"}, {"meant", "mean"}, {"met", "meet"}, {"heard", "hear"}, {"held", "hold"},
    {"lost", "lose"}, {"paid", "pay"}, {"sent", "send"}, {"spoke", "speak"}, {"spoken", "speak"}, {"slept", "sleep"},
    {"children", "child"}, {"men", "man"}, {"women", "woman"}, {"people", "person"}, {"feet", "foot"},
    {"teeth", "tooth"}, {"mice", "mouse"}, {"better", "good"}, {"best", "good"}, {"worse", "bad"}, {"worst", "bad"},
};

std::string verbStem(const std::string& w, size_t cut) {
  std::string s = w.substr(0, w.size() - cut);
  const size_t n = s.size();
  // doubled final consonant: stopped -> stop, running -> run (not -ll/-ss/-zz: called, passed)
  if (n >= 3 && s[n - 1] == s[n - 2] && !isVowel(s[n - 1]) && s[n - 1] != 'l' && s[n - 1] != 's' && s[n - 1] != 'z')
    return s.substr(0, n - 1);
  // consonant-vowel-consonant short stems and -v/-c/-g endings take a silent e: liked -> like, making -> make
  if (n >= 2 && (s[n - 1] == 'v' || s[n - 1] == 'c' || (s[n - 1] == 'g' && s[n - 2] != 'n')))
    return s + "e";
  if (n == 3 && !isVowel(s[0]) && isVowel(s[1]) && !isVowel(s[2]) && s[2] != 'w' && s[2] != 'x' && s[2] != 'y')
    return s + "e";
  return s;
}

std::string englishLemma(const std::string& w, const std::string& upos) {
  for (const Irregular& ir : kIrregular)
    if (w == ir.form) return ir.lemma;
  if (!isAsciiWord(w) || w.size() < 3) return w;
  if (upos == "NOUN" || upos == "VERB" || upos == "AUX") {
    if (endsWith(w, "ies") && w.size() > 4) return w.substr(0, w.size() - 3) + "y";
    if (endsWith(w, "sses") || endsWith(w, "shes") || endsWith(w, "ches") || endsWith(w, "xes") ||
        endsWith(w, "zes"))
      return w.substr(0, w.size() - 2);
    if (endsWith(w, "s") && !endsWith(w, "ss") && !endsWith(w, "us") && !endsWith(w, "is") && w.size() > 3)
      return w.substr(0, w.size() - 1);
  }
  if (upos == "VERB" || upos == "AUX") {
    if (endsWith(w, "ied") && w.size() > 4) return w.substr(0, w.size() - 3) + "y";
    if (endsWith(w, "eed")) return w;
    if (endsWith(w, "ed") && w.size() > 4) return verbStem(w, 2);
    if (endsWith(w, "ing") && w.size() > 5) return verbStem(w, 3);
  }
  if (upos == "ADJ" || upos == "ADV") {
    if (endsWith(w, "iest") && w.size() > 5) return w.substr(0, w.size() - 4) + "y";
    if (endsWith(w, "ier") && w.size() > 4) return w.substr(0, w.size() - 3) + "y";
    if (endsWith(w, "est") && w.size() > 5) return verbStem(w, 3);
    if (endsWith(w, "er") && w.size() > 4) return verbStem(w, 2);
  }
  return w;
}

}  // namespace

std::string ruleLemma(Lang lang, const Token& token) {
  if (token.upos == "PROPN") return token.text;
  if (lang == Lang::Es) return token.lower;
  return englishLemma(token.lower, token.upos);
}

}  // namespace vp::nlp
