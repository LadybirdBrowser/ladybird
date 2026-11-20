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
//!
//! A slot of the compiled function's frame that nothing writes (see
//! `frame_home()`) holds the value the function started with everywhere, and
//! nothing on any path may store to it. Every load of such a slot loads that
//! value, so the state names it by the slot rather than by a load.

use ak::Zone;
use ak::ZoneVec;
use core::fmt;

use super::Allocation;
use super::Location;
use super::Move;
use super::RegisterSet;
use super::liveness::phi_inputs_from;
use crate::codegen::lowering_info::Clobbers;
use crate::codegen::lowering_info::InputConstraint;
use crate::codegen::lowering_info::lowering_info;
use crate::debug_text::DebugText;
use crate::ir::BlockId;
use crate::ir::Graph;
use crate::ir::NodeId;
use crate::ir::Op;
use crate::tables::Run;

/// What a location holds.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum Content {
    Value(NodeId),
    Constant(u64),
    /// The value a slot that nothing writes holds, which every load of it loads.
    UnwrittenSlot(u32),
}

/// How many places a state has for registers, which come before the places for stack slots: one for each number a
/// register can have.
const REGISTER_PLACES: usize = 256;

/// The place of `location` in a state, if a state has one for it: registers and stack slots, in the order of
/// `Location`. Moves never write the other locations.
fn place(location: Location) -> Option<usize> {
    match location {
        Location::Register(register) => Some(usize::from(register)),
        Location::Stack(slot) => Some(REGISTER_PLACES + slot as usize),
        Location::Constant(_) | Location::Frame(_) => None,
    }
}

/// The location that has the place `place` in a state.
fn location_at(place: usize) -> Location {
    match u8::try_from(place) {
        Ok(register) => Location::Register(register),
        Err(_) => Location::Stack((place - REGISTER_PLACES) as u32),
    }
}

/// What the locations hold at one point of the program.
trait Holds {
    fn holds(&self, location: Location) -> Option<Content>;
}

/// What each location holds at the point that the walk is at, with a place for each register and stack slot.
struct State<'w> {
    contents: ZoneVec<'w, Option<Content>>,
    /// The places that were given contents since the state was last cleared, maybe more than one time.
    filled: ZoneVec<'w, u32>,
}

impl<'w> State<'w> {
    /// A state in which nothing holds anything, with places for `stack_slots` stack slots.
    fn new_in(stack_slots: usize, work: &'w Zone) -> Self {
        Self {
            contents: ZoneVec::from_elem_in(None, REGISTER_PLACES + stack_slots, work),
            filled: ZoneVec::new_in(work),
        }
    }

    fn insert(&mut self, location: Location, content: Content) {
        let Some(place) = place(location) else {
            return;
        };
        if self.contents[place].is_none() {
            self.filled.push(place as u32);
        }
        self.contents[place] = Some(content);
    }

    fn remove(&mut self, location: Location) -> Option<Content> {
        self.contents[place(location)?].take()
    }

    /// Makes the state what `stored` (see `store()`) says.
    fn load(&mut self, stored: &[(Location, Content)]) {
        for place in self.filled.drain_all() {
            self.contents[place as usize] = None;
        }
        for (location, content) in stored {
            self.insert(*location, *content);
        }
    }

    /// Writes what each location holds to `out`, in the order of the locations.
    fn store(&mut self, out: &mut ZoneVec<'_, (Location, Content)>) {
        self.filled.sort_unstable();
        self.filled.dedup();
        out.clear();
        for place in &self.filled {
            if let Some(content) = self.contents[*place as usize] {
                out.push((location_at(*place as usize), content));
            }
        }
    }
}

impl Holds for State<'_> {
    fn holds(&self, location: Location) -> Option<Content> {
        self.contents[place(location)?]
    }
}

/// What each location holds, in the order of the locations (see `State::store()`).
impl Holds for [(Location, Content)] {
    fn holds(&self, location: Location) -> Option<Content> {
        let index = self.binary_search_by_key(&location, |(location, _)| *location).ok()?;
        Some(self[index].1)
    }
}

/// A state for each block, if it has one yet, as what `State::store()` writes. The states of all the blocks are in
/// one list.
struct States<'w> {
    runs: ZoneVec<'w, Option<Run>>,
    /// How many elements the part of `items` of each block has room for.
    rooms: ZoneVec<'w, u32>,
    items: ZoneVec<'w, (Location, Content)>,
}

