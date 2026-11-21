/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The graph builder: one forward walk over the bytecode in reverse post
//! order that turns it into SSA IR.
//!
//! The builder keeps an abstract frame: for every register (the reserved ones included), local and argument slot, the
//! SSA value it currently holds, and whether frame memory holds that value too (`in_sync`). `Mov` only renames,
//! constants become constant nodes, and slow paths and calls take the values of their operands as inputs and give their
//! outputs as values (see `slow_paths`). Values come from frame memory only where the code starts using it: `LoadSlot`
//! of every slot live at an entry (the function's start, and those that continue a frame, see `osr`), and of the slots
//! a node that writes the frame wrote, right after it. Values go back to frame memory only where something reads them
//! there: `StoreSlot` of out of sync slots before calls that read the frame (`Generic` nodes, and calls that read their
//! operands there) and before `Suspend`, and of the arguments that the legacy `arguments` of the compiled function
//! shows, which any code it calls can read. Merges and loop back edges never write frame memory: slots that differ get
//! phis. The verifier checks that `StoreSlot` writes nothing else (see `passes::verify`).
//!
//! Every bytecode basic block becomes one IR block. Phis are created lazily
//! at forward merges, and eagerly at loop headers for slots assigned in the
//! loop. Every conditional branch goes through a fresh block per successor,
//! so a block with several successors never leads directly to a merge, and
//! code that reconciles abstract frames at a merge always has a place to go.

mod allocation;
mod arguments;
mod array_iterators;
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
use crate::ir::Block;
use crate::ir::BlockId;
use crate::ir::BranchCondition;
use crate::ir::FrameField;
use crate::ir::FrameStateId;
use crate::ir::Graph;
use crate::ir::Locations;
use crate::ir::NodeId;
use crate::ir::Op;
use crate::ir::SuspendKind;
use crate::ir::value;
use crate::snapshot::CallFeedbackSnapshot;
use crate::snapshot::CellId;
use crate::snapshot::ExecutableSnapshot;
use crate::snapshot::Intrinsic;
use crate::snapshot::RuntimeInfo;
use crate::snapshot::Snapshot;
use ak::Zone;
use ak::ZoneBitSet;
use ak::ZoneMap;
use ak::ZoneVec;
use core::cell::Cell;
use core::cell::OnceCell;
pub(crate) use slow_paths::is_ssa_slow_path_output;
pub(crate) use slow_paths::js_call_operands;

/// Builds the IR for `snapshot.executables[0]`, with the graph and the work data of the build both in `zone`.
#[cfg(test)]
pub fn build_graph<'z>(snapshot: &Snapshot, zone: &'z Zone) -> Result<Graph<'z>, CompileFailure> {
    build_graph_with_feedback(
        snapshot,
        &|pc| {
            snapshot
                .executables
                .first()
                .is_none_or(|executable| has_run(snapshot, executable, pc))
        },
        zone,
    )
}

/// Like `build_graph()`, with the "did this instruction ever run" oracle given by the caller.
#[cfg(test)]
pub fn build_graph_with_feedback<'z>(
    snapshot: &Snapshot,
    has_run: &dyn Fn(u32) -> bool,
    zone: &'z Zone,
) -> Result<Graph<'z>, CompileFailure> {
    let mut attempts = Attempts::new(snapshot, has_run, zone);
    loop {
        if let Some(graph) = attempts.build(zone)? {
            return Ok(graph);
        }
    }
}

/// Whether the instruction at `pc` ever ran in the interpreter, as far as the
/// feedback it records tells: an instruction that records feedback ran if any
/// of its slots recorded something. Instructions without feedback count as
/// having run. Instructions that never ran are compiled as unconditional exits.
pub fn has_run(snapshot: &Snapshot, executable: &ExecutableSnapshot, pc: u32) -> bool {
    let Some(slots) = crate::bytecode::feedback_slots_at(&snapshot[executable.bytecode], pc) else {
        return true;
    };
    let feedback = &executable.feedback;
    let slot = |slot: Option<u16>| slot.map(usize::from);
    let recorded = [
        slot(slots.arith)
            .and_then(|slot| snapshot[feedback.arith].get(slot))
            .map(|bits| *bits != 0),
        slot(slots.value)
            .and_then(|slot| snapshot[feedback.value].get(slot))
            .map(|bits| *bits != 0),
        slot(slots.cache_value)
            .and_then(|cache| snapshot[feedback.cache_value].get(cache))
            .map(|bits| *bits != 0),
        slot(slots.call)
            .and_then(|slot| snapshot[feedback.call].get(slot))
            .map(|call| call.target.is_some() || call.flags != 0 || call.value != 0),
        slot(slots.keyed)
            .and_then(|slot| snapshot[feedback.keyed].get(slot))
            .map(|keyed| keyed.bits != 0),
    ];
    recorded.iter().all(Option::is_none) || recorded.contains(&Some(true))
}

/// The builds of the IR for `snapshot.executables[0]`: one, or more where a graph came out that cannot be used.
///
/// Where an inlined callee had a virtual arguments object, a graph that needs it, or a build that failed, is built
/// again without those (see `arguments`), which then are not inlined. A graph that needs the arguments object of the
/// compiled function itself is built again without its on-stack replacement entries, and then with a real arguments
/// object.
pub struct Attempts<'a> {
    snapshot: &'a Snapshot,
    /// Whether the instruction at a pc ever ran. The instructions that never ran become unconditional exits.
    has_run: &'a dyn Fn(u32) -> bool,
    /// Where the builds keep their own data. It holds the data of all the builds until its owner resets it.
    work: &'a Zone,
    decoded: &'a [OnceCell<Result<DecodedFunction<'a>, CompileFailure>>],
    virtualized_inlined_arguments: &'a Cell<bool>,
    next: Attempt,
}

/// What a build of `Attempts` does differently from the first one.
#[derive(Clone, Copy)]
enum Attempt {
    First,
    WithoutVirtualArgumentsOfInlinedCallees,
    WithoutOsrEntries,
    WithoutVirtualArguments,
}

impl<'a> Attempts<'a> {
    pub fn new(snapshot: &'a Snapshot, has_run: &'a dyn Fn(u32) -> bool, work: &'a Zone) -> Self {
        Self {
            snapshot,
            has_run,
            work,
            decoded: work.alloc_slice_from_iter((0..snapshot.executables.len()).map(|_| OnceCell::new())),
            virtualized_inlined_arguments: work.alloc(Cell::new(false)),
            next: Attempt::First,
        }
    }

    /// Builds a graph in `zone`. Returns `None` for a graph that cannot be used: the caller then drops what is in
    /// `zone` and calls this again, which builds the graph in another way. The builds keep the decoded functions and
    /// their analyses, which are in the work zone.
    pub fn build<'z>(&mut self, zone: &'z Zone) -> Result<Option<Graph<'z>>, CompileFailure> {
        let build = |virtualize_arguments, virtualize_inlined_arguments, build_osr_entries| {
            GraphBuilder::new(
                self.snapshot,
                self.has_run,
                self.decoded,
                ArgumentsVirtualization {
                    compiled_function: virtualize_arguments,
                    inlined_callees: virtualize_inlined_arguments,
                    inlined_callees_virtualized: self.virtualized_inlined_arguments,
                },
                build_osr_entries,
                zone,
                self.work,
            )?
            .build()
        };
        let graph = match self.next {
            Attempt::First => match build(true, true, true) {
                Ok(graph) if !arguments::uses_virtual_arguments(&graph) => return Ok(Some(graph)),
                _ if self.virtualized_inlined_arguments.get() => {
                    self.next = Attempt::WithoutVirtualArgumentsOfInlinedCallees;
                    return Ok(None);
                }
                result => result?,
            },
            Attempt::WithoutVirtualArgumentsOfInlinedCallees => {
                let graph = build(true, false, true)?;
                if !arguments::uses_virtual_arguments(&graph) {
                    return Ok(Some(graph));
                }
                graph
            }
            Attempt::WithoutOsrEntries => {
                let graph = build(true, false, false)?;
                if !arguments::uses_virtual_arguments(&graph) {
                    return Ok(Some(graph));
                }
                // Something needs the arguments object itself.
                self.next = Attempt::WithoutVirtualArguments;
                return Ok(None);
            }
            Attempt::WithoutVirtualArguments => return build(false, false, true).map(Some),
        };
        // NB: The graph uses the virtual arguments object of the compiled function.
        self.next = if graph.osr_entries.is_empty() {
            // Something needs the arguments object itself.
            Attempt::WithoutVirtualArguments
        } else {
            Attempt::WithoutOsrEntries
        };
        Ok(None)
    }
}

