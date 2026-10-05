# PREPLAN — vetus poeta (pre-plan for M0.4, written 2026-10-05)

Status: input to `docs/DESIGN.md`. Nothing here is binding; `docs/DECISIONS.md` is. Every number marked
"measured" was computed on 2026-10-05 from files in `data/raw/` (Kaikki dump dated 2026-10-03) or by running the
tool named. Anything I could not check is marked **unverified**. The measuring scripts were throwaway (scratch
directory, not committed); task M1.1 must reproduce the numbers and emit them as a build report.

Reading order: section 0 (what changes the plan), then 4 (tasks), then 1-3, 5, 6 as needed.

## 0. Findings that change the plan

1. **Latin to English is well covered; English to Latin is not.** The Kaikki Latin and Ancient Greek files contain
   **zero** `translations` entries (measured, full files). The English-edition file has only **3,397** rows
   translating into Latin (3,256 distinct English-Latin pairs, 2,032 English entries) and **1,816** into Ancient
   Greek (1,757 pairs). es.wiktionary adds 1,838 Spanish-Latin and 211 Spanish-Greek pairs. So the EN/ES to LA/GRC
   lexicon must mostly be built by **inverting the English glosses of 48k Latin and 21k Greek lemmas**, ranked with
   Whitaker frequency, DCC core ranks and the hand-written tiers. This is the largest data risk (section 5, R1).
2. **Inflected forms are almost free; generation can be a lookup.** Latin: about **1.10 M** distinct
   macron-stripped surface forms with tags; Greek: about **0.95 M**. Wiktionary's own tables carry the forms, so the
   rule engine selects a form from a lemma's table rather than implementing declension/conjugation code. Rule-based
   paradigm generation is only a fallback (Whitaker INFLECTS.LAT has ~1,750 ending rows).
3. **Macrons are not in the `word` field.** `word` is always macron-free (0 of 3,000 sample entries and 0 in
   Greek). Macrons live in `forms[].form`, `head_templates[].expansion` and `head_templates[].args`. Greek
   vowel length (breve/macron) is in `forms[].form` as combining marks; `word` keeps accents and breathings only.
4. **No frequency data in Kaikki.** Frequency/tier data must come from Whitaker (A-F codes, 39,335 entries), the DCC
   core lists (997 Latin, 523 Greek, CC BY-SA 3.0, Spanish version of the Latin list exists) and our own counts over
   CC BY-SA Perseus corpora. **No open Orberg (LLPSI) or Athenaze word list exists** (searched; only commercial
   glossaries). T1 needs the teacher's input or a derived approximation (section 1.8).
5. **The 0.5B model cannot generate Latin.** Measured with Qwen2.5-0.5B-Instruct Q4_K_M under the vendored-version
   llama.cpp: greedy EN to LA outputs were Italian or nonsense; of two LA to EN sentences one was mostly right (puella read as "witch") and one wrong; in
   10 grammatical/ungrammatical minimal pairs it preferred the correct Latin/Greek sentence 6 times (chance level at
   n=10). Speed on this box is fine (32-58 tokens/s). Engine (ii) must therefore be **optional, English/Spanish-side
   first (sense disambiguation), with a measured on/off gate for any Latin-side reranking** (section 2.4, R6).
6. **Fonts and llama.cpp are in good shape.** Gentium Plus, Cardo and Noto Serif were downloaded, are OFL, and cover
   all 233 assigned Greek Extended code points plus macron/breve vowels. llama.cpp b11433 (MIT) builds CPU-only
   and static in 60 s on 4 cores with `LLAMA_BUILD_COMMON=OFF`; libs total ~13 MB.
7. **Share-alike scope needs a lawyer.** Kaikki/Wiktionary (CC BY-SA 4.0), Perseus LS/LSJ (CC BY-SA 4.0), DCC
   (CC BY-SA 3.0). The compiled lexicon file is an adaptation and must ship under CC BY-SA with attribution and as
   a separate, replaceable data file next to the proprietary program. Whether the program itself is affected is a
   legal question I cannot answer (R12). Non-commercial treebanks (UD Latin-Perseus/PROIEL/ITTB, UD Greek-Perseus/
   PROIEL: CC BY-NC-SA) are **excluded**.

## 1. Data inventory (verified)

### 1.1 Kaikki Latin: `kaikki.org-dictionary-Latin.jsonl`

| Fact | Value (measured) |
|---|---|
| URL | https://kaikki.org/dictionary/Latin/kaikki.org-dictionary-Latin.jsonl, HTTP 200, range requests work (`accept-ranges: bytes`) |
| Size / date | 1,231,621,764 bytes, `last-modified` Sat 03 Oct 2026; local copy `data/raw/kaikki-Latin.jsonl` is complete (last line parses) |
| Lines (entries) | **892,320** (one JSON object per line, UTF-8, longest line 146,546 chars) |
| Distinct `word` strings | 842,381 (equals the "All word forms (842381 distinct words)" figure on kaikki.org) |
| Licence | kaikki.org/dictionary/ states the data is "made available under the same licenses as Wiktionary" (CC BY-SA 4.0 and GFDL) |
| `lang_code` | `la` for every line (3,000 of 3,000 in sample) |

**Entry kinds (full file).** 843,783 entries are pure form pages (every sense has `form_of` or `alt_of`, or the tag
`form-of`); 48,537 are lemma-like by that test. Caveat: about 5,000 of the "lemma-like" verb entries are really
form pages whose senses lack `form_of` (head template name `head`, gloss like "second-person plural present passive
indicative of X"). Only 59 `la-verb` lemmas lack a conjugation table, so use the head template (`la-verb`, `la-noun`,
`la-adj`, `la-proper noun`, ...) plus a gloss regex to separate lemmas from form pages. 5,852 entries are `alt_of`
(orthographic variants such as conpello for compello).

**Counts in the first 3,000 lines** (requested sample; note it is lemma-heavy: 1,617 of 4,867 senses are form/alt-of,
versus 843k form entries in the whole file, so it is *not* representative):

| Measure | Sample (3,000 lines) | Full file |
|---|---|---|
| entries | 3,000 | 892,320 |
| senses / all with `glosses` | 4,867 / 4,867 | 1,008,871 |
| glosses (strings) | not counted | 1,180,116 (53,078,059 chars, mostly form-page boilerplate) |
| entries with `forms` | 2,535 | table rows 1,929,210 |
| `forms` rows total | 43,624 | |
| entries with `etymology_text` | 1,612 | 58,557 |
| entries with `sounds` | 2,159 | 84,521 |
| entries with `descendants` | 905 | 17,694 |
| `translations` | **0** | **0** |
| senses with `examples` | 854 | 10,527 |
| POS (top) | noun 1,162; verb 807; adj 426; name 225; adv 104; pron 36; num 34 | verb 683,069; adj 101,867; noun 83,000; name 16,703; adv 2,699; suffix 1,822 |
| lemma-like by POS | | noun 15,502; verb 11,101 (6,111 real); name 9,463; adj 9,247; adv 1,822; num 174; pron 100 |

**Schema that matters.**

```
{ "word": "pies",                        // ALWAYS macron-free, original case ("December", "Aulus")
  "lang_code": "la", "pos": "verb",       // pos values: noun verb adj adv pron num prep conj intj det name phrase
                                          //   suffix prefix proverb character symbol postp particle ...
  "head_templates": [{"name":"la-noun","args":{"1":"thēsaurus<2>"},
                      "expansion":"thēsaurus m (genitive thēsaurī); second declension"}],
  "forms": [ {"form":"thēsaurus","tags":["canonical","masculine"]},          // macronised headword
             {"form":"thēsaurī","tags":["genitive"]},                        // head-line forms
             {"form":"no-table-tags","source":"declension","tags":["table-tags"]},      // table markers: skip
             {"form":"la-ndecl","source":"declension","tags":["inflection-template"]}, // skip
             {"form":"thēsaurō","tags":["dative","singular"],"source":"declension",
              "links":[["thēsaurō","thesauro#Latin"]]},                      // real table cell
             {"form":"thensaurus","tags":["alternative"]} ],                 // spelling variants
  "senses": [ {"glosses":["…"], "tags":["declension-2","masculine"], "id":"en-…",
               "form_of":[{"word":"piō"}],            // lemma target, MACRONISED, needs strip to join
               "alt_of":[…], "examples":[…], "qualifier":…, "raw_tags":…, "topics":…, "synonyms":…} ],
  "etymology_text": "…", "etymology_templates":[…], "sounds":[…], "descendants":[…], "derived":[…] }
```

* **Inflection tables:** `forms[]` rows with `source` in {`declension`,`conjugation`,`inflection`}; marker rows have
  tags `table-tags`, `inflection-template`, `class` and must be skipped; `romanization` rows are Greek only.
  Measured in the sample: `source` = conjugation 19,593; declension 18,924; none (head-line forms) 4,700;
  inflection 407. 25,910 of 38,517 table forms in the sample carry a macron (67 %).
* **Tag vocabulary** (sample, rows): singular 17,159; plural 15,111; active 11,633; indicative 7,745; passive 6,989;
  second-person 5,429; present 5,293; subjunctive 5,125; future 5,094; third-person 5,028; first-person 4,850;
  perfect 4,659; genitive 4,138; accusative 3,312; imperfect 3,174; nominative 2,885; vocative 2,870; ablative 2,863;
  imperative 2,854; dative 2,683; pluperfect 1,928; infinitive 1,465; participle 1,325; gerund 540; supine 394;
  locative 90; plus era/register tags (Medieval-Latin 40, Old-Latin 27, Late-Latin 27, New-Latin on 175 senses).
  There are **447 distinct tag combinations** in lemma tables, which fit a 9-bit "feature set" id.
