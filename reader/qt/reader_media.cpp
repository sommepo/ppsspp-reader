// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <exception>
#include <utility>
#include <QBuffer>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QMutexLocker>
#include <QSaveFile>
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include "reader/qt/host.h"
#include "reader/core/audio_tap.h"
#include "reader/desktop/card_builder.h"
#include "reader/desktop/word_audio.h"
#include "reader/desktop/media.h"
#include "reader/qt/reader_games.h"
#include "reader/qt/reader_media.h"
#include "reader/qt/reader_session.h"

namespace ReaderUi {

using ReaderApp::AnkiConnectClient;
using ReaderApp::CardContext;
using ReaderApp::Duplicates;
using ReaderApp::LookupResult;
using ReaderApp::PresentedLine;

namespace {

constexpr int LineSettleMs = 1500;
constexpr int JpegQuality = 88;
constexpr int Channels = 2;
constexpr int SilenceThreshold = 180;

qint64 Now() {
    return QDateTime::currentMSecsSinceEpoch();
}

/// The picture without the black bars around it, as a JPEG (a PNG where this Qt cannot write
/// JPEG). Empty when it cannot be written at all.
QByteArray EncodePicture(const QImage& image) {
    const QImage trimmed = ReaderApp::Letterbox::Trim(image);
    QByteArray bytes;
    {
        QBuffer buffer(&bytes);
        buffer.open(QIODevice::WriteOnly);
        if (trimmed.save(&buffer, "JPG", JpegQuality) && !bytes.isEmpty()) {
            return bytes;
        }
    }
    bytes.clear();
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    if (!trimmed.save(&buffer, "PNG")) {
        LOG_ERROR(Frontend, "[Reader] A screenshot could not be encoded");
        return {};
    }
    return bytes;
}

QString PictureExtension(const QByteArray& bytes) {
    return bytes.startsWith("\x89PNG") ? QStringLiteral("png") : QStringLiteral("jpg");
}

} // namespace

// ---- MediaCapture ----

MediaCapture::MediaCapture(ReaderApp::ReaderSettings& settings_) : settings{settings_} {
    directory =
        CacheDirectory() + QStringLiteral("/media");
    QDir().mkpath(directory);
    background.setMaxThreadCount(1);
    // Screenshots of an earlier run belong to lines that can no longer be matched reliably.
    background.start([dir = directory] {
        for (const QString& name : QDir(dir).entryList(QDir::Files)) {
            QFile::remove(dir + u'/' + name);
        }
    });
}

MediaCapture::~MediaCapture() {
    for (QTimer* timer : std::as_const(pending)) {
        timer->stop();
        delete timer;
    }
    pending.clear();
    background.waitForDone();
}

QString MediaCapture::FileFor(qint64 backlog_id) const {
    return directory + QStringLiteral("/line-%1.img").arg(backlog_id);
}

void MediaCapture::CaptureForLine(qint64 backlog_id, bool immediately) {
    if (backlog_id < 0 || settings.ScreenshotCacheLines() == 0) {
        return;
    }
    if (captured.contains(backlog_id)) {
        return;
    }
    if (QTimer* waiting = pending.take(backlog_id)) {
        waiting->stop();
        waiting->deleteLater();
    }
    if (immediately) {
        CaptureInto(backlog_id);
        return;
    }
    auto* timer = new QTimer(this);
    timer->setSingleShot(true);
    pending.insert(backlog_id, timer);
    connect(timer, &QTimer::timeout, this, [this, backlog_id, timer] {
        if (pending.value(backlog_id) == timer) {
            pending.remove(backlog_id);
        }
        timer->deleteLater();
        CaptureInto(backlog_id);
    });
    timer->start(LineSettleMs);
}

void MediaCapture::CaptureInto(qint64 backlog_id) {
    if (!frame_source) {
        return;
    }
    frame_source(settings.ScreenshotMaxWidth(), [this, backlog_id](const QImage& image) {
        if (image.isNull() || captured.contains(backlog_id)) {
            return;
        }
        captured.append(backlog_id);
        QStringList evicted;
        while (captured.size() > settings.ScreenshotCacheLines()) {
            evicted.append(FileFor(captured.takeFirst()));
        }
        background.start([image, file = FileFor(backlog_id), evicted] {
            const QByteArray bytes = EncodePicture(image);
            if (!bytes.isEmpty()) {
                // Written under another name first: a card may ask for it at any moment.
                QSaveFile out(file);
                if (out.open(QIODevice::WriteOnly)) {
                    out.write(bytes);
                    out.commit();
                }
            }
            for (const QString& old : evicted) {
                QFile::remove(old);
            }
        });
    });
}

QByteArray MediaCapture::ScreenshotFor(qint64 backlog_id) const {
    if (!captured.contains(backlog_id)) {
        return {};
    }
    QFile file(FileFor(backlog_id));
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return file.readAll();
}

void MediaCapture::CaptureNow(std::function<void(const QByteArray&)> callback) {
    if (!frame_source) {
        callback({});
        return;
    }
    frame_source(settings.ScreenshotMaxWidth(),
                 [this, callback = std::move(callback)](const QImage& image) {
                     if (image.isNull()) {
                         callback({});
                         return;
                     }
                     background.start([this, image, callback] {
                         const QByteArray bytes = EncodePicture(image);
                         QMetaObject::invokeMethod(this, [callback, bytes] { callback(bytes); },
                                                   Qt::QueuedConnection);
                     });
                 });
}

qint64 MediaCapture::ClipStart(qint64 start) const {
    const qint64 lead =
        static_cast<qint64>(settings.AudioLeadMs()) * Reader::AudioTap::SampleRate / 1000;
    return std::max<qint64>(start - lead, 0);
}

void MediaCapture::KeepAudioOf(qint64 backlog_id, qint64 start) {
    if (!settings.CaptureAudio() || start < 0 || backlog_id < 0 || holding == backlog_id) {
        return;
    }
    const qint64 rate = Reader::AudioTap::SampleRate;
    const qint64 from = ClipStart(start);
    const qint64 to = from + settings.AudioClipMaxSeconds() * rate;
    if (static_cast<qint64>(Reader::AudioTap::Instance().Position()) < to) {
        return;
    }
    holding = backlog_id;
    background.start([this, backlog_id, from, to] {
        u64 actual = 0;
        std::vector<s16> samples = Reader::AudioTap::Instance().Read(
            static_cast<u64>(from), static_cast<u64>(to), actual);
        if (!samples.empty() && static_cast<qint64>(actual) == from) {
            QMutexLocker lock(&held_mutex);
            held = Held{backlog_id, from, std::move(samples)};
        }
    });
}

void MediaCapture::AudioClip(qint64 backlog_id, qint64 start, std::optional<qint64> end,
                             std::function<void(const Clip&)> callback) {
    if (!settings.CaptureAudio() || start < 0) {
        callback({});
        return;
    }
    const qint64 rate = Reader::AudioTap::SampleRate;
    const qint64 from = ClipStart(start);
    const qint64 limit = from + settings.AudioClipMaxSeconds() * rate;
    const qint64 to = std::min(
        end.value_or(static_cast<qint64>(Reader::AudioTap::Instance().Position())), limit);
    background.start([this, backlog_id, from, to, rate, callback = std::move(callback)] {
        Clip clip;
        u64 actual = 0;
        std::vector<s16> samples;
        if (to > from) {
            samples = Reader::AudioTap::Instance().Read(static_cast<u64>(from),
                                                        static_cast<u64>(to), actual);
        }
        if (samples.empty() || static_cast<qint64>(actual) > from) {
            // The history has moved past the start of the line: use what was kept.
            QMutexLocker lock(&held_mutex);
            if (held.backlog_id == backlog_id && held.from == from) {
                const qint64 wanted = std::clamp<qint64>(
                    (to - from) * Channels, 0, static_cast<qint64>(held.samples.size()));
                samples.assign(held.samples.begin(), held.samples.begin() + wanted);
            } else {
                samples.clear();
            }
        }
        const std::size_t shortest = static_cast<std::size_t>(rate / 5 * Channels);
        if (samples.size() >= shortest) {
            std::vector<s16> trimmed = ReaderApp::PcmTools::TrimTrailingSilence(
                samples, Channels, SilenceThreshold, static_cast<int>(rate / 4));
            if (trimmed.size() >= shortest) {
                ReaderApp::PcmTools::FadeEdges(trimmed, Channels, static_cast<int>(rate / 100));
                clip.ogg = ReaderApp::EncodeOggOpusAt(trimmed, Channels, static_cast<int>(rate));
                LOG_INFO(Frontend, "[Reader] Audio clip: {:.1f} s, {} bytes",
                         static_cast<double>(trimmed.size()) / Channels / static_cast<double>(rate),
                         clip.ogg.size());
                if (!clip.ogg.isEmpty()) {
                    clip.samples =
                        std::make_shared<const std::vector<qint16>>(std::move(trimmed));
                }
            }
        }
        QMetaObject::invokeMethod(this, [callback, clip] { callback(clip); },
                                  Qt::QueuedConnection);
    });
}

// ---- Miner ----

namespace {

constexpr qint64 KnownForMs = 60000;
constexpr int KnownLimit = 500;
constexpr qint64 QuietAfterFailureMs = 20000;
constexpr int QuickConnectMs = 1500;
constexpr int QuickReadMs = 5000;

/// The Anki setup as it was when a card was asked for: the settings are the main thread's.
struct Setup {
    QString problem; ///< Why no card can be made, or empty
    QString url;
    QString key;
    QString deck;
    QString model;
    QMap<QString, QString> templates;
    QStringList tags;
    Duplicates duplicates{Duplicates::RefuseInDeck};
    /// The deck existing notes are looked for in, or empty for the whole collection.
    QString scope;
    /// The note type's first field, when the setup screen recorded it.
    QString key_field;
    QString game_title;
    /// The game's page, for {url} and for a card that goes through VN Library's media add-on.
    QString game_url;
    /// The field that add-on asked an address for at this Anki address, when it did.
    QString address_field;

