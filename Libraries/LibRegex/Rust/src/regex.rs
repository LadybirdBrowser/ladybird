/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/// High-level regex API.
///
/// This is the main entry point for using the regex engine.
use crate::ast::{Alternative, Atom, Disjunction, Flags, Pattern, Term};
use crate::bytecode::{Instruction, NamedGroupEntry, append_code_point_wtf16};
use crate::{compiler, parser, vm};
use std::cell::RefCell;

/// A compiled regular expression.
pub struct Regex {
    /// Program for the backtracking VM (with fused loop optimizations).
    program: crate::bytecode::Program,
    flags: Flags,
    hints: vm::PatternHints,
    /// Pre-computed u16 literal for whole-pattern literal search.
    literal_u16: Option<Vec<u16>>,
    /// Pre-computed u16 alternatives for fast literal alternation matching.
    /// Alternatives stay in source order to preserve leftmost-first semantics.
    literal_alt_u16: Option<Vec<Vec<u16>>>,
    /// Cached VM scratch space for reuse across exec calls.
    scratch: RefCell<vm::VmScratch>,
}

impl Regex {
    /// Compile a regex pattern with the given flags.
    pub fn compile(pattern: &str, flags: Flags) -> Result<Self, parser::Error> {
        let parsed = parser::parse(pattern, flags)?;
        let mut program = compiler::compile(&parsed);
        Self::resolve_properties(&mut program);
        let hints = vm::analyze_pattern(&program, pattern_can_match_empty(&parsed));

        let literal_u16 = extract_literal_u16(&parsed, flags);
        let literal_alt_u16 = extract_literal_alternatives_u16(&parsed, flags);

        Ok(Self {
            program,
            flags,
            hints,
            literal_u16,
            literal_alt_u16,
            scratch: RefCell::new(vm::VmScratch::new()),
        })
    }

    /// Execute the regex, writing captures directly into a provided buffer.
    /// Returns a VmResult indicating match, no-match, or limit exceeded.
    /// The buffer should have `(capture_count + 1) * 2` i32 slots.
    /// On match, captures are written as pairs of (start, end) i32 values.
    pub fn exec_into(&self, input: &[u16], start: usize, out: &mut [i32]) -> vm::VmResult {
        self.exec_into_input(input, start, out)
    }

    pub fn exec_into_ascii(&self, input: &[u8], start: usize, out: &mut [i32]) -> vm::VmResult {
        self.exec_into_input(input, start, out)
    }

    pub(crate) fn exec_into_input<I: vm::Input>(
        &self,
        input: I,
        start: usize,
        out: &mut [i32],
    ) -> vm::VmResult {
        // Fast path for literal patterns: use fast substring search.
        // NB: Literal searches never hit the step limit.
        if let Some(ref needle) = self.literal_u16 {
            return if Self::literal_search(input, start, needle, &self.flags, out) {
                vm::VmResult::Match
            } else {
                vm::VmResult::NoMatch
            };
        }
        // Fast path for literal alternation patterns.
        if let Some(ref alts) = self.literal_alt_u16 {
            return if Self::literal_alt_search(input, start, alts, out) {
                vm::VmResult::Match
            } else {
                vm::VmResult::NoMatch
            };
        }
        let scratch = &mut *self.scratch.borrow_mut();
        vm::execute_into_with_scratch(&self.program, input, start, &self.hints, out, scratch)
    }

    /// Test whether the regex matches anywhere in the input.
    pub fn test(&self, input: &[u16], start: usize) -> vm::VmResult {
        self.test_input(input, start)
    }

    pub fn test_ascii(&self, input: &[u8], start: usize) -> vm::VmResult {
        self.test_input(input, start)
    }

