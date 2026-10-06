// FrameBuilder (DESIGN.md §10.1 items 2-6): tokens -> contractions -> tagger/parser -> lexicon lemmas -> phrasebook
// pre-pass -> units (phrase pieces and clause groups) -> SemFrame from the dependency tree.
// Closed classes are normalised to canonical English words here (determiners, prepositions, connectors, wh words),
// so the transfer stage is language neutral; content lemmas stay in the source language.
#include <algorithm>
#include <cstring>

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
                          {"whether", Relation::Complement, "whether"}, {"like", Relation::Manner, "as"}};
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
    if (in(w, {"and", "but", "or", "so", "then", "yet", "nor", "also", "therefore", "however"})) return w;
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
                                  "through", "along", "inside", "outside"};
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

int numberValue(const std::string& w) {
  static const std::pair<const char*, int> kNum[] = {
      {"one", 1},      {"two", 2},        {"three", 3},     {"four", 4},     {"five", 5},     {"six", 6},
      {"seven", 7},    {"eight", 8},      {"nine", 9},      {"ten", 10},     {"eleven", 11},  {"twelve", 12},
      {"twenty", 20},  {"thirty", 30},    {"hundred", 100}, {"thousand", 1000},
      {"uno", 1},      {"una", 1},        {"un", 1},        {"dos", 2},      {"tres", 3},     {"cuatro", 4},
      {"cinco", 5},    {"seis", 6},       {"siete", 7},     {"ocho", 8},     {"nueve", 9},    {"diez", 10},
      {"once", 11},    {"doce", 12},      {"veinte", 20},   {"cien", 100},   {"ciento", 100}, {"mil", 1000}};
  for (const auto& n : kNum)
    if (w == n.first) return n.second;
  bool digits = !w.empty();
  int v = 0;
  for (char c : w) {
    if (c < '0' || c > '9') { digits = false; break; }
    v = v * 10 + (c - '0');
    if (v > 100000) break;
  }
  return digits ? v : 0;
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
      for (const lex::Analysis& x : an) {
        const lex::Lemma l = lex_->lemma(x.lemma);
        const std::string h = text::lower(l.head);
        if (l.pos == feat::Verb && h.size() > 4 && h.compare(h.size() - 2, 2, "se") == 0) tryKnown = true;
      }
      bool phraseWord = false;
      for (const curated::PhraseEntry& e : cd_.phrasebookEs())
        if (e.pattern.find(' ') == std::string::npos && text::lower(e.pattern) == low) phraseWord = true;
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
          push(t, bestBase, cap);
          for (const std::string& c : bestCl) push(t, c, false);
        }
      }
    }
    if (!split) {
      out.push_back(t);
      out.back().lower = low;
    }
  }
}

