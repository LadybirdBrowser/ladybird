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
//! definition. Values live across a call always get one.
//!
//! A merge or loop header starts with the register state of its first
//! predecessor in linear order (its "state predecessor"; for a loop header,
//! the block entering the loop), and its phis get registers that are free
//! there. The other predecessors, back edges included, move their values (and
//! their phis' inputs) into place at their end, as parallel moves. The rare
//! merges whose other predecessors do not jump to them start with every value
//! in its stack slot (see `liveness::state_predecessor()`).
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
//! around their calls (see `saves_only_gprs()`) clobber the floating point
//! ones, so float64 values live across them get stack slots, like values
//! live across a call.

mod liveness;
mod moves;
mod verify;

pub use moves::resolve_parallel_moves;
pub use verify::verify;

use crate::code::Repr;
use crate::inline_vec::InlineVec;
use crate::ir::BlockId;
use crate::ir::BranchCondition;
use crate::ir::Graph;
use crate::ir::Node;
use crate::ir::NodeId;
use crate::ir::Op;
use liveness::ValueLiveness;
use liveness::is_allocated_value;
use liveness::state_predecessor;

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

/// Whether the lowering of `op` calls out of line keeping only the general
/// purpose registers (register-saving `CallSlowPath`s, the growth of arrays and named storage, the runtime's
/// cache probes, and allocations that call the runtime): it clobbers every
/// floating point register.
pub fn saves_only_gprs(op: &Op) -> bool {
    matches!(
        op,
        Op::CallSlowPath {
            saves_registers: true,
            ..
        } | Op::CallArrayPush
            | Op::AddNamed { .. }
            | Op::ProbeKeyedStore { .. }
            | Op::ProbePropertyCache { .. }
            | Op::ProbePropertyStore { .. }
            | Op::ProbeHasProperty { .. }
            | Op::AllocateObject { .. }
            | Op::AllocateArray { .. }
            | Op::AllocateFunction { .. }
            | Op::AllocateEnvironment { .. }
    )
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

    /// The registers of both classes a call clobbers.
    pub fn caller_saved(&self) -> RegisterMask {
        RegisterMask(self.caller_saved_gprs.0 | (self.caller_saved_fprs.0 << FPR_BASE))
    }

    /// The fewest allocatable registers allocation works with: the ones
    /// calls take their arguments and return their results in, and
    /// `extra` more.
    pub fn few_registers(&self, extra: u32) -> RegisterMask {
        let mut mask = RegisterMask(0).with(self.return_gpr);
        for register in &self.argument_gprs {
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

/// How the allocator placed one node.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct NodeAllocation {
    /// Moves to perform, in order, right before the node.
    pub moves_before: Vec<Move>,
    /// Where the node finds each input: a register, or a constant for
    /// inputs that may be immediates.
    pub inputs: InlineVec<Location, 4>,
    pub temps: InlineVec<u8, 4>,
    /// Where the node puts its value: a register, or a stack slot for phis.
    pub output: Option<Location>,
    /// If set and the output is a register, the value must be stored to this
    /// spill slot right after its definition.
    pub spill_slot: Option<u32>,
    /// For nodes with a frame state, where the exit finds each of its values,
    /// as (frame slot, location).
    pub exit_values: Vec<(u32, Location)>,
}

#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct Allocation {
    /// Indexed by `NodeId`.
    pub nodes: Vec<NodeAllocation>,
    /// Moves to perform, in order, at the end of each block, after its body
    /// and before its control node. Only blocks ending in a jump to a merge
    /// or a loop header have any.
    pub block_end_moves: Vec<Vec<Move>>,
    pub spill_slot_count: u32,
    /// The values live into each block, indexed by `BlockId`.
    pub live_in: Vec<crate::bitset::BitSet>,
}

impl Allocation {
    pub fn node(&self, node: NodeId) -> &NodeAllocation {
        &self.nodes[node.index()]
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
enum InputConstraint {
    #[default]
    Register,
    FixedRegister(u8),
    /// A register, or the constant itself if the input is one, which code
    /// generation uses as an immediate.
    RegisterOrConstant,
    /// Wherever the value is: a register, its stack slot or a constant.
    Anywhere,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum OutputConstraint {
    None,
    Register,
    FixedRegister(u8),
}

struct Constraints {
    inputs: InlineVec<InputConstraint, 4>,
    output: OutputConstraint,
    temps: u8,
    is_call: bool,
}

fn constraints(node: &Node, registers: &RegisterSet) -> Constraints {
    let register_inputs = || InlineVec::from_elem(InputConstraint::Register, node.inputs.len());
    let output = if node.repr.is_some() {
        OutputConstraint::Register
    } else {
        OutputConstraint::None
    };
    let mut constraints = Constraints {
        inputs: register_inputs(),
        output,
        temps: 0,
        is_call: node.op.properties().is_call,
    };
    match node.op {
        Op::Constant(_) | Op::Phi => unreachable!("constants and phis have no constraints"),
        Op::ToBoolean => {
            constraints.inputs = InlineVec::from_elem(InputConstraint::FixedRegister(registers.argument_gprs[0]), 1);
            constraints.output = OutputConstraint::FixedRegister(registers.return_gpr);
        }
        // NB: The VM is the first argument.
        Op::PrimitiveToString | Op::ToObject | Op::ArrayCreate => {
            constraints.inputs = (0..node.inputs.len())
                .map(|index| InputConstraint::FixedRegister(registers.argument_gprs[index + 1]))
                .collect();
            constraints.output = OutputConstraint::FixedRegister(registers.return_gpr);
        }
        Op::Generic { .. } => {
            if node.repr.is_some() {
                constraints.output = OutputConstraint::FixedRegister(registers.return_gpr);
            }
        }
        Op::SliceArguments => {
            constraints.inputs = InlineVec::from_elem(InputConstraint::FixedRegister(registers.argument_gprs[2]), 1);
            constraints.output = OutputConstraint::FixedRegister(registers.return_gpr);
        }
        Op::Return => {
            constraints.inputs = InlineVec::from_elem(InputConstraint::FixedRegister(registers.return_gpr), 1);
        }
        // Comparisons and int32 arithmetic take a constant right-hand side
        // as an immediate.
        Op::Int32Compare { .. }
        | Op::TaggedEquals { .. }
        | Op::Branch {
            condition: BranchCondition::Int32(_) | BranchCondition::TaggedEquals { .. },
            ..
        } => constraints.inputs[1] = InputConstraint::RegisterOrConstant,
        Op::Int32Binary { op } if op != crate::ir::BinaryOp::Mul => {
            constraints.inputs[1] = InputConstraint::RegisterOrConstant;
            // The quotient of a remainder.
            if op == crate::ir::BinaryOp::Mod {
                constraints.temps = 1;
            }
        }
        Op::Uint32ShiftRight => constraints.inputs[1] = InputConstraint::RegisterOrConstant,
        // The new rope string, its length and a temp.
        Op::ConcatenateStrings => constraints.temps = 3,
        Op::StringsEqual => constraints.temps = 1,
        // A value and a cursor for filling the registers and locals.
        Op::InitializeFrame | Op::EnsureFrameInitialized => constraints.temps = 2,
        // The object's property storage.
        Op::StoreNamed { .. } | Op::AddNamed { .. } | Op::CheckAccessorFunction { .. } => constraints.temps = 1,
        // The object whose prototype comes next.
        Op::HasInPrototypeChain => constraints.temps = 1,
        // The environment's binding values.
        Op::StoreGlobalBinding { .. } => constraints.temps = 1,
        // Direct and native calls with inputs build the callee's frame in
        // temps.
        Op::CallDirect {
            stores_operands: true, ..
        }
        | Op::CallNative {
            stores_operands: true, ..
        } => {
            // NB: The call stores its operands to the frame first.
            constraints.inputs = InlineVec::from_elem(InputConstraint::Anywhere, node.inputs.len());
            constraints.output = OutputConstraint::FixedRegister(registers.return_gpr);
        }
        Op::CallDirect { .. } | Op::CallNative { .. } if !node.inputs.is_empty() => {
            constraints.temps = crate::codegen::DIRECT_CALL_TEMPS;
            // NB: Calls in inlined callees return their result.
            if node.repr.is_some() {
                constraints.output = OutputConstraint::FixedRegister(registers.return_gpr);
            }
        }
        // The object, its shape and the cache entry.
        Op::ProbePropertyCache { .. } | Op::ProbeKeyedCache { .. } => constraints.temps = 3,
        // Those, and the named property storage of property additions.
        Op::ProbeKeyedStore { .. } | Op::ProbePropertyStore { .. } => constraints.temps = 4,
        // The cache, the global declarative environment, the global object
        // and a temp.
        Op::ProbeGlobalCache { .. } | Op::ProbeGlobalStore { .. } => constraints.temps = 4,
        // The object or its elements, and the keyed cache's registers.
        Op::ProbeHasProperty { .. } => constraints.temps = 3,
        // The address of the object.
        Op::Branch {
            condition: BranchCondition::ElementsKind(_),
            ..
        } => constraints.temps = 1,
        // NB: Slow path calls read their operands from where they are.
        Op::CallSlowPath { saves_registers, .. } => {
            constraints.inputs = InlineVec::from_elem(InputConstraint::Anywhere, node.inputs.len());
            // NB: A call's value is moved into the return register once its
            //     slow path continued, where a conditional jump's control
            //     word is.
            if !saves_registers && node.repr.is_some() {
                constraints.output = OutputConstraint::FixedRegister(registers.return_gpr);
            }
        }
        // NB: The output comes from where the slow path call left it.
        Op::SlowPathOutput { .. } => constraints.inputs = InlineVec::from_elem(InputConstraint::Anywhere, 1),
        // The object, the property iterator cache, and a temp.
        Op::Branch {
            condition: BranchCondition::PropertyIteratorCacheValid,
            ..
        } => constraints.temps = 3,
        // The binding's index, and a temp; the environment's shape and the
        // binding's index.
        Op::Branch {
            condition: BranchCondition::BindingMutable { .. } | BranchCondition::NextBindingOfShape { .. },
            ..
        } => constraints.temps = 2,
        // The environment's binding values (and their count).
        Op::StoreEnvironmentBinding { .. } | Op::AppendEnvironmentBinding => constraints.temps = 1,
        // The array, then its prototypes.
        Op::CheckAppendableArray => constraints.temps = 2,
        // The object's elements.
        Op::LoadElementsCapacity | Op::AppendElement => constraints.temps = 1,
        // The object, then its elements or element address.
        Op::CheckElements { .. } | Op::CheckIdentityComparable | Op::CheckClosure { .. } => constraints.temps = 1,
        // The elements or the element address.
        Op::LoadElementAt { .. } | Op::StoreElementAt { .. } => constraints.temps = 1,
        // The heap and a value.
        Op::AllocateObject { .. } => constraints.temps = 2,
        // The object's property storage.
        Op::InitializeNamed { .. } => constraints.temps = 1,
        // The element storage and a value.
        Op::AllocateArray { .. } => constraints.temps = 2,
        // The function and a value.
        Op::AllocateFunction { .. } | Op::AllocateEnvironment { .. } => constraints.temps = 2,
        // The array's elements.
        Op::InitializeElement { .. } => constraints.temps = 1,
        // The function object.
        Op::Branch {
            condition: BranchCondition::Builtin(_),
            ..
        } => constraints.temps = 1,
        _ => {}
    }
    constraints
}

/// Assigns locations to the values of `graph`. The verifier checks the
/// result where the IR is verified (see `verify()`).
pub fn allocate(graph: &Graph, registers: &RegisterSet) -> Allocation {
    Allocator::new(graph, registers).run()
}

struct Allocator<'a> {
    graph: &'a Graph,
    registers: &'a RegisterSet,
    liveness: ValueLiveness,
    allocation: Allocation,
    /// The value each register holds.
    contents: Vec<Option<NodeId>>,
    /// The register each value is in, if any.
    value_registers: Vec<Option<u8>>,
    /// The register contents at the end of each finished block.
    end_contents: Vec<Option<Vec<Option<NodeId>>>>,
    /// The position of the node being allocated.
    position: u32,
}

impl<'a> Allocator<'a> {
    fn new(graph: &'a Graph, registers: &'a RegisterSet) -> Self {
        assert!(registers.allocatable_gprs.0 >> FPR_BASE == 0);
        assert!(!registers.allocatable_gprs.contains(registers.scratch_gpr));
        Self {
            graph,
            registers,
            liveness: ValueLiveness::compute(graph),
            allocation: Allocation {
                nodes: vec![NodeAllocation::default(); graph.nodes.len()],
                block_end_moves: vec![Vec::new(); graph.blocks.len()],
                spill_slot_count: 0,
                live_in: Vec::new(),
            },
            contents: vec![None; REGISTER_COUNT],
            value_registers: vec![None; graph.nodes.len()],
            end_contents: vec![None; graph.blocks.len()],
            position: 0,
        }
    }

    fn run(mut self) -> Allocation {
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
                let live_in = self.liveness.live_in[index].iter().collect::<Vec<_>>();
                for value in live_in {
                    self.ensure_spill_slot(NodeId::from_index(value));
                }
            }
        }

        let mut cycle_temp = None;
        for block in 0..self.graph.blocks.len() {
            let block = BlockId::from_index(block);
            self.start_block(block);
            let block_data = self.graph.block(block);
            for node in &block_data.body {
                self.allocate_node(*node);
            }
            self.allocation.block_end_moves[block.index()] = self.phi_moves(block, &mut cycle_temp);
            self.allocate_node(self.graph.control_id(block));
            self.end_contents[block.index()] = Some(self.contents.clone());
        }
        self.allocation.live_in = self.liveness.live_in;
        self.allocation
    }

    /// Gives `phi` its location: `register` if it has one, and its spill
    /// slot (which it may have already, if it is live into a block
    /// allocated before its own) if it has no register or needs a slot.
    fn place_phi(&mut self, phi: NodeId, register: Option<u8>) {
        let slot = match register {
            Some(_)
                if !self.liveness.needs_spill.contains(phi.index())
                    && self.allocation.nodes[phi.index()].spill_slot.is_none() =>
            {
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
        let slot = self.new_spill_slot();
        self.allocation.nodes[value.index()].spill_slot = Some(slot);
        slot
    }

    fn set_contents(&mut self, contents: Vec<Option<NodeId>>) {
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
    fn contents_entering(&self, predecessor: BlockId, block: BlockId) -> Vec<Option<NodeId>> {
        let live_in = &self.liveness.live_in[block.index()];
        let mut contents = self.end_contents[predecessor.index()]
            .clone()
            .expect("blocks come after the predecessor whose state they start with");
        for value in &mut contents {
            if value.is_some_and(|value| !live_in.contains(value.index())) {
                *value = None;
            }
        }
        contents
    }

    /// The register contents a merge with a state predecessor starts with:
    /// those at the end of that predecessor, live into the merge, and the
    /// phis in their registers.
    fn merge_contents(&self, block: BlockId) -> Option<Vec<Option<NodeId>>> {
        let predecessor = state_predecessor(self.graph, block)?;
        let mut contents = self.contents_entering(predecessor, block);
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
                let mut contents = vec![None; REGISTER_COUNT];
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
    /// its spill slot, which it gets if it had none.
    fn evict(&mut self, register: u8) {
        if let Some(value) = self.contents[register as usize] {
            self.ensure_spill_slot(value);
            self.free_register(register);
        }
    }

    fn next_use(&self, value: NodeId) -> u32 {
        let uses = &self.liveness.use_positions[value.index()];
        let index = uses.partition_point(|position| *position <= self.position);
        uses.get(index).copied().unwrap_or(u32::MAX)
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
        let slot = self.allocation.nodes[value.index()]
            .spill_slot
            .unwrap_or_else(|| panic!("v{} is neither in a register nor spilled", value.0));
        Location::Stack(slot)
    }

    /// Whether `value`, which `node` uses or which is in a register at
    /// `node`, is still needed after it. Registers only ever hold live values.
    fn is_live_after(&self, value: NodeId, node: NodeId) -> bool {
        !self.liveness.dies_at[node.index()].contains(&value)
    }

    fn exit_values(&self, node: NodeId) -> Vec<(u32, Location)> {
        let Some(frame_state) = self.graph.node(node).frame_state else {
            return Vec::new();
        };
        self.graph
            .frame_state_values(frame_state)
            .into_iter()
            .map(|(slot, value)| (slot, self.current_location(value)))
            .collect()
    }

    fn allocate_node(&mut self, node_id: NodeId) {
        let node = self.graph.node(node_id);
        self.position = self.liveness.positions[node_id.index()];
        let constraints = constraints(node, self.registers);
        let mut moves = Vec::new();
        let mut blocked = RegisterMask::default();
        let mut inputs = InlineVec::from_elem(Location::Register(0), node.inputs.len());

        // Fixed register inputs first, so that inputs in any register do not
        // take their registers.
        for (index, (input, constraint)) in node.inputs.iter().zip(&constraints.inputs).enumerate() {
            let InputConstraint::FixedRegister(register) = *constraint else {
                continue;
            };
            let is_value = is_allocated_value(self.graph, *input);
            if !is_value || self.value_registers[input.index()] != Some(register) {
                debug_assert!(!blocked.contains(register), "two inputs need the same register");
                self.evict(register);
                let from = self.current_location(*input);
                moves.push(Move {
                    from,
                    to: Location::Register(register),
                });
                match from {
                    // The value moves, unless an earlier input of this node
                    // needs it where it is; then this is a copy for the node.
                    Location::Register(previous) if !blocked.contains(previous) => {
                        self.free_register(previous);
                        self.assign_register(register, *input);
                    }
                    Location::Stack(_) => self.assign_register(register, *input),
                    _ => {}
                }
            }
            blocked = blocked.with(register);
            inputs[index] = Location::Register(register);
        }
        for (index, (input, constraint)) in node.inputs.iter().zip(&constraints.inputs).enumerate() {
            if *constraint == InputConstraint::RegisterOrConstant
                && let Some(bits) = self.graph.constant_value(*input)
            {
                inputs[index] = Location::Constant(bits);
                continue;
            }
            if *constraint == InputConstraint::Anywhere {
                inputs[index] = self.current_location(*input);
                continue;
            }
            if matches!(constraint, InputConstraint::FixedRegister(_)) {
                continue;
            }
            let register = match self.value_registers[input.index()] {
                Some(register) if is_allocated_value(self.graph, *input) => register,
                _ => {
                    let register = self.pick_register(blocked, RegisterClass::of(self.graph.node(*input).repr));
                    moves.push(Move {
                        from: self.current_location(*input),
                        to: Location::Register(register),
                    });
                    if is_allocated_value(self.graph, *input) {
                        self.assign_register(register, *input);
                    }
                    register
                }
            };
            blocked = blocked.with(register);
            inputs[index] = Location::Register(register);
        }

        let mut temps = InlineVec::new();
        for _ in 0..constraints.temps {
            let register = self.pick_register(blocked, RegisterClass::General);
            blocked = blocked.with(register);
            temps.push(register);
        }

        // Pick the output register before recording where exits find their
        // values: a node may exit after writing its output (a check of a value
        // it loaded), so no exit value may stay in that register.
        let pick_output = |allocator: &mut Self| match constraints.output {
            OutputConstraint::None => None,
            OutputConstraint::Register => Some(allocator.pick_register(blocked, RegisterClass::of(node.repr))),
            OutputConstraint::FixedRegister(register) => {
                debug_assert!(constraints.is_call || !blocked.contains(register));
                allocator.evict(register);
                Some(register)
            }
        };
        let clobbered = if constraints.is_call {
            self.registers.caller_saved()
        } else if saves_only_gprs(&node.op) {
            self.registers.allocatable(RegisterClass::Float)
        } else {
            RegisterMask::default()
        };
        {
            for register in clobbered.iter() {
                if let Some(value) = self.contents.get(register as usize).copied().flatten() {
                    debug_assert!(
                        self.allocation.nodes[value.index()].spill_slot.is_some()
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
        let exit_values = self.exit_values(node_id);

        // Values whose last use is this node no longer need their registers.
        for index in 0..self.liveness.dies_at[node_id.index()].len() {
            let value = self.liveness.dies_at[node_id.index()][index];
            if let Some(register) = self.value_registers[value.index()] {
                self.free_register(register);
            }
        }

        if let Some(register) = output {
            self.assign_register(register, node_id);
            if self.liveness.needs_spill.contains(node_id.index()) {
                self.ensure_spill_slot(node_id);
            }
            if self.liveness.dead_outputs.contains(node_id.index()) {
                self.free_register(register);
            }
        }

        let allocation = &mut self.allocation.nodes[node_id.index()];
        allocation.moves_before = moves;
        allocation.inputs = inputs;
        allocation.temps = temps;
        allocation.output = output.map(Location::Register);
        allocation.exit_values = exit_values;
    }

    /// Gives the phis of the merge `join` their locations, at the end of its
    /// state predecessor: registers that hold no value live into the merge
    /// (preferably the input's own), or stack slots if there are none.
    fn place_merge_phis(&mut self, predecessor: BlockId, join: BlockId) {
        let live_in = &self.liveness.live_in[join.index()];
        let mut taken = RegisterMask::default();
        for (register, value) in self.contents.iter().enumerate() {
            if value.is_some_and(|value| live_in.contains(value.index())) {
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

    /// The moves at the end of `block` that put the values live into the
    /// merge `join` (and its phis' inputs) where the merge expects them.
    fn merge_moves(&self, block: BlockId, join: BlockId) -> Vec<Move> {
        let contents = self.merge_contents(join).expect("the merge has a state predecessor");
        let phis = &self.graph.block(join).phis;
        contents
            .iter()
            .enumerate()
            .filter_map(|(register, value)| {
                let value = (*value)?;
                if phis.contains(&value) {
                    return None;
                }
                let to = Location::Register(u8::try_from(register).expect("register numbers fit in u8"));
                let from = self.current_location(value);
                (from != to).then_some(Move { from, to })
            })
            .chain(
                liveness::phi_inputs_from(self.graph, block, join)
                    .into_iter()
                    .map(|(phi, input)| Move {
                        from: self.current_location(input),
                        to: self.allocation.nodes[phi.index()].output.expect("phis have locations"),
                    }),
            )
            .collect()
    }

    /// The moves at the end of `block` that put the inputs of the phis of its
    /// successor in place.
    fn phi_moves(&mut self, block: BlockId, cycle_temp: &mut Option<u32>) -> Vec<Move> {
        let successors = self.graph.successors(block);
        let [successor] = &*successors else {
            for successor in successors.iter() {
                debug_assert!(self.graph.block(*successor).phis.is_empty(), "critical edge into a phi");
            }
            return Vec::new();
        };
        let moves = match state_predecessor(self.graph, *successor) {
            Some(predecessor) if predecessor != block => self.merge_moves(block, *successor),
            merge => {
                if merge.is_some() {
                    self.place_merge_phis(block, *successor);
                }
                liveness::phi_inputs_from(self.graph, block, *successor)
                    .into_iter()
                    .map(|(phi, input)| Move {
                        from: self.current_location(input),
                        to: self.allocation.nodes[phi.index()].output.expect("phis have locations"),
                    })
                    .collect::<Vec<_>>()
            }
        };
        if moves.is_empty() {
            return moves;
        }
        let temp = *cycle_temp.get_or_insert_with(|| {
            let slot = self.allocation.spill_slot_count;
            self.allocation.spill_slot_count += 1;
            slot
        });
        resolve_parallel_moves(&moves, Location::Stack(temp))
    }
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
            let node = &allocation.nodes[node_id.index()];
            if Some(node_id) == graph.block(block).control && !allocation.block_end_moves[index].is_empty() {
                writeln!(out, "  moves {}", moves_text(&allocation.block_end_moves[index])).unwrap();
            }
            if !node.moves_before.is_empty() {
                writeln!(out, "  moves {}", moves_text(&node.moves_before)).unwrap();
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
