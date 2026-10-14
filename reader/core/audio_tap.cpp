// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the Eden Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstring>
#include "reader/core/audio_tap.h"

namespace Reader {

AudioTap& AudioTap::Instance() {
    static AudioTap instance;
    return instance;
}

void AudioTap::SetEnabled(bool enable, u32 seconds) {
    std::scoped_lock lock{mutex};
    if (enable) {
        const std::size_t wanted = static_cast<std::size_t>(SampleRate) * std::max<u32>(seconds, 1);
        if (wanted != capacity) {
            capacity = wanted;
            ring.assign(capacity * 2, 0);
            position = 0;
        }
    } else {
        capacity = 0;
        position = 0;
        ring.clear();
        ring.shrink_to_fit();
    }
    enabled.store(enable, std::memory_order_relaxed);
}

void AudioTap::Push(const s16* samples, std::size_t frames) {
    std::scoped_lock lock{mutex};
    if (capacity == 0) {
        return;
    }
    if (frames > capacity) {
        samples += (frames - capacity) * 2;
        position += frames - capacity;
        frames = capacity;
    }
    const std::size_t offset = static_cast<std::size_t>(position % capacity);
    const std::size_t first = std::min(frames, capacity - offset);
    std::memcpy(&ring[offset * 2], samples, first * 2 * sizeof(s16));
    if (first < frames) {
        std::memcpy(ring.data(), samples + first * 2, (frames - first) * 2 * sizeof(s16));
    }
    position += frames;
}

void AudioTap::PushStream(const void* stream, const s16* samples, std::size_t count,
                          u32 channels) {
    if (!enabled.load(std::memory_order_relaxed) || channels == 0) [[likely]] {
        return;
    }
    const s16* stereo = samples;
    std::size_t frames = count / channels;
    {
        std::scoped_lock lock{mutex};
        if (stream != stream_kept) {
            // Another stream: it takes over once the kept one has been silent for a while.
            if (stream_kept && ++stream_idle < 64) {
                return;
            }
            stream_kept = stream;
        }
        stream_idle = 0;
        if (channels != 2) {
            mixdown.resize(frames * 2);
            for (std::size_t i = 0; i < frames; ++i) {
                const s16* in = samples + i * channels;
                float left = in[0];
                float right = channels > 1 ? in[1] : in[0];
                if (channels >= 6) {
                    // Front left, front right, centre, low frequency, back left, back right.
                    left += 0.596f * in[2] + 0.354f * in[3] + 0.707f * in[4];
                    right += 0.596f * in[2] + 0.354f * in[3] + 0.707f * in[5];
                }
                mixdown[i * 2] = static_cast<s16>(std::clamp(left, -32768.0f, 32767.0f));
                mixdown[i * 2 + 1] = static_cast<s16>(std::clamp(right, -32768.0f, 32767.0f));
            }
            stereo = mixdown.data();
        }
    }
    Push(stereo, frames);
}

u64 AudioTap::Position() const {
    std::scoped_lock lock{mutex};
    return position;
}

std::vector<s16> AudioTap::Read(u64 start, u64 end, u64& actual_start) const {
    std::scoped_lock lock{mutex};
    actual_start = start;
    if (capacity == 0) {
        return {};
    }
    end = std::min(end, position);
    const u64 oldest = position > capacity ? position - capacity : 0;
    start = std::max(start, oldest);
    actual_start = start;
    if (start >= end) {
        return {};
    }
    const std::size_t frames = static_cast<std::size_t>(end - start);
    std::vector<s16> out(frames * 2);
    const std::size_t offset = static_cast<std::size_t>(start % capacity);
    const std::size_t first = std::min(frames, capacity - offset);
    std::memcpy(out.data(), &ring[offset * 2], first * 2 * sizeof(s16));
    if (first < frames) {
        std::memcpy(out.data() + first * 2, ring.data(), (frames - first) * 2 * sizeof(s16));
    }
    return out;
}

} // namespace Reader
