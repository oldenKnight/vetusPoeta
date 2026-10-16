// engine/rules, task C40 (RULES-R): the last wrong-among-OK cases after C39 and the owner follow-ups. (a) the Latin
// locative of towns and small islands (Rōmae, Corinthī, Athēnīs, Carthāgine; "to" -> Rōmam, "from" -> Rōmā, no
// preposition), EN and ES; (b) grc2x: an adjective used as a noun with the article (readable_grc tags noun: / noun-pl: /
// alt:, αὐτουργός = the farmer who works his own land; never "the self-sufficients"); (c) EN / ES -> GRC "..., not much,
// but enough" keeps its negation and agrees with the noun (οὐ πολὺν ἀλλὰ ἱκανόν); (d) although / aunque -> καίπερ + the
// participle with the same subject, else μέν ... ὅμως δέ before the main clause, καίτοι after it; Latin quamquam. Own
// sentences only: the cases below and tests/regression/own_last*.txt with their gold (written before any C40 rule).
// Cases that need the real data (data/work or VP_DATA_WORK) skip with a message.
#include <doctest.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "vp/engine_config.h"
#include "vp/features.h"
#include "vp/lex.h"
#include "vp/realise_la.h"
#include "vp/rules.h"
#include "vp/text.h"

namespace stdfs = std::filesystem;
using namespace vp;

