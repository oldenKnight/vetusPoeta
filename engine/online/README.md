# engine/online (`vp_online`) — engine iii, the optional online check

Asks en.wiktionary.org whether a lemma's English gloss matches Wiktionary's definitions and returns evidence
(`Agrees` / `Disagrees` / `Unknown` / `Error`). Evidence only: it never changes text, it can only lower a cue to
Check and add a "why this word" reason. The only network code in the engine (DESIGN §1.3, §12).
Public API: `include/vp/online.h`; test doubles: `include/vp/online_mock.h` (`MockTransport`, `FakeClock`).
What is sent and stored, robots.txt and User-Agent notes: `docs/online_notes.md`.

## Guard (offline means offline)
- `onlineAllowed(settings)` = `engines.online && online.wiktionary` (both default off), read at every call.
- `makeSystemTransport(settings)` and `Wiktionary::create(...)` return `online_disabled` otherwise.
- `checkLemma()` / `probe()` re-check the settings first and answer `Error "online disabled"` without touching
  the transport (test: 100 calls -> 0 transport calls).

## Client (`src/wiktionary.cpp`)
- GET `https://en.wiktionary.org/api/rest_v1/page/definition/{title}`; title = lemma in NFC with macrons and
  breves removed (Greek accents kept), percent-encoded, spaces as `_`. Headers: `User-Agent: vetus-poeta/<version>
  (offline Latin and Greek tutor; https://github.com/oldenKnight/vetusPoeta)`, `Accept: application/json`. 8 s.
- Throttle: one token bucket (1 request/s) shared by every client in the process (`sharedThrottle()`); cache hits
  bypass it. The client sleeps for the throttle and for Retry-After: call it from the worker thread only.
- 429/503: Retry-After (seconds) <= 60 is honoured once; otherwise, or if the retry is limited again, `Unknown
  "rate limited"` and no request for max(60 s, Retry-After) (capped at 1 h).
- Network error -> `Error` with the message; other HTTP status or malformed JSON -> `Error`; 404 -> `Unknown
  "no Wiktionary entry"`.
- Cache: `<data>/online-cache/<sha256(url)>.json` = `{"fetchedAt":<unix ms>,"status","url","body"}` (fetchedAt
  first), 200 and 404 answers, valid 30 days; folder capped at 20 MB (oldest fetch first, down to 90 %);
  damaged files are removed; foreign files are left alone.
- Verdict (`src/parse.cpp`): definitions from the section keyed `la` (Latin) and from `other` entries whose
  `language` is "Latin" / "Ancient Greek" (Greek is there, checked live); HTML tags and entities removed; gloss
  keywords = lower-case content words with a light plural stem. Any keyword in any definition -> Agrees; section
  present without overlap -> Disagrees (first definition in the summary); none -> Unknown. Summary <= 160 code points.
- `stats()` {requests, cacheHits, throttled, errors}; `makeOnlineCheck(client)` is the rules-engine hook.

## Transports
- Windows (shipped): `src/transport_winhttp.cpp`, WinHTTP (system DLL, `winhttp` linked), system proxy settings.
- Elsewhere: `src/transport_posix_dev.cpp`, DEV ONLY, runs `curl` via posix_spawnp (no socket code linked); the
  Linux engine exists for tests and is never shipped. `makeSystemTransport` fails "dev transport: curl not found".

## CLI
`online.test` (engine/cli/src/server_export.cpp) -> `{ok, latencyMs, message, verdict, summary, url}` after
fetching "aqua" without reading the cache; `online_disabled` / `online_failed` otherwise. The translate job keeps
`engines.online` only when allowed; handing `makeOnlineCheck` to the rules engine waits for C2's
`engine_config.h` (TODO in server_cues.cpp).

## Tests
`vp_tests -tc='online*'` (engine/tests/test_online.cpp, fixtures tests/fixtures/online/, no network).
Live (one request each for aqua and λόγος): `VP_ONLINE_LIVE=1 vp_tests -tc='online: live*'`.
ctest `vp_online_symbols` (`tools/check_symbols.py <build>`): no other engine library references socket,
resolver, WinHTTP/WinINet, libcurl or process-spawn symbols; also works on the MinGW build tree.
