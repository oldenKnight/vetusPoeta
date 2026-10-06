#!/bin/sh
# Runs the UI checks and unit tests (tools/jstest/run.js). Usage: tools/jstest.sh [pattern] [--verbose]
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
NODE="${NODE:-}"
if [ -z "$NODE" ]; then
  if command -v node >/dev/null 2>&1; then NODE=node; elif [ -x /opt/node22/bin/node ]; then NODE=/opt/node22/bin/node; else
    echo "jstest: Node.js (>= 18) not found; set NODE=/path/to/node" >&2; exit 2; fi
fi
exec "$NODE" "$HERE/jstest/run.js" "$@"
