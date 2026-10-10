/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Lowers register allocated IR onto a `PortableMacroAssembler`.
//!
//! Compiled code is entered as `JitResult entry(VM*, ExecutionContext* frame)`
//! and keeps the VM and the frame in callee-saved registers. Frame slots are
//! embedded in the `ExecutionContext`, so they are addressed relative to the
//! frame register.
//!
//! The JIT frame (the locals area of the machine frame) holds, from the stack
//! pointer up: outgoing stack arguments of slow path calls, the operand record
//! slow paths take by reference, the index of the exit being taken, the
//! register dump of the exit stub, and the spill slots.
//!
//! Generic nodes call the interpreter's slow paths with the interpreter's
//! calling conventions (see `slow_path`). A slow path returns a control word:
//! a continuation with the next instruction's pc means the instruction is done
//! and compiled code continues; anything else returns to the caller with
//! `JitStatus::Resume` (the interpreter continues where the slow path left the
//! running execution context) or, for negative control words,
//! `JitStatus::ExitInterpreter`.

mod allocation;
mod call;
mod call_stub;
mod entry;
pub use call::DIRECT_CALL_MAX_INPUTS;
pub use call::DIRECT_CALL_TEMPS;

/// Where a node finds an input that may be an immediate.
#[derive(Debug, Clone, Copy)]
enum InputValue {
    Register(Gpr),
    Constant(u64),
}
pub use call_stub::generate_call_stub;
pub use entry::generate_entry_trampoline;
mod arrays;
mod concatenation;
mod elements;
mod environments;
mod float64;
mod named_storage;
mod probes;
mod property_iterator;
mod sites;
mod slow_path;
mod slow_path_calls;
mod speculation;
mod strings;
mod values;

pub use slow_path::SlowPathCall;
pub use slow_path::ValueSource;
pub use slow_path::binary_operands;
pub use slow_path::jump_operands;
pub use slow_path::slow_path_call;
pub use slow_path::slow_path_symbol;

use crate::CompileFailure;
use crate::asm::Address;
use crate::asm::Architecture;
use crate::asm::Condition;
use crate::asm::Fpr;
use crate::asm::FprSet;
use crate::asm::Gpr;
use crate::asm::GprSet;
use crate::asm::Label;
use crate::asm::MachineFrame;
use crate::asm::PortableMacroAssembler;
use crate::asm::disassembler;
use crate::bytecode::DecodedInstruction;
use crate::bytecode::OpCode;
use crate::bytecode::OperandRole;
use crate::bytecode::SlowPathAbi;
use crate::bytecode::decode_instruction;
use crate::bytecode::slow_path_layout;
use crate::code::JitStatus;
use crate::code::Site;
use crate::code::SiteKind;
use crate::code::ValueLocation;
use crate::ir;
use crate::ir::BlockId;
use crate::ir::BranchCondition;
use crate::ir::FrameField;
use crate::ir::Graph;
use crate::ir::NodeId;
use crate::ir::Op;
use crate::ir::ShapeCheck;
use crate::ir::value;
use crate::regalloc::Allocation;
use crate::regalloc::Location;
use crate::regalloc::Move;
use crate::regalloc::RegisterMask;
use crate::regalloc::RegisterSet;
use crate::snapshot::CellId;
use crate::snapshot::ExecutableSnapshot;
use crate::snapshot::RuntimeInfo;
use crate::snapshot::StressOptions;
pub(crate) use sites::site;

/// Bit 32 of a slow path control word marks a continuation in the same frame.
const CONTINUATION_BIT: u64 = 1 << 32;

/// The registers codegen keeps for itself; none of them is allocatable.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct PinnedRegisters {
    pub vm: Gpr,
    pub frame: Gpr,
    /// The register allocator's scratch register: moves between stack slots
    /// go through it, and node lowerings may clobber it.
    pub scratch: Gpr,
}

/// The register allocator's view of the target, and the registers codegen
/// pins: the first three callee-saved allocatable registers.
pub fn target_registers<M: PortableMacroAssembler>() -> (RegisterSet, PinnedRegisters) {
    let mut callee_saved = M::CALLEE_SAVED_GPRS.intersection(M::ALLOCATABLE_GPRS).iter();
    let mut next = || {
        callee_saved
            .next()
            .expect("the target has three callee-saved registers")
    };
    let pinned = PinnedRegisters {
        vm: next(),
        frame: next(),
        scratch: next(),
    };
    let allocatable = M::ALLOCATABLE_GPRS
        .without(pinned.vm)
        .without(pinned.frame)
        .without(pinned.scratch);
    // NB: Lowerings compute with the first floating point registers (see
    //     `Codegen::lowering_fpr()`), which hold no values.
    let allocatable_fprs = M::ALLOCATABLE_FPRS
        .iter()
        .skip(values::LOWERING_FPRS)
        .fold(0u64, |mask, register| mask | (1 << register.0));
    let registers = RegisterSet {
        allocatable_gprs: RegisterMask(u64::from(allocatable.0)),
        allocatable_fprs: RegisterMask(allocatable_fprs),
        caller_saved_gprs: RegisterMask(u64::from(M::CALLER_SAVED_GPRS.0)),
        caller_saved_fprs: RegisterMask(u64::from(M::CALLER_SAVED_FPRS.0)),
        argument_gprs: M::ARGUMENT_GPRS.iter().map(|register| register.0).collect(),
        return_gpr: M::RETURN_GPRS[0].0,
        scratch_gpr: pinned.scratch.0,
    };
    (registers, pinned)
}

/// The registers an exit stub saves in its `RegisterDump`: every GPR and
/// FPR encoding below these counts (16 + 16 on x86-64, 31 + 32 on AArch64).
pub fn register_dump_counts<M: PortableMacroAssembler>() -> (u32, u32) {
    let gprs = M::ALLOCATABLE_GPRS
        .union(M::CALLER_SAVED_GPRS)
        .union(M::CALLEE_SAVED_GPRS)
        .union(M::SCRATCH_GPRS)
        .with(M::FRAME_POINTER);
    let fprs = M::ALLOCATABLE_FPRS
        .union(M::CALLER_SAVED_FPRS)
        .union(M::CALLEE_SAVED_FPRS)
        .union(M::SCRATCH_FPRS);
    let count = |highest: Option<u8>| u32::from(highest.map_or(0, |register| register + 1));
    (
        count(gprs.iter().next_back().map(|register| register.0)),
        count(fprs.iter().next_back().map(|register| register.0)),
    )
}

/// A register of either class, from its number in `Location`s (see
/// `regalloc::FPR_BASE`).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(super) enum Register {
    General(Gpr),
    Float(Fpr),
}

pub(super) fn register_of(register: u8) -> Register {
    if register >= crate::regalloc::FPR_BASE {
        Register::Float(Fpr(register - crate::regalloc::FPR_BASE))
    } else {
        Register::General(Gpr(register))
    }
}

/// Machine code for one graph, before it becomes `CompiledCode`.
pub struct GeneratedCode {
    pub code: Vec<u8>,
    /// Where the instructions end and the constants they load begin.
    pub data_offset: u32,
    pub entry_offset: u32,
    pub sites: Vec<Site>,
    /// What the code from each offset on was generated for, in emission
    /// order. Only `dump-asm` reads it.
    pub annotations: Vec<(u32, CodeAnnotation)>,
    /// The on-stack replacement entry points, as (back edge pc, code offset).
    pub osr_entries: Vec<(u32, u32)>,
    /// What invalidates the code: the bytes to write at each offset (see
    /// `Op::AssumeValid`).
    pub invalidation_patches: Vec<(u32, Vec<u8>)>,
}

/// What a run of generated code is for, so `dump-asm` can label it.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum CodeAnnotation {
    Prologue,
    Block(BlockId),
    Node(NodeId),
    /// The register allocator's moves at the end of a block, before its
    /// control node.
    BlockEndMoves,
    /// Out-of-line code of a node, emitted after the blocks.
    Deferred(NodeId),
    /// The code that takes the exit or leave site `index` (an index into
    /// `sites`) of `node`.
    Exit {
        index: u32,
        node: NodeId,
    },
    /// Shared code after the blocks.
    Tail(&'static str),
}

/// Where a slow path leaves an output value.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(super) enum OutputSource {
    Register(Gpr),
    /// An offset into the locals area (the operand record).
    Record(u32),
}

/// Offsets within the locals area, relative to the stack pointer.
#[derive(Debug, Clone, Copy, Default)]
struct Locals {
    record: u32,
    exit_index: u32,
    /// The compiled function's frame, while inlined frames are pushed.
    root_frame: u32,
    /// The control word of a slow path that did not continue, while its
    /// frame state is written.
    leave_control: u32,
    dump: u32,
    /// Where the slow paths of `CallSlowPath` nodes save registers.
    saves: u32,
    spills: u32,
}

struct Codegen<'a, M: PortableMacroAssembler> {
    masm: M,
    graph: &'a Graph,
    allocation: &'a Allocation,
    /// `Snapshot::executables`; index 0 is the compiled function.
    executables: &'a [ExecutableSnapshot],
    runtime: &'a RuntimeInfo,
    stress: StressOptions,
    pinned: PinnedRegisters,
    frame: MachineFrame,
    locals: Locals,
    gpr_dump_count: u32,
    fpr_dump_count: u32,
    block_labels: Vec<Label>,
    /// Where control that goes to each block lands (see `landing_blocks()`).
    landing_blocks: Vec<BlockId>,
    exit_stub: Label,
    resume: Label,
    /// Where the prologues resume in the interpreter.
    entry_resume: Label,
    exit_interpreter: Label,
    /// The nodes of the code's sites, by site index, with what they are.
    sites: Vec<(NodeId, SiteKind)>,
    /// Out of line stubs that store an exit index and jump to the exit stub.
    exit_site_stubs: Vec<(Label, u32)>,
    annotations: Vec<(u32, CodeAnnotation)>,
    /// The block being emitted.
    block: BlockId,
    deferred_cache_probes: Vec<probes::DeferredCacheProbe>,
    deferred_allocations: Vec<allocation::DeferredAllocation>,
    deferred_storage_growths: Vec<named_storage::DeferredStorageGrowth>,
    /// `InitializeNamed` nodes whose constant the allocation of their object
    /// stored already.
    absorbed_initializations: Vec<NodeId>,
    /// The slots of the compiled function's frame that hold its arguments
    /// object, which the code never created, when the node being emitted
    /// (in the compiled function or an inlined callee) leaves the code after
    /// its slow path: they get the object before the interpreter continues
    /// with the frames. Whether it is a mapped one.
    leave_arguments: Vec<(u32, bool)>,
    /// Out of line code for slow paths that do not continue with the next
    /// instruction, emitted with the tails.
    leave_stubs: Vec<LeaveStub>,
    /// How the slow path of the node being emitted leaves compiled code, if
    /// it writes the node's frame state then (see `LeaveFrame`).
    leave_frame: Option<LeaveFrame>,
    /// The shared code that writes the frame state of a leave.
    leave_tail: Label,
    /// While a `CallSlowPath` node that is a call is emitted: where the
    /// operands of its slow path are.
    slow_path_values: Option<SlowPathValues>,
    /// The target of the branch on the result of the cache probe being
    /// emitted, which the probe branches to itself where it finds nothing
    /// (see `fused_probe_branch()`).
    fused_probe_failure: Option<Label>,
    /// The no-ops of `AssumeValid` nodes, with the exits that the jumps
    /// replacing them go to once the code is invalidated.
    invalidation_points: Vec<(usize, Label)>,
}

/// The operands of the slow path of a `CallSlowPath` node that is a call,
/// which reads them from the node's inputs instead of frame slots.
struct SlowPathValues {
    node: NodeId,
    /// The operands of the node's inputs, in input order.
    operands: Vec<u32>,
    /// Where the inputs in registers are.
    registers: InputRegisters,
    /// The outputs that are SSA values, which no frame slot receives.
    ssa_outputs: Vec<u32>,
}

/// Where the code of a slow path call or a call finds those of its inputs
/// that are in registers, once it clobbered registers.
#[derive(Debug, Clone)]
pub(super) enum InputRegisters {
    /// In their registers, except for these, which are in the saves area,
    /// in the order of their words there.
    Saved(Vec<Gpr>),
    /// In the register dump that publishing the frames of the inlined calls
    /// wrote (see `emit_publish_frames()`), since that clobbered every
    /// register a call clobbers.
    Dumped,
}

/// What a slow path that does not continue in compiled code does before it
/// returns to the interpreter: it writes the frame state of `node` into the
/// compiled function's frame through `RuntimeInfo::jit_exit`, which finds
/// the values like for an exit but knows from the `SiteKind::Leave` that
/// the code keeps running. Compiled code only writes the slots a slow
/// path reads before calling it, so this is where the other values reach
/// the frame. Slow paths and calls in inlined callees run in frames the
/// runtime published with their headers only (see `SiteKind::Publish`),
/// which the leave fills as far as they are still on the stack.
#[derive(Debug, Clone)]
struct LeaveFrame {
    node: NodeId,
    /// Registers a slow path saved, with where, to restore first.
    restore: Vec<(Gpr, u32)>,
}

/// The out of line part of a slow path call: what runs when the slow path
/// does not continue with the next instruction.
struct LeaveStub {
    label: Label,
    /// See `Codegen::leave_frame`.
    leave_frame: Option<LeaveFrame>,
    /// Outputs that are also inputs, stored back if the frame still runs.
    input_outputs: Vec<(OutputSource, u32)>,
    /// See `Codegen::leave_arguments`.
    leave_arguments: Vec<(u32, bool)>,
}

