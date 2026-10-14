#!/usr/bin/env python3
# SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the Eden Reader fork)
# SPDX-License-Identifier: GPL-2.0-or-later
"""Send commands to the desktop app started with tools/reader/desktop.sh and print its replies.

    dc.py [--socket PATH] status "click 400 520" "grab private/desktop/shots/a.png"
    dc.py [--socket PATH] < commands.txt

One command per argument (or per line of standard input). The socket defaults to
$READER_DESKTOP_CONTROL or private/desktop/sock. Exit status is 1 when any command
answered ERR. The commands are in src/yuzu/reader/reader_control.cpp.
"""
import os
import socket
import sys


def send(path: str, commands: list[str]) -> str:
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
        s.connect(path)
        s.sendall(("\n".join(commands) + "\n").encode())
        chunks = []
        answered = 0
        # One line comes back per command.
        while answered < len(commands):
            data = s.recv(1 << 20)
            if not data:
                break
            chunks.append(data)
            answered += data.count(b"\n")
    return b"".join(chunks).decode("utf-8", "replace")


def main() -> int:
    args = sys.argv[1:]
    path = os.environ.get("READER_DESKTOP_CONTROL", "private/desktop/sock")
    if args[:1] == ["--socket"]:
        path = args[1]
        args = args[2:]
    commands = args if args else [line.rstrip("\n") for line in sys.stdin if line.strip()]
    reply = send(path, commands)
    sys.stdout.write(reply)
    return 1 if any(line.startswith("ERR") for line in reply.splitlines()) else 0


if __name__ == "__main__":
    sys.exit(main())