/// Where the graph builder may build virtual arguments objects (see
/// `arguments`).
#[derive(Clone, Copy)]
struct ArgumentsVirtualization<'a> {
    compiled_function: bool,
    inlined_callees: bool,
    /// Set once an inlined callee got a virtual arguments object.
    inlined_callees_virtualized: &'a Cell<bool>,
}

/// What the abstract frame knows about one slot: the value it holds, and whether frame memory holds that value too.
/// Frame states list a slot that is in sync as held by the frame (see `ir::FrameState::in_frame`), so that it keeps
/// no value alive, and nodes that read the frame need no store of it.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
struct SlotState {
    node: NodeId,
    in_sync: bool,
}

/// How many allocated objects the graph builder knows the shapes of at once
/// (see `AbstractFrame::fresh_shapes`).
const MAX_FRESH_SHAPES: usize = 16;

/// The builder's view of the frame's registers, locals and arguments,
/// indexed by `FrameLayout::tracked_index()`, and of the fields of its
/// execution context.
#[derive(Debug)]
struct AbstractFrame<'a> {
    /// Every slot holds a value. Slots that are not live hold `undefined` where the walk does not know their values
    /// (at entries and merges), as in sync, so that nothing ever stores them, though frame memory may hold anything.
    slots: ZoneVec<'a, SlotState>,
    /// For each `FrameField` (in the order of `FrameField::ALL`), the value
    /// the field holds on this path, once loaded or set (see
    /// `GraphBuilder::frame_field()`).
    fields: [Option<NodeId>; FrameField::ALL.len()],
    /// The shapes of objects the code allocated, by their allocation nodes,
    /// as far as this path knows them: each object's shape since its last
    /// addition, until something runs that may change shapes otherwise.
    fresh_shapes: ZoneVec<'a, (NodeId, CellId)>,
}

impl Clone for AbstractFrame<'_> {
    fn clone(&self) -> Self {
        Self {
            slots: self.slots.clone(),
            fields: self.fields,
            fresh_shapes: self.fresh_shapes.clone(),
        }
    }

    fn clone_from(&mut self, other: &Self) {
        // NB: This keeps the memory of the lists where the copies fit.
        self.slots.assign(&other.slots);
        self.fields = other.fields;
        self.fresh_shapes.assign(&other.fresh_shapes);
    }
}

impl<'a> AbstractFrame<'a> {
    /// A frame whose slots all hold `value` in sync and whose fields are not loaded yet, with its lists in `work`.
    fn filled(slot_count: usize, value: NodeId, work: &'a Zone) -> Self {
        Self {
            slots: ZoneVec::from_elem_in(
                SlotState {
                    node: value,
                    in_sync: true,
                },
                slot_count,
                work,
            ),
            fields: [None; FrameField::ALL.len()],
            fresh_shapes: ZoneVec::new_in(work),
        }
    }

    fn field(&self, field: FrameField) -> Option<NodeId> {
        self.fields[field as usize]
    }

    fn set_field(&mut self, field: FrameField, value: NodeId) {
        self.fields[field as usize] = Some(value);
    }

    /// Makes `fresh_shapes` the shapes every one of `frames` (their
    /// `fresh_shapes`) knows its objects have.
    fn set_common_fresh_shapes<'f>(&mut self, mut frames: impl Iterator<Item = &'f [(NodeId, CellId)]>) {
        self.fresh_shapes.clear();
        let Some(first) = frames.next() else {
            return;
        };
        self.fresh_shapes.extend_from_slice(first);
        for other in frames {
            self.fresh_shapes.retain(|shape| other.contains(shape));
        }
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
fn loops_replacing_environments<'w>(
    cfg: &Cfg<'_>,
    instructions: &[DecodedInstruction],
    work: &'w Zone,
) -> ZoneVec<'w, bool> {
    ZoneVec::from_iter_in(
        cfg.loops.iter().map(|current_loop| {
            current_loop.blocks.iter().any(|block| {
                cfg.blocks[*block]
                    .instructions
                    .clone()
                    .any(|index| replaces_environments(instructions[index].instruction.opcode()))
            })
        }),
        work,
    )
}

/// What ends the instruction currently being built.
enum Flow {
    /// The block continues with the next instruction.
    Continue,
    /// The instruction ended the block.
    Ended,
}

fn written_constants<'w>(instructions: &[DecodedInstruction], layout: &FrameLayout, work: &'w Zone) -> ZoneBitSet<'w> {
    let mut written = ZoneBitSet::new_in(layout.number_of_constants as usize, work);
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
fn loop_phi_slots<'w>(cfg: &Cfg<'w>, work: &'w Zone) -> ZoneVec<'w, ZoneBitSet<'w>> {
    let mut slots = ZoneVec::from_iter_in(
        cfg.loops.iter().map(|current_loop| current_loop.assigned_slots.clone()),
        work,
    );
    let mut other_slots = ZoneBitSet::new_in(0, work);
    let mut changed = true;
    while changed {
        changed = false;
        for (index, current_loop) in cfg.loops.iter().enumerate() {
            for (other_index, other) in cfg.loops.iter().enumerate() {
                if other_index != index && current_loop.blocks.contains(&other.header) {
                    other_slots.clone_from(&slots[other_index]);
                    changed |= slots[index].union_with(&other_slots);
                }
            }
        }
    }
    slots
}

/// Bytecode decoding and analyses shared by every walk of an executable.
/// Analyses are lazy: an inlining budget rejection only needs decoding.
/// The analyses are in the work zone of the builds.
struct DecodedFunction<'w> {
    instructions: &'w [DecodedInstruction<'w>],
    analysis: OnceCell<Result<FunctionAnalysis<'w>, CompileFailure>>,
}

fn decoded_function<'c, 'w>(
    snapshot: &Snapshot,
    executable: &ExecutableSnapshot,
    cache: &'c OnceCell<Result<DecodedFunction<'w>, CompileFailure>>,
    work: &'w Zone,
) -> Result<&'c DecodedFunction<'w>, CompileFailure> {
    cache
        .get_or_init(|| {
            let bytecode = &snapshot[executable.bytecode];
            let instructions = decode_all(bytecode, work).map_err(|error| CompileFailure::InvalidBytecode {
                pc: error.pc,
                reason: "undecodable instruction",
            })?;
            if instructions.is_empty() {
                return Err(CompileFailure::InvalidBytecode {
                    pc: 0,
                    reason: "empty bytecode",
                });
            }
            Ok(DecodedFunction {
                instructions,
                analysis: OnceCell::new(),
            })
        })
        .as_ref()
        .map_err(|error| *error)
}

struct FunctionAnalysis<'w> {
    cfg: Cfg<'w>,
    /// Liveness without and with arguments observable through `f.arguments`.
    liveness: [OnceCell<Liveness<'w>>; 2],
    loop_phi_slots: ZoneVec<'w, ZoneBitSet<'w>>,
    loops_replacing_environments: ZoneVec<'w, bool>,
    written_constants: ZoneBitSet<'w>,
    writes_arguments: bool,
    may_write_parameter_bindings: bool,
}

