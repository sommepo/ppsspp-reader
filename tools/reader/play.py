#!/usr/bin/env python3
# SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the Eden Reader fork)
# SPDX-License-Identifier: GPL-2.0-or-later
"""Drive the harness for a while and print the text events compactly.

    play.py STEPS [--button circle] [--gap 120] [--shots DIR] [--every 5]

Each step presses the button (or just runs when --button none), runs --gap
frames, then prints any new events as: frame [channel] speaker: text {meta}.
"""
import argparse
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def hc(*commands: str) -> str:
    return subprocess.run([sys.executable, os.path.join(HERE, "hc.py"), *commands],
                          capture_output=True, text=True).stdout


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("steps", type=int)
    ap.add_argument("--button", default="circle")
    ap.add_argument("--gap", type=int, default=120)
    ap.add_argument("--shots")
    ap.add_argument("--every", type=int, default=5)
    ap.add_argument("--meta", action="store_true")
    args = ap.parse_args()
    for step in range(args.steps):
        commands = []
        if args.button != "none":
            commands.append(f"press {args.button} 4 {args.gap}")
        else:
            commands.append(f"run {args.gap}")
        commands.append("events")
        if args.shots and step % args.every == 0:
            commands.append(f"shot {args.shots}/p{step:03d}.png")
        out = hc(*commands)
        for line in out.splitlines():
            if line.startswith("{"):
                e = json.loads(line)
                text = e["text"].rstrip("\n").replace("\n", " ⏎ ")
                extra = f"  {e['meta']}" if args.meta else ""
                print(f"{e['frame']:>6} [{e['channel']}] {e['speaker']}: {text}{extra}")
            elif line.startswith("ERR") or line.startswith("["):
                print(line)
        sys.stdout.flush()
    return 0


if __name__ == "__main__":
    sys.exit(main())
