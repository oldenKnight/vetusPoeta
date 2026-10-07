// engine/rules Greek quality loop 3 (C18): light verbs and verb + object idioms (lexical_en_grc.tsv kind light),
// noun sense consistency in a batch, purpose and result clauses, circumstantial participles, vocatives and
// particles, the Spanish possessive dative, the frame builder's derived English forms on the Greek path. Every rule
// is checked on our own sentences (at least two each, written before the regression file was run: the
// generalisation guard of docs/rules_grc3_notes.md). Needs the real data (VP_DATA_WORK); skips otherwise.
#include <doctest.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "vp/curated.h"
#include "vp/engine_config.h"
#include "vp/frame.h"
#include "vp/lex.h"
#include "vp/morph_grc.h"
#include "vp/nlp.h"
#include "vp/realise_grc.h"
#include "vp/rules.h"
#include "vp/text.h"
#include "vp/transfer_grc.h"

namespace stdfs = std::filesystem;
using namespace vp;

namespace {

stdfs::path repo3() { return stdfs::path(VP_FIXTURES_DIR).parent_path().parent_path(); }
stdfs::path work3() {
  const char* e = std::getenv("VP_DATA_WORK");
  return e && *e ? stdfs::path(e) : repo3() / "data" / "work";
}

struct Real3 {
  lex::Lexicon grc, en, es;
  nlp::Pipeline pen, pes;
  bool ok = false, esOk = false;
  std::string why;
};
const Real3& real3() {
  static Real3 r = [] {
    Real3 x;
    const char* g = std::getenv("VP_GREEK_VPL");
    auto grc = lex::Lexicon::open(g && *g ? stdfs::path(g) : work3() / "greek.vpl");
    auto en = lex::Lexicon::open(work3() / "english.vpl");
    auto pen = nlp::Pipeline::open(nlp::Lang::En, (work3() / "nlp" / "english.tag.vpt").string(),
                                   (work3() / "nlp" / "english.dep.vpt").string());
    if (!grc.ok() || !en.ok() || !pen.ok()) {
      x.why = !grc.ok() ? grc.error().message : !en.ok() ? en.error().message : pen.error().message;
      return x;
    }
    x.grc = std::move(grc.value());
    x.en = std::move(en.value());
    x.pen = std::move(pen.value());
    x.ok = true;
    auto es = lex::Lexicon::open(work3() / "spanish.vpl");
    auto pes = nlp::Pipeline::open(nlp::Lang::Es, (work3() / "nlp" / "spanish.tag.vpt").string(),
                                   (work3() / "nlp" / "spanish.dep.vpt").string());
    if (es.ok() && pes.ok()) {
      x.es = std::move(es.value());
      x.pes = std::move(pes.value());
      x.esOk = true;
    }
    return x;
  }();
  return r;
}
#define NEED_REAL3()                                                                                 \
  if (!real3().ok) {                                                                                 \
    MESSAGE("real Greek data not available (" << real3().why << "); set VP_DATA_WORK. Skipped.");   \
    return;                                                                                          \
  }

std::unique_ptr<rules::Engine> engine3() {
  rules::EngineConfig cfg = rules::defaultEngineConfig();
  cfg.curatedDir = (repo3() / "data" / "curated").string();
  cfg.dataDir = work3().string();
  cfg.nlpDir = (work3() / "nlp").string();
  auto e = rules::makeEngine(cfg);
  e->setLexicons(nullptr, &real3().grc, &real3().en, real3().esOk ? &real3().es : nullptr);
  return e;
}

std::string flat3(std::string s) {
  std::replace(s.begin(), s.end(), '\n', ' ');
  return s;
}

// As the regression: NFC, lower case, punctuation-insensitive, spaces collapsed; accents and breathings count.
std::string norm3(const std::string& s) {
  std::string l = text::lower(text::nfc(s));
  std::string out;
  size_t i = 0;
  while (i < l.size()) {
    size_t j = i;
    const char32_t c = text::decodeUtf8(l, j);
    const bool punct = c == '.' || c == ',' || c == ';' || c == '!' || c == '?' || c == ':' || c == 0xB7 ||
                       c == 0x387 || c == 0x37E || c == '"';
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

const char* conf3(rules::Confidence c) {
  return c == rules::Confidence::Ok ? "ok" : c == rules::Confidence::Check ? "check" : "fix";
}

rules::Options opts3(bool es) {
  rules::Options o;
  o.source = es ? rules::Lang::Es : rules::Lang::En;
  o.target = rules::Lang::Grc;
  o.fidelity = 2;
  o.speakerGender = 'f';
  return o;
}

// One cue at a time (no discourse memory between cases); `expected` may list alternatives after " | ".
struct Case3 { const char* src; const char* expected; };
int runCases(rules::Engine& e, const std::vector<Case3>& cases, bool es, const char* what) {
  int ok = 0;
  for (const Case3& k : cases) {
    rules::CueInput x;
    x.sourceText = k.src;
    auto r = e.translate({x}, opts3(es), rules::Context{}, nullptr, nullptr);
    REQUIRE(r.ok());
    const std::string got = flat3(r.value()[0].target);
    bool hit = false;
    std::string exp = k.expected;
    size_t a = 0;
    for (;;) {
      const size_t b = exp.find(" | ", a);
      const std::string alt = exp.substr(a, b == std::string::npos ? std::string::npos : b - a);
      hit = hit || norm3(got) == norm3(text::nfc(alt));
      if (b == std::string::npos) break;
      a = b + 3;
    }
    CHECK_MESSAGE(hit, what << ": " << k.src << " -> '" << got << "' expected '" << k.expected << "'");
    CHECK_MESSAGE(r.value()[0].confidence != rules::Confidence::Fix, what << ": " << k.src << " is Fix");
    ok += hit;
  }
  MESSAGE(std::string(what) << ": " << ok << " / " << cases.size());
  return ok;
}

}  // namespace

TEST_CASE("rules-grc3: debug frames (VP_GRC3_FRAMES=<file>, VP_GRC3_ES=1 for Spanish)") {
  const char* e = std::getenv("VP_GRC3_FRAMES");
  if (!e || !*e) return;
  NEED_REAL3();
  const char* esv = std::getenv("VP_GRC3_ES");
  const bool es = esv && *esv;
  if (es && !real3().esOk) return;
  static curated::CuratedData cg = [] {
    auto r = curated::CuratedData::load(repo3() / "data" / "curated");
    REQUIRE(r.ok());
    return std::move(r.value());
  }();
  auto gd = grc::GreekData::load(repo3() / "data" / "curated");
  REQUIRE(gd.ok());
  auto gt = grc::GreekTables::load(repo3() / "data" / "curated");
  REQUIRE(gt.ok());
  cg.replacePhrasebooks(gd.value().phrasebook(), gt.value().phrasebookEs());
  frame::FrameBuilder fb(es ? frame::SrcLang::Es : frame::SrcLang::En, es ? &real3().pes : &real3().pen,
                         es ? &real3().es : &real3().en, cg);
  std::ifstream in(e);
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty()) continue;
    for (const auto& ss : frame::mapSentences({line})) {
      frame::SemSentence s;
      fb.analyse(ss.text, s);
      std::cout << line << "\n   " << frame::describe(s);
      for (const std::string& d : s.doubts) std::cout << " [" << d << "]";
      std::cout << "\n";
    }
  }
}

