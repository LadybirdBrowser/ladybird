/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Sparse conditional constant propagation and integer value analysis.

use super::optimize::{
    eliminate_unreachable_blocks, rewrite_terminator, rewrite_values,
};
use super::pass::{AnalysisManager};
use super::{
    BinaryOperation, BlockId, CheckedIntegerOperation, ComparisonDomain, ComparisonRelation, Constant, Edge, Effects,
    Function, IntegerBinaryOperation, IntegerComparisonOperation, Intrinsic, Operation, ShiftOperation,
    Terminator, ValueDefinition, ValueId, ValueOperation,
};
use crate::types::Type;
use std::collections::{HashMap, HashSet};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum LatticeValue {
    Unknown,
    Integer(IntegerFacts),
    Overdefined,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct IntegerFacts {
    bits: u8,
    signed: bool,
    min: i128,
    max: i128,
    known_zero: u64,
    known_one: u64,
}

impl IntegerFacts {
    fn full(ty: &Type) -> Option<Self> {
        let (bits, signed) = integer_type(ty)?;
        let (min, max) = type_bounds(bits, signed);
        Some(Self {
            bits,
            signed,
            min,
            max,
            known_zero: 0,
            known_one: 0,
        })
    }

    fn constant(ty: &Type, value: i64) -> Option<Self> {
        let mut facts = Self::full(ty)?;
        let raw = truncate(value as u64, facts.bits);
        let value = semantic_value(raw, facts.bits, facts.signed);
        facts.min = value;
        facts.max = value;
        facts.known_one = raw;
        facts.known_zero = facts.mask() & !raw;
        Some(facts)
    }

    fn mask(self) -> u64 {
        bit_mask(self.bits)
    }

    fn exact_raw(self) -> Option<u64> {
        if self.min == self.max {
            return Some(truncate(self.min as u64, self.bits));
        }
        ((self.known_zero | self.known_one) == self.mask()).then_some(self.known_one)
    }

    fn exact_integer(self) -> Option<i64> {
        let raw = self.exact_raw()?;
        Some(if self.signed {
            semantic_value(raw, self.bits, true) as i64
        } else {
            raw as i64
        })
    }

    fn join(self, other: Self) -> Self {
        debug_assert_eq!((self.bits, self.signed), (other.bits, other.signed));
        Self {
            bits: self.bits,
            signed: self.signed,
            min: self.min.min(other.min),
            max: self.max.max(other.max),
            known_zero: self.known_zero & other.known_zero,
            known_one: self.known_one & other.known_one,
        }
    }

    fn bool(value: bool) -> Self {
        Self::constant(&Type::Bool, i64::from(value)).unwrap()
    }
}

pub(super) fn run(function: &mut Function, analyses: &mut AnalysisManager) -> bool {
    let loop_blocks = analyses
        .loops(function)
        .iter()
        .flat_map(|natural_loop| natural_loop.blocks.iter().copied())
        .collect::<HashSet<_>>();
    let mut values = function
        .values
        .iter()
        .map(|value| initial_value(&value.ty, &value.definition))
        .collect::<Vec<_>>();
    let mut executable_blocks = vec![false; function.blocks.len()];
    let mut executable_edges = HashSet::new();
    executable_blocks[function.entry.0] = true;

    loop {
        let mut changed = false;

        for (block_index, executable) in executable_blocks.iter().copied().enumerate() {
            if !executable {
                continue;
            }
            let block = BlockId(block_index);
            for (parameter_index, parameter) in function.blocks[block_index].parameters.iter().enumerate() {
                let mut incoming = LatticeValue::Unknown;
                for predecessor_index in 0..function.blocks.len() {
                    for (edge_index, edge) in selected_edges(function, BlockId(predecessor_index), &values) {
                        if edge.block == block && executable_edges.contains(&(predecessor_index, edge_index)) {
                            incoming = join_lattice(incoming, values[edge.arguments[parameter_index].0]);
                        }
                    }
                }
                changed |= merge_value(&mut values[parameter.0], incoming, loop_blocks.contains(&block));
            }

            for instruction_id in &function.blocks[block_index].instructions {
                let instruction = &function.instructions[instruction_id.0];
                let results = evaluate_operation(
                    function,
                    &instruction.operation,
                    &instruction.inputs,
                    &instruction.results,
                    instruction.effects,
                    &values,
                );
                for (result, lattice) in instruction.results.iter().zip(results) {
                    changed |= merge_value(&mut values[result.0], lattice, loop_blocks.contains(&block));
                }
            }

            if let Terminator::CheckedOperation {
                operation,
                inputs,
                results,
                effects,
                ..
            } = function.blocks[block_index].terminator.as_ref().unwrap()
            {
                let result_values = evaluate_operation(function, operation, inputs, results, *effects, &values);
                for (result, lattice) in results.iter().zip(result_values) {
                    changed |= merge_value(&mut values[result.0], lattice, loop_blocks.contains(&block));
                }
            }
        }

        let mut predecessor_index = 0;
        while predecessor_index < function.blocks.len() {
            if !executable_blocks[predecessor_index] {
                predecessor_index += 1;
                continue;
            }
            for (edge_index, edge) in selected_edges(function, BlockId(predecessor_index), &values) {
                if executable_edges.insert((predecessor_index, edge_index)) {
                    changed = true;
                }
                if !executable_blocks[edge.block.0] {
                    executable_blocks[edge.block.0] = true;
                    changed = true;
                }
            }
            predecessor_index += 1;
        }

        if !changed {
            break;
        }
    }

    rewrite(function, &values, &executable_blocks)
}

fn initial_value(ty: &Type, definition: &ValueDefinition) -> LatticeValue {
    match definition {
        ValueDefinition::Constant(Constant::Integer(value)) => IntegerFacts::constant(ty, *value)
            .map(LatticeValue::Integer)
            .unwrap_or(LatticeValue::Overdefined),
        ValueDefinition::FunctionParameter(_) => IntegerFacts::full(ty)
            .map(LatticeValue::Integer)
            .unwrap_or(LatticeValue::Overdefined),
        ValueDefinition::Constant(_) => LatticeValue::Overdefined,
        ValueDefinition::Dead
        | ValueDefinition::BlockParameter { .. }
        | ValueDefinition::InstructionResult { .. }
        | ValueDefinition::TerminatorResult { .. } => LatticeValue::Unknown,
    }
}

fn merge_value(destination: &mut LatticeValue, incoming: LatticeValue, widen: bool) -> bool {
    let joined = if widen {
        join_lattice_with_widening(*destination, incoming)
    } else {
        join_lattice(*destination, incoming)
    };
    if joined == *destination {
        return false;
    }
    *destination = joined;
    true
}

fn join_lattice_with_widening(lhs: LatticeValue, rhs: LatticeValue) -> LatticeValue {
    let joined = join_lattice(lhs, rhs);
    let (LatticeValue::Integer(lhs), LatticeValue::Integer(mut joined)) = (lhs, joined) else {
        return joined;
    };
    let (min, max) = type_bounds(joined.bits, joined.signed);
    if joined.min < lhs.min {
        joined.min = min;
    }
    if joined.max > lhs.max {
        joined.max = max;
    }
    LatticeValue::Integer(joined)
}

fn join_lattice(lhs: LatticeValue, rhs: LatticeValue) -> LatticeValue {
    match (lhs, rhs) {
        (LatticeValue::Unknown, value) | (value, LatticeValue::Unknown) => value,
        (LatticeValue::Overdefined, _) | (_, LatticeValue::Overdefined) => LatticeValue::Overdefined,
        (LatticeValue::Integer(lhs), LatticeValue::Integer(rhs))
            if (lhs.bits, lhs.signed) == (rhs.bits, rhs.signed) =>
        {
            LatticeValue::Integer(lhs.join(rhs))
        }
        (LatticeValue::Integer(_), LatticeValue::Integer(_)) => LatticeValue::Overdefined,
    }
}

fn evaluate_operation(
    function: &Function,
    operation: &Operation,
    inputs: &[ValueId],
    results: &[ValueId],
    effects: Effects,
    values: &[LatticeValue],
) -> Vec<LatticeValue> {
    if effects != Effects::PURE {
        return results
            .iter()
            .map(|result| integer_or_overdefined(function, *result))
            .collect();
    }
    if inputs.iter().any(|input| values[input.0] == LatticeValue::Unknown) {
        return vec![LatticeValue::Unknown; results.len()];
    }
    let Some(result) = results.first() else {
        return Vec::new();
    };
    if results.len() != 1 {
        return results
            .iter()
            .map(|result| integer_or_overdefined(function, *result))
            .collect();
    }
    let Some(result_type) = IntegerFacts::full(&function.values[result.0].ty) else {
        return vec![LatticeValue::Overdefined];
    };
    let input_facts = inputs
        .iter()
        .map(|input| match values[input.0] {
            LatticeValue::Integer(facts) => Some(facts),
            LatticeValue::Unknown | LatticeValue::Overdefined => None,
        })
        .collect::<Option<Vec<_>>>();
    let Some(input_facts) = input_facts else {
        return vec![LatticeValue::Integer(result_type)];
    };
    let facts = match operation {
        Operation::Intrinsic(Intrinsic::Value(operation)) => evaluate_value_operation(*operation, &input_facts, result_type),
        Operation::Intrinsic(Intrinsic::IntegerBinary(operation)) => evaluate_integer_binary(*operation, &input_facts, result_type),
        Operation::Intrinsic(Intrinsic::IntegerComparison(operation)) => evaluate_integer_comparison(*operation, &input_facts, result_type),
        _ => result_type,
    };
    vec![LatticeValue::Integer(facts)]
}

fn integer_or_overdefined(function: &Function, value: ValueId) -> LatticeValue {
    IntegerFacts::full(&function.values[value.0].ty)
        .map(LatticeValue::Integer)
        .unwrap_or(LatticeValue::Overdefined)
}

fn evaluate_value_operation(
    operation: ValueOperation,
    inputs: &[IntegerFacts],
    result: IntegerFacts,
) -> IntegerFacts {
    match (operation, inputs) {
        (
            ValueOperation::ToInt32
            | ValueOperation::ToUint32
            | ValueOperation::TruncateUint8
            | ValueOperation::TruncateUint16
            | ValueOperation::AssumeInt32
            | ValueOperation::AssumeUint32,
            [input],
        ) => exact_unary(*input, result, |value| value),
        (ValueOperation::LogicalNot, [input]) => {
            exact_unary(*input, result, |value| u64::from(value == 0))
        }
        _ => result,
    }
}

fn evaluate_integer_comparison(
    operation: IntegerComparisonOperation,
    inputs: &[IntegerFacts],
    result: IntegerFacts,
) -> IntegerFacts {
    let [lhs, rhs] = inputs else {
        return result;
    };
    match operation {
        IntegerComparisonOperation::Equal => equality(true, *lhs, *rhs),
        IntegerComparisonOperation::NotEqual => equality(false, *lhs, *rhs),
        IntegerComparisonOperation::Relational { relation: comparison, domain }
            if domain != ComparisonDomain::UnsignedInteger =>
        {
            let range_relation = match comparison {
                ComparisonRelation::Less => Relation::Less,
                ComparisonRelation::LessOrEqual => Relation::LessEqual,
                ComparisonRelation::Greater => Relation::Greater,
                ComparisonRelation::GreaterOrEqual => Relation::GreaterEqual,
            };
            relation(*lhs, *rhs, range_relation)
        }
        _ => result,
    }
}

fn evaluate_integer_binary(
    operation: IntegerBinaryOperation,
    inputs: &[IntegerFacts],
    result: IntegerFacts,
) -> IntegerFacts {
    let [lhs, rhs] = inputs else {
        return result;
    };
    match operation {
        IntegerBinaryOperation::Binary(BinaryOperation::Add) => arithmetic_range(*lhs, *rhs, result, Arithmetic::Add),
        IntegerBinaryOperation::Binary(BinaryOperation::Subtract) => arithmetic_range(*lhs, *rhs, result, Arithmetic::Sub),
        IntegerBinaryOperation::Binary(BinaryOperation::Multiply) => arithmetic_range(*lhs, *rhs, result, Arithmetic::Mul),
        IntegerBinaryOperation::Binary(BinaryOperation::And) => bitwise(*lhs, *rhs, result, Bitwise::And),
        IntegerBinaryOperation::Binary(BinaryOperation::Or) => bitwise(*lhs, *rhs, result, Bitwise::Or),
        IntegerBinaryOperation::Binary(BinaryOperation::Xor) => bitwise(*lhs, *rhs, result, Bitwise::Xor),
        IntegerBinaryOperation::Shift(ShiftOperation::Left) => shift(*lhs, *rhs, result, Shift::Left),
        IntegerBinaryOperation::Shift(ShiftOperation::RightLogical) => shift(*lhs, *rhs, result, Shift::LogicalRight),
        IntegerBinaryOperation::Shift(ShiftOperation::RightArithmetic) => shift(*lhs, *rhs, result, Shift::ArithmeticRight),
    }
}

fn exact_unary(input: IntegerFacts, result: IntegerFacts, operation: impl FnOnce(u64) -> u64) -> IntegerFacts {
    let Some(value) = input.exact_raw() else {
        return result;
    };
    constant_with_layout(result, operation(value))
}

#[derive(Clone, Copy)]
enum Arithmetic {
    Add,
    Sub,
    Mul,
}

fn arithmetic_range(lhs: IntegerFacts, rhs: IntegerFacts, result: IntegerFacts, operation: Arithmetic) -> IntegerFacts {
    if let (Some(lhs), Some(rhs)) = (lhs.exact_raw(), rhs.exact_raw()) {
        let value = match operation {
            Arithmetic::Add => lhs.wrapping_add(rhs),
            Arithmetic::Sub => lhs.wrapping_sub(rhs),
            Arithmetic::Mul => lhs.wrapping_mul(rhs),
        };
        return constant_with_layout(result, value);
    }
    let Some(candidates) = arithmetic_candidates(lhs, rhs, operation) else {
        return result;
    };
    let min = *candidates.iter().min().unwrap();
    let max = *candidates.iter().max().unwrap();
    let (type_min, type_max) = type_bounds(result.bits, result.signed);
    if min < type_min || max > type_max {
        result
    } else {
        IntegerFacts { min, max, ..result }
    }
}

#[derive(Clone, Copy)]
enum Bitwise {
    And,
    Or,
    Xor,
}

fn bitwise(lhs: IntegerFacts, rhs: IntegerFacts, result: IntegerFacts, operation: Bitwise) -> IntegerFacts {
    if let (Some(lhs), Some(rhs)) = (lhs.exact_raw(), rhs.exact_raw()) {
        return constant_with_layout(
            result,
            match operation {
                Bitwise::And => lhs & rhs,
                Bitwise::Or => lhs | rhs,
                Bitwise::Xor => lhs ^ rhs,
            },
        );
    }
    let (known_zero, known_one) = match operation {
        Bitwise::And => (lhs.known_zero | rhs.known_zero, lhs.known_one & rhs.known_one),
        Bitwise::Or => (lhs.known_zero & rhs.known_zero, lhs.known_one | rhs.known_one),
        Bitwise::Xor => (
            (lhs.known_zero & rhs.known_zero) | (lhs.known_one & rhs.known_one),
            (lhs.known_zero & rhs.known_one) | (lhs.known_one & rhs.known_zero),
        ),
    };
    IntegerFacts {
        known_zero: known_zero & result.mask(),
        known_one: known_one & result.mask(),
        ..result
    }
}

#[derive(Clone, Copy)]
enum Shift {
    Left,
    LogicalRight,
    ArithmeticRight,
}

fn shift(lhs: IntegerFacts, rhs: IntegerFacts, result: IntegerFacts, operation: Shift) -> IntegerFacts {
    let bits = lhs.bits;
    let (Some(lhs), Some(rhs)) = (lhs.exact_raw(), rhs.exact_raw()) else {
        return result;
    };
    // NB: Narrower operands are shifted in a 32-bit register and truncated afterwards, which is
    //     what the backends emit for them, so only a 64-bit operand counts the shift modulo 64.
    let value = if bits > 32 {
        let shift = (rhs & 63) as u32;
        match operation {
            Shift::Left => lhs.wrapping_shl(shift),
            Shift::LogicalRight => lhs >> shift,
            Shift::ArithmeticRight => ((lhs as i64) >> shift) as u64,
        }
    } else {
        let shift = (rhs & 31) as u32;
        match operation {
            Shift::Left => (lhs as u32).wrapping_shl(shift) as u64,
            Shift::LogicalRight => ((lhs as u32) >> shift) as u64,
            Shift::ArithmeticRight => ((lhs as u32 as i32) >> shift) as u32 as u64,
        }
    };
    constant_with_layout(result, value)
}

fn equality(equal: bool, lhs: IntegerFacts, rhs: IntegerFacts) -> IntegerFacts {
    let is_equal = match (lhs.exact_raw(), rhs.exact_raw()) {
        (Some(lhs), Some(rhs)) => Some(lhs == rhs),
        _ if lhs.max < rhs.min || rhs.max < lhs.min => Some(false),
        _ if ((lhs.known_one & rhs.known_zero) | (rhs.known_one & lhs.known_zero)) != 0 => Some(false),
        _ => None,
    };
    is_equal
        .map(|is_equal| IntegerFacts::bool(is_equal == equal))
        .unwrap_or_else(|| IntegerFacts::full(&Type::Bool).unwrap())
}

#[derive(Clone, Copy)]
enum Relation {
    Less,
    LessEqual,
    Greater,
    GreaterEqual,
}

fn relation(lhs: IntegerFacts, rhs: IntegerFacts, relation: Relation) -> IntegerFacts {
    let value = match relation {
        Relation::Less if lhs.max < rhs.min => Some(true),
        Relation::Less if lhs.min >= rhs.max => Some(false),
        Relation::LessEqual if lhs.max <= rhs.min => Some(true),
        Relation::LessEqual if lhs.min > rhs.max => Some(false),
        Relation::Greater if lhs.min > rhs.max => Some(true),
        Relation::Greater if lhs.max <= rhs.min => Some(false),
        Relation::GreaterEqual if lhs.min >= rhs.max => Some(true),
        Relation::GreaterEqual if lhs.max < rhs.min => Some(false),
        _ => None,
    };
    value
        .map(IntegerFacts::bool)
        .unwrap_or_else(|| IntegerFacts::full(&Type::Bool).unwrap())
}

fn selected_edges<'a>(function: &'a Function, block: BlockId, values: &[LatticeValue]) -> Vec<(usize, &'a Edge)> {
    match function.blocks[block.0].terminator.as_ref().unwrap() {
        Terminator::Jump(edge) => vec![(0, edge)],
        Terminator::Branch {
            condition,
            then_edge,
            else_edge,
        } => {
            if values[condition.0] == LatticeValue::Unknown {
                Vec::new()
            } else {
                match exact_integer(values[condition.0]) {
                    Some(0) => vec![(1, else_edge)],
                    Some(_) => vec![(0, then_edge)],
                    None => vec![(0, then_edge), (1, else_edge)],
                }
            }
        }
        Terminator::Switch { value, cases, default } => {
            if values[value.0] == LatticeValue::Unknown {
                return Vec::new();
            }
            let Some(value) = exact_integer(values[value.0]) else {
                return cases
                    .iter()
                    .enumerate()
                    .map(|(index, (_, edge))| (index, edge))
                    .chain([(cases.len(), default)])
                    .collect();
            };
            let (index, edge) = cases
                .iter()
                .enumerate()
                .find_map(|(index, (pattern, edge))| pattern_matches(function, pattern, value).then_some((index, edge)))
                .unwrap_or((cases.len(), default));
            vec![(index, edge)]
        }
        Terminator::CheckedOperation {
            operation,
            inputs,
            success,
            failure,
            ..
        } => {
            if inputs.iter().any(|input| values[input.0] == LatticeValue::Unknown) {
                Vec::new()
            } else if checked_operation_cannot_fail(operation, inputs, values) {
                vec![(0, success)]
            } else {
                vec![(0, success), (1, failure)]
            }
        }
        Terminator::IndirectJump { .. } | Terminator::Return(_) | Terminator::Unreachable => Vec::new(),
    }
}

