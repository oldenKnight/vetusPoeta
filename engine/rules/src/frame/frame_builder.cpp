// FrameBuilder (DESIGN.md §10.1 items 2-6): tokens -> contractions -> tagger/parser -> lexicon lemmas -> phrasebook
// pre-pass -> units (phrase pieces and clause groups) -> SemFrame from the dependency tree.
// Closed classes are normalised to canonical English words here (determiners, prepositions, connectors, wh words),
// so the transfer stage is language neutral; content lemmas stay in the source language.
#include <algorithm>
#include <cstring>

#include "english.h"
#include "spanish.h"
#include "vp/features.h"
#include "vp/frame.h"
#include "vp/text.h"

namespace vp::frame {

namespace {

using nlp::Token;

bool in(const std::string& w, std::initializer_list<const char*> set) {
  for (const char* s : set)
    if (w == s) return true;
  return false;
}

bool isPunctTok(const Token& t) { return t.upos == "PUNCT" || t.upos == "SYM"; }
// acute vowels (UTF-8) and their plain forms, null-terminated pairs
const char* const kAcute[] = {"\xC3\xA1", "a", "\xC3\xA9", "e", "\xC3\xAD", "i", "\xC3\xB3", "o", "\xC3\xBA", "u",
                              nullptr, nullptr};

uint32_t fget(const Token& t, nlp::morph::Shift s) { return nlp::morph::get(t.feats, s); }

bool upper(const std::string& s) { return !s.empty() && ((s[0] >= 'A' && s[0] <= 'Z') || (unsigned char)s[0] >= 0xC3); }

// ---- canonical closed classes ------------------------------------------------------------------------------------
struct PronInfo { const char* w; uint8_t person, number, gender; bool refl; };
const PronInfo kPronEn[] = {
    {"i", 1, 1, 0, false},       {"me", 1, 1, 0, false},       {"myself", 1, 1, 0, true},
    {"you", 2, 0, 0, false},     {"yourself", 2, 1, 0, true},  {"yourselves", 2, 2, 0, true},
    {"he", 3, 1, feat::M, false}, {"him", 3, 1, feat::M, false}, {"himself", 3, 1, feat::M, true},
    {"she", 3, 1, feat::F, false}, {"her", 3, 1, feat::F, false}, {"herself", 3, 1, feat::F, true},
    {"it", 3, 1, feat::N, false}, {"itself", 3, 1, feat::N, true},
    {"we", 1, 2, 0, false},      {"us", 1, 2, 0, false},       {"ourselves", 1, 2, 0, true},
    {"they", 3, 2, 0, false},    {"them", 3, 2, 0, false},     {"themselves", 3, 2, 0, true},
    {"thee", 2, 1, 0, false},    {"thou", 2, 1, 0, false},     {"ye", 2, 2, 0, false}};
const PronInfo kPronEs[] = {
    {"yo", 1, 1, 0, false},      {"me", 1, 1, 0, false},       {"mí", 1, 1, 0, false},     {"conmigo", 1, 1, 0, false},
    {"tú", 2, 1, 0, false},      {"tu", 2, 1, 0, false},       {"te", 2, 1, 0, false},     {"ti", 2, 1, 0, false},
    {"contigo", 2, 1, 0, false}, {"vos", 2, 1, 0, false},      {"usted", 2, 1, 0, false},  {"ustedes", 2, 2, 0, false},
    {"él", 3, 1, feat::M, false}, {"ella", 3, 1, feat::F, false}, {"ello", 3, 1, feat::N, false},
    {"lo", 3, 1, feat::M, false}, {"la", 3, 1, feat::F, false}, {"le", 3, 1, 0, false},
    {"nosotros", 1, 2, feat::M, false}, {"nosotras", 1, 2, feat::F, false}, {"nos", 1, 2, 0, false},
    {"vosotros", 2, 2, feat::M, false}, {"vosotras", 2, 2, feat::F, false}, {"os", 2, 2, 0, false},
    {"ellos", 3, 2, feat::M, false}, {"ellas", 3, 2, feat::F, false}, {"los", 3, 2, feat::M, false},
    {"las", 3, 2, feat::F, false}, {"les", 3, 2, 0, false},   {"se", 3, 0, 0, true},      {"sí", 3, 0, 0, true},
    {"consigo", 3, 0, 0, true}};

struct PossInfo { const char* w; uint8_t person, number; };
const PossInfo kPossEn[] = {{"my", 1, 1}, {"mine", 1, 1}, {"your", 2, 0}, {"yours", 2, 0}, {"his", 3, 1},
                            {"her", 3, 1}, {"hers", 3, 1}, {"its", 3, 1}, {"our", 1, 2}, {"ours", 1, 2},
                            {"their", 3, 2}, {"theirs", 3, 2}, {"thy", 2, 1}};
const PossInfo kPossEs[] = {{"mi", 1, 1}, {"mis", 1, 1}, {"mío", 1, 1}, {"mía", 1, 1}, {"tu", 2, 1}, {"tus", 2, 1},
                            {"tuyo", 2, 1}, {"tuya", 2, 1}, {"su", 3, 0}, {"sus", 3, 0}, {"suyo", 3, 0},
                            {"suya", 3, 0}, {"nuestro", 1, 2}, {"nuestra", 1, 2}, {"nuestros", 1, 2},
                            {"nuestras", 1, 2}, {"vuestro", 2, 2}, {"vuestra", 2, 2}, {"vuestros", 2, 2},
                            {"vuestras", 2, 2}};

// Determiners / quantifiers -> canonical English.
std::string canonDet(SrcLang lang, const std::string& w) {
  if (lang == SrcLang::En) {
    if (in(w, {"an"})) return "a";
    if (in(w, {"each"})) return "every";
    if (in(w, {"these"})) return "this";
    if (in(w, {"those"})) return "that";
    if (in(w, {"whose"})) return "whose";
    return w;
  }
  if (in(w, {"el", "la", "los", "las", "lo"})) return "the";
  if (in(w, {"un", "una", "unos", "unas"})) return "a";
  if (in(w, {"este", "esta", "estos", "estas", "esto"})) return "this";
  if (in(w, {"ese", "esa", "esos", "esas", "eso", "aquel", "aquella", "aquellos", "aquellas", "aquello"})) return "that";
  if (in(w, {"algún", "alguno", "alguna", "algunos", "algunas"})) return "some";
  if (in(w, {"ningún", "ninguno", "ninguna"})) return "no";
  if (in(w, {"cada"})) return "every";
  if (in(w, {"todo", "toda", "todos", "todas"})) return "all";
  if (in(w, {"otro", "otra", "otros", "otras"})) return "another";
  if (in(w, {"qué"})) return "what";
  if (in(w, {"cuál", "cuáles"})) return "which";
  if (in(w, {"cuánto", "cuánta", "cuántos", "cuántas"})) return "how many";
  if (in(w, {"mucho", "mucha", "muchos", "muchas"})) return "many";
  if (in(w, {"poco", "poca", "pocos", "pocas"})) return "few";
  return w;
}

// Prepositions -> canonical English (preps_en_la.tsv is English-keyed).
std::string canonPrep(SrcLang lang, const std::string& w) {
  if (lang == SrcLang::En) return w;
  static const std::pair<const char*, const char*> kMap[] = {
      {"a", "to"},         {"en", "in"},          {"de", "of"},         {"con", "with"},     {"sin", "without"},
      {"por", "by"},       {"para", "for"},       {"desde", "from"},    {"hasta", "until"},  {"sobre", "on"},
      {"bajo", "under"},   {"entre", "between"},  {"contra", "against"}, {"hacia", "toward"}, {"tras", "after"},
      {"ante", "before"},  {"durante", "during"}, {"cerca", "near"},    {"delante", "in front of"},
      {"detrás", "behind"}, {"dentro", "inside"}, {"fuera", "outside"}, {"encima", "above"}, {"debajo", "below"},
      {"según", "according to"}, {"excepto", "except"}, {"alrededor", "around"}, {"a través", "through"}};
  for (const auto& m : kMap)
    if (w == m.first) return m.second;
  return w;
}

// Subordinators -> relation + canonical marker.
bool relationOf(SrcLang lang, const std::string& m, Relation& rel, std::string& canon) {
  struct R { const char* w; Relation r; const char* c; };
  static const R kEn[] = {{"because", Relation::Cause, "because"}, {"since", Relation::Cause, "because"},
                          {"as", Relation::Cause, "because"},      {"when", Relation::Time, "when"},
                          {"whenever", Relation::Time, "when"},    {"while", Relation::Time, "while"},
                          {"after", Relation::Time, "after"},      {"before", Relation::Time, "before"},
                          {"until", Relation::Time, "until"},      {"till", Relation::Time, "until"},
                          {"once", Relation::Time, "when"},        {"if", Relation::Condition, "if"},
                          {"unless", Relation::Condition, "unless"}, {"so", Relation::Purpose, "so that"},
                          {"to", Relation::Purpose, "to"},         {"although", Relation::Concession, "although"},
                          {"though", Relation::Concession, "although"}, {"that", Relation::Complement, "that"},
                          {"whether", Relation::Complement, "whether"}, {"like", Relation::Manner, "as"},
                          {"soon", Relation::Time, "as soon as"}};   // C19: "as soon as" (repairTree marks "soon")
  static const R kEs[] = {{"porque", Relation::Cause, "because"},   {"pues", Relation::Cause, "because"},
                          {"como", Relation::Cause, "because"},     {"cuando", Relation::Time, "when"},
                          {"mientras", Relation::Time, "while"},    {"después", Relation::Time, "after"},
                          {"antes", Relation::Time, "before"},      {"hasta", Relation::Time, "until"},
                          {"si", Relation::Condition, "if"},        {"para", Relation::Purpose, "to"},
                          {"aunque", Relation::Concession, "although"}, {"que", Relation::Complement, "that"}};
  const std::string w = text::lower(m);
  const R* tab = lang == SrcLang::En ? kEn : kEs;
  const size_t n = lang == SrcLang::En ? sizeof(kEn) / sizeof(kEn[0]) : sizeof(kEs) / sizeof(kEs[0]);
  for (size_t i = 0; i < n; ++i)
    if (w == tab[i].w) { rel = tab[i].r; canon = tab[i].c; return true; }
  if (lang == SrcLang::En && w == "in") { rel = Relation::Purpose; canon = "to"; return true; }
  return false;
}

// Connectors at the start of a clause -> canonical English.
std::string canonConnector(SrcLang lang, const std::string& w) {
  if (lang == SrcLang::En) {
    if (in(w, {"and", "but", "or", "so", "then", "yet", "nor", "also", "therefore", "however", "for", "besides",
               "still", "thus", "hence", "moreover", "what-if", "if"})) return w;   // C22: what-if, if (What if ...?)
    return "";
  }
  if (in(w, {"y", "e"})) return "and";
  if (in(w, {"pero", "mas", "sino"})) return "but";
  if (in(w, {"o", "u"})) return "or";
  if (in(w, {"entonces", "luego"})) return "then";
  if (in(w, {"así", "pues"})) return "so";
  if (in(w, {"ni"})) return "nor";
  return "";
}

const char* const kParticles[] = {"away", "back", "up", "down", "off", "out", "in", "over", "on", "around",
                                  "through", "along", "inside", "outside", "by", "past"};   // C22: by, past ("go rolling by")
bool particleWord(const std::string& w) {
  for (const char* p : kParticles)
    if (w == p) return true;
  return false;
}

bool timeNoun(const std::string& l) {
  return in(l, {"morning", "evening", "night", "day", "week", "month", "year", "hour", "minute", "time", "moment",
                "today", "tomorrow", "yesterday", "monday", "tuesday", "wednesday", "thursday", "friday",
                "saturday", "sunday", "summer", "winter", "spring", "autumn", "afternoon", "weekend", "noon",
                "mañana", "tarde", "noche", "día", "semana", "mes", "año", "hora", "momento", "hoy", "ayer"});
}

// "oscuro", "tarde", "imposible" ...: adjectives said of the situation (neuter "it"), as in transfer's table
bool transferImpersonal(const std::string& l) {
  return in(l, {"oscuro", "tarde", "temprano", "frío", "caliente", "verdad", "posible", "imposible", "fácil",
                "difícil", "importante", "necesario", "claro", "extraño", "bueno", "malo", "cierto", "obvio"});
}

int numberValue(const std::string& w) {
  static const std::pair<const char*, int> kNum[] = {
      {"one", 1},      {"two", 2},        {"three", 3},     {"four", 4},     {"five", 5},     {"six", 6},
      {"seven", 7},    {"eight", 8},      {"nine", 9},      {"ten", 10},     {"eleven", 11},  {"twelve", 12},
      {"twenty", 20},  {"thirty", 30},    {"hundred", 100}, {"thousand", 1000},
      // C19: every English number word up to ninety ("twenty-two" below)
      {"thirteen", 13}, {"fourteen", 14}, {"fifteen", 15}, {"sixteen", 16}, {"seventeen", 17}, {"eighteen", 18},
      {"nineteen", 19}, {"forty", 40},    {"fifty", 50},    {"sixty", 60},   {"seventy", 70},  {"eighty", 80},
      {"ninety", 90},
      {"uno", 1},      {"una", 1},        {"un", 1},        {"dos", 2},      {"tres", 3},     {"cuatro", 4},
      {"cinco", 5},    {"seis", 6},       {"siete", 7},     {"ocho", 8},     {"nueve", 9},    {"diez", 10},
      {"once", 11},    {"doce", 12},      {"veinte", 20},   {"cien", 100},   {"ciento", 100}, {"mil", 1000}};
  for (const auto& n : kNum)
    if (w == n.first) return n.second;
  // C19: "twenty-two", "ninety-nine"
  const size_t hy = w.find('-');
  if (hy != std::string::npos && hy > 0) {
    const int a = numberValue(w.substr(0, hy)), b = numberValue(w.substr(hy + 1));
    if (a >= 20 && a <= 90 && a % 10 == 0 && b >= 1 && b <= 9) return a + b;
  }
  bool digits = !w.empty();
  int v = 0;
  for (char c : w) {
    if (c < '0' || c > '9') { digits = false; break; }
    v = v * 10 + (c - '0');
    if (v > 100000) break;
  }
  return digits ? v : 0;
}

// C15: discourse words that open a sentence before a comma ("Oh, ...", "Why, ...", "But, comrades, ..."): their own
// segment, parsed apart from the clause that follows.
bool leadWord(const std::string& w) {
  return in(w, {"oh", "ah", "why", "well", "but", "and", "so", "now", "however", "besides", "yes", "no", "alas",
                "come", "nay", "indeed", "still", "then", "therefore", "meanwhile", "anyway", "hush", "look", "see"});
}

// C15: parentheticals that stand between commas ("..., you know,", "..., as I said;", "..., however;").
const char* const kParentheticals[][4] = {
    {"you", "know", nullptr, nullptr}, {"you", "see", nullptr, nullptr},   {"i", "suppose", nullptr, nullptr},
    {"i", "think", nullptr, nullptr},  {"i", "believe", nullptr, nullptr}, {"as", "i", "said", nullptr},
    {"as", "you", "know", nullptr},    {"however", nullptr, nullptr, nullptr}, {"of", "course", nullptr, nullptr},
    {"i", "am", "sure", nullptr},      {"i", "hope", nullptr, nullptr},    {"though", nullptr, nullptr, nullptr},
    {"too", nullptr, nullptr, nullptr}, {"perhaps", nullptr, nullptr, nullptr}, {"please", nullptr, nullptr, nullptr}};

bool segmentBreak(const Token& t) {
  return t.text == "," || t.text == ";" || t.text == ":" || t.text == "\xE2\x80\x94" || t.text == "\xE2\x80\x93" ||
         t.text == "-" || t.text == "--";
}
bool hardBreak(const Token& t) {
  return t.text == ";" || t.text == ":" || t.text == "\xE2\x80\x94" || t.text == "\xE2\x80\x93" || t.text == "--";
}

// C15: a vocative-like span: [my|your|our|dear]? ADJ* NOUN|PROPN+ (no article, no verb), at most four words.
bool vocativeSpan(const std::vector<Token>& tk, int a, int b) {
  if (b < a || b - a > 3) return false;
  bool noun = false;
  for (int i = a; i <= b; ++i) {
    const Token& t = tk[(size_t)i];
    if (t.upos == "NOUN" || t.upos == "PROPN") { noun = true; continue; }
    if (t.upos == "ADJ" && i < b) continue;
    if (i == a && in(t.lower, {"my", "your", "our", "dear", "o"})) continue;
    if (t.upos == "ADJ" && i == b && i > a && in(tk[(size_t)a].lower, {"my", "your", "our"})) { noun = true; continue; }   // "my dear"
    return false;
  }
  return noun;
}

// C15: the tagger's VERB / ADJ for a word the English lexicon never reads that way ("comrades" VERB, "farmer" ADJ):
// the lexicon's noun (or adjective) reading. True when a tag changed.
bool lexiconVeto(std::vector<Token>& tk, const lex::Lexicon& lx) {
  bool changed = false;
  for (Token& t : tk) {
    if (t.upos != "VERB" && t.upos != "ADJ") continue;
    if (t.lower.empty() || t.text.empty()) continue;
    std::vector<lex::Analysis> an;
    lx.lookup(text::en_key(t.lower), an);
    if (an.empty()) continue;
    bool verb = false, noun = false, adj = false, plural = false;
    for (const lex::Analysis& a : an) {
      const uint8_t pos = lx.lemma(a.lemma).pos;
      verb = verb || pos == feat::Verb;
      adj = adj || pos == feat::Adj || pos == feat::Participle;
      if (pos == feat::Noun) { noun = true; plural = plural || feat::unpack(lx.feature(a.feat)).number == feat::Pl; }
    }
    if (t.upos == "VERB" && !verb && (noun || adj)) {
      t.upos = noun ? "NOUN" : "ADJ";
      t.feats = noun ? nlp::morph::fromString(plural ? "Number=Plur" : "Number=Sing") : 0;
      changed = true;
    } else if (t.upos == "ADJ" && !adj && !verb && noun) {
      t.upos = "NOUN";
      t.feats = nlp::morph::fromString(plural ? "Number=Plur" : "Number=Sing");
      changed = true;
    }
  }
  return changed;
}

// C15: a punctuation token as the root ("But I cannot!", "but if you will come ...,"): the verb (else the last auxiliary)
// becomes the root and takes the punctuation's dependents. Works per tree (segments have their own roots).
void punctRoot(std::vector<Token>& tk) {
  const int n = (int)tk.size();
  for (int r = 0; r < n; ++r) {
    if (tk[(size_t)r].head != 0 || (tk[(size_t)r].upos != "PUNCT" && tk[(size_t)r].upos != "SYM")) continue;
    // the tokens of this tree
    std::vector<int> tree;
    for (int i = 0; i < n; ++i) {
      int x = i, steps = 0;
      while (x >= 0 && x != r && tk[(size_t)x].head > 0 && steps++ <= n) x = tk[(size_t)x].head - 1;
      if (x == r) tree.push_back(i);
    }
    int v = -1;
    for (int i : tree)
      if (i != r && tk[(size_t)i].upos == "VERB" && tk[(size_t)i].head == r + 1) v = i;
    if (v < 0)
      for (int i : tree)
        if (i != r && tk[(size_t)i].upos == "VERB") { v = i; break; }
    if (v < 0)
      for (int i : tree)
        if (i != r && tk[(size_t)i].upos == "AUX") v = i;
    if (v < 0) continue;
    for (int i : tree)
      if (i != v && tk[(size_t)i].head == r + 1) tk[(size_t)i].head = v + 1;
    // an auxiliary that becomes the root takes the subject / negation hung on the old head
    tk[(size_t)v].head = 0;
    tk[(size_t)v].deprel = "root";
    tk[(size_t)r].head = v + 1;
    tk[(size_t)r].deprel = "punct";
    if (tk[(size_t)v].upos == "AUX")
      for (int i : tree)
        if (i != v && (tk[(size_t)i].deprel == "nsubj" || (tk[(size_t)i].deprel == "advmod" && tk[(size_t)i].lower == "not")) &&
            tk[(size_t)i].head > 0 && tk[(size_t)tk[(size_t)i].head - 1].upos == "AUX")
          tk[(size_t)i].head = v + 1;
  }
}

// C15: after a retag the parser may still hang the verb under a noun ("The farmer carries water." -> carries/amod
// of water). A simple clause made only of nominal words around one verb is rebuilt by hand: the verb is the root,
// the last noun before it the subject, the last noun after it the object, determiners / adjectives / numerals
// attach to the next noun, adverbs and punctuation to the verb. Returns false (tree unchanged) for anything else.
bool flatClause(std::vector<Token>& tk, int v) {
  const int n = (int)tk.size();
  if (v < 0 || v >= n || tk[(size_t)v].deprel == "root") return false;
  for (int i = 0; i < n; ++i) {
    if (i == v) continue;
    // C17: a possessive 's after a noun ("The farmer's wife baked bread.")
    if (tk[(size_t)i].upos == "PART" && (tk[(size_t)i].lower == "'s" || tk[(size_t)i].lower == "'") && i > 0 &&
        in(tk[(size_t)i - 1].upos, {"NOUN", "PROPN"}))
      continue;
    // C17: prepositions after the verb ("barked at the stranger all night")
    if (tk[(size_t)i].upos == "ADP" && i > v) continue;
    if (!in(tk[(size_t)i].upos, {"DET", "ADJ", "NOUN", "PROPN", "PRON", "NUM", "PUNCT", "ADV"})) return false;
  }
  auto possTok = [&](int i) { return i >= 0 && i < n && tk[(size_t)i].upos == "PART"; };
  auto nominal = [&](int i) { return in(tk[(size_t)i].upos, {"NOUN", "PROPN", "PRON"}) && !possTok(i + 1); };
  int subj = -1;
  for (int i = 0; i < v; ++i)
    if (nominal(i)) subj = i;
  if (subj < 0) return false;
  // before the verb: an earlier noun group ("Every morning I walk ...") is an oblique of time, not part of the subject
  int subjStart = subj;
  while (subjStart > 0 && !(tk[(size_t)subjStart].upos == "PRON" && nominal(subjStart - 1)) &&
         in(tk[(size_t)subjStart - 1].upos, {"DET", "ADJ", "NUM", "NOUN", "PROPN", "PART"}) &&
         !(tk[(size_t)subjStart - 1].upos == "DET" && subjStart - 2 >= 0 && nominal(subjStart - 2)))
    --subjStart;
  int preHead = -1;   // the head of an earlier group
  for (int i = 0; i < subjStart; ++i)
    if (nominal(i)) preHead = i;
  // after the verb: noun groups, a new one at a preposition or at a determiner after a noun; the first group without a
  // preposition is the object, the others obliques
  std::vector<std::pair<int, int>> groups;
  {
    int a = v + 1;
    for (int i = v + 1; i <= n; ++i) {
      const bool cut = i == n || tk[(size_t)i].upos == "PUNCT" ||
                       (i > a && (tk[(size_t)i].upos == "ADP" || (tk[(size_t)i].upos == "DET" && nominal(i - 1))));
      if (!cut) continue;
      if (i > a) groups.emplace_back(a, i - 1);
      a = tk[(size_t)std::min(i, n - 1)].upos == "PUNCT" || i == n ? i + 1 : i;
    }
  }
  std::vector<int> role(n, 0);   // 1 subject, 2 object, 3 oblique
  std::vector<int> headOf(n, -1);
  bool objTaken = false;
  for (const auto& g : groups) {
    int h = -1;
    for (int i = g.first; i <= g.second; ++i)
      if (nominal(i)) h = i;
    if (h < 0) continue;
    const bool prep = tk[(size_t)g.first].upos == "ADP";
    role[(size_t)h] = !prep && !objTaken ? 2 : 3;
    objTaken = true;   // only the group right after the verb can be its object ("barked at X all night")
    for (int i = g.first; i <= g.second; ++i) headOf[(size_t)i] = h;
  }
  for (int i = 0; i < n; ++i) {
    Token& t = tk[(size_t)i];
    if (i == v) { t.head = 0; t.deprel = "root"; continue; }
    if (i == subj) { t.head = v + 1; t.deprel = "nsubj"; continue; }
    if (i == preHead) { t.head = v + 1; t.deprel = "obl"; continue; }
    if (i < subjStart && preHead >= 0 && t.upos != "PUNCT" && t.upos != "ADV") {
      t.head = preHead + 1;
      t.deprel = t.upos == "DET" ? "det" : t.upos == "NUM" ? "nummod" : nominal(i) ? "compound" : "amod";
      continue;
    }
    if (role[(size_t)i] == 2) { t.head = v + 1; t.deprel = "obj"; continue; }
    if (role[(size_t)i] == 3) { t.head = v + 1; t.deprel = "obl"; continue; }
    if (t.upos == "PUNCT" || t.upos == "ADV") { t.head = v + 1; t.deprel = t.upos == "PUNCT" ? "punct" : "advmod"; continue; }
    if (possTok(i)) { t.head = i; t.deprel = "case"; continue; }            // C17: 's on its possessor
    if (possTok(i + 1)) {                                                   // the possessor on the next noun
      int owner = -1;
      for (int j = i + 2; j < n && owner < 0; ++j)
        if (in(tk[(size_t)j].upos, {"NOUN", "PROPN"})) owner = j;
      if (owner >= 0) { t.head = owner + 1; t.deprel = "nmod"; continue; }
    }
    const int noun = i < v ? subj : headOf[(size_t)i];
    if (noun < 0 || (i < v && i > subj)) { t.head = v + 1; t.deprel = "dep"; continue; }
    t.head = noun + 1;
    t.deprel = t.upos == "ADP" ? "case" : t.upos == "DET" ? "det" : t.upos == "NUM" ? "nummod"
             : nominal(i) ? "compound" : "amod";
  }
  return true;
}


// C24: free relatives with "what" ("The sea would be what the sky is.", "What you have, you keep.", "I like what I see."):
// the parser hangs these clauses in many wrong ways, so a sentence made only of such clauses gets its tree from a
// small grammar. "what" heads its relative clause (acl) and takes the role of the clause it stands in: the predicate
// of "be" (the copula hangs on it) or the object of a verb. Shapes (CONJ, ADV and commas around):
//   SUBJ VG FR | FR , SUBJ VG | FR VG FR, clauses joined by because / since / and / but.
// FR = what SUBJ VG; SUBJ = a pronoun, a name or [det] [adj] noun; VG = auxiliaries, not, a verb. A verb of knowing
// or asking before "what" makes an indirect question (left to the parser). False when the sentence does not fit.
namespace fr {
bool beForm(const nlp::Token& t) {
  return in(t.lower, {"be", "is", "are", "was", "were", "am", "'s", "'re", "'m", "been"});
}
bool auxWord(const nlp::Token& t) {
  return t.upos == "AUX" || in(t.lower, {"would", "could", "should", "will", "shall", "can", "may", "might", "must",
                                         "do", "does", "did", "'d", "'ll"});
}
bool negWord(const nlp::Token& t) { return in(t.lower, {"not", "n't", "never"}); }
// subject at i: end (exclusive) and head, or -1
int subject(const std::vector<nlp::Token>& tk, int i, int& head) {
  const int n = (int)tk.size();
  if (i >= n) return -1;
  const nlp::Token& t = tk[(size_t)i];
  if (t.upos == "PRON" && !in(t.lower, {"what", "which", "who", "whom", "that"})) { head = i; return i + 1; }
  if (t.upos == "PROPN") { head = i; return i + 1; }
  int j = i;
  if (t.upos == "DET" || t.upos == "PRON") ++j;   // "the", "my"
  while (j < n && tk[(size_t)j].upos == "ADJ") ++j;
  if (j < n && tk[(size_t)j].upos == "NOUN") { head = j; return j + 1; }
  return -1;
}
// verb group at i: end (exclusive), head (the verb, else a form of be, else the last auxiliary), whether it has a verb
int verbGroup(const std::vector<nlp::Token>& tk, int i, int& head, bool& verb, bool& be) {
  const int n = (int)tk.size();
  int j = i;
  head = -1;
  verb = be = false;
  int lastAux = -1, beTok = -1;
  while (j < n) {
    const nlp::Token& t = tk[(size_t)j];
    if (t.upos == "VERB" && !beForm(t)) { if (verb) break; verb = true; head = j; ++j; continue; }
    if (beForm(t)) { beTok = j; ++j; continue; }
    if (auxWord(t)) { if (verb) break; lastAux = j; ++j; continue; }
    if (negWord(t) || in(t.lower, {"really", "always", "just", "still", "also", "ever"})) { ++j; continue; }
    break;
  }
  if (!verb) {
    if (beTok >= 0) { head = beTok; be = true; }
    else head = lastAux;
  }
  if (head < 0) return -1;
  return j;
}
struct Clause { int head = -1; int end = -1; };

void hang(std::vector<nlp::Token>& tk, int k, int head, const char* rel) {
  tk[(size_t)k].head = head + 1;
  tk[(size_t)k].deprel = rel;
}

// the relative clause of "what" at w: SUBJ VG after it; returns the end or -1
int relative(std::vector<nlp::Token>& tk, int w, bool apply) {
  int sh = -1, vh = -1;
  bool verb = false, be = false;
  const int e1 = subject(tk, w + 1, sh);
  if (e1 < 0) return -1;
  const int e2 = verbGroup(tk, e1, vh, verb, be);
  if (e2 < 0) return -1;
  if (!apply) return e2;
  hang(tk, vh, w, "acl");
  if (be) { tk[(size_t)vh].upos = "VERB"; tk[(size_t)vh].lemma = "be"; }
  for (int k = w + 1; k < e1; ++k) hang(tk, k, k == sh ? vh : sh, k == sh ? "nsubj" : tk[(size_t)k].upos == "ADJ" ? "amod" : "det");
  for (int k = e1; k < e2; ++k) {
    if (k == vh) continue;
    hang(tk, k, vh, negWord(tk[(size_t)k]) || tk[(size_t)k].upos == "ADV" || tk[(size_t)k].upos == "PART" ? "advmod" : "aux");
  }
  return e2;
}

// one clause at p (see the shapes above); apply = write the tree
bool clause(std::vector<nlp::Token>& tk, int p, Clause& out, bool apply, bool& anyFr) {
  const int n = (int)tk.size();
  if (p >= n) return false;
  // FR , SUBJ VG  (fronted)   and   FR VG FR
  if (tk[(size_t)p].lower == "what") {
    const int e1 = relative(tk, p, false);
    if (e1 < 0) return false;
    if (e1 < n && tk[(size_t)e1].text == ",") {
      int sh = -1, vh = -1;
      bool verb = false, be = false;
      const int e2 = subject(tk, e1 + 1, sh);
      if (e2 < 0) return false;
      const int e3 = verbGroup(tk, e2, vh, verb, be);
      if (e3 < 0) return false;
      anyFr = true;
      if (apply) {
        relative(tk, p, true);
        const int hd = verb ? vh : be ? p : vh;   // "it isn't": what is the predicate; "you keep": object
        if (hd == p) {
          hang(tk, sh, p, "nsubj");
          for (int k = e2; k < e3; ++k)
            hang(tk, k, p, k == vh ? "cop" : negWord(tk[(size_t)k]) || tk[(size_t)k].upos == "ADV" ? "advmod" : "aux");
        } else {
          if (!verb) tk[(size_t)vh].upos = "AUX";   // "it would": an elliptic verb group
          hang(tk, sh, vh, "nsubj");
          for (int k = e2; k < e3; ++k)
            if (k != vh) hang(tk, k, vh, negWord(tk[(size_t)k]) || tk[(size_t)k].upos == "ADV" ? "advmod" : "aux");
          hang(tk, p, vh, "obj");
        }
        hang(tk, e1, hd, "punct");
        for (int k = e1 + 1; k < e2; ++k)
          if (k != sh) hang(tk, k, sh, tk[(size_t)k].upos == "ADJ" ? "amod" : "det");
      }
      out.head = verb ? vh : be ? p : vh;
      out.end = e3;
      return true;
    }
    int vh = -1;
    bool verb = false, be = false;
    const int e2 = verbGroup(tk, e1, vh, verb, be);
    if (e2 < 0 || !be || verb || e2 >= n || tk[(size_t)e2].lower != "what") return false;
    const int e3 = relative(tk, e2, false);
    if (e3 < 0) return false;
    anyFr = true;
    if (apply) {
      relative(tk, p, true);
      relative(tk, e2, true);
      hang(tk, p, e2, "nsubj");
      for (int k = e1; k < e2; ++k)
        hang(tk, k, e2, k == vh ? "cop" : negWord(tk[(size_t)k]) || tk[(size_t)k].upos == "ADV" ? "advmod" : "aux");
    }
    out.head = e2;
    out.end = e3;
    return true;
  }
  // SUBJ VG FR
  int sh = -1, vh = -1;
  bool verb = false, be = false;
  const int e1 = subject(tk, p, sh);
  if (e1 < 0) return false;
  const int e2 = verbGroup(tk, e1, vh, verb, be);
  if (e2 < 0 || e2 >= n || tk[(size_t)e2].lower != "what") return false;
  if (verb && in(tk[(size_t)vh].lemma.empty() ? tk[(size_t)vh].lower : tk[(size_t)vh].lemma,
                 {"know", "wonder", "ask", "tell", "see", "guess", "understand", "remember", "forget", "learn", "show",
                  "explain", "decide", "care", "mind", "say", "think", "hear", "find", "notice", "imagine"}))
    return false;   // an indirect question ("I know what you want")
  if (!verb && !be) return false;
  const int e3 = relative(tk, e2, false);
  if (e3 < 0) return false;
  anyFr = true;
  if (apply) {
    relative(tk, e2, true);
    const int hd = verb ? vh : e2;
    hang(tk, sh, hd, "nsubj");
    for (int k = p; k < e1; ++k)
      if (k != sh) hang(tk, k, sh, tk[(size_t)k].upos == "ADJ" ? "amod" : "det");
    for (int k = e1; k < e2; ++k) {
      if (k == hd) continue;
      hang(tk, k, hd, (!verb && k == vh) ? "cop" : negWord(tk[(size_t)k]) || tk[(size_t)k].upos == "ADV" ? "advmod" : "aux");
    }
    if (verb) hang(tk, e2, vh, "obj");
  }
  out.head = verb ? vh : e2;
  out.end = e3;
  return true;
}

bool sentence(std::vector<nlp::Token>& tk, bool apply) {
  const int n = (int)tk.size();
  int p = 0;
  std::vector<int> pre;   // leading and / but, adverbs, commas
  while (p < n && (tk[(size_t)p].text == "," || (tk[(size_t)p].upos == "CCONJ" && pre.empty()) ||
                   (tk[(size_t)p].upos == "ADV" && tk[(size_t)p].lower != "what")))
    pre.push_back(p++);
  bool anyFr = false;
  Clause c1;
  if (!clause(tk, p, c1, apply, anyFr)) return false;
  int root = c1.head;
  int q = c1.end;
  std::vector<std::pair<int, int>> more;   // (connector token, clause head)
  while (q < n && tk[(size_t)q].upos != "PUNCT") {
    const nlp::Token& t = tk[(size_t)q];
    if (!in(t.lower, {"because", "since", "and", "but"})) return false;
    Clause c2;
    if (!clause(tk, q + 1, c2, apply, anyFr)) return false;
    more.emplace_back(q, c2.head);
    q = c2.end;
  }
  std::vector<int> post;
  while (q < n && tk[(size_t)q].upos == "PUNCT" && tk[(size_t)q].text != ",") post.push_back(q++);
  if (q != n || !anyFr || post.empty()) return false;
  if (!apply) return true;
  tk[(size_t)root].head = 0;
  tk[(size_t)root].deprel = "root";
  for (int k : pre)
    hang(tk, k, root, tk[(size_t)k].text == "," ? "punct" : tk[(size_t)k].upos == "CCONJ" ? "cc" : "advmod");
  for (const auto& m : more) {
    const bool sub = in(tk[(size_t)m.first].lower, {"because", "since"});
    hang(tk, m.second, root, sub ? "advcl" : "conj");
    hang(tk, m.first, m.second, sub ? "mark" : "cc");
    if (sub) tk[(size_t)m.first].upos = "SCONJ";
  }
  for (int k : post) hang(tk, k, root, "punct");
  return true;
}

// C24: a noun with its relative clause and nothing else ("a boat that carries me", "the girls who sing"):
// the parser reads the relative clause as the main clause; the noun is the fragment's head, the clause its relative
// clause (the relative word the object when the clause has its own subject, else the subject)
bool nounRelative(std::vector<nlp::Token>& tk, bool apply) {
  const int n = (int)tk.size();
  int i = 0;
  if (i < n && tk[(size_t)i].upos == "DET") ++i;
  while (i < n && tk[(size_t)i].upos == "ADJ") ++i;
  if (i >= n || tk[(size_t)i].upos != "NOUN") return false;
  const int noun = i;
  const int rel = i + 1;
  if (rel >= n || !in(tk[(size_t)rel].lower, {"that", "which", "who"})) return false;
  int sh = -1;
  const int e1 = subject(tk, rel + 1, sh);
  int vh = -1;
  bool verb = false, be = false;
  const int e2 = verbGroup(tk, e1 >= 0 ? e1 : rel + 1, vh, verb, be);
  if (e2 < 0 || !verb) return false;
  int q = e2;
  while (q < n && tk[(size_t)q].upos == "PUNCT") ++q;
  if (q != n) return false;
  if (!apply) return true;
  for (int k = 0; k < noun; ++k) hang(tk, k, noun, tk[(size_t)k].upos == "ADJ" ? "amod" : "det");
  tk[(size_t)noun].head = 0;
  tk[(size_t)noun].deprel = "root";
  hang(tk, vh, noun, "acl");
  tk[(size_t)rel].upos = "PRON";
  hang(tk, rel, vh, e1 >= 0 ? "obj" : "nsubj");
  if (e1 >= 0) {
    hang(tk, sh, vh, "nsubj");
    for (int k = rel + 1; k < e1; ++k)
      if (k != sh) hang(tk, k, sh, tk[(size_t)k].upos == "ADJ" ? "amod" : "det");
  }
  for (int k = e1 >= 0 ? e1 : rel + 1; k < e2; ++k)
    if (k != vh) hang(tk, k, vh, negWord(tk[(size_t)k]) || tk[(size_t)k].upos == "ADV" || tk[(size_t)k].upos == "PART" ? "advmod" : "aux");
  for (int k = e2; k < n; ++k) hang(tk, k, noun, "punct");
  return true;
}
}  // namespace fr

// C24: "What the cat won't eat, the dog will.": an elliptic verb group whose object is a free relative takes the
// relative clause's verb; with "be" the free relative is the predicate
void freeRelativeEllipsis(SemFrame& f) {
  for (SemSub& sb : f.subordinate)
    for (SemFrame& x : sb.frame) freeRelativeEllipsis(x);
  const bool auxOnly = f.hasPred && f.pred.complementVerb.empty() &&
                       in(f.pred.lemma, {"would", "will", "could", "should"});
  if (!f.hasPred || !(f.pred.ellipsis || auxOnly) || !f.hasObject || f.object.pronLemma != "what" ||
      f.object.relative.empty())
    return;
  const SemFrame& r = f.object.relative[0];
  if (!r.hasPred || r.pred.lemma.empty()) return;
  if (f.pred.lemma == "would") f.pred.mood = SrcMood::Conditional;
  else if (f.pred.lemma == "will") f.pred.tense = Tense::Future;
  else if (f.pred.lemma == "could") { f.pred.modality = Modality::Can; f.pred.pastModal = true; }
  else if (f.pred.lemma == "should") f.pred.modality = Modality::Should;
  f.pred.ellipsis = false;
  f.pred.lemma = r.pred.lemma;
  if (r.pred.lemma == "be" || r.copula) {
    f.pred.lemma = "be";
    f.copula = true;
    f.predicative.push_back(f.object);
    f.hasObject = false;
    f.object = SemNP{};
  }
}
}  // namespace

// ---- names ---------------------------------------------------------------------------------------------------------
const char* kindName(Kind k) {
  switch (k) {
    case Kind::Decl: return "decl"; case Kind::Yn: return "yn"; case Kind::Wh: return "wh";
    case Kind::Imp: return "imp"; case Kind::Excl: return "excl"; case Kind::Frag: return "frag";
    case Kind::Nonverbal: return "nonverbal"; case Kind::Song: return "song";
  }
  return "?";
}
const char* tenseName(Tense t) { return t == Tense::Past ? "past" : t == Tense::Future ? "future" : "present"; }
const char* aspectName(Aspect a) {
  return a == Aspect::Progressive ? "progressive" : a == Aspect::Perfect ? "perfect" : "simple";
}
const char* modalityName(Modality m) {
  switch (m) {
    case Modality::None: return "none"; case Modality::Can: return "can"; case Modality::Must: return "must";
    case Modality::May: return "may"; case Modality::Want: return "want"; case Modality::Should: return "should";
    case Modality::Will: return "will"; case Modality::Let: return "let";
  }
  return "?";
}
const char* relationName(Relation r) {
  switch (r) {
    case Relation::Cause: return "cause"; case Relation::Time: return "time"; case Relation::Condition: return "condition";
    case Relation::Purpose: return "purpose"; case Relation::Concession: return "concession";
    case Relation::Relative: return "relative"; case Relation::Complement: return "complement";
    case Relation::Result: return "result"; case Relation::Manner: return "manner"; case Relation::Coord: return "coord";
  }
  return "?";
}
const char* roleName(Role r) {
  switch (r) {
    case Role::None: return "none"; case Role::Subject: return "subject"; case Role::Object: return "object";
    case Role::IndirectObject: return "iobj"; case Role::Oblique: return "oblique"; case Role::Predicate: return "predicate";
    case Role::Adverb: return "adverb"; case Role::Determiner: return "det";
  }
  return "?";
}

namespace {
std::string npText(const SemNP& n) {
  std::string s;
  if (n.isPronoun) {
    s = (n.pronLemma.empty() ? std::string("pron") : n.pronLemma) + "(p" + std::to_string(n.pron.person) +
        (n.pron.number == 2 ? "pl" : n.pron.number == 1 ? "sg" : "") + ")";
  } else {
    s = n.head;
    if (n.isName) s += "(name)";
    if (n.number == 2) s += "(pl)";
  }
  if (!n.determiner.empty() && n.determiner != "the" && n.determiner != "a") s = n.determiner + " " + s;
  if (n.negative) s = "NEG:" + s;
  if (n.interrogative) s = "WH:" + s;
  for (const SemAdj& a : n.adjectives) s += "+" + a.lemma;
  if (!n.numeral.empty()) s = n.numeral + " " + s;
  for (const SemNP& p : n.possessor) s += " poss:" + npText(p);
  for (const SemNP& g : n.genitive) s += " of:" + npText(g);
  for (const SemNP& c : n.coord) s += " " + (n.coordConj.empty() ? std::string("and") : n.coordConj) + " " + npText(c);
  if (!n.relative.empty()) s += " [rel:" + describe(n.relative[0]) + "]";
  return s;
}
}  // namespace

std::string describe(const SemFrame& f) {
  std::string s = kindName(f.type);
  if (f.hasPred) {
    s += " pred=" + f.pred.lemma;
    if (!f.pred.particle.empty()) s += "_" + f.pred.particle;
    if (!f.pred.complementVerb.empty()) s += "+" + f.pred.complementVerb;
    s += std::string(" tense=") + tenseName(f.pred.tense) + " aspect=" + aspectName(f.pred.aspect) +
         " mod=" + modalityName(f.pred.modality);
    if (f.pred.voice == Voice::Passive) s += " passive";
    if (f.pred.ellipsis) s += " ellipsis";
  }
  if (f.negative) s += " neg";
  if (f.existential) s += " exist";
  if (f.hasSubject) s += " subj=" + npText(f.subject);
  if (f.hasObject) s += " obj=" + npText(f.object);
  if (f.hasIndirect) s += " iobj=" + npText(f.indirectObject);
  for (const SemNP& p : f.predicative) s += " prednp=" + npText(p);
  for (const SemAdj& a : f.predAdj) {
    s += " predadj=";
    for (const std::string& v : a.adverbs) s += v + "_";
    s += a.lemma;
  }
  for (const SemOblique& o : f.obliques) s += " obl=" + (o.prep.empty() ? std::string("-") : o.prep) + ":" + npText(o.np);
  for (const SemAdverb& a : f.adverbs) s += " adv=" + a.lemma;
  for (const SemNP& v : f.vocatives) s += " voc=" + npText(v);
  for (const std::string& c : f.connectors) s += " conn=" + c;
  for (const std::string& i : f.interjections) s += " intj=" + i;
  if (f.type == Kind::Wh) s += " wh=" + f.wh.word + ":" + roleName(f.wh.role);
  for (const SemSub& sb : f.subordinate) {
    s += std::string(" sub=") + relationName(sb.relation) + ":" + sb.marker + (sb.before ? "<" : ">");
    if (!sb.frame.empty()) s += "{" + describe(sb.frame[0]) + "}";
  }
  return s;
}

std::string describe(const SemSentence& s) {
  std::string out;
  for (const Unit& u : s.units) {
    if (!out.empty()) out += " | ";
    if (u.type == Unit::Phrase) out += "phrase(" + u.phrase.pattern + "=>" + u.phrase.latin + ")";
    else out += (u.vocative ? "voc:" : "") + describe(u.frame);
    if (!u.sepAfter.empty()) out += " " + u.sepAfter;
  }
  return out;
}

// ---- construction --------------------------------------------------------------------------------------------------
FrameBuilder::FrameBuilder(SrcLang lang, const nlp::Pipeline* pipeline, const lex::Lexicon* srcLex,
                           const curated::CuratedData& cd)
    : lang_(lang), nlp_(pipeline), lex_(srcLex), cd_(cd) {
  {
    // English: phrasebook_en_la.tsv + contractions_en.tsv; Spanish (C13): phrasebook_es_la.tsv + contractions_es.tsv
    const std::vector<curated::PairEntry>& contr = lang == SrcLang::En ? cd.contractions() : cd.contractionsEs();
    book_.build(lang == SrcLang::En ? cd.phrasebook() : cd.phrasebookEs(), contr);
    for (const curated::PairEntry& c : contr) {
      std::vector<std::string> ws;
      size_t a = 0;
      const std::string e = text::lower(c.b);
      for (;;) {
        size_t b = e.find(' ', a);
        std::string w = e.substr(a, b == std::string::npos ? std::string::npos : b - a);
        if (!w.empty()) ws.push_back(w);
        if (b == std::string::npos) break;
        a = b + 1;
      }
      contractions_.emplace_back(nlp::normalise(c.a), std::move(ws));
    }
    std::stable_sort(contractions_.begin(), contractions_.end(),
                     [](const auto& x, const auto& y) { return x.first < y.first; });
  }
}

// ---- tokenisation with contractions -----------------------------------------------------------------------------------
void FrameBuilder::tokenize(std::string_view sentence, std::vector<Token>& out) const {
  out.clear();
  std::vector<Token> raw;
  nlp::Tokenizer tk(lang_ == SrcLang::En ? nlp::Lang::En : nlp::Lang::Es);
  tk.tokenize(sentence, raw);
  auto push = [&](const Token& orig, const std::string& w, bool cap) {
    Token t;
    t.text = w;
    if (cap && !t.text.empty() && t.text[0] >= 'a' && t.text[0] <= 'z') t.text[0] = (char)(t.text[0] - 32);
    if (t.text == "i") t.text = "I";
    t.lower = nlp::normalise(w);
    t.start = orig.start;
    t.end = orig.end;
    out.push_back(std::move(t));
  };
  for (const Token& t : raw) {
    const std::string low = nlp::normalise(t.text);
    const bool cap = upper(t.text);
    if (lang_ == SrcLang::En && low == "o'clock") {
      out.push_back(t);
      out.back().lower = low;
      continue;
    }
    if (lang_ == SrcLang::En) {
      auto it = std::lower_bound(contractions_.begin(), contractions_.end(), low,
                                 [](const auto& e, const std::string& k) { return e.first < k; });
      if (it != contractions_.end() && it->first == low) {
        for (size_t k = 0; k < it->second.size(); ++k) push(t, it->second[k], cap && k == 0);
        continue;
      }
      // generic suffixes
      auto ends = [&](const char* suf) {
        const size_t n = std::strlen(suf);
        return low.size() > n && low.compare(low.size() - n, n, suf) == 0;
      };
      const std::string orig = t.text;
      auto base = [&](size_t cut) { return orig.substr(0, orig.size() - cut); };
      // apostrophe length in the original (curly quote = 3 bytes)
      const size_t apos = orig.find("\xE2\x80\x99") != std::string::npos ? 3 : 1;
      if (ends("n't")) { push(t, base(2 + apos), false); push(t, "not", false); continue; }
      if (ends("'re")) { push(t, base(2 + apos), false); push(t, "are", false); continue; }
      if (ends("'ve")) { push(t, base(2 + apos), false); push(t, "have", false); continue; }
      if (ends("'ll")) { push(t, base(2 + apos), false); push(t, "will", false); continue; }
      if (ends("'d")) { push(t, base(1 + apos), false); push(t, "would", false); continue; }
      if (ends("'m")) { push(t, base(1 + apos), false); push(t, "am", false); continue; }
      if (ends("'s")) {
        const std::string b = low.substr(0, low.size() - 2);
        if (in(b, {"he", "she", "it", "that", "there", "here", "what", "who", "where", "how", "when", "why",
                   "this", "everyone", "everybody", "nobody", "someone", "something", "everything", "nothing"})) {
          push(t, base(1 + apos), false);
          push(t, "is", false);
        } else {
          push(t, base(1 + apos), false);
          push(t, "'s", false);
        }
        continue;
      }
      if (ends("s'") && low.size() > 3) { push(t, base(apos), false); push(t, "'s", false); continue; }
      out.push_back(t);
      out.back().lower = low;
      continue;
    }
    // Spanish: the contraction table (contractions_es.tsv: del, al, pa' ...), then enclitic pronouns on verbs
    {
      auto it = std::lower_bound(contractions_.begin(), contractions_.end(), low,
                                 [](const auto& e, const std::string& k) { return e.first < k; });
      if (it != contractions_.end() && it->first == low) {
        for (size_t k = 0; k < it->second.size(); ++k) push(t, it->second[k], cap && k == 0);
        continue;
      }
      if (contractions_.empty() && (low == "del" || low == "al")) {   // no table: the two obligatory ones
        push(t, low == "del" ? "de" : "a", cap);
        push(t, "el", false);
        continue;
      }
    }
    bool split = false;
    if (lex_ && low.size() > 3) {
      std::vector<lex::Analysis> an;
      lex_->lookup(text::es_key(low), an);
      // a known word is split when the lexicon reads it as a pronominal verb form ("siéntate" <- sentarse, "vete" <-
      // irse) or, at the start of the sentence, when its base is a one-syllable imperative ("Dame", "Dime", "Hazlo");
      // a one-word phrasebook pattern ("ándale") is never split (C13)
      bool tryKnown = false;
      bool firstWord = true;
      for (const Token& o : out)
        if (o.text != "\xC2\xA1" && o.text != "\xC2\xBF" && o.text != "\"" && o.text != "-") firstWord = false;
      bool nonVerb = false;
      for (const lex::Analysis& x : an) {
        const lex::Lemma l = lex_->lemma(x.lemma);
        const std::string h = text::lower(l.head);
        if (l.pos == feat::Verb && h.size() > 4 && h.compare(h.size() - 2, 2, "se") == 0) tryKnown = true;
        if (l.pos != feat::Verb) nonVerb = true;
      }
      if (nonVerb) tryKnown = false;   // "vela" (noun; velarse) stays whole
      bool phraseWord = false;
      for (const curated::PhraseEntry& e : cd_.phrasebookEs())
        if (e.pattern.find(' ') == std::string::npos) {
          const std::string pat = text::lower(e.pattern);
          size_t a = 0;
          for (;;) {   // alternatives "perdona|perdóname"
            const size_t b = pat.find('|', a);
            if (pat.substr(a, b == std::string::npos ? std::string::npos : b - a) == low) phraseWord = true;
            if (b == std::string::npos) break;
            a = b + 1;
          }
        }
      if ((an.empty() || tryKnown || firstWord) && !phraseWord) {
        std::vector<std::string> kCl;
        for (const curated::CliticEntry& ce : cd_.clitics()) kCl.push_back(ce.form);
        if (kCl.empty()) kCl = {"me", "te", "se", "nos", "os", "lo", "la", "los", "las", "le", "les"};
        std::stable_sort(kCl.begin(), kCl.end(), [](const std::string& x, const std::string& y) { return x.size() > y.size(); });
        std::string b = low, bestBase;
        std::vector<std::string> cl, bestCl;
        for (int round = 0; round < 2; ++round) {   // the deepest split whose base is a verb wins (dámelo -> da me lo)
          bool cut = false;
          for (const std::string& c : kCl) {
            const size_t n = c.size();
            if (b.size() > n + 1 && b.compare(b.size() - n, n, c) == 0) {
              b = b.substr(0, b.size() - n);
              cl.insert(cl.begin(), c);
              cut = true;
              break;
            }
          }
          if (!cut) break;
          std::vector<lex::Analysis> a2;
          lex_->lookup(text::es_key(b), a2);
          if (a2.empty()) lex_->lookup(text::es_bare(b), a2);
          bool ok = false;
          for (const lex::Analysis& x : a2) {
            if (lex_->lemma(x.lemma).pos != feat::Verb) continue;
            const uint8_t md = feat::unpack(lex_->feature(x.feat)).mood;
            // an unknown word: any verb base; a known one: an imperative, infinitive or gerund base
            if (an.empty() || md == feat::Imperative || md == feat::Infinitive || md == feat::Gerund ||
                (md == feat::Subjunctive && tryKnown))
              ok = true;
          }
          if (ok && !an.empty() && !tryKnown) {   // sentence start: one-syllable imperatives only (da, di, haz ...)
            static const char* const kShort[] = {"da", "di", "haz", "pon", "ten", "ve", "ven", "sal", "sé", "dé"};
            ok = false;
            for (const char* k : kShort) ok = ok || b == k;
          }
          if (ok) { split = true; bestBase = b; bestCl = cl; }
        }
        if (split) {
          // the written stress of the enclitic form goes with the clitics: "pása|me" -> "pasa me", "siénta|te" ->
          // "sienta te" (one-syllable bases keep theirs: "dé")
          std::string base = bestBase;
          int vowels = 0;
          for (size_t q = 0; q < base.size(); ++q)
            if (std::strchr("aeiou", base[q]) || (unsigned char)base[q] == 0xC3) ++vowels;
          if (vowels > 1 || (base != "d\xC3\xA9" && base != "s\xC3\xA9"))   // "dé", "sé" keep theirs
            for (const char* const* acc = kAcute; *acc; acc += 2)
              for (size_t at; (at = base.find(acc[0])) != std::string::npos;) base.replace(at, 2, acc[1]);
          push(t, base, cap);
          for (const std::string& c : bestCl) push(t, c, false);
        }
      }
    }
    if (!split) {
      out.push_back(t);
      out.back().lower = low;
    }
  }
  if (lang_ == SrcLang::En) contractionContext(sentence, out);
}

// C19: contractions whose expansion depends on the next word: "'d" + past participle / "better" = had ("He'd seen",
// "You'd better go"), "'s" + got / been = has ("He's got a dog"), and "have got" (possession) = have ("I've got a
// hat", "Have you got a pen?", "We've got to go" = have to).
void FrameBuilder::contractionContext(std::string_view sentence, std::vector<Token>& out) const {
  auto endsWith = [](const std::string& w, const std::string& suf) {
    return w.size() >= suf.size() && w.compare(w.size() - suf.size(), suf.size(), suf) == 0;
  };
  auto fromApostrophe = [&](const Token& t, const char* letter) {
    if (t.start < 0 || t.end > (int)sentence.size() || t.end <= t.start) return false;
    const std::string w = text::lower(std::string(sentence.substr((size_t)t.start, (size_t)(t.end - t.start))));
    return endsWith(w, std::string("'") + letter) || endsWith(w, std::string("\xE2\x80\x99") + letter);
  };
  auto skipAdverbs = [&](size_t j) {
    while (j < out.size() && in(out[j].lower, {"never", "ever", "already", "just", "not", "always", "only", "once",
                                               "really", "all", "still", "almost", "nearly", "often"}))
      ++j;
    return j;
  };
  auto participle = [&](const std::string& w) {
    if (w == "been" || w == "better") return true;
    if (!lex_) return false;
    bool present = false;
    const std::string v = en::verbOfForm(*lex_, w, &present);
    if (!v.empty() && !present) {
      std::vector<lex::Analysis> an;   // a form that is also a present ("come", "run", "put") stays "would"
      lex_->lookup(text::en_key(w), an);
      for (const lex::Analysis& a : an) {
        const lex::Lemma l = lex_->lemma(a.lemma);
        const feat::Features f = feat::unpack(lex_->feature(a.feat));
        if (l.pos == feat::Verb && (f.tense == feat::Present || f.mood == feat::Infinitive) && text::lower(l.head) != v)
          return false;
        if (l.pos == feat::Verb && text::lower(l.head) == w) return false;
      }
      return true;
    }
    return w.size() > 4 && endsWith(w, "ed") && !en::knownBase(*lex_, w, "VERB").empty();
  };
  for (size_t i = 0; i < out.size(); ++i) {
    Token& t = out[i];
    const size_t j = skipAdverbs(i + 1);
    if (t.lower == "would" && fromApostrophe(t, "d") && j < out.size() && participle(out[j].lower)) {
      t.text = "had";
      t.lower = "had";
    } else if (t.lower == "is" && fromApostrophe(t, "s") && j < out.size() && in(out[j].lower, {"got", "been"})) {
      t.text = "has";
      t.lower = "has";
    }
  }
  // "had better" + verb = should ("You'd better go home." -> Domum īre dēbēs)
  for (size_t i = 0; i + 2 < out.size(); ++i)
    if (out[i].lower == "had" && out[i + 1].lower == "better" &&
        !in(out[i + 2].text, {".", ",", "!", "?", ";", "than"})) {
      out[i].text = "should";
      out[i].lower = "should";
      out.erase(out.begin() + (long)i + 1);
    }
  for (size_t i = 1; i + 1 < out.size(); ++i) {
    if (out[i].lower != "got") continue;
    const bool haveBefore = in(out[i - 1].lower, {"have", "has", "had"}) ||
                            (i >= 2 && in(out[i - 2].lower, {"have", "has", "had"}) &&
                             in(out[i - 1].lower, {"you", "we", "they", "he", "she", "it", "i", "not"}));
    const std::string& nx = out[i + 1].text;
    if (!haveBefore || nx.empty() || nx == "." || nx == "," || nx == "!" || nx == "?" || nx == ";") continue;
    out.erase(out.begin() + (long)i);
    --i;
  }
}

// ---- lemmas ----------------------------------------------------------------------------------------------------------
std::string FrameBuilder::lemmaOf(const Token& t) const {
  const std::string rule = nlp::ruleLemma(lang_ == SrcLang::En ? nlp::Lang::En : nlp::Lang::Es, t);
  if (t.upos == "PROPN") return t.text;
  if (in(t.upos, {"PUNCT", "SYM", "ADP", "CCONJ", "SCONJ", "PART", "DET", "PRON", "INTJ"})) return t.lower;
  // C17: English nouns used only in the plural are their own lemma ("clothes" is not "cloth" or "clothe")
  if (lang_ == SrcLang::En && t.upos == "NOUN" &&
      in(t.lower, {"clothes", "scissors", "trousers", "spectacles", "riches", "goods", "wages", "stairs", "tongs",
                   "pincers", "oats", "ashes", "thanks", "remains", "surroundings", "belongings", "manners"}))
    return t.lower;
  if (!lex_) return rule;
  std::vector<lex::Analysis> an;
  const std::string key = lang_ == SrcLang::En ? text::en_key(t.lower) : text::es_key(t.lower);
  lex_->lookup(key, an);
  if (an.empty() && lang_ == SrcLang::Es) lex_->lookup(text::es_bare(t.lower), an);
  if (an.empty() && lang_ == SrcLang::En) {   // C17: a form the lexicon does not list: a known base word
    const std::string b = en::knownBase(*lex_, t.lower, t.upos);
    if (!b.empty()) return b;
  }
  // C19: the rule lemmatiser doubles a final consonant of a past that is its own lemma ("set" -> "sett")
  if (an.empty() && lang_ == SrcLang::En && (t.upos == "VERB" || t.upos == "AUX") && !t.lower.empty() &&
      rule == t.lower + t.lower.back())
    return t.lower;
  if (an.empty()) return rule;
  // C15: the rule lemmatiser may cut a word to a non-word ("sting" -> "st"); the lexicon knows the form itself
  bool ruleKnown = rule == t.lower || lang_ != SrcLang::En;
  if (!ruleKnown) {   // the rule lemma must be a lemma of the tagged part of speech ("st" is an abbreviation)
    std::vector<lex::Analysis> ra;
    lex_->lookup(text::en_key(rule), ra);
    const uint8_t want = t.upos == "VERB" || t.upos == "AUX" ? feat::Verb : t.upos == "NOUN" ? feat::Noun
                       : t.upos == "ADJ" ? feat::Adj : 0;
    for (const lex::Analysis& a : ra) {
      const lex::Lemma l = lex_->lemma(a.lemma);
      if (text::lower(l.head) == rule && (!want || l.pos == want)) ruleKnown = true;
    }
  }
  auto posOk = [&](uint8_t pos) {
    if (t.upos == "NOUN") return pos == feat::Noun;
    if (t.upos == "VERB" || t.upos == "AUX") return pos == feat::Verb;
    if (t.upos == "ADJ") return pos == feat::Adj || pos == feat::Participle || pos == feat::Verb;
    if (t.upos == "ADV") return pos == feat::Adv || pos == feat::Adj;
    if (t.upos == "NUM") return pos == feat::Num || pos == feat::Noun || pos == feat::Adj;
    return true;
  };
  const uint32_t tense = fget(t, nlp::morph::TenseShift), vf = fget(t, nlp::morph::VerbFormShift),
                 num = fget(t, nlp::morph::NumberShift), pers = fget(t, nlp::morph::PersonShift);
  bool inflected = false;
  if (t.upos == "VERB" || t.upos == "AUX")
    inflected = tense == nlp::morph::TensePast || vf == nlp::morph::VfPart || vf == nlp::morph::VfGer ||
                (pers == nlp::morph::Pers3 && num == nlp::morph::NumSing) || lang_ == SrcLang::Es;
  else if (t.upos == "NOUN") inflected = num == nlp::morph::NumPlur;
  else if (t.upos == "ADJ") {
    const std::string& w = t.lower;
    inflected = lang_ == SrcLang::Es || (w.size() > 4 && (w.compare(w.size() - 2, 2, "er") == 0 ||
                                                           w.compare(w.size() - 3, 3, "est") == 0));
  }
  // analyses whose features agree with the tagger's (person, number, mood) are preferred: "sé" = saber 1sg, not
  // the imperative of ser
  const uint32_t tmood = fget(t, nlp::morph::MoodShift);
  const bool es = lang_ == SrcLang::Es;
  // Spanish (C13): an adjective form that the lexicon also lists as an adjective lemma keeps it ("extraño", not
  // extrañar; "profundo", not profundar)
  bool esAdjLemma = false;
  if (es && t.upos == "ADJ")
    for (const lex::Analysis& a : an)
      if (lex_->lemma(a.lemma).pos == feat::Adj) esAdjLemma = true;
  auto agree = [&](const lex::Analysis& a) {
    const feat::Features f = feat::unpack(lex_->feature(a.feat));
    int sc = 0;   // only disagreements count (a lemma's own headword analysis often has no features)
    if (pers && f.person && f.person != pers) sc -= 2;
    if (num && f.number && f.number != (num == nlp::morph::NumPlur ? feat::Pl : feat::Sg)) sc -= 1;
    if (f.mood == feat::Imperative && tmood && tmood != nlp::morph::MoodImp) sc -= 2;
    if (es) {
      // "visto" after haber is ver's participle, not vestir 1sg; a pronominal lemma ("regresarse") only through a
      // clitic (the clause builder adds the particle "se")
      if (vf == nlp::morph::VfPart && f.person) sc -= 2;
      if (vf == nlp::morph::VfPart && f.mood && f.mood != feat::ParticipleMood && !f.person) sc -= 1;
      const std::string h = text::lower(lex_->lemma(a.lemma).head);
      if (lex_->lemma(a.lemma).pos == feat::Verb && h.size() > 4 && h.compare(h.size() - 2, 2, "se") == 0) sc -= 1;
      if (esAdjLemma && lex_->lemma(a.lemma).pos != feat::Adj) sc -= 3;
      if (es::rareLemma(h)) sc -= 1;   // "duele" is doler, not dolar (hew)
    }
    return sc;
  };
  int bestAgree = -100;
  for (const lex::Analysis& a : an) {
    const lex::Lemma l = lex_->lemma(a.lemma);
    if (l.id == lex::kNoLemma || !posOk(l.pos)) continue;
    bestAgree = std::max(bestAgree, agree(a));
  }
  std::string same, other, ruleHit;
  std::vector<uint32_t> sameFeats, otherFeats;   // C15: the form's features under its own lemma and under the other
  for (const lex::Analysis& a : an) {
    const lex::Lemma l = lex_->lemma(a.lemma);
    if (l.id == lex::kNoLemma || !posOk(l.pos)) continue;
    if (agree(a) < bestAgree) continue;
    std::string h = text::lower(l.head);
    if (h.find(' ') != std::string::npos) continue;
    {   // C15: abbreviations ("ST", "STing") are never the lemma of an ordinary word
      bool caps = l.head.size() > 1;
      for (size_t q = 0; q < 2 && q < l.head.size(); ++q) caps = caps && l.head[q] >= 'A' && l.head[q] <= 'Z';
      if (caps && t.text.size() > 1 && !(t.text[1] >= 'A' && t.text[1] <= 'Z')) continue;
    }
    const std::string hk = lang_ == SrcLang::En ? text::en_key(h) : text::es_key(h);
    if (hk == key) { if (same.empty()) same = h; if (h == same) sameFeats.push_back(a.feat); }
    else { if (other.empty()) other = h; if (h == other) otherFeats.push_back(a.feat); }
    if (h == rule && ruleHit.empty()) ruleHit = h;
  }
  // C15: "have come": "come" is the participle of come itself and of the rare "cum" alike: the form's own lemma
  bool sameInflects = false;
  for (uint32_t f : sameFeats)
    sameInflects = sameInflects || std::find(otherFeats.begin(), otherFeats.end(), f) != otherFeats.end();
  if (es && t.upos == "NOUN" && !same.empty() && num != nlp::morph::NumPlur) return same;   // "hermana", "niña", "reina" keep their own lemma
  // C19: a verb whose past is its own form ("the sun set", "he put", "she cut"): the rule lemmatiser invents "sett"
  if (lang_ == SrcLang::En && (t.upos == "VERB" || t.upos == "AUX") &&
      ((!ruleKnown && ruleHit.empty()) || rule == t.lower + t.lower.back())) {
    for (const lex::Analysis& a : an) {
      const lex::Lemma l = lex_->lemma(a.lemma);
      if (l.id != lex::kNoLemma && l.pos == feat::Verb && text::lower(l.head) == t.lower) return t.lower;
    }
  }
  if (inflected && lang_ == SrcLang::En && !same.empty() && !other.empty() && other == t.lower + t.lower.back())
    return same;   // C19: "set" is the past of set, not of a rare "sett"
  if (inflected) {
    if (!ruleHit.empty() && ruleHit != t.lower) return ruleHit;
    // C15: a form that is its own lemma and its own rule lemma ("have come") is not a rare homograph ("cum")
    if (!same.empty() && sameInflects && !es) return same;
    if (!other.empty()) return other;
    if (!same.empty()) return same;
  } else {
    if (!same.empty()) return same;
    if (!ruleHit.empty() && ruleHit != t.lower) return ruleHit;
    // C17: an adjective that is a verb's participle ("a lighted match": light)
    if (lang_ == SrcLang::En && t.upos == "ADJ") {
      const std::string v = en::verbOfForm(*lex_, t.lower);
      if (!v.empty()) return v;
    }
    if (!ruleHit.empty()) return ruleHit;
    if (!other.empty() && lang_ == SrcLang::Es) return other;
  }
  if (!ruleKnown) return t.lower;
  return rule;
}

// ---- clause building -------------------------------------------------------------------------------------------------
struct FrameBuilder::Ctx {
  SemSentence& s;
  std::vector<int> par;                       // 0-based parent, -1 root
  std::vector<std::vector<int>> kids;         // children in order
  std::vector<char> consumed;                 // phrase tokens
  std::vector<char> taken;                    // tokens already given a role (NP parts, predicate modifiers)
  std::vector<SemOblique>* lifted = nullptr;  // obliques lifted out of NPs of the clause being built
  std::vector<SemAdverb>* liftedAdv = nullptr;
  bool question = false;
  std::vector<PhraseMatch> vpHits;            // phrasebook rows of register "vp" (verb + complements)
  explicit Ctx(SemSentence& ss) : s(ss) {}
  void repair(const char* what) {
    if (std::find(s.repairs.begin(), s.repairs.end(), what) == s.repairs.end()) s.repairs.emplace_back(what);
  }
  const Token& t(int i) const { return s.tokens[(size_t)i]; }
  bool ok(int i) const { return i >= 0 && i < (int)s.tokens.size() && !consumed[(size_t)i]; }
  bool free(int i) const { return ok(i) && !taken[(size_t)i]; }
  void take(int i) { if (i >= 0 && i < (int)taken.size()) taken[(size_t)i] = 1; }
  std::string dep(int i) const { return t(i).deprel; }
  std::string lem(int i) const { return text::lower(t(i).lemma.empty() ? t(i).lower : t(i).lemma); }
  void subtree(int i, std::vector<int>& out) const {
    out.push_back(i);
    for (int k : kids[(size_t)i])
      if (!consumed[(size_t)k]) subtree(k, out);
  }
  void drop(int i, Drop d) {
    if (i >= 0 && i < (int)s.drop.size() && s.drop[(size_t)i] == Drop::No) s.drop[(size_t)i] = d;
  }
};

void FrameBuilder::buildNP(Ctx& c, int h, SemNP& np) const {
  // C19: "witches and sorceresses" parsed as compound + amod "and" on the last noun: a coordination of two nouns
  if (lang_ == SrcLang::En && c.t(h).upos == "NOUN") {
    int k1 = -1, k2 = -1;
    for (int k : c.kids[(size_t)h]) {
      if (!c.ok(k) || k > h) continue;
      if (c.t(k).upos == "NOUN" && (c.dep(k) == "compound" || c.dep(k) == "amod") && k1 < 0) k1 = k;
      else if (k1 >= 0 && k > k1 && in(c.t(k).lower, {"and", "or"}) && c.kids[(size_t)k].empty()) k2 = k;
    }
    if (k1 >= 0 && k2 == k1 + 1 && k2 == h - 1) {
      auto& hk = c.kids[(size_t)h];
      hk.erase(std::remove(hk.begin(), hk.end(), k1), hk.end());
      hk.erase(std::remove(hk.begin(), hk.end(), k2), hk.end());
      c.drop(k2, Drop::Marker);
      c.take(k2);
      SemNP first, second;
      buildNP(c, k1, first);
      buildNP(c, h, second);
      np = first;
      np.coordConj = c.t(k2).lower;
      np.coord.push_back(second);
      np.tokens.insert(np.tokens.end(), second.tokens.begin(), second.tokens.end());
      np.tokens.push_back(k2);
      return;
    }
  }
  // C22: "a dozen apples", "two dozen eggs", "a dozen of the eggs": the noun is the head, the numeral twelve (times
  // the number before dozen); the parser makes "dozen" the head or a numeral, never the noun's genitive
  if (lang_ == SrcLang::En && (c.t(h).lower == "dozen" || c.t(h).lower == "dozens")) {
    int noun = -1;
    for (int k : c.kids[(size_t)h])
      if (c.ok(k) && k > h && (c.t(k).upos == "NOUN" || c.t(k).upos == "PROPN") && noun < 0) noun = k;
    if (noun >= 0) {
      auto& hk = c.kids[(size_t)h];
      hk.erase(std::remove(hk.begin(), hk.end(), noun), hk.end());
      int mult = 1;
      std::vector<int> extra;
      for (int k : hk) {
        if (!c.ok(k)) continue;
        if (c.t(k).upos == "NUM" && numberValue(c.t(k).lower) > 0) mult = numberValue(c.t(k).lower);
        extra.push_back(k);
      }
      for (int k : c.kids[(size_t)noun])   // "of" before the noun
        if (c.ok(k) && k < noun && c.dep(k) == "case" && c.t(k).lower == "of") { c.take(k); c.drop(k, Drop::Marker); }
      buildNP(c, noun, np);
      for (int k : extra) { c.take(k); c.drop(k, Drop::Article); np.tokens.push_back(k); }
      np.numeral = c.t(h).lower;
      np.numeralValue = 12 * mult;
      np.number = 2;
      np.tokens.push_back(h);
      return;
    }
  }
  const Token& ht = c.t(h);
  const std::string low = ht.lower;
  np.token = h;
  np.surface = ht.text;
  np.head = c.lem(h);
  np.tokens.push_back(h);
  const uint32_t num = fget(ht, nlp::morph::NumberShift);
  np.number = num == nlp::morph::NumPlur ? 2 : 1;
  // pronouns
  const bool en = lang_ == SrcLang::En;
  bool pron = false;
  if (ht.upos == "PRON" || ht.upos == "DET" || (en && in(low, {"i", "me", "you", "we", "us", "they", "them"}))) {
    const PronInfo* b = en ? kPronEn : kPronEs;
    const size_t n = en ? sizeof(kPronEn) / sizeof(kPronEn[0]) : sizeof(kPronEs) / sizeof(kPronEs[0]);
    for (size_t i = 0; i < n; ++i)
      if (low == b[i].w) {
        np.isPronoun = true;
        np.pron.person = b[i].person;
        np.pron.number = b[i].number;
        np.pron.gender = b[i].gender;
        np.pron.reflexive = b[i].refl;
        np.number = b[i].number == 2 ? 2 : 1;
        np.pronLemma = low;
        pron = true;
        break;
      }
    if (!pron) {
      // indefinite / demonstrative / interrogative pronouns: canonical English in pronLemma
      std::string w = low;
      if (!en) {
        if (in(w, {"todos", "todas"})) w = "everyone";
        else if (in(w, {"todo"})) w = "everything";
        else if (in(w, {"nadie"})) w = "nobody";
        else if (in(w, {"nada"})) w = "nothing";
        else if (in(w, {"alguien"})) w = "someone";
        else if (in(w, {"algo"})) w = "something";
        else if (in(w, {"este", "esta", "estos", "estas", "ese", "esa", "esos", "esas", "aquel", "aquella",
                        "aquellos", "aquellas"})) {
          // a gendered demonstrative standing alone points at a noun ("Esta sí.", "Este sí puede."): like "this
          // one", its Latin gender is that noun's, not the Spanish one (C13)
          np.determiner = w[0] == 'e' && w[1] == 's' && w[2] == 't' ? "this" : "that";
          w = (w.back() == 's') ? "ones" : "one";
        }
        else if (in(w, {"esto"})) w = "this";
        else if (in(w, {"eso", "aquello"})) w = "that";
        else if (in(w, {"quién", "quiénes"})) w = "who";
        else if (in(w, {"qué"})) w = "what";
        else if (in(w, {"cuál"})) w = "which";
        else if (in(w, {"que", "quien", "cual"})) w = "which";
      }
      if (w == "no-one" || w == "noone") w = "nobody";
      if (in(w, {"everyone", "everybody", "everything", "someone", "somebody", "something", "anyone", "anybody",
                 "anything", "nobody", "nothing", "none", "this", "that", "these", "those", "who", "whom", "what",
                 "which", "one", "ones", "all", "both", "each", "another", "other", "others", "many", "few",
                 "some", "any", "much", "mine", "yours", "ours"})) {
        np.isPronoun = true;
        np.pronLemma = w == "somebody" ? "someone" : w == "everybody" ? "everyone" : w == "anybody" ? "anyone"
                     : w == "whom" ? "who" : w;
        np.pron.person = 3;
        np.pron.number = in(w, {"these", "those", "ones", "all", "both", "others", "many", "few", "some"}) ? 2 : 1;
        if (in(w, {"everyone", "all"})) np.pron.number = 2;
        np.number = np.pron.number;
        np.negative = in(w, {"nobody", "nothing", "none"});
        // (C28: "whom" too: "Whom did you see?")
        np.interrogative = in(w, {"who", "whom", "what", "which"}) && (c.question || fget(ht, nlp::morph::PronTypeShift) ==
                                                                                 nlp::morph::PtInt);
        // C24: "what" heading its own relative clause is a free relative ("quod vidētur"), not a question word
        if (w == "what" && en)
          for (int k : c.kids[(size_t)h])
            if (c.ok(k) && c.dep(k) == "acl" && c.t(k).upos == "VERB") np.interrogative = false;
        if (np.interrogative) np.wh = np.pronLemma;
        pron = true;
      }
    }
  }
  // C22: generic "one" as the subject of a modal ("How can one read in the dark?", "One must eat."): a person in
  // general (homō), not the numeral (ūnus homo) nor an anaphoric "this one"
  if (!pron && en && low == "one" && c.dep(h) == "nsubj" && c.par[(size_t)h] >= 0) {
    bool kids = false, modal = false;
    for (int k : c.kids[(size_t)h]) kids = kids || c.ok(k);
    for (int k : c.kids[(size_t)c.par[(size_t)h]])
      modal = modal || (c.dep(k) == "aux" && in(c.t(k).lower, {"can", "could", "must", "should", "may", "might", "ought"}));
    if (!kids && modal) {
      np.isPronoun = true;
      np.pronLemma = "one-generic";
      np.pron.person = 3;
      np.pron.number = 1;
      np.number = 1;
      pron = true;
    }
  }
  if (!pron && en && (low == "one" || low == "ones") &&
      (ht.upos == "NOUN" || (ht.upos == "NUM" && [&] {   // C22: a bare numeral "one" too ("I have one.")
         for (int k : c.kids[(size_t)h]) if (c.ok(k) && c.dep(k) != "punct") return false;
         return true; }()))) {
    np.isPronoun = true;
    np.pronLemma = low;
    np.pron.person = 3;
    np.pron.number = low == "ones" ? 2 : 1;
    np.number = np.pron.number;
    pron = true;
  }
  if (ht.upos == "PROPN") {
    np.isName = true;
    np.head = ht.text;
    // Spanish (C13): a capitalised word inside the sentence that spanish.vpl knows as a common noun is a title
    // ("la Reina de Corazones" -> Rēgīna Cordium): its lemma, translated and capitalised
    bool sentenceFirst = true;
    for (int q = 0; q < h; ++q) sentenceFirst = sentenceFirst && isPunctTok(c.t(q));
    if (!en && lex_ && sentenceFirst) {   // "Niña, ven aquí.": a capital only because it opens the sentence
      std::vector<lex::Analysis> an;
      lex_->lookup(text::es_key(low), an);
      bool noun = false, name = cd_.nameByLatin(text::latin_key(ht.text)) != nullptr || cd_.nameByEnglish(ht.text);
      std::string nl;
      for (const lex::Analysis& a : an) {
        const lex::Lemma l = lex_->lemma(a.lemma);
        if (l.pos == feat::Noun && (nl.empty() || text::lower(l.head) == low)) { noun = true; nl = text::lower(l.head); }
        if (l.pos == feat::Name) name = true;
      }
      if (noun && !name) { np.isName = false; np.head = nl; }
    }
    if (!en && lex_ && h > 0 && !isPunctTok(c.t(h - 1))) {
      std::vector<lex::Analysis> an;
      lex_->lookup(text::es_key(low), an);
      for (const lex::Analysis& a : an) {
        const lex::Lemma l = lex_->lemma(a.lemma);
        if (l.pos == feat::Noun) {
          np.title = true;
          np.head = text::lower(l.head);
          if (feat::unpack(lex_->feature(a.feat)).number == feat::Pl) np.number = 2;   // "Corazones"
          break;
        }
      }
    }
  }
  // Spanish (C13): an adjective standing for a noun ("La reina quería rojas.") is "red ones"
  if (!en && !pron && ht.upos == "ADJ") {
    SemAdj a;
    a.lemma = np.head;
    a.token = h;
    np.adjectives.push_back(a);
    np.isPronoun = true;
    np.pronLemma = np.number == 2 ? "ones" : "one";
    np.pron.person = 3;
    np.pron.number = np.number;
    pron = true;
  }
  // Spanish diminutives (C13): "gatito" -> gato + pequeño (parvus), "casita" -> casa + pequeño, "florecita" -> flor
  if (!en && ht.upos == "NOUN" && lex_ && !es::notDiminutive(np.head)) {
    const std::string& w = np.head;
    auto ends = [&](const char* suf) {
      const size_t k = std::strlen(suf);
      return w.size() > k + 2 && w.compare(w.size() - k, k, suf) == 0;
    };
    std::vector<std::string> bases;
    for (const char* suf : {"ecito", "ecita", "cito", "cita", "ito", "ita"}) {
      if (!ends(suf)) continue;
      const std::string stem = w.substr(0, w.size() - std::strlen(suf));
      const char g = w.back();   // o / a
      bases = {stem + g, stem + (g == 'o' ? "a" : "o"), stem, stem + "e"};
      break;
    }
    for (const std::string& b : bases) {
      std::vector<lex::Analysis> an;
      lex_->lookup(text::es_key(b), an);
      if (an.empty()) lex_->lookup(text::es_bare(b), an);
      std::string found;
      for (const lex::Analysis& a : an) {
        const lex::Lemma l = lex_->lemma(a.lemma);
        if (l.pos == feat::Noun && text::es_bare(text::lower(l.head)) == text::es_bare(b)) { found = text::lower(l.head); break; }
      }
      if (found.empty()) continue;
      np.head = found;
      SemAdj a;
      a.lemma = "pequeño";
      a.token = h;
      np.adjectives.push_back(a);
      break;
    }
  }
  // Spanish feminine person nouns keep their own head ("hermana" is not hermano: soror, not frāter) (C13)
  if (!en && ht.upos == "NOUN" && np.head.size() > 1 && np.head.back() == 'o') {
    std::string sg = low;
    if (sg.size() > 2 && sg.back() == 's') sg.pop_back();
    if (sg.size() == np.head.size() && sg.back() == 'a' && sg.compare(0, sg.size() - 1, np.head, 0, np.head.size() - 1) == 0)
      np.head = sg;
  }
  if (!en && ht.upos == "NOUN" && lex_) {   // source gender of a Spanish noun: the form's features, else its ending
    std::vector<lex::Analysis> an;
    lex_->lookup(text::es_key(low), an);
    for (const lex::Analysis& a : an) {
      const uint8_t g = feat::unpack(lex_->feature(a.feat)).gender;
      if ((g == feat::M || g == feat::F) && lex_->lemma(a.lemma).pos == feat::Noun) { np.srcGender = g; break; }
    }
    if (!np.srcGender && low.size() > 2) {
      const char e1 = low.back(), e2 = low[low.size() - 2];
      if (e1 == 'o' || (e1 == 's' && e2 == 'o')) np.srcGender = feat::M;
      else if (e1 == 'a' || (e1 == 's' && e2 == 'a')) np.srcGender = feat::F;
    }
  }
  if (ht.upos == "NUM") {
    np.numeral = low;
    np.numeralValue = numberValue(low);
  }
  // dependents
  for (int k : c.kids[(size_t)h]) {
    if (!c.ok(k)) continue;
    const Token& kt = c.t(k);
    const std::string d = kt.deprel;
    const std::string kl = kt.lower;
    if (d == "det" || (d == "nmod" && (kt.upos == "DET" || (en && kl == "whose")))) {   // C26: "whose" read as nmod
      np.tokens.push_back(k);
      const std::string cd = canonDet(lang_, kl);
      // possessive determiners (Spanish "mi", "su")
      bool poss = false;
      const PossInfo* pb = en ? kPossEn : kPossEs;
      const size_t pn = en ? sizeof(kPossEn) / sizeof(kPossEn[0]) : sizeof(kPossEs) / sizeof(kPossEs[0]);
      for (size_t i = 0; i < pn; ++i)
        if (kl == pb[i].w) {
          SemNP p;
          p.isPronoun = true;
          p.pron.person = pb[i].person;
          p.pron.number = pb[i].number;
          p.pronLemma = kl;
          p.token = k;
          p.tokens.push_back(k);
          np.possessor.push_back(p);
          poss = true;
          break;
        }
      if (poss) continue;
      if (cd == "the") { np.definite = true; c.drop(k, Drop::Article); continue; }
      // Spanish "un poco de té" (es::normalise hangs "un poco" on the noun): a little (C13)
      if (!en && np.determiner == "a" && in(kl, {"poco", "poca"})) { np.determiner = "a little"; continue; }
      if (cd == "a") { c.drop(k, Drop::Article); if (np.determiner.empty()) np.determiner = "a"; continue; }
      if (cd == "what" || cd == "which" || cd == "whose" || cd == "how many") {
        // "what a strange garden" (exclamation) vs "what time" / "which way" (question)
        np.determiner = cd;
        if (c.question || cd != "what") { np.interrogative = true; np.wh = cd; }
        continue;
      }
      np.determiner = cd;
      if (cd == "no") np.negative = true;
      continue;
    }
    if (d == "nmod" && kt.upos == "PRON") {   // possessive pronoun ("my", "his")
      const PossInfo* pb = en ? kPossEn : kPossEs;
      const size_t pn = en ? sizeof(kPossEn) / sizeof(kPossEn[0]) : sizeof(kPossEs) / sizeof(kPossEs[0]);
      for (size_t i = 0; i < pn; ++i)
        if (kl == pb[i].w) {
          SemNP p;
          p.isPronoun = true;
          p.pron.person = pb[i].person;
          p.pron.number = pb[i].number;
          if (kl == "his") p.pron.gender = feat::M;
          if (kl == "her" || kl == "hers") p.pron.gender = feat::F;
          if (kl == "its") p.pron.gender = feat::N;
          p.pronLemma = kl;
          p.token = k;
          p.tokens.push_back(k);
          np.possessor.push_back(p);
          np.tokens.push_back(k);
          break;
        }
      if (!np.possessor.empty() && np.possessor.back().token == k) continue;
    }
    // C17: a participle before its noun ("the sleeping dog", "a broken cart") is an adjective: the verb's participle
    uint8_t participle = 0;
    if (en && (d == "amod" || d == "compound") && (kt.upos == "VERB" || kt.upos == "ADJ") && k < h) {
      const uint32_t vf = fget(kt, nlp::morph::VerbFormShift);
      // C20: a verb tagged amod without a verb form ("the lost boy" read "lost" as a finite verb and made it a genitive
      // noun "lose") is a participle as well
      const bool bareAmod = d == "amod" && kt.upos == "VERB" && vf == 0 && kl != c.lem(k);
      if (vf == nlp::morph::VfPart || vf == nlp::morph::VfGer || bareAmod) {
        bool bare = true;
        for (int g : c.kids[(size_t)k])
          if (c.ok(g) && c.dep(g) != "advmod") bare = false;
        const bool ing = kt.lower.size() > 4 && kt.lower.compare(kt.lower.size() - 3, 3, "ing") == 0;
        if (bare && (kt.upos == "VERB" || c.lem(k) != kl)) participle = ing ? 2 : 1;
      }
    }
    if (participle || d == "amod" || (d == "compound" && (kt.upos == "ADJ" || kt.upos == "NOUN"))) {
      bool adjLike = kt.upos == "ADJ" || participle;
      // C19: a word tagged ADJ before its noun that the lexicon knows only as a noun ("the poppy bed"): a noun compound
      if (en && adjLike && !participle && kt.upos == "ADJ" && lex_ && k < h) {
        std::vector<lex::Analysis> an;
        lex_->lookup(text::en_key(kl), an);
        bool adj = false, noun = false;
        for (const lex::Analysis& a : an) {
          const uint8_t ps = lex_->lemma(a.lemma).pos;
          adj = adj || ps == feat::Adj || ps == feat::Participle;
          noun = noun || ps == feat::Noun;
        }
        if (noun && !adj) adjLike = false;
      }
      if (!adjLike && kt.upos == "NOUN" && lex_) {   // "white roses" tagged NOUN compound: adjective reading?
        std::vector<lex::Analysis> an;
        lex_->lookup(en ? text::en_key(kl) : text::es_key(kl), an);
        for (const lex::Analysis& a : an)
          if (lex_->lemma(a.lemma).pos == feat::Adj) { adjLike = true; break; }
      }
      if (adjLike) {
        SemAdj a;
        a.lemma = c.lem(k);
        if (kt.upos != "ADJ" && !participle) a.lemma = kl;
        a.token = k;
        a.participle = participle;
        if (kl == "much" || kl == "many") {
          np.determiner = kl;
          np.tokens.push_back(k);
          for (int g : c.kids[(size_t)k])
            if (c.ok(g) && c.t(g).lower == "how") {
              np.interrogative = true;
              np.wh = kl == "many" ? "how many" : "how much";
              np.determiner.clear();
              np.tokens.push_back(g);
            }
          continue;
        }
        if (kl == "more" || kl == "most") { np.tokens.push_back(k); continue; }
        if (kl != a.lemma && en) {
          if (kl.size() > 2 && kl.compare(kl.size() - 2, 2, "er") == 0) a.degree = feat::Comparative;
          if (kl.size() > 3 && kl.compare(kl.size() - 3, 3, "est") == 0) a.degree = feat::Superlative;
          if (kl == "better" || kl == "worse") a.degree = feat::Comparative;
          if (kl == "best" || kl == "worst") a.degree = feat::Superlative;
        }
        for (int g : c.kids[(size_t)k]) {
          if (!c.ok(g)) continue;
          if (c.dep(g) == "advmod") {
            a.adverbs.push_back(c.t(g).lower);
            a.advTokens.push_back(g);
            np.tokens.push_back(g);
            if (c.t(g).lower == "how" && (kl == "many" || kl == "much")) { np.interrogative = true; np.wh = "how many"; }
          }
        }
        np.adjectives.push_back(a);
        np.tokens.push_back(k);
        // C17: coordinated adjectives ("the third and last time", "a big and ugly dog")
        if (en)
          for (int g : c.kids[(size_t)k]) {
            if (!c.ok(g) || c.dep(g) != "conj" || c.t(g).upos != "ADJ") continue;
            SemAdj b;
            b.lemma = c.lem(g);
            b.token = g;
            np.adjectives.push_back(b);
            np.tokens.push_back(g);
            c.take(g);
            for (int q : c.kids[(size_t)g])
              if (c.ok(q) && c.dep(q) == "cc") { np.tokens.push_back(q); c.take(q); c.drop(q, Drop::Other); }
          }
        continue;
      }
      // noun compound: "apple tree" -> genitive attribute
      SemNP g;
      buildNP(c, k, g);
      np.genitive.push_back(g);
      np.tokens.insert(np.tokens.end(), g.tokens.begin(), g.tokens.end());
      continue;
    }
    if (d == "nummod") {
      np.numeral = kl;
      np.numeralValue = numberValue(kl);
      // C22: "two dozen eggs" (two hung on dozen): twenty-four; "a dozen eggs": twelve
      if (en && (kl == "dozen" || kl == "dozens")) {
        int mult = 1;
        for (int q : c.kids[(size_t)k]) {
          if (!c.ok(q)) continue;
          if (numberValue(c.t(q).lower) > 0) mult = numberValue(c.t(q).lower);
          np.tokens.push_back(q);
          c.take(q);
          c.drop(q, Drop::Article);
        }
        np.numeralValue = 12 * mult;
        np.number = 2;
      }
      np.tokens.push_back(k);
      if (np.head == "hour" && low == "o'clock") np.ordinal = true;
      continue;
    }
    // C19: a proper adjective before a common noun ("a Roman soldier", "the Greek ship"): an adjective, never dropped
    if (en && !np.isName && (d == "compound" || d == "amod") && kt.upos == "PROPN" && k < h && lex_ &&
        c.t(h).upos == "NOUN") {
      std::vector<lex::Analysis> an;
      lex_->lookup(text::en_key(kl), an);
      bool adj = false;
      for (const lex::Analysis& a : an) adj = adj || lex_->lemma(a.lemma).pos == feat::Adj;
      if (adj) {
        SemAdj a;
        a.lemma = kl;
        a.token = k;
        np.adjectives.push_back(a);
        np.tokens.push_back(k);
        continue;
      }
    }
    if (d == "flat" || (d == "compound" && kt.upos == "PROPN")) {   // multi-word names
      if (np.isName) np.head = k < h ? kt.text + " " + np.head : np.head + " " + kt.text;   // source order (C15)
      np.tokens.push_back(k);
      continue;
    }
    if (d == "case") {
      if (kl == "'s" || kl == "'") np.tokens.push_back(k);
      continue;   // the caller reads the preposition
    }
    if (!en && d == "nmod" && kt.upos == "ADV") {   // "Se venden casas aquí": the adverb belongs to the clause
      if (c.liftedAdv && c.free(k)) {
        SemAdverb a;
        a.lemma = c.lem(k);
        a.token = k;
        c.liftedAdv->push_back(a);
      }
      c.take(k);
      continue;
    }
    if (d == "nmod" || (d == "obl" && kt.upos != "ADV")) {
      // case marker of the attribute
      std::string prep;
      bool poss = false;
      for (int g : c.kids[(size_t)k])
        if (c.ok(g) && c.dep(g) == "case") {
          const std::string gl = c.t(g).lower;
          if (gl == "'s" || gl == "'") poss = true;
          else prep = prep.empty() ? gl : prep + " " + gl;
        }
      prep = canonPrep(lang_, prep);
      SemNP g;
      buildNP(c, k, g);
      if (poss) { np.possessor.push_back(g); np.tokens.insert(np.tokens.end(), g.tokens.begin(), g.tokens.end()); continue; }
      if (prep == "of" || prep.empty()) {
        np.genitive.push_back(g);
        np.tokens.insert(np.tokens.end(), g.tokens.begin(), g.tokens.end());
        continue;
      }
      if (c.lifted) {
        SemOblique o;
        o.prep = prep;
        o.np = g;
        o.token = k;
        c.lifted->push_back(o);
      }
      c.take(k);
      continue;
    }
    if (d == "acl") {
      // relative clause (acl:relcl collapsed): built as a frame; the role of the gap is set by the transfer
      {   // C15 confidence: a relative clause without a relative word, a to-infinitive or a participle on a noun
        bool relWord = false, toMark = false;
        for (int g : c.kids[(size_t)k]) {
          if (!c.ok(g)) continue;
          if (in(c.t(g).lower, {"who", "whom", "which", "that", "whose", "where", "when", "que", "quien", "donde"})) relWord = true;
          if (c.dep(g) == "mark" && c.t(g).lower == "to") toMark = true;
        }
        if (en && toMark) c.s.doubt("noun-infinitive");
        else if (en && fget(c.t(k), nlp::morph::VerbFormShift) == nlp::morph::VfPart && !relWord) c.s.doubt("participle-phrase");
        else if (en && !relWord) c.s.doubt("contact-relative");
      }
      SemFrame rf;
      std::vector<SemOblique>* keep = c.lifted;
      std::vector<SemAdverb>* keepAdv = c.liftedAdv;
      std::vector<SemOblique> l2;
      std::vector<SemAdverb> a2;
      c.lifted = &l2;
      c.liftedAdv = &a2;
      buildClause(c, k, rf);
      c.lifted = keep;
      c.liftedAdv = keepAdv;
      rf.obliques.insert(rf.obliques.end(), l2.begin(), l2.end());
      rf.adverbs.insert(rf.adverbs.end(), a2.begin(), a2.end());
      // C26: a bare past participle on a noun ("a crown made of gold", "a house built of stone") is passive: the noun
      // is what was made (quae ex aurō facta est), never the maker
      if (en && fget(c.t(k), nlp::morph::VerbFormShift) == nlp::morph::VfPart &&
          fget(c.t(k), nlp::morph::TenseShift) == nlp::morph::TensePast && !rf.hasSubject && rf.hasPred &&
          rf.pred.voice == Voice::Active && rf.pred.auxTokens.empty() && !rf.hasObject && k > h) {
        bool relWord = false;
        for (int g : c.kids[(size_t)k])
          if (c.ok(g) && in(c.t(g).lower, {"who", "whom", "which", "that", "whose"})) relWord = true;
        if (!relWord) {
          rf.pred.voice = Voice::Passive;
          rf.pred.tense = Tense::Present;
          rf.pred.aspect = Aspect::Simple;
        }
      }
      // C15: a stranded preposition of a contact relative ("I told you of") is the relative pronoun's preposition
      if (en)
        for (size_t q = 0; q < rf.adverbs.size(); ++q) {
          const std::string& w = rf.adverbs[q].lemma;
          if (!in(w, {"of", "about", "with", "to", "for", "from", "in", "on", "at"})) continue;
          bool last = true;
          for (int x : rf.tokens)
            if (x > rf.adverbs[q].token && !isPunctTok(c.t(x))) last = false;
          if (!last) continue;
          SemOblique o;
          o.prep = w;
          o.token = rf.adverbs[q].token;
          o.np.isPronoun = true;
          o.np.pronLemma = "which";
          o.np.pron.person = 3;
          o.np.token = rf.adverbs[q].token;
          o.np.tokens.push_back(rf.adverbs[q].token);
          rf.obliques.push_back(o);
          rf.adverbs.erase(rf.adverbs.begin() + (long)q);
          c.drop(o.token, Drop::Marker);
          break;
        }
      np.relative.push_back(rf);
      np.tokens.insert(np.tokens.end(), rf.tokens.begin(), rf.tokens.end());
      continue;
    }
    // C22: "Edwin and Paul, the Dukes of Rome, fought ...", "Robert, the Bishop of London, agreed ...": a definite
    // noun phrase after a comma without its own conjunction is an apposition, not a further conjunct
    if (en && (d == "appos" || d == "conj") && (kt.upos == "NOUN" || kt.upos == "PROPN") && k > h) {
      bool the = false, cc = false, commaKid = false;
      int first = k;
      for (int x : c.kids[(size_t)k]) {
        if (!c.ok(x)) continue;
        if (c.dep(x) == "det" && c.t(x).lower == "the") the = true;
        if (c.dep(x) == "cc") cc = true;
        if (c.t(x).text == "," && x < k) { commaKid = true; continue; }
        if (x < first) first = x;
      }
      const bool comma = commaKid || (first > 0 && c.t(first - 1).text == ",");
      // C26: "the lion, the tiger and the bear": a conjunct after a comma with a later conjunct joined by and / or is
      // a member of a list, not an apposition
      bool listMember = false;
      if (d == "conj")
        for (int x : c.kids[(size_t)h])
          if (x > k && c.ok(x) && c.dep(x) == "conj")
            for (int y : c.kids[(size_t)x])
              if (c.ok(y) && c.dep(y) == "cc" && in(c.t(y).lower, {"and", "or"})) listMember = true;
      if ((d == "appos" || (the && comma)) && !cc && !listMember) {
        // "of" phrases the parser hung on the head after the apposition belong to the apposition ("Edwin, the Duke
        // of Rome,")
        std::vector<int> moved;
        for (int x : c.kids[(size_t)h])
          if (x > k && c.ok(x) && c.dep(x) == "nmod") moved.push_back(x);
        for (int x : moved) c.kids[(size_t)k].push_back(x);
        std::sort(c.kids[(size_t)k].begin(), c.kids[(size_t)k].end());
        // a capitalised common noun after "the" is a title, not a name ("the Bishop of London" -> episcopus)
        nlp::Token& at = c.s.tokens[(size_t)k];
        if (at.upos == "PROPN" && lex_ && !cd_.nameByEnglish(at.text)) {
          std::vector<lex::Analysis> an;
          lex_->lookup(text::en_key(nlp::normalise(at.text)), an);
          bool noun = false, plural = false;
          for (const lex::Analysis& a : an)
            if (lex_->lemma(a.lemma).pos == feat::Noun) {
              noun = true;
              plural = plural || feat::unpack(lex_->feature(a.feat)).number == feat::Pl;
            }
          if (noun) {
            at.upos = "NOUN";
            at.lower = nlp::normalise(at.text);
            at.feats = nlp::morph::fromString(plural ? "Number=Plur" : "Number=Sing");
            at.lemma = lemmaOf(at);
          }
        }
        SemNP g;
        buildNP(c, k, g);
        for (int x : moved) c.consumed[(size_t)x] = 1;   // built with the apposition: not again with the head
        np.apposition.push_back(g);
        np.tokens.insert(np.tokens.end(), g.tokens.begin(), g.tokens.end());
        continue;
      }
    }
    if (d == "conj") {
      // C26: a verb with its own subject coordinated with a predicate noun ("you will be a great man, for I have
      // given you ...") is a clause of its own (the clause builder takes it as a coordinated clause), never a conjunct
      if (en && (kt.upos == "VERB" || kt.upos == "AUX") && k > h) {
        bool ownSubj = false;
        for (int g : c.kids[(size_t)k]) ownSubj = ownSubj || (c.ok(g) && c.dep(g) == "nsubj");
        if (ownSubj) continue;
      }
      if (en && (kt.upos == "VERB" || kt.upos == "AUX")) c.s.doubt("participle-phrase");   // C15: a verb hung on a noun
      SemNP g;
      buildNP(c, k, g);
      for (int x : c.kids[(size_t)k])
        if (c.ok(x) && c.dep(x) == "cc") {
          np.coordConj = canonConnector(lang_, c.t(x).lower);
          c.drop(x, Drop::Marker);
        }
      np.coord.push_back(g);
      np.tokens.insert(np.tokens.end(), g.tokens.begin(), g.tokens.end());
      continue;
    }
    if (d == "advmod") {
      if (c.liftedAdv && c.free(k)) {
        SemAdverb a;
        a.lemma = c.lem(k);
        a.token = k;
        // C15: a sentence-initial adverb hung on a predicate NP ("Now the Golden Cap is yours") stays first
        bool firstWord = true;
        for (int q = 0; q < k; ++q)
          if (!isPunctTok(c.t(q)) && c.dep(q) != "cc") firstWord = false;
        a.front = firstWord && k < h;
        c.liftedAdv->push_back(a);
      }
      c.take(k);
      continue;
    }
    if (d == "punct") continue;
  }
  if (!en) {
    // "todas las mañanas" = every morning (C13)
    if (np.determiner == "all" && np.number == 2 && timeNoun(np.head)) { np.determiner = "every"; np.number = 1; }
    // "dos hermanos y una hermana": "una" is the numeral next to another numeral
    if (!np.numeral.empty())
      for (SemNP& g : np.coord)
        if (g.numeral.empty() && g.determiner == "a") { g.numeral = "1"; g.numeralValue = 1; g.determiner.clear(); }
  }
  // C15: "no one" = nobody, "any one" = anyone
  if (np.isPronoun && np.pronLemma == "one" && (np.determiner == "no" || np.determiner == "any")) {
    np.pronLemma = np.determiner == "no" ? "nobody" : "anyone";
    np.negative = np.determiner == "no";
    np.determiner.clear();
  }
  std::sort(np.tokens.begin(), np.tokens.end());
  for (int x : np.tokens) c.take(x);
  np.tokens.erase(std::unique(np.tokens.begin(), np.tokens.end()), np.tokens.end());
}

void FrameBuilder::buildClause(Ctx& c, int h, SemFrame& f) const {
  const bool en = lang_ == SrcLang::En;
  const Token& ht = c.t(h);
  std::vector<int> sub;
  c.subtree(h, sub);
  std::sort(sub.begin(), sub.end());
  f.tokens = sub;
  int first = sub.empty() ? h : sub.front();
  for (int x : sub)   // "¡Entonces juega!": the first word, not the inverted mark
    if (!isPunctTok(c.t(x))) { first = x; break; }
  int firstContent = first;   // C15: the first word after a coordinating conjunction ("and then you will know")
  for (int x : sub)
    if (!isPunctTok(c.t(x)) && !(c.dep(x) == "cc" && x < h)) { firstContent = x; break; }

  const bool topLevel = c.par[(size_t)h] < 0 || c.consumed[(size_t)c.par[(size_t)h]];
  // A noun root whose clause the parser hung on it as an acl: re-root.
  // "Every morning I walk to school." (time noun + acl with its own subject: the noun is a time oblique);
  // "The little girl walks in the garden." (acl finite verb without subject: the noun is the subject).
  if (topLevel && (ht.upos == "NOUN" || ht.upos == "ADV" || ht.upos == "PROPN" || ht.upos == "PRON")) {
    const bool timeRoot = timeNoun(c.lem(h));
    bool hasCop = false;
    for (int k : c.kids[(size_t)h])
      if (c.ok(k) && c.dep(k) == "cop") hasCop = true;
    for (int k : c.kids[(size_t)h]) {
      if (hasCop || !c.ok(k) || c.dep(k) != "acl" || c.t(k).upos != "VERB") continue;
      bool subj = false, relPron = false;
      for (int g : c.kids[(size_t)k]) {
        if (!c.ok(g)) continue;
        if (c.dep(g) == "nsubj") subj = true;
        const std::string gl = c.t(g).lower;
        if (in(gl, {"who", "which", "that", "whom", "whose", "que", "quien"}) && g < k) relPron = true;
      }
      const uint32_t vf = fget(c.t(k), nlp::morph::VerbFormShift), tt = fget(c.t(k), nlp::morph::TenseShift);
      const bool finite = vf == nlp::morph::VfFin || (tt != 0 && vf != nlp::morph::VfPart);
      if (!timeRoot && (subj || relPron || !finite || k < h)) continue;
      if (timeRoot && !subj) continue;
      c.repair("reroot");
      if (!timeRoot) {
        const char keep = c.consumed[(size_t)k];
        c.consumed[(size_t)k] = 1;
        SemNP sn;
        buildNP(c, h, sn);
        c.consumed[(size_t)k] = keep;
        buildClause(c, k, f);
        if (!f.hasSubject) { f.hasSubject = true; f.subject = sn; }
        if (f.type == Kind::Imp) f.type = Kind::Decl;
        f.tokens = sub;
        return;
      }
      const char keep = c.consumed[(size_t)k];
      c.consumed[(size_t)k] = 1;
      SemNP tn;
      buildNP(c, h, tn);
      c.consumed[(size_t)k] = keep;
      buildClause(c, k, f);
      SemOblique o;
      o.np = tn;
      o.token = h;
      o.front = h < k;
      f.obliques.insert(f.obliques.begin(), o);
      f.tokens = sub;
      return;
    }
  }

  std::vector<SemOblique> lifted;
  std::vector<SemAdverb> liftedAdv;
  std::vector<SemOblique>* keepL = c.lifted;
  std::vector<SemAdverb>* keepA = c.liftedAdv;
  c.lifted = &lifted;
  c.liftedAdv = &liftedAdv;

  int cop = -1, nsubj = -1, obj = -1, iobj = -1, expl = -1, xcomp = -1, hidden = -1;
  std::vector<int> extra;   // dependents of a catenative complement that belong to this clause
  std::vector<int> auxes, marks;
  for (int k : c.kids[(size_t)h]) {
    if (!c.ok(k)) continue;
    const std::string d = c.dep(k);
    if (d == "cop" && cop < 0) cop = k;
    else if (d == "aux") auxes.push_back(k);
    // C22: "There would be new birds": "there" parsed as the subject of a copula is the expletive
    else if (d == "nsubj" && nsubj < 0 && lang_ == SrcLang::En && c.t(k).lower == "there" && k < h) expl = k;
    else if ((d == "nsubj" || d == "csubj") && nsubj < 0) nsubj = k;
    else if (d == "obj" && obj < 0) obj = k;
    else if (d == "iobj" && iobj < 0) iobj = k;
    else if (d == "expl") expl = k;
    else if (d == "mark") marks.push_back(k);
    else if (d == "xcomp" && xcomp < 0) xcomp = k;
  }
  const std::string hu = ht.upos;
  const std::string hl = c.lem(h);
  bool seImpers = false;   // Spanish impersonal "se" (3rd plural subject)

  // C17: an adjective standing as the object of an ordinary verb ("you have white in your frock", "witches wear
  // white"): a substantive (album), not a purpose clause
  if (en && hu == "VERB" && obj < 0 && xcomp >= 0 && c.t(xcomp).upos == "ADJ" &&
      in(hl, {"have", "wear", "like", "love", "want", "need", "see", "choose", "prefer", "hate", "find"})) {
    bool own = false;
    for (int g : c.kids[(size_t)xcomp])
      if (c.ok(g) && !in(c.dep(g), {"advmod", "det", "punct", "obl", "nmod"})) own = true;
    if (!own) {
      for (int g : c.kids[(size_t)xcomp])   // "in your frock" belongs to the verb
        if (c.ok(g) && in(c.dep(g), {"obl", "nmod"})) {
          c.s.tokens[(size_t)g].head = h + 1;
          c.s.tokens[(size_t)g].deprel = "obl";
          c.par[(size_t)g] = h;
          auto& kx = c.kids[(size_t)xcomp];
          kx.erase(std::remove(kx.begin(), kx.end(), g), kx.end());
          c.kids[(size_t)h].push_back(g);
          std::sort(c.kids[(size_t)h].begin(), c.kids[(size_t)h].end());
        }
      c.s.tokens[(size_t)xcomp].deprel = "obj";
      obj = xcomp;
      xcomp = -1;
    }
  }
  // C17: "become / remain / seem" + a noun or adjective: the predicate of the subject ("I may become the King of
  // Beasts" -> Rēx Bēstiārum fīam), not a purpose clause
  if (en && hu == "VERB" && xcomp >= 0 && in(hl, {"become", "remain", "seem", "appear", "grow", "turn", "get"}) &&
      in(c.t(xcomp).upos, {"NOUN", "PROPN", "ADJ"}) && !(in(hl, {"grow", "turn", "get"}) && c.t(xcomp).upos != "ADJ")) {
    bool mark = false;
    for (int g : c.kids[(size_t)xcomp])
      if (c.ok(g) && c.dep(g) == "mark") mark = true;
    if (!mark) {
      if (c.t(xcomp).upos == "ADJ") {
        SemAdj a;
        a.lemma = c.lem(xcomp);
        a.token = xcomp;
        for (int g : c.kids[(size_t)xcomp])
          if (c.ok(g) && c.dep(g) == "advmod") { a.adverbs.push_back(c.t(g).lower); a.advTokens.push_back(g); }
        f.predAdj.push_back(a);
      } else {
        SemNP pn;
        buildNP(c, xcomp, pn);
        f.predicative.push_back(pn);
      }
      std::vector<int> xt;
      c.subtree(xcomp, xt);
      for (int q : xt) { c.consumed[(size_t)q] = 1; f.tokens.push_back(q); if (c.t(q).upos == "DET") c.drop(q, Drop::Article); }
      xcomp = -1;
    }
  }
  // C17: the object complement of a factitive verb ("They made him king.", "That doesn't make me any braver.", "The
  // rain made the road wet."): a noun / adjective xcomp, an adjective the parser hung on the verb as a conj without a
  // conjunction, or an adjective after the object noun
  if (en && hu == "VERB" && obj >= 0 &&
      in(hl, {"make", "call", "name", "elect", "appoint", "crown", "render", "paint", "consider", "keep", "leave"})) {
    int comp = -1;
    for (int k : c.kids[(size_t)h]) {
      if (!c.ok(k) || k < obj) continue;
      const std::string d = c.dep(k);
      const std::string& u = c.t(k).upos;
      bool mark = false, cc = false;
      for (int g : c.kids[(size_t)k])
        if (c.ok(g)) { mark = mark || c.dep(g) == "mark"; cc = cc || c.dep(g) == "cc"; }
      if (d == "xcomp" && in(u, {"NOUN", "PROPN", "ADJ"}) && !mark) comp = k;
      else if (d == "conj" && u == "ADJ" && !cc) comp = k;
      if (comp >= 0) break;
    }
    if (comp < 0 && in(c.t(obj).upos, {"NOUN", "PROPN", "PRON"}))
      for (int k : c.kids[(size_t)obj])
        if (c.ok(k) && k > obj && c.dep(k) == "amod" && c.t(k).upos == "ADJ" && !in(hl, {"keep", "leave"})) { comp = k; break; }
    if (comp < 0 && in(c.t(obj).upos, {"NOUN", "PROPN", "PRON"}))   // "made the girl her friend": a bare NP after it
      for (int k : c.kids[(size_t)obj]) {
        if (!c.ok(k) || k <= obj || c.dep(k) != "nmod" || !in(c.t(k).upos, {"NOUN", "PROPN"})) continue;
        bool cs = false;
        for (int g : c.kids[(size_t)k])
          if (c.ok(g) && c.dep(g) == "case") cs = true;
        if (!cs) { comp = k; break; }
      }
    if (comp >= 0) {
      const nlp::Token& ct = c.t(comp);
      if (ct.upos == "ADJ") {
        SemAdj a;
        a.lemma = c.lem(comp);
        a.token = comp;
        const std::string kl = ct.lower;
        if (kl != a.lemma) {
          if (kl.size() > 2 && kl.compare(kl.size() - 2, 2, "er") == 0) a.degree = feat::Comparative;
          if (kl.size() > 3 && kl.compare(kl.size() - 3, 3, "est") == 0) a.degree = feat::Superlative;
          if (kl == "better" || kl == "worse") a.degree = feat::Comparative;
          if (kl == "best" || kl == "worst") a.degree = feat::Superlative;
        }
        for (int g : c.kids[(size_t)comp]) {
          if (!c.ok(g)) continue;
          if (c.t(g).lower == "more" || c.t(g).lower == "most") a.degree = c.t(g).lower == "more" ? feat::Comparative : feat::Superlative;
          else if (c.dep(g) == "advmod" && c.t(g).lower != "any") { a.adverbs.push_back(c.t(g).lower); a.advTokens.push_back(g); }
        }
        f.objComplementAdj.push_back(a);
      } else {
        SemNP np;
        buildNP(c, comp, np);
        f.objComplement.push_back(np);
      }
      std::vector<int> compToks;
      c.subtree(comp, compToks);
      for (int k : compToks) {
        c.consumed[(size_t)k] = 1;
        f.tokens.push_back(k);
        if (c.t(k).upos == "DET") c.drop(k, Drop::Other);
      }
    }
  }

  // C17: a participle / adjective phrase after a comma describing the subject ("I am only a Scarecrow, stuffed with
  // straw.", "I am a Cowardly Lion, afraid of everything.", "He came home, tired and hungry."): a conj of the clause
  // head without its own conjunction or subject, after a comma
  if (en)
    for (int k : c.kids[(size_t)h]) {
      if (!c.ok(k) || c.dep(k) != "conj") continue;
      const nlp::Token& kt = c.t(k);
      if (k <= h && !(en && kt.upos == "VERB" && kt.lower.size() > 4 && kt.lower.compare(kt.lower.size() - 3, 3, "ing") == 0))
        continue;
      const uint32_t vf = fget(kt, nlp::morph::VerbFormShift);
      const bool part = kt.upos == "VERB" && vf == nlp::morph::VfPart && fget(kt, nlp::morph::TenseShift) == nlp::morph::TensePast;
      // C19: an -ing participle phrase between commas ("The shepherd, seeing the wolf, fled.")
      const bool ingPart = en && kt.upos == "VERB" && vf == nlp::morph::VfPart &&
                           fget(kt, nlp::morph::TenseShift) == nlp::morph::TensePres && k < h;
      if (kt.upos != "ADJ" && !part && !ingPart) continue;
      bool comma = k > 0 && c.t(k - 1).lower == ",", own = false;
      for (int g : c.kids[(size_t)k])
        if (c.ok(g) && in(c.dep(g), {"cc", "nsubj", "aux", "cop", "mark"}) && !(c.dep(g) == "cc" && g > k)) own = true;
      if (!comma || own) continue;
      SemFrame sf;
      sf.type = Kind::Frag;
      auto addAdj = [&](int a0) {
        const nlp::Token& at = c.t(a0);
        SemAdj a;
        a.lemma = c.lem(a0);
        a.token = a0;
        const uint32_t avf = fget(at, nlp::morph::VerbFormShift);
        if (at.upos == "VERB" || avf == nlp::morph::VfPart)
          a.participle = fget(at, nlp::morph::TenseShift) == nlp::morph::TensePres ? 2 : 1;
        for (int g : c.kids[(size_t)a0]) {
          if (!c.ok(g)) continue;
          if (c.dep(g) == "advmod") { a.adverbs.push_back(c.t(g).lower); a.advTokens.push_back(g); }
          if (c.dep(g) == "obj" && !sf.hasObject && in(c.t(g).upos, {"NOUN", "PROPN", "PRON"})) {   // C19: "lupum vidēns"
            buildNP(c, g, sf.object);
            sf.hasObject = true;
            continue;
          }
          if (in(c.dep(g), {"obl", "nmod"}) && in(c.t(g).upos, {"NOUN", "PROPN", "PRON"})) {
            std::string prep;
            for (int q : c.kids[(size_t)g])
              if (c.ok(q) && c.dep(q) == "case") prep = prep.empty() ? c.t(q).lower : prep + " " + c.t(q).lower;
            SemOblique o;
            o.prep = canonPrep(lang_, prep);
            buildNP(c, g, o.np);
            o.token = g;
            sf.obliques.push_back(o);
          }
        }
        sf.predAdj.push_back(a);
      };
      addAdj(k);
      for (int g : c.kids[(size_t)k])   // "tired and hungry"
        if (c.ok(g) && c.dep(g) == "conj" && c.t(g).upos == "ADJ") addAdj(g);
      std::vector<int> toks;
      c.subtree(k, toks);
      if (k > 0 && c.t(k - 1).lower == ",") toks.push_back(k - 1);
      for (int q : toks) {
        c.consumed[(size_t)q] = 1;
        sf.tokens.push_back(q);
        f.tokens.push_back(q);
        if (c.t(q).upos == "PUNCT" || c.t(q).upos == "ADP" || c.t(q).upos == "CCONJ" || c.t(q).upos == "DET")
          c.drop(q, c.t(q).upos == "PUNCT" ? Drop::Punct : Drop::Other);
      }
      f.secondary.push_back(sf);
    }

  // ---- predicate --------------------------------------------------------------------------------------------------

  bool verbal = hu == "VERB" || (hu == "AUX" && cop < 0);
  if (cop >= 0) {
    f.copula = true;
    f.hasPred = true;
    f.pred.lemma = "be";
    f.pred.token = cop;

    c.drop(cop, Drop::Copula);
  } else if (verbal) {
    f.hasPred = true;
    f.pred.lemma = hl;
    f.pred.token = h;
    if (hu == "AUX") {
      // "This one does." / "This one can." / "You must be": an auxiliary standing for the verb
      if (in(hl, {"do", "does", "did", "hacer"})) {
        // "what shall we do?" / "I'll do anything": "do" with an object is the verb itself (faciō), not an ellipsis
        bool object = false;
        for (int k : c.kids[(size_t)h])
          if (c.ok(k) && (c.dep(k) == "obj" || (c.t(k).upos == "PRON" && k < h && in(c.t(k).lower, {"what", "anything",
                                                                                                    "everything", "nothing", "something", "this", "that", "it"}) && c.dep(k) != "nsubj")))
            object = true;
        if (object) f.pred.lemma = "do";
        else f.pred.ellipsis = true;
      }
      // C22: "But you would." / "But he will.": a bare would / will stands for the previous clause's verb, in the
      // conditional / the future ("Sed dīcerēs.")
      if (en && in(c.t(h).lower, {"would", "'d", "will", "'ll", "shall"})) {
        bool more = false;
        for (int k : c.kids[(size_t)h])
          if (c.ok(k) && in(c.dep(k), {"obj", "xcomp", "ccomp", "cop", "obl"})) more = true;
        if (!more) {
          f.pred.ellipsis = true;
          f.pred.lemma = "do";
          c.s.doubt("ellipsis");   // the verb is taken from the previous clause: a guess (Check)
          if (in(c.t(h).lower, {"would", "'d"})) f.pred.mood = SrcMood::Conditional;
          else f.pred.tense = Tense::Future;
        }
      }
      if (in(hl, {"can", "could", "poder"})) { f.pred.lemma = "can"; f.pred.pastModal = hl == "could"; }
      if (in(hl, {"must", "should"})) { f.pred.lemma = "be"; f.pred.modality = Modality::Must; }
      if (hl == "be" || hl == "ser" || hl == "estar") f.pred.lemma = "be";
    }
  }
  // "Am I mad?": a 'verb' after an inverted "be" that the lexicon knows only as an adjective
  if (verbal && hu == "VERB" && lex_ && en && fget(ht, nlp::morph::VerbFormShift) != nlp::morph::VfPart &&
      fget(ht, nlp::morph::VerbFormShift) != nlp::morph::VfGer) {
    bool beA = false;
    for (int a : auxes)
      if (c.lem(a) == "be") beA = true;
    if (beA) {
      std::vector<lex::Analysis> an;
      lex_->lookup(text::en_key(ht.lower), an);
      bool adj = false, verb = false;
      for (const lex::Analysis& a : an) {
        const uint8_t pos = lex_->lemma(a.lemma).pos;
        adj = adj || pos == feat::Adj;
        verb = verb || pos == feat::Verb;
      }
      const bool bareTag = fget(ht, nlp::morph::VerbFormShift) == 0 && fget(ht, nlp::morph::TenseShift) == 0;
      if (adj && (!verb || bareTag)) {
        for (size_t i = 0; i < auxes.size(); ++i)
          if (c.lem(auxes[i]) == "be") { cop = auxes[i]; auxes.erase(auxes.begin() + (long)i); break; }
        verbal = false;
        f.copula = true;
        f.hasPred = true;
        f.pred.lemma = "be";
        f.pred.token = cop;
        c.drop(cop, Drop::Copula);
      }
    }
  }
  // Spanish "hay / había / habrá" (haber without a participle) is the existential "there is" (C13)
  if (!en && verbal && hl == "haber" && hu == "VERB") {
    f.existential = true;
    f.pred.lemma = "be";
  }
  if (!en && cop >= 0 && c.lem(cop) == "haber") {   // the parser took "había" for a copula: existential too
    f.existential = true;
  }
  // Spanish "estar" + participle (C13): the resultant state of the verb's passive ("El reloj está roto" -> frāctum
  // est), unless states_es_la.tsv lists the adjective ("cansado", "enojado")
  if (!en && cop >= 0 && c.lem(cop) == "estar" && lex_ && (hu == "ADJ" || hu == "VERB") && !cd_.state(hl) &&
      !cd_.state(ht.lower)) {
    std::vector<lex::Analysis> an;
    lex_->lookup(text::es_key(ht.lower), an);
    std::string part;
    for (const lex::Analysis& a : an) {
      const lex::Lemma l = lex_->lemma(a.lemma);
      if (l.pos != feat::Verb || feat::unpack(lex_->feature(a.feat)).mood != feat::ParticipleMood) continue;
      const std::string hd = text::lower(l.head);
      if (part.empty() || (part.size() > 2 && part.compare(part.size() - 2, 2, "se") == 0)) part = hd;
    }
    // "cerradas" <- the participle lemma "cerrado": the verb is cerrar
    if (part.size() > 4 && (part.compare(part.size() - 3, 3, "ado") == 0 || part.compare(part.size() - 3, 3, "ido") == 0)) {
      const std::string stem = part.substr(0, part.size() - 3);
      const bool ado = part.compare(part.size() - 3, 3, "ado") == 0;
      for (const char* e : {"ar", "er", "ir"}) {
        if (ado != (std::string(e) == "ar")) continue;
        std::vector<lex::Analysis> a2;
        lex_->lookup(text::es_key(stem + e), a2);
        bool verb = false;
        for (const lex::Analysis& x : a2)
          verb = verb || (lex_->lemma(x.lemma).pos == feat::Verb && text::lower(lex_->lemma(x.lemma).head) == stem + e);
        if (verb) { part = stem + e; break; }
      }
    }
    if (!part.empty()) {
      f.copula = false;
      verbal = true;
      f.pred.lemma = part;
      f.pred.token = h;
      f.pred.voice = Voice::Passive;
      c.drop(cop, Drop::Aux);
      if (fget(c.t(cop), nlp::morph::TenseShift) == nlp::morph::TensePast ||
          fget(c.t(cop), nlp::morph::TenseShift) == nlp::morph::TenseImp)
        f.pred.tense = Tense::Past;
    }
  }
  const bool adjHead = hu == "ADJ" || (f.copula && hu == "VERB" && cop >= 0);
  // aux chain
  bool haveAux = false, beAux = false;
  for (int a : auxes) {
    const std::string al = c.t(a).lower;
    const std::string alem = c.lem(a);
    f.pred.auxTokens.push_back(a);
    c.drop(a, Drop::Aux);
    if (en) {
      if (al == "will" || al == "'ll" || al == "wo") f.pred.tense = Tense::Future;
      else if (al == "shall") { f.pred.tense = Tense::Future; f.pred.deliberative = true; }

      else if (al == "would" || al == "'d") f.pred.mood = SrcMood::Conditional;
      else if (al == "can") f.pred.modality = Modality::Can;
      else if (al == "could") { f.pred.modality = Modality::Can; f.pred.pastModal = true; }
      else if (al == "must") f.pred.modality = Modality::Must;
      else if (al == "may" || al == "might") f.pred.modality = Modality::May;
      else if (al == "should" || al == "ought") f.pred.modality = Modality::Should;
      else if (alem == "do") {
        if (fget(c.t(a), nlp::morph::TenseShift) == nlp::morph::TensePast) f.pred.tense = Tense::Past;
        if (fget(c.t(a), nlp::morph::MoodShift) == nlp::morph::MoodImp && !c.question) f.type = Kind::Imp;   // not "don't you know?" (C15)
      } else if (alem == "have") {
        haveAux = true;
        f.pred.aspect = Aspect::Perfect;
        if (fget(c.t(a), nlp::morph::TenseShift) == nlp::morph::TensePast || al == "had") f.pred.tense = Tense::Past;
      } else if (alem == "be") {
        beAux = true;
        // "has been kissed" is a present perfect: "been" (a participle tagged past) does not make the clause past
        if ((fget(c.t(a), nlp::morph::TenseShift) == nlp::morph::TensePast && al != "been") || al == "was" || al == "were")
          f.pred.tense = Tense::Past;
      } else if (al == "to") {
        c.drop(a, Drop::Marker);
      }
    } else {
      if (in(alem, {"haber", "he", "has", "ha", "hemos", "han", "había"})) {
        haveAux = true;
        f.pred.aspect = Aspect::Perfect;
        const uint32_t tt = fget(c.t(a), nlp::morph::TenseShift);
        if (tt == nlp::morph::TenseImp || tt == nlp::morph::TensePast) f.pred.tense = Tense::Past;
      } else if (in(alem, {"estar"})) {
        beAux = true;
        const uint32_t tt = fget(c.t(a), nlp::morph::TenseShift);
        if (tt == nlp::morph::TensePast || tt == nlp::morph::TenseImp) f.pred.tense = Tense::Past;
      } else if (in(alem, {"ser"})) {
        beAux = true;
      } else if (alem == "poder") f.pred.modality = Modality::Can;
      else if (alem == "deber") f.pred.modality = Modality::Must;
      else if (alem == "querer") f.pred.modality = Modality::Want;
      else if (alem == "ir") f.pred.tense = Tense::Future;
      // the tense of a modal auxiliary is the clause's ("podía sonreír" -> poterat, "pudo" -> potuit) (C13)
      if (alem == "poder" || alem == "deber" || alem == "querer") {
        const uint32_t tt = fget(c.t(a), nlp::morph::TenseShift);
        if (tt == nlp::morph::TenseImp) { f.pred.tense = Tense::Past; f.pred.pastModal = true; }
        else if (tt == nlp::morph::TensePast) f.pred.tense = Tense::Past;
        else if (tt == nlp::morph::TenseFut) f.pred.tense = Tense::Future;
        if (fget(c.t(a), nlp::morph::MoodShift) == nlp::morph::MoodCnd) f.pred.mood = SrcMood::Conditional;
      }
    }
  }
  if (f.hasPred && !verbal && cop >= 0 && auxes.empty()) {
    if (fget(c.t(cop), nlp::morph::TenseShift) == nlp::morph::TensePast ||
        fget(c.t(cop), nlp::morph::TenseShift) == nlp::morph::TenseImp)
      f.pred.tense = Tense::Past;
  }
  if (verbal && !ht.text.empty()) {
    const uint32_t vf = fget(ht, nlp::morph::VerbFormShift), tt = fget(ht, nlp::morph::TenseShift);
    // C19: a form that is past and participle alike, tagged as a finite past or a present participle after "be" ("The
    // boy was badly hurt", "The rope was cut"): passive
    bool partLike = false;
    if (en && beAux && vf != nlp::morph::VfGer && ht.lower.size() > 2 &&
        ht.lower.compare(ht.lower.size() - 3, 3, "ing") != 0 && !(vf == nlp::morph::VfPart && tt == nlp::morph::TensePast))
      partLike = in(ht.lower, {"hurt", "cut", "put", "hit", "set", "shut", "burst", "cast", "spread", "split", "shed",
                               "beaten", "broken", "eaten", "taken", "given", "stolen", "frozen", "chosen", "written"}) ||
                 (ht.lower.size() > 4 && ht.lower.compare(ht.lower.size() - 2, 2, "ed") == 0);
    if (en && beAux && ht.lower == "gone") {   // C15: "Oz is gone" -> Oz abiit (go away, perfect)
      f.pred.aspect = Aspect::Perfect;
      if (f.pred.particle.empty()) f.pred.particle = "away";
    } else
    if (beAux && ((vf == nlp::morph::VfPart && (tt == nlp::morph::TensePast || !en)) || partLike)) f.pred.voice = Voice::Passive;
    else if (beAux && (vf == nlp::morph::VfGer || (vf == nlp::morph::VfPart && tt != nlp::morph::TensePast)))
      f.pred.aspect = Aspect::Progressive;
    if (auxes.empty() && (tt == nlp::morph::TensePast || tt == nlp::morph::TenseImp)) f.pred.tense = Tense::Past;
    if (!en) {
      if (tt == nlp::morph::TenseFut) f.pred.tense = Tense::Future;
      const uint32_t md = fget(ht, nlp::morph::MoodShift);
      if (md == nlp::morph::MoodSub) f.pred.mood = SrcMood::Subjunctive;
      if (md == nlp::morph::MoodCnd) f.pred.mood = SrcMood::Conditional;
      if (md == nlp::morph::MoodImp) f.type = Kind::Imp;
    }
  }
  (void)haveAux;

  // Spanish "ir a + infinitivo" (C13): the infinitive with the marker "a" is the complement, whatever the parser
  // called it ("Va a llover", "¿Qué voy a hacer?")
  if (!en && verbal && hl == "ir") {
    for (int k : c.kids[(size_t)h]) {
      if (!c.ok(k) || c.t(k).upos != "VERB" || fget(c.t(k), nlp::morph::VerbFormShift) != nlp::morph::VfInf) continue;
      bool a = false;
      for (int g : c.kids[(size_t)k])
        if (c.ok(g) && c.t(g).lower == "a" && g < k) a = true;
      if (k > 0 && c.t(k - 1).lower == "a") a = true;
      if (a) { xcomp = k; break; }
    }
  }
  // catenative verbs: want to / have to / going to / let us / know how to / used to
  if (verbal && xcomp < 0) {
    for (int k : c.kids[(size_t)h]) {
      if (!c.ok(k) || (c.dep(k) != "ccomp" && c.dep(k) != "advcl") || c.t(k).upos != "VERB") continue;
      bool to = false;
      for (int g : c.kids[(size_t)k])
        if (c.ok(g) && (c.dep(g) == "mark" || c.dep(g) == "aux") && c.t(g).lower == "to") to = true;
      if (to) { xcomp = k; break; }
    }
  }
  if (verbal && xcomp >= 0 && (c.t(xcomp).upos == "VERB" || c.t(xcomp).upos == "AUX")) {
    const std::string xl = c.lem(xcomp);
    bool take = true;
    bool goParticle = false;   // C15: "I am now going away to make a visit": motion + purpose, not the future
    for (int k : c.kids[(size_t)h])
      if (c.ok(k) && particleWord(c.t(k).lower) && k > h && k < xcomp) goParticle = true;
    // C17: "I am going to the market to buy apples": a place between "going" and "to + verb" is motion + purpose
    if (en && hl == "go")
      for (int k = h + 1; k < xcomp; ++k)
        if (in(c.t(k).upos, {"NOUN", "PROPN", "ADP", "ADV"}) && c.t(k).lower != "to") goParticle = true;
    if (en && hl == "go" && f.pred.aspect == Aspect::Progressive && !goParticle) {   // "going to rain"
      f.pred.tense = Tense::Future;
      f.pred.aspect = Aspect::Simple;
      f.pred.lemma = xl;
      f.pred.token = xcomp;
      c.drop(h, Drop::Aux);
    } else if (!en && hl == "ir") {   // "voy a hacer" -> future (in the past: "iba a" -> past)
      if (f.pred.tense != Tense::Past) f.pred.tense = Tense::Future;
      // "Vamos a cantar." at the start of a statement is "let's sing" (es-MX)
      if (ht.lower == "vamos" && !c.question && nsubj < 0 && h == first) {
        f.pred.tense = Tense::Present;
        f.pred.modality = Modality::Let;
      }
      f.pred.lemma = xl;
      f.pred.token = xcomp;
      c.drop(h, Drop::Aux);
    } else if (in(hl, {"want", "wish", "querer", "desear"})) {
      f.pred.modality = Modality::Want;
      f.pred.lemma = xl;
      f.pred.token = xcomp;
      c.drop(h, Drop::Aux);
    } else if (in(hl, {"have", "need", "tener", "necesitar"}) && f.pred.aspect != Aspect::Perfect &&
               !(en && obj >= 0 && obj < xcomp)) {   // C26: "have time to think" is no "have to think"
      f.pred.modality = Modality::Must;
      f.pred.lemma = xl;
      f.pred.token = xcomp;
      c.drop(h, Drop::Aux);
    } else if ((hl == "let" && !(en && nsubj >= 0 && obj >= 0)) || hl == "dejar") {
      // C26: "let" with its own subject and an object ("She never let the dog come in") is "allow" (sinō + accusative +
      // infinitive), not the jussive of "let us go" / "let him go"
      f.pred.modality = Modality::Let;
      f.pred.lemma = xl;
      f.pred.token = xcomp;
      c.drop(h, Drop::Aux);
    } else if (hl == "use" && f.pred.tense == Tense::Past) {   // used to
      f.pred.habitual = true;
      f.pred.lemma = xl;
      f.pred.token = xcomp;
      c.drop(h, Drop::Aux);
    } else if (in(hl, {"poder"})) {
      f.pred.modality = Modality::Can;
      f.pred.lemma = xl;
      f.pred.token = xcomp;
      c.drop(h, Drop::Aux);
    } else if (in(hl, {"deber"})) {
      f.pred.modality = Modality::Must;
      f.pred.lemma = xl;
      f.pred.token = xcomp;
      c.drop(h, Drop::Aux);
    } else if (en && in(hl, {"like", "love"}) &&
               (f.pred.mood == SrcMood::Conditional || f.pred.modality == Modality::Should)) {
      // C15 modal.like: "I should like to cry" / "I would like to go" -> velim + infinitive
      f.pred.modality = Modality::Want;
      f.pred.mood = SrcMood::Subjunctive;
      f.pred.tense = Tense::Present;
      f.pred.lemma = xl;
      f.pred.token = xcomp;
      c.drop(h, Drop::Aux);
    } else if (en && hl == "like" && obj < 0) {
      // C15: "I always like to help" -> semper libenter adiuvō (the complement is the verb, "gladly")
      f.pred.lemma = xl;
      f.pred.token = xcomp;
      SemAdverb g;
      g.lemma = "gladly";
      g.token = h;
      f.adverbs.push_back(g);
      c.drop(h, Drop::Aux);
    } else if (en && hl == "go" && obj < 0 && c.t(xcomp).lower.size() > 4 &&
               c.t(xcomp).lower.compare(c.t(xcomp).lower.size() - 3, 3, "ing") == 0 && !goParticle) {
      // C22: "go + -ing" ("The clouds go rolling by", "We went fishing"): the -ing verb is the clause's verb
      f.pred.lemma = xl;
      f.pred.token = xcomp;
      c.drop(h, Drop::Aux);
    } else if (en && hl == "keep" && obj < 0 && c.t(xcomp).lower.size() > 4 &&
               c.t(xcomp).lower.compare(c.t(xcomp).lower.size() - 3, 3, "ing") == 0) {
      // C22: "I keep wishing", "The baby keeps crying" -> semper optō, semper flet (keep + -ing = again and again)
      f.pred.lemma = xl;
      f.pred.token = xcomp;
      SemAdverb g;
      g.lemma = "always";
      g.token = h;
      f.adverbs.push_back(g);
      c.drop(h, Drop::Aux);
    } else if (in(hl, {"try", "begin", "start", "know", "like", "love", "learn", "forget", "hope", "decide", "seem",
                       "dare", "prefer", "continue", "stop", "saber", "intentar", "empezar", "comenzar"}) ||
               (en && obj < 0 && in(hl, {"help", "refuse", "promise", "fail", "manage", "hate", "fear", "cease",
                                         "intend", "plan", "attempt", "deserve", "mean", "agree"}))) {   // C15: "help to keep away"; C22: agree
      f.pred.complementVerb = xl;
      f.pred.complementToken = xcomp;
    } else {
      take = false;
    }
    if (take) {
      // the complement's own dependents belong to this clause
      for (int k : c.kids[(size_t)xcomp]) {
        if (!c.ok(k)) continue;
        const std::string d = c.dep(k);
        if (d == "mark" || d == "aux") { c.drop(k, Drop::Marker); c.take(k); continue; }
        if (d == "advmod" && c.t(k).lower == "how") { c.drop(k, Drop::Marker); c.take(k); continue; }
        if (d == "obj" && obj < 0) obj = k;
        else if (d == "iobj" && iobj < 0) iobj = k;
        extra.push_back(k);
      }
      hidden = xcomp;
    }
  }
  if (f.pred.modality == Modality::Let && obj >= 0) {
    const std::string ol = c.t(obj).lower;
    if (ol == "us") { f.hasSubject = true; f.subject.isPronoun = true; f.subject.pron.person = 1; f.subject.pron.number = 2;
                      f.subject.number = 2; f.subject.pronLemma = "we"; f.subject.token = obj; f.subject.tokens.push_back(obj); }
    else { SemNP s; buildNP(c, obj, s); f.hasSubject = true; f.subject = s; }
    c.take(obj);
    obj = -1;
    f.type = Kind::Decl;
  }

  // phrasebook "vp" rows ("play cards" -> chartīs lūdere): the row starts at the predicate verb; the tokens it covers
  // are not analysed further (the transfer stage uses the row's Latin)
  if (verbal && f.pred.token >= 0)
    for (const PhraseMatch& m : c.vpHits)
      if (m.first == f.pred.token && m.slots.empty()) {
        // C17: a row noted "(transitive)" needs the verb's object ("I can starve you" -> tē famē cōnficere, not "they
        // starved")
        if (en && m.entry >= 0 && (size_t)m.entry < cd_.phrasebook().size() &&
            cd_.phrasebook()[(size_t)m.entry].note.find("(transitive)") != std::string::npos && obj < 0)
          continue;
        f.pred.fixedLatin = m.latin;
        f.pred.fixedEntry = m.entry;
        for (int k = m.first + 1; k <= m.last; ++k) {
          c.take(k);
          c.drop(k, Drop::Phrase);
          if (k == obj) obj = -1;
        }
        break;
      }

  // ---- subject -------------------------------------------------------------------------------------------------------
  if (nsubj >= 0) {
    // an "nsubj" with its own preposition is an oblique the parser misread ("Behind the door was a garden")
    bool hasCase = false;
    for (int g : c.kids[(size_t)nsubj])
      if (c.ok(g) && c.dep(g) == "case") hasCase = true;
    SemNP s;
    buildNP(c, nsubj, s);
    if (hasCase && (hu == "NOUN" || hu == "PROPN" || (en && hu == "PRON")) && cop >= 0) {   // C22: PRON too
      std::string prep;
      for (int g : c.kids[(size_t)nsubj])
        if (c.ok(g) && c.dep(g) == "case") prep = prep.empty() ? c.t(g).lower : prep + " " + c.t(g).lower;
      SemOblique o;
      o.prep = canonPrep(lang_, prep);
      o.np = s;
      o.token = nsubj;
      o.front = nsubj < h;
      f.obliques.push_back(o);
      // C22: "In my dream the trees were nothing but flowers": a second subject after the phrase is the subject and
      // the root stays the predicate
      int second = -1;
      for (int k : c.kids[(size_t)h])
        if (c.ok(k) && k != nsubj && c.dep(k) == "nsubj" && k > nsubj) { second = k; break; }
      if (en && second >= 0) {
        SemNP s2;
        buildNP(c, second, s2);
        f.hasSubject = true;
        f.subject = s2;
      } else {
      SemNP subj;
      buildNP(c, h, subj);
      f.hasSubject = true;
      f.subject = subj;
      f.existential = false;
      f.copula = false;
      f.pred.lemma = "be";
      }
    } else if (!f.hasSubject) {
      f.hasSubject = true;
      f.subject = s;
    }
  }
  if (expl >= 0) {
    const std::string el = c.t(expl).lower;
    c.drop(expl, Drop::Marker);
    if (el == "there") f.existential = true;
    if (el == "it") f.pred.impersonal = true;
  }

  // ---- predicate of copula clauses -----------------------------------------------------------------------------------
  // C22: English too: "There will be cake for everyone." (the noun is the root with the copula)
  if (f.copula && f.existential && cop >= 0 && !f.hasSubject && (hu == "NOUN" || hu == "PROPN" || (!en && hu == "PRON"))) {
    SemNP p;   // "Había una niña": the noun is what there is
    buildNP(c, h, p);
    f.hasSubject = true;
    f.subject = p;
  }
  // C19: "She is in the garden.", "She had never been to the sea.": the root noun carries a preposition, so the
  // predicate is a place, not a noun ("In hortō est", not "Hortus est")
  int predCase = -1;
  if (f.copula && !f.existential && en && (hu == "NOUN" || hu == "PROPN" || hu == "PRON"))
    for (int g : c.kids[(size_t)h])
      if (c.ok(g) && c.dep(g) == "case" && g < h && c.t(g).upos == "ADP" && !in(c.t(g).lower, {"'s", "of", "than", "as"}))
        { predCase = g; break; }
  if (predCase >= 0) {
    std::string prep;
    for (int g : c.kids[(size_t)h])
      if (c.ok(g) && c.dep(g) == "case" && g < h && c.t(g).upos == "ADP") {
        prep = prep.empty() ? c.t(g).lower : prep + " " + c.t(g).lower;
        for (int x : c.kids[(size_t)g])
          if (c.ok(x) && c.dep(x) == "fixed") prep += " " + c.t(x).lower;
        c.drop(g, Drop::Marker);
        c.take(g);
      }
    SemOblique o;
    o.prep = canonPrep(lang_, prep == "to" ? std::string("to") : prep);
    buildNP(c, h, o.np);
    o.token = h;
    f.obliques.push_back(o);
    f.copula = false;   // "be" as a full verb with a place: est / fuerat
  } else if (f.copula && !f.existential) {
    if (adjHead || (hu == "VERB" && fget(ht, nlp::morph::VerbFormShift) == nlp::morph::VfPart)) {
      SemAdj a;
      a.lemma = hu == "ADJ" ? hl : c.t(h).lower;
      a.token = h;
      // C15: degree from the form ("greater", "braver", "best") and from "more" / "most"
      if (en && hu == "ADJ") {
        const std::string& kl = c.t(h).lower;
        if (kl != a.lemma) {
          if (kl.size() > 2 && kl.compare(kl.size() - 2, 2, "er") == 0) a.degree = feat::Comparative;
          if (kl.size() > 3 && kl.compare(kl.size() - 3, 3, "est") == 0) a.degree = feat::Superlative;
        }
        if (kl == "better" || kl == "worse") a.degree = feat::Comparative;
        if (kl == "best" || kl == "worst") a.degree = feat::Superlative;
      }
      for (int k : c.kids[(size_t)h])
        if (c.ok(k) && c.dep(k) == "advmod" && en && in(c.t(k).lower, {"more", "most"})) {
          a.degree = c.t(k).lower == "more" ? feat::Comparative : feat::Superlative;
          c.drop(k, Drop::Marker);
          c.take(k);
        }
      for (int k : c.kids[(size_t)h])
        if (c.ok(k) && c.dep(k) == "advmod" && in(c.t(k).lower, {"very", "too", "so", "quite", "rather", "really",
                                                                  "extremely", "muy", "demasiado", "tan", "bastante",
                                                                  "as", "awfully", "terribly"})) {   // C22
          a.adverbs.push_back(c.t(k).lower);
          a.advTokens.push_back(k);
          c.take(k);
        }
      f.predAdj.push_back(a);
      if (en && c.t(h).lower == "gone" && f.predAdj.size() == 1) {   // C15: "Oz is gone" -> Oz abiit
        f.predAdj.clear();
        f.copula = false;
        f.pred.lemma = "go";
        f.pred.token = h;
        f.pred.particle = "away";
        f.pred.aspect = Aspect::Perfect;
        c.drop(cop, Drop::Aux);
      }
      for (int k : c.kids[(size_t)h])
        if (c.ok(k) && c.dep(k) == "conj" && (c.t(k).upos == "ADJ")) {
          SemAdj b;
          b.lemma = c.lem(k);
          b.token = k;
          for (int g : c.kids[(size_t)k])
            if (c.ok(g) && c.dep(g) == "advmod") { b.adverbs.push_back(c.t(g).lower); b.advTokens.push_back(g); c.take(g); }
            else if (c.ok(g) && c.dep(g) == "cc") { c.drop(g, Drop::Marker); c.take(g); }
          c.take(k);
          f.predAdj.push_back(b);
        }
    } else if (hu == "ADV" || (hu == "PRON" && c.t(h).lower == "where")) {
      // "Where is the ball?" / "You are here": locative predicate
      SemAdverb a;
      a.lemma = hl;
      a.token = h;
      const bool whw = in(c.t(h).lower, {"where", "dónde", "donde"});
      if (whw) { f.type = Kind::Wh; f.wh.word = "where"; f.wh.role = Role::Adverb; f.wh.token = h; }
      else f.adverbs.push_back(a);
      f.existential = whw;
    } else if (hu == "ADP") {
      // "It's in the garden" mis-headed: nothing to do
    } else if (!en && hu == "NUM" && numberValue(ht.lower) >= 1 && numberValue(ht.lower) <= 12 &&
               [&] { for (int k : c.kids[(size_t)h]) if (c.ok(k) && in(c.t(k).lower, {"la", "las"})) return true;
                     return false; }()) {
      // Spanish clock time (C13): "son las seis" / "es la una" -> hōra sexta est (subject "it", like "six o'clock")
      SemNP p;
      p.head = "hour";
      p.token = h;
      p.ordinal = true;
      p.numeral = ht.lower;
      p.numeralValue = numberValue(ht.lower);
      p.tokens.push_back(h);
      for (int k : c.kids[(size_t)h])
        if (c.ok(k) && c.dep(k) == "det") { p.tokens.push_back(k); c.drop(k, Drop::Article); c.take(k); }
      c.take(h);
      f.predicative.push_back(p);
      f.hasSubject = true;
      f.subject = SemNP{};
      f.subject.isPronoun = true;
      f.subject.pronLemma = "it";
      f.subject.pron.person = 3;
      f.subject.pron.number = 1;
      f.subject.pron.gender = feat::N;
      f.subject.number = 1;
    } else {
      SemNP p;
      buildNP(c, h, p);
      // "Who are you?": the interrogative is the predicate, the pronoun the subject
      if (f.hasSubject && f.subject.interrogative && !p.interrogative) {
        SemNP wh = f.subject;
        f.subject = p;
        p = wh;
      }
      f.predicative.push_back(p);
    }
  } else if (!f.hasPred && !en && ht.lower == "sí") {
    // Spanish "Esta sí." (C13): an affirmative reply standing for the previous verb ("This one does.")
    c.drop(h, Drop::Discourse);
    int dem = -1;
    for (int k : c.kids[(size_t)h])
      if (c.ok(k) && (c.t(k).upos == "DET" || c.t(k).upos == "PRON" || c.t(k).upos == "NOUN" || c.t(k).upos == "PROPN"))
        { dem = k; break; }
    if (dem >= 0) {
      buildNP(c, dem, f.subject);
      f.hasSubject = true;
      f.hasPred = true;
      f.pred.lemma = "do";
      f.pred.ellipsis = true;
      f.pred.token = h;
    } else {
      f.type = Kind::Frag;
      f.interjections.push_back("sí");
    }
  } else if (!f.hasPred) {
    // fragment / exclamation / vocative
    f.type = Kind::Frag;
    if (hu == "NOUN" || hu == "PROPN" || hu == "PRON" || hu == "NUM") {
      // C19: a cue that is only a prepositional phrase ("under the big table,", "with a loud cry.") keeps the case
      // of its preposition: an oblique of a verbless fragment, not a nominative subject that drops the preposition
      std::string prep;
      for (int g : c.kids[(size_t)h]) {
        if (!c.ok(g) || c.dep(g) != "case" || g > h || c.t(g).upos != "ADP") continue;
        const std::string gl = c.t(g).lower;
        if (gl == "'s" || gl == "of" || gl == "de") continue;
        prep = prep.empty() ? gl : prep + " " + gl;
        for (int x : c.kids[(size_t)g])
          if (c.ok(x) && c.dep(x) == "fixed") prep += " " + c.t(x).lower;
      }
      // C22: "Under the moon and over the sea": a conjunct with its own preposition is a second prepositional phrase
      // (et super mare), not a second noun of the first one ("Sub lūnā et marī" lost "over")
      std::vector<std::pair<int, std::string>> ppConj;
      if (en && !prep.empty())
        for (int k : c.kids[(size_t)h]) {
          if (!c.ok(k) || c.dep(k) != "conj" || k < h) continue;
          std::string p2;
          for (int g : c.kids[(size_t)k])
            if (c.ok(g) && c.dep(g) == "case" && g < k && c.t(g).upos == "ADP") p2 = p2.empty() ? c.t(g).lower : p2 + " " + c.t(g).lower;
          if (!p2.empty()) ppConj.emplace_back(k, p2);
        }
      // C22: "Over the hill or here or there": conjuncts that are place adverbs are coordinated adverbs (aut hīc aut
      // illīc), never nouns of the phrase ("[here]")
      std::vector<int> advConj;
      if (en)
        for (int k : c.kids[(size_t)h])
          if (c.ok(k) && c.dep(k) == "conj" && k > h && c.t(k).upos == "ADV" &&
              in(c.t(k).lower, {"here", "there", "everywhere", "somewhere", "nowhere", "anywhere", "home"}))
            advConj.push_back(k);
      // the conj chain: "or here or there" hangs "there" on "here"
      for (size_t q = 0; q < advConj.size(); ++q)
        for (int k : c.kids[(size_t)advConj[q]])
          if (c.ok(k) && c.dep(k) == "conj" && c.t(k).upos == "ADV" &&
              in(c.t(k).lower, {"here", "there", "everywhere", "somewhere", "nowhere", "anywhere", "home"}))
            advConj.push_back(k);
      std::sort(advConj.begin(), advConj.end());
      for (const auto& pc : ppConj) {
        auto& hk = c.kids[(size_t)h];
        hk.erase(std::remove(hk.begin(), hk.end(), pc.first), hk.end());
      }
      for (int k : advConj) {
        auto& hk = c.kids[(size_t)h];
        hk.erase(std::remove(hk.begin(), hk.end(), k), hk.end());
        for (auto& kk : c.kids) kk.erase(std::remove(kk.begin(), kk.end(), k), kk.end());
      }
      SemNP p;
      buildNP(c, h, p);
      for (const auto& pc : ppConj) {
        SemSub sb;
        sb.relation = Relation::Coord;
        for (int g : c.kids[(size_t)pc.first])
          if (c.ok(g) && c.dep(g) == "cc" && sb.marker.empty()) { sb.marker = canonConnector(lang_, c.t(g).lower); c.drop(g, Drop::Marker); c.take(g); }
        if (sb.marker.empty()) sb.marker = "and";
        for (int g : c.kids[(size_t)pc.first])
          if (c.ok(g) && c.dep(g) == "case" && g < pc.first && c.t(g).upos == "ADP") { c.drop(g, Drop::Marker); c.take(g); }
        SemFrame cf;
        cf.type = Kind::Frag;
        SemOblique o2;
        o2.prep = canonPrep(lang_, pc.second);
        buildNP(c, pc.first, o2.np);
        o2.token = pc.first;
        cf.obliques.push_back(o2);
        cf.tokens = o2.np.tokens;
        sb.frame.push_back(cf);
        f.subordinate.push_back(sb);
      }
      bool hereToo = false;
      for (int k : advConj) hereToo = hereToo || c.t(k).lower == "here";
      for (int k : advConj) {
        SemSub sb;
        sb.relation = Relation::Coord;
        int cc = -1;
        for (int g = k - 1; g > h && cc < 0; --g)
          if (c.t(g).upos == "CCONJ") cc = g;
        if (cc >= 0 && c.ok(cc)) { sb.marker = canonConnector(lang_, c.t(cc).lower); c.drop(cc, Drop::Marker); c.take(cc); }
        if (sb.marker.empty()) sb.marker = "and";
        SemFrame cf;
        cf.type = Kind::Frag;
        SemAdverb a;
        a.lemma = c.t(k).lower == "there" && hereToo ? "there-contrast" : c.t(k).lower;
        a.token = k;
        cf.adverbs.push_back(a);
        cf.tokens.push_back(k);
        c.take(k);
        sb.frame.push_back(cf);
        f.subordinate.push_back(sb);
      }
      if (p.determiner == "what" && !c.question) {   // "What a strange garden!"
        f.type = Kind::Excl;
        f.exclQuam = true;
      }
      if (!prep.empty() && f.type == Kind::Frag && !p.interrogative) {
        for (int g : c.kids[(size_t)h])
          if (c.ok(g) && c.dep(g) == "case" && g < h && c.t(g).upos == "ADP") { c.drop(g, Drop::Marker); c.take(g); }
        SemOblique o;
        o.prep = canonPrep(lang_, prep);
        o.np = p;
        o.token = h;
        o.front = true;
        f.obliques.push_back(o);
      } else {
        f.hasSubject = true;
        f.subject = p;
      }
    } else if (hu == "ADJ") {
      SemAdj a;
      a.lemma = hl;
      a.token = h;
      for (int k : c.kids[(size_t)h])
        if (c.ok(k) && c.dep(k) == "advmod") {
          if (in(c.t(k).lower, {"how", "qué"}) && !c.question) { f.type = Kind::Excl; f.exclQuam = true; c.drop(k, Drop::Marker); }
          else { a.adverbs.push_back(c.t(k).lower); a.advTokens.push_back(k); c.take(k); }   // once (C15: "Valdē valdē")
        }
      f.predAdj.push_back(a);
    } else if (hu == "ADV" || hu == "ADP") {
      SemAdverb a;
      a.lemma = hl;
      a.token = h;
      if (hu == "ADP" && hl == "off") {}   // handled by the phrasebook ("off with ...")
      f.adverbs.push_back(a);
    } else if (hu == "INTJ" && !canonConnector(lang_, c.t(h).lower).empty()) {   // "Besides," tagged INTJ (C15)
      f.connectors.push_back(canonConnector(lang_, c.t(h).lower));
      c.drop(h, Drop::Marker);
    } else if (hu == "INTJ") {
      f.interjections.push_back(c.t(h).lower);
    } else if (hu == "CCONJ" || hu == "SCONJ") {   // C15: "But, comrades, ...": the connector alone
      const std::string cn = canonConnector(lang_, c.t(h).lower);
      if (!cn.empty()) { f.connectors.push_back(cn); c.drop(h, Drop::Marker); }
      else if (c.t(h).lower == "for") { f.connectors.push_back("for"); c.drop(h, Drop::Marker); }
    }
  }
  if (f.hasPred && cop < 0 && verbal && f.pred.lemma == "be" && !f.existential && !f.copula) {
    // "be" as a full verb: "I am here" (parsed with "be" root): locative
  }

  // ---- other dependents ------------------------------------------------------------------------------------------------
  auto addAdverb = [&](int k) {
    const std::string kl = c.t(k).lower;
    SemAdverb a;
    a.lemma = c.lem(k);
    a.token = k;
    a.front = (k == first || k == firstContent) && k < h;
    f.adverbs.push_back(a);
    (void)kl;
  };
  std::vector<int> deps = c.kids[(size_t)h];
  deps.insert(deps.end(), extra.begin(), extra.end());
  for (int k : deps) {
    if (!c.free(k) || k == hidden) continue;
    c.take(k);
    const Token& kt = c.t(k);
    const std::string d = kt.deprel;
    const std::string kl = kt.lower;
    if (k == nsubj || k == cop || k == expl) continue;
    if (std::find(auxes.begin(), auxes.end(), k) != auxes.end()) continue;
    if (d == "punct") { c.drop(k, Drop::Punct); continue; }
    if (en && kl == "to" && c.kids[(size_t)k].empty() && k > h) {   // C15: "if I wanted to," (the verb is understood)
      bool last = true;
      for (int x : f.tokens)
        if (x > k && !isPunctTok(c.t(x))) last = false;
      if (last) { c.drop(k, Drop::Marker); c.s.doubt("ellipsis"); continue; }
    }
    // Spanish clitics normalised by es::normalise (C13): the particle of a pronominal verb, passive / impersonal "se"
    if (d == "expl:pv") { f.pred.particle = "se"; c.drop(k, Drop::Particle); continue; }
    if (d == "expl:pass") { f.pred.voice = Voice::Passive; f.pred.particle = "se"; c.drop(k, Drop::Particle); continue; }
    if (d == "expl:impers") { seImpers = true; c.drop(k, Drop::Particle); continue; }
    if (!en && kl == "sí" && kt.upos != "PRON") { c.drop(k, Drop::Discourse); continue; }   // emphatic "sí"
    if (d == "obj" && k == obj) {
      SemNP o;
      buildNP(c, k, o);
      f.hasObject = true;
      f.object = o;
      // C15 confidence: "make a visit", "take a walk", "have a look": the noun is the verb's meaning
      if (en && in(hl, {"make", "take", "have", "give", "pay", "do"}) && !o.isPronoun && !o.isName && o.determiner == "a")
        c.s.doubt("light-verb");
      // C17: "get" + a thing is take, receive, fetch or obtain by context: never OK on its own
      if (en && hl == "get" && !o.isPronoun && !o.isName) c.s.doubt("light-verb");
      continue;
    }
    if ((d == "iobj" && k == iobj) || (d == "obj" && k != obj && !f.hasIndirect && f.hasObject)) {
      SemNP o;
      buildNP(c, k, o);
      f.hasIndirect = true;
      f.indirectObject = o;
      continue;
    }
    if (d == "obj" || d == "iobj") {
      SemNP o;
      buildNP(c, k, o);
      if (!f.hasObject) { f.hasObject = true; f.object = o; }
      else if (!f.hasIndirect) { f.hasIndirect = true; f.indirectObject = o; }
      continue;
    }
    if (d == "obl" || (d == "nmod" && f.hasPred) || (d == "advmod" && kt.upos == "ADP" && !c.kids[(size_t)k].empty())) {
      std::string prep;
      bool poss = false;
      for (int g : c.kids[(size_t)k]) {
        if (!c.ok(g)) continue;
        if (c.dep(g) == "case") {
          const std::string gl = c.t(g).lower;
          if (gl == "'s") { poss = true; continue; }
          prep = prep.empty() ? gl : prep + " " + gl;
          for (int x : c.kids[(size_t)g])
            if (c.ok(x) && c.dep(x) == "fixed") prep += " " + c.t(x).lower;
          c.drop(g, Drop::Marker);
        }
      }
      (void)poss;
      if (kt.upos == "ADV") { addAdverb(k); continue; }
      if (!en && prep.empty() && kt.upos == "PRON" && in(kt.lower, {"me", "te", "le", "les", "nos", "os", "se"}) &&
          !f.hasIndirect) {   // a clitic without preposition: indirect object ("Dámelo")
        buildNP(c, k, f.indirectObject);
        f.hasIndirect = true;
        continue;
      }
      if (!en && prep == "por" && c.lem(k) == "favor") {   // "por favor" = please
        f.discourse.push_back("please");
        c.drop(k, Drop::Discourse);
        for (int g : c.kids[(size_t)k]) c.drop(g, Drop::Discourse);
        continue;
      }
      if (prep.empty() && !timeNoun(c.lem(k)) && kt.upos != "NOUN" && kt.upos != "PROPN" && kt.upos != "PRON") {
        addAdverb(k);
        continue;
      }
      SemOblique o;
      o.prep = canonPrep(lang_, prep);
      buildNP(c, k, o.np);
      o.token = k;
      {
        std::vector<int> st;
        c.subtree(k, st);
        int mn = k;
        for (int x : st) mn = std::min(mn, x);
        o.front = k < h && mn == first;
      }
      // "wh" inside an oblique: "on where you want to go"
      // (C28: also after its preposition: "To whom did she write it?", "With whom did you come?")
      if (o.np.isPronoun && o.np.interrogative && f.type != Kind::Wh && c.question && (k == first || o.front)) {
        f.type = Kind::Wh;
        f.wh.word = o.np.pronLemma;
        f.wh.role = Role::Oblique;
        f.wh.token = k;
      }
      f.obliques.push_back(o);
      continue;
    }
    if (d == "advmod" || (d == "compound" && (kt.upos == "ADP" || kt.upos == "ADV")) || d == "obl:tmod") {
      if (in(kl, {"not", "n't", "no", "never", "nunca", "jamás", "tampoco"}) && kt.upos != "INTJ") {
        f.negative = true;
        if (kl == "not" || kl == "n't" || kl == "no") c.drop(k, Drop::Marker);
        else addAdverb(k);
        continue;
      }
      // "When the teacher came, ...": a fronted wh adverb of an adverbial clause is its subordinator
      if (in(kl, {"when", "where", "while", "cuando", "donde"}) && c.par[(size_t)h] >= 0 && c.dep(h) == "advcl" &&
          k == first) {
        c.drop(k, Drop::Marker);
        continue;
      }
      // wh adverbs at the front of a question
      if (in(kl, {"where", "why", "how", "when", "whither", "dónde", "donde", "adónde", "cuándo", "cómo", "por qué"}) &&
          (c.question || k == first) && f.type != Kind::Wh) {
        if (kl == "how" && !c.question) { f.type = Kind::Excl; f.exclQuam = true; c.drop(k, Drop::Marker); continue; }
        f.type = Kind::Wh;
        f.wh.word = kl == "dónde" || kl == "donde" ? "where" : kl == "adónde" ? "whither"
                  : kl == "cuándo" ? "when" : kl == "cómo" ? "how" : kl == "por qué" ? "why" : kl;
        f.wh.role = Role::Adverb;
        f.wh.token = k;
        continue;
      }
      // a clause-final wh word after the verb ("I don't much care where."): an elliptical indirect question
      if (in(kl, {"where", "when", "why", "how"}) && k > h && f.hasPred && !c.question && c.kids[(size_t)k].empty()) {
        bool last = true;
        for (int x : f.tokens)
          if (x > k && !isPunctTok(c.t(x))) last = false;
        if (last) {
          addAdverb(k);
          f.adverbs.back().ellipticWh = true;
          continue;
        }
      }
      if (en && particleWord(kl) && f.hasPred && f.pred.particle.empty() && k > h && c.kids[(size_t)k].empty()) {
        f.pred.particle = kl;
        c.drop(k, Drop::Particle);   // accounted for by the phrasal verb (A7)
        continue;
      }
      // C15: "get back to Kansas": the particle carries the goal phrase; the goal belongs to the clause
      if (en && particleWord(kl) && f.hasPred && f.pred.particle.empty() && k > h && kt.upos == "ADV") {
        bool onlyObl = true;
        for (int g : c.kids[(size_t)k])
          if (c.ok(g) && !in(c.dep(g), {"obl", "nmod"})) onlyObl = false;
        if (onlyObl) {
          f.pred.particle = kl;
          c.drop(k, Drop::Particle);
          for (int g : c.kids[(size_t)k]) {
            if (!c.ok(g) || !c.free(g)) continue;
            std::string prep;
            for (int x : c.kids[(size_t)g])
              if (c.ok(x) && c.dep(x) == "case") { prep = prep.empty() ? c.t(x).lower : prep + " " + c.t(x).lower; c.drop(x, Drop::Marker); }
            SemOblique o;
            o.prep = prep;
            buildNP(c, g, o.np);
            o.token = g;
            c.take(g);
            f.obliques.push_back(o);
          }
          continue;
        }
      }
      // "then" / "so" at the start of the clause: connector
      if ((k == first || k == firstContent) && in(kl, {"then", "so", "entonces", "luego", "also", "therefore", "however",
                                                       "besides", "still"})) {
        c.drop(k, Drop::Marker);   // accounted for by the connector table
        f.connectors.push_back(kl == "entonces" || kl == "luego" ? "then" : kl);
        continue;
      }
      addAdverb(k);
      // "how many"/"too much" children stay with the adverb
      for (int g : c.kids[(size_t)k])
        if (c.ok(g) && c.dep(g) == "advmod" && in(c.t(g).lower, {"very", "too", "so", "muy"})) {
          f.adverbs.back().lemma = c.t(g).lower + " " + f.adverbs.back().lemma;
        }
      continue;
    }
    if (d == "discourse" || (d == "intj")) {
      const std::string w = kl;
      if (in(w, {"please", "por favor"})) { f.discourse.push_back("please"); continue; }
      if (in(w, {"well", "now", "so", "bueno", "pues"})) { f.discourse.push_back(w); c.drop(k, Drop::Discourse); continue; }
      f.interjections.push_back(w);
      continue;
    }
    if (d == "vocative") {
      SemNP v;
      buildNP(c, k, v);
      f.vocatives.push_back(v);
      continue;
    }
    if (d == "cc") {
      const std::string cn = canonConnector(lang_, kl);
      if (!cn.empty()) f.connectors.push_back(cn);
      c.drop(k, Drop::Marker);
      continue;
    }
    if (d == "mark") {
      Relation r;
      std::string canon;
      if (relationOf(lang_, kl, r, canon) && c.par[(size_t)h] < 0) {
        // a subordinator on a main clause ("Because the clock is broken."): connector
        f.connectors.push_back(canon);
        c.drop(k, Drop::Marker);
      } else {
        c.drop(k, Drop::Marker);
      }
      continue;
    }
    if (en && (d == "conj" || d == "advcl" || d == "parataxis") && kt.upos == "VERB" &&
        fget(kt, nlp::morph::VerbFormShift) == nlp::morph::VfPart) {   // C15: "a Scarecrow, stuffed with straw"
      bool own = false;
      for (int g : c.kids[(size_t)k])
        if (c.ok(g) && in(c.dep(g), {"nsubj", "aux", "cop"})) own = true;
      if (!own) c.s.doubt("participle-phrase");
    }
    if (d == "conj" && en && f.hasObject && f.object.coord.empty() && (kt.upos == "NOUN" || kt.upos == "PROPN")) {
      // C15: "to build this City, and my Palace" -> hanc Urbem et Rēgiam meam aedificāre: a bare noun coordinated with
      // the verb is a second object
      bool clauseLike = false;
      for (int g : c.kids[(size_t)k])
        if (c.ok(g) && in(c.dep(g), {"nsubj", "cop", "aux", "obj"})) clauseLike = true;
      if (!clauseLike) {
        SemNP g2;
        for (int g : c.kids[(size_t)k])
          if (c.ok(g) && c.dep(g) == "cc") { f.object.coordConj = canonConnector(lang_, c.t(g).lower); c.drop(g, Drop::Marker); c.take(g); }
        buildNP(c, k, g2);
        f.object.coord.push_back(g2);
        f.object.tokens.insert(f.object.tokens.end(), g2.tokens.begin(), g2.tokens.end());
        continue;
      }
    }
    if (d == "conj") {
      const std::string ku = kt.upos;
      if (f.copula && ku == "ADJ") continue;   // handled with the predicate
      // coordinated clause (same subject unless it has its own)
      SemSub s;
      s.relation = Relation::Coord;
      for (int g : c.kids[(size_t)k])
        if (c.ok(g) && c.dep(g) == "cc" && s.marker.empty()) {
          s.marker = canonConnector(lang_, c.t(g).lower);
          c.drop(g, Drop::Marker);
          c.take(g);   // C15: the conjunction is the coordination's, not a connector of the second clause ("et et")
        }
      if (s.marker.empty()) s.marker = "and";
      SemFrame cf;
      buildClause(c, k, cf);
      // C22: a second verb coordinated with a catenative complement ("agreed to meet him and give him the crown",
      // "wants to sing and dance") is a second complement of the same verb, not a finite verb of its own
      if (en && f.hasPred && cf.hasPred && !cf.hasSubject && hidden >= 0 && c.par[(size_t)k] == hidden &&
          cf.type == Kind::Decl && cf.pred.auxTokens.empty()) {
        if (!f.pred.complementVerb.empty()) {
          cf.pred.complementVerb = cf.pred.lemma;
          cf.pred.complementToken = cf.pred.token;
          cf.pred.lemma = f.pred.lemma;
        }
        cf.pred.modality = f.pred.modality;
        cf.pred.tense = f.pred.tense;
        cf.pred.mood = f.pred.mood;
        cf.pred.pastModal = f.pred.pastModal;
        cf.pred.aspect = f.pred.aspect;
      }
      // C22: a bare second verb shares the first one's auxiliary ("He will come and help us." -> veniet et nōs
      // adiuvābit; "They would sit and talk" -> sedērent et loquerentur): tense, mood and modality
      if (en && f.hasPred && cf.hasPred && !cf.hasSubject && cf.pred.auxTokens.empty() &&
          (!f.pred.auxTokens.empty() || f.pred.modality != Modality::None) &&   // C24: "May we go out and play?"
          cf.type == Kind::Decl && (f.type == Kind::Decl || f.type == Kind::Yn || f.type == Kind::Wh)) {
        const uint32_t vf = fget(kt, nlp::morph::VerbFormShift), tt = fget(kt, nlp::morph::TenseShift);
        const bool bare = vf == nlp::morph::VfInf || (tt == 0 && vf != nlp::morph::VfPart && vf != nlp::morph::VfGer);
        if (bare) {
          cf.pred.tense = f.pred.tense;
          cf.pred.mood = f.pred.mood;
          cf.pred.modality = f.pred.modality;
          cf.pred.pastModal = f.pred.pastModal;
          if (f.pred.aspect == Aspect::Perfect) cf.pred.aspect = Aspect::Perfect;
        }
      }
      if (f.type == Kind::Imp && cf.type == Kind::Decl && !cf.hasSubject) cf.type = Kind::Imp;
      // C15: the second verb of a question shares the question ("Why don't you run and jump?" -> ... et salīs?)
      if (cf.type == Kind::Yn && !cf.hasSubject) cf.type = Kind::Decl;
      s.frame.push_back(cf);
      f.subordinate.push_back(s);
      continue;
    }
    if (d == "advcl" || d == "ccomp" || d == "xcomp" || d == "parataxis" || d == "csubj") {
      SemSub s;
      std::string marker;
      bool found = false;
      for (int g : c.kids[(size_t)k]) {
        if (!c.ok(g)) continue;
        if (c.dep(g) == "mark") {
          std::string canon;
          Relation r;
          const std::string gl = c.t(g).lower;
          // two-word markers: "so that", "even though", "in order to"
          if (relationOf(lang_, gl, r, canon)) {
            if (!found || canon == "to") { s.relation = r; marker = canon; found = true; }
          }
          // C17: "so that" + clause is purpose (ut + subjunctive) when "so" stands right before "that" ("We ran so
          // that we could catch the bus"; "so tired that" is a result and has the adjective in between)
          if (en && gl == "that" && g > 0 && c.t(g - 1).lower == "so") {
            s.relation = Relation::Purpose;
            marker = "so that";
            found = true;
            c.consumed[(size_t)g - 1] = 1;
            c.drop(g - 1, Drop::Marker);
          }
          // C26: "without + -ing" ("walked for hours without finding water"): a negated coordinated clause of the
          // same subject (nec aquam invēnērunt); the clause is marked negative below
          if (en && gl == "without" && c.t(k).upos == "VERB") { s.relation = Relation::Coord; marker = "without"; found = true; }
          // C17: "as" after its main clause compares ("as men call me", "as I promised"): ut + indicative
          if (en && gl == "as" && k > h && found && marker == "because") { s.relation = Relation::Manner; marker = "as"; }
          c.drop(g, Drop::Marker);
        }
      }
      if (!found) {
        if (d == "ccomp" || d == "csubj") { s.relation = Relation::Complement; marker = "that"; }
        else if (d == "xcomp") { s.relation = Relation::Purpose; marker = "to"; }
        else if (d == "parataxis") { s.relation = Relation::Coord; marker = ""; }
        else { s.relation = Relation::Time; marker = "when"; }
      }
      // C15: verb + object + (to) infinitive. Verbs of ordering / teaching / allowing / wanting take the object and
      // the infinitive ("I ordered them to build this City" -> Eōs iussī hanc Urbem aedificāre); verbs of helping /
      // asking / urging take ut + subjunctive with the object as its subject ("help me find my way" -> mē adiuvāre ut
      // viam inveniam)
      if (en && (d == "xcomp" || d == "ccomp" || d == "advcl") && (marker == "to" || d == "xcomp") &&
          c.t(k).upos == "VERB" && (obj >= 0 || f.hasObject)) {
        if (in(hl, {"order", "command", "teach", "allow", "permit", "want", "wish", "force", "compel", "forbid",
                    "let", "make", "bid", "like", "expect", "need"})) {
          s.relation = Relation::Complement;
          marker = "inf";
        } else if (in(hl, {"help", "ask", "beg", "persuade", "urge", "tell", "warn", "advise", "encourage", "invite",
                           "remind"})) {
          s.relation = Relation::Purpose;
          marker = "to-obj";
        }
      }
      // C17: a wh word + to-infinitive ("tell you how to use them", "tell him where to go", "what to do") is an
      // indirect question in the subjunctive (quōmodo eīs ūtāris), not a purpose clause; after teach / know the
      // infinitive stays ("docuit nōs nāre")
      int whTok = -1;
      if (en && (marker == "to" || marker == "inf" || marker == "to-obj")) {
        int toTok = -1;
        for (int g : c.kids[(size_t)k])
          if (c.dep(g) == "mark" && c.t(g).lower == "to") toTok = g;
        if (toTok > 0 && in(c.t(toTok - 1).lower, {"how", "where", "what", "which", "when", "who", "whom"})) whTok = toTok - 1;
      }
      if (whTok >= 0 && !in(hl, {"teach", "learn", "know"})) {
        s.relation = Relation::Complement;
        marker = "towh";
      }
      s.marker = marker;
      s.before = k < h;
      if (en && s.relation == Relation::Purpose && marker == "to" &&
          !in(hl, {"go", "come", "run", "walk", "hurry", "travel", "return", "send", "get"}))
        c.s.doubt("purpose-guess");
      SemFrame sf;
      const bool whFree = whTok >= 0 && c.ok(whTok) && c.par[(size_t)whTok] != k;   // "where" hung on the main verb
      if (whFree) c.consumed[(size_t)whTok] = 1;
      buildClause(c, k, sf);
      // C15: a dependent clause is never a yes/no question of its own ("help me find my way?" -> ut viam inveniam)
      if (sf.type == Kind::Yn) sf.type = Kind::Decl;
      if (marker == "without") { sf.negative = true; s.before = false; }   // C26
      if (whTok >= 0) {
        const std::string ww = c.t(whTok).lower;
        // the wh word belongs to the dependent clause; the main clause loses it
        f.adverbs.erase(std::remove_if(f.adverbs.begin(), f.adverbs.end(), [&](const SemAdverb& a) { return a.token == whTok; }),
                        f.adverbs.end());
        if (marker == "towh") {
          if (sf.type != Kind::Wh || sf.wh.word.empty()) {
            sf.type = Kind::Wh;
            sf.wh.word = ww;
            sf.wh.token = whTok;
            sf.wh.role = in(ww, {"what", "which", "who", "whom"}) && !sf.hasObject ? Role::Object : Role::Adverb;
          }
          sf.exclQuam = false;
          if (sf.wh.role == Role::Adverb) c.drop(whTok, Drop::No);
          sf.tokens.push_back(whTok);
        } else {   // teach / know how to: the plain infinitive
          if (sf.type == Kind::Excl || sf.type == Kind::Wh) { sf.type = Kind::Decl; sf.exclQuam = false; sf.wh = SemWh{}; }
          c.drop(whTok, Drop::Marker);
        }
      }
      s.frame.push_back(sf);
      f.subordinate.push_back(s);
      continue;
    }
    if (d == "dep" || d == "appos" || d == "list" || d == "orphan" || d == "reparandum" || d == "goeswith") {
      if (kt.upos == "NOUN" || kt.upos == "PROPN") {
        SemNP v;
        buildNP(c, k, v);
        f.vocatives.push_back(v);
      }
      continue;
    }
    if (d == "nsubj" || d == "csubj") {   // a second subject: ignored (A7 reports it), an expletive "it" is dropped
      if (kl == "it" && (f.copula || f.hasSubject)) c.drop(k, Drop::Marker);
      continue;
    }
  }
  // Spanish existential (C13): "No hay té" -> the "object" of haber is the subject; "Había una niña" (haber read as a
  // copula) -> the predicate noun is the subject
  if (!en && f.existential) {
    if (!f.hasSubject && f.hasObject) { f.hasSubject = true; f.subject = f.object; f.hasObject = false; f.object = SemNP{}; }
    if (!f.hasSubject && !f.predicative.empty()) { f.hasSubject = true; f.subject = f.predicative[0]; f.predicative.clear(); }
    f.copula = false;
    f.pred.lemma = "be";
    // "No hay té." -> Nūlla thēa est (like "There isn't any tea")
    if (f.negative && f.hasSubject && !f.subject.isPronoun && f.subject.determiner.empty() && f.subject.numeral.empty())
      { f.subject.determiner = "no"; f.subject.negative = true; }
  }
  // lifted obliques and adverbs from NPs
  for (SemOblique& o : lifted) f.obliques.push_back(o);
  for (SemAdverb& a : liftedAdv) {
    if (in(a.lemma, {"not", "n't", "no"})) { f.negative = true; c.drop(a.token, Drop::Marker); continue; }
    if (a.lemma == "never" || a.lemma == "nunca") f.negative = true;
    if (in(a.lemma, {"where", "why", "how", "when"}) && c.question && f.type != Kind::Wh) {
      f.type = Kind::Wh;
      f.wh.word = a.lemma;
      f.wh.role = Role::Adverb;
      f.wh.token = a.token;
      continue;
    }
    f.adverbs.push_back(a);
  }
  c.lifted = keepL;
  c.liftedAdv = keepA;
  // floating "all" with a pronoun subject ("You're all very rude"): the subject is plural, quantified
  for (size_t i = 0; i < f.adverbs.size(); ++i)
    if (f.adverbs[i].lemma == "all" && f.hasSubject && f.subject.isPronoun && f.subject.pron.person < 3 &&
        f.subject.pronLemma != "all" && f.subject.pron.number != 1) {
      f.subject.pron.number = 2;
      f.subject.number = 2;
      f.subject.determiner = "all";
      c.drop(f.adverbs[i].token, Drop::Marker);
      f.adverbs.erase(f.adverbs.begin() + (long)i);
      break;
    }
  // a time word parsed as the subject ("What day is it today?") is an adverb
  if (f.hasSubject && !f.subject.isPronoun && f.subject.determiner.empty() &&
      in(f.subject.head, {"today", "tomorrow", "yesterday", "tonight"}) && (f.copula || !f.predicative.empty())) {
    SemAdverb a;
    a.lemma = f.subject.head;
    a.token = f.subject.token;
    f.adverbs.push_back(a);
    f.hasSubject = false;
    f.subject = SemNP{};
  }
  // Spanish (C13): person and number of the finite verb (an auxiliary, the clause head, or the predicate), from the
  // tagger, else from the lexicon when its readings agree ("Puedes")
  uint32_t esPe = 0, esNu = 0;
  int esFin = -1;
  if (!en && f.hasPred) {
    std::vector<int> cands = auxes;
    if (cop >= 0) cands.push_back(cop);
    cands.push_back(h);
    if (f.pred.token >= 0) cands.push_back(f.pred.token);
    for (int a : cands)
      if (fget(c.t(a), nlp::morph::PersonShift)) { esFin = a; break; }
    if (esFin < 0)
      for (int a : cands)
        if (fget(c.t(a), nlp::morph::VerbFormShift) == nlp::morph::VfFin) { esFin = a; break; }
    if (esFin < 0) esFin = h;
    esPe = fget(c.t(esFin), nlp::morph::PersonShift);
    esNu = fget(c.t(esFin), nlp::morph::NumberShift);
    if (!esPe && lex_) {
      std::vector<lex::Analysis> an;
      lex_->lookup(text::es_key(c.t(esFin).lower), an);
      uint8_t p0 = 0, n0 = 0;
      bool one = true;
      for (const lex::Analysis& a : an) {
        if (lex_->lemma(a.lemma).pos != feat::Verb) continue;
        const feat::Features ff = feat::unpack(lex_->feature(a.feat));
        if (!ff.person || ff.mood == feat::Imperative) continue;
        if (!p0) { p0 = ff.person; n0 = ff.number; }
        else if (ff.person != p0 || ff.number != n0) one = false;
      }
      if (p0 && one) { esPe = p0; esNu = n0 == feat::Pl ? nlp::morph::NumPlur : nlp::morph::NumSing; }
    }
    // a noun "subject" of a 1st / 2nd person verb is its object ("¿Cuántos hermanos tienes?")
    if (f.hasSubject && !f.hasObject && (esPe == 1 || esPe == 2) && !f.copula && !f.existential &&
        (!f.subject.isPronoun || (f.subject.pron.person == 3 && !f.subject.pronLemma.empty() &&
                                  f.subject.pronLemma != "everyone"))) {
      if (timeNoun(f.subject.head)) {   // "Todas las mañanas camino ...": a time oblique
        SemOblique o;
        o.np = f.subject;
        o.token = f.subject.token;
        o.front = f.subject.token < h;
        f.obliques.insert(f.obliques.begin(), o);
      } else {
        f.hasObject = true;
        f.object = f.subject;
      }
      f.hasSubject = false;
      f.subject = SemNP{};
    }
  }
  // inversion after a fronted oblique ("In the garden lived a cat"): the post-verbal object is the subject; Spanish
  // intransitive verbs also without the oblique ("En el jardín vivía un gato", "Llegó el maestro") (C13)
  if (((en && !f.obliques.empty() && f.obliques[0].token < h) ||
       (!en && (es::intransitiveVerb(f.pred.lemma) || (es::experiencerVerb(f.pred.lemma) && f.hasIndirect)) &&
        esPe != 1 && esPe != 2 && !f.object.isPronoun &&
        ((f.object.number == 2) == (esNu == nlp::morph::NumPlur)))) &&
      !f.hasSubject && f.hasObject && f.hasPred && !f.copula && f.type == Kind::Decl &&
      f.object.token > h && f.pred.voice == Voice::Active) {
    f.hasSubject = true;
    f.subject = f.object;
    f.hasObject = false;
    f.object = SemNP{};
    if (!f.obliques.empty() && f.obliques[0].token < h) f.obliques[0].front = true;
  }
  // Spanish passive "se" ("Se venden casas"): an object is the subject
  if (!en && f.pred.voice == Voice::Passive && !f.hasSubject && f.hasObject) {
    f.hasSubject = true;
    f.subject = f.object;
    f.hasObject = false;
    f.object = SemNP{};
  }

  // C22: "in a world of my own" with "of my own" hung on the clause: the possessive of the noun before it ("in mundō
  // meō", not "dē propriō meō")
  if (en)
    for (size_t i = 1; i < f.obliques.size(); ++i) {
      SemOblique& o = f.obliques[i];
      if (o.prep != "of" || text::lower(o.np.head) != "own" || o.np.possessor.empty()) continue;
      SemOblique& p = f.obliques[i - 1];
      if (!p.np.possessor.empty() || p.np.isPronoun) continue;
      p.np.possessor = o.np.possessor;
      for (int t : o.np.tokens) c.drop(t, Drop::Phrase);
      if (o.token >= 0) c.drop(o.token, Drop::Phrase);
      f.obliques.erase(f.obliques.begin() + (long)i);
      break;
    }
  // C22: "say hello (to X)" -> salūtāre (X in the accusative), "say goodbye (to X)" -> valedīcere (X in the dative):
  // the greeting is the verb, never a bracketed word
  if (en && f.hasPred && f.pred.lemma == "say" && f.hasObject && f.pred.fixedLatin.empty() &&
      in(text::lower(f.object.head), {"hello", "hi", "goodbye", "bye", "farewell", "good-bye", "goodnight"})) {
    const bool hello = in(text::lower(f.object.head), {"hello", "hi"});
    f.pred.lemma = hello ? "greet" : "say goodbye";
    for (int k : f.object.tokens) c.drop(k, Drop::Phrase);
    f.hasObject = false;
    f.object = SemNP{};
    for (size_t i = 0; i < f.obliques.size(); ++i)
      if (f.obliques[i].prep == "to") {
        if (hello) { f.object = f.obliques[i].np; f.hasObject = true; }
        else { f.indirectObject = f.obliques[i].np; f.hasIndirect = true; }
        if (f.obliques[i].token >= 0) c.drop(f.obliques[i].token, Drop::Marker);
        f.obliques.erase(f.obliques.begin() + (long)i);
        break;
      }
  }

  // ---- clause type -----------------------------------------------------------------------------------------------------
  // questions: "?" or aux/copula before the subject
  if (f.type != Kind::Wh && f.type != Kind::Excl && f.hasPred) {
    // English inversion marks a question; Spanish verb-subject order does not ("Son todos muy groseros.") (C13)
    bool inverted = en && nsubj >= 0 && ((cop >= 0 && cop < nsubj) || (!auxes.empty() && auxes.front() < nsubj));
    // C22: "There'd be flowers everywhere." / "There will be cake.": "there" before the auxiliary is no inversion
    if (inverted && expl >= 0 && !auxes.empty() && expl < auxes.front() && !c.question) inverted = false;
    // C15: "nor do I know ...", "never have I seen ...": inversion after a negative word is not a question
    if (inverted && !c.question)
      for (int i : f.tokens)
        if (!isPunctTok(c.t(i))) { if (in(c.t(i).lower, {"nor", "neither", "never", "only", "rarely", "seldom"})) inverted = false; break; }
    if (f.hasSubject && f.subject.interrogative && f.subject.isPronoun) {
      f.type = Kind::Wh;
      f.wh.word = f.subject.pronLemma;
      f.wh.role = Role::Subject;
      f.wh.token = f.subject.token;
    } else if (f.hasObject && f.object.interrogative) {
      f.type = Kind::Wh;
      f.wh.word = f.object.isPronoun ? f.object.pronLemma : f.object.wh;
      f.wh.role = Role::Object;
      f.wh.token = f.object.token;
    } else if (!f.predicative.empty() && f.predicative[0].interrogative) {
      f.type = Kind::Wh;
      f.wh.word = f.predicative[0].isPronoun ? f.predicative[0].pronLemma : f.predicative[0].wh;
      f.wh.role = Role::Predicate;
      f.wh.token = f.predicative[0].token;
    } else if (f.hasSubject && f.subject.interrogative) {
      f.type = Kind::Wh;
      f.wh.word = f.subject.wh;
      f.wh.role = Role::Subject;
      f.wh.token = f.subject.token;
    } else if (inverted || (c.question && f.type != Kind::Imp)) {
      if (f.type != Kind::Imp) f.type = Kind::Yn;
    }
  }
  if (f.type == Kind::Yn && f.negative && auxes.size() && auxes.front() < nsubj) f.expectYes = true;
  // C22: a "when" clause that is no question (a line of a song or a clause cut at a cue end: "When the clouds roll
  // by") is a time clause: cum + the verb, not "Quandō ...?"
  if (en && f.type == Kind::Wh && f.wh.word == "when" && !c.question && f.wh.token == first) {
    bool qmark = false;
    for (int t : sub) qmark = qmark || c.t(t).text == "?";
    if (!qmark) {
      f.type = Kind::Decl;
      if (f.wh.token >= 0) c.drop(f.wh.token, Drop::Marker);
      f.wh = SemWh{};
      f.connectors.insert(f.connectors.begin(), "when-time");
      c.s.doubt("fragment");   // a time clause without its main clause: Check
      f.punct.clear();
    }
  }
  // C22: "What if I should fall ...?" -> Quid sī ... cadam? (a condition in the present subjunctive, no -ne)
  if (en && std::find(f.connectors.begin(), f.connectors.end(), "what-if") != f.connectors.end()) {
    if (f.type == Kind::Yn) f.type = Kind::Decl;
    if (f.pred.modality == Modality::Should || f.pred.modality == Modality::Will) f.pred.modality = Modality::None;
    f.pred.mood = SrcMood::Subjunctive;
    if (f.pred.tense == Tense::Future) f.pred.tense = Tense::Present;
    for (SemSub& sb : f.subordinate)
      if (sb.relation == Relation::Coord && !sb.frame.empty() && !sb.frame[0].hasSubject && sb.frame[0].hasPred) {
        sb.frame[0].pred.mood = SrcMood::Subjunctive;
        sb.frame[0].pred.modality = f.pred.modality;
        sb.frame[0].pred.tense = f.pred.tense;
        if (sb.frame[0].type == Kind::Imp) sb.frame[0].type = Kind::Decl;
      }
  }
  // C22: "Will you kindly pay attention ...", "Would you please sit down?", "Could you kindly ...": a polite request
  // is an imperative with quaesō, not a question about the future
  if (en && f.type == Kind::Yn && !f.negative && f.hasSubject && f.subject.isPronoun && f.subject.pron.person == 2 &&
      !auxes.empty() && in(c.t(auxes.front()).lower, {"will", "would", "could", "can"}) && f.pred.modality != Modality::Want) {
    bool polite = std::find(f.discourse.begin(), f.discourse.end(), "please") != f.discourse.end();
    for (size_t q = 0; q < f.adverbs.size(); ++q)
      if (in(f.adverbs[q].lemma, {"kindly", "please"})) {
        polite = true;
        if (f.adverbs[q].token >= 0) c.drop(f.adverbs[q].token, Drop::Discourse);
        f.adverbs.erase(f.adverbs.begin() + (long)q);
        --q;
      }
    if (polite) {
      f.type = Kind::Imp;
      f.imperativePlural = f.subject.pron.number == 2;
      for (int t : f.subject.tokens) c.drop(t, Drop::Marker);
      if (f.subject.token >= 0) c.drop(f.subject.token, Drop::Marker);
      f.hasSubject = false;
      f.subject = SemNP{};
      f.pred.tense = Tense::Present;
      f.pred.modality = Modality::None;
      f.pred.mood = SrcMood::Indicative;
      f.pred.pastModal = false;
      if (std::find(f.discourse.begin(), f.discourse.end(), "please") == f.discourse.end()) f.discourse.push_back("please");
      c.s.doubt("polite-request");   // the engine writes "." for the "?" of the request
    }
  }
  // imperative: base verb at the start with no subject (or Mood=Imp), not a question
  if (f.hasPred && f.type == Kind::Decl && !c.question) {
    const Token& vt = c.t(f.pred.token >= 0 ? f.pred.token : h);
    const uint32_t md = fget(vt, nlp::morph::MoodShift), tt = fget(vt, nlp::morph::TenseShift);
    // first content token of the clause (skipping connectors, discourse, interjections, "then", "please")
    const int vtok0 = cop >= 0 ? cop : (f.pred.token >= 0 ? f.pred.token : h);
    int lead = -1;
    // C15: a fronted adverbial clause ("if you wish for anything, ring the bell") does not hide the imperative
    std::vector<int> frontSub;
    for (int k : c.kids[(size_t)h])
      if (k < h && in(c.dep(k), {"advcl", "ccomp"})) c.subtree(k, frontSub);
    for (int i : f.tokens) {
      const Token& x = c.t(i);
      if (isPunctTok(x) || c.consumed[(size_t)i]) continue;
      if (std::find(frontSub.begin(), frontSub.end(), i) != frontSub.end()) continue;
      const std::string xd = x.deprel;
      if (xd == "cc" || xd == "discourse" || xd == "vocative" || xd == "intj" ||
          (xd == "advmod" && in(x.lower, {"then", "now", "so", "just", "please", "first", "also"})) ||
          (xd == "aux" && in(c.lem(i), {"do"})) || in(x.lower, {"not", "never", "please"}) ||
          (!en && (in(x.lower, {"entonces", "luego", "pues", "ahora", "ya", "no", "bueno", "y", "pero", "sí", "nunca",
                                "jamás", "por favor", "todos", "todas"}) || xd == "fixed" || xd == "expl:pv" ||
                   ((xd == "obj" || xd == "iobj") && x.upos == "PRON" && cd_.clitic(x.lower) && i < vtok0))))
        continue;
      lead = i;
      break;
    }
    const bool subjOk = !f.hasSubject || (f.subject.isPronoun && (f.subject.pronLemma == "everyone" ||
                                                                  f.subject.pronLemma == "you")) ;
    const int vtok = cop >= 0 ? cop : (f.pred.token >= 0 ? f.pred.token : h);
    const bool lexImp = md == nlp::morph::MoodImp;
    const bool bare = tt == 0 && fget(vt, nlp::morph::VerbFormShift) != nlp::morph::VfPart &&
                      fget(vt, nlp::morph::VerbFormShift) != nlp::morph::VfGer;
    // C22: a past form is never an imperative ("... wanted more bread." continues a sentence whose subject came
    // before: a statement without its subject, Check)
    const bool pastForm = en && !lexImp && tt == nlp::morph::TensePast && f.pred.modality == Modality::None;
    if (pastForm && topLevel && !f.hasSubject && lead == vtok) c.s.doubt("fragment");
    if (en && topLevel && !pastForm && ((lead == vtok || (lead >= 0 && cop >= 0 && lead == cop)) && (lexImp || bare || !f.hasSubject) &&
               subjOk && f.pred.modality != Modality::Let && f.pred.tense != Tense::Future)) {
      bool doNot = false;
      for (int a : auxes)
        if (c.lem(a) == "do") doNot = true;
      if (!f.hasSubject || f.subject.pronLemma == "everyone" || f.subject.pronLemma == "you") {
        f.type = Kind::Imp;
        if (f.hasSubject) {
          if (f.subject.pronLemma == "everyone") { f.imperativePlural = true; f.vocatives.insert(f.vocatives.begin(), f.subject); }
          f.hasSubject = false;
        }
      }
      (void)doNot;
    } else if (!en && topLevel) {
      // Spanish (C13). The verb that opens the clause, with no subject of its own (or "todos" set off by a comma):
      //  - imperative mood (tagger or lexicon), or a 2nd-person imperative reading of a present form ("Bebe esto.",
      //    "Cierra la ventana."): imperative; "¡" or "por favor" also for other forms
      //  - "no" + 2nd-person present subjunctive: prohibition ("No tengas miedo." -> Nōlī timēre.)
      //  - 3rd-person plural present subjunctive: the ustedes imperative ("¡Todos, inclínense!")
      //  - 1st-person plural present subjunctive: hortative ("Corramos", "Entremos" -> let us)
      const uint32_t pe = esPe, nu = esNu;
      const bool everyoneSubj = f.hasSubject && f.subject.isPronoun && f.subject.pronLemma == "everyone";
      const bool noSubj = !f.hasSubject || everyoneSubj;
      const bool leads = lead == vtok || (lead >= 0 && lead == vtok0);
      const bool excl = c.s.text.find("\xC2\xA1") != std::string::npos ||
                        std::find(f.discourse.begin(), f.discourse.end(), "please") != f.discourse.end();
      const std::string vl = c.lem(vtok);
      const bool notAux = !in(vl, {"haber", "ser", "estar", "be", "llover", "nevar"}) && !es::experiencerVerb(vl) &&
                          f.pred.modality == Modality::None &&
                          f.pred.tense == Tense::Present && !f.existential;
      bool imp = false, plural = false;
      if (leads && noSubj && notAux) {
        if (lexImp && pe != 1) { imp = true; plural = nu == nlp::morph::NumPlur; }
        else if (md == nlp::morph::MoodSub && pe == 2 && f.negative) { imp = true; plural = nu == nlp::morph::NumPlur; }
        else if (md == nlp::morph::MoodSub && pe == 3 && nu == nlp::morph::NumPlur && (excl || everyoneSubj)) {
          imp = true;
          plural = true;
        } else if (md == nlp::morph::MoodSub && pe == 1 && nu == nlp::morph::NumPlur && !f.negative) {
          f.pred.modality = Modality::Let;
          f.hasSubject = true;
          f.implicitSubject = true;
          f.subject = SemNP{};
          f.subject.isPronoun = true;
          f.subject.pronLemma = "we";
          f.subject.pron.person = 1;
          f.subject.pron.number = 2;
          f.subject.number = 2;
        } else if (md != nlp::morph::MoodSub && pe != 1 && !(pe == 3 && nu == nlp::morph::NumPlur) &&
                   lex_ && es::hasImperative2(*lex_, vt.lower) && !c.s.text.empty()) {
          imp = true;
        } else if (excl && pe != 1 && pe != 3 && md != nlp::morph::MoodSub) {
          imp = true;
        }
      }
      if (imp) {
        f.type = Kind::Imp;
        if (plural) f.imperativePlural = true;
        if (everyoneSubj) {
          f.imperativePlural = true;
          f.vocatives.insert(f.vocatives.begin(), f.subject);
          f.hasSubject = false;
          f.subject = SemNP{};
        }
      }
    }
    // "Everyone bow!" with a bare verb after an "everyone" subject
    if (en && f.type == Kind::Decl && f.hasSubject && f.subject.isPronoun && f.subject.pronLemma == "everyone" &&
        bare && auxes.empty() && c.s.finalPunct == "!") {
      f.type = Kind::Imp;
      f.imperativePlural = true;
      f.vocatives.insert(f.vocatives.begin(), f.subject);
      f.hasSubject = false;
    }
  }
  // Spanish "¡Todos, inclínense!": "todos" before an imperative is the addressee (C13)
  if (!en && f.type == Kind::Imp && f.hasSubject && f.subject.isPronoun && f.subject.pronLemma == "everyone") {
    f.imperativePlural = true;
    f.vocatives.insert(f.vocatives.begin(), f.subject);
    f.hasSubject = false;
    f.subject = SemNP{};
  }
  if (f.type == Kind::Imp && f.hasSubject && f.subject.isPronoun && f.subject.pron.number == 2) f.imperativePlural = true;
  if (!f.hasSubject && f.hasPred && !en && f.type != Kind::Imp && esPe) {
    f.hasSubject = true;
    f.implicitSubject = true;
    f.subject.isPronoun = true;
    f.subject.pron.person = (uint8_t)esPe;
    f.subject.pron.number = esNu == nlp::morph::NumPlur ? 2 : 1;
    if (seImpers) f.subject.pron.number = 2;   // impersonal "se": they
    f.subject.number = f.subject.pron.number;
    // "Aquí está oscuro." / "Es tarde.": an impersonal predicate adjective has the neuter "it" as subject
    if (esPe == 3 && f.subject.pron.number == 1 && f.copula && f.predicative.empty() && f.predAdj.size() == 1 &&
        transferImpersonal(f.predAdj[0].lemma)) {
      f.subject.pronLemma = "ello";
      f.subject.pron.gender = feat::N;
    }
  }
  // Spanish deliberative question (C13): "¿Qué voy a hacer?" / "¿Qué haré?" -> quid faciam?
  if (!en && f.type == Kind::Wh && f.pred.tense == Tense::Future && f.pred.modality == Modality::None)
    f.pred.deliberative = true;
  // deliberative subjunctive (C15): "what shall I do?" -> quid faciam, "Shall we go there?" -> Eāmusne illūc?;
  // "what shall we do now?" asks about the future -> quid faciēmus
  const bool p1 = f.hasSubject && f.subject.isPronoun && f.subject.pron.person == 1;
  if (f.pred.deliberative && p1 &&
      ((f.type == Kind::Wh && (f.subject.pron.number != 2 || f.pred.modality == Modality::Should)) ||
       (f.type == Kind::Yn && en)))
    f.pred.deliberative = true;
  else f.pred.deliberative = false;

  // the interrogative NP of a wh question that is an object/oblique keeps its NP (realiser moves it first)
  (void)first;
}

// ---- units -----------------------------------------------------------------------------------------------------------
void FrameBuilder::fillSlot(Ctx& c, PhraseSlot& slot) const {
  // head of the slot range: the token whose parent is outside the range
  int head = slot.first;
  for (int i = slot.first; i <= slot.last; ++i) {
    const int p = c.par[(size_t)i];
    if (p < slot.first || p > slot.last) { head = i; break; }
  }
  // treat everything outside the range as consumed while building
  std::vector<char> keep = c.consumed;
  for (size_t i = 0; i < c.consumed.size(); ++i)
    c.consumed[i] = ((int)i < slot.first || (int)i > slot.last) ? 1 : 0;
  switch (slot.kind) {
    case SlotKind::NP: case SlotKind::Name: case SlotKind::Num:
      buildNP(c, head, slot.np);
      // C19: a possessive the parser hung outside the noun ("Where is your mother?") is never lost
      if (lang_ == SrcLang::En && slot.kind == SlotKind::NP && slot.np.possessor.empty() && !slot.np.isPronoun)
        for (int i = slot.first; i <= slot.last; ++i) {
          const std::string& w = c.t(i).lower;
          if (std::find(slot.np.tokens.begin(), slot.np.tokens.end(), i) != slot.np.tokens.end() || i == slot.np.token) continue;
          int per = 0, num = 0;
          uint8_t g = 0;
          if (w == "my") { per = 1; num = 1; }
          else if (w == "your") { per = 2; num = 0; }
          else if (w == "his") { per = 3; num = 1; g = feat::M; }
          else if (w == "her") { per = 3; num = 1; g = feat::F; }
          else if (w == "its") { per = 3; num = 1; g = feat::N; }
          else if (w == "our") { per = 1; num = 2; }
          else if (w == "their") { per = 3; num = 2; }
          if (!per || i >= slot.np.token) continue;
          SemNP p;
          p.isPronoun = true;
          p.pron.person = (uint8_t)per;
          p.pron.number = (uint8_t)num;
          p.pron.gender = g;
          p.pronLemma = w;
          p.token = i;
          p.tokens.push_back(i);
          slot.np.possessor.push_back(p);
          slot.np.tokens.push_back(i);
          break;
        }
      if (slot.kind == SlotKind::Name) { slot.np.isName = true; slot.np.head.clear();
        for (int i = slot.first; i <= slot.last; ++i) { if (!slot.np.head.empty()) slot.np.head += " "; slot.np.head += c.t(i).text; } }
      break;
    case SlotKind::Adj:
      slot.adj.lemma = c.lem(slot.last);
      slot.adj.token = slot.last;
      if (slot.last > slot.first) slot.adj.adverbs.push_back(c.t(slot.first).lower);
      break;
    case SlotKind::VP: {
      SemFrame f;
      buildClause(c, head, f);
      slot.vp.push_back(f);
      break;
    }
    case SlotKind::Wh: {
      // "which way you go": the parser hangs the clause on the noun as an acl; the noun is the verb's object
      if (c.t(head).upos == "NOUN")
        for (int k : std::vector<int>(c.kids[(size_t)head])) {
          if (!c.ok(k) || c.dep(k) != "acl" || c.t(k).upos != "VERB") continue;
          auto& hk = c.kids[(size_t)head];
          hk.erase(std::remove(hk.begin(), hk.end(), k), hk.end());
          c.par[(size_t)k] = c.par[(size_t)head];
          c.par[(size_t)head] = k;
          auto& kk = c.kids[(size_t)k];
          kk.insert(std::lower_bound(kk.begin(), kk.end(), head), head);
          c.s.tokens[(size_t)head].deprel = "obj";
          head = k;
          break;
        }
      SemFrame f;
      const bool q = c.question;
      c.question = true;   // an indirect question: wh words are interrogative
      buildClause(c, head, f);
      c.question = q;
      const std::string w = c.t(slot.first).lower;
      if (f.type != Kind::Wh && in(w, {"where", "when", "why", "how", "dónde", "cuándo", "cómo"})) {
        f.type = Kind::Wh;
        f.wh.word = w == "dónde" ? "where" : w == "cuándo" ? "when" : w == "cómo" ? "how" : w;
        f.wh.role = Role::Adverb;
        f.wh.token = slot.first;
        f.adverbs.erase(std::remove_if(f.adverbs.begin(), f.adverbs.end(),
                                       [&](const SemAdverb& a) { return a.token == slot.first; }),
                        f.adverbs.end());
      }
      if (f.type == Kind::Yn) f.type = Kind::Decl;
      slot.wh.push_back(f);
      break;
    }
  }
  c.consumed = keep;
}

void FrameBuilder::buildUnits(SemSentence& s) const {
  Ctx c(s);
  const int n = (int)s.tokens.size();
  c.par.assign((size_t)n, -1);
  c.kids.assign((size_t)n, {});
  c.consumed.assign((size_t)n, 0);
  c.taken.assign((size_t)n, 0);
  for (int i = 0; i < n; ++i) {
    const int hd = s.tokens[(size_t)i].head;
    c.par[(size_t)i] = (hd >= 1 && hd <= n && hd - 1 != i) ? hd - 1 : -1;
  }
  // break cycles defensively (a well-formed parse has none)
  for (int i = 0; i < n; ++i) {
    int x = i, steps = 0;
    while (x >= 0 && steps <= n) { x = c.par[(size_t)x]; ++steps; }
    if (steps > n) c.par[(size_t)i] = -1;
  }
  for (int i = 0; i < n; ++i)
    if (c.par[(size_t)i] >= 0) c.kids[(size_t)c.par[(size_t)i]].push_back(i);
  // final punctuation
  s.finalPunct.clear();
  for (int i = n - 1; i >= 0 && isPunctTok(s.tokens[(size_t)i]); --i) {
    const std::string& w = s.tokens[(size_t)i].text;
    if (w == "." || w == "!" || w == "?" || w == "..." || w == "\xE2\x80\xA6" || w == "?!" || w == "!?")
      s.finalPunct = w + s.finalPunct;
    else if (w == "\xC2\xBF" || w == "\xC2\xA1") continue;
    else if (!s.finalPunct.empty()) break;
  }
  if (s.finalPunct.find('?') != std::string::npos) s.question = true;
  for (int i = 0; i < n; ++i)
    if (s.tokens[(size_t)i].text == "\xC2\xBF") s.question = true;   // ¿
  c.question = s.question;
  for (int i = 0; i < n; ++i)
    if (isPunctTok(s.tokens[(size_t)i])) s.drop[(size_t)i] = Drop::Punct;

  // phrasebook pre-pass
  std::vector<PhraseMatch> phrases;
  auto transparent = [&](int i) {
    const std::string& w = s.tokens[(size_t)i].lower;
    return in(w, {"then", "now", "so", "well", "oh", "and", "but", "entonces", "pues", "y", "pero"});
  };
  if (book_.size() > 0) {
    const std::vector<curated::PhraseEntry>& book = lang_ == SrcLang::En ? cd_.phrasebook() : cd_.phrasebookEs();
    for (int i = 0; i < n;) {
      if (isPunctTok(s.tokens[(size_t)i])) { ++i; continue; }
      // C15: register "lead": one word opening the sentence before a comma ("Why, ..." -> Quid? ...)
      bool sentenceStart = true;
      for (int k = 0; k < i; ++k) sentenceStart = sentenceStart && isPunctTok(s.tokens[(size_t)k]);
      const bool leadPos = sentenceStart && i + 2 < n && s.tokens[(size_t)i + 1].text == ",";
      if (leadPos) {
        int le = -1;
        for (size_t q = 0; q < book.size() && le < 0; ++q)
          if (book[q].reg == "lead" && !(s.classical && book[q].eccl) &&
              text::lower(book[q].pattern) == s.tokens[(size_t)i].lower) le = (int)q;
        if (le >= 0) {
          PhraseMatch m;
          m.entry = le;
          m.first = m.last = i;
          m.pattern = book[(size_t)le].pattern;
          m.latin = book[(size_t)le].latin;
          m.reg = book[(size_t)le].reg;
          m.note = book[(size_t)le].note;
          m.tier = book[(size_t)le].tier;
          c.consumed[(size_t)i] = 1;
          phrases.push_back(std::move(m));
          ++i;
          continue;
        }
      }
      PhraseMatch m;
      if (!book_.match(s.tokens, i, m, s.classical)) { ++i; continue; }
      const curated::PhraseEntry& e = book[(size_t)m.entry];
      if (e.reg == "lead") { ++i; continue; }
      // C19: an imperative row ("wake up" -> ēvigilā, "hurry" -> festīnā) needs the bare verb: "She woke up." and
      // "He hurried home." are statements, not commands
      if (e.reg == "imp" && lang_ == SrcLang::En && m.first < (int)s.tokens.size()) {
        const nlp::Token& t0 = s.tokens[(size_t)m.first];
        const std::string w0 = text::lower(e.pattern.substr(0, e.pattern.find(' ')));
        const uint32_t tt = fget(t0, nlp::morph::TenseShift);
        if (t0.lower != w0 && w0.find('|') == std::string::npos && w0.find('(') == std::string::npos &&
            (tt == nlp::morph::TensePast || (t0.lower.size() > 2 && t0.lower.back() == 's' && w0.back() != 's') ||
             (t0.lower.size() > 4 && t0.lower.compare(t0.lower.size() - 3, 3, "ing") == 0))) { ++i; continue; }
      }
      bool leftOk = m.first == 0 || isPunctTok(s.tokens[(size_t)m.first - 1]) ||
                    s.tokens[(size_t)m.first - 1].upos == "CCONJ";
      if (!leftOk) {
        bool allT = true;
        for (int k = 0; k < m.first; ++k)
          if (!isPunctTok(s.tokens[(size_t)k]) && !transparent(k)) allT = false;
        leftOk = allT;
      }
      // C17: after a comma and a transparent connector (", so it will be no trouble to ..."); an adverbial phrase
      // right after a subordinator ("so that in reality I may ...")
      if (!leftOk && m.first >= 2 && transparent(m.first - 1) && isPunctTok(s.tokens[(size_t)m.first - 2]))
        leftOk = true;
      if (!leftOk && m.first >= 1 && s.tokens[(size_t)m.first - 1].upos == "SCONJ" &&
          (book[(size_t)m.entry].reg == "adv" || book[(size_t)m.entry].reg == "tail"))
        leftOk = true;
      // C17: an adverbial phrase of several words inside a clause, before a verb, adjective or adverb ("I will of
      // course help you", "She was in fact very kind"); "after all the children ..." is not one
      if (!leftOk && lang_ == SrcLang::En && (book[(size_t)m.entry].reg == "adv" || book[(size_t)m.entry].reg == "answer") &&
          m.last > m.first && m.slots.empty() &&
          ((m.last + 1 < n && in(s.tokens[(size_t)m.last + 1].upos, {"VERB", "AUX", "ADJ", "ADV"})) ||
           (book[(size_t)m.entry].reg == "adv" && s.tokens[(size_t)m.first].lower != "in" &&
            (m.last + 1 == n || isPunctTok(s.tokens[(size_t)m.last + 1])))))
        leftOk = true;   // ... or closing the clause ("We found the house at last.")
      const bool rightOk = m.last == n - 1 || isPunctTok(s.tokens[(size_t)m.last + 1]) ||
                           s.tokens[(size_t)m.last + 1].upos == "CCONJ";
      int top = m.first;
      for (int k = m.first; k <= m.last; ++k) {
        const int p = c.par[(size_t)k];
        if (p < m.first || p > m.last) { top = k; break; }
      }
      if (e.reg == "vp") {   // a verb phrase inside a clause: handled by the clause builder
        m.pattern = e.pattern;
        m.latin = e.latin;
        m.reg = e.reg;
        m.tier = e.tier;
        c.vpHits.push_back(std::move(m));
        i = c.vpHits.back().last + 1;
        continue;
      }
      const std::string topDep = s.tokens[(size_t)top].deprel;
      // C24: a "tail" phrase may also open the sentence ("On a cold afternoon the cat slept.")
      const bool prefixOk = e.reg == "adv" || e.reg == "narr" || (e.reg == "tail" && m.first == 0) ||
                            ((topDep == "advmod" || topDep == "discourse") &&
                             (e.reg == "answer" || e.reg == "excl" || e.reg == "polite"));
      // C15: register "tail": an adverbial phrase that closes a clause ("in my day", "as soon as you can")
      if (e.reg == "tail" && rightOk && !leftOk) { leftOk = true; s.doubt("phrase-order"); }
      if (leftOk && (rightOk || prefixOk)) {
        m.pattern = e.pattern;
        m.latin = e.latin;
        m.reg = e.reg;
        m.note = e.note;
        m.tier = e.tier;
        // C24: an adverbial phrase that is the predicate of "be" ("He is in trouble.", "The picture is upside down."):
        // the parser hung the subject and the copula on a word of the phrase; the copula becomes the clause's verb
        // (subject and copula a clause of their own, the phrase placed before the verb: "In perīculō est.")
        if (lang_ == SrcLang::En && m.slots.empty() && (e.reg == "tail" || e.reg == "adv") && top >= 0) {
          int cop = -1;
          for (int k : c.kids[(size_t)top])
            if ((k < m.first || k > m.last) && s.tokens[(size_t)k].deprel == "cop") cop = k;
          if (cop >= 0 && cop < m.first) {
            const int up = c.par[(size_t)top];
            std::vector<int> moved;
            for (int k : c.kids[(size_t)top])
              if (k != cop && (k < m.first || k > m.last)) moved.push_back(k);
            c.kids[(size_t)top].erase(std::remove_if(c.kids[(size_t)top].begin(), c.kids[(size_t)top].end(),
                                                     [&](int k) { return k == cop || std::find(moved.begin(), moved.end(), k) != moved.end(); }),
                                      c.kids[(size_t)top].end());
            for (int k : moved) {
              c.par[(size_t)k] = cop;
              s.tokens[(size_t)k].head = cop + 1;
              c.kids[(size_t)cop].push_back(k);
            }
            std::sort(c.kids[(size_t)cop].begin(), c.kids[(size_t)cop].end());
            c.par[(size_t)cop] = up;
            s.tokens[(size_t)cop].head = up + 1;
            s.tokens[(size_t)cop].deprel = s.tokens[(size_t)top].deprel;
            s.tokens[(size_t)cop].upos = "VERB";
            if (up >= 0) {
              auto& uk = c.kids[(size_t)up];
              std::replace(uk.begin(), uk.end(), top, cop);
            }
            c.par[(size_t)top] = cop;
            s.tokens[(size_t)top].head = cop + 1;
            s.tokens[(size_t)top].deprel = "advmod";
            c.kids[(size_t)cop].push_back(top);
            std::sort(c.kids[(size_t)cop].begin(), c.kids[(size_t)cop].end());
            m.reg = "adv";   // placed before the verb by the engine (an adverbial phrase that closes its clause)
          }
        }
        for (int k = m.first; k <= m.last; ++k) c.consumed[(size_t)k] = 1;
        phrases.push_back(std::move(m));
        i = phrases.back().last + 1;
      } else {
        ++i;
      }
    }
  }
  // slots of phrase matches are analysed as NPs / clauses
  for (PhraseMatch& m : phrases) {
    for (PhraseSlot& sl : m.slots) fillSlot(c, sl);
    for (int k = m.first; k <= m.last; ++k) c.drop(k, Drop::Phrase);
  }
  // clause groups: every unconsumed, non-punctuation token belongs to the group of its highest unconsumed ancestor
  std::vector<int> groupOf((size_t)n, -1);
  std::vector<int> heads;
  for (int i = 0; i < n; ++i) {
    if (c.consumed[(size_t)i] || isPunctTok(s.tokens[(size_t)i])) continue;
    int x = i;
    while (c.par[(size_t)x] >= 0 && !c.consumed[(size_t)c.par[(size_t)x]]) x = c.par[(size_t)x];
    groupOf[(size_t)i] = x;
    if (std::find(heads.begin(), heads.end(), x) == heads.end()) heads.push_back(x);
  }
  // a group made only of a punctuation head (rare parser output) is merged into its first real token
  struct Item { int first, last; bool phrase; size_t idx; };
  std::vector<Item> items;
  for (size_t pi = 0; pi < phrases.size(); ++pi) items.push_back(Item{phrases[pi].first, phrases[pi].last, true, pi});
  for (size_t hi = 0; hi < heads.size(); ++hi) {
    int a = n, b = -1;
    for (int i = 0; i < n; ++i)
      if (groupOf[(size_t)i] == heads[hi]) { a = std::min(a, i); b = std::max(b, i); }
    if (b >= 0) items.push_back(Item{a, b, false, hi});
  }
  std::sort(items.begin(), items.end(), [](const Item& x, const Item& y) { return x.first < y.first; });
  s.units.clear();
  for (const Item& it : items) {
    Unit u;
    u.first = it.first;
    u.last = it.last;
    if (it.phrase) {
      u.type = Unit::Phrase;
      u.phrase = phrases[it.idx];
    } else {
      u.type = Unit::Clause;
      // the clause is built with only its own group visible: other groups count as consumed
      std::vector<char> keep = c.consumed;
      for (int i = 0; i < n; ++i)
        if (groupOf[(size_t)i] != heads[it.idx] && !isPunctTok(s.tokens[(size_t)i])) c.consumed[(size_t)i] = 1;
      buildClause(c, heads[it.idx], u.frame);
      c.consumed = keep;
      freeRelativeEllipsis(u.frame);
      // C15: only the clause that carries the question mark is the question ("Even if I wanted to, how could I ...?")
      if (u.frame.type == Kind::Yn && s.question && &it != &items.back()) {
        bool qmark = false;
        for (int k = it.first; k <= it.last + 1 && k < n; ++k) qmark = qmark || s.tokens[(size_t)k].text == "?";
        if (!qmark) u.frame.type = Kind::Decl;
      }
      // a bare NP after/before a comma next to another unit is a vocative ("What is your name, child?")
      if (u.frame.type == Kind::Frag && u.frame.hasSubject && !u.frame.subject.isPronoun && items.size() > 1 &&
          u.frame.adverbs.empty())
        u.vocative = true;
      // Spanish (C13): only when a comma (or the sentence edge) sets it off: "Había una vez una niña" has none
      if (u.vocative && lang_ == SrcLang::Es) {
        bool comma = false;
        for (int k = 0; k < n; ++k)
          if ((k == it.first - 1 || k == it.last + 1) && s.tokens[(size_t)k].text == ",") comma = true;
        u.vocative = comma;
      }
    }
    s.units.push_back(std::move(u));
  }
  // C15: a lead connector set off by a comma ("However, I will ...", "Besides, you have ...") opens the next clause
  // (not before a vocative: "But, comrades, ..." keeps "Sed," apart)
  for (size_t ui = 0; ui + 1 < s.units.size(); ++ui) {
    Unit& u = s.units[ui];
    Unit& nx = s.units[ui + 1];
    if (u.type != Unit::Clause || nx.type != Unit::Clause || nx.vocative || u.frame.type != Kind::Frag ||
        u.frame.hasSubject || !u.frame.predAdj.empty() || !u.frame.interjections.empty())
      continue;
    std::string w;
    if (u.frame.connectors.size() == 1 && u.frame.adverbs.empty()) w = u.frame.connectors[0];
    else if (u.frame.adverbs.size() == 1 && u.frame.connectors.empty()) w = u.frame.adverbs[0].lemma;
    if (!in(w, {"however", "besides", "therefore", "still", "yet", "thus", "moreover", "then", "so", "and", "but",
                "for"}))
      continue;
    nx.frame.connectors.insert(nx.frame.connectors.begin(), w);
    for (int k = u.first; k <= u.last; ++k) c.drop(k, Drop::Marker);
    nx.first = u.first;
    s.units.erase(s.units.begin() + (long)ui);
    --ui;
  }
  // transparent connectors before a phrase ("Then go away."): attach to the phrase
  for (size_t ui = 0; ui + 1 < s.units.size(); ++ui) {
    Unit& u = s.units[ui];
    Unit& nx = s.units[ui + 1];
    if (u.type == Unit::Clause && nx.type == Unit::Phrase && u.frame.type == Kind::Frag && !u.frame.hasSubject &&
        u.frame.predAdj.empty() && u.frame.adverbs.size() + u.frame.connectors.size() == 1) {
      std::string w = !u.frame.connectors.empty() ? u.frame.connectors[0] : u.frame.adverbs[0].lemma;
      if (lang_ == SrcLang::Es && !canonConnector(lang_, w).empty()) w = canonConnector(lang_, w);   // entonces
      if (lang_ == SrcLang::Es && w == "ahora") w = "now";
      if (in(w, {"then", "so", "and", "but", "now"})) {
        bool punctBetween = false;
        for (int k = u.last + 1; k < nx.first; ++k)
          if (isPunctTok(s.tokens[(size_t)k])) punctBetween = true;
        if (!punctBetween) {
          nx.frame.connectors.push_back(w);
          nx.first = u.first;
          s.units.erase(s.units.begin() + (long)ui);
          --ui;
        }
      }
    }
  }
  // C22: a verbless fragment followed by ", too." ("And the other children, too."): "too" is "also" (quoque) of the
  // fragment, not a unit of its own (nimis)
  if (lang_ == SrcLang::En && s.units.size() >= 2) {
    Unit& lu = s.units.back();
    Unit& pu = s.units[s.units.size() - 2];
    const SemFrame& lf = lu.frame;
    if (lu.type == Unit::Clause && pu.type == Unit::Clause && lf.type == Kind::Frag && !lf.hasPred && !lf.hasSubject &&
        !lf.hasObject && lf.predAdj.empty() && lf.predicative.empty() && lf.obliques.empty() && lf.adverbs.size() == 1 &&
        (lf.adverbs[0].lemma == "too" || lf.adverbs[0].lemma == "also")) {
      SemAdverb a = lf.adverbs[0];
      a.lemma = "also";
      pu.frame.adverbs.push_back(a);
      pu.frame.tokens.push_back(a.token);
      pu.vocative = false;   // "And the other children, too." is no address
      pu.last = lu.last;
      s.units.pop_back();
    }
  }
  // separators and final punctuation of clause units
  for (size_t ui = 0; ui < s.units.size(); ++ui) {
    Unit& u = s.units[ui];
    const int end = ui + 1 < s.units.size() ? s.units[ui + 1].first : n;
    for (int k = u.last + 1; k < end; ++k) {
      const std::string& w = s.tokens[(size_t)k].text;
      if (w == "," || w == ";" || w == ":" || w == "-" || w == "\xE2\x80\x94" || w == "\xE2\x80\x93") {
        if (u.sepAfter.empty()) u.sepAfter = w == "\xE2\x80\x93" ? "-" : w;
      }
    }
    if (ui + 1 == s.units.size()) u.sepAfter.clear();
    if (u.type == Unit::Clause) u.frame.punct = ui + 1 == s.units.size() ? s.finalPunct : "";
    // Spanish (C13): a greeting followed by a clause of its own is a separate utterance in Latin ("Buenas noches,
    // duerme bien." -> "Bonam noctem; bene dormī."); before a vocative the comma stays ("Salvē, Māiestās Tua")
    if (lang_ == SrcLang::Es && u.type == Unit::Phrase && u.phrase.reg == "greet" && u.sepAfter == "," &&
        ui + 1 < s.units.size() && s.units[ui + 1].type == Unit::Clause && !s.units[ui + 1].vocative &&
        s.units[ui + 1].frame.hasPred)
      u.sepAfter = ";";
  }
}

// A clause the parser buried under a non-clausal dependent of a verb: "I thought it was Monday." (Monday = obl of
// thought, with its own nsubj + cop) -> ccomp; "You must be, or you wouldn't be here." (here = advmod of the first
// "be", carrying cc + nsubj + cop of the second clause) -> conj.
void FrameBuilder::repairTree(SemSentence& s) const {
  std::vector<nlp::Token>& tk = s.tokens;
  const int n = (int)tk.size();
  // C26: "The kindness of the old woman surprised everyone.": a past verb the parser hung on a noun inside the
  // subject phrase (acl, no subject, no relative word) when the sentence has no other verb is the main verb
  if (lang_ == SrcLang::En && n >= 5) {
    int r = -1, verbs = 0, v = -1;
    for (int k = 0; k < n; ++k) {
      if (tk[(size_t)k].head == 0) r = k;
      if (tk[(size_t)k].upos == "VERB" || tk[(size_t)k].upos == "AUX") { ++verbs; v = k; }
    }
    if (r >= 0 && verbs == 1 && v > r && in(tk[(size_t)r].upos, {"NOUN", "PROPN"}) && tk[(size_t)v].deprel == "acl" &&
        nlp::morph::get(tk[(size_t)v].feats, nlp::morph::TenseShift) == nlp::morph::TensePast &&
        nlp::morph::get(tk[(size_t)v].feats, nlp::morph::VerbFormShift) != nlp::morph::VfPart) {
      bool own = false;
      for (int k = 0; k < n; ++k)
        if (tk[(size_t)k].head == v + 1 && (tk[(size_t)k].deprel == "nsubj" ||
                                            in(tk[(size_t)k].lower, {"who", "which", "that", "whom", "whose"})))
          own = true;
      int up = tk[(size_t)v].head - 1;   // the noun it hangs on must lie inside the root's phrase
      bool inside = false;
      for (int guard = 0; up >= 0 && guard < n; ++guard) {
        if (up == r) { inside = true; break; }
        up = tk[(size_t)up].head - 1;
      }
      if (!own && inside) {
        tk[(size_t)v].head = 0;
        tk[(size_t)v].deprel = "root";
        tk[(size_t)r].head = v + 1;
        tk[(size_t)r].deprel = "nsubj";
        for (int k = 0; k < n; ++k)
          if (k != v && tk[(size_t)k].head == r + 1 && tk[(size_t)k].deprel == "punct" && k > v) tk[(size_t)k].head = v + 1;
      }
    }
  }
  // C26: verbless fragments (a cue that is a noun phrase).
  bool anyVerb = false;
  for (const nlp::Token& t : tk) anyVerb = anyVerb || t.upos == "VERB" || t.upos == "AUX";
  if (lang_ == SrcLang::En && !anyVerb && n >= 6) {
    // (a) a list "the brains, the heart and the courage.": every noun phrase after a comma or and / or is a conjunct
    // of the first (the parser made them punctuation, genitives or appositions)
    std::vector<int> heads;   // the head noun of each segment
    std::vector<int> cc;      // the and / or tokens
    int segStart = 0;
    bool ok = true, sawAnd = false;
    for (int k = 0; k <= n && ok; ++k) {
      const bool end = k == n || tk[(size_t)k].text == "," || in(tk[(size_t)k].lower, {"and", "or"}) ||
                       (tk[(size_t)k].upos == "PUNCT" && k == n - 1);
      if (!end) continue;
      int h = -1;
      for (int j = segStart; j < k; ++j) {
        const std::string& u = tk[(size_t)j].upos;
        if (in(u, {"NOUN", "PROPN"})) h = j;
        else if (!in(u, {"DET", "ADJ", "NUM", "PRON"})) ok = false;
      }
      if (k < n && in(tk[(size_t)k].lower, {"and", "or"})) { cc.push_back(k); sawAnd = true; }
      if (h < 0 && k > segStart) ok = false;
      if (h >= 0) heads.push_back(h);
      segStart = k + 1;
      if (k < n && tk[(size_t)k].upos == "PUNCT" && k == n - 1) break;
    }
    if (ok && sawAnd && heads.size() >= 3 && cc.size() == 1 && cc[0] > heads[heads.size() - 2]) {
      const int r = heads[0];
      for (int k = 0; k < n; ++k) {
        nlp::Token& t = tk[(size_t)k];
        if (k == r) { t.head = 0; t.deprel = "root"; continue; }
        int hn = -1;   // the head of k's segment
        for (int h : heads) if (h >= k && (hn < 0 || h < hn)) hn = h;
        bool isHead = false;
        for (int h : heads) isHead = isHead || h == k;
        if (isHead) { t.head = r + 1; t.deprel = "conj"; continue; }
        if (k == cc[0]) { t.head = heads.back() + 1; t.deprel = "cc"; continue; }
        if (t.upos == "PUNCT") { t.head = (k == n - 1 ? r : (hn >= 0 ? hn : r)) + 1; t.deprel = "punct"; continue; }
        if (hn >= 0) { t.head = hn + 1; t.deprel = t.upos == "DET" ? "det" : t.upos == "ADJ" ? "amod" : t.upos == "NUM" ? "nummod" : "nmod"; }
      }
    }
  }
  // (a2) a verbless cue the parser rooted on its final punctuation ("and the girls too."): the first noun is the root,
  // the words before it its determiners / adjectives, a leading and / or / but its conjunction, an adverb its modifier
  if (lang_ == SrcLang::En && !anyVerb && n >= 3) {
    int root = -1, noun = -1;
    for (int k = 0; k < n; ++k) {
      if (tk[(size_t)k].head == 0 && root < 0) root = k;
      if (noun < 0 && in(tk[(size_t)k].upos, {"NOUN", "PROPN"})) noun = k;
    }
    bool simple = noun >= 0;
    for (int k = 0; k < n && simple; ++k)
      simple = in(tk[(size_t)k].upos, {"DET", "ADJ", "NUM", "NOUN", "PROPN", "PRON", "ADV", "CCONJ", "PUNCT"}) &&
               (k <= noun || !in(tk[(size_t)k].upos, {"NOUN", "PROPN", "DET", "ADJ"}));
    if (root >= 0 && tk[(size_t)root].upos == "PUNCT" && simple) {
      for (int k = 0; k < n; ++k) {
        nlp::Token& t = tk[(size_t)k];
        if (k == noun) { t.head = 0; t.deprel = "root"; continue; }
        t.head = noun + 1;
        t.deprel = t.upos == "DET" ? "det" : t.upos == "ADJ" ? "amod" : t.upos == "NUM" ? "nummod" : t.upos == "PRON" ? "nmod"
                 : t.upos == "ADV" ? "advmod" : t.upos == "CCONJ" ? "cc" : "punct";
      }
    }
  }
  // (b) "Not my sister!", "Not the old dog!": "not" before a noun phrase without a verb is the negation of the
  // phrase (nōn soror mea), not a verb
  if (lang_ == SrcLang::En && n >= 3 && tk[0].lower == "not" && in(tk[1].upos, {"DET", "PRON", "NOUN", "PROPN", "ADJ", "NUM"})) {
    bool verb = false;
    for (int k = 1; k < n; ++k) verb = verb || (tk[(size_t)k].upos == "VERB" || tk[(size_t)k].upos == "AUX");
    int h = -1;
    for (int k = 1; k < n; ++k)
      if (in(tk[(size_t)k].upos, {"NOUN", "PROPN"})) { h = k; break; }
    if (!verb && h > 0) {
      for (int k = 0; k < n; ++k) {
        nlp::Token& t = tk[(size_t)k];
        if (k == h) { t.head = 0; t.deprel = "root"; }
        else if (t.head == 1 || k == 0) t.head = h + 1;
      }
      tk[0].upos = "PART";
      tk[0].deprel = "advmod";
      tk[0].feats = 0;
      for (int k = 1; k < h; ++k)
        tk[(size_t)k].deprel = tk[(size_t)k].upos == "DET" ? "det" : tk[(size_t)k].upos == "PRON" ? "nmod"
                             : tk[(size_t)k].upos == "NUM" ? "nummod" : "amod";
    }
  }
  // (c) "A crown made of gold.", "A house built of stone.": a noun phrase with an indefinite article and a past
  // participle + "of" / "from" / "with" and no other verb is a noun phrase with a participle, not a sentence
  if (lang_ == SrcLang::En && n >= 5) {
    int v = -1, verbs = 0;
    for (int k = 0; k < n; ++k)
      if (tk[(size_t)k].upos == "VERB" || tk[(size_t)k].upos == "AUX") { ++verbs; v = k; }
    int last = n - 1;
    while (last > 0 && tk[(size_t)last].upos == "PUNCT") --last;
    if (verbs == 1 && v >= 2 && tk[(size_t)v].deprel == "root" && in(tk[(size_t)v].lower,
            {"made", "built", "covered", "filled", "painted", "carved", "woven", "sewn", "dressed", "tied", "full"}) &&
        in(tk[0].lower, {"a", "an", "two", "three", "some", "many"}) && v + 1 <= last &&
        in(tk[(size_t)v + 1].lower, {"of", "from", "with", "in"}) && tk[(size_t)n - 1].text != "?") {
      int s0 = -1;
      for (int k = 0; k < v; ++k)
        if (tk[(size_t)k].head == v + 1 && tk[(size_t)k].deprel == "nsubj") s0 = k;
      if (s0 >= 0) {
        tk[(size_t)s0].head = 0;
        tk[(size_t)s0].deprel = "root";
        tk[(size_t)v].head = s0 + 1;
        tk[(size_t)v].deprel = "acl";
        tk[(size_t)v].feats = nlp::morph::fromString("Tense=Past|VerbForm=Part");
        for (int k = 0; k < n; ++k)
          if (k != v && k != s0 && tk[(size_t)k].head == v + 1 && tk[(size_t)k].deprel == "punct") tk[(size_t)k].head = s0 + 1;
      }
    }
  }
  // C26: "he never lets anyone come into his presence", "makes the children laugh": a bare verb the parser hung on the
  // object of let / make / help / see / hear / watch (acl) is that verb's complement (xcomp): sinō + accusative +
  // infinitive, not a relative clause of the object
  if (lang_ == SrcLang::En)
    for (int v = 0; v < n; ++v) {
      if (tk[(size_t)v].upos != "VERB" || !in(tk[(size_t)v].lower, {"let", "lets", "letting"})) continue;
      for (int o = v + 1; o < n; ++o) {
        if (tk[(size_t)o].head != v + 1 || tk[(size_t)o].deprel != "obj") continue;
        for (int k = o + 1; k < n; ++k) {
          if (tk[(size_t)k].head != o + 1 || tk[(size_t)k].deprel != "acl" || tk[(size_t)k].upos != "VERB") continue;
          const uint32_t vf = nlp::morph::get(tk[(size_t)k].feats, nlp::morph::VerbFormShift);
          if (vf == nlp::morph::VfPart || vf == nlp::morph::VfGer || vf == nlp::morph::VfFin) continue;
          bool to = false;
          for (int j = 0; j < n; ++j) to = to || (tk[(size_t)j].head == k + 1 && tk[(size_t)j].lower == "to");
          if (to) continue;
          tk[(size_t)k].head = v + 1;
          tk[(size_t)k].deprel = "xcomp";
        }
      }
    }
  // C26: "Is the box big or small?", "Are the apples ripe?": a yes / no question opening with a form of "be" and a
  // predicate adjective (or noun) at the root: the noun phrase between them is the subject (the parser made it an
  // oblique without a preposition)
  if (lang_ == SrcLang::En && n >= 4 && in(tk[0].lower, {"is", "are", "was", "were"}) && tk[0].deprel == "cop") {
    const int r = tk[0].head - 1;
    bool subj = false;
    for (int k = 1; k < n; ++k) subj = subj || (tk[(size_t)k].head == r + 1 && tk[(size_t)k].deprel == "nsubj");
    if (r > 1 && r < n && in(tk[(size_t)r].upos, {"ADJ", "NOUN"}) && !subj)
      for (int k = 1; k < r; ++k) {
        if (tk[(size_t)k].head != r + 1 || !in(tk[(size_t)k].deprel, {"obl", "obj", "nmod"}) ||
            !in(tk[(size_t)k].upos, {"NOUN", "PROPN"}))
          continue;
        bool cs = false;
        for (int j = 0; j < n; ++j) cs = cs || (tk[(size_t)j].head == k + 1 && tk[(size_t)j].deprel == "case");
        if (!cs) tk[(size_t)k].deprel = "nsubj";
        break;
      }
  }
  // C26: "without" + an -ing verb the parser hung on the next noun as an adjective ("walked without finding water"):
  // the verb is a clause of the main verb with "without" as its marker and the noun as its object
  if (lang_ == SrcLang::En)
    for (int k = 0; k + 2 < n; ++k) {
      if (tk[(size_t)k].lower != "without") continue;
      nlp::Token& v = tk[(size_t)k + 1];
      if (v.upos != "VERB" || v.lower.size() < 5 || v.lower.compare(v.lower.size() - 3, 3, "ing") != 0) continue;
      if (v.deprel != "amod" && v.deprel != "compound") continue;
      const int nn = v.head - 1;
      if (nn <= k + 1 || nn >= n || !in(tk[(size_t)nn].upos, {"NOUN", "PROPN"})) continue;
      const int mh = tk[(size_t)nn].head;
      if (mh <= 0) continue;
      v.head = mh;
      v.deprel = "advcl";
      tk[(size_t)nn].head = k + 2;
      tk[(size_t)nn].deprel = "obj";
      tk[(size_t)k].head = k + 2;
      tk[(size_t)k].deprel = "mark";
      tk[(size_t)k].upos = "SCONJ";
    }
  // C26: "Whose house did you see?", "Which book will you read?": an interrogative noun phrase before an auxiliary
  // and a second subject is the object of the verb (the parser made it a second subject)
  if (lang_ == SrcLang::En && n >= 5 && in(tk[0].lower, {"whose", "which", "what"})) {
    int q = 1;
    while (q < n && tk[(size_t)q].upos == "ADJ") ++q;
    const int nh = q;
    if (nh < n - 3 && in(tk[(size_t)nh].upos, {"NOUN", "PROPN"}) && tk[(size_t)nh].deprel == "nsubj" &&
        tk[(size_t)nh + 1].upos == "AUX" && in(tk[(size_t)nh + 1].lower, {"do", "does", "did", "will", "can", "could",
                                                                        "should", "would", "shall", "must", "may"})) {
      const int v = tk[(size_t)nh].head - 1;
      int other = -1;
      for (int j = nh + 2; j < n; ++j)
        if (tk[(size_t)j].head == v + 1 && tk[(size_t)j].deprel == "nsubj") other = j;
      int last = n - 1;
      while (last > 0 && tk[(size_t)last].upos == "PUNCT") --last;
      if (v > nh && other > nh && tk[(size_t)v].upos == "VERB" && tk[(size_t)last].upos != "ADP") {
        tk[(size_t)nh].deprel = "obj";
        tk[0].head = nh + 1;
        tk[0].deprel = "det";
        tk[0].upos = "DET";
      }
    }
  }
  // C26: "Whose book is this?", "Whose shoes are those?": "whose" + noun phrase + be + a demonstrative / pronoun
  // at the end is a copula question: the noun phrase is the predicate (root), the demonstrative its subject (the
  // parser hung the copula on the pronoun and the clause became a fragment)
  if (lang_ == SrcLang::En && n >= 5 && tk[0].lower == "whose") {
    int q = 1;
    while (q < n && tk[(size_t)q].upos == "ADJ") ++q;
    const int nh = q;
    int last = n - 1;
    while (last > 0 && tk[(size_t)last].upos == "PUNCT") --last;
    if (nh < n && in(tk[(size_t)nh].upos, {"NOUN", "PROPN"}) && nh + 2 == last &&
        in(tk[(size_t)nh + 1].lower, {"is", "are", "was", "were"}) &&
        in(tk[(size_t)last].lower, {"this", "that", "these", "those", "it", "they"}) && tk[(size_t)n - 1].text == "?") {
      tk[0].head = nh + 1;
      tk[0].deprel = "det";
      tk[0].upos = "DET";
      for (int k = 1; k < nh; ++k) { tk[(size_t)k].head = nh + 1; tk[(size_t)k].deprel = "amod"; }
      tk[(size_t)nh].head = 0;
      tk[(size_t)nh].deprel = "root";
      tk[(size_t)nh + 1].head = nh + 1;
      tk[(size_t)nh + 1].deprel = "cop";
      tk[(size_t)nh + 1].upos = "AUX";
      tk[(size_t)last].head = nh + 1;
      tk[(size_t)last].deprel = "nsubj";
      tk[(size_t)last].upos = "PRON";
      for (int k = last + 1; k < n; ++k) { tk[(size_t)k].head = nh + 1; tk[(size_t)k].deprel = "punct"; }
    }
  }
  // C24: "After this he will ...": "after" + "this" before the subject is a time phrase (post hoc), not a
  // subordinator with a second subject
  if (lang_ == SrcLang::En && n >= 4 && in(tk[0].lower, {"after", "before"}) && in(tk[1].lower, {"this", "that"}) &&
      tk[1].deprel == "nsubj") {
    const int h = tk[1].head - 1;
    bool other = false;
    for (int j = 2; j < n; ++j) other = other || (tk[(size_t)j].head == h + 1 && tk[(size_t)j].deprel == "nsubj");
    if (other) {
      tk[1].deprel = "obl";
      tk[1].upos = "PRON";
      tk[0].head = 2;
      tk[0].deprel = "case";
      tk[0].upos = "ADP";
    }
  }
  // C24: "What could a fox possibly want?", "What are you afraid of?", "What would the old king say?": in a question
  // "what" + auxiliaries + a noun phrase, the noun phrase is the subject (the parser sometimes made "what" a second
  // subject or the noun an oblique); "what" is the object of the preposition stranded at the end, else the object
  if (lang_ == SrcLang::En && n >= 4 && tk[0].lower == "what" && tk[0].upos == "PRON" && tk[0].head > 0) {
    bool qm = false;
    for (const nlp::Token& t : tk) qm = qm || t.text == "?";
    int h = tk[0].head - 1;
    int j = 1;
    while (j < n && (tk[(size_t)j].upos == "AUX" || in(tk[(size_t)j].lower, {"not", "n't"}))) ++j;
    int sh = -1, s0 = j;
    if (j > 1 && j < n) {
      if (tk[(size_t)j].upos == "PRON" && in(tk[(size_t)j].lower, {"you", "he", "she", "it", "we", "they", "i"})) sh = j;
      else {
        int q = j;
        if (q < n && tk[(size_t)q].upos == "DET") ++q;
        while (q < n && tk[(size_t)q].upos == "ADJ") ++q;
        if (q < n && in(tk[(size_t)q].upos, {"NOUN", "PROPN"})) sh = q;
      }
    }
    int last = n - 1;
    while (last > 0 && tk[(size_t)last].upos == "PUNCT") --last;
    if (qm && sh > 0 && h >= 0 && h != sh && h > sh && (tk[0].deprel == "nsubj" || tk[0].deprel == "obl")) {
      for (int k = 0; k < n; ++k)   // dependents of the noun phrase words hung elsewhere stay; the head is the subject
        if (k > sh && tk[(size_t)k].head == sh + 1 && tk[(size_t)k].deprel == "advmod") tk[(size_t)k].head = h + 1;
      tk[(size_t)sh].head = h + 1;
      tk[(size_t)sh].deprel = "nsubj";
      for (int k = s0; k < sh; ++k) {
        tk[(size_t)k].head = sh + 1;
        tk[(size_t)k].deprel = tk[(size_t)k].upos == "ADJ" ? "amod" : "det";
      }
      for (int k = 1; k < s0; ++k)
        if (tk[(size_t)k].head != h + 1) tk[(size_t)k].head = h + 1;
      for (int k = 0; k < n; ++k)
        if (k != h && tk[(size_t)k].head == h + 1 && tk[(size_t)k].deprel == "nsubj" && k != sh && k != 0)
          tk[(size_t)k].deprel = "obl";
      if (tk[(size_t)last].upos == "ADP" && last > sh) {
        tk[0].deprel = "obl";
        tk[(size_t)last].head = 1;
        tk[(size_t)last].deprel = "case";
        for (int k = 1; k < n; ++k)
          if (tk[(size_t)k].head == 1 && in(tk[(size_t)k].deprel, {"cop", "aux"})) tk[(size_t)k].head = h + 1;
      } else if (tk[(size_t)h].upos == "VERB") {
        tk[0].deprel = "obj";
      }
    }
  }
  // C24: "the town where my uncle works": "where" after a noun of the clause, followed by a subject and a verb,
  // opens a relative clause of that noun ("ubi", "in quō"), not a time clause or a question; the noun belongs to the
  // verb before it
  if (lang_ == SrcLang::En)
    for (int k = 2; k + 2 < n; ++k) {
      if (tk[(size_t)k].lower != "where" || !in(tk[(size_t)k - 1].upos, {"NOUN", "PROPN"})) continue;
      const int noun = k - 1;
      int sh = -1, q = k + 1;
      if (tk[(size_t)q].upos == "PRON" && in(tk[(size_t)q].lower, {"i", "you", "he", "she", "it", "we", "they"})) sh = q;
      else {
        if (tk[(size_t)q].upos == "DET" || in(tk[(size_t)q].lower, {"my", "your", "his", "her", "our", "their", "its"})) ++q;
        while (q < n && tk[(size_t)q].upos == "ADJ") ++q;
        if (q < n && in(tk[(size_t)q].upos, {"NOUN", "PROPN"})) sh = q;
      }
      if (sh < 0) continue;
      int v = sh + 1;
      while (v < n && (tk[(size_t)v].upos == "AUX" || in(tk[(size_t)v].lower, {"not", "n't", "never", "always"}))) ++v;
      // "where my grandmother lives": a present form read as a plural noun (lives) is the verb
      if (v < n && tk[(size_t)v].upos == "NOUN" && lex_ && tk[(size_t)v].lower.size() > 2 && tk[(size_t)v].lower.back() == 's') {
        std::vector<lex::Analysis> an;
        lex_->lookup(text::en_key(tk[(size_t)v].lower), an);
        for (const lex::Analysis& a : an) {
          const lex::Lemma l = lex_->lemma(a.lemma);
          if (l.pos == feat::Verb) {
            tk[(size_t)v].upos = "VERB";
            tk[(size_t)v].lemma = text::lower(std::string(l.head));
            tk[(size_t)v].feats = nlp::morph::fromString("Mood=Ind|Number=Sing|Person=3|Tense=Pres|VerbForm=Fin");
            break;
          }
        }
      }
      if (v >= n || tk[(size_t)v].upos != "VERB") continue;
      int mv = -1;   // the verb of the clause the noun belongs to
      for (int j = noun - 1; j >= 0 && mv < 0; --j)
        if (tk[(size_t)j].upos == "VERB") mv = j;
      if (mv < 0) continue;
      auto inRel = [&](int x) { return x == k || x == sh || x == v || (x > k && x < v); };
      // dependents of the relative words that lie outside the relative clause (after the verb's phrase) stay as they are
      if (inRel(tk[(size_t)noun].head - 1) || tk[(size_t)noun].head == 0) { tk[(size_t)noun].head = mv + 1; }
      if (!in(tk[(size_t)noun].deprel, {"obl", "obj", "nmod"})) tk[(size_t)noun].deprel = "obl";
      if (tk[(size_t)mv].head - 1 == v) { tk[(size_t)mv].head = tk[(size_t)v].head; tk[(size_t)mv].deprel = tk[(size_t)v].deprel; }
      for (int x = 0; x < n; ++x)   // what hung on the relative verb before the noun (the main clause) goes to mv
        if (x < noun && x != mv && tk[(size_t)x].head == v + 1) tk[(size_t)x].head = mv + 1;
      if (tk[(size_t)mv].head == mv + 1 || tk[(size_t)mv].head == v + 1) { tk[(size_t)mv].head = 0; tk[(size_t)mv].deprel = "root"; }
      tk[(size_t)v].head = noun + 1;
      tk[(size_t)v].deprel = "acl";
      tk[(size_t)k].head = v + 1;
      tk[(size_t)k].deprel = "advmod";
      tk[(size_t)k].upos = "ADV";
      tk[(size_t)sh].head = v + 1;
      tk[(size_t)sh].deprel = "nsubj";
      for (int x = k + 1; x < sh; ++x) { tk[(size_t)x].head = sh + 1; tk[(size_t)x].deprel = tk[(size_t)x].upos == "ADJ" ? "amod" : "det"; }
      for (int x = sh + 1; x < v; ++x) { tk[(size_t)x].head = v + 1; tk[(size_t)x].deprel = tk[(size_t)x].upos == "AUX" ? "aux" : "advmod"; }
      for (int x = 0; x < n; ++x)   // the noun's own words
        if (x < noun && tk[(size_t)x].head == v + 1 && in(tk[(size_t)x].upos, {"DET", "ADJ"})) tk[(size_t)x].head = noun + 1;
      break;
    }
  // C24: "Cats say it and dogs say it.": a clause complement that opens with its own "and" / "but" / "or" is the
  // second clause of a coordination (conj), not the content of the first verb
  if (lang_ == SrcLang::En)
    for (int k = 0; k < n; ++k) {
      if (tk[(size_t)k].deprel != "ccomp" || tk[(size_t)k].upos != "VERB") continue;
      int lm = k;   // leftmost word of the clause's subtree
      for (int j = 0; j < k; ++j) {
        int x = j, guard = 0;
        while (x >= 0 && x != k && guard++ < n) x = tk[(size_t)x].head - 1;
        if (x == k) { lm = j; break; }
      }
      if (lm < k && tk[(size_t)lm].deprel == "cc" && in(tk[(size_t)lm].lower, {"and", "but", "or"}) &&
          tk[(size_t)k].head - 1 < lm)
        tk[(size_t)k].deprel = "conj";
    }
  // C24: "fell down the steps", "rolled down the hill": "down" read as the verb's particle while the noun after it
  // hangs on the verb without a preposition: "down" is that noun's preposition (dē scālīs)
  if (lang_ == SrcLang::En)
    for (int k = 0; k + 1 < n; ++k) {
      nlp::Token& t = tk[(size_t)k];
      if (t.lower != "down" || (t.upos != "ADP" && t.upos != "ADV") ||
          (t.deprel != "compound" && t.deprel != "compound:prt" && t.deprel != "advmod"))
        continue;
      const int h = t.head - 1;
      if (h < 0 || h >= k) continue;
      int g = -1;
      for (int j = k + 1; j < n; ++j) {
        const std::string& u = tk[(size_t)j].upos;
        if (u == "DET" || u == "ADJ" || u == "NUM" || (u == "PRON" && tk[(size_t)j].deprel == "nmod:poss")) continue;
        // an object only after a verb of motion ("Don't fall down the steps!"); "put down the book" keeps it
        const bool motion = in(text::lower(tk[(size_t)h].lemma.empty() ? tk[(size_t)h].lower : tk[(size_t)h].lemma),
                               {"fall", "tumble", "roll", "slide", "slip", "run", "walk", "go", "come", "climb", "hurry",
                                "jump", "float", "fly", "ride", "sail", "swim", "rush", "crawl", "hop", "skip"});
        if ((u == "NOUN" || u == "PROPN") && tk[(size_t)j].head == h + 1 &&
            (tk[(size_t)j].deprel == "obl" || (tk[(size_t)j].deprel == "obj" && motion)))
          g = j;
        break;
      }
      if (g < 0) continue;
      tk[(size_t)g].deprel = "obl";
      bool hasCase = false;
      for (int j = 0; j < n; ++j)
        if (tk[(size_t)j].head == g + 1 && tk[(size_t)j].deprel == "case") hasCase = true;
      if (hasCase) continue;
      t.head = g + 1;
      t.deprel = "case";
      t.upos = "ADP";
    }
  // C19: "Where were you?" (where read as the subject of "you") and "Where have you been?" ("been" hung on "where"
  // as a clause): the wh adverb is the place predicate of "be", the pronoun its subject, as in "Where is the ball?"
  if (lang_ == SrcLang::En && n >= 3) {
    const int w = 0;
    const std::string wl = tk[0].lower;
    if (wl == "where" && (tk[0].upos == "ADV" || tk[0].upos == "PRON")) {
      int be = -1, subj = -1, relStart = n;
      std::vector<int> aux;
      for (int j = 1; j < n; ++j) {
        const std::string& l = tk[(size_t)j].lower;
        if (in(l, {"is", "are", "was", "were", "am", "been", "be", "'s", "'re", "'m"})) { if (be < 0) be = j; continue; }
        if (in(l, {"have", "has", "had", "will", "would", "'ve", "'d", "'ll", "shall", "can", "could"}) && be < 0) { aux.push_back(j); continue; }
        if (tk[(size_t)j].upos == "PRON" || tk[(size_t)j].upos == "PROPN" || tk[(size_t)j].upos == "NOUN" ||
            tk[(size_t)j].upos == "DET") { if (subj < 0) subj = j; continue; }
        if (subj >= 0 && in(l, {"who", "which", "that"})) { relStart = j; break; }   // "Where is the boy who ...?"
        if (tk[(size_t)j].upos == "ADJ") { subj = -2; break; }   // "How old are you?" stays as it is
        if (tk[(size_t)j].upos == "PUNCT" && j == n - 1) continue;
        if (tk[(size_t)j].upos == "VERB" || tk[(size_t)j].upos == "ADV" || tk[(size_t)j].upos == "ADP") { subj = -2; break; }
      }
      // only the plain shape: wh + (aux) + be-form + subject words ... or wh + aux + subject + been
      // the subject phrase: the subject word and its determiners / possessives up to the end
      int sh = -1;
      if (subj >= 0)
        for (int j = subj; j < relStart; ++j)
          if (tk[(size_t)j].upos == "PRON" || tk[(size_t)j].upos == "NOUN" || tk[(size_t)j].upos == "PROPN") sh = j;
      const bool broken = be >= 0 && subj >= 0 && sh >= 0 &&
                          (tk[(size_t)w].deprel != "root" || tk[(size_t)be].deprel == "advcl" ||
                           tk[(size_t)be].deprel == "ccomp" || tk[(size_t)w].deprel == "nsubj" ||
                           tk[(size_t)sh].head != w + 1);
      if (broken) {
        {
          for (int j = 0; j < n; ++j) {
            nlp::Token& t = tk[(size_t)j];
            if (j == w) { t.head = 0; t.deprel = "root"; }
            else if (j == be) { t.head = w + 1; t.deprel = "cop"; }
            else if (std::find(aux.begin(), aux.end(), j) != aux.end()) { t.head = w + 1; t.deprel = "aux"; }
            else if (j == sh) { t.head = w + 1; t.deprel = "nsubj"; }
            else if (j >= subj && j < sh) { t.head = sh + 1; t.deprel = t.upos == "DET" ? "det" : "nmod:poss"; }
            else if (t.upos == "PUNCT" && j == n - 1) { t.head = w + 1; t.deprel = "punct"; }
            else if (j >= relStart) {
              if (t.head - 1 < relStart || t.head == 0) { t.head = sh + 1; t.deprel = "acl"; }   // the relative clause
            }
            else if (t.upos == "PUNCT") { t.head = w + 1; t.deprel = "punct"; }
            else if (j > sh) { t.head = w + 1; t.deprel = "dep"; }
          }
          tk[(size_t)w].upos = "ADV";
        }
      }
    }
  }
  // C19: "As soon as the bell rang, the boys ran out.": the clause after "as soon as" is a time clause of the main verb;
  // "soon" carries the marker, both "as" are fixed parts of it
  if (lang_ == SrcLang::En)
    for (int i = 0; i + 3 < n; ++i) {
      if (tk[(size_t)i].lower != "as" || tk[(size_t)i + 1].lower != "soon" || tk[(size_t)i + 2].lower != "as") continue;
      int v = -1, end = n;
      for (int j = i + 3; j < n; ++j) {
        if (tk[(size_t)j].text == "," || tk[(size_t)j].text == ";") { end = j; break; }
        if (v < 0 && tk[(size_t)j].upos == "VERB") v = j;
      }
      if (v < 0) continue;
      int main = -1;
      for (int j = 0; j < n; ++j)
        if ((j < i || j >= end) && tk[(size_t)j].upos == "VERB" && (tk[(size_t)j].head == 0 || main < 0) &&
            (j < i || j > end)) { main = j; if (tk[(size_t)j].head == 0) break; }
      if (main < 0) continue;
      for (int j = 0; j < n; ++j)   // the main clause's words that hung on the time clause go back to the main verb
        if ((j < i || j > end) && j != main && tk[(size_t)j].head - 1 >= i && tk[(size_t)j].head - 1 <= end)
          tk[(size_t)j].head = main + 1;
      tk[(size_t)main].head = 0;
      tk[(size_t)main].deprel = "root";
      tk[(size_t)v].head = main + 1;
      tk[(size_t)v].deprel = "advcl";
      tk[(size_t)i + 1].head = v + 1;
      tk[(size_t)i + 1].deprel = "mark";
      tk[(size_t)i + 1].upos = "SCONJ";
      tk[(size_t)i].head = i + 2;
      tk[(size_t)i].deprel = "fixed";
      tk[(size_t)i + 2].head = i + 2;
      tk[(size_t)i + 2].deprel = "fixed";
      for (int j = i + 3; j < end; ++j)
        if (j != v && (tk[(size_t)j].head - 1 < i || tk[(size_t)j].head - 1 > end || tk[(size_t)j].head == 0)) tk[(size_t)j].head = v + 1;
      if (end < n) { tk[(size_t)end].head = v + 1; tk[(size_t)end].deprel = "punct"; }
      tk[(size_t)i].upos = "SCONJ";
      tk[(size_t)i + 2].upos = "SCONJ";
      if (s.drop.size() == tk.size()) { s.drop[(size_t)i] = Drop::Marker; s.drop[(size_t)i + 2] = Drop::Marker; }
      break;
    }
  // C19: "Before the sun set, we were home.": a sentence that opens with a subordinator has its main clause after the
  // comma; when the parser made the subordinate verb the root, the main clause's head becomes the root again
  if (lang_ == SrcLang::En && n >= 5 &&
      in(tk[0].lower, {"when", "before", "after", "while", "if", "because", "until", "till", "once", "since", "although"})) {
    int comma = -1;
    for (int j = 2; j < n; ++j)
      if (tk[(size_t)j].text == ",") { comma = j; break; }
    int root = -1;
    for (int j = 0; j < n; ++j)
      if (tk[(size_t)j].head == 0) { root = j; break; }
    if (comma > 0 && root > 0 && root < comma) {
      int mh = -1;   // the main clause head: a word after the comma hung directly on the root
      for (int j = comma + 1; j < n; ++j)
        if (tk[(size_t)j].head == root + 1 && !in(tk[(size_t)j].deprel, {"punct", "cc", "mark", "det", "case"}) &&
            tk[(size_t)j].upos != "PUNCT" && tk[(size_t)j].upos != "DET" && tk[(size_t)j].upos != "ADP") {
          mh = j;
          break;
        }
      if (mh > 0) {
        // the main clause: everything after the comma hung on the old root, and the subject / copula of the predicate
        for (int j = comma + 1; j < n; ++j)
          if (j != mh && tk[(size_t)j].head == root + 1) tk[(size_t)j].head = mh + 1;
        for (int j = comma + 1; j < n; ++j)
          if (j != mh && (tk[(size_t)j].deprel == "nsubj" || tk[(size_t)j].deprel == "cop" || tk[(size_t)j].deprel == "aux") &&
              tk[(size_t)j].head - 1 > comma && tk[(size_t)j].head - 1 != mh && tk[(size_t)tk[(size_t)j].head - 1].upos == "AUX")
            tk[(size_t)j].head = mh + 1;
        tk[(size_t)mh].head = 0;
        tk[(size_t)mh].deprel = "root";
        tk[(size_t)root].head = mh + 1;
        tk[(size_t)root].deprel = "advcl";
        tk[0].head = root + 1;
        tk[0].deprel = "mark";
        tk[0].upos = "SCONJ";
        tk[(size_t)comma].head = root + 1;
      }
    }
  }
  // C19: "The fisherman's wife wanted ...": a possessor ('s) the parser hung on the verb belongs to the noun after it
  if (lang_ == SrcLang::En)
    for (int q = 1; q + 1 < n; ++q) {
      if (tk[(size_t)q].lower != "'s" || tk[(size_t)q].deprel != "case") continue;
      const int o = tk[(size_t)q].head - 1;
      if (o != q - 1 && o < 0) continue;
      int pd = -1;
      for (int j = q + 1; j < n && j <= q + 4; ++j) {
        if (tk[(size_t)j].upos == "NOUN" || tk[(size_t)j].upos == "PROPN") { pd = j; break; }
        if (!in(tk[(size_t)j].upos, {"ADJ", "NUM", "ADV"})) break;
      }
      if (pd < 0 || o < 0 || tk[(size_t)o].head == pd + 1) continue;
      if (!in(tk[(size_t)o].deprel, {"obl", "nmod", "obj", "dep", "compound", "nsubj"}) || tk[(size_t)o].head - 1 == pd) continue;
      if (tk[(size_t)pd].head == o + 1) {   // the possessed hung on the owner: give it the owner's place
        tk[(size_t)pd].head = tk[(size_t)o].head;
        tk[(size_t)pd].deprel = tk[(size_t)o].deprel;
      } else if (tk[(size_t)o].deprel == "nsubj" && tk[(size_t)pd].deprel != "nsubj") {
        continue;
      }
      tk[(size_t)o].head = pd + 1;
      tk[(size_t)o].deprel = "nmod";
    }
  // C19: "She gave the poor old man some bread and cheese.": the person (object) carries the thing as a noun modifier;
  // with give / show / bring / send / hand / offer the thing is the object and the person the indirect object
  if (lang_ == SrcLang::En)
    for (int v = 0; v < n; ++v) {
      if (tk[(size_t)v].upos != "VERB" ||
          !in(tk[(size_t)v].lemma.empty() ? tk[(size_t)v].lower : text::lower(tk[(size_t)v].lemma),
              {"give", "show", "bring", "send", "hand", "offer", "tell", "teach", "lend", "pass", "throw", "buy"}))
        continue;
      int obj = -1, iobj = -1;
      for (int j = v + 1; j < n; ++j) {
        if (tk[(size_t)j].head != v + 1) continue;
        if (tk[(size_t)j].deprel == "obj") obj = j;
        if (tk[(size_t)j].deprel == "iobj") iobj = j;
      }
      if (obj < 0 || iobj >= 0) continue;
      int thing = -1;
      for (int j = obj + 1; j < n; ++j)
        if (tk[(size_t)j].head == obj + 1 && in(tk[(size_t)j].deprel, {"nmod", "compound", "dep", "appos"}) &&
            (tk[(size_t)j].upos == "NOUN" || tk[(size_t)j].upos == "PROPN")) {
          bool ownCase = false;
          for (int g = 0; g < n; ++g) ownCase = ownCase || (tk[(size_t)g].head == j + 1 && tk[(size_t)g].deprel == "case");
          if (!ownCase) { thing = j; break; }
        }
      if (thing < 0) continue;
      tk[(size_t)obj].deprel = "iobj";
      tk[(size_t)thing].head = v + 1;
      tk[(size_t)thing].deprel = "obj";
      for (int g = obj + 1; g < thing; ++g)   // the thing's determiners ("some")
        if (tk[(size_t)g].head == obj + 1 && in(tk[(size_t)g].deprel, {"det", "amod", "nummod"})) tk[(size_t)g].head = thing + 1;
    }
  // C19: "But to those who are honest,": a cue-initial coordinator taken as the root, the phrase hung on it as "dep":
  // the phrase is the root, the coordinator its cc
  if (lang_ == SrcLang::En && n >= 3 && tk[0].upos == "CCONJ" && tk[0].head == 0) {
    int ph = -1;
    for (int j = 1; j < n; ++j)
      if (tk[(size_t)j].head == 1 && tk[(size_t)j].deprel == "dep" &&
          in(tk[(size_t)j].upos, {"NOUN", "PROPN", "PRON", "ADJ", "NUM"})) { ph = j; break; }
    if (ph > 0) {
      for (int j = 1; j < n; ++j)
        if (j != ph && tk[(size_t)j].head == 1) tk[(size_t)j].head = ph + 1;
      tk[(size_t)ph].head = 0;
      tk[(size_t)ph].deprel = "root";
      tk[0].head = ph + 1;
      tk[0].deprel = "cc";
    }
  }
  // C19: "those who are not honest, or who approach him": a second relative clause hung on the antecedent as a conj
  // belongs to the first relative clause (coordinated relatives)
  if (lang_ == SrcLang::En)
    for (int v = 1; v < n; ++v) {
      if (tk[(size_t)v].deprel != "conj" || tk[(size_t)v].head <= 0) continue;
      const int a = tk[(size_t)v].head - 1;
      bool whoSubj = false;
      for (int j = 0; j < v; ++j)
        whoSubj = whoSubj || (tk[(size_t)j].head == v + 1 && in(tk[(size_t)j].lower, {"who", "which"}) &&
                              in(tk[(size_t)j].deprel, {"nsubj", "obj"}));
      if (!whoSubj) continue;
      int r = -1;
      for (int j = a + 1; j < v; ++j)
        if (tk[(size_t)j].head == a + 1 && tk[(size_t)j].deprel == "acl") { r = j; break; }
      if (r < 0)   // hung on the main verb: the nearest relative clause before it (with its own who / which)
        for (int j = v - 1; j > 0 && r < 0; --j) {
          if (tk[(size_t)j].deprel != "acl") continue;
          for (int g = 0; g < j; ++g)
            if (tk[(size_t)g].head == j + 1 && in(tk[(size_t)g].lower, {"who", "which"})) { r = j; break; }
        }
      if (r < 0) continue;
      tk[(size_t)v].head = r + 1;
    }
  // C19: "We will meet the king tomorrow.": the parser hangs the object on the time word ("the king tomorrow" as one
  // noun phrase); a noun before tomorrow / today / yesterday / tonight is the verb's object when the verb has none
  if (lang_ == SrcLang::En)
    for (int i = 1; i < n; ++i) {
      if (!in(tk[(size_t)i].lower, {"tomorrow", "today", "yesterday", "tonight"})) continue;
      const int v = tk[(size_t)i].head - 1;
      if (v < 0 || (tk[(size_t)v].upos != "VERB" && tk[(size_t)v].upos != "AUX")) continue;
      bool hasObj = false;
      for (int j = 0; j < n; ++j) hasObj = hasObj || (tk[(size_t)j].head == v + 1 && tk[(size_t)j].deprel == "obj");
      if (hasObj) continue;
      for (int j = 0; j < i; ++j) {
        nlp::Token& x = tk[(size_t)j];
        if (x.head != i + 1 || !(x.upos == "NOUN" || x.upos == "PROPN" || x.upos == "PRON") ||
            !in(x.deprel, {"nmod", "compound", "nmod:poss", "obl", "dep"}))
          continue;
        bool ownCase = false;
        for (int g = 0; g < n; ++g) ownCase = ownCase || (tk[(size_t)g].head == j + 1 && tk[(size_t)g].deprel == "case");
        if (ownCase) continue;
        x.head = v + 1;
        x.deprel = "obj";
        for (int g = 0; g < j; ++g)   // its determiners stay with it
          if (tk[(size_t)g].head == i + 1 && in(tk[(size_t)g].deprel, {"det", "amod", "nmod:poss"})) tk[(size_t)g].head = j + 1;
        break;
      }
    }
  // C19: a relative clause after a comma ("I love my teacher, who is very kind.", "our friend, the Cowardly Lion, who
  // is asleep"): the parser hangs it on the main verb as parataxis / conj; it belongs to the noun before the comma
  // (never "et quis ...")
  if (lang_ == SrcLang::En)
    for (int i = 1; i < n; ++i) {
      if (!in(tk[(size_t)i].lower, {"who", "whom", "which"})) continue;
      const bool comma = i >= 2 && tk[(size_t)i - 1].text == ",";
      const int v = tk[(size_t)i].head - 1;
      if (v < 0 || v <= i) continue;
      // without a comma: right after its noun ("people who are honest", "the man who was hungry")
      if (!comma) {
        const int a = i - 1;
        const std::string& u = tk[(size_t)a].upos;
        const bool noun = u == "NOUN" || u == "PROPN" ||
                          (u == "PRON" && in(tk[(size_t)a].lower, {"those", "everyone", "everybody", "anyone", "anybody",
                                                                   "someone", "somebody", "one"}));
        if (!noun || !in(tk[(size_t)i].deprel, {"nsubj", "obj", "nsubj:pass"})) continue;
        const bool loose = in(tk[(size_t)v].deprel, {"parataxis", "conj", "advcl", "ccomp", "dep", "list", "appos", "xcomp"});
        const bool otherNoun = tk[(size_t)v].deprel == "acl" && tk[(size_t)v].head != a + 1 &&
                               tk[(size_t)i].lower == "who" && tk[(size_t)a].deprel == "nmod";
        if (!loose && !otherNoun) continue;
        tk[(size_t)v].head = a + 1;
        tk[(size_t)v].deprel = "acl";
        continue;
      }
      if (!in(tk[(size_t)v].deprel, {"parataxis", "conj", "advcl", "ccomp", "dep", "list", "appos", "root"}))
        continue;
      int a = -1;
      for (int j = i - 2; j >= 0 && j >= i - 8; --j) {
        const std::string& u = tk[(size_t)j].upos;
        if (u == "NOUN" || u == "PROPN" || (u == "PRON" && in(tk[(size_t)j].lower, {"those", "these", "them", "everyone",
                                                                                      "everybody", "anyone", "anybody"}))) { a = j; break; }
        if (u == "VERB" || u == "AUX" || tk[(size_t)j].text == ";") break;
      }
      if (a < 0) continue;
      // a coordinated second relative ("or who approach him") stays a conjunct of the first
      if (tk[(size_t)v].deprel == "conj" && tk[(size_t)v].head > 0 && tk[(size_t)tk[(size_t)v].head - 1].deprel == "acl") continue;
      tk[(size_t)v].head = a + 1;
      tk[(size_t)v].deprel = "acl";
    }
  if (lang_ == SrcLang::En) {
    auto stranded = [&](int i) {   // a preposition without its noun ("I told you of.")
      if (tk[(size_t)i].upos != "ADP" && !(tk[(size_t)i].upos == "ADV" && in(tk[(size_t)i].lower, {"of"}))) return false;
      if (!in(tk[(size_t)i].lower, {"of", "about", "with", "to", "for", "from", "in", "on", "at"})) return false;
      for (int j = 0; j < n; ++j)
        if (tk[(size_t)j].head == i + 1) return false;
      return tk[(size_t)i].deprel == "obl" || tk[(size_t)i].deprel == "advmod" || tk[(size_t)i].deprel == "case" ||
             tk[(size_t)i].deprel == "compound";
    };
    // C22: "a dog with a hat and a stick": a noun coordinated after the noun's "with" phrase belongs to that phrase
    // (the dog has both), not to the dog
    for (int h = 0; h < n; ++h) {
      if (tk[(size_t)h].upos != "NOUN") continue;
      int w = -1;
      for (int q = h + 1; q < n; ++q)
        if (tk[(size_t)q].head == h + 1 && tk[(size_t)q].deprel == "nmod" && tk[(size_t)q].upos == "NOUN") {
          bool with = false;
          for (int g = h + 1; g < q; ++g) with = with || (tk[(size_t)g].head == q + 1 && tk[(size_t)g].lower == "with");
          if (with) { w = q; break; }
        }
      if (w < 0) continue;
      for (int k = w + 1; k < n; ++k)
        if (tk[(size_t)k].head == h + 1 && tk[(size_t)k].deprel == "conj" && tk[(size_t)k].upos == "NOUN") tk[(size_t)k].head = w + 1;
    }
    // C22: a verb the parser left as "dep" with its own "and" before it is a coordinated verb ("... and come out the
    // other side")
    for (int v = 0; v < n; ++v) {
      if (tk[(size_t)v].deprel != "dep" || tk[(size_t)v].upos != "VERB") continue;
      bool cc = false;
      for (int q = 0; q < v; ++q) cc = cc || (tk[(size_t)q].head == v + 1 && tk[(size_t)q].deprel == "cc");
      if (cc) tk[(size_t)v].deprel = "conj";
    }
    // C22: "What if I should fall ...?": the clause after "if" is the root, "what if" opens it (Quid sī ...?)
    if (n > 3 && tk[0].lower == "what" && tk[1].lower == "if" && tk[0].head == 0) {
      const int v = tk[1].head - 1;
      if (v > 1 && v < n && tk[(size_t)v].head == 1) {
        tk[(size_t)v].head = 0;
        tk[(size_t)v].deprel = "root";
        for (int q = 0; q < n; ++q)
          if (q != v && tk[(size_t)q].head == 1) tk[(size_t)q].head = v + 1;
        tk[0].head = v + 1;
        tk[0].deprel = "cc";
        tk[0].upos = "CCONJ";
        tk[0].lemma = "what-if";
        tk[0].lower = "what-if";
        tk[1].deprel = "cc";
        tk[1].upos = "CCONJ";
        s.repairs.emplace_back("reroot");
      }
    }
    // C22: an unfinished sentence that stops at its verb ("The king's answer at first was..."): the last word, a form
    // of "be" hung on the root noun, is the verb and the noun its subject (the verb was lost)
    {
      int last = n - 1;
      while (last > 0 && in(tk[(size_t)last].upos, {"PUNCT"})) --last;
      const int r = last >= 0 ? tk[(size_t)last].head - 1 : -1;
      if (last > 1 && in(tk[(size_t)last].lower, {"was", "is", "were", "are", "be"}) &&
          !in(tk[(size_t)last].deprel, {"cop", "aux", "root"}) && r >= 0 && r < last && tk[(size_t)r].head == 0 &&
          in(tk[(size_t)r].upos, {"NOUN", "PROPN", "PRON"})) {
        bool kids = false;
        for (int q = 0; q < n; ++q)
          kids = kids || (tk[(size_t)q].head == last + 1 && in(tk[(size_t)q].deprel, {"nsubj", "obj", "cop", "xcomp", "ccomp", "expl"}));
        if (!kids) {
          tk[(size_t)last].head = 0;
          tk[(size_t)last].deprel = "root";
          tk[(size_t)last].upos = "VERB";
          tk[(size_t)r].head = last + 1;
          tk[(size_t)r].deprel = "nsubj";
          for (int q = 0; q < n; ++q)
            if (q != r && tk[(size_t)q].head == r + 1 && (tk[(size_t)q].upos == "PUNCT" || tk[(size_t)q].deprel == "advmod" || tk[(size_t)q].deprel == "obl"))
              tk[(size_t)q].head = last + 1;
          s.repairs.emplace_back("reroot");
        }
      }
    }
    // C22: "Robert, the Bishop of London, agreed ...": a name read as a vocative root and the apposition as the
    // subject: the name is the subject, the noun phrase its apposition
    for (int r = 0; r + 3 < n; ++r) {
      if (tk[(size_t)r].head != 0 || tk[(size_t)r].upos != "PROPN" || tk[(size_t)r + 1].text != ",") continue;
      bool lead = true;
      for (int q = 0; q < r; ++q) lead = lead && in(tk[(size_t)q].upos, {"PUNCT", "CCONJ", "ADV"});
      if (!lead) break;
      int j = -1;
      for (int q = r + 2; q < n; ++q)
        if (tk[(size_t)q].deprel == "nsubj" && in(tk[(size_t)q].upos, {"NOUN", "PROPN"})) { j = q; break; }
      if (j < 0) break;
      const int v = tk[(size_t)j].head - 1;
      bool the = false;
      for (int q = r + 2; q < j; ++q) the = the || (tk[(size_t)q].lower == "the" && tk[(size_t)q].head == j + 1);
      bool commaBeforeVerb = false;
      for (int q = j + 1; q < v; ++q) commaBeforeVerb = commaBeforeVerb || tk[(size_t)q].text == ",";
      if (!the || v <= j || !commaBeforeVerb || tk[(size_t)v].upos != "VERB") break;
      tk[(size_t)r].head = v + 1;
      tk[(size_t)r].deprel = "nsubj";
      tk[(size_t)j].head = r + 1;
      tk[(size_t)j].deprel = "appos";
      tk[(size_t)v].head = 0;
      tk[(size_t)v].deprel = "root";
      for (int q = 0; q < n; ++q)
        if (q != r && q < r && tk[(size_t)q].head == r + 1 && tk[(size_t)q].deprel != "punct") tk[(size_t)q].head = v + 1;
      s.repairs.emplace_back("reroot");
      break;
    }
    // C24: "And even Paul, the Bishop of York, agreed to ...": the name is the root with its apposition and the verb
    // after the second comma hangs on it without a subject (dep / acl / parataxis): the verb is the root, the name
    // its subject (the long quoted sentences of a history lesson)
    for (int r = 0; r + 4 < n; ++r) {
      if (tk[(size_t)r].head != 0 || tk[(size_t)r].upos != "PROPN" || tk[(size_t)r + 1].text != ",") continue;
      bool lead = true;
      for (int q = 0; q < r; ++q) lead = lead && in(tk[(size_t)q].upos, {"PUNCT", "CCONJ", "ADV"});
      if (!lead) break;
      int ap = -1, v = -1;
      for (int q = r + 2; q < n; ++q) {
        if (tk[(size_t)q].head != r + 1) continue;
        if (tk[(size_t)q].deprel == "appos" && ap < 0) ap = q;
        else if (ap >= 0 && q > ap && tk[(size_t)q].upos == "VERB" && in(tk[(size_t)q].deprel, {"dep", "acl", "parataxis"})) { v = q; break; }
      }
      if (ap < 0 || v < 0 || tk[(size_t)v - 1].text != ",") break;
      bool subj = false;
      for (int q = 0; q < n; ++q) subj = subj || (tk[(size_t)q].head == v + 1 && in(tk[(size_t)q].deprel, {"nsubj", "nsubj:pass"}));
      if (subj) break;
      for (int q = 0; q < n; ++q)
        if (q != v && tk[(size_t)q].head == r + 1 && (q < r ? tk[(size_t)q].deprel != "punct" : q > v)) tk[(size_t)q].head = v + 1;
      tk[(size_t)v].head = 0;
      tk[(size_t)v].deprel = "root";
      tk[(size_t)r].head = v + 1;
      tk[(size_t)r].deprel = "nsubj";
      s.repairs.emplace_back("reroot");
      break;
    }
    // C22: a fronted prepositional phrase read as the root noun with the clause hung on it ("Within the castle we
    // were safe." -> castle + acl): the clause is the root, the phrase its oblique
    for (int r = 0; r < n; ++r) {
      if (tk[(size_t)r].head != 0 || !in(tk[(size_t)r].upos, {"NOUN", "PROPN", "PRON"})) continue;
      bool cased = false;
      for (int j = 0; j < r; ++j)
        if (tk[(size_t)j].head == r + 1 && tk[(size_t)j].deprel == "case" && tk[(size_t)j].upos == "ADP") cased = true;
      if (!cased) break;
      for (int k = r + 1; k < n; ++k) {
        if (tk[(size_t)k].head != r + 1 || !in(tk[(size_t)k].deprel, {"acl", "acl:relcl", "ccomp", "advcl", "parataxis"})) continue;
        bool subj = false, rel = false;
        for (int g = r + 1; g < k; ++g) {
          if (tk[(size_t)g].head != k + 1) continue;
          if (tk[(size_t)g].deprel == "nsubj" && tk[(size_t)g].upos == "PRON" &&
              !in(tk[(size_t)g].lower, {"that", "which", "who", "what"})) subj = true;
          if (in(tk[(size_t)g].lower, {"that", "which", "who", "whom", "whose", "where", "when"})) rel = true;
        }
        if (!subj || rel) continue;
        tk[(size_t)k].head = 0;
        tk[(size_t)k].deprel = "root";
        tk[(size_t)r].head = k + 1;
        tk[(size_t)r].deprel = "obl";
        for (int j = k + 1; j < n; ++j)
          if (tk[(size_t)j].head == r + 1 && tk[(size_t)j].deprel == "punct") tk[(size_t)j].head = k + 1;
        s.repairs.emplace_back("reroot");
        break;
      }
      break;
    }
    // C15: "the Great Wizard I told you of." / "a man I know": a noun read as the subject of a verb that has a
    // pronoun subject right after it is the antecedent of a contact relative clause
    for (int v = 0; v < n; ++v) {
      if (tk[(size_t)v].upos != "VERB") continue;
      int nounSubj = -1, pronSubj = -1;
      for (int j = 0; j < n; ++j) {
        if (tk[(size_t)j].head != v + 1 || tk[(size_t)j].deprel != "nsubj") continue;
        if ((tk[(size_t)j].upos == "NOUN" || tk[(size_t)j].upos == "PROPN") && nounSubj < 0) nounSubj = j;
        else if (tk[(size_t)j].upos == "PRON") pronSubj = j;
      }
      if (nounSubj < 0 || pronSubj != nounSubj + 1 || pronSubj > v) continue;
      tk[(size_t)nounSubj].head = tk[(size_t)v].head;
      tk[(size_t)nounSubj].deprel = tk[(size_t)v].deprel;
      for (int j = 0; j < n; ++j)
        if (tk[(size_t)j].head == v + 1 && j < nounSubj) tk[(size_t)j].head = nounSubj + 1;   // its determiners
      for (int j = 0; j < n; ++j)
        if (j != v && tk[(size_t)j].head == v + 1 && tk[(size_t)j].deprel == "punct" && j == n - 1) tk[(size_t)j].head = nounSubj + 1;
      tk[(size_t)v].head = nounSubj + 1;
      tk[(size_t)v].deprel = "acl";
      s.repairs.emplace_back("reroot");
      break;
    }
    // C15: "The woman who sings is my mother." parsed as "woman [who sings mother]" with an orphan copula on the
    // object: the object is the predicate, the noun with its relative clause the subject
    for (int cp = 0; cp < n; ++cp) {
      if (tk[(size_t)cp].deprel != "cop") continue;
      const int x = tk[(size_t)cp].head - 1;
      if (x < 0 || tk[(size_t)x].deprel != "obj") continue;
      const int v = tk[(size_t)x].head - 1;
      if (v < 0 || tk[(size_t)v].deprel != "acl" || x < cp) continue;
      const int nn = tk[(size_t)v].head - 1;
      if (nn < 0 || tk[(size_t)nn].head != 0 || cp < v) continue;
      tk[(size_t)x].head = 0;
      tk[(size_t)x].deprel = "root";
      tk[(size_t)nn].head = x + 1;
      tk[(size_t)nn].deprel = "nsubj";
      for (int j = 0; j < n; ++j)
        if (tk[(size_t)j].head == nn + 1 && tk[(size_t)j].deprel == "punct" && j > x) tk[(size_t)j].head = x + 1;
      s.repairs.emplace_back("reroot");
      break;
    }
    // C15: a to-infinitive hung on the verb of a contact relative after its stranded preposition ("the arts I know of
    // to keep you from harm") belongs to the main clause (purpose)
    for (int v = 0; v < n; ++v) {
      if (tk[(size_t)v].deprel != "acl") continue;
      int prepAt = -1;
      for (int j = v + 1; j < n; ++j)
        if (tk[(size_t)j].head == v + 1 && stranded(j)) { prepAt = j; break; }
      if (prepAt < 0) continue;
      const int noun = tk[(size_t)v].head - 1;
      if (noun < 0) continue;
      const int mainV = tk[(size_t)noun].head - 1;
      if (mainV < 0 || (tk[(size_t)mainV].upos != "VERB" && tk[(size_t)mainV].upos != "AUX")) continue;
      for (int j = prepAt + 1; j < n; ++j) {
        if (tk[(size_t)j].head != v + 1 || tk[(size_t)j].upos != "VERB") continue;
        bool to = false;
        for (int g = 0; g < n; ++g)
          if (tk[(size_t)g].head == j + 1 && tk[(size_t)g].lower == "to") to = true;
        if (!to) continue;
        tk[(size_t)j].head = mainV + 1;
        tk[(size_t)j].deprel = "advcl";
      }
    }
  }
  // C17: "She went to the river to wash the clothes": a to-infinitive hung on the goal of a verb of motion is the
  // purpose of the motion, not a property of the place
  if (lang_ == SrcLang::En)
    for (int v = 0; v < n; ++v) {
      if (tk[(size_t)v].upos != "VERB" || !in(tk[(size_t)v].lemma.empty() ? tk[(size_t)v].lower : tk[(size_t)v].lower,
                                              {"go", "goes", "went", "gone", "going", "come", "comes", "came", "coming",
                                               "run", "ran", "runs", "running", "walk", "walked", "walks", "hurry",
                                               "hurried", "return", "returned", "ride", "rode", "fly", "flew", "sail",
                                               "sailed", "travel", "travelled", "traveled"}))
        continue;
      for (int o = v + 1; o < n; ++o) {
        if (tk[(size_t)o].head != v + 1 || tk[(size_t)o].deprel != "obl") continue;
        for (int a = o + 1; a < n; ++a) {
          if (tk[(size_t)a].head != o + 1 || tk[(size_t)a].deprel != "acl" || tk[(size_t)a].upos != "VERB") continue;
          bool to = false;
          for (int g = 0; g < n; ++g)
            if (tk[(size_t)g].head == a + 1 && tk[(size_t)g].lower == "to" && g < a) to = true;
          if (!to) continue;
          tk[(size_t)a].head = v + 1;
          tk[(size_t)a].deprel = "advcl";
        }
      }
    }
  // C17: two subjects of one verb joined by "and" ("The Scarecrow and the Lion were happy."): the second is a
  // conjunct of the first
  if (lang_ == SrcLang::En)
    for (int v = 0; v < n; ++v) {
      int s1 = -1, s2 = -1;
      for (int j = 0; j < n; ++j)
        if (tk[(size_t)j].head == v + 1 && tk[(size_t)j].deprel == "nsubj") { if (s1 < 0) s1 = j; else if (s2 < 0) s2 = j; }
      if (s1 < 0 || s2 < 0 || s2 < s1) continue;
      int cc = -1;
      for (int j = s1 + 1; j < s2; ++j)
        if (tk[(size_t)j].upos == "CCONJ") cc = j;
      if (cc < 0) continue;
      tk[(size_t)s2].head = s1 + 1;
      tk[(size_t)s2].deprel = "conj";
      tk[(size_t)cc].head = s2 + 1;
      tk[(size_t)cc].deprel = "cc";
    }
  // C17: "cry a little because ...": "a little" right after a verb is that verb's degree adverb
  if (lang_ == SrcLang::En)
    for (int i = 1; i + 1 < n; ++i) {
      if (tk[(size_t)i].lower != "a" || tk[(size_t)i + 1].lower != "little" || tk[(size_t)i - 1].upos != "VERB") continue;
      if (i + 2 < n && in(tk[(size_t)i + 2].upos, {"NOUN", "ADJ", "PROPN"})) continue;   // "a little girl", "a little cold"
      bool kids = false;
      for (int j = 0; j < n; ++j)
        if (tk[(size_t)j].head == i + 2 && j != i) kids = true;
      if (kids) {   // the parser hung a following clause on "little": give it to the verb
        for (int j = 0; j < n; ++j)
          if (tk[(size_t)j].head == i + 2 && j != i) tk[(size_t)j].head = i;
      }
      tk[(size_t)i + 1].upos = "ADV";
      tk[(size_t)i + 1].head = i;
      tk[(size_t)i + 1].deprel = "advmod";
      tk[(size_t)i].upos = "DET";
      tk[(size_t)i].head = i + 2;
      tk[(size_t)i].deprel = "det";
    }
  // C17: "(he asked me) what you looked like" / "What does it look like?": "what" is the stranded object of "like";
  // the look / seem verb heads the (indirect) question
  if (lang_ == SrcLang::En)
    for (int i = 0; i + 2 < n; ++i) {
      if (tk[(size_t)i].lower != "what") continue;
      int v = -1;
      for (int j = i + 1; j < n && j <= i + 4; ++j)
        if (in(tk[(size_t)j].lower, {"look", "looks", "looked", "seem", "seems", "seemed"})) { v = j; break; }
      if (v < 0 || v + 1 >= n || tk[(size_t)v + 1].lower != "like") continue;
      const int lk = v + 1;
      if (lk + 1 < n && !isPunctTok(tk[(size_t)lk + 1])) continue;
      int m = -1;
      for (int j = i - 1; j >= 0 && m < 0; --j)
        if (tk[(size_t)j].upos == "VERB") m = j;
      for (int j = i + 1; j < v; ++j) {
        if (in(tk[(size_t)j].upos, {"PRON", "NOUN", "PROPN"})) { tk[(size_t)j].head = v + 1; tk[(size_t)j].deprel = "nsubj"; }
        else if (tk[(size_t)j].upos == "AUX") { tk[(size_t)j].head = v + 1; tk[(size_t)j].deprel = "aux"; }
        else if (tk[(size_t)j].upos == "DET") { /* keep */ }
      }
      for (int j = 0; j < n; ++j)   // dependents hung on "what" / "like" go to the verb
        if ((tk[(size_t)j].head == i + 1 || tk[(size_t)j].head == lk + 1) && j != v && j != lk && j != i)
          tk[(size_t)j].head = v + 1;
      tk[(size_t)v].upos = "VERB";
      if (m >= 0) { tk[(size_t)v].head = m + 1; tk[(size_t)v].deprel = "ccomp"; }
      else { tk[(size_t)v].head = 0; tk[(size_t)v].deprel = "root"; }
      tk[(size_t)i].head = v + 1;
      tk[(size_t)i].deprel = "obl";
      tk[(size_t)lk].upos = "ADP";
      tk[(size_t)lk].head = i + 1;
      tk[(size_t)lk].deprel = "case";
      if (m < 0)
        for (int j = 0; j < n; ++j)
          if (j != v && tk[(size_t)j].head == 0) tk[(size_t)j].head = v + 1;
      break;
    }
  // "Here the cat always sleeps.": a fronted adverb made the root, the real clause hung on it as advcl -> the verb is
  // the root, the adverb its (fronted) advmod
  for (int r = 0; r < n; ++r) {
    if (tk[(size_t)r].head != 0 || tk[(size_t)r].upos != "ADV") continue;
    int v = -1;
    for (int j = 0; j < n; ++j)
      if (tk[(size_t)j].head == r + 1 && tk[(size_t)j].upos == "VERB" && j > r &&
          in(tk[(size_t)j].deprel, {"advcl", "parataxis", "ccomp"})) { v = j; break; }
    if (v < 0) continue;
    bool mark = false, subj = false;
    for (int j = 0; j < n; ++j)
      if (tk[(size_t)j].head == v + 1) {
        mark = mark || tk[(size_t)j].deprel == "mark";
        subj = subj || tk[(size_t)j].deprel == "nsubj";
      }
    if (mark || !subj) continue;
    for (int j = 0; j < n; ++j)
      if (tk[(size_t)j].head == r + 1 && j != v) tk[(size_t)j].head = v + 1;
    tk[(size_t)v].head = 0;
    tk[(size_t)v].deprel = "root";
    tk[(size_t)r].head = v + 1;
    tk[(size_t)r].deprel = "advmod";
    s.repairs.emplace_back("reroot");
    break;
  }
  for (int i = 0; i < n; ++i) {
    nlp::Token& d = tk[(size_t)i];
    if (!in(d.deprel, {"obl", "obj", "advmod", "nmod", "amod"})) continue;
    const int p = d.head - 1;
    if (p < 0 || p >= n || (tk[(size_t)p].upos != "VERB" && tk[(size_t)p].upos != "AUX")) continue;
    bool subj = false, verbal = false, cc = false, mark = false;
    for (int j = 0; j < n; ++j) {
      if (tk[(size_t)j].head != i + 1) continue;
      const std::string& r = tk[(size_t)j].deprel;
      subj = subj || r == "nsubj";
      verbal = verbal || r == "cop" || r == "aux";
      cc = cc || r == "cc";
      mark = mark || (r == "mark" && tk[(size_t)j].lower == "que");
    }
    // Spanish pro-drop (C13): "Pensé que era lunes." has no subject but the complementiser "que"
    if (lang_ == SrcLang::Es && mark && verbal) subj = true;
    if (!subj || !verbal) continue;
    d.deprel = cc ? "conj" : "ccomp";
    if (std::find(s.repairs.begin(), s.repairs.end(), "clause-repair") == s.repairs.end())
      s.repairs.emplace_back("clause-repair");
  }
}

bool FrameBuilder::troubled(const SemSentence& s) {
  for (const Unit& u : s.units) {
    if (u.type != Unit::Clause || u.vocative) continue;
    bool verb = false;
    // C15: verbs of a relative clause on the fragment's noun are where they belong ("the Great Wizard I told you of.")
    std::vector<int> relTok;
    // C19: also the relative clauses of a fragment's prepositional phrase ("to those who are not honest, or who
    // approach him") and the clauses coordinated inside them
    struct Collect {
      static void frame(const SemFrame& f, std::vector<int>& out) {
        out.insert(out.end(), f.tokens.begin(), f.tokens.end());
        for (const SemSub& sb : f.subordinate)
          for (const SemFrame& g : sb.frame) frame(g, out);
      }
    };
    if (u.frame.hasSubject)
      for (const SemFrame& r : u.frame.subject.relative) Collect::frame(r, relTok);
    if (u.frame.type == Kind::Frag && !u.frame.hasPred)
      for (const SemOblique& o : u.frame.obliques)
        for (const SemFrame& r : o.np.relative) Collect::frame(r, relTok);
    for (int k = u.first; k <= u.last && k < (int)s.tokens.size(); ++k)
      if (s.tokens[(size_t)k].upos == "VERB" && s.drop[(size_t)k] == Drop::No &&
          std::find(relTok.begin(), relTok.end(), k) == relTok.end())
        verb = true;
    if (verb && (!u.frame.hasPred || u.frame.type == Kind::Frag)) return true;
  }
  return false;
}

// C15: segments parsed on their own. A sentence-initial discourse word before a comma ("Oh,", "Why,", "But,"), a
// vocative between commas ("comrades", "my dear", "your Majesty"), a parenthetical between commas ("you know",
// "as I said", "however") and every clause after ";" / ":" / a dash is parsed apart: each becomes a unit of its own
// (the frame builder's groups follow the roots), so a stray word no longer bends the parse of the clause.
bool FrameBuilder::segmentParse(std::vector<Token>& tk) const {
  const int n = (int)tk.size();
  if (n < 3 || !nlp_) return false;
  std::vector<int> cuts;   // segment starts (after the separator)
  auto addCut = [&](int at) {
    if (at > 0 && at < n && std::find(cuts.begin(), cuts.end(), at) == cuts.end()) cuts.push_back(at);
  };
  for (int i = 0; i + 1 < n; ++i)
    if (hardBreak(tk[(size_t)i]) || (tk[(size_t)i].lower == "to" && i > 0 && tk[(size_t)i + 1].text == "," &&
                                     tk[(size_t)i - 1].upos == "VERB" && i + 2 < n))   // C15: "Even if I wanted to, ..."
      addCut(tk[(size_t)i].lower == "to" ? i + 2 : i + 1);
  // starts of comma-delimited spans
  std::vector<int> starts = {0};
  for (int i = 0; i + 1 < n; ++i)
    if (segmentBreak(tk[(size_t)i])) starts.push_back(i + 1);
  for (size_t si = 0; si < starts.size(); ++si) {
    const int a = starts[si];
    int b = a;   // last token of the span (before its separator / the end)
    while (b + 1 < n && !segmentBreak(tk[(size_t)b + 1]) && !(tk[(size_t)b + 1].upos == "PUNCT" && b + 2 == n)) ++b;
    const int sep = b + 1;   // separator or final punctuation or n
    const bool sepIsBreak = sep < n && segmentBreak(tk[(size_t)sep]);
    const bool atEnd = sep >= n || (sep + 1 == n && tk[(size_t)sep].upos == "PUNCT" && !sepIsBreak);
    if (!sepIsBreak && !atEnd) continue;
    bool cut = false;
    if (a == 0 && b == 0 && sepIsBreak && leadWord(tk[0].lower)) cut = true;            // "Oh, ..."
    // "But, comrades, ...": a one-word "verb" after a lead word that the lexicon also reads as a noun is the addressee
    if (a == 2 && b == 2 && sepIsBreak && leadWord(tk[0].lower) && tk[2].upos == "VERB" && lex_) {
      std::vector<lex::Analysis> an;
      lex_->lookup(text::en_key(tk[2].lower), an);
      for (const lex::Analysis& x : an)
        if (lex_->lemma(x.lemma).pos == feat::Noun) {
          tk[2].upos = "NOUN";
          tk[2].feats = nlp::morph::fromString(feat::unpack(lex_->feature(x.feat)).number == feat::Pl ? "Number=Plur" : "Number=Sing");
          break;
        }
    }
    if (!cut && (sepIsBreak || a > 0) && !(a == 0 && atEnd) && vocativeSpan(tk, a, b)) {
      // a vocative needs something else in the sentence and no article; time nouns are adverbs, not addressees
      bool time = false;
      for (int i = a; i <= b; ++i) time = time || timeNoun(tk[(size_t)i].lower);
      cut = !time;
    }
    if (!cut && (a > 0 || sepIsBreak))
      for (const auto& p : kParentheticals) {
        int k = 0;
        while (k < 4 && p[k]) ++k;
        if (b - a + 1 != k) continue;
        bool same = true;
        for (int j = 0; j < k; ++j) same = same && tk[(size_t)a + (size_t)j].lower == p[j];
        if (same) { cut = true; break; }
      }
    if (cut) { addCut(a); if (sep + 1 < n && sepIsBreak) addCut(sep + 1); }
  }
  if (cuts.empty()) return false;
  std::sort(cuts.begin(), cuts.end());
  cuts.push_back(n);
  int a = 0;
  for (int cutAt : cuts) {
    if (cutAt <= a) continue;
    std::vector<Token> seg(tk.begin() + a, tk.begin() + cutAt);
    for (Token& t : seg) { t.head = -1; t.deprel.clear(); }
    nlp_->parser().parse(seg);
    for (int i = 0; i < (int)seg.size(); ++i) {
      Token& t = tk[(size_t)(a + i)];
      t.head = seg[(size_t)i].head > 0 ? seg[(size_t)i].head + a : 0;
      t.deprel = seg[(size_t)i].deprel;
    }
    a = cutAt;
  }
  return true;
}

std::vector<size_t> FrameBuilder::splitPoints(std::string_view t) {
  std::vector<size_t> out;
  for (size_t i = 0; i + 1 < t.size(); ++i) {
    if (t[i] == ';') {
      size_t j = i + 1;
      while (j < t.size() && t[j] == ' ') ++j;
      if (j < t.size()) out.push_back(j);
      continue;
    }
    if (t[i] != ',') continue;
    size_t j = i + 1;
    while (j < t.size() && t[j] == ' ') ++j;
    for (const char* c : {"or ", "and ", "but ", "o ", "y ", "pero "}) {
      const size_t m = std::strlen(c);
      if (t.substr(j, m) == c) { out.push_back(j); break; }
    }
  }
  return out;
}

void FrameBuilder::analyse(std::string_view sentence, SemSentence& out, bool classical) const {
  out.clear();
  out.classical = classical;
  out.lang = lang_;
  out.text = std::string(sentence);
  tokenize(sentence, out.tokens);
  // C17: a cleft wh question ("How was it that you appeared ...?", "Why is it that ...?") is the plain question: the
  // words "was it that" carry nothing to translate
  if (lang_ == SrcLang::En && out.tokens.size() > 5) {
    std::vector<nlp::Token>& t0 = out.tokens;
    auto low = [&](size_t i) { return text::lower(t0[i].text); };
    if (in(low(0), {"how", "why", "where", "when", "what"}) && in(low(1), {"was", "is"}) && low(2) == "it" &&
        low(3) == "that")
      t0.erase(t0.begin() + 1, t0.begin() + 4);
  }
  out.drop.assign(out.tokens.size(), Drop::No);
  if (out.tokens.empty()) return;
  if (nlp_) nlp_->analyse(out.tokens);
  // C22: "<clause>, too." at the end: the comma is dropped so the parser hangs "too" on the clause (it is "also",
  // quoque) instead of a fragment "too" of its own (nimis); a verbless fragment keeps it (merged in buildUnits)
  if (nlp_ && lang_ == SrcLang::En && out.tokens.size() >= 4) {
    std::vector<nlp::Token>& t0 = out.tokens;
    size_t last = t0.size() - 1;
    while (last > 0 && (t0[last].text == "." || t0[last].text == "!" || t0[last].text == "?")) --last;
    bool verb = false;
    for (size_t q = 0; q + 2 < last; ++q) verb = verb || t0[q].upos == "VERB" || t0[q].upos == "AUX";
    if (verb && last >= 2 && text::lower(t0[last].text) == "too" && t0[last - 1].text == ",") {
      t0.erase(t0.begin() + (long)last - 1);
      out.drop.assign(out.tokens.size(), Drop::No);
      nlp_->analyse(out.tokens);
    }
  }
  std::vector<nlp::Token>& tk = out.tokens;
  const int n = (int)tk.size();
  // C17: forms the tagger misread, corrected from english.vpl (irregular pasts, a past read as a noun, an adjective
  // read as a noun), then a new parse
  if (nlp_ && lex_ && lang_ == SrcLang::En) {
    for (nlp::Token& t : tk)
      if (t.lower.empty()) t.lower = nlp::normalise(t.text);
    const std::string firstTag = tk.empty() ? std::string() : tk[0].upos;
    bool again = en::retagForms(tk, *lex_);
    // C19: a sentence-initial word the tagger took for a name and the lexicon gives back as a verb or a common noun
    // ("Hurry, ...", "Grandmother, ..."): a repaired analysis (Check), as every rebuilt structure
    if (!tk.empty() && (firstTag == "PROPN" || firstTag == "INTJ") && tk[0].upos != firstTag) out.repairs.emplace_back("retag");
    // C24: "After this he will ...", "Before that, we ...": the preposition and the pronoun "this" / "that" (an
    // oblique of time), not a subordinator and a subject
    if (n >= 4 && in(tk[0].lower, {"after", "before"}) && in(tk[1].lower, {"this", "that"}) &&
        (tk[2].text == "," || in(tk[2].upos, {"PRON", "PROPN", "NOUN", "DET"}))) {
      if (tk[0].upos != "ADP" || tk[1].upos != "PRON") again = true;
      tk[0].upos = "ADP";
      tk[0].feats = 0;
      tk[1].upos = "PRON";
      tk[1].feats = nlp::morph::fromString("Number=Sing|PronType=Dem");
    }
    // "so that" + clause (purpose): both words are the subordinator, not "so" + the pronoun "that"
    for (int i = 0; i + 2 < n; ++i)
      if (tk[(size_t)i].lower == "so" && tk[(size_t)i + 1].lower == "that" &&
          (i == 0 || !in(tk[(size_t)i - 1].upos, {"ADJ", "ADV"})) &&
          in(tk[(size_t)i + 2].upos, {"PRON", "PROPN", "NOUN", "DET", "ADP"}) && tk[(size_t)i + 1].upos != "SCONJ") {
        tk[(size_t)i].upos = "SCONJ";
        tk[(size_t)i].feats = 0;
        tk[(size_t)i + 1].upos = "SCONJ";
        tk[(size_t)i + 1].feats = 0;
        again = true;
      }
    // C22: a greeting after "say" ("say hello to grandmother", "said goodbye") is the object of "say" (rebuilt as
    // salūtāre / valedīcere by the clause builder), not an interjection or a clause
    for (size_t ti = 0; ti + 1 < tk.size(); ++ti)
      if (in(tk[ti].lower, {"say", "says", "said", "saying"}) &&
          in(tk[ti + 1].lower, {"hello", "hi", "goodbye", "bye", "farewell", "good-bye"}) && tk[ti + 1].upos != "NOUN") {
        tk[ti + 1].upos = "NOUN";
        tk[ti + 1].feats = nlp::morph::fromString("Number=Sing");
        again = true;
      }
    // C22: a capitalised word of names_la.tsv (a person or a place, not a title row) that the tagger read as a common
    // noun, an adjective or an interjection ("Lucy!", "Mary in the garden") is the name
    for (size_t ti = 0; ti < tk.size(); ++ti) {
      nlp::Token& t = tk[ti];
      if (t.text.empty() || !(t.text[0] >= 'A' && t.text[0] <= 'Z') || t.upos == "PROPN") continue;
      const curated::NameEntry* ne = cd_.nameByEnglish(t.text);
      if (!ne || ne->policy == curated::NamePolicy::Translate) continue;
      bool first = true;   // "Mark the page.": a sentence-initial verb stays a verb
      for (size_t q = 0; q < ti; ++q) first = first && tk[q].upos == "PUNCT";
      if (!in(t.upos, {"NOUN", "ADJ", "INTJ", "X"}) && !(!first && in(t.upos, {"VERB", "ADV"}))) continue;
      t.upos = "PROPN";
      t.feats = nlp::morph::fromString("Number=Sing");
      again = true;
    }
    if (again) nlp_->parser().parse(tk);
  }
  // no verb at all in a sentence of three or more words: the tagger probably missed one; retag and re-parse
  // ("Light the candle." -> imperative; "My mother teaches children." -> 3rd person present)
  if (nlp_ && lex_ && lang_ == SrcLang::En && n >= 3) {
    bool verb = false;
    for (const nlp::Token& t : tk)
      verb = verb || t.upos == "VERB" || (t.upos == "AUX" && !in(t.lower, {"do", "does", "did"}));
    auto verbReading = [&](const nlp::Token& t) {
      std::vector<lex::Analysis> an;
      lex_->lookup(text::en_key(t.lower), an);
      for (const lex::Analysis& a : an)
        if (lex_->lemma(a.lemma).pos == feat::Verb) return true;
      return false;
    };
    int fix = -1;
    uint32_t feats = 0;
    // C15: an "adjective" the lexicon knows only as a noun ("The farmer carries water.": farmer tagged as the
    // comparative of "farm") is a noun; then the -s word after it can be the verb
    std::vector<int> nounFix;
    if (!verb)
      for (int i = 0; i + 1 < n; ++i) {
        if (tk[(size_t)i].upos != "ADJ") continue;
        std::vector<lex::Analysis> an;
        lex_->lookup(text::en_key(tk[(size_t)i].lower), an);
        bool noun = false, adj = false;
        for (const lex::Analysis& a : an) {
          const uint8_t pos = lex_->lemma(a.lemma).pos;
          noun = noun || pos == feat::Noun;
          adj = adj || pos == feat::Adj || pos == feat::Participle;
        }
        if (noun && !adj) nounFix.push_back(i);
      }
    auto nounLike = [&](int i) {
      const std::string& pu = tk[(size_t)i].upos;
      return pu == "NOUN" || pu == "PROPN" || pu == "PRON" ||
             std::find(nounFix.begin(), nounFix.end(), i) != nounFix.end();
    };
    if (!verb) {
      // "He does not like fish": the word after do + not; "I don't much care": one degree adverb in between
      for (int i = 2; i < n && fix < 0; ++i) {
        int j = i - 1;
        const bool degree = in(tk[(size_t)j].lower, {"much", "really", "even", "ever", "always", "just", "quite"});
        if (degree) --j;
        if (j < 1) continue;
        if ((tk[(size_t)j].lower == "not" || tk[(size_t)j].lower == "n't") &&
            in(tk[(size_t)j - 1].lower, {"do", "does", "did"}) && tk[(size_t)i].upos != "VERB" &&
            verbReading(tk[(size_t)i])) {
          fix = i;
          feats = nlp::morph::fromString("VerbForm=Inf");
          if (degree) { tk[(size_t)i - 1].upos = "ADV"; tk[(size_t)i - 1].feats = 0; }
        }
      }
    }
    if (!verb && fix < 0) {
      if ((tk[1].upos == "DET" || tk[1].upos == "PRON") && verbReading(tk[0]) && tk[0].upos != "ADP" &&
          !in(tk[0].lower, {"and", "or", "but", "nor", "just", "only", "merely", "simply", "not", "even"})) {   // C19: "with her grandmother.", "and the queen's crown,"; C26: "Just an old box."
        fix = 0;
        feats = nlp::morph::fromString("VerbForm=Fin|Mood=Imp");
      } else {
        // C19: a base form after a plural noun subject ("Only kings and queens wear crowns."): the plural verb
        for (int i = 1; i + 1 < n && fix < 0; ++i) {
          const std::string& w = tk[(size_t)i].lower;
          const std::string& pw = tk[(size_t)i - 1].lower;
          if (tk[(size_t)i].upos == "NOUN" && w.back() != 's' && tk[(size_t)i - 1].upos == "NOUN" && pw.size() > 3 &&
              i >= 3 && tk[(size_t)i - 2].upos == "CCONJ" &&   // "kings and queens wear": a coordinated plural subject
              pw.back() == 's' && verbReading(tk[(size_t)i]) && i + 1 < n &&
              in(tk[(size_t)i + 1].upos, {"NOUN", "DET", "ADJ", "PRON", "ADP", "ADV", "PUNCT"})) {
            fix = i;
            feats = nlp::morph::fromString("Number=Plur|Person=3|Tense=Pres|VerbForm=Fin|Mood=Ind");
          }
        }
        for (int i = 1; i + 1 < n && fix < 0; ++i) {
          const std::string& w = tk[(size_t)i].lower;
          // C26: not a word in -ss / -us / -is ("a silk dress", "the glass", "a circus"): no 3rd-person -s
          if ((tk[(size_t)i].upos == "NOUN" || tk[(size_t)i].upos == "ADJ") && w.size() > 3 && w.back() == 's' &&
              w[w.size() - 2] != 's' && w[w.size() - 2] != 'u' && w[w.size() - 2] != 'i' &&
              nounLike(i - 1) && verbReading(tk[(size_t)i]) &&
              !in(tk[(size_t)i - 1].lower, {"my", "your", "our", "their", "its", "his"})) {   // C22: "My ears!" 
            fix = i;
            feats = nlp::morph::fromString("Number=Sing|Person=3|Tense=Pres|VerbForm=Fin|Mood=Ind");
          }
        }
      }
    }
    if (fix >= 0) {
      for (int i : nounFix)
        if (i < fix) { tk[(size_t)i].upos = "NOUN"; tk[(size_t)i].feats = nlp::morph::fromString("Number=Sing"); }
      tk[(size_t)fix].upos = "VERB";
      tk[(size_t)fix].feats = feats;
      nlp_->parser().parse(tk);
      out.repairs.emplace_back("retag");
      flatClause(tk, fix);
    }
  }
  // Spanish (C13): tags and features from spanish.vpl (second-person verbs read as nouns: "¿A dónde vas?", "¿Quién
  // eres?"; tense / mood the tagger left out: "escribiremos", "entremos"), then a new parse
  if (nlp_ && lex_ && lang_ == SrcLang::Es && es::retag(tk, *lex_)) {
    nlp_->parser().parse(tk);
    out.repairs.emplace_back("retag");
  }
  bool reparse = nlp_ && lex_ && lang_ == SrcLang::En && lexiconVeto(tk, *lex_);
  // C15: "for" before a subject pronoun and its verb is the conjunction "for" (= nam): "for you will help ...",
  // ", for she does not know ..."
  if (nlp_ && lang_ == SrcLang::En)
    for (int i = 0; i + 2 < n; ++i) {
      if (tk[(size_t)i].lower != "for" || tk[(size_t)i].upos == "CCONJ") continue;
      if (!in(tk[(size_t)i + 1].lower, {"i", "you", "he", "she", "we", "they", "it", "there"})) continue;
      bool verbNext = false;
      for (int j = i + 2; j < n && j <= i + 3; ++j)
        verbNext = verbNext || tk[(size_t)j].upos == "AUX" || tk[(size_t)j].upos == "VERB";
      if (tk[(size_t)i + 1].lower == "you" && !(tk[(size_t)i + 2].upos == "AUX" || tk[(size_t)i + 2].upos == "VERB")) verbNext = false;
      if (!verbNext) continue;
      tk[(size_t)i].upos = "CCONJ";
      tk[(size_t)i].feats = 0;
      reparse = true;
    }
  // C15: "how am I to get back?" = how shall I get back: the "be" is a future auxiliary, "to" a marker
  if (nlp_ && lang_ == SrcLang::En)
    for (int i = 0; i + 3 < n; ++i)
      if (in(tk[(size_t)i].lower, {"am", "is", "are"}) && tk[(size_t)i + 1].upos == "PRON" && tk[(size_t)i + 2].lower == "to" &&
          (tk[(size_t)i + 3].upos == "VERB" || tk[(size_t)i + 3].upos == "AUX" || tk[(size_t)i + 3].upos == "NOUN")) {
        tk[(size_t)i].lower = "will";
        tk[(size_t)i].upos = "AUX";
        tk[(size_t)i].feats = 0;
        tk[(size_t)i + 2].upos = "PART";
        tk[(size_t)i + 3].upos = "VERB";
        tk[(size_t)i + 3].feats = nlp::morph::fromString("VerbForm=Inf");
        reparse = true;
      }
  // C15: "The woman who sings ...": after a relative pronoun an -s word the lexicon knows as a verb is the verb
  if (nlp_ && lex_ && lang_ == SrcLang::En)
    for (int i = 1; i + 1 < n; ++i) {
      nlp::Token& t = tk[(size_t)i];
      if (!in(tk[(size_t)i - 1].lower, {"who", "which", "that"}) || t.upos != "NOUN" || t.lower.size() < 4 ||
          t.lower.back() != 's')
        continue;
      std::vector<lex::Analysis> an;
      lex_->lookup(text::en_key(t.lower), an);
      bool verb = false;
      for (const lex::Analysis& a : an) verb = verb || lex_->lemma(a.lemma).pos == feat::Verb;
      if (!verb) continue;
      t.upos = "VERB";
      t.feats = nlp::morph::fromString("Number=Sing|Person=3|Tense=Pres|VerbForm=Fin|Mood=Ind");
      reparse = true;
    }
  // C15: lexicalised -ed adjectives the tagger reads as participles ("I have been wicked")
  if (nlp_ && lang_ == SrcLang::En)
    for (nlp::Token& t : tk)
      if (t.upos == "VERB" && in(t.lower, {"wicked", "naked", "crooked", "rugged", "ragged", "sacred", "wretched",
                                           "beloved", "aged", "learned", "blessed", "cursed", "jagged", "dogged"})) {
        t.upos = "ADJ";
        t.feats = 0;
        reparse = true;
      }
  if (reparse) nlp_->parser().parse(tk);
  // C17: a statement whose root is a noun while its one verb hangs under a noun ("The tired horses drank water.",
  // "The farmer's wife baked bread."): a simple clause made of nominal words around that verb is rebuilt by hand
  if (nlp_ && lex_ && lang_ == SrcLang::En && n >= 3) {
    int root = -1, verbAt = -1, verbs = 0;
    for (int i = 0; i < n; ++i) {
      if (tk[(size_t)i].head == 0) root = root < 0 ? i : -2;
      if (tk[(size_t)i].upos == "VERB" || tk[(size_t)i].upos == "AUX") { ++verbs; verbAt = i; }
    }
    const bool statement = tk[(size_t)n - 1].lower == "." || tk[(size_t)n - 1].lower == "!";
    if (root >= 0 && verbs == 1 && statement && in(tk[(size_t)root].upos, {"NOUN", "PROPN"}) &&
        tk[(size_t)verbAt].upos == "VERB" && fget(tk[(size_t)verbAt], nlp::morph::VerbFormShift) != nlp::morph::VfGer) {
      nlp::Token& v = tk[(size_t)verbAt];
      bool present = false;
      const std::string pastOf = en::verbOfForm(*lex_, v.lower, &present);
      const bool finite = fget(v, nlp::morph::VerbFormShift) == nlp::morph::VfFin || (!pastOf.empty() && !present);
      if (finite && flatClause(tk, verbAt)) {
        if (fget(v, nlp::morph::VerbFormShift) != nlp::morph::VfFin)
          v.feats = nlp::morph::fromString("Tense=Past|VerbForm=Fin|Mood=Ind");
        out.repairs.emplace_back("clause-repair");
      }
    }
  }
  if (nlp_ && lang_ == SrcLang::En) segmentParse(tk);
  if (nlp_) punctRoot(tk);
  if (nlp_) repairTree(out);
  // "Bow!": a one-word exclamation tagged as an interjection that the lexicon knows as a verb -> imperative
  if (nlp_ && lex_ && lang_ == SrcLang::En) {
    int words = 0, w0 = -1;
    for (int i = 0; i < n; ++i)
      if (tk[(size_t)i].upos != "PUNCT") { ++words; if (w0 < 0) w0 = i; }
    if (words == 1 && tk[(size_t)w0].upos == "INTJ" && sentence.find('!') != std::string_view::npos) {
      std::vector<lex::Analysis> an;
      lex_->lookup(text::en_key(tk[(size_t)w0].lower), an);
      bool verb = false;
      for (const lex::Analysis& a : an) verb = verb || lex_->lemma(a.lemma).pos == feat::Verb;
      const bool realIntj = in(tk[(size_t)w0].lower, {"oh", "ah", "hey", "wow", "alas", "ouch", "ow", "hello", "hi", "bye",
                                                      "yes", "no", "please", "ok", "okay", "well", "hooray", "hurrah",
                                                      "ugh", "eh", "goodbye", "thanks", "bravo", "hush", "boo"});
      if (verb && !realIntj) {
        tk[(size_t)w0].upos = "VERB";
        tk[(size_t)w0].feats = nlp::morph::fromString("VerbForm=Fin|Mood=Imp");
        nlp_->parser().parse(tk);
        out.repairs.emplace_back("retag");
      }
    }
  }
  // Spanish: a sentence-initial verb that carried enclitics ("Dámelo") is an imperative when the lexicon has one
  if (lang_ == SrcLang::Es && lex_) {
    int firstWord = 0;
    while (firstWord < n && (tk[(size_t)firstWord].upos == "PUNCT" || tk[(size_t)firstWord].text == "\xC2\xA1")) ++firstWord;
    if (firstWord + 1 < n && tk[(size_t)firstWord + 1].start == tk[(size_t)firstWord].start) {
      std::vector<lex::Analysis> an;
      lex_->lookup(text::es_key(tk[(size_t)firstWord].lower), an);
      if (an.empty()) lex_->lookup(text::es_bare(tk[(size_t)firstWord].lower), an);
      for (const lex::Analysis& a : an)
        if (feat::unpack(lex_->feature(a.feat)).mood == feat::Imperative) {
          const bool same = tk[(size_t)firstWord].upos == "VERB" &&
                            nlp::morph::get(tk[(size_t)firstWord].feats, nlp::morph::MoodShift) == nlp::morph::MoodImp;
          const feat::Features ff = feat::unpack(lex_->feature(a.feat));
          std::string ud = "VerbForm=Fin|Mood=Imp";
          if (ff.person) ud += "|Person=" + std::to_string((int)ff.person);
          if (ff.number) ud += ff.number == feat::Pl ? "|Number=Plur" : "|Number=Sing";
          tk[(size_t)firstWord].feats = nlp::morph::fromString(ud);
          tk[(size_t)firstWord].upos = "VERB";
          if (!same && nlp_) nlp_->parser().parse(tk);   // the imperative heads the clause (C13)
          break;
        }
    }
  }
  for (nlp::Token& t : tk) {
    if (t.lower.empty()) t.lower = nlp::normalise(t.text);
    if (!nlp_) { t.upos = "X"; t.head = 0; t.deprel = "root"; }
    t.lemma = lemmaOf(t);
    // C17: an adjective whose lemma is a verb is that verb's participle (VerbForm=Part, present for -ing)
    if (lex_ && lang_ == SrcLang::En && t.upos == "ADJ" && t.lemma != t.lower && fget(t, nlp::morph::VerbFormShift) == 0) {
      bool present = false;
      if (en::verbOfForm(*lex_, t.lower, &present) == t.lemma)
        t.feats = nlp::morph::fromString(present ? "Tense=Pres|VerbForm=Part" : "Tense=Past|VerbForm=Part");
    }
  }
  // C22: a title before a capitalised word ("Mr. Fox", "Miss Lucy", "Mrs. Goose") is not translated as a separate
  // name or a genitive ("Vulpēs Mr.", "Bear Dominī"): the title is dropped (as an article) and the word is the name;
  // a word english.vpl knows as a common noun is that noun ("Mr. Rabbit" -> cunīcule)
  if (lang_ == SrcLang::En && lex_)
    for (int i = 0; i + 1 < n; ++i) {
      nlp::Token& t = tk[(size_t)i];
      std::string tl = t.lower;
      if (!tl.empty() && tl.back() == '.') tl.pop_back();
      if (!in(tl, {"mr", "mrs", "ms", "miss", "mister", "missus"}) || t.text.empty() || !(t.text[0] >= 'A' && t.text[0] <= 'Z'))
        continue;
      nlp::Token& w = tk[(size_t)i + 1];
      if (w.text.empty() || !(w.text[0] >= 'A' && w.text[0] <= 'Z') || w.upos == "PUNCT") continue;
      // the word takes the title's place in the tree
      if (w.head == i + 1) { w.head = t.head; w.deprel = t.deprel; }
      for (int k = 0; k < n; ++k)
        if (k != i + 1 && tk[(size_t)k].head == i + 1) tk[(size_t)k].head = i + 2;
      if (w.head == i + 2) { w.head = 0; w.deprel = "root"; }
      t.head = i + 2;
      t.deprel = "det";
      t.upos = "DET";
      t.lemma = "the";
      t.lower = "the";
      const curated::NameEntry* ne = cd_.nameByEnglish(w.text);
      if (!ne) {
        std::vector<lex::Analysis> an;
        lex_->lookup(text::en_key(nlp::normalise(w.text)), an);
        bool noun = false;
        for (const lex::Analysis& a : an) noun = noun || lex_->lemma(a.lemma).pos == feat::Noun;
        if (noun) {
          w.upos = "NOUN";
          w.lower = nlp::normalise(w.text);
          w.lemma = lemmaOf(w);
          w.feats = nlp::morph::fromString("Number=Sing");
        }
      }
    }
  // C22: a clause-final "too" that modifies no adjective or adverb is "also" (quoque): "I want some cake too."
  if (lang_ == SrcLang::En && n >= 2) {
    int last = n - 1;
    while (last > 0 && tk[(size_t)last].upos == "PUNCT") --last;
    nlp::Token& t = tk[(size_t)last];
    const int hd = t.head - 1;
    if (t.lower == "too" && !(hd >= 0 && hd < n && in(tk[(size_t)hd].upos, {"ADJ", "ADV"}) && hd > last)) t.lemma = "also";
  }
  if (lang_ == SrcLang::Es && nlp_) es::normalise(out, lex_, cd_);   // C13: clitics, personal "a", "por qué" ...
  // "six o'clock": the clock word heads the numeral and stands for "hour" (RULE time.hour)
  for (int i = 1; i < n; ++i) {
    if (tk[(size_t)i].lower != "o'clock" || tk[(size_t)i - 1].upos != "NUM") continue;
    nlp::Token& num = tk[(size_t)i - 1];
    nlp::Token& clk = tk[(size_t)i];
    int newHead = num.head;
    std::string newRel = num.deprel;
    if (newHead == i + 1) { newHead = clk.head; newRel = clk.deprel; }
    if (newHead == i + 1 || newHead == i) { newHead = 0; newRel = "root"; }
    for (int k = 0; k < n; ++k)
      if (k != i && tk[(size_t)k].head == i) tk[(size_t)k].head = i + 1;
    clk.head = newHead;
    clk.deprel = newRel;
    clk.upos = "NOUN";
    clk.lemma = "hour";
    num.head = i + 1;
    num.deprel = "nummod";
  }
  // C24: free relatives with "what" get their tree from a small grammar (see fr::sentence)
  if (lang_ == SrcLang::En && nlp_) {
    bool q = false;
    for (const nlp::Token& t : tk) q = q || t.text == "?";
    if (!(q && !tk.empty() && tk[0].lower == "what") && fr::sentence(tk, false)) {
      fr::sentence(tk, true);
      out.doubt("free-relative");
    } else if (fr::nounRelative(tk, false)) {
      fr::nounRelative(tk, true);
    }
  }
  buildUnits(out);
}

}  // namespace vp::frame
