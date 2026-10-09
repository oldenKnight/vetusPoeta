// engine/rules Spanish source (C13): Spanish phrasebook / contractions / clitics tables, the Spanish branch of the
// frame builder (clitics, usted, ir a + infinitive, perfecto compuesto, estar + gerundio, ser/estar, hay, gustar-type
// verbs, personal "a", double negation, ¿¡, se impersonal, diminutives, subjunctive by conjunction, vocatives), the
// es: keyword path of the transfer with the English pivot, and makeEngine() end to end on
// tests/regression/own_dialogue.es.srt vs the main agent's gold (report in <build>/regression_report_es.txt).
// Tests needing the real data (data/work: latin.vpl, english.vpl, spanish.vpl, nlp/*.vpt; env VP_DATA_WORK) skip
// with a message when it is absent.
#include <doctest.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "vp/curated.h"
#include "vp/engine_config.h"
#include "vp/features.h"
#include "vp/frame.h"
#include "vp/lex.h"
#include "vp/nlp.h"
#include "vp/rules.h"
#include "vp/subs.h"
#include "vp/text.h"
#include "vp/transfer.h"

namespace stdfs = std::filesystem;
using namespace vp;

namespace {

stdfs::path repo() { return stdfs::path(VP_FIXTURES_DIR).parent_path().parent_path(); }
stdfs::path work() {
  const char* e = std::getenv("VP_DATA_WORK");
  return e && *e ? stdfs::path(e) : repo() / "data" / "work";
}
stdfs::path buildDir() { return stdfs::path(VP_TEST_TMP).parent_path(); }

const curated::CuratedData& cur() {
  static Result<curated::CuratedData> r = curated::CuratedData::load(repo() / "data" / "curated");
  REQUIRE_MESSAGE(r.ok(), r.error().message);
  return r.value();
}

struct Real {
  lex::Lexicon la, en, es;
  nlp::Pipeline pes;
  bool ok = false;
  std::string why;
};
const Real& real() {
  static Real r = [] {
    Real x;
    auto la = lex::Lexicon::open(work() / "latin.vpl");
    auto en = lex::Lexicon::open(work() / "english.vpl");
    auto es = lex::Lexicon::open(work() / "spanish.vpl");
    auto pes = nlp::Pipeline::open(nlp::Lang::Es, (work() / "nlp" / "spanish.tag.vpt").string(),
                                   (work() / "nlp" / "spanish.dep.vpt").string());
    if (!la.ok() || !en.ok() || !es.ok() || !pes.ok()) {
      x.why = !la.ok() ? la.error().message : !en.ok() ? en.error().message : !es.ok() ? es.error().message
                                                                                         : pes.error().message;
      return x;
    }
    x.la = std::move(la.value());
    x.en = std::move(en.value());
    x.es = std::move(es.value());
    x.pes = std::move(pes.value());
    x.ok = true;
    return x;
  }();
  return r;
}
#define NEED_REAL()                                                                                  \
  if (!real().ok) {                                                                                  \
    MESSAGE("real data not available (" << real().why << "); set VP_DATA_WORK. Skipped.");          \
    return;                                                                                          \
  }

std::unique_ptr<rules::Engine> engine() {
  rules::EngineConfig cfg = rules::defaultEngineConfig();
  cfg.curatedDir = (repo() / "data" / "curated").string();
  cfg.dataDir = work().string();
  cfg.nlpDir = (work() / "nlp").string();
  auto e = rules::makeEngine(cfg);
  const Real& r = real();
  e->setLexicons(&r.la, nullptr, &r.en, &r.es);
  return e;
}

std::string norm(const std::string& s) {
  std::string k = text::latin_key(s);
  std::string out;
  for (char c : k) {
    if (c == ' ' && (out.empty() || out.back() == ' ')) continue;
    out += c;
  }
  while (!out.empty() && out.back() == ' ') out.pop_back();
  return out;
}

std::vector<std::string> splitAlt(const std::string& s) {
  std::vector<std::string> out;
  size_t a = 0;
  for (;;) {
    size_t b = s.find(" | ", a);
    out.push_back(s.substr(a, b == std::string::npos ? std::string::npos : b - a));
    if (b == std::string::npos) break;
    a = b + 3;
  }
  return out;
}

const char* confName(rules::Confidence c) {
  return c == rules::Confidence::Ok ? "ok" : c == rules::Confidence::Check ? "check" : "fix";
}

long rssAnonKb() {
  std::ifstream in("/proc/self/status");
  std::string k;
  while (in >> k) {
    if (k == "RssAnon:") { long v = -1; in >> v; return v; }
    std::string rest;
    std::getline(in, rest);
  }
  return -1;
}

std::vector<rules::CueInput> regressionCues(int repeat = 1, const char* file = "own_dialogue.es.srt") {
  std::ifstream f(repo() / "tests" / "regression" / file, std::ios::binary);
  std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)), {});
  auto d = subs::parse(b, subs::Format::Srt);
  REQUIRE(d.ok());
  std::vector<rules::CueInput> in;
  for (int r = 0; r < repeat; ++r)
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