fn checked_operation_cannot_fail(operation: &Operation, inputs: &[ValueId], values: &[LatticeValue]) -> bool {
    let Operation::Intrinsic(Intrinsic::CheckedInteger(operation)) = operation else {
        return false;
    };
    let facts = inputs
        .iter()
        .map(|input| match values[input.0] {
            LatticeValue::Integer(facts) => Some(facts),
            _ => None,
        })
        .collect::<Option<Vec<_>>>();
    let Some(facts) = facts else { return false };
    match (operation, facts.as_slice()) {
        (CheckedIntegerOperation::Add, [lhs, rhs]) => range_fits(*lhs, *rhs, Arithmetic::Add, 32, true),
        (CheckedIntegerOperation::Subtract, [lhs, rhs]) => range_fits(*lhs, *rhs, Arithmetic::Sub, 32, true),
        (CheckedIntegerOperation::Multiply, [lhs, rhs]) => range_fits(*lhs, *rhs, Arithmetic::Mul, 32, true),
        _ => false,
    }
}

fn range_fits(lhs: IntegerFacts, rhs: IntegerFacts, operation: Arithmetic, bits: u8, signed: bool) -> bool {
    let (min, max) = type_bounds(bits, signed);
    arithmetic_candidates(lhs, rhs, operation)
        .is_some_and(|candidates| candidates.iter().all(|value| min <= *value && *value <= max))
}

