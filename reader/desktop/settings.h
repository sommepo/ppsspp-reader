// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <optional>
#include <QString>
#include <QVariant>

class QSettings;

namespace ReaderApp {

/**
 * The reader's preferences on the desktop: a file of their own (reader.ini) beside the
 * emulator's settings. The keys are those of the Android app, where there is such a setting
 * there.
 */
class ReaderSettings {
public:
    /// The file the settings are kept in.
    explicit ReaderSettings(const QString& file_path);
    ~ReaderSettings();

    [[nodiscard]] QVariant Value(const QString& key, const QVariant& fallback = {}) const;
    void SetValue(const QString& key, const QVariant& value);
    void Sync();

    // ---- Text in the game's picture ----
    /// Words in the game's own text can be clicked, where the game's profile says where it is.
    [[nodiscard]] bool ScreenText() const;
    /// Draw the outlines of what can be clicked, to check that they sit on the text.
    [[nodiscard]] bool ScreenTextOutline() const;
    /// Playing by touch or click alone, for a game whose profile says how ("-- reader: touch"):
    /// a click beside the text goes on, a click on an answer of a choice picks it.
    [[nodiscard]] bool TouchPlay() const;
    /// How the word being looked up is marked in the game's picture: "outline" (a frame
    /// around it; the first value), "highlight" (a coloured box over it) or "none".
    [[nodiscard]] QString WordMark() const;
    /// The corner of each headword's part of the popup where its card buttons stand:
    /// "bottom_left" (the first value), "bottom_right", "top_left" or "top_right".
    [[nodiscard]] QString CardCorner() const;
    /// Text size of the dictionary popup, in percent.
    [[nodiscard]] int PopupScale() const;
    /// How tall the dictionary popup may get, in percent of the picture's height.
    [[nodiscard]] int PopupHeightPercent() const;
    /// The popup goes away by itself after this long without the pointer on it; 0: it stays.
    [[nodiscard]] int PopupAutoHideSeconds() const;
    /// The popup goes away when the pointer (the mouse, or the controller's cursor) leaves
    /// its word and the popup.
    [[nodiscard]] bool PopupHideOnExit() const;
    /// Looking a word up by resting the mouse pointer on it: "off", "shift" (while Shift is
    /// held) or "always".
    [[nodiscard]] QString HoverLookup() const;

    // ---- Cursor ----
    /// The emulated button (a NativeButton number) that, held, lets the sticks steer a cursor
    /// over the game's picture. -1 for none.
    [[nodiscard]] int CursorButton() const;
    /// The controller button that pauses and resumes the statistics; -1 for none.
    [[nodiscard]] int StatsButton() const;
    /// What back button `button` does (the four grips of a Steam Controller are 1 to 4, its
    /// "..." button is 5): the name of one of the reader's actions, or nothing. Out of the
    /// box: pause the game, pause the statistics, a card of the sentence, the backlog, the
    /// reader's menu.
    [[nodiscard]] QString BackButtonAction(int button) const;
    [[nodiscard]] static QString DefaultBackButtonAction(int button);
    /// The cursor's button switches it on and off with a press each, instead of being held.
    [[nodiscard]] bool CursorToggle() const;
    /// How fast the cursor moves, in percent.
    [[nodiscard]] int CursorSpeed() const;
    /// On a character of the game's text the cursor is a frame around that character.
    [[nodiscard]] bool CursorFrame() const;
    /// With a new line the cursor is put just before its first character.
    [[nodiscard]] bool CursorHome() const;

    // ---- Statistics ----
    [[nodiscard]] bool StatsPaused() const;
    void SetStatsPaused(bool paused);
    [[nodiscard]] qint64 IdleTimeoutMs() const;
    /// Show the running session's characters, speed and time in a corner of the game.
    [[nodiscard]] bool SessionHud() const;
    /// The Discord status (DiscordPresence): off until switched on, and of no effect without
    /// the ID of a Discord application of the user's.
    [[nodiscard]] bool DiscordEnabled() const;
    [[nodiscard]] QString DiscordApplicationId() const;

