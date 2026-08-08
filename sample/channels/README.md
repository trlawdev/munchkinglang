# channels — bidirectional peer demo

`channel(id)` opens a hub-backed bidirectional link between **exactly two**
processes (see comments in `sample/sample.mx`). Before sending, the writer
offers; the hub grants Accept when the peer is attached (or has announced
`<=:`). Simultaneous offers collide (`Busy`) and both peers back off 1–10 ms.

| Syntax | Meaning |
|--------|---------|
| `ch = channel("id")` | Join / create the named channel (max 2 peers) |
| `value :=> ch` | Offer → Accept|Busy, then publish payload |
| `value = <=: ch` | RecvReady, then wait for Deliver |

## Run (VM / JIT)

Terminal 1:

```bash
./munxc --run sample/channels/peer_a.mx
```

Terminal 2:

```bash
./munxc --run sample/channels/peer_b.mx "hello from B"
```

## Native AOT

```bash
./munxc --native -o /tmp/peer_a sample/channels/peer_a.mx
./munxc --native -o /tmp/peer_b sample/channels/peer_b.mx
# start a hub (or let the first peer auto-spawn one)
./munxc --pipe-hub &
/tmp/peer_a &
/tmp/peer_b "hello from B"
```

VM and native peers can interoperate on the same hub.
