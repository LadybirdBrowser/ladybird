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

Token Token::create_ident(FlyString ident, String original_source_text)
{
    Token token;
    token.m_type = Type::Ident;
    token.m_value = move(ident);
    token.m_original_source_text = move(original_source_text);
    return token;
}

Token Token::create_function(FlyString name, String original_source_text)
{
    Token token;
    token.m_type = Type::Function;
    token.m_value = move(name);
    token.m_original_source_text = move(original_source_text);
    return token;
}

Token Token::create_at_keyword(FlyString name, String original_source_text)
{
    Token token;
    token.m_type = Type::AtKeyword;
    token.m_value = move(name);
    token.m_original_source_text = move(original_source_text);
    return token;
}

Token Token::create_hash(FlyString value, HashType hash_type, String original_source_text)
{
    Token token;
    token.m_type = Type::Hash;
    token.m_value = move(value);
    token.m_hash_type = hash_type;
    token.m_original_source_text = move(original_source_text);
    return token;
}

Token Token::create_string(FlyString value, String original_source_text)
{
    Token token;
    token.m_type = Type::String;
    token.m_value = move(value);
    token.m_original_source_text = move(original_source_text);
    return token;
}

Token Token::create_url(FlyString url, String original_source_text)
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
    token.m_value = String::from_code_point(delim);
    token.m_original_source_text = move(original_source_text);
    return token;
}

Token Token::create_number(Number value, String original_source_text)
{
    Token token;
    token.m_type = Type::Number;
    token.m_number_value = value;
    token.m_original_source_text = move(original_source_text);
    return token;
}

Token Token::create_percentage(Number value, String original_source_text)
{
    Token token;
    token.m_type = Type::Percentage;
    token.m_number_value = value;
    token.m_original_source_text = move(original_source_text);
    return token;
}

Token Token::create_dimension(Number value, FlyString unit, String original_source_text)
{
    Token token;
    token.m_type = Type::Dimension;
    token.m_number_value = value;
    token.m_value = move(unit);
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

String Token::to_string() const
{
    StringBuilder builder;

    switch (m_type) {
    case Type::EndOfFile:
        return String {};
    case Type::Ident:
        return serialize_an_identifier(ident());
    case Type::Function:
        return MUST(String::formatted("{}(", serialize_an_identifier(function())));
    case Type::AtKeyword:
        return MUST(String::formatted("@{}", serialize_an_identifier(at_keyword())));
    case Type::Hash: {
        switch (m_hash_type) {
        case HashType::Id:
            return MUST(String::formatted("#{}", serialize_an_identifier(hash_value())));
        case HashType::Unrestricted:
            return MUST(String::formatted("#{}", hash_value()));
        }
        VERIFY_NOT_REACHED();
    }
    case Type::String:
        return serialize_a_string(string());
    case Type::BadString:
        return String {};
    case Type::Url:
        return serialize_a_url(url());
    case Type::BadUrl:
        return "url()"_string;
    case Type::Delim:
        return String { m_value };
    case Type::Number:
        return String::number(m_number_value.value());
    case Type::Percentage:
        return MUST(String::formatted("{}%", m_number_value.value()));
    case Type::Dimension:
        return MUST(String::formatted("{}{}", m_number_value.value(), dimension_unit()));
    case Type::Whitespace:
        return " "_string;
    case Type::CDO:
        return "<!--"_string;
    case Type::CDC:
        return "-->"_string;
    case Type::Colon:
        return ":"_string;
    case Type::Semicolon:
        return ";"_string;
    case Type::Comma:
        return ","_string;
    case Type::OpenSquare:
        return "["_string;
    case Type::CloseSquare:
        return "]"_string;
    case Type::OpenParen:
        return "("_string;
    case Type::CloseParen:
        return ")"_string;
    case Type::OpenCurly:
        return "{"_string;
    case Type::CloseCurly:
        return "}"_string;
    case Type::Invalid:
    default:
        VERIFY_NOT_REACHED();
    }
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
        append_quoted_string(builder, ident().bytes_as_string_view());
        has_type_specific_fields = true;
        break;
    case Type::Function:
        builder.append("Function(value="sv);
        append_quoted_string(builder, function().bytes_as_string_view());
        has_type_specific_fields = true;
        break;
    case Type::AtKeyword:
        builder.append("AtKeyword(value="sv);
        append_quoted_string(builder, at_keyword().bytes_as_string_view());
        has_type_specific_fields = true;
        break;
    case Type::Hash:
        builder.append("Hash(value="sv);
        append_quoted_string(builder, hash_value().bytes_as_string_view());
        builder.appendff(", hash_type={}", hash_type_name(m_hash_type));
        has_type_specific_fields = true;
        break;
    case Type::String:
        builder.append("String(value="sv);
        append_quoted_string(builder, string().bytes_as_string_view());
        has_type_specific_fields = true;
        break;
    case Type::BadString:
        builder.append("BadString("sv);
        break;
    case Type::Url:
        builder.append("Url(value="sv);
        append_quoted_string(builder, url().bytes_as_string_view());
        has_type_specific_fields = true;
        break;
    case Type::BadUrl:
        builder.append("BadUrl("sv);
        break;
    case Type::Delim:
        builder.append("Delim(value="sv);
        append_quoted_string(builder, m_value.bytes_as_string_view());
        builder.appendff(", code_point=U+{:04X}", delim());
        has_type_specific_fields = true;
        break;
    case Type::Number:
        builder.appendff("Number(value={}, number_type={}", number_value(), number_type_name(m_number_value.type()));
        has_type_specific_fields = true;
        break;
    case Type::Percentage:
        builder.appendff("Percentage(value={}, number_type={}", percentage(), number_type_name(m_number_value.type()));
        has_type_specific_fields = true;
        break;
    case Type::Dimension:
        builder.appendff("Dimension(value={}, number_type={}, unit=", dimension_value(), number_type_name(m_number_value.type()));
        append_quoted_string(builder, dimension_unit().bytes_as_string_view());
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

void Token::set_position_range(Badge<Tokenizer>, Position start, Position end)
{
    m_start_position = start;
    m_end_position = end;
}

}