impl<'w> States<'w> {
    fn new_in(block_count: usize, work: &'w Zone) -> Self {
        Self {
            runs: ZoneVec::from_elem_in(None, block_count, work),
            rooms: ZoneVec::from_elem_in(0, block_count, work),
            items: ZoneVec::new_in(work),
        }
    }

    fn get(&self, block: BlockId) -> Option<&[(Location, Content)]> {
        Some(&self.items[self.runs[block.index()]?.range()])
    }

    fn set(&mut self, block: BlockId, state: &[(Location, Content)]) {
        let index = block.index();
        match self.runs[index] {
            // NB: A new state of a block takes the place of the one it had where it fits, which it mostly does: the
            //     states of a block mostly hold less each time they are computed again.
            Some(run) if state.len() <= self.rooms[index] as usize => {
                let start = run.range().start;
                self.items[start..start + state.len()].copy_from_slice(state);
                self.runs[index] = Some(Run::new(start, start + state.len()));
            }
            _ => {
                let start = self.items.len();
                self.items.extend_from_slice(state);
                self.rooms[index] = state.len() as u32;
                self.runs[index] = Some(Run::new(start, self.items.len()));
            }
        }
    }
}

/// How many stack slots the locations of `allocation` name.
fn stack_slot_count(allocation: &Allocation) -> usize {
    let mut count = allocation.spill_slot_count as usize;
    let mut note = |location: Location| {
        if let Location::Stack(slot) = location {
            count = count.max(slot as usize + 1);
        }
    };
    for row in &allocation.nodes {
        row.output.into_iter().for_each(&mut note);
        row.spill_slot.map(Location::Stack).into_iter().for_each(&mut note);
    }
    for move_ in &allocation.moves {
        note(move_.from);
        note(move_.to);
    }
    allocation.locations.iter().copied().for_each(&mut note);
    allocation.exit_values.iter().for_each(|(_, location)| note(*location));
    count
}

/// What an error is about.
#[derive(Clone, Copy)]
enum Context {
    Node(NodeId),
    Input(NodeId, usize),
    /// The exit value of a node for a slot.
    ExitValue(NodeId, u32),
    /// The moves at the end of a block.
    EndOfBlock(BlockId),
}

impl fmt::Display for Context {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match *self {
            Self::Node(node) => write!(f, "v{}", node.0),
            Self::Input(node, index) => write!(f, "v{}, input {index}", node.0),
            Self::ExitValue(node, slot) => write!(f, "v{}, exit value for slot {slot}", node.0),
            Self::EndOfBlock(block) => write!(f, "end of b{}", block.0),
        }
    }
}

struct Verifier<'a, 'w> {
    graph: &'a Graph<'a>,
    registers: &'a RegisterSet,
    allocation: &'a Allocation<'a>,
    /// Whether to report errors; off while the block states are still
    /// being computed.
    checking: bool,
    /// The errors, one on each line.
    errors: DebugText<'w>,
    work: &'w Zone,
}

/// Checks `allocation` against `graph`, returning a description of every
/// problem found.
pub fn verify<'w>(
    graph: &Graph,
    registers: &RegisterSet,
    allocation: &Allocation,
    work: &'w Zone,
) -> Result<(), DebugText<'w>> {
    let mut verifier = Verifier {
        graph,
        registers,
        allocation,
        checking: false,
        errors: DebugText::new_in(work),
        work,
    };
    let block_count = graph.blocks.len();
    let block_ids = (0..block_count).map(BlockId::from_index);
    let mut state = State::new_in(stack_slot_count(allocation), work);
    let mut entry = ZoneVec::new_in(work);
    let mut exit = ZoneVec::new_in(work);

    // Compute the state at the start of every block: what holds on all
    // paths, starting optimistically from the predecessors seen so far.
    let mut entry_states = States::new_in(block_count, work);
    let mut exit_states = States::new_in(block_count, work);
    let mut changed = true;
    while changed {
        changed = false;
        for block in block_ids.clone() {
            if !verifier.entry_state(block, &exit_states, &mut state, &mut entry) {
                continue;
            }
            if entry_states.get(block) == Some(&entry[..]) && exit_states.get(block).is_some() {
                continue;
            }
            verifier.simulate_block(block, &entry, &mut state, &mut exit);
            entry_states.set(block, &entry);
            exit_states.set(block, &exit);
            changed = true;
        }
    }

    verifier.checking = true;
    for block in block_ids {
        match entry_states.get(block) {
            Some(entry) => {
                verifier.check_phi_inputs(block, &exit_states);
                verifier.simulate_block(block, entry, &mut state, &mut exit);
            }
            None => verifier.error(format_args!("b{} is unreachable", block.0)),
        }
    }

    if verifier.errors.is_empty() {
        Ok(())
    } else {
        Err(verifier.errors)
    }
}

