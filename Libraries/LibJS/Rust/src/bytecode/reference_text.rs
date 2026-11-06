/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The text of a reference for the message of an error, from the source around the instruction that threw.
//!
//! Code keeps no text for this: error messages are rare, and the source says it all. Where an instruction is in the source
//! tells what it is about. A `.` or a `[` comes right after the base of a member access, a `(` right after what gets
//! called, an assignment operator right after its target, and `new` and a prefix update right in front of theirs. Anything
//! else has no text. The source gets read backward from there, as far as a chain of members goes.

use crate::host::js_number_to_utf16;
use crate::utf16::Utf16View;

/// What a reference starts with.
enum Base {
    Name(Vec<u16>),
    /// `this`, a string or a number.
    Literal(Vec<u16>),
    /// Something with no text of its own.
    Unnamed,
}

/// What comes after the base of a reference.
enum Link {
    Name(Vec<u16>),
    PrivateName(Vec<u16>),
    Computed(Reference),
}

struct Reference {
    base: Base,
    links: Vec<Link>,
}

impl Reference {
    fn unnamed() -> Self {
        Self {
            base: Base::Unnamed,
            links: Vec::new(),
        }
    }

    /// The text of something that got called or constructed, in which everything has a place.
    fn append_callee_text(&self, text: &mut Vec<u16>) {
        match &self.base {
            Base::Name(name) | Base::Literal(name) => text.extend_from_slice(name),
            Base::Unnamed => text.extend("<object>".encode_utf16()),
        }
        for link in &self.links {
            match link {
                Link::Name(name) | Link::PrivateName(name) => {
                    text.push(u16::from(b'.'));
                    text.extend_from_slice(name);
                }
                Link::Computed(key) => {
                    text.push(u16::from(b'['));
                    key.append_callee_text(text);
                    text.push(u16::from(b']'));
                }
            }
        }
    }

    /// The text of the base of a member access, which leaves out what it has no text for. Returns whether it has one.
    fn append_base_text(&self, text: &mut Vec<u16>) -> bool {
        let has_base = match &self.base {
            Base::Name(name) | Base::Literal(name) => {
                text.extend_from_slice(name);
                true
            }
            Base::Unnamed => false,
        };
        for link in &self.links {
            match link {
                Link::Name(name) => {
                    text.push(u16::from(b'.'));
                    text.extend_from_slice(name);
                }
                Link::PrivateName(_) => {}
                Link::Computed(key) => {
                    let start = text.len();
                    text.push(u16::from(b'['));
                    if key.append_base_text(text) {
                        text.push(u16::from(b']'));
                    } else {
                        text.truncate(start);
                    }
                }
            }
        }
        has_base || !self.links.is_empty()
    }
}

fn is_word_unit(unit: u16) -> bool {
    char::from_u32(u32::from(unit)).is_some_and(|c| c.is_alphanumeric() || c == '_' || c == '$')
}

fn is_whitespace_unit(unit: u16) -> bool {
    char::from_u32(u32::from(unit)).is_some_and(char::is_whitespace) || unit == 0xFEFF
}

fn is_quote(unit: u16) -> bool {
    unit == u16::from(b'"') || unit == u16::from(b'\'')
}

fn is_digit(unit: u16) -> bool {
    (0x30..=0x39).contains(&unit)
}

fn equals(units: &[u16], text: &str) -> bool {
    units.iter().copied().eq(text.encode_utf16())
}

/// Whether the word `units` is a keyword that a value comes after, so that a `(` or a `[` behind it starts something new.
fn is_keyword_in_front_of_value(units: &[u16]) -> bool {
    const KEYWORDS: [&str; 15] = [
        "await",
        "case",
        "delete",
        "do",
        "else",
        "extends",
        "in",
        "instanceof",
        "new",
        "of",
        "return",
        "throw",
        "typeof",
        "void",
        "yield",
    ];
    KEYWORDS.iter().any(|keyword| equals(units, keyword))
}

/// What the name of a number is in the message: the number, and not how the source writes it.
fn text_of_number(units: &[u16]) -> Option<Vec<u16>> {
    let text = String::from_utf16(units).ok()?;
    let text = text.replace('_', "");
    let value = match text.get(..2).map(str::to_ascii_lowercase).as_deref() {
        Some("0x") => u64::from_str_radix(&text[2..], 16).ok()? as f64,
        Some("0o") => u64::from_str_radix(&text[2..], 8).ok()? as f64,
        Some("0b") => u64::from_str_radix(&text[2..], 2).ok()? as f64,
        _ => text.parse::<f64>().ok()?,
    };
    Some(Utf16View::of_string(&js_number_to_utf16(value)).code_units().collect())
}

