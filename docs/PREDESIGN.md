# PREDESIGN — vetus poeta (pre-design for M0.4 and M4, written 2026-10-05)

Status: input to `docs/DESIGN.md`. `docs/DECISIONS.md` wins on any conflict. No application code here, only
decisions, wireframes and numbers. What I checked is stated as verified; the rest is marked **unverified**.
Companion: `docs/PREPLAN.md` (data, models, tasks). Prototype reference: BabyDaVinci `gui/ui` (dark, 3-step top bar,
44 px targets, spotlight walkthrough, mock engine for browser testing): its good ideas are kept, its single 5,622-line
`app.js` and `window.bridge` global are not (CLAUDE.md: separate files, one `VP_<Name>` global per IIFE).

Design principles (the teacher and students have low tolerance for frustration):
1. **Always a next step.** Every screen has one obvious primary action; empty states say what to do.
2. **Nothing is ever lost.** Autosave visible, undo for everything, no modal "are you sure" for reversible actions.
3. **Say why.** Every Latin or Greek word can answer "why this word?" in plain language.
4. **Quiet confidence.** Green/yellow/red is explicit and honest; yellow and red never hide.
5. **Slow computer friendly.** No decoration that costs frames; budgets in section 6.

## 1. Information architecture and screens

```
App
├─ Start                     recent projects, new from file, quick text, sample project, language pair
├─ Workspace (one project open at a time)
│   ├─ Mode: Translate | Orbergise                    (tabs in the top bar)
│   ├─ Kind: Subtitles (cues) | Text (single document)  (same panels; Text has no timing column)
│   ├─ Left:   Cue list (virtualised, confidence chip per cue, filters)
│   ├─ Centre: Source pane + Target pane (+ player-style preview, alternatives)
│   └─ Right:  tabs  Word | Engines | Names | Corrections | Words in file
├─ Export (dialog)
├─ Settings (dialog/page)
├─ About and attributions (dialog/page)
└─ First-run tour (spotlight overlay, re-openable from "?")
```

Language pairs offered in one picker (D11 order of delivery): English to Latin, Español to Latin, Latin to English,
Latin to Español, then Ancient Greek both ways, then "Orbergise a Latin file". Unavailable pairs are shown disabled
with "coming in a later version", never hidden (students see the road map).

### 1.1 Start screen

Purpose: get to a first translated cue in two clicks. Layout (1280x800 minimum, centred column 880 px max):

```
┌──────────────────────────────────────────────────────────────────────────────┐
│ [logo] vetus poeta                                          [EN | ES]  [gear] [?] │
├──────────────────────────────────────────────────────────────────────────────┤
│                                                                              │
│   What would you like to do?                                                 │
│                                                                              │
│   ┌───────────────────────────┐  ┌───────────────────────────┐              │
│   │  ▤  Subtitle file          │  │  ✎  Type or paste text    │              │
│   │  Drop .srt .vtt .ass here  │  │  Translate a few lines    │              │
│   │  or [ Choose file… ]       │  │  [ Start ]                │              │
│   └───────────────────────────┘  └───────────────────────────┘              │
│                                                                              │
│   Translate from  [ English ▾ ]  to  [ Latin ▾ ]        ☐ Orbergise a Latin file │
│                                                                              │
│   Recent projects                                          [ Try the sample ]│
│   ┌──────────────────────────────────────────────────────────────────────┐   │
│   │ ● Fabulae.srt       EN → LA     842 / 1,019 cues   12 need review  2 h ago│   │
│   │ ● Carmina.vtt       EN → LA     120 / 120 cues     done            Mon    │   │
│   │   (nothing else yet)                                                      │   │
│   └──────────────────────────────────────────────────────────────────────┘   │
│  Status: model not installed (optional) · online check off · lexicon OK v1.0 │
└──────────────────────────────────────────────────────────────────────────────┘
```
Rules: dropping a file anywhere on the window starts the same flow; a crashed session shows a banner "We found work
from your last session. [Recover] [Keep the saved version]" above the recent list (default = Recover, as D16 and the
prototype). Missing lexicon shows one red card with the exact fix. "Try the sample" opens a built-in 12-cue project
made of our own sentences so a student can explore with no file. Recent list: max 20, each row is a real button.

### 1.2 Project workspace

Layout at 1280 wide: left 320 px, centre flexible (>= 560), right 360 px; top bar 56 px; status bar 36 px.

```
+- TopBar --------------------------------------------------------------------------------+
| [logo] Fabulae.srt  (o) Saved 12:03   [EN > LA v]  [ Translate | Orbergise ]  Undo Redo [gear] [?] |
+--------------------+----------------------------------------+---------------------------+
| CUES  [Filter v][find] | SOURCE (English)      00:04:12,300 - 00:04:14,900   17 cps | [Word][Engines][Names]  |
|--------------------| +------------------------------------+ | [Corrections][Words]      |
| 218 (o) Festina!   | | Quick! The bus is late!             | |                           |
| 219 (o) Ubi est... | +------------------------------------+ | WORD INSPECTOR            |
|>220 /\ Celeriter!  | TARGET (Latin)    (o) OK /\ Check [] Fix | | sero                      |
| 221 (o) Quid est.. | +------------------------------------+ | adverb - T1 *** - [emoji] |
| 222 []  (unknown)  | | Celeriter! Raeda sero venit!        | | "late; at a late hour"    |
| 223 (o) ...        | |  (each word is a clickable chip)    | | Form: adverb, not declined|
|  ... virtualised   | +------------------------------------+ | Why this word?  [>]       |
|                    | Preview, as a player shows it:          | Other choices:            |
|                    | +------------------------------------+ |  tarde (T2)               |
|                    | |        Celeriter!                   | | [Use this] [Add to names] |
|                    | |        Raeda sero venit!            | |                           |
|                    | +------------------------------------+ |                           |
|                    | Alternatives [1] .. [2] ..   [Accept Enter] [Edit E] [Next Down]     |
+--------------------+----------------------------------------+---------------------------+
| Autosaved 12:03 - 842/1019 translated - 12 to check - 3 to fix - Rules on, Model off, Online off |
+-----------------------------------------------------------------------------------------+
```
(ASCII wireframe: macrons are dropped in the drawing only; the real UI shows "sero" as "sero" with its macron,
"Festina!" as "Festina!", and so on. Confidence marks: (o) circle = OK, /\ triangle = Check, [] square = Fix.)

