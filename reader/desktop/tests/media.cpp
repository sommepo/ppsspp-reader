// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cmath>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include "reader/desktop/media.h"
#include "reader/desktop/tests/test_util.h"

using namespace ReaderApp;

TEST_CASE("PcmTools: resampling keeps the length in time", "[reader][media]") {
    const std::vector<qint16> input{0, 0, 100, -100, 200, -200, 300, -300};
    REQUIRE(PcmTools::Resample(input, 2, 48000, 48000) == input);
    const std::vector<qint16> doubled = PcmTools::Resample(input, 2, 24000, 48000);
    REQUIRE(doubled.size() == 16);
    REQUIRE(doubled[0] == 0);
    REQUIRE(doubled[2] == 50);
    REQUIRE(doubled[3] == -50);
    REQUIRE(doubled[4] == 100);
    REQUIRE(PcmTools::Resample(input, 2, 48000, 24000) ==
            std::vector<qint16>{0, 0, 200, -200});
    REQUIRE(PcmTools::Resample({}, 2, 48000, 24000).empty());
}

TEST_CASE("PcmTools: trailing silence is dropped, with a little kept", "[reader][media]") {
    std::vector<qint16> input(20, 0);
    input[4] = 5000; // frame 2, left
    input[9] = -900; // frame 4, right
    REQUIRE(PcmTools::TrimTrailingSilence(input, 2, 180, 2).size() == 14); // frames 0..6
    REQUIRE(PcmTools::TrimTrailingSilence(input, 2, 180, 99).size() == 20);
    REQUIRE(PcmTools::TrimTrailingSilence(input, 2, 6000, 2).empty());
}

TEST_CASE("PcmTools: the edges are faded", "[reader][media]") {
    std::vector<qint16> samples(16, 1000);
    PcmTools::FadeEdges(samples, 2, 2);
    REQUIRE(samples[0] == 0);
    REQUIRE(samples[2] == 500);
    REQUIRE(samples[4] == 1000);
    REQUIRE(samples[15] == 0);
    REQUIRE(samples[13] == 500);
    REQUIRE(samples[11] == 1000);
}

TEST_CASE("Letterbox: black bars are cut off, a dark picture is left whole", "[reader][media]") {
    QImage image(100, 60, QImage::Format_RGB32);
    image.fill(Qt::black);
    for (int y = 10; y < 50; ++y) {
        for (int x = 20; x < 90; ++x) {
            image.setPixel(x, y, qRgb(30, 60, 90));
        }
    }
    REQUIRE(Letterbox::Bounds(image) == QRect(20, 10, 70, 40));
    REQUIRE(Letterbox::Trim(image).size() == QSize(70, 40));

    QImage dark(100, 60, QImage::Format_RGB32);
    dark.fill(Qt::black);
    dark.setPixel(50, 30, qRgb(255, 255, 255));
    REQUIRE(Letterbox::Bounds(dark) == QRect(0, 0, 100, 60));
    dark.fill(Qt::black);
    REQUIRE(Letterbox::Bounds(dark) == QRect(0, 0, 100, 60));
    REQUIRE(Letterbox::Trim(dark).size() == QSize(100, 60));
}

