/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The register allocator: one forward walk over the blocks in linear order, the approach of libjs_jit's
//! allocator on the WebAssembly IR.
//!
//! - Values get registers where their node is allocated, as the node's constraints say (`constraints()`). When no
//!   register is free, the value needed furthest in the future is evicted. Inside a loop, the values the rest of
//!   the iteration uses come first, then those the next iteration reads, then those only used after the loop.
//! - Spilling happens at the definition: a value that ever needs a stack slot gets one for its whole life, and code
//!   generation stores it there right after defining it, so a slot always holds its value once it has one. Values
//!   live across a call get one, since calls clobber every register.
//! - A block with one predecessor starts with that predecessor's registers. A merge starts with those of its last
//!   predecessor, and a loop header with those of the block entering the loop, without the values the loop only
//!   carries through that have a slot already. Their phis get registers that are free there, preferably those of
//!   the inputs from that predecessor. Every other edge into them moves its values into place, as parallel moves.
//!   Edges from blocks with several successors that need moves get a stub of their own in code generation.

pub mod liveness;
pub mod verify;

use crate::codegen;
use crate::convention::{self, Place, Register};
use crate::ir::{BlockId, Func, NONE, Op, Type, Value};
use crate::passes::cleanup::Liveliness;
use liveness::{Liveness, NO_HINT, block_start_position, node_position, predecessor_index};

/// No register.
pub const NO_REGISTER: u8 = u8::MAX;
/// An input that code generation uses as an immediate.
pub const CONSTANT_INPUT: u8 = u8::MAX - 1;
/// An input that code generation reads from its spill slot, as the memory operand of the instruction.
pub const SLOT_INPUT: u8 = u8::MAX - 2;

/// OPTIMIZATION: A spilled input is read from its slot by the instruction itself unless it is needed again within
/// this many positions (four per node), where loading it into a register once serves both uses.
const SLOT_INPUT_REUSE_DISTANCE: u32 = 4 * 8;

/// OPTIMIZATION: Parallel moves up to this many are ordered by rescanning them, more by counting their readers.
const SMALL_PARALLEL_MOVE: usize = 8;

/// No spill slot.
pub const NO_SLOT: i32 = i32::MIN;

const REGISTER_COUNT: usize = 32;

/// Where a value is, or where a move reads or writes.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum Location {
    Register(Register),
    /// A frame slot, by its offset from fp.
    Slot(i32),
    /// The outgoing area of a call: [rsp + 8 * index].
    Outgoing(u32),
    /// The result area of the function: [result area pointer + 8 * index].
    ResultArea(u32),
    /// A constant node, materialized where it is moved to. Only a source.
    Constant(Value),
}

/// A copy of a value. Moves between memory locations go through the scratch registers.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Move {
    pub from: Location,
    pub to: Location,
    /// Whether the value is a v128, which takes 16 bytes in memory.
    pub vector: bool,
}

/// How the allocator placed one node.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct NodeAllocation {
    /// The registers of the first three inputs, or CONSTANT_INPUT for immediates. Variadic nodes but stores get
    /// theirs moved into place instead.
    pub inputs: [u8; 3],
    pub output: u8,
    pub temps: [u8; 2],
    /// The moves before the node: `Allocation::moves[start..start + count]`.
    pub moves_start: u32,
    pub moves_count: u32,
}

impl Default for NodeAllocation {
    fn default() -> Self {
        Self {
            inputs: [NO_REGISTER; 3],
            output: NO_REGISTER,
            temps: [NO_REGISTER; 2],
            moves_start: 0,
            moves_count: 0,
        }
    }
}

#[derive(Default)]
pub struct Allocation {
    pub nodes: Vec<NodeAllocation>,
    pub moves: Vec<Move>,
    /// The moves of each edge, by its slot in `Func::successors`: (start, count) in `moves`.
    pub edge_moves: Vec<(u32, u32)>,
    /// The spill slot of each value, as an fp offset, or NO_SLOT.
    pub slots: Vec<i32>,
    pub slot_count: u32,
    /// The bytes of the outgoing area.
    pub outgoing_size: u32,
    /// The registers each block starts with, for the verifier: `block_states[block]`.
    pub block_states: Vec<[u32; REGISTER_COUNT]>,
    /// A slot for breaking cycles of edge moves.
    pub cycle_slot: i32,
}

impl Allocation {
    pub fn node(&self, value: Value) -> &NodeAllocation {
        &self.nodes[value as usize]
    }

    pub fn node_moves(&self, value: Value) -> &[Move] {
        let node = &self.nodes[value as usize];
        &self.moves[node.moves_start as usize..(node.moves_start + node.moves_count) as usize]
    }

