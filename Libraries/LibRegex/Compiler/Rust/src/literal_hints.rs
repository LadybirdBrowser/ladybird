/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The literals of a pattern that let a search skip the VM: a literal that each match contains, and the literal, the
//! `\bliteral\b` and the literal alternatives that a whole pattern can be. They come from the syntax tree, and they
//! and the work data of their search are in the zone of the compile.

use ak::Zone;
use ak::ZoneMap;
use ak::ZoneVec;

use crate::ast::Alternative;
use crate::ast::AssertionKind;
use crate::ast::Atom;
use crate::ast::Disjunction;
use crate::ast::Flags;
use crate::ast::Pattern;
use crate::ast::Term;

const MAX_REQUIRED_LITERAL_RUNS: usize = 16;
const MAX_REQUIRED_LITERAL_CODE_UNITS: usize = 64;

/// A literal that each match of a pattern contains.
#[derive(Debug, Clone, Copy)]
pub struct RequiredLiteral<'z> {
    pub literal: &'z [u16],
    pub ascii_case_insensitive: bool,
}

#[inline(always)]
pub fn is_ascii_word_code_unit(ch: u16) -> bool {
    matches!(ch, 0x30..=0x39 | 0x41..=0x5A | 0x5F | 0x61..=0x7A)
}

/// Encode a Unicode code point as WTF-16 into `out`.
/// Returns `None` if the code point is out of range (> U+10FFFF).
fn append_code_point_wtf16(out: &mut ZoneVec<'_, u16>, cp: u32) -> Option<()> {
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

/// A run of code units of the literals of a required literal analysis: `len` of its units from `start` on.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
struct Run {
    start: usize,
    len: usize,
}

impl Run {
    fn units(self, units: &[u16]) -> &[u16] {
        &units[self.start..self.start + self.len]
    }
}

/// The search for a literal that each match contains.
///
/// The analysis of a node of the syntax tree gives its exact literal, if each match of the node is that literal, and
/// its guaranteed runs, literals that each match of the node contains. The exact literal of a node is all the code
/// units that its analysis adds to `units`, so the exact literals of the terms of an alternative add up to the exact
/// literal of the alternative where they are, and the units of a node that no run refers to any more are dropped.
struct RequiredLiteralAnalysis<'z> {
    zone: &'z Zone,
    flags: Flags,
    /// The code units of the literals, which the runs are parts of.
    units: ZoneVec<'z, u16>,
    /// The guaranteed runs of the nodes that are being analyzed, the ones of the innermost node last.
    runs: ZoneVec<'z, Run>,
    /// Room for the common runs of the alternatives of a disjunction.
    common_runs: ZoneVec<'z, Run>,
    /// Room for the windows of a run that are in another run.
    matches: ZoneVec<'z, usize>,
}

pub fn extract_required_literal_hint<'z>(
    zone: &'z Zone,
    pattern: &Pattern<'z>,
    flags: Flags,
) -> Option<RequiredLiteral<'z>> {
    let mut analysis = RequiredLiteralAnalysis {
        zone,
        flags,
        units: ZoneVec::new_in(zone),
        runs: ZoneVec::new_in(zone),
        common_runs: ZoneVec::new_in(zone),
        matches: ZoneVec::new_in(zone),
    };
    analysis.analyze_disjunction(&pattern.disjunction);
    let run = *analysis.runs.iter().max_by_key(|run| run.len)?;
    let literal = run.units(analysis.units.into_slice());
    if literal.is_empty() {
        return None;
    }

    let ascii_case_insensitive = if flags.ignore_case {
        if flags.unicode || flags.unicode_sets || literal.iter().any(|ch| *ch > 0x7F) {
            return None;
        }
        true
    } else {
        false
    };

    Some(RequiredLiteral {
        literal,
        ascii_case_insensitive,
    })
}

impl<'z> RequiredLiteralAnalysis<'z> {
    /// The exact literal of the units from `start` on.
    fn exact_from(&self, start: usize) -> Run {
        Run {
            start,
            len: self.units.len() - start,
        }
    }

    fn exact_units(&self, exact: Option<Run>) -> Option<&[u16]> {
        exact.map(|run| run.units(&self.units))
    }

