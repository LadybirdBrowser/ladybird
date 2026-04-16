/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! ECMA-262 regular expression parser.
//!
//! Parses a regex pattern string into an [`ast::Pattern`].
//!
//! Spec:
//! - <https://tc39.es/ecma262/#sec-patterns>
//! - <https://tc39.es/ecma262/#sec-parsepattern>
use crate::ast::*;

const MAX_BRACED_QUANTIFIER: u32 = i32::MAX as u32;

/// Parse a regex pattern string with the given flags.
///
/// Spec entry point: `ParsePattern`.
/// <https://tc39.es/ecma262/#sec-parsepattern>
pub fn parse(source: &str, flags: Flags) -> Result<Pattern, Error> {
    Parser::new(source, flags).parse()
}

/// Parse a flags string like `"gimsu"` into [`Flags`].
pub fn parse_flags(s: &str) -> Result<Flags, Error> {
    let mut flags = Flags::default();
    for ch in s.chars() {
        match ch {
            'g' if !flags.global => flags.global = true,
            'i' if !flags.ignore_case => flags.ignore_case = true,
            'm' if !flags.multiline => flags.multiline = true,
            's' if !flags.dot_all => flags.dot_all = true,
            'u' if !flags.unicode && !flags.unicode_sets => flags.unicode = true,
            'v' if !flags.unicode_sets && !flags.unicode => flags.unicode_sets = true,
            'y' if !flags.sticky => flags.sticky = true,
            'd' if !flags.has_indices => flags.has_indices = true,
            _ => return Err(Error::InvalidFlags(s.to_string())),
        }
    }
    Ok(flags)
}

/// A parser error with a position in the source string.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Error {
    UnexpectedEnd,
    UnexpectedChar(char),
    InvalidEscape(char),
    InvalidFlags(String),
    InvalidQuantifier,
    InvalidCharacterClass,
    InvalidCharacterRange(u32, u32),
    InvalidUnicodeEscape,
    InvalidUnicodeProperty(String),
    InvalidBackreference(u32),
    InvalidNamedBackreference(String),
    InvalidGroupName,
    InvalidModifierFlags,
    DuplicateGroupName(String),
    DuplicateFlag(char),
    LoneTrailingBackslash,
    UnmatchedParen,
    QuantifierWithoutAtom,
    NothingToRepeat,
    NumberOverflow,
}

impl std::fmt::Display for Error {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::UnexpectedEnd => write!(f, "unexpected end of pattern"),
            Self::UnexpectedChar(c) => write!(f, "unexpected character '{c}'"),
            Self::InvalidEscape(c) => write!(f, "invalid escape '\\{c}'"),
            Self::InvalidFlags(s) => write!(f, "invalid flags \"{s}\""),
            Self::InvalidQuantifier => write!(f, "invalid quantifier"),
            Self::InvalidCharacterClass => write!(f, "invalid character class"),
            Self::InvalidCharacterRange(lo, hi) => {
                let lo_str = char::from_u32(*lo)
                    .map_or_else(|| format!("\\u{{{lo:04X}}}"), |c| format!("{c}"));
                let hi_str = char::from_u32(*hi)
                    .map_or_else(|| format!("\\u{{{hi:04X}}}"), |c| format!("{c}"));
                write!(f, "invalid character range '{lo_str}'-'{hi_str}'")
            }
            Self::InvalidUnicodeEscape => write!(f, "invalid Unicode escape"),
            Self::InvalidUnicodeProperty(p) => write!(f, "invalid Unicode property '{p}'"),
            Self::InvalidBackreference(n) => write!(f, "invalid backreference \\{n}"),
            Self::InvalidNamedBackreference(n) => {
                write!(f, "invalid named backreference \\k<{n}>")
            }
            Self::InvalidGroupName => write!(f, "invalid group name"),
            Self::InvalidModifierFlags => write!(f, "invalid modifier flags"),
            Self::DuplicateGroupName(n) => write!(f, "duplicate group name '{n}'"),
            Self::DuplicateFlag(c) => write!(f, "duplicate flag '{c}'"),
            Self::LoneTrailingBackslash => write!(f, "lone trailing backslash"),
            Self::UnmatchedParen => write!(f, "unmatched parenthesis"),
            Self::QuantifierWithoutAtom => write!(f, "quantifier without atom"),
            Self::NothingToRepeat => write!(f, "nothing to repeat"),
            Self::NumberOverflow => write!(f, "number overflow"),
        }
    }
}

impl std::error::Error for Error {}

struct Parser {
    /// Source pattern as chars for easy random access.
    source: Vec<char>,
    /// Current position in `source`.
    pos: usize,
    /// Parsed flags.
    flags: Flags,
    /// Next capture group index (1-based).
    next_capture_index: u32,
    /// Total number of capture groups in the pattern (from pre-scan).
    total_capture_count: u32,
    /// Whether any named groups exist in the pattern (from pre-scan).
    has_any_named_groups: bool,
    /// Named capture groups seen so far.
    named_groups: Vec<NamedGroup>,
    /// Stack of name sets for duplicate detection across alternatives.
    /// Each entry is the set of names used in the current alternative
    /// at that disjunction nesting level.
    alternative_name_stack: Vec<std::collections::HashSet<String>>,
    /// True when parsing inside a negated character class (`[^...]`).
    in_negated_class: bool,
}

impl Parser {
    fn new(source: &str, flags: Flags) -> Self {
        let chars: Vec<char> = source.chars().collect();
        let (total, has_named) = Self::prescan_captures(&chars);
        Self {
            source: chars,
            pos: 0,
            flags,
            next_capture_index: 1,
            total_capture_count: total,
            has_any_named_groups: has_named,
            named_groups: Vec::new(),
            alternative_name_stack: Vec::new(),
            in_negated_class: false,
        }
    }

    /// Quick pre-scan to count total capturing groups and detect named groups.
    /// Returns `(capture_count, has_named_groups)`.
    ///
    /// We intentionally do this before full parsing so Annex B decimal escapes
    /// can decide between backreference and legacy octal using the total number
    /// of capturing groups, including forward references.
    fn prescan_captures(source: &[char]) -> (u32, bool) {
        let mut count = 0u32;
        let mut has_named = false;
        let mut i = 0;
        let mut in_char_class = false;
        while i < source.len() {
            match source[i] {
                '\\' => {
                    i += 2;
                    continue;
                }
                '[' if !in_char_class => {
                    in_char_class = true;
                }
                ']' if in_char_class => {
                    in_char_class = false;
                }
                '(' if !in_char_class => {
                    if i + 1 < source.len() && source[i + 1] == '?' {
                        if i + 2 < source.len()
                            && source[i + 2] == '<'
                            && (i + 3 >= source.len()
                                || (source[i + 3] != '=' && source[i + 3] != '!'))
                        {
                            count += 1;
                            has_named = true;
                        }
                    } else {
                        count += 1;
                    }
                }
                _ => {}
            }
            i += 1;
        }
        (count, has_named)
    }

    fn parse(mut self) -> Result<Pattern, Error> {
        let disjunction = self.parse_disjunction()?;
        if self.pos < self.source.len() {
            return Err(Error::UnexpectedChar(self.source[self.pos]));
        }

        // Named backreferences can point forward, so validate them only after
        // the whole pattern has been parsed and every group name is known.
        let group_names: std::collections::HashSet<&str> =
            self.named_groups.iter().map(|g| g.name.as_str()).collect();
        Self::validate_backrefs(&disjunction, &group_names)?;

        Ok(Pattern {
            disjunction,
            flags: self.flags,
            capture_count: self.next_capture_index - 1,
            named_groups: self.named_groups,
        })
    }

    /// Apply the named-backreference early error after all group names have
    /// been collected.
    /// <https://tc39.es/ecma262/#sec-patterns-static-semantics-early-errors>
    fn validate_backrefs(
        disj: &Disjunction,
        group_names: &std::collections::HashSet<&str>,
    ) -> Result<(), Error> {
        for alt in &disj.alternatives {
            for term in &alt.terms {
                match &term.atom {
                    Atom::Backreference(Backreference::Named(name))
                        if !group_names.contains(name.as_str()) =>
                    {
                        return Err(Error::InvalidNamedBackreference(name.clone()));
                    }
                    Atom::Group(g) => Self::validate_backrefs(&g.body, group_names)?,
                    Atom::NonCapturingGroup(g) => Self::validate_backrefs(&g.body, group_names)?,
                    Atom::Lookaround(la) => Self::validate_backrefs(&la.body, group_names)?,
                    Atom::ModifierGroup(mg) => Self::validate_backrefs(&mg.body, group_names)?,
                    _ => {}
                }
            }
        }
        Ok(())
    }