fn arithmetic_candidates(lhs: IntegerFacts, rhs: IntegerFacts, operation: Arithmetic) -> Option<[i128; 4]> {
    let apply = |lhs: i128, rhs: i128| match operation {
        Arithmetic::Add => lhs.checked_add(rhs),
        Arithmetic::Sub => lhs.checked_sub(rhs),
        Arithmetic::Mul => lhs.checked_mul(rhs),
    };
    Some([
        apply(lhs.min, rhs.min)?,
        apply(lhs.min, rhs.max)?,
        apply(lhs.max, rhs.min)?,
        apply(lhs.max, rhs.max)?,
    ])
}

fn rewrite(function: &mut Function, values: &[LatticeValue], executable_blocks: &[bool]) -> bool {
    let mut changed = false;
    let mut replacements = HashMap::new();
    let mut eliminated_checked_results = Vec::new();
    for (index, lattice) in values.iter().enumerate() {
        let value = ValueId(index);
        if matches!(function.values[index].definition, ValueDefinition::Constant(_)) {
            continue;
        }
        let LatticeValue::Integer(facts) = lattice else {
            continue;
        };
        let Some(integer) = facts.exact_integer() else { continue };
        let constant = function.add_constant(function.values[index].ty.clone(), Constant::Integer(integer));
        replacements.insert(value, constant);
    }

    for block_index in 0..function.blocks.len() {
        let terminator = function.blocks[block_index].terminator.as_ref().unwrap().clone();
        let replacement = match terminator {
            Terminator::Branch {
                condition,
                then_edge,
                else_edge,
            } => exact_integer(values[condition.0]).map(|condition| {
                Terminator::Jump(if condition != 0 {
                    then_edge.clone()
                } else {
                    else_edge.clone()
                })
            }),
            Terminator::Switch { value, cases, default } => exact_integer(values[value.0]).map(|value| {
                let edge = cases
                    .iter()
                    .find_map(|(pattern, edge)| pattern_matches(function, pattern, value).then_some(edge))
                    .unwrap_or(&default);
                Terminator::Jump(edge.clone())
            }),
            Terminator::CheckedOperation {
                operation,
                inputs,
                results,
                effects,
                success,
                ..
            } if checked_operation_cannot_fail(&operation, &inputs, values) => {
                if results.iter().all(|result| replacements.contains_key(result)) {
                    Some(Terminator::Jump(success))
                } else {
                    unchecked_operation(&operation).map(|operation| {
                        let result_types = results
                            .iter()
                            .map(|result| function.values[result.0].ty.clone())
                            .collect();
                        let new_results = function.append_instruction_with_effects(
                            BlockId(block_index),
                            operation,
                            inputs,
                            result_types,
                            effects,
                        );
                        for (old, new) in results.iter().zip(&new_results) {
                            replacements.insert(*old, *new);
                        }
                        Terminator::Jump(success)
                    })
                }
            }
            _ => None,
        };
        if let Some(mut replacement) = replacement {
            if let Terminator::CheckedOperation { results, .. } =
                function.blocks[block_index].terminator.as_ref().unwrap()
            {
                eliminated_checked_results.extend(results.iter().copied());
            }
            rewrite_terminator(&mut replacement, &replacements);
            function.blocks[block_index].terminator = Some(replacement);
            changed = true;
        }
    }
    // NB: Rewriting after the block loop, so that a checked operation replaced by an unchecked one
    //     also redirects the uses of its results, including in the replacement instruction itself.
    for instruction in &mut function.instructions {
        rewrite_values(&mut instruction.inputs, &replacements);
    }
    for block in &mut function.blocks {
        rewrite_terminator(block.terminator.as_mut().unwrap(), &replacements);
    }
    for result in eliminated_checked_results {
        function.values[result.0].definition = ValueDefinition::Dead;
    }
    changed |= !replacements.is_empty();
    changed |= executable_blocks.iter().any(|executable| !executable) && eliminate_unreachable_blocks(function);
    changed
}

