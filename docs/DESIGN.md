# vetus poeta — Design Contract (v1)

This document is the single source of truth for implementers. Subagents implement it; the main agent changes it.
Sections marked **[CONTRACT]** are interfaces other modules depend on: do not change them unilaterally; record any
needed change in `docs/STATUS.md` under "API changes" and stop. `docs/DECISIONS.md` (owner decisions) wins over
this file; this file wins over `docs/PREPLAN.md` and `docs/PREDESIGN.md`, which are background reading.

Corrections to the pre-plan, measured by the main agent on the full English dump (2026-10-05): English Wiktionary
entries with a Latin translation = **20,003** (33,453 rows, **30,639 distinct English-Latin pairs**, 18,774 distinct
English words), not 3,397 rows. PREPLAN section 1.3 and risk R1 are therefore too pessimistic; the pivot is a real
lexicon seed, and gloss inversion is the second source, not the first.

## 0. Vision in one sentence
A Windows desktop app that turns English or Spanish subtitles and texts into Latin and Ancient Greek a beginner can
read (Orberg / Athenaze style), and Latin or Greek texts into English or Spanish, entirely offline, never guessing
silently: every word can explain itself, every doubtful cue is marked.

## 1. Approach

### 1.1 Three engines, one pipeline
```
 source cues ──► SourceAnalysis ──► SemFrames ──► Transfer ──► Candidates ──► Realise ──► Check ──► cue text
   (EN/ES)        tagger+parser      (language     lexicon +     (N best)      Latin/Greek   A1..A9    + confidence
                  + phrasebook       neutral)      valency +                   morphology              + reasons
                                                   tiers
                      ▲                               ▲             ▲
                      │ engine ii (optional)          │             │ engine ii: closed-choice sense pick,
                      │ sentence simplification       │             │ gated reranking (PREPLAN 2.2 gate)
                      └───────────────────────────────┴─────────────┘
                                                 engine iii (optional, online): confirms lemma/gloss on wiktionary.org,
                                                 never changes text by itself; adds evidence to "why this word"
```
* **Engine i (rules)** is the only engine that produces text. It is deterministic: same input, same settings, same
  lexicon file = byte-identical output.
* **Engine ii (local model)** only ever answers closed questions: "which of these N senses / candidates" or "rewrite
  this English sentence in simpler English". Its answers enter the pipeline as scores or as an alternative source
  sentence, never as Latin text pasted into the output. Latin-side reranking ships only if the gate in PREPLAN 2.2
  passes (>= 75 % on >= 500 generated minimal pairs); the gate result is written to the eval report either way.
* **Engine iii (online)** adds evidence (Wiktionary agrees / disagrees / unknown) to the per-word reasons and can
  lower confidence to Check; it never raises confidence to OK and never edits text.

### 1.2 Style targets (the "board" of section 15 judges against these)
Latin: the structures and vocabulary of a first-year reader. Concretely: indicative main clauses; subordination with
quod/quia (cause), cum + indicative (time, "when"), sī (condition), ut/nē + subjunctive only for purpose and
after verbs of asking; relative clauses with quī; accusative + infinitive after verbs of saying/thinking in flexible
mode replaced by direct speech or "quod" only when the source is informal; imperatives and vocatives for address;
nōlī/nōlīte + infinitive for prohibitions; no ablative absolute, no gerundive of obligation and no supine in
flexible mode (periphrasis instead); SOV neutral order, verb-final; adjectives after nouns except demonstratives,
numerals and quantity words; nōn before the verb; -ne on the first word of yes/no questions; pronoun subjects
dropped unless contrastive. Vocabulary: T1 whenever the sense is exact; T2 when T1 would change meaning; T3 only in
faithful mode or for names and realia. Macrons shown in the app on every vowel the lexicon marks long.
Greek: Attic, polytonic, article used as Greek requires, particles μέν/δέ/γάρ/οὖν/ἀλλά for connection, aorist for
past narration, present for present, imperfect for background; no dual; no optative in flexible mode; prefer
lemmas shared with Modern Greek with unchanged meaning (data flag `shared_el`).

### 1.3 What "offline" means in code
The engine binary has exactly one network client, in `engine/online/`, compiled behind a runtime switch that is read
from settings on every call. Tests link the engine with a transport mock that counts calls; a test runs a full
translation with online off and asserts zero calls. No other module includes a socket header.

## 2. Repository layout and ownership [CONTRACT]
```
CMakeLists.txt, cmake/        BUILD   top-level build, presets, sanitizer options, xcompile toolchain file
third_party/                  BUILD   vendored: miniz (zip), nlohmann json (single header), doctest, llama.cpp
                                       (pinned commit, CPU only); LICENSES.md lists each with licence text path
tools/build_library/          LIB     Python 3 stdlib-only pipeline raw dumps -> curated TSVs -> .vpl files
tools/train/                  NLP     Python 3 trainers for the EN/ES tagger+parser (.vpt files), stdlib only
tools/jstest/                 JSTEST  Node-only ES5 lint + unit runner + i18n check (tools/jstest.sh)
tools/eval/                   EVAL    error-measurement harness, GT comparison tool (dev only, Node + Playwright)
tools/*.py, tools/*.sh        BUILD   pack_ui.py, make_dist.py, make_icons.py, xcompile_check.sh
data/curated/                 CUR     committed hand-made TSVs: tiers, phrasebook, valency, names, emoji, es-MX glosses
data/raw/, data/work/         -       gitignored (downloads, intermediates, built .vpl)
engine/core/                  CORE    utf8 + normalisation, errors, fs utils, mmap RAII, project (.vpoeta) format,
                                       autosave, lock/recovery, settings
engine/lex/                   LEX     .vpl reader: lookup / analyse / generate / reverse / lemma info
engine/subs/                  SUBS    .srt .vtt .ass parse + write, byte-exact timing, tag protection, line breaking
engine/nlp/                   NLP     EN/ES tokeniser, lemmatiser, perceptron tagger + arc-eager parser (.vpt reader)
engine/rules/                 RULES   SemFrame builder, phrasebook, transfer, Latin/Greek realisation, checker,
                                       LA/GRC -> EN/ES, Orbergise, confidence
engine/llm/                   LLM     llama.cpp wrapper (load/unload, closed choice, score)
engine/online/                ONLINE  Wiktionary client + transport interface + mock
engine/cli/                   CLI     `vpengine` executable: `serve` (JSON lines), `translate`, `check`, `inspect`
engine/tests/                 each    doctest unit tests, one file per module (test_<module>.cpp), python server tests
gui/shell/                    SHELL   Win32 + WebView2 host (port of the prototype), engine supervisor
gui/ui/                       UI      index.html, css/*.css, js/*.js (one IIFE, one VP_* global each), i18n/, fonts/
assets/                       ART     logo*.svg, icon.ico, PNGs, splash; fonts licences
tests/heldout/, tests/regression/, tests/fixtures/   EVAL   own sentences, PD texts as SRT, tiny fixtures
docs/                         MAIN    this file, STATUS.md, DECISIONS.md, HANDOFF.md, BUILD.md, USER_GUIDE*.md
```
CMake targets: `vp_core`, `vp_lex`, `vp_subs`, `vp_nlp`, `vp_rules`, `vp_llm` (optional, `VP_WITH_LLM`), `vp_online`
(all static), `vpengine` (exe), `vp_tests` (exe, doctest), `VetusPoeta` (Windows GUI exe, `VP_BUILD_GUI`, MSVC or
MinGW). Public headers live in `engine/<module>/include/vp/<module>.h`; nothing else is included across modules.