namespace {

stdfs::path repoR() { return stdfs::path(VP_FIXTURES_DIR).parent_path().parent_path(); }
stdfs::path workR() {
  const char* e = std::getenv("VP_DATA_WORK");
  return e && *e ? stdfs::path(e) : repoR() / "data" / "work";
}
stdfs::path buildR() { return stdfs::path(VP_TEST_TMP).parent_path(); }

struct RealR {
  lex::Lexicon la, grc, en, es;
  bool ok = false;
  std::string why;
};
const RealR& realR() {
  static RealR r = [] {
    RealR x;
    auto la = lex::Lexicon::open(workR() / "latin.vpl");
    auto grc = lex::Lexicon::open(workR() / "greek.vpl");
    auto en = lex::Lexicon::open(workR() / "english.vpl");
    auto es = lex::Lexicon::open(workR() / "spanish.vpl");
    const bool nlp = stdfs::exists(workR() / "nlp" / "english.tag.vpt") && stdfs::exists(workR() / "nlp" / "spanish.tag.vpt");
    if (!la.ok() || !grc.ok() || !en.ok() || !es.ok() || !nlp) {
      x.why = !la.ok() ? la.error().message : !grc.ok() ? grc.error().message : !en.ok() ? en.error().message
              : !es.ok() ? es.error().message : "no nlp/*.tag.vpt";
      return x;
    }
    x.la = std::move(la.value());
    x.grc = std::move(grc.value());
    x.en = std::move(en.value());
    x.es = std::move(es.value());
    x.ok = true;
    return x;
  }();
  return r;
}
#define NEED_REALR()                                                                             \
  if (!realR().ok) {                                                                             \
    MESSAGE("real data not available (" << realR().why << "); set VP_DATA_WORK. Skipped.");      \
    return;                                                                                      \
  }

std::unique_ptr<rules::Engine> engineR() {
  rules::EngineConfig cfg = rules::defaultEngineConfig();
  cfg.curatedDir = (repoR() / "data" / "curated").string();
  cfg.dataDir = workR().string();
  cfg.nlpDir = (workR() / "nlp").string();
  auto e = rules::makeEngine(cfg);
  const RealR& r = realR();
  e->setLexicons(&r.la, &r.grc, &r.en, &r.es);
  return e;
}

rules::Options optR(rules::Lang s, rules::Lang t) {
  rules::Options o;
  o.source = s;
  o.target = t;
  o.fidelity = 2;
  o.speakerGender = 'm';
  o.textProject = true;
  return o;
}

std::vector<rules::CueOutput> runR(rules::Engine& e, const std::vector<std::string>& units, const rules::Options& o) {
  std::vector<rules::CueInput> in(units.size());
  for (size_t i = 0; i < units.size(); ++i) {
    in[i].index = (uint32_t)i;
    in[i].sourceText = units[i];
  }
  auto r = e.translate(in, o, rules::Context{}, nullptr, nullptr);
  REQUIRE(r.ok());
  return r.value();
}
rules::CueOutput oneR(rules::Engine& e, const std::string& s, const rules::Options& o) { return runR(e, {s}, o)[0]; }

std::string confR(rules::Confidence c) {
  return c == rules::Confidence::Ok ? "ok" : c == rules::Confidence::Check ? "check" : "fix";
}

// the gold comparison of tools/eval/evallib.py (as test_rules_q.cpp): lower case, combining marks dropped, punctuation
// and quotes as spaces, v -> u, j -> i
std::string normR(const std::string& s0) {
  const std::string s = text::lower(text::nfd(s0));
  std::string out;
  size_t i = 0;
  while (i < s.size()) {
    size_t j = i;
    const char32_t c = text::decodeUtf8(s, j);
    if (j == i) j = i + 1;
    const bool mark = c >= 0x300 && c <= 0x36F;
    const bool punct = c == '.' || c == ',' || c == ';' || c == '!' || c == '?' || c == ':' || c == '"' || c == '\'' ||
                       c == '-' || c == '(' || c == ')' || c == 0xB7 || c == 0x387 || c == 0x37E || c == 0xA1 ||
                       c == 0xBF || c == 0xAB || c == 0xBB || c == 0x201C || c == 0x201D || c == 0x2019 || c == '\n';
    if (mark) {
    } else if (punct || c == ' ' || c == '\t') {
      if (!out.empty() && out.back() != ' ') out += ' ';
    } else if (c == 'v') {
      out += 'u';
    } else if (c == 'j') {
      out += 'i';
    } else {
      out.append(s, i, j - i);
    }
    i = j;
  }
  while (!out.empty() && out.back() == ' ') out.pop_back();
  return out;
}
bool matchR(const std::string& got, const std::string& gold) {
  for (size_t a = 0;;) {
    const size_t b = gold.find(" | ", a);
    if (normR(got) == normR(gold.substr(a, b == std::string::npos ? std::string::npos : b - a))) return true;
    if (b == std::string::npos) return false;
    a = b + 3;
  }
}
std::vector<std::string> linesR(const stdfs::path& p) {
  std::vector<std::string> out;
  std::ifstream in(p);
  std::string l;
  while (std::getline(in, l)) {
    if (!l.empty() && l.back() == '\r') l.pop_back();
    if (!l.empty() && l[0] != '#') out.push_back(l);
  }
  return out;
}

struct Pair { const char* src; const char* out; };

void exactR(rules::Engine& e, const std::vector<Pair>& cases, const rules::Options& o) {
  for (const Pair& c : cases) {
    const rules::CueOutput r = oneR(e, c.src, o);
    CHECK_MESSAGE(r.target == c.out, c.src << " -> " << r.target << " (" << confR(r.confidence) << ")");
    CHECK(r.confidence != rules::Confidence::Fix);
  }
}

}  // namespace

// ---- (a) the locative of names_la.tsv towns: no data needed --------------------------------------------------------
TEST_CASE("rules-r: Latin town names in the locative (Names::decline)") {
  using realise::Names;
  struct D { const char* nom; const char* gen; int decl; uint8_t g; const char* loc; };
  const D ds[] = {
      {"Rōma", "Rōmae", 1, feat::F, "Rōmae"},        {"Athēnae", "Athēnārum", 1, feat::F, "Athēnīs"},
      {"Corinthus", "Corinthī", 2, feat::F, "Corinthī"}, {"Londinium", "Londiniī", 2, feat::N, "Londiniī"},
      {"Delphī", "Delphōrum", 2, feat::M, "Delphīs"},  {"Carthāgō", "Carthāginis", 3, feat::F, "Carthāgine"},
  };
  for (const D& d : ds) {
    std::string out;
    CHECK(Names::decline(text::nfc(d.nom), text::nfc(d.gen), d.decl, d.g, feat::Loc, feat::Sg, out));
    CHECK(out == text::nfc(d.loc));
  }
  // a plural town of the 2nd declension declines as a plural (Delphōs, Pompēiōrum)
  std::string acc;
  CHECK(Names::decline(text::nfc("Delphī"), text::nfc("Delphōrum"), 2, feat::M, feat::Acc, feat::Sg, acc));
  CHECK(acc == text::nfc("Delphōs"));
  // the other cases are unchanged
  CHECK(Names::decline(text::nfc("Rōma"), text::nfc("Rōmae"), 1, feat::F, feat::Abl, feat::Sg, acc));
  CHECK(acc == text::nfc("Rōmā"));
}