    // ---------------------------------------------------------------
    // Character access helpers
    // ---------------------------------------------------------------

    fn peek(&self) -> Option<char> {
        self.source.get(self.pos).copied()
    }

    fn peek_at(&self, offset: usize) -> Option<char> {
        self.source.get(self.pos + offset).copied()
    }

    fn advance(&mut self) -> Option<char> {
        let ch = self.source.get(self.pos).copied()?;
        self.pos += 1;
        Some(ch)
    }

    fn eat(&mut self, expected: char) -> bool {
        if self.peek() == Some(expected) {
            self.pos += 1;
            true
        } else {
            false
        }
    }

    fn expect(&mut self, expected: char) -> Result<(), Error> {
        if self.eat(expected) {
            Ok(())
        } else {
            match self.peek() {
                Some(c) => Err(Error::UnexpectedChar(c)),
                None => Err(Error::UnexpectedEnd),
            }
        }
    }

    fn at_end(&self) -> bool {
        self.pos >= self.source.len()
    }

    /// Whether we're in Unicode-aware mode (`/u` or `/v`).
    fn unicode_aware(&self) -> bool {
        self.flags.unicode || self.flags.unicode_sets
    }

    // ---------------------------------------------------------------
    // Disjunction: Alternative ('|' Alternative)*
    // ---------------------------------------------------------------

    /// Parse `Disjunction`.
    /// <https://tc39.es/ecma262/#sec-patterns>
    fn parse_disjunction(&mut self) -> Result<Disjunction, Error> {
        self.alternative_name_stack
            .push(std::collections::HashSet::new());
        let mut alternatives = vec![self.parse_alternative()?];
        while self.eat('|') {
            // Reset the current alternative's name set for the next alternative.
            if let Some(names) = self.alternative_name_stack.last_mut() {
                names.clear();
            }
            alternatives.push(self.parse_alternative()?);
        }
        self.alternative_name_stack.pop();
        Ok(Disjunction { alternatives })
    }

    // ---------------------------------------------------------------
    // Alternative: Term*
    // ---------------------------------------------------------------

    /// Parse `Alternative`.
    /// <https://tc39.es/ecma262/#sec-patterns>
    fn parse_alternative(&mut self) -> Result<Alternative, Error> {
        let mut terms = Vec::new();
        while let Some(term) = self.try_parse_term()? {
            terms.push(term);
        }
        Ok(Alternative { terms })
    }

    // ---------------------------------------------------------------
    // Term: Atom Quantifier?
    // ---------------------------------------------------------------

    /// Parse `Term`, including its optional trailing quantifier.
    /// <https://tc39.es/ecma262/#sec-compilesubpattern>
    fn try_parse_term(&mut self) -> Result<Option<Term>, Error> {
        let Some(atom) = self.try_parse_atom()? else {
            return Ok(None);
        };

        // Check if the atom can be quantified.
        // In Unicode mode, no assertions or lookarounds are quantifiable.
        // In non-Unicode mode (Annex B), only lookaheads are quantifiable.
        // Lookbehinds and simple assertions (\b, \B, ^, $) are never quantifiable.
        let quantifiable = match &atom {
            Atom::Assertion(_) => false,
            Atom::Lookaround(_) if self.unicode_aware() => false,
            Atom::Lookaround(la) => matches!(
                la.kind,
                LookaroundKind::LookaheadPositive | LookaroundKind::LookaheadNegative
            ),
            _ => true,
        };

        let quantifier = if quantifiable {
            self.try_parse_quantifier()?
        } else if self.peek_is_quantifier() {
            return Err(Error::NothingToRepeat);
        } else {
            None
        };
        Ok(Some(Term { atom, quantifier }))
    }

    /// Check if the next character starts a quantifier.
    fn peek_is_quantifier(&self) -> bool {
        match self.peek() {
            Some('*' | '+' | '?') => true,
            Some('{') => self.looks_like_quantifier(),
            _ => false,
        }
    }

    // ---------------------------------------------------------------
    // Quantifier: ('*' | '+' | '?' | '{' ...) '?'?
    // ---------------------------------------------------------------

    /// Parse `Quantifier`.
    /// <https://tc39.es/ecma262/#sec-compilequantifier>
    fn try_parse_quantifier(&mut self) -> Result<Option<Quantifier>, Error> {
        let Some(ch) = self.peek() else {
            return Ok(None);
        };

        let quantifier = match ch {
            '*' => {
                self.advance();
                let greedy = !self.eat('?');
                Quantifier::zero_or_more(greedy)
            }
            '+' => {
                self.advance();
                let greedy = !self.eat('?');
                Quantifier::one_or_more(greedy)
            }
            '?' => {
                self.advance();
                let greedy = !self.eat('?');
                Quantifier::zero_or_one(greedy)
            }
            '{' => {
                if let Some(q) = self.try_parse_braced_quantifier()? {
                    q
                } else {
                    return Ok(None);
                }
            }
            _ => return Ok(None),
        };

        Ok(Some(quantifier))
    }

    /// Try to parse `{n}`, `{n,}`, or `{n,m}`.
    /// Returns `None` if the `{...}` isn't a valid quantifier (in which case
    /// the `{` is treated as a literal in non-unicode mode).
    fn try_parse_braced_quantifier(&mut self) -> Result<Option<Quantifier>, Error> {
        let start = self.pos;
        self.pos += 1; // skip '{'

        let min = match self.try_parse_decimal()? {
            Some(n) => n,
            None => {
                // Not a valid quantifier -- rewind.
                self.pos = start;
                if self.unicode_aware() {
                    return Err(Error::InvalidQuantifier);
                }
                return Ok(None);
            }
        };

        let max = if self.eat(',') {
            self.try_parse_decimal()?
        } else {
            Some(min) // {n} means exactly n
        };

        if !self.eat('}') {
            // Rewind — not a valid quantifier.
            self.pos = start;
            if self.unicode_aware() {
                return Err(Error::InvalidQuantifier);
            }
            return Ok(None);
        }

        // Validate: max >= min if both present.
        if let Some(max_val) = max
            && max_val < min
        {
            return Err(Error::InvalidQuantifier);
        }

        let greedy = !self.eat('?');
        Ok(Some(Quantifier { min, max, greedy }))
    }

    /// Try to parse a decimal integer. Returns:
    /// - `Ok(None)` if no digits were found
    /// - `Ok(Some(n))` if a valid u32 was parsed
    ///
    /// Clamp braced quantifier bounds to 2^31 - 1 to match browser behavior.
    fn try_parse_decimal(&mut self) -> Result<Option<u32>, Error> {
        let start = self.pos;
        let mut value: u32 = 0;
        while let Some(ch) = self.peek() {
            if let Some(digit) = ch.to_digit(10) {
                self.pos += 1;
                value = value.saturating_mul(10).saturating_add(digit);
                value = value.min(MAX_BRACED_QUANTIFIER);
            } else {
                break;
            }
        }
        if self.pos == start {
            Ok(None)
        } else {
            Ok(Some(value))
        }
    }

    // ---------------------------------------------------------------
    // Atom
    // ---------------------------------------------------------------