impl Verifier<'_, '_> {
    fn error(&mut self, message: fmt::Arguments<'_>) {
        if self.checking {
            if !self.errors.is_empty() {
                self.errors.push('\n');
            }
            self.errors.write_fmt(message);
        }
    }

    fn content_of_value(&self, value: NodeId) -> Content {
        if matches!(self.graph.node(value).op, Op::VirtualObject { .. }) {
            return Content::Constant(crate::ir::value::EMPTY);
        }
        if let Op::LoadSlot { slot } = self.graph.node(value).op
            && self.graph.is_unwritten_slot(slot)
        {
            return Content::UnwrittenSlot(slot);
        }
        match self.graph.constant_value(value) {
            Some(bits) => Content::Constant(bits),
            None => Content::Value(value),
        }
    }

    fn read(&self, state: &(impl Holds + ?Sized), location: Location) -> Option<Content> {
        match location {
            Location::Constant(bits) => Some(Content::Constant(bits)),
            Location::Frame(slot) if self.graph.is_unwritten_slot(slot) => Some(Content::UnwrittenSlot(slot)),
            _ => state.holds(location),
        }
    }

    /// Writes to `out` the state at the start of `block`: what holds at the end of all its predecessors seen so far,
    /// with the phis in place. Returns false if no predecessor was seen yet. `state` is for the work.
    fn entry_state(
        &self,
        block: BlockId,
        exit_states: &States,
        state: &mut State,
        out: &mut ZoneVec<'_, (Location, Content)>,
    ) -> bool {
        out.clear();
        // NB: The entry and the on-stack replacement entries start with
        //     every value in the frame.
        let block_data = self.graph.block(block);
        if block.0 == 0 || block_data.predecessors.is_empty() {
            return true;
        }
        let mut seen = false;
        for predecessor in &block_data.predecessors {
            let Some(exit) = exit_states.get(*predecessor) else {
                continue;
            };
            state.load(exit);
            for (phi, input) in phi_inputs_from(self.graph, *predecessor, block) {
                let output = self.allocation.node(phi).output.expect("phis have a location");
                for location in core::iter::once(output).chain(self.filled_slot(phi)) {
                    if self.read(&*state, location) == Some(self.content_of_value(input)) {
                        state.insert(location, Content::Value(phi));
                    } else {
                        state.remove(location);
                    }
                }
            }
            if seen {
                out.retain(|(location, content)| state.holds(*location) == Some(*content));
            } else {
                state.store(out);
                seen = true;
            }
        }
        seen
    }

