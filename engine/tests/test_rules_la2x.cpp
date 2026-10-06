// engine/rules la2x (C11): Latin -> English / Spanish. Needs the real lexicon (data/work/latin.vpl or VP_DATA_WORK);
// every case skips with a message when it is absent.
#include <doctest.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "../rules/src/la2x/internal.h"
#include "vp/check.h"
#include "vp/curated.h"
#include "vp/engine_config.h"
#include "vp/features.h"
#include "vp/la2x.h"
#include "vp/lex.h"
#include "vp/rules.h"
#include "vp/text.h"

namespace stdfs = std::filesystem;
using namespace vp;

namespace {

stdfs::path repoDir() { return stdfs::path(VP_FIXTURES_DIR).parent_path().parent_path(); }
stdfs::path workDir() {
  const char* e = std::getenv("VP_DATA_WORK");
  return e && *e ? stdfs::path(e) : repoDir() / "data" / "work";
}

struct World {
  lex::Lexicon la;
  std::unique_ptr<curated::CuratedData> cd;
  std::unique_ptr<la2x::Translator> tr;
  bool ok = false;
  std::string why;
};
World& world() {
  static World w = [] {
    World x;
    auto la = lex::Lexicon::open(workDir() / "latin.vpl");
    if (!la.ok()) { x.why = la.error().message; return x; }
    x.la = std::move(la.value());
    auto cd = curated::CuratedData::load(repoDir() / "data" / "curated");
    if (!cd.ok()) { x.why = cd.error().message; return x; }
    x.cd = std::make_unique<curated::CuratedData>(std::move(cd.value()));
    auto tr = la2x::Translator::create(x.la, *x.cd, {repoDir() / "data" / "curated"});
    if (!tr.ok()) { x.why = tr.error().message; return x; }
    x.tr = std::move(tr.value());
    x.ok = true;
    return x;
  }();
  return w;
}
#define NEED_WORLD()                                                         \
  World& W = world();                                                        \
  if (!W.ok) {                                                               \
    MESSAGE("skipped: real Latin lexicon not available (" << W.why << ")");  \
    return;                                                                  \
  }

// Whitespace / punctuation normalisation for the readable comparison (case kept).
std::string norm(const std::string& s) {
  std::string out;
  size_t i = 0;
  while (i < s.size()) {
    size_t j = i;
    const char32_t c = text::decodeUtf8(s, j);
    const bool punct = c == '.' || c == ',' || c == '!' || c == '?' || c == ';' || c == ':' || c == U'\u00BF' ||
                       c == U'\u00A1' || c == '"' || c == '\'';
    const bool space = c == ' ' || c == '\t' || c == '\n';
    if (punct || space) {
      if (!out.empty() && out.back() != ' ') out += ' ';
    } else {
      out.append(s, i, j - i);
    }
    i = j;
  }
  while (!out.empty() && out.back() == ' ') out.pop_back();
  return out;
}

std::vector<std::vector<std::string>> readTsv(const stdfs::path& p) {
  std::vector<std::vector<std::string>> rows;
  std::ifstream in(p);
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::vector<std::string> c;
    size_t a = 0;
    for (;;) {
      const size_t b = line.find('\t', a);
      c.push_back(line.substr(a, b == std::string::npos ? std::string::npos : b - a));
      if (b == std::string::npos) break;
      a = b + 1;
    }
    rows.push_back(c);
  }
  return rows;
}

long rssAnonKbLa2x() {
  std::ifstream in("/proc/self/status");
  std::string k;
  long v = -1;
  while (in >> k) {
    if (k == "RssAnon:") { in >> v; return v; }
    std::string rest;
    std::getline(in, rest);
  }
  return v;
}

const la2x::Word* wordAt(const la2x::SentenceOut& o, const std::string& text) {
  for (const la2x::Word& w : o.words)
    if (w.text == text) return &w;
  return nullptr;
}

}  // namespace


