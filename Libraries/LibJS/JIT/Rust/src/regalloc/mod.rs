/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! A straightforward linear-order register allocator.
//!
//! One forward walk over the blocks in linear order assigns registers to
//! values as nodes need them, using the constraints each node declares. When
//! no register is free, the value whose next use is furthest away is evicted.
//!
//! Spilling is simple: a value that ever needs a stack slot gets one slot for
//! its whole life, and code generation stores it there right after its
//! definition. Values live across a call always get one. Values loaded from a
//! frame slot that nothing writes need none: they are reloaded from the frame
//! (see `frame_home()`).
//! Phis share their slots with the values flowing into them where their
//! lives do not overlap (see `coalescing`), so that values carried around
//! loops in stack slots need no copying.
//!
//! A merge or loop header starts with the register state of its first
//! predecessor in linear order (its "state predecessor"; for a loop header,
//! the block entering the loop), and its phis get registers that are free
//! there. The other predecessors, back edges included, move their values (and
//! their phis' inputs) into place at their end, as parallel moves. The rare
//! merges whose other predecessors do not jump to them start with every value
//! in its stack slot (see `liveness::state_predecessor()`).
//!
//! Loops (see `loops`) keep their registers for what they use: a loop header
//! leaves the values that the loop only carries through in their stack slots
//! where they have them already, and inside a loop, the values it reads again
//! in its next iteration are evicted after those used only after it.
//!
//! Cold blocks (see `Block::is_cold`) come last in linear order and cost the
//! other blocks nothing: they start with the register state of the block
//! they branch off from, and move their values into place where they rejoin
//! the other blocks.
//!
//! There are two classes of registers: general purpose registers hold
//! every value but `Repr::Float64` ones, which floating point registers hold.
//! Floating point registers are numbered from `FPR_BASE` on in `Location`s
//! and masks. Nodes whose slow paths save the general purpose registers
//! around their calls (see `Clobbers::Floats`) clobber the floating point
//! ones, so float64 values live across them get stack slots, like values
//! live across a call.
//!
//! What each node needs (where its inputs and its output are, its temps and
//! what it clobbers) is declared next to its lowering, in
//! `codegen::lowering_info()`.

mod coalescing;
mod liveness;
mod loops;
mod moves;
mod verify;

pub use moves::resolve_parallel_moves;
pub use verify::verify;

use crate::CompileFailure;
use crate::code::Repr;
use crate::codegen::lowering_info::Clobbers;
use crate::codegen::lowering_info::FixedRegister;
use crate::codegen::lowering_info::InputConstraint;
use crate::codegen::lowering_info::OutputConstraint;
use crate::codegen::lowering_info::lowering_info;
use crate::ir::BlockId;
use crate::ir::Graph;
use crate::ir::NodeId;
use crate::ir::Op;
use crate::tables::Lists;
use crate::tables::Run;
use crate::tables::release;
use crate::tables::reset;
use ak::Column;
use ak::ColumnFull;
use coalescing::Coalescing;
use liveness::ValueLiveness;
use liveness::is_allocated_value;
use liveness::state_predecessor;
use loops::Loops;

/// The number of the first floating point register in `Location`s and
/// `RegisterMask`s: floating point register `n` is register `FPR_BASE + n`.
pub const FPR_BASE: u8 = 32;

/// How many registers of both classes there are at most.
const REGISTER_COUNT: usize = 64;

/// What a value's register must be.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum RegisterClass {
    General,
    Float,
}

impl RegisterClass {
    /// The class of the registers that hold values of representation `repr`.
    pub fn of(repr: Option<Repr>) -> Self {
        match repr {
            Some(Repr::Float64) => RegisterClass::Float,
            _ => RegisterClass::General,
        }
    }

    /// The class of register number `register`.
    pub fn of_register(register: u8) -> Self {
        if register >= FPR_BASE {
            RegisterClass::Float
        } else {
            RegisterClass::General
        }
    }
}

/// A set of registers, as a bit mask of register numbers.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct RegisterMask(pub u64);

impl RegisterMask {
    pub fn contains(self, register: u8) -> bool {
        self.0 & (1 << register) != 0
    }

    pub fn with(self, register: u8) -> Self {
        Self(self.0 | (1 << register))
    }

    pub fn iter(self) -> impl Iterator<Item = u8> {
        (0..64).filter(move |register| self.contains(*register))
    }
}

/// The registers of a target, as far as register allocation cares.
/// Registers are named by their hardware encoding.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct RegisterSet {
    /// Registers the allocator may assign to values.
    pub allocatable_gprs: RegisterMask,
    pub allocatable_fprs: RegisterMask,
    /// Registers a call clobbers.
    pub caller_saved_gprs: RegisterMask,
    pub caller_saved_fprs: RegisterMask,
    /// Where calls take their arguments, in order.
    pub argument_gprs: Vec<u8>,
    /// Where calls return their (first) result.
    pub return_gpr: u8,
    /// Where shifts by a register amount take the amount, on targets that shift by one register only.
    pub shift_amount_gpr: Option<u8>,
    /// Never allocated. Moves between stack slots and of constants into stack
    /// slots go through it, and node lowerings may clobber it.
    pub scratch_gpr: u8,
}

impl RegisterSet {
    /// The registers of `class` the allocator may assign to values.
    pub fn allocatable(&self, class: RegisterClass) -> RegisterMask {
        match class {
            RegisterClass::General => self.allocatable_gprs,
            RegisterClass::Float => RegisterMask(self.allocatable_fprs.0 << FPR_BASE),
        }
    }

