/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Backtracking regex VM operating on ASCII or WTF-16 input.
//!
//! Executes the compiled matcher against an input string using an explicit
//! backtrack stack instead of recursion.
//!
//! Spec:
//! - <https://tc39.es/ecma262/#sec-pattern-semantics>
//! - <https://tc39.es/ecma262/#sec-regexpbuiltinexec>
use crate::bytecode::*;

/// Maximum number of steps before aborting (prevents ReDoS).
const MATCH_LIMIT: u64 = 10_000_000;

/// Tri-state result from a single VM execution.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum VmResult {
    Match,
    NoMatch,
    LimitExceeded,
}

pub trait Input: Copy {
    fn len(self) -> usize;
    fn code_unit(self, pos: usize) -> u16;

    #[inline(always)]
    fn is_empty(self) -> bool {
        self.len() == 0
    }

    #[inline(always)]
    fn find_code_unit(self, start: usize, end: usize, ch16: u16) -> Option<usize> {
        let mut pos = start;
        while pos < end {
            if self.code_unit(pos) == ch16 {
                return Some(pos);
            }
            pos += 1;
        }
        None
    }

    #[inline(always)]
    fn next_literal_start(self, start_pos: usize, ch16: u16) -> Option<usize> {
        self.find_code_unit(start_pos, self.len(), ch16)
    }

    #[inline(always)]
    fn matches_u16_at(self, pos: usize, needle: &[u16]) -> bool {
        if pos + needle.len() > self.len() {
            return false;
        }
        for (offset, expected) in needle.iter().enumerate() {
            if self.code_unit(pos + offset) != *expected {
                return false;
            }
        }
        true
    }

    #[inline(always)]
    fn ends_with_u16(self, suffix: &[u16]) -> bool {
        if suffix.len() > self.len() {
            return false;
        }
        self.matches_u16_at(self.len() - suffix.len(), suffix)
    }

    #[inline(always)]
    fn ranges_equal(self, left_start: usize, right_start: usize, len: usize) -> bool {
        if left_start + len > self.len() || right_start + len > self.len() {
            return false;
        }
        for offset in 0..len {
            if self.code_unit(left_start + offset) != self.code_unit(right_start + offset) {
                return false;
            }
        }
        true
    }
}

impl Input for &[u16] {
    #[inline(always)]
    fn len(self) -> usize {
        <[u16]>::len(self)
    }

    #[inline(always)]
    fn code_unit(self, pos: usize) -> u16 {
        self[pos]
    }

    #[inline(always)]
    fn find_code_unit(self, start: usize, end: usize, ch16: u16) -> Option<usize> {
        let offset = self.get(start..end)?.iter().position(|&c| c == ch16)?;
        Some(start + offset)
    }

    #[inline(always)]
    fn matches_u16_at(self, pos: usize, needle: &[u16]) -> bool {
        self.get(pos..pos + needle.len()) == Some(needle)
    }

    #[inline(always)]
    fn ends_with_u16(self, suffix: &[u16]) -> bool {
        self.ends_with(suffix)
    }

    #[inline(always)]
    fn ranges_equal(self, left_start: usize, right_start: usize, len: usize) -> bool {
        self.get(left_start..left_start + len) == self.get(right_start..right_start + len)
    }
}

impl Input for &[u8] {
    #[inline(always)]
    fn len(self) -> usize {
        <[u8]>::len(self)
    }

    #[inline(always)]
    fn code_unit(self, pos: usize) -> u16 {
        self[pos] as u16
    }

    #[inline(always)]
    fn find_code_unit(self, start: usize, end: usize, ch16: u16) -> Option<usize> {
        let byte = u8::try_from(ch16).ok()?;
        if byte > 0x7F {
            return None;
        }
        let offset = self.get(start..end)?.iter().position(|&c| c == byte)?;
        Some(start + offset)
    }

    #[inline(always)]
    fn matches_u16_at(self, pos: usize, needle: &[u16]) -> bool {
        let Some(slice) = self.get(pos..pos + needle.len()) else {
            return false;
        };
        for (actual, expected) in slice.iter().zip(needle.iter()) {
            if *expected > 0x7F || *actual != *expected as u8 {
                return false;
            }
        }
        true
    }

    #[inline(always)]
    fn ends_with_u16(self, suffix: &[u16]) -> bool {
        if suffix.len() > self.len() {
            return false;
        }
        self.matches_u16_at(self.len() - suffix.len(), suffix)
    }
}

/// Execute the program, writing captures directly into `out` buffer.
/// Returns true on match. `out` should have `(capture_count + 1) * 2` i32 slots.
/// Copy captures from VM registers to the output buffer.
#[inline(always)]
fn copy_captures_to_out(registers: &[i32], capture_count: u32, out: &mut [i32]) {
    let slots = (capture_count as usize + 1) * 2;
    let copy_len = slots.min(registers.len()).min(out.len());
    out[..copy_len].copy_from_slice(&registers[..copy_len]);
}

#[inline(always)]
fn next_search_position(match_start: i32, match_end: i32) -> usize {
    if match_end == match_start {
        match_end as usize + 1
    } else {
        match_end as usize
    }
}

use crate::bytecode::append_code_point_wtf16;

fn extract_trailing_literal(
    instructions: &[Instruction],
    assert_end_pc: usize,
) -> Option<Vec<u16>> {
    let mut trailing_code_points = Vec::new();
    let mut suffix_start = assert_end_pc;
    let mut pc = assert_end_pc;

    while pc > 0 {
        let prev_pc = pc - 1;
        match &instructions[prev_pc] {
            Instruction::Char(cp) => {
                trailing_code_points.push(*cp);
                suffix_start = prev_pc;
                pc = prev_pc;
            }
            // Skip zero-width bookkeeping so suffixes after capture boundaries still count.
            Instruction::Save(_)
            | Instruction::ClearRegister(_)
            | Instruction::PopModifiers
            | Instruction::Nop => {
                suffix_start = prev_pc;
                pc = prev_pc;
            }
            _ => break,
        }
    }

    if trailing_code_points.is_empty() {
        return None;
    }
    if !suffix_has_linear_tail(instructions, suffix_start) {
        return None;
    }

    trailing_code_points.reverse();
    let mut suffix = Vec::new();
    for cp in trailing_code_points {
        append_code_point_wtf16(&mut suffix, cp)?;
    }
    Some(suffix)
}

fn suffix_has_linear_tail(instructions: &[Instruction], suffix_start: usize) -> bool {
    if suffix_start + 1 >= instructions.len() {
        return false;
    }

    let mut predecessor_count = vec![0usize; instructions.len()];
    let mut predecessor = vec![usize::MAX; instructions.len()];
    for (pc, instruction) in instructions.iter().enumerate() {
        for_each_successor(instruction, pc, instructions.len(), |successor| {
            predecessor_count[successor] += 1;
            if predecessor[successor] == usize::MAX {
                predecessor[successor] = pc;
            }
        });
    }

    for target in suffix_start + 1..instructions.len() {
        if predecessor_count[target] != 1 || predecessor[target] != target - 1 {
            return false;
        }
    }

    true
}

fn for_each_successor(
    instruction: &Instruction,
    pc: usize,
    instruction_count: usize,
    mut callback: impl FnMut(usize),
) {
    match instruction {
        Instruction::Jump(target) => callback(*target as usize),
        Instruction::Split { prefer, other } => {
            callback(*prefer as usize);
            callback(*other as usize);
        }
        Instruction::RepeatCheck { body, .. } => {
            callback(*body as usize);
            if pc + 1 < instruction_count {
                callback(pc + 1);
            }
        }
        // Conservatively model lookarounds as flowing both into the body and
        // to the continuation after the assertion.
        Instruction::LookStart { end, .. } => {
            callback(*end as usize);
            if pc + 1 < instruction_count {
                callback(pc + 1);
            }
        }
        Instruction::Match | Instruction::Fail | Instruction::LookEnd => {}
        _ => {
            if pc + 1 < instruction_count {
                callback(pc + 1);
            }
        }
    }
}

#[inline(always)]
fn fails_trailing_literal_hint<I: Input>(input: I, hints: &PatternHints) -> bool {
    let Some(suffix) = hints.trailing_literal.as_deref() else {
        return false;
    };
    !input.ends_with_u16(suffix)
}

#[inline(always)]
fn contains_u16_from<I: Input>(input: I, start: usize, needle: &[u16]) -> bool {
    if needle.is_empty() {
        return true;
    }

    if needle.len() == 1 {
        return input
            .find_code_unit(start, input.len(), needle[0])
            .is_some();
    }

    if start + needle.len() > input.len() {
        return false;
    }

    let mut pos = start;
    let end = input.len() - needle.len() + 1;
    while pos < end {
        match input.find_code_unit(pos, end, needle[0]) {
            Some(candidate_pos) => pos = candidate_pos,
            None => return false,
        }
        if input.matches_u16_at(pos, needle) {
            return true;
        }
        pos += 1;
    }
    false
}

#[inline(always)]
fn fails_required_literal_hint<I: Input>(input: I, start: usize, hints: &PatternHints) -> bool {
    let Some(literal) = hints.required_literal.as_deref() else {
        return false;
    };
    !contains_u16_from(input, start, literal)
}

#[inline(always)]
fn first_char_matches<I: Input>(
    program: &Program,
    input: I,
    pos: usize,
    ch: u32,
    case_insensitive: bool,
) -> bool {
    if pos >= input.len() {
        return false;
    }

    let input_cp = if program.unicode
        && is_high_surrogate(input.code_unit(pos))
        && pos + 1 < input.len()
        && is_low_surrogate(input.code_unit(pos + 1))
    {
        0x10000
            + ((input.code_unit(pos) as u32 - 0xD800) << 10)
            + (input.code_unit(pos + 1) as u32 - 0xDC00)
    } else {
        input.code_unit(pos) as u32
    };

    if case_insensitive {
        case_fold_eq(input_cp, ch, program.unicode)
    } else {
        input_cp == ch
    }
}

#[inline(always)]
fn first_filter_matches<I: Input>(
    program: &Program,
    input: I,
    pos: usize,
    filter: &SimpleMatch,
) -> bool {
    let cp = decode_code_point(program.unicode, input, pos);
    match filter {
        SimpleMatch::BuiltinClass(class) => {
            match_builtin_class(cp, *class, program.ignore_case && program.unicode)
        }
        SimpleMatch::CharClass { ranges, negated } => char_in_ranges(cp, ranges) != *negated,
        SimpleMatch::AnyChar { dot_all } => *dot_all || !is_line_terminator(cp),
        SimpleMatch::Char(c) => cp == *c,
        SimpleMatch::CharNoCase(lo, _hi) => case_fold_eq(cp, *lo, program.unicode),
        SimpleMatch::UnicodeProperty(data) => {
            let matched = match_unicode_property_resolved(
                cp,
                &data.name,
                data.value.as_deref(),
                data.resolved.as_ref(),
            );
            matched != data.negated
        }
    }
}

#[inline(always)]
fn next_candidate_start<I: Input>(
    program: &Program,
    input: I,
    hints: &PatternHints,
    mut pos: usize,
) -> Option<usize> {
    while pos <= input.len() {
        if program.unicode
            && !hints.can_match_empty
            && pos > 0
            && pos < input.len()
            && is_low_surrogate(input.code_unit(pos))
            && is_high_surrogate(input.code_unit(pos - 1))
        {
            pos += 1;
            continue;
        }

        if hints.starts_with_anchor
            && pos != 0
            && !is_line_terminator(input.code_unit(pos - 1) as u32)
        {
            pos += 1;
            continue;
        }

        if let Some((ch, case_insensitive)) = hints.first_char
            && !first_char_matches(program, input, pos, ch, case_insensitive)
        {
            if pos >= input.len() {
                return None;
            }
            pos += 1;
            continue;
        }

        if let Some(ref filter) = hints.first_filter {
            if pos >= input.len() {
                return None;
            }
            if !first_filter_matches(program, input, pos, filter) {
                pos += 1;
                continue;
            }
        }

        return Some(pos);
    }

    None
}