    /// Each analysis returns the exact literal of its node, and leaves the guaranteed runs of the node after the runs
    /// that were there.
    fn analyze_disjunction(&mut self, disjunction: &Disjunction<'z>) -> Option<Run> {
        if crate::stack::is_exhausted() {
            return None;
        }
        let first_run = self.runs.len();
        let Some((first, rest)) = disjunction.alternatives.split_first() else {
            return Some(self.exact_from(self.units.len()));
        };

        let mut exact_literal = self.analyze_alternative(first);
        // NB: The runs of the other alternatives are parts of the runs of the first one, so their units can go.
        let end_of_first = self.units.len();
        for alternative in rest {
            let first_other_run = self.runs.len();
            let other_exact_literal = self.analyze_alternative(alternative);
            if self.exact_units(exact_literal) != self.exact_units(other_exact_literal) {
                exact_literal = None;
            }
            self.intersect_required_literal_runs(first_run, first_other_run);
            self.units.truncate(end_of_first);
            if self.runs.len() == first_run && exact_literal.is_none() {
                break;
            }
        }

        if let Some(literal) = exact_literal
            && literal.len != 0
        {
            self.runs.push(literal);
        }

        self.prune_required_literal_runs(first_run);
        exact_literal
    }

    fn analyze_alternative(&mut self, alternative: &Alternative<'z>) -> Option<Run> {
        let first_run = self.runs.len();
        let start = self.units.len();
        let mut is_exact = true;
        let mut current_run_start = start;

        for term in alternative.terms {
            let term_start = self.units.len();
            let first_term_run = self.runs.len();
            if self.analyze_required_literal_term(term).is_some() {
                // NB: The exact literal of the term continues the current run where it is, and covers its runs.
                self.runs.truncate(first_term_run);
                continue;
            }

            is_exact = false;
            if term_start != current_run_start {
                self.runs.push(Run {
                    start: current_run_start,
                    len: term_start - current_run_start,
                });
            }
            current_run_start = self.units.len();
        }

        if self.units.len() != current_run_start {
            let current_run = self.exact_from(current_run_start);
            self.runs.push(current_run);
        }

        let exact_literal = is_exact.then(|| self.exact_from(start));
        if let Some(literal) = exact_literal
            && literal.len != 0
        {
            self.runs.push(literal);
        }

        self.prune_required_literal_runs(first_run);
        exact_literal
    }

    fn analyze_required_literal_term(&mut self, term: &Term<'z>) -> Option<Run> {
        let start = self.units.len();
        let first_run = self.runs.len();
        let atom_exact_literal = self.analyze_required_literal_atom(&term.atom);
        let Some(quantifier) = &term.quantifier else {
            return atom_exact_literal;
        };
        if quantifier.min == 0 {
            self.runs.truncate(first_run);
            self.units.truncate(start);
            return None;
        }

        let mut exact_literal = None;
        if let Some(literal) = atom_exact_literal {
            let repeated_len = literal.len.saturating_mul(quantifier.min as usize);
            // NB: The literal is the last of the units, so its repetitions follow it.
            let repeated_guaranteed_run = Run {
                start: literal.start,
                len: repeated_len.min(MAX_REQUIRED_LITERAL_CODE_UNITS),
            };
            for index in literal.len..repeated_guaranteed_run.len {
                let unit = self.units[literal.start + index - literal.len];
                self.units.push(unit);
            }
            // Preserve exact-literal tracking only while the full literal fits in the
            // retained hint budget. Longer exact runs still contribute via prefixes.
            if quantifier.max == Some(quantifier.min)
                && literal
                    .len
                    .checked_mul(quantifier.min as usize)
                    .is_some_and(|len| len <= MAX_REQUIRED_LITERAL_CODE_UNITS)
            {
                exact_literal = Some(repeated_guaranteed_run);
            }
            if repeated_guaranteed_run.len != 0 {
                self.runs.push(repeated_guaranteed_run);
            }
        }

        self.prune_required_literal_runs(first_run);
        exact_literal
    }

