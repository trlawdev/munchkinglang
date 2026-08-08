#!/usr/bin/env bash
# Compile / smoke-run sample programs (VM + native pipehub).
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"
export MUNX_HUB_EXECUTABLE="${MUNX_HUB_EXECUTABLE:-$root/munxc}"

pass=0
fail=0
skip=0

ok() { echo "  PASS $*"; pass=$((pass + 1)); }
bad() { echo "  FAIL $*"; fail=$((fail + 1)); }
skp() { echo "  SKIP $*"; skip=$((skip + 1)); }

echo "== Build munxc =="
./build.sh >/tmp/munx_suite_build.log 2>&1 || { tail -20 /tmp/munx_suite_build.log; exit 1; }

echo "== Bytecode compile: sample/valid/*.mx =="
shopt -s nullglob
for f in sample/valid/*.mx; do
  # Package-relative imports are not resolvable from this tree alone.
  [[ "$(basename "$f")" == "01_package_imports.mx" ]] && { skp "compile $f (missing packages)"; continue; }
  if MUNX_PIPE_HUB=0 ./munxc "$f" >/tmp/munx_suite_compile.log 2>&1; then
    ok "compile $f"
  else
    bad "compile $f ($(head -1 /tmp/munx_suite_compile.log))"
  fi
done

echo "== Bytecode compile: sample entrypoints =="
for f in sample/pipehub/*.mx sample/channels/*.mx sample/marketfeed/*.mx \
         sample/chatrelay/main.mx \
         sample/logscope/main.mx sample/shipyard/main.mx sample/matrix_tool/main.mx \
         sample/io.mx sample/string.mx sample/process_.mx sample/thread_.mx \
         sample/value_model_test.mx; do
  [[ -f "$f" ]] || continue
  [[ "$f" == *.md ]] && continue
  if MUNX_PIPE_HUB=0 ./munxc "$f" >/tmp/munx_suite_compile.log 2>&1; then
    ok "compile $f"
  else
    bad "compile $f ($(head -1 /tmp/munx_suite_compile.log))"
  fi
done
# sample/sample.mx depends on external package `lib1`.
skp "compile sample/sample.mx (missing package lib1)"

echo "== Native compile smoke (subset) =="
mkdir -p /tmp/munx_suite_native
for f in tests/programs/hello.mx tests/programs/arithmetic.mx \
         tests/programs/loops.mx tests/programs/functions.mx \
         tests/programs/if_else.mx \
         sample/pipehub/publisher.mx sample/pipehub/subscriber.mx \
         sample/pipehub/queue_reader.mx \
         sample/channels/peer_a.mx sample/channels/peer_b.mx \
         sample/reflexpr_.mx \
         sample/marketfeed/exchange.mx sample/marketfeed/tape.mx \
         sample/marketfeed/risk.mx; do
  base=$(basename "$f" .mx)
  if ./munxc --native -o "/tmp/munx_suite_native/$base" "$f" \
        >/tmp/munx_suite_native.log 2>&1; then
    ok "native $f"
  else
    bad "native $f ($(head -1 /tmp/munx_suite_native.log))"
  fi
done

echo "== Native pipehub interop =="
export MUNX_PIPE_DIR=/tmp/munx-suite-pipes
rm -rf "$MUNX_PIPE_DIR"
rm -f /tmp/munx_suite_pub.txt /tmp/munx_suite_sub.txt
./munxc --pipe-hub >/tmp/munx_suite_hub.log 2>&1 &
hub_pid=$!
sleep 0.4
if ! kill -0 "$hub_pid" 2>/dev/null; then
  bad "hub failed to start"
else
  # Publisher-first: writer must stay attached while the subscriber attaches.
  # Line-buffer stdout so "published:" is visible before sleep() finishes.
  stdbuf -oL /tmp/munx_suite_native/publisher "suite-ping" \
    >/tmp/munx_suite_pub.txt 2>&1 &
  pub_pid=$!
  ready=0
  for _ in $(seq 1 80); do
    if grep -q 'published: suite-ping' /tmp/munx_suite_pub.txt 2>/dev/null &&
       kill -0 "$pub_pid" 2>/dev/null; then
      ready=1
      break
    fi
    sleep 0.1
  done
  if [[ "$ready" -ne 1 ]]; then
    bad "native pub/sub (publisher not ready: $(cat /tmp/munx_suite_pub.txt 2>/dev/null))"
  else
    timeout 10 stdbuf -oL /tmp/munx_suite_native/subscriber \
      >/tmp/munx_suite_sub.txt 2>&1 || true
    if grep -q 'received: suite-ping' /tmp/munx_suite_sub.txt; then
      ok "native pub/sub"
    else
      bad "native pub/sub (out=$(cat /tmp/munx_suite_sub.txt) pub=$(cat /tmp/munx_suite_pub.txt))"
    fi
  fi
  kill "$pub_pid" 2>/dev/null || true
  wait "$pub_pid" 2>/dev/null || true
fi
kill "$hub_pid" 2>/dev/null || true
wait "$hub_pid" 2>/dev/null || true

echo "== Marketfeed pub/sub (native AOT) =="
export MUNX_PIPE_DIR=/tmp/munx-suite-marketfeed
rm -rf "$MUNX_PIPE_DIR"
mkdir -p /tmp/munx_suite_mf
sed -e 's/^COUNT = .*/COUNT = 200/' -e 's/^HOLD_MS = .*/HOLD_MS = 1500/' \
  sample/marketfeed/exchange.mx >/tmp/munx_suite_mf/exchange.mx
