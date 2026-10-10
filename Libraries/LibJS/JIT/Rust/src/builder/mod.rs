/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The graph builder: one forward walk over the bytecode in reverse post
//! order that turns it into SSA IR.
//!
//! The builder keeps an abstract frame: for every register, local and
//! argument slot, either the SSA value it currently holds (and whether frame
//! memory is in sync with that value), or nothing when frame memory is the
//! only place that has it. `Mov` only renames, constants become constant
//! nodes, and slow paths and calls take the values of their operands as
//! inputs and give their outputs as values (see `slow_paths`). Values move
//! between frame memory and SSA values lazily: `LoadSlot` on the first use
//! of a slot that is only in memory, and `StoreSlot` of out of sync slots
//! where memory must hold them: before calls that read the frame (`Generic`
//! nodes, which read the whole frame, and calls that read their operands
//! there). Merges and loop back edges never write frame memory: a slot every
//! path leaves in memory stays there, and the others get phis, with loads at
//! the predecessors that only have them in memory. The verifier checks that
//! `StoreSlot` writes nothing else (see `passes::verify`).
//!
//! The reserved registers live only in frame memory: every read is a
//! `LoadSlot` and every write a `StoreSlot`. The exception is the this value
//! register, which only frame setup and `ResolveThisBinding` write: it is
//! loaded once at entry and tracked like other slots, except at loop
//! headers, where it stays in memory wherever memory holds it.
//!
//! Every bytecode basic block becomes one IR block. Phis are created lazily
//! at forward merges, and eagerly at loop headers for slots assigned in the
//! loop. Every conditional branch goes through a fresh block per successor,
//! so a block with several successors never leads directly to a merge, and
//! code that reconciles abstract frames at a merge always has a place to go.

mod allocation;
mod arguments;
mod caches;
mod checks;
mod elements;
mod environments;
mod globals;
mod handling;
mod inlining;
mod intrinsics;
mod keyed;
mod named;
mod osr;
mod property_access;
mod property_additions;
mod property_iterator;
mod slow_paths;
mod speculation;

pub use handling::GenericInfo;
pub use handling::Handling;
pub use handling::handling;
pub use speculation::OperationInput;
// NB: Only tests outside the builder need it.
#[cfg(test)]
#[cfg(all(test, target_arch = "x86_64"))]
pub(crate) use osr::is_loop_back_edge;

use crate::CompileFailure;
use crate::bitset::BitSet;
use crate::bytecode::DecodedInstruction;
use crate::bytecode::FrameLayout;
use crate::bytecode::Instruction;
use crate::bytecode::Label;
use crate::bytecode::OpCode;
use crate::bytecode::Operand;
use crate::bytecode::PUT_KIND_NORMAL;
use crate::bytecode::RESERVED_REGISTER_COUNT;
use crate::bytecode::THIS_VALUE_REGISTER;
use crate::bytecode::cfg::BlockIndex;
use crate::bytecode::cfg::Cfg;
use crate::bytecode::decode_all;
use crate::bytecode::liveness::Liveness;
use crate::code::ExitKind;
use crate::code::Repr;
use crate::code::ResumeMode;
use crate::fast_hash::HashMap;
use crate::ir::Block;
use crate::ir::BlockId;
use crate::ir::BranchCondition;
use crate::ir::FrameField;
use crate::ir::FrameStateId;
use crate::ir::Graph;
use crate::ir::Node;
use crate::ir::NodeId;
use crate::ir::Op;
use crate::ir::value;
use crate::snapshot::CallFeedbackSnapshot;
use crate::snapshot::ExecutableSnapshot;
use crate::snapshot::Intrinsic;
use crate::snapshot::RuntimeInfo;
use crate::snapshot::Snapshot;
pub(crate) use slow_paths::is_ssa_slow_path_output;
pub(crate) use slow_paths::js_call_operands;

/// Builds the IR for `snapshot.executables[0]`.
pub fn build_graph(snapshot: &Snapshot) -> Result<Graph, CompileFailure> {
    build_graph_with_feedback(snapshot, &|pc| {
        snapshot
            .executables
            .first()
            .is_none_or(|executable| has_run(executable, pc))
    })
}

/// Whether the instruction at `pc` ever ran in the interpreter, as far as the
/// feedback it records tells: an instruction that records feedback ran if any
/// of its slots recorded something. Instructions without feedback count as
/// having run. Instructions that never ran are compiled as unconditional exits.
pub fn has_run(executable: &ExecutableSnapshot, pc: u32) -> bool {
    let Ok(instruction) = crate::bytecode::decode_instruction(&executable.bytecode, pc) else {
        return true;
    };
    let slots = instruction.instruction.feedback_slots();
    let feedback = &executable.feedback;
    let slot = |slot: Option<u16>| slot.map(usize::from);
    let recorded = [
        slot(slots.arith)
            .and_then(|slot| feedback.arith.get(slot))
            .map(|bits| *bits != 0),
        slot(slots.value)
            .and_then(|slot| feedback.value.get(slot))
            .map(|bits| *bits != 0),
        slot(slots.call)
            .and_then(|slot| feedback.call.get(slot))
            .map(|call| call.target.is_some() || call.flags != 0),
        slot(slots.keyed)
            .and_then(|slot| feedback.keyed.get(slot))
            .map(|keyed| keyed.bits != 0),
    ];
    recorded.iter().all(Option::is_none) || recorded.contains(&Some(true))
}