/// Generates code for `graph` with `allocation`, whose registers came from
/// `target_registers::<M>()`.
pub fn generate<M: PortableMacroAssembler>(
    graph: &Graph,
    allocation: &Allocation,
    executables: &[ExecutableSnapshot],
    runtime: &RuntimeInfo,
    stress: StressOptions,
) -> Result<GeneratedCode, CompileFailure> {
    let (_, pinned) = target_registers::<M>();
    let (gpr_dump_count, fpr_dump_count) = register_dump_counts::<M>();
    let mut masm = M::new();
    let block_labels = graph.blocks.iter().map(|_| masm.new_label()).collect();
    let landing_blocks = landing_blocks(graph, allocation);
    let exit_stub = masm.new_label();
    let resume = masm.new_label();
    let entry_resume = masm.new_label();
    let exit_interpreter = masm.new_label();
    let leave_tail = masm.new_label();

    let mut codegen = Codegen {
        masm,
        graph,
        allocation,
        executables,
        runtime,
        stress,
        pinned,
        frame: M::frame(GprSet::EMPTY, FprSet::EMPTY, 0),
        locals: Locals::default(),
        gpr_dump_count,
        fpr_dump_count,
        block_labels,
        landing_blocks,
        exit_stub,
        resume,
        entry_resume,
        exit_interpreter,
        sites: Vec::new(),
        exit_site_stubs: Vec::new(),
        annotations: Vec::new(),
        block: BlockId(0),
        deferred_cache_probes: Vec::new(),
        deferred_allocations: Vec::new(),
        deferred_storage_growths: Vec::new(),
        absorbed_initializations: Vec::new(),
        leave_arguments: Vec::new(),
        leave_stubs: Vec::new(),
        leave_frame: None,
        leave_tail,
        slow_path_values: None,
        invalidation_points: Vec::new(),
        fused_probe_failure: None,
    };
    codegen.lay_out_frame()?;
    codegen.emit_prologue();
    for index in 0..graph.blocks.len() {
        let block = BlockId::from_index(index);
        // NB: Nothing jumps to a block that only jumps on, nor falls into it.
        if codegen.landing_blocks[index] == block {
            codegen.emit_block(block)?;
        }
    }
    codegen.emit_deferred_code()?;
    codegen.emit_tails();
    let osr_entries = codegen.emit_osr_entries();

    let fp_offset = |slot: u32| codegen.frame_pointer_offset(slot);
    let sites = codegen
        .sites
        .iter()
        .map(|(node, kind)| {
            let mut descriptor = site(graph, allocation, *node, *kind, &fp_offset);
            // NB: A slow path call keeps the registers it saved in the saves
            //     area while it runs, where its exits find them.
            if let Op::CallSlowPath {
                saves_registers: true, ..
            } = graph.node(*node).op
            {
                let saved = slow_path_calls::slow_path_saved_registers_of::<M>(allocation.node(*node));
                let saves = codegen.save_slot_offsets(&saved);
                for frame in &mut descriptor.frames {
                    for (_, location) in &mut frame.values {
                        relocate_saved_register(location, &saves);
                    }
                }
                for object in &mut descriptor.objects {
                    for location in &mut object.properties {
                        relocate_saved_register(location, &saves);
                    }
                }
            }
            descriptor
        })
        .collect();
    let annotations = std::mem::take(&mut codegen.annotations);
    let invalidation_patches = codegen
        .invalidation_points
        .iter()
        .map(|(offset, exit)| {
            let target = codegen.masm.label_offset(*exit).expect("exit stubs are bound");
            (
                u32::try_from(*offset).expect("code fits in 4 GiB"),
                M::jump_patch(*offset, target),
            )
        })
        .collect();
    let (code, data_offset) = codegen
        .masm
        .finish_with_data_offset()
        .map_err(|_| CompileFailure::CodeGeneration)?;
    let data_offset = u32::try_from(data_offset).expect("code fits in 4 GiB");
    Ok(GeneratedCode {
        code,
        data_offset,
        entry_offset: 0,
        sites,
        annotations,
        osr_entries,
        invalidation_patches,
    })
}

/// Points `location`, a register among `saves` (registers with the frame
/// pointer offsets of their words in the saves area), at its word there.
fn relocate_saved_register(location: &mut ValueLocation, saves: &[(Gpr, i32)]) {
    // NB: Float64 values are in floating point registers, which are not saved.
    if let ValueLocation::Register(register, repr) = *location
        && repr != crate::code::Repr::Float64
        && let Some((_, offset)) = saves.iter().find(|(saved, _)| saved.0 == register)
    {
        *location = ValueLocation::Stack(*offset, repr);
    }
}

/// The machine code as disassembly, under headers naming the block, IR node
/// or tail each run of instructions was generated for. Calls of runtime
/// helpers and well-known values are named.
pub fn dump_code(
    graph: &Graph,
    executable: &ExecutableSnapshot,
    runtime: &RuntimeInfo,
    architecture: Architecture,
    generated: &GeneratedCode,
) -> String {
    use std::fmt::Write;

    let mut symbols = vec![
        (runtime.jit_call, "libjs_jit_call".to_string()),
        (runtime.jit_exit, "libjs_jit_exit".to_string()),
        (runtime.finish_direct_call, "libjs_jit_finish_direct_call".to_string()),
        (runtime.to_boolean, "asm_helper_to_boolean".to_string()),
        (runtime.heap_region_base, "js_heap_region_base".to_string()),
    ];
    for (index, address) in runtime.slow_paths.iter().enumerate() {
        let opcode = u8::try_from(index).ok().and_then(OpCode::from_u8);
        if let Some(symbol) = opcode.and_then(slow_path_symbol) {
            symbols.push((*address, symbol));
        }
    }
    symbols.retain(|(address, _)| *address != 0);
    let describe_constant = |bits: u64| {
        if let Some((_, symbol)) = symbols.iter().find(|(address, _)| *address == bits) {
            return Some(symbol.clone());
        }
        let bytecode_end = executable.bytecode_address + executable.bytecode.len() as u64;
        if (executable.bytecode_address..bytecode_end).contains(&bits) {
            return Some(format!("bytecode @{}", bits - executable.bytecode_address));
        }
        let description = value::describe(bits);
        (!description.starts_with("0x")).then_some(description)
    };

    let instructions = disassembler::listing_with_data(architecture, &generated.code, generated.data_offset as usize);
    let mut annotations = generated.annotations.iter().peekable();
    let layout = &executable.layout;
    let mut out = String::new();
    for instruction in &instructions {
        while let Some((_, annotation)) = annotations.next_if(|(offset, _)| *offset as usize <= instruction.offset) {
            match annotation {
                CodeAnnotation::Prologue => out.push_str("prologue:\n"),
                CodeAnnotation::Block(block) => writeln!(out, "b{}:", block.0).unwrap(),
                CodeAnnotation::Node(node) => {
                    writeln!(out, "  {}", ir::node_text(graph, layout, *node)).unwrap();
                }
                CodeAnnotation::BlockEndMoves => out.push_str("  block end moves\n"),
                CodeAnnotation::Deferred(node) => {
                    writeln!(out, "deferred code of {}", ir::node_text(graph, layout, *node)).unwrap();
                }
                CodeAnnotation::Tail(name) => writeln!(out, "{name}:").unwrap(),
                CodeAnnotation::Exit { index, node } => {
                    let kind = match generated.sites[*index as usize].kind {
                        SiteKind::Exit(kind) => format!("{kind:?}"),
                        kind => format!("{kind:?}"),
                    };
                    writeln!(out, "  exit #{index} ({kind}) of v{}:", node.0).unwrap();
                }
            }
        }
        write!(out, "    {:06x}  {}", instruction.offset, instruction.text).unwrap();
        if let Some(description) = instruction.constant.and_then(describe_constant) {
            write!(out, "  ; {description}").unwrap();
        }
        out.push('\n');
    }
    let count = |kind: fn(SiteKind) -> bool| generated.sites.iter().filter(|site| kind(site.kind)).count();
    writeln!(
        out,
        "{} instructions, {} bytes, {} exits, {} leaves, {} publish sites, {} call sites",
        instructions.len(),
        generated.code.len(),
        count(|kind| matches!(kind, SiteKind::Exit(_))),
        count(|kind| kind == SiteKind::Leave),
        count(|kind| kind == SiteKind::Publish),
        count(|kind| kind == SiteKind::Call),
    )
    .unwrap();
    out
}

/// For a block that ends in a cache probe and a branch on its result, where
/// the result is not live into the branch's target for failure: the branch,
/// that target and the target for success. The probe branches to the target
/// for failure itself, without producing a value, and the branch only
/// continues at the target for success.
fn fused_probe_branch(graph: &Graph, allocation: &Allocation, block: BlockId) -> Option<(NodeId, BlockId, BlockId)> {
    let data = graph.block(block);
    let probe = *data.body.last()?;
    let control = data.control?;
    let inputs = &graph.node(control).inputs;
    if inputs.first() != Some(&probe) {
        return None;
    }
    let (failure, success) = match (&graph.node(probe).op, &graph.node(control).op) {
        (
            Op::ProbePropertyCache { .. }
            | Op::ProbeKeyedCache { .. }
            | Op::ProbeGlobalCache { .. }
            | Op::ProbeHasProperty { .. },
            Op::Branch {
                condition: BranchCondition::TaggedEquals { equal: true },
                if_true,
                if_false,
            },
        ) if graph.constant_value(inputs[1]) == Some(value::EMPTY) => (*if_true, *if_false),
        (
            Op::ProbeKeyedStore { .. } | Op::ProbePropertyStore { .. } | Op::ProbeGlobalStore { .. },
            Op::Branch {
                condition: BranchCondition::Bool,
                if_true,
                if_false,
            },
        ) => (*if_false, *if_true),
        _ => return None,
    };
    (!allocation.live_in[failure.index()].contains(probe.index())).then_some((control, failure, success))
}

/// Where control that goes to each block lands: blocks that only jump on, like the edges the builder splits off
/// branches, pass it on, so jumps go straight to where they lead and those blocks have no code. The first block, which
/// the prologue falls into, and blocks that jump on in a cycle land in themselves.
fn landing_blocks(graph: &Graph, allocation: &Allocation) -> Vec<BlockId> {
    let block_count = graph.blocks.len();
    let jumps_on = |index: usize| -> Option<BlockId> {
        let block = &graph.blocks[index];
        if index == 0
            || !block.phis.is_empty()
            || !block.body.is_empty()
            || !allocation.block_end_moves[index].is_empty()
        {
            return None;
        }
        match graph.node(block.control?).op {
            Op::Jump { target } => Some(target),
            _ => None,
        }
    };
    (0..block_count)
        .map(|index| {
            let mut landing = index;
            for _ in 0..block_count {
                match jumps_on(landing) {
                    Some(target) => landing = target.index(),
                    None => return BlockId::from_index(landing),
                }
            }
            BlockId::from_index(index)
        })
        .collect()
}

fn checked_i32(value: u64) -> Result<i32, CompileFailure> {
    i32::try_from(value).map_err(|_| CompileFailure::CodeGeneration)
}

