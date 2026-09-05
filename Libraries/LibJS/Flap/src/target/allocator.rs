/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Register allocator for typed virtual registers.
//!
//! This module:
//!
//!   1. Computes per-instruction use/def sets and runs an iterative
//!      backward dataflow to derive liveness.
//!   2. Builds an interference graph between virtual registers (and
//!      between virtual and physical registers killed by hidden
//!      clobbers, implicit i/o, fixed operand requirements, and
//!      caller-saved kills at C++ calls).
//!   3. Greedily assigns each virtual register a physical register from the
//!      allocatable register pool. Hard-errors if one cannot be placed; we
//!      never spill to the stack.
//!   4. Rewrites operands to typed physical-register identities.
//!
//! Handlers that don't use virtual registers require no register assignment.

use crate::Architecture;
use crate::low_ir::analysis::virtual_register_access;
use crate::low_ir::cfg::{ControlFlowGraph, instruction_successors};
use crate::low_ir::optimize::{invert_branches_over_jumps, remove_unreferenced_labels};
#[cfg(test)]
use crate::target::description::{
    AssertionOperation, CallKind, ControlOperation, EqualityCondition, MemoryWidth, PairWidth, TestCondition,
    ZeroCondition,
};
use crate::target::description::{InstructionDescription, OperandKind, SelectedOpcode};
use crate::target::ir::{
    AddressRegister, AllocatedFunction, AllocatedInstruction, AllocatedOperand, AllocatedProgram,
    Function as TargetFunction, Instruction as MachineInstruction, Operand, PhysicalMemoryAddress,
    Program as TargetProgram, RuntimeConstants, VirtualRegister, visit_virtual_registers,
};
#[cfg(test)]
use crate::target::registers::physical_register_named;
use crate::target::registers::{PhysicalRegister, register_info_for, resolve_interpreter_register};
use crate::types::InterpreterRegister;
use crate::types::RegisterClass as VirtualRegisterClass;
use crate::{CompileError, CompileStage};

/// A set of physical registers.
///
/// A register is identified by its class and its architecture-local number,
/// which together fit in six bits, so a whole set is one machine word. Every
/// function is compiled for a single architecture, so no further qualification
/// is needed to keep two registers apart.
type RegisterSet = u64;

fn register_bit(register: PhysicalRegister) -> RegisterSet {
    assert!(
        register != crate::target::registers::aarch64::XZR,
        "the AArch64 zero register is introduced only after allocation"
    );
    let class = match register.class() {
        crate::target::registers::RegisterClass::GeneralPurpose => 0,
        crate::target::registers::RegisterClass::FloatingPoint => 32,
    };
    let number = u32::from(register.number());
    assert!(number < 32, "register number {number} does not fit in a RegisterSet");
    1 << (class + number)
}

/// A set of virtual registers, held sorted.
///
/// The obvious representation is a bitmap over the dense numbering, and for a
/// handler-sized function it is the fastest one. A large function is not
/// handler-sized: the largest in Octane's zlib has 64298 virtual registers over
/// 286178 instructions, where a bitmap per instruction is half a gigabyte and
/// sweeping it is gigabytes of traffic. The number of registers live at any one
/// point stays small whatever the function's size, so the sets are sparse.
type LiveSet = Vec<u32>;

/// Merge `source` into the sorted set `into`, which stays sorted and unique.
fn merge_into(into: &mut LiveSet, source: &[u32]) {
    if into.is_empty() {
        into.extend_from_slice(source);
        return;
    }
    into.extend_from_slice(source);
    into.sort_unstable();
    into.dedup();
}

#[derive(Debug, Default, Clone, PartialEq, Eq)]
struct UseDef {
    uses: Vec<u32>,
    defs: Vec<u32>,
    clobbers: RegisterSet,
}

impl UseDef {
    fn use_register(&mut self, register: u32) {
        self.uses.push(register);
    }

    fn define_operand(&mut self, operand: &Operand, names: &Names, architecture: Architecture) {
        match operand {
            Operand::VirtualRegister(register) => {
                let register = names.index(register);
                self.defs.push(register);
            }
            Operand::InterpreterRegister(register) => {
                if let Some(register) = resolve_interpreter_register(*register, architecture) {
                    self.clobbers |= register_bit(register);
                }
            }
            Operand::PhysicalRegister(register) => {
                self.clobbers |= register_bit(*register);
            }
            _ => {}
        }
    }

    fn normalize(&mut self) {
        self.uses.sort_unstable();
        self.uses.dedup();
        self.defs.sort_unstable();
        self.defs.dedup();
    }
}

/// The debug names for the dense virtual-register numbering.
struct Names<'a> {
    function: crate::identity::HandlerId,
    registers: &'a [VirtualRegister],
}

impl<'a> Names<'a> {
    fn new(function: crate::identity::HandlerId, registers: &'a [VirtualRegister]) -> Self {
        Self { function, registers }
    }

    fn len(&self) -> usize {
        self.registers.len()
    }

    fn index(&self, register: &VirtualRegister) -> u32 {
        let id = register.id().expect("target virtual registers must be interned");
        assert_eq!(
            id.function(),
            self.function,
            "virtual register belongs to another handler"
        );
        debug_assert_eq!(self.registers[id.index() as usize], *register);
        id.index()
    }

