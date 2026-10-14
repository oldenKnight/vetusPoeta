// engine/rules, task C39 (RULES-Q): follow-ups of C37 / C38 on the shared English frame (a "The well supplies water.",
// b 3rd-person verbs read as nouns, c "O Jupiter," / an address after an opening quote, e adjectives of proper
// names), d text projects never wrapped like subtitles, f la2x (abstract subjects without the article, one's own
// master, nōn multum sed satis), g EN -> GRC "enough", h the Greek row of ἔργον. Own sentences only: the cases below
// and tests/regression/own_frame*.txt with their gold (written before any C39 rule). Cases that need the real data
// (data/work or VP_DATA_WORK: latin.vpl, greek.vpl, english.vpl, spanish.vpl, nlp/*.vpt) skip with a message.
#include <doctest.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "vp/cue.h"
#include "vp/engine_config.h"
#include "vp/lex.h"
#include "vp/rules.h"
#include "vp/text.h"

namespace stdfs = std::filesystem;
using namespace vp;

namespace {

stdfs::path repoQ() { return stdfs::path(VP_FIXTURES_DIR).parent_path().parent_path(); }
stdfs::path workQ() {
  const char* e = std::getenv("VP_DATA_WORK");
  return e && *e ? stdfs::path(e) : repoQ() / "data" / "work";
}
stdfs::path buildQ() { return stdfs::path(VP_TEST_TMP).parent_path(); }

struct RealQ {
  lex::Lexicon la, grc, en, es;
  bool ok = false;
  std::string why;
};
const RealQ& realQ() {
  static RealQ r = [] {
    RealQ x;
    auto la = lex::Lexicon::open(workQ() / "latin.vpl");
    auto grc = lex::Lexicon::open(workQ() / "greek.vpl");
    auto en = lex::Lexicon::open(workQ() / "english.vpl");
    auto es = lex::Lexicon::open(workQ() / "spanish.vpl");
    const bool nlp = stdfs::exists(workQ() / "nlp" / "english.tag.vpt") && stdfs::exists(workQ() / "nlp" / "spanish.tag.vpt");
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
#define NEED_REALQ()                                                                             \
  if (!realQ().ok) {                                                                             \
    MESSAGE("real data not available (" << realQ().why << "); set VP_DATA_WORK. Skipped.");      \
    return;                                                                                      \
  }

std::unique_ptr<rules::Engine> engineQ() {
  rules::EngineConfig cfg = rules::defaultEngineConfig();
  cfg.curatedDir = (repoQ() / "data" / "curated").string();
  cfg.dataDir = workQ().string();
  cfg.nlpDir = (workQ() / "nlp").string();
  auto e = rules::makeEngine(cfg);
  const RealQ& r = realQ();
  e->setLexicons(&r.la, &r.grc, &r.en, &r.es);
  return e;
}

rules::Options optQ(rules::Lang s, rules::Lang t, bool textProject = true) {
  rules::Options o;
  o.source = s;
  o.target = t;
  o.fidelity = 2;
  o.speakerGender = 'm';
  o.textProject = textProject;
  return o;
}

std::vector<rules::CueOutput> runQ(rules::Engine& e, const std::vector<std::string>& units, const rules::Options& o) {
  std::vector<rules::CueInput> in(units.size());
  for (size_t i = 0; i < units.size(); ++i) {
    in[i].index = (uint32_t)i;
    in[i].sourceText = units[i];
  }
  auto r = e.translate(in, o, rules::Context{}, nullptr, nullptr);
  REQUIRE(r.ok());
  return r.value();
}

// one sentence on its own (no discourse memory from other sentences)
rules::CueOutput oneQ(rules::Engine& e, const std::string& s, const rules::Options& o) { return runQ(e, {s}, o)[0]; }

bool flagQ(const rules::CueOutput& o, const char* f) { return std::find(o.flags.begin(), o.flags.end(), f) != o.flags.end(); }
bool checkOkQ(const rules::CueOutput& o, const char* id) {
  for (const rules::Check& c : o.checks)
    if (c.id == id) return c.ok;
  return true;
}

// the gold comparison of tools/eval/evallib.py: lower case, every combining mark dropped (macrons, accents,
// breathings), punctuation and quotes as spaces, v -> u and j -> i
std::string normQ(const std::string& s0) {
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

bool matchQ(const std::string& got, const std::string& gold) {
  for (size_t a = 0;;) {
    const size_t b = gold.find(" | ", a);
    if (normQ(got) == normQ(gold.substr(a, b == std::string::npos ? std::string::npos : b - a))) return true;
    if (b == std::string::npos) return false;
    a = b + 3;
  }
}

std::vector<std::string> linesQ(const stdfs::path& p) {
  std::vector<std::string> out;
  std::ifstream in(p);
  std::string l;
  while (std::getline(in, l)) {
    if (!l.empty() && l.back() == '\r') l.pop_back();
    if (!l.empty() && l[0] != '#') out.push_back(l);
  }
  return out;
}

std::string confQ(rules::Confidence c) {
  return c == rules::Confidence::Ok ? "ok" : c == rules::Confidence::Check ? "check" : "fix";
}

}  // namespace

// ---- (d) text projects: no data needed for the layout helpers -------------------------------------------------------
TEST_CASE("rules-q: a text unit keeps its own lines (cue::textLayout, cue::beginsSourceLine)") {
  const cue::Layout one = cue::textLayout("Agricola cum fīliō suō cotīdiē ā parvō praediō ad magnam urbem ambulat.");
  CHECK(one.lines.size() == 1);
  CHECK(!one.overflow);
  CHECK(one.joined.find('\n') == std::string::npos);
  const cue::Layout two = cue::textLayout("Puella canit.\nPuer librum legit.");
  CHECK(two.lines.size() == 2);
  CHECK(two.joined == "Puella canit.\nPuer librum legit.");
  // the source of a unit with two lines: the first sentence opens the unit, the second a new line
  const std::string src = "The girl sings.\nThe boy reads a book.";
  size_t cur = 0;
  CHECK(!cue::beginsSourceLine(src, "The girl sings.", cur));
  CHECK(cue::beginsSourceLine(src, "The boy reads a book.", cur));
  // a sentence that goes on on the same line, and one after a speaker dash or an opening quote on a new line
  const std::string src2 = "He came. She left.\n- Where is he?\n\"Here!\"";
  cur = 0;
  CHECK(!cue::beginsSourceLine(src2, "He came.", cur));
  CHECK(!cue::beginsSourceLine(src2, "She left.", cur));
  CHECK(cue::beginsSourceLine(src2, "Where is he?", cur));
  CHECK(cue::beginsSourceLine(src2, "Here!\"", cur));
  CHECK(!cue::beginsSourceLine(src2, "nowhere", cur));   // not found: no break, the cursor stays
}

TEST_CASE("rules-q: a text project is never wrapped like a subtitle (Latin and Greek, item d)") {
  NEED_REALQ();
  auto e = engineQ();
  const std::string longEn = "The farmer walks with his son from the small farm to the big city every day.";
  for (rules::Lang t : {rules::Lang::La, rules::Lang::Grc}) {
    const rules::CueOutput txt = oneQ(*e, longEn, optQ(rules::Lang::En, t, true));
    INFO((t == rules::Lang::La ? "la: " : "grc: ") << txt.target);
    CHECK(txt.target.find('\n') == std::string::npos);
    CHECK(txt.target.size() > 42);
    CHECK(!flagQ(txt, "overflow"));
    CHECK(checkOkQ(txt, "A8"));
    // a subtitle project (the default) still breaks the same cue into lines of at most 42 characters
    const rules::CueOutput sub = oneQ(*e, longEn, optQ(rules::Lang::En, t, false));
    CHECK(sub.target.find('\n') != std::string::npos);
  }
  // a unit of two source lines (the CLI hands the lines over in the spans) keeps two lines; the re-check of an edited
  // text unit has no line-length limit
  rules::CueInput c;
  c.sourceText = "The girl sings. The boy reads a book.";
  for (const char* p : {"The girl sings.", "\n", "The boy reads a book."}) {
    rules::SpanIn sp;
    sp.raw = p;
    c.spans.push_back(sp);
  }
  for (rules::Lang t : {rules::Lang::La, rules::Lang::Grc}) {
    auto r = e->translate({c}, optQ(rules::Lang::En, t, true), rules::Context{}, nullptr, nullptr);
    REQUIRE(r.ok());
    INFO(r.value()[0].target);
    CHECK(std::count(r.value()[0].target.begin(), r.value()[0].target.end(), '\n') == 1);
  }
  auto chk = e->check(c, "Agricola cum fīliō suō cotīdiē ā parvō praediō ad magnam urbem ambulat et cantat.",
                      optQ(rules::Lang::En, rules::Lang::La, true), rules::Context{});
  REQUIRE(chk.ok());
  CHECK(checkOkQ(chk.value(), "A8"));
}

// ---- (a, b, c, e, g) the English frame ------------------------------------------------------------------------------
TEST_CASE("rules-q: English tagging follow-ups, Latin side (items a b c e)") {
  NEED_REALQ();
  auto e = engineQ();
  const rules::Options o = optQ(rules::Lang::En, rules::Lang::La);
  struct C { const char* en; const char* la; };
  const C exact[] = {
      {"The well supplies water.", "Puteus aquam praebet."},                          // (a) C37's open sentence
      {"The sun shines and the farmer groans.", "Sōl lūcet et agricola gemit."},      // (b)
      {"The old woman smiles and waters the garden.", "Anus rīdet et hortum irrigat."},
      {"Stephen unties the oxen and drives them to the field.", "Stephanus bovēs solvit et eōs ad agrum pellit."},
      {"The farmer lifts the stone and carries it out of the field.", "Agricola saxum tollit et id ex agrō portat."},
      {"O Jupiter, help the sailors!", "Ō Iuppiter, adiuvā nautās!"},                 // (c)
      {"The Greek slave reads a book.", "Servus Graecus librum legit."},               // (e)
      {"He has not much money.", "Multam pecūniam nōn habet."},
      {"My grandmother tells stories.", "Avia mea fābulās nārrat."},                    // guard: the first -s word
  };
  for (const C& c : exact) {
    const rules::CueOutput r = oneQ(*e, c.en, o);
    CHECK_MESSAGE(r.target == c.la, c.en << " -> " << r.target << " (" << confQ(r.confidence) << ")");
    CHECK(r.confidence != rules::Confidence::Fix);
  }
  struct P { const char* en; const char* part; };
  const P parts[] = {
      {"Marcus unties the horse and leads it into the field.", "equum solvit et"},
      {"Sextus honours the gods and works hard.", "deōs honōrat et multum labōrat"},
      {"\"Father, the field is wet.\"", "\"Pater, ager"},
      {"\"Mother, the bread is good.\"", "\"Māter, pānis"},
      {"The Athenian sailor loves the sea.", "Nauta Athēniēnsis"},
      {"Marcus is a Roman farmer.", "Rōmānus"},
      {"We have enough water.", "satis"},
  };
  for (const P& c : parts) {
    const rules::CueOutput r = oneQ(*e, c.en, o);
    CHECK_MESSAGE(r.target.find(c.part) != std::string::npos, c.en << " -> " << r.target);
  }
  // (e) the Spanish source too (gentilicios are written in lower case in Spanish)
  const rules::Options oes = optQ(rules::Lang::Es, rules::Lang::La);
  for (const P& c : {P{"La niña ateniense canta en el jardín.", "Athēniēnsis"}, P{"El maestro es griego.", "Graecus"}}) {
    const rules::CueOutput r = oneQ(*e, c.en, oes);
    CHECK_MESSAGE(r.target.find(c.part) != std::string::npos, c.en << " -> " << r.target);
  }
  // (a) a sentence whose only verb is a word far more often a noun is never OK; an order with that verb is plain
  const rules::CueOutput w = oneQ(*e, "The girl waters the roses.", o);
  CHECK(w.confidence != rules::Confidence::Ok);
  CHECK(flagQ(w, "verb-doubt"));
  CHECK(!flagQ(oneQ(*e, "Water the flowers!", o), "verb-doubt"));
  // guards: a verbless noun phrase stays one (no OK verb made of a plural noun), the subject of an -s verb that ends
  // the clause is a person, an animal or a thing that acts by itself
  CHECK(oneQ(*e, "The house plants.", o).confidence != rules::Confidence::Ok);
  // (c) "for" and "so" opening a clause were already right after C37 (kept)
  CHECK(oneQ(*e, "For the road is long, and the boy is tired.", o).target == "Nam via longa est et puer fessus est.");
  CHECK(oneQ(*e, "So the farmer goes home.", o).target == "Itaque agricola domum it.");
}

TEST_CASE("rules-q: EN -> GRC enough is ἱκανός, never βουπόρος; the ellipsis is never OK (item g)") {
  NEED_REALQ();
  auto e = engineQ();
  const rules::Options o = optQ(rules::Lang::En, rules::Lang::Grc);
  for (const char* s : {"The farmer has enough food.", "We have enough water.", "The garden gives fruit: not much, but enough.",
                        "We have water, not much, but enough.", "The river provides water, not much but enough."}) {
    const rules::CueOutput r = oneQ(*e, s, o);
    INFO(s << " -> " << r.target);
    CHECK(r.target.find("βουπόρ") == std::string::npos);
    if (std::string(s).find("not much") != std::string::npos) CHECK(r.confidence != rules::Confidence::Ok);
    else CHECK((r.target.find("ἱκαν") != std::string::npos || r.target.find("Ἱκαν") != std::string::npos));
  }
  CHECK(oneQ(*e, "The farmer has enough food.", o).target == "Ὁ γεωργὸς ἱκανὸν σῖτον ἔχει.");
  // "although" without a concessive Greek word is never OK (the length Check of a wrapped text unit hid it)
  const rules::CueOutput a = oneQ(*e, "Although he is tired, the farmer works.", o);
  CHECK(a.confidence != rules::Confidence::Ok);
}

// ---- (f, h) Latin / Greek -> English ----------------------------------------------------------------------------------
TEST_CASE("rules-q: la2x abstract subjects, one's own master, nōn multum sed satis (item f)") {
  NEED_REALQ();
  auto e = engineQ();
  struct C { const char* la; const char* en; const char* es; };
  const C cases[] = {
      {"Labor longus est, sed somnus dulcis est.", "Work is long, but sleep is sweet.", nullptr},
      {"Amīcitia bona est.", "Friendship is good.", "La amistad es buena."},
      {"Lūcius līber est et dominus suus.", "Lucius is free and his own master.", "Lucio es libre y dueño de sí mismo."},
      {"Servus nōn est dominus suus.", "The slave is not his own master.", "El esclavo no es dueño de sí mismo."},
      {"Līber enim est et dominus.", "For he is free and his own master.", nullptr},
      {"Pecūniam nōn multam sed satis habet.", "He has not much money but enough.", "No tiene mucho dinero sino bastante."},
      {"Fīlia eius nōn multum sed satis labōrat.", "His daughter works not much but enough.", nullptr},
      {"Vīta agricolae difficilis est.", "The life of the farmer is difficult.", nullptr},   // a genitive keeps "the"
      {"Mārcus dominus est.", "Marcus is a master.", nullptr},                              // no suus, no līber
  };
  for (const C& c : cases) {
    const rules::CueOutput en = oneQ(*e, c.la, optQ(rules::Lang::La, rules::Lang::En));
    CHECK_MESSAGE(en.target == c.en, c.la << " -> " << en.target);
    if (c.es) {
      const rules::CueOutput es = oneQ(*e, c.la, optQ(rules::Lang::La, rules::Lang::Es));
      CHECK_MESSAGE(es.target == c.es, c.la << " -> " << es.target);
    }
  }
  // an abstract noun named before stays without the article as a general subject
  const std::vector<rules::CueOutput> two =
      runQ(*e, {"Vīta difficilis est.", "Ō Iuppiter, difficilis est vīta."}, optQ(rules::Lang::La, rules::Lang::En));
  CHECK(two[1].target == "O Jupiter, life is difficult.");
}

TEST_CASE("rules-q: ἔργον is the noun when nothing else decides (item h)") {
  NEED_REALQ();
  auto e = engineQ();
  const rules::CueOutput r = oneQ(*e, "ἔργον ἔχει.", optQ(rules::Lang::Grc, rules::Lang::En));
  INFO(r.target);
  CHECK(r.target.find("work") != std::string::npos);
  CHECK(r.target.find("clos") == std::string::npos);
  CHECK(oneQ(*e, "τὸ ἔργον χαλεπόν ἐστιν.", optQ(rules::Lang::Grc, rules::Lang::En)).target == "The work is difficult.");
}

// ---- own material end to end (text project, one job per file) --------------------------------------------------------
TEST_CASE("rules-q: own material vs gold (tests/regression/own_frame*.txt; report regression_report_q.txt)") {
  NEED_REALQ();
  auto e = engineQ();
  struct F { const char* src; const char* gold; rules::Lang s, t; int floor; int okMissMax; };
  // floors: the counts after C39 (first run before any rule: 7 / 18, 2 / 4, 1 / 7, 0 / 4, 3 / 6); grc-en keeps the
  // two αὐτουργός units of item (h) that rows alone cannot fix (docs/rules_grc3_notes.md "Frame follow-ups (C39)")
  const F files[] = {
      {"own_frame.en.txt", "own_frame.la.gold.txt", rules::Lang::En, rules::Lang::La, 15, 0},
      {"own_frame.es.txt", "own_frame.es.la.gold.txt", rules::Lang::Es, rules::Lang::La, 2, 0},
      {"own_frame.la.txt", "own_frame.en.gold.txt", rules::Lang::La, rules::Lang::En, 7, 0},
      {"own_frame_grc.en.txt", "own_frame_grc.grc.gold.txt", rules::Lang::En, rules::Lang::Grc, 2, 0},
      {"own_frame_grc.grc.txt", "own_frame_grc.en.gold.txt", rules::Lang::Grc, rules::Lang::En, 3, 2},
  };
  std::ostringstream rep;
  for (const F& f : files) {
    const std::vector<std::string> src = linesQ(repoQ() / "tests" / "regression" / f.src);
    const std::vector<std::string> gold = linesQ(repoQ() / "tests" / "regression" / "expected" / f.gold);
    REQUIRE(src.size() == gold.size());
    const rules::Options o = optQ(f.s, f.t);
    const std::vector<rules::CueOutput> out = runQ(*e, src, o);
    REQUIRE(out.size() == src.size());
    int right = 0, ok = 0, okMiss = 0;
    rep << "== " << f.src << "\n";
    for (size_t i = 0; i < src.size(); ++i) {
      const bool m = matchQ(out[i].target, gold[i]);
      right += m;
      if (out[i].confidence == rules::Confidence::Ok) {
        ++ok;
        okMiss += !m;
      }
      CHECK(out[i].target.find('\n') == std::string::npos);   // a text project: one line per one-line unit
      rep << (i + 1) << "\t" << (m ? "=" : "x") << "\t" << out[i].target << "\t" << confQ(out[i].confidence) << "\n";
    }
    rep << "right " << right << " / " << src.size() << "; ok " << ok << " (mismatches among OK " << okMiss << ")\n";
    MESSAGE(std::string(f.src) << ": right " << right << " / " << src.size() << ", ok " << ok << ", mismatches among OK " << okMiss);
    CHECK(right >= f.floor);
    CHECK(okMiss <= f.okMissMax);
    // deterministic: the same job twice gives the same bytes
    const std::vector<rules::CueOutput> again = runQ(*e, src, o);
    for (size_t i = 0; i < src.size(); ++i) CHECK(again[i].target == out[i].target);
  }
  std::ofstream(buildQ() / "regression_report_q.txt") << rep.str();
}
