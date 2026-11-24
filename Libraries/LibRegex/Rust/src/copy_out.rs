/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The copy of what a regex keeps of its compile, out of the zone of the compile and at its final size: the program
//! that the VM runs and the literals that let a search skip the VM.

use libregex_compiler::ast::UnicodeProperty;
use libregex_compiler::compiler;
use libregex_compiler::literal_hints::RequiredLiteral;
use libregex_compiler::zone_program::CodePointRange;
use libregex_compiler::zone_program::ZoneInstruction;
use libregex_compiler::zone_program::ZoneProgram;
use libregex_compiler::zone_program::ZoneSimpleMatch;

use crate::bytecode::CharRange;
use crate::bytecode::CharRanges;
use crate::bytecode::Instruction;
use crate::bytecode::NamedGroupEntry;
use crate::bytecode::Program;
use crate::bytecode::SimpleMatch;
use crate::bytecode::UnicodePropertyData;
use crate::vm;

pub(crate) fn program(program: &ZoneProgram<'_>) -> Program {
    let mut instructions = Vec::with_capacity(program.instructions.len());
    instructions.extend(program.instructions.iter().map(instruction));
    let mut named_groups = Vec::with_capacity(program.named_groups.len());
    named_groups.extend(program.named_groups.iter().map(|group| NamedGroupEntry {
        name: group.name.to_string(),
        index: group.index,
    }));
    Program {
        instructions,
        capture_count: program.capture_count,
        register_count: program.register_count,
        unicode: program.unicode,
        unicode_sets: program.unicode_sets,
        ignore_case: program.ignore_case,
        multiline: program.multiline,
        dot_all: program.dot_all,
        named_groups,
    }
}

fn instruction(instruction: &ZoneInstruction<'_>) -> Instruction {
    match *instruction {
        ZoneInstruction::Char(cp) => Instruction::Char(cp),
        ZoneInstruction::CharNoCase(lhs, rhs) => Instruction::CharNoCase(lhs, rhs),
        ZoneInstruction::AnyChar { dot_all } => Instruction::AnyChar { dot_all },
        ZoneInstruction::CharClass { ranges, negated } => Instruction::CharClass {
            ranges: char_ranges(ranges),
            negated,
        },
        ZoneInstruction::BuiltinClass(class) => Instruction::BuiltinClass(class),
        ZoneInstruction::UnicodeProperty(property) => Instruction::UnicodeProperty(unicode_property(property)),
        ZoneInstruction::Jump(target) => Instruction::Jump(target),
        ZoneInstruction::Split { prefer, other } => Instruction::Split { prefer, other },
        ZoneInstruction::Save(reg) => Instruction::Save(reg),
        ZoneInstruction::ClearRegister(reg) => Instruction::ClearRegister(reg),
        ZoneInstruction::AssertStart { multiline } => Instruction::AssertStart { multiline },
        ZoneInstruction::AssertEnd { multiline } => Instruction::AssertEnd { multiline },
        ZoneInstruction::AssertWordBoundary => Instruction::AssertWordBoundary,
        ZoneInstruction::AssertNonWordBoundary => Instruction::AssertNonWordBoundary,
        ZoneInstruction::Match => Instruction::Match,
        ZoneInstruction::Fail => Instruction::Fail,
        ZoneInstruction::Backref(index) => Instruction::Backref(index),
        ZoneInstruction::BackrefNamed(name) => Instruction::BackrefNamed(name.to_string()),
        ZoneInstruction::RepeatStart { counter_reg } => Instruction::RepeatStart { counter_reg },
        ZoneInstruction::RepeatCheck {
            counter_reg,
            min,
            max,
            body,
            greedy,
        } => Instruction::RepeatCheck {
            counter_reg,
            min,
            max,
            body,
            greedy,
        },
        ZoneInstruction::LookStart { positive, forward, end } => Instruction::LookStart { positive, forward, end },
        ZoneInstruction::LookEnd => Instruction::LookEnd,
        ZoneInstruction::PushModifiers {
            ignore_case,
            multiline,
            dot_all,
        } => Instruction::PushModifiers {
            ignore_case,
            multiline,
            dot_all,
        },
        ZoneInstruction::PopModifiers => Instruction::PopModifiers,
        ZoneInstruction::StringPropertyMatch { strings, property } => Instruction::StringPropertyMatch {
            strings: strings.into(),
            property: unicode_property(property),
        },
        ZoneInstruction::ProgressCheck { reg, clear_captures } => Instruction::ProgressCheck {
            reg,
            clear_captures: clear_captures.to_vec(),
        },
        ZoneInstruction::GreedyLoop { matcher, min, max } => Instruction::GreedyLoop {
            matcher: simple_match(&matcher),
            min,
            max,
        },
        ZoneInstruction::LazyLoop { matcher, min, max } => Instruction::LazyLoop {
            matcher: simple_match(&matcher),
            min,
            max,
        },
    }
}

fn simple_match(matcher: &ZoneSimpleMatch<'_>) -> SimpleMatch {
    match *matcher {
        ZoneSimpleMatch::AnyChar { dot_all } => SimpleMatch::AnyChar { dot_all },
        ZoneSimpleMatch::Char(cp) => SimpleMatch::Char(cp),
        ZoneSimpleMatch::CharNoCase(lhs, rhs) => SimpleMatch::CharNoCase(lhs, rhs),
        ZoneSimpleMatch::CharClass { ranges, negated } => SimpleMatch::CharClass {
            ranges: char_ranges(ranges),
            negated,
        },
        ZoneSimpleMatch::BuiltinClass(class) => SimpleMatch::BuiltinClass(class),
        ZoneSimpleMatch::UnicodeProperty(property) => SimpleMatch::UnicodeProperty(unicode_property(property)),
        ZoneSimpleMatch::Union(lhs, rhs) => {
            SimpleMatch::Union(Box::new(simple_match(lhs)), Box::new(simple_match(rhs)))
        }
    }
}

fn char_ranges(ranges: &[CodePointRange]) -> CharRanges {
    let mut copy = Vec::with_capacity(ranges.len());
    copy.extend(ranges.iter().map(|range| CharRange {
        start: range.start,
        end: range.end,
    }));
    CharRanges::new(copy)
}

/// The data of a Unicode property match, with the property resolved to an ICU enum ID. This allows O(1) trie lookups
/// at match time instead of string-based resolution.
fn unicode_property(property: &UnicodeProperty<'_>) -> Box<UnicodePropertyData> {
    Box::new(UnicodePropertyData {
        negated: property.negated,
        name: property.name.to_string(),
        value: property.value.map(str::to_string),
        resolved: compiler::resolve_property(property.name, property.value),
    })
}

pub(crate) fn required_literal_hint(hint: &RequiredLiteral<'_>) -> vm::RequiredLiteralHint {
    vm::RequiredLiteralHint {
        literal: hint.literal.to_vec(),
        ascii_case_insensitive: hint.ascii_case_insensitive,
    }
}

pub(crate) fn literal_alternatives(alternatives: &[&[u16]]) -> Vec<Vec<u16>> {
    let mut copy = Vec::with_capacity(alternatives.len());
    copy.extend(alternatives.iter().map(|alternative| alternative.to_vec()));
    copy
}