TEST_CASE("rules-la2x: English and Spanish word morphology") {
  using namespace vp::la2x::detail;
  CHECK(en::verb("love", en::VForm::S3) == "loves");
  CHECK(en::verb("go", en::VForm::Past) == "went");
  CHECK(en::verb("stop", en::VForm::Ing) == "stopping");
  CHECK(en::verb("write", en::VForm::PastPart) == "written");
  CHECK(en::verb("look at", en::VForm::S3) == "looks at");
  CHECK(en::verb("cry", en::VForm::S3) == "cries");
  CHECK(en::verb("understand", en::VForm::Past) == "understood");
  CHECK(en::plural("woman") == "women");
  CHECK(en::plural("city") == "cities");
  CHECK(en::plural("old man") == "old men");
  CHECK(en::comparative("tall") == "taller");
  CHECK(en::comparative("beautiful") == "more beautiful");
  CHECK(en::comparative("happy") == "happier");
  CHECK(en::indefinite("apple") == "an");
  CHECK(en::indefinite("rose") == "a");
  CHECK(es::verb("amar", es::VTense::Present, 3, 1) == "ama");
  CHECK(es::verb("comer", es::VTense::Imperfect, 1, 2) == "comíamos");
  CHECK(es::verb("vivir", es::VTense::Preterite, 3, 2) == "vivieron");
  CHECK(es::verb("tener", es::VTense::Present, 1, 1) == "tengo");
  CHECK(es::verb("ser", es::VTense::Preterite, 3, 1) == "fue");
  CHECK(es::verb("ir", es::VTense::Future, 1, 2) == "iremos");
  CHECK(es::verb("hacer", es::VTense::Preterite, 3, 1) == "hizo");
  CHECK(es::verb("decir", es::VTense::Preterite, 3, 2) == "dijeron");
  CHECK(es::verb("dormir", es::VTense::Present, 3, 1) == "duerme");
  CHECK(es::verb("pedir", es::VTense::Preterite, 3, 1) == "pidió");
  CHECK(es::verb("buscar", es::VTense::Preterite, 1, 1) == "busqué");
  CHECK(es::verb("conocer", es::VTense::Present, 1, 1) == "conozco");
  CHECK(es::verb("leer", es::VTense::Preterite, 3, 1) == "leyó");
  CHECK(es::verb("venir", es::VTense::Imperative, 2, 1) == "ven");
  CHECK(es::verb("correr", es::VTense::Imperative, 2, 2) == "corran");
  CHECK(es::verb("temer", es::VTense::SubjPresent, 2, 1) == "temas");
  CHECK(es::verb("ver", es::VTense::SubjImperfect, 3, 1) == "viera");
  CHECK(es::verb("estar sentado", es::VTense::Present, 3, 2) == "están sentado");
  CHECK(es::verb("levantarse", es::VTense::Present, 3, 1) == "levanta");
  CHECK(es::verb("irse", es::VTense::Preterite, 3, 1) == "fue");
  CHECK(es::verb("escribir", es::VTense::PastPart, 3, 1) == "escrito");
  CHECK(es::plural("jardín") == "jardines");
  CHECK(es::plural("canción") == "canciones");
  CHECK(es::plural("voz") == "voces");
  CHECK(es::adjective("bonito", vp::feat::F, 2) == "bonitas");
  CHECK(es::adjective("feliz", vp::feat::F, 2) == "felices");
  CHECK(es::nounGender("rosa", 0) == vp::feat::F);
  CHECK(es::nounGender("día", 0) == vp::feat::M);
  CHECK(es::nounGender("ciudad", vp::feat::F) == vp::feat::F);
  CHECK(es::nounGender("bosque", vp::feat::F) == vp::feat::M);
  CHECK(es::withClitics("mira", "me") == "mírame");
  CHECK(es::withClitics("da", "me") == "dame");
  CHECK(es::withClitics("da", "melo") == "dámelo");
  CHECK(es::withClitics("escucha", "nos") == "escúchanos");
  CHECK(es::withClitics("ver", "la") == "verla");
}

TEST_CASE("rules-la2x: own sentences, readable English and Spanish") {
  NEED_WORLD();
  const auto rows = readTsv(repoDir() / "tests" / "fixtures" / "la2x" / "sentences.tsv");
  REQUIRE(rows.size() >= 80);
  int okEn = 0, okEs = 0;
  for (const auto& r : rows) {
    REQUIRE(r.size() >= 3);
    la2x::SentenceOut en, es;
    W.tr->resetDiscourse();
    W.tr->sentence(r[0], la2x::Target::En, en);
    W.tr->resetDiscourse();
    W.tr->sentence(r[0], la2x::Target::Es, es);
    const bool a = norm(en.text) == norm(r[1]);
    const bool b = norm(es.text) == norm(r[2]);
    okEn += a;
    okEs += b;
    CHECK_MESSAGE(a, r[0] << " -> " << en.text << " (expected " << r[1] << ")");
    CHECK_MESSAGE(b, r[0] << " -> " << es.text << " (expected " << r[2] << ")");
  }
  MESSAGE("own sentences: English " << okEn << "/" << rows.size() << ", Spanish " << okEs << "/" << rows.size());
}