* **Form pages** (e.g. `pies`): gloss "second-person singular present active subjunctive of piō", `form_of`
  [{word:"piō"}], sense tags `["active","form-of","present","second-person","singular","subjunctive"]`, and
  `forms[0] = {form:"piēs", tags:["canonical"]}` giving the macronised spelling; `head_templates[0].args.head` also
  has it. 936,089 `form_of`/`alt_of` references exist. Joined by macron-stripped target to a *lemma-like* `word`:
  **67.0 %**; to *any* entry: **99.8 %**. The gap is chains (306,475 references point at another form page, e.g.
  participle form -> adjective -> verb); the build tool must follow chains to depth 3.
* **Macrons:** precomposed NFC letters (ā ē ī ō ū ȳ), 29,635 forms in the sample use them. `head_templates[].expansion`
  repeats them ("tāgō (present infinitive tāgere, perfect active tetigī, supine tāctum); third conjugation").
  In the sample 1,853 entries have a `canonical` form; 1,847 of those contain a macron and 1,852 differ from `word`.
* **Orthography:** headwords use v and i (vir, iam), j appears only in variants; matches D13 (u/v distinguished, i
  for consonantal j). 56 of 3,000 sample words are multi-word; 263 start with a capital (names).
* **Quirks to code against:** (a) `senses[].glosses` can be a list of 2+ strings where the first is a parent
  category such as "[with subjunctive]" and the last is the real gloss; (b) `head_templates` is missing on 24
  of 3,000 sample entries; (c) `etymology_text` of some entries starts with the text "Etymology tree"; (d) `_dis`/`_dis1`
  fields in translations/coordinate terms are disambiguation noise; (e) JSON lines up to 146 KB: use a streaming
  reader, never `readlines()`.

### 1.2 Kaikki Ancient Greek: `kaikki.org-dictionary-AncientGreek.jsonl`

| Fact | Value (measured) |
|---|---|
| Size | 403,924,438 bytes; local copy complete |
| Entries | **68,196**; lemma-like 21,401; form pages 46,795; `alt_of` 1,844 |
| Senses / glosses | 90,475 / 100,177 (4,090,465 chars) |
| Distinct `word` | 64,151 |
| Distinct surface forms | **953,275** (lower-case, breve/macron removed, accents kept) |
| POS (entries) | noun 22,873; verb 20,213; adj 15,525; name 6,281; adv 844; pron 662; suffix 527; det 386 |
| With `descendants` | 10,705; **6,968** lemma entries have a Modern Greek (`el`) descendant, **5,282** of those spell it identically modulo accents (candidates for "shared with Modern Greek", D12) |
| `translations` | 0 |

First 3,000 lines: 5,505 senses; 111,585 `forms` rows (source: inflection 52,635; conjugation 25,036; declension
24,875; none 9,039); 2,995 entries with forms; 1,500 of 3,000 are `name` entries (sample is name-heavy).

**How `word` relates to forms and accents (verified):**
* `word` = page title: NFC, **with** acute/grave/circumflex, breathings, iota subscript, diaeresis; **without**
  vowel-length marks. Combining marks in sample `word` fields: acute 2,624; comma above (smooth breathing) 897;
  perispomeni 283; reversed comma (rough) 222; diaeresis 41; ypogegrammeni 27; grave 3. 0 of 3,000 are non-NFC.
* `forms[].form` for the headword (`canonical`) and table cells uses **combining breve U+0306 (89,747 uses in
  sample) and macron U+0304 (17,473)** to mark short/long α ι υ: `word` "σκύλος" vs canonical "σκῠ́λος";
  1,918 of 1,926 sample canonical forms differ from `word`. The table cell also has `roman` (transliteration)
  and `links` whose second element is the plain spelling ("σκύλος#Ancient_Greek"): that is the join key to `word`.
  Normalising rule for the build: NFC(strip U+0306, U+0304) is the lookup key; keep the marked form for display.
* Several tables per entry for dialects (rows tagged `Attic`, `Ionic`, `Doric`, `Epic`, `Aeolic`, `Koine`...). A
  `table-tags` marker row ("Attic declension-3", "Ionic declension-3") starts each table; group rows by that
  marker and keep **Attic** (D13). Sample sense-tag counts: Attic 203, Koine 189, Ionic 156, Epic 126, Doric 68.
* Kaikki POS is unreliable for participles: κύων's participle page is `pos: verb` with head template `grc-part-1&3`.
  Use the head template name (`grc-noun`, `grc-verb`, `grc-adj-1&2`, `grc-part-…`) as the real class.
* Head templates (sample): grc-proper noun 1,482; grc-noun 725; head 208; grc-verb 120; grc-adj-1&2 116; tlb 79.
* Accent handling in the engine: forms are stored accented (polytonic NFC); lookup also builds a second key with
  all accents/breathings removed, because pasted or typed Greek is often monotonic or unaccented.

### 1.3 Kaikki English edition (pivot for EN to LA/GRC)

`data/raw/kaikki-English.jsonl`, 3,335,546,346 bytes, 1,492,836 entries (English headwords). Translation rows:

| Target | English entries having it | Rows | Distinct EN-target pairs | Distinct targets |
|---|---|---|---|---|
| Latin (`la`) | 2,032 (noun 1,064; name 327; verb 282; adj 237; adv 38) | 3,397 | 3,256 | 3,066 |
| Ancient Greek (`grc`) | 1,210 | 1,816 | 1,757 | 1,647 |

Row shape: `{"lang":"Latin","code":"la","lang_code":"la","sense":"inorganic compound H₂O","tags":["feminine"],
"word":"aqua","_dis1":"…"}` (sense text is the join key to the English sense; gender in `tags`). Examples seen:
water -> aqua, lympha; king -> rēx; Greek king -> βασιλεύς, ἄναξ. Coverage is tiny (2,032 English entries) next to
what a subtitle file needs, so this source is a **high-precision seed**, not the lexicon. English `forms[]`
(47,260 of the first 60,000 entries have them) give an English inflected-form to lemma table. English entries also
carry Spanish translations (7,123 `es` rows in the first 60,000 entries; full count not measured): this is the
EN-ES pivot of D8.

### 1.4 es.wiktionary extract: `data/raw/es-extract.jsonl.gz`

103,226,106 bytes gz, **1,016,609** entries, Spanish-language glosses. Origin URL not recorded in HANDOFF (**unverified**;
M1.4 must record it and re-check the licence, expected CC BY-SA). Keys: word, pos, pos_title, lang_code, lang, senses
(glosses, categories, sense_index), categories, sounds, hyphenations, translations, forms, etymology_texts.

| Content | Measured |
|---|---|
| Spanish entries (`es`) | 855,637; 86,066 with `forms` (conjugations: tags like first-person, singular, indicative, present; raw_tags "yo"); 828,627 form-of senses |
| **Latin entries (`la`) with Spanish glosses** | **7,022** (verb 2,786; noun 1,756; adj 1,035; participle 322; pron 272; adv 232; name 160; phrase 99); includes IPA and macron-bearing `sounds` |
| Ancient Greek entries | 278 |
| Entries with `translations` | 32,495 |
| Spanish to Latin pairs | 2,134 rows, **1,838 distinct** (e.g. mujer -> femina, plata -> argentum, universidad -> universitas) |
| Spanish to Ancient Greek pairs | 258 rows, 211 distinct |

So Spanish glosses for Latin lemmas exist for ~7k of 48k lemmas; the rest need pivot (Latin -> English gloss ->
Spanish via English-edition `es` translations) or the hand-written tier vocabulary (D8).

### 1.5 Whitaker's Words (mk270/whitakers-words)

Files (all HTTP 200 on raw.githubusercontent.com, `master`): `DICTLINE.GEN` 6,115,855 bytes; `INFLECTS.LAT` 129,314;
`ADDONS.LAT` 34,697; `UNIQUES.LAT` 9,642. (`STEMLIST.GEN` is 404; it is generated.)

**DICTLINE.GEN: 39,335 lines, 7-bit ASCII (0 non-ASCII lines, no macrons), fixed-width, mean meaning length 43 chars.**
Verified column layout (0-based): stems 1-4 at columns 0, 19, 38, 57 (19 chars each, blank or `zzz` when unused);
POS at 76-82; grammar codes at 83-99 (declension, variant, gender/type for N: `9 8 M N`; for V: `1 1 TRANS`; for ADJ
`1 1 POS`; PREP `ABL`); then single-letter codes at columns **100, 102, 104, 106, 108** = age, area, geography,
frequency, source; meaning text from column 110. Example (cols trimmed):
`abact  abact  …  N  4 1 M T  X A X E O  cattle thieving, stealing of cattle, rustling;`

| Code | Distribution (measured) |
|---|---|
| POS | N 19,619; ADJ 9,159; V 7,791; ADV 2,204; NUM 127; INTERJ 106; CONJ 102; PREP 93; PACK 73; PRON 61 |
| Frequency (col 106) | A 2,183; B 2,743; C 5,052; D 8,409; E 11,218; F 8,020; I 430; N 1,280 |
| Age (col 100) | X 28,963; D 3,959; F 1,992; G 1,909; E 1,739; B 456; H 192; C 63; A 62 |

