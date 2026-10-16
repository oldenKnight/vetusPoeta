// engine/rules Greek loop 9 (C35): Spanish source into Greek. The regression on the 120 own Mexican-Spanish cues of
// tests/regression/own_dialogue2.es.srt against the Attic gold tests/regression/expected/own_dialogue2.grc.gold.txt
// (written before the engine was run on the file), the Spanish constructions of the loop (two or more own sentences
// per rule, written before the rule: docs/rules_grc3_notes.md "Loop 9") and the fixes after the blind check. Needs the
// real data (VP_DATA_WORK); skips otherwise.
#include <doctest.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "vp/curated.h"
#include "vp/engine_config.h"
#include "vp/frame.h"
#include "vp/realise_grc.h"
#include "vp/transfer_grc.h"
#include "vp/lex.h"
#include "vp/nlp.h"
#include "vp/rules.h"
#include "vp/subs.h"
#include "vp/text.h"

namespace stdfs = std::filesystem;
using namespace vp;

namespace {

stdfs::path repo9() { return stdfs::path(VP_FIXTURES_DIR).parent_path().parent_path(); }
stdfs::path work9() {
  const char* e = std::getenv("VP_DATA_WORK");
  return e && *e ? stdfs::path(e) : repo9() / "data" / "work";
}
stdfs::path buildDir9() { return stdfs::path(VP_TEST_TMP).parent_path(); }

struct Real9 {
  lex::Lexicon grc, en, es;
  nlp::Pipeline pes;
  bool ok = false;
  std::string why;
};
const Real9& real9() {
  static Real9 r = [] {
    Real9 x;
    const char* g = std::getenv("VP_GREEK_VPL");
    auto grc = lex::Lexicon::open(g && *g ? stdfs::path(g) : work9() / "greek.vpl");
    auto en = lex::Lexicon::open(work9() / "english.vpl");
    auto es = lex::Lexicon::open(work9() / "spanish.vpl");
    auto pes = nlp::Pipeline::open(nlp::Lang::Es, (work9() / "nlp" / "spanish.tag.vpt").string(),
                                   (work9() / "nlp" / "spanish.dep.vpt").string());
    if (!grc.ok() || !en.ok() || !es.ok() || !pes.ok()) {
      x.why = !grc.ok() ? grc.error().message : !en.ok() ? en.error().message : !es.ok() ? es.error().message
                                                                                         : pes.error().message;
      return x;
    }
    x.grc = std::move(grc.value());
    x.en = std::move(en.value());
    x.es = std::move(es.value());
    x.pes = std::move(pes.value());
    x.ok = true;
    return x;
  }();
  return r;
}
#define NEED_REAL9()                                                                                          \
  if (!real9().ok) {                                                                                          \
    MESSAGE("real Greek / Spanish data not available (" << real9().why << "); set VP_DATA_WORK. Skipped.");  \
    return;                                                                                                   \
  }

std::unique_ptr<rules::Engine> engine9() {
  rules::EngineConfig cfg = rules::defaultEngineConfig();
  cfg.curatedDir = (repo9() / "data" / "curated").string();
  cfg.dataDir = work9().string();
  cfg.nlpDir = (work9() / "nlp").string();
  auto e = rules::makeEngine(cfg);
  e->setLexicons(nullptr, &real9().grc, &real9().en, &real9().es);
  return e;
}

// As the other Greek regressions: NFC, lower case, punctuation-insensitive, spaces collapsed; accents and breathings
// count.
std::string norm9(const std::string& s) {
  std::string l = text::lower(text::nfc(s));
  std::string out;
  size_t i = 0;
  while (i < l.size()) {
    size_t j = i;
    const char32_t c = text::decodeUtf8(l, j);
    const bool punct = c == '.' || c == ',' || c == ';' || c == '!' || c == '?' || c == ':' || c == 0xB7 ||
                       c == 0x387 || c == 0x37E || c == '"' || c == 0xA1 || c == 0xBF;
    if (punct || c == ' ' || c == '\n') {
      if (!out.empty() && out.back() != ' ') out += ' ';
    } else {
      out += l.substr(i, j - i);
    }
    i = j;
  }
  while (!out.empty() && out.back() == ' ') out.pop_back();
  return out;
}

bool matches9(const std::string& got, const std::string& expected) {
  for (size_t a = 0;;) {
    const size_t b = expected.find(" | ", a);
    if (norm9(got) == norm9(text::nfc(expected.substr(a, b == std::string::npos ? std::string::npos : b - a))))
      return true;
    if (b == std::string::npos) return false;
    a = b + 3;
  }
}

rules::Options opts9() {
  rules::Options o;
  o.source = rules::Lang::Es;
  o.target = rules::Lang::Grc;
  o.fidelity = 2;
  o.speakerGender = 'f';
  return o;
}

const char* confName9(rules::Confidence c) {
  return c == rules::Confidence::Ok ? "ok" : c == rules::Confidence::Check ? "check" : "fix";
}

std::string flat9(std::string s) {
  std::replace(s.begin(), s.end(), '\n', ' ');
  return s;
}

bool hasFlag9(const rules::CueOutput& o, const char* f) {
  return std::find(o.flags.begin(), o.flags.end(), f) != o.flags.end();
}

// One cue at a time; `expected` may list alternatives after " | ". `mustCheck`: the cue must not be OK (a rebuilt
// structure, a repaired misreading, a guess). A cue with a rebuilt / guessed flag is never OK.
struct Case9 { const char* src; const char* expected; bool mustCheck; };
int run9(rules::Engine& e, const std::vector<Case9>& cases, const char* what) {
  int ok = 0;
  for (const Case9& k : cases) {
    rules::CueInput x;
    x.sourceText = k.src;
    auto r = e.translate({x}, opts9(), rules::Context{}, nullptr, nullptr);
    REQUIRE(r.ok());
    const rules::CueOutput& o = r.value()[0];
    const std::string got = flat9(o.target);
    const bool hit = matches9(got, k.expected);
    CHECK_MESSAGE(hit, what << ": " << k.src << " -> '" << got << "' expected '" << k.expected << "'");
    CHECK_MESSAGE(o.confidence != rules::Confidence::Fix, what << ": " << k.src << " is Fix");
    bool rebuilt = false;
    for (const char* f : {"clause-repair", "participle-phrase", "realia", "past-form", "from-rule", "det-adverb",
                          "light-verb", "subject-guess", "speech-inversion", "idiom", "lexicon-gap", "addressee-guess"})
      rebuilt = rebuilt || hasFlag9(o, f);
    if (rebuilt || k.mustCheck)
      CHECK_MESSAGE(o.confidence != rules::Confidence::Ok, what << ": " << k.src << " was rebuilt / guessed but is OK");
    ok += hit;
  }
  MESSAGE(std::string(what) << ": " << ok << " / " << cases.size());
  return ok;
}

std::vector<rules::CueInput> srtCues9(const char* file) {
  std::ifstream f(repo9() / "tests" / "regression" / file, std::ios::binary);
  std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)), {});
  auto d = subs::parse(b, subs::Format::Srt);
  REQUIRE(d.ok());
  std::vector<rules::CueInput> in;
  for (size_t i = 0; i < d->cues.size(); ++i) {
    rules::CueInput c;
    c.index = (uint32_t)in.size();
    c.sourceText = d->cues[i].plainText();
    int64_t s = 0, t = 0;
    subs::parseTiming(d->cues[i].timingRaw, subs::Format::Srt, s, t);
    c.startMs = s;
    c.endMs = t;
    in.push_back(c);
  }
  return in;
}

}  // namespace