TEST_CASE("rules-r: EN / ES -> LA towns and small islands take no preposition (item a)") {
  NEED_REALR();
  auto e = engineR();
  exactR(*e,
         {{"The poet lives in Rome.", "Poēta Rōmae habitat."},
          {"The philosophers teach in Athens.", "Philosophī Athēnīs docent."},
          {"The soldier lives at Carthage.", "Mīles Carthāgine habitat."},
          {"The merchant stays at Corinth with his brother.", "Mercātor Corinthī cum frātre suō manet."},
          {"My friend was at Rome yesterday.", "Amīcus meus heri Rōmae erat."},   // "Rome yesterday" was one noun
          {"The girls live in Rome and Athens.", "Puellae Rōmae et Athēnīs habitant."},
          {"The ships sail to Rome.", "Nāvēs Rōmam nāvigant."},                 // Rōmae (a dative) before
          {"The ships sail to Delos.", "Nāvēs Dēlum nāvigant."},
          {"The farmer goes to Rome with his son.", "Agricola Rōmam cum fīliō suō it."},
          {"The merchant comes from Carthage.", "Mercātor Carthāgine venit."},
          {"We stay in London.", "Londiniī manēmus."},
          {"We have been to Rome.", "Rōmae fuimus."},                            // C17 expectation kept
          {"He lives in ancient Rome.", "In antīquā Rōmā habitat."}},            // an attribute keeps the preposition
         optR(rules::Lang::En, rules::Lang::La));
  exactR(*e,
         {{"Mi hermano vive en Corinto.", "Frāter meus Corinthī habitat."},
          {"Vivo en Atenas.", "Athēnīs habitō."},
          {"Volvemos de Roma.", "Rōmā redīmus."},
          {"La carta viene de Atenas.", "Epistula Athēnīs venit."},
          {"Hablamos de Roma.", "Dē Rōmā loquimur."}},                           // "about Rome": not a place
         optR(rules::Lang::Es, rules::Lang::La));
  // never "in Rōmā" for the place where
  for (const char* s : {"The poet lives in Rome.", "Many people live in Rome.", "The boy was born in Rome."}) {
    const rules::CueOutput r = oneR(*e, s, optR(rules::Lang::En, rules::Lang::La));
    INFO(s << " -> " << r.target);
    CHECK(r.target.find("in Rōmā") == std::string::npos);
    CHECK(r.target.find("In Rōmā") == std::string::npos);
  }
}