    pub fn edge(&self, slot: u32) -> &[Move] {
        let (start, count) = self.edge_moves[slot as usize];
        &self.moves[start as usize..(start + count) as usize]
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum Input {
    Register,
    Fixed(Register),
    /// A register, or the constant itself if it fits the instruction as an immediate.
    RegisterOrConstant,
    /// A register, or a v128 constant itself, which the instruction reads from the constant pool.
    RegisterOrVectorConstant,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum Output {
    None,
    Register,
    Fixed(Register),
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum Temp {
    None,
    Gpr,
    Fpr,
    Fixed(Register),
}

struct Constraints {
    inputs: [Input; 3],
    output: Output,
    temps: [Temp; 2],
    /// The registers the node overwrites once it has read its inputs.
    clobbers: u32,
    /// Whether the output may take the first input's register where that input dies, as two-address instructions
    /// want.
    reuse_first: bool,
    /// The inputs code generation may read from their slots (`SLOT_INPUT`), as a mask.
    slot_inputs: u8,
}

/// Whether constant `value` fits an instruction as an immediate: sign-extended 32 bits for 64-bit operations.
pub fn fits_immediate(func: &Func, value: Value) -> bool {
    let node = func.node(value);
    match node.constant() {
        Some(bits) if node.ty.is_64() => i32::try_from(bits as i64).is_ok(),
        Some(_) => true,
        None => false,
    }
}

/// Whether a constant index and offset of a memory access fit its displacement.
pub fn fits_displacement(func: &Func, index: Value, offset: u32) -> bool {
    func.node(index)
        .constant()
        .is_some_and(|bits| (bits & 0xffff_ffff) + u64::from(offset) <= i32::MAX as u64)
}

fn constraints(func: &Func, value: Value) -> Constraints {
    use convention::{RAX, RCX, RDX};
    let node = func.node(value);
    let output = if node.ty == Type::None {
        Output::None
    } else {
        Output::Register
    };
    let mut constraints = Constraints {
        inputs: [Input::Register; 3],
        output,
        temps: [Temp::None; 2],
        clobbers: 0,
        reuse_first: false,
        slot_inputs: 0,
    };
    let input_type = |index: usize| func.node(func.inputs(value)[index]).ty;
    match node.op {
        Op::Add | Op::Sub | Op::And | Op::Or | Op::Xor | Op::Mul => {
            constraints.inputs[1] = Input::RegisterOrConstant;
            constraints.reuse_first = true;
            constraints.slot_inputs = 0b010;
        }
        Op::Shl | Op::ShrS | Op::ShrU => constraints.inputs[1] = Input::RegisterOrConstant,
        Op::Rotl | Op::Rotr => {
            if func.node(node.args[1]).op.is_constant() {
                constraints.inputs[1] = Input::RegisterOrConstant;
            } else {
                constraints.inputs[1] = Input::Fixed(RCX);
                constraints.reuse_first = true;
            }
        }
        Op::DivS | Op::DivU | Op::RemS | Op::RemU => {
            // NB: The divisor may not be in rdx, which the dividend's extension writes first.
            constraints.inputs[0] = Input::Fixed(RAX);
            constraints.temps[0] = Temp::Fixed(RDX);
            constraints.clobbers = (1 << RAX) | (1 << RDX);
            let quotient = matches!(node.op, Op::DivS | Op::DivU);
            constraints.output = Output::Fixed(if quotient { RAX } else { RDX });
        }
        Op::Compare | Op::BranchCompare => {
            constraints.inputs[1] = Input::RegisterOrConstant;
            constraints.slot_inputs = 0b010;
        }
        Op::FAdd | Op::FSub | Op::FMul | Op::FDiv => constraints.slot_inputs = 0b010,
        Op::FloatCompare => {
            if matches!(
                crate::ir::FloatCondition::from_flags(node.flags),
                crate::ir::FloatCondition::Eq | crate::ir::FloatCondition::Ne
            ) {
                constraints.temps[0] = Temp::Gpr;
            }
        }
        Op::FCopysign => constraints.temps[0] = Temp::Fpr,
        Op::Select => {
            if !node.ty.uses_xmm() {
                constraints.reuse_first = false;
            }
        }
        Op::TruncS | Op::TruncU | Op::TruncSatS | Op::TruncSatU => {
            constraints.temps[0] = Temp::Fpr;
        }
        Op::ConvertU if input_type(0) == Type::I64 => {
            constraints.temps[0] = Temp::Gpr;
        }
        Op::Load | Op::VectorLoad => {
            if fits_displacement(func, node.args[1], node.args[2]) {
                constraints.inputs[1] = Input::RegisterOrConstant;
            }
        }
        Op::Store => {
            let inputs = func.inputs(value);
            if fits_displacement(func, inputs[1], node.args[2]) {
                constraints.inputs[1] = Input::RegisterOrConstant;
            }
            constraints.inputs[2] = Input::RegisterOrConstant;
        }
        Op::GlobalSet => constraints.inputs[1] = Input::RegisterOrConstant,
        Op::WriteField => constraints.inputs[1] = Input::RegisterOrConstant,
        Op::CheckFunction => constraints.temps = [Temp::Gpr, Temp::Gpr],
        Op::CheckedAddress => {
            let size = 1u64 << (node.flags & 7);
            let offset = func.node(node.args[2]).constant();
            // NB: A memory64's offset can be close enough to 2^64 that adding the size wraps around.
            if offset
                .and_then(|offset| offset.checked_add(size))
                .is_some_and(|end| end <= i32::MAX as u64)
            {
                constraints.inputs[2] = Input::RegisterOrConstant;
            }
        }
        Op::AtomicRmw => {
            constraints.temps[0] = Temp::Fixed(RAX);
            if matches!(
                crate::ir::AtomicOp::from_index(node.args[2]),
                crate::ir::AtomicOp::And | crate::ir::AtomicOp::Or | crate::ir::AtomicOp::Xor
            ) {
                constraints.temps[1] = Temp::Gpr;
            }
            constraints.clobbers = 1 << RAX;
            constraints.output = Output::Fixed(RAX);
        }
        Op::AtomicCompareExchange => {
            constraints.inputs[1] = Input::Fixed(RAX);
            constraints.clobbers = 1 << RAX;
            constraints.output = Output::Fixed(RAX);
        }
        Op::RefTest => {
            constraints.inputs[1] = Input::RegisterOrConstant;
            constraints.temps = match crate::ir::HeapTest::from_flags(node.flags) {
                crate::ir::HeapTest::Struct | crate::ir::HeapTest::Array | crate::ir::HeapTest::ConcreteObject => {
                    [Temp::Gpr, Temp::None]
                }
                crate::ir::HeapTest::ConcreteFunction => [Temp::Gpr, Temp::Gpr],
                _ => [Temp::None, Temp::None],
            };
        }
        Op::BrTable => constraints.temps[0] = Temp::Gpr,
        Op::Shuffle | Op::VectorUnary | Op::VectorBinary | Op::VectorTernary => {
            for temp in constraints
                .temps
                .iter_mut()
                .take(codegen::x86_64::vector_temps(func, value))
            {
                *temp = Temp::Fpr;
            }
            if node.op == Op::VectorBinary && codegen::x86_64::is_vector_shift(node.args[2]) {
                constraints.inputs[1] = Input::RegisterOrConstant;
            }
            // NB: The operands that may be constants in the constant pool may be in their slots just as well.
            let constants = codegen::x86_64::vector_constant_inputs(func, value);
            for (index, input) in constraints.inputs.iter_mut().enumerate() {
                if constants & (1 << index) != 0 {
                    *input = Input::RegisterOrVectorConstant;
                }
            }
            constraints.slot_inputs = constants;
        }
        _ => {}
    }
    constraints
}

/// The register class of values of a type: true for xmm registers.
fn is_float(ty: Type) -> bool {
    ty.uses_xmm()
}

/// The reusable state of the allocator.
#[derive(Default)]
pub struct Allocator {
    pub liveness: Liveness,
    /// The time stamp counter ticks the last liveness computation took.
    pub liveness_ticks: u64,
    dominators: crate::passes::dominators::Dominators,
    contents: [u32; REGISTER_COUNT],
    value_registers: Vec<u8>,
    end_states: Vec<[u32; REGISTER_COUNT]>,
    /// The registers each loop's header starts with, for its back edges.
    header_states: Vec<[u32; REGISTER_COUNT]>,
    position: u32,
    current_block: BlockId,
    pending: Vec<Move>,
    resolved: Vec<Move>,
    /// How many pending moves read each location, in `resolve_many()`.
    readers: crate::table::Table<u64>,
    places: Vec<Place>,
    types: Vec<Type>,
    /// The inputs of the node being allocated, whose registers only it may take.
    inputs: Vec<Value>,
    /// The registers values may get of each class, in order of preference.
    gprs: &'static [Register],
    fprs: &'static [Register],
}

/// With `stress-registers`, the registers values get besides those of fixed constraints: few enough that most
/// values spill.
const STRESS_GPRS: [Register; 3] = [convention::RAX, convention::RBX, convention::R12];
const STRESS_FPRS: [Register; 2] = [convention::xmm(8), convention::xmm(9)];

impl Allocator {
    /// Allocates registers for `func` into `allocation`.
    /// Allocates registers for `func` into `allocation`, with only a few registers if `stress`.
    pub fn allocate_with(&mut self, func: &Func, liveliness: &Liveliness, allocation: &mut Allocation, stress: bool) {
        if stress {
            self.gprs = &STRESS_GPRS;
            self.fprs = &STRESS_FPRS;
        }
        self.allocate(func, liveliness, allocation);
        self.gprs = &convention::ALLOCATABLE_GPRS;
        self.fprs = &convention::ALLOCATABLE_FPRS;
    }

    pub fn allocate(&mut self, func: &Func, liveliness: &Liveliness, allocation: &mut Allocation) {
        if self.gprs.is_empty() {
            self.gprs = &convention::ALLOCATABLE_GPRS;
            self.fprs = &convention::ALLOCATABLE_FPRS;
        }
        let start = crate::ticks();
        self.liveness.compute(func, liveliness);
        self.liveness_ticks = crate::ticks() - start;
        self.dominators.compute(func);
        let count = func.nodes.len();
        allocation.nodes.clear();
        allocation.nodes.resize(count, NodeAllocation::default());
        allocation.moves.clear();
        allocation.edge_moves.clear();
        allocation.edge_moves.resize(func.successors.len(), (0, 0));
        allocation.slots.clear();
        allocation.slots.resize(count, NO_SLOT);
        allocation.slot_count = 0;
        allocation.outgoing_size = 0;
        allocation.block_states.clear();
        allocation
            .block_states
            .resize(func.blocks.len(), [NONE; REGISTER_COUNT]);
        allocation.cycle_slot = NO_SLOT;
        self.value_registers.clear();
        self.value_registers.resize(count, NO_REGISTER);
        self.contents = [NONE; REGISTER_COUNT];
        self.end_states.clear();
        self.end_states.resize(func.blocks.len(), [NONE; REGISTER_COUNT]);
        self.header_states.clear();
        self.header_states.resize(func.loops.len(), [NONE; REGISTER_COUNT]);

        for block in 0..func.blocks.len() as BlockId {
            self.current_block = block;
            self.start_block(func, liveliness, allocation, block);
            allocation.block_states[block as usize] = self.contents;
            let data = func.blocks[block as usize];
            for value in data.start..data.end {
                let op = func.node(value).op;
                if liveliness.is_dead(value) || matches!(op, Op::Phi | Op::Forward) || op.is_entry_value() {
                    continue;
                }
                self.allocate_node(func, liveliness, allocation, value);
            }
            self.end_states[block as usize] = self.contents;
            let successors_start = data.successors_start;
            for (index, &successor) in func.successors(block).iter().enumerate() {
                if successor <= block {
                    let loop_index = func.blocks[successor as usize].loop_index;
                    let state = self.header_states[loop_index as usize];
                    self.edge_moves(
                        func,
                        allocation,
                        block,
                        successor,
                        successors_start + index as u32,
                        &state,
                    );
                }
            }
        }
    }

    // Register state.

    fn set_contents(&mut self, contents: [u32; REGISTER_COUNT]) {
        for value in self.contents {
            if value != NONE {
                self.value_registers[value as usize] = NO_REGISTER;
            }
        }
        self.contents = contents;
        for (register, value) in contents.iter().enumerate() {
            if *value != NONE {
                self.value_registers[*value as usize] = register as u8;
            }
        }
    }

    fn assign(&mut self, register: Register, value: Value) {
        debug_assert_eq!(self.contents[register as usize], NONE);
        self.contents[register as usize] = value;
        self.value_registers[value as usize] = register;
    }

    fn free(&mut self, register: Register) {
        let value = self.contents[register as usize];
        if value != NONE {
            self.value_registers[value as usize] = NO_REGISTER;
            self.contents[register as usize] = NONE;
        }
    }

    fn ensure_slot(func: &Func, allocation: &mut Allocation, value: Value) -> i32 {
        let slot = allocation.slots[value as usize];
        if slot != NO_SLOT {
            return slot;
        }
        let node = func.node(value);
        let slot = match node.op {
            Op::Param => convention::home_slot_offset(convention::home_slot(&func.params, node.args[0] as usize)),
            Op::Instance => convention::FRAME_INSTANCE,
            _ => {
                // NB: A v128 takes two slots, the lower of which has its address.
                allocation.slot_count += node.ty.slots();
                convention::spill_slot_offset(allocation.slot_count - 1)
            }
        };
        allocation.slots[value as usize] = slot;
        slot
    }

    /// Frees `register`, keeping its value in its slot if it is still needed.
    fn evict(&mut self, func: &Func, allocation: &mut Allocation, register: Register) {
        let value = self.contents[register as usize];
        if value == NONE {
            return;
        }
        if self.liveness.is_live_after(value, self.position) {
            Self::ensure_slot(func, allocation, value);
        }
        self.free(register);
    }

    fn location(&self, func: &Func, allocation: &Allocation, value: Value) -> Location {
        if func.node(value).op.is_constant() {
            return Location::Constant(value);
        }
        let register = self.value_registers[value as usize];
        if register != NO_REGISTER {
            return Location::Register(register);
        }
        let slot = allocation.slots[value as usize];
        debug_assert!(slot != NO_SLOT, "v{value} is neither in a register nor in a slot");
        Location::Slot(slot)
    }

    /// How soon `value` is needed again, as a key that orders the values by it (smaller is sooner).
    fn eviction_key(&self, func: &Func, value: Value) -> (u8, u32) {
        let next = self.liveness.next_use(value, self.position);
        let loop_index = func.blocks[self.current_block as usize].loop_index;
        if loop_index == NONE {
            return next.map_or((3, 0), |position| (0, position));
        }
        let loop_end = self.liveness.loop_ends[loop_index as usize];
        if let Some(position) = next
            && position <= loop_end
        {
            return (0, position);
        }
        let loop_start = block_start_position(func, func.loops[loop_index as usize].header);
        if self.liveness.definition[value as usize] < loop_start
            && let Some(position) = self.liveness.next_use(value, loop_start.saturating_sub(1))
            && position <= self.position
        {
            return (1, position - loop_start);
        }
        next.map_or((3, 0), |position| (2, position))
    }

    /// A register of the class that is not `blocked`, preferably `hint`, freeing one if necessary.
    fn pick(&mut self, func: &Func, allocation: &mut Allocation, blocked: u32, float: bool, hint: u8) -> Register {
        let candidates = if float { self.fprs } else { self.gprs };
        if hint != NO_HINT
            && hint != NO_REGISTER
            && convention::is_fpr(hint) == float
            && blocked & (1 << hint) == 0
            && self.contents[hint as usize] == NONE
        {
            return hint;
        }
        for &register in candidates {
            if blocked & (1 << register) == 0 && self.contents[register as usize] == NONE {
                return register;
            }
        }
        let mut victim = NO_REGISTER;
        let mut best = (0, 0);
        // NB: Under stress-registers, a node may need more registers than the few values get; then it takes others.
        let full: &[Register] = if float {
            &convention::ALLOCATABLE_FPRS
        } else {
            &convention::ALLOCATABLE_GPRS
        };
        for candidates in [candidates, full] {
            for &register in candidates {
                if blocked & (1 << register) != 0 || self.inputs.contains(&self.contents[register as usize]) {
                    continue;
                }
                if self.contents[register as usize] == NONE {
                    return register;
                }
                let key = self.eviction_key(func, self.contents[register as usize]);
                if victim == NO_REGISTER || key > best {
                    victim = register;
                    best = key;
                }
            }
            if victim != NO_REGISTER {
                break;
            }
        }
        assert!(victim != NO_REGISTER, "a node needs more registers than there are");
        self.evict(func, allocation, victim);
        victim
    }

    // Blocks.

    /// The registers of `state` that hold values live at the start of `block`. For merges, also only those whose
    /// definition dominates it: an interval can span a merge that some paths reach without its definition, where
    /// the value is dead, like a block the definition branches over.
    fn filtered(&self, func: &Func, state: &[u32; REGISTER_COUNT], block: BlockId) -> [u32; REGISTER_COUNT] {
        let position = block_start_position(func, block);
        let is_merge = func.predecessors(block).len() > 1;
        let mut result = *state;
        for value in &mut result {
            if *value != NONE
                && (!self.liveness.is_live_at(*value, position)
                    || (is_merge && !self.dominators.dominates(self.liveness.block[*value as usize], block)))
            {
                *value = NONE;
            }
        }
        result
    }

    fn start_block(&mut self, func: &Func, liveliness: &Liveliness, allocation: &mut Allocation, block: BlockId) {
        self.position = block_start_position(func, block);
        if block == 0 {
            self.start_entry(func, liveliness, allocation);
            return;
        }
        let predecessors = func.predecessors(block);
        if func.is_loop_header(block) {
            let entry = predecessors[0];
            debug_assert!(entry < block);
            let mut state = self.filtered(func, &self.end_states[entry as usize], block);
            // The values the loop only carries through stay in their slots, if they have one, so that the loop can
            // use their registers.
            let loop_index = func.blocks[block as usize].loop_index;
            let loop_end = self.liveness.loop_ends[loop_index as usize];
            for value in &mut state {
                if *value != NONE
                    && allocation.slots[*value as usize] != NO_SLOT
                    && self
                        .liveness
                        .next_use(*value, self.position)
                        .is_none_or(|position| position > loop_end)
                {
                    *value = NONE;
                }
            }
            self.place_phis(func, allocation, block, entry, &mut state);
            // NB: Every forward edge gets its moves here: the entry's, and an OSR compile's second way in. The back
            //     edges get theirs where their blocks end.
            for index in 0..predecessors.len() {
                let predecessor = func.predecessors(block)[index];
                if predecessor < block {
                    let slot = edge_slot(func, predecessor, block);
                    self.edge_moves(func, allocation, predecessor, block, slot, &state);
                }
            }
            self.header_states[loop_index as usize] = state;
            self.set_contents(state);
            return;
        }
        if predecessors.len() == 1 {
            let state = self.filtered(func, &self.end_states[predecessors[0] as usize], block);
            self.set_contents(state);
            return;
        }
        let state_predecessor = *predecessors.iter().max().expect("merges have predecessors");
        let mut state = self.filtered(func, &self.end_states[state_predecessor as usize], block);
        self.place_phis(func, allocation, block, state_predecessor, &mut state);
        for index in 0..func.predecessors(block).len() {
            let predecessor = func.predecessors(block)[index];
            let slot = edge_slot(func, predecessor, block);
            self.edge_moves(func, allocation, predecessor, block, slot, &state);
        }
        let _ = liveliness;
        self.set_contents(state);
    }

    fn start_entry(&mut self, func: &Func, liveliness: &Liveliness, allocation: &mut Allocation) {
        let mut state = [NONE; REGISTER_COUNT];
        self.places.clear();
        convention::parameter_places(&func.params, &mut self.places);
        let data = func.blocks[0];
        for value in data.start..data.end {
            let node = func.node(value);
            if liveliness.is_dead(value) {
                continue;
            }
            match node.op {
                Op::Instance => state[convention::INSTANCE_REGISTER as usize] = value,
                Op::MemoryBase => state[convention::MEMORY_BASE_REGISTER as usize] = value,
                Op::Param => match self.places[node.args[0] as usize] {
                    Place::Register(register) => state[register as usize] = value,
                    Place::Memory(index) => allocation.slots[value as usize] = convention::home_slot_offset(index),
                },
                _ => {}
            }
        }
        for (register, value) in state.iter().enumerate() {
            if *value != NONE {
                allocation.nodes[*value as usize].output = register as u8;
            }
        }
        self.set_contents(state);
    }

    /// Gives the phis of `block` their locations in `state`, which starts as the registers at the end of
    /// `predecessor`.
    fn place_phis(
        &mut self,
        func: &Func,
        allocation: &mut Allocation,
        block: BlockId,
        predecessor: BlockId,
        state: &mut [u32; REGISTER_COUNT],
    ) {
        let index = predecessor_index(func, block, predecessor);
        let end = &self.end_states[predecessor as usize];
        for phi in func.phis(block) {
            let float = is_float(func.node(phi).ty);
            let input = func.inputs(phi)[index];
            let mut register = NO_REGISTER;
            if let Some(own) = end.iter().position(|value| *value == input)
                && state[own] == NONE
                && convention::is_fpr(own as u8) == float
            {
                register = own as u8;
            }
            if register == NO_REGISTER {
                let candidates = if float { self.fprs } else { self.gprs };
                let hint = self.liveness.hint[phi as usize];
                if hint != NO_HINT && convention::is_fpr(hint) == float && state[hint as usize] == NONE {
                    register = hint;
                } else if let Some(&free) = candidates.iter().find(|register| state[**register as usize] == NONE) {
                    register = free;
                }
            }
            if register == NO_REGISTER {
                Self::ensure_slot(func, allocation, phi);
            } else {
                state[register as usize] = phi;
            }
            allocation.nodes[phi as usize].output = register;
        }
    }

    /// The moves on the edge from `from` (allocated, its registers in `end_states`, or the current ones for the
    /// current block) to `to`, which starts with `target`.
    fn edge_moves(
        &mut self,
        func: &Func,
        allocation: &mut Allocation,
        from: BlockId,
        to: BlockId,
        slot: u32,
        target: &[u32; REGISTER_COUNT],
    ) {
        let source = self.end_states[from as usize];
        let index = predecessor_index(func, to, from);
        let location_at_end = |value: Value, allocation: &Allocation| -> Location {
            if func.node(value).op.is_constant() {
                return Location::Constant(value);
            }
            if let Some(register) = source.iter().position(|candidate| *candidate == value) {
                return Location::Register(register as u8);
            }
            let slot = allocation.slots[value as usize];
            debug_assert!(
                slot != NO_SLOT,
                "v{value} is live on the edge b{from} -> b{to} but nowhere"
            );
            Location::Slot(slot)
        };
        self.pending.clear();
        for (register, &value) in target.iter().enumerate() {
            if value == NONE {
                continue;
            }
            let node = func.node(value);
            let vector = node.ty == Type::V128;
            let from_location = if node.op == Op::Phi && self.liveness.block[value as usize] == to {
                location_at_end(func.inputs(value)[index], allocation)
            } else {
                location_at_end(value, allocation)
            };
            let to_location = Location::Register(register as u8);
            if from_location != to_location {
                self.pending.push(Move {
                    from: from_location,
                    to: to_location,
                    vector,
                });
            }
        }
        for phi in func.phis(to) {
            if allocation.nodes[phi as usize].output != NO_REGISTER {
                continue;
            }
            let from_location = location_at_end(func.inputs(phi)[index], allocation);
            let to_location = Location::Slot(allocation.slots[phi as usize]);
            if from_location != to_location {
                self.pending.push(Move {
                    from: from_location,
                    to: to_location,
                    vector: func.node(phi).ty == Type::V128,
                });
            }
        }
        if self.pending.is_empty() {
            allocation.edge_moves[slot as usize] = (allocation.moves.len() as u32, 0);
            return;
        }
        let start = allocation.moves.len() as u32;
        self.resolve(func, allocation);
        allocation.edge_moves[slot as usize] = (start, allocation.moves.len() as u32 - start);
    }

    /// Orders the parallel moves in `pending` into a sequence with the same effect, appended to the allocation's
    /// moves. Cycles go through the cycle slot.
    fn resolve(&mut self, func: &Func, allocation: &mut Allocation) {
        let _ = func;
        self.resolved.clear();
        if self.pending.len() > SMALL_PARALLEL_MOVE {
            self.resolve_many(allocation);
            allocation.moves.extend_from_slice(&self.resolved);
            return;
        }
        while !self.pending.is_empty() {
            let ready = self
                .pending
                .iter()
                .position(|candidate| self.pending.iter().all(|other| other.from != candidate.to));
            if let Some(index) = ready {
                let ready = self.pending.swap_remove(index);
                self.resolved.push(ready);
                continue;
            }
            // Every destination is still a source: the moves form cycles. Save one destination in the cycle slot.
            if allocation.cycle_slot == NO_SLOT {
                // NB: Two slots, in case the value is a v128.
                allocation.slot_count += 2;
                allocation.cycle_slot = convention::spill_slot_offset(allocation.slot_count - 1);
            }
            let saved = self.pending[0].to;
            let temp = Location::Slot(allocation.cycle_slot);
            let vector = self.pending[0].vector;
            self.resolved.push(Move {
                from: saved,
                to: temp,
                vector,
            });
            for pending in &mut self.pending {
                if pending.from == saved {
                    pending.from = temp;
                }
            }
        }
        allocation.moves.extend_from_slice(&self.resolved);
    }

    /// `resolve()` for many moves, like the edges into a merge with many phis: it counts the readers of each
    /// location, so that finding the moves whose destination nobody reads any more takes one pass, not one per move.
    fn resolve_many(&mut self, allocation: &mut Allocation) {
        fn key(location: Location) -> u64 {
            match location {
                Location::Register(register) => (1 << 40) | u64::from(register),
                Location::Slot(slot) => (2 << 40) | u64::from(slot as u32),
                Location::Outgoing(index) => (3 << 40) | u64::from(index),
                Location::ResultArea(index) => (4 << 40) | u64::from(index),
                Location::Constant(value) => (5 << 40) | u64::from(value),
            }
        }
        let readers = &mut self.readers;
        readers.clear();
        let add = |readers: &mut crate::table::Table<u64>, location: Location, delta: i64| {
            let count = i64::from(readers.get(key(location)).unwrap_or(0)) + delta;
            readers.insert(key(location), count as u32);
        };
        for pending in &self.pending {
            add(readers, pending.from, 1);
        }
        while !self.pending.is_empty() {
            let mut progress = false;
            let mut index = 0;
            while index < self.pending.len() {
                if readers.get(key(self.pending[index].to)).unwrap_or(0) == 0 {
                    let ready = self.pending.swap_remove(index);
                    add(readers, ready.from, -1);
                    self.resolved.push(ready);
                    progress = true;
                } else {
                    index += 1;
                }
            }
            if progress || self.pending.is_empty() {
                continue;
            }
            // Every destination is still a source: the moves form cycles. Save one destination in the cycle slot.
            if allocation.cycle_slot == NO_SLOT {
                // NB: Two slots, in case the value is a v128.
                allocation.slot_count += 2;
                allocation.cycle_slot = convention::spill_slot_offset(allocation.slot_count - 1);
            }
            let saved = self.pending[0].to;
            let temp = Location::Slot(allocation.cycle_slot);
            let vector = self.pending[0].vector;
            self.resolved.push(Move {
                from: saved,
                to: temp,
                vector,
            });
            for pending in &mut self.pending {
                if pending.from == saved {
                    pending.from = temp;
                    add(readers, saved, -1);
                    add(readers, temp, 1);
                }
            }
        }
    }

    // Nodes.

    fn allocate_node(&mut self, func: &Func, liveliness: &Liveliness, allocation: &mut Allocation, value: Value) {
        self.position = node_position(value);
        let node = *func.node(value);
        let moves_start = allocation.moves.len() as u32;
        self.inputs.clear();
        self.inputs.extend_from_slice(func.inputs(value));
        match node.op {
            Op::Call => self.allocate_call(func, allocation, value),
            Op::TailCall => self.place_call_operands(func, allocation, value),
            Op::Helper => self.allocate_helper(func, allocation, value),
            Op::Return => self.allocate_return(func, allocation, value),
            Op::CallResult => self.allocate_call_result(func, allocation, value),
            _ => self.allocate_simple(func, allocation, value),
        }
        let _ = liveliness;
        let record = &mut allocation.nodes[value as usize];
        record.moves_start = moves_start;
        record.moves_count = allocation.moves.len() as u32 - moves_start;
    }

    fn move_to(&mut self, allocation: &mut Allocation, from: Location, to: Location, vector: bool) {
        allocation.moves.push(Move { from, to, vector });
    }

    /// Frees `register` for the node being allocated. An input of the node in it moves to another register, or to
    /// its slot if none is free.
    fn make_room(&mut self, func: &Func, allocation: &mut Allocation, register: Register, blocked: u32) {
        let occupant = self.contents[register as usize];
        if occupant == NONE {
            return;
        }
        if !self.inputs.contains(&occupant) {
            self.evict(func, allocation, register);
            return;
        }
        let float = convention::is_fpr(register);
        let candidates = if float { self.fprs } else { self.gprs };
        let free = candidates.iter().copied().find(|candidate| {
            *candidate != register && blocked & (1 << candidate) == 0 && self.contents[*candidate as usize] == NONE
        });
        match free {
            Some(free) => {
                let vector = func.node(occupant).ty == Type::V128;
                self.move_to(
                    allocation,
                    Location::Register(register),
                    Location::Register(free),
                    vector,
                );
                self.free(register);
                self.assign(free, occupant);
            }
            None => {
                Self::ensure_slot(func, allocation, occupant);
                self.free(register);
            }
        }
    }

    /// Puts `input` in the fixed register `register` for a node, with the registers in `blocked` taken.
    fn fix_input(&mut self, func: &Func, allocation: &mut Allocation, input: Value, register: Register, blocked: u32) {
        if !func.node(input).op.is_constant() && self.value_registers[input as usize] == register {
            return;
        }
        self.make_room(func, allocation, register, blocked | (1 << register));
        let from = self.location(func, allocation, input);
        let vector = func.node(input).ty == Type::V128;
        self.move_to(allocation, from, Location::Register(register), vector);
        match from {
            Location::Register(previous) if blocked & (1 << previous) == 0 => {
                self.free(previous);
                self.assign(register, input);
            }
            Location::Slot(_) => self.assign(register, input),
            _ => {}
        }
    }

    /// Whether the node being allocated should read `input`, one of its inputs that may be a memory operand, from its
    /// slot: it is in no register, the node takes it once, and it is not needed again soon.
    fn reads_from_slot(&self, func: &Func, allocation: &Allocation, input: Value) -> bool {
        if func.node(input).op.is_constant()
            || self.value_registers[input as usize] != NO_REGISTER
            || allocation.slots[input as usize] == NO_SLOT
            || self.inputs.iter().filter(|other| **other == input).count() != 1
        {
            return false;
        }
        self.liveness
            .next_use(input, self.position)
            .is_none_or(|next| next > self.position + SLOT_INPUT_REUSE_DISTANCE)
    }

    /// Puts `input` in some register for a node, with the registers in `blocked` taken.
    fn register_input(&mut self, func: &Func, allocation: &mut Allocation, input: Value, blocked: u32) -> Register {
        let float = is_float(func.node(input).ty);
        if !func.node(input).op.is_constant() {
            let register = self.value_registers[input as usize];
            if register != NO_REGISTER {
                return register;
            }
        }
        let register = self.pick(func, allocation, blocked, float, NO_HINT);
        let from = self.location(func, allocation, input);
        let vector = func.node(input).ty == Type::V128;
        self.move_to(allocation, from, Location::Register(register), vector);
        if !func.node(input).op.is_constant() {
            self.assign(register, input);
        }
        register
    }

    fn free_dying_inputs(&mut self, func: &Func, value: Value) {
        for &input in func.inputs(value) {
            if func.node(input).op.is_constant() {
                continue;
            }
            let register = self.value_registers[input as usize];
            if register != NO_REGISTER && !self.liveness.is_live_after(input, self.position) {
                self.free(register);
            }
        }
    }

    fn define(&mut self, func: &Func, allocation: &mut Allocation, value: Value, register: Register) {
        self.assign(register, value);
        allocation.nodes[value as usize].output = register;
        if self.liveness.end[value as usize] <= self.position {
            self.free(register);
        }
        let _ = func;
    }

    fn allocate_simple(&mut self, func: &Func, allocation: &mut Allocation, value: Value) {
        let constraints = constraints(func, value);
        let node = *func.node(value);
        let inputs = func.inputs(value);
        let mut blocked = 0u32;
        let mut registers = [NO_REGISTER; 3];
        // Fixed temps first, then fixed inputs, so that other inputs do not take their registers.
        for temp in constraints.temps {
            if let Temp::Fixed(register) = temp {
                self.make_room(func, allocation, register, blocked | (1 << register));
                blocked |= 1 << register;
            }
        }
        for (index, &input) in inputs.iter().enumerate().take(3) {
            if let Input::Fixed(register) = constraints.inputs[index] {
                self.fix_input(func, allocation, input, register, blocked);
                blocked |= 1 << register;
                registers[index] = register;
            }
        }
        for (index, &input) in inputs.iter().enumerate().take(3) {
            match constraints.inputs[index] {
                Input::Fixed(_) => continue,
                Input::RegisterOrConstant if func.node(input).op.is_constant() && fits_immediate(func, input) => {
                    registers[index] = CONSTANT_INPUT;
                }
                Input::RegisterOrVectorConstant if func.node(input).op == Op::Const128 => {
                    registers[index] = CONSTANT_INPUT;
                }
                _ if constraints.slot_inputs & (1 << index) != 0 && self.reads_from_slot(func, allocation, input) => {
                    registers[index] = SLOT_INPUT;
                }
                _ => {
                    let register = self.register_input(func, allocation, input, blocked);
                    blocked |= 1 << register;
                    registers[index] = register;
                }
            }
        }
        for register in 0..REGISTER_COUNT as Register {
            if constraints.clobbers & (1 << register) != 0 {
                self.evict(func, allocation, register);
            }
        }
        let mut temps = [NO_REGISTER; 2];
        for (index, temp) in constraints.temps.iter().enumerate() {
            temps[index] = match temp {
                Temp::None => NO_REGISTER,
                Temp::Fixed(register) => *register,
                Temp::Gpr | Temp::Fpr => {
                    let register = self.pick(func, allocation, blocked, *temp == Temp::Fpr, NO_HINT);
                    blocked |= 1 << register;
                    register
                }
            };
        }
        let output = match constraints.output {
            Output::None => NO_REGISTER,
            Output::Fixed(register) => {
                self.evict(func, allocation, register);
                register
            }
            Output::Register => {
                let first = inputs.first().copied().unwrap_or(NONE);
                let float = is_float(node.ty);
                if constraints.reuse_first
                    && registers[0] != CONSTANT_INPUT
                    && registers[0] != NO_REGISTER
                    && !func.node(first).op.is_constant()
                    && !self.liveness.is_live_after(first, self.position)
                    && convention::is_fpr(registers[0]) == float
                    && !inputs[1..].contains(&first)
                {
                    registers[0]
                } else {
                    self.pick(func, allocation, blocked, float, self.liveness.hint[value as usize])
                }
            }
        };
        self.free_dying_inputs(func, value);
        if output != NO_REGISTER {
            if self.contents[output as usize] != NONE {
                // The first input, which dies here.
                self.free(output);
            }
            self.define(func, allocation, value, output);
        }
        let record = &mut allocation.nodes[value as usize];
        record.inputs = registers;
        record.temps = temps;
    }

    fn allocate_call(&mut self, func: &Func, allocation: &mut Allocation, value: Value) {
        self.place_call_operands(func, allocation, value);
        // The call clobbers every register.
        for register in 0..REGISTER_COUNT as Register {
            if self.contents[register as usize] != NONE {
                self.evict(func, allocation, register);
            }
        }
        let node = func.node(value);
        if node.ty != Type::None {
            let register = if node.ty.uses_xmm() {
                convention::FPR_RESULTS[0]
            } else {
                convention::GPR_RESULTS[0]
            };
            self.define(func, allocation, value, register);
        }
    }

    fn allocate_helper(&mut self, func: &Func, allocation: &mut Allocation, value: Value) {
        let mut blocked = 0u32;
        for (index, &input) in func.inputs(value).iter().enumerate() {
            let register = convention::HELPER_REGISTERS[index];
            self.fix_input(func, allocation, input, register, blocked);
            blocked |= 1 << register;
        }
        for register in 0..REGISTER_COUNT as Register {
            if convention::HOST_CLOBBERS & (1 << register) != 0 && self.contents[register as usize] != NONE {
                self.evict(func, allocation, register);
            }
        }
        if func.node(value).ty != Type::None {
            self.define(func, allocation, value, convention::RAX);
        }
    }

    /// Moves the operands of a call or tail call where the convention wants them.
    fn place_call_operands(&mut self, func: &Func, allocation: &mut Allocation, value: Value) {
        let inputs = func.inputs(value);
        let (header, instance, memory_base) = (inputs[0], inputs[1], inputs[2]);
        let arguments = &inputs[3..];
        self.types.clear();
        self.types
            .extend(arguments.iter().map(|argument| func.node(*argument).ty));
        self.places.clear();
        convention::parameter_places(&self.types, &mut self.places);
        // Arguments passed in memory first: they only read the registers.
        for (index, &argument) in arguments.iter().enumerate() {
            if let Place::Memory(slot) = self.places[index] {
                let from = self.location(func, allocation, argument);
                let vector = func.node(argument).ty == Type::V128;
                self.move_to(allocation, from, Location::Outgoing(slot), vector);
            }
        }
        let mut blocked = 0u32;
        for (index, &argument) in arguments.iter().enumerate() {
            if let Place::Register(register) = self.places[index] {
                self.fix_input(func, allocation, argument, register, blocked);
                blocked |= 1 << register;
            }
        }
        for (input, register) in [
            (header, convention::CALLEE_REGISTER),
            (instance, convention::INSTANCE_REGISTER),
            (memory_base, convention::MEMORY_BASE_REGISTER),
        ] {
            self.fix_input(func, allocation, input, register, blocked);
            blocked |= 1 << register;
        }
        let mut parameters = std::mem::take(&mut self.types);
        parameters.clear();
        parameters.extend(arguments.iter().map(|argument| func.node(*argument).ty));
        let call_results = call_result_types(func, value);
        // NB: A tail call passes on the function's result area.
        let results: &[Type] = if func.node(value).op == Op::TailCall {
            &func.results
        } else {
            &call_results
        };
        allocation.outgoing_size = allocation
            .outgoing_size
            .max(convention::outgoing_size(&parameters, results));
        self.types = parameters;
    }

    fn allocate_call_result(&mut self, func: &Func, allocation: &mut Allocation, value: Value) {
        let node = func.node(value);
        let call = node.args[0];
        let index = node.args[1] as usize;
        let results = call_result_types(func, call);
        self.places.clear();
        convention::result_places(&results, &mut self.places);
        let register = match self.places[index] {
            Place::Register(register) => {
                self.evict(func, allocation, register);
                register
            }
            Place::Memory(_) => self.pick(
                func,
                allocation,
                0,
                node.ty.uses_xmm(),
                self.liveness.hint[value as usize],
            ),
        };
        self.define(func, allocation, value, register);
    }

    fn allocate_return(&mut self, func: &Func, allocation: &mut Allocation, value: Value) {
        let inputs = func.inputs(value);
        self.places.clear();
        convention::result_places(&func.results, &mut self.places);
        for (index, &result) in inputs.iter().enumerate() {
            if let Place::Memory(slot) = self.places[index] {
                let from = self.location(func, allocation, result);
                let vector = func.node(result).ty == Type::V128;
                self.move_to(allocation, from, Location::ResultArea(slot), vector);
            }
        }
        let mut blocked = 0u32;
        for (index, &result) in inputs.iter().enumerate() {
            if let Place::Register(register) = self.places[index] {
                self.fix_input(func, allocation, result, register, blocked);
                blocked |= 1 << register;
            }
        }
        self.free_dying_inputs(func, value);
    }
}

/// The result types of a call, from its node and the `CallResult`s right after it, which cleanup keeps.
pub fn call_result_types(func: &Func, call: Value) -> crate::inline_vec::InlineVec<Type, 4> {
    let mut types = crate::inline_vec::InlineVec::new();
    let node = func.node(call);
    if node.ty != Type::None {
        types.push(node.ty);
        let mut next = call + 1;
        while (next as usize) < func.nodes.len()
            && func.node(next).op == Op::CallResult
            && func.node(next).args[0] == call
        {
            types.push(func.node(next).ty);
            next += 1;
        }
    }
    types
}

/// The slot in `Func::successors` of the edge from `from` to `to`.
pub fn edge_slot(func: &Func, from: BlockId, to: BlockId) -> u32 {
    let data = &func.blocks[from as usize];
    let index = func
        .successors(from)
        .iter()
        .position(|successor| *successor == to)
        .expect("predecessors list their successors");
    data.successors_start + index as u32
}
