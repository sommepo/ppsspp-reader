#!/usr/bin/env python3
# SPDX-FileCopyrightText: PPSSPP Reader fork contributors
# SPDX-License-Identifier: GPL-2.0-or-later
"""Make headers that name the Windows headers the way the sources do.

    win_headers.py MINGW_INCLUDE_DIR OUT_DIR SOURCE_DIR...

The emulator's sources include <Windows.h>, <WinSock2.h>, <ShlObj.h>: on Windows any
spelling finds the file, on Linux only MinGW's own (all lower case) does. For every include
that exists in MinGW under another spelling, a one-line header of the sources' spelling is
written to OUT_DIR, which the Windows build puts on its include path.
"""
import os
import re
import sys


def main() -> int:
    mingw, out = sys.argv[1], sys.argv[2]
    known = {}
    for base, _, files in os.walk(mingw):
        for name in files:
            rel = os.path.relpath(os.path.join(base, name), mingw)
            known.setdefault(rel.lower(), rel)
    wanted = set()
    pattern = re.compile(rb'^\s*#\s*include\s*[<"]([^>"]+)[>"]', re.M)
    for source in sys.argv[3:]:
        for base, dirs, files in os.walk(source):
            dirs[:] = [d for d in dirs if not d.startswith(('.', 'build-'))]
            for name in files:
                if not name.endswith(('.c', '.cc', '.cpp', '.h', '.hpp', '.inc', '.m', '.mm')):
                    continue
                try:
                    data = open(os.path.join(base, name), 'rb').read()
                except OSError:
                    continue
                for match in pattern.finditer(data):
                    wanted.add(match.group(1).decode('utf-8', 'replace').replace('\\', '/'))
    made = 0
    for name in sorted(wanted):
        real = known.get(name.lower())
        if real is None or real == name:
            continue
        path = os.path.join(out, name)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, 'w') as f:
            f.write(f'#include <{real}>\n')
        made += 1
    print(f'{made} header(s) in {out}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