struct Scanner<'a> {
    units: &'a [u16],
}

impl Scanner<'_> {
    fn unit_at(&self, index: usize) -> u16 {
        self.units.get(index).copied().unwrap_or(0)
    }

    fn starts_with_at(&self, index: usize, text: &str) -> bool {
        (index..)
            .zip(text.encode_utf16())
            .all(|(position, unit)| self.unit_at(position) == unit)
    }

    /// Where a word starts that ends at `end`, which is `end` itself if there is none.
    fn start_of_word(&self, first: usize, end: usize) -> usize {
        let mut start = end;
        while start > first && is_word_unit(self.units[start - 1]) {
            start -= 1;
        }
        start
    }

    fn end_of_word(&self, start: usize) -> usize {
        let mut end = start;
        while end < self.units.len() && is_word_unit(self.units[end]) {
            end += 1;
        }
        end
    }

    /// Goes back over whitespace and comments from `end`.
    fn skip_trivia_before(&self, first: usize, mut end: usize) -> usize {
        loop {
            while end > first && is_whitespace_unit(self.units[end - 1]) {
                let is_line_end = matches!(self.units[end - 1], 0x0A | 0x0D | 0x2028 | 0x2029);
                end -= 1;
                if is_line_end && let Some(comment) = self.line_comment_ending_at(first, end) {
                    end = comment;
                }
            }
            if end >= first + 2 && self.starts_with_at(end - 2, "*/") {
                let mut start = end - 2;
                while start > first && !self.starts_with_at(start, "/*") {
                    start -= 1;
                }
                if !self.starts_with_at(start, "/*") {
                    return end;
                }
                end = start;
                continue;
            }
            return end;
        }
    }

    /// Where a `//` comment starts on the line that ends at `line_end`.
    fn line_comment_ending_at(&self, first: usize, line_end: usize) -> Option<usize> {
        let mut start = line_end;
        while start > first && !matches!(self.units[start - 1], 0x0A | 0x0D | 0x2028 | 0x2029) {
            start -= 1;
        }
        (start..line_end.saturating_sub(1)).find(|&index| self.starts_with_at(index, "//"))
    }

    fn skip_trivia_after(&self, mut start: usize) -> usize {
        loop {
            while start < self.units.len() && is_whitespace_unit(self.units[start]) {
                start += 1;
            }
            if self.starts_with_at(start, "//") {
                while start < self.units.len() && !matches!(self.units[start], 0x0A | 0x0D | 0x2028 | 0x2029) {
                    start += 1;
                }
                continue;
            }
            if self.starts_with_at(start, "/*") {
                start += 2;
                while start < self.units.len() && !self.starts_with_at(start, "*/") {
                    start += 1;
                }
                start += 2;
                continue;
            }
            return start;
        }
    }

    /// Where the string starts that has its closing quote at `close`.
    fn start_of_string(&self, first: usize, close: usize) -> Option<usize> {
        let quote = self.units[close];
        (first..close)
            .rev()
            .find(|&index| self.units[index] == quote && (index == first || self.units[index - 1] != u16::from(b'\\')))
    }

    /// Where the `(` or `[` is that goes with the one at `close`.
    fn opening_bracket(&self, first: usize, close: usize) -> Option<usize> {
        let mut depth = 0usize;
        let mut index = close + 1;
        while index > first {
            index -= 1;
            let unit = self.units[index];
            match char::from_u32(u32::from(unit)) {
                Some(')' | ']' | '}') => depth += 1,
                Some('(' | '[' | '{') => {
                    depth -= 1;
                    if depth == 0 {
                        return Some(index);
                    }
                }
                Some('"' | '\'' | '`') => index = self.start_of_string(first, index)?,
                _ => {}
            }
        }
        None
    }

    /// Where the bracket that opens at `open` ends, just behind it.
    fn end_of_brackets(&self, open: usize) -> Option<usize> {
        let mut depth = 0usize;
        let mut index = open;
        while index < self.units.len() {
            let unit = self.units[index];
            match char::from_u32(u32::from(unit)) {
                Some('(' | '[' | '{') => depth += 1,
                Some(')' | ']' | '}') => {
                    depth -= 1;
                    if depth == 0 {
                        return Some(index + 1);
                    }
                }
                Some('"' | '\'' | '`') => {
                    index += 1;
                    while index < self.units.len() && self.units[index] != unit {
                        index += usize::from(self.units[index] == u16::from(b'\\')) + 1;
                    }
                }
                _ => {}
            }
            index += 1;
        }
        None
    }

    /// Whether a value ends at `end`, which makes a bracket right there part of the same expression.
    fn value_ends_at(&self, first: usize, end: usize) -> bool {
        if end == first {
            return false;
        }
        let unit = self.units[end - 1];
        if is_word_unit(unit) {
            return !is_keyword_in_front_of_value(&self.units[self.start_of_word(first, end)..end]);
        }
        unit == u16::from(b')') || unit == u16::from(b']') || unit == u16::from(b'`') || is_quote(unit)
    }

    /// The reference that all of what is between `start` and `end` is, or something without a name if it is more than that.
    fn reference_of_range(&self, start: usize, end: usize) -> Reference {
        let (reference, first) = self.reference_ending_at(start, end);
        if first == self.skip_trivia_after(start) {
            reference
        } else {
            Reference::unnamed()
        }
    }

    /// The reference that ends at `end`, and where it starts, going no further back than `first`.
    fn reference_ending_at(&self, first: usize, mut end: usize) -> (Reference, usize) {
        let mut links = Vec::new();
        let base;
        let start;
        loop {
            end = self.skip_trivia_before(first, end);
            if end == first {
                return (Reference::unnamed(), end);
            }
            let last = self.units[end - 1];
            if is_word_unit(last) {
                let mut word_start = self.start_of_word(first, end);
                let is_private = word_start > first && self.units[word_start - 1] == u16::from(b'#');
                word_start -= usize::from(is_private);
                let word = self.units[word_start..end].to_vec();
                let before = self.skip_trivia_before(first, word_start);
                let is_link = before > first
                    && self.units[before - 1] == u16::from(b'.')
                    && !(before >= first + 2 && self.units[before - 2] == u16::from(b'.'));
                if !is_link {
                    start = word_start;
                    base = Self::base_of_word(word);
                    break;
                }
                if before >= first + 2 && self.units[before - 2] == u16::from(b'?') {
                    return (Reference::unnamed(), self.start_of_word(first, before - 2));
                }
                let object_end = self.skip_trivia_before(first, before - 1);
                let object_start = self.start_of_word(first, object_end);
                let object = &self.units[object_start..object_end];
                // NB: These have no text. What comes after them does.
                if equals(object, "new") || equals(object, "import") {
                    start = object_start;
                    base = Base::Unnamed;
                    break;
                }
                links.push(if is_private {
                    Link::PrivateName(word)
                } else {
                    Link::Name(word)
                });
                end = before - 1;
                continue;
            }
            if last == u16::from(b']') {
                let Some(open) = self.opening_bracket(first, end - 1) else {
                    return (Self::with_links(Base::Unnamed, links), end);
                };
                let before = self.skip_trivia_before(first, open);
                if !self.value_ends_at(first, before) {
                    start = open;
                    base = Base::Unnamed;
                    break;
                }
                links.push(Link::Computed(self.reference_of_range(open + 1, end - 1)));
                end = before;
                continue;
            }
            if last == u16::from(b')') {
                let Some(open) = self.opening_bracket(first, end - 1) else {
                    return (Self::with_links(Base::Unnamed, links), end);
                };
                let before = self.skip_trivia_before(first, open);
                if self.value_ends_at(first, before) {
                    // NB: The result of a call has no text, however far the expression goes back.
                    let (_, callee_start) = self.reference_ending_at(first, before);
                    return (Self::with_links(Base::Unnamed, links), callee_start);
                }
                let mut inner = self.reference_of_range(open + 1, end - 1);
                links.reverse();
                inner.links.extend(links);
                return (inner, open);
            }
            if is_quote(last) {
                let Some(open) = self.start_of_string(first, end - 1) else {
                    return (Self::with_links(Base::Unnamed, links), end);
                };
                let mut text = vec![u16::from(b'\'')];
                text.extend_from_slice(&self.units[open + 1..end - 1]);
                text.push(u16::from(b'\''));
                start = open;
                base = Base::Literal(text);
                break;
            }
            return (Self::with_links(Base::Unnamed, links), end);
        }
        (Self::with_links(base, links), start)
    }

    fn with_links(base: Base, mut links: Vec<Link>) -> Reference {
        links.reverse();
        Reference { base, links }
    }

    fn base_of_word(word: Vec<u16>) -> Base {
        if word.first().copied().is_some_and(is_digit) {
            return text_of_number(&word).map_or(Base::Unnamed, Base::Literal);
        }
        if equals(&word, "this") {
            return Base::Literal(word);
        }
        const NO_NAMES: [&str; 5] = ["false", "null", "super", "true", "function"];
        if NO_NAMES.iter().any(|keyword| equals(&word, keyword)) {
            return Base::Unnamed;
        }
        Base::Name(word)
    }

    /// Where the reference ends that starts at `start`, and that is no more than a chain of members.
    fn end_of_reference_at(&self, start: usize) -> Option<usize> {
        let start = self.skip_trivia_after(start);
        let first = self.unit_at(start);
        let mut end = if first == u16::from(b'(') {
            self.end_of_brackets(start)?
        } else if is_quote(first) {
            let mut end = start + 1;
            while end < self.units.len() && self.units[end] != first {
                end += usize::from(self.units[end] == u16::from(b'\\')) + 1;
            }
            // NB: What is available of the source can end in the middle of a string.
            (end + 1).min(self.units.len())
        } else {
            let end = self.end_of_word(start);
            if end == start || equals(&self.units[start..end], "new") {
                return None;
            }
            end
        };
        loop {
            let next = self.skip_trivia_after(end);
            if self.unit_at(next) == u16::from(b'.') {
                let word = self.skip_trivia_after(next + 1);
                let word = word + usize::from(self.unit_at(word) == u16::from(b'#'));
                let word_end = self.end_of_word(word);
                if word_end == word {
                    return Some(end);
                }
                end = word_end;
            } else if self.unit_at(next) == u16::from(b'[') {
                end = self.end_of_brackets(next)?;
            } else {
                return Some(end);
            }
        }
    }

    /// The reference that starts at `start`.
    fn reference_at(&self, start: usize) -> Option<Reference> {
        let end = self.end_of_reference_at(start)?;
        Some(self.reference_of_range(start, end))
    }

    fn is_update_at(&self, at: usize) -> bool {
        self.starts_with_at(at, "++") || self.starts_with_at(at, "--")
    }

    fn is_new_at(&self, at: usize) -> bool {
        self.starts_with_at(at, "new") && !is_word_unit(self.unit_at(at + 3))
    }
}