    /// The number of the calling convention's register `register`.
    pub fn fixed(&self, register: FixedRegister) -> u8 {
        match register {
            FixedRegister::Argument(index) => self.argument_gprs[usize::from(index)],
            FixedRegister::Return => self.return_gpr,
            FixedRegister::ShiftAmount => self
                .shift_amount_gpr
                .expect("only targets with a shift amount register have fixed shift amounts"),
        }
    }

    /// Where an input of `constraint` must be on this target, if it is a constant (`is_constant`) or not.
    pub fn resolve(&self, constraint: InputConstraint, is_constant: bool) -> InputConstraint {
        match constraint {
            InputConstraint::ShiftAmount if is_constant || self.shift_amount_gpr.is_none() => {
                InputConstraint::RegisterOrConstant
            }
            InputConstraint::ShiftAmount => InputConstraint::Fixed(FixedRegister::ShiftAmount),
            constraint => constraint,
        }
    }

    /// The registers of both classes a call clobbers.
    pub fn caller_saved(&self) -> RegisterMask {
        RegisterMask(self.caller_saved_gprs.0 | (self.caller_saved_fprs.0 << FPR_BASE))
    }

    /// The fewest allocatable registers allocation works with: the ones
    /// calls take their arguments and return their results in, the one
    /// shifts take their amount in, and `extra` more.
    pub fn few_registers(&self, extra: u32) -> RegisterMask {
        let mut mask = RegisterMask(0).with(self.return_gpr);
        for register in self.argument_gprs.iter().chain(&self.shift_amount_gpr) {
            mask = mask.with(*register);
        }
        let mut mask = RegisterMask(mask.0 & self.allocatable_gprs.0);
        for register in self
            .allocatable_gprs
            .iter()
            .filter(|register| !mask.contains(*register))
            .take(extra as usize)
            .collect::<Vec<_>>()
        {
            mask = mask.with(register);
        }
        mask
    }

    /// A small made up target for tests: six allocatable registers, all of
    /// them clobbered by calls.
    pub fn test_configuration() -> Self {
        Self {
            allocatable_gprs: RegisterMask(0b0011_1111),
            allocatable_fprs: RegisterMask(0b1111_1111),
            caller_saved_gprs: RegisterMask(0b1111_1111),
            caller_saved_fprs: RegisterMask(0b1111_1111),
            argument_gprs: vec![1, 2, 3],
            return_gpr: 0,
            shift_amount_gpr: None,
            scratch_gpr: 7,
        }
    }
}

/// Where a value is, or where a move reads or writes.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub enum Location {
    Register(u8),
    /// A spill slot of the JIT frame.
    Stack(u32),
    /// A NaN-boxed constant; only ever a move source.
    Constant(u64),
    /// A slot of the compiled function's frame (an operand), which holds the value loaded from it wherever that value
    /// is live (see `frame_home()`); only ever a move source.
    Frame(u32),
}

impl Default for Location {
    fn default() -> Self {
        Location::Register(0)
    }
}

/// Copies a value. Moves between stack slots and of constants into stack
/// slots go through the scratch register.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Move {
    pub from: Location,
    pub to: Location,
}

/// The most temps a node has (see `LoweringInfo::temps`).
const MAX_TEMPS: usize = 4;

/// How the allocator placed one node.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct NodeAllocation<'a> {
    /// Moves to perform, in order, right before the node.
    pub moves_before: &'a [Move],
    /// Where the node finds each input: a register, or a constant for
    /// inputs that may be immediates.
    pub inputs: &'a [Location],
    pub temps: &'a [u8],
    /// Where the node puts its value: a register, or a stack slot for phis.
    pub output: Option<Location>,
    /// If set and the output is a register, the value must be stored to this
    /// spill slot right after its definition.
    pub spill_slot: Option<u32>,
    /// For nodes with a frame state, where the exit finds each of its values,
    /// as (frame slot, location).
    pub exit_values: &'a [(u32, Location)],
}

/// The row of a node in the tables of an allocation (see `NodeAllocation`). Its lists are parts of the columns that
/// hold the lists of all the nodes.
#[derive(Clone, Copy, Default)]
struct NodeRow {
    moves_before: Run,
    inputs: Run,
    exit_values: Run,
    output: Option<Location>,
    spill_slot: Option<u32>,
    temps: [u8; MAX_TEMPS],
    temp_count: u8,
}

/// Where the allocator placed the values of a graph, in tables that serve one compile after the other.
pub struct Allocation {
    /// Indexed by `NodeId`.
    nodes: Column<NodeRow>,
    /// The moves before the nodes and at the ends of the blocks.
    moves: Column<Move>,
    /// The locations of the inputs of the nodes.
    locations: Column<Location>,
    exit_values: Column<(u32, Location)>,
    /// The moves at the end of each block, as a part of `moves`.
    block_end_moves: Column<Run>,
    pub spill_slot_count: u32,
    /// The values live into each block, sorted.
    live_in: Lists<NodeId>,
}

impl Allocation {
    // NB: A compile whose graph needs more of these fails.
    const MAX_NODES: usize = 1 << 24;
    const MAX_ITEMS: usize = 1 << 26;

    pub fn new() -> Self {
        Self {
            nodes: Column::new(Self::MAX_NODES, c"JIT allocation of nodes"),
            moves: Column::new(Self::MAX_ITEMS, c"JIT allocation moves"),
            locations: Column::new(Self::MAX_ITEMS, c"JIT allocation locations"),
            exit_values: Column::new(Self::MAX_ITEMS, c"JIT allocation exit values"),
            block_end_moves: Column::new(Self::MAX_NODES, c"JIT allocation of blocks"),
            spill_slot_count: 0,
            live_in: ValueLiveness::new_live_in(),
        }
    }

