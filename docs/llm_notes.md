# Local model (engine ii): measurements and the Latin gate

Measured 2026-10-06 on the build container (Intel Xeon 2.1 GHz, 4 cores, AVX-512 present but the build targets
AVX2; other implementers' jobs ran at the same time, load average about 4.5, so every time below is pessimistic for
this box and says nothing about the target i3). Model file: see `models/README.md` (Q4_K_M, 397,808,192 bytes,
SHA-256 verified). Engine settings: mmap, n_ctx 512, n_batch 64, 4 threads, greedy, prompt version 1.

## Gate: Latin minimal pairs (PREPLAN 2.2) — NOT PASSED
Rule fixed before the run: pairwise accuracy on the summed log-probability of the clause (correct clause strictly
higher, ties count as wrong); ship Latin-side reranking only if accuracy >= 75 % and the lower bound of the 95 %
Wilson interval is above 70 %. One run, 1,000 pairs, seed 1 (`python3 tools/eval/llm_gate.py --pairs 1000`;
numbers in `tools/eval/llm_gate_report.json`).

| Measure | Correct / pairs | Accuracy | 95 % Wilson |
|---|---|---|---|
| **Summed log-probability (primary)** | **749 / 1,000** | **74.9 %** | **72.1 – 77.5 %** |
| Mean log-probability per token (secondary) | 581 / 1,000 | 58.1 % | 55.0 – 61.1 % |
| Summed, pairs with equal token counts | 460 / 624 | 73.7 % | 70.1 – 77.0 % |
| Summed, corrupted clause has more tokens | 251 / 305 | 82.3 % | 77.6 – 86.2 % |
| Summed, corrupted clause has fewer tokens | 38 / 71 | 53.5 % | 42.0 – 64.6 % |

By template (95 % Wilson): `N A est` 168/214 = 78.5 % (72.5 – 83.5), `S O V` 242/289 = 83.7 % (79.0 – 87.5),
`Npl Vpl` 89/142 = 62.7 % (54.5 – 70.2), `in N V` 250/355 = 70.4 % (65.5 – 74.9). By corruption: case 445/600 =
74.2 % (70.5 – 77.5), person 214/271 = 79.0 % (73.7 – 83.4), number 55/78 = 70.5 % (59.6 – 79.5), gender 35/51 =
68.6 % (55.0 – 79.7). Load 336 ms; 262 ms per pair (two clauses of about 7 tokens after a 3-token prefix).

**Decision:** 74.9 % is below the 75 % threshold (the lower bound, 72.1 %, is above 70 %, but both conditions are
required). Latin-side reranking is off: `prompts::kLatinRerankGatePassed = false`, `rerankEnabled()` is false,
`Advisors.rerankLatin` is empty and `model.status` reports `rerankEnabled:false`. The model is offered for English
(and Spanish) source understanding only: closed sense choice and simplification. The result sits on the threshold;
re-running with other seeds or templates until it passes would be fishing, so this run stands. The length analysis
above also shows that part of the 74.9 % comes from corrupted forms that are simply longer (more tokens = lower
summed probability); with equal token counts the model is at 73.7 %, and per token it is at 58 %.

How the pairs are built (engine/llm/src/gate.cpp): tier-1/2 lemmas with tables (nouns of fixed gender, adjectives
with all three nominative singulars, non-deponent verbs with the whole present indicative active; for `S O V` verbs
with a transitive sense), four templates, and one word replaced by another cell of the same lemma. A corrupted
clause is kept only if a generous clause checker finds no grammatical reading of it with any analysis of any word
in the lexicon (predicate nominative/genitive/dative/ablative after `est`, pronoun subjects, adjunct datives and
ablatives, `in` + accusative); vocative readings are not accepted because the clauses carry no commas. Forms are
scored without macrons (the model has seen Latin without them). Caveats: the checker is coarse, semantics are
random ("Elephantus folium dicit."), and tier 2 contains rare nouns; none of this favours either member of a pair.

## Engine numbers (engine/tests/test_llm.cpp, ctest vp_llm_protocol)
- Load: 351 ms (warm page cache; a cold first load from disk was not measured). SHA-256 of the file: 2.0 s, done
  once per file by `model.status` / `model.locate` and cached; `engine.hello` never hashes.
- Memory (RssAnon, Linux): 10.8 MB before load, 173.7 MB after load and 23 calls (the CPU backend repacks the Q4_K
  weights into anonymous memory; VmRSS 567 MB including the file-backed mapping), 173.7 MB after 50 more `choose`
  calls (growth 0 %), 3.8 MB after unload (llama frees everything; `malloc_trim` returns it to the OS).
- `model.test` over the protocol: 818 ms for load + one 3-option `choose` + unload; it picks "puella" for "girl"
  (mean log-probabilities -0.13, -6.82, -3.25).
- English sense questions (our 10 closed items in test_llm.cpp): 9/10 right (95 % Wilson 59.6 – 98.2 %), identical answers and scores on a
  repeated call. Anecdotal (n = 10), recorded, not asserted.
- `simplify` examples: "Notwithstanding the inclement weather, the expedition proceeded northward." -> "Despite
  the inclement weather, the expedition continued northward."; Spanish "A pesar del mal tiempo, la expedición siguió
  hacia el norte." -> "Despite the bad weather, the expedition continued north."

## Unverified
Running on Windows (MinGW build links, not executed), speed and load time on an AVX2 i3 with 2 threads (`eco`),
cold-cache load time, MSVC build, behaviour on a CPU without AVX2 (only the CPUID branch is unit-tested on a CPU that
has it), the hand-off of `Advisors.chooseSense` into engine i (waits for C2's hook).
