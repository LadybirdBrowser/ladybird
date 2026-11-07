/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::ast::Atom;
use crate::ast::CharacterClassBody;
use crate::ast::Disjunction;
use crate::bytecode::BuiltinCharacterClass;
use crate::bytecode::CharRange;
use crate::bytecode::Instruction;
use crate::bytecode::Program;
use crate::bytecode::SimpleMatch;

#[derive(Debug, Clone, Default)]
pub(crate) struct Optimization {
    pub leading_match: Vec<Option<usize>>,
    pub atomic_loop: Vec<bool>,
    pub literal_runs: Vec<Option<Box<[u16]>>>,
}

pub(crate) fn normalize_character_classes(program: &mut Program) {
    fn normalize(ranges: &mut Vec<CharRange>) {
        ranges.sort_unstable_by_key(|range| range.start);
        ranges.dedup_by(|next, previous| {
            if next.start <= previous.end.saturating_add(1) {
                previous.end = previous.end.max(next.end);
                true
            } else {
                false
            }
        });
    }
    fn normalize_matcher(matcher: &mut SimpleMatch) {
        match matcher {
            SimpleMatch::CharClass { ranges, .. } => normalize(ranges),
            SimpleMatch::Union(left, right) => {
                normalize_matcher(left);
                normalize_matcher(right);
            }
            _ => {}
        }
    }
    for instruction in &mut program.instructions {
        match instruction {
            Instruction::CharClass { ranges, .. } => normalize(ranges),
            Instruction::GreedyLoop { matcher, .. } | Instruction::LazyLoop { matcher, .. } => {
                normalize_matcher(matcher)
            }
            _ => {}
        }
    }
}

pub(crate) fn minimum_length(disjunction: &Disjunction) -> usize {
    disjunction
        .alternatives
        .iter()
        .map(|alternative| {
            alternative.terms.iter().fold(0usize, |length, term| {
                let atom_length = match &term.atom {
                    Atom::Literal(cp) => {
                        if *cp > 0xFFFF {
                            2
                        } else {
                            1
                        }
                    }
                    Atom::Dot | Atom::BuiltinCharacterClass(_) => 1,
                    Atom::CharacterClass(class) if matches!(class.body, CharacterClassBody::Ranges(_)) => 1,
                    Atom::Group(group) => minimum_length(&group.body),
                    Atom::NonCapturingGroup(group) => minimum_length(&group.body),
                    Atom::ModifierGroup(group) => minimum_length(&group.body),
                    // Backreferences can refer to an unmatched capture; /v sets and
                    // properties can contain strings, including the empty string.
                    _ => 0,
                };
                length
                    .saturating_add(atom_length.saturating_mul(term.quantifier.as_ref().map_or(1, |q| q.min as usize)))
            })
        })
        .min()
        .unwrap_or(0)
}