sed -e 's/^EXPECT = .*/EXPECT = 200/' \
  sample/marketfeed/tape.mx >/tmp/munx_suite_mf/tape.mx
cp sample/marketfeed/risk.mx /tmp/munx_suite_mf/risk.mx
./munxc --native -o /tmp/munx_suite_mf/exchange /tmp/munx_suite_mf/exchange.mx \
  >/tmp/munx_suite_mf_build.log 2>&1
./munxc --native -o /tmp/munx_suite_mf/tape /tmp/munx_suite_mf/tape.mx \
  >/tmp/munx_suite_mf_build.log 2>&1
./munxc --native -o /tmp/munx_suite_mf/risk /tmp/munx_suite_mf/risk.mx \
  >/tmp/munx_suite_mf_build.log 2>&1
./munxc --pipe-hub >/tmp/munx_suite_mf_hub.log 2>&1 &
mf_hub_pid=$!
sleep 0.4
if ! kill -0 "$mf_hub_pid" 2>/dev/null; then
  bad "marketfeed hub failed to start"
else
  stdbuf -oL /tmp/munx_suite_mf/exchange >/tmp/munx_suite_mf_ex.txt 2>&1 &
  mf_ex_pid=$!
  for _ in $(seq 1 80); do
    grep -q 'exchange:online' /tmp/munx_suite_mf_ex.txt 2>/dev/null && break
    sleep 0.05
  done
  stdbuf -oL /tmp/munx_suite_mf/tape >/tmp/munx_suite_mf_tape.txt 2>&1 &
  mf_tape_pid=$!
  timeout 15 stdbuf -oL /tmp/munx_suite_mf/risk >/tmp/munx_suite_mf_risk.txt 2>&1 || true
  for _ in $(seq 1 100); do
    grep -q 'tape:done' /tmp/munx_suite_mf_tape.txt 2>/dev/null && break
    sleep 0.05
  done
  if grep -q 'tape:received=200' /tmp/munx_suite_mf_tape.txt &&
     grep -q 'risk:done' /tmp/munx_suite_mf_risk.txt; then
    ok "native marketfeed"
  else
    bad "native marketfeed (tape=$(tr '\n' '|' </tmp/munx_suite_mf_tape.txt) risk=$(tr '\n' '|' </tmp/munx_suite_mf_risk.txt))"
  fi
  kill "$mf_ex_pid" "$mf_tape_pid" 2>/dev/null || true
  wait "$mf_ex_pid" "$mf_tape_pid" 2>/dev/null || true
fi
kill "$mf_hub_pid" 2>/dev/null || true
wait "$mf_hub_pid" 2>/dev/null || true

echo "== VM channel peer smoke =="
export MUNX_PIPE_DIR=/tmp/munx-suite-channels
rm -rf "$MUNX_PIPE_DIR"
rm -f /tmp/munx_suite_ch_a.txt /tmp/munx_suite_ch_b.txt
./munxc --pipe-hub >/tmp/munx_suite_ch_hub.log 2>&1 &
ch_hub_pid=$!
sleep 0.4
if ! kill -0 "$ch_hub_pid" 2>/dev/null; then
  bad "channel hub failed to start"
