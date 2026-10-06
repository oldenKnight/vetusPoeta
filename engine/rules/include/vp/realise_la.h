// Latin realisation primitives (DESIGN.md §10.3), driven by data/curated/order_la.txt, valency_la.tsv,
// preps_en_la.tsv, names_la.tsv and emoji_la.tsv. Input: a small clause structure (LaClause / LaNP) that the frame
// builder and transfer stage (C2) fill with lemma ids; output: one sentence with byte offsets per token, the
// reasons and flags. Deterministic; scratch buffers live in the LatinRealiser and are reused between calls.
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "vp/curated.h"
#include "vp/features.h"
#include "vp/lex.h"
#include "vp/morph.h"
#include "vp/rules.h"

namespace vp::realise {

constexpr uint32_t kNone = lex::kNoLemma;

enum class ClauseType : uint8_t { Decl, Yn, Wh, Imp, Excl, Frag };
enum class Polarity : uint8_t { Pos, Neg };
enum class YnBias : uint8_t { Neutral, ExpectYes /* nōnne */, ExpectNo /* num */ };
enum class Det : uint8_t { None, Hic, Ille, Is, Iste };
// Clause roles (relative pronoun role, wh role).
enum class Role : uint8_t { None, Subject, Object, IndirectObject, Oblique, Predicate };
enum class AdvPos : uint8_t { Auto /* order.adv: time adverbs first, others before the verb */, Front, BeforeVerb, End };
enum class SubRel : uint8_t { Cause, Time, Condition, Purpose, Concession, Result, AccInf, IndirectQ, Coord };

struct LaClause;

struct LaPronoun { uint8_t person = 0, number = feat::Sg, gender = 0; bool reflexive = false; };
struct LaAdj { uint32_t lemma = kNone; uint8_t degree = 0; std::vector<uint32_t> adverbs; /* "nimis parva" */ };

struct LaNP {
  uint32_t head = kNone;          // noun / pronoun / substantive adjective lemma (kNone for names and pronoun specs)
  uint8_t number = feat::Sg;
  uint8_t gender = 0;             // 0 = from the lemma (common gender -> masculine), the name table or the pronoun
  Det det = Det::None;
  uint32_t possessive = kNone;    // meus, tuus, suus, noster, vester
  bool possContrast = false;      // order.poss: contrastive possessive goes before the noun
  std::vector<LaAdj> adjectives;
  uint32_t numeral = kNone;       // cardinal (ūnus, duo, trēs ...) or ordinal adjective
  uint32_t interrogative = kNone; // quot, quotus, quī, quantus: the NP is the wh element
  std::vector<LaNP> genitive;     // 0 or 1 genitive attribute (order.gen: after the noun)
  std::vector<LaClause> relative; // 0 or 1 relative clause (order.rel: right after the antecedent)
  bool isName = false;
  std::string name;               // source spelling of a name ("Alice"); looked up in glossary / names_la.tsv
  bool isPronoun = false;
  LaPronoun pron;                 // personal (person 1-3) or reflexive pronoun
  uint8_t case_ = 0;              // 0 = assigned by the CaseAssigner
  bool emphasis = false;          // contrast / stress: keeps a pronoun subject, hosts -ne in yes/no questions
  bool capitalise = false;        // title words ("Rēgīna Cordium")
  std::vector<LaNP> coord;        // further conjuncts: "X et Y"
  bool coordQue = false;          // enclitic.que: "pater māterque" (flexible mode only)
  std::string literal;            // unknown source word, kept verbatim (marked unknown)
};

struct LaOblique { uint32_t prep = kNone; uint8_t case_ = 0; LaNP np; bool front = false; };
struct LaAdverb { uint32_t lemma = kNone; AdvPos pos = AdvPos::Auto; };

struct LaPredicate {
  uint32_t lemma = kNone;          // the verb (the infinitive when a modal is present)
  uint8_t tense = feat::Present, mood = feat::Indicative, voice = feat::Active;
  uint8_t person = 0, number = 0;  // 0 = from the subject (3rd singular without one)
  uint32_t modal = kNone;          // possum, volō, nōlō, dēbeō, soleō, licet ...: finite modal + infinitive
  uint8_t infTense = feat::Present, infVoice = feat::Active;
};

struct LaSub {
  SubRel rel = SubRel::Cause;
  uint32_t conj = kNone;           // explicit conjunction lemma (default by relation: quia, cum, sī, ut, et ...)
  bool before = false;             // order.sub.pre: before the main clause when the source puts it first
  std::vector<LaClause> clause;    // exactly one
};

struct LaWh { uint32_t lemma = kNone; Role role = Role::None; uint8_t gender = 0; };

struct LaClause {
  ClauseType type = ClauseType::Decl;
  Polarity polarity = Polarity::Pos;
  YnBias bias = YnBias::Neutral;
  LaPredicate pred;
  bool hasSubject = false, hasObject = false, hasIndirect = false;
  LaNP subject, object, indirect;
  std::vector<LaOblique> obliques;
  std::vector<LaNP> predicative;   // copula with a noun predicate (0 or 1)
  std::vector<LaAdj> predAdj;      // copula with adjective predicate(s), joined by et
  uint8_t predGender = 0, predNumber = 0;   // agreement of a predicate without a subject ("obscūrum est")
  std::vector<LaNP> vocatives;
  std::vector<uint32_t> interjections;
  std::vector<LaAdverb> adverbs;
  std::vector<uint32_t> connectors;        // sed, autem, igitur ... (order.conn / order.conn.first)
  std::vector<uint32_t> politeness;        // quaesō: ", quaesō" at the end
  std::vector<LaSub> subs;
  LaWh wh;                                 // wh questions: interrogative pronoun/adverb (or LaNP::interrogative)
  bool existential = false;                // order.exist: "there is" (V S)
  bool exclQuam = false;                   // order.excl: "Quam mīrus hortus!"
  bool exclO = false;                      // order.excl: "Ō mē miseram!" (subject NP in the accusative)
  Role relRole = Role::None;               // set when the clause is a relative clause
  uint32_t relPrep = kNone;                // relative pronoun inside a prepositional phrase
  std::string punct;                       // source punctuation; empty = "." / "?" by type
};

struct LaSentence {
  std::string text;
  std::vector<rules::TokenView> tokens;
  std::vector<rules::Reason> reasons;
  std::vector<std::string> flags;          // "name-guessed", "name-kept", "missing-form", "from-rule", "unknown"
  void clear() { text.clear(); tokens.clear(); reasons.clear(); flags.clear(); }
};

struct RealiseOptions {
  bool macrons = true;
  bool emoji = true;            // TokenView::emoji for qualifying nouns
  bool emojiInText = false;     // also write the emoji after the noun in `text` (app view)
  int fidelity = 2;             // 1 faithful .. 3 flexible (enclitic -que only when flexible)
  const std::vector<rules::GlossaryEntry>* glossary = nullptr;
};

// One realised word before linearisation (exposed for the components and for C2).
struct Word {
  std::string form;             // display form (with macrons; Macrons strips them on output when asked)
  uint32_t lemma = kNone;
  uint32_t packed = 0;          // features of the form (0 for invariable words)
  bool fromRule = false, missing = false, unknown = false, name = false, title = false;
  std::string emoji;
  std::string punctAfter;       // "," attached to this word
  const char* rule = "";        // rule id that produced or placed it (reason)
};

// ---- components ---------------------------------------------------------------------------------------------------
// Picks lexicon cells (morph::generate: exact FEAT id, tolerant match, periphrasis), falls back to the paradigm,
// else reports missing ("[head]"). Invariable parts of speech give the headword.
class FormSelector {
 public:
  explicit FormSelector(const lex::Lexicon& lx) : lx_(lx) {}
  bool select(uint32_t lemma, const feat::Features& f, Word& out) const;   // false = missing (out.missing)
  void invariable(uint32_t lemma, Word& out) const;
  const lex::Lexicon& lexicon() const { return lx_; }
 private:
  const lex::Lexicon& lx_;
};

// Agreement features: adjective / participle / determiner with its noun, predicate with the subject, subject-verb,
// relative pronoun with its antecedent (gender, number) and its own role (case).
struct AgreeInfo { uint8_t case_ = 0, number = feat::Sg, gender = feat::M, person = 3; };
class Agreement {
 public:
  explicit Agreement(const lex::Lexicon& lx) : lx_(lx) {}
  uint8_t nounGender(const LaNP&, const curated::CuratedData&) const;          // lemma / name / pronoun gender
  AgreeInfo ofNP(const LaNP&, const curated::CuratedData&) const;              // person, number, gender of an NP
  feat::Features modifier(const AgreeInfo& head, uint8_t degree = 0) const;    // adj/det/participle features
  feat::Features finiteVerb(const AgreeInfo& subject, const LaPredicate&) const;
  feat::Features relative(const AgreeInfo& antecedent, uint8_t ownCase) const;
 private:
  const lex::Lexicon& lx_;
};

// Case of objects (valency_la.tsv frames, then sense tags), of prepositional phrases (preps_en_la.tsv) and of the
// other roles.
class CaseAssigner {
 public:
  CaseAssigner(const lex::Lexicon& lx, const curated::CuratedData& cd) : lx_(lx), cd_(cd) {}
  uint8_t objectCase(uint32_t verb) const;            // feat::Acc by default
  uint8_t prepCase(uint32_t prep, uint8_t override) const;
  void assign(LaClause&, bool accInf = false) const;  // fills every NP's case_ that is 0
 private:
  const lex::Lexicon& lx_;
  const curated::CuratedData& cd_;
};

// Names: glossary policy, then names_la.tsv (keep / decline / translate), then proper-name lemmas; unknown
// capitalised names are kept undeclined and flagged.
struct NameForm { std::string form; uint8_t gender = 0; bool kept = false, guessed = false, translated = false; };
class Names {
 public:
  explicit Names(const curated::CuratedData& cd) : cd_(cd) {}
  NameForm form(std::string_view source, uint8_t case_, uint8_t number,
                const std::vector<rules::GlossaryEntry>* glossary) const;
  // Declines a Latin name from nominative + genitive + declension (1, 2, 3); false when the endings don't fit.
  static bool decline(std::string_view nom, std::string_view gen, int declension, uint8_t gender, uint8_t case_,
                      uint8_t number, std::string& out);
 private:
  const curated::CuratedData& cd_;
};

// Word order from order_la.txt: slot templates (order.decl, order.copula, order.exist, order.inf, order.imp.long),
// connector positions (order.conn / order.conn.first), adjective exceptions (order.adj), time adverbs (order.adv),
// enclitic cum forms (order.prep).
class Orderer {
 public:
  Orderer(const lex::Lexicon& lx, const curated::CuratedData& cd);
  const std::vector<std::string>& slots(std::string_view ruleId) const;   // template, built-in default if missing
  bool connectorSecond(uint32_t lemma) const;
  bool adjectiveBefore(uint32_t lemma) const;    // demonstratives, quantity words (order.adj)
  bool timeAdverb(uint32_t lemma) const;         // order.adv
  bool encliticCum(std::string_view latinKey) const;
 private:
  std::vector<std::pair<std::string, std::vector<std::string>>> templates_;
  std::vector<uint32_t> connSecond_, adjBefore_, timeAdv_;
  std::vector<std::string> encliticCum_;
  std::vector<std::string> empty_;
};

// Negation: nōn directly before the verb unless the clause already has nūllus / nēmō / nihil / numquam.
class Negation {
 public:
  explicit Negation(const lex::Lexicon& lx);
  bool negativeWord(uint32_t lemma) const;
  bool clauseHasNegativeWord(const LaClause&) const;
 private:
  std::vector<uint32_t> neg_;
};

// Pronoun lemmas and the dropping rules of order_la.txt (pron.drop, pron.is, pron.refl).
class Pronouns {
 public:
  explicit Pronouns(const lex::Lexicon& lx);
  uint32_t personal(uint8_t person, uint8_t number, bool reflexive) const;   // ego, tū, nōs, vōs, is, sē
  uint32_t possessive(uint8_t person, uint8_t number, bool reflexive) const;  // meus ... suus
  bool dropSubject(const LaClause&) const;
 private:
  uint32_t ego_, tu_, nos_, vos_, is_, se_, meus_, tuus_, noster_, vester_, suus_;
};

// Display: macrons on/off and anceps cleanup (morph::displayForm); sentence capital; final punctuation.
struct Macrons { static std::string apply(std::string_view form, bool macrons); };
struct Punctuation {
  static std::string finalMark(const LaClause&);
  static void capitaliseFirst(std::string& text);
};
// Emoji of a noun lemma (emoji_la.tsv row whose headword equals the lemma's; never names, never non-nouns).
class Emoji {
 public:
  Emoji(const lex::Lexicon& lx, const curated::CuratedData& cd) : lx_(lx), cd_(cd) {}
  std::string_view forLemma(uint32_t lemma) const;
 private:
  const lex::Lexicon& lx_;
  const curated::CuratedData& cd_;
};

// FeatureView strings (DESIGN §9.2), the same names the CLI uses.
rules::Features featureView(uint32_t packed);

// ---- the realiser -------------------------------------------------------------------------------------------------
class LatinRealiser {
 public:
  LatinRealiser(const lex::Lexicon& lx, const curated::CuratedData& cd);
  // One clause (with its subordinates) -> one sentence. `out` is cleared first; its buffers are reused.
  void realise(const LaClause&, const RealiseOptions&, LaSentence& out);
  LaSentence realise(const LaClause& c, const RealiseOptions& o) { LaSentence s; realise(c, o, s); return s; }

