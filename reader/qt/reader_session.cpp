// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

#include <iterator>
#include <mutex>
#include <map>
#include <fstream>
#include <filesystem>
#include <string>
#include <utility>
#include <QClipboard>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QVariant>
#include <format>
#include "reader/qt/host.h"
#include "reader/core/audio_tap.h"
#include "reader/core/engine.h"
#include "reader/core/input_filter.h"
#include "reader/qt/reader_media.h"
#include "reader/qt/reader_crt_page.h"
#include "reader/qt/reader_session.h"

namespace ReaderUi {

using ReaderApp::PresentedLine;
using ReaderApp::ReadingCounters;
using ReaderApp::ScreenLayout;
using ReaderApp::TextLine;

namespace {

constexpr qint64 DuplicateWindowMs = 1500;
constexpr int HeartbeatMs = 1000;
constexpr qint64 FlushIntervalMs = 15000;
constexpr int RecentLimit = 300;

qint64 Now() {
    return QDateTime::currentMSecsSinceEpoch();
}

TextLine FromEvent(const Reader::TextEvent& event) {
    TextLine line;
    line.sequence = static_cast<qint64>(event.sequence);
    line.frame = static_cast<qint64>(event.frame);
    line.audio_position = static_cast<qint64>(event.audio_position);
    line.channel = event.channel.empty() ? ReaderApp::ChannelDialogue()
                                         : QString::fromStdString(event.channel);
    line.speaker = QString::fromStdString(event.speaker);
    line.text = QString::fromStdString(event.text);
    for (const Reader::TextEvent::Ruby& ruby : event.ruby) {
        line.ruby.append(ReaderApp::RubySpan{static_cast<int>(ruby.start),
                                             static_cast<int>(ruby.length),
                                             QString::fromStdString(ruby.reading)});
    }
    line.source_id = QString::fromStdString(event.source_id);
    if (!event.meta_json.empty()) {
        const QJsonObject meta =
            QJsonDocument::fromJson(QByteArray::fromStdString(event.meta_json)).object();
        for (auto it = meta.constBegin(); it != meta.constEnd(); ++it) {
            line.meta.insert(it.key(), it.value().toVariant().toString());
        }
    }
    line.window = QString::fromStdString(event.window);
    return line;
}

std::optional<QRectF> FromRect(const std::optional<Reader::ScreenRect>& rect) {
    if (!rect || rect->width < 0 || rect->height < 0) {
        return std::nullopt;
    }
    return QRectF(rect->x, rect->y, rect->width, rect->height);
}

ScreenLayout FromLayout(const Reader::TextLayout& layout) {
    ScreenLayout out;
    out.window = QString::fromStdString(layout.window);
    out.sequence = static_cast<qint64>(layout.sequence);
    out.closed = layout.closed;
    if (layout.closed) {
        return out;
    }
    out.space_width = layout.space_width >= 1.0f ? layout.space_width : ScreenLayout::DefaultWidth;
    out.space_height =
        layout.space_height >= 1.0f ? layout.space_height : ScreenLayout::DefaultHeight;
    out.box = FromRect(layout.box);
    out.cells.reserve(static_cast<qsizetype>(layout.glyphs.size()));
    for (const auto& glyph : layout.glyphs) {
        out.cells.append(FromRect(glyph));
    }
    return out;
}

std::optional<QByteArray> ReadFile(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return std::nullopt;
    }
    return file.readAll();
}

const QString BundledProfiles = QStringLiteral(":/reader/profiles/");

// The same files for the emulator's own threads, which may not touch Qt (ReaderUi::PostToGui
// says why): the bundled ones are read once on the main thread, the user's folder is read
// with the standard library.
std::mutex g_plain_mutex;
std::map<std::string, std::string> g_plain_bundled;
std::filesystem::path g_plain_user_directory;

/// A profile or one of its data files by name, the user's own before the bundled one.
std::optional<std::string> PlainSource(const std::string& name) {
    if (name.empty() || name.find("..") != std::string::npos ||
        name.find('/') != std::string::npos || name.find('\\') != std::string::npos) {
        return std::nullopt;
    }
    std::filesystem::path directory;
    {
        std::scoped_lock lock{g_plain_mutex};
        directory = g_plain_user_directory;
    }
    if (!directory.empty()) {
        std::ifstream file{directory / name, std::ios::binary};
        if (file) {
            return std::string{std::istreambuf_iterator<char>{file},
                               std::istreambuf_iterator<char>{}};
        }
    }
    std::scoped_lock lock{g_plain_mutex};
    const auto found = g_plain_bundled.find(name);
    return found == g_plain_bundled.end() ? std::nullopt
                                          : std::optional<std::string>{found->second};
}

/// Asks for the running game's profile and loads it. Never from one of the game's own
/// threads: the main thread before the game runs, or where frames are counted.
void LoadProfileFromFrontend() {
    auto& engine = Reader::Engine::Instance();
    const std::string title = engine.GameId();
    if (title.empty()) {
        return;
    }
    // On the loader's thread or the CPU's: nothing of Qt here.
    const auto source = PlainSource(title + ".lua");
    auto& session = ReaderSession::Instance();
    if (!source) {
        engine.UnloadProfile();
        session.PostProfileStatus(title, false, 0, {});
        return;
    }
    if (engine.LoadProfile(title + ".lua", *source)) {
        session.PostProfileStatus(title, true, static_cast<int>(engine.HookCount()), {});
    } else {
        session.PostProfileStatus(title, false, 0, engine.LastError());
    }
}

} // namespace