TEST_CASE("rules-la2x: lemma and features per token") {
  NEED_WORLD();
  const auto rows = readTsv(repoDir() / "tests" / "fixtures" / "la2x" / "tokens.tsv");
  REQUIRE(rows.size() >= 150);
  std::string lastSentence;
  la2x::SentenceOut out;
  int ok = 0;
  for (const auto& r : rows) {
    REQUIRE(r.size() >= 4);
    if (r[0] != lastSentence) {
      W.tr->resetDiscourse();
      W.tr->sentence(r[0], la2x::Target::En, out);
      lastSentence = r[0];
    }
    const la2x::Word* w = wordAt(out, r[1]);
    REQUIRE_MESSAGE(w != nullptr, r[0] << ": no word " << r[1]);
    // optional 5th column: the English interlinear gloss (tells homograph lemmas with one head apart: volō want / fly)
    const bool glossOk = r.size() < 5 || r[4].empty() || w->gloss == r[4];
    const bool good = w->head == r[2] && w->featureText == r[3] && glossOk;
    ok += good;
    CHECK_MESSAGE(good, r[0] << ": " << r[1] << " = " << w->head << " [" << w->featureText << "] '" << w->gloss
                               << "', expected " << r[2] << " [" << r[3] << "]" << (r.size() >= 5 ? " '" + r[4] + "'" : ""));
  }
  MESSAGE("tokens: " << ok << "/" << rows.size() << " lemma + features exact");
}

TEST_CASE("rules-la2x: disambiguation by context") {
  NEED_WORLD();
  auto analyse = [&](const char* s) {
    la2x::SentenceOut o;
    W.tr->resetDiscourse();
    W.tr->sentence(s, la2x::Target::En, o);
    return o;
  };
  auto feat = [&](const la2x::SentenceOut& o, const char* w) {
    const la2x::Word* x = wordAt(o, w);
    return x ? x->featureText : std::string("<missing>");
  };
  auto head = [&](const la2x::SentenceOut& o, const char* w) {
    const la2x::Word* x = wordAt(o, w);
    return x ? x->head : std::string("<missing>");
  };
  // puella / puellā without macrons: the preposition decides
  CHECK(feat(analyse("Puella in via ambulat."), "Puella") == "noun, nominative singular");
  CHECK(feat(analyse("Marcus cum puella ambulat."), "puella") == "noun, ablative singular");
  CHECK(feat(analyse("Marcus in via ambulat."), "via") == "noun, ablative singular");
  // the verb's number decides the subject: puellae nominative plural / dative singular
  CHECK(feat(analyse("Puellae rosam amant."), "Puellae") == "noun, nominative plural");
  CHECK(feat(analyse("Puer puellae rosam dat."), "puellae") == "noun, dative singular");
  CHECK(feat(analyse("Puellae puer rosam dat."), "Puellae") == "noun, dative singular");
  // amat vs amant with their subjects; the present before the look-alike perfect
  CHECK(feat(analyse("Puella rosam amat."), "amat") == "verb, third person singular present indicative active");
  CHECK(feat(analyse("Puellae rosam amant."), "amant") == "verb, third person plural present indicative active");
  CHECK(feat(analyse("Puer venit."), "venit") == "verb, third person singular present indicative active");
  // relative pronouns: case from their own clause, gender/number from the antecedent
  CHECK(feat(analyse("Puer quem puella amat currit."), "quem") == "pronoun, accusative singular masculine");
  CHECK(feat(analyse("Puella quae cantat laeta est."), "quae") == "pronoun, nominative singular feminine");
  CHECK(feat(analyse("Rosae quas puella portat pulchrae sunt."), "quas") == "pronoun, accusative plural feminine");
  // sentence-final verb: est is sum, not edō; amor in subject position is the noun
  CHECK(head(analyse("Amor magnus est."), "Amor") == "amor");
  CHECK(head(analyse("Rosa pulchra est."), "est") == "sum");
  // cum: preposition with one verb, conjunction with two
  CHECK(feat(analyse("Cum amico ambulat."), "Cum") == "preposition");
  CHECK(feat(analyse("Cum puella venit, puer gaudet."), "Cum") == "conjunction");
  // quod: conjunction unless a neuter noun precedes
  CHECK(feat(analyse("Rideo quod laetus sum."), "quod") == "conjunction");
  // capitals: a core common noun at the start is not a name; a name mid-sentence is
  CHECK(head(analyse("Equus currit."), "Equus") == "equus");
  CHECK(feat(analyse("Puer Marcum videt."), "Marcum") == "name, accusative singular");
  // enclitics: -ne on a known form, -que coordination
  CHECK(head(analyse("Videsne puellam?"), "Videsne") == "videō");
  CHECK(analyse("Puellae puerique cantant.").text == "The girls and the boys sing.");
  // confidence: unambiguous words 1.0, ambiguous ones lower, alternatives listed
  const la2x::SentenceOut o = analyse("Puer puellae rosam dat.");
  CHECK(wordAt(o, "rosam")->confidence == doctest::Approx(1.0));
  CHECK(wordAt(o, "puellae")->confidence < 1.0);
  CHECK(!wordAt(o, "puellae")->alternatives.empty());
}