    pub(crate) fn test_input<I: vm::Input>(&self, input: I, start: usize) -> vm::VmResult {
        if let Some(ref needle) = self.literal_u16 {
            return if Self::literal_test(input, start, needle, &self.flags) {
                vm::VmResult::Match
            } else {
                vm::VmResult::NoMatch
            };
        }
        if let Some(ref alts) = self.literal_alt_u16 {
            let mut out = [-1i32; 2];
            return if Self::literal_alt_search(input, start, alts, &mut out) {
                vm::VmResult::Match
            } else {
                vm::VmResult::NoMatch
            };
        }
        // Reuse cached scratch space for the VM. Only need group 0 for test().
        let mut out = [-1i32; 2];
        let scratch = &mut *self.scratch.borrow_mut();
        vm::execute_into_with_scratch(&self.program, input, start, &self.hints, &mut out, scratch)
    }

    /// Fast literal substring search for whole-pattern literal fast paths.
    fn literal_search<I: vm::Input>(
        input: I,
        start: usize,
        needle: &[u16],
        flags: &Flags,
        out: &mut [i32],
    ) -> bool {
        if needle.is_empty() {
            // Empty pattern matches at start position.
            if out.len() >= 2 {
                out[0] = start as i32;
                out[1] = start as i32;
            }
            return true;
        }

        if flags.ignore_case {
            // Case-insensitive literal search — fall back to linear scan with case folding.
            let needle_len = needle.len();
            if start + needle_len > input.len() {
                return false;
            }
            'outer: for pos in start..=input.len() - needle_len {
                for (j, expected) in needle.iter().enumerate() {
                    if !vm::case_fold_eq(
                        input.code_unit(pos + j) as u32,
                        *expected as u32,
                        flags.unicode || flags.unicode_sets,
                    ) {
                        continue 'outer;
                    }
                }
                if out.len() >= 2 {
                    out[0] = pos as i32;
                    out[1] = (pos + needle_len) as i32;
                }
                return true;
            }
            return false;
        }

