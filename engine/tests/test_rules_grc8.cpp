// engine/rules Greek loop 8 (C33): grc2x readability (a lemma without a readable_grc.tsv row takes the head gloss of
// its dictionary entry, never the whole definition: "The leader or commander of an army: general" -> "general"; readable
// rows for the commonest military, political, family and everyday words), the times of day ("at dawn / al amanecer"
// -> ἅμα τῇ ἕῳ, "at dusk / al atardecer" -> πρὸς ἑσπέραν, "at noon / a mediodía" -> μεσημβρίας) and the fixes after
// the blind batch of 10 English + 10 Spanish sentences. Every rule is checked on our own sentences (two or more each,
// written before the rule: docs/rules_grc3_notes.md "Loop 8"); none is a blind-check sentence. Needs the real data
// (VP_DATA_WORK); skips otherwise.
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
#include "vp/grc2x.h"
#include "vp/lex.h"
#include "vp/morph.h"
#include "vp/morph_grc.h"
#include "vp/nlp.h"
#include "vp/realise_grc.h"
#include "vp/rules.h"
#include "vp/text.h"
#include "vp/transfer_grc.h"

namespace stdfs = std::filesystem;
using namespace vp;

namespace {

stdfs::path repo8() { return stdfs::path(VP_FIXTURES_DIR).parent_path().parent_path(); }
stdfs::path work8() {
  const char* e = std::getenv("VP_DATA_WORK");
  return e && *e ? stdfs::path(e) : repo8() / "data" / "work";
}

struct Real8 {
  lex::Lexicon grc, en, es;
  nlp::Pipeline pen, pes;
  bool ok = false, esOk = false;
  std::string why;
};
const Real8& real8() {
  static Real8 r = [] {
    Real8 x;
    const char* g = std::getenv("VP_GREEK_VPL");
    auto grc = lex::Lexicon::open(g && *g ? stdfs::path(g) : work8() / "greek.vpl");
    auto en = lex::Lexicon::open(work8() / "english.vpl");
    auto pen = nlp::Pipeline::open(nlp::Lang::En, (work8() / "nlp" / "english.tag.vpt").string(),
                                   (work8() / "nlp" / "english.dep.vpt").string());
    if (!grc.ok() || !en.ok() || !pen.ok()) {
      x.why = !grc.ok() ? grc.error().message : !en.ok() ? en.error().message : pen.error().message;
      return x;
    }
    x.grc = std::move(grc.value());
    x.en = std::move(en.value());
    x.pen = std::move(pen.value());
    x.ok = true;
    auto es = lex::Lexicon::open(work8() / "spanish.vpl");
    auto pes = nlp::Pipeline::open(nlp::Lang::Es, (work8() / "nlp" / "spanish.tag.vpt").string(),
                                   (work8() / "nlp" / "spanish.dep.vpt").string());
    if (es.ok() && pes.ok()) {
      x.es = std::move(es.value());
      x.pes = std::move(pes.value());
      x.esOk = true;
    }
    return x;
  }();
  return r;
}
#define NEED_REAL8()                                                                                 \
  if (!real8().ok) {                                                                                 \
    MESSAGE("real Greek data not available (" << real8().why << "); set VP_DATA_WORK. Skipped.");   \
    return;                                                                                          \
  }

std::unique_ptr<rules::Engine> engine8() {
  rules::EngineConfig cfg = rules::defaultEngineConfig();
  cfg.curatedDir = (repo8() / "data" / "curated").string();
  cfg.dataDir = work8().string();
  cfg.nlpDir = (work8() / "nlp").string();
  auto e = rules::makeEngine(cfg);
  e->setLexicons(nullptr, &real8().grc, &real8().en, real8().esOk ? &real8().es : nullptr);
  return e;
}

// As the regression: NFC, lower case, punctuation-insensitive, spaces collapsed; accents and breathings count.
std::string norm8(const std::string& s) {
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

rules::Options opts8(bool es) {
  rules::Options o;
  o.source = es ? rules::Lang::Es : rules::Lang::En;
  o.target = rules::Lang::Grc;
  o.fidelity = 2;
  o.speakerGender = 'f';
  return o;
}

bool hasFlag8(const rules::CueOutput& o, const char* f) {
  return std::find(o.flags.begin(), o.flags.end(), f) != o.flags.end();
}

// One cue at a time; `expected` may list alternatives after " | ". `mustCheck`: the cue must not be OK (a rebuilt
// structure, a repaired misreading, a calque, a rule form). Every rebuilt cue is never OK.
struct Case8 { const char* src; const char* expected; bool mustCheck; };
int run8(rules::Engine& e, const std::vector<Case8>& cases, bool es, const char* what) {
  int ok = 0;
  for (const Case8& k : cases) {
    rules::CueInput x;
    x.sourceText = k.src;
    auto r = e.translate({x}, opts8(es), rules::Context{}, nullptr, nullptr);
    REQUIRE(r.ok());
    const rules::CueOutput& o = r.value()[0];
    std::string got = o.target;
    std::replace(got.begin(), got.end(), '\n', ' ');
    bool hit = false;
    const std::string exp = k.expected;
    for (size_t a = 0;;) {
      const size_t b = exp.find(" | ", a);
      hit = hit || norm8(got) == norm8(text::nfc(exp.substr(a, b == std::string::npos ? std::string::npos : b - a)));
      if (b == std::string::npos) break;
      a = b + 3;
    }
    CHECK_MESSAGE(hit, what << ": " << k.src << " -> '" << got << "' expected '" << k.expected << "'");
    CHECK_MESSAGE(o.confidence != rules::Confidence::Fix, what << ": " << k.src << " is Fix");
    bool rebuilt = false;
    for (const char* f : {"clause-repair", "participle-phrase", "realia", "past-form", "from-rule", "det-adverb",
                          "light-verb", "subject-guess", "speech-inversion", "idiom", "lexicon-gap", "addressee-guess"})
      rebuilt = rebuilt || hasFlag8(o, f);
    if (rebuilt || k.mustCheck)
      CHECK_MESSAGE(o.confidence != rules::Confidence::Ok, what << ": " << k.src << " was rebuilt / guessed but is OK");
    ok += hit;
  }
  MESSAGE(std::string(what) << ": " << ok << " / " << cases.size());
  return ok;
}

// GRC -> EN / ES readable sentence (normalised as norm8).
std::string toX8(rules::Engine& e, const char* grc, bool es) {
  rules::Options o;
  o.source = rules::Lang::Grc;
  o.target = es ? rules::Lang::Es : rules::Lang::En;
  o.fidelity = 2;
  rules::CueInput x;
  x.sourceText = text::nfc(grc);
  auto r = e.translate({x}, o, rules::Context{}, nullptr, nullptr);
  REQUIRE(r.ok());
  return r.value()[0].target;
}

}  // namespace

// Debug: raw dictionary glosses and the head gloss of Greek lemmas (VP_GRC8_GLOSS=<file of Greek heads>).
TEST_CASE("rules-grc8: debug glosses (VP_GRC8_GLOSS=<file>)") {
  const char* e = std::getenv("VP_GRC8_GLOSS");
  if (!e || !*e) return;
  NEED_REAL8();
  const lex::Lexicon& lx = real8().grc;
  std::ifstream in(e);
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty()) continue;
    const uint32_t id = grc::findLemma(lx, text::nfc(line));
    if (id == lex::kNoLemma) { std::cout << line << "\t(unknown)\n"; continue; }
    const lex::Lemma l = lx.lemma(id);
    std::cout << line << "\tpos " << (int)l.pos << " rank " << l.freqRank << "\t" << l.glossEn << "\t" << l.glossEs
              << "\t-> " << grc2x::headGloss(std::string(l.glossEn), true) << " | " << grc2x::headGloss(std::string(l.glossEs), false)
              << "\n";
  }
}