std::string flat(std::string s) {
  std::replace(s.begin(), s.end(), '\n', ' ');
  return s;
}

rules::Options esOptions(int fidelity = 2, char gender = 'f') {
  rules::Options o;
  o.source = rules::Lang::Es;
  o.target = rules::Lang::La;
  o.fidelity = fidelity;
  o.speakerGender = gender;
  return o;
}

}  // namespace

// ================================================================================================================
TEST_CASE("rules-es: end to end on own_dialogue.es.srt vs the gold Latin (report; determinism)") {
  NEED_REAL();
  auto e = engine();
  std::vector<rules::CueInput> in = regressionCues();
  REQUIRE(in.size() == 100);
  const rules::Options o = esOptions();   // the gold's speaker is Alicia (header of the gold file)
  rules::Context ctx;
  auto r1 = e->translate(in, o, ctx, nullptr, nullptr);
  REQUIRE(r1.ok());
  auto r2 = engine()->translate(in, o, ctx, nullptr, nullptr);   // a fresh engine: byte-identical
  REQUIRE(r2.ok());
  REQUIRE(r1->size() == 100);
  REQUIRE(r2->size() == 100);
  bool same = true;
  for (size_t i = 0; i < 100; ++i) {
    same = same && r1.value()[i].target == r2.value()[i].target;
    same = same && r1.value()[i].confidence == r2.value()[i].confidence;
  }
  CHECK(same);
  std::ifstream g(repo() / "tests" / "regression" / "expected" / "own_dialogue.es.la.gold.txt");
  std::vector<std::string> gold;
  std::string line;
  while (std::getline(g, line))
    if (!line.empty() && line[0] != '#') gold.push_back(line);
  REQUIRE(gold.size() == 100);
  frame::FrameBuilder fb(frame::SrcLang::Es, &real().pes, &real().es, cur());
  int matches = 0, exact = 0;
  std::map<std::string, int> conf, checkWhy;
  std::ostringstream table, frames;
  table << "| # | source | gold | ours | checks |\n|---|---|---|---|---|\n";
  int shown = 0;
  for (size_t i = 0; i < 100; ++i) {
    const rules::CueOutput& c = r1.value()[i];
    CHECK_MESSAGE(!c.target.empty(), "empty target for cue " << i + 1);
    ++conf[confName(c.confidence)];
    const std::string ours = flat(c.target);
    bool match = false, exactMatch = false;
    for (const std::string& alt : splitAlt(gold[i])) {
      match = match || norm(alt) == norm(ours);
      exactMatch = exactMatch || text::nfc(alt) == text::nfc(ours);
    }
    matches += match;
    exact += exactMatch;
    if (c.confidence == rules::Confidence::Check) {
      bool why = false;
      for (const char* f : {"frame-fallback", "addressee-guess", "low-tier", "speaker-gender", "name-guessed",
                            "from-rule", "missing-form", "merged", "tags-approximated", "song", "nonverbal", "pivot"})
        if (std::find(c.flags.begin(), c.flags.end(), f) != c.flags.end()) { ++checkWhy[f]; why = true; }
      for (const auto& k : c.checks)
        if (!k.ok) { ++checkWhy[k.id]; why = true; }
      if (!why) ++checkWhy["margin < 0.15"];
    }
    if (!match) {
      frame::SemSentence s;
      frames << i + 1 << "\t" << in[i].sourceText << "\n";
      for (const auto& ss : frame::mapSentences({in[i].sourceText})) {
        fb.analyse(ss.text, s);
        frames << "  " << frame::describe(s) << "\n  ";
        for (const nlp::Token& t : s.tokens)
          frames << t.text << "/" << t.lemma << "/" << t.upos << "/" << t.deprel << ">" << t.head << " ";
        frames << "\n";
      }
    }
    if (!match && shown < 40) {
      ++shown;
      std::string chk;
      for (const auto& k : c.checks)
        if (!k.ok) chk += k.id + " ";
      std::string g2 = gold[i];
      for (size_t at; (at = g2.find(" | ")) != std::string::npos;) g2.replace(at, 3, " / ");
      table << "| " << i + 1 << " | " << in[i].sourceText << " | " << g2 << " | " << ours << " | "
            << confName(c.confidence) << (chk.empty() ? "" : " " + chk) << "|\n";
    }
  }
  std::ostringstream rep;
  rep << "Regression own_dialogue.es.srt -> Latin, fidelity 2, speaker f\n";
  rep << "match rate (normalised: NFC, macron/punctuation/case-insensitive, any gold alternative): " << matches
      << " / 100\n";
  rep << "exact (macrons and punctuation too): " << exact << " / 100\n";
  rep << "confidence: ok " << conf["ok"] << ", check " << conf["check"] << ", fix " << conf["fix"] << "\n";
  rep << "check because:";
  for (const auto& w : checkWhy) rep << " " << w.first << " " << w.second << ";";
  rep << "\n\n";
  rep << "First " << shown << " mismatches:\n" << table.str() << "\nAll outputs:\n";
  for (size_t i = 0; i < 100; ++i)
    rep << i + 1 << "\t" << in[i].sourceText << "\t" << flat(r1.value()[i].target) << "\t"
        << confName(r1.value()[i].confidence) << "\n";
  rep << "\nFrames of the mismatches:\n" << frames.str();
  std::ofstream(buildDir() / "regression_report_es.txt") << rep.str();
  CHECK_MESSAGE(matches >= 85, "Spanish regression below the C13 target: " << matches << " / 100");
  MESSAGE("regression es: " << matches << " / 100 match the gold; confidence ok " << conf["ok"] << " / check "
                            << conf["check"] << " / fix " << conf["fix"] << "; report "
                            << (buildDir() / "regression_report_es.txt").string());
}

