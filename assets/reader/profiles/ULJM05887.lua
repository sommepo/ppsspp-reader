-- reader: no hooks
-- reader: title STEINS;GATE (PSP)
-- reader: reads The message window with speakers and furigana, and the phone's mails
-- reader: version ULJM-05887
-- STEINS;GATE (5pb./Nitroplus, PSP, 2011), ULJM-05887. EBOOT.BIN ABD66EFB.
--
-- The game keeps the line it is showing as Shift-JIS text with a little markup, at a fixed
-- place: 【name】 in front of what somebody says, {word}[reading] for furigana, and commands
-- that begin with a percent sign (%K wait for the button, %P new page, ...). The window
-- lays it out on a grid, so the place of every character follows from the text.

local BUILD = "ABD66EFB"
if reader.build_id ~= BUILD then
    error("this profile is for STEINS;GATE ULJM-05887 (EBOOT " .. BUILD .. "), the game is " ..
          tostring(reader.build_id))
end

-- Data of the program (it is not relocatable).
local MESSAGE_TEXT = 0x08f24b4c   -- pointer to the line being shown
local MESSAGE_STATE = 0x08c97474  -- lowest byte: 0 while the line is typed out, 1 when it is all there;
                                  -- the next byte is 1 while the game waits for the phone to be taken out
-- (0x08c97468 counts the lines, but only as far as the game's own log holds them: it stops
-- at 1,024 and tells nothing after that.)
local WINDOW = 0x08c96a88         -- 0x101 while the message window is on screen: 0x100 the player
                                  -- has not hidden it, 1 the game's own screen is up (not its menu)

-- The window's grid, in pixels of the 480x272 screen.
local LEFT, TOP = 8, 204
local CELL = 16      -- a full-width character; a half-width one takes half
local LINE = 25      -- from one line's top to the next
local COLUMNS = 29   -- full-width characters to a line
local RUBY = 8       -- a reading's characters, above their word

-- A line is reported when it has been complete for this many frames: what a skip runs
-- through is not read.
local SHOWN_FRAMES = 4
-- How many frames more a line waits for the places the game drew its characters at.
local DRAWN_WAIT = 20

