// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the Flycast Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStandardPaths>
#include "reader/qt/reader_link.h"

namespace ReaderUi::HomeLink {

QString Folder() {
    const QString set = qEnvironmentVariable("READER_HOME_DIR");
    if (!set.isEmpty()) {
        return set;
    }
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/reader-home");
}

QJsonObject SharedSettings(bool* changed) {
    static QDateTime seen;
    static qint64 seen_size = -1;
    const QFileInfo info(Folder() + QStringLiteral("/settings.json"));
    if (changed != nullptr) {
        const QDateTime modified = info.exists() ? info.lastModified() : QDateTime();
        const qint64 size = info.exists() ? info.size() : -1;
        *changed = modified != seen || size != seen_size;
        seen = modified;
        seen_size = size;
    }
    QFile file(info.absoluteFilePath());
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return QJsonDocument::fromJson(file.readAll()).object();
}

bool Write(const About& about, const QList<Game>* games, const QJsonObject* stats) {
    if (about.app.isEmpty()) {
        return false;
    }
    const QString folder = Folder() + QStringLiteral("/apps");
    const QString path = folder + QLatin1Char('/') + about.app + QStringLiteral(".json");
    QJsonObject before;
    {
        QFile file(path);
        if (file.open(QIODevice::ReadOnly)) {
            before = QJsonDocument::fromJson(file.readAll()).object();
        }
    }
    QJsonObject root;
    root.insert(QStringLiteral("format"), 1);
    root.insert(QStringLiteral("app"), about.app);
    root.insert(QStringLiteral("name"), about.name);
    root.insert(QStringLiteral("system"), about.system);
    root.insert(QStringLiteral("program"), QDir::toNativeSeparators(QCoreApplication::applicationFilePath()));
    root.insert(QStringLiteral("game_args"), QJsonArray::fromStringList(about.game_args));
    root.insert(QStringLiteral("menu_args"), QJsonArray::fromStringList(about.menu_args));
    if (games != nullptr) {
        QJsonArray list;
        for (const Game& game : *games) {
            QJsonObject entry;
            entry.insert(QStringLiteral("id"), game.id);
            entry.insert(QStringLiteral("title"), game.title);
            entry.insert(QStringLiteral("path"), game.path);
            entry.insert(QStringLiteral("cover"), game.cover);
            entry.insert(QStringLiteral("readable"), true);
            list.append(entry);
        }
        root.insert(QStringLiteral("games"), list);
    } else {
        root.insert(QStringLiteral("games"), before.value(QStringLiteral("games")).toArray());
    }
    root.insert(QStringLiteral("stats"), stats != nullptr ? *stats : before.value(QStringLiteral("stats")).toObject());
    // Only what changed is worth a write: the time alone is not.
    QJsonObject unchanged = before;
    unchanged.remove(QStringLiteral("written"));
    if (unchanged == root && !before.isEmpty()) {
        return true;
    }
    root.insert(QStringLiteral("written"), QDateTime::currentMSecsSinceEpoch());
    if (!QDir().mkpath(folder)) {
        return false;
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    return file.commit();
}

} // namespace ReaderUi::HomeLink
