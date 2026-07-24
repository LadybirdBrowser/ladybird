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

use crate::low_ir::cfg::ControlFlowGraph;
use crate::target::description::{
    InstructionDescription, OperandKind, SelectedOpcode,
};
#[cfg(test)]
use crate::target::description::{AssertionOperation, CallKind, ControlOperation, EqualityCondition, MemoryWidth, PairWidth, TestCondition, ZeroCondition};
use crate::types::RegisterClass as VirtualRegisterClass;
use crate::types::InterpreterRegister;
use crate::Architecture;
use crate::target::registers::{PhysicalRegister, register_info_for, resolve_interpreter_register};
#[cfg(test)]
use crate::target::registers::physical_register_named;
use crate::target::ir::{
    AddressRegister, AllocatedFunction,
    AllocatedInstruction, AllocatedOperand,
    AllocatedProgram,
    Function as TargetFunction, RuntimeConstants,
    Instruction as MachineInstruction, MemoryAddress, Operand,
    PhysicalMemoryAddress, Program as TargetProgram, count_virtual_register_uses,
    visit_virtual_registers, VirtualRegister,
};
use crate::low_ir::optimize::{invert_branches_over_jumps, remove_unreferenced_labels};
use crate::{CompileError, CompileStage};
use std::collections::{BTreeSet, HashMap, HashSet};

#[derive(Debug, Default)]
struct UseDef {
    uses: BTreeSet<VirtualRegister>,
    defs: BTreeSet<VirtualRegister>,
    clobbers: BTreeSet<PhysicalRegister>,
}

impl UseDef {
    fn use_operand(&mut self, operand: &Operand) {
        if let Operand::VirtualRegister(register) = operand {
            self.uses.insert(register.clone());
        }
    }

    fn define_operand(&mut self, operand: &Operand, architecture: Architecture) {
        match operand {
            Operand::VirtualRegister(register) => {
                self.defs.insert(register.clone());
            }
            Operand::InterpreterRegister(register) => {
                self.clobbers.extend(resolve_interpreter_register(*register, architecture));
            }
            Operand::PhysicalRegister(register) => {
                self.clobbers.insert(*register);
            }
            _ => {}
        }
    }

    fn use_address_register(&mut self, register: &AddressRegister) {
        if let AddressRegister::Virtual(register) = register {
            self.uses.insert(register.clone());
        }
    }
}

struct Liveness {
    live_in: Vec<BTreeSet<VirtualRegister>>,
    live_out: Vec<BTreeSet<VirtualRegister>>,
    use_def: Vec<UseDef>,
}

#[derive(Default)]
struct Interference {
    neighbors: HashSet<VirtualRegister>,
    forbidden: HashSet<PhysicalRegister>,
    affinities: Vec<VirtualRegister>,
    pinned: Option<PhysicalRegister>,
    live_range_size: usize,
}

type InterferenceGraph = HashMap<VirtualRegister, Interference>;

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
    let flat = handler.instructions.clone();
    let arch = handler.architecture;
    let mut needs = false;
    for instruction in &flat {
        for operand in &instruction.operands {
            visit_virtual_registers(operand, &mut |_| needs = true);
        }
    }
    if !needs {
        return build_allocated_graph(
            handler,
            rewrite(handler, flat, &AllocationPlan::default(), runtime)?,
        );
    }
    let graph = build_graph(handler, flat.clone())?;
    let control_flow = graph.linearize_for_analysis();
    let assignments = allocate(
        handler,
        &control_flow.instructions,
        &control_flow.successors,
        arch,
    )?;
    let mut rewritten = rewrite(handler, flat, &assignments, runtime)?;
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
    let mut graph =
        ControlFlowGraph::from_instructions(instructions)
            .map_err(|message| allocation_error(&handler.name, message))?;
    graph.thread_unconditional_jumps();
    graph.remove_unreferenced_jump_blocks();
    graph.remove_unreachable_blocks();
    Ok(graph)
}

fn build_graph(
    handler: &TargetFunction,
    instructions: Vec<MachineInstruction>,
) -> Result<ControlFlowGraph<Operand, SelectedOpcode>, CompileError> {
    ControlFlowGraph::from_instructions(instructions)
        .map_err(|message| allocation_error(&handler.name, message))
}

// ============================================================================
// Use/def computation
// ============================================================================

/// Compute the physical and virtual register uses, definitions, and clobbers
/// for one machine instruction.
fn analyze_instruction(
    insn: &MachineInstruction,
    arch: Architecture,
) -> UseDef {
    let mut ud = UseDef::default();
    let info = insn.opcode.description();
    let zeroes_register = info.zeroes_if_inputs_alias
        && insn.operands.len() >= 2
        && registers_alias(&insn.operands[0], &insn.operands[1], arch);

    for (i, op) in insn.operands.iter().enumerate() {
        // `xor dst, dst` is the canonical zero-the-register idiom. It
        // defines the destination without reading either semantic operand.
        // Target-selected scratch operands still retain their ordinary
        // definitions.
        if i < 2
            && zeroes_register
        {
            if i == 0 {
                ud.define_operand(op, arch);
            }
            continue;
        }
        let kind =
            operand_kind_at(info, i, insn.operands.len(), arch);
        apply_operand_kind(&mut ud, op, kind, arch);
    }
    // Implicit i/o and all-caller-saved kills.
    let arch_spec = info.arch(arch);
    for r in arch_spec.implicit_outputs {
        ud.clobbers.insert(*r);
    }
    if info.is_call {
        let registers = register_info_for(arch);
        for r in registers.caller_saved_gpr {
            ud.clobbers.insert(*r);
        }
        for r in registers.caller_saved_fpr {
            ud.clobbers.insert(*r);
        }
    }

    ud
}