    fn name(&self, index: u32) -> &VirtualRegister {
        &self.registers[index as usize]
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
struct Liveness {
    live_in: Vec<LiveSet>,
    live_out: Vec<LiveSet>,
    use_def: Vec<UseDef>,
}

#[derive(Default, Clone)]
struct Interference {
    forbidden: RegisterSet,
    affinities: Vec<u32>,
    pinned: Option<PhysicalRegister>,
    live_range_size: usize,
}

struct InterferenceGraph {
    neighbors: Vec<LiveSet>,
    nodes: Vec<Interference>,
}

impl InterferenceGraph {
    fn neighbors(&self, register: u32) -> &[u32] {
        &self.neighbors[register as usize]
    }

    fn interferes(&self, register: u32, other: u32) -> bool {
        self.neighbors(register).binary_search(&other).is_ok()
    }
}

fn allocation_error(handler: &str, message: impl Into<String>) -> CompileError {
    CompileError::new(CompileStage::Allocation, Some(handler), message)
}

fn finalization_error(handler: &str, message: impl Into<String>) -> CompileError {
    CompileError::new(CompileStage::Finalization, Some(handler), message)
}

pub(crate) fn allocate_program(program: TargetProgram) -> Result<AllocatedProgram, CompileError> {
    let functions = program
        .functions
        .iter()
        .map(|function| {
            if function.architecture != program.architecture {
                return Err(allocation_error(
                    &function.name,
                    "selected function architecture does not match its target program",
                ));
            }
            allocate_handler(function, &program.runtime).map(|cfg| AllocatedFunction {
                id: function.id,
                name: function.name.clone(),
                size: function.size,
                is_cold: function.is_cold,
                cfg,
            })
        })
        .collect::<Result<_, _>>()?;
    let allocated = AllocatedProgram {
        runtime: program.runtime,
        dispatch_handlers: program.dispatch_handlers,
        architecture: program.architecture,
        functions,
    };
    crate::target::verify::verify_program(&allocated)?;
    Ok(allocated)
}

/// Prepare a handler body and run register allocation on it. Returns a machine
/// control-flow graph with all virtual-register references replaced by their
/// assigned physical registers.
pub(crate) fn allocate_handler(
    handler: &TargetFunction,
    runtime: &RuntimeConstants,
) -> Result<ControlFlowGraph<AllocatedOperand, SelectedOpcode>, CompileError> {
    let arch = handler.architecture;
    let mut needs = false;
    for instruction in &handler.instructions {
        for operand in &instruction.operands {
            visit_virtual_registers(operand, &mut |_| needs = true);
        }
    }
    if !needs {
        return build_allocated_graph(
            handler,
            rewrite(handler, &handler.instructions, &AllocationPlan::default(), runtime)?,
        );
    }
    // A cold annotation says where a block belongs in the final layout and
    // executes nothing, so control never reaches it and liveness must not see
    // it as an instruction that can branch to the label it names.
    let analyzed = handler
        .instructions
        .iter()
        .filter(|instruction| instruction.opcode.operation() != crate::target::description::Operation::Cold)
        .collect::<Vec<_>>();
    let successors = instruction_successors(&analyzed);
    let assignments = allocate(handler, &analyzed, &successors, arch)?;
    let mut rewritten = rewrite(handler, &handler.instructions, &assignments, runtime)?;
    invert_branches_over_jumps(&mut rewritten);
    remove_unreferenced_labels(&mut rewritten);
    invert_branches_over_jumps(&mut rewritten);
    remove_unreferenced_labels(&mut rewritten);
    build_allocated_graph(handler, rewritten)
}

fn build_allocated_graph(
    handler: &TargetFunction,
    instructions: Vec<AllocatedInstruction>,
) -> Result<ControlFlowGraph<AllocatedOperand, SelectedOpcode>, CompileError> {
    let mut graph = ControlFlowGraph::from_instructions(instructions)
        .map_err(|message| allocation_error(&handler.name, message))?;

    graph.simplify();

    Ok(graph)
}

// ============================================================================
// Use/def computation
// ============================================================================

/// Compute the physical and virtual register uses, definitions, and clobbers
/// for one machine instruction.
fn analyze_instruction(insn: &MachineInstruction, names: &Names, arch: Architecture) -> UseDef {
    let mut ud = UseDef::default();
    let info = insn.opcode.description();
    let access = virtual_register_access(insn, Some(arch));
    for register in access.reads {
        ud.use_register(names.index(&register));
    }
    for register in access.definitions {
        ud.defs.push(names.index(&register));
    }
    for (i, op) in insn.operands.iter().enumerate() {
        let kind = operand_kind_at(info, i, insn.operands.len(), arch);
        if matches!(
            kind,
            OperandKind::GprOut
                | OperandKind::GprInOut
                | OperandKind::GprScratch
                | OperandKind::FprOut
                | OperandKind::FprInOut
                | OperandKind::FprScratch
                | OperandKind::RegisterOut
        ) && !matches!(op, Operand::VirtualRegister(_))
        {
            ud.define_operand(op, names, arch);
        }
    }
    // Implicit i/o and all-caller-saved kills.
    let arch_spec = info.arch(arch);
    for r in arch_spec.implicit_outputs {
        ud.clobbers |= register_bit(*r);
    }
    if info.is_call {
        ud.clobbers |= caller_saved_registers(arch);
    }

    ud
}

/// Every register a C++ call is allowed to destroy, as one set.
fn caller_saved_registers(architecture: Architecture) -> RegisterSet {
    let registers = register_info_for(architecture);
    registers
        .caller_saved_gpr
        .iter()
        .chain(registers.caller_saved_fpr)
        .fold(0, |set, register| set | register_bit(*register))
}

fn operand_kind_at(
    description: &InstructionDescription,
    index: usize,
    operand_count: usize,
    architecture: Architecture,
) -> OperandKind {
    description
        .selected_operand_kind(index, operand_count, architecture)
        .expect("operand index out of range")
}

fn compute_liveness(
    handler_name: &str,
    instructions: &[&MachineInstruction],
    successors: &[Vec<usize>],
    names: &Names,
    arch: Architecture,
) -> Result<Liveness, CompileError> {
    let n = instructions.len();
    assert_eq!(successors.len(), n, "invalid CFG for handler '{handler_name}'");

    let use_def = instructions
        .iter()
        .map(|insn| analyze_instruction(insn, names, arch))
        .collect::<Vec<_>>();
    let liveness = solve_liveness(use_def, successors);

    // Sanity-check: a virtual register must be defined before its first use
    // along every path. We catch the simplest case: no virtual register may
    // be live at handler entry.
    if let Some(register) = liveness.live_in[0].first().copied() {
        // Walk forward and find the first instruction that pulls this
        // virtual register into its live-in without having defined it
        // yet, so the error points at the actual problem.
        let mut culprit = String::from("(unknown)");
        for (i, ud) in liveness.use_def.iter().enumerate() {
            if ud.uses.binary_search(&register).is_ok() && ud.defs.binary_search(&register).is_err() {
                culprit = format!("instruction #{i}: {:?}", instructions[i]);
                break;
            }
        }
        let register = names.name(register);
        return Err(allocation_error(
            handler_name,
            format!(
                "virtual register '{register}' is used before being assigned (live at handler entry); first use: {culprit}"
            ),
        ));
    }

    Ok(liveness)
}

fn solve_liveness(mut use_def: Vec<UseDef>, successors: &[Vec<usize>]) -> Liveness {
    let n = use_def.len();
    assert_eq!(successors.len(), n);
    for use_def in &mut use_def {
        use_def.normalize();
    }
    let mut predecessors = vec![Vec::new(); n];
    for (instruction, successors) in successors.iter().enumerate() {
        for successor in successors {
            predecessors[*successor].push(instruction);
        }
    }

    let mut live_in = vec![LiveSet::new(); n];
    let mut live_out = vec![LiveSet::new(); n];
    let mut next_out = LiveSet::new();
    let mut next_in = LiveSet::new();
    // What is live before an instruction depends only on what is live before
    // its successors, so revisiting an instruction is only worthwhile when one
    // of those moved. Sweeping the whole function instead costs a sweep to
    // discover that nothing changed, plus one for every step a value has to
    // travel backwards.
    let mut queued = vec![true; n];
    let mut queue = (0..n).collect::<Vec<_>>();
    while let Some(instruction) = queue.pop() {
        queued[instruction] = false;
        next_out.clear();
        for successor in &successors[instruction] {
            merge_into(&mut next_out, &live_in[*successor]);
        }
        next_in.clear();
        next_in.extend(
            next_out
                .iter()
                .copied()
                .filter(|live| use_def[instruction].defs.binary_search(live).is_err()),
        );
        if !use_def[instruction].uses.is_empty() {
            merge_into(&mut next_in, &use_def[instruction].uses);
        }
        if live_out[instruction] != next_out {
            live_out[instruction].clear();
            live_out[instruction].extend_from_slice(&next_out);
        }
        if live_in[instruction] == next_in {
            continue;
        }
        live_in[instruction].clear();
        live_in[instruction].extend_from_slice(&next_in);
        for predecessor in &predecessors[instruction] {
            if !std::mem::replace(&mut queued[*predecessor], true) {
                queue.push(*predecessor);
            }
        }
    }
    Liveness {
        live_in,
        live_out,
        use_def,
    }
}

fn build_interference(liveness: &Liveness, count: usize) -> InterferenceGraph {
    let mut graph = InterferenceGraph {
        neighbors: vec![LiveSet::new(); count],
        nodes: vec![Interference::default(); count],
    };
    for instruction in 0..liveness.use_def.len() {
        let use_def = &liveness.use_def[instruction];
        let live_out = &liveness.live_out[instruction];
        for live in [&liveness.live_in[instruction], live_out] {
            for register in live {
                graph.nodes[*register as usize].live_range_size += 1;
            }
        }
        // Two values interfere when both are live at once, and one of them was
        // written while the other was already live: a value live at a point is
        // written on every path reaching it, so whichever of the two was
        // written second saw the other live. Recording the conflict where a
        // value is written therefore records every conflict, without walking
        // the pairs of every live set at every instruction.
        //
        // What an instruction writes conflicts with whatever is still live
        // after it even when nothing reads the result: a value the branch
        // conditions on but never names again is written and immediately dead,
        // so it appears in no live set at all, and without this it would
        // collect no neighbors and could be handed the register of a value that
        // has to survive the instruction.
        for defined in &use_def.defs {
            for other in live_out.iter().chain(&use_def.defs) {
                if other == defined {
                    continue;
                }
                graph.neighbors[*defined as usize].push(*other);
                graph.neighbors[*other as usize].push(*defined);
            }
        }

        // A value that passes through an instruction, live on both sides
        // without being rewritten, cannot live in a register the instruction
        // destroys. A value live after the instruction and not written by it is
        // live before it too, so the live-out set names exactly those.
        if use_def.clobbers != 0 {
            for register in live_out {
                if use_def.defs.binary_search(register).is_ok() {
                    continue;
                }
                graph.nodes[*register as usize].forbidden |= use_def.clobbers;
            }
        }
    }
    for neighbors in &mut graph.neighbors {
        neighbors.sort_unstable();
        neighbors.dedup();
    }
    debug_assert!(records_every_simultaneous_pair(&graph, liveness));
    graph
}

/// Check the claim `build_interference` rests on: that recording a conflict
/// where a value is written records every pair of values that are ever live at
/// the same time.
///
/// This walks the pairs of every live set, which is what the real construction
/// exists to avoid, so it is limited to functions small enough to afford it.
#[cfg(debug_assertions)]
fn records_every_simultaneous_pair(graph: &InterferenceGraph, liveness: &Liveness) -> bool {
    const AFFORDABLE_INSTRUCTIONS: usize = 2000;
    if liveness.use_def.len() > AFFORDABLE_INSTRUCTIONS {
        return true;
    }
    for instruction in 0..liveness.use_def.len() {
        for live in [&liveness.live_in[instruction], &liveness.live_out[instruction]] {
            for (position, register) in live.iter().enumerate() {
                for other in &live[position + 1..] {
                    if !graph.interferes(*register, *other) {
                        return false;
                    }
                }
            }
        }
    }
    true
}

#[cfg(not(debug_assertions))]
fn records_every_simultaneous_pair(_: &InterferenceGraph, _: &Liveness) -> bool {
    true
}

// ============================================================================
// Interference and allocation
// ============================================================================

#[derive(Default)]
struct AllocationPlan(Vec<Option<PhysicalRegister>>);

impl AllocationPlan {
    fn physical(&self, register: &VirtualRegister) -> PhysicalRegister {
        let id = register.id().expect("target virtual registers must be interned");
        self.0[id.index() as usize].expect("every virtual register must have an allocation")
    }
}

/// Coalescing groups, as a dense map from virtual register to the group it
/// belongs to. A group is a set of move-related values that all take the same
/// physical register, so the moves relating them disappear.
#[derive(Default, Clone)]
struct CoalescingGroups {
    group_of: Vec<Option<u32>>,
    groups: Vec<Vec<u32>>,
}

impl CoalescingGroups {
    fn empty(count: usize) -> Self {
        Self {
            group_of: vec![None; count],
            groups: Vec::new(),
        }
    }

    fn of(&self, register: u32) -> Option<&[u32]> {
        self.group_of[register as usize].map(|group| self.groups[group as usize].as_slice())
    }

    fn add(&mut self, members: Vec<u32>) {
        let group = self.groups.len() as u32;
        for member in &members {
            self.group_of[*member as usize] = Some(group);
        }
        self.groups.push(members);
    }

    fn remove_last(&mut self) {
        let members = self.groups.pop().expect("a candidate coalescing group was just added");
        for member in members {
            self.group_of[member as usize] = None;
        }
    }
}

struct ColoringContext<'a> {
    handler: &'a TargetFunction,
    interference: &'a InterferenceGraph,
    names: &'a Names<'a>,
    gpr_pool: &'a [PhysicalRegister],
    fpr_pool: &'a [PhysicalRegister],
    gpr_mask: RegisterSet,
    fpr_mask: RegisterSet,
}

impl ColoringContext<'_> {
    fn pool_mask(&self, class: VirtualRegisterClass) -> RegisterSet {
        match class {
            VirtualRegisterClass::GeneralPurpose => self.gpr_mask,
            VirtualRegisterClass::FloatingPoint => self.fpr_mask,
        }
    }

    fn forbid_conflicts(
        &self,
        member: u32,
        pool_mask: RegisterSet,
        assignments: &[Option<PhysicalRegister>],
        forbidden: &mut RegisterSet,
    ) {
        *forbidden |= self.interference.nodes[member as usize].forbidden & pool_mask;
        *forbidden |= self.assigned_neighbors(member, assignments);
    }

    /// Every physical register already handed to something this value
    /// interferes with.
    fn assigned_neighbors(&self, register: u32, assignments: &[Option<PhysicalRegister>]) -> RegisterSet {
        let mut taken = 0;
        for other in self.interference.neighbors(register) {
            if let Some(physical) = assignments[*other as usize] {
                taken |= register_bit(physical);
            }
        }
        taken
    }

    fn cheapest(&self, pool: &[PhysicalRegister], forbidden: RegisterSet) -> Option<PhysicalRegister> {
        pool.iter()
            .enumerate()
            .filter(|(_, candidate)| forbidden & register_bit(**candidate) == 0)
            .min_by_key(|(index, candidate)| (candidate.allocation_cost(), *index))
            .map(|(_, candidate)| *candidate)
    }
}

#[derive(Clone, Copy)]
enum AllocationPriority {
    UseCount,
    LiveRange,
}

/// Union-find over virtual registers, carrying each set's members so that
/// merging two sets never has to rescan every register.
struct Coalescer {
    parents: Vec<u32>,
    members: Vec<Vec<u32>>,
}

impl Coalescer {
    fn new(count: usize) -> Self {
        Self {
            parents: (0..count as u32).collect(),
            members: (0..count as u32).map(|index| vec![index]).collect(),
        }
    }

    fn root_of(&mut self, mut index: u32) -> u32 {
        while self.parents[index as usize] != index {
            let grandparent = self.parents[self.parents[index as usize] as usize];
            self.parents[index as usize] = grandparent;
            index = grandparent;
        }
        index
    }

    fn merge(&mut self, into: u32, from: u32) {
        self.parents[from as usize] = into;
        let moved = std::mem::take(&mut self.members[from as usize]);
        self.members[into as usize].extend(moved);
    }
}

