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

pub use libunicode_rust::character_types::PropertyKind;
pub use libunicode_rust::character_types::ResolvedProperty;

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

    // The data of the instructions that do not fit in an instruction. An instruction refers to its data by its index in
    // one of these tables, so that each instruction is as small as the most common instructions.
    /// The code units of `Literal` instructions after their first.
    pub literals: Vec<u16>,
    /// The character classes of `CharClass` instructions.
    pub classes: Vec<CharRanges>,
    /// The properties of `UnicodeProperty` instructions.
    pub unicode_properties: Vec<UnicodePropertyData>,
    /// The group names of `BackrefNamed` instructions.
    pub backref_names: Vec<String>,
    /// The data of `RepeatCheck` instructions.
    pub repeat_checks: Vec<RepeatCheck>,
    /// The data of `StringPropertyMatch` instructions.
    pub string_properties: Vec<StringProperty>,
    /// The registers that `ProgressCheck` instructions clear, as lists packed as [len, reg0, reg1, ..., len, ...].
    pub register_lists: Vec<u32>,
    /// The data of `GreedyLoop` and `LazyLoop` instructions.
    pub loops: Vec<SimpleLoop>,
}

/// A single bytecode instruction in Ladybird's concrete implementation of the
/// abstract matchers from ECMA-262 Pattern Semantics.
/// <https://tc39.es/ecma262/#sec-pattern-semantics>
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Instruction {
    /// Match a single character (u32 code point, supports WTF-16 lone surrogates).
    Char(u32),

    /// Match a single character, case-insensitive (u32 code points).
    CharNoCase(u32, u32),

    /// Match the run of literal code units of `first` and then `Program::literals[rest_start..rest_start + rest_len]`,
    /// as the `Char` instructions of each code unit would, which compare under case folding if `ignore_case` is set.
    /// The first code unit is in the instruction, as most attempts to match a literal fail at it.
    Literal {
        first: u16,
        ignore_case: bool,
        rest_start: u32,
        rest_len: u32,
    },

    /// Match any character (`.`). If `dot_all` is true, matches newlines too.
    AnyChar { dot_all: bool },

    /// Match a character in the set of ranges `Program::classes[class]`. `negated` inverts the match.
    CharClass { class: u32, negated: bool },

    /// Match a built-in character class (\d, \w, \s and negations).
    BuiltinClass(BuiltinCharacterClass),

    /// Match the Unicode property `Program::unicode_properties[index]`.
    UnicodeProperty(u32),

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

    /// Named backreference, to the group named `Program::backref_names[index]`.
    BackrefNamed(u32),

    /// Begin a repetition counter at register `counter_reg`.
    /// Sets the register to 0.
    RepeatStart { counter_reg: u32 },

    /// Check repetition: if counter < max, increment and jump to `body`.
    /// Otherwise fall through. Used with Split for greedy/lazy.
    /// The bounds of the check are `Program::repeat_checks[check]`.
    RepeatCheck { check: u32, body: u32 },

    /// Lookahead/lookbehind assertion.
    /// `positive`: whether match must succeed or fail.
    /// `forward`: lookahead (true) or lookbehind (false).
    /// `body`: start of the assertion body.
    /// `end`: instruction after the assertion.
    LookStart { positive: bool, forward: bool, end: u32 },

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

    /// Atomically match one string from the Unicode string property `Program::string_properties[index]`.
    /// Tries multi-codepoint strings first (longest match wins), then falls
    /// back to single-codepoint UnicodeProperty match. Does not create
    /// backtrack points -- once a match is found, it's committed.
    StringPropertyMatch(u32),

    /// Progress check: save position at `reg`, fail if no progress since last visit.
    /// Used to prevent infinite loops in zero-width quantifier bodies.
    /// The registers of the list at `Program::register_lists[clear_captures]` are cleared to -1 before
    /// backtracking on zero-width, per ECMA-262 RepeatMatcher step 2.b.
    ProgressCheck { reg: u32, clear_captures: u32 },

    /// Greedy loop for simple character matchers.
    /// Greedily consumes as many matching characters as possible, then pushes a
    /// single backtrack state. On backtrack, gives up one character at a time.
    /// This avoids per-iteration Split/backtrack overhead for simple quantifiers.
    /// The loop is `Program::loops[index]`. `same_matcher_suffix` tells if the loop has a `same_matcher_suffix_min`,
    /// so that a backtrack into the loop looks at the table only then.
    GreedyLoop { index: u32, same_matcher_suffix: bool },

    /// Lazy loop for simple character matchers.
    /// Tries to match as few characters as possible, then on backtrack consumes one more.
    /// The loop is `Program::loops[index]`.
    LazyLoop(u32),
}

// A program is mostly instructions, so their size is most of the memory of a regex.
const _: () = assert!(size_of::<Instruction>() == 12);

/// The data of a `RepeatCheck` instruction.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct RepeatCheck {
    pub counter_reg: u32,
    pub min: u32,
    pub max: Option<u32>,
    pub greedy: bool,
}

/// The data of a `StringPropertyMatch` instruction.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct StringProperty {
    /// Multi-codepoint strings, sorted longest first and packed as:
    /// [len, cp0, cp1, ..., len, cp0, ...]
    pub strings: Box<[u32]>,
    /// Fallback for single-codepoint matches.
    pub property: UnicodePropertyData,
}

