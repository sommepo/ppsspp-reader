// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QEventLoop>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHash>
#include <QHeaderView>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QProgressDialog>
#include <QApplication>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextLayout>
#include <QThreadPool>
#include <QTreeWidget>
#include <QVBoxLayout>
#include "reader/qt/host.h"
#include "reader/desktop/anki.h"
#include "reader/desktop/card_builder.h"
#include "reader/qt/lookup_flow.h"
#include "reader/qt/reader_crt_page.h"
#include "reader/qt/reader_dialogs.h"
#include "reader/qt/reader_overlay.h"
#include "reader/qt/reader_session.h"

namespace ReaderUi {

using ReaderApp::ReadingCounters;

namespace {

QString Tr(const char* text) {
    return QCoreApplication::translate("ReaderUi", text);
}

/// Runs `work` on a thread of its own while the window stays alive, and returns when it is
/// done. For the few things that take a moment: asking Anki, importing a dictionary.
void RunBlocking(const std::function<void()>& work) {
    QEventLoop loop;
    QThreadPool pool;
    pool.setMaxThreadCount(1);
    pool.start([&work, &loop] {
        work();
        QMetaObject::invokeMethod(&loop, [&loop] { loop.quit(); }, Qt::QueuedConnection);
    });
    loop.exec(QEventLoop::ExcludeUserInputEvents);
    pool.waitForDone();
}

QString Number(qint64 value) {
    return QLocale().toString(value);
}

QString Speed(const ReadingCounters& counters) {
    const std::optional<double> speed = counters.CharactersPerHour();
    if (!speed || counters.active_ms < ReaderApp::SessionSummary::SpeedAfterMs) {
        return QStringLiteral("–");
    }
    return Number(static_cast<qint64>(*speed / 10.0 + 0.5) * 10) + QStringLiteral("/h");
}

QString When(qint64 epoch_ms) {
    return QLocale().toString(QDateTime::fromMSecsSinceEpoch(epoch_ms), QLocale::ShortFormat);
}

// ---- A line of text whose words can be clicked ----

/// Shows one line of the backlog as text of the window's own; a click on a character says
/// which one, and a range of it can be marked.
class LineView : public QWidget {
public:
    explicit LineView(QWidget* parent) : QWidget(parent) {
        QFont big = font();
        big.setFamilies(Style::JapaneseFamilies());
        big.setPointSizeF(big.pointSizeF() * 1.5);
        setFont(big);
        setCursor(Qt::IBeamCursor);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        setMinimumHeight(fontMetrics().height() * 3);
    }

    /// The character clicked: its index in the line's text and where it is in this view.
    std::function<void(int offset, const QRect& where)> on_click;

    void SetLine(const QString& text_) {
        text = text_;
        mark_first = -1;
        mark_last = -1;
        Relayout();
    }

    void SetMark(int first, int last) {
        mark_first = first;
        mark_last = last;
        update();
    }

    [[nodiscard]] QSize sizeHint() const override {
        return QSize(400, std::max(static_cast<int>(text_height) + 8, minimumHeight()));
    }

protected:
    void resizeEvent(QResizeEvent*) override {
        Relayout();
    }

    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        QList<QTextLayout::FormatRange> selections;
        if (mark_first >= 0 && mark_last >= mark_first) {
            QTextLayout::FormatRange range;
            range.start = mark_first;
            range.length = mark_last - mark_first + 1;
            range.format.setBackground(palette().highlight());
            range.format.setForeground(palette().highlightedText());
            selections.append(range);
        }
        painter.setPen(palette().color(QPalette::Text));
        layout.draw(&painter, QPointF(4, 4), selections);
    }

    void mouseReleaseEvent(QMouseEvent* event) override {
        if (event->button() != Qt::LeftButton || !on_click) {
            return;
        }
        const QPointF point = event->position() - QPointF(4, 4);
        for (int i = 0; i < layout.lineCount(); ++i) {
            const QTextLine line = layout.lineAt(i);
            if (point.y() < line.y() || point.y() >= line.y() + line.height()) {
                continue;
            }
            if (point.x() < 0 || point.x() > line.naturalTextWidth()) {
                return;
            }
            const int offset = line.xToCursor(point.x(), QTextLine::CursorOnCharacter);
            if (offset < 0 || offset >= text.size()) {
                return;
            }
            const qreal left = line.cursorToX(offset);
            const qreal right = line.cursorToX(offset + 1);
            on_click(offset, QRectF(left + 4, line.y() + 4, std::max(right - left, 1.0),
                                    line.height())
                                 .toAlignedRect());
            return;
        }
    }

private:
    void Relayout() {
        // A line break inside the text is kept as one character, so that offsets stay those
        // of the line's text.
        QString shown = text;
        shown.replace(u'\n', QChar::LineSeparator);
        layout.setText(shown);
        layout.setFont(font());
        QTextOption option;
        option.setWrapMode(QTextOption::WrapAnywhere);
        layout.setTextOption(option);
        layout.beginLayout();
        qreal y = 0;
        while (true) {
            QTextLine line = layout.createLine();
            if (!line.isValid()) {
                break;
            }
            line.setLineWidth(std::max(width() - 8, 40));
            line.setPosition(QPointF(0, y));
            y += line.height();
        }
        layout.endLayout();
        text_height = y;
        updateGeometry();
        update();
    }