TEST_CASE("rules-la2x: periphrases are one verb, both words noted (C11b)") {
  NEED_WORLD();
  const auto rows = readTsv(repoDir() / "tests" / "fixtures" / "la2x" / "sentences.tsv");
  int n = 0;
  for (const auto& r : rows) {
    if (r.size() < 4 || r[3].compare(0, 11, "periphrasis") != 0) continue;
    ++n;
    la2x::SentenceOut o;
    W.tr->resetDiscourse();
    W.tr->sentence(r[0], la2x::Target::En, o);
    int noted = 0, verbs = 0;
    for (const la2x::Word& w : o.words) {
      noted += !w.note.empty();
      verbs += w.role == "verb";
    }
    CHECK_MESSAGE(noted == 2, r[0] << ": " << noted << " words carry the periphrasis note");
    CHECK_MESSAGE(verbs == 2, r[0] << ": " << verbs << " words have the role verb");
  }
  CHECK(n >= 15);
  la2x::SentenceOut en, es;
  W.tr->resetDiscourse();
  W.tr->sentence("Mārcus in templum ingressus est.", la2x::Target::En, en);
  REQUIRE(wordAt(en, "ingressus") != nullptr);
  CHECK(wordAt(en, "ingressus")->note == "ingressus est = entered (one verb: perfect of the deponent ingredior)");
  CHECK(wordAt(en, "est")->note == wordAt(en, "ingressus")->note);
  CHECK(std::find(en.analysis.tokens[(size_t)wordAt(en, "est")->token].why.begin(),
                  en.analysis.tokens[(size_t)wordAt(en, "est")->token].why.end(),
                  wordAt(en, "est")->note) != en.analysis.tokens[(size_t)wordAt(en, "est")->token].why.end());
  W.tr->resetDiscourse();
  W.tr->sentence("Puer ā mātre amātus erat.", la2x::Target::Es, es);
  REQUIRE(wordAt(es, "amātus") != nullptr);
  CHECK(wordAt(es, "amātus")->note == "amātus erat = había sido amado (un solo verbo: pluscuamperfecto pasivo de amō)");
  // the cue's word-by-word line gives the verb once
  rules::EngineConfig cfg = rules::defaultEngineConfig();
  cfg.curatedDir = (repoDir() / "data" / "curated").string();
  cfg.dataDir = workDir().string();
  cfg.nlpDir = (workDir() / "nlp").string();
  std::unique_ptr<rules::Engine> eng = rules::makeEngine(cfg);
  REQUIRE(eng->setLexicons(&W.la, nullptr, nullptr, nullptr).ok());
  std::vector<rules::CueInput> cues(1);
  cues[0].sourceText = "Puella Mārcum secūta est.";
  rules::Options opt;
  opt.source = rules::Lang::La;
  opt.target = rules::Lang::En;
  auto r = eng->translate(cues, opt, rules::Context(), nullptr, nullptr);
  REQUIRE(r.ok());
  CHECK(r.value()[0].target == "The girl followed Marcus.");
  REQUIRE(!r.value()[0].alternatives.empty());
  CHECK(r.value()[0].alternatives[0].text == "girl Marcus followed");
  bool noteInReasons = false;
  for (const rules::Reason& x : r.value()[0].reasons)
    noteInReasons = noteInReasons || x.data.find("\"note\":\"secūta est = followed") != std::string::npos;
  CHECK(noteInReasons);
}

TEST_CASE("rules-la2x: A6 counts a participle at its verb's tier (C11b, check.cpp)") {
  NEED_WORLD();
  check::LatinChecker ck(W.la, *W.cd);
  check::Options o;
  o.tierCeiling = 1;
  const check::Report rep = ck.check("Epistula lēcta est.", o);   // lēctus (participle lemma, tier 3) -> legō, tier 1
  CHECK_MESSAGE(rep.ok("A6"), "A6: " << rep.checks.back().detail);
  CHECK(ck.check("Puella Mārcum secūta est.", o).ok("A6"));      // secūtus -> sequor, tier 1
  CHECK(!ck.check("Epistula lēcta est et gladius nitidus est.", o).ok("A6"));   // a tier-3 word still fails
}