TEST_CASE("rules-grc3: try sentences (VP_GRC3_TRY=<file>, VP_GRC3_ES=1 for Spanish, VP_GRC3_WHY=1)") {
  const char* e = std::getenv("VP_GRC3_TRY");
  if (!e || !*e) return;
  NEED_REAL3();
  const char* esv = std::getenv("VP_GRC3_ES");
  const bool es = esv && *esv;
  if (es && !real3().esOk) return;
  auto en = engine3();
  std::ifstream f(e);
  std::string line;
  std::vector<rules::CueInput> in;
  while (std::getline(f, line)) {
    if (line.empty()) continue;
    rules::CueInput c;
    c.index = (uint32_t)in.size();
    c.sourceText = line;
    in.push_back(c);
  }
  const char* why = std::getenv("VP_GRC3_WHY");
  for (size_t i = 0; i < in.size(); ++i) {   // one cue at a time, as the unit cases
    auto r = en->translate({in[i]}, opts3(es), rules::Context{}, nullptr, nullptr);
    REQUIRE(r.ok());
    const rules::CueOutput& o = r.value()[0];
    std::cout << in[i].sourceText << "\t" << flat3(o.target) << "\t" << conf3(o.confidence);
    for (const auto& fl : o.flags) std::cout << " [" << fl << "]";
    std::cout << "\n";
    if (why && *why) {
      for (const auto& rs : o.reasons) std::cout << "    " << rs.kind << ": " << rs.text << "\n";
      for (const auto& k : o.checks)
        if (!k.ok || k.detail.rfind("warning", 0) == 0) std::cout << "    " << k.id << (k.ok ? " ok " : " FAIL ") << k.detail << "\n";
    }
  }
}