fn operand_kind_at(
    description: InstructionDescription,
    index: usize,
    operand_count: usize,
    architecture: Architecture,
) -> OperandKind {
    description
        .selected_operand_kind(index, operand_count, architecture)
        .expect("operand index out of range")
}

fn apply_operand_kind(
    ud: &mut UseDef,
    op: &Operand,
    kind: OperandKind,
    arch: Architecture,
) {
    match kind {
        OperandKind::GprIn | OperandKind::FprIn | OperandKind::RegisterIn => {
            ud.use_operand(op);
        }
        OperandKind::GprOut
        | OperandKind::GprScratch
        | OperandKind::FprScratch
        | OperandKind::FprOut
        | OperandKind::RegisterOut => {
            ud.define_operand(op, arch);
        }
        OperandKind::GprInOut | OperandKind::FprInOut => {
            ud.use_operand(op);
            ud.define_operand(op, arch);
        }
        OperandKind::GprInOrImm | OperandKind::GprInOrMemory => {
            ud.use_operand(op);
            if kind == OperandKind::GprInOrMemory
                && let Operand::Address(address) = op
            {
                for register in address_registers(address) {
                    ud.use_address_register(register);
                }
            }
        }
        OperandKind::Memory => {
            if let Operand::Address(address) = op {
                for register in address_registers(address) {
                    ud.use_address_register(register);
                }
            }
        }
        OperandKind::Imm | OperandKind::Label | OperandKind::FuncSymbol => {}
    }
}

fn address_registers(address: &MemoryAddress) -> impl Iterator<Item = &AddressRegister> {
    std::iter::once(&address.base).chain(address.index.iter())
}

fn registers_alias(lhs: &Operand, rhs: &Operand, architecture: Architecture) -> bool {
    match (lhs, rhs) {
        (Operand::VirtualRegister(lhs), Operand::VirtualRegister(rhs)) => lhs == rhs,
        (Operand::InterpreterRegister(lhs), Operand::InterpreterRegister(rhs)) => {
            resolve_interpreter_register(*lhs, architecture)
                .zip(resolve_interpreter_register(*rhs, architecture))
                .is_some_and(|(lhs, rhs)| lhs == rhs)
        }
        (Operand::InterpreterRegister(interpreter), Operand::PhysicalRegister(physical))
        | (Operand::PhysicalRegister(physical), Operand::InterpreterRegister(interpreter)) => {
            resolve_interpreter_register(*interpreter, architecture) == Some(*physical)
        }
        (Operand::PhysicalRegister(lhs), Operand::PhysicalRegister(rhs)) => lhs == rhs,
        _ => false,
    }
}

fn compute_liveness(
    handler_name: &str,
    instructions: &[MachineInstruction],
    successors: &[Vec<usize>],
    arch: Architecture,
) -> Result<Liveness, CompileError> {
    let n = instructions.len();
    assert_eq!(successors.len(), n, "invalid CFG for handler '{handler_name}'");

    let use_def = instructions
        .iter()
        .map(|insn| analyze_instruction(insn, arch))
        .collect::<Vec<_>>();
    let mut live_in = vec![BTreeSet::new(); use_def.len()];
    let mut live_out = live_in.clone();
    loop {
        let mut changed = false;
        for instruction in (0..use_def.len()).rev() {
            let mut next_out = BTreeSet::new();
            for successor in &successors[instruction] {
                next_out.extend(live_in[*successor].iter().cloned());
            }
            let mut next_in = use_def[instruction].uses.clone();
            next_in.extend(
                next_out
                    .iter()
                    .filter(|register| !use_def[instruction].defs.contains(*register))
                    .cloned(),
            );
            if next_in != live_in[instruction]
                || next_out != live_out[instruction]
            {
                live_in[instruction] = next_in;
                live_out[instruction] = next_out;
                changed = true;
            }
        }
        if !changed {
            break;
        }
    }
    let liveness = Liveness { live_in, live_out, use_def };

    // Sanity-check: a virtual register must be defined before its first use
    // along every path. We catch the simplest case: no virtual register may
    // be live at handler entry.
    let entry_live = &liveness.live_in[0];
    if let Some(register) = entry_live.iter().next() {
        // Walk forward and find the first instruction that pulls this
        // virtual register into its live-in without having defined it
        // yet, so the error points at the actual problem.
        let mut culprit = String::from("(unknown)");
        for (i, ud) in liveness.use_def.iter().enumerate() {
            if ud.uses.contains(register) && !ud.defs.contains(register) {
                culprit = format!("instruction #{i}: {:?}", instructions[i]);
                break;
            }
        }
        return Err(allocation_error(
            handler_name,
            format!(
                "virtual register '{register}' is used before being assigned (live at handler entry); first use: {culprit}"
            ),
        ));
    }

    Ok(liveness)
}

fn build_interference(
    liveness: &Liveness,
    virtuals: &[VirtualRegister],
) -> InterferenceGraph {
    let mut graph = virtuals
        .iter()
        .map(|register| (register.clone(), Interference::default()))
        .collect::<InterferenceGraph>();
    for (instruction, (live_in, live_out)) in liveness
        .live_in
        .iter()
        .zip(&liveness.live_out)
        .enumerate()
    {
        for live in [live_in, live_out] {
            for (position, register) in live.iter().enumerate() {
                graph.get_mut(register).unwrap().live_range_size += 1;
                for other in live.iter().skip(position + 1) {
                    graph.get_mut(register).unwrap().neighbors.insert(other.clone());
                    graph.get_mut(other).unwrap().neighbors.insert(register.clone());
                }
            }
        }
        for register in live_in.intersection(live_out) {
            if liveness.use_def[instruction].defs.contains(register) {
                continue;
            }
            let forbidden = &mut graph.get_mut(register).unwrap().forbidden;
            forbidden.extend(&liveness.use_def[instruction].clobbers);
        }
    }
    graph
}

