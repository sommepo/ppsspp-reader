// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QVBoxLayout>
#include "reader/qt/audio_prompt.h"
#include "reader/qt/reader_overlay.h"

namespace ReaderUi {

namespace {

constexpr double TextPx = 14;

QString ButtonStyle(const QColor& colour, int pixels, int pad_x, int pad_y) {
    QColor edge = colour;
    edge.setAlpha(0xA0);
    return QStringLiteral("QPushButton { color: %1; background: %2; border: 1px solid %3; "
                          "border-radius: 4px; padding: %4px %5px; font-size: %6px; }"
                          "QPushButton:pressed { background: %7; }")
        .arg(colour.name(), Style::PopupBox.name(), edge.name(QColor::HexArgb))
        .arg(pad_y)
        .arg(pad_x)
        .arg(pixels)
        .arg(Style::PopupBoxPressed.name());
}

} // namespace

AudioPrompt::AudioPrompt(QWidget* parent)
    : QWidget(parent, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus |
                          Qt::NoDropShadowWindowHint |
                          // As the layer over the game (reader_overlay.cpp): kept from the window
                          // manager with READER_OVERLAY_UNMANAGED=1, so it stands where it is put.
                          (qEnvironmentVariableIntValue("READER_OVERLAY_UNMANAGED") != 0
                               ? Qt::X11BypassWindowManagerHint
                               : Qt::WindowFlags())) {
    setAttribute(Qt::WA_ShowWithoutActivating);
    setFocusPolicy(Qt::NoFocus);
    QFont base = font();
    base.setFamilies(Style::JapaneseFamilies());
    setFont(base);

    question = new QLabel(tr("Add the line's audio to the card?"), this);
    listen = new QPushButton(tr("Listen"), this);
    keep = new QPushButton(tr("With audio"), this);
    drop = new QPushButton(tr("Without"), this);
    for (QPushButton* button : {listen, keep, drop}) {
        button->setFocusPolicy(Qt::NoFocus);
        button->setCursor(Style::PointerCursor(Style::PointerSize(parent), devicePixelRatioF()));
    }
    auto* row = new QHBoxLayout;
    row->setSpacing(6);
    row->addWidget(listen);
    row->addWidget(keep);
    row->addWidget(drop);
    row->addStretch(1);
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(question);
    layout->addLayout(row);
    layout->setSizeConstraint(QLayout::SetFixedSize);

    // The question comes up where the pointer has just been: the press that asked for the
    // card, or its bounce, is not an answer.
    const auto guarded = [this](void (AudioPrompt::*answer)()) {
        return [this, answer] {
            if (shown.isValid() && shown.elapsed() >= GuardMs) {
                emit(this->*answer)();
            }
        };
    };
    connect(listen, &QPushButton::clicked, this, guarded(&AudioPrompt::ListenClicked));
    connect(keep, &QPushButton::clicked, this, guarded(&AudioPrompt::KeepClicked));
    connect(drop, &QPushButton::clicked, this, guarded(&AudioPrompt::DropClicked));
    Prepare(100);
}

void AudioPrompt::Prepare(int scale) {
    const double factor = std::clamp(scale, 50, 250) / 100.0;
    const int pixels = std::max(9, static_cast<int>(TextPx * factor + 0.5));
    const int pad = static_cast<int>(8 * factor + 0.5);
    layout()->setContentsMargins(pad, pad, pad, pad);
    layout()->setSpacing(pad);
    question->setStyleSheet(
        QStringLiteral("color: %1; font-size: %2px;").arg(Style::PopupText.name()).arg(pixels));
    const int pad_x = static_cast<int>(12 * factor + 0.5);
    const int pad_y = static_cast<int>(7 * factor + 0.5);
    listen->setStyleSheet(ButtonStyle(Style::PopupText, pixels, pad_x, pad_y));
    keep->setStyleSheet(ButtonStyle(Style::PopupAccent, pixels, pad_x, pad_y));
    drop->setStyleSheet(ButtonStyle(Style::PopupText, pixels, pad_x, pad_y));
    // The first button says Stop while the clip plays: it is as wide as the wider of its two
    // labels, so that the buttons beside it keep their places (in the Android app they moved
    // when the label changed).
    listen->setMinimumWidth(0);
    int widest = 0;
    for (const QString& label : {tr("Stop"), tr("Listen")}) {
        listen->setText(label);
        listen->ensurePolished();
        widest = std::max(widest, listen->sizeHint().width());
    }
    listen->setMinimumWidth(widest);
    SetPlaying(false);
    adjustSize();
    shown.start();
}

void AudioPrompt::SetPlaying(bool playing_) {
    playing = playing_;
    listen->setText(playing ? tr("Stop") : tr("Listen"));
}

void AudioPrompt::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    Style::PaintPopupGround(painter, rect());
    Style::PaintPopupEdge(painter, rect());
}

void AudioPrompt::mousePressEvent(QMouseEvent* event) {
    event->accept(); // clicks between the buttons answer nothing
}

} // namespace ReaderUi