/// Execute the program, reusing a provided VmScratch to avoid per-call allocation.
pub fn execute_into_with_scratch<I: Input>(
    program: &Program,
    input: I,
    start_pos: usize,
    hints: &PatternHints,
    out: &mut [i32],
    scratch: &mut VmScratch,
) -> VmResult {
    execute_into_impl(program, input, start_pos, hints, out, scratch)
}

/// Find all non-overlapping matches, writing (start, end) i32 pairs into result_buf.
/// Returns number of matches found, or -1 if buffer is too small, or -2 if step limit exceeded.
/// Keeps the VM alive across matches to avoid per-match setup overhead.
pub fn find_all_with_scratch<I: Input>(
    program: &Program,
    input: I,
    start_pos: usize,
    hints: &PatternHints,
    result_buf: &mut [i32],
    scratch: &mut VmScratch,
) -> i32 {
    if fails_trailing_literal_hint(input, hints) {
        return 0;
    }
    if fails_required_literal_hint(input, start_pos, hints) {
        return 0;
    }

    // Fast path for simple single-matcher patterns: use specialized scan.
    if let Some(ref scan) = hints.simple_scan {
        return find_all_simple_scan(program, input, start_pos, scan, result_buf);
    }

    let mut vm = Vm::new(program, input, start_pos, scratch);
    let capacity = result_buf.len();
    let mut count = 0i32;
    let mut pos = start_pos;

    // Fast path: non-unicode pattern starting with a literal character.
    if let Some((ch, false)) = hints.first_char
        && !program.unicode
        && ch <= 0xFFFF
    {
        let ch16 = ch as u16;
        while let Some(candidate_pos) = input.next_literal_start(pos, ch16) {
            vm.reset(candidate_pos);
            match vm.run() {
                VmResult::Match => {
                    let match_start = vm.registers[0];
                    let match_end = vm.registers[1];
                    let idx = count as usize * 2;
                    if idx + 1 >= capacity {
                        return -1;
                    }
                    result_buf[idx] = match_start;
                    result_buf[idx + 1] = match_end;
                    count += 1;
                    pos = next_search_position(match_start, match_end);
                }
                VmResult::LimitExceeded => return -2,
                VmResult::NoMatch => {
                    pos = candidate_pos + 1;
                }
            }
        }
        return count;
    }

    // Anchored non-multiline: only try pos 0.
    if hints.starts_with_anchor && !hints.anchor_multiline {
        vm.reset(start_pos);
        match vm.run() {
            VmResult::Match => {
                if capacity >= 2 {
                    result_buf[0] = vm.registers[0];
                    result_buf[1] = vm.registers[1];
                    return 1;
                }
                return -1;
            }
            VmResult::LimitExceeded => return -2,
            VmResult::NoMatch => return 0,
        }
    }

    // General loop.
    while let Some(candidate_pos) = next_candidate_start(program, input, hints, pos) {
        vm.reset(candidate_pos);
        match vm.run() {
            VmResult::Match => {
                let match_start = vm.registers[0];
                let match_end = vm.registers[1];
                let idx = count as usize * 2;
                if idx + 1 >= capacity {
                    return -1;
                }
                result_buf[idx] = match_start;
                result_buf[idx + 1] = match_end;
                count += 1;
                pos = next_search_position(match_start, match_end);
            }
            VmResult::LimitExceeded => return -2,
            VmResult::NoMatch => {
                pos = candidate_pos + 1;
            }
        }
    }
    count
}

/// Find all matches for simple scan patterns (no VM needed).
fn find_all_simple_scan<I: Input>(
    program: &Program,
    input: I,
    start_pos: usize,
    scan: &SimpleScan,
    result_buf: &mut [i32],
) -> i32 {
    let capacity = result_buf.len();
    let mut count = 0i32;
    let mut pos = start_pos;
    loop {
        if pos > input.len() {
            break;
        }
        let mut out = [-1i32; 2];
        if !execute_simple_scan_into(program, input, pos, scan, &mut out) {
            break;
        }
        let match_start = out[0];
        let match_end = out[1];
        let idx = count as usize * 2;
        if idx + 1 >= capacity {
            return -1;
        }
        result_buf[idx] = match_start;
        result_buf[idx + 1] = match_end;
        count += 1;
        if match_end == match_start {
            pos = match_end as usize + 1;
        } else {
            pos = match_end as usize;
        }
    }
    count
}

fn execute_into_impl<I: Input>(
    program: &Program,
    input: I,
    start_pos: usize,
    hints: &PatternHints,
    out: &mut [i32],
    scratch: &mut VmScratch,
) -> VmResult {
    if fails_trailing_literal_hint(input, hints) {
        return VmResult::NoMatch;
    }
    if fails_required_literal_hint(input, start_pos, hints) {
        return VmResult::NoMatch;
    }

    // Fast path for simple single-matcher patterns.
    // NB: Simple scans never hit the step limit (no backtracking).
    if let Some(ref scan) = hints.simple_scan {
        return if execute_simple_scan_into(program, input, start_pos, scan, out) {
            VmResult::Match
        } else {
            VmResult::NoMatch
        };
    }

    // Reuse a single VM across all starting positions to avoid repeated allocation.
    let mut vm = Vm::new(program, input, start_pos, scratch);
    let mut hit_limit = false;

    // Fast path: non-unicode pattern starting with a literal character.
    // Use iter().position() for bulk scanning (LLVM can auto-vectorize this).
    if let Some((ch, false)) = hints.first_char
        && !program.unicode
        && ch <= 0xFFFF
    {
        let ch16 = ch as u16;
        let mut pos = start_pos;
        while let Some(candidate_pos) = input.next_literal_start(pos, ch16) {
            vm.reset(candidate_pos);
            match vm.run() {
                VmResult::Match => {
                    copy_captures_to_out(vm.registers, program.capture_count, out);
                    return VmResult::Match;
                }
                VmResult::LimitExceeded => hit_limit = true,
                VmResult::NoMatch => {}
            }
            pos = candidate_pos + 1;
        }
        return if hit_limit {
            VmResult::LimitExceeded
        } else {
            VmResult::NoMatch
        };
    }

    // Anchored non-multiline: only try pos 0.
    if hints.starts_with_anchor && !hints.anchor_multiline {
        vm.reset(start_pos);
        let result = vm.run();
        if result == VmResult::Match {
            copy_captures_to_out(vm.registers, program.capture_count, out);
        }
        return result;
    }

    // General start-position loop.
    let mut pos = start_pos;
    while let Some(candidate_pos) = next_candidate_start(program, input, hints, pos) {
        // Reset VM state for this starting position.
        vm.reset(candidate_pos);
        match vm.run() {
            VmResult::Match => {
                copy_captures_to_out(vm.registers, program.capture_count, out);
                return VmResult::Match;
            }
            VmResult::LimitExceeded => hit_limit = true,
            VmResult::NoMatch => {}
        }
        pos = candidate_pos + 1;
    }

    if hit_limit {
        VmResult::LimitExceeded
    } else {
        VmResult::NoMatch
    }
}

/// Fast path for simple scans that writes directly into caller's buffer.
fn execute_simple_scan_into<I: Input>(
    program: &Program,
    input: I,
    start_pos: usize,
    scan: &SimpleScan,
    out: &mut [i32],
) -> bool {
    match scan {
        SimpleScan::GreedyQuantifier { matcher, min, max } => {
            execute_simple_greedy_scan_into(program, input, start_pos, matcher, *min, *max, out)
        }
        _ => {
            let mut pos = start_pos;
            while pos < input.len() {
                let cp = decode_code_point(program.unicode, input, pos);
                let matched = match_simple_scan(scan, cp);
                if matched {
                    let end = code_point_len(program.unicode, input, pos) + pos;
                    out[0] = pos as i32;
                    out[1] = end as i32;
                    return true;
                }
                pos += code_point_len(program.unicode, input, pos);
            }
            false
        }
    }
}

/// Match a single code point against a SimpleScan pattern.
#[inline(always)]
fn match_simple_scan(scan: &SimpleScan, cp: u32) -> bool {
    match scan {
        SimpleScan::CharClass { ranges, negated } => char_in_ranges(cp, ranges) != *negated,
        SimpleScan::BuiltinClass(class) => match_builtin_class(cp, *class, false),
        SimpleScan::Char(c) => cp == *c,
        SimpleScan::GreedyQuantifier { .. } => false, // handled separately
    }
}

/// Match a single code point against a SimpleMatch.
#[inline(always)]
fn match_simple_match(matcher: &SimpleMatch, cp: u32) -> bool {
    match matcher {
        SimpleMatch::AnyChar { dot_all } => *dot_all || !is_line_terminator(cp),
        SimpleMatch::Char(c) => cp == *c,
        SimpleMatch::CharNoCase(c1, c2) => cp == *c1 || cp == *c2,
        SimpleMatch::CharClass { ranges, negated } => char_in_ranges(cp, ranges) != *negated,
        SimpleMatch::BuiltinClass(class) => match_builtin_class(cp, *class, false),
        SimpleMatch::UnicodeProperty(data) => {
            let matched = match_unicode_property_resolved(
                cp,
                &data.name,
                data.value.as_deref(),
                data.resolved.as_ref(),
            );
            matched != data.negated
        }
    }
}

/// Decode a code point at a position, handling surrogate pairs in unicode mode.
#[inline(always)]
pub(crate) fn decode_code_point<I: Input>(unicode: bool, input: I, pos: usize) -> u32 {
    if unicode
        && is_high_surrogate(input.code_unit(pos))
        && pos + 1 < input.len()
        && is_low_surrogate(input.code_unit(pos + 1))
    {
        0x10000
            + ((input.code_unit(pos) as u32 - 0xD800) << 10)
            + (input.code_unit(pos + 1) as u32 - 0xDC00)
    } else {
        input.code_unit(pos) as u32
    }
}

/// Get the length (in code units) of a code point at a position.
#[inline(always)]
pub(crate) fn code_point_len<I: Input>(unicode: bool, input: I, pos: usize) -> usize {
    if unicode
        && is_high_surrogate(input.code_unit(pos))
        && pos + 1 < input.len()
        && is_low_surrogate(input.code_unit(pos + 1))
    {
        2
    } else {
        1
    }
}

/// Fast path for greedy quantifier patterns like \s+, \d+.
/// Scans for the first position where the matcher matches, then greedily consumes.
fn execute_simple_greedy_scan_into<I: Input>(
    program: &Program,
    input: I,
    start_pos: usize,
    matcher: &SimpleMatch,
    min: u32,
    max: Option<u32>,
    out: &mut [i32],
) -> bool {
    let mut pos = start_pos;
    while pos < input.len() {
        let cp = decode_code_point(program.unicode, input, pos);
        if match_simple_match(matcher, cp) {
            // Found a match start. Greedily consume as many as possible.
            let match_start = pos;
            let mut count = 0u32;
            let mut end = pos;
            while end < input.len() {
                if let Some(m) = max
                    && count >= m
                {
                    break;
                }
                let cp = decode_code_point(program.unicode, input, end);
                if !match_simple_match(matcher, cp) {
                    break;
                }
                end += code_point_len(program.unicode, input, end);
                count += 1;
            }
            if count >= min {
                out[0] = match_start as i32;
                out[1] = end as i32;
                return true;
            }
            // Not enough matches, advance past this position.
            pos = end;
        } else {
            pos += code_point_len(program.unicode, input, pos);
        }
    }
    false
}

/// Hints about the pattern structure for optimizing the starting position loop.
pub struct PatternHints {
    /// First literal character to filter on (char, is_case_insensitive).
    first_char: Option<(u32, bool)>,
    /// First instruction filter: skip positions where the first matcher can't match.
    first_filter: Option<SimpleMatch>,
    /// Pattern starts with ^ (AssertStart) — only try at line starts (or input start).
    starts_with_anchor: bool,
    /// Whether the anchor is multiline (^ matches at line starts, not just input start).
    anchor_multiline: bool,
    /// Literal suffix immediately before a non-multiline end anchor.
    trailing_literal: Option<Vec<u16>>,
    /// Literal tail in the linear success path that every match must contain.
    required_literal: Option<Vec<u16>>,
    /// Simple pattern: just a single matcher (Save(0) + matcher + Save(1) + Match).
    /// Can be executed with a fast scan without the full VM.
    simple_scan: Option<SimpleScan>,
    /// Whether the full pattern can match without consuming input.
    can_match_empty: bool,
}

