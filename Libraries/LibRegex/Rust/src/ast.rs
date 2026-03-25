/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Abstract syntax tree for ECMA-262 regular expressions.
//!
//! The AST is designed to faithfully represent any syntactically valid
//! ECMAScript regex pattern, preserving all information needed for
//! compilation, optimization, and error reporting.
//!
//! Spec:
//! - <https://tc39.es/ecma262/#sec-patterns>
//! - <https://tc39.es/ecma262/#sec-pattern-semantics>

/// A complete parsed regex pattern with its flags.
///
/// Spec model: the `Pattern` grammar goal and the RegExp Record built from it.
/// - <https://tc39.es/ecma262/#sec-patterns>
/// - <https://tc39.es/ecma262/#sec-regexp-records>
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Pattern {
    pub disjunction: Disjunction,
    pub flags: Flags,
    /// Total number of capture groups (not counting group 0).
    pub capture_count: u32,
    /// Named capture groups, in order of appearance.
    pub named_groups: Vec<NamedGroup>,
}

/// A named capture group declaration.
///
/// Spec model: a named `GroupSpecifier` that later participates in named
/// backreference resolution.
/// <https://tc39.es/ecma262/#sec-patterns>
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct NamedGroup {
    pub name: String,
    pub index: u32,
}

/// Regex flags parsed from the trailing `/flags` portion.
///
/// Spec model: the boolean fields in the RegExp Record that affect parsing and
/// matcher semantics.
/// <https://tc39.es/ecma262/#sec-regexp-records>
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct Flags {
    pub global: bool,
    pub ignore_case: bool,
    pub multiline: bool,
    pub dot_all: bool,
    pub unicode: bool,
    pub unicode_sets: bool,
    pub sticky: bool,
    pub has_indices: bool,
}

/// A disjunction is a list of alternatives separated by `|`.
///
/// `/a|b|c/` → `Disjunction { alternatives: [a, b, c] }`
///
/// Spec model: `Disjunction`.
/// <https://tc39.es/ecma262/#sec-compilesubpattern>
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Disjunction {
    pub alternatives: Vec<Alternative>,
}

/// An alternative is a sequence of terms matched left-to-right.
///
/// `/abc/` → `Alternative { terms: [a, b, c] }`
///
/// Spec model: `Alternative`.
/// <https://tc39.es/ecma262/#sec-compilesubpattern>
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Alternative {
    pub terms: Vec<Term>,
}

/// A single term in an alternative: an atom optionally followed by a quantifier.
///
/// Spec model: `Term`.
/// <https://tc39.es/ecma262/#sec-compilesubpattern>
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Term {
    pub atom: Atom,
    pub quantifier: Option<Quantifier>,
}

/// A quantifier specifies repetition of the preceding atom.
///
/// Spec model: the quantifier record produced by `CompileQuantifier`.
/// <https://tc39.es/ecma262/#sec-compilequantifier>
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Quantifier {
    pub min: u32,
    pub max: Option<u32>,
    pub greedy: bool,
}

impl Quantifier {
    pub fn zero_or_more(greedy: bool) -> Self {
        Self {
            min: 0,
            max: None,
            greedy,
        }
    }

    pub fn one_or_more(greedy: bool) -> Self {
        Self {
            min: 1,
            max: None,
            greedy,
        }
    }

    pub fn zero_or_one(greedy: bool) -> Self {
        Self {
            min: 0,
            max: Some(1),
            greedy,
        }
    }
}

/// An atom is the fundamental matching unit.
///
/// Spec model: `Atom` and `AtomEscape`, plus assertion-like terms that are
/// represented as dedicated AST variants for easier downstream handling.
/// - <https://tc39.es/ecma262/#sec-compileatom>
/// - <https://tc39.es/ecma262/#sec-compileassertion>
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Atom {
    /// A literal character (as a u32 code point, to support WTF-16 lone surrogates).
    /// `/a/` → `Literal(0x61)`
    Literal(u32),

    /// `.` — matches any character (except newline, unless `dot_all`).
    Dot,

    /// A character class like `[a-z]` or `[^0-9]`.
    CharacterClass(CharacterClass),

    /// A built-in character class escape: `\d`, `\D`, `\w`, `\W`, `\s`, `\S`.
    BuiltinCharacterClass(BuiltinCharacterClass),

    /// A Unicode property escape: `\p{Letter}`, `\P{Script=Greek}`.
    UnicodeProperty(UnicodeProperty),

    /// A capturing group: `(expr)` or `(?<name>expr)`.
    Group(Group),

    /// A non-capturing group: `(?:expr)`.
    NonCapturingGroup(NonCapturingGroup),

    /// A lookaround assertion: `(?=expr)`, `(?!expr)`, `(?<=expr)`, `(?<!expr)`.
    Lookaround(Lookaround),

    /// A backreference: `\1`, `\k<name>`.
    Backreference(Backreference),

    /// An assertion that matches a position, not a character.
    Assertion(AssertionKind),

    /// A modifier group that changes flags for its contents: `(?i:expr)`.
    ModifierGroup(ModifierGroup),
}

