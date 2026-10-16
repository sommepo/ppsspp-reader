# STEINS;GATE (PSP)

5pb./Nitroplus, 2011. Disc **ULJM-05887**; `EBOOT.BIN` checksum `ABD66EFB`
(`reader.build_id`). Profile: `assets/reader/profiles/ULJM05887.lua`. It hooks nothing.

## What is read

- **The message window**: each line once it is complete and has been up for four frames,
  with the **speaker** (the game writes it in front of the line as 【name】, also where it
  shows ？？？) and **furigana**, and a cell for every character.
- **The phone's mails**, sent and received: what of a mail is on the phone's screen, with
  the other party as the speaker (`meta.mail` says `sent` or `received`), the subject as
  the first line when it is in view, the mail's text after it, and a cell for every
  character on the phone. **A mail longer than the screen is read again each time it comes
  to rest after scrolling**, as what is then in view: so that every word on the phone can be
  tapped, at the price of the rows that stayed in view being in the backlog twice.

## What is not read

- Everything else the phone shows (its menu, the lists of mails, the address book, calls),
  the @channel board, the TIPS list (the L button), the backlog, the menus.
- What the game's skip runs through.
- **The lines the game repeats while it waits for the phone to be taken out** are read the
  first time round only.

## How the game keeps its text

Unlike STEINS;GATE ELITE on the Switch, whose text is numbers of cells of a font, this
version keeps plain Shift-JIS, at fixed places of the program (it is not relocatable):

- `0x08f24b4c` points to the line being shown: 【name】 in front of what somebody says,
  `{word}[reading]` for furigana, commands that begin with a percent sign (`%K` wait for the
  button, `%P` a new page). The long dash is written with two codes that are box-drawing
  characters elsewhere (0x84a5, 0x84a7).
- `0x08c97474`: the lowest byte is 0 while the line is typed out and 1 when it is all
  there; the next byte is 1 while the game waits for the phone to be taken out (it then
  shows the same two lines in turn); the third is 1 for a line with a name. The profile
  takes the line's text at the frame the first byte becomes 1: **after the button is pressed
  the game fills the next line's text in at the same place, in pieces, before it starts
  typing it**, and a text read then is part of the next line.
- `0x08c97468` counts the lines, **but only up to 1,024** (the size of the game's own log)
  and tells nothing after that. The profile went by it at first and read nothing after the
  1,024th line; it does not use it now.
- `0x08c96a88` is 0x101 while the window is on screen: without 0x100 the player has hidden
  it (cross), without 1 the game's menu is up.
- **Where each character is drawn** is in two lists of records, at `0x0954760e` and
  `0x0954ce0e` (used in turn from line to line; the same places in every run looked at):
  88 bytes to a record, one for everything in the line as the game keeps it (the brackets
  of a word with a reading, the reading's characters and the commands have records too),
  the character's code first and its rectangle on the 480x272 screen at +64 (left, top,
  right, bottom, two bytes each). **The font is not of one width** (brackets, commas and
  some kana are narrower, a line holds 29 to 31 characters, lines are 24 pixels apart), so
  a grid puts a line's later characters up to two characters off: the profile did that at
  first, and the user saw a word's mark beside the word on the Thor. The profile matches
  the line's characters against both lists and takes the one that fits; a character's
  place is settled only when it comes up on screen, so the places are read again every
  few frames until they stop changing; a rectangle is cut where the next one begins. A
  line that fits neither list goes out on a grid of 16 pixels from (8, 204).
- A command is a percent sign and a letter (`%K`, `%P`, `%N`), some with digits, and some
  in pairs with a second letter: `%BS … %BE` around words in bold (the profile first let
  the S and the E through as text).
- `0x08a86d4c`, bit 8: the phone is on screen, taken out by the story or by the player
  (found by comparing memory with the phone out and away; it held in six saved places).
  `0x08ce29e8`, which the profile used first, stays 4 from the first mail on.
- The phone's screen is a grid of cells at `0x08ce3540`: 14 rows of 266 bytes, each four
  bytes and then 22 cells of 12: the character's Shift-JIS code (two bytes), its width in
  cells (two), what it is (one byte: 3 for a mail's own text, 1 for a heading's), and for
  a picture which one. A full-width character takes two cells. **Only what is on the
  phone's screen is in the grid**: a mail's rows move up through it as it is scrolled. A row
  that begins with a picture is a heading: 5 the screen's title (受信メール, 送信メール; the
  lists say …メールBOX), 0 a received mail's date, 1 its sender, 2 a sent mail's addressee,
  3 the subject. On screen a cell is 6.05 by 13.33 pixels from (321, 26).
- The mails' and the board's texts are also in the program as Shift-JIS strings (around
  `0x08a9....`); the profile does not use them.

## Buttons

Circle advances, cross hides the window, square is the menu, select auto mode, start skip
mode (it only runs through text that was read before), L the TIPS. **Triangle, held a
moment, takes the phone out** (a short press does nothing): in its menu circle chooses and
cross goes back, up and down scroll a mail. Where the story waits for the phone (a small
phone beside the line), circle does not advance.

Played by touch (the Fold), its column of buttons is `up down left right circle cross square
triangle`: left and right since 2026-10-16, for the questions whose はい and いいえ stand side by
side (loading a save: "このデータで開始しますか?", いいえ lit at first), which up and down do not
move.

## How it was checked

- **Harness** (the real game): from the prologue into chapter 1, some 1,350 lines pressed
  through, past the 1,024th (229 lines from the first mail on, read the same in two runs,
  none in parts, none twice); **the characters' places** drawn over screenshots for three
  lines once they were fully shown (three rows, a reading, narrow punctuation), all 95
  lines of four stretches placed from the game's lists; speakers on spoken lines, the two readings of the first line
  with furigana; cells drawn over screenshots for a two-line message and for the sent mail;
  nothing read while skip mode was on. The sent mail of the prologue (addressee, subject,
  text). **A received mail**: the one in the box when the story first waits for the phone
  (its text is not Japanese: rows of strokes and letters), read with its sender, read again
  after one row of scrolling, and nothing read from the phone's menu and the list of mails.
  The waiting lines read once in 400 frames.
- **Not looked at**: a received mail in Japanese and a reply (none had come by then), a
  mail with an empty line in it, the cells of a received mail over a screenshot, a line with
  half-width characters, lines of three rows, the window hidden and the menu over it with
  the profile loaded.
- **Tests**: none. **Device**: the profile as it was before these changes loaded on the Thor
  and a tap on a word of the first line opened the popup; the profile as it is now has not
  been on a device.