/// The state of the builder's walk over one executable: the compiled
/// function or an inlined callee.
struct Function<'a> {
    snapshot: &'a Snapshot,
    /// Index into `Snapshot::executables`.
    index: u32,
    executable: &'a ExecutableSnapshot,
    layout: FrameLayout,
    instructions: &'a [DecodedInstruction<'a>],
    cfg: &'a Cfg<'a>,
    liveness: &'a Liveness<'a>,
    // NB: The lists of the walk below have an element for each bytecode block once the walk starts (see
    //     `start_walk()`), and none before.
    /// The IR block of each bytecode block, created when first referenced.
    ir_blocks: ZoneVec<'a, Option<BlockId>>,
    /// Forward edges into each bytecode block not yet started: the IR
    /// predecessor and the abstract frame at its end.
    incoming: ZoneVec<'a, ZoneVec<'a, (BlockId, AbstractFrame<'a>)>>,
    /// Whether each bytecode block was started.
    started: ZoneVec<'a, bool>,
    /// The abstract frame at the start of each loop header that was started,
    /// which every back edge must reconcile with.
    loop_header_frames: ZoneVec<'a, Option<AbstractFrame<'a>>>,
    /// For each loop, the slots that get phis at its header.
    loop_phi_slots: &'a [ZoneBitSet<'a>],
    /// For each loop, whether it may replace an environment.
    loops_replacing_environments: &'a [bool],
    /// The loop headers that on-stack replacement entries lead to over back
    /// edges, which are built even if no forward edge reaches them.
    osr_loop_headers: ZoneVec<'a, bool>,
    /// Constant slots some instruction writes. The interpreter keeps a copy of
    /// the constants in the frame, and writes to a constant operand change that
    /// copy, so such slots live in frame memory.
    written_constants: &'a ZoneBitSet<'a>,
    /// Index of the instruction being built.
    instruction_index: usize,
    pc: u32,
    /// The frame state shared by every eager exit of the current instruction.
    eager_frame_state: Option<FrameStateId>,
    /// How this executable was called, if it is an inlined callee.
    inline: Option<inlining::InlineCall<'a>>,
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
    /// The state for a walk over executable `index` of `snapshot`, which `decoded` is the instructions of, with its
    /// data in `work`.
    fn new(
        snapshot: &'a Snapshot,
        index: u32,
        decoded: &'a DecodedFunction<'a>,
        has_legacy_arguments: bool,
        work: &'a Zone,
    ) -> Result<Self, CompileFailure> {
        let executable = &snapshot.executables[index as usize];
        let layout = executable.layout;
        let instructions = decoded.instructions;
        let analysis = decoded
            .analysis
            .get_or_init(|| {
                let handlers = &snapshot[executable.exception_handlers];
                let cfg =
                    Cfg::new(instructions, handlers, &layout, work).map_err(|_| CompileFailure::InvalidBytecode {
                        pc: 0,
                        reason: "invalid control flow",
                    })?;
                let loop_phi_slots = loop_phi_slots(&cfg, work);
                let loops_replacing_environments = loops_replacing_environments(&cfg, instructions, work);
                let written_constants = written_constants(instructions, &layout, work);
                let may_write_parameter_bindings = instructions
                    .iter()
                    .any(|decoded| arguments::may_write_parameter_bindings(decoded.instruction.opcode()));
                let mut writes_arguments = false;
                for decoded in instructions {
                    decoded.instruction.for_each_operand(|operand, role| {
                        writes_arguments |= role.is_written() && operand.raw() >= layout.arguments_base();
                    });
                }
                Ok(FunctionAnalysis {
                    cfg,
                    liveness: [OnceCell::new(), OnceCell::new()],
                    loop_phi_slots,
                    loops_replacing_environments,
                    written_constants,
                    writes_arguments,
                    may_write_parameter_bindings,
                })
            })
            .as_ref()
            .map_err(|error| *error)?;
        let liveness = analysis.liveness[usize::from(has_legacy_arguments)]
            .get_or_init(|| Liveness::compute(instructions, &analysis.cfg, &layout, has_legacy_arguments, work));
        Ok(Self {
            snapshot,
            index,
            executable,
            layout,
            instructions,
            cfg: &analysis.cfg,
            liveness,
            ir_blocks: ZoneVec::new_in(work),
            incoming: ZoneVec::new_in(work),
            started: ZoneVec::new_in(work),
            loop_header_frames: ZoneVec::new_in(work),
            loop_phi_slots: &analysis.loop_phi_slots,
            loops_replacing_environments: &analysis.loops_replacing_environments,
            osr_loop_headers: ZoneVec::new_in(work),
            written_constants: &analysis.written_constants,
            instruction_index: 0,
            pc: 0,
            eager_frame_state: None,
            inline: None,
            writes_arguments: analysis.writes_arguments,
            skipped_instructions: 0,
            may_write_parameter_bindings: analysis.may_write_parameter_bindings,
        })
    }

    /// Gives the lists of the walk an element for each bytecode block. A function that is not walked, like a callee
    /// that is not inlined after all, takes no memory for them.
    fn start_walk(&mut self) {
        let block_count = self.cfg.blocks.len();
        self.ir_blocks.resize(block_count, None);
        let work = self.ir_blocks.zone();
        self.incoming.resize(block_count, ZoneVec::new_in(work));
        self.started.resize(block_count, false);
        self.loop_header_frames.resize(block_count, None);
        self.osr_loop_headers.resize(block_count, false);
    }

    /// Whether this is the frame of an inlined call, whose slots are only
    /// SSA values until it is materialized.
    fn is_virtual(&self) -> bool {
        self.inline.is_some()
    }

    /// Whether this inlined call must preserve legacy `arguments`.
    fn has_legacy_arguments(&self) -> bool {
        self.inline.as_ref().is_some_and(|call| call.has_legacy_arguments)
    }
}

struct GraphBuilder<'a, 'z> {
    snapshot: &'a Snapshot,
    runtime: &'a RuntimeInfo,
    has_run: &'a dyn Fn(u32) -> bool,

    decoded: &'a [OnceCell<Result<DecodedFunction<'a>, CompileFailure>>],
    graph: Graph<'z>,
    /// Where the builder keeps its own data. Nothing of the graph is there.
    work: &'a Zone,
    /// The blocks in linear order, as they are started.
    order: ZoneVec<'a, BlockId>,
    /// The constant node of each value and representation.
    constants: ZoneMap<'a, (u64, Repr), NodeId>,

    block: BlockId,
    frame: AbstractFrame<'a>,
    /// The executable being walked.
    function: Function<'a>,
    /// The executables whose walks are suspended at an inlined call, outermost first.
    callers: ZoneVec<'a, Function<'a>>,
    /// Bytecode instructions inlined so far, for the inlining budget.
    inlined_instructions: usize,
    /// Where `CreateArguments` may build a virtual arguments object.
    virtualize_arguments: ArgumentsVirtualization<'a>,
    /// Whether to build on-stack replacement entries (see `arguments`).
    build_osr_entries: bool,
    /// The intrinsics among the constants, by value.
    intrinsics: ZoneMap<'a, u64, Intrinsic>,
    /// The functions among the constants that calls may inline, by value:
    /// their indices in `Snapshot::executables`.
    inlinable_functions: ZoneMap<'a, u64, u32>,
    /// The functions among the constants that calls may call directly, by
    /// value.
    direct_call_functions: ZoneMap<'a, u64, crate::snapshot::DirectCallTarget>,
    /// How `instanceof` runs with the functions the code uses as constants
    /// on its right-hand side (see `GlobalValueSnapshot::has_instance`), by
    /// their bits.
    has_instance: ZoneMap<'a, u64, crate::snapshot::OrdinaryHasInstanceSnapshot>,
    /// Sets of slots that are free to use again (see `live_in()`).
    free_slot_sets: ZoneVec<'a, ZoneBitSet<'a>>,
    /// Lists of slots that are free to use again (see `slot_list()`).
    free_slot_lists: ZoneVec<'a, ZoneVec<'a, usize>>,
    /// Lists of nodes that are free to use again (see `node_list()`).
    free_node_lists: ZoneVec<'a, ZoneVec<'a, NodeId>>,
}