    /// Drops the tables and gives their memory back to the system.
    fn release_memory(&mut self) {
        release(&mut self.nodes);
        release(&mut self.moves);
        release(&mut self.locations);
        release(&mut self.exit_values);
        release(&mut self.block_end_moves);
        self.live_in.release_memory();
    }

    /// Starts the allocation of a graph with `node_count` nodes and `block_count` blocks, in tables that hold
    /// nothing.
    fn begin(&mut self, node_count: usize, block_count: usize) -> Result<(), ColumnFull> {
        self.nodes.clear();
        self.nodes.try_resize(node_count, NodeRow::default())?;
        self.moves.clear();
        self.locations.clear();
        self.exit_values.clear();
        self.block_end_moves.clear();
        self.block_end_moves.try_resize(block_count, Run::default())?;
        self.spill_slot_count = 0;
        Ok(())
    }

    /// How many nodes have an allocation.
    pub fn node_count(&self) -> usize {
        self.nodes.len()
    }

    pub fn node(&self, node: NodeId) -> NodeAllocation<'_> {
        let row = &self.nodes[node.index()];
        NodeAllocation {
            moves_before: &self.moves[row.moves_before.range()],
            inputs: &self.locations[row.inputs.range()],
            temps: &row.temps[..usize::from(row.temp_count)],
            output: row.output,
            spill_slot: row.spill_slot,
            exit_values: &self.exit_values[row.exit_values.range()],
        }
    }

    /// Moves to perform, in order, at the end of `block`, after its body
    /// and before its control node. Only blocks ending in a jump to a merge
    /// or a loop header have any.
    pub fn block_end_moves(&self, block: BlockId) -> &[Move] {
        &self.moves[self.block_end_moves[block.index()].range()]
    }

    /// Whether `value` is live into `block`.
    pub fn is_live_in(&self, block: BlockId, value: NodeId) -> bool {
        self.live_in.get(block.index()).binary_search(&value).is_ok()
    }
}

impl Default for Allocation {
    fn default() -> Self {
        Self::new()
    }
}

// NB: The tests of the verifier make wrong allocations with these.
#[cfg(test)]
impl Allocation {
    /// All the moves of the allocation.
    pub(crate) fn moves(&self) -> &[Move] {
        &self.moves
    }

    pub(crate) fn set_spill_slot(&mut self, node: NodeId, slot: Option<u32>) {
        self.nodes[node.index()].spill_slot = slot;
    }

    pub(crate) fn set_input(&mut self, node: NodeId, index: usize, location: Location) {
        let inputs = self.nodes[node.index()].inputs.range();
        self.locations[inputs][index] = location;
    }

    pub(crate) fn set_move_before(&mut self, node: NodeId, index: usize, new_move: Move) {
        let moves = self.nodes[node.index()].moves_before.range();
        self.moves[moves][index] = new_move;
    }

    /// Adds the allocation of one more node, which has `inputs` and `output` and nothing else.
    pub(crate) fn push_node(&mut self, inputs: &[Location], output: Option<Location>) {
        let start = self.locations.len();
        self.locations.extend_from_slice(inputs);
        self.nodes.push(NodeRow {
            inputs: Run::new(start, self.locations.len()),
            output,
            ..NodeRow::default()
        });
    }
}

/// What the register allocator works with and its result, in tables that serve one compile after the other. The
/// allocator makes no block of the heap: all its data is here.
pub struct Regalloc {
    pub allocation: Allocation,
    liveness: ValueLiveness,
    coalescing: Coalescing,
    /// The register each value is in, if any.
    value_registers: Column<Option<u8>>,
    /// The register contents at the end of each finished block: `REGISTER_COUNT` values for each block.
    end_contents: Column<Option<NodeId>>,
    /// Whether each block is finished.
    finished: Column<bool>,
    /// The slot of each spill slot class that has one.
    class_slots: Column<Option<u32>>,
    /// Moves that happen in parallel, before they are put in an order.
    parallel_moves: Column<Move>,
}

impl Regalloc {
    /// Tables that take no memory before a compile uses them.
    pub fn new() -> Self {
        Self {
            allocation: Allocation::new(),
            liveness: ValueLiveness::new(),
            coalescing: Coalescing::new(),
            value_registers: Column::new(Allocation::MAX_NODES, c"JIT allocator work"),
            end_contents: Column::new(
                Allocation::MAX_NODES * REGISTER_COUNT,
                c"JIT allocator register contents",
            ),
            finished: Column::new(Allocation::MAX_NODES, c"JIT allocator work"),
            class_slots: Column::new(Allocation::MAX_NODES, c"JIT allocator work"),
            parallel_moves: Column::new(Allocation::MAX_ITEMS, c"JIT allocator work"),
        }
    }

    /// Drops the tables and gives their memory back to the system.
    pub fn release_memory(&mut self) {
        self.allocation.release_memory();
        self.liveness.release_memory();
        self.coalescing.release_memory();
        release(&mut self.value_registers);
        release(&mut self.end_contents);
        release(&mut self.finished);
        release(&mut self.class_slots);
        release(&mut self.parallel_moves);
    }
}

impl Default for Regalloc {
    fn default() -> Self {
        Self::new()
    }
}

/// The value each register holds.
type Contents = [Option<NodeId>; REGISTER_COUNT];

/// The slot of the compiled function's frame that holds `value` wherever it is live, if there is one: a value loaded
/// from one of the slots whose frame memory never changes (`Graph::unwritten_slots`).
///
/// OPTIMIZATION: Such a value needs no spill slot, and no store right after its definition: where no register holds
///               it, it is reloaded from the frame. Exits find it there too (`code::ValueLocation::Frame`).
pub fn frame_home(graph: &Graph, value: NodeId) -> Option<u32> {
    match graph.node(value).op {
        Op::LoadSlot { slot } if graph.is_unwritten_slot(slot) => Some(slot),
        _ => None,
    }
}