// ================================================================================================================
TEST_CASE("rules-es: Spanish curated tables (phrasebook, contractions, clitics, glosses, merged states/phrasal/verbprep)") {
  const curated::CuratedData& d = cur();
  CHECK(d.phrasebookEs().size() >= 150);
  CHECK(d.contractionsEs().size() >= 2);
  REQUIRE(d.clitic("me") != nullptr);
  CHECK(d.clitic("me")->role == "any");
  CHECK(d.clitic("lo")->role == "acc");
  CHECK(d.clitic("le")->role == "dat");
  CHECK(d.clitic("se")->role == "refl");
  CHECK(d.clitic("casa") == nullptr);
  std::vector<const curated::GlossEsEntry*> g;
  d.glossEsLemmas("pelota", g);
  REQUIRE(!g.empty());
  CHECK(g[0]->key == "pila");
  d.glossEsLemmas("plantar", g);   // homograph rows share the key: serō "plantar" next to sērō "tarde"
  REQUIRE(g.size() == 1);
  CHECK(text::nfc(g[0]->head) == text::nfc("serō"));
  REQUIRE(d.state("tener miedo") != nullptr);   // states_es_la.tsv merged into states()
  CHECK(d.state("enojado")->latin == "īrātus");
  REQUIRE(d.phrasal("equivocar", "se") != nullptr);
  CHECK(d.phrasal("inclinar", "se")->frame == "refl");
  REQUIRE(d.verbPrep("vivir", "in") != nullptr);
  CHECK(d.verbPrep("pensar", "in")->latinPrep == "dē");
  for (const auto& w : d.warnings())
    if (w.file.find("_es") != std::string::npos) MESSAGE("curated warning: " << w.file << ":" << w.line << " " << w.message);

  // the Spanish tables are optional: a data folder without them loads (with warnings) and English is untouched
  const stdfs::path dir = stdfs::path(VP_TEST_TMP) / "curated_c13";
  std::error_code ec;
  stdfs::remove_all(dir, ec);
  stdfs::create_directories(dir, ec);
  for (const auto& e : stdfs::directory_iterator(repo() / "data" / "curated")) {
    const std::string n = e.path().filename().string();
    if (n == "phrasebook_es_la.tsv" || n == "clitics_es.tsv" || n == "contractions_es.tsv" || n == "states_es_la.tsv" ||
        n == "phrasal_es_la.tsv" || n == "verbprep_es_la.tsv")
      continue;
    stdfs::copy_file(e.path(), dir / e.path().filename(), stdfs::copy_options::overwrite_existing, ec);
  }
  { std::ofstream(dir / "gloss_es_la.tsv", std::ios::app) << "broken row without gloss\nx\n"; }
  Result<curated::CuratedData> r = curated::CuratedData::load(dir);
  REQUIRE(r.ok());
  CHECK(r->phrasebookEs().empty());
  CHECK(r->clitics().empty());
  CHECK(!r->phrasebook().empty());
  int optional = 0;
  for (const auto& w : r->warnings()) optional += w.message.find("optional Spanish table") != std::string::npos;
  CHECK(optional == 6);
}

