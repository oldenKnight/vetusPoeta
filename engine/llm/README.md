# engine/llm — engine ii, the optional local model

Wraps the vendored llama.cpp (`third_party/llama`, target `vp_llama`, CPU only) for closed questions only
(DESIGN §1.1, §11). Public API: `include/vp/llm.h` (`vp::llm::Model`, `Config`, `Status`, `Advisors`, CPUID gate,
file check) and `include/vp/llm_gate.h` (the Latin minimal-pair gate). Prompts: `prompts.h` (versioned).
Model file and hash: `models/README.md`. Measurements: `docs/llm_notes.md`.

Built only with `-DVP_WITH_LLM=ON` (static `vp_llm`, defines `VP_LLM_REAL=1`). With OFF, engine/cli defines an
INTERFACE `vp_llm` carrying only the include directory and `vp/llm.h` compiles to a header-only stub whose calls
answer `model_missing`.

## Behaviour
- `load`: CPUID first (AVX2 + FMA + F16C + BMI2 and OS AVX state; `model_unsupported_cpu`), then the GGUF header
  (`model_missing` / `model_load_failed`), then llama.cpp with `load_mode = MMAP` (this llama.cpp version replaced
  `use_mmap`/`use_mlock` by `load_mode`; MMAP = mmap without mlock), no GPU layers, `n_ctx` 512, `n_batch` =
  `n_ubatch` = 64, `n_threads` = min(4, hardware), one sequence. `llama_backend_init` runs once, after the CPUID
  gate; `llama_backend_free` at exit. llama.cpp's log goes to `setLogSink` (warnings and errors only).
- `choose`: system line + user turn (question + options) through the model's chat template; the prompt is decoded
  once, its last logits kept; each option (+ end-of-turn token) is teacher-forced after it and then removed from
  the KV cache with `llama_memory_seq_rm` (no state snapshot: cost = prompt once + every option's tokens once).
  Score = mean log-probability per token; ties keep the lowest index. Greedy, no sampling: deterministic for a
  file and a thread count (the "seed 1" of DESIGN §11 has nothing to seed).
- `scoreText(prefix, continuation)`: raw text, summed log-probability of the continuation's tokens.
- `simplify(sentence, "en"|"es")`: greedy, at most 48 tokens, grammar sampler built from the GBNF in `prompts.h`
  (printable ASCII, no newline), trimmed. Refuses with `model_load_failed` when not loaded.
- Idle: `touch()` on every call, `maybeUnload(nowMs)` from the CLI's 1 s tick after 60 s; the CLI also unloads at
  the end of every job and after `model.test`. `unload` frees context, model and buffers and calls `malloc_trim`.
- `rerankEnabled()` is the gate result recorded in `prompts.h` (`kLatinRerankGatePassed`).

## CLI wiring (engine/cli)
`model.status` (hashes the file once, cached), `model.locate`, `model.unload`, `model.test`, `engine.hello`'s
`model` field, idle unload in `Server::tick`, `translate.start` with `engines.model` builds `Advisors` (lazy load,
unloaded after the job; hand-off to engine i waits for C2's hook), `vpengine llm-gate`.

## Tests
- `vp_tests -tc='llm*'` (engine/tests/test_llm.cpp): without a model the error paths, CPUID, file checks, Wilson,
  gate pairs; with a model (`VP_MODEL_GGUF` or `models/*.gguf`; sanitizer builds only with `VP_MODEL_GGUF`)
  load time, choose determinism, simplify, RSS, idle unload. `VP_LLM_SKIP_MODEL=1` skips the model part.
- ctest `vp_llm_protocol` (`test_model_protocol.py`): the model.* commands against `vpengine serve`.
- Gate: `python3 tools/eval/llm_gate.py --pairs 1000` (writes `tools/eval/llm_gate_report.json`).