/// Like `build_graph()`, with the "did this instruction ever run" oracle
/// given by the caller.
pub fn build_graph_with_feedback(snapshot: &Snapshot, has_run: &dyn Fn(u32) -> bool) -> Result<Graph, CompileFailure> {
    let graph = GraphBuilder::new(snapshot, has_run, true, true)?.build()?;
    if !arguments::uses_virtual_arguments(&graph) {
        return Ok(graph);
    }
    if !graph.osr_entries.is_empty() {
        let graph = GraphBuilder::new(snapshot, has_run, true, false)?.build()?;
        if !arguments::uses_virtual_arguments(&graph) {
            return Ok(graph);
        }
    }
    // Something needs the arguments object itself.
    GraphBuilder::new(snapshot, has_run, false, true)?.build()
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum SlotState {
    /// Only frame memory has the slot's value.
    InMemory,
    /// The slot holds `node`. If `in_sync`, frame memory holds it as well.
    Value { node: NodeId, in_sync: bool },
}

/// The builder's view of the frame's registers, locals and arguments,
/// indexed by `FrameLayout::tracked_index()`, and of the fields of its
/// execution context.
#[derive(Debug, Clone)]
struct AbstractFrame {
    slots: Vec<SlotState>,
    /// For each `FrameField` (in the order of `FrameField::ALL`), the value
    /// the field holds on this path, once loaded or set (see
    /// `GraphBuilder::frame_field()`).
    fields: [Option<NodeId>; FrameField::ALL.len()],
}

impl AbstractFrame {
    /// A frame whose slots are only in frame memory and whose fields are
    /// not loaded yet.
    fn in_memory(slot_count: usize) -> Self {
        Self {
            slots: vec![SlotState::InMemory; slot_count],
            fields: [None; FrameField::ALL.len()],
        }
    }

    fn field(&self, field: FrameField) -> Option<NodeId> {
        self.fields[field as usize]
    }

    fn set_field(&mut self, field: FrameField, value: NodeId) {
        self.fields[field as usize] = Some(value);
    }

    /// Forgets the environments, which an instruction may change.
    fn forget_environments(&mut self) {
        for field in FrameField::ALL.into_iter().filter(|field| field.is_environment()) {
            self.fields[field as usize] = None;
        }
    }
}

/// Whether an instruction of `opcode` may replace an environment of the
/// running execution context (as opposed to changing its bindings).
fn replaces_environments(opcode: OpCode) -> bool {
    matches!(
        opcode,
        OpCode::SetLexicalEnvironment
            | OpCode::CreateLexicalEnvironment
            | OpCode::CreateVariableEnvironment
            | OpCode::CreatePrivateEnvironment
            | OpCode::LeavePrivateEnvironment
            | OpCode::EnterObjectEnvironment
            | OpCode::NewClass
            | OpCode::CallDirectEval
            | OpCode::CallDirectEvalWithArgumentArray
            | OpCode::Debugger
    )
}

/// Whether each loop has an instruction that may replace an environment
/// (see `replaces_environments()`), nested loops included.
fn loops_replacing_environments(cfg: &Cfg, instructions: &[DecodedInstruction]) -> Vec<bool> {
    cfg.loops
        .iter()
        .map(|current_loop| {
            current_loop.blocks.iter().any(|block| {
                cfg.blocks[*block]
                    .instructions
                    .clone()
                    .any(|index| replaces_environments(instructions[index].instruction.opcode()))
            })
        })
        .collect()
}

/// Whether an operand is a reserved register that lives in frame memory: one
/// other than `this` of the compiled function. Inlined frames (`is_virtual`)
/// have no memory, so theirs are tracked.
fn is_reserved(operand: Operand, is_virtual: bool) -> bool {
    operand.raw() < RESERVED_REGISTER_COUNT && operand.raw() != THIS_VALUE_REGISTER && !is_virtual
}

/// What ends the instruction currently being built.
enum Flow {
    /// The block continues with the next instruction.
    Continue,
    /// The instruction ended the block.
    Ended,
}

fn written_constants(instructions: &[DecodedInstruction], layout: &FrameLayout) -> BitSet {
    let mut written = BitSet::new(layout.number_of_constants as usize);
    let constants = layout.constants_base()..layout.arguments_base();
    for instruction in instructions {
        instruction.instruction.for_each_operand(|operand, role| {
            if role.is_written() && constants.contains(&operand.raw()) {
                written.insert((operand.raw() - layout.constants_base()) as usize);
            }
        });
    }
    written
}

/// The slots that need phis at each loop's header: those the loop assigns,
/// and those any loop whose header lies in its body assigns, since that
/// header gives them new values. Without irreducible control flow (as
/// `finally` dispatch can create), those loops are nested in it anyway.
fn loop_phi_slots(cfg: &Cfg) -> Vec<BitSet> {
    let mut slots = cfg
        .loops
        .iter()
        .map(|current_loop| current_loop.assigned_slots.clone())
        .collect::<Vec<_>>();
    let mut changed = true;
    while changed {
        changed = false;
        for (index, current_loop) in cfg.loops.iter().enumerate() {
            for (other_index, other) in cfg.loops.iter().enumerate() {
                if other_index != index && current_loop.blocks.contains(&other.header) {
                    let other_slots = slots[other_index].clone();
                    changed |= slots[index].union_with(&other_slots);
                }
            }
        }
    }
    slots
}

/// The state of the builder's walk over one executable: the compiled
/// function or an inlined callee.
struct Function<'a> {
    /// Index into `Snapshot::executables`.
    index: u32,
    executable: &'a ExecutableSnapshot,
    layout: FrameLayout,
    instructions: Vec<DecodedInstruction>,
    cfg: Cfg,
    liveness: Liveness,
    /// The IR block of each bytecode block, created when first referenced.
    ir_blocks: Vec<Option<BlockId>>,
    /// Forward edges into each bytecode block not yet started: the IR
    /// predecessor and the abstract frame at its end.
    incoming: Vec<Vec<(BlockId, AbstractFrame)>>,
    /// Whether each bytecode block was started.
    started: Vec<bool>,
    /// The abstract frame at the start of each loop header that was started,
    /// which every back edge must reconcile with.
    loop_header_frames: Vec<Option<AbstractFrame>>,
    /// For each loop, the slots that get phis at its header.
    loop_phi_slots: Vec<BitSet>,
    /// For each loop, whether it may replace an environment.
    loops_replacing_environments: Vec<bool>,
    /// The loop headers that on-stack replacement entries lead to over back
    /// edges, which are built even if no forward edge reaches them.
    osr_loop_headers: Vec<bool>,
    /// Constant slots some instruction writes. The interpreter keeps a copy of
    /// the constants in the frame, and writes to a constant operand change that
    /// copy, so such slots live in frame memory like the reserved registers.
    written_constants: BitSet,
    /// Index of the instruction being built.
    instruction_index: usize,
    pc: u32,
    /// The frame state shared by every eager exit of the current instruction.
    eager_frame_state: Option<FrameStateId>,
    /// How this executable was called, if it is an inlined callee.
    inline: Option<inlining::InlineCall>,
    /// Whether some instruction writes an argument slot.
    writes_arguments: bool,
    /// How many of the next instructions the instruction being built built
    /// as well.
    skipped_instructions: usize,
    /// Whether some instruction may write the bindings of the parameters,
    /// which a mapped arguments object aliases, or create a closure that may.
    may_write_parameter_bindings: bool,
}

impl<'a> Function<'a> {
    fn new(index: u32, executable: &'a ExecutableSnapshot) -> Result<Self, CompileFailure> {
        let layout = executable.layout;
        let instructions = decode_all(&executable.bytecode).map_err(|error| CompileFailure::InvalidBytecode {
            pc: error.pc,
            reason: "undecodable instruction",
        })?;
        if instructions.is_empty() {
            return Err(CompileFailure::InvalidBytecode {
                pc: 0,
                reason: "empty bytecode",
            });
        }
        let cfg = Cfg::new(&instructions, &executable.exception_handlers, &layout).map_err(|_| {
            CompileFailure::InvalidBytecode {
                pc: 0,
                reason: "invalid control flow",
            }
        })?;
        let liveness = Liveness::compute(&instructions, &cfg, &layout);
        let block_count = cfg.blocks.len();
        let loop_phi_slots = loop_phi_slots(&cfg);
        let loops_replacing_environments = loops_replacing_environments(&cfg, &instructions);
        let written_constants = written_constants(&instructions, &layout);
        let may_write_parameter_bindings = instructions
            .iter()
            .any(|decoded| arguments::may_write_parameter_bindings(decoded.instruction.opcode()));
        let mut writes_arguments = false;
        for decoded in &instructions {
            decoded.instruction.for_each_operand(|operand, role| {
                writes_arguments |= role.is_written() && operand.raw() >= layout.arguments_base();
            });
        }
        Ok(Self {
            index,
            executable,
            layout,
            instructions,
            cfg,
            liveness,
            ir_blocks: vec![None; block_count],
            incoming: vec![Vec::new(); block_count],
            started: vec![false; block_count],
            loop_header_frames: vec![None; block_count],
            loop_phi_slots,
            loops_replacing_environments,
            osr_loop_headers: vec![false; block_count],
            written_constants,
            instruction_index: 0,
            pc: 0,
            eager_frame_state: None,
            inline: None,
            writes_arguments,
            skipped_instructions: 0,
            may_write_parameter_bindings,
        })
    }

    /// Whether this is the frame of an inlined call, whose slots are only
    /// SSA values until it is materialized.
    fn is_virtual(&self) -> bool {
        self.inline.is_some()
    }

    /// Whether this is an inlined callee with legacy `arguments`: a function
    /// that is not strict.
    fn has_legacy_arguments(&self) -> bool {
        self.is_virtual() && self.is_non_strict_function()
    }

    /// Whether this is a function that is not strict (and no builtin).
    fn is_non_strict_function(&self) -> bool {
        !self.executable.builtin && self.executable.function.is_some_and(|function| !function.strict)
    }
}

struct GraphBuilder<'a> {
    snapshot: &'a Snapshot,
    runtime: &'a RuntimeInfo,
    has_run: &'a dyn Fn(u32) -> bool,

    graph: Graph,
    /// The blocks in linear order, as they are started.
    order: Vec<BlockId>,
    /// The constant node of each value and representation.
    constants: HashMap<(u64, Repr), NodeId>,

    block: BlockId,
    frame: AbstractFrame,
    /// The executable being walked.
    function: Function<'a>,
    /// The executables whose walks are suspended at an inlined call, outermost first.
    callers: Vec<Function<'a>>,
    /// Bytecode instructions inlined so far, for the inlining budget.
    inlined_instructions: usize,
    /// Whether `CreateArguments` may build a virtual arguments object.
    virtualize_arguments: bool,
    /// Whether to build on-stack replacement entries (see `arguments`).
    build_osr_entries: bool,
    /// The intrinsics among the constants, by value.
    intrinsics: HashMap<u64, Intrinsic>,
    /// How `instanceof` runs with the functions the code uses as constants
    /// on its right-hand side (see `GlobalValueSnapshot::has_instance`), by
    /// their bits.
    has_instance: HashMap<u64, crate::snapshot::OrdinaryHasInstanceSnapshot>,
}

impl<'a> GraphBuilder<'a> {
    fn new(
        snapshot: &'a Snapshot,
        has_run: &'a dyn Fn(u32) -> bool,
        virtualize_arguments: bool,
        build_osr_entries: bool,
    ) -> Result<Self, CompileFailure> {
        let executable = snapshot.executables.first().ok_or(CompileFailure::InvalidBytecode {
            pc: 0,
            reason: "no executable",
        })?;
        let function = Function::new(0, executable)?;
        let slot_count = function.layout.tracked_slot_count();
        Ok(Self {
            snapshot,
            runtime: &snapshot.runtime,
            has_run,
            graph: Graph::default(),
            order: Vec::new(),
            constants: HashMap::default(),
            block: BlockId(0),
            frame: AbstractFrame::in_memory(slot_count),
            function,
            callers: Vec::new(),
            inlined_instructions: 0,
            virtualize_arguments,
            build_osr_entries,
            intrinsics: HashMap::default(),
            has_instance: HashMap::default(),
        })
    }

