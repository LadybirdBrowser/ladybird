/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Checks a register allocation by simulating what every location holds.
//!
//! The verifier walks the allocated program and tracks, for every register
//! and stack slot, which value it holds on every path reaching each point.
//! Every input, phi input and exit value must be found in the location the
//! allocation claims, no matter which path led there.

use super::Allocation;
use super::Location;
use super::Move;
use super::RegisterSet;
use super::liveness::phi_inputs_from;
use crate::ir::BlockId;
use crate::ir::Graph;
use crate::ir::NodeId;
use crate::ir::Op;
use std::collections::BTreeMap;

/// What a location holds.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum Content {
    Value(NodeId),
    Constant(u64),
}

type State = BTreeMap<Location, Content>;

struct Verifier<'a> {
    graph: &'a Graph,
    registers: &'a RegisterSet,
    allocation: &'a Allocation,
    /// Whether to report errors; off while the block states are still
    /// being computed.
    checking: bool,
    errors: Vec<String>,
}

/// Checks `allocation` against `graph`, returning a description of every
/// problem found.
pub fn verify(graph: &Graph, registers: &RegisterSet, allocation: &Allocation) -> Result<(), String> {
    let mut verifier = Verifier {
        graph,
        registers,
        allocation,
        checking: false,
        errors: Vec::new(),
    };
    let block_count = graph.blocks.len();
    let block_ids = (0..block_count).map(BlockId::from_index).collect::<Vec<_>>();

    // Compute the state at the start of every block: what holds on all
    // paths, starting optimistically from the predecessors seen so far.
    let mut entry_states: Vec<Option<State>> = vec![None; block_count];
    let mut exit_states: Vec<Option<State>> = vec![None; block_count];
    let mut changed = true;
    while changed {
        changed = false;
        for block in &block_ids {
            let Some(entry) = verifier.entry_state(*block, &exit_states) else {
                continue;
            };
            if entry_states[block.index()].as_ref() == Some(&entry) && exit_states[block.index()].is_some() {
                continue;
            }
            let exit = verifier.simulate_block(*block, entry.clone());
            entry_states[block.index()] = Some(entry);
            exit_states[block.index()] = Some(exit);
            changed = true;
        }
    }

    verifier.checking = true;
    for block in &block_ids {
        match &entry_states[block.index()] {
            Some(entry) => {
                verifier.check_phi_inputs(*block, &exit_states);
                verifier.simulate_block(*block, entry.clone());
            }
            None => verifier.errors.push(format!("b{} is unreachable", block.0)),
        }
    }

    if verifier.errors.is_empty() {
        Ok(())
    } else {
        Err(verifier.errors.join("\n"))
    }
}

