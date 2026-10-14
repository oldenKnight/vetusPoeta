// engine/rules la2x, task C36: Latin -> English / Spanish on textbook prose and devotional verse. Own sentences only
// (two or more per rule), the meaning-coverage calibration, and the regression on tests/regression/own_prose.la.txt
// and own_prayer.la.txt against their gold. Needs the real lexicon (data/work/latin.vpl or VP_DATA_WORK); every case
// skips with a message when it is absent.
#include <doctest.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "../rules/src/la2x/internal.h"
#include "vp/curated.h"
#include "vp/la2x.h"
#include "vp/lex.h"
#include "vp/rules.h"
#include "vp/text.h"

namespace stdfs = std::filesystem;
using namespace vp;

namespace {

stdfs::path repo3() { return stdfs::path(VP_FIXTURES_DIR).parent_path().parent_path(); }
stdfs::path work3() {
  const char* e = std::getenv("VP_DATA_WORK");
  return e && *e ? stdfs::path(e) : repo3() / "data" / "work";
}
stdfs::path build3() {
  const stdfs::path p = stdfs::path(VP_TEST_TMP).parent_path();
  return p;
}

struct World3 {
  lex::Lexicon la;
  std::unique_ptr<curated::CuratedData> cd;
  std::unique_ptr<la2x::Translator> tr;
  bool ok = false;
  std::string why;
};
World3& world3() {
  static World3 w = [] {
    World3 x;
    auto la = lex::Lexicon::open(work3() / "latin.vpl");
    if (!la.ok()) { x.why = la.error().message; return x; }
    x.la = std::move(la.value());
    auto cd = curated::CuratedData::load(repo3() / "data" / "curated");
    if (!cd.ok()) { x.why = cd.error().message; return x; }
    x.cd = std::make_unique<curated::CuratedData>(std::move(cd.value()));
    auto tr = la2x::Translator::create(x.la, *x.cd, {repo3() / "data" / "curated"});
    if (!tr.ok()) { x.why = tr.error().message; return x; }
    x.tr = std::move(tr.value());
    x.ok = true;
    return x;
  }();
  return w;
}
#define NEED_WORLD3()                                                        \
  World3& W = world3();                                                      \
  if (!W.ok) {                                                               \
    MESSAGE("skipped: real Latin lexicon not available (" << W.why << ")");  \
    return;                                                                  \
  }

// lower case, punctuation and extra spaces removed (the gold comparison of the regression files)
std::string norm3(const std::string& s0) {
  const std::string s = text::lower(text::nfc(s0));
  std::string out;
  size_t i = 0;
  while (i < s.size()) {
    size_t j = i;
    const char32_t c = text::decodeUtf8(s, j);
    const bool punct = c == '.' || c == ',' || c == '!' || c == '?' || c == ';' || c == ':' || c == U'¿' ||
                       c == U'¡' || c == '"' || c == '\'' || c == U'“' || c == U'”';
    if (punct || c == ' ' || c == '\t') {
      if (!out.empty() && out.back() != ' ') out += ' ';
    } else {
      out.append(s, i, j - i);
    }
    i = j;
  }
  while (!out.empty() && out.back() == ' ') out.pop_back();
  return out;
}

bool matches3(const std::string& ours, const std::string& gold) {
  size_t a = 0;
  for (;;) {
    const size_t b = gold.find(" | ", a);
    if (norm3(ours) == norm3(gold.substr(a, b == std::string::npos ? std::string::npos : b - a))) return true;
    if (b == std::string::npos) return false;
    a = b + 3;
  }
}

std::vector<std::string> lines3(const stdfs::path& p) {
  std::vector<std::string> out;
  std::ifstream in(p);
  std::string l;
  while (std::getline(in, l)) {
    if (!l.empty() && l.back() == '\r') l.pop_back();
    if (!l.empty() && l[0] != '#') out.push_back(l);
  }
  return out;
}

struct Case3 { const char* la; const char* en; const char* es; };

void runCases(World3& W, const std::vector<Case3>& cases) {
  for (const Case3& c : cases) {
    la2x::SentenceOut en, es;
    W.tr->resetDiscourse();
    W.tr->sentence(c.la, la2x::Target::En, en);
    W.tr->resetDiscourse();
    W.tr->sentence(c.la, la2x::Target::Es, es);
    INFO(c.la << " -> " << en.text << " / " << es.text);
    CHECK(en.text == c.en);
    CHECK(es.text == c.es);
  }
}

// the readable text of each unit of a text, translated as one job (units joined like a text project)
std::vector<rules::CueOutput> runUnits(World3& W, const std::vector<std::string>& units, la2x::Target t) {
  std::vector<rules::CueInput> in(units.size());
  for (size_t i = 0; i < units.size(); ++i) {
    in[i].index = (uint32_t)i;
    in[i].sourceText = units[i];
  }
  rules::Options opt;
  opt.source = rules::Lang::La;
  opt.target = t == la2x::Target::En ? rules::Lang::En : rules::Lang::Es;
  return W.tr->cues(in, opt, rules::Context(), nullptr, nullptr);
}

// A lexical source made from the Translator's glosses, for the coverage check on hand-made wrong translations
class GlossSource : public la2x::detail::LexicalSource {
 public:
  explicit GlossSource(const la2x::Translator& t) : t_(t) {}
  la2x::detail::Lexical lexical(const la2x::detail::TokInfo& t, la2x::Target lang) const override {
    la2x::detail::Lexical l;
    if (t.lemma == lex::kNoLemma) { l.word = t.text; return l; }
    l.word = t_.gloss(t.verbLemma != lex::kNoLemma ? t.verbLemma : t.lemma, lang);
    return l;
  }
  std::string nameIn(const la2x::detail::TokInfo& t, la2x::Target) const override { return t.text; }

