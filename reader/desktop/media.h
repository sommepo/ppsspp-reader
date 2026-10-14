// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <vector>
#include <QByteArray>
#include <QImage>
#include <QRect>

namespace ReaderApp {

/// Helpers for interleaved 16-bit PCM.
namespace PcmTools {
/**
 * Resamples interleaved PCM with linear interpolation. Good enough for speech taken from a
 * console's mix, and cheap.
 */
[[nodiscard]] std::vector<qint16> Resample(const std::vector<qint16>& input, int channels,
                                           int from_rate, int to_rate);
/**
 * Drops near-silent audio from the end, keeping `padding_frames` after the last frame louder
 * than `threshold`. Empty when everything is silent.
 */
[[nodiscard]] std::vector<qint16> TrimTrailingSilence(const std::vector<qint16>& input,
                                                      int channels, int threshold,
                                                      int padding_frames);
/// Fades the first and last `frames` frames to avoid clicks at the clip's edges.
void FadeEdges(std::vector<qint16>& samples, int channels, int frames);
} // namespace PcmTools

/// Removes the black bars the screen layout leaves around the game picture.
namespace Letterbox {
/**
 * The picture without its pure-black border rows and columns. A picture that is mostly black
 * (a fade, a dark scene) is left whole rather than cropped to a sliver.
 */
[[nodiscard]] QRect Bounds(const QImage& image);
/// `image` cropped to Bounds; the same picture when there is nothing to remove.
[[nodiscard]] QImage Trim(const QImage& image);
} // namespace Letterbox

/**
 * Encodes interleaved 16-bit stereo or mono PCM at 48 kHz as an Ogg Opus file: what Anki on a
 * computer and AnkiDroid both play, at a tenth of the size of the same clip uncompressed.
 * Empty when the encoder cannot be set up.
 */
[[nodiscard]] QByteArray EncodeOggOpus(const std::vector<qint16>& samples, int channels,
                                       int bitrate = 96000);
/// The rate EncodeOggOpus takes its samples at.
constexpr int OggOpusRate = 48000;
/**
 * The same for PCM at another rate, brought to 48 kHz first. The PSP mixes at 44.1 kHz: its
 * samples handed to EncodeOggOpus as they are play a tenth too fast and too high.
 */
[[nodiscard]] QByteArray EncodeOggOpusAt(const std::vector<qint16>& samples, int channels,
                                         int rate, int bitrate = 96000);

/// The checksum Ogg pages carry. For tests of the container.
[[nodiscard]] quint32 OggCrc(const QByteArray& data);

} // namespace ReaderApp
