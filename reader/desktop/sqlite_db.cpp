// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the PPSSPP Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

#include <string>
#include <sqlite3.h>
#include "reader/desktop/sqlite_db.h"

namespace ReaderApp {

namespace {

[[noreturn]] void Fail(sqlite3* handle, const char* what) {
    std::string message = what;
    if (handle) {
        message += ": ";
        message += sqlite3_errmsg(handle);
    }
    throw SqliteError(message);
}

} // namespace

SqliteDb::~SqliteDb() {
    Close();
}

void SqliteDb::Open(const QString& path) {
    Close();
    const QByteArray utf8 = path.toUtf8();
    if (sqlite3_open_v2(utf8.constData(), &handle,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                        nullptr) != SQLITE_OK) {
        const std::string message =
            std::string("Cannot open ") + utf8.constData() + ": " +
            (handle ? sqlite3_errmsg(handle) : "out of memory");
        sqlite3_close(handle);
        handle = nullptr;
        throw SqliteError(message);
    }
    sqlite3_busy_timeout(handle, 5000);
    // The app is the only writer; a write-ahead log keeps readers going during an import.
    Exec("PRAGMA journal_mode=WAL");
    Exec("PRAGMA synchronous=NORMAL");
}

void SqliteDb::Close() {
    if (handle) {
        sqlite3_close(handle);
        handle = nullptr;
    }
}

void SqliteDb::Exec(const char* sql) {
    if (!handle) {
        Fail(nullptr, "The database is not open");
    }
    if (sqlite3_exec(handle, sql, nullptr, nullptr, nullptr) != SQLITE_OK) {
        Fail(handle, sql);
    }
}

qint64 SqliteDb::LastInsertId() const {
    return handle ? sqlite3_last_insert_rowid(handle) : 0;
}

int SqliteDb::UserVersion() {
    Statement statement(*this, "PRAGMA user_version");
    return statement.Step() ? static_cast<int>(statement.Int(0)) : 0;
}

void SqliteDb::SetUserVersion(int version) {
    const std::string sql = "PRAGMA user_version=" + std::to_string(version);
    Exec(sql.c_str());
}

SqliteDb::Transaction::Transaction(SqliteDb& db_) : db{db_} {
    db.Exec("BEGIN IMMEDIATE");
}

SqliteDb::Transaction::~Transaction() {
    if (!done) {
        try {
            db.Exec("ROLLBACK");
        } catch (const SqliteError&) {
            // Nothing more to be done about it here.
        }
    }
}

void SqliteDb::Transaction::Commit() {
    db.Exec("COMMIT");
    done = true;
}

SqliteDb::Statement::Statement(SqliteDb& db, const char* sql) : handle{db.handle} {
    if (!handle) {
        Fail(nullptr, "The database is not open");
    }
    if (sqlite3_prepare_v2(handle, sql, -1, &statement, nullptr) != SQLITE_OK) {
        Fail(handle, sql);
    }
}

SqliteDb::Statement::~Statement() {
    sqlite3_finalize(statement);
}

SqliteDb::Statement& SqliteDb::Statement::Bind(int index, qint64 value) {
    if (sqlite3_bind_int64(statement, index, value) != SQLITE_OK) {
        Fail(handle, "bind");
    }
    return *this;
}

SqliteDb::Statement& SqliteDb::Statement::Bind(int index, const QString& value) {
    const QByteArray utf8 = value.toUtf8();
    if (sqlite3_bind_text(statement, index, utf8.constData(), static_cast<int>(utf8.size()),
                          SQLITE_TRANSIENT) != SQLITE_OK) {
        Fail(handle, "bind");
    }
    return *this;
}

SqliteDb::Statement& SqliteDb::Statement::BindBlob(int index, const QByteArray& value) {
    // A null pointer would store NULL; an empty blob is still a blob.
    if (sqlite3_bind_blob(statement, index, value.isEmpty() ? "" : value.constData(),
                          static_cast<int>(value.size()), SQLITE_TRANSIENT) != SQLITE_OK) {
        Fail(handle, "bind");
    }
    return *this;
}

SqliteDb::Statement& SqliteDb::Statement::BindNull(int index) {
    if (sqlite3_bind_null(statement, index) != SQLITE_OK) {
        Fail(handle, "bind");
    }
    return *this;
}

bool SqliteDb::Statement::Step() {
    const int result = sqlite3_step(statement);
    if (result == SQLITE_ROW) {
        return true;
    }
    if (result != SQLITE_DONE) {
        Fail(handle, "step");
    }
    return false;
}

void SqliteDb::Statement::Run() {
    while (Step()) {
    }
    Reset();
}

void SqliteDb::Statement::Reset() {
    sqlite3_reset(statement);
    sqlite3_clear_bindings(statement);
}

qint64 SqliteDb::Statement::Int(int column) const {
    return sqlite3_column_int64(statement, column);
}

QString SqliteDb::Statement::Text(int column) const {
    const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(statement, column));
    return text ? QString::fromUtf8(text, sqlite3_column_bytes(statement, column)) : QString();
}

QByteArray SqliteDb::Statement::Blob(int column) const {
    const auto* data = static_cast<const char*>(sqlite3_column_blob(statement, column));
    return data ? QByteArray(data, sqlite3_column_bytes(statement, column)) : QByteArray();
}

bool SqliteDb::Statement::IsNull(int column) const {
    return sqlite3_column_type(statement, column) == SQLITE_NULL;
}

} // namespace ReaderApp