/// A simple pattern that can be scanned without the full VM.
enum SimpleScan {
    /// A single character class.
    CharClass {
        ranges: Vec<CharRange>,
        negated: bool,
    },
    /// A single builtin class.
    BuiltinClass(BuiltinCharacterClass),
    /// A single character.
    Char(u32),
    /// A greedy quantifier of a simple matcher (e.g., \s+, \d+, .+).
    GreedyQuantifier {
        matcher: SimpleMatch,
        min: u32,
        max: Option<u32>,
    },
}

/// Find a common first character across all alternatives in a Split chain.
/// Returns Some((char, case_insensitive)) if all branches start with the same character.
fn find_common_first_char(instructions: &[Instruction], start: u32) -> Option<(u32, bool)> {
    let mut common: Option<(u32, bool)> = None;
    let mut pc = start as usize;
    loop {
        if pc >= instructions.len() {
            return None;
        }
        match &instructions[pc] {
            Instruction::Split { prefer, other } => {
                // Check the preferred branch's first char instruction.
                let prefer_char = first_char_at(instructions, *prefer as usize)?;
                match common {
                    None => common = Some(prefer_char),
                    Some(c) if c.0 == prefer_char.0 => {}
                    _ => return None,
                }
                // Follow the other branch (which may be another Split or a final alternative).
                pc = *other as usize;
            }
            // The last alternative in the chain (no more Splits).
            _ => {
                let last_char = first_char_at(instructions, pc)?;
                match common {
                    None => return Some(last_char),
                    Some(c) if c.0 == last_char.0 => return common,
                    _ => return None,
                }
            }
        }
    }
}

/// Get the first character-matching instruction at a given position.
fn first_char_at(instructions: &[Instruction], pc: usize) -> Option<(u32, bool)> {
    match instructions.get(pc)? {
        Instruction::Char(c) => Some((*c, false)),
        Instruction::CharNoCase(c, _) => Some((*c, true)),
        // Skip Save instructions (capture group starts) to find the actual char.
        Instruction::Save(_) => first_char_at(instructions, pc + 1),
        _ => None,
    }
}

/// Analyze the program to extract optimization hints.
pub fn analyze_pattern(program: &Program, can_match_empty: bool) -> PatternHints {
    // Pattern typically starts with Save(0), then the first real instruction.
    let skip = if matches!(program.instructions.first(), Some(Instruction::Save(0))) {
        1
    } else {
        0
    };
    let first_inst = program.instructions.get(skip);

    // Determine the first "character-matching" instruction for start-position filtering.
    // If the first instruction is an assertion (\b, \B), look past it to the next instruction.
    let (filter_inst, filter_offset) = match first_inst {
        Some(Instruction::AssertWordBoundary | Instruction::AssertNonWordBoundary) => {
            (program.instructions.get(skip + 1), skip + 1)
        }
        _ => (first_inst, skip),
    };

    let first_char = match filter_inst {
        Some(Instruction::Char(c)) => Some((*c, false)),
        Some(Instruction::CharNoCase(c, _)) => Some((*c, true)),
        // For disjunctions (Split chains), find the common first character
        // across all alternatives. E.g., /<script|<style|<link/ all start with '<'.
        Some(Instruction::Split { .. }) => {
            find_common_first_char(&program.instructions, filter_offset as u32)
        }
        _ => None,
    };

    // Extract a filter for the first matching instruction.
    // This allows skipping positions in the start-position loop.
    // Don't use filters for case-insensitive patterns (case folding complicates filtering).
    let first_filter = if first_char.is_none() && !program.ignore_case {
        match filter_inst {
            Some(Instruction::BuiltinClass(class)) => Some(SimpleMatch::BuiltinClass(*class)),
            Some(Instruction::CharClass { ranges, negated }) => Some(SimpleMatch::CharClass {
                ranges: ranges.clone(),
                negated: *negated,
            }),
            Some(Instruction::GreedyLoop { matcher, min, .. }) if *min >= 1 => {
                Some(matcher.clone())
            }
            _ => None,
        }
    } else {
        None
    };

    let (starts_with_anchor, anchor_multiline) = match first_inst {
        Some(Instruction::AssertStart { multiline }) => (true, *multiline || program.multiline),
        _ => (false, false),
    };

    let trailing_literal = match program.instructions.as_slice() {
        [
            ..,
            Instruction::AssertEnd { multiline },
            Instruction::Save(1),
            Instruction::Match,
        ] if !(program.ignore_case || *multiline || program.multiline) => {
            extract_trailing_literal(&program.instructions, program.instructions.len() - 3)
        }
        _ => None,
    };

    let required_literal = match program.instructions.as_slice() {
        [.., Instruction::Save(1), Instruction::Match] if !program.ignore_case => {
            extract_trailing_literal(&program.instructions, program.instructions.len() - 2)
        }
        [
            ..,
            Instruction::AssertEnd { .. },
            Instruction::Save(1),
            Instruction::Match,
        ] if !program.ignore_case => {
            extract_trailing_literal(&program.instructions, program.instructions.len() - 3)
        }
        _ => None,
    };

    // Detect simple patterns: Save(0) + single_matcher + Save(1) + Match
    let simple_scan = if !program.ignore_case
        && program.instructions.len() == 4
        && matches!(program.instructions.first(), Some(Instruction::Save(0)))
        && matches!(program.instructions.get(2), Some(Instruction::Save(1)))
        && matches!(program.instructions.get(3), Some(Instruction::Match))
    {
        match &program.instructions[1] {
            Instruction::CharClass { ranges, negated } => Some(SimpleScan::CharClass {
                ranges: ranges.clone(),
                negated: *negated,
            }),
            Instruction::BuiltinClass(class) => Some(SimpleScan::BuiltinClass(*class)),
            Instruction::Char(c) => Some(SimpleScan::Char(*c)),
            // GreedyLoop of a simple matcher: e.g., \s+, \d+, .+
            // Only when min >= 1 (min=0 means every position matches, complicating the scan).
            Instruction::GreedyLoop { matcher, min, max } if *min >= 1 => {
                Some(SimpleScan::GreedyQuantifier {
                    matcher: matcher.clone(),
                    min: *min,
                    max: *max,
                })
            }
            _ => None,
        }
    } else {
        None
    };

    PatternHints {
        first_char,
        first_filter,
        starts_with_anchor,
        anchor_multiline,
        trailing_literal,
        required_literal,
        simple_scan,
        can_match_empty,
    }
}

/// Saved state for backtracking.
#[derive(Clone)]
enum SavedState {
    /// Normal backtrack state with full register snapshot.
    Normal {
        pc: u32,
        pos: usize,
        reg_snapshot_offset: usize,
        modifiers: ActiveModifiers,
        modifier_stack_len: usize,
    },
    /// Greedy loop backtrack: give up one character at a time from the right.
    /// Carries a register snapshot because later failed alternatives may have
    /// mutated captures before we revisit the loop choice point.
    Greedy {
        pc: u32,
        /// Starting position of the greedy consumption.
        start_pos: usize,
        /// Current right edge (position after last consumed char).
        current_pos: usize,
        reg_snapshot_offset: usize,
        /// Minimum number of chars that must be kept.
        min: u32,
        /// Whether this greedy loop was executing in backward mode (lookbehind).
        backward: bool,
        modifiers: ActiveModifiers,
        modifier_stack_len: usize,
    },
    /// Lazy loop backtrack: try consuming one more character.
    /// Carries a register snapshot because later failed alternatives may have
    /// mutated captures before we revisit the loop choice point.
    Lazy {
        pc: u32,
        /// Position to try consuming from next.
        pos: usize,
        reg_snapshot_offset: usize,
        /// Maximum total matches allowed.
        max: Option<u32>,
        /// Number of matches consumed so far.
        consumed: u32,
        modifiers: ActiveModifiers,
        modifier_stack_len: usize,
    },
}

/// Active modifier flags during execution.
#[derive(Clone, Copy)]
struct ActiveModifiers {
    ignore_case: bool,
    multiline: bool,
    dot_all: bool,
}

/// Reusable scratch space for the VM. Cached in the Regex struct to avoid
/// per-call allocation of registers, backtrack stack, and register pool.
#[derive(Default)]
pub struct VmScratch {
    registers: Vec<i32>,
    backtrack_stack: Vec<SavedState>,
    register_pool: Vec<i32>,
    modifier_stack: Vec<ActiveModifiers>,
}

impl VmScratch {
    pub fn new() -> Self {
        Self::default()
    }
}

struct Vm<'a, I: Input> {
    program: &'a Program,
    input: I,
    pc: u32,
    pos: usize,
    registers: &'a mut Vec<i32>,
    backtrack_stack: &'a mut Vec<SavedState>,
    /// Flat pool for register snapshots. Each saved state stores an offset into
    /// this pool. Avoids per-Split Vec allocation.
    register_pool: &'a mut Vec<i32>,
    steps: u64,
    modifiers: ActiveModifiers,
    modifier_stack: &'a mut Vec<ActiveModifiers>,
    /// True when executing a lookbehind body (characters consumed right-to-left).
    backward: bool,
    /// Backtrack floor: prevents backtracking past this depth during lookaround bodies.
    bt_floor: usize,
}

impl<'a, I: Input> Vm<'a, I> {
    fn new(program: &'a Program, input: I, start_pos: usize, scratch: &'a mut VmScratch) -> Self {
        let reg_count = program.register_count as usize;
        // Ensure registers are the right size.
        scratch.registers.resize(reg_count, -1);
        scratch.registers.fill(-1);
        scratch.backtrack_stack.clear();
        scratch.register_pool.clear();
        scratch.modifier_stack.clear();
        Self {
            program,
            input,
            pc: 0,
            pos: start_pos,
            registers: &mut scratch.registers,
            backtrack_stack: &mut scratch.backtrack_stack,
            register_pool: &mut scratch.register_pool,
            steps: 0,
            modifiers: ActiveModifiers {
                ignore_case: program.ignore_case,
                multiline: program.multiline,
                dot_all: program.dot_all,
            },
            modifier_stack: &mut scratch.modifier_stack,
            backward: false,
            bt_floor: 0,
        }
    }

    #[inline(always)]
    fn input_len(&self) -> usize {
        self.input.len()
    }

    #[inline(always)]
    fn input_code_unit(&self, pos: usize) -> u16 {
        self.input.code_unit(pos)
    }

    /// Reset VM state for a new match attempt at the given position.
    /// Reuses existing allocations.
    #[inline(always)]
    fn reset(&mut self, pos: usize) {
        self.pc = 0;
        self.pos = pos;
        self.registers.fill(-1);
        self.backtrack_stack.clear();
        self.register_pool.clear();
        self.steps = 0;
        self.modifiers = ActiveModifiers {
            ignore_case: self.program.ignore_case,
            multiline: self.program.multiline,
            dot_all: self.program.dot_all,
        };
        self.modifier_stack.clear();
        self.backward = false;
        self.bt_floor = 0;
    }

    fn run(&mut self) -> VmResult {
        // Use the fast path for non-Unicode, forward-only patterns.
        if !self.program.unicode && !self.backward {
            return self.run_fast();
        }
        self.run_general()
    }