/// A character class: `[abc]`, `[a-z]`, `[^0-9]`.
///
/// Spec model: `CharacterClass`.
/// <https://tc39.es/ecma262/#sec-compilecharacterclass>
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CharacterClass {
    pub negated: bool,
    pub body: CharacterClassBody,
}

/// The body of a character class, which differs between `/u`|`/v` mode and
/// legacy mode.
///
/// Spec model: the split between legacy `ClassContents` and `/v`
/// `ClassSetExpression`.
/// - <https://tc39.es/ecma262/#sec-patterns>
/// - <https://tc39.es/ecma262/#sec-compilecharacterclass>
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum CharacterClassBody {
    /// Legacy or `/u` mode: a flat list of ranges/chars.
    Ranges(Vec<CharacterClassRange>),

    /// `/v` (unicode sets) mode: supports set operations.
    UnicodeSet(ClassSetExpression),
}

/// A single element in a `[...]` character class (legacy and `/u` mode).
///
/// Spec model: the pieces consumed by `CompileToCharSet`.
/// <https://tc39.es/ecma262/#sec-compiletocharset>
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum CharacterClassRange {
    /// A single character (as u32 code point, to support WTF-16 lone surrogates).
    Single(u32),

    /// A character range (as u32 code points).
    Range(u32, u32),

    /// A character class escape inside `[...]`: `[\d]`, `[\w]`.
    BuiltinClass(BuiltinCharacterClass),

    /// A Unicode property escape inside `[...]`: `[\p{Letter}]`.
    UnicodeProperty(UnicodeProperty),
}

/// Set operations for `/v` (unicode sets) mode character classes.
///
/// Spec model: `ClassSetExpression`.
/// <https://tc39.es/ecma262/#sec-compilecharacterclass>
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum ClassSetExpression {
    /// A union of class set operands (the default when just listing items).
    Union(Vec<ClassSetOperand>),

    /// Intersection: `[a-z&&[aeiou]]`.
    Intersection(Vec<ClassSetOperand>),

    /// Subtraction: `[a-z--[aeiou]]`.
    Subtraction(Vec<ClassSetOperand>),
}

/// An operand in a unicode sets class expression.
///
/// Spec model: `ClassSetOperand` and `ClassStringDisjunction`.
/// - <https://tc39.es/ecma262/#sec-patterns>
/// - <https://tc39.es/ecma262/#sec-compileclasssetstring>
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum ClassSetOperand {
    /// A single character.
    Char(char),

    /// A range: `a-z`.
    Range(char, char),

    /// A nested character class: `[a-z]` inside another class.
    NestedClass(CharacterClass),

    /// A builtin class escape: `\d`, `\w`, etc.
    BuiltinClass(BuiltinCharacterClass),

    /// A Unicode property: `\p{Letter}`.
    UnicodeProperty(UnicodeProperty),

    /// A string literal in a class (v-flag): `\q{abc}`.
    StringLiteral(Vec<char>),
}

/// Built-in character class escapes.
///
/// Spec model: `CharacterClassEscape`.
/// <https://tc39.es/ecma262/#sec-compiletocharset>
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum BuiltinCharacterClass {
    /// `\d` — digits `[0-9]`.
    Digit,
    /// `\D` — non-digits `[^0-9]`.
    NonDigit,
    /// `\w` — word chars `[a-zA-Z0-9_]`.
    Word,
    /// `\W` — non-word chars `[^a-zA-Z0-9_]`.
    NonWord,
    /// `\s` — whitespace.
    Whitespace,
    /// `\S` — non-whitespace.
    NonWhitespace,
}

