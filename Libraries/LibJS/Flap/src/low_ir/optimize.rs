/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Machine-level optimizations before register allocation.

use super::{
    AddressDisplacement, AddressRegister, AddressScale, ControlFlowOpcode, ControlFlowOperand, Instruction, Label,
    MemoryAddress, Operand, VirtualRegister,
};
use crate::hash::{HashMap, HashSet};
use crate::intrinsic::{BranchOperation, IntegerBinaryOperation};
#[cfg(test)]
use crate::target::description::ZeroCondition;
use crate::target::description::{
    BinaryOperation, IntegerWidth, MemoryWidth, OperandKind, Operation, PairWidth, SignCondition,
};

/// Invert a conditional branch when its taken block follows an unconditional
/// jump. This removes the jump while preserving block order.
pub(crate) fn invert_branches_over_jumps<O: ControlFlowOperand, C: ControlFlowOpcode>(
    instructions: &mut Vec<Instruction<O, C>>,
) {
    let (mut read, mut write) = (0, 0);
    while read < instructions.len() {
        let inverted = (read + 2 < instructions.len())
            .then(|| inverted_branch(instructions, read))
            .flatten();
        if write != read {
            instructions.swap(write, read);
        }
        let Some((operation, jump_target)) = inverted else {
            read += 1;
            write += 1;
            continue;
        };
        instructions[write].opcode = instructions[write].opcode.replacing_operation(operation);
        *instructions[write]
            .operands
            .last_mut()
            .expect("a branch names its target") = O::from_label(jump_target);
        read += 2;
        write += 1;
    }
    instructions.truncate(write);
}

fn inverted_branch<O: ControlFlowOperand, C: ControlFlowOpcode>(
    instructions: &[Instruction<O, C>],
    index: usize,
) -> Option<(Operation, Label)> {
    let operation = inverted_branch_operation(&instructions[index].opcode)?;
    let branch_target = instructions[index].operands.last().and_then(O::label)?;
    let [jump_target] = &instructions[index + 1].operands[..] else {
        return None;
    };
    let jump_target = jump_target.label()?;
    let [fallthrough_label] = &instructions[index + 2].operands[..] else {
        return None;
    };
    let fallthrough_label = fallthrough_label.label()?;
    if instructions[index + 1].opcode.operation() != Operation::Control(crate::intrinsic::ControlOperation::JumpLabel)
        || instructions[index + 2].opcode.operation() != Operation::Label
        || branch_target != fallthrough_label
    {
        return None;
    }
    Some((operation, jump_target.clone()))
}

pub(crate) fn inverted_branch_operation(opcode: &impl ControlFlowOpcode) -> Option<Operation> {
    let operation = opcode.operation();
    Some(match operation {
        Operation::Branch(operation) => Operation::Branch(operation.inverted()?),
        Operation::Or32Branch(condition) => Operation::Or32Branch(condition.inverted()),
        _ => return None,
    })
}

pub(crate) fn remove_unreferenced_labels<O: ControlFlowOperand, C: ControlFlowOpcode>(
    instructions: &mut Vec<Instruction<O, C>>,
) {
    let keep = {
        let referenced = instructions
            .iter()
            .filter(|instruction| instruction.opcode.operation() != Operation::Label)
            .flat_map(|instruction| &instruction.operands)
            .filter_map(ControlFlowOperand::label)
            .collect::<HashSet<_>>();
        instructions
            .iter()
            .map(|instruction| {
                instruction.opcode.operation() != Operation::Label
                    || matches!(instruction.operands.as_slice(), [operand] if operand.label().is_some_and(|label| referenced.contains(label)))
            })
            .collect::<Vec<_>>()
    };
    let mut index = 0;
    instructions.retain(|_| {
        index += 1;
        keep[index - 1]
    });
}

/// How many operands of a listing name each virtual register.
///
/// A peephole rewrite that folds a value away has to know the value is named
/// nowhere else. Counting that per window means scanning the whole listing per
/// instruction, which on a large function is thousands of scans of thousands
/// of instructions.
#[derive(Default)]
struct OperandReferences(HashMap<VirtualRegister, usize>);

