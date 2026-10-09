// Source analysis for the rule engine (DESIGN.md §10.1): cue -> sentence mapping, tokenisation with the contraction
// table, tagging + parsing through vp::nlp, lemmatisation through the English/Spanish lexicon, the phrasebook
// pre-pass and the dependency-to-frame mapping. Output: one SemSentence per source sentence, made of units (fixed
// phrasebook pieces and clause frames) in source order. Language neutral from here on: English and Spanish fill the
// same structures. Deterministic; nothing here throws across the module boundary (callers catch at the engine).
#pragma once
#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "vp/curated.h"
#include "vp/lex.h"
#include "vp/nlp.h"

namespace vp::frame {

enum class SrcLang : uint8_t { En, Es };

// ---- cue -> sentence mapping (§10.1 item 1) -------------------------------------------------------------------------
enum class CueKind : uint8_t { Speech, Song, Nonverbal, Empty };

// One part of a sentence that comes from one cue: [start, end) are byte offsets in SourceSentence::text.
struct CuePart { size_t cue = 0; int start = 0, end = 0; };

struct SourceSentence {
  std::string text;              // the sentence (speech: joined across cues with one space; song: text between ♪)
  CueKind kind = CueKind::Speech;
  std::vector<CuePart> parts;    // in order; more than one when the sentence spans cues (split points = parts[i].start)
  bool dash = false;             // started with a speaker dash ("- ")
  std::string prefix, suffix;    // song: the ♪ marks around the text; nonverbal: the brackets ("[", "]")
};

// Classifies cues and joins them into sentences: a speech cue that ends without terminal punctuation continues into
// the next speech cue (unless that one starts with a speaker dash); speaker dashes start new sentences; cues with ♪
// are songs; text entirely in [...] or (...) is nonverbal (bracket groups inside a speech cue become nonverbal
// sentences of their own). Abbreviations (Mr. Mrs. Dr. St. Sr. Sra.) do not end a sentence.
std::vector<SourceSentence> mapSentences(const std::vector<std::string>& cueTexts);
// C30: with `narrative` (the Latin engine) narrative prose cut mid-sentence stays one sentence: direct speech goes on
// with its reporting frame ("Where is my goat?" cried the old woman. / "Anna!" | she cried.), a quotation still open
// continues into the next cue, a cue ending at a comma continues into a next cue that starts in lower case (not with a
// coordinator), and a cue that is only a connector ("However,") continues. Without it: the behaviour of C15-C28.
std::vector<SourceSentence> mapSentences(const std::vector<std::string>& cueTexts, bool narrative);
// True when the text ends a sentence (. ! ? … possibly followed by closing quotes/brackets).
bool endsSentence(std::string_view text);

// ---- semantic frame (§10.1 item 5) ---------------------------------------------------------------------------------
enum class Kind : uint8_t { Decl, Yn, Wh, Imp, Excl, Frag, Nonverbal, Song };
enum class Tense : uint8_t { Present, Past, Future };
enum class Aspect : uint8_t { Simple, Progressive, Perfect };
enum class Modality : uint8_t { None, Can, Must, May, Want, Should, Will, Let };
enum class Voice : uint8_t { Active, Passive };
enum class SrcMood : uint8_t { Indicative, Subjunctive, Conditional };
enum class Relation : uint8_t { Cause, Time, Condition, Purpose, Concession, Relative, Complement, Result, Manner, Coord };
enum class Role : uint8_t { None, Subject, Object, IndirectObject, Oblique, Predicate, Adverb, Determiner };

struct SemPronoun {
  uint8_t person = 0;            // 1..3
  uint8_t number = 0;            // 1 singular, 2 plural, 0 unknown ("you")
  uint8_t gender = 0;            // vp::feat::Gender (M, F, N), 0 unknown
  bool reflexive = false;        // myself, se
  bool emphatic = false;         // "I myself", "you too"
};

struct SemAdj {
  std::string lemma;             // source lemma ("small")
  int token = -1;
  std::vector<std::string> adverbs;   // "very", "too"
  std::vector<int> advTokens;
  uint8_t degree = 0;            // vp::feat::Degree (0 positive)
  // C17: a verb's participle used as an adjective ("a lighted match", "the sleeping dog"): `lemma` is the verb;
  // 1 = past participle (passive), 2 = present participle (active); 0 = an ordinary adjective
  uint8_t participle = 0;
};

struct SemFrame;

struct SemNP {
  std::string head;              // source lemma; names keep the written form ("Alice")
  std::string surface;           // the head as written
  int token = -1;                // head token (index into SemSentence::tokens), -1 for none
  uint8_t number = 1;            // 1 sg, 2 pl
  bool definite = false;
  std::string determiner;        // lower-case source determiner: the a this that these those some any no every all ...
  std::vector<SemNP> possessor;  // 0 or 1 ("my" -> pronoun NP; "the queen's" -> NP)
  std::vector<SemAdj> adjectives;
  std::string numeral;           // "two", "6"
  int numeralValue = 0;          // 0 unknown
  bool ordinal = false;          // numeral is an ordinal ("six o'clock" -> sexta)
  std::vector<SemNP> genitive;   // "of" attributes
  std::vector<SemFrame> relative;   // 0 or 1 relative clause
  bool isName = false;
  bool title = false;            // capitalised common noun used as a title ("Queen of Hearts")
  bool isPronoun = false;
  SemPronoun pron;
  std::string pronLemma;         // closed-class pronoun as written (lower): "everyone", "this", "nobody", "one"
  std::vector<SemNP> coord;      // further conjuncts
  std::string coordConj;         // "and", "or"
  bool negative = false;         // nobody, nothing, "no X"
  bool interrogative = false;    // who, what, "which way", "how many"
  std::string wh;                // the wh word when interrogative
  bool literalUnknown = false;   // could not be analysed (kept verbatim)
  uint8_t srcGender = 0;         // grammatical gender of the source noun (Spanish), vp::feat::Gender M/F, 0 unknown
  std::vector<int> tokens;       // all tokens of the NP
  // C22: an apposition between commas ("Edwin and Paul, the Dukes of Rome, fought ..."): same case, after the NP
  std::vector<SemNP> apposition;
  // C30: the name the noun is called by ("a girl called Anna" -> puella nōmine Anna; "her dog, whose name was Rufus"
  // -> canem suum, cui nōmen erat Rūfus): 0 or 1 NP; `calledRel` for the relative form, `calledPast` its tense
  std::vector<SemNP> called;
  bool calledRel = false, calledPast = false;
};

struct SemOblique { std::string prep; SemNP np; int token = -1; bool front = false; };

struct SemPredicate {
  std::string lemma;             // main verb lemma ("be" for copula clauses)
  std::string particle;          // phrasal particle ("away", "back", "off", "up")
  std::string prepVerb;          // preposition that turns its object into the verb's object ("wait for", "look at")
  int token = -1;
  Tense tense = Tense::Present;
  Aspect aspect = Aspect::Simple;
  Modality modality = Modality::None;
  Voice voice = Voice::Active;
  SrcMood mood = SrcMood::Indicative;
  bool habitual = false;         // "used to"
  bool pastModal = false;        // could / was able to
  bool deliberative = false;     // "what shall I do?"
  bool ellipsis = false;         // "This one does." / "This one can.": the verb is the previous clause's
  bool impersonal = false;       // expletive "it" subject ("it doesn't matter", "it is raining")
  std::string complementVerb;    // xcomp verb with the modal or catenative verb ("want to go": lemma=want, complement=go)
  int complementToken = -1;
  std::vector<int> auxTokens;
  // A phrasebook row of register "vp" matched on this verb and its complements ("play cards" -> chartīs lūdere):
  // the Latin (complement words + infinitive) replaces the verb and the complements the row covers.
  std::string fixedLatin;
  int fixedEntry = -1;
  int repeatToken = -1;          // C30: "They walked and walked": the second token of the verb said twice (ambulābant et
                                 // ambulābant); -1 none
};

struct SemAdverb {
  std::string lemma; int token = -1; bool front = false;
  bool ellipticWh = false;       // a clause-final wh word standing for an indirect question ("I don't care where")
};

struct SemSub;
struct SemWh { std::string word; Role role = Role::None; int token = -1; };

struct SemFrame {
  Kind type = Kind::Decl;
  bool negative = false;
  bool expectYes = false;        // "don't you ...?", tag questions
  bool hasPred = false;
  SemPredicate pred;
  bool hasSubject = false, hasObject = false, hasIndirect = false;
  SemNP subject, object, indirectObject;
  std::vector<SemOblique> obliques;
  bool copula = false, existential = false;
  std::vector<SemNP> predicative;     // copula + noun
  std::vector<SemAdj> predAdj;        // copula + adjective(s)
  std::vector<SemNP> vocatives;
  std::vector<std::string> interjections;
  std::vector<SemAdverb> adverbs;
  std::vector<std::string> discourse;     // well, oh, now (discourse markers)
  std::vector<std::string> connectors;    // clause-initial and / but / so / then / or
  std::vector<SemSub> subordinate;
  SemWh wh;
  bool exclQuam = false;                  // "what a ...!", "how ...!"
  bool imperativePlural = false;          // addressee count > 1 ("everyone", "you all")
  std::string punct;                      // final punctuation of the clause in the source ("." "?" "!")
  std::vector<int> tokens;                // every token of the clause
  bool implicitSubject = false;           // pro-drop subject taken from the verb (Spanish) or the imperative
  // C17: object complement of a factitive verb ("make him king", "make me braver", "call me a fool")
  std::vector<SemNP> objComplement;
  std::vector<SemAdj> objComplementAdj;
  // C17: a participle or adjective phrase after a comma that describes the subject (or the predicate noun): "I am only
  // a Scarecrow, stuffed with straw.", "I am a Cowardly Lion, afraid of everything." Each is a Frag frame with one
  // predAdj (SemAdj::participle for participles) and its obliques.
  std::vector<SemFrame> secondary;
  // C30: a coordinated clause without a subject of its own shares the first clause's ("The fisherman smiled and said
  // that he would ...": the fisherman says it): 0 or 1 NP, never realised, only read for reference
  std::vector<SemNP> inheritedSubject;
};

struct SemSub { Relation relation = Relation::Cause; std::string marker; bool before = false; std::vector<SemFrame> frame; };

// ---- phrasebook (§10.1 item 4) -------------------------------------------------------------------------------------
// WH: a wh word and the rest of its clause ("that depends on {WH}" -> "where you want to go"), realised as an
// indirect question; the Latin side may ask for the subjunctive with {1:subj}.
enum class SlotKind : uint8_t { NP, Name, VP, Adj, Num, Wh };
struct PhraseSlot {
  SlotKind kind = SlotKind::NP;
  int first = 0, last = 0;       // token range [first, last]
  SemNP np;                      // NP / NAME / NUM slots
  std::vector<SemFrame> vp;      // VP slot: the clause frame (no subject)
  SemAdj adj;                    // ADJ slot
  std::vector<SemFrame> wh;      // WH slot: the clause (type wh)
};
struct PhraseMatch {
  int entry = -1;                // index into CuratedData::phrasebook()
  int first = 0, last = 0;       // token range [first, last]
  std::string pattern, latin, reg, note;
  uint8_t tier = 0;
  std::vector<PhraseSlot> slots; // in slot-number order ({1}, {2} ...)
};

// One compiled phrasebook (patterns with contractions expanded, alternatives and optional groups).
class Phrasebook {
 public:
  void build(const std::vector<curated::PhraseEntry>& entries, const std::vector<curated::PairEntry>& contractions);
  // Longest match starting at token `from` (patterns are matched against each token's lower form and lemma);
  // slots are filled with token ranges only. Ties: the earlier table row wins. C23: `classical` skips the rows marked
  // eccl (curated::PhraseEntry::eccl).
  bool match(const std::vector<nlp::Token>& toks, int from, PhraseMatch& out, bool classical = false) const;
  size_t size() const { return pats_.size(); }