    // ---- Text output ----
    [[nodiscard]] bool TextHookEnabled() const;
    [[nodiscard]] int TextHookPort() const;
    [[nodiscard]] bool TextHookLan() const;
    [[nodiscard]] QString TextHookFormat() const;
    [[nodiscard]] bool TextHookSpeaker() const;
    [[nodiscard]] bool ClipboardAuto() const;

    // ---- Mining ----
    /**
     * What a card does with its line's audio. That audio is everything the game played while
     * the line was up, so a game without voices still has some: music and effects.
     */
    enum class CardAudio {
        Always,
        Ask, ///< The user is asked for each card, and can listen first.
        Never
    };
    [[nodiscard]] static QString CardAudioValue(CardAudio choice);
    [[nodiscard]] static std::optional<CardAudio> CardAudioOfValue(const QString& value);
    /// The general choice, for every game that has none of its own.
    [[nodiscard]] CardAudio CardAudioGeneral() const;
    /// The choice made for the game `game_key` alone, or nothing while it follows the
    /// general one.
    [[nodiscard]] std::optional<CardAudio> CardAudioOf(const QString& game_key) const;
    /// Sets the choice for the game; nothing makes it follow the general one again.
    void SetCardAudioOf(const QString& game_key, std::optional<CardAudio> choice);
    /// What a card of the game `game_key` does with its line's audio.
    /// The user's choice for the game, else what the game's profile says (`profile_default`:
    /// a game without voices says never), else the general choice.
    [[nodiscard]] CardAudio CardAudioFor(const QString& game_key,
                                         std::optional<CardAudio> profile_default = {}) const;

    [[nodiscard]] bool CaptureAudio() const;
    [[nodiscard]] int AudioHistorySeconds() const {
        return 90;
    }
    [[nodiscard]] int AudioClipMaxSeconds() const;
    [[nodiscard]] int AudioLeadMs() const;
    [[nodiscard]] int ScreenshotCacheLines() const;
    [[nodiscard]] int ScreenshotMaxWidth() const;

    // ---- Anki ----
    /// "none" or "ankiconnect".
    [[nodiscard]] QString AnkiTransport() const;
    [[nodiscard]] QString AnkiDeck() const;
    [[nodiscard]] QString AnkiModel() const;
    [[nodiscard]] QString AnkiFieldMap() const;
    [[nodiscard]] QString AnkiTags() const;
    [[nodiscard]] QString AnkiConnectUrl() const;
    [[nodiscard]] QString AnkiConnectKey() const;
    [[nodiscard]] bool AnkiAllowDuplicates() const;
    /// A word counts as known when any deck has it, not only the one cards go to.
    [[nodiscard]] bool AnkiDuplicatesAnywhere() const;
    /// The first field of note type `model`, which is what Anki tells notes apart by, or
    /// empty when it was not recorded for this note type.
    [[nodiscard]] QString AnkiKeyField(const QString& model) const;
    /// What KeyAnkiKeyField holds: the note type the field belongs to, and the field.
    [[nodiscard]] static QString KeyFieldValue(const QString& model, const QString& field);
    /// The field VN Library's media add-on wanted a web address in when a card went through
    /// it at `address`, or empty. Remembered so that the next card has it at once
    /// (SourceAddress in anki.h).
    [[nodiscard]] QString AnkiAddressField(const QString& address) const;
    /// What KeyAnkiAddressField holds: the AnkiConnect address it was learnt at, and the field.
    [[nodiscard]] static QString AddressFieldValue(const QString& address, const QString& field);