    QString text;
    QTextLayout layout;
    qreal text_height{};
    int mark_first{-1};
    int mark_last{-1};
};

// ---- Backlog ----

constexpr int BacklogPage = 200;

void Backlog(QWidget* parent) {
    auto& session = ReaderSession::Instance();
    auto& database = session.Database();

    QDialog dialog(parent);
    dialog.setWindowTitle(Tr("Backlog"));
    dialog.resize(820, 620);
    auto* layout = new QVBoxLayout(&dialog);

    auto* top = new QHBoxLayout;
    auto* games = new QComboBox;
    QString current_game = session.GameKey();
    try {
        for (const ReaderApp::GameTotals& game : database.AllGameTotals()) {
            games->addItem(game.title, game.game);
        }
    } catch (const std::exception& error) {
        LOG_ERROR(Frontend, "[Reader] Backlog: {}", error.what());
    }
    if (!current_game.isEmpty() && games->findData(current_game) < 0) {
        games->insertItem(0, session.GameTitle(), current_game);
    }
    if (!current_game.isEmpty()) {
        games->setCurrentIndex(games->findData(current_game));
    }
    auto* search = new QLineEdit;
    search->setPlaceholderText(Tr("Search the lines read"));
    search->setClearButtonEnabled(true);
    top->addWidget(games, 1);
    top->addWidget(search, 2);
    layout->addLayout(top);

    auto* list = new QListWidget;
    QFont list_font = list->font();
    list_font.setFamilies(Style::JapaneseFamilies());
    list->setFont(list_font);
    list->setWordWrap(true);
    list->setAlternatingRowColors(true);
    layout->addWidget(list, 1);

    auto* speaker = new QLabel;
    layout->addWidget(speaker);
    auto* view = new LineView(&dialog);
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setWidget(view);
    scroll->setMinimumHeight(view->minimumHeight() + 12);
    scroll->setMaximumHeight(220);
    layout->addWidget(scroll);

    auto* status = new QLabel(Tr("Click a word of the line to look it up."));
    layout->addWidget(status);
    auto* buttons = new QHBoxLayout;
    auto* older = new QPushButton(Tr("Older lines"));
    auto* card = new QPushButton(Tr("Card of this sentence"));
    auto* close = new QPushButton(Tr("Close"));
    buttons->addWidget(older);
    buttons->addStretch(1);
    buttons->addWidget(card);
    buttons->addWidget(close);
    layout->addLayout(buttons);

    auto entries = std::make_shared<QList<ReaderApp::BacklogEntry>>();
    LookupFlow flow(&dialog);
    flow.mark = [view, entries, list](qint64 backlog_id, int first, int last) {
        const int row = list->currentRow();
        if (first < 0 || (row >= 0 && row < entries->size() && entries->at(row).id == backlog_id)) {
            view->SetMark(first, last);
        }
    };
    flow.notify = [status](const QString& text) { status->setText(text); };
    flow.open_dictionaries = [&dialog] { RunDictionariesDialog(&dialog); };

    const auto game_key = [games] { return games->currentData().toString(); };
    const auto show_entries = [list, entries] {
        list->clear();
        for (const ReaderApp::BacklogEntry& entry : std::as_const(*entries)) {
            QString text = entry.line.text;
            text.replace(u'\n', u' ');
            if (!entry.line.speaker.isEmpty()) {
                text = entry.line.speaker + QStringLiteral("： ") + text;
            }
            list->addItem(text);
        }
    };
    const auto reload = [&, show_entries] {
        flow.Close();
        entries->clear();
        const QString game = game_key();
        if (!game.isEmpty()) {
            try {
                const QString query = search->text().trimmed();
                *entries = query.isEmpty() ? database.RecentLines(game, BacklogPage)
                                           : database.SearchLines(game, query, BacklogPage);
            } catch (const std::exception& error) {
                status->setText(QString::fromUtf8(error.what()));
            }
        }
        show_entries();
        older->setEnabled(search->text().trimmed().isEmpty() && entries->size() >= BacklogPage);
        if (!entries->isEmpty()) {
            list->setCurrentRow(0);
        } else {
            view->SetLine({});
            speaker->clear();
        }
    };
    const auto selected = [list, entries]() -> std::optional<ReaderApp::PresentedLine> {
        const int row = list->currentRow();
        if (row < 0 || row >= entries->size()) {
            return std::nullopt;
        }
        const ReaderApp::BacklogEntry& entry = entries->at(row);
        // A line of this run still has its place in the game's audio.
        if (auto recent = ReaderSession::Instance().RecentLine(entry.id)) {
            return recent;
        }
        ReaderApp::PresentedLine presented;
        presented.line = entry.line;
        presented.backlog_id = entry.id;
        return presented;
    };

    QObject::connect(list, &QListWidget::currentRowChanged, &dialog, [&, selected](int) {
        flow.Close();
        const auto presented = selected();
        view->SetLine(presented ? presented->line.text : QString());
        speaker->setText(presented ? presented->line.speaker : QString());
        card->setEnabled(presented.has_value());
    });
    view->on_click = [&, selected](int offset, const QRect& where) {
        const auto presented = selected();
        if (!presented) {
            return;
        }
        LookupFlow::Anchor anchor;
        anchor.owner = &dialog;
        anchor.bounds = dialog.rect();
        const QPoint origin = view->mapTo(&dialog, QPoint(0, 0));
        anchor.keep_clear = QRect(origin + where.topLeft(), where.size()).adjusted(0, -4, 0, 4);
        anchor.near_x = origin.x() + where.left();
        flow.Lookup(*presented, offset, anchor);
    };
    QObject::connect(search, &QLineEdit::textChanged, &dialog, [reload] { reload(); });
    QObject::connect(games, &QComboBox::currentIndexChanged, &dialog, [reload] { reload(); });
    QObject::connect(older, &QPushButton::clicked, &dialog, [&, show_entries] {
        if (entries->isEmpty()) {
            return;
        }
        try {
            const auto more = database.RecentLines(game_key(), BacklogPage, entries->last().id);
            const int row = list->currentRow();
            entries->append(more);
            show_entries();
            list->setCurrentRow(row);
            older->setEnabled(more.size() >= BacklogPage);
        } catch (const std::exception& error) {
            status->setText(QString::fromUtf8(error.what()));
        }
    });
    QObject::connect(card, &QPushButton::clicked, &dialog, [&, selected] {
        if (const auto presented = selected()) {
            status->setText(Tr("Making a card…"));
            flow.MineSentence(*presented);
        }
    });
    QObject::connect(close, &QPushButton::clicked, &dialog, &QDialog::accept);

    reload();
    dialog.exec();
    flow.Close();
}

// ---- Statistics ----

QStringList CounterColumns(const ReadingCounters& counters) {
    return {Number(counters.characters),      Number(counters.unique_characters),
            Number(counters.reread_characters), Number(counters.lines),
            ReaderApp::SessionSummary::Duration(counters.active_ms), Speed(counters)};
}

QTreeWidget* MakeTable(const QStringList& first_columns) {
    auto* tree = new QTreeWidget;
    QStringList headers = first_columns;
    headers << Tr("Characters") << Tr("New") << Tr("Reread") << Tr("Lines") << Tr("Time")
            << Tr("Speed");
    tree->setHeaderLabels(headers);
    tree->setRootIsDecorated(false);
    tree->setUniformRowHeights(true);
    tree->setAlternatingRowColors(true);
    tree->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
    return tree;
}

void Statistics(QWidget* parent) {
    auto& session = ReaderSession::Instance();
    auto& database = session.Database();

    QDialog dialog(parent);
    dialog.setWindowTitle(Tr("Reading statistics"));
    dialog.resize(860, 600);
    auto* layout = new QVBoxLayout(&dialog);

    auto* summary = new QLabel;
    summary->setTextFormat(Qt::PlainText);
    layout->addWidget(summary);

    auto* tabs = new QTabWidget;
    auto* days = MakeTable({Tr("Reading day")});
    auto* games = MakeTable({Tr("Game")});
    auto* sessions = MakeTable({Tr("Started"), Tr("Game")});
    tabs->addTab(days, Tr("Days"));
    tabs->addTab(games, Tr("Games"));
    tabs->addTab(sessions, Tr("Sessions"));
    layout->addWidget(tabs, 1);

    auto* paused = new QCheckBox(Tr("Pause statistics: count neither characters nor time"));
    paused->setChecked(session.Settings().StatsPaused());
    layout->addWidget(paused);

    auto* buttons = new QHBoxLayout;
    auto* delete_session = new QPushButton(Tr("Delete the selected session"));
    auto* export_json = new QPushButton(Tr("Export JSON…"));
    auto* export_csv = new QPushButton(Tr("Export CSV…"));
    auto* strip = new QPushButton(Tr("Remove text counted twice…"));
    strip->setToolTip(Tr("A long text read by scrolling (a mail on a phone in a game) was counted again at every stop: "
                         "this takes the repeats out of the counts. The lines themselves stay."));
    auto* clear_game = new QPushButton(Tr("Clear this game"));
    auto* clear_all = new QPushButton(Tr("Clear everything"));
    auto* close = new QPushButton(Tr("Close"));
    buttons->addWidget(delete_session);
    buttons->addWidget(export_json);
    buttons->addWidget(export_csv);
    buttons->addWidget(strip);
    buttons->addStretch(1);
    buttons->addWidget(clear_game);
    buttons->addWidget(clear_all);
    buttons->addWidget(close);
    layout->addLayout(buttons);
    clear_game->setEnabled(session.IsActive());

    const auto refresh = [&] {
        try {
            const QString today = ReaderApp::ReadingRules::ReadingDay(
                QDateTime::currentMSecsSinceEpoch());
            const auto line = [](const QString& label, const ReadingCounters& counters) {
                return QStringLiteral("%1: %2 %3 · %4 · %5\n")
                    .arg(label, Number(counters.characters), Tr("characters"),
                         ReaderApp::SessionSummary::Duration(counters.active_ms), Speed(counters));
            };
            QString text = line(Tr("Today"), database.DayTotals(today));
            if (session.IsActive()) {
                text += line(session.GameTitle(), database.Totals(session.GameKey()));
            }
            text += line(Tr("All games"), database.Totals());
            summary->setText(text.trimmed());

            QHash<QString, QString> titles;
            games->clear();
            for (const ReaderApp::GameTotals& game : database.AllGameTotals()) {
                titles.insert(game.game, game.title);
                new QTreeWidgetItem(games, QStringList{game.title} + CounterColumns(game.counters));
            }
            days->clear();
            for (const auto& [day, counters] : database.DailyHistory({}, 30)) {
                new QTreeWidgetItem(days, QStringList{day} + CounterColumns(counters));
            }
            sessions->clear();
            for (const ReaderApp::SessionRecord& record : database.Sessions()) {
                auto* item = new QTreeWidgetItem(
                    sessions, QStringList{When(record.started),
                                          titles.value(record.game, record.game)} +
                                  CounterColumns(record.counters));
                item->setData(0, Qt::UserRole, record.id);
            }
        } catch (const std::exception& error) {
            summary->setText(QString::fromUtf8(error.what()));
        }
    };

    const QPointer<QDialog> alive(&dialog);
    // What has not been saved yet goes in first.
    const auto flush_and_refresh = [&session, alive, refresh] {
        session.Flush([alive, refresh] {
            if (alive) {
                refresh();
            }
        });
    };

    QObject::connect(paused, &QCheckBox::toggled, &dialog, [&session](bool on) {
        session.Settings().SetStatsPaused(on);
        session.OnSettingsChanged();
    });
    QObject::connect(delete_session, &QPushButton::clicked, &dialog, [&] {
        QTreeWidgetItem* item = sessions->currentItem();
        if (!item || tabs->currentWidget() != sessions) {
            tabs->setCurrentWidget(sessions);
            return;
        }
        if (QMessageBox::question(&dialog, Tr("Delete session"),
                                  Tr("Delete this session and what it counted?")) !=
            QMessageBox::Yes) {
            return;
        }
        database.DeleteSession(item->data(0, Qt::UserRole).toLongLong());
        session.OnActivityCleared();
        refresh();
    });
    const auto save = [&dialog](const QString& name, const QString& filter,
                                const QByteArray& bytes) {
        const QString path = QFileDialog::getSaveFileName(&dialog, Tr("Export"), name, filter);
        if (path.isEmpty()) {
            return;
        }
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) {
            QMessageBox::warning(&dialog, Tr("Export"), Tr("The file could not be written."));
        }
    };
    QObject::connect(export_json, &QPushButton::clicked, &dialog, [&, save] {
        save(QStringLiteral("ppsspp-reader-activity.json"), QStringLiteral("JSON (*.json)"),
             QJsonDocument(database.ExportActivityJson()).toJson(QJsonDocument::Indented));
    });
    QObject::connect(export_csv, &QPushButton::clicked, &dialog, [&, save] {
        save(QStringLiteral("ppsspp-reader-sessions.csv"), QStringLiteral("CSV (*.csv)"),
             database.ExportSessionsCsv().toUtf8());
    });
    const auto clear = [&](const QString& game, const QString& question) {
        if (QMessageBox::question(&dialog, Tr("Clear statistics"), question) != QMessageBox::Yes) {
            return;
        }
        // Sessions and totals only: the backlog and what counts as already read stay.
        database.ClearActivity(game);
        session.OnActivityCleared();
        refresh();
    };
    QObject::connect(clear_game, &QPushButton::clicked, &dialog, [&, clear] {
        clear(session.GameKey(), Tr("Remove the sessions and totals of this game?"));
    });
    QObject::connect(strip, &QPushButton::clicked, &dialog, [&] {
        try {
            QApplication::setOverrideCursor(Qt::WaitCursor);
            const qint64 found = database.StripOverlaps(false);
            QApplication::restoreOverrideCursor();
            if (found == 0) {
                QMessageBox::information(&dialog, Tr("Remove text counted twice"),
                                         Tr("Nothing was counted twice."));
                return;
            }
            if (QMessageBox::question(&dialog, Tr("Remove text counted twice"),
                                      Tr("%1 characters were counted that had already stood in the lines just "
                                         "before them. Take them out of the counts?")
                                          .arg(Number(found))) != QMessageBox::Yes) {
                return;
            }
            QApplication::setOverrideCursor(Qt::WaitCursor);
            database.StripOverlaps(true);
            QApplication::restoreOverrideCursor();
        } catch (const std::exception& error) {
            QApplication::restoreOverrideCursor();
            QMessageBox::warning(&dialog, Tr("Remove text counted twice"), QString::fromUtf8(error.what()));
        }
        refresh();
    });
    QObject::connect(clear_all, &QPushButton::clicked, &dialog, [&, clear] {
        clear({}, Tr("Remove the sessions and totals of every game?"));
    });
    QObject::connect(close, &QPushButton::clicked, &dialog, &QDialog::accept);

