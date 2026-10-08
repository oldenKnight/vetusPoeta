# Regression set

Our own sentences in subtitle form (`own_dialogue.en.srt`, from `own_dialogue.en.txt`), used to tune rules and to catch
regressions. Expected Latin outputs will live in `expected/` once the engine produces them and the main agent has
reviewed them; a test fails when an output changes without an updated expectation.

`own_turns.en.srt` (C28, from `own_turns.en.txt`): 120 cues of our own subtitle-style dialogue for cue context: speaker
dashes, sentences split over two or three cues, lower-case continuations, noun-phrase and prepositional-phrase answers,
interjections, vocatives, yes / no answers and a few song lines. Gold: `expected/own_turns.la.gold.txt`.