TEST_CASE("rules-la2x: interlinear view") {
  NEED_WORLD();
  la2x::SentenceOut en, es;
  W.tr->resetDiscourse();
  W.tr->sentence("Puella rosam amat.", la2x::Target::En, en);
  W.tr->resetDiscourse();
  W.tr->sentence("Puella rosam amat.", la2x::Target::Es, es);
  REQUIRE(en.words.size() == 3);
  CHECK(en.words[0].head == "puella");
  CHECK(en.words[0].features.case_ == "nominative");
  CHECK(en.words[0].gloss == "girl");
  CHECK(en.words[0].role == "subject");
  CHECK(en.words[1].role == "object");
  CHECK(en.words[2].features.person == "third");
  CHECK(en.words[2].tier == 1);
  CHECK(es.words[0].gloss.find("niña") != std::string::npos);
  CHECK(es.words[1].gloss == "rosa");   // gloss_es_la.tsv beats the lexicon's pivot gloss
  // a pivot gloss is flagged for the "(via English)" note
  bool anyPivot = false;
  la2x::SentenceOut p;
  W.tr->sentence("Agricola rosas in agro portat.", la2x::Target::Es, p);
  for (const la2x::Word& w : p.words) anyPivot = anyPivot || w.glossPivot;
  (void)anyPivot;   // depends on the library build; reported, not asserted
  // unknown words are kept and flagged; a capitalised unknown word is a name guess
  la2x::SentenceOut u;
  W.tr->sentence("Puella xyzzyqua amat.", la2x::Target::En, u);
  CHECK(wordAt(u, "xyzzyqua")->unknown);
  CHECK(std::find(u.flags.begin(), u.flags.end(), "unknown") != u.flags.end());
  la2x::SentenceOut g;
  W.tr->sentence("Puella Zorbam amat.", la2x::Target::En, g);
  CHECK(wordAt(g, "Zorbam")->nameGuess);
}

TEST_CASE("rules-la2x: engine pairs la-en / la-es, inspect, A9") {
  NEED_WORLD();
  rules::EngineConfig cfg = rules::defaultEngineConfig();
  cfg.curatedDir = (repoDir() / "data" / "curated").string();
  cfg.dataDir = workDir().string();
  cfg.nlpDir = (workDir() / "nlp").string();
  std::unique_ptr<rules::Engine> eng = rules::makeEngine(cfg);
  REQUIRE(eng->setLexicons(&W.la, nullptr, nullptr, nullptr).ok());
  std::vector<rules::CueInput> cues(2);
  cues[0].index = 0;
  cues[0].sourceText = "Puella rosam amat. Puer cantat.";
  cues[1].index = 1;
  cues[1].sourceText = "Mārce, venī!";
  rules::Options opt;
  opt.source = rules::Lang::La;
  opt.target = rules::Lang::En;
  size_t progressed = 0;
  auto r = eng->translate(cues, opt, rules::Context(), [&](size_t d) { progressed = d; }, nullptr);
  REQUIRE(r.ok());
  REQUIRE(r.value().size() == 2);
  CHECK(r.value()[0].target == "The girl loves a rose. The boy sings.");
  CHECK(r.value()[1].target == "Marcus, come!");
  CHECK(progressed == 2);
  const rules::CueOutput& o = r.value()[0];
  REQUIRE(o.tokens.size() == 5);
  CHECK(o.tokens[0].text == "Puella");
  CHECK(cues[0].sourceText.substr((size_t)o.tokens[3].start, (size_t)(o.tokens[3].end - o.tokens[3].start)) == "Puer");
  CHECK(o.tokens[1].hasLemma);
  CHECK(std::find(o.flags.begin(), o.flags.end(), "source-tokens") != o.flags.end());
  size_t analyses = 0;
  for (const rules::Reason& x : o.reasons) analyses += x.kind == "analysis";
  CHECK(analyses == 5);
  bool a1 = false, amb = false;
  for (const rules::Check& c : o.checks) {
    if (c.id == "A1") a1 = c.ok;
    if (c.id == "ambiguity") amb = true;
  }
  CHECK(a1);
  CHECK(amb);
  opt.target = rules::Lang::Es;
  auto rs = eng->translate(cues, opt, rules::Context(), nullptr, nullptr);
  REQUIRE(rs.ok());
  CHECK(rs.value()[0].target == "La niña ama una rosa. El niño canta.");
  // an unknown word: A1 fails, confidence Fix
  std::vector<rules::CueInput> bad(1);
  bad[0].sourceText = "Puella qwertyx amat.";
  opt.target = rules::Lang::En;
  auto rb = eng->translate(bad, opt, rules::Context(), nullptr, nullptr);
  REQUIRE(rb.ok());
  CHECK(rb.value()[0].confidence == rules::Confidence::Fix);
  // inspect: Latin glosses from the curated tables
  auto ins = eng->inspect("rosam", rules::Lang::La, opt);
  REQUIRE(ins.ok());
  REQUIRE(!ins.value().analyses.empty());
  CHECK(ins.value().analyses[0].glossEn == "rose");
  CHECK(ins.value().analyses[0].glossEs == "rosa");
  // A9 hook: content-lemma overlap of a Latin sentence with its English source
  CHECK(W.tr->roundTripOverlap("Puella rosam amat.", {"girl", "love", "rose", "the", "a"}) == doctest::Approx(1.0));
  CHECK(W.tr->roundTripOverlap("Puer cantat.", {"girl", "sing"}) == doctest::Approx(0.5));
  CHECK(W.tr->roundTripOverlap("Puella cantat.", {"niña", "cantar"}, la2x::Target::Es) == doctest::Approx(1.0));
}