/// The data of a `GreedyLoop` or `LazyLoop` instruction.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SimpleLoop {
    pub matcher: SimpleMatch,
    pub min: u32,
    pub max: Option<u32>,
    /// The sum of the minimum counts of the loops right after this one that match with the same matcher, if one of
    /// them has a minimum count, so a greedy loop can give back that suffix in one step.
    pub same_matcher_suffix_min: Option<u32>,
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
    CharClass { ranges: CharRanges, negated: bool },
    /// Built-in class (\d, \w, \s, etc.)
    BuiltinClass(BuiltinCharacterClass),
    /// Unicode property (\p{...}, \P{...}).
    UnicodeProperty(Box<UnicodePropertyData>),
    /// Union of two matchers: matches if either alternative matches.
    Union(Box<SimpleMatch>, Box<SimpleMatch>),
}

/// The ranges of a character class, sorted, with its ASCII members in a bitmap that spares the search through the
/// ranges for most characters of most inputs.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CharRanges {
    ranges: Vec<CharRange>,
    ascii: [u64; 2],
}

impl CharRanges {
    pub fn new(ranges: Vec<CharRange>) -> Self {
        let mut ascii = [0u64; 2];
        for range in &ranges {
            for code_point in range.start..=range.end.min(0x7F) {
                ascii[(code_point / 64) as usize] |= 1 << (code_point % 64);
            }
        }
        Self { ranges, ascii }
    }

    /// Whether `code_point` is in one of the ranges.
    #[inline(always)]
    pub fn contains(&self, code_point: u32) -> bool {
        if code_point < 0x80 {
            return self.ascii[(code_point / 64) as usize] & (1 << (code_point % 64)) != 0;
        }
        crate::vm::char_in_ranges(code_point, &self.ranges)
    }

    pub fn into_vec(self) -> Vec<CharRange> {
        self.ranges
    }
}

impl core::ops::Deref for CharRanges {
    type Target = [CharRange];

    fn deref(&self) -> &[CharRange] {
        &self.ranges
    }
}

/// A character range for CharClass instructions (u32 code points).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CharRange {
    pub start: u32,
    pub end: u32,
}

/// Re-export for use by the compiler and VM.
pub use libregex_compiler::ast::BuiltinCharacterClass;

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
            literals: Vec::new(),
            classes: Vec::new(),
            unicode_properties: Vec::new(),
            backref_names: Vec::new(),
            repeat_checks: Vec::new(),
            string_properties: Vec::new(),
            register_lists: Vec::new(),
            loops: Vec::new(),
        }
    }

    #[inline(always)]
    pub fn literal_rest(&self, start: u32, len: u32) -> &[u16] {
        &self.literals[start as usize..start as usize + len as usize]
    }

    /// The code unit that `instruction` matches first, if it is a `Char` of the BMP or a `Literal` without case
    /// folding.
    #[inline(always)]
    pub fn leading_code_unit(&self, instruction: Option<&Instruction>) -> Option<u16> {
        match instruction? {
            Instruction::Char(code_point) => u16::try_from(*code_point).ok(),
            Instruction::Literal {
                first,
                ignore_case: false,
                ..
            } => Some(*first),
            _ => None,
        }
    }

    #[inline(always)]
    pub fn class(&self, index: u32) -> &CharRanges {
        &self.classes[index as usize]
    }

    #[inline(always)]
    pub fn unicode_property(&self, index: u32) -> &UnicodePropertyData {
        &self.unicode_properties[index as usize]
    }

    #[inline(always)]
    pub fn backref_name(&self, index: u32) -> &str {
        &self.backref_names[index as usize]
    }

    #[inline(always)]
    pub fn repeat_check(&self, index: u32) -> &RepeatCheck {
        &self.repeat_checks[index as usize]
    }

    #[inline(always)]
    pub fn string_property(&self, index: u32) -> &StringProperty {
        &self.string_properties[index as usize]
    }

    /// The registers of the list that starts at `index` in `register_lists`.
    #[inline(always)]
    pub fn register_list(&self, index: u32) -> &[u32] {
        let start = index as usize;
        let len = self.register_lists[start] as usize;
        &self.register_lists[start + 1..start + 1 + len]
    }

    #[inline(always)]
    pub fn simple_loop(&self, index: u32) -> &SimpleLoop {
        &self.loops[index as usize]
    }

    /// Set `same_matcher_suffix_min` of the loop of each `GreedyLoop` instruction.
    ///
    /// The suffix of a loop is the run of `GreedyLoop` and `LazyLoop` instructions right after it
    /// that have the same matcher. One backward pass keeps the run that starts at `pc + 1`.
    pub fn compute_same_matcher_suffix_mins(&mut self) {
        // The loop at `pc + 1`, and the sum (None on overflow) and positive-min flag of the run
        // of loops with its matcher that starts there.
        let mut next_run: Option<(u32, Option<u32>, bool)> = None;

        for pc in (0..self.instructions.len()).rev() {
            let index = match self.instructions[pc] {
                Instruction::GreedyLoop { index, .. } | Instruction::LazyLoop(index) => index,
                _ => {
                    next_run = None;
                    continue;
                }
            };
            let simple_loop = &self.loops[index as usize];

            let (suffix_total, suffix_has_positive_min) = match next_run {
                Some((next_index, total, has_positive_min))
                    if self.loops[next_index as usize].matcher == simple_loop.matcher =>
                {
                    (total, has_positive_min)
                }
                _ => (Some(0), false),
            };
            next_run = Some((
                index,
                suffix_total.and_then(|total| total.checked_add(simple_loop.min)),
                suffix_has_positive_min || simple_loop.min > 0,
            ));

            if let Instruction::GreedyLoop { .. } = self.instructions[pc] {
                let suffix_min = if suffix_has_positive_min { suffix_total } else { None };
                self.loops[index as usize].same_matcher_suffix_min = suffix_min;
                self.instructions[pc] = Instruction::GreedyLoop {
                    index,
                    same_matcher_suffix: suffix_min.is_some(),
                };
            }
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