    /// Parse an `Atom`, `Assertion`, or literal source character.
    /// - <https://tc39.es/ecma262/#sec-compileatom>
    /// - <https://tc39.es/ecma262/#sec-compileassertion>
    fn try_parse_atom(&mut self) -> Result<Option<Atom>, Error> {
        let Some(ch) = self.peek() else {
            return Ok(None);
        };

        match ch {
            // These end the current alternative or group.
            ')' | '|' => Ok(None),

            // Quantifiers without a preceding atom.
            '*' | '+' | '?' => Err(Error::NothingToRepeat),

            // InvalidBracedQuantifier: `{n}`, `{n,}`, `{n,m}` without a preceding atom
            // is an error in both unicode and non-unicode mode (Annex B).
            '{' if self.looks_like_quantifier() => Err(Error::NothingToRepeat),

            // In unicode mode, bare `{` is always an error.
            '{' if self.unicode_aware() => Err(Error::UnexpectedChar('{')),

            // Closing bracket is an error in unicode mode.
            ']' if self.unicode_aware() => Err(Error::UnexpectedChar(']')),
            '}' if self.unicode_aware() => Err(Error::UnexpectedChar('}')),

            '.' => {
                self.advance();
                Ok(Some(Atom::Dot))
            }

            '^' => {
                self.advance();
                Ok(Some(Atom::Assertion(AssertionKind::StartOfInput)))
            }

            '$' => {
                self.advance();
                Ok(Some(Atom::Assertion(AssertionKind::EndOfInput)))
            }

            '\\' => self.parse_atom_escape().map(Some),

            '[' => self
                .parse_character_class()
                .map(|cc| Some(Atom::CharacterClass(cc))),

            '(' => self.parse_group().map(Some),

            // Literal character.
            _ => {
                self.advance();
                Ok(Some(Atom::Literal(ch as u32)))
            }
        }
    }

    /// Check if the current position looks like a braced quantifier `{n}`, `{n,}`, `{n,m}`.
    fn looks_like_quantifier(&self) -> bool {
        if self.peek() != Some('{') {
            return false;
        }
        let mut i = self.pos + 1;
        // Must have at least one digit.
        if i >= self.source.len() || !self.source[i].is_ascii_digit() {
            return false;
        }
        while i < self.source.len() && self.source[i].is_ascii_digit() {
            i += 1;
        }
        if i >= self.source.len() {
            return false;
        }
        if self.source[i] == '}' {
            return true;
        }
        if self.source[i] == ',' {
            i += 1;
            // Optional digits after comma.
            while i < self.source.len() && self.source[i].is_ascii_digit() {
                i += 1;
            }
            if i < self.source.len() && self.source[i] == '}' {
                return true;
            }
        }
        false
    }

    // ---------------------------------------------------------------
    // Escape sequences
    // ---------------------------------------------------------------

    /// Parse `AtomEscape`.
    /// <https://tc39.es/ecma262/#sec-compileatom>
    fn parse_atom_escape(&mut self) -> Result<Atom, Error> {
        self.expect('\\')?;
        let ch = self.advance().ok_or(Error::LoneTrailingBackslash)?;

        match ch {
            // Character class escapes.
            'd' => Ok(Atom::BuiltinCharacterClass(BuiltinCharacterClass::Digit)),
            'D' => Ok(Atom::BuiltinCharacterClass(BuiltinCharacterClass::NonDigit)),
            'w' => Ok(Atom::BuiltinCharacterClass(BuiltinCharacterClass::Word)),
            'W' => Ok(Atom::BuiltinCharacterClass(BuiltinCharacterClass::NonWord)),
            's' => Ok(Atom::BuiltinCharacterClass(
                BuiltinCharacterClass::Whitespace,
            )),
            'S' => Ok(Atom::BuiltinCharacterClass(
                BuiltinCharacterClass::NonWhitespace,
            )),

            // Assertions.
            'b' => Ok(Atom::Assertion(AssertionKind::WordBoundary)),
            'B' => Ok(Atom::Assertion(AssertionKind::NonWordBoundary)),

            // Unicode property escapes.
            'p' | 'P' if self.unicode_aware() => {
                let negated = ch == 'P';
                let prop = self.parse_unicode_property()?;
                // String properties (e.g. Basic_Emoji) are only valid with /v flag,
                // and cannot be negated with \P.
                if prop.1.is_none() && Self::is_string_property(&prop.0) {
                    if !self.flags.unicode_sets {
                        return Err(Error::InvalidUnicodeProperty(prop.0));
                    }
                    if negated {
                        return Err(Error::InvalidUnicodeProperty(prop.0));
                    }
                }
                Ok(Atom::UnicodeProperty(UnicodeProperty {
                    negated,
                    name: prop.0,
                    value: prop.1,
                }))
            }

            // Backreferences.
            '1'..='9' if self.is_backreference_context(ch) => {
                // Consume all consecutive digits to form the backreference number.
                let backref_start = self.pos;
                let mut n = ch.to_digit(10).unwrap();
                let mut overflowed = false;
                while let Some(d) = self.peek().and_then(|c| c.to_digit(10)) {
                    match n.checked_mul(10).and_then(|v| v.checked_add(d)) {
                        Some(val) => n = val,
                        None => overflowed = true,
                    }
                    self.advance();
                }
                if self.unicode_aware() {
                    // In unicode mode, backreference to non-existent group is an error.
                    if overflowed || n > self.total_capture_count {
                        return Err(Error::InvalidBackreference(n));
                    }
                    Ok(Atom::Backreference(Backreference::Index(n)))
                } else if overflowed || n > self.total_capture_count {
                    // Per Annex B, if the consumed number exceeds the total
                    // capture count, reinterpret as a legacy octal escape.
                    self.pos = backref_start;
                    if ch >= '8' {
                        Ok(Atom::Literal(ch as u32))
                    } else {
                        let value = self.parse_legacy_octal(ch);
                        Ok(Atom::Literal(value as u32))
                    }
                } else {
                    Ok(Atom::Backreference(Backreference::Index(n)))
                }
            }

            // Named backreference.
            'k' if self.has_named_groups_or_unicode() => {
                self.expect('<')?;
                let name = self.parse_group_name()?;
                self.expect('>')?;
                Ok(Atom::Backreference(Backreference::Named(name)))
            }

            // Character escapes.
            'n' => Ok(Atom::Literal('\n' as u32)),
            'r' => Ok(Atom::Literal('\r' as u32)),
            't' => Ok(Atom::Literal('\t' as u32)),
            'f' => Ok(Atom::Literal('\u{0C}' as u32)),
            'v' => Ok(Atom::Literal('\u{0B}' as u32)),

            // NUL character.
            '0' if !self.peek().is_some_and(|c| c.is_ascii_digit()) => Ok(Atom::Literal(0)),

            // Control character escape \cA-\cZ.
            'c' => {
                if let Some(ctrl) = self.advance() {
                    if ctrl.is_ascii_alphabetic() {
                        Ok(Atom::Literal(((ctrl as u8) & 0x1f) as u32))
                    } else if self.unicode_aware() {
                        Err(Error::InvalidEscape('c'))
                    } else {
                        // In legacy mode, \c followed by non-alpha is literal backslash.
                        // Put back both the non-alpha char and 'c' so they are parsed
                        // as subsequent literals.
                        self.pos -= 2;
                        Ok(Atom::Literal('\\' as u32))
                    }
                } else if self.unicode_aware() {
                    Err(Error::InvalidEscape('c'))
                } else {
                    // \c at end of pattern in legacy mode: literal backslash.
                    // Put back 'c' so it's parsed as the next literal.
                    self.pos -= 1;
                    Ok(Atom::Literal('\\' as u32))
                }
            }

            // Hex escape \xHH.
            'x' => {
                if self.unicode_aware() {
                    let value = self.parse_hex_escape(2)?;
                    Ok(Atom::Literal(value))
                } else {
                    // In non-unicode mode, incomplete \x is treated as literal.
                    let saved = self.pos;
                    match self.parse_hex_escape(2) {
                        Ok(value) => Ok(Atom::Literal(value)),
                        Err(_) => {
                            self.pos = saved;
                            Ok(Atom::Literal('x' as u32))
                        }
                    }
                }
            }

            // Unicode escape \uHHHH or \u{HHHH}.
            'u' => {
                if self.unicode_aware() {
                    let is_braced = self.peek() == Some('{');
                    let value = self.parse_unicode_escape()?;
                    // In unicode mode, handle surrogate pairs: \uD83D\uDE00 → U+1F600.
                    // Only combine from \uHHHH form, NOT \u{HHHH} form.
                    if !is_braced && (0xD800..=0xDBFF).contains(&value) {
                        let saved = self.pos;
                        if self.eat('\\') && self.eat('u') && self.peek() != Some('{') {
                            let low = self.parse_unicode_escape()?;
                            if (0xDC00..=0xDFFF).contains(&low) {
                                let combined = 0x10000 + ((value - 0xD800) << 10) + (low - 0xDC00);
                                return Ok(Atom::Literal(combined));
                            }
                        }
                        self.pos = saved;
                        // Lone high surrogate is valid in u-mode (matches lone surrogate code unit).
                    }
                    // Lone surrogates (0xD800-0xDFFF) are valid in u-mode as code unit matchers.
                    if value > 0x10FFFF {
                        return Err(Error::InvalidUnicodeEscape);
                    }
                    Ok(Atom::Literal(value))
                } else {
                    // In non-unicode mode, incomplete \u is treated as literal.
                    let saved = self.pos;
                    match self.parse_unicode_escape() {
                        Ok(value) => Ok(Atom::Literal(value)),
                        Err(_) => {
                            self.pos = saved;
                            Ok(Atom::Literal('u' as u32))
                        }
                    }
                }
            }

            // Octal escapes (legacy, non-unicode mode).
            '0'..='7' if !self.unicode_aware() => {
                let value = self.parse_legacy_octal(ch);
                Ok(Atom::Literal(value as u32))
            }

            // Legacy numeric escapes that aren't backreferences.
            '8' | '9' if !self.unicode_aware() => Ok(Atom::Literal(ch as u32)),

            // Identity escape — in unicode mode, only syntax characters.
            _ => {
                if self.unicode_aware() {
                    if is_syntax_character(ch) || ch == '/' {
                        Ok(Atom::Literal(ch as u32))
                    } else {
                        Err(Error::InvalidEscape(ch))
                    }
                } else {
                    // Legacy mode: any character can be identity-escaped.
                    Ok(Atom::Literal(ch as u32))
                }
            }
        }
    }