## 3. Build, toolchain and definition of done
* C++17, GCC 13 / Clang on Linux, MSVC 2022 or MinGW-w64 on Windows. Warnings `-Wall -Wextra -Wshadow` clean.
* `cmake -S . -B build -DVP_BUILD_GUI=OFF && cmake --build build -j4 && ctest --test-dir build --output-on-failure`.
* Sanitizers: `-DVP_SANITIZE=ON` (ASan + LSan + UBSan). Zero findings is part of done.
* Cross-compile check: `tools/xcompile_check.sh` (x86_64-w64-mingw32, engine only). If MinGW is absent on the build
  box the script prints `SKIPPED (no mingw)` and exits 0; the main agent runs it where MinGW exists.
* JS: `tools/jstest.sh` (Node >= 18, no npm packages).
* Python tools: Python 3.11 stdlib only (fontTools is allowed in `tools/make_icons.py` only, dev machine).
* Every module has a `README.md` of at most 60 lines: purpose, public API pointer, how to run its tests.

## 4. Text normalisation [CONTRACT]
One implementation in C++ (`engine/core/include/vp/text.h`) and one in Python (`tools/build_library/vptext.py`),
both tested against the golden file `tests/fixtures/normalisation_golden.tsv` (>= 200 rows, both sides must agree).

| Function | Rule |
|---|---|
| `nfc(s)` | Unicode NFC. C++ side: a table-driven composer for the Latin-1 Supplement, Latin Extended-A/B, Latin Extended Additional, Greek and Coptic, Greek Extended and combining marks U+0300-036F only (everything else passes through unchanged); Python uses `unicodedata`. |
| `latin_key(s)` | nfc, lower-case, strip macron/breve (combining U+0304/U+0306 and precomposed ā ē ī ō ū ȳ ă ĕ ĭ ŏ ŭ), `j`->`i`, `v`->`u`, `æ`->`ae`, `œ`->`oe`, drop everything that is not a letter or hyphen or space. Examples: `Iūlius`->`iulius`, `vir`->`uir`, `Sīc`->`sic`. |
| `greek_key(s)` | nfc, lower-case, final sigma `ς`->`σ`, strip U+0304/U+0306 (length marks). Keeps accents and breathings. |
| `greek_bare(s)` | `greek_key` then strip U+0300 U+0301 U+0302 U+0308 U+0313 U+0314 U+0342 U+0343 U+0344 U+0345 (grave, acute, circumflex, diaeresis, smooth, rough, perispomeni, koronis, dialytika tonos, ypogegrammeni) after decomposing to NFD, then recompose NFC. |
| `en_key(s)` / `es_key(s)` | nfc, lower-case, curly quotes -> straight, strip combining marks for ES only for the secondary key (`es_bare`). |
| `display_latin(form, macrons)` | returns the macronised form or the plain form; never changes letters other than removing U+0304/U+0306 and precomposed length marks. |

Keys are compared bytewise. Sorting of keys in `.vpl` is bytewise on the UTF-8 key.

## 5. Lexicon file `.vpl` [CONTRACT]
One file per language: `latin.vpl`, `greek.vpl`, `english.vpl`, `spanish.vpl`. Read-only, memory-mapped, little-endian,
every section 8-byte aligned, all offsets relative to the start of the file, all counts `uint32`. Written only by
`tools/build_library/pack.py`, read only by `engine/lex`. Reproducible bytes: same inputs give the same SHA-256 (no
timestamps). Version bump = incompatible layout; the reader refuses other major versions with `lexicon_version`.