    fn check_phi_inputs(&mut self, block: BlockId, exit_states: &States) {
        let graph = self.graph;
        for predecessor in graph.block(block).predecessors.iter().copied() {
            let Some(exit) = exit_states.get(predecessor) else {
                continue;
            };
            for (phi, input) in phi_inputs_from(self.graph, predecessor, block) {
                let output = self.allocation.node(phi).output.expect("phis have a location");
                for location in core::iter::once(output).chain(self.filled_slot(phi)) {
                    let found = self.read(exit, location);
                    if found != Some(self.content_of_value(input)) {
                        self.error(format_args!(
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
    }

    /// The spill slot of a phi in a register that its inputs fill, rather
    /// than a store at the start of its block.
    fn filled_slot(&self, phi: NodeId) -> Option<Location> {
        let allocation = self.allocation.node(phi);
        match (allocation.output, allocation.spill_slot) {
            (Some(Location::Register(_)), Some(slot))
                if super::phi_spill_store(self.graph, self.allocation, phi).is_none() =>
            {
                Some(Location::Stack(slot))
            }
            _ => None,
        }
    }

    fn apply_moves(&mut self, state: &mut State, moves: &[Move], context: Context) {
        for m in moves {
            if matches!(m.to, Location::Constant(_) | Location::Frame(_)) {
                self.error(format_args!("{context}: move into {:?}", m.to));
                continue;
            }
            self.check_location(m.to, context);
            match self.read(state, m.from) {
                Some(content) => {
                    state.insert(m.to, content);
                }
                None => {
                    self.error(format_args!("{context}: move reads {:?}, which holds nothing", m.from));
                    state.remove(m.to);
                }
            }
        }
    }

    fn check_location(&mut self, location: Location, context: Context) {
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
            self.error(format_args!("{context}: r{register} is not allocatable"));
        }
    }

    fn expect(&mut self, state: &State, location: Location, value: NodeId, context: Context) {
        let expected = self.content_of_value(value);
        let found = self.read(state, location);
        if found != Some(expected) {
            self.error(format_args!(
                "{context}: expected {expected:?} in {location:?}, found {found:?}"
            ));
        }
    }

    fn check_exit_values(&mut self, state: &State, node: NodeId, context: Context) {
        let Some(frame_state) = self.graph.node(node).frame_state else {
            return;
        };
        let expected_count = self.graph.frame_state_values(frame_state, self.work).count();
        let exit_values = self.allocation.node(node).exit_values;
        if expected_count != exit_values.len() {
            self.error(format_args!(
                "{context}: exit has {} values, frame state {}",
                exit_values.len(),
                expected_count
            ));
            return;
        }
        for ((slot, value), (exit_slot, location)) in
            self.graph.frame_state_values(frame_state, self.work).zip(exit_values)
        {
            if slot != *exit_slot {
                self.error(format_args!(
                    "{context}: exit value for slot {exit_slot}, frame state slot {slot}"
                ));
            }
            self.expect(state, *location, value, Context::ExitValue(node, slot));
        }
    }

    /// Writes to `out` what holds at the end of `block`, which starts with `entry`. `state` is for the work.
    fn simulate_block(
        &mut self,
        block: BlockId,
        entry: &[(Location, Content)],
        state: &mut State,
        out: &mut ZoneVec<'_, (Location, Content)>,
    ) {
        state.load(entry);
        let block_data = self.graph.block(block);
        // Phis in registers are stored to their spill slots first, unless
        // their inputs filled them already (see `filled_slot()`).
        for phi in &block_data.phis {
            if let Some(slot) = super::phi_spill_store(self.graph, self.allocation, *phi) {
                state.insert(Location::Stack(slot), Content::Value(*phi));
            }
        }
        for node in &block_data.body {
            self.simulate_node(state, *node);
        }
        let end_moves = self.allocation.block_end_moves(block);
        self.apply_moves(state, end_moves, Context::EndOfBlock(block));
        self.simulate_node(state, self.graph.control_id(block));
        state.store(out);
    }

    fn simulate_node(&mut self, state: &mut State, node_id: NodeId) {
        let node = self.graph.node(node_id);
        let allocation = self.allocation.node(node_id);
        let context = Context::Node(node_id);
        debug_assert!(node.op != Op::Phi);

        self.apply_moves(state, allocation.moves_before, context);
        if let Op::StoreSlot { slot } = node.op
            && self.graph.is_unwritten_slot(slot)
        {
            self.error(format_args!("{context}: stores to slot {slot}, which nothing writes"));
        }
        if allocation.inputs.len() != node.inputs.len() {
            self.error(format_args!("{context}: wrong number of input locations"));
            return;
        }
        let lowering = lowering_info(node);
        if allocation.temps.len() != usize::from(lowering.temps) {
            self.error(format_args!(
                "{context}: {} temps, but its lowering takes {}",
                allocation.temps.len(),
                lowering.temps
            ));
        }
        for (index, (input, location)) in node.inputs.iter().zip(allocation.inputs).enumerate() {
            if let Location::Constant(bits) = location {
                if self.graph.constant_value(*input) != Some(*bits) {
                    self.error(format_args!("{context}: input {index} is not the constant {bits:#x}"));
                }
                continue;
            }
            // NB: Slow path calls, and calls that store their operands, read
            //     them from where they are.
            let reads_input_anywhere = lowering.inputs.get(index) == InputConstraint::Anywhere;
            if !matches!(location, Location::Register(_)) && !reads_input_anywhere {
                self.error(format_args!("{context}: input {index} is not in a register"));
            }
            if let InputConstraint::Fixed(register) = self.registers.resolve(lowering.inputs.get(index), false)
                && *location != Location::Register(self.registers.fixed(register))
            {
                self.error(format_args!("{context}: input {index} is not in its fixed register"));
            }
            self.check_location(*location, context);
            self.expect(state, *location, *input, Context::Input(node_id, index));
        }

        // A node may exit at any point after reading its inputs, including
        // after it wrote its temps and its output: exit values must survive both.
        for temp in allocation.temps {
            self.check_location(Location::Register(*temp), context);
            if allocation.inputs.contains(&Location::Register(*temp)) {
                self.error(format_args!("{context}: temp r{temp} is also an input"));
            }
            state.remove(Location::Register(*temp));
        }
        let clobbered = match lowering.clobbers {
            Clobbers::CallerSaved => self.registers.caller_saved(),
            Clobbers::Floats => self.registers.allocatable(super::RegisterClass::Float),
            Clobbers::Nothing => super::RegisterMask::default(),
        };
        for register in clobbered.iter() {
            state.remove(Location::Register(register));
        }
        if let Some(output @ Location::Register(_)) = allocation.output {
            // NB: Cloning the state instead would take time quadratic in the size of code that keeps many values in
            //     their stack slots past many nodes.
            let previous = state.remove(output);
            self.check_exit_values(state, node_id, context);
            if let Some(previous) = previous {
                state.insert(output, previous);
            }
        } else {
            self.check_exit_values(state, node_id, context);
        }

        match (node.repr.is_some(), allocation.output) {
            (true, Some(Location::Register(register))) => {
                self.check_location(Location::Register(register), context);
                if super::RegisterClass::of_register(register) != super::RegisterClass::of(node.repr) {
                    self.error(format_args!(
                        "{context}: the output is in a register of the other class"
                    ));
                }
                state.insert(Location::Register(register), self.content_of_value(node_id));
                if let Some(slot) = allocation.spill_slot {
                    state.insert(Location::Stack(slot), self.content_of_value(node_id));
                }
            }
            (false, None) => {}
            (_, output) => self.error(format_args!("{context}: unexpected output location {output:?}")),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn states_keep_the_order_of_the_locations_and_their_places() {
        let work = crate::test_work();
        let mut state = State::new_in(4, &work);
        let mut stored = ZoneVec::new_in(&work);
        state.insert(Location::Stack(3), Content::Constant(1));
        state.insert(Location::Register(200), Content::Value(NodeId(2)));
        state.insert(Location::Register(1), Content::Value(NodeId(3)));
        state.insert(Location::Stack(0), Content::UnwrittenSlot(4));
        assert_eq!(state.remove(Location::Stack(3)), Some(Content::Constant(1)));
        state.insert(Location::Stack(3), Content::Constant(5));
        state.insert(Location::Frame(7), Content::Constant(6));
        state.store(&mut stored);
        let expected = [
            (Location::Register(1), Content::Value(NodeId(3))),
            (Location::Register(200), Content::Value(NodeId(2))),
            (Location::Stack(0), Content::UnwrittenSlot(4)),
            (Location::Stack(3), Content::Constant(5)),
        ];
        assert_eq!(stored, expected);
        assert_eq!(stored.holds(Location::Stack(0)), Some(Content::UnwrittenSlot(4)));
        assert_eq!(stored.holds(Location::Stack(1)), None);
        assert_eq!(state.holds(Location::Frame(7)), None);

        state.load(&expected[1..]);
        assert_eq!(state.holds(Location::Register(1)), None);
        assert_eq!(state.holds(Location::Register(200)), Some(Content::Value(NodeId(2))));

        let mut states = States::new_in(2, &work);
        assert_eq!(states.get(BlockId(1)), None);
        states.set(BlockId(1), &expected);
        states.set(BlockId(0), &expected[..1]);
        assert_eq!(states.get(BlockId(1)), Some(&expected[..]));
        // NB: A smaller state takes the place of the one before it, and a larger one goes to new room.
        states.set(BlockId(1), &expected[2..]);
        assert_eq!(states.items.len(), 5);
        assert_eq!(states.get(BlockId(1)), Some(&expected[2..]));
        states.set(BlockId(0), &expected[..2]);
        assert_eq!(states.items.len(), 7);
        assert_eq!(states.get(BlockId(0)), Some(&expected[..2]));
        assert_eq!(states.get(BlockId(1)), Some(&expected[2..]));
    }
}