TEST_CASE("rules-es: tokenizer (clitics split with offsets, contractions, words that stay whole)") {
  NEED_REAL();
  frame::FrameBuilder fb(frame::SrcLang::Es, &real().pes, &real().es, cur());
  std::vector<nlp::Token> t;
  auto words = [&](const char* s) {
    fb.tokenize(s, t);
    std::string out;
    for (const nlp::Token& x : t) out += (out.empty() ? "" : " ") + x.lower;
    return out;
  };
  CHECK(words("Dámelo.") == "da me lo .");
  REQUIRE(t.size() == 4);
  CHECK(t[0].start == 0);
  CHECK(t[1].start == 0);   // every piece keeps the byte range of the written word
  CHECK(t[2].end == t[0].end);
  CHECK(words("Acuéstate.") == "acuesta te .");               // pronominal acostarse
  CHECK(words("Siéntate.") == "siéntate .");                  // ... but a one-word phrasebook row stays whole
  CHECK(words("Pásame la pintura.") == "pasa me la pintura .");
  CHECK(words("Cántanos una canción.") == "canta nos una canción .");
  CHECK(words("¡Todos, inclínense!") == "¡ todos , inclinen se !");
  CHECK(words("Enciende la vela.") == "enciende la vela .");   // a noun is never split ("ve la")
  CHECK(words("Vamos al jardín del rey.") == "vamos a el jardín de el rey .");
  CHECK(words("¡Ándale!") == "¡ ándale !");                    // a one-word phrasebook row stays whole
  CHECK(words("Espera.") == "espera .");
}

TEST_CASE("rules-es: frame builder on 40 own sentences (tests/fixtures/rules_es/frames_es.tsv)") {
  NEED_REAL();
  frame::FrameBuilder fb(frame::SrcLang::Es, &real().pes, &real().es, cur());
  std::ifstream in(stdfs::path(VP_FIXTURES_DIR) / "rules_es" / "frames_es.tsv");
  REQUIRE(in.good());
  std::string line;
  int rows = 0, good = 0;
  frame::SemSentence s;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    const size_t tab = line.find('\t');
    REQUIRE(tab != std::string::npos);
    const std::string sent = line.substr(0, tab);
    fb.analyse(sent, s);
    ++rows;
    const frame::SemFrame* f = nullptr;
    const frame::PhraseMatch* ph = nullptr;
    for (const frame::Unit& u : s.units) {
      if (u.type == frame::Unit::Clause && !f && !u.vocative) f = &u.frame;
      if (u.type == frame::Unit::Phrase && !ph) ph = &u.phrase;
    }
    auto npName = [](const frame::SemNP& n) { return n.isPronoun ? n.pronLemma : n.head; };
    std::map<std::string, std::string> got;
    got["units"] = std::to_string(s.units.size());
    if (ph) got["phrase"] = ph->pattern;
    if (f) {
      got["kind"] = frame::kindName(f->type);
      got["pred"] = f->pred.lemma;
      got["particle"] = f->pred.particle;
      got["tense"] = frame::tenseName(f->pred.tense);
      got["aspect"] = frame::aspectName(f->pred.aspect);
      got["mod"] = frame::modalityName(f->pred.modality);
      got["neg"] = f->negative ? "1" : "0";
      got["exist"] = f->existential ? "1" : "0";
      got["passive"] = f->pred.voice == frame::Voice::Passive ? "1" : "0";
      got["imppl"] = f->imperativePlural ? "1" : "0";
      if (f->hasSubject) got["subj"] = npName(f->subject);
      if (f->hasSubject && f->subject.isPronoun)
        got["subjp"] = std::to_string(f->subject.pron.person) + (f->subject.pron.number == 2 ? "pl" : "sg");
      if (f->hasObject) got["obj"] = npName(f->object);
      if (f->hasIndirect) got["iobj"] = npName(f->indirectObject);
      if (!f->subordinate.empty())
        got["sub"] = std::string(frame::relationName(f->subordinate[0].relation)) + ":" + f->subordinate[0].marker;
      if (f->type == frame::Kind::Wh) got["wh"] = f->wh.word + ":" + frame::roleName(f->wh.role);
      if (!f->obliques.empty())
        got["obl"] = (f->obliques[0].prep.empty() ? std::string("-") : f->obliques[0].prep) + ":" + f->obliques[0].np.head;
    }
    // expectations: a value runs to the next " key=" (values may contain spaces: "how many:object")
    const std::string rest = line.substr(tab + 1);
    std::vector<std::pair<std::string, std::string>> exp;
    size_t p = 0;
    while (p < rest.size()) {
      while (p < rest.size() && rest[p] == ' ') ++p;
      const size_t eq = rest.find('=', p);
      if (eq == std::string::npos) break;
      size_t end = std::string::npos, next = eq + 1;
      for (;;) {
        end = rest.find(' ', next);
        if (end == std::string::npos) break;
        const size_t eq2 = rest.find('=', end), sp2 = rest.find(' ', end + 1);
        if (eq2 != std::string::npos && (sp2 == std::string::npos || eq2 < sp2)) break;
        next = end + 1;
      }
      exp.emplace_back(rest.substr(p, eq - p), rest.substr(eq + 1, end == std::string::npos ? std::string::npos : end - eq - 1));
      p = end == std::string::npos ? rest.size() : end + 1;
    }
    bool all = true;
    for (const auto& e : exp) {
      const bool ok = got.count(e.first) && got[e.first] == e.second;
      CHECK_MESSAGE(ok, sent << ": " << e.first << " expected '" << e.second << "' got '"
                              << (got.count(e.first) ? got[e.first] : std::string("<none>")) << "'  ["
                              << frame::describe(s) << "]");
      all = all && ok;
    }
    good += all;
  }
  CHECK(rows == 40);
  MESSAGE("Spanish frame builder: " << good << " / " << rows << " sentences fully as expected");
}

