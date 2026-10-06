# Online check (engine iii) — what goes over the wire, what is stored, terms

Owner: ONLINE (task C7). Code: `engine/online/` (README there). Decision D3 / DESIGN §1.3, §12: off by default.

## When anything is sent
Only when both settings are on: `engines.online` and `online.wiktionary` (defaults: off). The engine re-reads them
on every call; with either off, no transport object is even created (`makeSystemTransport` and
`Wiktionary::create` return `online_disabled`), and `checkLemma` answers "online disabled" without a request
(tested: 100 calls, 0 transport calls). Requests come from two places only: the `online.test` command (button in
Settings) and, once the rules engine takes the hook (`Advisors.onlineCheck`, C2), a translate job started with
`engines.online`. Latinitium is not queried in v1 (its terms for automated queries are unverified; the
`online.latinitium` switch stays hidden and has no code behind it).

## What is sent
One HTTPS GET per lemma not in the cache:
`https://en.wiktionary.org/api/rest_v1/page/definition/<title>` where `<title>` is the dictionary form of the
Latin or Greek lemma (macrons and breves removed, Greek accents kept). Nothing else from the user's file: no cue
text, no English/Spanish source, no file names, no settings, no cookies (WinHTTP and the dev curl transport keep
none). Headers we add: `User-Agent: vetus-poeta/<version> (offline Latin and Greek tutor;
https://github.com/oldenKnight/vetusPoeta)` and `Accept: application/json`. The operating system adds the
usual TLS/HTTP details (and any proxy configured in Windows); the server sees the user's IP address.
Rate: at most 1 request per second for the whole process; on 429/503 the client waits a Retry-After of at most
60 s once, otherwise it sends nothing for 60 s (or the longer Retry-After, capped at 1 h).

## What is stored
`<data>/online-cache/<sha256 of the URL>.json` (`%LOCALAPPDATA%\vetus-poeta\online-cache` on Windows): the
response body with its status, URL and fetch time, for 200 and 404 answers only. Entries are used for 30 days,
then fetched again; the folder is capped at 20 MB (oldest fetch removed first). Deleting the folder is always
safe. Nothing is uploaded, nothing else is written.

## Terms checked (2026-10-06)
- **robots.txt** (fetched https://en.wiktionary.org/robots.txt, 394 lines): for `User-agent: *` it has
  `Disallow: /api/` and `Disallow: /w/` with explicit `Allow` exceptions only for `/w/api.php?action=mobileview&`,
  `/w/load.php?`, `/api/rest_v1/?doc` and the sitemap; one crawler group has `Crawl-delay: 5`. The header says
  "Friendly, low-speed bots are welcome viewing article pages, but not dynamically-generated pages". robots.txt
  addresses crawlers; our client is user-initiated, one lemma at a time, cached for 30 days and throttled to
  1 request/s, which is the API-client situation the Wikimedia API etiquette (User-Agent policy, rate limits)
  covers rather than crawling. **Owner decision needed:** if the owner reads `/api/` as off-limits for this use
  too, the alternative is the article page `/wiki/<title>` (allowed for low-speed bots) parsed as HTML, which is
  heavier and more fragile; v1 keeps the REST endpoint the design names.
- **User-Agent policy** (fetched https://foundation.wikimedia.org/wiki/Policy:Wikimedia_Foundation_User-Agent_Policy):
  every request must carry an informative User-Agent with contact information (an e-mail address, a website or
  a wiki user), in the form `name/version (contact) library/version`; generic agents such as "curl" or
  "python-requests" may be blocked; a browser's agent must not be copied; automated agents are asked to include
  "bot"; requests without a descriptive agent may get HTTP 403. Ours names the program and version and gives the
  project website as contact. Open points for the owner: add an e-mail address, and whether a user-triggered
  dictionary lookup should carry "bot" (we left it out: it is not an autonomous crawler).
- **Rate limits:** the 429 body points to https://www.mediawiki.org/wiki/Wikimedia_APIs/Rate_limits and
  bot-traffic@wikimedia.org. From this build box (a shared egress address) the endpoint answered 429 with
  Retry-After 19-45 s on several first requests; the client's Retry-After handling then succeeded (see C7 notes).
- **Content licence:** Wiktionary text is CC BY-SA 4.0; we only cache it locally for comparison and show at most a
  160-character excerpt with a link to the page, the same attribution the About screen gives for the library.

## Format facts learnt live (2026-10-06)
The definition endpoint keys some languages by code (`la` for Latin) and puts others under `other` with their
English name in `language`: Ancient Greek (λόγος) came back as `other[].language == "Ancient Greek"`, not `grc`.
The parser reads both places. Definitions are HTML (Parsoid spans, links, `<link rel="mw:PageProp/...">`).

## Development transport
On Linux/macOS the engine has a DEV-ONLY transport that runs the `curl` program (posix_spawnp; no socket code
in the binary). It exists so the client can be tried on the build box and is never shipped; the Windows build
uses WinHTTP. ctest `vp_online_symbols` checks that no other engine library references network or process
symbols.
