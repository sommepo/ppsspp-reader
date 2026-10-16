// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

#include <vector>
#include <limits>
#include <algorithm>
#include <cmath>
#include <exception>
#include <utility>
#include <QCursor>
#include <QAbstractButton>
#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QEvent>
#include <QGuiApplication>
#include <QMainWindow>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPushButton>
#include <QTimer>
#include <QUrl>
#include "reader/qt/host.h"
#include "reader/qt/reader_controller.h"
#include "reader/qt/reader_pad.h"
#include "reader/qt/reader_steam.h"
#include "reader/qt/reader_dialogs.h"
#include "reader/core/emulator.h"
#include "reader/core/engine.h"
#include "reader/core/input_filter.h"
#include "reader/qt/reader_games.h"
#include "reader/qt/reader_session.h"

namespace ReaderUi {

using ReaderApp::PlacedText;
using ReaderApp::PresentedLine;

namespace {

/// How far the pointer may wander between going down and up and still have clicked.
constexpr qreal ClickSlop = 8.0;
// Touch play: a press held this long on an answer looks it up instead of picking it; how long a
// button of the game is held; the steps of moving the cursor to an answer; the PSP's buttons
// (bits of its button word).
constexpr qint64 LongPressMs = 500;
constexpr int PressMs = 80;
constexpr int PickStepMs = 220;
constexpr int MaxPickPresses = 48;
constexpr int PspUp = 4, PspRight = 5, PspDown = 6, PspLeft = 7, PspCircle = 13;
/// How long the pointer rests on a word before it is looked up.
constexpr int HoverMs = 140;
constexpr int CaptureRetryMs = 60;
constexpr int CaptureTimeoutMs = 2500;

} // namespace

ReaderController::ReaderController(QMainWindow* main_window_, QWidget* render_window_, Host host_)
    : QObject(main_window_), main_window{main_window_}, render_window{render_window_},
      host{std::move(host_)} {
    auto& session = ReaderSession::Instance();

    overlay = new ReaderOverlay(main_window);
    overlay->hide();
    connect(overlay, &ReaderOverlay::ContentChanged, this, [this] { sync_timer.start(); });

    lookups = std::make_unique<LookupFlow>(main_window);
    lookups->SetHome(render_window);
    lookups->mark = [this](qint64 backlog_id, int first, int last) {
        mark_line = first < 0 ? -1 : backlog_id;
        mark_first = first;
        mark_last = last;
        UpdateMarks();
    };
    lookups->notify = [this](const QString& text) { Notify(text); };
    lookups->pointer_inside_now = [this] { return PointerInsideNow(); };
    // Once the window is up. Not when the app is driven by the development channel, which
    // asks for it itself (first-start).
    if (qEnvironmentVariableIsEmpty("READER_DESKTOP_CONTROL")) {
        QTimer::singleShot(1500, this, [this] { FirstStart(); });
    }
    lookups->open_dictionaries = [this] {
        RunDialog([](QWidget* parent) { RunDictionariesDialog(parent); });
    };

    connect(&session, &ReaderSession::ScreenTextChanged, this, [this] {
        RefreshPlaced();
        UpdateMarks();
        QList<qint64> lines;
        bool fresh = false;
        for (const PlacedText& text : std::as_const(placed)) {
            lines.append(text.presented.backlog_id);
            fresh = fresh || !placed_lines.contains(text.presented.backlog_id);
        }
        placed_lines = lines;
        if (cursor->IsActive()) {
            HomeCursor();
            if (fresh && !ReaderSession::Instance().Settings().CursorHome()) {
                // The cursor stayed where it was and a new text came under it: nobody
                // pointed at what is there now.
                cursor_pointed = false;
                hover_timer.stop();
                resting.reset();
                hovered_line = -1;
                hovered_offset = -1;
            }
            OnCursorMoved(false);
        }
    });
    connect(&session, &ReaderSession::LinePresented, this, [this](const PresentedLine& presented) {
        // The popup belongs to the line it was opened on: the next line takes it away.
        if (!presented.line.IsChoice()) {
            lookups->Close();
        }
    });
    connect(&session, &ReaderSession::SessionActivityChanged, this,
            &ReaderController::UpdateSessionLine);
    connect(&session, &ReaderSession::ActivityChanged, this, &ReaderController::UpdateSessionLine);
    connect(&session, &ReaderSession::ProfileStatusChanged, this, [this] {
        const ProfileStatus& status = ReaderSession::Instance().Profile();
        if (!status.error.isEmpty()) {
            Notify(tr("Text profile: %1").arg(status.error));
        }
        cursor->SetArmed(ReaderSession::Instance().IsActive() && status.loaded);
        UpdateMenu();
    });

    cursor = new StickCursor(this);
    cursor->refresh_sticks = [this] { RefreshSticks(); };
    connect(cursor, &StickCursor::ActiveChanged, this, &ReaderController::OnCursorActive);
    connect(cursor, &StickCursor::Moved, this, &ReaderController::OnCursorMoved);
    connect(cursor, &StickCursor::Clicked, this, &ReaderController::CursorClick);

    // The overlay and the popup are windows of their own: they follow the game's picture.
    sync_timer.setSingleShot(true);
    sync_timer.setInterval(0);
    connect(&sync_timer, &QTimer::timeout, this, &ReaderController::SyncWindows);
    render_window->installEventFilter(this);
    main_window->installEventFilter(this);

    hover_timer.setSingleShot(true);
    hover_timer.setInterval(HoverMs);
    connect(&hover_timer, &QTimer::timeout, this, [this] {
        if (resting && resting->kind == DownKind::Character) {
            // Quietly: a pointer resting on a comma is no reason to say that a comma is no
            // word.
            LookUp(resting->presented, resting->offset, resting->rows, resting->near_x, true);
            BeginWay(resting->position);
        }
    });

    BuildMenu();
    host.set_pointer_filter(this);
}

ReaderController::~ReaderController() {
    host.set_pointer_filter(nullptr);
    lookups.reset();
    delete overlay.data();
}

// ---- The menu ----

void ReaderController::BuildMenu() {
    menu = new QMenu(tr("&Reader"), main_window);
    steam = new SteamControllerBridge(this);
    pad = new ReaderPad(this);
    // Queued: a button may open one of the reader's windows, which runs the events itself
    // until it is closed, and that must not happen inside the reading of the controller.
    connect(pad, &ReaderPad::Pressed, this, &ReaderController::OnBackButton, Qt::QueuedConnection);
    const auto add = [this](const QString& text, const QString& name, auto&& slot) {
        QAction* action = menu->addAction(text);
        action->setObjectName(name);
        connect(action, &QAction::triggered, this, std::forward<decltype(slot)>(slot));
        return action;
    };

    action_status = menu->addAction(QString());
    action_status->setEnabled(false);
    // Statistics come first: what was read today is said in the entry itself.
    action_statistics = add(tr("&Statistics…"), QStringLiteral("reader_statistics"), [this] {
        RunDialog([](QWidget* parent) { RunStatisticsDialog(parent); });
        UpdateMenu();
        UpdateSessionLine();
    });
    add(tr("Game &scripts…"), QStringLiteral("reader_games"), [this] {
        RunDialog([](QWidget* parent) { RunGameScriptsDialog(parent); });
    });
    menu->addSeparator();
    action_sentence_card = add(tr("Make a &card of the sentence"),
                               QStringLiteral("reader_sentence_card"), [this] { MineSentence(); });
    action_backlog = add(tr("&Backlog…"), QStringLiteral("reader_backlog"), [this] {
        RunDialog([](QWidget* parent) { RunBacklogDialog(parent); });
    });
    menu->addSeparator();
    add(tr("&Dictionaries…"), QStringLiteral("reader_dictionaries"), [this] {
        RunDialog([](QWidget* parent) { RunDictionariesDialog(parent); });
    });
    add(tr("&Anki setup…"), QStringLiteral("reader_anki"), [this] {
        RunDialog([](QWidget* parent) { RunAnkiSetupDialog(parent); });
    });
    // A game's own choice comes before the general one in the settings.
    menu_card_audio = menu->addMenu(tr("&Line audio on this game's cards"));
    auto* audio_group = new QActionGroup(menu_card_audio);
    const QStringList audio_names{QString(), tr("Always add it"), tr("Ask for each card"),
                                  tr("Never add it")};
    for (int index = 0; index < audio_names.size(); ++index) {
        QAction* action = menu_card_audio->addAction(audio_names.at(index));
        action->setCheckable(true);
        action->setObjectName(QStringLiteral("reader_card_audio_%1").arg(index));
        audio_group->addAction(action);
        card_audio_actions.append(action);
        connect(action, &QAction::triggered, this, [this, index] {
            using CardAudio = ReaderApp::ReaderSettings::CardAudio;
            auto& session = ReaderSession::Instance();
            std::optional<CardAudio> choice;
            if (index == 1) {
                choice = CardAudio::Always;
            } else if (index == 2) {
                choice = CardAudio::Ask;
            } else if (index == 3) {
                choice = CardAudio::Never;
            }
            session.Settings().SetCardAudioOf(session.GameKey(), choice);
            session.Settings().Sync();
        });
    }
    add(tr("Reader s&ettings…"), QStringLiteral("reader_settings"), [this] {
        RunDialog([](QWidget* parent) { RunSettingsDialog(parent); });
        OnSettingsChanged();
    });
    menu->addSeparator();
    action_pause_stats = menu->addAction(tr("&Pause statistics"));
    action_pause_stats->setObjectName(QStringLiteral("reader_pause_stats"));
    action_pause_stats->setCheckable(true);
    connect(action_pause_stats, &QAction::toggled, this, [this](bool paused) {
        auto& session = ReaderSession::Instance();
        if (session.Settings().StatsPaused() != paused) {
            session.Settings().SetStatsPaused(paused);
            session.OnSettingsChanged();
            UpdateSessionLine();
        }
    });
    // The game stands still and what it shows stays up, with every word still to be looked
    // up: for text that does not wait (a cutscene's lines).
    action_pause_game = menu->addAction(tr("Pause the &game, to look words up"));
    action_pause_game->setObjectName(QStringLiteral("reader_pause_game"));
    action_pause_game->setCheckable(true);
    connect(action_pause_game, &QAction::triggered, this, [this](bool paused) {
        if (paused == user_paused) {
            return;
        }
        if (paused) {
            if (!(host.is_running && host.is_running()) || !host.pause) {
                action_pause_game->setChecked(false);
                return;
            }
            user_paused = true;
            host.pause();
        } else {
            user_paused = false;
            lookups->Close();
            if (host.resume) {
                host.resume();
            }
        }
        ReaderSession::Instance().SetReading((running || user_paused) && application_active &&
                                             dialogs_open == 0);
    });
    action_reload_profile =
        add(tr("&Reload text profile"), QStringLiteral("reader_reload_profile"), [this] {
            ReaderBridge::ReloadProfile();
            Games::Forget();
            Notify(tr("Text profile reloaded"));
        });
    add(tr("Open the game &scripts folder"), QStringLiteral("reader_profiles_folder"), [] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(ReaderBridge::UserProfileDirectory()));
    });
    // Not in the menu: for the development control channel, after it changed a setting.
    auto* apply = new QAction(menu);
    apply->setObjectName(QStringLiteral("reader_apply_settings"));
    connect(apply, &QAction::triggered, this, &ReaderController::OnSettingsChanged);
    auto* refresh = new QAction(menu);
    refresh->setObjectName(QStringLiteral("reader_refresh_menu"));
    connect(refresh, &QAction::triggered, this, &ReaderController::UpdateMenu);
    connect(menu, &QMenu::aboutToShow, this, &ReaderController::UpdateMenu);
    // In full screen there is no menu bar: the keys still work.
    action_sentence_card->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_D));
    action_backlog->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_H));
    action_pause_stats->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_T));
    action_pause_game->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_G));
    for (QAction* action : {action_sentence_card, action_backlog, action_pause_stats, action_pause_game}) {
        action->setShortcutContext(Qt::WindowShortcut);
        render_window->addAction(action);
    }
    UpdateMenu();
}