TEST_CASE("rules-la2x: A9 inside the EN->LA engine") {
  NEED_WORLD();
  auto en = lex::Lexicon::open(workDir() / "english.vpl");
  if (!en.ok() || !stdfs::exists(workDir() / "nlp" / "english.tag.vpt")) {
    MESSAGE("skipped: english.vpl / nlp models not available");
    return;
  }
  rules::EngineConfig cfg = rules::defaultEngineConfig();
  cfg.curatedDir = (repoDir() / "data" / "curated").string();
  cfg.dataDir = workDir().string();
  cfg.nlpDir = (workDir() / "nlp").string();
  std::unique_ptr<rules::Engine> eng = rules::makeEngine(cfg);
  REQUIRE(eng->setLexicons(&W.la, nullptr, &en.value(), nullptr).ok());
  std::vector<rules::CueInput> cues(1);
  cues[0].sourceText = "The girl loves the rose.";
  rules::Options opt;
  auto r = eng->translate(cues, opt, rules::Context(), nullptr, nullptr);
  REQUIRE(r.ok());
  bool found = false;
  for (const rules::Check& c : r.value()[0].checks)
    if (c.id == "A9") {
      found = true;
      CHECK(c.ok);
      CHECK(c.detail.find("round-trip overlap") != std::string::npos);
      MESSAGE("EN->LA '" << cues[0].sourceText << "' -> '" << r.value()[0].target << "': " << c.detail);
    }
  CHECK(found);
}