impl OperandReferences {
    fn count(instructions: &[Instruction]) -> Self {
        let mut counts: HashMap<VirtualRegister, usize> = HashMap::default();
        for instruction in instructions {
            for operand in &instruction.operands {
                match operand {
                    Operand::VirtualRegister(register) => *counts.entry(register.clone()).or_default() += 1,
                    // An address naming the same register as both base and
                    // index is still one operand that references it.
                    Operand::Address(address) => {
                        let mut counted = None;
                        for candidate in std::iter::once(&address.base).chain(&address.index) {
                            let AddressRegister::Virtual(register) = candidate else {
                                continue;
                            };
                            if counted == Some(register) {
                                continue;
                            }
                            counted = Some(register);
                            *counts.entry(register.clone()).or_default() += 1;
                        }
                    }
                    _ => {}
                }
            }
        }
        Self(counts)
    }

    fn get(&self, register: &VirtualRegister) -> usize {
        self.0.get(register).copied().unwrap_or_default()
    }
}

fn rewrite_windows(
    instructions: &mut Vec<Instruction>,
    width: usize,
    mut rewrite: impl FnMut(&[Instruction]) -> Option<Instruction>,
) {
    let (mut read, mut write) = (0, 0);
    while read < instructions.len() {
        if read + width <= instructions.len()
            && let Some(replacement) = rewrite(&instructions[read..read + width])
        {
            instructions[write] = replacement;
            read += width;
            write += 1;
            continue;
        }
        if write != read {
            instructions.swap(write, read);
        }
        read += 1;
        write += 1;
    }
    instructions.truncate(write);
}

pub(crate) fn propagate_single_assignment_copies(instructions: &mut Vec<Instruction>) {
    let mut definition_counts = HashMap::default();
    let mut pinned = HashSet::default();
    for instruction in instructions.iter() {
        let info = instruction.opcode.description();
        for (index, _) in info.x86_64.fixed_operands.iter().chain(info.aarch64.fixed_operands) {
            if let Some(Operand::VirtualRegister(register)) = instruction.operands.get(*index) {
                pinned.insert(register.clone());
            }
        }
        for (index, operand) in instruction.operands.iter().enumerate() {
            let kind = info.operand_kind(index, instruction.operands.len());
            if !matches!(
                kind,
                Some(
                    OperandKind::GprOut
                        | OperandKind::GprInOut
                        | OperandKind::FprOut
                        | OperandKind::FprInOut
                        | OperandKind::RegisterOut
                )
            ) {
                continue;
            }
            if let Operand::VirtualRegister(register) = operand {
                *definition_counts.entry(register.clone()).or_default() += 1;
            }
        }
    }
    let replacements = instructions
        .iter()
        .filter_map(|instruction| {
            let [Operand::VirtualRegister(destination), Operand::VirtualRegister(source)] =
                instruction.operands.as_slice()
            else {
                return None;
            };
            let same_class_float_copy =
                instruction.opcode.operation() == Operation::FloatMove && destination.class() == source.class();
            ((instruction.opcode.operation() == Operation::Move(IntegerWidth::U64) || same_class_float_copy)
                && destination.is_ssa()
                && !destination.is_edge_temporary()
                && source.is_ssa()
                && !pinned.contains(destination)
                && !pinned.contains(source)
                && definition_counts.get(destination) == Some(&1))
            .then(|| (destination.clone(), source.clone()))
        })
        .collect::<HashMap<_, _>>();
    if replacements.is_empty() {
        return;
    }
    let resolve = |register: &super::VirtualRegister| {
        let mut resolved = register;
        while let Some(replacement) = replacements.get(resolved) {
            resolved = replacement;
        }
        resolved.clone()
    };
    for instruction in instructions.iter_mut() {
        for operand in &mut instruction.operands {
            match operand {
                Operand::VirtualRegister(register) => *register = resolve(register),
                Operand::Address(address) => {
                    for register in [&mut address.base].into_iter().chain(address.index.iter_mut()) {
                        if let AddressRegister::Virtual(register) = register {
                            *register = resolve(register);
                        }
                    }
                }
                _ => {}
            }
        }
    }
    instructions.retain(|instruction| {
        !matches!(
            instruction.operands.as_slice(),
            [Operand::VirtualRegister(destination), Operand::VirtualRegister(source)]
                if matches!(
                    instruction.opcode.operation(),
                    Operation::Move(IntegerWidth::U64) | Operation::FloatMove
                ) && destination == source
        )
    });
}