// ---- lemmas ----------------------------------------------------------------------------------------------------------
std::string FrameBuilder::lemmaOf(const Token& t) const {
  const std::string rule = nlp::ruleLemma(lang_ == SrcLang::En ? nlp::Lang::En : nlp::Lang::Es, t);
  if (t.upos == "PROPN") return t.text;
  if (in(t.upos, {"PUNCT", "SYM", "ADP", "CCONJ", "SCONJ", "PART", "DET", "PRON", "INTJ"})) return t.lower;
  if (!lex_) return rule;
  std::vector<lex::Analysis> an;
  const std::string key = lang_ == SrcLang::En ? text::en_key(t.lower) : text::es_key(t.lower);
  lex_->lookup(key, an);
  if (an.empty() && lang_ == SrcLang::Es) lex_->lookup(text::es_bare(t.lower), an);
  if (an.empty()) return rule;
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
  for (const lex::Analysis& a : an) {
    const lex::Lemma l = lex_->lemma(a.lemma);
    if (l.id == lex::kNoLemma || !posOk(l.pos)) continue;
    if (agree(a) < bestAgree) continue;
    std::string h = text::lower(l.head);
    if (h.find(' ') != std::string::npos) continue;
    const std::string hk = lang_ == SrcLang::En ? text::en_key(h) : text::es_key(h);
    if (hk == key) { if (same.empty()) same = h; }
    else if (other.empty()) other = h;
    if (h == rule && ruleHit.empty()) ruleHit = h;
  }
  if (inflected) {
    if (!ruleHit.empty() && ruleHit != t.lower) return ruleHit;
    if (!other.empty()) return other;
    if (!same.empty()) return same;
  } else {
    if (!same.empty()) return same;
    if (!ruleHit.empty()) return ruleHit;
    if (!other.empty() && lang_ == SrcLang::Es) return other;
  }
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
        else if (in(w, {"esto", "este", "esta"})) w = "this";
        else if (in(w, {"eso", "ese", "esa", "aquello"})) w = "that";
        else if (in(w, {"quién", "quiénes"})) w = "who";
        else if (in(w, {"qué"})) w = "what";
        else if (in(w, {"cuál"})) w = "which";
        else if (in(w, {"que", "quien", "cual"})) w = "which";
      }
      if (w == "no-one" || w == "noone") w = "nobody";
      if (in(w, {"everyone", "everybody", "everything", "someone", "somebody", "something", "anyone", "anybody",
                 "anything", "nobody", "nothing", "none", "this", "that", "these", "those", "who", "whom", "what",
                 "which", "one", "ones", "all", "both", "each", "another", "other", "others", "many", "few",
                 "some", "any", "much"})) {
        np.isPronoun = true;
        np.pronLemma = w == "somebody" ? "someone" : w == "everybody" ? "everyone" : w == "anybody" ? "anyone"
                     : w == "whom" ? "who" : w;
        np.pron.person = 3;
        np.pron.number = in(w, {"these", "those", "ones", "all", "both", "others", "many", "few"}) ? 2 : 1;
        if (in(w, {"everyone", "all"})) np.pron.number = 2;
        np.number = np.pron.number;
        np.negative = in(w, {"nobody", "nothing", "none"});
        np.interrogative = in(w, {"who", "what", "which"}) && (c.question || fget(ht, nlp::morph::PronTypeShift) ==
                                                                                 nlp::morph::PtInt);
        if (np.interrogative) np.wh = np.pronLemma;
        pron = true;
      }
    }
  }
  if (!pron && en && (low == "one" || low == "ones") && ht.upos == "NOUN") {
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
    if (d == "det" || (d == "nmod" && kt.upos == "DET")) {
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
    if (d == "amod" || (d == "compound" && (kt.upos == "ADJ" || kt.upos == "NOUN"))) {
      bool adjLike = kt.upos == "ADJ";
      if (!adjLike && kt.upos == "NOUN" && lex_) {   // "white roses" tagged NOUN compound: adjective reading?
        std::vector<lex::Analysis> an;
        lex_->lookup(en ? text::en_key(kl) : text::es_key(kl), an);
        for (const lex::Analysis& a : an)
          if (lex_->lemma(a.lemma).pos == feat::Adj) { adjLike = true; break; }
      }
      if (adjLike) {
        SemAdj a;
        a.lemma = c.lem(k);
        if (kt.upos != "ADJ") a.lemma = kl;
        a.token = k;
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
      np.tokens.push_back(k);
      if (np.head == "hour" && low == "o'clock") np.ordinal = true;
      continue;
    }
    if (d == "flat" || (d == "compound" && kt.upos == "PROPN")) {   // multi-word names
      if (np.isName) np.head += " " + kt.text;
      np.tokens.push_back(k);
      continue;
    }
    if (d == "case") {
      if (kl == "'s" || kl == "'") np.tokens.push_back(k);
      continue;   // the caller reads the preposition
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
      np.relative.push_back(rf);
      np.tokens.insert(np.tokens.end(), rf.tokens.begin(), rf.tokens.end());
      continue;
    }
    if (d == "conj") {
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
        c.liftedAdv->push_back(a);
      }
      c.take(k);
      continue;
    }
    if (d == "punct") continue;
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
  const int first = sub.empty() ? h : sub.front();

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
    else if ((d == "nsubj" || d == "csubj") && nsubj < 0) nsubj = k;
    else if (d == "obj" && obj < 0) obj = k;
    else if (d == "iobj" && iobj < 0) iobj = k;
    else if (d == "expl") expl = k;
    else if (d == "mark") marks.push_back(k);
    else if (d == "xcomp" && xcomp < 0) xcomp = k;
  }
  const std::string hu = ht.upos;
  const std::string hl = c.lem(h);

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
      if (in(hl, {"do", "does", "did", "hacer"})) f.pred.ellipsis = true;
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
        if (fget(c.t(a), nlp::morph::MoodShift) == nlp::morph::MoodImp) f.type = Kind::Imp;
      } else if (alem == "have") {
        haveAux = true;
        f.pred.aspect = Aspect::Perfect;
        if (fget(c.t(a), nlp::morph::TenseShift) == nlp::morph::TensePast || al == "had") f.pred.tense = Tense::Past;
      } else if (alem == "be") {
        beAux = true;
        if (fget(c.t(a), nlp::morph::TenseShift) == nlp::morph::TensePast || al == "was" || al == "were")
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
    if (beAux && vf == nlp::morph::VfPart && tt == nlp::morph::TensePast) f.pred.voice = Voice::Passive;
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
    if (en && hl == "go" && f.pred.aspect == Aspect::Progressive) {   // "going to rain"
      f.pred.tense = Tense::Future;
      f.pred.aspect = Aspect::Simple;
      f.pred.lemma = xl;
      f.pred.token = xcomp;
      c.drop(h, Drop::Aux);
    } else if (!en && hl == "ir") {   // "voy a hacer" -> future (in the past: "iba a" -> past)
      if (f.pred.tense != Tense::Past) f.pred.tense = Tense::Future;
      f.pred.lemma = xl;
      f.pred.token = xcomp;
      c.drop(h, Drop::Aux);
    } else if (in(hl, {"want", "wish", "querer", "desear"})) {
      f.pred.modality = Modality::Want;
      f.pred.lemma = xl;
      f.pred.token = xcomp;
      c.drop(h, Drop::Aux);
    } else if (in(hl, {"have", "need", "tener", "necesitar"}) && f.pred.aspect != Aspect::Perfect) {
      f.pred.modality = Modality::Must;
      f.pred.lemma = xl;
      f.pred.token = xcomp;
      c.drop(h, Drop::Aux);
    } else if (hl == "let" || hl == "dejar") {
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
    } else if (in(hl, {"try", "begin", "start", "know", "like", "love", "learn", "forget", "hope", "decide", "seem",
                       "dare", "prefer", "continue", "stop", "saber", "intentar", "empezar", "comenzar"})) {
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
    if (hasCase && (hu == "NOUN" || hu == "PROPN") && cop >= 0) {
      std::string prep;
      for (int g : c.kids[(size_t)nsubj])
        if (c.ok(g) && c.dep(g) == "case") prep = prep.empty() ? c.t(g).lower : prep + " " + c.t(g).lower;
      SemOblique o;
      o.prep = canonPrep(lang_, prep);
      o.np = s;
      o.token = nsubj;
      o.front = nsubj < h;
      f.obliques.push_back(o);
      SemNP subj;
      buildNP(c, h, subj);
      f.hasSubject = true;
      f.subject = subj;
      f.existential = false;
      f.copula = false;
      f.pred.lemma = "be";
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
  if (f.copula && !f.existential) {
    if (adjHead || (hu == "VERB" && fget(ht, nlp::morph::VerbFormShift) == nlp::morph::VfPart)) {
      SemAdj a;
      a.lemma = hu == "ADJ" ? hl : c.t(h).lower;
      a.token = h;
      for (int k : c.kids[(size_t)h])
        if (c.ok(k) && c.dep(k) == "advmod" && in(c.t(k).lower, {"very", "too", "so", "quite", "rather", "really",
                                                                  "extremely", "muy", "demasiado", "tan", "bastante"})) {
          a.adverbs.push_back(c.t(k).lower);
          a.advTokens.push_back(k);
          c.take(k);
        }
      f.predAdj.push_back(a);
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
  } else if (!f.hasPred) {
    // fragment / exclamation / vocative
    f.type = Kind::Frag;
    if (hu == "NOUN" || hu == "PROPN" || hu == "PRON" || hu == "NUM") {
      SemNP p;
      buildNP(c, h, p);
      if (p.determiner == "what" && !c.question) {   // "What a strange garden!"
        f.type = Kind::Excl;
        f.exclQuam = true;
      }
      f.hasSubject = true;
      f.subject = p;
    } else if (hu == "ADJ") {
      SemAdj a;
      a.lemma = hl;
      a.token = h;
      for (int k : c.kids[(size_t)h])
        if (c.ok(k) && c.dep(k) == "advmod") {
          if (in(c.t(k).lower, {"how", "qué"}) && !c.question) { f.type = Kind::Excl; f.exclQuam = true; c.drop(k, Drop::Marker); }
          else { a.adverbs.push_back(c.t(k).lower); a.advTokens.push_back(k); }
        }
      f.predAdj.push_back(a);
    } else if (hu == "ADV" || hu == "ADP") {
      SemAdverb a;
      a.lemma = hl;
      a.token = h;
      if (hu == "ADP" && hl == "off") {}   // handled by the phrasebook ("off with ...")
      f.adverbs.push_back(a);
    } else if (hu == "INTJ") {
      f.interjections.push_back(c.t(h).lower);
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
    a.front = k == first && k < h;
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
    if (d == "obj" && k == obj) {
      SemNP o;
      buildNP(c, k, o);
      f.hasObject = true;
      f.object = o;
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
      if (o.np.isPronoun && o.np.interrogative && f.type != Kind::Wh && c.question && k == first) {
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
      // "then" / "so" at the start of the clause: connector
      if (k == first && in(kl, {"then", "so", "entonces", "luego", "also"})) {
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
    if (d == "conj") {
      const std::string ku = kt.upos;
      if (f.copula && ku == "ADJ") continue;   // handled with the predicate
      // coordinated clause (same subject unless it has its own)
      SemSub s;
      s.relation = Relation::Coord;
      for (int g : c.kids[(size_t)k])
        if (c.ok(g) && c.dep(g) == "cc") { s.marker = canonConnector(lang_, c.t(g).lower); c.drop(g, Drop::Marker); }
      if (s.marker.empty()) s.marker = "and";
      SemFrame cf;
      buildClause(c, k, cf);
      if (f.type == Kind::Imp && cf.type == Kind::Decl && !cf.hasSubject) cf.type = Kind::Imp;
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
          c.drop(g, Drop::Marker);
        }
      }
      if (!found) {
        if (d == "ccomp" || d == "csubj") { s.relation = Relation::Complement; marker = "that"; }
        else if (d == "xcomp") { s.relation = Relation::Purpose; marker = "to"; }
        else if (d == "parataxis") { s.relation = Relation::Coord; marker = ""; }
        else { s.relation = Relation::Time; marker = "when"; }
      }
      s.marker = marker;
      s.before = k < h;
      SemFrame sf;
      buildClause(c, k, sf);
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
        f.subject.pronLemma != "all") {
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
  // inversion after a fronted oblique ("In the garden lived a cat"): the post-verbal object is the subject
  if (en && !f.hasSubject && f.hasObject && f.hasPred && !f.copula && f.type == Kind::Decl && !f.obliques.empty() &&
      f.obliques[0].token < h && f.object.token > h && f.pred.voice == Voice::Active) {
    f.hasSubject = true;
    f.subject = f.object;
    f.hasObject = false;
    f.object = SemNP{};
    f.obliques[0].front = true;
  }

  // ---- clause type -----------------------------------------------------------------------------------------------------
  // questions: "?" or aux/copula before the subject
  if (f.type != Kind::Wh && f.type != Kind::Excl && f.hasPred) {
    const bool inverted = nsubj >= 0 && ((cop >= 0 && cop < nsubj) || (!auxes.empty() && auxes.front() < nsubj));
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
  // imperative: base verb at the start with no subject (or Mood=Imp), not a question
  if (f.hasPred && f.type == Kind::Decl && !c.question) {
    const Token& vt = c.t(f.pred.token >= 0 ? f.pred.token : h);
    const uint32_t md = fget(vt, nlp::morph::MoodShift), tt = fget(vt, nlp::morph::TenseShift);
    // first content token of the clause (skipping connectors, discourse, interjections, "then", "please")
    int lead = -1;
    for (int i : f.tokens) {
      const Token& x = c.t(i);
      if (isPunctTok(x) || c.consumed[(size_t)i]) continue;
      const std::string xd = x.deprel;
      if (xd == "cc" || xd == "discourse" || xd == "vocative" || xd == "intj" ||
          (xd == "advmod" && in(x.lower, {"then", "now", "so", "just", "please", "first", "also"})) ||
          (xd == "aux" && in(c.lem(i), {"do"})) || in(x.lower, {"not", "never", "please"}))
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
    if (en && topLevel && ((lead == vtok || (lead >= 0 && cop >= 0 && lead == cop)) && (lexImp || bare || !f.hasSubject) &&
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
    } else if (!en && (lexImp || ((std::find(f.discourse.begin(), f.discourse.end(), "please") != f.discourse.end() ||
                                    c.s.text.find("\xC2\xA1") != std::string::npos) &&
                                   !f.hasSubject && lead == vtok && fget(vt, nlp::morph::PersonShift) != 1))) {
      f.type = Kind::Imp;
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
  if (f.type == Kind::Imp && f.hasSubject && f.subject.isPronoun && f.subject.pron.number == 2) f.imperativePlural = true;
  if (!f.hasSubject && f.hasPred && !en && f.type != Kind::Imp) {
    // the finite verb: an auxiliary, the clause head (a modal or catenative verb), or the predicate
    std::vector<int> cands = auxes;
    cands.push_back(h);
    if (f.pred.token >= 0) cands.push_back(f.pred.token);
    int fin = -1;
    for (int a : cands)
      if (fget(c.t(a), nlp::morph::PersonShift)) { fin = a; break; }
    if (fin < 0)
      for (int a : cands)
        if (fget(c.t(a), nlp::morph::VerbFormShift) == nlp::morph::VfFin) { fin = a; break; }
    if (fin < 0) fin = h;
    uint32_t pe = fget(c.t(fin), nlp::morph::PersonShift);
    uint32_t nu = fget(c.t(fin), nlp::morph::NumberShift);
    if (!pe && lex_) {   // the tagger left the person out ("Puedes"): the lexicon's analyses, when they agree
      std::vector<lex::Analysis> an;
      lex_->lookup(text::es_key(c.t(fin).lower), an);
      uint8_t p0 = 0, n0 = 0;
      bool one = true;
      for (const lex::Analysis& a : an) {
        if (lex_->lemma(a.lemma).pos != feat::Verb) continue;
        const feat::Features ff = feat::unpack(lex_->feature(a.feat));
        if (!ff.person || ff.mood == feat::Imperative) continue;
        if (!p0) { p0 = ff.person; n0 = ff.number; }
        else if (ff.person != p0 || ff.number != n0) one = false;
      }
      if (p0 && one) { pe = p0; nu = n0 == feat::Pl ? nlp::morph::NumPlur : nlp::morph::NumSing; }
    }
    if (pe) {
      f.hasSubject = true;
      f.implicitSubject = true;
      f.subject.isPronoun = true;
      f.subject.pron.person = (uint8_t)pe;
      f.subject.pron.number = nu == nlp::morph::NumPlur ? 2 : 1;
      f.subject.number = f.subject.pron.number;
    }
  }
  if (f.type == Kind::Wh && f.pred.deliberative && f.hasSubject && f.subject.isPronoun && f.subject.pron.person == 1)
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
    for (int i = 0; i < n;) {
      if (isPunctTok(s.tokens[(size_t)i])) { ++i; continue; }
      PhraseMatch m;
      if (!book_.match(s.tokens, i, m)) { ++i; continue; }
      const curated::PhraseEntry& e = (lang_ == SrcLang::En ? cd_.phrasebook() : cd_.phrasebookEs())[(size_t)m.entry];
      bool leftOk = m.first == 0 || isPunctTok(s.tokens[(size_t)m.first - 1]) ||
                    s.tokens[(size_t)m.first - 1].upos == "CCONJ";
      if (!leftOk) {
        bool allT = true;
        for (int k = 0; k < m.first; ++k)
          if (!isPunctTok(s.tokens[(size_t)k]) && !transparent(k)) allT = false;
        leftOk = allT;
      }
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
      const bool prefixOk = e.reg == "adv" || e.reg == "narr" ||
                            ((topDep == "advmod" || topDep == "discourse") &&
                             (e.reg == "answer" || e.reg == "excl" || e.reg == "polite"));
      if (leftOk && (rightOk || prefixOk)) {
        m.pattern = e.pattern;
        m.latin = e.latin;
        m.reg = e.reg;
        m.note = e.note;
        m.tier = e.tier;
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
  // transparent connectors before a phrase ("Then go away."): attach to the phrase
  for (size_t ui = 0; ui + 1 < s.units.size(); ++ui) {
    Unit& u = s.units[ui];
    Unit& nx = s.units[ui + 1];
    if (u.type == Unit::Clause && nx.type == Unit::Phrase && u.frame.type == Kind::Frag && !u.frame.hasSubject &&
        u.frame.predAdj.empty() && u.frame.adverbs.size() + u.frame.connectors.size() == 1) {
      std::string w = !u.frame.connectors.empty() ? u.frame.connectors[0] : u.frame.adverbs[0].lemma;
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
    bool subj = false, verbal = false, cc = false;
    for (int j = 0; j < n; ++j) {
      if (tk[(size_t)j].head != i + 1) continue;
      const std::string& r = tk[(size_t)j].deprel;
      subj = subj || r == "nsubj";
      verbal = verbal || r == "cop" || r == "aux";
      cc = cc || r == "cc";
    }
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
    for (int k = u.first; k <= u.last && k < (int)s.tokens.size(); ++k)
      if (s.tokens[(size_t)k].upos == "VERB" && s.drop[(size_t)k] == Drop::No) verb = true;
    if (verb && (!u.frame.hasPred || u.frame.type == Kind::Frag)) return true;
  }
  return false;
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

void FrameBuilder::analyse(std::string_view sentence, SemSentence& out) const {
  out.clear();
  out.lang = lang_;
  out.text = std::string(sentence);
  tokenize(sentence, out.tokens);
  out.drop.assign(out.tokens.size(), Drop::No);
  if (out.tokens.empty()) return;
  if (nlp_) nlp_->analyse(out.tokens);
  std::vector<nlp::Token>& tk = out.tokens;
  const int n = (int)tk.size();
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
      if ((tk[1].upos == "DET" || tk[1].upos == "PRON") && verbReading(tk[0])) {
        fix = 0;
        feats = nlp::morph::fromString("VerbForm=Fin|Mood=Imp");
      } else {
        for (int i = 1; i + 1 < n && fix < 0; ++i) {
          const std::string& w = tk[(size_t)i].lower;
          const std::string& pu = tk[(size_t)i - 1].upos;
          if ((tk[(size_t)i].upos == "NOUN" || tk[(size_t)i].upos == "ADJ") && w.size() > 3 && w.back() == 's' &&
              (pu == "NOUN" || pu == "PROPN" || pu == "PRON") && verbReading(tk[(size_t)i])) {
            fix = i;
            feats = nlp::morph::fromString("Number=Sing|Person=3|Tense=Pres|VerbForm=Fin|Mood=Ind");
          }
        }
      }
    }
    if (fix >= 0) {
      tk[(size_t)fix].upos = "VERB";
      tk[(size_t)fix].feats = feats;
      nlp_->parser().parse(tk);
      out.repairs.emplace_back("retag");
    }
  }
  // Spanish (C13): tags and features from spanish.vpl (second-person verbs read as nouns: "¿A dónde vas?", "¿Quién
  // eres?"; tense / mood the tagger left out: "escribiremos", "entremos"), then a new parse
  if (nlp_ && lex_ && lang_ == SrcLang::Es && es::retag(tk, *lex_)) {
    nlp_->parser().parse(tk);
    out.repairs.emplace_back("retag");
  }
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
  buildUnits(out);
}

}  // namespace vp::frame