/// Assigns locations to the values of `graph`, into `regalloc.allocation`. The verifier checks the
/// result where the IR is verified (see `verify()`).
pub fn allocate(graph: &Graph, registers: &RegisterSet, regalloc: &mut Regalloc) -> Result<(), CompileFailure> {
    assert!(registers.allocatable_gprs.0 >> FPR_BASE == 0);
    assert!(!registers.allocatable_gprs.contains(registers.scratch_gpr));
    let Regalloc {
        allocation,
        liveness,
        coalescing,
        value_registers,
        end_contents,
        finished,
        class_slots,
        parallel_moves,
    } = regalloc;
    liveness.compute(graph)?;
    coalescing.compute(graph, liveness)?;
    let loops = Loops::compute(graph, liveness);
    allocation.begin(graph.nodes.len(), graph.blocks.len())?;
    reset(value_registers, graph.nodes.len(), None)?;
    reset(end_contents, graph.blocks.len() * REGISTER_COUNT, None)?;
    reset(finished, graph.blocks.len(), false)?;
    reset(class_slots, graph.nodes.len(), None)?;
    let allocator = Allocator {
        graph,
        registers,
        liveness,
        allocation,
        contents: [None; REGISTER_COUNT],
        value_registers,
        end_contents,
        finished,
        position: 0,
        slot_classes: &coalescing.classes,
        class_slots,
        parallel_moves,
        loops,
    };
    allocator.run()?;
    // NB: The code generator reads which values are live into a block.
    core::mem::swap(&mut allocation.live_in, &mut liveness.live_in);
    Ok(())
}

struct Allocator<'a> {
    graph: &'a Graph,
    registers: &'a RegisterSet,
    liveness: &'a ValueLiveness,
    allocation: &'a mut Allocation,
    contents: Contents,
    value_registers: &'a mut Column<Option<u8>>,
    end_contents: &'a mut Column<Option<NodeId>>,
    finished: &'a mut Column<bool>,
    /// The position of the node being allocated.
    position: u32,
    /// The spill slot class of each value (see `coalescing`), and the
    /// slot of each class that has one.
    slot_classes: &'a [u32],
    class_slots: &'a mut Column<Option<u32>>,
    parallel_moves: &'a mut Column<Move>,
    loops: Loops,
}

