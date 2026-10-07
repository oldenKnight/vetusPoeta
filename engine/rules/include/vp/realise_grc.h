// Greek (Attic) realisation primitives, the counterpart of realise_la.h (DESIGN.md §1.2, §10.3 mirrored; D12, D13).
// Input: a clause structure (GrcClause / GrcNP) that the frame builder and transfer stage fill with lemma ids of
// greek.vpl; output: one sentence (lower case, polytonic, final sigma, no length marks) with byte offsets per
// token, reasons and flags. Driven by data/curated/order_grc.txt, valency_grc.tsv, preps_en_grc.tsv,
// names_grc.tsv, particles_grc.tsv (GreekData) and emoji_grc.tsv / tiers_grc.tsv (CuratedData). Forms come from
// vp::grc::generate (Attic cells first) and the built-in closed-class tables; the sentence is finished by the
// sandhi pass (movable nu, οὐ/οὐκ/οὐχ, enclitic accents, grave). Deterministic; scratch buffers are reused.
#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "vp/curated.h"
#include "vp/features.h"
#include "vp/lex.h"
#include "vp/morph_grc.h"
#include "vp/realise_la.h"   // shared enums: ClauseType, Polarity, YnBias, Role, AdvPos, SubRel
#include "vp/result.h"
#include "vp/rules.h"

namespace vp::grc {

constexpr uint32_t kNone = lex::kNoLemma;
using realise::AdvPos;
using realise::ClauseType;
using realise::Polarity;
using realise::Role;
using realise::SubRel;
using realise::YnBias;

// ---- curated Greek tables ---------------------------------------------------------------------------------------
// C18 (appended): PurpInf = "purp:inf", the verb takes a bare infinitive of purpose ("δός μοι ὕδωρ πιεῖν").
enum class FrameKind : uint8_t { Acc, Gen, Dat, DatAcc, AccAcc, AccInf, Inf, Intr, Copula, Prep, ImpersAccInf,
                                 ImpersDatInf, Other, PurpInf };
struct Frame {
  FrameKind kind = FrameKind::Other;
  bool middle = false;        // "mid:" prefix: the frame of the middle voice
  std::string raw;            // "acc", "mid:dat", "prep:εἰς+acc" ...
  std::string prepKey;        // greek_key of the preposition of a Prep frame
  uint8_t prepCase = 0;
};
struct Valency { std::string key, example, note; std::vector<Frame> frames; };
struct PrepEntry {
  std::string english, context, greek, greekKey, caseRaw, note;   // greek "-" = bare case (greekKey empty)
  uint8_t case_ = 0;
  bool infinitive = false;
};
struct NameEntry {
  std::string english, nom, gen, voc, note;
  uint8_t gender = 0;
  int declension = -1;         // 1, 2, 3; 0 indeclinable; -1 not given
  curated::NamePolicy policy = curated::NamePolicy::Keep;
};
struct ParticleEntry { std::string head, key, function, english, note; bool second = false; };

class GreekData {
 public:
  // Reads valency_grc.tsv, preps_en_grc.tsv, names_grc.tsv, particles_grc.tsv, phrasebook_en_grc.tsv and
  // order_grc.txt from `dir`. A missing file is an error with a hint; a malformed line is a warning.
  static Result<GreekData> load(const std::filesystem::path& dir);
  const std::vector<curated::LoadWarning>& warnings() const { return warnings_; }

  const Valency* valency(std::string_view greekKey) const;
  uint16_t prepCases(std::string_view greekKey) const;   // bit set (1 << feat::Case); 0 = not a preposition
  uint8_t prepDefaultCase(std::string_view greekKey) const;
  const std::vector<PrepEntry>& preps() const { return preps_; }
  const std::vector<NameEntry>& names() const { return names_; }
  const NameEntry* nameByEnglish(std::string_view english) const;   // case-insensitive
  const NameEntry* nameByGreek(std::string_view greekKey) const;    // greek_key of the nominative
  const ParticleEntry* particle(std::string_view greekKey) const;
  const std::vector<ParticleEntry>& particles() const { return particles_; }
  const std::vector<curated::PhraseEntry>& phrasebook() const { return phrasebook_; }
  const std::vector<curated::OrderRule>& orderRules() const { return order_; }
  const curated::OrderRule* rule(std::string_view id) const;
  // Words inside "{...}" of a rule's condition / inside the first "(...)" after `marker` in its ordering text
  // (greek_key), and the slot template of a rule ("[VOC,] [CONN] S IO O OBL ADV [NEG] V" -> VOC CONN S ...).
  std::vector<std::string> conditionSet(std::string_view ruleId) const;
  std::vector<std::string> orderingList(std::string_view ruleId, std::string_view marker) const;
  std::vector<std::string> slotTemplate(std::string_view ruleId) const;

