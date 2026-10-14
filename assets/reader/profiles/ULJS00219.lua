-- reader: title 428 ～封鎖された渋谷で～ (428: Shibuya Scramble)
-- reader: reads The story's text with furigana, TIPS and choices
-- reader: version ULJS-00219, disc version 1.01
-- reader: audio never
-- 428 ～封鎖された渋谷で～ (Chunsoft, 2009), PSP, ULJS-00219 v1.01. EBOOT.BIN F4CEFE66.
--
-- The game lays its text out one character at a time with one routine, which appends a record
-- to a list of the text surface it is drawing on: the character as Unicode, its colour and
-- opacity, and its rectangle on the screen in pixels. The story's text stays in that list
-- for as long as it is on the page. This profile hooks the routine only to learn which
-- surface is the story's and which characters are furigana; the text and its places are read
-- from the list once a frame.

local BUILD = "F4CEFE66"
if reader.build_id ~= BUILD then
    error("this profile is for 428 ULJS-00219 v1.01 (EBOOT " .. BUILD .. "), the game is " ..
          tostring(reader.build_id))
end
local base = reader.base - 0x08804000 -- the executable is not relocatable: this is zero

-- Code. a0 is the text surface, a1 the character (Unicode).
local DRAW_GLYPH = 0x0884e674
-- Where DRAW_GLYPH returns to when the story's font object calls it.
local RA_STORY = 0x0886c074    -- a character of the script
local RA_ELLIPSIS = 0x0886c3f8 -- a dot of the script's "……" command
local RA_RUBY = 0x0886cd8c     -- a character of a reading, drawn right after its base

-- The text surface.
local SURFACE_GLYPHS = 0x13c -- pointer to the records
local SURFACE_COUNT = 0x140  -- how many are in use
-- A record, 60 bytes: u32 character, f32 red green blue alpha, f32 x y (in the game's own
-- 1280x720 units), f32 left top right bottom (pixels of the 480x272 screen), ...
local RECORD = 60
local RECORD_FORMAT = "<I4 f f f f f f f f f f"

-- The script's commands, by number: a table of handlers, eight bytes each.
local COMMANDS = 0x089c090c
-- The commands that end a piece of text: wait for the button (0x1e), wait and then turn the
-- page (0x1f), go on without waiting (0x20). Each runs once, when the piece is laid out.
local BOUNDARIES = { 0x1e, 0x1f, 0x20 }
-- A choice: 0x53 (and 0x54, a choice with a time limit) begins one, 0x5e ends an answer, 0xbc
-- follows the last. The answers are laid out on the story's surface like any text.
local CHOICE_BEGIN = { 0x53, 0x54 }
local CHOICE_CUT = { 0x5e, 0xbc }
-- The answers are taken as shown when nothing was added for this many frames.
local CHOICE_FRAMES = 20
-- A piece is reported when it has been up for this many frames and its last character has
-- faded in: what a skip runs through is not read.
local SHOWN_FRAMES = 4
-- Text that no such command followed is taken as a piece when nothing was added for this long.
local QUIET_FRAMES = 120

-- Lines that end like this end a sentence: a line break after anything else is the game
-- wrapping a sentence, and is left out of the text.
local SENTENCE_END = {}
for _, c in ipairs{ "。", "！", "？", "」", "』", "）", "…", "‥", "―", "♪", "!", "?", ".", "〟", "”" } do
    SENTENCE_END[utf8.codepoint(c)] = true
end

local choice_begun -- the frame a choice last began on
local story -- the story's surface, once a character was seen being drawn on it
local ruby_at = {} -- by index in the surface's list: true for a reading's character

-- The page as it is known: every record read so far, and the pieces it was reported in.
local page = { glyphs = {}, pieces = {}, pending_from = 1, grown = 0 }
-- The last few pages that were cleared, newest first, to tell a page that is put back (after
-- the menu, after a TIP was read) from a new one.
local history = {}
local HISTORY_PAGES = 4
local piece_serial = 0