    /// Determine if a digit after `\` should be parsed as a backreference
    /// instead of a legacy octal escape.
    /// - <https://tc39.es/ecma262/#sec-parsepattern>
    /// - <https://tc39.es/ecma262/#sec-parsepattern-annexb>
    fn is_backreference_context(&self, first_digit: char) -> bool {
        let n = first_digit.to_digit(10).unwrap();
        // In unicode mode, \1-\9 are always backreferences (or errors later).
        if self.unicode_aware() {
            return n > 0;
        }
        // Per ECMA-262 Annex B, a DecimalEscape is a backreference if the
        // value is ≤ NcapturingParens (the total number of groups in the
        // entire pattern, including forward references). We use the pre-scanned
        // total to handle forward backreferences correctly.
        if n > 0 && n <= self.total_capture_count {
            return true;
        }
        // In legacy mode, \8 and \9 are literal characters if not backreferences.
        // \1-\7 without matching groups are legacy octal escapes.
        false
    }

    fn has_named_groups_or_unicode(&self) -> bool {
        self.unicode_aware() || self.has_any_named_groups
    }

    // ---------------------------------------------------------------
    // Unicode property: {Name} or {Name=Value}
    // ---------------------------------------------------------------

    /// Parse a Unicode property escape payload.
    /// - <https://tc39.es/ecma262/#sec-runtime-semantics-unicodematchproperty-p>
    /// - <https://tc39.es/ecma262/#sec-runtime-semantics-unicodematchpropertyvalue-p-v>
    fn parse_unicode_property(&mut self) -> Result<(String, Option<String>), Error> {
        self.expect('{')?;
        let name = self.parse_unicode_property_name()?;
        let value = if self.eat('=') {
            Some(self.parse_unicode_property_name()?)
        } else {
            None
        };
        self.expect('}')?;

        // Validate against the ECMA-262 allow-list up front so later compiler
        // and VM stages can assume the escape names already match spec.
        if !crate::unicode_ffi::is_valid_ecma262_property(&name, value.as_deref()) {
            return Err(Error::InvalidUnicodeProperty(name));
        }

        Ok((name, value))
    }

    /// Check if a property name refers to a Unicode string property (e.g. Basic_Emoji).
    fn is_string_property(name: &str) -> bool {
        crate::unicode_ffi::is_string_property(name)
    }

    fn parse_unicode_property_name(&mut self) -> Result<String, Error> {
        let mut name = String::new();
        while let Some(ch) = self.peek() {
            if ch.is_ascii_alphanumeric() || ch == '_' {
                name.push(ch);
                self.advance();
            } else {
                break;
            }
        }
        if name.is_empty() {
            return Err(Error::InvalidUnicodeProperty(String::new()));
        }
        Ok(name)
    }

    // ---------------------------------------------------------------
    // Hex/Unicode escapes
    // ---------------------------------------------------------------

    fn parse_hex_escape(&mut self, digits: usize) -> Result<u32, Error> {
        let mut value: u32 = 0;
        for _ in 0..digits {
            let ch = self.advance().ok_or(Error::InvalidUnicodeEscape)?;
            let d = ch.to_digit(16).ok_or(Error::InvalidUnicodeEscape)?;
            value = value * 16 + d;
        }
        Ok(value)
    }

    fn parse_unicode_escape(&mut self) -> Result<u32, Error> {
        if self.eat('{') {
            // \u{HHHH...} — variable length, unicode mode.
            if !self.unicode_aware() {
                return Err(Error::InvalidUnicodeEscape);
            }
            let mut value: u32 = 0;
            let mut count = 0;
            while let Some(ch) = self.peek() {
                if ch == '}' {
                    break;
                }
                let d = ch.to_digit(16).ok_or(Error::InvalidUnicodeEscape)?;
                value = value.checked_mul(16).ok_or(Error::InvalidUnicodeEscape)?;
                value = value.checked_add(d).ok_or(Error::InvalidUnicodeEscape)?;
                if value > 0x10FFFF {
                    return Err(Error::InvalidUnicodeEscape);
                }
                self.advance();
                count += 1;
            }
            if count == 0 {
                return Err(Error::InvalidUnicodeEscape);
            }
            self.expect('}')?;
            Ok(value)
        } else {
            // \uHHHH — exactly 4 hex digits.
            self.parse_hex_escape(4)
        }
    }

    /// Parse legacy octal escape. `first` is the first octal digit already consumed.
    fn parse_legacy_octal(&mut self, first: char) -> u8 {
        let mut value = first.to_digit(8).unwrap();
        // Up to 3 octal digits total, but value must fit in a byte.
        if let Some(d) = self.peek().and_then(|c| c.to_digit(8))
            && value * 8 + d <= 0o377
        {
            self.advance();
            value = value * 8 + d;
            if let Some(d2) = self.peek().and_then(|c| c.to_digit(8))
                && value * 8 + d2 <= 0o377
            {
                self.advance();
                value = value * 8 + d2;
            }
        }
        value as u8
    }

    // ---------------------------------------------------------------
    // Groups
    // ---------------------------------------------------------------

    /// Parse the parenthesized atom forms: capture groups, lookarounds, and
    /// modifier groups.
    /// - <https://tc39.es/ecma262/#sec-compileatom>
    /// - <https://tc39.es/ecma262/#sec-patterns-static-semantics-early-errors>
    fn parse_group(&mut self) -> Result<Atom, Error> {
        self.expect('(')?;

        if self.eat('?') {
            match self.peek() {
                Some(':') => {
                    self.advance();
                    let body = self.parse_disjunction()?;
                    self.expect(')')?;
                    Ok(Atom::NonCapturingGroup(NonCapturingGroup { body }))
                }
                Some('=') => {
                    self.advance();
                    let body = self.parse_disjunction()?;
                    self.expect(')')?;
                    Ok(Atom::Lookaround(Lookaround {
                        kind: LookaroundKind::LookaheadPositive,
                        body,
                    }))
                }
                Some('!') => {
                    self.advance();
                    let body = self.parse_disjunction()?;
                    self.expect(')')?;
                    Ok(Atom::Lookaround(Lookaround {
                        kind: LookaroundKind::LookaheadNegative,
                        body,
                    }))
                }
                Some('<') => {
                    self.advance();
                    match self.peek() {
                        Some('=') => {
                            self.advance();
                            let body = self.parse_disjunction()?;
                            self.expect(')')?;
                            Ok(Atom::Lookaround(Lookaround {
                                kind: LookaroundKind::LookbehindPositive,
                                body,
                            }))
                        }
                        Some('!') => {
                            self.advance();
                            let body = self.parse_disjunction()?;
                            self.expect(')')?;
                            Ok(Atom::Lookaround(Lookaround {
                                kind: LookaroundKind::LookbehindNegative,
                                body,
                            }))
                        }
                        _ => {
                            // Named capture group: (?<name>...).
                            let name = self.parse_group_name()?;
                            self.expect('>')?;
                            // Duplicate names are allowed across alternatives,
                            // but not within the same alternative.
                            if let Some(current_alt_names) = self.alternative_name_stack.last()
                                && current_alt_names.contains(&name)
                            {
                                return Err(Error::DuplicateGroupName(name));
                            }
                            let index = self.next_capture_index;
                            self.next_capture_index += 1;
                            // Propagate the name to every active disjunction
                            // level so a duplicate later in the same enclosing
                            // alternative is rejected even if the first use was
                            // nested inside a child disjunction.
                            for level in self.alternative_name_stack.iter_mut() {
                                level.insert(name.clone());
                            }
                            self.named_groups.push(NamedGroup {
                                name: name.clone(),
                                index,
                            });
                            let body = self.parse_disjunction()?;
                            self.expect(')')?;
                            Ok(Atom::Group(Group {
                                index,
                                name: Some(name),
                                body,
                            }))
                        }
                    }
                }
                // Modifier group: (?flags:...) or (?flags-flags:...).
                Some(c) if is_modifier_flag(c) || c == '-' => {
                    let (add, remove) = self.parse_modifier_flags()?;
                    self.expect(':')?;
                    let body = self.parse_disjunction()?;
                    self.expect(')')?;
                    Ok(Atom::ModifierGroup(ModifierGroup {
                        add_flags: add,
                        remove_flags: remove,
                        body,
                    }))
                }
                Some(c) => Err(Error::UnexpectedChar(c)),
                None => Err(Error::UnexpectedEnd),
            }
        } else {
            // Plain capturing group.
            let index = self.next_capture_index;
            self.next_capture_index += 1;
            let body = self.parse_disjunction()?;
            self.expect(')')?;
            Ok(Atom::Group(Group {
                index,
                name: None,
                body,
            }))
        }
    }

