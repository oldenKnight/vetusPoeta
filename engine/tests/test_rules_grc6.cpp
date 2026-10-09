// engine/rules Greek loop 6 (C29): irregular English pasts the tagger reads as a noun or a name (re-analysed through
// frame::en::verbOfForm, reused), a subject noun read as an adjective, city and country names (names_grc.tsv: Ἀθῆναι
// declined in the plural, εἰς / ἐκ / ἐν without the article, Μεξικόν as a neologism, never OK), "hope" -> ἐλπίζω with
// the subject of the infinitive kept (ἐλπίζω σε αὔριον ἥξειν), "money" -> ἀργύριον, "need" -> δέομαι + gen, Spanish and
// English addresses after or before a comma (ὦ παῖ, never an object), "ven" as the command of venir, "qué" + verb,
// "next to" / "beside" / "junto a" -> παρά + dat, the loop-5 frame-builder wishes repaired on the Greek side (generic
// "one", "rode home", Spanish verbs read as nouns), the fixes after the blind check, and the morphology / checker
// repairs (the doubled augment, the perfect of ἵστημι, the root aorist of ἀνίστημι, a noun after the article). Every
// rule is checked on our own sentences (two or more each, written before the rule: docs/rules_grc3_notes.md "Loop
// 6"); none is a blind-check sentence. Needs the real data (VP_DATA_WORK); skips otherwise.
#include <doctest.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "vp/engine_config.h"
#include "vp/lex.h"
#include "vp/morph_grc.h"
#include "vp/realise_grc.h"
#include "vp/rules.h"
#include "vp/text.h"

namespace stdfs = std::filesystem;
using namespace vp;