    fn build(mut self) -> Result<Graph, CompileFailure> {
        // NB: The entry block has no predecessors and jumps to the first bytecode
        //     block, which may be a loop header.
        let entry = self.start_new_block(Vec::new());
        self.enter_function(entry);
        if self.build_osr_entries {
            self.build_osr_entries()?;
        }
        self.build_function()?;

        let mut graph = self.graph;
        graph.exit_sites = self.snapshot.executables[0].exit_sites.clone();
        assert_eq!(graph.blocks.len(), self.order.len(), "every created block was started");
        graph.reorder_blocks(&self.order);
        for block in &graph.blocks {
            assert!(block.control.is_some(), "every block was finished");
            for phi in &block.phis {
                assert_eq!(
                    graph.nodes[phi.index()].inputs.len(),
                    block.predecessors.len(),
                    "phis have one input per predecessor"
                );
            }
        }
        Ok(graph)
    }

    /// Ends the current block with a jump to the first bytecode block of the
    /// function being walked, with the current abstract frame.
    fn enter_function(&mut self, from: BlockId) {
        let first_block = self.ir_block(0);
        self.function.incoming[0].push((from, self.frame.clone()));
        self.block = from;
        self.set_control(Op::Jump { target: first_block }, Vec::new());
    }

    /// Builds the blocks of the function being walked, in reverse post order.
    fn build_function(&mut self) -> Result<(), CompileFailure> {
        for rpo_index in 0..self.function.cfg.reverse_post_order.len() {
            let block_index = self.function.cfg.reverse_post_order[rpo_index];
            if self.start_bytecode_block(block_index) {
                self.build_bytecode_block(block_index)?;
            }
        }
        Ok(())
    }

    // Blocks.

    fn ir_block(&mut self, block_index: BlockIndex) -> BlockId {
        if let Some(block) = self.function.ir_blocks[block_index] {
            return block;
        }
        let block = self.graph.add_block(Block::default());
        self.function.ir_blocks[block_index] = Some(block);
        block
    }

    /// Creates a block, appends it to the linear order and makes it current.
    /// The caller sets the abstract frame.
    fn start_new_block(&mut self, predecessors: Vec<BlockId>) -> BlockId {
        let block = self.graph.add_block(Block {
            predecessors,
            ..Block::default()
        });
        self.order.push(block);
        self.block = block;
        block
    }

    /// Starts the IR block of a bytecode block, merging the abstract frames of
    /// its forward predecessors. Returns false if no built block reaches it.
    fn start_bytecode_block(&mut self, block_index: BlockIndex) -> bool {
        let incoming = std::mem::take(&mut self.function.incoming[block_index]);
        // NB: A loop whose forward entry was never built can still be
        //     running when compiled code is entered at a back edge inside it.
        let only_back_edges = incoming.is_empty() && self.function.osr_loop_headers[block_index];
        if incoming.is_empty() && !only_back_edges {
            return false;
        }
        let bytecode_block = &self.function.cfg.blocks[block_index];
        let is_loop_header = bytecode_block.is_loop_header();
        let first_instruction = bytecode_block.instructions.start;
        let block = self.ir_block(block_index);
        self.function.started[block_index] = true;
        self.order.push(block);
        self.block = block;
        {
            let block = &mut self.graph.blocks[block.index()];
            block.predecessors = incoming.iter().map(|(predecessor, _)| *predecessor).collect();
            block.bytecode_start = Some(self.function.instructions[first_instruction].pc);
            block.is_loop_header = is_loop_header;
        }
        self.function.instruction_index = first_instruction;
        self.function.pc = self.function.instructions[first_instruction].pc;

        self.frame = if only_back_edges {
            // NB: Frame memory holds the slots the loop does not assign, and
            //     the others get phis of what the back edges bring.
            let mut frame = AbstractFrame::in_memory(self.function.layout.tracked_slot_count());
            let live = self.function.liveness.live_in(first_instruction).clone();
            let assigned = self.function.cfg.blocks[block_index]
                .loop_index
                .map(|loop_index| self.function.loop_phi_slots[loop_index].clone());
            for slot in live.iter() {
                if self.tracks_slot(slot)
                    && (slot != THIS_VALUE_REGISTER as usize || self.function.is_virtual())
                    && assigned.as_ref().is_some_and(|assigned| assigned.contains(slot))
                {
                    let node = self.add_phi(Vec::new());
                    frame.slots[slot] = SlotState::Value { node, in_sync: false };
                }
            }
            frame
        } else if incoming.len() == 1 && !is_loop_header {
            incoming.into_iter().next().unwrap().1
        } else {
            self.merge(block_index, incoming)
        };
        if is_loop_header {
            self.function.loop_header_frames[block_index] = Some(self.frame.clone());
        }
        true
    }

    /// The abstract frame at the start of a merge or loop header, given the
    /// abstract frames at the end of its forward predecessors. Adds phis to
    /// the current block, and writes values back to the frame at the end of
    /// predecessors where the merged slot is only in memory.
    fn merge(&mut self, block_index: BlockIndex, incoming: Vec<(BlockId, AbstractFrame)>) -> AbstractFrame {
        let bytecode_block = &self.function.cfg.blocks[block_index];
        let live = self
            .function
            .liveness
            .live_in(bytecode_block.instructions.start)
            .clone();
        let assigned_in_loop = bytecode_block
            .loop_index
            .map(|loop_index| self.function.loop_phi_slots[loop_index].clone());
        let is_loop_header = bytecode_block.is_loop_header();

        let mut merged = AbstractFrame::in_memory(self.function.layout.tracked_slot_count());
        // NB: A field keeps its value where every path brings the same one,
        //     except environments at the headers of loops that may replace
        //     them, and every field at the headers of loops that on-stack
        //     replacement entries reach over back edges, which bring none.
        let replaces_environments_in_loop = is_loop_header
            && bytecode_block
                .loop_index
                .is_some_and(|loop_index| self.function.loops_replacing_environments[loop_index]);
        let osr_loop_header = self.function.osr_loop_headers[block_index];
        for field in FrameField::ALL {
            let first = incoming.first().and_then(|(_, frame)| frame.field(field));
            if let Some(value) = first
                && incoming.iter().all(|(_, frame)| frame.field(field) == Some(value))
                && !(field.is_environment() && replaces_environments_in_loop)
                && !osr_loop_header
            {
                merged.set_field(field, value);
            }
        }
        // NB: Reserved registers live in frame memory, except in inlined
        //     callees, which track them like other slots.
        for slot in 0..merged.slots.len() {
            if !self.tracks_slot(slot) || !live.contains(slot) {
                continue;
            }
            // NB: Back edges from paths that start at an on-stack replacement
            //     entry inside the loop bring the values that entry loaded,
            //     so slots the loop does not assign live in the frame there.
            let invariant_in_osr_loop = self.function.osr_loop_headers[block_index]
                && assigned_in_loop
                    .as_ref()
                    .is_some_and(|assigned| !assigned.contains(slot));
            let values = incoming
                .iter()
                .map(|(_, frame)| match frame.slots[slot] {
                    SlotState::Value { node, in_sync } if !invariant_in_osr_loop => Some((node, in_sync)),
                    _ => None,
                })
                .collect::<Option<Vec<_>>>();
            let assigned = assigned_in_loop
                .as_ref()
                .is_some_and(|assigned| assigned.contains(slot));
            let operand = self.function.layout.operand_for_tracked_index(slot);
            // NB: Frame memory holds the this value wherever it is in sync, and
            //     only slow paths change it, which write it there. Loops keep
            //     it there, instead of in a phi of what their back edges bring.
            if is_loop_header
                && slot == THIS_VALUE_REGISTER as usize
                && !self.function.is_virtual()
                && incoming.iter().all(|(_, frame)| {
                    matches!(
                        frame.slots[slot],
                        SlotState::InMemory | SlotState::Value { in_sync: true, .. }
                    )
                })
            {
                continue;
            }
            let Some(values) = values else {
                // NB: Frame memory holds the slot on some path. Where the
                //     other paths bring values it holds too, it stays there;
                //     values the loop never changes enter the loop as SSA
                //     values, loaded before it, so that nodes using them can
                //     move out of the loop too. Mapped arguments objects can
                //     change arguments behind the code's back.
                let loop_invariant = assigned_in_loop.is_some()
                    && !assigned
                    && !self.function.osr_loop_headers[block_index]
                    && !self.function.is_virtual()
                    && slot >= RESERVED_REGISTER_COUNT as usize
                    && !(operand.raw() >= self.function.layout.arguments_base()
                        && self.function.may_write_parameter_bindings);
                if let [(predecessor, frame)] = incoming.as_slice()
                    && is_loop_header
                    && loop_invariant
                    && frame.slots[slot] == SlotState::InMemory
                {
                    let node = self.append_to_finished_block(
                        *predecessor,
                        Op::LoadSlot { slot: operand.raw() },
                        Vec::new(),
                        Some(Repr::Tagged),
                    );
                    merged.slots[slot] = SlotState::Value { node, in_sync: true };
                    continue;
                }
                let all_in_frame = incoming.iter().all(|(_, frame)| {
                    matches!(
                        frame.slots[slot],
                        SlotState::InMemory | SlotState::Value { in_sync: true, .. }
                    )
                });
                // NB: Frame memory changes only where instructions write it,
                //     never at merges or loop back edges: a slot the loop
                //     assigns gets a phi, and other paths bring their values,
                //     loaded where only frame memory holds them.
                if all_in_frame && !(is_loop_header && assigned) {
                    continue;
                }
                let inputs = incoming
                    .iter()
                    .map(|(predecessor, frame)| match frame.slots[slot] {
                        SlotState::Value { node, .. } => self.tagged_at_end_of(*predecessor, node),
                        SlotState::InMemory => self.append_to_finished_block(
                            *predecessor,
                            Op::LoadSlot { slot: operand.raw() },
                            Vec::new(),
                            Some(Repr::Tagged),
                        ),
                    })
                    .collect();
                let node = self.add_phi(inputs);
                merged.slots[slot] = SlotState::Value { node, in_sync: false };
                continue;
            };
            let first = values[0].0;
            let needs_phi = values.iter().any(|(node, _)| *node != first) || assigned;
            // NB: A loop header's phi gets the values its back edges bring,
            //     which frame memory need not hold.
            let in_sync = values.iter().all(|(_, in_sync)| *in_sync) && !(is_loop_header && assigned);
            let node = if needs_phi {
                // NB: Phis start out tagged; representation selection
                //     unboxes them later.
                let inputs = incoming
                    .iter()
                    .zip(&values)
                    .map(|((predecessor, _), (node, _))| self.tagged_at_end_of(*predecessor, *node))
                    .collect();
                self.add_phi(inputs)
            } else {
                first
            };
            merged.slots[slot] = SlotState::Value { node, in_sync };
        }
        merged
    }

