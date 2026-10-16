# models/ — the optional local model (engine ii)

`*.gguf` files are never committed (`.gitignore`). The engine never downloads anything: the installer offers the
file, or the user places it here (or anywhere, then picks it in Settings, which sends `model.locate`).

## The expected file
| Field | Value |
|---|---|
| Model | Qwen2.5-0.5B-Instruct, 4-bit `Q4_K_M` GGUF (494 M parameters) |
| File name | `Qwen2.5-0.5B-Instruct-Q4_K_M.gguf` |
| Source | `https://huggingface.co/bartowski/Qwen2.5-0.5B-Instruct-GGUF/resolve/main/Qwen2.5-0.5B-Instruct-Q4_K_M.gguf` |
| Size | 397,808,192 bytes |
| SHA-256 | `6eb923e7d26e9cea28811e1a8e852009b21242fb157b26149d3b188f3a8c8653` |
| Licence | Apache-2.0 (upstream model); quantised file redistributed under the same licence |

Verified 2026-10-06: the local copy hashes to the value above, and a fresh HEAD request on the source URL
reported `x-linked-size: 397808192` and `x-linked-etag` equal to the same SHA-256.

The size and hash are also compiled into the engine (`kModelSizeBytes`, `kModelSha256` in
`engine/llm/include/vp/llm.h`); a unit test keeps this file and the code equal. `model.status` reports
`sha256ok:false` (and `available:false`) for any other file.

## Why this quantisation
The official `Qwen/Qwen2.5-0.5B-Instruct-GGUF` Q4_K_M file is 491,400,032 bytes: it fits the 500 MB budget of
decision D4 by only 8.6 MB. The bartowski build of the same weights and quantisation type is 397,808,192 bytes and
leaves about 100 MB of headroom, so it is the one we ship and measure (PREPLAN 2.1 benchmarked the same file).

## Where the engine looks
1. `settings.modelPath` (set by `model.locate`);
2. the environment variable `VP_MODEL_GGUF` (development and tests);
3. the first `*.gguf` by name in `<vpengine folder>/models/`, then in `<data folder>/models/`.
