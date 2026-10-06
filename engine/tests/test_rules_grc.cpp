// engine/rules Greek side (C9): morphology helpers, accents and sandhi, realisation, checker.
#include <doctest.h>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "vp/lex.h"
#include "vp/morph_grc.h"
#include "vp/text.h"

namespace stdfs = std::filesystem;
using namespace vp;
using namespace vp::feat;

namespace {
stdfs::path repoRoot() { return stdfs::path(VP_FIXTURES_DIR).parent_path().parent_path(); }
struct Held { lex::Lexicon lx; bool ok = false; std::string why; };
Held openHeld(const stdfs::path& p) {
  Held h;
  auto r = lex::Lexicon::open(p);
  if (r.ok()) { h.lx = std::move(r.value()); h.ok = true; }
  else h.why = r.error().message;
  return h;
}
const Held& realGrc() {
  static Held h = [] {
    const char* env = std::getenv("VP_GREEK_VPL");
    return openHeld(env && *env ? stdfs::path(env) : repoRoot() / "data" / "work" / "greek.vpl");
  }();
  return h;
}
}  // namespace

TEST_CASE("rules-grc: scratch (VP_GRC_SCRATCH=1)") {
  if (!std::getenv("VP_GRC_SCRATCH") || !realGrc().ok) return;
  const lex::Lexicon& lx = realGrc().lx;
  auto gen = [&](const char* head, uint8_t pos, Features f) {
    uint32_t id = grc::findLemma(lx, head, pos);
    std::string out;
    grc::GenInfo gi;
    bool ok = grc::generate(lx, id, f, out, &gi);
    std::cout << head << " [" << id << "] -> " << (ok ? out : "<none>") << (gi.attic ? " attic" : "")
              << (gi.contracted ? " contr" : "") << (gi.movableNu ? " nu" : "") << "\n";
  };
  gen("λύω", Verb, grc::verbForm(3, Pl, Present));
  gen("λύω", Verb, grc::verbForm(3, Sg, Aorist));
  gen("εἰμί", Verb, grc::verbForm(3, Sg, Present));
  gen("εἰμί", Verb, grc::verbForm(2, Sg, Imperfect));
  gen("εἰμί", Verb, grc::verbForm(3, Sg, Future));
  gen("ποιέω", Verb, grc::verbForm(3, Sg, Present));
  gen("τιμάω", Verb, grc::verbForm(1, Pl, Present));
  gen("δηλόω", Verb, grc::verbForm(3, Pl, Imperfect));
  gen("ἄνθρωπος", Noun, grc::nounForm(Gen, Sg));
  gen("ἄνθρωπος", Noun, grc::nounForm(Dat, Pl));
  gen("πόλις", Noun, grc::nounForm(Dat, Sg));
  gen("καλός", Adj, grc::adjForm(Dat, Pl, F));
  gen("οἶδα", Verb, grc::verbForm(1, Sg, Present));
  gen("φοβέω", Verb, grc::imperative(Sg, Present, Middle));
  gen("δύναμαι", Verb, grc::verbForm(3, Sg, Present));
  gen("ἔρχομαι", Verb, grc::verbForm(2, Sg, Present));
  gen("ὁράω", Verb, grc::verbForm(2, Sg, Aorist));
  gen("δίδωμι", Verb, grc::imperative(Sg, Aorist));
  gen("οὗτος", feat::Det, grc::adjForm(Nom, Pl, N));
  for (const char* p : {"ἄνθρωπος τις", "λόγος ἐστί.", "λόγοι τινές.", "δῶρόν τι", "οὐ ἔστι", "ποῦ ἐστιν ἡ σφαῖρα;",
                        "καλός ἐστιν.", "δός μοι τὴν σφαῖραν.", "εἰ τις", "παῖδες τινες", "πηνίκα ἐστί;",
                        "τὰ ἄνθη οὐ δύναται λαλεῖν.", "ὁ πατήρ μου γεωργός ἐστι.", "ἡ θύρα μικρά ἐστι."})
    std::cout << p << " => " << grc::accentuate(p) << "\n";
  for (const char* w : {"ἄνθρωπον", "ανθρωπον", "ἀνθρώποιο", "ἐστίν", "πόλει", "οἶδα", "ἀδελφούς"}) {
    morph::Token t;
    grc::analyse(lx, w, t);
    std::cout << w << ": " << t.analyses.size() << (t.accentInsensitive ? " (bare)" : "") << "\n";
  }
}