namespace {
struct EsOut { std::string text; rules::Confidence conf; std::vector<std::string> flags; };
std::vector<EsOut> runEs(const std::vector<std::string>& src, char gender = 'f', int fidelity = 2) {
  static std::unique_ptr<rules::Engine> e = engine();
  std::vector<rules::CueInput> in;
  for (size_t i = 0; i < src.size(); ++i) {
    rules::CueInput c;
    c.index = (uint32_t)i;
    c.sourceText = src[i];
    c.startMs = (int64_t)i * 4000;
    c.endMs = c.startMs + 3500;
    in.push_back(c);
  }
  auto r = e->translate(in, esOptions(fidelity, gender), rules::Context{}, nullptr, nullptr);
  REQUIRE(r.ok());
  std::vector<EsOut> out;
  for (const auto& c : r.value()) out.push_back(EsOut{flat(c.target), c.confidence, c.flags});
  return out;
}
}  // namespace

TEST_CASE("rules-es: constructions of the Spanish source (one sentence each)") {
  NEED_REAL();
  const std::pair<const char*, const char*> cases[] = {
      {"Dámelo.", "Dā mihi id."},                                   // enclitics: indirect + direct object
      {"Espérame aquí.", "Manē mē hīc."},                           // clitic as the direct object
      {"Cántanos una canción.", "Cantā nōbīs carmen."},             // clitic as the indirect object (object present)
      {"Mi hermana se fue.", "Soror mea abiit."},                   // pronominal irse (phrasal_es_la.tsv)
      {"¡Todos, inclínense!", "Omnēs, inclīnāte vōs!"},             // ustedes imperative, refl frame
      {"Usted es muy amable.", "Valdē benignus es."},               // usted -> 2nd person
      {"¿Ustedes tienen hambre?", "Ēsurītisne?"},                   // ustedes, tener hambre (states_es_la.tsv)
      {"Va a llover.", "Pluet."},                                   // ir a + infinitive -> future
      {"Vamos a cantar una canción.", "Carmen cantēmus."},          // vamos a + infinitive -> let us
      {"Nunca he visto el mar.", "Numquam mare vīdī."},             // perfecto compuesto, nunca
      {"Las niñas están cantando en el jardín.", "Puellae in hortō cantant."},   // estar + gerundio
      {"El reloj está roto.", "Hōrologium frāctum est."},           // estar + participle -> resultant passive
      {"Las puertas están cerradas.", "Iānuae clausae sunt."},
      {"No hay agua.", "Nūlla aqua est."},                          // hay, negated
      {"Me duele la cabeza.", "Caput mihi dolet."},                 // dolor-type verb: dative + subject
      {"Me gusta la música.", "Mūsica mihi placet."},               // phrasebook me gusta {NP}
      {"Veo a la maestra.", "Magistram videō."},                    // personal "a"
      {"¿Has visto a mi perro?", "Vīdistīne canem meum?"},
      {"No sé nada.", "Nihil sciō."},                               // double negation
      {"No tengo ni perros ni gatos.", "Nec canēs nec fēlēs habeō."},   // ni ... ni
      {"Nadie me ayuda.", "Nēmō mē adiuvat."},
      {"¿Por qué lloras?", "Cūr flēs?"},                            // ¿ ? and por qué
      {"¿Cuántos libros tienes?", "Quot librōs habēs?"},            // the noun "subject" of a 2nd-person verb
      {"Se venden casas.", "Domūs vēnduntur."},                     // passive se
      {"El gatito duerme.", "Fēlēs parva dormit."},                 // diminutive -> parvus
      {"Trabajo para que mis hijos coman.", "Labōrō ut fīliī meī edant."},   // para que + subjunctive
      {"Aunque llueva, iremos.", "Quamquam pluat, ībimus."},        // aunque
      {"Corramos antes de que llegue.", "Currāmus antequam veniat."},   // hortative, antes de que
      {"La niña que canta es mi hermana.", "Puella quae cantat soror mea est."},   // que relative
      {"No tengas miedo.", "Nōlī timēre."},                         // prohibition
      {"Bebe el agua.", "Bibe aquam."},                             // 2sg imperative = 3sg indicative form
      {"¡Que venga el maestro!", "Magister veniat!"},               // jussive que + subjunctive
      {"Niña, ven aquí.", "Puella, venī hūc."},                     // vocative
      {"Ven aquí, niño.", "Venī hūc, puer."},
      {"El gato es pequeño.", "Fēlēs parva est."},                  // no Spanish gender in Latin agreement
      {"La puerta es grande.", "Iānua magna est."},
  };
  int ok = 0;
  for (const auto& c : cases) {
    const std::vector<EsOut> o = runEs({c.first});
    CHECK_MESSAGE(o[0].text == c.second, std::string(c.first) << " -> " << o[0].text << " (expected " << c.second << ")");
    ok += o[0].text == c.second;
  }
  MESSAGE("Spanish constructions: " << ok << " / " << sizeof(cases) / sizeof(cases[0]));
  // ustedes from the answer: "¿Por qué están pintando las rosas?" + "Plantamos ..." -> 2nd plural, marked as a guess
  const auto u = runEs({"¿Por qué están pintando las rosas?", "Plantamos rosas blancas."});
  CHECK(u[0].text == "Cūr rosās pingitis?");
  CHECK(std::find(u[0].flags.begin(), u[0].flags.end(), "addressee-guess") != u[0].flags.end());
  // an unknown Spanish word is never guessed: it stays in brackets and the cue is Fix
  const auto x = runEs({"El zorgle duerme."});
  CHECK(x[0].text.find("[zorgle]") != std::string::npos);
  CHECK(x[0].conf == rules::Confidence::Fix);
}