    refresh();
    flush_and_refresh();
    dialog.exec();
}

// ---- Dictionaries ----

void Dictionaries(QWidget* parent) {
    auto& database = ReaderSession::Instance().Dictionaries();

    QDialog dialog(parent);
    dialog.setWindowTitle(Tr("Dictionaries"));
    dialog.resize(640, 460);
    auto* layout = new QVBoxLayout(&dialog);
    auto* hint = new QLabel(
        Tr("Dictionaries in the Yomitan format (.zip): term dictionaries for definitions, "
           "frequency and pitch dictionaries for the extra lines. The order of the list is the "
           "order entries are shown in. Nothing is bundled and nothing is downloaded."));
    hint->setWordWrap(true);
    layout->addWidget(hint);

    auto* list = new QListWidget;
    layout->addWidget(list, 1);
    auto* buttons = new QHBoxLayout;
    auto* import = new QPushButton(Tr("Import…"));
    auto* up = new QPushButton(Tr("Move up"));
    auto* down_button = new QPushButton(Tr("Move down"));
    auto* remove = new QPushButton(Tr("Delete"));
    auto* close = new QPushButton(Tr("Close"));
    buttons->addWidget(import);
    buttons->addWidget(up);
    buttons->addWidget(down_button);
    buttons->addWidget(remove);
    buttons->addStretch(1);
    buttons->addWidget(close);
    layout->addLayout(buttons);

    bool filling = false;
    const auto refresh = [&](qint64 select) {
        filling = true;
        list->clear();
        try {
            for (const ReaderApp::DictionaryInfo& info : database.Dictionaries()) {
                QString text = info.title;
                QStringList details;
                if (info.term_count > 0) {
                    details.append(Tr("%1 entries").arg(Number(info.term_count)));
                }
                if (info.meta_count > 0) {
                    details.append(Tr("%1 frequency or pitch entries").arg(Number(info.meta_count)));
                }
                if (!info.revision.isEmpty()) {
                    details.append(info.revision);
                }
                text += QStringLiteral("  —  ") + details.join(QStringLiteral(", "));
                auto* item = new QListWidgetItem(text, list);
                item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
                item->setCheckState(info.enabled ? Qt::Checked : Qt::Unchecked);
                item->setData(Qt::UserRole, info.id);
                if (info.id == select) {
                    list->setCurrentItem(item);
                }
            }
        } catch (const std::exception& error) {
            QMessageBox::warning(&dialog, Tr("Dictionaries"), QString::fromUtf8(error.what()));
        }
        filling = false;
    };
    const auto ids = [list] {
        QList<qint64> out;
        for (int i = 0; i < list->count(); ++i) {
            out.append(list->item(i)->data(Qt::UserRole).toLongLong());
        }
        return out;
    };
    const auto move = [&, ids](int by) {
        const int row = list->currentRow();
        const int target = row + by;
        if (row < 0 || target < 0 || target >= list->count()) {
            return;
        }
        QList<qint64> order = ids();
        order.swapItemsAt(row, target);
        database.SetOrder(order);
        refresh(order.at(target));
    };

    QObject::connect(list, &QListWidget::itemChanged, &dialog, [&](QListWidgetItem* item) {
        if (!filling) {
            database.SetEnabled(item->data(Qt::UserRole).toLongLong(),
                                item->checkState() == Qt::Checked);
        }
    });
    QObject::connect(up, &QPushButton::clicked, &dialog, [move] { move(-1); });
    QObject::connect(down_button, &QPushButton::clicked, &dialog, [move] { move(1); });
    QObject::connect(remove, &QPushButton::clicked, &dialog, [&] {
        QListWidgetItem* item = list->currentItem();
        if (!item) {
            return;
        }
        if (QMessageBox::question(&dialog, Tr("Delete dictionary"),
                                  Tr("Delete this dictionary from the app?")) != QMessageBox::Yes) {
            return;
        }
        const qint64 id = item->data(Qt::UserRole).toLongLong();
        QGuiApplication::setOverrideCursor(Qt::WaitCursor);
        RunBlocking([&database, id] {
            try {
                database.Delete(id);
            } catch (const std::exception& error) {
                LOG_ERROR(Frontend, "[Reader] Deleting a dictionary failed: {}", error.what());
            }
        });
        QGuiApplication::restoreOverrideCursor();
        refresh(-1);
    });
    QObject::connect(import, &QPushButton::clicked, &dialog, [&] {
        const QStringList files = QFileDialog::getOpenFileNames(
            &dialog, Tr("Import dictionaries"), QString(),
            Tr("Yomitan dictionaries (*.zip);;All files (*)"));
        QStringList problems;
        for (const QString& file : files) {
            QProgressDialog progress(Tr("Importing…"), Tr("Cancel"), 0, 100, &dialog);
            progress.setWindowModality(Qt::WindowModal);
            progress.setMinimumDuration(0);
            progress.setAutoClose(false);
            progress.setAutoReset(false);
            progress.setValue(0);
            std::atomic<bool> cancelled{false};
            QObject::connect(&progress, &QProgressDialog::canceled, &progress,
                             [&cancelled] { cancelled = true; });
            ReaderApp::DictionaryImporter::Result result;
            QEventLoop loop;
            QThreadPool pool;
            pool.start([&] {
                ReaderApp::DictionaryImporter importer(database);
                result = importer.Import(
                    file,
                    [&progress](int done, int total, const QString& title) {
                        QMetaObject::invokeMethod(
                            &progress,
                            [&progress, done, total, title] {
                                progress.setLabelText(Tr("Importing %1…").arg(title));
                                progress.setMaximum(std::max(total, 1));
                                progress.setValue(done);
                            },
                            Qt::QueuedConnection);
                    },
                    [&cancelled] { return !cancelled.load(); });
                QMetaObject::invokeMethod(&loop, [&loop] { loop.quit(); }, Qt::QueuedConnection);
            });
            loop.exec();
            pool.waitForDone();
            progress.close();
            if (!result.ok && !cancelled) {
                problems.append(result.error);
            }
        }
        refresh(-1);
        if (!problems.isEmpty()) {
            QMessageBox::warning(&dialog, Tr("Import dictionaries"), problems.join(u'\n'));
        }
    });
    QObject::connect(close, &QPushButton::clicked, &dialog, &QDialog::accept);

    refresh(-1);
    dialog.exec();
}