        // Case-sensitive: use fast first-character scan + verify.
        let first = needle[0];
        let needle_len = needle.len();
        if start + needle_len > input.len() {
            return false;
        }
        let mut pos = start;
        let end = input.len() - needle_len + 1;
        while pos < end {
            // Bulk scan for first character.
            match input.find_code_unit(pos, end, first) {
                Some(candidate_pos) => pos = candidate_pos,
                None => return false,
            }
            // Verify rest of needle.
            if input.matches_u16_at(pos, needle) {
                if out.len() >= 2 {
                    out[0] = pos as i32;
                    out[1] = (pos + needle_len) as i32;
                }
                return true;
            }
            pos += 1;
        }
        false
    }

    /// Fast literal test (no captures needed).
    fn literal_test<I: vm::Input>(input: I, start: usize, needle: &[u16], flags: &Flags) -> bool {
        let mut out = [0i32; 2];
        Self::literal_search(input, start, needle, flags, &mut out)
    }

    /// Fast literal alternation search: find the first matching alternative.
    /// Alternatives are in source order to preserve ECMAScript leftmost-first semantics.
    fn literal_alt_search<I: vm::Input>(
        input: I,
        start: usize,
        alts: &[Vec<u16>],
        out: &mut [i32],
    ) -> bool {
        for pos in start..input.len() {
            let first_ch = input.code_unit(pos);
            for alt in alts {
                if alt[0] != first_ch {
                    continue;
                }
                let alt_len = alt.len();
                if pos + alt_len > input.len() {
                    continue;
                }
                if input.matches_u16_at(pos, alt) {
                    if out.len() >= 2 {
                        out[0] = pos as i32;
                        out[1] = (pos + alt_len) as i32;
                    }
                    return true;
                }
            }
        }
        false
    }

    /// Find all literal matches, writing (start, end) pairs into result_buf.
    fn literal_find_all<I: vm::Input>(
        input: I,
        start: usize,
        needle: &[u16],
        flags: &Flags,
        result_buf: &mut [i32],
    ) -> i32 {
        let capacity = result_buf.len();
        let mut count = 0i32;
        let mut pos = start;
        let mut out = [-1i32; 2];
        loop {
            if pos > input.len() {
                break;
            }
            out[0] = -1;
            out[1] = -1;
            if !Self::literal_search(input, pos, needle, flags, &mut out) {
                break;
            }
            let idx = count as usize * 2;
            if idx + 1 >= capacity {
                return -1;
            }
            result_buf[idx] = out[0];
            result_buf[idx + 1] = out[1];
            count += 1;
            if out[1] == out[0] {
                pos = out[1] as usize + 1;
            } else {
                pos = out[1] as usize;
            }
        }
        count
    }

    /// Find all literal alternation matches, writing (start, end) pairs into result_buf.
    fn literal_alt_find_all<I: vm::Input>(
        input: I,
        start: usize,
        alts: &[Vec<u16>],
        result_buf: &mut [i32],
    ) -> i32 {
        let capacity = result_buf.len();
        let mut count = 0i32;
        let mut pos = start;
        let mut out = [-1i32; 2];
        loop {
            if pos > input.len() {
                break;
            }
            out[0] = -1;
            out[1] = -1;
            if !Self::literal_alt_search(input, pos, alts, &mut out) {
                break;
            }
            let idx = count as usize * 2;
            if idx + 1 >= capacity {
                return -1;
            }
            result_buf[idx] = out[0];
            result_buf[idx + 1] = out[1];
            count += 1;
            if out[1] == out[0] {
                pos = out[1] as usize + 1;
            } else {
                pos = out[1] as usize;
            }
        }
        count
    }

    /// Post-process the program to resolve Unicode property names to ICU enum IDs.
    /// This allows O(1) trie lookups at match time instead of string-based resolution.
    fn resolve_properties(program: &mut crate::bytecode::Program) {
        let resolve = |data: &mut crate::bytecode::UnicodePropertyData| {
            if data.resolved.is_none() {
                data.resolved = compiler::resolve_property(&data.name, data.value.as_deref());
            }
        };
        for inst in &mut program.instructions {
            match inst {
                Instruction::UnicodeProperty(data) => {
                    resolve(data);
                }
                Instruction::StringPropertyMatch { property, .. } => {
                    resolve(property);
                }
                Instruction::GreedyLoop { matcher, .. } | Instruction::LazyLoop { matcher, .. } => {
                    if let crate::bytecode::SimpleMatch::UnicodeProperty(data) = matcher {
                        resolve(data);
                    }
                }
                _ => {}
            }
        }
    }

    /// Get the number of capture groups (not counting group 0).
    pub fn capture_count(&self) -> u32 {
        self.program.capture_count
    }

    /// Get the named capture groups (in order of appearance in the pattern).
    pub fn named_groups(&self) -> &[NamedGroupEntry] {
        &self.program.named_groups
    }

    pub fn is_single_non_bmp_literal(&self) -> bool {
        (self.flags.unicode || self.flags.unicode_sets)
            && matches!(
                self.literal_u16.as_deref(),
                Some([high, low])
                    if (0xD800..=0xDBFF).contains(high) && (0xDC00..=0xDFFF).contains(low)
            )
    }

    /// Find all non-overlapping matches starting from `start`.
    /// Writes (match_start, match_end) i32 pairs directly into `result_buf`.
    /// Returns number of matches found, or -1 if buffer is too small.
    pub fn find_all_into(&self, input: &[u16], start: usize, result_buf: &mut [i32]) -> i32 {
        self.find_all_into_input(input, start, result_buf)
    }

    pub fn find_all_into_ascii(&self, input: &[u8], start: usize, result_buf: &mut [i32]) -> i32 {
        self.find_all_into_input(input, start, result_buf)
    }

    pub(crate) fn find_all_into_input<I: vm::Input>(
        &self,
        input: I,
        start: usize,
        result_buf: &mut [i32],
    ) -> i32 {
        // Fast path for literal patterns.
        if let Some(ref needle) = self.literal_u16 {
            return Self::literal_find_all(input, start, needle, &self.flags, result_buf);
        }
        // Fast path for literal alternation patterns.
        if let Some(ref alts) = self.literal_alt_u16 {
            return Self::literal_alt_find_all(input, start, alts, result_buf);
        }
        // Use the VM-internal find_all loop which reuses a single VM across matches.
        let scratch = &mut *self.scratch.borrow_mut();
        vm::find_all_with_scratch(
            &self.program,
            input,
            start,
            &self.hints,
            result_buf,
            scratch,
        )
    }
}