namespace {

stdfs::path repo6() { return stdfs::path(VP_FIXTURES_DIR).parent_path().parent_path(); }
stdfs::path work6() {
  const char* e = std::getenv("VP_DATA_WORK");
  return e && *e ? stdfs::path(e) : repo6() / "data" / "work";
}

struct Real6 {
  lex::Lexicon grc, en, es;
  bool ok = false, esOk = false;
  std::string why;
};
const Real6& real6() {
  static Real6 r = [] {
    Real6 x;
    const char* g = std::getenv("VP_GREEK_VPL");
    auto grc = lex::Lexicon::open(g && *g ? stdfs::path(g) : work6() / "greek.vpl");
    auto en = lex::Lexicon::open(work6() / "english.vpl");
    if (!grc.ok() || !en.ok() || !stdfs::exists(work6() / "nlp" / "english.tag.vpt")) {
      x.why = !grc.ok() ? grc.error().message : !en.ok() ? en.error().message : "english.tag.vpt missing";
      return x;
    }
    x.grc = std::move(grc.value());
    x.en = std::move(en.value());
    x.ok = true;
    auto es = lex::Lexicon::open(work6() / "spanish.vpl");
    if (es.ok() && stdfs::exists(work6() / "nlp" / "spanish.tag.vpt")) {
      x.es = std::move(es.value());
      x.esOk = true;
    }
    return x;
  }();
  return r;
}
#define NEED_REAL6()                                                                                 \
  if (!real6().ok) {                                                                                 \
    MESSAGE("real Greek data not available (" << real6().why << "); set VP_DATA_WORK. Skipped.");   \
    return;                                                                                          \
  }

std::unique_ptr<rules::Engine> engine6() {
  rules::EngineConfig cfg = rules::defaultEngineConfig();
  cfg.curatedDir = (repo6() / "data" / "curated").string();
  cfg.dataDir = work6().string();
  cfg.nlpDir = (work6() / "nlp").string();
  auto e = rules::makeEngine(cfg);
  e->setLexicons(nullptr, &real6().grc, &real6().en, real6().esOk ? &real6().es : nullptr);
  return e;
}

// As the regression: NFC, lower case, punctuation-insensitive, spaces collapsed; accents and breathings count.
std::string norm6(const std::string& s) {
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

rules::Options opts6(bool es) {
  rules::Options o;
  o.source = es ? rules::Lang::Es : rules::Lang::En;
  o.target = rules::Lang::Grc;
  o.fidelity = 2;
  o.speakerGender = 'f';
  return o;
}

bool hasFlag6(const rules::CueOutput& o, const char* f) {
  return std::find(o.flags.begin(), o.flags.end(), f) != o.flags.end();
}

// One cue at a time; `expected` may list alternatives after " | ". `mustCheck`: the cue must not be OK (a rebuilt
// structure, a repaired misreading, a calque, a rule form). Every rebuilt cue (clause-repair, participle-phrase,
// realia, past-form, from-rule, det-adverb, light-verb, subject-guess, speech-inversion) is never OK.
struct Case6 { const char* src; const char* expected; bool mustCheck; };
int run6(rules::Engine& e, const std::vector<Case6>& cases, bool es, const char* what) {
  int ok = 0;
  for (const Case6& k : cases) {
    rules::CueInput x;
    x.sourceText = k.src;
    auto r = e.translate({x}, opts6(es), rules::Context{}, nullptr, nullptr);
    REQUIRE(r.ok());
    const rules::CueOutput& o = r.value()[0];
    std::string got = o.target;
    std::replace(got.begin(), got.end(), '\n', ' ');
    bool hit = false;
    const std::string exp = k.expected;
    for (size_t a = 0;;) {
      const size_t b = exp.find(" | ", a);
      hit = hit || norm6(got) == norm6(text::nfc(exp.substr(a, b == std::string::npos ? std::string::npos : b - a)));
      if (b == std::string::npos) break;
      a = b + 3;
    }
    CHECK_MESSAGE(hit, what << ": " << k.src << " -> '" << got << "' expected '" << k.expected << "'");
    CHECK_MESSAGE(o.confidence != rules::Confidence::Fix, what << ": " << k.src << " is Fix");
    bool rebuilt = false;
    for (const char* f : {"clause-repair", "participle-phrase", "realia", "past-form", "from-rule", "det-adverb",
                          "light-verb", "subject-guess", "speech-inversion"})
      rebuilt = rebuilt || hasFlag6(o, f);
    if (rebuilt || k.mustCheck)
      CHECK_MESSAGE(o.confidence != rules::Confidence::Ok, what << ": " << k.src << " was rebuilt / guessed but is OK");
    ok += hit;
  }
  MESSAGE(std::string(what) << ": " << ok << " / " << cases.size());
  return ok;
}

}  // namespace


TEST_CASE("rules-grc6: C29 constructions (pasts, places, hope, money, addresses, next to, loop-5 wishes)") {
  NEED_REAL6();
  auto e = engine6();
  const std::vector<Case6> en = {
      // (1) irregular pasts read as a noun or a name; a subject noun read as an adjective
      {"Mary wept for her brother.", "ἡ μαρία τὸν ἀδελφὸν αὐτῆς ἔκλαυσεν.", true},
      {"The soldier wept because his horse died.", "ὁ στρατιώτης ἔκλαυσεν ὅτι ὁ ἵππος αὐτοῦ ἀπέθανεν.", true},
      {"The baby wept all night.", "τὸ βρέφος πᾶσαν τὴν νύκτα ἔκλαυσεν.", false},
      {"The children wept bitterly.", "οἱ παῖδες πικρῶς ἔκλαυσαν.", false},
      {"The boy slid on the ice.", "ὁ παῖς ἐπὶ τοῦ κρυστάλλου ὠλίσθησεν.", true},
      {"The soldier sang.", "ὁ στρατιώτης ᾖσεν.", true},
      {"The horse died.", "ὁ ἵππος ἀπέθανεν.", false},
      // (2) places: no article after the preposition, Ἀθῆναι plural, Μεξικόν a neologism (never OK)
      {"We live in Athens.", "ἐν ἀθήναις οἰκοῦμεν.", false},
      {"The ship sailed to Athens.", "ἡ ναῦς εἰς ἀθήνας ἔπλευσεν.", false},
      {"The merchant came from Athens.", "ὁ ἔμπορος ἐξ ἀθηνῶν ἦλθεν.", false},
      {"The king of Athens was wise.", "ὁ βασιλεὺς ἀθηνῶν σοφὸς ἦν.", false},
      {"The road to Athens is long.", "ἡ ὁδὸς εἰς ἀθήνας μακρά ἐστιν.", false},
      {"My uncle lives in Rome.", "ὁ θεῖός μου ἐν ῥώμῃ οἰκεῖ.", false},
      {"They went to Egypt.", "εἰς αἴγυπτον ἀπῆλθον.", false},
      {"Greece is beautiful.", "ἡ ἑλλὰς καλή ἐστιν.", false},
      {"I was born in Mexico.", "ἐν μεξικῷ ἐγενόμην.", true},
      {"The soldiers marched from Thebes to Corinth.", "οἱ στρατιῶται ἐκ θηβῶν εἰς κόρινθον ἐβάδισαν.", false},
      {"We will go to Italy tomorrow.", "αὔριον εἰς ἰταλίαν ἴμεν.", false},
      // (3) hope -> ἐλπίζω, the subject of the infinitive kept; a missing future infinitive -> the aorist
      {"I hope that you will come tomorrow.", "ἐλπίζω σε αὔριον ἥξειν.", false},
      {"She hopes that her father will return.", "ἐλπίζει τὸν πατέρα αὐτῆς ἐπανελθεῖν.", false},
      {"They hope that the rain will stop.", "ἐλπίζουσι τὸν ὄμβρον παύσεσθαι.", false},
      // (4) money -> ἀργύριον; need -> δέομαι + gen ("Δέω σε" for "I need you" was OK and wrong)
      {"I have no money.", "οὐδὲν ἀργύριον ἔχω.", false},
      {"The farmer needs money.", "ὁ γεωργὸς ἀργυρίου δεῖται.", false},
      {"We need water.", "ὕδατος δεόμεθα.", false},
      {"I need you.", "δέομαί σου.", false},
      {"The boy spent his money.", "ὁ παῖς τὸ ἀργύριον αὐτοῦ ἀνήλωσεν.", false},
      // (5) addresses
      {"Where are you, my son?", "ποῦ εἶ, ὦ υἱέ;", true},
      {"Why are you crying, girl?", "διὰ τί κλαίεις, ὦ κόρη;", false},
      {"Come here, Mary.", "ἐλθὲ δεῦρο, ὦ μαρία.", false},
      // (6) next to / beside -> παρά + dat
      {"The dog sat next to the boy.", "ὁ κύων παρὰ τῷ παιδὶ ἐκάθητο.", false},
      {"The house is next to the river.", "ἡ οἰκία παρὰ τῷ ποταμῷ ἐστιν.", false},
      {"She sat next to me.", "παρὰ ἐμοὶ ἐκάθητο.", false},
      {"The girl stood beside her mother.", "ἡ κόρη παρὰ τῇ μητρὶ αὐτῆς εἱστήκει.", false},
      // (7) loop-5 wishes repaired on the Greek side
      {"When one is tired, one sleeps.", "ὅτε τις κάμνει, καθεύδει τις.", true},
      {"We rode home on a donkey.", "ἐπὶ ὄνου οἴκαδε ἠλάσαμεν.", false},
      {"The children ran home.", "οἱ παῖδες οἴκαδε ἔδραμον.", false},
      // found while probing: shine, stand (the perfect system), stand up / get up (the root aorist), born
      {"The moon shone.", "ἡ σελήνη ἔλαμψεν.", false},
      {"The boy stands near the door.", "ὁ παῖς ἐγγὺς τῆς θύρας ἕστηκεν.", false},
      {"Stand up!", "ἀνάστηθι!", false},
      {"The boy stood up.", "ὁ παῖς ἀνέστη.", false},
      {"Get up, children!", "ἀνάστητε, ὦ παῖδες!", false},
      {"She dreamt of the sea.", "περὶ τῆς θαλάττης ἠνυπνίασεν.", false},
  };
  const int okEn = run6(*e, en, false, "C29 constructions EN");
  CHECK(okEn == (int)en.size());
  if (!real6().esOk) return;
  const std::vector<Case6> es = {
      {"Vivo en Atenas.", "ἐν ἀθήναις οἰκῶ.", false},
      {"Fuimos a Roma.", "εἰς ῥώμην ἀπήλθομεν.", false},
      {"Mi padre viene de México.", "ὁ πατήρ μου ἐκ μεξικοῦ ἔρχεται.", true},
      {"El barco navegó a Corinto.", "τὸ πλοῖον εἰς κόρινθον ἔπλευσεν.", false},
      {"Nací en México.", "ἐν μεξικῷ ἐγενόμην.", true},
      {"Espero que vengas mañana.", "ἐλπίζω σε αὔριον ἥξειν.", false},
      {"Esperamos ganar.", "ἐλπίζομεν νικήσειν.", false},
      {"No tengo dinero.", "ἀργύριον οὐκ ἔχω.", false},
      {"El mercader tiene mucho dinero.", "ὁ πώλης πολὺ ἀργύριον ἔχει.", false},
      {"Necesito agua.", "ὕδατος δέομαι.", false},
      {"¿Por qué lloras, niña?", "διὰ τί κλαίεις, ὦ παῖ;", false},   // C35: relaxed (trailing address read by the frame since C34)
      {"¿Dónde estás, hijo?", "ποῦ εἶ, ὦ υἱέ;", false},   // C35: relaxed (trailing address read by the frame since C34)
      {"Ven aquí, niño.", "ἐλθὲ δεῦρο, ὦ παῖ.", false},   // C35: relaxed ("Ven" read by the frame since C34)
      {"Ven aquí.", "ἐλθὲ δεῦρο.", false},   // C35: relaxed ("Ven" read by the frame since C34)
      {"Niña, ¿por qué lloras?", "ὦ παῖ, διὰ τί κλαίεις;", false},   // C35: relaxed (leading vocative segment of the frame since C34)
      {"Hijo, ven aquí.", "ὦ υἱέ, ἐλθὲ δεῦρο.", true},
      {"¿Qué haces, mamá?", "τί ποιεῖς, ὦ μάμμα; | τί ποιεῖς, ὦ μῆτερ;", true},   // C35: alternative added (mamá -> μήτηρ)
      {"¿Qué comes?", "τί ἐσθίεις;", true},
      {"El perro duerme junto al fuego.", "ὁ κύων παρὰ τῷ πυρὶ καθεύδει.", false},
      {"La niña se sentó junto a su madre.", "ἡ παῖς παρὰ τῇ μητρὶ αὐτῆς ἐκάθητο. | ἡ παῖς παρὰ τῇ μητρὶ ἐκάθητο. | ἡ παῖς παρὰ τῇ μητρὶ ἐκάθισεν. | ἡ παῖς παρὰ τῇ μητρὶ αὐτῆς ἐκάθισεν.", false},   // C35: alternative added ("su" -> the article)
      {"El búho caza de noche.", "ἡ γλαῦξ νύκτωρ διώκει.", true},
      {"El perro ladra.", "ὁ κύων ὑλακτεῖ.", true},
      {"La reina lloró por su hijo.", "ἡ βασίλεια τὸν υἱὸν αὐτῆς ἔκλαυσεν. | ἡ βασίλεια τὸν υἱὸν ἔκλαυσεν.", false},   // C35: alternative added ("su" -> the article)
      {"El niño gastó su dinero.", "ὁ παῖς τὸ ἀργύριον αὐτοῦ ἀνήλωσεν. | ὁ παῖς τὸ ἀργύριον ἀνήλωσεν.", false},   // C35: alternative added ("su" -> the article)
  };
  const int okEs = run6(*e, es, true, "C29 constructions ES");
  CHECK(okEs == (int)es.size());
}

// After the blind run: two or more own sentences for each fix (written before the fix); none is a blind sentence.
TEST_CASE("rules-grc6: C29 after the blind check (address interjections, a lot of, children, rain, cart, throw at)") {
  NEED_REAL6();
  auto e = engine6();
  const std::vector<Case6> en = {
      {"Mother, where is my hat?", "ὦ μῆτερ, ποῦ ἐστιν ὁ πέτασός μου;", true},
      {"Grandmother, where are you?", "ὦ τήθη, ποῦ εἶ;", true},
      {"We ate a lot of bread.", "πολὺν ἄρτον ἐφάγομεν.", false},
      {"She has lots of friends.", "πολλοὺς φίλους ἔχει.", false},
      {"Children, be quiet!", "ὦ παῖδες, σιγᾶτε!", true},   // C31: the plural command (σίγα was singular)
      {"Listen to me, children.", "ἀκούετέ μου, ὦ παῖδες.", false},
      {"The rain started.", "ὁ ὄμβρος ἤρξατο.", false},
      {"The rain is starting again.", "αὖθις ὁ ὄμβρος ἄρχεται.", false},
      {"The farmer has a cart.", "ὁ γεωργὸς ἅμαξαν ἔχει.", false},
      {"The cart is full of apples.", "ἡ ἅμαξα μήλων πλήρης ἐστίν.", false},
      {"Don't throw sand at your sister!", "μὴ βάλλε ἄμμον εἰς τὴν ἀδελφήν σου!", false},
      {"The dog dug a hole.", "ὁ κύων βόθρον ὤρυξεν.", false},
      {"The boys dug a deep pit near the tree.", "οἱ παῖδες βαθὺν βόθρον ἐγγὺς τοῦ δένδρου ὤρυξαν.", false},
      {"The village is quiet.", "ἡ κώμη ἥσυχός ἐστιν.", false},
      {"We walked through a quiet village.", "διὰ ἡσύχου κώμης ἐβαδίσαμεν.", false},
      {"The chickens are in the yard.", "οἱ ὄρνιθες ἐν τῇ αὐλῇ εἰσιν.", false},
      {"I gave bread to the chicken.", "τῷ ὄρνιθι ἄρτον ἔδωκα.", false},
      {"My sister has a new doll.", "ἡ ἀδελφή μου νέον παίγνιον ἔχει.", true},
      {"The doll is on the bed.", "τὸ παίγνιον ἐπὶ τῆς κοίτης ἐστίν.", true},
  };
  const int okEn = run6(*e, en, false, "C29 after blind EN");
  CHECK(okEn == (int)en.size());
}

TEST_CASE("rules-grc6: place names decline, GRC -> EN / ES names, the doubled augment, ἵστημι / ἀνίστημι, the article") {
  NEED_REAL6();
  auto gd = grc::GreekData::load(repo6() / "data" / "curated");
  REQUIRE(gd.ok());
  struct N { const char* en; uint8_t case_; const char* form; };
  for (const N& n : {N{"Athens", feat::Nom, "Ἀθῆναι"}, N{"Athens", feat::Gen, "Ἀθηνῶν"}, N{"Athens", feat::Dat, "Ἀθήναις"},
                     N{"Athens", feat::Acc, "Ἀθήνας"}, N{"Thebes", feat::Dat, "Θήβαις"}, N{"Rome", feat::Acc, "Ῥώμην"},
                     N{"Corinth", feat::Dat, "Κορίνθῳ"}, N{"Greece", feat::Acc, "Ἑλλάδα"}, N{"Mexico", feat::Dat, "Μεξικῷ"},
                     N{"Atenas", feat::Acc, "Ἀθήνας"}, N{"México", feat::Gen, "Μεξικοῦ"}}) {
    const grc::NameEntry* e = gd.value().nameByEnglish(n.en);
    REQUIRE_MESSAGE(e != nullptr, n.en);
    std::string out;
    CHECK_MESSAGE(grc::declineName(*e, n.case_, out), n.en);
    CHECK_MESSAGE(out == text::nfc(n.form), n.en << ": " << out << " expected " << n.form);
  }
  const grc::NameEntry* ath = gd.value().nameByEnglish("Athens");
  REQUIRE(ath != nullptr);
  CHECK(ath->place);
  CHECK(ath->number == feat::Pl);
  CHECK(ath->spanish == "Atenas");
  const grc::NameEntry* mex = gd.value().nameByEnglish("Mexico");
  REQUIRE(mex != nullptr);
  CHECK(mex->neologism);
  // GRC -> EN / ES: the place names read back (Spanish spelling for Spanish)
  auto e = engine6();
  struct G { const char* grc; const char* en; const char* es; };
  for (const G& g : {G{"ἐν Ἀθήναις οἰκοῦμεν.", "athens", "atenas"}, G{"εἰς Ῥώμην ἀπήλθομεν.", "rome", "roma"},
                     G{"ὁ ἔμπορος ἐξ Αἰγύπτου ἦλθεν.", "egypt", "egipto"}}) {
    for (int es = 0; es < 2; ++es) {
      rules::Options o;
      o.source = rules::Lang::Grc;
      o.target = es ? rules::Lang::Es : rules::Lang::En;
      o.fidelity = 2;
      rules::CueInput x;
      x.sourceText = text::nfc(g.grc);
      auto r = e->translate({x}, o, rules::Context{}, nullptr, nullptr);
      REQUIRE(r.ok());
      const std::string t = text::lower(r.value()[0].target);
      CHECK_MESSAGE(t.find(es ? g.es : g.en) != std::string::npos, g.grc << " -> '" << r.value()[0].target << "'");
    }
  }
  const lex::Lexicon& lx = real6().grc;
  auto gen = [&](const char* verb, uint8_t tense, uint8_t mood, uint8_t person, uint8_t number) {
    const uint32_t v = grc::findLemma(lx, verb, feat::Verb);
    REQUIRE(v != lex::kNoLemma);
    feat::Features f;
    f.pos = feat::Verb;
    f.mood = mood;
    f.tense = tense;
    f.person = person;
    f.number = number;
    std::string out;
    CHECK(grc::generate(lx, v, f, out));
    return out;
  };
  // ἐνυπνιάζω: the table writes ἐἐνυπνίασε; generate() writes ἠνυπνίασε and reads it back
  const std::string dream = gen("ἐνυπνιάζω", feat::Aorist, feat::Indicative, feat::P3, feat::Sg);
  CHECK(text::greek_bare(dream).compare(0, text::greek_bare("ἠνυπνιασε").size(), text::greek_bare("ἠνυπνιασε")) == 0);
  morph::Token t;
  grc::analyse(lx, dream, t);
  bool back = false;
  for (const lex::Analysis& a : t.analyses) back = back || a.lemma == grc::findLemma(lx, "ἐνυπνιάζω", feat::Verb);
  CHECK(back);
  // ἵστημι "stand": the Attic perfect (ἕστηκα, not the Doric ἕστακα); ἀνίστημι "stand up": the root aorist
  CHECK(text::greek_bare(gen("ἵστημι", feat::Perfect, feat::Indicative, feat::P3, feat::Sg)).rfind(text::greek_bare("ἑστηκε"), 0) == 0);
  CHECK(gen("ἀνίστημι", feat::Aorist, feat::Indicative, feat::P3, feat::Sg) == text::nfc("ἀνέστη"));
  CHECK(gen("ἀνίστημι", feat::Aorist, feat::Imperative, feat::P2, feat::Sg) == text::nfc("ἀνάστηθι"));
  // the checker: a word right after the article is a noun ("ἐν τῇ αὐλῇ" is not a form of αὐλέω)
  rules::Options o = opts6(false);
  rules::CueInput x;
  x.sourceText = text::nfc("οἱ ὄρνιθες ἐν τῇ αὐλῇ εἰσιν.");
  auto cr = e->check(x, x.sourceText, o, rules::Context{});
  REQUIRE(cr.ok());
  for (const rules::Check& k : cr.value().checks)
    if (k.id == "A3") CHECK_MESSAGE(k.ok, "A3: " << k.detail);
}
