#!/usr/bin/env python3
"""Minimal pipe hub protocol test against a running daemon."""

import os
import socket
import struct
import sys
import time

SOCK_PATH = os.environ.get("MUNX_PIPE_DIR", "/tmp/munx-pipes") + "/hub.sock"


def frame(body: bytes) -> bytes:
    return struct.pack("<I", len(body)) + body


def append_string(text: str) -> bytes:
    encoded = text.encode()
    return struct.pack("<I", len(encoded)) + encoded


def hello(pid: int) -> bytes:
    return bytes([1]) + struct.pack("<Q", pid)


def attach(channel: str, mode: int) -> bytes:
    return bytes([3]) + append_string(channel) + bytes([mode])


def publish(channel: str, payload: bytes) -> bytes:
    return bytes([5]) + append_string(channel) + struct.pack("<I", len(payload)) + payload


def read_frame(conn: socket.socket, timeout: float = 5.0) -> bytes:
    conn.settimeout(timeout)
    header = conn.recv(4)
    if len(header) < 4:
        raise RuntimeError("short header")
    (length,) = struct.unpack("<I", header)
    body = b""
    while len(body) < length:
        chunk = conn.recv(length - len(body))
        if not chunk:
            raise RuntimeError("short body")
        body += chunk
    return body


def main() -> int:
    sub = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sub.connect(SOCK_PATH)
    sub.sendall(frame(hello(os.getpid())))
    sub.sendall(frame(attach("pipehub.events", 2)))  # BroadcastIn
    print("subscriber attached", flush=True)

    pub = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    pub.connect(SOCK_PATH)
    pub.sendall(frame(hello(os.getpid())))
    pub.sendall(frame(attach("pipehub.events", 0)))  # Writer
    pub.sendall(frame(publish("pipehub.events", b"test-payload")))
    print("publisher sent publish", flush=True)

    deliver = read_frame(sub)
    print(f"subscriber got opcode {deliver[0]}", flush=True)
    ack = read_frame(pub)
    print(f"publisher got opcode {ack[0]}", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