// C18: every rule of the loop on our own sentences (at least two each; none is a regression or sample line).
TEST_CASE("rules-grc3: C18 constructions (light verbs, purpose / result, participles, vocatives, particles)") {
  NEED_REAL3();
  auto e = engine3();
  const std::vector<Case3> en = {
      // light verbs and verb + object idioms (lexical_en_grc.tsv kind light)
      {"I made a mistake.", "ἥμαρτον."},
      {"You made a mistake.", "ἥμαρτες."},
      {"Did you make a mistake?", "ἆρα ἥμαρτες;"},
      {"Everyone makes mistakes.", "πάντες ἁμαρτάνουσιν."},
      {"Let's take a walk.", "περιπατῶμεν."},
      {"The farmer took a walk in the garden.", "ὁ γεωργὸς ἐν τῷ κήπῳ περιεπάτησεν."},
      {"Let's have a rest.", "ἀναπαυώμεθα."},
      {"The horses had a rest.", "οἱ ἵπποι ἀνεπαύσαντο."},
      {"The boy gave a shout.", "ὁ παῖς ἐβόησεν."},
      {"Give a shout!", "βόησον!"},
      {"The doctor made a visit to the king.", "ὁ ἰατρὸς τὸν βασιλέα ἐπεσκέψατο."},
      {"Don't tell lies.", "μὴ ψεύδου."},
      {"The girl asked a question.", "ἡ κόρη ἠρώτησεν."},
      // purpose: the main clause's person, the modal of a purpose clause dropped, bare infinitive by valency
      {"I came here to see you.", "δεῦρο ἦλθον ἵνα σε ἴδω."},
      {"We went to the market to buy bread.", "εἰς τὴν ἀγορὰν ἀπήλθομεν ἵνα ἄρτον ἀγοράσωμεν."},
      {"Open the door so that the cat can come in.", "ἄνοιξον τὴν θύραν ἵνα ἡ γαλῆ εἰσέλθῃ."},
      {"Give me water to drink.", "δός μοι ὕδωρ πιεῖν."},
      // result: ὥστε + indicative (actual), + infinitive (possible); οὕτω before a consonant
      {"The water was so cold that we ran home.", "τὸ ὕδωρ οὕτω ψυχρὸν ἦν ὥστε οἴκαδε ἐδράμομεν."},
      {"The boy ran so fast that he fell.", "ὁ παῖς οὕτω ταχέως ἔδραμεν ὥστε ἔπεσεν."},
      // circumstantial participles agreeing with the subject
      {"Seeing the wolf, the shepherd ran away.", "ὁ ποιμὴν τὸν λύκον ὁρῶν ἔφυγεν."},
      {"Singing a song, the girl walked to the river.", "ἡ κόρη ᾠδὴν ᾄδουσα εἰς τὸν ποταμὸν ἐβάδισεν."},
      {"When the girl saw the cat, she laughed.", "ἡ κόρη τὴν γαλῆν ἰδοῦσα ἐγέλασεν."},
      {"When I saw the queen, I bowed.", "τὴν βασίλειαν ἰδοῦσα προσεκύνησα."},
      {"After we ate dinner, we slept.", "δεῖπνον φαγόντες ἐκαθεύδομεν."},
      {"While he was walking, he sang.", "βαδίζων ᾖσεν."},
      // another subject: the finite clause stays (ἥκω in the past: imperfect)
      {"When the teacher came, the children sat down.", "ἐπεὶ ὁ διδάσκαλος ἧκεν, οἱ παῖδες ἐκάθισαν."},
      // vocatives without the possessive, γάρ second, impersonal "it is late"
      {"Come here, my child.", "ἐλθὲ δεῦρο, ὦ παῖ."},
      {"Mother, look at the bird!", "ὦ μῆτερ, βλέπε πρὸς τὸν ὄρνιν!"},
      {"Let's go home, for it is late.", "οἴκαδε ἴωμεν, ὀψὲ γάρ ἐστιν."},
      {"It is late.", "ὀψέ ἐστιν."},
      {"It is early.", "πρωΐ ἐστιν. | πρῴ ἐστιν."},   // C21: Attic πρῴ
      // a verb without aorist cells: the narrative imperfect; derived English forms (C17) on the Greek path
      {"The dog barked at the moon.", "ὁ κύων πρὸς τὴν σελήνην ὑλάκτει."},
      {"The girl swam in the river.", "ἡ κόρη ἐν τῷ ποταμῷ ἔνευσεν."},
      {"The boy ran to the sea-shore.", "ὁ παῖς εἰς τὸν αἰγιαλὸν ἔδραμεν."},
      {"The children were singing happily.", "οἱ παῖδες ἡδέως ᾖδον."},
      // rules added after the first blind run, each with two own sentences (none of them a blind sentence):
      // an -ing phrase the parser read as an imperative coordinated with the clause
      {"Jumping over the wall, the cat ran away.", "ἡ γαλῆ ὑπὲρ τοῦ τείχους πηδῶσα ἔφυγεν."},
      {"Laughing loudly, the children ran home.", "οἱ παῖδες μέγα γελῶντες οἴκαδε ἔδραμον."},
      // "oh" + a noun before a question: the addressee; "where is X" with X as the subject and its article
      {"Oh small dog, where is your master?", "ὦ μικρὲ κύον, ποῦ ἐστιν ὁ δεσπότης σου;"},
      {"O little child, where is your mother?", "ὦ μικρὲ παῖ, ποῦ ἐστιν ἡ μήτηρ σου;"},
      // the verb chosen by its subject (kind subject), "wake up" in the passive (phrasal frame pass)
      {"When the sun rose, the birds sang.", "ἐπεὶ ὁ ἥλιος ἀνέτειλεν, οἱ ὄρνιθες ᾖσαν."},
      {"The moon rose over the hill.", "ἡ σελήνη ὑπὲρ τοῦ ὄρους ἀνέτειλεν."},
      {"The children woke up early.", "οἱ παῖδες πρωῒ ἠγέρθησαν. | οἱ παῖδες πρῲ ἠγέρθησαν."},
      {"The king woke up.", "ὁ βασιλεὺς ἠγέρθη."},
      // "all" + a singular time noun: the accusative of duration
      {"The baby slept all night.", "τὸ βρέφος πᾶσαν τὴν νύκτα ἐκάθευδεν."},
      {"We waited all day.", "πᾶσαν τὴν ἡμέραν ἐμείναμεν."},
      // "old man" / "old woman" as one noun
      {"The old woman is wise.", "ἡ γραῦς σοφή ἐστιν."},
      {"An old man lives in the house.", "γέρων ἐν τῇ οἰκίᾳ οἰκεῖ."},
      // a statement after a command without a conjunction: γάρ; "or" after a command / must: εἰ δὲ μή
      {"Run, the wolf is coming!", "τρέχε, ὁ γὰρ λύκος ἔρχεται!"},
      {"Come quickly, the king is waiting!", "ἐλθὲ ταχέως, ὁ γὰρ βασιλεὺς μένει!"},
      {"You must eat, or you will be hungry.", "δεῖ σε φαγεῖν, εἰ δὲ μή, πεινήσεις."},
      {"Hurry up, or we will be late.", "σπεῦσον, εἰ δὲ μή, ὑστερήσομεν. | σπεῦδε, εἰ δὲ μή, ὑστερήσομεν."},   // C21: σπεύδω durative (σπεῦδε)
      // "it" for the animal of the main clause takes the state verb; the degree word stays with it; durative states
      {"The cat is crying because it is hungry.", "ἡ γαλῆ κλαίει ὅτι πεινῇ."},
      {"The horse was so tired that it slept.", "ὁ ἵππος οὕτως ἔκαμνεν ὥστε ἐκάθευδεν."},
      {"The farmer's horse was so hungry that it ate the roses.", "ὁ ἵππος τοῦ γεωργοῦ οὕτως ἐπείνη ὥστε τὰ ῥόδα ἔφαγεν."},
      // teacher glosses added (cry -> κλαίω, yard -> αὐλή)
      {"Why is the baby crying?", "διὰ τί τὸ βρέφος κλαίει;"},
      {"The children played in the yard.", "οἱ παῖδες ἐν τῇ αὐλῇ ἔπαισαν."},
  };
  const int okEn = runCases(*e, en, false, "C18 constructions EN");
  CHECK(okEn == (int)en.size());
  if (real3().esOk) {
    const std::vector<Case3> es = {
        {"Cometí un error.", "ἥμαρτον."},
        {"Demos un paseo.", "περιπατῶμεν."},
        {"Tomemos un descanso.", "ἀναπαυώμεθα."},
        {"El niño dio un grito.", "ὁ παῖς ἐβόησεν."},
        {"El médico hizo una visita al rey.", "ὁ ἰατρὸς τὸν βασιλέα ἐπεσκέψατο."},
        // possessive dative with a body part
        {"El perro le mordió la mano.", "ὁ κύων τὴν χεῖρα αὐτοῦ ἔδακεν."},
        {"La madre le besó la cara.", "ἡ μήτηρ τὸ πρόσωπον αὐτοῦ ἐφίλησεν."},
        {"¡Que le corten la mano!", "ἀποτέμετε τὴν χεῖρα αὐτοῦ!"},
        // common gender by the Spanish noun; a gerund phrase as a participle
        {"La niña es pequeña.", "ἡ παῖς μικρά ἐστιν."},
        {"Veo a la niña.", "τὴν παῖδα ὁρῶ."},
        {"Viendo al lobo, el pastor huyó.", "ὁ ποιμὴν τὸν λύκον ὁρῶν ἔφυγεν."},
        {"Cuando la niña vio al gato, se rió.", "ἡ παῖς τὴν γαλῆν ἰδοῦσα ἐγέλασεν."},
    };
    const int okEs = runCases(*e, es, true, "C18 constructions ES");
    CHECK(okEs == (int)es.size());
  }
  // realia (decision 1): the hypernym, flag realia, never OK; the A9 round trip is reported
  for (const char* src : {"The tea is hot.", "We ate pizza."}) {
    rules::CueInput x;
    x.sourceText = src;
    auto r = e->translate({x}, opts3(false), rules::Context{}, nullptr, nullptr);
    REQUIRE(r.ok());
    const auto& o = r.value()[0];
    CHECK(std::find(o.flags.begin(), o.flags.end(), "realia") != o.flags.end());
    CHECK(o.confidence == rules::Confidence::Check);
    bool a9 = false;
    for (const auto& k : o.checks) a9 = a9 || (k.id == "A9" && k.detail.find("overlap") != std::string::npos);
    CHECK(a9);
  }
  // a light verb handled by its row is not a guess (no light-verb flag); an unhandled one stays Check
  {
    rules::CueInput x;
    x.sourceText = "I made a mistake.";
    auto r = e->translate({x}, opts3(false), rules::Context{}, nullptr, nullptr);
    REQUIRE(r.ok());
    CHECK(std::find(r.value()[0].flags.begin(), r.value()[0].flags.end(), "light-verb") == r.value()[0].flags.end());
    x.sourceText = "The children made a noise.";
    auto r2 = e->translate({x}, opts3(false), rules::Context{}, nullptr, nullptr);
    REQUIRE(r2.ok());
    CHECK(r2.value()[0].confidence == rules::Confidence::Check);
  }
}