// ============================================================================
// Interference and allocation
// ============================================================================

type AllocationPlan = HashMap<VirtualRegister, PhysicalRegister>;

struct ColoringContext<'a> {
    handler: &'a TargetFunction,
    interference: &'a InterferenceGraph,
    gpr_pool: &'a [PhysicalRegister],
    fpr_pool: &'a [PhysicalRegister],
}

impl ColoringContext<'_> {
    fn forbid_conflicts(
        &self,
        member: &VirtualRegister,
        pool: &[PhysicalRegister],
        assignments: &HashMap<VirtualRegister, PhysicalRegister>,
        forbidden: &mut HashSet<PhysicalRegister>,
    ) {
        forbidden.extend(
            self.interference[member]
                .forbidden
                .iter()
                .filter(|register| pool.contains(register)),
        );
        for other in &self.interference[member].neighbors {
            if let Some(&physical) = assignments.get(other) {
                forbidden.insert(physical);
            }
        }
    }
}

#[derive(Clone, Copy)]
enum AllocationPriority {
    UseCount,
    LiveRange,
}

fn root_of(parents: &[usize], mut index: usize) -> usize {
    while parents[index] != index {
        index = parents[index];
    }
    index
}

fn allocate(
    handler: &TargetFunction,
    instructions: &[MachineInstruction],
    successors: &[Vec<usize>],
    arch: Architecture,
) -> Result<AllocationPlan, CompileError> {

    // Collect typed virtual registers.
    let mut virtuals = HashSet::new();
    for insn in instructions {
        for operand in &insn.operands {
            visit_virtual_registers(operand, &mut |register| {
                virtuals.insert(register.clone());
            });
        }
    }

    let liveness = compute_liveness(
        &handler.name,
        instructions,
        successors,
        arch,
    )?;
    let mut all_virtuals = virtuals.iter().cloned().collect::<Vec<_>>();
    all_virtuals.sort();

    let mut interference = build_interference(&liveness, &all_virtuals);
    let mut move_affinity_edges = Vec::new();
    for insn in instructions {
        let info = insn.opcode.description();
        let arch_spec = info.arch(arch);
        for &(index, physical) in arch_spec.fixed_operands {
            if let Some(Operand::VirtualRegister(register)) = insn.operands.get(index)
                && let Some(previous) = interference.get_mut(register).unwrap().pinned.replace(physical)
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

        let mut explicit_scratches = Vec::new();
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
                explicit_scratches.push(*register);
            }
        }
        if !explicit_scratches.is_empty() {
            for operand in &insn.operands {
                visit_virtual_registers(operand, &mut |register| {
                    interference
                        .get_mut(register)
                        .unwrap()
                        .forbidden
                        .extend(explicit_scratches.iter().copied());
                });
            }
        }
        for (index, operand) in insn.operands.iter().enumerate() {
            if let Operand::VirtualRegister(register) = operand {
                let pinned_here = arch_spec
                    .fixed_operands
                    .iter()
                    .find_map(|&(operand, physical)| (operand == index).then_some(physical));
                interference.get_mut(register).unwrap().forbidden.extend(
                    arch_spec
                        .implicit_outputs
                        .iter()
                        .copied()
                        .filter(|physical| Some(*physical) != pinned_here),
                );
            }
        }
        // Only prefer aliases when both backends can omit a copy. For example,
        // mov32 must still execute to zero-extend its input even when both
        // operands share a register.
        for &(output, input) in info.coalescing_pairs {
            let (
                Some(Operand::VirtualRegister(output)),
                Some(Operand::VirtualRegister(input)),
            ) = (
                insn.operands.get(output),
                insn.operands.get(input),
            ) else {
                continue;
            };
            if output == input || output.class() != input.class() {
                continue;
            }
            interference.get_mut(output).unwrap().affinities.push(input.clone());
            interference.get_mut(input).unwrap().affinities.push(output.clone());
            move_affinity_edges.push((output.clone(), input.clone()));
        }
    }

    // Build candidate physical-register pools.
    let mapping = register_info_for(arch);
    let gpr_pool = mapping.temporaries.to_vec();
    let fpr_pool = mapping.fp_temporaries.to_vec();

    // On x86_64 each saved use of a low-cost register saves an encoding
    // byte, so hot virtual registers want to land in cheap registers.
    let use_counts = count_virtual_register_uses(instructions);

    // Merge move-related values whenever the combined group remains fully
    // noninterfering. Greedy compatible subgroups preserve opportunities in
    // affinity graphs where one interfering edge prevents full coalescing.
    let virtual_indices = all_virtuals
        .iter()
        .enumerate()
        .map(|(index, register)| (register.clone(), index))
        .collect::<HashMap<_, _>>();
    let mut parents = (0..all_virtuals.len()).collect::<Vec<_>>();
    for (lhs, rhs) in move_affinity_edges {
        let lhs = virtual_indices[&lhs];
        let rhs = virtual_indices[&rhs];
        let lhs_root = root_of(&parents, lhs);
        let rhs_root = root_of(&parents, rhs);
        if lhs_root == rhs_root {
            continue;
        }
        let members = (0..all_virtuals.len())
            .filter(|index| {
                let root = root_of(&parents, *index);
                root == lhs_root || root == rhs_root
            })
            .collect::<Vec<_>>();
        let can_coalesce_all = members.iter().enumerate().all(|(position, member)| {
            let member = &all_virtuals[*member];
            members[position + 1..].iter().all(|other| {
                let other = &all_virtuals[*other];
                !interference[member].neighbors.contains(other)
            })
        });
        let pinned_registers = members
            .iter()
            .filter_map(|member| interference[&all_virtuals[*member]].pinned)
            .collect::<HashSet<_>>();
        if can_coalesce_all && pinned_registers.len() <= 1 {
            parents[rhs_root] = lhs_root;
        }
    }

    let mut coalescing_groups = HashMap::new();
    for root in 0..all_virtuals.len() {
        if root_of(&parents, root) != root {
            continue;
        }
        let mut members = (0..all_virtuals.len())
            .filter(|index| root_of(&parents, *index) == root)
            .map(|index| all_virtuals[index].clone())
            .collect::<Vec<_>>();
        if members.len() <= 1 {
            continue;
        }
        members.sort();
        for member in &members {
            coalescing_groups.insert(member.clone(), members.clone());
        }
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
    let context = ColoringContext {
        handler,
        interference: &interference,
        gpr_pool: &gpr_pool,
        fpr_pool: &fpr_pool,
    };
    let try_color = |sorted, groups| color(&context, sorted, groups);
    let compare = |a: &VirtualRegister, b: &VirtualRegister, priority| {
        let ap = interference[a].pinned.is_some();
        let bp = interference[b].pinned.is_some();
        let aa = interference[a]
            .affinities
            .iter()
            .any(|neighbor| interference[neighbor].pinned.is_some());
        let ba = interference[b]
            .affinities
            .iter()
            .any(|neighbor| interference[neighbor].pinned.is_some());
        let au = use_counts.get(a).copied().unwrap_or(0) as usize;
        let bu = use_counts.get(b).copied().unwrap_or(0) as usize;
        let aw = interference[a].live_range_size;
        let bw = interference[b].live_range_size;
        let ((af, bf), (as_, bs)) = match priority {
            AllocationPriority::UseCount => ((au, bu), (aw, bw)),
            AllocationPriority::LiveRange => ((aw, bw), (au, bu)),
        };
        bp.cmp(&ap)
            .then_with(|| ba.cmp(&aa))
            .then_with(|| bf.cmp(&af))
            .then_with(|| bs.cmp(&as_))
            .then_with(|| a.cmp(b))
    };

    let mut use_order = all_virtuals.clone();
    use_order.sort_by(|a, b| compare(a, b, AllocationPriority::UseCount));
    if let Ok(plan) = try_color(&use_order, &coalescing_groups) {
        return Ok(plan);
    }

    let mut live_order = all_virtuals.clone();
    live_order.sort_by(|a, b| compare(a, b, AllocationPriority::LiveRange));
    if let Ok(plan) = try_color(&live_order, &coalescing_groups) {
        return Ok(plan);
    }

    let mut groups = coalescing_groups
        .iter()
        .filter(|(member, group)| group.first() == Some(*member))
        .map(|(_, group)| group.clone())
        .collect::<Vec<_>>();
    let group_score = |group: &[VirtualRegister]| {
        (
            group.iter().map(|member| use_counts.get(member).copied().unwrap_or(0)).sum::<u32>(),
            group.len(),
        )
    };
    groups.sort_by(|lhs, rhs| {
        group_score(rhs)
            .cmp(&group_score(lhs))
            .then_with(|| lhs.cmp(rhs))
    });
    let mut retained_groups = HashMap::new();
    for group in groups {
        let mut candidate_groups = retained_groups.clone();
        for member in &group {
            candidate_groups.insert(member.clone(), group.clone());
        }
        if color(&context, &use_order, &candidate_groups).is_ok()
            || color(&context, &live_order, &candidate_groups).is_ok()
        {
            retained_groups = candidate_groups;
        }
    }
    if !retained_groups.is_empty() {
        if let Ok(plan) = try_color(&use_order, &retained_groups) {
            return Ok(plan);
        }
        if let Ok(plan) = try_color(&live_order, &retained_groups) {
            return Ok(plan);
        }
    }

    let no_groups = HashMap::new();
    if let Ok(plan) = try_color(&use_order, &no_groups) {
        return Ok(plan);
    }
    try_color(&live_order, &no_groups)
}

