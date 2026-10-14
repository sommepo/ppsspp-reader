// SPDX-FileCopyrightText: PPSSPP Reader fork contributors
// SPDX-License-Identifier: GPL-2.0-or-later

// The reader in the emulator's Qt frontend: what the windows ask of the emulator (host.h),
// answered with PPSSPP, and the one function the frontend calls to have the reader at all.

#include <deque>
#include <map>
#include <mutex>
#include <QApplication>
#include <QCursor>
#include <QDir>
#include <QDateTime>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QOpenGLWidget>
#include <QTimer>

#include "Common/Log.h"
#include "Common/System/Display.h"
#include "Core/Config.h"
#include "Core/Core.h"
#include "Core/ELF/ParamSFO.h"
#include "Core/Screenshot.h"
#include "Core/System.h"
#include "GPU/Common/PresentationCommon.h"

#include "reader/core/emulator.h"
#include "reader/qt/host.h"
#include "reader/qt/install.h"
#include "reader/qt/reader_control.h"
#include "reader/qt/reader_controller.h"
#include "reader/qt/reader_link.h"
#include "reader/ui/xmb.h"
#include "reader/qt/reader_session.h"
#include "reader/save_sync/host.h"

namespace ReaderUi {

void WriteLog(HostLog level, const std::string& message) {
    switch (level) {
    case HostLog::Info:
        INFO_LOG(Log::System, "%s", message.c_str());
        break;
    case HostLog::Warning:
        WARN_LOG(Log::System, "%s", message.c_str());
        break;
    case HostLog::Error:
        ERROR_LOG(Log::System, "%s", message.c_str());
        break;
    }
}

namespace {
std::mutex g_gui_tasks_mutex;
std::deque<std::function<void()>> g_gui_tasks;

/// On the main thread, from a timer: what the emulator's threads asked for.
void RunGuiTasks() {
    for (;;) {
        std::function<void()> task;
        {
            std::scoped_lock lock{g_gui_tasks_mutex};
            if (g_gui_tasks.empty()) {
                return;
            }
            task = std::move(g_gui_tasks.front());
            g_gui_tasks.pop_front();
        }
        task();
    }
}
} // namespace

void PostToGui(std::function<void()> task) {
    std::scoped_lock lock{g_gui_tasks_mutex};
    g_gui_tasks.push_back(std::move(task));
}

QString ReaderDirectory() {
    // Beside the emulator's own files on its memory stick folder: PSP/SYSTEM/reader.
    const QString directory =
        QString::fromStdString(GetSysDirectory(DIRECTORY_SYSTEM).ToString()) + QStringLiteral("/reader");
    QDir().mkpath(directory);
    return QDir::cleanPath(directory);
}

QString CacheDirectory() {
    const QString directory = ReaderDirectory() + QStringLiteral("/cache");
    QDir().mkpath(directory);
    return directory;
}

namespace {
bool g_pointer_said = false;
QPoint g_pointer_position;
} // namespace

QPoint PointerPosition() {
    return g_pointer_said ? g_pointer_position : QCursor::pos();
}

namespace {
QString g_surface_grab;
}

void RequestSurfaceGrab(const QString& path) {
    g_surface_grab = path;
}

void SurfaceDrawn(unsigned int framebuffer, int width, int height) {
    if (g_surface_grab.isEmpty() || width <= 0 || height <= 0) {
        return;
    }
    QOpenGLFunctions* gl = QOpenGLContext::currentContext()->functions();
    QImage picture(width, height, QImage::Format_RGBA8888);
    gl->glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    gl->glPixelStorei(GL_PACK_ALIGNMENT, 4);
    gl->glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, picture.bits());
    picture.convertToFormat(QImage::Format_RGB32).mirrored().save(g_surface_grab);
    g_surface_grab.clear();
}

void SetPointerPositionForTests(const QPoint& global) {
    g_pointer_said = true;
    g_pointer_position = global;
}

namespace {

ReaderPointerFilter* g_pointer_filter = nullptr;
bool g_has_pointer = false;
ReaderController* g_controller = nullptr;

bool GameInFront() {
    return PSP_IsInited() && GetUIState() == UISTATE_INGAME;
}

/// Where the game's picture is in the render widget, in the widget's own units.
QRectF PictureRect(QWidget* widget) {
    if (!PSP_IsInited() || g_display.pixel_xres <= 0 || g_display.pixel_yres <= 0 ||
        widget->width() <= 0) {
        return {};
    }
    const DisplayLayoutConfig& config = g_Config.GetDisplayLayoutConfig(g_display.GetDeviceOrientation());
    const FRect frame{0.0f, 0.0f, static_cast<float>(g_display.pixel_xres),
                      static_cast<float>(g_display.pixel_yres)};
    FRect rc{};
    CalculateDisplayOutputRect(config, &rc, 480.0f, 272.0f, frame, config.iInternalScreenRotation);
    // The emulator counts in pixels of its backbuffer; the widget in its own units.
    const qreal sx = static_cast<qreal>(widget->width()) / g_display.pixel_xres;
    const qreal sy = static_cast<qreal>(widget->height()) / g_display.pixel_yres;
    return QRectF(rc.x * sx, rc.y * sy, rc.w * sx, rc.h * sy);
}

} // namespace

