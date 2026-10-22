/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The register allocation verifier. It simulates what every register holds through each block, from the state the
//! allocator says the block starts with, and checks that every input is where the allocation says, and that every
//! edge's moves turn the state at the end of its source into the state its target starts with (with phis replaced
//! by their inputs). A value's slot is written where it is defined (a phi's where its block starts, or by the moves
//! of its edges), and values that share a slot must have intervals that do not overlap, so that a slot that the
//! block has not written holds the one of its values that is live there.

use super::{Allocation, CONSTANT_INPUT, Location, NO_REGISTER, REGISTER_COUNT, SLOT_INPUT, call_result_types};
use crate::convention::{self, Place};
use crate::ir::{BlockId, Func, NONE, Op, Shape, Value};
use crate::regalloc::liveness::{Liveness, block_start_position, edge_position, node_position, predecessor_index};
use std::collections::HashMap;

struct Simulation<'a> {
    liveness: &'a Liveness,
    registers: [u32; REGISTER_COUNT],
    /// What the block wrote to slots so far.
    slots: HashMap<i32, Value>,
    /// The values of each slot.
    owners: HashMap<i32, Vec<Value>>,
    /// The position the simulation is at.
    position: u32,
    errors: Vec<String>,
}

impl Simulation<'_> {
    fn error(&mut self, message: String) {
        if self.errors.len() < 20 {
            self.errors.push(message);
        }
    }

    /// The value `location` holds, as far as the simulation knows.
    fn read(&self, location: Location) -> Option<Value> {
        match location {
            Location::Register(register) => Some(self.registers[register as usize]).filter(|value| *value != NONE),
            Location::Slot(slot) => self.slots.get(&slot).copied().or_else(|| self.owner_at(slot)),
            Location::Constant(value) => Some(value),
            Location::Outgoing(_) | Location::ResultArea(_) => None,
        }
    }

    /// The value of `slot` that is live at the current position, which a slot that the block did not write holds.
    fn owner_at(&self, slot: i32) -> Option<Value> {
        let owners = self.owners.get(&slot)?;
        if let [owner] = owners.as_slice() {
            return Some(*owner);
        }
        owners.iter().copied().find(|owner| {
            self.liveness.definition[*owner as usize] < self.position
                && self.liveness.end[*owner as usize] >= self.position
        })
    }

    fn apply(&mut self, moves: &[super::Move]) {
        for item in moves {
            let value = self.read(item.from).unwrap_or(NONE);
            match item.to {
                Location::Register(register) => self.registers[register as usize] = value,
                Location::Slot(slot) => {
                    self.slots.insert(slot, value);
                }
                _ => {}
            }
        }
    }

    fn check_holds(&mut self, location: Location, value: Value, what: &str) {
        let ok = match location {
            Location::Register(register) => self.registers[register as usize] == value,
            Location::Slot(slot) => self.read(Location::Slot(slot)) == Some(value),
            Location::Constant(constant) => constant == value,
            _ => true,
        };
        if !ok {
            self.error(format!("{what}: v{value} is not in {location:?}"));
        }
    }
}

