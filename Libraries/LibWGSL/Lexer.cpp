/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/CharacterTypes.h>
#include <AK/HashMap.h>
#include <AK/HashTable.h>
#include <LibWGSL/Lexer.h>

namespace WGSL {
namespace {

HashMap<String, TokenType> keywords {
    { "struct"_string, KeywordToken { KeywordToken::Value::Struct } },
    { "fn"_string, KeywordToken { KeywordToken::Value::Fn } },
    { "var"_string, KeywordToken { KeywordToken::Value::Var } },
    { "return"_string, KeywordToken { KeywordToken::Value::Return } },
};

HashMap<String, TokenType> types {
    { "vec3f"_string, TypeToken { TypeToken::Value::vec3f } },
    { "vec4f"_string, TypeToken { TypeToken::Value::vec4f } },
};

HashTable<String> attributes { 4 };

}

Lexer::Lexer(StringView const processed_text)
    : m_lexer(processed_text)
{
    attributes.set("builtin"_string);
    attributes.set("location"_string);
    attributes.set("vertex"_string);
    attributes.set("fragment"_string);
}

Token Lexer::next_token()
{
    skip_blankspace();

    if (m_lexer.is_eof()) {
        return Token { EndOfFileToken {}, m_lexer.tell(), m_current_line, m_current_column };
    }

    size_t const start_pos = m_lexer.tell();
    size_t const start_line = m_current_line;
    size_t const start_column = m_current_column;

    char const c = m_lexer.peek();

    // FIXME: WGSL tokenization needs to handle unicode but GenericLexer is currently ASCII only

    if (is_ascii_digit(c)) {
        return tokenize_integer_literal();
    }

    if (is_ascii_alphanumeric(c) || c == '_') {
        StringBuilder text;
        size_t const pos = m_lexer.tell();
        size_t const line = m_current_line;
        size_t const column = m_current_column;

        if (char const first = m_lexer.peek(); !is_ascii_alphanumeric(first) && first != '_') {
            return Token { InvalidToken { "Invalid name start character"sv }, start_pos, start_line, start_column };
        }

        text.append(m_lexer.consume());
        m_current_column++;

        while (is_ascii_alphanumeric(m_lexer.peek()) || m_lexer.peek() == '_') {
            text.append(m_lexer.consume());
            m_current_column++;
        }

        StringView const text_view = m_lexer.input().substring_view(pos, m_lexer.tell() - pos);
        return tokenize(text_view, pos, line, column);
    }

    switch (c) {
    case '(':
        m_lexer.consume();
        m_current_column++;
        return Token { SyntacticToken { SyntacticToken::Value::OpenParen }, start_pos, start_line, start_column };
    case ')':
        m_lexer.consume();
        m_current_column++;
        return Token { SyntacticToken { SyntacticToken::Value::CloseParen }, start_pos, start_line, start_column };
    case '{':
        m_lexer.consume();
        m_current_column++;
        return Token { SyntacticToken { SyntacticToken::Value::OpenBrace }, start_pos, start_line, start_column };
    case '}':
        m_lexer.consume();
        m_current_column++;
        return Token { SyntacticToken { SyntacticToken::Value::CloseBrace }, start_pos, start_line, start_column };
    case ';':
        m_lexer.consume();
        m_current_column++;
        return Token { SyntacticToken { SyntacticToken::Value::Semicolon }, start_pos, start_line, start_column };
    case ',':
        m_lexer.consume();
        m_current_column++;
        return Token { SyntacticToken { SyntacticToken::Value::Comma }, start_pos, start_line, start_column };
    case ':':
        m_lexer.consume();
        m_current_column++;
        if (m_lexer.peek() == ':') {
            m_lexer.consume();
            m_current_column++;
            return Token { InvalidToken { "Unexpected '::' operator"sv }, start_pos, start_line, start_column };
        }
        return Token { SyntacticToken { SyntacticToken::Value::Colon }, start_pos, start_line, start_column };
    case '.':
        m_lexer.consume();
        m_current_column++;
        return Token { SyntacticToken { SyntacticToken::Value::Dot }, start_pos, start_line, start_column };
    case '@':
        m_lexer.consume();
        m_current_column++;
        return Token { SyntacticToken { SyntacticToken::Value::At }, start_pos, start_line, start_column };
    case '-':
        m_lexer.consume();
        m_current_column++;
        if (m_lexer.peek() == '>') {
            m_lexer.consume();
            m_current_column++;
            return Token { SyntacticToken { SyntacticToken::Value::Arrow }, start_pos, start_line, start_column };
        }
        return Token { InvalidToken { "Unexpected '-' operator"sv }, start_pos, start_line, start_column };
    case '=':
        m_lexer.consume();
        m_current_column++;
        return Token { SyntacticToken { SyntacticToken::Value::Equals }, start_pos, start_line, start_column };
    default:
        break;
    }

    m_lexer.consume();
    m_current_column++;
    StringView const unknown = m_lexer.input().substring_view(start_pos, 1);
    return Token { InvalidToken { ByteString::formatted("Invalid token encountered: {}"sv, unknown) }, start_pos, start_line, start_column };
}

void Lexer::skip_blankspace()
{
    while (!m_lexer.is_eof()) {
        if (char const c = m_lexer.peek(); c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r') {
            if (c == '\n') {
                m_current_line++;
                m_current_column = 1;
            } else {
                m_current_column++;
            }
            m_lexer.consume();
        } else {
            break;
        }
    }
}

Token Lexer::tokenize_integer_literal()
{
    size_t const start_pos = m_lexer.tell();
    size_t const start_line = m_current_line;
    size_t const start_column = m_current_column;

    char const first = m_lexer.peek();
    if (!is_ascii_digit(first)) {
        return Token { InvalidToken { "Invalid integer literal start character"sv }, start_pos, start_line, start_column };
    }

    StringBuilder text;
    text.append(m_lexer.consume());
    m_current_column++;

    if (first != '0') {
        while (is_ascii_digit(m_lexer.peek())) {
            text.append(m_lexer.consume());
            m_current_column++;
        }
    } else if (is_ascii_digit(m_lexer.peek())) {
        return Token { InvalidToken { "Leading zero in integer literal is not allowed"sv }, start_pos, start_line, start_column };
    }

    return Token { LiteralToken { LiteralToken::Value::Int }, start_pos, start_line, start_column };
}

Token Lexer::tokenize(StringView const text, size_t const start_pos, size_t const start_line, size_t const start_column)
{
    if (auto keyword_type = keywords.get(text); keyword_type.has_value()) {
        return Token { keyword_type.value(), start_pos, start_line, start_column };
    }
    if (auto type_type = types.get(text); type_type.has_value()) {
        return Token { type_type.value(), start_pos, start_line, start_column };
    }
    if (attributes.contains(text)) {
        return tokenize_attribute(text, start_pos, start_line, start_column);
    }
    if (text == "_") {
        return Token { InvalidToken { "Single underscore is not a valid identifier"sv }, start_pos, start_line, start_column };
    }
    if (text.to_byte_string().starts_with("__"sv)) {
        return Token { InvalidToken { "Identifiers cannot start with double underscore"sv }, start_pos, start_line, start_column };
    }
    return Token { IdentifierToken { MUST(String::from_byte_string(text.to_byte_string())) }, start_pos, start_line, start_column };
}

Token Lexer::tokenize_attribute(StringView const name, size_t const start_pos, size_t const start_line, size_t const start_column)
{
    // Check for arguments
    skip_blankspace();
    if (m_lexer.peek() == '(') {
        m_lexer.consume();
        m_current_column++;
        skip_blankspace();

        StringBuilder arg_text;
        int paren_level = 1;
        while (!m_lexer.is_eof() && paren_level > 0) {
            if (char const c = m_lexer.peek(); c == '(') {
                paren_level++;
            } else if (c == ')') {
                paren_level--;
                if (paren_level == 0) {
                    m_lexer.consume();
                    m_current_column++;
                    break;
                }
            }
            arg_text.append(m_lexer.consume());
            m_current_column++;
        }

        if (paren_level > 0) {
            return Token { InvalidToken { "Unclosed attribute argument parentheses"sv }, start_pos, start_line, start_column };
        }

        StringView const arg = arg_text.string_view();

        if (name == "builtin") {
            if (arg == "position") {
                return Token { AttributeToken { BuiltinAttribute { BuiltinAttribute::Flags::Position } }, start_pos, start_line, start_column };
            }
            return Token { InvalidToken { "Invalid builtin attribute argument"sv }, start_pos, start_line, start_column };
        }
        if (name == "location") {
            auto maybe_value = arg.to_number<u32>();
            if (!maybe_value.has_value()) {
                return Token { InvalidToken { "Invalid location attribute argument"sv }, start_pos, start_line, start_column };
            }
            return Token { AttributeToken { LocationAttribute { maybe_value.value() } }, start_pos, start_line, start_column };
        }
        return Token { InvalidToken { "Invalid attribute with arguments"sv }, start_pos, start_line, start_column };
    }

    if (name == "vertex") {
        return Token { AttributeToken { VertexAttribute {} }, start_pos, start_line, start_column };
    }
    if (name == "fragment") {
        return Token { AttributeToken { FragmentAttribute {} }, start_pos, start_line, start_column };
    }

    return Token { InvalidToken { "Invalid attribute name"sv }, start_pos, start_line, start_column };
}

String Token::to_string() const
{
    StringBuilder builder;
    type.visit(
        [&](InvalidToken const& token) {
            builder.append(ByteString::formatted("Invalid: {}"sv, token.error_message));
        },
        [&](EndOfFileToken const&) {
            builder.append("EndOfFile"_string);
        },
        [&](SyntacticToken const& token) {
            String value;
            switch (token.value) {
            case SyntacticToken::Value::OpenParen:
                value = "OpenParen"_string;
                break;
            case SyntacticToken::Value::CloseParen:
                value = "CloseParen"_string;
                break;
            case SyntacticToken::Value::OpenBrace:
                value = "OpenBrace"_string;
                break;
            case SyntacticToken::Value::CloseBrace:
                value = "CloseBrace"_string;
                break;
            case SyntacticToken::Value::Semicolon:
                value = "Semicolon"_string;
                break;
            case SyntacticToken::Value::Comma:
                value = "Comma"_string;
                break;
            case SyntacticToken::Value::Colon:
                value = "Colon"_string;
                break;
            case SyntacticToken::Value::Dot:
                value = "Dot"_string;
                break;
            case SyntacticToken::Value::Arrow:
                value = "Arrow"_string;
                break;
            case SyntacticToken::Value::Equals:
                value = "Equals"_string;
                break;
            case SyntacticToken::Value::At:
                value = "At"_string;
                break;
            }
            builder.appendff("Syntactic:{}", value);
        },
        [&](TypeToken const& token) {
            String value;
            switch (token.value) {
            case TypeToken::Value::vec3f:
                value = "vec3f"_string;
                break;
            case TypeToken::Value::vec4f:
                value = "vec4f"_string;
                break;
            }
            builder.appendff("Type:{}", value);
        },
        [&](IdentifierToken const& token) {
            builder.appendff("Identifier:{}", token.value);
        },
        [&](KeywordToken const& token) {
            String value;
            switch (token.value) {
            case KeywordToken::Value::Struct:
                value = "Struct"_string;
                break;
            case KeywordToken::Value::Fn:
                value = "Fn"_string;
                break;
            case KeywordToken::Value::Var:
                value = "Var"_string;
                break;
            case KeywordToken::Value::Return:
                value = "Return"_string;
                break;
            }
            builder.appendff("Keyword:{}", value);
        },
        [&](LiteralToken const& token) {
            String value;
            switch (token.value) {
            case LiteralToken::Value::Int:
                value = "Int"_string;
                break;
            }
            builder.appendff("Literal:{}", value);
        },
        [&](AttributeToken const& token) {
            token.visit(
                [&](BuiltinAttribute const& attr) {
                    String value;
                    switch (attr.value) {
                    case BuiltinAttribute::Flags::Position:
                        value = "Position"_string;
                        break;
                    }
                    builder.appendff("Attribute:Builtin[{}]", value);
                },
                [&](LocationAttribute const& attr) {
                    builder.appendff("Attribute:Location[{}]", attr.value);
                },
                [&](VertexAttribute const&) {
                    builder.append("Attribute:Vertex"_string);
                },
                [&](FragmentAttribute const&) {
                    builder.append("Attribute:Fragment"_string);
                });
        });
    builder.appendff(" at position {}, line {}, column {}", position, line, column);
    return builder.to_string().value();
}

}