    fn add_phi(&mut self, inputs: Vec<NodeId>) -> NodeId {
        let phi = self
            .graph
            .add_node(Node::new(Op::Phi, inputs, Some(Repr::Tagged), self.function.pc));
        self.graph.blocks[self.block.index()].phis.push(phi);
        phi
    }

    /// Adds a node to the body of a block whose control node is already set.
    fn append_to_finished_block(&mut self, block: BlockId, op: Op, inputs: Vec<NodeId>, repr: Option<Repr>) -> NodeId {
        let pc = self.graph.control(block).pc;
        let node = self.graph.add_node(Node::new(op, inputs, repr, pc));
        self.graph.blocks[block.index()].body.push(node);
        node
    }

    fn build_bytecode_block(&mut self, block_index: BlockIndex) -> Result<(), CompileFailure> {
        let instructions = self.function.cfg.blocks[block_index].instructions.clone();
        let block = &self.graph.blocks[self.block.index()];
        let is_merge = block.predecessors.len() > 1 || block.is_loop_header;
        for instruction_index in instructions.clone() {
            self.function.instruction_index = instruction_index;
            self.function.pc = self.function.instructions[instruction_index].pc;
            self.function.eager_frame_state = None;
            if instruction_index == instructions.start && is_merge && !self.function.is_virtual() {
                let frame_state = self.eager_frame_state();
                self.graph.blocks[self.block.index()].start_frame_state = Some(frame_state);
            }
            if self.function.skipped_instructions > 0 {
                self.function.skipped_instructions -= 1;
                continue;
            }
            if let Flow::Ended = self.build_instruction()? {
                return Ok(());
            }
        }
        // NB: The block falls through into the next one.
        let next_pc = self.function.instructions[self.function.instruction_index].next_pc();
        let next = self.block_for_label(Label(next_pc))?;
        self.end_with_jump(next)
    }

    fn block_for_label(&self, label: Label) -> Result<BlockIndex, CompileFailure> {
        self.function
            .cfg
            .block_for_pc(label.0)
            .ok_or(CompileFailure::InvalidBytecode {
                pc: self.function.pc,
                reason: "jump target outside the bytecode",
            })
    }

    // Edges.

    /// Ends the current block with a jump to a bytecode block.
    fn end_with_jump(&mut self, target: BlockIndex) -> Result<(), CompileFailure> {
        let target_block = self.ir_block(target);
        if self.function.started[target] {
            // NB: Edges to blocks that were already started are loop back edges.
            if self.function.loop_header_frames[target].is_none() {
                return Err(CompileFailure::InvalidBytecode {
                    pc: self.function.pc,
                    reason: "edge to a started block that is not a loop header",
                });
            }
            self.reconcile_back_edge(target)?;
            self.graph.blocks[target_block.index()].predecessors.push(self.block);
        } else {
            self.function.incoming[target].push((self.block, self.frame.clone()));
        }
        self.set_control(Op::Jump { target: target_block }, Vec::new());
        Ok(())
    }

    /// Ends the current block with a branch to two bytecode blocks, through
    /// a fresh block for each successor.
    fn end_with_branch(
        &mut self,
        make_op: impl FnOnce(BlockId, BlockId) -> Op,
        inputs: Vec<NodeId>,
        if_true: BlockIndex,
        if_false: BlockIndex,
    ) -> Result<(), CompileFailure> {
        let source = self.block;
        let frame = self.frame.clone();
        let true_edge = self.graph.add_block(Block {
            predecessors: vec![source],
            ..Block::default()
        });
        let false_edge = self.graph.add_block(Block {
            predecessors: vec![source],
            ..Block::default()
        });
        self.order.push(true_edge);
        self.order.push(false_edge);
        self.set_control(make_op(true_edge, false_edge), inputs);
        for (edge, target) in [(true_edge, if_true), (false_edge, if_false)] {
            self.block = edge;
            self.frame = frame.clone();
            self.end_with_jump(target)?;
        }
        Ok(())
    }

    /// Makes the abstract frame at the end of the current block match the
    /// one at the start of the loop header `target`, and adds the inputs of
    /// the header's phis for this back edge.
    fn reconcile_back_edge(&mut self, target: BlockIndex) -> Result<(), CompileFailure> {
        let header = self.function.ir_blocks[target].expect("started loop headers have an IR block");
        let header_frame = self.function.loop_header_frames[target]
            .clone()
            .expect("the loop header was started");
        let live = self
            .function
            .liveness
            .live_in(self.function.cfg.blocks[target].instructions.start)
            .clone();
        for slot in 0..header_frame.slots.len() {
            if !self.tracks_slot(slot) || !live.contains(slot) {
                continue;
            }
            let operand = self.function.layout.operand_for_tracked_index(slot);
            match header_frame.slots[slot] {
                SlotState::Value { node, .. } if self.graph.block(header).phis.contains(&node) => {
                    let value = self.read_tracked(slot, operand);
                    let value = self.tagged(value);
                    self.graph.nodes[node.index()].inputs.push(value);
                }
                SlotState::Value { node, .. } => {
                    if !matches!(self.frame.slots[slot], SlotState::Value { node: current, .. } if current == node) {
                        // NB: Only slots assigned in the loop can change, and those have phis.
                        return Err(CompileFailure::InvalidBytecode {
                            pc: self.function.pc,
                            reason: "loop back edge changes a value the loop does not assign",
                        });
                    }
                }
                // NB: Frame memory holds the slot at the header, and the loop
                //     does not assign it, so it still does. The this value is
                //     the exception (see `merge()`): its memory is written
                //     where the loop changed it.
                SlotState::InMemory if slot == THIS_VALUE_REGISTER as usize && !self.function.is_virtual() => {
                    self.sync_slot(slot);
                }
                SlotState::InMemory => {
                    if let SlotState::Value { in_sync: false, .. } = self.frame.slots[slot] {
                        return Err(CompileFailure::InvalidBytecode {
                            pc: self.function.pc,
                            reason: "loop back edge changes a slot the loop does not assign",
                        });
                    }
                }
            }
        }
        // NB: The fields the header knows are those the loop never replaces.
        for field in FrameField::ALL {
            if let Some(value) = header_frame.field(field)
                && self.frame.field(field) != Some(value)
            {
                return Err(CompileFailure::InvalidBytecode {
                    pc: self.function.pc,
                    reason: "loop back edge changes a frame field the loop does not replace",
                });
            }
        }
        Ok(())
    }

