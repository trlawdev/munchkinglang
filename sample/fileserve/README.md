# fileserve

Real HTTP/1.1 static file server written in Munx. Point it at a directory and
open it in a browser or with `curl` — suitable for local previews, sharing
build output on a LAN, or serving downloads.

## Features

- TCP accept loop with `GET` / `HEAD`
- `index.html` for `/` and extensionless directory paths
- MIME types for common web assets
- Path traversal protection (`..`, encoded dots, encoded slashes)
- Optional Combined Log Format access log (`--log`)
- `HTTP/1.1` responses with `Content-Length` and `Connection: close`

## Build

From the repository root:

```bash
./munxc sample/fileserve/main.mx
```

## Run

Prefer the bytecode interpreter for this server (the JIT currently has stability
issues under bursty socket load):

```bash
# demo site shipped with the project
./munxc --run --interp sample/fileserve/main.mxb -- \
  --root sample/fileserve/www --host 127.0.0.1 --port 8080

# convenience wrapper (same defaults)
./sample/fileserve/run.sh

# your own folder + access log
./munxc --run --interp sample/fileserve/main.mxb -- \
  --root ./dist --port 3000 --host 127.0.0.1 --log /tmp/fileserve.access.log
```

Then:

```bash
curl -i http://127.0.0.1:8080/
curl -i http://127.0.0.1:8080/about.html
curl -i http://127.0.0.1:8080/data/hello.json
```

## Options

| Flag | Default | Meaning |
|------|---------|---------|
| `--root DIR` | `.` | Document root |
| `--host ADDR` | `0.0.0.0` | Bind address |
| `--port N` | `8080` | Listen port |
| `--log PATH` | (off) | Access log file |
| `--help` | | Show usage |

## Package layout

| Package | Role |
|---------|------|
| `types` | IO enums + `HttpRequest` / `HttpResponse` |
| `pathutil` | Safe path join, extension, query strip |
| `mime` | Content-Type by extension |
| `http` | Request parse + response framing |
| `fs` | Document-root file loading |
| `accesslog` | Combined Log Format writer |
| `server` | Accept loop + connection handler |
| `fileserve` | CLI entry (`main.mx`) |

## Notes

- Only `GET`/`HEAD` are implemented (enough for static hosting).
- There is no directory listing; place an `index.html` in folders you want browsable.
- Bind to `127.0.0.1` if you do not intend LAN exposure.
- Use `--interp` (or `MUNX_VM_JIT=0`) until JIT socket workloads are hardened.