// ---- The bridge ----

QString ReaderBridge::UserProfileDirectory() {
    return ReaderSession::DataDirectory() + QStringLiteral("/profiles");
}

std::optional<QByteArray> ReaderBridge::ProfileSource(const QString& title_id) {
    const QString name = title_id + QStringLiteral(".lua");
    if (auto user = ReadFile(UserProfileDirectory() + u'/' + name)) {
        return user;
    }
    return ReadFile(BundledProfiles + name);
}

bool ReaderBridge::HasUserProfile(const QString& title_id) {
    return QFile::exists(UserProfileDirectory() + u'/' + title_id + QStringLiteral(".lua"));
}

bool ReaderBridge::HasBundledProfile(const QString& title_id) {
    return QFile::exists(BundledProfiles + title_id + QStringLiteral(".lua"));
}

void ReaderBridge::PrepareFiles() {
    std::map<std::string, std::string> bundled;
    const QDir directory{BundledProfiles};
    for (const QString& name : directory.entryList(QDir::Files)) {
        if (const auto content = ReadFile(BundledProfiles + name)) {
            bundled[name.toStdString()] = content->toStdString();
        }
    }
    const QByteArray user_utf8 = UserProfileDirectory().toUtf8();
    const std::u8string user{reinterpret_cast<const char8_t*>(user_utf8.constData()),
                             static_cast<std::size_t>(user_utf8.size())};
    std::scoped_lock lock{g_plain_mutex};
    g_plain_bundled = std::move(bundled);
    g_plain_user_directory = std::filesystem::path{user};
}