void ReaderController::UpdateMenu() {
    auto& session = ReaderSession::Instance();
    const bool active = session.IsActive();
    const ProfileStatus& status = session.Profile();
    QString text;
    if (!active) {
        text = tr("No game is running");
    } else if (status.loaded) {
        text = tr("Reading %1").arg(session.GameTitle());
    } else if (!status.error.isEmpty()) {
        text = tr("Text profile failed: %1").arg(status.error.left(80));
    } else {
        text = tr("No text profile for this game (%1)").arg(session.GameKey());
    }
    action_status->setText(text);
    // What was read today, in the entry that opens the statistics.
    QString today;
    try {
        const ReaderApp::ReadingCounters counters = session.Database().DayTotals(
            ReaderApp::ReadingRules::ReadingDay(QDateTime::currentMSecsSinceEpoch()));
        if (!counters.IsEmpty()) {
            today = tr("   (today %1)").arg(ReaderApp::SessionSummary::Format(counters));
        }
    } catch (const std::exception&) {
        // The entry says less.
    }
    action_statistics->setText(tr("&Statistics…") + today);
    using CardAudio = ReaderApp::ReaderSettings::CardAudio;
    const auto audio_name = [](CardAudio choice) {
        switch (choice) {
        case CardAudio::Always:
            return tr("always");
        case CardAudio::Never:
            return tr("never");
        case CardAudio::Ask:
            break;
        }
        return tr("ask");
    };
    menu_card_audio->setEnabled(active);
    const std::optional<CardAudio> said =
        ReaderApp::ReaderSettings::CardAudioOfValue(Games::AudioOf(session.GameKey()));
    card_audio_actions.at(0)->setText(
        said ? tr("As set for this game (%1)").arg(audio_name(*said))
             : tr("As in Reader settings (%1)").arg(audio_name(session.Settings().CardAudioGeneral())));
    const std::optional<CardAudio> own = session.Settings().CardAudioOf(session.GameKey());
    const int chosen =
        !own ? 0 : *own == CardAudio::Always ? 1 : *own == CardAudio::Ask ? 2 : 3;
    card_audio_actions.at(chosen)->setChecked(true);
    action_sentence_card->setEnabled(active && session.Current().has_value());
    action_reload_profile->setEnabled(active);
    const bool paused = session.Settings().StatsPaused();
    if (action_pause_stats->isChecked() != paused) {
        action_pause_stats->setChecked(paused);
    }
}