pub(crate) fn select_copy_add_immediates(instructions: &mut Vec<Instruction>) {
    rewrite_windows(instructions, 2, |window| {
        let [copy, add] = window else { unreachable!() };
        let (
            [Operand::VirtualRegister(destination), Operand::VirtualRegister(source)],
            [Operand::VirtualRegister(add_destination), Operand::Immediate(immediate)],
        ) = (copy.operands.as_slice(), add.operands.as_slice())
        else {
            return None;
        };
        if copy.opcode.operation() != Operation::Move(IntegerWidth::U64)
            || add.opcode.operation()
                != (Operation::IntegerBinary {
                    operation: IntegerBinaryOperation::Binary(BinaryOperation::Add),
                    width: IntegerWidth::U64,
                })
            || destination != add_destination
            || destination == source
        {
            return None;
        }
        Some(Instruction {
            opcode: Operation::LoadEffectiveAddress,
            operands: vec![
                Operand::VirtualRegister(destination.clone()),
                Operand::Address(MemoryAddress {
                    base: AddressRegister::Virtual(source.clone()),
                    index: None,
                    scale: None,
                    displacement: Some(AddressDisplacement::Immediate(*immediate)),
                }),
            ],
        })
    });
}

pub(crate) fn fuse_or32_sign_branches(instructions: &mut Vec<Instruction>) {
    rewrite_windows(instructions, 2, |window| {
        let [operation, branch] = window else { unreachable!() };
        let [destination, lhs, rhs] = operation.operands.as_slice() else {
            return None;
        };
        let [Operand::VirtualRegister(branch_value), label] = branch.operands.as_slice() else {
            return None;
        };
        let Operand::VirtualRegister(destination_register) = destination else {
            return None;
        };
        let opcode = match branch.opcode.operation() {
            Operation::Branch(BranchOperation::Sign {
                width: IntegerWidth::U32,
                condition: SignCondition::Negative,
            }) => Operation::Or32Branch(SignCondition::Negative),
            Operation::Branch(BranchOperation::Sign {
                width: IntegerWidth::U32,
                condition: SignCondition::NotNegative,
            }) => Operation::Or32Branch(SignCondition::NotNegative),
            _ => return None,
        };
        if operation.opcode.operation()
            != (Operation::IntegerBinary {
                operation: IntegerBinaryOperation::Binary(BinaryOperation::Or),
                width: IntegerWidth::U32,
            })
            || destination_register != branch_value
        {
            return None;
        }
        Some(Instruction {
            opcode,
            operands: vec![destination.clone(), lhs.clone(), rhs.clone(), label.clone()],
        })
    });
}

pub(crate) fn fuse_adjacent_sequence_stores(instructions: &mut Vec<Instruction>) {
    let adjacent_indices = instructions
        .iter()
        .filter_map(|instruction| {
            let [Operand::VirtualRegister(destination), Operand::Address(address)] = instruction.operands.as_slice()
            else {
                return None;
            };
            let AddressRegister::Virtual(base) = &address.base else {
                return None;
            };
            (instruction.opcode.operation() == Operation::LoadEffectiveAddress
                && address.index.is_none()
                && address.scale.is_none()
                && address.displacement == Some(AddressDisplacement::Immediate(1)))
            .then(|| (destination.clone(), base.clone()))
        })
        .collect::<HashMap<_, _>>();
    rewrite_windows(instructions, 2, |window| {
        let [first, second] = window else { unreachable!() };
        let ([Operand::Address(first_address), first_value], [Operand::Address(second_address), second_value]) =
            (first.operands.as_slice(), second.operands.as_slice())
        else {
            return None;
        };
        let (first_index, first_scale) = virtual_scaled_index(first_address)?;
        let (second_index, second_scale) = virtual_scaled_index(second_address)?;
        if first.opcode.operation() != Operation::store(MemoryWidth::DoubleWord)
            || second.opcode.operation() != Operation::store(MemoryWidth::DoubleWord)
            || first_address.base != second_address.base
            || first_scale != 8
            || second_scale != first_scale
            || adjacent_indices.get(second_index) != Some(first_index)
        {
            return None;
        }
        Some(Instruction {
            opcode: Operation::StorePairIndexed(PairWidth::DoubleWord),
            operands: vec![
                address_register_operand(&first_address.base),
                Operand::VirtualRegister(first_index.clone()),
                Operand::Immediate(8),
                first_value.clone(),
                second_value.clone(),
            ],
        })
    });
}