// Debug: Spanish cues of a file into Greek, each alone (VP_GRC9_TRY=<file>; VP_GRC9_WHY=1 adds the reasons).
TEST_CASE("rules-grc9: try (VP_GRC9_TRY=<file>)") {
  const char* env = std::getenv("VP_GRC9_TRY");
  if (!env || !*env) return;
  NEED_REAL9();
  auto e = engine9();
  std::ifstream f(env);
  std::string line;
  while (std::getline(f, line)) {
    if (line.empty() || line[0] == '#') continue;
    rules::CueInput c;
    c.sourceText = line;
    auto r = e->translate({c}, opts9(), rules::Context{}, nullptr, nullptr);
    REQUIRE(r.ok());
    const rules::CueOutput& o = r.value()[0];
    std::string extra;
    for (const auto& fl : o.flags) extra += " " + fl;
    std::cout << line << "\t" << flat9(o.target) << "\t" << confName9(o.confidence) << " |" << extra << "\n";
    if (std::getenv("VP_GRC9_WHY"))
      for (const auto& k : o.checks)
        if (!k.ok || k.detail.rfind("warning", 0) == 0) std::cout << "      " << k.id << ": " << k.detail << "\n";
    if (std::getenv("VP_GRC9_WHY"))
      for (const auto& w : o.reasons) std::cout << "      reason " << w.kind << ": " << w.text << "\n";
    if (std::getenv("VP_GRC9_FRAME")) {   // the shared frame builder as the Greek engine sets it up
      static curated::CuratedData cg = [] {
        auto cl = curated::CuratedData::load(repo9() / "data" / "curated");
        REQUIRE(cl.ok());
        return std::move(cl.value());
      }();
      static bool once = [] {
        auto gd = grc::GreekData::load(repo9() / "data" / "curated");
        auto gt = grc::GreekTables::load(repo9() / "data" / "curated");
        REQUIRE(gd.ok());
        REQUIRE(gt.ok());
        cg.replacePhrasebooks(gd.value().phrasebook(), gt.value().phrasebookEs());
        return true;
      }();
      (void)once;
      frame::FrameBuilder fb(frame::SrcLang::Es, &real9().pes, &real9().es, cg);
      fb.setCommaClauses(true);
      for (const auto& ss : frame::mapSentences({line})) {
        frame::SemSentence sm;
        fb.analyse(ss.text, sm);
        std::string tk;
        for (const auto& t : sm.tokens)
          tk += " " + t.text + "/" + t.upos + "/" + t.deprel + ">" + std::to_string(t.head) + "(" + fb.lemmaOf(t) + ")";
        std::cout << "      tokens:" << tk << "\n      frame: " << frame::describe(sm) << "\n";
      }
    }
  }
}

