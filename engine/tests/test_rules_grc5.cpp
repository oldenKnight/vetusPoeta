// engine/rules Greek loop 5 (C25): the result clause after "such / so / tan ... that" read from the source tokens
// (ὥστε, never ὅτι), English irregular pasts the tagger misreads (frame::en::verbOfForm: "bit", "blew", "swung", "lay"),
// Spanish "anciano / anciana" -> γέρων / γραῦς, "have a good time" (kind light, frame manner: ἡδέως διάγω; a calque is
// never OK), "ride" -> ἐλαύνω / ἱππεύω, generic "one" -> τις (enclitic, never first), quoted sound words kept as written,
// and the fixes after the blind check (question repair from the auxiliary, "my friends and I", time adverbs,
// "across", washing hands, attributive participles, bare kinship objects, oblique participle forms, the smooth
// augment). Every rule is checked on our own sentences (two or more each, written before the rule:
// docs/rules_grc3_notes.md "Loop 5"); none is a blind-check sentence. Needs the real data (VP_DATA_WORK); skips
// otherwise.
#include <doctest.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "vp/engine_config.h"
#include "vp/lex.h"
#include "vp/morph_grc.h"
#include "vp/rules.h"
#include "vp/text.h"

namespace stdfs = std::filesystem;
using namespace vp;

