/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/// High-level regex API.
///
/// This is the main entry point for using the regex engine.
use crate::bytecode::NamedGroupEntry;
use crate::copy_out;
use crate::vm;
use crate::work_zone::with_work_zone;
use ak::Zone;
use libregex_compiler::ast::Flags;
use libregex_compiler::compiler;
use libregex_compiler::literal_hints;
use libregex_compiler::literal_hints::is_ascii_word_code_unit;
use libregex_compiler::parser;
use std::cell::RefCell;

/// A compiled regular expression.
pub struct Regex {
    /// Program for the backtracking VM (with fused loop optimizations).
    program: crate::bytecode::Program,
    flags: Flags,
    hints: vm::PatternHints,
    /// Pre-computed u16 literal for whole-pattern literal search.
    literal_u16: Option<Vec<u16>>,
    /// Pre-computed u16 literal for whole-pattern `\bliteral\b` matching.
    word_boundary_literal_u16: Option<Vec<u16>>,
    /// Pre-computed u16 alternatives for fast literal alternation matching.
    /// Alternatives stay in source order to preserve leftmost-first semantics.
    literal_alt_u16: Option<Vec<Vec<u16>>>,
    /// Cached VM scratch space for reuse across exec calls.
    scratch: RefCell<vm::VmScratch>,
}