  // Closed-class lemma ids resolved at construction (kNone when the lexicon lacks them; literals are used then).
  struct Closed {
    uint32_t sum = kNone, nolo = kNone, non = kNone, nonne = kNone, num = kNone, quam = kNone, o = kNone,
             et = kNone, cum = kNone, ab = kNone, ex = kNone, qui = kNone, quis = kNone, hic = kNone, hicDet = kNone,
             ille = kNone, iste = kNone, is = kNone, quia = kNone, cumConj = kNone, si = kNone, ut = kNone,
             ne = kNone, quamquam = kNone;
  };
  const Closed& closed() const { return k_; }
  const FormSelector& forms() const { return forms_; }
  const Orderer& orderer() const { return order_; }

 private:
  struct Slots;
  struct ClauseCtx {
    bool main = true, accInf = false, relative = false, suppressNon = false;
    uint8_t forceMood = 0;   // purpose clauses: subjunctive
    AgreeInfo ante;          // relative clauses: the antecedent
  };
  void clause(const LaClause&, const RealiseOptions&, std::vector<Word>& out, const ClauseCtx&);
  void np(const LaNP&, uint8_t case_, const LaClause* owner, const RealiseOptions&, std::vector<Word>& out);
  void nameWord(const LaNP&, uint8_t case_, const RealiseOptions&, Word& w);
  void verbGroup(const LaClause&, const AgreeInfo& subj, std::vector<Word>& finite, std::vector<Word>& inf,
                 const ClauseCtx&);
  void literal(uint32_t lemma, const char* fallback, Word& w, const char* rule) const;
  void finish(const RealiseOptions&, std::vector<Word>& words, LaSentence& out);

  const lex::Lexicon& lx_;
  const curated::CuratedData& cd_;
  FormSelector forms_;
  Agreement agree_;
  CaseAssigner cases_;
  Names names_;
  Orderer order_;
  Negation neg_;
  Pronouns pron_;
  Emoji emoji_;
  Closed k_;
  std::vector<Word> words_;          // scratch, reused
  std::vector<std::string> flags_;   // scratch
};

}  // namespace vp::realise