impl BuiltinCharacterClass {
    /// Return the complementary (negated) class.
    pub fn negated(self) -> Self {
        match self {
            Self::Digit => Self::NonDigit,
            Self::NonDigit => Self::Digit,
            Self::Word => Self::NonWord,
            Self::NonWord => Self::Word,
            Self::Whitespace => Self::NonWhitespace,
            Self::NonWhitespace => Self::Whitespace,
        }
    }
}

/// A Unicode property escape: `\p{...}` or `\P{...}`.
///
/// Spec model: `UnicodePropertyValueExpression` and the property matching
/// abstract operations used to validate it.
/// - <https://tc39.es/ecma262/#sec-compiletocharset>
/// - <https://tc39.es/ecma262/#sec-runtime-semantics-unicodematchproperty-p>
/// - <https://tc39.es/ecma262/#sec-runtime-semantics-unicodematchpropertyvalue-p-v>
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct UnicodeProperty {
    /// Whether this is `\P{...}` (negated) or `\p{...}`.
    pub negated: bool,
    /// The property name, e.g. `"Letter"`, `"Script"`.
    pub name: String,
    /// The property value, if present, e.g. `"Greek"` in `\p{Script=Greek}`.
    pub value: Option<String>,
}

/// A capturing group: `(expr)` or `(?<name>expr)`.
///
/// Spec model: a capturing `Atom` with an optional `GroupSpecifier`.
/// <https://tc39.es/ecma262/#sec-compileatom>
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Group {
    /// 1-based capture index.
    pub index: u32,
    /// Optional group name for `(?<name>...)`.
    pub name: Option<String>,
    pub body: Disjunction,
}

/// A non-capturing group: `(?:expr)`.
///
/// Spec model: the `(?: Disjunction )` branch of `Atom`.
/// <https://tc39.es/ecma262/#sec-compileatom>
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct NonCapturingGroup {
    pub body: Disjunction,
}

/// A lookaround assertion.
///
/// Spec model: lookahead and lookbehind forms handled by
/// `Runtime Semantics: CompileAssertion`.
/// <https://tc39.es/ecma262/#sec-compileassertion>
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Lookaround {
    pub kind: LookaroundKind,
    pub body: Disjunction,
}

/// Spec model: the four lookaround assertion forms.
/// <https://tc39.es/ecma262/#sec-compileassertion>
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum LookaroundKind {
    /// `(?=...)` — positive lookahead.
    LookaheadPositive,
    /// `(?!...)` — negative lookahead.
    LookaheadNegative,
    /// `(?<=...)` — positive lookbehind.
    LookbehindPositive,
    /// `(?<!...)` — negative lookbehind.
    LookbehindNegative,
}

/// A backreference to a capture group.
///
/// Spec model: decimal and named backreference atoms.
/// - <https://tc39.es/ecma262/#sec-compileatom>
/// - <https://tc39.es/ecma262/#sec-backreference-matcher>
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Backreference {
    /// `\1`, `\2`, etc. — numeric backreference.
    Index(u32),
    /// `\k<name>` — named backreference.
    Named(String),
}

/// A zero-width assertion.
///
/// Spec model: the zero-width assertion terms compiled by
/// `Runtime Semantics: CompileAssertion`.
/// <https://tc39.es/ecma262/#sec-compileassertion>
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum AssertionKind {
    /// `^` — start of input (or line in multiline mode).
    StartOfInput,
    /// `$` — end of input (or line in multiline mode).
    EndOfInput,
    /// `\b` — word boundary.
    WordBoundary,
    /// `\B` — non-word boundary.
    NonWordBoundary,
}

/// A modifier group: `(?imsu-imsu:expr)`.
///
/// Spec model: modifier-scoped `Atom` forms that call `UpdateModifiers`.
/// - <https://tc39.es/ecma262/#sec-compileatom>
/// - <https://tc39.es/ecma262/#sec-updatemodifiers>
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ModifierGroup {
    pub add_flags: ModifierFlags,
    pub remove_flags: ModifierFlags,
    pub body: Disjunction,
}

/// Flags that can be toggled in a modifier group.
///
/// Spec model: the subset of flags accepted by `UpdateModifiers` inside
/// a modifier group.
/// <https://tc39.es/ecma262/#sec-updatemodifiers>
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct ModifierFlags {
    pub ignore_case: bool,
    pub multiline: bool,
    pub dot_all: bool,
}