TEST_CASE("EncodeOggOpus: a clip becomes a well-formed Ogg Opus stream", "[reader][media]") {
    // One second of a 440 Hz tone, stereo.
    std::vector<qint16> samples(48000 * 2);
    for (int i = 0; i < 48000; ++i) {
        const auto value = static_cast<qint16>(8000 * std::sin(i * 2.0 * 3.141592653589793 * 440 / 48000));
        samples[i * 2] = value;
        samples[i * 2 + 1] = value;
    }
    const QByteArray ogg = EncodeOggOpus(samples, 2);
    REQUIRE(ogg.size() > 2000);
    REQUIRE(ogg.size() < 40000); // a tenth of the 192,000 bytes that went in, or less

    // Walk the pages: each begins with OggS, carries a sound checksum and follows the last.
    int pages = 0;
    qsizetype at = 0;
    qint64 last_granule = -1;
    quint8 last_type = 0;
    while (at < ogg.size()) {
        REQUIRE(ogg.mid(at, 4) == QByteArray("OggS"));
        const quint8 type = static_cast<quint8>(ogg.at(at + 5));
        qint64 granule = 0;
        for (int i = 7; i >= 0; --i) {
            granule = (granule << 8) | static_cast<quint8>(ogg.at(at + 6 + i));
        }
        quint32 sequence = 0;
        quint32 stored = 0;
        for (int i = 3; i >= 0; --i) {
            sequence = (sequence << 8) | static_cast<quint8>(ogg.at(at + 18 + i));
            stored = (stored << 8) | static_cast<quint8>(ogg.at(at + 22 + i));
        }
        REQUIRE(sequence == static_cast<quint32>(pages));
        const int segments = static_cast<quint8>(ogg.at(at + 26));
        qsizetype body = 0;
        for (int i = 0; i < segments; ++i) {
            body += static_cast<quint8>(ogg.at(at + 27 + i));
        }
        const qsizetype size = 27 + segments + body;
        QByteArray page = ogg.mid(at, size);
        for (int i = 0; i < 4; ++i) {
            page[22 + i] = 0;
        }
        REQUIRE(OggCrc(page) == stored);
        if (pages == 0) {
            REQUIRE(type == 0x02);
            REQUIRE(ogg.mid(at + 28, 8) == QByteArray("OpusHead"));
            REQUIRE(ogg.at(at + 28 + 9) == 2); // channels
        } else if (pages == 1) {
            REQUIRE(ogg.mid(at + 28, 8) == QByteArray("OpusTags"));
        }
        last_granule = granule;
        last_type = type;
        at += size;
        ++pages;
    }
    REQUIRE(at == ogg.size());
    REQUIRE(pages >= 3);
    REQUIRE(last_type == 0x04);
    // The stream is as long as what went in, plus what the decoder skips at the start.
    REQUIRE(last_granule >= 48000);
    REQUIRE(last_granule < 48000 + 1000);

    REQUIRE(EncodeOggOpus(samples, 3).isEmpty());
    // Nothing in: still a stream that ends properly.
    REQUIRE(EncodeOggOpus({}, 2).contains("OpusHead"));
}

TEST_CASE("EncodeOggOpusAt: a clip at the PSP's rate lasts as long as it did", "[reader][media]") {
    // Two seconds of a tone at 44.1 kHz, stereo. Taken for 48 kHz as it is, it would last
    // 1.84 seconds and sound a tenth too high (a card made on Windows did).
    const int rate = 44100;
    std::vector<qint16> samples(static_cast<std::size_t>(rate) * 2 * 2);
    for (int i = 0; i < rate * 2; ++i) {
        const auto value = static_cast<qint16>(8000 * std::sin(i * 2.0 * 3.141592653589793 * 440 / rate));
        samples[i * 2] = value;
        samples[i * 2 + 1] = value;
    }
    const auto seconds = [](const QByteArray& ogg) {
        const qsizetype last = ogg.lastIndexOf("OggS");
        REQUIRE(last >= 0);
        qint64 granule = 0;
        for (int i = 7; i >= 0; --i) {
            granule = (granule << 8) | static_cast<quint8>(ogg.at(last + 6 + i));
        }
        // The granule counts 48 kHz samples, the encoder's lead-in among them.
        return static_cast<double>(granule) / OggOpusRate;
    };
    const double there = seconds(EncodeOggOpusAt(samples, 2, rate));
    CHECK(there > 1.98);
    CHECK(there < 2.05);
    // The same samples said to be at 48 kHz already are that much shorter.
    const double wrong = seconds(EncodeOggOpusAt(samples, 2, OggOpusRate));
    CHECK(wrong > 1.82);
    CHECK(wrong < 1.87);
}
