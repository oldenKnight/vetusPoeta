// GreekTransfer: SemFrame -> GrcClause (see vp/transfer_grc.h). Mirrors the Latin transfer (src/transfer) with the
// Greek closed classes, article policy, particles, tense / aspect mapping and the lexical rules of lexical_en_grc.tsv.
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

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
      {"again", "αὖθις", nullptr},    {"today", "σήμερον", nullptr},  {"tomorrow", "αὔριον", nullptr},
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
      {"early", "πρῴ", nullptr},      {"enough", "ἅλις", nullptr},    {"far", "πόρρω", nullptr},
      {"near", "ἐγγύς", nullptr},     {"sometimes", "ἐνίοτε", nullptr}, {"often", "πολλάκις", nullptr},
      {"immediately", "εὐθύς", nullptr}, {"even", "καί", nullptr},    {"how", "ὡς", nullptr},
      {"thus", "οὕτως", nullptr},     {"together", "ὁμοῦ", nullptr},  {"loudly", "μέγα", nullptr},
      {"gladly", "ἡδέως", nullptr},   {"next", "ἔπειτα", nullptr},    {"below", "κάτω", nullptr},
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
      x.why += ", via English \"" + pivotVia + "\"";
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
    if (low == "hour" && n.ordinal) { id = greek("ὥρα", Noun); c.table(id, n.head, n.token); }
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
      if (l.pos == Noun) {
        c.mem.lastGender = simpleGender(l.gender);
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
    o.number = Pl;
    o.definite = true;
  } else if (d == "many") { quantAdj("πολύς", d); o.number = Pl; }
  else if (d == "few") { quantAdj("ὀλίγος", d); o.number = Pl; }
  else if (d == "much") quantAdj("πολύς", d);
  else if (d == "another" || d == "other") quantAdj("ἄλλος", d);
  if (o.dem != Demonstrative::None) o.definite = true;
  if (n.interrogative) {
    if (n.wh == "how many" || n.wh == "how much") o.interrogative = greek("πόσος", Adj);
    else if (n.wh == "what" || n.wh == "qué") o.interrogative = greek("τίς", Pron);   // C16: "τίς ἡμέρα"
    else o.interrogative = greek("ποῖος", Adj);
    if (o.interrogative == kNone) o.interrogative = greek("τίς", Pron);
    o.definite = false;
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
    ga.lemma = select(a.lemma, Adj, c.context, false, false, c.st, ch);
    c.out.choices.push_back(ch);
    c.cover(a.token);
    if (ga.lemma == kNone) { c.out.unknownWords.push_back(a.lemma); continue; }
    for (size_t i = 0; i < a.adverbs.size(); ++i) {
      const uint32_t av = adverb(a.adverbs[i], i < a.advTokens.size() ? a.advTokens[i] : -1, c, false);
      if (av != kNone) ga.adverbs.push_back(av);
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
    GrcNP x;
    npInto(g, c, x);
    o.genitive.push_back(x);
    break;
  }
  // relative clause
  for (const SemFrame& rf : n.relative) {
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
      if (!r) r = gt_.find("pp", (prep.empty() ? "" : prep + " ") + head);
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
    if (prep.empty()) { add(nullptr, time ? (uint8_t)Dat : (uint8_t)Acc); return; }
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
    if (!person) { add(nullptr, Dat); return; }
    add("παρά", Dat);
    return;
  }
  if (prep == "from") { if (person) add("παρά", Gen); else add("ἐκ", Gen); return; }
  if (prep == "out of") { add("ἐκ", Gen); return; }
  if (prep == "away from") { add("ἀπό", Gen); return; }
  if (prep == "towards" || prep == "toward") { add("πρός", Acc); return; }
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
    for (const std::string& s : {src, en}) {
      if (s.empty()) continue;
      if (const LexRow* r = row("verb", s, want)) return r;
      for (const LexRow& r : gt_.rows())
        if (r.kind == "verb" && r.source == s && (r.frame.empty())) return &r;
    }
    return nullptr;
  };
  auto choose = [&](const std::string& lm, int token, bool hasObj, bool personObj) {
    Choice ch;
    ch.token = token;
    uint32_t id = kNone;
    if (lm == "be" || lm == "ser" || lm == "estar") { id = greek("εἰμί", Verb); c.table(id, lm, token); return id; }
    if (lm == "can" || lm == "poder") { id = greek("δύναμαι", Verb); c.table(id, lm, token); return id; }
    if (const LexRow* r = tableVerb(lm)) {
      id = lexRowLemma(r, Verb);
      if (id != kNone) { c.table(id, lm, token, "lexical_en_grc.tsv"); return id; }
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
  const bool seParticle = sp.particle == "se";   // Spanish pronominal verb ("irse", "inclinarse"): the Greek middle
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
               mk == "οἴομαι" || sp.habitual;
  // C16: "be" + state adjective in the past: imperfect for a lasting state (ὠργίζετο), aorist for an event (ἥμαρτες)
  if (c.forcedVerb != kNone && main == c.forcedVerb) state = durative(std::string(), c.forcedVerb, c.st);
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
  if (sp.voice == frame::Voice::Passive) {
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
    }
  }
  p.tense = tense;
  if (p.modal != kNone) {
    p.infTense = durative(sp.complementVerb.empty() ? sp.lemma : sp.complementVerb, p.lemma, c.st) ? (uint8_t)Present
                                                                                                     : (uint8_t)Aorist;
    if (p.lemma != kNone && lx_.lemma(p.lemma).key == std::string("εἰμί")) p.infTense = Present;
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
  const SemFrame* keepFrame = c.frame;
  const bool keepMotion = c.motion, keepNeg = c.negative, keepExist = c.existential;
  const uint8_t keepPerson = c.subjPerson, keepNumber = c.subjNumber, keepGender = c.subjGender;
  const std::string keepObjPrep = c.objPrep, keepPO = c.prepOverride, keepPOG = c.prepOverrideGreek;
  const uint8_t keepPOC = c.prepOverrideCase;
  const uint8_t keepStart = c.startGender;
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
    if (f.existential) cl.subject.adjFirst = false;   // C16: new information: "ἦν ποτε κόρη μικρά"
    if (f.subject.pron.emphatic) cl.subject.emphasis = true;
    if (f.subject.determiner == "all" && f.subject.isPronoun) {
      cl.pred.person = f.subject.pron.person ? f.subject.pron.person : 3;
      cl.pred.number = Pl;
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
                            (f.hasSubject && f.subject.isPronoun && f.subject.pron.person > 0 && f.subject.pron.person < 3);
    if (personSubj) {
      const std::string a = text::lower(f.predAdj[0].lemma);
      state = gt_.find("state", a);
      if (!state) { const std::string ea = english(a, c.st); if (!ea.empty()) state = gt_.find("state", ea); }
      if (state && state->frame == "adj") state = nullptr;   // handled with the predicate adjective below
    }
  }
  if (!state && f.hasObject && f.hasPred)   // Spanish "tener miedo"
    state = gt_.find("state", text::lower(f.pred.lemma) + " " + text::lower(f.object.head));
  const bool stateObj = state && f.hasObject && !f.copula;
  if (f.hasPred) {
    if (state) {
      c.forcedVerb = lexRowLemma(state, Verb);
      c.forcedVoice = state->frame == "mid" ? (uint8_t)Middle : (uint8_t)0;
      if (c.forcedVerb != kNone) {
        c.table(c.forcedVerb, stateObj ? f.pred.lemma + " " + f.object.head : f.predAdj[0].lemma,
                stateObj ? f.pred.token : f.predAdj[0].token, "state: lexical_en_grc.tsv");
        if (stateObj) c.cover(f.object.tokens);
        else c.cover(f.predAdj[0].token);
      } else {
        state = nullptr;
      }
    }
    predicateInto(f, c, cl);
    c.forcedVerb = kNone;
    c.forcedVoice = 0;
  }
  if (!f.hasPred && f.type != Kind::Frag && f.type != Kind::Excl) cl.type = ClauseType::Frag;
  // imperatives: number (imp.number) and aspect (decision 8: aorist for a single act, present for continuing or
  // general commands and for prohibitions)
  if (f.type == Kind::Imp) {
    bool pl = f.imperativePlural;
    if (!pl && c.mem.addresseePlural) { pl = true; c.mem.addresseeGuess = true; }
    if (pl) { cl.pred.number = Pl; c.mem.sawPlural = true; }
    const bool dur = state || f.negative || f.copula ||
                     durative(f.pred.lemma, cl.pred.modal != kNone ? cl.pred.modal : cl.pred.lemma, c.st);
    if (cl.pred.modal == kNone) cl.pred.tense = dur ? (uint8_t)Present : (uint8_t)Aorist;
  }
  // object
  // a reflexive pronoun object ("se fueron", "inclínense") is the middle voice in Greek or nothing at all
  const bool reflexiveObj = f.hasObject && f.object.isPronoun &&
                            (f.object.pron.reflexive || f.object.pronLemma == "se") && !f.object.pron.emphatic;
  if (reflexiveObj) c.cover(f.object.tokens);
  if (f.hasObject && !stateObj && !reflexiveObj &&
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
      GrcNP x;
      npInto(pn, c, x);
      // decision 9: a unique definite predicate (a title, "the X of Y") keeps the article
      if (pn.definite && (pn.title || pn.isName || !pn.genitive.empty() || !x.genitive.empty())) x.forceArticle = true;
      cl.predicative.push_back(x);
    }
    for (const frame::SemAdj& a : f.predAdj) {
      Choice ch;
      ch.token = a.token;
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
    if (a.ellipticWh) {   // "I don't care where" -> ... ὅπου / ὅποι
      const char* w = a.lemma == "where" ? (c.mem.lastMotion || c.motion ? "ὅποι" : "ὅπου")
                    : a.lemma == "when" ? "ὁπότε" : a.lemma == "why" ? "διότι" : "ὅπως";
      const uint32_t id = greek(w);
      c.cover(a.token);
      if (id != kNone) { cl.adverbs.push_back(GrcAdverb{id, AdvPos::End}); c.table(id, a.lemma, a.token, "elliptical indirect question"); }
      continue;
    }
    const uint32_t id = adverb(a.lemma, a.token, c, c.motion);
    if (id == kNone) continue;
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
    if (w == "where") g = c.motion ? "ποῖ" : "ποῦ";
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
    clauseInto(sf, c, sc);
    c.depth -= dep;
    c.frame = &f;
    const std::string mk = text::lower(sb.marker);
    auto conj = [&](const char* h) { uint32_t id = greek(h, Conj); return id != kNone ? id : greek(h); };
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
        else if (mk == "while" || mk == "until") gs.conj = conj("ἕως");
        else if (mk == "when" && sf.pred.tense != frame::Tense::Past) gs.conj = conj("ὅτε");
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
        sc.pred.tense = durative(sf.pred.lemma, sc.pred.lemma, c.st) ? Present : Aorist;
        break;
      case Relation::Result: gs.rel = SubRel::Result; break;
      case Relation::Concession: gs.rel = SubRel::Coord; gs.conj = conj("ἀλλά"); break;
      case Relation::Complement: {
        if (sf.type == Kind::Wh) { gs.rel = SubRel::IndirectQ; break; }
        // C16: verbs of thinking (valency_grc.tsv acc+inf: οἴομαι, νομίζω) take accusative + infinitive
        // ("ᾤμην Σελήνης ἡμέραν εἶναι"); others ὅτι + indicative ("πιστεύεις ὅτι ἔστιν")
        bool accInf = false;
        const uint32_t mv = cl.pred.modal != kNone ? cl.pred.modal : cl.pred.lemma;
        if (mv != kNone)
          if (const Valency* v = gd_.valency(lx_.lemma(mv).key))
            accInf = !v->frames.empty() && v->frames[0].kind == FrameKind::AccInf;
        if (accInf) gs.rel = SubRel::AccInf;
        else { gs.rel = SubRel::Cause; gs.conj = conj("ὅτι"); }
        break;
      }
      case Relation::Coord: {
        gs.rel = SubRel::Coord;
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
}

void GreekTransfer::clause(const SemFrame& f, const SemSentence& s, const transfer::Settings& st,
                           transfer::Memory& mem, GrcClauseOut& out, bool subordinate) const {
  out.clear();
  Ctx c(s, st, mem, out);
  c.depth = subordinate ? 1 : 0;
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
  npInto(n, c, x);
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
  out.clause.vocatives.push_back(x);
  if (x.number == Pl) mem.sawPlural = true;
  std::sort(out.covered.begin(), out.covered.end());
  out.covered.erase(std::unique(out.covered.begin(), out.covered.end()), out.covered.end());
}

}  // namespace vp::grc