 private:
  const la2x::Translator& t_;
};

std::vector<std::string> coverageOf(World3& W, const char* latin, const std::string& text, la2x::Target t) {
  la2x::Sentence s;
  W.tr->analyser().analyse(latin, s);
  std::vector<la2x::detail::TokInfo> ti;
  la2x::detail::tokInfos(W.la, s, ti);
  la2x::detail::Tables en, es;
  std::string err;
  REQUIRE(en.load(repo3() / "data" / "curated" / "readable_en.tsv", err));
  REQUIRE(es.load(repo3() / "data" / "curated" / "readable_es.tsv", err));
  la2x::detail::Built b;
  la2x::detail::buildFrames(W.la, t == la2x::Target::En ? en : es, s, ti, b);
  la2x::detail::Coverage cov;
  cov.init(en, es);
  GlossSource src(*W.tr);
  la2x::detail::CoverageIn ci;
  ci.s = &s;
  ci.ti = &ti;
  ci.b = &b;
  ci.tab = t == la2x::Target::En ? &en : &es;
  ci.lex = &src;
  ci.lang = t;
  const std::vector<uint32_t> none;
  ci.mentionedBefore = &none;
  std::vector<std::string> flags, notes;
  cov.check(ci, text, flags, notes);
  return flags;
}
bool hasFlag3(const std::vector<std::string>& f, const char* x) { return std::find(f.begin(), f.end(), x) != f.end(); }

}  // namespace

TEST_CASE("rules-la2x3: meaning coverage makes a wrong translation Check (C36 calibration)") {
  NEED_WORLD3();
  using la2x::Target;
  // a right translation raises nothing
  CHECK(coverageOf(W, "Puella rosam amat.", "The girl loves a rose.", Target::En).empty());
  CHECK(coverageOf(W, "Puer aquam portat.", "The boy carries water.", Target::En).empty());
  // a dropped content word
  CHECK(hasFlag3(coverageOf(W, "Puella rosam amat.", "The girl loves.", Target::En), "meaning-dropped"));
  CHECK(hasFlag3(coverageOf(W, "Puer aquam portat.", "El niño lleva.", Target::Es), "meaning-dropped"));
  // an added content word
  CHECK(hasFlag3(coverageOf(W, "Puella rosam amat.", "The girl loves a rose and a horse.", Target::En), "meaning-added"));
  CHECK(hasFlag3(coverageOf(W, "Puer cantat.", "El niño canta en el jardín.", Target::Es), "meaning-added"));
  // a dangling conjunction
  CHECK(hasFlag3(coverageOf(W, "Puer cantat.", "The boy sings and.", Target::En), "dangling-conjunction"));
  CHECK(hasFlag3(coverageOf(W, "Puella cantat.", "La niña canta y.", Target::Es), "dangling-conjunction"));
  // a participle that contradicts the gender of the Latin (the addressee)
  CHECK(hasFlag3(coverageOf(W, "Laeta es.", "Estás contento.", Target::Es), "gender-mismatch"));
  CHECK(!hasFlag3(coverageOf(W, "Laeta es.", "Estás contenta.", Target::Es), "gender-mismatch"));
  CHECK(hasFlag3(coverageOf(W, "Puella fessa est.", "La niña está cansado.", Target::Es), "gender-mismatch"));
}

