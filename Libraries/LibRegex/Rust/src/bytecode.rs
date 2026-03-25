/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Bytecode instruction set for the regex VM.
//!
//! This is an implementation artifact for the matcher closures described in
//! ECMA-262 Pattern Semantics.
//!
//! Spec:
//! - <https://tc39.es/ecma262/#sec-pattern-semantics>
//! - <https://tc39.es/ecma262/#sec-compilepattern>
//!
//! Instructions are compact and operate on a virtual machine with:
//! - A current position in the input string
//! - A set of registers for capture group positions
//! - A backtrack stack for saving/restoring state

/// A named capture group mapping derived from the pattern's named captures.
/// <https://tc39.es/ecma262/#sec-parsepattern>
#[derive(Debug, Clone)]
pub struct NamedGroupEntry {
    pub name: String,
    pub index: u32,
}

/// A compiled regex program.
///
/// Spec model: the internal matcher produced by `CompilePattern`.
/// <https://tc39.es/ecma262/#sec-compilepattern>
#[derive(Debug, Clone)]
pub struct Program {
    /// The bytecode instructions.
    pub instructions: Vec<Instruction>,
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
    /// Named capture groups (name → group index).
    pub named_groups: Vec<NamedGroupEntry>,
}

/// A single bytecode instruction in Ladybird's concrete implementation of the
/// abstract matchers from ECMA-262 Pattern Semantics.
/// <https://tc39.es/ecma262/#sec-pattern-semantics>
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Instruction {
    /// Match a single character (u32 code point, supports WTF-16 lone surrogates).
    Char(u32),

    /// Match a single character, case-insensitive (u32 code points).
    CharNoCase(u32, u32),

    /// Match any character (`.`). If `dot_all` is true, matches newlines too.
    AnyChar { dot_all: bool },

    /// Match a character in a set of ranges. `negated` inverts the match.
    CharClass {
        ranges: Vec<CharRange>,
        negated: bool,
    },

    /// Match a built-in character class (\d, \w, \s and negations).
    BuiltinClass(BuiltinCharacterClass),

    /// Match a Unicode property (boxed to keep Instruction small).
    UnicodeProperty(Box<UnicodePropertyData>),

    /// Unconditional jump to target instruction.
    Jump(u32),

    /// Split execution: try `prefer` first, backtrack to `other`.
    /// This is the fundamental backtracking primitive.
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
    BackrefNamed(String),

    /// Begin a repetition counter at register `counter_reg`.
    /// Sets the register to 0.
    RepeatStart { counter_reg: u32 },

    /// Check repetition: if counter < max, increment and jump to `body`.
    /// Otherwise fall through. Used with Split for greedy/lazy.
    RepeatCheck {
        counter_reg: u32,
        min: u32,
        max: Option<u32>,
        body: u32,
        greedy: bool,
    },

    /// Lookahead/lookbehind assertion.
    /// `positive`: whether match must succeed or fail.
    /// `forward`: lookahead (true) or lookbehind (false).
    /// `body`: start of the assertion body.
    /// `end`: instruction after the assertion.
    LookStart {
        positive: bool,
        forward: bool,
        end: u32,
    },

    /// End of a lookaround body. Signals success of the assertion sub-match.
    LookEnd,

    /// Push modifier flags onto the modifier stack.
    PushModifiers {
        ignore_case: Option<bool>,
        multiline: Option<bool>,
        dot_all: Option<bool>,
    },

    /// Pop modifier flags from the modifier stack.
    PopModifiers,

    /// No-op, used as a placeholder during compilation.
    Nop,

    /// Atomically match one string from a Unicode string property.
    /// Tries multi-codepoint strings first (longest match wins), then falls
    /// back to single-codepoint UnicodeProperty match. Does not create
    /// backtrack points -- once a match is found, it's committed.
    StringPropertyMatch {
        /// Multi-codepoint strings, sorted longest first and packed as:
        /// [len, cp0, cp1, ..., len, cp0, ...]
        strings: Box<[u32]>,
        /// Fallback for single-codepoint matches.
        property: Box<UnicodePropertyData>,
    },

    /// Progress check: save position at `reg`, fail if no progress since last visit.
    /// Used to prevent infinite loops in zero-width quantifier bodies.
    /// When `clear_captures` is set, those registers are cleared to -1 before
    /// backtracking on zero-width, per ECMA-262 RepeatMatcher step 2.b.
    ProgressCheck { reg: u32, clear_captures: Vec<u32> },

    /// Greedy loop for simple character matchers.
    /// Greedily consumes as many matching characters as possible, then pushes a
    /// single backtrack state. On backtrack, gives up one character at a time.
    /// This avoids per-iteration Split/backtrack overhead for simple quantifiers.
    GreedyLoop {
        matcher: SimpleMatch,
        min: u32,
        max: Option<u32>,
    },

    /// Lazy loop for simple character matchers.
    /// Tries to match as few characters as possible, then on backtrack consumes one more.
    LazyLoop {
        matcher: SimpleMatch,
        min: u32,
        max: Option<u32>,
    },
}