void ReaderBridge::OnGameLoading() {
    auto& engine = Reader::Engine::Instance();
    // The session exists before the first game is loaded (the reader's controller makes it on
    // the main thread), so the callbacks only ever post to it.
    // All of these are called on the emulator's threads: plain data only, handed to the main
    // thread (ReaderUi::PostToGui), where it becomes the app's own.
    engine.SetEventCallback([](const Reader::TextEvent& event) {
        ReaderUi::PostToGui([event] { ReaderSession::Instance().PostLine(FromEvent(event)); });
    });
    engine.SetLayoutCallback([](const Reader::TextLayout& layout) {
        ReaderUi::PostToGui(
            [layout] { ReaderSession::Instance().PostLayout(FromLayout(layout)); });
    });
    engine.SetLogCallback([](Reader::LogLevel level, const std::string& message) {
        switch (level) {
        case Reader::LogLevel::Info:
            LOG_INFO(Frontend, "[Reader] {}", message);
            break;
        case Reader::LogLevel::Warning:
            LOG_WARNING(Frontend, "[Reader] {}", message);
            break;
        case Reader::LogLevel::Error:
            LOG_ERROR(Frontend, "[Reader] {}", message);
            break;
        }
        ReaderUi::PostToGui([level, message] {
            ReaderSession::Instance().PostLog(static_cast<int>(level),
                                              QString::fromStdString(message));
        });
    });
    engine.SetDataProvider([](const std::string& name) -> std::optional<std::string> {
        return PlainSource(Reader::Engine::Instance().GameId() + "." + name);
    });
    // Asked by the loader: a game whose profile hooks its code runs on the recompiler. On a
    // PC every game does anyway; the answer matters where code can run natively.
    engine.SetHookPredicate([](const std::string& game_id) {
        const auto source = PlainSource(game_id + ".lua");
        return source && Reader::Engine::ProfileWantsHooks(*source);
    });
}

void ReaderBridge::OnGameStarted() {
    LoadProfileFromFrontend();
}

void ReaderBridge::OnGameStopped() {
    // The engine goes on reporting to the app: the next game is loaded by the emulator's own
    // screens, without this layer being told beforehand.
    Reader::InputFilter::Instance().SetHandlers(nullptr, nullptr);
    Reader::InputFilter::Instance().SetPauseButton(-1, nullptr);
    Reader::InputFilter::Instance().HoldSticks(false);
}

void ReaderBridge::ReloadProfile() {
    // Profiles are swapped where frames are counted: between frames.
    Reader::Engine::Instance().Post([] { LoadProfileFromFrontend(); });
}

// ---- The session ----

ReaderSession& ReaderSession::Instance() {
    static ReaderSession* instance = [] {
        auto* session = new ReaderSession;
        // Ends with the application, before Qt itself is taken down.
        session->setParent(QCoreApplication::instance());
        return session;
    }();
    return *instance;
}

ReaderSession::ReaderSession() {
    worker_context = new QObject;
    worker_context->moveToThread(&worker_thread);
    worker_thread.setObjectName(QStringLiteral("ReaderWorker"));
    worker_thread.start();
    heartbeat.setInterval(HeartbeatMs);
    connect(&heartbeat, &QTimer::timeout, this, &ReaderSession::Heartbeat);
}

ReaderSession::~ReaderSession() {
    Stop();
    worker_thread.quit();
    worker_thread.wait();
    delete worker_context;
}

QString ReaderSession::DataDirectory() {
    return ReaderDirectory();
}

void ReaderSession::OpenData() {
    if (settings) {
        return;
    }
    const QString directory = DataDirectory();
    QDir().mkpath(directory);
    QDir().mkpath(ReaderBridge::UserProfileDirectory());
    settings = std::make_unique<ReaderApp::ReaderSettings>(
        directory + QStringLiteral("/reader.ini"));
    ReaderCrtPage::Load(*settings);
    database = std::make_unique<ReaderApp::ReaderDatabase>(
        directory + u'/' + QLatin1String(ReaderApp::ReaderDatabase::FileName));
    dictionaries = std::make_unique<ReaderApp::DictionaryDatabase>(
        directory + u'/' + QLatin1String(ReaderApp::DictionaryDatabase::FileName));
    lookup = std::make_unique<ReaderApp::DictionaryLookup>(*dictionaries);
}

ReaderApp::ReaderSettings& ReaderSession::Settings() {
    OpenData();
    return *settings;
}

ReaderApp::ReaderDatabase& ReaderSession::Database() {
    OpenData();
    return *database;
}

ReaderApp::DictionaryDatabase& ReaderSession::Dictionaries() {
    OpenData();
    return *dictionaries;
}

