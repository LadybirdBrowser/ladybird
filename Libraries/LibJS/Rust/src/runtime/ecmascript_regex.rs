/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The compiled regular expressions of RegExp objects, which call the libregex_rust crate directly, where C++ code goes
//! through the wrappers of Libraries/LibRegex/ECMAScriptRegex.cpp and RustRegex.cpp.

use core::cell::RefCell;

use ak::Utf16FlyString;
use libregex_rust::ast::Flags;
use libregex_rust::regex::Regex;
use libregex_rust::vm::VmResult;

use crate::utf16::Utf16View;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum MatchResult {
    Match,
    NoMatch,
    LimitExceeded,
}

impl MatchResult {
    fn from_vm_result(result: VmResult) -> Self {
        match result {
            VmResult::Match => Self::Match,
            VmResult::NoMatch => Self::NoMatch,
            VmResult::LimitExceeded => Self::LimitExceeded,
        }
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct EcmaScriptCompileFlags {
    pub global: bool,
    pub ignore_case: bool,
    pub multiline: bool,
    pub dot_all: bool,
    pub unicode: bool,
    pub unicode_sets: bool,
    pub sticky: bool,
    pub has_indices: bool,
}

pub struct EcmaScriptNamedCaptureGroup {
    pub name: Utf16FlyString,
    pub index: u32,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct MatchPair {
    pub start: i32,
    pub end: i32,
}

/// What rust_regex_find_all returns when the buffer it was given is too small for every match.
const FIND_ALL_BUFFER_TOO_SMALL: i32 = -1;

/// regex::ECMAScriptRegex. An exec leaves its captures in a buffer of the regex, which capture_slot() reads until the
/// next exec.
pub struct EcmaScriptRegex {
    regex: Regex,
    named_groups: Vec<EcmaScriptNamedCaptureGroup>,
    capture_buffer: RefCell<Vec<i32>>,
    find_all_buffer: RefCell<Vec<i32>>,
}

impl EcmaScriptRegex {
    pub fn compile(pattern: Utf16View<'_>, flags: EcmaScriptCompileFlags) -> Result<EcmaScriptRegex, String> {
        let rust_flags = Flags {
            global: flags.global,
            ignore_case: flags.ignore_case,
            multiline: flags.multiline,
            dot_all: flags.dot_all,
            unicode: flags.unicode,
            unicode_sets: flags.unicode_sets,
            sticky: flags.sticky,
            has_indices: flags.has_indices,
        };

        // Converted as rust_regex_compile_ascii and rust_regex_compile convert the pattern they are passed.
        let pattern_characters = match pattern {
            Utf16View::Ascii(bytes) => bytes.iter().map(|&byte| char::from(byte)).collect(),
            Utf16View::Utf16(code_units) => char::decode_utf16(code_units.iter().copied())
                .map(|character| character.unwrap_or(char::REPLACEMENT_CHARACTER))
                .collect(),
        };

        let regex = Regex::compile_chars(pattern_characters, rust_flags).map_err(|error| error.to_string())?;

        let named_groups = regex
            .named_groups()
            .iter()
            .map(|group| EcmaScriptNamedCaptureGroup {
                name: Utf16FlyString::from_utf8(&group.name),
                index: group.index,
            })
            .collect();

        Ok(EcmaScriptRegex {
            regex,
            named_groups,
            capture_buffer: RefCell::new(Vec::new()),
            find_all_buffer: RefCell::new(Vec::new()),
        })
    }

    /// Execute and fill internal capture buffer.
    /// After a successful call, read results via capture_slot().
    pub fn exec(&self, input: Utf16View<'_>, start_position: usize) -> MatchResult {
        let slots = self.total_groups() as usize * 2;
        let mut capture_buffer = self.capture_buffer.borrow_mut();
        capture_buffer.resize(slots, 0);
        let result = match input {
            Utf16View::Ascii(bytes) => self.regex.exec_into_ascii(bytes, start_position, &mut capture_buffer),
            Utf16View::Utf16(code_units) => self.regex.exec_into(code_units, start_position, &mut capture_buffer),
        };
        MatchResult::from_vm_result(result)
    }

    /// Read a capture slot from the internal buffer (after exec).
    /// Even slots are start positions, odd slots are end positions.
    /// Returns -1 for unmatched captures.
    pub fn capture_slot(&self, slot: u32) -> i32 {
        self.capture_buffer.borrow()[slot as usize]
    }

    /// Test for a match without filling capture buffer.
    pub fn test(&self, input: Utf16View<'_>, start_position: usize) -> MatchResult {
        let result = match input {
            Utf16View::Ascii(bytes) => self.regex.test_ascii(bytes, start_position),
            Utf16View::Utf16(code_units) => self.regex.test(code_units, start_position),
        };
        MatchResult::from_vm_result(result)
    }

    /// Number of numbered capture groups (excluding group 0).
    pub fn capture_count(&self) -> u32 {
        self.regex.capture_count()
    }

    /// Total number of capture groups including group 0.
    pub fn total_groups(&self) -> u32 {
        self.regex.capture_count() + 1
    }

    pub fn is_single_non_bmp_literal(&self) -> bool {
        self.regex.is_single_non_bmp_literal()
    }

    /// Named capture groups with their indices.
    pub fn named_groups(&self) -> &[EcmaScriptNamedCaptureGroup] {
        &self.named_groups
    }

    /// Find all non-overlapping matches. Returns number of matches found, or a negative number when the step limit
    /// was exceeded.
    /// Access results via find_all_match(i) after calling.
    pub fn find_all(&self, input: Utf16View<'_>, start_position: usize) -> i32 {
        let mut find_all_buffer = self.find_all_buffer.borrow_mut();
        // Start with reasonable capacity; keep doubling until it fits.
        if find_all_buffer.len() < 256 {
            find_all_buffer.resize(256, 0);
        }

        loop {
            let result = match input {
                Utf16View::Ascii(bytes) => self
                    .regex
                    .find_all_into_ascii(bytes, start_position, &mut find_all_buffer),
                Utf16View::Utf16(code_units) => {
                    self.regex
                        .find_all_into(code_units, start_position, &mut find_all_buffer)
                }
            };
            if result != FIND_ALL_BUFFER_TOO_SMALL {
                return result;
            }
            let doubled_length = find_all_buffer.len() * 2;
            find_all_buffer.resize(doubled_length, 0);
        }
    }

    /// Get the i-th match from find_all results.
    pub fn find_all_match(&self, index: i32) -> MatchPair {
        let find_all_buffer = self.find_all_buffer.borrow();
        let index = index as usize;
        MatchPair {
            start: find_all_buffer[index * 2],
            end: find_all_buffer[index * 2 + 1],
        }
    }
}
