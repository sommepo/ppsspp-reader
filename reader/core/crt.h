// SPDX-FileCopyrightText: Reader fork contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <vector>

/**
 * The CRT picture: the game shown as a cathode-ray tube would show it (scan lines, phosphor,
 * curved glass, glow), by one shader that every reader fork carries (crt_shader.glsl).
 *
 * This file is the same in every fork and knows nothing of an emulator or a toolkit. It has
 *  - the table of what can be set (Params): the settings pages on a computer and on Android
 *    are both built from it, so a new slider is one row here and its use in the shader;
 *  - the values, which a settings page writes and a renderer reads, from any thread;
 *  - what a renderer hands the shader (Pack), and the shader's text for each graphics API.
 * Kept as one line of text ("enabled=1;scanlines=0.55;..."), whatever a platform keeps its
 * settings in.
 */
namespace Reader::Crt {

enum class Kind {
    Toggle,
    Slider,
    Choice, ///< `choices`: "value=Name" entries separated by '|'
};

struct Param {
    const char* key;   ///< In the kept text; never renamed
    const char* name;  ///< As the settings page shows it
    const char* group; ///< The heading it stands under
    const char* hint;  ///< One sentence for the settings page
    Kind kind;
    float low;
    float high;
    float step;
    float initial;
    const char* choices;
    /// The tube's frame around the picture (reader_crt_frame), drawn by the computer's reader
    /// window: not part of a look nor of Reset, and not on the Android settings page.
    bool frame = false;
};

struct Preset {
    const char* key;
    const char* name;
    const char* values; ///< As the kept text, without "enabled"; what it leaves out is as initially
};

[[nodiscard]] const std::vector<Param>& Params();
[[nodiscard]] const std::vector<Preset>& Presets();

[[nodiscard]] float Get(const char* key);
void Set(const char* key, float value);
/// Every value as at first, but for whether the picture is on and the frame's.
void Reset();
/// Sets what `preset` names and everything else as at first. False when there is no such preset.
bool ApplyPreset(const char* preset);
/// The key of the preset the values are those of, or "" when they are the user's own.
[[nodiscard]] std::string CurrentPreset();

[[nodiscard]] std::string Serialize();
/// Takes the kept text; a key it does not name keeps its value, one it does not know is skipped.
void Deserialize(const std::string& text);

[[nodiscard]] bool Enabled();
/// Counts up with every change, for whoever keeps something made from the values.
[[nodiscard]] unsigned Revision();

/// How many lines the console's own picture has (480 for a Dreamcast): what "Lines: as the
/// console" means. Told once by the fork's emulator glue.
void SetNativeLines(int lines);

/// Where the picture is, for one drawing of it.
struct Picture {
    /// The source texture's coordinate of the picture's top left corner as it lies on the
    /// screen, and how far the coordinate moves along the picture's whole width (`right`)
    /// and whole height (`down`). A turned or mirrored source is said with these.
    float origin[2] = {0.f, 0.f};
    float right[2] = {1.f, 0.f};
    float down[2] = {0.f, 1.f};
    /// The source's pixels under the picture, across and down as it lies on the screen.
    float source_width = 640.f;
    float source_height = 480.f;
    /// The picture's size on the screen, in the screen's pixels.
    float output_width = 640.f;
    float output_height = 480.f;
};

inline constexpr int UniformVectors = 8;
inline constexpr int UniformFloats = UniformVectors * 4;
/// Fills the eight vectors the shader reads with CRT_U(0) to CRT_U(7): 128 bytes, which is
/// what every graphics API hands a shader without a buffer of its own.
void Pack(const Picture& picture, float out[UniformFloats]);

/// The shader's text: the function `vec3 ReaderCrt(vec2 p)` (crt_shader.glsl). What must be
/// defined before it is said at its top.
[[nodiscard]] const char* ShaderBody();
/// Put before ShaderBody() in HLSL: the names GLSL has for what HLSL calls otherwise.
[[nodiscard]] const char* HlslDefines();

/// Where on the screen a place of the picture is seen once the glass has bent it; both in
/// the picture's own measure, 0 to 1 from its top left corner. The place itself while the
/// picture is off or flat. For whatever is laid over the game's text.
void ToScreen(float& x, float& y);
/// The other way: what of the picture a place on the screen shows.
void FromScreen(float& x, float& y);

} // namespace Reader::Crt
