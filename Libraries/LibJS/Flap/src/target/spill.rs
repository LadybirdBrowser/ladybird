/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Spilling for handlers that do not fit in the allocatable registers.
//!
//! A runtime that keeps a spill area at a fixed offset from the machine stack pointer describes it with the
//! SPILL_AREA_OFFSET and SPILL_AREA_SLOTS layout constants. Without them, allocation stays spill-free and fails
//! instead. A spilled value lives in its slot: every instruction that reads it loads it into a fresh register first,
//! and every instruction that writes it stores that register afterwards. The slot is addressed from the stack pointer,
//! so a value that is live across a write of a register to `sp` moves to the same slot from the new stack pointer
//! right before the write. Values move in the order they were spilled, which is the order of their slots, so that is
//! only safe when the write lowers `sp`, which is what moving into a new frame does.

use crate::frontend::layout::KnownLayoutConstant;
use crate::low_ir::analysis::virtual_register_access;
use crate::low_ir::{AddressDisplacement, AddressRegister, MemoryAddress, VirtualRegister};
use crate::target::description::{MemoryWidth, OperandKind, Operation};
use crate::target::ir::{Function, Instruction, Operand, RuntimeConstants};
use crate::types::{InterpreterRegister, RegisterClass};

#[derive(Debug, Clone, Copy)]
pub(crate) struct SpillArea {
    pub(crate) offset: i64,
    pub(crate) slots: u32,
}

impl SpillArea {
    pub(crate) fn of(runtime: &RuntimeConstants) -> Option<Self> {
        let offset = runtime.get(KnownLayoutConstant::SpillAreaOffset)?;
        let slots = runtime.get(KnownLayoutConstant::SpillAreaSlots)?;
        Some(Self {
            offset,
            slots: u32::try_from(slots).ok()?,
        })
    }
}

#[derive(Debug, Default, Clone)]
struct Candidate {
    first: usize,
    last: usize,
    occurrences: usize,
    definitions: usize,
    excluded: bool,
    /// The interpreter register the value is a copy of, if its only definition copies one.
    copy_of: Option<InterpreterRegister>,
}

fn writes_interpreter_register(instruction: &Instruction, register: InterpreterRegister) -> bool {
    let description = instruction.opcode.description();
    let count = instruction.operands.len();
    instruction.operands.iter().enumerate().any(|(position, operand)| {
        *operand == Operand::InterpreterRegister(register)
            && matches!(
                description.operand_kind(position, count),
                Some(OperandKind::GprOut | OperandKind::GprInOut | OperandKind::RegisterOut)
            )
    })
}

fn writes_stack_pointer(instruction: &Instruction) -> bool {
    writes_interpreter_register(instruction, InterpreterRegister::StackPointer)
}

/// Give every operand that must be in a particular register a virtual register of its own, connected to the value by
/// moves, so that the value itself is free to live elsewhere. Returns false if there was no such operand.
pub(crate) fn isolate_fixed_operands(function: &mut Function) -> bool {
    let architecture = function.architecture;
    let instructions = std::mem::take(&mut function.instructions);
    let mut rewritten = Vec::with_capacity(instructions.len());
    let mut changed = false;
    for (index, mut instruction) in instructions.into_iter().enumerate() {
        let fixed = instruction.opcode.description().arch(architecture).fixed_operands;
        if fixed.is_empty() {
            rewritten.push(instruction);
            continue;
        }
        let access = virtual_register_access(&instruction, Some(architecture));
        let mut after = Vec::new();
        for &(position, _) in fixed {
            let Some(Operand::VirtualRegister(register)) = instruction.operands.get(position).cloned() else {
                continue;
            };
            if register.class() != RegisterClass::GeneralPurpose {
                continue;
            }
            let isolated = VirtualRegister::interned(
                format!("fixed_{index}_{position}"),
                function.id,
                function.virtual_registers.len() as u32,
                register.class(),
            );
            function.virtual_registers.push(isolated.clone());
            let move_instruction = |destination: &VirtualRegister, source: &VirtualRegister| {
                let mut operands = vec![
                    Operand::VirtualRegister(destination.clone()),
                    Operand::VirtualRegister(source.clone()),
                ];
                let opcode = Operation::Move(crate::target::description::IntegerWidth::U64)
                    .select_for_operands(architecture, &operands);
                opcode.add_contract_operands(&mut operands);
                Instruction { opcode, operands }
            };
            if access.reads.contains(&register) {
                rewritten.push(move_instruction(&isolated, &register));
            }
            if access.definitions.contains(&register) {
                after.push(move_instruction(&register, &isolated));
            }
            instruction.operands[position] = Operand::VirtualRegister(isolated);
            changed = true;
        }
        rewritten.push(instruction);
        rewritten.extend(after);
    }
    function.instructions = rewritten;
    changed
}