```
Header (256 bytes):  magic "VPLX" | u16 major=1 | u16 minor | char[8] lang ("la","grc","en","es") | u32 section_count
                     | u64 file_size | u8[32] sha256 of bytes [256, file_size) | u8[...] zero padding
Section table:       section_count x { char[4] tag, u32 reserved, u64 offset, u64 length }  (tags below)
NOTE  licence/attribution text, UTF-8 (shown verbatim in About)
STRS  string blob: UTF-8 strings, each NUL-terminated; all *_off fields below are u32 offsets into STRS
KEYS  u32 n_keys; then n_keys x u32 key_off (sorted bytewise by the key string); then n_keys+1 x u32 anal_start
      (index into ANAL; analyses of key k are ANAL[anal_start[k] .. anal_start[k+1]) )
ANAL  u32 n_anal; then n_anal x { u32 lemma_id; u16 feat_id; u16 flags; u32 display_off }   (12 bytes)
      flags: bit0 from inflection table, bit1 from form-of page, bit2 Whitaker-only, bit3 alternative spelling,
             bit4 non-Attic/dialectal, bit5 Medieval/Late/New Latin, bit6 poetic/rare, bit7 enclitic-stripped form
LEMM  u32 n_lemmas; then n_lemmas x LemmaRec (48 bytes):
      { u32 head_off (display headword with length marks); u32 key_off; u8 pos; u8 cls; u8 gender; u8 tier (1,2,3; 0 none);
        u16 freq_rank (0 unknown); u8 whit_freq ('A'..'F', 0); u8 tier_source (0 none, 1 derived, 2 teacher);
        u32 emoji_off (0 none); u32 gloss_en_off (one-line gloss); u32 gloss_es_off; u32 senses_start (into SENS);
        u16 senses_count; u16 flags (bit0 proper name, bit1 indeclinable, bit2 deponent, bit3 impersonal,
        bit4 shared_el, bit5 plural-only, bit6 defective, bit7 has_table); u32 gen_start (into GENX); u16 gen_count;
        u16 principal_off_count; u32 principal_off (STRS: principal parts / genitive line) }
SENS  u32 n_senses; then n_senses x { u32 gloss_en_off; u32 gloss_es_off; u32 keywords_off (space-separated en_keys);
      u16 tags (bit flags: transitive, intransitive, figurative, rare, archaic, poetic, Medieval, New Latin,
      with-dat, with-abl, with-gen, with-acc, with-inf, impersonal, reflexive, reserved); u16 sense_rank }
FEAT  u32 n_feat; then n_feat x u32 packed features:
      pos:5 | case:4 | number:2 | gender:3 | person:2 | tense:4 | mood:3 | voice:2 | degree:2 | extra:5
      enumerations in engine/lex/include/vp/features.h and tools/build_library/features.py (identical, tested)
GENX  u32 n_cells; then n_cells x { u16 feat_id; u16 reserved; u32 form_off }   sorted by feat_id inside each lemma
REVX  u32 n_kw; then n_kw x u32 kw_off (sorted en_key / es_key); then n_kw+1 x u32 cand_start;
      then CAND: n_cand x { u32 lemma_id; u16 sense_idx; u8 score (0..255); u8 pos }   (score: see 5.2)
```
Section order in the file: header, table, NOTE, STRS, KEYS, ANAL, LEMM, SENS, FEAT, GENX, REVX. English and
Spanish files have only NOTE, STRS, KEYS, ANAL, LEMM, FEAT (inflected form -> lemma + features for the NLP module).

### 5.1 Reader API (`engine/lex/include/vp/lex.h`)
```cpp
namespace vp::lex {
struct Analysis { uint32_t lemma; uint16_t feat; uint16_t flags; std::string_view display; };
struct Lemma    { std::string_view head, key, glossEn, glossEs, emoji, principal; uint8_t pos, cls, gender, tier,
                  whitFreq, tierSource; uint16_t freqRank, flags; uint32_t id; };
struct Sense    { std::string_view glossEn, glossEs, keywords; uint16_t tags, rank; };
struct Candidate{ uint32_t lemma; uint16_t sense; uint8_t score, pos; };
class Lexicon {                      // non-copyable, movable; holds one mmap; all views die with it
public:
  static Result<Lexicon> open(const std::filesystem::path&);   // validates header, sizes, sha256 optional (flag)
  std::string_view lang() const; std::string_view notice() const; uint32_t lemmaCount() const;
  bool lookup(std::string_view key, std::vector<Analysis>& out) const;        // exact key; out appended
  void prefix(std::string_view keyPrefix, size_t max, std::vector<std::string_view>& out) const;
  Lemma lemma(uint32_t id) const; void senses(uint32_t id, std::vector<Sense>& out) const;
  bool generate(uint32_t lemma, uint32_t packedFeatures, std::string_view& form) const;  // exact cell
  void cells(uint32_t lemma, std::vector<std::pair<uint32_t,std::string_view>>& out) const;
  void reverse(std::string_view keyword, std::vector<Candidate>& out) const;
  uint32_t feature(uint16_t featId) const; uint16_t featId(uint32_t packed, bool& found) const;
};
}
```
Never throws across the boundary: `Result<T>` carries `{code, message, hint}`. A truncated, zero-filled or bit-flipped
file must give `lexicon_corrupt`, never UB (bounds-checked offsets everywhere; fuzz test required). RSS: only touched
pages; the reader owns no heap besides the small `Result`.

### 5.2 Reverse-index scoring (build time, `tools/build_library/gloss.py`)
score = min(255, round(100 * w)) where w sums: 1.0 if the English word is an exact Wiktionary translation of the lemma
(pivot rows); 0.6 if the keyword is the head word of a lemma gloss ("to love" -> love); 0.35 if it is another content
word of a gloss; +0.15 if Whitaker frequency A/B; +0.1 if DCC core; +0.2 if tier 1; -0.3 if the sense is tagged rare /
archaic / poetic / Medieval / New Latin; -0.2 if the lemma is a proper name and the keyword is lower-case. Candidates
are sorted by score desc, lemma id asc (deterministic). Keywords are `en_key` of lemmatised gloss words (stop words
removed: tools/build_library/stopwords_en.txt, stopwords_es.txt). Spanish keywords come from es.wiktionary Latin
entries, the DCC Spanish list, `data/curated/gloss_es_*.tsv` and the EN->ES pivot (English entry translations `es`).

## 6. Morphology API [CONTRACT] (`engine/rules/include/vp/morph.h`, implemented in `engine/rules/src/morph/`)
```cpp
namespace vp::morph {
struct Token { std::string text; std::string key; std::vector<lex::Analysis> analyses; bool enclitic=false;
               std::string encliticText; /* "que","ne","ue" */ bool unknown=false; };
// Latin: strips -que -ne -ue(-ve) when the base is known and the whole is not; tries u/v, i/j variants (key already
// folds them); falls back to Whitaker stem+ending (ANAL flag bit2) through the lexicon's Whitaker-only analyses.
void analyseLatin(const lex::Lexicon&, std::string_view word, Token& out);
// Greek: exact greek_key, then greek_bare match (flag "accent-insensitive" on the Token), then capital-initial fold.
void analyseGreek(const lex::Lexicon&, std::string_view word, Token& out);
// Generation: feature packing helpers and the one entry point. Returns false if the cell does not exist; the caller
// decides (periphrasis or another lemma). Never invents a form.
bool generate(const lex::Lexicon&, uint32_t lemma, const Features&, std::string& out, bool macrons);
}
```
`Features` is the struct form of the packed word (pos, case_, number, gender, person, tense, mood, voice, degree).
Rule-based paradigm fallback (`paradigm_la.cpp`) exists only for: regular 1st/2nd declension nouns and adjectives, 1st
and 2nd conjugation regular verbs and comparatives/superlatives, and is used only when `has_table` is false; every
generated fallback form carries `fromRule=true` so the checker can flag it (Check, never OK).