void ReaderController::OnBackButton(int button) {
    auto& session = ReaderSession::Instance();
    // The buttons are the reader's while a game is: not in the emulator's menus, and not
    // under one of the reader's own windows.
    if (!session.IsActive() || dialogs_open != 0 || (!running && !user_paused)) {
        return;
    }
    const QString name = session.Settings().BackButtonAction(button);
    if (name.isEmpty()) {
        return;
    }
    if (name == QStringLiteral("reader_menu")) {
        menu->popup(PointerPosition());
    } else if (name == QStringLiteral("reader_close_popup")) {
        lookups->Close();
    } else {
        TriggerAction(name);
    }
}

void ReaderController::GamePauseEnded() {
    if (!user_paused) {
        return;
    }
    user_paused = false;
    if (action_pause_game) {
        action_pause_game->setChecked(false);
    }
}

void ReaderController::TriggerAction(const QString& name) {
    if (QAction* action = menu->findChild<QAction*>(name)) {
        action->trigger();
    }
}

// ---- From the main window ----

void ReaderController::OnGameBooted(const QString& title_id, const QString& title) {
    auto& session = ReaderSession::Instance();
    session.Start(title_id, title);
    if (MediaCapture* media = session.Media()) {
        media->frame_source = [this](int max_width, std::function<void(const QImage&)> callback) {
            CaptureFrame(max_width, std::move(callback));
        };
    }
    hovered_line = -1;
    hovered_offset = -1;
    cursor_homed_for = -2;
    placed_lines.clear();
    ApplyCursorSettings();
    cursor->SetArmed(session.Profile().loaded);
    cursor->Attach();
    RefreshPlaced();
    UpdateSessionLine();
    UpdateMenu();
    sync_timer.start();
}

void ReaderController::OnGameStopped() {
    lookups->CancelQuestions();
    lookups->Close();
    cursor->SetArmed(false);
    cursor->Detach();
    running = false;
    ReaderSession::Instance().Stop();
    placed.clear();
    mark_line = -1;
    UpdateMarks();
    UpdateSessionLine();
    UpdateMenu();
    overlay->hide();
}

