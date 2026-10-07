// engine/rules Greek loop 4 (C21): degree words ("such a big X" -> οὕτω μέγαν / τοιοῦτον, "so ... that" -> ὥστε, a
// dropped degree word is never OK), weather and nature verbs (lexical_en_grc.tsv kind subject with the voice frames
// mid / mid-pres, impersonal verb rows, Spanish mirrors), the negation right before its verb (neg.verb), the gloss
// gaps of loop 3 (tiers_grc.tsv teacher glosses, kind noun / realia for "nest"), the present participle of κάθημαι,
// "by the fire" -> παρά, an enclitic pronoun never first, "Hurry, ..." -> σπεῦδε. Every rule is checked on our own
// sentences (two or more each, written before the regression file was run: docs/rules_grc3_notes.md "Loop 4").
// Needs the real data (VP_DATA_WORK); skips otherwise.
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

stdfs::path repo4() { return stdfs::path(VP_FIXTURES_DIR).parent_path().parent_path(); }
stdfs::path work4() {
  const char* e = std::getenv("VP_DATA_WORK");
  return e && *e ? stdfs::path(e) : repo4() / "data" / "work";
}

struct Real4 {
  lex::Lexicon grc, en, es;
  bool ok = false, esOk = false;
  std::string why;
};
const Real4& real4() {
  static Real4 r = [] {
    Real4 x;
    const char* g = std::getenv("VP_GREEK_VPL");
    auto grc = lex::Lexicon::open(g && *g ? stdfs::path(g) : work4() / "greek.vpl");
    auto en = lex::Lexicon::open(work4() / "english.vpl");
    if (!grc.ok() || !en.ok() || !stdfs::exists(work4() / "nlp" / "english.tag.vpt")) {
      x.why = !grc.ok() ? grc.error().message : !en.ok() ? en.error().message : "english.tag.vpt missing";
      return x;
    }
    x.grc = std::move(grc.value());
    x.en = std::move(en.value());
    x.ok = true;
    auto es = lex::Lexicon::open(work4() / "spanish.vpl");
    if (es.ok() && stdfs::exists(work4() / "nlp" / "spanish.tag.vpt")) {
      x.es = std::move(es.value());
      x.esOk = true;
    }
    return x;
  }();
  return r;
}
#define NEED_REAL4()                                                                                 \
  if (!real4().ok) {                                                                                 \
    MESSAGE("real Greek data not available (" << real4().why << "); set VP_DATA_WORK. Skipped.");   \
    return;                                                                                          \
  }

std::unique_ptr<rules::Engine> engine4() {
  rules::EngineConfig cfg = rules::defaultEngineConfig();
  cfg.curatedDir = (repo4() / "data" / "curated").string();
  cfg.dataDir = work4().string();
  cfg.nlpDir = (work4() / "nlp").string();
  auto e = rules::makeEngine(cfg);
  e->setLexicons(nullptr, &real4().grc, &real4().en, real4().esOk ? &real4().es : nullptr);
  return e;
}

// As the regression: NFC, lower case, punctuation-insensitive, spaces collapsed; accents and breathings count.
std::string norm4(const std::string& s) {
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

rules::Options opts4(bool es) {
  rules::Options o;
  o.source = es ? rules::Lang::Es : rules::Lang::En;
  o.target = rules::Lang::Grc;
  o.fidelity = 2;
  o.speakerGender = 'f';
  return o;
}

bool hasFlag(const rules::CueOutput& o, const char* f) { return std::find(o.flags.begin(), o.flags.end(), f) != o.flags.end(); }

// One cue at a time; `expected` may list alternatives after " | ". `mustCheck`: the cue must not be OK (a rebuilt
// structure, a realia hypernym, a dropped word). Every rebuilt cue (clause-repair, participle-phrase) is never OK.
struct Case4 { const char* src; const char* expected; bool mustCheck; };
int run4(rules::Engine& e, const std::vector<Case4>& cases, bool es, const char* what) {
  int ok = 0;
  for (const Case4& k : cases) {
    rules::CueInput x;
    x.sourceText = k.src;
    auto r = e.translate({x}, opts4(es), rules::Context{}, nullptr, nullptr);
    REQUIRE(r.ok());
    const rules::CueOutput& o = r.value()[0];
    std::string got = o.target;
    std::replace(got.begin(), got.end(), '\n', ' ');
    bool hit = false;
    const std::string exp = k.expected;
    for (size_t a = 0;;) {
      const size_t b = exp.find(" | ", a);
      hit = hit || norm4(got) == norm4(text::nfc(exp.substr(a, b == std::string::npos ? std::string::npos : b - a)));
      if (b == std::string::npos) break;
      a = b + 3;
    }
    CHECK_MESSAGE(hit, what << ": " << k.src << " -> '" << got << "' expected '" << k.expected << "'");
    CHECK_MESSAGE(o.confidence != rules::Confidence::Fix, what << ": " << k.src << " is Fix");
    const bool rebuilt = hasFlag(o, "clause-repair") || hasFlag(o, "participle-phrase") || hasFlag(o, "realia");
    if (rebuilt || k.mustCheck)
      CHECK_MESSAGE(o.confidence != rules::Confidence::Ok, what << ": " << k.src << " was rebuilt / guessed but is OK");
    ok += hit;
  }
  MESSAGE(std::string(what) << ": " << ok << " / " << cases.size());
  return ok;
}

}  // namespace