TEST_CASE("rules-grc9: ES -> GRC regression on own_dialogue2.es.srt (120 cues) vs the Greek gold") {
  NEED_REAL9();
  auto e = engine9();
  std::vector<rules::CueInput> in = srtCues9("own_dialogue2.es.srt");
  std::ifstream g(repo9() / "tests" / "regression" / "expected" / "own_dialogue2.grc.gold.txt");
  std::vector<std::string> gold;
  std::string line;
  while (std::getline(g, line))
    if (!line.empty() && line[0] != '#') gold.push_back(line);
  REQUIRE(gold.size() == 120);
  REQUIRE(in.size() == 120);
  const size_t n = gold.size();
  const rules::Options o = opts9();
  auto r1 = e->translate(in, o, rules::Context{}, nullptr, nullptr);
  REQUIRE(r1.ok());
  REQUIRE(r1->size() == n);
  int matches = 0;
  std::map<std::string, int> conf, confMiss;
  std::ostringstream table, all;
  table << "| # | source | gold | ours | checks |\n|---|---|---|---|---|\n";
  for (size_t i = 0; i < n; ++i) {
    const rules::CueOutput& c = r1.value()[i];
    ++conf[confName9(c.confidence)];
    const std::string ours = flat9(c.target);
    const bool match = matches9(ours, gold[i]);
    matches += match;
    if (!match) {
      ++confMiss[confName9(c.confidence)];
      std::string chk;
      for (const auto& k : c.checks)
        if (!k.ok) chk += k.id + " ";
      std::string g2 = gold[i].substr(0, gold[i].find(" | "));
      table << "| " << i + 1 << " | " << in[i].sourceText << " | " << g2 << " | " << ours << " | "
            << confName9(c.confidence) << (chk.empty() ? "" : " " + chk) << "|\n";
    }
    std::string why;
    for (const auto& k : c.checks)
      if (!k.ok || k.detail.rfind("warning", 0) == 0) why += k.id + "(" + k.detail + ") ";
    for (const auto& f : c.flags) why += "[" + f + "] ";
    all << i + 1 << "\t" << (match ? "=" : "x") << "\t" << flat9(in[i].sourceText) << "\t" << ours << "\t"
        << confName9(c.confidence) << "\t" << why << "\n";
  }
  std::ostringstream rep;
  rep << "Regression own_dialogue2.es.srt (" << n << " cues) -> Attic Greek, fidelity 2, speaker f\n";
  rep << "match rate (normalised: NFC, case- and punctuation-insensitive, accents count, any gold alternative): "
      << matches << " / " << n << "\n";
  rep << "confidence: ok " << conf["ok"] << ", check " << conf["check"] << ", fix " << conf["fix"]
      << "; mismatches: ok " << confMiss["ok"] << ", check " << confMiss["check"] << ", fix " << confMiss["fix"]
      << "\n\nMismatches:\n" << table.str() << "\nAll outputs:\n" << all.str();
  std::ofstream(buildDir9() / "regression_report_grc_es2.txt") << rep.str();
  MESSAGE("ES -> GRC own_dialogue2: " << matches << " / " << n << "; confidence ok " << conf["ok"] << " / check "
                                      << conf["check"] << " / fix " << conf["fix"] << " (mismatches rated OK: "
                                      << confMiss["ok"] << "); report "
                                      << (buildDir9() / "regression_report_grc_es2.txt").string());
  CHECK(matches >= 118);   // C35: 17 / 120 at the first run; 118 / 120 at the end (gold + the proposed alternatives)
  // determinism: a second engine gives byte-identical cues
  auto e2 = engine9();
  auto r2 = e2->translate(in, o, rules::Context{}, nullptr, nullptr);
  REQUIRE(r2.ok());
  bool same = true;
  for (size_t i = 0; i < n; ++i) same = same && r1.value()[i].target == r2.value()[i].target;
  CHECK(same);
}

