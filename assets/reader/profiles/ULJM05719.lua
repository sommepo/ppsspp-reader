-- reader: title D.C.I&II P.S.P. ～ダ・カーポI&II～ プラスシチュエーション ポータブル, disc 2: D.C.II P.S.
-- reader: reads The message window with speakers and furigana, and choices
-- reader: touch press
-- reader: version ULJM-05719
-- D.C.I&II P.S.P. (Kadokawa Shoten/CIRCUS, 2010), PSP, disc 2 (D.C.II P.S.), ULJM-05719.
-- EBOOT.BIN BA67308D. Disc 1 is ULJM05718.lua.
--
-- The game keeps the line of the message window as plain Shift-JIS at a fixed place, with the
-- speaker's name in front of it, and draws it on a grid of full-width cells: the place of
-- every character follows from the text. Nothing is hooked.
--
-- The two discs are one program built twice: this file and its twin are the same below
-- their first lines, and the places of each build are in this table.
--
-- state: the message window. The lowest byte says what it shows: 1 nothing (it is away),
--   2 the line, 4 an older line (the game's BACK TEXT). The next byte is 1 once the line is
--   typed out.
-- name: the speaker, 32 bytes; empty for narration.
-- line: the line, 256 bytes: rows end with a line feed, a word with a reading is written
--   ｛reading／word｝ (dots over a word are a reading of ・).
-- typed: how much of the line is typed out, two bytes: it begins anew with every line.
-- opacity: the window's, 255 while it is up, less while it fades and under the game's menu.
-- select: the choice. Its lowest byte is 2 while a choice is up; +0x48 points to the answers
--   (132 bytes each: a number, then the text, which begins with the ○ the game draws in
--   front), +0x66 is how many there are, +0x68 its opacity, and from +0x6c there is a
--   pointer for each answer to the part that draws it, which has its place at +0x40 (x and
--   y, from the first answer's).
local BUILDS = {
    ["95CE6F69"] = { disc = "disc 1 (D.C.P.S., ULJM-05718)", state = 0x08c8bd60,
                     name = 0x08c8bdc4, line = 0x08c8bde4, typed = 0x08c8c016,
                     opacity = 0x08c8c10f, select = 0x08c8c440 },
    ["BA67308D"] = { disc = "disc 2 (D.C.II P.S., ULJM-05719)", state = 0x08d23020,
                     name = 0x08d23084, line = 0x08d230a4, typed = 0x08d232d6,
                     opacity = 0x08d233cf, select = 0x08d23570 },
}
local build = BUILDS[reader.build_id]
if not build then
    error("this profile is for D.C.I&II P.S.P., ULJM-05718 and ULJM-05719 (EBOOT 95CE6F69 " ..
          "and BA67308D), the game is " .. tostring(reader.build_id))
end
local base = reader.base - 0x08804000 -- the executable is not relocatable: this is zero
local STATE = base + build.state
local NAME = base + build.name
local LINE = base + build.line
local TYPED = base + build.typed
local OPACITY = base + build.opacity
local SELECT = base + build.select
local SELECT_LIST, SELECT_COUNT, SELECT_OPACITY, SELECT_PARTS = 0x48, 0x66, 0x68, 0x6c
local SELECT_CURSOR = 0x65 -- the answer the cursor is on
local ANSWER, ANSWER_TEXT, PART_PLACE = 0x84, 4, 0x40

-- The line has to stand for this many frames: what a skip runs through is not read.
local SHOWN_FRAMES = 4

-- Where the game draws: the first character's cell and the step from one to the next, in
-- pixels of the 480x272 screen. A row holds 21 characters (22 with a mark that hangs over).
-- A row with a reading over it stands lower: the first by RUBY_FIRST, a later one (and all
-- rows after it) by RUBY_LATER.
local LEFT, TOP, STEP_X, STEP_Y = 20, 202, 18, 17
local RUBY_FIRST, RUBY_LATER = 5, 9
-- Characters of one byte come from the PSP's own font, each of its own width: about two
-- thirds of a cell, the thin ones a quarter. Their cells are near, not exact.
local NARROW, THIN = 12, 5
local THIN_ONES = " .,:;!'|Iijl"
local ROW_FULL = 20 -- a row of at least this many cells was broken by the game, not the writer

-- The first answer of a choice: its first character's cell (the ○ in front of it).
local ANSWER_LEFT, ANSWER_TOP = 74, 54

local OPEN, SLASH, CLOSE = "\x81\x6f", "\x81\x5e", "\x81\x70"
local IDEOGRAPHIC_SPACE = "\x81\x40"

-- Shift-JIS to a list of characters, each with its width in half cells.
local function characters(raw)
    local out = {}
    local i = 1
    while i <= #raw do
        local c = raw:byte(i)
        local double = (c >= 0x81 and c <= 0x9f) or (c >= 0xe0 and c <= 0xfc)
        local n = (double and i < #raw) and 2 or 1
        out[#out + 1] = { sjis = raw:sub(i, i + n - 1), width = n }
        i = i + n
    end
    return out
end

-- The line as text, its readings and a cell for every character. The cells are for a line
-- of the message window; `left` and `top` put them elsewhere.
local function compose(raw, left, top)
    left, top = left or LEFT, top or TOP
    local b = reader.builder()
    local ruby, cells = {}, {}
    local x, row = 0, 0
    local row_ruby = {} -- by row: true when a word in it has something over it
    local reading, word_start -- inside ｛reading／word｝
    local skip_indent = false
    for _, c in ipairs(characters(raw)) do
        if c.sjis == OPEN and not reading and not word_start then
            reading = {}
        elseif c.sjis == SLASH and reading and not word_start then
            word_start = b:length()
            row_ruby[row] = true
        elseif c.sjis == CLOSE and word_start then
            local kana = text.sjis(table.concat(reading))
            local dots = kana:gsub("・", "") == ""
            if b:length() > word_start and not dots then
                ruby[#ruby + 1] = { word_start, b:length() - word_start, kana }
            end
            reading, word_start = nil, nil
        elseif reading and not word_start then
            reading[#reading + 1] = c.sjis
        elseif c.sjis == "\n" then
            -- A row the game broke because it was full goes on in the next one, which then
            -- begins with a space under an opening bracket. (A line of narration begins
            -- with a space on disc 2: that one is left out as well.)
            if x >= ROW_FULL * STEP_X then
                skip_indent = true
            else
                b:add("\n")
                cells[#cells + 1] = false
                skip_indent = false
            end
            x, row = 0, row + 1
        elseif (skip_indent or b:length() == 0) and x == 0 and c.sjis == IDEOGRAPHIC_SPACE then
            x = x + STEP_X
        else
            skip_indent = false
            local width = STEP_X
            if c.width == 1 then
                width = THIN_ONES:find(c.sjis, 1, true) and THIN or NARROW
            end
            b:add(text.sjis(c.sjis))
            cells[#cells + 1] = { left + x, row, width, STEP_Y }
            x = x + width
        end
    end
    -- The rows' places, now that it is known which have readings.
    local tops = {}
    local y = top + (row_ruby[0] and RUBY_FIRST or 0)
    for r = 0, row do
        if r > 0 then y = y + STEP_Y + (row_ruby[r] and RUBY_LATER or 0) end
        tops[r] = y
    end
    for _, cell in ipairs(cells) do
        if cell then cell[2] = tops[cell[2]] end
    end
    local line = b:text()
    -- The line feed that ends the line is not text.
    while cells[#cells] == false do
        cells[#cells] = nil
        line = line:sub(1, -2)
    end
    return line, ruby, cells
end

local armed = true -- the line on screen has not been reported yet
local stood = 0    -- for how many frames it has been ready
local shown = false
local current      -- the reported line: { raw, name, cells }
local typed_before = 0

local function hide()
    if shown then
        reader.layout("message", nil)
        shown = false
    end
end

reader.on_frame(function()
    local state = mem.u32(STATE)
    local opacity = mem.u8(OPACITY)
    if not state or not opacity then return end
    local typed = state & 0x100 ~= 0
    local showing = state & 0xff
    -- A new line: the count of what is typed begins again, or the text is another. (A
    -- press on a line that is still being spoken takes the "typed out" away for a frame:
    -- that alone is no new line.)
    local count = mem.u16(TYPED) or 0
    if count < typed_before then armed = true end
    typed_before = count
    local raw = mem.cstr(LINE, 256) or ""
    local name = mem.cstr(NAME, 32) or ""
    if current and showing == 2 and (raw ~= current.raw or name ~= current.name) then
        armed = true
    end
    -- Under a choice the window says 1 and the line stays up; when the window is away it
    -- says 1 too, and the line is gone from memory.
    local up = opacity == 0xff and
               (showing == 2 or (showing == 1 and current ~= nil and raw == current.raw))
    if not (up and typed) then
        stood = 0
        hide()
        return
    end
    if armed then
        hide()
        stood = stood + 1
        if stood < SHOWN_FRAMES then return end
        armed = false
        stood = 0
        current = { raw = raw, name = name }
        local line, ruby, cells = compose(raw)
        if line == "" then
            current.cells = nil
            return
        end
        current.cells = cells
        local speaker = name ~= "" and text.sjis(name) or nil
        reader.emit{ text = line, speaker = speaker, ruby = ruby, window = "message" }
    end
    if not shown and current and current.cells then
        reader.layout("message", { space = { 480, 272 }, box = { 0, 196, 480, 76 },
                                   glyphs = current.cells })
        shown = true
    end
end)

-- The choice: one event, an answer to a line.
local choice_stood = 0
local choice_shown = false

local function read_choice()
    local list = mem.u32(SELECT + SELECT_LIST)
    local count = mem.u8(SELECT + SELECT_COUNT)
    if not list or not count or count == 0 or count > 8 then return nil end
    local texts, ruby, cells = {}, {}, {}
    local length = 0
    for i = 0, count - 1 do
        local raw = mem.cstr(list + i * ANSWER + ANSWER_TEXT, ANSWER - ANSWER_TEXT) or ""
        local part = mem.u32(SELECT + SELECT_PARTS + i * 4)
        local x = part and mem.s32(part + PART_PLACE) or 0
        local y = part and mem.s32(part + PART_PLACE + 4) or 0
        local line, spans, places = compose(raw, ANSWER_LEFT + x, ANSWER_TOP + y)
        -- The ○ in front is the game's mark, not the answer's text.
        local skip = 0
        if line:sub(1, 3) == "○" then
            line = line:sub(4)
            skip = 1
        end
        if line ~= "" then
            if #texts > 0 then
                cells[#cells + 1] = false
                length = length + 1
            end
            texts[#texts + 1] = line
            for _, span in ipairs(spans) do
                ruby[#ruby + 1] = { span[1] - skip + length, span[2], span[3] }
            end
            for n = 1 + skip, #places do cells[#cells + 1] = places[n] end
            length = length + text.len(line)
        end
    end
    if #texts == 0 then return nil end
    return table.concat(texts, "\n"), ruby, cells
end

reader.on_frame(function()
    local state = mem.u32(SELECT)
    local up = state and state & 0xff == 2 and mem.u8(SELECT + SELECT_OPACITY) == 0xff
    if not up then
        choice_stood = 0
        reader.cursor(nil)
        if choice_shown then
            reader.layout("choice", nil)
            choice_shown = false
        end
        return
    end
    if choice_shown then
        -- Which answer the cursor is on: what a tap on an answer is turned into presses with.
        local at = mem.u8(SELECT + SELECT_CURSOR)
        if at and at < 8 then reader.cursor(at) end
        return
    end
    choice_stood = choice_stood + 1
    if choice_stood < SHOWN_FRAMES then return end
    local answers, ruby, cells = read_choice()
    if not answers then return end
    reader.emit{ text = answers, ruby = ruby, channel = "choice", window = "choice" }
    reader.layout("choice", { space = { 480, 272 }, glyphs = cells })
    choice_shown = true
end)

reader.on_state_loaded(function()
    choice_stood = 0
    reader.cursor(nil)
    if choice_shown then
        reader.layout("choice", nil)
        choice_shown = false
    end
    hide()
    armed = true
    stood = 0
    current = nil
    typed_before = 0
end)