TEST_CASE("rules-la2x: back-translation of the regression gold file") {
  NEED_WORLD();
  const stdfs::path gold = repoDir() / "tests" / "regression" / "expected" / "own_dialogue.la.gold.txt";
  const stdfs::path src = repoDir() / "tests" / "regression" / "own_dialogue.en.txt";
  std::ifstream gi(gold), si(src);
  if (!gi || !si) {
    MESSAGE("skipped: regression files not found");
    return;
  }
  auto enLex = lex::Lexicon::open(workDir() / "english.vpl");
  std::vector<std::string> la, en;
  std::string line;
  while (std::getline(gi, line)) {
    if (line.empty() || line[0] == '#') continue;
    const size_t bar = line.find(" | ");
    la.push_back(bar == std::string::npos ? line : line.substr(0, bar));
  }
  while (std::getline(si, line)) en.push_back(line);
  REQUIRE(la.size() == en.size());
  // English content lemmas (english.vpl when present, else lower-case words)
  auto lemmas = [&](const std::string& s) {
    std::vector<std::string> out;
    std::string w;
    auto flush = [&]() {
      if (w.empty()) return;
      std::string lemma = w;
      if (enLex.ok()) {
        std::vector<lex::Analysis> an;
        enLex.value().lookup(text::en_key(w), an);
        // regular inflections first ("going" -> go, "flowers" -> flower), then an irregular form's lemma (went -> go)
        std::vector<std::string> heads;
        for (const lex::Analysis& a : an) heads.push_back(text::lower(enLex.value().lemma(a.lemma).head));
        auto has = [&](const std::string& h) { return std::find(heads.begin(), heads.end(), h) != heads.end(); };
        auto ends = [&](const char* x) {
          const size_t k = std::char_traits<char>::length(x);
          return w.size() > k + 1 && w.compare(w.size() - k, k, x) == 0;
        };
        std::vector<std::string> cand;
        if (ends("ing")) { const std::string st = w.substr(0, w.size() - 3); cand = {st, st + "e"}; if (st.size() > 2 && st[st.size() - 1] == st[st.size() - 2]) cand.push_back(st.substr(0, st.size() - 1)); }
        else if (ends("ies")) cand = {w.substr(0, w.size() - 3) + "y"};
        else if (ends("ied")) cand = {w.substr(0, w.size() - 3) + "y"};
        else if (ends("ed")) { const std::string st = w.substr(0, w.size() - 2); cand = {st, st + "e"}; }
        else if (ends("es")) cand = {w.substr(0, w.size() - 2), w.substr(0, w.size() - 1)};
        else if (ends("s") && !ends("ss")) cand = {w.substr(0, w.size() - 1)};
        bool done = false;
        for (const std::string& c : cand)
          if (has(c)) { lemma = c; done = true; break; }
        if (!done && !has(w)) {
          for (const std::string& h : heads)
            if (!h.empty() && h.find(' ') == std::string::npos) { lemma = h; break; }
        }
      }
      out.push_back(lemma);
      w.clear();
    };
    for (char c : text::lower(s)) {
      if ((c >= 'a' && c <= 'z') || c == '\'') w += c;
      else flush();
    }
    flush();
    // contractions
    std::vector<std::string> o2;
    for (std::string& x : out) {
      const size_t ap = x.find('\'');
      o2.push_back(ap == std::string::npos ? x : x.substr(0, ap));
    }
    return o2;
  };
  static const char* const kStop[] = {"a", "an", "the", "be", "is", "are", "am", "was", "were", "do", "does", "did",
                                      "have", "has", "to", "of", "and", "or", "not", "n", "i", "you", "he", "she",
                                      "it", "we", "they", "me", "him", "her", "us", "them", "my", "your", "his", "its",
                                      "our", "their", "this", "that", "what", "who", "where", "how", "why", "when",
                                      "will", "shall", "can", "could", "would", "should", "may", "must", "let", "s",
                                      "m", "re", "ll", "ve", "d", "t", "oh", "so", "then", "now", "very", "too",
                                      "please", "here", "there", "with", "in", "on", "at", "for", "from", "by", "o"};
  auto content = [&](const std::vector<std::string>& ws) {
    std::vector<std::string> out;
    for (const std::string& w : ws) {
      bool stop = false;
      for (const char* k : kStop) stop = stop || w == k;
      if (!stop && std::find(out.begin(), out.end(), w) == out.end()) out.push_back(w);
    }
    return out;
  };
  struct Row { size_t i; double readable, a9; std::string back; };
  std::vector<Row> rows;
  double sumR = 0, sumA = 0;
  for (size_t i = 0; i < la.size(); ++i) {
    W.tr->resetDiscourse();
    std::vector<la2x::SentenceOut> outs;
    W.tr->text(la[i], la2x::Target::En, outs);
    std::string back;
    for (const auto& o : outs) back += (back.empty() ? "" : " ") + o.text;
    const std::vector<std::string> srcL = content(lemmas(en[i]));
    const std::vector<std::string> backL = content(lemmas(back));
    size_t hit = 0;
    for (const std::string& w : srcL)
      if (std::find(backL.begin(), backL.end(), w) != backL.end()) ++hit;
    const double r = srcL.empty() ? 1.0 : (double)hit / (double)srcL.size();
    const double a = W.tr->roundTripOverlap(la[i], lemmas(en[i]));
    rows.push_back(Row{i, r, a, back});
    sumR += r;
    sumA += a;
  }
  const double meanR = sumR / (double)rows.size(), meanA = sumA / (double)rows.size();
  std::stable_sort(rows.begin(), rows.end(), [](const Row& x, const Row& y) { return x.readable < y.readable; });
  std::ostringstream rep;
  rep << "back-translation of " << rows.size() << " gold lines: mean content-lemma overlap (readable English vs source) "
      << meanR << ", mean A9 gloss overlap " << meanA << "\nworst 15:\n";
  for (size_t k = 0; k < rows.size() && k < 15; ++k)
    rep << "  #" << rows[k].i + 1 << " " << rows[k].readable << " / A9 " << rows[k].a9 << "  " << la[rows[k].i] << " -> "
        << rows[k].back << "   [source: " << en[rows[k].i] << "]\n";
  MESSAGE(rep.str());
  {
    std::ofstream f(stdfs::path(VP_TEST_TMP).parent_path() / "la2x_backtranslation.txt");
    f << rep.str();
    for (const Row& r : rows) f << r.i + 1 << "\t" << r.readable << "\t" << r.a9 << "\t" << la[r.i] << "\t" << r.back << "\t" << en[r.i] << "\n";
  }
  CHECK(meanR >= 0.0);   // reported, no threshold (task C11)
}

