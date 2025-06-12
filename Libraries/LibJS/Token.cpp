/*
 * Copyright (c) 2020, Stephan Unverwerth <s.unverwerth@serenityos.org>
 * Copyright (c) 2020-2021, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Token.h"
#include <AK/Assertions.h>
#include <AK/CharacterTypes.h>
#include <AK/FloatingPointStringConversions.h>
#include <AK/GenericLexer.h>
#include <AK/StringBuilder.h>

namespace JS {

char const* Token::name(TokenType type)
{
    switch (type) {
#define __ENUMERATE_JS_TOKEN(type, category) \
    case TokenType::type:                    \
        return #type;
        ENUMERATE_JS_TOKENS
#undef __ENUMERATE_JS_TOKEN
    default:
        VERIFY_NOT_REACHED();
        return "<Unknown>";
    }
}

char const* Token::name() const
{
    return name(m_type);
}

TokenCategory Token::category(TokenType type)
{
    switch (type) {
#define __ENUMERATE_JS_TOKEN(type, category) \
    case TokenType::type:                    \
        return TokenCategory::category;
        ENUMERATE_JS_TOKENS
#undef __ENUMERATE_JS_TOKEN
    default:
        VERIFY_NOT_REACHED();
    }
}

TokenCategory Token::category() const
{
    return category(m_type);
}

double Token::double_value() const
{
    VERIFY(type() == TokenType::NumericLiteral);

    Vector<char, 32> buffer;

    for (auto ch : value()) {
        if (ch == '_')
            continue;
        buffer.append(ch);
    }
    buffer.append('\0');

    auto value_string = StringView { buffer.data(), buffer.size() - 1 };
    if (value_string[0] == '0' && value_string.length() >= 2) {
        if (value_string[1] == 'x' || value_string[1] == 'X') {
            // hexadecimal
            return static_cast<double>(strtoul(value_string.characters_without_null_termination() + 2, nullptr, 16));
        } else if (value_string[1] == 'o' || value_string[1] == 'O') {
            // octal
            return static_cast<double>(strtoul(value_string.characters_without_null_termination() + 2, nullptr, 8));
        } else if (value_string[1] == 'b' || value_string[1] == 'B') {
            // binary
            return static_cast<double>(strtoul(value_string.characters_without_null_termination() + 2, nullptr, 2));
        } else if (is_ascii_digit(value_string[1])) {
            // also octal, but syntax error in strict mode
            if (!value().contains('8') && !value().contains('9'))
                return static_cast<double>(strtoul(value_string.characters_without_null_termination() + 1, nullptr, 8));
        }
    }
    // This should always be a valid double
    return value_string.to_number<double>().release_value();
}

static u32 hex2int(char x)
{
    VERIFY(is_ascii_hex_digit(x));
    if (x >= '0' && x <= '9')
        return x - '0';
    return 10u + (to_ascii_lowercase(x) - 'a');
}

ByteString Token::string_value(StringValueStatus& status) const
{
    VERIFY(type() == TokenType::StringLiteral || type() == TokenType::TemplateLiteralString);

    auto is_template = type() == TokenType::TemplateLiteralString;
    GenericLexer lexer(is_template ? value() : value().substring_view(1, value().length() - 2));

    auto encoding_failure = [&status](StringValueStatus parse_status) -> ByteString {
        status = parse_status;
        return {};
    };

    StringBuilder builder;
    while (!lexer.is_eof()) {
        // No escape, consume one char and continue
        if (!lexer.next_is('\\')) {

            if (is_template && lexer.next_is('\r')) {
                lexer.ignore();
                if (lexer.next_is('\n'))
                    lexer.ignore();

                builder.append('\n');
                continue;
            }

            builder.append(lexer.consume());
            continue;
        }

        // Unicode escape
        if (lexer.next_is("\\u"sv)) {
            auto code_point_or_error = lexer.consume_escaped_code_point();

            if (code_point_or_error.is_error()) {
                switch (code_point_or_error.error()) {
                case GenericLexer::UnicodeEscapeError::MalformedUnicodeEscape:
                    return encoding_failure(StringValueStatus::MalformedUnicodeEscape);
                case GenericLexer::UnicodeEscapeError::UnicodeEscapeOverflow:
                    return encoding_failure(StringValueStatus::UnicodeEscapeOverflow);
                }
            }

            builder.append_code_point(code_point_or_error.value());
            continue;
        }

        lexer.ignore();
        VERIFY(!lexer.is_eof());

        // Line continuation
        if (lexer.next_is('\n') || lexer.next_is('\r')) {
            if (lexer.next_is("\r\n"))
                lexer.ignore();
            lexer.ignore();
            continue;
        }
        // Line continuation
        if (lexer.next_is(LINE_SEPARATOR_STRING) || lexer.next_is(PARAGRAPH_SEPARATOR_STRING)) {
            lexer.ignore(3);
            continue;
        }
        // Null-byte escape
        if (lexer.next_is('0') && !is_ascii_digit(lexer.peek(1))) {
            lexer.ignore();
            builder.append('\0');
            continue;
        }
        // Hex escape
        if (lexer.next_is('x')) {
            lexer.ignore();
            if (!is_ascii_hex_digit(lexer.peek()) || !is_ascii_hex_digit(lexer.peek(1)))
                return encoding_failure(StringValueStatus::MalformedHexEscape);
            auto code_point = hex2int(lexer.consume()) * 16 + hex2int(lexer.consume());
            VERIFY(code_point <= 255);
            builder.append_code_point(code_point);
            continue;
        }

        // In non-strict mode LegacyOctalEscapeSequence is allowed in strings:
        // https://tc39.es/ecma262/#sec-additional-syntax-string-literals
        Optional<ByteString> octal_str;

        auto is_octal_digit = [](char ch) { return ch >= '0' && ch <= '7'; };
        auto is_zero_to_three = [](char ch) { return ch >= '0' && ch <= '3'; };
        auto is_four_to_seven = [](char ch) { return ch >= '4' && ch <= '7'; };

        // OctalDigit [lookahead ∉ OctalDigit]
        if (is_octal_digit(lexer.peek()) && !is_octal_digit(lexer.peek(1)))
            octal_str = lexer.consume(1);
        // ZeroToThree OctalDigit [lookahead ∉ OctalDigit]
        else if (is_zero_to_three(lexer.peek()) && is_octal_digit(lexer.peek(1)) && !is_octal_digit(lexer.peek(2)))
            octal_str = lexer.consume(2);
        // FourToSeven OctalDigit
        else if (is_four_to_seven(lexer.peek()) && is_octal_digit(lexer.peek(1)))
            octal_str = lexer.consume(2);
        // ZeroToThree OctalDigit OctalDigit
        else if (is_zero_to_three(lexer.peek()) && is_octal_digit(lexer.peek(1)) && is_octal_digit(lexer.peek(2)))
            octal_str = lexer.consume(3);

        if (octal_str.has_value()) {
            status = StringValueStatus::LegacyOctalEscapeSequence;
            auto code_point = strtoul(octal_str->characters(), nullptr, 8);
            VERIFY(code_point <= 255);
            builder.append_code_point(code_point);
            continue;
        }

        if (lexer.next_is('8') || lexer.next_is('9')) {
            status = StringValueStatus::LegacyOctalEscapeSequence;
            builder.append(lexer.consume());
            continue;
        }

        lexer.retreat();
        builder.append(lexer.consume_escaped_character('\\', "b\bf\fn\nr\rt\tv\v"sv));
    }
    return builder.to_byte_string();
}

// 12.8.6.2 Static Semantics: TRV, https://tc39.es/ecma262/#sec-static-semantics-trv
ByteString Token::raw_template_value() const
{
    return value().replace("\r\n"sv, "\n"sv, ReplaceMode::All).replace("\r"sv, "\n"sv, ReplaceMode::All);
}

bool Token::bool_value() const
{
    VERIFY(type() == TokenType::BoolLiteral);
    return value() == "true";
}

bool Token::is_identifier_name() const
{
    // IdentifierNames are Identifiers + ReservedWords
    // The standard defines this reversed: Identifiers are IdentifierNames except reserved words
    // https://tc39.es/ecma262/#prod-Identifier
    return m_type == TokenType::Identifier
        || m_type == TokenType::EscapedKeyword
        || m_type == TokenType::Await
        || m_type == TokenType::Async
        || m_type == TokenType::BoolLiteral
        || m_type == TokenType::Break
        || m_type == TokenType::Case
        || m_type == TokenType::Catch
        || m_type == TokenType::Class
        || m_type == TokenType::Const
        || m_type == TokenType::Continue
        || m_type == TokenType::Debugger
        || m_type == TokenType::Default
        || m_type == TokenType::Delete
        || m_type == TokenType::Do
        || m_type == TokenType::Else
        || m_type == TokenType::Enum
        || m_type == TokenType::Export
        || m_type == TokenType::Extends
        || m_type == TokenType::Finally
        || m_type == TokenType::For
        || m_type == TokenType::Function
        || m_type == TokenType::If
        || m_type == TokenType::Import
        || m_type == TokenType::In
        || m_type == TokenType::Instanceof
        || m_type == TokenType::Let
        || m_type == TokenType::New
        || m_type == TokenType::NullLiteral
        || m_type == TokenType::Return
        || m_type == TokenType::Super
        || m_type == TokenType::Switch
        || m_type == TokenType::This
        || m_type == TokenType::Throw
        || m_type == TokenType::Try
        || m_type == TokenType::Typeof
        || m_type == TokenType::Var
        || m_type == TokenType::Void
        || m_type == TokenType::While
        || m_type == TokenType::With
        || m_type == TokenType::Yield;
}

bool Token::trivia_contains_line_terminator() const
{
    return m_trivia.contains('\n') || m_trivia.contains('\r') || m_trivia.contains(LINE_SEPARATOR_STRING) || m_trivia.contains(PARAGRAPH_SEPARATOR_STRING);
}

}
