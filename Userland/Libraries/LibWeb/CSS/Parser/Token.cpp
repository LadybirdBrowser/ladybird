/*
 * Copyright (c) 2020-2021, the SerenityOS developers.
 * Copyright (c) 2022-2023, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/Parser/Token.h>
#include <LibWeb/CSS/Serialize.h>

namespace Web::CSS::Parser {

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
    switch (m_type) {
    case Type::Invalid:
        VERIFY_NOT_REACHED();

    case Type::EndOfFile:
        return "__EOF__"_string;
    case Type::Ident:
        return MUST(String::formatted("Ident: {}", ident()));
    case Type::Function:
        return MUST(String::formatted("Function: {}", function()));
    case Type::AtKeyword:
        return MUST(String::formatted("AtKeyword: {}", at_keyword()));
    case Type::Hash:
        return MUST(String::formatted("Hash: {} (hash_type: {})", hash_value(), m_hash_type == HashType::Unrestricted ? "Unrestricted" : "Id"));
    case Type::String:
        return MUST(String::formatted("String: {}", string()));
    case Type::BadString:
        return "BadString"_string;
    case Type::Url:
        return MUST(String::formatted("Url: {}", url()));
    case Type::BadUrl:
        return "BadUrl"_string;
    case Type::Delim:
        return MUST(String::formatted("Delim: {}", m_value));
    case Type::Number:
        return MUST(String::formatted("Number: {}{} (number_type: {})", m_number_value.value() > 0 && m_number_value.is_integer_with_explicit_sign() ? "+" : "", m_number_value.value(), m_number_value.is_integer() ? "Integer" : "Number"));
    case Type::Percentage:
        return MUST(String::formatted("Percentage: {}% (number_type: {})", percentage(), m_number_value.is_integer() ? "Integer" : "Number"));
    case Type::Dimension:
        return MUST(String::formatted("Dimension: {}{} (number_type: {})", dimension_value(), dimension_unit(), m_number_value.is_integer() ? "Integer" : "Number"));
    case Type::Whitespace:
        return "Whitespace"_string;
    case Type::CDO:
        return "CDO"_string;
    case Type::CDC:
        return "CDC"_string;
    case Type::Colon:
        return "Colon"_string;
    case Type::Semicolon:
        return "Semicolon"_string;
    case Type::Comma:
        return "Comma"_string;
    case Type::OpenSquare:
        return "OpenSquare"_string;
    case Type::CloseSquare:
        return "CloseSquare"_string;
    case Type::OpenParen:
        return "OpenParen"_string;
    case Type::CloseParen:
        return "CloseParen"_string;
    case Type::OpenCurly:
        return "OpenCurly"_string;
    case Type::CloseCurly:
        return "CloseCurly"_string;
    }
    VERIFY_NOT_REACHED();
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

}
