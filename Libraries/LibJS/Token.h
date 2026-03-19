/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>

namespace JS {

constexpr u32 LINE_SEPARATOR = 0x2028;
constexpr u32 PARAGRAPH_SEPARATOR = 0x2029;

// NB: These enums must match the Rust token::TokenCategory and
//     token::TokenType enums in Libraries/LibJS/Rust/src/token.rs.

enum class TokenCategory : u8 {
    Invalid,
    Trivia,
    Number,
    String,
    Punctuation,
    Operator,
    Keyword,
    ControlKeyword,
    Identifier,
};

// NB: Keep in sync with define_tokens! in token.rs.
//     The order must be identical (alphabetical by variant name).
enum class TokenType : u8 {
    Ampersand,
    AmpersandEquals,
    Arrow,
    Asterisk,
    AsteriskEquals,
    Async,
    Await,
    BigIntLiteral,
    BoolLiteral,
    BracketClose,
    BracketOpen,
    Break,
    Caret,
    CaretEquals,
    Case,
    Catch,
    Class,
    Colon,
    Comma,
    Const,
    Continue,
    CurlyClose,
    CurlyOpen,
    Debugger,
    Default,
    Delete,
    Do,
    DoubleAmpersand,
    DoubleAmpersandEquals,
    DoubleAsterisk,
    DoubleAsteriskEquals,
    DoublePipe,
    DoublePipeEquals,
    DoubleQuestionMark,
    DoubleQuestionMarkEquals,
    Else,
    Enum,
    Eof,
    Equals,
    EqualsEquals,
    EqualsEqualsEquals,
    EscapedKeyword,
    ExclamationMark,
    ExclamationMarkEquals,
    ExclamationMarkEqualsEquals,
    Export,
    Extends,
    Finally,
    For,
    Function,
    GreaterThan,
    GreaterThanEquals,
    Identifier,
    If,
    Implements,
    Import,
    In,
    Instanceof,
    Interface,
    Invalid,
    LessThan,
    LessThanEquals,
    Let,
    Minus,
    MinusEquals,
    MinusMinus,
    New,
    NullLiteral,
    NumericLiteral,
    Package,
    ParenClose,
    ParenOpen,
    Percent,
    PercentEquals,
    Period,
    Pipe,
    PipeEquals,
    Plus,
    PlusEquals,
    PlusPlus,
    Private,
    PrivateIdentifier,
    Protected,
    Public,
    QuestionMark,
    QuestionMarkPeriod,
    RegexFlags,
    RegexLiteral,
    Return,
    Semicolon,
    ShiftLeft,
    ShiftLeftEquals,
    ShiftRight,
    ShiftRightEquals,
    Slash,
    SlashEquals,
    Static,
    StringLiteral,
    Super,
    Switch,
    TemplateLiteralEnd,
    TemplateLiteralExprEnd,
    TemplateLiteralExprStart,
    TemplateLiteralStart,
    TemplateLiteralString,
    This,
    Throw,
    Tilde,
    TripleDot,
    Trivia,
    Try,
    Typeof,
    UnsignedShiftRight,
    UnsignedShiftRightEquals,
    UnterminatedRegexLiteral,
    UnterminatedStringLiteral,
    UnterminatedTemplateLiteral,
    Var,
    Void,
    While,
    With,
    Yield,
    _COUNT_OF_TOKENS,
};

// Pack token type and category into a u64 for span data storage.
inline u64 pack_token_data(TokenType type, TokenCategory category)
{
    return (static_cast<u64>(category) << 8) | static_cast<u64>(type);
}

inline TokenType token_type_from_packed(u64 data)
{
    return static_cast<TokenType>(data & 0xFF);
}

inline TokenCategory token_category_from_packed(u64 data)
{
    return static_cast<TokenCategory>((data >> 8) & 0xFF);
}

}
