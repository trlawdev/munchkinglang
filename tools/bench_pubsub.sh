#!/usr/bin/env bash
# Pub/sub throughput: marketfeed exchange → tape under interp / JIT / native AOT.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MUNXC="${ROOT}/munxc"
COUNT="${COUNT:-2000}"
RUNS="${RUNS:-3}"
HOLD_MS="${HOLD_MS:-5000}"
TAPE_TIMEOUT="${TAPE_TIMEOUT:-120}"
WORKDIR="${TMPDIR:-/tmp}/munx-bench-pubsub-$$"

if [[ ! -x "$MUNXC" ]]; then
  echo "Build munxc first: ./build.sh" >&2
  exit 1
fi

mkdir -p "$WORKDIR"
HUB_PIDS=()
cleanup() {
  local pid
  for pid in "${HUB_PIDS[@]:-}"; do
    kill "$pid" 2>/dev/null || true
  done
  rm -rf "$WORKDIR"
}
trap cleanup EXIT

sed -e "s/^COUNT = .*/COUNT = ${COUNT}/" \
    -e "s/^HOLD_MS = .*/HOLD_MS = ${HOLD_MS}/" \
    "$ROOT/sample/marketfeed/exchange.mx" >"$WORKDIR/exchange.mx"
sed -e "s/^EXPECT = .*/EXPECT = ${COUNT}/" \
    "$ROOT/sample/marketfeed/tape.mx" >"$WORKDIR/tape.mx"

echo "Compiling native AOT binaries…"
"$MUNXC" --native -o "$WORKDIR/exchange_native" "$WORKDIR/exchange.mx"
"$MUNXC" --native -o "$WORKDIR/tape_native" "$WORKDIR/tape.mx"

ms_now() { date +%s%N; }

run_pipeline() {
  local mode="$1"
  shift
  local -a exch_cmd=()
  local -a tape_cmd=()
  local side=exch
  for arg in "$@"; do
    if [[ "$arg" == "--" ]]; then
      side=tape
      continue
    fi
    if [[ "$side" == exch ]]; then
      exch_cmd+=("$arg")
    else
      tape_cmd+=("$arg")
    fi
  done

  local pipe_dir="$WORKDIR/pipes_${mode}_$RANDOM"
  rm -rf "$pipe_dir"
  mkdir -p "$pipe_dir"
  export MUNX_PIPE_DIR="$pipe_dir"
  export MUNX_HUB_EXECUTABLE="$MUNXC"

  "$MUNXC" --pipe-hub >"$WORKDIR/hub_${mode}.log" 2>&1 &
  local hub_pid=$!
  HUB_PIDS+=("$hub_pid")
  sleep 0.25
  if ! kill -0 "$hub_pid" 2>/dev/null; then
    echo "hub failed ($mode)" >&2
    cat "$WORKDIR/hub_${mode}.log" >&2 || true
    return 1
  fi

  local pub_log="$WORKDIR/pub_${mode}.log"
  local sub_log="$WORKDIR/sub_${mode}.log"
  rm -f "$pub_log" "$sub_log"

  stdbuf -oL "${exch_cmd[@]}" >"$pub_log" 2>&1 &
  local pub_pid=$!

  local ready=0
  local i
  for i in $(seq 1 200); do
    if grep -q 'exchange:online' "$pub_log" 2>/dev/null && kill -0 "$pub_pid" 2>/dev/null; then
      ready=1
      break
    fi
    sleep 0.05
  done
  if [[ "$ready" -ne 1 ]]; then
    echo "publisher not ready ($mode): $(cat "$pub_log" 2>/dev/null)" >&2
    kill "$pub_pid" "$hub_pid" 2>/dev/null || true
    return 1
  fi

  local start end elapsed
  start=$(ms_now)
  set +e
  timeout "$TAPE_TIMEOUT" stdbuf -oL "${tape_cmd[@]}" >"$sub_log" 2>&1
  local sub_rc=$?
  set -e
  end=$(ms_now)
  elapsed=$(( (end - start) / 1000000 ))

  kill "$pub_pid" 2>/dev/null || true
  wait "$pub_pid" 2>/dev/null || true
  kill "$hub_pid" 2>/dev/null || true
  wait "$hub_pid" 2>/dev/null || true

  if [[ "$sub_rc" -ne 0 ]] || ! grep -q "tape:received=${COUNT}" "$sub_log"; then
    echo "subscriber failed ($mode, rc=$sub_rc):" >&2
    echo "  pub: $(tr '\n' ' ' <"$pub_log")" >&2
    echo "  sub: $(tr '\n' ' ' <"$sub_log")" >&2
    return 1
  fi

  printf '%s\n' "$elapsed"
}

bench_mode() {
  local mode="$1"
  shift
  local total=0
  local run
  echo "=== ${mode} (COUNT=${COUNT}, RUNS=${RUNS}) ==="
  for run in $(seq 1 "$RUNS"); do
    local ms
    ms=$(run_pipeline "$mode" "$@") || {
      echo "  run ${run}: FAILED" >&2
      return 1
    }
    total=$((total + ms))
    echo "  run ${run}: ${ms} ms"
  done
  local avg=$((total / RUNS))
  local rate=0
  if [[ "$avg" -gt 0 ]]; then
    rate=$(( COUNT * 1000 / avg ))
  fi
  echo "  avg: ${avg} ms  →  ~${rate} msg/s"
  echo
  echo "${mode}|${avg}|${rate}" >>"$WORKDIR/summary.tsv"
}

rm -f "$WORKDIR/summary.tsv"

bench_mode "interp" \
  "$MUNXC" --run --interp "$WORKDIR/exchange.mx" -- \
  "$MUNXC" --run --interp "$WORKDIR/tape.mx"

bench_mode "jit" \
  "$MUNXC" --run "$WORKDIR/exchange.mx" -- \
  "$MUNXC" --run "$WORKDIR/tape.mx"

bench_mode "native" \
  "$WORKDIR/exchange_native" -- \
  "$WORKDIR/tape_native"

echo "======== summary (COUNT=${COUNT}) ========"
printf '%-10s %10s %12s\n' "mode" "avg_ms" "msg_per_s"
if [[ -f "$WORKDIR/summary.tsv" ]]; then
  while IFS='|' read -r mode avg rate; do
    printf '%-10s %10s %12s\n' "$mode" "$avg" "$rate"
  done <"$WORKDIR/summary.tsv"
fi