    // ---- Save sync ----
    /// Where the saves are kept, which the user chooses.
    enum class SaveSyncMethod {
        Off,
        /// This computer keeps them: the app runs the service itself (SaveSync::Hub).
        Hub,
        /// Another computer does: an app with the choice above, or the standalone service.
        Client,
    };
    /// A setup from before there was a choice (only the switch) counts as Client.
    [[nodiscard]] SaveSyncMethod SaveSyncMode() const;
    [[nodiscard]] static QString SaveSyncModeText(SaveSyncMethod method);
    /// Saves are kept the same as those of the other devices (src/save_sync).
    [[nodiscard]] bool SaveSyncEnabled() const;
    /// The port this computer answers on when it keeps the saves.
    [[nodiscard]] int SaveSyncPort() const;
    /// The user was asked once, at the first start, whether they want save sync.
    [[nodiscard]] bool SaveSyncAsked() const;
    /// Whether another question of the first start (one of the KeySetup…Asked) was asked.
    [[nodiscard]] bool SetupAsked(const QString& key) const;
    /// The service's address, or several separated by commas.
    [[nodiscard]] QString SaveSyncServer() const;
    [[nodiscard]] QString SaveSyncCode() const;
    /// What this computer is called on the other devices; its own name until one is entered.
    [[nodiscard]] QString SaveSyncDevice() const;
    /// What tells this install's reading statistics from another's on the save sync service:
    /// sixteen hexadecimal digits, made up once.
    [[nodiscard]] QString StatsInstall();

    static const QString KeyAnkiAddressField;
    /// The questions of the first start that were asked (each is asked once).
    static const QString KeySetupDictionariesAsked;
    static const QString KeySetupAnkiAsked;
    static const QString KeySaveSyncMode;
    static const QString KeySaveSyncPort;
    static const QString KeySaveSyncAsked;
    static const QString KeySaveSyncEnabled;
    static const QString KeySaveSyncServer;
    static const QString KeySaveSyncCode;
    static const QString KeySaveSyncDevice;
    static const QString KeyScreenText;
    static const QString KeyScreenTextOutline;
    static const QString KeyTouchPlay;
    static const QString KeyWordMark;
    static const QString KeyCardCorner;
    static const QString KeyPopupScale;
    static const QString KeyPopupHeight;
    static const QString KeyPopupAutoHide;
    static const QString KeyPopupHideOnExit;
    static const QString KeyHoverLookup;
    static const QString KeyCursorButton;
    static const QString KeyStatsButton;
    /// What a controller's back button does: this, with the button's number (1 to 5) after it.
    static const QString KeyBackButton;
    static const QString KeyStatsInstall;
    static const QString KeyCursorToggle;
    static const QString KeyCursorSpeed;
    static const QString KeyCursorFrame;
    static const QString KeyCursorHome;
    static const QString KeyStatsPaused;
    static const QString KeyIdleMinutes;
    static const QString KeySessionHud;
    static const QString KeyDiscordEnabled;
    static const QString KeyDiscordApplicationId;
    static const QString KeyHookEnabled;
    static const QString KeyHookPort;
    static const QString KeyHookLan;
    static const QString KeyHookFormat;
    static const QString KeyHookSpeaker;
    static const QString KeyClipboardAuto;
    static const QString KeyCaptureAudio;
    static const QString KeyAudioMax;
    static const QString KeyAudioLead;
    static const QString KeyScreenshotCache;
    static const QString KeyScreenshotWidth;
    static const QString KeyCardAudio;
    /// Followed by a game's key: that game's own choice.
    static const QString KeyCardAudioGame;
    static const QString KeyAnkiTransport;
    static const QString KeyAnkiDeck;
    static const QString KeyAnkiModel;
    static const QString KeyAnkiFields;
    static const QString KeyAnkiTags;
    static const QString KeyAnkiConnectUrl;
    static const QString KeyAnkiConnectKey;
    static const QString KeyAnkiDuplicates;
    static const QString KeyAnkiDuplicatesAnywhere;
    static const QString KeyAnkiKeyField;

private:
    [[nodiscard]] bool Bool(const QString& key, bool fallback) const;
    [[nodiscard]] int Int(const QString& key, int fallback, int low, int high) const;
    [[nodiscard]] QString String(const QString& key, const QString& fallback) const;

    std::unique_ptr<QSettings> settings;
};

} // namespace ReaderApp