TEST_CASE("rules-grc4: C21 constructions (degree words, weather verbs, negation order, glosses, participles)") {
  NEED_REAL4();
  auto e = engine4();
  const std::vector<Case4> en = {
      // (1) "such" + adjective -> οὕτω(ς) on the adjective; "such" + noun -> τοιοῦτος; "such" left out of the NP or
      // read as a genitive by the frame builder is found by its token
      {"I have never seen such a big dog.", "οὐδέποτε οὕτω μέγαν κύνα εἶδον. | οὐδέποτε τοιοῦτον κύνα εἶδον.", false},
      {"She has such a beautiful garden.", "οὕτω καλὸν κῆπον ἔχει.", false},
      {"I have never eaten such good bread.", "οὐδέποτε οὕτως ἀγαθὸν ἄρτον ἔφαγον.", false},
      {"We have never heard such a story.", "οὐδέποτε τοιοῦτον μῦθον ἠκούσαμεν.", false},
      {"Why do you have such a horse?", "διὰ τί τοιοῦτον ἵππον ἔχεις;", false},
      {"I have never seen such dogs.", "οὐδέποτε τοιούτους κύνας εἶδον.", false},
      {"Such dogs are dangerous.", "οἱ τοιοῦτοι κύνες δεινοί εἰσιν.", false},
      // "so ... that" keeps οὕτω(ς) ... ὥστε; a broken result clause is analysed in two parts (Check)
      {"The wind was so strong that the tree fell.", "ὁ ἄνεμος οὕτως ἰσχυρὸς ἦν ὥστε τὸ δένδρον ἔπεσεν.", true},
      {"The night was so dark that we saw nothing.", "ἡ νὺξ οὕτω σκοτεινὴ ἦν ὥστε οὐδὲν εἴδομεν.", false},
      // a two-word degree group is rendered whole and accounted for ("so fast")
      {"You ran so fast!", "οὕτω ταχέως ἔδραμες!", false},
      // (2) weather and nature verbs: subject rows (mid: παύομαι; mid-pres: δύεται / ἔδυ), impersonal verbs
      {"The rain stopped.", "ὁ ὄμβρος ἐπαύσατο.", false},
      {"The rain will stop soon.", "ὁ ὄμβρος αὐτίκα παύσεται.", false},
      {"The rain ceased.", "ὁ ὄμβρος ἐπαύσατο.", false},
      {"The rain will cease.", "ὁ ὄμβρος παύσεται.", false},
      {"The wind stopped.", "ὁ ἄνεμος ἐπαύσατο.", false},
      {"The wind will stop tomorrow.", "αὔριον ὁ ἄνεμος παύσεται.", false},
      {"The moon is setting.", "ἡ σελήνη δύεται.", false},
      {"The stars are setting.", "τὰ ἄστρα δύεται.", false},
      {"The sun is setting.", "ὁ ἥλιος δύεται.", false},
      {"The sun has set.", "ὁ ἥλιος ἔδυ.", false},
      {"The sun set.", "ὁ ἥλιος ἔδυ.", true},      // the frame builder makes "set" a noun: rebuilt (Check)
      {"The moon set.", "ἡ σελήνη ἔδυ.", true},
      {"The stars set.", "τὰ ἄστρα δύεται.", true},   // a neuter plural subject takes a singular verb
      {"It is raining.", "ὕει.", false},
      {"It rained yesterday.", "χθὲς ὗσεν.", false},
      {"It is snowing.", "νίφει.", false},
      {"It was snowing.", "ἔνιφεν.", false},
      {"The wind blows.", "ὁ ἄνεμος πνεῖ.", false},
      {"A cold wind was blowing.", "ψυχρὸς ἄνεμος ἔπνει.", false},
      {"It is late.", "ὀψέ ἐστιν.", false},
      {"It is early.", "πρῴ ἐστιν.", false},
      // (3) οὐ right before its verb: a time / place adverb goes first in a statement, after the verb in a question
      {"Why did you not come yesterday?", "διὰ τί οὐκ ἦλθες χθές;", false},
      {"Why did the children not play yesterday?", "διὰ τί οἱ παῖδες οὐκ ἔπαισαν χθές;", false},
      {"We did not see the dog yesterday.", "χθὲς τὸν κύνα οὐκ εἴδομεν.", false},
      {"I did not go to school today.", "σήμερον πρὸς τὸν διδάσκαλον οὐκ ἀπῆλθον.", false},
      // (4) gloss gaps: teacher glosses in tiers_grc.tsv (box, table, stick, game, ring, gold), lose -> ἀπόλλυμι,
      // nest -> καλιά as a realia hypernym until greek.vpl has νεοττιά (Check)
      {"Put the box on the table.", "θὲς τὸ κιβώτιον ἐπὶ τὴν τράπεζαν.", false},
      {"The box is under the table.", "τὸ κιβώτιον ὑπὸ τῇ τραπέζῃ ἐστίν.", false},
      {"The old man has a stick.", "ὁ γέρων βακτηρίαν ἔχει.", false},
      {"The shepherd carried a stick.", "ὁ ποιμὴν βακτηρίαν ἤνεγκεν.", false},
      {"This game is easy.", "αὕτη ἡ παιδιὰ ῥᾳδία ἐστίν.", false},
      {"The children love this game.", "οἱ παῖδες ταύτην τὴν παιδιὰν φιλοῦσιν.", false},
      {"The queen has a gold ring.", "ἡ βασίλεια χρυσοῦν δακτύλιον ἔχει.", false},
      {"I lost my ring.", "τὸν δακτύλιόν μου ἀπώλεσα.", false},
      {"The boy lost his ball.", "ὁ παῖς τὴν σφαῖραν αὐτοῦ ἀπώλεσεν.", false},
      {"The old woman has a magic wand.", "ἡ γραῦς μαγικὴν ῥάβδον ἔχει.", false},
      {"The shepherd hit the dog with a rod.", "ὁ ποιμὴν τὸν κύνα ῥάβδῳ ἔβαλεν. | ὁ ποιμὴν τὸν κύνα ῥάβδῳ ἔπαισεν.", false},
      {"The king has a seal.", "ὁ βασιλεὺς σφραγῖδα ἔχει.", false},
      {"The king has a golden cup.", "ὁ βασιλεὺς χρυσοῦν ποτήριον ἔχει.", false},
      {"The winter was long.", "ὁ χειμὼν μακρὸς ἦν.", false},
      {"The winter is cold.", "ὁ χειμὼν ψυχρός ἐστιν.", false},
      {"The game is difficult.", "ἡ παιδιὰ χαλεπή ἐστιν.", false},
      {"The bird has a nest.", "ὁ ὄρνις καλιὰν ἔχει. | ὁ ὄρνις νεοττιὰν ἔχει.", true},
      {"The nest is empty.", "ἡ καλιὰ κενή ἐστιν. | ἡ νεοττιὰ κενή ἐστιν.", true},
      {"The sea is dangerous.", "ἡ θάλαττα δεινή ἐστιν.", false},
      {"The road was difficult.", "ἡ ὁδὸς χαλεπὴ ἦν.", false},
      // the present participle of κάθημαι (καθήμενος, -η, -ον); "-ing phrase, S V" analysed in two parts (Check)
      {"Sitting under the tree, the girl sang.", "ἡ κόρη ὑπὸ τῷ δένδρῳ καθημένη ᾖσεν.", true},
      {"Sitting by the fire, the old man slept.", "ὁ γέρων παρὰ τῷ πυρὶ καθήμενος ἐκάθευδεν.", true},
      {"Sitting on the wall, the boys laughed.", "οἱ παῖδες ἐπὶ τοῦ τείχους καθήμενοι ἐγέλασαν.", true},
      // "by" + a definite place with an active verb -> παρά + dative
      {"The children played by the river.", "οἱ παῖδες παρὰ τῷ ποταμῷ ἔπαισαν. | οἱ παῖδες παρὰ τῷ ποταμῷ ἔπαιζον.", false},
      // (5) "Hurry, ..." -> σπεῦδε (σπεύδω durative); the reason clause with γάρ or in asyndeton
      {"Hurry, the ship is leaving!", "σπεῦδε, ἡ γὰρ ναῦς ἀποπλεῖ! | σπεῦδε, ἡ ναῦς ἀποπλεῖ!", false},
      {"Hurry, the teacher is coming!", "σπεῦδε, ὁ γὰρ διδάσκαλος ἔρχεται! | σπεῦδε, ὁ διδάσκαλος ἔρχεται!", false},
      // pron.encl: an enclitic pronoun never begins the sentence
      {"I love you.", "φιλῶ σε.", false},
      {"I see you.", "ὁρῶ σε.", false},
  };
  const int okEn = run4(*e, en, false, "C21 constructions EN");
  CHECK(okEn == (int)en.size());
  // a degree word the Greek does not render is never OK: "so" of "so much fun" is lost (A7, Check)
  {
    rules::CueInput x;
    x.sourceText = "It was so much fun.";
    auto r = e->translate({x}, opts4(false), rules::Context{}, nullptr, nullptr);
    REQUIRE(r.ok());
    const rules::CueOutput& o = r.value()[0];
    if (o.target.find(text::nfc("οὕτω")) == std::string::npos) {
      CHECK(o.confidence != rules::Confidence::Ok);
      bool a7 = false;
      for (const rules::Check& k : o.checks) a7 = a7 || (k.id == "A7" && !k.ok && k.detail.find("so") != std::string::npos);
      CHECK(a7);
    }
  }
  if (!real4().esOk) return;
  const std::vector<Case4> es = {
      {"Nunca he visto un perro tan grande.", "οὐδέποτε οὕτω μέγαν κύνα εἶδον.", false},
      {"La lluvia paró.", "ὁ ὄμβρος ἐπαύσατο.", false},
      {"La lluvia cesó.", "ὁ ὄμβρος ἐπαύσατο.", false},
      {"La lluvia se detuvo.", "ὁ ὄμβρος ἐπαύσατο.", false},
      {"La lluvia parará pronto.", "ὁ ὄμβρος αὐτίκα παύσεται.", false},
      {"La lluvia cesará.", "ὁ ὄμβρος παύσεται.", false},
      {"La lluvia se detiene.", "ὁ ὄμβρος παύεται.", false},
      {"La luna se pone.", "ἡ σελήνη δύεται.", false},
      {"Nevaba.", "ἔνιφεν.", false},
      {"El pájaro tiene un nido.", "ὁ ὄρνις καλιὰν ἔχει. | ὁ ὄρνις νεοττιὰν ἔχει.", true},
      {"El nido está vacío.", "ἡ καλιὰ κενή ἐστιν. | ἡ νεοττιὰ κενή ἐστιν.", true},
      {"La caja está sobre la mesa.", "τὸ κιβώτιον ἐπὶ τῆς τραπέζης ἐστίν.", false},
      {"El juego es fácil.", "ἡ παιδιὰ ῥᾳδία ἐστίν.", false},
      {"El sol se puso.", "ὁ ἥλιος ἔδυ.", false},
      {"La luna se puso.", "ἡ σελήνη ἔδυ.", false},
      {"El sol se pone.", "ὁ ἥλιος δύεται.", false},
      {"Llueve.", "ὕει.", false},
      {"Está lloviendo.", "ὕει.", false},
      {"Nieva.", "νίφει.", true},                      // read as an imperative by the tagger: rebuilt (Check)
      {"El viento sopla.", "ὁ ἄνεμος πνεῖ.", false},
      {"Soplaba un viento frío.", "ψυχρὸς ἄνεμος ἔπνει.", true},   // subject after the verb read as the object
      // the 2nd person preterite in -ste (the tagger said 3rd); the Spanish imperfect is the Greek imperfect
      {"¿Por qué no viniste ayer?", "διὰ τί οὐκ ἦλθες χθές;", false},
      {"Ayer no comiste nada.", "χθὲς οὐδὲν ἔφαγες.", false},
      {"La niña cantaba.", "ἡ παῖς ᾖδεν.", false},
      {"Los niños jugaban en el jardín.", "οἱ παῖδες ἐν τῷ κήπῳ ἔπαιζον.", false},
      {"Perdí mi anillo.", "τὸν δακτύλιόν μου ἀπώλεσα.", false},
      {"El niño perdió su pelota.", "ὁ παῖς τὴν σφαῖραν αὐτοῦ ἀπώλεσεν.", false},
  };
  const int okEs = run4(*e, es, true, "C21 constructions ES");
  CHECK(okEs == (int)es.size());
}