// ---- Anki ----

void AnkiSetup(QWidget* parent) {
    auto& settings = ReaderSession::Instance().Settings();
    using Settings = ReaderApp::ReaderSettings;

    QDialog dialog(parent);
    dialog.setWindowTitle(Tr("Anki setup"));
    dialog.resize(680, 620);
    auto* layout = new QVBoxLayout(&dialog);

    auto* enabled = new QCheckBox(Tr("Make cards in Anki, through the AnkiConnect add-on"));
    enabled->setChecked(settings.AnkiTransport() == QStringLiteral("ankiconnect"));
    layout->addWidget(enabled);

    auto* form = new QFormLayout;
    auto* url = new QLineEdit(settings.AnkiConnectUrl());
    auto* key = new QLineEdit(settings.AnkiConnectKey());
    key->setPlaceholderText(Tr("Only if you set one in AnkiConnect"));
    auto* connect_row = new QHBoxLayout;
    auto* connect_button = new QPushButton(Tr("Connect"));
    auto* connection = new QLabel;
    connection->setWordWrap(true);
    connect_row->addWidget(connect_button);
    connect_row->addWidget(connection, 1);
    auto* deck = new QComboBox;
    auto* model = new QComboBox;
    deck->setEditable(true);
    model->setEditable(true);
    deck->setCurrentText(settings.AnkiDeck());
    model->setCurrentText(settings.AnkiModel());
    form->addRow(Tr("Address"), url);
    form->addRow(Tr("API key"), key);
    form->addRow(QString(), connect_row);
    form->addRow(Tr("Deck"), deck);
    form->addRow(Tr("Note type"), model);
    layout->addLayout(form);

    auto* fields = new QTableWidget(0, 2);
    fields->setHorizontalHeaderLabels({Tr("Field"), Tr("What it gets")});
    fields->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    fields->horizontalHeader()->setStretchLastSection(true);
    fields->verticalHeader()->setVisible(false);
    layout->addWidget(fields, 1);
    auto* markers_hint = new QLabel(
        Tr("A field is a template: markers such as {expression}, {sentence}, {screenshot} or "
           "{audio}, with any text around them. Leave a field empty to leave it alone."));
    markers_hint->setWordWrap(true);
    layout->addWidget(markers_hint);
    auto* suggest = new QPushButton(Tr("Suggest from field names"));
    auto* suggest_row = new QHBoxLayout;
    suggest_row->addWidget(suggest);
    suggest_row->addStretch(1);
    layout->addLayout(suggest_row);

    auto* form2 = new QFormLayout;
    auto* tags = new QLineEdit(settings.AnkiTags());
    form2->addRow(Tr("Tags"), tags);
    layout->addLayout(form2);
    auto* duplicates = new QCheckBox(Tr("Allow duplicate cards"));
    duplicates->setChecked(settings.AnkiAllowDuplicates());
    auto* anywhere = new QCheckBox(Tr("A word in any deck counts as already there"));
    anywhere->setChecked(settings.AnkiDuplicatesAnywhere());
    layout->addWidget(duplicates);
    layout->addWidget(anywhere);

    auto* box = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    layout->addWidget(box);

    auto mapping = std::make_shared<QMap<QString, QString>>(
        ReaderApp::CardBuilder::ParseMapping(settings.AnkiFieldMap()));
    auto field_order = std::make_shared<QStringList>();

    const auto template_of = [fields](int row) {
        auto* combo = qobject_cast<QComboBox*>(fields->cellWidget(row, 1));
        return combo ? combo->currentText() : QString();
    };
    const auto read_table = [fields, mapping, template_of] {
        for (int row = 0; row < fields->rowCount(); ++row) {
            mapping->insert(fields->item(row, 0)->text(), template_of(row));
        }
    };
    const auto fill_table = [fields, mapping, field_order] {
        fields->setRowCount(0);
        QStringList names = *field_order;
        if (names.isEmpty()) {
            names = mapping->keys(); // not connected: what was saved, in no particular order
        }
        for (const QString& name : std::as_const(names)) {
            const int row = fields->rowCount();
            fields->insertRow(row);
            auto* item = new QTableWidgetItem(name);
            item->setFlags(item->flags() & ~Qt::ItemIsEditable);
            fields->setItem(row, 0, item);
            auto* combo = new QComboBox;
            combo->setEditable(true);
            combo->addItem(QString());
            for (const auto& [marker, description] : ReaderApp::CardBuilder::Markers()) {
                combo->addItem(u'{' + marker + u'}');
                combo->setItemData(combo->count() - 1, description, Qt::ToolTipRole);
            }
            combo->setCurrentText(mapping->value(name));
            fields->setCellWidget(row, 1, combo);
        }
    };
    const auto client = [url, key] {
        return ReaderApp::AnkiConnectClient(url->text().trimmed(), key->text(), 4000, 15000);
    };
    const auto load_fields = [&, read_table, fill_table, client] {
        const QString name = model->currentText().trimmed();
        if (name.isEmpty()) {
            return;
        }
        read_table();
        QStringList names;
        QString problem;
        QGuiApplication::setOverrideCursor(Qt::WaitCursor);
        RunBlocking([&names, &problem, name, client] {
            try {
                auto anki = client();
                names = anki.FieldNames(name);
            } catch (const ReaderApp::AnkiError& error) {
                problem = error.Message();
            }
        });
        QGuiApplication::restoreOverrideCursor();
        if (!problem.isEmpty()) {
            connection->setText(problem);
            return;
        }
        *field_order = names;
        fill_table();
    };
    QObject::connect(connect_button, &QPushButton::clicked, &dialog, [&, client, load_fields] {
        QStringList decks;
        QStringList models;
        QString problem;
        int version = 0;
        connection->setText(Tr("Asking Anki…"));
        QGuiApplication::setOverrideCursor(Qt::WaitCursor);
        RunBlocking([&decks, &models, &problem, &version, client] {
            try {
                auto anki = client();
                version = anki.Version();
                decks = anki.DeckNames();
                models = anki.ModelNames();
            } catch (const ReaderApp::AnkiError& error) {
                problem = error.Message();
            }
        });
        QGuiApplication::restoreOverrideCursor();
        if (!problem.isEmpty()) {
            connection->setText(problem);
            return;
        }
        connection->setText(Tr("Connected (AnkiConnect %1): %2 decks, %3 note types")
                                .arg(version)
                                .arg(decks.size())
                                .arg(models.size()));
        const QString chosen_deck = deck->currentText();
        const QString chosen_model = model->currentText();
        decks.sort(Qt::CaseInsensitive);
        models.sort(Qt::CaseInsensitive);
        {
            const QSignalBlocker block_deck(deck);
            const QSignalBlocker block_model(model);
            deck->clear();
            deck->addItems(decks);
            deck->setCurrentText(chosen_deck);
            model->clear();
            model->addItems(models);
            model->setCurrentText(chosen_model);
        }
        load_fields();
    });
    QObject::connect(model, &QComboBox::activated, &dialog, [load_fields] { load_fields(); });
    QObject::connect(suggest, &QPushButton::clicked, &dialog, [&, fill_table] {
        QStringList names;
        for (int row = 0; row < fields->rowCount(); ++row) {
            names.append(fields->item(row, 0)->text());
        }
        const auto suggested = ReaderApp::CardBuilder::SuggestMapping(names);
        for (auto it = suggested.constBegin(); it != suggested.constEnd(); ++it) {
            mapping->insert(it.key(), it.value());
        }
        fill_table();
    });
    QObject::connect(box, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QObject::connect(box, &QDialogButtonBox::accepted, &dialog, [&, read_table] {
        read_table();
        // Only the fields of the chosen note type are kept.
        QMap<QString, QString> kept;
        for (int row = 0; row < fields->rowCount(); ++row) {
            const QString name = fields->item(row, 0)->text();
            kept.insert(name, mapping->value(name));
        }
        settings.SetValue(Settings::KeyAnkiTransport, enabled->isChecked()
                                                          ? QStringLiteral("ankiconnect")
                                                          : QStringLiteral("none"));
        settings.SetValue(Settings::KeyAnkiConnectUrl, url->text().trimmed());
        settings.SetValue(Settings::KeyAnkiConnectKey, key->text());
        settings.SetValue(Settings::KeyAnkiDeck, deck->currentText().trimmed());
        settings.SetValue(Settings::KeyAnkiModel, model->currentText().trimmed());
        settings.SetValue(Settings::KeyAnkiFields, ReaderApp::CardBuilder::MappingToJson(kept));
        settings.SetValue(Settings::KeyAnkiTags, tags->text().trimmed());
        settings.SetValue(Settings::KeyAnkiDuplicates, duplicates->isChecked());
        settings.SetValue(Settings::KeyAnkiDuplicatesAnywhere, anywhere->isChecked());
        // The note type's first field is what Anki tells notes apart by: recorded while it
        // is known, so that asking "does Anki have this word" needs no extra question.
        if (!field_order->isEmpty()) {
            settings.SetValue(Settings::KeyAnkiKeyField,
                              Settings::KeyFieldValue(model->currentText().trimmed(),
                                                      field_order->first()));
        }
        settings.Sync();
        Miner::Forget();
        dialog.accept();
    });

    fill_table();
    dialog.exec();
}

// ---- Settings ----

void ReaderSettingsDialog(QWidget* parent) {
    auto& session = ReaderSession::Instance();
    auto& settings = session.Settings();
    using Settings = ReaderApp::ReaderSettings;

    // One page per section, the sections in a list beside them: each page is short.
    QDialog dialog(parent);
    dialog.setWindowTitle(Tr("Reader settings"));
    auto* outer = new QVBoxLayout(&dialog);
    auto* body = new QHBoxLayout;
    auto* sections = new QListWidget;
    sections->setFixedWidth(170);
    auto* pages = new QStackedWidget;
    body->addWidget(sections);
    body->addWidget(pages, 1);
    outer->addLayout(body, 1);
    QObject::connect(sections, &QListWidget::currentRowChanged, pages,
                     &QStackedWidget::setCurrentIndex);

    QList<std::function<void()>> savers;
    const auto section = [sections, pages](const QString& title) {
        auto* page = new QWidget;
        auto* layout = new QVBoxLayout(page);
        auto* form = new QFormLayout;
        layout->addLayout(form);
        layout->addStretch(1);
        pages->addWidget(page);
        sections->addItem(title);
        return form;
    };
    const auto check = [&](QFormLayout* form, const QString& label, const QString& key_name,
                           bool value) {
        auto* box = new QCheckBox(label);
        box->setChecked(value);
        form->addRow(box);
        savers.append([&settings, box, key_name] { settings.SetValue(key_name, box->isChecked()); });
        return box;
    };
    const auto spin = [&](QFormLayout* form, const QString& label, const QString& key_name,
                          int value, int low, int high, const QString& suffix) {
        auto* box = new QSpinBox;
        box->setRange(low, high);
        box->setValue(value);
        box->setSuffix(suffix);
        form->addRow(label, box);
        savers.append([&settings, box, key_name] { settings.SetValue(key_name, box->value()); });
        return box;
    };
    const auto choice = [&](QFormLayout* form, const QString& label, const QString& key_name,
                            const QList<std::pair<QString, QString>>& options,
                            const QString& value) {
        auto* box = new QComboBox;
        for (const auto& [text, data] : options) {
            box->addItem(text, data);
        }
        box->setCurrentIndex(std::max(0, box->findData(value)));
        form->addRow(label, box);
        savers.append(
            [&settings, box, key_name] { settings.SetValue(key_name, box->currentData()); });
        return box;
    };

    QFormLayout* text = section(Tr("Looking words up"));
    check(text, Tr("Click words in the game's own text"), Settings::KeyScreenText,
          settings.ScreenText());
    check(text, Tr("Play by touch: beside the text goes on, an answer is picked"),
          Settings::KeyTouchPlay, settings.TouchPlay());
    choice(text, Tr("By pointing at a word"), Settings::KeyHoverLookup,
           {{Tr("Off"), QStringLiteral("off")},
            {Tr("While Shift is held"), QStringLiteral("shift")},
            {Tr("Always"), QStringLiteral("always")}},
           settings.HoverLookup());
    spin(text, Tr("Popup text size"), Settings::KeyPopupScale, settings.PopupScale(), 60, 200,
         QStringLiteral(" %"));
    spin(text, Tr("Popup height, at most"), Settings::KeyPopupHeight,
         settings.PopupHeightPercent(), 10, 80, Tr(" % of the picture"));
    spin(text, Tr("Close the popup after"), Settings::KeyPopupAutoHide,
         settings.PopupAutoHideSeconds(), 0, 120, Tr(" s left alone (0: it stays)"));
    check(text, Tr("Close the popup when the pointer leaves its word and the popup"),
          Settings::KeyPopupHideOnExit, settings.PopupHideOnExit());
    choice(text, Tr("Where the add button stands"), Settings::KeyCardCorner,
           {{Tr("Bottom left"), QStringLiteral("bottom_left")},
            {Tr("Bottom right"), QStringLiteral("bottom_right")},
            {Tr("Top left"), QStringLiteral("top_left")},
            {Tr("Top right"), QStringLiteral("top_right")}},
           settings.CardCorner());
    choice(text, Tr("Mark the word looked up"), Settings::KeyWordMark,
           {{Tr("Outline"), QStringLiteral("outline")},
            {Tr("Highlight"), QStringLiteral("highlight")},
            {Tr("Not at all"), QStringLiteral("none")}},
           settings.WordMark());
    check(text, Tr("Outline what can be clicked"), Settings::KeyScreenTextOutline,
          settings.ScreenTextOutline());

    QFormLayout* cursor = section(Tr("Cursor"));
    choice(cursor, Tr("Button"), Settings::KeyCursorButton,
           {{Tr("None"), QStringLiteral("-1")},
            {QStringLiteral("L2"), QStringLiteral("104")},
            {QStringLiteral("R2"), QStringLiteral("105")},
            {QStringLiteral("L1"), QStringLiteral("102")},
            {QStringLiteral("R1"), QStringLiteral("103")},
            {Tr("Left stick, pressed"), QStringLiteral("106")},
            {Tr("Right stick, pressed"), QStringLiteral("107")},
            {Tr("Select"), QStringLiteral("109")},
            {Tr("Start"), QStringLiteral("108")}},
           QString::number(settings.CursorButton()));
    check(cursor, Tr("Press once instead of holding"), Settings::KeyCursorToggle,
          settings.CursorToggle());
    spin(cursor, Tr("Speed"), Settings::KeyCursorSpeed, settings.CursorSpeed(), 25, 300,
         QStringLiteral(" %"));
    check(cursor, Tr("Frame the character under the cursor"), Settings::KeyCursorFrame,
          settings.CursorFrame());
    check(cursor, Tr("Jump to the start of each new line"), Settings::KeyCursorHome,
          settings.CursorHome());

    QFormLayout* cards = section(Tr("Cards"));
    choice(cards, Tr("The line's audio"), Settings::KeyCardAudio,
           {{Tr("Ask for each card"), QStringLiteral("ask")},
            {Tr("Always add it"), QStringLiteral("always")},
            {Tr("Never add it"), QStringLiteral("never")}},
           Settings::CardAudioValue(settings.CardAudioGeneral()));
    check(cards, Tr("Keep the game's audio for cards"), Settings::KeyCaptureAudio,
          settings.CaptureAudio());
    spin(cards, Tr("Audio clip, at most"), Settings::KeyAudioMax, settings.AudioClipMaxSeconds(),
         3, 60, Tr(" s"));
    spin(cards, Tr("Start the clip earlier by"), Settings::KeyAudioLead, settings.AudioLeadMs(), 0,
         2000, Tr(" ms"));
    spin(cards, Tr("Screenshots kept"), Settings::KeyScreenshotCache,
         settings.ScreenshotCacheLines(), 0, 200, Tr(" lines"));
    spin(cards, Tr("Screenshot width, at most"), Settings::KeyScreenshotWidth,
         settings.ScreenshotMaxWidth(), 320, 3840, Tr(" px"));

    // The buttons on a controller's back, which no game uses.
    QFormLayout* back = section(Tr("Back buttons (Steam Controller grips)"));
    {
        const QList<std::pair<QString, QString>> actions{
            {Tr("Nothing"), QStringLiteral("none")},
            {Tr("Pause the game, to look words up"), QStringLiteral("reader_pause_game")},
            {Tr("Pause statistics"), QStringLiteral("reader_pause_stats")},
            {Tr("Make a card of the sentence"), QStringLiteral("reader_sentence_card")},
            {Tr("Backlog"), QStringLiteral("reader_backlog")},
            {Tr("Statistics"), QStringLiteral("reader_statistics")},
            {Tr("Close the popup"), QStringLiteral("reader_close_popup")},
            {Tr("The reader's menu"), QStringLiteral("reader_menu")}};
        const QStringList names{Tr("Grip 1"), Tr("Grip 2"), Tr("Grip 3"), Tr("Grip 4"), Tr("The \"...\" button")};
        for (int button = 1; button <= names.size(); ++button) {
            const QString now = settings.BackButtonAction(button);
            choice(back, names.at(button - 1), Settings::KeyBackButton + QString::number(button), actions,
                   now.isEmpty() ? QStringLiteral("none") : now);
        }
    }

    QFormLayout* stats = section(Tr("Statistics"));
    check(stats, Tr("Session line in a corner of the game"), Settings::KeySessionHud,
          settings.SessionHud());
    check(stats, Tr("Pause statistics (Ctrl+T)"), Settings::KeyStatsPaused, settings.StatsPaused());
    choice(stats, Tr("Button that pauses statistics"), Settings::KeyStatsButton,
           {{Tr("None"), QStringLiteral("-1")},
            {QStringLiteral("L2"), QStringLiteral("104")},
            {QStringLiteral("R2"), QStringLiteral("105")},
            {QStringLiteral("L1"), QStringLiteral("102")},
            {QStringLiteral("R1"), QStringLiteral("103")},
            {Tr("Left stick, pressed"), QStringLiteral("106")},
            {Tr("Right stick, pressed"), QStringLiteral("107")},
            {Tr("Select"), QStringLiteral("109")},
            {Tr("Start"), QStringLiteral("108")}},
           QString::number(settings.StatsButton()));
    spin(stats, Tr("Reading time stops after"), Settings::KeyIdleMinutes,
         static_cast<int>(settings.IdleTimeoutMs() / 60000), 1, 60, Tr(" min without a new line"));
    check(stats, Tr("Show the game, characters read and speed in my Discord status"),
          Settings::KeyDiscordEnabled, settings.DiscordEnabled());
    {
        auto* id = new QLineEdit(settings.DiscordApplicationId());
        id->setPlaceholderText(Tr("17 to 20 digits"));
        stats->addRow(Tr("Discord application ID"), id);
        savers.append([&settings, id] {
            settings.SetValue(Settings::KeyDiscordApplicationId, id->text().trimmed());
        });
        auto* about = new QLabel(
            Tr("The status reads \"Playing\" and the name of a Discord application of your "
               "own: make one at discord.com/developers/applications (New Application, any "
               "name) and copy its Application ID here."));
        about->setWordWrap(true);
        stats->addRow(about);
    }

    QFormLayout* output = section(Tr("Text output"));
    check(output, Tr("WebSocket text server"), Settings::KeyHookEnabled,
          settings.TextHookEnabled());
    spin(output, Tr("Port"), Settings::KeyHookPort, settings.TextHookPort(), 1024, 65535,
         QString());
    check(output, Tr("Allow other devices (no password)"), Settings::KeyHookLan,
          settings.TextHookLan());
    choice(output, Tr("Format"), Settings::KeyHookFormat,
           {{Tr("Plain text"), QStringLiteral("plain")},
            {QStringLiteral("{\"sentence\": …}"), QStringLiteral("sentence")},
            {Tr("Full JSON event"), QStringLiteral("json")}},
           settings.TextHookFormat());
    check(output, Tr("Speaker's name before the line"), Settings::KeyHookSpeaker,
          settings.TextHookSpeaker());
    check(output, Tr("Copy each new line to the clipboard"), Settings::KeyClipboardAuto,
          settings.ClipboardAuto());
    const QString address = session.TextHookAddress();
    if (!address.isEmpty()) {
        output->addRow(new QLabel(Tr("Listening at %1").arg(address)));
    }

    // The CRT picture: a page built from the table in reader/core/crt.h. What it changes shows in
    // the game's picture at once, and is put back when the window is left unsaved.
    const ReaderCrtPage::Page crt = ReaderCrtPage::Make(settings, &dialog);
    pages->addWidget(crt.widget);
    sections->addItem(Tr("CRT picture"));
    savers.append(crt.save);
    QObject::connect(&dialog, &QDialog::rejected, &dialog, crt.revert);

    sections->setCurrentRow(0);
    auto* box = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    outer->addWidget(box);
    QObject::connect(box, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QObject::connect(box, &QDialogButtonBox::accepted, &dialog, [&] {
        for (const auto& save : std::as_const(savers)) {
            save();
        }
        settings.Sync();
        dialog.accept();
    });
    dialog.resize(640, 440);
    dialog.exec();
}

} // namespace

void RunBacklogDialog(QWidget* parent) {
    Backlog(parent);
}

void RunStatisticsDialog(QWidget* parent) {
    Statistics(parent);
}

void RunDictionariesDialog(QWidget* parent) {
    Dictionaries(parent);
}

void RunAnkiSetupDialog(QWidget* parent) {
    AnkiSetup(parent);
}

void RunSettingsDialog(QWidget* parent) {
    ReaderSettingsDialog(parent);
}

} // namespace ReaderUi
