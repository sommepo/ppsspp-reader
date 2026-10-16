// SPDX-FileCopyrightText: Reader fork contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// The tube's frame around the game's picture (reader/qt/reader_crt_frame.h has what it looks
// like and why): the drawing itself, free of Qt, so that the desktop's reader window and the
// Android apps (crt_jni.h) draw the same frame. Header only, so that no fork's build has to
// list it. The same in every fork (tools/reader/crt.py sync).

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>
#include "reader/core/crt.h"

namespace Reader::Crt::Frame {

struct V3 {
    float r, g, b;
};
inline V3 operator+(V3 a, V3 b) {
    return {a.r + b.r, a.g + b.g, a.b + b.b};
}
inline V3 operator-(V3 a, V3 b) {
    return {a.r - b.r, a.g - b.g, a.b - b.b};
}
inline V3 operator*(V3 a, float s) {
    return {a.r * s, a.g * s, a.b * s};
}
inline V3 Mix(V3 a, V3 b, float t) {
    return a + (b - a) * t;
}
inline float Saturate(float v) {
    return std::clamp(v, 0.f, 1.f);
}
inline float Smooth(float e0, float e1, float x) {
    const float t = Saturate((x - e0) / (e1 - e0));
    return t * t * (3 - 2 * t);
}
inline float Fract(float v) {
    return v - std::floor(v);
}
inline float Hash(float x, float y) {
    x = Fract(x * 123.34f);
    y = Fract(y * 456.21f);
    const float d = x * (x + 45.32f) + y * (y + 45.32f);
    return Fract((x + d) * (y + d));
}
/// Smooth value noise: the case's stains, grain and dust, never in a grid.
inline float Noise(float x, float y) {
    const float ix = std::floor(x), iy = std::floor(y);
    const float fx = x - ix, fy = y - iy;
    const float ux = fx * fx * (3 - 2 * fx), uy = fy * fy * (3 - 2 * fy);
    const float top = Hash(ix, iy) + (Hash(ix + 1, iy) - Hash(ix, iy)) * ux;
    const float bottom = Hash(ix, iy + 1) + (Hash(ix + 1, iy + 1) - Hash(ix, iy + 1)) * ux;
    return top + (bottom - top) * uy;
}

/// The plastic of each colour: as new, what the years turn it to, and how much they may.
struct Plastic {
    V3 fresh;
    V3 aged;
    float ageing;
};
inline Plastic PlasticOf(int colour) {
    switch (colour) {
    case 2: // black
        return {{0.058f, 0.058f, 0.064f}, {0.085f, 0.083f, 0.080f}, 0.35f};
    case 3: // beige, the putty of a 90s office monitor
        return {{0.67f, 0.64f, 0.57f}, {0.76f, 0.70f, 0.48f}, 1.0f};
    default: // charcoal
        return {{0.150f, 0.150f, 0.158f}, {0.180f, 0.174f, 0.166f}, 0.5f};
    }
}

struct FrameLook {
    int colour = 0;
    bool dark = false;
    float wear = 0.5f;
    float warp = 0.f;
    float corners = 0.f;
};

inline FrameLook FrameLookNow() {
    FrameLook look;
    if (!Reader::Crt::Enabled()) {
        return look;
    }
    look.colour = static_cast<int>(std::lround(Reader::Crt::Get("frame")));
    look.dark = Reader::Crt::Get("frame_room") >= 0.5f;
    look.wear = Reader::Crt::Get("frame_wear");
    look.warp = Reader::Crt::Get("warp");
    look.corners = Reader::Crt::Get("corners");
    return look;
}

/// Rows `first` to `last` of the frame, into `out` (W by H words, a row after another): michadame's fs_retro_frame, sections 3 to 11, with the
/// opening being the CRT shader's own tube edge and the picture's light a steady soft one.
inline void DrawFrameRows(std::uint32_t* out, int W, int H, float pic_left, float pic_top, float pw, float ph,
                          const FrameLook& look, int first, int last, bool premultiplied) {
    const float aspect = pw / ph;
    const float half = ph / 2; // the shader's unit: half the picture's height
    const float min_dim = std::min(pw, ph);
    const float scale = std::max(min_dim / 1080.f, 0.5f);
    const Plastic plastic_kind = PlasticOf(look.colour);
    const float wear = look.wear;
    const bool dark = look.dark;

    // The opening: the tube's edge, a little inside it, with corners at least so round.
    const float tube_corner = std::max(look.corners * 0.16f, 0.0015f);
    const float corner = std::max(tube_corner, 0.035f);
    const float inset = 0.012f; // of `half`
    const float shadow_w = 34.f * scale;
    const float gasket_w = 4.5f * scale;
    const float bevel_base = 62.f * scale;
    const float hal_lambda = 11.f * scale;
    const V3 light_dir = [] {
        const float x = -0.28f, y = 0.65f, z = 0.70f;
        const float n = std::sqrt(x * x + y * y + z * z);
        return V3{x / n, y / n, z / n};
    }();
    // The picture's light where it meets the frame: a steady, soft, slightly warm grey.
    const V3 picture_light{0.36f, 0.35f, 0.33f};
    const float ambient_glow = 0.6f;

    for (int y = first; y < last; ++y) {
        std::uint32_t* line = out + static_cast<std::size_t>(y) * static_cast<std::size_t>(W);
        for (int x = 0; x < W; ++x) {
            const float fx = x + 0.5f, fy = y + 0.5f, up = H - fy; // `up`: gl_FragCoord.y
            // Where this is on the bulging glass, as the CRT shader reckons it.
            const float cx = (fx - pic_left) / pw * 2 - 1;
            const float cy = (fy - pic_top) / ph * 2 - 1;
            const float wx = cx * (1 + look.warp * 0.062f * cy * cy) * aspect;
            const float wy = cy * (1 + look.warp * 0.082f * cx * cx);
            const float qx = std::abs(wx) - (aspect - inset - corner);
            const float qy = std::abs(wy) - (1 - inset - corner);
            const float mqx = std::max(qx, 0.f), mqy = std::max(qy, 0.f);
            const float lmq = std::sqrt(mqx * mqx + mqy * mqy);
            const float d_ap = (std::min(std::max(qx, qy), 0.f) + lmq - corner) * half;
            float nx, ny;
            if (lmq > 0.0001f) {
                nx = mqx / lmq;
                ny = mqy / lmq;
            } else if (qx > qy) {
                nx = 1, ny = 0;
            } else {
                nx = 0, ny = 1;
            }
            nx = wx < 0 ? -nx : nx;
            ny = wy < 0 ? -ny : ny;

            // The recess's shadow on the glass, more from the lamp's side.
            const float falloff = Saturate((d_ap + shadow_w) / shadow_w);
            if (d_ap < -shadow_w) {
                line[x] = 0;
                continue;
            }
            const float top_bias = Saturate(-ny * 0.5f + 0.5f), left_bias = Saturate(-nx * 0.5f + 0.5f);
            const float shadow_alpha =
                dark ? 0.f : std::pow(falloff, 1.6f) * 0.65f * (0.65f + 0.35f * (top_bias * 0.65f + left_bias * 0.35f));

            // The recess: deep, sloping in, its bottom a shelf.
            const float bevel_w = bevel_base * (1 + Saturate(ny) * 0.25f);
            const float t_bevel = Saturate((d_ap - gasket_w) / std::max(bevel_w - gasket_w, 1.f));
            const float nbx = -nx * 0.62f, nby = ny * 0.62f;
            const V3 nb{nbx, nby, std::sqrt(std::max(1 - nbx * nbx - nby * nby, 0.01f))};
            const float diffuse = Saturate(nb.r * light_dir.r + nb.g * light_dir.g + nb.b * light_dir.b);
            const float cavity = 0.45f + 0.55f * std::pow(t_bevel, 0.75f);
            const float bevel_light = dark ? 0.12f + diffuse * 0.12f : (0.35f + diffuse * 0.85f) * cavity;
            const float shelf = Saturate(ny) * t_bevel * t_bevel * (dark ? 0.18f : 0.35f);
            const float crest = std::exp(-0.5f * std::pow((t_bevel - 0.97f) / 0.028f, 2.f));
            const V3 crest_col = V3{0.14f, 0.14f, 0.11f} * (crest * Saturate(nx * 0.40f - ny * 0.75f) * (dark ? 0.45f : 1.f));

            // The picture's light on the seal and the recess.
            const float pl = 0.2126f * picture_light.r + 0.7152f * picture_light.g + 0.0722f * picture_light.b;
            const float glow_resp = dark ? std::clamp(ambient_glow / 0.825f, 0.f, 1.25f) : Saturate(ambient_glow * 0.9f);
            const V3 contact = picture_light * ((dark ? 0.45f : 0.25f) * std::exp(-std::max(d_ap, 0.f) / hal_lambda) * glow_resp);
            V3 glow{0, 0, 0};
            if (d_ap > 0 && d_ap < bevel_w) {
                const float energy = std::clamp(0.60f + 1.20f * pl, 0.60f, 1.80f);
                glow = Mix(V3{pl, pl, pl}, picture_light, 0.4f) *
                       (energy * (dark ? 0.65f : 0.95f) * ambient_glow * std::pow(1 - t_bevel, 2.2f));
            }

            // The case's plastic, aged where the lamp and the years reached it.
            const float n_coarse = Noise(fx / 220, up / 220), n_med = Noise(fx / 65, up / 65);
            const float n_fine = Noise(fx / 18, up / 18), n_micro = Noise(fx / 5.5f, up / 5.5f);
            const float stain = n_coarse * 0.5f + n_med * 0.35f + n_fine * 0.15f;
            const float exposure = Saturate(std::pow(up / H, 1.3f) * 0.75f + (1 - fx / W) * 0.25f);
            const float age = plastic_kind.ageing * wear * 2;
            V3 plastic = Mix(plastic_kind.fresh, plastic_kind.aged, Saturate((exposure * 0.45f + stain * 0.35f + 0.10f) * age));
            const V3 tar = Mix(plastic_kind.fresh, V3{0.52f, 0.40f, 0.22f}, plastic_kind.ageing);
            plastic = Mix(plastic, tar, Saturate((stain - 0.40f) * 2) * (0.5f + 0.5f * Saturate(ny)) * 0.28f * age);
            const V3 soot = plastic_kind.fresh * 0.30f;
            const float seam = std::exp(-0.5f * std::pow(std::abs(d_ap - gasket_w) / 3.2f, 2.f)) * 0.55f;
            const float cowl_grime = std::clamp((1 - t_bevel) * 0.35f + Saturate(ny) * std::pow(1 - t_bevel, 2.f) * 0.25f, 0.f, 0.6f);
            const float corner_grime = Saturate(lmq / corner) * (1 - t_bevel) * 0.35f;
            const float grime = (dark ? std::clamp(seam * 0.2f + (1 - t_bevel) * 0.1f, 0.f, 0.3f)
                                      : std::clamp(seam + cowl_grime * (0.6f + 0.4f * n_fine) + corner_grime, 0.f, 0.85f)) *
                                std::min(wear * 2, 1.f);
            const float corner_dist = std::hypot(std::min(fx, W - fx), up);
            const float handling = std::exp(-0.5f * std::pow(corner_dist / (140 * scale), 2.f)) * (0.12f + 0.08f * n_med);
            plastic = plastic * (1 - std::clamp(handling * wear * 2, 0.f, 0.35f) * 0.4f);
            // The pebbled surface of moulded ABS.
            const float g1 = Noise(fx / 1.3f, up / 1.3f), g2 = Noise(fx * 0.31f + up * 0.21f, -fx * 0.21f + up * 0.31f);
            const float shade = 0.2126f * plastic.r + 0.7152f * plastic.g + 0.0722f * plastic.b;
            const float grain = (((g1 + g2) * 0.5f - 0.5f) * 0.07f + (n_micro - 0.5f) * 0.06f) * (0.35f + shade);
            plastic = V3{Saturate(plastic.r + grain), Saturate(plastic.g + grain), Saturate(plastic.b + grain * 0.9f)};

            const float edge = std::min(std::min(fx, W - fx), std::min(up, H - up));
            V3 chassis = plastic * (0.75f + 0.25f * std::sqrt(Saturate(edge / (22 * scale))));
            if (dark) {
                chassis = chassis * 0.022f;
            }
            const V3 bevel_plastic = dark ? plastic * 0.22f : plastic;
            const V3 surface = Mix(bevel_plastic * ((bevel_light + shelf) * (0.75f + 0.25f * t_bevel)) + crest_col,
                                   dark ? soot * 0.2f : soot, dark ? grime * 0.15f : grime);
            const V3 reflectance = dark ? plastic * 0.75f + V3{0.25f, 0.25f, 0.25f} : plastic * 0.85f + V3{0.15f, 0.15f, 0.15f};
            const V3 lit_glow{glow.r * reflectance.r, glow.g * reflectance.g, glow.b * reflectance.b};
            V3 frame = Mix(surface + lit_glow + contact, chassis, Smooth(bevel_w - 1.2f, bevel_w + 1.2f, d_ap));

            // Dust settled on the recess's bottom shelf.
            if (ny > 0.15f && d_ap > gasket_w - 0.5f && d_ap < bevel_w && wear > 0) {
                const float inlet = Saturate((d_ap - gasket_w) / 2) * Saturate((bevel_w - d_ap) / 2);
                const float gravity = std::pow(Saturate(ny), 1.9f);
                const float profile = Saturate(t_bevel / 0.09f) * std::pow(Saturate((1 - t_bevel) / 0.16f), 0.55f);
                const float drift = Noise(fx / 40, up / 40), silt = Noise((fx + 85.3f) / 12, (up + 32.7f) / 12);
                const float rx = fx * 0.866f - up * 0.5f, ry = fx * 0.5f + up * 0.866f;
                const float density = inlet * gravity * profile * (0.65f + 0.25f * drift + 0.10f * silt) * 1.6f * wear * 2;
                if (density > 0.002f) {
                    const float gx = Noise((rx + 0.8f) / 3.2f, ry / 3.2f) - Noise((rx - 0.8f) / 3.2f, ry / 3.2f);
                    const float gy = Noise(rx / 3.2f, (ry + 0.8f) / 3.2f) - Noise(rx / 3.2f, (ry - 0.8f) / 3.2f);
                    V3 nd{nb.r + gx * 0.4f, nb.g + gy * 0.4f, nb.b};
                    const float nn = std::sqrt(nd.r * nd.r + nd.g * nd.g + nd.b * nd.b);
                    nd = nd * (1 / nn);
                    const float lit = Saturate(nd.r * light_dir.r + nd.g * light_dir.g + nd.b * light_dir.b);
                    const float dust_ao = 0.45f + 0.55f * std::pow(t_bevel, 0.55f);
                    const V3 albedo = V3{0.70f, 0.68f, 0.63f} + V3{1, 1, 1} * ((silt - 0.5f) * 0.05f);
                    const V3 dust = albedo * ((dark ? 0.06f + 0.08f * lit : 0.40f + 0.60f * lit) * dust_ao);
                    const float grains = Noise(rx / 3.2f, ry / 3.2f);
                    frame = Mix(frame, dust, dark ? std::clamp(density * 0.75f, 0.f, 0.85f)
                                                  : std::clamp(density * (0.8f + 0.2f * grains), 0.f, 0.92f));
                }
            }

            // The rubber seal along the glass.
            const V3 seal = V3{0.038f, 0.038f, 0.042f} * (dark ? 0.6f : 1.f) + contact;
            frame = Mix(seal, frame, Smooth(gasket_w - 0.75f, gasket_w + 0.75f, d_ap));

            // Over the glass only the shadow; the frame itself is solid.
            const float coverage = Smooth(-0.75f, 0.75f, d_ap);
            const float alpha = coverage + shadow_alpha * (1 - coverage);
            // 0xAARRGGBB, premultiplied (Qt) or straight (Android's bitmaps); the shadow is black.
            const int a = static_cast<int>(Saturate(alpha) * 255.f + 0.5f);
            const float un = premultiplied || alpha <= 0.0001f ? 1.f : 1.f / Saturate(alpha);
            const int r = std::min(static_cast<int>(Saturate(frame.r * coverage * un) * 255.f + 0.5f), premultiplied ? a : 255);
            const int g = std::min(static_cast<int>(Saturate(frame.g * coverage * un) * 255.f + 0.5f), premultiplied ? a : 255);
            const int b = std::min(static_cast<int>(Saturate(frame.b * coverage * un) * 255.f + 0.5f), premultiplied ? a : 255);
            line[x] = (static_cast<std::uint32_t>(a) << 24) | (static_cast<std::uint32_t>(r) << 16) |
                      (static_cast<std::uint32_t>(g) << 8) | static_cast<std::uint32_t>(b);
        }
    }
}

/// Whether a frame is wanted at all: the CRT picture on and a frame chosen.
inline bool Wanted() {
    return Reader::Crt::Enabled() && std::lround(Reader::Crt::Get("frame")) > 0;
}

/// The frame for a W by H screen around the picture at (left, top, width, height), as the CRT
/// settings say now: W x H words 0xAARRGGBB, or empty when none is chosen. Drawn on a few
/// threads; a few hundred milliseconds at a large size.
inline std::vector<std::uint32_t> Draw(int W, int H, float left, float top, float width, float height,
                                       bool premultiplied, const FrameLook& look = FrameLookNow()) {
    if (look.colour <= 0 || W <= 0 || H <= 0 || width < 16 || height < 16) {
        return {};
    }
    std::vector<std::uint32_t> out(static_cast<std::size_t>(W) * static_cast<std::size_t>(H));
    const int threads = std::clamp(static_cast<int>(std::thread::hardware_concurrency()) / 2, 1, 6);
    std::vector<std::thread> workers;
    for (int i = 0; i < threads; ++i) {
        const int first = H * i / threads, last = H * (i + 1) / threads;
        workers.emplace_back([&out, W, H, left, top, width, height, look, first, last, premultiplied] {
            DrawFrameRows(out.data(), W, H, left, top, width, height, look, first, last, premultiplied);
        });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }
    return out;
}

} // namespace Reader::Crt::Frame
