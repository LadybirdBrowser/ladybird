/*
 * Copyright (c) 2020, Stephan Unverwerth <s.unverwerth@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/StringView.h>
#include <AK/Utf16FlyString.h>
#include <AK/Utf16String.h>
#include <AK/Variant.h>
#include <LibJS/Export.h>

namespace JS {

// U+00A0 NO BREAK SPACE
constexpr inline char16_t const NO_BREAK_SPACE { 0x00A0 };

// U+200C ZERO WIDTH NON-JOINER
constexpr inline char16_t const ZERO_WIDTH_NON_JOINER { 0x200C };

// U+200D ZERO WIDTH JOINER
constexpr inline char16_t const ZERO_WIDTH_JOINER { 0x200D };

// U+2028 LINE SEPARATOR
constexpr inline char16_t const LINE_SEPARATOR { 0x2028 };

// U+2029 PARAGRAPH SEPARATOR
constexpr inline char16_t const PARAGRAPH_SEPARATOR { 0x2029 };

// U+FEFF ZERO WIDTH NO-BREAK SPACE
constexpr inline char16_t const ZERO_WIDTH_NO_BREAK_SPACE { 0xFEFF };

#define ENUMERATE_JS_TOKENS                                     \
    __ENUMERATE_JS_TOKEN(Ampersand, Operator)                   \
    __ENUMERATE_JS_TOKEN(AmpersandEquals, Operator)             \
    __ENUMERATE_JS_TOKEN(Arrow, Operator)                       \
    __ENUMERATE_JS_TOKEN(Asterisk, Operator)                    \
    __ENUMERATE_JS_TOKEN(AsteriskEquals, Operator)              \
    __ENUMERATE_JS_TOKEN(Async, Keyword)                        \
    __ENUMERATE_JS_TOKEN(Await, Keyword)                        \
    __ENUMERATE_JS_TOKEN(BigIntLiteral, Number)                 \
    __ENUMERATE_JS_TOKEN(BoolLiteral, Keyword)                  \
    __ENUMERATE_JS_TOKEN(BracketClose, Punctuation)             \
    __ENUMERATE_JS_TOKEN(BracketOpen, Punctuation)              \
    __ENUMERATE_JS_TOKEN(Break, Keyword)                        \
    __ENUMERATE_JS_TOKEN(Caret, Operator)                       \
    __ENUMERATE_JS_TOKEN(CaretEquals, Operator)                 \
    __ENUMERATE_JS_TOKEN(Case, ControlKeyword)                  \
    __ENUMERATE_JS_TOKEN(Catch, ControlKeyword)                 \
    __ENUMERATE_JS_TOKEN(Class, Keyword)                        \
    __ENUMERATE_JS_TOKEN(Colon, Punctuation)                    \
    __ENUMERATE_JS_TOKEN(Comma, Punctuation)                    \
    __ENUMERATE_JS_TOKEN(Const, Keyword)                        \
    __ENUMERATE_JS_TOKEN(Continue, ControlKeyword)              \
    __ENUMERATE_JS_TOKEN(CurlyClose, Punctuation)               \
    __ENUMERATE_JS_TOKEN(CurlyOpen, Punctuation)                \
    __ENUMERATE_JS_TOKEN(Debugger, Keyword)                     \
    __ENUMERATE_JS_TOKEN(Default, ControlKeyword)               \
    __ENUMERATE_JS_TOKEN(Delete, Keyword)                       \
    __ENUMERATE_JS_TOKEN(Do, ControlKeyword)                    \
    __ENUMERATE_JS_TOKEN(DoubleAmpersand, Operator)             \
    __ENUMERATE_JS_TOKEN(DoubleAmpersandEquals, Operator)       \
    __ENUMERATE_JS_TOKEN(DoubleAsterisk, Operator)              \
    __ENUMERATE_JS_TOKEN(DoubleAsteriskEquals, Operator)        \
    __ENUMERATE_JS_TOKEN(DoublePipe, Operator)                  \
    __ENUMERATE_JS_TOKEN(DoublePipeEquals, Operator)            \
    __ENUMERATE_JS_TOKEN(DoubleQuestionMark, Operator)          \
    __ENUMERATE_JS_TOKEN(DoubleQuestionMarkEquals, Operator)    \
    __ENUMERATE_JS_TOKEN(Else, ControlKeyword)                  \
    __ENUMERATE_JS_TOKEN(Enum, Keyword)                         \
    __ENUMERATE_JS_TOKEN(Eof, Invalid)                          \
    __ENUMERATE_JS_TOKEN(Equals, Operator)                      \
    __ENUMERATE_JS_TOKEN(EqualsEquals, Operator)                \
    __ENUMERATE_JS_TOKEN(EqualsEqualsEquals, Operator)          \
    __ENUMERATE_JS_TOKEN(EscapedKeyword, Identifier)            \
    __ENUMERATE_JS_TOKEN(ExclamationMark, Operator)             \
    __ENUMERATE_JS_TOKEN(ExclamationMarkEquals, Operator)       \
    __ENUMERATE_JS_TOKEN(ExclamationMarkEqualsEquals, Operator) \
    __ENUMERATE_JS_TOKEN(Export, Keyword)                       \
    __ENUMERATE_JS_TOKEN(Extends, Keyword)                      \
    __ENUMERATE_JS_TOKEN(Finally, ControlKeyword)               \
    __ENUMERATE_JS_TOKEN(For, ControlKeyword)                   \
    __ENUMERATE_JS_TOKEN(Function, Keyword)                     \
    __ENUMERATE_JS_TOKEN(GreaterThan, Operator)                 \
    __ENUMERATE_JS_TOKEN(GreaterThanEquals, Operator)           \
    __ENUMERATE_JS_TOKEN(Identifier, Identifier)                \
    __ENUMERATE_JS_TOKEN(If, ControlKeyword)                    \
    __ENUMERATE_JS_TOKEN(Implements, Keyword)                   \
    __ENUMERATE_JS_TOKEN(Import, Keyword)                       \
    __ENUMERATE_JS_TOKEN(In, Keyword)                           \
    __ENUMERATE_JS_TOKEN(Instanceof, Keyword)                   \
    __ENUMERATE_JS_TOKEN(Interface, Keyword)                    \
    __ENUMERATE_JS_TOKEN(Invalid, Invalid)                      \
    __ENUMERATE_JS_TOKEN(LessThan, Operator)                    \
    __ENUMERATE_JS_TOKEN(LessThanEquals, Operator)              \
    __ENUMERATE_JS_TOKEN(Let, Keyword)                          \
    __ENUMERATE_JS_TOKEN(Minus, Operator)                       \
    __ENUMERATE_JS_TOKEN(MinusEquals, Operator)                 \
    __ENUMERATE_JS_TOKEN(MinusMinus, Operator)                  \
    __ENUMERATE_JS_TOKEN(New, Keyword)                          \
    __ENUMERATE_JS_TOKEN(NullLiteral, Keyword)                  \
    __ENUMERATE_JS_TOKEN(NumericLiteral, Number)                \
    __ENUMERATE_JS_TOKEN(Package, Keyword)                      \
    __ENUMERATE_JS_TOKEN(ParenClose, Punctuation)               \
    __ENUMERATE_JS_TOKEN(ParenOpen, Punctuation)                \
    __ENUMERATE_JS_TOKEN(Percent, Operator)                     \
    __ENUMERATE_JS_TOKEN(PercentEquals, Operator)               \
    __ENUMERATE_JS_TOKEN(Period, Operator)                      \
    __ENUMERATE_JS_TOKEN(Pipe, Operator)                        \
    __ENUMERATE_JS_TOKEN(PipeEquals, Operator)                  \
    __ENUMERATE_JS_TOKEN(Plus, Operator)                        \
    __ENUMERATE_JS_TOKEN(PlusEquals, Operator)                  \
    __ENUMERATE_JS_TOKEN(PlusPlus, Operator)                    \
    __ENUMERATE_JS_TOKEN(Private, Keyword)                      \
    __ENUMERATE_JS_TOKEN(PrivateIdentifier, Identifier)         \
    __ENUMERATE_JS_TOKEN(Protected, Keyword)                    \
    __ENUMERATE_JS_TOKEN(Public, Keyword)                       \
    __ENUMERATE_JS_TOKEN(QuestionMark, Operator)                \
    __ENUMERATE_JS_TOKEN(QuestionMarkPeriod, Operator)          \
    __ENUMERATE_JS_TOKEN(RegexFlags, String)                    \
    __ENUMERATE_JS_TOKEN(RegexLiteral, String)                  \
    __ENUMERATE_JS_TOKEN(Return, ControlKeyword)                \
    __ENUMERATE_JS_TOKEN(Semicolon, Punctuation)                \
    __ENUMERATE_JS_TOKEN(ShiftLeft, Operator)                   \
    __ENUMERATE_JS_TOKEN(ShiftLeftEquals, Operator)             \
    __ENUMERATE_JS_TOKEN(ShiftRight, Operator)                  \
    __ENUMERATE_JS_TOKEN(ShiftRightEquals, Operator)            \
    __ENUMERATE_JS_TOKEN(Slash, Operator)                       \
    __ENUMERATE_JS_TOKEN(SlashEquals, Operator)                 \
    __ENUMERATE_JS_TOKEN(Static, Keyword)                       \
    __ENUMERATE_JS_TOKEN(StringLiteral, String)                 \
    __ENUMERATE_JS_TOKEN(Super, Keyword)                        \
    __ENUMERATE_JS_TOKEN(Switch, ControlKeyword)                \
    __ENUMERATE_JS_TOKEN(TemplateLiteralEnd, String)            \
    __ENUMERATE_JS_TOKEN(TemplateLiteralExprEnd, Punctuation)   \
    __ENUMERATE_JS_TOKEN(TemplateLiteralExprStart, Punctuation) \
    __ENUMERATE_JS_TOKEN(TemplateLiteralStart, String)          \
    __ENUMERATE_JS_TOKEN(TemplateLiteralString, String)         \
    __ENUMERATE_JS_TOKEN(This, Keyword)                         \
    __ENUMERATE_JS_TOKEN(Throw, ControlKeyword)                 \
    __ENUMERATE_JS_TOKEN(Tilde, Operator)                       \
    __ENUMERATE_JS_TOKEN(TripleDot, Operator)                   \
    __ENUMERATE_JS_TOKEN(Trivia, Trivia)                        \
    __ENUMERATE_JS_TOKEN(Try, ControlKeyword)                   \
    __ENUMERATE_JS_TOKEN(Typeof, Keyword)                       \
    __ENUMERATE_JS_TOKEN(UnsignedShiftRight, Operator)          \
    __ENUMERATE_JS_TOKEN(UnsignedShiftRightEquals, Operator)    \
    __ENUMERATE_JS_TOKEN(UnterminatedRegexLiteral, String)      \
    __ENUMERATE_JS_TOKEN(UnterminatedStringLiteral, String)     \
    __ENUMERATE_JS_TOKEN(UnterminatedTemplateLiteral, String)   \
    __ENUMERATE_JS_TOKEN(Var, Keyword)                          \
    __ENUMERATE_JS_TOKEN(Void, Keyword)                         \
    __ENUMERATE_JS_TOKEN(While, ControlKeyword)                 \
    __ENUMERATE_JS_TOKEN(With, ControlKeyword)                  \
    __ENUMERATE_JS_TOKEN(Yield, ControlKeyword)

enum class TokenType {
#define __ENUMERATE_JS_TOKEN(type, category) type,
    ENUMERATE_JS_TOKENS
#undef __ENUMERATE_JS_TOKEN
        _COUNT_OF_TOKENS
};
constexpr size_t cs_num_of_js_tokens = static_cast<size_t>(TokenType::_COUNT_OF_TOKENS);

enum class TokenCategory {
    Invalid,
    Trivia,
    Number,
    String,
    Punctuation,
    Operator,
    Keyword,
    ControlKeyword,
    Identifier
};

class JS_API Token {
public:
    Token() = default;

    Token(TokenType type, StringView message, Utf16View const& trivia, Utf16View const& value, size_t line_number, size_t line_column, size_t offset)
        : m_type(type)
        , m_message(message)
        , m_trivia(trivia)
        , m_original_value(value)
        , m_value(value)
        , m_line_number(line_number)
        , m_line_column(line_column)
        , m_offset(offset)
    {
    }

    TokenType type() const { return m_type; }
    TokenCategory category() const;
    static TokenCategory category(TokenType);
    char const* name() const;
    static char const* name(TokenType);

    StringView message() const { return m_message; }
    Utf16View const& trivia() const { return m_trivia; }
    Utf16View const& original_value() const { return m_original_value; }

    Utf16View value() const
    {
        return m_value.visit(
            [](Utf16View const& view) { return view; },
            [](Utf16FlyString const& identifier) { return identifier.view(); },
            [](Empty) -> Utf16View { VERIFY_NOT_REACHED(); });
    }

    Utf16FlyString fly_string_value() const
    {
        return m_value.visit(
            [](Utf16View const& view) { return Utf16FlyString::from_utf16(view); },
            [](Utf16FlyString const& identifier) { return identifier; },
            [](Empty) -> Utf16FlyString { VERIFY_NOT_REACHED(); });
    }

    size_t line_number() const { return m_line_number; }
    size_t line_column() const { return m_line_column; }
    size_t offset() const { return m_offset; }
    double double_value() const;
    bool bool_value() const;

    enum class StringValueStatus {
        Ok,
        MalformedHexEscape,
        MalformedUnicodeEscape,
        UnicodeEscapeOverflow,
        LegacyOctalEscapeSequence,
    };
    Utf16String string_value(StringValueStatus& status) const;
    Utf16String raw_template_value() const;

    void set_identifier_value(Utf16FlyString value)
    {
        m_value = move(value);
    }

    bool is_identifier_name() const;
    bool trivia_contains_line_terminator() const;

private:
    TokenType m_type { TokenType::Invalid };
    StringView m_message;
    Utf16View m_trivia;
    Utf16View m_original_value;
    Variant<Empty, Utf16View, Utf16FlyString> m_value;
    size_t m_line_number { 0 };
    size_t m_line_column { 0 };
    size_t m_offset { 0 };
};

}
