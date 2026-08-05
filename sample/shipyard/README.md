# shipyard

`shipyard` is an order-fulfillment pipeline and a third multi-package munx
application alongside `chatrelay` and `logscope`.

Unlike `logscope`, which *analyzes* log files, shipyard is built around **writing
a lot of structured logs** while it runs. Every stage — ingest, validation,
inventory reservation, routing, packing, shipping, and audit — emits trace,
debug, info, warn, and error lines through a shared async logger.

It demonstrates:

- a dedicated logging package with levels, correlation ids, and an async sink;
- verbose operator visibility (optional terminal mirroring with `--verbose`);
- CSV ingestion with header skipping and malformed-line recovery;
- lock-protected inventory with an audit trail on every mutation;
- enum-driven routing by order priority; and
- a final run summary on stdout plus a detailed log file.

## Input format

Each data row in the orders CSV has five comma-separated fields:

```text
ORDER_ID,SKU,QTY,CUSTOMER,PRIORITY
```

`PRIORITY` is one of `standard`, `express`, or `overnight`. Several rows in
`orders.csv` are intentionally invalid (unknown SKU, bad email, zero quantity,
insufficient stock) so rejection paths produce warn/error log lines.

Known SKUs: `WIDGET-A`, `WIDGET-B`, `GADGET-X`, `GADGET-Y`, `PART-Z`.

## Build

From the repository root:

```bash
./munxc sample/shipyard/main.mx
```

The resulting `sample/shipyard/main.mxb` bundles every transitive package.

## Run

```bash
./munxc --run sample/shipyard/main.mx sample/shipyard/orders.csv
./munxc --run sample/shipyard/main.mx sample/shipyard/orders.csv --log /tmp/shipyard.log
./munxc --run sample/shipyard/main.mx sample/shipyard/orders.csv --verbose --trace
```

Options:

| Flag | Effect |
|------|--------|
| `--log PATH` | Write the audit log to `PATH` (default `shipyard.log`) |
| `--verbose` | Mirror every emitted log line to the terminal |
| `--debug` | Emit `DEBUG` level and above |
| `--trace` | Emit `TRACE` level and above (very noisy) |

After a successful run you should see a short summary on stdout and a log file
with well over a hundred structured lines for the bundled `orders.csv`.

## Package layout

| Package | Role |
|---------|------|
| `types` | Domain enums and objects (`OrderLine`, `FulfillmentRecord`, …) |
| `logging` | Async structured logger (`log_info`, `log_warn`, …) |
| `catalog` | SKU catalog and validation helpers |
| `inventory` | Stock ledger with lock-protected reserve/release |
| `pipeline` | Ingest, validate, fulfill, and audit each order |
| `shipyard` | CLI entry point (`main.mx`) |
