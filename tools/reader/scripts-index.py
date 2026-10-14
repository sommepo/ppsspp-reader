#!/usr/bin/env python3
"""Make the list of game scripts that the apps install from (Reader, Game scripts).

    tools/reader/scripts-index.py FOLDER [--check] [--system NAME]

Reads every <ID>.lua in FOLDER (assets/reader/profiles) and writes FOLDER/index.json: for each script
the game's ID, what its first lines say of it (title, reads, version, api), and the name, size
and SHA-256 of the script and of every data file it asks for with reader.data("name")
(<ID>.name beside it). A script whose data file is not in the folder (a character table made
from the game, which is not published) is left out of the list and named on standard error.
With --check nothing is written: the list is printed. --system names the console (psp).

The apps fetch the list from the published repository and each file from beside it, and check
every file against its size and SHA-256 before installing it (reader/desktop/script_index.* and
android/src/org/ppsspp/ppsspp/reader/ScriptStore.kt). The format is theirs: change both with it.
"""
import hashlib
import json
import re
import sys
from pathlib import Path

FORMAT = 1
NAME = re.compile(r"^([A-Z]{4}-?[0-9]{5}|[0-9A-F]{16})\.lua$")
HEADER = re.compile(r"^--\s*reader:\s*(\S+)\s+(.*?)\s*$")
DATA = re.compile(r"""reader\.data\(\s*["']([A-Za-z0-9_.-]+)["']\s*\)""")


def header(source):
    """The lines "-- reader: <key> <value>" among the comments a script begins with."""
    said = {}
    for raw in source.splitlines():
        line = raw.strip()
        if not line:
            continue
        if not line.startswith("--"):
            break
        match = HEADER.match(line)
        if match:
            said.setdefault(match.group(1), match.group(2))
    return said


def file_entry(path):
    data = path.read_bytes()
    return {"file": path.name, "size": len(data), "sha256": hashlib.sha256(data).hexdigest()}


def make(folder, system):
    scripts, left_out = [], []
    for path in sorted(folder.glob("*.lua")):
        match = NAME.match(path.name)
        if not match:
            continue
        game = match.group(1)
        source = path.read_text(encoding="utf-8")
        said = header(source)
        data, missing = [], []
        for name in sorted(set(DATA.findall(source))):
            data_path = folder / f"{game}.{name}"
            (data.append(file_entry(data_path)) if data_path.is_file() else missing.append(data_path.name))
        if missing:
            left_out.append((game, missing))
            continue
        entry = {
            "id": game,
            "title": said.get("title", game),
            "reads": said.get("reads", ""),
            "version": said.get("version", ""),
            "api": int(said.get("api", "1")),
            **file_entry(path),
        }
        if data:
            entry["data"] = data
        scripts.append(entry)
    return {"format": FORMAT, "system": system, "scripts": scripts}, left_out


def main(argv):
    if not argv or argv[0].startswith("-"):
        print(__doc__, file=sys.stderr)
        return 2
    folder = Path(argv[0])
    system = argv[argv.index("--system") + 1] if "--system" in argv else "psp"
    index, left_out = make(folder, system)
    for game, missing in left_out:
        print(f"left out {game}: {', '.join(missing)} not in {folder}", file=sys.stderr)
    text = json.dumps(index, ensure_ascii=False, indent=2) + "\n"
    if "--check" in argv[1:]:
        sys.stdout.write(text)
    else:
        (folder / "index.json").write_text(text, encoding="utf-8")
        print(f"{folder / 'index.json'}: {len(index['scripts'])} scripts", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