## 7. Subtitle I/O [CONTRACT] (`engine/subs/include/vp/subs.h`)
```cpp
namespace vp::subs {
enum class Format { Srt, Vtt, Ass, Txt };
struct Span { enum Kind { Text, Tag, Newline } kind; std::string raw; };     // Tag spans are opaque, copied verbatim
struct Cue  { uint32_t index; std::string idRaw;                   // numbering line exactly as in the file ("12")
              std::string timingRaw;                               // the whole timing line, byte exact
              std::string styleRaw;                                // ASS: everything before the text field
              std::vector<Span> spans;                             // text with tags/newlines as spans
              std::string plainText() const;                       // Text spans joined, tags dropped, newlines -> ' '
              std::string sourceText; /* set by rules */ };
struct Document { Format format; std::string encoding; bool bom; std::string newline; /* "\r\n" or "\n" */
                  std::string headerRaw; /* VTT/ASS header blocks verbatim */ std::vector<Cue> cues;
                  std::string trailerRaw; };
Result<Document> parse(const std::vector<uint8_t>& bytes, Format hint);
Result<std::vector<uint8_t>> write(const Document&, const WriteOptions&);  // WriteOptions: encoding, bom, maxLine=42,
                                                                            // maxLines=2, rebreak=true
// Line breaking: operates on Text spans only; never splits a word, a Tag span or an ASS override block {...};
// prefers breaks after , ; : . ! ? and before conjunctions/prepositions given by the caller; balances lines.
std::vector<std::string> breakLines(const std::string& text, const BreakHints&, int maxLine, int maxLines);
double charsPerSecond(const Cue&);     // from timingRaw; 0 if unparsable
}
```
Guarantees (tested): `write(parse(x))` with unchanged spans is byte-identical to `x` for well-formed files; for
malformed files (gaps in numbering, overlapping times, empty cues, missing blank lines) parsing never fails hard:
the cue is kept with `idRaw`/`timingRaw` as found and a `warnings` list is returned. Encodings: UTF-8 (BOM or not),
UTF-16 LE/BE with BOM, Windows-1252 fallback when invalid UTF-8; the output encoding defaults to the input one.
Numbering and timing lines are **never regenerated**; the writer copies `idRaw` and `timingRaw` verbatim.

## 8. Project file `.vpoeta` [CONTRACT] (`engine/core/include/vp/project.h`)
Zip (miniz, deflate, fixed timestamps) with: `manifest.json` {format: 1, app version, created, pair, kind, source
file name, sha256 of source bytes, settings snapshot}; `source.bin` (the original file bytes, byte exact);
`cues.jsonl` (one line per cue: index, state, target text, chosen alternative, alternatives[], confidence, checks[],
reasons[], edited flag, reviewed flag); `glossary.json` (names with policy, forms, gender, declension); `corrections.json`
(source phrase key -> target phrase, scope, count); `stats.json` (counts for the start screen). Atomic save = write
`name.vpoeta.tmp`, fsync, rename over. Autosave: debounced 5 s after the last change, at most every 60 s, to
`name.vpoeta.autosave` next to the project; a lock file `name.vpoeta.lock` holds pid + time; on open, if a lock exists
with a dead pid or an autosave newer than the project, the engine reports `recoverable` and the UI offers recovery.
Corrupted zip (truncated, bad CRC, missing manifest) -> `project_corrupt` with the newest readable autosave suggested.
Version migration: `format` < current triggers an in-memory upgrade function per version step.

## 9. Engine protocol (JSON lines over stdin/stdout) [CONTRACT]
Same framing as the prototype: request `{"id":<int>,"cmd":"<name>","params":{...}}` -> exactly one response
`{"id":<int>,"ok":true,"result":{...}}` or `{"id":<int>,"ok":false,"error":{"code":"...","message":"...","hint":"..."}}`;
events `{"event":"<name>",...}` without id. One JSON object per line, UTF-8, no raw newlines. Long jobs run on one
worker thread; `*.cancel` is honoured at cue granularity. The engine answers `engine.ping` within 100 ms even while a
job runs (the reader thread answers it). Error codes: `bad_params, not_found, io, unsupported_format, lexicon_missing,
lexicon_corrupt, lexicon_version, project_corrupt, model_missing, model_load_failed, model_unsupported_cpu,
online_disabled, online_failed, busy, internal`. Every error carries a human `hint` in English; the UI translates by
code and falls back to the hint.

