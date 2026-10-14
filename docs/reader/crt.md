# The CRT picture

*Reader settings, CRT picture* shows the game as a cathode-ray tube would show it. **In this app it is on from the first start** (the build says so: `READER_CRT_DEFAULT_ON` on the reader's core); the switch turns it off.
It is the reader's own: the emulator's own filters and shaders are
left as they are, and while the picture is on it takes the place of nothing: it is one more pass after whatever post-processing shaders are chosen.

## What it does

One switch, six ready-made looks, and a slider for every part. A change shows in the game at
once.

| Group | Sliders | |
| --- | --- | --- |
| Lines | Scan lines, Beam, Lines, Sharpness | The dark gaps between the tube's lines; how much a bright line swells and closes them; how many lines there are; how small the beam's spot is along a line. |
| Phosphor | Phosphor, Phosphor strength, Phosphor size | The pattern of red, green and blue the tube's face is made of: aperture grille (stripes), slot mask (bricks), shadow mask (dots), or a fine one for small screens. |
| Glass | Warp, Corners, Vignette, Reflection | The bulge of the glass, its rounded corners, the darker corners, a lamp mirrored faintly in it. |
| Light | Glow, Halation, Bloom, Brightness | Light that spreads a little around what is bright, light that spreads far and warm through the glass, bright light running over the phosphor's pattern. |
| Colour | Warmth, Colour, Contrast | The tube's white from bluish to amber, the strength of its colours. |
| Signal | Convergence, Grain, Flicker | Red and blue landing apart, snow, a hum bar wandering up the picture and lines that tremble. |

The looks: **Old tube, strong** (the values a new install has, which the maintainer chose on
their own screen: 240 thick lines, a plain slot mask, a strong bulge, much glow, a little
snow and flicker), **Warm anime, soft** (the gentle one, with the console's own number of
lines), **Trinitron**, **Old television**, **Arcade cabinet**, **Studio monitor** and
**Scan lines only**. Moving a slider makes the picture "My own".

Things worth knowing:

- **Lines: as the console** takes the console's own number of lines (272 for a PSP) and halves it
  until the screen has three of its pixels for each: fewer cannot show a line and its gap,
  and lines that are still too fine fade out rather than beat against the screen's pixels.
  A number chosen by hand is taken as it is.
- **A picture drawn finer than the console drew it** (a higher rendering resolution) keeps
  its detail: the lines are laid over it. A picture at the console's own resolution is
  shown as its rows, each the middle of a line.
- **Phosphor size: as the screen** is one pixel up to 1080 lines of screen, two at 2160.
- **The words stay where the text is.** What the reader lays over the game's text (the
  places a click or a tap finds a word by, the mark on a word) is bent with the glass.
- **The handheld make is lighter**: the Android apps build the shader with fewer samples for
  the glow and along a line. Everything else is the same.

## How it is made

One shader, one pass, the same text in every reader fork and for every graphics API:
`reader/core/crt_shader.glsl`, written in what GLSL ES 1.00, GLSL 3.30, Vulkan GLSL and HLSL have
in common. It was written here; what it does follows the CRT filters of
[michadame](https://github.com/Morgawr/michadame) (its two tube filters, its glass and its
cathode interference), which the maintainer asked to be the model, and its phosphor patterns
follow Timothy Lottes' public-domain CRT shader, as michadame's do.

| File | |
| --- | --- |
| `reader/core/crt.h`, `crt.cpp` | The table of what can be set (`kParams`), the looks (`kPresets`), the values, and `Pack`: the eight vectors (128 bytes) a renderer hands the shader. Knows nothing of an emulator or a toolkit. |
| `reader/core/crt_shader.glsl`, `crt_shader.inc` | The shader, and the same as a C++ string (made by `crt.py`). |
| `reader/qt/reader_crt_page.*` | The settings page on a computer, built from the table. |
| `reader/core/crt_jni.h`, `android/src/org/ppsspp/ppsspp/reader/ui/CrtSettings.kt` | The Android apps' bridge and their settings page, built from the same table. |
| `reader/desktop/tests/crt_test.cpp` | Tests of the table, the kept text, the looks, what the shader is handed, the glass's bend. |
| `assets/shaders/reader_crt.fsh`, `reader_crt_mobile.fsh` | The shader as two of this emulator's post-processing files (the second is the handheld make), made by `crt.py`: do not edit. |

**These files are the same in every fork, and the Flycast Reader fork's are the ones to
change.** `tools/reader/crt.py sync` there writes `crt_shader.inc` and copies them to the
sibling forks beside it, each with its own `#include` paths and Kotlin package (and makes
the shader files the PPSSPP and Eden forks build theirs from); `crt.py check` says which
copy differs.

**A new slider** is a row in `kParams`, a place in `Pack` (the eight vectors are full: two
pairs already share a number each, as the shader's first lines say), its use in the shader,
`crt.py sync`, and a build. Neither settings page names a slider. The values are kept as
one line of text (`enabled=1;scanlines=0.550;...`): `picture/crt` in the desktop app's
`reader.ini`, the preference `reader_crt` on Android. A key is never renamed.

## In this emulator

The emulator has a chain of post-processing shaders, written once and translated for each
graphics API. The CRT picture is one more of them, `ReaderCRT` (`ReaderCRTMobile` on
Android) in `assets/shaders/defaultshaders.ini`, not shown in the emulator's own list:

- `PresentationCommon::UpdatePostShader` puts it at the chain's end while the picture is on,
  and `FramebufferManagerCommon::CheckPostShaders` has the chain made again when it is
  switched.
- Its eight vectors travel with the post-processing shaders' own values:
  `PostShaderUniforms::crt` (`GPU/Common/PresentationCommon.h`), named `u_crt0` to `u_crt7`
  in `PresentationCommon.cpp` and in the blocks `Common/GPU/ShaderTranslation.cpp` writes
  for Vulkan and Direct3D 11. `vr` moved to directly behind `video` in that structure, which
  is where those blocks always had it.

The emulator's own post-processing shaders still run before it. Whether a card's picture
shows the CRT picture was not looked at.

## How it was checked

- **Tests** with made-up values: `crt_test.cpp`, part of `tools/reader/test-desktop.sh`.
- **In the app on Linux, OpenGL, drawn in software** (2026-10-13): STEINS;GATE with the
  picture switched on over the development channel, in a picture of the window: the bent
  glass, the rounded corners, the lamp at the top. The Windows app draws with OpenGL too.
- **The shader as the Vulkan path gets it**: the emulator's own conversion replayed by hand
  and the result compiled and checked as SPIR-V. **Not run**: nothing here draws this
  emulator with Vulkan, which is what the Android app uses.
- **Windows**: the package was built with it (2026-10-13). Never run.
- **Android**: the APK was built with it (both shader files are in it) and the 193 tests of the reading layer pass; the page itself was drawn nowhere. On no device.
- **Not looked at**: Direct3D 11, the picture together with another post-processing shader,
  the sliders moved by hand.