    [[nodiscard]] bool Complete() const {
        return problem.isEmpty() && !deck.isEmpty() && !model.isEmpty() && !templates.isEmpty();
    }
};

Setup Snapshot() {
    auto& session = ReaderSession::Instance();
    const ReaderApp::ReaderSettings& settings = session.Settings();
    Setup setup;
    if (settings.AnkiTransport() != QStringLiteral("ankiconnect")) {
        setup.problem = Miner::tr("Cards are switched off under Reader → Anki setup.");
    } else if (settings.AnkiConnectUrl().trimmed().isEmpty()) {
        setup.problem = Miner::tr("Anki is not set up yet: see Reader → Anki setup.");
    }
    setup.url = settings.AnkiConnectUrl().trimmed();
    setup.key = settings.AnkiConnectKey();
    setup.deck = settings.AnkiDeck();
    setup.model = settings.AnkiModel();
    const QMap<QString, QString> mapping =
        ReaderApp::CardBuilder::ParseMapping(settings.AnkiFieldMap());
    for (auto it = mapping.constBegin(); it != mapping.constEnd(); ++it) {
        if (!it.value().trimmed().isEmpty()) {
            setup.templates.insert(it.key(), it.value());
        }
    }
    setup.tags = settings.AnkiTags().split(u' ', Qt::SkipEmptyParts);
    setup.duplicates = settings.AnkiAllowDuplicates()
                           ? Duplicates::Allow
                           : settings.AnkiDuplicatesAnywhere() ? Duplicates::RefuseAnywhere
                                                               : Duplicates::RefuseInDeck;
    setup.scope = settings.AnkiDuplicatesAnywhere() ? QString() : setup.deck;
    setup.key_field = settings.AnkiKeyField(setup.model);
    setup.game_title = session.GameTitle();
    setup.game_url = ReaderApp::SourceAddress::Of(session.GameKey());
    setup.address_field = settings.AnkiAddressField(setup.url);
    return setup;
}

QMutex cache_mutex;
/// What Anki said it has, for a short while: the same word is often looked up twice.
QHash<QString, std::pair<qint64, QList<qint64>>> known;
/// First fields of note types, as Anki named them.
QHash<QString, QString> key_fields;
std::atomic<qint64> quiet_until{0};

struct NoteKey {
    QString field;
    QString value;
};

QString CacheKey(const Setup& setup, const NoteKey& key) {
    return QStringList{setup.url, setup.scope.isEmpty() ? QString(QChar(u'\0')) : setup.scope,
                       key.field, key.value}
        .join(QChar(0x1F));
}

std::optional<QList<qint64>> Recalled(const Setup& setup, const NoteKey& key) {
    QMutexLocker lock(&cache_mutex);
    const auto found = known.constFind(CacheKey(setup, key));
    if (found == known.constEnd() || Now() - found->first >= KnownForMs) {
        return std::nullopt;
    }
    return found->second;
}

void Remember(const Setup& setup, const NoteKey& key, const QList<qint64>& notes) {
    QMutexLocker lock(&cache_mutex);
    if (known.size() > KnownLimit) {
        known.clear();
    }
    known.insert(CacheKey(setup, key), {Now(), notes});
}

/**
 * The field Anki tells notes apart by (the note type's first) and what a card made from
 * `card` would put in it, or nothing when the mapping leaves that field empty.
 */
std::optional<NoteKey> KeyOf(AnkiConnectClient& client, const Setup& setup, const CardContext& card) {
    QString field = setup.key_field;
    if (field.isEmpty()) {
        QMutexLocker lock(&cache_mutex);
        field = key_fields.value(setup.model);
    }
    if (field.isEmpty()) {
        // A setup saved before the field was recorded: Anki is asked once.
        const QStringList fields = client.FieldNames(setup.model);
        if (fields.isEmpty()) {
            return std::nullopt;
        }
        field = fields.first();
        QMutexLocker lock(&cache_mutex);
        key_fields.insert(setup.model, field);
    }
    const auto found = setup.templates.constFind(field);
    if (found == setup.templates.constEnd()) {
        return std::nullopt;
    }
    const QString value =
        ReaderApp::CardBuilder::Build({{field, *found}}, card).value(field);
    if (value.trimmed().isEmpty()) {
        return std::nullopt;
    }
    return NoteKey{field, value};
}

CardContext ContextOf(const PresentedLine& presented, const Setup& setup,
                      const std::optional<LookupResult>& word, int first, int last) {
    CardContext card;
    card.line = presented.line;
    card.game_title = setup.game_title;
    card.game_url = setup.game_url;
    card.word = word;
    card.word_first = first;
    card.word_last = last;
    return card;
}

Miner::Outcome Failed(const QString& message) {
    return Miner::Outcome{Miner::Outcome::Kind::Failed, {}, message};
}

/// On a thread of the miner's: looks for the word, stores the media, adds the note.
Miner::Outcome Send(const Setup& setup, CardContext card, const QByteArray& picture,
                    const QByteArray& clip) {
    try {
        AnkiConnectClient client(setup.url, setup.key);
        // Not every Anki refuses a duplicate when asked to, so look before sending anything.
        std::optional<NoteKey> key;
        if (setup.duplicates != Duplicates::Allow) {
            key = KeyOf(client, setup, card);
        }
        if (key) {
            const QList<qint64> existing = client.FindNotes(setup.scope, key->field, key->value);
            if (!existing.isEmpty()) {
                Remember(setup, *key, existing);
                return Miner::Outcome{Miner::Outcome::Kind::Exists, existing, {}};
            }
        }
        const QString stamp = QString::number(Now());
        if (!picture.isEmpty()) {
            card.screenshot_name = client.StoreMedia(
                picture, QStringLiteral("ppsspp_reader_%1.%2").arg(stamp, PictureExtension(picture)));
        }
        if (!clip.isEmpty()) {
            card.audio_name =
                client.StoreMedia(clip, QStringLiteral("ppsspp_reader_%1.ogg").arg(stamp));
        }
        // The word as somebody says it, when a field asks for it and the card has a word:
        // fetched now, and left out when it cannot be had.
        if (card.word && ReaderApp::CardBuilder::NeedsWordAudio(setup.templates.values())) {
            const QByteArray spoken =
                ReaderApp::WordAudio::Fetch(card.word->expression, card.word->reading);
            if (!spoken.isEmpty()) {
                card.word_audio_name = client.StoreMedia(
                    spoken,
                    ReaderApp::WordAudio::FileName(card.word->expression, card.word->reading));
            }
        }
        const QMap<QString, QString> fields = ReaderApp::CardBuilder::Build(setup.templates, card);
        const ReaderApp::SourceAddress::Added added = ReaderApp::SourceAddress::AddNote(
            [&](const QMap<QString, QString>& with) {
                return client.AddNote(setup.deck, setup.model, with, setup.tags, setup.duplicates);
            },
            fields, setup.game_url, setup.game_title, setup.address_field);
        const qint64 id = added.id;
        if (!key) {
            try {
                key = KeyOf(client, setup, card);
            } catch (const ReaderApp::AnkiError&) {
                // The note is in; it just will not be remembered.
            }
        }
        if (key) {
            Remember(setup, *key, {id});
        }
        Miner::Outcome outcome{Miner::Outcome::Kind::Added, {id}, {}};
        if (added.field != setup.address_field) {
            outcome.address_field = added.field;
        }
        return outcome;
    } catch (const ReaderApp::AnkiError& error) {
        return Failed(error.Message());
    } catch (const std::exception& error) {
        LOG_ERROR(Frontend, "[Reader] Card failed: {}", error.what());
        return Failed(QString::fromUtf8(error.what()));
    }
}

} // namespace

Miner::Miner(QObject* parent) : QObject(parent) {
    worker.setMaxThreadCount(1);
    checker.setMaxThreadCount(1);
}

Miner::~Miner() {
    worker.waitForDone();
    checker.waitForDone();
}

void Miner::Forget() {
    QMutexLocker lock(&cache_mutex);
    known.clear();
    key_fields.clear();
    quiet_until = 0;
}

void Miner::Mine(const PresentedLine& presented, const std::optional<LookupResult>& word,
                 int first, int last, AudioQuestion question,
                 std::function<void(const Outcome&)> callback) {
    const Setup setup = Snapshot();
    if (!setup.problem.isEmpty()) {
        callback(Failed(setup.problem));
        return;
    }
    if (!setup.Complete()) {
        callback(Failed(tr("Anki is not set up yet: choose a deck and a note type under "
                           "Reader → Anki setup.")));
        return;
    }
    auto& session = ReaderSession::Instance();
    MediaCapture* media = session.Media();
    const qint64 id = presented.backlog_id;
    const bool is_current = session.Current() && session.Current()->backlog_id == id;
    const QStringList templates = setup.templates.values();
    using CardAudio = ReaderApp::ReaderSettings::CardAudio;
    const CardAudio audio = session.Settings().CardAudioFor(
        session.GameKey(),
        ReaderApp::ReaderSettings::CardAudioOfValue(Games::AudioOf(session.GameKey())));
    const bool wants_picture = ReaderApp::CardBuilder::NeedsScreenshot(templates) && media;
    const bool wants_audio = audio != CardAudio::Never &&
                             ReaderApp::CardBuilder::NeedsAudio(templates) && media &&
                             presented.line.audio_position >= 0;
    const bool asks = audio == CardAudio::Ask && question;
    const CardContext card = ContextOf(presented, setup, word, first, last);

    // Gather the media (each step may finish later), then hand over to the worker.
    const auto send = [this, setup, card, callback](const QByteArray& picture,
                                                    const QByteArray& clip) {
        worker.start([this, setup, card, picture, clip, callback] {
            const Outcome outcome = Send(setup, card, picture, clip);
            QMetaObject::invokeMethod(
                this,
                [callback, outcome, address = setup.url] {
                    if (!outcome.address_field.isEmpty()) {
                        auto& settings = ReaderSession::Instance().Settings();
                        settings.SetValue(ReaderApp::ReaderSettings::KeyAnkiAddressField,
                                          ReaderApp::ReaderSettings::AddressFieldValue(
                                              address, outcome.address_field));
                        settings.Sync();
                    }
                    callback(outcome);
                },
                Qt::QueuedConnection);
        });
    };
    const auto with_audio = [send, wants_audio, asks, question, callback, id,
                             position = presented.line.audio_position](const QByteArray& picture) {
        auto& current_session = ReaderSession::Instance();
        MediaCapture* capture = current_session.Media();
        if (!wants_audio || !capture) {
            send(picture, {});
            return;
        }
        capture->AudioClip(
            id, position, current_session.AudioEndFor(id),
            [send, picture, asks, question, callback](const MediaCapture::Clip& clip) {
                if (clip.IsEmpty() || !asks) {
                    send(picture, clip.ogg);
                    return;
                }
                // The card is not sent before the answer. The picture is the one of the
                // moment the card was asked for.
                question(clip, [send, picture, clip, callback](std::optional<bool> keep) {
                    if (!keep) {
                        callback(Outcome{Outcome::Kind::Cancelled, {}, {}});
                    } else {
                        send(picture, *keep ? clip.ogg : QByteArray());
                    }
                });
            });
    };
    if (!wants_picture) {
        with_audio({});
    } else if (is_current) {
        // The scene on screen belongs to the current line; older lines use what was kept.
        media->CaptureNow([with_audio, id](const QByteArray& jpeg) {
            MediaCapture* capture = ReaderSession::Instance().Media();
            with_audio(!jpeg.isEmpty() || !capture ? jpeg : capture->ScreenshotFor(id));
        });
    } else {
        with_audio(media->ScreenshotFor(id));
    }
}

void Miner::FindExisting(const PresentedLine& presented, const LookupResult& word, int first,
                         int last,
                         std::function<void(const std::optional<QList<qint64>>&)> callback) {
    const Setup setup = Snapshot();
    if (!setup.Complete()) {
        callback(std::nullopt);
        return;
    }
    const CardContext card = ContextOf(presented, setup, word, first, last);
    checker.start([this, setup, card, callback] {
        std::optional<QList<qint64>> found;
        try {
            if (Now() >= quiet_until.load()) {
                AnkiConnectClient client(setup.url, setup.key, QuickConnectMs, QuickReadMs);
                if (const auto key = KeyOf(client, setup, card)) {
                    found = Recalled(setup, *key);
                    if (!found) {
                        found = client.FindNotes(setup.scope, key->field, key->value);
                        Remember(setup, *key, *found);
                    }
                }
            }
        } catch (const std::exception& error) {
            // An Anki that is not there is asked again later, not for every word looked up.
            quiet_until = Now() + QuietAfterFailureMs;
            LOG_WARNING(Frontend, "[Reader] Could not ask Anki about a word: {}", error.what());
            found.reset();
        }
        QMetaObject::invokeMethod(this, [callback, found] { callback(found); },
                                  Qt::QueuedConnection);
    });
}

// ---- ClipPlayer ----

ClipPlayer::ClipPlayer(QObject* parent) : QObject(parent) {
    poll.setInterval(100);
    connect(&poll, &QTimer::timeout, this, &ClipPlayer::Poll);
}

ClipPlayer::~ClipPlayer() {
    Stop();
}

bool ClipPlayer::Play(std::shared_ptr<const std::vector<qint16>> samples) {
    Stop();
    if (!samples || samples->empty()) {
        return false;
    }
    if (SDL_WasInit(SDL_INIT_AUDIO) == 0 && SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        LOG_WARNING(Frontend, "[Reader] No sound to listen with: {}", SDL_GetError());
        return false;
    }
    SDL_AudioSpec wanted{};
    wanted.freq = static_cast<int>(Reader::AudioTap::SampleRate);
    wanted.format = AUDIO_S16SYS;
    wanted.channels = static_cast<Uint8>(Channels);
    wanted.samples = 2048;
    SDL_AudioSpec got{};
    device = SDL_OpenAudioDevice(nullptr, 0, &wanted, &got, 0);
    if (device == 0) {
        LOG_WARNING(Frontend, "[Reader] No sound device to listen with: {}", SDL_GetError());
        return false;
    }
    playing = std::move(samples);
    SDL_QueueAudio(device, playing->data(),
                   static_cast<Uint32>(playing->size() * sizeof(qint16)));
    SDL_PauseAudioDevice(device, 0);
    poll.start();
    emit PlayingChanged(true);
    return true;
}

void ClipPlayer::Stop() {
    if (device == 0) {
        return;
    }
    poll.stop();
    SDL_CloseAudioDevice(device);
    device = 0;
    playing.reset();
    emit PlayingChanged(false);
}

void ClipPlayer::Poll() {
    if (device != 0 && SDL_GetQueuedAudioSize(device) == 0) {
        Stop();
    }
}

void Miner::ShowNotes(const QList<qint64>& notes, std::function<void(const QString&)> callback) {
    const Setup setup = Snapshot();
    if (!setup.problem.isEmpty()) {
        callback(setup.problem);
        return;
    }
    worker.start([this, setup, notes, callback] {
        QString failure;
        try {
            AnkiConnectClient client(setup.url, setup.key);
            client.ShowNotes(notes);
        } catch (const ReaderApp::AnkiError& error) {
            failure = error.Message();
        } catch (const std::exception& error) {
            failure = QString::fromUtf8(error.what());
        }
        QMetaObject::invokeMethod(this, [callback, failure] { callback(failure); },
                                  Qt::QueuedConnection);
    });
}

} // namespace ReaderUi
