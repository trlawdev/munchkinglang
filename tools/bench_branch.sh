#!/usr/bin/env bash
# Branch microbenchmark for munx threaded JIT.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MUNXC="${ROOT}/munxc"
RUNS="${RUNS:-5}"

if [[ ! -x "$MUNXC" ]]; then
  echo "Build munxc first: ./build.sh" >&2
  exit 1
fi

bench_one() {
  local sample="$1"
  local total=0
  local index=0
  while [[ "$index" -lt "$RUNS" ]]; do
    local start end elapsed
    start=$(date +%s%N)
    env MUNX_PIPE_HUB=0 "$MUNXC" --run "$sample" >/dev/null
    end=$(date +%s%N)
    elapsed=$(( (end - start) / 1000000 ))
    total=$(( total + elapsed ))
    index=$(( index + 1 ))
  done
  echo "$(( total / RUNS ))ms (avg of $RUNS)"
}

for sample in branch_count branch_random branch_nested branch_mixed; do
  path="${ROOT}/sample/bench/${sample}.mx"
  echo "=== ${sample} ==="
  bench_one "$path"
  if command -v perf >/dev/null 2>&1; then
    echo "perf:"
    perf stat -e cycles,branches,branch-misses \
      env MUNX_PIPE_HUB=0 "$MUNXC" --run "$path" >/dev/null 2>&1 || true
  fi
  echo
done