    /// Parse `RegExpIdentifierName` inside `(?<name>...)` and `\k<name>`.
    /// <https://tc39.es/ecma262/#sec-patterns>
    fn parse_group_name(&mut self) -> Result<String, Error> {
        let mut name = String::new();
        let first = self.parse_group_name_char()?;
        if !is_id_start(first) {
            return Err(Error::InvalidGroupName);
        }
        name.push(first);
        while let Some(ch) = self.peek() {
            if ch == '>' {
                break;
            }
            let ch = self.parse_group_name_char()?;
            if !is_id_continue(ch) {
                return Err(Error::InvalidGroupName);
            }
            name.push(ch);
        }
        Ok(name)
    }

    /// Parse a single character in a group name, handling \u escapes.
    fn parse_group_name_char(&mut self) -> Result<char, Error> {
        let ch = self.advance().ok_or(Error::InvalidGroupName)?;
        if ch == '\\' {
            if self.eat('u') {
                let first_is_braced = self.peek() == Some('{');
                let cp = self.parse_unicode_escape_in_name()?;
                // Handle surrogate pairs only for \uHHHH\uHHHH, not \u{...}.
                if !first_is_braced
                    && (0xD800..=0xDBFF).contains(&cp)
                    && self.eat('\\')
                    && self.eat('u')
                    && self.peek() != Some('{')
                {
                    let low = self.parse_unicode_escape_in_name()?;
                    if (0xDC00..=0xDFFF).contains(&low) {
                        let combined = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                        return char::from_u32(combined).ok_or(Error::InvalidGroupName);
                    }
                    return Err(Error::InvalidGroupName);
                } else if (0xD800..=0xDBFF).contains(&cp) {
                    return Err(Error::InvalidGroupName);
                }
                char::from_u32(cp).ok_or(Error::InvalidGroupName)
            } else {
                Err(Error::InvalidGroupName)
            }
        } else {
            Ok(ch)
        }
    }

    /// Parse \uXXXX or \u{XXXX} in a group name context.
    fn parse_unicode_escape_in_name(&mut self) -> Result<u32, Error> {
        if self.eat('{') {
            // \u{XXXX}
            let mut val = 0u32;
            let mut digits = 0;
            while let Some(ch) = self.peek() {
                if ch == '}' {
                    self.advance();
                    if digits == 0 || val > 0x10FFFF {
                        return Err(Error::InvalidGroupName);
                    }
                    return Ok(val);
                }
                let d = ch.to_digit(16).ok_or(Error::InvalidGroupName)?;
                val = val.checked_mul(16).ok_or(Error::InvalidGroupName)?;
                val = val.checked_add(d).ok_or(Error::InvalidGroupName)?;
                digits += 1;
                self.advance();
            }
            Err(Error::InvalidGroupName)
        } else {
            // \uXXXX (exactly 4 hex digits)
            let mut val = 0u32;
            for _ in 0..4 {
                let ch = self.advance().ok_or(Error::InvalidGroupName)?;
                let d = ch.to_digit(16).ok_or(Error::InvalidGroupName)?;
                val = val * 16 + d;
            }
            Ok(val)
        }
    }

    /// Parse the flag delta in a modifier group before `UpdateModifiers`
    /// applies it to the enclosed disjunction.
    /// <https://tc39.es/ecma262/#sec-updatemodifiers>
    fn parse_modifier_flags(&mut self) -> Result<(ModifierFlags, ModifierFlags), Error> {
        let mut add = ModifierFlags::default();
        let mut removing = false;
        let mut remove = ModifierFlags::default();
        // Track all flags seen (on either side of '-') to reject duplicates.
        let mut seen = ModifierFlags::default();

        loop {
            match self.peek() {
                Some(':') | Some(')') | None => break,
                Some('-') if !removing => {
                    self.advance();
                    removing = true;
                }
                Some(c) if is_modifier_flag(c) => {
                    self.advance();
                    // Reject duplicate flags, e.g. (?ii:x) or (?i-i:x).
                    let already_seen = match c {
                        'i' => seen.ignore_case,
                        'm' => seen.multiline,
                        's' => seen.dot_all,
                        _ => return Err(Error::InvalidModifierFlags),
                    };
                    if already_seen {
                        return Err(Error::InvalidModifierFlags);
                    }
                    let target = if removing { &mut remove } else { &mut add };
                    match c {
                        'i' => {
                            target.ignore_case = true;
                            seen.ignore_case = true;
                        }
                        'm' => {
                            target.multiline = true;
                            seen.multiline = true;
                        }
                        's' => {
                            target.dot_all = true;
                            seen.dot_all = true;
                        }
                        _ => unreachable!(),
                    }
                }
                _ => return Err(Error::InvalidModifierFlags),
            }
        }
        // (?-: with no flags on either side of '-' is invalid.
        let add_empty = !add.ignore_case && !add.multiline && !add.dot_all;
        let remove_empty = !remove.ignore_case && !remove.multiline && !remove.dot_all;
        if removing && add_empty && remove_empty {
            return Err(Error::InvalidModifierFlags);
        }
        Ok((add, remove))
    }

    // ---------------------------------------------------------------
    // Character classes
    // ---------------------------------------------------------------

    /// Parse `CharacterClass`.
    /// <https://tc39.es/ecma262/#sec-compilecharacterclass>
    fn parse_character_class(&mut self) -> Result<CharacterClass, Error> {
        self.expect('[')?;
        let negated = self.eat('^');

        if self.flags.unicode_sets {
            let saved_negated = self.in_negated_class;
            // `/v` has extra restrictions for string-valued members inside
            // `[^ ... ]`, so remember whether the current class is negated
            // while parsing nested class-set operands.
            self.in_negated_class = negated;
            let expr = if self.peek() == Some(']') {
                ClassSetExpression::Union(Vec::new())
            } else {
                self.parse_class_set_expression()?
            };
            self.in_negated_class = saved_negated;
            if negated && !Self::class_set_expression_strings(&expr).is_empty() {
                return Err(Error::InvalidCharacterClass);
            }
            self.expect(']')?;
            Ok(CharacterClass {
                negated,
                body: CharacterClassBody::UnicodeSet(expr),
            })
        } else {
            let ranges = self.parse_class_ranges()?;
            self.expect(']')?;
            Ok(CharacterClass {
                negated,
                body: CharacterClassBody::Ranges(ranges),
            })
        }
    }