 private:
  struct Elem { std::vector<std::string> words; bool optional = false; int slot = -1; SlotKind kind = SlotKind::NP; };
  struct Pattern { std::vector<Elem> elems; int entry = -1; bool eccl = false; };
  bool matchFrom(const std::vector<nlp::Token>& toks, const Pattern& p, size_t ei, int at,
                 std::vector<PhraseSlot>& slots, int& end) const;
  std::vector<Pattern> pats_;
};

// ---- sentence analysis result ----------------------------------------------------------------------------------------
struct Unit {
  enum Type : uint8_t { Clause, Phrase } type = Clause;
  SemFrame frame;                // Clause
  PhraseMatch phrase;            // Phrase
  int first = 0, last = -1;      // token range covered (inclusive)
  bool vocative = false;         // Clause holding a bare NP addressed to someone ("..., child?")
  std::string sepAfter;          // source punctuation between this unit and the next ("," ";" ":" or "")
};

// Why a source token does not need a Latin counterpart (A7 bookkeeping).
enum class Drop : uint8_t { No, Punct, Article, Aux, Marker, Particle, Phrase, Discourse, Copula, Other };

struct SemSentence {
  SrcLang lang = SrcLang::En;
  std::string text;                     // the sentence as given
  std::vector<nlp::Token> tokens;       // expanded tokens; start/end are offsets into `text`
  std::vector<Drop> drop;               // per token
  std::vector<Unit> units;
  std::string finalPunct;               // ".", "?", "!", "...", "" (from the source)
  bool question = false;
  // Fallbacks the analysis used (confidence Check, DESIGN 10.4): "retag" (a verb the tagger missed), "reroot"
  // (a clause hung on a noun), "clause-repair" (a clause buried under an adverb/oblique relabelled), "split"
  // (re-analysed as separate clauses), "simplified" (discourse words dropped), "no-verb" (a fragment with a verb).
  std::vector<std::string> repairs;
  // C15: constructions the analysis renders by a rule of thumb (confidence Check, never OK): "contact-relative" (a
  // relative clause without a relative word: "the arts I know of"), "noun-infinitive" ("no right to take"),
  // "purpose-guess" (a to-infinitive read as purpose), "light-verb" ("make a visit"), "phrase-order" (a phrasebook
  // phrase placed after its clause), "participle-phrase" ("a Scarecrow, stuffed with straw").
  std::vector<std::string> doubts;
  void doubt(const char* what) {
    if (std::find(doubts.begin(), doubts.end(), what) == doubts.end()) doubts.emplace_back(what);
  }
  // C23: the analysis ran with Options::latinity "classical": phrasebook rows marked eccl were not matched
  bool classical = false;
  void clear() { text.clear(); tokens.clear(); drop.clear(); units.clear(); finalPunct.clear(); question = false;
                 repairs.clear(); doubts.clear(); classical = false; }
};

// ---- frame builder -----------------------------------------------------------------------------------------------------
class FrameBuilder {
 public:
  // `pipeline` may be null (then every sentence becomes one fragment of unknown words); `srcLex` may be null (the nlp
  // rule lemmatiser is used); `cd` gives the contraction table and the phrasebook.
  FrameBuilder(SrcLang lang, const nlp::Pipeline* pipeline, const lex::Lexicon* srcLex, const curated::CuratedData& cd);

