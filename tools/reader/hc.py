#!/usr/bin/env python3
# SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the Eden Reader fork)
# SPDX-License-Identifier: GPL-2.0-or-later
"""Send commands to a running ppsspp-reader-harness and print its reply.

    hc.py [--socket PATH] "run 60" "shot out.png"      # one command per argument
    hc.py [--socket PATH] < commands.txt

The socket defaults to $READER_HARNESS_SOCKET or private/harness/sock.
Exit status is 1 when any command answered ERR.
"""
import os
import socket
import sys


def send(path: str, commands: list[str]) -> str:
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
        s.connect(path)
        s.sendall(("\n".join(commands) + "\n").encode())
        s.shutdown(socket.SHUT_WR)
        chunks = []
        while True:
            data = s.recv(65536)
            if not data:
                break
            chunks.append(data)
    return b"".join(chunks).decode("utf-8", "replace")


def main() -> int:
    args = sys.argv[1:]
    path = os.environ.get("READER_HARNESS_SOCKET", "private/harness/sock")
    if args[:1] == ["--socket"]:
        path = args[1]
        args = args[2:]
    commands = args if args else [line.rstrip("\n") for line in sys.stdin]
    reply = send(path, commands)
    sys.stdout.write(reply)
    return 1 if any(line.startswith("ERR") for line in reply.splitlines()) else 0


if __name__ == "__main__":
    sys.exit(main())