local function sjis_chars(raw)
    -- Splits Shift-JIS bytes into characters (one or two bytes each).
    local out, i = {}, 1
    while i <= #raw do
        local b = raw:byte(i)
        local n = ((b >= 0x81 and b <= 0x9f) or (b >= 0xe0 and b <= 0xfc)) and 2 or 1
        out[#out + 1] = raw:sub(i, i + n - 1)
        i = i + n
    end
    return out
end

-- The game writes its long dash with two codes that are box-drawing characters elsewhere.
local DASH = { ["\x84\xa5"] = "―", ["\x84\xa7"] = "―" }
local OPEN_NAME, CLOSE_NAME = "\x81\x79", "\x81\x7a" -- 【 】

-- Takes the markup out of a line. Returns the speaker, and the pieces of the body: each a
-- character with how wide it is (2 full, 1 half), or a ruby group.
local function parse(raw)
    local speaker = ""
    if raw:sub(1, 2) == OPEN_NAME then
        local close = raw:find(CLOSE_NAME, 3, true)
        if close then
            speaker = text.sjis(raw:sub(3, close - 1))
            raw = raw:sub(close + 2)
        end
    end
    local chars = sjis_chars(raw)
    local body = {} -- { text = utf8, width = 1|2, reading = utf8 or nil, first/last of a ruby word }
    local i = 1
    local word -- index in body where a {word} began
    while i <= #chars do
        local c = chars[i]
        if c == "%" then
            -- A command: the percent sign and the letters and digits after it.
            i = i + 1
            while i <= #chars and chars[i]:match("^[%w]$") do
                local was = chars[i]
                i = i + 1
                -- %K, %P, %N and their like are one letter; a few take digits after it, and
                -- some come in pairs with a second letter (%BS ... %BE around words in bold).
                if was:match("^%a$") then
                    if not was:match("^[KPN]$") and i <= #chars and chars[i]:match("^[A-Z]$") then
                        i = i + 1
                    end
                    while i <= #chars and chars[i]:match("^%d$") do i = i + 1 end
                    break
                end
            end
        elseif c == "{" then
            word = #body + 1
            i = i + 1
        elseif c == "}" then
            i = i + 1
            if chars[i] == "[" then
                local reading = {}
                i = i + 1
                while i <= #chars and chars[i] ~= "]" do
                    reading[#reading + 1] = chars[i]
                    i = i + 1
                end
                i = i + 1
                if word and word <= #body then
                    body[word].ruby_length = #body - word + 1
                    body[word].reading = text.sjis(table.concat(reading))
                end
            end
            word = nil
        elseif c == "\n" or c == "\r" then
            body[#body + 1] = { newline = true }
            i = i + 1
        else
            body[#body + 1] = { text = DASH[c] or text.sjis(c), width = #c == 2 and 2 or 1, sjis = c }
            i = i + 1
        end
    end
    return speaker, body
end

-- Where the game drew each character of the line: a list of records, one to a character in
-- the order they are drawn, each with the character's code and its rectangle on the 480x272
-- screen. The font is not of one width (brackets, commas and some kana are narrower), so the
-- places cannot be worked out from a grid. The lists are at the same places in every run
-- looked at; what they hold is checked against the line's own characters, and a line that
-- neither matches is laid out on the grid instead.
local DRAWN = { 0x0954760e, 0x0954ce0e } -- two lists, used in turn from line to line
local DRAWN_RECORD = 88 -- the character's code first, its rectangle (left, top, right, bottom) at +64
local DRAWN_SKIP = 24   -- records that are not the line's own characters (a reading's) lie between

local function drawn_in(list, body)
    local wanted = 0
    for _, piece in ipairs(body) do
        if not piece.newline then wanted = wanted + 1 end
    end
    if wanted == 0 then return nil end
    -- The list has a record for everything in the line as the game keeps it: the brackets
    -- of a word with a reading, the reading's own characters, the commands.
    local count = wanted * 2 + 60
    local raw = mem.read(list, count * DRAWN_RECORD)
    if #raw < count * DRAWN_RECORD then return nil end
    local cells, record, matched = {}, 0, 0
    local last -- the cell before, for a character the list does not have as itself
    for _, piece in ipairs(body) do
        if piece.newline then
            cells[#cells + 1] = false
        else
            local found
            for skip = 0, DRAWN_SKIP do
                local at = (record + skip) * DRAWN_RECORD
                if record + skip >= count then break end
                if raw:sub(at + 1, at + #piece.sjis) == piece.sjis then
                    found = record + skip
                    break
                end
            end
            local cell
            if found then
                local left, top, right, bottom = string.unpack("<I2I2I2I2", raw, found * DRAWN_RECORD + 65)
                if right > left and bottom > top and right <= 480 and bottom <= 272 then
                    cell = { left, top, right - left, bottom - top }
                    record = found + 1
                    matched = matched + 1
                end
            end
            if not cell then
                -- Beside the one before it, on the same line.
                if not last then return nil end
                cell = { last[1] + last[3], last[2], piece.width * CELL / 2, last[4] }
            end
            cells[#cells + 1] = cell
            last = cell
        end
    end
    -- Mostly the line's own characters, or it is another line's list.
    if matched * 10 < wanted * 8 then return nil end
    -- A character's rectangle is as wide as the font's square, which is wider than the step
    -- to the next character: each cell ends where the next one on its row begins.
    for i = 1, #cells - 1 do
        local cell, after = cells[i], cells[i + 1]
        if cell and after and after[2] == cell[2] and after[1] > cell[1] and after[1] < cell[1] + cell[3] then
            cell[3] = after[1] - cell[1]
        end
    end
    return cells
end

local function drawn_cells(body)
    for _, list in ipairs(DRAWN) do
        local cells = drawn_in(list, body)
        if cells then return cells end
    end
    return nil
end

-- The text, its ruby spans and a cell for every character: where the game drew it, or, when
-- that cannot be had, laid out on a grid as the window roughly does.
local function compose(body)
    local b = reader.builder()
    local ruby, cells = {}, {}
    local column, row = 0, 0 -- in half-width units
    for index, piece in ipairs(body) do
        if piece.newline then
            b:add("\n")
            cells[#cells + 1] = false
            column, row = 0, row + 1
        else
            if column + piece.width > COLUMNS * 2 then
                column, row = 0, row + 1
            end
            if piece.reading then
                ruby[#ruby + 1] = { b:length(), piece.ruby_length, piece.reading }
            end
            b:add(piece.text)
            cells[#cells + 1] = { LEFT + column * CELL / 2, TOP + row * LINE,
                                  piece.width * CELL / 2, CELL }
            column = column + piece.width
        end
    end
    return b:text(), ruby, cells
end

local complete_since    -- the frame the line's text was first all there
local captured          -- the line's bytes as they were at that frame
local reported = false
local shown = false
local cells
local settling          -- the line whose characters' places are still being looked at
-- While the game waits for the phone to be taken out it shows the same few lines over and
-- over: each is reported the first time round only.
local waiting_seen = {}

local function withdraw()
    if shown then
        reader.layout("message", nil)
        shown = false
    end
end

reader.on_frame(function(frame)
    local state = mem.u32(MESSAGE_STATE)
    local window = mem.u32(WINDOW)
    if not state or not window then return end
    local visible = (window & 0x101) == 0x101
    if (state & 0xff) ~= 1 then
        -- The next line is being typed out: the one before is gone.
        complete_since = nil
        captured = nil
        reported = false
        cells = nil
        settling = nil
        withdraw()
        return
    end
    if not complete_since then
        complete_since = frame
        -- Taken now and not when it is reported: once the button is pressed the game begins
        -- to put the next line's text here, in pieces, before it starts typing it.
        local pointer = mem.u32(MESSAGE_TEXT)
        captured = pointer and pointer ~= 0 and mem.cstr(pointer, 2048) or ""
    end
    if not reported and visible and frame - complete_since >= SHOWN_FRAMES then
        local raw = captured or ""
        local again = false
        if (state & 0xff00) ~= 0 then
            again = waiting_seen[raw] == true
        else
            waiting_seen = {}
        end
        if #raw == 0 or again then
            reported = true
        else
            local speaker, body = parse(raw)
            local line, ruby, grid = compose(body)
            -- The game's own places for the characters are there a few frames after the line
            -- is: the line waits that long for them, and goes out on the grid otherwise.
            local drawn = line ~= "" and drawn_cells(body) or nil
            if line == "" then
                reported = true
            elseif drawn or frame - complete_since >= SHOWN_FRAMES + DRAWN_WAIT then
                reported = true
                waiting_seen[raw] = (state & 0xff00) ~= 0 or nil
                cells = drawn or grid
                -- The characters come up one after another, and the game settles a
                -- character's place when it comes up: the places are looked at again
                -- until they have stopped changing.
                settling = { body = body, frame = frame, same = 0, key = "" }
                reader.emit{ text = line, speaker = speaker, ruby = ruby, window = "message",
                             id = text.hash(raw) }
            end
        end
    end
    if settling and reported and (frame - settling.frame) % 3 == 0 then
        local drawn = drawn_cells(settling.body)
        local parts = {}
        for i, cell in ipairs(drawn or {}) do
            parts[i] = cell and (cell[1] .. "," .. cell[2] .. "," .. cell[3]) or "-"
        end
        local key = table.concat(parts, " ")
        if drawn and key ~= settling.key then
            settling.key = key
            settling.same = 0
            cells = drawn
            withdraw()
        else
            settling.same = settling.same + 1
            if settling.same > 40 then settling = nil end
        end
    end
    if cells then
        if visible and not shown then
            reader.layout("message", { space = { 480, 272 }, box = { 0, 196, 480, 76 },
                                       glyphs = cells })
            shown = true
        elseif not visible then
            withdraw()
        end
    end
end)

-- ---- The phone ----
--
-- What the phone's screen shows is a grid of cells, 22 to a row and 14 rows: a full-width
-- character takes two cells. Only what is on the phone's screen is in it: a long mail's rows
-- move up through it as the player scrolls. A row that begins with one of the phone's little
-- pictures is a heading: the screen's title, a mail's date, sender, addressee or subject.
local PHONE_OUT = 0x08a86d4c  -- bit 8: the phone is on screen (taken out by the story or the player)
local PHONE_GRID = 0x08ce3540
local PHONE_ROW = 266         -- bytes: four, 22 cells of 12, and two less
local PHONE_COLUMNS = 22
local PHONE_ROWS = 14
local PHONE_LEFT, PHONE_TOP = 321, 26
local PHONE_CELL_W, PHONE_CELL_H = 6.05, 13.33
-- A cell: the character (two bytes), its width in cells (two), what it is (one), and for a
-- picture which one.
local KIND_PICTURE, KIND_TEXT = 0, 3
local ICON_DATE, ICON_FROM, ICON_TO, ICON_SUBJECT, ICON_TITLE = 0, 1, 2, 3, 5
-- A mail is reported when the screen has been the same for this many frames (it has stopped
-- scrolling).
local PHONE_FRAMES = 30

local MAIL = "\x83\x81\x81\x5b\x83\x8b" -- メール
local RECEIVED = "\x8e\xf3\x90\x4d" -- 受信

-- The rows of the grid: for each its characters (with column, width and kind), the picture it
-- begins with if any, and its text as bytes.
local function phone_rows()
    local raw = mem.read(PHONE_GRID, PHONE_ROW * PHONE_ROWS)
    if #raw < PHONE_ROW * PHONE_ROWS then return nil end
    local rows = {}
    for r = 0, PHONE_ROWS - 1 do
        local row = { chars = {} }
        local col = 0
        while col < PHONE_COLUMNS do
            local at = r * PHONE_ROW + col * 12 + 5
            local b0, b1 = raw:byte(at), raw:byte(at + 1)
            local width, kind = raw:byte(at + 2), raw:byte(at + 4)
            if col == 0 and b0 == 0 and b1 == 0 and width == 2 and kind == KIND_PICTURE then
                row.icon = raw:byte(at + 5)
                col = col + 2
            elseif b0 >= 0x81 and b1 ~= 0 then
                row.chars[#row.chars + 1] = { sjis = string.char(b0, b1), col = col, width = 2, kind = kind }
                col = col + 2
            elseif b0 >= 0x20 and b0 < 0x7f and b1 == 0 then
                row.chars[#row.chars + 1] = { sjis = string.char(b0), col = col, width = 1, kind = kind }
                col = col + 1
            else
                col = col + 1
            end
        end
        local parts = {}
        for _, c in ipairs(row.chars) do parts[#parts + 1] = c.sjis end
        row.text = table.concat(parts)
        rows[r + 1] = row
    end
    return rows
end

local phone_seen, phone_since, phone_reported
local phone_shown = false
local mail_party  -- who the mail on screen is from or to, kept while its text is scrolled

local function phone_withdraw()
    if phone_shown then
        reader.layout("phone", nil)
        phone_shown = false
    end
end

reader.on_frame(function(frame)
    local out = mem.u32(PHONE_OUT)
    if not out or (out & 0x100) == 0 then
        phone_seen, phone_reported, mail_party = nil, nil, nil
        phone_withdraw()
        return
    end
    local rows = phone_rows()
    if not rows then return end
    -- What the screen says, whatever else its cells hold (they change while it is drawn).
    local said = {}
    for r, row in ipairs(rows) do said[r] = (row.icon or "") .. row.text end
    local raw = table.concat(said, "\n")
    if raw ~= phone_seen then
        phone_seen = raw
        phone_since = frame
        phone_withdraw()
        return
    end
    if phone_reported == raw or frame - phone_since < PHONE_FRAMES then return end
    phone_reported = raw
    -- A mail: the screen's title says so (the lists of mails say "…メールBOX").
    local title = rows[1]
    if title.icon ~= ICON_TITLE or not title.text:find(MAIL, 1, true) or title.text:find("BOX", 1, true) then
        mail_party = nil
        return
    end
    local kind = title.text:find(RECEIVED, 1, true) and "received" or "sent"
    local b = reader.builder()
    local cells = {}
    local function add(r, chars)
        for _, c in ipairs(chars) do
            b:add(text.sjis(c.sjis))
            cells[#cells + 1] = { PHONE_LEFT + c.col * PHONE_CELL_W, PHONE_TOP + (r - 1) * PHONE_CELL_H,
                                  c.width * PHONE_CELL_W, PHONE_CELL_H - 1 }
        end
    end
    local function newline()
        if b:length() > 0 then
            b:add("\n")
            cells[#cells + 1] = false
        end
    end
    local blank = false
    local body = false
    for r = 2, PHONE_ROWS do
        local row = rows[r]
        if row.icon == ICON_FROM or row.icon == ICON_TO then
            if row.text ~= "" then mail_party = text.sjis(row.text) end
        elseif row.icon == ICON_SUBJECT then
            if row.text ~= "" then
                add(r, row.chars)
                blank = true -- the text begins on a line of its own
            end
        elseif not row.icon then
            -- The mail's own text: its rows are the phone wrapping it, so they are one text,
            -- and an empty row is where the mail has an empty line.
            local chars = {}
            for _, c in ipairs(row.chars) do
                if c.kind == KIND_TEXT then chars[#chars + 1] = c end
            end
            if #chars == 0 then
                blank = body or blank
            else
                if blank then newline() end
                blank = false
                body = true
                add(r, chars)
            end
        end
    end
    local line = b:text()
    if line == "" or not body then return end
    reader.emit{ text = line, speaker = mail_party or "", window = "phone",
                 meta = { kind = "mail", mail = kind } }
    reader.layout("phone", { space = { 480, 272 }, glyphs = cells })
    phone_shown = true
end)

reader.on_state_loaded(function()
    complete_since = nil
    captured = nil
    reported = false
    waiting_seen = {}
    settling = nil
    cells = nil
    shown = false
    phone_seen, phone_reported, mail_party = nil, nil, nil
    phone_shown = false
end)