impl Allocator<'_> {
    fn run(mut self) -> Result<(), ColumnFull> {
        // Give the phis of blocks without a state predecessor their locations
        // up front, since their predecessors may be allocated after them.
        // Phis of loop headers live in registers, which the header starts
        // with (it starts with every other value in its stack slot); the
        // others live in stack slots.
        for (index, block) in self.graph.blocks.iter().enumerate() {
            // NB: The phis of merges with a state predecessor get their
            //     locations when it ends (see `place_merge_phis()`).
            if state_predecessor(self.graph, BlockId::from_index(index)).is_some() {
                continue;
            }
            let is_loop_header = block
                .predecessors
                .iter()
                .any(|predecessor| predecessor.index() >= index);
            let mut general = self.registers.allocatable(RegisterClass::General).iter();
            let mut float = self.registers.allocatable(RegisterClass::Float).iter();
            for phi in &block.phis {
                let registers = match RegisterClass::of(self.graph.node(*phi).repr) {
                    RegisterClass::General => &mut general,
                    RegisterClass::Float => &mut float,
                };
                let register = if is_loop_header { registers.next() } else { None };
                self.place_phi(*phi, register);
            }
        }

        // Values live into blocks reached over back edges may be defined
        // after them in linear order; give them their slots up front too.
        for (index, block) in self.graph.blocks.iter().enumerate() {
            let block_id = BlockId::from_index(index);
            if block
                .predecessors
                .iter()
                .any(|predecessor| predecessor.index() >= index)
                && state_predecessor(self.graph, block_id).is_none()
            {
                let liveness = self.liveness;
                for value in liveness.live_in.get(index) {
                    self.ensure_home(*value);
                }
            }
        }

        let mut cycle_temp = None;
        for block in 0..self.graph.blocks.len() {
            let block = BlockId::from_index(block);
            self.start_block(block);
            let block_data = self.graph.block(block);
            for node in &block_data.body {
                self.allocate_node(*node)?;
            }
            self.allocation.block_end_moves[block.index()] = self.phi_moves(block, &mut cycle_temp)?;
            self.allocate_node(self.graph.control_id(block))?;
            let start = block.index() * REGISTER_COUNT;
            self.end_contents[start..start + REGISTER_COUNT].copy_from_slice(&self.contents);
            self.finished[block.index()] = true;
        }
        Ok(())
    }

    /// Gives `phi` its location: `register` if it has one, and its spill
    /// slot (which it may have already, if it is live into a block
    /// allocated before its own) if it has no register or needs a slot.
    fn place_phi(&mut self, phi: NodeId, register: Option<u8>) {
        let slot = match register {
            Some(_) if !self.liveness.needs_spill(phi) && self.allocation.nodes[phi.index()].spill_slot.is_none() => {
                None
            }
            _ => Some(self.ensure_spill_slot(phi)),
        };
        let node = &mut self.allocation.nodes[phi.index()];
        node.output = Some(match (register, slot) {
            (Some(register), _) => Location::Register(register),
            (None, Some(slot)) => Location::Stack(slot),
            (None, None) => unreachable!("phis without a register have a slot"),
        });
        node.spill_slot = slot;
    }

    fn new_spill_slot(&mut self) -> u32 {
        let slot = self.allocation.spill_slot_count;
        self.allocation.spill_slot_count += 1;
        slot
    }

    fn ensure_spill_slot(&mut self, value: NodeId) -> u32 {
        if let Some(slot) = self.allocation.nodes[value.index()].spill_slot {
            return slot;
        }
        let class = self.slot_classes[value.index()] as usize;
        let slot = match self.class_slots[class] {
            Some(slot) => slot,
            None => {
                let slot = self.new_spill_slot();
                self.class_slots[class] = Some(slot);
                slot
            }
        };
        self.allocation.nodes[value.index()].spill_slot = Some(slot);
        slot
    }

    /// Makes sure that `value` is available outside of registers: in its frame slot (see `frame_home()`), or in a
    /// spill slot.
    fn ensure_home(&mut self, value: NodeId) {
        if frame_home(self.graph, value).is_none() {
            self.ensure_spill_slot(value);
        }
    }

    fn set_contents(&mut self, contents: Contents) {
        for value in self.contents.iter().flatten() {
            self.value_registers[value.index()] = None;
        }
        self.contents = contents;
        for (register, value) in self.contents.iter().enumerate() {
            if let Some(value) = value {
                self.value_registers[value.index()] = Some(u8::try_from(register).expect("register numbers fit in u8"));
            }
        }
    }

    /// The register contents at the end of `predecessor` (which comes before
    /// `block` in linear order) that are live into `block`.
    fn contents_entering(&self, predecessor: BlockId, block: BlockId) -> Contents {
        assert!(
            self.finished[predecessor.index()],
            "blocks come after the predecessor whose state they start with"
        );
        let start = predecessor.index() * REGISTER_COUNT;
        let mut contents = [None; REGISTER_COUNT];
        contents.copy_from_slice(&self.end_contents[start..start + REGISTER_COUNT]);
        for value in &mut contents {
            if value.is_some_and(|value| !self.liveness.is_live_in(block, value)) {
                *value = None;
            }
        }
        contents
    }

    /// Whether the loop with the header `block`, if it is one, leaves `value` in its stack slot: a value that has one
    /// from its definition on and that the loop only carries through, so that its register is free for the loop.
    fn loop_leaves_in_stack(&self, block: BlockId, value: NodeId) -> bool {
        self.loops
            .headed_by(block)
            .is_some_and(|loop_| loop_.carries_through(value.index()))
            && self.liveness.needs_spill(value)
    }

    /// The register contents a merge with a state predecessor starts with:
    /// those at the end of that predecessor, live into the merge (but those
    /// a loop leaves in their stack slots), and the phis in their registers.
    fn merge_contents(&self, block: BlockId) -> Option<Contents> {
        let predecessor = state_predecessor(self.graph, block)?;
        let mut contents = self.contents_entering(predecessor, block);
        for value in &mut contents {
            if value.is_some_and(|value| self.loop_leaves_in_stack(block, value)) {
                *value = None;
            }
        }
        for phi in &self.graph.block(block).phis {
            if let Some(Location::Register(register)) = self.allocation.nodes[phi.index()].output {
                debug_assert!(contents[register as usize].is_none());
                contents[register as usize] = Some(*phi);
            }
        }
        Some(contents)
    }

    fn start_block(&mut self, block: BlockId) {
        let block_data = self.graph.block(block);
        if let Some(contents) = self.merge_contents(block) {
            self.set_contents(contents);
            if let Some(first) = self.graph.block_nodes(block).next() {
                self.position = self.liveness.positions[first.index()];
            }
            return;
        }
        let contents = match block_data.predecessors.as_slice() {
            // NB: The only predecessor of a loop header that on-stack
            //     replacement entries lead to may be a back edge, which is
            //     allocated after it.
            [predecessor] if predecessor.index() < block.index() => self.contents_entering(*predecessor, block),
            // Merges and loop headers start with every value in its stack
            // slot, and phis in their registers.
            _ => {
                let mut contents = [None; REGISTER_COUNT];
                for phi in &block_data.phis {
                    if let Some(Location::Register(register)) = self.allocation.nodes[phi.index()].output {
                        contents[register as usize] = Some(*phi);
                    }
                }
                contents
            }
        };
        self.set_contents(contents);
        if let Some(first) = self.graph.block_nodes(block).next() {
            self.position = self.liveness.positions[first.index()];
        }
    }

    fn free_register(&mut self, register: u8) {
        if let Some(value) = self.contents[register as usize].take() {
            self.value_registers[value.index()] = None;
        }
    }

    fn assign_register(&mut self, register: u8, value: NodeId) {
        debug_assert!(self.contents[register as usize].is_none());
        self.contents[register as usize] = Some(value);
        self.value_registers[value.index()] = Some(register);
    }

    /// Removes the value in `register` from it. The value stays available in
    /// its frame slot or its spill slot, which it gets if it had none.
    fn evict(&mut self, register: u8) {
        if let Some(value) = self.contents[register as usize] {
            self.ensure_home(value);
            self.free_register(register);
        }
    }

    /// How soon `value` is needed again, as a key that orders the values by it (smaller is sooner). Inside a loop,
    /// the uses later in the iteration come first, then the values the next iteration reads (live into the loop and
    /// used in it before here), in the order it reads them, then the values used after the loop.
    fn next_use(&self, value: NodeId) -> (u8, u32) {
        let uses = self.liveness.use_positions(value);
        let index = uses.partition_point(|position| *position <= self.position);
        let next = uses.get(index).copied();
        let Some(loop_) = self.loops.innermost_at(self.position) else {
            return next.map_or((3, 0), |position| (0, position));
        };
        if let Some(position) = self.loops.first_use_in(loop_, uses, index..uses.len()) {
            return (0, position);
        }
        if self.liveness.is_live_in(loop_.header, value)
            && let Some(position) = self.loops.first_use_in(loop_, uses, 0..index)
        {
            return (1, position - loop_.start);
        }
        next.map_or((3, 0), |position| (2, position))
    }

    /// A register of `class` that is not `blocked`, freeing one if necessary
    /// by evicting the value used furthest in the future.
    fn pick_register(&mut self, blocked: RegisterMask, class: RegisterClass) -> u8 {
        let candidates = || {
            self.registers
                .allocatable(class)
                .iter()
                .filter(move |register| !blocked.contains(*register))
        };
        if let Some(free) = candidates().find(|register| self.contents[*register as usize].is_none()) {
            return free;
        }
        let victim = candidates()
            .max_by_key(|register| {
                let value = self.contents[*register as usize].expect("no free register");
                (self.next_use(value), std::cmp::Reverse(*register))
            })
            .expect("a node needs more registers than the target has");
        self.evict(victim);
        victim
    }

    /// Where a value can be read from right now.
    fn current_location(&self, value: NodeId) -> Location {
        if let Some(bits) = self.graph.constant_value(value) {
            return Location::Constant(bits);
        }
        // NB: Exits find virtual objects through their properties.
        if matches!(self.graph.node(value).op, Op::VirtualObject { .. }) {
            return Location::Constant(crate::ir::value::EMPTY);
        }
        if let Some(register) = self.value_registers[value.index()] {
            return Location::Register(register);
        }
        if let Some(slot) = frame_home(self.graph, value) {
            return Location::Frame(slot);
        }
        let slot = self.allocation.nodes[value.index()]
            .spill_slot
            .unwrap_or_else(|| panic!("v{} is neither in a register nor spilled", value.0));
        Location::Stack(slot)
    }

    /// Whether `value`, which `node` uses or which is in a register at
    /// `node`, is still needed after it. Registers only ever hold live values.
    fn is_live_after(&self, value: NodeId, node: NodeId) -> bool {
        !self.liveness.dies_at(node).contains(&value)
    }

    /// Adds a move before the node that is being allocated.
    fn add_move(&mut self, from: Location, to: Location) -> Result<(), ColumnFull> {
        self.allocation.moves.try_push(Move { from, to })
    }

    fn allocate_node(&mut self, node_id: NodeId) -> Result<(), ColumnFull> {
        let node = self.graph.node(node_id);
        self.position = self.liveness.positions[node_id.index()];
        let constraints = lowering_info(node);
        // NB: The moves, the input locations and the exit values of the node go to the ends of their columns, where
        //     nothing else is added while the node is allocated.
        let moves_start = self.allocation.moves.len();
        let inputs_start = self.allocation.locations.len();
        self.allocation
            .locations
            .try_resize(inputs_start + node.inputs.len(), Location::Register(0))?;
        let constrained_inputs = node.inputs.len().min(constraints.inputs.len());
        let constraint_of = |allocator: &Self, index: usize| {
            allocator.registers.resolve(
                constraints.inputs[index],
                allocator.graph.constant_value(node.inputs[index]).is_some(),
            )
        };
        let mut blocked = RegisterMask::default();

        // Fixed register inputs first, so that inputs in any register do not
        // take their registers.
        for index in 0..constrained_inputs {
            let input = node.inputs[index];
            let InputConstraint::Fixed(register) = constraint_of(self, index) else {
                continue;
            };
            let register = self.registers.fixed(register);
            let is_value = is_allocated_value(self.graph, input);
            if !is_value || self.value_registers[input.index()] != Some(register) {
                debug_assert!(!blocked.contains(register), "two inputs need the same register");
                self.evict(register);
                let from = self.current_location(input);
                self.add_move(from, Location::Register(register))?;
                match from {
                    // The value moves, unless an earlier input of this node
                    // needs it where it is; then this is a copy for the node.
                    Location::Register(previous) if !blocked.contains(previous) => {
                        self.free_register(previous);
                        self.assign_register(register, input);
                    }
                    Location::Stack(_) => self.assign_register(register, input),
                    _ => {}
                }
            }
            blocked = blocked.with(register);
            self.allocation.locations[inputs_start + index] = Location::Register(register);
        }
        for index in 0..constrained_inputs {
            let input = node.inputs[index];
            let constraint = constraint_of(self, index);
            if constraint == InputConstraint::RegisterOrConstant
                && let Some(bits) = self.graph.constant_value(input)
            {
                self.allocation.locations[inputs_start + index] = Location::Constant(bits);
                continue;
            }
            if constraint == InputConstraint::Anywhere {
                self.allocation.locations[inputs_start + index] = self.current_location(input);
                continue;
            }
            if matches!(constraint, InputConstraint::Fixed(_)) {
                continue;
            }
            let register = match self.value_registers[input.index()] {
                Some(register) if is_allocated_value(self.graph, input) => register,
                _ => {
                    let register = self.pick_register(blocked, RegisterClass::of(self.graph.node(input).repr));
                    let from = self.current_location(input);
                    self.add_move(from, Location::Register(register))?;
                    if is_allocated_value(self.graph, input) {
                        self.assign_register(register, input);
                    }
                    register
                }
            };
            blocked = blocked.with(register);
            self.allocation.locations[inputs_start + index] = Location::Register(register);
        }

        let temp_count = usize::from(constraints.temps);
        assert!(
            temp_count <= MAX_TEMPS,
            "a node has more temps than an allocation has room for"
        );
        let mut temps = [0; MAX_TEMPS];
        for temp in &mut temps[..temp_count] {
            let register = self.pick_register(blocked, RegisterClass::General);
            blocked = blocked.with(register);
            *temp = register;
        }

        // NB: Two-address instructions (like those of x86) write their output
        //     over their first input, which then needs no copy where it dies
        //     here. Nodes that may exit keep their inputs for the exit.
        let first_input = self.allocation.locations[inputs_start..].first().copied();
        let first_input_register = match first_input {
            Some(Location::Register(register))
                if node.frame_state.is_none()
                    && constraints.reuses_first_input
                    && is_allocated_value(self.graph, node.inputs[0])
                    && self.liveness.dies_at(node_id).contains(&node.inputs[0])
                    && RegisterClass::of_register(register) == RegisterClass::of(node.repr)
                    && !temps[..temp_count].contains(&register) =>
            {
                Some(register)
            }
            _ => None,
        };

        // Pick the output register before recording where exits find their
        // values: a node may exit after writing its output (a check of a value
        // it loaded), so no exit value may stay in that register.
        let pick_output = |allocator: &mut Self| match constraints.output {
            OutputConstraint::None => None,
            OutputConstraint::Register => Some(
                first_input_register.unwrap_or_else(|| allocator.pick_register(blocked, RegisterClass::of(node.repr))),
            ),
            OutputConstraint::Return => {
                let register = allocator.registers.return_gpr;
                debug_assert!(constraints.clobbers == Clobbers::CallerSaved || !blocked.contains(register));
                allocator.evict(register);
                Some(register)
            }
        };
        let clobbered = match constraints.clobbers {
            Clobbers::CallerSaved => self.registers.caller_saved(),
            Clobbers::Floats => self.registers.allocatable(RegisterClass::Float),
            Clobbers::Nothing => RegisterMask::default(),
        };
        {
            for register in clobbered.iter() {
                if let Some(value) = self.contents.get(register as usize).copied().flatten() {
                    debug_assert!(
                        self.allocation.nodes[value.index()].spill_slot.is_some()
                            || frame_home(self.graph, value).is_some()
                            || !self.is_live_after(value, node_id),
                        "v{} is live across a call that clobbers its register without a spill slot",
                        value.0
                    );
                    self.free_register(register);
                }
            }
        }
        let output = pick_output(self);
        // Lazy exits of calls happen after the call returns, so they find
        // their values after the clobber.
        let exit_values_start = self.allocation.exit_values.len();
        if let Some(frame_state) = node.frame_state {
            for (slot, value) in self.graph.frame_state_values(frame_state) {
                let location = self.current_location(value);
                self.allocation.exit_values.try_push((slot, location))?;
            }
        }

        // Values whose last use is this node no longer need their registers.
        let liveness = self.liveness;
        for value in liveness.dies_at(node_id) {
            if let Some(register) = self.value_registers[value.index()] {
                self.free_register(register);
            }
        }

        if let Some(register) = output {
            self.assign_register(register, node_id);
            if self.liveness.needs_spill(node_id) {
                self.ensure_home(node_id);
            }
            if self.liveness.is_dead_output(node_id) {
                self.free_register(register);
            }
        }

        let moves_before = Run::new(moves_start, self.allocation.moves.len());
        let inputs = Run::new(inputs_start, self.allocation.locations.len());
        let exit_values = Run::new(exit_values_start, self.allocation.exit_values.len());
        let allocation = &mut self.allocation.nodes[node_id.index()];
        allocation.moves_before = moves_before;
        allocation.inputs = inputs;
        allocation.temps = temps;
        allocation.temp_count = constraints.temps;
        allocation.output = output.map(Location::Register);
        allocation.exit_values = exit_values;
        Ok(())
    }

    /// Gives the phis of the merge `join` their locations, at the end of its
    /// state predecessor: registers that hold no value live into the merge
    /// (preferably the input's own), or stack slots if there are none.
    fn place_merge_phis(&mut self, predecessor: BlockId, join: BlockId) {
        let mut taken = RegisterMask::default();
        for (register, value) in self.contents.iter().enumerate() {
            if value
                .is_some_and(|value| self.liveness.is_live_in(join, value) && !self.loop_leaves_in_stack(join, value))
            {
                taken = taken.with(u8::try_from(register).expect("register numbers fit in u8"));
            }
        }
        for (phi, input) in liveness::phi_inputs_from(self.graph, predecessor, join) {
            let own = self.value_registers[input.index()]
                .filter(|register| is_allocated_value(self.graph, input) && !taken.contains(*register));
            let register = own.or_else(|| {
                let mut free = self
                    .registers
                    .allocatable(RegisterClass::of(self.graph.node(phi).repr))
                    .iter()
                    .filter(|register| !taken.contains(*register));
                let first = free.next()?;
                std::iter::once(first)
                    .chain(free)
                    .find(|register| self.contents[*register as usize].is_none())
                    .or(Some(first))
            });
            self.place_phi(phi, register);
            if let Some(register) = register {
                taken = taken.with(register);
            }
        }
    }

    /// Adds the moves into the phis of `successor` at the end of `block`, as moves that happen in parallel.
    fn add_phi_moves(&mut self, block: BlockId, successor: BlockId) -> Result<(), ColumnFull> {
        for (phi, input) in liveness::phi_inputs_from(self.graph, block, successor) {
            let parallel_move = Move {
                from: self.current_location(input),
                to: self.allocation.nodes[phi.index()].output.expect("phis have locations"),
            };
            self.parallel_moves.try_push(parallel_move)?;
        }
        Ok(())
    }

    /// Adds the moves at the end of `block` that put the values live into the
    /// merge `join` (and its phis' inputs) where the merge expects them, as moves that happen in parallel.
    fn add_merge_moves(&mut self, block: BlockId, join: BlockId) -> Result<(), ColumnFull> {
        let contents = self.merge_contents(join).expect("the merge has a state predecessor");
        let phis = &self.graph.block(join).phis;
        for (register, value) in contents.iter().enumerate() {
            let Some(value) = *value else {
                continue;
            };
            if phis.contains(&value) {
                continue;
            }
            let to = Location::Register(u8::try_from(register).expect("register numbers fit in u8"));
            let from = self.current_location(value);
            if from != to {
                self.parallel_moves.try_push(Move { from, to })?;
            }
        }
        self.add_phi_moves(block, join)
    }

    /// The moves at the end of `block` that put the inputs of the phis of its
    /// successor in place, as a part of the moves of the allocation.
    fn phi_moves(&mut self, block: BlockId, cycle_temp: &mut Option<u32>) -> Result<Run, ColumnFull> {
        let liveness = self.liveness;
        let successors = liveness.successors.get(block.index());
        let [successor] = successors else {
            for successor in successors {
                debug_assert!(self.graph.block(*successor).phis.is_empty(), "critical edge into a phi");
            }
            return Ok(Run::default());
        };
        self.parallel_moves.clear();
        match state_predecessor(self.graph, *successor) {
            Some(predecessor) if predecessor != block => self.add_merge_moves(block, *successor)?,
            merge => {
                if merge.is_some() {
                    self.place_merge_phis(block, *successor);
                }
                self.add_phi_moves(block, *successor)?;
            }
        }
        if self.parallel_moves.is_empty() {
            return Ok(Run::default());
        }
        let temp = *cycle_temp.get_or_insert_with(|| {
            let slot = self.allocation.spill_slot_count;
            self.allocation.spill_slot_count += 1;
            slot
        });
        let start = self.allocation.moves.len();
        resolve_parallel_moves(self.parallel_moves, Location::Stack(temp), &mut self.allocation.moves)?;
        Ok(Run::new(start, self.allocation.moves.len()))
    }
}

