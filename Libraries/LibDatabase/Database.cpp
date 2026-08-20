/*
 * Copyright (c) 2022-2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
#include <AK/ByteString.h>
#include <AK/String.h>
#include <AK/Time.h>
#include <AK/Utf16String.h>
#include <LibCore/Directory.h>
#include <LibDatabase/Database.h>
#include <LibDatabase/ResultRow.h>

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

#define ENUMERATE_SQL_TYPES        \
    __ENUMERATE_TYPE(String)       \
    __ENUMERATE_TYPE(Utf16String)  \
    __ENUMERATE_TYPE(ByteString)   \
    __ENUMERATE_TYPE(ByteBuffer)   \
    __ENUMERATE_TYPE(UnixDateTime) \
    __ENUMERATE_TYPE(i8)           \
    __ENUMERATE_TYPE(i16)          \
    __ENUMERATE_TYPE(i32)          \
    __ENUMERATE_TYPE(long)         \
    __ENUMERATE_TYPE(long long)    \
    __ENUMERATE_TYPE(u8)           \
    __ENUMERATE_TYPE(u16)          \
    __ENUMERATE_TYPE(u32)          \
    __ENUMERATE_TYPE(double)       \
    __ENUMERATE_TYPE(bool)

#define ENUMERATE_SQL_BIND_ONLY_TYPES \
    __ENUMERATE_TYPE(Bytes)           \
    __ENUMERATE_TYPE(ReadonlyBytes)

ErrorOr<NonnullRefPtr<Database>> Database::create_memory_backed(Options options)
{
    sqlite3* sql_database { nullptr };
    SQL_TRY(sqlite3_open(":memory:", &sql_database));
    return create(sql_database, options);
}

ErrorOr<NonnullRefPtr<Database>> Database::create(ByteString const& directory, StringView name, Options options)
{
    TRY(Core::Directory::create(directory, Core::Directory::CreateDirectories::Yes));
    LexicalPath database_path { ByteString::formatted("{}/{}.db", directory, name) };
    sqlite3* sql_database { nullptr };
    SQL_TRY(sqlite3_open(database_path.string().characters(), &sql_database));
    return create(sql_database, options, database_path);
}

ErrorOr<NonnullRefPtr<Database>> Database::create(sqlite3* sql_database, Options options, Optional<LexicalPath> database_path)
{
    auto database = TRY(adopt_nonnull_ref_or_enomem(new (nothrow) Database(sql_database, move(database_path))));

    options.journal_mode = TRY(database->set_journal_mode_pragma(options.journal_mode));
    TRY(database->set_synchronous_pragma(options.synchronous));
    TRY(database->set_foreign_keys_pragma(options.foreign_keys));
    database->m_options = options;

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
    for (auto& prepared : m_prepared_statements)
        sqlite3_finalize(prepared.statement);

    sqlite3_close(m_database);
}

ErrorOr<StatementID> Database::prepare_statement(StringView statement)
{
    sqlite3_stmt* prepared_statement { nullptr };
    SQL_TRY(sqlite3_prepare_v2(m_database, statement.characters_without_null_termination(), static_cast<int>(statement.length()), &prepared_statement, nullptr));

    auto parameter_count = static_cast<size_t>(sqlite3_bind_parameter_count(prepared_statement));
    Bitmap bound_parameters;
    if (parameter_count > 0) {
        auto result = Bitmap::create(parameter_count, false);
        if (result.is_error()) {
            sqlite3_finalize(prepared_statement);
            return result.release_error();
        }
        bound_parameters = result.release_value();
    }

    auto statement_id = m_prepared_statements.size();
    m_prepared_statements.append({
        .statement = prepared_statement,
        .bound_parameters = move(bound_parameters),
    });

    return statement_id;
}

void Database::execute_statement_internal(StatementID statement_id, OnResult on_result)
{
    if (auto result = try_execute_statement_internal(statement_id, move(on_result)); result.is_error()) [[unlikely]] {
        warnln("\033[31;1mDatabase error\033[0m: {}: {}", result.error(), sqlite3_errmsg(m_database));
        VERIFY_NOT_REACHED();
    }
}

Database::StatementExecutionOutcome Database::execute_interruptible_statement_internal(StatementID statement_id, OnResult on_result)
{
    auto* statement = prepared_statement(statement_id).statement;

    while (true) {
        auto result = sqlite3_step(statement);

        switch (result) {
        case SQLITE_DONE:
            SQL_MUST(sqlite3_reset(statement));
            return StatementExecutionOutcome::Completed;

        case SQLITE_ROW:
            if (on_result)
                MUST(on_result(statement_id));
            continue;

        case SQLITE_INTERRUPT:
            // sqlite3_reset() reports the interrupted statement's error code, so intentionally ignore
            // its result here. Resetting still makes the prepared statement reusable.
            sqlite3_reset(statement);
            return StatementExecutionOutcome::Interrupted;

        default:
            sqlite3_reset(statement);
            warnln("\033[31;1mDatabase error\033[0m: {}: {}", sql_error(result), sqlite3_errmsg(m_database));
            VERIFY_NOT_REACHED();
        }
    }
}

void Database::interrupt()
{
    sqlite3_interrupt(m_database);
}

ErrorOr<void> Database::try_execute_statement_internal(StatementID statement_id, OnResult on_result)
{
    auto* statement = prepared_statement(statement_id).statement;

    Optional<Error> row_error;
    while (sqlite3_step(statement) == SQLITE_ROW) {
        if (!on_result)
            continue;
        if (auto result = on_result(statement_id); result.is_error()) {
            row_error = result.release_error();
            break;
        }
    }

    // Reset the statement and re-report any sqlite3_step() failure.
    SQL_TRY(sqlite3_reset(statement));
    if (row_error.has_value())
        return row_error.release_value();
    return {};
}

int Database::bound_parameter_count(StatementID statement_id)
{
    auto* statement = prepared_statement(statement_id).statement;
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

#define __ENUMERATE_TYPE(type)                                                             \
    template DATABASE_API void Database::apply_placeholder(StatementID, int, type const&); \
    template DATABASE_API void Database::apply_placeholder(StatementID, int, Optional<type> const&);
ENUMERATE_SQL_TYPES
ENUMERATE_SQL_BIND_ONLY_TYPES
#undef __ENUMERATE_TYPE

static ErrorOr<void> bind_blob_bytes(sqlite3_stmt* statement, int index, ReadonlyBytes bytes)
{
    // SQLite binds a null pointer as NULL, so empty blobs need a non-null sentinel.
    static constexpr u8 empty_blob {};
    SQL_TRY(sqlite3_bind_blob64(statement, index, bytes.is_empty() ? &empty_blob : bytes.data(), bytes.size(), SQLITE_TRANSIENT));
    return {};
}

template<typename ValueType>
ErrorOr<void> Database::try_apply_placeholder(StatementID statement_id, int index, ValueType const& value)
{
    auto* statement = prepared_statement(statement_id).statement;

    if constexpr (IsSpecializationOf<ValueType, Optional>) {
        if (!value.has_value()) {
            SQL_TRY(sqlite3_bind_null(statement, index));
            return {};
        }
        TRY(try_apply_placeholder(statement_id, index, *value));
    } else if constexpr (IsSame<ValueType, String>) {
        auto bytes = value.bytes();
        SQL_TRY(sqlite3_bind_text64(statement, index, reinterpret_cast<char const*>(bytes.data()), bytes.size(), SQLITE_TRANSIENT, SQLITE_UTF8));
    } else if constexpr (IsSame<ValueType, Utf16String>) {
        // Stored as WTF-8 TEXT; SQLite's own UTF-16 interface would replace lonely surrogates with U+FFFD.
        TRY(try_apply_placeholder(statement_id, index, value.to_utf8()));
    } else if constexpr (IsSame<ValueType, ByteString>) {
        TRY(bind_blob_bytes(statement, index, value.bytes()));
    } else if constexpr (IsOneOf<ValueType, ByteBuffer, Bytes, ReadonlyBytes>) {
        TRY(bind_blob_bytes(statement, index, value));
    } else if constexpr (IsSame<ValueType, UnixDateTime>) {
        TRY(try_apply_placeholder(statement_id, index, value.offset_to_epoch().to_milliseconds()));
    } else if constexpr (IsSame<ValueType, double>) {
        SQL_TRY(sqlite3_bind_double(statement, index, value));
    } else if constexpr (IsIntegral<ValueType>) {
        static_assert(!Detail::is_unsupported_unsigned_sql_integer<ValueType>);
        static_assert(sizeof(ValueType) <= sizeof(sqlite3_int64));
        SQL_TRY(sqlite3_bind_int64(statement, index, static_cast<sqlite3_int64>(value)));
    } else {
        static_assert(DependentFalse<ValueType>);
    }

    return {};
}

#define __ENUMERATE_TYPE(type)                                                                          \
    template DATABASE_API ErrorOr<void> Database::try_apply_placeholder(StatementID, int, type const&); \
    template DATABASE_API ErrorOr<void> Database::try_apply_placeholder(StatementID, int, Optional<type> const&);
ENUMERATE_SQL_TYPES
ENUMERATE_SQL_BIND_ONLY_TYPES
#undef __ENUMERATE_TYPE

template<typename ValueType>
requires(!Detail::is_unsupported_unsigned_sql_integer<ValueType>)
ValueType Database::result_column(StatementID statement_id, int column)
{
    auto* statement = prepared_statement(statement_id).statement;

    if constexpr (IsSame<ValueType, String>) {
        auto const* text = reinterpret_cast<char const*>(sqlite3_column_text(statement, column));
        auto length = sqlite3_column_bytes(statement, column);
        return MUST(String::from_utf8(StringView { text, static_cast<size_t>(length) }));
    } else if constexpr (IsSame<ValueType, Utf16String>) {
        auto const* text = reinterpret_cast<char const*>(sqlite3_column_text(statement, column));
        auto length = sqlite3_column_bytes(statement, column);
        return Utf16String::from_utf8(StringView { text, static_cast<size_t>(length) });
    } else if constexpr (IsSame<ValueType, ByteString>) {
        auto const* blob = sqlite3_column_blob(statement, column);
        auto length = sqlite3_column_bytes(statement, column);
        return ByteString { reinterpret_cast<char const*>(blob), static_cast<size_t>(length) };
    } else if constexpr (IsSame<ValueType, ByteBuffer>) {
        auto const* blob = sqlite3_column_blob(statement, column);
        auto length = sqlite3_column_bytes(statement, column);
        return MUST(ByteBuffer::copy(blob, static_cast<size_t>(length)));
    } else if constexpr (IsSame<ValueType, UnixDateTime>) {
        auto milliseconds = result_column<sqlite3_int64>(statement_id, column);
        return UnixDateTime::from_milliseconds_since_epoch(milliseconds);
    } else if constexpr (IsSame<ValueType, double>) {
        return sqlite3_column_double(statement, column);
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

ErrorOr<i64, Database::ColumnReadError> Database::result_i64_checked(StatementID statement_id, int column)
{
    auto* statement = prepared_statement(statement_id).statement;
    if (sqlite3_column_type(statement, column) != SQLITE_INTEGER)
        return ColumnReadError::WrongType;
    return static_cast<i64>(sqlite3_column_int64(statement, column));
}

ErrorOr<StringView, Database::ColumnReadError> Database::result_text_column_bounded(Badge<ResultRow>, StatementID statement_id, int column, size_t max_bytes)
{
    auto* statement = prepared_statement(statement_id).statement;
    if (sqlite3_column_type(statement, column) != SQLITE_TEXT)
        return ColumnReadError::WrongType;
    // sqlite3_column_bytes() must follow the accessor; taken first it can report a converted length.
    auto const* text = reinterpret_cast<char const*>(sqlite3_column_text(statement, column));
    auto length = static_cast<size_t>(sqlite3_column_bytes(statement, column));
    if (length > max_bytes)
        return ColumnReadError::TooLarge;
    return StringView { text, length };
}

ErrorOr<ReadonlyBytes, Database::ColumnReadError> Database::result_blob_column_bounded(Badge<ResultRow>, StatementID statement_id, int column, size_t max_bytes)
{
    auto* statement = prepared_statement(statement_id).statement;
    if (sqlite3_column_type(statement, column) != SQLITE_BLOB)
        return ColumnReadError::WrongType;
    auto const* blob = sqlite3_column_blob(statement, column);
    auto length = static_cast<size_t>(sqlite3_column_bytes(statement, column));
    if (length > max_bytes)
        return ColumnReadError::TooLarge;
    return ReadonlyBytes { blob, length };
}

template<typename ValueType>
ErrorOr<void> Database::bind_parameter(StatementID statement_id, StringView name, ValueType const& value)
{
    auto& prepared = prepared_statement(statement_id);

    // The placeholder is resolved as a C string, so an embedded NUL would target a shorter, different one.
    if (name.contains('\0'))
        return Error::from_string_literal("Bound parameter name contains an embedded NUL");

    auto placeholder = ByteString::formatted(":{}", name);
    auto index = sqlite3_bind_parameter_index(prepared.statement, placeholder.characters());
    if (index == 0)
        return Error::from_string_literal("Unknown bound parameter name");

    TRY(try_apply_placeholder(statement_id, index, value));

    prepared.bound_parameters.set(index - 1, true);
    return {};
}

#define __ENUMERATE_TYPE(type)                                                                          \
    template DATABASE_API ErrorOr<void> Database::bind_parameter(StatementID, StringView, type const&); \
    template DATABASE_API ErrorOr<void> Database::bind_parameter(StatementID, StringView, Optional<type> const&);
ENUMERATE_SQL_TYPES
ENUMERATE_SQL_BIND_ONLY_TYPES
#undef __ENUMERATE_TYPE

ErrorOr<void> Database::try_step_bound_statement(StatementID statement_id, OnResultRow on_result)
{
    auto& prepared = prepared_statement(statement_id);

    // Reject unbound parameters, which SQLite otherwise treats as NULL.
    if (auto unbound = prepared.bound_parameters.find_first_unset(); unbound.has_value()) {
        auto parameter_index = static_cast<int>(*unbound) + 1;
        auto const* name = sqlite3_bind_parameter_name(prepared.statement, parameter_index);
        dbgln("Database: refusing to step statement with unbound parameter {} ({})", parameter_index, name ? name : "?");
        return Error::from_string_literal("Statement executed with an unbound parameter");
    }

    if (!on_result)
        return try_execute_statement_internal(statement_id, {});

    ResultRow row { {}, *this, statement_id };
    return try_execute_statement_internal(statement_id, [&](StatementID) { return on_result(row); });
}

ErrorOr<int> Database::result_column_index(StatementID statement_id, StringView name)
{
    auto& prepared = prepared_statement(statement_id);

    auto generation = sqlite3_stmt_status(prepared.statement, SQLITE_STMTSTATUS_REPREPARE, 0);
    if (generation != prepared.result_column_generation) {
        prepared.result_column_indices.clear();
        auto column_count = sqlite3_column_count(prepared.statement);
        for (int column = 0; column < column_count; ++column) {
            auto const* column_name = sqlite3_column_name(prepared.statement, column);
            if (column_name == nullptr)
                continue;
            if (prepared.result_column_indices.set(ByteString { column_name }, column) == HashSetResult::ReplacedExistingEntry)
                prepared.result_column_indices.set(ByteString { column_name }, -1);
        }
        prepared.result_column_generation = generation;
    }

    auto match = prepared.result_column_indices.get(name);
    if (!match.has_value())
        return Error::from_string_literal("Unknown result column name");
    if (*match == -1)
        return Error::from_string_literal("Ambiguous result column name");
    return *match;
}

bool Database::result_column_is_null(Badge<ResultRow>, StatementID statement_id, int column)
{
    return sqlite3_column_type(prepared_statement(statement_id).statement, column) == SQLITE_NULL;
}

void Database::clear_bound_parameters(StatementID statement_id)
{
    auto& prepared = prepared_statement(statement_id);
    SQL_MUST(sqlite3_clear_bindings(prepared.statement));
    if (prepared.bound_parameters.size() > 0)
        prepared.bound_parameters.fill(false);
}

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

ErrorOr<void> Database::transaction(Function<ErrorOr<void>()> callback)
{
    Transaction transaction { *this };
    TRY(transaction.begin());
    TRY(callback());
    TRY(transaction.commit());
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
    TRY(try_execute_statement(
        *m_table_exists_statement,
        [&](auto) -> ErrorOr<void> {
            exists = true;
            return {};
        },
        TRY(String::from_utf8(table))));
    return exists;
}

ErrorOr<Optional<u32>> Database::schema_version(StringView store)
{
    if (!TRY(table_exists("SchemaVersions"sv)))
        return OptionalNone {};

    if (!m_schema_version_statement.has_value())
        m_schema_version_statement = TRY(prepare_statement("SELECT version FROM SchemaVersions WHERE store = ?;"sv));

    Optional<u32> version;
    TRY(try_execute_statement(
        *m_schema_version_statement,
        [&](auto statement_id) -> ErrorOr<void> {
            version = result_column<u32>(statement_id, 0);
            return {};
        },
        TRY(String::from_utf8(store))));
    return version;
}

ErrorOr<Database::JournalMode> Database::set_journal_mode_pragma(JournalMode journal_mode)
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
    auto statement_id = TRY(prepare_statement(pragma));
    Optional<JournalMode> applied_journal_mode;
    TRY(try_execute_statement(statement_id, [&](auto result_statement_id) -> ErrorOr<void> {
        auto applied_mode = result_column<String>(result_statement_id, 0);
        if (applied_mode.equals_ignoring_ascii_case("delete"sv))
            applied_journal_mode = JournalMode::Delete;
        else if (applied_mode.equals_ignoring_ascii_case("truncate"sv))
            applied_journal_mode = JournalMode::Truncate;
        else if (applied_mode.equals_ignoring_ascii_case("persist"sv))
            applied_journal_mode = JournalMode::Persist;
        else if (applied_mode.equals_ignoring_ascii_case("memory"sv))
            applied_journal_mode = JournalMode::Memory;
        else if (applied_mode.equals_ignoring_ascii_case("wal"sv))
            applied_journal_mode = JournalMode::WriteAheadLog;
        else if (applied_mode.equals_ignoring_ascii_case("off"sv))
            applied_journal_mode = JournalMode::Off;
        else
            return Error::from_string_literal("PRAGMA journal_mode returned an unknown mode");
        return {};
    }));

    if (!applied_journal_mode.has_value())
        return Error::from_string_literal("PRAGMA journal_mode returned no mode");
    return *applied_journal_mode;
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

ErrorOr<void> Database::set_busy_timeout(i32 milliseconds)
{
    if (milliseconds < 0)
        return Error::from_string_literal("Database busy timeout must not be negative");
    SQL_TRY(sqlite3_busy_timeout(m_database, milliseconds));
    return {};
}

ErrorOr<void> Database::set_foreign_keys_pragma(ForeignKeys foreign_keys)
{
    TRY(execute_raw(foreign_keys == ForeignKeys::Yes ? "PRAGMA foreign_keys=ON;" : "PRAGMA foreign_keys=OFF;"));
    return {};
}

}