// Debug: Greek sentences of a file -> English and Spanish (VP_GRC8_GRC2X=<file>).
TEST_CASE("rules-grc8: grc2x try (VP_GRC8_GRC2X=<file>)") {
  const char* e = std::getenv("VP_GRC8_GRC2X");
  if (!e || !*e) return;
  NEED_REAL8();
  auto en = engine8();
  std::ifstream in(e);
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty()) continue;
    std::cout << line << "\t" << toX8(*en, line.c_str(), false) << "\t" << toX8(*en, line.c_str(), true) << "\n";
  }
}

// C33 work item 1: a lemma without a readable row prints its head gloss; the readable rows for the commonest words.
TEST_CASE("rules-grc8: head gloss of a dictionary gloss") {
  struct H { const char* gloss; bool en; const char* head; };
  for (const H& h : {H{"A leader or commander of an army: general", true, "general"},
                     H{"something said: word, speech, conversation", true, "word"},
                     H{"The place of one's father/ancestors: fatherland, hometown", true, "fatherland"},
                     H{"of parties giving or receiving hospitality: host and much\xE2\x80\xA6", true, "host"},
                     H{"to loose; to loosen; to untie", true, "loose"},
                     H{"(of persons) brave, manly", true, "brave"},
                     H{"one who is armed, heavily armored", true, "one who is armed"},
                     H{"of or from the gods or God, divine", true, "of or from the gods or god"},
                     H{"to help somebody", true, "help"},
                     H{"a cake or loaf", true, "cake or loaf"},
                     H{"el escudo (de bronce)", false, "escudo"},
                     H{"comandante, almirante", false, "comandante"}}) {
    const std::string got = grc2x::headGloss(h.gloss, h.en);
    CHECK_MESSAGE(got == h.head, h.gloss << " -> '" << got << "' expected '" << h.head << "'");
  }
}

