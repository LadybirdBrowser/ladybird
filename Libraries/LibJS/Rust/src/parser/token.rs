/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Token kinds, and the names that have fixed ids so that a token's kind falls out of interning its text.

#[repr(u8)]
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
#[rustfmt::skip]
pub enum T {
    Eof, Name, PrivateName, Num, BigInt, Str, Backtick, Regex, EscapedKeyword, Invalid,
    LBrace, RBrace, LParen, RParen, LBracket, RBracket, Semi, Comma, Dot, QDot, Question, Colon, Arrow, Ellipsis,
    // Assignment operators, in the order of the assignment node tags.
    Assign, AddAssign, SubAssign, MulAssign, DivAssign, ModAssign, ExpAssign, ShlAssign, ShrAssign, UShrAssign,
    AndAssign, OrAssign, XorAssign, LogAndAssign, LogOrAssign, NullishAssign,
    Inc, Dec, Not, Tilde,
    // Binary operators, in the order of the binary node tags.
    Nullish, LogOr, LogAnd, BitOr, BitXor, BitAnd, EqEq, NotEq, EqEqEq, NotEqEq, Lt, Gt, Le, Ge, Shl, Shr, UShr,
    Plus, Minus, Star, Slash, Percent, StarStar, KIn, KInstanceof,
    // Keywords.
    KBreak, KCase, KCatch, KClass, KConst, KContinue, KDebugger, KDefault, KDelete, KDo, KElse, KEnum, KExport,
    KExtends, KFalse, KFinally, KFor, KFunction, KIf, KImport, KNew, KNull, KReturn, KSuper, KSwitch, KThis,
    KThrow, KTrue, KTry, KTypeof, KVar, KVoid, KWhile, KWith,
}

/// What error messages call a token kind.
#[rustfmt::skip]
const TOKEN_NAMES: &[&str] = &[
    "Eof", "Identifier", "PrivateIdentifier", "NumericLiteral", "BigIntLiteral", "StringLiteral", "TemplateLiteralStart",
    "RegexLiteral", "EscapedKeyword", "Invalid",
    "CurlyOpen", "CurlyClose", "ParenOpen", "ParenClose", "BracketOpen", "BracketClose", "Semicolon", "Comma", "Period",
    "QuestionMarkPeriod", "QuestionMark", "Colon", "Arrow", "TripleDot",
    "Equals", "PlusEquals", "MinusEquals", "AsteriskEquals", "SlashEquals", "PercentEquals", "DoubleAsteriskEquals",
    "ShiftLeftEquals", "ShiftRightEquals", "UnsignedShiftRightEquals", "AmpersandEquals", "PipeEquals", "CaretEquals",
    "DoubleAmpersandEquals", "DoublePipeEquals", "DoubleQuestionMarkEquals",
    "PlusPlus", "MinusMinus", "ExclamationMark", "Tilde",
    "DoubleQuestionMark", "DoublePipe", "DoubleAmpersand", "Pipe", "Caret", "Ampersand", "EqualsEquals",
    "ExclamationMarkEquals", "EqualsEqualsEquals", "ExclamationMarkEqualsEquals", "LessThan", "GreaterThan",
    "LessThanEquals", "GreaterThanEquals", "ShiftLeft", "ShiftRight", "UnsignedShiftRight", "Plus", "Minus", "Asterisk",
    "Slash", "Percent", "DoubleAsterisk", "In", "Instanceof",
    "Break", "Case", "Catch", "Class", "Const", "Continue", "Debugger", "Default", "Delete", "Do", "Else", "Enum", "Export",
    "Extends", "BoolLiteral", "Finally", "For", "Function", "If", "Import", "New", "NullLiteral", "Return", "Super", "Switch",
    "This", "Throw", "BoolLiteral", "Try", "Typeof", "Var", "Void", "While", "With",
];

impl T {
    /// How error messages spell a token that the grammar wants.
    pub fn spelling(self) -> &'static str {
        match self {
            T::LBrace => "'{'",
            T::RBrace => "'}'",
            T::LParen => "'('",
            T::RParen => "')'",
            T::LBracket => "'['",
            T::RBracket => "']'",
            T::Semi => "';'",
            T::Comma => "','",
            T::Colon => "':'",
            T::Arrow => "'=>'",
            T::Assign => "'='",
            T::KWhile => "'while'",
            _ => TOKEN_NAMES[self as usize],
        }
    }
}

