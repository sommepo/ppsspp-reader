// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <stdexcept>
#include <QByteArray>
#include <QString>

struct sqlite3;
struct sqlite3_stmt;

namespace ReaderApp {

/// Something SQLite refused. The databases are the app's own, so this is not expected to happen.
class SqliteError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/**
 * A SQLite database file (or ":memory:"). Thin on purpose: the reader's two databases have the
 * same tables as the Android app's, and the queries are written out where they are used.
 *
 * One connection may be used from several threads, one at a time (SQLite's serialized mode).
 */
class SqliteDb {
public:
    /// One prepared statement. Bind, then Step until it returns false.
    class Statement {
    public:
        Statement(SqliteDb& db, const char* sql);
        ~Statement();
        Statement(const Statement&) = delete;
        Statement& operator=(const Statement&) = delete;

        /// Parameters are numbered from 1.
        Statement& Bind(int index, qint64 value);
        Statement& Bind(int index, const QString& value);
        Statement& BindBlob(int index, const QByteArray& value);
        Statement& BindNull(int index);

        /// Runs to the next row. False when there is none (or the statement is done).
        bool Step();
        /// Runs a statement that returns nothing, and makes it ready to be bound again.
        void Run();
        void Reset();

        /// Columns are numbered from 0.
        [[nodiscard]] qint64 Int(int column) const;
        [[nodiscard]] QString Text(int column) const;
        [[nodiscard]] QByteArray Blob(int column) const;
        [[nodiscard]] bool IsNull(int column) const;

    private:
        sqlite3* handle;
        sqlite3_stmt* statement{};
    };

    SqliteDb() = default;
    ~SqliteDb();
    SqliteDb(const SqliteDb&) = delete;
    SqliteDb& operator=(const SqliteDb&) = delete;

    /// Opens (and creates) the file. Throws when it cannot.
    void Open(const QString& path);
    void Close();
    [[nodiscard]] bool IsOpen() const {
        return handle != nullptr;
    }

    void Exec(const char* sql);
    [[nodiscard]] qint64 LastInsertId() const;
    /// The schema version the file says it has (PRAGMA user_version).
    [[nodiscard]] int UserVersion();
    void SetUserVersion(int version);

    /// BEGIN on construction; ROLLBACK on destruction unless Commit was called.
    class Transaction {
    public:
        explicit Transaction(SqliteDb& db_);
        ~Transaction();
        void Commit();

    private:
        SqliteDb& db;
        bool done{};
    };

private:
    friend class Statement;
    sqlite3* handle{};
};

} // namespace ReaderApp