else
  # Line-buffer stdout so the suite can observe "waiting" before peer B starts.
  stdbuf -oL ./munxc --run --interp sample/channels/peer_a.mx \
    >/tmp/munx_suite_ch_a.txt 2>&1 &
  ch_a_pid=$!
  for _ in $(seq 1 40); do
    grep -q 'waiting for a message' /tmp/munx_suite_ch_a.txt 2>/dev/null && break
    sleep 0.1
  done
  sleep 0.2
  timeout 10 stdbuf -oL ./munxc --run --interp sample/channels/peer_b.mx "suite-ch" \
    >/tmp/munx_suite_ch_b.txt 2>&1 || true
  timeout 10 wait "$ch_a_pid" 2>/dev/null || kill "$ch_a_pid" 2>/dev/null || true
  if grep -q 'peer A received: suite-ch' /tmp/munx_suite_ch_a.txt &&
     grep -q 'peer B received: ack from A' /tmp/munx_suite_ch_b.txt; then
    ok "vm channel peers"
  else
    bad "vm channel peers (a=$(cat /tmp/munx_suite_ch_a.txt) b=$(cat /tmp/munx_suite_ch_b.txt))"
  fi
  kill "$ch_a_pid" 2>/dev/null || true
fi
kill "$ch_hub_pid" 2>/dev/null || true
wait "$ch_hub_pid" 2>/dev/null || true

echo "== Native channel peer smoke =="
export MUNX_PIPE_DIR=/tmp/munx-suite-channels-native
export MUNX_HUB_EXECUTABLE="$root/munxc"
rm -rf "$MUNX_PIPE_DIR"
rm -f /tmp/munx_suite_nch_a.txt /tmp/munx_suite_nch_b.txt
./munxc --pipe-hub >/tmp/munx_suite_nch_hub.log 2>&1 &
nch_hub_pid=$!
sleep 0.4
if ! kill -0 "$nch_hub_pid" 2>/dev/null; then
  bad "native channel hub failed to start"
elif [[ ! -x /tmp/munx_suite_native/peer_a || ! -x /tmp/munx_suite_native/peer_b ]]; then
  bad "native channel binaries missing (compile smoke failed?)"
else
  stdbuf -oL /tmp/munx_suite_native/peer_a \
    >/tmp/munx_suite_nch_a.txt 2>&1 &
  nch_a_pid=$!
  for _ in $(seq 1 40); do
    grep -q 'waiting for a message' /tmp/munx_suite_nch_a.txt 2>/dev/null && break
    sleep 0.1
  done
  sleep 0.2
  timeout 10 stdbuf -oL /tmp/munx_suite_native/peer_b "suite-nch" \
    >/tmp/munx_suite_nch_b.txt 2>&1 || true
  timeout 10 wait "$nch_a_pid" 2>/dev/null || kill "$nch_a_pid" 2>/dev/null || true
  if grep -q 'peer A received: suite-nch' /tmp/munx_suite_nch_a.txt &&
     grep -q 'peer B received: ack from A' /tmp/munx_suite_nch_b.txt; then
    ok "native channel peers"
  else
    bad "native channel peers (a=$(cat /tmp/munx_suite_nch_a.txt) b=$(cat /tmp/munx_suite_nch_b.txt))"
  fi
  kill "$nch_a_pid" 2>/dev/null || true
fi
kill "$nch_hub_pid" 2>/dev/null || true
wait "$nch_hub_pid" 2>/dev/null || true

echo "== Compile-time reflexpr =="
if ./munxc --run --interp sample/reflexpr_.mx >/tmp/munx_suite_refl.txt 2>&1 &&
   grep -q 'primitive member: name' /tmp/munx_suite_refl.txt &&
   grep -q 'object member: detail' /tmp/munx_suite_refl.txt; then
  ok "interp reflexpr"
else
  bad "interp reflexpr ($(tr '\n' '|' </tmp/munx_suite_refl.txt))"
fi
if [[ -x /tmp/munx_suite_native/reflexpr_ ]]; then
  if /tmp/munx_suite_native/reflexpr_ >/tmp/munx_suite_refl_n.txt 2>&1 &&
     grep -q 'primitive member: name' /tmp/munx_suite_refl_n.txt &&
     grep -q 'object member: detail' /tmp/munx_suite_refl_n.txt; then
    ok "native reflexpr"
  else
    bad "native reflexpr ($(tr '\n' '|' </tmp/munx_suite_refl_n.txt))"
  fi
else
  bad "native reflexpr binary missing"
fi

echo "== VM run smokes (hub disabled) =="
for f in tests/programs/hello.mx tests/programs/arithmetic.mx; do
  if MUNX_PIPE_HUB=0 ./munxc --run "$f" >/tmp/munx_suite_run.txt 2>&1; then
    ok "run $f"
  else
    bad "run $f"
  fi
done

echo
echo "Summary: pass=$pass fail=$fail skip=$skip"
[[ "$fail" -eq 0 ]]
