// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <catch2/catch_tostring.hpp>
#include <QString>
#include <QStringList>

/// Text of a test, as the QString the code under test works with.
inline QString Q(const char16_t* text) {
    return QString::fromUtf16(text);
}

namespace Catch {
template <>
struct StringMaker<QString> {
    static std::string convert(const QString& value) {
        return '"' + value.toStdString() + '"';
    }
};
template <>
struct StringMaker<QStringList> {
    static std::string convert(const QStringList& value) {
        return '[' + value.join(QStringLiteral(", ")).toStdString() + ']';
    }
};
} // namespace Catch
