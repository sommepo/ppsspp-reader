# PPSSPP Reader

Read Japanese PSP games like a visual novel in a browser with Yomitan: point at a word in the
game's own text and its dictionary entry opens. Make Anki cards, keep a backlog and reading
statistics.

For Windows and Android.

<table>
  <tr>
    <td width="50%"><img src="docs/reader/media/lookup-demo.gif" alt="Pointing at words in 428's text"><br><sub>Windows: point at a word</sub></td>
    <td width="50%"><img src="docs/reader/media/two-screens-demo.gif" alt="The lower screen moves the cursor over words in STEINS;GATE's text"><br><sub>Android, two screens: the lower one is a trackpad</sub></td>
  </tr>
</table>

> A fork of [PPSSPP](https://github.com/hrydgard/ppsspp) v1.20.4. It is not PPSSPP: please
> report nothing about it to the PPSSPP project. PPSSPP's README is
> [README.ppsspp.md](README.ppsspp.md). The reading layer is AI-written.

## Features

- **Look up words in the game's text.** On a computer, point at a word. On Android, move the
  cursor onto it with the controller or, on a two-screen handheld, the lower screen as a
  trackpad. Clicking or touching a word works too. Uses Yomitan dictionaries.
- **Anki cards** with the sentence and a picture of the scene.
- **Backlog and reading statistics.**
- **Save sync** between your devices.
- **The PSP's own XMB** as the main screen.
- **CRT picture** (optional, in Reader settings).

<table>
  <tr>
    <td width="50%"><img src="docs/reader/media/lookup.jpg" alt="A dictionary popup over 428"><br><sub>A lookup on Windows</sub></td>
    <td width="50%"><img src="docs/reader/media/two-screens.jpg" alt="A cursor lookup, the lower screen as trackpad"><br><sub>Cursor lookup on Android</sub></td>
  </tr>
  <tr>
    <td><img src="docs/reader/media/xmb.jpg" alt="The XMB-style main screen"><br><sub>The XMB</sub></td>
    <td><img src="docs/reader/media/crt.jpg" alt="STEINS;GATE with the CRT picture on"><br><sub>CRT picture</sub></td>
  </tr>
</table>

## Get it

From the [releases](../../releases):

- **Windows**: `PPSSPPReader-<version>-windows.zip`. Unpack it and start `PPSSPPQt.exe`.
- **Android**: `PPSSPPReader-<version>-android.apk` (arm64). Installs beside PPSSPP.

You need your own games.

## Game scripts

A game is read through a **game script**: a small Lua file that tells the app where the game
keeps its text. The apps install them from the list in this repository: **Game scripts** in
the XMB's Reader column (on Windows also **Reader → Game scripts…**). New games appear there
without an app update. You can also install a script from a file.

| Game | ID | Reads |
| --- | --- | --- |
| 428 ～封鎖された渋谷で～ | ULJS00219 | Story text, furigana, TIPS, choices |
| STEINS;GATE | ULJM05887 | Message window, speakers, furigana, phone mails |
| CHAOS;HEAD らぶChu☆Chu! | ULJM05821 | Message window, furigana |
| D.C.I&II P.S.P., disc 1 (D.C.P.S.) | ULJM05718 | Message window, speakers, furigana, choices |
| D.C.I&II P.S.P., disc 2 (D.C.II P.S.) | ULJM05719 | Message window, speakers, furigana, choices |

### Writing a script

A script holds addresses and decoding logic only, never text or data from the game.
[AGENTS.md](AGENTS.md), "Adding a game", explains how a game is run headless while a script is
written; each game's notes are in [docs/reader/games](docs/reader/games). Pull requests with
new scripts are welcome.

## Building

See [AGENTS.md](AGENTS.md).

## Licence

GPL-2.0-or-later, as PPSSPP ([LICENSE.TXT](LICENSE.TXT)).