fn allocate(
    handler: &TargetFunction,
    instructions: &[&MachineInstruction],
    successors: &[Vec<usize>],
    arch: Architecture,
) -> Result<AllocationPlan, CompileError> {
    let names = Names::new(handler.id, &handler.virtual_registers);
    let count = names.len();
    let liveness = compute_liveness(&handler.name, instructions, successors, &names, arch)?;

    let mut interference = build_interference(&liveness, count);
    let mut move_affinity_edges = Vec::new();
    for insn in instructions {
        let info = insn.opcode.description();
        let arch_spec = info.arch(arch);
        for &(index, physical) in arch_spec.fixed_operands {
            if let Some(Operand::VirtualRegister(register)) = insn.operands.get(index)
                && let Some(previous) = interference.nodes[names.index(register) as usize]
                    .pinned
                    .replace(physical)
                && previous != physical
            {
                return Err(allocation_error(
                    &handler.name,
                    format!(
                        "virtual register '{register}' is pinned to two different physical registers ({previous} and {physical})"
                    ),
                ));
            }
        }

        let mut explicit_scratches = 0;
        for (index, operand) in insn.operands.iter().enumerate() {
            if info
                .selected_operand_kind(index, insn.operands.len(), arch)
                .is_some_and(|kind| matches!(kind, OperandKind::GprScratch | OperandKind::FprScratch))
            {
                let Operand::PhysicalRegister(register) = operand else {
                    return Err(allocation_error(
                        &handler.name,
                        "target-selected scratch operand must be a physical register",
                    ));
                };
                explicit_scratches |= register_bit(*register);
            }
        }
        if explicit_scratches != 0 {
            for operand in &insn.operands {
                visit_virtual_registers(operand, &mut |register| {
                    interference.nodes[names.index(register) as usize].forbidden |= explicit_scratches;
                });
            }
        }
        if !arch_spec.implicit_outputs.is_empty() {
            for (index, operand) in insn.operands.iter().enumerate() {
                if let Operand::VirtualRegister(register) = operand {
                    let pinned_here = arch_spec
                        .fixed_operands
                        .iter()
                        .find_map(|&(operand, physical)| (operand == index).then_some(physical));
                    let forbidden = arch_spec
                        .implicit_outputs
                        .iter()
                        .filter(|physical| Some(**physical) != pinned_here)
                        .fold(0, |set, physical| set | register_bit(*physical));
                    interference.nodes[names.index(register) as usize].forbidden |= forbidden;
                }
            }
        }
        // Only prefer aliases when both backends can omit a copy. For example,
        // mov32 must still execute to zero-extend its input even when both
        // operands share a register.
        for &(output, input) in info.coalescing_pairs {
            let (Some(Operand::VirtualRegister(output)), Some(Operand::VirtualRegister(input))) =
                (insn.operands.get(output), insn.operands.get(input))
            else {
                continue;
            };
            if output == input || output.class() != input.class() {
                continue;
            }
            let (output, input) = (names.index(output), names.index(input));
            interference.nodes[output as usize].affinities.push(input);
            interference.nodes[input as usize].affinities.push(output);
            move_affinity_edges.push((output, input));
        }
    }

    // Build candidate physical-register pools.
    let mapping = register_info_for(arch);
    let gpr_pool = mapping.temporaries.to_vec();
    let fpr_pool = mapping.fp_temporaries.to_vec();

    // On x86_64 each saved use of a low-cost register saves an encoding
    // byte, so hot virtual registers want to land in cheap registers.
    let mut use_counts = vec![0u32; count];
    for insn in instructions {
        for operand in &insn.operands {
            visit_virtual_registers(operand, &mut |register| {
                use_counts[names.index(register) as usize] += 1;
            });
        }
    }

    // Merge move-related values whenever the combined group remains fully
    // noninterfering. Greedy compatible subgroups preserve opportunities in
    // affinity graphs where one interfering edge prevents full coalescing.
    let mut coalescer = Coalescer::new(count);
    for (lhs, rhs) in move_affinity_edges {
        let lhs_root = coalescer.root_of(lhs);
        let rhs_root = coalescer.root_of(rhs);
        if lhs_root == rhs_root {
            continue;
        }
        // Members of either set already do not interfere with each other, so
        // only the pairs that cross between the two sets are new.
        let can_coalesce_all = coalescer.members[lhs_root as usize].iter().all(|member| {
            coalescer.members[rhs_root as usize]
                .iter()
                .all(|other| !interference.interferes(*member, *other))
        });
        let mut pinned = None;
        let mut conflicting_pins = false;
        for member in coalescer.members[lhs_root as usize]
            .iter()
            .chain(&coalescer.members[rhs_root as usize])
        {
            if let Some(physical) = interference.nodes[*member as usize].pinned {
                conflicting_pins |= pinned.is_some_and(|already| already != physical);
                pinned = Some(physical);
            }
        }
        if can_coalesce_all && !conflicting_pins {
            coalescer.merge(lhs_root, rhs_root);
        }
    }

    let mut coalescing_groups = CoalescingGroups::empty(count);
    for root in 0..count as u32 {
        if coalescer.parents[root as usize] != root || coalescer.members[root as usize].len() <= 1 {
            continue;
        }
        let mut members = std::mem::take(&mut coalescer.members[root as usize]);
        members.sort_unstable();
        coalescing_groups.add(members);
    }

    // Greedy graph coloring is sensitive to the order virtual registers are
    // processed in. Two orders are useful here:
    //
    //   - Use-count-first is cost-optimal: the virtual with the most operand
    //     references claims the cheapest register, so byte savings
    //     concentrate where they matter most.
    //
    //   - Live-range-first is fit-optimal: the most constrained virtuals
    //     (those alive across the most instructions, which see the most
    //     interference) claim registers first and are the most likely to
    //     fit. Some packed handlers (e.g. `Call`) only color successfully
    //     under this order.
    //
    // Values move-related to a hard-pinned value follow the pinned values
    // in either order, before unrelated virtuals can claim the ABI register.
    // We then try cost-first and fall back to fit-first only when cost-first
    // can't color. This wins the byte savings when the handler has slack
    // and never regresses fit when it doesn't.
    let gpr_mask = gpr_pool.iter().fold(0, |set, register| set | register_bit(*register));
    let fpr_mask = fpr_pool.iter().fold(0, |set, register| set | register_bit(*register));
    let context = ColoringContext {
        handler,
        interference: &interference,
        names: &names,
        gpr_pool: &gpr_pool,
        fpr_pool: &fpr_pool,
        gpr_mask,
        fpr_mask,
    };
    let try_color = |sorted: &[u32], groups: &CoalescingGroups| color(&context, sorted, groups);
    let follows_pinned = (0..count)
        .map(|register| {
            interference.nodes[register]
                .affinities
                .iter()
                .any(|neighbor| interference.nodes[*neighbor as usize].pinned.is_some())
        })
        .collect::<Vec<_>>();
    let compare = |a: &u32, b: &u32, priority| {
        let (a, b) = (*a as usize, *b as usize);
        let ap = interference.nodes[a].pinned.is_some();
        let bp = interference.nodes[b].pinned.is_some();
        let au = use_counts[a] as usize;
        let bu = use_counts[b] as usize;
        let aw = interference.nodes[a].live_range_size;
        let bw = interference.nodes[b].live_range_size;
        let ((af, bf), (as_, bs)) = match priority {
            AllocationPriority::UseCount => ((au, bu), (aw, bw)),
            AllocationPriority::LiveRange => ((aw, bw), (au, bu)),
        };
        bp.cmp(&ap)
            .then_with(|| follows_pinned[b].cmp(&follows_pinned[a]))
            .then_with(|| bf.cmp(&af))
            .then_with(|| bs.cmp(&as_))
            .then_with(|| a.cmp(&b))
    };

    let mut use_order = (0..count as u32).collect::<Vec<_>>();
    use_order.sort_by(|a, b| compare(a, b, AllocationPriority::UseCount));
    if let Ok(plan) = try_color(&use_order, &coalescing_groups) {
        return Ok(plan);
    }

    let mut live_order = use_order.clone();
    live_order.sort_by(|a, b| compare(a, b, AllocationPriority::LiveRange));
    if let Ok(plan) = try_color(&live_order, &coalescing_groups) {
        return Ok(plan);
    }

    let mut groups = coalescing_groups.groups.clone();
    let group_score = |group: &[u32]| {
        (
            group.iter().map(|member| use_counts[*member as usize]).sum::<u32>(),
            group.len(),
        )
    };
    groups.sort_by(|lhs, rhs| group_score(rhs).cmp(&group_score(lhs)).then_with(|| lhs.cmp(rhs)));
    let mut retained_groups = CoalescingGroups::empty(count);
    const COALESCING_GROUP_RETENTION_LIMIT: usize = 32;
    for group in groups.into_iter().take(COALESCING_GROUP_RETENTION_LIMIT) {
        retained_groups.add(group);
        if color(&context, &use_order, &retained_groups).is_err()
            && color(&context, &live_order, &retained_groups).is_err()
        {
            retained_groups.remove_last();
        }
    }
    if !retained_groups.groups.is_empty() {
        if let Ok(plan) = try_color(&use_order, &retained_groups) {
            return Ok(plan);
        }
        if let Ok(plan) = try_color(&live_order, &retained_groups) {
            return Ok(plan);
        }
    }

    let no_groups = CoalescingGroups::empty(count);
    if let Ok(plan) = try_color(&use_order, &no_groups) {
        return Ok(plan);
    }
    if let Ok(plan) = try_color(&live_order, &no_groups) {
        return Ok(plan);
    }

    match exact_color(&context) {
        ExactColorResult::Colored(plan) => Ok(plan),
        ExactColorResult::Impossible => Err(spill_free_allocation_error(
            &handler.name,
            &liveness,
            &names,
            gpr_pool.len(),
            fpr_pool.len(),
            "no legal register coloring exists",
        )),
        ExactColorResult::SearchLimit => Err(spill_free_allocation_error(
            &handler.name,
            &liveness,
            &names,
            gpr_pool.len(),
            fpr_pool.len(),
            "the bounded exact-coloring fallback reached its search limit",
        )),
    }
}

/// Greedy linear-scan coloring driven by the order in `sorted_virtuals`.
/// Returns Err if any virtual register cannot be placed; the caller decides
/// whether to retry with a different ordering.
fn color(
    context: &ColoringContext<'_>,
    sorted_virtuals: &[u32],
    coalescing_groups: &CoalescingGroups,
) -> Result<AllocationPlan, CompileError> {
    let ColoringContext {
        handler,
        interference,
        names,
        gpr_pool,
        fpr_pool,
        ..
    } = context;
    let mut assignments = vec![None; interference.nodes.len()];

    for name in sorted_virtuals {
        let name = *name;
        if assignments[name as usize].is_some() {
            continue;
        }
        let class = names.name(name).class();
        let is_gpr = class == VirtualRegisterClass::GeneralPurpose;
        let pool = if is_gpr { gpr_pool } else { fpr_pool };
        let pool_mask = context.pool_mask(class);
        if let Some(group) = coalescing_groups.of(name) {
            let mut forbidden = 0;
            let mut pinned = None;
            let mut conflicting_pins = false;
            for member in group {
                context.forbid_conflicts(*member, pool_mask, &assignments, &mut forbidden);
                if let Some(physical) = interference.nodes[*member as usize].pinned {
                    conflicting_pins |= pinned.is_some_and(|already| already != physical);
                    pinned = Some(physical);
                }
            }
            let pick = if let Some(pinned) = pinned {
                if conflicting_pins || pool_mask & register_bit(pinned) == 0 || forbidden & register_bit(pinned) != 0 {
                    return Err(allocation_error(
                        &handler.name,
                        format!(
                            "could not coalesce move affinity group containing '{}'",
                            names.name(name)
                        ),
                    ));
                }
                pinned
            } else {
                context.cheapest(pool, forbidden).ok_or_else(|| {
                    allocation_error(
                        &handler.name,
                        format!(
                            "could not coalesce move affinity group containing '{}'",
                            names.name(name)
                        ),
                    )
                })?
            };
            for member in group {
                assignments[*member as usize] = Some(pick);
            }
            continue;
        }
        // Build the set of physical regs we cannot use for this virtual:
        //   - any phys reg killed at an instruction the virtual passes
        //     through (alive on both sides without being redefined). A
        //     virtual that only enters (use, dying here) or only exits (def,
        //     newly born here) the instruction is fine: the kill happens
        //     between input read and output write, so the virtual's value
        //     is no longer needed (or not yet needed) at that moment.
        //   - any phys reg forbidden by an operand constraint.
        //   - any phys reg already assigned to an interfering virtual. A
        //     "before only" overlap with an "after only" overlap at the
        //     same instruction does NOT collide: the codegen reads input
        //     operands before writing output operands, so the same physical
        //     register can host an outgoing value and an incoming value at
        //     the same instruction.
        let mut forbidden = 0;
        context.forbid_conflicts(name, pool_mask, &assignments, &mut forbidden);

        let pick = if let Some(pinned) = interference.nodes[name as usize].pinned {
            if pool_mask & register_bit(pinned) == 0 {
                return Err(allocation_error(
                    &handler.name,
                    format!(
                        "virtual register '{}' must be pinned to '{pinned}', but that register is not in the {} pool",
                        names.name(name),
                        if is_gpr { "GPR" } else { "FPR" }
                    ),
                ));
            }
            if forbidden & register_bit(pinned) != 0 {
                return Err(allocation_error(
                    &handler.name,
                    format!(
                        "virtual register '{}' must be pinned to '{pinned}', but that register is killed or occupied across its live range (forbidden: {forbidden:#x})",
                        names.name(name),
                    ),
                ));
            }
            pinned
        } else {
            let preferred = interference.nodes[name as usize]
                .affinities
                .iter()
                .filter_map(|other| assignments[*other as usize])
                .find(|register| forbidden & register_bit(*register) == 0);
            // If an affinity neighbor has not been colored yet, choose a
            // register it can use too. This lets continuation parameters
            // follow constrained producers instead of claiming a scratch
            // register that forces a move at the control-flow edge.
            let future_preferred = preferred.is_none().then(|| {
                // Each uncolored affinity neighbor rules out the registers
                // its own neighbors already hold, plus whatever its operand
                // constraints forbid. Summarizing that per neighbor keeps the
                // candidate scan out of the neighbor walk.
                let mut unusable = forbidden;
                let mut any = false;
                for neighbor in &interference.nodes[name as usize].affinities {
                    if assignments[*neighbor as usize].is_some() || interference.interferes(name, *neighbor) {
                        continue;
                    }
                    any = true;
                    unusable |= interference.nodes[*neighbor as usize].forbidden;
                    unusable |= context.assigned_neighbors(*neighbor, &assignments);
                    if let Some(physical) = interference.nodes[*neighbor as usize].pinned {
                        // A pinned neighbor can follow us into exactly one
                        // register, so every other register is no help.
                        unusable |= !register_bit(physical);
                    }
                }
                any.then(|| context.cheapest(pool, unusable)).flatten()
            });
            if let Some(preferred) = preferred {
                preferred
            } else if let Some(preferred) = future_preferred.flatten() {
                preferred
            } else {
                context.cheapest(pool, forbidden).ok_or_else(|| {
                    allocation_error(
                        &handler.name,
                        format!(
                            "could not allocate {} virtual register '{}': {} pool exhausted (forbidden: {forbidden:#x})",
                            if is_gpr { "general-purpose" } else { "floating-point" },
                            names.name(name),
                            if is_gpr { "GPR" } else { "FPR" },
                        ),
                    )
                })?
            }
        };

        assignments[name as usize] = Some(pick);
    }

    Ok(AllocationPlan(assignments))
}