void ReaderController::OnEmulationRunning(bool running_) {
    running = running_;
    if (!running) {
        // A paused game hears of no button being let go.
        cursor->Cancel();
    } else {
        // It goes on: whoever made it go on, the reader's own pause is over.
        GamePauseEnded();
    }
    // Reading goes on while the game stands still at the user's word.
    ReaderSession::Instance().SetReading((running || user_paused) && application_active &&
                                         dialogs_open == 0);
    sync_timer.start();
}

void ReaderController::OnApplicationActive(bool active) {
    application_active = active;
    ReaderSession::Instance().SetReading((running || user_paused) && application_active &&
                                         dialogs_open == 0);
}

void ReaderController::OnSettingsChanged() {
    ReaderSession::Instance().OnSettingsChanged();
    Miner::Forget();
    ApplyCursorSettings();
    RefreshPlaced();
    UpdateMarks();
    UpdateSessionLine();
    UpdateMenu();
}

void ReaderController::FirstStart() {
    if (running) {
        return; // started straight into a game: at the next start
    }
    if (QApplication::activeModalWidget() != nullptr) {
        // A question of the emulator's own first start is up (keys, firmware): after it.
        QTimer::singleShot(2000, this, [this] { FirstStart(); });
        return;
    }

    auto& session = ReaderSession::Instance();
    auto& settings = session.Settings();
    // Asks once; true when the user wants the window.
    const auto wants = [&](const QString& key, const QString& title, const QString& question,
                           const QString& about, const QString& yes) {
        settings.SetValue(key, true);
        settings.Sync();
        QMessageBox box{main_window};
        box.setIcon(QMessageBox::Question);
        box.setWindowTitle(title);
        box.setText(question);
        box.setInformativeText(about);
        QPushButton* open = box.addButton(yes, QMessageBox::AcceptRole);
        box.addButton(tr("Later"), QMessageBox::RejectRole);
        box.setDefaultButton(open);
        box.exec();
        return box.clickedButton() == open;
    };
    using Settings = ReaderApp::ReaderSettings;
    if (!settings.SetupAsked(Settings::KeySetupDictionariesAsked)) {
        if (!session.Dictionaries().Dictionaries().isEmpty()) {
            settings.SetValue(Settings::KeySetupDictionariesAsked, true);
        } else if (wants(Settings::KeySetupDictionariesAsked, tr("Dictionaries"),
                         tr("Install a dictionary?"),
                         tr("A word you click in a game's text is looked up in the dictionaries "
                            "you install: dictionaries made for Yomitan (zip files), such as "
                            "Jitendex. Without one nothing can be looked up.\n\nAt any time: "
                            "Reader > Dictionaries…"),
                         tr("Install dictionaries…"))) {
            RunDialog([](QWidget* parent) { RunDictionariesDialog(parent); });
        }
    }
    if (!settings.SetupAsked(Settings::KeySetupAnkiAsked)) {
        if (!settings.AnkiDeck().isEmpty() && !settings.AnkiModel().isEmpty()) {
            settings.SetValue(Settings::KeySetupAnkiAsked, true);
        } else if (wants(Settings::KeySetupAnkiAsked, tr("Anki"),
                         tr("Make Anki cards from what you read?"),
                         tr("A word you looked up becomes a card in Anki, with its line, a "
                            "picture of the game and the line's sound. Anki needs its "
                            "AnkiConnect add-on for that, and has to be open.\n\nAt any time: "
                            "Reader > Anki setup…"),
                         tr("Set up Anki…"))) {
            RunDialog([](QWidget* parent) { RunAnkiSetupDialog(parent); });
        }
    }
    settings.Sync();
}

void ReaderController::RunDialog(const std::function<void(QWidget* parent)>& run) {
    lookups->Close();
    // The game waits while one of the reader's windows is in front, and goes on by itself
    // when it is left.
    const bool was_running = host.is_running && host.is_running();
    if (was_running && host.pause) {
        host.pause();
    }
    ++dialogs_open;
    ReaderSession::Instance().SetReading(false);
    run(main_window);
    --dialogs_open;
    if (was_running && host.resume) {
        host.resume();
    }
    ReaderSession::Instance().SetReading((running || user_paused) && application_active &&
                                         dialogs_open == 0);
}

// ---- Where the game's text is ----

QRectF ReaderController::PictureArea() const {
    return host.picture_rect ? host.picture_rect() : QRectF();
}

void ReaderController::RefreshPlaced() {
    auto& session = ReaderSession::Instance();
    placed.clear();
    if (!session.IsActive() || !session.Settings().ScreenText()) {
        overlay->SetOutlines({});
        return;
    }
    const QRectF area = PictureArea();
    if (area.isValid()) {
        for (const ReaderApp::ScreenText& text : session.OnScreen()) {
            if (auto one = PlacedText::Place(text.presented, text.layout, area)) {
                placed.append(*one);
            }
        }
    }
    overlay->SetOutlines(session.Settings().ScreenTextOutline() ? placed
                                                                : QList<PlacedText>{});
}

const QList<PlacedText>& ReaderController::Placed() {
    RefreshPlaced();
    return placed;
}

PlacedText::Hit ReaderController::HitAt(const QPointF& position) {
    PlacedText::Hit box;
    for (const PlacedText& text : std::as_const(placed)) {
        const PlacedText::Hit hit = text.HitTest(position);
        if (hit.kind == PlacedText::HitKind::Character) {
            return hit;
        }
        if (hit.kind == PlacedText::HitKind::Box && box.kind == PlacedText::HitKind::Outside) {
            box = hit;
        }
    }
    return box;
}

namespace {
/// What the popup keeps clear of: the row of the character looked up, across its text. The
/// popup stands right next to the word, over the text's other rows if need be.
QRectF RowOf(const PlacedText& text, const PlacedText::Glyph& glyph) {
    return QRectF(text.rows.left(), glyph.drawn.top(), text.rows.width(), glyph.drawn.height());
}
} // namespace