pub(crate) fn fuse_indexed_offset_stores(instructions: &mut Vec<Instruction>) {
    let references = OperandReferences::count(instructions);
    rewrite_windows(instructions, 2, |window| {
        let [load_address, store] = window else { unreachable!() };
        let (
            [Operand::VirtualRegister(address), Operand::Address(offset_address)],
            [Operand::Address(store_address), value],
        ) = (load_address.operands.as_slice(), store.operands.as_slice())
        else {
            return None;
        };
        let AddressRegister::Virtual(base) = &offset_address.base else {
            return None;
        };
        let offset = match &offset_address.displacement {
            Some(AddressDisplacement::Immediate(offset)) => Operand::Immediate(*offset),
            Some(AddressDisplacement::LayoutConstant(offset)) => Operand::LayoutConstant(*offset),
            _ => return None,
        };
        let (store_index, scale) = virtual_scaled_index(store_address)?;
        if load_address.opcode.operation() != Operation::LoadEffectiveAddress
            || store.opcode.operation() != Operation::store(MemoryWidth::DoubleWord)
            || store_address.base != AddressRegister::Virtual(address.clone())
            || offset_address.index.is_some()
            || offset_address.scale.is_some()
            || references.get(address) != 2
        {
            return None;
        }
        if !matches!(scale, 1 | 2 | 4 | 8) {
            return None;
        }
        Some(Instruction {
            opcode: Operation::Store64IndexedOffset,
            operands: vec![
                Operand::VirtualRegister(base.clone()),
                Operand::VirtualRegister(store_index.clone()),
                Operand::Immediate(scale),
                offset,
                value.clone(),
            ],
        })
    });
}

pub(crate) fn select_zero_moves(instructions: &mut [Instruction]) {
    for instruction in instructions {
        let [Operand::VirtualRegister(destination), Operand::Immediate(0)] = instruction.operands.as_slice() else {
            continue;
        };
        if instruction.opcode.operation() == Operation::Move(IntegerWidth::U64) {
            instruction.opcode = Operation::IntegerBinary {
                operation: IntegerBinaryOperation::Binary(BinaryOperation::Xor),
                width: IntegerWidth::U64,
            };
            instruction.operands = vec![
                Operand::VirtualRegister(destination.clone()),
                Operand::VirtualRegister(destination.clone()),
            ];
        }
    }
}

pub(crate) fn fuse_scaled_addresses(instructions: &mut Vec<Instruction>) {
    let references = OperandReferences::count(instructions);
    rewrite_windows(instructions, 3, |window| {
        let [multiply, copy, add] = window else { unreachable!() };
        let (
            [
                Operand::VirtualRegister(product),
                Operand::VirtualRegister(scaled_index),
                Operand::Immediate(scale),
            ],
            [Operand::VirtualRegister(destination), Operand::VirtualRegister(base)],
            [
                Operand::VirtualRegister(add_destination),
                Operand::VirtualRegister(addend),
            ],
        ) = (
            multiply.operands.as_slice(),
            copy.operands.as_slice(),
            add.operands.as_slice(),
        )
        else {
            return None;
        };
        if multiply.opcode.operation()
            != (Operation::IntegerBinary {
                operation: IntegerBinaryOperation::Binary(BinaryOperation::Multiply),
                width: IntegerWidth::U64,
            })
            || copy.opcode.operation() != Operation::Move(IntegerWidth::U64)
            || add.opcode.operation()
                != (Operation::IntegerBinary {
                    operation: IntegerBinaryOperation::Binary(BinaryOperation::Add),
                    width: IntegerWidth::U64,
                })
            || destination != add_destination
            || product != addend
            || !matches!(scale, 1 | 2 | 4 | 8)
            || references.get(product) != 2
        {
            return None;
        }
        Some(Instruction {
            opcode: Operation::LoadEffectiveAddress,
            operands: vec![
                Operand::VirtualRegister(destination.clone()),
                Operand::Address(MemoryAddress {
                    base: AddressRegister::Virtual(base.clone()),
                    index: Some(AddressRegister::Virtual(scaled_index.clone())),
                    scale: Some(AddressScale::Immediate(*scale)),
                    displacement: None,
                }),
            ],
        })
    });
}