/// The text of what a call or a `new` at `at` is about, if it has one.
pub fn callee_text(units: &[u16], at: usize) -> Option<Vec<u16>> {
    let scanner = Scanner { units };
    let reference = if scanner.is_new_at(at) {
        scanner.reference_at(at + 3)?
    } else if scanner.unit_at(at) == u16::from(b'(') {
        scanner.reference_ending_at(0, at).0
    } else {
        return None;
    };
    if reference.links.is_empty() && !matches!(reference.base, Base::Name(_)) {
        return None;
    }
    let mut text = Vec::new();
    reference.append_callee_text(&mut text);
    Some(text)
}

/// The text of the base of a member access at `at`, if it has one.
pub fn base_text(units: &[u16], at: usize) -> Option<Vec<u16>> {
    let scanner = Scanner { units };
    let unit = char::from_u32(u32::from(scanner.unit_at(at)))?;
    // NB: All of an optional chain is where it starts, and it has no text.
    if scanner.starts_with_at(at, "?.") {
        return None;
    }
    let reference = match unit {
        '.' | '[' => scanner.reference_ending_at(0, at).0,
        '+' | '-' if scanner.is_update_at(at) && !scanner.value_ends_at(0, scanner.skip_trivia_before(0, at)) => {
            let mut reference = scanner.reference_at(at + 2)?;
            reference.links.pop()?;
            reference
        }
        '(' | '`' | '=' | '+' | '-' | '*' | '/' | '%' | '&' | '|' | '^' | '<' | '>' | '?' => {
            let mut reference = scanner.reference_ending_at(0, at).0;
            // NB: Something computed that gets called has its base left out.
            if matches!(reference.links.pop()?, Link::Computed(_)) && unit == '(' {
                return None;
            }
            reference
        }
        // NB: A tagged template is where its tag starts.
        _ => {
            let end = scanner.end_of_reference_at(at)?;
            if scanner.unit_at(scanner.skip_trivia_after(end)) != u16::from(b'`') {
                return None;
            }
            let mut reference = scanner.reference_of_range(at, end);
            reference.links.pop()?;
            reference
        }
    };
    let mut text = Vec::new();
    reference.append_base_text(&mut text).then_some(text)
}