TEST_CASE("rules-grc8: GRC -> EN / ES readable glosses (military, political, family, everyday) and times of day") {
  NEED_REAL8();
  auto e = engine8();
  struct G { const char* grc; const char* en; const char* es; };
  // our own Attic sentences (the probe of docs/rules_grc3_notes.md "Loop 8"; the old outputs in the notes)
  const std::vector<G> cases = {
      {"ὁ στρατηγὸς τοὺς στρατιώτας εἰς τὴν πόλιν ἤγαγεν.", "The general led the soldiers into the city.",
       "El general llevó a los soldados a la ciudad."},
      {"ὁ δῆμος τὸν στρατηγὸν ἐτίμησεν.", "The people honoured the general.", "El pueblo honró al general."},
      {"ἡ βουλὴ ἐν τῇ ἀγορᾷ συνῆλθεν.", "The council came together in the market place.", "El consejo se reunió en la plaza."},
      {"οἱ πολῖται τοὺς νόμους φυλάττουσιν.", "The citizens guard the laws.", "Los ciudadanos guardan las leyes."},
      {"οἱ ὁπλῖται τὰς ἀσπίδας ἔφερον.", "The hoplites were bringing the shields.", "Los hoplitas traían los escudos."},
      {"ὁ ἡγεμὼν τὴν ὁδὸν ἔδειξεν.", "The leader showed the road.", "El jefe enseñó el camino."},
      {"ὁ ἄρχων τὸν νόμον ἔγραψεν.", "The ruler wrote the law.", "El gobernante escribió la ley."},
      {"οἱ σύμμαχοι ἀφίκοντο.", "The allies arrived.", "Los aliados arribaron."},
      {"ὁ στρατὸς πρὸς τὸ ὄρος ἐπορεύετο.", "The army was marching to the mountain.", "El ejército marchaba hacia la montaña."},
      {"ὁ κῆρυξ τὴν εἰρήνην ἤγγειλεν.", "The herald announced the peace.", "El heraldo anunció la paz."},
      {"ἡ νίκη καλή ἐστιν.", "The victory is beautiful.", "La victoria es hermosa."},
      {"ὁ ναύαρχος τὰς ναῦς ἔπεμψεν.", "The admiral sent the ships.", "El almirante envió las naves."},
      {"ὁ τύραννος τῆς πόλεως ἦρχεν.", "The tyrant was ruling the city.", "El tirano gobernaba la ciudad."},
      {"οἱ φύλακες τὰς πύλας ἔκλεισαν.", "The guards shut the gates.", "Los guardias cerraron las puertas."},
      {"ἡ μήτηρ τῷ παιδὶ μῦθον λέγει.", "The mother tells a story to the boy.", "La madre cuenta un cuento al niño."},
      {"ὁ δεσπότης τὸν οἰκέτην ἔπεμψεν.", "The master sent the servant.", "El amo envió al criado."},
      {"ἡ τροφὸς τὸ βρέφος ἔχει.", "The nurse has the baby.", "La nodriza tiene al bebé."},
      {"ὁ διδάσκαλος τοὺς μαθητὰς διδάσκει.", "The teacher teaches the pupils.", "El maestro enseña a los alumnos."},
      {"ὁ ἔμπορος τὸν οἶνον ἐπώλησεν.", "The merchant sold the wine.", "El comerciante vendió el vino."},
      {"ἡ κόρη ἄνθη συλλέγει.", "The girl gathers flowers.", "La muchacha recoge flores."},
      {"τὸ δεῖπνον ἕτοιμόν ἐστιν.", "The meal is ready.", "La comida está lista."},
      {"ὁ ἄγγελος τὴν ἐπιστολὴν ἐκόμισεν.", "The messenger brought the letter.", "El mensajero llevó la carta."},
      {"οἱ ἄνθρωποι τοὺς θεοὺς σέβονται.", "The men revere the gods.", "Los hombres veneran a los dioses."},
      {"ὁ κριτὴς τὸν κλέπτην ἐκόλασεν.", "The judge punished the thief.", "El juez castigó al ladrón."},
      {"οἱ νεανίαι ἐν τῷ γυμνασίῳ γυμνάζονται.", "The young men exercise in the gymnasium.",
       "Los jóvenes se ejercitan en el gimnasio."},
      {"ὁ φίλος μοι ἐβοήθησεν.", "The friend helped me.", "El amigo me ayudó."},
      {"ὁ γέρων ἐπὶ τῆς κλίνης κεῖται.", "The old man lies on the bed.", "El viejo yace sobre el lecho."},
      {"ἡ πατρὶς ἐλευθέρα ἐστίν.", "The fatherland is free.", "La patria es libre."},
      // times of day read back
      {"ἅμα τῇ ἕῳ οἱ στρατιῶται ἀπῆλθον.", "At dawn the soldiers went away.", "Al amanecer los soldados se fueron."},
      {"ἡ ναῦς πρὸς ἑσπέραν ἀφίκετο.", "The ship arrived towards evening.", "La nave arribó al atardecer."},
      {"οἱ παῖδες μεσημβρίας καθεύδουσιν.", "The boys sleep at noon.", "Los niños duermen a mediodía."},
      {"οἱ λύκοι νυκτὸς ἔρχονται.", "The wolves are coming at night.", "Los lobos vienen de noche."},
      // πάλιν ἥξω, ἐφάνθη
      {"αὔριον πάλιν ἥξομεν.", "Tomorrow we will come back.", "Mañana volveremos."},
      {"οἱ στρατιῶται πάλιν ἥξουσιν.", "The soldiers will come back.", "Los soldados volverán."},
      {"ἡ σελήνη ἐφάνθη.", "The moon appeared.", "La luna apareció."},
  };
  int ok = 0;
  for (const G& g : cases) {
    const std::string en = toX8(*e, g.grc, false), es = toX8(*e, g.grc, true);
    CHECK_MESSAGE(en == g.en, g.grc << " -> '" << en << "' expected '" << g.en << "'");
    CHECK_MESSAGE(es == g.es, g.grc << " -> '" << es << "' expected '" << g.es << "'");
    ok += en == g.en && es == g.es;
  }
  MESSAGE("C33 GRC -> EN / ES: " << ok << " / " << cases.size());
}

