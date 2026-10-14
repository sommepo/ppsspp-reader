// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

#include <QCryptographicHash>
#include <QEventLoop>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include "reader/desktop/word_audio.h"

namespace ReaderApp::WordAudio {

const char* const Missing = "ae6398b5a27bc8c0a771df6c907ade794be15518174773c58c7c7ddd17098906";

namespace {

constexpr qsizetype Smallest = 256;
constexpr qsizetype Largest = 2 * 1024 * 1024;
constexpr int TimeoutMs = 10000;

QByteArray Download(const QString& address) {
    QNetworkAccessManager manager;
    QNetworkRequest request{QUrl{address}};
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setMaximumRedirectsAllowed(4);
    QNetworkReply* reply = manager.get(request);
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    timer.start(TimeoutMs);
    loop.exec();
    QByteArray out;
    if (reply->isFinished() && reply->error() == QNetworkReply::NoError &&
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200) {
        out = reply->read(Largest + 1);
    } else {
        reply->abort();
    }
    reply->deleteLater();
    return out;
}

} // namespace

QString Address(const QString& expression, const QString& reading) {
    const QString kana = reading.isEmpty() ? expression : reading;
    const QString written = expression == kana ? QString() : expression;
    QUrlQuery query;
    if (!written.isEmpty()) {
        query.addQueryItem(QStringLiteral("kanji"),
                           QString::fromLatin1(QUrl::toPercentEncoding(written)));
    }
    if (!kana.isEmpty()) {
        query.addQueryItem(QStringLiteral("kana"),
                           QString::fromLatin1(QUrl::toPercentEncoding(kana)));
    }
    return QStringLiteral("https://assets.languagepod101.com/dictionary/japanese/audiomp3.php?") +
           query.toString(QUrl::FullyEncoded);
}

QString FileName(const QString& expression, const QString& reading) {
    QByteArray key = expression.toUtf8();
    key.append('\0');
    key.append(reading.toUtf8());
    const QByteArray digest = QCryptographicHash::hash(key, QCryptographicHash::Sha256).toHex();
    return QStringLiteral("reader_word_") + QString::fromLatin1(digest.left(16)) +
           QStringLiteral(".mp3");
}

bool IsRecording(const QByteArray& bytes) {
    if (bytes.size() < Smallest || bytes.size() > Largest) {
        return false;
    }
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex() !=
           QByteArray{Missing};
}

QByteArray Fetch(const QString& expression, const QString& reading, const Fetcher& fetcher) {
    if (expression.trimmed().isEmpty() && reading.trimmed().isEmpty()) {
        return {};
    }
    const QString address = Address(expression, reading);
    QByteArray bytes;
    try {
        bytes = fetcher ? fetcher(address) : Download(address);
    } catch (...) {
        return {};
    }
    return IsRecording(bytes) ? bytes : QByteArray{};
}

} // namespace ReaderApp::WordAudio
