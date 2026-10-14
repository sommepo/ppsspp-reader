#!/usr/bin/env python3
# SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the Eden Reader fork)
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check a folder of Windows programs and libraries as Windows' loader would, as far as that
can be done without Windows: every function a file imports from a library that is in the
folder must be one that library exports.

    pe_check.py FOLDER

Windows refuses to start a program when one is missing ("The procedure entry point ... could
not be located"), which happened with the first package: the program was linked against the
compiler's own thread library and packaged with an older one of the same name. Libraries that
are not in the folder are Windows' own; they are listed and cannot be checked here.
Exit status 1 when anything is missing.
"""
import os
import struct
import sys


class Pe:
    """The imports and exports of a PE32+ (64-bit) or PE32 file."""

    def __init__(self, path: str):
        with open(path, "rb") as source:
            self.data = source.read()
        data = self.data
        if data[:2] != b"MZ":
            raise ValueError("not a Windows program")
        pe = struct.unpack_from("<I", data, 0x3C)[0]
        if data[pe:pe + 4] != b"PE\0\0":
            raise ValueError("not a Windows program")
        sections, optional_size = struct.unpack_from("<H", data, pe + 6)[0], \
            struct.unpack_from("<H", data, pe + 20)[0]
        optional = pe + 24
        magic = struct.unpack_from("<H", data, optional)[0]
        self.wide = magic == 0x20B
        directories = optional + (112 if self.wide else 96)
        self.export_dir = struct.unpack_from("<II", data, directories)
        self.import_dir = struct.unpack_from("<II", data, directories + 8)
        table = optional + optional_size
        self.sections = []
        for i in range(sections):
            virtual_size, virtual, raw_size, raw = struct.unpack_from("<IIII", data, table + 40 * i + 8)
            self.sections.append((virtual, max(virtual_size, raw_size), raw))

    def at(self, rva: int) -> int:
        for virtual, size, raw in self.sections:
            if virtual <= rva < virtual + size:
                return rva - virtual + raw
        raise ValueError(f"address {rva:#x} is in no section")

    def text(self, rva: int) -> str:
        start = self.at(rva)
        return self.data[start:self.data.index(b"\0", start)].decode("latin-1")

    def exports(self) -> set[str]:
        rva, size = self.export_dir
        if not rva:
            return set()
        count, = struct.unpack_from("<I", self.data, self.at(rva) + 24)
        names, = struct.unpack_from("<I", self.data, self.at(rva) + 32)
        base = self.at(names)
        return {self.text(struct.unpack_from("<I", self.data, base + 4 * i)[0]) for i in range(count)}

    def imports(self) -> dict[str, list[str]]:
        """Library name -> the functions imported from it by name."""
        rva, size = self.import_dir
        result: dict[str, list[str]] = {}
        if not rva:
            return result
        entry = self.at(rva)
        width = 8 if self.wide else 4
        by_number = 1 << (63 if self.wide else 31)
        while True:
            lookup, _, _, name, thunk = struct.unpack_from("<IIIII", self.data, entry)
            if not name:
                break
            functions = result.setdefault(self.text(name), [])
            position = self.at(lookup or thunk)
            while True:
                value, = struct.unpack_from("<Q" if self.wide else "<I", self.data, position)
                if not value:
                    break
                if not value & by_number:
                    functions.append(self.text(value + 2))  # after the hint
                position += width
            entry += 20
        return result


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    folder = sys.argv[1]
    files = {}
    for base, _, names in os.walk(folder):
        for name in names:
            if name.lower().endswith((".exe", ".dll")):
                files[os.path.join(base, name)] = Pe(os.path.join(base, name))
    # Windows does not tell upper case from lower in file names. Libraries are found beside
    # the program, which is where every library of the package is.
    here = {os.path.basename(path).lower(): pe.exports() for path, pe in files.items()
            if os.path.dirname(path) == folder.rstrip("/")}
    missing = 0
    functions = 0
    windows_own: set[str] = set()
    for path, pe in sorted(files.items()):
        for library, wanted in pe.imports().items():
            exported = here.get(library.lower())
            if exported is None:
                windows_own.add(library.lower())
                continue
            for function in wanted:
                functions += 1
                if function not in exported:
                    missing += 1
                    print(f"MISSING: {os.path.relpath(path, folder)} wants {function} from "
                          f"{library}, which does not have it", file=sys.stderr)
    print(f"{len(files)} files, {functions} functions imported from libraries of the package: "
          f"{missing} missing")
    print("left to Windows: " + " ".join(sorted(windows_own)))
    return 1 if missing else 0


if __name__ == "__main__":
    sys.exit(main())
