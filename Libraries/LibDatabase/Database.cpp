/*
 * Copyright (c) 2022-2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteString.h>
#include <AK/String.h>
#include <AK/Time.h>
#include <LibCore/Directory.h>
#include <LibDatabase/Database.h>

#include <sqlite3.h>

namespace Database {

static constexpr StringView sql_error(int error_code)
{
    char const* _sql_error = sqlite3_errstr(error_code);
    return { _sql_error, __builtin_strlen(_sql_error) };
}

#define SQL_TRY(expression)                                                \
    ({                                                                     \
        /* Ignore -Wshadow to allow nesting the macro. */                  \
        AK_IGNORE_DIAGNOSTIC("-Wshadow", auto _sql_result = (expression)); \
        if (_sql_result != SQLITE_OK) [[unlikely]]                         \
            return Error::from_string_view(sql_error(_sql_result));        \
    })

#define SQL_MUST(expression)                                                                                       \
    ({                                                                                                             \
        /* Ignore -Wshadow to allow nesting the macro. */                                                          \
        AK_IGNORE_DIAGNOSTIC("-Wshadow", auto _sql_result = (expression));                                         \
        if (_sql_result != SQLITE_OK) [[unlikely]] {                                                               \
            warnln("\033[31;1mDatabase error\033[0m: {}: {}", sql_error(_sql_result), sqlite3_errmsg(m_database)); \
            VERIFY_NOT_REACHED();                                                                                  \
        }                                                                                                          \
    })

#define ENUMERATE_SQL_TYPES              \
    __ENUMERATE_TYPE(String)             \
    __ENUMERATE_TYPE(ByteString)         \
    __ENUMERATE_TYPE(UnixDateTime)       \
    __ENUMERATE_TYPE(i8)                 \
    __ENUMERATE_TYPE(i16)                \
    __ENUMERATE_TYPE(i32)                \
    __ENUMERATE_TYPE(long)               \
    __ENUMERATE_TYPE(long long)          \
    __ENUMERATE_TYPE(u8)                 \
    __ENUMERATE_TYPE(u16)                \
    __ENUMERATE_TYPE(u32)                \
    __ENUMERATE_TYPE(unsigned long)      \
    __ENUMERATE_TYPE(unsigned long long) \
    __ENUMERATE_TYPE(bool)

ErrorOr<NonnullRefPtr<Database>> Database::create_memory_backed()
{
    sqlite3* sql_database { nullptr };
    SQL_TRY(sqlite3_open(":memory:", &sql_database));
    return create(sql_database);
}

ErrorOr<NonnullRefPtr<Database>> Database::create(ByteString const& directory, StringView name)
{
    TRY(Core::Directory::create(directory, Core::Directory::CreateDirectories::Yes));
    LexicalPath database_path { ByteString::formatted("{}/{}.db", directory, name) };
    sqlite3* sql_database { nullptr };
    SQL_TRY(sqlite3_open(database_path.string().characters(), &sql_database));
    return create(sql_database, database_path);
}

ErrorOr<NonnullRefPtr<Database>> Database::create(sqlite3* sql_database, Optional<LexicalPath> database_path)
{
    auto database = TRY(adopt_nonnull_ref_or_enomem(new (nothrow) Database(sql_database, move(database_path))));

    // Enable the WAL and set the synchronous pragma to normal by default for performance.
    TRY(database->set_journal_mode_pragma(JournalMode::WriteAheadLog));
    TRY(database->set_synchronous_pragma(Synchronous::Normal));

    return database;
}

Database::Database(sqlite3* database, Optional<LexicalPath> database_path)
    : m_database_path(move(database_path))
    , m_database(database)
{
    VERIFY(m_database);
}

Database::~Database()
{
    for (auto* prepared_statement : m_prepared_statements)
        sqlite3_finalize(prepared_statement);

    sqlite3_close(m_database);
}

ErrorOr<StatementID> Database::prepare_statement(StringView statement)
{
    sqlite3_stmt* prepared_statement { nullptr };
    SQL_TRY(sqlite3_prepare_v2(m_database, statement.characters_without_null_termination(), static_cast<int>(statement.length()), &prepared_statement, nullptr));

    auto statement_id = m_prepared_statements.size();
    m_prepared_statements.append(prepared_statement);

    return statement_id;
}

void Database::execute_statement_internal(StatementID statement_id, OnResult on_result)
{
    if (auto result = try_execute_statement_internal(statement_id, move(on_result)); result.is_error()) [[unlikely]] {
        warnln("\033[31;1mDatabase error\033[0m: {}: {}", result.error(), sqlite3_errmsg(m_database));
        VERIFY_NOT_REACHED();
    }
}

