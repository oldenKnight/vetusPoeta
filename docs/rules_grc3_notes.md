# Greek quality loop 3 (C18) — generalisation guard, light verbs, purpose / result, participles, vocatives

## Blind check sentences (written at the start of C18, before any change; output not looked at until the end)
Children's-book dialogue, our own sentences, none from a sample or the regression file.
1. Mother, I made a mistake.
2. Let's take a walk in the garden.
3. The dog is barking because it is hungry.
4. We went to the river to catch fish.
5. Running home, the girl fell.
6. Oh little bird, where is your nest?
7. When the sun rose, the children woke up.
8. The old man was so tired that he slept all day.
9. Give me the apple, please.
10. My brother is afraid of the big horse.
11. Who ate my bread?
12. I will tell you a story tonight.
13. After dinner we played in the yard.
14. The farmer's daughter has three goats.
15. Don't make a noise, the baby is sleeping.
16. Can you see the stars in the sky?
17. We must hurry, or we will be late.
18. The teacher gave the boy a new book.
19. Why are you crying, my friend?
20. Sitting under the tree, the shepherd sang a song.

## Blind check result
First run (after the work items below, before any fix for these sentences): **9 / 20** acceptable Attic of the same
meaning (1, 2, 4, 9, 10, 11, 14, 16, 18). Faults: "because it is hungry" (λιμηρόν, neuter "it"), "Running home, the girl
fell" (read as an imperative + καί: an OK cue that was wrong), "Oh little bird" (not a vocative), "the sun rose" (ἵστημι)
and "woke up" (active ἐγείρω), "so tired ... all day" (οὕτω lost, πάσας τὰς ἡμέρας, ἀνὴρ παλαιός), "tonight" unknown and
"a story" lost by the parser, "yard" -> ναυπηγίον, a comma splice after a command joined with καί, "or we will be late"
(ἢ πολλαί), "crying" -> βοάω, "Sitting under the tree" (imperative + καί). After the fixes (each with two own sentences
in the unit case, none of them a blind sentence): **17 / 20**. Still wrong: #6 "nest" (greek.vpl has no νεοττιά: ἅλως),
#12 "a story" dropped by the frame builder ("obl=-:tonight of:story"), #20 κάθημαι has no usable present participle
cell (the table lists καθήμενος as a plural perfect), so the clause stays finite (ἐπεὶ ... κάθηται, wrong tense).
The first-run figure is the one that predicts held-out behaviour.

## Numbers (fidelity 2, speaker f, real data)
| set | before C18 | after C18 |
|---|---|---|
| EN -> GRC own_dialogue (114) | 114 / 114, ok 80 / check 34 / fix 0 | 114 / 114, ok 80 / check 34 / fix 0 (no output changed) |
| ES -> GRC lines 1-40 | 38 / 40 | **39 / 40** (#27 fixed; #24 stays) |
| GRC -> EN / GRC -> ES (40) | 40 / 40, 40 / 40 | 40 / 40, 40 / 40 |
| C16 constructions | 37 / 37 | 37 / 37 |
| C18 constructions (new) | - | EN 56 / 56, ES 12 / 12, + realia, light-verb flag, participle forms, noun memory |
| blind check (20) | - | first run 9 / 20, after 17 / 20 |
| Latin EN / ES / oz_sample / la2x / orberg | 114 / 100 / 71 / 201 / 60 | unchanged (no Latin file touched) |

#52, #107, #112 of quality loop 2 already match through the gold alternatives the main agent accepted; nothing changed
there. ES #24 "Son todos muy groseros." -> ...εἰσιν: the Spanish has no second person to read (ustedes / ellos); the gold
line is shared with the English source ("You're all"), so no alternative can be proposed. **No gold alternative is
proposed and the gold file is unchanged.**

## What changed
* **Light verbs and verb + object idioms** (lexical_en_grc.tsv kind `light`, source = verb lemma + object noun lemma;
  frame `mid` = middle voice, `obj:<prep>` = that PP's noun becomes the object): make a mistake -> ἁμαρτάνω (ἥμαρτον),
  take / go for a walk -> περιπατέω, have / take a rest -> ἀναπαύομαι, give a shout / cry -> βοάω, make / pay a visit
  to X -> X ἐπισκέπτομαι, take / have a bath -> λούομαι, have a swim -> νέω, take / have a look at -> σκοπέω, tell a
  lie -> ψεύδομαι, ask a question -> ἐρωτάω, give an answer -> ἀποκρίνομαι, make a promise -> ὑπισχνέομαι, have dinner
  -> δειπνέω; Spanish mirrors (cometer un error, dar un paseo, tomar un descanso, dar un grito, hacer una visita, tomar
  un baño, decir una mentira, hacer una pregunta, dar una respuesta, hacer una promesa). "make a noise" keeps θόρυβον
  ποιεῖν (Attic; greek.vpl has no θορυβέω). λάθος stays tier 3: "mistake" is ἁμαρτία / ἁμαρτάνω.
* **Doubts on the Greek path** (engine_grc): the frame builder's rule-of-thumb flags (light-verb, purpose-guess,
  noun-infinitive, contact-relative, participle-phrase, phrase-order, ellipsis) make a Greek cue Check as on the Latin
  path; a light verb handled by its `light` row and a noun + infinitive handled by valency purp:inf are not guesses.
  The regression's ok count did not move (80).
* **Noun sense consistency in a batch** (Greek side, `transfer::Memory::nounSense` as a data field only, no Latin code
  called): a noun keeps the Greek word chosen earlier in the cue / batch when it is a candidate; 32 entries, oldest
  dropped; forced alternatives bypass it.
* **Purpose and result**: a purpose clause without its own subject takes the main clause's person and number (ἦλθον ἵνα
  σε ἴδω; was ἴδῃ, an OK cue that was wrong); can / may inside a purpose clause is dropped (ἵνα ἡ γαλῆ εἰσέλθῃ); a
  to-infinitive after the object of a verb with valency `purp:inf` (δίδωμι, παρέχω) is the bare infinitive (δός μοι
  ὕδωρ πιεῖν; was ὃ πίνει); "so ADJ / ADV that" -> ὥστε + indicative (actual result, οὐ) or + infinitive when the result
  clause has can / could (μή); οὕτω before a consonant, οὕτως before a vowel (sandhi); two-word adverb groups ("so fast").