namespace {

stdfs::path repo5() { return stdfs::path(VP_FIXTURES_DIR).parent_path().parent_path(); }
stdfs::path work5() {
  const char* e = std::getenv("VP_DATA_WORK");
  return e && *e ? stdfs::path(e) : repo5() / "data" / "work";
}

struct Real5 {
  lex::Lexicon grc, en, es;
  bool ok = false, esOk = false;
  std::string why;
};
const Real5& real5() {
  static Real5 r = [] {
    Real5 x;
    const char* g = std::getenv("VP_GREEK_VPL");
    auto grc = lex::Lexicon::open(g && *g ? stdfs::path(g) : work5() / "greek.vpl");
    auto en = lex::Lexicon::open(work5() / "english.vpl");
    if (!grc.ok() || !en.ok() || !stdfs::exists(work5() / "nlp" / "english.tag.vpt")) {
      x.why = !grc.ok() ? grc.error().message : !en.ok() ? en.error().message : "english.tag.vpt missing";
      return x;
    }
    x.grc = std::move(grc.value());
    x.en = std::move(en.value());
    x.ok = true;
    auto es = lex::Lexicon::open(work5() / "spanish.vpl");
    if (es.ok() && stdfs::exists(work5() / "nlp" / "spanish.tag.vpt")) {
      x.es = std::move(es.value());
      x.esOk = true;
    }
    return x;
  }();
  return r;
}
#define NEED_REAL5()                                                                                 \
  if (!real5().ok) {                                                                                 \
    MESSAGE("real Greek data not available (" << real5().why << "); set VP_DATA_WORK. Skipped.");   \
    return;                                                                                          \
  }

std::unique_ptr<rules::Engine> engine5() {
  rules::EngineConfig cfg = rules::defaultEngineConfig();
  cfg.curatedDir = (repo5() / "data" / "curated").string();
  cfg.dataDir = work5().string();
  cfg.nlpDir = (work5() / "nlp").string();
  auto e = rules::makeEngine(cfg);
  e->setLexicons(nullptr, &real5().grc, &real5().en, real5().esOk ? &real5().es : nullptr);
  return e;
}

// As the regression: NFC, lower case, punctuation-insensitive, spaces collapsed; accents and breathings count.
std::string norm5(const std::string& s) {
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

rules::Options opts5(bool es) {
  rules::Options o;
  o.source = es ? rules::Lang::Es : rules::Lang::En;
  o.target = rules::Lang::Grc;
  o.fidelity = 2;
  o.speakerGender = 'f';
  return o;
}

bool hasFlag5(const rules::CueOutput& o, const char* f) {
  return std::find(o.flags.begin(), o.flags.end(), f) != o.flags.end();
}

// One cue at a time; `expected` may list alternatives after " | ". `mustCheck`: the cue must not be OK (a rebuilt
// structure, a repaired misreading, a calque, a rule form). Every rebuilt cue (clause-repair, participle-phrase,
// realia, past-form, from-rule, det-adverb, light-verb, subject-guess, speech-inversion) is never OK.
struct Case5 { const char* src; const char* expected; bool mustCheck; };
int run5(rules::Engine& e, const std::vector<Case5>& cases, bool es, const char* what) {
  int ok = 0;
  for (const Case5& k : cases) {
    rules::CueInput x;
    x.sourceText = k.src;
    auto r = e.translate({x}, opts5(es), rules::Context{}, nullptr, nullptr);
    REQUIRE(r.ok());
    const rules::CueOutput& o = r.value()[0];
    std::string got = o.target;
    std::replace(got.begin(), got.end(), '\n', ' ');
    bool hit = false;
    const std::string exp = k.expected;
    for (size_t a = 0;;) {
      const size_t b = exp.find(" | ", a);
      hit = hit || norm5(got) == norm5(text::nfc(exp.substr(a, b == std::string::npos ? std::string::npos : b - a)));
      if (b == std::string::npos) break;
      a = b + 3;
    }
    CHECK_MESSAGE(hit, what << ": " << k.src << " -> '" << got << "' expected '" << k.expected << "'");
    CHECK_MESSAGE(o.confidence != rules::Confidence::Fix, what << ": " << k.src << " is Fix");
    bool rebuilt = false;
    for (const char* f : {"clause-repair", "participle-phrase", "realia", "past-form", "from-rule", "det-adverb",
                          "light-verb", "subject-guess", "speech-inversion"})
      rebuilt = rebuilt || hasFlag5(o, f);
    if (rebuilt || k.mustCheck)
      CHECK_MESSAGE(o.confidence != rules::Confidence::Ok, what << ": " << k.src << " was rebuilt / guessed but is OK");
    ok += hit;
  }
  MESSAGE(std::string(what) << ": " << ok << " / " << cases.size());
  return ok;
}

}  // namespace

TEST_CASE("rules-grc5: C25 constructions (result clauses, irregular pasts, anciano, idioms, ride, generic one, quotes)") {
  NEED_REAL5();
  auto e = engine5();
  const std::vector<Case5> en = {
      // (1) such / so ... that: the degree word read from the tokens -> ὥστε ("such good friends that" was ὅτι, OK)
      {"It was such a cold night that the river froze.", "οὕτω ψυχρὰ νὺξ ἦν ὥστε ὁ ποταμὸς ἔπηξεν.", true},
      {"They were such good friends that they never fought.", "οὕτως ἀγαθοὶ φίλοι ἦσαν ὥστε οὐδέποτε ἐμαχέσαντο.", false},
      {"It was such a long road that we were tired.", "οὕτω μακρὰ ὁδὸς ἦν ὥστε ἐκάμνομεν.", true},
      {"He ran so fast that nobody caught him.", "οὕτω ταχέως ἔδραμεν ὥστε οὐδεὶς αὐτὸν κατέλαβεν.", false},
      // (2) irregular pasts the tagger misreads (frame::en::verbOfForm)
      {"The dog bit the boy.", "ὁ κύων τὸν παῖδα ἔδακεν.", true},
      {"The wind blew.", "ὁ ἄνεμος ἔπνευσεν.", true},
      {"The dog lay there all day.", "ὁ κύων πᾶσαν τὴν ἡμέραν ἐκεῖ ἔκειτο.", true},
      {"The children swung on the gate.", "οἱ παῖδες ἐπὶ τῆς πύλης κραδῶσιν.", true},   // historic present: no past cells
      {"The girl wept all night.", "ἡ κόρη πᾶσαν τὴν νύκτα ἔκλαυσεν.", false},
      // (4) have a good time (light, frame manner); a calque without a manner row is never OK
      {"We had such a good time.", "οὕτως ἡδέως διηγάγομεν.", false},
      {"We had a good time.", "ἡδέως διηγάγομεν.", false},
      {"We had a bad time.", "κακῶς διηγάγομεν.", false},
      {"The children had a wonderful time at the party.", "οἱ παῖδες ἐν τῇ ἑορτῇ πάνυ ἡδέως διήγαγον.", false},
      {"We have time.", "χρόνον ἔχομεν.", false},
      {"Have a good time!", "δίαγε ἡδέως!", false},
      {"We had an exciting time.", "ἀπαθῆ χρόνον εἴχομεν.", true},
      // (5) ride: ἐλαύνω / ἱππεύω, never a verb of carrying
      {"The king rode to the city.", "ὁ βασιλεὺς εἰς τὴν πόλιν ἤλασεν.", false},
      {"Can you ride a horse?", "ἆρα δύνασαι ἱππεύειν;", false},
      {"We rode on a donkey.", "ἐπὶ ὄνου ἠλάσαμεν.", false},
      {"She rides a horse every day.", "ἱππεύει ἑκάστης ἡμέρας.", false},
      // (6) generic one -> τις (enclitic, never first); with must / should the impersonal verb alone
      {"One must not lie.", "οὐ δεῖ ψεύδεσθαι.", false},
      {"One never knows.", "οὐδέποτέ τις οἶδεν.", false},
      {"One can see the sea from the hill.", "δύναταί τις τὴν θάλατταν ἐκ τοῦ ὄρους ἰδεῖν.", false},
      {"If one is hungry, one must eat.", "εἴ τις πεινῇ, δεῖ φαγεῖν.", false},
      {"Someone ate my bread.", "τὸν ἄρτον μου ἔφαγέ τις.", false},
      {"If someone comes, call me.", "ἐάν τις ἥξῃ, κάλεσόν με.", false},
      {"I saw someone in the garden.", "ἐν τῷ κήπῳ εἶδόν τινα.", false},
      {"I want the red one.", "τὸ ἐρυθρὸν βούλομαι.", false},
      // (8) quoted sound words kept as written; a quoted word that is not rendered is never OK
      {"The dog said \"woof\".", "ὁ κύων \"woof\" ἔλεξεν.", false},
      {"The cow says \"moo\".", "ὁ βοῦς \"moo\" λέγει.", false},
      {"\"Woof!\" said the dog.", "\"woof!\" ὁ κύων ἔλεξεν.", true},
      {"She wrote the word \"love\" on the door.", "τὸν λόγον ἐπὶ τῆς θύρας ἔγραψεν.", true},
      // found while probing: possessive pronouns as the predicate (were "[mine]", Fix)
      {"This book is mine.", "τοῦτο τὸ βιβλίον ἐμόν ἐστιν.", false},
      {"The red ball is yours.", "ἡ ἐρυθρὰ σφαῖρα σή ἐστιν.", false},
      {"The garden is theirs.", "ὁ κῆπος ἐκείνων ἐστίν.", false},
      {"The dog is his.", "ὁ κύων ἐκείνου ἐστίν.", false},
      // "lie" without a place is "tell lies" (κεῖμαι was OK and wrong); "lie to" -> ψεύδω + acc
      {"Do not lie!", "μὴ ψεύδου!", false},
      {"He lied to his mother.", "τὴν μητέρα αὐτοῦ ἔψευσεν.", false},
      {"The cat lies on the bed.", "ἡ γαλῆ ἐπὶ τῆς κοίτης κεῖται.", false},
      // know how to + present infinitive (the complement verb took the main verb's row and vanished)
      {"I know how to read.", "οἶδα ἀναγιγνώσκειν.", false},
      {"Do you know how to swim?", "ἆρα οἶσθα νεῖν;", false},
  };
  const int okEn = run5(*e, en, false, "C25 constructions EN");
  CHECK(okEn == (int)en.size());
  if (!real5().esOk) return;
  const std::vector<Case5> es = {
      {"Corrió tan rápido que nadie lo alcanzó.", "οὕτω ταχέως ἔδραμεν ὥστε οὐδεὶς αὐτὸν ἐξίκετο.", false},
      {"Tenía un perro tan grande que todos tenían miedo.", "οὕτω μέγαν κύνα εἶχεν ὥστε πάντες ἐφοβοῦντο.", true},
      // (3) anciano / anciana (ἀκτέα, the elder tree, came through the English pivot "elder")
      {"El anciano duerme.", "ὁ γέρων καθεύδει.", false},
      {"La anciana canta.", "ἡ γραῦς ᾄδει.", false},
      {"Los ancianos hablan.", "οἱ γέροντες λαλοῦσιν.", false},
      {"Un hombre viejo camina.", "γέρων βαδίζει.", false},
      {"La mujer anciana duerme.", "ἡ γραῦς καθεύδει.", false},
      {"Un anciano vive en la casa.", "γέρων ἐν τῷ οἴκῳ οἰκεῖ.", false},   // "en la casa" is not "en casa" (οἴκοι)
      {"Comemos en casa.", "οἴκοι ἐσθίομεν.", false},
      // (4) pasarlo bien, divertirse, que + subjunctive as a wish
      {"Lo pasamos muy bien.", "πάνυ ἡδέως διάγομεν.", false},
      {"Lo pasamos mal.", "κακῶς διάγομεν.", false},
      {"Los niños se divirtieron en la fiesta.", "οἱ παῖδες ἐν τῇ ἑορτῇ ηὐφράνθησαν.", false},
      {"¡Que te diviertas!", "εὐφράνθητι!", false},
      {"¡Que duermas bien!", "κάθευδε εὖ!", false},
      // (5) cabalgar, montar a caballo, montar en burro
      {"El rey cabalgó hasta la ciudad.", "ὁ βασιλεὺς μέχρι τῆς πόλεως ἤλασεν.", false},
      {"Montábamos a caballo.", "ἱππεύομεν.", false},
      {"Mi hermano monta en burro.", "ὁ ἀδελφός μου ἐπὶ ὄνου ἐλαύνει.", false},
      // (8) Spanish quoted sounds ("eso" read in their place)
      {"El perro dijo \"guau\".", "ὁ κύων \"guau\" ἔλεξεν.", false},
      {"La vaca dice \"mu\".", "ἡ βοῦς \"mu\" λέγει.", false},
      // saber + infinitive (the infinitive vanished, OK and wrong); querer que -> accusative + infinitive (ὅτι, OK)
      {"Sé leer.", "οἶδα ἀναγιγνώσκειν.", false},
      {"No sé cantar.", "οὐκ οἶδα ᾄδειν.", false},
      {"¿Sabes nadar?", "ἆρα οἶσθα νεῖν;", false},
      {"Quiero que vengas.", "βούλομαί σε ἐλθεῖν.", false},
      {"Queremos que el rey venga.", "βουλόμεθα τὸν βασιλέα ἐλθεῖν.", false},
      {"Mi madre quiere que duerma.", "ἡ μήτηρ μου βούλεται καθεύδειν.", true},
      // (7) frame misreadings repaired on the Greek side: "está en casa" read as a fragment
      {"Mi madre está en casa.", "ἡ μήτηρ μου οἴκοι ἐστίν.", true},
      {"Los niños están en casa.", "οἱ παῖδες οἴκοι εἰσίν.", true},
  };
  const int okEs = run5(*e, es, true, "C25 constructions ES");
  CHECK(okEs == (int)es.size());
}

// C25 after the blind check: each fix on two or more own sentences (none of them a blind sentence)
TEST_CASE("rules-grc5: C25 after the blind check (questions, address phrases, time adverbs, participles, objects)") {
  NEED_REAL5();
  auto e = engine5();
  const std::vector<Case5> en = {
      // a question the parser breaks, analysed from its auxiliary on
      {"Why do the dogs bark?", "διὰ τί οἱ κύνες ὑλακτοῦσιν;", true},
      {"Did the dog bark?", "ἆρα ὑλάκτει ὁ κύων;", true},
      {"How many horses does the king have?", "πόσους ἵππους ὁ βασιλεὺς ἔχει;", true},
      {"How did the sheep escape?", "πῶς τὰ πρόβατα ἔφυγεν;", true},
      // "my friend(s) and I": the address row before "and"
      {"My friends and I went to the sea.", "οἱ φίλοι μου καὶ ἐγὼ εἰς τὴν θάλατταν ἀπήλθομεν.", true},
      {"My friend and I played in the garden.", "ὁ φίλος μου καὶ ἐγὼ ἐν τῷ κήπῳ ἐπαίσαμεν.", true},
      {"My friend, come here.", "ὦ φίλε, ἐλθὲ δεῦρο.", false},
      // a when-clause read as a that-complement (ὅτι was produced): a time clause / participle
      {"The girl smiled when she saw her mother.", "ἡ κόρη τὴν μητέρα αὐτῆς ἰδοῦσα ἐμειδίασεν.", true},
      {"The boy laughed when he saw the cat.", "ὁ παῖς τὴν γαλῆν ἰδὼν ἐγέλασεν.", true},
      // times of day: ἕωθεν, νύκτωρ (the poetic ἠώς was chosen); across -> διά + gen
      {"We swam in the river this morning.", "ἐν τῷ ποταμῷ ἕωθεν ἐνεύσαμεν.", false},
      {"The owl hunts at night.", "ἡ γλαῦξ νύκτωρ διώκει.", true},
      {"The boys ran across the field.", "οἱ παῖδες διὰ τοῦ ἀγροῦ ἔδραμον.", false},
      {"We sailed across the river.", "διὰ τοῦ ποταμοῦ ἐπλεύσαμεν.", false},
      // washing hands / feet / face: νίζομαι (λοῦσον τὰς χεῖρας was OK and wrong); the smooth augment ἐνίψατο
      {"Wash your face!", "νίψαι τὸ πρόσωπόν σου!", false},
      {"The boy washed his feet in the river.", "ὁ παῖς τοὺς πόδας αὐτοῦ ἐν τῷ ποταμῷ ἐνίψατο.", false},
      // attributive participles of state verbs (kind ptc); oblique forms by rule are Check
      {"The hungry wolf came to the farm.", "ὁ πεινῶν λύκος εἰς τὸν ἀγρὸν ἦλθεν.", false},
      {"We gave water to the thirsty horses.", "τοῖς διψῶσιν ἵπποις ὕδωρ ἔδομεν.", false},
      {"The angry king shouted.", "ὁ ὀργιζόμενος βασιλεὺς ἐβόησεν.", false},
      {"She helped the frightened girl.", "τῇ φοβουμένῃ κόρῃ ἐβοήθησεν.", true},
      // a bare kinship noun read as a determiner / an adverb: the object
      {"We will visit grandmother tomorrow.", "αὔριον τὴν τήθην ἐπισκεψόμεθα.", true},
      {"I will see grandfather today.", "σήμερον τὸν πάππον ὄψομαι.", true},
      // a time adverb read as an "of" attribute; a plural head the frame left singular
      {"We will eat bread tomorrow.", "αὔριον ἄρτον ἐδόμεθα.", false},
      {"I bought apples today.", "σήμερον μῆλα ἠγόρασα.", false},
      // "of the well": the noun after "of the" read as an adverb (εὖ) -> genitive attribute (Check)
      {"The water of the well is cold.", "τὸ ὕδωρ τοῦ φρέατος ψυχρόν ἐστιν.", true},
      {"Bring me the water of the well.", "ἔνεγκόν μοι τὸ ὕδωρ τοῦ φρέατος.", true},
      // found while probing: shake with an object -> σείω (ἔτρεμεν was OK and wrong), without one τρέμω
      {"The boy shook the tree.", "ὁ παῖς τὸ δένδρον ἔσεισεν.", false},
      {"The dog shook its head.", "ὁ κύων τὴν κεφαλὴν αὐτοῦ ἔσεισεν.", true},
      {"The little girl was shaking.", "ἡ μικρὰ κόρη ἔτρεμεν.", false},
  };
  const int okEn = run5(*e, en, false, "C25 after blind EN");
  CHECK(okEn == (int)en.size());
  if (!real5().esOk) return;
  const std::vector<Case5> es = {
      {"Esta mañana comimos pan.", "ἕωθεν ἄρτον ἐφάγομεν.", false},
      {"Esta noche dormimos en casa.", "ταύτῃ τῇ νυκτὶ οἴκοι καθεύδομεν.", false},
      {"Lávate las manos.", "νίψαι τὰς χεῖρας.", false},
      {"El niño se lavó la cara.", "ὁ παῖς τὸ πρόσωπον ἐνίψατο.", false},
      {"El lobo hambriento vino.", "ὁ πεινῶν λύκος ἦλθεν.", false},
      {"Dimos agua a los caballos sedientos.", "τοῖς διψῶσιν ἵπποις ὕδωρ ἔδομεν.", false},
      // own spot check after the blind run: río -> ποταμός (στόμα "mouth" came through the pivot), "nada" as the
      // verb nadar in a sentence without a verb ("the boy of nothing in the river" was OK), "cerca de" -> ἐγγύς + gen,
      // a verbless subject + phrase is never OK
      {"Nos divertimos en el río.", "ἐν τῷ ποταμῷ εὐφραινόμεθα.", false},
      {"El niño nada en el río.", "ὁ παῖς ἐν τῷ ποταμῷ νεῖ.", true},
      {"Mi hermano nada muy bien.", "ὁ ἀδελφός μου πάνυ εὖ νεῖ.", true},
      {"Los niños jugaban cerca del río.", "οἱ παῖδες ἐγγὺς τοῦ ποταμοῦ ἔπαιζον.", false},
      {"La casa está cerca del mar.", "ὁ οἶκος ἐγγὺς τῆς θαλάττης ἐστίν.", false},
      {"La niña de la casa en el jardín.", "ἡ παῖς τοῦ οἴκου ἐν τῷ κήπῳ.", true},
  };
  const int okEs = run5(*e, es, true, "C25 after blind ES");
  CHECK(okEs == (int)es.size());
}

TEST_CASE("rules-grc5: oblique participles, the smooth augment, ἐπισκέπτομαι") {
  NEED_REAL5();
  const lex::Lexicon& lx = real5().grc;
  struct P { const char* verb; uint8_t tense, voice, case_, number, gender; const char* form; };
  for (const P& p : {P{"πεινάω", feat::Present, 0, feat::Dat, feat::Sg, feat::M, "πεινῶντι"},
                     P{"πεινάω", feat::Present, 0, feat::Gen, feat::Pl, feat::M, "πεινώντων"},
                     P{"κάμνω", feat::Present, 0, feat::Gen, feat::Sg, feat::F, "καμνούσης"},
                     P{"κάμνω", feat::Present, 0, feat::Acc, feat::Sg, feat::F, "κάμνουσαν"},
                     P{"κάμνω", feat::Present, 0, feat::Gen, feat::Pl, feat::M, "καμνόντων"},
                     P{"κάμνω", feat::Present, 0, feat::Gen, feat::Pl, feat::F, "καμνουσῶν"},
                     P{"κάμνω", feat::Present, 0, feat::Dat, feat::Pl, feat::M, "κάμνουσι"},
                     P{"κάμνω", feat::Present, 0, feat::Acc, feat::Pl, feat::M, "κάμνοντας"},
                     P{"ὁράω", feat::Aorist, 0, feat::Gen, feat::Sg, feat::M, "ἰδόντος"},
                     P{"λύω", feat::Present, feat::Middle, feat::Gen, feat::Sg, feat::M, "λυομένου"},
                     P{"λύω", feat::Present, feat::Middle, feat::Acc, feat::Sg, feat::F, "λυομένην"},
                     P{"λύω", feat::Present, feat::Middle, feat::Dat, feat::Pl, feat::M, "λυομένοις"},
                     P{"ὀργίζω", feat::Present, feat::Passive, feat::Nom, feat::Sg, feat::M, "ὀργιζόμενος"}}) {
    const uint32_t v = grc::findLemma(lx, p.verb, feat::Verb);
    REQUIRE(v != lex::kNoLemma);
    std::string out;
    CHECK_MESSAGE(grc::participle(lx, v, p.tense, p.voice, p.case_, p.number, p.gender, out), p.verb << " " << p.form);
    CHECK_MESSAGE(out == text::nfc(p.form), p.verb << ": " << out << " expected " << p.form);
  }
  // νίζω: the table writes the augment with a rough breathing (ἕνιψα); generate() writes ἔνιψα and reads it back
  const uint32_t nizo = grc::findLemma(lx, "νίζω", feat::Verb);
  REQUIRE(nizo != lex::kNoLemma);
  feat::Features f;
  f.pos = feat::Verb;
  f.mood = feat::Indicative;
  f.tense = feat::Aorist;
  f.person = feat::P1;
  f.number = feat::Sg;
  std::string out;
  CHECK(grc::generate(lx, nizo, f, out));
  CHECK(out == text::nfc("ἔνιψα"));
  morph::Token t;
  grc::analyse(lx, out, t);
  bool back = false;
  for (const lex::Analysis& a : t.analyses) back = back || a.lemma == nizo;
  CHECK(back);
  CHECK_FALSE(t.accentInsensitive);
}