    /// Parse the legacy and `/u` `ClassContents` forms that flatten into ranges
    /// and single atoms.
    /// <https://tc39.es/ecma262/#sec-compiletocharset>
    fn parse_class_ranges(&mut self) -> Result<Vec<CharacterClassRange>, Error> {
        let mut ranges = Vec::new();
        while self.peek() != Some(']') && !self.at_end() {
            let first = self.parse_class_atom()?;
            if self.eat('-') {
                if self.peek() == Some(']') {
                    // e.g. [a-] — the '-' is a literal.
                    ranges.push(first);
                    ranges.push(CharacterClassRange::Single('-' as u32));
                } else {
                    let second = self.parse_class_atom()?;
                    // Build a range from first-second.
                    let lo = class_atom_to_code_point(&first);
                    let hi = class_atom_to_code_point(&second);
                    if let (Ok(lo), Ok(hi)) = (lo, hi) {
                        if lo > hi {
                            return Err(Error::InvalidCharacterRange(lo, hi));
                        } else {
                            ranges.push(CharacterClassRange::Range(lo, hi));
                        }
                    } else if self.unicode_aware() {
                        // In unicode mode, character class in range is an error.
                        return Err(Error::InvalidCharacterClass);
                    } else {
                        // In non-unicode mode, treat as three separate items.
                        ranges.push(first);
                        ranges.push(CharacterClassRange::Single('-' as u32));
                        ranges.push(second);
                    }
                }
            } else {
                ranges.push(first);
            }
        }
        Ok(ranges)
    }

    fn parse_class_atom(&mut self) -> Result<CharacterClassRange, Error> {
        let ch = self.peek().ok_or(Error::InvalidCharacterClass)?;
        match ch {
            '\\' => self.parse_class_escape(),
            _ => {
                self.advance();
                Ok(CharacterClassRange::Single(ch as u32))
            }
        }
    }

    /// Parse `ClassEscape`.
    /// <https://tc39.es/ecma262/#sec-compiletocharset>
    fn parse_class_escape(&mut self) -> Result<CharacterClassRange, Error> {
        self.expect('\\')?;
        let ch = self.advance().ok_or(Error::LoneTrailingBackslash)?;
        match ch {
            'd' => Ok(CharacterClassRange::BuiltinClass(
                BuiltinCharacterClass::Digit,
            )),
            'D' => Ok(CharacterClassRange::BuiltinClass(
                BuiltinCharacterClass::NonDigit,
            )),
            'w' => Ok(CharacterClassRange::BuiltinClass(
                BuiltinCharacterClass::Word,
            )),
            'W' => Ok(CharacterClassRange::BuiltinClass(
                BuiltinCharacterClass::NonWord,
            )),
            's' => Ok(CharacterClassRange::BuiltinClass(
                BuiltinCharacterClass::Whitespace,
            )),
            'S' => Ok(CharacterClassRange::BuiltinClass(
                BuiltinCharacterClass::NonWhitespace,
            )),
            'p' | 'P' if self.unicode_aware() => {
                let negated = ch == 'P';
                let prop = self.parse_unicode_property()?;
                // String properties are only valid with /v flag, not /u.
                if prop.1.is_none() && Self::is_string_property(&prop.0) {
                    if !self.flags.unicode_sets {
                        return Err(Error::InvalidUnicodeProperty(prop.0));
                    }
                    if negated {
                        return Err(Error::InvalidUnicodeProperty(prop.0));
                    }
                }
                Ok(CharacterClassRange::UnicodeProperty(UnicodeProperty {
                    negated,
                    name: prop.0,
                    value: prop.1,
                }))
            }
            'b' => Ok(CharacterClassRange::Single('\u{08}' as u32)), // backspace in char class
            'n' => Ok(CharacterClassRange::Single('\n' as u32)),
            'r' => Ok(CharacterClassRange::Single('\r' as u32)),
            't' => Ok(CharacterClassRange::Single('\t' as u32)),
            'f' => Ok(CharacterClassRange::Single('\u{0C}' as u32)),
            'v' => Ok(CharacterClassRange::Single('\u{0B}' as u32)),
            '0' if !self.peek().is_some_and(|c| c.is_ascii_digit()) => {
                Ok(CharacterClassRange::Single(0))
            }
            'c' => {
                if let Some(ctrl) = self.advance() {
                    if ctrl.is_ascii_alphabetic() {
                        Ok(CharacterClassRange::Single(((ctrl as u8) & 0x1f) as u32))
                    } else if self.unicode_aware() {
                        Err(Error::InvalidEscape('c'))
                    } else if ctrl.is_ascii_digit() || ctrl == '_' {
                        // AnnexB ClassControlLetter: digits and underscore are valid.
                        Ok(CharacterClassRange::Single(((ctrl as u8) & 0x1f) as u32))
                    } else {
                        // Put back both the non-alpha char and 'c'.
                        self.pos -= 2;
                        Ok(CharacterClassRange::Single('\\' as u32))
                    }
                } else if self.unicode_aware() {
                    Err(Error::InvalidEscape('c'))
                } else {
                    // \c at end of pattern in legacy mode: literal backslash.
                    self.pos -= 1;
                    Ok(CharacterClassRange::Single('\\' as u32))
                }
            }
            'x' => {
                if self.unicode_aware() {
                    let val = self.parse_hex_escape(2)?;
                    Ok(CharacterClassRange::Single(val))
                } else {
                    let saved = self.pos;
                    match self.parse_hex_escape(2) {
                        Ok(val) => Ok(CharacterClassRange::Single(val)),
                        Err(_) => {
                            self.pos = saved;
                            Ok(CharacterClassRange::Single('x' as u32))
                        }
                    }
                }
            }
            'u' => {
                if self.unicode_aware() {
                    let is_braced = self.peek() == Some('{');
                    let val = self.parse_unicode_escape()?;
                    if !is_braced && (0xD800..=0xDBFF).contains(&val) {
                        let saved = self.pos;
                        if self.eat('\\') && self.eat('u') && self.peek() != Some('{') {
                            let low = self.parse_unicode_escape()?;
                            if (0xDC00..=0xDFFF).contains(&low) {
                                let combined = 0x10000 + ((val - 0xD800) << 10) + (low - 0xDC00);
                                return Ok(CharacterClassRange::Single(combined));
                            }
                        }
                        self.pos = saved;
                    }
                    if val > 0x10FFFF {
                        return Err(Error::InvalidUnicodeEscape);
                    }
                    Ok(CharacterClassRange::Single(val))
                } else {
                    let saved = self.pos;
                    match self.parse_unicode_escape() {
                        Ok(val) => Ok(CharacterClassRange::Single(val)),
                        Err(_) => {
                            self.pos = saved;
                            Ok(CharacterClassRange::Single('u' as u32))
                        }
                    }
                }
            }
            '0'..='7' if !self.unicode_aware() => {
                let value = self.parse_legacy_octal(ch);
                Ok(CharacterClassRange::Single(value as u32))
            }
            _ => {
                if self.unicode_aware() {
                    if is_syntax_character(ch) || ch == '/' || ch == '-' {
                        Ok(CharacterClassRange::Single(ch as u32))
                    } else {
                        Err(Error::InvalidEscape(ch))
                    }
                } else {
                    Ok(CharacterClassRange::Single(ch as u32))
                }
            }
        }
    }

    // ---------------------------------------------------------------
    // Unicode sets mode character classes (/v flag)
    // ---------------------------------------------------------------

    /// Parse `/v` `ClassSetExpression`.
    /// <https://tc39.es/ecma262/#sec-compilecharacterclass>
    fn parse_class_set_expression(&mut self) -> Result<ClassSetExpression, Error> {
        let first = self.parse_class_set_operand()?;

        // Check for set operation operators.
        match self.peek_pair() {
            Some(('-', '-')) => {
                // The grammar does not mix `--` and `&&` at the same level, so
                // once we see subtraction we stay in that operator family until
                // the surrounding class or nested operand ends.
                // Subtraction: A--B--C
                let mut operands = vec![first];
                while self.peek_pair() == Some(('-', '-')) {
                    self.pos += 2; // consume '--'
                    operands.push(self.parse_class_set_operand()?);
                }
                Self::validate_class_set_operation_operands(&operands)?;
                Ok(ClassSetExpression::Subtraction(operands))
            }
            Some(('&', '&')) => {
                // Same idea as subtraction above: a single expression level is
                // either an intersection chain or something else, never both.
                // Intersection: A&&B&&C
                let mut operands = vec![first];
                while self.peek_pair() == Some(('&', '&')) {
                    self.pos += 2; // consume '&&'
                    operands.push(self.parse_class_set_operand()?);
                }
                Self::validate_class_set_operation_operands(&operands)?;
                Ok(ClassSetExpression::Intersection(operands))
            }
            _ => {
                // Union (default): just accumulate operands.
                let mut operands = vec![first];
                while self.peek() != Some(']') && !self.at_end() {
                    operands.push(self.parse_class_set_operand()?);
                }
                Ok(ClassSetExpression::Union(operands))
            }
        }
    }