void ReaderController::UpdateMarks() {
    QList<QRectF> marks;
    if (mark_line >= 0 && mark_first >= 0) {
        for (const PlacedText& text : std::as_const(placed)) {
            if (text.presented.backlog_id == mark_line) {
                marks.append(text.Marks(mark_first, mark_last));
            }
        }
    }
    overlay->SetMarkStyle(ReaderSession::Instance().Settings().WordMark());
    overlay->SetMarks(marks);
}

void ReaderController::UpdateSessionLine() {
    auto& session = ReaderSession::Instance();
    QString text;
    if (session.IsActive() && session.Settings().SessionHud() && session.SessionActivity()) {
        text = ReaderApp::SessionSummary::Format(*session.SessionActivity(), false);
    }
    // Paused statistics show as a small mark in the corner, with or without the line.
    overlay->SetSessionLine(text, PictureArea(),
                            session.IsActive() && session.Settings().StatsPaused());
    UpdateDiscord();
}

void ReaderController::UpdateDiscord() {
    auto& session = ReaderSession::Instance();
    discord.Configure(session.Settings().DiscordEnabled(),
                      session.Settings().DiscordApplicationId());
    if (!session.IsActive()) {
        session_started_s = 0;
        discord.Set(std::nullopt);
        return;
    }
    if (session_started_s == 0) {
        session_started_s = QDateTime::currentSecsSinceEpoch();
    }
    ReaderApp::DiscordPresence::Activity activity;
    activity.game = session.GameTitle();
    if (const auto& counters = session.SessionActivity()) {
        activity.characters = counters->characters;
        activity.active_ms = counters->active_ms;
    }
    activity.started_s = session_started_s;
    activity.paused = session.Settings().StatsPaused();
    discord.Set(activity);
}

void ReaderController::Notify(const QString& text) {
    QRectF area = PictureArea();
    if (!area.isValid()) {
        area = QRectF(render_window->rect());
    }
    overlay->ShowMessage(text, area);
    LOG_INFO(Frontend, "[Reader] {}", text.toStdString());
}

// ---- Lookups ----

LookupFlow::Anchor ReaderController::AnchorFor(const QRectF& rows, qreal near_x) const {
    LookupFlow::Anchor anchor;
    anchor.owner = render_window;
    anchor.bounds = render_window->rect();
    anchor.keep_clear = rows.toAlignedRect();
    anchor.near_x = static_cast<int>(std::lround(near_x));
    return anchor;
}

void ReaderController::LookUp(const PresentedLine& presented, int offset, const QRectF& rows,
                              qreal near_x, bool quiet) {
    anchor_rows = rows;
    anchor_near_x = near_x;
    lookups->SetPopupParent(render_window->window());
    lookups->Lookup(presented, offset, AnchorFor(rows, near_x), quiet);
}

void ReaderController::MineSentence() {
    auto& session = ReaderSession::Instance();
    if (!session.Current()) {
        Notify(tr("No line has been read yet"));
        return;
    }
    session.NoteInteraction();
    lookups->MineSentence(*session.Current());
}

// ---- The pointer ----

bool ReaderController::OnPointerPress(const QPointF& position, bool left_button) {
    hover_timer.stop();
    resting.reset();
    down = Down{};
    if (!left_button) {
        return false;
    }
    auto& session = ReaderSession::Instance();
    if (!session.IsActive()) {
        return false;
    }
    down.position = position;
    if (lookups->IsAsking()) {
        // A card is waiting to hear about its audio: a click anywhere else calls it off, and
        // does nothing besides.
        lookups->CancelQuestions();
        down.kind = DownKind::Swallow;
        return true;
    }
    // The picture may have moved since the last look.
    RefreshPlaced();
    const PlacedText::Hit hit = HitAt(position);
    const bool pressing = PlayingByPresses();
    down.at = QDateTime::currentMSecsSinceEpoch();
    if (hit.kind == PlacedText::HitKind::Outside) {
        if (lookups->IsOpen()) {
            // Beside an open popup: the click closes it and never clicks what lies beneath.
            down.kind = DownKind::Scrim;
            return true;
        }
        if (pressing) {
            // Touch play: the player goes on; while a choice is up a click beside it would
            // confirm whatever answer is lit, so it does nothing (an answer is clicked).
            const bool choice = std::any_of(placed.begin(), placed.end(), [](const PlacedText& text) {
                return text.presented.line.IsChoice();
            });
            down.kind = choice ? DownKind::Swallow : DownKind::GoOn;
            return true;
        }
        return false;
    }
    // A press that begins inside a text box is the reader's: looking a word up cannot
    // advance the text or pick an answer.
    down.presented = hit.text->presented;
    down.rows = hit.text->rows;
    if (hit.kind == PlacedText::HitKind::Character) {
        down.kind = DownKind::Character;
        down.offset = hit.glyph->offset;
        down.near_x = hit.glyph->drawn.left();
        down.rows = RowOf(*hit.text, *hit.glyph);
        if (pressing && down.presented.line.IsChoice()) {
            // Touch play: an answer picked (a long press still looks its word up).
            down.kind = DownKind::Answer;
            down.answer = static_cast<int>(
                down.presented.line.text.left(down.offset).count(QLatin1Char('\n')));
        }
    } else if (pressing && !down.presented.line.IsChoice()) {
        down.kind = DownKind::GoOn; // in a message's box beside its words
    } else {
        down.kind = DownKind::Box;
    }
    return true;
}