impl<M: PortableMacroAssembler> Codegen<'_, M> {
    // Frame layout.

    /// Sizes the locals area and decides which callee-saved registers to save.
    fn lay_out_frame(&mut self) -> Result<(), CompileFailure> {
        let argument_registers = M::ARGUMENT_GPRS.len() as u32;
        let mut outgoing_bytes = 0;
        let mut record_bytes = 0;
        // NB: Slow paths that do not continue write their frame state like
        //     exits do (see `LeaveFrame`).
        let has_exits = self
            .graph
            .nodes
            .iter()
            .any(|node| node.op.exit_kind().is_some() || node.frame_state.is_some());
        let needs_saves_area = (0..self.graph.blocks.len())
            .flat_map(|block| self.graph.block_nodes(BlockId::from_index(block)))
            .any(|node| self.uses_saves_area(node));
        for node in &self.graph.nodes {
            // NB: The runtime's PutById cache probe takes a record.
            if let Op::ProbePropertyStore { .. } = node.op {
                record_bytes = record_bytes.max(probes::PUT_BY_ID_RECORD_BYTES);
            }
            // NB: Binary slow paths called on values write their result into
            //     the record.
            if let Op::CallSlowPath { opcode, .. } = node.op
                && slow_path_call(opcode) == Some(SlowPathCall::BinaryValues)
            {
                record_bytes = record_bytes.max(8);
            }
            match node.op {
                Op::Generic { opcode, executable, pc }
                | Op::CallSlowPath {
                    opcode, executable, pc, ..
                } if slow_path_call(opcode) == Some(SlowPathCall::Record) => {
                    let layout = slow_path_layout(opcode);
                    let (inputs, fixed_registers) = match layout.abi {
                        SlowPathAbi::Scalar => (
                            layout
                                .fields
                                .iter()
                                .filter(|field| field.role == OperandRole::In)
                                .count(),
                            3,
                        ),
                        SlowPathAbi::ScalarInputs => (
                            layout
                                .fields
                                .iter()
                                .filter(|field| field.role != OperandRole::Out)
                                .count(),
                            4,
                        ),
                        SlowPathAbi::Record => (0, 4),
                    };
                    let stack_arguments = (inputs as u32).saturating_sub(argument_registers - fixed_registers);
                    outgoing_bytes = outgoing_bytes.max(8 * stack_arguments);
                    let array_count = layout
                        .array
                        .map_or(Ok(0), |array| self.read_u32(executable, pc, array.count_offset))?;
                    let record = match layout.abi {
                        SlowPathAbi::Scalar => 0,
                        SlowPathAbi::ScalarInputs => layout.fields.len() as u32,
                        SlowPathAbi::Record => layout.fields.len() as u32 + array_count,
                    };
                    record_bytes = record_bytes.max(8 * record);
                }
                _ => {}
            }
        }
        let record = outgoing_bytes.next_multiple_of(16);
        let exit_index = record + record_bytes;
        let root_frame = exit_index + 8;
        let leave_control = root_frame + 8;
        let dump = (leave_control + 8).next_multiple_of(16);
        let dump_bytes = if has_exits {
            8 * (self.gpr_dump_count + self.fpr_dump_count)
        } else {
            0
        };
        let saves = (dump + dump_bytes).next_multiple_of(8);
        let saves_bytes = if needs_saves_area {
            8 * slow_path_calls::slow_path_saved_registers::<M>().len() as u32
        } else {
            0
        };
        let spills = saves + saves_bytes;
        let size = spills + 8 * self.allocation.spill_slot_count;
        self.locals = Locals {
            record,
            exit_index,
            root_frame,
            leave_control,
            dump,
            saves,
            spills,
        };

        // NB: JIT code leaves the pinned VM register as it found it (callers
        //     load it, see `entry.rs`) and does not keep the scratch register.
        let mut saved = GprSet::EMPTY.with(self.pinned.frame);
        let mut saved_fprs = FprSet::EMPTY;
        for register in self.used_registers() {
            match register {
                Register::General(register) if M::CALLEE_SAVED_GPRS.contains(register) => {
                    saved = saved.with(register);
                }
                Register::Float(register) if M::CALLEE_SAVED_FPRS.contains(register) => {
                    saved_fprs = saved_fprs.with(register);
                }
                _ => {}
            }
        }
        self.frame = M::frame(saved, saved_fprs, size);
        Ok(())
    }

    fn used_registers(&self) -> Vec<Register> {
        let mut registers = Vec::new();
        let mut add = |location: &Location| {
            if let Location::Register(register) = location {
                registers.push(register_of(*register));
            }
        };
        for node in &self.allocation.nodes {
            node.inputs.iter().for_each(&mut add);
            node.output.iter().for_each(&mut add);
            for m in &node.moves_before {
                add(&m.from);
                add(&m.to);
            }
            for temp in &node.temps {
                add(&Location::Register(*temp));
            }
        }
        for moves in &self.allocation.block_end_moves {
            for m in moves {
                add(&m.from);
                add(&m.to);
            }
        }
        registers
    }

    /// The `saved` registers of a slow path call, with the frame pointer
    /// offsets of their words in the saves area.
    fn save_slot_offsets(&self, saved: &[Gpr]) -> Vec<(Gpr, i32)> {
        let frame_pointer = i64::from(self.frame.caller_stack_offset) - 16;
        saved
            .iter()
            .enumerate()
            .map(|(index, register)| {
                let from_stack_pointer = i64::from(self.locals.saves) + 8 * index as i64;
                (
                    *register,
                    i32::try_from(from_stack_pointer - frame_pointer).expect("JIT frames are smaller than 2 GiB"),
                )
            })
            .collect()
    }

    /// The offset of a spill slot from the frame pointer, as exits record it.
    fn frame_pointer_offset(&self, slot: u32) -> i32 {
        let from_stack_pointer = i64::from(self.locals.spills + 8 * slot);
        let frame_pointer = i64::from(self.frame.caller_stack_offset) - 16;
        i32::try_from(from_stack_pointer - frame_pointer).expect("JIT frames are smaller than 2 GiB")
    }

    fn spill_address(&self, slot: u32) -> Address {
        Address::new(M::STACK_POINTER, (self.locals.spills + 8 * slot) as i32)
    }

    fn local_address(&self, offset: u32) -> Address {
        Address::new(M::STACK_POINTER, offset as i32)
    }

    /// The address of frame slot `index` (a flat operand index).
    fn slot_address(&self, index: u32) -> Result<Address, CompileFailure> {
        let offset = u64::from(self.runtime.offsets.execution_context_slots) + 8 * u64::from(index);
        Ok(Address::new(self.pinned.frame, checked_i32(offset)?))
    }

    fn frame_field(&self, offset: u32) -> Address {
        Address::new(self.pinned.frame, offset as i32)
    }

    /// The instruction at `pc` of executable `executable`.
    fn decoded(&self, executable: u32, pc: u32) -> Result<DecodedInstruction, CompileFailure> {
        decode_instruction(&self.executables[executable as usize].bytecode, pc).map_err(|_| {
            CompileFailure::InvalidBytecode {
                pc,
                reason: "undecodable instruction",
            }
        })
    }

    /// The u32 at `offset` in the instruction at `pc` of executable `executable`.
    fn read_u32(&self, executable: u32, pc: u32, offset: u32) -> Result<u32, CompileFailure> {
        let at = (pc + offset) as usize;
        let bytes =
            self.executables[executable as usize]
                .bytecode
                .get(at..at + 4)
                .ok_or(CompileFailure::InvalidBytecode {
                    pc,
                    reason: "operand field outside the bytecode",
                })?;
        Ok(u32::from_ne_bytes(bytes.try_into().expect("four bytes")))
    }

    // Prologue and tails.

    fn annotate(&mut self, annotation: CodeAnnotation) {
        let offset = u32::try_from(self.masm.offset()).expect("code fits in 4 GiB");
        self.annotations.push((offset, annotation));
    }

    fn emit_prologue(&mut self) {
        self.annotate(CodeAnnotation::Prologue);
        let frame = self.frame;
        self.masm.emit_prologue(&frame);
        // NB: The VM is in its pinned register already.
        self.masm.move64(self.pinned.frame, M::ARGUMENT_GPRS[1]);
        let root_frame = self.local_address(self.locals.root_frame);
        self.masm.store64(&root_frame, self.pinned.frame);

        // Exit to the interpreter (which runs the function on its own stack,
        // and reports overflows of that) if the native stack is nearly
        // exhausted. No value lives in a register yet.
        let limit = Address::new(self.pinned.vm, self.runtime.offsets.vm_jit_native_stack_limit as i32);
        let resume = self.entry_resume;
        self.masm.branch_if_stack_pointer_below(&limit, resume);

        // Materializing the frames of inlined calls must not run out of
        // interpreter stack, so exit if they would not all fit.
        if self.graph.materialized_frame_bytes != 0 {
            let offsets = self.runtime.offsets;
            let top = M::RETURN_GPRS[1];
            self.masm.load64(
                top,
                &Address::new(self.pinned.vm, offsets.vm_interpreter_stack_top as i32),
            );
            self.masm
                .add64_imm(top, top, self.graph.materialized_frame_bytes as i64);
            self.masm.load64(
                self.pinned.scratch,
                &Address::new(self.pinned.vm, offsets.vm_interpreter_stack_limit as i32),
            );
            self.masm.branch64(Condition::Above, top, self.pinned.scratch, resume);
        }
    }

    fn emit_return(&mut self, status: JitStatus) {
        self.masm.move_imm32(M::RETURN_GPRS[1], status as u32);
        let frame = self.frame;
        self.masm.emit_epilogue(&frame);
        self.masm.ret();
    }

    fn emit_tails(&mut self) {
        self.annotate(CodeAnnotation::Tail("return Resume"));
        let resume = self.resume;
        // NB: The interpreter runs a frame that resumes before it ran, which
        //     must be the running one.
        let entry_resume = self.entry_resume;
        self.masm.bind(entry_resume);
        self.emit_publish_frame();
        self.masm.bind(resume);
        self.masm.move_imm32(M::RETURN_GPRS[0], 0);
        self.emit_return(JitStatus::Resume);

        self.annotate(CodeAnnotation::Tail("return ExitInterpreter"));
        let exit_interpreter = self.exit_interpreter;
        self.masm.bind(exit_interpreter);
        self.masm.move_imm32(M::RETURN_GPRS[0], 0);
        self.emit_return(JitStatus::ExitInterpreter);

        let leave_stubs = std::mem::take(&mut self.leave_stubs);
        if !leave_stubs.is_empty() {
            self.annotate(CodeAnnotation::Tail("slow paths that did not continue"));
        }
        let mut writes_leave_frames = false;
        for stub in leave_stubs {
            writes_leave_frames |= stub.leave_frame.is_some();
            self.emit_leave_stub(stub)
                .expect("outputs of slow paths have valid slots");
        }
        if writes_leave_frames {
            self.emit_leave_tail();
        }
        if !self.exit_site_stubs.is_empty() {
            self.annotate(CodeAnnotation::Tail("exit sites"));
        }
        for (label, index) in std::mem::take(&mut self.exit_site_stubs) {
            let node = self.sites[index as usize].0;
            self.annotate(CodeAnnotation::Exit { index, node });
            self.masm.bind(label);
            let address = self.local_address(self.locals.exit_index);
            self.masm.store_imm32(&address, index);
            let exit_stub = self.exit_stub;
            self.masm.jump(exit_stub);
        }
        if !self.sites.is_empty() {
            self.emit_exit_stub();
        }
    }

    /// Registers an exit of `node` (with its frame state) and returns the
    /// label its checks branch to when they fail.
    fn exit_site(&mut self, node: NodeId) -> Label {
        let kind = self.graph.node(node).op.exit_kind().expect("the node can exit");
        let index = u32::try_from(self.sites.len()).expect("site count fits in u32");
        self.sites.push((node, SiteKind::Exit(kind)));
        let label = self.masm.new_label();
        self.exit_site_stubs.push((label, index));
        label
    }

    /// With `StressOptions::exit_countdown`, counts the countdown down at
    /// the start of a node that can exit and takes the node's exit when it
    /// reaches 0. Its inputs and every value of its frame state are where
    /// the exit expects them here, before the node did anything.
    fn emit_stress_exit(&mut self, node: NodeId) {
        let countdown = self.stress.exit_countdown;
        let data = self.graph.node(node);
        if countdown == 0
            || data.frame_state.is_none()
            || matches!(data.op, Op::Exit { .. })
            || data.op.exit_kind().is_none()
        {
            return;
        }
        let exit = self.exit_site(node);
        let scratch = self.pinned.scratch;
        self.masm.move_imm64(scratch, countdown);
        let address = Address::new(scratch, 0);
        self.masm.add32_to_memory_imm(&address, -1);
        self.masm.branch32_memory_imm(Condition::Equal, &address, 0, exit);
    }

    /// Dumps every register, calls `libjs_jit_exit(VM*, ExecutionContext*,
    /// u32 exit_index, RegisterDump const*)` and returns `Resume`.
    fn emit_exit_stub(&mut self) {
        self.annotate(CodeAnnotation::Tail("exit stub (shared by all exits)"));
        let exit_stub = self.exit_stub;
        self.masm.bind(exit_stub);
        self.emit_exit_runtime_call();
        self.masm.move_imm32(M::RETURN_GPRS[0], 0);
        self.emit_return(JitStatus::Resume);
    }

    /// Writes the frame state of the leave whose index is in the locals,
    /// with `libjs_jit_exit()`, then leaves like any slow path that did
    /// not continue, with the control word saved in the locals.
    fn emit_leave_tail(&mut self) {
        self.annotate(CodeAnnotation::Tail("leave stub (shared by all leaves)"));
        let leave_tail = self.leave_tail;
        self.masm.bind(leave_tail);
        self.emit_exit_runtime_call();
        let control = self.local_address(self.locals.leave_control);
        self.masm.load64(M::RETURN_GPRS[0], &control);
        self.leave_arguments = Vec::new();
        self.emit_leave_after_slow_path();
    }

    /// Dumps every register and calls `libjs_jit_exit()` with the exit
    /// index in the locals.
    fn emit_exit_runtime_call(&mut self) {
        for encoding in 0..self.gpr_dump_count {
            let register = Gpr(encoding as u8);
            if register == M::STACK_POINTER || M::SCRATCH_GPRS.contains(register) {
                continue;
            }
            let address = self.local_address(self.locals.dump + 8 * encoding);
            self.masm.store64(&address, register);
        }
        for encoding in 0..self.fpr_dump_count {
            let register = Fpr(encoding as u8);
            if M::SCRATCH_FPRS.contains(register) {
                continue;
            }
            let address = self.local_address(self.locals.dump + 8 * (self.gpr_dump_count + encoding));
            self.masm.store_double(&address, register);
        }
        // NB: The compiled function's frame, which is not the running one
        //     while frames of inlined calls are pushed.
        let arguments = M::ARGUMENT_GPRS;
        let root_frame = self.local_address(self.locals.root_frame);
        self.masm.load64(arguments[1], &root_frame);
        let exit_index = self.local_address(self.locals.exit_index);
        self.masm.load32(arguments[2], &exit_index);
        let dump = self.local_address(self.locals.dump);
        self.masm.load_effective_address(arguments[3], &dump);
        self.masm.move64(arguments[0], self.pinned.vm);
        self.masm.call_absolute(self.runtime.jit_exit);
    }

    /// Emits an entry point for every on-stack replacement entry block:
    /// the prologue, then a jump to the block.
    fn emit_osr_entries(&mut self) -> Vec<(u32, u32)> {
        let mut entries = Vec::new();
        for (pc, block) in &self.graph.osr_entries {
            let offset = u32::try_from(self.masm.offset()).expect("code fits in 4 GiB");
            self.emit_prologue();
            let label = self.label(*block);
            self.masm.jump(label);
            entries.push((*pc, offset));
        }
        entries
    }

    // Blocks and nodes.

    /// The block emitted after the one being emitted.
    fn next_block(&self) -> Option<BlockId> {
        (self.block.index() + 1..self.graph.blocks.len())
            .map(BlockId::from_index)
            .find(|block| self.landing_blocks[block.index()] == *block)
    }

    fn jump_to(&mut self, target: BlockId) {
        let target = self.landing_blocks[target.index()];
        if self.next_block() != Some(target) {
            let label = self.block_labels[target.index()];
            self.masm.jump(label);
        }
    }

    fn label(&self, block: BlockId) -> Label {
        self.block_labels[self.landing_blocks[block.index()].index()]
    }

    fn emit_block(&mut self, block: BlockId) -> Result<(), CompileFailure> {
        self.block = block;
        self.annotate(CodeAnnotation::Block(block));
        let label = self.label(block);
        self.masm.bind(label);
        let block_data = self.graph.block(block);
        // Phis in registers that need their spill slots get them here.
        for phi in &block_data.phis {
            let allocation = self.allocation.node(*phi);
            if let (Some(Location::Register(register)), Some(slot)) = (allocation.output, allocation.spill_slot) {
                self.emit_move(&Move {
                    from: Location::Register(register),
                    to: Location::Stack(slot),
                });
            }
        }
        let fused = fused_probe_branch(self.graph, self.allocation, block);
        for node in &block_data.body {
            if let Some((_, failure, _)) = fused
                && Some(node) == block_data.body.last()
            {
                self.fused_probe_failure = Some(self.label(failure));
            }
            self.emit_node(*node)?;
        }
        if let Some((control, _, success)) = fused {
            self.annotate(CodeAnnotation::Node(control));
            self.jump_to(success);
            return Ok(());
        }
        let end_moves = &self.allocation.block_end_moves[block.index()];
        if !end_moves.is_empty() {
            self.annotate(CodeAnnotation::BlockEndMoves);
        }
        for m in end_moves {
            self.emit_move(m);
        }
        self.emit_node(self.graph.control_id(block))
    }

    fn emit_move(&mut self, m: &Move) {
        match (m.from, m.to) {
            (Location::Register(from), Location::Register(to)) => match (register_of(from), register_of(to)) {
                (Register::General(from), Register::General(to)) => self.masm.move64(to, from),
                (Register::Float(from), Register::Float(to)) => self.masm.move_double(to, from),
                _ => unreachable!("moves stay within a register class"),
            },
            (Location::Register(from), Location::Stack(to)) => {
                let address = self.spill_address(to);
                match register_of(from) {
                    Register::General(from) => self.masm.store64(&address, from),
                    Register::Float(from) => self.masm.store_double(&address, from),
                }
            }
            (Location::Stack(from), Location::Register(to)) => {
                let address = self.spill_address(from);
                match register_of(to) {
                    Register::General(to) => self.masm.load64(to, &address),
                    Register::Float(to) => self.masm.load_double(to, &address),
                }
            }
            (Location::Stack(from), Location::Stack(to)) => {
                let from = self.spill_address(from);
                let to = self.spill_address(to);
                self.masm.load64(self.pinned.scratch, &from);
                self.masm.store64(&to, self.pinned.scratch);
            }
            (Location::Constant(bits), Location::Register(to)) => match register_of(to) {
                Register::General(to) => self.masm.move_imm64(to, bits),
                Register::Float(to) => {
                    self.masm.move_imm64(self.pinned.scratch, bits);
                    self.masm.move_gpr_to_double(to, self.pinned.scratch);
                }
            },
            (Location::Constant(bits), Location::Stack(to)) => {
                let address = self.spill_address(to);
                self.masm.store_imm64(&address, bits);
            }
            (_, Location::Constant(_)) => unreachable!("moves never write constants"),
        }
    }

    fn input(&self, node: NodeId, index: usize) -> Gpr {
        match self.allocation.node(node).inputs[index] {
            Location::Register(register) => match register_of(register) {
                Register::General(register) => register,
                Register::Float(_) => unreachable!("float64 inputs are read with `float_input()`"),
            },
            _ => unreachable!("inputs are in registers"),
        }
    }

    /// Input `index` of `node`, a `Repr::Float64` value.
    fn float_input(&self, node: NodeId, index: usize) -> Fpr {
        match self.allocation.node(node).inputs[index] {
            Location::Register(register) => match register_of(register) {
                Register::Float(register) => register,
                Register::General(_) => unreachable!("only float64 inputs are in floating point registers"),
            },
            _ => unreachable!("inputs are in registers"),
        }
    }

    /// The output register of `node`, a `Repr::Float64` value.
    fn float_output(&self, node: NodeId) -> Fpr {
        match self.allocation.node(node).output {
            Some(Location::Register(register)) => match register_of(register) {
                Register::Float(register) => register,
                Register::General(_) => unreachable!("only float64 values are in floating point registers"),
            },
            _ => unreachable!("the node has a register output"),
        }
    }

    /// Input `index` of `node`: in a register, or a constant for inputs that
    /// may be immediates.
    fn input_value(&self, node: NodeId, index: usize) -> InputValue {
        match self.allocation.node(node).inputs[index] {
            Location::Register(register) => InputValue::Register(Gpr(register)),
            Location::Constant(bits) => InputValue::Constant(bits),
            Location::Stack(_) => unreachable!("inputs are in registers or constants"),
        }
    }

    fn output(&self, node: NodeId) -> Gpr {
        match self.allocation.node(node).output {
            Some(Location::Register(register)) => Gpr(register),
            _ => unreachable!("the node has a register output"),
        }
    }

    fn emit_node(&mut self, node_id: NodeId) -> Result<(), CompileFailure> {
        self.annotate(CodeAnnotation::Node(node_id));
        let allocation = self.allocation.node(node_id);
        for m in &allocation.moves_before {
            self.emit_move(m);
        }
        let node = self.graph.node(node_id);
        self.emit_stress_exit(node_id);
        self.leave_arguments = self.virtual_arguments_slots(node_id);
        self.leave_frame = self.leave_frame_of(node_id).map(|node| LeaveFrame {
            node,
            restore: Vec::new(),
        });
        match &node.op {
            Op::Constant(_) | Op::Phi | Op::VirtualObject { .. } | Op::Refine { .. } => {
                unreachable!("constants, phis, virtual objects and refinements have no code")
            }
            Op::LoadSlot { slot } => {
                let address = self.slot_address(*slot)?;
                self.masm.load64(self.output(node_id), &address);
            }
            Op::StoreSlot { slot } => self.emit_store_slot(node_id, *slot)?,
            Op::InitializeFrame => self.emit_initialize_frame(node_id)?,
            Op::EnsureFrameInitialized => self.emit_ensure_frame_initialized(node_id)?,
            Op::PublishFrame => self.emit_publish_frame(),
            Op::LoadOuterEnvironment => {
                let (output, input) = (self.output(node_id), self.input(node_id, 0));
                let outer = Address::new(input, self.runtime.layout.environment_outer as i32);
                self.masm.load64(output, &outer);
            }
            Op::LoadEnvironmentBinding { index } => {
                let (output, input) = (self.output(node_id), self.input(node_id, 0));
                let values = Address::new(input, self.runtime.layout.declarative_environment_binding_values as i32);
                self.masm.load64(output, &values);
                self.masm
                    .load64(output, &Address::new(output, checked_i32(8 * u64::from(*index))?));
            }
            Op::StoreEnvironmentBinding { index } => {
                let (environment, value) = (self.input(node_id, 0), self.input(node_id, 1));
                let values = self.temp(node_id, 0);
                self.masm.load64(
                    values,
                    &Address::new(
                        environment,
                        self.runtime.layout.declarative_environment_binding_values as i32,
                    ),
                );
                self.masm
                    .store64(&Address::new(values, checked_i32(8 * u64::from(*index))?), value);
            }
            Op::AppendEnvironmentBinding => {
                let environment = self.input(node_id, 0);
                self.emit_append_environment_binding(node_id, environment);
            }
            Op::BoxCell => {
                let (output, input) = (self.output(node_id), self.input(node_id, 0));
                self.box_cell(output, input);
            }
            Op::SetLexicalEnvironment => {
                let output = self.output(node_id);
                self.emit_unbox_cell(output, self.input(node_id, 0));
                let environment = self.frame_field(self.runtime.offsets.execution_context_lexical_environment);
                self.masm.store64(&environment, output);
            }
            Op::LeavePrivateEnvironment => {
                let scratch = self.pinned.scratch;
                let field = self.frame_field(self.runtime.offsets.execution_context_private_environment);
                self.masm.load64(scratch, &field);
                self.masm.load64(
                    scratch,
                    &Address::new(scratch, self.runtime.offsets.private_environment_outer as i32),
                );
                self.masm.store64(&field, scratch);
            }
            Op::IsCallable => self.emit_is_callable(node_id),
            Op::CheckClosure { shared_data } => {
                let exit = self.exit_site(node_id);
                let (value, function) = (self.input(node_id, 0), self.temp(node_id, 0));
                self.emit_closure_check(value, *shared_data, function, exit);
            }
            Op::LoadFunctionEnvironment { private } => {
                let (output, function) = (self.output(node_id), self.input(node_id, 0));
                let offset = if *private {
                    self.runtime.offsets.ecmascript_function_private_environment
                } else {
                    self.runtime.offsets.ecmascript_function_environment
                };
                self.masm.load64(output, &Address::new(function, offset as i32));
            }
            Op::CellAddress => {
                // NB: Only objects get their cell addresses taken.
                let (output, input) = (self.output(node_id), self.input(node_id, 0));
                self.emit_unbox_cell(output, input);
            }
            Op::CheckInt32 => self.emit_check_int32(node_id),
            Op::CheckNumber => self.emit_check_number(node_id),
            Op::CheckIdentityComparable => self.emit_check_identity_comparable(node_id),
            Op::BoxInt32 => {
                let (output, input) = (self.output(node_id), self.input(node_id, 0));
                self.emit_box_int32(output, input);
            }
            Op::BoxBool => {
                let (output, input) = (self.output(node_id), self.input(node_id, 0));
                self.emit_box_bool(output, input);
            }
            Op::Int32Binary { op } => self.emit_int32_binary(node_id, *op),
            Op::Int32Compare { comparison } => self.emit_int32_compare(node_id, *comparison),
            Op::CheckElements { kind } => self.emit_check_elements(node_id, *kind),
            Op::LoadTypedArrayLength => self.emit_load_typed_array_length(node_id),
            Op::CheckBounds => self.emit_check_bounds(node_id),
            Op::LoadElementAt { kind } => self.emit_load_element_at(node_id, *kind),
            Op::StoreElementAt { kind } => self.emit_store_element_at(node_id, *kind),
            Op::CheckNotHole => self.emit_check_not_hole(node_id),
            Op::TaggedEquals { equal } => {
                let (lhs, output) = (self.input(node_id, 0), self.output(node_id));
                let condition = if *equal { Condition::Equal } else { Condition::NotEqual };
                match self.input_value(node_id, 1) {
                    InputValue::Register(rhs) => self.masm.compare64_set(condition, output, lhs, rhs),
                    InputValue::Constant(bits) => self.masm.compare64_imm_set(condition, output, lhs, bits as i64),
                }
            }
            Op::ToBoolean => {
                self.masm.call_absolute(self.runtime.to_boolean);
            }
            Op::PrimitiveToString | Op::ToObject | Op::ArrayCreate => {
                let function = match node.op {
                    Op::PrimitiveToString => self.runtime.primitive_to_string,
                    Op::ToObject => self.runtime.to_object,
                    _ => self.runtime.array_create,
                };
                if function == 0 {
                    return Err(CompileFailure::MissingRuntimeHelper { opcode: None });
                }
                self.masm.move64(M::ARGUMENT_GPRS[0], self.pinned.vm);
                self.masm.call_absolute(function);
            }
            Op::Generic { opcode, executable, pc } => {
                self.emit_generic(*opcode, *executable, *pc)?;
            }
            Op::CallDirect { executable, pc, .. } | Op::CallNative { executable, pc, .. } => {
                let (executable, pc) = (*executable, *pc);
                // NB: A call in an inlined callee that does not run without
                //     the frames of its inlined calls runs in them.
                let (registers, published) = if self.publishes_frames(node_id) {
                    self.emit_publish_frames_or_save_inputs(node_id)?
                } else {
                    (InputRegisters::Saved(Vec::new()), 0)
                };
                if matches!(self.graph.node(node_id).op, Op::CallDirect { .. }) {
                    self.emit_call_direct(node_id, executable, pc, &registers)?;
                } else {
                    self.emit_call_native(node_id, executable, pc, &registers)?;
                }
                for _ in 0..published {
                    self.emit_pop_inline_frame();
                }
            }
            Op::CallForwardingArguments { executable, pc } => {
                let program_counter = self.frame_field(self.runtime.offsets.execution_context_program_counter);
                self.masm.store_imm32(&program_counter, *pc);
                self.emit_frame_runtime_call(self.runtime.call_forwarding_arguments, *pc);
                let next_pc = self.decoded(*executable, *pc)?.next_pc();
                self.emit_continuation_check(next_pc, &[]);
            }
            Op::AllocateObject { shape, reserve, .. } => {
                self.emit_allocate_object(node_id, *shape, *reserve)?;
            }
            Op::InitializeNamed { offset } => self.emit_initialize_named(node_id, *offset)?,
            Op::AllocateArray { count } => self.emit_allocate_array(node_id, *count)?,
            Op::AllocateFunction {
                shared_function_data_index,
            } => self.emit_allocate_function(node_id, *shared_function_data_index)?,
            Op::AllocateEnvironment { shape_cache, capacity } => {
                self.emit_allocate_environment(node_id, *shape_cache, *capacity)?;
            }
            Op::InitializeElement { index } => self.emit_initialize_element(node_id, *index)?,
            Op::LoadFrameField { field } => self.emit_load_frame_field(node_id, *field),
            Op::SliceArguments => {
                let exit = self.exit_site(node_id);
                let start = self.input(node_id, 0);
                debug_assert_eq!(start, M::ARGUMENT_GPRS[2]);
                let scratch = self.pinned.scratch;
                self.branch_on_tag(Condition::NotEqual, start, value::INT32_TAG, scratch, exit);
                self.masm.move64(M::ARGUMENT_GPRS[0], self.pinned.vm);
                self.masm.move64(M::ARGUMENT_GPRS[1], self.pinned.frame);
                self.masm.call_absolute(self.runtime.slice_arguments);
            }
            Op::ArgumentCount => {
                let output = self.output(node_id);
                let count = self.frame_field(self.runtime.offsets.execution_context_passed_argument_count);
                self.masm.load32(output, &count);
                self.masm.or64_imm(output, output, value::int32(0));
            }
            Op::LoadArgument { arguments_base } => self.emit_load_argument(node_id, *arguments_base)?,
            Op::Jump { target } => self.jump_to(*target),
            Op::Branch {
                condition,
                if_true,
                if_false,
            } => self.emit_branch(node_id, *condition, *if_true, *if_false),
            Op::BranchTruthy {
                if_true,
                if_false,
                fallback,
            } => self.emit_branch_truthy(node_id, *if_true, *if_false, *fallback),
            Op::BranchOnPc { pc, if_true, if_false } => {
                let input = self.input(node_id, 0);
                let label = self.label(*if_true);
                self.masm.branch32_imm(Condition::Equal, input, *pc as i32, label);
                self.jump_to(*if_false);
            }
            Op::Return => {
                debug_assert_eq!(self.input(node_id, 0), M::RETURN_GPRS[0]);
                self.emit_return(JitStatus::Returned);
            }
            Op::Exit { kind } => {
                let index = u32::try_from(self.sites.len()).expect("site count fits in u32");
                self.sites.push((node_id, SiteKind::Exit(*kind)));
                self.annotate(CodeAnnotation::Exit { index, node: node_id });
                let address = self.local_address(self.locals.exit_index);
                self.masm.store_imm32(&address, index);
                let exit_stub = self.exit_stub;
                self.masm.jump(exit_stub);
            }
            Op::Unreachable => self.masm.unreachable(),
            Op::CheckValue { expected, .. } => {
                let exit = self.exit_site(node_id);
                let scratch = self.pinned.scratch;
                self.masm.move_imm64(scratch, *expected);
                self.masm
                    .branch64(Condition::NotEqual, self.input(node_id, 0), scratch, exit);
            }
            Op::Typeof => self.emit_typeof(node_id),
            Op::TypeofIs { kind, equal } => self.emit_typeof_is(node_id, *kind, *equal),
            Op::EmptyToUndefined => {
                let output = self.output(node_id);
                let done = self.masm.new_label();
                self.masm.move64(output, self.input(node_id, 0));
                self.masm
                    .branch64_imm(Condition::NotEqual, output, value::EMPTY as i64, done);
                self.masm.move_imm64(output, value::UNDEFINED);
                self.masm.bind(done);
            }
            Op::CheckObject => {
                let exit = self.exit_site(node_id);
                let scratch = self.pinned.scratch;
                self.branch_on_tag(
                    Condition::NotEqual,
                    self.input(node_id, 0),
                    value::OBJECT_TAG,
                    scratch,
                    exit,
                );
            }
            Op::CheckShape { shapes } => self.emit_check_shape(node_id, shapes),
            Op::CheckPrototypeChainValid { validity } => {
                let exit = self.exit_site(node_id);
                let scratch = self.pinned.scratch;
                self.masm.move_imm64(scratch, validity.0);
                self.branch_unless_prototype_chain_valid(scratch, exit);
            }
            Op::AssumeValid => {
                // NB: Costs nothing while the code is valid: the runtime
                //     replaces the no-ops with a jump to the exit when it
                //     invalidates the code.
                let exit = self.exit_site(node_id);
                let offset = self.masm.patchable_nop();
                self.invalidation_points.push((offset, exit));
            }
            Op::HasInPrototypeChain => self.emit_has_in_prototype_chain(node_id),
            Op::LoadGlobalBinding { environment, index } => {
                let exit = self.exit_site(node_id);
                let output = self.output(node_id);
                let slot = self.global_binding_slot(output, *environment, *index)?;
                self.masm.load64(output, &slot);
                self.masm
                    .branch64_imm(Condition::Equal, output, value::EMPTY as i64, exit);
            }
            Op::StoreGlobalBinding { environment, index } => {
                let exit = self.exit_site(node_id);
                let values = Gpr(self.allocation.node(node_id).temps[0]);
                let slot = self.global_binding_slot(values, *environment, *index)?;
                self.masm
                    .branch64_memory_imm(Condition::Equal, &slot, value::EMPTY as i64, exit);
                self.masm.store64(&slot, self.input(node_id, 0));
            }
            Op::LoadAccessorFunction { offset, part } => {
                let exit = self.exit_site(node_id);
                let output = self.output(node_id);
                let field = self.emit_load_accessor(node_id, output, *offset, *part, exit)?;
                self.masm.load64(output, &field);
                self.masm.branch_test64(Condition::Zero, output, u64::MAX, exit);
                self.box_object(output);
            }
            Op::CheckAccessorFunction { offset, part, function } => {
                let exit = self.exit_site(node_id);
                let accessor = Gpr(self.allocation.node(node_id).temps[0]);
                let field = self.emit_load_accessor(node_id, accessor, *offset, *part, exit)?;
                let scratch = self.pinned.scratch;
                self.masm.move_imm64(scratch, function.0);
                self.masm.branch64_memory(Condition::NotEqual, &field, scratch, exit);
            }
            Op::LoadNamed { offset } => {
                let exit = self.exit_site(node_id);
                let output = self.output(node_id);
                let slot = self.named_property_address(output, *offset)?;
                let cell = self.cell_address_of(node_id, output, 1);
                let storage = Address::new(cell, self.runtime.offsets.object_named_properties as i32);
                self.masm.load64(output, &storage);
                self.emit_exit_if_accessor(&slot, exit);
                self.masm.load64(output, &slot);
            }
            Op::StoreNamed { offset } => {
                let exit = self.exit_site(node_id);
                let storage = Gpr(self.allocation.node(node_id).temps[0]);
                let slot = self.named_property_address(storage, *offset)?;
                let cell = self.cell_address_of(node_id, storage, 2);
                let storage_field = Address::new(cell, self.runtime.offsets.object_named_properties as i32);
                self.masm.load64(storage, &storage_field);
                self.emit_exit_if_accessor(&slot, exit);
                self.masm.store64(&slot, self.input(node_id, 1));
            }
            Op::AddNamed { offset, shape, .. } => self.emit_add_named(node_id, *offset, *shape)?,
            Op::ShapeSwitch { cases } => {
                let exit = self.exit_site(node_id);
                self.emit_load_shape_of(node_id, self.pinned.scratch, 1);
                for (shape, block) in cases {
                    let label = self.label(*block);
                    self.emit_branch_on_shape(*shape, label, exit);
                }
                self.masm.jump(exit);
            }
            Op::ProbePropertyCache { executable, cache } => {
                self.emit_probe_property_cache(node_id, *executable, *cache)?;
            }
            Op::ProbeKeyedCache { executable, cache } => self.emit_probe_keyed_cache(node_id, *executable, *cache)?,
            Op::ProbeKeyedStore { executable, cache } => self.emit_probe_keyed_store(node_id, *executable, *cache)?,
            Op::ProbeGlobalCache { cache } => self.emit_probe_global_cache(node_id, *cache)?,
            Op::ProbeHasProperty { own } => self.emit_probe_has_property(node_id, *own),
            Op::ProbeGlobalStore { cache } => self.emit_probe_global_store(node_id, *cache)?,
            Op::ProbePropertyStore { executable, cache } => {
                self.emit_probe_property_store(node_id, *executable, *cache)?;
            }
            Op::CallSlowPath {
                opcode,
                executable,
                saves_registers: true,
                ..
            } => self.emit_call_slow_path(slow_path_calls::SlowPath {
                node: node_id,
                opcode: *opcode,
                executable: *executable,
            })?,
            Op::UnboxInt32 => {
                let (output, input) = (self.output(node_id), self.input(node_id, 0));
                self.masm.move32(output, input);
            }
            Op::UnboxDouble => self.emit_unbox_double(node_id),
            Op::Int32Abs => {
                let (output, input) = (self.output(node_id), self.input(node_id, 0));
                let positive = self.masm.new_label();
                self.masm.move32(output, input);
                self.masm
                    .branch32_imm(Condition::GreaterThanOrEqual, output, 0, positive);
                self.masm.neg32(output, output);
                self.masm.bind(positive);
            }
            Op::Int32ToFloat64 => {
                let (output, input) = (self.float_output(node_id), self.input(node_id, 0));
                self.masm.convert_int32_to_double(output, input);
            }
            Op::BoxFloat64 => self.emit_box_float64(node_id),
            Op::Float64Unary { op } => self.emit_float64_unary(node_id, *op),
            Op::Float64Binary { op } => self.emit_float64_binary(node_id, *op),
            Op::Float64Compare { comparison } => self.emit_float64_compare(node_id, *comparison),
            Op::Float64ToInt32 => self.emit_float64_to_int32(node_id),
            Op::Uint32ShiftRight => self.emit_uint32_shift_right(node_id),
            Op::Uint32ToFloat64 => self.emit_uint32_to_float64(node_id),
            Op::StringsEqual => self.emit_strings_equal(node_id),
            Op::ConcatenateStrings => self.emit_concatenate_strings(node_id),
            Op::IntegerToString => self.emit_integer_to_string(node_id),
            Op::StringAddress => {
                let (output, input) = (self.output(node_id), self.input(node_id, 0));
                self.emit_unbox_cell(output, input);
            }
            Op::StringLength => self.emit_string_length(node_id),
            Op::LoadStringCodeUnit => self.emit_load_string_code_unit_node(node_id),
            Op::SingleCharacterString => self.emit_single_character_string(node_id),
            Op::CheckAppendableArray => self.emit_check_appendable_array(node_id),
            Op::LoadElementsLength => {
                let (output, object) = (self.output(node_id), self.input(node_id, 0));
                let size = self.runtime.layout.object_indexed_array_like_size as i32;
                self.masm.load32(output, &Address::new(object, size));
            }
            Op::LoadElementsCapacity => self.emit_load_elements_capacity(node_id),
            Op::AppendElement => self.emit_append_element_node(node_id),
            Op::CallArrayPush => self.emit_call_array_push(node_id),
            Op::SlowPathOutput { index } => self.emit_slow_path_output(node_id, *index)?,
            Op::LoadPropertyIteratorKeyCount => self.emit_load_property_iterator_key_count(node_id),
            Op::LoadPropertyIteratorKey => self.emit_load_property_iterator_key(node_id),
            Op::CallSlowPath {
                opcode,
                executable,
                pc,
                saves_registers: false,
            } => self.emit_slow_path_call_on_values(node_id, *opcode, *executable, *pc)?,
        }

        if let (Some(Location::Register(register)), Some(slot)) = (allocation.output, allocation.spill_slot) {
            self.emit_move(&Move {
                from: Location::Register(register),
                to: Location::Stack(slot),
            });
        }
        Ok(())
    }

    /// The slots of the compiled function's frame, as the frame state of
    /// `node` describes it, that hold the arguments object the code never
    /// created, and whether it is a mapped one.
    fn virtual_arguments_slots(&self, node: NodeId) -> Vec<(u32, bool)> {
        self.graph
            .node(node)
            .frame_state
            .map(|frame_state| {
                // NB: Only the compiled function's own frame, the outermost
                //     one, keeps an arguments object virtual.
                let root = *self
                    .graph
                    .frame_state_chain(frame_state)
                    .last()
                    .expect("the chain is not empty");
                self.graph
                    .frame_state(root)
                    .values
                    .iter()
                    .filter_map(|(slot, value)| {
                        let bits = self.graph.constant_value(*value)?;
                        value::virtual_arguments_kind(bits).map(|mapped| (*slot, mapped))
                    })
                    .collect()
            })
            .unwrap_or_default()
    }

    fn emit_has_in_prototype_chain(&mut self, node: NodeId) {
        let exit = self.exit_site(node);
        let (value, prototype) = (self.input(node, 0), self.input(node, 1));
        let output = self.output(node);
        let object = Gpr(self.allocation.node(node).temps[0]);
        let scratch = self.pinned.scratch;
        let layout = self.runtime.layout;
        let offsets = self.runtime.offsets;
        let (walk, found, not_found, done) = (
            self.masm.new_label(),
            self.masm.new_label(),
            self.masm.new_label(),
            self.masm.new_label(),
        );
        if layout.class_object_methods == 0 {
            self.masm.jump(exit);
            return;
        }
        // OrdinaryHasInstance throws unless the prototype is an object, and
        // values that are not objects have no prototype chain.
        self.branch_on_tag(Condition::NotEqual, prototype, value::OBJECT_TAG, scratch, exit);
        self.branch_on_tag(Condition::NotEqual, value, value::OBJECT_TAG, scratch, not_found);
        self.emit_unbox_cell(object, value);
        self.masm.bind(walk);
        // Only objects with the ordinary [[GetPrototypeOf]] have the
        // prototype of their shape.
        self.masm.load64(scratch, &Address::new(object, 0));
        self.masm
            .load64(scratch, &Address::new(scratch, layout.class_object_methods as i32));
        self.masm.branch64_memory_imm(
            Condition::NotEqual,
            &Address::new(scratch, layout.object_methods_get_prototype_of as i32),
            0,
            exit,
        );
        self.masm
            .load64(object, &Address::new(object, offsets.object_shape as i32));
        self.masm
            .load64(object, &Address::new(object, layout.shape_prototype as i32));
        self.masm.branch_test64(Condition::Zero, object, u64::MAX, not_found);
        self.emit_unbox_cell(output, prototype);
        self.masm.branch64(Condition::Equal, object, output, found);
        self.masm.jump(walk);
        self.masm.bind(found);
        self.masm.move_imm32(output, 1);
        self.masm.jump(done);
        self.masm.bind(not_found);
        self.masm.move_imm32(output, 0);
        self.masm.bind(done);
    }

    /// Whether objects the code meets may have the `[[IsHTMLDDA]]` internal
    /// slot, unless the code depends on no such object existing (see
    /// `GraphBuilder::assume_no_htmldda_objects()`).
    pub(super) fn htmldda_objects_may_exist(&self) -> bool {
        !self
            .graph
            .dependencies
            .contains(&crate::code::Dependency::NoHtmlDdaObjects)
    }

    /// The address of binding `index` of the global declarative environment
    /// `environment`, whose binding values `values` gets.
    fn global_binding_slot(&mut self, values: Gpr, environment: CellId, index: u32) -> Result<Address, CompileFailure> {
        self.masm.move_imm64(values, environment.0);
        let field = Address::new(
            values,
            self.runtime.layout.declarative_environment_binding_values as i32,
        );
        self.masm.load64(values, &field);
        Ok(Address::new(values, checked_i32(8 * u64::from(index))?))
    }

    /// The address of named property `offset` in the storage at `storage`.
    fn named_property_address(&self, storage: Gpr, offset: u32) -> Result<Address, CompileFailure> {
        Ok(Address::new(storage, checked_i32(8 * u64::from(offset))?))
    }

    /// The register holding the address of the object in input 0 of
    /// `node`: its `CellAddress` input at `address_input`, if it has one, or
    /// `temp`, which gets the decoded address.
    fn cell_address_of(&mut self, node: NodeId, temp: Gpr, address_input: usize) -> Gpr {
        if self.graph.node(node).inputs.len() > address_input {
            return self.input(node, address_input);
        }
        self.emit_unbox_cell(temp, self.input(node, 0));
        temp
    }

    /// `dst` = the shape of the object in input 0 of `node`, whose
    /// `CellAddress` may be input `address_input`.
    fn emit_load_shape_of(&mut self, node: NodeId, dst: Gpr, address_input: usize) {
        let cell = self.cell_address_of(node, dst, address_input);
        let shape = Address::new(cell, self.runtime.offsets.object_shape as i32);
        self.masm.load64(dst, &shape);
    }
    /// `StoreSlot`: writes input 0, boxed if it is unboxed, to frame slot
    /// `slot`.
    fn emit_store_slot(&mut self, node_id: NodeId, slot: u32) -> Result<(), CompileFailure> {
        let address = self.slot_address(slot)?;
        let Location::Register(value) = self.allocation.node(node_id).inputs[0] else {
            unreachable!("inputs are in registers");
        };
        match self.graph.node(self.graph.node(node_id).inputs[0]).repr {
            Some(repr) if repr != crate::code::Repr::Tagged => {
                let scratch = self.pinned.scratch;
                self.emit_box_register(scratch, value, repr);
                self.masm.store64(&address, scratch);
            }
            _ => self.masm.store64(&address, Gpr(value)),
        }
        Ok(())
    }

    /// `LoadFrameField`: a field of the running execution context.
    fn emit_load_frame_field(&mut self, node_id: NodeId, field: FrameField) {
        let offsets = self.runtime.offsets;
        let offset = match field {
            FrameField::LexicalEnvironment => offsets.execution_context_lexical_environment,
            FrameField::VariableEnvironment => offsets.execution_context_variable_environment,
            FrameField::PrivateEnvironment => offsets.execution_context_private_environment,
            FrameField::Realm => offsets.execution_context_realm,
            FrameField::Executable => offsets.execution_context_executable,
        };
        let output = self.output(node_id);
        self.masm.load64(output, &self.frame_field(offset));
    }

    /// `LoadArgument`: the passed argument at the int32 index input 0, exiting
    /// for other indices and those past the passed argument count.
    fn emit_load_argument(&mut self, node_id: NodeId, arguments_base: u32) -> Result<(), CompileFailure> {
        let exit = self.exit_site(node_id);
        let (index, output, scratch) = (self.input(node_id, 0), self.output(node_id), self.pinned.scratch);
        self.branch_on_tag(Condition::NotEqual, index, value::INT32_TAG, scratch, exit);
        let count = self.frame_field(self.runtime.offsets.execution_context_passed_argument_count);
        self.masm.load32(scratch, &count);
        self.masm.move32(output, index);
        // Negative indices are above every count.
        self.masm.branch32(Condition::AboveOrEqual, output, scratch, exit);
        let first =
            checked_i32(u64::from(self.runtime.offsets.execution_context_slots) + 8 * u64::from(arguments_base))?;
        let address = Address::indexed(self.pinned.frame, output, crate::asm::Scale::Eight, first);
        self.masm.load64(output, &address);
        Ok(())
    }

    /// `CheckShape`: exits unless the object has one of `shapes`.
    fn emit_check_shape(&mut self, node_id: NodeId, shapes: &[ShapeCheck]) {
        let exit = self.exit_site(node_id);
        self.emit_load_shape_of(node_id, self.pinned.scratch, 1);
        match shapes {
            [shape] if shape.dictionary_generation.is_none() => {
                self.masm
                    .branch64_imm(Condition::NotEqual, self.pinned.scratch, shape.shape.0 as i64, exit);
            }
            _ => {
                let matched = self.masm.new_label();
                for shape in shapes {
                    self.emit_branch_on_shape(*shape, matched, exit);
                }
                self.masm.jump(exit);
                self.masm.bind(matched);
            }
        }
    }

    /// `AddNamed`: gives the object `shape` and stores input 1 into its new
    /// named property at `offset`.
    fn emit_add_named(&mut self, node_id: NodeId, offset: u32, shape: CellId) -> Result<(), CompileFailure> {
        // NB: Nothing can collect garbage between giving the object
        //     the shape with the property and storing its value.
        let storage = Gpr(self.allocation.node(node_id).temps[0]);
        let cell = self.cell_address_of(node_id, storage, 2);
        let offsets = self.runtime.offsets;
        self.masm.move_imm64(self.pinned.scratch, shape.0);
        self.masm
            .store64(&Address::new(cell, offsets.object_shape as i32), self.pinned.scratch);
        self.masm
            .load64(storage, &Address::new(cell, offsets.object_named_properties as i32));
        let slot = self.named_property_address(storage, offset)?;
        self.masm.store64(&slot, self.input(node_id, 1));
        Ok(())
    }

    /// Branches to `matched` if the shape in the scratch register is
    /// `shape`, or to `exit` if it is `shape` but its dictionary generation
    /// changed. Clobbers the scratch register only in the latter case.
    fn emit_branch_on_shape(&mut self, shape: ShapeCheck, matched: Label, exit: Label) {
        let scratch = self.pinned.scratch;
        let Some(generation) = shape.dictionary_generation else {
            self.masm
                .branch64_imm(Condition::Equal, scratch, shape.shape.0 as i64, matched);
            return;
        };
        let other_shape = self.masm.new_label();
        self.masm
            .branch64_imm(Condition::NotEqual, scratch, shape.shape.0 as i64, other_shape);
        let generation_address = Address::new(scratch, self.runtime.offsets.shape_dictionary_generation as i32);
        self.masm.load32(scratch, &generation_address);
        self.masm
            .branch32_imm(Condition::Equal, scratch, generation as i32, matched);
        self.masm.jump(exit);
        self.masm.bind(other_shape);
    }

    /// Branches to `exit` if `value` is an accessor.
    fn emit_exit_if_accessor(&mut self, slot: &Address, exit: Label) {
        // NB: The tag is in the top 16 bits of the value in the slot.
        let tag = Address {
            displacement: slot.displacement + 6,
            ..*slot
        };
        self.masm
            .branch16_memory_imm(Condition::Equal, &tag, value::ACCESSOR_TAG, exit);
    }

    /// Loads the accessor in the named property at `offset` of the object
    /// that is input 1 of `node` into `dst`, exiting to `exit` where the
    /// property holds no accessor, and returns the address of its getter or
    /// setter.
    fn emit_load_accessor(
        &mut self,
        node: NodeId,
        dst: Gpr,
        offset: u32,
        part: crate::ir::AccessorPart,
        exit: Label,
    ) -> Result<Address, CompileFailure> {
        let cell = self.cell_address_of(node, dst, 1);
        let storage = Address::new(cell, self.runtime.offsets.object_named_properties as i32);
        self.masm.load64(dst, &storage);
        let slot = self.named_property_address(dst, offset)?;
        self.masm.load64(dst, &slot);
        self.branch_on_tag(Condition::NotEqual, dst, value::ACCESSOR_TAG, self.pinned.scratch, exit);
        self.emit_unbox_cell(dst, dst);
        let field = match part {
            crate::ir::AccessorPart::Getter => self.runtime.offsets.accessor_getter,
            crate::ir::AccessorPart::Setter => self.runtime.offsets.accessor_setter,
        };
        Ok(Address::new(dst, field as i32))
    }

    /// `dst = heap_region_base + (value & offset_mask)`, the cell pointer of
    /// a cell value, which is in the heap region whatever the value is.
    fn emit_unbox_cell(&mut self, dst: Gpr, value: Gpr) {
        self.masm.and64_imm(dst, value, self.runtime.heap_region_offset_mask);
        self.masm.add64_imm(dst, dst, self.runtime.heap_region_base as i64);
    }

    /// The `Enter` bytecode: empty registers and locals, constants copied
    /// into the frame, and the frame marked initialized (and published).
    fn emit_initialize_frame(&mut self, node: NodeId) -> Result<(), CompileFailure> {
        let temps = &self.allocation.node(node).temps;
        let (value, cursor) = (Gpr(temps[0]), Gpr(temps[1]));
        self.emit_frame_initialization(value, Some(cursor))
    }

    /// `EnsureFrameInitialized`: initializes the frame unless it is.
    fn emit_ensure_frame_initialized(&mut self, node: NodeId) -> Result<(), CompileFailure> {
        let temps = &self.allocation.node(node).temps;
        let (value, cursor) = (Gpr(temps[0]), Gpr(temps[1]));
        let done = self.masm.new_label();
        self.branch_if_frame_initialized(done);
        self.emit_frame_initialization(value, Some(cursor))?;
        self.masm.bind(done);
        Ok(())
    }

    fn branch_if_frame_initialized(&mut self, target: Label) {
        let scratch = self.pinned.scratch;
        let flag = self.frame_field(self.runtime.offsets.execution_context_frame_initialized);
        self.masm.load8(scratch, &flag);
        self.masm.branch_test32(Condition::NonZero, scratch, 0xFF, target);
    }

    /// `PublishFrame`: makes the frame the running execution context, which
    /// it may be already.
    fn emit_publish_frame(&mut self) {
        let running = Address::new(self.pinned.vm, self.runtime.offsets.vm_running_execution_context as i32);
        self.masm.store64(&running, self.pinned.frame);
    }

    /// Publishes the frame and initializes it like `Enter`, with `value` for
    /// the stored values, and with a loop using `cursor` for large frames if
    /// there is one. Clobbers the scratch register.
    fn emit_frame_initialization(&mut self, value: Gpr, cursor: Option<Gpr>) -> Result<(), CompileFailure> {
        self.emit_publish_frame();
        // Up to this many registers and locals are emptied with one store
        // each, more in a loop.
        const UNROLLED_SLOTS: u32 = 32;
        let layout = self.executables[0].layout;
        let slots = self.runtime.offsets.execution_context_slots;
        let first = crate::bytecode::RESERVED_REGISTER_COUNT;
        let mut fields: Vec<call::ConstantField> = Vec::new();
        let unrolled = layout.registers_and_locals_count.saturating_sub(first) <= UNROLLED_SLOTS;
        if let (false, Some(cursor)) = (unrolled, cursor) {
            self.masm.move_imm64(value, value::EMPTY);
            let start = self.slot_address(first)?;
            let end = self.slot_address(layout.registers_and_locals_count)?;
            self.masm.load_effective_address(cursor, &start);
            self.masm.load_effective_address(self.pinned.scratch, &end);
            let again = self.masm.new_label();
            self.masm.bind(again);
            self.masm.store64(&Address::new(cursor, 0), value);
            self.masm.add64_imm(cursor, cursor, 8);
            self.masm.branch64(Condition::Below, cursor, self.pinned.scratch, again);
        } else {
            for index in first..layout.registers_and_locals_count {
                fields.push((slots + 8 * index, 8, value::EMPTY));
            }
        }
        for (index, constant) in (0..).zip(&self.executables[0].constants) {
            fields.push((slots + 8 * (layout.constants_base() + index), 8, *constant));
        }
        fields.push((self.runtime.offsets.execution_context_frame_initialized, 1, 1));
        self.emit_constant_fields(self.pinned.frame, value, &mut fields)
    }

    /// Whether the input is a function object, as a boolean.
    fn emit_is_callable(&mut self, node: NodeId) {
        let value = self.input(node, 0);
        let output = self.output(node);
        let scratch = self.pinned.scratch;
        let not_callable = self.masm.new_label();
        let done = self.masm.new_label();
        self.branch_on_tag(Condition::NotEqual, value, value::OBJECT_TAG, scratch, not_callable);
        self.emit_unbox_cell(scratch, value);
        self.masm.load16(
            scratch,
            &Address::new(scratch, self.runtime.offsets.object_flags as i32),
        );
        self.masm.branch_test32(
            Condition::Zero,
            scratch,
            u32::from(self.runtime.object_flag_is_function),
            not_callable,
        );
        self.masm.move_imm64(output, value::TRUE);
        self.masm.jump(done);
        self.masm.bind(not_callable);
        self.masm.move_imm64(output, value::FALSE);
        self.masm.bind(done);
    }

    /// Branches to `if_true` where `condition` holds, and to `if_false`
    /// otherwise. Where `if_true` comes next, the inverted condition branches
    /// to `if_false` instead, so that control falls through to `if_true`.
    fn emit_branch(&mut self, node: NodeId, condition: BranchCondition, if_true: BlockId, if_false: BlockId) {
        let true_landing = self.landing_blocks[if_true.index()];
        let invert = self.next_block() == Some(true_landing) && self.landing_blocks[if_false.index()] != true_landing;
        let target = if invert {
            self.label(if_false)
        } else {
            self.label(if_true)
        };
        if let BranchCondition::Float64(comparison) = condition {
            let condition = float64::double_condition(comparison);
            let condition = if invert { condition.invert() } else { condition };
            let (lhs, rhs) = (self.float_input(node, 0), self.float_input(node, 1));
            self.masm.branch_double(condition, lhs, rhs, target);
            if !invert {
                self.jump_to(if_false);
            }
            return;
        }
        let input = self.input(node, 0);
        let when = |condition: Condition| if invert { condition.invert() } else { condition };
        let scratch = self.pinned.scratch;
        match condition {
            BranchCondition::Bool => self.masm.branch_test64(when(Condition::NonZero), input, 1, target),
            BranchCondition::Int32(comparison) => {
                let condition = when(speculation::int32_condition(comparison));
                match self.input_value(node, 1) {
                    InputValue::Register(rhs) => self.masm.branch32(condition, input, rhs, target),
                    InputValue::Constant(bits) => self.masm.branch32_imm(condition, input, bits as i32, target),
                }
            }
            BranchCondition::TaggedEquals { equal } => {
                let condition = when(if equal { Condition::Equal } else { Condition::NotEqual });
                match self.input_value(node, 1) {
                    InputValue::Register(rhs) => self.masm.branch64(condition, input, rhs, target),
                    InputValue::Constant(bits) => self.masm.branch64_imm(condition, input, bits as i64, target),
                }
            }
            BranchCondition::Undefined => {
                self.masm
                    .branch64_imm(when(Condition::Equal), input, value::UNDEFINED as i64, target);
            }
            BranchCondition::Nullish => {
                // Undefined and null only differ in the lowest tag bit.
                self.masm.shr64_imm(scratch, input, 48);
                self.masm.or32_imm(scratch, scratch, 1);
                self.masm
                    .branch32_imm(when(Condition::Equal), scratch, i32::from(value::NULL_TAG), target);
            }
            BranchCondition::Object => {
                self.branch_on_tag(when(Condition::Equal), input, value::OBJECT_TAG, scratch, target);
            }
            BranchCondition::ElementsKind(kind) => self.emit_branch_on_elements_kind(node, kind, target, invert),
            BranchCondition::IndexInBounds => {
                let count = self.input(node, 1);
                self.masm.branch32(when(Condition::Below), input, count, target);
            }
            BranchCondition::Shape(_) => unreachable!("only refinements of shape switch cases test one shape"),
            BranchCondition::Float64(_) => unreachable!("float64 comparisons branch above"),
            BranchCondition::MagicalLength | BranchCondition::Extensible => {
                let flag = match condition {
                    BranchCondition::MagicalLength => self.runtime.layout.object_flag_has_magical_length,
                    _ => self.runtime.layout.object_flag_is_extensible,
                };
                self.masm
                    .load16(scratch, &Address::new(input, self.runtime.offsets.object_flags as i32));
                self.masm
                    .branch_test32(when(Condition::NonZero), scratch, u32::from(flag), target);
            }
            BranchCondition::NonNegativeInt32 => {
                // NB: The int32 tag, then zeros down to the sign bit.
                self.masm.shr64_imm(scratch, input, 31);
                self.masm.branch64_imm(
                    when(Condition::Equal),
                    scratch,
                    (u64::from(value::INT32_TAG) << 17) as i64,
                    target,
                );
            }
            BranchCondition::Int32Value => {
                self.branch_on_tag(when(Condition::Equal), input, value::INT32_TAG, scratch, target);
            }
            BranchCondition::String => {
                self.branch_on_tag(when(Condition::Equal), input, value::STRING_TAG, scratch, target);
            }
            BranchCondition::Double
            | BranchCondition::ResolvedString
            | BranchCondition::Builtin(_)
            | BranchCondition::PropertyIteratorCacheValid
            | BranchCondition::BindingMutable { .. }
            | BranchCondition::NextBindingOfShape { .. } => {
                // NB: These take several tests, which go to `yes` or `no`.
                let other = self.masm.new_label();
                let (yes, no) = if invert { (other, target) } else { (target, other) };
                match condition {
                    BranchCondition::Double => self.emit_double_test(input, yes, no),
                    BranchCondition::ResolvedString => self.emit_resolved_string_test(input, yes, no),
                    BranchCondition::Builtin(id) => self.emit_builtin_test(node, input, id, yes, no),
                    BranchCondition::PropertyIteratorCacheValid => {
                        let keys = self.input(node, 1);
                        self.emit_property_iterator_cache_test(node, input, keys, yes, no);
                    }
                    BranchCondition::BindingMutable { index } => {
                        let (index_register, temp) = (self.temp(node, 0), self.temp(node, 1));
                        self.masm.move_imm32(index_register, index);
                        self.branch_unless_binding_mutable(input, index_register, temp, no);
                        self.masm.jump(yes);
                    }
                    BranchCondition::NextBindingOfShape { name, flags } => {
                        self.emit_next_binding_of_shape_test(node, input, name, flags, no);
                        self.masm.jump(yes);
                    }
                    _ => unreachable!(),
                }
                self.masm.bind(other);
            }
        }
        if !invert {
            self.jump_to(if_false);
        }
    }

    fn emit_branch_truthy(&mut self, node: NodeId, if_true: BlockId, if_false: BlockId, fallback: BlockId) {
        let input = self.input(node, 0);
        let scratch = self.pinned.scratch;
        let (true_label, false_label, fallback_label) =
            (self.label(if_true), self.label(if_false), self.label(fallback));
        let boolean = self.masm.new_label();
        let other = self.masm.new_label();
        self.branch_on_tag(Condition::Equal, input, value::BOOLEAN_TAG, scratch, boolean);
        self.masm
            .branch32_imm(Condition::NotEqual, scratch, i32::from(value::INT32_TAG), other);
        self.masm.branch_test32(Condition::NonZero, input, u32::MAX, true_label);
        self.masm.jump(false_label);
        self.masm.bind(other);
        self.emit_truthiness_of_other_values(input, true_label, false_label, fallback_label);
        self.masm.bind(boolean);
        self.masm.branch_test64(Condition::NonZero, input, 1, true_label);
        self.jump_to(if_false);
    }

    // Generic nodes.

    fn load_value(&mut self, dst: Gpr, source: ValueSource) -> Result<(), CompileFailure> {
        match source {
            ValueSource::Slot(operand) => self.load_raw_operand(dst, operand.raw())?,
            ValueSource::Int32(integer) => self.masm.move_imm64(dst, value::int32(integer)),
        }
        Ok(())
    }

    /// Loads frame slot `operand` (a raw operand from the bytecode, which may
    /// be absent) into `dst`, or the empty value if it is absent.
    fn load_raw_operand(&mut self, dst: Gpr, operand: u32) -> Result<(), CompileFailure> {
        if operand == crate::bytecode::Operand::INVALID {
            self.masm.move_imm64(dst, value::EMPTY);
            return Ok(());
        }
        if let Some(values) = &self.slow_path_values
            && let Some(index) = values.operands.iter().position(|input| *input == operand)
        {
            let (node, registers) = (values.node, values.registers.clone());
            self.load_input(dst, node, index, &registers);
            return Ok(());
        }
        let address = self.slot_address(operand)?;
        self.masm.load64(dst, &address);
        Ok(())
    }

    /// Runs the slow path of the instruction at `pc` of `executable`, with
    /// its operands in their frame slots or, while `slow_path_values` is
    /// set, in the node's inputs. Returns where the outputs that are SSA
    /// values are, in layout order.
    fn emit_generic(
        &mut self,
        opcode: crate::bytecode::OpCode,
        executable: u32,
        pc: u32,
    ) -> Result<Vec<OutputSource>, CompileFailure> {
        if opcode == crate::bytecode::OpCode::Call && self.makes_dynamic_calls() {
            self.emit_generic_js_call(executable, pc)?;
            return Ok(Vec::new());
        }
        let decoded = self.decoded(executable, pc)?;
        let next_pc = decoded.next_pc();
        let call = slow_path_call(opcode).expect("generic nodes have a slow path");
        let address = match call {
            SlowPathCall::JitCall => self.runtime.jit_call,
            _ => self.runtime.slow_paths.get(opcode as usize).copied().unwrap_or(0),
        };
        if address == 0 {
            return Err(CompileFailure::MissingRuntimeHelper { opcode: Some(opcode) });
        }

        let program_counter = self.frame_field(self.runtime.offsets.execution_context_program_counter);
        self.masm.store_imm32(&program_counter, pc);
        let arguments = M::ARGUMENT_GPRS;
        let mut ssa_outputs = Vec::new();
        match call {
            SlowPathCall::JitCall => {
                self.emit_frame_runtime_call(address, pc);
                self.emit_continuation_check(next_pc, &[]);
            }
            SlowPathCall::BinaryValues => {
                let (dst, lhs, rhs) = binary_operands(&decoded.instruction).expect("binary slow path operands");
                let into_record = self
                    .slow_path_values
                    .as_ref()
                    .is_some_and(|values| values.ssa_outputs.contains(&dst.raw()));
                let destination = if into_record {
                    ssa_outputs.push(OutputSource::Record(self.locals.record));
                    self.local_address(self.locals.record)
                } else {
                    self.slot_address(dst.raw())?
                };
                self.masm.load_effective_address(arguments[2], &destination);
                self.load_value(arguments[3], lhs)?;
                self.load_value(arguments[4], rhs)?;
                self.masm.move64(arguments[0], self.pinned.vm);
                self.masm.move_imm32(arguments[1], pc);
                self.masm.call_absolute(address);
                self.emit_continuation_check(next_pc, &[]);
            }
            SlowPathCall::JumpValues => {
                let (lhs, rhs, true_target, false_target) =
                    jump_operands(&decoded.instruction).expect("jump slow path operands");
                self.load_value(arguments[2], lhs)?;
                self.load_value(arguments[3], rhs)?;
                self.masm.move_imm32(arguments[4], true_target);
                self.masm.move_imm32(arguments[5], false_target);
                self.masm.move64(arguments[0], self.pinned.vm);
                self.masm.move_imm32(arguments[1], pc);
                self.masm.call_absolute(address);
                self.emit_jump_target_check(pc, true_target, false_target);
            }
            SlowPathCall::Record => {
                ssa_outputs = self.emit_record_call(opcode, executable, pc, next_pc, address, None)?;
            }
        }
        Ok(ssa_outputs)
    }

    /// The operands a slow path call of the instruction at `pc` reads, in
    /// the order of the node's inputs, and the outputs it writes that are SSA
    /// values.
    fn slow_path_call_operands(
        &self,
        opcode: crate::bytecode::OpCode,
        executable: u32,
        pc: u32,
    ) -> Result<(Vec<u32>, Vec<u32>), CompileFailure> {
        let mut operands = Vec::new();
        let mut outputs = Vec::new();
        let is_js_call = slow_path_call(opcode) == Some(SlowPathCall::JitCall);
        if is_js_call {
            let decoded = self.decoded(executable, pc)?;
            let call_operands = crate::builder::js_call_operands(&decoded.instruction);
            operands = call_operands.inputs;
            outputs = call_operands.outputs.into_iter().map(|(_, operand)| operand).collect();
        } else {
            let layout = slow_path_layout(opcode);
            for field in layout.fields {
                let operand = self.read_u32(executable, pc, field.instruction_offset)?;
                if field.role != OperandRole::Out {
                    operands.push(operand);
                }
                if field.role != OperandRole::In && operand != crate::bytecode::Operand::INVALID {
                    outputs.push(operand);
                }
            }
            if let Some(array) = layout.array {
                for element in 0..self.read_u32(executable, pc, array.count_offset)? {
                    operands.push(self.read_u32(executable, pc, array.instruction_offset + 4 * element)?);
                }
            }
        }
        let frame_layout = self.executables[executable as usize].layout;
        let ssa_outputs = outputs
            .into_iter()
            .filter(|operand| {
                crate::builder::is_ssa_slow_path_output(
                    &frame_layout,
                    crate::bytecode::Operand::from_raw(*operand),
                    executable != 0,
                )
            })
            .collect::<Vec<_>>();
        Ok((operands, ssa_outputs))
    }

    /// A `CallSlowPath` node that is a call: runs the slow path of its
    /// instruction like a `Generic` node does, with the node's inputs as the
    /// operands, and puts the instruction's first output into the node's
    /// output register.
    fn emit_slow_path_call_on_values(
        &mut self,
        node: NodeId,
        opcode: crate::bytecode::OpCode,
        executable: u32,
        pc: u32,
    ) -> Result<(), CompileFailure> {
        let is_js_call = slow_path_call(opcode) == Some(SlowPathCall::JitCall);
        let (operands, ssa_outputs) = self.slow_path_call_operands(opcode, executable, pc)?;
        // NB: In an inlined callee, the slow path runs in the frames of the
        //     inlined calls, which compiled code pops again if it continues.
        let (registers, published) = self.emit_publish_frames_or_save_inputs(node)?;
        // NB: The runtime's call helper and the call stub read the operands
        //     of calls from the frame, and calls leave their result there.
        if is_js_call {
            self.emit_store_operand_inputs(node, &operands, &registers)?;
            self.emit_generic(opcode, executable, pc)?;
            if let (Some(destination), Some(Location::Register(output))) =
                (ssa_outputs.first(), self.allocation.node(node).output)
            {
                let address = self.slot_address(*destination)?;
                self.masm.load64(Gpr(output), &address);
            }
            for _ in 0..published {
                self.emit_pop_inline_frame();
            }
            return Ok(());
        }
        self.slow_path_values = Some(SlowPathValues {
            node,
            operands,
            registers,
            ssa_outputs,
        });
        let sources = self.emit_generic(opcode, executable, pc);
        self.slow_path_values = None;
        let sources = sources?;
        // NB: The value of a conditional jump is the control word, which is
        //     in the output register already.
        if let (Some(source), Some(Location::Register(output))) = (sources.first(), self.allocation.node(node).output) {
            let output = Gpr(output);
            match source {
                OutputSource::Register(register) => self.masm.move64(output, *register),
                OutputSource::Record(offset) => {
                    let record = self.local_address(*offset);
                    self.masm.load64(output, &record);
                }
            }
        }
        for _ in 0..published {
            self.emit_pop_inline_frame();
        }
        Ok(())
    }

    /// Stores the registers of the inputs of the slow path or call `node`
    /// that calls clobber in the saves area, and returns them: marshalling
    /// operands into argument registers and publishing frames clobbers them,
    /// so the call reads those inputs from there.
    fn save_input_registers(&mut self, node: NodeId) -> Vec<Gpr> {
        let clobbered = slow_path_calls::slow_path_saved_registers::<M>();
        let mut saved = Vec::new();
        for location in &self.allocation.node(node).inputs {
            if let Location::Register(register) = location
                && let Register::General(register) = register_of(*register)
                && clobbered.contains(register)
                && !saved.contains(&register)
            {
                saved.push(register);
            }
        }
        self.emit_save_registers(&saved);
        saved
    }

    /// Before the slow path call or call `node`: publishes the frames of the
    /// inlined calls it is in, if it runs in them (see
    /// `emit_publish_frames()`), and otherwise saves the registers of its
    /// inputs that marshalling its operands may clobber. Returns where its
    /// inputs in registers are then, and how many frames were published.
    fn emit_publish_frames_or_save_inputs(&mut self, node: NodeId) -> Result<(InputRegisters, usize), CompileFailure> {
        if !self.publishes_frames(node) {
            return Ok((InputRegisters::Saved(self.save_input_registers(node)), 0));
        }
        // NB: Publishing uses the registers a call clobbers, which the call
        //     clobbers anyway.
        let saved = slow_path_calls::slow_path_saved_registers::<M>()
            .iter()
            .collect::<Vec<_>>();
        self.emit_save_registers(&saved);
        let (published, registers) = self.emit_publish_frames(node, &saved)?;
        Ok((registers, published))
    }

    /// Stores the values of the inputs of `node` to the frame slots of
    /// `operands`, the operands of an instruction in input order, for code
    /// that reads them from there. Constants too: the frames of inlined
    /// calls that slow paths run in are not initialized.
    /// Its inputs in registers are where `registers` says.
    pub(super) fn emit_store_operand_inputs(
        &mut self,
        node: NodeId,
        operands: &[u32],
        registers: &InputRegisters,
    ) -> Result<(), CompileFailure> {
        let scratch = self.pinned.scratch;
        for (index, operand) in operands.iter().enumerate() {
            if *operand == crate::bytecode::Operand::INVALID {
                continue;
            }
            self.load_input(scratch, node, index, registers);
            let address = self.slot_address(*operand)?;
            self.masm.store64(&address, scratch);
        }
        Ok(())
    }

    /// Calls the slow path of a `CallSlowPath` node with the values of the
    /// node's inputs as its operands, and returns where its outputs are.
    /// Its inputs in registers are where `registers` says.
    pub(super) fn emit_slow_path_call_with_inputs(
        &mut self,
        node: NodeId,
        registers: &InputRegisters,
    ) -> Result<Vec<OutputSource>, CompileFailure> {
        let Op::CallSlowPath {
            opcode, executable, pc, ..
        } = self.graph.node(node).op
        else {
            unreachable!("only slow path calls take their operands as inputs");
        };
        // NB: Slow paths that take their operands by value run like those of
        //     calls, with the saves area for the inputs in saved registers.
        if slow_path_call(opcode) != Some(SlowPathCall::Record) {
            let (operands, ssa_outputs) = self.slow_path_call_operands(opcode, executable, pc)?;
            // NB: The runtime's call helper and the call stub read the
            //     operands of calls from the frame, and calls leave their
            //     result there.
            if slow_path_call(opcode) == Some(SlowPathCall::JitCall) {
                self.emit_store_operand_inputs(node, &operands, registers)?;
                self.emit_generic(opcode, executable, pc)?;
                let Some(destination) = ssa_outputs.first() else {
                    return Ok(Vec::new());
                };
                let result = M::RETURN_GPRS[0];
                let address = self.slot_address(*destination)?;
                self.masm.load64(result, &address);
                return Ok(vec![OutputSource::Register(result)]);
            }
            self.slow_path_values = Some(SlowPathValues {
                node,
                operands,
                registers: registers.clone(),
                ssa_outputs,
            });
            let sources = self.emit_generic(opcode, executable, pc);
            self.slow_path_values = None;
            return sources;
        }
        let decoded = self.decoded(executable, pc)?;
        let address = self.runtime.slow_paths.get(opcode as usize).copied().unwrap_or(0);
        if address == 0 {
            return Err(CompileFailure::MissingRuntimeHelper { opcode: Some(opcode) });
        }
        let program_counter = self.frame_field(self.runtime.offsets.execution_context_program_counter);
        self.masm.store_imm32(&program_counter, pc);
        self.emit_record_call(
            opcode,
            executable,
            pc,
            decoded.next_pc(),
            address,
            Some((node, registers)),
        )
    }

    /// Loads the boxed value of input `index` of `node` into `dst`, with its
    /// inputs in registers where `registers` says.
    fn load_input(&mut self, dst: Gpr, node: NodeId, index: usize, registers: &InputRegisters) {
        let input = self.graph.node(node).inputs[index];
        let repr = self.graph.node(input).repr.unwrap_or(crate::code::Repr::Tagged);
        match self.allocation.node(node).inputs[index] {
            Location::Register(register) => {
                let saved_at = match (registers, register_of(register)) {
                    (InputRegisters::Saved(saved), Register::General(gpr)) => saved
                        .iter()
                        .position(|saved| *saved == gpr)
                        .map(|position| self.locals.saves + 8 * position as u32),
                    (InputRegisters::Saved(_), Register::Float(_)) => None,
                    (InputRegisters::Dumped, Register::General(gpr)) => Some(self.locals.dump + 8 * u32::from(gpr.0)),
                    (InputRegisters::Dumped, Register::Float(fpr)) => {
                        Some(self.locals.dump + 8 * (self.gpr_dump_count + u32::from(fpr.0)))
                    }
                };
                match saved_at {
                    Some(offset) => {
                        let address = self.local_address(offset);
                        self.masm.load64(dst, &address);
                        self.emit_box(dst, dst, repr);
                    }
                    None => self.emit_box_register(dst, register, repr),
                }
            }
            Location::Stack(slot) => {
                let address = self.spill_address(slot);
                self.masm.load64(dst, &address);
                self.emit_box(dst, dst, repr);
            }
            Location::Constant(bits) => self.masm.move_imm64(dst, value::boxed_constant(bits, repr)),
        }
    }

    /// Calls a slow path that takes the instruction's `Op` record, passing
    /// operands as `slow_path_layout()` describes.
    /// With `inputs` (a `CallSlowPath` node and the registers saved around
    /// it), the operands it reads are the node's inputs in layout order, and
    /// its outputs stay where the slow path left them, which this returns.
    /// Otherwise they are in their frame slots.
    fn emit_record_call(
        &mut self,
        opcode: crate::bytecode::OpCode,
        executable: u32,
        pc: u32,
        next_pc: u32,
        address: u64,
        inputs: Option<(NodeId, &InputRegisters)>,
    ) -> Result<Vec<OutputSource>, CompileFailure> {
        let layout = slow_path_layout(opcode);
        let arguments = M::ARGUMENT_GPRS;
        let scratch = self.pinned.scratch;
        let operands = layout
            .fields
            .iter()
            .map(|field| self.read_u32(executable, pc, field.instruction_offset))
            .collect::<Result<Vec<_>, _>>()?;

        // Inputs passed by value: in the remaining argument registers, then on the stack.
        let (by_value, first_register) = match layout.abi {
            SlowPathAbi::Scalar => (
                layout
                    .fields
                    .iter()
                    .zip(&operands)
                    .filter(|(field, _)| field.role == OperandRole::In)
                    .map(|(_, operand)| *operand)
                    .collect::<Vec<_>>(),
                3,
            ),
            SlowPathAbi::ScalarInputs => (
                layout
                    .fields
                    .iter()
                    .zip(&operands)
                    .filter(|(field, _)| field.role != OperandRole::Out)
                    .map(|(_, operand)| *operand)
                    .collect(),
                4,
            ),
            SlowPathAbi::Record => (Vec::new(), 4),
        };
        // NB: The inputs of a slow path call are the operands of the fields
        //     that are not outputs, then the array's.
        let input_index = |field: usize| {
            layout.fields[..field]
                .iter()
                .filter(|field| field.role != OperandRole::Out)
                .count()
        };
        let by_value_fields = layout
            .fields
            .iter()
            .enumerate()
            .filter(|(_, field)| match layout.abi {
                SlowPathAbi::Scalar => field.role == OperandRole::In,
                SlowPathAbi::ScalarInputs => field.role != OperandRole::Out,
                SlowPathAbi::Record => false,
            })
            .map(|(index, _)| index)
            .collect::<Vec<_>>();
        let load = |codegen: &mut Self, dst: Gpr, operand: u32, input: usize| match inputs {
            Some((node, registers)) => {
                codegen.load_input(dst, node, input, registers);
                Ok(())
            }
            None => codegen.load_raw_operand(dst, operand),
        };
        for (index, operand) in by_value.iter().enumerate() {
            let input = input_index(by_value_fields[index]);
            match arguments.get(first_register + index) {
                Some(register) => load(self, *register, *operand, input)?,
                None => {
                    load(self, scratch, *operand, input)?;
                    let stack_index = (first_register + index - arguments.len()) as u32;
                    let address = self.local_address(8 * stack_index);
                    self.masm.store64(&address, scratch);
                }
            }
        }

        // The record of every operand value; inputs are filled in here.
        if layout.abi == SlowPathAbi::Record {
            for (index, (field, operand)) in layout.fields.iter().zip(&operands).enumerate() {
                if field.role != OperandRole::Out {
                    load(self, scratch, *operand, input_index(index))?;
                    let address = self.local_address(self.locals.record + 8 * index as u32);
                    self.masm.store64(&address, scratch);
                }
            }
            if let Some(array) = layout.array {
                let count = self.read_u32(executable, pc, array.count_offset)?;
                let first_input = input_index(layout.fields.len());
                for element in 0..count {
                    let operand = self.read_u32(executable, pc, array.instruction_offset + 4 * element)?;
                    load(self, scratch, operand, first_input + element as usize)?;
                    let index = layout.fields.len() as u32 + element;
                    let address = self.local_address(self.locals.record + 8 * index);
                    self.masm.store64(&address, scratch);
                }
            }
        }
        if layout.abi != SlowPathAbi::Scalar {
            let record = self.local_address(self.locals.record);
            self.masm.load_effective_address(arguments[3], &record);
        }
        let instruction = self.executables[executable as usize]
            .bytecode_address
            .checked_add(u64::from(pc))
            .ok_or(CompileFailure::InvalidBytecode {
                pc,
                reason: "instruction address overflows",
            })?;
        self.masm.move_imm64(arguments[2], instruction);
        self.masm.move_imm32(arguments[1], pc);
        self.masm.move64(arguments[0], self.pinned.vm);
        self.masm.call_absolute(address);

        // Outputs come back in the second return register (scalar) or in the record.
        let mut outputs = Vec::new();
        let mut input_outputs = Vec::new();
        for (index, (field, operand)) in layout.fields.iter().zip(&operands).enumerate() {
            if field.role == OperandRole::In || *operand == crate::bytecode::Operand::INVALID {
                continue;
            }
            let source = match layout.abi {
                SlowPathAbi::Scalar => OutputSource::Register(M::RETURN_GPRS[1]),
                _ => OutputSource::Record(self.locals.record + 8 * index as u32),
            };
            outputs.push((source, *operand));
            if field.role == OperandRole::InOut {
                input_outputs.push((source, *operand));
            }
        }
        if inputs.is_some() {
            self.emit_continuation_check(next_pc, &[]);
            return Ok(outputs.into_iter().map(|(source, _)| source).collect());
        }
        // NB: Outputs that are SSA values stay where the slow path left them.
        let ssa_outputs = self
            .slow_path_values
            .as_ref()
            .map(|values| values.ssa_outputs.clone())
            .unwrap_or_default();
        let (ssa_outputs, frame_outputs): (Vec<_>, Vec<_>) = outputs
            .into_iter()
            .partition(|(_, operand)| ssa_outputs.contains(operand));
        self.emit_continuation_check_with_outputs(next_pc, &frame_outputs, &input_outputs)?;
        Ok(ssa_outputs.into_iter().map(|(source, _)| source).collect())
    }

    /// Stores slow path outputs to their frame slots.
    fn store_outputs(&mut self, outputs: &[(OutputSource, u32)]) -> Result<(), CompileFailure> {
        // The control word stays in the first return register.
        let temp = M::ARGUMENT_GPRS[1];
        debug_assert_ne!(temp, M::RETURN_GPRS[0]);
        for (source, operand) in outputs {
            let address = self.slot_address(*operand)?;
            match source {
                OutputSource::Register(register) => self.masm.store64(&address, *register),
                OutputSource::Record(offset) => {
                    let record = self.local_address(*offset);
                    self.masm.load64(temp, &record);
                    self.masm.store64(&address, temp);
                }
            }
        }
        Ok(())
    }

    fn emit_continuation_check(&mut self, next_pc: u32, outputs: &[(OutputSource, u32)]) {
        self.emit_continuation_check_with_outputs(next_pc, outputs, &[])
            .expect("outputs of value slow paths have valid slots");
    }

    /// Continues if the control word in the first return register is a
    /// continuation at `next_pc`, after storing `outputs`. Otherwise stores
    /// `input_outputs` back if the frame is still running (as the interpreter
    /// does on exceptions) and returns to the caller.
    fn emit_continuation_check_with_outputs(
        &mut self,
        next_pc: u32,
        outputs: &[(OutputSource, u32)],
        input_outputs: &[(OutputSource, u32)],
    ) -> Result<(), CompileFailure> {
        let control = M::RETURN_GPRS[0];
        let label = self.masm.new_label();
        self.masm.branch64_imm(
            Condition::NotEqual,
            control,
            (CONTINUATION_BIT | u64::from(next_pc)) as i64,
            label,
        );
        self.leave_stubs.push(LeaveStub {
            label,
            leave_frame: self.leave_frame.clone(),
            input_outputs: input_outputs.to_vec(),
            leave_arguments: self.leave_arguments.clone(),
        });
        self.store_outputs(outputs)
    }

    /// The node whose frame state a slow path of `node` writes when it does
    /// not continue in compiled code: `node` itself, if its frame state is
    /// one of the compiled function's frame (not of inlined calls, which
    /// write their frames before their slow paths run) that lists values.
    fn leave_frame_of(&self, node: NodeId) -> Option<NodeId> {
        let frame_state = self.graph.frame_state(self.graph.node(node).frame_state?);
        if frame_state.parent.is_some() {
            // NB: Slow paths and calls in inlined callees run in their
            //     published frames, and an exception they throw may be caught
            //     further out, where the interpreter reads the frames. (The
            //     slow paths of fast paths run in frames written in full.)
            return matches!(
                self.graph.node(node).op,
                Op::Generic { .. }
                    | Op::CallSlowPath { .. }
                    | Op::CallDirect { .. }
                    | Op::CallNative { .. }
                    | Op::CallForwardingArguments { .. }
            )
            .then_some(node);
        }
        (frame_state.executable == 0 && !frame_state.values.is_empty()).then_some(node)
    }

    /// The out of line code of a slow path call that did not continue.
    fn emit_leave_stub(&mut self, stub: LeaveStub) -> Result<(), CompileFailure> {
        self.masm.bind(stub.label);
        if let Some(leave_frame) = stub.leave_frame {
            if !stub.input_outputs.is_empty() {
                self.emit_store_input_outputs(&stub.input_outputs)?;
            }
            // NB: Nothing runs the frame again once the interpreter is to
            //     exit, which needs no frame state.
            let exit_interpreter = self.exit_interpreter;
            self.masm
                .branch64_imm(Condition::LessThan, M::RETURN_GPRS[0], 0, exit_interpreter);
            let control = self.local_address(self.locals.leave_control);
            self.masm.store64(&control, M::RETURN_GPRS[0]);
            for (register, offset) in &leave_frame.restore {
                let address = self.local_address(*offset);
                self.masm.load64(*register, &address);
            }
            let index = u32::try_from(self.sites.len()).expect("site count fits in u32");
            self.sites.push((leave_frame.node, SiteKind::Leave));
            let address = self.local_address(self.locals.exit_index);
            self.masm.store_imm32(&address, index);
            let leave_tail = self.leave_tail;
            self.masm.jump(leave_tail);
            return Ok(());
        }
        if !stub.input_outputs.is_empty() {
            self.emit_store_input_outputs(&stub.input_outputs)?;
        }
        self.leave_arguments = stub.leave_arguments;
        self.emit_leave_after_slow_path();
        Ok(())
    }

    /// Stores the outputs of a slow path that are also its inputs back if
    /// the frame still runs, as the interpreter does on exceptions.
    fn emit_store_input_outputs(&mut self, input_outputs: &[(OutputSource, u32)]) -> Result<(), CompileFailure> {
        let scratch = self.pinned.scratch;
        let skip = self.masm.new_label();
        self.masm.load64(
            scratch,
            &Address::new(self.pinned.vm, self.runtime.offsets.vm_running_execution_context as i32),
        );
        self.masm
            .branch64(Condition::NotEqual, scratch, self.pinned.frame, skip);
        self.store_outputs(input_outputs)?;
        self.masm.bind(skip);
        Ok(())
    }

    /// The comparison jump slow paths return one of the two targets, unless
    /// an exception moved the running execution context elsewhere.
    fn emit_jump_target_check(&mut self, pc: u32, true_target: u32, false_target: u32) {
        let control = M::RETURN_GPRS[0];
        let scratch = self.pinned.scratch;
        let leave = self.masm.new_label();
        let done = self.masm.new_label();
        self.masm.load64(
            scratch,
            &Address::new(self.pinned.vm, self.runtime.offsets.vm_running_execution_context as i32),
        );
        self.masm
            .branch64(Condition::NotEqual, scratch, self.pinned.frame, leave);
        let program_counter = self.frame_field(self.runtime.offsets.execution_context_program_counter);
        self.masm
            .branch32_memory_imm(Condition::NotEqual, &program_counter, pc as i32, leave);
        self.masm
            .branch64_imm(Condition::Equal, control, i64::from(true_target), done);
        self.masm
            .branch64_imm(Condition::Equal, control, i64::from(false_target), done);
        self.masm.bind(leave);
        if self.leave_frame.is_some() {
            let stub = self.masm.new_label();
            self.leave_stubs.push(LeaveStub {
                label: stub,
                leave_frame: self.leave_frame.clone(),
                input_outputs: Vec::new(),
                leave_arguments: Vec::new(),
            });
            self.masm.jump(stub);
        } else {
            self.emit_leave_after_slow_path();
        }
        self.masm.bind(done);
    }

    /// Returns to the caller after a slow path did not continue as expected:
    /// `ExitInterpreter` for a negative control word, `Resume` otherwise. A
    /// continuation elsewhere in the frame is stored as the frame's pc first.
    fn emit_leave_after_slow_path(&mut self) {
        let control = M::RETURN_GPRS[0];
        let exit_interpreter = self.exit_interpreter;
        let resume = self.resume;
        self.masm
            .branch64_imm(Condition::LessThan, control, 0, exit_interpreter);
        // The interpreter continues with the frame, which needs the
        // arguments object the code never created.
        if !self.leave_arguments.is_empty() {
            let scratch = self.pinned.scratch;
            let arguments = M::ARGUMENT_GPRS;
            self.masm.move64(scratch, control);
            let mapped = self.leave_arguments[0].1;
            let root_frame = self.local_address(self.locals.root_frame);
            self.masm.move64(arguments[0], self.pinned.vm);
            self.masm.load64(arguments[1], &root_frame);
            self.masm.move_imm32(arguments[2], u32::from(mapped));
            self.masm.call_absolute(self.runtime.create_arguments);
            self.masm.load64(arguments[1], &root_frame);
            for (slot, _) in self.leave_arguments.clone() {
                let offset = u64::from(self.runtime.offsets.execution_context_slots) + 8 * u64::from(slot);
                let address = Address::new(
                    arguments[1],
                    checked_i32(offset).expect("frame state slots are in the frame"),
                );
                self.masm.store64(&address, M::RETURN_GPRS[0]);
            }
            self.masm.move64(control, scratch);
        }
        self.masm
            .branch_test64(Condition::Zero, control, CONTINUATION_BIT, resume);
        let program_counter = self.frame_field(self.runtime.offsets.execution_context_program_counter);
        self.masm.store32(&program_counter, control);
        self.masm.jump(resume);
    }
}