pub fn verify(func: &Func, allocation: &Allocation) -> Result<(), String> {
    let mut liveness = Liveness::default();
    liveness.compute(func);
    let mut simulation = Simulation {
        liveness: &liveness,
        registers: [NONE; REGISTER_COUNT],
        slots: HashMap::new(),
        owners: HashMap::new(),
        position: 0,
        errors: Vec::new(),
    };
    for (value, &slot) in allocation.slots.iter().enumerate() {
        if slot != super::NO_SLOT {
            simulation.owners.entry(slot).or_default().push(value as Value);
        }
    }
    // The values of a slot follow each other: none is defined while another one is live.
    let mut overlaps = Vec::new();
    for (slot, owners) in &mut simulation.owners {
        owners.sort_by_key(|owner| liveness.definition[*owner as usize]);
        for pair in owners.windows(2) {
            if liveness.end[pair[0] as usize] > liveness.definition[pair[1] as usize] {
                overlaps.push(format!(
                    "v{} and v{} share [fp{slot:+}] while both are live",
                    pair[0], pair[1]
                ));
            }
        }
    }
    for overlap in overlaps {
        simulation.error(overlap);
    }
    let mut places = Vec::new();
    for block in 0..func.blocks.len() as BlockId {
        simulation.registers = allocation.block_states[block as usize];
        simulation.slots.clear();
        simulation.position = block_start_position(func, block);
        for phi in func.phis(block) {
            let slot = allocation.slots[phi as usize];
            if slot != super::NO_SLOT {
                simulation.slots.insert(slot, phi);
            }
        }
        let data = func.blocks[block as usize];
        for value in data.start..data.end {
            let node = *func.node(value);
            if node.op.is_entry_value() {
                continue;
            }
            let record = *allocation.node(value);
            simulation.position = node_position(value);
            simulation.apply(allocation.node_moves(value));
            let inputs = func.inputs(value);
            match node.op {
                Op::Helper => {
                    for (index, &input) in inputs.iter().enumerate() {
                        let register = convention::HELPER_REGISTERS[index];
                        simulation.check_holds(Location::Register(register), input, &format!("helper v{value}"));
                    }
                    for register in 0..REGISTER_COUNT {
                        if convention::HOST_CLOBBERS & (1 << register) != 0 {
                            simulation.registers[register] = NONE;
                        }
                    }
                }
                Op::Call | Op::TailCall => {
                    let arguments = &inputs[3..];
                    let types = arguments
                        .iter()
                        .map(|argument| func.node(*argument).ty)
                        .collect::<Vec<_>>();
                    convention::parameter_places(&types, &mut places);
                    for (index, &argument) in arguments.iter().enumerate() {
                        if let Place::Register(register) = places[index] {
                            simulation.check_holds(Location::Register(register), argument, &format!("call v{value}"));
                        }
                    }
                    for (input, register) in [
                        (inputs[0], convention::CALLEE_REGISTER),
                        (inputs[1], convention::INSTANCE_REGISTER),
                        (inputs[2], convention::MEMORY_BASE_REGISTER),
                    ] {
                        simulation.check_holds(Location::Register(register), input, &format!("call v{value}"));
                    }
                    simulation.registers = [NONE; REGISTER_COUNT];
                    let _ = call_result_types(func, value);
                }
                Op::Return => {
                    convention::result_places(&func.results, &mut places);
                    for (index, &result) in inputs.iter().enumerate() {
                        if let Place::Register(register) = places[index] {
                            simulation.check_holds(Location::Register(register), result, &format!("return v{value}"));
                        }
                    }
                }
                _ => {
                    let count = match node.op.shape() {
                        Shape::Fixed(count) => count as usize,
                        Shape::Variadic => inputs.len().min(3),
                    };
                    for (index, &input) in inputs.iter().enumerate().take(count) {
                        let register = record.inputs[index];
                        if register == SLOT_INPUT {
                            let slot = allocation.slots[input as usize];
                            simulation.check_holds(Location::Slot(slot), input, &format!("v{value} input {index}"));
                            continue;
                        }
                        if register == CONSTANT_INPUT {
                            if !func.node(input).op.is_constant() {
                                simulation.error(format!("v{value} takes v{input} as an immediate"));
                            }
                            continue;
                        }
                        if func.node(input).op.is_constant() {
                            // Materialized into the register by the moves before the node.
                            continue;
                        }
                        simulation.check_holds(Location::Register(register), input, &format!("v{value} input {index}"));
                    }
                    for temp in record.temps {
                        if temp != NO_REGISTER {
                            simulation.registers[temp as usize] = NONE;
                        }
                    }
                    #[cfg(target_arch = "x86_64")]
                    if matches!(node.op, Op::DivS | Op::DivU | Op::RemS | Op::RemU) {
                        simulation.registers[convention::RAX as usize] = NONE;
                        simulation.registers[convention::RDX as usize] = NONE;
                    }
                }
            }
            if record.output != NO_REGISTER {
                simulation.registers[record.output as usize] = value;
            }
            let slot = allocation.slots[value as usize];
            if slot != super::NO_SLOT {
                simulation.slots.insert(slot, value);
            }
        }
        // The edges.
        simulation.position = edge_position(func, block);
        let end_registers = simulation.registers;
        let end_slots = simulation.slots.clone();
        let successors_start = data.successors_start;
        for (index, &successor) in func.successors(block).iter().enumerate() {
            simulation.registers = end_registers;
            simulation.slots = end_slots.clone();
            simulation.apply(allocation.edge(successors_start + index as u32));
            let predecessor = predecessor_index(func, successor, block);
            let target = allocation.block_states[successor as usize];
            for (register, &value) in target.iter().enumerate() {
                if value == NONE {
                    continue;
                }
                let expected = if func.node(value).op == Op::Phi && func.phis(successor).any(|phi| phi == value) {
                    func.inputs(value)[predecessor]
                } else {
                    value
                };
                let held = simulation.registers[register];
                if held != expected {
                    simulation.error(format!(
                        "edge b{block} -> b{successor}: r{register} holds v{held}, not v{expected}"
                    ));
                }
            }
            for phi in func.phis(successor) {
                if allocation.node(phi).output != NO_REGISTER {
                    continue;
                }
                let input = func.inputs(phi)[predecessor];
                let slot = allocation.slots[phi as usize];
                if simulation.read(Location::Slot(slot)) != Some(input) {
                    simulation.error(format!(
                        "edge b{block} -> b{successor}: the slot of phi v{phi} lacks v{input}"
                    ));
                }
            }
        }
    }
    if simulation.errors.is_empty() {
        Ok(())
    } else {
        Err(simulation.errors.join("\n"))
    }
}