void ReaderController::OnPointerMove(const QPointF& position) {
    if (down.kind != DownKind::None) {
        if (std::abs(position.x() - down.position.x()) > ClickSlop ||
            std::abs(position.y() - down.position.y()) > ClickSlop) {
            down.moved = true;
        }
        return;
    }
    PointerOver(position, false);
    HoverAt(position);
}

bool ReaderController::PointerInsideNow() {
    const LookupPopup* popup = lookups->Popup();
    if (!popup || !popup->isVisible()) {
        return false;
    }
    // The controller's cursor while it is in use, the mouse otherwise.
    const bool stick = cursor->IsActive();
    const QPoint global = stick ? render_window->mapToGlobal(cursor->Position().toPoint())
                                : PointerPosition();
    if (popup->geometry().contains(global)) {
        return true;
    }
    const QPointF position = stick ? cursor->Position()
                                   : QPointF(render_window->mapFromGlobal(global));
    const PlacedText::Hit hit = HitAt(position);
    return hit.kind == PlacedText::HitKind::Character &&
           lookups->IsAbout(hit.text->presented.backlog_id, hit.glyph->offset);
}

void ReaderController::PointerOver(const QPointF& position, bool on_popup) {
    if (!lookups->IsOpen()) {
        return;
    }
    // On the popup, on the word the popup is about, or on the way from the one to the other:
    // the popup is in use.
    bool inside = on_popup || OnWayToPopup(position);
    if (!inside) {
        const PlacedText::Hit hit = HitAt(position);
        inside = hit.kind == PlacedText::HitKind::Character &&
                 lookups->IsAbout(hit.text->presented.backlog_id, hit.glyph->offset);
    }
    lookups->PointerMoved(inside);
}

void ReaderController::OnPointerRelease(const QPointF&) {
    const Down released = down;
    down = Down{};
    if (released.moved) {
        return;
    }
    const bool held = QDateTime::currentMSecsSinceEpoch() - released.at >= LongPressMs;
    switch (released.kind) {
    case DownKind::GoOn:
        if (lookups->IsOpen()) {
            lookups->Close();
        } else {
            GoOn();
        }
        break;
    case DownKind::Answer:
        if (!held) {
            if (lookups->IsOpen()) {
                lookups->Close();
            } else {
                PickAnswer(released.answer);
            }
            break;
        }
        [[fallthrough]];
    case DownKind::Character:
        hovered_line = released.presented.backlog_id;
        hovered_offset = released.offset;
        LookUp(released.presented, released.offset, released.rows, released.near_x, false);
        BeginWay(released.position);
        break;
    case DownKind::Box:
    case DownKind::Scrim:
        lookups->Close();
        break;
    case DownKind::Swallow:
    case DownKind::None:
        break;
    }
}

bool ReaderController::PlayingByPresses() const {
    auto& session = ReaderSession::Instance();
    return session.IsActive() && session.Settings().TouchPlay() &&
           Games::TouchOf(session.GameKey()) == QStringLiteral("press");
}

void ReaderController::PressGameButton(int bit) {
    Reader::Emu::PressButton(bit, true);
    QTimer::singleShot(PressMs, this, [bit] { Reader::Emu::PressButton(bit, false); });
}

void ReaderController::GoOn() {
    PressGameButton(PspCircle);
}

void ReaderController::PickAnswer(int answer) {
    if (!Reader::Engine::Instance().Cursor()) {
        Notify(tr("This game's answers are picked with the controller: its script does not say "
                  "where the cursor is"));
        return;
    }
    pick_answer = answer;
    pick_presses = 0;
    pick_last = std::numeric_limits<int>::min();
    pick_stuck = 0;
    PickStep();
}

void ReaderController::PickStep() {
    const auto cursor = Reader::Engine::Instance().Cursor();
    if (!cursor || pick_answer < 0 || pick_presses >= MaxPickPresses) {
        pick_answer = -1;
        return;
    }
    if (cursor->index == pick_answer) {
        pick_answer = -1;
        GoOn();
        return;
    }
    // A press that leaves the cursor where it was, twice: something else has the input (a game's
    // lesson over its first choice wants the confirm button first). Confirm once.
    pick_stuck = cursor->index == pick_last ? pick_stuck + 1 : 0;
    pick_last = cursor->index;
    if (pick_stuck >= 2) {
        pick_stuck = 0;
        pick_last = std::numeric_limits<int>::min();
        GoOn();
        ++pick_presses;
        QTimer::singleShot(PickStepMs, this, [this] { PickStep(); });
        return;
    }
    const bool forward = cursor->index < pick_answer;
    PressGameButton(cursor->horizontal ? (forward ? PspRight : PspLeft) : (forward ? PspDown : PspUp));
    ++pick_presses;
    QTimer::singleShot(PickStepMs, this, [this] { PickStep(); });
}

void ReaderController::HoverAt(const QPointF& position) {
    auto& session = ReaderSession::Instance();
    if (!session.IsActive()) {
        return;
    }
    const QString mode = session.Settings().HoverLookup();
    const bool wanted =
        mode == QStringLiteral("always") ||
        (mode == QStringLiteral("shift") &&
         QGuiApplication::keyboardModifiers().testFlag(Qt::ShiftModifier));
    if (!wanted) {
        hover_timer.stop();
        resting.reset();
        hovered_line = -1;
        hovered_offset = -1;
        return;
    }
    RefreshPlaced();
    RestOn(position);
}

void ReaderController::BeginWay(const QPointF& from) {
    way_open = true;
    way_from = from;
    way_nearest = std::numeric_limits<qreal>::max();
}