enum ExactColorResult {
    Colored(AllocationPlan),
    Impossible,
    SearchLimit,
}

enum ExactSearchResult {
    Colored,
    Impossible,
    SearchLimit,
}

const EXACT_COLOR_REGISTER_LIMIT: usize = 256;
const EXACT_COLOR_WORK_LIMIT: u64 = 2_000_000;

struct ExactColorBudget {
    remaining: u64,
}

impl ExactColorBudget {
    fn spend(&mut self, work: usize) -> bool {
        let Ok(work) = u64::try_from(work) else {
            return false;
        };
        if work > self.remaining {
            self.remaining = 0;
            return false;
        }
        self.remaining -= work;
        true
    }
}

fn exact_color(context: &ColoringContext<'_>) -> ExactColorResult {
    if context.interference.nodes.len() > EXACT_COLOR_REGISTER_LIMIT {
        return ExactColorResult::SearchLimit;
    }
    let mut assignments = vec![None; context.interference.nodes.len()];
    let mut budget = ExactColorBudget {
        remaining: EXACT_COLOR_WORK_LIMIT,
    };
    match exact_color_search(context, &mut assignments, &mut budget) {
        ExactSearchResult::Colored => ExactColorResult::Colored(AllocationPlan(assignments)),
        ExactSearchResult::Impossible => ExactColorResult::Impossible,
        ExactSearchResult::SearchLimit => ExactColorResult::SearchLimit,
    }
}

fn exact_color_search(
    context: &ColoringContext<'_>,
    assignments: &mut [Option<PhysicalRegister>],
    budget: &mut ExactColorBudget,
) -> ExactSearchResult {
    if !budget.spend(1) {
        return ExactSearchResult::SearchLimit;
    }

    let mut selected = None;
    let mut selected_candidates = Vec::new();
    let mut selected_degree = 0;
    for register in 0..assignments.len() as u32 {
        if !budget.spend(1) {
            return ExactSearchResult::SearchLimit;
        }
        if assignments[register as usize].is_some() {
            continue;
        }
        let class = context.names.name(register).class();
        let pool = if class == VirtualRegisterClass::GeneralPurpose {
            context.gpr_pool
        } else {
            context.fpr_pool
        };
        let mut forbidden = 0;
        if !budget.spend(context.interference.neighbors(register).len()) {
            return ExactSearchResult::SearchLimit;
        }
        context.forbid_conflicts(register, context.pool_mask(class), assignments, &mut forbidden);
        let pinned = context.interference.nodes[register as usize].pinned;
        if !budget.spend(pool.len()) {
            return ExactSearchResult::SearchLimit;
        }
        let mut candidates = pool
            .iter()
            .copied()
            .filter(|candidate| {
                pinned.is_none_or(|pinned| pinned == *candidate) && forbidden & register_bit(*candidate) == 0
            })
            .collect::<Vec<_>>();
        if candidates.is_empty() {
            return ExactSearchResult::Impossible;
        }
        let affinities = &context.interference.nodes[register as usize].affinities;
        if !budget.spend(affinities.len()) {
            return ExactSearchResult::SearchLimit;
        }
        let affinity_assignments = affinities
            .iter()
            .filter_map(|neighbor| assignments[*neighbor as usize])
            .fold(0, |set, physical| set | register_bit(physical));
        if !budget.spend(candidates.len().saturating_mul(candidates.len())) {
            return ExactSearchResult::SearchLimit;
        }
        candidates.sort_by_key(|candidate| {
            let follows_affinity = affinity_assignments & register_bit(*candidate) != 0;
            (!follows_affinity, candidate.allocation_cost())
        });
        let degree = context.interference.neighbors(register).len();
        if selected.is_none()
            || candidates.len() < selected_candidates.len()
            || (candidates.len() == selected_candidates.len() && degree > selected_degree)
        {
            selected = Some(register);
            selected_candidates = candidates;
            selected_degree = degree;
        }
    }

    let Some(register) = selected else {
        return ExactSearchResult::Colored;
    };
    let mut reached_limit = false;
    for candidate in selected_candidates {
        if !budget.spend(1) {
            return ExactSearchResult::SearchLimit;
        }
        assignments[register as usize] = Some(candidate);
        match exact_color_search(context, assignments, budget) {
            ExactSearchResult::Colored => return ExactSearchResult::Colored,
            ExactSearchResult::Impossible => {}
            ExactSearchResult::SearchLimit => reached_limit = true,
        }
        assignments[register as usize] = None;
        if reached_limit {
            return ExactSearchResult::SearchLimit;
        }
    }
    ExactSearchResult::Impossible
}

fn spill_free_allocation_error(
    handler: &str,
    liveness: &Liveness,
    names: &Names,
    gpr_capacity: usize,
    fpr_capacity: usize,
    reason: &str,
) -> CompileError {
    let mut peak_gpr = (0, 0);
    let mut peak_fpr = (0, 0);
    for (instruction, live) in liveness.live_in.iter().enumerate() {
        let gpr = live
            .iter()
            .filter(|register| names.name(**register).class() == VirtualRegisterClass::GeneralPurpose)
            .count();
        let fpr = live.len() - gpr;
        if gpr > peak_gpr.0 {
            peak_gpr = (gpr, instruction);
        }
        if fpr > peak_fpr.0 {
            peak_fpr = (fpr, instruction);
        }
    }
    let witness_instruction = if peak_gpr.0.saturating_sub(gpr_capacity) >= peak_fpr.0.saturating_sub(fpr_capacity) {
        peak_gpr.1
    } else {
        peak_fpr.1
    };
    let witness = liveness.live_in[witness_instruction]
        .iter()
        .take(8)
        .map(|register| names.name(*register).to_string())
        .collect::<Vec<_>>()
        .join(", ");
    allocation_error(
        handler,
        format!(
            "could not allocate spill-free handler: {reason}; peak GPR pressure {}/{} and FPR pressure {}/{}; values live near instruction #{}: {}",
            peak_gpr.0, gpr_capacity, peak_fpr.0, fpr_capacity, witness_instruction, witness
        ),
    )
}

// ============================================================================
// Operand rewriting
// ============================================================================

fn rewrite(
    handler: &TargetFunction,
    instructions: &[MachineInstruction],
    plan: &AllocationPlan,
    runtime: &RuntimeConstants,
) -> Result<Vec<AllocatedInstruction>, CompileError> {
    instructions
        .iter()
        .map(|insn| {
            let operands: Vec<_> = insn
                .operands
                .iter()
                .map(|op| rewrite_operand(handler, op.clone(), plan, runtime))
                .collect::<Result<_, _>>()?;
            if insn.opcode.description().identity_if_inputs_alias && operands.len() == 2 && operands[0] == operands[1] {
                return Ok(None);
            }
            Ok(Some(AllocatedInstruction {
                opcode: insn.opcode.clone(),
                operands,
            }))
        })
        .filter_map(|instruction| instruction.transpose())
        .collect()
}

fn rewrite_operand(
    handler: &TargetFunction,
    op: Operand,
    table: &AllocationPlan,
    runtime: &RuntimeConstants,
) -> Result<AllocatedOperand, CompileError> {
    Ok(match op {
        Operand::VirtualRegister(register) => AllocatedOperand::PhysicalRegister(table.physical(&register)),
        Operand::InterpreterRegister(register) => AllocatedOperand::PhysicalRegister(
            resolve_interpreter_register(register, handler.architecture).ok_or_else(|| {
                allocation_error(
                    &handler.name,
                    format!(
                        "interpreter register '{}' is unavailable on {:?}",
                        register.as_str(),
                        handler.architecture
                    ),
                )
            })?,
        ),
        Operand::PhysicalRegister(register) => {
            if register.architecture() != handler.architecture {
                return Err(allocation_error(
                    &handler.name,
                    format!("register '{register}' belongs to the wrong target architecture"),
                ));
            }
            AllocatedOperand::PhysicalRegister(register)
        }
        Operand::Relocation(relocation) => AllocatedOperand::Relocation(relocation),
        Operand::Immediate(value) => AllocatedOperand::Immediate(value),
        Operand::ValueConstant(_) | Operand::LayoutConstant(_) | Operand::BytecodeField(_) => {
            unreachable!("symbolic operand survived target selection")
        }
        Operand::Address(address) => {
            let adjust_execution_context = handler.architecture == Architecture::X86_64
                && address.base == AddressRegister::Interpreter(InterpreterRegister::ExecutionContext);
            let mut address = PhysicalMemoryAddress {
                base: rewrite_address_register(handler, address.base, table)?,
                index: address
                    .index
                    .map(|register| rewrite_address_register(handler, register, table))
                    .transpose()?,
                scale: address.scale.map(|scale| match scale {
                    crate::low_ir::AddressScale::Immediate(scale) => scale,
                    crate::low_ir::AddressScale::LayoutConstant(_) => {
                        unreachable!("symbolic address scale survived target selection")
                    }
                }),
                displacement: address.displacement.map(|displacement| match displacement {
                    crate::low_ir::AddressDisplacement::Immediate(displacement) => displacement,
                    crate::low_ir::AddressDisplacement::LayoutConstant(_)
                    | crate::low_ir::AddressDisplacement::BytecodeField(_) => {
                        unreachable!("symbolic address displacement survived target selection")
                    }
                }),
            };
            if adjust_execution_context {
                let adjustment = super::finalize_support::required_runtime_constant(
                    runtime,
                    crate::frontend::layout::KnownLayoutConstant::SizeOfExecutionContext,
                    &handler.name,
                )?
                .checked_neg()
                .ok_or_else(|| finalization_error(&handler.name, "execution-context address adjustment overflow"))?;
                let displacement = address
                    .displacement
                    .unwrap_or(0)
                    .checked_add(adjustment)
                    .ok_or_else(|| finalization_error(&handler.name, "machine address displacement overflow"))?;
                address.displacement = (displacement != 0).then_some(displacement);
            }
            AllocatedOperand::Address(address)
        }
        Operand::Label(label) => AllocatedOperand::Label(label),
    })
}