/// Rewrite one virtual register of `class` in `function` to live in the spill area from slot `next_slot` on, which
/// then moves past the slots it took. A vector takes two slots, starting on a 16-byte boundary. Returns false if no
/// register of that class can be spilled, or its slots do not fit.
pub(crate) fn spill_one_register(
    function: &mut Function,
    area: &SpillArea,
    next_slot: &mut u32,
    class: RegisterClass,
) -> bool {
    let mut slot = *next_slot;
    let slot_count = if class == RegisterClass::Vector { 2 } else { 1 };
    if class == RegisterClass::Vector && (area.offset + i64::from(slot) * 8) % 16 != 0 {
        slot += 1;
    }
    let slots_left = slot + slot_count <= area.slots;
    let architecture = function.architecture;
    let mut candidates = vec![Candidate::default(); function.virtual_registers.len()];
    let mut stack_pointer_writes = Vec::new();
    // The register each write to `sp` copies, if it is a plain move of one.
    let mut stack_pointer_sources = Vec::new();
    let mut calls = Vec::new();
    for (index, instruction) in function.instructions.iter().enumerate() {
        if writes_stack_pointer(instruction) {
            stack_pointer_writes.push(index);
            let source = match (instruction.opcode.operation(), instruction.operands.as_slice()) {
                (
                    Operation::Move(crate::target::description::IntegerWidth::U64),
                    [
                        Operand::InterpreterRegister(InterpreterRegister::StackPointer),
                        Operand::VirtualRegister(source),
                    ],
                ) => Some(AddressRegister::Virtual(source.clone())),
                (
                    Operation::Move(crate::target::description::IntegerWidth::U64),
                    [
                        Operand::InterpreterRegister(InterpreterRegister::StackPointer),
                        Operand::InterpreterRegister(source),
                    ],
                ) => Some(AddressRegister::Interpreter(*source)),
                _ => None,
            };
            stack_pointer_sources.push(source);
        }
        if instruction.opcode.description().is_call {
            calls.push(index);
        }
        let has_label = instruction
            .operands
            .iter()
            .any(|operand| matches!(operand, Operand::Label(_)));
        let access = virtual_register_access(instruction, Some(architecture));
        let copied_register = match (instruction.opcode.operation(), instruction.operands.as_slice()) {
            (
                Operation::Move(crate::target::description::IntegerWidth::U64),
                [Operand::VirtualRegister(_), Operand::InterpreterRegister(register)],
            ) => Some(*register),
            _ => None,
        };
        for register in &access.definitions {
            if let Some(id) = register.id() {
                let candidate = &mut candidates[id.index() as usize];
                candidate.definitions += 1;
                candidate.copy_of = copied_register;
            }
        }
        for operand in &instruction.operands {
            let mut note = |register: &VirtualRegister, excluded: bool| {
                let Some(id) = register.id() else {
                    return;
                };
                let candidate = &mut candidates[id.index() as usize];
                if candidate.occurrences == 0 {
                    candidate.first = index;
                }
                candidate.last = index;
                candidate.occurrences += 1;
                candidate.excluded |= excluded;
            };
            match operand {
                Operand::VirtualRegister(register) => {
                    let defines_on_a_branch = has_label && access.definitions.contains(register);
                    note(register, defines_on_a_branch);
                }
                Operand::Address(address) => {
                    for register in [&address.base].into_iter().chain(address.index.as_ref()) {
                        if let AddressRegister::Virtual(register) = register {
                            note(register, false);
                        }
                    }
                }
                _ => {}
            }
        }
    }

    // A copy of an interpreter register can be reread from that register instead of a slot, as long as nothing
    // writes the register before the copy's last use.
    let rematerializes = |candidate: &Candidate| {
        candidate.definitions == 1
            && candidate.copy_of.is_some_and(|register| {
                function.instructions[candidate.first..=candidate.last]
                    .iter()
                    .all(|instruction| !writes_interpreter_register(instruction, register))
            })
    };
    let live_after = crate::target::allocator::live_after_each_instruction(function).unwrap_or_default();
    let live_after_instruction = |instruction: usize, register: usize| {
        live_after
            .get(instruction)
            .is_some_and(|live| live.binary_search(&(register as u32)).is_ok())
    };
    let mut best: Option<(usize, u64)> = None;
    for (index, candidate) in candidates.iter().enumerate() {
        let register = &function.virtual_registers[index];
        let rematerialized = rematerializes(candidate);
        if candidate.excluded
            || (!slots_left && !rematerialized)
            || candidate.occurrences == 0
            || register.class() != class
            || register.to_string().starts_with("spill_")
            || (!rematerialized
                && stack_pointer_writes
                    .iter()
                    .zip(&stack_pointer_sources)
                    .any(|(&write, source)| {
                        // The new stack pointer must be at hand to move the value, so it cannot be the value itself.
                        live_after_instruction(write, index)
                            && source.as_ref().is_none_or(
                                |source| matches!(source, AddressRegister::Virtual(source) if source == register),
                            )
                    }))
        {
            continue;
        }
        // Prefer values live across a call, which clobbers every register they could be in, then copies of
        // interpreter registers, which cost nothing to reread, then long-lived values that are rarely used: spilling
        // them frees a register for longest at the cost of the fewest loads and stores.
        let mut score = ((candidate.last - candidate.first) as u64 * 1024) / candidate.occurrences as u64;
        if rematerialized {
            score += 1 << 40;
        }
        if calls.iter().any(|&call| live_after_instruction(call, index)) {
            score += 1 << 41;
        }
        // Moving a value into a new frame costs a load and a store at every write to sp it lives across.
        if !rematerialized
            && stack_pointer_writes
                .iter()
                .any(|&write| live_after_instruction(write, index))
        {
            score /= 4;
        }
        if best.is_none_or(|(_, best_score)| score > best_score) {
            best = Some((index, score));
        }
    }
    let Some((victim_index, _)) = best else {
        return false;
    };
    let victim = function.virtual_registers[victim_index].clone();
    let rematerialized_register = rematerializes(&candidates[victim_index])
        .then(|| candidates[victim_index].copy_of)
        .flatten();
    // A value that is reread from its interpreter register needs no slot.
    if rematerialized_register.is_none() {
        *next_slot = slot + slot_count;
    }

    let slot_address = Operand::Address(MemoryAddress {
        base: AddressRegister::Interpreter(InterpreterRegister::StackPointer),
        index: None,
        scale: None,
        displacement: Some(AddressDisplacement::Immediate(area.offset + i64::from(slot) * 8)),
    });
    let slot_width = match class {
        RegisterClass::GeneralPurpose => MemoryWidth::DoubleWord,
        RegisterClass::FloatingPoint => MemoryWidth::Double,
        RegisterClass::Vector => MemoryWidth::Vector,
    };
    let instructions = std::mem::take(&mut function.instructions);
    let mut rewritten = Vec::with_capacity(instructions.len() + 8);
    for (index, mut instruction) in instructions.into_iter().enumerate() {
        // Before a write to `sp` the value is live across, move it to its slot in the new frame.
        let moves_frames = rematerialized_register.is_none() && live_after_instruction(index, victim_index);
        if moves_frames && let Some(position) = stack_pointer_writes.iter().position(|&write| write == index) {
            let new_stack_pointer = stack_pointer_sources[position]
                .clone()
                .expect("only values live across moves into sp are spilled");
            let new_slot = Operand::Address(MemoryAddress {
                base: new_stack_pointer,
                index: None,
                scale: None,
                displacement: Some(AddressDisplacement::Immediate(area.offset + i64::from(slot) * 8)),
            });
            let carrier = VirtualRegister::interned(
                format!("spill_{slot}_{index}_move"),
                function.id,
                function.virtual_registers.len() as u32,
                class,
            );
            function.virtual_registers.push(carrier.clone());
            let mut operands = vec![Operand::VirtualRegister(carrier.clone()), slot_address.clone()];
            let opcode = Operation::load(slot_width, false).select_for_operands(architecture, &operands);
            opcode.add_contract_operands(&mut operands);
            rewritten.push(Instruction { opcode, operands });
            let mut operands = vec![new_slot, Operand::VirtualRegister(carrier)];
            let opcode = Operation::store(slot_width).select_for_operands(architecture, &operands);
            opcode.add_contract_operands(&mut operands);
            rewritten.push(Instruction { opcode, operands });
        }
        let access = virtual_register_access(&instruction, Some(architecture));
        let reads = access.reads.contains(&victim)
            || instruction.operands.iter().any(|operand| {
                matches!(operand, Operand::Address(address)
                if [&address.base].into_iter().chain(address.index.as_ref()).any(|register| {
                    matches!(register, AddressRegister::Virtual(register) if *register == victim)
                }))
            });
        let defines = access.definitions.contains(&victim);
        if !reads && !defines {
            rewritten.push(instruction);
            continue;
        }
        let replacement = VirtualRegister::interned(
            format!("spill_{slot}_{index}"),
            function.id,
            function.virtual_registers.len() as u32,
            class,
        );
        function.virtual_registers.push(replacement.clone());
        for operand in &mut instruction.operands {
            match operand {
                Operand::VirtualRegister(register) if *register == victim => *register = replacement.clone(),
                Operand::Address(address) => {
                    for register in std::iter::once(&mut address.base).chain(address.index.as_mut()) {
                        if matches!(register, AddressRegister::Virtual(register) if *register == victim) {
                            *register = AddressRegister::Virtual(replacement.clone());
                        }
                    }
                }
                _ => {}
            }
        }
        if let Some(register) = rematerialized_register {
            if defines {
                // The copy itself goes away; every read rereads the register.
                continue;
            }
            let mut operands = vec![
                Operand::VirtualRegister(replacement.clone()),
                Operand::InterpreterRegister(register),
            ];
            let opcode = Operation::Move(crate::target::description::IntegerWidth::U64)
                .select_for_operands(architecture, &operands);
            opcode.add_contract_operands(&mut operands);
            rewritten.push(Instruction { opcode, operands });
            rewritten.push(instruction);
            continue;
        }
        if reads {
            let mut operands = vec![Operand::VirtualRegister(replacement.clone()), slot_address.clone()];
            let opcode = Operation::load(slot_width, false).select_for_operands(architecture, &operands);
            opcode.add_contract_operands(&mut operands);
            rewritten.push(Instruction { opcode, operands });
        }
        rewritten.push(instruction);
        if defines {
            let mut operands = vec![slot_address.clone(), Operand::VirtualRegister(replacement)];
            let opcode = Operation::store(slot_width).select_for_operands(architecture, &operands);
            opcode.add_contract_operands(&mut operands);
            rewritten.push(Instruction { opcode, operands });
        }
    }
    function.instructions = rewritten;
    true
}