ErrorOr<void> Database::try_execute_statement_internal(StatementID statement_id, OnResult on_result)
{
    auto* statement = prepared_statement(statement_id);

    while (true) {
        auto result = sqlite3_step(statement);

        switch (result) {
        case SQLITE_DONE:
            SQL_TRY(sqlite3_reset(statement));
            return {};

        case SQLITE_ROW:
            if (on_result)
                on_result(statement_id);
            continue;

        default:
            // Reset so the failed statement does not stay active and block a later COMMIT or ROLLBACK.
            sqlite3_reset(statement);
            return Error::from_string_view(sql_error(result));
        }
    }
}

int Database::bound_parameter_count(StatementID statement_id)
{
    auto* statement = prepared_statement(statement_id);
    return sqlite3_bind_parameter_count(statement);
}

template<typename ValueType>
void Database::apply_placeholder(StatementID statement_id, int index, ValueType const& value)
{
    if (auto result = try_apply_placeholder(statement_id, index, value); result.is_error()) [[unlikely]] {
        warnln("\033[31;1mDatabase error\033[0m: {}: {}", result.error(), sqlite3_errmsg(m_database));
        VERIFY_NOT_REACHED();
    }
}

#define __ENUMERATE_TYPE(type) \
    template DATABASE_API void Database::apply_placeholder(StatementID, int, type const&);
ENUMERATE_SQL_TYPES
#undef __ENUMERATE_TYPE

template<typename ValueType>
ErrorOr<void> Database::try_apply_placeholder(StatementID statement_id, int index, ValueType const& value)
{
    auto* statement = prepared_statement(statement_id);

    if constexpr (IsSame<ValueType, String>) {
        StringView string { value };
        SQL_TRY(sqlite3_bind_text(statement, index, string.characters_without_null_termination(), static_cast<int>(string.length()), SQLITE_TRANSIENT));
    } else if constexpr (IsSame<ValueType, ByteString>) {
        SQL_TRY(sqlite3_bind_blob(statement, index, value.characters(), static_cast<int>(value.length()), SQLITE_TRANSIENT));
    } else if constexpr (IsSame<ValueType, UnixDateTime>) {
        TRY(try_apply_placeholder(statement_id, index, value.offset_to_epoch().to_milliseconds()));
    } else if constexpr (IsIntegral<ValueType>) {
        if constexpr (sizeof(ValueType) <= sizeof(int))
            SQL_TRY(sqlite3_bind_int(statement, index, static_cast<int>(value)));
        else
            SQL_TRY(sqlite3_bind_int64(statement, index, static_cast<sqlite3_int64>(value)));
    } else {
        static_assert(DependentFalse<ValueType>);
    }

    return {};
}

#define __ENUMERATE_TYPE(type) \
    template DATABASE_API ErrorOr<void> Database::try_apply_placeholder(StatementID, int, type const&);
ENUMERATE_SQL_TYPES
#undef __ENUMERATE_TYPE

template<typename ValueType>
ValueType Database::result_column(StatementID statement_id, int column)
{
    auto* statement = prepared_statement(statement_id);

    if constexpr (IsSame<ValueType, String>) {
        auto length = sqlite3_column_bytes(statement, column);
        auto const* text = reinterpret_cast<char const*>(sqlite3_column_text(statement, column));
        return MUST(String::from_utf8(StringView { text, static_cast<size_t>(length) }));
    } else if constexpr (IsSame<ValueType, ByteString>) {
        auto length = sqlite3_column_bytes(statement, column);
        auto const* text = sqlite3_column_blob(statement, column);
        return ByteString { reinterpret_cast<char const*>(text), static_cast<size_t>(length) };
    } else if constexpr (IsSame<ValueType, UnixDateTime>) {
        auto milliseconds = result_column<sqlite3_int64>(statement_id, column);
        return UnixDateTime::from_milliseconds_since_epoch(milliseconds);
    } else if constexpr (IsIntegral<ValueType>) {
        if constexpr (sizeof(ValueType) <= sizeof(int))
            return static_cast<ValueType>(sqlite3_column_int(statement, column));
        else
            return static_cast<ValueType>(sqlite3_column_int64(statement, column));
    } else {
        static_assert(DependentFalse<ValueType>);
    }
}

#define __ENUMERATE_TYPE(type) \
    template DATABASE_API type Database::result_column(StatementID, int);
ENUMERATE_SQL_TYPES
#undef __ENUMERATE_TYPE

ErrorOr<void> Database::execute_raw(ByteString const& sql)
{
    SQL_TRY(sqlite3_exec(m_database, sql.characters(), nullptr, nullptr, nullptr));
    return {};
}

ErrorOr<void> Database::Transaction::begin()
{
    TRY(m_database.execute_raw("BEGIN IMMEDIATE;"));
    m_active = true;
    return {};
}

Database::Transaction::~Transaction()
{
    if (!m_active)
        return;
    if (auto result = m_database.execute_raw("ROLLBACK;"); result.is_error())
        warnln("\033[31;1mDatabase error\033[0m: Unable to roll back transaction: {}", result.error());
}

