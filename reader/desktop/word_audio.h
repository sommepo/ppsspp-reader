// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>
#include <QByteArray>
#include <QString>

/**
 * The word of a card as somebody says it: a recording from JapanesePod101's dictionary, the
 * same place Yomitan asks first. The address takes the word as written and its reading; it
 * answers a word it has with the recording (by way of another address) and a word it has not
 * with one and the same recording of a voice saying so, which is told by its fingerprint and
 * never put on a card. Asked only when a card's fields use {word-audio}; the word and its
 * reading go to that server and nothing else does.
 */
namespace ReaderApp::WordAudio {

/// SHA-256 of the recording that says the dictionary has no such word.
extern const char* const Missing;

/// What an address answers, redirections followed; empty when it cannot be had.
using Fetcher = std::function<QByteArray(const QString& address)>;

/// Where the recording of `expression` read as `reading` is asked for. A word written in kana
/// alone is asked for by its reading only, as the dictionary lists it.
[[nodiscard]] QString Address(const QString& expression, const QString& reading);

/// The name the recording is kept under in Anki: the same word gets the same file.
[[nodiscard]] QString FileName(const QString& expression, const QString& reading);

/// True when `bytes` are a recording of a word: something, and not the "no such word" one.
[[nodiscard]] bool IsRecording(const QByteArray& bytes);

/// The recording of the word, or nothing when the dictionary has none or cannot be reached.
/// Blocks for as long as the answer takes (some seconds at most): not for the main thread.
[[nodiscard]] QByteArray Fetch(const QString& expression, const QString& reading,
                               const Fetcher& fetcher = {});

} // namespace ReaderApp::WordAudio