| Command | Params -> Result |
|---|---|
| `engine.hello {}` | `{version, lexicons:[{lang, path, version, lemmas, notice}], model:{available, path, sizeBytes, cpuOk}, threads, dataDir}` |
| `engine.ping {}`, `engine.shutdown {}` | `{}` |
| `settings.get {}` / `settings.set {patch}` | full settings object (keys in 9.1) |
| `project.new {kind:"subs"|"text", pair:"en-la"|"es-la"|"la-en"|"la-es"|"en-grc"|"es-grc"|"grc-en"|"grc-es"|"la-la", sourcePath?, text?}` | `{project}` (manifest + stats; cues paged separately) |
| `project.open {path}` | `{project, recoverable:{autosavePath, at}?, warnings:[...]}` |
| `project.recover {path}` / `project.save {path?}` / `project.saveAs {path}` / `project.close {}` | `{path, at}` |
| `cue.page {from, count}` | `{total, cues:[CueView]}` (CueView in 9.2); UI never asks for more than 200 at once |
| `cue.get {index}` | `{cue:CueView, alternatives:[AltView], tokens:[TokenView], checks:[CheckView], reasons:[ReasonView]}` |
| `cue.set {index, text, remember?: "phrase"|"cue"|null}` | `{cue:CueView, correctionAdded?}` (user edit; state=edited) |
| `cue.choose {index, alternative}` | `{cue:CueView}` |
| `cue.review {indices:[...], reviewed:bool}` | `{count}` |
| `translate.start {indices?:[...], engines:{rules:true, model:bool, online:bool}, fidelity:1|2|3, orbergise?:bool}` | `{jobId}`; events `translate.progress {jobId, done, total, cuesPerSec, etaSec}`, `translate.cue {jobId, cue:CueView}` (batched up to 20 per event), `translate.done {jobId, stats}`, `translate.error {jobId, code, message, hint}` |
| `translate.cancel {jobId}` | `{}` |
| `word.inspect {text, lang:"la"|"grc"|"en"|"es"}` | `{analyses:[{lemma:LemmaView, features:FeatureView, display}], suggestions:[...] (if unknown)}` |
| `lemma.get {lang, id}` | `{lemma:LemmaView, senses:[...], cells:[{features:FeatureView, form}]}` |
| `names.list {}` / `names.set {name, policy:"keep"|"decline"|"translate", form?, gender?, declension?}` | `{names:[...]}` / `{affectedCues:[...]}` |
| `corrections.list {}` / `corrections.remove {id}` | `{corrections:[...]}` |
| `words.list {}` | `{words:[{lemma:LemmaView, count, tier}], tierShare:{t1,t2,t3,names}}` |
| `export.write {path, format:"srt"|"vtt"|"ass"|"txt", emoji:bool, macrons:bool, greek:"polytonic"|"monotonic", encoding, bom, rebreak:bool}` | `{path, warnings:[{index, kind}]}` (never overwrites without `overwrite:true`) |
| `export.preview {indices, ...same options}` | `{cues:[{index, lines:[...]}]}` |
| `model.status {}` / `model.locate {path}` / `model.unload {}` / `model.test {}` | `{available, path, sizeBytes, sha256ok, loaded, cpuOk, lastLoadMs}` |
| `online.test {}` | `{ok, latencyMs, message}` (only when online is enabled in settings; else `online_disabled`) |
| `orbergise.start {tier:1|2, keepNames:bool, simplify:bool, originalPath?}` | `{jobId}`; same events as translate |
| `history.undo {}` / `history.redo {}` | `{canUndo, canRedo, changedIndices}` (cue-level history lives in the engine; UI mirrors it) |
| `eval.run {outPath}` (dev) | `{reportPath}` runs the checker over all cues and writes report.json |

Shell-handled commands (never reach the engine): `dialog.openFile`, `dialog.saveFile`, `shell.revealFile`,
`shell.openExternal`, `dialog.droppedFiles` (event), `power.status` (event), `engine.restarted` (event).

### 9.1 Settings keys
`lang` ("en-US"|"es-MX"), `theme` ("auto"|"light"|"dark"), `textScale` (90..140), `showMacrons` (bool), `showEmoji`
(bool), `grammarColours` (bool), `defaultPair`, `defaultFidelity` (1..3), `export.{emoji,macrons,encoding,bom,rebreak}`,
`engines.model` (bool), `engines.online` (bool), `online.wiktionary` (bool), `online.latinitium` (bool, hidden until
terms are confirmed), `modelPath`, `eco` (bool: 2 threads, unload after each job), `autosave` (bool),
`cps.adult` (17), `cps.child` (20), `tourSeenVersion`, `recentProjects` (max 20).

### 9.2 Views
`CueView {index, idRaw, timingRaw, start, end, durationMs, source, target, state:"new"|"translated"|"edited"|
"reviewed"|"stale", confidence:"ok"|"check"|"fix", score:0..1, cps, lines:[...], flags:[...]}`;
`TokenView {text, display, start, end, lemmaId?, features?:FeatureView, tier?, emoji?, unknown?, fromRule?}`;
`FeatureView {pos, case, number, gender, person, tense, mood, voice, degree}` (strings, English keys the UI translates);
`LemmaView {id, head, pos, gender, cls, tier, tierSource, freqRank, whitFreq, glossEn, glossEs, emoji, principal,
flags:[...]}`; `CheckView {id:"A1".."A9", ok:bool, detail}`; `ReasonView {tokenIndex, kind:"sense"|"candidate"|"form"|
"evidence"|"correction"|"phrasebook"|"name", text, data}`; `AltView {text, reason, score}`.

## 10. Rule engine (engine i) — design
Directory `engine/rules/src/` with sub-directories `morph/`, `frame/`, `transfer/`, `realise_la/`, `realise_grc/`,
`check/`, `la2x/`, `grc2x/`, `orberg/`, `cue/`. One class per stage, pure functions where possible, every stage
unit-tested on its own fixtures.

### 10.1 Source analysis (`engine/nlp` + `rules/frame`)
1. **Cue to sentence mapping.** Cues are joined into a stream; sentence boundaries by punctuation and cue-final
   markers; a sentence spanning cues keeps the cue boundaries as "split points" (10.5). Speaker dashes (`- `) start a
   new sentence. Sound cues `[laughs]`, `(music)`, `♪ ... ♪` are classified `nonverbal` or `song`.
2. **Tokenise / normalise.** Contractions expanded with a table (`data/curated/contractions_en.tsv`: don't -> do not,
   I'm -> I am, gonna -> going to, 'em -> them, ...), curly quotes normalised, numbers kept, emoji in source dropped.
3. **Lemmatise + tag + parse.** `english.vpl`/`spanish.vpl` give form -> lemma + features; the tagger is an averaged
   perceptron (features: word, lower, suffixes 1-3, prefix 1, shape, prev/next word, prev tags) trained by
   `tools/train/train_tagger.py` on UD_English-EWT (CC BY-SA 4.0) and UD_Spanish-GSD + AnCora (CC BY-SA / CC BY),
   exported to `english.vpt` / `spanish.vpt` (binary: feature hash table -> weights int16, quantised). The parser is a
   greedy arc-eager transition parser with an averaged perceptron, same training data, UPOS + deprel. Target: >= 94 %
   UPOS, >= 78 % LAS on the UD dev sets, measured by the trainer and written to `tools/train/report.json`. Inference
   in C++ (`engine/nlp`), deterministic.