/// Greedy linear-scan coloring driven by the order in `sorted_virtuals`.
/// Returns Err if any virtual register cannot be placed; the caller decides
/// whether to retry with a different ordering.
fn color(
    context: &ColoringContext<'_>,
    sorted_virtuals: &[VirtualRegister],
    coalescing_groups: &HashMap<VirtualRegister, Vec<VirtualRegister>>,
) -> Result<AllocationPlan, CompileError> {
    let ColoringContext {
        handler,
        interference,
        gpr_pool,
        fpr_pool,
    } = context;
    let mut assignments = HashMap::new();

    for name in sorted_virtuals {
        if assignments.contains_key(name) {
            continue;
        }
        let is_gpr = name.class() == VirtualRegisterClass::GeneralPurpose;
        let pool = if is_gpr { gpr_pool } else { fpr_pool };
        if let Some(group) = coalescing_groups.get(name) {
            let mut forbidden = HashSet::new();
            for member in group {
                context.forbid_conflicts(
                    member,
                    pool,
                    &assignments,
                    &mut forbidden,
                );
            }
            let pinned_registers = group
                .iter()
                .filter_map(|member| interference[member].pinned)
                .collect::<HashSet<_>>();
            let pick = if let Some(&pinned) = pinned_registers.iter().next() {
                if pinned_registers.len() != 1
                    || !pool.contains(&pinned)
                    || forbidden.contains(&pinned)
                {
                    return Err(allocation_error(
                        &handler.name,
                        format!("could not coalesce move affinity group containing '{name}'"),
                    ));
                }
                pinned
            } else {
                *pool
                    .iter()
                    .enumerate()
                    .filter(|(_, candidate)| !forbidden.contains(*candidate))
                    .min_by_key(|(index, candidate)| {
                        (candidate.allocation_cost(), *index)
                    })
                    .map(|(_, candidate)| candidate)
                    .ok_or_else(|| {
                        allocation_error(
                            &handler.name,
                            format!("could not coalesce move affinity group containing '{name}'"),
                        )
                    })?
            };
            for member in group {
                assignments.insert(member.clone(), pick);
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
        let mut forbidden = HashSet::new();
        context.forbid_conflicts(
            name,
            pool,
            &assignments,
            &mut forbidden,
        );

        let pick = if let Some(pinned) = interference[name].pinned {
            if !pool.contains(&pinned) {
                return Err(allocation_error(
                    &handler.name,
                    format!(
                        "virtual register '{name}' must be pinned to '{pinned}', but that register is not in the {} pool",
                        if is_gpr { "GPR" } else { "FPR" }
                    ),
                ));
            }
            if forbidden.contains(&pinned) {
                let mut sorted = forbidden.iter().copied().collect::<Vec<_>>();
                sorted.sort();
                return Err(allocation_error(
                    &handler.name,
                    format!(
                        "virtual register '{name}' must be pinned to '{pinned}', but that register is killed or occupied across its live range (forbidden: {sorted:?})"
                    ),
                ));
            }
            pinned
        } else {
            let preferred = interference[name]
                .affinities
                .iter()
                .filter_map(|other| assignments.get(other).copied())
                .find(|register| !forbidden.contains(register));
            let coalescable_neighbors = interference[name]
                .affinities
                .iter()
                .filter(|neighbor| !assignments.contains_key(*neighbor))
                .filter(|neighbor| !interference[name].neighbors.contains(*neighbor))
                .collect::<Vec<_>>();
            let available_for_neighbor =
                |neighbor: &VirtualRegister, candidate: PhysicalRegister| {
                if interference[neighbor]
                    .pinned
                    .is_some_and(|pinned| pinned != candidate)
                    || interference[neighbor].forbidden.contains(&candidate)
                {
                    return false;
                }
                interference[neighbor].neighbors.iter().all(|other| {
                    assignments.get(other) != Some(&candidate)
                })
            };
            // If an affinity neighbor has not been colored yet, choose a
            // register it can use too. This lets continuation parameters
            // follow constrained producers instead of claiming a scratch
            // register that forces a move at the control-flow edge.
            let future_preferred = (!coalescable_neighbors.is_empty())
                .then(|| {
                    pool.iter()
                        .enumerate()
                        .filter(|(_, candidate)| {
                            !forbidden.contains(*candidate)
                                && coalescable_neighbors
                                    .iter()
                                    .all(|neighbor| {
                                        available_for_neighbor(neighbor, **candidate)
                                    })
                        })
                        .min_by_key(|(i, candidate)| {
                            (candidate.allocation_cost(), *i)
                        })
                        .map(|(_, candidate)| *candidate)
                })
                .flatten();
            // Pick the cheapest available register for this virtual. Pool
            // position breaks ties so behavior stays deterministic when
            // several registers tie in cost (e.g. all FPRs, or all
            // aarch64 GPRs).
            if let Some(preferred) = preferred {
                preferred
            } else if let Some(preferred) = future_preferred {
                preferred
            } else {
                *pool
                    .iter()
                    .enumerate()
                    .filter(|(_, p)| !forbidden.contains(*p))
                    .min_by_key(|(i, p)| (p.allocation_cost(), *i))
                    .map(|(_, p)| p)
                    .ok_or_else(|| {
                        allocation_error(
                            &handler.name,
                            format!(
                                "could not allocate {} virtual register '{name}': {} pool exhausted (forbidden: {:?})",
                                if is_gpr { "general-purpose" } else { "floating-point" },
                                if is_gpr { "GPR" } else { "FPR" },
                                forbidden,
                            ),
                        )
                    })?
            }
        };

        assignments.insert(name.clone(), pick);
    }

    Ok(assignments)
}

// ============================================================================
// Operand rewriting
// ============================================================================

fn rewrite(
    handler: &TargetFunction,
    instructions: Vec<MachineInstruction>,
    plan: &AllocationPlan,
    runtime: &RuntimeConstants,
) -> Result<Vec<AllocatedInstruction>, CompileError> {
    instructions
        .into_iter()
        .map(|insn| {
            let operands: Vec<_> = insn
                .operands
                .into_iter()
                .map(|op| rewrite_operand(handler, op, plan, runtime))
                .collect::<Result<_, _>>()?;
            if insn.opcode.description().identity_if_inputs_alias
                && operands.len() == 2
                && operands[0] == operands[1]
            {
                return Ok(None);
            }
            Ok(Some(AllocatedInstruction {
                opcode: insn.opcode,
                operands,
            }))
        })
        .filter_map(|instruction| instruction.transpose())
        .collect()
}

fn rewrite_operand(
    handler: &TargetFunction,
    op: Operand,
    table: &HashMap<VirtualRegister, PhysicalRegister>,
    runtime: &RuntimeConstants,
) -> Result<AllocatedOperand, CompileError> {
    Ok(match op {
        Operand::VirtualRegister(register) => AllocatedOperand::PhysicalRegister(
            *table
                .get(&register)
                .expect("every virtual register must have an allocation"),
        ),
        Operand::InterpreterRegister(register) => {
            AllocatedOperand::PhysicalRegister(resolve_interpreter_register(register, handler.architecture)
                .ok_or_else(|| {
                    allocation_error(
                        &handler.name,
                        format!(
                            "interpreter register '{}' is unavailable on {:?}",
                            register.as_str(),
                            handler.architecture
                        ),
                    )
                })?)
        }
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
                    .ok_or_else(|| finalization_error(
                        &handler.name,
                        "execution-context address adjustment overflow",
                    ))?;
                let displacement = address
                    .displacement
                    .unwrap_or(0)
                    .checked_add(adjustment)
                    .ok_or_else(|| finalization_error(
                        &handler.name,
                        "machine address displacement overflow",
                    ))?;
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
    table: &HashMap<VirtualRegister, PhysicalRegister>,
) -> Result<PhysicalRegister, CompileError> {
    match register {
        AddressRegister::Virtual(register) => Ok(*table
            .get(&register)
            .expect("every virtual register must have an allocation")),
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
        AddressRegister::Physical(register)
            if register.architecture() == handler.architecture =>
        {
            Ok(register)
        }
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
    use crate::low_ir::{
        AddressRegister as SourceAddressRegister, Handler,
        Instruction as SourceInstruction, MemoryAddress as SourceMemoryAddress,
        Operand as SourceOperand, Program, VirtualRegister as SourceVirtualRegister,
    };
    use crate::low_ir::preallocate::schedule_x86_relational_operand_loads;
    use crate::target::description::{
        BinaryOperation, FloatBinaryOperation, FloatConversion,
        FloatingPointOperation, IntegerBinaryOperation, IntegerWidth,
        IntegerSignedness, Operation, ShiftOperation,
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
            physical_register_named(name, Architecture::X86_64)
                .expect("test operand must name an x86-64 register"),
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
        let constants =
            crate::frontend::layout::LayoutConstants::from_values([(
                name.to_string(),
                value,
            )]);
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
            instruction!(Operation::load(MemoryWidth::DoubleWord, false), register("lhs"), memory("values", Some("lhs_index"))),
            instruction!(Operation::load(MemoryWidth::DoubleWord, false), register("rhs"), memory("values", Some("rhs_index"))),
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
                Operation::branch_ordered(IntegerWidth::U32, crate::target::description::OrderedCondition::Less, IntegerSignedness::Signed),
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
            instructions.iter().map(|instruction| instruction.opcode.operation()).collect::<Vec<_>>(),
            [
                Operation::load(MemoryWidth::Word, false),
                Operation::load(MemoryWidth::DoubleWord, false),
                Operation::load(MemoryWidth::Word, false),
                Operation::load(MemoryWidth::DoubleWord, false),
                Operation::branch_tag(EqualityCondition::NotEqual),
                Operation::ExtractTag,
                Operation::branch_equality(IntegerWidth::U16, EqualityCondition::NotEqual),
                Operation::branch_ordered(IntegerWidth::U32, crate::target::description::OrderedCondition::Less, IntegerSignedness::Signed),
            ]
        );
        assert_eq!(instructions[0].operands[0], register("rhs_index"));
        assert_eq!(instructions[2].operands[0], register("lhs_index"));
        assert_eq!(instructions[5].operands, [register("rhs_index"), register("rhs")]);
    }

    fn allocated(
        instructions: Vec<SourceInstruction>,
        architecture: Architecture,
    ) -> Vec<AllocatedInstruction> {
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

    #[test]
    fn rejects_unavailable_interpreter_register_during_allocation() {
        let error = build(
            vec![instruction!(
                Operation::Move(IntegerWidth::U64),
                SourceOperand::InterpreterRegister(
                    InterpreterRegister::Int32TagShifted,
                ),
                immediate(0)
            )],
            Architecture::Aarch64,
        )
        .unwrap_err();

        assert_eq!(error.stage, CompileStage::Allocation);
        assert_eq!(error.handler.as_deref(), Some("Test"));
        assert!(error.message.contains(
            "interpreter register 'int32_tag_shifted' is unavailable on Aarch64"
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
        crate::low_ir::lowering::materialize_layout_constants(
            &mut program.handlers[0],
        );
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

    fn find_operation(
        instructions: &[AllocatedInstruction],
        operation: Operation,
    ) -> &AllocatedInstruction {
        instructions
            .iter()
            .find(|instruction| is_operation(instruction, operation))
            .unwrap_or_else(|| panic!("no '{operation:?}' instruction found"))
    }

    fn assignment_for(insns: &[AllocatedInstruction], operation: Operation, op_index: usize) -> String {
        for insn in insns {
            if is_operation(insn, operation)
                && let Some(name) = insn.operands.get(op_index).and_then(AllocatedOperand::register_name) {
                    return name.to_string();
                }
        }
        panic!("no '{operation:?}' instruction found");
    }

    fn allocated_helper_call(architecture: Architecture) -> Vec<AllocatedInstruction> {
        allocated(
            vec![
                instruction!(Operation::Move(IntegerWidth::U64), register("value"), immediate(1)),
                instruction!(Operation::Call(CallKind::Helper), symbol("asm_helper_to_boolean"), register("value"), register("result")),
                instruction!(Operation::branch_zero(IntegerWidth::U64, ZeroCondition::NonZero), register("result"), label(".take")),
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
                instruction!(Operation::IntegerBinary { operation: IntegerBinaryOperation::Binary(BinaryOperation::Add), width: IntegerWidth::U64 }, register("foo"), immediate(1)),
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
    fn coalesces_checked_integer_refinement_moves() {
        let instructions = allocated(
            vec![
                instruction!(Operation::Move(IntegerWidth::U64), register("wide"), immediate(1)),
                instruction!(Operation::branch_bit(TestCondition::Set), register("wide"), immediate(31), label(".wide")),
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
                instruction!(Operation::Move(IntegerWidth::U64), register("destination"), register("source")),
                instruction!(Operation::Assertion(AssertionOperation::NonZero), register("destination")),
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
                instruction!(Operation::FloatMove, floating_point_register("source"), x86_register("xmm5")),
                instruction!(Operation::FloatMove, floating_point_register("destination"), floating_point_register("source")),
                instruction!(Operation::Float(FloatingPointOperation::Binary(FloatBinaryOperation::Add)), floating_point_register("destination"), floating_point_register("destination")),
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
                instruction!(Operation::Move(IntegerWidth::U64), register("middle"), register("producer")),
                instruction!(Operation::Move(IntegerWidth::U64), register("consumer"), register("middle")),
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
                instruction!(Operation::IntegerBinary { operation: IntegerBinaryOperation::Binary(BinaryOperation::Subtract), width: IntegerWidth::U64 }, register("local"), immediate(1)),
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
                instruction!(Operation::IntegerBinary { operation: IntegerBinaryOperation::Binary(BinaryOperation::Add), width: IntegerWidth::U64 }, register("result"), register("rhs")),
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
                instruction!(Operation::IntegerBinary { operation: IntegerBinaryOperation::Binary(BinaryOperation::Add), width: IntegerWidth::U64 }, register("a"), register("b")),
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
                instruction!(Operation::IntegerBinary { operation: IntegerBinaryOperation::Shift(ShiftOperation::RightLogical), width: IntegerWidth::U64 }, register("value"), register("count")),
            ],
            Architecture::X86_64,
        );
        let shift = find_operation(&out, Operation::IntegerBinary { operation: IntegerBinaryOperation::Shift(ShiftOperation::RightLogical), width: IntegerWidth::U64 });
        assert_eq!(shift.operands[1].register_name(), Some("rcx"));
    }

    #[test]
    fn distinguishes_virtual_register_classes_with_the_same_name() {
        let instructions = allocated(
            vec![
                instruction!(Operation::Move(IntegerWidth::U64), register("value"), immediate(1)),
                instruction!(Operation::Float(FloatingPointOperation::Convert(FloatConversion::Int32ToFloat64)), floating_point_register("value"), register("value")),
                instruction!(Operation::Control(ControlOperation::DispatchNext)),
            ],
            Architecture::X86_64,
        );

        assert!(assignment_for(&instructions, Operation::Float(FloatingPointOperation::Convert(FloatConversion::Int32ToFloat64)), 0).starts_with("xmm"));
        assert!(!assignment_for(&instructions, Operation::Float(FloatingPointOperation::Convert(FloatConversion::Int32ToFloat64)), 1).starts_with("xmm"));
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

        assert_ne!(assignment_for(&instructions, Operation::Move(IntegerWidth::U64), 0), "r13");
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
                instruction!(Operation::Call(CallKind::RawNative), register("function"), register("native_return"), register("variant")),
                instruction!(Operation::Move(IntegerWidth::U64), register("payload"), register("native_return")),
                instruction!(Operation::Move(IntegerWidth::U64), register("unrelated"), immediate(2)),
                instruction!(Operation::IntegerBinary { operation: IntegerBinaryOperation::Binary(BinaryOperation::Add), width: IntegerWidth::U64 }, register("unrelated"), immediate(3)),
                instruction!(Operation::branch_zero(IntegerWidth::U64, ZeroCondition::NonZero), register("variant"), label(".exception")),
                instruction!(Operation::StoreOperand, immediate(0), register("payload")),
                instruction!(Operation::Control(ControlOperation::DispatchNext)),
                instruction!(Operation::Label, label(".exception")),
                instruction!(Operation::Move(IntegerWidth::U64), register("helper_argument"), register("payload")),
                instruction!(Operation::Call(CallKind::Helper), symbol("asm_helper_handle_raw_native_exception"), register("helper_argument"), register("helper_result")),
                instruction!(Operation::Control(ControlOperation::Exit)),
            ],
            Architecture::Aarch64,
        );

        assert_eq!(assignment_for(&instructions, Operation::Call(CallKind::RawNative), 1), "x0");
        assert_eq!(assignment_for(&instructions, Operation::StoreOperand, 1), "x0");
        assert_eq!(assignment_for(&instructions, Operation::Call(CallKind::Helper), 1), "x0");
    }

    #[test]
    fn prioritizes_constrained_move_sources() {
        let instructions = allocated(
            vec![
                instruction!(Operation::Float(FloatingPointOperation::Convert(FloatConversion::Float64ToInt32)), register("raw"), x86_register("xmm0"), label(".fail")),
                instruction!(Operation::Move(IntegerWidth::U64), register("parameter"), register("raw")),
                instruction!(Operation::Assertion(AssertionOperation::NonZero), register("parameter")),
                instruction!(Operation::Assertion(AssertionOperation::NonZero), register("parameter")),
                instruction!(Operation::Control(ControlOperation::DispatchNext)),
                instruction!(Operation::Label, label(".fail")),
                instruction!(Operation::Control(ControlOperation::Exit)),
            ],
            Architecture::X86_64,
        );

        assert_eq!(
            assignment_for(&instructions, Operation::Float(FloatingPointOperation::Convert(FloatConversion::Float64ToInt32)), 0),
            assignment_for(&instructions, Operation::Assertion(AssertionOperation::NonZero), 0)
        );
        assert!(!instructions.iter().any(|instruction| is_operation(instruction, Operation::Move(IntegerWidth::U64))));
    }

    #[test]
    fn rejects_virtual_register_live_across_call() {
        let err = build(
            vec![
                instruction!(Operation::Move(IntegerWidth::U64), register("value"), immediate(7)),
                instruction!(Operation::Call(CallKind::Helper), symbol("asm_helper_to_boolean"), x86_register("rcx"), x86_register("rax")),
                instruction!(Operation::IntegerBinary { operation: IntegerBinaryOperation::Binary(BinaryOperation::Add), width: IntegerWidth::U64 }, register("value"), immediate(1)),
            ],
            Architecture::X86_64,
        )
        .expect_err("should reject value live across call_helper");
        assert!(
            err.message.contains("could not allocate") || err.message.contains("pool exhausted"),
            "unexpected error: {err:?}"
        );
    }

    #[test]
    fn separates_gpr_and_fpr_pools() {
        let out = allocated(
            vec![
                instruction!(Operation::Move(IntegerWidth::U64), register("i"), immediate(1)),
                instruction!(Operation::Float(FloatingPointOperation::Convert(FloatConversion::Int32ToFloat64)), floating_point_register("f"), register("i")),
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
                    instruction!(Operation::IntegerBinary { operation: IntegerBinaryOperation::Binary(BinaryOperation::Xor), width: IntegerWidth::U64 }, register("counter"), register("counter")),
                    instruction!(Operation::IntegerBinary { operation: IntegerBinaryOperation::Binary(BinaryOperation::Add), width: IntegerWidth::U64 }, register("counter"), immediate(1)),
                    instruction!(Operation::StoreOperand, immediate(0), register("counter")),
                    instruction!(Operation::Control(ControlOperation::DispatchNext)),
                ],
                architecture,
            );
        }
    }

    #[test]
    fn divmod_pins_outputs_and_keeps_inputs_off_rax_rdx() {
        // x86 idiv reads rax/rdx, so dividend / divisor operands must not
        // be allocated to rax or rdx (the implicit_outputs); the quotient
        // and remainder, however, are pinned to rax/rdx by fixed_operands
        // and that pin must NOT be vetoed by the operand-forbids rule.
        let out = allocated(
            vec![
                instruction!(Operation::Move(IntegerWidth::U64), register("dividend"), immediate(1)),
                instruction!(Operation::Move(IntegerWidth::U64), register("divisor"), immediate(2)),
                instruction!(Operation::DivMod, register("quot"), register("rem"), register("dividend"), register("divisor")),
                instruction!(Operation::BoxInt32 { clean: false }, register("dst"), register("rem")),
                instruction!(Operation::StoreOperand, immediate(0), register("dst")),
                instruction!(Operation::Control(ControlOperation::DispatchNext)),
            ],
            Architecture::X86_64,
        );
        let names = find_operation(&out, Operation::DivMod)
            .operands
            .iter()
            .map(|operand| operand.register_name().unwrap_or("<non-reg>"))
            .collect::<Vec<_>>();
        assert_eq!(&names[..2], ["rax", "rdx"]);
        assert!(!["rax", "rdx"].contains(&names[2]));
        assert!(!["rax", "rdx"].contains(&names[3]));
    }

    #[test]
    fn explicit_scratch_keeps_aarch64_operands_off_x9() {
        let out = allocated(
            vec![
                instruction!(
                    Operation::Move(IntegerWidth::U64),
                    register("lhs"),
                    immediate(6)
                ),
                instruction!(
                    Operation::Move(IntegerWidth::U64),
                    register("rhs"),
                    immediate(7)
                ),
                instruction!(
                    Operation::Overflow(
                        crate::target::description::OverflowOperation::MultiplyCopy,
                    ),
                    register("result"),
                    register("lhs"),
                    register("rhs"),
                    label(".overflow")
                ),
                instruction!(
                    Operation::StoreOperand,
                    immediate(0),
                    register("result")
                ),
                instruction!(Operation::Label, label(".overflow")),
                instruction!(Operation::Control(ControlOperation::DispatchNext)),
            ],
            Architecture::Aarch64,
        );

        let instruction = find_operation(
            &out,
            Operation::Overflow(
                crate::target::description::OverflowOperation::MultiplyCopy,
            ),
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
                instruction!(Operation::Move(IntegerWidth::U64), register("index"), immediate(0x123456789)),
                instruction!(Operation::IntegerBinary { operation: IntegerBinaryOperation::Binary(BinaryOperation::And), width: IntegerWidth::U64 }, register("index"), immediate(0x3ffffffff)),
                instruction!(Operation::IntegerBinary { operation: IntegerBinaryOperation::Binary(BinaryOperation::Add), width: IntegerWidth::U64 }, register("index"), register("base")),
                instruction!(Operation::StoreOperand, immediate(0), register("index")),
                instruction!(Operation::Control(ControlOperation::DispatchNext)),
            ],
            Architecture::X86_64,
        );
        for insn in &out {
            if is_operation(insn, Operation::Move(IntegerWidth::U64)) && insn.operands.len() == 2
                && let AllocatedOperand::Immediate(100) = insn.operands[1]
                    && let Some(name) = insn.operands[0].register_name() {
                        assert_ne!(name, "rax", "live virtual must avoid rax");
                        assert_ne!(name, "r11", "live virtual must avoid r11");
                    }
            if is_operation(insn, Operation::IntegerBinary { operation: IntegerBinaryOperation::Binary(BinaryOperation::And), width: IntegerWidth::U64 })
                && let Some(name) = insn.operands.first().and_then(AllocatedOperand::register_name) {
                    assert_ne!(name, "rax", "and dst must avoid rax");
                    assert_ne!(name, "r11", "and dst must avoid r11");
                }
        }
    }
}