fn rewrite_address_register(
    handler: &TargetFunction,
    register: AddressRegister,
    table: &AllocationPlan,
) -> Result<PhysicalRegister, CompileError> {
    match register {
        AddressRegister::Virtual(register) => Ok(table.physical(&register)),
        AddressRegister::Interpreter(register) => resolve_interpreter_register(register, handler.architecture)
            .ok_or_else(|| {
                allocation_error(
                    &handler.name,
                    format!(
                        "interpreter register '{}' is unavailable on {:?}",
                        register.as_str(),
                        handler.architecture
                    ),
                )
            }),
        AddressRegister::Physical(register) if register.architecture() == handler.architecture => Ok(register),
        AddressRegister::Physical(register) => Err(allocation_error(
            &handler.name,
            format!("register '{register}' belongs to the wrong target architecture"),
        )),
    }
}

// ============================================================================
// Tests
// ============================================================================

#[cfg(test)]
mod tests {
    use super::*;
    use crate::low_ir::preallocate::schedule_x86_relational_operand_loads;
    use crate::low_ir::{
        AddressRegister as SourceAddressRegister, Handler, Instruction as SourceInstruction,
        MemoryAddress as SourceMemoryAddress, Operand as SourceOperand, Program,
        VirtualRegister as SourceVirtualRegister,
    };
    use crate::target::description::{
        BinaryOperation, FloatBinaryOperation, FloatConversion, FloatingPointOperation, IntegerBinaryOperation,
        IntegerSignedness, IntegerWidth, Operation, ShiftOperation,
    };
    use crate::target::ir::AllocatedMemoryAddress;
    use crate::target::selection::select_program;

    macro_rules! instruction {
        ($operation:expr $(, $operand:expr)* $(,)?) => {
            SourceInstruction {
                opcode: $operation.into(),
                operands: vec![$($operand),*],
        }
        };
    }

    fn register(name: &str) -> SourceOperand {
        SourceOperand::VirtualRegister(name.into())
    }

    fn floating_point_register(name: &str) -> SourceOperand {
        SourceOperand::VirtualRegister(SourceVirtualRegister::with_class(
            name,
            VirtualRegisterClass::FloatingPoint,
        ))
    }

    fn x86_register(name: &str) -> SourceOperand {
        SourceOperand::PhysicalRegister(
            physical_register_named(name, Architecture::X86_64).expect("test operand must name an x86-64 register"),
        )
    }

    fn symbol(name: &str) -> SourceOperand {
        SourceOperand::Relocation(crate::low_ir::Relocation::function_call(
            crate::identity::ExternalSymbol::new(name),
        ))
    }

    fn immediate(value: i64) -> SourceOperand {
        SourceOperand::Immediate(value)
    }

    fn constant(name: &str) -> SourceOperand {
        let value = match name {
            "INT32_TAG" => 0x7ffa,
            _ => unreachable!("unknown test constant"),
        };
        let constants = crate::frontend::layout::LayoutConstants::from_values([(name.to_string(), value)]);
        SourceOperand::LayoutConstant(constants.get(name).unwrap())
    }

    fn memory(base: &str, index: Option<&str>) -> SourceOperand {
        let displacement = index.and_then(|index| index.parse().ok());
        let index = index.filter(|index| index.parse::<i64>().is_err());
        SourceOperand::Address(SourceMemoryAddress {
            base: SourceAddressRegister::named(base),
            index: index.map(SourceAddressRegister::named),
            scale: None,
            displacement: displacement.map(crate::low_ir::AddressDisplacement::Immediate),
        })
    }

    fn label(name: &str) -> SourceOperand {
        SourceOperand::Label(name.into())
    }

    fn relational_operand_loads() -> Vec<SourceInstruction> {
        vec![
            instruction!(
                Operation::load_pair(PairWidth::Word),
                register("lhs_index"),
                register("rhs_index"),
                immediate(0),
                immediate(4)
            ),
            instruction!(
                Operation::load(MemoryWidth::DoubleWord, false),
                register("lhs"),
                memory("values", Some("lhs_index"))
            ),
            instruction!(
                Operation::load(MemoryWidth::DoubleWord, false),
                register("rhs"),
                memory("values", Some("rhs_index"))
            ),
            instruction!(
                Operation::branch_tag(EqualityCondition::NotEqual),
                register("lhs"),
                constant("INT32_TAG"),
                label(".slow")
            ),
            instruction!(
                Operation::branch_tag(EqualityCondition::NotEqual),
                register("rhs"),
                constant("INT32_TAG"),
                label(".slow")
            ),
            instruction!(
                Operation::branch_ordered(
                    IntegerWidth::U32,
                    crate::target::description::OrderedCondition::Less,
                    IntegerSignedness::Signed
                ),
                register("lhs"),
                register("rhs"),
                label(".true")
            ),
        ]
    }

    #[test]
    fn schedules_relational_operand_loads_for_x86_64() {
        let mut instructions = relational_operand_loads();
        schedule_x86_relational_operand_loads(&mut instructions);

        assert_eq!(
            instructions
                .iter()
                .map(|instruction| instruction.opcode.operation())
                .collect::<Vec<_>>(),
            [
                Operation::load(MemoryWidth::Word, false),
                Operation::load(MemoryWidth::DoubleWord, false),
                Operation::load(MemoryWidth::Word, false),
                Operation::load(MemoryWidth::DoubleWord, false),
                Operation::branch_tag(EqualityCondition::NotEqual),
                Operation::ExtractTag,
                Operation::branch_equality(IntegerWidth::U16, EqualityCondition::NotEqual),
                Operation::branch_ordered(
                    IntegerWidth::U32,
                    crate::target::description::OrderedCondition::Less,
                    IntegerSignedness::Signed
                ),
            ]
        );
        assert_eq!(instructions[0].operands[0], register("rhs_index"));
        assert_eq!(instructions[2].operands[0], register("lhs_index"));
        assert_eq!(instructions[5].operands, [register("rhs_index"), register("rhs")]);
    }

    fn allocated(instructions: Vec<SourceInstruction>, architecture: Architecture) -> Vec<AllocatedInstruction> {
        build(instructions, architecture).expect("allocation should succeed")
    }

    #[test]
    fn preserves_structured_memory_addresses() {
        let out = allocated(
            vec![
                instruction!(Operation::Move(IntegerWidth::U64), register("index"), immediate(1)),
                instruction!(
                    Operation::load(MemoryWidth::DoubleWord, false),
                    register("value"),
                    SourceOperand::Address(SourceMemoryAddress {
                        base: SourceAddressRegister::named("values"),
                        index: Some(SourceAddressRegister::named("index")),
                        scale: Some(crate::low_ir::AddressScale::Immediate(8)),
                        displacement: None,
                    })
                ),
            ],
            Architecture::X86_64,
        );
        let address = find_operation(&out, Operation::load(MemoryWidth::DoubleWord, false))
            .operands
            .get(1);
        assert!(matches!(
            address,
            Some(AllocatedOperand::Address(AllocatedMemoryAddress {
                base,
                index: Some(_),
                scale: Some(8),
                displacement: None,
            })) if *base == resolve_interpreter_register(
                InterpreterRegister::Values,
                Architecture::X86_64,
            ).unwrap()
        ));
    }

    fn build(
        instructions: Vec<SourceInstruction>,
        arch: Architecture,
    ) -> Result<Vec<AllocatedInstruction>, CompileError> {
        let mut program = Program::new();
        program.handlers.push(Handler {
            id: crate::identity::HandlerId::new(0),
            name: "Test".to_string(),
            size: None,
            is_cold: false,
            instructions,
        });
        program.handlers[0].size = Some(4);
        crate::low_ir::lowering::materialize_layout_constants(&mut program.handlers[0]);
        let selected = select_program(program, arch)?;
        allocate_handler(&selected.functions[0], &selected.runtime)
            .map(|graph| graph.linearize_for_analysis().instructions)
    }

    fn is_operation(instruction: &AllocatedInstruction, operation: Operation) -> bool {
        instruction.opcode.operation() == operation
    }

    fn operation_count(instructions: &[AllocatedInstruction], operation: Operation) -> usize {
        instructions
            .iter()
            .filter(|instruction| is_operation(instruction, operation))
            .count()
    }

    fn find_operation(instructions: &[AllocatedInstruction], operation: Operation) -> &AllocatedInstruction {
        instructions
            .iter()
            .find(|instruction| is_operation(instruction, operation))
            .unwrap_or_else(|| panic!("no '{operation:?}' instruction found"))
    }

    fn assignment_for(insns: &[AllocatedInstruction], operation: Operation, op_index: usize) -> String {
        for insn in insns {
            if is_operation(insn, operation)
                && let Some(name) = insn.operands.get(op_index).and_then(AllocatedOperand::register_name)
            {
                return name.to_string();
            }
        }
        panic!("no '{operation:?}' instruction found");
    }

    fn allocated_helper_call(architecture: Architecture) -> Vec<AllocatedInstruction> {
        allocated(
            vec![
                instruction!(Operation::Move(IntegerWidth::U64), register("value"), immediate(1)),
                instruction!(
                    Operation::Call(CallKind::Helper),
                    symbol("asm_helper_to_boolean"),
                    register("value"),
                    register("result")
                ),
                instruction!(
                    Operation::branch_zero(IntegerWidth::U64, ZeroCondition::NonZero),
                    register("result"),
                    label(".take")
                ),
                instruction!(Operation::Control(ControlOperation::DispatchNext)),
                instruction!(Operation::Label, label(".take")),
                instruction!(Operation::LoadLabel, register("value"), immediate(0)),
                instruction!(Operation::Control(ControlOperation::GotoHandler), register("value")),
            ],
            architecture,
        )
    }