4. **Phrasebook pre-pass** (`data/curated/phrasebook_en_la.tsv`, `phrasebook_es_la.tsv`, later `_grc`): columns
   `pattern | latin | tier | register | note`. Pattern tokens: literal lower-case words, `{NP}`, `{NAME}`, `{VP}`,
   `{ADJ}`, `{NUM}` slots, optional tokens in `(...)`. Matching is longest-first on the lemmatised token stream.
   The Latin side uses slot references `{1}` and may demand a case: `{1:acc}`. Example rows (our own):
   `oh dear | ō mē miserum/miseram | 1 | excl | gender from speaker glossary, default masculine`,
   `come on | age | 1 | excl |`, `what time is it | quota hōra est | 1 | q |`, `i am sorry | ignōsce mihi | 1 | polite |`.
   Matches become fixed sub-frames (not re-translated) and carry reason `phrasebook`.
5. **SemFrame.** For each clause: `{type: decl|yn|wh|imp|excl|frag|nonverbal|song, polarity, predicate:{lemma, tense:
   past|present|future, aspect: simple|progressive|perfect, modality: none|can|must|may|want|should|will,
   voice}, subject, object, indirectObject, obliques:[{prep, np}], predicative (for copula), vocatives, interjections,
   adverbs, discourse ("well", "oh", "now"), subordinate:[{relation: cause|time|condition|purpose|concession|
   relative|complement|result|manner, frame}], wh:{word, role}}`. NPs: `{head lemma, number, definiteness,
   determiner, possessor, adjectives, numeral, relativeClause, isName, pronoun:{person, number, gender, reflexive}}`.
   The frame builder uses the dependency tree first and falls back to chunk heuristics when the parse is implausible
   (no root verb, or a fragment): fragments become `frag` frames with one NP or one ADJ.
6. **Spanish source** uses the same frame; Spanish-specific rules: clitic pronouns to arguments, `usted` to 2nd
   person, subjunctive moods mapped by the governing conjunction.

### 10.2 Lexical transfer (`rules/transfer`)
For each content lemma: candidates from `REVX` (keyword = source lemma; for multi-word senses the phrasebook wins),
filtered by target POS compatibility, then scored: base `score/255` + sense-match bonus (keywords of the sense overlap
the clause's other lemmas; verbs prefer senses whose `tags` fit the frame: transitive with an object, with-dat when the
object is a person and the verb is in the dative-verbs list) + tier term (fidelity 1: no tier penalty; 2: -0.25 per
tier above 2; 3: -0.5 per tier above 1, periphrasis allowed from `data/curated/periphrasis_la.tsv`) + corrections
memory override (+1.0, reason `correction`) + glossary names policy. Closed classes by tables: pronouns, determiners
(definite article dropped; "this/that" -> hic/ille; "some/any" -> aliquis/quis/ūllus by polarity), prepositions ->
`{latin preposition or bare case, case}` (`data/curated/preps_en_la.tsv`), conjunctions, numerals (cardinals to 100
declined where Latin declines them, otherwise numerals in digits when the source uses digits), interjections.
**Valency** (`data/curated/valency_la.tsv`): lemma -> frame: `acc`, `dat`, `abl`, `gen`, `acc+inf`, `dat+acc`,
`ut`, `nē`, `quod`, impersonal (`mē paenitet`, `mihi licet`, `oportet mē`), prepositional (`in + abl`, `ad + acc`,
`dē + abl`, `cum + abl`, `ā/ab + abl`). Unknown words: no guess; the token is marked unknown (Fix) and the source word
is kept in brackets.

### 10.3 Latin realisation (`rules/realise_la`)
* **Agreement** adj-noun (case, number, gender), subject-verb (person, number), predicate noun/adjective with the
  subject, relative pronoun with its antecedent (gender, number) and its own clause role (case).
* **Tense/mood mapping** (English simple/progressive/perfect/future, modals; Spanish tenses): present -> present;
  past simple/progressive -> perfect for events, imperfect for states and background ("was sitting", "used to",
  habitual markers); past perfect -> pluperfect; future / "going to" / "shall" -> future; "can" -> possum + inf;
  "must/have to" -> dēbeō + inf (flexible) or oportet (faithful allowed); "may/might" (permission) -> licet + dat +
  inf, (possibility) -> fortasse + future/present; "want" -> volō + inf; "let's" -> 1st pl. present subjunctive;
  "would" (conditional) -> imperfect subjunctive in both clauses (faithful), or present indicative with sī (flexible);
  imperative -> imperative sing./pl. by the addressee count (glossary/speaker heuristics, default singular);
  prohibition -> nōlī/nōlīte + inf; "don't you ..." questions -> nōnne.
