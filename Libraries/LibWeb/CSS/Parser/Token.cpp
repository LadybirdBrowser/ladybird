/*
 * Copyright (c) 2020-2021, the SerenityOS developers.
 * Copyright (c) 2022-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/GenericShorthands.h>
#include <LibWeb/CSS/Parser/Token.h>
#include <LibWeb/CSS/Serialize.h>

namespace Web::CSS::Parser {

Token Token::create(Type type, String original_source_text)
{
    VERIFY(first_is_one_of(type,
        Type::Invalid,
        Type::EndOfFile,
        Type::BadString,
        Type::BadUrl,
        Type::CDO,
        Type::CDC,
        Type::Colon,
        Type::Semicolon,
        Type::Comma,
        Type::OpenSquare,
        Type::CloseSquare,
        Type::OpenParen,
        Type::CloseParen,
        Type::OpenCurly,
        Type::CloseCurly));

    Token token;
    token.m_type = type;
    token.m_original_source_text = move(original_source_text);
    return token;
}

Token Token::create_ident(Utf16FlyString ident, String original_source_text)
{
    Token token;
    token.m_type = Type::Ident;
    token.m_value = move(ident);
    token.m_original_source_text = move(original_source_text);
    return token;
}

Token Token::create_function(Utf16FlyString name, String original_source_text)
{
    Token token;
    token.m_type = Type::Function;
    token.m_value = move(name);
    token.m_original_source_text = move(original_source_text);
    return token;
}

Token Token::create_at_keyword(Utf16FlyString name, String original_source_text)
{
    Token token;
    token.m_type = Type::AtKeyword;
    token.m_value = move(name);
    token.m_original_source_text = move(original_source_text);
    return token;
}

Token Token::create_hash(Utf16FlyString value, HashType hash_type, String original_source_text)
{
    Token token;
    token.m_type = Type::Hash;
    token.m_value = HashValue { move(value), hash_type };
    token.m_original_source_text = move(original_source_text);
    return token;
}

Token Token::create_string(Utf16FlyString value, String original_source_text)
{
    Token token;
    token.m_type = Type::String;
    token.m_value = move(value);
    token.m_original_source_text = move(original_source_text);
    return token;
}

Token Token::create_url(Utf16FlyString url, String original_source_text)
{
    Token token;
    token.m_type = Type::Url;
    token.m_value = move(url);
    token.m_original_source_text = move(original_source_text);
    return token;
}

Token Token::create_delim(u32 delim, String original_source_text)
{
    Token token;
    token.m_type = Type::Delim;
    token.m_value = delim;
    token.m_original_source_text = move(original_source_text);
    return token;
}

Token Token::create_number(Number value, String original_source_text)
{
    Token token;
    token.m_type = Type::Number;
    token.m_value = value;
    token.m_original_source_text = move(original_source_text);
    return token;
}

Token Token::create_percentage(Number value, String original_source_text)
{
    Token token;
    token.m_type = Type::Percentage;
    token.m_value = value;
    token.m_original_source_text = move(original_source_text);
    return token;
}

Token Token::create_dimension(Number value, Utf16FlyString unit, String original_source_text)
{
    Token token;
    token.m_type = Type::Dimension;
    token.m_value = DimensionValue { value, move(unit) };
    token.m_original_source_text = move(original_source_text);
    return token;
}

Token Token::create_whitespace(String original_source_text)
{
    Token token;
    token.m_type = Type::Whitespace;
    token.m_original_source_text = move(original_source_text);
    return token;
}

void Token::serialize_to(Utf16StringBuilder& builder) const
{
    switch (m_type) {
    case Type::EndOfFile:
        return;
    case Type::Ident:
        serialize_an_identifier(builder, ident());
        return;
    case Type::Function: {
        serialize_an_identifier(builder, function());
        builder.append_ascii('(');
        return;
    }
    case Type::AtKeyword:
        builder.append_ascii('@');
        serialize_an_identifier(builder, at_keyword());
        return;
    case Type::Hash:
        builder.append_ascii('#');
        switch (hash_type()) {
        case HashType::Id:
            serialize_an_identifier(builder, hash_value());
            return;
        case HashType::Unrestricted:
            builder.append(hash_value());
            return;
        }
        VERIFY_NOT_REACHED();
    case Type::String:
        serialize_a_string(builder, string());
        return;
    case Type::BadString:
        return;
    case Type::Url:
        builder.append_ascii("url("sv);
        serialize_a_string(builder, url());
        builder.append_ascii(')');
        return;
    case Type::BadUrl:
        builder.append_ascii("url()"sv);
        return;
    case Type::Delim:
        builder.append_code_point(delim());
        return;
    case Type::Number:
        builder.appendff("{}", m_value.get<Number>().value());
        return;
    case Type::Percentage:
        builder.appendff("{}%", m_value.get<Number>().value());
        return;
    case Type::Dimension:
        builder.appendff("{}", m_value.get<DimensionValue>().number.value());
        builder.append(dimension_unit());
        return;
    case Type::Whitespace:
        builder.append_ascii(' ');
        return;
    case Type::CDO:
        builder.append_ascii("<!--"sv);
        return;
    case Type::CDC:
        builder.append_ascii("-->"sv);
        return;
    case Type::Colon:
        builder.append_ascii(':');
        return;
    case Type::Semicolon:
        builder.append_ascii(';');
        return;
    case Type::Comma:
        builder.append_ascii(',');
        return;
    case Type::OpenSquare:
        builder.append_ascii('[');
        return;
    case Type::CloseSquare:
        builder.append_ascii(']');
        return;
    case Type::OpenParen:
        builder.append_ascii('(');
        return;
    case Type::CloseParen:
        builder.append_ascii(')');
        return;
    case Type::OpenCurly:
        builder.append_ascii('{');
        return;
    case Type::CloseCurly:
        builder.append_ascii('}');
        return;
    case Type::Invalid:
    default:
        VERIFY_NOT_REACHED();
    }
}

Utf16String Token::to_string() const
{
    Utf16StringBuilder builder;
    serialize_to(builder);
    return builder.to_string();
}

String Token::to_debug_string() const
{
    auto append_quoted_string = [](StringBuilder& builder, StringView string) {
        builder.append('"');
        builder.append_escaped_for_json(string);
        builder.append('"');
    };

    auto hash_type_name = [](HashType hash_type) -> StringView {
        switch (hash_type) {
        case HashType::Id:
            return "Id"sv;
        case HashType::Unrestricted:
            return "Unrestricted"sv;
        }
        VERIFY_NOT_REACHED();
    };

    auto number_type_name = [](Number::Type number_type) -> StringView {
        switch (number_type) {
        case Number::Type::Number:
            return "Number"sv;
        case Number::Type::IntegerWithExplicitSign:
            return "IntegerWithExplicitSign"sv;
        case Number::Type::Integer:
            return "Integer"sv;
        }
        VERIFY_NOT_REACHED();
    };

    StringBuilder builder;
    bool has_type_specific_fields = false;
    switch (m_type) {
    case Type::Invalid:
        VERIFY_NOT_REACHED();

    case Type::EndOfFile:
        builder.append("__EOF__("sv);
        break;
    case Type::Ident:
        builder.append("Ident(value="sv);
        append_quoted_string(builder, MUST(ident().view().to_utf8()));
        has_type_specific_fields = true;
        break;
    case Type::Function:
        builder.append("Function(value="sv);
        append_quoted_string(builder, MUST(function().view().to_utf8()));
        has_type_specific_fields = true;
        break;
    case Type::AtKeyword:
        builder.append("AtKeyword(value="sv);
        append_quoted_string(builder, MUST(at_keyword().view().to_utf8()));
        has_type_specific_fields = true;
        break;
    case Type::Hash:
        builder.append("Hash(value="sv);
        append_quoted_string(builder, MUST(hash_value().view().to_utf8()));
        builder.appendff(", hash_type={}", hash_type_name(hash_type()));
        has_type_specific_fields = true;
        break;
    case Type::String:
        builder.append("String(value="sv);
        append_quoted_string(builder, MUST(string().view().to_utf8()));
        has_type_specific_fields = true;
        break;
    case Type::BadString:
        builder.append("BadString("sv);
        break;
    case Type::Url:
        builder.append("Url(value="sv);
        append_quoted_string(builder, MUST(url().view().to_utf8()));
        has_type_specific_fields = true;
        break;
    case Type::BadUrl:
        builder.append("BadUrl("sv);
        break;
    case Type::Delim:
        builder.append("Delim(value="sv);
        append_quoted_string(builder, String::from_code_point(delim()));
        builder.appendff(", code_point=U+{:04X}", delim());
        has_type_specific_fields = true;
        break;
    case Type::Number:
        builder.appendff("Number(value={}, number_type={}", number_value(), number_type_name(m_value.get<Number>().type()));
        has_type_specific_fields = true;
        break;
    case Type::Percentage:
        builder.appendff("Percentage(value={}, number_type={}", percentage(), number_type_name(m_value.get<Number>().type()));
        has_type_specific_fields = true;
        break;
    case Type::Dimension:
        builder.appendff("Dimension(value={}, number_type={}, unit=", dimension_value(), number_type_name(m_value.get<DimensionValue>().number.type()));
        append_quoted_string(builder, MUST(dimension_unit().view().to_utf8()));
        has_type_specific_fields = true;
        break;
    case Type::Whitespace:
        builder.append("Whitespace("sv);
        break;
    case Type::CDO:
        builder.append("CDO("sv);
        break;
    case Type::CDC:
        builder.append("CDC("sv);
        break;
    case Type::Colon:
        builder.append("Colon("sv);
        break;
    case Type::Semicolon:
        builder.append("Semicolon("sv);
        break;
    case Type::Comma:
        builder.append("Comma("sv);
        break;
    case Type::OpenSquare:
        builder.append("OpenSquare("sv);
        break;
    case Type::CloseSquare:
        builder.append("CloseSquare("sv);
        break;
    case Type::OpenParen:
        builder.append("OpenParen("sv);
        break;
    case Type::CloseParen:
        builder.append("CloseParen("sv);
        break;
    case Type::OpenCurly:
        builder.append("OpenCurly("sv);
        break;
    case Type::CloseCurly:
        builder.append("CloseCurly("sv);
        break;
    }

    builder.append(has_type_specific_fields ? ", source="sv : "source="sv);
    append_quoted_string(builder, m_original_source_text.bytes_as_string_view());
    builder.appendff(", start={}:{}, end={}:{})", m_start_position.line, m_start_position.column, m_end_position.line, m_end_position.column);
    return builder.to_string_without_validation();
}

Token::Type Token::mirror_variant() const
{
    if (is(Token::Type::OpenCurly)) {
        return Type::CloseCurly;
    }

    if (is(Token::Type::OpenSquare)) {
        return Type::CloseSquare;
    }

    if (is(Token::Type::OpenParen)) {
        return Type::CloseParen;
    }

    return Type::Invalid;
}

StringView Token::bracket_string() const
{
    if (is(Token::Type::OpenCurly)) {
        return "{"sv;
    }

    if (is(Token::Type::CloseCurly)) {
        return "}"sv;
    }

    if (is(Token::Type::OpenSquare)) {
        return "["sv;
    }

    if (is(Token::Type::CloseSquare)) {
        return "]"sv;
    }

    if (is(Token::Type::OpenParen)) {
        return "("sv;
    }

    if (is(Token::Type::CloseParen)) {
        return ")"sv;
    }

    return ""sv;
}

StringView Token::bracket_mirror_string() const
{
    if (is(Token::Type::OpenCurly)) {
        return "}"sv;
    }

    if (is(Token::Type::CloseCurly)) {
        return "{"sv;
    }

    if (is(Token::Type::OpenSquare)) {
        return "]"sv;
    }

    if (is(Token::Type::CloseSquare)) {
        return "["sv;
    }

    if (is(Token::Type::OpenParen)) {
        return ")"sv;
    }

    if (is(Token::Type::CloseParen)) {
        return "("sv;
    }

    return ""sv;
}

void Token::set_position_range(Badge<Tokenizer, RustTokenizer>, SourcePosition start, SourcePosition end)
{
    m_start_position = start;
    m_end_position = end;
}

Utf16FlyString const& Token::string_value() const
{
    return m_value.get<Utf16FlyString>();
}

Number const& Token::number_value_for_type() const
{
    if (m_type == Type::Dimension)
        return m_value.get<DimensionValue>().number;
    return m_value.get<Number>();
}

}
