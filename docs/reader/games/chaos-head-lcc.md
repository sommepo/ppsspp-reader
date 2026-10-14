# CHAOS;HEAD らぶChu☆Chu! (PSP)

5pb./Nitroplus, 2011. Disc **ULJM-05821**; `EBOOT.BIN` checksum `667BCE2C`
(`reader.build_id`). Profile: `assets/reader/profiles/ULJM05821.lua`. It hooks nothing.

## What is read

- **The message window and the lines the game shows in the middle of the screen**: each line
  once it has stood for four frames, with **furigana** and a cell for every character where
  the game drew it.

## What is not read

- **The speaker.** The name over the window is not in what the profile reads, and the line's
  own text is nowhere in memory as plain text (below), so a line that somebody says comes
  without a name.
- Choices and the delusion triggers (none was met), the TIPS, the backlog, the menus.
- What the game's skip runs through.
- **The line on screen right after a savestate is loaded**: which of the two lists is the
  one on screen is told by which changed last, and after a load neither has. The next line
  is read again.

## How the game keeps its text

The engine is the one STEINS;GATE for the PSP has ([its notes](steins-gate.md)), with one
difference that decides everything: **the line being shown is not in memory as Shift-JIS**
(a search for a line's characters in every usual encoding, and for its pattern of repeated
characters at any spacing, finds nothing but the lists below).

- **Two lists of what was drawn**, at `0x090b0c8e` and `0x090b648e`, used in turn from line
  to line: 88 bytes to a record, one for everything in the line: a character (its
  Shift-JIS code first, its rectangle on the 480x272 screen at +64: left, top, right,
  bottom), the brackets of a word with a reading (`{`, `}`, `[`, `]`) and the reading's
  characters between the square ones, and the commands (a percent sign and a letter: `%N` a
  new line, `%K` wait for the button, `%P` the end of the line, others for where the line
  stands). **A line is the records up to its `%P`**; what lies after is an older line's
  rest. A character's place is settled when it comes up on screen, so the profile sends the
  places again whenever they change.
- `0x08c9bbad` is 0 while the message is on screen and not 0 when the player has hidden the
  window (cross) or the menu (square) is up: found by comparing memory in those states.
- The list that changed last holds the line on screen. A variable that says which was
  looked for (eight lines, the program's data and the heap compared) and not found.

## Buttons

Circle advances, cross hides the window, square is the menu (with Tips on L).

## How it was checked

- **Harness** (the real game, from a new game): 118 lines read while pressing through the
  first scenes, none twice, none in parts; four lines with readings; the cells drawn over a
  screenshot of a line with two readings; nothing read with the window hidden.
- **Not looked at**: choices and delusion triggers, lines of three rows, a line with
  half-width characters, the middle-of-the-screen lines' cells over a screenshot.
- **Tests**: none. **Device**: not yet.