TEST_CASE("rules-la2x3: copula, particles, not ... but, predicate adjectives (C36)") {
  NEED_WORLD3();
  runCases(W, {
      // copula with the subject after it: "the noun is adjective"
      {"Pulchra est rosa.", "The rose is beautiful.", "La rosa es bonita."},
      {"Longus est fluvius.", "The river is long.", "El río es largo."},
      // postpositive particles
      {"Puer autem dormit.", "But the boy sleeps.", "Pero el niño duerme."},
      {"Laeta enim est puella.", "For the girl is happy.", "Pues la niña está contenta."},
      {"Servus vero laborat.", "But the slave works.", "Pero el esclavo trabaja."},
      {"Puella igitur cantat.", "Therefore the girl sings.", "Por lo tanto la niña canta."},
      // nōn ... sed
      {"Mārcus nōn in hortō sed in viā ambulat.", "Marcus walks not in the garden but in the road.",
       "Marco no camina en el jardín sino en el camino."},
      {"Puer nōn trīstis est sed laetus.", "The boy is not sad but happy.", "El niño no está triste sino contento."},
      {"Puella nōn dormit sed legit.", "The girl does not sleep but reads.", "La niña no duerme sino que lee."},
      {"Servus nōn sōlum labōrat sed etiam cantat.", "The slave not only works but also sings.",
       "El esclavo no solo trabaja sino que también canta."},
      // līber "free" in predicate position, liber "book" otherwise
      {"Servus līber est.", "The slave is free.", "El esclavo es libre."},
      {"Līberī sunt servī.", "The slaves are free.", "Los esclavos son libres."},
      {"Liber magnus est.", "The book is big.", "El libro es grande."},
      // sine: a first mention is indefinite in English, bare in Spanish
      {"Puer sine mātre est.", "The boy is without a mother.", "El niño está sin madre."},
      {"Fēlīx est puella.", "The girl is happy.", "La niña es feliz."},
  });
}

TEST_CASE("rules-la2x3: prayers: greetings, names, imperatives, prohibitions, vocatives (C36)") {
  NEED_WORLD3();
  runCases(W, {
      // Ave as a greeting (never aveō or avis), names
      {"Ave, Marīa!", "Hail, Mary!", "¡Ave, María!"},
      {"Ave, rēgīna!", "Hail, queen!", "¡Ave, reina!"},
      {"Iuppiter deus est.", "Jupiter is a god.", "Júpiter es un dios."},
      {"Iēsū Chrīste, audī nōs.", "Jesus Christ, hear us.", "Jesucristo, óyenos."},
      // esse / estō are sum, never edō
      {"Esto bonus!", "Be good!", "¡Sé bueno!"},
      // deponent imperatives
      {"Sequere mē!", "Follow me!", "¡Sígueme!"},
      {"Cōnsōlāre puellam!", "Console a girl!", "¡Consuela a una niña!"},
      {"Miserēre nostrī.", "Have mercy on us.", "Ten piedad de nosotros."},
      // nē + subjunctive without a main verb: a prohibition
      {"Nē timeās!", "Do not fear!", "¡No temas!"},
      {"Et nē nōs relinquās.", "And do not abandon us.", "Y no nos dejes."},
      // a capitalised vocative inside the sentence
      {"Audī nōs, Rēgīna caelī.", "Hear us, queen of heaven.", "Óyenos, reina del cielo."},
      // the addressee's gender
      {"Ēlēcta es.", "You were chosen.", "Fuiste elegida."},
      // et after the verb = also
      {"Dā et aquam.", "Give water also.", "Da también agua."},
      {"Portā et vīnum.", "Carry wine also.", "Lleva también vino."},
      // cōnferre = grant
      {"Cōnfer mihi pācem.", "Grant me peace.", "Concédeme paz."},
  });
}