ReaderApp::DictionaryLookup& ReaderSession::Lookup() {
    OpenData();
    return *lookup;
}

void ReaderSession::OnWorker(std::function<void()> task) {
    QMetaObject::invokeMethod(worker_context, std::move(task), Qt::QueuedConnection);
}

void ReaderSession::AfterWrites(std::function<void()> task) {
    OpenData();
    OnWorker(std::move(task));
}

void ReaderSession::Start(const QString& game_id_, const QString& title_) {
    Stop();
    OpenData();
    const QString key = game_id_.trimmed().toUpper();
    title = title_;
    title_id = key;
    ++generation;
    current.reset();
    current_choices.reset();
    session_activity.reset();
    recent.clear();
    ClearScreenTexts();
    // A profile may have been loaded already: the engine reports that as soon as the game is
    // in memory, which is before the frontend knows what the game is called.
    if (profile_status.title_id != key) {
        profile_status = ProfileStatus{key, false, 0, {}};
    }

    const qint64 idle = settings->IdleTimeoutMs();
    const bool paused = settings->StatsPaused();
    OnWorker([this, key, name = title, idle, paused] {
        try {
            database->SetGameTitle(key, name);
        } catch (const ReaderApp::SqliteError& error) {
            LOG_ERROR(Frontend, "[Reader] Could not store the game's title: {}", error.what());
        }
        tracker = std::make_unique<ReaderApp::ActivityTracker>(
            *database, key, ReaderApp::ActivityTracker::DayOf{}, idle);
        tracker->paused = paused;
        last_line.reset();
        last_presented.reset();
        published_activity.reset();
        last_flush = Now();
        worker_title_id = key;
    });

    media = std::make_unique<MediaCapture>(*settings);
    Reader::AudioTap::Instance().SetEnabled(settings->CaptureAudio(),
                                            static_cast<u32>(settings->AudioHistorySeconds()));
    ApplyTextHookSettings();
    heartbeat.start();
    emit ProfileStatusChanged();
}

void ReaderSession::Stop() {
    if (!IsActive()) {
        return;
    }
    heartbeat.stop();
    OnWorker([this] {
        FlushTracker();
        tracker.reset();
        worker_title_id.clear();
    });
    hook_server.reset();
    media.reset();
    Reader::AudioTap::Instance().SetEnabled(false, 0);
    title_id.clear();
    ++generation;
    reading = false;
    current.reset();
    current_choices.reset();
    profile_status = ProfileStatus{};
    ClearScreenTexts();
    if (session_activity) {
        session_activity.reset();
        emit SessionActivityChanged();
    }
    emit ProfileStatusChanged();
}

void ReaderSession::SetReading(bool value) {
    if (reading == value) {
        return;
    }
    reading = value;
    if (!value) {
        OnWorker([this] { FlushTracker(); });
    }
}

void ReaderSession::OnSettingsChanged() {
    if (!settings) {
        return;
    }
    ReaderCrtPage::Load(*settings);
    if (IsActive()) {
        Reader::AudioTap::Instance().SetEnabled(settings->CaptureAudio(),
                                                static_cast<u32>(settings->AudioHistorySeconds()));
        ApplyTextHookSettings();
        const qint64 idle = settings->IdleTimeoutMs();
        const bool paused = settings->StatsPaused();
        OnWorker([this, idle, paused] {
            if (tracker) {
                tracker->paused = paused;
                tracker->idle_timeout_ms = idle;
            }
        });
    }
    emit ActivityChanged();
}

void ReaderSession::NoteInteraction() {
    const qint64 now = Now();
    OnWorker([this, now] {
        if (tracker) {
            tracker->OnInteraction(now);
        }
    });
}

void ReaderSession::OnActivityCleared() {
    OnWorker([this] {
        if (tracker) {
            tracker->Reset();
            PublishActivity();
        }
    });
    emit ActivityChanged();
}

