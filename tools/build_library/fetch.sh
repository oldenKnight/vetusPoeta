#!/usr/bin/env bash
# Download the raw dictionary dumps into data/raw (gitignored) and record their SHA-256 and dump dates.
#
#   tools/build_library/fetch.sh [--raw DIR] [fetch|check|record|dates|aux|auxcheck]
#     fetch   (default) resumable download of the four files, then `dates` and `record`
#     aux     download the auxiliary sources (Whitaker, Perseus Lewis & Short + LSJ, DCC core lists) into
#             DIR/aux and write DIR/aux/SOURCES.json (url, fetch date, size, last-modified, sha256, licence)
#     auxcheck  verify DIR/aux/SHA256SUMS
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
    fetch|check|record|dates|aux|auxcheck) MODE="$1"; shift ;;
    -h|--help) sed -n '2,15p' "$0"; exit 0 ;;
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

# auxiliary sources: name|url|licence
WW="https://raw.githubusercontent.com/mk270/whitakers-words/master"
PL="https://raw.githubusercontent.com/PerseusDL/lexica/master/CTS_XML_TEI/perseus/pdllex"
WW_LIC="Whitaker's Words: permission text in README.md / LICENCE.txt (quoted in tools/build_library/README.md)"
PL_LIC="CC BY-SA 4.0 (PerseusDL/lexica README)"
DCC_LIC="CC BY-SA 3.0 Unported (https://dcc.dickinson.edu/vocab/core-vocabulary)"
AUX_FILES=(
  "whitaker/DICTLINE.GEN|$WW/DICTLINE.GEN|$WW_LIC"
  "whitaker/INFLECTS.LAT|$WW/INFLECTS.LAT|$WW_LIC"
  "whitaker/ADDONS.LAT|$WW/ADDONS.LAT|$WW_LIC"
  "whitaker/UNIQUES.LAT|$WW/UNIQUES.LAT|$WW_LIC"
  "whitaker/README.md|$WW/README.md|$WW_LIC"
  "whitaker/LICENCE.txt|$WW/LICENCE.txt|$WW_LIC"
  "whitaker/latin_utils-dictionary_package.ads|$WW/src/latin_utils/latin_utils-dictionary_package.ads|$WW_LIC"
  "whitaker/latin_utils-inflections_package.ads|$WW/src/latin_utils/latin_utils-inflections_package.ads|$WW_LIC"
  "perseus/lat.ls.perseus-eng1.xml|$PL/lat/ls/lat.ls.perseus-eng1.xml|$PL_LIC"
  "perseus/README.md|https://raw.githubusercontent.com/PerseusDL/lexica/master/README.md|$PL_LIC"
  "dcc/latin-core-list.csv|https://dcc.dickinson.edu/latin-core-list.csv?page&_format=csv|$DCC_LIC"
  "dcc/latin-core-list-es.csv|https://dcc.dickinson.edu/es/latin-core-list.csv?page&_format=csv|$DCC_LIC; Spanish translation credited to Francisco Javier Pérez Cartagena"
  "dcc/greek-core-list.csv|https://dcc.dickinson.edu/greek-core-list.csv?page&_format=csv|$DCC_LIC"
)
for i in $(seq 1 27); do
  AUX_FILES+=("perseus/grc.lsj.perseus-eng$i.xml|$PL/grc/lsj/grc.lsj.perseus-eng$i.xml|$PL_LIC")
done

json_str() { python3 -c 'import json,sys; print(json.dumps(sys.argv[1], ensure_ascii=False))' "$1"; }

do_aux() {
  local AUX="$RAW/aux"
  mkdir -p "$AUX/whitaker" "$AUX/perseus" "$AUX/dcc"
  local today
  today="$(date -u +%Y-%m-%d)"
  for entry in "${AUX_FILES[@]}"; do
    local name="${entry%%|*}" rest="${entry#*|}"
    local url="${rest%%|*}"
    local dest="$AUX/$name"
    if [ -s "$dest" ] && [ -z "${VP_AUX_REFETCH:-}" ]; then
      echo "$name: present ($(stat -c %s "$dest") bytes), skipped (VP_AUX_REFETCH=1 to refetch)"
      continue
    fi
    echo "$name: downloading"
    curl -L --fail --retry 5 --retry-delay 10 -sS -A "$UA" -o "$dest.part" "$url"
    mv "$dest.part" "$dest"
    printf '%s\n' "$today" > "$dest.fetched"
    curl -sSIL --max-time 60 -A "$UA" "$url" | tr -d '\r' | awk 'BEGIN{IGNORECASE=1} tolower($1)=="last-modified:" {sub(/^[^:]*:[ ]*/,""); v=$0} tolower($1)=="content-type:" {sub(/^[^:]*:[ ]*/,""); t=$0} END{print v; print t}' > "$dest.headers" || true
  done
  # DCC: the server must return CSV, not an HTML page
  for f in dcc/latin-core-list.csv dcc/latin-core-list-es.csv dcc/greek-core-list.csv; do
    if head -c 200 "$AUX/$f" | grep -qi '<html\|<!doctype'; then echo "$f: not CSV" >&2; exit 1; fi
  done
  local tmp="$AUX/SOURCES.json.tmp" first=1
  {
    echo "{"
    for entry in "${AUX_FILES[@]}"; do
      local name="${entry%%|*}" rest="${entry#*|}"
      local url="${rest%%|*}" lic="${rest#*|}"
      local dest="$AUX/$name" fetched="" lm="" ctype=""
      [ -f "$dest.fetched" ] && fetched="$(cat "$dest.fetched")"
      if [ -f "$dest.headers" ]; then lm="$(sed -n 1p "$dest.headers")"; ctype="$(sed -n 2p "$dest.headers")"; fi
      [ $first -eq 1 ] || echo ","
      first=0
      printf ' %s: {"url": %s, "fetched": "%s", "last_modified": %s, "content_type": %s, "size": %s, "sha256": "%s", "licence": %s}' \
        "$(json_str "$name")" "$(json_str "$url")" "$fetched" "$(json_str "$lm")" "$(json_str "$ctype")" \
        "$(stat -c %s "$dest")" "$(sha256sum "$dest" | cut -d' ' -f1)" "$(json_str "$lic")"
    done
    echo
    echo "}"
  } > "$tmp"
  mv "$tmp" "$AUX/SOURCES.json"
  (cd "$AUX" && for entry in "${AUX_FILES[@]}"; do echo "${entry%%|*}"; done | xargs sha256sum) > "$AUX/SHA256SUMS"
  echo "wrote $AUX/SOURCES.json and $AUX/SHA256SUMS"
}

do_auxcheck() {
  (cd "$RAW/aux" && sha256sum -c SHA256SUMS)
}

case "$MODE" in
  fetch) do_fetch ;;
  dates) do_dates ;;
  record) do_record ;;
  check) do_check ;;
  aux) do_aux ;;
  auxcheck) do_auxcheck ;;
esac
