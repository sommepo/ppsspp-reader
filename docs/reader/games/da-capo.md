# D.C.I&II P.S.P. ～ダ・カーポI&II～ プラスシチュエーション ポータブル (PSP)

Kadokawa Shoten/CIRCUS, 2010. Two discs, each a game of its own with its own save:

| Disc | Game | Disc ID | `EBOOT.BIN` (`reader.build_id`) | Profile |
| --- | --- | --- | --- | --- |
| 1 | D.C.P.S. | **ULJM-05718** | `95CE6F69` | `assets/reader/profiles/ULJM05718.lua` |
| 2 | D.C.II P.S. | **ULJM-05719** | `BA67308D` | `assets/reader/profiles/ULJM05719.lua` |

The two are one program built twice. **The two profiles are the same file below the lines
that name the disc**, with a table of each build's addresses at the top; a change goes into
both (a test of the desktop reading logic compares them). They hook nothing.

## What is read

- **The message window**: each line once it is typed out and has stood for four frames,
  with the **speaker** (the name plate over the window; narration has none) and
  **furigana**, and a cell for every character.
- **Choices**: the answers as one event of the channel `choice`, an answer to a line, with a
  cell for every character. The line that asks stays on screen under them and stays
  tappable.

How the text is made from what the game holds:

- A row the game broke because it was full (20 characters or more) goes on without a break;
  the space the game puts at the start of the next row under an opening bracket is left
  out. A shorter row keeps its line break (the three dots one under another that mark a
  pause are `・\n・\n・`).
- The space every line of narration begins with on disc 2 is left out.
- Dots over a word for emphasis are written by the game as a reading made of `・`: those
  are no reading and are left out.
- The `○` the game draws in front of each answer is not part of the answer.

## What is not read

- The game's BACK TEXT (older lines, shown in the window in another colour), the menu, the
  save and load screens, the date card between days, EXTRA, CONFIG, NAME.
- What the game's skip runs through. The line it stops on is read.
- Anything of the map or of other screens later in the games, if there are any: nothing
  but lines and choices was met in what was played (below).

## How the game keeps its text

Everything is at fixed places (the program is not relocatable), in a row of objects of the
game's interface that all begin alike; the message window's and the choice's are two of
them. Disc 1's addresses, disc 2's in brackets:

- **The message window**, `0x08c8bd60` (`0x08d23020`). Its first byte says what the window
  shows: 1 nothing, 2 the line, 4 an older line (BACK TEXT). The second byte is 1 once the
  line is typed out. **Under a choice the byte is 1 as well, with the line still up**: what
  tells that from a window that is away is that the line is then still in memory.
  - `+0x64`, 32 bytes: the speaker, Shift-JIS, empty for narration.
  - `+0x84`, 256 bytes: the line, Shift-JIS. Rows end with a line feed. A word with a
    reading is `｛reading／word｝`.
  - `+0x184`, 256 bytes: the same line without the readings. During BACK TEXT the older
    line is put here, and the line at `+0x84` stays.
  - `+0x284`: four entries of twelve bytes for the readings (a pointer to the kana, how
    many, the column, the row, the word's length). Not used: the line has it all.
  - `+0x2b6`, two bytes: how much of the line is typed out. It begins anew with every
    line, which is how the profile knows a new line from the same text shown twice. **A
    press on a line that is still being spoken clears the "typed out" byte for one frame
    and sets it again**: the profile first took that for a new line and read every voiced
    line twice.
  - `+0x3af`: the window's opacity, 255 while it is up. It is less while the window fades
    and 0 under the game's menu (which changes nothing else here).
- **The choice**, `0x08c8c440` (`0x08d23570`): the first byte is 2 while a choice is up.
  `+0x48` points to the answers (132 bytes each: a number, then the text), `+0x66` is how
  many, `+0x65` the one the cursor is on, `+0x68` the opacity, and from `+0x6c` there is a
  pointer for each answer to the part that draws it, with its place at `+0x40` (x, y; the
  answers are 42 pixels apart).
- **The game's log** (what BACK TEXT shows) is a ring of entries of 292 bytes around
  `0x08c51310` on disc 1: the speaker, then the line with its readings. Not used. A line
  put into an entry there is laid out by the game when BACK TEXT shows it: that is how the
  places below were measured, with made-up lines.

**Where the characters are** follows from the text, and was measured in screenshots: a
full-width character is a cell of 18 pixels, the first at x 20; rows are 17 pixels apart,
the first at y 202. **A row with a reading over it stands lower**: the first row by 5
pixels, any later row by 9 (and every row after it with it). A word does not get wider for a
long reading. An answer's first character is at (74, 54).

**Characters of one byte** (Latin letters, digits) are drawn with the PSP's own font
(`sceLibFont`), each of its own width: a digit about 12 pixels, `I` 4, a space 5. The
profile gives them 12, and 5 for the thin ones, so their cells and those after them in the
row are near, not exact. **None was met in the games' own text**, which writes its digits
and letters full-width; the widths were measured with made-up lines only.

## Buttons

Circle advances (on a line still being spoken the first press ends the typing, the second
goes on), square and L advance too, R is BACK TEXT (up and down inside it, cross leaves),
cross is the menu (SKIP, BACK TEXT, DATA SAVE, DATA LOAD, TITLE, CONFIG, QUICK SAVE, QUICK
LOAD, SCENARIO) and cross again closes it or ends a skip.

At its first start a disc makes its system save through the PSP's own save screen
(circle, circle, then cross on "save completed"; in the harness that screen runs on a real
clock and wants a second between presses).

## How it was checked

- **Harness** (the real games, each from a new game, on 2026-10-12), circle pressed through
  with the profile loaded and every event kept:
  - **Disc 1**: 2,237 lines from the first line on with an earlier state of the profile
    (no line twice, none in parts), then 1,461 lines again from the first choice with the
    profile as it is: 24 speakers, 43 lines with readings (every reading inside its line),
    2 choices (of three and of two answers).
  - **Disc 2**: 1,587 lines from the first line: 23 speakers, 28 lines with readings, 1
    choice (three answers).
  - The same line twice running came up four times on disc 1 (a silence said twice, the
    three dots): the game shows those twice.
  - **Cells drawn over screenshots** (`tools/reader/layout_check.py`) and looked at: on
    disc 1 a line of one row with a reading, lines of two rows with a reading in the
    first and in the second, a line of two rows whose second begins under a bracket, a
    line of three rows; all three choices; on disc 2 one line.
  - **The rows' places** for every arrangement of readings over three rows, and that a
    long reading does not widen its word: made-up lines put into the game's log on disc 1
    and shown by BACK TEXT, measured in the screenshots.
  - On both discs: the cells go away under the menu and in BACK TEXT and come back
    without the line being read again; a skip of 600 frames (from the menu) reads
    nothing, and the line it is stopped on is read; a state loaded on a line reads it.
- **The desktop app** (the Linux build on a display-less compositor, disc 1, from a new
  game): the bundled profile loads, the first two lines are read, and a click on 桜 in the
  second marks that word and opens the popup with its entry. Nothing else of it was tried
  with this game.
- **Not looked at**: a choice of four or more answers, or one with a reading in an
  answer; an answer too long for its box; characters of one byte in the games' own text; anything past the first
  days of either game; the opening films (the harness shows them black on disc 2).

- **Tests**: with made-up text, none of the profile itself (this fork has no tests of
  profiles yet). The catalog tests know the two IDs, and the desktop one holds the two
  files to the same body.
- **Device**: both discs and the app with these profiles are on an AYN Thor (2026-10-12)
  and the app lists them; **neither was started there**. Nothing on Windows; disc 2 only in
  the harness.
