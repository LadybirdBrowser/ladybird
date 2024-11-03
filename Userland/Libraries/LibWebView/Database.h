/*
 * Copyright (c) 2022-2024, Tim Flynn <trflynn89@serenityos.org>
 * Copyright (c) 2023, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/Function.h>
#include <AK/NonnullRefPtr.h>
#include <AK/RefCounted.h>
#include <AK/StringView.h>
#include <AK/Vector.h>

struct sqlite3;
struct sqlite3_stmt;

namespace WebView {

class Database : public RefCounted<Database> {
public:
    static ErrorOr<NonnullRefPtr<Database>> create();
    ~Database();

    using StatementID = size_t;
    using OnResult = Function<void(StatementID)>;

    ErrorOr<StatementID> prepare_statement(StringView statement);
    void execute_statement(StatementID, OnResult on_result);

    template<typename... PlaceholderValues>
    void execute_statement(StatementID statement_id, OnResult on_result, PlaceholderValues&&... placeholder_values)
    {
        int index = 1;
        (apply_placeholder(statement_id, index++, forward<PlaceholderValues>(placeholder_values)), ...);

        execute_statement(statement_id, move(on_result));
    }

    template<typename ValueType>
    ValueType result_column(StatementID, int column);

private:
    explicit Database(sqlite3*);

    template<typename ValueType>
    void apply_placeholder(StatementID statement_id, int index, ValueType const& value);

    ALWAYS_INLINE sqlite3_stmt* prepared_statement(StatementID statement_id)
    {
        VERIFY(statement_id < m_prepared_statements.size());
        return m_prepared_statements[statement_id];
    }

    sqlite3* m_database { nullptr };
    Vector<sqlite3_stmt*> m_prepared_statements;
};

}
