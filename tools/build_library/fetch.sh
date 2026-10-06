#!/usr/bin/env bash
# Download the raw dictionary dumps into data/raw (gitignored) and record their SHA-256 and dump dates.
#
#   tools/build_library/fetch.sh [--raw DIR] [fetch|check|record|dates]
#     fetch   (default) resumable download of the four files, then `dates` and `record`
#     check   verify DIR/SHA256SUMS (sha256sum -c); exit 1 on any mismatch or missing file
#     record  write DIR/SHA256SUMS from the files present
#     dates   HEAD requests: write DIR/SOURCES.json with url, size and the server's last-modified date
#             (that is the kaikki.org dump date; build.py copies it into report.json)
# Network is used only by fetch and dates. Downloads resume with curl -C -; a file whose size already equals the
# server's content-length is not requested again.
set -euo pipefail

RAW="data/raw"
MODE="fetch"
while [ $# -gt 0 ]; do
  case "$1" in
    --raw) RAW="$2"; shift 2 ;;
    fetch|check|record|dates) MODE="$1"; shift ;;
    -h|--help) sed -n '2,12p' "$0"; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

# name|url
FILES=(
  "kaikki-Latin.jsonl|https://kaikki.org/dictionary/Latin/kaikki.org-dictionary-Latin.jsonl"
  "kaikki-AncientGreek.jsonl|https://kaikki.org/dictionary/Ancient%20Greek/kaikki.org-dictionary-AncientGreek.jsonl"
  "kaikki-English.jsonl|https://kaikki.org/dictionary/English/kaikki.org-dictionary-English.jsonl"
  "es-extract.jsonl.gz|https://kaikki.org/dictionary/downloads/es/es-extract.jsonl.gz"
)
UA="vetus-poeta-library-build/1 (offline dictionary build; resumable fetch)"

header() {  # url field -> value of the response header (last response after redirects)
  curl -sSIL --max-time 60 -A "$UA" "$1" | tr -d '\r' | awk -v f="$2" 'BEGIN{IGNORECASE=1} tolower($1)==tolower(f)":" {sub(/^[^:]*:[ ]*/,""); v=$0} END{print v}'
}

do_dates() {
  mkdir -p "$RAW"
  local tmp="$RAW/SOURCES.json.tmp"
  {
    echo "{"
    local first=1
    for entry in "${FILES[@]}"; do
      local name="${entry%%|*}" url="${entry#*|}"
      local lm size
      lm="$(header "$url" last-modified)"
      size="$(header "$url" content-length)"
      [ $first -eq 1 ] || echo ","
      first=0
      printf ' "%s": {"url": "%s", "last_modified": "%s", "content_length": "%s"}' "$name" "$url" "$lm" "$size"
    done
    echo
    echo "}"
  } > "$tmp"
  mv "$tmp" "$RAW/SOURCES.json"
  echo "wrote $RAW/SOURCES.json"
}

do_fetch() {
  mkdir -p "$RAW"
  for entry in "${FILES[@]}"; do
    local name="${entry%%|*}" url="${entry#*|}"
    local dest="$RAW/$name" remote local_size
    remote="$(header "$url" content-length)"
    local_size=0
    [ -f "$dest" ] && local_size="$(stat -c %s "$dest")"
    if [ -n "$remote" ] && [ "$local_size" = "$remote" ]; then
      echo "$name: complete ($local_size bytes), skipped"
      continue
    fi
    echo "$name: downloading (have $local_size of ${remote:-?} bytes)"
    curl -L --fail --retry 5 --retry-delay 10 -C - -A "$UA" -o "$dest" "$url"
  done
  do_dates
  do_record
}

do_record() {
  local names=()
  for entry in "${FILES[@]}"; do
    local name="${entry%%|*}"
    [ -f "$RAW/$name" ] && names+=("$name")
  done
  (cd "$RAW" && sha256sum "${names[@]}") > "$RAW/SHA256SUMS.tmp"
  mv "$RAW/SHA256SUMS.tmp" "$RAW/SHA256SUMS"
  echo "wrote $RAW/SHA256SUMS (${#names[@]} files)"
}

do_check() {
  if [ ! -f "$RAW/SHA256SUMS" ]; then echo "no $RAW/SHA256SUMS (run: $0 record)" >&2; exit 1; fi
  (cd "$RAW" && sha256sum -c SHA256SUMS)
}

case "$MODE" in
  fetch) do_fetch ;;
  dates) do_dates ;;
  record) do_record ;;
  check) do_check ;;
esac