    // Nodes.

    fn emit(&mut self, op: Op, inputs: Vec<NodeId>, repr: Option<Repr>) -> NodeId {
        debug_assert!(!op.is_control());
        let node = self.graph.add_node(Node::new(op, inputs, repr, self.function.pc));
        self.graph.blocks[self.block.index()].body.push(node);
        node
    }

    fn set_control(&mut self, op: Op, inputs: Vec<NodeId>) -> NodeId {
        debug_assert!(op.is_control());
        let node = self.graph.add_node(Node::new(op, inputs, None, self.function.pc));
        let block = &mut self.graph.blocks[self.block.index()];
        assert!(block.control.is_none(), "a block has one control node");
        block.control = Some(node);
        node
    }

    fn int32_constant(&mut self, value: i32) -> NodeId {
        self.typed_constant(u64::from(value.cast_unsigned()), Repr::Int32)
    }

    fn typed_constant(&mut self, bits: u64, repr: Repr) -> NodeId {
        if let Some(node) = self.constants.get(&(bits, repr)) {
            return *node;
        }
        let node = self
            .graph
            .add_node(Node::new(Op::Constant(bits), Vec::new(), Some(repr), 0));
        self.constants.insert((bits, repr), node);
        node
    }

    /// The `CellAddress` of the cell `value`, a constant for constants.
    fn cell_address(&mut self, value: NodeId) -> NodeId {
        match self.graph.constant_value(value) {
            Some(bits) => self.typed_constant(
                self.runtime.heap_region_base + (bits & self.runtime.heap_region_offset_mask),
                Repr::Pointer,
            ),
            None => self.emit(Op::CellAddress, vec![value], Some(Repr::Pointer)),
        }
    }

    /// The tagged form of `value`, boxing it in the current block if it is
    /// unboxed.
    fn tagged(&mut self, value: NodeId) -> NodeId {
        self.tagged_at_end_of(self.block, value)
    }

    /// The tagged form of `value`, boxing it at the end of `block`, the
    /// current block or a finished one, if it is unboxed.
    fn tagged_at_end_of(&mut self, block: BlockId, value: NodeId) -> NodeId {
        if self.graph.node(value).repr != Some(Repr::Int32) {
            return value;
        }
        if let Some(bits) = self.graph.constant_value(value) {
            return self.constant(value::int32((bits as u32).cast_signed()));
        }
        if block == self.block {
            return self.emit(Op::BoxInt32, vec![value], Some(Repr::Tagged));
        }
        self.append_to_finished_block(block, Op::BoxInt32, vec![value], Some(Repr::Tagged))
    }

    fn constant(&mut self, bits: u64) -> NodeId {
        self.typed_constant(bits, Repr::Tagged)
    }

    // Frame slots.

    /// Whether an operand is a reserved register that lives in frame
    /// memory (see `is_reserved()`).
    fn is_reserved(&self, operand: Operand) -> bool {
        is_reserved(operand, self.function.is_virtual())
    }

    /// Whether the abstract frame tracks the slot with this tracked index,
    /// instead of leaving it in frame memory.
    fn tracks_slot(&self, slot: usize) -> bool {
        slot >= RESERVED_REGISTER_COUNT as usize || slot == THIS_VALUE_REGISTER as usize || self.function.is_virtual()
    }

    /// What the frame holds for an operand, without building anything.
    fn peek(&self, operand: Operand) -> SlotState {
        match self.function.layout.tracked_index(operand) {
            Some(slot) if !self.is_reserved(operand) => self.frame.slots[slot],
            _ => SlotState::InMemory,
        }
    }

    /// The intrinsic function an operand holds as a constant, if any.
    fn constant_intrinsic(&self, operand: Operand) -> Option<Intrinsic> {
        let SlotState::Value { node, .. } = self.peek(operand) else {
            return None;
        };
        let bits = self.graph.constant_value(node)?;
        self.intrinsics.get(&bits).copied()
    }

    /// The feedback a call instruction recorded in its feedback slot `slot`.
    fn call_feedback(&self, slot: u16) -> Option<CallFeedbackSnapshot> {
        self.function.executable.feedback.call.get(usize::from(slot)).copied()
    }

    /// Whether the instruction at `pc` of the function being walked ever ran.
    fn instruction_has_run(&self, pc: u32) -> bool {
        if self.function.index == 0 {
            (self.has_run)(pc)
        } else {
            has_run(self.function.executable, pc)
        }
    }

    /// The tagged value of an operand, loading it from the frame if needed.
    fn read(&mut self, operand: Operand) -> Result<NodeId, CompileFailure> {
        let value = self.read_value(operand)?;
        Ok(self.tagged(value))
    }

    /// The SSA value of an operand in whatever representation the frame
    /// holds it, loading it from the frame if needed.
    fn read_value(&mut self, operand: Operand) -> Result<NodeId, CompileFailure> {
        if self.is_reserved(operand) {
            return Ok(self.emit(Op::LoadSlot { slot: operand.raw() }, Vec::new(), Some(Repr::Tagged)));
        }
        if operand.raw() >= self.function.layout.arguments_base() + self.function.layout.number_of_arguments {
            return Err(CompileFailure::InvalidBytecode {
                pc: self.function.pc,
                reason: "read of an operand past the frame",
            });
        }
        match self.function.layout.tracked_index(operand) {
            Some(slot) => Ok(self.read_tracked(slot, operand)),
            None => {
                let index = (operand.raw() - self.function.layout.constants_base()) as usize;
                if index < self.function.written_constants.capacity() && self.function.written_constants.contains(index)
                {
                    return Ok(self.emit(Op::LoadSlot { slot: operand.raw() }, Vec::new(), Some(Repr::Tagged)));
                }
                let bits = *self
                    .function
                    .executable
                    .constants
                    .get(index)
                    .ok_or(CompileFailure::InvalidBytecode {
                        pc: self.function.pc,
                        reason: "read of a constant that does not exist",
                    })?;
                Ok(self.constant(bits))
            }
        }
    }

    fn read_tracked(&mut self, slot: usize, operand: Operand) -> NodeId {
        match self.frame.slots[slot] {
            SlotState::Value { node, .. } => node,
            SlotState::InMemory => {
                assert!(
                    !self.function.is_virtual(),
                    "live slots of inlined frames always hold a value"
                );
                let node = self.emit(Op::LoadSlot { slot: operand.raw() }, Vec::new(), Some(Repr::Tagged));
                self.frame.slots[slot] = SlotState::Value { node, in_sync: true };
                node
            }
        }
    }

    fn write(&mut self, operand: Operand, value: NodeId) -> Result<(), CompileFailure> {
        if self.is_reserved(operand) {
            self.emit(Op::StoreSlot { slot: operand.raw() }, vec![value], None);
            return Ok(());
        }
        if operand.raw() >= self.function.layout.arguments_base() + self.function.layout.number_of_arguments {
            return Err(CompileFailure::InvalidBytecode {
                pc: self.function.pc,
                reason: "write to an operand past the frame",
            });
        }
        let Some(slot) = self.function.layout.tracked_index(operand) else {
            // NB: A written constant lives in frame memory.
            self.emit(Op::StoreSlot { slot: operand.raw() }, vec![value], None);
            return Ok(());
        };
        // NB: Inlined frames have no memory to be out of sync with.
        let is_virtual = self.function.is_virtual();
        let state = &mut self.frame.slots[slot];
        if *state
            != (SlotState::Value {
                node: value,
                in_sync: true,
            })
        {
            *state = SlotState::Value {
                node: value,
                in_sync: is_virtual,
            };
        }
        Ok(())
    }

    /// Writes a slot's value back to frame memory if it is out of sync.
    fn sync_slot(&mut self, slot: usize) {
        if let SlotState::Value { node, in_sync: false } = self.frame.slots[slot] {
            let operand = self.function.layout.operand_for_tracked_index(slot);
            self.emit(Op::StoreSlot { slot: operand.raw() }, vec![node], None);
            self.frame.slots[slot] = SlotState::Value { node, in_sync: true };
        }
    }