bool PointerPress(const QPointF& position, bool left_button) {
    if (!g_pointer_filter || !GameInFront()) {
        return false;
    }
    if (g_pointer_filter->OnPointerPress(position, left_button)) {
        g_has_pointer = true;
        return true;
    }
    return false;
}

bool PointerMove(const QPointF& position) {
    if (!g_pointer_filter || !GameInFront()) {
        return false;
    }
    g_pointer_filter->OnPointerMove(position);
    return g_has_pointer;
}

bool PointerRelease(const QPointF& position) {
    if (!g_has_pointer) {
        return false;
    }
    g_has_pointer = false;
    if (g_pointer_filter) {
        g_pointer_filter->OnPointerRelease(position);
    }
    return true;
}


namespace {

/// What Reader Home is told of this app (reader_link.h).
const HomeLink::About& LinkAbout() {
    static const HomeLink::About about{QStringLiteral("ppsspp-reader"), QStringLiteral("PPSSPP Reader"),
                                       QStringLiteral("PlayStation Portable"),
                                       {QStringLiteral("--fullscreen"), QStringLiteral("%ROM%")}, {}};
    return about;
}

/// The reading statistics, for Reader Home's sums.
void TellHomeStats() {
    auto& session = ReaderSession::Instance();
    ReaderApp::ReaderDatabase* database = &session.Database();
    session.AfterWrites([database] {
        const QJsonObject record = database->StatsRecord();
        HomeLink::Write(LinkAbout(), nullptr, &record);
    });
}

/// The games whose text can be read, for Reader Home's library: asked again and again,
/// since a game's ID is read in the background; written when the answer has changed.
void TellHomeGames() {
    QList<HomeLink::Game> games;
    for (const ReaderXmb::ReadableGame& game : ReaderXmb::ReadableGames()) {
        games.append({QString::fromStdString(game.id), QString::fromStdString(game.title),
                      QDir::toNativeSeparators(QString::fromStdString(game.path)), QString()});
    }
    HomeLink::Write(LinkAbout(), &games, nullptr);
}

/// The reading statistics go along with the saves: every reading day of every game, whole
/// each time, under an ID of this install's own. Nothing is said when it does not arrive.
void SendStats() {
    if (!SaveSync::Host::Get().enabled) {
        return;
    }
    auto& session = ReaderSession::Instance();
    const std::string install = session.Settings().StatsInstall().toStdString();
    const QString device = QString::fromStdString(SaveSync::Host::Get().device);
    ReaderApp::ReaderDatabase* database = &session.Database();
    session.AfterWrites([install, device, database] {
        QJsonObject record = database->StatsRecord();
        record.insert(QStringLiteral("app"), QStringLiteral("ppsspp-reader"));
#ifdef _WIN32
        record.insert(QStringLiteral("platform"), QStringLiteral("windows"));
#else
        record.insert(QStringLiteral("platform"), QStringLiteral("linux"));
#endif
        record.insert(QStringLiteral("device"), device);
        record.insert(QStringLiteral("sent"), QDateTime::currentMSecsSinceEpoch());
        SaveSync::Host::SendStats(
            install, QJsonDocument(record).toJson(QJsonDocument::Compact).toStdString());
    });
}

} // namespace

