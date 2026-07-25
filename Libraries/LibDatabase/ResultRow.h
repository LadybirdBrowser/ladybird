/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Badge.h>
#include <AK/ByteBuffer.h>
#include <AK/ByteString.h>
#include <AK/Checked.h>
#include <AK/Concepts.h>
#include <AK/Error.h>
#include <AK/Noncopyable.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/StringView.h>
#include <AK/Utf16String.h>
#include <LibDatabase/Database.h>

namespace Database {

// Callback-scoped access to named columns through checked, bounded reads.
class DATABASE_API ResultRow {
    AK_MAKE_NONCOPYABLE(ResultRow);
    AK_MAKE_NONMOVABLE(ResultRow);

public:
    template<Integral T>
    requires(!IsSame<T, bool>)
    ErrorOr<T> read_integer(StringView column)
    {
        auto index = TRY(m_database.result_column_index(m_statement_id, column));
        auto value = m_database.result_i64_checked(m_statement_id, index);
        if (value.is_error())
            return Error::from_string_literal("Column is not an integer");
        if (!AK::is_within_range<T>(value.value()))
            return Error::from_string_literal("Column value is out of range");
        return static_cast<T>(value.value());
    }

    template<Integral T>
    requires(!IsSame<T, bool>)
    ErrorOr<Optional<T>> read_optional_integer(StringView column)
    {
        if (TRY(is_null(column)))
            return Optional<T> {};
        return Optional<T> { TRY(read_integer<T>(column)) };
    }

    ErrorOr<bool> is_null(StringView column)
    {
        auto index = TRY(m_database.result_column_index(m_statement_id, column));
        return m_database.result_column_is_null({}, m_statement_id, index);
    }

    ErrorOr<bool> read_bool(StringView column)
    {
        auto value = TRY(read_integer<i64>(column));
        if (value != 0 && value != 1)
            return Error::from_string_literal("Column is not a boolean");
        return value == 1;
    }

    ErrorOr<String> read_text(StringView column, size_t max_bytes)
    {
        auto index = TRY(m_database.result_column_index(m_statement_id, column));
        auto value = m_database.result_text_column_bounded({}, m_statement_id, index, max_bytes);
        if (value.is_error()) {
            if (value.error() == Database::ColumnReadError::WrongType)
                return Error::from_string_literal("Column is not text");
            return Error::from_string_literal("Text column exceeds the size limit");
        }
        return String::from_utf8(value.release_value());
    }

    ErrorOr<Optional<String>> read_optional_text(StringView column, size_t max_bytes)
    {
        if (TRY(is_null(column)))
            return Optional<String> {};
        return Optional<String> { TRY(read_text(column, max_bytes)) };
    }

    ErrorOr<Utf16String> read_utf16_text(StringView column, size_t max_bytes)
    {
        auto index = TRY(m_database.result_column_index(m_statement_id, column));
        auto value = m_database.result_text_column_bounded({}, m_statement_id, index, max_bytes);
        if (value.is_error()) {
            if (value.error() == Database::ColumnReadError::WrongType)
                return Error::from_string_literal("Column is not text");
            return Error::from_string_literal("Text column exceeds the size limit");
        }
        return Utf16String::try_from_utf8(value.release_value());
    }

    ErrorOr<Optional<Utf16String>> read_optional_utf16_text(StringView column, size_t max_bytes)
    {
        if (TRY(is_null(column)))
            return Optional<Utf16String> {};
        return Optional<Utf16String> { TRY(read_utf16_text(column, max_bytes)) };
    }

    ErrorOr<ByteBuffer> read_blob(StringView column, size_t max_bytes)
    {
        auto index = TRY(m_database.result_column_index(m_statement_id, column));
        auto value = m_database.result_blob_column_bounded({}, m_statement_id, index, max_bytes);
        if (value.is_error()) {
            if (value.error() == Database::ColumnReadError::WrongType)
                return Error::from_string_literal("Column is not a blob");
            return Error::from_string_literal("Blob column exceeds the size limit");
        }
        return ByteBuffer::copy(value.release_value());
    }

    ErrorOr<Optional<ByteBuffer>> read_optional_blob(StringView column, size_t max_bytes)
    {
        if (TRY(is_null(column)))
            return Optional<ByteBuffer> {};
        return Optional<ByteBuffer> { TRY(read_blob(column, max_bytes)) };
    }

    ResultRow(Badge<Database>, Database& database, StatementID statement_id)
        : m_database(database)
        , m_statement_id(statement_id)
    {
    }

private:
    Database& m_database;
    StatementID m_statement_id;
};

}
