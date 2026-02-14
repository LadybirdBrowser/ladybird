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
#include <AK/RefCounted.h>
#include <AK/StringView.h>
#include <AK/Vector.h>
#include <LibDatabase/Forward.h>

struct sqlite3;
struct sqlite3_stmt;

namespace Database {

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

    int bound_parameter_count(StatementID);

    template<typename ValueType>
    void apply_placeholder(StatementID statement_id, int index, ValueType const& value);

    ALWAYS_INLINE sqlite3_stmt* prepared_statement(StatementID statement_id)
    {
        VERIFY(statement_id < m_prepared_statements.size());
        return m_prepared_statements[statement_id];
    }

    Optional<LexicalPath> m_database_path;
    sqlite3* m_database { nullptr };
    Vector<sqlite3_stmt*> m_prepared_statements;
};

}