// C35: the rules of the loop, each with two or more own sentences (none from own_dialogue2.es.txt or the blind list),
// written at 18:30 UTC after the regression rules (docs/rules_grc3_notes.md "Loop 9"). Each sentence is translated alone.
// Cues the shared frame builder still misparses are not listed here (API changes, "Wish for frame/").
TEST_CASE("rules-grc9: C35 constructions (Spanish source)") {
  NEED_REAL9();
  auto e = engine9();
  const std::vector<Case9> es = {
      {"Mateo come pan.", "ὁ Ματθαῖος ἄρτον ἐσθίει.", false},
      {"Sofía, ¿dónde estás?", "ὦ Σοφία, ποῦ εἶ;", false},
      {"Mi cuarto es pequeño.", "τὸ δωμάτιόν μου μικρόν ἐστιν.", false},
      {"Duermo en mi cuarto.", "ἐν τῷ δωματίῳ μου καθεύδω.", false},
      {"Tengo un lápiz nuevo.", "καινὴν γραφίδα ἔχω.", false},
      {"Mi perrito duerme.", "τὸ κυνίδιόν μου καθεύδει.", false},
      {"Me pongo el abrigo.", "τὸ ἱμάτιον ἐνδύομαι.", false},
      {"Ponte los zapatos.", "ἔνδυσαι τὰ ὑποδήματα.", false},
      {"¡Juan, levántate!", "ὦ Ἰωάννη, ἀνάστηθι!", false},
      {"Levántate, hijo.", "ἀνάστηθι, ὦ υἱέ.", true},
      {"Sofía me quitó la pelota.", "ἡ Σοφία μοι τὴν σφαῖραν ἀφείλετο.", false},
      {"Te lo juro, papá.", "ὄμνυμί σοι, ὦ πάτερ.", true},
      {"¡Cállate, Mateo!", "σίγα, ὦ Ματθαῖε!", true},
      {"Mi hermana se enojó.", "ἡ ἀδελφή μου ὠργίσθη.", false},
      {"Temo que llueva mañana.", "φοβοῦμαι μὴ αὔριον ὕσῃ.", false},
      {"Temo que el perro se escape.", "φοβοῦμαι μὴ ὁ κύων φύγῃ.", false},
      {"La maestra me dijo que me sentara.", "ἡ διδάσκαλός με ἐκέλευσε καθῆσθαι.", false},
      {"El niño seguía llorando.", "ὁ παῖς ἔτι ἔκλαιεν.", false},
      {"Los perros seguían ladrando.", "οἱ κύνες ἔτι ὑλάκτουν.", false},
      {"El gato me da miedo.", "τὴν γαλῆν φοβοῦμαι.", false},
      {"Papá está enojado conmigo.", "ὁ πατήρ μοι ὀργίζεται.", true},
      {"La maestra está enojada con los niños.", "ἡ διδάσκαλος τοῖς παισὶν ὀργίζεται.", false},
      {"Durante el invierno dormimos mucho.", "τὸν χειμῶνα πολὺ καθεύδομεν.", false},
      {"Caminamos durante tres horas.", "τρεῖς ὥρας βαδίζομεν.", false},
      {"Se metió en la casa.", "εἰς τὸν οἶκον εἰσῆλθεν.", false},
      {"No lo vuelvo a decir.", "οὐκέτι λέξω.", false},
      {"No lo vuelvo a hacer nunca.", "αὖθις οὐδέποτε τοῦτο ποιήσω.", false},
      {"Lo voy a hacer mañana.", "αὔριον τοῦτο ποιήσω.", false},
      {"Alguien te quitó tu libro.", "τὸ βιβλίον σου ἀφείλετό τις.", false},
      {"Mi hermano me rompió mi juguete.", "ὁ ἀδελφός μου τὸ παίγνιόν μου κατέαξεν.", false},
      {"El niño perdió su libro.", "ὁ παῖς τὸ βιβλίον ἀπώλεσεν.", false},
      {"Escriban su nombre.", "γράφετε τὸ ὄνομα ὑμῶν.", false},
      {"Mira, aquí está tu libro.", "ἰδού, ἐνθάδε τὸ βιβλίον σού ἐστιν.", false},
      {"Había una vez un rey muy viejo.", "ἦν ποτε βασιλεὺς πάνυ γέρων.", false},
      {"Había una vez una niña pobre.", "ἦν ποτε παῖς ἀκτήμων.", false},
      {"Que duerman bien, niños.", "καθεύδετε εὖ, ὦ παῖδες.", true},
      {"Que coman bien, hijos.", "φάγετε εὖ, ὦ υἱεῖς.", true},
      {"¿Hay pan?", "ἆρα ἔστιν ἄρτος;", false},
      {"¿Hay manzanas?", "ἆρα ἔστι μῆλα;", false},
      {"¡Qué grandes!", "ὡς μεγάλα!", false},
      {"¡Qué pequeñas!", "ὡς μικραί!", false},
      {"Aquí está el tuyo.", "ἐνθάδε ὁ σός ἐστιν.", true},
      {"¡Yo, papá!", "ἐγώ, ὦ πάτερ!", false},
      {"¡Nosotros, maestra!", "ἡμεῖς, ὦ διδάσκαλε!", false},
      {"No, mi amor. Ya es tarde.", "οὔ, ὦ τέκνον. ἤδη ὀψέ ἐστιν.", true},
      {"Sí, mi vida.", "ναί, ὦ τέκνον.", true},
      {"Llegué hace dos días.", "πρὸ δυοῖν ἡμερῶν ἀφικόμην.", false},
      {"Comimos hace una hora.", "πρὸ ὥρας ἐφάγομεν.", false},
      {"Ella siempre deja la ventana abierta.", "ἀεὶ τὴν θυρίδα ἀνεῳγμένην καταλείπει.", false},
      {"Siempre la deja cerrada.", "ἀεὶ αὐτὴν κεκλειμένην καταλείπει.", false},
      {"¿Por qué tienes los pies sucios?", "διὰ τί τοὺς πόδας ῥυπαροὺς ἔχεις;", false},
      {"Tienes las manos frías.", "τὰς χεῖρας ψυχρὰς ἔχεις.", false},
      {"La niña caminó sola.", "ἡ παῖς μόνη ἐβάδισεν.", true},
      {"¿Dónde estabas, niño? Estaba muy cansada.", "ποῦ ἦσθα, ὦ παῖ; πάνυ ἔκαμνον.", true},
      {"Mamá, ¿quieres que te lleve?", "ὦ μῆτερ, ἆρα βούλει μέ σε ἀγαγεῖν;", true},
      {"Papá, ¿quieres que te ayude?", "ὦ πάτερ, ἆρα βούλει μέ σοι βοηθεῖν;", true},
      {"Hay que comer bien.", "δεῖ εὖ φαγεῖν.", false},
      {"¿Quién quiere jugar primero?", "τίς βούλεται πρῶτον παίζειν;", false},
      {"¿Quién quiere hablar primero?", "τίς βούλεται πρῶτον λαλεῖν;", false},
      {"Gracias, eres muy bueno.", "χάριν οἶδα, πάνυ ἀγαθὸς εἶ.", false},
      {"Eres muy amable, niño.", "πάνυ χρηστὸς εἶ, ὦ παῖ.", true},
      {"Sí, pero tengo frío.", "ναί, ἀλλὰ ῥιγῶ.", false},
      {"Me gustó mucho el cuento.", "ὁ μῦθός μοι σφόδρα ἤρεσεν.", false},
      {"Mi papá se enojó mucho.", "ὁ πατήρ μου σφόδρα ὠργίσθη.", false},
      {"Me duele la mano.", "ἡ χείρ μοι ἀλγεῖ.", false},
      {"Me duele el pie.", "ὁ πούς μοι ἀλγεῖ.", false},
      {"¡Qué ricas uvas!", "ὡς ἡδεῖαι αἱ σταφυλαί!", false},
      {"¡Qué rico pan!", "ὡς ἡδὺς ὁ ἄρτος!", false},
      {"¿Cómo se dice perro en griego?", "πῶς κύων λέγεται Ἑλληνιστί;", false},
      {"Se dice que el río es largo.", "λέγουσιν ὅτι ὁ ποταμὸς μακρός ἐστιν.", true},
      {"No le jales el pelo a tu hermana.", "μὴ ἕλκε τὴν κόμην τῆς ἀδελφῆς σου.", false},
      {"Le corté la cola al perro.", "τὴν οὐρὰν τοῦ κυνὸς ἀπέτεμον.", false},
      {"Me lavo las manos.", "τὰς χεῖρας νίζομαι.", false},
      {"Estoy pensando en mi madre.", "περὶ τῆς μητρός μου ἐνθυμοῦμαι.", false},
      {"Pienso en mi abuela.", "περὶ τῆς τήθης μου ἐνθυμοῦμαι.", false},
      {"Mi amiga Ana canta.", "ἡ φίλη μου Ἄννα ᾄδει.", false},
      {"Vi a mi amigo Pablo.", "τὸν φίλον μου Παῦλον εἶδον.", false},
      {"El niño está dormido.", "ὁ παῖς καθεύδει.", false},
      {"La gata está escondida.", "ἡ γαλῆ κρυπτή ἐστιν.", false},
      {"Ven a la mesa, Pablo.", "ἐλθὲ πρὸς τὴν τράπεζαν, ὦ Παῦλε.", true},
      {"Vamos a la mesa.", "πρὸς τὴν τράπεζαν βαίνομεν.", false},
      {"El mar está lejos.", "ἡ θάλαττα μακράν ἐστιν.", false},
      {"Mi casa está muy lejos.", "ὁ οἶκός μου μακρὰν πάνυ ἐστίν.", false},
      {"Por fin llegamos.", "τέλος ἀφικνούμεθα.", false},
      {"Por fin terminó la guerra.", "ὁ πόλεμος τέλος ἐτελεύτησεν.", false},
      {"En la noche llovió.", "νύκτωρ ὗσεν.", false},
      {"A la mañana siguiente salimos.", "τῇ ὑστεραίᾳ ἐξερχόμεθα.", false},
  };
  const int ok = run9(*e, es, "C35 constructions ES");
  CHECK(ok == (int)es.size());
}