impl Regex {
    /// Compile a regex pattern with the given flags. If it does not compile, the result is what `error` makes of the
    /// parser error, which lives only for the call of `error`.
    pub fn compile<E>(pattern: &str, flags: Flags, error: impl FnOnce(parser::Error<'_>) -> E) -> Result<Self, E> {
        with_work_zone(|zone| {
            let source = zone.alloc_slice_from_iter(pattern.chars());
            Self::compile_in_zone(zone, source, flags).map_err(error)
        })
    }

    /// Compile a regex pattern of UTF-16 code units, in which a lone surrogate is U+FFFD (see `compile()`).
    pub fn compile_utf16<E>(
        pattern: &[u16],
        flags: Flags,
        error: impl FnOnce(parser::Error<'_>) -> E,
    ) -> Result<Self, E> {
        with_work_zone(|zone| {
            let source = zone.alloc_slice_from_iter(
                char::decode_utf16(pattern.iter().copied())
                    .map(|character| character.unwrap_or(char::REPLACEMENT_CHARACTER)),
            );
            Self::compile_in_zone(zone, source, flags).map_err(error)
        })
    }

    /// Compile a regex pattern of Latin-1 code units (see `compile()`).
    pub fn compile_latin1<E>(
        pattern: &[u8],
        flags: Flags,
        error: impl FnOnce(parser::Error<'_>) -> E,
    ) -> Result<Self, E> {
        with_work_zone(|zone| {
            let source = zone.alloc_slice_from_iter(pattern.iter().map(|byte| char::from(*byte)));
            Self::compile_in_zone(zone, source, flags).map_err(error)
        })
    }

    fn compile_in_zone<'z>(zone: &'z Zone, source: &'z [char], flags: Flags) -> Result<Self, parser::Error<'z>> {
        let parsed = parser::parse(zone, source, flags)?;
        let zone_program = compiler::compile(zone, &parsed)?;
        let required_literal_hint = literal_hints::extract_required_literal_hint(zone, &parsed, flags);
        let literal_u16 = literal_hints::extract_literal_u16(zone, &parsed, flags);
        let word_boundary_literal_u16 = literal_hints::extract_word_boundary_literal_u16(zone, &parsed, flags);
        let literal_alt_u16 = literal_hints::extract_literal_alternatives_u16(zone, &parsed, flags);

        let program = copy_out::program(&zone_program);
        let hints = vm::analyze_pattern(
            &program,
            required_literal_hint.as_ref().map(copy_out::required_literal_hint),
        );
        let literal_u16 = literal_u16.map(<[u16]>::to_vec);
        let word_boundary_literal_u16 = word_boundary_literal_u16.map(<[u16]>::to_vec);
        let literal_alt_u16 = literal_alt_u16.map(copy_out::literal_alternatives);

        Ok(Self {
            program,
            flags,
            hints,
            literal_u16,
            word_boundary_literal_u16,
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

    pub fn exec_into_latin1(&self, input: &[u8], start: usize, out: &mut [i32]) -> vm::VmResult {
        self.exec_into_input(input, start, out)
    }

    pub(crate) fn exec_into_input<I: vm::Input>(&self, input: I, start: usize, out: &mut [i32]) -> vm::VmResult {
        if self.flags.sticky {
            let scratch = &mut *self.scratch.borrow_mut();
            return vm::execute_anchored_into_with_scratch(&self.program, input, start, &self.hints, out, scratch);
        }

        // Fast path for literal patterns: use fast substring search.
        // NB: Literal searches never hit the step limit.
        if let Some(ref needle) = self.literal_u16 {
            return if Self::literal_search(input, start, needle, &self.flags, out) {
                vm::VmResult::Match
            } else {
                vm::VmResult::NoMatch
            };
        }
        if let Some(ref needle) = self.word_boundary_literal_u16 {
            return if Self::word_boundary_literal_search(input, start, needle, &self.flags, out) {
                vm::VmResult::Match
            } else {
                vm::VmResult::NoMatch
            };
        }
        // Fast path for literal alternation patterns.
        if let Some(ref alts) = self.literal_alt_u16 {
            return if Self::literal_alt_search(input, start, alts, &self.flags, out) {
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

    pub fn test_latin1(&self, input: &[u8], start: usize) -> vm::VmResult {
        self.test_input(input, start)
    }

    pub(crate) fn test_input<I: vm::Input>(&self, input: I, start: usize) -> vm::VmResult {
        if self.flags.sticky {
            let mut out = [-1i32; 2];
            let scratch = &mut *self.scratch.borrow_mut();
            return vm::execute_anchored_into_with_scratch(&self.program, input, start, &self.hints, &mut out, scratch);
        }

        if let Some(ref needle) = self.literal_u16 {
            return if Self::literal_test(input, start, needle, &self.flags) {
                vm::VmResult::Match
            } else {
                vm::VmResult::NoMatch
            };
        }
        if let Some(ref needle) = self.word_boundary_literal_u16 {
            return if Self::word_boundary_literal_test(input, start, needle, &self.flags) {
                vm::VmResult::Match
            } else {
                vm::VmResult::NoMatch
            };
        }
        if let Some(ref alts) = self.literal_alt_u16 {
            let mut out = [-1i32; 2];
            return if Self::literal_alt_search(input, start, alts, &self.flags, &mut out) {
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
    fn literal_search<I: vm::Input>(input: I, start: usize, needle: &[u16], flags: &Flags, out: &mut [i32]) -> bool {
        if needle.is_empty() {
            // Empty pattern matches at start position.
            if out.len() >= 2 {
                out[0] = start as i32;
                out[1] = start as i32;
            }
            return true;
        }

        if flags.ignore_case {
            let needle_len = needle.len();
            if start + needle_len > input.len() {
                return false;
            }
            if needle.iter().all(|ch| *ch <= 0x7F) {
                let first = needle[0];
                let mut pos = start;
                let end = input.len() - needle_len + 1;
                while pos < end {
                    let next = if is_ascii_alpha(first) {
                        find_ascii_case_insensitive_code_unit(input, pos, end, first)
                    } else {
                        input.find_code_unit(pos, end, first)
                    };
                    match next {
                        Some(candidate_pos) => pos = candidate_pos,
                        None => return false,
                    }
                    if matches_ascii_case_insensitive_u16_at(input, pos, needle) {
                        if out.len() >= 2 {
                            out[0] = pos as i32;
                            out[1] = (pos + needle_len) as i32;
                        }
                        return true;
                    }
                    pos += 1;
                }
                return false;
            }
            // Non-ASCII case-insensitive literals fall back to linear scan with
            // full case folding.
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

    fn word_boundary_literal_search<I: vm::Input>(
        input: I,
        start: usize,
        needle: &[u16],
        flags: &Flags,
        out: &mut [i32],
    ) -> bool {
        let mut pos = start;
        let mut literal_out = [-1i32; 2];
        while pos < input.len() {
            literal_out[0] = -1;
            literal_out[1] = -1;
            if !Self::literal_search(input, pos, needle, flags, &mut literal_out) {
                return false;
            }

            let match_start = literal_out[0] as usize;
            let match_end = literal_out[1] as usize;
            if has_ascii_word_boundaries_at(input, match_start, match_end) {
                if out.len() >= 2 {
                    out[0] = match_start as i32;
                    out[1] = match_end as i32;
                }
                return true;
            }

            pos = match_start + 1;
        }
        false
    }

    fn word_boundary_literal_test<I: vm::Input>(input: I, start: usize, needle: &[u16], flags: &Flags) -> bool {
        let mut out = [0i32; 2];
        Self::word_boundary_literal_search(input, start, needle, flags, &mut out)
    }

    /// Fast literal alternation search: find the first matching alternative.
    /// Alternatives are in source order to preserve ECMAScript leftmost-first semantics.
    fn literal_alt_search<I: vm::Input>(
        input: I,
        start: usize,
        alts: &[Vec<u16>],
        flags: &Flags,
        out: &mut [i32],
    ) -> bool {
        if flags.ignore_case {
            return Self::literal_alt_search_ascii_ignore_case(input, start, alts, out);
        }

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

    fn literal_alt_search_ascii_ignore_case<I: vm::Input>(
        input: I,
        start: usize,
        alts: &[Vec<u16>],
        out: &mut [i32],
    ) -> bool {
        let min_alt_len = alts.iter().map(|alt| alt.len()).min().unwrap_or(0);
        if min_alt_len == 0 || start + min_alt_len > input.len() {
            return false;
        }

        let mut first_code_units = [false; 128];
        for alt in alts {
            first_code_units[fold_ascii_for_compare(alt[0]) as usize] = true;
        }

        let mut pos = start;
        let end = input.len() - min_alt_len + 1;
        while pos < end {
            match find_ascii_case_insensitive_code_unit_in_set(input, pos, end, &first_code_units) {
                Some(candidate_pos) => pos = candidate_pos,
                None => return false,
            }

            let folded_first_code_unit = fold_ascii_for_compare(input.code_unit(pos));
            for alt in alts {
                if fold_ascii_for_compare(alt[0]) != folded_first_code_unit {
                    continue;
                }
                let alt_len = alt.len();
                if pos + alt_len > input.len() {
                    continue;
                }
                if matches_ascii_case_insensitive_u16_at(input, pos, alt) {
                    if out.len() >= 2 {
                        out[0] = pos as i32;
                        out[1] = (pos + alt_len) as i32;
                    }
                    return true;
                }
            }
            pos += 1;
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

    fn word_boundary_literal_find_all<I: vm::Input>(
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
            if !Self::word_boundary_literal_search(input, pos, needle, flags, &mut out) {
                break;
            }
            let idx = count as usize * 2;
            if idx + 1 >= capacity {
                return -1;
            }
            result_buf[idx] = out[0];
            result_buf[idx + 1] = out[1];
            count += 1;
            pos = out[1] as usize;
        }
        count
    }

    /// Find all literal alternation matches, writing (start, end) pairs into result_buf.
    fn literal_alt_find_all<I: vm::Input>(
        input: I,
        start: usize,
        alts: &[Vec<u16>],
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
            if !Self::literal_alt_search(input, pos, alts, flags, &mut out) {
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

    pub fn find_all_into_latin1(&self, input: &[u8], start: usize, result_buf: &mut [i32]) -> i32 {
        self.find_all_into_input(input, start, result_buf)
    }

    pub(crate) fn find_all_into_input<I: vm::Input>(&self, input: I, start: usize, result_buf: &mut [i32]) -> i32 {
        // Fast path for literal patterns.
        if let Some(ref needle) = self.literal_u16 {
            return Self::literal_find_all(input, start, needle, &self.flags, result_buf);
        }
        if let Some(ref needle) = self.word_boundary_literal_u16 {
            return Self::word_boundary_literal_find_all(input, start, needle, &self.flags, result_buf);
        }
        // Fast path for literal alternation patterns.
        if let Some(ref alts) = self.literal_alt_u16 {
            return Self::literal_alt_find_all(input, start, alts, &self.flags, result_buf);
        }
        // Use the VM-internal find_all loop which reuses a single VM across matches.
        let scratch = &mut *self.scratch.borrow_mut();
        vm::find_all_with_scratch(&self.program, input, start, &self.hints, result_buf, scratch)
    }
}

#[inline(always)]
fn is_ascii_alpha(ch: u16) -> bool {
    matches!(ch, 0x41..=0x5A | 0x61..=0x7A)
}

#[inline(always)]
fn fold_ascii_for_compare(ch: u16) -> u16 {
    match ch {
        0x41..=0x5A => ch + 32,
        _ => ch,
    }
}

#[inline(always)]
fn find_ascii_case_insensitive_code_unit<I: vm::Input>(
    input: I,
    start: usize,
    end: usize,
    needle: u16,
) -> Option<usize> {
    let folded_needle = fold_ascii_for_compare(needle);
    let mut pos = start;
    while pos < end {
        let code_unit = input.code_unit(pos);
        if code_unit <= 0x7F && fold_ascii_for_compare(code_unit) == folded_needle {
            return Some(pos);
        }
        pos += 1;
    }
    None
}

#[inline(always)]
fn find_ascii_case_insensitive_code_unit_in_set<I: vm::Input>(
    input: I,
    start: usize,
    end: usize,
    needles: &[bool; 128],
) -> Option<usize> {
    let mut pos = start;
    while pos < end {
        let code_unit = input.code_unit(pos);
        if code_unit <= 0x7F && needles[fold_ascii_for_compare(code_unit) as usize] {
            return Some(pos);
        }
        pos += 1;
    }
    None
}

#[inline(always)]
fn matches_ascii_case_insensitive_u16_at<I: vm::Input>(input: I, pos: usize, needle: &[u16]) -> bool {
    if pos + needle.len() > input.len() {
        return false;
    }
    for (offset, expected) in needle.iter().enumerate() {
        let actual = input.code_unit(pos + offset);
        if actual > 0x7F || fold_ascii_for_compare(actual) != fold_ascii_for_compare(*expected) {
            return false;
        }
    }
    true
}

#[inline(always)]
fn has_ascii_word_boundaries_at<I: vm::Input>(input: I, match_start: usize, match_end: usize) -> bool {
    let left_is_word = match_start > 0 && is_ascii_word_code_unit(input.code_unit(match_start - 1));
    let right_is_word = match_end < input.len() && is_ascii_word_code_unit(input.code_unit(match_end));
    !left_is_word && !right_is_word
}
