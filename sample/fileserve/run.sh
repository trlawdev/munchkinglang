#!/usr/bin/env bash
# Run the demo fileserve instance (interpreter backend).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"
if [[ ! -x ./munxc && ! -x ./build/munxc ]]; then
  echo "munxc not found; build the project first" >&2
  exit 1
fi
MUNXC=./munxc
[[ -x ./build/munxc ]] && MUNXC=./build/munxc
"$MUNXC" sample/fileserve/main.mx
exec "$MUNXC" --run --interp sample/fileserve/main.mxb -- \
  --root sample/fileserve/www \
  --host 127.0.0.1 \
  --port "${PORT:-8080}" \
  "$@"