TEST_CASE("rules-r: frame repairs met on the way (has + noun, a giving verb's two objects, although after a comma)") {
  NEED_REALR();
  auto e = engineR();
  exactR(*e,
         {{"The farmer has food.", "Agricola cibum habet."},                      // "has" was the noun's auxiliary
          {"The boy has books.", "Puer librōs habet."},                          // "books" was a verb
          {"The garden provides him food.", "Hortus cibum eī praebet."},
          {"For the garden is good and provides him food.", "Nam hortus bonus est et cibum eī praebet."},
          {"The boy walks to the city, although the road is long.", "Puer ad urbem ambulat quamquam via longa est."},
          {"Although he is tired, the farmer works in the field.", "Quamquam fessus est, agricola in agrō labōrat."}},
         optR(rules::Lang::En, rules::Lang::La));
  exactR(*e, {{"Aunque llueve, el niño juega en el jardín.", "Quamquam pluit, puer in hortō lūdit."}},
         optR(rules::Lang::Es, rules::Lang::La));
  // "..., and although X, Y": the tree turned round (quamquam kept; a rebuilt structure, never OK)
  struct T { const char* en; const char* la; const char* grc; };
  const T turned[] = {
      {"The day is warm, and although the boy is tired, he works.", "Diēs calidus est et quamquam puer fessus est, labōrat.",
       "Ἡ ἡμέρα θερμή ἐστι καὶ ὁ παῖς καίπερ κάμνων ἐργάζεται."},
      {"For the road is long, and although the hut is small, it gives shelter.",
       "Nam via longa est et quamquam casa parva est, tēctum dat.",
       "Ἡ γὰρ ὁδὸς μακρά ἐστιν, ἡ μὲν καλύβη μικρά ἐστιν, ὅμως δὲ σκέπην δίδωσιν."}};
  for (const T& t : turned) {
    const rules::CueOutput la = oneR(*e, t.en, optR(rules::Lang::En, rules::Lang::La));
    CHECK_MESSAGE(la.target == t.la, t.en << " -> " << la.target);
    CHECK(la.confidence == rules::Confidence::Check);
    const rules::CueOutput grc = oneR(*e, t.en, optR(rules::Lang::En, rules::Lang::Grc));
    CHECK_MESSAGE(grc.target == t.grc, t.en << " -> " << grc.target);
    CHECK(grc.confidence == rules::Confidence::Check);
  }
}

// ---- (b) grc2x: an adjective used as a noun ---------------------------------------------------------------------------
TEST_CASE("rules-r: αὐτουργός as a noun and as a predicate; plural adjectives as nouns (item b)") {
  NEED_REALR();
  auto e = engineR();
  exactR(*e,
         {{"ὁ αὐτουργὸς τὸν σῖτον εἰς τὴν οἰκίαν φέρει.", "The farmer who works his own land brings the food into the house."},
          {"οἱ αὐτουργοὶ τοὺς βοῦς φιλοῦσιν.", "The farmers who work their own land love the oxen."},
          {"ὁ δοῦλος τοὺς αὐτουργοὺς ἐν τῷ ἀγρῷ ὁρᾷ.", "The slave sees the farmers who work their own land in the field."},
          {"ὁ ἀνὴρ αὐτουργός ἐστιν.", "The man is self-employed."},
          {"οἱ ἀγαθοὶ οὐ κλέπτουσιν.", "The good do not steal."},              // never "the goods"
          {"οἱ σύμμαχοι ἀφίκοντο.", "The allies arrived."},                       // a person row is a noun: plural kept
          {"οἱ Ἀθηναῖοι τὴν πόλιν φιλοῦσιν.", "The Athenians love the city."}},
         optR(rules::Lang::Grc, rules::Lang::En));
  exactR(*e,
         {{"ὁ δοῦλος τοὺς αὐτουργοὺς ἐν τῷ ἀγρῷ ὁρᾷ.",
           "El esclavo ve a los campesinos que trabajan su propia tierra en el campo."},   // a person: "a"
          {"ὁ γεωργὸς τοῖς αὐτουργοῖς τὸν σῖτον δίδωσιν.",
           "El campesino da el alimento a los campesinos que trabajan su propia tierra."},
          {"ὁ ἀνὴρ αὐτουργός ἐστιν.", "El hombre es autónomo."},
          {"ἡ γυνὴ αὐτουργός ἐστιν.", "La mujer es autónoma."}},
         optR(rules::Lang::Grc, rules::Lang::Es));
  // the second reading of the predicate is offered as an alternative sentence
  for (bool es : {false, true}) {
    const rules::CueOutput r = oneR(*e, "ὁ ἀνὴρ αὐτουργός ἐστιν.", optR(rules::Lang::Grc, es ? rules::Lang::Es : rules::Lang::En));
    bool alt = false;
    for (const rules::Alternative& a : r.alternatives)
      alt = alt || a.text == (es ? "El hombre es autosuficiente." : "The man is self-sufficient.");
    CHECK_MESSAGE(alt, r.target);
  }
  // no English adjective with a plural -s anywhere in these
  for (const char* g : {"οἱ αὐτουργοὶ ἐν τοῖς ἀγροῖς πονοῦσιν.", "οἱ σοφοὶ λέγουσιν."}) {
    const rules::CueOutput r = oneR(*e, g, optR(rules::Lang::Grc, rules::Lang::En));
    INFO(g << " -> " << r.target);
    CHECK(r.target.find("sufficients") == std::string::npos);
    CHECK(r.target.find("employeds") == std::string::npos);
    CHECK(r.target.find("wises") == std::string::npos);
  }
}

