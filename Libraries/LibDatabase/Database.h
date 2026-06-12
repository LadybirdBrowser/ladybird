/*
 * Copyright (c) 2022-2026, Tim Flynn <trflynn89@ladybird.org>
 * Copyright (c) 2023, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/Function.h>
#include <AK/LexicalPath.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/RefCounted.h>
#include <AK/Span.h>
#include <AK/StringView.h>
#include <AK/Vector.h>
#include <LibDatabase/Forward.h>

struct sqlite3;
struct sqlite3_stmt;

namespace Database {

struct Migration {
    u32 version { 0 };

    // Executed first. May contain multiple statements.
    ByteString sql {};

    // Optional, runs after `sql`. Use this to bind placeholder values. Use
    // try_execute_statement to allow a failure to roll the migration transaction back.
    Function<ErrorOr<void>(Database&)> backfill {};
};

class DATABASE_API Database : public RefCounted<Database> {
public:
    static ErrorOr<NonnullRefPtr<Database>> create_memory_backed();
    static ErrorOr<NonnullRefPtr<Database>> create(ByteString const& directory, StringView name);
    ~Database();

    using OnResult = Function<void(StatementID)>;

    Optional<LexicalPath> const& database_path() const { return m_database_path; }

    ErrorOr<StatementID> prepare_statement(StringView statement);

    void execute_statement(StatementID statement_id, OnResult on_result)
    {
        VERIFY(bound_parameter_count(statement_id) == 0);
        execute_statement_internal(statement_id, move(on_result));
    }

    template<typename... PlaceholderValues>
    void execute_statement(StatementID statement_id, OnResult on_result, PlaceholderValues&&... placeholder_values)
    {
        int index = 1;
        (apply_placeholder(statement_id, index++, forward<PlaceholderValues>(placeholder_values)), ...);

        VERIFY(bound_parameter_count(statement_id) == index - 1);
        execute_statement_internal(statement_id, move(on_result));
    }

    template<typename ValueType>
    ValueType result_column(StatementID, int column);

    ErrorOr<void> execute_raw(ByteString const& sql);

    // Error-returning sibling of execute_statement, for callers that must handle failures
    // (e.g. migration backfills, which roll the migration transaction back) instead of
    // aborting the process.
    template<typename... PlaceholderValues>
    ErrorOr<void> try_execute_statement(StatementID statement_id, OnResult on_result, PlaceholderValues&&... placeholder_values)
    {
        if constexpr (sizeof...(PlaceholderValues) > 0) {
            int index = 1;
            Optional<Error> bind_error;

            auto bind = [&](auto const& value) {
                if (bind_error.has_value())
                    return;
                if (auto result = try_apply_placeholder(statement_id, index++, value); result.is_error())
                    bind_error = result.release_error();
            };
            (bind(forward<PlaceholderValues>(placeholder_values)), ...);

            if (bind_error.has_value())
                return bind_error.release_value();
        }

        VERIFY(bound_parameter_count(statement_id) == sizeof...(PlaceholderValues));
        return try_execute_statement_internal(statement_id, move(on_result));
    }

    // Brings the named store's schema to the latest version by replaying, in order, every
    // migration newer than the version recorded for it in the SchemaVersions table. Shipped
    // migration text is immutable; schema changes append a new version. Baseline (first)
    // migrations use CREATE ... IF NOT EXISTS so pre-versioning databases adopt them as a
    // no-op; later migrations must not.
    ErrorOr<MigrationOutcome> migrate(StringView store_name, ReadonlySpan<Migration> migrations, MigrationMode = MigrationMode::Apply);

    ErrorOr<bool> table_exists(StringView table);

    // The version recorded for the store in SchemaVersions, or empty if it has none yet.
    ErrorOr<Optional<u32>> schema_version(StringView store);

    // https://www.sqlite.org/pragma.html#pragma_journal_mode
    enum class JournalMode {
        Delete,
        Truncate,
        Persist,
        Memory,
        WriteAheadLog,
        Off,
    };
    ErrorOr<void> set_journal_mode_pragma(JournalMode);

    // https://www.sqlite.org/pragma.html#pragma_synchronous
    enum class Synchronous {
        Off,
        Normal,
        Full,
        Extra,
    };
    ErrorOr<void> set_synchronous_pragma(Synchronous);

private:
    static ErrorOr<NonnullRefPtr<Database>> create(sqlite3*, Optional<LexicalPath> database_path = {});
    Database(sqlite3*, Optional<LexicalPath> database_path);

    void execute_statement_internal(StatementID, OnResult);
    ErrorOr<void> try_execute_statement_internal(StatementID, OnResult);

    int bound_parameter_count(StatementID);

    template<typename ValueType>
    void apply_placeholder(StatementID statement_id, int index, ValueType const& value);

    template<typename ValueType>
    ErrorOr<void> try_apply_placeholder(StatementID statement_id, int index, ValueType const& value);

    class Transaction {
    public:
        explicit Transaction(Database& database)
            : m_database(database)
        {
        }
        ~Transaction();

        Transaction(Transaction&&) = delete;
        Transaction(Transaction const&) = delete;
        Transaction& operator=(Transaction&&) = delete;
        Transaction& operator=(Transaction const&) = delete;

        ErrorOr<void> begin();
        ErrorOr<void> commit();

    private:
        Database& m_database;
        bool m_active { false };
    };

    ALWAYS_INLINE sqlite3_stmt* prepared_statement(StatementID statement_id)
    {
        VERIFY(statement_id < m_prepared_statements.size());
        return m_prepared_statements[statement_id];
    }

    Optional<LexicalPath> m_database_path;
    sqlite3* m_database { nullptr };
    Vector<sqlite3_stmt*> m_prepared_statements;
    Optional<StatementID> m_table_exists_statement;
    Optional<StatementID> m_schema_version_statement;
};

}
