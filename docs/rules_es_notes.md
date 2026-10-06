# Rule engine, Spanish source (C13) — notes

Scope: Spanish -> Latin through the engine i pipeline of C2/C2b. Code: `engine/rules/src/frame/spanish.{h,cpp}` (new),
Spanish branches in `frame_builder.cpp` / `phrasebook.cpp`, the `es:` path of `transfer/transfer.cpp`, Spanish rows of
`transfer/tables.cpp`; small C1 additions (checker partitive genitive, `LaNP::coordConj/coordBoth`); one engine hook
(`Settings::srcLex`, ustedes from a "we" answer). Data: `phrasebook_es_la.tsv` (187 rows), `contractions_es.tsv`,
`clitics_es.tsv`, `states_es_la.tsv`, `phrasal_es_la.tsv`, `verbprep_es_la.tsv` (Spanish rows moved out of the English
files), `gloss_es_la.tsv` +50 rows / 4 extended / 2 corrected. Tests: `engine/tests/test_rules_es.cpp` (8 cases),
fixtures `tests/fixtures/rules_es/frames_es.tsv` (40 own sentences). Formats: `data/curated/README.md`.

## Measured (fidelity 2, speaker f, data/work of 2026-10-06 after B4b)
- Regression own_dialogue.es.srt: **97 / 100** normalised matches (exact incl. macrons 97); start of the task 15 / 100.
  Confidence ok 78 / check 22 / fix 0. Check because: analysis fallback (retag) 9, A8 reading speed 6, margin < 0.15
  4, A9 1, addressee-guess 1. Report: `<build>/regression_report_es.txt` (with the frames of every mismatch).
- English regression unchanged: 114 / 114 (also checked from the Spanish test, which fails below 110).
- Frame builder 40 / 40 own sentences; 36 / 36 one-sentence constructions; determinism (two fresh engines
  byte-identical); RssAnon flat over 1,000 Spanish cues (4,176 kB -> 4,176 kB).
- Caveat (D14): the regression file was used for tuning; the held-out number will be lower. tests/heldout/ not opened.