The meaning of the letters (A = very frequent … F = very rare, I = inscription, N = pure name; age A archaic … H
modern, X = all periods) is from Whitaker's documentation as I remember it: **unverified here**; M1.3 must read
the doc in the repo (`doc/`) before using them. `INFLECTS.LAT` rows: `N 1 1 NOM S C  1 1 a   X A` = POS, declension,
variant, case, number, gender, stem-length-to-chop, ending length, ending, age, frequency. Counted rows:
V 654, VPAR 346, N 262, ADJ 226, PRON 169, NUM 127, ADV 6, PREP 3, SUPINE 2, INTERJ 1, CONJ 1.
**Licence** (README.md, lines 51-68): "Permission is hereby freely given for any and all use of program and data. You
can sell it as your own, but at least tell me … All parts of the WORDS system, source code and data files, are made
freely available to anyone who wishes to use them, for whatever purpose." No SPDX identifier exists, so record the
quotation in the attribution screen and ask the owner to accept it (D3 already lists Whitaker as permissive).
Use: (1) frequency/age/area per lemma, (2) an *independent* check that a stem+ending analysis exists, (3) the ending
tables as the paradigm fallback, (4) `ADDONS.LAT` enclitics/prefixes (-que, -ne, -ve).

### 1.6 Perseus lexica (PerseusDL/lexica)

Repo README: "Unless otherwise indicated, all contents of this repository are licensed under a Creative Commons
Attribution-ShareAlike 4.0 International License"; the file headers themselves carry no licence text (**the Perseus
wiki page for each file is unverified**).

**Lewis & Short**: `CTS_XML_TEI/perseus/pdllex/lat/ls/lat.ls.perseus-eng1.xml`, 77,248,803 bytes, **51,596**
`<entryFree>`. TEI P4 (`TEI.2`), DOCTYPE with external DTD (do not resolve it; use a non-validating parser). Lines
1-352 are header plus revision log; entries start at line 353. An entry:
`<entryFree id="n55" type="main" key="abduco"><orth extent="full" lang="la">ab-dūco</orth>, <itype>xi, ctum, 3</itype>,
<sense id="n55.0" n="I" level="1"><hi rend="ital">v. a.</hi> …`. Element counts: bibl 379,302; author 351,449; hi 226,151;
quote 223,260; cit 223,223; **sense 101,992**; **orth 71,911**; **itype 55,543** (principal parts / genitive); gen
29,432; **tr 29,139 inside trans 24,113** (marked translations: the short English glosses); pos 22,121; etym 21,802;
usg 20,416. Macrons appear in `orth` (36,734 `orth` elements contain a precomposed macron vowel); the 2019 change note says
combining marks (macron on letters with no single glyph) follow the base letter. The `key` attribute is ASCII (no
macrons, digit suffix for homographs: `A1`, `a2`). Use: macronised headword, principal parts, a second English gloss
source (`<tr>`), and period/register labels in `<usg>`. Not usable: the citations (enormous and irrelevant).