fn unchecked_operation(operation: &Operation) -> Option<Operation> {
    let Operation::Intrinsic(Intrinsic::CheckedInteger(operation)) = operation else {
        return None;
    };
    operation
        .unchecked()
        .map(|operation| Operation::Intrinsic(Intrinsic::IntegerBinary(operation)))
}

fn exact_integer(value: LatticeValue) -> Option<i64> {
    match value {
        LatticeValue::Integer(facts) => facts.exact_integer(),
        _ => None,
    }
}

fn pattern_matches(function: &Function, pattern: &ValueId, value: i64) -> bool {
    (match function.values[pattern.0].definition {
        ValueDefinition::Constant(Constant::Integer(value)) => Some(value),
        _ => None,
    }) == Some(value)
}

fn constant_with_layout(layout: IntegerFacts, raw: u64) -> IntegerFacts {
    let raw = truncate(raw, layout.bits);
    let value = semantic_value(raw, layout.bits, layout.signed);
    IntegerFacts {
        min: value,
        max: value,
        known_zero: layout.mask() & !raw,
        known_one: raw,
        ..layout
    }
}

fn integer_type(ty: &Type) -> Option<(u8, bool)> {
    Some(match ty {
        Type::I8 => (8, true),
        Type::I16 => (16, true),
        Type::I32 => (32, true),
        Type::I64 => (64, true),
        Type::U8 => (8, false),
        Type::U16 | Type::ValueTag => (16, false),
        Type::U32 => (32, false),
        Type::U64 => (64, false),
        Type::Bool => (1, false),
        _ => return None,
    })
}