    /// Fast VM loop specialized for non-Unicode, forward-only patterns.
    /// Eliminates surrogate pair checks and backward mode branches.
    fn run_fast(&mut self) -> VmResult {
        let instructions = &self.program.instructions;
        let num_instructions = instructions.len();
        let input = self.input;
        let input_len = input.len();
        'vm_loop: loop {
            self.steps += 1;
            if self.steps >= MATCH_LIMIT {
                return VmResult::LimitExceeded;
            }

            let pc = self.pc as usize;
            if pc >= num_instructions {
                if !self.backtrack() {
                    return VmResult::NoMatch;
                }
                continue;
            }

            let inst = &instructions[pc];
            match inst {
                Instruction::Char(c) => {
                    let c = *c;
                    if self.pos < input_len {
                        let input_cp = input.code_unit(self.pos) as u32;
                        let matches = if self.modifiers.ignore_case {
                            case_fold_eq(input_cp, c, false)
                        } else {
                            input_cp == c
                        };
                        if matches {
                            self.pos += 1;
                            self.pc += 1;
                            continue;
                        }
                    }
                    if !self.backtrack() {
                        return VmResult::NoMatch;
                    }
                }

                Instruction::CharNoCase(lo, _hi) => {
                    if self.pos < input_len {
                        let input_cp = input.code_unit(self.pos) as u32;
                        if case_fold_eq(input_cp, *lo, false) {
                            self.pos += 1;
                            self.pc += 1;
                            continue;
                        }
                    }
                    if !self.backtrack() {
                        return VmResult::NoMatch;
                    }
                }

                Instruction::AnyChar { dot_all } => {
                    if self.pos < input_len {
                        let dot_all = *dot_all || self.modifiers.dot_all;
                        if dot_all || !is_line_terminator(input.code_unit(self.pos) as u32) {
                            self.pos += 1;
                            self.pc += 1;
                            continue;
                        }
                    }
                    if !self.backtrack() {
                        return VmResult::NoMatch;
                    }
                }

                Instruction::CharClass { ranges, negated } => {
                    if self.pos < input_len {
                        let cp = input.code_unit(self.pos) as u32;
                        let in_class = if self.modifiers.ignore_case {
                            match_char_class(cp, ranges, true, false, false)
                        } else {
                            char_in_ranges(cp, ranges)
                        };
                        if in_class != *negated {
                            self.pos += 1;
                            self.pc += 1;
                            continue;
                        }
                    }
                    if !self.backtrack() {
                        return VmResult::NoMatch;
                    }
                }

                Instruction::BuiltinClass(class) => {
                    let class = *class;
                    if self.pos < input_len {
                        let cp = input.code_unit(self.pos) as u32;
                        if match_builtin_class(cp, class, false) {
                            self.pos += 1;
                            self.pc += 1;
                            continue;
                        }
                    }
                    if !self.backtrack() {
                        return VmResult::NoMatch;
                    }
                }

                Instruction::UnicodeProperty(data) => {
                    if self.pos < input_len {
                        let cp = input.code_unit(self.pos) as u32;
                        // NB: run_fast is only entered when !unicode, so the
                        // case-insensitive Unicode path never applies here.
                        let result = {
                            let matched = match_unicode_property_resolved(
                                cp,
                                &data.name,
                                data.value.as_deref(),
                                data.resolved.as_ref(),
                            );
                            if data.negated { !matched } else { matched }
                        };
                        if result {
                            self.pos += 1;
                            self.pc += 1;
                            continue;
                        }
                    }
                    if !self.backtrack() {
                        return VmResult::NoMatch;
                    }
                }

                Instruction::Jump(target) => {
                    self.pc = *target;
                }

                Instruction::Split { prefer, other } => {
                    let prefer = *prefer;
                    let other = *other;
                    let pos = self.pos;
                    self.push_backtrack(other, pos);
                    self.pc = prefer;
                }

                Instruction::Save(reg) => {
                    let reg = *reg as usize;
                    let pos_i32 = self.pos as i32;
                    if reg < self.registers.len() {
                        self.registers[reg] = pos_i32;
                    }
                    self.pc += 1;
                    // Fuse consecutive Save/ClearRegister instructions.
                    loop {
                        match instructions.get(self.pc as usize) {
                            Some(Instruction::Save(r2)) => {
                                let r2 = *r2 as usize;
                                if r2 < self.registers.len() {
                                    self.registers[r2] = pos_i32;
                                }
                                self.pc += 1;
                            }
                            Some(Instruction::ClearRegister(r2)) => {
                                let r2 = *r2 as usize;
                                if r2 < self.registers.len() {
                                    self.registers[r2] = -1;
                                }
                                self.pc += 1;
                            }
                            _ => break,
                        }
                    }
                }

                Instruction::ClearRegister(reg) => {
                    let reg = *reg as usize;
                    if reg < self.registers.len() {
                        self.registers[reg] = -1;
                    }
                    self.pc += 1;
                    // Fuse consecutive ClearRegister/Save instructions.
                    loop {
                        match instructions.get(self.pc as usize) {
                            Some(Instruction::ClearRegister(r2)) => {
                                let r2 = *r2 as usize;
                                if r2 < self.registers.len() {
                                    self.registers[r2] = -1;
                                }
                                self.pc += 1;
                            }
                            Some(Instruction::Save(r2)) => {
                                let r2 = *r2 as usize;
                                if r2 < self.registers.len() {
                                    self.registers[r2] = self.pos as i32;
                                }
                                self.pc += 1;
                            }
                            _ => break,
                        }
                    }
                }

                Instruction::AssertStart { multiline } => {
                    if let Some(result) = self.handle_assert_start(*multiline) {
                        return result;
                    }
                }

                Instruction::AssertEnd { multiline } => {
                    if let Some(result) = self.handle_assert_end(*multiline, input_len) {
                        return result;
                    }
                }

                Instruction::AssertWordBoundary => {
                    if let Some(result) = self.handle_word_boundary_assert(true) {
                        return result;
                    }
                }

                Instruction::AssertNonWordBoundary => {
                    if let Some(result) = self.handle_word_boundary_assert(false) {
                        return result;
                    }
                }

                Instruction::Match => {
                    return VmResult::Match;
                }

                Instruction::Fail => {
                    if !self.backtrack() {
                        return VmResult::NoMatch;
                    }
                }

                Instruction::Backref(index) => {
                    if let Some(result) = self.handle_backref(*index) {
                        return result;
                    }
                }

                Instruction::BackrefNamed(name) => {
                    if let Some(result) = self.handle_backref_named(name) {
                        return result;
                    }
                }

                Instruction::RepeatStart { counter_reg } => {
                    self.handle_repeat_start(*counter_reg);
                }

                Instruction::RepeatCheck {
                    counter_reg,
                    min,
                    max,
                    body,
                    greedy,
                } => {
                    self.handle_repeat_check(*counter_reg, *min, *max, *body, *greedy);
                }

                Instruction::LookStart {
                    positive,
                    forward,
                    end,
                } => {
                    if let Some(result) = self.handle_look_start(*positive, *forward, *end) {
                        return result;
                    }
                }

                Instruction::LookEnd => {
                    return VmResult::Match;
                }

                Instruction::PushModifiers {
                    ignore_case,
                    multiline,
                    dot_all,
                } => {
                    self.handle_push_modifiers(*ignore_case, *multiline, *dot_all);
                }

                Instruction::PopModifiers => {
                    self.handle_pop_modifiers();
                }

                Instruction::Nop => {
                    self.pc += 1;
                }

                Instruction::StringPropertyMatch { strings, property } => {
                    if let Some(result) = self.handle_string_property_match(strings, property) {
                        return result;
                    }
                }

                Instruction::ProgressCheck {
                    reg,
                    clear_captures,
                } => {
                    let reg = *reg as usize;
                    let last_pos = self.registers[reg];
                    if last_pos == self.pos as i32 {
                        // Zero-width match detected. Per ECMA-262, captures from the
                        // body should be cleared to undefined before exiting.
                        for &cap_reg in clear_captures {
                            let r = cap_reg as usize;
                            if r < self.registers.len() {
                                self.registers[r] = -1;
                            }
                        }
                        if !self.backtrack() {
                            return VmResult::NoMatch;
                        }
                    } else {
                        self.registers[reg] = self.pos as i32;
                        self.pc += 1;
                    }
                }

                Instruction::GreedyLoop { matcher, min, max } => {
                    let min = *min;
                    let max = *max;
                    let start_pos = self.pos;

                    let count = Self::greedy_consume_fast(
                        input,
                        &mut self.pos,
                        matcher,
                        &self.modifiers,
                        max,
                    );

                    if count < min {
                        self.pos = start_pos;
                        if !self.backtrack() {
                            return VmResult::NoMatch;
                        }
                    } else {
                        if count > min {
                            let greedy_pos = self.pos;
                            self.push_greedy_backtrack(self.pc, start_pos, greedy_pos, min);
                        }
                        self.pc += 1;
                    }
                }

                Instruction::LazyLoop { matcher, min, max } => {
                    let min = *min;
                    let max = *max;
                    let start_pos = self.pos;

                    for _ in 0..min {
                        if self.pos < input_len {
                            let cp = input.code_unit(self.pos) as u32;
                            if self.match_simple(cp, matcher) {
                                self.pos += 1;
                            } else {
                                self.pos = start_pos;
                                if !self.backtrack() {
                                    return VmResult::NoMatch;
                                }
                                continue 'vm_loop;
                            }
                        } else {
                            self.pos = start_pos;
                            if !self.backtrack() {
                                return VmResult::NoMatch;
                            }
                            continue 'vm_loop;
                        }
                    }

                    self.push_lazy_backtrack(self.pc, self.pos, max, min);
                    self.pc += 1;
                }
            }
        }
    }

    /// General VM loop for Unicode and backward patterns.
    fn run_general(&mut self) -> VmResult {
        let instructions = &self.program.instructions;
        let num_instructions = instructions.len();
        'vm_loop: loop {
            self.steps += 1;
            if self.steps >= MATCH_LIMIT {
                return VmResult::LimitExceeded;
            }

            let pc = self.pc as usize;
            if pc >= num_instructions {
                // Fell off the end — no match.
                if !self.backtrack() {
                    return VmResult::NoMatch;
                }
                continue;
            }

            let inst = &instructions[pc];
            match inst {
                Instruction::Char(c) => {
                    if !self.match_char(*c) {
                        if !self.backtrack() {
                            return VmResult::NoMatch;
                        }
                    } else {
                        self.pc += 1;
                    }
                }

                Instruction::CharNoCase(lo, _hi) => {
                    if let Some(input_char) = self.current_code_point() {
                        if case_fold_eq(input_char, *lo, self.program.unicode) {
                            self.advance_char();
                            self.pc += 1;
                        } else if !self.backtrack() {
                            return VmResult::NoMatch;
                        }
                    } else if !self.backtrack() {
                        return VmResult::NoMatch;
                    }
                }

                Instruction::AnyChar { dot_all } => {
                    if let Some(cp) = self.current_code_point() {
                        let dot_all = *dot_all || self.modifiers.dot_all;
                        if !dot_all && is_line_terminator(cp) {
                            if !self.backtrack() {
                                return VmResult::NoMatch;
                            }
                            continue;
                        }
                        self.advance_char();
                        self.pc += 1;
                    } else if !self.backtrack() {
                        return VmResult::NoMatch;
                    }
                }

                Instruction::CharClass { ranges, negated } => {
                    if let Some(cp) = self.current_code_point() {
                        let in_class = match_char_class(
                            cp,
                            ranges,
                            self.modifiers.ignore_case,
                            self.program.unicode,
                            self.program.unicode_sets,
                        );
                        let matches = in_class != *negated;
                        if matches {
                            self.advance_char();
                            self.pc += 1;
                        } else if !self.backtrack() {
                            return VmResult::NoMatch;
                        }
                    } else if !self.backtrack() {
                        return VmResult::NoMatch;
                    }
                }

                Instruction::BuiltinClass(class) => {
                    let class = *class;
                    if let Some(cp) = self.current_code_point() {
                        let matches = match_builtin_class(
                            cp,
                            class,
                            self.modifiers.ignore_case && self.program.unicode,
                        );
                        if matches {
                            self.advance_char();
                            self.pc += 1;
                        } else if !self.backtrack() {
                            return VmResult::NoMatch;
                        }
                    } else if !self.backtrack() {
                        return VmResult::NoMatch;
                    }
                }

                Instruction::UnicodeProperty(data) => {
                    if let Some(cp) = self.current_code_point() {
                        let result = if self.modifiers.ignore_case && self.program.unicode {
                            if data.negated && !self.program.unicode_sets {
                                !match_unicode_property_all_case_equivalents(
                                    cp,
                                    &data.name,
                                    data.value.as_deref(),
                                )
                            } else {
                                let matched = match_unicode_property_case_insensitive(
                                    cp,
                                    &data.name,
                                    data.value.as_deref(),
                                );
                                if data.negated { !matched } else { matched }
                            }
                        } else {
                            let matched = match_unicode_property_resolved(
                                cp,
                                &data.name,
                                data.value.as_deref(),
                                data.resolved.as_ref(),
                            );
                            if data.negated { !matched } else { matched }
                        };
                        if result {
                            self.advance_char();
                            self.pc += 1;
                        } else if !self.backtrack() {
                            return VmResult::NoMatch;
                        }
                    } else if !self.backtrack() {
                        return VmResult::NoMatch;
                    }
                }

                Instruction::Jump(target) => {
                    self.pc = *target;
                }

                Instruction::Split { prefer, other } => {
                    let prefer = *prefer;
                    let other = *other;
                    let pos = self.pos;
                    self.push_backtrack(other, pos);
                    self.pc = prefer;
                }

                Instruction::Save(reg) => {
                    let mut reg = *reg as usize;
                    // In backward mode, swap start/end registers for capture groups
                    // since we're traversing right-to-left.
                    let capture_register_count = (self.program.capture_count as usize + 1) * 2;
                    if self.backward && reg < capture_register_count {
                        if reg.is_multiple_of(2) {
                            reg += 1; // start → end
                        } else {
                            reg -= 1; // end → start
                        }
                    }
                    if reg < self.registers.len() {
                        self.registers[reg] = self.pos as i32;
                    }
                    self.pc += 1;
                }

                Instruction::ClearRegister(reg) => {
                    let reg = *reg as usize;
                    if reg < self.registers.len() {
                        self.registers[reg] = -1;
                    }
                    self.pc += 1;
                }

                Instruction::AssertStart { multiline } => {
                    if let Some(result) = self.handle_assert_start(*multiline) {
                        return result;
                    }
                }

                Instruction::AssertEnd { multiline } => {
                    if let Some(result) = self.handle_assert_end(*multiline, self.input.len()) {
                        return result;
                    }
                }

                Instruction::AssertWordBoundary => {
                    if let Some(result) = self.handle_word_boundary_assert(true) {
                        return result;
                    }
                }

                Instruction::AssertNonWordBoundary => {
                    if let Some(result) = self.handle_word_boundary_assert(false) {
                        return result;
                    }
                }

                Instruction::Match => {
                    return VmResult::Match;
                }

                Instruction::Fail => {
                    if !self.backtrack() {
                        return VmResult::NoMatch;
                    }
                }

                Instruction::Backref(index) => {
                    if let Some(result) = self.handle_backref(*index) {
                        return result;
                    }
                }

                Instruction::BackrefNamed(name) => {
                    if let Some(result) = self.handle_backref_named(name) {
                        return result;
                    }
                }

                Instruction::RepeatStart { counter_reg } => {
                    self.handle_repeat_start(*counter_reg);
                }

                Instruction::RepeatCheck {
                    counter_reg,
                    min,
                    max,
                    body,
                    greedy,
                } => {
                    self.handle_repeat_check(*counter_reg, *min, *max, *body, *greedy);
                }

                Instruction::LookStart {
                    positive,
                    forward,
                    end,
                } => {
                    if let Some(result) = self.handle_look_start(*positive, *forward, *end) {
                        return result;
                    }
                }

                Instruction::LookEnd => {
                    return VmResult::Match;
                }

                Instruction::PushModifiers {
                    ignore_case,
                    multiline,
                    dot_all,
                } => {
                    self.handle_push_modifiers(*ignore_case, *multiline, *dot_all);
                }

                Instruction::PopModifiers => {
                    self.handle_pop_modifiers();
                }

                Instruction::Nop => {
                    self.pc += 1;
                }

                Instruction::StringPropertyMatch { strings, property } => {
                    if let Some(result) = self.handle_string_property_match(strings, property) {
                        return result;
                    }
                }

                Instruction::ProgressCheck {
                    reg,
                    clear_captures,
                } => {
                    let reg = *reg as usize;
                    let last_pos = self.registers[reg];
                    if last_pos == self.pos as i32 {
                        for &cap_reg in clear_captures {
                            let r = cap_reg as usize;
                            if r < self.registers.len() {
                                self.registers[r] = -1;
                            }
                        }
                        if !self.backtrack() {
                            return VmResult::NoMatch;
                        }
                    } else {
                        self.registers[reg] = self.pos as i32;
                        self.pc += 1;
                    }
                }

                Instruction::GreedyLoop { matcher, min, max } => {
                    let min = *min;
                    let max = *max;
                    let start_pos = self.pos;

                    // Fast path for common non-unicode matchers.
                    let count = if !self.program.unicode && !self.backward {
                        Self::greedy_consume_fast(
                            self.input,
                            &mut self.pos,
                            matcher,
                            &self.modifiers,
                            max,
                        )
                    } else {
                        let mut count: u32 = 0;
                        while max.is_none() || count < max.unwrap() {
                            if let Some(cp) = self.current_code_point() {
                                if self.match_simple(cp, matcher) {
                                    self.advance_char();
                                    count += 1;
                                } else {
                                    break;
                                }
                            } else {
                                break;
                            }
                        }
                        count
                    };

                    if count < min {
                        // Didn't match enough — fail.
                        self.pos = start_pos;
                        if !self.backtrack() {
                            return VmResult::NoMatch;
                        }
                    } else {
                        // Push a greedy backtrack state only if we consumed more than min.
                        if count > min {
                            let greedy_pos = self.pos;
                            self.push_greedy_backtrack(self.pc, start_pos, greedy_pos, min);
                        }
                        self.pc += 1;
                    }
                }

                Instruction::LazyLoop { matcher, min, max } => {
                    let min = *min;
                    let max = *max;
                    let start_pos = self.pos;

                    // Consume minimum required matches.
                    for _ in 0..min {
                        if let Some(cp) = self.current_code_point() {
                            if self.match_simple(cp, matcher) {
                                self.advance_char();
                            } else {
                                // Can't even match minimum — fail.
                                self.pos = start_pos;
                                if !self.backtrack() {
                                    return VmResult::NoMatch;
                                }
                                continue 'vm_loop;
                            }
                        } else {
                            self.pos = start_pos;
                            if !self.backtrack() {
                                return VmResult::NoMatch;
                            }
                            continue 'vm_loop;
                        }
                    }

                    // Push lazy backtrack: try matching one more on backtrack.
                    self.push_lazy_backtrack(self.pc, self.pos, max, min);
                    self.pc += 1;
                }
            }
        }
    }

    // Shared opcode handlers used by both VM loops.
    #[inline(always)]
    fn fail_current_path(&mut self) -> Option<VmResult> {
        if self.backtrack() {
            None
        } else {
            Some(VmResult::NoMatch)
        }
    }

    #[inline(always)]
    fn handle_assert_start(&mut self, multiline: bool) -> Option<VmResult> {
        let multiline = multiline || self.modifiers.multiline;
        let at_start = self.pos == 0
            || (multiline
                && self.pos > 0
                && is_line_terminator(self.input_code_unit(self.pos - 1) as u32));
        if at_start {
            self.pc += 1;
            None
        } else {
            self.fail_current_path()
        }
    }

    #[inline(always)]
    fn handle_assert_end(&mut self, multiline: bool, input_len: usize) -> Option<VmResult> {
        let multiline = multiline || self.modifiers.multiline;
        let at_end = self.pos >= input_len
            || (multiline && is_line_terminator(self.input_code_unit(self.pos) as u32));
        if at_end {
            self.pc += 1;
            None
        } else {
            self.fail_current_path()
        }
    }

    #[inline(always)]
    fn handle_word_boundary_assert(&mut self, should_match_boundary: bool) -> Option<VmResult> {
        if self.at_word_boundary() == should_match_boundary {
            self.pc += 1;
            None
        } else {
            self.fail_current_path()
        }
    }

    #[inline(always)]
    fn handle_backref(&mut self, index: u32) -> Option<VmResult> {
        if self.match_backref(index) {
            self.pc += 1;
            None
        } else {
            self.fail_current_path()
        }
    }

    #[inline(always)]
    fn handle_backref_named(&mut self, name: &str) -> Option<VmResult> {
        if self.match_backref_named(name) {
            self.pc += 1;
            None
        } else {
            self.fail_current_path()
        }
    }

    #[inline(always)]
    fn handle_repeat_start(&mut self, counter_reg: u32) {
        let reg = counter_reg as usize;
        if reg < self.registers.len() {
            self.registers[reg] = 0;
        }
        self.pc += 1;
    }

    #[inline(always)]
    fn handle_repeat_check(
        &mut self,
        counter_reg: u32,
        min: u32,
        max: Option<u32>,
        body: u32,
        greedy: bool,
    ) {
        let reg = counter_reg as usize;
        let count = self.registers[reg] as u32;

        if count < min {
            self.registers[reg] += 1;
            self.pc = body;
        } else if max.is_some_and(|m| count >= m) {
            self.pc += 1;
        } else if greedy {
            let pos = self.pos;
            let exit_pc = self.pc + 1;
            self.push_backtrack(exit_pc, pos);
            self.registers[reg] += 1;
            self.pc = body;
        } else {
            let pos = self.pos;
            let new_count = self.registers[reg] + 1;
            self.push_backtrack_modified(body, pos, reg, new_count);
            self.pc += 1;
        }
    }

    fn handle_look_start(&mut self, positive: bool, forward: bool, end: u32) -> Option<VmResult> {
        let saved_pos = self.pos;
        let saved_bt_len = self.backtrack_stack.len();
        let saved_pool_len = self.register_pool.len();
        let saved_bt_floor = self.bt_floor;
        let saved_backward = self.backward;
        let reg_save_offset = self.register_pool.len();
        let reg_count = self.registers.len();
        self.register_pool.extend_from_slice(self.registers);

        self.bt_floor = saved_bt_len;
        self.pc += 1; // Skip to body start.
        self.backward = !forward;

        let body_result = self.run();

        self.bt_floor = saved_bt_floor;
        self.backward = saved_backward;
        self.pos = saved_pos;
        self.backtrack_stack.truncate(saved_bt_len);

        if body_result == VmResult::LimitExceeded {
            return Some(VmResult::LimitExceeded);
        }
        let body_matched = body_result == VmResult::Match;

        if positive {
            if body_matched {
                self.register_pool.truncate(saved_pool_len);
                self.pc = end;
            } else {
                self.registers.copy_from_slice(
                    &self.register_pool[reg_save_offset..reg_save_offset + reg_count],
                );
                self.register_pool.truncate(saved_pool_len);
                return self.fail_current_path();
            }
        } else {
            self.registers
                .copy_from_slice(&self.register_pool[reg_save_offset..reg_save_offset + reg_count]);
            self.register_pool.truncate(saved_pool_len);
            if body_matched {
                return self.fail_current_path();
            }
            self.pc = end;
        }

        None
    }

    #[inline(always)]
    fn handle_push_modifiers(
        &mut self,
        ignore_case: Option<bool>,
        multiline: Option<bool>,
        dot_all: Option<bool>,
    ) {
        self.modifier_stack.push(self.modifiers);
        if let Some(v) = ignore_case {
            self.modifiers.ignore_case = v;
        }
        if let Some(v) = multiline {
            self.modifiers.multiline = v;
        }
        if let Some(v) = dot_all {
            self.modifiers.dot_all = v;
        }
        self.pc += 1;
    }

    #[inline(always)]
    fn handle_pop_modifiers(&mut self) {
        if let Some(prev) = self.modifier_stack.pop() {
            self.modifiers = prev;
        }
        self.pc += 1;
    }

    fn handle_string_property_match(
        &mut self,
        strings: &[u32],
        property: &UnicodePropertyData,
    ) -> Option<VmResult> {
        // Try multi-codepoint strings first (longest match, atomic).
        let mut matched_len = 0usize;
        let mut offset = 0usize;
        while offset < strings.len() {
            let slen = strings[offset] as usize;
            offset += 1;
            if offset + slen > strings.len() {
                break;
            }
            // Try to match this string at current position.
            let mut ok = true;
            let mut try_pos = self.pos;
            for i in 0..slen {
                let expected = strings[offset + i];
                if self.backward {
                    if try_pos == 0 {
                        ok = false;
                        break;
                    }
                    try_pos -= 1;
                    let cu = self.input_code_unit(try_pos) as u32;
                    if self.program.unicode
                        && is_low_surrogate(cu as u16)
                        && try_pos > 0
                        && is_high_surrogate(self.input_code_unit(try_pos - 1))
                    {
                        try_pos -= 1;
                        let hi = self.input_code_unit(try_pos) as u32;
                        let cp = 0x10000 + ((hi - 0xD800) << 10) + (cu - 0xDC00);
                        if cp != expected {
                            ok = false;
                            break;
                        }
                    } else if cu != expected {
                        ok = false;
                        break;
                    }
                } else {
                    if try_pos >= self.input_len() {
                        ok = false;
                        break;
                    }
                    let cu = self.input_code_unit(try_pos) as u32;
                    if self.program.unicode
                        && is_high_surrogate(self.input_code_unit(try_pos))
                        && try_pos + 1 < self.input_len()
                        && is_low_surrogate(self.input_code_unit(try_pos + 1))
                    {
                        let lo = self.input_code_unit(try_pos + 1) as u32;
                        let cp = 0x10000 + ((cu - 0xD800) << 10) + (lo - 0xDC00);
                        if cp != expected {
                            ok = false;
                            break;
                        }
                        try_pos += 2;
                    } else {
                        if cu != expected {
                            ok = false;
                            break;
                        }
                        try_pos += 1;
                    }
                }
            }
            if ok && slen > 0 {
                // Use this match (longest first, atomic).
                matched_len = try_pos.abs_diff(self.pos);
                break;
            }
            offset += slen;
        }

        if matched_len > 0 {
            if self.backward {
                self.pos -= matched_len;
            } else {
                self.pos += matched_len;
            }
            self.pc += 1;
            return None;
        }

        // Fall back to single-codepoint property match.
        if let Some(cp) = self.current_code_point() {
            let result = match_unicode_property_resolved(
                cp,
                &property.name,
                property.value.as_deref(),
                property.resolved.as_ref(),
            );
            if result {
                self.advance_char();
                self.pc += 1;
                None
            } else {
                self.fail_current_path()
            }
        } else {
            self.fail_current_path()
        }
    }

    #[inline(always)]
    fn match_char(&mut self, target_cp: u32) -> bool {
        if let Some(input_cp) = self.current_code_point() {
            let matches = if self.modifiers.ignore_case {
                case_fold_eq(input_cp, target_cp, self.program.unicode)
            } else {
                input_cp == target_cp
            };
            if matches {
                self.advance_char();
                true
            } else {
                false
            }
        } else {
            false
        }
    }

    /// Get the current code point.
    /// In Unicode mode, decodes surrogate pairs into a single code point.
    /// In non-Unicode mode, returns individual code units.
    #[inline(always)]
    fn current_code_point(&self) -> Option<u32> {
        if self.backward {
            // Backward mode: read character to the LEFT of current position.
            if self.pos == 0 {
                return None;
            }
            let cu = self.input_code_unit(self.pos - 1) as u32;
            // In Unicode mode, check for surrogate pair (low surrogate preceded by high).
            if self.program.unicode
                && is_low_surrogate(cu as u16)
                && self.pos >= 2
                && is_high_surrogate(self.input_code_unit(self.pos - 2))
            {
                let hi = self.input_code_unit(self.pos - 2) as u32;
                let lo = cu;
                Some(0x10000 + ((hi - 0xD800) << 10) + (lo - 0xDC00))
            } else {
                Some(cu)
            }
        } else {
            if self.pos >= self.input_len() {
                return None;
            }
            let cu = self.input_code_unit(self.pos) as u32;
            // Only decode surrogate pairs in Unicode mode.
            if self.program.unicode
                && is_high_surrogate(cu as u16)
                && self.pos + 1 < self.input_len()
                && is_low_surrogate(self.input_code_unit(self.pos + 1))
            {
                let hi = cu;
                let lo = self.input_code_unit(self.pos + 1) as u32;
                Some(0x10000 + ((hi - 0xD800) << 10) + (lo - 0xDC00))
            } else {
                Some(cu)
            }
        }
    }

    /// Advance past the current character.
    /// In Unicode mode, advances past surrogate pairs (2 code units).
    /// In non-Unicode mode, advances by 1 code unit.
    #[inline(always)]
    fn advance_char(&mut self) {
        if self.backward {
            // Backward mode: move position LEFT.
            if self.pos > 0 {
                self.pos -= 1;
                if self.program.unicode
                    && self.pos > 0
                    && is_low_surrogate(self.input_code_unit(self.pos))
                    && is_high_surrogate(self.input_code_unit(self.pos - 1))
                {
                    self.pos -= 1;
                }
            }
        } else if self.pos < self.input_len() {
            let cu = self.input_code_unit(self.pos);
            self.pos += 1;
            if self.program.unicode
                && is_high_surrogate(cu)
                && self.pos < self.input_len()
                && is_low_surrogate(self.input_code_unit(self.pos))
            {
                self.pos += 1;
            }
        }
    }

    /// Push a backtrack state with a snapshot of the current registers.
    #[inline(always)]
    fn push_backtrack(&mut self, pc: u32, pos: usize) {
        let offset = self.snapshot_registers();
        self.backtrack_stack.push(SavedState::Normal {
            pc,
            pos,
            reg_snapshot_offset: offset,
            modifiers: self.modifiers,
            modifier_stack_len: self.modifier_stack.len(),
        });
    }

    /// Push a backtrack state with a MODIFIED copy of the current registers.
    /// The modification is applied at `mod_index` with `mod_value`.
    #[inline(always)]
    fn push_backtrack_modified(&mut self, pc: u32, pos: usize, mod_index: usize, mod_value: i32) {
        let offset = self.snapshot_registers();
        self.register_pool[offset + mod_index] = mod_value;
        self.backtrack_stack.push(SavedState::Normal {
            pc,
            pos,
            reg_snapshot_offset: offset,
            modifiers: self.modifiers,
            modifier_stack_len: self.modifier_stack.len(),
        });
    }

    /// Push a greedy loop backtrack state.
    #[inline(always)]
    fn push_greedy_backtrack(&mut self, pc: u32, start_pos: usize, current_pos: usize, min: u32) {
        let reg_snapshot_offset = self.snapshot_registers();
        self.backtrack_stack.push(SavedState::Greedy {
            pc,
            start_pos,
            current_pos,
            reg_snapshot_offset,
            min,
            backward: self.backward,
            modifiers: self.modifiers,
            modifier_stack_len: self.modifier_stack.len(),
        });
    }

    /// Push a lazy loop backtrack state.
    #[inline(always)]
    fn push_lazy_backtrack(&mut self, pc: u32, pos: usize, max: Option<u32>, consumed: u32) {
        let reg_snapshot_offset = self.snapshot_registers();
        self.backtrack_stack.push(SavedState::Lazy {
            pc,
            pos,
            reg_snapshot_offset,
            max,
            consumed,
            modifiers: self.modifiers,
            modifier_stack_len: self.modifier_stack.len(),
        });
    }

    #[inline(always)]
    fn snapshot_registers(&mut self) -> usize {
        let offset = self.register_pool.len();
        self.register_pool.extend_from_slice(self.registers);
        offset
    }

    #[inline(always)]
    fn restore_registers(&mut self, reg_snapshot_offset: usize) {
        let reg_count = self.registers.len();
        self.registers.copy_from_slice(
            &self.register_pool[reg_snapshot_offset..reg_snapshot_offset + reg_count],
        );
    }

    fn backtrack(&mut self) -> bool {
        if self.backtrack_stack.len() <= self.bt_floor {
            return false;
        }
        if let Some(state) = self.backtrack_stack.pop() {
            match state {
                SavedState::Normal {
                    pc,
                    pos,
                    reg_snapshot_offset,
                    modifiers,
                    modifier_stack_len,
                } => {
                    self.pc = pc;
                    self.pos = pos;
                    self.restore_registers(reg_snapshot_offset);
                    self.register_pool.truncate(reg_snapshot_offset);
                    self.modifiers = modifiers;
                    self.modifier_stack.truncate(modifier_stack_len);
                }
                SavedState::Greedy {
                    pc,
                    start_pos,
                    current_pos,
                    reg_snapshot_offset,
                    min,
                    backward,
                    modifiers,
                    modifier_stack_len,
                } => {
                    self.restore_registers(reg_snapshot_offset);
                    self.register_pool.truncate(reg_snapshot_offset);
                    self.modifiers = modifiers;
                    self.modifier_stack.truncate(modifier_stack_len);

                    if backward {
                        // Backward mode (lookbehind): the greedy loop consumed
                        // leftward from start_pos to current_pos.
                        // start_pos >= current_pos. Backtracking means giving
                        // back characters by moving current_pos to the RIGHT
                        // (toward start_pos). The limit is that we must keep
                        // at least `min` characters consumed, so max_pos is
                        // start_pos retreated left by `min` characters.
                        let max_pos = if self.program.unicode {
                            let mut p = start_pos;
                            for _ in 0..min {
                                p = self.retreat_one_char(p);
                            }
                            p
                        } else {
                            start_pos - min as usize
                        };

                        let next_inst = self.program.instructions.get(pc as usize + 1);
                        let new_pos = match next_inst {
                            Some(Instruction::Char(target))
                                if !self.modifiers.ignore_case && *target <= 0xFFFF =>
                            {
                                let target = *target as u16;
                                // In backward mode the next Char reads the code
                                // point immediately to the left of the current
                                // position, so scan candidate boundaries and
                                // check the code unit before each boundary.
                                let mut scan_pos = self.advance_one_char(current_pos);
                                loop {
                                    if scan_pos > max_pos {
                                        return self.backtrack();
                                    }
                                    if scan_pos > 0 && self.input_code_unit(scan_pos - 1) == target
                                    {
                                        break scan_pos;
                                    }
                                    let next_scan_pos = self.advance_one_char(scan_pos);
                                    if next_scan_pos == scan_pos {
                                        return self.backtrack();
                                    }
                                    scan_pos = next_scan_pos;
                                }
                            }
                            Some(Instruction::Char(target))
                                if !self.modifiers.ignore_case && *target > 0xFFFF =>
                            {
                                let hi = ((*target - 0x10000) >> 10) as u16 + 0xD800;
                                let lo = ((*target - 0x10000) & 0x3FF) as u16 + 0xDC00;
                                let mut scan_pos = self.advance_one_char(current_pos);
                                loop {
                                    if scan_pos > max_pos {
                                        return self.backtrack();
                                    }
                                    if scan_pos >= 2
                                        && self.input_code_unit(scan_pos - 2) == hi
                                        && self.input_code_unit(scan_pos - 1) == lo
                                    {
                                        break scan_pos;
                                    }
                                    let next_scan_pos = self.advance_one_char(scan_pos);
                                    if next_scan_pos == scan_pos {
                                        return self.backtrack();
                                    }
                                    scan_pos = next_scan_pos;
                                }
                            }
                            _ => {
                                // Give back one character: advance rightward.
                                let advanced = self.advance_one_char(current_pos);
                                if advanced > max_pos {
                                    return self.backtrack();
                                }
                                advanced
                            }
                        };

                        // Push updated state if we can still give back more.
                        if new_pos < max_pos {
                            let reg_snapshot_offset = self.snapshot_registers();
                            self.backtrack_stack.push(SavedState::Greedy {
                                pc,
                                start_pos,
                                current_pos: new_pos,
                                reg_snapshot_offset,
                                min,
                                backward,
                                modifiers,
                                modifier_stack_len,
                            });
                        }

                        self.pos = new_pos;
                        self.pc = pc + 1;
                    } else {
                        // Forward mode: the greedy loop consumed rightward from
                        // start_pos to current_pos. Backtracking means giving
                        // back characters by moving current_pos LEFT.

                        // Compute minimum position by advancing `min` characters
                        // from start_pos. In Unicode mode supplementary characters
                        // occupy 2 code units, so we cannot simply add `min`.
                        let min_pos = if self.program.unicode {
                            let mut p = start_pos;
                            for _ in 0..min {
                                p = self.advance_one_char(p);
                            }
                            p
                        } else {
                            start_pos + min as usize
                        };

                        // Optimization: peek at the next instruction. If it's a
                        // Char, scan backward for that char to skip positions
                        // that can't match.
                        let next_inst = self.program.instructions.get(pc as usize + 1);
                        let new_pos = match next_inst {
                            Some(Instruction::Char(target))
                                if !self.modifiers.ignore_case && *target <= 0xFFFF =>
                            {
                                // BMP char: scan code units directly.
                                let target = *target as u16;
                                let mut scan_pos =
                                    if current_pos > 0 { current_pos - 1 } else { 0 };
                                loop {
                                    if scan_pos < min_pos {
                                        return self.backtrack();
                                    }
                                    if self.input_code_unit(scan_pos) == target {
                                        break scan_pos;
                                    }
                                    if scan_pos == 0 {
                                        return self.backtrack();
                                    }
                                    scan_pos -= 1;
                                }
                            }
                            Some(Instruction::Char(target))
                                if !self.modifiers.ignore_case && *target > 0xFFFF =>
                            {
                                // Supplementary char: scan for the surrogate pair.
                                let hi = ((*target - 0x10000) >> 10) as u16 + 0xD800;
                                let lo = ((*target - 0x10000) & 0x3FF) as u16 + 0xDC00;
                                let mut scan_pos =
                                    if current_pos > 1 { current_pos - 2 } else { 0 };
                                loop {
                                    if scan_pos < min_pos {
                                        return self.backtrack();
                                    }
                                    if scan_pos + 1 < self.input_len()
                                        && self.input_code_unit(scan_pos) == hi
                                        && self.input_code_unit(scan_pos + 1) == lo
                                    {
                                        break scan_pos;
                                    }
                                    if scan_pos == 0 {
                                        return self.backtrack();
                                    }
                                    scan_pos -= 1;
                                }
                            }
                            _ => {
                                // Default: retreat one character.
                                let retreated = self.retreat_one_char(current_pos);
                                if retreated < min_pos {
                                    return self.backtrack();
                                }
                                retreated
                            }
                        };

                        // Push updated greedy state for further backtracking.
                        if new_pos > min_pos {
                            let reg_snapshot_offset = self.snapshot_registers();
                            self.backtrack_stack.push(SavedState::Greedy {
                                pc,
                                start_pos,
                                current_pos: new_pos,
                                reg_snapshot_offset,
                                min,
                                backward,
                                modifiers,
                                modifier_stack_len,
                            });
                        }

                        self.pos = new_pos;
                        self.pc = pc + 1;
                    }
                }
                SavedState::Lazy {
                    pc,
                    pos,
                    reg_snapshot_offset,
                    max,
                    consumed,
                    modifiers,
                    modifier_stack_len,
                } => {
                    self.restore_registers(reg_snapshot_offset);
                    self.register_pool.truncate(reg_snapshot_offset);
                    self.modifiers = modifiers;
                    self.modifier_stack.truncate(modifier_stack_len);

                    // Check if we can consume one more.
                    if max.is_some_and(|m| consumed >= m) {
                        // At max — can't consume more.
                        return self.backtrack();
                    }

                    // Try to consume one more character.
                    self.pos = pos;
                    if let Some(cp) = self.current_code_point() {
                        let matcher = match &self.program.instructions[pc as usize] {
                            Instruction::LazyLoop { matcher, .. } => matcher,
                            _ => unreachable!(),
                        };
                        if self.match_simple(cp, matcher) {
                            self.advance_char();
                            let new_pos = self.pos;

                            // Push another lazy state for further backtracking.
                            self.push_lazy_backtrack(pc, new_pos, max, consumed + 1);

                            self.pc = pc + 1; // Continue to instruction after the loop.
                        } else {
                            // Can't match — continue backtracking.
                            return self.backtrack();
                        }
                    } else {
                        return self.backtrack();
                    }
                }
            }
            true
        } else {
            false
        }
    }

    /// Check if a code point matches a SimpleMatch.
    #[inline(always)]
    fn match_simple(&self, cp: u32, matcher: &SimpleMatch) -> bool {
        match matcher {
            SimpleMatch::AnyChar { dot_all } => {
                let dot_all = *dot_all || self.modifiers.dot_all;
                dot_all || !is_line_terminator(cp)
            }
            SimpleMatch::Char(c) => {
                if self.modifiers.ignore_case {
                    case_fold_eq(cp, *c, self.program.unicode)
                } else {
                    cp == *c
                }
            }
            SimpleMatch::CharNoCase(lo, _hi) => case_fold_eq(cp, *lo, self.program.unicode),
            SimpleMatch::CharClass { ranges, negated } => {
                let in_class = match_char_class(
                    cp,
                    ranges,
                    self.modifiers.ignore_case,
                    self.program.unicode,
                    self.program.unicode_sets,
                );
                in_class != *negated
            }
            SimpleMatch::BuiltinClass(class) => match_builtin_class(
                cp,
                *class,
                self.modifiers.ignore_case && self.program.unicode,
            ),
            SimpleMatch::UnicodeProperty(data) => {
                if self.modifiers.ignore_case && self.program.unicode {
                    if data.negated && !self.program.unicode_sets {
                        !match_unicode_property_all_case_equivalents(
                            cp,
                            &data.name,
                            data.value.as_deref(),
                        )
                    } else {
                        let matched = match_unicode_property_case_insensitive(
                            cp,
                            &data.name,
                            data.value.as_deref(),
                        );
                        if data.negated { !matched } else { matched }
                    }
                } else {
                    let matched = match_unicode_property_resolved(
                        cp,
                        &data.name,
                        data.value.as_deref(),
                        data.resolved.as_ref(),
                    );
                    matched != data.negated
                }
            }
        }
    }

    /// Move position back by one character (handling surrogate pairs in unicode mode).
    #[inline(always)]
    fn retreat_one_char(&self, pos: usize) -> usize {
        if pos == 0 {
            return 0;
        }
        if self.program.unicode
            && pos >= 2
            && is_low_surrogate(self.input_code_unit(pos - 1))
            && is_high_surrogate(self.input_code_unit(pos - 2))
        {
            pos - 2
        } else {
            pos - 1
        }
    }

    /// Move position forward by one character (handling surrogate pairs in unicode mode).
    #[inline(always)]
    fn advance_one_char(&self, pos: usize) -> usize {
        if pos >= self.input_len() {
            return pos;
        }
        let mut p = pos + 1;
        if self.program.unicode
            && p < self.input_len()
            && is_low_surrogate(self.input_code_unit(p))
            && p >= 1
            && is_high_surrogate(self.input_code_unit(p - 1))
        {
            p += 1;
        }
        p
    }

    /// Fast greedy consume loop for non-unicode, non-backward mode.
    /// Returns the number of characters consumed.
    #[inline(always)]
    fn greedy_consume_fast(
        input: I,
        pos: &mut usize,
        matcher: &SimpleMatch,
        modifiers: &ActiveModifiers,
        max: Option<u32>,
    ) -> u32 {
        let mut count: u32 = 0;
        let len = input.len();
        let limit = max.unwrap_or(u32::MAX);

        match matcher {
            SimpleMatch::AnyChar { dot_all } => {
                let dot_all = *dot_all || modifiers.dot_all;
                if dot_all {
                    // Match everything: just advance to end (or limit).
                    let advance = std::cmp::min(len - *pos, limit as usize);
                    *pos += advance;
                    count = advance as u32;
                } else {
                    // Match everything except line terminators.
                    while *pos < len && count < limit {
                        if is_line_terminator(input.code_unit(*pos) as u32) {
                            break;
                        }
                        *pos += 1;
                        count += 1;
                    }
                }
            }
            SimpleMatch::Char(c) => {
                let c = *c as u16;
                if modifiers.ignore_case {
                    while *pos < len && count < limit {
                        if !case_fold_eq(input.code_unit(*pos) as u32, c as u32, false) {
                            break;
                        }
                        *pos += 1;
                        count += 1;
                    }
                } else {
                    while *pos < len && count < limit {
                        if input.code_unit(*pos) != c {
                            break;
                        }
                        *pos += 1;
                        count += 1;
                    }
                }
            }
            SimpleMatch::CharNoCase(lo, _hi) => {
                while *pos < len && count < limit {
                    if !case_fold_eq(input.code_unit(*pos) as u32, *lo, false) {
                        break;
                    }
                    *pos += 1;
                    count += 1;
                }
            }
            SimpleMatch::CharClass { ranges, negated } => {
                if modifiers.ignore_case {
                    while *pos < len && count < limit {
                        let in_class = match_char_class(
                            input.code_unit(*pos) as u32,
                            ranges,
                            true,
                            false,
                            false,
                        );
                        if in_class == *negated {
                            break;
                        }
                        *pos += 1;
                        count += 1;
                    }
                } else {
                    while *pos < len && count < limit {
                        let in_class = char_in_ranges(input.code_unit(*pos) as u32, ranges);
                        if in_class == *negated {
                            break;
                        }
                        *pos += 1;
                        count += 1;
                    }
                }
            }
            SimpleMatch::BuiltinClass(class) => {
                while *pos < len && count < limit {
                    if !match_builtin_class(input.code_unit(*pos) as u32, *class, false) {
                        break;
                    }
                    *pos += 1;
                    count += 1;
                }
            }
            SimpleMatch::UnicodeProperty(data) => {
                while *pos < len && count < limit {
                    let cp = input.code_unit(*pos) as u32;
                    let matched = match_unicode_property_resolved(
                        cp,
                        &data.name,
                        data.value.as_deref(),
                        data.resolved.as_ref(),
                    );
                    if matched == data.negated {
                        break;
                    }
                    *pos += 1;
                    count += 1;
                }
            }
        }
        count
    }

    /// Check the current input position against the ECMA-262 word-boundary
    /// assertion definition.
    /// - <https://tc39.es/ecma262/#sec-compileassertion>
    /// - <https://tc39.es/ecma262/#sec-wordcharacters>
    fn at_word_boundary(&self) -> bool {
        let uic = self.modifiers.ignore_case && self.program.unicode;
        let before = if self.pos > 0 {
            is_word_char_unicode(self.input_code_unit(self.pos - 1) as u32, uic)
        } else {
            false
        };
        let after = if self.pos < self.input_len() {
            is_word_char_unicode(self.input_code_unit(self.pos) as u32, uic)
        } else {
            false
        };
        before != after
    }

    /// Implement `BackreferenceMatcher` for a numbered capture.
    /// <https://tc39.es/ecma262/#sec-backreference-matcher>
    fn match_backref(&mut self, index: u32) -> bool {
        let start = self
            .registers
            .get(index as usize * 2)
            .copied()
            .unwrap_or(-1);
        let end = self
            .registers
            .get(index as usize * 2 + 1)
            .copied()
            .unwrap_or(-1);
        if start < 0 || end < 0 {
            // Unmatched group — backreference succeeds matching empty string (ECMA-262).
            return true;
        }
        let start = start as usize;
        let end = end as usize;

        if self.program.unicode {
            // Unicode mode: compare code points, not code units.
            return self.match_backref_unicode(start, end);
        }

        let len = end - start;
        if self.backward {
            if self.pos < len {
                return false;
            }
            if self.modifiers.ignore_case {
                for offset in 0..len {
                    if !case_fold_eq(
                        self.input_code_unit(start + offset) as u32,
                        self.input_code_unit(self.pos - len + offset) as u32,
                        false,
                    ) {
                        return false;
                    }
                }
            } else if !self.input.ranges_equal(start, self.pos - len, len) {
                return false;
            }
            self.pos -= len;
        } else {
            if self.pos + len > self.input_len() {
                return false;
            }
            if self.modifiers.ignore_case {
                for offset in 0..len {
                    if !case_fold_eq(
                        self.input_code_unit(start + offset) as u32,
                        self.input_code_unit(self.pos + offset) as u32,
                        false,
                    ) {
                        return false;
                    }
                }
            } else if !self.input.ranges_equal(start, self.pos, len) {
                return false;
            }
            self.pos += len;
        }
        true
    }

    /// Unicode-mode backreference: compare code points instead of raw code
    /// units, matching `BackreferenceMatcher`'s Unicode-aware behavior.
    /// <https://tc39.es/ecma262/#sec-backreference-matcher>
    fn match_backref_unicode(&mut self, cap_start: usize, cap_end: usize) -> bool {
        let mut cap_pos = cap_start;
        let mut inp_pos = self.pos;

        if self.backward {
            // Count code points in captured text.
            let mut cp_count = 0;
            let mut p = cap_start;
            while p < cap_end {
                p += code_point_len(true, self.input, p);
                cp_count += 1;
            }
            // Walk backwards by code points first, then compare forward from
            // that candidate start so surrogate pairs are decoded the same way
            // in both the capture and the input window.
            let mut check_pos = self.pos;
            for _ in 0..cp_count {
                if check_pos == 0 {
                    return false;
                }
                check_pos -= 1;
                if check_pos > 0
                    && is_low_surrogate(self.input_code_unit(check_pos))
                    && is_high_surrogate(self.input_code_unit(check_pos - 1))
                {
                    check_pos -= 1;
                }
            }
            // Compare forward from check_pos against captured text.
            let mut a = cap_start;
            let mut b = check_pos;
            while a < cap_end {
                let cp_a = decode_code_point(true, self.input, a);
                if b >= self.pos {
                    return false;
                }
                let cp_b = decode_code_point(true, self.input, b);
                if self.modifiers.ignore_case {
                    if !case_fold_eq(cp_a, cp_b, true) {
                        return false;
                    }
                } else if cp_a != cp_b {
                    return false;
                }
                a += code_point_len(true, self.input, a);
                b += code_point_len(true, self.input, b);
            }
            self.pos = check_pos;
        } else {
            while cap_pos < cap_end {
                if inp_pos >= self.input_len() {
                    return false;
                }
                let cp_cap = decode_code_point(true, self.input, cap_pos);
                let cp_inp = decode_code_point(true, self.input, inp_pos);
                if self.modifiers.ignore_case {
                    if !case_fold_eq(cp_cap, cp_inp, true) {
                        return false;
                    }
                } else if cp_cap != cp_inp {
                    return false;
                }
                cap_pos += code_point_len(true, self.input, cap_pos);
                inp_pos += code_point_len(true, self.input, inp_pos);
            }
            self.pos = inp_pos;
        }
        true
    }

    /// Match a named backreference, handling duplicate named groups correctly.
    ///
    /// Per ECMA-262, when there are duplicate named capture groups (allowed in
    /// different alternatives), `\k<name>` should match the capture from the
    /// group that actually participated in the match. If none of the groups
    /// with this name participated, the backreference matches the empty string.
    /// <https://tc39.es/ecma262/#sec-backreference-matcher>
    fn match_backref_named(&mut self, name: &str) -> bool {
        // First, find a group with this name that actually captured something.
        let mut captured_index = None;
        let mut any_group_exists = false;
        for ng in &self.program.named_groups {
            if ng.name == name {
                any_group_exists = true;
                let start = self
                    .registers
                    .get(ng.index as usize * 2)
                    .copied()
                    .unwrap_or(-1);
                let end = self
                    .registers
                    .get(ng.index as usize * 2 + 1)
                    .copied()
                    .unwrap_or(-1);
                if start >= 0 && end >= 0 {
                    captured_index = Some(ng.index);
                    break;
                }
            }
        }
        if !any_group_exists {
            // No group with this name exists at all — match empty string.
            return true;
        }
        match captured_index {
            Some(index) => self.match_backref(index),
            // All groups with this name are unmatched — match empty string.
            None => true,
        }
    }
}

