-- reader: no hooks
-- reader: title CHAOS;HEAD らぶChu☆Chu! (Chaos;Head Love Chu Chu!)
-- reader: reads The message window and the lines in the middle of the screen, with furigana
-- reader: version ULJM-05821
-- CHAOS;HEAD らぶChu☆Chu! (5pb./Nitroplus, PSP, 2011), ULJM-05821. EBOOT.BIN 667BCE2C.
--
-- The game's engine is the one STEINS;GATE for the PSP has (ULJM05887.lua), but here the
-- line being shown is nowhere as plain text: what there is are the two lists of what the
-- game drew, one record for everything in the line (each character with the rectangle it
-- was drawn in, the brackets of a word with a reading, the reading's characters, the
-- commands). A line is read from them.

local BUILD = "667BCE2C"
if reader.build_id ~= BUILD then
    error("this profile is for CHAOS;HEAD Love Chu Chu ULJM-05821 (EBOOT " .. BUILD ..
          "), the game is " .. tostring(reader.build_id))
end

-- Data of the program (it is not relocatable).
local LISTS = { 0x090b0c8e, 0x090b648e } -- the lists of drawn characters, used in turn from line to line
local RECORD = 88        -- bytes: the character's code first, its rectangle (left, top, right, bottom) at +64
local MOST = 240         -- records looked at: a line ends at its %P long before
local COVERED = 0x08c9bbad -- 0 while the message is on screen; not 0 hidden by the player or under the menu

-- A line is reported when its list has been the same for this many frames: what a skip runs
-- through is not read.
local SHOWN_FRAMES = 4

-- The game writes its long dash with two codes that are box-drawing characters elsewhere.
local DASH = { ["\x84\xa5"] = "―", ["\x84\xa7"] = "―" }

-- What a list holds up to the end of its line: the records' codes as one string (which says
-- whether it is the same line), and the records themselves.
local function read_list(address)
    local raw = mem.read(address, MOST * RECORD)
    if #raw < MOST * RECORD then return nil end
    local tokens, codes = {}, {}
    for n = 0, MOST - 1 do
        local at = n * RECORD
        local b0, b1 = raw:byte(at + 1), raw:byte(at + 2)
        if b0 == 0xff and b1 == 0xff then return nil end -- a list without an end is no line
        local token = { at = at }
        if b0 == 0x25 then -- a command: the percent sign and its letter
            token.command = string.char(b1)
            codes[#codes + 1] = "%" .. token.command
        elseif b0 < 0x80 or (b0 >= 0xa1 and b0 <= 0xdf) then
            token.sjis = string.char(b0)
            codes[#codes + 1] = token.sjis
        else
            token.sjis = string.char(b0, b1)
            codes[#codes + 1] = token.sjis
        end
        tokens[#tokens + 1] = token
        if token.command == "P" then
            return { key = table.concat(codes), tokens = tokens, raw = raw }
        end
    end
    return nil
end

local function cell_of(list, token)
    local left, top, right, bottom = string.unpack("<I2I2I2I2", list.raw, token.at + 65)
    if right <= left or bottom <= top or right > 480 or bottom > 272 then return false end
    return { left, top, right - left, bottom - top }
end

-- The line's text, its readings and a cell for every character of the text.
local function compose(list)
    local b = reader.builder()
    local ruby, cells = {}, {}
    local word       -- where a {word} began, in characters of the text
    local reading    -- the characters of a [reading] being gathered
    for _, token in ipairs(list.tokens) do
        if token.command then
            -- %N is a new line; the rest (wait for the button, new page, where the line
            -- stands, bold) say nothing that is text.
            if token.command == "N" and b:length() > 0 and not reading then
                b:add("\n")
                cells[#cells + 1] = false
            end
        elseif reading then
            if token.sjis == "]" then
                if word and b:length() > word then
                    ruby[#ruby + 1] = { word, b:length() - word, text.sjis(table.concat(reading)) }
                end
                word, reading = nil, nil
            else
                reading[#reading + 1] = token.sjis
            end
        elseif token.sjis == "{" then
            word = b:length()
        elseif token.sjis == "}" then
            -- the reading follows
        elseif token.sjis == "[" and word then
            reading = {}
        else
            b:add(DASH[token.sjis] or text.sjis(token.sjis))
            -- A space has no place worth pointing at (and the one its record gives is not
            -- where the space is).
            local blank = token.sjis == " " or token.sjis == "\x81\x40"
            cells[#cells + 1] = not blank and cell_of(list, token)
        end
    end
    -- A character's rectangle is as wide as the font's square, which is wider than the step
    -- to the next character: each cell ends where the next one on its row begins.
    for i = 1, #cells - 1 do
        local cell, after = cells[i], cells[i + 1]
        if cell and after and after[2] == cell[2] and after[1] > cell[1] and after[1] < cell[1] + cell[3] then
            cell[3] = after[1] - cell[1]
        end
    end
    return b:text(), ruby, cells
end

-- The message window is along the bottom; a line elsewhere (the middle of a dark screen)
-- has the room its own characters take.
local function box_of(cells)
    local left, top, right, bottom
    for _, cell in ipairs(cells) do
        if cell then
            left = math.min(left or cell[1], cell[1])
            top = math.min(top or cell[2], cell[2])
            right = math.max(right or 0, cell[1] + cell[3])
            bottom = math.max(bottom or 0, cell[2] + cell[4])
        end
    end
    if not left then return nil end
    if top >= 180 then return { 0, 184, 480, 88 } end
    return { math.max(0, left - 8), math.max(0, top - 6), math.min(480, right - left + 16), bottom - top + 12 }
end

local seen = {}      -- each list's line as it was last looked at
local current        -- the list whose line is on screen: the one that changed last
local since          -- the frame its line was first seen as it is
local reported       -- the line that was reported, as its list's codes
local shown = false
local sent           -- the places that were sent last, to send them again only when they change

local function withdraw()
    if shown then
        reader.layout("message", nil)
        shown = false
    end
    sent = nil
end

reader.on_frame(function(frame)
    local covered = mem.u8(COVERED)
    if not covered then return end
    for index, address in ipairs(LISTS) do
        local list = read_list(address)
        local key = list and list.key or ""
        if seen[index] == nil then
            seen[index] = key -- what was there when the profile began: not known to be on screen
        elseif key ~= seen[index] then
            seen[index] = key
            current = index
            since = frame
            reported = nil
            withdraw()
        end
    end
    if not current or seen[current] == "" then return end
    if covered ~= 0 then
        withdraw()
        return
    end
    if frame - since < SHOWN_FRAMES then return end
    local list = read_list(LISTS[current])
    if not list or list.key ~= seen[current] then return end
    local line, ruby, cells = compose(list)
    if line == "" then return end
    if reported ~= list.key then
        reported = list.key
        reader.emit{ text = line, speaker = "", ruby = ruby, window = "message", id = text.hash(list.key) }
        sent = nil
    end
    -- The characters come up one after another and the game settles a character's place
    -- when it comes up: the places are sent again whenever they have changed.
    if frame % 3 == 0 or not shown then
        local parts = {}
        for i, cell in ipairs(cells) do
            parts[i] = cell and (cell[1] .. "," .. cell[2] .. "," .. cell[3]) or "-"
        end
        local places = table.concat(parts, " ")
        if places ~= sent then
            sent = places
            reader.layout("message", { space = { 480, 272 }, box = box_of(cells), glyphs = cells })
            shown = true
        end
    end
end)

reader.on_state_loaded(function()
    seen = {}
    current, since, reported = nil, nil, nil
    shown = false
    sent = nil
end)