/// The kind of a resolved Unicode property.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum PropertyKind {
    Script = 0,
    ScriptExtension = 1,
    GeneralCategory = 2,
    BinaryProperty = 3,
}

impl PropertyKind {
    pub fn from_u8(v: u8) -> Option<Self> {
        match v {
            0 => Some(Self::Script),
            1 => Some(Self::ScriptExtension),
            2 => Some(Self::GeneralCategory),
            3 => Some(Self::BinaryProperty),
            _ => None,
        }
    }
}

/// A resolved Unicode property — the string name/value has been resolved to
/// an ICU enum at compile time, so match-time lookups avoid string parsing.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct ResolvedProperty {
    /// The kind of Unicode property (Script, GeneralCategory, etc.).
    pub kind: PropertyKind,
    /// ICU enum value (e.g. script code, general category, binary property ID).
    pub id: u32,
}

/// Data for a Unicode property match instruction.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct UnicodePropertyData {
    pub negated: bool,
    pub name: String,
    pub value: Option<String>,
    /// Resolved property for fast O(1) ICU trie lookups at match time.
    pub resolved: Option<ResolvedProperty>,
}

/// A simple character matcher for optimized greedy/lazy loops.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum SimpleMatch {
    /// Any character (`.`), with dot_all flag.
    AnyChar { dot_all: bool },
    /// A single character.
    Char(u32),
    /// Case-insensitive character.
    CharNoCase(u32, u32),
    /// Character class (set of ranges), negated flag.
    CharClass {
        ranges: Vec<CharRange>,
        negated: bool,
    },
    /// Built-in class (\d, \w, \s, etc.)
    BuiltinClass(BuiltinCharacterClass),
    /// Unicode property (\p{...}, \P{...}).
    UnicodeProperty(Box<UnicodePropertyData>),
}

/// A character range for CharClass instructions (u32 code points).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CharRange {
    pub start: u32,
    pub end: u32,
}

/// Re-export for use by the compiler and VM.
pub use crate::ast::BuiltinCharacterClass;

impl Default for Program {
    fn default() -> Self {
        Self::new()
    }
}

impl Program {
    pub fn new() -> Self {
        Self {
            instructions: Vec::new(),
            capture_count: 0,
            register_count: 2, // group 0 always exists
            unicode: false,
            unicode_sets: false,
            ignore_case: false,
            multiline: false,
            dot_all: false,
            named_groups: Vec::new(),
        }
    }

    pub fn emit(&mut self, inst: Instruction) -> u32 {
        let idx = self.instructions.len() as u32;
        self.instructions.push(inst);
        idx
    }

    pub fn current_offset(&self) -> u32 {
        self.instructions.len() as u32
    }

    pub fn patch_jump(&mut self, at: u32, target: u32) {
        match &mut self.instructions[at as usize] {
            Instruction::Jump(t) => *t = target,
            Instruction::Split { prefer, .. } if *prefer == u32::MAX => *prefer = target,
            Instruction::Split { other, .. } if *other == u32::MAX => *other = target,
            inst => panic!("cannot patch non-jump instruction: {inst:?}"),
        }
    }
}

/// Encode a Unicode code point as WTF-16 into `out`.
/// Returns `None` if the code point is out of range (> U+10FFFF).
pub fn append_code_point_wtf16(out: &mut Vec<u16>, cp: u32) -> Option<()> {
    if cp <= 0xFFFF {
        out.push(cp as u16);
        return Some(());
    }
    if cp > 0x10FFFF {
        return None;
    }
    let cp = cp - 0x10000;
    out.push(0xD800 | ((cp >> 10) as u16));
    out.push(0xDC00 | ((cp & 0x3FF) as u16));
    Some(())
}