// -------------------------------------------------------------------
// Helper functions
// -------------------------------------------------------------------

pub(crate) fn is_high_surrogate(cu: u16) -> bool {
    (0xD800..=0xDBFF).contains(&cu)
}

pub(crate) fn is_low_surrogate(cu: u16) -> bool {
    (0xDC00..=0xDFFF).contains(&cu)
}

#[inline(always)]
pub(crate) fn is_line_terminator(cp: u32) -> bool {
    matches!(cp, 0x000A | 0x000D | 0x2028 | 0x2029)
}

/// Fast path for `Canonicalize`.
///
/// In Unicode mode the ASCII shortcut matches simple case folding; in legacy
/// mode it matches the spec's older uppercasing-based canonicalization.
/// <https://tc39.es/ecma262/#sec-runtime-semantics-canonicalize-ch>
#[inline(always)]
fn case_fold(cp: u32, unicode_mode: bool) -> u32 {
    if cp < 128 {
        if unicode_mode {
            // Unicode mode: simple case folding maps uppercase to lowercase.
            if cp >= b'A' as u32 && cp <= b'Z' as u32 {
                return cp + 32;
            }
        } else {
            // Non-unicode mode: Canonicalize uses toUppercase (ECMA-262 22.2.2.7.3).
            if cp >= b'a' as u32 && cp <= b'z' as u32 {
                return cp - 32;
            }
        }
        return cp;
    }
    // Non-ASCII: call FFI which handles both modes correctly.
    crate::unicode_ffi::simple_case_fold(cp, unicode_mode)
}