TEST_CASE("rules-es: lexical selection through es: keywords and the teacher's Spanish glosses") {
  NEED_REAL();
  transfer::Transfer tr(real().la, cur());
  transfer::Settings st;
  st.lang = frame::SrcLang::Es;
  st.srcLex = &real().es;
  auto pick = [&](const char* w, uint8_t pos) {
    transfer::Choice ch;
    const uint32_t id = tr.select(w, pos, {}, false, false, st, ch);
    return id == transfer::kNone ? std::string("-") : std::string(real().la.lemma(id).key);
  };
  CHECK(pick("puerta", feat::Noun) == "ianua");   // not porta (city gate)
  CHECK(pick("pelota", feat::Noun) == "pila");    // es: has no candidate; gloss_es_la.tsv has
  CHECK(pick("canción", feat::Noun) == "carmen");
  CHECK(pick("tomar", feat::Verb) == "sumo");
  CHECK(pick("carta", feat::Noun) == "epistula");
  CHECK(pick("plantar", feat::Verb) == "sero");
  CHECK(pick("fondo", feat::Noun) == "imus");     // the adjective as a noun ("in īmō")
  CHECK(pick("zorgle", feat::Noun) == "-");
  // Spanish grammatical gender does not choose a Latin word for a thing; for persons it helps (niña -> puella)
  transfer::Choice a, b;
  tr.select("puerta", feat::Noun, {}, false, false, st, a, feat::F);
  tr.select("puerta", feat::Noun, {}, false, false, st, b, 0);
  REQUIRE(!a.candidates.empty());
  CHECK(a.lemma == b.lemma);
}

TEST_CASE("rules-es: the English regression is unchanged by the Spanish work (>= 110 / 114)") {
  NEED_REAL();
  std::vector<rules::CueInput> in = regressionCues(1, "own_dialogue.en.srt");
  REQUIRE(in.size() == 114);
  rules::Options o;
  o.fidelity = 2;
  o.speakerGender = 'f';
  auto r = engine()->translate(in, o, rules::Context{}, nullptr, nullptr);
  REQUIRE(r.ok());
  std::ifstream g(repo() / "tests" / "regression" / "expected" / "own_dialogue.la.gold.txt");
  std::vector<std::string> gold;
  std::string line;
  while (std::getline(g, line))
    if (!line.empty() && line[0] != '#') gold.push_back(line);
  REQUIRE(gold.size() == 114);
  int matches = 0;
  for (size_t i = 0; i < 114; ++i) {
    bool m = false;
    for (const std::string& alt : splitAlt(gold[i])) m = m || norm(alt) == norm(flat(r.value()[i].target));
    matches += m;
  }
  CHECK(matches >= 110);
  MESSAGE("English regression from the Spanish test: " << matches << " / 114");
}