    fn analyze_required_literal_atom(&mut self, atom: &Atom<'z>) -> Option<Run> {
        match atom {
            Atom::Literal(cp) => self.analyze_literal(*cp),
            Atom::Group(group) => self.analyze_disjunction(&group.body),
            Atom::NonCapturingGroup(group) => self.analyze_disjunction(&group.body),
            Atom::ModifierGroup(group) => {
                if group.add_flags.ignore_case || group.remove_flags.ignore_case {
                    None
                } else {
                    self.analyze_disjunction(&group.body)
                }
            }
            Atom::Assertion(_) | Atom::Lookaround(_) => Some(self.exact_from(self.units.len())),
            Atom::Dot
            | Atom::CharacterClass(_)
            | Atom::BuiltinCharacterClass(_)
            | Atom::UnicodeProperty(_)
            | Atom::Backreference(_) => None,
        }
    }

    fn analyze_literal(&mut self, cp: u32) -> Option<Run> {
        if is_unsafe_unicode_literal(cp, self.flags) {
            return None;
        }

        let start = self.units.len();
        append_code_point_wtf16(&mut self.units, cp)?;
        let literal = self.exact_from(start);
        self.runs.push(literal);
        Some(literal)
    }

    /// Replaces the runs from `first_run` on, which are the runs of one node up to `first_other_run` and those of
    /// another node from there on, with the runs that both nodes are sure to have. Those are parts of the runs of
    /// the first node.
    fn intersect_required_literal_runs(&mut self, first_run: usize, first_other_run: usize) {
        let Self {
            zone,
            units,
            runs,
            common_runs,
            matches,
            ..
        } = self;
        let units = units.as_slice();
        let zone: &Zone = zone;
        let mut windows = ZoneMap::new_in(zone);
        common_runs.clear();
        for left in &runs[first_run..first_other_run] {
            for right in &runs[first_other_run..] {
                maximal_common_substrings(units, *left, *right, common_runs, matches, &mut windows);
            }
        }
        prune_required_literal_runs(units, common_runs, 0);
        runs.truncate(first_run);
        runs.extend_from_slice(common_runs);
    }

    fn prune_required_literal_runs(&mut self, first_run: usize) {
        prune_required_literal_runs(&self.units, &mut self.runs, first_run);
    }
}

/// Adds the longest substrings of `lhs` that are in `rhs` to `out`, as runs of `lhs`.
fn maximal_common_substrings<'a>(
    units: &'a [u16],
    lhs: Run,
    rhs: Run,
    out: &mut ZoneVec<'_, Run>,
    matches: &mut ZoneVec<'_, usize>,
    windows: &mut ZoneMap<'_, &'a [u16], Option<usize>>,
) {
    let max_len = lhs.len.min(rhs.len).min(MAX_REQUIRED_LITERAL_CODE_UNITS);
    let lhs_is_shorter = lhs.len <= rhs.len;
    let (shorter, longer) = if lhs_is_shorter { (lhs, rhs) } else { (rhs, lhs) };
    let first_substring = out.len();

    for len in (1..=max_len).rev() {
        // Required-literal hints keep at most 64 code units, so intersect
        // fixed-size windows instead of walking every start-position pair.
        // NB: Each window that both runs have gives one start in `lhs`, the first one.
        windows.clear();
        for (offset, window) in shorter.units(units).windows(len).enumerate() {
            windows.entry(window).or_insert(Some(shorter.start + offset));
        }
        matches.clear();
        for (offset, window) in longer.units(units).windows(len).enumerate() {
            if let Some(pending) = windows.get_mut(&window)
                && let Some(start) = pending.take()
            {
                matches.push(if lhs_is_shorter { start } else { longer.start + offset });
            }
        }

        matches.sort_unstable_by(|lhs, rhs| units[*lhs..*lhs + len].cmp(&units[*rhs..*rhs + len]));
        for start in matches.iter() {
            let candidate = Run { start: *start, len };
            if out[first_substring..]
                .iter()
                .any(|existing| contains_literal_subslice(existing.units(units), candidate.units(units)))
            {
                continue;
            }
            out.push(candidate);
            if out.len() - first_substring == MAX_REQUIRED_LITERAL_RUNS {
                return;
            }
        }
    }
    prune_required_literal_runs(units, out, first_substring);
}