TEST_CASE("rules-grc3: participle forms, προσ- augment, οὕτω / οὕτως") {
  NEED_REAL3();
  const lex::Lexicon& lx = real3().grc;
  auto P = [&](const char* verb, uint8_t tense, uint8_t number, uint8_t gender) {
    std::string out;
    const uint32_t id = grc::findLemma(lx, verb, feat::Verb);
    REQUIRE(id != lex::kNoLemma);
    if (!grc::participle(lx, id, tense, 0, feat::Nom, number, gender, out)) return std::string("-");
    return out;
  };
  using namespace vp::feat;
  CHECK(P("τρέχω", Present, Sg, M) == text::nfc("τρέχων"));
  CHECK(P("τρέχω", Present, Pl, M) == text::nfc("τρέχοντες"));
  CHECK(P("τρέχω", Present, Pl, F) == text::nfc("τρέχουσαι"));
  CHECK(P("τρέχω", Present, Pl, N) == text::nfc("τρέχοντα"));
  CHECK(P("ὁράω", Aorist, Sg, F) == text::nfc("ἰδοῦσα"));
  CHECK(P("ὁράω", Aorist, Pl, M) == text::nfc("ἰδόντες"));
  CHECK(P("ποιέω", Present, Pl, M) == text::nfc("ποιοῦντες"));
  CHECK(P("λύω", Aorist, Pl, M) == text::nfc("λύσαντες"));
  std::string out;
  CHECK_FALSE(grc::participle(lx, grc::findLemma(lx, "τρέχω", Verb), Present, 0, Gen, Sg, M, out));
  // προσκυνέω: the table's ἐπροσκύνησα is written προσεκύνησα, and the analysis reads it back
  const uint32_t pk = grc::findLemma(lx, "προσκυνέω", Verb);
  REQUIRE(pk != lex::kNoLemma);
  REQUIRE(grc::generate(lx, pk, grc::verbForm(P1, Sg, Aorist), out));
  CHECK(out == text::nfc("προσεκύνησα"));
  morph::Token t;
  grc::analyse(lx, text::nfc("προσεκύνησεν"), t);
  bool found = false;
  for (const auto& a : t.analyses) found = found || a.lemma == pk;
  CHECK(found);
  CHECK(grc::accentuate(text::nfc("οὕτως ψυχρόν")) == text::nfc("οὕτω ψυχρόν"));
  CHECK(grc::accentuate(text::nfc("οὕτω ἔχει")) == text::nfc("οὕτως ἔχει"));
}