// ---- (c) EN / ES -> GRC "not X but Y" after a noun --------------------------------------------------------------------
TEST_CASE("rules-r: \"..., not much, but enough\" keeps its negation and agrees with the noun (item c)") {
  NEED_REALR();
  auto e = engineR();
  exactR(*e,
         {{"The farmer has food, not much, but enough.", "Ὁ γεωργὸς σῖτον ἔχει, οὐ πολὺν ἀλλὰ ἱκανόν."},
          {"The girl has a house, not big but beautiful.", "Ἡ κόρη οἰκίαν ἔχει, οὐ μεγάλην ἀλλὰ καλήν."},
          {"The boy has books, not many but good.", "Ὁ παῖς βιβλία ἔχει, οὐ πολλὰ ἀλλὰ ἀγαθά."},
          {"We have water, not much, but enough.", "Ὕδωρ ἔχομεν, οὐ πολὺ ἀλλὰ ἱκανόν."},
          {"The garden gives fruit: not much, but enough.", "Ὁ κῆπος καρπὸν δίδωσιν· οὐ πολὺν ἀλλὰ ἱκανόν."},
          // the noun of a clause coordinated after the main one
          {"For the garden is good and provides him food: not much but enough.",
           "Ὁ γὰρ κῆπος ἀγαθός ἐστι καὶ αὐτῷ σῖτον παρέχει· οὐ πολὺν ἀλλὰ ἱκανόν."}},
         optR(rules::Lang::En, rules::Lang::Grc));
  exactR(*e, {{"Tenemos pan, no mucho, pero suficiente.", "Ἄρτον ἔχομεν, οὐ πολὺν ἀλλὰ ἱκανόν."},
              {"Tengo libros, no muchos, pero buenos.", "Βιβλία ἔχω, οὐ πολλὰ ἀλλὰ ἀγαθά."}},
         optR(rules::Lang::Es, rules::Lang::Grc));
  // the tail is about the word right before it: when that is not the clause's object it is never given back silently
  for (const char* s : {"The farmer gives food to the boy, not much but enough.", "The farmer is rich, not very but enough."}) {
    const rules::CueOutput r = oneR(*e, s, optR(rules::Lang::En, rules::Lang::Grc));
    INFO(s << " -> " << r.target);
    CHECK(r.confidence != rules::Confidence::Ok);
  }
}