// Rules added after the first blind run (docs/rules_grc3_notes.md "Loop 4"): two or more own sentences each, none a
// blind sentence.
TEST_CASE("rules-grc4: C21 after the blind check (checker, touch, bring, sad, middle futures, vocatives, roles)") {
  NEED_REAL4();
  auto e = engine4();
  const std::vector<Case4> en = {
      // checker: a nom/acc neuter before a verb that can be 1st / 2nd person is its object (ἔφαγον, ἔλαβον: 1 sg / 3 pl)
      {"I took the book.", "τὸ βιβλίον ἔλαβον.", false},
      {"I saw the tree.", "τὸ δένδρον εἶδον.", false},
      // touch -> ἅπτομαι + genitive (verb row frame mid, valency mid:gen)
      {"Don't touch the dog!", "μὴ ἅπτου τοῦ κυνός!", false},
      {"The boy touched the horse.", "ὁ παῖς τοῦ ἵππου ἥψατο.", false},
      // bring a thing -> φέρω (frame thing); a person or an animal -> ἄγω; "me" read as the object is the dative
      {"Bring me the book.", "ἔνεγκόν μοι τὸ βιβλίον. | ἔνεγκέ μοι τὸ βιβλίον.", false},
      {"Bring the horse.", "ἄγαγε τὸν ἵππον. | ἄγε τὸν ἵππον.", false},
      {"Bring me some bread.", "ἔνεγκόν μοι ἄρτον. | ἔνεγκέ μοι ἄρτον.", false},
      // sad -> λυπέομαι (state row frame pass; no imperfect cells: the aorist ἐλυπήθη)
      {"Why are you so sad?", "διὰ τί οὕτω λυπεῖ; | διὰ τί οὕτω λυπῇ;", false},
      {"The girl was sad.", "ἡ κόρη ἐλυπήθη. | ἡ κόρη ἐλυπεῖτο.", false},
      {"The boy was sad yesterday.", "ὁ παῖς χθὲς ἐλυπήθη. | ὁ παῖς χθὲς ἐλυπεῖτο.", false},
      {"The old man is sad.", "ὁ γέρων λυπεῖται.", false},
      // teacher glosses: shoe -> ὑπόδημα, well -> φρέαρ, at home -> οἴκοι
      {"I lost my shoes.", "τὰ ὑποδήματά μου ἀπώλεσα.", false},
      {"The well is deep.", "τὸ φρέαρ βαθύ ἐστιν.", false},
      {"The well is near the house.", "τὸ φρέαρ ἐγγὺς τῆς οἰκίας ἐστίν.", false},
      {"The shoes are new.", "τὰ ὑποδήματα νέα ἐστίν. | τὰ ὑποδήματα καινά ἐστιν.", false},
      {"We will stay at home.", "οἴκοι μενοῦμεν.", false},
      {"The children are at home.", "οἱ παῖδες οἴκοι εἰσίν.", false},
      // ", so ..." after a statement -> ὥστε + indicative (an actual result)
      {"It is dark, so we cannot play.", "σκοτεινόν ἐστιν ὥστε οὐ δυνάμεθα παίζειν.", false},
      {"The door is closed, so we cannot go in.", "ἡ θύρα κέκλειται ὥστε οὐ δυνάμεθα εἰσελθεῖν.", false},
      // Attic middle futures (ἀκούσομαι, γελάσομαι, ἀναγνώσομαι); ἀκούω + the person in the genitive
      {"Will you listen to me?", "ἆρα ἀκούσῃ μου; | ἆρα ἀκούσει μου;", false},
      {"We will hear the song.", "τὴν ᾠδὴν ἀκουσόμεθα.", false},
      {"The children will laugh.", "οἱ παῖδες γελάσονται.", false},
      {"Will you read the letter?", "ἆρα ἀναγνώσῃ τὴν ἐπιστολήν; | ἆρα ἀναγνώσει τὴν ἐπιστολήν;", false},
      {"Listen to me!", "ἄκουέ μου!", false},
      {"Listen to your mother!", "ἄκουε τῆς μητρός σου!", false},
      // a capitalised address word is a vocative noun, not a name
      {"Father, where is the dog?", "ὦ πάτερ, ποῦ ἐστιν ὁ κύων;", false},
      {"Teacher, the boy is crying.", "ὦ διδάσκαλε, ὁ παῖς κλαίει.", false},
      // a time word read as the predicate noun with the adjective on it
      {"The sea is calm tonight.", "ἡ θάλαττα νύκτωρ γαληνός ἐστιν.", true},
      {"The house is quiet tonight.", "ἡ οἰκία νύκτωρ ἤρεμός ἐστιν.", false},
      // verbs of placing ask ποῦ
      {"Where did you put the key?", "ποῦ τὴν κλεῖν ἔθηκας;", false},
      {"Where did the boy put his book?", "ποῦ ὁ παῖς τὸ βιβλίον αὐτοῦ ἔθηκεν;", false},
      // English verbs with the bare form as their past: a 3rd singular subject and no auxiliary -> past
      {"The king put his seal on the letter.", "ὁ βασιλεὺς τὴν σφραγῖδα αὐτοῦ ἐπὶ τὴν ἐπιστολὴν ἔθηκεν.", false},
      {"The girl cut the bread.", "ἡ κόρη τὸν ἄρτον ἔτεμεν.", false},
      // loud -> βοάω (state row; was "μὴ ἴσθι οὕτω μέγα", "so big")
      {"Don't be so loud!", "μὴ βόα οὕτως! | μὴ οὕτω βόα!", false},
      {"The children are loud.", "οἱ παῖδες βοῶσιν.", false},
      // the thing told read as the indirect object is the object
      {"The boy told the truth.", "ὁ παῖς τὴν ἀλήθειαν ἔλεξεν.", false},
      {"The old man told a long story.", "ὁ γέρων μακρὸν μῦθον ἔλεξεν.", false},
  };
  const int okEn = run4(*e, en, false, "C21 after blind EN");
  CHECK(okEn == (int)en.size());
  // a determiner before a word read as an adverb ("of the well" -> εὖ) is never OK
  {
    rules::CueInput x;
    x.sourceText = "The water of the well is cold.";
    auto r = e->translate({x}, opts4(false), rules::Context{}, nullptr, nullptr);
    REQUIRE(r.ok());
    if (r.value()[0].target.find(text::nfc("εὖ")) != std::string::npos)
      CHECK(r.value()[0].confidence != rules::Confidence::Ok);
  }
  // the checker: a preposition, a second-position particle, then its noun ("ἐν δὲ τῷ κήπῳ") is one phrase (A4 ok)
  {
    rules::CueInput x;
    x.sourceText = "But in the garden we stay.";
    for (const char* t : {"ἐν δὲ τῷ κήπῳ μένομεν.", "ἐν οὖν οἰκίᾳ μένομεν."}) {
      auto o = e->check(x, text::nfc(t), opts4(false), rules::Context{});
      REQUIRE(o.ok());
      bool a4 = true;
      for (const rules::Check& k : o.value().checks) a4 = a4 && (k.id != "A4" || k.ok);
      CHECK_MESSAGE(a4, t);
    }
  }
  if (!real4().esOk) return;
  const std::vector<Case4> es = {
      // the person of a Spanish simple verb form from spanish.vpl when the form has only one (puse, tocas, comas)
      {"Puse el libro en la mesa.", "τὸ βιβλίον ἐν τῇ τραπέζῃ ἔθηκα.", false},
      {"No tocas el fuego.", "τοῦ πυρὸς οὐχ ἅπτῃ. | τοῦ πυρὸς οὐχ ἅπτει.", false},
      {"No comas el pan.", "μὴ ἔσθιε τὸν ἄρτον.", false},
      {"No corráis.", "μὴ τρέχετε.", false},
      // a negative 2nd-person present subjunctive in a main clause is a prohibition (rebuilt: Check)
      {"No toques el fuego.", "μὴ ἅπτου τοῦ πυρός.", true},
      {"¡No toques eso!", "μὴ ἅπτου ἐκείνου!", true},
      // poner -> τίθημι; triste -> λυπέομαι; traer -> φέρω; en casa -> οἴκοι
      {"¿Dónde pusiste la llave?", "ποῦ τὴν κλεῖν ἔθηκας;", false},
      {"¿Por qué estás tan triste?", "διὰ τί οὕτω λυπεῖ; | διὰ τί οὕτω λυπῇ;", false},
      {"La niña está triste.", "ἡ παῖς λυπεῖται.", false},
      {"Trae el libro.", "ἔνεγκον τὸ βιβλίον. | ἔνεγκε τὸ βιβλίον.", false},
      {"Traje agua.", "ὕδωρ ἤνεγκα.", false},
      {"Comemos en casa.", "οἴκοι ἐσθίομεν.", false},
      {"Los niños son ruidosos.", "οἱ παῖδες βοῶσιν.", false},
      {"¡No seas tan ruidoso!", "μὴ βόα οὕτως! | μὴ οὕτω βόα!", false},
      {"El perro duerme en casa.", "ὁ κύων οἴκοι καθεύδει.", false},
  };
  const int okEs = run4(*e, es, true, "C21 after blind ES");
  CHECK(okEs == (int)es.size());
}