 private:
  std::vector<Valency> valency_;
  std::vector<PrepEntry> preps_;
  std::vector<std::pair<std::string, uint16_t>> prepCases_;
  std::vector<NameEntry> names_;
  std::vector<ParticleEntry> particles_;
  std::vector<curated::PhraseEntry> phrasebook_;
  std::vector<curated::OrderRule> order_;
  std::vector<curated::LoadWarning> warnings_;
  friend struct GreekLoader;
};

// ---- clause structure ---------------------------------------------------------------------------------------------
enum class Demonstrative : uint8_t { None, Houtos /* οὗτος */, Ekeinos /* ἐκεῖνος */ };

struct GrcClause;
struct GrcPronoun { uint8_t person = 0, number = feat::Sg, gender = 0; };
struct GrcAdj { uint32_t lemma = kNone; uint8_t degree = 0; std::vector<uint32_t> adverbs; /* "πάνυ μικρά" */ };

struct GrcNP {
  uint32_t head = kNone;          // noun / substantive adjective / pronoun lemma (kNone for names and pronoun specs)
  uint8_t number = feat::Sg;
  uint8_t gender = 0;             // 0 = from the lemma (common gender -> masculine), the name table or the pronoun
  bool definite = false;          // takes the article (order.art); names: the article in narrative
  bool forceArticle = false;      // keeps the article even as a predicate (identifying predicate, faithful)
  Demonstrative dem = Demonstrative::None;   // οὗτος ὁ ἀνήρ (predicate position, implies the article)
  uint8_t possPerson = 0, possNumber = feat::Sg, possGender = 0;   // possessor pronoun: μου σου αὐτοῦ ἡμῶν ...
  bool possEmphatic = false;      // ἐμός / σός / ἡμέτερος / ὑμέτερος between article and noun
  std::vector<GrcAdj> adjectives;
  uint32_t numeral = kNone;       // εἷς δύο τρεῖς ... (before the noun)
  uint32_t interrogative = kNone; // πόσος / ποῖος / τίς: the NP is the wh element (first in the clause)
  uint32_t quantifier = kNone;    // πᾶς / ὅλος: predicate position before the article
  std::vector<GrcNP> genitive;    // 0 or 1 genitive attribute (after the noun, own article)
  std::vector<GrcClause> relative;   // 0 or 1 relative clause
  bool isName = false;
  std::string name;               // source spelling of a name ("Alice")
  bool isPronoun = false;
  GrcPronoun pron;                // personal pronoun (person 1-3)
  uint8_t case_ = 0;              // 0 = assigned
  bool emphasis = false;          // keeps a pronoun subject; emphatic pronoun forms
  std::vector<GrcNP> coord;       // further conjuncts: "X καὶ Y"
  std::string literal;            // unknown source word kept verbatim (marked unknown)
  // C16 (additive): attributive adjectives of an indefinite NP before the noun ("εἰς βαθὺν βόθρον", the EN/ES
  // transfer's default; the realiser's own default stays after the noun: "ἀνὴρ ἀγαθός"); the genitive attribute
  // before the noun without its article ("Ἄρεως ἡμέρα").
  bool adjFirst = false;
  bool genFirst = false;
};

struct GrcOblique { uint32_t prep = kNone; uint8_t case_ = 0; GrcNP np; bool front = false;
                    bool end = false;   /* C16: after the verb ("ἐφυτεύσαμεν ἁμαρτόντες") */ };
struct GrcAdverb { uint32_t lemma = kNone; AdvPos pos = AdvPos::Auto; };

struct GrcPredicate {
  uint32_t lemma = kNone;          // the verb (the infinitive when a modal is present)
  uint8_t tense = feat::Present;   // Present, Imperfect, Future, Aorist, Perfect, Pluperfect; imperatives and
                                   // subjunctives: Present or Aorist (aspect)
  uint8_t mood = feat::Indicative;
  uint8_t voice = 0;               // 0 = active, or middle for middle-form lemmas; Middle for reflexive senses
  uint8_t person = 0, number = 0;  // 0 = from the subject
  uint32_t modal = kNone;          // δύναμαι, βούλομαι, ἐθέλω, μέλλω, δεῖ, ἔξεστι ...: finite modal + infinitive
  uint8_t infTense = feat::Present;   // Present or Aorist infinitive
  uint8_t infVoice = 0;
  uint8_t modalTense = 0;          // tense of the modal (0 = `tense`)
};

struct GrcSub {
  SubRel rel = SubRel::Cause;
  uint32_t conj = kNone;           // explicit conjunction (default by relation: ὅτι, ἐπεί, εἰ, ἵνα, ὥστε, καί)
  bool before = false;
  bool otherwise = false;          // C16: "or (else)" before a counterfactual: "εἰ δὲ μή, οὐκ ἂν ἐνθάδε ἦσθα"
  std::vector<GrcClause> clause;   // exactly one
  // C18 (additive): `participle` = a circumstantial participle (the clause's verb as a nominative participle agreeing
  // with the main clause's subject; the clause has no subject of its own), placed after the main subject or first:
  // "ὁ ποιμὴν ἰδὼν τὸν λύκον ἔφυγεν"; pred.tense Present (simultaneous) or Aorist (prior). `finite` = ὥστε + the
  // indicative (an actual result, negation οὐ) instead of the infinitive. `noConj` = no conjunction: the clause
  // carries a postpositive particle of its own (γάρ) and follows a comma.
  bool participle = false;
  bool finite = false;
  bool noConj = false;
};

struct GrcWh { uint32_t lemma = kNone; Role role = Role::None; uint8_t gender = 0, number = 0; };

struct GrcClause {
  ClauseType type = ClauseType::Decl;
  Polarity polarity = Polarity::Pos;
  YnBias bias = YnBias::Neutral;
  bool ara = false;                       // yes/no question with ἆρα
  GrcPredicate pred;
  bool hasSubject = false, hasObject = false, hasIndirect = false;
  GrcNP subject, object, indirect;
  std::vector<GrcOblique> obliques;
  std::vector<GrcNP> predicative;         // copula with a noun predicate (0 or 1): no article
  std::vector<GrcAdj> predAdj;            // copula with adjective predicate(s): "μικρά ἐστι καὶ λευκή"
  uint8_t predGender = 0, predNumber = 0; // predicate without a subject
  std::vector<GrcNP> vocatives;           // ὦ + vocative
  std::vector<uint32_t> interjections;    // οἴμοι, φεῦ
  std::vector<GrcAdverb> adverbs;
  std::vector<uint32_t> connectors;       // particles: δέ γάρ οὖν μέν γε (second), ἀλλά καί (first)
  std::vector<uint32_t> politeness;       // ἀντιβολῶ: ", ἀντιβολῶ" at the end (a verb is put in the 1st singular)
  std::vector<GrcSub> subs;
  GrcWh wh;
  bool existential = false;               // "there is" / location-first: V S, orthotone ἔστι
  bool exclHos = false;                   // ὡς + adjective + subject: "ὡς θαυμαστὸς ὁ κῆπος!"
  Role relRole = Role::None;              // relative clause: role of ὅς
  uint32_t relPrep = kNone;
  std::string punct;                      // source punctuation; empty = "." / ";" / "!" by type
  // C16 (additive)
  bool an = false;                        // modal particle ἄν (counterfactual "would"): after the negation or the
                                          // first word ("οὐκ ἂν ἐνθάδε ἦσθα")
  bool verbFirst = false;                 // V S order without the existential accent ("πρὶν ἥκειν αὐτήν")
};

struct GrcSentence {
  std::string text;
  std::vector<rules::TokenView> tokens;
  std::vector<rules::Reason> reasons;
  std::vector<std::string> flags;         // "name-guessed", "name-kept", "missing-form", "from-rule", "unknown"
  void clear() { text.clear(); tokens.clear(); reasons.clear(); flags.clear(); }
};

struct GrcOptions {
  bool emoji = true;
  bool emojiInText = false;
  int fidelity = 2;                       // 1 faithful .. 3 flexible
  bool elision = false;                   // off by default (D13 note in order_grc.txt)
  const std::vector<rules::GlossaryEntry>* glossary = nullptr;
};

// One realised word before the sandhi pass (exposed for tests and C2).
struct GWord {
  std::string form;
  uint32_t lemma = kNone;
  uint32_t packed = 0;
  bool enclitic = false, proclitic = false, movableNu = false, existential = false, interrogative = false;
  bool fromRule = false, missing = false, unknown = false, name = false;
  std::string emoji;
  std::string punctAfter;
  const char* rule = "";
};

// Declines a Greek name in the singular from nominative + genitive (names_grc.tsv declension 1, 2, 3; 0 =
// indeclinable). `voc` overrides the vocative. False for the plural or endings that do not fit.
bool declineName(std::string_view nom, std::string_view gen, int declension, uint8_t gender, uint8_t case_,
                 std::string_view voc, std::string& out);

// Tense of an English verb phrase in Greek (order_grc.txt tense.*): past events -> aorist, past states /
// background / progressive -> imperfect, perfect with a present result -> perfect, else aorist.
uint8_t mapTense(uint8_t englishTense, bool progressive, bool state, bool perfectResult);

class GreekRealiser {
 public:
  GreekRealiser(const lex::Lexicon& lx, const curated::CuratedData& cd, const GreekData& gd);
  void realise(const GrcClause&, const GrcOptions&, GrcSentence& out);
  GrcSentence realise(const GrcClause& c, const GrcOptions& o) { GrcSentence s; realise(c, o, s); return s; }