  // Full analysis of one sentence. C23: `classical` (Options::latinity "classical") leaves out the phrasebook rows
  // marked eccl (Medieval / ecclesiastical renderings).
  void analyse(std::string_view sentence, SemSentence& out, bool classical = false) const;
  // Parser-failure test (C2b): a clause unit without a predicate although the unit holds a verb, a fragment made of
  // a verb, or a dependent clause the parser could not attach. The engine then retries on simpler pieces.
  static bool troubled(const SemSentence& s);
  // Split points for that retry: byte offsets in the sentence where a new clause starts after ", or" / ", and" /
  // ", but" / ";" (the conjunction stays with the second piece). Empty when there is none.
  static std::vector<size_t> splitPoints(std::string_view sentence);

  // Pieces (exposed for tests).
  // Tokenise + contraction expansion; every expanded token keeps the byte range of the original word.
  void tokenize(std::string_view sentence, std::vector<nlp::Token>& out) const;
  // Lexicon lemma of an analysed token (upos + feats set); falls back to vp::nlp::ruleLemma.
  std::string lemmaOf(const nlp::Token& t) const;
  const Phrasebook& phrasebook() const { return book_; }
  SrcLang lang() const { return lang_; }

 private:
  struct Ctx;
  void buildUnits(SemSentence& s) const;
  void buildClause(Ctx& c, int head, SemFrame& f) const;
  void buildNP(Ctx& c, int head, SemNP& np) const;
  void fillSlot(Ctx& c, PhraseSlot& slot) const;
  void repairTree(SemSentence& s) const;
  bool segmentParse(std::vector<nlp::Token>& tk) const;   // C15: discourse words, vocatives, parentheticals apart
  void contractionContext(std::string_view sentence, std::vector<nlp::Token>& tk) const;   // C19: 'd = had, 's = has

  SrcLang lang_;
  const nlp::Pipeline* nlp_;
  const lex::Lexicon* lex_;
  const curated::CuratedData& cd_;
  Phrasebook book_;
  std::vector<std::pair<std::string, std::vector<std::string>>> contractions_;   // sorted by form
};

// Small helpers shared with transfer and tests.
const char* kindName(Kind k);
const char* tenseName(Tense t);
const char* aspectName(Aspect a);
const char* modalityName(Modality m);
const char* relationName(Relation r);
const char* roleName(Role r);
// One-line description of a frame for tests and debug output:
// "decl pred=know tense=present aspect=simple mod=none neg subj=I(pron1) obj=song sub=cause:..."
std::string describe(const SemFrame& f);
std::string describe(const SemSentence& s);

}  // namespace vp::frame