**LSJ**: the Greek lexicon is split into **27 files**, `…/grc/lsj/grc.lsj.perseus-eng1.xml` … `-eng27.xml` (eng28+ are
404); total **283,615,666 bytes**, **116,497** `<entryFree>`. Structure: `<div1>/<div2>` front matter (abbreviation
lists) first, then entries: `<entryFree id="n1" key="a)1" type="main" opt="n" TEIform="entryFree"><orth extent="full"
lang="greek">a)-</orth>, … <sense n="I" id="n1.0" level="2">`. **Greek is Beta Code**, not Unicode (`a)/lfa`,
`*a` = capital, `)` smooth, `(` rough, `/` acute, `\` grave, `=` circumflex, `|` iota subscript, `_` and `^` for macron/breve, inferred from the LS change log that mentions removing "^ and _ marks" from keys: **unverified for LSJ**); a Beta Code to Unicode converter is task M1.3. `key` is Beta Code, with digit suffix.
Use: breadth beyond Wiktionary (116k entries versus 21k Greek lemmas) for glosses of lower tiers, and
`<gen>`/`<itype>` for gender and endings. Not usable in the first release: LSJ has no inflection tables; its forms
would need generated paradigms.

### 1.7 Frequency lists and core vocabularies (HTTP status checked 2026-10-05)

| Resource | URL | Status | Licence | Size / format | Usable in a proprietary app? |
|---|---|---|---|---|---|
| DCC Latin Core Vocabulary (997 words, ranked, EN) | https://dcc.dickinson.edu/latin-core-list.csv?page&_format=csv | 200 | CC BY-SA 3.0 Unported (stated on https://dcc.dickinson.edu/vocab/core-vocabulary) | 81,391 B CSV; columns Headword, Definition, Part of Speech, Semantic Group, Frequency Rank; XML twin 376,311 B | **Yes** with attribution; SA applies to the derived list file. 992 of 997 first headwords join to Kaikki entries (measured) |
| DCC Latin Core, **Spanish** | https://dcc.dickinson.edu/es/latin-core-list.csv?page&_format=csv | 200 | page-level CC BY-SA 3.0; translator credit Francisco Javier Pérez Cartagena; separate terms **unverified** | 87,097 B CSV, 997 rows, same ranks | Yes if the site licence covers translations (credit them); ideal seed for es-MX glosses (check register) |
| DCC Greek Core Vocabulary (523 words) | https://dcc.dickinson.edu/greek-core-list.csv?page&_format=csv | 200 | CC BY-SA 3.0 | 65,261 B CSV; headwords carry article/forms ("ὁ ἡ τό", "εἰμί, ἔσομαι, impf. ἦν") | Yes. 517 of 524 first tokens join exactly to Kaikki Greek words (the 7 misses: four correlative pairs such as μέν…δέ, ἁπλῶς and πάντως absent as entries, and εἴκοσι(ν) with a parenthesis) |
| Whitaker DICTLINE.GEN freq codes | see 1.5 | 200 | permissive, custom text | 6.1 MB | Yes (with quotation) |
| Perseus canonical-latinLit (TEI texts) | https://github.com/PerseusDL/canonical-latinLit (raw files 200) | 200 | CC BY-SA 4.0 (README) | 334 files named `*perseus-lat*.xml` (git ls-tree); total size **unverified** | Yes, to compute our *own* lemma counts; counts are facts but keep the notice |
| Perseus canonical-greekLit | https://github.com/PerseusDL/canonical-greekLit (Iliad file 2,043,055 B, 200) | 200 | CC BY-SA 4.0 (README) | TEI XML | Yes, same use |
| UD Latin-CIRCSE | raw README 200 | 200 | CC BY-SA 4.0 | small (README mentions annotated passages of 7,714 and 5,580 tokens; total **unverified**) | Yes but too small to matter |
| UD Latin-LLCT | raw README 200 | 200 | CC BY-SA 4.0 | 242,411 tokens, medieval charters | Yes legally, but register is wrong (legal Latin); low weight |
| UD Latin-Perseus / PROIEL / ITTB / UDante; UD Greek-Perseus / PROIEL | raw READMEs 200 | 200 | **CC BY-NC-SA** (2.5 / 3.0) | CoNLL-U | **No** (non-commercial) |
| LASLA frequency dictionary PDF (linked from DCC) | http://promethee.philo.ulg.ac.be/LASLApdf/Dictionnairefrequentiel.pdf | not fetched | not stated | PDF | **No** until a licence is shown |
| Perseus Hopper vocab tool | http://www.perseus.tufts.edu/hopper/vocablist?… | 403 (base page 200) | n/a | tool, not a download | No |
| Wiktionary:Frequency lists/Latin | https://en.wiktionary.org/wiki/Wiktionary:Frequency_lists/Latin | 200 | CC BY-SA | page exists; contains an index of external links, no table found (**unverified in depth**) | Possibly; low value |
| Latin Wikipedia dump | https://dumps.wikimedia.org/lawiki/latest/ | 200 | CC BY-SA 4.0 + GFDL | size **unverified** | Yes for counts; Neo-Latin/ecclesiastical register, weight low |
| Hiberna-CR downloads (linked from DCC) | http://hiberna-cr.wikidot.com/downloads | 403 | unknown | unknown | No |
| Orberg LLPSI vocabulary (Familia Romana, Roma Aeterna) | none found | n/a | copyrighted (FOCUS Publishing glossaries) | n/a | **No.** Needs teacher-provided list; we only store *our own* derived tier assignment |
| Athenaze word lists | none found | n/a | copyrighted (Oxford) | n/a | **No.** Same remedy |

A 2026-10 search found no GitHub repository with an open-licence LLPSI list. Do not copy textbook vocabulary pages.

### 1.8 What "tiers" can be built from (D12), and what cannot

* **Latin T1 (Familia Romana core)**: unavailable as data. Proposed bootstrap, *to be replaced by the teacher's list*:
  DCC rank <= 600 AND Whitaker freq in {A,B} AND age in {X,C}, then a manual pass by the teacher via a review sheet the
  pipeline exports (`tools/build_library/export_tier_review.py`). Mark every word `tier_source = "derived"` or
  `"teacher"`. Size target ~1,500 lemmas.
* **T2** = T1 + DCC (997) + Whitaker A/B/C with age X/C/D (~6-9k lemmas; measured Whitaker A+B+C = 9,978 entries).
* **T3** = everything with a gloss and a form table.
* **Greek T1**: DCC Greek core (523) intersected with "identical modulo accents in Modern Greek" (5,282 candidate
  lemmas) plus a teacher list for Athenaze. Greek frequency beyond 523 words must come from counting canonical-greekLit.
* **Emoji table** (D9) is a hand-curated TSV of ~400 depictable nouns keyed by lemma + sense id; never derived
  automatically (rule: unambiguous only).
* **es-MX tier glosses** (D8, ~3,000 words) are hand-written; seed from the DCC Spanish list (997) and the 7,022
  es.wiktionary Latin entries, then verified by a native es-MX reader. Register conventions in PREDESIGN section 5.

## 2. Local models (engine ii)

### 2.1 Candidates (Hugging Face API + HEAD requests on `resolve/main`, 2026-10-05)

| Model | Q4 file (repo) | Size (bytes) | Licence | Budget (<= 500 MB) | Notes |
|---|---|---|---|---|---|
| **Qwen2.5-0.5B-Instruct** | `qwen2.5-0.5b-instruct-q4_k_m.gguf` (Qwen/…-GGUF) | **491,400,032** (HEAD) | Apache-2.0 | fits by 8.6 MB | 494 M params; model card claims 29+ languages (card claim, not re-read; Latin not known to be among them) |
| same, bartowski quant | `Qwen2.5-0.5B-Instruct-Q4_K_M.gguf` | **397,808,192** (HEAD) | Apache-2.0 | fits, 100 MB spare | the one I benchmarked; llama.cpp reports 373.71 MiB |
| Qwen3-0.6B | official repo only has `Qwen3-0.6B-Q8_0.gguf` 639.4 MB; unsloth `Qwen3-0.6B-Q4_K_M.gguf` 396.7 MB (API listing, not HEAD) | 396.7 MB | Apache-2.0 | Q4 fits via unsloth | blog lists 119 languages incl. "Greek" (modern), no Latin; has a thinking mode to disable |
| SmolLM2-360M-Instruct | official GGUF repo only `smollm2-360m-instruct-q8_0.gguf` | **386,404,992** (HEAD) | Apache-2.0 | fits at Q8 | Q4 builds from other repos not checked; English-centric |
| Gemma-3-270M-it | unsloth `gemma-3-270m-it-Q4_K_M.gguf` | **253,115,424** (HEAD) | "gemma" (Gemma Terms of Use, custom, not OSI) | fits | needs legal read: use restrictions flow down; 140-language claim is for the family, **unverified** for 270M |
| Gemma-3-1B-it | ggml-org `gemma-3-1b-it-Q4_K_M.gguf` | 806.1 MB | gemma | **over budget** | |
| Llama-3.2-1B-Instruct | bartowski `…-Q4_K_M.gguf` | 807.7 MB (IQ4_XS 743.1 MB) | Llama 3.2 community licence | **over budget** at Q4 (as expected) | attribution/naming terms |
| LiquidAI LFM2-350M | `LFM2-350M-Q4_K_M.gguf` | 229.3 MB (API) | "other: lfm1.0" | fits | licence terms **unverified**; llama.cpp has `lfm2.cpp` |

Architectures present in the vendored source: qwen2, qwen3, gemma3, llama, smollm3, lfm2 (`src/models/*.cpp`).
Pre-quantised files never go in git (`*.gguf` is in `.gitignore`); the installer downloads or bundles them with their
SHA-256 (as the prototype's `models/*.sha256` did).

### 2.2 Evidence of Latin/Greek ability

Published evidence: **none found** for any candidate (web search 2026-10; model cards and the Qwen3 blog name no
Latin and no Ancient Greek; Qwen3 blog names Modern Greek). So I measured, with the real library at the pinned
tag (scratch programs, not committed; model: bartowski Qwen2.5-0.5B-Instruct Q4_K_M; greedy, seed 1):

* **Generation, EN to LA (3 prompts):** "The girl is in the garden." -> "La ragazza è in gardens." (Italian);
  "Where are you going, little rabbit?" -> "Dixit, minx?"; "I am late, I am late!" -> "Ero, eri, erat!". 0/3 acceptable.
* **LA to EN (2 prompts):** "Puella in horto sedet et rosam spectat." -> "The witch sat in the garden and watched the roses."
  (puella = girl is wrong; rest ok); "Marcus amicum suum salutat, quia laetus est." -> nonsense. 0.5/2.
  Greek "ὁ ἄνθρωπος ἐν τῇ οἰκίᾳ ἐστίν." -> "The human lives in this world." (wrong).
* **Reranking by log-probability** (sum of token log-probs, 10 minimal pairs: correct vs agreement/case error):
  preferred the correct sentence in 6 of 10 (puella in horto vs puellam…hortus; ego te vs ego tu; puellae vs puella
  rosas amant; leporem vs lepus; and both Greek pairs). It preferred the **wrong** sentence for amicum/amicus, rosas/rosa,
  servo/servus and habitamus/habitat. With n=10 this is not distinguishable from chance. Tokenisation cost: about 2
  tokens per Latin word (a 4-word sentence is 9-10 tokens) and about 6 per Greek word (5 polytonic words = 33 tokens),
  which also makes Greek scoring slow.
* **English-side word sense (5 multiple-choice items):** 3 clear correct (light, bat, duck), 1 wrong (leave), 1
  unparseable. Anecdotal.
* **Speed** (llama-bench on this container: Intel Xeon @ 2.1 GHz with AVX-512, 4 cores; build native; Q4_K_M 373.7 MiB):
  2 threads: prompt 194.1 +/- 15.9 tokens/s, generation 31.9 tokens/s; 4 threads: 293.6 / 57.9. An i3 (AVX2, 2 cores/4
  threads) will differ; **unverified** for the target. Scoring a 12-token candidate costs about one 12-token forward
  (~0.1 s at 2 threads); a 1,000-cue file with 5 candidates each is 5,000 forwards = under 10 minutes worst case.

Consequence for D5: promise "source understanding and disambiguation" on the **English/Spanish side** first; treat
"reranking Latin candidates" as a hypothesis with an automatic go/no-go gate (M5.4): generate >= 500 minimal pairs by
corrupting tables (wrong case/number/person from the lexicon itself), measure pairwise accuracy; ship Latin-side
reranking only if >= 75 % (lower bound of a 95 % interval above 70 %). Vocabulary-constrained decoding with a GBNF
grammar built from the lexicon is available through `llama_sampler_init_grammar`, but with a model that does not know
Latin it only constrains junk: use it for **closed choices** (pick one of N candidate lemmas/orderings) not free
generation.

### 2.3 llama.cpp vendoring (GitHub API blocked; `git ls-remote` and shallow clone work)

* Latest build tag at check time: **b11433**, commit `50569eb87df530daff11afda229ceb9ab8e6cae8` (author date
  2026-10-06 04:04 +05:30 per `git log`). Licence: **MIT** ("Copyright (c) 2023-2026 The ggml authors"; `LICENSE`).
* Vendoring the whole repo is 178 MB; the build needs only these trees (sizes measured): `include/` (96 KB: `llama.h`
  and friends), `src/` (4.4 MB: `llama*.cpp`, `unicode*.cpp`, and `src/models/*.cpp`, 158 files, 1.77 MB, of which
  qwen2/qwen3/gemma3/llama/smollm3 are the ones we use; deleting the rest is possible but breaks upstream CMake, so
  prefer keeping them), `ggml/include/` (256 KB), `ggml/src/` generic part (`ggml.c`, `ggml-alloc.c`,
  `ggml-backend.cpp`, `ggml-backend-reg.cpp`, `ggml-backend-meta.cpp`, `ggml-opt.cpp`, `ggml-quants.c`, `ggml-threading.cpp`,
  `ggml.cpp`, `gguf.cpp`, headers; ~0.9 MB) and `ggml/src/ggml-cpu/` (4.6 MB incl. `arch/x86`, `llamafile`, `repack`),
  the `CMakeLists.txt` files and `cmake/`. **Not needed:** `common/`, `tools/`, `examples/`, `tests/`, `vendor/`,
  every GPU backend directory (`ggml-cuda`, `ggml-vulkan`, `ggml-metal`, ...), `ggml-blas`, `ggml-rpc`, `docs/`, Python
  scripts. We call only the C API in `llama.h` (load, tokenize, decode, logits, sampler chain with grammar).
* **Tested configure line** (Linux, GCC 13.3, CMake 3.28; build succeeded, target `llama`, 70 TUs, 60 s wall on 4
  cores): `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF -DGGML_NATIVE=OFF -DGGML_AVX2=ON
  -DGGML_FMA=ON -DGGML_F16C=ON -DGGML_BMI2=ON -DGGML_OPENMP=OFF -DGGML_CCACHE=OFF -DLLAMA_BUILD_COMMON=OFF
  -DLLAMA_BUILD_TESTS=OFF -DLLAMA_BUILD_TOOLS=OFF -DLLAMA_BUILD_EXAMPLES=OFF -DLLAMA_BUILD_SERVER=OFF
  -DLLAMA_BUILD_APP=OFF -DLLAMA_OPENSSL=OFF`. Output: `libllama.a` 9.75 MB, `libggml-cpu.a` 1.84 MB, `libggml-base.a` 1.37
  MB, `libggml.a` 65 KB. `GGML_NATIVE=OFF` + explicit AVX2 flags is required so the build does not depend on the
  build machine (D4: AVX2 minimum; a CPU without AVX2 must get "model unavailable", not a crash: run a CPUID check).
* **Not tested:** MinGW or MSVC build of this tree (no MinGW in this container: `x86_64-w64-mingw32-g++` absent, so
  `tools/xcompile_check.sh` cannot run here either; **unverified**), and runtime on Windows. M5.1 owns it.
* Update policy: pin the commit in `third_party/llama/VERSION`; upstream moves daily (the repo now has `app/`,
  `skills/` directories not present in older layouts), so never track master.

## 3. Compact lexicon design (`vpl` file, memory-mapped, read-only)

Goal (CLAUDE.md memory rules): one file per language pair set, mapped read-only, **no heap structures**, offsets only.
Latin ~1.1 M surface forms, 48.5 k lemmas; Greek ~0.95 M forms, 21.4 k lemmas.

**Measured inputs:** Latin unique macron-stripped lower-case forms from lemma tables: 786,220 (union with form pages
~1.10 M), mean 11.45 UTF-8 bytes per form (9.0 MB for the 786k, so ~12.6 MB for 1.1 M); analysis tuples
(form, lemma, feature-set) from lemma tables: 1,257,415, mean 1.60 analyses per form; analyses-per-form
histogram: 1 -> 595,976; 2 -> 108,695; 3 -> 32,301; 4 -> 33,086; 5 -> 3,310; 6 -> 9,168; 7+ -> 3,684; 447
distinct feature sets; lemma gloss text (last-level glosses of lemma entries): 2.48 MB for 48.5 k lemmas
(mean 1.51 glosses/lemma).

**Layout (little-endian, 8-byte aligned sections, header with magic, version, section table, SHA-256, licence notice
text block):**

| Section | Content | Latin size estimate |
|---|---|---|
| H | header, section table, licence/attribution notice (UTF-8) | < 16 KB |
| S | **form string table**: sorted macron-stripped lower-case keys, front-coded in blocks of 16, block heads in a separate array for binary search | 12.6 MB raw -> ~5-6 MB (estimate) |
| P | per-key payload offset (uint32 delta-coded per block) | 1.1 M x ~2 B = 2.2 MB |
| A | analysis records: lemma id (3 B), feature-set id (2 B), macron mask (2 B: one bit per vowel for the display form) = 7 B | ~1.75 M tuples (1.6 x 1.1 M, estimate) x 7 B = ~12 MB; varint/delta could give ~8 MB |
| L | lemma records: macron headword offset, pos, gender/decl/conj class, tier (2 bits) + tier_source, frequency rank, emoji id, gloss offsets (EN, ES), flags | 48.5 k x ~32 B = 1.6 MB |
| G | gloss strings, EN and ES (UTF-8, no compression; lemma-level 2.5 MB measured + sense context ~1.5 MB + ES ~2 MB) | ~6 MB |
| F | feature-set dictionary: 447+ rows -> bit-packed (case, number, gender, person, tense, voice, mood, degree) | < 20 KB |
| T | **generation index**: per lemma, offset to its "cells" sorted by feature set -> (form offset) so `generate(lemma, features)` is O(log 50) | ~4 MB |
| R | **reverse (EN/ES to lemma) index**: keyword -> [(lemma id, score u8)], keywords = lemmatised content words from glosses and pivot translations | ~100 k keywords x ~5 candidates x 4 B + strings = ~3 MB |
| M | macron/accent display map for lemma tables when not covered by mask | small |
| **Total Latin** | | **~35-45 MB** (estimate; well under the 100 MB budget) |

Greek is the same layout with an extra accent-stripped key section; estimate 30-40 MB. Spanish and English
morphology tables (inflected-form -> lemma+features; cut to the ~150 k most frequent lemmas each) add ~5-8 MB. Whole
shipped data ~100 MB before any compression; installer compression brings it near 40 MB (unverified).

**Alternatives considered.**
* *Minimal perfect hash* (CHD/BBHash, ~2-3 bits per key): smallest, but it answers "is key k in the set" only with
  a stored fingerprint, gives no prefix/neighbour enumeration (needed for typo suggestions and enclitic stripping
  -que/-ne/-ve) and needs an offline builder dependency. Rejected for v1.
* *DAWG/FST* (Lucene/`fst`-style): likely 2-4 MB for the keys with outputs and supports prefix and fuzzy search, but
  needs a careful vendored implementation (about 800 lines) and a builder; keep as a v2 optimisation behind the
  same reader interface.
* *Sorted string table with front coding + block index* (chosen): ~150 lines to read, trivially mmap-safe, enables
  prefix scan, deterministic bytes. A lookup is one binary search over ~70 k block heads (17 steps) plus a
  linear scan of <= 16 keys; ~1-2 microseconds from page cache; the touched pages are the only RSS.
* Working-set bound: a cue-by-cue run touches a few thousand pages; Windows file-backed pages are reclaimable, so
  they do not count as private bytes; the engine still caps its own caches (< 4 MB).

**Normalisation contract** (shared by the build tool, reader and tests; one implementation each side, tested against
a golden list): Latin key = NFC, lower-case, strip U+0304/U+0306 and precomposed macron/breve vowels, `j` -> `i`,
`v` kept (keys also stored under `u` for lookups of old spelling: build both variants only for entries whose Kaikki
spelling contains v); Greek key = NFC, lower-case, final-sigma folded to sigma, strip U+0306/U+0304, accent-free
secondary key = additionally strip U+0300 U+0301 U+0342 U+0313 U+0314 U+0308 U+0345.

## 4. Module and task breakdown

Rules recap for implementers: one directory each, Opus implementers, max 4 active (D10), depth 2, every task ends
with a REVIEW row and one commit of its own paths. Directory names below are proposals for DESIGN.md (the main agent
may rename them); `engine/*/include/` headers are the contracts.

### 4.1 Repository layout proposed

```
tools/build_library/   LIB    Python 3 pipeline (stdlib only; no pip packages at build time)
tools/jstest/          JSTEST Node-only test runner for ES5 UI code
tools/eval/            EVAL   cue-level error measurement, GT comparison (node + playwright, dev only)
data/curated/          LIB    small hand-curated TSVs (tiers, emoji, proper names, es-MX glosses): committed
data/raw/, data/work/  -      gitignored
engine/core/           CORE   types, utf8/normalisation, errors, project zip + autosave + lock/recovery (ported)
engine/lex/            LEX    vpl reader/writer-format spec, lookup, analyse, generate, reverse index
engine/subs/           SUBS   .srt/.vtt/.ass parse+write, byte-exact timing, tag protection, line breaking, CPS
engine/rules/          RULES  EN/ES analysis, transfer, Latin/Greek realisation, LA/GRC analysis, Orbergise
engine/llm/            LLM    llama.cpp wrapper: load/unload, score, closed-choice, grammar decode
engine/online/         ONLINE Wiktionary (and Latinitium?) client, cache, throttle, mock transport
engine/cli/            CLI    `vpengine serve` JSON-lines server, test subcommands
engine/tests/          each   doctest, one file per module
gui/shell/             SHELL  Win32 + WebView2 host (port of prototype)
gui/ui/                UI     ES5 UI: js/*.js (one IIFE, one VP_* global each), css/, i18n/, fonts/
assets/                ART    logo.svg, icons, splash; assets/fonts/ (OFL files + licences)
third_party/           -      miniz, json.hpp, doctest, llama (pinned), LICENSES.md
```

### 4.2 Task table

Difficulty 1 (hours) to 5 (several days, needs the main agent's review loop). Deps are task ids. "Tests" always
includes ASan/LSan/UBSan for C++ and `tools/jstest.sh` for JS.

| ID | Task | Dirs owned | Inputs | Outputs | Tests | Diff | Deps |
|---|---|---|---|---|---|---|---|
| **M1 Library** | | | | | | | |
| M1.1 | Kaikki streaming reader, normalisation, schema-drift canary, stats report that reproduces section 1 numbers | tools/build_library | data/raw/kaikki-*.jsonl | `data/work/{la,grc}/lemmas.jsonl`, `forms.tsv`, `report.json` | unittest on 200-line hand-made fixtures; canary fails the build if a measured count moves > 15 % or a key disappears | 3 | - |
| M1.2 | Form-of resolution (chains depth 3), tag set to feature-bit mapping, lemma vs form-page classifier, Attic table selection, Greek key normalisation | tools/build_library | M1.1 outputs | `analyses.tsv` (form, lemma id, feature id, macron mask) | fixtures incl. pies/piō, κύων participle, December; golden counts on the real file | 4 | M1.1 |
| M1.3 | Importers: Whitaker (fixed width), Perseus LS (`<tr>`, itype, orth), LSJ (Beta Code to Unicode, 27 files), DCC CSVs, UD CIRCSE/LLCT (optional) | tools/build_library | files in 1.5-1.7 | `whitaker.tsv`, `ls.tsv`, `lsj.tsv`, `dcc_*.tsv` | Beta Code golden table (100 cases), column-offset test | 3 | - |
| M1.4 | English/Spanish glosses: lemma gloss selection, keyword extraction, EN pivot from English edition (la/grc/es translation rows), es-extract Latin glosses, reverse index scoring | tools/build_library | M1.1-1.3, English jsonl, es-extract | `gloss_en.tsv`, `gloss_es.tsv`, `reverse_en.tsv`, `reverse_es.tsv` | fixtures; spot list: water, king, dog, rabbit, late, queen -> expected top-3 lemmas | 4 | M1.2, M1.3 |
| M1.5 | Tiers, frequency, emoji: tier assignment from DCC/Whitaker/teacher TSV, own counts from Perseus texts (tokenise + analyse with M2.5), emoji TSV loader, teacher review export | tools/build_library, data/curated | M1.3, M1.4, curated TSVs | `tiers.tsv`, `freq.tsv`, `review_tier_sheet.csv` | tier monotonic (T1 subset of T2); every emoji lemma exists | 3 | M1.4 (freq counts later need M2.5) |
| M1.6 | Packer: writes `.vpl` per section 3 with checksums and licence block; reproducible bytes | tools/build_library | all TSVs | `data/work/latin.vpl`, `greek.vpl`, `en.vpl`, `es.vpl` | round trip with C++ reader (M2.1) on 10 k random keys; same input twice gives same SHA-256 | 4 | M1.2, M1.4, M1.5, M2.1 |
| M1.7 | ES5 JS test runner and lints (section 4.4) | tools/jstest | gui/ui | `tools/jstest.sh`, runner, ES5 lint, i18n check | self-tests with deliberately bad fixtures | 2 | - |
| **M2 Engine core** | | | | | | | |
| M2.1 | Lexicon reader: mmap RAII (Windows `CreateFileMapping`, POSIX `mmap`), validation (truncated/garbage file never crashes), `lookup`, `prefix`, `analyse`, `generate`, `reverse` | engine/lex | `.vpl` spec in DESIGN | `lex.h`, `libvp_lex` | fuzz with truncated and bit-flipped files; 1M lookups RSS flat; golden analyses | 4 | spec (M0.4) |
| M2.2 | Subtitle I/O: SRT first, then VTT, ASS; encoding detection (UTF-8/BOM/UTF-16/Latin-1), CRLF vs LF preserved, tag protection (`<i>`, `{\an8}`, ASS override blocks) as opaque tokens, byte-identical round trip when text unchanged, line breaker (42 chars, 2 lines), CPS calculator, cue merge/split API | engine/subs | sample SRT/VTT/ASS (own sentences) | `subs.h` | round-trip byte-equality on 50 synthetic files incl. malformed numbering, empty cues, overlapping times; fuzz; timing/numbering bytes untouched asserted by diffing non-text bytes | 3 | - |
| M2.3 | Project format: port of prototype `project.cpp` (zip via miniz, manifest, atomic save, autosave, lock + recovery) with new content: cues, per-cue state, corrections, glossary, settings | engine/core | prototype `engine/core/src/project.cpp`, `fs_util.*` | `project.h` | truncated-zip fuzz, kill -9 recovery, same-bytes save, version migration stub | 3 | - |
| M2.4 | `vpengine serve`: JSON-lines protocol (command list in section 4.5), worker thread, cancel, watchdog ping, error `hint`s, integration test via stdin | engine/cli | M2.1-2.3 headers (stubs allowed) | `vpengine` exe | server test (python) incl. kill -9 + recover, malformed JSON, 10 k-line burst | 3 | M2.3 |
| M2.5 | Morphology: Latin analyser (lexicon lookup + enclitic -que/-ne/-ve strip + u/v and i/j variants + Whitaker stem fallback for forms missing from tables), Greek analyser (accent-insensitive fallback), Spanish and English lemmatisers from tables | engine/rules (morph/) | M2.1 | `morph.h` | held-out forms from Whitaker (not in Kaikki) analysed >= 98 %; ambiguity ranking deterministic | 4 | M2.1 |
| M2.6 | CMake, sanitizer presets, `tools/xcompile_check.sh` port, third_party vendoring (miniz, json, doctest), CI scripts | CMakeLists.txt, tools/ (scripts only), third_party/ | prototype | build works | `ctest` empty-suite passes on Linux; MinGW check where available | 2 | - |
| **M3 Rule engine EN/ES to LA (acceptance path)** | | | | | | | |
| M3.1 | Source analysis: sentence segmentation across cues, tokeniser, lemmatise, POS tagging by rules + lexicon, clause/verb-group chunking (tense, aspect, voice, modals), subject/object detection, questions/negation/imperative, idiom and phrase table, names/NER by capitalisation + glossary | engine/rules (src/) | M2.5 EN/ES lemmatisers | `SourceParse` | 300 hand-annotated sentences (own) | 5 | M2.5 |
| M3.2 | Lexical transfer: choose lemma per content word by fidelity/tier, sense by context rules, closed-class mapping (pronouns, articles dropped, prepositions to case frames), proper-name handling (declined vs kept), emoji attach | engine/rules | M1.6 data, M3.1 | `TransferResult` with alternatives + reasons ("why this word") | per-POS golden tables; determinism | 5 | M3.1, M2.1 |
| M3.3 | Latin realisation: choose forms from tables, agreement (adj-noun, subj-verb), case government, tense/mood mapping (cum/ut/ne clauses, indirect speech simplified), Orberg-style word order (SOV, verb final, simple clauses), enclitics, punctuation, capitalisation, long-vowel display | engine/rules | M3.2, M2.1 | Latin sentence + per-token analysis | checker (M3.5) finds 0 agreement/government errors on 2,000 generated sentences from templates | 5 | M3.2 |
| M3.4 | Cue-level assembly: map one Latin sentence back onto the English cue timing (split/merge across cues at clause boundaries), line breaking, CPS warnings, songs/repeats/ellipsis ("..."), sound cues "[laughs]" policy, speaker dashes, ASS/VTT markup | engine/rules (cue/) | M2.2, M3.3 | translated cue list with per-cue confidence | cue count and timing identical to input (asserted); length-ratio statistics | 4 | M3.3, M2.2 |
| M3.5 | Acceptance tooling: automatic checks A1-A8 (section 6), report generator, regression set runner, per-engine/per-combination matrix | tools/eval | engine exe, SRT files | `report.md/json`, review sheets | checker has its own tests with injected faults (each of A1-A8 caught) | 3 | M2.4 |
| M3.6 | Held-out and regression corpus: our own sentences + public-domain English SRTs with synthetic timing | tests/heldout, tests/regression | PD sources | SRT files | corpus lint (no copyrighted text, timing valid) | 2 | - |
| **M4 UI + shell + brand** | | | | | | | |
| M4.1 | Shell port: Win32 + WebView2, supervisor, recovery, single instance, dialogs, drop files, power status; rename to vetus poeta | gui/shell | prototype `gui/shell` | `VetusPoeta.exe` | portable `host_logic` tests; MinGW syntax check | 3 | M2.4 contract |
| M4.2 | UI core: `VP_Bridge`, `VP_I18n`, `VP_Store`, `VP_Toast`, `VP_Dialog`, `VP_Tour`, Promise polyfill, mock engine, CSP, font loading | gui/ui (js/core) | prototype `engine_bridge.js` | working shell of the app in browser with mock engine | jstest unit tests; i18n completeness | 3 | M1.7 |
| M4.3 | Workspace screens: start screen, source/target panes, **virtualised cue list**, per-cue confidence, edit/accept/undo-redo, find | gui/ui (js/workspace) | M4.2 | screens per PREDESIGN | jstest + Playwright smoke (DOM node count bounded, listener count flat) | 4 | M4.2 |
| M4.4 | Inspector panels: word inspector, "why this word", engine panel (3 toggles, fidelity slider, emoji toggle, Orbergise), glossary of names, corrections memory | gui/ui (js/panels) | M4.2 | | same | 4 | M4.2 |
| M4.5 | Export dialog, settings, attribution/about (licence texts from the .vpl notice), first-run tour, es-MX/en-US strings | gui/ui (js/dialogs, i18n) | M4.2 | | i18n test, screenshot tests | 3 | M4.2 |
| M4.6 | Brand and fonts: logo SVG per PREDESIGN section 3, icon.ico, splash, font files + OFL texts + `@font-face` CSS, tokens | assets/, gui/ui/css/ | PREDESIGN | | rendering test with Playwright screenshot (macrons, polytonic, emoji) | 2 | - |
| **M5 Model + online** | | | | | | | |
| M5.1 | Vendor llama.cpp (pinned), CMake wrapper target, Windows build notes, licence entry | third_party/llama | section 2.3 | `vp_llama` target | builds on Linux; MinGW check | 3 | M2.6 |
| M5.2 | `engine/llm`: load on demand/unload, idle timeout, thread cap, `score(prefix, candidate)`, `choose(prompt, options)`, grammar-constrained decode, fixed seed, RSS accounting, AVX2 CPUID gate | engine/llm | M5.1, a GGUF | `llm.h` | model-less stub for CI; real-model test optional; 1,000-call RSS growth < 5 % | 4 | M5.1 |
| M5.3 | `engine/online`: Wiktionary REST/API client (descriptive User-Agent, throttle 1 req/s, cache in project, honour 429), response parser, mock transport; Latinitium only after its terms are read (unverified) | engine/online | | `online.h` | mock-server tests incl. 429, timeouts, malformed JSON; network call count assertion (zero when off) | 3 | M2.3 |
| M5.4 | Combination logic and measured gates: engine matrix (rules / +model / +online / all), reranking go/no-go from section 2.2, per-combination error report | engine/rules (combine/), tools/eval | M3.x, M5.2, M5.3 | settings-driven pipeline | gate test with generated minimal pairs | 4 | M3.5, M5.2, M5.3 |
| **M6 LA to EN/ES, Greek, Orbergise** | | | | | | | |
| M6.1 | LA to EN/ES: analyse, disambiguate by tier/frequency/context, gloss selection, word-by-word and clause output, "why" data | engine/rules (la2x/) | M2.5, M1.4 | | held-out Latin sentences (own) | 4 | M3.x infra |
| M6.2 | Ancient Greek both ways: Attic tables, accent handling, article/particle handling, dual dropped, modern-Greek-shared preference | engine/rules (grc/), tools/build_library (greek bits) | Greek `.vpl` | | as M3.x | 5 | M6.1 |
| M6.3 | Orbergise: rewrite Latin with T1 vocabulary/structure, optional alignment with original-language file, meaning preserved check | engine/rules (orberg/) | tiers, M3.x | | meaning check against source lemmas; tier coverage report | 5 | M3.x, M1.5 |
| **M7** | | | | | | | |
| M7.1 | Google Translate comparison tool (section 6.5) | tools/eval | | side-by-side HTML/CSV (numbers + own sentences only) | dry run on 5 sentences | 3 | M3.5 |
| M7.2 | Packaging: `make_dist`, installer layout, model download/verify, attribution file | tools/ | prototype `make_dist.py` | | clean-VM smoke (owner) | 3 | all |

**Suggested waves (max 4 active):** A = M1.1, M2.2, M2.3, M2.6; B = M1.2, M1.3, M2.1, M1.7; C = M1.4, M2.4, M2.5, M4.1;
D = M1.5, M1.6, M3.1, M4.2; E = M3.2, M3.3, M3.5, M3.6; then M3.4, M4.3-4.6, M5.x, M6.x.

### 4.3 Python library pipeline (`tools/build_library/`)

Stdlib only (json, gzip, struct, hashlib, unicodedata, xml.etree.iterparse for the Perseus files, csv). Entry point
`python3 tools/build_library/build.py --raw data/raw --out data/work --lang la,grc` with stages that cache their output
and are individually runnable (`kaikki`, `resolve`, `import_aux`, `gloss`, `tiers`, `pack`). Each stage writes a
`stage.json` with input SHA-256s, output counts and duration; `build.py --check` compares against
`tools/build_library/expected_counts.json` and fails on drift (R2). Memory: stream line by line; keep only id tables;
the whole Latin run must stay under 3 GB RSS on the build machine (a streaming pass over the 1.2 GB file in plain Python
took on the order of a minute or two on this box; peak RSS was not recorded). Determinism: sort everything, no timestamps in outputs. The raw
download commands (resumable `curl -C -`) live in `tools/build_library/fetch.sh` with the URLs above.

### 4.4 JS test runner for ES5 code with only Node

`tools/jstest.sh` -> `node tools/jstest/run.js [pattern]` (Node >= 18; this box has v22.22.0). No npm packages.

1. **Loader:** for each `gui/ui/js/**/*.js`, read the file, run it in a fresh `vm` context whose globals are a small
   stub (`window`, `document` with `createElement`/`querySelector`/`addEventListener` recorders, `localStorage` memory,
   `navigator`, `setTimeout` fake clock). `Promise` is deleted from the context to prove the polyfill is loaded first.
2. **Contract checks per file:** exactly one new global, matching `^VP_[A-Za-z0-9]+$`; no other global leaks; no name
   collides with another file or with browser globals; the file is wrapped in an IIFE.
3. **ES5 check:** strip comments, strings, template-free regex literals with a small state-machine tokenizer, then ban:
   `=>`, backtick, `\blet\b`, `\bconst\b`, `\bclass\b`, `...`, `\basync\b`, `\bawait\b`, `for (... of`, `**`, shorthand methods
   and default parameters (regexes `function\s*\w*\(([^)]*=)`), `Promise.` outside the polyfill file, `.includes(`, `Object.assign`,
   `Array.from`, `String.prototype.padStart` family unless polyfilled. Also `node --check` for syntax.
4. **Unit tests:** `gui/ui/tests/*.test.js` written in ES5 against a 60-line harness (`describe`, `it`, `eq`, `deepEq`,
   `throws`) that loads the real module via the loader; DOM-heavy parts tested against the stub or, where a real
   browser is needed, `gui/ui/dev/*.py`-style smoke tests using Playwright (dev only, skipped when absent; this box has
   Playwright 1.56.1 under `/opt/node-tools` and Chromium at `/opt/pw-browsers/chromium-1194`).
5. **Leak tests:** helper `VP_TestHarness.countListeners()` wraps the stub `addEventListener/removeEventListener`;
   tests mount and unmount every screen 50 times and assert the count returns to baseline; cue-list test with 50,000
   cues asserts the DOM node count stays below a fixed bound (section 6 of PREDESIGN).
6. **i18n check:** port of the prototype's `test_i18n.py` (same keys in both files, same `{placeholders}`, every
   `VP_I18n.t("…")` literal and `data-i18n*` attribute resolves, dynamic key families listed in one place).
7. **Pack check:** `tools/pack_ui.py --check` equivalent confirming the UI bundle is current.

Deviation to settle in DESIGN: the prototype's `engine_bridge.js` uses native `Promise` and the global `window.bridge`;
the new rules forbid un-polyfilled Promise and require `VP_<Name>`. Plan: `VP_Bridge` plus `polyfill_promise.js` (about 70
lines, `window.Promise` only if missing) or a callback-style bridge. Also the prototype's `app.js` is a single 5,622-line
file; the new rule (separate files, one IIFE each) means roughly 25-35 files.

### 4.5 Engine protocol additions (for DESIGN.md, JSON lines as in the prototype section 9)

`engine.hello`, `engine.ping`, `engine.shutdown`, `settings.get/set`, `project.new/open/recover/save/saveAs`,
`subs.import {path}` -> cues (id, start, end, text, tags preserved), `translate.start {direction, pair, engines:{rules,model,online},
fidelity:"faithful|balanced|flexible"|tier, emoji, macrons, orbergise}` -> events `translate.progress`, `translate.cue`,
`translate.done`, `translate.cancel`, `cue.get {id}` -> alternatives, per-token analysis, reasons, confidence;
`cue.set {id,text}` (user edit); `word.inspect {form,lang}` -> analyses with lemma, features, gloss, tier, freq;
`glossary.get/set`, `corrections.list/apply`, `export.srt|vtt|ass {path, emoji, macrons}`, `lexicon.status`,
`model.status/locate/load/unload`, `online.test`, `history.undo/redo`. Error codes: `bad_params, not_found, io,
unsupported_format, lexicon_missing, model_missing, model_load_failed, online_disabled, online_failed, busy, internal`.

## 5. Risks and mitigations

| # | Risk | Likelihood / impact | Mitigation |
|---|---|---|---|
| R1 | **EN to LA lexical coverage:** pivot has 3.3k pairs; inversion of glosses is noisy ("fly" the insect vs the verb; "light" adjective) | high / high | Build reverse index with POS-aware, sense-aware scoring; seed 3k hand-verified T1/T2 mappings (D8 list doubles as EN to LA table); fidelity slider falls back to T1 words; add a "no confident Latin word" cue state (red) rather than guessing; coverage report per file (fraction of content words with score > threshold) |
| R2 | Kaikki schema drift (dump regenerated; new `tags`, renamed keys) | medium / high | Pin the dump SHA-256 and date in `fetch.sh`; build fails on canary drift; keep a golden 3,000-line sample fixture only for **structure** tests in `tests/fixtures` (our own re-typed minimal entries, not copied dumps); never auto-update raw files |
| R3 | Latin output longer than the cue allows (reading speed) | high / medium | Measure Latin/English char ratio on the regression set (**unverified**, expected 0.9-1.2 by words, longer by characters); line breaker (42 chars, 2 lines), CPS warning (adult 17 cps and children's 20 cps per Netflix guideline, **from memory, unverified**), allow cue merge/shorten via flexible tier and T1 shorter synonyms, never alter timing (CLAUDE.md), flag overflow as yellow |
| R4 | Songs, puns, wordplay, idioms, interjections ("Oh dear!") | high / medium | Songs detected by `♪`, repeated lines and metre-free heuristics -> yellow + "literal" translation; idiom table; interjections table ("heu!", "eheu!", "ecce"); never silently drop a cue; count them in the error report as their own stratum |
| R5 | Proper names (Alice, White Rabbit, Cheshire Cat) | high / medium | Glossary of names with per-name policy (keep, decline, translate); built-in table for common names with Latinised forms (Alicia, Leporem Album); user glossary persists in the project; capitalised unknown words -> keep + decline by heuristic (flag yellow) |
| R6 | 0.5B model too weak for Latin (measured, section 2.2) | high / medium | Rules own grammar; model optional, English-side first; go/no-go gate; report the model-alone numbers truthfully; closed-choice use only |
| R7 | Greek polytonic in subtitle players: many players/fonts lack Greek Extended; .srt encoding must be UTF-8 | high / medium | Export dialog option "monotonic Greek / unaccented fallback" and "ASCII-safe (Beta/transliteration)" copies; default UTF-8 with BOM choice; document in Export screen which players were tested (owner's player: unverified); warn once |
| R8 | Emoji in exported files: most players render with their own font or show tofu; ASS/libass needs a font with emoji | medium / low | Off in file by default (D9); preview in Export dialog shows warning; test in VLC/mpv (owner) |
| R9 | Macrons in exported files: combining marks/precomposed letters missing from the player font | medium / low | Default off in file (D13); option to export unmarked; app shows macrons always |
| R10 | Model licence (Gemma terms, Llama terms, LFM) | medium / medium | Prefer Apache-2.0 (Qwen2.5/Qwen3/SmolLM2); keep the licence text and "built with" notices in About; legal check before shipping any non-Apache model |
| R11 | Memory on 4 GB (D4: < 1.2 GB with model, < 250 MB without) | medium / high | mmap everything; model `use_mmap`, ctx <= 512, batch <= 64, loaded per job; measure RSS in tests after 10 and 1,000 cues; the Windows page cache counts against free RAM but is reclaimable; ASan builds for leaks; an i3/4 GB reference VM (owner) before release |
| R12 | Share-alike licensing of the lexicon and attributions | medium / high | Ship `.vpl` as a separate file with its CC BY-SA 4.0 notice (and DCC 3.0, Whitaker quote) in its header and in About; do not mix proprietary content into it; legal review before release (I cannot give legal advice) |
| R13 | Wiktionary/Latinitium online check: rate limits, ToS, changing markup | medium / low | Off by default; User-Agent with contact; 1 req/s; cache; treat result as a hint, never overwrite; Latinitium robots.txt only disallows `/legentibus-subscription-flow/`, but its terms for automated queries are **unverified** |
| R14 | Acceptance file not available yet (< 1 % cue error claim) | certain / high | Build all tooling and the held-out set first; measure on proxies; the claim is made only on the real file and reported with a confidence interval (section 6) |
| R15 | Alice acceptance file was tuned against: optimistic numbers | high / medium | Keep the held-out and fresh-file numbers separate, state both, and say which cues were touched by rule fixes |
| R16 | Hard-to-test Windows pieces (WebView2, file dialogs, mmap on Windows) | medium / medium | Keep OS code thin and behind RAII wrappers; portable logic unit-tested; MinGW compile check; owner smoke tests on a real Windows 10 and 11 box; no MinGW in this container |
| R17 | Spanish register (es-MX vs peninsular glosses) | medium / low | Hand-written tier glosses by a native reader; DCC Spanish list uses Spain-neutral Spanish, review its vosotros-free choices |
| R18 | JS Promise/ES5 rules conflict with the prototype bridge | low / low | See 4.4 |

## 6. Error-measurement protocol

### 6.1 Definitions (D14)

* **Cue** = one subtitle block with its own timing. Unit of measurement; the denominator is every cue whose source
  text contains at least one letter (sound-only cues such as "[music]" are reported separately, never silently
  excluded).
* A cue is **wrong** if it contains any one of: grammar fault (agreement, case, tense/mood, word order making it
  unreadable), meaning fault (changes or loses meaning, wrong sense), vocabulary fault (non-Latin/unknown word, wrong
  tier when strict, untranslated English left in), orthography fault (wrong macron when macrons are on, v/u, j), or
  markup fault (tag lost, timing/numbering changed).
* Metric per condition = wrong cues / counted cues, with the 95 % Clopper-Pearson interval. The report always
  prints numerator, denominator and interval; "< 1 %" is only claimed if the **upper** bound of the interval on the
  expert-reviewed sample is below 1 % **or** all cues were reviewed and fewer than 1 % are wrong.

### 6.2 Conditions (matrix)

Direction (EN to LA, ES to LA, later LA to EN/ES, GRC) x engine combination x fidelity tier:
`R` (rules), `M` (model alone, expected very bad, still reported), `R+M`, `R+O`, `R+M+O`, `O` (online alone, where it
can output anything) x `{T1, T2, T3}`. 7 combinations x 3 tiers = 21 cells for the acceptance file; the held-out
set is run on the same 21 cells. Same file, same settings, same bytes (determinism check run twice, hash compared).

### 6.3 Automatic checks (tools/eval, no human)

| ID | Check | How |
|---|---|---|
| A1 | Every Latin token is a known inflected form | analyse with the lexicon (macron-insensitive); unknown -> fail unless in the names glossary |
| A2 | Whitaker cross-check | stem+ending analysis independent of Kaikki tables; disagreement -> warning, not failure |
| A3 | Agreement | adj-noun in case/number/gender, subject-verb person/number, participle agreement; from the generator's per-token analysis **and** re-derived by re-analysing the output text |
| A4 | Case government | preposition + case table (e.g. in + abl./acc., ad + acc., cum + abl.), verb frames (dat. for dare), ablative of agent with ab |
| A5 | Markup/timing integrity | cue count, numbers, timestamps, tag positions identical to source; line count <= 2; max 42 chars/line |
| A6 | Tier compliance | every lemma tier <= selected tier unless flagged as name or loan |
| A7 | Source coverage | every source content lemma maps to something in the target (or an explicit drop reason); catches dropped words |
| A8 | Reading speed | CPS <= limit, duration >= minimum; warnings only |
| A9 | Round-trip sanity (LA to EN by our own engine) | back-translate; compare content lemmas to the source; low overlap -> yellow flag for the reviewer (not a failure) |

Checks A1, A3-A5 are failures that count as errors without human review; A6-A9 route cues to the expert.

### 6.4 Expert review

* Reviewer: the Latin teacher (owner) or a delegate, using the app's cue list with the red/yellow/green filter and an
  "error type" dropdown (grammar, meaning, vocabulary, orthography, markup, none). The tool exports `review_sheet.csv`
  and re-imports it.
* Sampling: (1) 100 % of cues flagged by any check or confidence yellow/red; (2) a **random sample of the remaining
  unflagged cues, n = 300**, seeded and reproducible (seed recorded). If 0 of 300 unflagged cues are wrong, the 95 %
  upper bound on the unflagged error rate is 1.0 % (rule of three: 3/300); if the file has about 1,000 cues and most
  are unflagged, that supports the "< 1 %" claim only when the flagged set is also reviewed and corrected as counted. If
  the owner can review all cues, do that instead (no interval needed).
* Second reader on a 10 % subsample for agreement (Cohen's kappa); disagreements adjudicated by the owner.
* Reviewer sees the source line and the Latin; never sees which engine produced it (blind) when comparing
  combinations; assignments are shuffled with a recorded seed.

### 6.5 Held-out set and rule-tuning hygiene (D7)

* `tests/heldout/` = our own sentences (>= 300) + a public-domain English text converted to SRT with synthetic timing.
  Avoid Carroll's *Alice* for the held-out set (the Disney film retells the same story, so vocabulary overlaps the
  acceptance file); use e.g. Aesop's fables (PD translations) and *The Wonderful Wizard of Oz* dialogue (PD), >= 600 cues.
* The held-out set is **frozen before rule work starts** (hash recorded in `tests/heldout/FROZEN.sha256`). Rules are
  tuned only on `tests/regression/` and the acceptance file. Any held-out cue ever inspected for debugging is moved to
  regression and the set is refilled; the report counts such "burned" cues.
* The owner's second subtitle file (D7) becomes the true held-out as soon as it arrives; it is never opened by an
  implementer.
* The acceptance-file result is always printed next to the held-out result, with the caveat that the former was used
  for tuning.

### 6.6 Google Translate comparison (D6; described, not run)

Latin only (Google Translate has no Ancient Greek: stated in D6). Tooling: Node + Playwright 1.56.1 (installed
globally at `/opt/node-tools`) driving the pre-installed Chromium (`/opt/pw-browsers/chromium-1194/chrome-linux/chrome`);
Python Playwright is **not** installed here. Steps:
1. Input: the same cue texts (markup stripped to plain text) as the engine saw; EN to LA direction `sl=en&tl=la`
   (`https://translate.google.com/?sl=en&tl=la&op=translate` returned HTTP 200 from this container).
2. Batching: the page limits input to about 5,000 characters; send groups of up to 25 cues separated by blank lines,
   read the output element text, split on blank lines; if the number of parts differs, fall back to one request per
   cue. Selectors for the source textarea and result span change over time (**unverified**; resolve by ARIA label/role and
   fail loudly).
3. Politeness: one browser context, realistic UA, 1.5-3 s jitter between requests, no parallelism, stop on CAPTCHA.
   Google's terms prohibit automated access; the owner approved the comparison (D6) for internal evaluation, so keep
   it small, never redistribute Google's output, and do not commit it (the repo rule on copyrighted text applies:
   only counts and our own example sentences are published).
4. Scoring: the **same** protocol as 6.4 on the same cues, blind: the expert sees A/B (engine vs Google, order random),
   marks each wrong or right with the same fault types; report wrong-cue rate per system on the same cue sample, a paired
   comparison (McNemar exact test on discordant pairs), plus the automatic checks A1/A3/A4 applied to Google's output
   (it makes no tier promises, so A6 is reported but not counted).
5. Timing/format: Google output is plain text; to keep cue structure, the comparison is by cue index.

### 6.7 What gets published

A table per cell of the matrix: cues, wrong, rate, 95 % interval, flagged/unflagged split, review coverage, burned-cue
count, determinism hash, run time and peak RSS. Plus the model-alone and online-alone rows even when they look bad.
No claim is made for any cell that was not measured.

## 7. Unverified list (so nobody mistakes it for fact)

Windows/MinGW/MSVC builds of the pinned llama.cpp; i3 performance of any model; legal reading of CC BY-SA 3.0/4.0 for the
compiled file; es-extract origin and licence statement; DCC Spanish list terms; Perseus per-file licence on the wiki;
Latinitium terms for automated use; Whitaker code letter meanings; Latin/English character-length ratio in subtitles;
Netflix reading-speed numbers; canonical-latinLit total size; Gemma/LFM licence consequences; non-Apache Q4 sizes of
SmolLM2; installer compression ratio; Google Translate selectors and rate behaviour.
