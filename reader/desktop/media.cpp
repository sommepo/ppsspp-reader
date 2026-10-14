// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <opus.h>
#include "reader/desktop/media.h"

namespace ReaderApp {

std::vector<qint16> PcmTools::Resample(const std::vector<qint16>& input, int channels,
                                       int from_rate, int to_rate) {
    if (channels <= 0 || from_rate <= 0 || to_rate <= 0) {
        return {};
    }
    const std::size_t in_frames = input.size() / static_cast<std::size_t>(channels);
    if (in_frames == 0 || from_rate == to_rate) {
        return std::vector<qint16>(input.begin(), input.begin() + in_frames * channels);
    }
    const std::size_t out_frames =
        static_cast<std::size_t>(static_cast<qint64>(in_frames) * to_rate / from_rate);
    std::vector<qint16> out(out_frames * channels);
    const double step = static_cast<double>(from_rate) / to_rate;
    for (std::size_t i = 0; i < out_frames; ++i) {
        const double position = static_cast<double>(i) * step;
        const std::size_t index =
            std::min(static_cast<std::size_t>(std::floor(position)), in_frames - 1);
        const std::size_t next = std::min(index + 1, in_frames - 1);
        const double fraction = position - static_cast<double>(index);
        for (int c = 0; c < channels; ++c) {
            const int a = input[index * channels + c];
            const int b = input[next * channels + c];
            out[i * channels + c] = static_cast<qint16>(a + (b - a) * fraction);
        }
    }
    return out;
}

std::vector<qint16> PcmTools::TrimTrailingSilence(const std::vector<qint16>& input, int channels,
                                                  int threshold, int padding_frames) {
    if (channels <= 0) {
        return {};
    }
    const qint64 frames = static_cast<qint64>(input.size()) / channels;
    qint64 last = -1;
    for (qint64 frame = frames - 1; frame >= 0 && last < 0; --frame) {
        for (int c = 0; c < channels; ++c) {
            if (std::abs(static_cast<int>(input[frame * channels + c])) > threshold) {
                last = frame;
                break;
            }
        }
    }
    if (last < 0) {
        return {};
    }
    const qint64 keep = std::min(last + 1 + padding_frames, frames);
    return std::vector<qint16>(input.begin(), input.begin() + keep * channels);
}

void PcmTools::FadeEdges(std::vector<qint16>& samples, int channels, int frames) {
    if (channels <= 0) {
        return;
    }
    const qint64 total = static_cast<qint64>(samples.size()) / channels;
    const qint64 n = std::min<qint64>(frames, total / 2);
    for (qint64 i = 0; i < n; ++i) {
        const float gain = static_cast<float>(i) / static_cast<float>(n);
        for (int c = 0; c < channels; ++c) {
            const qint64 head = i * channels + c;
            const qint64 tail = (total - 1 - i) * channels + c;
            samples[head] = static_cast<qint16>(samples[head] * gain);
            samples[tail] = static_cast<qint16>(samples[tail] * gain);
        }
    }
}

QRect Letterbox::Bounds(const QImage& source) {
    const QRect full(0, 0, source.width(), source.height());
    if (source.isNull() || source.width() <= 0 || source.height() <= 0) {
        return full;
    }
    const QImage image = source.format() == QImage::Format_RGB32 ||
                                 source.format() == QImage::Format_ARGB32
                             ? source
                             : source.convertToFormat(QImage::Format_RGB32);
    const int width = image.width();
    const int height = image.height();
    const auto row_black = [&image, width](int y) {
        const QRgb* line = reinterpret_cast<const QRgb*>(image.constScanLine(y));
        for (int x = 0; x < width; ++x) {
            if ((line[x] & 0x00FFFFFF) != 0) {
                return false;
            }
        }
        return true;
    };
    const auto column_black = [&image](int x, int top, int bottom) {
        for (int y = top; y < bottom; ++y) {
            const QRgb* line = reinterpret_cast<const QRgb*>(image.constScanLine(y));
            if ((line[x] & 0x00FFFFFF) != 0) {
                return false;
            }
        }
        return true;
    };
    int top = 0;
    while (top < height && row_black(top)) {
        ++top;
    }
    if (top == height) {
        return full;
    }
    int bottom = height;
    while (bottom > top && row_black(bottom - 1)) {
        --bottom;
    }
    int left = 0;
    while (left < width && column_black(left, top, bottom)) {
        ++left;
    }
    int right = width;
    while (right > left && column_black(right - 1, top, bottom)) {
        --right;
    }
    const QRect trimmed(left, top, right - left, bottom - top);
    const bool keeps_enough = static_cast<qint64>(trimmed.width()) * trimmed.height() * 10 >=
                              static_cast<qint64>(width) * height * 3;
    return keeps_enough ? trimmed : full;
}

QImage Letterbox::Trim(const QImage& image) {
    const QRect bounds = Bounds(image);
    if (bounds.width() == image.width() && bounds.height() == image.height()) {
        return image;
    }
    return image.copy(bounds);
}

// ---- Ogg Opus ----

namespace {

constexpr int OpusRate = 48000;
constexpr int FrameSamples = 960; // 20 ms
constexpr int PacketsPerPage = 50; // one second: well under the 255 segments a page can hold

const std::array<quint32, 256>& CrcTable() {
    static const std::array<quint32, 256> table = [] {
        std::array<quint32, 256> t{};
        for (quint32 i = 0; i < 256; ++i) {
            quint32 r = i << 24;
            for (int bit = 0; bit < 8; ++bit) {
                r = (r & 0x80000000U) ? (r << 1) ^ 0x04C11DB7U : r << 1;
            }
            t[i] = r;
        }
        return t;
    }();
    return table;
}

void PutLe(QByteArray& out, quint64 value, int bytes) {
    for (int i = 0; i < bytes; ++i) {
        out.append(static_cast<char>((value >> (8 * i)) & 0xFF));
    }
}

/// One Ogg page holding whole packets.
QByteArray Page(const QList<QByteArray>& packets, quint8 header_type, qint64 granule,
                quint32 serial, quint32 sequence) {
    QByteArray lacing;
    QByteArray body;
    for (const QByteArray& packet : packets) {
        qsizetype left = packet.size();
        while (left >= 255) {
            lacing.append(static_cast<char>(255));
            left -= 255;
        }
        lacing.append(static_cast<char>(left));
        body += packet;
    }
    QByteArray page;
    page.append("OggS", 4);
    page.append('\0'); // version
    page.append(static_cast<char>(header_type));
    PutLe(page, static_cast<quint64>(granule), 8);
    PutLe(page, serial, 4);
    PutLe(page, sequence, 4);
    PutLe(page, 0, 4); // checksum, filled in below
    page.append(static_cast<char>(lacing.size()));
    page += lacing;
    page += body;
    const quint32 crc = OggCrc(page);
    for (int i = 0; i < 4; ++i) {
        page[22 + i] = static_cast<char>((crc >> (8 * i)) & 0xFF);
    }
    return page;
}

} // namespace

quint32 OggCrc(const QByteArray& data) {
    const auto& table = CrcTable();
    quint32 crc = 0;
    for (const char byte : data) {
        crc = (crc << 8) ^ table[((crc >> 24) & 0xFF) ^ static_cast<quint8>(byte)];
    }
    return crc;
}

QByteArray EncodeOggOpusAt(const std::vector<qint16>& samples, int channels, int rate,
                           int bitrate) {
    if (rate == OggOpusRate) {
        return EncodeOggOpus(samples, channels, bitrate);
    }
    return EncodeOggOpus(PcmTools::Resample(samples, channels, rate, OggOpusRate), channels,
                         bitrate);
}

QByteArray EncodeOggOpus(const std::vector<qint16>& samples, int channels, int bitrate) {
    if (channels != 1 && channels != 2) {
        return {};
    }
    int error = OPUS_OK;
    OpusEncoder* encoder = opus_encoder_create(OpusRate, channels, OPUS_APPLICATION_AUDIO, &error);
    if (!encoder || error != OPUS_OK) {
        return {};
    }
    opus_encoder_ctl(encoder, OPUS_SET_BITRATE(bitrate));
    opus_int32 lookahead = 0;
    opus_encoder_ctl(encoder, OPUS_GET_LOOKAHEAD(&lookahead));

    const quint32 serial = 0x45524452; // any number: a file has one stream
    quint32 sequence = 0;
    QByteArray out;

    QByteArray head;
    head.append("OpusHead", 8);
    head.append('\1'); // version
    head.append(static_cast<char>(channels));
    PutLe(head, static_cast<quint64>(lookahead), 2); // pre-skip
    PutLe(head, OpusRate, 4);                        // the rate of what was put in
    PutLe(head, 0, 2);                               // output gain
    head.append('\0');                               // channel mapping family: mono or stereo
    out += Page({head}, 0x02, 0, serial, sequence++);

    QByteArray tags;
    tags.append("OpusTags", 8);
    const QByteArray vendor = "PPSSPP Reader";
    PutLe(tags, static_cast<quint64>(vendor.size()), 4);
    tags += vendor;
    PutLe(tags, 0, 4); // no comments
    out += Page({tags}, 0x00, 0, serial, sequence++);

    const qint64 total_frames = static_cast<qint64>(samples.size()) / channels;
    // The decoder throws the encoder's lookahead away at the start, so that much is fed in
    // again at the end for the last of the sound to come out.
    const qint64 to_encode = total_frames + lookahead;
    const qint64 packets = (to_encode + FrameSamples - 1) / FrameSamples;
    std::vector<qint16> frame(static_cast<std::size_t>(FrameSamples) * channels);
    std::array<unsigned char, 4000> packet{};
    QList<QByteArray> pending;
    int segments = 0;
    qint64 position = 0; // samples encoded so far, the lookahead included
    for (qint64 index = 0; index < packets; ++index) {
        std::fill(frame.begin(), frame.end(), qint16{0});
        const qint64 available = std::clamp<qint64>(total_frames - position, 0, FrameSamples);
        if (available > 0) {
            std::copy_n(samples.begin() + position * channels, available * channels,
                        frame.begin());
        }
        const opus_int32 size = opus_encode(encoder, frame.data(), FrameSamples, packet.data(),
                                            static_cast<opus_int32>(packet.size()));
        if (size < 0) {
            opus_encoder_destroy(encoder);
            return {};
        }
        // A page holds at most 255 segments of at most 255 bytes.
        const int needed = size / 255 + 1;
        if (!pending.isEmpty() && (segments + needed > 255 || pending.size() >= PacketsPerPage)) {
            out += Page(pending, 0x00, std::min(position, to_encode), serial, sequence++);
            pending.clear();
            segments = 0;
        }
        pending.append(QByteArray(reinterpret_cast<const char*>(packet.data()), size));
        segments += needed;
        position += FrameSamples;
    }
    // The last page says where the sound truly ends, which tells the decoder how much of the
    // last packet is padding.
    out += Page(pending, 0x04, to_encode, serial, sequence++);
    opus_encoder_destroy(encoder);
    return out;
}

} // namespace ReaderApp
