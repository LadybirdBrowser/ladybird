/*
 * Copyright (c) 2020-2021, the SerenityOS developers.
 * Copyright (c) 2021-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <AK/Variant.h>
#include <LibWeb/CSS/Number.h>
#include <LibWeb/CSS/Parser/SourcePosition.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>

namespace Web::CSS::Parser {

inline static double clamp_to_single_precision(double value)
{
    if (value > static_cast<double>(NumericLimits<float>::max()))
        return static_cast<double>(NumericLimits<float>::max());

    if (value < static_cast<double>(NumericLimits<float>::lowest()))
        return static_cast<double>(NumericLimits<float>::lowest());

    return value;
}

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
        return string_value();
    }

    FlyString const& function() const
    {
        VERIFY(m_type == Type::Function);
        return string_value();
    }

    u32 delim() const
    {
        VERIFY(m_type == Type::Delim);
        return m_value.get<u32>();
    }

    FlyString const& string() const
    {
        VERIFY(m_type == Type::String);
        return string_value();
    }

    FlyString const& url() const
    {
        VERIFY(m_type == Type::Url);
        return string_value();
    }

    FlyString const& at_keyword() const
    {
        VERIFY(m_type == Type::AtKeyword);
        return string_value();
    }

    HashType hash_type() const
    {
        VERIFY(m_type == Type::Hash);
        return m_value.get<HashValue>().type;
    }
    FlyString const& hash_value() const
    {
        VERIFY(m_type == Type::Hash);
        return m_value.get<HashValue>().value;
    }

    bool is_integer() const
    {
        VERIFY(m_type == Type::Number || m_type == Type::Dimension || m_type == Type::Percentage);
        return number_value_for_type().is_integer();
    }

    bool is_integer_with_explicit_sign() const
    {
        VERIFY(m_type == Type::Number || m_type == Type::Dimension || m_type == Type::Percentage);
        return number_value_for_type().is_integer_with_explicit_sign();
    }

    double number_value() const
    {
        VERIFY(m_type == Type::Number);
        return clamp_to_single_precision(m_value.get<Number>().value());
    }
    i32 to_integer() const
    {
        VERIFY(m_type == Type::Number && m_value.get<Number>().is_integer());
        return m_value.get<Number>().integer_value();
    }

    FlyString const& dimension_unit() const
    {
        VERIFY(m_type == Type::Dimension);
        return m_value.get<DimensionValue>().unit;
    }
    double dimension_value() const
    {
        VERIFY(m_type == Type::Dimension);
        return clamp_to_single_precision(m_value.get<DimensionValue>().number.value());
    }
    i32 dimension_value_int() const { return m_value.get<DimensionValue>().number.integer_value(); }

    double percentage() const
    {
        VERIFY(m_type == Type::Percentage);
        return clamp_to_single_precision(m_value.get<Number>().value());
    }

    Type mirror_variant() const;
    StringView bracket_string() const;
    StringView bracket_mirror_string() const;

    String to_string() const;
    String to_debug_string() const;

    String const& original_source_text() const { return m_original_source_text; }
    SourcePosition const& start_position() const { return m_start_position; }
    SourcePosition const& end_position() const { return m_end_position; }
    void set_position_range(Badge<Tokenizer, RustTokenizer>, SourcePosition start, SourcePosition end);

    bool operator==(Token const& other) const
    {
        return m_type == other.m_type && m_value == other.m_value;
    }

private:
    struct HashValue {
        FlyString value;
        HashType type { HashType::Unrestricted };

        bool operator==(HashValue const&) const = default;
    };

    struct DimensionValue {
        Number number;
        FlyString unit;

        bool operator==(DimensionValue const&) const = default;
    };

    FlyString const& string_value() const;
    Number const& number_value_for_type() const;

    Type m_type { Type::Invalid };

    Variant<Empty, FlyString, u32, Number, HashValue, DimensionValue> m_value;

    String m_original_source_text;
    SourcePosition m_start_position;
    SourcePosition m_end_position;
};

static_assert(sizeof(Token) <= 64, "Keep the size of CSS parser tokens down!");

}

template<>
struct AK::Formatter<Web::CSS::Parser::Token> : Formatter<StringView> {
    ErrorOr<void> format(FormatBuilder& builder, Web::CSS::Parser::Token const& token)
    {
        return Formatter<StringView>::format(builder, token.to_string());
    }
};