ErrorOr<void> Database::Transaction::commit()
{
    VERIFY(m_active);
    TRY(m_database.execute_raw("COMMIT;"));
    m_active = false;
    return {};
}

ErrorOr<MigrationOutcome> Database::migrate(StringView store_name, ReadonlySpan<Migration> migrations, MigrationMode mode)
{
    VERIFY(!migrations.is_empty());
    VERIFY(migrations.first().version >= 1);
    for (size_t i = 1; i < migrations.size(); ++i)
        VERIFY(migrations[i].version > migrations[i - 1].version);

    auto store = TRY(String::from_utf8(store_name));
    auto latest_version = migrations.last().version;

    // Fast path: only reads, so a database from a newer version of Ladybird is left untouched.
    if (auto recorded = TRY(schema_version(store_name)); recorded.has_value()) {
        if (*recorded > latest_version)
            return MigrationOutcome::DatabaseTooNew;
        if (*recorded == latest_version)
            return MigrationOutcome::Success;
    }

    Transaction transaction { *this };
    TRY(transaction.begin());

    TRY(execute_raw("CREATE TABLE IF NOT EXISTS SchemaVersions (store TEXT PRIMARY KEY, version INTEGER NOT NULL);"));

    // Re-read under the write lock; a concurrent process may have migrated since the fast path.
    auto recorded = TRY(schema_version(store_name));
    if (recorded.has_value()) {
        if (*recorded > latest_version)
            return MigrationOutcome::DatabaseTooNew;

        if (*recorded == latest_version) {
            if (mode == MigrationMode::CheckOnly)
                return MigrationOutcome::Success;

            TRY(transaction.commit());
            return MigrationOutcome::Success;
        }
    }

    auto current_version = recorded.value_or(0);

    if (mode == MigrationMode::CheckOnly)
        return MigrationOutcome::Success;

    for (auto const& migration : migrations) {
        if (migration.version <= current_version)
            continue;

        if (!migration.sql.is_empty())
            TRY(execute_raw(migration.sql));

        if (migration.backfill)
            TRY(migration.backfill(*this));
    }

    auto update_version = TRY(prepare_statement("INSERT INTO SchemaVersions (store, version) VALUES (?, ?) ON CONFLICT(store) DO UPDATE SET version = excluded.version;"sv));
    TRY(try_execute_statement(update_version, {}, store, latest_version));

    TRY(transaction.commit());
    return MigrationOutcome::Success;
}

ErrorOr<bool> Database::table_exists(StringView table)
{
    if (!m_table_exists_statement.has_value())
        m_table_exists_statement = TRY(prepare_statement("SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = ?;"sv));

    bool exists = false;
    TRY(try_execute_statement(*m_table_exists_statement, [&](auto) { exists = true; }, TRY(String::from_utf8(table))));
    return exists;
}

ErrorOr<Optional<u32>> Database::schema_version(StringView store)
{
    if (!TRY(table_exists("SchemaVersions"sv)))
        return OptionalNone {};

    if (!m_schema_version_statement.has_value())
        m_schema_version_statement = TRY(prepare_statement("SELECT version FROM SchemaVersions WHERE store = ?;"sv));

    Optional<u32> version;
    TRY(try_execute_statement(*m_schema_version_statement, [&](auto statement_id) { version = result_column<u32>(statement_id, 0); }, TRY(String::from_utf8(store))));
    return version;
}

ErrorOr<void> Database::set_journal_mode_pragma(JournalMode journal_mode)
{
    auto journal_mode_string = [&]() {
        switch (journal_mode) {
        case JournalMode::Delete:
            return "DELETE"sv;
        case JournalMode::Truncate:
            return "TRUNCATE"sv;
        case JournalMode::Persist:
            return "PERSIST"sv;
        case JournalMode::Memory:
            return "MEMORY"sv;
        case JournalMode::WriteAheadLog:
            return "WAL"sv;
        case JournalMode::Off:
            return "OFF"sv;
        }
        VERIFY_NOT_REACHED();
    }();

    auto pragma = ByteString::formatted("PRAGMA journal_mode={};", journal_mode_string);
    TRY(execute_raw(pragma));

    return {};
}

ErrorOr<void> Database::set_synchronous_pragma(Synchronous synchronous)
{
    auto synchronous_string = [&]() {
        switch (synchronous) {
        case Synchronous::Off:
            return "OFF"sv;
        case Synchronous::Normal:
            return "NORMAL"sv;
        case Synchronous::Full:
            return "FULL"sv;
        case Synchronous::Extra:
            return "EXTRA"sv;
        }
        VERIFY_NOT_REACHED();
    }();

    auto pragma = ByteString::formatted("PRAGMA synchronous={};", synchronous_string);
    TRY(execute_raw(pragma));

    return {};
}

}
