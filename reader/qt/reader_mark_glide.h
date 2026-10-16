// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <QList>
#include <QRectF>

namespace ReaderUi {

/**
 * How the mark on the word looked up goes from one word to the next: it glides there.
 *
 * Each edge of each of the mark's rectangles (one a row) follows where it should be on a
 * critically damped spring, so a mark sent somewhere else halfway keeps its speed and turns,
 * and settles without a bounce. The edge in front is a little quicker than the one behind, so
 * the mark stretches very slightly on its way and draws itself together as it arrives. A mark
 * that appears grows in from just smaller and fades in; one taken away fades out where it was.
 *
 * The same numbers as MarkGlide.kt in the Android app.
 */
class MarkGlide {
public:
    /// The mark is to be at `to` (empty: nowhere). `glide`: from where it is drawn now, else at
    /// once (the picture moved under it). A mark that was nowhere grows in either way.
    void SetTarget(const QList<QRectF>& to, bool glide) {
        if (to.isEmpty()) {
            opacity.target = 0;
            return;
        }
        if (rects.isEmpty()) {
            for (const QRectF& rect : to) {
                const qreal dx = rect.width() * (1 - AppearScale) / 2;
                const qreal dy = rect.height() * (1 - AppearScale) / 2;
                rects.append(Box{Edge::From(rect.left() + dx, rect.left()),
                                 Edge::From(rect.top() + dy, rect.top()),
                                 Edge::From(rect.right() - dx, rect.right()),
                                 Edge::From(rect.bottom() - dy, rect.bottom()), false});
            }
            opacity = Edge::Fade(0, 1);
            return;
        }
        opacity.target = 1;
        if (!glide) {
            rects.clear();
            for (const QRectF& rect : to) {
                rects.append(Box{Edge::At(rect.left()), Edge::At(rect.top()),
                                 Edge::At(rect.right()), Edge::At(rect.bottom()), false});
            }
            return;
        }
        const QRectF from = Bounds(Drawn());
        QRectF onto;
        for (const QRectF& rect : to) {
            onto = onto.isNull() ? rect : onto.united(rect);
        }
        // A row more than before starts from the last row drawn; a row fewer joins the last one.
        while (rects.size() < to.size()) {
            Box copy = rects.last();
            copy.leaving = false;
            for (Edge* edge : copy.Edges()) {
                edge->speed = 0;
            }
            rects.append(copy);
        }
        for (qsizetype i = 0; i < rects.size(); ++i) {
            const QRectF& rect = to[std::min(i, to.size() - 1)];
            Box& box = rects[i];
            box.leaving = i >= to.size();
            box.left.target = rect.left();
            box.top.target = rect.top();
            box.right.target = rect.right();
            box.bottom.target = rect.bottom();
        }
        const qreal dx = onto.center().x() - from.center().x();
        const qreal dy = onto.center().y() - from.center().y();
        for (Box& box : rects) {
            box.left.quickness = Quickness(-dx);
            box.right.quickness = Quickness(dx);
            box.top.quickness = Quickness(-dy);
            box.bottom.quickness = Quickness(dy);
        }
    }

    /// Moves on by `seconds`. False once everything has come to rest.
    bool Advance(qreal seconds) {
        const qreal dt = std::clamp<qreal>(seconds, 0, 0.05);
        for (Box& box : rects) {
            for (Edge* edge : box.Edges()) {
                edge->Advance(Omega * edge->quickness, dt);
            }
        }
        opacity.Advance(FadeOmega, dt);
        rects.erase(std::remove_if(rects.begin(), rects.end(),
                                   [](const Box& box) { return box.leaving && box.Resting(); }),
                    rects.end());
        if (opacity.target == 0 && opacity.Resting()) {
            rects.clear();
        }
        return Moving();
    }

    [[nodiscard]] bool Moving() const {
        if (rects.isEmpty()) {
            return false;
        }
        return !opacity.Resting() ||
               std::any_of(rects.begin(), rects.end(), [](const Box& box) { return !box.Resting(); });
    }

    /// True while anything of the mark is to be drawn, a fading one too.
    [[nodiscard]] bool Visible() const {
        return !rects.isEmpty();
    }

    [[nodiscard]] QList<QRectF> Drawn() const {
        QList<QRectF> out;
        for (const Box& box : rects) {
            out.append(QRectF(QPointF(box.left.at, box.top.at), QPointF(box.right.at, box.bottom.at))
                           .normalized());
        }
        return out;
    }

    [[nodiscard]] qreal Opacity() const {
        return std::clamp<qreal>(opacity.at, 0, 1);
    }

private:
    /// How quickly it all moves: about a fifth of a second to the next word, by eye.
    static constexpr qreal Omega = 2 * 3.14159265358979 / 0.20;
    static constexpr qreal FadeOmega = 2 * 3.14159265358979 / 0.16;
    static constexpr qreal Lead = 1.15;
    static constexpr qreal Trail = 0.9;
    static constexpr qreal AppearScale = 0.9;

    struct Edge {
        qreal at{};
        qreal speed{};
        qreal target{};
        qreal quickness{1};
        /// Close enough and slow enough to stop: a fifth of a pixel, or of 1% for the fade.
        qreal close_enough{0.2};
        qreal slow_enough{6};

        static Edge At(qreal value) {
            return From(value, value);
        }
        static Edge From(qreal from, qreal to) {
            Edge edge;
            edge.at = from;
            edge.target = to;
            return edge;
        }
        static Edge Fade(qreal from, qreal to) {
            Edge edge = From(from, to);
            edge.close_enough = 0.002;
            edge.slow_enough = 0.06;
            return edge;
        }
        /// One step of a critically damped spring, exactly (no step size to go wrong).
        void Advance(qreal omega, qreal dt) {
            const qreal x = at - target;
            const qreal decay = std::exp(-omega * dt);
            const qreal k = (speed + omega * x) * dt;
            at = target + (x + k) * decay;
            speed = (speed - omega * k) * decay;
            if (Resting()) {
                at = target;
                speed = 0;
            }
        }
        [[nodiscard]] bool Resting() const {
            return std::abs(at - target) < close_enough && std::abs(speed) < slow_enough;
        }
    };

    struct Box {
        Edge left, top, right, bottom;
        bool leaving{};

        std::array<Edge*, 4> Edges() {
            return {&left, &top, &right, &bottom};
        }
        [[nodiscard]] bool Resting() const {
            return left.Resting() && top.Resting() && right.Resting() && bottom.Resting();
        }
    };

    static qreal Quickness(qreal towards) {
        return towards > 0.5 ? Lead : towards < -0.5 ? Trail : 1;
    }
    static QRectF Bounds(const QList<QRectF>& rects_) {
        QRectF out;
        for (const QRectF& rect : rects_) {
            out = out.isNull() ? rect : out.united(rect);
        }
        return out;
    }

    QList<Box> rects;
    Edge opacity{Edge::Fade(1, 1)};
};

} // namespace ReaderUi