pub(crate) fn analyze(program: &Program) -> Optimization {
    let instructions = &program.instructions;
    let len = instructions.len();
    let mut result = Optimization {
        leading_match: vec![None; len],
        atomic_loop: vec![false; len],
        literal_runs: vec![None; len],
    };
    // Only traverse forward edges and bookkeeping with no effect on matching.
    for pc in (0..len).rev() {
        result.leading_match[pc] = match &instructions[pc] {
            Instruction::Char(_)
            | Instruction::CharNoCase(..)
            | Instruction::AnyChar { .. }
            | Instruction::CharClass { .. }
            | Instruction::BuiltinClass(_) => Some(pc),
            Instruction::GreedyLoop { min, .. } | Instruction::LazyLoop { min, .. } if *min > 0 => Some(pc),
            Instruction::Save(_) | Instruction::ClearRegister(_) | Instruction::Nop => {
                result.leading_match.get(pc + 1).copied().flatten()
            }
            Instruction::Jump(target) if *target as usize > pc => {
                result.leading_match.get(*target as usize).copied().flatten()
            }
            _ => None,
        };
    }

    let mut ignore_case = program.ignore_case;
    let mut modifiers = Vec::new();
    let mut pc = 0;
    while pc < len {
        match &instructions[pc] {
            Instruction::PushModifiers { ignore_case: value, .. } => {
                modifiers.push(ignore_case);
                ignore_case = value.unwrap_or(ignore_case);
            }
            Instruction::PopModifiers => ignore_case = modifiers.pop().unwrap_or(program.ignore_case),
            Instruction::GreedyLoop { matcher, .. } => {
                let next = result.leading_match.get(pc + 1).copied().flatten();
                if let Some(next) = next
                    && let (Some(left), Some(right)) = (
                        matcher_ranges(matcher, ignore_case, program.unicode),
                        instruction_ranges(&instructions[next], ignore_case, program.unicode),
                    )
                {
                    result.atomic_loop[pc] = !ranges_overlap(left, right);
                }
                // A trailing simple loop has no internal captures or choices.
                let mut next = pc + 1;
                while let Some(Instruction::Save(_) | Instruction::ClearRegister(_) | Instruction::Nop) =
                    instructions.get(next)
                {
                    next += 1;
                }
                if matches!(instructions.get(next), Some(Instruction::Match)) {
                    result.atomic_loop[pc] = true;
                }
            }
            Instruction::Char(_) if !ignore_case => {
                let start = pc;
                let mut literal = Vec::new();
                while let Some(Instruction::Char(cp)) = instructions.get(pc) {
                    // BMP non-surrogates have the same boundaries in both modes.
                    if *cp > 0xFFFF || (0xD800..=0xDFFF).contains(cp) {
                        break;
                    }
                    literal.push(*cp as u16);
                    pc += 1;
                }
                if literal.len() > 1 {
                    result.literal_runs[start] = Some(literal.into_boxed_slice());
                }
                if pc > start {
                    continue;
                }
            }
            _ => {}
        }
        pc += 1;
    }
    result
}

// Union of the possible first characters without changing alternative order.
pub(crate) fn first_filter(program: &Program, start: usize) -> Option<SimpleMatch> {
    let mut pending = vec![start];
    let mut ranges = Vec::new();
    let mut budget = 128usize;
    while let Some(mut pc) = pending.pop() {
        loop {
            budget = budget.checked_sub(1)?;
            let instruction = program.instructions.get(pc)?;
            match instruction {
                Instruction::Save(_) | Instruction::ClearRegister(_) | Instruction::Nop => pc += 1,
                Instruction::Jump(target) if *target as usize > pc => pc = *target as usize,
                Instruction::Split { prefer, other } if *prefer as usize > pc && *other as usize > pc => {
                    pending.push(*other as usize);
                    pc = *prefer as usize;
                }
                Instruction::GreedyLoop { min: 0, .. } | Instruction::LazyLoop { min: 0, .. } => return None,
                _ => {
                    ranges.extend(instruction_ranges(instruction, false, program.unicode)?);
                    break;
                }
            }
        }
    }
    Some(SimpleMatch::CharClass {
        ranges: normalize_ranges(ranges)
            .into_iter()
            .map(|(start, end)| CharRange { start, end })
            .collect(),
        negated: false,
    })
}

fn normalize_ranges(mut ranges: Ranges) -> Ranges {
    ranges.sort_unstable();
    let mut merged: Ranges = Vec::new();
    for (start, end) in ranges {
        if let Some(last) = merged.last_mut()
            && start <= last.1.saturating_add(1)
        {
            last.1 = last.1.max(end);
        } else {
            merged.push((start, end));
        }
    }
    merged
}

fn ranges_overlap(left: Ranges, right: Ranges) -> bool {
    let left = normalize_ranges(left);
    let right = normalize_ranges(right);
    let (mut a, mut b) = (0, 0);
    while a < left.len() && b < right.len() {
        if left[a].1 < right[b].0 {
            a += 1;
        } else if right[b].1 < left[a].0 {
            b += 1;
        } else {
            return true;
        }
    }
    false
}

type Ranges = Vec<(u32, u32)>;

fn instruction_ranges(instruction: &Instruction, ignore_case: bool, unicode: bool) -> Option<Ranges> {
    match instruction {
        Instruction::Char(cp) => expand_case_ranges(vec![(*cp, *cp)], ignore_case, unicode),
        Instruction::CharNoCase(cp, _) => expand_case_ranges(vec![(*cp, *cp)], true, unicode),
        Instruction::AnyChar { dot_all } => {
            matcher_ranges(&SimpleMatch::AnyChar { dot_all: *dot_all }, ignore_case, unicode)
        }
        Instruction::CharClass { ranges, negated } => Some(class_ranges(
            expand_case_ranges(ranges.iter().map(|r| (r.start, r.end)).collect(), ignore_case, unicode)?,
            *negated,
        )),
        Instruction::BuiltinClass(class) => builtin_ranges(*class, ignore_case, unicode),
        Instruction::GreedyLoop { matcher, .. } | Instruction::LazyLoop { matcher, .. } => {
            matcher_ranges(matcher, ignore_case, unicode)
        }
        _ => None,
    }
}

