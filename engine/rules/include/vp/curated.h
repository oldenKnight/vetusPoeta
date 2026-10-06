// Runtime loaders for the hand-written tables in data/curated/ (data/curated/README.md, DESIGN.md §10.2-10.3).
// Every file is UTF-8, tab-separated, `#` comments and blank lines ignored. CuratedData::load(dir) reads all of them
// into compact sorted vectors keyed by latin_key (or en_key for English-keyed tables); lookups are binary searches,
// deterministic, allocation-free. A missing file is an error with a hint; a malformed line is a warning (the line is
// skipped), never a crash. order_la.txt is parsed into a rule table that the realiser's Orderer consults.
#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "vp/result.h"

namespace vp::curated {

// ---- valency_la.tsv ---------------------------------------------------------------------------------------------
enum class FrameKind : uint8_t {
  Acc, Dat, Abl, Gen, DatAcc, AccAcc, AccAbl, AccInf, Inf, Ut, Ne, Quod, Impers, Prep, Intr, Copula, Refl, Other
};
struct Frame {
  FrameKind kind = FrameKind::Other;
  std::string raw;            // "acc", "prep:ad+acc", "impers:dat+inf" ...
  std::string prep;           // latin_key of the preposition for Prep frames ("ad", "in", "de", "cum", "a", "ex")
  uint8_t prepCase = 0;       // vp::feat::Case of a Prep frame
  std::string impers;         // the part after "impers:" ("dat+inf", "acc", "acc+gen", ...)
};
struct Valency { std::string key, example, note; std::vector<Frame> frames; };

// ---- names_la.tsv -----------------------------------------------------------------------------------------------
enum class NamePolicy : uint8_t { Keep, Decline, Translate };
struct NameEntry {
  std::string english, latinNom, latinGen, note;
  uint8_t gender = 0;          // vp::feat::Gender (M, F, N), 0 unknown
  int declension = 0;          // 1, 2, 3; 0 indeclinable; -1 not given ("-")
  NamePolicy policy = NamePolicy::Keep;
};

// ---- tiers_la.tsv / tiers_grc.tsv ---------------------------------------------------------------------------------
// `key` is latin_key(head) without the homograph digit of the file ("sero2" -> key "sero", homograph 2); key + pos is
// the identity (serō verb "sow" vs sērō adv "late"). The note lists English glosses ("hole, pit"): the transfer stage
// reads them as the teacher's own reverse index (glossTiers).
struct TierEntry { std::string key, head, pos, source, note; uint8_t tier = 0; uint8_t homograph = 0; };
// ---- emoji_la.tsv / emoji_grc.tsv --------------------------------------------------------------------------------
struct EmojiEntry { std::string key, head, emoji, note; };
// ---- periphrasis_la.tsv -----------------------------------------------------------------------------------------
struct PeriphrasisEntry { std::string key, periphrasis, note; uint8_t tier = 0; };
// ---- preps_en_la.tsv --------------------------------------------------------------------------------------------
struct PrepEntry {
  std::string english, context, latin, latinKey, caseRaw, note;   // latin "-" = bare case; latinKey "" then
  uint8_t case_ = 0;            // vp::feat::Case (0 when the column is "-", "inf" or unknown)
  bool infinitive = false;      // case column "inf"
};
// ---- phrasebook_en_la.tsv, contractions_en.tsv, nonverbal_en_la.tsv, gloss_es_la.tsv ------------------------------
struct PhraseEntry { std::string pattern, latin, reg, note; uint8_t tier = 0; };
struct PairEntry { std::string a, b; };                 // contractions (form, expansion), nonverbal (english, latin)
struct GlossEsEntry { std::string key, head, glossEs; };
// ---- macron_overrides.tsv (rules_la_notes.md decision 5) -------------------------------------------------------------
struct MacronOverride { std::string key, from, to, note; };   // stems as NFC with macrons ("narr" -> "nārr")
// ---- phrasal_en_la.tsv: English verb + particle -> Latin verb lemma + frame (C2b) --------------------------------------
// particle "-" = the bare verb ("bow" -> inclīnō + refl). frame: "" (as the verb is), "refl" (adds the reflexive object:
// "inclīnāte vōs"), "intr" (no object), "acc"/"dat"/"abl" (case of the object noun).
struct PhrasalEntry { std::string verb, particle, latin, frame, note; };
// ---- verbprep_en_la.tsv: English verb + preposition -> Latin verb + what happens to the PP (C2b) ---------------------
// latin "-" = keep the verb's own translation. frame: "obj" (the PP object becomes the direct object: "wait for me" ->
// exspectā mē), "pp" (the PP is translated as usual: "live in" -> habitō in + abl), "prep:<la>+<case>" (the verb fixes
// the Latin preposition: "depend on" -> pendeō ex + abl; "help with" -> adiuvō in + abl).
struct VerbPrepEntry { std::string verb, prep, latin, frame, note; std::string latinPrep; uint8_t prepCase = 0; };
// ---- states_en_la.tsv: "be" + state adjective (or verb + state noun) with a person subject (C2b) ----------------------
// kind "verb": the Latin verb replaces be + adjective ("afraid" -> timeō); kind "adj": the Latin adjective is the
// predicate without a lexicon search ("tired" -> fessus sum). source may be two words ("tener miedo").
struct StateEntry { std::string source, latin, kind, note; };
// ---- clitics_es.tsv: Spanish clitic pronouns (C13) ---------------------------------------------------------------------
// form, person (1-3), number (sg/pl/-), gender (m/f/-), role: acc (lo la los las), dat (le les), refl (se), any (me te
// nos os: accusative, dative or reflexive by context). The tokenizer splits these off verbs ("dámelo" -> da me lo).
struct CliticEntry { std::string form, role, note; uint8_t person = 0, number = 0, gender = 0; };

// ---- order_la.txt -----------------------------------------------------------------------------------------------
struct OrderRule {
  std::string id;                          // "order.decl"
  std::vector<std::string> condition;      // whitespace tokens of the condition
  std::vector<std::string> ordering;       // whitespace tokens of the ordering / construction
  std::string conditionText, orderingText, note;
  int line = 0;
};

struct LoadWarning { std::string file; int line = 0; std::string message; };

class CuratedData {
 public:
  // Reads every table from `dir` (normally <repo>/data/curated or <dataDir>/curated). Error io/not_found with a
  // hint naming the missing file; malformed lines become warnings().
  static Result<CuratedData> load(const std::filesystem::path& dir);