// ---- (d) although ---------------------------------------------------------------------------------------------------
TEST_CASE("rules-r: although / aunque -> καίπερ + participle, μέν ... ὅμως δέ, καίτοι (item d)") {
  NEED_REALR();
  auto e = engineR();
  exactR(*e,
         {{"Although he is tired, the slave carries the stones.", "Ὁ δοῦλος καίπερ κάμνων τοὺς λίθους φέρει."},
          {"Although they are tired, the farmers work in the field.", "Οἱ γεωργοὶ καίπερ κάμνοντες ἐν τῷ ἀγρῷ ἐργάζονται."},
          {"Although she is small, the girl carries the stone.", "Ἡ κόρη καίπερ μικρὰ οὖσα τὸν λίθον φέρει."},
          {"Although the road is long, the boy walks to the city.", "Ἡ μὲν ὁδὸς μακρά ἐστιν, ὅμως δὲ ὁ παῖς εἰς τὴν πόλιν βαδίζει."},
          {"Although it is raining, the boys play in the garden.", "Ὕει μέν, ὅμως δὲ οἱ παῖδες ἐν τῷ κήπῳ παίζουσιν."},
          {"The boy walks to the city, although the road is long.", "Ὁ παῖς εἰς τὴν πόλιν βαδίζει, καίτοι ἡ ὁδὸς μακρά ἐστιν."}},
         optR(rules::Lang::En, rules::Lang::Grc));
  exactR(*e, {{"El niño camina a la ciudad, aunque el camino es largo.", "Ὁ παῖς εἰς τὴν πόλιν βαδίζει, καίτοι ἡ ὁδὸς μακρά ἐστιν."}},
         optR(rules::Lang::Es, rules::Lang::Grc));
  // ἀλλά at the head of the concessive clause said the opposite: never again
  for (const char* s : {"Although he is tired, the slave carries the stones.", "Although the road is long, the boy walks to the city."}) {
    const rules::CueOutput r = oneR(*e, s, optR(rules::Lang::En, rules::Lang::Grc));
    CHECK(r.target.rfind("Ἀλλὰ", 0) != 0);
  }
}

// ---- own material end to end (text project, one job per file) --------------------------------------------------------
TEST_CASE("rules-r: own material vs gold (tests/regression/own_last*.txt; report regression_report_r.txt)") {
  NEED_REALR();
  auto e = engineR();
  struct F { const char* src; const char* gold; rules::Lang s, t; int floor; };
  // floors: the counts after C40 (first run before any rule: 4 / 8, 1 / 3, 1 / 3, 1 / 3, 0 / 4, 0 / 2)
  const F files[] = {
      {"own_last.en.txt", "own_last.la.gold.txt", rules::Lang::En, rules::Lang::La, 8},
      {"own_last.es.txt", "own_last.es.la.gold.txt", rules::Lang::Es, rules::Lang::La, 3},
      {"own_last_grc.grc.txt", "own_last_grc.en.gold.txt", rules::Lang::Grc, rules::Lang::En, 3},
      {"own_last_grc.grc.txt", "own_last_grc.es.gold.txt", rules::Lang::Grc, rules::Lang::Es, 3},
      {"own_last_grc.en.txt", "own_last_grc.grc.gold.txt", rules::Lang::En, rules::Lang::Grc, 4},
      {"own_last_grc.es.txt", "own_last_grc.es.grc.gold.txt", rules::Lang::Es, rules::Lang::Grc, 2},
  };
  std::ostringstream rep;
  for (const F& f : files) {
    const std::vector<std::string> src = linesR(repoR() / "tests" / "regression" / f.src);
    const std::vector<std::string> gold = linesR(repoR() / "tests" / "regression" / "expected" / f.gold);
    REQUIRE(src.size() == gold.size());
    const rules::Options o = optR(f.s, f.t);
    const std::vector<rules::CueOutput> out = runR(*e, src, o);
    REQUIRE(out.size() == src.size());
    int right = 0, ok = 0, okMiss = 0;
    rep << "== " << f.src << " -> " << f.gold << "\n";
    for (size_t i = 0; i < src.size(); ++i) {
      const bool m = matchR(out[i].target, gold[i]);
      right += m;
      if (out[i].confidence == rules::Confidence::Ok) {
        ++ok;
        okMiss += !m;
      }
      rep << (i + 1) << "\t" << (m ? "=" : "x") << "\t" << out[i].target << "\t" << confR(out[i].confidence) << "\n";
    }
    rep << "right " << right << " / " << src.size() << "; ok " << ok << " (mismatches among OK " << okMiss << ")\n";
    MESSAGE(std::string(f.gold) << ": right " << right << " / " << src.size() << ", ok " << ok << ", mismatches among OK " << okMiss);
    CHECK(right >= f.floor);
    CHECK(okMiss == 0);
    const std::vector<rules::CueOutput> again = runR(*e, src, o);   // deterministic
    for (size_t i = 0; i < src.size(); ++i) CHECK(again[i].target == out[i].target);
  }
  std::ofstream(buildR() / "regression_report_r.txt") << rep.str();
}