/// The spill slot to store `phi`, which is in a register, to at the start
/// of its block, if it has one that its inputs did not fill already. Inputs
/// sharing the phi's slot (see `coalescing`) are in it from their own
/// definition on.
pub fn phi_spill_store(graph: &Graph, allocation: &Allocation, phi: NodeId) -> Option<u32> {
    let node = allocation.node(phi);
    let (Some(Location::Register(_)), Some(slot)) = (node.output, node.spill_slot) else {
        return None;
    };
    let filled = graph
        .node(phi)
        .inputs
        .iter()
        .all(|input| allocation.node(*input).spill_slot == Some(slot));
    (!filled).then_some(slot)
}

/// Prints the allocation of `graph` as text: every node with the moves
/// before it, the locations of its inputs and output, and its spill slot.
pub fn dump_allocation(graph: &Graph, allocation: &Allocation) -> String {
    use std::fmt::Write;
    let location_name = |location: &Location| match location {
        Location::Register(register) if *register >= FPR_BASE => format!("f{}", register - FPR_BASE),
        Location::Register(register) => format!("r{register}"),
        Location::Stack(slot) => format!("s{slot}"),
        Location::Constant(bits) => format!("#{}", crate::ir::value::describe(*bits)),
        Location::Frame(slot) => format!("frame{slot}"),
    };
    let moves_text = |moves: &[Move]| {
        moves
            .iter()
            .map(|m| format!("{} -> {}", location_name(&m.from), location_name(&m.to)))
            .collect::<Vec<_>>()
            .join(", ")
    };
    let mut out = String::new();
    for index in 0..graph.blocks.len() {
        let block = BlockId::from_index(index);
        writeln!(out, "b{index}:").unwrap();
        for node_id in graph.block_nodes(block) {
            let node = allocation.node(node_id);
            if Some(node_id) == graph.block(block).control && !allocation.block_end_moves(block).is_empty() {
                writeln!(out, "  moves {}", moves_text(allocation.block_end_moves(block))).unwrap();
            }
            if !node.moves_before.is_empty() {
                writeln!(out, "  moves {}", moves_text(node.moves_before)).unwrap();
            }
            write!(out, "  v{} {}", node_id.0, graph.node(node_id).op.name()).unwrap();
            if !node.inputs.is_empty() {
                let inputs = node.inputs.iter().map(location_name).collect::<Vec<_>>();
                write!(out, " ({})", inputs.join(", ")).unwrap();
            }
            if let Some(output) = &node.output {
                write!(out, " -> {}", location_name(output)).unwrap();
            }
            if !node.temps.is_empty() {
                let temps = node
                    .temps
                    .iter()
                    .map(|register| format!("r{register}"))
                    .collect::<Vec<_>>();
                write!(out, " temps {}", temps.join(", ")).unwrap();
            }
            if let Some(slot) = node.spill_slot
                && graph.node(node_id).op != Op::Phi
            {
                write!(out, " spill s{slot}").unwrap();
            }
            if !node.exit_values.is_empty() {
                let values = node
                    .exit_values
                    .iter()
                    .map(|(slot, location)| format!("{slot}={}", location_name(location)))
                    .collect::<Vec<_>>();
                write!(out, " exit {{{}}}", values.join(", ")).unwrap();
            }
            out.push('\n');
        }
    }
    writeln!(out, "spill slots: {}", allocation.spill_slot_count).unwrap();
    out
}

#[cfg(test)]
pub(crate) mod tests;
