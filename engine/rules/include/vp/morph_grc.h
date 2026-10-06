// Greek morphology helpers over the lexicon and C1's morph (DESIGN.md §4, §5, §6; D13 Attic, polytonic, final sigma,
// no macrons). Three parts:
//  * Forms: Attic-first cell selection (cells carrying the Attic extra bit win; a plain cell whose analyses are all
//    flagged non-Attic is never chosen when another cell fits), contracted cells for contract verbs (the tables hold
//    both; the contracted Attic form wins), voice fallback for middle-form lemmas (-μαι), οἶδα-type perfects used
//    for the present, cell clean-up (length marks, an article prefix inside a cell, the movable-nu marker "(ν)",
//    word-final σ). Augment and reduplication always come from the tables; the rule paradigm (`fromRule`) covers
//    only regular 2nd-declension nouns and 1st/2nd-class adjectives of lemmas without a table. Closed classes
//    (article, personal / demonstrative / relative / interrogative / indefinite pronouns, οὐδείς, low numerals,
//    πᾶς) come from built-in Attic tables.
//  * Accents: syllable nuclei, accent position and type, the enclitic rules applied to the host, the grave rule,
//    and the sentence-level sandhi (movable nu, οὐ/οὐκ/οὐχ, ἐκ/ἐξ, optional elision; crasis is never produced).
//  * Analysis: morph::analyseGreek plus closed-class readings and the Attic filter (non-Attic analyses dropped when
//    an Attic one exists).
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "vp/features.h"
#include "vp/lex.h"
#include "vp/morph.h"

