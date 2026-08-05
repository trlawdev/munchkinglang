# pipehub — multi-VM pub/sub demo

Demonstrates the munx pipe hub coordinating named pipes across separate VM
processes.

## Queue vs subscribe

| Mode | API | Delivery |
|------|-----|----------|
| Work queue | `pipe(name, in)` | Each message goes to one reader |
| Pub/sub | `pipe(name, subscribe)` | Every subscriber receives a copy |
| Writer | `pipe(name, out)` | Publishes to the channel |

The hub starts automatically on the first `--run` that uses pipes and exits
when the last VM disconnects.

## Run

Terminal 1:

```bash
./munxc --run sample/pipehub/subscriber.mx
```

Terminal 2 (optional second subscriber):

```bash
./munxc --run sample/pipehub/subscriber.mx
```

Terminal 3:

```bash
./munxc --run sample/pipehub/publisher.mx "hello room"
```

Each subscriber prints `received: hello room`.

## Environment

- `$MUNX_PIPE_DIR` — directory for `hub.sock`, `hub.lock`, and legacy FIFOs
- `$MUNX_PIPE_HUB=0` — disable the hub and use direct POSIX FIFOs