impl<'a, 'z> GraphBuilder<'a, 'z> {
    fn new(
        snapshot: &'a Snapshot,
        has_run: &'a dyn Fn(u32) -> bool,
        decoded: &'a [OnceCell<Result<DecodedFunction<'a>, CompileFailure>>],
        virtualize_arguments: ArgumentsVirtualization<'a>,
        build_osr_entries: bool,
        zone: &'z Zone,
        work: &'a Zone,
    ) -> Result<Self, CompileFailure> {
        let executable = snapshot.executables.first().ok_or(CompileFailure::InvalidBytecode {
            pc: 0,
            reason: "no executable",
        })?;
        let decoded_root = decoded_function(snapshot, executable, &decoded[0], work)?;
        // NB: `GetArgument` reads the argument slots from frame memory, which only the slots it keeps live hold the
        //     current values of: a dead slot may be in sync with `undefined` instead (see `dead_frame()`). Inlined
        //     callees that read their passed arguments keep them live the same way (see `inlinable_callee()`).
        let reads_argument_slots = decoded_root
            .instructions
            .iter()
            .any(|instruction| instruction.instruction.opcode() == OpCode::GetArgument);
        let mut function = Function::new(snapshot, 0, decoded_root, reads_argument_slots, work)?;
        function.start_walk();
        let arguments_base = function.layout.arguments_base();
        let legacy_arguments = if executable.supports_legacy_arguments {
            arguments_base..arguments_base + function.layout.number_of_arguments
        } else {
            0..0
        };
        Ok(Self {
            snapshot,
            runtime: &snapshot.runtime,
            has_run,
            decoded,
            graph: Graph {
                legacy_arguments,
                ..Graph::new_in(zone)
            },
            work,
            order: ZoneVec::new_in(work),
            constants: ZoneMap::new_in(work),
            block: BlockId(0),
            // NB: Replaced by the frame of the first block built.
            frame: AbstractFrame::filled(0, NodeId(0), work),
            function,
            callers: ZoneVec::new_in(work),
            inlined_instructions: 0,
            virtualize_arguments,
            build_osr_entries,
            intrinsics: ZoneMap::new_in(work),
            inlinable_functions: ZoneMap::new_in(work),
            direct_call_functions: ZoneMap::new_in(work),
            has_instance: ZoneMap::new_in(work),
            free_slot_sets: ZoneVec::new_in(work),
            free_slot_lists: ZoneVec::new_in(work),
            free_node_lists: ZoneVec::new_in(work),
        })
    }

    fn build(mut self) -> Result<Graph<'z>, CompileFailure> {
        // NB: The entry block has no predecessors and jumps to the first bytecode
        //     block, which may be a loop header.
        let entry = self.start_new_block(&[]);
        // OPTIMIZATION: The code of top-level code that a loop triggered the compile of is only entered inside, so
        //               what only its start reaches is left out, like a large literal that ran before the loop.
        let builds_entry_at_osr_pc = self.snapshot.options.osr_pc.is_some_and(|pc| {
            let index = self
                .function
                .instructions
                .iter()
                .position(|instruction| instruction.pc == pc);
            index.is_some_and(|index| self.can_build_osr_entry(index))
        });
        if self.snapshot.options.runs_once && self.build_osr_entries && builds_entry_at_osr_pc {
            self.set_control(Op::Unreachable, &[]);
        } else {
            self.load_entry_slots();
            self.enter_function(entry);
        }
        if self.build_osr_entries {
            self.build_osr_entries()?;
            self.build_resume_entries()?;
            self.build_catch_entries()?;
        }
        self.build_function()?;
        let unwritten_slots = self.slots_kept_from_start();

        let mut graph = self.graph;
        let snapshot = self.snapshot;
        for (index, executable) in (0..).zip(snapshot.executables.iter()) {
            if executable.builtin {
                let sites = &snapshot[snapshot.executables[0].builtin_exit_sites];
                graph.exit_sites.extend(
                    sites
                        .iter()
                        .filter(|(cell, _, _)| *cell == executable.cell)
                        .map(|(_, pc, kind)| (index, *pc, *kind)),
                );
            } else {
                let sites = &snapshot[executable.exit_sites];
                graph
                    .exit_sites
                    .extend(sites.iter().map(|(pc, kind)| (index, *pc, *kind)));
            }
        }
        assert_eq!(graph.blocks.len(), self.order.len(), "every created block was started");
        graph.reorder_blocks(&self.order, self.work);
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
        graph.unwritten_slots = unwritten_slots;
        Ok(graph)
    }

    /// The slots (as operands) that hold the values the compiled function starts with wherever it runs: the arguments
    /// and the this value register, unless instructions of the function write them. The frame's initialization
    /// changes the other registers and the locals, and the runtime the other reserved registers.
    fn slots_kept_from_start(&self) -> ZoneBitSet<'z> {
        let layout = self.function.layout;
        let mut kept = ZoneBitSet::new_in(
            layout.arguments_base() as usize + layout.number_of_arguments as usize,
            self.graph.zone,
        );
        kept.insert_range(layout.arguments_base() as usize..kept.capacity());
        if THIS_VALUE_REGISTER < layout.registers_and_locals_count {
            kept.insert(THIS_VALUE_REGISTER as usize);
        }
        for decoded in self.function.instructions {
            if decoded.instruction.opcode() == OpCode::ResolveThisBinding {
                kept.remove(THIS_VALUE_REGISTER as usize);
            }
            decoded.instruction.for_each_operand(|operand, role| {
                if role.is_written() && (operand.raw() as usize) < kept.capacity() {
                    kept.remove(operand.raw() as usize);
                }
            });
        }
        kept
    }

    /// Makes the abstract frame the one the compiled function starts with, like the other entries' (see `osr`): the
    /// slots live there are loaded from frame memory (loads nothing uses go away in dead code elimination).
    fn load_entry_slots(&mut self) {
        self.frame = self.dead_frame();
        let live = self.live_in(0);
        for slot in live.iter() {
            self.load_slot(slot);
        }
        self.release_slot_set(live);
    }

    /// A frame whose slots are all dead: they hold `undefined` (see `AbstractFrame::slots`).
    fn dead_frame(&mut self) -> AbstractFrame<'a> {
        let undefined = self.constant(value::UNDEFINED);
        AbstractFrame::filled(self.function.layout.tracked_slot_count(), undefined, self.work)
    }

    /// The slots live before the instruction with the given index of the function being walked executes, in a set to
    /// give back with `release_slot_set()` once it is not needed any more.
    fn live_in(&mut self, instruction_index: usize) -> ZoneBitSet<'a> {
        let mut live = self.slot_set();
        self.function.liveness.live_in(instruction_index, &mut live);
        live
    }

    /// Like `live_in()`, for the slots live after the instruction.
    fn live_out(&mut self, instruction_index: usize) -> ZoneBitSet<'a> {
        let mut live = self.slot_set();
        self.function.liveness.live_out(instruction_index, &mut live);
        live
    }

    fn slot_set(&mut self) -> ZoneBitSet<'a> {
        self.free_slot_sets
            .pop()
            .unwrap_or_else(|| ZoneBitSet::new_in(0, self.work))
    }

    /// Gives back a set that `live_in()` or `live_out()` gave, for them to use again.
    fn release_slot_set(&mut self, set: ZoneBitSet<'a>) {
        self.free_slot_sets.push(set);
    }

    /// An empty list of slots, to give back with `release_slot_list()` once it is not needed any more.
    fn slot_list(&mut self) -> ZoneVec<'a, usize> {
        let mut list = self.free_slot_lists.pop().unwrap_or_else(|| ZoneVec::new_in(self.work));
        list.clear();
        list
    }

    /// Gives back a list that `slot_list()` gave, for it to use again.
    fn release_slot_list(&mut self, list: ZoneVec<'a, usize>) {
        self.free_slot_lists.push(list);
    }

    /// An empty list of nodes, to give back with `release_node_list()` once it is not needed any more.
    fn node_list(&mut self) -> ZoneVec<'a, NodeId> {
        let mut list = self.free_node_lists.pop().unwrap_or_else(|| ZoneVec::new_in(self.work));
        list.clear();
        list
    }

    /// Gives back a list that `node_list()` gave, for it to use again.
    fn release_node_list(&mut self, list: ZoneVec<'a, NodeId>) {
        self.free_node_lists.push(list);
    }

    /// Loads a slot from frame memory, which then holds its value in sync.
    fn load_slot(&mut self, slot: usize) -> NodeId {
        let operand = self.function.layout.operand_for_tracked_index(slot);
        let node = self.emit(Op::LoadSlot { slot: operand.raw() }, &[], Some(Repr::Tagged));
        self.frame.slots[slot] = SlotState { node, in_sync: true };
        node
    }

    /// Ends the current block with a jump to the first bytecode block of the
    /// function being walked, with the current abstract frame.
    fn enter_function(&mut self, from: BlockId) {
        let first_block = self.ir_block(0);
        self.function.incoming[0].push((from, self.frame.clone()));
        self.block = from;
        self.set_control(Op::Jump { target: first_block }, &[]);
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
        let block = self.graph.add_block(self.graph.new_block());
        self.function.ir_blocks[block_index] = Some(block);
        block
    }

    /// Creates a block, appends it to the linear order and makes it current.
    /// The caller sets the abstract frame.
    fn start_new_block(&mut self, predecessors: &[BlockId]) -> BlockId {
        let block = self.graph.add_block(Block {
            predecessors: self.graph.list(predecessors),
            ..self.graph.new_block()
        });
        self.order.push(block);
        self.block = block;
        block
    }

    /// Starts the IR block of a bytecode block, merging the abstract frames of
    /// its forward predecessors. Returns false if no built block reaches it.
    fn start_bytecode_block(&mut self, block_index: BlockIndex) -> bool {
        let incoming = self.function.incoming[block_index].take();
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
            block.predecessors.clear();
            block
                .predecessors
                .extend(incoming.iter().map(|(predecessor, _)| *predecessor));
            block.bytecode_start = Some(self.function.instructions[first_instruction].pc);
            block.is_loop_header = is_loop_header;
        }
        self.function.instruction_index = first_instruction;
        self.function.pc = self.function.instructions[first_instruction].pc;

        // NB: A loop header that only back edges reach is a merge without forward predecessors.
        self.frame = if incoming.len() == 1 && !is_loop_header {
            incoming.into_iter().next().unwrap().1
        } else {
            self.merge(block_index, incoming)
        };
        if is_loop_header {
            self.function.loop_header_frames[block_index] = Some(self.frame.clone());
        }
        true
    }

    /// The abstract frame at the start of a merge or loop header, given the abstract frames at the end of its forward
    /// predecessors: each live slot holds the value they all bring, or a phi of their values where they differ. A loop
    /// header has a phi for each live slot the loop assigns, whose back edges add their inputs (see
    /// `reconcile_back_edge()`). So does a loop header that on-stack replacement entries reach over back edges, for
    /// every live slot: the values there come from different entries.
    fn merge(
        &mut self,
        block_index: BlockIndex,
        incoming: ZoneVec<'a, (BlockId, AbstractFrame<'a>)>,
    ) -> AbstractFrame<'a> {
        let cfg = self.function.cfg;
        let bytecode_block = &cfg.blocks[block_index];
        let live = self.live_in(bytecode_block.instructions.start);
        let loop_phi_slots = self.function.loop_phi_slots;
        let assigned_in_loop = bytecode_block.loop_index.map(|loop_index| &loop_phi_slots[loop_index]);
        let is_loop_header = bytecode_block.is_loop_header();

        let mut merged = self.dead_frame();
        // NB: A field keeps its value where every path brings the same one,
        //     except environments at the headers of loops that may replace
        //     them, and every field at the headers of loops that on-stack
        //     replacement entries reach over back edges, which bring none.
        let replaces_environments_in_loop = is_loop_header
            && bytecode_block
                .loop_index
                .is_some_and(|loop_index| self.function.loops_replacing_environments[loop_index]);
        let osr_loop_header = self.function.osr_loop_headers[block_index];
        // NB: Loops may change the shapes their headers would know.
        if !is_loop_header {
            merged.set_common_fresh_shapes(incoming.iter().map(|(_, frame)| frame.fresh_shapes.as_slice()));
        }
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
        let mut values = ZoneVec::with_capacity_in(incoming.len(), self.work);
        let mut inputs = ZoneVec::with_capacity_in(incoming.len(), self.work);
        for slot in live.iter() {
            let assigned = assigned_in_loop.is_some_and(|assigned| assigned.contains(slot));
            values.clear();
            values.extend(incoming.iter().map(|(_, frame)| frame.slots[slot]));
            // NB: Back edges keep frame memory in sync with the values of the slots the loop does not assign, and
            //     the phis of the others get values memory need not hold.
            let in_sync = values.iter().all(|value| value.in_sync) && !(is_loop_header && assigned);
            let needs_phi = is_loop_header && (assigned || osr_loop_header);
            let shared = values
                .iter()
                .map(|value| value.node)
                .find(|candidate| values.iter().all(|value| self.graph.refines(value.node, *candidate)))
                .filter(|_| !needs_phi);
            // OPTIMIZATION: Where every path brings a value it loaded from the slot, which frame memory still holds,
            //               and the loop does not assign it, the merge loads the slot once more instead of taking a phi
            //               of those loads, like the merge of the start of the function and its on-stack replacement
            //               entries ahead of a loop. So does a loop header that only back edges reach, which keep
            //               frame memory in sync with the slots the loop does not assign. Loop invariant code motion
            //               takes these loads out of their loops.
            let operand = self.function.layout.operand_for_tracked_index(slot);
            let reloads = shared.is_none()
                && self.callers.is_empty()
                && !(is_loop_header && assigned)
                && values.iter().all(|value| {
                    value.in_sync
                        && matches!(self.graph.node(self.graph.unrefined(value.node)).op,
                            Op::LoadSlot { slot } if slot == operand.raw())
                });
            let node = match shared {
                Some(node) => node,
                None if reloads => self.emit(Op::LoadSlot { slot: operand.raw() }, &[], Some(Repr::Tagged)),
                None => {
                    // NB: Phis start out tagged; representation selection unboxes them later.
                    inputs.clear();
                    for ((predecessor, _), value) in incoming.iter().zip(&values) {
                        let input = self.tagged_at_end_of(*predecessor, value.node);
                        inputs.push(input);
                    }
                    self.add_phi(&inputs)
                }
            };
            merged.slots[slot] = SlotState { node, in_sync };
        }
        self.release_slot_set(live);
        merged
    }

    fn add_phi(&mut self, inputs: &[NodeId]) -> NodeId {
        let phi = self.graph.add(Op::Phi, inputs, Some(Repr::Tagged), self.function.pc);
        self.graph.blocks[self.block.index()].phis.push(phi);
        phi
    }

    /// Adds a node to the body of a block whose control node is already set.
    fn append_to_finished_block(
        &mut self,
        block: BlockId,
        op: Op<'z>,
        inputs: &[NodeId],
        repr: Option<Repr>,
    ) -> NodeId {
        let pc = self.graph.control(block).pc;
        let node = self.graph.add(op, inputs, repr, pc);
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
            if instruction_index == instructions.start && is_merge {
                let frame_state = self.eager_frame_state();
                self.graph.blocks[self.block.index()].start_frame_state = Some(frame_state);
            }
            if self.function.skipped_instructions > 0 {
                self.function.skipped_instructions -= 1;
                continue;
            }
            let first_node = self.graph.nodes.len();
            if let Flow::Ended = self.build_instruction()? {
                return Ok(());
            }
            self.guard_legacy_arguments(instruction_index, first_node);
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
        self.set_control(Op::Jump { target: target_block }, &[]);
        Ok(())
    }

    /// Ends the current block with a branch to two bytecode blocks, through
    /// a fresh block for each successor.
    fn end_with_branch(
        &mut self,
        make_op: impl FnOnce(BlockId, BlockId) -> Op<'z>,
        inputs: &[NodeId],
        if_true: BlockIndex,
        if_false: BlockIndex,
    ) -> Result<(), CompileFailure> {
        let source = self.block;
        let frame = self.frame.clone();
        let true_edge = self.graph.add_block(Block {
            predecessors: self.graph.list(&[source]),
            ..self.graph.new_block()
        });
        let false_edge = self.graph.add_block(Block {
            predecessors: self.graph.list(&[source]),
            ..self.graph.new_block()
        });
        self.order.push(true_edge);
        self.order.push(false_edge);
        self.set_control(make_op(true_edge, false_edge), inputs);
        for (edge, target) in [(true_edge, if_true), (false_edge, if_false)] {
            self.block = edge;
            self.frame.clone_from(&frame);
            self.end_with_jump(target)?;
        }
        Ok(())
    }

    /// Makes the abstract frame at the end of the current block match the
    /// one at the start of the loop header `target`, and adds the inputs of
    /// the header's phis for this back edge.
    fn reconcile_back_edge(&mut self, target: BlockIndex) -> Result<(), CompileFailure> {
        // NB: The frame goes back to its place below.
        let header_frame = self.function.loop_header_frames[target]
            .take()
            .expect("the loop header was started");
        let live = self.live_in(self.function.cfg.blocks[target].instructions.start);
        let result = self.reconcile_back_edge_with(target, &header_frame, &live);
        self.release_slot_set(live);
        self.function.loop_header_frames[target] = Some(header_frame);
        result
    }

    /// `reconcile_back_edge()` with the header's abstract frame `header_frame` and the slots `live` into it.
    fn reconcile_back_edge_with(
        &mut self,
        target: BlockIndex,
        header_frame: &AbstractFrame<'a>,
        live: &ZoneBitSet<'a>,
    ) -> Result<(), CompileFailure> {
        let header = self.function.ir_blocks[target].expect("started loop headers have an IR block");
        for slot in live.iter() {
            let header_value = header_frame.slots[slot];
            let current = self.frame.slots[slot];
            if self.graph.block(header).phis.contains(&header_value.node) {
                let value = self.tagged(current.node);
                self.graph.nodes[header_value.node.index()].inputs.push(value);
                continue;
            }
            // NB: Only slots assigned in the loop can change, and those have phis. A load at the header (see
            //     `merge()`) reads frame memory, which keeps the value of a slot the loop does not assign, again on
            //     every iteration, also where on-stack replacement entries reach the header over back edges.
            let reloaded = self.graph.block(header).body.contains(&header_value.node)
                && matches!(self.graph.node(header_value.node).op, Op::LoadSlot { .. });
            if !reloaded && !self.graph.has_value_of(current.node, header_value.node, self.work) {
                return Err(CompileFailure::InvalidBytecode {
                    pc: self.function.pc,
                    reason: "loop back edge changes a value the loop does not assign",
                });
            }
        }
        // NB: Frame memory keeps the values of the slots the loop does not assign (see `merge()`).
        if live
            .iter()
            .any(|slot| header_frame.slots[slot].in_sync && !self.frame.slots[slot].in_sync)
        {
            return Err(CompileFailure::InvalidBytecode {
                pc: self.function.pc,
                reason: "loop back edge leaves a slot out of sync that the header has in frame memory",
            });
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

    fn emit(&mut self, op: Op<'z>, inputs: &[NodeId], repr: Option<Repr>) -> NodeId {
        debug_assert!(!op.is_control());
        let node = self.graph.add(op, inputs, repr, self.function.pc);
        self.graph.blocks[self.block.index()].body.push(node);
        self.note_shape_changes(node);
        node
    }

    /// Updates the shapes the frame knows objects have (see
    /// `AbstractFrame::fresh_shapes`) for the effects of `node`.
    fn note_shape_changes(&mut self, node: NodeId) {
        let node_data = self.graph.node(node);
        match node_data.op {
            Op::AllocateObject { shape, .. } => {
                // NB: Only the latest allocations are worth knowing, and the
                //     frames that copy their shapes stay small.
                if self.frame.fresh_shapes.len() == MAX_FRESH_SHAPES {
                    self.frame.fresh_shapes.remove(0);
                }
                self.frame.fresh_shapes.push((node, shape));
            }
            Op::AddNamed { shape, .. } => {
                let object = self.graph.unrefined(node_data.inputs[0]);
                self.frame.fresh_shapes.retain(|(known, _)| *known != object);
                self.frame.fresh_shapes.push((object, shape));
            }
            ref op => {
                let properties = op.properties();
                if properties.runs_code || properties.writes.intersects(Locations::SHAPES) {
                    self.frame.fresh_shapes.clear();
                }
            }
        }
    }

    /// The shape the frame knows `object` has, if it is an object the code
    /// allocated.
    fn fresh_shape(&self, object: NodeId) -> Option<CellId> {
        let object = self.graph.unrefined(object);
        self.frame
            .fresh_shapes
            .iter()
            .find(|(known, _)| *known == object)
            .map(|(_, shape)| *shape)
    }

    fn set_control(&mut self, op: Op<'z>, inputs: &[NodeId]) -> NodeId {
        debug_assert!(op.is_control());
        let node = self.graph.add(op, inputs, None, self.function.pc);
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
        let node = self.graph.add(Op::Constant(bits), &[], Some(repr), 0);
        self.constants.insert((bits, repr), node);
        node
    }

    /// The `CellAddress` of the cell `value`, a constant for constants.
    fn cell_address(&mut self, value: NodeId) -> NodeId {
        match self.graph.constant_value(value) {
            Some(bits) => {
                let address = self.runtime.heap_region_base + (bits & self.runtime.heap_region_offset_mask);
                // NB: Only checks that always exit see constants that hold no cell.
                let pattern = self.runtime.shifted_is_cell_pattern;
                if bits & pattern == pattern {
                    self.embed(CellId(address));
                }
                self.typed_constant(address, Repr::Pointer)
            }
            None => self.emit(Op::CellAddress, &[value], Some(Repr::Pointer)),
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
            return self.emit(Op::BoxInt32, &[value], Some(Repr::Tagged));
        }
        self.append_to_finished_block(block, Op::BoxInt32, &[value], Some(Repr::Tagged))
    }

    fn constant(&mut self, bits: u64) -> NodeId {
        self.typed_constant(bits, Repr::Tagged)
    }

    // Frame slots.

    /// What the frame holds for an operand, if it is a tracked slot (not a constant).
    fn peek(&self, operand: Operand) -> Option<SlotState> {
        let slot = self.function.layout.tracked_index(operand)?;
        Some(self.frame.slots[slot])
    }

    /// The intrinsic function an operand holds as a constant, if any.
    fn constant_intrinsic(&self, operand: Operand) -> Option<Intrinsic> {
        let bits = self.graph.constant_value(self.peek(operand)?.node)?;
        self.intrinsics.get(&bits).copied()
    }

    /// The snapshot executable of the function an operand holds as a
    /// constant, if calls of it may be inlined.
    fn constant_inlinable_function(&self, operand: Operand) -> Option<u32> {
        let bits = self.graph.constant_value(self.peek(operand)?.node)?;
        self.inlinable_functions.get(&bits).copied()
    }

    /// The feedback a call instruction recorded in its feedback slot `slot`.
    fn call_feedback(&self, slot: u16) -> Option<CallFeedbackSnapshot> {
        self.snapshot[self.function.executable.feedback.call]
            .get(usize::from(slot))
            .copied()
    }

    /// Whether the instruction at `pc` of the function being walked ever ran.
    fn instruction_has_run(&self, pc: u32) -> bool {
        if self.function.index == 0 {
            (self.has_run)(pc)
        } else {
            has_run(self.snapshot, self.function.executable, pc)
        }
    }

    /// The tagged value of an operand.
    fn read(&mut self, operand: Operand) -> Result<NodeId, CompileFailure> {
        let value = self.read_value(operand)?;
        Ok(self.tagged(value))
    }

    /// The SSA value of an operand in whatever representation the frame
    /// holds it.
    fn read_value(&mut self, operand: Operand) -> Result<NodeId, CompileFailure> {
        if operand.raw() >= self.function.layout.arguments_base() + self.function.layout.number_of_arguments {
            return Err(CompileFailure::InvalidBytecode {
                pc: self.function.pc,
                reason: "read of an operand past the frame",
            });
        }
        match self.function.layout.tracked_index(operand) {
            Some(slot) => Ok(self.frame.slots[slot].node),
            None => {
                let index = (operand.raw() - self.function.layout.constants_base()) as usize;
                if index < self.function.written_constants.capacity() && self.function.written_constants.contains(index)
                {
                    return Ok(self.emit(Op::LoadSlot { slot: operand.raw() }, &[], Some(Repr::Tagged)));
                }
                let bits = *self.snapshot[self.function.executable.constants].get(index).ok_or(
                    CompileFailure::InvalidBytecode {
                        pc: self.function.pc,
                        reason: "read of a constant that does not exist",
                    },
                )?;
                Ok(self.constant(bits))
            }
        }
    }

    fn write(&mut self, operand: Operand, value: NodeId) -> Result<(), CompileFailure> {
        if operand.raw() >= self.function.layout.arguments_base() + self.function.layout.number_of_arguments {
            return Err(CompileFailure::InvalidBytecode {
                pc: self.function.pc,
                reason: "write to an operand past the frame",
            });
        }
        let Some(slot) = self.function.layout.tracked_index(operand) else {
            // NB: A written constant lives in frame memory.
            self.emit(Op::StoreSlot { slot: operand.raw() }, &[value], None);
            return Ok(());
        };
        // NB: Inlined frames have no memory to be out of sync with.
        let is_virtual = self.function.is_virtual();
        let writes_memory = !is_virtual && self.graph.legacy_arguments.contains(&operand.raw());
        // NB: Frame memory need not hold what a dead slot holds (see `AbstractFrame::slots`).
        let unchanged = self.frame.slots[slot]
            == (SlotState {
                node: value,
                in_sync: true,
            })
            && {
                let live = self.live_in(self.function.instruction_index);
                let live_slot = live.contains(slot);
                self.release_slot_set(live);
                live_slot
            };
        if !unchanged {
            if writes_memory {
                self.emit(Op::StoreSlot { slot: operand.raw() }, &[value], None);
            }
            self.frame.slots[slot] = SlotState {
                node: value,
                in_sync: is_virtual || writes_memory,
            };
        }
        Ok(())
    }

    /// Writes a slot's value back to frame memory if it is out of sync.
    fn sync_slot(&mut self, slot: usize) {
        let SlotState { node, in_sync } = self.frame.slots[slot];
        if !in_sync {
            let operand = self.function.layout.operand_for_tracked_index(slot);
            self.emit(Op::StoreSlot { slot: operand.raw() }, &[node], None);
            self.frame.slots[slot].in_sync = true;
        }
    }

    /// The frame state for eager exits of the current instruction: resume
    /// at it, with the values of every slot live into it.
    fn eager_frame_state(&mut self) -> FrameStateId {
        if let Some(frame_state) = self.function.eager_frame_state {
            return frame_state;
        }
        let live = self.live_in(self.function.instruction_index);
        let frame_state = self.new_frame_state(&live, ResumeMode::ResumeAt);
        self.release_slot_set(live);
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
                &[],
            );
            self.graph.nodes[exit.index()].frame_state = Some(frame_state);
            return Ok(Flow::Ended);
        }

        let instructions = self.function.instructions;
        let instruction = &instructions[self.function.instruction_index].instruction;
        if osr::is_loop_back_edge(instruction.opcode()) {
            self.emit_checked(Op::CheckInterrupt, &[], None);
        }
        if replaces_environments(instruction.opcode()) {
            self.frame.forget_environments();
        }
        match self.try_build_natively(instruction)? {
            Some(flow) => Ok(flow),
            None => self.build_by_handling(instruction),
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
                    self.emit(Op::InitializeFrame, &[], None);
                }
                let empty = self.constant(value::EMPTY);
                for slot in RESERVED_REGISTER_COUNT as usize..self.function.layout.registers_and_locals_count as usize {
                    self.frame.slots[slot] = SlotState {
                        node: empty,
                        in_sync: true,
                    };
                }
            }
            Instruction::Mov { dst, src } => {
                let value = self.read(src)?;
                self.write(dst, value)?;
            }
            // NB: Only what decodes the next instruction reads its operands.
            Instruction::WideNameIndices { .. } => {}
            Instruction::MovSrcUndefined { dst } => {
                let undefined = self.constant(value::UNDEFINED);
                self.write(dst, undefined)?;
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
                self.check_not_returning_virtual_arguments(value)?;
                if self.function.is_virtual() {
                    return Ok(Some(self.build_inline_return(value, true)?));
                }
                // NB: An empty value returns `undefined`, but a value that is
                //     never empty needs no check for it.
                let layout = self.function.layout;
                let arguments = layout.arguments_base()..layout.arguments_base() + layout.number_of_arguments;
                let value = if self.graph.is_known_non_empty(value, arguments, self.work) {
                    value
                } else {
                    self.emit(Op::EmptyToUndefined, &[value], Some(Repr::Tagged))
                };
                self.set_control(Op::Return, &[value]);
                return Ok(Some(Flow::Ended));
            }
            Instruction::End { value } => {
                let value = self.read(value)?;
                self.check_not_returning_virtual_arguments(value)?;
                // NB: In an inlined callee, End returns its value (never
                //     empty) to the caller like Return does.
                if self.function.is_virtual() {
                    return Ok(Some(self.build_inline_return(value, false)?));
                }
                self.set_control(Op::Return, &[value]);
                return Ok(Some(Flow::Ended));
            }
            Instruction::Yield {
                continuation_label,
                value,
            } => {
                return self
                    .build_suspend(continuation_label, value, SuspendKind::Yield)
                    .map(Some);
            }
            Instruction::YieldIteratorResult {
                continuation_label,
                value,
            } => {
                return self
                    .build_suspend(Some(continuation_label), value, SuspendKind::YieldIteratorResult)
                    .map(Some);
            }
            Instruction::Await {
                continuation_label,
                argument,
            } => {
                return self
                    .build_suspend(Some(continuation_label), argument, SuspendKind::Await)
                    .map(Some);
            }
            Instruction::GetLexicalEnvironment { dst } => {
                let address = self.frame_field(FrameField::LexicalEnvironment)?;
                let environment = self.emit(Op::BoxCell, &[address], Some(Repr::Tagged));
                self.write(dst, environment)?;
            }
            Instruction::SetLexicalEnvironment { environment } => {
                let environment = self.read(environment)?;
                let address = self.emit(Op::SetLexicalEnvironment, &[environment], Some(Repr::Pointer));
                self.frame.set_field(FrameField::LexicalEnvironment, address);
            }
            Instruction::LeavePrivateEnvironment => {
                self.emit(Op::LeavePrivateEnvironment, &[], None);
            }
            Instruction::IsCallable { dst, value } => {
                let value = self.read(value)?;
                let result = self.emit(Op::IsCallable, &[value], Some(Repr::Tagged));
                self.write(dst, result)?;
            }
            Instruction::GetArgumentCount { dst } => {
                // NB: An inlined callee was passed as many arguments as its call passed.
                let count = match &self.function.inline {
                    Some(call) => self.constant(value::int32(call.passed_argument_count.cast_signed())),
                    None => self.emit(Op::ArgumentCount, &[], Some(Repr::Tagged)),
                };
                self.write(dst, count)?;
            }
            Instruction::CreateArguments { .. } => {
                if !self.try_build_virtual_arguments(instruction)? {
                    return Ok(None);
                }
            }
            Instruction::GetArgument { dst, index, .. } => {
                if !self.try_build_argument(dst, index)? {
                    return Ok(None);
                }
            }
            Instruction::NewArray { dst, elements, .. } => {
                if elements.is_empty()
                    && let Some(flow) = self.try_build_spread_forwarding(dst)?
                {
                    return Ok(Some(flow));
                }
                let elements = ZoneVec::from_iter_in(
                    elements.iter().copied().map(allocation::ArrayElement::Operand),
                    self.work,
                );
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
                if !self.try_build_new_function(dst, shared_function_data_index.into(), home_object.is_some())? {
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
            Instruction::NewPrimitiveArray { dst, elements, .. } => {
                let elements = ZoneVec::from_iter_in(
                    elements.iter().copied().map(allocation::ArrayElement::Constant),
                    self.work,
                );
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
                if !self.try_build_init_object_literal_property(object, src, shape_cache_index, property_slot.into())? {
                    return Ok(None);
                }
            }
            Instruction::GetNewTarget { dst } => {
                if !self.try_build_new_target(dst)? {
                    return Ok(None);
                }
            }
            Instruction::ResolveSuperBase { dst } => {
                if !self.try_build_resolve_super_base(dst)? {
                    return Ok(None);
                }
            }
            Instruction::GetSuperConstructor { dst } => {
                if !self.try_build_super_constructor(dst)? {
                    return Ok(None);
                }
            }
            Instruction::BindThisFromSuperCall { result } => {
                if !self.try_build_bind_this_defining_fields(result)? {
                    if let Some(flow) = self.try_build_bind_this_initializing_instance_elements(instruction, result)? {
                        return Ok(Some(flow));
                    }
                    if !self.try_build_bind_this_from_super_call(result)? {
                        return Ok(None);
                    }
                }
            }
            Instruction::SuperCall { .. } | Instruction::SuperCallWithForwardedArguments { .. } => {
                return self.try_inline_super_call(instruction);
            }
            Instruction::CacheObjectShape { object, cache } => {
                if !self.try_build_cache_object_shape(object, cache)? {
                    return Ok(None);
                }
            }
            Instruction::GetLength { dst, base, cache, .. } => {
                // NB: Lengths of arrays never reach the cache, so where the site saw arrays, its cache does not show
                //     all receivers.
                let saw_arrays = self
                    .keyed_feedback_bits(instruction)
                    .is_some_and(|bits| bits & crate::snapshot::keyed_feedback_bits::MAGICAL_LENGTH != 0);
                if !self.try_build_arguments_length(dst, base)?
                    && !self.try_build_array_length(instruction, dst, base)?
                    && !self.try_build_typed_array_length(dst, base, cache)?
                    && (saw_arrays || !self.try_build_native_getter(dst, base, cache)?)
                {
                    return Ok(None);
                }
            }
            Instruction::CallConstruct { .. } => {
                if let Some(flow) = self.try_build_array_construct(instruction)? {
                    return Ok(Some(flow));
                }
                return self.try_inline_construct(instruction);
            }
            Instruction::Call { .. } => {
                if let Some(flow) = self.try_build_builtin_call(instruction)? {
                    return Ok(Some(flow));
                }
                if let Some(flow) = self.try_build_array_push(instruction)? {
                    return Ok(Some(flow));
                }
                if let Some(flow) = self.try_build_array_splice(instruction)? {
                    return Ok(Some(flow));
                }
                if let Some(flow) = self.try_build_math_min_max(instruction)? {
                    return Ok(Some(flow));
                }
                if let Some(flow) = self.try_build_math_imul(instruction)? {
                    return Ok(Some(flow));
                }
                if let Some(flow) = self.try_build_has_own_property(instruction)? {
                    return Ok(Some(flow));
                }
                if let Some(flow) = self.try_build_builtin_constructor_call(instruction)? {
                    return Ok(Some(flow));
                }
                if let Some(flow) = self.try_build_helper_call(instruction)? {
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
            Instruction::CreateDataPropertyOrThrow {
                object,
                property,
                value,
            } => return self.build_create_data_property(object, property, value),
            Instruction::In { dst, lhs, rhs } => return self.try_build_in(dst, lhs, rhs),
            Instruction::Typeof { dst, src } if self.runtime.layout.typeof_strings.number != 0 => {
                let value = self.read(src)?;
                self.assume_no_htmldda_objects();
                let result = self.emit(Op::Typeof, &[value], Some(Repr::Tagged));
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
                let result = self.emit(Op::Typeof, &[value], Some(Repr::Tagged));
                self.write(dst, result)?;
            }
            Instruction::SetGlobal { src, cache, .. } => {
                if !self.try_build_set_global(src, cache)? {
                    return Ok(None);
                }
            }
            Instruction::ResolveGlobalBinding { dst, cache, .. } => {
                if !self.try_build_resolve_global_binding(dst, cache)? {
                    return Ok(None);
                }
            }
            Instruction::GetById { dst, base, cache, .. } => {
                if self.try_build_cached_getter(dst, base, cache)? {
                    return Ok(Some(Flow::Continue));
                }
                if let Some(flow) = self.try_inline_getter(dst, base, cache)? {
                    return Ok(Some(flow));
                }
                if self.try_build_native_getter(dst, base, cache)? {
                    return Ok(Some(Flow::Continue));
                }
                if !self.try_build_get_by_id(dst, base, cache)? {
                    return Ok(None);
                }
            }
            // NB: Only data properties, whose value does not depend on the receiver, are loaded here. Getters run in
            //     the slow path with the this value.
            Instruction::GetByIdWithThis { dst, base, cache, .. } => {
                if !self.try_build_get_by_id(dst, base, cache)? {
                    return Ok(None);
                }
            }
            Instruction::PutById { base, src, cache, .. } => {
                if let Some(flow) = self.try_inline_setter(base, src, cache)? {
                    return Ok(Some(flow));
                }
                if self.try_build_native_setter(base, src, cache)? {
                    return Ok(Some(Flow::Continue));
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

    /// Builds a `Yield`, `YieldIteratorResult` or `Await`, which suspends
    /// the generator or async function running the compiled function and
    /// returns `value` to the code resuming it: puts the slots live at the
    /// continuation in frame memory, where the resume entry of the
    /// continuation (see `osr`) finds them, and records the continuation in
    /// the frame like the interpreter's instruction.
    fn build_suspend(
        &mut self,
        continuation: Option<Label>,
        value: Operand,
        kind: SuspendKind,
    ) -> Result<Flow, CompileFailure> {
        if self.function.is_virtual() {
            return Err(CompileFailure::InvalidBytecode {
                pc: self.function.pc,
                reason: "suspension in an inlined callee",
            });
        }
        let value = self.read(value)?;
        self.check_not_returning_virtual_arguments(value)?;
        if let Some(continuation) = continuation {
            let block = self.block_for_label(continuation)?;
            let start = self.function.cfg.blocks[block].instructions.start;
            let live = self.live_in(start);
            for slot in live.iter() {
                self.sync_slot(slot);
            }
            self.release_slot_set(live);
        }
        // NB: Like the interpreter's Yield and Await, an empty value yields
        //     `undefined`.
        let layout = self.function.layout;
        let arguments = layout.arguments_base()..layout.arguments_base() + layout.number_of_arguments;
        let value = if self.graph.is_known_non_empty(value, arguments, self.work) {
            value
        } else {
            self.emit(Op::EmptyToUndefined, &[value], Some(Repr::Tagged))
        };
        self.emit(
            Op::Suspend {
                continuation: continuation.map(|label| label.0),
                kind,
            },
            &[],
            None,
        );
        self.set_control(Op::Return, &[value]);
        Ok(Flow::Ended)
    }

    /// Builds `GetCompletionFields` (`Handling::Expanded`), which a
    /// resumed generator or async function starts with: the type and the
    /// value of the completion it resumed with, which the generator object
    /// it finds in its accumulator holds, and the slow path for other
    /// completion sources (async generators).
    pub(super) fn build_get_completion_fields(
        &mut self,
        type_dst: Operand,
        value_dst: Operand,
        completion: Operand,
    ) -> Result<Flow, CompileFailure> {
        let source = self.read(completion)?;
        let mut slow_paths = self.start_slow_paths(Some(Repr::Tagged))?;
        let object = self.branch_to_slow_path(&mut slow_paths, BranchCondition::Object, &[source], true);
        let object = self.cell_address(object);
        let generator = self.branch_to_slow_path(&mut slow_paths, BranchCondition::GeneratorObject, &[object], true);
        let completion_type = self.emit(Op::LoadGeneratorCompletionType, &[generator], Some(Repr::Int32));
        let completion_type = self.emit(Op::BoxInt32, &[completion_type], Some(Repr::Tagged));
        let value = self.emit(Op::LoadGeneratorCompletionValue, &[generator], Some(Repr::Tagged));
        let outputs = self.join_paths(slow_paths, &[completion_type, value], |_, _| {});
        self.write(type_dst, outputs[0])?;
        self.write(value_dst, outputs[1])?;
        Ok(Flow::Continue)
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
            &[value],
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
                predecessors: self.graph.list(&[source]),
                ..self.graph.new_block()
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
            &[value],
        );
        for (edge, target) in [(true_edge, if_true), (false_edge, if_false)] {
            self.block = edge;
            self.frame.clone_from(&frame);
            self.end_with_jump(target)?;
        }
        self.block = fallback;
        self.frame = frame;
        let boolean = self.emit(Op::ToBoolean, &[value], Some(Repr::Bool));
        self.end_with_branch(
            |if_true, if_false| Op::Branch {
                condition: BranchCondition::Bool,
                if_true,
                if_false,
            },
            &[boolean],
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
            &[value],
        );

        self.block = fallback;
        let boolean = self.emit(Op::ToBoolean, &[value], Some(Repr::Bool));
        let (fallback_true_block, fallback_false_block) = self.branch_both_ways(BranchCondition::Bool, &[boolean]);
        self.join_blocks(&[true_block, false_block, fallback_true_block, fallback_false_block]);
        self.frame = frame;

        let (truthy, falsy) = if negate {
            (value::FALSE, value::TRUE)
        } else {
            (value::TRUE, value::FALSE)
        };
        let truthy = self.constant(truthy);
        let falsy = self.constant(falsy);
        let result = self.add_phi(&[truthy, falsy, truthy, falsy]);
        self.write(dst, result)
    }

    /// Builds an instruction as a generic node: write back every slot it may
    /// read, run its slow path, and load the slots it writes from the frame.
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
        op: Op<'z>,
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
        op: Op<'z>,
        inputs: &[Operand],
        forwarded: Option<Operand>,
    ) -> Result<Flow, CompileFailure> {
        let opcode = instruction.opcode();
        let mut input_nodes = ZoneVec::with_capacity_in(inputs.len(), self.work);
        for operand in inputs {
            let node = self.read(*operand)?;
            input_nodes.push(node);
        }
        let layout = self.function.layout;
        let is_input_slot = |slot: &usize| {
            inputs
                .iter()
                .any(|operand| layout.tracked_index(*operand) == Some(*slot))
        };
        let live_in = self.live_in(self.function.instruction_index);
        // NB: A virtual arguments object stays virtual unless the slow path reads
        //     it; codegen creates it if the slow path leaves the compiled code.
        let mut read_slots = self.slot_list();
        let mut written_slots = self.slot_list();
        instruction.for_each_operand(|operand, role| {
            if role.is_read() && Some(operand) != forwarded {
                read_slots.extend(layout.tracked_index(operand).filter(|slot| !is_input_slot(slot)));
            }
            if role.is_written() {
                written_slots.extend(layout.tracked_index(operand));
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
        let is_operand_slot = |slot: &usize| {
            read_slots.contains(slot) || (written_slots.contains(slot) && live_in.contains(*slot) && !value_is_result)
        };
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
        // NB: Syncing a slot changes no other slot, so each one can be synced as soon as it is found to need it.
        for slot in 0..self.frame.slots.len() {
            let SlotState { node, in_sync } = self.frame.slots[slot];
            let needed = if writes_only_operands {
                is_operand_slot(&slot)
            } else {
                info.reads_whole_frame || live_in.contains(slot)
            };
            if !in_sync
                && needed
                && (!self.is_virtual_arguments(node) || info.reads_whole_frame || read_slots.contains(&slot))
            {
                self.sync_slot(slot);
            }
        }
        self.release_slot_set(live_in);
        self.release_slot_list(read_slots);

        let mut is_conditional_jump = false;
        instruction.for_each_jump_target(|_| is_conditional_jump = true);
        let repr = if is_conditional_jump {
            Some(Repr::Int32)
        } else {
            value_is_result.then_some(Repr::Tagged)
        };
        let node = self.emit(op, &input_nodes, repr);

        let mut destination = None;
        instruction.for_each_operand(|operand, role| {
            if role.is_written() {
                destination.get_or_insert(operand);
            }
        });
        // NB: The node writes its outputs to the frame, but for a call's result, which is its value. Where it does not
        //     continue, they hold what it wrote, or their old values, which the frame holds too (see above), so its
        //     frame state lists them as held by the frame. Where it continues, they are loaded from there. Written
        //     constants live in frame memory, where the slow path writes them.
        if value_is_result {
            written_slots.clear();
        }
        let frame_outputs = written_slots;
        for slot in &frame_outputs {
            self.frame.slots[*slot].in_sync = true;
        }
        let frame_state = self.resume_after_frame_state(destination.map_or(Operand::INVALID, Operand::raw));
        self.graph.nodes[node.index()].frame_state = Some(frame_state);
        for &slot in &frame_outputs {
            self.load_slot(slot);
        }
        self.release_slot_list(frame_outputs);
        if value_is_result && let Some(destination) = destination {
            self.write(destination, node)?;
        }
        self.end_generic_instruction(instruction, node)
    }

    /// Ends the instruction `node` ran through its slow path: with a branch
    /// on the control word of a conditional jump, and after instructions
    /// that never continue with the next one.
    fn end_generic_instruction(&mut self, instruction: &Instruction, node: NodeId) -> Result<Flow, CompileFailure> {
        let mut targets = ZoneVec::new_in(self.work);
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
                    &[node],
                    if_true,
                    if_false,
                )?;
            }
            return Ok(Flow::Ended);
        }
        if !instruction.opcode().can_fall_through() {
            self.set_control(Op::Unreachable, &[]);
            return Ok(Flow::Ended);
        }
        Ok(Flow::Continue)
    }
}

#[cfg(test)]
pub(crate) mod tests;