    fn validate_class_set_operation_operands(operands: &[ClassSetOperand]) -> Result<(), Error> {
        if operands
            .iter()
            .any(|operand| matches!(operand, ClassSetOperand::Range(_, _)))
        {
            return Err(Error::InvalidCharacterClass);
        }

        Ok(())
    }

    fn get_string_property_strings(name: &str) -> std::collections::BTreeSet<Vec<u32>> {
        let buf = crate::unicode_ffi::get_string_property_data(name);
        if buf.is_empty() {
            return std::collections::BTreeSet::new();
        }

        let count = buf[0] as usize;
        let mut strings = std::collections::BTreeSet::new();
        let mut offset = 1;
        for _ in 0..count {
            if offset >= buf.len() {
                break;
            }
            let len = buf[offset] as usize;
            offset += 1;
            if offset + len > buf.len() {
                break;
            }
            if len > 1 {
                strings.insert(buf[offset..offset + len].to_vec());
            }
            offset += len;
        }
        strings
    }

    fn class_set_expression_strings(
        expr: &ClassSetExpression,
    ) -> std::collections::BTreeSet<Vec<u32>> {
        match expr {
            ClassSetExpression::Union(operands) => {
                operands
                    .iter()
                    .fold(std::collections::BTreeSet::new(), |mut strings, operand| {
                        strings.extend(Self::class_set_operand_strings(operand));
                        strings
                    })
            }
            ClassSetExpression::Intersection(operands) => {
                let Some((first, rest)) = operands.split_first() else {
                    return std::collections::BTreeSet::new();
                };
                let mut strings = Self::class_set_operand_strings(first);
                for operand in rest {
                    let operand_strings = Self::class_set_operand_strings(operand);
                    strings.retain(|string| operand_strings.contains(string));
                }
                strings
            }
            ClassSetExpression::Subtraction(operands) => {
                let Some((first, rest)) = operands.split_first() else {
                    return std::collections::BTreeSet::new();
                };
                let mut strings = Self::class_set_operand_strings(first);
                for operand in rest {
                    let operand_strings = Self::class_set_operand_strings(operand);
                    strings.retain(|string| !operand_strings.contains(string));
                }
                strings
            }
        }
    }

    fn class_set_operand_strings(
        operand: &ClassSetOperand,
    ) -> std::collections::BTreeSet<Vec<u32>> {
        match operand {
            ClassSetOperand::NestedClass(class) => {
                if class.negated {
                    return std::collections::BTreeSet::new();
                }
                match &class.body {
                    CharacterClassBody::Ranges(_) => std::collections::BTreeSet::new(),
                    CharacterClassBody::UnicodeSet(expr) => {
                        Self::class_set_expression_strings(expr)
                    }
                }
            }
            ClassSetOperand::UnicodeProperty(property) => {
                if !property.negated
                    && property.value.is_none()
                    && Self::is_string_property(&property.name)
                {
                    return Self::get_string_property_strings(&property.name);
                }
                std::collections::BTreeSet::new()
            }
            ClassSetOperand::StringLiteral(chars) => {
                if chars.len() > 1 {
                    return [chars.iter().map(|ch| *ch as u32).collect()]
                        .into_iter()
                        .collect();
                }
                std::collections::BTreeSet::new()
            }
            ClassSetOperand::Char(_)
            | ClassSetOperand::Range(_, _)
            | ClassSetOperand::BuiltinClass(_) => std::collections::BTreeSet::new(),
        }
    }

    /// Parse one `/v` `ClassSetOperand`.
    /// - <https://tc39.es/ecma262/#sec-patterns>
    /// - <https://tc39.es/ecma262/#sec-compileclasssetstring>
    fn parse_class_set_operand(&mut self) -> Result<ClassSetOperand, Error> {
        let ch = self.peek().ok_or(Error::InvalidCharacterClass)?;
        match ch {
            '[' => {
                let cc = self.parse_character_class()?;
                Ok(ClassSetOperand::NestedClass(cc))
            }
            '\\' => {
                self.expect('\\')?;
                let esc = self.peek().ok_or(Error::LoneTrailingBackslash)?;
                match esc {
                    'q' => {
                        self.advance();
                        self.expect('{')?;
                        let mut alternatives: Vec<Vec<char>> = Vec::new();
                        let mut current = Vec::new();
                        while self.peek() != Some('}') && !self.at_end() {
                            let c = self.advance().ok_or(Error::UnexpectedEnd)?;
                            if c == '|' {
                                alternatives.push(std::mem::take(&mut current));
                            } else if matches!(c, '(' | ')' | '[' | ']' | '{' | '}' | '/' | '-') {
                                return Err(Error::InvalidCharacterClass);
                            } else if c == '\\' {
                                let escaped = self.advance().ok_or(Error::LoneTrailingBackslash)?;
                                current.push(match escaped {
                                    'n' => '\n',
                                    'r' => '\r',
                                    't' => '\t',
                                    'f' => '\u{0C}',
                                    'v' => '\u{0B}',
                                    '|' => '|',
                                    '\\' => '\\',
                                    '0' => '\0',
                                    'b' => '\u{08}',
                                    'x' => {
                                        let val = self.parse_hex_escape(2)?;
                                        char::from_u32(val).ok_or(Error::InvalidUnicodeEscape)?
                                    }
                                    'u' => {
                                        let is_braced = self.peek() == Some('{');
                                        let val = self.parse_unicode_escape()?;
                                        // Handle surrogate pairs: \uD800\uDC00
                                        if !is_braced && (0xD800..=0xDBFF).contains(&val) {
                                            let saved = self.pos;
                                            if self.eat('\\')
                                                && self.eat('u')
                                                && self.peek() != Some('{')
                                            {
                                                let low = self.parse_unicode_escape()?;
                                                if (0xDC00..=0xDFFF).contains(&low) {
                                                    let combined = 0x10000
                                                        + ((val - 0xD800) << 10)
                                                        + (low - 0xDC00);
                                                    char::from_u32(combined)
                                                        .ok_or(Error::InvalidUnicodeEscape)?
                                                } else {
                                                    self.pos = saved;
                                                    char::from_u32(val)
                                                        .ok_or(Error::InvalidUnicodeEscape)?
                                                }
                                            } else {
                                                self.pos = saved;
                                                char::from_u32(val)
                                                    .ok_or(Error::InvalidUnicodeEscape)?
                                            }
                                        } else {
                                            char::from_u32(val)
                                                .ok_or(Error::InvalidUnicodeEscape)?
                                        }
                                    }
                                    'c' => {
                                        let ctrl = self.advance().ok_or(Error::UnexpectedEnd)?;
                                        if ctrl.is_ascii_alphabetic() {
                                            char::from_u32((ctrl as u32) & 0x1F).unwrap()
                                        } else {
                                            return Err(Error::InvalidEscape(ctrl));
                                        }
                                    }
                                    // Only syntax characters and `-` may be escaped in \q{}.
                                    _ if is_syntax_character(escaped) || escaped == '-' => escaped,
                                    _ => return Err(Error::InvalidEscape(escaped)),
                                });
                            } else {
                                current.push(c);
                            }
                        }
                        alternatives.push(current);
                        self.expect('}')?;

                        // In a negated class, \q{} may not contain multi-char strings.
                        if self.in_negated_class && alternatives.iter().any(|alt| alt.len() > 1) {
                            return Err(Error::InvalidCharacterClass);
                        }

                        if alternatives.len() == 1 {
                            Ok(ClassSetOperand::StringLiteral(
                                alternatives.into_iter().next().unwrap(),
                            ))
                        } else {
                            // Multiple alternatives: wrap as a nested class with Union body.
                            // Sort longest-first so greedy matching prefers longer alternatives.
                            alternatives.sort_by_key(|b| std::cmp::Reverse(b.len()));
                            let operands: Vec<ClassSetOperand> = alternatives
                                .into_iter()
                                .map(ClassSetOperand::StringLiteral)
                                .collect();
                            Ok(ClassSetOperand::NestedClass(CharacterClass {
                                negated: false,
                                body: CharacterClassBody::UnicodeSet(ClassSetExpression::Union(
                                    operands,
                                )),
                            }))
                        }
                    }
                    'd' => {
                        self.advance();
                        Ok(ClassSetOperand::BuiltinClass(BuiltinCharacterClass::Digit))
                    }
                    'D' => {
                        self.advance();
                        Ok(ClassSetOperand::BuiltinClass(
                            BuiltinCharacterClass::NonDigit,
                        ))
                    }
                    'w' => {
                        self.advance();
                        Ok(ClassSetOperand::BuiltinClass(BuiltinCharacterClass::Word))
                    }
                    'W' => {
                        self.advance();
                        Ok(ClassSetOperand::BuiltinClass(
                            BuiltinCharacterClass::NonWord,
                        ))
                    }
                    's' => {
                        self.advance();
                        Ok(ClassSetOperand::BuiltinClass(
                            BuiltinCharacterClass::Whitespace,
                        ))
                    }
                    'S' => {
                        self.advance();
                        Ok(ClassSetOperand::BuiltinClass(
                            BuiltinCharacterClass::NonWhitespace,
                        ))
                    }
                    'p' | 'P' => {
                        let negated = esc == 'P';
                        self.advance();
                        let prop = self.parse_unicode_property()?;
                        // In v-mode, string properties cannot be negated (\P)
                        // or used inside a negated character class ([^...]).
                        if prop.1.is_none()
                            && Self::is_string_property(&prop.0)
                            && (negated || self.in_negated_class)
                        {
                            return Err(Error::InvalidUnicodeProperty(prop.0));
                        }
                        Ok(ClassSetOperand::UnicodeProperty(UnicodeProperty {
                            negated,
                            name: prop.0,
                            value: prop.1,
                        }))
                    }
                    _ => {
                        // Single escaped character.
                        let c = self.parse_class_set_char_escape()?;
                        self.try_parse_class_set_range(c)
                    }
                }
            }
            _ => {
                // In unicodeSets mode, ClassSetSyntaxCharacter cannot appear unescaped.
                // ClassSetSyntaxCharacter :: one of ( ) [ ] { } / - \ |
                // Note: `\` and `[` are handled above, `]` ends the class in the caller.
                if is_class_set_syntax_character(ch) {
                    return Err(Error::InvalidCharacterClass);
                }
                // ClassSetReservedDoublePunctuator check: pairs like &&, !!, ##, $$, etc.
                // are forbidden even though the individual characters are allowed.
                // Note: -- and && are handled as set operations by the caller.
                if let Some(next) = self.peek_at(1)
                    && ch == next
                    && is_class_set_reserved_double_punctuator_char(ch)
                {
                    return Err(Error::InvalidCharacterClass);
                }
                self.advance();
                self.try_parse_class_set_range(ch)
            }
        }
    }