void ReaderSession::Flush(std::function<void()> then) {
    OnWorker([this, then = std::move(then)] {
        FlushTracker();
        if (then) {
            QMetaObject::invokeMethod(this, then, Qt::QueuedConnection);
        }
    });
}

std::optional<qint64> ReaderSession::AudioEndFor(qint64 backlog_id) const {
    for (qsizetype i = 0; i + 1 < recent.size(); ++i) {
        if (recent.at(i).backlog_id == backlog_id) {
            const qint64 next = recent.at(i + 1).line.audio_position;
            return next >= 0 ? std::optional<qint64>(next) : std::nullopt;
        }
    }
    return std::nullopt;
}

std::optional<PresentedLine> ReaderSession::RecentLine(qint64 backlog_id) const {
    for (const PresentedLine& presented : recent) {
        if (presented.backlog_id == backlog_id) {
            return presented;
        }
    }
    return std::nullopt;
}

QString ReaderSession::TextHookAddress() const {
    return hook_server ? hook_server->DescribeAddress() : QString();
}

// ---- From the core ----

void ReaderSession::PostLine(TextLine line) {
    if (line.text.isEmpty()) {
        return;
    }
    const qint64 now = Now();
    line.timestamp = now;
    // Straight to the worker: lines are counted and stored in the order they arrive.
    OnWorker([this, line = std::move(line), now] { HandleLine(line, now); });
}

void ReaderSession::PostLayout(ScreenLayout layout) {
    QMetaObject::invokeMethod(
        this,
        [this, layout = std::move(layout)] {
            if (IsActive() && screen.ApplyLayout(layout)) {
                emit ScreenTextChanged();
            }
        },
        Qt::QueuedConnection);
}

void ReaderSession::PostLog(int level, QString message) {
    if (level < 2) {
        return;
    }
    QMetaObject::invokeMethod(
        this,
        [this, message = std::move(message)] {
            profile_status.error = message;
            emit ProfileStatusChanged();
        },
        Qt::QueuedConnection);
}

void ReaderSession::PostProfileStatus(std::string game_id, bool loaded, int hooks,
                                      std::string message) {
    // From the emulator's threads: no Qt until the main thread has it.
    ReaderUi::PostToGui([this, game_id = std::move(game_id), loaded, hooks,
                         message = std::move(message)] {
        profile_status = ProfileStatus{QString::fromStdString(game_id), loaded, hooks,
                                       QString::fromStdString(message)};
        emit ProfileStatusChanged();
    });
}

// ---- Worker thread ----

void ReaderSession::FlushTracker() {
    if (!tracker) {
        return;
    }
    try {
        tracker->Flush();
    } catch (const ReaderApp::SqliteError& error) {
        LOG_ERROR(Frontend, "[Reader] Could not save reading activity: {}", error.what());
    }
}

void ReaderSession::HandleLine(const TextLine& line, qint64 now) {
    if (!tracker) {
        return;
    }
    const quint64 for_generation = generation.load();
    if (last_line && last_line->source_id == line.source_id &&
        last_line->channel == line.channel && last_line->text == line.text &&
        now - last_line->timestamp < DuplicateWindowMs) {
        // The game laid the same text out again. Not a new line to read, but layouts from now
        // on are reported for this event.
        if (!line.window.isEmpty() && last_presented) {
            const PresentedLine again{line, last_presented->accounting, last_presented->backlog_id};
            QMetaObject::invokeMethod(
                this,
                [this, again, for_generation] {
                    if (for_generation == generation) {
                        AdoptWindowLine(again);
                    }
                },
                Qt::QueuedConnection);
        }
        return;
    }
    last_line = line;
    PresentedLine presented;
    presented.line = line;
    try {
        presented.accounting = tracker->OnLine(line, now);
        PublishActivity();
        presented.backlog_id = database->InsertLine(worker_title_id, line, presented.accounting);
        if (now - last_flush > FlushIntervalMs) {
            tracker->Flush();
            last_flush = now;
        }
    } catch (const ReaderApp::SqliteError& error) {
        LOG_ERROR(Frontend, "[Reader] Could not store a line: {}", error.what());
    }
    last_presented = presented;
    QMetaObject::invokeMethod(
        this,
        [this, presented, for_generation] {
            if (for_generation == generation) {
                Present(presented);
            }
        },
        Qt::QueuedConnection);
}

