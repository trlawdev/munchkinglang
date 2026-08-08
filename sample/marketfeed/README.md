# marketfeed — real-world pipe pub/sub

Simulates a **stock-exchange last-trade feed**:

| Process | Role | Pipe |
|---------|------|------|
| `exchange.mx` | Matching engine (publisher) | `market.ticks` (all prints), `market.blocks` (block trades) |
| `tape.mx` | Consolidated tape logger | subscribe `market.ticks` |
| `risk.mx` | Risk / surveillance | subscribe `market.blocks` |

Each subscriber uses `pipe(name, subscribe)`, so delivery is **fan-out**: every
subscriber gets a copy. The hub requires the exchange (writer) to attach first;
ticks published before a subscriber connects sit in the per-pipe pending queue.

Message wire format: `KIND|SYM|PRICE|QTY|SEQ` plus a final `END` sentinel.

## Interactive demo (VM / JIT)

```bash
# terminal 1
./munxc --run sample/marketfeed/exchange.mx

# terminal 2
./munxc --run sample/marketfeed/tape.mx

# terminal 3
./munxc --run sample/marketfeed/risk.mx
```

Force the bytecode interpreter with `--interp`. Native AOT:

```bash
./munxc --native -o /tmp/exchange sample/marketfeed/exchange.mx
./munxc --native -o /tmp/tape sample/marketfeed/tape.mx
./munxc --native -o /tmp/risk sample/marketfeed/risk.mx
# then run the binaries the same way (publisher first)
```

## Throughput benchmark (interp / JIT / native)

```bash
./tools/bench_pubsub.sh
COUNT=10000 RUNS=3 ./tools/bench_pubsub.sh
```

Reports wall-clock time and messages/sec for the exchange→tape pipeline under:

1. **interp** — `munxc --run --interp`
2. **JIT** — `munxc --run` (default threaded JIT)
3. **native** — `munxc --native` host executable

Example on a local Linux host after the DGRAM hub change (`COUNT=5000`, `RUNS=3`):

| mode | avg latency | throughput |
|------|-------------|------------|
| interp | ~189 ms | ~26k msg/s |
| JIT | ~140 ms | ~36k msg/s |
| native AOT | ~173 ms | ~29k msg/s |

Numbers are hub/IPC-bound and machine-specific; re-run the script for your host.