* **Word order** templates (`data/curated/order_la.txt`, readable rules): default
  `[vocative,] [connector] [subject] [indirect object] [object] [obliques] [adverbs] [negation] verb`; copula sentences
  `subject predicate est`; questions `-ne` on the first word (verb first for yn questions when there is no object:
  "Esne...?"), wh-word first; imperatives verb first when the clause has <= 3 words, else verb last; pronoun subjects
  dropped unless contrast (two different subjects in adjacent clauses) or the source stresses them ("I myself", "you
  too"). Adjectives after the noun, except hic/ille/is/iste, quantity (multī, omnēs, paucī), numerals and
  "magnus/parvus" in exclamations. Enclitic -que allowed in flexible mode for "and" between two single words.
* **Forms** always from `generate()`; when a cell is missing, try: (a) another lemma from the candidates, (b)
  periphrasis table, (c) mark Fix. Macrons: the display form; export strips them unless asked.
* **Names**: policy from the glossary; default for unknown capitalised nouns: keep as in source, undeclined, Check.
  The built-in table `data/curated/names_la.tsv` (our own list of Latinised first names and common appellatives,
  e.g. "rabbit" is not a name but "White Rabbit" as a title gets `Lepus Albus` declined as noun + adj) is a seed.
* **Emoji** (`data/curated/emoji_la.tsv`: lemma, sense_idx, emoji, note): attached after the noun form when the lemma
  has exactly one sense in the table or the chosen sense is listed; never for names, never for abstract nouns; in the
  app only unless export asks. The same table is keyed by Latin lemma so Greek uses `emoji_grc.tsv`.

### 10.4 Checker (`rules/check`) — produces `CheckView` A1..A9 as defined in PREPLAN 6.3
A1 known form, A2 Whitaker cross-check (warning), A3 agreement (re-derived from the output text by re-analysis, not
from the generator's bookkeeping), A4 case government (prepositions and valency), A5 markup/timing integrity, A6 tier
compliance, A7 source coverage (every content lemma of the source accounted for: translated, in phrasebook, or
dropped with a listed reason), A8 reading speed and line length, A9 round-trip overlap (LA -> EN by la2x, lemma
overlap with the source >= 0.5 else Check). Confidence: Fix if any of A1, A3, A4, A5 fails or a token is unknown;
Check if A6-A9 flag, if the best candidate margin < 0.15, if a name was guessed, if a form came from the paradigm
fallback, if the clause type is song/nonverbal, or if engines ii/iii disagree with the choice; else OK.
`score` = product of per-token candidate confidences, capped, for sorting only.

### 10.5 Cue assembly (`rules/cue`)
The translated sentence is mapped back onto the source cues it came from: split points follow the source's cue
boundaries at clause or phrase boundaries (never inside an NP), proportionally by source character share when no
boundary matches; each cue keeps its timing and numbering; line breaking per section 7; tags from the source cue are
re-attached: a cue fully italic stays italic, partial italics are dropped with a warning (A5 reports "tag position
approximated", Check). Songs: translated literally, marked Check, `♪` kept. Nonverbal cues: text copied unchanged
unless the glossary has a translation (`[laughs]` -> `[rīdet]` from `data/curated/nonverbal_en_la.tsv`).

### 10.6 Latin/Greek to English/Spanish (`rules/la2x`, `rules/grc2x`)
Analyse every token (10 morph), disambiguate by constraint propagation (agreement within NPs, verb-subject
number, preposition case, sentence-final verb heuristic), then by lemma frequency and tier; produce two outputs:
**interlinear** (per word: lemma, features in words, gloss in the UI language) and a **readable** sentence built from
a frame (same SemFrame type, filled from Latin roles) through simple English/Spanish realisation rules (SVO, articles
inserted by definiteness heuristics, tense mapping reversed). Confidence per token from analysis ambiguity.

### 10.7 Orbergise (`rules/orberg`)
Input: a Latin document (cues or text) and optionally the original-language file (aligned by cue index, then by time
overlap). Steps: analyse Latin; if the original is present, build the SemFrame from the original (10.1) and
re-realise with tier ceiling 1 or 2 and simplification on; else build the frame from the Latin analysis (10.6) and
re-realise. Every changed word carries `was -> now` with a reason. Meaning check = content-lemma overlap between input
Latin (or original) and output, reported per cue; cues under 0.6 overlap are Check.

### 10.8 Determinism and memory
No randomness anywhere in engine i; ties broken by lemma id, then by string. All per-cue scratch buffers live in a
`Workspace` object allocated once per job and reused (`clear()` not reallocation). Caches: analysis LRU 4,096 entries,
generation LRU 4,096, frame cache none. RSS test: translate 10 cues, record RSS; translate 1,000 cues; growth <= 5 %.

## 11. Local model (engine ii) [CONTRACT] (`engine/llm/include/vp/llm.h`)
```cpp
namespace vp::llm {
struct Status { bool available, loaded, cpuOk; std::string path; uint64_t sizeBytes; bool sha256ok; int lastLoadMs; };
class Model {                              // one instance in the engine; load on first use, unload after job / idle
public:
  Status status() const; Result<void> load(const Config&); void unload();           // Config: path, threads (<=4), ctx<=512
  // Closed choice: returns the index of the option with the highest log-probability of the option text given the
  // prompt (teacher-forced scoring, greedy, fixed seed 1). Deterministic for a fixed model file and thread count.
  Result<int> choose(std::string_view prompt, const std::vector<std::string>& options, std::vector<float>* scores);
  // Simplify: constrained generation (max 48 tokens, greedy, stop at newline) of an English paraphrase. Used only
  // when fidelity == 3 and the frame builder failed; the paraphrase re-enters SourceAnalysis as an alternative.
  Result<std::string> simplify(std::string_view sentence, std::string_view lang);
};
}
```
Build: `third_party/llama` pinned (commit in `third_party/llama/VERSION`), CPU only, `GGML_NATIVE=OFF`, AVX2 + FMA +
F16C; the engine checks CPUID at `load` and returns `model_unsupported_cpu` without loading when AVX2 is absent.
Model file: Qwen2.5-0.5B-Instruct Q4_K_M (Apache-2.0), SHA-256 recorded in `models/README.md`; the installer offers
the download; `*.gguf` never in git. Memory: `use_mmap=true`, `n_ctx=512`, `n_batch=64`; the model is unloaded at the end
of every job and after 60 s idle. Everything the model is asked goes through two prompt templates kept in
`engine/llm/prompts.h` and versioned.

## 12. Online check (engine iii) [CONTRACT] (`engine/online/include/vp/online.h`)
```cpp
namespace vp::online {
struct Transport { virtual ~Transport(); virtual Result<std::string> get(const std::string& url, int timeoutMs) = 0; };
struct Evidence { enum Verdict { Agrees, Disagrees, Unknown, Error } verdict; std::string summary; std::string url; };
class Wiktionary {                        // throttle 1 request/s, cache per project (hash of url -> body, 30 days)
public:
  Wiktionary(Transport&, const Settings&);
  Result<Evidence> checkLemma(std::string_view lang, std::string_view lemma, std::string_view glossEn);
};
}
```
URL: `https://en.wiktionary.org/api/rest_v1/page/definition/{title}` (JSON definitions by language) with
`User-Agent: vetus-poeta/<version> (offline Latin tutor; contact via GitHub oldenKnight)`; on 429 back off 60 s and
return Unknown. The engine refuses to construct a `Wiktionary` unless `settings.engines.online && online.wiktionary`
are true at call time; the UI shows the amber "Online check on" indicator whenever either is true. Latinitium is
not queried in v1 (terms unverified); the switch exists but is hidden.

## 13. UI architecture [CONTRACT]
Files under `gui/ui/`: `index.html`, `css/tokens.css` (colour tokens from PREDESIGN 2.1, light + dark), `css/base.css`,
`css/components.css`, `css/screens.css`, `fonts/GentiumPlus-{Regular,Italic,Bold}.ttf` + `fonts/OFL.txt`,
`i18n/en-US.json`, `i18n/es-MX.json`, and `js/` with exactly these files and globals:

| File | Global | Role |
|---|---|---|
| `polyfill_promise.js` | (none; defines `window.Promise` only if missing) | ES5 Promise/A+ |
| `vp_dom.js` | `VP_Dom` | `el()`, `on()/off()` with listener registry, `count()`, delegation helpers |
| `vp_timers.js` | `VP_Timers` | owned timers per screen, `clearAll(owner)` |
| `vp_i18n.js` | `VP_I18n` | `t(key, vars)`, plurals, `bind(root)`, `num()`, `setLang()` |
| `vp_bridge.js` | `VP_Bridge` | JSON-lines bridge to the shell/engine; `call(cmd, params)` returns a Promise; events |
| `vp_mock_engine.js` | `VP_MockEngine` | dev only: fake engine for browser testing; generates N cues |
| `vp_store.js` | `VP_Store` | app state (project, cues window, settings), subscribe/notify |
| `vp_history.js` | `VP_History` | UI command history mirror (undo/redo labels; engine is authoritative) |
| `vp_keys.js` | `VP_Keys` | one shortcut table, help popover data |
| `vp_toast.js`, `vp_dialog.js`, `vp_tour.js` | `VP_Toast`, `VP_Dialog`, `VP_Tour` | feedback, modal dialogs, first-run tour |
| `vp_router.js` | `VP_Router` | screen mount/unmount lifecycle (`mount(root)`, `destroy()` per screen) |
| `vp_start.js` | `VP_Start` | start screen |
| `vp_workspace.js` | `VP_Workspace` | layout, top bar, status bar, mode tabs |
| `vp_cuelist.js` | `VP_CueList` | virtualised list (fixed 48 px rows, <= 40 rendered rows) |
| `vp_panes.js` | `VP_Panes` | source pane, target pane with word chips, editor, preview strip, alternatives |
| `vp_inspector.js` | `VP_Inspector` | Word tab + "why this word" |
| `vp_engines.js` | `VP_Engines` | engines tab (toggles, fidelity, emoji, macrons) |
| `vp_names.js`, `vp_corrections.js`, `vp_words.js` | `VP_Names`, `VP_Corrections`, `VP_Words` | right-panel tabs |
| `vp_orberg.js` | `VP_Orberg` | Orbergise mode panes |
| `vp_export.js`, `vp_settings.js`, `vp_about.js` | `VP_Export`, `VP_Settings`, `VP_About` | dialogs/pages |
| `vp_debug.js` | `VP_Debug` | `stats()` for tests (listeners, timers, DOM nodes, cue rows, caches) |
| `vp_app.js` | `VP_App` | boot: polyfill check, i18n load, bridge hello, router start |

Rules: ES5 only; one IIFE per file; `index.html` loads them in the table's order; no inline scripts; CSP
`default-src 'self'; img-src 'self' data:; style-src 'self'; font-src 'self'`. Screens expose `mount(rootEl, params)`
and `destroy()`; `destroy()` must return `VP_Dom.count()` and `VP_Timers.count()` to their pre-mount values (tested).
All visible text from `VP_I18n`; Latin/Greek text spans carry `lang="la"|"grc"` and class `vp-text`.
Screens, wireframes, interaction, shortcuts, i18n key scheme, es-MX register, accessibility and budgets: PREDESIGN
sections 1, 4, 5, 6 are adopted as written, with these decisions: fidelity slider left = "Extremely faithful" =
tier 3 allowed (exact word), right = "Flexible" = tier 1 with paraphrase; middle = tier 2; the "Text" kind ships in
v1 (paragraph = pseudo-cue); the wordmark is "vetus poeta" without macron; cue text editor uses a `<textarea>` styled
like the pane (plaintext-only contenteditable is not relied upon).

## 14. Brand and fonts
Logo per PREDESIGN section 3 (concept A, construction plan 3.2) produced by `tools/make_icons.py` from
`assets/logo.svg` (original artwork; no existing mark is copied). Fonts: Gentium Plus Regular/Italic/Bold, OFL 1.1,
unmodified files with their OFL.txt (Reserved Font Name respected: never subset or rename); emoji from the system
font ("Segoe UI Emoji") via the `--font-emoji` stack; UI chrome in Segoe UI. Stack exactly as PREDESIGN 2.2.

## 15. Quality bar (main agent's review checklist, applied to every REVIEW row)
1. Builds, tests, sanitizers, xcompile (where available) all clean; no new warnings.
2. Contracts untouched or the change is recorded under "API changes" with a reason.
3. Memory: RAII, bounded buffers, caches capped, RSS growth test present for any long path.
4. Offline: no network symbol outside `engine/online`; the zero-calls test passes.
5. Latin/Greek quality: the "board" review (owner's 6.2): a Vivarium Novum teacher would accept the grammar; the
   word choice matches a first-year reader; a beginner can read it without a dictionary (tier share), Athenaze
   style for Greek. Reviewed on the regression set by the main agent with written notes in STATUS.md.
6. UI: ES5 lint clean, i18n complete, budgets (PREDESIGN 6.2) asserted by tests, keyboard reachable, both themes,
   both languages, screenshots inspected.
7. Honesty: every number in reports has numerator, denominator and interval; unverified items are labelled.
Error-measurement protocol: PREPLAN section 6 is adopted as written (cue-level metric, matrix, A1-A9, expert
review sampling, frozen held-out set, Google Translate comparison method, publication table).

## 16. Delivery waves (max 4 active implementers; tasks live in docs/STATUS.md)
Wave A: build skeleton + vendoring (BUILD), library stages 1-2 (LIB), subtitle I/O (SUBS), core + project (CORE).
Wave B: lexicon reader (LEX), library stages 3-6 incl. pack (LIB), NLP trainers + reader (NLP), jstest + UI core (UI).
Wave C: morphology + frame + transfer + realisation (RULES, split in two implementers), CLI server (CLI), UI workspace.
Wave D: checker + cue assembly + eval harness, UI panels/dialogs, shell port, brand.
Wave E: llm + online + combination gates; LA->EN/ES; Greek; Orbergise.
Wave F: acceptance loop on the owner's file, held-out measurement, Google Translate comparison, packaging.
