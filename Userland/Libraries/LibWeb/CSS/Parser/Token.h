/*
 * Copyright (c) 2020-2021, the SerenityOS developers.
 * Copyright (c) 2021-2023, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <LibWeb/CSS/Number.h>

namespace Web::CSS::Parser {

class Token {
    friend class Tokenizer;

public:
    enum class Type {
        Invalid,
        EndOfFile,
        Ident,
        Function,
        AtKeyword,
        Hash,
        String,
        BadString,
        Url,
        BadUrl,
        Delim,
        Number,
        Percentage,
        Dimension,
        Whitespace,
        CDO,
        CDC,
        Colon,
        Semicolon,
        Comma,
        OpenSquare,
        CloseSquare,
        OpenParen,
        CloseParen,
        OpenCurly,
        CloseCurly
    };

    enum class HashType {
        Id,
        Unrestricted,
    };

    struct Position {
        size_t line { 0 };
        size_t column { 0 };
    };

    Type type() const { return m_type; }
    bool is(Type type) const { return m_type == type; }

    FlyString const& ident() const
    {
        VERIFY(m_type == Type::Ident);
        return m_value;
    }

    FlyString const& function() const
    {
        VERIFY(m_type == Type::Function);
        return m_value;
    }

    u32 delim() const
    {
        VERIFY(m_type == Type::Delim);
        return *m_value.code_points().begin();
    }

    FlyString const& string() const
    {
        VERIFY(m_type == Type::String);
        return m_value;
    }

    FlyString const& url() const
    {
        VERIFY(m_type == Type::Url);
        return m_value;
    }

    FlyString const& at_keyword() const
    {
        VERIFY(m_type == Type::AtKeyword);
        return m_value;
    }

    HashType hash_type() const
    {
        VERIFY(m_type == Type::Hash);
        return m_hash_type;
    }
    FlyString const& hash_value() const
    {
        VERIFY(m_type == Type::Hash);
        return m_value;
    }

    Number const& number() const
    {
        VERIFY(m_type == Type::Number || m_type == Type::Dimension || m_type == Type::Percentage);
        return m_number_value;
    }
    double number_value() const
    {
        VERIFY(m_type == Type::Number);
        return m_number_value.value();
    }
    i64 to_integer() const
    {
        VERIFY(m_type == Type::Number && m_number_value.is_integer());
        return m_number_value.integer_value();
    }

    FlyString const& dimension_unit() const
    {
        VERIFY(m_type == Type::Dimension);
        return m_value;
    }
    double dimension_value() const
    {
        VERIFY(m_type == Type::Dimension);
        return m_number_value.value();
    }
    i64 dimension_value_int() const { return m_number_value.integer_value(); }

    double percentage() const
    {
        VERIFY(m_type == Type::Percentage);
        return m_number_value.value();
    }

    Type mirror_variant() const;
    StringView bracket_string() const;
    StringView bracket_mirror_string() const;

    String to_string() const;
    String to_debug_string() const;

    String const& original_source_text() const { return m_original_source_text; }
    Position const& start_position() const { return m_start_position; }
    Position const& end_position() const { return m_end_position; }

    static Token create_string(FlyString str)
    {
        Token token;
        token.m_type = Type::String;
        token.m_value = move(str);
        return token;
    }

    static Token create_number(double value, Number::Type number_type)
    {
        Token token;
        token.m_type = Type::Number;
        token.m_number_value = Number(number_type, value);
        return token;
    }

    static Token create_percentage(double value)
    {
        Token token;
        token.m_type = Type::Percentage;
        token.m_number_value = Number(Number::Type::Number, value);
        return token;
    }

    static Token create_dimension(double value, FlyString unit)
    {
        Token token;
        token.m_type = Type::Dimension;
        token.m_number_value = Number(Number::Type::Number, value);
        token.m_value = move(unit);
        return token;
    }

    static Token create_ident(FlyString ident)
    {
        Token token;
        token.m_type = Type::Ident;
        token.m_value = move(ident);
        return token;
    }

    static Token create_url(FlyString url)
    {
        Token token;
        token.m_type = Type::Url;
        token.m_value = move(url);
        return token;
    }

private:
    Type m_type { Type::Invalid };

    FlyString m_value;
    Number m_number_value;
    HashType m_hash_type { HashType::Unrestricted };

    String m_original_source_text;
    Position m_start_position;
    Position m_end_position;
};

}
