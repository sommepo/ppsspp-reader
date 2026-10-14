# PPSSPP Reader: a guide for contributors and their coding agents

PPSSPP Reader is a fork of the PPSSPP PSP emulator that shows a game's text as real text: tap
or click a word in the game's own picture to look it up, make Anki cards, keep a backlog and
reading statistics, sync saves between devices. It is the PSP sibling of Eden Reader (Switch) and Azahar Reader (3DS) and
shares their reading layer. `README.md` is the users' front page.

**It reads a game only when it has a text profile for it**: a Lua script that knows where
that game keeps the text it shows. The work most worth doing here is another game's profile:
[Adding a game](#adding-a-game).

If a file `MAINTAINER.md` exists (the maintainer's own checkout), read it after this one: it
has their machines, devices and requests. It is not needed for contributing.

## Ground rules

- **Nothing goes to upstream PPSSPP from here.** The reading layer is AI-written. Do not open
  issues, pull requests or patches on `hrydgard/ppsspp` for it, and do not present this app
  as PPSSPP. Bugs found in the emulator itself are written down in the game's notes
  (`docs/reader/games/`).
- **No game data in Git, ever.** Game files, anything extracted or derived from them (code,
  scripts, text, fonts, tables made from a game's font), savestates, screenshots and harness
  output live under `private/`, which Git ignores. A bundled profile holds **addresses and
  decoding logic only**. Tests use made-up text. The one exception, by the maintainer's word:
  the README's pictures and animations in `docs/reader/media`.
- **Games are your own**, dumped from your own discs. Nothing here downloads them and nothing
  is to be added that does.
- **Upstream files are touched as little as possible.** The reader lives in directories of
  its own (below). Where an upstream file must change, the change is small and carries a
  comment that begins `Reader fork`, so that a new upstream release merges cleanly.
- **Bound what you run.** `tools/reader/limit.sh` runs a command under a memory limit (a
  systemd scope) and every script in `tools/reader/` uses it; compile jobs are `READER_JOBS`
  (8). Never run the Windows build under Wine as a test.
- **Say what was checked and how.** There are three levels of evidence and they are not the
  same: a test with made-up text, the headless harness on the real game, a real device. A
  game's notes say which lines of which level were looked at, and what was not.

## Layout

| Path | |
| --- | --- |
| `reader/core/` | The text engine: hooks, the Lua profiles' API, text events. It knows nothing of the emulator; `emulator.cpp` is the one file that ties it to PPSSPP, and `emulator.h` is all that upstream files include. |
| `assets/reader/profiles/` | **The game scripts** (profiles), one `<GAMEID>.lua` per game (`ULJS00219.lua`): built into the apps of one's own, and published as the list the apps install from ([Game scripts](#game-scripts)). |
| `reader/ui/` | The app's main screen in the manner of the PSP's own menu (`xmb.cpp`), built from the emulator's interface parts. |
| `reader/desktop/`, `reader/qt/` | The desktop app's reading layer: the logic (with tests) and what is on screen; `reader/qt/host.cpp` ties it to the emulator's Qt frontend. |
| `reader/save_sync/`, `reader/ui/save_sync_screen.*` | Save sync (`docs/reader/save-sync.md`): the Eden Reader fork's engine, what fits it to the PSP's save folders, and its page. Tests start services of their own on loopback ports; never point one at a service someone uses. |
| `reader/harness/` | The headless harness: runs a real game without display, GPU or sound and takes commands. How profiles are written. |
| `android/src/org/ppsspp/ppsspp/reader/` | The Android reading layer (Kotlin); tests in `android/test/`. `ReaderGlue`, `ReaderInput` and `ReaderNative` are what ties it to this emulator; `android/jni/reader_jni.cpp` is the native side. |
| `tools/reader/` | Build, test and harness tools. `env.sh` has every path. |
| `docs/reader/` | Guides: `games/` (one page per game: how it keeps its text, what is read, how it was checked), `save-sync.md`, `crt.md`, `icon.md`; `media/` has the README's pictures. |
| `reader/icon/` | The app's own icon (`docs/reader/icon.md`). |
| `private/` | Ignored by Git. Your games, harness folders, screenshots, notes. |

Branch `reader`, based on upstream tag `v1.20.4`. Submodules are fetched shallow
(`git submodule update --init --depth 1`, and once more inside `ext/armips` and the others
that have their own).

## Build and test

```sh
tools/reader/build-host.sh ppsspp-reader-harness   # the harness (build-host/reader/)
tools/reader/harness.sh --rom /path/to/game.iso &  # run a game in it; talk to it with hc.py
tools/reader/build-android.sh                      # the APK (android/build/outputs/apk/reader/optimized/)
tools/reader/test-android.sh                       # JVM tests of the Android reading layer
tools/reader/build-desktop.sh                      # the desktop app for Linux (build-desktop/PPSSPPQt); desktop.sh runs it without a display
tools/reader/test-desktop.sh                       # tests of the desktop reading logic
tools/reader/test-savesync.sh                      # tests of save sync, against a service it starts itself
tools/reader/build-windows.sh                      # the desktop app for Windows, cross-compiled
tools/reader/package-windows.sh                    # build-windows/package/PPSSPPReader-windows.zip
```

They run on Linux (x86-64). `tools/reader/env.sh` names every path the scripts use, each a
variable that can be set; a file `private/env.sh` is read first, for a machine's own
settings. What is needed: a GCC 13 or newer with cmake and ninja, OpenGL and SDL2
development files (a conda environment with `libgl-devel libegl-devel libgles-devel mesalib
sdl2` is enough: `READER_MESA_PREFIX`), and for the APK a JDK 17+ and the Android SDK.

## Adding a game

What a profile is, in one paragraph: a Lua script that, each frame or from a hook in the
game's code, reads the text the game is showing out of the game's memory and reports it
(`reader.emit`), with furigana and, where it can be had, the place of every character on
screen, so that a tap finds its word. `reader/core/lua_api.cpp` is the API (every function a
profile can call, with its comment), `assets/reader/profiles/ULJS00219.lua` (428) the example,
`docs/reader/games/*.md` how each game was done, and `tools/reader/hc.py` with
`reader/harness/harness.cpp` the harness's commands.

In short, for a PSP game:

1. `tools/reader/harness.sh --rom private/<game>/game.iso &`, then
   `tools/reader/hc.py "run 600" "shot private/<game>/shots/a.png" modules`. The PSP has
   savestates: `save PATH` at every place worth coming back to, `load PATH` to return.
2. Press through to a line of text (`press circle`), take a screenshot, `findtext` what it
   shows (Shift-JIS is tried first). Most PSP games draw text one character at a time
   through one routine: find it (`disasm`, hooks from a probe profile loaded with
   `profile PATH`) and what it keeps of what it drew.
3. A PSP game's executable is loaded at the same address every time and a hook costs
   little: hooks are fine. Bind the profile to the build (`reader.build_id`, a checksum of
   the executable as the disc has it).
4. What a good profile does: **each line once**, when it is on screen; **nothing while it is
   not readable** (under menus, what a skip runs through); furigana as the game gives them;
   choices as a choice; **where each character is** (`reader.layout`, in the units of the
   480x272 screen), checked with `tools/reader/layout_check.py` over a screenshot; **only
   what was asked for**.
5. The profile begins with the lines the apps read: `-- reader: title`, `-- reader: reads`,
   `-- reader: version`, and `-- reader: audio never` for a game without voices.
6. Notes in `docs/reader/games/<game>.md`: what is read, what is not and why, how the game
   keeps its text, how it was checked at which level.

## Game scripts

The scripts the apps read games with can come from **the published list**:
`assets/reader/profiles/index.json` in the public repository, made with the scripts by
`tools/reader/scripts-index.py` when the public repository is (a maintainer's step; the list is
not kept in Git here). *Game scripts* (the XMB's Reader column; on a computer also *Reader →
Game scripts…*) fetches it, shows each game of the list or of the app as *Not installed*,
*Installed*, *Update available*, *Your own script* or *Needs a newer app* (`-- reader: api N`
above what the app has), and installs, updates and removes scripts in the user's
`reader/profiles` folder (which wins over a bundled script); every file is checked against
the list's size and SHA-256 before it is written, and `.installed.json` there records what came
from the list. *Install from a file* takes `<GAMEID>.lua` and `<GAMEID>.<name>` files of one's
own. A published build comes with no scripts: `-DREADER_BUNDLE_PROFILES=OFF` (desktop),
`-PreaderBundleScripts=false` (Android), `READER_PACKAGE_SCRIPTS=0` (the Windows package). The
desktop app fetches with Qt's network access (Windows' own TLS in the package), the Android app
on a thread of its own. `reader/desktop/script_index.*` and
`android/src/org/ppsspp/ppsspp/reader/ScriptStore.kt` are the same rules in C++ and Kotlin, and
the same as in the PS2 reader forks: a change to the list's format goes to both, and to
`scripts-index.py`.
**Checked**: tests of both with made-up scripts, and the Android screen with a stand-in list
(off the device). **Not tried**: the desktop window with the published list, a device.

## The controller's back buttons

`reader/qt/reader_pad` (the same file in every reader fork) reads the buttons no game uses, beside
the emulator: on Linux any controller the system offers that has the "extra" buttons
(`BTN_TRIGGER_HAPPY1` to `5`: what the Steam Controller bridge on a Batocera box makes of the
four grips and the "..." button); on Windows the Steam Controller's (2026) own reports, read
from the device beside whoever else reads it, nothing written. Out of the box: grip 1
**pauses the game** with its picture up and every word still to be looked up, and lets it go
on (Ctrl+G; nothing is said on screen); grip 2 pauses the statistics (Ctrl+T); grip 3 makes a
card of the sentence; grip 4 opens the backlog; "..." the reader's menu. *Reader settings,
Back buttons* has a row for each. The development channel's `back-button N` presses one.

Checked (2026-10-10): on a PC running Batocera, a stand-in controller that sends the bridge's
own reports pressed grips 1 and 2 twice each in a running game: the frame count stood still
and went on again, the statistics' mark came and went. **Not checked**: a real controller in
a hand; grips 3 to 5 there; anything of the Windows part, which has never run.


## The Steam Controller without Steam (Windows)

To Windows the Steam Controller (2026) is no gamepad by itself: left alone it plays keyboard
and mouse. While Steam is not running, `reader/qt/reader_steam` reads the controller's own reports,
switches that emulation off, and offers a gamepad the emulator finds like any other (made
inside the program through SDL's virtual joystick: no driver), the right pad as the mouse
pointer (a trackball; pressed, a click), the left pad as the wheel (pressed, the right
button). `reader/desktop/steam_bridge` has what the reports mean and is the Batocera box's bridge
(`sc_bridge.py`) in C++. The grips stay the reader's (above). With Steam running nothing is
touched. The development channel's `steam-report HEX` plays a report on any system.

Checked (2026-10-10): tests of the reports' meaning with made-up reports; on Linux without a
display a made-up report made the gamepad in the running app. **Never run**: everything
that is Windows only (finding and reading the controller, switching its emulation off, the
mouse), and whether this emulator takes the new gamepad for player one without being told.

## The CRT picture

*Reader settings, CRT picture* shows the game as a cathode-ray tube would: scan lines, a
phosphor pattern, bulging glass, glow, a warm white, each with a slider, and six ready-made
looks. **On from the first start here** (a strong picture; the switch turns it off). It is one shader and one table of sliders that every reader
fork carries (`reader/core/crt.*`, `crt_shader.glsl`); both settings pages, on a computer and
on Android, are built from the table. **`docs/reader/crt.md` has all of it**: what each
slider does, how a slider is added, where it is tied to this emulator, and what was checked
how. The shared files are changed in the Flycast Reader fork and copied here by its `tools/reader/crt.py sync`: do not edit them here.

## Writing here

Guides and notes are written for the reader who was not there: whole sentences, what
something is before how it was found, and no claim of "works" without saying where it was
seen working. Commit messages say what changed and how it was checked.

## The second display's trackpad (2026-10-13)

On a device with two displays (an AYN Thor) the display the game is not on is the cursor's
trackpad **for as long as a game with a text profile runs, without a button**
(`HoverCursor.always`): drawn in the console's look (`ConsoleLook.ground` under a veil, the
console's pointer under the finger), the word *Trackpad* for a few seconds. The cursor is
moved by the finger alone there and shows on the game while it is used and four seconds
after. **Nothing of the controller is taken**: the cursor's button, the sticks and the d-pad
(which a controller reports beside the sticks, and which the reader swallowed with them
while the trackpad was held up) stay the game's. On one display everything is as it was:
the button holds the cursor, the sticks steer it. The same code in the PPSSPP, ARMSX2,
Flycast and Eden Reader forks; Azahar Reader is left out (the 3DS has a bottom screen of its
own there). Checked with `CursorModeTest` off the device, and a picture of the pad looked at
(`READER_PREVIEW_DIR`, `trackpad.png`). **On a device** (AYN Thor, 2026-10-13, ARMSX2 Reader
with ペルソナ4, touches sent over adb to the lower display): the pad came up by itself with
the first text, a stroke on it moved the cursor on the game, the cursor on 選択 opened its
entry, the console's pointer showed under the finger. **Not tried on a device**: a real
controller's d-pad and sticks while the pad is up (the fault this mends, covered by the test
only), and the other three apps.