    #[test]
    fn allocates_simple_virtual_register() {
        let out = allocated(
            vec![
                instruction!(Operation::Move(IntegerWidth::U64), register("foo"), immediate(42)),
                instruction!(
                    Operation::IntegerBinary {
                        operation: IntegerBinaryOperation::Binary(BinaryOperation::Add),
                        width: IntegerWidth::U64
                    },
                    register("foo"),
                    immediate(1)
                ),
                instruction!(Operation::StoreOperand, immediate(0), register("foo")),
            ],
            Architecture::X86_64,
        );
        // `foo` should be assigned to one of rax..r11.
        let name = assignment_for(&out, Operation::Move(IntegerWidth::U64), 0);
        assert!(
            ["rax", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11"]
                .iter()
                .any(|p| *p == name),
            "expected physical reg, got {name}"
        );
    }

    #[test]
    fn exact_fallback_colors_a_graph_that_defeats_greedy_ordering() {
        let function = crate::identity::HandlerId::new(0);
        let mut seed = (0..6)
            .map(|index| {
                instruction!(
                    Operation::Move(IntegerWidth::U64),
                    register(&format!("crown_{index}")),
                    immediate(index)
                )
            })
            .collect::<Vec<_>>();
        let virtual_registers = crate::low_ir::intern_virtual_registers(function, &mut seed);
        let handler = TargetFunction {
            id: function,
            name: "Crown".to_string(),
            size: None,
            is_cold: false,
            architecture: Architecture::X86_64,
            virtual_registers,
            instructions: Vec::new(),
        };
        let names = Names::new(function, &handler.virtual_registers);
        let mut interference = InterferenceGraph {
            neighbors: vec![Vec::new(); 6],
            nodes: vec![Interference::default(); 6],
        };
        for lhs in 0..3u32 {
            for rhs in 0..3u32 {
                if lhs == rhs {
                    continue;
                }
                let rhs = rhs + 3;
                interference.neighbors[lhs as usize].push(rhs);
                interference.neighbors[rhs as usize].push(lhs);
            }
        }
        for neighbors in &mut interference.neighbors {
            neighbors.sort_unstable();
        }
        let registers = register_info_for(Architecture::X86_64);
        let gpr_pool = registers.temporaries.to_vec();
        let fpr_pool = registers.fp_temporaries.to_vec();
        let gpr_mask = gpr_pool.iter().fold(0, |set, register| set | register_bit(*register));
        let allowed = register_bit(gpr_pool[0]) | register_bit(gpr_pool[1]);
        for node in &mut interference.nodes {
            node.forbidden = gpr_mask & !allowed;
        }
        let context = ColoringContext {
            handler: &handler,
            interference: &interference,
            names: &names,
            gpr_pool: &gpr_pool,
            fpr_pool: &fpr_pool,
            gpr_mask,
            fpr_mask: fpr_pool.iter().fold(0, |set, register| set | register_bit(*register)),
        };
        let greedy_order = [0, 3, 1, 4, 2, 5];
        assert!(color(&context, &greedy_order, &CoalescingGroups::empty(6)).is_err());
        let ExactColorResult::Colored(plan) = exact_color(&context) else {
            panic!("the exact fallback should find the two-coloring");
        };
        for register in 0..6u32 {
            let physical = plan.0[register as usize].unwrap();
            assert_ne!(register_bit(physical) & allowed, 0);
            for neighbor in interference.neighbors(register) {
                assert_ne!(plan.0[*neighbor as usize], Some(physical));
            }
        }
    }

    #[test]
    fn liveness_normalizes_register_sets_before_solving() {
        let liveness = solve_liveness(
            vec![UseDef {
                uses: vec![2, 0, 2, 1],
                defs: vec![4, 3, 4],
                clobbers: 0,
            }],
            &[Vec::new()],
        );

        assert_eq!(liveness.use_def[0].uses, [0, 1, 2]);
        assert_eq!(liveness.use_def[0].defs, [3, 4]);
        assert_eq!(liveness.live_in[0], [0, 1, 2]);
    }

    fn reference_liveness(use_def: Vec<UseDef>, successors: &[Vec<usize>]) -> Liveness {
        let mut live_in = vec![LiveSet::new(); use_def.len()];
        let mut live_out = vec![LiveSet::new(); use_def.len()];
        loop {
            let mut changed = false;
            for instruction in (0..use_def.len()).rev() {
                let mut next_out = successors[instruction]
                    .iter()
                    .flat_map(|successor| live_in[*successor].iter().copied())
                    .collect::<LiveSet>();
                next_out.sort_unstable();
                next_out.dedup();

                let mut next_in = use_def[instruction].uses.clone();
                next_in.extend(
                    next_out
                        .iter()
                        .copied()
                        .filter(|register| !use_def[instruction].defs.contains(register)),
                );
                next_in.sort_unstable();
                next_in.dedup();
                changed |= live_in[instruction] != next_in || live_out[instruction] != next_out;
                live_in[instruction] = next_in;
                live_out[instruction] = next_out;
            }
            if !changed {
                return Liveness {
                    live_in,
                    live_out,
                    use_def,
                };
            }
        }
    }

    #[test]
    fn liveness_matches_reference_solver_on_random_cfgs() {
        let mut random_state = 0x6a09e667f3bcc909u64;
        let mut random = || {
            random_state = random_state
                .wrapping_mul(6364136223846793005)
                .wrapping_add(1442695040888963407);
            random_state
        };

        for _ in 0..250 {
            let instruction_count = (random() % 20 + 1) as usize;
            let register_count = (random() % 12 + 1) as u32;
            let mut use_def = Vec::with_capacity(instruction_count);
            for _ in 0..instruction_count {
                let mut instruction = UseDef::default();
                for register in 0..register_count {
                    if random() % 5 == 0 {
                        instruction.uses.push(register);
                    }
                    if random() % 7 == 0 {
                        instruction.defs.push(register);
                    }
                }
                use_def.push(instruction);
            }

            let mut successors = vec![Vec::new(); instruction_count];
            for instruction_successors in &mut successors {
                for successor in 0..instruction_count {
                    if random() % 9 == 0 {
                        instruction_successors.push(successor);
                    }
                }
            }

            assert_eq!(
                solve_liveness(use_def.clone(), &successors),
                reference_liveness(use_def, &successors)
            );
        }
    }

    #[test]
    fn exact_fallback_enforces_resource_limits() {
        let function = crate::identity::HandlerId::new(0);
        let mut seed = vec![instruction!(
            Operation::Move(IntegerWidth::U64),
            register("value"),
            immediate(0)
        )];
        let virtual_registers = crate::low_ir::intern_virtual_registers(function, &mut seed);
        let handler = TargetFunction {
            id: function,
            name: "Bounded".to_string(),
            size: None,
            is_cold: false,
            architecture: Architecture::X86_64,
            virtual_registers,
            instructions: Vec::new(),
        };
        let names = Names::new(function, &handler.virtual_registers);
        let registers = register_info_for(Architecture::X86_64);
        let gpr_pool = registers.temporaries.to_vec();
        let fpr_pool = registers.fp_temporaries.to_vec();
        let gpr_mask = gpr_pool.iter().fold(0, |set, register| set | register_bit(*register));
        let fpr_mask = fpr_pool.iter().fold(0, |set, register| set | register_bit(*register));

        let oversized_interference = InterferenceGraph {
            neighbors: vec![Vec::new(); EXACT_COLOR_REGISTER_LIMIT + 1],
            nodes: vec![Interference::default(); EXACT_COLOR_REGISTER_LIMIT + 1],
        };
        let oversized_context = ColoringContext {
            handler: &handler,
            interference: &oversized_interference,
            names: &names,
            gpr_pool: &gpr_pool,
            fpr_pool: &fpr_pool,
            gpr_mask,
            fpr_mask,
        };
        assert!(matches!(exact_color(&oversized_context), ExactColorResult::SearchLimit));

        let interference = InterferenceGraph {
            neighbors: vec![Vec::new()],
            nodes: vec![Interference::default()],
        };
        let context = ColoringContext {
            handler: &handler,
            interference: &interference,
            names: &names,
            gpr_pool: &gpr_pool,
            fpr_pool: &fpr_pool,
            gpr_mask,
            fpr_mask,
        };
        let mut assignments = vec![None];
        let mut budget = ExactColorBudget { remaining: 0 };
        assert!(matches!(
            exact_color_search(&context, &mut assignments, &mut budget),
            ExactSearchResult::SearchLimit
        ));
    }

    #[test]
    fn coalesces_checked_integer_refinement_moves() {
        let instructions = allocated(
            vec![
                instruction!(Operation::Move(IntegerWidth::U64), register("wide"), immediate(1)),
                instruction!(
                    Operation::branch_bit(TestCondition::Set),
                    register("wide"),
                    immediate(31),
                    label(".wide")
                ),
                instruction!(Operation::Move(IntegerWidth::U64), register("narrow"), register("wide")),
                instruction!(Operation::Assertion(AssertionOperation::NonZero), register("narrow")),
                instruction!(Operation::Control(ControlOperation::JumpLabel), label(".done")),
                instruction!(Operation::Label, label(".wide")),
                instruction!(Operation::Assertion(AssertionOperation::NonZero), register("wide")),
                instruction!(Operation::Label, label(".done")),
                instruction!(Operation::Control(ControlOperation::DispatchNext)),
            ],
            Architecture::X86_64,
        );

        assert_eq!(
            assignment_for(&instructions, Operation::branch_bit(TestCondition::Set), 0),
            assignment_for(&instructions, Operation::Assertion(AssertionOperation::NonZero), 0)
        );
        assert_eq!(operation_count(&instructions, Operation::Move(IntegerWidth::U64)), 1);
    }

    #[test]
    fn coalesces_noninterfering_moves() {
        let instructions = allocated(
            vec![
                instruction!(Operation::Move(IntegerWidth::U64), register("source"), immediate(1)),
                instruction!(
                    Operation::Move(IntegerWidth::U64),
                    register("destination"),
                    register("source")
                ),
                instruction!(
                    Operation::Assertion(AssertionOperation::NonZero),
                    register("destination")
                ),
                instruction!(Operation::Control(ControlOperation::DispatchNext)),
            ],
            Architecture::X86_64,
        );

        assert_eq!(operation_count(&instructions, Operation::Move(IntegerWidth::U64)), 1);
    }

    #[test]
    fn coalesces_noninterfering_float_moves() {
        let instructions = allocated(
            vec![
                instruction!(
                    Operation::FloatMove,
                    floating_point_register("source"),
                    x86_register("xmm5")
                ),
                instruction!(
                    Operation::FloatMove,
                    floating_point_register("destination"),
                    floating_point_register("source")
                ),
                instruction!(
                    Operation::Float(FloatingPointOperation::Binary(FloatBinaryOperation::Add)),
                    floating_point_register("destination"),
                    floating_point_register("destination")
                ),
                instruction!(Operation::Control(ControlOperation::DispatchNext)),
            ],
            Architecture::X86_64,
        );

        assert_eq!(operation_count(&instructions, Operation::FloatMove), 1);
    }

    #[test]
    fn coalesces_transitive_move_chains_around_interference() {
        let instructions = allocated(
            vec![
                instruction!(Operation::Move(IntegerWidth::U64), register("blocker"), immediate(1)),
                instruction!(Operation::Move(IntegerWidth::U64), register("producer"), immediate(2)),
                instruction!(Operation::Assertion(AssertionOperation::NonZero), register("blocker")),
                instruction!(Operation::Assertion(AssertionOperation::NonZero), register("blocker")),
                instruction!(Operation::Assertion(AssertionOperation::NonZero), register("blocker")),
                instruction!(Operation::Assertion(AssertionOperation::NonZero), register("blocker")),
                instruction!(
                    Operation::Move(IntegerWidth::U64),
                    register("middle"),
                    register("producer")
                ),
                instruction!(
                    Operation::Move(IntegerWidth::U64),
                    register("consumer"),
                    register("middle")
                ),
                instruction!(Operation::Assertion(AssertionOperation::NonZero), register("consumer")),
                instruction!(Operation::Assertion(AssertionOperation::NonZero), register("consumer")),
                instruction!(Operation::Control(ControlOperation::DispatchNext)),
            ],
            Architecture::X86_64,
        );

        assert_eq!(operation_count(&instructions, Operation::Move(IntegerWidth::U64)), 2);
    }

    #[test]
    fn coalesces_compatible_subgroups_of_move_graphs() {
        let instructions = allocated(
            vec![
                instruction!(Operation::Move(IntegerWidth::U64), register("source"), immediate(2)),
                instruction!(Operation::Move(IntegerWidth::U64), register("live"), register("source")),
                instruction!(Operation::Move(IntegerWidth::U64), register("local"), register("live")),
                instruction!(
                    Operation::IntegerBinary {
                        operation: IntegerBinaryOperation::Binary(BinaryOperation::Subtract),
                        width: IntegerWidth::U64
                    },
                    register("local"),
                    immediate(1)
                ),
                instruction!(Operation::Assertion(AssertionOperation::NonZero), register("live")),
                instruction!(Operation::Assertion(AssertionOperation::NonZero), register("local")),
                instruction!(Operation::Control(ControlOperation::DispatchNext)),
            ],
            Architecture::X86_64,
        );

        assert_eq!(operation_count(&instructions, Operation::Move(IntegerWidth::U64)), 2);
    }

    #[test]
    fn coalesces_commutative_updates_with_the_dying_input() {
        let instructions = allocated(
            vec![
                instruction!(Operation::Move(IntegerWidth::U64), register("lhs"), immediate(1)),
                instruction!(Operation::Move(IntegerWidth::U64), register("rhs"), immediate(2)),
                instruction!(Operation::Move(IntegerWidth::U64), register("result"), register("lhs")),
                instruction!(
                    Operation::IntegerBinary {
                        operation: IntegerBinaryOperation::Binary(BinaryOperation::Add),
                        width: IntegerWidth::U64
                    },
                    register("result"),
                    register("rhs")
                ),
                instruction!(Operation::Assertion(AssertionOperation::NonZero), register("lhs")),
                instruction!(Operation::Assertion(AssertionOperation::NonZero), register("result")),
                instruction!(Operation::Control(ControlOperation::DispatchNext)),
            ],
            Architecture::X86_64,
        );

        assert_eq!(operation_count(&instructions, Operation::Move(IntegerWidth::U64)), 2);
    }

    #[test]
    fn coalesces_noninterfering_instruction_inputs_and_outputs() {
        let instructions = allocated(
            vec![
                instruction!(Operation::Move(IntegerWidth::U64), register("boxed"), immediate(1)),
                instruction!(Operation::UnboxObject, register("object"), register("boxed")),
                instruction!(Operation::Assertion(AssertionOperation::NonZero), register("object")),
                instruction!(Operation::Control(ControlOperation::DispatchNext)),
            ],
            Architecture::X86_64,
        );
        let unbox = find_operation(&instructions, Operation::UnboxObject);

        assert_eq!(unbox.operands[0], unbox.operands[1]);
    }

    #[test]
    fn allocates_overlapping_virtuals_to_distinct_registers() {
        let out = allocated(
            vec![
                instruction!(Operation::Move(IntegerWidth::U64), register("a"), immediate(1)),
                instruction!(Operation::Move(IntegerWidth::U64), register("b"), immediate(2)),
                instruction!(
                    Operation::IntegerBinary {
                        operation: IntegerBinaryOperation::Binary(BinaryOperation::Add),
                        width: IntegerWidth::U64
                    },
                    register("a"),
                    register("b")
                ),
            ],
            Architecture::X86_64,
        );
        let a = assignment_for(&out, Operation::Move(IntegerWidth::U64), 0);
        let b = out
            .iter()
            .filter(|instruction| is_operation(instruction, Operation::Move(IntegerWidth::U64)))
            .nth(1)
            .expect("second move")
            .operands[0]
            .register_name()
            .expect("mov destination must be a register");
        assert_ne!(a, b, "overlapping virtuals must get distinct regs");
    }

    #[test]
    fn pins_virtual_register_to_rcx_for_shift_count() {
        let out = allocated(
            vec![
                instruction!(Operation::Move(IntegerWidth::U64), register("value"), immediate(0xff)),
                instruction!(Operation::Move(IntegerWidth::U64), register("count"), immediate(3)),
                instruction!(
                    Operation::IntegerBinary {
                        operation: IntegerBinaryOperation::Shift(ShiftOperation::RightLogical),
                        width: IntegerWidth::U64
                    },
                    register("value"),
                    register("count")
                ),
            ],
            Architecture::X86_64,
        );
        let shift = find_operation(
            &out,
            Operation::IntegerBinary {
                operation: IntegerBinaryOperation::Shift(ShiftOperation::RightLogical),
                width: IntegerWidth::U64,
            },
        );
        assert_eq!(shift.operands[1].register_name(), Some("rcx"));
    }

    #[test]
    fn distinguishes_virtual_register_classes_with_the_same_name() {
        let instructions = allocated(
            vec![
                instruction!(Operation::Move(IntegerWidth::U64), register("value"), immediate(1)),
                instruction!(
                    Operation::Float(FloatingPointOperation::Convert(FloatConversion::Int32ToFloat64)),
                    floating_point_register("value"),
                    register("value")
                ),
                instruction!(Operation::Control(ControlOperation::DispatchNext)),
            ],
            Architecture::X86_64,
        );

        assert!(
            assignment_for(
                &instructions,
                Operation::Float(FloatingPointOperation::Convert(FloatConversion::Int32ToFloat64)),
                0
            )
            .starts_with("xmm")
        );
        assert!(
            !assignment_for(
                &instructions,
                Operation::Float(FloatingPointOperation::Convert(FloatConversion::Int32ToFloat64)),
                1
            )
            .starts_with("xmm")
        );
    }

    #[test]
    fn virtual_register_names_do_not_alias_interpreter_registers() {
        let instructions = allocated(
            vec![
                instruction!(Operation::Move(IntegerWidth::U64), register("pc"), immediate(1)),
                instruction!(Operation::StoreOperand, immediate(0), register("pc")),
            ],
            Architecture::X86_64,
        );

        assert_ne!(
            assignment_for(&instructions, Operation::Move(IntegerWidth::U64), 0),
            "r13"
        );
    }

    #[test]
    fn call_helper_pins_virtual_input_and_output_to_rcx_and_rax() {
        // The input is pinned to rcx and the output to rax.
        // The input dies at the call (so being in the killed set is OK),
        // and the output is born there (so it survives).
        let instructions = allocated_helper_call(Architecture::X86_64);
        let call = find_operation(&instructions, Operation::Call(CallKind::Helper));
        assert_eq!(call.operands[1].register_name(), Some("rcx"));
        assert_eq!(call.operands[2].register_name(), Some("rax"));
    }

    #[test]
    fn aarch64_call_helper_virtual_input_and_output_share_x0() {
        // On aarch64, the call can put the dying input and
        // newly-born output in x0, which is the ABI argument and result
        // register. That avoids a redundant x1 -> x0 bridge.
        let instructions = allocated_helper_call(Architecture::Aarch64);
        let call = find_operation(&instructions, Operation::Call(CallKind::Helper));
        assert_eq!(call.operands[1].register_name(), Some("x0"));
        assert_eq!(call.operands[2].register_name(), Some("x0"));
    }

    #[test]
    fn prioritizes_move_affinities_with_abi_pinned_values() {
        let instructions = allocated(
            vec![
                instruction!(Operation::Move(IntegerWidth::U64), register("function"), immediate(1)),
                instruction!(
                    Operation::Call(CallKind::RawNative),
                    register("function"),
                    register("native_return"),
                    register("variant")
                ),
                instruction!(
                    Operation::Move(IntegerWidth::U64),
                    register("payload"),
                    register("native_return")
                ),
                instruction!(Operation::Move(IntegerWidth::U64), register("unrelated"), immediate(2)),
                instruction!(
                    Operation::IntegerBinary {
                        operation: IntegerBinaryOperation::Binary(BinaryOperation::Add),
                        width: IntegerWidth::U64
                    },
                    register("unrelated"),
                    immediate(3)
                ),
                instruction!(
                    Operation::branch_zero(IntegerWidth::U64, ZeroCondition::NonZero),
                    register("variant"),
                    label(".exception")
                ),
                instruction!(Operation::StoreOperand, immediate(0), register("payload")),
                instruction!(Operation::Control(ControlOperation::DispatchNext)),
                instruction!(Operation::Label, label(".exception")),
                instruction!(
                    Operation::Move(IntegerWidth::U64),
                    register("helper_argument"),
                    register("payload")
                ),
                instruction!(
                    Operation::Call(CallKind::Helper),
                    symbol("asm_helper_handle_raw_native_exception"),
                    register("helper_argument"),
                    register("helper_result")
                ),
                instruction!(Operation::Control(ControlOperation::Exit)),
            ],
            Architecture::Aarch64,
        );

        assert_eq!(
            assignment_for(&instructions, Operation::Call(CallKind::RawNative), 1),
            "x0"
        );
        assert_eq!(assignment_for(&instructions, Operation::StoreOperand, 1), "x0");
        assert_eq!(
            assignment_for(&instructions, Operation::Call(CallKind::Helper), 1),
            "x0"
        );
    }

    #[test]
    fn prioritizes_constrained_move_sources() {
        let instructions = allocated(
            vec![
                instruction!(
                    Operation::Float(FloatingPointOperation::Convert(FloatConversion::Float64ToInt32)),
                    register("raw"),
                    x86_register("xmm0"),
                    label(".fail")
                ),
                instruction!(
                    Operation::Move(IntegerWidth::U64),
                    register("parameter"),
                    register("raw")
                ),
                instruction!(Operation::Assertion(AssertionOperation::NonZero), register("parameter")),
                instruction!(Operation::Assertion(AssertionOperation::NonZero), register("parameter")),
                instruction!(Operation::Control(ControlOperation::DispatchNext)),
                instruction!(Operation::Label, label(".fail")),
                instruction!(Operation::Control(ControlOperation::Exit)),
            ],
            Architecture::X86_64,
        );

        assert_eq!(
            assignment_for(
                &instructions,
                Operation::Float(FloatingPointOperation::Convert(FloatConversion::Float64ToInt32)),
                0
            ),
            assignment_for(&instructions, Operation::Assertion(AssertionOperation::NonZero), 0)
        );
        assert!(
            !instructions
                .iter()
                .any(|instruction| is_operation(instruction, Operation::Move(IntegerWidth::U64)))
        );
    }

    #[test]
    fn rematerializes_virtual_register_live_across_call() {
        let out = build(
            vec![
                instruction!(Operation::Move(IntegerWidth::U64), register("value"), immediate(7)),
                instruction!(
                    Operation::Call(CallKind::Helper),
                    symbol("asm_helper_to_boolean"),
                    x86_register("rcx"),
                    x86_register("rax")
                ),
                instruction!(
                    Operation::IntegerBinary {
                        operation: IntegerBinaryOperation::Binary(BinaryOperation::Add),
                        width: IntegerWidth::U64
                    },
                    register("value"),
                    immediate(1)
                ),
                instruction!(Operation::Assertion(AssertionOperation::NonZero), register("value")),
            ],
            Architecture::X86_64,
        )
        .expect("the constant should be rematerialized after the call");

        assert_eq!(
            out.iter()
                .map(|instruction| instruction.opcode.operation())
                .collect::<Vec<_>>(),
            [
                Operation::Move(IntegerWidth::U64),
                Operation::Call(CallKind::Helper),
                Operation::Move(IntegerWidth::U64),
                Operation::IntegerBinary {
                    operation: IntegerBinaryOperation::Binary(BinaryOperation::Add),
                    width: IntegerWidth::U64,
                },
                Operation::Assertion(AssertionOperation::NonZero),
            ]
        );
        assert_eq!(out[0].operands[1], AllocatedOperand::Immediate(7));
        assert_eq!(out[2].operands[1], AllocatedOperand::Immediate(7));
        assert_eq!(out[2].operands[0], out[3].operands[0]);
        assert_eq!(out[3].operands[0], out[4].operands[0]);
    }

    #[test]
    fn rematerializes_multiple_widths_after_the_last_of_multiple_calls() {
        for architecture in [Architecture::X86_64, Architecture::Aarch64] {
            let out = allocated(
                vec![
                    instruction!(Operation::Move(IntegerWidth::U32), register("small"), immediate(7)),
                    instruction!(Operation::Move(IntegerWidth::U64), register("large"), immediate(9)),
                    instruction!(Operation::Move(IntegerWidth::U64), register("argument"), immediate(1)),
                    instruction!(
                        Operation::Call(CallKind::Helper),
                        symbol("asm_helper_to_boolean"),
                        register("argument"),
                        register("first_result")
                    ),
                    instruction!(
                        Operation::Call(CallKind::Helper),
                        symbol("asm_helper_to_boolean"),
                        register("argument"),
                        register("second_result")
                    ),
                    instruction!(Operation::Assertion(AssertionOperation::NonZero), register("small")),
                    instruction!(Operation::Assertion(AssertionOperation::NonZero), register("large")),
                ],
                architecture,
            );

            assert_eq!(operation_count(&out, Operation::Move(IntegerWidth::U32)), 2);
            assert_eq!(operation_count(&out, Operation::Move(IntegerWidth::U64)), 4);
            let second_call = out
                .iter()
                .rposition(|instruction| is_operation(instruction, Operation::Call(CallKind::Helper)))
                .unwrap();
            let rematerialized = &out[second_call + 1..second_call + 3];
            assert!(
                rematerialized
                    .iter()
                    .any(|instruction| is_operation(instruction, Operation::Move(IntegerWidth::U32)))
            );
            assert!(
                rematerialized
                    .iter()
                    .any(|instruction| is_operation(instruction, Operation::Move(IntegerWidth::U64)))
            );
        }
    }

    #[test]
    fn rematerializes_a_constant_used_by_an_address() {
        for architecture in [Architecture::X86_64, Architecture::Aarch64] {
            let out = allocated(
                vec![
                    instruction!(Operation::Move(IntegerWidth::U64), register("index"), immediate(1)),
                    instruction!(Operation::Move(IntegerWidth::U64), register("argument"), immediate(1)),
                    instruction!(
                        Operation::Call(CallKind::Helper),
                        symbol("asm_helper_to_boolean"),
                        register("argument"),
                        register("result")
                    ),
                    instruction!(
                        Operation::load(MemoryWidth::DoubleWord, false),
                        register("value"),
                        SourceOperand::Address(SourceMemoryAddress {
                            base: SourceAddressRegister::Interpreter(InterpreterRegister::Values),
                            index: Some(SourceAddressRegister::named("index")),
                            scale: Some(crate::low_ir::AddressScale::Immediate(8)),
                            displacement: None,
                        })
                    ),
                    instruction!(Operation::Assertion(AssertionOperation::NonZero), register("value")),
                ],
                architecture,
            );

            assert_eq!(operation_count(&out, Operation::Move(IntegerWidth::U64)), 3);
        }
    }

    #[test]
    fn rejects_nonrematerializable_virtual_register_live_across_call() {
        let err = build(
            vec![
                instruction!(Operation::Move(IntegerWidth::U64), register("seed"), immediate(7)),
                instruction!(Operation::Move(IntegerWidth::U64), register("value"), register("seed")),
                instruction!(
                    Operation::Call(CallKind::Helper),
                    symbol("asm_helper_to_boolean"),
                    x86_register("rcx"),
                    x86_register("rax")
                ),
                instruction!(
                    Operation::IntegerBinary {
                        operation: IntegerBinaryOperation::Binary(BinaryOperation::Add),
                        width: IntegerWidth::U64
                    },
                    register("value"),
                    immediate(1)
                ),
            ],
            Architecture::X86_64,
        )
        .expect_err("a nonrematerializable value cannot survive the call");
        assert!(
            err.message.contains("could not allocate spill-free handler") && err.message.contains("peak GPR pressure"),
            "unexpected error: {err:?}"
        );
    }

    #[test]
    fn does_not_rematerialize_over_a_call_result() {
        let out = allocated(
            vec![
                instruction!(Operation::Move(IntegerWidth::U64), register("value"), immediate(7)),
                instruction!(
                    Operation::Call(CallKind::Helper),
                    symbol("asm_helper_to_boolean"),
                    x86_register("rcx"),
                    register("value")
                ),
                instruction!(Operation::Assertion(AssertionOperation::NonZero), register("value")),
            ],
            Architecture::X86_64,
        );

        assert_eq!(
            out.iter()
                .map(|instruction| instruction.opcode.operation())
                .collect::<Vec<_>>(),
            [
                Operation::Move(IntegerWidth::U64),
                Operation::Call(CallKind::Helper),
                Operation::Assertion(AssertionOperation::NonZero),
            ]
        );
    }

    #[test]
    fn does_not_split_a_live_range_across_machine_blocks() {
        let err = build(
            vec![
                instruction!(Operation::Move(IntegerWidth::U64), register("value"), immediate(7)),
                instruction!(
                    Operation::Call(CallKind::Helper),
                    symbol("asm_helper_to_boolean"),
                    x86_register("rcx"),
                    x86_register("rax")
                ),
                instruction!(Operation::Control(ControlOperation::JumpLabel), label(".use")),
                instruction!(Operation::Label, label(".use")),
                instruction!(
                    Operation::IntegerBinary {
                        operation: IntegerBinaryOperation::Binary(BinaryOperation::Add),
                        width: IntegerWidth::U64
                    },
                    register("value"),
                    immediate(1)
                ),
            ],
            Architecture::X86_64,
        )
        .expect_err("cross-block rematerialization requires edge splitting");

        assert!(
            err.message.contains("could not allocate") || err.message.contains("pool exhausted"),
            "unexpected error: {err:?}"
        );
    }

    #[test]
    fn a_result_nothing_reads_still_conflicts_with_what_outlives_it() {
        // Nothing ever reads `discarded`, so it belongs to no live set, but the
        // instruction writing it still runs while `survivor` has to survive.
        // Handing both the same register would destroy `survivor`.
        let out = build(
            vec![
                instruction!(Operation::Move(IntegerWidth::U64), register("survivor"), immediate(7)),
                instruction!(Operation::Move(IntegerWidth::U64), register("discarded"), immediate(9)),
                instruction!(
                    Operation::IntegerBinary {
                        operation: IntegerBinaryOperation::Binary(BinaryOperation::Add),
                        width: IntegerWidth::U64
                    },
                    register("survivor"),
                    immediate(1)
                ),
            ],
            Architecture::X86_64,
        )
        .expect("allocation should succeed");

        let survivor = out[0]
            .operands
            .first()
            .and_then(AllocatedOperand::register_name)
            .expect("the surviving value should be in a register");
        let discarded = out[1]
            .operands
            .first()
            .and_then(AllocatedOperand::register_name)
            .expect("the discarded value should be in a register");
        assert_ne!(
            survivor, discarded,
            "a discarded result took the register of a value that outlives it"
        );
    }

    #[test]
    fn separates_gpr_and_fpr_pools() {
        let out = allocated(
            vec![
                instruction!(Operation::Move(IntegerWidth::U64), register("i"), immediate(1)),
                instruction!(
                    Operation::Float(FloatingPointOperation::Convert(FloatConversion::Int32ToFloat64)),
                    floating_point_register("f"),
                    register("i")
                ),
                instruction!(Operation::FloatMove, register("i"), floating_point_register("f")),
            ],
            Architecture::X86_64,
        );
        // `i` should land in a GPR; `f` should land in an FPR.
        let i_reg = assignment_for(&out, Operation::Move(IntegerWidth::U64), 0);
        assert!(
            ["rax", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11"]
                .iter()
                .any(|p| *p == i_reg)
        );
        let name = find_operation(&out, Operation::FloatMove).operands[1]
            .register_name()
            .expect("floating-point move source");
        assert!(["xmm0", "xmm1", "xmm2", "xmm3"].contains(&name));
    }

    #[test]
    fn xor_self_zeroes_without_a_phantom_use() {
        // The `xor reg, reg` idiom must not look like a self-read; if it
        // did, the allocator would treat the virtual register as live at the handler
        // entry and reject the program. A target-selected scratch does not
        // change the two semantic operands.
        for architecture in [Architecture::X86_64, Architecture::Aarch64] {
            allocated(
                vec![
                    instruction!(
                        Operation::IntegerBinary {
                            operation: IntegerBinaryOperation::Binary(BinaryOperation::Xor),
                            width: IntegerWidth::U64
                        },
                        register("counter"),
                        register("counter")
                    ),
                    instruction!(
                        Operation::IntegerBinary {
                            operation: IntegerBinaryOperation::Binary(BinaryOperation::Add),
                            width: IntegerWidth::U64
                        },
                        register("counter"),
                        immediate(1)
                    ),
                    instruction!(Operation::StoreOperand, immediate(0), register("counter")),
                    instruction!(Operation::Control(ControlOperation::DispatchNext)),
                ],
                architecture,
            );
        }
    }

    #[test]
    fn modulo_pins_output_and_keeps_inputs_off_rax_rdx() {
        // x86 idiv reads rax/rdx, so dividend / divisor operands must not
        // be allocated to rax or rdx; the remainder is pinned to rdx and
        // rax is an explicit scratch register.
        let out = allocated(
            vec![
                instruction!(Operation::Move(IntegerWidth::U64), register("dividend"), immediate(1)),
                instruction!(Operation::Move(IntegerWidth::U64), register("divisor"), immediate(2)),
                instruction!(
                    Operation::Modulo(IntegerWidth::U32),
                    register("rem"),
                    register("dividend"),
                    register("divisor")
                ),
                instruction!(Operation::BoxInt32 { clean: false }, register("dst"), register("rem")),
                instruction!(Operation::StoreOperand, immediate(0), register("dst")),
                instruction!(Operation::Control(ControlOperation::DispatchNext)),
            ],
            Architecture::X86_64,
        );
        let names = find_operation(&out, Operation::Modulo(IntegerWidth::U32))
            .operands
            .iter()
            .map(|operand| operand.register_name().unwrap_or("<non-reg>"))
            .collect::<Vec<_>>();
        assert_eq!(names[0], "rdx");
        assert!(!["rax", "rdx"].contains(&names[1]));
        assert!(!["rax", "rdx"].contains(&names[2]));
        assert_eq!(names[3], "rax");
    }

    #[test]
    fn explicit_scratch_keeps_aarch64_operands_off_x9() {
        let out = allocated(
            vec![
                instruction!(Operation::Move(IntegerWidth::U64), register("lhs"), immediate(6)),
                instruction!(Operation::Move(IntegerWidth::U64), register("rhs"), immediate(7)),
                instruction!(
                    Operation::Overflow(crate::target::description::OverflowOperation::MultiplyCopy,),
                    register("result"),
                    register("lhs"),
                    register("rhs"),
                    label(".overflow")
                ),
                instruction!(Operation::StoreOperand, immediate(0), register("result")),
                instruction!(Operation::Label, label(".overflow")),
                instruction!(Operation::Control(ControlOperation::DispatchNext)),
            ],
            Architecture::Aarch64,
        );

        let instruction = find_operation(
            &out,
            Operation::Overflow(crate::target::description::OverflowOperation::MultiplyCopy),
        );
        for operand in &instruction.operands[..3] {
            assert_ne!(
                operand.register_name(),
                Some("x9"),
                "logical operand must not alias the explicit scratch"
            );
        }
        assert_eq!(instruction.operands[3].register_name(), Some("x9"));
    }

    #[test]
    fn and_immediate_keeps_live_virtuals_off_x86_scratch_registers() {
        // x86_64 lowers large `and` immediates through rax, or through r11
        // when the destination is rax. Temps live across the instruction
        // must avoid both scratch registers.
        let out = allocated(
            vec![
                instruction!(Operation::Move(IntegerWidth::U64), register("base"), immediate(100)),
                instruction!(
                    Operation::Move(IntegerWidth::U64),
                    register("index"),
                    immediate(0x123456789)
                ),
                instruction!(
                    Operation::IntegerBinary {
                        operation: IntegerBinaryOperation::Binary(BinaryOperation::And),
                        width: IntegerWidth::U64
                    },
                    register("index"),
                    immediate(0x3ffffffff)
                ),
                instruction!(
                    Operation::IntegerBinary {
                        operation: IntegerBinaryOperation::Binary(BinaryOperation::Add),
                        width: IntegerWidth::U64
                    },
                    register("index"),
                    register("base")
                ),
                instruction!(Operation::StoreOperand, immediate(0), register("index")),
                instruction!(Operation::Control(ControlOperation::DispatchNext)),
            ],
            Architecture::X86_64,
        );
        for insn in &out {
            if is_operation(insn, Operation::Move(IntegerWidth::U64))
                && insn.operands.len() == 2
                && let AllocatedOperand::Immediate(100) = insn.operands[1]
                && let Some(name) = insn.operands[0].register_name()
            {
                assert_ne!(name, "rax", "live virtual must avoid rax");
                assert_ne!(name, "r11", "live virtual must avoid r11");
            }
            if is_operation(
                insn,
                Operation::IntegerBinary {
                    operation: IntegerBinaryOperation::Binary(BinaryOperation::And),
                    width: IntegerWidth::U64,
                },
            ) && let Some(name) = insn.operands.first().and_then(AllocatedOperand::register_name)
            {
                assert_ne!(name, "rax", "and dst must avoid rax");
                assert_ne!(name, "r11", "and dst must avoid r11");
            }
        }
    }
}