fn matcher_ranges(matcher: &SimpleMatch, ignore_case: bool, unicode: bool) -> Option<Ranges> {
    match matcher {
        SimpleMatch::Char(cp) => expand_case_ranges(vec![(*cp, *cp)], ignore_case, unicode),
        SimpleMatch::CharNoCase(cp, _) => expand_case_ranges(vec![(*cp, *cp)], true, unicode),
        SimpleMatch::AnyChar { dot_all: true } => Some(vec![(0, 0x10FFFF)]),
        SimpleMatch::AnyChar { dot_all: false } => Some(class_ranges(vec![(10, 10), (13, 13), (0x2028, 0x2029)], true)),
        SimpleMatch::CharClass { ranges, negated } => Some(class_ranges(
            expand_case_ranges(ranges.iter().map(|r| (r.start, r.end)).collect(), ignore_case, unicode)?,
            *negated,
        )),
        SimpleMatch::BuiltinClass(class) => builtin_ranges(*class, ignore_case, unicode),
        SimpleMatch::Union(left, right) => {
            let mut ranges = matcher_ranges(left, ignore_case, unicode)?;
            ranges.extend(matcher_ranges(right, ignore_case, unicode)?);
            Some(ranges)
        }
        // Unicode properties require more than raw intervals.
        _ => None,
    }
}

fn builtin_ranges(class: BuiltinCharacterClass, ignore_case: bool, unicode: bool) -> Option<Ranges> {
    use BuiltinCharacterClass::*;
    let ranges = match class {
        Digit | NonDigit => vec![(0x30, 0x39)],
        Word | NonWord => vec![(0x30, 0x39), (0x41, 0x5A), (0x5F, 0x5F), (0x61, 0x7A)],
        Whitespace | NonWhitespace => vec![
            (9, 13),
            (0x20, 0x20),
            (0xA0, 0xA0),
            (0x1680, 0x1680),
            (0x2000, 0x200A),
            (0x2028, 0x2029),
            (0x202F, 0x202F),
            (0x205F, 0x205F),
            (0x3000, 0x3000),
            (0xFEFF, 0xFEFF),
        ],
    };
    let ranges = if ignore_case && unicode && matches!(class, Word | NonWord) {
        expand_case_ranges(ranges, true, true)?
    } else {
        ranges
    };
    Some(class_ranges(
        ranges,
        matches!(class, NonDigit | NonWord | NonWhitespace),
    ))
}

fn expand_case_ranges(mut ranges: Ranges, ignore_case: bool, unicode: bool) -> Option<Ranges> {
    if !ignore_case {
        return Some(ranges);
    }
    if ranges.iter().any(|range| range.1 >= 128) {
        return None;
    }
    let original = normalize_ranges(ranges.clone());
    for (start, end) in original {
        for cp in start..=end {
            if !(cp as u8).is_ascii_alphabetic() {
                continue;
            }
            if unicode {
                let mut closure = [0u32; 16];
                let count = libunicode_rust::character_types::get_case_closure(cp, &mut closure);
                if count > closure.len() {
                    return None;
                }
                ranges.extend(closure[..count].iter().map(|cp| (*cp, *cp)));
            } else {
                let other = cp ^ 0x20;
                ranges.push((other, other));
            }
        }
    }
    Some(ranges)
}

fn class_ranges(mut ranges: Ranges, negated: bool) -> Ranges {
    if !negated {
        return ranges;
    }
    ranges.sort_unstable();
    let mut complement = Vec::new();
    let mut start = 0;
    for (low, high) in ranges {
        if start < low {
            complement.push((start, low - 1));
        }
        start = start.max(high.saturating_add(1));
    }
    if start <= 0x10FFFF {
        complement.push((start, 0x10FFFF));
    }
    complement
}
