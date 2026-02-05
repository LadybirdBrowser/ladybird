/*
 * Copyright (c) 2020-2021, the SerenityOS developers.
 * Copyright (c) 2021-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <LibWeb/CSS/Number.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>

namespace Web::CSS::Parser {

class WEB_API Token {
public:
    enum class Type : u8 {
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

    enum class HashType : u8 {
        Id,
        Unrestricted,
    };

    struct Position {
        size_t line { 0 };
        size_t column { 0 };
    };

    // Use this only to create types that don't have their own create_foo() methods below.
    static Token create(Type, String original_source_text = {});

    static Token create_ident(FlyString ident, String original_source_text = {});
    static Token create_function(FlyString name, String original_source_text = {});
    static Token create_at_keyword(FlyString name, String original_source_text = {});
    static Token create_hash(FlyString value, HashType hash_type, String original_source_text = {});
    static Token create_string(FlyString value, String original_source_text = {});
    static Token create_url(FlyString url, String original_source_text = {});
    static Token create_delim(u32 delim, String original_source_text = {});
    static Token create_number(Number value, String original_source_text = {});
    static Token create_percentage(Number value, String original_source_text = {});
    static Token create_dimension(Number value, FlyString unit, String original_source_text = {});
    static Token create_dimension(double value, FlyString unit, String original_source_text = {})
    {
        return create_dimension(Number { Number::Type::Number, value }, move(unit), move(original_source_text));
    }
    static Token create_whitespace(String original_source_text = {});

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
    void set_position_range(Badge<Tokenizer>, Position start, Position end);

    bool operator==(Token const& other) const
    {
        return m_type == other.m_type && m_value == other.m_value && m_number_value == other.m_number_value && m_hash_type == other.m_hash_type;
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

template<>
struct AK::Formatter<Web::CSS::Parser::Token> : Formatter<StringView> {
    ErrorOr<void> format(FormatBuilder& builder, Web::CSS::Parser::Token const& token)
    {
        return Formatter<StringView>::format(builder, token.to_string());
    }
};
