#!/usr/bin/env python3
"""Publish one message to the pipe hub (subscriber must already be attached)."""

import os
import socket
import struct
import sys

SOCK_PATH = os.environ.get("MUNX_PIPE_DIR", "/tmp/munx-pipes") + "/hub.sock"


def frame(body: bytes) -> bytes:
    return struct.pack("<I", len(body)) + body


def append_string(text: str) -> bytes:
    encoded = text.encode()
    return struct.pack("<I", len(encoded)) + encoded


def main() -> int:
    pub = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    pub.connect(SOCK_PATH)
    pub.sendall(frame(bytes([1]) + struct.pack("<Q", os.getpid())))
    pub.sendall(frame(bytes([3]) + append_string("pipehub.events") + bytes([0])))
    pub.sendall(frame(bytes([5]) + append_string("pipehub.events") + struct.pack("<I", 0)))
    ack = pub.recv(4096)
    print(f"ack bytes: {len(ack)}", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
