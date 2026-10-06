# Latin realiser notes (C1) — what `order_la.txt` rules are implemented, and open questions

Status per rule of `data/curated/order_la.txt`. "Done" = implemented and covered by a row of the realisation
table in `engine/tests/test_rules_la.cpp`; "partial" = implemented with a stated limitation; "C2" = needs the frame
builder / transfer (English side) and is out of C1's reach by design.

## Done
order.decl, order.copula, order.exist, order.inf (object before the infinitive, modal last; `nōn` between:
"Trānsīre nōn possum"), order.yn (-ne on the verb, or on the emphasised NP: `LaNP::emphasis`; the host moves to the
front), order.yn.nonne, order.yn.num, order.wh (wh word or interrogative NP first: "Quot frātrēs habēs?"),
order.imp / order.imp.long (see reading below), order.prohib (nōlī/nōlīte + inf; complements before the infinitive
when more than one), order.excl (quam + adj + noun; ō + accusative), order.voc (vocative first, after an
interjection, comma), order.conn / order.conn.first (second-position set read from the file), order.adj (set of
exceptions read from the file; adjective adverbs "nimis parva"), order.poss (after the noun; `possContrast` before),
order.gen, order.num, order.prep (ā/ab, ē/ex by the next sound; mēcum/tēcum/… from the file's list), order.adv
(time adverbs from the file go first, others before the verb; `AdvPos` overrides), order.rel (relative pronoun
agrees with the antecedent, case from `relRole`, preposition or -cum inside), order.sub.pre (before/after by the
source), order.sub.purp (ut/nē + subjunctive forced), order.acc.inf, order.dupl (caller repeats the clause),
pron.drop (a, b through `emphasis`, c for "Ego Alīcia sum"), pron.is (objects eum/eam/id), pron.refl (sē/suus),
neg.nullus, neg.nemo, neg.numquam (single negation), punct (source mark copied, default by type), caps,
enclitic.que (flexible mode only), name.policy (glossary, names_la.tsv keep/decline/translate, unknown kept +
flag), cop.there (`existential`), cop.it.is (`predGender` = neuter: "Hīc obscūrum est").

## Partial
* order.imp: "words <= 3" is read as *complements* (verb and vocative excluded) and a fronted adverb keeps the
  verb last: "Dā mihi colōrem rubrum" (3 complements) and "Prīmum nōmen tuum scrībe" (gold) both come out as in the
  gold file. Please confirm this reading or amend the rule text.
* order.name.first, q.tag, tense.*, mood.*, modal.*, have.poss, like.placet, need.opus, title.voc, num.digits,
  time.hour, time.day, length.fit, order.sub.quod: these map English structures onto the clause structure and belong
  to C2 (frame builder / transfer). The primitives they need exist: modal + infinitive, subjunctive moods, dative
  verbs (`placeō` with `io`), `nōlō`, numerals before nouns, impersonal frames in `CaseAssigner`.
* neg.neque: a coordinated negative clause uses `SubRel::Coord` with `conj = neque`; not in the table yet.
* Enclitic -ne on a non-verb host: implemented only for the emphasised NP ("Rosāsne vīdistī?"); choosing the focus is
  the frame builder's job.

## Lexicon findings that shape the output (for the teacher)
1. `narrō` is the headword (Wiktionary); its imperative cell is "narrā" with "nārrā" as an alternative. The realiser
   writes **"Sedē et narrā mihi fābulam."** where the gold has "nārrā". Which spelling should win?
2. `nōlō` has a cell "nōn vīs" for the 2nd person singular; a declarative "you do not want" comes out as two words
   (expected), and the token for "nōn" carries the lemma nōn.
3. Deponent perfects and perfect passives are periphrastic from the participle cell + `sum` ("secūta est", "amātus
   erat"); the participle agrees with the subject's gender. A subject-less passive defaults to masculine.
4. The library has no Whitaker-only analyses (ANAL bit2 is never set); A2 therefore uses `whit_freq != 0` or tier
   1-2 as "attested" and only warns. The checker treats A2 as a warning as the design says.
5. Proper-name lemmas exist for many common nouns (Dominus, Servius): the checker treats a token as a name only when
   every reading is a name or the word is unknown and capitalised.

## Open questions for the main agent
1. **Gender of first-person predicates** ("I am tired"): without a speaker glossary the realiser needs
   `LaNP::gender` on the pronoun subject; what should C2 default to — masculine, or Check with both forms as
   alternatives ("fessus | fessa")?
2. **Imperative number** without a vocative: default singular (gold: "Adiuvāte nōs" needs the plural from context).
   Should the frame builder look at "everyone", "you all", or the previous cue's vocatives?
3. **-ne placement** in yes/no questions with an object: the gold prefers the verb ("Vīdistīne fēlem meam?", "Lūdisne
   chartīs?") even when an object is present; the rule text says "else the focused word". The realiser follows the
   gold (verb unless an NP is marked emphatic). Confirm.
4. `cum` + ablative versus the conjunction: the checker treats mid-clause `cum` as the preposition and clause-initial
   `cum` as the conjunction only when the sentence has two finite verbs. Acceptable for beginner texts?
5. names_la.tsv: "translate" rows (Queen, King, Mr) are ordinary nouns; should the table carry them, or should the
   phrasebook ("Your Majesty") own them?
6. Emoji on "hortus" has two rows (🏡 and 🌷) and "pōns" three; the first wins. Which should stay?

## Curated rows added
None. Duplicates found (loader warnings): tiers_la.tsv `aqua` (twice), emoji_la.tsv `hortus` and `pons`.


## Decisions by the main agent (2026-10-06), binding for C1/C2
1. Speaker gender: `Options.speakerGender` ("m" | "f" | "unknown", project setting, default "m"). Unknown -> masculine
   form, feminine as alternative 1, confidence Check. (rules.h updated.)
2. Imperative number: singular by default; plural from an addressee NP (everyone, you all, boys, ladies and gentlemen),
   a plural vocative in this or the previous cue, or the previous cue addressing a group; a guess from the previous cue
   is Check. "let's" -> 1st plural subjunctive. (order_la.txt RULE imp.number.)
3. -ne host: the verb, verb first, unless the source marks an NP as contrastive focus. (order.yn amended.)
4. order.imp counts complements, not the verb; a fronted adverb keeps the verb last. (Amended.)
5. Vowel quantities: the reader tradition wins over the lexicon where they differ; `data/curated/macron_overrides.tsv`
   (key, stem_from, stem_to) is applied to display forms; first row narro -> nārr-. Apply in the Macrons primitive
   or in the engine's display step (C2).
6. names_la.tsv "translate" rows stay there (they are name/title policies); they are ordinary nouns for the checker.
7. cum: preposition when followed by an ablative NP, else conjunction when two finite verbs. (RULE cum.pos.)
8. A2 stays a warning; LIB-3 (B4b) may add Whitaker-only ANAL rows later.