bool ReaderController::OnWayToPopup(const QPointF& position) {
    if (!way_open) {
        return false;
    }
    const LookupPopup* popup = lookups->Popup();
    if (!lookups->IsOpen() || !popup || !popup->isVisible()) {
        // Not up yet (the dictionary is still being asked), or gone.
        return false;
    }
    const QRectF box{QPointF(render_window->mapFromGlobal(popup->geometry().topLeft())),
                     QSizeF(popup->size())};
    if (box.contains(position)) {
        return true;
    }
    // How far it still is to the popup. A pointer that turns back is going somewhere else.
    const qreal dx = std::max<qreal>({box.left() - position.x(), 0, position.x() - box.right()});
    const qreal dy = std::max<qreal>({box.top() - position.y(), 0, position.y() - box.bottom()});
    const qreal distance = std::hypot(dx, dy);
    constexpr qreal TurnedBack = 14;
    if (distance > way_nearest + TurnedBack) {
        way_open = false;
        return false;
    }
    way_nearest = std::min(way_nearest, distance);
    // Between the place the lookup was made from (with some room around it) and the popup:
    // the smallest shape without dents that holds both. A point is in it when it is on the
    // inner side of every edge.
    constexpr qreal Room = 18;
    const QRectF start{way_from.x() - Room, way_from.y() - Room, 2 * Room, 2 * Room};
    std::vector<QPointF> points{start.topLeft(), start.topRight(), start.bottomRight(),
                                start.bottomLeft(), box.topLeft(), box.topRight(),
                                box.bottomRight(), box.bottomLeft()};
    std::sort(points.begin(), points.end(), [](const QPointF& a, const QPointF& b) {
        return a.x() < b.x() || (a.x() == b.x() && a.y() < b.y());
    });
    const auto cross = [](const QPointF& o, const QPointF& a, const QPointF& b) {
        return (a.x() - o.x()) * (b.y() - o.y()) - (a.y() - o.y()) * (b.x() - o.x());
    };
    std::vector<QPointF> hull;
    for (int pass = 0; pass < 2; ++pass) {
        const std::size_t floor = hull.size() + 1;
        for (const QPointF& point : points) {
            while (hull.size() > floor && cross(hull[hull.size() - 2], hull.back(), point) <= 0) {
                hull.pop_back();
            }
            hull.push_back(point);
        }
        hull.pop_back();
        std::reverse(points.begin(), points.end());
    }
    for (std::size_t i = 0; i < hull.size(); ++i) {
        if (cross(hull[i], hull[(i + 1) % hull.size()], position) < 0) {
            // Off the way: from here on the pointer is like any other.
            way_open = false;
            return false;
        }
    }
    return true;
}

void ReaderController::RestOn(const QPointF& position) {
    if (OnWayToPopup(position)) {
        // What lies under the way to the popup is not looked up.
        hover_timer.stop();
        resting.reset();
        return;
    }
    const PlacedText::Hit hit = HitAt(position);
    if (hit.kind != PlacedText::HitKind::Character) {
        // Where no word begins nothing happens, and a popup that is open stays.
        hover_timer.stop();
        resting.reset();
        return;
    }
    const qint64 line = hit.text->presented.backlog_id;
    if (line == hovered_line && hit.glyph->offset == hovered_offset) {
        return; // still on the character that was looked up last
    }
    hovered_line = line;
    hovered_offset = hit.glyph->offset;
    Down target;
    target.kind = DownKind::Character;
    target.presented = hit.text->presented;
    target.offset = hit.glyph->offset;
    target.rows = RowOf(*hit.text, *hit.glyph);
    target.near_x = hit.glyph->drawn.left();
    target.position = position;
    resting = target;
    hover_timer.start();
}

// ---- The cursor, for a controller ----

void ReaderController::ApplyCursorSettings() {
    const auto& settings = ReaderSession::Instance().Settings();
    cursor->Configure(settings.CursorButton(), settings.CursorSpeed(), settings.CursorToggle());
    // The statistics button: a press pauses or resumes them. The cursor's button wins when
    // both are set to the same one. Input arrives on whatever thread reads the controller.
    const int stats_button = settings.StatsButton() == settings.CursorButton() ? -1
                                                                               : settings.StatsButton();
    Reader::InputFilter::Instance().SetPauseButton(stats_button, [this] {
        QMetaObject::invokeMethod(this, [this] { action_pause_stats->toggle(); },
                                  Qt::QueuedConnection);
    });
}

void ReaderController::RefreshSticks() {
    // The stick as the game last saw it would stay where it was while the cursor has it.
    Reader::Emu::RestSticks();
}

void ReaderController::OnCursorActive(bool active) {
    if (active) {
        RefreshPlaced();
        cursor->SetBounds(QRectF(render_window->rect()));
        // Put at the start of the text it points at nothing yet; left where it was, it has
        // not been pointed anywhere this time.
        cursor_pointed = ReaderSession::Instance().Settings().CursorHome();
        HomeCursor();
        OnCursorMoved(false);
    } else {
        hover_timer.stop();
        resting.reset();
        hovered_line = -1;
        hovered_offset = -1;
        overlay->SetCursor(false, {}, std::nullopt);
    }
}

/**
 * A new text has the cursor at its start, just before the first character, so that the way to
 * a word is always the same. Within one text the cursor stays where it was left. As a setting
 * it stays where it was left throughout, and only starts out in the middle of the picture.
 */
void ReaderController::HomeCursor() {
    const bool home = ReaderSession::Instance().Settings().CursorHome();
    if (home && !placed.isEmpty()) {
        const PlacedText& text = placed.first();
        const qint64 line = text.presented.backlog_id;
        if (cursor_homed_for == line) {
            return;
        }
        cursor_homed_for = line;
        const PlacedText::Glyph& first = text.characters.first();
        cursor->Place(QPointF(first.touch.left() - 1, first.drawn.center().y()));
    } else if (cursor_homed_for == -2) {
        cursor_homed_for = -1;
        QRectF area = PictureArea();
        if (!area.isValid()) {
            area = QRectF(render_window->rect());
        }
        cursor->Place(area.center());
    }
}