**Cue list (left).** Row = number, confidence chip (shape + colour + word on hover/focus: circle OK, triangle Check,
square Fix; never colour alone), first line of target (source if untranslated, in muted italic). Row height 48 px,
fixed, so virtualisation is arithmetic. Filters: All, Needs review (yellow+red), Red only, Edited by me, With
emoji, Over reading speed, Unknown names. Search finds in source or target (macron-insensitive). Selected row is
mirrored in the centre; arrow keys move; the list scrolls itself to keep the selection visible.

**Source pane (centre top).** Shows the original cue text with markup rendered (italic tags shown as italic, ASS
overrides shown as small grey chips so they are visible but not editable). Header shows timing (read-only,
monospace), duration and characters-per-second. Source words are also clickable (inspector shows the English or
Spanish sense chosen and the Latin candidates).

**Target pane (centre middle).** Latin or Greek text as **word chips**: each word is a text span (no box until
hover) that opens the Word inspector. Editing = press `E` or click a gap: the pane turns into a plain text editor
(`contenteditable="plaintext-only"` or a textarea; unverified which behaves better in WebView2, M4.3 decides) with
live check marks; `Esc` cancels, `Ctrl+Enter` accepts. While editing, unknown words are underlined in red with the
lexicon's nearest suggestions. Macrons toggle (`Ctrl+M`) and emoji toggle (`Ctrl+E`) apply to the display only.

**Preview strip.** A dark rounded band rendering the cue the way a player would (two lines, 42 chars, text shadow);
shows what line breaking and emoji/macrons will look like in the file given Export settings; a warning chip appears
here for CPS and line-length breaches.

**Alternatives row.** Up to 3 alternatives, numbered 1-3 (keys `1`,`2`,`3`), each with a one-line reason
("simpler words", "closer to the English", "from your corrections"). Choosing one moves the previous choice into
alternatives, so nothing is lost.

**Right panel tabs.**
* *Word* — see 1.2.1.
* *Engines* — see 1.2.2.
* *Names* — glossary of proper names (4.2).
* *Corrections* — the correction memory list (4.2), each entry removable, with a counter of times applied.
* *Words in file* — frequency-sorted vocabulary of the target text with tier badges, filterable by tier; button
  "Copy as list" and "Export CSV" for flashcards (didactic; also shows how many words are outside the chosen tier).

#### 1.2.1 Word inspector

```
┌ Word ──────────────────────────────┐
│ sērō  ·  adverb         [T1 ★★★]   │   tier badge: 3 laurel leaves = T1, 2 = T2, 1 = T3
│ "late; at a late hour"   [emoji] (emoji if depictable noun)   │
│ From: sērus (adj.)  · dictionary form shown with macrons   │
│ ───────────────────────────────── │
│ This cue uses:   adverb (not declined)                      │
│ Other forms ▸ (opens the table: sērus, sēra, sērum …)       │
│ ───────────────────────────────── │
│ Why this word? ▸                                            │
│   English "late" (adverb, here: after the expected time)    │
│   Candidates:                                               │
│    1 sērō   T1  common      ◀ chosen                        │
│    2 tardē  T2  rarer                                       │
│    3 nuper  T2  means "recently" ✗ sense mismatch           │
│   Checks: known form ✓  agrees ✓  tier ≤ T1 ✓               │
│   Sources: Wiktionary ✓  Whitaker ✓  Online ✗ (off)         │
│ [ Use another word ▾ ]  [ Add to my corrections ]           │
└────────────────────────────────────────────────────────────┘
```
The analysis lines are produced by the engine (`word.inspect`, `cue.get` with reasons), never invented by the UI.
Grammar terms are written in words first, abbreviation second ("accusative singular (acc. sg.)"), with a "Grammar
help" link per term that opens a one-paragraph explanation (strings in i18n, written by the teacher).

#### 1.2.2 Engine panel

```
┌ Engines ───────────────────────────────────┐
│ Rules (dictionary + grammar)      [■ on ]  │   always on; shown as locked on (explained)
│   Lexicon v1.0 · 1.1 M Latin forms         │
│ Local model (optional)            [  off ] │   needs the model file
│   Not installed. [Install…] [Find file…]   │   or "Qwen2.5-0.5B · 374 MiB · loads when needed"
│   Helps with: meaning of English words     │
│ Online check (wiktionary.org)     [  off ] │   off by default
│   Sends single words to wiktionary.org.    │
│   [ Test connection ]                      │
│ ────────────────────────────────────────── │
│ How close to the original?                 │
│  Extremely faithful ●────────○ Flexible    │   3 stops (maps to tiers, see note)
│   Uses any word needed (T3)  ·  Simple words only (T1), may rephrase │
│ ☑ Emoji after picturable nouns (in app)    │
│ ☐ Macrons in exported file                 │   links to Export defaults
│ [ Translate again (selected cues | all) ]  │
└────────────────────────────────────────────┘
```
Mapping note (needs confirmation by the main agent): D12 says the toggle "moves the vocabulary tier" and that
"flexible may paraphrase with T1 words". I read it as **faithful = widest vocabulary allowed (T3) so the exact
word can be used; flexible = narrowest vocabulary (T1) with permission to rephrase**. If the owner means the
opposite, only the end captions swap. The slider has 3 detents and a text line that always states the effect in
numbers ("Uses about 1,500 core words and may rephrase"). Changing any engine control marks affected cues stale
(grey dot) and offers "Translate again", it never silently rewrites reviewed or edited cues (those keep a lock icon).
Toggling Online shows a one-time explanation dialog (what leaves the computer; nothing else is ever sent).

#### 1.2.3 Orbergise mode