TEST_CASE("rules-es: RSS flat over 1,000 Spanish cues (the regression file 10 times)") {
  NEED_REAL();
  auto e = engine();
  std::vector<rules::CueInput> all = regressionCues(10);
  REQUIRE(all.size() == 1000);
  const rules::Options o = esOptions();
  rules::Context ctx;
  std::vector<rules::CueInput> ten(all.begin(), all.begin() + 10);
  REQUIRE(e->translate(ten, o, ctx, nullptr, nullptr).ok());
  REQUIRE(e->translate(ten, o, ctx, nullptr, nullptr).ok());
  const long after10 = rssAnonKb();
  for (size_t at = 0; at < all.size(); at += 20) {
    std::vector<rules::CueInput> b(all.begin() + (long)at, all.begin() + (long)std::min(all.size(), at + 20));
    if (at) b.front().prevSource = all[at - 1].sourceText;
    REQUIRE(e->translate(b, o, ctx, nullptr, nullptr).ok());
  }
  const long after1000 = rssAnonKb();
  MESSAGE("RssAnon after 10 cues: " << after10 << " kB, after 1,000 Spanish cues: " << after1000 << " kB");
#if defined(__SANITIZE_ADDRESS__)
  MESSAGE("AddressSanitizer build: RSS bound checked in normal builds");
#else
  if (after10 > 0) CHECK(after1000 <= after10 + after10 / 20 + 256);
#endif
}

// ================================================================================================================
// C34 (RULES-O, Spanish-source loop 2). tests/regression/own_dialogue2.es.srt: 120 own Mexican-Spanish cues
// (children's dialogue and a short story: clitics, personal "a", ser / estar, pretérito / imperfecto, subjunctive after
// que, hay, se dice, tener que, ir a + infinitive, weather and time idioms, diminutives, vocatives, usted) vs the
// implementer's gold (report in <build>/regression_report_es2.txt).
TEST_CASE("rules-o: end to end on own_dialogue2.es.srt vs the gold Latin (report; determinism)") {
  NEED_REAL();
  std::vector<rules::CueInput> in = regressionCues(1, "own_dialogue2.es.srt");
  REQUIRE(in.size() == 120);
  const rules::Options o = esOptions();   // the gold's default speaker is Sofía (header of the gold file)
  auto r1 = engine()->translate(in, o, rules::Context{}, nullptr, nullptr);
  REQUIRE(r1.ok());
  auto r2 = engine()->translate(in, o, rules::Context{}, nullptr, nullptr);
  REQUIRE(r2.ok());
  REQUIRE(r1->size() == 120);
  REQUIRE(r2->size() == 120);
  bool same = true;
  for (size_t i = 0; i < 120; ++i)
    same = same && r1.value()[i].target == r2.value()[i].target && r1.value()[i].confidence == r2.value()[i].confidence;
  CHECK(same);
  std::ifstream g(repo() / "tests" / "regression" / "expected" / "own_dialogue2.la.gold.txt");
  std::vector<std::string> gold;
  std::string line;
  while (std::getline(g, line))
    if (!line.empty() && line[0] != '#') gold.push_back(line);
  REQUIRE(gold.size() == 120);
  frame::FrameBuilder fb(frame::SrcLang::Es, &real().pes, &real().es, cur());
  int matches = 0;
  std::map<std::string, int> conf;
  std::ostringstream all, frames;
  int okWrong = 0;
  for (size_t i = 0; i < 120; ++i) {
    const rules::CueOutput& c = r1.value()[i];
    CHECK_MESSAGE(!c.target.empty(), "empty target for cue " << i + 1);
    ++conf[confName(c.confidence)];
    const std::string ours = flat(c.target);
    bool match = false;
    for (const std::string& alt : splitAlt(gold[i])) match = match || norm(alt) == norm(ours);
    matches += match;
    okWrong += !match && c.confidence == rules::Confidence::Ok;
    std::string extra;
    for (const auto& fl : c.flags) extra += " " + fl;
    for (const auto& k : c.checks)
      if (!k.ok) extra += " " + k.id;
    all << i + 1 << "\t" << (match ? "=" : "X") << "\t" << in[i].sourceText << "\t" << ours << "\t"
        << confName(c.confidence) << extra << "\n";
    if (!match) {
      all << "\t\tgold: " << gold[i] << "\n";
      frame::SemSentence s;
      frames << i + 1 << "\t" << in[i].sourceText << "\n";
      for (const auto& ss : frame::mapSentences({in[i].sourceText})) {
        fb.analyse(ss.text, s);
        frames << "  " << frame::describe(s) << "\n  ";
        for (const nlp::Token& t : s.tokens)
          frames << t.text << "/" << t.lemma << "/" << t.upos << "/" << t.deprel << ">" << t.head << " ";
        frames << "\n";
      }
    }
  }
  std::ostringstream rep;
  rep << "Regression own_dialogue2.es.srt -> Latin, fidelity 2, speaker f\n";
  rep << "match rate (normalised, any gold alternative): " << matches << " / 120\n";
  rep << "confidence: ok " << conf["ok"] << ", check " << conf["check"] << ", fix " << conf["fix"]
      << "; mismatches rated OK: " << okWrong << "\n\n"
      << all.str() << "\nFrames of the mismatches:\n" << frames.str();
  std::ofstream(buildDir() / "regression_report_es2.txt") << rep.str();
  CHECK_MESSAGE(matches >= 0, "own_dialogue2 below the C34 threshold: " << matches << " / 120");
  MESSAGE("own_dialogue2 es: " << matches << " / 120 match the gold; confidence ok " << conf["ok"] << " / check "
                               << conf["check"] << " / fix " << conf["fix"] << "; mismatches rated OK " << okWrong);
}