    /// The frame state for eager exits of the current instruction: resume
    /// at it, with the values of every slot live into it.
    fn eager_frame_state(&mut self) -> FrameStateId {
        if let Some(frame_state) = self.function.eager_frame_state {
            return frame_state;
        }
        let frame_state = self.new_frame_state(
            self.function.liveness.live_in(self.function.instruction_index),
            ResumeMode::ResumeAt,
        );
        let frame_state = self.graph.add_frame_state(frame_state);
        self.function.eager_frame_state = Some(frame_state);
        frame_state
    }

    // Instructions.

    fn build_instruction(&mut self) -> Result<Flow, CompileFailure> {
        // NB: An instruction that never ran exits, unless that already happened here.
        let has_run = self.instruction_has_run(self.function.pc);
        if !has_run && self.may_speculate(ExitKind::NoFeedback) {
            let frame_state = self.eager_frame_state();
            let exit = self.set_control(
                Op::Exit {
                    kind: ExitKind::NoFeedback,
                },
                Vec::new(),
            );
            self.graph.nodes[exit.index()].frame_state = Some(frame_state);
            return Ok(Flow::Ended);
        }

        let instruction = self.function.instructions[self.function.instruction_index]
            .instruction
            .clone();
        if replaces_environments(instruction.opcode()) {
            self.frame.forget_environments();
        }
        match self.try_build_natively(&instruction)? {
            Some(flow) => Ok(flow),
            None => self.build_by_handling(&instruction),
        }
    }

    /// Builds an instruction whose handling is `Handling::Native`, or one
    /// that what its feedback and caches saw lets the code do faster than
    /// its handling table entry says. Returns `None`, having built nothing
    /// that matters, for the others.
    fn try_build_natively(&mut self, instruction: &Instruction) -> Result<Option<Flow>, CompileFailure> {
        match *instruction {
            Instruction::Enter => {
                // NB: Inlined frames are set up by the caller.
                if !self.function.is_virtual() {
                    self.emit(Op::InitializeFrame, Vec::new(), None);
                    let this_value = self.emit(
                        Op::LoadSlot {
                            slot: THIS_VALUE_REGISTER,
                        },
                        Vec::new(),
                        Some(Repr::Tagged),
                    );
                    self.frame.slots[THIS_VALUE_REGISTER as usize] = SlotState::Value {
                        node: this_value,
                        in_sync: true,
                    };
                }
                let empty = self.constant(value::EMPTY);
                for slot in RESERVED_REGISTER_COUNT as usize..self.function.layout.registers_and_locals_count as usize {
                    self.frame.slots[slot] = SlotState::Value {
                        node: empty,
                        in_sync: true,
                    };
                }
            }
            Instruction::Mov { dst, src } => self.build_moves(&[(dst, src)])?,
            Instruction::Mov2 {
                c0_dst,
                c0_src,
                c1_dst,
                c1_src,
            } => self.build_moves(&[(c0_dst, c0_src), (c1_dst, c1_src)])?,
            Instruction::Mov3 {
                c0_dst,
                c0_src,
                c1_dst,
                c1_src,
                c2_dst,
                c2_src,
            } => self.build_moves(&[(c0_dst, c0_src), (c1_dst, c1_src), (c2_dst, c2_src)])?,
            Instruction::MovSrcUndefined { dst } => self.build_undefined_moves(&[dst])?,
            Instruction::MovUndefined2 { c0_dst, c1_dst } => self.build_undefined_moves(&[c0_dst, c1_dst])?,
            Instruction::MovUndefined3 { c0_dst, c1_dst, c2_dst } => {
                self.build_undefined_moves(&[c0_dst, c1_dst, c2_dst])?;
            }
            Instruction::Jump { target } | Instruction::JumpLoop { target } => {
                let target = self.block_for_label(target)?;
                self.end_with_jump(target)?;
                return Ok(Some(Flow::Ended));
            }
            Instruction::JumpIf {
                condition,
                true_target,
                false_target,
            }
            | Instruction::JumpIfLoop {
                condition,
                true_target,
                false_target,
            } => {
                let if_true = self.block_for_label(true_target)?;
                let if_false = self.block_for_label(false_target)?;
                self.build_truthiness_branch(condition, if_true, if_false)?;
                return Ok(Some(Flow::Ended));
            }
            Instruction::JumpTrue { condition, target } | Instruction::JumpTrueLoop { condition, target } => {
                let if_true = self.block_for_label(target)?;
                let if_false = self.fallthrough_block()?;
                self.build_truthiness_branch(condition, if_true, if_false)?;
                return Ok(Some(Flow::Ended));
            }
            Instruction::JumpFalse { condition, target } | Instruction::JumpFalseLoop { condition, target } => {
                let if_true = self.fallthrough_block()?;
                let if_false = self.block_for_label(target)?;
                self.build_truthiness_branch(condition, if_true, if_false)?;
                return Ok(Some(Flow::Ended));
            }
            Instruction::JumpNullish {
                condition,
                true_target,
                false_target,
            } => {
                self.build_tag_branch(BranchCondition::Nullish, condition, true_target, false_target)?;
                return Ok(Some(Flow::Ended));
            }
            Instruction::JumpUndefined {
                condition,
                true_target,
                false_target,
            } => {
                self.build_tag_branch(BranchCondition::Undefined, condition, true_target, false_target)?;
                return Ok(Some(Flow::Ended));
            }
            Instruction::Not { dst, src } => self.build_truthiness_value(dst, src, true)?,
            Instruction::ToBoolean { dst, value } => self.build_truthiness_value(dst, value, false)?,
            Instruction::Return { value } => {
                let value = self.read(value)?;
                if self.function.is_virtual() {
                    return Ok(Some(self.build_inline_return(value, true)));
                }
                // NB: An empty value returns `undefined`, but a value that is
                //     never empty needs no check for it.
                let layout = self.function.layout;
                let arguments = layout.arguments_base()..layout.arguments_base() + layout.number_of_arguments;
                let value = if self.graph.is_known_non_empty(value, arguments) {
                    value
                } else {
                    self.emit(Op::EmptyToUndefined, vec![value], Some(Repr::Tagged))
                };
                self.set_control(Op::Return, vec![value]);
                return Ok(Some(Flow::Ended));
            }
            Instruction::End { value } => {
                let value = self.read(value)?;
                // NB: In an inlined callee, End returns its value (never
                //     empty) to the caller like Return does.
                if self.function.is_virtual() {
                    return Ok(Some(self.build_inline_return(value, false)));
                }
                self.set_control(Op::Return, vec![value]);
                return Ok(Some(Flow::Ended));
            }
            Instruction::GetLexicalEnvironment { dst } => {
                let address = self.frame_field(FrameField::LexicalEnvironment)?;
                let environment = self.emit(Op::BoxCell, vec![address], Some(Repr::Tagged));
                self.write(dst, environment)?;
            }
            Instruction::SetLexicalEnvironment { environment } => {
                let environment = self.read(environment)?;
                let address = self.emit(Op::SetLexicalEnvironment, vec![environment], Some(Repr::Pointer));
                self.frame.set_field(FrameField::LexicalEnvironment, address);
            }
            Instruction::LeavePrivateEnvironment => {
                self.emit(Op::LeavePrivateEnvironment, Vec::new(), None);
            }
            Instruction::IsCallable { dst, value } => {
                let value = self.read(value)?;
                let result = self.emit(Op::IsCallable, vec![value], Some(Repr::Tagged));
                self.write(dst, result)?;
            }
            Instruction::GetArgumentCount { dst } => {
                // NB: An inlined callee was passed as many arguments as its call passed.
                let count = match &self.function.inline {
                    Some(call) => self.constant(value::int32(call.passed_argument_count.cast_signed())),
                    None => self.emit(Op::ArgumentCount, Vec::new(), Some(Repr::Tagged)),
                };
                self.write(dst, count)?;
            }
            Instruction::CreateArguments {
                dst,
                kind,
                creates_parameter_bindings,
                ..
            } => {
                // NB: Creating the parameter bindings is a side effect that a virtual arguments object would skip.
                if creates_parameter_bindings || !self.try_build_virtual_arguments(dst, kind)? {
                    return Ok(None);
                }
            }
            Instruction::NewArray { dst, ref elements, .. } => {
                if elements.is_empty()
                    && let Some(flow) = self.try_build_spread_forwarding(dst)?
                {
                    return Ok(Some(flow));
                }
                let elements = elements
                    .iter()
                    .copied()
                    .map(allocation::ArrayElement::Operand)
                    .collect::<Vec<_>>();
                if !self.try_build_new_array(dst, &elements)? {
                    return Ok(None);
                }
            }
            Instruction::NewFunction {
                dst,
                shared_function_data_index,
                home_object,
                ..
            } => {
                if !self.try_build_new_function(dst, shared_function_data_index, home_object.is_some())? {
                    return Ok(None);
                }
            }
            Instruction::CreateLexicalEnvironment {
                dst,
                parent,
                capacity,
                shape_cache,
                is_catch_environment: false,
            } => {
                if !self.try_build_create_lexical_environment(dst, parent, capacity, shape_cache)? {
                    return Ok(None);
                }
            }
            Instruction::NewPrimitiveArray { dst, ref elements, .. } => {
                let elements = elements
                    .iter()
                    .copied()
                    .map(allocation::ArrayElement::Constant)
                    .collect::<Vec<_>>();
                if !self.try_build_new_array(dst, &elements)? {
                    return Ok(None);
                }
            }
            Instruction::NewObject { dst, cache } => {
                if !self.try_build_new_object(dst, cache)? {
                    return Ok(None);
                }
            }
            Instruction::InitObjectLiteralProperty {
                object,
                src,
                shape_cache_index,
                property_slot,
                ..
            } => {
                if !self.try_build_init_object_literal_property(object, src, shape_cache_index, property_slot)? {
                    return Ok(None);
                }
            }
            Instruction::CacheObjectShape { object, cache } => {
                if !self.try_build_cache_object_shape(object, cache)? {
                    return Ok(None);
                }
            }
            Instruction::GetLength { dst, base, .. } => {
                if !self.try_build_arguments_length(dst, base)? {
                    return Ok(None);
                }
            }
            Instruction::CallConstruct { .. } => return self.try_inline_construct(instruction),
            Instruction::Call { .. } => {
                if let Some(flow) = self.try_build_array_push(instruction)? {
                    return Ok(Some(flow));
                }
                if let Some(flow) = self.try_build_has_own_property(instruction)? {
                    return Ok(Some(flow));
                }
                if let Some(flow) = self.try_build_builtin_constructor_call(instruction)? {
                    return Ok(Some(flow));
                }
                if let Some(flow) = self.try_build_arguments_slice(instruction)? {
                    return Ok(Some(flow));
                }
                if let Some(flow) = self.try_inline_call(instruction)? {
                    return Ok(Some(flow));
                }
                if let Some(forwarded) = self.forwarded_arguments(instruction) {
                    return self.build_forwarding_call(instruction, forwarded).map(Some);
                }
                if let Some(flow) = self.try_direct_call(instruction)? {
                    return Ok(Some(flow));
                }
                return Ok(None);
            }
            Instruction::CallBuiltinStringFromCharCode { .. } => {
                return self.try_build_string_from_char_code(instruction);
            }
            Instruction::CreateDataPropertyOrThrow {
                object,
                property,
                value,
            } => return self.build_create_data_property(object, property, value),
            Instruction::In { dst, lhs, rhs } => return self.try_build_in(dst, lhs, rhs),
            Instruction::Typeof { dst, src } if self.runtime.layout.typeof_strings.number != 0 => {
                let value = self.read(src)?;
                self.assume_no_htmldda_objects();
                let result = self.emit(Op::Typeof, vec![value], Some(Repr::Tagged));
                self.write(dst, result)?;
            }
            Instruction::InstanceOf { dst, lhs, rhs } => {
                if !self.try_build_instance_of(dst, lhs, rhs)? {
                    return Ok(None);
                }
            }
            Instruction::GetGlobal { dst, cache, .. } => {
                if !self.try_build_get_global(dst, cache)? {
                    return Ok(None);
                }
            }
            // NB: `typeof` of a global variable its cache found is the type of the value GetGlobal reads.
            Instruction::TypeofGlobal { dst, cache, .. }
                if self.runtime.layout.typeof_strings.number != 0 && self.try_build_get_global(dst, cache)? =>
            {
                let value = self.read(dst)?;
                self.assume_no_htmldda_objects();
                let result = self.emit(Op::Typeof, vec![value], Some(Repr::Tagged));
                self.write(dst, result)?;
            }
            Instruction::SetGlobal { src, cache, .. } => {
                if !self.try_build_set_global(src, cache)? {
                    return Ok(None);
                }
            }
            Instruction::GetById { dst, base, cache, .. } => {
                if let Some(flow) = self.try_inline_getter(dst, base, cache)? {
                    return Ok(Some(flow));
                }
                if !self.try_build_get_by_id(dst, base, cache)? {
                    return Ok(None);
                }
            }
            Instruction::PutById { base, src, cache, .. } => {
                if let Some(flow) = self.try_inline_setter(base, src, cache)? {
                    return Ok(Some(flow));
                }
                if !self.try_build_put_by_id(base, src, cache)? && !self.try_build_add_named(base, src, cache)? {
                    return Ok(None);
                }
            }
            Instruction::GetByValue {
                dst,
                base,
                property,
                cache,
                ..
            } => {
                if !self.try_build_arguments_index(dst, base, property)?
                    && !self.try_build_element_load(instruction, dst, base, property)?
                    && !self.try_build_get_by_value(dst, base, property, cache)?
                {
                    return Ok(None);
                }
            }
            // NB: Only plain assignments store into existing data
            //     properties like the cache says.
            Instruction::PutByValue {
                base,
                property,
                src,
                kind: PUT_KIND_NORMAL,
                cache,
                ..
            } => {
                if !self.try_build_element_store(instruction, base, property, src)?
                    && !self.try_build_put_by_value(base, property, src, cache)?
                {
                    return Ok(None);
                }
            }
            _ => return Ok(None),
        }
        Ok(Some(Flow::Continue))
    }