  const std::vector<LoadWarning>& warnings() const { return warnings_; }

  // Lookups by latin_key (valency, tiers, emoji, periphrasis, gloss_es) / exact English (names) / latin_key of the
  // Latin nominative (names). nullptr when absent. For duplicate keys the first row wins (a warning is recorded).
  const Valency* valency(std::string_view key) const;
  const TierEntry* tier(std::string_view key) const;
  const TierEntry* tier(std::string_view key, std::string_view pos) const;   // pos as in the file ("verb", "adv")
  const TierEntry* tier(std::string_view key, uint8_t latinPos) const;        // vp::feat::Pos of the lemma
  // The tier that counts: the curated row of key + pos (teacher-editable, wins), else the lexicon's own tier, else 3.
  uint8_t effectiveTier(std::string_view key, uint8_t latinPos, uint8_t lexiconTier) const;
  // Tier rows whose note lists `english` (lower case, a whole comma-separated item) as a gloss: "hole" -> fovea.
  void glossTiers(std::string_view english, std::vector<const TierEntry*>& out) const;
  // File spelling of a vp::feat::Pos in tiers_la.tsv ("noun", "verb", "adj", "adv", ...); "" when not listed.
  static const char* tierPos(uint8_t latinPos);
  const MacronOverride* macronOverride(std::string_view key) const;
  const PhrasalEntry* phrasal(std::string_view verb, std::string_view particle) const;
  const VerbPrepEntry* verbPrep(std::string_view verb, std::string_view prep) const;
  const StateEntry* state(std::string_view source) const;
  const std::vector<PhrasalEntry>& phrasals() const { return phrasal_; }
  const std::vector<VerbPrepEntry>& verbPreps() const { return verbPrep_; }
  const std::vector<StateEntry>& states() const { return states_; }
  const TierEntry* tierGreek(std::string_view key) const;
  const EmojiEntry* emoji(std::string_view key) const;
  const EmojiEntry* emojiGreek(std::string_view key) const;
  const PeriphrasisEntry* periphrasis(std::string_view key) const;
  const GlossEsEntry* glossEs(std::string_view key) const;
  // gloss_es_la.tsv read backwards (C13): rows whose gloss_es lists `spanish` (lower case, a whole comma- or
  // semicolon-separated item, parentheses dropped): the teacher's Spanish reverse index ("pelota" -> pila).
  void glossEsLemmas(std::string_view spanish, std::vector<const GlossEsEntry*>& out) const;
  // Spanish source tables (C13). Optional files: a missing one is a warning and leaves the table empty.
  // states_es_la.tsv / phrasal_es_la.tsv / verbprep_es_la.tsv are merged into states() / phrasals() / verbPreps()
  // (their source words are Spanish lemmas, so they never collide with the English rows).
  const std::vector<PhraseEntry>& phrasebookEs() const { return phrasebookEs_; }      // phrasebook_es_la.tsv
  const std::vector<PairEntry>& contractionsEs() const { return contractionsEs_; }    // contractions_es.tsv
  const std::vector<CliticEntry>& clitics() const { return clitics_; }                // clitics_es.tsv
  const CliticEntry* clitic(std::string_view form) const;
  const NameEntry* nameByEnglish(std::string_view english) const;   // case-insensitive (en_key)
  const NameEntry* nameByLatin(std::string_view latinKey) const;    // latin_key of latin_nom
  // Cases a Latin preposition governs according to preps_en_la.tsv (keys "in", "ad", "cum", "a"/"ab", "e"/"ex" ...),
  // as a bit set (1 << feat::Case). 0 when the word is not a Latin preposition in the table.
  uint16_t prepCases(std::string_view latinKey) const;
  const std::vector<PrepEntry>& preps() const { return preps_; }
  const std::vector<NameEntry>& names() const { return names_; }
  const std::vector<PhraseEntry>& phrasebook() const { return phrasebook_; }
  const std::vector<PairEntry>& contractions() const { return contractions_; }
  const std::vector<PairEntry>& nonverbal() const { return nonverbal_; }
  const std::vector<OrderRule>& orderRules() const { return order_; }
  const OrderRule* rule(std::string_view id) const;

