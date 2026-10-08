// GreekTransfer: SemFrame -> GrcClause (see vp/transfer_grc.h). Mirrors the Latin transfer (src/transfer) with the
// Greek closed classes, article policy, particles, tense / aspect mapping and the lexical rules of lexical_en_grc.tsv.
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "frame/english.h"
#include "transfer/tables.h"
#include "vp/morph_grc.h"
#include "vp/text.h"
#include "vp/transfer_grc.h"

namespace vp::grc {

using namespace vp::feat;
using frame::Kind;
using frame::Modality;
using frame::Relation;
using frame::SemFrame;
using frame::SemNP;
using frame::SemSentence;
using transfer::Choice;

namespace {

bool posCompatible(uint8_t gp, uint8_t want, uint16_t flags) {
  switch (want) {
    case Noun: return gp == Noun;
    case Name: return gp == Name || gp == Noun;
    case Verb: return gp == Verb;
    case Adj: return gp == Adj || gp == Num || (gp == Participle && (flags & lex::HasTable));
    case Adv: return gp == Adv || (gp == Particle && !(flags & lex::HasTable)) || (gp == Participle && !(flags & lex::HasTable));
    case Intj: return gp == Intj;
    default: return gp == want;
  }
}

std::vector<std::string> words(const std::string& s) {
  std::vector<std::string> out;
  size_t a = 0;
  while (a < s.size()) {
    while (a < s.size() && s[a] == ' ') ++a;
    size_t b = a;
    while (b < s.size() && s[b] != ' ') ++b;
    if (b > a) out.push_back(s.substr(a, b - a));
    a = b;
  }
  return out;
}

uint8_t simpleGender(uint8_t g) {
  switch (g) {
    case M: case F: case N: return g;
    case FN: return F;
    default: return M;
  }
}

bool in(const std::string& w, std::initializer_list<const char*> l) {
  for (const char* x : l)
    if (w == x) return true;
  return false;
}

// English adverb -> Greek headword (motion variant second); nullptr when not listed.
const char* adverbTable(const std::string& a, bool motion) {
  struct R { const char* en; const char* rest; const char* motion; };
  static const R kRows[] = {
      {"here", "ἐνθάδε", "δεῦρο"},   {"there", "ἐκεῖ", "ἐκεῖσε"},   {"now", "νῦν", nullptr},
      {"always", "ἀεί", nullptr},     {"never", "οὐδέποτε", nullptr}, {"before", "πρότερον", nullptr},
      {"again", "αὖθις", nullptr},    {"today", "σήμερον", nullptr},  {"tomorrow", "αὔριον", nullptr},  {"tonight", "νύκτωρ", nullptr},
      {"yesterday", "χθές", nullptr}, {"very", "πάνυ", nullptr},      {"too", "λίαν", nullptr},
      {"so", "οὕτως", nullptr},       {"also", "καί", nullptr},       {"only", "μόνον", nullptr},
      {"well", "εὖ", nullptr},        {"badly", "κακῶς", nullptr},    {"quickly", "ταχέως", nullptr},
      {"fast", "ταχέως", nullptr},    {"slowly", "βραδέως", nullptr}, {"together", "ὁμοῦ", nullptr},
      {"already", "ἤδη", nullptr},    {"still", "ἔτι", nullptr},      {"yet", "ἔτι", nullptr},
      {"soon", "αὐτίκα", nullptr},    {"much", "πολύ", nullptr},      {"really", "ἀληθῶς", nullptr},
      {"perhaps", "ἴσως", nullptr},   {"maybe", "ἴσως", nullptr},     {"once", "ποτέ", nullptr},
      {"ever", "ποτέ", nullptr},      {"home", "οἴκοι", "οἴκαδε"},    {"outside", "ἔξω", nullptr},
      {"inside", "ἔνδον", nullptr},   {"almost", "σχεδόν", nullptr},  {"first", "πρῶτον", nullptr},
      {"later", "ὕστερον", nullptr},  {"then", "τότε", nullptr},      {"late", "ὀψέ", nullptr},
      {"early", "πρωΐ", nullptr},      {"enough", "ἅλις", nullptr},    {"far", "πόρρω", nullptr},
      {"near", "ἐγγύς", nullptr},     {"sometimes", "ἐνίοτε", nullptr}, {"often", "πολλάκις", nullptr},
      {"immediately", "εὐθύς", nullptr}, {"even", "καί", nullptr},    {"how", "ὡς", nullptr},
      {"thus", "οὕτως", nullptr},     {"together", "ὁμοῦ", nullptr},  {"loudly", "μέγα", nullptr},
      {"gladly", "ἡδέως", nullptr},   {"happily", "ἡδέως", nullptr},   {"next", "ἔπειτα", nullptr},    {"below", "κάτω", nullptr},
  };
  for (const R& r : kRows)
    if (a == r.en) return motion && r.motion ? r.motion : r.rest;
  return nullptr;
}

bool dropAdverb(const std::string& a) {
  return in(a, {"just", "back", "away", "off", "up", "down", "out", "in", "over", "around", "not", "n't", "about",
                "through", "please", "on", "along", "by"});
}

// C16: sense head-word weighting (as the Latin transfer, C15). Where does `word` stand in the English sense gloss?
// 2 = head of the first gloss item ("house, dwelling" for house), 1 = head of a later item ("dwelling place,
// settlement, house"), -1 = only inside an item as a modifier, 0 = not in the gloss. The head of a noun / adjective
// item is its last word before a preposition or relative word; of a verb item, its first word after "to".
bool sameEnglish(const std::string& a, const std::string& b) {
  if (a == b) return true;
  auto stem = [](std::string x) {
    if (x.size() > 4 && x.compare(x.size() - 3, 3, "ies") == 0) return x.substr(0, x.size() - 3) + "y";
    if (x.size() > 3 && x.compare(x.size() - 2, 2, "es") == 0 && (x[x.size() - 3] == 's' || x[x.size() - 3] == 'x'))
      return x.substr(0, x.size() - 2);
    if (x.size() > 3 && x.back() == 's' && x[x.size() - 2] != 's') x.pop_back();
    return x;
  };
  return stem(a) == stem(b);
}
int glossHead(const std::string& gloss0, const std::string& word, uint8_t pos) {
  const std::string gloss = text::lower(gloss0);
  std::string g;
  int depth = 0;
  for (char ch : gloss) {
    if (ch == '(' || ch == '[') { ++depth; continue; }
    if (ch == ')' || ch == ']') { if (depth) --depth; continue; }
    if (!depth) g += ch;
  }
  bool seen = false;
  int item = 0;
  size_t a = 0;
  while (a <= g.size()) {
    size_t b = g.find_first_of(",;:", a);
    if (b == std::string::npos) b = g.size();
    const std::string it = g.substr(a, b - a);
    a = b + 1;
    std::vector<std::string> ws;
    std::string cur;
    for (char ch : it) {
      if ((ch >= 'a' && ch <= 'z') || ch == '-' || ch == '\'') cur += ch;
      else if (!cur.empty()) { ws.push_back(cur); cur.clear(); }
    }
    if (!cur.empty()) ws.push_back(cur);
    while (!ws.empty() && in(ws[0], {"to", "a", "an", "the", "one's", "be", "become"})) ws.erase(ws.begin());
    if (ws.empty()) { if (b >= g.size()) break; continue; }
    ++item;
    for (const std::string& w : ws) seen = seen || sameEnglish(w, word);
    std::string head;
    if (pos == Verb) head = ws[0];
    else {
      size_t end = ws.size();
      for (size_t i = 1; i < ws.size(); ++i)
        if (in(ws[i], {"of", "for", "in", "on", "with", "by", "to", "from", "at", "that", "which", "who", "used",
                       "paid", "made", "as", "into", "between", "having"})) { end = i; break; }
      head = ws[end - 1];
      if (end < ws.size() && (ws[end] == "or" || ws[end] == "and") && end + 1 < ws.size() && sameEnglish(ws[end + 1], word))
        head = ws[end + 1];
    }
    if (sameEnglish(head, word)) return item == 1 ? 2 : 1;
    if (b >= g.size()) break;
  }
  return seen && pos != Verb ? -1 : 0;
}

const char* cardinal(int v) {
  switch (v) {
    case 1: return "εἷς"; case 2: return "δύο"; case 3: return "τρεῖς"; case 4: return "τέτταρες";
    case 5: return "πέντε"; case 6: return "ἕξ"; case 7: return "ἑπτά"; case 8: return "ὀκτώ"; case 9: return "ἐννέα";
    case 10: return "δέκα"; case 12: return "δώδεκα"; case 20: return "εἴκοσι"; case 100: return "ἑκατόν";
    case 1000: return "χίλιοι";
    default: return nullptr;
  }
}
const char* ordinal(int v) {
  switch (v) {
    case 1: return "πρῶτος"; case 2: return "δεύτερος"; case 3: return "τρίτος"; case 4: return "τέταρτος";
    case 5: return "πέμπτος"; case 6: return "ἕκτος"; case 7: return "ἕβδομος"; case 8: return "ὄγδοος";
    case 9: return "ἔνατος"; case 10: return "δέκατος"; case 11: return "ἑνδέκατος"; case 12: return "δωδέκατος";
    default: return nullptr;
  }
}

// C18: an English -ing form or a Spanish gerund (-ando / -iendo / -yendo) as written
bool ingForm(const std::string& w) {
  auto ends = [&](const char* e) { const size_t n = std::strlen(e); return w.size() >= n + 1 && w.compare(w.size() - n, n, e) == 0; };
  if (w.size() < 5) return false;
  if (in(w, {"bring", "sing", "sting", "swing", "spring", "string", "fling", "cling", "wring", "thing", "king"})) return false;
  return ends("ing") || ends("ando") || ends("iendo") || ends("yendo");
}
const std::string& tokenLower(const SemSentence& s, int tok) {
  static const std::string kEmpty;
  return tok >= 0 && (size_t)tok < s.tokens.size() ? s.tokens[(size_t)tok].lower : kEmpty;
}
// C18: does the clause carry "so" / "such" on its predicate or an adverb ("so cold that", "so fast that")?
bool soDegree(const SemFrame& f) {
  auto isSo = [](const std::string& a) { return a == "so" || a == "such" || a == "tan" || a == "tanto" || a.rfind("so ", 0) == 0; };
  for (const frame::SemAdj& a : f.predAdj)
    for (const std::string& x : a.adverbs)
      if (isSo(text::lower(x))) return true;
  for (const frame::SemAdverb& a : f.adverbs)
    if (isSo(text::lower(a.lemma))) return true;
  if (f.hasObject && (f.object.determiner == "such" || f.object.determiner == "tanto")) return true;
  return false;
}
// C25: a degree word in the main clause before the result clause, read from the source tokens (the frame builder
// often drops it from the NP or the predicate: "such a cold night that", "such good friends that", "tan rápido que"):
// such / tanto ... anywhere, so / tan before an adjective, an adverb or much / many; only tokens of the main clause
// (f.tokens) before the first token of the dependent clause count
bool soDegreeTokens(const SemFrame& f, const SemFrame& sf, const SemSentence& s) {
  int first = sf.pred.token >= 0 ? sf.pred.token : (int)s.tokens.size();
  for (int t : sf.tokens) if (t >= 0) first = std::min(first, t);
  if (sf.hasSubject) for (int t : sf.subject.tokens) if (t >= 0) first = std::min(first, t);
  for (int t : f.tokens) {
    if (t < 0 || t >= first || (size_t)t >= s.tokens.size()) continue;
    const std::string& w = s.tokens[(size_t)t].lower;
    if (in(w, {"such", "tanto", "tanta", "tantos", "tantas"})) return true;
    if ((w == "so" || w == "tan") && (size_t)t + 1 < s.tokens.size()) {
      const nlp::Token& nx = s.tokens[(size_t)t + 1];
      if (nx.upos == "ADJ" || nx.upos == "ADV" || in(nx.lower, {"much", "many", "few", "little"})) return true;
    }
  }
  return false;
}
// C25: an irregular English past the tagger lemmatised wrongly or read as a present, repaired from english.vpl
// through C17's frame::en::verbOfForm (reused, not copied): (1) "The dog bit the boy." (lemma "bit"), "The wind blew."
// (lemma "blue"): the form is a listed past of another verb and greek.vpl has no verb for the tagger's lemma -> that
// verb, past (how = 1); (2) "The children swung on the gate." (swing, present): the form is only a past of the lemma
// (no present reading) -> past (how = 2). Only a finite verb without auxiliaries, modal or progressive.
bool pastFormRepair(const lex::Lexicon& grc, const lex::Lexicon& en, const SemFrame& f, const SemSentence& s,
                    SemFrame& out, int& how) {
  how = 0;
  if (!f.hasPred || f.copula || f.pred.token < 0 || (size_t)f.pred.token >= s.tokens.size()) return false;
  if (!f.pred.auxTokens.empty() || f.pred.modality != Modality::None || f.pred.aspect != frame::Aspect::Simple ||
      !f.pred.complementVerb.empty() || f.pred.tense == frame::Tense::Future || f.type == Kind::Imp)
    return false;
  const std::string w = s.tokens[(size_t)f.pred.token].lower;
  const std::string lemma = text::lower(f.pred.lemma);
  if (w.size() < 3 || w == lemma + "s" || (w.size() > 4 && w.compare(w.size() - 3, 3, "ing") == 0)) return false;
  const std::string v = frame::en::verbOfForm(en, w);
  if (v.empty() || v.find(' ') != std::string::npos) return false;
  if (v != lemma) {
    // the tagger's lemma must give no Greek verb at all (else it is a real reading: "left" of "leave" vs "left");
    // "lay" without an object is the past of "lie" ("the dog lay there"), whatever the Greek of "lay"
    const bool layLie = w == "lay" && lemma == "lay" && v == "lie" && !f.hasObject;
    std::vector<lex::Candidate> raw;
    if (!layLie) grc.reverse(text::en_key(lemma), raw);
    for (const lex::Candidate& k : raw)
      if (grc.lemma(k.lemma).pos == Verb) return false;
    out = f;
    out.pred.lemma = v;
    out.pred.tense = frame::Tense::Past;
    how = 1;
    return true;
  }
  if (f.pred.tense == frame::Tense::Past || w == lemma) return false;
  // the form has no present / base reading of this verb ("put", "read", "set" do)
  std::vector<lex::Analysis> an;
  en.lookup(text::en_key(w), an);
  for (const lex::Analysis& a : an) {
    const lex::Lemma l = en.lemma(a.lemma);
    if (l.pos != Verb || text::lower(std::string(l.head)) != v) continue;
    const Features af = unpack(en.feature(a.feat));
    if (af.tense != Perfect && af.tense != Pluperfect) return false;
  }
  out = f;
  out.pred.tense = frame::Tense::Past;
  how = 2;
  return true;
}
// C25: generic "one" as a subject ("One must not lie.", "One never knows.", "If one is hungry, ..."): the frame
// builder's "one-generic" (a modal clause) or a bare "one" that opens its clause (no determiner, adjective, "of"
// attribute or relative; nothing but a conjunction or punctuation before it): τις, not the numeral or οὗτος
bool genericOne(const SemNP& n, const SemSentence& s) {
  if (!n.isPronoun) return false;
  if (n.pronLemma == "one-generic") return true;
  const bool one = n.pronLemma == "one" || (n.pronLemma.empty() && text::lower(n.head) == "one");
  if (!one || !n.determiner.empty() || !n.adjectives.empty() || !n.genitive.empty() || !n.relative.empty() ||
      n.number == 2 || n.token < 0 || (size_t)n.token >= s.tokens.size() || s.tokens[(size_t)n.token].lower != "one")
    return false;
  if (n.token == 0) return true;
  const nlp::Token& pv = s.tokens[(size_t)n.token - 1];
  return pv.upos == "PUNCT" || pv.upos == "SCONJ" || pv.upos == "CCONJ" ||
         in(pv.lower, {"if", "when", "that", "because", "so", "and", "but", "then", "where", "unless", "until", "before",
                       "after", "as", "while", "how", "why", "what"});
}
// C25: a Spanish clitic object of the subject's own person ("nos divertimos", "me lavo", "se divirtieron"): reflexive
bool reflexiveClitic(const SemFrame& f, bool es) {
  if (!f.hasObject || !f.object.isPronoun || f.object.pron.emphatic) return false;
  if (f.object.pron.reflexive || f.object.pronLemma == "se") return true;
  if (!es) return false;
  const std::string o = text::lower(f.object.pronLemma.empty() ? f.object.head : f.object.pronLemma);
  if (!in(o, {"me", "te", "nos", "os"})) return false;
  return f.hasSubject && f.subject.isPronoun && f.subject.pron.person && f.subject.pron.person == f.object.pron.person;
}
// C21: the person and number of a Spanish verb form when spanish.vpl gives exactly one (puse = 1 sg, tocas = 2 sg,
// comas = 2 sg); false when the form is unknown or ambiguous
bool esPersonNumber(const lex::Lexicon* es, const std::string& form, uint8_t& person, uint8_t& number) {
  if (!es || form.empty()) return false;
  std::vector<lex::Analysis> an;
  es->lookup(text::es_key(form), an);
  bool one = false;
  for (const lex::Analysis& a : an) {
    const Features af = unpack(es->feature(a.feat));
    if (af.pos != Verb || !af.person || !af.number) continue;
    if (!one) { person = af.person; number = af.number; one = true; }
    else if (af.person != person || af.number != number) return false;
  }
  return one;
}
bool personalPron(const SemNP& n) {
  return n.isPronoun && n.pron.person > 0 && (n.pronLemma.empty() || transfer::tables::personalPronoun(n.pronLemma));
}

}  // namespace

// ---- context ------------------------------------------------------------------------------------------------------
struct GreekTransfer::Ctx {
  const SemSentence& s;
  const transfer::Settings& st;
  transfer::Memory& mem;
  GrcClauseOut& out;
  std::vector<std::string> context;
  const SemFrame* frame = nullptr;
  bool motion = false, negative = false, question = false, existential = false;
  bool subjectRole = false;             // the NP being built is the clause subject (generic plural -> article)
  uint8_t subjPerson = 3, subjNumber = 1, subjGender = 0;
  uint32_t forcedVerb = kNone;          // state adjective -> verb
  uint8_t forcedVoice = 0;
  std::string objPrep;                  // verbprep frame obj: the PP with this preposition is the object
  std::string prepOverride, prepOverrideGreek;
  uint8_t prepOverrideCase = 0;
  int depth = 0;                        // C16: > 0 inside a subordinate clause or an indirect question
  uint8_t startGender = 0;              // C16: the last noun's gender before this clause ("red ones" after "roses")
  uint32_t verb = kNone;                // C18: the Greek verb of the clause being built (valency purp:inf)
  bool animateIt = false;               // C18: a dependent clause of a clause whose subject is a person or an animal
  std::vector<GrcClause> pendingInf;    // C18: bare infinitives of purpose found inside the object NP
  bool mannerLight = false;             // C25: a light row of frame "manner" chose the verb (adverbs by kind manner)
  bool forcedLight = false;             // C25: forcedVerb comes from a light row (an event: no state imperfect)
  Ctx(const SemSentence& ss, const transfer::Settings& t, transfer::Memory& m, GrcClauseOut& o)
      : s(ss), st(t), mem(m), out(o) {}
  void cover(int tok) { if (tok >= 0) out.covered.push_back(tok); }
  void cover(const std::vector<int>& v) { out.covered.insert(out.covered.end(), v.begin(), v.end()); }
  void table(uint32_t lemma, const std::string& src, int token, const std::string& note = "") {
    Choice ch;
    ch.token = token;
    ch.source = src;
    ch.lemma = lemma;
    ch.kind = "table";
    ch.note = note;
    out.choices.push_back(ch);
  }
};

GreekTransfer::GreekTransfer(const lex::Lexicon& grc, const curated::CuratedData& cd, const GreekData& gd,
                             const GreekTables& gt)
    : lx_(grc), cd_(cd), gd_(gd), gt_(gt) {}

uint32_t GreekTransfer::greek(const char* head, uint8_t pos) const {
  std::string key = std::string(head) + "#" + std::to_string((int)pos);
  auto it = std::lower_bound(cache_.begin(), cache_.end(), key,
                             [](const std::pair<std::string, uint32_t>& e, const std::string& k) { return e.first < k; });
  if (it != cache_.end() && it->first == key) return it->second;
  uint32_t id = findLemma(lx_, head, pos);
  if (id == kNone && pos) id = findLemma(lx_, head);
  if (cache_.size() < 1024) cache_.insert(it, {std::move(key), id});   // bounded: closed-class words only
  return id;
}

std::string GreekTransfer::english(const std::string& src, const transfer::Settings& st) const {
  if (st.lang != frame::SrcLang::Es || !st.srcLex) return text::lower(src);
  std::vector<lex::Analysis> an;
  st.srcLex->lookup(text::es_key(src), an);
  for (const lex::Analysis& a : an) {
    const lex::Lemma sl = st.srcLex->lemma(a.lemma);
    if (sl.id == kNone || sl.glossEn.empty()) continue;
    std::string g = text::lower(std::string(sl.glossEn));
    const size_t cut = g.find_first_of(";,(");
    if (cut != std::string::npos) g = g.substr(0, cut);
    while (!g.empty() && g.back() == ' ') g.pop_back();
    if (g.compare(0, 3, "to ") == 0) g = g.substr(3);
    if (!g.empty()) return g;
  }
  return std::string();
}

uint32_t GreekTransfer::adjAdverb(const char* form) const {
  morph::Token t;
  analyse(lx_, form, t);
  for (const lex::Analysis& a : t.analyses) {
    const lex::Lemma l = lx_.lemma(a.lemma);
    if (l.id == kNone || (l.pos != Adj && l.pos != Num)) continue;
    if (unpack(lx_.feature(a.feat)).pos == Adv) return a.lemma;
  }
  return kNone;
}

uint32_t GreekTransfer::lexRowLemma(const LexRow* r, uint8_t pos) const {
  if (!r || r->greek.empty() || r->greek == "-") return kNone;
  return greek(r->greek.c_str(), pos);
}

bool GreekTransfer::durative(const std::string& src, uint32_t greekLemma, const transfer::Settings& st) const {
  // the Greek verb decides when it is known (lexical_en_grc.tsv kind durative lists the Greek verb of each row):
  // "have" -> λαμβάνω is a single act (λαβέ), "wait" -> μένω continues (μένε)
  if (greekLemma != kNone) {
    const std::string k = std::string(lx_.lemma(greekLemma).key);
    for (const LexRow& r : gt_.rows())
      if (r.kind == "durative" && text::greek_key(r.greek) == k) return true;
    return false;
  }
  if (gt_.durative(text::lower(src))) return true;
  const std::string en = english(src, st);
  return !en.empty() && gt_.durative(en);
}

// ---- lexical selection ----------------------------------------------------------------------------------------------
uint32_t GreekTransfer::select(const std::string& sourceLemma, uint8_t pos, const std::vector<std::string>& context,
                               bool hasObject, bool personObject, const transfer::Settings& st, Choice& c,
                               uint8_t srcGender) const {
  c.source = sourceLemma;
  c.candidates.clear();
  c.lemma = kNone;
  c.kind = "sense";
  const bool es = st.lang == frame::SrcLang::Es;
  std::vector<lex::Candidate> raw;
  lx_.reverse(es ? "es:" + text::es_key(sourceLemma) : text::en_key(sourceLemma), raw);
  if (raw.empty() && es) lx_.reverse("es:" + text::es_bare(sourceLemma), raw);
  std::vector<const TaughtGloss*> taught;
  if (!es) gt_.taught(text::lower(sourceLemma), taught);
  gt_.taughtReadable(sourceLemma, es, taught);
  // English pivot for Spanish: the Spanish lexicon's one-line English gloss gives the teacher's glosses always and
  // the reverse-index candidates when the Spanish keyword has none
  std::string pivotVia;
  // C18: an English word neither the reverse index nor the teacher's glosses know: the base words of C17's derivation
  // (frame/english.h: "sea-shore" -> seashore / shore, "happily" -> happy), checked against greek.vpl's reverse index
  if (!es && raw.empty() && taught.empty()) {
    const char* upos = pos == Noun || pos == Name ? "NOUN" : pos == Verb ? "VERB" : pos == Adj ? "ADJ" : pos == Adv ? "ADV" : "";
    for (const std::string& b : frame::en::baseCandidates(text::lower(sourceLemma), upos)) {
      if (b == text::lower(sourceLemma)) continue;
      lx_.reverse(text::en_key(b), raw);
      gt_.taught(b, taught);
      gt_.taughtReadable(b, false, taught);
      if (!raw.empty() || !taught.empty()) { pivotVia = b; break; }
    }
  }
  if (es) {
    const std::string en = english(sourceLemma, st);
    if (!en.empty() && en.find(' ') == std::string::npos) {
      const size_t before = taught.size();
      gt_.taught(en, taught);
      if (raw.empty()) lx_.reverse(text::en_key(en), raw);
      if (!raw.empty() || taught.size() > before) pivotVia = en;
    }
  }
  auto isTaught = [&](const lex::Lemma& l) {
    for (const TaughtGloss* g : taught)
      if (g->key == l.key && (g->pos.empty() || g->pos == curated::CuratedData::tierPos(l.pos) ||
                              (g->pos == "particle" && l.pos == Particle)))
        return true;
    return false;
  };
  const double taughtBonus = st.fidelity >= 3 ? 1.0 : st.fidelity == 2 ? 0.5 : 0.1;
  struct Scored { uint32_t lemma; uint16_t sense; double score; std::string why; uint8_t tier; bool kwHit; double base; };
  std::vector<Scored> sc;
  std::vector<lex::Sense> senses;
  auto tierOf = [&](const lex::Lemma& l) -> uint8_t {
    if (const curated::TierEntry* te = cd_.tierGreek(l.key))
      if (te->tier) return te->tier;
    for (const TaughtGloss* g : taught)
      if (g->key == l.key && g->tier) return g->tier;
    return l.tier ? l.tier : 3;
  };
  auto tierTerm = [&](uint8_t tier, std::string& why) {
    double tt = 0;
    if (st.fidelity == 2) tt = -0.25 * std::max(0, tier - 2);
    else if (st.fidelity >= 3) tt = -0.5 * std::max(0, tier - 1);
    if (st.fidelity >= 2 && tier == 1) tt += 0.1;
    if (tt != 0) why += ", tier " + std::to_string(tier);
    return tt;
  };
  auto common = [&](const lex::Lemma& l, double& s, std::string& why) {
    // D12: words shared with Modern Greek (unchanged meaning) at fidelity 2/3
    if ((l.flags & lex::SharedEl) && st.fidelity >= 2) {
      s += st.fidelity >= 3 ? 0.1 : 0.05;
      why += ", shared with Modern Greek";
    }
    // a declinable word without an inflection table cannot be inflected reliably
    if ((l.pos == Noun || l.pos == Verb || l.pos == Adj) && !(l.flags & lex::HasTable) && !(l.flags & lex::Indeclinable)) {
      s -= 0.2;
      why += ", no table";
    }
    if (l.flags & lex::Defective) { s -= 0.1; why += ", defective"; }
    if (srcGender && l.pos == Noun && simpleGender(l.gender) == srcGender) { s += 0.15; why += ", gender"; }
    if (st.context)
      for (const rules::Correction& cr : st.context->corrections)
        if ((es ? text::es_key(cr.sourceKey) == text::es_key(sourceLemma)
                : text::en_key(cr.sourceKey) == text::en_key(sourceLemma)) &&
            text::greek_key(cr.target) == std::string(l.key)) {
          s += 1.0;
          why += ", correction";
        }
  };
  for (const lex::Candidate& k : raw) {
    const lex::Lemma l = lx_.lemma(k.lemma);
    if (l.id == kNone || !posCompatible(l.pos, pos, l.flags)) continue;
    if ((l.flags & lex::ProperName) && pos != Name) continue;
    if (l.head.find(' ') != std::string_view::npos || l.head.find('-') != std::string_view::npos) continue;
    bool dup = false;
    for (const Scored& x : sc) dup = dup || x.lemma == k.lemma;
    if (dup) continue;
    double s = k.score / 255.0;
    std::string why = "base " + std::to_string(k.score);
    const uint8_t tier = tierOf(l);
    s += tierTerm(tier, why);
    if (isTaught(l)) { s += taughtBonus; why += ", teacher gloss"; }
    common(l, s, why);
    bool kwHit = isTaught(l);
    senses.clear();
    lx_.senses(k.lemma, senses);
    if (k.sense < senses.size()) {
      const lex::Sense& se = senses[k.sense];
      // C16: head-word weighting: the source word as the head of the gloss outweighs the word as a modifier
      if (!es) {
        const int hw = glossHead(std::string(se.glossEn), text::lower(sourceLemma), pos == Name ? (uint8_t)Noun : pos);
        if (hw == 2) { s += 0.05; why += ", gloss head"; }
        else if (hw == -1 && !isTaught(l)) { s -= 0.3; why += ", gloss modifier only"; }
      }
      double ov = 0;
      for (const std::string& kw : words(std::string(se.keywords))) {
        if (kw == sourceLemma) kwHit = true;
        for (const std::string& cx : context)
          if (kw == cx && cx != sourceLemma) ov += 0.1;
      }
      if (es && k.score >= 51) kwHit = true;
      ov = std::min(ov, 0.2);
      if (ov > 0) { s += ov; why += ", sense overlap"; }
      if (pos == Verb) {
        const uint16_t tg = se.tags;
        if ((tg & 1u) && hasObject) { s += 0.1; why += ", transitive"; }
        if ((tg & 2u) && !hasObject) { s += 0.05; why += ", intransitive"; }
        if ((tg & (1u << 8)) && personObject) { s += 0.05; why += ", with-dat"; }
      }
    }
    sc.push_back(Scored{k.lemma, k.sense, s, why, tier, kwHit, k.score / 255.0});
  }
  // taught lemmas the reverse index does not list for this word: base 0.5
  for (const TaughtGloss* g : taught) {
    uint8_t lp = 0;
    for (uint8_t p : {Noun, Verb, Adj, Adv, Pron, Num, Prep, Conj, Intj, Det, Particle})
      if (g->pos == curated::CuratedData::tierPos(p)) lp = p;
    if (g->pos == "particle") lp = Particle;
    const bool substantive = pos == Noun && lp == Adj;
    if (!lp || (!posCompatible(lp, pos, lex::HasTable) && !substantive && !(pos == Adv && lp == Particle))) continue;
    uint32_t id = findLemma(lx_, g->head, lp);
    if (id == kNone) id = findLemma(lx_, g->head);
    if (id == kNone) continue;
    bool dup = false;
    for (const Scored& x : sc) dup = dup || x.lemma == id;
    if (dup) continue;
    const lex::Lemma l = lx_.lemma(id);
    std::string why = "teacher gloss (tiers_grc.tsv)";
    const uint8_t tier = tierOf(l);
    double s = 0.5 + taughtBonus + tierTerm(tier, why);
    common(l, s, why);
    if (substantive) { s -= 0.4; why += ", adjective as noun"; }   // C16: a taught noun ("the dark" -> σκότος) wins
    sc.push_back(Scored{id, 0, s, why, tier, true, 0.5});
  }
  if (!pivotVia.empty())
    for (Scored& x : sc) {
      x.score -= 0.05;
      x.why += (es ? ", via English \"" : ", derived from \"") + pivotVia + "\"";
    }
  std::stable_sort(sc.begin(), sc.end(), [](const Scored& a, const Scored& b) {
    if (a.score != b.score) return a.score > b.score;
    return a.lemma < b.lemma;
  });
  if (st.fidelity >= 2 && !sc.empty() && sc[0].tier >= 3 && sc[0].why.find("correction") == std::string::npos) {
    for (size_t i = 1; i < sc.size(); ++i)
      if (sc[i].tier <= 2 && sc[i].kwHit && sc[i].base >= 0.2 && sc[i].base >= 0.5 * sc[0].base) {
        Scored x = sc[i];
        x.why += ", tier preference";
        sc.erase(sc.begin() + (long)i);
        sc.insert(sc.begin(), x);
        break;
      }
  }
  for (size_t i = 0; i < sc.size() && i < 6; ++i)
    c.candidates.push_back(transfer::Candidate{sc[i].lemma, sc[i].sense, std::round(sc[i].score * 1000) / 1000, sc[i].why});
  if (sc.empty()) { c.unknown = true; c.kind = "unknown"; return kNone; }
  size_t pick = 0;
  for (const auto& ov : st.overrides)
    if (ov.first == c.token && ov.second >= 0 && (size_t)ov.second < sc.size()) pick = (size_t)ov.second;
  c.lemma = sc[pick].lemma;
  c.margin = sc.size() > 1 ? std::max(0.0, sc[0].score - sc[1].score) : 1.0;
  if (sc.size() > 1 && sc[0].why.find("tier preference") != std::string::npos) c.margin = std::max(c.margin, 0.15);
  if (sc.size() > 1 && sc[0].why.find("teacher gloss") != std::string::npos &&
      sc[1].why.find("teacher gloss") == std::string::npos)
    c.margin = std::max(c.margin, 0.15);   // the teacher's choice is not a coin toss
  if (sc[pick].tier >= 3)
    for (size_t i = 0; i < sc.size(); ++i)
      if (i != pick && sc[i].tier <= 2 && sc[i].kwHit && sc[i].base >= 0.2) c.lowTier = true;
  if (sc[pick].why.find("correction") != std::string::npos) { c.kind = "correction"; c.lowTier = false; }
  return c.lemma;
}

// ---- adverbs ---------------------------------------------------------------------------------------------------------
uint32_t GreekTransfer::adverb(const std::string& lemma0, int token, Ctx& c, bool motion, bool* front) const {
  const std::string lemma = c.st.lang == frame::SrcLang::Es ? transfer::tables::spanishAdverb(lemma0) : lemma0;
  if (front) *front = false;
  if (const char* g = adverbTable(lemma, motion)) {
    uint32_t id = greek(g, Adv);
    if (id == kNone) id = greek(g, Particle);
    if (id == kNone) id = adjAdverb(g);   // C16: πρῶτον, ἡδέως are adverb cells of πρῶτος, ἡδύς
    if (id != kNone) {
      c.table(id, lemma0, token);
      c.cover(token);
      return id;
    }
  }
  if (dropAdverb(lemma)) { c.cover(token); return kNone; }
  Choice ch;
  ch.token = token;
  uint32_t id = select(lemma0, Adv, {}, false, false, c.st, ch);
  if (id == kNone && lemma.size() > 4 && lemma.compare(lemma.size() - 2, 2, "ly") == 0) {
    Choice c2;
    c2.token = token;
    id = select(lemma.substr(0, lemma.size() - 2), Adv, {}, false, false, c.st, c2);
    if (id != kNone) ch = c2;
  }
  // C18: an English -ly adverb the reverse index does not list: the adjective it comes from ("happily" -> happy, C17's
  // base words), rendered by the realiser as that adjective's adverb cell (ἡδέως) when the table has one
  if (id == kNone && c.st.lang == frame::SrcLang::En && lemma.size() > 4 && lemma.compare(lemma.size() - 2, 2, "ly") == 0) {
    for (const std::string& b : frame::en::baseCandidates(lemma, "ADV")) {
      Choice c2;
      c2.token = token;
      const uint32_t aj = select(b, Adj, {}, false, false, c.st, c2);
      if (aj == kNone) continue;
      Features af;
      af.pos = Adv;
      std::string probe;
      if (generate(lx_, aj, af, probe) && !probe.empty()) {
        id = aj;
        ch = c2;
        ch.note = "the adverb of the adjective (" + b + ")";
        break;
      }
    }
  }
  ch.token = token;
  c.out.choices.push_back(ch);
  c.cover(token);
  if (id == kNone) c.out.unknownWords.push_back(lemma0);
  return id;
}

// ---- noun phrases ---------------------------------------------------------------------------------------------------
void GreekTransfer::npInto(const SemNP& n, Ctx& c, GrcNP& o) const {
  o = GrcNP{};
  c.cover(n.tokens);
  c.cover(n.token);
  o.number = n.number == 2 ? Pl : Sg;
  const bool subjectRole = c.subjectRole;
  c.subjectRole = false;
  if (n.literalUnknown) { o.literal = n.surface; c.out.unknownWords.push_back(n.surface); return; }
  if (n.isPronoun) {
    const std::string& p = n.pronLemma;
    const bool personal = transfer::tables::personalPronoun(p);
    if (personal || (n.pron.person > 0 && p.empty())) {
      o.isPronoun = true;
      o.pron.person = n.pron.person ? n.pron.person : 3;
      uint8_t num = n.pron.number == 2 ? Pl : n.pron.number == 1 ? Sg : 0;
      if (!num) {
        num = (o.pron.person == 2 && (c.mem.addresseePlural || c.mem.answerWe)) ? Pl : Sg;
        if (num == Pl && o.pron.person == 2) c.mem.addresseeGuess = true;
      }
      if (n.determiner == "all") num = Pl;
      o.pron.number = num;
      o.number = num;
      if (o.pron.person == 1) {
        char g = c.st.speakerGender;
        if (c.st.flipSpeakerGender) g = g == 'f' ? 'm' : 'f';
        o.pron.gender = g == 'f' ? F : M;
      } else if (o.pron.person == 3) {
        uint8_t g = n.pron.gender;
        if ((p == "it" || p == "ello" || p == "lo") && c.mem.lastGender && c.frame && !c.frame->copula &&
            c.frame->pred.lemma != "be")
          g = c.mem.lastGender;
        if ((p == "it" || p == "ello") && !g) g = N;
        o.pron.gender = g ? g : (uint8_t)M;
      } else {
        o.pron.gender = M;
      }
      o.gender = o.pron.gender;
      o.emphasis = n.pron.emphatic;
      if (n.determiner == "all") {   // "you all", "all of you": πάντες, the verb in the pronoun's person
        GrcNP q;
        q.head = greek("πᾶς", Adj);
        q.number = Pl;
        q.gender = M;
        o = q;
        c.table(q.head, "all", n.token);
      }
      return;
    }
    uint32_t id = kNone;
    uint8_t gender = 0, number = Sg;
    if (subjectRole && genericOne(n, c.s)) {   // C25: "One must not lie" -> τις (see genericOne)
      id = greek("τις", Pron);
      if (id != kNone) {
        o.head = id;
        o.number = Sg;
        o.gender = M;
        c.table(id, "one", n.token, "generic \"one\": τις");
        return;
      }
    }
    if (p == "everyone" || p == "everybody" || p == "all") { id = greek("πᾶς", Adj); number = Pl; gender = M; }
    else if (p == "everything") { id = greek("πᾶς", Adj); number = Pl; gender = N; }
    else if (p == "nobody" || p == "none" || p == "no one") { id = greek("οὐδείς", Pron); gender = M; }
    else if (p == "nothing") { id = greek("οὐδείς", Pron); gender = N; }
    else if (p == "someone" || p == "somebody" || p == "anyone" || p == "anybody") {
      if (c.negative) id = greek("οὐδείς", Pron);
      else id = greek("τις", Pron);
      gender = M;
    } else if (p == "something" || p == "anything") {
      id = c.negative ? greek("οὐδείς", Pron) : greek("τις", Pron);
      gender = N;
    } else if (p == "who") { id = greek("τίς", Pron); gender = M; }
    else if (p == "what") { id = greek("τίς", Pron); gender = N; }
    else if (p == "which") { id = greek("ὅς", Pron); gender = 0; }
    else if (p == "many") { id = greek("πολύς", Adj); number = Pl; gender = M; }
    else if (p == "few") { id = greek("ὀλίγος", Adj); number = Pl; gender = M; }
    else if (p == "both") { id = greek("ἀμφότερος", Adj); number = Pl; gender = M; }
    else if (p == "other" || p == "others" || p == "another") { id = greek("ἄλλος", Adj); number = p == "others" ? Pl : Sg; gender = M; }
    else if (p == "this" || p == "that" || p == "these" || p == "those" || p == "one" || p == "ones") {
      const bool anaphor = p == "one" || p == "ones";
      const std::string det = anaphor ? n.determiner : p;
      gender = N;
      if (anaphor && c.mem.lastGender) gender = c.mem.lastGender;
      // C16: "The queen wanted red ones": the noun before this clause (ῥόδα), not the clause's own subject
      if (anaphor && c.startGender) gender = c.startGender;
      number = n.number == 2 || p == "these" || p == "those" ? Pl : Sg;
      if (anaphor && !n.adjectives.empty() && det.empty()) {   // "red ones": the adjective is the head
        Choice ch;
        ch.token = n.adjectives[0].token;
        id = select(n.adjectives[0].lemma, Adj, c.context, false, false, c.st, ch);
        c.out.choices.push_back(ch);
        if (id == kNone) c.out.unknownWords.push_back(n.adjectives[0].lemma);
        o.head = id;
        o.number = number;
        o.gender = gender ? gender : (uint8_t)M;
        o.definite = n.definite;   // C16: "red ones" -> ἐρυθρά, "the red ones" -> τὰ ἐρυθρά
        return;
      }
      id = (det == "that" || det == "those") ? greek("ἐκεῖνος") : greek("οὗτος", Det);
    }
    if (id == kNone) {
      o.literal = n.surface;
      c.out.unknownWords.push_back(n.surface);
      return;
    }
    o.head = id;
    o.number = number;
    o.gender = gender ? gender : (uint8_t)M;
    c.table(id, p, n.token);
    return;
  }
  // ---- names ----
  if (n.isName) {
    const std::string low = text::lower(n.head);
    // C16: weekday names (lexical_en_grc.tsv kind weekday): the god's name in the genitive + ἡμέρα ("Ἄρεως ἡμέρα"),
    // or the ordinal counted from Sunday ("τρίτη ἡμέρα") for the alternative
    if (const LexRow* wd = gt_.find("weekday", low)) {
      const uint32_t day = greek("ἡμέρα", Noun);
      const uint32_t god = greek(wd->greek.c_str(), Name) != kNone ? greek(wd->greek.c_str(), Name)
                                                                   : greek(wd->greek.c_str());
      const char* ord = ordinal(std::atoi(wd->frame.c_str()));
      const uint32_t ordId = ord ? (greek(ord, Adj) != kNone ? greek(ord, Adj) : greek(ord)) : kNone;
      if (day != kNone && (weekdayOrdinal_ ? ordId != kNone : god != kNone)) {
        o.head = day;
        o.definite = false;
        if (weekdayOrdinal_) {
          GrcAdj a;
          a.lemma = ordId;
          o.adjectives.push_back(a);
        } else {
          GrcNP g;
          g.head = god;
          g.isName = true;
          g.number = Sg;
          o.genitive.push_back(g);
          o.genFirst = true;
        }
        c.table(day, n.head, n.token, weekdayOrdinal_ ? "weekday: ordinal" : "weekday: " + wd->greek);
        if (std::find(c.out.flags.begin(), c.out.flags.end(), "weekday") == c.out.flags.end())
          c.out.flags.push_back("weekday");
        c.mem.lastGender = F;
        return;
      }
    }
    const bool inTable = gd_.nameByEnglish(n.head) != nullptr;
    bool glossary = false;
    if (c.st.context)
      for (const rules::GlossaryEntry& g : c.st.context->glossary)
        if (text::en_key(g.name) == text::en_key(n.head)) glossary = true;
    if (!inTable && !glossary && (n.number == 2 || n.title)) {
      // a title word ("Queen of Hearts"): the common noun, with the article
      Choice ch;
      ch.token = n.token;
      std::string sg = low;
      if (n.number == 2 && sg.size() > 3 && sg.back() == 's') sg.pop_back();
      o.head = select(sg, Noun, c.context, false, false, c.st, ch);
      c.out.choices.push_back(ch);
      if (o.head != kNone) {
        o.definite = true;
        c.mem.lastGender = simpleGender(lx_.lemma(o.head).gender);
        for (const SemNP& g : n.genitive) {
          GrcNP x;
          npInto(g, c, x);
          x.definite = x.definite || g.isName || g.title;
          o.genitive.push_back(x);
          break;
        }
        return;
      }
    }
    if (!inTable && !glossary) {
      Choice ch;
      ch.token = n.token;
      const uint32_t id = select(low, Name, {}, false, false, c.st, ch);
      if (id != kNone && (lx_.lemma(id).flags & lex::ProperName) && !ch.candidates.empty() &&
          ch.candidates[0].score >= 0.5) {
        ch.kind = "name";
        c.out.choices.push_back(ch);
        o.head = id;
        o.isName = true;
        o.definite = true;
        return;
      }
    }
    o.isName = true;
    o.name = n.head;
    o.definite = true;   // decision 6: names take the article in narrative (not in the vocative or a predicate)
    if (n.number == 2) o.number = Pl;
    Choice ch;
    ch.token = n.token;
    ch.source = n.head;
    ch.kind = "name";
    c.out.choices.push_back(ch);
    if (const NameEntry* e = gd_.nameByEnglish(n.head)) c.mem.lastGender = simpleGender(e->gender);
    for (const SemNP& g : n.genitive) {
      GrcNP x;
      npInto(g, c, x);
      o.genitive.push_back(x);
      break;
    }
    return;
  }
  // C25: an English plural noun the frame left singular ("I bought apples today." -> obj=apple): english.vpl reads the
  // head token as a plural only
  if (c.st.lang == frame::SrcLang::En && c.st.srcLex && n.number != 2 && n.token >= 0 && (size_t)n.token < c.s.tokens.size() &&
      n.numeral.empty() && n.determiner.empty()) {
    const std::string& w = c.s.tokens[(size_t)n.token].lower;
    if (w != text::lower(n.head) && w.size() > 2 && w.back() == 's') {
      std::vector<lex::Analysis> an;
      c.st.srcLex->lookup(text::en_key(w), an);
      bool pl = false, other = false;
      for (const lex::Analysis& a : an) {   // the noun readings decide (the frame made it a noun head)
        if (c.st.srcLex->lemma(a.lemma).pos != Noun) continue;
        const Features af = unpack(c.st.srcLex->feature(a.feat));
        if (af.number == Pl) pl = true; else other = true;
      }
      if (pl && !other) o.number = Pl;
    }
  }
  // ---- common nouns ----
  bool substAdj = false;
  o.adjFirst = true;   // C16: attributive adjectives before an indefinite noun (εἰς βαθὺν βόθρον)
  {
    const std::string low = text::lower(n.head);
    Choice ch;
    ch.token = n.token;
    uint32_t id = kNone;
    const LexRow* realia = gt_.find("realia", low);
    if (!realia) { const std::string en = english(n.head, c.st); if (!en.empty()) realia = gt_.find("realia", en); }
    // C21 (lexical_en_grc.tsv kind noun): a fixed noun, used only when greek.vpl has the lemma (else the realia row)
    const LexRow* fixedNoun = gt_.find("noun", low);
    if (fixedNoun && lexRowLemma(fixedNoun, Noun) == kNone) fixedNoun = nullptr;
    if (fixedNoun) {
      id = lexRowLemma(fixedNoun, Noun);
      c.table(id, n.head, n.token, "lexical_en_grc.tsv: noun");
    }
    else if (low == "hour" && n.ordinal) { id = greek("ὥρα", Noun); c.table(id, n.head, n.token); }
    else if ((low == "man" || low == "woman" || low == "hombre" || low == "mujer") && n.adjectives.size() == 1 &&
             in(text::lower(n.adjectives[0].lemma), {"old", "viejo", "vieja", "anciano", "anciana"}) &&
             n.adjectives[0].adverbs.empty() &&
             (id = greek(low == "man" || low == "hombre" ? "γέρων" : "γραῦς", Noun)) != kNone) {
      // C18: "old man" -> γέρων, "old woman" -> γραῦς (one Greek noun); C25: "hombre viejo", "mujer anciana"
      substAdj = true;
      c.cover(n.adjectives[0].token);
      c.table(id, "old " + low, n.token, "one Greek noun");
    }
    else if ((low == "thing" || low == "cosa") && !n.adjectives.empty() && !n.adjectives[0].lemma.empty()) {
      // C16: "six impossible things" -> ἓξ ἀδύνατα: the adjective as a neuter noun
      ch.token = n.adjectives[0].token;
      id = select(n.adjectives[0].lemma, Adj, c.context, false, false, c.st, ch);
      c.out.choices.push_back(ch);
      c.cover(n.adjectives[0].token);
      if (id != kNone) { substAdj = true; o.gender = N; }
    }
    else if (realia && (id = lexRowLemma(realia, Noun)) != kNone) {
      ch.source = n.head;
      ch.lemma = id;
      ch.kind = "realia";
      ch.note = "no Attic word; equivalent used: " + realia->greek;
      c.out.choices.push_back(ch);
      if (std::find(c.out.flags.begin(), c.out.flags.end(), "realia") == c.out.flags.end()) c.out.flags.push_back("realia");
      c.out.notes.push_back(rules::Reason{-1, "sense", "\"" + n.head + "\": no Attic word; equivalent used (" +
                                                          realia->greek + ")", realia->note});
    } else {
      id = select(n.head, Noun, c.context, false, false, c.st, ch, transfer::animate(n) ? n.srcGender : 0);
      // C18: consistency in a batch (as C17's Latin rule): a noun already rendered in this cue or a cue before keeps
      // its Greek word when that word is one of the candidates here too ("box" stays κιβωτός); forced alternatives
      // bypass the memory
      if (id != kNone && ch.kind == "sense") {
        bool forced = false;
        for (const auto& ov : c.st.overrides) forced = forced || ov.first == n.token;
        const std::string key = low;
        if (!forced && ch.candidates.size() > 1)
          for (const auto& ns : c.mem.nounSense)
            if (ns.first == key && ns.second != id)
              for (const transfer::Candidate& k : ch.candidates)
                if (k.lemma == ns.second) {
                  id = ns.second;
                  ch.lemma = id;
                  ch.note = "the same Greek word as before for \"" + key + "\"";
                  break;
                }
        auto it = std::find_if(c.mem.nounSense.begin(), c.mem.nounSense.end(),
                               [&](const std::pair<std::string, uint32_t>& e) { return e.first == key; });
        if (it != c.mem.nounSense.end()) it->second = id;
        else {
          if (c.mem.nounSense.size() >= 32) c.mem.nounSense.erase(c.mem.nounSense.begin());   // bounded, oldest first
          c.mem.nounSense.emplace_back(key, id);
        }
      }
      if (id == kNone && !n.head.empty()) {   // substantive adjective ("the dark")
        Choice c2;
        c2.token = n.token;
        id = select(n.head, Adj, c.context, false, false, c.st, c2);
        if (id != kNone) { ch = c2; o.gender = N; }
      }
      c.out.choices.push_back(ch);
    }
    if (id == kNone) {
      if (!n.head.empty() || n.numeral.empty()) {
        o.literal = n.surface.empty() ? n.head : n.surface;
        c.out.unknownWords.push_back(o.literal);
      }
    } else {
      o.head = id;
      const lex::Lemma l = lx_.lemma(id);
      if (l.flags & lex::PluralOnly) o.number = Pl;
      if (l.pos == Adj) o.gender = N;
      // C18: a common-gender noun (ὁ / ἡ παῖς) takes the source noun's feminine ("la niña" -> ἡ παῖς)
      if (l.pos == Noun && (l.gender == MF || l.gender == MFN) && n.srcGender == F) o.gender = F;
      if (l.pos == Noun) {
        c.mem.lastGender = o.gender ? o.gender : simpleGender(l.gender);
        c.mem.lastNumber = o.number;
      }
    }
  }
  // ---- article policy (order.art; decisions 6 and 9 for names and predicates) ----
  o.definite = n.definite || !n.possessor.empty();
  // a generic plural subject takes the article in Greek ("Flowers can't talk" -> τὰ ἄνθη)
  if (subjectRole && !o.definite && n.number == 2 && n.determiner.empty() && n.numeral.empty() && o.head != kNone)
    o.definite = true;
  const std::string& d = n.determiner;
  auto quantAdj = [&](const char* head, const std::string& src) {
    GrcAdj a;
    a.lemma = greek(head, Adj);
    if (a.lemma == kNone) a.lemma = greek(head);
    if (a.lemma == kNone) return;
    o.adjectives.push_back(a);
    c.table(a.lemma, src, n.token);
  };
  auto oudeis = [&]() {   // "no song" / "not any songs": οὐδεμία ᾠδή (singular)
    GrcAdj a;
    a.lemma = greek("οὐδείς", Pron);
    if (a.lemma == kNone) a.lemma = greek("οὐδείς");
    if (a.lemma == kNone) return;
    o.adjectives.insert(o.adjectives.begin(), a);
    o.definite = false;
    if (o.head != kNone && !(lx_.lemma(o.head).flags & lex::PluralOnly)) o.number = Sg;
    c.table(a.lemma, d, n.token);
  };
  if (d == "this" || d == "these") o.dem = Demonstrative::Houtos;
  else if (d == "that" || d == "those") o.dem = Demonstrative::Ekeinos;
  else if (d == "no") { oudeis(); c.negative = true; }
  else if (d == "any") {
    if (c.negative && !c.existential) oudeis();
  } else if (d == "every" || d == "each") {
    o.quantifier = greek("πᾶς", Adj);
    o.definite = false;
  } else if (d == "all") {
    o.quantifier = greek("πᾶς", Adj);
    // C18: "all day" -> πᾶσαν τὴν ἡμέραν (the whole): a singular noun stays singular
    const bool massSg = n.number == 1 && o.head != kNone && lx_.lemma(o.head).pos == Noun &&
                        !(lx_.lemma(o.head).flags & lex::PluralOnly) && (transfer::tables::timeNoun(text::lower(n.head)) ||
                                                                        !transfer::animate(n));
    o.number = massSg ? (uint8_t)Sg : (uint8_t)Pl;
    o.definite = true;
  } else if (d == "many") { quantAdj("πολύς", d); o.number = Pl; }
  else if (d == "few") { quantAdj("ὀλίγος", d); o.number = Pl; }
  else if (d == "much") quantAdj("πολύς", d);
  else if (d == "another" || d == "other") quantAdj("ἄλλος", d);
  // C21: "such a big dog" -> οὕτω μέγαν κύνα (the degree word goes with the adjective, below); "such a dog" ->
  // τοιοῦτον κύνα; Spanish "tal" / "semejante" the same. The degree word is never dropped silently.
  bool suchAdj = false;
  // the frame builder sometimes leaves "such" out of the NP or makes it a genitive ("dogs of such"): found by its token
  int suchTok = -1;
  {
    int first = n.token;
    for (int t : n.tokens) if (t >= 0 && (first < 0 || t < first)) first = t;
    for (const frame::SemAdj& a : n.adjectives) if (a.token >= 0 && (first < 0 || a.token < first)) first = a.token;
    if (first > 0 && (size_t)first <= c.s.tokens.size() && c.s.tokens[(size_t)first - 1].lower == "such") suchTok = first - 1;
    for (const SemNP& g : n.genitive)
      if (text::lower(g.head) == "such") suchTok = g.token >= 0 ? g.token : suchTok >= 0 ? suchTok : -2;
  }
  if (d == "such" || d == "tal" || d == "semejante" || suchTok != -1) {
    int dt = suchTok >= 0 ? suchTok : -1;
    for (int t : n.tokens)
      if (t >= 0 && (size_t)t < c.s.tokens.size() && in(c.s.tokens[(size_t)t].lower, {"such", "tal", "semejante"})) dt = t;
    bool adjOk = false;
    for (const frame::SemAdj& a : n.adjectives) adjOk = adjOk || (!a.lemma.empty() && a.adverbs.empty());
    if (adjOk && !substAdj) {
      suchAdj = true;
    } else {
      const size_t before = o.adjectives.size();
      quantAdj("τοιοῦτος", d);
      if (o.adjectives.size() > before) c.cover(dt);
    }
    if (suchAdj) c.cover(dt);
  }
  if (o.dem != Demonstrative::None) o.definite = true;
  if (n.interrogative) {
    if (n.wh == "how many" || n.wh == "how much") o.interrogative = greek("πόσος", Adj);
    else if (n.wh == "what" || n.wh == "qué") o.interrogative = greek("τίς", Pron);   // C16: "τίς ἡμέρα"
    else o.interrogative = greek("ποῖος", Adj);
    if (o.interrogative == kNone) o.interrogative = greek("τίς", Pron);
    o.definite = false;
  }
  // C18: a possessive determiner right before the head that the frame builder left out ("Where is my book?" -> exist
  // subj=book): restored from the token ("ποῦ ἐστι τὸ βιβλίον μου;"); "her" is left alone (object pronoun or possessive)
  if (n.possessor.empty() && n.token > 0 && (size_t)n.token < c.s.tokens.size() &&
      std::find(n.tokens.begin(), n.tokens.end(), n.token - 1) == n.tokens.end()) {
    const std::string w = c.s.tokens[(size_t)n.token - 1].lower;
    uint8_t pp = 0, pn = 0, pg = 0;
    if (w == "my") { pp = 1; pn = Sg; }
    else if (w == "our") { pp = 1; pn = Pl; }
    else if (w == "your") { pp = 2; pn = c.mem.addresseePlural ? (uint8_t)Pl : (uint8_t)Sg; }
    else if (w == "his") { pp = 3; pn = Sg; pg = M; }
    else if (w == "its") { pp = 3; pn = Sg; pg = c.mem.lastGender ? c.mem.lastGender : (uint8_t)N; }
    else if (w == "their") { pp = 3; pn = Pl; pg = M; }
    if (pp && o.head != kNone && !o.isPronoun) {
      o.possPerson = pp;
      o.possNumber = pn;
      o.possGender = pg;
      o.definite = true;
      c.cover(n.token - 1);
    }
  }
  // possessor
  for (const SemNP& p : n.possessor) {
    if (p.isPronoun && p.pron.person > 0) {
      const uint8_t num = p.pron.number == 2 ? Pl : p.pron.number == 1 ? Sg : (c.mem.addresseePlural ? Pl : Sg);
      o.possPerson = p.pron.person;
      o.possNumber = num;
      o.possGender = p.pron.person == 3 ? (p.pron.gender ? simpleGender(p.pron.gender) : (uint8_t)M) : 0;
      if (p.pron.emphatic && p.pron.person <= 2) o.possEmphatic = true;
      c.cover(p.tokens);
      c.cover(p.token);
    } else {
      GrcNP g;
      npInto(p, c, g);
      g.definite = g.definite || g.head != kNone;
      o.genitive.push_back(g);
    }
    break;
  }
  // adjectives
  for (const frame::SemAdj& a : n.adjectives) {
    if (substAdj && &a == &n.adjectives[0]) continue;   // the head already
    Choice ch;
    ch.token = a.token;
    GrcAdj ga;
    ga.degree = a.degree;
    // C25 (lexical_en_grc.tsv kind ptc): a state adjective of a person or an animal used attributively is the present
    // participle of its Greek verb ("the hungry bird" -> ὁ πεινῶν ὄρνις; λιμηρός was chosen); frame mid / pass = voice
    bool anim = transfer::animate(n);
    if (!anim && c.st.lang == frame::SrcLang::Es) {   // the Spanish noun through its English gloss ("lobo" -> wolf)
      SemNP e = n;
      e.head = english(n.head, c.st);
      anim = !e.head.empty() && transfer::animate(e);
    }
    if (const LexRow* pr = a.adverbs.empty() && anim ? gt_.find("ptc", text::lower(a.lemma)) : nullptr) {
      const uint32_t v = lexRowLemma(pr, Verb);
      if (v != kNone) {
        ga.lemma = v;
        ga.participle = true;
        ga.voice = pr->frame == "mid" ? (uint8_t)Middle : pr->frame == "pass" ? (uint8_t)Passive : (uint8_t)Active;
        c.table(v, a.lemma, a.token, "attributive participle (lexical_en_grc.tsv: ptc)");
        c.cover(a.token);
        o.adjectives.push_back(ga);
        continue;
      }
    }
    ga.lemma = select(a.lemma, Adj, c.context, false, false, c.st, ch);
    c.out.choices.push_back(ch);
    c.cover(a.token);
    if (ga.lemma == kNone) { c.out.unknownWords.push_back(a.lemma); continue; }
    for (size_t i = 0; i < a.adverbs.size(); ++i) {
      const uint32_t av = adverb(a.adverbs[i], i < a.advTokens.size() ? a.advTokens[i] : -1, c, false);
      if (av != kNone) ga.adverbs.push_back(av);
    }
    // C21: "such a big X" -> οὕτω(ς) on the first adjective (sandhi: οὕτω before a consonant)
    if (suchAdj && ga.adverbs.empty()) {
      const uint32_t so = greek("οὕτως", Adv) != kNone ? greek("οὕτως", Adv) : greek("οὕτως");
      if (so != kNone) { ga.adverbs.push_back(so); c.table(so, "such", n.token, "\"such\" + adjective"); }
      suchAdj = false;
    }
    o.adjectives.push_back(ga);
  }
  // numerals
  if (!n.numeral.empty()) {
    const int v = n.numeralValue;
    if (n.ordinal) {
      if (const char* ord = ordinal(v)) {
        GrcAdj a;
        a.lemma = greek(ord, Adj);
        if (a.lemma != kNone) { o.adjectives.push_back(a); c.table(a.lemma, n.numeral, n.token); }
      }
    } else if (const char* card = cardinal(v)) {
      o.numeral = greek(card, Num);
      if (o.numeral == kNone) o.numeral = greek(card);
      c.table(o.numeral, n.numeral, n.token);
      if (v > 1) o.number = Pl;
    }
  }
  // "of" attributes: a genitive with its own article when definite
  for (const SemNP& g : n.genitive) {
    if (text::lower(g.head) == "such") continue;   // C21: "such" read as a genitive (handled above)
    GrcNP x;
    npInto(g, c, x);
    o.genitive.push_back(x);
    break;
  }
  // relative clause
  for (const SemFrame& rf : n.relative) {
    // C18: "water to drink" (a to-infinitive without a relative word) after a verb whose valency_grc.tsv frames list
    // purp:inf (δίδωμι, παρέχω, πέμπω): the bare infinitive of purpose after the noun ("δός μοι ὕδωρ πιεῖν")
    if (!rf.hasSubject && rf.hasPred && rf.type == Kind::Decl && tokenLower(c.s, rf.pred.token - 1) == "to" &&
        c.verb != kNone && rf.subordinate.empty()) {
      bool purp = false;
      if (const Valency* v = gd_.valency(lx_.lemma(c.verb).key))
        for (const Frame& fr : v->frames) purp = purp || fr.kind == FrameKind::PurpInf;
      if (purp) {
        GrcClause ic;
        const uint32_t keepVerb = c.verb;
        ++c.depth;
        clauseInto(rf, c, ic);
        --c.depth;
        c.verb = keepVerb;
        ic.type = ClauseType::Decl;
        ic.hasSubject = false;
        if (ic.pred.modal == kNone)
          ic.pred.tense = durative(rf.pred.lemma, ic.pred.lemma, c.st) ? (uint8_t)Present : (uint8_t)Aorist;
        c.table(c.verb, rf.pred.lemma, rf.pred.token, "bare infinitive of purpose (valency purp:inf)");
        c.pendingInf.push_back(std::move(ic));
        break;
      }
    }
    GrcClause rc;
    const bool keepSubj = c.subjectRole;
    clauseInto(rf, c, rc);
    c.subjectRole = keepSubj;
    Role role = Role::Subject;
    auto relPron = [](const SemNP& x) {
      return x.isPronoun && (x.pronLemma == "which" || x.pronLemma == "who" || x.pronLemma == "that" ||
                             x.pronLemma == "que" || x.pronLemma == "quien");
    };
    if (rf.hasSubject && relPron(rf.subject)) { role = Role::Subject; rc.hasSubject = false; }
    else if (rf.hasObject && relPron(rf.object)) { role = Role::Object; rc.hasObject = false; }
    else if (rf.hasSubject && !rf.hasObject) role = Role::Object;
    rc.relRole = role;
    o.relative.push_back(rc);
    break;
  }
  for (const SemNP& k : n.coord) {
    GrcNP x;
    npInto(k, c, x);
    o.coord.push_back(x);
  }
}

GrcNP GreekTransfer::np(const SemNP& n, const SemSentence& s, const transfer::Settings& st, transfer::Memory& mem,
                        GrcClauseOut& out) const {
  Ctx c(s, st, mem, out);
  GrcNP o;
  npInto(n, c, o);
  return o;
}

// ---- obliques -------------------------------------------------------------------------------------------------------
void GreekTransfer::obliqueInto(const frame::SemOblique& ob, Ctx& c, GrcClause& cl) const {
  const SemNP& n = ob.np;
  const std::string prep = ob.prep;
  c.cover(ob.token);
  const std::string head = text::lower(n.head);
  const bool time = transfer::tables::timeNoun(head);
  // C16: fixed prepositional phrases (lexical_en_grc.tsv kind pp): "in Latin" -> Ῥωμαϊστί, "at the bottom" -> ἐν τῷ
  // βάθει, "to school" -> πρὸς τὸν διδάσκαλον, "by mistake" -> ἁμαρτών agreeing with the subject, after the verb
  {
    const LexRow* r = nullptr;
    const std::string det = text::lower(n.determiner);
    if (n.adjectives.empty() && n.possessor.empty() && n.numeral.empty()) {
      if (!det.empty()) r = gt_.find("pp", (prep.empty() ? "" : prep + " ") + det + " " + head);
      if (!r) {
        r = gt_.find("pp", (prep.empty() ? "" : prep + " ") + head);
        // C25: an adverb row without the article is not the phrase with it: "en casa" -> οἴκοι, but "vive en la
        // casa" is in the house (ἐν τῇ οἰκίᾳ)
        if (r && r->frame == "adv" && (!det.empty() || n.definite)) r = nullptr;
      }
    }
    if (r) {
      const std::string& fr = r->frame;
      bool done = false;
      if (fr == "adv") {
        uint32_t id = greek(r->greek.c_str(), Adv);
        if (id == kNone) id = adjAdverb(r->greek.c_str());
        if (id != kNone) {
          cl.adverbs.push_back(GrcAdverb{id, ob.front ? AdvPos::Front : AdvPos::Auto});
          c.table(id, prep + " " + n.head, n.token, "fixed phrase (lexical_en_grc.tsv)");
          done = true;
        }
      } else if (fr.compare(0, 5, "prep:") == 0) {
        std::string spec = fr.substr(5);
        const bool def = spec.size() > 4 && spec.compare(spec.size() - 4, 4, " def") == 0;
        if (def) spec.resize(spec.size() - 4);
        const size_t plus = spec.find('+');
        GrcOblique o;
        o.prep = greek(spec.substr(0, plus).c_str(), Prep);
        o.case_ = plus == std::string::npos ? (uint8_t)Acc : curated::parseCase(spec.substr(plus + 1));
        o.np.head = greek(r->greek.c_str(), Noun);
        o.np.definite = def;
        o.np.case_ = o.case_;
        o.front = ob.front;
        if (o.prep != kNone && o.np.head != kNone) {
          c.table(o.prep, prep, ob.token);
          c.table(o.np.head, n.head, n.token, "fixed phrase (lexical_en_grc.tsv)");
          c.mem.lastGender = simpleGender(lx_.lemma(o.np.head).gender);
          cl.obliques.push_back(o);
          done = true;
        }
      } else if (fr == "ptc") {
        uint32_t id = greek(r->greek.c_str(), Participle);
        if (id == kNone) id = greek(r->greek.c_str());
        if (id != kNone && c.frame) {
          const SemFrame& sf = *c.frame;
          const bool plural = sf.hasSubject && (sf.subject.isPronoun ? sf.subject.pron.number == 2 : sf.subject.number == 2);
          GrcOblique o;
          o.case_ = Nom;
          o.np.head = id;
          o.np.case_ = Nom;
          o.np.number = plural ? Pl : Sg;
          // a plural group is masculine unless a feminine noun names it; a singular speaker / subject keeps its gender
          o.np.gender = plural && (!sf.hasSubject || sf.subject.isPronoun) ? (uint8_t)M : c.subjGender ? c.subjGender : (uint8_t)M;
          o.end = true;
          c.table(id, prep + " " + n.head, n.token, "fixed phrase: participle (lexical_en_grc.tsv)");
          cl.obliques.push_back(o);
          done = true;
        }
      }
      if (done) {
        c.cover(n.tokens);
        c.cover(n.token);
        return;
      }
    }
  }
  auto add = [&](const char* gprep, uint8_t cs) {
    GrcOblique o;
    if (gprep) {
      o.prep = greek(gprep, Prep);
      if (o.prep != kNone) c.table(o.prep, prep, ob.token);
    }
    o.case_ = cs;
    npInto(n, c, o.np);
    o.np.case_ = cs;
    o.front = ob.front;
    cl.obliques.push_back(o);
  };
  // C25: "(in / this / that / one) morning" -> ἕωθεν (Attic prose has no everyday noun: ἠώς is poetic); Spanish "la /
  // esta mañana" (without an article "mañana" is tomorrow); "(at) night" -> νύκτωρ
  {
    const std::string dt = text::lower(n.determiner);
    const bool morning = (head == "morning" && dt != "every" && dt != "each") ||
                         (head == "mañana" && in(dt, {"la", "esta", "esa", "aquella", "una", "this", "that", "the", "a"}));
    const bool night = (head == "night" && (prep == "at" || prep == "by")) || (head == "noche" && (prep == "of" || prep == "by"));
    if ((morning || night) && n.adjectives.empty() && n.genitive.empty() && n.possessor.empty() &&
        (prep.empty() || in(prep, {"in", "at", "on", "by", "during", "of"}))) {
      const uint32_t id = greek(morning ? "ἕωθεν" : "νύκτωρ", Adv);
      if (id != kNone) {
        cl.adverbs.push_back(GrcAdverb{id, ob.front ? AdvPos::Front : AdvPos::Auto});
        c.table(id, (prep.empty() ? "" : prep + " ") + n.head, n.token, "time of day: Attic adverb");
        c.cover(n.tokens);
        c.cover(n.token);
        return;
      }
    }
  }
  // time adverbs as nouns ("today", "tomorrow")
  if (prep.empty() || ((prep == "on" || prep == "at" || prep == "in") && time)) {
    if (const char* adv = adverbTable(head, false)) {
      if (n.determiner.empty() && n.adjectives.empty()) {
        GrcAdverb a;
        a.lemma = greek(adv, Adv);
        a.pos = ob.front ? AdvPos::Front : AdvPos::Auto;
        if (a.lemma != kNone) {
          c.cover(n.tokens);
          cl.adverbs.push_back(a);
          c.table(a.lemma, head, n.token);
          return;
        }
      }
    }
    // C18: "all day", "all night": the whole stretch of time is the accusative of duration (πᾶσαν τὴν νύκτα)
    if (prep.empty()) { add(nullptr, time && text::lower(n.determiner) != "all" ? (uint8_t)Dat : (uint8_t)Acc); return; }
    add(nullptr, Dat);   // bare dative of time: "on the third day"
    return;
  }
  if (n.isPronoun && n.pronLemma.empty() && n.pron.person == 0) { c.cover(n.tokens); return; }   // "in here"
  const bool person = transfer::animate(n);
  if (!c.prepOverride.empty() && prep == c.prepOverride && !c.prepOverrideGreek.empty()) {
    add(c.prepOverrideGreek.c_str(), c.prepOverrideCase ? c.prepOverrideCase : (uint8_t)Acc);
    return;
  }
  if (!c.objPrep.empty() && prep == c.objPrep && !cl.hasObject) {   // verbprep frame obj ("wait for me")
    cl.hasObject = true;
    npInto(n, c, cl.object);
    return;
  }
  if (prep == "to") {
    if (person && !c.motion) {
      if (!cl.hasIndirect) { cl.hasIndirect = true; npInto(n, c, cl.indirect); return; }
      add(nullptr, Dat);
      return;
    }
    if (person) { add("πρός", Acc); return; }
    add("εἰς", Acc);
    return;
  }
  if (prep == "for") {
    if (time) { add(nullptr, Acc); return; }
    if (person && !cl.hasIndirect) { cl.hasIndirect = true; npInto(n, c, cl.indirect); return; }
    add("ὑπέρ", Gen);
    return;
  }
  if (prep == "with") { if (person) add("μετά", Gen); else add(nullptr, Dat); return; }
  if (prep == "at") {
    if (time) { add(nullptr, Dat); return; }
    if (person) { add("παρά", Dat); return; }
    add("ἐν", Dat);
    return;
  }
  if (prep == "in" || prep == "inside") {
    if (time) { add(nullptr, Dat); return; }
    if (c.motion && (c.frame && (c.frame->pred.lemma == "put" || c.frame->pred.lemma == "fall"))) { add("εἰς", Acc); return; }
    add("ἐν", Dat);
    return;
  }
  if (prep == "on" || prep == "upon") { if (c.motion) add("ἐπί", Acc); else add("ἐπί", Gen); return; }
  if (prep == "into" || prep == "onto") { add(prep == "into" ? "εἰς" : "ἐπί", Acc); return; }
  if (prep == "of" || prep == "about") { add("περί", Gen); return; }
  if (prep == "by") {
    if (c.frame && c.frame->pred.voice == frame::Voice::Passive && person) { add("ὑπό", Gen); return; }
    // C21: "by the fire / river / door" (a definite thing, an active verb) is a place: παρά + dative ("παρὰ τῷ πυρί");
    // without the article it is the means ("by ship" -> dative)
    const bool passive = c.frame && c.frame->pred.voice == frame::Voice::Passive;
    if (!person && (passive || !n.definite || n.isPronoun)) { add(nullptr, Dat); return; }
    add("παρά", Dat);
    return;
  }
  if (prep == "from") { if (person) add("παρά", Gen); else add("ἐκ", Gen); return; }
  if (prep == "out of") { add("ἐκ", Gen); return; }
  if (prep == "away from") { add("ἀπό", Gen); return; }
  if (prep == "towards" || prep == "toward") { add("πρός", Acc); return; }
  // C25: "across the sea / the river / the field" -> διά + genitive ("[across]" was left unknown, Fix)
  if (prep == "across") { add("διά", Gen); return; }
  // C25: Spanish "cerca de" (the frame keeps it as written) -> ἐγγύς + genitive, as English "near"
  if (prep == "cerca de" || prep == "near") {
    const uint32_t eg = greek("ἐγγύς", Adv) != kNone ? greek("ἐγγύς", Adv) : greek("ἐγγύς");
    if (eg != kNone) {
      GrcOblique o;
      o.prep = eg;
      c.table(eg, prep, ob.token);
      o.case_ = Gen;
      npInto(n, c, o.np);
      o.np.case_ = Gen;
      o.front = ob.front;
      cl.obliques.push_back(o);
      return;
    }
  }
  // the rest from preps_en_grc.tsv (first row of the preposition)
  for (const PrepEntry& pe : gd_.preps()) {
    if (pe.english != prep) continue;
    if (pe.infinitive) break;
    if (pe.greek == "-" || pe.greekKey.empty()) { add(nullptr, pe.case_ ? pe.case_ : (uint8_t)Dat); return; }
    GrcOblique o;
    o.prep = findLemma(lx_, pe.greek, Prep);
    if (o.prep == kNone) o.prep = findLemma(lx_, pe.greek);
    if (o.prep != kNone) c.table(o.prep, prep, ob.token);
    o.case_ = pe.case_;
    npInto(n, c, o.np);
    o.np.case_ = pe.case_;
    o.front = ob.front;
    cl.obliques.push_back(o);
    return;
  }
  c.out.unknownWords.push_back(prep);
  add(nullptr, Dat);
}

// ---- verb phrase of a phrasebook "vp" row ("play cards" -> χάρταις παίζειν) --------------------------------------
bool GreekTransfer::fixedVerbPhrase(const std::string& greekText, Ctx& c, GrcClause& cl) const {
  (void)c;
  std::vector<std::string> ws = words(greekText);
  if (ws.empty()) return false;
  morph::Token mt;
  analyse(lx_, ws.back(), mt);
  uint32_t verb = kNone;
  for (const lex::Analysis& a : mt.analyses)
    if (lx_.lemma(a.lemma).pos == Verb) { verb = a.lemma; break; }
  if (verb == kNone) return false;
  cl.pred.lemma = verb;
  ws.pop_back();
  for (const std::string& w : ws) {
    morph::Token t;
    analyse(lx_, w, t);
    bool done = false;
    for (const lex::Analysis& a : t.analyses) {
      const lex::Lemma l = lx_.lemma(a.lemma);
      const Features f = unpack(lx_.feature(a.feat));
      if (l.pos == Noun && f.case_) {
        GrcOblique o;
        o.case_ = f.case_;
        o.np.head = a.lemma;
        o.np.number = f.number ? f.number : (uint8_t)Sg;
        o.np.case_ = f.case_;
        cl.obliques.push_back(o);
        done = true;
        break;
      }
      if (l.pos == Adv || l.pos == Particle) {
        cl.adverbs.push_back(GrcAdverb{a.lemma, AdvPos::Auto});
        done = true;
        break;
      }
    }
    if (!done) return false;
  }
  return true;
}

// ---- predicate ------------------------------------------------------------------------------------------------------
void GreekTransfer::predicateInto(const SemFrame& f, Ctx& c, GrcClause& cl) const {
  GrcPredicate& p = cl.pred;
  const frame::SemPredicate& sp = f.pred;
  c.cover(sp.token);
  c.cover(sp.auxTokens);
  const std::string lemma = text::lower(sp.lemma);
  const std::string en = english(sp.lemma, c.st);
  const bool past = sp.tense == frame::Tense::Past;
  auto row = [&](const char* kind, const std::string& src, std::string_view frm = {}) -> const LexRow* {
    if (const LexRow* r = gt_.find(kind, src, frm)) return r;
    return nullptr;
  };
  auto tableVerb = [&](const std::string& src) -> const LexRow* {   // kind verb, by tense
    const char* want = past ? "past" : "present";
    // C25: the pivot of this source verb, not of the clause's main verb: a catenative complement ("sé leer", "I know
    // how to read") took the main verb's row (οἶδα) and vanished
    const std::string srcEn = src == lemma ? en : english(src, c.st);
    for (const std::string& s : {src, srcEn}) {
      if (s.empty()) continue;
      // C18: frame "intr" = the verb without an object ("the ship is leaving" -> ἀπέρχεται, not λείπει)
      // C25: not with a place adverb ("lay there", "lie down": a position, not "tell a lie")
      bool place = false;
      for (const frame::SemAdverb& a : f.adverbs)
        place = place || in(text::lower(a.lemma), {"here", "there", "down", "aquí", "allí", "ahí", "allá"});
      place = place || in(text::lower(sp.particle), {"down"});
      if (!f.hasObject && f.obliques.empty() && !place) {
        if (const LexRow* r = row("verb", s, "intr")) return r;
        if (const LexRow* r = row("verb", s, "intr-mid")) return r;   // C25: intransitive and middle (ψεύδομαι)
      }
      if (const LexRow* r = row("verb", s, want)) return r;
      for (const LexRow& r : gt_.rows())
        if (r.kind == "verb" && r.source == s &&
            (r.frame.empty() || r.frame == "impers" || r.frame == "mid" ||
             // C21: frame "thing" = only with an object that is a thing ("bring the water" -> φέρω; a person or an
             // animal keeps the reverse index's ἄγω)
             (r.frame == "thing" && f.hasObject && !transfer::animate(f.object) && !f.object.isPronoun)))
          return &r;
    }
    return nullptr;
  };
  bool rowMiddle = false;   // C21: a verb row of frame "mid" chose the verb
  auto choose = [&](const std::string& lm, int token, bool hasObj, bool personObj) {
    Choice ch;
    ch.token = token;
    uint32_t id = kNone;
    if (lm == "be" || lm == "ser" || lm == "estar") { id = greek("εἰμί", Verb); c.table(id, lm, token); return id; }
    if (lm == "can" || lm == "poder") { id = greek("δύναμαι", Verb); c.table(id, lm, token); return id; }
    // C25 (lexical_en_grc.tsv kind verbobj): the verb chosen by its object, the object kept ("wash your hands" ->
    // νίψαι τὰς χεῖρας, middle; λοῦσον was OK and wrong)
    if (hasObj && f.hasObject && !f.object.isPronoun)
      if (const LexRow* r = gt_.find("verbobj", lm + " " + text::lower(f.object.head))) {
        id = lexRowLemma(r, Verb);
        if (id != kNone) {
          c.table(id, lm + " " + f.object.head, token, "lexical_en_grc.tsv: verbobj");
          if (r->frame == "mid") rowMiddle = true;
          return id;
        }
      }
    if (const LexRow* r = tableVerb(lm)) {
      id = lexRowLemma(r, Verb);
      if (id != kNone) {
        c.table(id, lm, token, "lexical_en_grc.tsv");
        if (r->frame == "mid" || r->frame == "intr-mid") rowMiddle = true;   // C21: "touch" -> ἅπτομαι (+ gen)
        return id;
      }
    }
    id = select(lm, Verb, c.context, hasObj, personObj, c.st, ch);
    c.out.choices.push_back(ch);
    if (id == kNone) c.out.unknownWords.push_back(lm);
    return id;
  };
  const bool hasObj = f.hasObject;
  const bool personObj = f.hasObject && transfer::animate(f.object);
  uint32_t verb = kNone;
  // verb + preposition (lexical_en_grc.tsv kind verbprep)
  const LexRow* vpr = nullptr;
  for (const frame::SemOblique& o : f.obliques) {
    vpr = row("verbprep", lemma + " " + o.prep);
    if (!vpr && !en.empty()) vpr = row("verbprep", en + " " + o.prep);
    if (vpr) {
      if (vpr->frame == "obj") c.objPrep = o.prep;
      else if (vpr->frame.compare(0, 5, "prep:") == 0) {
        const std::string spec = vpr->frame.substr(5);
        const size_t plus = spec.find('+');
        c.prepOverride = o.prep;
        c.prepOverrideGreek = spec.substr(0, plus);
        c.prepOverrideCase = plus == std::string::npos ? (uint8_t)Acc : curated::parseCase(spec.substr(plus + 1));
      }
      break;
    }
  }
  bool midPresent = false;   // C21: subject row frame "mid-pres" (set below, applied once the tense is known)
  bool rowVoice = false;     // C21: the subject row fixed the voice
  // Spanish pronominal verb ("irse", "inclinarse"): the Greek middle; C25: also with the clitic read as the object
  // ("nos divertimos", "se divirtieron" -> the row "divertirse")
  const bool seParticle = sp.particle == "se" ||
                          (c.st.lang == frame::SrcLang::Es && reflexiveClitic(f, true) && row("verb", lemma + "se"));
  const LexRow* ph = nullptr;
  if (seParticle) ph = row("verb", lemma + "se");
  if (!sp.particle.empty() && !seParticle) {
    ph = row("phrasal", lemma + " " + sp.particle);
    if (!ph && !en.empty()) ph = row("phrasal", en + " " + sp.particle);
  }
  if (!sp.fixedLatin.empty() && fixedVerbPhrase(sp.fixedLatin, c, cl)) {
    verb = cl.pred.lemma;
    Choice ch;
    ch.token = sp.token;
    ch.source = sp.lemma;
    ch.lemma = verb;
    ch.kind = "phrasebook";
    ch.note = "phrasebook: " + sp.fixedLatin;
    c.out.choices.push_back(ch);
  } else if (c.forcedVerb != kNone) {
    verb = c.forcedVerb;
  } else if (ph && (verb = lexRowLemma(ph, Verb)) != kNone) {
    c.table(verb, seParticle ? sp.lemma + "se" : sp.lemma + " " + sp.particle, sp.token, "lexical_en_grc.tsv");
    if (ph->frame == "mid") cl.pred.voice = Middle;   // C18: the voice of the row
    else if (ph->frame == "pass") cl.pred.voice = Passive;   // "wake up" -> ἠγέρθην
  } else if (const LexRow* sv = f.hasSubject && !f.subject.isPronoun
                                    ? row("subject", text::lower(f.subject.head) + " " + lemma) : nullptr) {
    // C18 (lexical_en_grc.tsv kind subject): the verb chosen by its subject ("the sun rose" -> ἀνέτειλεν)
    verb = lexRowLemma(sv, Verb);
    if (verb == kNone) verb = choose(lemma, sp.token, hasObj, personObj);
    else {
      c.table(verb, f.subject.head + " " + sp.lemma, sp.token, "lexical_en_grc.tsv: subject");
      // C21: the voice of the row: "mid" = middle ("the rain stopped" -> ἐπαύσατο), "mid-pres" = middle in the
      // present system only ("the sun is setting" -> δύεται; "the sun set" -> ἔδυ, the intransitive root aorist)
      if (sv->frame == "mid") { cl.pred.voice = Middle; rowVoice = true; }
      else if (sv->frame == "mid-pres") { midPresent = true; rowVoice = true; }
    }
  } else if (vpr && vpr->greek != "-" && (verb = lexRowLemma(vpr, Verb)) != kNone) {
    c.table(verb, sp.lemma, sp.token, "verb with its preposition");
  } else if ((lemma == "have" || en == "have") && f.type == Kind::Imp && sp.complementVerb.empty()) {
    verb = greek("λαμβάνω", Verb);   // "Have some tea." = take
    c.table(verb, sp.lemma, sp.token, "\"have\" as a command: take");
  } else if (sp.ellipsis || ((lemma == "can" || lemma == "do") && !f.hasObject && f.obliques.empty() &&
                             f.predAdj.empty() && f.predicative.empty() && sp.complementVerb.empty() &&
                             c.mem.lastVerb != kNone && f.type == Kind::Decl)) {
    verb = c.mem.lastVerb != kNone ? c.mem.lastVerb : greek("ποιέω", Verb);
    if (lemma == "can" || lemma == "poder") {   // C16: "This one can." -> αὕτη δύναται (the modal alone)
      verb = greek("δύναμαι", Verb);
      c.table(verb, sp.lemma, sp.token, "elliptical \"can\"");
    } else {
      c.table(verb, sp.lemma, sp.token, "verb of the previous clause");
    }
  } else {
    verb = choose(lemma, sp.token, hasObj, personObj);
    if (!sp.particle.empty() && !seParticle) {
      const uint32_t a = adverb(sp.particle, -1, c, c.motion);
      if (a != kNone) cl.adverbs.push_back(GrcAdverb{a, AdvPos::Auto});
    }
  }
  p.lemma = verb;
  if (c.forcedVoice) p.voice = c.forcedVoice;
  if (rowMiddle) { p.voice = Middle; rowVoice = true; }
  // catenative complement ("want to go" -> βούλομαι + infinitive)
  if (!sp.complementVerb.empty()) {
    const uint32_t comp = choose(text::lower(sp.complementVerb), sp.complementToken, hasObj, personObj);
    c.cover(sp.complementToken);
    p.modal = verb;
    p.lemma = comp;
  }
  switch (sp.modality) {
    case Modality::Can: p.modal = greek("δύναμαι", Verb); break;
    case Modality::Must: p.modal = greek("δεῖ", Verb); break;
    case Modality::Should:   // C16: "Which way should I go?" -> ποῖ χρή με ἰέναι;
      p.modal = greek("χρή", Verb) != kNone ? greek("χρή", Verb) : greek("δεῖ", Verb);
      break;
    case Modality::Want: p.modal = greek("βούλομαι", Verb); break;
    case Modality::May: p.modal = greek("ἔξεστι", Verb); break;
    case Modality::Let: p.mood = Subjunctive; p.person = 1; p.number = Pl; break;
    default: break;
  }
  if (p.modal != kNone && p.modal == p.lemma) p.modal = kNone;
  // tense / aspect (order_grc.txt tense.*): aorist for past events, imperfect for past states and background,
  // perfect only for a resulting state (passive present without an agent: "the clock is broken")
  const uint32_t main = p.modal != kNone ? p.modal : p.lemma;
  const std::string mk = main != kNone ? std::string(lx_.lemma(main).key) : std::string();
  bool state = mk == "εἰμί" || mk == "ἔχω" || mk == "οἶδα" || mk == "οἰκέω" || mk == "βούλομαι" ||
               mk == "δύναμαι" || mk == "ἐθέλω" || mk == "νομίζω" || mk == "δοκέω" || mk == "κεῖμαι" ||
               mk == "οἴομαι" || mk == "καθεύδω" || sp.habitual;   // C18: καθεύδω has no Attic aorist in prose
  // C16: "be" + state adjective in the past: imperfect for a lasting state (ὠργίζετο), aorist for an event (ἥμαρτες)
  if (c.forcedVerb != kNone && main == c.forcedVerb && !c.forcedLight) state = durative(std::string(), c.forcedVerb, c.st);
  uint8_t tense = Present;
  if (sp.tense == frame::Tense::Future) tense = Future;
  else if (past) {
    if (sp.aspect == frame::Aspect::Perfect && !sp.pastModal && p.modal == kNone) tense = Pluperfect;   // C16
    else if (sp.aspect == frame::Aspect::Progressive || sp.habitual || state || sp.pastModal) tense = Imperfect;
    else tense = Aorist;
  } else if (sp.aspect == frame::Aspect::Perfect) {
    tense = Aorist;
  } else if (sp.pastModal) {
    tense = Imperfect;
  }
  // C21: a subject row with its own voice ("mid", "mid-pres") decides: a Spanish "se" read as a passive is not one
  // ("el sol se pone" -> δύεται, not the perfect δέδυκεν)
  if (sp.voice == frame::Voice::Passive && !rowVoice) {
    p.voice = Passive;
    bool agent = false;
    for (const frame::SemOblique& o : f.obliques) agent = agent || o.prep == "by";
    if (tense == Present && !agent) {
      tense = Perfect;
      // C16 (lexical_en_grc.tsv kind perfect): the perfect active has the passive sense ("is broken" -> κατέαγεν)
      if (main != kNone)
        for (const LexRow& r : gt_.rows())
          if (r.kind == "perfect" && text::greek_key(r.greek) == mk) { p.voice = 0; break; }
    }
  }
  // C21: English verbs whose past is the bare form (put, cut, hit, set ...): with a 3rd-person singular subject and no
  // auxiliary the bare form cannot be the present ("The king put his seal on the letter" came as a present)
  if (tense == Present && c.st.lang == frame::SrcLang::En && sp.aspect == frame::Aspect::Simple && sp.auxTokens.empty() &&
      sp.modality == Modality::None && f.type == Kind::Decl && f.hasSubject && sp.token >= 0 &&
      (size_t)sp.token < c.s.tokens.size() && c.s.tokens[(size_t)sp.token].lower == lemma &&
      in(lemma, {"put", "cut", "hit", "set", "shut", "let", "hurt", "cost", "spread", "burst", "split", "quit"})) {
    const SemNP& sn = f.subject;
    const bool third = sn.isPronoun ? (sn.pron.person == 3 && sn.pron.number != 2 && sn.determiner != "all")
                                    : (sn.number != 2 && sn.coord.empty());
    if (third) tense = Aorist;
  }
  // C21: a Spanish imperfect ("soplaba", "cantaba") is a Greek imperfect (the frame keeps only "past")
  if (tense == Aorist && c.st.lang == frame::SrcLang::Es && sp.auxTokens.empty() && sp.token >= 0 &&
      (size_t)sp.token < c.s.tokens.size() &&
      ((c.s.tokens[(size_t)sp.token].feats >> nlp::morph::TenseShift) & 7u) == nlp::morph::TenseImp)
    tense = Imperfect;
  if (sp.deliberative) { tense = Aorist; p.mood = Subjunctive; }   // "τί ποιήσω;"
  // "would": no optative (style 1.2); C16: a counterfactual main clause takes ἄν + imperfect (present time) or aorist
  // (past time: "would have"): "οὐκ ἂν ἐνθάδε ἦσθα"
  if (sp.mood == frame::SrcMood::Conditional) {
    if (c.depth == 0 && sp.modality != Modality::Let) {
      cl.an = true;
      tense = past || sp.aspect == frame::Aspect::Perfect ? (uint8_t)Aorist : (uint8_t)Imperfect;
      if (state && tense == Aorist) tense = Imperfect;
    } else {
      tense = past ? Imperfect : Present;
    }
  }
  // C16: verb rows of frame "nonfinite" (an infinitive, a subjunctive, a future or a dependent clause: "go" -> εἶμι,
  // ἰέναι / ἴωμεν / εἶ) and "sub" (a dependent clause only: "before she comes" -> πρὶν ἥκειν)
  if (c.forcedVerb == kNone && !ph && !(vpr && vpr->greek != "-") && sp.fixedLatin.empty() && p.lemma != kNone) {
    const bool nonfinite = p.modal != kNone || p.mood == Subjunctive || tense == Future || c.depth > 0;
    const std::string src = text::lower(sp.complementVerb.empty() ? sp.lemma : sp.complementVerb);
    const std::string srcEn = english(src, c.st);
    const LexRow* r = nullptr;
    for (const std::string& k : {src, srcEn}) {
      if (k.empty() || r) continue;
      if (c.depth > 0) r = row("verb", k, "sub");
      if (!r && nonfinite) r = row("verb", k, "nonfinite");
    }
    const uint32_t id = lexRowLemma(r, Verb);
    if (id != kNone) {
      p.lemma = id;
      c.table(id, src, sp.complementVerb.empty() ? sp.token : sp.complementToken, "lexical_en_grc.tsv: " + r->frame);
      if (r->frame == "nonfinite" && (tense == Future || tense == Aorist)) tense = Present;   // εἶμι: present system
      // C18: ἥκω "I have come" has no prose aorist: a past "came" in a dependent clause is the imperfect ἧκεν
      if (r->frame == "sub" && tense == Aorist && lx_.lemma(id).key == std::string("ἥκω")) tense = Imperfect;
    }
  }
  p.tense = tense;
  if (midPresent)   // also over a Spanish "se" read as a passive ("el sol se puso" -> ἔδυ, not ἐδύθη)
    p.voice = tense == Present || tense == Imperfect || tense == Future ? (uint8_t)Middle : (uint8_t)0;
  if (p.modal != kNone) {
    p.infTense = durative(sp.complementVerb.empty() ? sp.lemma : sp.complementVerb, p.lemma, c.st) ? (uint8_t)Present
                                                                                                     : (uint8_t)Aorist;
    if (p.lemma != kNone && lx_.lemma(p.lemma).key == std::string("εἰμί")) p.infTense = Present;
    // C25: "know how to" (οἶδα / ἐπίσταμαι + infinitive) is a general ability: the present infinitive (οἶδα νεῖν)
    if (lx_.lemma(p.modal).key == std::string("οἶδα") || lx_.lemma(p.modal).key == std::string("ἐπίσταμαι"))
      p.infTense = Present;
    p.infVoice = p.voice;
    p.voice = 0;
  }
  if (p.lemma != kNone) {
    c.mem.lastVerb = p.lemma;
    c.mem.lastMotion = c.motion;
  }
}

// ---- clauses --------------------------------------------------------------------------------------------------------
void GreekTransfer::clauseInto(const SemFrame& f, Ctx& c, GrcClause& cl) const {
  // C25: an irregular English past misread by the tagger (see pastFormRepair); (1) is Check, (2) only fixes the tense
  if (c.st.lang == frame::SrcLang::En && c.st.srcLex) {
    SemFrame fx;
    int how = 0;
    if (pastFormRepair(lx_, *c.st.srcLex, f, c.s, fx, how)) {
      c.out.notes.push_back(rules::Reason{-1, "form", "\"" + c.s.tokens[(size_t)f.pred.token].text + "\" read as the past of \"" +
                                                        fx.pred.lemma + "\" (english.vpl)", ""});
      if (how == 1 && std::find(c.out.flags.begin(), c.out.flags.end(), "past-form") == c.out.flags.end())
        c.out.flags.push_back("past-form");
      clauseInto(fx, c, cl);
      return;
    }
  }
  // C25: a time adverb the parser made an "of" attribute ("We will eat bread tomorrow." -> bread of tomorrow; ἐπιουσίου
  // was produced): the clause's adverb (αὔριον)
  if (c.st.lang == frame::SrcLang::En && f.hasPred && f.hasObject && f.object.genitive.size() == 1) {
    const SemNP& g = f.object.genitive[0];
    const std::string gh = text::lower(g.head);
    if (in(gh, {"today", "tomorrow", "yesterday", "tonight"}) && g.determiner.empty() && g.adjectives.empty() &&
        g.genitive.empty() && g.token > f.object.token) {
      SemFrame fx = f;
      fx.object.genitive.clear();
      frame::SemAdverb a;
      a.lemma = gh;
      a.token = g.token;
      fx.adverbs.push_back(a);
      c.out.notes.push_back(rules::Reason{-1, "form", "\"" + g.head + "\" read as an attribute: the clause's adverb", ""});
      clauseInto(fx, c, cl);
      return;
    }
  }
  // C25: a bare noun object the parser hung elsewhere: "We will visit grandfather tomorrow." ("grandfather" as the
  // determiner of "tomorrow"), "I will see grandmother today." ("grandmother" as an adverb). A transitive clause without
  // an object: a word english.vpl knows only as a noun becomes the object (with the article, as Greek says ὁ πάππος)
  if (c.st.lang == frame::SrcLang::En && c.st.srcLex && f.hasPred && !f.hasObject && !f.copula && f.type != Kind::Frag &&
      f.pred.complementVerb.empty() && !transfer::tables::motionVerb(f.pred.lemma)) {
    auto nounOnly = [&](const std::string& w) {
      std::vector<lex::Analysis> an;
      c.st.srcLex->lookup(text::en_key(w), an);
      bool noun = false, other = false;
      for (const lex::Analysis& a : an) {
        const uint8_t lp = c.st.srcLex->lemma(a.lemma).pos;
        if (lp == Noun) noun = true;
        else if (lp == Adv || lp == Adj || lp == Det || lp == Pron || lp == Prep) other = true;   // a verb reading is no obstacle
      }
      return noun && !other;
    };
    SemFrame fx;
    bool moved = false;
    for (size_t i = 0; i < f.obliques.size() && !moved; ++i) {
      const frame::SemOblique& o = f.obliques[i];
      const std::string det = text::lower(o.np.determiner);
      if (!o.prep.empty() || det.empty() || !transfer::tables::timeNoun(text::lower(o.np.head)) || !nounOnly(det)) continue;
      int dt = -1;
      for (int t : o.np.tokens) if (tokenLower(c.s, t) == det) dt = t;
      if (dt < 0) continue;
      fx = f;
      fx.hasObject = true;
      fx.object = SemNP{};
      fx.object.head = det;
      fx.object.surface = c.s.tokens[(size_t)dt].text;
      fx.object.token = dt;
      fx.object.tokens = {dt};
      fx.object.definite = true;
      fx.obliques[i].np.determiner.clear();
      fx.obliques[i].np.tokens.erase(std::remove(fx.obliques[i].np.tokens.begin(), fx.obliques[i].np.tokens.end(), dt),
                                     fx.obliques[i].np.tokens.end());
      moved = true;
    }
    for (size_t i = 0; i < f.adverbs.size() && !moved; ++i) {
      const frame::SemAdverb& a = f.adverbs[i];
      if (a.token < 0 || a.lemma.find(' ') != std::string::npos || !nounOnly(text::lower(a.lemma))) continue;
      fx = f;
      fx.hasObject = true;
      fx.object = SemNP{};
      fx.object.head = text::lower(a.lemma);
      fx.object.surface = c.s.tokens[(size_t)a.token].text;
      fx.object.token = a.token;
      fx.object.tokens = {a.token};
      fx.object.definite = true;
      fx.adverbs.erase(fx.adverbs.begin() + (long)i);
      moved = true;
    }
    if (moved) {
      if (std::find(c.out.flags.begin(), c.out.flags.end(), "clause-repair") == c.out.flags.end())
        c.out.flags.push_back("clause-repair");
      c.out.notes.push_back(rules::Reason{-1, "form", "\"" + fx.object.surface + "\" read as the object: check it", ""});
      clauseInto(fx, c, cl);
      return;
    }
  }
  // C25: Spanish "Esta mañana comimos pan.": a time noun with a demonstrative read as the subject while the verb has
  // another person (comimos = 1st plural): the NP is a time adverbial and the subject the verb's own pronoun
  if (c.st.lang == frame::SrcLang::Es && c.st.srcLex && f.hasSubject && !f.subject.isPronoun && f.hasPred &&
      f.pred.token >= 0 && (size_t)f.pred.token < c.s.tokens.size() &&
      in(text::lower(f.subject.head), {"mañana", "noche", "tarde", "día", "semana", "año", "mes", "invierno", "verano"}) &&
      in(text::lower(f.subject.determiner), {"este", "esta", "ese", "esa", "aquel", "aquella", "la", "el", "this", "that", "the"})) {
    uint8_t pe = 0, nu = 0;
    if (esPersonNumber(c.st.srcLex, c.s.tokens[(size_t)f.pred.token].lower, pe, nu) && pe != P3) {
      SemFrame fx = f;
      frame::SemOblique ob;
      ob.np = f.subject;
      ob.token = -1;
      ob.front = true;
      fx.obliques.insert(fx.obliques.begin(), ob);
      fx.subject = SemNP{};
      fx.subject.isPronoun = true;
      fx.subject.pron.person = pe;
      fx.subject.pron.number = nu == Pl ? 2 : 1;
      fx.subject.number = fx.subject.pron.number;
      fx.implicitSubject = true;
      c.out.notes.push_back(rules::Reason{-1, "form", "a time noun read as the subject: a time adverbial", ""});
      clauseInto(fx, c, cl);
      return;
    }
  }
  // C25: "The baby laughed when she saw the dog.": the frame builder may hang a when-clause as a that-complement whose
  // own connector is "when-time"; it is a time clause (ὅτε / ἐπεί, or C18's participle): ὅτι was produced
  {
    bool any = false;
    for (const frame::SemSub& sb : f.subordinate)
      if (sb.relation == Relation::Complement && !sb.frame.empty() && (sb.marker.empty() || text::lower(sb.marker) == "that"))
        for (const std::string& k : sb.frame[0].connectors) any = any || text::lower(k) == "when-time";
    if (any) {
      SemFrame fx = f;
      for (frame::SemSub& sb : fx.subordinate) {
        if (sb.relation != Relation::Complement || sb.frame.empty()) continue;
        std::vector<std::string>& ks = sb.frame[0].connectors;
        const auto it = std::find_if(ks.begin(), ks.end(), [](const std::string& k) { return text::lower(k) == "when-time"; });
        if (it == ks.end()) continue;
        ks.erase(it);
        sb.relation = Relation::Time;
        sb.marker = "when";
      }
      c.out.notes.push_back(rules::Reason{-1, "form", "a \"when\" clause read as a that-clause: a time clause", ""});
      clauseInto(fx, c, cl);
      return;
    }
  }
  // C25: Spanish "¡Que te diviertas!" / "¡Que duermas bien!": que + a 2nd person present subjunctive without a main
  // clause is a wish, the Greek imperative (εὐφραίνου); spanish.vpl must give only subjunctive 2nd person readings
  if (c.st.lang == frame::SrcLang::Es && c.st.srcLex && c.depth == 0 && f.type == Kind::Decl && f.hasPred &&
      f.pred.token >= 0 && (size_t)f.pred.token < c.s.tokens.size() && f.pred.auxTokens.empty() &&
      f.pred.modality == Modality::None) {
    bool que = false;
    for (const std::string& k : f.connectors) que = que || text::lower(k) == "that" || text::lower(k) == "que";
    std::vector<lex::Analysis> an;
    if (que) c.st.srcLex->lookup(text::es_key(c.s.tokens[(size_t)f.pred.token].lower), an);
    bool subj2 = false, other = false;
    uint8_t num = 0;
    for (const lex::Analysis& a : an) {
      const Features af = unpack(c.st.srcLex->feature(a.feat));
      if (af.pos != Verb) continue;
      if (af.mood == Subjunctive && af.person == P2) { subj2 = true; num = af.number; }
      else other = true;
    }
    if (subj2 && !other) {
      SemFrame fx = f;
      fx.type = Kind::Imp;
      fx.connectors.clear();
      for (const std::string& k : f.connectors)
        if (text::lower(k) != "that" && text::lower(k) != "que") fx.connectors.push_back(k);
      fx.imperativePlural = num == Pl;
      c.out.notes.push_back(rules::Reason{-1, "form", "\"que\" + subjunctive (a wish) rendered as a command", ""});
      clauseInto(fx, c, cl);
      return;
    }
  }
  const SemFrame* keepFrame = c.frame;
  const bool keepMotion = c.motion, keepNeg = c.negative, keepExist = c.existential;
  const uint8_t keepPerson = c.subjPerson, keepNumber = c.subjNumber, keepGender = c.subjGender;
  const std::string keepObjPrep = c.objPrep, keepPO = c.prepOverride, keepPOG = c.prepOverrideGreek;
  const uint8_t keepPOC = c.prepOverrideCase;
  const uint8_t keepStart = c.startGender;
  const uint32_t keepVerb = c.verb;
  const bool keepManner = c.mannerLight;
  c.mannerLight = false;
  std::vector<GrcClause> keepPending;
  keepPending.swap(c.pendingInf);
  c.verb = kNone;
  c.startGender = c.mem.lastGender;
  c.objPrep.clear();
  c.prepOverride.clear();
  c.prepOverrideGreek.clear();
  c.prepOverrideCase = 0;
  c.frame = &f;
  c.negative = f.negative;
  c.existential = f.existential;
  c.question = f.type == Kind::Yn || f.type == Kind::Wh;
  c.motion = transfer::tables::motionVerb(f.pred.lemma) || transfer::tables::motionVerb(f.pred.complementVerb);
  c.subjPerson = f.hasSubject && f.subject.isPronoun ? (f.subject.pron.person ? f.subject.pron.person : 3) : 3;
  c.subjNumber = f.hasSubject ? f.subject.number : 1;
  c.subjGender = 0;
  cl = GrcClause{};
  switch (f.type) {
    case Kind::Yn: cl.type = ClauseType::Yn; cl.ara = true; break;   // decision 4: ἆρα + verb first
    case Kind::Wh: cl.type = ClauseType::Wh; break;
    case Kind::Imp: cl.type = ClauseType::Imp; break;
    case Kind::Excl: cl.type = ClauseType::Excl; break;
    case Kind::Frag: cl.type = ClauseType::Frag; break;
    default: cl.type = ClauseType::Decl; break;
  }
  if (f.negative) cl.polarity = Polarity::Neg;
  if (f.expectYes) { cl.bias = YnBias::ExpectYes; cl.ara = false; }
  cl.existential = f.existential;
  c.context.clear();
  if (f.hasPred) c.context.push_back(f.pred.lemma);
  if (f.hasSubject) c.context.push_back(f.subject.head);
  if (f.hasObject) c.context.push_back(f.object.head);
  for (const frame::SemOblique& o : f.obliques) c.context.push_back(o.np.head);
  for (const frame::SemAdj& a : f.predAdj) c.context.push_back(a.lemma);

  // subject
  if (f.hasSubject && f.type != Kind::Imp) {
    cl.hasSubject = true;
    c.subjectRole = true;
    npInto(f.subject, c, cl.subject);
    c.subjectRole = false;
    // C25: generic "one" with must / should: the impersonal δεῖ / χρή says it alone ("One must not lie." -> οὐ δεῖ
    // ψεύδεσθαι)
    if (genericOne(f.subject, c.s) &&
        (f.pred.modality == Modality::Must || f.pred.modality == Modality::Should))
      cl.hasSubject = false;
    if (f.existential) cl.subject.adjFirst = false;   // C16: new information: "ἦν ποτε κόρη μικρά"
    if (f.subject.pron.emphatic) cl.subject.emphasis = true;
    if (f.subject.determiner == "all" && f.subject.isPronoun) {
      cl.pred.person = f.subject.pron.person ? f.subject.pron.person : 3;
      cl.pred.number = Pl;
    }
    // C21: Spanish pro-drop: the person and number of the implicit subject are the verb's own ("¿Por qué no viniste
    // ayer?" came as a 3rd singular): read from the verb token's features when they say 1st or 2nd person
    if (f.implicitSubject && cl.subject.isPronoun && c.st.lang == frame::SrcLang::Es && f.pred.token >= 0 &&
        (size_t)f.pred.token < c.s.tokens.size()) {
      const uint32_t ft = c.s.tokens[(size_t)f.pred.token].feats;
      uint32_t pe = (ft >> nlp::morph::PersonShift) & 3u, nu = (ft >> nlp::morph::NumberShift) & 3u;
      // the preterite 2nd person in -ste / -steis is unambiguous (viniste, cantaste, comisteis); the tagger's
      // features sometimes say 3rd
      const std::string& vw = c.s.tokens[(size_t)f.pred.token].lower;
      auto endsW = [&](const char* e) { const size_t n = std::strlen(e); return vw.size() > n + 2 && vw.compare(vw.size() - n, n, e) == 0; };
      bool sure = false;   // the form itself decides (not only the tagger's features)
      if (endsW("ste")) { pe = nlp::morph::Pers2; nu = nlp::morph::NumSing; sure = true; }
      else if (endsW("steis")) { pe = nlp::morph::Pers2; nu = nlp::morph::NumPlur; sure = true; }
      else {
        // spanish.vpl: when every analysis of the form has one person and number, that is the subject's ("Puse el
        // libro ..." came as a 3rd singular: puse is only 1st singular; "No tocas ..." came as a 2nd plural)
        uint8_t p1 = 0, n1 = 0;
        // only a simple form: in "has visto" the participle visto is also "yo visto" (vestir)
        if (f.pred.auxTokens.empty() && esPersonNumber(c.st.srcLex, vw, p1, n1) && (p1 == P1 || p1 == P2)) {
          pe = p1 == P1 ? nlp::morph::Pers1 : nlp::morph::Pers2;
          nu = n1 == Pl ? nlp::morph::NumPlur : nlp::morph::NumSing;
          sure = true;
        }
      }
      const uint8_t curNum = cl.subject.pron.number == Pl ? 2 : 1;
      const uint8_t newNum = nu == nlp::morph::NumPlur ? 2 : 1;
      if ((pe == nlp::morph::Pers1 || pe == nlp::morph::Pers2) && nu &&
          (cl.subject.pron.person == 3 || (sure && (cl.subject.pron.person != pe || curNum != newNum)))) {
        cl.subject.pron.person = (uint8_t)pe;
        cl.subject.pron.number = nu == nlp::morph::NumPlur ? (uint8_t)Pl : (uint8_t)Sg;
        cl.subject.number = cl.subject.pron.number;
        c.subjPerson = (uint8_t)pe;
        c.subjNumber = nu == nlp::morph::NumPlur ? 2 : 1;
      }
    }
    // a 3rd-person subject taken from the verb (Spanish pro-drop: "Es muy pequeño") stands for the last noun: its
    // Greek gender, not the Spanish adjective's (el gato -> ἡ γαλῆ)
    if (f.implicitSubject && cl.subject.isPronoun && cl.subject.pron.person == 3 && c.mem.lastGender &&
        c.st.lang == frame::SrcLang::Es) {
      cl.subject.pron.gender = c.mem.lastGender;
      cl.subject.gender = c.mem.lastGender;
    }
    c.subjGender = cl.subject.isPronoun ? cl.subject.pron.gender
                 : cl.subject.gender ? cl.subject.gender
                 : cl.subject.head != kNone ? simpleGender(lx_.lemma(cl.subject.head).gender) : 0;
  }
  // "be" + state adjective with a person subject (lexical_en_grc.tsv kind state): "Don't be afraid" -> μὴ φοβοῦ
  const LexRow* state = nullptr;
  if (f.copula && f.predAdj.size() == 1 && f.predicative.empty()) {
    const bool personSubj = (f.hasSubject && transfer::animate(f.subject)) || f.type == Kind::Imp ||
                            (f.hasSubject && f.subject.isPronoun && f.subject.pron.person > 0 && f.subject.pron.person < 3) ||
                            // C18: "The dog is barking because it is hungry": "it" for the animal of the main clause
                            (c.animateIt && f.hasSubject && f.subject.isPronoun && f.subject.pron.person == 3 &&
                             f.subject.pron.number != 2);
    if (personSubj) {
      const std::string a = text::lower(f.predAdj[0].lemma);
      state = gt_.find("state", a);
      if (!state) { const std::string ea = english(a, c.st); if (!ea.empty()) state = gt_.find("state", ea); }
      if (state && (state->frame == "adj" || state->frame == "adv")) state = nullptr;   // handled below
    }
  }
  if (!state && f.hasObject && f.hasPred)   // Spanish "tener miedo"
    state = gt_.find("state", text::lower(f.pred.lemma) + " " + text::lower(f.object.head));
  // C18: light verbs and verb + object idioms (lexical_en_grc.tsv kind light): the noun is the verb's meaning and is
  // not translated: "make a mistake" -> ἁμαρτάνω (ἥμαρτον), "take a walk" -> περιπατέω, "have a rest" -> ἀναπαύομαι
  // (frame mid), "pay a visit to X" -> X ἐπισκέπτομαι (frame obj:to = the PP's noun is the object)
  const LexRow* light = nullptr;
  if (!state && f.hasPred && f.hasObject && !f.copula && !f.object.isPronoun && !f.object.isName &&
      f.object.possessor.empty() && f.object.relative.empty() && f.object.coord.empty())
    light = gt_.find("light", text::lower(f.pred.lemma) + " " + text::lower(f.object.head));
  // C25: light rows of frame "manner" ("have a good time" -> ἡδέως διάγω, "pasarlo bien"): the evaluative adjective of
  // the object (or the clause's adverb, "lo pasamos bien") is the Greek verb's manner adverb (kind manner); without one
  // the row does not apply ("we have time" stays ἔχομεν χρόνον). Also with a pronoun object ("pasar lo").
  std::vector<uint32_t> mannerWords;
  bool mannerDegree = false;
  {
    const LexRow* m = light;
    if (!m && !state && f.hasPred && f.hasObject && !f.copula && f.object.isPronoun && f.object.possessor.empty()) {
      const std::string oh = text::lower(f.object.pronLemma.empty() ? f.object.head : f.object.pronLemma);
      m = gt_.find("light", text::lower(f.pred.lemma) + " " + oh);
    }
    if (m && m->frame.compare(0, 6, "manner") == 0) {
      auto addWords = [&](const LexRow* r) {
        for (const std::string& w : words(r->greek)) {
          uint32_t id = greek(w.c_str(), Adv);
          if (id == kNone) id = adjAdverb(w.c_str());
          if (id == kNone) id = greek(w.c_str(), Particle);
          if (id != kNone) mannerWords.push_back(id);
        }
      };
      bool any = false;
      for (const frame::SemAdj& a : f.object.adjectives)
        if (const LexRow* r = gt_.find("manner", text::lower(a.lemma))) {
          any = true;
          for (size_t i = 0; i < a.adverbs.size(); ++i) {   // "a very good time" -> πάνυ ἡδέως
            const uint32_t av = adverb(a.adverbs[i], i < a.advTokens.size() ? a.advTokens[i] : -1, c, false);
            if (av != kNone) mannerWords.push_back(av);
          }
          addWords(r);
        }
      for (const frame::SemAdverb& a : f.adverbs)
        for (const std::string& w : words(text::lower(a.lemma)))
          any = any || gt_.find("manner", w) != nullptr;
      // "such a good time" / "tan bien": the degree word goes first (οὕτως ἡδέως)
      if (any && f.object.determiner == "such") mannerDegree = true;
      if (any)
        for (int t : f.object.tokens)
          if (tokenLower(c.s, t) == "such" || tokenLower(c.s, t) == "so") { mannerDegree = true; c.cover(t); }
      light = any ? m : nullptr;
      c.mannerLight = any;
    }
  }
  const bool stateObj = state && f.hasObject && !f.copula;
  if (f.hasPred) {
    if (light) {
      c.forcedVerb = lexRowLemma(light, Verb);
      c.forcedVoice = light->frame.find("mid") != std::string::npos    ? (uint8_t)Middle
                      : light->frame.find("pass") != std::string::npos ? (uint8_t)Passive   // C25: "have fun"
                                                                        : (uint8_t)0;
      if (c.forcedVerb != kNone) {
        c.forcedLight = true;
        c.table(c.forcedVerb, f.pred.lemma + " " + f.object.head, f.pred.token, "light verb (lexical_en_grc.tsv)");
        c.cover(f.object.tokens);
        c.cover(f.object.token);
        if (mannerDegree) {
          const uint32_t so = greek("οὕτως", Adv);
          if (so != kNone) cl.adverbs.push_back(GrcAdverb{so, AdvPos::BeforeVerb});
        }
        for (uint32_t w : mannerWords) cl.adverbs.push_back(GrcAdverb{w, AdvPos::BeforeVerb});
      } else {
        light = nullptr;
        c.mannerLight = false;
      }
    } else if (state) {
      c.forcedVerb = lexRowLemma(state, Verb);
      c.forcedVoice = state->frame == "mid" ? (uint8_t)Middle : state->frame == "pass" ? (uint8_t)Passive : (uint8_t)0;   // C21: pass
      if (c.forcedVerb != kNone) {
        c.table(c.forcedVerb, stateObj ? f.pred.lemma + " " + f.object.head : f.predAdj[0].lemma,
                stateObj ? f.pred.token : f.predAdj[0].token, "state: lexical_en_grc.tsv");
        if (stateObj) c.cover(f.object.tokens);
        else c.cover(f.predAdj[0].token);
        // C18: the adjective's degree words go with the verb ("so tired that" -> οὕτω ἔκαμνεν ὥστε)
        if (!stateObj)
          for (size_t i = 0; i < f.predAdj[0].adverbs.size(); ++i) {
            const uint32_t av = adverb(f.predAdj[0].adverbs[i],
                                       i < f.predAdj[0].advTokens.size() ? f.predAdj[0].advTokens[i] : -1, c, false);
            if (av != kNone) cl.adverbs.push_back(GrcAdverb{av, AdvPos::BeforeVerb});
          }
      } else {
        state = nullptr;
      }
    }
    predicateInto(f, c, cl);
    c.forcedVerb = kNone;
    c.forcedVoice = 0;
    c.forcedLight = false;
    // C25: "had a good time" left word for word (no manner row for its adjective) is a calque: never OK
    if (!light && f.hasObject && !f.object.adjectives.empty() &&
        in(text::lower(f.pred.lemma), {"have", "tener", "pasar"}) &&
        in(text::lower(f.object.head), {"time", "tiempo", "rato"}) &&
        std::find(c.out.flags.begin(), c.out.flags.end(), "light-verb") == c.out.flags.end()) {
      c.out.flags.push_back("light-verb");
      c.out.notes.push_back(rules::Reason{-1, "form", "\"" + f.pred.lemma + " a ... " + f.object.head +
                                                        "\" rendered word for word (an idiom): check it", ""});
    }
    c.verb = cl.pred.modal != kNone ? kNone : cl.pred.lemma;
    if (light) {   // frame obj:<prep>: that PP's noun is the object of the Greek verb
      const size_t op = light->frame.find("obj:");
      if (op != std::string::npos) {
        std::string pr = light->frame.substr(op + 4);
        const size_t e = pr.find_first_of(" ,;");
        if (e != std::string::npos) pr.resize(e);
        c.objPrep = pr;
      }
    }
  }
  if (!f.hasPred && f.type != Kind::Frag && f.type != Kind::Excl) cl.type = ClauseType::Frag;
  // imperatives: number (imp.number) and aspect (decision 8: aorist for a single act, present for continuing or
  // general commands and for prohibitions)
  if (f.type == Kind::Imp) {
    bool pl = f.imperativePlural;
    // C21: a Spanish command whose verb form is only singular ("No comas el pan": comas = 2 sg) is singular
    if (pl && c.st.lang == frame::SrcLang::Es && f.pred.token >= 0 && (size_t)f.pred.token < c.s.tokens.size()) {
      uint8_t ip = 0, in_ = 0;
      if (f.pred.auxTokens.empty() && esPersonNumber(c.st.srcLex, c.s.tokens[(size_t)f.pred.token].lower, ip, in_) &&
          ip == P2 && in_ == Sg)
        pl = false;
    }
    if (!pl && c.mem.addresseePlural) { pl = true; c.mem.addresseeGuess = true; }
    if (pl) { cl.pred.number = Pl; c.mem.sawPlural = true; }
    const bool dur = state || f.negative || f.copula ||
                     durative(f.pred.lemma, cl.pred.modal != kNone ? cl.pred.modal : cl.pred.lemma, c.st);
    if (cl.pred.modal == kNone) cl.pred.tense = dur ? (uint8_t)Present : (uint8_t)Aorist;
  }
  // object
  // a reflexive pronoun object ("se fueron", "inclínense") is the middle voice in Greek or nothing at all
  const bool reflexiveObj = reflexiveClitic(f, c.st.lang == frame::SrcLang::Es);   // C25: also "nos divertimos"
  if (reflexiveObj) c.cover(f.object.tokens);
  if (f.hasObject && !stateObj && !reflexiveObj && !light &&
      !(f.type == Kind::Wh && f.wh.role == frame::Role::Object && f.object.isPronoun && f.object.interrogative)) {
    cl.hasObject = true;
    npInto(f.object, c, cl.object);
    // C16: a verb whose valency_grc.tsv frames are "dat;acc" (πιστεύω): the person in the dative, a thing in the
    // accusative ("ἓξ ἀδύνατα πιστεύω")
    if (cl.pred.lemma != kNone && cl.pred.modal == kNone && !transfer::animate(f.object) && !f.object.isPronoun)
      if (const Valency* v = gd_.valency(lx_.lemma(cl.pred.lemma).key)) {
        bool dat = false, acc = false;
        for (const Frame& fr : v->frames) {
          if (fr.middle) continue;
          dat = dat || (fr.kind == FrameKind::Dat && !acc);
          acc = acc || fr.kind == FrameKind::Acc;
        }
        if (dat && acc) cl.object.case_ = Acc;
      }
  }
  if (f.hasIndirect) {
    cl.hasIndirect = true;
    npInto(f.indirectObject, c, cl.indirect);
  }
  // C18: the Spanish possessive dative with a body part ("le cortaron la cabeza" -> τὴν κεφαλὴν αὐτοῦ ἀπέτεμον): the
  // dative pronoun becomes the possessor of the object
  if (c.st.lang == frame::SrcLang::Es && cl.hasIndirect && cl.indirect.isPronoun && cl.indirect.pron.person == 3 &&
      cl.hasObject && !cl.object.isPronoun && !cl.object.possPerson && cl.object.genitive.empty() && f.hasObject &&
      in(text::lower(f.object.head), {"cabeza", "mano", "pie", "cara", "pelo", "cabello", "ojo", "boca", "brazo",
                                      "pierna", "oreja", "nariz", "cuello", "espalda", "diente", "dedo", "rostro"})) {
    cl.object.possPerson = 3;
    cl.object.possNumber = cl.indirect.pron.number ? cl.indirect.pron.number : (uint8_t)Sg;
    cl.object.possGender = cl.indirect.pron.gender ? simpleGender(cl.indirect.pron.gender) : (uint8_t)M;
    cl.object.definite = true;
    cl.hasIndirect = false;
  }
  for (GrcClause& ic : c.pendingInf) {   // C18: the bare infinitive of purpose after the object
    GrcSub gs;
    gs.rel = SubRel::AccInf;
    gs.clause.push_back(std::move(ic));
    cl.subs.push_back(std::move(gs));
  }
  c.pendingInf.clear();
  for (const frame::SemOblique& o : f.obliques) {
    if (state && !stateObj && (o.prep == "of" || o.prep == "about") && !cl.hasObject) {   // "afraid of the dark"
      cl.hasObject = true;
      npInto(o.np, c, cl.object);
      c.cover(o.token);
      continue;
    }
    obliqueInto(o, c, cl);
  }
  // copula predicate
  if (f.copula && !state) {
    for (const SemNP& pn : f.predicative) {
      if (f.type == Kind::Wh && f.wh.role == frame::Role::Predicate && pn.isPronoun && pn.interrogative) {
        c.cover(pn.tokens);
        continue;
      }
      // C25: a possessive pronoun as the predicate ("This book is mine." was "[mine]", Fix): mine / yours / ours ->
      // ἐμός / σός (ὑμέτερος for several addressees) / ἡμέτερος agreeing with the subject (τοῦτο ἐμόν ἐστιν); his /
      // hers / theirs -> the genitive of ἐκεῖνος (ὁ κῆπος ἐκείνων ἐστίν)
      if ((pn.isPronoun || (pn.determiner.empty() && pn.adjectives.empty())) &&
          (in(text::lower(pn.pronLemma.empty() ? pn.head : pn.pronLemma),
              {"mine", "yours", "ours", "his", "hers", "theirs"}) || tokenLower(c.s, pn.token) == "hers")) {
        std::string pw = text::lower(pn.pronLemma.empty() ? pn.head : pn.pronLemma);
        if (tokenLower(c.s, pn.token) == "hers") pw = "hers";
        c.cover(pn.tokens);
        c.cover(pn.token);
        if (pw == "mine" || pw == "yours" || pw == "ours") {
          const char* g = pw == "mine" ? "ἐμός" : pw == "ours" ? "ἡμέτερος" : c.mem.addresseePlural ? "ὑμέτερος" : "σός";
          const uint32_t id = greek(g, Adj);
          if (id != kNone) {
            GrcAdj a;
            a.lemma = id;
            cl.predAdj.push_back(a);
            c.table(id, pw, pn.token, "possessive pronoun as the predicate");
            continue;
          }
        } else {
          const uint32_t id = greek("ἐκεῖνος");
          if (id != kNone) {
            GrcNP x;
            x.head = id;
            x.case_ = Gen;
            x.number = pw == "theirs" ? Pl : Sg;
            x.gender = pw == "hers" ? F : M;
            cl.predicative.push_back(x);
            c.table(id, pw, pn.token, "possessive pronoun as the predicate: genitive");
            continue;
          }
        }
      }
      GrcNP x;
      npInto(pn, c, x);
      // decision 9: a unique definite predicate (a title, "the X of Y") keeps the article
      if (pn.definite && (pn.title || pn.isName || !pn.genitive.empty() || !x.genitive.empty())) x.forceArticle = true;
      cl.predicative.push_back(x);
    }
    const bool impersIt = !f.hasSubject || f.pred.impersonal ||
                          (f.subject.isPronoun && (f.subject.pronLemma == "it" || f.subject.pronLemma == "ello"));
    for (const frame::SemAdj& a : f.predAdj) {
      Choice ch;
      ch.token = a.token;
      // C18 (lexical_en_grc.tsv kind state, frame adv): "it is late" -> ὀψέ ἐστιν (an adverb with the impersonal copula)
      if (impersIt && f.predAdj.size() == 1 && a.adverbs.empty()) {
        const LexRow* sv = gt_.find("state", text::lower(a.lemma), "adv");
        if (!sv) { const std::string ea = english(a.lemma, c.st); if (!ea.empty()) sv = gt_.find("state", ea, "adv"); }
        const uint32_t av = sv ? greek(sv->greek.c_str(), Adv) : kNone;
        if (av != kNone) {
          cl.adverbs.push_back(GrcAdverb{av, AdvPos::BeforeVerb});
          c.table(av, a.lemma, a.token, "lexical_en_grc.tsv: impersonal");
          c.cover(a.token);
          if (f.hasSubject && f.subject.isPronoun) cl.hasSubject = false;
          continue;
        }
      }
      GrcAdj ga;
      ga.degree = a.degree;
      const LexRow* sa = gt_.find("state", text::lower(a.lemma), "adj");
      if (sa && (ga.lemma = lexRowLemma(sa, Adj)) != kNone) c.table(ga.lemma, a.lemma, a.token, "lexical_en_grc.tsv");
      else {
        ga.lemma = select(a.lemma, Adj, c.context, false, false, c.st, ch);
        c.out.choices.push_back(ch);
      }
      c.cover(a.token);
      if (ga.lemma == kNone) { c.out.unknownWords.push_back(a.lemma); continue; }
      for (size_t i = 0; i < a.adverbs.size(); ++i) {
        const uint32_t av = adverb(a.adverbs[i], i < a.advTokens.size() ? a.advTokens[i] : -1, c, false);
        if (av != kNone) ga.adverbs.push_back(av);
      }
      cl.predAdj.push_back(ga);
    }
    const bool itSubj = f.hasSubject && f.subject.isPronoun && (f.subject.pronLemma == "it" || f.subject.pronLemma == "ello");
    if (!cl.predAdj.empty() && (!f.hasSubject || f.pred.impersonal || (itSubj && !c.mem.lastGender))) {
      cl.predGender = N;
      if (itSubj) { cl.subject.pron.gender = N; cl.subject.gender = N; }
    }
  }
  // C18: "where is your mother?" (the noun parsed as a predicate): the noun is the subject of the existential order,
  // with its article ("ποῦ ἐστιν ἡ μήτηρ σου;")
  if (f.type == Kind::Wh && f.copula && !cl.hasSubject && cl.predicative.size() == 1 && cl.predAdj.empty() &&
      cl.predicative[0].interrogative == kNone && !cl.predicative[0].isPronoun &&
      (text::lower(f.wh.word) == "where" || text::lower(f.wh.word) == "dónde")) {
    cl.subject = cl.predicative[0];
    cl.subject.forceArticle = false;
    cl.subject.definite = cl.subject.definite || cl.subject.possPerson || !f.predicative[0].possessor.empty() ||
                          f.predicative[0].definite;
    cl.hasSubject = true;
    cl.predicative.clear();
  }
  // C16: an elliptical "it is" (nothing predicated) is the existential, orthotone ἔστι ("πιστεύεις ὅτι ἔστιν")
  {
    const std::string pl = text::lower(f.pred.lemma);
    const bool be = pl == "be" || pl == "ser" || pl == "estar";
    const bool itSubj = !f.hasSubject || (f.subject.isPronoun && (f.subject.pronLemma == "it" || f.subject.pronLemma == "that" ||
                                                                   f.subject.pronLemma == "ello" || f.subject.pronLemma == "eso"));
    if (be && f.hasPred && f.type == Kind::Decl && !state && itSubj && cl.predicative.empty() && cl.predAdj.empty() &&
        cl.obliques.empty() && cl.adverbs.empty() && !cl.hasObject && cl.pred.modal == kNone && f.subordinate.empty())
      cl.existential = true;
  }
  // fragment adjectives ("Very strange!")
  if (!f.hasPred && !f.predAdj.empty()) {
    for (const frame::SemAdj& a : f.predAdj) {
      Choice ch;
      ch.token = a.token;
      GrcAdj ga;
      ga.lemma = select(a.lemma, Adj, c.context, false, false, c.st, ch);
      c.out.choices.push_back(ch);
      c.cover(a.token);
      if (ga.lemma == kNone) { c.out.unknownWords.push_back(a.lemma); continue; }
      for (size_t i = 0; i < a.adverbs.size(); ++i) {
        const uint32_t av = adverb(a.adverbs[i], i < a.advTokens.size() ? a.advTokens[i] : -1, c, false);
        if (av != kNone) ga.adverbs.push_back(av);
      }
      cl.predAdj.push_back(ga);
      cl.predGender = N;
    }
  }
  // exclamations: "What a strange garden!" -> ὡς θαυμαστὸς ὁ κῆπος (order.excl)
  if (f.type == Kind::Excl) {
    cl.exclHos = cl.hasSubject || !cl.predAdj.empty();
    if (cl.hasSubject) cl.subject.definite = true;
  }
  // vocatives, interjections, adverbs, discourse, connectors
  for (const SemNP& v : f.vocatives) {
    GrcNP x;
    npInto(v, c, x);
    // "¡Qué jardín tan extraño!" read as an address: ὡς θαυμαστὸς ὁ κῆπος!
    if (!f.hasPred && !cl.hasSubject && (v.determiner == "what" || v.determiner == "qué") && !v.adjectives.empty() &&
        x.head != kNone) {
      cl.type = ClauseType::Excl;
      cl.exclHos = true;
      cl.hasSubject = true;
      x.definite = true;
      for (GrcAdj& a : x.adjectives) a.adverbs.clear();   // "tan" is in ὡς already
      cl.subject = x;
      cl.punct = "!";
      continue;
    }
    x.definite = false;
    if (!x.possEmphatic) x.possPerson = 0;   // C18: "my child" -> ὦ παῖ (no μου with a vocative)
    cl.vocatives.push_back(x);
    if (x.number == Pl) c.mem.sawPlural = true;
  }
  for (const std::string& ij : f.interjections) {
    const std::string w = text::lower(ij);
    const char* g = w == "alas" || w == "ay" ? "φεῦ" : (w == "oh" || w == "o") && !cl.vocatives.empty() ? nullptr : nullptr;
    if (g) {
      const uint32_t id = greek(g);
      if (id != kNone) { cl.interjections.push_back(id); continue; }
    }
    if (!f.hasPred && f.predAdj.empty() && !f.hasSubject && f.adverbs.empty() && f.vocatives.empty())
      c.out.unknownWords.push_back(ij);
  }
  for (const frame::SemAdverb& a : f.adverbs) {
    // C21: a determiner never stands before an adverb: "the water of the well" read with "well" as an adverb (εὖ)
    // is a misparse of a noun; the cue is never OK
    // C25: "the water of the well": a determiner after "of" / "de" before a word with a Greek noun reading: that noun
    // as the genitive attribute of the NP before "of" (τὸ ὕδωρ τοῦ φρέατος); still Check (det-adverb)
    if (a.token > 1 && in(tokenLower(c.s, a.token - 1), {"the", "a", "an", "el", "la", "los", "las", "un", "una"}) &&
        in(tokenLower(c.s, a.token - 2), {"of", "de", "del"})) {
      GrcNP* host = nullptr;
      const int before = a.token - 3;
      if (cl.hasObject && f.hasObject)
        for (int t : f.object.tokens) host = t == before ? &cl.object : host;
      if (!host && cl.hasObject && f.hasObject && f.object.token == before) host = &cl.object;
      if (!host && cl.hasSubject && f.hasSubject && (f.subject.token == before ||
          std::find(f.subject.tokens.begin(), f.subject.tokens.end(), before) != f.subject.tokens.end()))
        host = &cl.subject;
      if (host && host->genitive.empty()) {
        Choice ch;
        ch.token = a.token;
        const uint32_t nid = select(a.lemma, Noun, c.context, false, false, c.st, ch);
        if (nid != kNone) {
          c.out.choices.push_back(ch);
          GrcNP g;
          g.head = nid;
          g.definite = in(tokenLower(c.s, a.token - 1), {"the", "el", "la", "los", "las"});
          g.number = Sg;
          host->genitive.push_back(g);
          c.cover(a.token);
          c.cover(a.token - 1);
          c.cover(a.token - 2);
          if (std::find(c.out.flags.begin(), c.out.flags.end(), "det-adverb") == c.out.flags.end())
            c.out.flags.push_back("det-adverb");
          c.out.notes.push_back(rules::Reason{-1, "form", "\"" + a.lemma + "\" after \"of the\" read as a noun: check it", ""});
          continue;
        }
      }
    }
    if (a.token > 0 && in(tokenLower(c.s, a.token - 1), {"the", "a", "an", "these", "those", "my", "your",
                                                          "his", "its", "our", "their", "el", "la", "los", "las", "un", "una"}) &&
        std::find(c.out.flags.begin(), c.out.flags.end(), "det-adverb") == c.out.flags.end()) {
      c.out.flags.push_back("det-adverb");
      c.out.notes.push_back(rules::Reason{-1, "form", "\"" + a.lemma + "\" after a determiner was read as an adverb: check it", ""});
    }
    if (a.ellipticWh) {   // "I don't care where" -> ... ὅπου / ὅποι
      const char* w = a.lemma == "where" ? (c.mem.lastMotion || c.motion ? "ὅποι" : "ὅπου")
                    : a.lemma == "when" ? "ὁπότε" : a.lemma == "why" ? "διότι" : "ὅπως";
      const uint32_t id = greek(w);
      c.cover(a.token);
      if (id != kNone) { cl.adverbs.push_back(GrcAdverb{id, AdvPos::End}); c.table(id, a.lemma, a.token, "elliptical indirect question"); }
      continue;
    }
    // C25: a light row of frame "manner" ("lo pasamos (muy) bien" -> (πάνυ) ἡδέως διηγάγομεν): its adverbs by kind manner
    if (c.mannerLight) {
      bool done = false;
      const std::vector<std::string> gw = words(text::lower(a.lemma));
      for (const std::string& w : gw) done = done || gt_.find("manner", w) != nullptr;
      if (done) {
        for (size_t k = 0; k < gw.size(); ++k) {
          const int tok = a.token - (int)(gw.size() - 1 - k);
          if (const LexRow* r = gt_.find("manner", gw[k])) {
            for (const std::string& g : words(r->greek)) {
              uint32_t id = greek(g.c_str(), Adv);
              if (id == kNone) id = adjAdverb(g.c_str());
              if (id != kNone) { cl.adverbs.push_back(GrcAdverb{id, AdvPos::BeforeVerb}); c.table(id, gw[k], tok, "manner (lexical_en_grc.tsv)"); }
            }
            c.cover(tok);
          } else {
            const uint32_t id = adverb(gw[k], tok, c, false);
            if (id != kNone) cl.adverbs.push_back(GrcAdverb{id, AdvPos::BeforeVerb});
          }
        }
        continue;
      }
    }
    // C18: a two-word adverb group the frame keeps as one lemma ("so fast" -> οὕτω ταχέως)
    if (a.lemma.find(' ') != std::string::npos && !adverbTable(a.lemma, false)) {
      const std::vector<std::string> gw = words(text::lower(a.lemma));
      for (const std::string& w : gw) {
        const uint32_t wid = adverb(w, a.token, c, c.motion);
        if (wid != kNone) cl.adverbs.push_back(GrcAdverb{wid, a.front ? AdvPos::Front : AdvPos::Auto});
      }
      // C21: the group's other words are rendered too ("so fast": the token of "so" is accounted for)
      for (size_t k = 1; k < gw.size() && a.token - (int)k >= 0; ++k)
        if (tokenLower(c.s, a.token - (int)k) == gw[gw.size() - 1 - k]) c.cover(a.token - (int)k);
      continue;
    }
    // C25: a degree word right before a single adverb that the frame builder left out ("corrió tan rápido que",
    // "muy despacio"): rendered before it (οὕτω ταχέως, πάνυ βραδέως)
    int degTok = -1;
    if (a.token > 0) {
      const std::string& pw = tokenLower(c.s, a.token - 1);
      bool listed = false;
      for (const frame::SemAdverb& b : f.adverbs) listed = listed || b.token == a.token - 1;
      if (!listed && in(pw, {"tan", "muy", "demasiado", "so", "very", "too"}) &&
          std::find(c.out.covered.begin(), c.out.covered.end(), a.token - 1) == c.out.covered.end())
        degTok = a.token - 1;
    }
    const uint32_t id = adverb(a.lemma, a.token, c, c.motion);
    if (id == kNone) continue;
    if (degTok >= 0) {
      const uint32_t dg = adverb(tokenLower(c.s, degTok), degTok, c, false);
      if (dg != kNone) cl.adverbs.push_back(GrcAdverb{dg, a.front ? AdvPos::Front : AdvPos::Auto});
    }
    // C16: "first ... then": πρῶτον ... ἔπειτα, both at the front
    const bool seq = a.lemma == "first" || a.lemma == "primero";
    if (seq) c.mem.sawFirst = true;
    // C16: an adverb right after a subject NP modifies it ("Everyone here is mad" -> πάντες ἐνθάδε μαίνονται)
    int subjLast = f.hasSubject ? f.subject.token : -1;
    if (f.hasSubject)
      for (int t : f.subject.tokens) subjLast = std::max(subjLast, t);
    const bool postSubj = !seq && !a.front && f.hasSubject && subjLast >= 0 && a.token == subjLast + 1 &&
                          f.type == Kind::Decl && (!f.subject.isPronoun || f.subject.determiner == "all" ||
                                                   f.subject.pronLemma == "everyone" || f.subject.pronLemma == "everybody" ||
                                                   f.subject.pronLemma == "all");
    cl.adverbs.push_back(GrcAdverb{id, a.front || seq ? AdvPos::Front : postSubj ? AdvPos::BeforeVerb : AdvPos::Auto});
  }
  for (const std::string& d : f.discourse)
    if (d == "please" || d == "por favor") {
      if (c.st.fidelity >= 3) {   // decision 2: dropped in flexible mode
        c.out.notes.push_back(rules::Reason{-1, "sense", "\"please\" left out (flexible mode: the command says it)", ""});
        continue;
      }
      const uint32_t q = greek("ἀντιβολέω", Verb);
      if (q != kNone) cl.politeness.push_back(q);
    }
  for (const std::string& k0 : f.connectors) {
    const std::string k = text::lower(k0);
    const char* g = nullptr;
    if (k == "and" || k == "y") g = "καί";
    else if (k == "but" || k == "pero") g = f.negative ? "ἀλλά" : "δέ";
    else if (k == "or" || k == "o") g = "ἤ";
    else if ((k == "then" || k == "next" || k == "luego" || k == "después") && c.mem.prevFirst && f.type == Kind::Imp) {
      const uint32_t ep = greek("ἔπειτα", Adv);   // C16: "First write your name. Then write the date."
      if (ep != kNone) {
        cl.adverbs.insert(cl.adverbs.begin(), GrcAdverb{ep, AdvPos::Front});
        c.table(ep, k0, -1, "connector after \"first\"");
        continue;
      }
    }
    else if (k == "so" || k == "then" || k == "therefore" || k == "entonces" || k == "pues") g = "οὖν";
    else if (k == "because" || k == "porque") g = "ὅτι";
    else if (k == "now") g = "νῦν";
    else if (k == "also") g = "καί";
    if (!g) continue;
    uint32_t id = greek(g, Particle);
    if (id == kNone) id = greek(g);
    if (id == kNone) continue;
    if (std::string(g) == "νῦν") cl.adverbs.insert(cl.adverbs.begin(), GrcAdverb{id, AdvPos::Front});
    else cl.connectors.push_back(id);
    c.table(id, k0, -1, "connector");
  }
  // an elliptical clause with a demonstrative subject contrasts with the previous one: "This one does." -> αὕτη δὲ
  if (f.pred.ellipsis && cl.hasSubject && !cl.subject.isPronoun && cl.connectors.empty() && cl.subject.head != kNone &&
      lx_.lemma(cl.subject.head).key == std::string("οὗτοσ")) {
    const uint32_t de = greek("δέ", Particle) != kNone ? greek("δέ", Particle) : greek("δέ");
    if (de != kNone) cl.connectors.push_back(de);
  }
  // wh word
  if (f.type == Kind::Wh) {
    const std::string w = text::lower(f.wh.word);
    const char* g = nullptr;
    // C21: verbs of placing ask "where" with ποῦ (the place where the thing lies: "ποῦ ἔθηκας;")
    if (w == "where") g = c.motion && !in(text::lower(f.pred.lemma), {"put", "place", "lay", "leave", "poner", "colocar", "dejar"}) ? "ποῖ" : "ποῦ";
    else if (w == "whither") g = "ποῖ";
    else if (w == "whence") g = "πόθεν";
    else if (w == "how") g = "πῶς";
    else if (w == "when") g = "πότε";
    if (g) {
      cl.wh.lemma = greek(g, Adv);
      if (cl.wh.lemma == kNone) cl.wh.lemma = greek(g);
      c.cover(f.wh.token);
      if (cl.wh.lemma != kNone) c.table(cl.wh.lemma, f.wh.word, f.wh.token);
    } else if (w == "why") {   // διὰ τί
      GrcOblique o;
      o.prep = greek("διά", Prep);
      o.case_ = Acc;
      o.np.head = greek("τίς", Pron);
      o.np.gender = N;
      o.np.case_ = Acc;
      if (o.prep != kNone && o.np.head != kNone) { cl.obliques.insert(cl.obliques.begin(), o); c.table(o.prep, "why", f.wh.token); }
      c.cover(f.wh.token);
    } else if ((w == "who" || w == "what") &&
               !((f.wh.role == frame::Role::Subject && f.hasSubject && !f.subject.isPronoun) ||
                 (f.wh.role == frame::Role::Object && f.hasObject && !f.object.isPronoun) ||
                 // C16: "What day is it today?": the interrogative is the predicate NP's determiner (τίς ἡμέρα)
                 (f.wh.role == frame::Role::Predicate && !f.predicative.empty() && !f.predicative[0].isPronoun &&
                  f.predicative[0].interrogative))) {
      cl.wh.lemma = greek("τίς", Pron);
      cl.wh.gender = w == "what" ? N : M;
      cl.wh.role = f.wh.role == frame::Role::Subject ? Role::Subject
                 : f.wh.role == frame::Role::Object ? Role::Object
                 : f.wh.role == frame::Role::Predicate ? Role::Predicate : Role::None;
      if (cl.wh.role == Role::None) cl.wh.role = Role::Predicate;
      // "who are you?" / "¿quién eres?": with the copula the wh word is the predicate (nominative)
      const std::string pl0 = text::lower(f.pred.lemma);
      if ((f.copula || pl0 == "be" || pl0 == "ser" || pl0 == "estar") && cl.wh.role == Role::Object)
        cl.wh.role = Role::Predicate;
      if (f.wh.role == frame::Role::Subject) cl.hasSubject = false;
      if (f.wh.role == frame::Role::Object && f.hasObject && f.object.isPronoun) cl.hasObject = false;
      c.cover(f.wh.token);
      if (cl.wh.lemma != kNone) c.table(cl.wh.lemma, f.wh.word, f.wh.token);
    }
  }
  // "where is the ball?" / "¿dónde está la pelota?": be without a predicate is the existential order (V S)
  if (f.type == Kind::Wh && cl.predicative.empty() && cl.predAdj.empty() && cl.pred.modal == kNone &&
      cl.pred.lemma != kNone && lx_.lemma(cl.pred.lemma).key == std::string("εἰμί") && cl.hasSubject)
    cl.existential = true;
  // subordinate clauses
  for (const frame::SemSub& sb : f.subordinate) {
    if (sb.frame.empty()) continue;
    const SemFrame& sf = sb.frame[0];
    GrcSub gs;
    gs.before = sb.before;
    GrcClause sc;
    const int dep = sb.relation == Relation::Coord ? 0 : 1;   // a coordinated clause is not dependent
    c.depth += dep;
    const bool keepAnim = c.animateIt;
    c.animateIt = f.hasSubject && !f.subject.isPronoun && transfer::animate(f.subject);
    clauseInto(sf, c, sc);
    c.animateIt = keepAnim;
    c.depth -= dep;
    c.frame = &f;
    const std::string mk = text::lower(sb.marker);
    auto conj = [&](const char* h) { uint32_t id = greek(h, Conj); return id != kNone ? id : greek(h); };
    // C18: the main clause's subject in Greek terms (number, gender) for a participle agreeing with it
    auto mainAgree = [&](uint8_t& num, uint8_t& gen) -> bool {
      if (!cl.hasSubject || !cl.subject.coord.empty() || !cl.subject.literal.empty()) return false;
      const GrcNP& n = cl.subject;
      if (n.isPronoun) {
        num = n.pron.number ? n.pron.number : (uint8_t)Sg;
        gen = n.pron.person <= 2 && num == Pl ? (uint8_t)M : simpleGender(n.pron.gender ? n.pron.gender : n.gender);
        return true;
      }
      num = n.number ? n.number : (uint8_t)Sg;
      if (n.gender) gen = simpleGender(n.gender);
      else if (n.isName && n.name.size()) { const NameEntry* e = gd_.nameByEnglish(n.name); gen = e ? simpleGender(e->gender) : (uint8_t)M; }
      else if (n.head != kNone) gen = simpleGender(lx_.lemma(n.head).gender);
      else return false;
      return true;
    };
    // C18: is the dependent clause's subject the main clause's? 0 no, 1 yes, 2 yes and the dependent clause names it
    // ("When the girl saw the cat, she laughed": ἡ κόρη becomes the main subject)
    auto sameSubject = [&](bool ing) -> int {
      if (!sf.hasSubject) return ing && f.hasSubject ? 1 : 0;
      if (!f.hasSubject) return 0;
      const SemNP& a = f.subject;
      const SemNP& b = sf.subject;
      if (personalPron(a) && personalPron(b)) {
        if (a.pron.person != b.pron.person) return 0;
        if (a.pron.number && b.pron.number && a.pron.number != b.pron.number) return 0;
        if (a.pron.person == 3 && a.pron.gender && b.pron.gender && a.pron.gender != b.pron.gender) return 0;
        return 1;
      }
      if (!a.isPronoun && !b.isPronoun && !a.isName && !b.isName)
        return text::lower(a.head) == text::lower(b.head) && a.number == b.number && a.coord.empty() ? 1 : 0;
      auto npGender = [&](const GrcNP& g) -> uint8_t {
        if (g.gender) return simpleGender(g.gender);
        if (g.isName) { const NameEntry* e = gd_.nameByEnglish(g.name); return e ? simpleGender(e->gender) : 0; }
        return g.head != kNone ? simpleGender(lx_.lemma(g.head).gender) : 0;
      };
      if (personalPron(a) && a.pron.person == 3 && !b.isPronoun && sb.before && b.coord.empty()) {
        if ((a.pron.number == 2) != (b.number == 2)) return 0;
        const uint8_t bg = npGender(sc.subject);
        if (!bg || (a.pron.gender && simpleGender(a.pron.gender) != bg)) return 0;
        return 2;
      }
      if (!a.isPronoun && personalPron(b) && b.pron.person == 3 && a.coord.empty()) {
        if ((b.pron.number == 2) != (a.number == 2)) return 0;
        const uint8_t ag = npGender(cl.subject);
        if (!ag || (b.pron.gender && simpleGender(b.pron.gender) != ag)) return 0;
        return 1;
      }
      return 0;
    };
    // C21: "It is snowing, so we cannot go out.": a clause after the main one led by "so" (the frame builder may make
    // it a time clause with an invented "when", or a coordinated one) is the consequence: ὥστε + the indicative (an
    // actual result, οὐ): "νίφει, ὥστε οὐ δυνάμεθα ἐξελθεῖν"
    bool soResult = false;
    if (!sb.before && (sb.relation == Relation::Time || sb.relation == Relation::Coord)) {
      bool soConn = false;
      for (const std::string& k : sf.connectors) soConn = soConn || in(text::lower(k), {"so", "así"});
      bool mkSeen = false;
      for (const nlp::Token& t : c.s.tokens) mkSeen = mkSeen || (!mk.empty() && t.lower == mk);
      soResult = soConn && !mkSeen;
    }
    if (soResult) {
      gs.rel = SubRel::Result;
      gs.finite = true;
      const uint32_t oun = greek("οὖν", Particle) != kNone ? greek("οὖν", Particle) : greek("οὖν");
      sc.connectors.erase(std::remove(sc.connectors.begin(), sc.connectors.end(), oun), sc.connectors.end());
    } else
    switch (sb.relation) {
      case Relation::Cause:
        gs.rel = SubRel::Cause;
        if (mk == "since" || mk == "as") gs.conj = conj("ἐπεί");
        break;
      case Relation::Time:
        gs.rel = SubRel::Time;
        if (mk == "before" || mk == "antes") {
          gs.conj = conj("πρίν");
          // C16 (order.prin): after an affirmative main clause πρίν + infinitive, its own subject in the accusative
          // after the verb ("τρέχωμεν πρὶν ἥκειν αὐτήν"); the same subject as the main clause is left out
          if (!f.negative && sc.type == ClauseType::Decl && sc.pred.lemma != kNone) {
            gs.rel = SubRel::AccInf;
            sc.verbFirst = true;
            if (sc.hasSubject && sc.subject.isPronoun) {
              const bool same = cl.hasSubject && cl.subject.isPronoun && cl.subject.pron.person == sc.subject.pron.person &&
                                cl.subject.pron.number == sc.subject.pron.number;
              if (same) sc.hasSubject = false;
              else sc.subject.emphasis = true;
            }
          }
        }
        else {
          // C18: a circumstantial participle agreeing with the subject where Attic prefers it: an English -ing adjunct
          // ("Seeing the wolf, the shepherd ran away" -> ὁ ποιμὴν ὁρῶν τὸν λύκον ἔφυγεν: present, simultaneous) and a
          // when / after / while clause with the main clause's subject ("When the girl saw the cat, she laughed" -> ἡ
          // κόρη ἰδοῦσα τὴν γαλῆν ἐγέλασεν: aorist for a prior event, present for while / as)
          const bool ing = !sf.hasSubject && ingForm(tokenLower(c.s, sf.pred.token));
          uint8_t pt = 0;
          const bool plain = sf.hasPred && !sf.copula && sf.predAdj.empty() && sf.predicative.empty() && !sf.negative &&
                             sf.pred.modality == Modality::None && sf.subordinate.empty() && sc.pred.modal == kNone &&
                             sc.pred.lemma != kNone && sc.type == ClauseType::Decl && f.type == Kind::Decl &&
                             sf.pred.tense != frame::Tense::Future;
          const bool mainImpers = cl.pred.modal != kNone &&
                                  (lx_.lemma(cl.pred.modal).key == std::string("δεῖ") ||
                                   lx_.lemma(cl.pred.modal).key == std::string("ἔξεστι") ||
                                   lx_.lemma(cl.pred.modal).key == std::string("χρή"));
          if (plain && !mainImpers) {
            if (ing && (mk.empty() || mk == "when" || mk == "while" || mk == "cuando" || mk == "mientras")) pt = Present;
            else if (mk == "while" || mk == "as" || mk == "mientras") pt = Present;
            else if ((mk == "when" || mk == "after" || mk == "cuando" || mk == "después") &&
                     sf.pred.tense == frame::Tense::Past)
              pt = sf.pred.aspect == frame::Aspect::Progressive ? (uint8_t)Present : (uint8_t)Aorist;
          }
          const int same = pt ? sameSubject(ing) : 0;
          if (same) {
            const GrcNP keepSubj = cl.subject;
            const bool keepHas = cl.hasSubject;
            if (same == 2) { cl.subject = sc.subject; cl.hasSubject = true; }
            uint8_t num = Sg, gen = M;
            std::string probe;
            if (mainAgree(num, gen) &&
                participle(lx_, sc.pred.lemma, pt, sc.pred.voice, Nom, num, gen, probe)) {
              gs.participle = true;
              gs.conj = kNone;
              // the subordinator is dropped and the phrase rebuilt as a participle: Check, never OK
              if (std::find(c.out.flags.begin(), c.out.flags.end(), "participle-phrase") == c.out.flags.end())
                c.out.flags.push_back("participle-phrase");
              sc.hasSubject = false;
              sc.pred.tense = pt;
              sc.connectors.clear();
              c.table(sc.pred.lemma, sf.pred.lemma, sf.pred.token,
                      pt == Aorist ? "circumstantial participle (aorist: prior event)"
                                   : "circumstantial participle (present: at the same time)");
              break;
            }
            cl.subject = keepSubj;
            cl.hasSubject = keepHas;
          }
          if (mk == "while" || mk == "until") gs.conj = conj("ἕως");
          else if (mk == "when" && sf.pred.tense != frame::Tense::Past) gs.conj = conj("ὅτε");
        }
        break;
      case Relation::Condition:
        gs.rel = SubRel::Condition;
        // future / general conditions: ἐάν + subjunctive; others εἰ + indicative
        if (sf.pred.tense == frame::Tense::Future || f.type == Kind::Imp || f.pred.tense == frame::Tense::Future) {
          sc.pred.mood = Subjunctive;
          // C16: ἐάν + aorist subjunctive for a single event ("ἐὰν λευκὰ ῥόδα ἴδῃ"), present for a lasting one
          if (sc.pred.tense == Future || sc.pred.tense == Present)
            sc.pred.tense = durative(sf.pred.lemma, sc.pred.lemma, c.st) ? Present : Aorist;
        }
        if (mk == "unless") sc.polarity = sc.polarity == Polarity::Neg ? Polarity::Pos : Polarity::Neg;
        break;
      case Relation::Purpose:
        gs.rel = SubRel::Purpose;
        // C18: "so that the cat can come in" -> ἵνα ἡ γαλῆ εἰσέλθῃ: the subjunctive says "can / may" already
        if (sc.pred.modal != kNone && (sf.pred.modality == Modality::Can || sf.pred.modality == Modality::May)) {
          sc.pred.modal = kNone;
          sc.pred.voice = sc.pred.infVoice;
        }
        if (sc.pred.modal == kNone)
          sc.pred.tense = durative(sf.pred.complementVerb.empty() ? sf.pred.lemma : sf.pred.complementVerb, sc.pred.lemma,
                                   c.st) ? Present : Aorist;
        // C18: no subject of its own: the main clause's person and number ("I came to see you" -> ἵνα σε ἴδω)
        if (!sc.hasSubject && !sf.hasSubject && !sc.pred.person) {
          if (cl.type == ClauseType::Imp) {
            sc.pred.person = 2;
            sc.pred.number = cl.pred.number ? cl.pred.number : (uint8_t)Sg;
          } else if (cl.hasSubject && cl.subject.isPronoun) {
            sc.pred.person = cl.subject.pron.person ? cl.subject.pron.person : 3;
            sc.pred.number = cl.subject.pron.number ? cl.subject.pron.number : (uint8_t)Sg;
          } else if (cl.hasSubject && cl.subject.literal.empty()) {
            uint8_t num = Sg, gen = M;
            if (mainAgree(num, gen)) {
              sc.pred.person = 3;
              sc.pred.number = num == Pl && gen == N && cl.subject.coord.empty() ? (uint8_t)Sg : num;   // agree.neut.pl
            }
          }
        }
        break;
      case Relation::Result: gs.rel = SubRel::Result; break;
      case Relation::Concession: gs.rel = SubRel::Coord; gs.conj = conj("ἀλλά"); break;
      case Relation::Complement: {
        if (sf.type == Kind::Wh) { gs.rel = SubRel::IndirectQ; break; }
        // C18: "so cold that ...", "so fast that ...": a result clause, ὥστε + the indicative for an actual result (οὐ),
        // + the infinitive when the result is only possible ("so tired that he could not walk": ὥστε μὴ δύνασθαι ...)
        if ((soDegree(f) || soDegreeTokens(f, sf, c.s)) && (mk == "that" || mk == "que" || mk.empty())) {
          gs.rel = SubRel::Result;
          gs.finite = sf.pred.modality != Modality::Can && sf.pred.modality != Modality::May;
          break;
        }
        // C16: verbs of thinking (valency_grc.tsv acc+inf: οἴομαι, νομίζω) take accusative + infinitive
        // ("ᾤμην Σελήνης ἡμέραν εἶναι"); others ὅτι + indicative ("πιστεύεις ὅτι ἔστιν")
        bool accInf = false;
        const uint32_t mv = cl.pred.modal != kNone ? cl.pred.modal : cl.pred.lemma;
        if (mv != kNone)
          if (const Valency* v = gd_.valency(lx_.lemma(mv).key))
            accInf = !v->frames.empty() && v->frames[0].kind == FrameKind::AccInf;
        // C25: a verb of wanting whose first frame is the infinitive (βούλομαι, ἐθέλω): "quiero que vengas" -> βούλομαί
        // σε ἐλθεῖν (accusative + infinitive; ὅτι + indicative was wrong and rated OK)
        bool want = false;
        if (!accInf && mv != kNone && (mk == "that" || mk == "que"))
          if (const Valency* v = gd_.valency(lx_.lemma(mv).key))
            want = !v->frames.empty() && v->frames[0].kind == FrameKind::Inf && sf.type == Kind::Decl && sf.hasPred;
        if (want) {
          gs.rel = SubRel::AccInf;
          if (sc.pred.modal == kNone)
            sc.pred.tense = durative(sf.pred.lemma, sc.pred.lemma, c.st) ? Present : Aorist;
          // ἥκω (the dependent-clause row of "venir") has no Attic aorist: ἐλθεῖν
          if (sc.pred.tense == Aorist && sc.pred.lemma != kNone && lx_.lemma(sc.pred.lemma).key == std::string("ἥκω"))
            if (const uint32_t er = greek("ἔρχομαι", Verb); er != kNone) sc.pred.lemma = er;
          // the subject of the infinitive: a pronoun of another person is the object of the verb of wanting
          // (βούλομαί σε ἐλθεῖν, as the English "I want you to come"); a noun stays the accusative subject; the main
          // subject's own person is not repeated
          if (sc.hasSubject && sc.subject.isPronoun) {
            const uint8_t mp = cl.hasSubject && cl.subject.isPronoun ? cl.subject.pron.person : 3;
            const bool same = mp == sc.subject.pron.person &&
                              (!cl.hasSubject || !cl.subject.isPronoun || cl.subject.pron.number == sc.subject.pron.number);
            if (!same && !cl.hasObject) {
              cl.hasObject = true;
              cl.object = sc.subject;
              cl.object.case_ = Acc;
            }
            sc.hasSubject = false;
          } else if (!sc.hasSubject) {
            // "quiere que duerma": a different subject is implied but not said (duerma = 1st or 3rd person): Check
            if (std::find(c.out.flags.begin(), c.out.flags.end(), "subject-guess") == c.out.flags.end())
              c.out.flags.push_back("subject-guess");
            c.out.notes.push_back(rules::Reason{-1, "form", "the subject of the wished action is not expressed: check it", ""});
          }
        } else if (accInf) gs.rel = SubRel::AccInf;
        else { gs.rel = SubRel::Cause; gs.conj = conj("ὅτι"); }
        break;
      }
      case Relation::Coord: {
        gs.rel = SubRel::Coord;
        // C18: "Be quiet, the baby is sleeping": a statement after a command without a conjunction explains it (γάρ)
        const bool splice = mk.empty() && f.type == Kind::Imp && sf.type == Kind::Decl && sf.hasSubject && !sb.before;
        // C18: "We must hurry, or we will be late" -> "..., εἰ δὲ μή, ὑστερήσομεν" (or else after a command / must)
        if ((mk == "or" || mk == "o") && sf.type == Kind::Decl && sf.pred.tense == frame::Tense::Future &&
            (f.type == Kind::Imp || f.pred.modality == Modality::Must || f.pred.modality == Modality::Should)) {
          gs.otherwise = true;
          gs.noConj = true;
          const uint32_t e = conj("ἤ");
          sc.connectors.erase(std::remove(sc.connectors.begin(), sc.connectors.end(), e), sc.connectors.end());
          break;
        }
        if (mk == "for" || splice) {   // C18: "..., for it is late" -> ", ὀψὲ γάρ ἐστιν": γάρ second, no conjunction
          const uint32_t gar = greek("γάρ", Particle) != kNone ? greek("γάρ", Particle) : greek("γάρ");
          if (gar != kNone) {
            gs.noConj = true;
            sc.connectors.erase(std::remove(sc.connectors.begin(), sc.connectors.end(), gar), sc.connectors.end());
            sc.connectors.insert(sc.connectors.begin(), gar);
            c.table(gar, "for", -1, "connector");
            break;
          }
        }
        gs.conj = conj(mk == "but" || mk == "pero" ? "ἀλλά" : mk == "or" || mk == "o" ? "ἤ" : "καί");
        if ((mk == "or" || mk == "o") && sc.an) gs.otherwise = true;   // C16: εἰ δὲ μή, + counterfactual
        // the coordinating word is the conjunction already: not a connector of the clause too ("ἢ ἢ")
        sc.connectors.erase(std::remove(sc.connectors.begin(), sc.connectors.end(), gs.conj), sc.connectors.end());
        if (f.type == Kind::Imp && sc.type == ClauseType::Decl && !sc.hasSubject) {
          sc.type = ClauseType::Imp;
          sc.pred.tense = durative(sf.pred.lemma, sc.pred.lemma, c.st) ? Present : Aorist;
        }
        if (sc.type == ClauseType::Imp && cl.pred.number) sc.pred.number = cl.pred.number;
        break;
      }
      default: gs.rel = SubRel::Coord; gs.conj = conj("καί"); break;
    }
    gs.clause.push_back(std::move(sc));
    cl.subs.push_back(std::move(gs));
  }
  if (c.negative && !f.negative) cl.polarity = Polarity::Neg;
  // C21: ἀκούω takes the person heard in the genitive ("listen to me" -> ἄκουέ μου, "τοῦ πατρὸς ἀκούει"), the thing in
  // the accusative (valency_grc.tsv acc;gen, note "person in the genitive")
  if (cl.hasObject && !cl.object.case_ && cl.pred.lemma != kNone) {
    const std::string vk = std::string(lx_.lemma(cl.pred.lemma).key);
    const std::string ak = text::greek_key("ἀκούω");
    const bool akouo = vk.size() >= ak.size() && vk.compare(vk.size() - ak.size(), ak.size(), ak) == 0 && vk.size() <= ak.size() + 4;
    const bool person = (cl.object.isPronoun && cl.object.pron.person > 0) ||
                        (f.hasObject && transfer::animate(f.object)) ||
                        (!c.objPrep.empty() && [&] {
                          for (const frame::SemOblique& o : f.obliques)
                            if (o.prep == c.objPrep) return transfer::animate(o.np) || (o.np.isPronoun && o.np.pron.person > 0);
                          return false;
                        }());
    if (akouo && person && cl.pred.voice != Passive) cl.object.case_ = Gen;
  }
  c.frame = keepFrame;
  c.motion = keepMotion;
  c.negative = keepNeg;
  c.existential = keepExist;
  c.subjPerson = keepPerson;
  c.subjNumber = keepNumber;
  c.subjGender = keepGender;
  c.objPrep = keepObjPrep;
  c.prepOverride = keepPO;
  c.prepOverrideGreek = keepPOG;
  c.prepOverrideCase = keepPOC;
  c.startGender = keepStart;
  c.verb = keepVerb;
  c.mannerLight = keepManner;
  c.pendingInf.swap(keepPending);
}

namespace {
// C18: two analyses of the shared frame builder that the Greek path repairs for itself (recorded in STATUS C18,
// "API changes", as a wish for frame/): a sentence-initial "When X, Y." that is not a question comes as a wh question
// "when" with Y coordinated; "Singing a song, the girl walked ..." comes as an imperative "sing" with the real clause
// as a time clause after it. Both become the main clause Y with X as a time clause before it.
// C21: three readings of a nature / weather clause that the frame builder gets wrong, recognised through the
// lexical_en_grc.tsv rows (kind subject, verb rows with frame "impers"), so they hold only for the verbs listed there:
// (1) "The sun set." built as a fragment NP "set (of the sun)": the subject row "sun set" makes it a clause, past for
// the bare form after a singular subject ("set"), present for "sets"; (2) Spanish "Nieva." read as the imperative
// (nieva = 3 sg / 2 sg imperative): an impersonal weather verb is a statement; (3) Spanish "Soplaba un viento frío."
// (the subject after the verb read as the object, the subject implicit): the object is the subject when the subject
// row "viento soplar" exists. True when `out` holds a rebuilt frame.
bool repairNature(const SemFrame& f, const SemSentence& s, const GreekTables& gt, const lex::Lexicon* esLex,
                  SemFrame& out) {
  // (7) Spanish "No toques el fuego." built as a negative statement with an implicit "tú": a 2nd-person present
  // subjunctive after "no" in a main clause is a prohibition (μὴ ἅπτου τοῦ πυρός), not "you do not touch"
  if (esLex && f.type == Kind::Decl && f.negative && f.implicitSubject && f.hasSubject && f.subject.isPronoun &&
      f.subject.pron.person == 2 && f.hasPred && f.pred.auxTokens.empty() && f.pred.token >= 0 &&
      (size_t)f.pred.token < s.tokens.size() && f.subordinate.empty()) {
    std::vector<lex::Analysis> an;
    esLex->lookup(text::es_key(s.tokens[(size_t)f.pred.token].lower), an);
    bool subj2 = false, ind2 = false;
    for (const lex::Analysis& a : an) {
      const Features af = unpack(esLex->feature(a.feat));
      if (af.pos != Verb || af.person != P2) continue;
      if (af.mood == Subjunctive && af.tense == Present) subj2 = true;
      if (af.mood == Indicative || af.mood == Imperative) ind2 = true;
    }
    if (subj2 && !ind2) {
      out = f;
      out.type = Kind::Imp;
      out.hasSubject = false;
      out.subject = SemNP{};
      return true;
    }
  }
  auto subjRow = [&](const std::string& subj, const std::string& verb) {
    return gt.find("subject", text::lower(subj) + " " + text::lower(verb)) != nullptr;
  };
  if (f.type == Kind::Frag && !f.hasPred && f.hasSubject && !f.subject.isPronoun && f.subject.genitive.size() == 1 &&
      f.subject.possessor.empty() && f.subject.adjectives.empty() && f.subordinate.empty() &&
      f.subject.token >= 0 && (size_t)f.subject.token < s.tokens.size()) {
    const SemNP& g = f.subject.genitive[0];
    const std::string w = s.tokens[(size_t)f.subject.token].lower;
    std::string verb = text::lower(f.subject.head);
    // the head written after its "genitive" (no "of" between: "The sun set."), at the end of the sentence
    bool after = g.token >= 0 && g.token < f.subject.token;
    for (int t = g.token + 1; after && t < f.subject.token; ++t)
      if (t >= 0 && (size_t)t < s.tokens.size() && s.tokens[(size_t)t].upos != "PUNCT") after = false;
    if (after && !g.isPronoun && subjRow(g.head, verb)) {
      out = SemFrame{};
      out.type = Kind::Decl;
      out.hasPred = true;
      out.pred.lemma = verb;
      out.pred.token = f.subject.token;
      out.pred.tense = w == verb && g.number == 1 ? frame::Tense::Past : frame::Tense::Present;
      out.hasSubject = true;
      out.subject = g;
      out.subject.definite = g.definite || f.subject.definite;   // "The sun set.": the article went with "set"
      if (out.subject.determiner.empty()) out.subject.determiner = f.subject.determiner;
      out.connectors = f.connectors;
      out.adverbs = f.adverbs;
      out.punct = f.punct;
      return true;
    }
  }
  auto impersonal = [&](const std::string& lemma) {
    return gt.find("verb", text::lower(lemma), "impers") != nullptr;
  };
  if (f.type == Kind::Imp && f.hasPred && !f.hasSubject && !f.hasObject && f.vocatives.empty() && !f.negative &&
      impersonal(f.pred.lemma)) {
    out = f;
    out.type = Kind::Decl;
    out.implicitSubject = true;
    return true;
  }
  // (5) "The wind is cold tonight.": a time word read as the predicate noun with the adjective on it ("tonight +
  // cold"): the adjective is the predicate, the time word an adverb
  if (f.predicative.size() == 1 && f.predAdj.empty() && !f.predicative[0].adjectives.empty() &&
      in(text::lower(f.predicative[0].head), {"tonight", "today", "tomorrow", "yesterday", "now"}) &&
      f.predicative[0].possessor.empty() && f.predicative[0].genitive.empty()) {
    out = f;
    out.predAdj = f.predicative[0].adjectives;
    frame::SemAdverb av;
    av.lemma = text::lower(f.predicative[0].head);
    av.token = f.predicative[0].token;
    out.adverbs.push_back(av);
    out.predicative.clear();
    return true;
  }
  // (6) "The boy told the truth." built with the thing told as the indirect object and no object: a thing is the
  // object (an indirect object is a person or an animal)
  if (f.hasPred && f.hasIndirect && !f.hasObject && !f.indirectObject.isPronoun && !f.indirectObject.isName &&
      !transfer::animate(f.indirectObject) && f.predicative.empty() && f.predAdj.empty()) {
    out = f;
    out.hasObject = true;
    out.object = f.indirectObject;
    out.hasIndirect = false;
    out.indirectObject = SemNP{};
    return true;
  }
  // (4) "Bring me some water": the pronoun read as the object and the thing as the indirect object (the double
  // object misparsed): the person is the indirect object, the thing the object
  if (f.hasPred && f.hasObject && f.hasIndirect && personalPron(f.object) && f.object.pron.person <= 2 &&
      !f.indirectObject.isPronoun && !f.indirectObject.isName && !transfer::animate(f.indirectObject)) {
    out = f;
    std::swap(out.object, out.indirectObject);
    return true;
  }
  if ((f.type == Kind::Decl || f.type == Kind::Wh) && f.hasPred && f.hasObject && !f.object.isPronoun &&
      (!f.hasSubject || (f.implicitSubject && f.subject.isPronoun)) && subjRow(f.object.head, f.pred.lemma)) {
    out = f;
    out.hasSubject = true;
    out.subject = f.object;
    out.implicitSubject = false;
    out.hasObject = false;
    out.object = SemNP{};
    return true;
  }
  return false;
}

bool repairFrontedTime(const SemFrame& f, const SemSentence& s, SemFrame& out) {
  // "Where is his dog?" left as a fragment "dog (his) + where" without its verb: the wh question with "be" (existential
  // order: ποῦ ἐστιν ὁ κύων αὐτοῦ;)
  if (f.type == Kind::Frag && !f.hasPred && f.hasSubject && !f.subject.isPronoun && f.subordinate.empty() &&
      f.predAdj.empty() && f.predicative.empty() && f.adverbs.size() == 1 &&
      (s.question || s.finalPunct.find('?') != std::string::npos)) {
    const std::string a = text::lower(f.adverbs[0].lemma);
    int be = -1;
    bool pastBe = false;
    for (size_t t = 0; t < s.tokens.size(); ++t)
      if (in(s.tokens[t].lower, {"is", "are", "was", "were", "'s", "'re"})) { be = (int)t; pastBe = s.tokens[t].lower[0] == 'w'; break; }
    if (be >= 0 && (a == "where" || a == "how" || a == "when")) {
      out = f;
      out.type = Kind::Wh;
      out.hasPred = true;
      out.copula = false;
      out.existential = a == "where";
      out.pred = frame::SemPredicate{};
      out.pred.lemma = a == "how" ? "have" : "be";   // "how is X?" -> πῶς ἔχει X;
      out.pred.token = be;
      out.pred.tense = pastBe ? frame::Tense::Past : frame::Tense::Present;
      out.wh.word = a;
      out.wh.role = frame::Role::Adverb;
      out.wh.token = f.adverbs[0].token;
      out.adverbs.clear();
      return true;
    }
  }
  if (f.subordinate.empty()) return false;
  const frame::SemSub& last = f.subordinate.back();
  if (last.frame.empty() || !last.frame[0].hasPred || last.before) return false;
  const bool question = s.question || s.finalPunct.find('?') != std::string::npos;
  const std::string w = text::lower(f.wh.word);
  if (!question && f.type == Kind::Wh && (w == "when" || w == "cuando") && last.relation == Relation::Coord && last.marker.empty() &&
      f.hasPred) {
    SemFrame tf = f;
    tf.subordinate.pop_back();
    tf.type = Kind::Decl;
    tf.wh = frame::SemWh{};
    out = last.frame[0];
    if (out.type == Kind::Yn || out.type == Kind::Wh) return false;
    for (const std::string& k : f.connectors) out.connectors.push_back(k);
    tf.connectors.clear();
    frame::SemSub ts;
    ts.relation = Relation::Time;
    ts.marker = w;
    ts.before = true;
    ts.frame.push_back(std::move(tf));
    out.subordinate.insert(out.subordinate.begin(), std::move(ts));
    return true;
  }
  if (!question && (f.type == Kind::Imp || f.type == Kind::Decl) && !f.hasSubject && f.pred.token >= 0 && (size_t)f.pred.token < s.tokens.size() &&
      ingForm(s.tokens[(size_t)f.pred.token].lower) &&
      (last.relation == Relation::Time || (last.relation == Relation::Coord && last.marker.empty())) &&
      last.frame[0].hasSubject &&
      (last.frame[0].type == Kind::Decl)) {
    // the -ing word starts the sentence and a comma closes its phrase
    bool comma = false;
    for (int t : f.tokens)
      if (t >= 0 && (size_t)t < s.tokens.size() && s.tokens[(size_t)t].text == ",") comma = true;
    for (size_t t = 0; t < s.tokens.size() && !comma; ++t)
      if (s.tokens[t].text == "," && (int)t > f.pred.token) comma = true;
    if (f.pred.token != 0 || !comma) return false;
    SemFrame pf = f;
    pf.subordinate.pop_back();
    pf.type = Kind::Decl;
    out = last.frame[0];
    frame::SemSub ts;
    ts.relation = Relation::Time;
    ts.marker = "";
    ts.before = true;
    ts.frame.push_back(std::move(pf));
    out.subordinate.insert(out.subordinate.begin(), std::move(ts));
    return true;
  }
  // "The shepherd, seeing the wolf, fled.": the -ing word (no auxiliary) was taken for the main verb and the real
  // verb hangs in a time / coordinated clause without a subject: that clause is the main one, with this subject
  if (!question && f.type == Kind::Decl && f.hasSubject && f.pred.token > 0 && (size_t)f.pred.token < s.tokens.size() &&
      f.pred.auxTokens.empty() && f.pred.aspect == frame::Aspect::Simple &&
      ingForm(s.tokens[(size_t)f.pred.token].lower) && s.tokens[(size_t)f.pred.token - 1].text == "," &&
      (last.relation == Relation::Time || last.relation == Relation::Coord) && !last.frame[0].hasSubject &&
      last.frame[0].type == Kind::Decl) {
    SemFrame pf = f;
    pf.subordinate.pop_back();
    pf.hasSubject = false;
    pf.subject = SemNP{};
    out = last.frame[0];
    out.hasSubject = true;
    out.subject = f.subject;
    for (const std::string& k : f.connectors) out.connectors.push_back(k);
    pf.connectors.clear();
    frame::SemSub ts;
    ts.relation = Relation::Time;
    ts.marker = "";
    ts.before = true;
    ts.frame.push_back(std::move(pf));
    out.subordinate.insert(out.subordinate.begin(), std::move(ts));
    return true;
  }
  // "Oh little bird, where is your nest?": a fragment NP after "oh" with the real clause coordinated after it is the
  // addressee of that clause (ὦ + vocative)
  bool oh = false;
  for (const std::string& ij : f.interjections) oh = oh || text::lower(ij) == "oh" || text::lower(ij) == "o";
  if (f.type == Kind::Frag && !f.hasPred && f.hasSubject && !f.subject.isPronoun && oh && last.relation == Relation::Coord &&
      last.marker.empty()) {
    out = last.frame[0];
    out.vocatives.insert(out.vocatives.begin(), f.subject);
    return true;
  }
  return false;
}
}  // namespace

void GreekTransfer::clause(const SemFrame& f0, const SemSentence& s, const transfer::Settings& st,
                           transfer::Memory& mem, GrcClauseOut& out, bool subordinate) const {
  out.clear();
  SemFrame repaired;
  const bool rep = !subordinate && repairFrontedTime(f0, s, repaired);
  SemFrame natural;
  const bool nat = !subordinate && repairNature(rep ? repaired : f0, s, gt_,
                                                st.lang == frame::SrcLang::Es ? st.srcLex : nullptr, natural);   // C21
  // C25: '"Woof!" said the dog.': a speech verb first with its "object" after it and no subject is the inverted
  // subject of a quotation (the frame builder reads "said the dog" as dog = object): the object becomes the subject
  SemFrame inverted;
  bool inv = false;
  {
    const SemFrame& g = nat ? natural : rep ? repaired : f0;
    if (!subordinate && st.lang == frame::SrcLang::En && g.type == Kind::Decl && g.hasPred && !g.hasSubject &&
        g.hasObject && !g.object.isPronoun && g.pred.token >= 0 && g.object.token > g.pred.token &&
        in(text::lower(g.pred.lemma), {"say", "ask", "cry", "shout", "reply", "answer", "call", "whisper", "sing"})) {
      bool first = true;
      for (int t = 0; t < g.pred.token && (size_t)t < s.tokens.size(); ++t)
        first = first && (s.tokens[(size_t)t].upos == "PUNCT" || s.tokens[(size_t)t].lower == "\"");
      if (first) {
        inverted = g;
        inverted.hasSubject = true;
        inverted.subject = g.object;
        inverted.hasObject = false;
        inverted.object = SemNP{};
        inv = true;
      }
    }
  }
  const SemFrame& f = inv ? inverted : nat ? natural : rep ? repaired : f0;
  Ctx c(s, st, mem, out);
  if (inv) {
    out.flags.push_back("speech-inversion");
    out.notes.push_back(rules::Reason{-1, "form", "\"said the X\" after a quotation: X read as the speaker: check it", ""});
  }
  c.depth = subordinate ? 1 : 0;
  if (rep) {   // the "when" of a repaired fronted time clause; a rebuilt structure is never OK
    c.cover(f0.wh.token);
    out.flags.push_back("clause-repair");
  }
  if (nat && std::find(out.flags.begin(), out.flags.end(), "clause-repair") == out.flags.end())
    out.flags.push_back("clause-repair");   // C21: a rebuilt weather / nature clause is never OK
  clauseInto(f, c, out.clause);
  // C16: a clause that starts with "if" and has no main clause ("Only if you believe it is.") is the condition
  // alone; "only if" = "not unless": εἰ μή ("εἰ μὴ πιστεύεις ὅτι ἔστιν")
  bool ifConn = false;
  for (const std::string& k : f.connectors) ifConn = ifConn || text::lower(k) == "if" || text::lower(k) == "si";
  if (ifConn && f.type == Kind::Decl && f.hasPred) {
    GrcClause inner = out.clause;
    bool only = false;
    for (const frame::SemAdverb& a : f.adverbs) only = only || a.lemma == "only" || a.lemma == "solo" || a.lemma == "sólo";
    if (only) {
      const uint32_t monon = adverbTable("only", false) ? greek(adverbTable("only", false), Adv) : kNone;
      inner.adverbs.erase(std::remove_if(inner.adverbs.begin(), inner.adverbs.end(),
                                         [&](const GrcAdverb& a) { return a.lemma == monon; }),
                          inner.adverbs.end());
      inner.polarity = inner.polarity == Polarity::Neg ? Polarity::Pos : Polarity::Neg;
    }
    GrcClause outer;
    outer.type = ClauseType::Frag;
    outer.punct = inner.punct;
    GrcSub gs;
    gs.rel = SubRel::Condition;
    gs.conj = greek("εἰ", Conj) != kNone ? greek("εἰ", Conj) : greek("εἰ");
    gs.clause.push_back(std::move(inner));
    outer.subs.push_back(std::move(gs));
    out.clause = std::move(outer);
    c.table(out.clause.subs[0].conj, "if", -1, only ? "\"only if\": εἰ μή (not unless)" : "condition without a main clause");
  }
  // C16: "..., or you wouldn't be here": "or (else)" before a counterfactual is εἰ δὲ μή, + the clause with ἄν
  bool orConn = false;
  for (const std::string& k : f.connectors) orConn = orConn || text::lower(k) == "or" || text::lower(k) == "o";
  if (orConn && out.clause.an) {
    GrcClause inner = out.clause;
    const uint32_t e = greek("ἤ", Conj) != kNone ? greek("ἤ", Conj) : greek("ἤ");
    inner.connectors.erase(std::remove(inner.connectors.begin(), inner.connectors.end(), e), inner.connectors.end());
    GrcClause outer;
    outer.type = ClauseType::Frag;
    outer.punct = inner.punct;
    GrcSub gs;
    gs.rel = SubRel::Coord;
    gs.otherwise = true;
    gs.clause.push_back(std::move(inner));
    outer.subs.push_back(std::move(gs));
    out.clause = std::move(outer);
  }
  if (mem.addresseeGuess) out.flags.push_back("addressee-guess");
  std::sort(out.covered.begin(), out.covered.end());
  out.covered.erase(std::unique(out.covered.begin(), out.covered.end()), out.covered.end());
}

void GreekTransfer::vocative(const SemNP& n, const SemSentence& s, const transfer::Settings& st,
                             transfer::Memory& mem, GrcClauseOut& out) const {
  out.clear();
  Ctx c(s, st, mem, out);
  out.clause.type = ClauseType::Frag;
  GrcNP x;
  // C21: a capitalised address word at the start ("Father, ...", "Teacher, ...") that the tagger took for a name is
  // the common noun in the vocative (ὦ πάτερ), unless the glossary or names_grc.tsv knows it as a name
  SemNP nn = n;
  {
    const std::string low = text::lower(n.head);
    if (n.isName && !gd_.nameByEnglish(n.head) &&
        in(low, {"father", "mother", "grandfather", "grandmother", "brother", "sister", "son", "daughter", "uncle",
                 "aunt", "teacher", "master", "friend", "child", "boy", "girl", "king", "queen", "doctor", "shepherd",
                 "farmer", "sir", "madam", "lady", "papá", "mamá", "padre", "madre", "abuelo", "abuela", "hermano",
                 "hermana", "hijo", "hija", "maestro", "maestra", "amigo", "amiga", "niño", "niña", "rey", "reina"})) {
      nn.isName = false;
      nn.title = false;
      nn.head = low;
    }
  }
  npInto(nn, c, x);
  // "¡Qué jardín tan extraño!" (an NP the parser left as an address): ὡς θαυμαστὸς ὁ κῆπος
  if ((n.determiner == "what" || n.determiner == "qué") && !n.adjectives.empty() && x.head != kNone) {
    out.clause.type = ClauseType::Excl;
    out.clause.exclHos = true;
    out.clause.hasSubject = true;
    x.definite = true;
    out.clause.subject = x;
    out.clause.punct = "!";
    std::sort(out.covered.begin(), out.covered.end());
    out.covered.erase(std::unique(out.covered.begin(), out.covered.end()), out.covered.end());
    return;
  }
  x.definite = false;
  if (!x.possEmphatic) x.possPerson = 0;   // C18: "my child" -> ὦ παῖ
  out.clause.vocatives.push_back(x);
  if (x.number == Pl) mem.sawPlural = true;
  std::sort(out.covered.begin(), out.covered.end());
  out.covered.erase(std::unique(out.covered.begin(), out.covered.end()), out.covered.end());
}

}  // namespace vp::grc
