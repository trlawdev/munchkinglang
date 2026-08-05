# logscope

`logscope` is a concurrent access-log analyzer and a second multi-package munx
application alongside `chatrelay`.

It demonstrates:

- streaming file input;
- a fixed parser worker pool;
- pipe-based work queues and completion sentinels;
- structured parsing with error recovery;
- lock-protected shared statistics;
- thread joining; and
- terminal or file report output.

Each input line has this whitespace-delimited format:

```text
METHOD PATH STATUS BYTES DURATION_MS
```

Compile the project from the repository root:

```bash
./munxc sample/logscope/main.mx
```

The resulting `sample/logscope/main.mxb` contains the entry package and all
transitive package dependencies.

Intended runtime usage once the VM is available:

```bash
munx-run sample/logscope/main.mxb -- sample/logscope/access.log
munx-run sample/logscope/main.mxb -- sample/logscope/access.log --output report.txt
```