impl Verifier<'_> {
    fn error(&mut self, message: String) {
        if self.checking {
            self.errors.push(message);
        }
    }

    fn content_of_value(&self, value: NodeId) -> Content {
        if matches!(self.graph.node(value).op, Op::VirtualObject { .. }) {
            return Content::Constant(crate::ir::value::EMPTY);
        }
        match self.graph.constant_value(value) {
            Some(bits) => Content::Constant(bits),
            None => Content::Value(value),
        }
    }

    fn read(state: &State, location: Location) -> Option<Content> {
        match location {
            Location::Constant(bits) => Some(Content::Constant(bits)),
            _ => state.get(&location).copied(),
        }
    }

    /// The state at the start of `block`: what holds at the end of all its
    /// predecessors seen so far, with the phis in place. `None` if no
    /// predecessor was seen yet.
    fn entry_state(&self, block: BlockId, exit_states: &[Option<State>]) -> Option<State> {
        // NB: The entry and the on-stack replacement entries start with
        //     every value in the frame.
        let block_data = self.graph.block(block);
        if block.0 == 0 || block_data.predecessors.is_empty() {
            return Some(State::new());
        }
        let mut result: Option<State> = None;
        for predecessor in &block_data.predecessors {
            let Some(exit) = &exit_states[predecessor.index()] else {
                continue;
            };
            let mut state = exit.clone();
            for (phi, input) in phi_inputs_from(self.graph, *predecessor, block) {
                let location = self.allocation.node(phi).output.expect("phis have a location");
                if Self::read(&state, location) == Some(self.content_of_value(input)) {
                    state.insert(location, Content::Value(phi));
                } else {
                    state.remove(&location);
                }
            }
            result = Some(match result {
                None => state,
                Some(previous) => previous
                    .into_iter()
                    .filter(|(location, content)| state.get(location) == Some(content))
                    .collect(),
            });
        }
        result
    }

    fn check_phi_inputs(&mut self, block: BlockId, exit_states: &[Option<State>]) {
        for predecessor in self.graph.block(block).predecessors.clone() {
            let Some(exit) = &exit_states[predecessor.index()] else {
                continue;
            };
            for (phi, input) in phi_inputs_from(self.graph, predecessor, block) {
                let location = self.allocation.node(phi).output.expect("phis have a location");
                let found = Self::read(exit, location);
                if found != Some(self.content_of_value(input)) {
                    self.error(format!(
                        "phi v{} in b{}: input from b{} should be {:?} in {location:?}, found {found:?}",
                        phi.0,
                        block.0,
                        predecessor.0,
                        self.content_of_value(input)
                    ));
                }
            }
        }
    }

    fn apply_moves(&mut self, state: &mut State, moves: &[Move], context: &str) {
        for m in moves {
            if matches!(m.to, Location::Constant(_)) {
                self.error(format!("{context}: move into a constant"));
                continue;
            }
            self.check_location(m.to, context);
            match Self::read(state, m.from) {
                Some(content) => {
                    state.insert(m.to, content);
                }
                None => {
                    self.error(format!("{context}: move reads {:?}, which holds nothing", m.from));
                    state.remove(&m.to);
                }
            }
        }
    }

    fn check_location(&mut self, location: Location, context: &str) {
        if let Location::Register(register) = location
            && !self
                .registers
                .allocatable(super::RegisterClass::General)
                .contains(register)
            && !self
                .registers
                .allocatable(super::RegisterClass::Float)
                .contains(register)
        {
            self.error(format!("{context}: r{register} is not allocatable"));
        }
    }

    fn expect(&mut self, state: &State, location: Location, value: NodeId, context: &str) {
        let expected = self.content_of_value(value);
        let found = Self::read(state, location);
        if found != Some(expected) {
            self.error(format!(
                "{context}: expected {expected:?} in {location:?}, found {found:?}"
            ));
        }
    }

    fn check_exit_values(&mut self, state: &State, node: NodeId, context: &str) {
        let Some(frame_state) = self.graph.node(node).frame_state else {
            return;
        };
        let expected = &self.graph.frame_state_values(frame_state);
        let exit_values = &self.allocation.node(node).exit_values;
        if expected.len() != exit_values.len() {
            self.error(format!(
                "{context}: exit has {} values, frame state {}",
                exit_values.len(),
                expected.len()
            ));
            return;
        }
        for ((slot, value), (exit_slot, location)) in expected.iter().zip(exit_values) {
            if slot != exit_slot {
                self.error(format!(
                    "{context}: exit value for slot {exit_slot}, frame state slot {slot}"
                ));
            }
            self.expect(
                state,
                *location,
                *value,
                &format!("{context}, exit value for slot {slot}"),
            );
        }
    }

    fn simulate_block(&mut self, block: BlockId, mut state: State) -> State {
        let block_data = self.graph.block(block);
        // Phis in registers are stored to their spill slots first.
        for phi in &block_data.phis {
            let allocation = self.allocation.node(*phi);
            if let (Some(Location::Register(_)), Some(slot)) = (allocation.output, allocation.spill_slot) {
                state.insert(Location::Stack(slot), Content::Value(*phi));
            }
        }
        for node in &block_data.body {
            self.simulate_node(&mut state, *node);
        }
        let end_moves = &self.allocation.block_end_moves[block.index()];
        self.apply_moves(&mut state, end_moves, &format!("end of b{}", block.0));
        self.simulate_node(&mut state, self.graph.control_id(block));
        state
    }

    fn simulate_node(&mut self, state: &mut State, node_id: NodeId) {
        let node = self.graph.node(node_id);
        let allocation = self.allocation.node(node_id);
        let context = format!("v{}", node_id.0);
        debug_assert!(node.op != Op::Phi);

        self.apply_moves(state, &allocation.moves_before, &context);
        if allocation.inputs.len() != node.inputs.len() {
            self.error(format!("{context}: wrong number of input locations"));
            return;
        }
        for (index, (input, location)) in node.inputs.iter().zip(&allocation.inputs).enumerate() {
            if let Location::Constant(bits) = location {
                if self.graph.constant_value(*input) != Some(*bits) {
                    self.error(format!("{context}: input {index} is not the constant {bits:#x}"));
                }
                continue;
            }
            // NB: Slow path calls, and calls that store their operands, read
            //     them from where they are.
            let reads_inputs_anywhere = matches!(
                node.op,
                Op::CallSlowPath { .. }
                    | Op::CallDirect {
                        stores_operands: true,
                        ..
                    }
                    | Op::CallNative {
                        stores_operands: true,
                        ..
                    }
                    | Op::SlowPathOutput { .. }
            );
            if !matches!(location, Location::Register(_)) && !reads_inputs_anywhere {
                self.error(format!("{context}: input {index} is not in a register"));
            }
            self.check_location(*location, &context);
            self.expect(state, *location, *input, &format!("{context}, input {index}"));
        }

        // A node may exit at any point after reading its inputs, including
        // after it wrote its temps and its output: exit values must survive both.
        let properties = node.op.properties();
        for temp in &allocation.temps {
            self.check_location(Location::Register(*temp), &context);
            if allocation.inputs.contains(&Location::Register(*temp)) {
                self.error(format!("{context}: temp r{temp} is also an input"));
            }
            state.remove(&Location::Register(*temp));
        }
        if properties.is_call {
            for register in self.registers.caller_saved().iter() {
                state.remove(&Location::Register(register));
            }
        } else if super::saves_only_gprs(&node.op) {
            for register in self.registers.allocatable(super::RegisterClass::Float).iter() {
                state.remove(&Location::Register(register));
            }
        }
        if let Some(output @ Location::Register(_)) = allocation.output {
            let mut clobbered = state.clone();
            clobbered.remove(&output);
            self.check_exit_values(&clobbered, node_id, &context);
        } else {
            self.check_exit_values(state, node_id, &context);
        }

        match (node.repr.is_some(), allocation.output) {
            (true, Some(Location::Register(register))) => {
                self.check_location(Location::Register(register), &context);
                if super::RegisterClass::of_register(register) != super::RegisterClass::of(node.repr) {
                    self.error(format!("{context}: the output is in a register of the other class"));
                }
                state.insert(Location::Register(register), Content::Value(node_id));
                if let Some(slot) = allocation.spill_slot {
                    state.insert(Location::Stack(slot), Content::Value(node_id));
                }
            }
            (false, None) => {}
            (_, output) => self.error(format!("{context}: unexpected output location {output:?}")),
        }
    }
}