void Install(QMainWindow* main_window, QOpenGLWidget* render_widget) {
    if (g_controller) {
        return;
    }
    ReaderSession::Instance();
    // The text engine reports to the app from here on.
    ReaderBridge::OnGameLoading();

    EmulatorHost host;
    host.is_powered_on = [] { return PSP_IsInited(); };
    host.is_running = [] { return GameInFront() && coreState != CORE_STEPPING_CPU; };
    // Around the reader's own windows the game stands still, the way it does while the
    // emulator's debugger has it.
    host.pause = [] {
        if (PSP_IsInited() && coreState != CORE_STEPPING_CPU) {
            Core_Break(BreakReason::UIFocus, 0);
        }
    };
    host.resume = [] {
        if (PSP_IsInited() && coreState == CORE_STEPPING_CPU) {
            Core_Resume();
        }
    };
    host.picture_rect = [render_widget] { return PictureRect(render_widget); };
    host.grab_picture = [](std::function<void(const QImage&)> callback) {
        if (!PSP_IsInited()) {
            callback(QImage());
            return;
        }
        // The emulator writes what the game is showing to a file at the end of a frame; the
        // picture is read back from it here.
        static int serial = 0;
        const QString file = CacheDirectory() + QStringLiteral("/frame-%1.png").arg(++serial);
        ScheduleScreenshot(Path(file.toStdString()), ScreenshotFormat::PNG, ScreenshotType::Display, 2,
                           [file, callback](ScreenshotResult result) {
                               QMetaObject::invokeMethod(
                                   qApp,
                                   [file, callback, result] {
                                       QImage picture;
                                       if (result == ScreenshotResult::Success) {
                                           picture.load(file);
                                       }
                                       QFile::remove(file);
                                       callback(picture.convertToFormat(QImage::Format_RGB32));
                                   },
                                   Qt::QueuedConnection);
                           });
    };
    host.set_pointer_filter = [](ReaderPointerFilter* filter) {
        g_pointer_filter = filter;
        g_has_pointer = false;
    };

    g_controller = new ReaderController(main_window, render_widget, std::move(host));
    main_window->menuBar()->addMenu(g_controller->Menu());

    // The emulator starts and stops games from its own screens and tells the engine; the
    // engine tells us, on the emulator's threads.
    // Nothing of Qt in these (PostToGui says why): plain strings go over to the main thread.
    auto* gui_tasks = new QTimer(g_controller);
    gui_tasks->setInterval(4);
    QObject::connect(gui_tasks, &QTimer::timeout, g_controller, [] { RunGuiTasks(); });
    gui_tasks->start();
    ReaderBridge::PrepareFiles();
    Reader::Emu::SetGameLoadedCallback([](const std::string& game_id) {
        ReaderBridge::OnGameStarted(); // the profile goes in before the game's code runs
        const std::string title = g_paramSFO.GetValueString("TITLE");
        PostToGui([game_id, title] {
            g_controller->OnGameBooted(QString::fromStdString(game_id),
                                       QString::fromStdString(title));
        });
    });
    Reader::Emu::SetGameStoppedCallback([] {
        PostToGui([] {
            ReaderBridge::OnGameStopped();
            g_controller->OnGameStopped();
            SendStats();
            TellHomeStats();
        });
    });
    // A game's stop is not the only moment the statistics go out: a window closed with its game
    // still running stops nothing, and so sent nothing. Also soon after the app starts (what
    // the last run counted) and every few minutes of a game.
    QTimer::singleShot(10000, g_controller, [] { SendStats(); });
    auto* stats_timer = new QTimer(g_controller);
    stats_timer->setInterval(5 * 60 * 1000);
    QObject::connect(stats_timer, &QTimer::timeout, g_controller, [] {
        if (ReaderSession::Instance().IsActive()) {
            SendStats();
        }
    });
    stats_timer->start();
    TellHomeStats();
    auto* told = new QTimer(g_controller);
    told->setInterval(5000);
    QObject::connect(told, &QTimer::timeout, g_controller, [] { TellHomeGames(); });
    told->start();
    QTimer::singleShot(1500, g_controller, [] { TellHomeGames(); });
    // The Reader entry of the emulator's own pause and main screens: the same menu.
    // The game list's mark for a game that can be read: a bundled profile, or one of the user's.
    Reader::Emu::SetProfileCheck([](const std::string& game_id) {
        const QString name = QString::fromStdString(game_id) + QStringLiteral(".lua");
        return QFile::exists(QStringLiteral(":/reader/profiles/") + name) ||
               QFile::exists(ReaderSession::DataDirectory() + QStringLiteral("/profiles/") + name);
    });
    Reader::Emu::SetMenuCallback([](const std::string& what) {
        // The main screen names a page; the emulator's other menus ask for the Reader menu.
        static const std::map<std::string, QString> pages{
            {"stats", QStringLiteral("reader_statistics")},
            {"games", QStringLiteral("reader_games")},
            {"dictionaries", QStringLiteral("reader_dictionaries")},
            {"anki", QStringLiteral("reader_anki")},
            {"settings", QStringLiteral("reader_settings")},
        };
        PostToGui([what] {
            const auto page = pages.find(what);
            if (page == pages.end()) {
                g_controller->Menu()->popup(PointerPosition());
            } else {
                g_controller->TriggerAction(page->second);
            }
        });
    });

    // Whether the game is in front (not paused, not behind the emulator's menus) is asked.
    auto* watch = new QTimer(g_controller);
    watch->setInterval(150);
    QObject::connect(watch, &QTimer::timeout, g_controller, [] {
        static bool was = false;
        const bool now = GameInFront() && coreState != CORE_STEPPING_CPU;
        if (now != was) {
            was = now;
            g_controller->OnEmulationRunning(now);
        }
    });
    watch->start();
    QObject::connect(qApp, &QGuiApplication::applicationStateChanged, g_controller,
                     [](Qt::ApplicationState state) {
                         g_controller->OnApplicationActive(state == Qt::ApplicationActive);
                     });

    ReaderControl::StartIfWanted(main_window, render_widget, g_controller);
}

} // namespace ReaderUi