void ReaderController::ShowCursor() {
    if (!cursor->IsActive()) {
        return;
    }
    const QPointF position = cursor->Position();
    std::optional<QRectF> character;
    if (ReaderSession::Instance().Settings().CursorFrame()) {
        const PlacedText::Hit hit = HitAt(position);
        if (hit.kind == PlacedText::HitKind::Character) {
            character = hit.glyph->drawn;
        }
    }
    overlay->SetCursor(true, position, character);
}

void ReaderController::OnCursorMoved(bool by_player) {
    if (!cursor->IsActive()) {
        return;
    }
    if (by_player && !cursor_pointed) {
        // Moved by the player: from here on, what it rests on was pointed at.
        cursor_pointed = true;
        hovered_line = -1;
        hovered_offset = -1;
    }
    ShowCursor();
    const QPointF position = cursor->Position();
    const LookupPopup* popup = lookups->Popup();
    const bool over_popup =
        popup->isVisible() &&
        popup->geometry().contains(render_window->mapToGlobal(position.toPoint()));
    PointerOver(position, over_popup);
    if (!cursor_pointed || over_popup) {
        hover_timer.stop();
        return;
    }
    RestOn(position);
}

/// A click at the cursor, for whatever of the reader's is there: a word, the popup's box, or
/// nothing, which closes the popup as a click beside it would.
void ReaderController::CursorClick() {
    if (!cursor->IsActive()) {
        return;
    }
    const QPointF position = cursor->Position();
    LookupPopup* popup = lookups->Popup();
    const QPoint global = render_window->mapToGlobal(position.toPoint());
    if (AudioPrompt* prompt = lookups->Prompt();
        prompt->isVisible() && prompt->geometry().contains(global)) {
        // One of the question's buttons, or the space between them.
        if (auto* button =
                qobject_cast<QAbstractButton*>(prompt->childAt(prompt->mapFromGlobal(global)))) {
            button->click();
        }
        return;
    }
    if (popup->isVisible() && popup->geometry().contains(global)) {
        const QPoint local = popup->mapFromGlobal(global);
        for (const QEvent::Type type : {QEvent::MouseButtonPress, QEvent::MouseButtonRelease}) {
            QMouseEvent event(type, QPointF(local), QPointF(global), Qt::LeftButton,
                              type == QEvent::MouseButtonPress ? Qt::LeftButton : Qt::NoButton,
                              Qt::NoModifier);
            QCoreApplication::sendEvent(popup, &event);
        }
        return;
    }
    if (OnPointerPress(position, true)) {
        OnPointerRelease(position);
    }
}

// ---- The windows that lie over the game ----

bool ReaderController::eventFilter(QObject* watched, QEvent* event) {
    switch (event->type()) {
    case QEvent::Move:
    case QEvent::Resize:
    case QEvent::Show:
    case QEvent::Hide:
    case QEvent::ParentChange:
    case QEvent::WindowStateChange:
        sync_timer.start();
        break;
    default:
        break;
    }
    return QObject::eventFilter(watched, event);
}

void ReaderController::SyncWindows() {
    auto& session = ReaderSession::Instance();
    QWidget* top = render_window->window();
    // The render window is a window of its own at times (full screen, or when the game is not
    // shown inside the main window): what lies over it belongs to that window then.
    if (top != main_window && top != nullptr) {
        top->removeEventFilter(this);
        top->installEventFilter(this);
    }
    if (overlay->parentWidget() != top) {
        overlay->setParent(top, overlay->windowFlags());
    }
    lookups->SetPopupParent(top);

    const bool shown = session.IsActive() && render_window->isVisible() && top &&
                       !top->isMinimized() && dialogs_open == 0;
    const QRect global(render_window->mapToGlobal(QPoint(0, 0)), render_window->size());
    cursor->SetBounds(QRectF(render_window->rect()));
    if (overlay->geometry() != global) {
        overlay->setGeometry(global);
        // The picture's place in the window changes with the window's size.
        RefreshPlaced();
        UpdateMarks();
        UpdateSessionLine();
    }
    const bool overlay_wanted = shown && overlay->HasContent();
    if (overlay->isVisible() != overlay_wanted) {
        overlay->setVisible(overlay_wanted);
    }
    if (lookups->IsOpen()) {
        if (!shown) {
            lookups->Close();
        } else {
            lookups->Reposition(AnchorFor(anchor_rows, anchor_near_x));
        }
    }
    if (!shown && dialogs_open == 0) {
        lookups->CancelQuestions();
    }
}

// ---- The game's picture, for cards ----

void ReaderController::CaptureFrame(int max_width, std::function<void(const QImage&)> callback) {
    TryCapture(max_width, std::move(callback), 8);
}

void ReaderController::TryCapture(int max_width, std::function<void(const QImage&)> callback,
                                  int tries_left) {
    Q_UNUSED(tries_left);
    if (!host.is_powered_on || !host.is_powered_on() || !host.grab_picture) {
        callback(QImage());
        return;
    }
    const int width = std::max(max_width, 320);
    const QPointer<ReaderController> self(this);
    host.grab_picture([self, width, callback](const QImage& frame) {
        if (!self) {
            return;
        }
        QImage picture = frame;
        if (!picture.isNull() && picture.width() > width) {
            picture = picture.scaledToWidth(width, Qt::SmoothTransformation);
        }
        callback(picture);
    });
}

} // namespace ReaderUi
