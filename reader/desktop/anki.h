// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>
#include <optional>
#include <stdexcept>
#include <QByteArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>

namespace ReaderApp {

/// Something went wrong talking to Anki; the message is shown to the user.
class AnkiError : public std::runtime_error {
public:
    explicit AnkiError(const QString& message)
        : std::runtime_error(message.toStdString()), text{message} {}

    [[nodiscard]] const QString& Message() const {
        return text;
    }

private:
    QString text;
};

/// What Anki itself is asked to do about a note whose first field it already has.
enum class Duplicates { Allow, RefuseInDeck, RefuseAnywhere };

/// Search strings in the syntax of Anki's card browser.
namespace AnkiSearch {
/// Notes whose `field` is exactly `value`, within `deck` and its subdecks unless it is empty.
[[nodiscard]] QString FieldIs(const QString& deck, const QString& field, const QString& value);
[[nodiscard]] QString Notes(const QList<qint64>& note_ids);
/// `text` for use inside a quoted term: nothing in it acts as a wildcard or ends the quote.
[[nodiscard]] QString Escape(const QString& text);
} // namespace AnkiSearch

/**
 * A card's source as a web address.
 *
 * A card made with Yomitan in a browser carries the page's address ({url}); a game has no
 * page of that kind, but it has one at its maker's: Nintendo's address for a program by its
 * ID. That is what the marker {url} gives here, so a note format that maps a field to {url}
 * means the same in both.
 *
 * It matters for cards that go to Anki through VN Library's media add-on (the way from a
 * handheld into a desktop collection): the add-on refuses a note whose source field holds no
 * web address, because for VN Library's own cards that address is how it finds the picture
 * and the sound. This app brings its own. When the add-on refuses a card for that reason,
 * the field it names gets the game's address, as a link around what the field held, and the
 * card is sent again: the user sets nothing up for it.
 */
namespace SourceAddress {
/// The game's page at Nintendo, or nothing without a game's ID (16 hexadecimal digits).
[[nodiscard]] QString Of(const QString& title_id);
/// The field the add-on wants an address in, when `error` is its refusal for the lack of
/// one; empty for anything else. The two sentences are the add-on's (vnkit/anki_bridge.py
/// in VN Library, MediaBridge.enrich): an empty field, and one with text but no address.
[[nodiscard]] QString AskedFor(const QString& error);
[[nodiscard]] bool HasAddress(const QString& value);
/// `fields` with `field` holding `url`: as a link around what the field holds, or around the
/// game's `title` when it holds nothing. A field that has an address already is left. A note
/// type without such a field is not harmed: Anki ignores a field it does not have.
[[nodiscard]] QMap<QString, QString> With(const QMap<QString, QString>& fields,
                                          const QString& field, const QString& url,
                                          const QString& title);
/// A note's id, and the field that was given an address for it (to remember), if any.
struct Added {
    qint64 id{};
    QString field;
};
/// Adds the note with `add`, which throws AnkiError. `known` is the field an earlier card had
/// to be given an address in; it gets one at once. A refusal for the lack of an address is
/// answered once, by giving the named field the address and sending again.
Added AddNote(const std::function<qint64(const QMap<QString, QString>&)>& add,
              const QMap<QString, QString>& fields, const QString& url, const QString& title,
              const QString& known);
} // namespace SourceAddress

/**
 * AnkiConnect (version 6 protocol): Anki on this computer or another one, reached over HTTP.
 * Media is sent inline, so Anki does not need access to this app's files. All calls block and
 * may throw AnkiError: use a worker thread.
 */
class AnkiConnectClient {
public:
    static constexpr int ConnectTimeoutMs = 8000;
    static constexpr int ReadTimeoutMs = 30000;

    explicit AnkiConnectClient(QString url_, QString key_ = {},
                               int connect_timeout_ms_ = ConnectTimeoutMs,
                               int read_timeout_ms_ = ReadTimeoutMs);

    [[nodiscard]] QStringList DeckNames();
    [[nodiscard]] QStringList ModelNames();
    [[nodiscard]] QStringList FieldNames(const QString& model);
    /// Puts `data` into the collection's media. Returns the name it was stored under.
    QString StoreMedia(const QByteArray& data, const QString& preferred_name);
    /// Adds a note and returns its id. `fields` is keyed by field name.
    qint64 AddNote(const QString& deck, const QString& model, const QMap<QString, QString>& fields,
                   const QStringList& tags, Duplicates duplicates);
    /// Ids of the notes whose `field` holds exactly `value`: those with a card in `deck` or
    /// one of its subdecks, or in the whole collection when `deck` is empty.
    [[nodiscard]] QList<qint64> FindNotes(const QString& deck, const QString& field,
                                          const QString& value);
    /// Opens Anki's card browser on `note_ids`.
    void ShowNotes(const QList<qint64>& note_ids);
    /// The protocol version the server reports; a cheap way to test the connection.
    [[nodiscard]] int Version();

    /// The request body for `action`, as sent. For tests.
    [[nodiscard]] QByteArray RequestBody(const QString& action, const QJsonObject* params) const;

private:
    QJsonValue Invoke(const QString& action, const QJsonObject* params = nullptr);
    QByteArray Post(const QByteArray& body);

    QString url;
    QString key;
    int connect_timeout_ms;
    int read_timeout_ms;
};

} // namespace ReaderApp
