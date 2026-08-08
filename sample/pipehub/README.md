# pipehub — multi-VM pub/sub demo

Demonstrates the munx pipe hub coordinating named pipes across separate VM
(or native) processes.

## Queue vs subscribe

| Mode | API | Delivery |
|------|-----|----------|
| Work queue | `pipe(name, in)` | Each message goes to one reader |
| Pub/sub | `pipe(name, subscribe)` | Every subscriber receives a copy |
| Writer | `pipe(name, out)` | Creates the pipe and publishes |

## Publisher-first + pending queue

A reader (`in` / `subscribe`) **cannot attach** until at least one writer has
opened `pipe(name, out)` for that pipe id. If a writer publishes while no
readers are attached, payloads are queued under that pipe id (bounded) and
flushed when a reader later attaches.

## Run

Terminal 1 (publisher — must start first and stay up briefly):

```bash
./munxc --run sample/pipehub/publisher.mx "hello room"
```

Terminal 2 (optional second subscriber):

```bash
./munxc --run sample/pipehub/subscriber.mx
```

Each subscriber prints `received: hello room`.

## Environment

- `$MUNX_PIPE_DIR` — directory for `hub.sock`, `hub.lock`, and legacy FIFOs
- `$MUNX_PIPE_HUB=0` — disable the hub and use direct POSIX FIFOs