// C18 (b): noun sense consistency inside a batch (transfer::Memory::nounSense, Greek side): the earlier Greek word is
// kept when it is one of the candidates; a remembered word that is no candidate is ignored.
TEST_CASE("rules-grc3: noun sense consistency in a batch") {
  NEED_REAL3();
  static curated::CuratedData cg = [] {
    auto r = curated::CuratedData::load(repo3() / "data" / "curated");
    REQUIRE(r.ok());
    return std::move(r.value());
  }();
  auto gd = grc::GreekData::load(repo3() / "data" / "curated");
  auto gt = grc::GreekTables::load(repo3() / "data" / "curated");
  REQUIRE(gd.ok());
  REQUIRE(gt.ok());
  grc::GreekTransfer xf(real3().grc, cg, gd.value(), gt.value());
  transfer::Settings st;
  frame::SemSentence s;
  frame::SemNP n;
  n.head = "boat";
  n.token = 0;
  transfer::Choice ch;
  const uint32_t first = xf.select("boat", feat::Noun, {}, false, false, st, ch);
  REQUIRE(first != lex::kNoLemma);
  REQUIRE(ch.candidates.size() > 1);
  const uint32_t other = ch.candidates[1].lemma;
  transfer::Memory mem;
  grc::GrcClauseOut out;
  CHECK(xf.np(n, s, st, mem, out).head == first);           // nothing remembered: the best candidate
  mem.nounSense.clear();
  mem.nounSense.emplace_back("boat", other);
  CHECK(xf.np(n, s, st, mem, out).head == other);           // the earlier choice wins
  mem.nounSense.clear();
  mem.nounSense.emplace_back("boat", grc::findLemma(real3().grc, "λίθος", feat::Noun));
  CHECK(xf.np(n, s, st, mem, out).head == first);           // not a candidate: ignored, and replaced
  REQUIRE(mem.nounSense.size() == 1);
  CHECK(mem.nounSense[0].second == first);
  n.head = "ship";
  for (int i = 0; i < 40; ++i) {   // the memory is bounded (32 entries)
    n.head = "ship" + std::to_string(i % 2 ? 0 : 1);
    xf.np(n, s, st, mem, out);
  }
  CHECK(mem.nounSense.size() <= 32);
}