    /// Builds an instruction as its handling table entry says.
    fn build_by_handling(&mut self, instruction: &Instruction) -> Result<Flow, CompileFailure> {
        let opcode = instruction.opcode();
        match handling(opcode) {
            Handling::Generic(info) => self.build_generic(instruction, info),
            Handling::Expanded(info) => self.build_expanded(instruction, info),
            Handling::Unsupported(reason) => Err(CompileFailure::UnsupportedInstruction {
                pc: self.function.pc,
                opcode,
                reason,
            }),
            Handling::Native => unreachable!("{} is native but not built natively", opcode.name()),
        }
    }

    fn fallthrough_block(&self) -> Result<BlockIndex, CompileFailure> {
        self.block_for_label(Label(
            self.function.instructions[self.function.instruction_index].next_pc(),
        ))
    }

    fn build_moves(&mut self, moves: &[(Operand, Operand)]) -> Result<(), CompileFailure> {
        for (dst, src) in moves {
            let value = self.read(*src)?;
            self.write(*dst, value)?;
        }
        Ok(())
    }

    fn build_undefined_moves(&mut self, destinations: &[Operand]) -> Result<(), CompileFailure> {
        let undefined = self.constant(value::UNDEFINED);
        for dst in destinations {
            self.write(*dst, undefined)?;
        }
        Ok(())
    }

    fn build_tag_branch(
        &mut self,
        condition: BranchCondition,
        operand: Operand,
        true_target: Label,
        false_target: Label,
    ) -> Result<(), CompileFailure> {
        let if_true = self.block_for_label(true_target)?;
        let if_false = self.block_for_label(false_target)?;
        let value = self.read(operand)?;
        self.end_with_branch(
            |if_true, if_false| Op::Branch {
                condition,
                if_true,
                if_false,
            },
            vec![value],
            if_true,
            if_false,
        )
    }

    /// Branches on the truthiness of `operand`. Booleans and int32 values are
    /// tested inline; other values go through a fallback block that calls the
    /// runtime's `to_boolean` helper.
    fn build_truthiness_branch(
        &mut self,
        operand: Operand,
        if_true: BlockIndex,
        if_false: BlockIndex,
    ) -> Result<(), CompileFailure> {
        let value = self.read(operand)?;
        let source = self.block;
        let frame = self.frame.clone();
        let [true_edge, false_edge, fallback] = [(); 3].map(|()| {
            let block = self.graph.add_block(Block {
                predecessors: vec![source],
                ..Block::default()
            });
            self.order.push(block);
            block
        });
        self.assume_no_htmldda_objects();
        self.set_control(
            Op::BranchTruthy {
                if_true: true_edge,
                if_false: false_edge,
                fallback,
            },
            vec![value],
        );
        for (edge, target) in [(true_edge, if_true), (false_edge, if_false)] {
            self.block = edge;
            self.frame = frame.clone();
            self.end_with_jump(target)?;
        }
        self.block = fallback;
        self.frame = frame;
        let boolean = self.emit(Op::ToBoolean, vec![value], Some(Repr::Bool));
        self.end_with_branch(
            |if_true, if_false| Op::Branch {
                condition: BranchCondition::Bool,
                if_true,
                if_false,
            },
            vec![boolean],
            if_true,
            if_false,
        )
    }

