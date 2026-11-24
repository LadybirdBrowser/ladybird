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
use crate::bytecode::RepeatCheck;
use crate::bytecode::SimpleLoop;
use crate::bytecode::SimpleMatch;
use crate::bytecode::StringProperty;
use crate::bytecode::UnicodePropertyData;
use crate::vm;

pub(crate) fn program(zone_program: &ZoneProgram<'_>) -> Program {
    let mut named_groups = Vec::with_capacity(zone_program.named_groups.len());
    named_groups.extend(zone_program.named_groups.iter().map(|group| NamedGroupEntry {
        name: group.name.to_string(),
        index: group.index,
    }));
    let mut program = Program {
        capture_count: zone_program.capture_count,
        register_count: zone_program.register_count,
        unicode: zone_program.unicode,
        unicode_sets: zone_program.unicode_sets,
        ignore_case: zone_program.ignore_case,
        multiline: zone_program.multiline,
        dot_all: zone_program.dot_all,
        named_groups,
        ..Program::new()
    };
    let mut instructions = Vec::with_capacity(zone_program.instructions.len());
    instructions.extend(
        zone_program
            .instructions
            .iter()
            .map(|zone_instruction| instruction(zone_instruction, &mut program)),
    );
    program.instructions = instructions;
    program.compute_same_matcher_suffix_mins();
    program.literals.shrink_to_fit();
    program.classes.shrink_to_fit();
    program.unicode_properties.shrink_to_fit();
    program.backref_names.shrink_to_fit();
    program.repeat_checks.shrink_to_fit();
    program.string_properties.shrink_to_fit();
    program.register_lists.shrink_to_fit();
    program.loops.shrink_to_fit();
    program
}

/// Appends `item` to `table` and returns its index.
fn push<T>(table: &mut Vec<T>, item: T) -> u32 {
    let index = table.len() as u32;
    table.push(item);
    index
}

/// The kept instruction of `instruction`, with the data that does not fit in an instruction appended to the tables
/// of `program`.
fn instruction(instruction: &ZoneInstruction<'_>, program: &mut Program) -> Instruction {
    match *instruction {
        ZoneInstruction::Char(cp) => Instruction::Char(cp),
        ZoneInstruction::CharNoCase(lhs, rhs) => Instruction::CharNoCase(lhs, rhs),
        ZoneInstruction::Literal {
            code_units,
            ignore_case,
        } => {
            let rest_start = program.literals.len() as u32;
            program.literals.extend_from_slice(&code_units[1..]);
            Instruction::Literal {
                first: code_units[0],
                ignore_case,
                rest_start,
                rest_len: code_units.len() as u32 - 1,
            }
        }
        ZoneInstruction::AnyChar { dot_all } => Instruction::AnyChar { dot_all },
        ZoneInstruction::CharClass { ranges, negated } => Instruction::CharClass {
            class: push(&mut program.classes, char_ranges(ranges)),
            negated,
        },
        ZoneInstruction::BuiltinClass(class) => Instruction::BuiltinClass(class),
        ZoneInstruction::UnicodeProperty(property) => {
            Instruction::UnicodeProperty(push(&mut program.unicode_properties, unicode_property(property)))
        }
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
        ZoneInstruction::BackrefNamed(name) => {
            Instruction::BackrefNamed(push(&mut program.backref_names, name.to_string()))
        }
        ZoneInstruction::RepeatStart { counter_reg } => Instruction::RepeatStart { counter_reg },
        ZoneInstruction::RepeatCheck {
            counter_reg,
            min,
            max,
            body,
            greedy,
        } => Instruction::RepeatCheck {
            check: push(
                &mut program.repeat_checks,
                RepeatCheck {
                    counter_reg,
                    min,
                    max,
                    greedy,
                },
            ),
            body,
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
        ZoneInstruction::StringPropertyMatch { strings, property } => Instruction::StringPropertyMatch(push(
            &mut program.string_properties,
            StringProperty {
                strings: strings.into(),
                property: unicode_property(property),
            },
        )),
        ZoneInstruction::ProgressCheck { reg, clear_captures } => {
            let list = program.register_lists.len() as u32;
            program.register_lists.push(clear_captures.len() as u32);
            program.register_lists.extend_from_slice(clear_captures);
            Instruction::ProgressCheck {
                reg,
                clear_captures: list,
            }
        }
        ZoneInstruction::GreedyLoop { matcher, min, max } => Instruction::GreedyLoop {
            index: push(
                &mut program.loops,
                SimpleLoop {
                    matcher: simple_match(&matcher),
                    min,
                    max,
                    same_matcher_suffix_min: None,
                },
            ),
            same_matcher_suffix: false,
        },
        ZoneInstruction::LazyLoop { matcher, min, max } => Instruction::LazyLoop(push(
            &mut program.loops,
            SimpleLoop {
                matcher: simple_match(&matcher),
                min,
                max,
                same_matcher_suffix_min: None,
            },
        )),
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
        ZoneSimpleMatch::UnicodeProperty(property) => {
            SimpleMatch::UnicodeProperty(Box::new(unicode_property(property)))
        }
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
fn unicode_property(property: &UnicodeProperty<'_>) -> UnicodePropertyData {
    UnicodePropertyData {
        negated: property.negated,
        name: property.name.to_string(),
        value: property.value.map(str::to_string),
        resolved: compiler::resolve_property(property.name, property.value),
    }
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
