// SPDX-FileCopyrightText: Reader fork contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "reader/core/crt.h"

namespace Reader::Crt {

namespace {

// Whether the picture is on in an install that was never told: a fork's build says so
// (READER_CRT_DEFAULT_ON, on its reader core). On for the consoles that were played on a
// tube, off for the two that never were.
#ifdef READER_CRT_DEFAULT_ON
#define READER_CRT_ON_AT_FIRST 1
#else
#define READER_CRT_ON_AT_FIRST 0
#endif

// ---- What can be set. The order is the settings page's. ----
//
// A new slider: a row here, a place for it in Pack(), and its use in crt_shader.glsl. A key
// is never renamed or reused, since it is what the users' settings are kept under.
const std::vector<Param> kParams = {
    {"enabled", "CRT picture", "", "Show the game as on a cathode-ray tube.", Kind::Toggle, 0, 1, 1,
     READER_CRT_ON_AT_FIRST,
     nullptr},

    {"scanlines", "Scan lines", "Lines", "How dark the gaps between the lines are.", Kind::Slider, 0,
     1, 0.05f, 0.90f, nullptr},
    {"beam", "Beam", "Lines", "How much a bright line swells and closes the gaps.", Kind::Slider, 0, 1,
     0.05f, 0.55f, nullptr},
    {"lines", "Lines", "Lines",
     "How many lines the tube draws. \"As the console\" halves them on a screen too coarse to show them.",
     Kind::Choice, 0, 720, 1, 240, "0=As the console|240=240|288=288|360=360|480=480|540=540|720=720"},
    {"sharpness", "Sharpness", "Lines", "How small the beam's spot is along a line.", Kind::Slider, 0,
     1, 0.05f, 0.55f, nullptr},

    {"mask", "Phosphor", "Phosphor", "The pattern of red, green and blue the tube's face is made of.",
     Kind::Choice, 0, 4, 1, 2,
     "0=None|1=Aperture grille (stripes)|2=Slot mask (bricks)|3=Shadow mask (dots)|4=Fine (small screens)"},
    {"mask_strength", "Phosphor strength", "Phosphor", "How plainly the pattern shows.", Kind::Slider,
     0, 1, 0.05f, 0.70f, nullptr},
    {"mask_size", "Phosphor size", "Phosphor", "How large the pattern is, in pixels of your screen.",
     Kind::Choice, 0, 3, 1, 0, "0=As the screen|1=1|2=2|3=3"},

    {"warp", "Warp", "Glass", "How much the glass bulges.", Kind::Slider, 0, 1, 0.05f, 0.80f, nullptr},
    {"corners", "Corners", "Glass", "How round the tube's corners are.", Kind::Slider, 0, 1, 0.05f,
     0.50f, nullptr},
    {"vignette", "Vignette", "Glass", "How much darker the picture gets towards its corners.",
     Kind::Slider, 0, 1, 0.05f, 0.50f, nullptr},
    {"glass", "Reflection", "Glass", "A lamp and the room, mirrored faintly in the glass.",
     Kind::Slider, 0, 1, 0.05f, 0.45f, nullptr},

    {"glow", "Glow", "Light", "Light that spreads a little around everything bright.", Kind::Slider,
     0, 1, 0.05f, 0.60f, nullptr},
    {"halation", "Halation", "Light", "Light that spreads far through the glass, warm and soft.",
     Kind::Slider, 0, 1, 0.05f, 0.60f, nullptr},
    {"bloom", "Bloom", "Light", "How readily bright light runs over the phosphor's pattern.",
     Kind::Slider, 0, 1, 0.05f, 0.60f, nullptr},
    {"brightness", "Brightness", "Light", "", Kind::Slider, 0.5f, 1.5f, 0.05f, 1.10f, nullptr},

    {"warmth", "Warmth", "Colour", "The tube's white: bluish to the left, amber to the right.",
     Kind::Slider, -1, 1, 0.05f, 0.50f, nullptr},
    {"colour", "Colour", "Colour", "How strong the colours are.", Kind::Slider, 0, 2, 0.05f, 1.15f,
     nullptr},
    {"contrast", "Contrast", "Colour", "", Kind::Slider, 0.7f, 1.4f, 0.05f, 1.00f, nullptr},

    {"convergence", "Convergence", "Signal",
     "How far apart red and blue land: coloured fringes at edges.", Kind::Slider, 0, 1, 0.05f, 0.40f,
     nullptr},
    {"grain", "Grain", "Signal", "Snow in the signal.", Kind::Slider, 0, 1, 0.05f, 0.25f, nullptr},
    {"flicker", "Flicker", "Signal", "A hum bar that wanders up the picture, and lines that tremble.",
     Kind::Slider, 0, 1, 0.05f, 0.20f, nullptr},

    {"frame", "Frame", "Frame",
     "The tube's surround: the plastic, the deep recess the glass sits in and the shadow it casts.",
     Kind::Choice, 0, 3, 1, 1, "0=None|1=Charcoal|2=Black|3=Beige", true},
    {"frame_room", "Room", "Frame",
     "Lit: a lamp in the room lights the plastic. Dark: only the picture does.", Kind::Choice, 0, 1,
     1, 0, "0=Lit|1=Dark", true},
    {"frame_wear", "Wear", "Frame", "Dust in the recess, grime in the seams, plastic aged by the years.",
     Kind::Slider, 0, 1, 0.05f, 0.50f, nullptr, true},
};

enum Index {
    Enabled_,
    Scanlines,
    Beam,
    Lines,
    Sharpness,
    Mask,
    MaskStrength,
    MaskSize,
    Warp,
    Corners,
    Vignette,
    Glass,
    Glow,
    Halation,
    Bloom,
    Brightness,
    Warmth,
    Colour,
    Contrast,
    Convergence,
    Grain,
    Flicker,
    FrameColour,
    FrameRoom,
    FrameWear,
    Count
};

// Each look names every value it does not share with the first, which is the values the
// table starts with: a change to the table's first values changes only the first look.
const std::vector<Preset> kPresets = {
    {"tube", "Old tube, strong", ""},
    {"warm", "Warm anime, soft",
     "lines=0;scanlines=0.55;beam=0.60;sharpness=0.70;mask=2;mask_strength=0.35;warp=0.35;corners=0.25;"
     "vignette=0.30;glass=0;glow=0.35;halation=0.30;bloom=0.50;brightness=1.05;warmth=0.35;"
     "colour=1.05;contrast=1.00;convergence=0.15;grain=0.10;flicker=0"},
    {"trinitron", "Trinitron",
     "lines=0;scanlines=0.65;beam=0.60;sharpness=0.85;mask=1;mask_strength=0.45;warp=0.15;corners=0.10;"
     "vignette=0.20;glass=0;glow=0.25;halation=0.20;bloom=0.50;brightness=1.05;warmth=0.10;"
     "colour=1.05;contrast=1.00;convergence=0.05;grain=0.05;flicker=0"},
    {"television", "Old television",
     "lines=0;scanlines=0.45;beam=0.75;sharpness=0.35;mask=3;mask_strength=0.40;warp=0.60;corners=0.45;"
     "vignette=0.45;glass=0.40;glow=0.50;halation=0.50;bloom=0.50;brightness=1.05;warmth=0.45;"
     "colour=1.05;contrast=1.00;convergence=0.40;grain=0.30;flicker=0.30"},
    {"arcade", "Arcade cabinet",
     "lines=0;scanlines=0.80;beam=0.50;sharpness=0.60;mask=2;mask_strength=0.55;warp=0.45;corners=0.30;"
     "vignette=0.30;glass=0;glow=0.40;halation=0.35;bloom=0.60;brightness=1.10;warmth=0.20;"
     "colour=1.20;contrast=1.00;convergence=0.15;grain=0.10;flicker=0"},
    {"monitor", "Studio monitor",
     "lines=0;scanlines=0.85;beam=0.45;sharpness=1.00;mask=1;mask_strength=0.30;warp=0;corners=0;"
     "vignette=0;glass=0;glow=0.15;halation=0.10;bloom=0.40;brightness=1.05;warmth=0;colour=1.00;"
     "contrast=1.00;convergence=0;grain=0;flicker=0"},
    {"lines", "Scan lines only",
     "lines=0;scanlines=0.60;beam=0.60;sharpness=0.90;mask=0;mask_strength=0;warp=0;corners=0;"
     "vignette=0;glass=0;glow=0.10;halation=0;bloom=0.50;brightness=1.00;warmth=0;colour=1.00;"
     "contrast=1.00;convergence=0;grain=0;flicker=0"},
};

std::array<std::atomic<float>, Count> g_values;
std::atomic<unsigned> g_revision{1};
std::atomic<int> g_native_lines{480};
std::atomic<bool> g_ready{false};

void EnsureReady() {
    if (g_ready.load(std::memory_order_acquire)) {
        return;
    }
    static std::atomic_flag once = ATOMIC_FLAG_INIT;
    if (!once.test_and_set()) {
        for (int i = 0; i < Count; i++) {
            g_values[i].store(kParams[i].initial, std::memory_order_relaxed);
        }
        g_ready.store(true, std::memory_order_release);
    } else {
        while (!g_ready.load(std::memory_order_acquire)) {
        }
    }
}

int IndexOf(const char* key) {
    for (int i = 0; i < Count; i++) {
        if (std::strcmp(kParams[i].key, key) == 0) {
            return i;
        }
    }
    return -1;
}

float Value(int index) {
    EnsureReady();
    return g_values[index].load(std::memory_order_relaxed);
}

float Fit(const Param& param, float value) {
    if (!(value == value)) {
        return param.initial;
    }
    return std::clamp(value, param.low, param.high);
}

// Calls `each(key, value)` for every "key=value" of the text.
template <typename F>
void EachPair(const std::string& text, F&& each) {
    std::size_t at = 0;
    while (at < text.size()) {
        std::size_t end = text.find(';', at);
        if (end == std::string::npos) {
            end = text.size();
        }
        const std::size_t equals = text.find('=', at);
        if (equals != std::string::npos && equals < end) {
            const std::string key = text.substr(at, equals - at);
            const std::string value = text.substr(equals + 1, end - equals - 1);
            char* stop = nullptr;
            // A number is written with a point whatever the user's language.
            double number = 0;
            {
                double sign = 1, whole = 0, part = 0, scale = 1;
                const char* p = value.c_str();
                if (*p == '-') {
                    sign = -1;
                    p++;
                }
                bool any = false;
                for (; *p >= '0' && *p <= '9'; p++, any = true) {
                    whole = whole * 10 + (*p - '0');
                }
                if (*p == '.') {
                    for (p++; *p >= '0' && *p <= '9'; p++, any = true) {
                        scale /= 10;
                        part += (*p - '0') * scale;
                    }
                }
                stop = any && *p == 0 ? const_cast<char*>(p) : nullptr;
                number = sign * (whole + part);
            }
            if (stop != nullptr) {
                each(key.c_str(), static_cast<float>(number));
            }
        }
        at = end + 1;
    }
}

float Smooth(float low, float high, float x) {
    const float t = std::clamp((x - low) / (high - low), 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
}

// How far the glass bends, as crt_shader.glsl does it.
void Bend(float warp, float& x, float& y) {
    const float cx = x * 2.f - 1.f;
    const float cy = y * 2.f - 1.f;
    x = cx * (1.f + warp * 0.062f * cy * cy) * 0.5f + 0.5f;
    y = cy * (1.f + warp * 0.082f * cx * cx) * 0.5f + 0.5f;
}

const char kShaderBody[] =
#include "reader/core/crt_shader.inc"
    ;

} // namespace

const std::vector<Param>& Params() {
    return kParams;
}

const std::vector<Preset>& Presets() {
    return kPresets;
}

float Get(const char* key) {
    const int index = IndexOf(key);
    return index < 0 ? 0.f : Value(index);
}

void Set(const char* key, float value) {
    const int index = IndexOf(key);
    if (index < 0) {
        return;
    }
    EnsureReady();
    const float fitted = Fit(kParams[index], value);
    if (g_values[index].exchange(fitted, std::memory_order_relaxed) != fitted) {
        g_revision.fetch_add(1, std::memory_order_relaxed);
    }
}

void Reset() {
    for (int i = 1; i < Count; i++) {
        if (!kParams[i].frame) {
            Set(kParams[i].key, kParams[i].initial);
        }
    }
}

bool ApplyPreset(const char* preset) {
    for (const Preset& entry : kPresets) {
        if (std::strcmp(entry.key, preset) == 0) {
            Reset();
            EachPair(entry.values, [](const char* key, float value) {
                const int index = IndexOf(key);
                if (index > 0 && !kParams[index].frame) {
                    Set(key, value);
                }
            });
            return true;
        }
    }
    return false;
}

std::string CurrentPreset() {
    for (const Preset& entry : kPresets) {
        std::array<float, Count> wanted;
        for (int i = 0; i < Count; i++) {
            wanted[i] = kParams[i].initial;
        }
        EachPair(entry.values, [&wanted](const char* key, float value) {
            const int index = IndexOf(key);
            if (index > 0) {
                wanted[index] = value;
            }
        });
        bool same = true;
        for (int i = 1; i < Count && same; i++) {
            same = kParams[i].frame || std::fabs(wanted[i] - Value(i)) < 0.001f;
        }
        if (same) {
            return entry.key;
        }
    }
    return {};
}

std::string Serialize() {
    std::string text;
    for (int i = 0; i < Count; i++) {
        // Thousandths, written without the user's language having a say in the point.
        const long thousandths = std::lround(Value(i) * 1000.f);
        const long whole = std::labs(thousandths) / 1000;
        const long part = std::labs(thousandths) % 1000;
        char number[32];
        std::snprintf(number, sizeof(number), "%s%ld.%03ld", thousandths < 0 ? "-" : "", whole, part);
        if (!text.empty()) {
            text += ';';
        }
        text += kParams[i].key;
        text += '=';
        text += number;
    }
    return text;
}

void Deserialize(const std::string& text) {
    EachPair(text, [](const char* key, float value) { Set(key, value); });
}

bool Enabled() {
    return Value(Enabled_) >= 0.5f;
}

unsigned Revision() {
    return g_revision.load(std::memory_order_relaxed);
}

void SetNativeLines(int lines) {
    if (lines >= 100 && lines <= 2160 && g_native_lines.exchange(lines) != lines) {
        g_revision.fetch_add(1, std::memory_order_relaxed);
    }
}

void Pack(const Picture& picture, float out[UniformFloats]) {
    const float source_w = std::max(picture.source_width, 1.f);
    const float source_h = std::max(picture.source_height, 1.f);
    const float output_w = std::max(picture.output_width, 1.f);
    const float output_h = std::max(picture.output_height, 1.f);

    // The lines: the user's number, or the console's, halved until the screen has three of
    // its pixels for each (fewer cannot show a line and its gap).
    const float native = static_cast<float>(g_native_lines.load(std::memory_order_relaxed));
    float lines = Value(Lines);
    if (lines < 1.f) {
        lines = native;
        while (output_h / lines < 3.f && lines > 240.f) {
            lines *= 0.5f;
        }
    }
    lines = std::floor(lines);
    // Lines too close for the screen fade out rather than beat against its pixels.
    const float fade = Smooth(2.0f, 3.2f, output_h / lines);
    // A source finer than a row a line keeps its own detail under the lines.
    const float own = Smooth(1.2f, 2.0f, source_h / lines);
    // The beam's spot is measured in the console's own pixels along a line.
    const float across = std::min(source_w, std::max(lines, native) * output_w / output_h);

    float mask_size = Value(MaskSize);
    if (mask_size < 1.f) {
        mask_size = std::max(1.f, std::round(output_h / 1080.f));
    }

    // Only what moves needs the time, which is kept small for the shader's arithmetic.
    static const auto start = std::chrono::steady_clock::now();
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    const float time = static_cast<float>(std::fmod(seconds, 600.0));

    // Two pairs share a number each (crt_shader.glsl says why): whole lines with the
    // source's own detail behind the point, the pattern with its size behind the point.
    const float lines_and_own = std::floor(lines) + std::min(own, 1.f) * 0.99f;
    const float mask = std::floor(Value(Mask) + 0.5f) + std::min(mask_size, 9.f) * 0.1f;
    const float values[UniformFloats] = {
        picture.origin[0], picture.origin[1], picture.right[0],  picture.right[1],
        picture.down[0],   picture.down[1],   source_w,          source_h,
        output_w,          output_h,          lines_and_own,     across,
        time,              mask,              Value(Scanlines) * fade, Value(Beam),
        Value(Sharpness),  Value(MaskStrength), Value(Warp),     Value(Corners),
        Value(Vignette),   Value(Glow),       Value(Halation),   Value(Bloom),
        Value(Brightness), Value(Warmth),     Value(Colour),     Value(Contrast),
        Value(Grain),      Value(Flicker),    Value(Glass),      Value(Convergence),
    };
    std::memcpy(out, values, sizeof(values));
}

const char* ShaderBody() {
    return kShaderBody;
}

const char* HlslDefines() {
    return "#define vec2 float2\n"
           "#define vec3 float3\n"
           "#define vec4 float4\n"
           "#define mix lerp\n"
           "#define fract frac\n";
}

void FromScreen(float& x, float& y) {
    if (!Enabled()) {
        return;
    }
    const float warp = Value(Warp);
    if (warp > 0.f) {
        Bend(warp, x, y);
    }
}

void ToScreen(float& x, float& y) {
    if (!Enabled()) {
        return;
    }
    const float warp = Value(Warp);
    if (warp <= 0.f) {
        return;
    }
    // Bend() has no formula for its way back; it is nearly straight, so stepping towards
    // the place gets there in a few steps.
    float sx = x;
    float sy = y;
    for (int i = 0; i < 6; i++) {
        float bx = sx;
        float by = sy;
        Bend(warp, bx, by);
        sx += x - bx;
        sy += y - by;
    }
    x = sx;
    y = sy;
}

} // namespace Reader::Crt