// C33 work items 2 and 3, each on our own sentences (two or more per rule, written before the rule).
TEST_CASE("rules-grc8: C33 constructions (times of day, Attic futures of ἔρχομαι, loop-7 faults)") {
  NEED_REAL8();
  auto e = engine8();
  const std::vector<Case8> en = {
      // (2) times of day
      {"The soldiers left the camp at dawn.", "οἱ στρατιῶται τὸ στρατόπεδον ἅμα τῇ ἕῳ ἔλιπον.", false},
      {"At dawn the farmer goes to the field.", "ἅμα τῇ ἕῳ ὁ γεωργὸς εἰς τὸν ἀγρὸν βαίνει.", false},
      {"The ship arrived at dusk.", "ἡ ναῦς πρὸς ἑσπέραν ἀφίκετο.", false},
      {"We will come back at dusk.", "πρὸς ἑσπέραν πάλιν ἥξομεν.", false},
      {"The children sleep at noon.", "οἱ παῖδες μεσημβρίας καθεύδουσιν.", false},
      {"At noon the sun is hot.", "μεσημβρίας ὁ ἥλιος θερμός ἐστιν.", false},
      {"The wolves come at night.", "οἱ λύκοι νύκτωρ ἔρχονται.", false},
      {"In the morning my mother bakes bread.", "ἕωθεν ἡ μήτηρ μου ἄρτον ὀπτᾷ.", false},
      // (3) the Attic future of ἔρχομαι and its compounds (ἐλεύσῃ is Ionic; [ἐπανέρχομαι] had no future)
      {"You will come tomorrow.", "αὔριον ἥξεις.", false},
      {"Will you come with me?", "ἆρα ἥξεις μετὰ ἐμοῦ; | ἆρα ἥξεις μετ' ἐμοῦ;", false},
      {"We will come back tomorrow.", "αὔριον πάλιν ἥξομεν.", false},
      {"The soldiers will go back home.", "οἱ στρατιῶται πάλιν οἴκαδε ἥξουσιν.", false},
      {"The soldiers will go out of the camp.", "οἱ στρατιῶται ἐκ τοῦ στρατοπέδου ἐξίασιν.", false},
      {"They will go away tomorrow.", "αὔριον ἀπίασιν.", false},
      {"The boy came out of the house.", "ὁ παῖς ἐκ τῆς οἰκίας ἐξῆλθεν.", false},
      {"Get out of the water!", "ἔξελθε ἐκ τοῦ ὕδατος!", false},
      // (3) the loop-7 Check faults: appeared (ἐδόκει "seemed" was OK), the king, my father (an address, OK), tall
      // (βαθύς "deep" for alto), the generic article, strong / sad of a thing, began (ἦρξεν "ruled")
      {"The girl appeared at the door.", "ἡ κόρη ἐν τῇ θύρᾳ ἐφάνθη.", true},
      {"The moon appeared.", "ἡ σελήνη ἐφάνθη.", true},
      {"I saw the king, my father.", "τὸν βασιλέα εἶδον, ὦ πάτερ.", true},
      {"We met the teacher, my friend.", "τῷ διδασκάλῳ ἐνετύχομεν, ὦ φίλε.", true},
      {"Life is good.", "ὁ βίος ἀγαθός ἐστιν.", false},
      {"Water is cold.", "τὸ ὕδωρ ψυχρόν ἐστιν.", false},
      {"Dogs are loyal.", "οἱ κύνες πιστοί εἰσιν.", false},
      {"My brother is tall.", "ὁ ἀδελφός μου μακρός ἐστιν.", false},
      {"The tree is tall.", "τὸ δένδρον ὑψηλόν ἐστιν.", false},
      {"The wall is strong.", "τὸ τεῖχος ἰσχυρόν ἐστιν.", false},
      {"The story is sad.", "ὁ μῦθος λυπηρός ἐστιν.", false},
      {"The war began.", "ὁ πόλεμος ἤρξατο.", false},
  };
  const int okEn = run8(*e, en, false, "C33 constructions EN");
  CHECK(okEn == (int)en.size());
  if (!real8().esOk) return;
  const std::vector<Case8> es = {
      // (2) times of day: "al atardecer" read as a verb is repaired (Check)
      {"Los soldados salieron del campamento al amanecer.", "οἱ στρατιῶται ἐκ τοῦ στρατοπέδου ἅμα τῇ ἕῳ ἐξῆλθον.", false},
      {"Al amanecer el campesino va al campo.", "ἅμα τῇ ἕῳ ὁ γεωργὸς εἰς τὸν ἀγρὸν βαίνει.", false},
      {"El barco llegó al atardecer.", "τὸ πλοῖον πρὸς ἑσπέραν ἀφίκετο.", true},
      {"Volveremos al atardecer.", "πρὸς ἑσπέραν πάλιν ἥξομεν.", true},
      {"Los niños duermen a mediodía.", "οἱ παῖδες μεσημβρίας καθεύδουσιν.", false},
      {"Los lobos vienen de noche.", "οἱ λύκοι νύκτωρ ἔρχονται.", false},
      {"Por la mañana mi madre hace pan.", "ἕωθεν ἡ μήτηρ μου ἄρτον ποιεῖ.", false},   // παρὰ τῇ ἕῳ was OK
      {"El pastor volvió a casa por la noche.", "ὁ ποιμὴν νύκτωρ οἴκαδε ἐπανῆλθεν.", false},
      // (3) futures, conmigo / contigo, salir de
      {"Vendrás mañana.", "αὔριον ἥξεις.", false},
      {"Volveremos mañana.", "αὔριον πάλιν ἥξομεν.", false},
      {"¿Vendrás conmigo?", "ἆρα ἥξεις μετὰ ἐμοῦ; | ἆρα ἥξεις μετ' ἐμοῦ;", false},
      {"Voy contigo.", "μετὰ σοῦ βαίνω.", false},
      {"Los soldados volverán a casa.", "οἱ στρατιῶται πάλιν οἴκαδε ἥξουσιν.", false},
      {"Los soldados salieron del campamento.", "οἱ στρατιῶται ἐκ τοῦ στρατοπέδου ἐξῆλθον.", false},
      // (3) the loop-7 faults on the Spanish side
      {"La niña apareció en la puerta.", "ἡ παῖς ἐν τῇ θύρᾳ ἐφάνθη.", true},
      {"Mi hermano es alto.", "ὁ ἀδελφός μου μακρός ἐστιν.", false},
      {"La muralla es alta.", "τὸ τεῖχος ὑψηλόν ἐστιν.", false},
      {"El árbol es alto.", "τὸ δένδρον ὑψηλόν ἐστιν.", false},
      {"Mi cuerpo es fuerte.", "τὸ σῶμά μου ἰσχυρόν ἐστιν.", false},
      {"La muerte es triste.", "ὁ θάνατος λυπηρός ἐστιν.", false},
      {"La cena está lista.", "τὸ δεῖπνον ἕτοιμόν ἐστιν.", false},                 // σοφόν "clever" was OK
      {"La comida ya está lista.", "τὸ δεῖπνον ἤδη ἕτοιμόν ἐστιν.", false},
      {"Mi hermana es muy lista.", "ἡ ἀδελφή μου πάνυ σοφή ἐστιν.", false},
      {"La guerra empezó.", "ὁ πόλεμος ἤρξατο.", false},
  };
  const int okEs = run8(*e, es, true, "C33 constructions ES");
  CHECK(okEs == (int)es.size());
}