## What the Spanish branch does
- Tokens: `contractions_es.tsv` (del, al, pa', porfa ...); enclitics split off imperatives, infinitives and gerunds
  ("dámelo" -> da me lo, "cántanos" -> canta nos, "inclínense" -> inclinen se), known words only when the lexicon reads
  them as pronominal verbs ("siéntate" <- sentarse) or, sentence-initially, one-syllable imperatives ("Dame", "Dime");
  the stress mark moves with the clitic ("pása|me" -> pasa me); nouns ("vela") and one-word phrase rows stay whole.
- Tags from spanish.vpl (`es::retag`, then a new parse): 2nd-person verbs the tagger read as nouns ("vas", "eres",
  "tiemblas", "Tienes"), 1st person "camino" when no other verb, participles after estar/ser ("roto"), adjectives after a
  noun ("profundo"), noun-only "adjectives" ("agricultor"), tense/mood/person the tagger left out ("escribiremos",
  "entremos"), the indicative after a noun subject ("El gatito duerme"). Lemmas: adjective lemmas over verbs
  ("extraño"), ver's participle "visto", non-pronominal lemmas, feminine person nouns keep their head (hermana -> soror),
  a short list of homograph lemmas chosen last (dolar/doler, crear/creer, vetar/vete).
- Tree (`es::normalise`): clitics -> object / indirect object (me te nos os: indirect when the verb has another object
  or is a giving/telling/gustar verb) or the particle `se` of a pronominal verb (phrasal_es_la.tsv) or passive /
  impersonal "se"; personal "a" before a person object; "por qué", "a dónde"; "detrás de", "cerca de" ...; "a veces",
  "otra vez", "de nuevo" ...; "un poco de X" -> paulum + genitive; "antes de que" / "después de que" as subordinators;
  a punctuation root ("¡Que venga ...!").
- Clause: usted/ustedes 2nd person; pro-drop subject from the finite verb; "ir a + inf" future ("vamos a + inf" at the
  start of a statement: let us); perfecto compuesto; estar + gerundio progressive; estar + participle -> resultant
  passive ("frāctum est") unless states_es_la.tsv lists the adjective; hay/había existential (negated: nūllus); clock
  time "son las seis" -> hōra sexta est; gustar/doler type verbs (dative + subject); imperative from the mood or the
  2nd-person imperative reading of a clause-initial present ("Bebe esto"), prohibition "no + subjunctive", ustedes
  imperative ("¡Todos, inclínense!" -> Omnēs, inclīnāte vōs!), hortative 1st plural subjunctive ("Corramos"), jussive
  "que + subjunctive"; deliberative wh question in the future ("¿Qué voy a hacer?" -> quid faciam); a noun "subject" of a
  1st/2nd-person verb is its object (time nouns: an oblique); post-verbal subject of intransitive verbs; no question
  from Spanish verb-subject order; "Esta sí." / "Este sí puede." as ellipsis with the Latin gender of the noun meant;
  diminutives -ito/-ita -> noun + parvus; "ni ... ni" -> neque ... neque; "una" as a numeral next to another numeral;
  "todas las mañanas" -> omnī māne; capitalised common nouns as titles ("la Reina de Corazones" -> Rēgīna Cordium);
  vocatives only when a comma sets them off; a greeting before a clause of its own is followed by ";".
- Transfer: `es:` + es_key(lemma) from REVX, plus gloss_es_la.tsv read backwards as the teacher's Spanish glosses
  (same bonus as the English tier notes); tier logic identical to English; the Spanish noun gender bonus only for persons
  and animals (gato -> fēlēs f, puerta -> iānua whatever the Spanish gender); él/ella/lo/la of things take the Latin
  gender of the last noun; English pivot through the Spanish lexicon entry's English gloss when nothing Spanish names the
  word (spanish.vpl carries no English glosses yet, so today such a word stays unknown in brackets: Fix, never a guess).

## Remaining mismatches
| # | source | gold | ours | why |
|---|---|---|---|---|
| 24 | Son todos muy groseros. | Omnēs valdē inurbānī estis. | Omnēs valdē inurbānī sunt. | 3rd plural without subject: "ellos" and "ustedes" read the same; nothing in the text says the speaker addresses them |
| 57 | Plantamos rosas blancas por error. | Rosās albās errōre sēvimus. | Rosās albās errōre serimus. | "plantamos" is present and preterite alike; the tagger and the lexicon cannot choose |
| 99 | Buenas noches, duerme bien. | Bonam noctem; bene dormī. | Bonam noctem; dormī bene. | the realiser puts a manner adverb after an imperative (same as the English "Sleep well" -> dormī bene, accepted there) |

## Proposed gold alternatives (for the main agent; the gold file is unchanged)
- #24 add "Omnēs valdē inurbānī sunt." — without "ustedes" the 3rd plural is the plain reading; the English source had
  "you", the Spanish one does not.
- #57 add "Rosās albās errōre serimus." — or keep the gold and accept this as a known limit; a past reading needs
  context ("ayer", "ya") the sentence does not give.
- #99 add "Bonam noctem; dormī bene." — the English gold accepts "dormī bene" for #113.
- #58 (matches now): Spanish imperfect "quería" is rendered as the English simple past (voluit); "volēbat" is the more
  literal rendering of the imperfecto and could be added.

## Gaps by construction
- Spanish preterite vs present in -amos/-imos 1st plural (#57); conditional (podría) only through the tagger's mood.
- "se" with a non-pronominal verb and an object ("se lava las manos") is read as an indirect reflexive object.
- Language names as objects ("se habla latín"): no lexicon choice (Latīnē only after "en").
- es-MX "ahorita", "ándale", "órale", "ni modo" are phrasebook rows; other es-MX discourse words are not.
- The English pivot is wired but idle: spanish.vpl has no gloss_en (LIB could add it from translations_es.tsv).
- Diminutives: -ito/-ita/-cito/-ecito only, with an exception list (señorita, mosquito, bonito, ahorita ...).

## Questions for the main agent
1. Should LIB pack an English gloss into spanish.vpl lemmas (from data/work/en/translations_es.tsv) so the pivot works
   for words without es: candidates?
2. gloss_es_la.tsv rows changed: capio "tomar" removed (tomar -> sūmō, as in the gold), charta "carta" -> "naipe"
   (carta -> epistula). C11 reads these glosses Latin -> Spanish; please confirm.
3. "vamos a + infinitive" at the start of a statement is read as "let us" (es-MX usage); "Vamos a ver" stays future?
