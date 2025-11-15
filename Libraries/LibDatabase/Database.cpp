/*
 * Copyright (c) 2022-2025, Tim Flynn <trflynn89@ladybird.org>
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

ErrorOr<NonnullRefPtr<Database>> Database::create(ByteString const& directory, StringView name)
{
    TRY(Core::Directory::create(directory, Core::Directory::CreateDirectories::Yes));
    auto database_file = ByteString::formatted("{}/{}.db", directory, name);

    sqlite3* m_database { nullptr };
    SQL_TRY(sqlite3_open(database_file.characters(), &m_database));

    return adopt_nonnull_ref_or_enomem(new (nothrow) Database(m_database));
}

Database::Database(sqlite3* database)
    : m_database(database)
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

void Database::execute_statement(StatementID statement_id, OnResult on_result)
{
    auto* statement = prepared_statement(statement_id);

    while (true) {
        auto result = sqlite3_step(statement);

        switch (result) {
        case SQLITE_DONE:
            SQL_MUST(sqlite3_reset(statement));
            return;

        case SQLITE_ROW:
            if (on_result)
                on_result(statement_id);
            continue;

        default:
            SQL_MUST(result);
            return;
        }
    }
}

template<typename ValueType>
void Database::apply_placeholder(StatementID statement_id, int index, ValueType const& value)
{
    auto* statement = prepared_statement(statement_id);

    if constexpr (IsSame<ValueType, String>) {
        StringView string { value };
        SQL_MUST(sqlite3_bind_text(statement, index, string.characters_without_null_termination(), static_cast<int>(string.length()), SQLITE_TRANSIENT));
    } else if constexpr (IsSame<ValueType, ByteString>) {
        SQL_MUST(sqlite3_bind_blob(statement, index, value.characters(), static_cast<int>(value.length()), SQLITE_TRANSIENT));
    } else if constexpr (IsSame<ValueType, UnixDateTime>) {
        apply_placeholder(statement_id, index, value.offset_to_epoch().to_milliseconds());
    } else if constexpr (IsIntegral<ValueType>) {
        if constexpr (sizeof(ValueType) <= sizeof(int))
            SQL_MUST(sqlite3_bind_int(statement, index, static_cast<int>(value)));
        else
            SQL_MUST(sqlite3_bind_int64(statement, index, static_cast<sqlite3_int64>(value)));
    } else {
        static_assert(DependentFalse<ValueType>);
    }
}

#define __ENUMERATE_TYPE(type) \
    template DATABASE_API void Database::apply_placeholder(StatementID, int, type const&);
ENUMERATE_SQL_TYPES
#undef __ENUMERATE_TYPE

template<typename ValueType>
ValueType Database::result_column(StatementID statement_id, int column)
{
    auto* statement = prepared_statement(statement_id);

    if constexpr (IsSame<ValueType, String>) {
        auto const* text = reinterpret_cast<char const*>(sqlite3_column_text(statement, column));
        return MUST(String::from_utf8(StringView { text, strlen(text) }));
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

}