// C18 review: four cases of the main agent's spot check plus two of our own per rule. Structures the shared frame
// builder gets wrong are rebuilt on the Greek side and are never OK; the text must be right whether or not the frame
// builder is fixed.
TEST_CASE("rules-grc3: C18 review (fronted when, -ing between commas, lost possessives, intransitive leave)") {
  NEED_REAL3();
  auto e = engine3();
  struct R { const char* src; const char* expected; bool mustCheck; };
  const std::vector<R> cases = {
      // "-ing" phrase between commas after the subject: a participle with that subject (rebuilt: Check)
      {"The shepherd, seeing the wolf, fled.", "ὁ ποιμὴν τὸν λύκον ὁρῶν ἔφυγεν. | ὁ ποιμὴν τὸν λύκον ἰδὼν ἔφυγεν.", true},
      {"The girl, hearing the bell, ran home.", "ἡ κόρη τὸν κώδωνα ἀκούουσα οἴκαδε ἔδραμεν.", true},
      {"The farmer, seeing the horse, laughed.", "ὁ γεωργὸς τὸν ἵππον ὁρῶν ἐγέλασεν.", true},
      // "When X, Y." is a statement with a time clause, never πότε
      {"When the sun rose, we went to the river.",
       "ἐπεὶ ὁ ἥλιος ἀνέτειλεν, εἰς τὸν ποταμὸν ἀπήλθομεν. | ἐπεὶ ὁ ἥλιος ἀνέτειλεν, εἰς τὸν ποταμὸν ἤλθομεν.", false},
      {"When the moon rose, the dogs barked.", "ἐπεὶ ἡ σελήνη ἀνέτειλεν, οἱ κύνες ὑλάκτουν.", false},
      {"When the king came, we bowed.", "ἐπεὶ ὁ βασιλεὺς ἧκεν, προσεκυνήσαμεν.", false},
      // possessives the frame builder lost; a verbless "where" fragment (rebuilt: Check)
      {"Where is your mother?", "ποῦ ἐστιν ἡ μήτηρ σου;", false},
      {"Where is my book?", "ποῦ ἐστι τὸ βιβλίον μου;", false},
      {"Where is his dog?", "ποῦ ἐστιν ὁ κύων αὐτοῦ;", false},
      {"Where is her cat?", "ποῦ ἐστιν ἡ γαλῆ αὐτῆς;", false},
      // intransitive "leave" = depart; a ship / boat sails away (kind subject)
      {"The guests are leaving.", "οἱ ξένοι ἀπέρχονται.", false},
      {"We left early.", "πρωῒ ἀπήλθομεν. | πρῲ ἀπήλθομεν.", false},
      {"The boat is leaving.", "ἡ ναῦς ἀποπλεῖ.", false},
  };
  int ok = 0;
  for (const R& k : cases) {
    rules::CueInput x;
    x.sourceText = k.src;
    auto r = e->translate({x}, opts3(false), rules::Context{}, nullptr, nullptr);
    REQUIRE(r.ok());
    const rules::CueOutput& o = r.value()[0];
    const std::string got = flat3(o.target);
    bool hit = false;
    std::string exp = k.expected;
    for (size_t a = 0;;) {
      const size_t b = exp.find(" | ", a);
      hit = hit || norm3(got) == norm3(text::nfc(exp.substr(a, b == std::string::npos ? std::string::npos : b - a)));
      if (b == std::string::npos) break;
      a = b + 3;
    }
    CHECK_MESSAGE(hit, k.src << " -> '" << got << "' expected '" << k.expected << "'");
    ok += hit;
    const bool rebuilt = std::find(o.flags.begin(), o.flags.end(), "clause-repair") != o.flags.end() ||
                         std::find(o.flags.begin(), o.flags.end(), "wh-statement") != o.flags.end() ||
                         std::find(o.flags.begin(), o.flags.end(), "participle-phrase") != o.flags.end();
    if (rebuilt || k.mustCheck) CHECK_MESSAGE(o.confidence != rules::Confidence::Ok, k.src << " was rebuilt but is OK");
    CHECK(o.confidence != rules::Confidence::Fix);
  }
  MESSAGE("C18 review cases: " << ok << " / " << cases.size());
  // "Hurry, the ship is leaving!": "Hurry" is read as a name by the frame builder (Check); the ship sails away
  {
    rules::CueInput x;
    x.sourceText = "Hurry, the ship is leaving!";
    auto r = e->translate({x}, opts3(false), rules::Context{}, nullptr, nullptr);
    REQUIRE(r.ok());
    CHECK(r.value()[0].target.find(text::nfc("ἀποπλεῖ")) != std::string::npos);
    CHECK(r.value()[0].confidence == rules::Confidence::Check);
  }
  // the transfer's own repair, independent of the frame builder: a statement "When the sun rose, we went to the
  // river." built as a wh question "when" with the main clause coordinated becomes a time clause + main clause, flagged
  {
    static curated::CuratedData cg = [] {
      auto r = curated::CuratedData::load(repo3() / "data" / "curated");
      REQUIRE(r.ok());
      return std::move(r.value());
    }();
    auto gd = grc::GreekData::load(repo3() / "data" / "curated");
    auto gt = grc::GreekTables::load(repo3() / "data" / "curated");
    REQUIRE(gd.ok());
    REQUIRE(gt.ok());
    grc::GreekTransfer xf(real3().grc, cg, gd.value(), gt.value());
    grc::GreekRealiser rl(real3().grc, cg, gd.value());
    frame::SemSentence s;
    s.finalPunct = ".";
    frame::SemFrame f;
    f.type = frame::Kind::Wh;
    f.wh.word = "when";
    f.wh.role = frame::Role::Adverb;
    f.hasPred = true;
    f.pred.lemma = "rise";
    f.pred.tense = frame::Tense::Past;
    f.hasSubject = true;
    f.subject.head = "sun";
    f.subject.definite = true;
    frame::SemFrame m;
    m.hasPred = true;
    m.pred.lemma = "go";
    m.pred.tense = frame::Tense::Past;
    m.hasSubject = true;
    m.subject.isPronoun = true;
    m.subject.pronLemma = "we";
    m.subject.pron.person = 1;
    m.subject.pron.number = 2;
    frame::SemOblique ob;
    ob.prep = "to";
    ob.np.head = "river";
    ob.np.definite = true;
    m.obliques.push_back(ob);
    frame::SemSub sb;
    sb.relation = frame::Relation::Coord;
    sb.frame.push_back(m);
    f.subordinate.push_back(sb);
    transfer::Settings st;
    transfer::Memory mem;
    grc::GrcClauseOut out;
    xf.clause(f, s, st, mem, out);
    CHECK(std::find(out.flags.begin(), out.flags.end(), "clause-repair") != out.flags.end());
    CHECK(out.clause.type == realise::ClauseType::Decl);
    grc::GrcOptions go;
    const std::string txt = rl.realise(out.clause, go).text;
    CHECK_MESSAGE(norm3(txt) == norm3(text::nfc("ἐπεὶ ὁ ἥλιος ἀνέτειλεν, εἰς τὸν ποταμὸν ἀπήλθομεν.")), txt);
  }
}