local function close_pieces()
    for _, piece in ipairs(page.pieces) do
        if piece.shown then
            reader.layout(piece.window, nil)
            piece.shown = false
        end
    end
end

local function new_page()
    close_pieces()
    if #page.pieces > 0 then
        table.insert(history, 1, page)
        history[HISTORY_PAGES + 1] = nil
    end
    page = { glyphs = {}, pieces = {}, pending_from = 1, grown = reader.frame() }
end

local function read_records(surface, from, to)
    local list = mem.u32(surface + SURFACE_GLYPHS)
    if not list then return nil end
    local raw = mem.read(list + from * RECORD, (to - from) * RECORD)
    if #raw < (to - from) * RECORD then return nil end
    local out = {}
    for i = 0, to - from - 1 do
        local cp, _, _, _, alpha, _, _, left, top, right, bottom =
            string.unpack(RECORD_FORMAT, raw, i * RECORD + 1)
        out[#out + 1] = { cp = cp, alpha = alpha, left = left, top = top, right = right,
                          bottom = bottom, index = from + i }
    end
    return out
end

-- Turns glyphs first..last of the page into text, ruby spans and one cell per character.
local function compose(first, last)
    local glyphs = page.glyphs
    -- A reading's characters are small: when the hook did not see them drawn (a page that
    -- was up before the profile was loaded), their height tells.
    local tallest = 0
    for i = first, last do
        tallest = math.max(tallest, glyphs[i].bottom - glyphs[i].top)
    end
    local function is_ruby(g)
        local known = ruby_at[g.index]
        if known ~= nil then return known end
        return (g.bottom - g.top) < tallest * 0.7
    end

    local b = reader.builder()
    local cells = {}
    local spans = {} -- { start, length, reading } in code points, one per base character
    local previous -- the previous base glyph
    local line_last -- the last character of the line so far
    for i = first, last do
        local g = glyphs[i]
        if is_ruby(g) then
            local span = spans[#spans]
            if previous and span and span.base == previous then
                span.reading = span.reading .. utf8.char(g.cp)
            elseif previous then
                spans[#spans + 1] = { start = b:length() - 1, length = 1, base = previous,
                                      reading = utf8.char(g.cp) }
            end
        else
            if previous and math.abs(g.top - previous.top) > (previous.bottom - previous.top) * 0.5 then
                -- A new line: a break of the text only where a sentence ended, or where the
                -- line is another kind of text (a TIP's title above its body: a larger step
                -- down, another size).
                local height = previous.bottom - previous.top
                local other = math.abs((g.bottom - g.top) - height) > height * 0.05 or
                              g.top - previous.top > height * 1.5
                if SENTENCE_END[line_last] or other then
                    b:add("\n")
                    cells[#cells + 1] = false
                end
            end
            b:add(utf8.char(g.cp))
            cells[#cells + 1] = { g.left, g.top, g.right - g.left, g.bottom - g.top }
            previous = g
            line_last = g.cp
        end
    end
    -- Readings of characters side by side are one reading of the word.
    local ruby = {}
    for _, span in ipairs(spans) do
        local last_span = ruby[#ruby]
        if last_span and last_span[1] + last_span[2] == span.start then
            last_span[2] = last_span[2] + span.length
            last_span[3] = last_span[3] .. span.reading
        else
            ruby[#ruby + 1] = { span.start, span.length, span.reading }
        end
    end
    return b:text(), ruby, cells
end

local function show(piece)
    reader.layout(piece.window, { space = { 480, 272 }, glyphs = piece.cells })
    piece.shown = true
end

-- Glyphs first..last of the page, which begin where the last piece ended, become a piece.
local function report(first, last)
    if last < first then return end
    local text, ruby, cells = compose(first, last)
    page.pending_from = last + 1
    if text == "" then return end
    -- A page that is put back as it was: the same pieces again, not new text.
    local piece
    if page.replay then
        for _, old in ipairs(page.replay.pieces) do
            if old.first == first and old.last == last and old.text == text then
                piece = { window = old.window, first = first, last = last, text = text }
                break
            end
        end
    end
    if not piece then
        piece_serial = piece_serial + 1
        piece = { window = "story." .. piece_serial, first = first, last = last, text = text }
        reader.emit{ text = text, ruby = ruby, window = piece.window }
    end
    piece.cells = cells
    page.pieces[#page.pieces + 1] = piece
    show(piece)
end

-- The answers of a choice: one event, an answer to a line.
local function report_choice()
    local choice = page.choice
    local cuts = {}
    for _, cut in ipairs(choice.cuts) do
        if cut > (cuts[#cuts] or 0) and cut < #page.glyphs then cuts[#cuts + 1] = cut end
    end
    cuts[#cuts + 1] = #page.glyphs
    local texts, ruby, cells = {}, {}, {}
    local first, length = 1, 0
    for _, cut in ipairs(cuts) do
        local line, spans, places = compose(first, cut)
        if line ~= "" then
            if #texts > 0 then
                cells[#cells + 1] = false
                length = length + 1
            end
            texts[#texts + 1] = line
            for _, span in ipairs(spans) do
                ruby[#ruby + 1] = { span[1] + length, span[2], span[3] }
            end
            for _, cell in ipairs(places) do cells[#cells + 1] = cell end
            length = length + text.len(line)
        end
        first = cut + 1
    end
    page.pending_from = #page.glyphs + 1
    choice.reported = true
    if #texts == 0 then return end
    local piece = { window = "choice", first = 1, last = #page.glyphs,
                    text = table.concat(texts, "\n"), cells = cells }
    -- The same answers put back (after the menu) are not a new choice.
    local again = false
    for _, old in ipairs(history) do
        if old.choice and old.pieces[1] and old.pieces[1].text == piece.text then again = true end
    end
    if not again then
        reader.emit{ text = piece.text, ruby = ruby, channel = "choice", window = "choice" }
    end
    page.pieces[#page.pieces + 1] = piece
    show(piece)
end

-- Everything on the page that is not in a piece yet. A page that was put back came all at
-- once: it is cut where it was cut when it was first read.
local function report_pending(last)
    if page.replay then
        local again = true
        while again do
            again = false
            for _, old in ipairs(page.replay.pieces) do
                if old.first == page.pending_from and old.last <= last then
                    report(old.first, old.last)
                    again = true
                    break
                end
            end
        end
    end
    report(page.pending_from, last)
end

reader.hook(base + DRAW_GLYPH, function(cpu)
    local ra = cpu.ra - base
    if ra ~= RA_STORY and ra ~= RA_RUBY and ra ~= RA_ELLIPSIS then return end
    local surface = cpu.a0
    if story ~= surface then
        story = surface
        ruby_at = {}
        new_page()
        history = {}
        page.found_at = mem.u32(surface + SURFACE_COUNT)
        -- The surface's first text since the profile was loaded may be a choice's answers.
        if page.found_at == 0 and choice_begun and reader.frame() - choice_begun < 600 then
            page.choice = { cuts = {} }
        end
    end
    local index = mem.u32(surface + SURFACE_COUNT)
    if index then ruby_at[index] = (ra == RA_RUBY) end
end)

local function command_handler(code)
    local handler = mem.u32(base + COMMANDS + code * 8)
    if not handler or handler < 0x08800000 then
        error(string.format("no handler for the script's command %02x where it is expected", code))
    end
    return handler
end

for _, code in ipairs(CHOICE_BEGIN) do
    reader.hook(command_handler(code), function()
        choice_begun = reader.frame()
        if not story then return end
        -- The page was cleared for the answers a moment ago.
        local count = mem.u32(story + SURFACE_COUNT)
        if count and count < #page.glyphs then new_page() end
        if #page.glyphs == 0 then page.choice = { cuts = {} } end
    end)
end
for _, code in ipairs(CHOICE_CUT) do
    reader.hook(command_handler(code), function()
        if not story or not page.choice then return end
        local count = mem.u32(story + SURFACE_COUNT)
        if count then page.choice.cuts[#page.choice.cuts + 1] = count end
    end)
end

for _, code in ipairs(BOUNDARIES) do
    reader.hook(command_handler(code), function()
        if not story then return end
        local count = mem.u32(story + SURFACE_COUNT)
        if count and count > 0 then
            page.boundary = count
            page.boundary_frame = reader.frame()
        end
    end)
end

reader.on_frame(function(frame)
    if not story then return end
    local count = mem.u32(story + SURFACE_COUNT)
    if not count or count > 4096 then return end
    if count < #page.glyphs then
        new_page()
    end
    if count > #page.glyphs then
        local records = read_records(story, #page.glyphs, count)
        if not records then return end
        for _, g in ipairs(records) do page.glyphs[#page.glyphs + 1] = g end
        page.grown = frame
        -- Is this the page from before the clearing, being put back?
        if page.replay == nil then
            page.candidates = { table.unpack(history) }
        end
        if page.replay ~= false then
            for n = #page.candidates, 1, -1 do
                local old = page.candidates[n].glyphs
                for i = #page.glyphs - #records + 1, #page.glyphs do
                    if not old[i] or old[i].cp ~= page.glyphs[i].cp then
                        table.remove(page.candidates, n)
                        break
                    end
                end
            end
            page.replay = page.candidates[1] or false
        end
        -- What was on the page before the profile saw anything drawn is a piece of its own.
        if page.found_at then
            if page.found_at > 0 and page.found_at <= #page.glyphs then
                report(1, page.found_at)
            end
            page.found_at = nil
        end
        return
    end
    if count == 0 then return end
    -- Nothing new this frame. Has a piece ended?
    local last = nil
    if page.choice then
        if not page.choice.reported and frame - page.grown >= CHOICE_FRAMES then
            local fresh = read_records(story, count - 1, count)
            if fresh and fresh[1].alpha >= 0.99 then
                local now = read_records(story, 0, count)
                if now then page.glyphs = now end
                report_choice()
            end
        end
    elseif page.boundary and page.boundary >= page.pending_from and page.boundary <= count and
       frame - page.boundary_frame >= SHOWN_FRAMES then
        last = page.boundary
    elseif page.replay and page.pending_from <= count and frame - page.grown >= SHOWN_FRAMES then
        last = count -- a page put back: no command of the script runs for it again
    elseif page.pending_from <= count and frame - page.grown >= QUIET_FRAMES then
        last = count
    end
    if last then
        local fresh = read_records(story, last - 1, last)
        if fresh and fresh[1].alpha >= 0.99 then
            -- Places and opacity as they are now, not as they were when the glyphs came.
            local now = read_records(story, page.pending_from - 1, count)
            if now then
                for i, g in ipairs(now) do page.glyphs[page.pending_from - 1 + i] = g end
            end
            report_pending(last)
            page.boundary = nil
        end
    end
    -- Text that has faded away is not on screen, whatever the list says.
    for _, piece in ipairs(page.pieces) do
        local g = read_records(story, piece.first - 1, piece.first)
        local visible = g ~= nil and g[1].alpha >= 0.5
        if piece.shown and not visible then
            reader.layout(piece.window, nil)
            piece.shown = false
        elseif not piece.shown and visible then
            show(piece)
        end
    end
end)

reader.on_state_loaded(function()
    story = nil
    ruby_at = {}
    page = { glyphs = {}, pieces = {}, pending_from = 1, grown = 0 }
    history = {}
end)