TEST_CASE("rules-la2x3: context across sentences (implicit subjects, devotional words, Maria) (C36)") {
  NEED_WORLD3();
  auto second = [&](const char* text, la2x::Target t) {
    std::vector<la2x::SentenceOut> out;
    W.tr->resetDiscourse();
    W.tr->text(text, t, out);
    return out.size() >= 2 ? out[1].text : std::string();
  };
  using la2x::Target;
  // an object-first clause without a subject while the text is about a person: the neuter is the object
  CHECK(second("Puer in agrō est. Frūmentum portat.", Target::En) == "He carries grain.");
  CHECK(second("Puer in agrō est. Frūmentum portat.", Target::Es) == "Lleva trigo.");
  CHECK(second("Puella in hortō est. Vīnum et oleum portat.", Target::En) == "She carries wine and oil.");
  // without a person in the discourse the neuter stays the subject
  CHECK(second("Hōra est. Tempus fugit.", Target::En) == "The time flees.");
  // salvē in a prayer is "hail", in the classroom "hello"; Marīa is Mary only in a prayer
  CHECK(second("Ave, Marīa. Salvē, rēgīna.", Target::En) == "Hail, queen.");
  CHECK(second("Ave, Marīa. Salvē, rēgīna.", Target::Es) == "Salve, reina.");
  W.tr->resetDiscourse();
  la2x::SentenceOut o;
  W.tr->sentence("Marīa epistulam scrībit.", Target::En, o);
  CHECK(o.text == "Maria writes a letter.");
  // the addressee's home and the restored person
  CHECK(second("Vir in agrō labōrat. Amat domum.", Target::En) == "He loves his home.");
  CHECK(second("Vir in agrō labōrat. Amat domum.", Target::Es) == "Ama su casa.");
}

TEST_CASE("rules-la2x3: own prose and own prayer lines against the gold (C36)") {
  NEED_WORLD3();
  struct File { const char* la; const char* gold; la2x::Target t; int floor; };
  // first run (before the C36 rules, through vpengine serve): prose EN 18/60, ES 18/60; prayer EN 4/30, ES 6/30.
  // Final through vpengine serve: 56, 54, 28, 30 (this one-job run keeps every mention in the discourse: 54, 53).
  const File files[] = {
      {"own_prose.la.txt", "own_prose.en.gold.txt", la2x::Target::En, 54},
      {"own_prose.la.txt", "own_prose.es.gold.txt", la2x::Target::Es, 53},
      {"own_prayer.la.txt", "own_prayer.en.gold.txt", la2x::Target::En, 28},
      {"own_prayer.la.txt", "own_prayer.es.gold.txt", la2x::Target::Es, 30},
  };
  std::ofstream rep(build3() / "la2x3_report.txt");
  rep << "# C36 own material: file, unit, ok?, confidence, ours, gold (first alternative)\n";
  for (const File& f : files) {
    const std::vector<std::string> units = lines3(repo3() / "tests" / "regression" / f.la);
    const std::vector<std::string> gold = lines3(repo3() / "tests" / "regression" / "expected" / f.gold);
    REQUIRE(units.size() == gold.size());
    const std::vector<rules::CueOutput> out = runUnits(W, units, f.t);
    REQUIRE(out.size() == units.size());
    int right = 0, ok = 0, okWrong = 0;
    for (size_t i = 0; i < units.size(); ++i) {
      const bool m = matches3(out[i].target, gold[i]);
      right += m;
      if (out[i].confidence == rules::Confidence::Ok) {
        ++ok;
        okWrong += !m;
      }
      rep << f.gold << "\t" << i + 1 << "\t" << (m ? "ok" : "MISMATCH") << "\t"
          << (out[i].confidence == rules::Confidence::Ok ? "ok" : out[i].confidence == rules::Confidence::Check ? "check" : "fix")
          << "\t" << out[i].target << "\t" << gold[i].substr(0, gold[i].find(" | ")) << "\n";
    }
    MESSAGE(std::string(f.gold) << ": " << right << "/" << units.size() << " right, OK " << ok << " (" << okWrong
                   << " of them not matching the gold)");
    CHECK(right >= f.floor);
  }
}

TEST_CASE("rules-la2x3: determinism of the C36 paths") {
  NEED_WORLD3();
  const std::vector<std::string> units = lines3(repo3() / "tests" / "regression" / "own_prayer.la.txt");
  const auto a = runUnits(W, units, la2x::Target::Es);
  const auto b = runUnits(W, units, la2x::Target::Es);
  REQUIRE(a.size() == b.size());
  for (size_t i = 0; i < a.size(); ++i) {
    CHECK(a[i].target == b[i].target);
    CHECK(a[i].confidence == b[i].confidence);
  }
}