// readable_grc.tsv rows of loop 4 (Greek -> English / Spanish glosses)
TEST_CASE("rules-grc4: GRC -> EN / ES glosses of the loop 4 words") {
  NEED_REAL4();
  auto e = engine4();
  struct G { const char* grc; const char* en; const char* es; };
  const std::vector<G> cases = {
      {"ὁ ἄνεμος πνεῖ.", "wind", "viento"},
      {"ὁ ὄμβρος ἐπαύσατο.", "rain", "lluvia"},
      {"τὸ κιβώτιον ἐπὶ τῆς τραπέζης ἐστίν.", "table", "mesa"},
      {"ὁ γέρων βακτηρίαν ἔχει.", "stick", "bastón"},
      {"αὕτη ἡ παιδιὰ ῥᾳδία ἐστίν.", "game", "juego"},
      {"ἡ βασίλεια δακτύλιον ἔχει.", "ring", "anillo"},
      {"ὁ βασιλεὺς σφραγῖδα ἔχει.", "seal", "sello"},
      {"νίφει.", "snow", "niev"},
      {"ὕει.", "rain", "llue"},
      {"τὸ κιβώτιον καλόν ἐστιν.", "box", "caja"},
  };
  for (const G& g : cases) {
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
}

TEST_CASE("rules-grc4: κάθημαι participle cells, δύω root aorist") {
  NEED_REAL4();
  const lex::Lexicon& lx = real4().grc;
  const uint32_t kath = grc::findLemma(lx, "κάθημαι", feat::Verb);
  REQUIRE(kath != lex::kNoLemma);
  struct P { uint8_t number, gender; const char* form; };
  for (const P& p : {P{feat::Sg, feat::M, "καθήμενος"}, P{feat::Sg, feat::F, "καθημένη"}, P{feat::Sg, feat::N, "καθήμενον"},
                     P{feat::Pl, feat::M, "καθήμενοι"}, P{feat::Pl, feat::F, "καθήμεναι"}}) {
    std::string out;
    CHECK(grc::participle(lx, kath, feat::Present, 0, feat::Nom, p.number, p.gender, out));
    CHECK_MESSAGE(out == text::nfc(p.form), out);
  }
  // an ordinary verb keeps its own present participle cells (τρέχων)
  {
    const uint32_t trecho = grc::findLemma(lx, "τρέχω", feat::Verb);
    REQUIRE(trecho != lex::kNoLemma);
    std::string out;
    CHECK(grc::participle(lx, trecho, feat::Present, 0, feat::Nom, feat::Sg, feat::M, out));
    CHECK(out == text::nfc("τρέχων"));
  }
  // δύω: the intransitive root aorist ἔδυ (the lexicon flags the cell non-Attic), the middle in the present
  const uint32_t duo = grc::findLemma(lx, "δύω", feat::Verb);
  REQUIRE(duo != lex::kNoLemma);
  feat::Features f;
  f.pos = feat::Verb;
  f.mood = feat::Indicative;
  f.tense = feat::Aorist;
  f.person = feat::P3;
  f.number = feat::Sg;
  std::string out;
  CHECK(grc::generate(lx, duo, f, out));
  CHECK(out == text::nfc("ἔδυ"));
  f.tense = feat::Present;
  f.voice = feat::Middle;
  CHECK(grc::generate(lx, duo, f, out));
  CHECK(out == text::nfc("δύεται"));
}