/// Compare two code points for case-insensitive equality.
#[inline(always)]
pub(crate) fn case_fold_eq(a: u32, b: u32, unicode_mode: bool) -> bool {
    if a == b {
        return true;
    }
    case_fold(a, unicode_mode) == case_fold(b, unicode_mode)
}

#[inline(always)]
fn is_word_char(cp: u32) -> bool {
    matches!(cp, 0x30..=0x39 | 0x41..=0x5A | 0x61..=0x7A | 0x5F)
}

/// Check if a code point is a word character using the `WordCharacters`
/// definition, including the Unicode ignore-case extension when requested.
/// <https://tc39.es/ecma262/#sec-wordcharacters>
#[inline(always)]
fn is_word_char_unicode(cp: u32, unicode_ignore_case: bool) -> bool {
    if is_word_char(cp) {
        return true;
    }
    if unicode_ignore_case && is_word_char(case_fold(cp, true)) {
        return true;
    }
    false
}

#[inline(always)]
fn is_digit(cp: u32) -> bool {
    (0x30..=0x39).contains(&cp)
}

#[inline(always)]
fn is_whitespace(cp: u32) -> bool {
    matches!(
        cp,
        0x0009 | 0x000A | 0x000B | 0x000C | 0x000D | 0x0020 | 0x00A0 | 0x1680 | 0x2000
            ..=0x200A | 0x2028 | 0x2029 | 0x202F | 0x205F | 0x3000 | 0xFEFF
    )
}

