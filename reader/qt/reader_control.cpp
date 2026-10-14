// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cmath>
#include <exception>
#include <functional>
#include <QAbstractButton>
#include <QApplication>
#include <QComboBox>
#include <QCursor>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocalServer>
#include <QLocalSocket>
#include <QMainWindow>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QTableWidget>
#include <QTimer>
#include <QTreeWidget>
#include "reader/qt/host.h"
#include "reader/core/engine.h"
#include "reader/core/emulator.h"
#include "reader/core/input_filter.h"
#include "reader/qt/reader_control.h"
#include "reader/qt/reader_pad.h"
#include "reader/qt/reader_steam.h"
#include "reader/qt/reader_controller.h"
#include "reader/qt/reader_games.h"
#include "reader/qt/reader_session.h"

namespace ReaderUi {

namespace {

constexpr int PointerPress = 0;
constexpr int PointerMove = 1;
constexpr int PointerRelease = 2;

QJsonArray Rect(const QRectF& rect) {
    const auto round = [](qreal value) { return std::round(value * 100.0) / 100.0; };
    return QJsonArray{round(rect.x()), round(rect.y()), round(rect.width()), round(rect.height())};
}

QString Json(const QJsonObject& object) {
    return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
}

QString Plain(QString text) {
    text.remove(u'&');
    return text;
}

} // namespace

void ReaderControl::StartIfWanted(QMainWindow* main_window, QWidget* render_window,
                                  ReaderController* controller) {
    const QString path = qEnvironmentVariable("READER_DESKTOP_CONTROL");
    if (path.isEmpty()) {
        return;
    }
    new ReaderControl(path, main_window, render_window, controller);
}

ReaderControl::ReaderControl(const QString& path, QMainWindow* main_window_,
                             QWidget* render_window_, ReaderController* controller_)
    : QObject(main_window_), main_window{main_window_}, render_window{render_window_},
      controller{controller_} {
    QLocalServer::removeServer(path);
    server = new QLocalServer(this);
    if (!server->listen(path)) {
        LOG_ERROR(Frontend, "[Reader] The control channel cannot listen at {}: {}",
                  path.toStdString(), server->errorString().toStdString());
        return;
    }
    LOG_INFO(Frontend, "[Reader] Control channel at {}", path.toStdString());
    connect(server, &QLocalServer::newConnection, this, [this] {
        while (QLocalSocket* socket = server->nextPendingConnection()) {
            connections.insert(socket, {});
            connect(socket, &QLocalSocket::readyRead, this, [this, socket] { OnData(socket); });
            connect(socket, &QLocalSocket::disconnected, this, [this, socket] {
                connections.remove(socket);
                socket->deleteLater();
            });
            OnData(socket);
        }
    });
}

void ReaderControl::OnData(QLocalSocket* socket) {
    auto found = connections.find(socket);
    if (found == connections.end()) {
        return;
    }
    found->buffer += socket->readAll();
    while (true) {
        const qsizetype end = found->buffer.indexOf('\n');
        if (end < 0) {
            break;
        }
        const QString line = QString::fromUtf8(found->buffer.left(end)).trimmed();
        found->buffer.remove(0, end + 1);
        if (!line.isEmpty()) {
            found->commands.append(line);
        }
    }
    Pump(socket);
}

void ReaderControl::Pump(QLocalSocket* socket) {
    auto found = connections.find(socket);
    if (found == connections.end() || found->busy || found->commands.isEmpty()) {
        return;
    }
    found->busy = true;
    const QString command = found->commands.takeFirst();
    const QPointer<QLocalSocket> alive(socket);
    Run(command, [this, alive](const QString& answer) {
        if (!alive) {
            return;
        }
        alive->write(answer.toUtf8() + '\n');
        alive->flush();
        auto connection = connections.find(alive.data());
        if (connection != connections.end()) {
            connection->busy = false;
            // The next command after this one has gone through the event loop: a click's
            // effects are in place before what follows is run.
            QTimer::singleShot(0, this, [this, alive] {
                if (alive) {
                    Pump(alive.data());
                }
            });
        }
    });
}

void ReaderControl::SendPointer(const QPointF& position, int kind,
                                Qt::KeyboardModifiers modifiers) {
    const QPoint local = position.toPoint();
    const QPoint global = render_window->mapToGlobal(local);
    // Where the pointer is, for whoever asks where it is now rather than where the event was.
    SetPointerPositionForTests(global);
    const QEvent::Type type = kind == PointerPress     ? QEvent::MouseButtonPress
                              : kind == PointerRelease ? QEvent::MouseButtonRelease
                                                       : QEvent::MouseMove;
    const Qt::MouseButton button = kind == PointerMove ? Qt::NoButton : Qt::LeftButton;
    const Qt::MouseButtons buttons = kind == PointerPress ? Qt::LeftButton : Qt::NoButton;
    QMouseEvent event(type, QPointF(local), QPointF(global), button, buttons, modifiers);
    QCoreApplication::sendEvent(render_window, &event);
}

QString ReaderControl::Status() const {
    auto& session = ReaderSession::Instance();
    QJsonObject root;
    root.insert(QStringLiteral("active"), session.IsActive());
    // Game frames counted so far: says whether the game is going on.
    root.insert(QStringLiteral("frame"), static_cast<qint64>(Reader::Engine::Instance().Frame()));
    root.insert(QStringLiteral("title_id"), session.GameKey());
    root.insert(QStringLiteral("title"), session.GameTitle());
    const ProfileStatus& profile = session.Profile();
    root.insert(QStringLiteral("profile"),
                QJsonObject{{QStringLiteral("loaded"), profile.loaded},
                            {QStringLiteral("hooks"), profile.hooks},
                            {QStringLiteral("error"), profile.error}});
    if (session.Current()) {
        root.insert(QStringLiteral("current"),
                    QJsonObject{{QStringLiteral("id"), session.Current()->backlog_id},
                                {QStringLiteral("speaker"), session.Current()->line.speaker},
                                {QStringLiteral("text"), session.Current()->line.text},
                                {QStringLiteral("window"), session.Current()->line.window}});
    }
    if (session.CurrentChoices()) {
        root.insert(QStringLiteral("choices"), session.CurrentChoices()->line.text);
    }
    if (session.SessionActivity()) {
        root.insert(QStringLiteral("session"),
                    QJsonObject{{QStringLiteral("characters"), session.SessionActivity()->characters},
                                {QStringLiteral("lines"), session.SessionActivity()->lines},
                                {QStringLiteral("active_ms"), session.SessionActivity()->active_ms}});
    }
    root.insert(QStringLiteral("window"),
                QJsonArray{render_window->width(), render_window->height()});
    root.insert(QStringLiteral("render_visible"), render_window->isVisible());
    root.insert(QStringLiteral("picture"), Rect(controller->PictureArea()));

    QJsonArray texts;
    for (const ReaderApp::PlacedText& text : controller->Placed()) {
        QJsonArray characters;
        for (const ReaderApp::PlacedText::Glyph& glyph : text.characters) {
            characters.append(QJsonObject{{QStringLiteral("offset"), glyph.offset},
                                          {QStringLiteral("row"), glyph.row},
                                          {QStringLiteral("cell"), Rect(glyph.drawn)}});
        }
        texts.append(QJsonObject{{QStringLiteral("window"), text.window},
                                 {QStringLiteral("id"), text.presented.backlog_id},
                                 {QStringLiteral("text"), text.presented.line.text},
                                 {QStringLiteral("box"), Rect(text.box)},
                                 {QStringLiteral("rows"), Rect(text.rows)},
                                 {QStringLiteral("characters"), characters}});
    }
    root.insert(QStringLiteral("texts"), texts);

    const ReaderOverlay* overlay = controller->Overlay();
    QJsonArray marks;
    for (const QRectF& mark : overlay->Marks()) {
        marks.append(Rect(mark));
    }
    root.insert(QStringLiteral("overlay"),
                QJsonObject{{QStringLiteral("visible"), overlay->isVisible()},
                            {QStringLiteral("geometry"), Rect(overlay->geometry())},
                            {QStringLiteral("marks"), marks},
                            {QStringLiteral("session"), overlay->SessionLine()},
                            {QStringLiteral("stats_paused_mark"), overlay->PauseMark()},
                            {QStringLiteral("message"), overlay->Message()}});

    const LookupPopup* popup = controller->Lookups().Popup();
    QJsonObject popup_state{{QStringLiteral("open"), popup->isVisible()}};
    if (popup->isVisible()) {
        const QPoint origin =
            popup->mapToGlobal(QPoint(0, 0)) - render_window->mapToGlobal(QPoint(0, 0));
        popup_state.insert(QStringLiteral("rect"), Rect(QRectF(origin, popup->size())));
        popup_state.insert(QStringLiteral("text"), popup->PlainText());
        QJsonArray boxes;
        for (int i = 0; i < popup->ResultCount(); ++i) {
            const auto state = popup->GetCardState(i);
            boxes.append(QJsonObject{
                {QStringLiteral("state"), state == LookupPopup::CardState::Add       ? QStringLiteral("add")
                                          : state == LookupPopup::CardState::Working ? QStringLiteral("working")
                                                                                     : QStringLiteral("in-deck")},
                {QStringLiteral("add"), Rect(popup->AddBox(i))},
                {QStringLiteral("book"), Rect(popup->BookBox(i))}});
        }
        popup_state.insert(QStringLiteral("results"), boxes);
        popup_state.insert(QStringLiteral("scrolled"), popup->Scrolled());
    }
    root.insert(QStringLiteral("popup"), popup_state);
    const AudioPrompt* prompt = controller->Lookups().Prompt();
    QJsonObject asking{{QStringLiteral("up"), controller->Lookups().IsAsking()}};
    if (prompt->isVisible()) {
        const QPoint origin =
            prompt->mapToGlobal(QPoint(0, 0)) - render_window->mapToGlobal(QPoint(0, 0));
        asking.insert(QStringLiteral("rect"), Rect(QRectF(origin, prompt->size())));
        asking.insert(QStringLiteral("playing"), prompt->IsPlaying());
    }
    root.insert(QStringLiteral("question"), asking);
    root.insert(QStringLiteral("dialog"), QApplication::activeModalWidget() != nullptr);
    return Json(root);
}

QString ReaderControl::DescribeDialog() const {
    QWidget* dialog = QApplication::activeModalWidget();
    if (!dialog) {
        return QStringLiteral("{}");
    }
    QJsonObject root{{QStringLiteral("class"), QString::fromLatin1(dialog->metaObject()->className())},
                     {QStringLiteral("title"), dialog->windowTitle()},
                     {QStringLiteral("size"), QJsonArray{dialog->width(), dialog->height()}}};
    QJsonArray labels;
    for (const QLabel* label : dialog->findChildren<QLabel*>()) {
        if (label->isVisible() && !label->text().isEmpty()) {
            labels.append(label->text());
        }
    }
    root.insert(QStringLiteral("labels"), labels);
    QJsonArray buttons;
    for (const QAbstractButton* button : dialog->findChildren<QAbstractButton*>()) {
        if (button->isVisible() && !button->text().isEmpty()) {
            buttons.append(QJsonObject{{QStringLiteral("text"), Plain(button->text())},
                                       {QStringLiteral("checked"), button->isChecked()},
                                       {QStringLiteral("enabled"), button->isEnabled()}});
        }
    }
    root.insert(QStringLiteral("buttons"), buttons);
    QJsonArray edits;
    for (const QLineEdit* edit : dialog->findChildren<QLineEdit*>()) {
        if (edit->isVisible()) {
            edits.append(edit->text());
        }
    }
    root.insert(QStringLiteral("edits"), edits);
    QJsonArray lists;
    for (const QListWidget* list : dialog->findChildren<QListWidget*>()) {
        QJsonArray items;
        for (int i = 0; i < std::min(list->count(), 12); ++i) {
            items.append(list->item(i)->text());
        }
        lists.append(QJsonObject{{QStringLiteral("count"), list->count()},
                                 {QStringLiteral("items"), items}});
    }
    root.insert(QStringLiteral("lists"), lists);
    QJsonArray trees;
    for (const QTreeWidget* tree : dialog->findChildren<QTreeWidget*>()) {
        QJsonArray rows;
        for (int i = 0; i < std::min(tree->topLevelItemCount(), 8); ++i) {
            QStringList columns;
            for (int column = 0; column < tree->columnCount(); ++column) {
                columns.append(tree->topLevelItem(i)->text(column));
            }
            rows.append(columns.join(QStringLiteral(" | ")));
        }
        trees.append(QJsonObject{{QStringLiteral("count"), tree->topLevelItemCount()},
                                 {QStringLiteral("rows"), rows}});
    }
    root.insert(QStringLiteral("trees"), trees);
    QJsonArray tables;
    for (const QTableWidget* table : dialog->findChildren<QTableWidget*>()) {
        QJsonArray rows;
        for (int row = 0; row < table->rowCount(); ++row) {
            QString value;
            if (const auto* combo = qobject_cast<QComboBox*>(table->cellWidget(row, 1))) {
                value = combo->currentText();
            }
            rows.append(QString(table->item(row, 0)->text() + QStringLiteral(" = ") + value));
        }
        tables.append(rows);
    }
    root.insert(QStringLiteral("tables"), tables);
    return Json(root);
}

void ReaderControl::Grab(const QString& file, const Reply& reply) {
    // What a player would see: the game's picture where the window shows it, and over it
    // what the reader draws and the popup.
    controller->CaptureFrame(1920, [this, file, reply](const QImage& frame) {
        QImage canvas(render_window->size(), QImage::Format_ARGB32_Premultiplied);
        canvas.fill(Qt::black);
        QPainter painter(&canvas);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        if (!frame.isNull()) {
            painter.drawImage(controller->PictureArea(), frame);
        }
        ReaderOverlay* overlay = controller->Overlay();
        if (overlay->isVisible()) {
            painter.drawPixmap(0, 0, overlay->grab());
        }
        LookupPopup* popup = controller->Lookups().Popup();
        if (popup->isVisible()) {
            painter.drawPixmap(popup->mapToGlobal(QPoint(0, 0)) -
                                   render_window->mapToGlobal(QPoint(0, 0)),
                               popup->grab());
        }
        AudioPrompt* prompt = controller->Lookups().Prompt();
        if (prompt->isVisible()) {
            painter.drawPixmap(prompt->mapToGlobal(QPoint(0, 0)) -
                                   render_window->mapToGlobal(QPoint(0, 0)),
                               prompt->grab());
        }
        painter.end();
        reply(canvas.save(file) ? QStringLiteral("OK %1%2").arg(file, frame.isNull()
                                                                          ? QStringLiteral(" (no frame)")
                                                                          : QString())
                                : QStringLiteral("ERR cannot write ") + file);
    });
}

void ReaderControl::Run(const QString& command, const Reply& reply) {
    const QStringList parts = command.split(u' ', Qt::SkipEmptyParts);
    const QString name = parts.value(0);
    const QString rest = command.mid(name.size()).trimmed();
    const auto number = [&parts](int index) { return parts.value(index).toDouble(); };
    const auto ok = [&reply](const QString& text = {}) {
        reply(text.isEmpty() ? QStringLiteral("OK") : QStringLiteral("OK ") + text);
    };
    try {
        if (name == QStringLiteral("status")) {
            ok(Status());
        } else if (name == QStringLiteral("click")) {
            const QPointF at(number(1), number(2));
            SendPointer(at, PointerPress, Qt::NoModifier);
            SendPointer(at, PointerRelease, Qt::NoModifier);
            ok();
        } else if (name == QStringLiteral("press")) {
            SendPointer(QPointF(number(1), number(2)), PointerPress, Qt::NoModifier);
            ok();
        } else if (name == QStringLiteral("release")) {
            SendPointer(QPointF(number(1), number(2)), PointerRelease, Qt::NoModifier);
            ok();
        } else if (name == QStringLiteral("move")) {
            SendPointer(QPointF(number(1), number(2)), PointerMove, Qt::NoModifier);
            ok();
        } else if (name == QStringLiteral("key")) {
            // A key of the keyboard, held for a moment: the game's buttons are bound to keys.
            const QKeySequence sequence(parts.value(1));
            if (sequence.isEmpty()) {
                reply(QStringLiteral("ERR unknown key"));
                return;
            }
            const int key = sequence[0].key();
            const int hold = parts.size() > 2 ? parts.value(2).toInt() : 100;
            QKeyEvent down(QEvent::KeyPress, key, Qt::NoModifier);
            QCoreApplication::sendEvent(render_window, &down);
            QTimer::singleShot(hold, this, [this, key, reply] {
                QKeyEvent up(QEvent::KeyRelease, key, Qt::NoModifier);
                QCoreApplication::sendEvent(render_window, &up);
                reply(QStringLiteral("OK"));
            });
        } else if (name == QStringLiteral("psp")) {
            // psp BIT [MS]: a button of the PSP itself, by its bit in the button word (13
            // circle, 14 cross, 3 start, 0 select, 4 up, 5 right, 6 down, 7 left), held a moment.
            const int bit = parts.value(1).toInt();
            const int hold = parts.size() > 2 ? parts.value(2).toInt() : 100;
            Reader::Emu::PressButton(bit, true);
            QTimer::singleShot(hold, this, [bit, reply] {
                Reader::Emu::PressButton(bit, false);
                reply(QStringLiteral("OK"));
            });
        } else if (name == QStringLiteral("steam-report")) {
            // steam-report HEX: as if a Steam Controller had sent this report; the answer says
            // what the bridge has done so far. "steam-report" alone only asks.
            if (!rest.isEmpty()) {
                controller->Steam()->InjectReport(QByteArray::fromHex(rest.toLatin1()));
            }
            ok(controller->Steam()->Describe());
        } else if (name == QStringLiteral("back-button")) {
            // back-button N: as if back button N (1 to 5) of a controller had been pressed.
            controller->Pad()->Inject(parts.value(1).toInt());
            ok(QStringLiteral("%1 controller(s) with back buttons; game paused by the user: %2")
                   .arg(controller->Pad()->Sources())
                   .arg(controller->GamePausedByUser() ? QStringLiteral("yes") : QStringLiteral("no")));
        } else if (name == QStringLiteral("cursor-button")) {
            // The cursor's button as an emulated controller would report it: down or up.
            auto& filter = Reader::InputFilter::Instance();
            const int button = ReaderSession::Instance().Settings().CursorButton();
            const bool taken = filter.OnButton(this, static_cast<std::size_t>(std::max(button, 0)),
                                               parts.value(1) == QStringLiteral("down"));
            ok(taken ? QStringLiteral("taken") : QStringLiteral("passed to the game"));
        } else if (name == QStringLiteral("stick")) {
            // A stick: which (0 left, 1 right), then x and y from -1 to 1, up being positive.
            const bool taken = Reader::InputFilter::Instance().OnStick(
                static_cast<std::size_t>(parts.value(1).toInt()), static_cast<float>(number(2)),
                static_cast<float>(number(3)));
            ok(taken ? QStringLiteral("taken") : QStringLiteral("passed to the game"));
        } else if (name == QStringLiteral("stick-press")) {
            auto& filter = Reader::InputFilter::Instance();
            const bool down = filter.OnButton(this, 4, true);
            const bool up = filter.OnButton(this, 4, false);
            ok(down && up ? QStringLiteral("taken") : QStringLiteral("passed to the game"));
        } else if (name == QStringLiteral("cursor")) {
            const StickCursor* cursor = controller->Cursor();
            ok(Json(QJsonObject{{QStringLiteral("active"), cursor->IsActive()},
                                {QStringLiteral("x"), cursor->Position().x()},
                                {QStringLiteral("y"), cursor->Position().y()}}));
        } else if (name == QStringLiteral("resize")) {
            // The main window, to a size of the render window: W H.
            const QSize wanted(static_cast<int>(number(1)), static_cast<int>(number(2)));
            main_window->resize(main_window->size() + wanted - render_window->size());
            ok();
        } else if (name == QStringLiteral("toplevels")) {
            QJsonArray windows;
            for (QWidget* widget : QApplication::topLevelWidgets()) {
                if (!widget->isVisible()) {
                    continue;
                }
                QJsonObject window{{QStringLiteral("class"),
                                    QString::fromLatin1(widget->metaObject()->className())},
                                   {QStringLiteral("title"), widget->windowTitle()},
                                   {QStringLiteral("geometry"), Rect(widget->geometry())}};
                if (const auto* popup = qobject_cast<LookupPopup*>(widget)) {
                    window.insert(QStringLiteral("text"), popup->PlainText());
                }
                windows.append(window);
            }
            ok(QString::fromUtf8(QJsonDocument(windows).toJson(QJsonDocument::Compact)));
        } else if (name == QStringLiteral("toplevel-grab")) {
            // A visible window by its class name, then the file.
            const QString wanted = parts.value(1);
            const QString file = rest.mid(wanted.size()).trimmed();
            for (QWidget* widget : QApplication::topLevelWidgets()) {
                if (widget->isVisible() &&
                    QString::fromLatin1(widget->metaObject()->className()).endsWith(wanted)) {
                    ok(widget->grab().save(file) ? file : QStringLiteral("(not written)"));
                    return;
                }
            }
            reply(QStringLiteral("ERR no such window"));
        } else if (name == QStringLiteral("wait")) {
            QTimer::singleShot(std::clamp(parts.value(1).toInt(), 0, 60000), this,
                               [reply] { reply(QStringLiteral("OK")); });
        } else if (name == QStringLiteral("grab")) {
            Grab(rest, reply);
        } else if (name == QStringLiteral("shot")) {
            controller->CaptureFrame(1920, [rest, reply](const QImage& frame) {
                reply(!frame.isNull() && frame.save(rest) ? QStringLiteral("OK ") + rest
                                                          : QStringLiteral("ERR no frame"));
            });
        } else if (name == QStringLiteral("grab-window")) {
            // The emulator's picture is OpenGL's and not in a widget's grab.
            RequestSurfaceGrab(rest);
            ok(rest);
        } else if (name == QStringLiteral("action")) {
            // Not from here: a window of the reader's runs a loop of its own until it is
            // closed, and this command is answered before that.
            QTimer::singleShot(0, controller, [this, rest] { controller->TriggerAction(rest); });
            ok();
        } else if (name == QStringLiteral("dialog")) {
            ok(DescribeDialog());
        } else if (name == QStringLiteral("dialog-grab")) {
            QWidget* dialog = QApplication::activeModalWidget();
            if (dialog && dialog->grab().save(rest)) {
                ok(rest);
            } else {
                reply(QStringLiteral("ERR no window to grab"));
            }
        } else if (name == QStringLiteral("dialog-button")) {
            QWidget* dialog = QApplication::activeModalWidget();
            QAbstractButton* wanted = nullptr;
            if (dialog) {
                for (QAbstractButton* button : dialog->findChildren<QAbstractButton*>()) {
                    if (button->isVisible() && Plain(button->text()).contains(rest)) {
                        wanted = button;
                        break;
                    }
                }
            }
            if (!wanted) {
                reply(QStringLiteral("ERR no such button"));
                return;
            }
            QTimer::singleShot(0, wanted, [wanted] { wanted->click(); });
            ok(Plain(wanted->text()));
        } else if (name == QStringLiteral("dialog-select")) {
            // Makes the first row of a list or tree in the open window whose text has TEXT the
            // current one, as a click on it would.
            QWidget* dialog = QApplication::activeModalWidget();
            if (dialog) {
                for (QTreeWidget* tree : dialog->findChildren<QTreeWidget*>()) {
                    const auto found = tree->findItems(rest, Qt::MatchContains | Qt::MatchRecursive);
                    if (!found.isEmpty()) {
                        tree->setCurrentItem(found.first());
                        tree->scrollToItem(found.first());
                        ok(found.first()->text(0).section(u'\n', 0, 0));
                        return;
                    }
                }
                for (QListWidget* list : dialog->findChildren<QListWidget*>()) {
                    const auto found = list->findItems(rest, Qt::MatchContains);
                    if (!found.isEmpty()) {
                        list->setCurrentItem(found.first());
                        ok(found.first()->text().section(u'\n', 0, 0));
                        return;
                    }
                }
            }
            reply(QStringLiteral("ERR no such row"));
        } else if (name == QStringLiteral("dialog-close")) {
            if (QWidget* dialog = QApplication::activeModalWidget()) {
                QTimer::singleShot(0, dialog, [dialog] { dialog->close(); });
                ok();
            } else {
                reply(QStringLiteral("ERR no window open"));
            }
        } else if (name == QStringLiteral("dialog-click")) {
            // A click at a point of the reader's window that is open: X Y, in its own
            // coordinates.
            QWidget* dialog = QApplication::activeModalWidget();
            QWidget* target = dialog ? dialog->childAt(QPoint(static_cast<int>(number(1)),
                                                              static_cast<int>(number(2))))
                                     : nullptr;
            if (!target) {
                reply(QStringLiteral("ERR nothing there"));
                return;
            }
            const QPoint local = target->mapFrom(dialog, QPoint(static_cast<int>(number(1)),
                                                                static_cast<int>(number(2))));
            const QPoint global = target->mapToGlobal(local);
            for (const QEvent::Type type : {QEvent::MouseButtonPress, QEvent::MouseButtonRelease}) {
                QMouseEvent event(type, QPointF(local), QPointF(global), Qt::LeftButton,
                                  type == QEvent::MouseButtonPress ? Qt::LeftButton : Qt::NoButton,
                                  Qt::NoModifier);
                QCoreApplication::sendEvent(target, &event);
            }
            ok(QString::fromLatin1(target->metaObject()->className()));
        } else if (name == QStringLiteral("popup-add") || name == QStringLiteral("popup-book")) {
            LookupPopup* popup = controller->Lookups().Popup();
            const int index = parts.value(1).toInt();
            const QRect box = name == QStringLiteral("popup-add") ? popup->AddBox(index)
                                                                  : popup->BookBox(index);
            if (!popup->isVisible() || box.isNull()) {
                reply(QStringLiteral("ERR no such box"));
                return;
            }
            const QPoint local = box.center();
            for (const QEvent::Type type : {QEvent::MouseButtonPress, QEvent::MouseButtonRelease}) {
                QMouseEvent event(type, QPointF(local), QPointF(popup->mapToGlobal(local)),
                                  Qt::LeftButton,
                                  type == QEvent::MouseButtonPress ? Qt::LeftButton : Qt::NoButton,
                                  Qt::NoModifier);
                QCoreApplication::sendEvent(popup, &event);
            }
            ok();
        } else if (name == QStringLiteral("badge")) {
            // A game list picture as the game of this ID would get it.
            QPixmap picture(128, 128);
            picture.fill(QColor(0x4A, 0x6A, 0x8A));
            const QString file = rest.mid(parts.value(1).size()).trimmed();
            const QString id = parts.value(1);
            ok(QStringLiteral("covered=%1 %2")
                   .arg(Games::Covers(id))
                   .arg(Games::Badged(picture, id).save(file) ? file : QStringLiteral("(not written)")));
        } else if (name == QStringLiteral("menu")) {
            // What the Reader menu says right now.
            controller->TriggerAction(QStringLiteral("reader_refresh_menu"));
            QJsonArray entries;
            const std::function<void(const QMenu*, const QString&)> walk =
                [&entries, &walk](const QMenu* menu, const QString& indent) {
                    for (const QAction* action : menu->actions()) {
                        if (action->isSeparator()) {
                            continue;
                        }
                        entries.append(QString(indent + Plain(action->text()) +
                                               (action->isChecked() ? QStringLiteral(" [x]") : QString()) +
                                               (action->isEnabled() ? QString() : QStringLiteral(" (off)"))));
                        if (action->menu()) {
                            walk(action->menu(), indent + QStringLiteral("  "));
                        }
                    }
                };
            walk(controller->Menu(), QString());
            ok(QString::fromUtf8(QJsonDocument(entries).toJson(QJsonDocument::Compact)));
        } else if (name == QStringLiteral("answer")) {
            // The question about a card's audio: listen, keep or drop.
            AudioPrompt* prompt = controller->Lookups().Prompt();
            QAbstractButton* wanted = nullptr;
            const QList<QAbstractButton*> buttons = prompt->findChildren<QAbstractButton*>();
            const int index = rest == QStringLiteral("listen") ? 0
                              : rest == QStringLiteral("keep") ? 1
                              : rest == QStringLiteral("drop") ? 2
                                                               : -1;
            if (prompt->isVisible() && index >= 0 && index < buttons.size()) {
                wanted = buttons.at(index);
            }
            if (!wanted) {
                reply(QStringLiteral("ERR no question is up"));
                return;
            }
            wanted->click();
            ok(Plain(wanted->text()));
        } else if (name == QStringLiteral("import-dictionary")) {
            ReaderApp::DictionaryImporter importer(ReaderSession::Instance().Dictionaries());
            const auto result = importer.Import(rest);
            if (result.ok) {
                ok(QStringLiteral("%1: %2 terms, %3 meta").arg(result.title).arg(result.terms).arg(result.metas));
            } else {
                reply(QStringLiteral("ERR ") + result.error);
            }
        } else if (name == QStringLiteral("set")) {
            const QString key = parts.value(1);
            const QString value = rest.mid(key.size()).trimmed();
            ReaderSession::Instance().Settings().SetValue(key, value);
            ReaderSession::Instance().Settings().Sync();
            controller->TriggerAction(QStringLiteral("reader_apply_settings"));
            ok();
        } else if (name == QStringLiteral("main-action")) {
            // One of the main window's own actions by its name, such as action_Stop (stop
            // the game) or action_Exit (close the app as its user would).
            QAction* action = main_window->findChild<QAction*>(rest);
            if (!action) {
                reply(QStringLiteral("ERR no such action"));
                return;
            }
            QTimer::singleShot(0, action, [action] { action->trigger(); });
            ok();
        } else if (name == QStringLiteral("first-start")) {
            // The questions of the first start (save sync, dictionaries, Anki), which this
            // channel otherwise keeps away. Each is asked once per install.
            QTimer::singleShot(0, controller, [this] { controller->FirstStart(); });
            ok();
        } else if (name == QStringLiteral("backlog")) {
            auto& session = ReaderSession::Instance();
            QJsonArray lines;
            for (const ReaderApp::BacklogEntry& entry :
                 session.Database().RecentLines(session.GameKey(), std::max(1, parts.value(1).toInt()))) {
                lines.append(QJsonObject{{QStringLiteral("id"), entry.id},
                                         {QStringLiteral("speaker"), entry.line.speaker},
                                         {QStringLiteral("text"), entry.line.text},
                                         {QStringLiteral("reread"), entry.reread}});
            }
            ok(QString::fromUtf8(QJsonDocument(lines).toJson(QJsonDocument::Compact)));
        } else {
            reply(QStringLiteral("ERR unknown command: ") + name);
        }
    } catch (const std::exception& error) {
        reply(QStringLiteral("ERR ") + QString::fromUtf8(error.what()));
    }
}

} // namespace ReaderUi