// C33 after the blind batch (docs/rules_grc3_notes.md "Loop 8"): our own sentences, written at 12:54 UTC before the
// fixes; none is a blind sentence.
TEST_CASE("rules-grc8: C33 after the blind check (time words, anywhere, street, enseñar, hijo mío, miedo de, reunirse)") {
  NEED_REAL8();
  auto e = engine8();
  const std::vector<Case8> en = {
      // a bare time word the parser hangs on the noun before it ("of:tomorrow" -> ἐπιουσίου)
      {"We will walk to the river tomorrow.", "αὔριον εἰς τὸν ποταμὸν βαδιούμεθα.", false},
      {"They will go to the market tomorrow.", "αὔριον εἰς τὴν ἀγορὰν ἴασιν.", false},
      {"The king will visit the city today.", "σήμερον ὁ βασιλεὺς τὴν πόλιν ἐπισκέψεται.", false},
      {"My uncle came from the village yesterday.", "ὁ θεῖός μου ἐκ τῆς κώμης χθὲς ἦλθεν.", false},
      // "anywhere" in a negative clause -> οὐδαμοῦ (που "somewhere" was OK and wrong)
      {"I cannot see the cat anywhere.", "οὐδαμοῦ δύναμαι τὴν γαλῆν ἰδεῖν.", false},
      {"We could not find the dog anywhere.", "οὐδαμοῦ ἐδυνάμεθα τὸν κύνα εὑρεῖν.", false},
      // street -> ὁδός (πλατεῖα is Hellenistic); in the evening -> ἑσπέρας
      {"The children play in the street.", "οἱ παῖδες ἐν τῇ ὁδῷ παίζουσιν.", false},
      {"In the evening we read a book.", "ἑσπέρας βιβλίον ἀνέγνωμεν. | ἑσπέρας βιβλίον ἀναγιγνώσκομεν.", false},
      {"The birds sing in the evening.", "οἱ ὄρνιθες ἑσπέρας ᾄδουσιν.", false},
      // the double accusative of διδάσκω
      {"My father taught me a new word.", "ὁ πατήρ μού με νέον λόγον ἐδίδαξεν.", false},
      {"The teacher taught the children a song.", "ὁ διδάσκαλος ᾠδὴν τοὺς παῖδας ἐδίδαξεν.", false},
  };
  const int okEn = run8(*e, en, false, "C33 after blind EN");
  CHECK(okEn == (int)en.size());
  if (!real8().esOk) return;
  const std::vector<Case8> es = {
      {"Los perros corren en la calle.", "οἱ κύνες ἐν τῇ ὁδῷ τρέχουσιν.", false},
      {"Mi casa está en esta calle.", "ὁ οἶκός μου ἐν ταύτῃ τῇ ὁδῷ ἐστιν.", false},
      // the subject noun read as the root ("El maestro nos enseñó ..." lost the teacher), the double accusative
      {"La maestra nos enseñó un juego.", "ἡ διδάσκαλος ἡμᾶς παιδιὰν ἐδίδαξεν.", false},   // C35: relaxed (the frame reroots the subject noun since C34; esRootSubject removed)
      {"Mi padre me enseñó una palabra nueva.", "ὁ πατήρ μού με καινὸν λόγον ἐδίδαξεν. | ὁ πατήρ μού με καινὴν λέξιν ἐδίδαξεν.", false},   // C35: alternative added (palabra -> λέξις)   // C35: relaxed (the frame reroots the subject noun since C34; esRootSubject removed)
      // the possessive after the address word
      {"¡Ven conmigo, hija mía!", "ἐλθὲ μετὰ ἐμοῦ, ὦ θύγατερ! | ἐλθὲ μετ' ἐμοῦ, ὦ θύγατερ!", true},
      {"¿Dónde estás, hijo mío?", "ποῦ εἶ, ὦ υἱέ;", true},
      // tener miedo de X: X is the object ("No tengo miedo del lobo." -> "Οὐ φοβοῦμαι." was OK and wrong)
      {"La niña tiene miedo del perro.", "ἡ παῖς τὸν κύνα φοβεῖται.", false},
      {"No tenemos miedo de la noche.", "τὴν νύκτα οὐ φοβούμεθα.", false},
      // reunirse -> συνέρχομαι (also when the parser reads "se" as a passive), plaza -> ἀγορά
      {"Los soldados se reunieron en el campo.", "οἱ στρατιῶται ἐν τῷ ἀγρῷ συνῆλθον.", false},
      {"Las mujeres se reunieron en la plaza.", "αἱ γυναῖκες ἐν τῇ ἀγορᾷ συνῆλθον.", false},
      {"Los ciudadanos se reunieron en la plaza.", "οἱ πολῖται ἐν τῇ ἀγορᾷ συνῆλθον.", false},
      // por la tarde -> ἑσπέρας (παρὰ τῇ ἑσπέρᾳ was produced)
      {"Mi padre vuelve a casa por la tarde.", "ὁ πατήρ μου ἑσπέρας οἴκαδε ἐπανέρχεται.", false},
      {"Por la tarde jugamos en el jardín.", "ἑσπέρας ἐν τῷ κήπῳ παίζομεν.", false},
  };
  const int okEs = run8(*e, es, true, "C33 after blind ES");
  CHECK(okEs == (int)es.size());
}
