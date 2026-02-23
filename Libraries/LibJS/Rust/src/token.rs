/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Token types and Token struct for the lexer.

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum TokenCategory {
    Invalid,
    Trivia,
    Number,
    String,
    Punctuation,
    Operator,
    Keyword,
    ControlKeyword,
    Identifier,
}

/// Generates the `TokenType` enum with `category()` and `name()` methods.
/// Each entry maps a variant to its `TokenCategory`. The name is derived
/// automatically via `stringify!`.
macro_rules! define_tokens {
    ( $( $variant:ident => $category:ident ),* $(,)? ) => {
        #[derive(Debug, Clone, Copy, PartialEq, Eq)]
        pub enum TokenType {
            $( $variant, )*
        }

        impl TokenType {
            pub fn category(self) -> TokenCategory {
                match self {
                    $( TokenType::$variant => TokenCategory::$category, )*
                }
            }

            pub fn name(self) -> &'static str {
                match self {
                    $( TokenType::$variant => stringify!($variant), )*
                }
            }
        }
    };
}

define_tokens! {
    Ampersand                  => Operator,
    AmpersandEquals            => Operator,
    Arrow                      => Operator,
    Asterisk                   => Operator,
    AsteriskEquals             => Operator,
    Async                      => Keyword,
    Await                      => Keyword,
    BigIntLiteral              => Number,
    BoolLiteral                => Keyword,
    BracketClose               => Punctuation,
    BracketOpen                => Punctuation,
    Break                      => ControlKeyword,
    Caret                      => Operator,
    CaretEquals                => Operator,
    Case                       => ControlKeyword,
    Catch                      => ControlKeyword,
    Class                      => Keyword,
    Colon                      => Punctuation,
    Comma                      => Punctuation,
    Const                      => Keyword,
    Continue                   => ControlKeyword,
    CurlyClose                 => Punctuation,
    CurlyOpen                  => Punctuation,
    Debugger                   => Keyword,
    Default                    => ControlKeyword,
    Delete                     => Keyword,
    Do                         => ControlKeyword,
    DoubleAmpersand            => Operator,
    DoubleAmpersandEquals      => Operator,
    DoubleAsterisk             => Operator,
    DoubleAsteriskEquals       => Operator,
    DoublePipe                 => Operator,
    DoublePipeEquals           => Operator,
    DoubleQuestionMark         => Operator,
    DoubleQuestionMarkEquals   => Operator,
    Else                       => ControlKeyword,
    Enum                       => Keyword,
    Eof                        => Invalid,
    Equals                     => Operator,
    EqualsEquals               => Operator,
    EqualsEqualsEquals         => Operator,
    EscapedKeyword             => Identifier,
    ExclamationMark            => Operator,
    ExclamationMarkEquals      => Operator,
    ExclamationMarkEqualsEquals => Operator,
    Export                     => Keyword,
    Extends                    => Keyword,
    Finally                    => ControlKeyword,
    For                        => ControlKeyword,
    Function                   => Keyword,
    GreaterThan                => Operator,
    GreaterThanEquals          => Operator,
    Identifier                 => Identifier,
    If                         => ControlKeyword,
    Implements                 => Keyword,
    Import                     => Keyword,
    In                         => Keyword,
    Instanceof                 => Keyword,
    Interface                  => Keyword,
    Invalid                    => Invalid,
    LessThan                   => Operator,
    LessThanEquals             => Operator,
    Let                        => Keyword,
    Minus                      => Operator,
    MinusEquals                => Operator,
    MinusMinus                 => Operator,
    New                        => Keyword,
    NullLiteral                => Keyword,
    NumericLiteral             => Number,
    Package                    => Keyword,
    ParenClose                 => Punctuation,
    ParenOpen                  => Punctuation,
    Percent                    => Operator,
    PercentEquals              => Operator,
    Period                     => Operator,
    Pipe                       => Operator,
    PipeEquals                 => Operator,
    Plus                       => Operator,
    PlusEquals                 => Operator,
    PlusPlus                   => Operator,
    Private                    => Keyword,
    PrivateIdentifier          => Identifier,
    Protected                  => Keyword,
    Public                     => Keyword,
    QuestionMark               => Operator,
    QuestionMarkPeriod         => Operator,
    RegexFlags                 => String,
    RegexLiteral               => String,
    Return                     => ControlKeyword,
    Semicolon                  => Punctuation,
    ShiftLeft                  => Operator,
    ShiftLeftEquals            => Operator,
    ShiftRight                 => Operator,
    ShiftRightEquals           => Operator,
    Slash                      => Operator,
    SlashEquals                => Operator,
    Static                     => Keyword,
    StringLiteral              => String,
    Super                      => Keyword,
    Switch                     => ControlKeyword,
    TemplateLiteralEnd         => String,
    TemplateLiteralExprEnd     => Punctuation,
    TemplateLiteralExprStart   => Punctuation,
    TemplateLiteralStart       => String,
    TemplateLiteralString      => String,
    This                       => Keyword,
    Throw                      => ControlKeyword,
    Tilde                      => Operator,
    TripleDot                  => Operator,
    Trivia                     => Trivia,
    Try                        => ControlKeyword,
    Typeof                     => Keyword,
    UnsignedShiftRight         => Operator,
    UnsignedShiftRightEquals   => Operator,
    UnterminatedRegexLiteral   => String,
    UnterminatedStringLiteral  => String,
    UnterminatedTemplateLiteral => String,
    Var                        => Keyword,
    Void                       => Keyword,
    While                      => ControlKeyword,
    With                       => ControlKeyword,
    Yield                      => ControlKeyword,
}

impl TokenType {
    pub fn is_identifier_name(self) -> bool {
        self != TokenType::PrivateIdentifier
            && matches!(
                self.category(),
                TokenCategory::Identifier | TokenCategory::Keyword | TokenCategory::ControlKeyword
            )
    }
}

#[derive(Debug, Clone)]
pub struct Token {
    pub token_type: TokenType,
    pub trivia_start: u32,
    pub trivia_len: u32,
    pub value_start: u32,
    pub value_len: u32,
    pub line_number: u32,
    pub line_column: u32,
    pub offset: u32,
    pub trivia_has_line_terminator: bool,
    /// Decoded identifier value, set when the identifier contains unicode
    /// escape sequences (e.g. `l\u0065t` → `let`).
    pub identifier_value: Option<crate::ast::Utf16String>,
    /// Error message for Invalid tokens (e.g. "Unterminated multi-line comment").
    pub message: Option<String>,
}

impl Token {
    pub fn new(token_type: TokenType) -> Self {
        Token {
            token_type,
            trivia_start: 0,
            trivia_len: 0,
            value_start: 0,
            value_len: 0,
            line_number: 0,
            line_column: 0,
            offset: 0,
            trivia_has_line_terminator: false,
            identifier_value: None,
            message: None,
        }
    }
}