/// Check if a code point falls within any of the given ranges.
#[inline(always)]
pub(crate) fn char_in_ranges(cp: u32, ranges: &[CharRange]) -> bool {
    if ranges.len() <= 8 {
        ranges.iter().any(|r| cp >= r.start && cp <= r.end)
    } else {
        ranges
            .binary_search_by(|r| {
                if cp < r.start {
                    std::cmp::Ordering::Greater
                } else if cp > r.end {
                    std::cmp::Ordering::Less
                } else {
                    std::cmp::Ordering::Equal
                }
            })
            .is_ok()
    }
}

/// Check if a code point is in a character class.
///
/// This is the VM-level realization of `CharacterSetMatcher`, including the
/// `MaybeSimpleCaseFolding` rules for ignore-case matching.
/// - <https://tc39.es/ecma262/#sec-runtime-semantics-charactersetmatcher-abstract-operation>
/// - <https://tc39.es/ecma262/#sec-maybesimplecasefolding>
#[inline(always)]
pub(crate) fn match_char_class(
    cp: u32,
    ranges: &[CharRange],
    ignore_case: bool,
    unicode_mode: bool,
    unicode_sets: bool,
) -> bool {
    if ignore_case && unicode_sets {
        // v-flag case-insensitive: check full case closure.
        // Get all case-equivalent code points and check if any falls in the ranges.
        let mut closure_buf = [0u32; 16];
        let count = crate::unicode_ffi::get_case_closure(cp, &mut closure_buf);
        for item in closure_buf.iter().take(count) {
            let equiv = *item;
            if char_in_ranges(equiv, ranges) {
                return true;
            }
        }
        false
    } else if ignore_case {
        // Per ECMA-262, the character set is built from un-folded range
        // endpoints, then Canonicalize is applied to each member for
        // comparison. We must not fold the range endpoints because case
        // folding is not order-preserving.
        if char_in_ranges(cp, ranges) {
            return true;
        }
        let folded = case_fold(cp, unicode_mode);
        if folded != cp && char_in_ranges(folded, ranges) {
            return true;
        }
        // Canonicalize is many-to-one, so we must also check the inverse:
        // other characters that share the same canonical form as cp.
        // Always use the ICU-based range matcher which correctly handles
        // cross-script case folding (e.g. U+017F ſ folds to ASCII s).
        ranges.iter().any(|r| {
            crate::unicode_ffi::code_point_matches_range_ignoring_case(
                cp,
                r.start,
                r.end,
                unicode_mode,
            )
        })
    } else {
        char_in_ranges(cp, ranges)
    }
}