fn type_bounds(bits: u8, signed: bool) -> (i128, i128) {
    if signed {
        let limit = 1i128 << (bits - 1);
        (-limit, limit - 1)
    } else {
        (0, (1i128 << bits) - 1)
    }
}

fn bit_mask(bits: u8) -> u64 {
    if bits == 64 { u64::MAX } else { (1u64 << bits) - 1 }
}

fn truncate(value: u64, bits: u8) -> u64 {
    value & bit_mask(bits)
}

fn semantic_value(raw: u64, bits: u8, signed: bool) -> i128 {
    if !signed || raw & (1u64 << (bits - 1)) == 0 {
        raw as i128
    } else {
        raw as i128 - (1i128 << bits)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn run_sccp(function: &mut Function) {
        let mut analyses = AnalysisManager::default();
        run(function, &mut analyses);
        function.validate().unwrap();
    }

    fn append_pure(
        function: &mut Function,
        block: BlockId,
        operation: Operation,
        inputs: Vec<ValueId>,
        ty: Type,
    ) -> ValueId {
        function.append_instruction(
            block,
            operation,
            inputs,
            vec![ty],
        )[0]
    }

    #[test]
    fn propagates_constant_expressions_through_control_flow() {
        let mut function = Function::new("constant", Vec::new(), vec![Type::I32]);
        let entry = function.entry;
        let selected = function.create_empty_block("selected", super::super::BlockLayout::Hot);
        let rejected = function.create_empty_block("rejected", super::super::BlockLayout::Cold);
        let three = function.add_constant(Type::I32, Constant::Integer(3));
        let four = function.add_constant(Type::I32, Constant::Integer(4));
        let seven = function.add_constant(Type::I32, Constant::Integer(7));
        let sum = append_pure(
            &mut function,
            entry,
            Operation::Intrinsic(Intrinsic::IntegerBinary(IntegerBinaryOperation::Binary(BinaryOperation::Add))),
            vec![three, four],
            Type::I32,
        );
        let condition = append_pure(
            &mut function,
            entry,
            Operation::Intrinsic(Intrinsic::IntegerComparison(IntegerComparisonOperation::Equal)),
            vec![sum, seven],
            Type::Bool,
        );
        function.set_terminator(
            entry,
            Terminator::branch_edges(condition, Edge::new(selected), Edge::new(rejected)),
        );
        function.set_terminator(selected, Terminator::Return(vec![sum]));
        function.set_terminator(rejected, Terminator::Return(vec![three]));

        run_sccp(&mut function);

        assert_eq!(function.blocks.len(), 2);
        let Terminator::Jump(edge) = function.blocks[entry.0].terminator.as_ref().unwrap() else {
            panic!("expected a constant branch to become a jump")
        };
        assert_eq!(edge.block, BlockId(1));
        let Terminator::Return(values) = function.blocks[1].terminator.as_ref().unwrap() else {
            panic!("expected the selected return")
        };
        assert_eq!(constant_integer(&function, values[0]), Some(7));
    }

    #[test]
    fn uses_known_bits_to_fold_nonconstant_inputs() {
        let mut function = Function::new("known-bits", vec![Type::I32], Vec::new());
        let entry = function.entry;
        let selected = function.create_empty_block("selected", super::super::BlockLayout::Hot);
        let rejected = function.create_empty_block("rejected", super::super::BlockLayout::Cold);
        let zero = function.add_constant(Type::I32, Constant::Integer(0));
        let input = function.parameter(0);
        let masked = append_pure(
            &mut function,
            entry,
            Operation::Intrinsic(Intrinsic::IntegerBinary(IntegerBinaryOperation::Binary(BinaryOperation::And))),
            vec![input, zero],
            Type::I32,
        );
        let condition = append_pure(
            &mut function,
            entry,
            Operation::Intrinsic(Intrinsic::IntegerComparison(IntegerComparisonOperation::Equal)),
            vec![masked, zero],
            Type::Bool,
        );
        function.set_terminator(
            entry,
            Terminator::branch_edges(condition, Edge::new(selected), Edge::new(rejected)),
        );
        function.set_terminator(selected, Terminator::Return(Vec::new()));
        function.set_terminator(rejected, Terminator::Unreachable);

        run_sccp(&mut function);

        assert_eq!(function.blocks.len(), 2);
        assert!(matches!(function.blocks[entry.0].terminator, Some(Terminator::Jump(_))));
    }

    #[test]
    fn range_analysis_removes_impossible_checked_arithmetic_failures() {
        let mut function = Function::new("checked-range", vec![Type::Bool], vec![Type::I32]);
        let entry = function.entry;
        let left = function.create_empty_block("left", super::super::BlockLayout::Hot);
        let right = function.create_empty_block("right", super::super::BlockLayout::Hot);
        let join = function.create_named_block("join", super::super::BlockLayout::Hot, vec![Type::I32]);
        let success = function.create_named_block("success", super::super::BlockLayout::Hot, vec![Type::I32]);
        let failure = function.create_empty_block("failure", super::super::BlockLayout::Cold);
        let ten = function.add_constant(Type::I32, Constant::Integer(10));
        let twenty = function.add_constant(Type::I32, Constant::Integer(20));
        let one = function.add_constant(Type::I32, Constant::Integer(1));
        function.set_terminator(
            entry,
            Terminator::branch(function.parameter(0), left, right),
        );
        function.set_terminator(
            left,
            Terminator::jump_with_arguments(join, vec![ten]),
        );
        function.set_terminator(
            right,
            Terminator::jump_with_arguments(join, vec![twenty]),
        );
        function.set_checked_operation(
            join,
            Intrinsic::CheckedInteger(CheckedIntegerOperation::Add),
            vec![function.blocks[join.0].parameters[0], one],
            vec![Type::I32],
            Effects::PURE,
            success,
            Vec::new(),
            Edge::new(failure),
        );
        function.set_terminator(
            success,
            Terminator::Return(vec![function.blocks[success.0].parameters[0]]),
        );
        function.set_terminator(failure, Terminator::Unreachable);

        run_sccp(&mut function);

        assert_eq!(function.blocks.len(), 5);
        assert!(
            function
                .instructions
                .iter()
                .any(|instruction| {
                    instruction.operation
                        == Operation::Intrinsic(Intrinsic::IntegerBinary(IntegerBinaryOperation::Binary(BinaryOperation::Add)))
                })
        );
        assert!(matches!(function.blocks[join.0].terminator, Some(Terminator::Jump(_))));
    }

    #[test]
    fn folds_shifts_at_the_width_of_the_shifted_value() {
        let mut function = Function::new("wide-shift", Vec::new(), vec![Type::U64]);
        let entry = function.entry;
        let one = function.add_constant(Type::U64, Constant::Integer(1));
        let forty = function.add_constant(Type::U64, Constant::Integer(40));
        let shifted = append_pure(
            &mut function,
            entry,
            Operation::Intrinsic(Intrinsic::IntegerBinary(IntegerBinaryOperation::Shift(ShiftOperation::Left))),
            vec![one, forty],
            Type::U64,
        );
        function.set_terminator(entry, Terminator::Return(vec![shifted]));

        run_sccp(&mut function);

        let Terminator::Return(values) = function.blocks[entry.0].terminator.as_ref().unwrap() else {
            panic!("expected a return")
        };
        assert_eq!(constant_integer(&function, values[0]), Some(1 << 40));
    }

    #[test]
    fn redirects_instruction_operands_to_replaced_checked_results() {
        let mut function = Function::new("checked-instruction", vec![Type::Bool], vec![Type::I32]);
        let entry = function.entry;
        let left = function.create_empty_block("left", super::super::BlockLayout::Hot);
        let right = function.create_empty_block("right", super::super::BlockLayout::Hot);
        let join = function.create_named_block("join", super::super::BlockLayout::Hot, vec![Type::I32]);
        let success = function.create_named_block("success", super::super::BlockLayout::Hot, vec![Type::I32]);
        let failure = function.create_empty_block("failure", super::super::BlockLayout::Cold);
        let ten = function.add_constant(Type::I32, Constant::Integer(10));
        let twenty = function.add_constant(Type::I32, Constant::Integer(20));
        let one = function.add_constant(Type::I32, Constant::Integer(1));
        function.set_terminator(entry, Terminator::branch(function.parameter(0), left, right));
        function.set_terminator(left, Terminator::jump_with_arguments(join, vec![ten]));
        function.set_terminator(right, Terminator::jump_with_arguments(join, vec![twenty]));
        let results = function.set_checked_operation(
            join,
            Intrinsic::CheckedInteger(CheckedIntegerOperation::Add),
            vec![function.blocks[join.0].parameters[0], one],
            vec![Type::I32],
            Effects::PURE,
            success,
            Vec::new(),
            Edge::new(failure),
        );
        // The sum cannot overflow, so the checked operation becomes a plain one. Its result is used
        // by an instruction rather than by a terminator, which is what has to be redirected.
        let doubled = append_pure(
            &mut function,
            success,
            Operation::Intrinsic(Intrinsic::IntegerBinary(IntegerBinaryOperation::Binary(BinaryOperation::Add))),
            vec![results[0], results[0]],
            Type::I32,
        );
        function.set_terminator(success, Terminator::Return(vec![doubled]));
        function.set_terminator(failure, Terminator::Unreachable);

        run_sccp(&mut function);

        let instruction = function.blocks[success.0].instructions[0];
        for input in &function.instructions[instruction.0].inputs {
            assert!(
                !matches!(function.values[input.0].definition, ValueDefinition::Dead),
                "an instruction still uses the replaced checked result"
            );
        }
    }

    #[test]
    fn keeps_differing_join_values_conservative() {
        let mut function = Function::new("join", vec![Type::Bool], Vec::new());
        let entry = function.entry;
        let left = function.create_empty_block("left", super::super::BlockLayout::Hot);
        let right = function.create_empty_block("right", super::super::BlockLayout::Hot);
        let join = function.create_named_block("join", super::super::BlockLayout::Hot, vec![Type::I32]);
        let zero_block = function.create_empty_block("zero", super::super::BlockLayout::Hot);
        let nonzero_block =
            function.create_empty_block("nonzero", super::super::BlockLayout::Hot);
        let zero = function.add_constant(Type::I32, Constant::Integer(0));
        let one = function.add_constant(Type::I32, Constant::Integer(1));
        function.set_terminator(
            entry,
            Terminator::branch(function.parameter(0), left, right),
        );
        function.set_terminator(
            left,
            Terminator::jump_with_arguments(join, vec![zero]),
        );
        function.set_terminator(
            right,
            Terminator::jump_with_arguments(join, vec![one]),
        );
        let join_value = function.blocks[join.0].parameters[0];
        let condition = append_pure(
            &mut function,
            join,
            Operation::Intrinsic(Intrinsic::IntegerComparison(IntegerComparisonOperation::Equal)),
            vec![join_value, zero],
            Type::Bool,
        );
        function.set_terminator(
            join,
            Terminator::branch_edges(condition, Edge::new(zero_block), Edge::new(nonzero_block)),
        );
        function.set_terminator(zero_block, Terminator::Return(Vec::new()));
        function.set_terminator(nonzero_block, Terminator::Return(Vec::new()));

        run_sccp(&mut function);

        assert_eq!(function.blocks.len(), 6);
        assert!(matches!(
            function.blocks[join.0].terminator,
            Some(Terminator::Branch { .. })
        ));
    }

    #[test]
    fn widens_loop_ranges_to_reach_a_fixed_point() {
        let mut function = Function::new("loop", vec![Type::Bool], Vec::new());
        let entry = function.entry;
        let header = function.create_named_block("header", super::super::BlockLayout::Hot, vec![Type::I32]);
        let exit = function.create_empty_block("exit", super::super::BlockLayout::Hot);
        let zero = function.add_constant(Type::I32, Constant::Integer(0));
        let one = function.add_constant(Type::I32, Constant::Integer(1));
        function.set_terminator(
            entry,
            Terminator::jump_with_arguments(header, vec![zero]),
        );
        let induction = function.blocks[header.0].parameters[0];
        let next = append_pure(
            &mut function,
            header,
            Operation::Intrinsic(Intrinsic::IntegerBinary(IntegerBinaryOperation::Binary(BinaryOperation::Add))),
            vec![induction, one],
            Type::I32,
        );
        function.set_terminator(
            header,
            Terminator::branch_edges(
                function.parameter(0),
                Edge::with_arguments(header, vec![next]),
                Edge::new(exit),
            ),
        );
        function.set_terminator(exit, Terminator::Return(Vec::new()));

        run_sccp(&mut function);

        assert_eq!(function.blocks.len(), 3);
        assert!(matches!(
            function.blocks[header.0].terminator,
            Some(Terminator::Branch { .. })
        ));
    }

    #[test]
    fn keeps_wide_wrapping_arithmetic_conservative() {
        let mut function = Function::new("wide", vec![Type::U64, Type::U64], vec![Type::U64]);
        let entry = function.entry;
        let lhs = function.parameter(0);
        let rhs = function.parameter(1);
        let product = append_pure(
            &mut function,
            entry,
            Operation::Intrinsic(Intrinsic::IntegerBinary(IntegerBinaryOperation::Binary(BinaryOperation::Multiply))),
            vec![lhs, rhs],
            Type::U64,
        );
        function.set_terminator(entry, Terminator::Return(vec![product]));

        run_sccp(&mut function);

        let Terminator::Return(values) = function.blocks[entry.0].terminator.as_ref().unwrap() else {
            panic!("expected a return")
        };
        assert_eq!(values, &[product]);
    }

    fn constant_integer(function: &Function, value: ValueId) -> Option<i64> {
        match function.values[value.0].definition {
            ValueDefinition::Constant(Constant::Integer(value)) => Some(value),
            _ => None,
        }
    }
}