void ReaderSession::PublishActivity() {
    const std::optional<ReadingCounters> counters =
        tracker ? tracker->Session() : std::optional<ReadingCounters>{};
    if (counters == published_activity) {
        return;
    }
    published_activity = counters;
    const quint64 for_generation = generation.load();
    QMetaObject::invokeMethod(
        this,
        [this, counters, for_generation] {
            if (for_generation != generation || !IsActive()) {
                return;
            }
            session_activity = counters;
            emit SessionActivityChanged();
        },
        Qt::QueuedConnection);
}

// ---- Main thread ----

void ReaderSession::Present(const PresentedLine& presented) {
    if (!IsActive()) {
        return;
    }
    const TextLine& line = presented.line;
    if (line.IsChoice()) {
        current_choices = presented;
    } else {
        // The frame on screen still shows the previous line: keep it for that line.
        if (current && media) {
            media->CaptureForLine(current->backlog_id, true);
        }
        current = presented;
        current_choices.reset();
        recent.append(presented);
        while (recent.size() > RecentLimit) {
            recent.removeFirst();
        }
        if (media) {
            media->CaptureForLine(presented.backlog_id, false);
        }
    }
    if (hook_server) {
        hook_server->Broadcast(line);
    }
    if (settings->ClipboardAuto() && !line.IsChoice()) {
        QGuiApplication::clipboard()->setText(line.text);
    }
    emit LinePresented(presented);
    if (!line.window.isEmpty()) {
        AdoptWindowLine(presented);
    }
}

void ReaderSession::AdoptWindowLine(const PresentedLine& presented) {
    if (IsActive() && screen.AdoptLine(presented)) {
        emit ScreenTextChanged();
    }
}

void ReaderSession::ClearScreenTexts() {
    if (screen.Clear()) {
        emit ScreenTextChanged();
    }
}

void ReaderSession::ApplyTextHookSettings() {
    const bool wanted = settings->TextHookEnabled();
    if (hook_server && (!wanted || hook_server->Port() != settings->TextHookPort() ||
                        hook_server->Lan() != settings->TextHookLan())) {
        hook_server.reset();
    }
    if (wanted && !hook_server) {
        hook_server = std::make_unique<ReaderApp::TextHookServer>(settings->TextHookPort(),
                                                                  settings->TextHookLan());
        if (!hook_server->Start()) {
            LOG_WARNING(Frontend, "[Reader] The text server cannot listen on port {}",
                        settings->TextHookPort());
        }
    }
    if (hook_server) {
        hook_server->format = settings->TextHookFormat();
        hook_server->include_speaker = settings->TextHookSpeaker();
        hook_server->game_id = title_id;
    }
}

void ReaderSession::Heartbeat() {
    const qint64 now = Now();
    const bool is_reading = reading;
    if (current && media) {
        media->KeepAudioOf(current->backlog_id, current->line.audio_position);
    }
    OnWorker([this, now, is_reading] {
        if (!tracker) {
            return;
        }
        try {
            tracker->OnHeartbeat(now, is_reading);
            PublishActivity();
            if (now - last_flush > FlushIntervalMs) {
                tracker->Flush();
                last_flush = now;
                QMetaObject::invokeMethod(this, [this] { emit ActivityChanged(); },
                                          Qt::QueuedConnection);
            }
        } catch (const ReaderApp::SqliteError& error) {
            LOG_ERROR(Frontend, "[Reader] Could not save reading activity: {}", error.what());
        }
    });
}

} // namespace ReaderUi