/// The reserved words. A name's id is its index here, so ids below `KEYWORD_COUNT` are keywords.
#[rustfmt::skip]
pub const KEYWORDS: [(&str, T); 36] = [
    ("in", T::KIn), ("instanceof", T::KInstanceof),
    ("break", T::KBreak), ("case", T::KCase), ("catch", T::KCatch), ("class", T::KClass), ("const", T::KConst),
    ("continue", T::KContinue), ("debugger", T::KDebugger), ("default", T::KDefault), ("delete", T::KDelete),
    ("do", T::KDo), ("else", T::KElse), ("enum", T::KEnum), ("export", T::KExport), ("extends", T::KExtends),
    ("false", T::KFalse), ("finally", T::KFinally), ("for", T::KFor), ("function", T::KFunction), ("if", T::KIf),
    ("import", T::KImport), ("new", T::KNew), ("null", T::KNull), ("return", T::KReturn), ("super", T::KSuper),
    ("switch", T::KSwitch), ("this", T::KThis), ("throw", T::KThrow), ("true", T::KTrue), ("try", T::KTry),
    ("typeof", T::KTypeof), ("var", T::KVar), ("void", T::KVoid), ("while", T::KWhile), ("with", T::KWith),
];
pub const KEYWORD_COUNT: u32 = KEYWORDS.len() as u32;

/// Names the grammar looks at, after the keywords. Every id from `FIRST_PLAIN_NAME` on is an ordinary name, so one
/// compare clears an identifier of all reserved word checks.
#[rustfmt::skip]
pub const SPECIAL_NAMES: [&str; 27] = [
    "implements", "interface", "let", "package", "private", "protected", "public", "static", "yield",
    "await", "eval", "arguments", "async", "of", "get", "set", "as", "from", "target", "meta", "constructor",
    "prototype", "__proto__", "using", "accessor", "defer", "source",
];
pub const N_IMPLEMENTS: u32 = KEYWORD_COUNT;
pub const N_LET: u32 = KEYWORD_COUNT + 2;
pub const N_STATIC: u32 = KEYWORD_COUNT + 7;
pub const N_YIELD: u32 = KEYWORD_COUNT + 8;
pub const N_AWAIT: u32 = KEYWORD_COUNT + 9;
pub const N_EVAL: u32 = KEYWORD_COUNT + 10;
pub const N_ARGUMENTS: u32 = KEYWORD_COUNT + 11;
pub const N_ASYNC: u32 = KEYWORD_COUNT + 12;
pub const N_OF: u32 = KEYWORD_COUNT + 13;
pub const N_GET: u32 = KEYWORD_COUNT + 14;
pub const N_SET: u32 = KEYWORD_COUNT + 15;
pub const N_AS: u32 = KEYWORD_COUNT + 16;
pub const N_FROM: u32 = KEYWORD_COUNT + 17;
pub const N_TARGET: u32 = KEYWORD_COUNT + 18;
pub const N_META: u32 = KEYWORD_COUNT + 19;
pub const N_CONSTRUCTOR: u32 = KEYWORD_COUNT + 20;
pub const N_PROTOTYPE: u32 = KEYWORD_COUNT + 21;
pub const N_PROTO: u32 = KEYWORD_COUNT + 22;
pub const N_USING: u32 = KEYWORD_COUNT + 23;
/// Pseudo names: they cannot be spelled, and `this`, `new.target` and `super` use them to flow through scope analysis
/// as ordinary free names, which is what makes an arrow function's use of them a capture.
pub const N_DOT_THIS: u32 = KEYWORD_COUNT + SPECIAL_NAMES.len() as u32;
pub const N_DOT_NEW_TARGET: u32 = N_DOT_THIS + 1;
pub const N_DOT_SUPER: u32 = N_DOT_THIS + 2;
/// What a function that gets compiled later would make of a name that no scope around it binds.
pub const N_DOT_UNBOUND: u32 = N_DOT_THIS + 3;
pub const FIRST_PLAIN_NAME: u32 = N_DOT_THIS + 4;

/// Binary operator precedence by token kind, zero for everything else.
pub const fn build_precedence() -> [u8; 256] {
    let mut p = [0u8; 256];
    p[T::Nullish as usize] = 1;
    p[T::LogOr as usize] = 1;
    p[T::LogAnd as usize] = 2;
    p[T::BitOr as usize] = 3;
    p[T::BitXor as usize] = 4;
    p[T::BitAnd as usize] = 5;
    p[T::EqEq as usize] = 6;
    p[T::NotEq as usize] = 6;
    p[T::EqEqEq as usize] = 6;
    p[T::NotEqEq as usize] = 6;
    p[T::Lt as usize] = 7;
    p[T::Gt as usize] = 7;
    p[T::Le as usize] = 7;
    p[T::Ge as usize] = 7;
    p[T::KIn as usize] = 7;
    p[T::KInstanceof as usize] = 7;
    p[T::Shl as usize] = 8;
    p[T::Shr as usize] = 8;
    p[T::UShr as usize] = 8;
    p[T::Plus as usize] = 9;
    p[T::Minus as usize] = 9;
    p[T::Star as usize] = 10;
    p[T::Slash as usize] = 10;
    p[T::Percent as usize] = 10;
    p
}
pub static PRECEDENCE: [u8; 256] = build_precedence();