  // Helpers on order rules. Latin words listed inside "{...}" of the condition (connector sets), and the words of
  // the first "(...)" group that follows `marker` in the ordering text (adjective exceptions, time adverbs, wh
  // words, enclitic cum forms). Words are returned as latin_key. Empty when the rule or the group is absent.
  std::vector<std::string> conditionSet(std::string_view ruleId) const;
  std::vector<std::string> orderingList(std::string_view ruleId, std::string_view marker) const;
  // Slot template of a rule whose ordering is a slot sequence ("[VOC,] [CONN] S IO O OBL ADV [NEG] V"): the slot
  // names in order with optional markers stripped ("VOC","CONN","S",...). Tokens that are not slot names end it.
  std::vector<std::string> slotTemplate(std::string_view ruleId) const;

 private:
  std::vector<Valency> valency_;
  std::vector<NameEntry> names_;
  std::vector<uint32_t> namesByLatin_;   // indices into names_, sorted by latin_key(latinNom)
  std::vector<std::string> namesLatinKey_;
  std::vector<TierEntry> tiers_, tiersGrc_;
  std::vector<EmojiEntry> emoji_, emojiGrc_;
  std::vector<PeriphrasisEntry> periphrasis_;
  std::vector<PrepEntry> preps_;
  std::vector<std::pair<std::string, uint16_t>> prepCases_;   // sorted latin key -> case bits
  std::vector<PhraseEntry> phrasebook_, phrasebookEs_;
  std::vector<PairEntry> contractions_, nonverbal_, contractionsEs_;
  std::vector<CliticEntry> clitics_;
  std::vector<std::pair<std::string, uint32_t>> glossEsIndex_;   // Spanish gloss item -> index into glossEs_, sorted
  std::vector<GlossEsEntry> glossEs_;
  std::vector<OrderRule> order_;
  std::vector<MacronOverride> macron_;
  std::vector<PhrasalEntry> phrasal_;
  std::vector<VerbPrepEntry> verbPrep_;
  std::vector<StateEntry> states_;
  std::vector<std::pair<std::string, uint32_t>> glossIndex_;   // note gloss -> index into tiers_, sorted
  std::vector<LoadWarning> warnings_;

  friend struct Loader;
};

// Parses one frame string of valency_la.tsv ("acc", "prep:in+abl", "impers:dat+inf" ...). Unknown -> Other.
Frame parseFrame(std::string_view s);
// Parses the case column of preps_en_la.tsv or a frame case ("acc", "abl", "dat", "gen", "loc", "nom", "voc").
uint8_t parseCase(std::string_view s);

}  // namespace vp::curated