fn address_register_operand(register: &AddressRegister) -> Operand {
    match register {
        AddressRegister::Virtual(register) => Operand::VirtualRegister(register.clone()),
        AddressRegister::Interpreter(register) => Operand::InterpreterRegister(*register),
        AddressRegister::Physical(register) => Operand::PhysicalRegister(*register),
    }
}

fn virtual_scaled_index(address: &MemoryAddress) -> Option<(&VirtualRegister, i64)> {
    if address.displacement.is_some() {
        return None;
    }
    let Some(AddressRegister::Virtual(index)) = &address.index else {
        return None;
    };
    let Some(AddressScale::Immediate(scale)) = &address.scale else {
        return None;
    };
    Some((index, *scale))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn test_layout_constant(name: &str, value: i64) -> crate::frontend::layout::LayoutConstant {
        crate::frontend::layout::LayoutConstants::from_values([(name.to_string(), value)])
            .get(name)
            .unwrap()
    }

    fn instruction(operation: Operation, operands: impl IntoIterator<Item = Operand>) -> Instruction {
        Instruction {
            opcode: operation,
            operands: operands.into_iter().collect(),
        }
    }

    fn register(name: &str) -> Operand {
        Operand::VirtualRegister(name.into())
    }

    fn label(name: &str) -> Operand {
        Operand::Label(name.into())
    }

    fn move64(destination: &str, source: &str) -> Instruction {
        instruction(
            Operation::Move(IntegerWidth::U64),
            [register(destination), register(source)],
        )
    }

    fn assert_nonzero(register_name: &str) -> Instruction {
        instruction(
            Operation::Assertion(crate::intrinsic::AssertionOperation::NonZero),
            [register(register_name)],
        )
    }

    fn add64(destination: &str, rhs: Operand) -> Instruction {
        instruction(
            Operation::IntegerBinary {
                operation: IntegerBinaryOperation::Binary(BinaryOperation::Add),
                width: IntegerWidth::U64,
            },
            [register(destination), rhs],
        )
    }

    fn multiply64(destination: &str, lhs: &str, rhs: Operand) -> Instruction {
        instruction(
            Operation::IntegerBinary {
                operation: IntegerBinaryOperation::Binary(BinaryOperation::Multiply),
                width: IntegerWidth::U64,
            },
            [register(destination), register(lhs), rhs],
        )
    }

    fn store64(address: Operand, value: &str) -> Instruction {
        instruction(Operation::store(MemoryWidth::DoubleWord), [address, register(value)])
    }

    fn load_effective_address(destination: &str, address: Operand) -> Instruction {
        instruction(Operation::LoadEffectiveAddress, [register(destination), address])
    }

    fn floating_point_register(name: &str) -> Operand {
        Operand::VirtualRegister(VirtualRegister::with_class(
            name,
            crate::types::RegisterClass::FloatingPoint,
        ))
    }

    fn address(
        base: &str,
        index: Option<&str>,
        scale: Option<i64>,
        displacement: Option<AddressDisplacement>,
    ) -> Operand {
        Operand::Address(MemoryAddress {
            base: AddressRegister::named(base),
            index: index.map(AddressRegister::named),
            scale: scale.map(AddressScale::Immediate),
            displacement,
        })
    }

    fn evaluate_integer_listing(instructions: &[Instruction]) -> Vec<u64> {
        let mut registers = HashMap::default();
        let mut stored = Vec::new();
        let value = |operand: &Operand, registers: &HashMap<VirtualRegister, u64>| match operand {
            Operand::VirtualRegister(register) => registers[register],
            Operand::Immediate(immediate) => *immediate as u64,
            _ => panic!("unsupported oracle operand: {operand:?}"),
        };
        for instruction in instructions {
            match (instruction.opcode.operation(), instruction.operands.as_slice()) {
                (Operation::Move(IntegerWidth::U64), [Operand::VirtualRegister(destination), source]) => {
                    registers.insert(destination.clone(), value(source, &registers));
                }
                (
                    Operation::IntegerBinary {
                        operation: IntegerBinaryOperation::Binary(BinaryOperation::Add),
                        width: IntegerWidth::U64,
                    },
                    [Operand::VirtualRegister(destination), rhs],
                ) => {
                    let result = value(&Operand::VirtualRegister(destination.clone()), &registers)
                        .wrapping_add(value(rhs, &registers));
                    registers.insert(destination.clone(), result);
                }
                (
                    Operation::IntegerBinary {
                        operation: IntegerBinaryOperation::Binary(BinaryOperation::Xor),
                        width: IntegerWidth::U64,
                    },
                    [Operand::VirtualRegister(destination), rhs],
                ) => {
                    let result = if rhs == &Operand::VirtualRegister(destination.clone()) {
                        0
                    } else {
                        value(&Operand::VirtualRegister(destination.clone()), &registers) ^ value(rhs, &registers)
                    };
                    registers.insert(destination.clone(), result);
                }
                (
                    Operation::LoadEffectiveAddress,
                    [
                        Operand::VirtualRegister(destination),
                        Operand::Address(MemoryAddress {
                            base: AddressRegister::Virtual(base),
                            index: None,
                            scale: None,
                            displacement: Some(AddressDisplacement::Immediate(displacement)),
                        }),
                    ],
                ) => {
                    registers.insert(destination.clone(), registers[base].wrapping_add(*displacement as u64));
                }
                (Operation::StoreOperand, [Operand::Immediate(_), source]) => {
                    stored.push(value(source, &registers));
                }
                _ => panic!("unsupported oracle instruction: {instruction:?}"),
            }
        }
        stored
    }

    #[test]
    fn integer_peepholes_preserve_randomized_listings() {
        let mut random_state = 0xbb67ae8584caa73bu64;
        let mut random = || {
            random_state = random_state.wrapping_mul(2862933555777941757).wrapping_add(3037000493);
            random_state
        };

        for _ in 0..250 {
            let mut instructions = vec![instruction(
                Operation::Move(IntegerWidth::U64),
                [register("ssa_0"), Operand::Immediate(random() as i64)],
            )];
            let mut current = "ssa_0".to_string();
            for index in 1..=(random() % 30 + 1) {
                let next = format!("ssa_{index}");
                match random() % 3 {
                    0 => instructions.push(move64(&next, &current)),
                    1 => {
                        instructions.push(move64(&next, &current));
                        instructions.push(add64(&next, Operand::Immediate(random() as i64)));
                    }
                    _ => instructions.push(instruction(
                        Operation::Move(IntegerWidth::U64),
                        [register(&next), Operand::Immediate(0)],
                    )),
                }
                current = next;
            }
            instructions.push(instruction(
                Operation::StoreOperand,
                [Operand::Immediate(0), register(&current)],
            ));

            let expected = evaluate_integer_listing(&instructions);
            propagate_single_assignment_copies(&mut instructions);
            select_copy_add_immediates(&mut instructions);
            select_zero_moves(&mut instructions);
            assert_eq!(evaluate_integer_listing(&instructions), expected);
        }
    }

    #[test]
    fn inverts_branches_over_jumps() {
        let mut instructions = vec![
            instruction(
                Operation::branch_zero(IntegerWidth::U64, ZeroCondition::Zero),
                [register("value"), label("failure")],
            ),
            instruction(
                Operation::Control(crate::intrinsic::ControlOperation::JumpLabel),
                [label("success")],
            ),
            instruction(Operation::Label, [label("failure")]),
        ];

        invert_branches_over_jumps(&mut instructions);

        assert_eq!(instructions.len(), 2);
        assert_eq!(
            instructions[0].opcode.operation(),
            Operation::branch_zero(IntegerWidth::U64, ZeroCondition::NonZero),
        );
        assert!(matches!(
            instructions[0].operands.last(),
            Some(Operand::Label(label)) if label.as_str() == "success"
        ));
    }

    #[test]
    fn removes_only_unreferenced_labels() {
        let mut instructions = vec![
            instruction(
                Operation::Control(crate::intrinsic::ControlOperation::JumpLabel),
                [label("used")],
            ),
            instruction(Operation::Label, [label("unused")]),
            instruction(Operation::Label, [label("used")]),
        ];

        remove_unreferenced_labels(&mut instructions);

        assert_eq!(instructions.len(), 2);
        assert!(matches!(
            instructions[1].operands.as_slice(),
            [Operand::Label(label)] if label.as_str() == "used"
        ));
    }

    #[test]
    fn propagates_single_assignment_copy_chains() {
        let mut instructions = vec![
            instruction(
                Operation::load(MemoryWidth::DoubleWord, false),
                [register("ssa_source"), address("values", None, None, None)],
            ),
            move64("ssa_copy", "ssa_source"),
            move64("ssa_result", "ssa_copy"),
            assert_nonzero("ssa_result"),
        ];

        propagate_single_assignment_copies(&mut instructions);

        assert!(
            !instructions
                .iter()
                .any(|instruction| instruction.opcode.operation() == Operation::Move(IntegerWidth::U64))
        );
        assert_eq!(instructions.last().unwrap().operands, [register("ssa_source")]);
    }

    #[test]
    fn propagates_single_assignment_float_copies() {
        let mut instructions = vec![
            instruction(
                Operation::FloatMove,
                vec![
                    floating_point_register("ssa_copy"),
                    floating_point_register("ssa_source"),
                ],
            ),
            instruction(
                Operation::Branch(BranchOperation::Float(
                    crate::target::description::FloatCondition::Equal,
                )),
                vec![
                    floating_point_register("ssa_copy"),
                    floating_point_register("ssa_copy"),
                    label("done"),
                ],
            ),
        ];

        propagate_single_assignment_copies(&mut instructions);

        assert!(
            !instructions
                .iter()
                .any(|instruction| { instruction.opcode.operation() == Operation::FloatMove })
        );
        assert_eq!(
            instructions.last().unwrap().operands[0],
            floating_point_register("ssa_source")
        );
        assert_eq!(
            instructions.last().unwrap().operands[1],
            floating_point_register("ssa_source")
        );
    }

    #[test]
    fn preserves_float_copies_between_register_classes() {
        let mut instructions = vec![instruction(
            Operation::FloatMove,
            vec![floating_point_register("ssa_double"), register("ssa_bits")],
        )];

        propagate_single_assignment_copies(&mut instructions);

        assert!(
            instructions
                .iter()
                .any(|instruction| { instruction.opcode.operation() == Operation::FloatMove })
        );
    }

    #[test]
    fn preserves_copies_into_abi_pinned_operands() {
        let mut instructions = vec![
            instruction(
                Operation::load(MemoryWidth::DoubleWord, false),
                vec![register("ssa_source"), address("values", None, None, None)],
            ),
            move64("ssa_argument", "ssa_source"),
            instruction(
                Operation::Call(crate::target::description::CallKind::Helper),
                [register("helper"), register("ssa_argument"), register("ssa_result")],
            ),
        ];

        propagate_single_assignment_copies(&mut instructions);

        assert!(instructions.iter().any(|instruction| {
            instruction.opcode.operation() == Operation::Move(IntegerWidth::U64)
                && instruction.operands == [register("ssa_argument"), register("ssa_source")]
        }));
    }

    #[test]
    fn preserves_copies_out_of_abi_pinned_results() {
        let mut instructions = vec![
            instruction(
                Operation::Call(crate::target::description::CallKind::RawNative),
                [
                    register("ssa_function"),
                    register("ssa_payload"),
                    register("ssa_variant"),
                ],
            ),
            move64("ssa_saved", "ssa_payload"),
            assert_nonzero("ssa_saved"),
        ];

        propagate_single_assignment_copies(&mut instructions);

        assert!(instructions.iter().any(|instruction| {
            instruction.opcode.operation() == Operation::Move(IntegerWidth::U64)
                && instruction.operands == [register("ssa_saved"), register("ssa_payload")]
        }));
    }

    #[test]
    fn selects_single_instruction_copy_adds() {
        let mut instructions = vec![move64("next", "index"), add64("next", Operand::Immediate(1))];

        select_copy_add_immediates(&mut instructions);

        assert_eq!(instructions.len(), 1);
        assert_eq!(instructions[0].opcode.operation(), Operation::LoadEffectiveAddress,);
        assert_eq!(
            instructions[0].operands,
            [
                register("next"),
                address("index", None, None, Some(AddressDisplacement::Immediate(1))),
            ]
        );
    }

    #[test]
    fn fuses_adjacent_indexed_sequence_stores() {
        let indexed = |index: &str| address("values", Some(index), Some(8), None);
        let mut instructions = vec![
            load_effective_address(
                "next",
                address("index", None, None, Some(AddressDisplacement::Immediate(1))),
            ),
            store64(indexed("index"), "first"),
            store64(indexed("next"), "second"),
        ];

        fuse_adjacent_sequence_stores(&mut instructions);

        assert_eq!(instructions.len(), 2);
        assert_eq!(
            instructions[1].opcode.operation(),
            Operation::StorePairIndexed(PairWidth::DoubleWord),
        );
        assert_eq!(
            instructions[1].operands,
            [
                Operand::InterpreterRegister(crate::types::InterpreterRegister::Values),
                register("index"),
                Operand::Immediate(8),
                register("first"),
                register("second"),
            ]
        );
    }

    #[test]
    fn selects_zero_idioms() {
        let mut instructions = vec![instruction(
            Operation::Move(IntegerWidth::U64),
            [register("value"), Operand::Immediate(0)],
        )];

        select_zero_moves(&mut instructions);

        assert_eq!(
            instructions[0].opcode.operation(),
            Operation::IntegerBinary {
                operation: IntegerBinaryOperation::Binary(BinaryOperation::Xor),
                width: IntegerWidth::U64,
            },
        );
        assert_eq!(instructions[0].operands, [register("value"), register("value")]);
    }

    #[test]
    fn fuses_or32_with_a_sign_branch() {
        let mut instructions = vec![
            instruction(
                Operation::IntegerBinary {
                    operation: IntegerBinaryOperation::Binary(BinaryOperation::Or),
                    width: IntegerWidth::U32,
                },
                vec![register("result"), register("lhs"), register("rhs")],
            ),
            instruction(
                Operation::branch_sign(IntegerWidth::U32, SignCondition::Negative),
                [register("result"), label("negative")],
            ),
        ];

        fuse_or32_sign_branches(&mut instructions);

        assert_eq!(instructions.len(), 1);
        assert_eq!(
            instructions[0].opcode.operation(),
            Operation::Or32Branch(SignCondition::Negative),
        );
        assert_eq!(
            instructions[0].operands,
            [register("result"), register("lhs"), register("rhs"), label("negative"),]
        );
    }

    #[test]
    fn fuses_scaled_integer_additions_into_addresses() {
        let mut instructions = vec![
            multiply64("product", "index", Operand::Immediate(4)),
            move64("address", "base"),
            add64("address", register("product")),
        ];

        fuse_scaled_addresses(&mut instructions);

        assert_eq!(
            instructions[0].operands,
            vec![register("address"), address("base", Some("index"), Some(4), None),]
        );
        assert_eq!(instructions[0].opcode.operation(), Operation::LoadEffectiveAddress,);
        assert_eq!(instructions.len(), 1);
    }

    #[test]
    fn preserves_scaled_values_with_additional_uses() {
        let mut instructions = vec![
            multiply64("product", "index", Operand::Immediate(4)),
            move64("address", "base"),
            add64("address", register("product")),
            assert_nonzero("product"),
        ];

        fuse_scaled_addresses(&mut instructions);

        assert_eq!(
            instructions[0].opcode.operation(),
            Operation::IntegerBinary {
                operation: IntegerBinaryOperation::Binary(BinaryOperation::Multiply),
                width: IntegerWidth::U64,
            },
        );
        assert_eq!(instructions.len(), 4);
    }

    #[test]
    fn folds_indexed_stores_into_offset_addresses() {
        let mut instructions = vec![
            load_effective_address(
                "address",
                address(
                    "base",
                    None,
                    None,
                    Some(AddressDisplacement::LayoutConstant(test_layout_constant(
                        "FIELD_OFFSET",
                        24,
                    ))),
                ),
            ),
            store64(address("address", Some("index"), Some(8), None), "value"),
        ];

        fuse_indexed_offset_stores(&mut instructions);

        assert_eq!(instructions.len(), 1);
        assert_eq!(instructions[0].opcode.operation(), Operation::Store64IndexedOffset,);
        assert_eq!(
            instructions[0].operands,
            [
                register("base"),
                register("index"),
                Operand::Immediate(8),
                Operand::LayoutConstant(test_layout_constant("FIELD_OFFSET", 24,)),
                register("value"),
            ]
        );
    }
}
