// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the Eden Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <mutex>
#include <vector>
#include "reader/core/types.h"

namespace Reader {

/**
 * Keeps the most recent emulated audio output so the audio of a text line can be attached to a
 * flashcard after the fact. Stores interleaved stereo 16-bit frames at the console's native rate.
 * Disabled (and holding no memory) until a frontend turns it on.
 */
class AudioTap {
public:
    static constexpr u32 SampleRate = 44100; ///< What the PSP mixes its output at
    static constexpr u32 DefaultSeconds = 90;

    static AudioTap& Instance();

    /// Turns capture on or off. Turning it on keeps `seconds` of history.
    void SetEnabled(bool enabled, u32 seconds = DefaultSeconds);
    [[nodiscard]] bool IsEnabled() const {
        return enabled.load(std::memory_order_relaxed);
    }

    /// Appends interleaved stereo frames. Called from the thread that produces audio.
    void Push(const s16* samples, std::size_t frames);

    /**
     * Appends what an output stream of the console is about to play: `count` interleaved
     * samples of `channels` channels (two, or six in the console's surround order, which are
     * mixed down). A game has one stream as a rule; when there are several, the one that is
     * playing is kept and the others are left out rather than woven into it.
     */
    void PushStream(const void* stream, const s16* samples, std::size_t count, u32 channels);

    /// Number of frames pushed since capture was enabled.
    [[nodiscard]] u64 Position() const;

    /// Copies the frames in [start, end) that are still held. `actual_start` receives the
    /// position of the first returned frame, which is later than `start` when older audio has
    /// already been overwritten.
    [[nodiscard]] std::vector<s16> Read(u64 start, u64 end, u64& actual_start) const;

private:
    AudioTap() = default;

    std::atomic<bool> enabled{false};
    mutable std::mutex mutex;
    std::vector<s16> ring;  ///< capacity_frames * 2 samples
    const void* stream_kept{}; ///< The stream whose audio is being kept
    u64 stream_idle{};         ///< Pushes of other streams since it last played
    std::vector<s16> mixdown;
    std::size_t capacity{}; ///< in frames
    u64 position{};         ///< total frames pushed
};

} // namespace Reader
