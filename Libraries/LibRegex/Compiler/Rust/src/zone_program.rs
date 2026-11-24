/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The program that the compiler makes of a pattern, in the zone of the compile.
//!
//! It has the instructions of the program that the VM runs (see `libregex_rust::bytecode::Program`), but its lists
//! and names are slices and strings of the zone, so it is plain data with the lifetime `'z` of the zone. What a compile
//! keeps is a copy of it at its final size, outside of the zone.

use crate::ast::BuiltinCharacterClass;
use crate::ast::NamedGroup;
use crate::ast::UnicodeProperty;

/// A compiled regex program in a zone.
///
/// Spec model: the internal matcher produced by `CompilePattern`.
/// <https://tc39.es/ecma262/#sec-compilepattern>
#[derive(Debug, Clone, Copy)]
pub struct ZoneProgram<'z> {
    /// The bytecode instructions.
    pub instructions: &'z [ZoneInstruction<'z>],
    /// Number of capture groups (not counting group 0).
    pub capture_count: u32,
    /// Total number of registers needed (2 per capture group + 2 for group 0).
    pub register_count: u32,
    /// Whether Unicode mode is enabled (affects surrogate pair decoding).
    pub unicode: bool,
    /// Whether v-flag (unicode sets) mode is enabled.
    pub unicode_sets: bool,
    /// Base ignore_case flag from pattern flags.
    pub ignore_case: bool,
    /// Base multiline flag from pattern flags.
    pub multiline: bool,
    /// Base dot_all flag from pattern flags.
    pub dot_all: bool,
    /// Named capture groups, in order of appearance.
    pub named_groups: &'z [NamedGroup<'z>],
}

/// A single bytecode instruction, as `libregex_rust::bytecode::Instruction` has it, with the variable data in the zone.
/// <https://tc39.es/ecma262/#sec-pattern-semantics>
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ZoneInstruction<'z> {
    /// Match a single character (u32 code point, supports WTF-16 lone surrogates).
    Char(u32),

    /// Match a single character, case-insensitive (u32 code points).
    CharNoCase(u32, u32),

    /// Match any character (`.`). If `dot_all` is true, matches newlines too.
    AnyChar { dot_all: bool },

    /// Match a character in a set of ranges, which are sorted. `negated` inverts the match.
    CharClass {
        ranges: &'z [CodePointRange],
        negated: bool,
    },

    /// Match a built-in character class (\d, \w, \s and negations).
    BuiltinClass(BuiltinCharacterClass),

    /// Match a Unicode property (by reference to keep the instruction small).
    UnicodeProperty(&'z UnicodeProperty<'z>),

    /// Unconditional jump to target instruction.
    Jump(u32),

    /// Split execution: try `prefer` first, backtrack to `other`.
    Split { prefer: u32, other: u32 },

    /// Save current input position to register `reg`.
    Save(u32),

    /// Clear register `reg` to -1 (no match).
    ClearRegister(u32),

    /// Assert start of input (or line if multiline).
    AssertStart { multiline: bool },

    /// Assert end of input (or line if multiline).
    AssertEnd { multiline: bool },

    /// Assert word boundary.
    AssertWordBoundary,

    /// Assert non-word boundary.
    AssertNonWordBoundary,

    /// Match succeeded.
    Match,

    /// Fail — force backtrack.
    Fail,

    /// Backreference: match the same string as capture group `index`.
    Backref(u32),

    /// Named backreference.
    BackrefNamed(&'z str),

    /// Begin a repetition counter at register `counter_reg`.
    RepeatStart { counter_reg: u32 },

    /// Check repetition: if counter < max, increment and jump to `body`.
    RepeatCheck {
        counter_reg: u32,
        min: u32,
        max: Option<u32>,
        body: u32,
        greedy: bool,
    },

    /// Lookahead/lookbehind assertion.
    LookStart { positive: bool, forward: bool, end: u32 },

    /// End of a lookaround body.
    LookEnd,

    /// Push modifier flags onto the modifier stack.
    PushModifiers {
        ignore_case: Option<bool>,
        multiline: Option<bool>,
        dot_all: Option<bool>,
    },

    /// Pop modifier flags from the modifier stack.
    PopModifiers,

    /// Atomically match one string from a Unicode string property.
    StringPropertyMatch {
        /// Multi-codepoint strings, sorted longest first and packed as:
        /// [len, cp0, cp1, ..., len, cp0, ...]
        strings: &'z [u32],
        /// Fallback for single-codepoint matches.
        property: &'z UnicodeProperty<'z>,
    },

    /// Progress check: save position at `reg`, fail if no progress since last visit.
    ProgressCheck { reg: u32, clear_captures: &'z [u32] },

    /// Greedy loop for simple character matchers.
    GreedyLoop {
        matcher: ZoneSimpleMatch<'z>,
        min: u32,
        max: Option<u32>,
    },

    /// Lazy loop for simple character matchers.
    LazyLoop {
        matcher: ZoneSimpleMatch<'z>,
        min: u32,
        max: Option<u32>,
    },
}

/// A simple character matcher for optimized greedy/lazy loops, as `libregex_rust::bytecode::SimpleMatch` has it.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ZoneSimpleMatch<'z> {
    /// Any character (`.`), with dot_all flag.
    AnyChar { dot_all: bool },
    /// A single character.
    Char(u32),
    /// Case-insensitive character.
    CharNoCase(u32, u32),
    /// Character class (set of sorted ranges), negated flag.
    CharClass {
        ranges: &'z [CodePointRange],
        negated: bool,
    },
    /// Built-in class (\d, \w, \s, etc.)
    BuiltinClass(BuiltinCharacterClass),
    /// Unicode property (\p{...}, \P{...}).
    UnicodeProperty(&'z UnicodeProperty<'z>),
    /// Union of two matchers: matches if either alternative matches.
    Union(&'z ZoneSimpleMatch<'z>, &'z ZoneSimpleMatch<'z>),
}

/// A range of code points of a character class, both ends included.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct CodePointRange {
    pub start: u32,
    pub end: u32,
}