  // Closed-class lemma ids resolved at construction (kNone when the lexicon lacks them; literals are used then).
  struct Closed {
    uint32_t eimi = kNone, art = kNone, ou = kNone, me = kNone, ara = kNone, kai = kNone, o = kNone, hos = kNone,
             hosRel = kNone, ego = kNone, su = kNone, hemeis = kNone, humeis = kNone, autos = kNone, houtos = kNone,
             ekeinos = kNone, tis = kNone, tisIndef = kNone, hoti = kNone, epei = kNone, hote = kNone, ei = kNone,
             ean = kNone, hina = kNone, hopos = kNone, hoste = kNone, hos2 = kNone /* ὡς */, emos = kNone,
             sos = kNone, hemeteros = kNone, humeteros = kNone, dei = kNone, exesti = kNone, phemi = kNone,
             oudeis = kNone, medeis = kNone;
  };
  const Closed& closed() const { return k_; }
  // Case a verb gives its object (valency_grc.tsv; `middle` selects mid: frames), Acc by default.
  uint8_t objectCase(uint32_t verb, bool middle) const;
  uint8_t prepCase(uint32_t prep, uint8_t override) const;

 private:
  struct Agree { uint8_t case_ = 0, number = feat::Sg, gender = feat::M, person = 3; };
  struct Ctx {
    bool main = true, relative = false, infinitival = false, negMe = false;
    uint8_t forceMood = 0;     // ἵνα / ἐάν: subjunctive
    Agree ante;                // relative clauses: the antecedent
    bool participle = false;   // C18: the verb is a participle agreeing with `ante` (the main clause's subject)
  };
  Agree ofNP(const GrcNP&) const;
  uint8_t nounGender(const GrcNP&) const;
  void np(const GrcNP&, uint8_t case_, bool afterPrep, bool predicate, const GrcOptions&, std::vector<GWord>& out);
  void nameWord(const GrcNP&, uint8_t case_, const GrcOptions&, GWord& w);
  void pronounWord(const GrcNP&, uint8_t case_, bool afterPrep, GWord& w);
  void adjWord(const GrcAdj&, const Agree&, const char* rule, std::vector<GWord>& out);
  void verbGroup(const GrcClause&, const Agree& subj, std::vector<GWord>& fin, std::vector<GWord>& inf, const Ctx&);
  void form(uint32_t lemma, const Features& f, GWord& w, const char* rule);
  void literal(uint32_t lemma, const char* fallback, GWord& w, const char* rule) const;
  void clause(const GrcClause&, const GrcOptions&, std::vector<GWord>& out, const Ctx&);
  void finish(const GrcOptions&, std::vector<GWord>& words, const std::string& finalPunct, GrcSentence& out);
  bool middleLemma(uint32_t lemma) const;
  bool timeAdverb(uint32_t lemma) const;
  bool beforeNoun(uint32_t adjLemma) const;
  bool negativeWord(uint32_t lemma) const;

  const lex::Lexicon& lx_;
  const curated::CuratedData& cd_;
  const GreekData& gd_;
  Closed k_;
  std::vector<std::string> secondKeys_, firstKeys_, timeAdv_, beforeNoun_, negKeys_;
  std::vector<std::string> negOut_;   // C21 (neg.verb): adverbs that never stand between οὐ / μή and the verb
  std::vector<GWord> words_;
  std::vector<std::string> flags_;
  std::vector<SandhiWord> sw_;
};

}  // namespace vp::grc