    /// Stores the truthiness of `src` (negated if `negate`) as a boolean
    /// value into `dst`, with a truthiness branch and a phi of constants.
    fn build_truthiness_value(&mut self, dst: Operand, src: Operand, negate: bool) -> Result<(), CompileFailure> {
        let value = self.read(src)?;
        let source = self.block;
        let frame = self.frame.clone();
        let [true_block, false_block, fallback] = [(); 3].map(|()| self.add_block_after(source));
        self.assume_no_htmldda_objects();
        self.set_control(
            Op::BranchTruthy {
                if_true: true_block,
                if_false: false_block,
                fallback,
            },
            vec![value],
        );

        self.block = fallback;
        let boolean = self.emit(Op::ToBoolean, vec![value], Some(Repr::Bool));
        let (fallback_true_block, fallback_false_block) = self.branch_both_ways(BranchCondition::Bool, vec![boolean]);
        self.join_blocks(vec![true_block, false_block, fallback_true_block, fallback_false_block]);
        self.frame = frame;

        let (truthy, falsy) = if negate {
            (value::FALSE, value::TRUE)
        } else {
            (value::TRUE, value::FALSE)
        };
        let truthy = self.constant(truthy);
        let falsy = self.constant(falsy);
        let result = self.add_phi(vec![truthy, falsy, truthy, falsy]);
        self.write(dst, result)
    }

    /// Builds an instruction as a generic node: write back every slot it may
    /// read, run its slow path, and forget the SSA values of the slots it
    /// writes, so that later reads load them from the frame.
    fn build_generic(&mut self, instruction: &Instruction, info: GenericInfo) -> Result<Flow, CompileFailure> {
        if let Some(flow) = self.try_build_slow_path_call(instruction, info)? {
            return Ok(flow);
        }
        let op = Op::Generic {
            opcode: instruction.opcode(),
            executable: self.function.index,
            pc: self.function.pc,
        };
        self.build_generic_node(instruction, info, op)
    }

    /// Like `build_generic()`, with `op` (a `Generic` node or one that runs
    /// the instruction like one) running the instruction.
    pub(super) fn build_generic_node(
        &mut self,
        instruction: &Instruction,
        info: GenericInfo,
        op: Op,
    ) -> Result<Flow, CompileFailure> {
        self.build_generic_node_with_inputs(instruction, info, op, &[], None)
    }

    /// Like `build_generic_node()`, with the values of the operands `inputs`
    /// as the node's inputs: the node's code reads them from its inputs, and
    /// writes them to the frame itself before its slow paths read them, so
    /// they are not written to the frame before it. The node does not read
    /// `forwarded`, the arguments object operand of a call that forwards the
    /// frame's arguments instead.
    pub(super) fn build_generic_node_with_inputs(
        &mut self,
        instruction: &Instruction,
        info: GenericInfo,
        op: Op,
        inputs: &[Operand],
        forwarded: Option<Operand>,
    ) -> Result<Flow, CompileFailure> {
        let opcode = instruction.opcode();
        let input_nodes = inputs
            .iter()
            .map(|operand| self.read(*operand))
            .collect::<Result<Vec<_>, _>>()?;
        let input_slots = inputs
            .iter()
            .filter_map(|operand| self.function.layout.tracked_index(*operand))
            .collect::<Vec<_>>();
        let live_in = self.function.liveness.live_in(self.function.instruction_index).clone();
        // NB: A virtual arguments object stays virtual unless the slow path reads
        //     it; codegen creates it if the slow path leaves the compiled code.
        let mut read_slots = Vec::new();
        let mut written_slots = Vec::new();
        instruction.for_each_operand(|operand, role| {
            if role.is_read() && Some(operand) != forwarded {
                read_slots.extend(
                    self.function
                        .layout
                        .tracked_index(operand)
                        .filter(|slot| !input_slots.contains(slot)),
                );
            }
            if role.is_written() {
                written_slots.extend(self.function.layout.tracked_index(operand));
            }
        });
        // NB: The slow path reads its operands from the frame, and a slot it
        //     writes keeps its old value if it throws first. The other values
        //     only reach the frames if the slow path does not continue in
        //     compiled code (see `codegen::LeaveFrame`).
        let writes_only_operands = !info.reads_whole_frame;
        // NB: Calls with inputs have their result as their value, and their
        //     frame state gives the destination its old value where the call
        //     does not continue (see `ir::FrameState::in_frame`).
        let value_is_result = !input_nodes.is_empty() && matches!(op, Op::CallDirect { .. } | Op::CallNative { .. });
        let operand_slots = read_slots
            .iter()
            .copied()
            .chain(
                written_slots
                    .iter()
                    .copied()
                    .filter(|slot| live_in.contains(*slot) && !value_is_result),
            )
            .collect::<Vec<_>>();
        // NB: Calls in inlined callees run in the published frames of the
        //     inlined calls (see `SiteKind::Publish`), which take all their
        //     operands as inputs. Nothing else reads the frames of inlined
        //     callees (see `inlining::can_inline()`).
        if self.function.is_virtual() && !value_is_result {
            return Err(CompileFailure::UnsupportedInstruction {
                pc: self.function.pc,
                opcode,
                reason: "reads the frame of an inlined callee",
            });
        }
        let out_of_sync = (0..self.frame.slots.len())
            .filter(|slot| match self.frame.slots[*slot] {
                SlotState::Value { node, in_sync: false } => {
                    let needed = if writes_only_operands {
                        operand_slots.contains(slot)
                    } else {
                        info.reads_whole_frame || live_in.contains(*slot)
                    };
                    needed && (!self.is_virtual_arguments(node) || info.reads_whole_frame || read_slots.contains(slot))
                }
                _ => false,
            })
            .collect::<Vec<_>>();
        for slot in out_of_sync {
            self.sync_slot(slot);
        }

        let mut targets = Vec::new();
        instruction.for_each_jump_target(|label| targets.push(label));
        let is_conditional_jump = !targets.is_empty();
        let repr = if is_conditional_jump {
            Some(Repr::Int32)
        } else {
            value_is_result.then_some(Repr::Tagged)
        };
        let node = self.emit(op, input_nodes, repr);

        let mut destination = None;
        instruction.for_each_operand(|operand, role| {
            if !role.is_written() {
                return;
            }
            destination.get_or_insert(operand);
            if (self.is_reserved(operand) && !self.function.is_virtual()) || value_is_result {
                return;
            }
            // NB: Written constants live in frame memory, where the slow path writes them.
            if let Some(slot) = self.function.layout.tracked_index(operand) {
                self.frame.slots[slot] = SlotState::InMemory;
            }
        });

        let frame_state = self.resume_after_frame_state(destination.map_or(Operand::INVALID, Operand::raw));
        self.graph.nodes[node.index()].frame_state = Some(frame_state);
        if value_is_result && let Some(destination) = destination {
            self.write(destination, node)?;
        }
        self.end_generic_instruction(instruction, node)
    }

    /// Ends the instruction `node` ran through its slow path: with a branch
    /// on the control word of a conditional jump, and after instructions
    /// that never continue with the next one.
    fn end_generic_instruction(&mut self, instruction: &Instruction, node: NodeId) -> Result<Flow, CompileFailure> {
        let mut targets = Vec::new();
        instruction.for_each_jump_target(|label| targets.push(label));
        let is_conditional_jump = !targets.is_empty();
        if is_conditional_jump {
            let if_true = self.block_for_label(targets[0])?;
            let if_false = match targets.get(1) {
                Some(label) => self.block_for_label(*label)?,
                None => self.fallthrough_block()?,
            };
            if if_true == if_false {
                self.end_with_jump(if_true)?;
            } else {
                let true_pc = targets[0].0;
                self.end_with_branch(
                    |if_true, if_false| Op::BranchOnPc {
                        pc: true_pc,
                        if_true,
                        if_false,
                    },
                    vec![node],
                    if_true,
                    if_false,
                )?;
            }
            return Ok(Flow::Ended);
        }
        if !instruction.opcode().can_fall_through() {
            self.set_control(Op::Unreachable, Vec::new());
            return Ok(Flow::Ended);
        }
        Ok(Flow::Continue)
    }
}

#[cfg(test)]
pub(crate) mod tests;