* **Circumstantial participles** (realise_grc GrcSub::participle; morph_grc `participle()`): an English -ing adjunct
  (present) and a when / after (aorist, past simple) or while / as (present) clause with the main clause's subject
  become a nominative participle agreeing with that subject, after the subject or first ("ὁ ποιμὴν τὸν λύκον ὁρῶν
  ἔφυγεν", "ἡ κόρη τὴν γαλῆν ἰδοῦσα ἐγέλασεν", "δεῖπνον φαγόντες ἐκαθεύδομεν"); a dependent NP subject with a pronoun in
  the main clause moves to the main clause; another subject, a negation, a modal, a copula, a future or an impersonal
  main verb keep the finite clause. Singular forms are table cells; plurals are derived (τρέχοντες, ἰδοῦσαι, λυόμενοι)
  and are Check (from-rule) when greek.vpl does not list them. Spanish gerunds the same way. The checker accepts a
  participle that agrees with a head of its clause or, with the subject dropped, a nominative of the verb's number.
* **Repairs of two frame-builder analyses on the Greek side** (see API changes): "When X, Y." parsed as a wh question
  with Y coordinated; "Singing a song, the girl walked ..." / "Viendo al lobo, ..." parsed as an imperative / a clause
  with the real clause as a time or coordinated clause; "Oh little bird, where ...?" parsed as a fragment.
* **Vocatives and particles**: no 1st-person possessive with a vocative (ὦ παῖ for "my child"); "where is your X?" ->
  X as the subject with its article (ποῦ ἐστιν ἡ μήτηρ σου); ", for ..." and a statement after a command without a
  conjunction -> γάρ second (ὀψὲ γάρ ἐστιν, ὁ γὰρ λύκος ἔρχεται); "or" after a command / must + future -> εἰ δὲ μή.
* **Other**: "it is late / early" -> ὀψέ / πρωΐ ἐστιν (state frame `adv`); a person "late" -> ὑστερέω; "it" for the
  animal of the main clause takes the state verb (ὅτι πεινῇ); the degree word of a state adjective stays with the verb
  (οὕτως ἔκαμνεν); durative states be tired / be hungry (ἔκαμνεν, ἐπείνη); καθεύδω in the past -> imperfect (no Attic
  prose aorist); ἥκω in a dependent past clause -> ἧκεν (not ἧξεν); a verb without aorist cells (ὑλακτέω) -> narrative
  imperfect; "all" + a singular noun -> πᾶσαν τὴν ἡμέραν, accusative of duration; "old man / woman" -> γέρων / γραῦς;
  phrasal frames mid / pass ("wake up" -> ἠγέρθησαν); kind `subject` (sun / moon rise -> ἀνατέλλω); a Spanish
  common-gender noun takes the source's feminine (la niña -> ἡ παῖς); the Spanish possessive dative with a body part
  (le mordió la mano -> τὴν χεῖρα αὐτοῦ); ES #27 row "que le corten {NP}" -> ἀποτέμετε {1:acc} αὐτοῦ and the grave rule
  across phrasebook pieces; προσκυνέω's table augment ἐπροσκύνησα written προσεκύνησα (and analysed back); realia
  pizza -> πλακοῦς.
* **Unknown English forms (C17's english.{h,cpp}) on the Greek path** (verified): irregular pasts reach it through the
  frame builder ("The girl swam in the river." -> ἔνευσεν, already before C18); hyphenated compounds and -ly adverbs did
  not ("sea-shore", "happily" stayed unknown, Fix) because that derivation sits in the Latin transfer's select. The Greek
  select now calls `frame::en::baseCandidates` (reused, not copied) when neither the reverse index nor a teacher gloss
  knows the word, and -ly adverbs go through the adjective's adverb cell: εἰς τὸν αἰγιαλόν, ἡδέως.
* **Realia policy (A9)**: checked: realia keep the hypernym, flag `realia`, Check; A9 runs and is reported (on "The tea
  is hot." it passes at 0.5 because "hot" round-trips; the realia flag is what makes the cue Check, as decided).

## Rows added
lexical_en_grc.tsv: light 30 (20 English, 10 Spanish), subject 3, phrasal 2 (wake up, get up), state 3 (late verb, late
adv, early adv), verbprep 1 (bark at), durative 2 (be tired, be hungry), realia 2 (pizza); tiers_grc.tsv: 25 rows + 2
notes (βασιλεύς "king", φιλέω "kiss"); valency_grc.tsv: δίδωμι purp:inf (changed), παρέχω (new); phrasebook_es_grc.tsv:
1 changed (que le corten), 1 new (que les corten).

## API changes (additive)
realise_grc.h: FrameKind::PurpInf (appended), GrcSub::participle / finite / noConj; GreekRealiser::Ctx::participle
(private). morph_grc.h: `participle()`. No change in frame/, transfer/, realise_la/, check/, cue/ or english.*.
**Wish for frame/ (C19 or later):** (1) a sentence-initial "When X, Y." that is not a question is built as a wh
question "when" with Y coordinated; (2) "-ing phrase, S V" is built as an imperative with the clause as a time or
coordinated clause (Spanish gerunds as a clause); (3) "Oh N, wh...?" leaves N as a fragment subject; (4) "Where is my
book?" (existential) loses "my"; (5) "I will tell you a story tonight" makes "story" a genitive of "tonight"; (6)
"Hurry, ..." reads "Hurry" as a name. The Greek path repairs (1)-(3) for itself; Latin has the same analyses.

## Lexicon gaps (for LIB)
No νεοττιά (nest), θορυβέω, ἀριστάω, ἀληθεύω; κάθημαι's participle cells are tagged plural perfect (καθήμενη without
the accent shift); προσκυνέω's aorist cells have the augment before the prefix (ἐπροσκύνησα); ὑλακτέω has no aorist;
πρῴ is only listed as πρωΐ; "morning" gives the poetic ἠώς (ἠόα); "happy" -> λευκός, "box" -> κυψέλη, "table" ->
ποτηροθήκη, "stick" -> ἀρχή, "game" -> θήραμα, "ring" -> κύκλος (and "gold" -> ἄχρυσος) in the reverse index (teacher
glosses needed; not added in this loop beyond the unit sentences).

## Review round 2 (main agent's spot check, 2026-10-07)
| # | source | before | after |
|---|---|---|---|
| 1 | The shepherd, seeing the wolf, fled. | Ὁ ποιμὴν τὸν λύκον ὁρᾷ ἐπεὶ ἔφυγεν. (OK, wrong) | Ὁ ποιμὴν τὸν λύκον ὁρῶν ἔφυγεν. (Check) |
| 2 | When the sun rose, we went to the river. | Πότε τὸ ῥόδον ἡλίου εἰς τὸν ποταμὸν ἀπήλθομεν. (OK, wrong) | Ἐπεὶ ὁ ἥλιος ἀνέτειλεν, εἰς τὸν ποταμὸν ἀπήλθομεν. (Check when rebuilt) |
| 3 | Where is your mother? | Ποῦ ἐστι μήτηρ; (Check) | Ποῦ ἐστιν ἡ μήτηρ σου; |
| 4 | Hurry, the ship is leaving! | Ὦ Hurry, ἡ ναῦς λείπει! (Check) | Ὦ Hurry, ἡ ναῦς ἀποπλεῖ! (Check: "Hurry" read as a name, frame builder) |

Fixes (Greek side only; they hold whether or not the frame builder changes):
* An -ing word without an auxiliary right after a comma is never the main verb: the clause the frame hung after it is
  the main clause with that subject, the -ing phrase a participle (#1; "The girl, hearing the bell, ran home.").
* "When X, Y." that is not a question (engine_grc `frontedWhen`): when the frame builder gives no time clause before Y,
  X and Y are analysed apart (X alone, so "the bell rang." / "the sun rose." are retagged as pasts by C17's helpers)
  and merged as time clause + main clause; flag clause-repair (Check). With C19's live frame builder the sentence is
  already parsed right and the cue may be OK; the HEAD frame builder exercises the rebuild. A wh frame in a sentence
  without "?" is flagged wh-statement (Check). The transfer's own repair (wh "when" + coordinated clause) is tested on
  a hand-built frame.
* A possessive determiner (my, your, his, its, our, their; not the ambiguous "her") just before the head noun that
  the frame did not attach is restored from its token (#3, "Where is my book?"); a verbless fragment "where + NP" in a
  question is rebuilt as the wh question with "be" (πῶς ἔχει for "how is"), Check.
* Intransitive "leave" -> ἀπέρχομαι (verb rows may have frame `intr`: used without object and PP); "ship / boat leave"
  -> ἀποπλέω (kind subject).
* Conservative confidence: clause-repair, wh-statement and participle-phrase (every participle made from a clause or an
  -ing phrase: the subordinator is dropped) are never OK.
Unit case "C18 review" 13 / 13 (the four sentences and two own ones per rule) and the transfer-level repair test.
