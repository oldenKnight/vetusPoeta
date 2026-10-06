// English word morphology for the readable sentence: verb forms, noun plurals, comparison, a/an.
#include <algorithm>
#include <cstring>

#include "internal.h"

namespace vp::la2x::detail::en {

namespace {

struct Irr { const char* base; const char* past; const char* pp; };
// Irregular verbs (base, simple past, past participle). Sorted by base for binary search.
const Irr kIrr[] = {
    {"arise", "arose", "arisen"}, {"awake", "awoke", "awoken"}, {"be", "was", "been"}, {"bear", "bore", "borne"},
    {"beat", "beat", "beaten"}, {"become", "became", "become"}, {"begin", "began", "begun"}, {"bend", "bent", "bent"},
    {"bind", "bound", "bound"}, {"bite", "bit", "bitten"}, {"bleed", "bled", "bled"}, {"blow", "blew", "blown"},
    {"break", "broke", "broken"}, {"bring", "brought", "brought"}, {"build", "built", "built"},
    {"burn", "burned", "burned"}, {"buy", "bought", "bought"}, {"catch", "caught", "caught"},
    {"choose", "chose", "chosen"}, {"cling", "clung", "clung"}, {"come", "came", "come"}, {"cost", "cost", "cost"},
    {"creep", "crept", "crept"}, {"cut", "cut", "cut"}, {"deal", "dealt", "dealt"}, {"dig", "dug", "dug"},
    {"do", "did", "done"}, {"draw", "drew", "drawn"}, {"dream", "dreamed", "dreamed"}, {"drink", "drank", "drunk"},
    {"drive", "drove", "driven"}, {"dwell", "dwelt", "dwelt"}, {"eat", "ate", "eaten"}, {"fall", "fell", "fallen"},
    {"feed", "fed", "fed"}, {"feel", "felt", "felt"}, {"fight", "fought", "fought"}, {"find", "found", "found"},
    {"flee", "fled", "fled"}, {"fling", "flung", "flung"}, {"fly", "flew", "flown"}, {"forbid", "forbade", "forbidden"},
    {"forget", "forgot", "forgotten"}, {"forgive", "forgave", "forgiven"}, {"freeze", "froze", "frozen"},
    {"get", "got", "got"}, {"give", "gave", "given"}, {"go", "went", "gone"}, {"grind", "ground", "ground"},
    {"grow", "grew", "grown"}, {"hang", "hung", "hung"}, {"have", "had", "had"}, {"hear", "heard", "heard"},
    {"hide", "hid", "hidden"}, {"hit", "hit", "hit"}, {"hold", "held", "held"}, {"hurt", "hurt", "hurt"},
    {"keep", "kept", "kept"}, {"kneel", "knelt", "knelt"}, {"know", "knew", "known"}, {"lay", "laid", "laid"},
    {"lead", "led", "led"}, {"lean", "leaned", "leaned"}, {"leap", "leapt", "leapt"}, {"learn", "learned", "learned"},
    {"leave", "left", "left"}, {"lend", "lent", "lent"}, {"let", "let", "let"}, {"lie", "lay", "lain"},
    {"light", "lit", "lit"}, {"lose", "lost", "lost"}, {"make", "made", "made"}, {"mean", "meant", "meant"},
    {"meet", "met", "met"}, {"pay", "paid", "paid"}, {"put", "put", "put"}, {"quit", "quit", "quit"},
    {"read", "read", "read"}, {"rend", "rent", "rent"}, {"ride", "rode", "ridden"}, {"ring", "rang", "rung"},
    {"rise", "rose", "risen"}, {"run", "ran", "run"}, {"say", "said", "said"}, {"see", "saw", "seen"},
    {"seek", "sought", "sought"}, {"sell", "sold", "sold"}, {"send", "sent", "sent"}, {"set", "set", "set"},
    {"sew", "sewed", "sewn"}, {"shake", "shook", "shaken"}, {"shed", "shed", "shed"}, {"shine", "shone", "shone"},
    {"shoot", "shot", "shot"}, {"show", "showed", "shown"}, {"shut", "shut", "shut"}, {"sing", "sang", "sung"},
    {"sink", "sank", "sunk"}, {"sit", "sat", "sat"}, {"slay", "slew", "slain"}, {"sleep", "slept", "slept"},
    {"slide", "slid", "slid"}, {"sling", "slung", "slung"}, {"sow", "sowed", "sown"}, {"speak", "spoke", "spoken"},
    {"spend", "spent", "spent"}, {"spin", "spun", "spun"}, {"spit", "spat", "spat"}, {"split", "split", "split"},
    {"spread", "spread", "spread"}, {"spring", "sprang", "sprung"}, {"stand", "stood", "stood"},
    {"steal", "stole", "stolen"}, {"stick", "stuck", "stuck"}, {"sting", "stung", "stung"},
    {"strike", "struck", "struck"}, {"strive", "strove", "striven"}, {"swear", "swore", "sworn"},
    {"sweep", "swept", "swept"}, {"swim", "swam", "swum"}, {"swing", "swung", "swung"}, {"take", "took", "taken"},
    {"teach", "taught", "taught"}, {"tear", "tore", "torn"}, {"tell", "told", "told"}, {"think", "thought", "thought"},
    {"throw", "threw", "thrown"}, {"tread", "trod", "trodden"}, {"understand", "understood", "understood"},
    {"wake", "woke", "woken"}, {"wear", "wore", "worn"}, {"weave", "wove", "woven"}, {"weep", "wept", "wept"},
    {"win", "won", "won"}, {"wind", "wound", "wound"}, {"wring", "wrung", "wrung"}, {"write", "wrote", "written"},
};

const Irr* irregular(const std::string& base) {
  const Irr* b = std::begin(kIrr);
  const Irr* e = std::end(kIrr);
  const Irr* it = std::lower_bound(b, e, base, [](const Irr& x, const std::string& k) { return std::strcmp(x.base, k.c_str()) < 0; });
  if (it != e && base == it->base) return it;
  // prefixed verbs: undergo, overcome, withdraw, foresee, ...
  for (const char* pre : {"under", "over", "with", "fore", "re", "mis", "un", "out", "be"}) {
    const size_t n = std::strlen(pre);
    if (base.size() > n + 1 && base.compare(0, n, pre) == 0) {
      const std::string rest = base.substr(n);
      it = std::lower_bound(b, e, rest, [](const Irr& x, const std::string& k) { return std::strcmp(x.base, k.c_str()) < 0; });
      if (it != e && rest == it->base && rest != "be") return it;
    }
  }
  return nullptr;
}

bool isVowel(char c) { return c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u'; }

// consonant-vowel-consonant ending of a one-syllable word (stop, run, sit): double the last letter
bool doubles(const std::string& w) {
  const size_t n = w.size();
  if (n < 3) return false;
  const char c = w[n - 1], v = w[n - 2], p = w[n - 3];
  if (isVowel(c) || !isVowel(v) || isVowel(p) || c == 'w' || c == 'x' || c == 'y') return false;
  int groups = 0;
  bool in = false;
  for (char ch : w) {
    const bool vv = isVowel(ch) || ch == 'y';
    if (vv && !in) ++groups;
    in = vv;
  }
  if (groups == 1) return true;
  // two-syllable words stressed on the last syllable (begin, forget, permit, admit, occur, prefer, refer)
  static const char* const kLast[] = {"admit", "begin", "commit", "compel", "control", "expel", "forbid", "forget",
                                      "occur", "omit", "patrol", "permit", "prefer", "propel", "refer", "regret",
                                      "submit", "transmit", "upset"};
  for (const char* x : kLast)
    if (w == x) return true;
  return false;
}

std::string inflectWord(const std::string& w, VForm f) {
  if (w.empty()) return w;
  if (w == "be") {
    switch (f) {
      case VForm::Base: return "be";
      case VForm::S3: return "is";
      case VForm::Past: return "was";
      case VForm::PastPart: return "been";
      case VForm::Ing: return "being";
    }
  }
  if (f == VForm::Base) return w;
  const size_t n = w.size();
  const char last = w[n - 1];
  if (f == VForm::S3) {
    if (w == "have") return "has";
    if (w == "do") return "does";
    if (w == "go") return "goes";
    if (last == 'y' && n > 1 && !isVowel(w[n - 2])) return w.substr(0, n - 1) + "ies";
    if (last == 's' || last == 'x' || last == 'z' || last == 'o' || (n > 1 && (w.compare(n - 2, 2, "ch") == 0 || w.compare(n - 2, 2, "sh") == 0)))
      return w + "es";
    return w + "s";
  }
  if (f == VForm::Ing) {
    if (w == "be") return "being";
    if (n > 2 && w.compare(n - 2, 2, "ie") == 0) return w.substr(0, n - 2) + "ying";
    if (last == 'e' && n > 2 && w != "see" && w != "flee" && w != "agree" && w.compare(n - 2, 2, "ee") != 0 &&
        w.compare(n - 2, 2, "ye") != 0 && w.compare(n - 2, 2, "oe") != 0)
      return w.substr(0, n - 1) + "ing";
    if (doubles(w)) return w + w.back() + "ing";
    return w + "ing";
  }
  if (const Irr* irr = irregular(w)) {
    if (w == irr->base) return f == VForm::Past ? irr->past : irr->pp;
    // prefixed
    const size_t pre = w.size() - std::strlen(irr->base);
    return w.substr(0, pre) + (f == VForm::Past ? irr->past : irr->pp);
  }
  if (last == 'e') return w + "d";
  if (last == 'y' && n > 1 && !isVowel(w[n - 2])) return w.substr(0, n - 1) + "ied";
  if (doubles(w)) return w + w.back() + "ed";
  return w + "ed";
}

}  // namespace

std::string verb(std::string_view base, VForm f) {
  std::string b(base);
  const size_t sp = b.find(' ');
  if (sp == std::string::npos) return inflectWord(b, f);
  return inflectWord(b.substr(0, sp), f) + b.substr(sp);
}

std::string plural(std::string_view noun) {
  std::string w(noun);
  if (w.empty()) return w;
  // "X of Y" -> pluralise X
  const size_t of = w.find(" of ");
  if (of != std::string::npos) return plural(w.substr(0, of)) + w.substr(of);
  const size_t sp = w.rfind(' ');
  if (sp != std::string::npos) return w.substr(0, sp + 1) + plural(w.substr(sp + 1));
  static const char* const kIrrPl[][2] = {
      {"man", "men"}, {"woman", "women"}, {"child", "children"}, {"foot", "feet"}, {"tooth", "teeth"},
      {"goose", "geese"}, {"mouse", "mice"}, {"person", "people"}, {"ox", "oxen"}, {"sheep", "sheep"},
      {"deer", "deer"}, {"fish", "fish"}, {"life", "lives"}, {"wife", "wives"}, {"knife", "knives"},
      {"leaf", "leaves"}, {"wolf", "wolves"}, {"half", "halves"}, {"calf", "calves"}, {"loaf", "loaves"},
      {"thief", "thieves"}, {"shelf", "shelves"}, {"self", "selves"}, {"elf", "elves"}, {"hero", "heroes"},
      {"potato", "potatoes"}, {"tomato", "tomatoes"}, {"god", "gods"}, {"louse", "lice"}, {"die", "dice"},
      {"species", "species"}, {"series", "series"}, {"people", "people"}, {"children", "children"}};
  for (const auto& p : kIrrPl)
    if (w == p[0]) return p[1];
  if (w.size() > 3 && w.compare(w.size() - 3, 3, "man") == 0 && w != "human" && w != "german" && w != "roman")
    return w.substr(0, w.size() - 3) + "men";
  const size_t n = w.size();
  const char last = w[n - 1];
  if (last == 'y' && n > 1 && !isVowel(w[n - 2])) return w.substr(0, n - 1) + "ies";
  if (last == 's' || last == 'x' || last == 'z' || (n > 1 && (w.compare(n - 2, 2, "ch") == 0 || w.compare(n - 2, 2, "sh") == 0)))
    return w + "es";
  return w + "s";
}

namespace {
int syllables(const std::string& w) {
  int groups = 0;
  bool in = false;
  for (size_t i = 0; i < w.size(); ++i) {
    const char ch = w[i];
    const bool vv = isVowel(ch) || (ch == 'y' && i > 0);
    if (vv && !in) ++groups;
    in = vv;
  }
  if (w.size() > 2 && w.back() == 'e' && !isVowel(w[w.size() - 2]) && groups > 1 && w[w.size() - 2] != 'l') --groups;
  return groups;
}
std::string compareForm(std::string_view adj, bool super) {
  std::string w(adj);
  if (w.find(' ') != std::string::npos) return std::string(super ? "most " : "more ") + w;
  static const char* const kIrrCmp[][3] = {{"good", "better", "best"}, {"bad", "worse", "worst"},
                                        {"much", "more", "most"},   {"many", "more", "most"},
                                        {"little", "less", "least"}, {"far", "farther", "farthest"},
                                        {"well", "better", "best"}};
  for (const auto& r : kIrrCmp)
    if (w == r[0]) return super ? r[2] : r[1];
  const int syl = syllables(w);
  const size_t n = w.size();
  if (syl == 1 || (syl == 2 && n > 1 && w.back() == 'y')) {
    std::string stem = w;
    if (stem.back() == 'y' && n > 1 && !isVowel(stem[n - 2])) stem = stem.substr(0, n - 1) + "i";
    else if (stem.back() == 'e') stem.pop_back();
    else if (doubles(stem)) stem += stem.back();
    return stem + (super ? "est" : "er");
  }
  return std::string(super ? "most " : "more ") + w;
}
}  // namespace

std::string comparative(std::string_view adj) { return compareForm(adj, false); }
std::string superlative(std::string_view adj) { return compareForm(adj, true); }

std::string indefinite(std::string_view w) {
  if (w.empty()) return "a";
  std::string low;
  for (char c : w) low += (char)((c >= 'A' && c <= 'Z') ? c + 32 : c);
  for (const char* x : {"hour", "honest", "honour", "honor", "heir"})
    if (low.compare(0, std::strlen(x), x) == 0) return "an";
  for (const char* x : {"uni", "use", "usu", "one", "once", "eu", "ewe", "ure"})
    if (low.compare(0, std::strlen(x), x) == 0) return "a";
  return isVowel(low[0]) ? "an" : "a";
}

}  // namespace vp::la2x::detail::en