TEST_CASE("rules-la2x: determinism") {
  NEED_WORLD();
  auto t2 = la2x::Translator::create(W.la, *W.cd, {repoDir() / "data" / "curated"});
  REQUIRE(t2.ok());
  const auto rows = readTsv(repoDir() / "tests" / "fixtures" / "la2x" / "sentences.tsv");
  std::string a, b;
  for (int pass = 0; pass < 2; ++pass) {
    la2x::Translator& t = pass == 0 ? *W.tr : *t2.value();
    std::string& acc = pass == 0 ? a : b;
    t.resetDiscourse();
    for (const auto& r : rows)
      for (la2x::Target tg : {la2x::Target::En, la2x::Target::Es}) {
        la2x::SentenceOut o;
        t.sentence(r[0], tg, o);
        acc += o.text + "|" + std::to_string(o.confidence) + "|";
        for (const la2x::Word& w : o.words) acc += w.head + "/" + w.featureText + "/" + w.gloss + "/" + std::to_string(w.confidence) + ";";
        acc += "\n";
      }
  }
  CHECK(a == b);
}

TEST_CASE("rules-la2x: RSS flat over 1,000 sentences") {
  NEED_WORLD();
  const auto rows = readTsv(repoDir() / "tests" / "fixtures" / "la2x" / "sentences.tsv");
  la2x::SentenceOut o;
  long after10 = -1;
  for (int i = 0; i < 1000; ++i) {
    W.tr->sentence(rows[(size_t)i % rows.size()][0], (i & 1) ? la2x::Target::Es : la2x::Target::En, o);
    if (i == 9) after10 = rssAnonKbLa2x();
  }
  const long after1000 = rssAnonKbLa2x();
  MESSAGE("RssAnon after 10 sentences: " << after10 << " kB, after 1,000: " << after1000 << " kB");
#if defined(__SANITIZE_ADDRESS__)
  MESSAGE("AddressSanitizer build: the quarantine holds freed memory, RSS is reported only");
#else
  if (after10 > 0 && after1000 > 0) CHECK((double)after1000 <= (double)after10 * 1.05 + 64.0);
#endif
}

TEST_CASE("rules-la2x: odd input never crashes") {
  NEED_WORLD();
  std::string longS;
  for (int i = 0; i < 120; ++i) longS += (i % 3 == 0) ? "puella " : (i % 3 == 1) ? "rosam " : "amat et ";
  longS += "cantat.";
  const char* inputs[] = {"", ".", "?!", "   ", "123 456.", "ἄνθρωπος λέγει.", "Puella, , , amat!!!", "-que -ne", "QVIS ES?",
                          "puella\tamat\nrosam", "Mārce, Iūlia, Quīnte, venīte!", "et et et et.", "Nōn.", "Ō!"};
  for (const char* in : inputs) {
    la2x::SentenceOut o;
    W.tr->sentence(in, la2x::Target::En, o);
    W.tr->sentence(in, la2x::Target::Es, o);
    CHECK(o.confidence >= 0.0);
  }
  la2x::SentenceOut o;
  W.tr->sentence(longS, la2x::Target::En, o);
  CHECK(!o.text.empty());
  std::vector<la2x::SentenceOut> many;
  W.tr->text("Puella cantat. Puer saltat! Quis venit? Valē", la2x::Target::Es, many);
  CHECK(many.size() == 4);
  CHECK(la2x::splitSentences("Ā. B? C! D").size() == 4);
}

TEST_CASE("rules-la2x: dev dump (VP_LA2X_DUMP=<file or sentence>)") {
  const char* e = std::getenv("VP_LA2X_DUMP");
  if (!e || !*e) return;
  NEED_WORLD();
  std::vector<std::string> lines;
  std::ifstream in(e);
  if (in) {
    std::string l;
    while (std::getline(in, l))
      if (!l.empty() && l[0] != '#') lines.push_back(l.substr(0, l.find('\t')));
  } else {
    lines.push_back(e);
  }
  for (const std::string& l : lines) {
    la2x::SentenceOut en, es;
    W.tr->resetDiscourse();
    W.tr->sentence(l, la2x::Target::En, en);
    W.tr->resetDiscourse();
    W.tr->sentence(l, la2x::Target::Es, es);
    std::cout << "LA " << l << "\nEN " << en.text << "\nES " << es.text << "\nFR " << en.frame << "\n";
    for (const la2x::Word& w : en.words)
    {
      std::cout << "   " << w.text << " = " << w.head << " [" << w.featureText << "] '" << w.gloss << "' c=" << w.confidence
                << " role=" << w.role << (w.alternatives.empty() ? "" : " alt: " + w.alternatives[0])
                << (w.note.empty() ? "" : " note: " + w.note) << "\n";
      if (std::getenv("VP_LA2X_SCORES")) {
        const la2x::Token& tk = en.analysis.tokens[(size_t)w.token];
        for (const la2x::Reading& r : tk.readings)
          std::cout << "        " << r.display << " L" << r.lemma << " p=" << r.prior << " s=" << r.score << " " << std::hex << r.packed << std::dec << "\n";
        for (const std::string& y : tk.why) std::cout << "        why: " << y << "\n";
      }
    }
  }
}