/// The allocation as text, for `dump-ir` and failures.
pub fn dump(func: &Func, allocation: &Allocation) -> String {
    use std::fmt::Write;
    let name = |location: &Location| match location {
        Location::Register(register) if convention::is_fpr(*register) => {
            format!("xmm{}", register - convention::FPR_BASE)
        }
        Location::Register(register) => format!("r{register}"),
        Location::Slot(slot) => format!("[fp{slot:+}]"),
        Location::Outgoing(index) => format!("out{index}"),
        Location::ResultArea(index) => format!("result{index}"),
        Location::Constant(value) => format!("#v{value}"),
    };
    let register = |register: u8| match register {
        NO_REGISTER => "-".to_string(),
        CONSTANT_INPUT => "#".to_string(),
        SLOT_INPUT => "[slot]".to_string(),
        _ => name(&Location::Register(register)),
    };
    let moves = |moves: &[super::Move]| {
        moves
            .iter()
            .map(|item| format!("{} -> {}", name(&item.from), name(&item.to)))
            .collect::<Vec<_>>()
            .join(", ")
    };
    let mut out = String::new();
    for block in 0..func.blocks.len() as BlockId {
        writeln!(out, "b{block}:").unwrap();
        for phi in func.phis(block) {
            let record = allocation.node(phi);
            writeln!(
                out,
                "  v{phi} phi -> {} slot {}",
                register(record.output),
                allocation.slots[phi as usize]
            )
            .unwrap();
        }
        let data = func.blocks[block as usize];
        for value in data.start..data.end {
            let node = func.node(value);
            if node.op.is_entry_value() {
                continue;
            }
            let record = allocation.node(value);
            if record.moves_count > 0 {
                writeln!(out, "  moves {}", moves(allocation.node_moves(value))).unwrap();
            }
            let inputs = record.inputs.iter().map(|input| register(*input)).collect::<Vec<_>>();
            write!(out, "  v{value} {} ({})", node.op.name(), inputs.join(", ")).unwrap();
            if record.output != NO_REGISTER {
                write!(out, " -> {}", register(record.output)).unwrap();
            }
            if allocation.slots[value as usize] != super::NO_SLOT {
                write!(out, " spill [fp{:+}]", allocation.slots[value as usize]).unwrap();
            }
            out.push('\n');
        }
        for (index, &successor) in func.successors(block).iter().enumerate() {
            let edge = allocation.edge(data.successors_start + index as u32);
            if !edge.is_empty() {
                writeln!(out, "  edge -> b{successor}: {}", moves(edge)).unwrap();
            }
        }
    }
    writeln!(
        out,
        "slots: {}, outgoing: {}",
        allocation.slot_count, allocation.outgoing_size
    )
    .unwrap();
    out
}
