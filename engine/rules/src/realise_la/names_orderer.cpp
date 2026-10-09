// Names (names_la.tsv + glossary policies) and Orderer (order_la.txt templates and word lists).
#include <algorithm>

#include "../morph/paradigm_la.h"
#include "vp/realise_la.h"
#include "vp/text.h"

namespace vp::realise {

using namespace vp::feat;
using morph::detail::dropSuffix;
using morph::detail::endsWith;

// ---- Names --------------------------------------------------------------------------------------------------------
bool Names::decline(std::string_view nom, std::string_view gen, int declension, uint8_t gender, uint8_t case_,
                    uint8_t number, std::string& out) {
  const int ci = case_ == Nom ? 0 : case_ == Gen ? 1 : case_ == Dat ? 2 : case_ == Acc ? 3 : case_ == Abl ? 4
                 : case_ == Voc ? 5 : -1;
  if (declension <= 0 || ci < 0) { out = std::string(nom); return declension <= 0 && ci >= 0; }
  if (declension == 1) {
    static const char* const sg[6] = {"a", "ae", "ae", "am", "ā", "a"};
    static const char* const pl[6] = {"ae", "ārum", "īs", "ās", "īs", "ae"};
    if (endsWith(nom, "ae")) { out = dropSuffix(nom, "ae") + pl[ci]; return true; }   // Athēnae (plural name)
    if (endsWith(nom, "ās")) {                                                      // Thomās (Greek type)
      static const char* const gr[6] = {"ās", "ae", "ae", "am", "ā", "ā"};
      out = dropSuffix(nom, "ās") + gr[ci];
      return true;
    }
    if (!endsWith(nom, "a")) return false;
    out = dropSuffix(nom, "a") + (number == Pl ? pl[ci] : sg[ci]);
    return true;
  }
  if (declension == 2) {
    if (!endsWith(gen, "ī")) return false;
    const std::string stem = dropSuffix(gen, "ī");
    if (endsWith(nom, "um") || gender == N) {
      static const char* const n[6] = {"um", "ī", "ō", "um", "ō", "um"};
      out = stem + n[ci];
      return true;
    }
    static const char* const m[6] = {"@", "ī", "ō", "um", "ō", "e"};
    if (ci == 0) { out = std::string(nom); return true; }
    if (ci == 5) {
      if (endsWith(nom, "ius")) { out = dropSuffix(nom, "ius") + "ī"; return true; }
      if (endsWith(nom, "us")) { out = dropSuffix(nom, "us") + "e"; return true; }
      out = std::string(nom);
      return true;
    }
    out = stem + m[ci];
    return true;
  }
  if (declension == 3) {
    if (!endsWith(gen, "is")) return false;
    const std::string stem = dropSuffix(gen, "is");
    static const char* const e[6] = {"@", "is", "ī", "em", "e", "@"};
    if (ci == 0 || ci == 5 || (ci == 3 && gender == N)) out = std::string(nom);
    else out = stem + e[ci];
    return true;
  }
  return false;
}

NameForm Names::form(std::string_view source, uint8_t case_, uint8_t number,
                     const std::vector<rules::GlossaryEntry>* glossary) const {
  NameForm r;
  const std::string sk = text::en_key(source);
  if (glossary) {
    for (const rules::GlossaryEntry& g : *glossary) {
      if (text::en_key(g.name) != sk) continue;
      std::string_view gg = g.gender;
      r.gender = gg.empty() ? 0 : (gg[0] == 'f' ? (uint8_t)F : gg[0] == 'n' ? (uint8_t)N : (uint8_t)M);
      if (g.policy == "keep" || g.form.empty()) { r.form = std::string(source); r.kept = true; return r; }
      const std::string nom = text::nfc(g.form);
      int dcl = g.declension;
      std::string gen;
      if (dcl == 0) dcl = endsWith(nom, "a") ? 1 : (endsWith(nom, "us") || endsWith(nom, "um")) ? 2 : 0;
      if (dcl == 1) gen = dropSuffix(nom, "a") + "ae";
      else if (dcl == 2) gen = (endsWith(nom, "us") ? dropSuffix(nom, "us") : dropSuffix(nom, "um")) + "ī";
      if (!decline(nom, gen, dcl, r.gender, case_, number, r.form)) { r.form = nom; r.kept = true; }
      r.translated = g.policy == "translate";
      return r;
    }
  }
  if (const curated::NameEntry* n = cd_.nameByEnglish(source)) {
    r.gender = n->gender;
    r.translated = n->policy == curated::NamePolicy::Translate;
    if (n->policy == curated::NamePolicy::Keep || n->declension <= 0 || n->latinGen.empty()) {
      r.form = n->latinNom;
      r.kept = n->policy == curated::NamePolicy::Keep;
      return r;
    }
    if (!decline(n->latinNom, n->latinGen, n->declension, n->gender, case_, number, r.form)) {
      r.form = n->latinNom;
      r.kept = true;
    }
    return r;
  }
  r.form = std::string(source);
  r.kept = r.guessed = true;
  return r;
}

// ---- Orderer ------------------------------------------------------------------------------------------------------
namespace {
std::vector<uint32_t> resolveWords(const lex::Lexicon& lx, const std::vector<std::string>& keys,
                                   std::initializer_list<uint8_t> pos) {
  std::vector<uint32_t> out;
  std::vector<lex::Analysis> an;
  for (const std::string& k : keys) {
    an.clear();
    lx.lookup(k, an);
    for (const lex::Analysis& a : an) {
      const lex::Lemma l = lx.lemma(a.lemma);
      if (std::find(pos.begin(), pos.end(), l.pos) == pos.end()) continue;
      if (a.flags & (lex::AltSpelling | lex::LateLatin)) continue;
      out.push_back(a.lemma);
    }
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}
}  // namespace

Orderer::Orderer(const lex::Lexicon& lx, const curated::CuratedData& cd) {
  struct Def { const char* id; std::vector<std::string> slots; };
  const Def defs[] = {
      {"order.decl", {"VOC", "CONN", "S", "IO", "O", "OBL", "ADV", "NEG", "V"}},
      {"order.copula", {"VOC", "CONN", "S", "PRED", "NEG", "V"}},
      {"order.copula.rel", {"VOC", "CONN", "S", "NEG", "V", "PRED"}},   // C28
      {"order.exist", {"CONN", "OBL", "V", "S"}},
      {"order.inf", {"O", "OBL", "INF", "V"}},
      {"order.imp.long", {"VOC", "O", "OBL", "V"}},
  };
  for (const Def& d : defs) {
    std::vector<std::string> t = cd.slotTemplate(d.id);
    templates_.emplace_back(d.id, t.empty() ? d.slots : t);
  }
  connSecond_ = resolveWords(lx, cd.conditionSet("order.conn"), {Conj, Adv, Particle});
  std::vector<std::string> before = cd.orderingList("order.adj", "demonstratives");
  for (const std::string& q : cd.orderingList("order.adj", "quantity")) before.push_back(q);
  adjBefore_ = resolveWords(lx, before, {Adj, feat::Det, Pron, Num});
  timeAdv_ = resolveWords(lx, cd.orderingList("order.adv", "time adverbs"), {Adv});
  degreeAdv_ = resolveWords(lx, cd.orderingList("order.neg.degree", "degree adverb"), {Adv});
  encliticCum_ = cd.orderingList("order.prep", "enclitic");
  std::sort(encliticCum_.begin(), encliticCum_.end());
}

const std::vector<std::string>& Orderer::slots(std::string_view id) const {
  for (const auto& t : templates_)
    if (t.first == id) return t.second;
  return empty_;
}
bool Orderer::connectorSecond(uint32_t l) const { return std::binary_search(connSecond_.begin(), connSecond_.end(), l); }
bool Orderer::adjectiveBefore(uint32_t l) const { return std::binary_search(adjBefore_.begin(), adjBefore_.end(), l); }
bool Orderer::timeAdverb(uint32_t l) const { return std::binary_search(timeAdv_.begin(), timeAdv_.end(), l); }
bool Orderer::degreeAdverb(uint32_t l) const { return std::binary_search(degreeAdv_.begin(), degreeAdv_.end(), l); }
bool Orderer::encliticCum(std::string_view k) const {
  return std::binary_search(encliticCum_.begin(), encliticCum_.end(), std::string(k));
}

}  // namespace vp::realise
