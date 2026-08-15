#!/usr/bin/env bash
# Compare stack VM (interp / JIT) vs mx32 pipelines (scalar / inorder / ooo).
#
# Env:
#   RUNS=5                 repeats per cell (avg)
#   LIMIT=1000000          loop trip count (nested uses isqrt(LIMIT)^2)
#   SAMPLES="..."          which benches
#   INCLUDE_SLOW=1         include mx-inorder + mx-ooo-rob (very slow at high LIMIT)
#   MUNXC=./build/munxc
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MUNXC="${MUNXC:-}"
if [[ -z "$MUNXC" ]]; then
  if [[ -x "$ROOT/build/munxc" ]]; then
    MUNXC="$ROOT/build/munxc"
  elif [[ -x "$ROOT/munxc" ]]; then
    MUNXC="$ROOT/munxc"
  else
    echo "Build munxc first: cmake --build build" >&2
    exit 1
  fi
fi

RUNS="${RUNS:-5}"
LIMIT="${LIMIT:-1000000}"
SAMPLES="${SAMPLES:-branch_count branch_nested branch_mixed branch_random}"
INCLUDE_SLOW="${INCLUDE_SLOW:-0}"
OUT="${OUT:-${TMPDIR:-/tmp}/munx-bench-pipeline-$$}"
mkdir -p "$OUT"

export MUNX_PIPE_HUB=0

# At large trip counts, pipeline simulators dominate wall time; opt-in.
if [[ "$LIMIT" -ge 10000000 && "$INCLUDE_SLOW" != "1" ]]; then
  INCLUDE_SLOW=0
fi

ms_now() { date +%s%N; }

avg_ms() {
  local total="$1" n="$2"
  if [[ "$n" -le 0 ]]; then
    echo "n/a"
    return
  fi
  echo "$(( total / n ))"
}

time_cmd() {
  local total=0 ok=0 index=0
  while [[ "$index" -lt "$RUNS" ]]; do
    local start end elapsed ec=0
    start=$(ms_now)
    set +e
    "$@" >/dev/null 2>"$OUT/last.err"
    ec=$?
    set -e
    end=$(ms_now)
    if [[ "$ec" -ne 0 ]]; then
      echo "FAIL(ec=$ec)"
      if [[ -s "$OUT/last.err" ]]; then
        sed -n '1,3p' "$OUT/last.err" >&2
      fi
      return 0
    fi
    elapsed=$(( (end - start) / 1000000 ))
    total=$(( total + elapsed ))
    ok=$(( ok + 1 ))
    index=$(( index + 1 ))
  done
  echo "$(avg_ms "$total" "$ok")ms"
}

prepare_src() {
  local sample="$1"
  local src="$ROOT/sample/bench/${sample}.mx"
  local dst="$OUT/${sample}.mx"
  if [[ "$sample" == "branch_nested" ]]; then
    local side
    side="$(python3 -c "import math; print(max(1, int(math.isqrt(int('$LIMIT')))))")"
    sed -e "s/outer_limit = 1000/outer_limit = ${side}/" \
        -e "s/inner_limit = 1000/inner_limit = ${side}/" \
        "$src" >"$dst"
    echo "${sample}: outer=inner=${side} (~$((side * side)) iters)" >&2
  else
    sed -e "s/limit = 1000000/limit = ${LIMIT}/" "$src" >"$dst"
    echo "${sample}: limit=${LIMIT}" >&2
  fi
}

echo "munxc: $MUNXC"
echo "runs:  $RUNS   limit: $LIMIT   slow-pipes: $INCLUDE_SLOW"
echo "out:   $OUT"
echo

if [[ "$INCLUDE_SLOW" == "1" ]]; then
  printf '%-16s  %-12s  %-12s  %-12s  %-12s  %-12s  %-12s\n' \
    "sample" "v8-interp" "v8-jit" "mx-scalar" "mx-inorder" "mx-ooo" "mx-ooo-rob"
  printf '%s\n' "----------------------------------------------------------------------------------------------------------------"
else
  printf '%-16s  %-12s  %-12s  %-12s  %-12s\n' \
    "sample" "v8-interp" "v8-jit" "mx-scalar" "mx-ooo"
  printf '%s\n' "----------------------------------------------------------------------------"
fi

for sample in $SAMPLES; do
  src="$ROOT/sample/bench/${sample}.mx"
  if [[ ! -f "$src" ]]; then
    echo "skip missing $src" >&2
    continue
  fi

  prepare_src "$sample"
  work="$OUT/${sample}.mx"
  v8_mxb="$OUT/${sample}.v8.mxb"
  mx_mxb="$OUT/${sample}.mx32.mxb"

  # Compile once into $OUT (do not touch repo sample/*.mxb).
  env MUNX_PIPE_HUB=0 "$MUNXC" --run "$work" >/dev/null
  cp -f "${work%.mx}.mxb" "$v8_mxb"

  env MUNX_PIPE_HUB=0 "$MUNXC" --run --mx32 "$work" >/dev/null
  cp -f "${work%.mx}.mxb" "$mx_mxb"

  v8_interp=$(time_cmd env MUNX_PIPE_HUB=0 MUNX_VM_JIT=0 "$MUNXC" --run --interp "$v8_mxb")
  v8_jit=$(time_cmd env MUNX_PIPE_HUB=0 "$MUNXC" --run "$v8_mxb")
  mx_scalar=$(time_cmd env MUNX_PIPE_HUB=0 MUNX_VM_PIPELINE=scalar "$MUNXC" --run "$mx_mxb")
  mx_ooo=$(time_cmd env MUNX_PIPE_HUB=0 MUNX_VM_PIPELINE=ooo "$MUNXC" --run "$mx_mxb")

  if [[ "$INCLUDE_SLOW" == "1" ]]; then
    mx_inorder=$(time_cmd env MUNX_PIPE_HUB=0 MUNX_VM_PIPELINE=inorder "$MUNXC" --run "$mx_mxb")
    mx_rob=$(time_cmd env MUNX_PIPE_HUB=0 MUNX_VM_PIPELINE=ooo MUNX_VM_OOO_ROB=1 "$MUNXC" --run "$mx_mxb")
    printf '%-16s  %-12s  %-12s  %-12s  %-12s  %-12s  %-12s\n' \
      "$sample" "$v8_interp" "$v8_jit" "$mx_scalar" "$mx_inorder" "$mx_ooo" "$mx_rob"
  else
    printf '%-16s  %-12s  %-12s  %-12s  %-12s\n' \
      "$sample" "$v8_interp" "$v8_jit" "$mx_scalar" "$mx_ooo"
  fi
done

echo
echo "Notes:"
echo "  Trip count LIMIT=$LIMIT (nested ≈ isqrt(LIMIT)²)."
echo "  Compile/lower excluded; times are image run only (avg of $RUNS)."
if [[ "$INCLUDE_SLOW" != "1" ]]; then
  echo "  Skipped mx-inorder / mx-ooo-rob (set INCLUDE_SLOW=1 to enable)."
fi