fn pattern_can_match_empty(pattern: &Pattern) -> bool {
    disjunction_can_match_empty(&pattern.disjunction)
}

fn disjunction_can_match_empty(disjunction: &Disjunction) -> bool {
    disjunction
        .alternatives
        .iter()
        .any(alternative_can_match_empty)
}

fn alternative_can_match_empty(alternative: &Alternative) -> bool {
    alternative.terms.iter().all(term_can_match_empty)
}

fn term_can_match_empty(term: &Term) -> bool {
    if let Some(quantifier) = &term.quantifier
        && quantifier.min == 0
    {
        return true;
    }

    atom_can_match_empty(&term.atom)
}

fn atom_can_match_empty(atom: &Atom) -> bool {
    match atom {
        Atom::Literal(_)
        | Atom::Dot
        | Atom::CharacterClass(_)
        | Atom::BuiltinCharacterClass(_)
        | Atom::UnicodeProperty(_) => false,
        Atom::Group(group) => disjunction_can_match_empty(&group.body),
        Atom::NonCapturingGroup(group) => disjunction_can_match_empty(&group.body),
        Atom::Lookaround(_) | Atom::Assertion(_) => true,
        // Backreferences can match empty if their capture is unset or empty.
        Atom::Backreference(_) => true,
        Atom::ModifierGroup(group) => disjunction_can_match_empty(&group.body),
    }
}

fn extract_literal_u16(pattern: &Pattern, flags: Flags) -> Option<Vec<u16>> {
    if flags.ignore_case && (flags.unicode || flags.unicode_sets) {
        return None;
    }
    if pattern.disjunction.alternatives.len() != 1 {
        return None;
    }

    let alternative = &pattern.disjunction.alternatives[0];
    let mut literal = Vec::new();
    for term in &alternative.terms {
        if term.quantifier.is_some() {
            return None;
        }
        let Atom::Literal(cp) = term.atom else {
            return None;
        };
        if is_unsafe_unicode_literal(cp, flags) {
            return None;
        }
        append_code_point_wtf16(&mut literal, cp)?;
    }

    Some(literal)
}

fn extract_literal_alternatives_u16(pattern: &Pattern, flags: Flags) -> Option<Vec<Vec<u16>>> {
    if flags.ignore_case {
        return None;
    }

    let alternatives = &pattern.disjunction.alternatives;
    if alternatives.len() < 2 || pattern.capture_count > 0 {
        return None;
    }

    let mut literal_alternatives = Vec::with_capacity(alternatives.len());
    for alternative in alternatives {
        let mut literal = Vec::new();
        for term in &alternative.terms {
            if term.quantifier.is_some() {
                return None;
            }
            let Atom::Literal(cp) = term.atom else {
                return None;
            };
            if is_unsafe_unicode_literal(cp, flags) {
                return None;
            }
            append_code_point_wtf16(&mut literal, cp)?;
        }
        if literal.is_empty() {
            return None;
        }
        literal_alternatives.push(literal);
    }

    Some(literal_alternatives)
}

fn is_unsafe_unicode_literal(cp: u32, flags: Flags) -> bool {
    (flags.unicode || flags.unicode_sets) && (0xD800..=0xDFFF).contains(&cp)
}