/// Sorts the runs from `first_run` on, longest first, and keeps the first ones that no run before them contains.
fn prune_required_literal_runs(units: &[u16], runs: &mut ZoneVec<'_, Run>, first_run: usize) {
    // NB: Runs that are the same are equal in each way that counts, so their order does not matter.
    runs[first_run..].sort_unstable_by(|lhs, rhs| {
        rhs.len
            .cmp(&lhs.len)
            .then_with(|| lhs.units(units).cmp(rhs.units(units)))
    });

    let mut pruned_count = first_run;
    for index in first_run..runs.len() {
        let run = runs[index];
        if run.len == 0 {
            continue;
        }
        if runs[first_run..pruned_count]
            .iter()
            .any(|existing| contains_literal_subslice(existing.units(units), run.units(units)))
        {
            continue;
        }
        runs[pruned_count] = run;
        pruned_count += 1;
        if pruned_count - first_run == MAX_REQUIRED_LITERAL_RUNS {
            break;
        }
    }
    runs.truncate(pruned_count);
}

fn contains_literal_subslice(haystack: &[u16], needle: &[u16]) -> bool {
    if needle.is_empty() {
        return true;
    }
    if needle.len() > haystack.len() {
        return false;
    }
    haystack.windows(needle.len()).any(|window| window == needle)
}

pub fn extract_literal_u16<'z>(zone: &'z Zone, pattern: &Pattern<'z>, flags: Flags) -> Option<&'z [u16]> {
    if flags.ignore_case && (flags.unicode || flags.unicode_sets) {
        return None;
    }
    if pattern.disjunction.alternatives.len() != 1 {
        return None;
    }

    let alternative = &pattern.disjunction.alternatives[0];
    let mut literal = ZoneVec::new_in(zone);
    for term in alternative.terms {
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

    Some(literal.into_slice())
}

pub fn extract_literal_alternatives_u16<'z>(
    zone: &'z Zone,
    pattern: &Pattern<'z>,
    flags: Flags,
) -> Option<&'z [&'z [u16]]> {
    if flags.ignore_case && (flags.unicode || flags.unicode_sets) {
        return None;
    }

    let alternatives = pattern.disjunction.alternatives;
    if alternatives.len() < 2 || pattern.capture_count > 0 {
        return None;
    }

    let mut literal_alternatives = ZoneVec::with_capacity_in(alternatives.len(), zone);
    for alternative in alternatives {
        let mut literal = ZoneVec::new_in(zone);
        for term in alternative.terms {
            if term.quantifier.is_some() {
                return None;
            }
            let Atom::Literal(cp) = term.atom else {
                return None;
            };
            if is_unsafe_unicode_literal(cp, flags) {
                return None;
            }
            if flags.ignore_case && cp > 0x7F {
                return None;
            }
            append_code_point_wtf16(&mut literal, cp)?;
        }
        if literal.is_empty() {
            return None;
        }
        literal_alternatives.push(&*literal.into_slice());
    }

    Some(literal_alternatives.into_slice())
}

pub fn extract_word_boundary_literal_u16<'z>(zone: &'z Zone, pattern: &Pattern<'z>, flags: Flags) -> Option<&'z [u16]> {
    if flags.unicode || flags.unicode_sets || pattern.capture_count > 0 {
        return None;
    }
    if pattern.disjunction.alternatives.len() != 1 {
        return None;
    }

    let terms = pattern.disjunction.alternatives[0].terms;
    if terms.len() < 3 {
        return None;
    }

    let first_term = terms.first()?;
    let last_term = terms.last()?;
    if first_term.quantifier.is_some() || last_term.quantifier.is_some() {
        return None;
    }
    if !matches!(first_term.atom, Atom::Assertion(AssertionKind::WordBoundary)) {
        return None;
    }
    if !matches!(last_term.atom, Atom::Assertion(AssertionKind::WordBoundary)) {
        return None;
    }

    let mut literal = ZoneVec::new_in(zone);
    for term in &terms[1..terms.len() - 1] {
        if term.quantifier.is_some() {
            return None;
        }
        let Atom::Literal(cp) = term.atom else {
            return None;
        };
        if is_unsafe_unicode_literal(cp, flags) {
            return None;
        }
        if flags.ignore_case && cp > 0x7F {
            return None;
        }
        append_code_point_wtf16(&mut literal, cp)?;
    }

    if literal.is_empty() {
        return None;
    }
    if !is_ascii_word_code_unit(*literal.first()?) || !is_ascii_word_code_unit(*literal.last()?) {
        return None;
    }

    Some(literal.into_slice())
}

fn is_unsafe_unicode_literal(cp: u32, flags: Flags) -> bool {
    (flags.unicode || flags.unicode_sets) && (0xD800..=0xDFFF).contains(&cp)
}