    /// Parse the restricted escape forms allowed when a `/v` class set operand
    /// expects a single character.
    /// <https://tc39.es/ecma262/#sec-patterns>
    fn parse_class_set_char_escape(&mut self) -> Result<char, Error> {
        let ch = self.advance().ok_or(Error::LoneTrailingBackslash)?;
        match ch {
            'n' => Ok('\n'),
            'r' => Ok('\r'),
            't' => Ok('\t'),
            'f' => Ok('\u{0C}'),
            'v' => Ok('\u{0B}'),
            'b' => Ok('\u{08}'),
            '0' if !self.peek().is_some_and(|c| c.is_ascii_digit()) => Ok('\0'),
            'x' => {
                let val = self.parse_hex_escape(2)?;
                char::from_u32(val).ok_or(Error::InvalidUnicodeEscape)
            }
            'u' => {
                let is_braced = self.peek() == Some('{');
                let val = self.parse_unicode_escape()?;
                if !is_braced && (0xD800..=0xDBFF).contains(&val) {
                    let saved = self.pos;
                    if self.eat('\\') && self.eat('u') && self.peek() != Some('{') {
                        let low = self.parse_unicode_escape()?;
                        if (0xDC00..=0xDFFF).contains(&low) {
                            let combined = 0x10000 + ((val - 0xD800) << 10) + (low - 0xDC00);
                            return char::from_u32(combined).ok_or(Error::InvalidUnicodeEscape);
                        }
                        self.pos = saved;
                    } else {
                        self.pos = saved;
                    }
                }
                char::from_u32(val).ok_or(Error::InvalidUnicodeEscape)
            }
            _ if is_syntax_character(ch) || ch == '/' || ch == '-' => Ok(ch),
            _ => Err(Error::InvalidEscape(ch)),
        }
    }

    /// After reading a character in a unicode-sets class, check if it's followed by `-` to form a range.
    fn try_parse_class_set_range(&mut self, first: char) -> Result<ClassSetOperand, Error> {
        if self.eat('-') {
            if self.peek() == Some(']')
                || self.peek() == Some('-')
                || self.peek_pair() == Some(('&', '&'))
            {
                // `-]`, `--`, and `-&&` do not start a character range here.
                // Leave the `-` for the outer class-set parser to reinterpret.
                self.pos -= 1;
                return Ok(ClassSetOperand::Char(first));
            }
            let second = if self.eat('\\') {
                self.parse_class_set_char_escape()?
            } else {
                let c = self.peek().ok_or(Error::InvalidCharacterClass)?;
                if is_class_set_syntax_character(c) {
                    return Err(Error::InvalidCharacterClass);
                }
                self.advance().ok_or(Error::InvalidCharacterClass)?
            };
            if first > second {
                return Err(Error::InvalidCharacterRange(first as u32, second as u32));
            }
            Ok(ClassSetOperand::Range(first, second))
        } else {
            Ok(ClassSetOperand::Char(first))
        }
    }

    fn peek_pair(&self) -> Option<(char, char)> {
        Some((self.peek()?, self.peek_at(1)?))
    }
}

// -------------------------------------------------------------------
// Helper functions
// -------------------------------------------------------------------

fn is_syntax_character(ch: char) -> bool {
    matches!(
        ch,
        '^' | '$' | '\\' | '.' | '*' | '+' | '?' | '(' | ')' | '[' | ']' | '{' | '}' | '|'
    )
}

/// ClassSetSyntaxCharacter :: one of ( ) [ ] { } / - \ |
fn is_class_set_syntax_character(ch: char) -> bool {
    matches!(ch, '(' | ')' | '{' | '}' | '/' | '-' | '|')
}

/// Characters that form ClassSetReservedDoublePunctuator when doubled.
/// ClassSetReservedDoublePunctuator :: one of && !! ## $$ %% ** ++ ,, .. :: ;; << == >> ?? @@ ^^ `` ~~
fn is_class_set_reserved_double_punctuator_char(ch: char) -> bool {
    matches!(
        ch,
        '&' | '!'
            | '#'
            | '$'
            | '%'
            | '*'
            | '+'
            | ','
            | '.'
            | ':'
            | ';'
            | '<'
            | '='
            | '>'
            | '?'
            | '@'
            | '^'
            | '`'
            | '~'
    )
}

fn is_modifier_flag(ch: char) -> bool {
    matches!(ch, 'i' | 'm' | 's')
}

fn is_id_start(ch: char) -> bool {
    ch == '$' || ch == '_' || crate::unicode_ffi::is_id_start(ch as u32)
}

fn is_id_continue(ch: char) -> bool {
    matches!(ch, '$' | '_' | '\u{200C}' | '\u{200D}')
        || crate::unicode_ffi::is_id_continue(ch as u32)
}

/// Extract a single code point from a class atom, for use in ranges.
fn class_atom_to_code_point(atom: &CharacterClassRange) -> Result<u32, Error> {
    match atom {
        CharacterClassRange::Single(cp) => Ok(*cp),
        CharacterClassRange::BuiltinClass(_) | CharacterClassRange::UnicodeProperty(_) => {
            Err(Error::InvalidCharacterClass)
        }
        CharacterClassRange::Range(_, _) => Err(Error::InvalidCharacterClass),
    }
}