namespace vp::grc {

using feat::Features;

// ---- text -----------------------------------------------------------------------------------------------------------
// Length marks (U+0304/U+0306, also inside precomposed letters) removed, NFC.
std::string stripLength(std::string_view s);
// A word-final σ (before a non-letter or the end) written as ς.
std::string finalSigma(std::string_view s);
// The display form of a lexicon string (headword or analysis display): stripLength + finalSigma + NFC.
std::string display(std::string_view s);
// A table cell to a single display word: drops a leading article word ("τῆς ἀνθρώπου" -> "ἀνθρώπου"), the
// movable-nu marker ("ἐστῐ́(ν)" -> "ἐστί", *movableNu = true), length marks; final sigma. Empty when the cell is
// not one word (periphrastic perfects, notes).
std::string cleanCell(std::string_view cell, bool* movableNu = nullptr);
// Lower-case, accents and breathings removed (greek_bare) — for comparisons only.
std::string bare(std::string_view s);
bool startsWithVowel(std::string_view word);           // after breathing marks: α ε η ι ο υ ω (any case)
bool startsWithRough(std::string_view word);           // rough breathing on the first syllable (or initial ῥ)
bool isGreekWord(std::string_view word);                // contains a Greek letter

// ---- accents --------------------------------------------------------------------------------------------------------
enum class Accent : uint8_t { None, Acute, Grave, Circumflex };
struct AccentInfo {
  int syllables = 0;      // vowel nuclei (diphthongs count once)
  int accents = 0;        // accent marks in the word (acute, grave, circumflex)
  int position = -1;      // syllable of the LAST accent counted from the end: 0 ultima, 1 penult, 2 antepenult
  Accent type = Accent::None;
  int firstPosition = -1; // syllable of the first accent from the end (differs when an enclitic added a second)
  Accent firstType = Accent::None;
};
AccentInfo accentOf(std::string_view word);
// Adds an acute on the ultima (on the second vowel of a diphthong, after any breathing). No-op when the ultima
// already carries an accent.
std::string addUltimaAcute(std::string_view word);
// Acute on the ultima -> grave, and grave on the ultima -> acute. Other accents untouched.
std::string ultimaToGrave(std::string_view word);
std::string ultimaToAcute(std::string_view word);
// Removes every acute / grave / circumflex (breathings, iota subscript and diaeresis stay).
std::string stripAccents(std::string_view word);
// Accent of an orthotone disyllabic enclitic on its ultima: acute (τινός, ἐστί), circumflex for τινῶν.
std::string encliticAccented(std::string_view word);

// Closed lists (by the bare, lower-case form; the accented forms of a list member are recognised too).
bool isProclitic(std::string_view word);   // ὁ ἡ οἱ αἱ ἐν εἰς ἐς ἐκ ἐξ εἰ ὡς οὐ οὐκ οὐχ
// Enclitic by form: the unaccented (or ultima-accented) forms of τις, μου μοι με σου σοι σε, εἰμί (present
// indicative except εἶ), φημί (present indicative except φῄς), γε τε που ποι ποθεν ποτε πως πω τοι νυν(enclitic).
// Accented interrogatives (τίς, τί, τίνος, ποῦ, πότε, πῶς ...) are not enclitic.
bool isEncliticForm(std::string_view word);
bool isPostpositive(std::string_view word);   // ἄν γάρ γε δέ δή μέν μήν οὖν τε τοι τοίνυν δήπου (second position)

// One word of a sentence for the sandhi pass.
struct SandhiWord {
  std::string form;            // lexical accentuation (acute, not grave, on an oxytone)
  bool enclitic = false;       // set by the caller (or by isEncliticForm when `autoEnclitic`)
  bool proclitic = false;
  bool movableNu = false;      // the form may take ν (λύουσι, ἔλυσε, ἐστί, πᾶσι)
  bool existential = false;    // ἐστί meaning "there is / exists": orthotone ἔστι(ν)
  bool interrogative = false;  // τίς / τί: never grave
  bool noGrave = false;
  std::string punctAfter;      // punctuation written right after the word ("," "." ";" "·" "!")
};
struct SandhiOptions {
  bool elision = false;        // δέ ἀλλά τε οὐδέ μηδέ ἀπό ἐπί κατά μετά παρά διά ὑπό ἀντί before a vowel
  bool autoEnclitic = true;    // mark enclitics by isEncliticForm when the caller did not
};
// Applies, in order: οὐ -> οὐκ / οὐχ (before vowels, smooth / rough) and οὔ before a stop; ἐκ -> ἐξ before
// vowels; movable nu (before a vowel, a sentence end . ; ! · ? and a comma); optional elision; the enclitic accent
// rules (host oxytone keeps the acute; perispomenon unchanged; paroxytone: a disyllabic enclitic keeps its accent
// on the ultima; proparoxytone / properispomenon: the host adds an acute on the ultima; proclitic / enclitic host:
// acute on its ultima; ἐστί orthotone ἔστι at the start, after οὐκ μή εἰ ὡς καί ἀλλά τοῦτο and when
// existential); then the grave rule (an acute on the ultima becomes grave before another word without
// punctuation, not before an enclitic, never for τίς / τί).
void sandhi(std::vector<SandhiWord>& words, const SandhiOptions& o = {});
// Convenience for tests and the checker: splits `phrase` on spaces (punctuation attached to a word stays with it),
// applies sandhi with autoEnclitic and returns the joined text.
std::string accentuate(std::string_view phrase, const SandhiOptions& o = {});

// ---- lemmas and forms ---------------------------------------------------------------------------------------------
// The lemma whose key is greek_key(head) (pos 0 = any), preferring: exact headword without length marks,
// has_table, the requested pos, tier (1 < 2 < 3 < none), frequency rank, lower id. lex::kNoLemma when none.
uint32_t findLemma(const lex::Lexicon&, std::string_view head, uint8_t pos = 0);

struct GenInfo {
  bool exact = false;        // a cell with exactly these features (Attic bit aside)
  bool attic = false;        // the cell carried the Attic bit
  bool contracted = false;   // contracted cell of a contract verb
  bool closed = false;       // built-in closed-class table
  bool fromRule = false;     // rule paradigm (no table): Check, never OK
  bool movableNu = false;    // the form may take a movable ν
  bool presentAsPerfect = false;   // οἶδα: perfect cells used for the present
  uint32_t packed = 0;       // features of the cell used (Attic / contracted bits cleared)
};
// Never invents a form: false when no cell fits. Voice: a middle-form lemma (head in -μαι or the deponent flag)
// asked for the active gives the middle (then passive) cells; middle falls back to passive and vice versa for the
// present system. Tense: a lemma without present cells but with perfect cells (οἶδα) gives the perfect for the
// present and the pluperfect for the imperfect.
bool generate(const lex::Lexicon&, uint32_t lemma, const Features&, std::string& out, GenInfo* info = nullptr);

// C18: the nominative (= vocative) of a participle of `lemma` (tense Present / Aorist / Perfect / Future, voice as
// generate()), agreeing in number and gender with a subject. The singular is a table cell (the tables list the
// participles as nominatives singular); the plural is derived from the singular cells by the endings of the third and
// second declension (τρέχων -> τρέχοντες, ἰδοῦσα -> ἰδοῦσαι, λυόμενος -> λυόμενοι), `info->fromRule` when the lexicon
// does not list the derived form. False for other cases and for endings the rule does not know.
bool participle(const lex::Lexicon&, uint32_t lemma, uint8_t tense, uint8_t voice, uint8_t case_, uint8_t number,
                uint8_t gender, std::string& out, GenInfo* info = nullptr);

// Built-in closed-class tables (Attic). Key = the lemma's greek_key ("ὁ", "ἐγώ", "σύ", "ἡμεῖσ", "ὑμεῖσ", "αὐτόσ",
// "οὗτοσ", "ἐκεῖνοσ", "ὅσ", "τίσ", "τισ", "οὐδείσ", "μηδείσ", "εἷσ", "δύο", "τρεῖσ", "τέτταρεσ", "πᾶσ").
// `enclitic` asks for the enclitic personal forms (μου μοι με σου σοι σε). Gender 0 = masculine.
bool closedForm(std::string_view lemmaKey, uint8_t case_, uint8_t number, uint8_t gender, bool enclitic,
                std::string& out);
bool hasClosedTable(std::string_view lemmaKey);
bool article(uint8_t case_, uint8_t number, uint8_t gender, std::string& out);
// Readings of a written word in the closed tables (for the checker): lemma key, case, number, gender (0 = any).
struct ClosedReading { const char* lemmaKey; uint8_t case_, number, gender; bool enclitic; uint8_t person; };
void closedReadings(std::string_view word, std::vector<ClosedReading>& out);

// Rule paradigm for a lemma WITHOUT an inflection table: regular 2nd-declension nouns (-ος m/f, -ον n) and
// 1st/2nd-class adjectives (-ος -η -ον, -ος -α -ον after ε ι ρ, two-termination -ος -ον), accents recomputed by
// the persistent-accent rules (antepenult -> penult before a long ultima; oxytones circumflex in gen/dat).
struct RuleCell { uint32_t packed; std::string form; };
bool paradigmApplies(const lex::Lemma&);
void paradigm(const lex::Lemma&, std::vector<RuleCell>& out);

// Analysis with the Attic filter: morph::analyseGreek, then analyses flagged non-Attic are dropped when another
// analysis of the same lemma remains; then the paradigm fallback (fromRule) for lemmas without a table.
// Exact readings are tried on the canonical spellings of a sentence word first (grave -> acute, the acute a
// following enclitic added dropped, an unaccented enclitic with its orthotone accent, a movable ν dropped, an elided
// word restored); only then the accent-insensitive search (Token::accentInsensitive = "accent differs").
void analyse(const lex::Lexicon&, std::string_view word, morph::Token& out);
// The full form of an elided word ("δ’" -> "δέ", "ἀφ’" -> "ἀπό"); empty when the word is not elided.
std::string restoreElided(std::string_view word);

// The feature set the realiser asks for, in Greek terms (aorist is feat::Aorist).
Features verbForm(uint8_t person, uint8_t number, uint8_t tense, uint8_t mood = feat::Indicative,
                  uint8_t voice = feat::Active);
Features infinitive(uint8_t tense = feat::Present, uint8_t voice = feat::Active);
Features imperative(uint8_t number, uint8_t tense = feat::Present, uint8_t voice = feat::Active);
Features nounForm(uint8_t case_, uint8_t number);
Features adjForm(uint8_t case_, uint8_t number, uint8_t gender, uint8_t degree = 0);

// True when the word may carry a movable ν by its ending and features: verb 3rd person in -σι, verb 3rd singular
// in -ε of a past or perfect tense, dative plural in -σι, the form ἐστί / εἰσί.
bool movableNuCandidate(std::string_view form, const Features& f);

}  // namespace vp::grc