Same workspace, tab switched. Left list unchanged. Centre becomes three stacked panes: **Latin file** (the text to
rewrite), **Original-language file** (optional, aligned by cue number then by time overlap; shows "not loaded —
[Choose file…]"), and **Orberg version** (editable). Changed words are marked with a subtle underline and their
tier badge; clicking shows "was → now" with the reason (vocabulary swap, structure change) in the right panel.
A "Meaning check" chip compares content lemmas of the rewrite with the source (percentage + list of missing ones).
Options in the Engines tab: Tier ceiling (T1/T2), "Keep names", "Simplify sentence structure" (on).

#### 1.2.4 Text kind (feature A without a file)

The Start screen's "Type or paste text" opens the same workspace with one pseudo-cue per paragraph, no timing column, no
reading speed, no Export dialog formats except "Copy" and ".txt". The same inspector and engines panels apply.

### 1.3 Export dialog

```
┌ Export ─────────────────────────────────────────────────────┐
│ Format      ( ) .srt   ( ) .vtt   ( ) .ass      (only the original's format is on by default)
│ File name   [ Alicia.la.srt                ] [ Choose… ]
│ ───────────────────────────────────────────────────────────  │
│ Text options                                                  │
│  ☐ Emoji in the file          (players often show boxes)      │  default off (D9)
│  ☐ Macrons (ā ē ī ō ū) in the file                            │  default off (D13)
│  Greek: ( ) polytonic  ( ) monotonic fallback                  │  Greek only
│  Encoding  [UTF-8 ▾]  ☐ add BOM (needed by some old players)  │
│  Line breaks: re-break at 42 characters, max 2 lines   ☑      │  D15
│ ───────────────────────────────────────────────────────────  │
│ Checks before saving                                          │
│  ✓ 1,019 cues, numbering and timing unchanged                 │
│  ▲ 14 cues faster than 17 characters/second  [Show them]      │
│  ■ 3 cues marked Fix                         [Review first]   │
│ Preview   ┌──────────────────────────┐                        │
│           │      Celeriter!           │  (first 3 cues, as a player shows them) │
│           └──────────────────────────┘                        │
│                              [ Cancel ]   [ Export ]          │
└───────────────────────────────────────────────────────────────┘
```
Export never blocks on warnings (a teacher may want the file anyway) but red cues need one explicit tick ("Export with
3 cues to fix"). The default file name inserts the language code before the extension; existing files are never
overwritten without asking (and the engine writes to a temp file then renames).

### 1.4 Settings

Single scrolling page with a left anchor list (keeps 44 px targets): **General** (language en-US / es-MX, theme Auto /
Light / Dark, text size 90-140 %, show macrons in app, show emoji in app, grammar colours on/off), **Translation
defaults** (default pair, default fidelity, export defaults for emoji/macrons/encoding), **Engines** (model status:
path, size, SHA-256 check, last load time, "Test model", unload now; Online: master switch + Wiktionary switch +
"Test"; Latinitium switch appears only if the owner confirms its terms), **Saving** (autosave on, debounce 5 s and
maximum 60 s as in the prototype, "Open projects folder", "Recover from last crash"), **Performance** (eco mode:
fewer threads, model unloaded; "Free memory now"), **Learning** (reading speed targets adult/child, tour reset).
Every setting applies immediately; there is no Save button; a "Reset to defaults" at the end.

### 1.5 Attribution and About

Dialog with tabs **About** (version, build, logo, owner), **Data sources and licences** (text read from the lexicon
file's notice block so it can never drift from the data: Wiktionary/Kaikki CC BY-SA 4.0, Perseus Lewis & Short and
LSJ CC BY-SA 4.0, DCC Core Vocabulary CC BY-SA 3.0, Whitaker's Words permission text quoted), **Software** (llama.cpp
MIT, miniz, nlohmann/json, WebView2 runtime, model licence) and **Fonts** (Gentium Plus, OFL 1.1, SIL). Each entry has
name, author, licence name, link text (opens through `shell.openExternal`) and "Show licence text". The same file is
shipped as `THIRD_PARTY_NOTICES.txt` next to the exe.

### 1.6 First-run tour

Spotlight overlay (prototype pattern), 6 steps, skippable, shown when `settings.tourSeenVersion` differs, re-openable
from `?`. Steps: (1) Open or drop a subtitle file; (2) choose the pair; (3) the engines and the faithful-flexible
slider; (4) the cue list and the three colours (shape + colour); (5) click a word to see why; (6) export. The last
step offers "Open the sample project" so the tour ends with something to touch. Each step has Back / Next / Skip,
keyboard operable, text fixed to the control (never covers it), and respects reduced motion.

## 2. Visual direction

Warm, bookish, calm: parchment background, ink-brown text, **terracotta** as the Latin/primary accent, **Aegean
blue** as the Greek accent, olive green / amber / brick for OK / Check / Fix. Light is the default; dark is a warm
dark ("candlelit study"), not a hacker theme; "Auto" follows Windows. Generous spacing (8 px grid), 12 px radius
for panels, 8 px for controls, soft shadows, no gradients except the logo. Motion is short (120-180 ms ease-out),
removed under `prefers-reduced-motion`.

### 2.1 Colour tokens (contrast verified with a script on 2026-10-05: WCAG relative luminance)

| Token | Light | Dark | Use |
|---|---|---|---|
| `--bg` | `#FBF7F0` | `#1B1815` | window background |
| `--surface` | `#FFFFFF` | `#25211D` | panels, cards |
| `--surface-2` | `#F3ECDF` | `#2E2924` | list stripes, inputs, chips |
| `--border` | `#D9CDB8` | `#3F3830` | dividers (decorative, 1.5:1 allowed) |
| `--border-strong` | `#8A7F6D` | `#8C8173` | input and button outlines (3.7:1 / 4.6:1 on bg) |
| `--text` | `#2A2622` | `#F0E9DE` | body (14.1:1 / 14.7:1) |
| `--muted` | `#6B6157` | `#B5AA9B` | secondary text (5.7:1 / 7.7:1) |
| `--accent` (Latin, primary) | `#B3452A` | `#E8825F` | primary buttons, selection (5.2:1 / 6.6:1 on bg) |
| `--on-accent` | `#FFFFFF` | `#1B1815` | text on accent (5.5:1 / 6.6:1) |
| `--greek` | `#2B6A9B` | `#86B8E3` | Greek mode accent (5.4:1 / 8.4:1) |
| `--ok` | `#2F6F44` | `#7FC48F` | green cue (5.7:1 / 8.6:1) |
| `--warn` | `#8A5A00` | `#E6B35C` | yellow cue (5.6:1 / 9.2:1) |
| `--bad` | `#A8322B` | `#F08A80` | red cue (6.2:1 / 7.3:1) |
| `--focus` | `#1F5FBF` | `#8DB8FF` | focus ring 3 px with 2 px offset |
| `--accent-soft` | `#F6DDD3` | `#3A2620` | selected row, active tab background |

All text tokens above reach >= 4.5:1 against `--bg`, `--surface` and `--surface-2` in both themes (measured; the
tightest is `--accent` on `--surface-2`, 4.70:1). Cue confidence is encoded three ways: colour, shape (circle /
triangle / square) and a word ("OK", "Check", "Fix"), so colour-blind users and monochrome screens lose nothing. The
grammar-colour option (case colouring of words) uses a colour-blind-safe set plus underline styles (solid, dashed,
dotted, double, wavy) per case; default off.

### 2.2 Typography and font stack

Needs: Latin with macrons and breves, polytonic Greek (all of Greek Extended U+1F00-1FFF with correct mark
stacking), emoji, readable at 16-20 px on a small laptop screen, redistributable inside a proprietary app.

**Verified on 2026-10-05** (download from `raw.githubusercontent.com/google/fonts/main/ofl/…`, HTTP 200; coverage
counted with fontTools: precomposed macron vowels 12 tested, breve vowels 10, combining marks 9, Greek Extended 233
assigned code points, a GPOS `mark` feature present; rendering checked in the container's Chromium with the real
files, see below):

| Font | Licence | Files and sizes (TTF, bytes) | Latin macron / breve | Combining marks | Greek Extended | Notes |
|---|---|---|---|---|---|---|
| **Gentium Plus** 6.101 | SIL OFL 1.1, **Reserved Font Names "Gentium" and "SIL"** | Regular 822,956; Italic 873,104; Bold 824,904; BoldItalic 890,860 | 12/12, 10/10 | 9/9 | **233/233** | Designed for Latin and polytonic Greek by SIL; 4,307 glyphs; warm, slightly rounded; **must be shipped unmodified or renamed** (RFN): never subset under the same name |
| Gentium Book Plus | OFL 1.1, same RFN | Regular 818,952 | 12/12, 10/10 | 9/9 | 233/233 | heavier, bookish; alternative |
| **Cardo** 1.0451 | OFL 1.1 (David J. Perry; no RFN declared in the copyright line) | Regular 400,468; Italic 262,388; Bold 348,320 | 12/12, 10/10 | 9/9 | **233/233** (Greek 134/135) | classicist's font, scholarly Bembo-like look, extensive OpenType features (smcp, onum, hist, salt) |
| Libertinus Serif | OFL 1.1 | Regular 603,444 | 12/12, 10/10 | 9/9 | 233/233 | modern Linux-Libertine descendant; Greek block 110/135 |
| Noto Serif (variable) | OFL 1.1 | `NotoSerif[wdth,wght].ttf` 1,887,192 | 12/12, 10/10 | 9/9 | 233/233 | neutral, big; good fallback |
| Noto Sans (variable) | OFL 1.1 | 2,049,096 | 12/12, 10/10 | 9/9 | 233/233 | UI fallback if Segoe UI missing |
| EB Garamond (variable) | OFL 1.1 | 851,176 | yes | yes | 233/233 | elegant, Greek block only 94/135 |
| Alegreya (variable) | OFL 1.1 | 425,288 | yes | yes | 233/233 | calligraphic, Greek block 82/135 |
| Atkinson Hyperlegible | OFL 1.1 | 54,348 | **8/12**, 2/10 | 5/9 | **0/233** | rejected: no polytonic Greek |
| Noto Color Emoji | OFL 1.1 | 25,332,736 | n/a | n/a | n/a | has all 8 emoji tested; **too big to bundle**; the Windows system emoji font is enough |
| Brill | free only for non-commercial use (from memory) | not downloaded | | | | **rejected** (licence); owner already excluded it |

Licence texts: the OFL.txt files for Cardo, Gentium Plus and Noto Color Emoji were fetched (HTTP 200). Rendering test:
an HTML page with the four font files loaded through `@font-face` in the container's Chromium 1194 (Linux, HarfBuzz)
drew "Mārcus Iūlia fīliī; ĕ ŏ ŭ", "ἄνθρωπος ἀγαθός" with stacked breve + acute + smooth breathing (ᾰ̓́ ῐ̔̃ ᾱ̀ ῠ́) and emoji
correctly in Gentium Plus, Cardo and Libertinus Serif (screenshot inspected). **Windows 10/11 DirectWrite rendering is
unverified** and must be checked by the owner in M4.6; the same combining sequences are the risk (precomposed forms are
safe).

**Recommended stack (one decision):**

```css
:root {
  /* Latin, Greek and cue text: bundled, always the same on every PC */
  --font-text: "Gentium Plus", "Cardo", "Cambria", "Palatino Linotype", "Times New Roman",
               "Noto Serif", "Segoe UI Emoji", "Noto Color Emoji", serif;
  /* application chrome: the system UI font, no download */
  --font-ui:   "Segoe UI Variable Text", "Segoe UI", "Noto Sans", system-ui, sans-serif;
  /* emoji only (used on the emoji span so it never changes line height) */
  --font-emoji: "Segoe UI Emoji", "Noto Color Emoji", "Apple Color Emoji", sans-serif;
  /* numbers: timing and counts */
  --font-mono: "Cascadia Mono", "Consolas", ui-monospace, monospace;
}
```
* **Bundle Gentium Plus Regular, Italic, Bold** (about 2.5 MB; BoldItalic skipped, synthesised bold-italic is not used)
  as `@font-face` from the app origin with `font-display: swap`, preloading Regular. Reason: it is the only candidate
  that is designed for exactly this mix (Latin quantity marks plus polytonic Greek), the warmest to read for beginners,
  and the files are small. `unicode-range` is not used (one script mix per line must stay in one face).
* **Cardo is the in-app fallback** only if the owner later finds a rendering problem on Windows with Gentium; it is
  not shipped in v1 (saves 0.4 MB and one more face to test); keep its OFL notice ready.
* Windows 10/11 fallback chain if the bundled file fails to load: Cambria and Palatino Linotype (both have Greek
  Extended; unverified on every Windows 10 build), Times New Roman, Noto Serif (not preinstalled), generic serif.
* Emoji: never rely on the text font; the emoji span uses `--font-emoji` where **Segoe UI Emoji** ships with Windows
  10 and 11 (colour via DirectWrite in WebView2; unverified on Windows 10 1809-) and renders the Windows-version look
  (flat on 10, Fluent on 11). Noto Color Emoji is named only for Linux test runs.
* UI chrome uses the system font so the app looks native; any Latin/Greek text, including macron-bearing dictionary
  headwords in menus, uses `--font-text` via the `.vp-text` class and a `lang="la"` / `lang="grc"` attribute.
* `font-feature-settings`: `"liga" 1, "kern" 1`; no `smcp`/`onum` by default (Gentium Plus defaults are right); never
  use `font-variant-caps` on Latin text (it changes `V`/`U` shapes for students).
* Sizes (rem, base 16 px, user scale 90-140 %): cue text 1.125 rem (18 px) / 1.5; cue list row 1 rem; inspector headword
  1.5 rem Gentium Plus Bold; UI labels 0.875 rem; timing mono 0.8125 rem. Minimum text anywhere: 12 px.
* Subtitles in the preview strip use `--font-text` 1.25 rem so macrons are legible; file output never embeds fonts.

### 2.3 Layout, components and states

8 px grid; targets >= 44 px for buttons and tabs (rows in lists 48 px are targets); cards 12 px radius; one primary
button per view (filled terracotta/blue by pair), secondary = outline with `--border-strong`, tertiary = text. Focus ring
3 px `--focus` with 2 px offset on every interactive element, never removed. Toasts bottom-centre, 6 s, with Undo
when the action is undoable, `role="status"`. Empty, loading, error states each have an illustration-free text card
with the next action. Language pair accent: Latin = terracotta, Greek = Aegean blue (swaps `--accent` at the
workspace root class `.pair-grc`).

## 3. Logo

### 3.1 Concepts (original, none copies an existing mark)

* **A. "Arch and macron".** A Roman arch (the doorway to the old language) with a long-vowel macron floating above it
  like a roof-beam, and a single quill leaf standing inside the arch (the poet). Reads as "a door to Latin you can
  walk through"; works at 16 px as arch + bar; uses the product's own typographic fingerprint (the macron).
* **B. "Laurel book".** An open book whose two pages curve up into a laurel wreath (poeta laureatus). Friendly and
  clear, but laurel+book is the most common classics cliché and detail is lost under 24 px.
* **C. "Two tongues".** A lowercase **v** drawn as a quill nib whose left arm is terracotta (Latin) and right arm blue
  (Greek) with a dot as the ink drop. Very simple and bilingual, but the "v" reads as a checkmark and the macron
  (the app's distinctive element) is missing.

**Pick A** (Arch and macron): distinctive silhouette, legible at tiny sizes, carries both brand colours and the
macron. Concept C's two-colour idea is kept for the Greek-mode variant (arch turns blue).

### 3.2 Construction plan for `assets/logo.svg` (viewBox `0 0 256 256`, all coordinates in that space)

Palette (same in light and dark themes; the mark has its own tile):
Ink-blue tile `#22344A`, terracotta `#E2704A`, parchment `#FBF7F0`, gold `#E8B84A`. Measured contrast on the tile:
parchment 11.9:1, gold 6.9:1, terracotta 4.0:1 (graphic object, 3:1 suffices).

1. **Tile:** `<rect x="16" y="16" width="224" height="224" rx="52" fill="#22344A"/>`. (Corner radius ratio 0.23.)
2. **Arch (Latin):** one path, stroke only, centre x = 128. `<path d="M76 196 V128 A52 52 0 0 1 180 128 V196"
   fill="none" stroke="#E2704A" stroke-width="26" stroke-linecap="butt" stroke-linejoin="round"/>`. Outer width
   104 + 26 = 130; apex outer edge at y = 128 - 52 - 13 = 63; feet end at y = 196 (flat, like stonework).
3. **Threshold line:** `<rect x="62" y="196" width="132" height="12" rx="6" fill="#E2704A"/>` (the step under the
   arch; omitted in the 16/24 px variant).
4. **Macron (the beam):** `<rect x="84" y="34" width="88" height="14" rx="7" fill="#E8B84A"/>`; centre x = 128; 15 px gap
   to the arch apex (48 to 63). Length : arch width = 0.68, so it reads as a vowel mark, not a lintel.
5. **Quill leaf (the poet):** inside the arch opening (inner x 89-167, inner apex y = 89): path
   `M112 190 C100 150 118 112 150 98 C154 134 140 168 112 190 Z`, fill `#FBF7F0`; **rachis cut** (negative line)
   `<path d="M112 190 L146 108" stroke="#22344A" stroke-width="4" stroke-linecap="round" fill="none"/>`; **nib**
   `<path d="M112 190 L106 204" stroke="#FBF7F0" stroke-width="5" stroke-linecap="round" fill="none"/>`.
   The leaf leans 20 degrees to the right (writing direction) and its tip stays 9 px below the arch apex inner edge.
6. **Greek variant** (used when the workspace is in a Greek pair, and for the Greek installer tile): same shapes, arch
   `#86B8E3`, macron replaced by a **circumflex-style tilde** `M88 46 q10 -14 20 0 t20 0 t20 0 t20 0` stroke `#E8B84A`
   width 7 round (a perispomeni, the Greek long mark), quill unchanged.
7. **Small variant `logo_small.svg`** (16-32 px): drop the threshold line, quill and rachis; arch stroke 32, macron
   height 18; check that it survives at 16 px.
8. **Wordmark `logo_wordmark.svg`:** text converted to outlines from **Gentium Plus Bold** (OFL allows embedding
   outlined glyphs in artwork; unmodified font, no renaming issue because no font file is distributed), lowercase
   `vetus poeta`, colour `--text`, 0.04 em letter-spacing, mark at left with height = 1.6 x-height x 4. Optional
   owner decision: set it as "vetus poēta" with a macron on the e (a nod to correct quantity); D1 fixes the wordmark as
   "vetus poeta", so the macron version is **only a proposal**.
9. **Files to produce (M4.6):** `assets/logo.svg`, `logo_small.svg`, `logo_wordmark.svg`, `logo_grc.svg`, `icon.ico`
   (16, 20, 24, 32, 40, 48, 64, 128, 256 px; PNG-compressed), `logo_256.png`, `logo_512.png`, `splash.png`
   (parchment `--bg`, mark 160 px, wordmark under it). Generator script in `tools/` like the prototype's
   `make_icons.py`. Test: render all sizes in headless Chromium and diff against stored hashes.

## 4. Interaction details

### 4.1 Cue review flow

1. After "Translate" finishes (progress shows cues/s and a Cancel button; the list fills as cues arrive, updates
   coalesced to 10 per second), the list opens on the **first yellow or red cue** and the filter shows "Needs
   review (15)".
2. `↓`/`↑` (or `J`/`K`) moves; `Enter` accepts the cue (it gets a green tick and a tiny "reviewed" mark, independent of
   the engine confidence); `E` edits; `1`-`3` choose an alternative; `Backspace` marks "undo my accept". A reviewed
   cue is locked from bulk re-translation.
3. `Shift+Enter` accepts and moves to the next cue needing review; at the end a card says "All cues checked. Export?".
4. "Accept all green" button exists in the list header with a count and an Undo toast (never silent).
5. Review state is stored in the project (`cue.state`: new, translated, edited, reviewed, stale) and drives the
   status bar counters; progress survives crashes through autosave.

Confidence meaning (shown in a legend popover and in the tour): **OK** = every word known, all checks pass, no
ambiguity above threshold; **Check** = ambiguity (two plausible senses), guessed name, reading-speed breach,
song/idiom heuristics, or a model/online disagreement; **Fix** = unknown word, failed agreement or government check,
source not understood, markup problem.

### 4.2 Correction memory and glossary

* **Correction memory:** when the user edits a cue the app asks once (inline chip, not modal): "Remember this
  change? [This phrase] [Just this cue]". The memory stores `source phrase (normalised) -> target phrase` with a
  scope (project; "all my projects" is a later option) and a counter. It is applied before the rule engine on later
  cues, shown with a "From your corrections" reason, listed and removable in the *Corrections* tab, and exported with
  the project. Edits that only change word forms keep the lemma-level preference instead ("use *sērō* for 'late'").
* **Glossary of proper names:** the *Names* tab lists detected candidates (capitalised words not at sentence start, or
  from a built-in table) with occurrence counts and one policy per name: **Keep** (as in source), **Decline** (Latin
  endings: Alicia, Aliciae), **Translate** (Leporem Album) and a typed Latin form. Setting a policy re-translates only
  the cues containing the name, shows how many, and offers Undo. Names inherit gender/declension guesses that the
  teacher can overwrite. The glossary is part of the project file (D16) and can be exported/imported as CSV.

### 4.3 Keyboard shortcuts (all also have buttons)

| Keys | Action |
|---|---|
| `Ctrl+O` / `Ctrl+N` / `Ctrl+S` | Open file / New / Save now (autosave stays on) |
| `Ctrl+Z`, `Ctrl+Y` (and `Ctrl+Shift+Z`) | Undo, redo |
| `↓ ↑`, `J K` | Next, previous cue |
| `Ctrl+↓` / `Ctrl+↑` | Next, previous cue that needs review |
| `Enter`, `Shift+Enter` | Accept; accept and go to next needing review |
| `E`, `Esc`, `Ctrl+Enter` | Edit, cancel edit, accept edit |
| `1` `2` `3` | Choose alternative |
| `Tab` / `Shift+Tab` | Move through words of the target pane, then to the right panel |
| `Space` on a word | Open the Word inspector |
| `W` | Toggle "Why this word?" open |
| `Ctrl+F` | Search cues |
| `Ctrl+M`, `Ctrl+E` | Toggle macrons, emoji display |
| `Ctrl+Shift+E` | Export |
| `F1` / `?` | Help and tour |
| `Ctrl+,` | Settings |
Shortcuts are listed in a `?` popover generated from one table (`VP_Keys`), localised, and never fire while typing in
an input unless they use `Ctrl`.

### 4.4 Undo/redo

Command-based history in the UI store (not the engine's strokes as in the prototype): edit cue, accept, choose
alternative, change policy of a name, bulk translate (one entry), accept-all-green, add/remove correction. Cap 500
entries or 5 MB of diffs, whichever first (oldest dropped with a one-time notice); each entry stores only the changed
cue fields. Undo is instant (local), then sent to the engine as `cue.set` events so the project and autosave stay
authoritative. History is cleared on project close, not on autosave.

### 4.5 "Why this word" panel

Always one click from any target word (also from the source word in reverse), always the same four blocks:
**Meaning** (which sense of the source word and why: the context words used), **Candidates** (ranked list with tier
badge, frequency band and a one-line reason each; the chosen one marked), **Form** (case/number/gender or
person/tense/voice/mood in words with the grammar term; "Other forms" opens the paradigm table with the used cell
highlighted; macrons shown), **Evidence** (Wiktionary, Whitaker, model, online each with tick, cross or "off"). Every
line is data from the engine. If the model contributed, a line says so ("the local model preferred this sense"); if
the cue came from the correction memory it says that instead. Never claims certainty that is not computed.

### 4.6 Didactic touches for students

* **Hover/focus gloss:** hovering (or focusing) a Latin/Greek word for 400 ms shows a small card: dictionary form with
  macrons, short English/Spanish meaning, the form in words ("accusative plural"), tier badge. Same card for source
  words in the Orbergise mode. Cards are keyboard reachable and dismiss on `Esc`.
* **Tier badge:** three laurel leaves (T1 = 3 filled), 16 px, with a text alternative "core word"/"common word"/"rare
  word" (names from i18n, wording by the teacher). A file-level bar shows % of words per tier.
* **Emoji as comprehensible input:** an emoji follows an unambiguous depictable noun (hand-curated table, D9), in the
  app only by default, at 0.9 em with a tooltip naming the word; clicking opens the inspector. If a noun has several
  senses, **no** emoji is shown (wrong picture is worse than none). The toggle is in the Engines tab and `Ctrl+E`.
* **Show grammar:** optional colouring/underlines by case (nouns) and a left-to-right reading aid that highlights the
  verb. Off by default; remembered per user.
* **Macron toggle** hides/shows quantity marks (D13); the preview strip reflects the Export setting.
* **Words in file** list with tiers (see 1.2) doubles as a vocabulary sheet.
* **Read aloud** is not planned (no TTS offline in scope); noted as a possible later feature, not designed.

### 4.7 Reading speed and line breaking

Reading speed shown per cue as characters per second beside the timing, using the Netflix-style defaults
(17 cps adult, 20 cps children's programme: **numbers from memory, unverified**, configurable in Settings > Learning).
Over the limit: yellow chip "fast", tooltip with the exact cps and the cue duration; never changes timing (CLAUDE.md).
Hints offered by the engine: "use shorter synonym (T1)", "merge with previous cue is not possible (timing is
fixed)". Line breaking follows D15: re-break at 42 characters, max 2 lines, break after punctuation or before
conjunctions/prepositions, avoid leaving a single short word on the second line (balanced lines), never break inside
a word, a tag or an ASS override block, keep a dialogue dash with its speaker's line. The cue editor shows a thin
ruler at 42 characters; breaches show in the preview strip with a red line.

### 4.8 Errors, offline guarantee and recovery in the UI

* Engine errors show a toast with the localised title for the error code and the engine's `hint` verbatim only if the
  UI has no translation for that code (the engine's hint is English).
* An always-visible **network indicator** in the status bar: "Offline" (default) or "Online check on" (amber), so the
  teacher can see that nothing leaves the computer. Any request attempt while the switch is off is a bug (CLAUDE.md).
* Engine crash: banner "The translator restarted. Your work is safe." and the project reopens through `project.recover`;
  in-flight translation resumes from the last finished cue.
* Corrupt or truncated project: "This file is damaged. We can open the last autosave from 12:03. [Open autosave]
  [Choose another file]".

## 5. i18n string-key conventions and es-MX register

### 5.1 Files and mechanics

* `gui/ui/i18n/en-US.json` and `gui/ui/i18n/es-MX.json` (D17; names as in CLAUDE.md), flat objects of
  `"key": "string"`, UTF-8, keys sorted alphabetically (a test enforces order so diffs are small).
* `VP_I18n.t(key, vars)`; `vars` fill `{name}`; **no string concatenation to build sentences** and no HTML inside
  strings (emphasis is done with separate keys such as `.lead` / `.rest` or a tiny markup token set `[b]…[/b]` parsed by
  `VP_I18n`, decided in DESIGN). Language switch is instant: strings re-bound through `data-i18n`,
  `data-i18n-title`, `data-i18n-placeholder`, `data-i18n-aria` attributes plus an `onLanguageChanged` list for
  dynamic parts (listeners removed on screen teardown).
* Plurals: keys end `.one` / `.other` (English and Spanish use the same two categories); `t("cues.count", {n:1})` picks
  `cues.count.one`; `.zero` optional. Numbers formatted by `VP_I18n.num` (en-US `1,019`, es-MX `1,019`: Mexico uses the
  comma as thousands separator and the period as decimal separator, same as en-US).
* Latin/Greek words, lexicon glosses, grammar example words and emoji are **data**, never in the string tables.
  Grammar terms are strings (`grammar.case.accusative` = "accusative" / "acusativo").
* Engine error codes map to `error.<code>.title` and `error.<code>.hint` (codes listed in PREPLAN 4.5); the engine's own
  `hint` is the fallback.

### 5.2 Key naming

`<area>.<component>[.<part>].<kind>` all lowerCamelCase segments separated by dots. Areas: `app`, `start`, `workspace`,
`cue`, `source`, `target`, `inspector`, `why`, `engines`, `fidelity`, `names`, `corrections`, `words`, `orbergise`, `export`,
`settings`, `about`, `tour`, `grammar`, `tier`, `confidence`, `error`, `toast`, `dialog`, `a11y`, `unit`, `key`.
Kinds: `.label` (visible text), `.title` (heading), `.hint` (secondary help), `.placeholder`, `.tooltip`, `.aria`
(screen-reader-only name), `.cta` (button text), `.confirm`, `.empty` (empty-state text), `.one/.other`.
Examples: `start.sample.cta`, `workspace.cue.status.ok.label`, `confidence.check.tooltip`, `engines.online.hint`,
`export.options.emoji.label`, `error.model_missing.title`, `a11y.cueList.aria`, `unit.cps.other`.
Dynamic key families (e.g. `grammar.case.*`, `tier.*`, `error.*`, `tour.step1.title`…) are declared in
`tools/jstest/dynamic_keys.json` so the completeness test can see them (same trick as the prototype's `DYNAMIC_KEYS`).
Rules enforced by `tools/jstest`: both files have identical keys; no empty values; identical `{placeholders}`;
every `t("literal")` and `data-i18n*` in the code exists; no key unused for two releases (warning).
Layout budget: Spanish is typically 15-30 % longer; every control must fit with +40 % text (a pseudo-locale `xx-LONG` in
the test harness doubles vowels to verify).

### 5.3 es-MX register and vocabulary decisions

* **Address:** informal **tú** in sentences ("Elige un archivo", "Tu trabajo está a salvo"), because users are
  students and a teacher; **infinitive for buttons and menu items** ("Abrir", "Guardar", "Exportar", "Deshacer"). No
  *vosotros*, no *vos*. Prompts that need politeness toward the teacher use neutral wording without pronouns.
* **Words:** *archivo* (not *fichero*), *computadora* (not *ordenador*), *descargar*, *guardar*, *carpeta*, *clic* ("haz clic"),
  *subtítulos*, *docente*/*profesor o profesora* (neutral: "docente"), *estudiantes* (no *@*, *x* or *e* endings), *traducción*,
  *vocabulario*, *palabra*, *fuente* for typeface in settings ("Tipo de letra" preferred to avoid the ambiguity with "source"),
  *origen* for the source text pane ("Texto original" in the pane title), *velocidad de lectura*, *caracteres por segundo*.
* **Latin-teaching terms** use the usual Spanish school terminology: nominativo, vocativo, acusativo, genitivo, dativo,
  ablativo; declinación, conjugación; presente, imperfecto, perfecto, pluscuamperfecto, futuro; indicativo, subjuntivo,
  imperativo, infinitivo; voz activa/pasiva; *cantidad vocálica* and *macrón* (mark over long vowels, also acceptable "raya
  sobre la vocal larga" in explanations); *palabra básica* for T1 in student-facing text. The teacher reviews all grammar
  explanations (strings authored in English and Spanish by the teacher, not machine-translated).
* **Punctuation and format:** inverted `¿ ¡`; quotation marks “ ” (the prototype's "Saved “{name}”" pattern);
  dates `dd/mm/aaaa`, 24-hour time; numbers with comma thousands and period decimals (Mexico); `%` attached to the number.
* **Tone:** short, calm, no exclamation marks except celebrating completion; errors say what happened and what to do
  ("No pudimos abrir el archivo. Revisa que no esté abierto en otro programa."), never blame.
* **Review:** a native es-MX reader signs off the file; a test fails the build on obvious Spain-only words from a
  deny-list (`ordenador`, `fichero`, `vale`, `vosotros`, `coger`).

## 6. Accessibility and performance budgets

### 6.1 Accessibility

* Target WCAG 2.2 AA: text contrast >= 4.5:1 (verified for tokens in 2.1), non-text >= 3:1 (outline tokens), focus visible
  3 px ring, target size >= 44 px for buttons/tabs/rows (WCAG minimum is 24 px), no information by colour alone
  (shape + word for confidence), no time limits, no flashing, `prefers-reduced-motion` and `forced-colors` (Windows
  High Contrast) honoured (borders become `CanvasText`, chips keep their shapes).
* Semantics: landmarks (`header`, `nav`, `main`, `aside`, `footer`); the cue list is a `role="listbox"` (or `grid`) with
  `aria-rowcount` = total cues and `aria-rowindex`/`aria-posinset` on rendered rows (needed because the DOM holds only
  a window), `aria-activedescendant` for the selection; chips have `aria-label` ("Cue 220, needs checking"); the
  autosave and progress texts live in `aria-live="polite"` regions; dialogs use `role="dialog"` + `aria-modal` with focus
  trap and focus return; the tour is announced step by step.
* Language tagging: Latin spans `lang="la"`, Greek `lang="grc"`, so screen readers and hyphenation pick the right
  voice (Windows Narrator may have no Latin voice; **unverified**; fall back to reading letters is acceptable); UI strings
  carry the page `lang` (en-US / es-MX switched at runtime on `<html lang>`).
* Keyboard: everything reachable without a mouse; no keyboard traps except dialogs; visible shortcuts list; focus never
  lost on re-render (the virtual list restores focus to the same cue index).
* Text size 90-140 % and full layout reflow at 1280x800 minimum; the minimum window is 1024x640 with the right panel
  collapsing to a drawer (below 1180 px).
* Test: axe-core is not assumed available (no npm packages are allowed); instead `tools/jstest` runs a small structural
  linter (names on buttons, `alt`, labels, unique ids, contrast of token pairs from CSS) and the owner does a keyboard-only
  pass and a Narrator pass before release.

### 6.2 Performance and memory budgets (UI process, WebView2 on i3 / 4 GB)

| Budget | Value | How it is enforced |
|---|---|---|
| Rendered cue rows | window = visible rows + 2 x 8 overscan, hard cap **40 rows**; fixed 48 px row height | cue list virtualiser, test mounts 50,000 cues and counts `.vp-cue-row` nodes |
| DOM nodes in the workspace | <= 800 total (about 12 per row x 40 + chrome + inspector) | `document.getElementsByTagName('*').length` asserted in smoke test |
| Event listeners | one delegated listener per container; every `addEventListener` goes through `VP_Dom.on` which records it; `VP_Dom.count()` must return to baseline after 50 mount/unmount cycles of each screen | jstest leak test and Playwright smoke |
| Timers | no free-running `setInterval`; autosave age text updates every 15 s only while visible; all timers owned by a screen are cleared in its `destroy()` | lint: `setInterval` only in `VP_Timers`; test counts live timers |
| JS heap | <= 120 MB with a 5,000-cue project loaded; <= 60 MB idle; cue data stored as compact objects, large source/target strings kept once | Playwright `performance.memory` (Chromium) in the smoke test |
| Message handling | engine events coalesced: at most 10 UI updates/s during translation, batches applied in one `requestAnimationFrame`; progress is a single element update | bridge test with 1,000 events/s fake engine |
| Caches | inspector LRU 200 entries; hover-card LRU 100; alternatives only for the selected cue +-20; every cache has a cap and clear-on-project-close | unit tests per cache |
| Large arrays | on project close: set to `null`, drop references, ask the engine to release; no object URLs or base64 images in v1 | smoke test: open/close project 20 times, heap returns within 10 % |
| Startup | window shown < 1.0 s, interactive < 2.0 s on i3 (target, **unverified** until measured by the owner); fonts preloaded, nothing blocks first paint | Playwright timing on the dev machine, owner measures on the i3 |
| Interaction latency | select cue <= 100 ms, keystroke in editor <= 50 ms, scroll 60 fps on 50,000 cues in the container's Chromium (software rendering is the pessimistic case) | smoke test records long tasks > 50 ms |
| Assets | CSS <= 60 KB, JS <= 300 KB gzip in total (about 30 files), fonts <= 2.6 MB, SVG sprite for icons (no icon font, no bitmaps) | `pack_ui` size report fails the build when exceeded |
| Fonts | three Gentium Plus faces, `font-display: swap`, no web fonts from the network (CSP `default-src 'self'`) | CSP header test |
| Engine/UI boundary | the UI never holds more than the visible window of cues in the DOM and never more than 50,000 cue records in JS; larger files are paged by the engine (`cue.page {from,count}`) | contract in DESIGN |

Memory discipline copied from CLAUDE.md for JS: remove listeners you add, cap caches, null out large arrays, never keep
more than the visible window of cues in DOM.

### 6.3 Test harness hooks the UI must expose (so the budgets are checkable)

`VP_Debug.stats()` returns `{listeners, timers, domNodes, cueRows, caches:{name:size}}` (disabled in release builds by a
flag; present in the mock-engine browser build); the mock engine can generate N synthetic cues and fake progress
events; screenshots (light/dark, en-US/es-MX, Latin/Greek) are generated by a Playwright script like the prototype's
`dev/screenshots.py` and reviewed by the main agent.

## 7. Open points for the main agent / owner

1. Direction of the fidelity slider relative to tiers (1.2.2). 2. Wordmark with macron on "poēta" (3.2 step 8).
3. Whether Latinitium may be queried at all (terms unverified). 4. Teacher input needed for: T1 list, grammar help text, tier
labels, emoji list approval. 5. Windows DirectWrite check of combining marks in Gentium Plus (owner machine).
6. Whether "Text" kind (no file) is v1 or v1.1; it is cheap in the UI but the engine must offer paragraph segmentation.
7. Confirm reading-speed limits (17/20 cps) and the 42-character rule with the teacher's players.
