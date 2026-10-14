// SPDX-FileCopyrightText: PPSSPP Reader fork contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <optional>
#include <QApplication>
#include <QEventLoop>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHash>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QMutex>
#include <QMutexLocker>
#include <QPainter>
#include <QPushButton>
#include <QSet>
#include <QTimer>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>
#include <format>
#include "reader/desktop/script_index.h"
#include "reader/qt/reader_games.h"
#include "reader/qt/reader_overlay.h"
#include "reader/qt/reader_session.h"

namespace ReaderUi {

namespace {

/// Enough of a profile to hold the comments it begins with.
constexpr qint64 HeadBytes = 4096;
const QString Bundled = QStringLiteral(":/reader/profiles");

QString Tr(const char* text) {
    return QCoreApplication::translate("ReaderUi", text);
}

QString Head(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    // A character cut in two at the end is in no line that matters: those end with a line
    // break.
    return QString::fromUtf8(file.read(HeadBytes));
}

QMutex known_mutex;
std::optional<QSet<QString>> known; // the game list is filled on a thread of its own
std::optional<QHash<QString, QString>> audio_said;

QSet<QString> KnownIds() {
    QMutexLocker lock(&known_mutex);
    if (!known) {
        QSet<QString> ids;
        for (const QString& folder : {ReaderBridge::UserProfileDirectory(), Bundled}) {
            for (const QString& name : QDir(folder).entryList(QDir::Files)) {
                if (const auto id = ReaderApp::Catalog::TitleIdOf(name)) {
                    ids.insert(*id);
                }
            }
        }
        known = ids;
    }
    return *known;
}

} // namespace

QList<ReaderApp::ProfileInfo> Games::Supported() {
    QList<ReaderApp::ProfileInfo> games;
    QSet<QString> seen;
    const auto take = [&games, &seen](const QString& folder, bool own) {
        for (const QString& name : QDir(folder).entryList(QDir::Files, QDir::Name)) {
            const auto id = ReaderApp::Catalog::TitleIdOf(name);
            if (!id || seen.contains(*id)) {
                continue;
            }
            seen.insert(*id);
            games.append(ReaderApp::Catalog::Describe(*id, Head(folder + u'/' + name), own));
        }
    };
    take(ReaderBridge::UserProfileDirectory(), true);
    take(Bundled, false);
    std::stable_sort(games.begin(), games.end(),
                     [](const ReaderApp::ProfileInfo& a, const ReaderApp::ProfileInfo& b) {
                         return a.title.compare(b.title, Qt::CaseInsensitive) < 0;
                     });
    {
        QMutexLocker lock(&known_mutex);
        known = seen;
    }
    return games;
}

bool Games::Covers(const QString& game_id) {
    if (game_id.isEmpty()) {
        return false;
    }
    return KnownIds().contains(game_id.trimmed().toUpper());
}

QString Games::AudioOf(const QString& title_id) {
    {
        QMutexLocker lock(&known_mutex);
        if (audio_said) {
            return audio_said->value(title_id.toUpper());
        }
    }
    QHash<QString, QString> said;
    for (const ReaderApp::ProfileInfo& game : Supported()) {
        if (!game.audio.isEmpty()) {
            said.insert(game.title_id.toUpper(), game.audio);
        }
    }
    QMutexLocker lock(&known_mutex);
    audio_said = said;
    return said.value(title_id.toUpper());
}

void Games::Forget() {
    QMutexLocker lock(&known_mutex);
    known.reset();
    audio_said.reset();
}

QPixmap Games::Badged(const QPixmap& picture, const QString& game_id) {
    if (picture.isNull() || !Covers(game_id)) {
        return picture;
    }
    QPixmap out = picture;
    QPainter painter(&out);
    painter.setRenderHint(QPainter::Antialiasing);
    const qreal width = out.width() / out.devicePixelRatio();
    const qreal height = out.height() / out.devicePixelRatio();
    // A picture smaller than a few badges across gets a badge to match.
    const qreal side = std::min({26.0, width / 3.0, height / 3.0});
    if (side < 6) {
        return picture;
    }
    const qreal gap = 6.0 * side / 26.0;
    const QRectF box(width - gap - side, gap, side, side);
    painter.setBrush(Style::PopupBackground);
    painter.setPen(QPen(Style::Accent, std::max(1.0, side / 17.0)));
    painter.drawRoundedRect(box, side / 5.0, side / 5.0);
    const qreal inset = side * 0.22;
    Style::DrawOpenBook(painter, box.adjusted(inset, inset, -inset, -inset), Style::Accent,
                        std::max(1.2, side / 13.0));
    return out;
}

namespace {

/// The list of scripts the app offers: READER_SCRIPTS_URL, or the published one. A path or a
/// file:// address is read from the disk (a list of one's own, being tried).
QString IndexAddress() {
    const QString wanted = qEnvironmentVariable("READER_SCRIPTS_URL").trimmed();
    return wanted.isEmpty() ? QString::fromLatin1(ReaderApp::Scripts::DefaultIndexUrl) : wanted;
}

QUrl UrlOf(const QString& address) {
    if (QDir::isAbsolutePath(address)) {
        return QUrl::fromLocalFile(address);
    }
    return QUrl(address);
}

/// Fetches a file with Qt's network access; the window waits, the app goes on drawing.
/// Nothing, and `error` set, when it cannot be had.
std::optional<QByteArray> Fetch(const QUrl& url, QString* error) {
    if (url.isLocalFile()) {
        QFile file(url.toLocalFile());
        if (!file.open(QIODevice::ReadOnly)) {
            *error = Tr("Cannot read %1: %2").arg(url.toLocalFile(), file.errorString());
            return std::nullopt;
        }
        return file.readAll();
    }
    if (url.scheme() != QStringLiteral("https") && url.scheme() != QStringLiteral("http")) {
        *error = Tr("%1 is not an address the app can fetch").arg(url.toString());
        return std::nullopt;
    }
    QNetworkAccessManager network;
    QNetworkRequest request(url);
    request.setTransferTimeout(30000);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply* reply = network.get(request);
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QApplication::setOverrideCursor(Qt::WaitCursor);
    loop.exec(QEventLoop::ExcludeUserInputEvents);
    QApplication::restoreOverrideCursor();
    std::optional<QByteArray> out;
    if (reply->error() == QNetworkReply::NoError) {
        out = reply->readAll();
    } else {
        *error = Tr("Could not fetch %1 (%2)").arg(url.toString(), reply->errorString());
    }
    reply->deleteLater();
    return out;
}

std::optional<QByteArray> BundledScript(const QString& id) {
    QFile file(Bundled + u'/' + id + QStringLiteral(".lua"));
    if (!file.open(QIODevice::ReadOnly)) {
        return std::nullopt;
    }
    return file.readAll();
}

/// A game in the window: what is installed of it and what the list has.
struct Row {
    QString id;
    QString title;
    QString reads;
    QString version;
    std::optional<ReaderApp::Scripts::Entry> entry;
    std::optional<ReaderApp::Scripts::State> state; ///< When the list has the game
    bool user{};    ///< A script in the user's folder
    bool bundled{}; ///< A script that comes with the app
};

QString StateText(const Row& row, bool list_known) {
    using ReaderApp::Scripts::State;
    if (row.state) {
        switch (*row.state) {
        case State::NotInstalled:
            return Tr("Not installed");
        case State::Installed:
            return Tr("Installed");
        case State::Update:
            return Tr("Update available");
        case State::Own:
            return Tr("Your own script");
        case State::TooNew:
            return Tr("Needs a newer app");
        }
    }
    if (row.user) {
        return list_known ? Tr("Your own script") : Tr("Installed");
    }
    return row.bundled ? Tr("Comes with the app") : QString();
}

} // namespace

void RunGameScriptsDialog(QWidget* parent) {
    using ReaderApp::Scripts::State;
    QDialog dialog(parent);
    dialog.setWindowTitle(Tr("Game scripts"));
    dialog.resize(820, 560);
    auto* layout = new QVBoxLayout(&dialog);
    auto* about = new QLabel(
        Tr("A game's text is read by a script written for that game. Install one from the list "
           "published with the app, or a file of your own. A game with a script carries a book "
           "badge in the game list; a script installed while its game runs is used at once."));
    about->setWordWrap(true);
    layout->addWidget(about);

    auto* tree = new QTreeWidget;
    tree->setColumnCount(2);
    tree->setHeaderLabels({Tr("Game"), Tr("Script")});
    tree->setRootIsDecorated(false);
    tree->setAlternatingRowColors(true);
    tree->setWordWrap(true);
    tree->setUniformRowHeights(false);
    tree->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    tree->header()->setStretchLastSection(false);
    tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    QFont font = tree->font();
    font.setFamilies(Style::JapaneseFamilies());
    tree->setFont(font);
    layout->addWidget(tree, 1);

    auto* status = new QLabel;
    status->setWordWrap(true);
    layout->addWidget(status);

    auto* buttons = new QHBoxLayout;
    auto* install = new QPushButton(Tr("Install"));
    auto* remove = new QPushButton(Tr("Remove"));
    auto* from_file = new QPushButton(Tr("Install from a file…"));
    auto* again = new QPushButton(Tr("Look at the list again"));
    auto* folder_button = new QPushButton(Tr("Open the folder"));
    install->setObjectName(QStringLiteral("reader_scripts_install"));
    remove->setObjectName(QStringLiteral("reader_scripts_remove"));
    for (QPushButton* button : {install, remove, from_file, again, folder_button}) {
        buttons->addWidget(button);
    }
    buttons->addStretch(1);
    auto* close = new QPushButton(Tr("Close"));
    buttons->addWidget(close);
    QObject::connect(close, &QPushButton::clicked, &dialog, &QDialog::reject);
    layout->addLayout(buttons);

    ReaderApp::Scripts::Folder folder(ReaderBridge::UserProfileDirectory());
    std::optional<ReaderApp::Scripts::Index> index;
    QUrl index_url;
    QList<Row> rows;

    const auto selected = [&]() -> const Row* {
        const QTreeWidgetItem* item = tree->currentItem();
        if (!item) {
            return nullptr;
        }
        const QString id = item->data(0, Qt::UserRole).toString();
        for (const Row& row : rows) {
            if (row.id == id) {
                return &row;
            }
        }
        return nullptr;
    };

    const auto update_buttons = [&] {
        const Row* row = selected();
        install->setEnabled(row && row->state && *row->state != State::Installed &&
                            *row->state != State::TooNew);
        install->setText(row && row->state == State::Update ? Tr("Update")
                         : row && row->state == State::Own  ? Tr("Replace with the list's")
                                                             : Tr("Install"));
        remove->setEnabled(row && row->user);
    };

    const auto fill = [&] {
        const QString keep = tree->currentItem() ? tree->currentItem()->data(0, Qt::UserRole).toString()
                                                 : QString();
        Games::Forget();
        QHash<QString, Row> by_id;
        for (const ReaderApp::ProfileInfo& game : Games::Supported()) {
            Row row{game.title_id, game.title, game.reads, game.version};
            row.user = game.own;
            row.bundled = ReaderBridge::HasBundledProfile(game.title_id);
            by_id.insert(row.id, row);
        }
        if (index) {
            for (const ReaderApp::Scripts::Entry& entry : index->entries) {
                Row& row = by_id[entry.id];
                row.id = entry.id;
                row.title = entry.title;
                row.reads = entry.reads;
                row.version = entry.version;
                row.entry = entry;
                row.user = folder.Script(entry.id).has_value();
                row.bundled = ReaderBridge::HasBundledProfile(entry.id);
                row.state = ReaderApp::Scripts::StateOf(entry, folder.Script(entry.id),
                                                        BundledScript(entry.id),
                                                        folder.Recorded(entry.id));
            }
        }
        rows = by_id.values();
        std::stable_sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
            return a.title.compare(b.title, Qt::CaseInsensitive) < 0;
        });
        tree->clear();
        for (const Row& row : rows) {
            QString text = row.title;
            if (!row.reads.isEmpty()) {
                text += QStringLiteral("\n    ") + row.reads;
            }
            text += QStringLiteral("\n    ");
            // A version line that names the game's ID already says it.
            if (!row.version.isEmpty()) {
                text += row.version;
            }
            if (!QString(row.version).remove(u'-').contains(row.id)) {
                text += (row.version.isEmpty() ? QString() : QStringLiteral(" · ")) + row.id;
            }
            auto* item = new QTreeWidgetItem(tree, {text, StateText(row, index.has_value())});
            item->setData(0, Qt::UserRole, row.id);
            item->setTextAlignment(1, Qt::AlignTop | Qt::AlignLeft);
            if (row.id == keep) {
                tree->setCurrentItem(item);
            }
        }
        update_buttons();
    };

    const auto fetch_index = [&] {
        index_url = UrlOf(IndexAddress());
        QString error;
        const auto bytes = Fetch(index_url, &error);
        if (bytes) {
            index = ReaderApp::Scripts::ParseIndex(*bytes, &error);
        }
        if (index) {
            status->setText(Tr("Scripts in the published list: %1.").arg(index->entries.size()));
        } else {
            status->setText(Tr("The list of scripts could not be had: %1").arg(error));
        }
        fill();
    };

    // A script of the running game is put to use at once.
    const auto changed = [&](const QStringList& ids) {
        Games::Forget();
        const auto& session = ReaderSession::Instance();
        if (session.IsActive() && ids.contains(session.GameKey())) {
            ReaderBridge::ReloadProfile();
        }
        fill();
    };

    QObject::connect(tree, &QTreeWidget::currentItemChanged, &dialog, [&] { update_buttons(); });
    QObject::connect(again, &QPushButton::clicked, &dialog, [&] { fetch_index(); });
    QObject::connect(folder_button, &QPushButton::clicked, &dialog, [] {
        QDir().mkpath(ReaderBridge::UserProfileDirectory());
        QDesktopServices::openUrl(QUrl::fromLocalFile(ReaderBridge::UserProfileDirectory()));
    });
    QObject::connect(install, &QPushButton::clicked, &dialog, [&] {
        const Row* row = selected();
        if (!row || !row->entry) {
            return;
        }
        const ReaderApp::Scripts::Entry entry = *row->entry;
        if (row->state == State::Own &&
            QMessageBox::question(&dialog, Tr("Game scripts"),
                                  Tr("Your own script of %1 is replaced by the list's.")
                                      .arg(entry.title),
                                  QMessageBox::Ok | QMessageBox::Cancel) != QMessageBox::Ok) {
            return;
        }
        QHash<QString, QByteArray> files;
        QString error;
        QList<ReaderApp::Scripts::FileEntry> wanted{entry.script};
        wanted.append(entry.data);
        for (const ReaderApp::Scripts::FileEntry& file : wanted) {
            const auto bytes = Fetch(index_url.resolved(QUrl(file.file)), &error);
            if (!bytes) {
                break;
            }
            files.insert(file.file, *bytes);
        }
        if (files.size() == wanted.size() && folder.Install(entry, files, &error)) {
            status->setText(Tr("Installed the script of %1.").arg(entry.title));
            changed({entry.id});
        } else {
            status->setText(Tr("The script of %1 was not installed: %2").arg(entry.title, error));
        }
    });
    QObject::connect(remove, &QPushButton::clicked, &dialog, [&] {
        const Row* row = selected();
        if (!row || !row->user) {
            return;
        }
        const QString id = row->id;
        const QString title = row->title;
        QString text = Tr("The script of %1 is removed from the app's folder.").arg(title);
        if (row->bundled) {
            text += u' ' + Tr("The one that comes with the app is used again.");
        }
        if (QMessageBox::question(&dialog, Tr("Game scripts"), text,
                                  QMessageBox::Ok | QMessageBox::Cancel) != QMessageBox::Ok) {
            return;
        }
        folder.Remove(id);
        status->setText(Tr("Removed the script of %1.").arg(title));
        changed({id});
    });
    QObject::connect(from_file, &QPushButton::clicked, &dialog, [&] {
        const QStringList paths = QFileDialog::getOpenFileNames(
            &dialog, Tr("Install game scripts"), QString(),
            Tr("Game scripts and their data (*.lua *.charset *.bin);;All files (*)"));
        if (paths.isEmpty()) {
            return;
        }
        QString error;
        const QStringList ids = folder.InstallFiles(paths, &error);
        QString text = ids.isEmpty() ? QString() : Tr("Installed: %1.").arg(ids.join(QStringLiteral(", ")));
        if (!error.isEmpty()) {
            text += (text.isEmpty() ? QString() : QStringLiteral("\n")) + error;
        }
        status->setText(text);
        changed(ids);
    });

    fill();
    // The window is up before the list is fetched.
    QTimer::singleShot(0, &dialog, [&] { fetch_index(); });
    dialog.exec();
}
} // namespace ReaderUi