// C35 after the blind check: own sentences written at 18:27 UTC, after the blind run and before the fixes; none is a
// blind sentence ("El viejo pescador durmió." was added for the -dor / -ero person nouns).
TEST_CASE("rules-grc9: C35 after the blind check") {
  NEED_REAL9();
  auto e = engine9();
  const std::vector<Case9> es = {
      {"¡Cuidado, la sopa está caliente!", "εὐλαβοῦ, ὁ ζωμὸς θερμός ἐστιν!", false},
      {"¡Cuidado, el perro muerde!", "εὐλαβοῦ, ὁ κύων δάκνει!", false},
      {"¿Me prestas tu lápiz?", "ἆρα δίδως μοι τὴν γραφίδα σου;", true},
      {"Le presté mi pelota a Juan.", "τῷ Ἰωάννῃ τὴν σφαῖράν μου ἔδωκα.", true},
      {"Mi papá compró pan.", "ὁ πατήρ μου ἄρτον ἠγόρασεν.", false},
      {"Compramos una casa nueva.", "καινὸν οἶκον ἀγοράζομεν.", false},
      {"Los niños hacen mucho ruido.", "οἱ παῖδες θόρυβον πολὺ ποιοῦσιν.", false},
      {"No hagan ruido, por favor.", "μὴ ποιεῖτε θόρυβον, ἀντιβολῶ.", false},
      {"El viejo pastor durmió.", "ὁ γέρων ποιμὴν ἐκάθευδεν.", false},
      {"El viejo marinero cantó.", "ὁ γέρων ναύτης ᾖσεν.", false},
      {"Niño, ¿cuántos años tienes?", "ὦ παῖ, πόσων ἐτῶν εἶ;", false},
      {"¿Cuántos años tiene usted, señora?", "πόσων ἐτῶν εἶ, ὦ δέσποινα;", true},
      {"Mi madre me enseñó a nadar.", "ἡ μήτηρ μού με ἐδίδαξε νεῖν.", false},
      {"El maestro nos enseña a leer.", "ὁ διδάσκαλος ἡμᾶς διδάσκει ἀναγιγνώσκειν.", false},
      {"La sopa está deliciosa.", "ὁ ζωμὸς ἡδύς ἐστιν.", false},
      {"Comimos un pan delicioso.", "ἡδὺν ἄρτον ἐφάγομεν.", false},
      {"El niño es más alto que su hermano.", "ὁ παῖς τοῦ ἀδελφοῦ μακρότερός ἐστιν.", true},
      {"Los soldados lucharon con valor.", "οἱ στρατιῶται ἀνδρείως ἐμαχέσαντο.", false},
      {"Defendimos la casa.", "τῷ οἴκῳ ἠμύναμεν.", false},
      {"El viejo pescador durmió.", "ὁ γέρων ἁλιεὺς ἐκάθευδεν.", false},
  };
  const int ok = run9(*e, es, "C35 after blind ES");
  CHECK(ok == (int)es.size());
}
