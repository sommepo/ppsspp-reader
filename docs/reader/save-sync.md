# Save sync

Keeps each game's save the same on every device PPSSPP Reader runs on: play on the computer,
go on where you stopped on a handheld, and back. It is in the Android app and in the desktop
app. **One computer of yours keeps the saves** and the other devices sync with it; nothing
leaves your home and Tailscale networks, and there is no account anywhere.

It is Eden Reader's save sync, the same code on the apps' side and **the same service**: the
standalone service of the Eden Reader fork (`tools/reader/savesync/server.py` there), or a
computer on which Eden Reader's desktop app keeps the saves. One service keeps the saves of
both apps. That fork's `docs/reader/save-sync.md` describes the service, its versions of each
save, how to install it and its protocol; this page says what is particular to the PSP.

## Setting it up

In the app's main screen: **Reader, Save sync** (in PPSSPP's own main screen: the *Save
sync* entry beside *Reader*).

- **Address of the service**: `host`, `host:port` (8642 when no port is given), several with
  commas between them: they are tried in turn, so a Tailscale name and a home address can
  both be given.
- **Code**: the service's (`server.py code` prints it).
- **This device's name**: what the other devices' messages call this one.
- **Sync saves** switches it on. *Check the connection* says whether the address and the
  code are right; *Sync every game now* brings every game with a save here in line.

## What happens when

| When | What the app does |
| --- | --- |
| A game starts | Asks the service before the game runs. If another device saved since this one last did, its save is fetched. If this device has the newer save, it is sent. If both changed, see below. |
| The game saves | The save is sent a moment (1.5 seconds of quiet) after the game wrote it, without a word. |
| The game stops | The save is sent once more if it changed. |
| The app starts | Every game that has a save here is brought in line in the background, where that needs no question. |
| The service cannot be reached | The game starts with this device's save (after two and a half seconds of waiting for each address), and you are told once. The save goes up at the next save or start. |

**No save is replaced without being kept**: the service keeps versions, and a save that
another one takes the place of here is moved to `PSP/SYSTEM/reader-savesync/backups/<title>/`
on the memory stick (the last ten for each game).

### When two devices both played

That happens when a device played without reaching the service and another played the same
game before the first came back. Nothing is overwritten: the game starts with this device's
save, a message says so, and nothing of that game is sent until you choose. The Save sync
page then lists the game with when each save was made and two entries: *Use the other
device's save* (this one's goes to the backups and stays on the service as a version of its
own; not while the game runs) and *Use this device's save* (it becomes the current one for
all devices).

## What a PSP game's save is

A PSP game keeps its save in as many folders of `PSP/SAVEDATA` as it cares to make, each
named with the game's ID and whatever the game adds (`ULJS00219DATA01`, `ULJS00219SYSTEM`).
A game's save is **every folder whose name begins with its ID**; a folder that is deleted on
one device goes on the others too (into their backups). Folders of homebrew, whose names are
not a disc's ID, are not synced, and neither are savestates.

The service names games by 16 hexadecimal digits (a Switch title ID). A PSP game's is made
from its ID: the codes of the four letters, three zeros, the five digits. `ULJM05887` is
`554C4A4D00005887`. The service's `list` shows the game's name beside it.

Where the memory stick is a folder that only Android's file picker reaches (a document
tree), save sync is not available and its page says so. The app's own folder
(`Android/data/…/files`), which is what a new install uses, is fine.

## Reading statistics

With save sync on, the desktop app sends this install's reading statistics to the service
when a game stops, as Eden Reader does (app `ppsspp-reader`). The Android app sends them to
the address and code under *Reader settings, Statistics options* when those are entered, and
to save sync's otherwise.

## Where the code is

| Path | |
| --- | --- |
| `reader/save_sync/archive.*`, `remote.*`, `engine.*` | From the Eden Reader fork: a save as an archive with its fingerprint, the service's protocol, and the engine that decides. Two additions for the PSP: a game's folders are asked for each time (`Game::locate`), and a folder that only the other device has gets a place (`Game::place`). SHA-256 is the emulator's own. |
| `reader/save_sync/host.*` | The PSP's side, free of the emulator: which folders are a game's, the settings, the open conflicts, what the user is told. |
| `reader/ui/save_sync_screen.*` | The page, made of PPSSPP's own interface parts, and the start with the app. |
| `reader/core/emulator.cpp` | The three calls from the emulator: the game is loaded (before it runs), it wrote a save (`Core/Dialog/SavedataParam.cpp`), it stopped. |
| `ext/reader-httplib`, `ext/reader-json` | cpp-httplib and nlohmann's JSON, single headers (MIT). Plain HTTP only. |
| `reader/save_sync/tests/`, `tools/reader/test-savesync.sh` | The Eden fork's tests of the archive, the decision and the engine, and those of the PSP's side (`psp.cpp`), against a service the script starts for the purpose. |

A game that writes its save with plain file calls instead of the PSP's save dialog is not
noticed while it runs: its save goes up when it stops and at the next start.

## How it was checked

- **Tests** (`tools/reader/test-savesync.sh`, 31 cases): with made-up saves against two
  services started on loopback ports with folders of their own.
- **The desktop app without a display** (Linux build), 428 and a service started for the
  purpose: the existing save went up at the app's start; with the save folders removed, the
  save was fetched before the game ran and was the same files; the game's autosave made a
  second version on the service; the page's entries, a conflict listed and answered with
  *Use this device's save*.
- **On a device** (AYN Thor, the same throwaway service through `adb reverse`): the page,
  the address and the code typed in Android's input screen, both games' saves sent when
  *Sync saves* was switched on, and 428 started from the main screen: the save the desktop
  app had made current was fetched before the game ran ("took the save made on …"), the
  files were the same as the desktop's, the replaced save was in the backups.
- **Not checked**: anything on Windows; the statistics going up from either app (the
  Android side's choice of address has a test with a stand-in); a save made by playing on the device going up from
  there; *Use the other device's save*, the service away, and a game without the PSP's save
  dialog, outside the tests.