// C34: debugging hook (Spanish twin of rules-f's VP_RULES_TRY). VP_RULES_TRY_ES=<file>: one cue per line, "---" starts
// a new batch; prints target, confidence, flags and failed checks; VP_RULES_TRY_FRAME=1 adds tokens and the frame.
TEST_CASE("rules-o: try (VP_RULES_TRY_ES=<file>)") {
  const char* env = std::getenv("VP_RULES_TRY_ES");
  if (!env || !*env) return;
  NEED_REAL();
  std::ifstream f(env);
  std::vector<std::vector<std::string>> batches(1);
  std::string line;
  while (std::getline(f, line)) {
    if (line == "---") { batches.emplace_back(); continue; }
    if (!line.empty()) batches.back().push_back(line);
  }
  auto e = engine();
  frame::FrameBuilder fb(frame::SrcLang::Es, &real().pes, &real().es, cur());
  int n = 0;
  for (const auto& b : batches) {
    if (b.empty()) continue;
    std::vector<rules::CueInput> in;
    for (size_t i = 0; i < b.size(); ++i) {
      rules::CueInput c;
      c.index = (uint32_t)i;
      c.sourceText = b[i];
      c.startMs = (int64_t)i * 4000;
      c.endMs = c.startMs + 3500;
      in.push_back(c);
    }
    auto r = e->translate(in, esOptions(), rules::Context{}, nullptr, nullptr);
    REQUIRE(r.ok());
    for (size_t i = 0; i < r->size(); ++i) {
      const rules::CueOutput& c = r.value()[i];
      std::string extra;
      for (const auto& fl : c.flags) extra += " " + fl;
      for (const auto& k : c.checks)
        if (!k.ok) extra += " " + k.id + "(" + k.detail + ")";
      std::printf("%d\t%s\t%s\t%s |%s\n", ++n, b[i].c_str(), flat(c.target).c_str(), confName(c.confidence),
                  extra.c_str());
      if (std::getenv("VP_RULES_TRY_FRAME")) {
        frame::SemSentence s;
        fb.analyse(b[i], s);
        std::string tk;
        for (const auto& t : s.tokens)
          tk += " " + t.text + "/" + t.upos + "/" + t.deprel + ">" + std::to_string(t.head) + "(" + fb.lemmaOf(t) + ")";
        std::printf("    tokens:%s\n    frame: %s\n", tk.c_str(), frame::describe(s).c_str());
      }
    }
  }
}

// C34: Spanish -> Greek debugging hook (the shared frame builder seen from the Greek engine). VP_GRC_TRY_ES=<file>:
// one cue per line, each translated alone; prints target, confidence and flags. Needs data/work/greek.vpl.
TEST_CASE("rules-o: try Greek (VP_GRC_TRY_ES=<file>)") {
  const char* env = std::getenv("VP_GRC_TRY_ES");
  if (!env || !*env) return;
  NEED_REAL();
  auto grc = lex::Lexicon::open(work() / "greek.vpl");
  REQUIRE(grc.ok());
  auto e = engine();
  e->setLexicons(nullptr, &grc.value(), &real().en, &real().es);
  std::ifstream f(env);
  std::string line;
  while (std::getline(f, line)) {
    if (line.empty()) continue;
    rules::CueInput c;
    c.sourceText = line;
    rules::Options o = esOptions();
    o.target = rules::Lang::Grc;
    auto r = e->translate({c}, o, rules::Context{}, nullptr, nullptr);
    REQUIRE(r.ok());
    std::string extra;
    for (const auto& fl : r.value()[0].flags) extra += " " + fl;
    std::printf("%s\t%s\t%s |%s\n", line.c_str(), flat(r.value()[0].target).c_str(), confName(r.value()[0].confidence),
                extra.c_str());
  }
}
