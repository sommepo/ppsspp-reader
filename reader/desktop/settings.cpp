// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <QRegularExpression>
#include <QRandomGenerator>
#include <QSettings>
#include <QSysInfo>
#include "reader/desktop/card_builder.h"
#include "reader/desktop/settings.h"

namespace ReaderApp {

const QString ReaderSettings::KeyScreenText = QStringLiteral("reader_screen_text");
const QString ReaderSettings::KeyScreenTextOutline = QStringLiteral("reader_screen_text_outline");
const QString ReaderSettings::KeyPopupScale = QStringLiteral("reader_popup_scale");
const QString ReaderSettings::KeyPopupHeight = QStringLiteral("reader_popup_height");
const QString ReaderSettings::KeyPopupAutoHide = QStringLiteral("reader_popup_autohide");
const QString ReaderSettings::KeyPopupHideOnExit = QStringLiteral("reader_popup_hide_on_exit");
const QString ReaderSettings::KeyHoverLookup = QStringLiteral("reader_hover_lookup");
const QString ReaderSettings::KeyCursorButton = QStringLiteral("reader_cursor_button");
const QString ReaderSettings::KeyStatsButton = QStringLiteral("reader_stats_button");
const QString ReaderSettings::KeyBackButton = QStringLiteral("reader_back_button_");
const QString ReaderSettings::KeyStatsInstall = QStringLiteral("reader_stats_install");
const QString ReaderSettings::KeyCursorToggle = QStringLiteral("reader_cursor_toggle");
const QString ReaderSettings::KeyCursorSpeed = QStringLiteral("reader_cursor_speed");
const QString ReaderSettings::KeyCursorFrame = QStringLiteral("reader_cursor_frame");
const QString ReaderSettings::KeyCursorHome = QStringLiteral("reader_cursor_home");
const QString ReaderSettings::KeyStatsPaused = QStringLiteral("reader_stats_paused");
const QString ReaderSettings::KeyIdleMinutes = QStringLiteral("reader_idle_minutes");
const QString ReaderSettings::KeySessionHud = QStringLiteral("reader_session_hud");
const QString ReaderSettings::KeyDiscordEnabled = QStringLiteral("reader_discord_enabled");
const QString ReaderSettings::KeyDiscordApplicationId = QStringLiteral("reader_discord_app_id");
const QString ReaderSettings::KeyHookEnabled = QStringLiteral("reader_texthook_enabled");
const QString ReaderSettings::KeyHookPort = QStringLiteral("reader_texthook_port");
const QString ReaderSettings::KeyHookLan = QStringLiteral("reader_texthook_lan");
const QString ReaderSettings::KeyHookFormat = QStringLiteral("reader_texthook_format");
const QString ReaderSettings::KeyHookSpeaker = QStringLiteral("reader_texthook_speaker");
const QString ReaderSettings::KeyClipboardAuto = QStringLiteral("reader_clipboard_auto");
const QString ReaderSettings::KeyCaptureAudio = QStringLiteral("reader_capture_audio");
const QString ReaderSettings::KeyAudioMax = QStringLiteral("reader_audio_max_seconds");
const QString ReaderSettings::KeyAudioLead = QStringLiteral("reader_audio_lead_ms");
const QString ReaderSettings::KeyScreenshotCache = QStringLiteral("reader_screenshot_cache");
const QString ReaderSettings::KeyScreenshotWidth = QStringLiteral("reader_screenshot_width");
const QString ReaderSettings::KeyCardAudio = QStringLiteral("reader_card_audio");
const QString ReaderSettings::KeyCardAudioGame = QStringLiteral("reader_card_audio_game_");
const QString ReaderSettings::KeyAnkiTransport = QStringLiteral("reader_anki_transport");
const QString ReaderSettings::KeyAnkiDeck = QStringLiteral("reader_anki_deck");
const QString ReaderSettings::KeyAnkiModel = QStringLiteral("reader_anki_model");
const QString ReaderSettings::KeyAnkiFields = QStringLiteral("reader_anki_fields");
const QString ReaderSettings::KeyAnkiTags = QStringLiteral("reader_anki_tags");
const QString ReaderSettings::KeyAnkiConnectUrl = QStringLiteral("reader_ankiconnect_url");
const QString ReaderSettings::KeyAnkiConnectKey = QStringLiteral("reader_ankiconnect_key");
const QString ReaderSettings::KeyAnkiDuplicates = QStringLiteral("reader_anki_duplicates");
const QString ReaderSettings::KeyAnkiDuplicatesAnywhere =
    QStringLiteral("reader_anki_duplicates_anywhere");
const QString ReaderSettings::KeyAnkiKeyField = QStringLiteral("reader_anki_key_field");
const QString ReaderSettings::KeyAnkiAddressField = QStringLiteral("reader_anki_address_field");
const QString ReaderSettings::KeySetupDictionariesAsked =
    QStringLiteral("reader_setup_dictionaries_asked");
const QString ReaderSettings::KeySetupAnkiAsked = QStringLiteral("reader_setup_anki_asked");
const QString ReaderSettings::KeySaveSyncMode = QStringLiteral("reader_savesync_mode");
const QString ReaderSettings::KeySaveSyncPort = QStringLiteral("reader_savesync_port");
const QString ReaderSettings::KeySaveSyncAsked = QStringLiteral("reader_savesync_asked");
const QString ReaderSettings::KeySaveSyncEnabled = QStringLiteral("reader_savesync_enabled");
const QString ReaderSettings::KeySaveSyncServer = QStringLiteral("reader_savesync_server");
const QString ReaderSettings::KeySaveSyncCode = QStringLiteral("reader_savesync_code");
const QString ReaderSettings::KeySaveSyncDevice = QStringLiteral("reader_savesync_device");

namespace {
// A line break: fine in a settings file, and in no note type's name.
constexpr QChar Separator = u'\n';
} // namespace

ReaderSettings::ReaderSettings(const QString& file_path)
    : settings{std::make_unique<QSettings>(file_path, QSettings::IniFormat)} {}

ReaderSettings::~ReaderSettings() = default;

QVariant ReaderSettings::Value(const QString& key, const QVariant& fallback) const {
    return settings->value(key, fallback);
}

void ReaderSettings::SetValue(const QString& key, const QVariant& value) {
    settings->setValue(key, value);
}

void ReaderSettings::Sync() {
    settings->sync();
}

bool ReaderSettings::Bool(const QString& key, bool fallback) const {
    return settings->value(key, fallback).toBool();
}

int ReaderSettings::Int(const QString& key, int fallback, int low, int high) const {
    bool ok = false;
    const int value = settings->value(key, fallback).toInt(&ok);
    return std::clamp(ok ? value : fallback, low, high);
}

QString ReaderSettings::String(const QString& key, const QString& fallback) const {
    const QVariant value = settings->value(key, fallback);
    // A value with commas that was typed into the file without quotes is read as a list.
    if (value.typeId() == QMetaType::QStringList) {
        return value.toStringList().join(QStringLiteral(", "));
    }
    return value.toString();
}

bool ReaderSettings::ScreenText() const {
    return Bool(KeyScreenText, true);
}
bool ReaderSettings::ScreenTextOutline() const {
    return Bool(KeyScreenTextOutline, false);
}
int ReaderSettings::PopupScale() const {
    return Int(KeyPopupScale, 100, 60, 200);
}
int ReaderSettings::PopupHeightPercent() const {
    return Int(KeyPopupHeight, 27, 10, 80);
}
int ReaderSettings::PopupAutoHideSeconds() const {
    return Int(KeyPopupAutoHide, 15, 0, 120);
}
bool ReaderSettings::PopupHideOnExit() const {
    return Bool(KeyPopupHideOnExit, false);
}
QString ReaderSettings::HoverLookup() const {
    const QString value = String(KeyHoverLookup, QStringLiteral("shift"));
    if (value == QStringLiteral("off") || value == QStringLiteral("always")) {
        return value;
    }
    return QStringLiteral("shift");
}

int ReaderSettings::CursorButton() const {
    return Int(KeyCursorButton, 104, -1, 512);
}
QString ReaderSettings::DefaultBackButtonAction(int button) {
    switch (button) {
    case 1:
        return QStringLiteral("reader_pause_game");
    case 2:
        return QStringLiteral("reader_pause_stats");
    case 3:
        return QStringLiteral("reader_sentence_card");
    case 4:
        return QStringLiteral("reader_backlog");
    case 5:
        return QStringLiteral("reader_menu");
    default:
        return {};
    }
}
QString ReaderSettings::BackButtonAction(int button) const {
    const QString said = String(KeyBackButton + QString::number(button), DefaultBackButtonAction(button));
    return said == QStringLiteral("none") ? QString() : said;
}
int ReaderSettings::StatsButton() const {
    return Int(KeyStatsButton, -1, -1, 512);
}
bool ReaderSettings::CursorToggle() const {
    return Bool(KeyCursorToggle, false);
}
int ReaderSettings::CursorSpeed() const {
    return Int(KeyCursorSpeed, 100, 25, 300);
}
bool ReaderSettings::CursorFrame() const {
    return Bool(KeyCursorFrame, true);
}
bool ReaderSettings::CursorHome() const {
    return Bool(KeyCursorHome, true);
}

bool ReaderSettings::StatsPaused() const {
    return Bool(KeyStatsPaused, false);
}
void ReaderSettings::SetStatsPaused(bool paused) {
    settings->setValue(KeyStatsPaused, paused);
}
qint64 ReaderSettings::IdleTimeoutMs() const {
    return static_cast<qint64>(Int(KeyIdleMinutes, 5, 1, 60)) * 60000;
}
bool ReaderSettings::SessionHud() const {
    return Bool(KeySessionHud, true);
}

bool ReaderSettings::DiscordEnabled() const {
    return Bool(KeyDiscordEnabled, false);
}

QString ReaderSettings::DiscordApplicationId() const {
    return String(KeyDiscordApplicationId, {}).trimmed();
}

bool ReaderSettings::TextHookEnabled() const {
    return Bool(KeyHookEnabled, false);
}
int ReaderSettings::TextHookPort() const {
    return Int(KeyHookPort, 6677, 1, 65535);
}
bool ReaderSettings::TextHookLan() const {
    return Bool(KeyHookLan, false);
}
QString ReaderSettings::TextHookFormat() const {
    return String(KeyHookFormat, QStringLiteral("plain"));
}
bool ReaderSettings::TextHookSpeaker() const {
    return Bool(KeyHookSpeaker, false);
}
bool ReaderSettings::ClipboardAuto() const {
    return Bool(KeyClipboardAuto, false);
}

QString ReaderSettings::CardAudioValue(CardAudio choice) {
    switch (choice) {
    case CardAudio::Always:
        return QStringLiteral("always");
    case CardAudio::Never:
        return QStringLiteral("never");
    case CardAudio::Ask:
        break;
    }
    return QStringLiteral("ask");
}

std::optional<ReaderSettings::CardAudio> ReaderSettings::CardAudioOfValue(const QString& value) {
    if (value == QStringLiteral("always")) {
        return CardAudio::Always;
    }
    if (value == QStringLiteral("ask")) {
        return CardAudio::Ask;
    }
    if (value == QStringLiteral("never")) {
        return CardAudio::Never;
    }
    return std::nullopt;
}

ReaderSettings::CardAudio ReaderSettings::CardAudioGeneral() const {
    return CardAudioOfValue(String(KeyCardAudio, {})).value_or(CardAudio::Ask);
}

std::optional<ReaderSettings::CardAudio> ReaderSettings::CardAudioOf(const QString& game_key) const {
    if (game_key.isEmpty()) {
        return std::nullopt;
    }
    return CardAudioOfValue(String(KeyCardAudioGame + game_key, {}));
}

void ReaderSettings::SetCardAudioOf(const QString& game_key, std::optional<CardAudio> choice) {
    if (game_key.isEmpty()) {
        return;
    }
    if (choice) {
        settings->setValue(KeyCardAudioGame + game_key, CardAudioValue(*choice));
    } else {
        settings->remove(KeyCardAudioGame + game_key);
    }
}

ReaderSettings::CardAudio ReaderSettings::CardAudioFor(
    const QString& game_key, std::optional<CardAudio> profile_default) const {
    if (const auto own = CardAudioOf(game_key)) {
        return *own;
    }
    return profile_default.value_or(CardAudioGeneral());
}

bool ReaderSettings::CaptureAudio() const {
    return Bool(KeyCaptureAudio, true);
}
int ReaderSettings::AudioClipMaxSeconds() const {
    return Int(KeyAudioMax, 20, 3, 60);
}
int ReaderSettings::AudioLeadMs() const {
    return Int(KeyAudioLead, 0, 0, 2000);
}
int ReaderSettings::ScreenshotCacheLines() const {
    return Int(KeyScreenshotCache, 30, 0, 200);
}
int ReaderSettings::ScreenshotMaxWidth() const {
    return Int(KeyScreenshotWidth, 1280, 320, 3840);
}

ReaderSettings::SaveSyncMethod ReaderSettings::SaveSyncMode() const {
    const QString mode = String(KeySaveSyncMode, {}).trimmed().toLower();
    if (mode == QStringLiteral("hub")) {
        return SaveSyncMethod::Hub;
    }
    if (mode == QStringLiteral("client")) {
        return SaveSyncMethod::Client;
    }
    if (mode == QStringLiteral("off")) {
        return SaveSyncMethod::Off;
    }
    return Bool(KeySaveSyncEnabled, false) ? SaveSyncMethod::Client : SaveSyncMethod::Off;
}
QString ReaderSettings::SaveSyncModeText(SaveSyncMethod method) {
    switch (method) {
    case SaveSyncMethod::Hub:
        return QStringLiteral("hub");
    case SaveSyncMethod::Client:
        return QStringLiteral("client");
    case SaveSyncMethod::Off:
        break;
    }
    return QStringLiteral("off");
}
bool ReaderSettings::SaveSyncEnabled() const {
    return SaveSyncMode() != SaveSyncMethod::Off;
}
int ReaderSettings::SaveSyncPort() const {
    // 8642 is the standalone service's port too; below 1024 a program may not listen.
    return Int(KeySaveSyncPort, 8642, 1024, 65535);
}
bool ReaderSettings::SaveSyncAsked() const {
    return Bool(KeySaveSyncAsked, false);
}
bool ReaderSettings::SetupAsked(const QString& key) const {
    return Bool(key, false);
}
QString ReaderSettings::SaveSyncServer() const {
    return String(KeySaveSyncServer, {}).trimmed();
}
QString ReaderSettings::SaveSyncCode() const {
    return String(KeySaveSyncCode, {}).trimmed();
}
QString ReaderSettings::StatsInstall() {
    static const QRegularExpression install{QStringLiteral("^[0-9a-f]{16}$")};
    const QString kept = String(KeyStatsInstall, {});
    if (install.match(kept).hasMatch()) {
        return kept;
    }
    const QString made =
        QStringLiteral("%1").arg(QRandomGenerator::system()->generate64(), 16, 16, QLatin1Char('0'));
    settings->setValue(KeyStatsInstall, made);
    settings->sync();
    return made;
}

QString ReaderSettings::SaveSyncDevice() const {
    const QString name = String(KeySaveSyncDevice, {}).trimmed();
    if (!name.isEmpty()) {
        return name;
    }
    const QString host = QSysInfo::machineHostName();
    return host.isEmpty() ? QStringLiteral("Computer") : host;
}

QString ReaderSettings::AnkiTransport() const {
    // On a computer cards go through AnkiConnect, which is asked at its usual address until
    // another one is entered.
    return String(KeyAnkiTransport, QStringLiteral("ankiconnect"));
}
QString ReaderSettings::AnkiDeck() const {
    return String(KeyAnkiDeck, {});
}
QString ReaderSettings::AnkiModel() const {
    return String(KeyAnkiModel, {});
}
QString ReaderSettings::AnkiFieldMap() const {
    const QString stored = String(KeyAnkiFields, {});
    // Once, a mapping made before {word-audio} existed gets it in the fields that are for the
    // word's sound and were left empty. (Written from a reading function: it is the one place
    // every reader of the mapping goes through.)
    static const QString offered_key = QStringLiteral("reader_anki_word_audio_offered");
    if (stored.isEmpty() || Bool(offered_key, false)) {
        return stored;
    }
    const QString offered =
        CardBuilder::MappingToJson(CardBuilder::OfferWordAudio(CardBuilder::ParseMapping(stored)));
    auto* self = const_cast<ReaderSettings*>(this);
    self->SetValue(KeyAnkiFields, offered);
    self->SetValue(offered_key, true);
    return offered;
}
QString ReaderSettings::AnkiTags() const {
    return String(KeyAnkiTags, QStringLiteral("ppsspp-reader"));
}
QString ReaderSettings::AnkiConnectUrl() const {
    return String(KeyAnkiConnectUrl, QStringLiteral("http://127.0.0.1:8765"));
}
QString ReaderSettings::AnkiConnectKey() const {
    return String(KeyAnkiConnectKey, {});
}
bool ReaderSettings::AnkiAllowDuplicates() const {
    return Bool(KeyAnkiDuplicates, false);
}
bool ReaderSettings::AnkiDuplicatesAnywhere() const {
    return Bool(KeyAnkiDuplicatesAnywhere, false);
}

QString ReaderSettings::AnkiKeyField(const QString& model) const {
    const QString stored = String(KeyAnkiKeyField, {});
    const qsizetype at = stored.indexOf(Separator);
    if (at > 0 && stored.left(at) == model) {
        return stored.mid(at + 1);
    }
    return {};
}

QString ReaderSettings::KeyFieldValue(const QString& model, const QString& field) {
    return model + Separator + field;
}

QString ReaderSettings::AnkiAddressField(const QString& address) const {
    const QString stored = String(KeyAnkiAddressField, {});
    const qsizetype at = stored.indexOf(Separator);
    if (at > 0 && stored.left(at) == address.trimmed()) {
        return stored.mid(at + 1);
    }
    return {};
}

QString ReaderSettings::AddressFieldValue(const QString& address, const QString& field) {
    return address.trimmed() + Separator + field;
}

} // namespace ReaderApp
