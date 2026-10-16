// SPDX-FileCopyrightText: Reader fork contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cmath>
#include <thread>
#include <vector>
#include <QCoreApplication>
#include <QMetaObject>
#include "reader/core/crt.h"
#include "reader/core/crt_frame.h"
#include "reader/qt/reader_crt_frame.h"

namespace ReaderUi {

namespace {

using Look = Reader::Crt::Frame::FrameLook;

Look LookNow() {
    return Reader::Crt::Frame::FrameLookNow();
}

/// The frame as an image: the shared drawing (reader/core/crt_frame.h), premultiplied.
QImage DrawLook(QSize size, QRectF picture, const Look& look) {
    std::vector<std::uint32_t> pixels = Reader::Crt::Frame::Draw(
        size.width(), size.height(), static_cast<float>(picture.left()), static_cast<float>(picture.top()),
        static_cast<float>(picture.width()), static_cast<float>(picture.height()), true, look);
    if (pixels.empty()) {
        return {};
    }
    QImage out(size, QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < size.height(); ++y) {
        std::copy_n(pixels.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(size.width()),
                    size.width(), reinterpret_cast<std::uint32_t*>(out.scanLine(y)));
    }
    return out;
}

} // namespace

CrtFrame::CrtFrame() : generation(std::make_shared<std::atomic<unsigned>>(0)) {}

CrtFrame::~CrtFrame() {
    // A frame still being made is thrown away when it is done.
    generation->fetch_add(1);
}

bool CrtFrame::Wanted() const {
    return Reader::Crt::Frame::Wanted();
}

QImage CrtFrame::Draw(QSize size, QRectF picture) {
    return DrawLook(size, picture, LookNow());
}

void CrtFrame::Update(QSize size, QRectF picture, qreal device_pixel_ratio) {
    const QSize pixels = (QSizeF(size) * device_pixel_ratio).toSize();
    const QRectF picture_pixels(picture.topLeft() * device_pixel_ratio, picture.size() * device_pixel_ratio);
    const unsigned revision = Reader::Crt::Revision();
    if (pixels == made_size && picture_pixels == made_picture && revision == made_revision) {
        return;
    }
    made_size = pixels;
    made_picture = picture_pixels;
    made_revision = revision;
    const Look look = LookNow();
    const unsigned wanted = generation->fetch_add(1) + 1;
    if (look.colour <= 0 || picture_pixels.isEmpty()) {
        image = QImage();
        if (ready) {
            ready();
        }
        return;
    }
    std::shared_ptr<std::atomic<unsigned>> current = generation;
    // The picture of the case is a few hundred milliseconds' work at a large size: not on the
    // thread that draws the window. The last one asked for is the one kept.
    std::thread([this, current, wanted, pixels, picture_pixels, look, device_pixel_ratio] {
        QImage made = DrawLook(pixels, picture_pixels, look);
        made.setDevicePixelRatio(device_pixel_ratio);
        if (current->load() != wanted) {
            return;
        }
        QMetaObject::invokeMethod(
            QCoreApplication::instance(),
            [this, current, wanted, made] {
                // On the window's thread: still the one wanted, and this frame still there.
                if (current->load() != wanted) {
                    return;
                }
                image = made;
                if (ready) {
                    ready();
                }
            },
            Qt::QueuedConnection);
    }).detach();
}

} // namespace ReaderUi
