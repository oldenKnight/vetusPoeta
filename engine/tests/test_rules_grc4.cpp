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
      {"The wind stopped.", "ὁ ἄνεμος ἐπαύσατο.", false},
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