#[inline(always)]
pub(crate) fn match_builtin_class(
    cp: u32,
    class: BuiltinCharacterClass,
    unicode_ignore_case: bool,
) -> bool {
    match class {
        BuiltinCharacterClass::Digit => is_digit(cp),
        BuiltinCharacterClass::NonDigit => !is_digit(cp),
        BuiltinCharacterClass::Word => is_word_char_unicode(cp, unicode_ignore_case),
        BuiltinCharacterClass::NonWord => !is_word_char_unicode(cp, unicode_ignore_case),
        BuiltinCharacterClass::Whitespace => is_whitespace(cp),
        BuiltinCharacterClass::NonWhitespace => !is_whitespace(cp),
    }
}

/// Match a Unicode property considering case closure for ignore-case matching.
/// <https://tc39.es/ecma262/#sec-maybesimplecasefolding>
pub(crate) fn match_unicode_property_case_insensitive(
    cp: u32,
    name: &str,
    value: Option<&str>,
) -> bool {
    crate::unicode_ffi::property_matches_case_insensitive(cp, name, value)
}

/// Check if all case-equivalents of `cp` have the property.
/// <https://tc39.es/ecma262/#sec-maybesimplecasefolding>
pub(crate) fn match_unicode_property_all_case_equivalents(
    cp: u32,
    name: &str,
    value: Option<&str>,
) -> bool {
    crate::unicode_ffi::property_all_case_equivalents_match(cp, name, value)
}

/// Match a Unicode property via FFI to C++ LibUnicode.
/// - <https://tc39.es/ecma262/#sec-runtime-semantics-unicodematchproperty-p>
/// - <https://tc39.es/ecma262/#sec-runtime-semantics-unicodematchpropertyvalue-p-v>
fn match_unicode_property(cp: u32, name: &str, value: Option<&str>) -> bool {
    crate::unicode_ffi::property_matches(cp, name, value)
}

/// Match a Unicode property using a resolved ICU property ID if available,
/// falling back to string-based lookup.
/// - <https://tc39.es/ecma262/#sec-runtime-semantics-unicodematchproperty-p>
/// - <https://tc39.es/ecma262/#sec-runtime-semantics-unicodematchpropertyvalue-p-v>
#[inline(always)]
pub(crate) fn match_unicode_property_resolved(
    cp: u32,
    name: &str,
    value: Option<&str>,
    resolved: Option<&ResolvedProperty>,
) -> bool {
    if let Some(r) = resolved {
        return crate::unicode_ffi::resolved_property_matches(cp, *r);
    }
    match_unicode_property(cp, name, value)
}
