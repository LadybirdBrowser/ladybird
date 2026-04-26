/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Register allocator for named DSL temporaries.
//!
//! When a handler (or any macro it invokes) declares named temporaries via
//! `temp foo, bar` / `ftemp baz`, this module:
//!
//!   1. Flattens the handler body, expanding macros and uniquifying any
//!      `temp` / `ftemp` declarations and label references that live
//!      inside macro bodies. Macro-local temps and labels never leak.
//!   2. Computes per-instruction use/def sets and runs an iterative
//!      backward dataflow to derive liveness.
//!   3. Builds an interference graph between named temporaries (and
//!      between named temps and physical registers killed by hidden
//!      clobbers, implicit i/o, fixed operand requirements, and
//!      caller-saved kills at C++ calls).
//!   4. Greedily assigns each named temporary a physical register from
//!      the public DSL pool. Hard-errors if a temp cannot be placed; we
//!      never spill to the stack.
//!   5. Rewrites operands so the codegen sees only physical register
//!      names.
//!
//! Handlers that don't use named temps go through the existing codegen
//! path with no allocator involvement.

#![allow(dead_code)]

use crate::instructions::{InstructionInfo, OperandKind, lookup};
use crate::parser::{AsmInstruction, Handler, Operand, Program};
use crate::registers::{Arch, mapping_for, register_cost, resolve_register};
use std::collections::{BTreeSet, HashMap, HashSet};

/// Registers x86-64 calls clobber (caller-saved). System V AMD64.
const X86_64_CALLER_SAVED_GPR: &[&str] = &[
    "rax", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11",
];
const X86_64_CALLER_SAVED_FPR: &[&str] =
    &["xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5"];

/// Registers aarch64 calls clobber. AAPCS64 caller-saved set; we only list
/// the ones in (or adjacent to) the public temp pool, since pinned
/// registers (x19-x28, d8) survive by ABI.
const AARCH64_CALLER_SAVED_GPR: &[&str] = &[
    "x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7", "x8", "x9", "x10",
    "x11", "x12", "x13", "x14", "x15", "x16", "x17",
];
const AARCH64_CALLER_SAVED_FPR: &[&str] =
    &["d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7"];

fn caller_saved(arch: Arch) -> (&'static [&'static str], &'static [&'static str]) {
    match arch {
        Arch::X86_64 => (X86_64_CALLER_SAVED_GPR, X86_64_CALLER_SAVED_FPR),
        Arch::Aarch64 => (AARCH64_CALLER_SAVED_GPR, AARCH64_CALLER_SAVED_FPR),
    }
}

#[derive(Debug)]
pub struct AllocationError {
    pub handler: String,
    pub message: String,
}

/// Decide whether a handler needs the allocator. Returns true if the
/// handler's body or any macro it transitively invokes contains a `temp`
/// or `ftemp` declaration.
pub fn handler_uses_named_temps(handler: &Handler, program: &Program) -> bool {
    let mut visited_macros = HashSet::new();
    body_uses_named_temps(&handler.instructions, program, &mut visited_macros)
}

fn body_uses_named_temps(
    body: &[AsmInstruction],
    program: &Program,
    visited: &mut HashSet<String>,
) -> bool {
    for insn in body {
        if insn.mnemonic == "temp" || insn.mnemonic == "ftemp" {
            return true;
        }
        if let Some(mac) = program.macros.get(&insn.mnemonic) {
            if visited.insert(insn.mnemonic.clone())
                && body_uses_named_temps(&mac.body, program, visited)
            {
                return true;
            }
        }
    }
    false
}

/// Flatten a handler body and run register allocation on it. Returns the
/// rewritten flat instruction list with all named-temp references replaced
/// by their assigned physical registers.
pub fn flatten_and_allocate(
    handler: &Handler,
    program: &Program,
    arch: Arch,
    macro_counter: &mut u32,
) -> Result<Vec<AsmInstruction>, AllocationError> {
    let flat = flatten(handler, program, macro_counter);
    let needs = flat
        .iter()
        .any(|i| i.mnemonic == "temp" || i.mnemonic == "ftemp");
    if !needs {
        return Ok(flat);
    }
    let assignments = allocate(handler, &flat, program, arch)?;
    Ok(rewrite(flat, &assignments))
}

// ============================================================================
// Flattening (macro expansion + per-expansion uniquification)
// ============================================================================

/// Expand all macro invocations into a flat instruction list. Each macro
/// expansion uniquifies its local labels (matching the existing
/// `uniquify_macro_labels` behavior) and its `temp` / `ftemp` declarations
/// so two invocations of the same macro produce disjoint name sets.
pub fn flatten(
    handler: &Handler,
    program: &Program,
    counter: &mut u32,
) -> Vec<AsmInstruction> {
    let mut out = Vec::new();
    flatten_into(&handler.instructions, program, counter, &HashMap::new(), &mut out);
    out
}

fn flatten_into(
    insns: &[AsmInstruction],
    program: &Program,
    counter: &mut u32,
    parent_substitution: &HashMap<String, Operand>,
    out: &mut Vec<AsmInstruction>,
) {
    for insn in insns {
        // Apply any inherited macro-parameter substitution first so the
        // mnemonic itself can be a parameter (the existing macro semantics
        // treats the mnemonic as a substitutable name).
        let substituted = substitute_operands(insn, parent_substitution);

        if let Some(mac) = program.macros.get(&substituted.mnemonic) {
            // Build a substitution map: each parameter maps to the operand
            // the caller passed at that position.
            let mut param_map: HashMap<String, Operand> = HashMap::new();
            for (i, param) in mac.params.iter().enumerate() {
                if let Some(op) = substituted.operands.get(i) {
                    param_map.insert(param.clone(), op.clone());
                }
            }

            let id = *counter;
            *counter += 1;

            // Uniquify any self-contained labels in the macro body.
            let body = uniquify_macro_locals(&mac.body, id);
            flatten_into(&body, program, counter, &param_map, out);
        } else {
            out.push(substituted);
        }
    }
}

/// Rename labels that are both defined and referenced inside the macro
/// body, and rename `temp` / `ftemp` declarations together with all their
/// references inside the body. Names that appear to cross the macro
/// boundary (defined here, referenced outside, or vice versa) are left
/// alone so they keep matching the caller's view.
fn uniquify_macro_locals(body: &[AsmInstruction], suffix: u32) -> Vec<AsmInstruction> {
    // Collect label definitions and references separately.
    let mut label_defs: HashSet<String> = HashSet::new();
    let mut label_refs: HashSet<String> = HashSet::new();
    let mut temps_declared: HashSet<String> = HashSet::new();
    for insn in body {
        if insn.mnemonic == "label" {
            if let Some(Operand::Label(name)) = insn.operands.first() {
                if name.starts_with('.') {
                    label_defs.insert(name.clone());
                }
            }
        } else if insn.mnemonic == "temp" || insn.mnemonic == "ftemp" {
            for op in &insn.operands {
                if let Operand::Register(name) = op {
                    temps_declared.insert(name.clone());
                }
            }
        } else {
            for op in &insn.operands {
                if let Operand::Label(name) = op {
                    if name.starts_with('.') {
                        label_refs.insert(name.clone());
                    }
                }
            }
        }
    }

    let label_renames: HashMap<String, String> = label_defs
        .intersection(&label_refs)
        .map(|name| (name.clone(), format!("{name}_m{suffix}")))
        .collect();

    let temp_renames: HashMap<String, String> = temps_declared
        .iter()
        .map(|name| (name.clone(), format!("{name}_m{suffix}")))
        .collect();

    if label_renames.is_empty() && temp_renames.is_empty() {
        return body.to_vec();
    }

    body.iter()
        .map(|insn| rename_in_instruction(insn, &label_renames, &temp_renames))
        .collect()
}

fn rename_in_instruction(
    insn: &AsmInstruction,
    label_renames: &HashMap<String, String>,
    temp_renames: &HashMap<String, String>,
) -> AsmInstruction {
    let rename_string = |s: &str| -> String {
        if let Some(new) = temp_renames.get(s) {
            new.clone()
        } else {
            s.to_string()
        }
    };
    let operands = insn
        .operands
        .iter()
        .map(|op| match op {
            Operand::Label(name) => {
                if let Some(new_name) = label_renames.get(name) {
                    Operand::Label(new_name.clone())
                } else {
                    op.clone()
                }
            }
            Operand::Register(name) => {
                if let Some(new_name) = temp_renames.get(name) {
                    Operand::Register(new_name.clone())
                } else {
                    op.clone()
                }
            }
            Operand::Memory { base, index, scale } => Operand::Memory {
                base: rename_string(base),
                index: index.as_ref().map(|s| rename_string(s)),
                scale: scale.as_ref().map(|s| rename_string(s)),
            },
            _ => op.clone(),
        })
        .collect();
    AsmInstruction {
        mnemonic: insn.mnemonic.clone(),
        operands,
    }
}

/// Replace operand-level references named in `param_map` with the caller's
/// operand. Memory operands have their base/index/scale strings substituted
/// when those strings happen to be parameter names.
fn substitute_operands(
    insn: &AsmInstruction,
    param_map: &HashMap<String, Operand>,
) -> AsmInstruction {
    if param_map.is_empty() {
        return insn.clone();
    }

    let substitute_string = |s: &str| -> String {
        match param_map.get(s) {
            Some(Operand::Register(n)) => n.clone(),
            Some(Operand::Constant(n)) => n.clone(),
            Some(Operand::FieldRef(n)) => n.clone(),
            Some(Operand::Immediate(v)) => v.to_string(),
            _ => s.to_string(),
        }
    };

    let operands = insn
        .operands
        .iter()
        .map(|op| match op {
            Operand::Register(name) => param_map
                .get(name)
                .cloned()
                .unwrap_or_else(|| op.clone()),
            Operand::Constant(name) => param_map
                .get(name)
                .cloned()
                .unwrap_or_else(|| op.clone()),
            Operand::FieldRef(name) => param_map
                .get(name)
                .cloned()
                .unwrap_or_else(|| op.clone()),
            Operand::Memory { base, index, scale } => Operand::Memory {
                base: substitute_string(base),
                index: index.as_ref().map(|s| substitute_string(s)),
                scale: scale.as_ref().map(|s| substitute_string(s)),
            },
            _ => op.clone(),
        })
        .collect();

    let mnemonic = match param_map.get(&insn.mnemonic) {
        Some(Operand::Register(n)) => n.clone(),
        _ => insn.mnemonic.clone(),
    };

    AsmInstruction { mnemonic, operands }
}

// ============================================================================
// Use/def computation
// ============================================================================

/// Per-instruction register footprint. All entries are register *names*
/// (either a named-temp string like "foo" or a physical reg like "rax");
/// the allocator distinguishes by checking whether each name is a declared
/// temp or a known physical/positional reg.
#[derive(Debug, Default, Clone)]
struct UseDef {
    uses: BTreeSet<String>,
    defs: BTreeSet<String>,
    // Physical registers killed at this instruction even when not explicitly
    // listed as a def (hidden clobbers, implicit outputs, all-caller-saved
    // for is_call=true).
    kills: BTreeSet<String>,
}

fn analyze_instruction(
    insn: &AsmInstruction,
    arch: Arch,
    declared_gpr_temps: &HashSet<String>,
    declared_fpr_temps: &HashSet<String>,
) -> UseDef {
    let mut ud = UseDef::default();

    // `temp` / `ftemp` decls are pure annotations; they do not contribute
    // uses or defs by themselves. (The first use of the named temp later
    // in the body is what starts its live range.)
    if insn.mnemonic == "temp" || insn.mnemonic == "ftemp" {
        return ud;
    }

    // `label` is a pure marker. (Its operand is a Label, not a register.)
    if insn.mnemonic == "label" {
        return ud;
    }

    // `xor dst, dst` is the canonical zero-the-register idiom on x86. The
    // generated machine instruction reads no input; treat both operands as
    // pure defs so the temp doesn't appear live before this point.
    if insn.mnemonic == "xor" && insn.operands.len() == 2 {
        if let (Operand::Register(a), Operand::Register(b)) =
            (&insn.operands[0], &insn.operands[1])
        {
            if a == b
                && is_register_name(a, arch, declared_gpr_temps, declared_fpr_temps)
            {
                ud.defs.insert(a.clone());
                return ud;
            }
        }
    }

    let info = lookup(&insn.mnemonic);

    let push_register_use = |ud: &mut UseDef, name: &str| {
        if is_register_name(name, arch, declared_gpr_temps, declared_fpr_temps) {
            ud.uses.insert(name.to_string());
        }
    };
    let push_register_def = |ud: &mut UseDef, name: &str| {
        if is_register_name(name, arch, declared_gpr_temps, declared_fpr_temps) {
            ud.defs.insert(name.to_string());
        }
    };

    // Walk operands using the metadata if available.
    if let Some(info) = info {
        for (i, op) in insn.operands.iter().enumerate() {
            let kind = operand_kind_at(info, i);
            apply_operand_kind(&mut ud, op, kind, &push_register_use, &push_register_def);
        }
        // Hidden clobbers / implicit i/o / all-caller-saved kills.
        let arch_spec = info.arch(arch);
        for r in arch_spec.implicit_inputs {
            ud.uses.insert((*r).to_string());
        }
        for r in arch_spec.implicit_outputs {
            ud.defs.insert((*r).to_string());
            ud.kills.insert((*r).to_string());
        }
        for r in arch_spec.clobbers_gpr {
            ud.kills.insert((*r).to_string());
        }
        for r in arch_spec.clobbers_fpr {
            ud.kills.insert((*r).to_string());
        }
        if info.is_call {
            let (gpr, fpr) = caller_saved(arch);
            for r in gpr {
                ud.kills.insert((*r).to_string());
            }
            for r in fpr {
                ud.kills.insert((*r).to_string());
            }
        }
    } else {
        // No metadata: conservative fallback. Treat every operand as both a
        // use and a def. The InstructionInfo table is supposed to cover
        // every codegen mnemonic, so this branch should be rare.
        for op in &insn.operands {
            collect_operand_registers(op, &mut |name| push_register_use(&mut ud, name));
            collect_operand_registers(op, &mut |name| push_register_def(&mut ud, name));
        }
    }

    ud
}

fn operand_kind_at(info: &InstructionInfo, index: usize) -> OperandKind {
    if let Some(k) = info.operands.get(index) {
        return *k;
    }
    info.variadic_kind
        .expect("operand index out of range without variadic_kind")
}

fn apply_operand_kind(
    ud: &mut UseDef,
    op: &Operand,
    kind: OperandKind,
    push_use: &dyn Fn(&mut UseDef, &str),
    push_def: &dyn Fn(&mut UseDef, &str),
) {
    match kind {
        OperandKind::GprIn | OperandKind::FprIn => {
            if let Operand::Register(name) = op {
                push_use(ud, name);
            }
        }
        OperandKind::GprOut | OperandKind::FprOut => {
            if let Operand::Register(name) = op {
                push_def(ud, name);
            }
        }
        OperandKind::GprInOut | OperandKind::FprInOut => {
            if let Operand::Register(name) = op {
                push_use(ud, name);
                push_def(ud, name);
            }
        }
        OperandKind::GprInOrImm => {
            if let Operand::Register(name) = op {
                push_use(ud, name);
            }
        }
        OperandKind::Memory => {
            // Any GPR named inside the brackets is a use.
            if let Operand::Memory { base, index, scale } = op {
                push_use(ud, base);
                if let Some(s) = index {
                    push_use(ud, s);
                }
                if let Some(s) = scale {
                    push_use(ud, s);
                }
            }
        }
        OperandKind::Imm
        | OperandKind::Label
        | OperandKind::FieldRef
        | OperandKind::FuncSymbol => {}
    }
}

/// Count how many times each register name appears in the operand stream.
/// This is a coarse spill-cost proxy: the more an x86_64 temp is referenced,
/// the more bytes we save by placing it in a low-encoding-cost register.
fn count_register_uses(instructions: &[AsmInstruction]) -> HashMap<String, u32> {
    let mut counts: HashMap<String, u32> = HashMap::new();
    for insn in instructions {
        if insn.mnemonic == "temp" || insn.mnemonic == "ftemp" {
            // Declarations themselves are not real uses.
            continue;
        }
        for op in &insn.operands {
            collect_operand_registers(op, &mut |name| {
                *counts.entry(name.to_string()).or_insert(0) += 1;
            });
        }
    }
    counts
}

fn collect_operand_registers<F: FnMut(&str)>(op: &Operand, f: &mut F) {
    match op {
        Operand::Register(name) => f(name),
        Operand::Memory { base, index, scale } => {
            f(base);
            if let Some(s) = index {
                f(s);
            }
            if let Some(s) = scale {
                f(s);
            }
        }
        _ => {}
    }
}

/// Whether a name refers to *any* register: a declared named temp, a
/// positional alias (t0, ft1, pc, ...), or a known physical reg.
fn is_register_name(
    name: &str,
    arch: Arch,
    declared_gpr_temps: &HashSet<String>,
    declared_fpr_temps: &HashSet<String>,
) -> bool {
    declared_gpr_temps.contains(name)
        || declared_fpr_temps.contains(name)
        || resolve_register(name, arch).is_some()
        || is_physical_register(name, arch)
}

fn is_physical_register(name: &str, arch: Arch) -> bool {
    match arch {
        Arch::X86_64 => name.starts_with('r')
            || name.starts_with('e')
            || name.starts_with("xmm"),
        Arch::Aarch64 => {
            (name.starts_with('x') || name.starts_with('w'))
                && name[1..].chars().all(|c| c.is_ascii_digit())
                || (name.starts_with('d') || name.starts_with('s'))
                    && name[1..].chars().all(|c| c.is_ascii_digit())
        }
    }
}

// ============================================================================
// Liveness (iterative backward dataflow)
// ============================================================================

#[derive(Debug, Default)]
struct Liveness {
    /// Successors of each instruction by index.
    successors: Vec<Vec<usize>>,
    /// `live_in[i]` = registers live just before instruction `i`.
    live_in: Vec<BTreeSet<String>>,
    /// `live_out[i]` = registers live just after instruction `i`.
    live_out: Vec<BTreeSet<String>>,
    /// Per-instruction use/def/kill sets.
    use_def: Vec<UseDef>,
}

fn compute_liveness(
    handler_name: &str,
    instructions: &[AsmInstruction],
    arch: Arch,
    declared_gpr_temps: &HashSet<String>,
    declared_fpr_temps: &HashSet<String>,
) -> Result<Liveness, AllocationError> {
    let n = instructions.len();
    let mut successors: Vec<Vec<usize>> = vec![Vec::new(); n];

    // Map label -> instruction index for branch resolution.
    let mut labels: HashMap<String, usize> = HashMap::new();
    for (i, insn) in instructions.iter().enumerate() {
        if insn.mnemonic == "label" {
            if let Some(Operand::Label(name)) = insn.operands.first() {
                labels.insert(name.clone(), i);
            }
        }
    }

    for (i, insn) in instructions.iter().enumerate() {
        // The only mnemonics without an InstructionInfo entry are the
        // declaration pseudo-instructions `temp` and `ftemp`, which are
        // pure annotations and never end the basic block.
        let terminal = match lookup(&insn.mnemonic) {
            Some(info) => info.terminal,
            None => {
                assert!(
                    insn.mnemonic == "temp" || insn.mnemonic == "ftemp",
                    "no InstructionInfo for mnemonic '{}' in handler '{handler_name}'",
                    insn.mnemonic
                );
                false
            }
        };

        // Branch targets (any Label operand is a possible successor for
        // non-jmp branches; jmp itself replaces fall-through).
        for op in &insn.operands {
            if let Operand::Label(name) = op {
                if let Some(&target) = labels.get(name) {
                    successors[i].push(target);
                }
            }
        }

        // Fall-through unless terminal.
        if !terminal && i + 1 < n {
            successors[i].push(i + 1);
        }
    }

    let use_def: Vec<UseDef> = instructions
        .iter()
        .map(|insn| analyze_instruction(insn, arch, declared_gpr_temps, declared_fpr_temps))
        .collect();

    let mut live_in: Vec<BTreeSet<String>> = vec![BTreeSet::new(); n];
    let mut live_out: Vec<BTreeSet<String>> = vec![BTreeSet::new(); n];

    // Iterative backward dataflow until fixpoint.
    let mut changed = true;
    while changed {
        changed = false;
        for i in (0..n).rev() {
            let mut new_out: BTreeSet<String> = BTreeSet::new();
            for &succ in &successors[i] {
                new_out.extend(live_in[succ].iter().cloned());
            }
            // live_in = use ∪ (live_out - def - kill)
            let mut new_in: BTreeSet<String> = use_def[i].uses.clone();
            for r in &new_out {
                if !use_def[i].defs.contains(r) && !use_def[i].kills.contains(r) {
                    new_in.insert(r.clone());
                }
            }
            if new_in != live_in[i] || new_out != live_out[i] {
                changed = true;
                live_in[i] = new_in;
                live_out[i] = new_out;
            }
        }
    }

    // Sanity-check: a temp must be defined before its first use along
    // every path. We catch the simplest case: the handler entry has no
    // declared temps live-in.
    let entry_live = &live_in[0];
    for r in entry_live {
        if declared_gpr_temps.contains(r) || declared_fpr_temps.contains(r) {
            // Walk forward and find the first instruction that pulls this
            // temp into its live-in without having defined it yet, so the
            // error points at the actual problem.
            let mut culprit = String::from("(unknown)");
            for (i, ud) in use_def.iter().enumerate() {
                if ud.uses.contains(r) && !ud.defs.contains(r) {
                    culprit = format!("instruction #{i}: {:?}", instructions[i]);
                    break;
                }
            }
            return Err(AllocationError {
                handler: handler_name.to_string(),
                message: format!(
                    "named temp '{r}' is used before being assigned (live at handler entry); first use: {culprit}"
                ),
            });
        }
    }

    Ok(Liveness {
        successors,
        live_in,
        live_out,
        use_def,
    })
}

// ============================================================================
// Interference and allocation
// ============================================================================

#[derive(Debug)]
struct AllocationPlan {
    gpr_assignments: HashMap<String, &'static str>,
    fpr_assignments: HashMap<String, &'static str>,
}

fn allocate(
    handler: &Handler,
    instructions: &[AsmInstruction],
    program: &Program,
    arch: Arch,
) -> Result<AllocationPlan, AllocationError> {
    let _ = program;

    // Collect named temp declarations.
    let mut declared_gpr_temps: HashSet<String> = HashSet::new();
    let mut declared_fpr_temps: HashSet<String> = HashSet::new();
    for insn in instructions {
        match insn.mnemonic.as_str() {
            "temp" => {
                for op in &insn.operands {
                    if let Operand::Register(name) = op {
                        if !declared_gpr_temps.insert(name.clone()) {
                            return Err(AllocationError {
                                handler: handler.name.clone(),
                                message: format!("named temp '{name}' declared more than once"),
                            });
                        }
                        if declared_fpr_temps.contains(name) {
                            return Err(AllocationError {
                                handler: handler.name.clone(),
                                message: format!(
                                    "named temp '{name}' declared as both temp and ftemp"
                                ),
                            });
                        }
                        if resolve_register(name, arch).is_some() {
                            return Err(AllocationError {
                                handler: handler.name.clone(),
                                message: format!(
                                    "named temp '{name}' shadows a positional alias or pinned register"
                                ),
                            });
                        }
                    }
                }
            }
            "ftemp" => {
                for op in &insn.operands {
                    if let Operand::Register(name) = op {
                        if !declared_fpr_temps.insert(name.clone()) {
                            return Err(AllocationError {
                                handler: handler.name.clone(),
                                message: format!("named ftemp '{name}' declared more than once"),
                            });
                        }
                        if declared_gpr_temps.contains(name) {
                            return Err(AllocationError {
                                handler: handler.name.clone(),
                                message: format!(
                                    "named temp '{name}' declared as both temp and ftemp"
                                ),
                            });
                        }
                        if resolve_register(name, arch).is_some() {
                            return Err(AllocationError {
                                handler: handler.name.clone(),
                                message: format!(
                                    "named ftemp '{name}' shadows a positional alias or pinned register"
                                ),
                            });
                        }
                    }
                }
            }
            _ => {}
        }
    }

    // For each named temp, the set of instruction indices where it is
    // alive (in live_in or live_out). This is its live range.
    let liveness = compute_liveness(
        &handler.name,
        instructions,
        arch,
        &declared_gpr_temps,
        &declared_fpr_temps,
    )?;

    // Per-temp sets of instruction indices, split by "alive before the
    // instruction begins" (live_in) and "alive after the instruction
    // ends" (live_out). The split matters for interference: a use-only
    // operand at instruction i (live_in but not live_out) does not
    // collide with a def-only operand at the same instruction (live_out
    // but not live_in), because the codegen reads inputs before writing
    // outputs. The original positional code exploited this directly by
    // reusing the same physical register as a load's address operand and
    // its destination operand.
    let mut alive_in: HashMap<String, BTreeSet<usize>> = HashMap::new();
    let mut alive_out: HashMap<String, BTreeSet<usize>> = HashMap::new();
    let mut crosses: HashMap<String, BTreeSet<usize>> = HashMap::new();
    for name in declared_gpr_temps.iter().chain(declared_fpr_temps.iter()) {
        let mut ain = BTreeSet::new();
        let mut aout = BTreeSet::new();
        let mut crossing = BTreeSet::new();
        for i in 0..instructions.len() {
            let live_before = liveness.live_in[i].contains(name);
            let live_after = liveness.live_out[i].contains(name);
            if live_before {
                ain.insert(i);
            }
            if live_after {
                aout.insert(i);
            }
            if live_before
                && live_after
                && !liveness.use_def[i].defs.contains(name)
            {
                crossing.insert(i);
            }
        }
        alive_in.insert(name.clone(), ain);
        alive_out.insert(name.clone(), aout);
        crosses.insert(name.clone(), crossing);
    }

    // Per-instruction set of physical registers killed (defs + kills).
    let mut killed_phys_per_insn: Vec<BTreeSet<String>> = vec![BTreeSet::new(); instructions.len()];
    for (i, ud) in liveness.use_def.iter().enumerate() {
        for r in ud.defs.iter().chain(ud.kills.iter()) {
            if !declared_gpr_temps.contains(r) && !declared_fpr_temps.contains(r) {
                killed_phys_per_insn[i].insert(r.clone());
            }
        }
    }

    // Hard fixed-operand constraints: for each instruction, if operand i
    // must resolve to physical reg P and operand i is a named temp, the
    // temp must be allocated to P.
    let mut hard_pins: HashMap<String, &'static str> = HashMap::new();
    for insn in instructions {
        let info = match lookup(&insn.mnemonic) {
            Some(info) => info,
            None => continue,
        };
        let arch_spec = info.arch(arch);
        for &(op_index, phys) in arch_spec.fixed_operands {
            if let Some(Operand::Register(name)) = insn.operands.get(op_index) {
                if declared_gpr_temps.contains(name) || declared_fpr_temps.contains(name) {
                    if let Some(prev) = hard_pins.insert(name.clone(), phys) {
                        if prev != phys {
                            return Err(AllocationError {
                                handler: handler.name.clone(),
                                message: format!(
                                    "named temp '{name}' is pinned to two different physical registers ({prev} and {phys})"
                                ),
                            });
                        }
                    }
                }
            }
        }
    }

    // Per-temp set of physical regs forbidden because the temp is named as
    // an operand at an instruction that clobbers them as scratch. This is
    // distinct from the "passes through" rule -- an operand register that
    // also happens to be the codegen's scratch register collides even when
    // the operand is a brand-new def, because the codegen's scratch use is
    // interleaved with the operand write (see e.g. x86 box_int32_clean,
    // which clobbers rax while it's also writing the dst operand).
    //
    // Implicit-output registers are treated the same way: an x86 idiv
    // writes rax/rdx and any operand-temp that overlaps either of those
    // physical regs would be clobbered before the idiv reads it. The
    // exception is the pinned operand at that index -- fixed_operands
    // already places it in the implicit-output reg by design.
    let mut operand_forbids: HashMap<String, HashSet<&'static str>> = HashMap::new();
    for insn in instructions {
        let info = match lookup(&insn.mnemonic) {
            Some(info) => info,
            None => continue,
        };
        let arch_spec = info.arch(arch);
        if arch_spec.clobbers_gpr.is_empty()
            && arch_spec.clobbers_fpr.is_empty()
            && arch_spec.implicit_outputs.is_empty()
        {
            continue;
        }
        for (op_index, op) in insn.operands.iter().enumerate() {
            if let Operand::Register(name) = op {
                if !(declared_gpr_temps.contains(name) || declared_fpr_temps.contains(name)) {
                    continue;
                }
                let pinned_here = arch_spec
                    .fixed_operands
                    .iter()
                    .find_map(|&(idx, phys)| (idx == op_index).then_some(phys));
                let entry = operand_forbids.entry(name.clone()).or_default();
                let mut add = |r: &'static str| {
                    if Some(r) != pinned_here {
                        entry.insert(r);
                    }
                };
                for r in arch_spec.clobbers_gpr {
                    add(*r);
                }
                for r in arch_spec.clobbers_fpr {
                    add(*r);
                }
                for r in arch_spec.implicit_outputs {
                    add(*r);
                }
            }
        }
    }

    // Build candidate physical-register pools.
    let mapping = mapping_for(arch);
    let gpr_pool: Vec<&'static str> = mapping.temporaries.to_vec();
    let fpr_pool: Vec<&'static str> = mapping.fp_temporaries.to_vec();

    // Per-temp use count: every operand-level reference to the temp's name
    // (including occurrences inside memory operands) contributes one use.
    // On x86_64 each saved use of a low-cost register saves an encoding
    // byte, so hot temps want to land in cheap registers.
    let use_counts = count_register_uses(instructions);

    let all_temps: Vec<(String, bool)> = declared_gpr_temps
        .iter()
        .map(|n| (n.clone(), true))
        .chain(declared_fpr_temps.iter().map(|n| (n.clone(), false)))
        .collect();

    // Greedy graph coloring is sensitive to the order temps are processed
    // in. Two orders are useful here:
    //
    //   - Use-count-first is cost-optimal: the temp with the most operand
    //     references claims the cheapest register, so byte savings
    //     concentrate where they matter most.
    //
    //   - Live-range-first is fit-optimal: the most constrained temps
    //     (those alive across the most instructions, which see the most
    //     interference) claim registers first and are the most likely to
    //     fit. Some packed handlers (e.g. `Call`) only color successfully
    //     under this order.
    //
    // We try cost-first and fall back to fit-first only when cost-first
    // can't color. This wins the byte savings when the handler has slack
    // and never regresses fit when it doesn't.
    let try_color = |sorted: &[(String, bool)]| -> Result<AllocationPlan, AllocationError> {
        color(
            handler,
            sorted,
            &alive_in,
            &alive_out,
            &crosses,
            &killed_phys_per_insn,
            &operand_forbids,
            &hard_pins,
            &gpr_pool,
            &fpr_pool,
            arch,
        )
    };

    let by_use_count_first = |a: &(String, bool), b: &(String, bool)| {
        let ap = hard_pins.contains_key(&a.0);
        let bp = hard_pins.contains_key(&b.0);
        let au = use_counts.get(&a.0).copied().unwrap_or(0);
        let bu = use_counts.get(&b.0).copied().unwrap_or(0);
        let aw = alive_in.get(&a.0).map_or(0, |s| s.len())
            + alive_out.get(&a.0).map_or(0, |s| s.len());
        let bw = alive_in.get(&b.0).map_or(0, |s| s.len())
            + alive_out.get(&b.0).map_or(0, |s| s.len());
        bp.cmp(&ap)
            .then_with(|| bu.cmp(&au))
            .then_with(|| bw.cmp(&aw))
            .then_with(|| a.0.cmp(&b.0))
    };
    let by_live_range_first = |a: &(String, bool), b: &(String, bool)| {
        let ap = hard_pins.contains_key(&a.0);
        let bp = hard_pins.contains_key(&b.0);
        let au = use_counts.get(&a.0).copied().unwrap_or(0);
        let bu = use_counts.get(&b.0).copied().unwrap_or(0);
        let aw = alive_in.get(&a.0).map_or(0, |s| s.len())
            + alive_out.get(&a.0).map_or(0, |s| s.len());
        let bw = alive_in.get(&b.0).map_or(0, |s| s.len())
            + alive_out.get(&b.0).map_or(0, |s| s.len());
        bp.cmp(&ap)
            .then_with(|| bw.cmp(&aw))
            .then_with(|| bu.cmp(&au))
            .then_with(|| a.0.cmp(&b.0))
    };

    let mut sorted = all_temps.clone();
    sorted.sort_by(by_use_count_first);
    if let Ok(plan) = try_color(&sorted) {
        return Ok(plan);
    }

    let mut sorted = all_temps;
    sorted.sort_by(by_live_range_first);
    try_color(&sorted)
}

/// Greedy linear-scan coloring driven by the order in `sorted_temps`.
/// Returns Err if any temp can't be placed; the caller decides whether to
/// retry with a different ordering.
#[allow(clippy::too_many_arguments)]
fn color(
    handler: &Handler,
    sorted_temps: &[(String, bool)],
    alive_in: &HashMap<String, BTreeSet<usize>>,
    alive_out: &HashMap<String, BTreeSet<usize>>,
    crosses: &HashMap<String, BTreeSet<usize>>,
    killed_phys_per_insn: &[BTreeSet<String>],
    operand_forbids: &HashMap<String, HashSet<&'static str>>,
    hard_pins: &HashMap<String, &'static str>,
    gpr_pool: &[&'static str],
    fpr_pool: &[&'static str],
    arch: Arch,
) -> Result<AllocationPlan, AllocationError> {
    let mut gpr_assignments: HashMap<String, &'static str> = HashMap::new();
    let mut fpr_assignments: HashMap<String, &'static str> = HashMap::new();

    for (name, is_gpr) in sorted_temps {
        let pool: &[&'static str] = if *is_gpr { gpr_pool } else { fpr_pool };
        let my_in = alive_in
            .get(name)
            .expect("missing live_in for declared temp");
        let my_out = alive_out
            .get(name)
            .expect("missing live_out for declared temp");
        let crossing = crosses.get(name).expect("missing cross-set for declared temp");

        // Build the set of physical regs we cannot use for this temp:
        //   - any phys reg killed at an instruction the temp passes
        //     through (alive on both sides without being redefined). A
        //     temp that only enters (use, dying here) or only exits (def,
        //     newly born here) the instruction is fine: the kill happens
        //     between input read and output write, so the temp's value
        //     is no longer needed (or not yet needed) at that moment.
        //   - any phys reg listed in operand_forbids for this temp.
        //   - any phys reg already assigned to another temp that overlaps
        //     ours either before-the-instruction (alive_in vs alive_in)
        //     or after-the-instruction (alive_out vs alive_out). A
        //     "before only" overlap with an "after only" overlap at the
        //     same instruction does NOT collide: the codegen reads input
        //     operands before writing output operands, so the same
        //     physical register can host an outgoing value at one
        //     instruction and an incoming value at the same instruction
        //     -- this is exactly how the original positional code
        //     reused `t6` for vm_ptr (read) and frame_base (written) in
        //     the same load_pair64.
        let mut forbidden: HashSet<&'static str> = HashSet::new();
        for &i in crossing {
            for r in &killed_phys_per_insn[i] {
                if let Some(slot) = pool.iter().find(|p| **p == r.as_str()) {
                    forbidden.insert(*slot);
                }
            }
        }
        if let Some(extra) = operand_forbids.get(name) {
            for r in extra {
                if pool.iter().any(|p| *p == *r) {
                    forbidden.insert(*r);
                }
            }
        }

        for (other, &other_phys) in gpr_assignments.iter().chain(fpr_assignments.iter()) {
            if other == name {
                continue;
            }
            let other_in = alive_in
                .get(other)
                .expect("missing live_in for already-assigned temp");
            let other_out = alive_out
                .get(other)
                .expect("missing live_out for already-assigned temp");
            let inputs_overlap = !my_in.is_disjoint(other_in);
            let outputs_overlap = !my_out.is_disjoint(other_out);
            if inputs_overlap || outputs_overlap {
                forbidden.insert(other_phys);
            }
        }

        let pick = if let Some(&pinned) = hard_pins.get(name) {
            if !pool.iter().any(|p| *p == pinned) {
                return Err(AllocationError {
                    handler: handler.name.clone(),
                    message: format!(
                        "named temp '{name}' must be pinned to '{pinned}', but that register is not in the {} pool",
                        if *is_gpr { "GPR" } else { "FPR" }
                    ),
                });
            }
            if forbidden.contains(&pinned) {
                let mut sorted: Vec<&str> = forbidden.iter().copied().collect();
                sorted.sort();
                return Err(AllocationError {
                    handler: handler.name.clone(),
                    message: format!(
                        "named temp '{name}' must be pinned to '{pinned}', but that register is killed or occupied across its live range (forbidden: {sorted:?})"
                    ),
                });
            }
            pinned
        } else {
            // Pick the cheapest available register for this temp. Pool
            // position breaks ties so behavior stays deterministic when
            // several registers tie in cost (e.g. all FPRs, or all
            // aarch64 GPRs).
            *pool
                .iter()
                .enumerate()
                .filter(|(_, p)| !forbidden.contains(*p))
                .min_by_key(|(i, p)| (register_cost(p, arch), *i))
                .map(|(_, p)| p)
                .ok_or_else(|| AllocationError {
                    handler: handler.name.clone(),
                    message: format!(
                        "could not allocate {} '{name}': {} pool exhausted (forbidden: {:?})",
                        if *is_gpr { "temp" } else { "ftemp" },
                        if *is_gpr { "GPR" } else { "FPR" },
                        forbidden,
                    ),
                })?
        };

        if *is_gpr {
            gpr_assignments.insert(name.clone(), pick);
        } else {
            fpr_assignments.insert(name.clone(), pick);
        }
    }

    Ok(AllocationPlan {
        gpr_assignments,
        fpr_assignments,
    })
}

// ============================================================================
// Operand rewriting
// ============================================================================

fn rewrite(instructions: Vec<AsmInstruction>, plan: &AllocationPlan) -> Vec<AsmInstruction> {
    let mut combined: HashMap<String, &'static str> = HashMap::new();
    combined.extend(plan.gpr_assignments.iter().map(|(k, v)| (k.clone(), *v)));
    combined.extend(plan.fpr_assignments.iter().map(|(k, v)| (k.clone(), *v)));

    instructions
        .into_iter()
        .filter_map(|insn| {
            // Drop the declarations themselves; codegen no-ops them anyway,
            // but pruning here keeps the IR cleaner if other passes look at
            // it.
            if insn.mnemonic == "temp" || insn.mnemonic == "ftemp" {
                return None;
            }
            let operands = insn
                .operands
                .into_iter()
                .map(|op| rewrite_operand(op, &combined))
                .collect();
            Some(AsmInstruction {
                mnemonic: insn.mnemonic,
                operands,
            })
        })
        .collect()
}

fn rewrite_operand(op: Operand, table: &HashMap<String, &'static str>) -> Operand {
    let rewrite_str = |s: String| -> String {
        match table.get(&s) {
            Some(p) => (*p).to_string(),
            None => s,
        }
    };
    match op {
        Operand::Register(name) => match table.get(&name) {
            Some(p) => Operand::Register((*p).to_string()),
            None => Operand::Register(name),
        },
        Operand::Memory { base, index, scale } => Operand::Memory {
            base: rewrite_str(base),
            index: index.map(|s| rewrite_str(s)),
            scale: scale.map(|s| rewrite_str(s)),
        },
        other => other,
    }
}

// ============================================================================
// Tests
// ============================================================================

#[cfg(test)]
mod tests {
    use super::*;
    use crate::parser::parse;

    fn build(source: &str, arch: Arch) -> Result<Vec<AsmInstruction>, AllocationError> {
        let mut program = parse(source);
        // Inject a minimal Mov layout so dispatch_next has a known size.
        use bytecode_def::OpLayout;
        program.op_layouts.insert(
            "Test".into(),
            OpLayout {
                field_offsets: HashMap::new(),
                size: Some(4),
            },
        );
        let mut counter = 0u32;
        flatten_and_allocate(&program.handlers[0], &program, arch, &mut counter)
    }

    fn assignment_for(insns: &[AsmInstruction], mnemonic: &str, op_index: usize) -> String {
        for insn in insns {
            if insn.mnemonic == mnemonic {
                if let Some(Operand::Register(name)) = insn.operands.get(op_index) {
                    return name.clone();
                }
            }
        }
        panic!("no '{mnemonic}' instruction found");
    }

    #[test]
    fn allocates_simple_named_temp() {
        let src = "
handler Test
    temp foo
    mov foo, 42
    add foo, 1
    store_operand m_dst, foo
end
";
        let out = build(src, Arch::X86_64).expect("allocation should succeed");
        // `foo` should be assigned to one of t0..t8 = rax..r11.
        let name = assignment_for(&out, "mov", 0);
        assert!(
            ["rax", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11"]
                .iter()
                .any(|p| *p == name),
            "expected physical reg, got {name}"
        );
        // No `temp` decl should remain in the rewritten output.
        for insn in &out {
            assert_ne!(insn.mnemonic, "temp");
            assert_ne!(insn.mnemonic, "ftemp");
        }
    }

    #[test]
    fn allocates_two_temps_to_distinct_registers_when_overlapping() {
        let src = "
handler Test
    temp a, b
    mov a, 1
    mov b, 2
    add a, b
end
";
        let out = build(src, Arch::X86_64).expect("allocation should succeed");
        let a = assignment_for(&out, "mov", 0);
        // The second mov target is `b` after rewriting.
        let mut bs: Vec<&Operand> = Vec::new();
        let mut seen_first = false;
        for insn in &out {
            if insn.mnemonic == "mov" {
                if seen_first {
                    bs.push(&insn.operands[0]);
                    break;
                }
                seen_first = true;
            }
        }
        let b = match bs[0] {
            Operand::Register(n) => n.clone(),
            _ => unreachable!(),
        };
        assert_ne!(a, b, "overlapping temps must get distinct regs");
    }

    #[test]
    fn pins_named_temp_to_rcx_for_shift_count() {
        let src = "
handler Test
    temp value, count
    mov value, 0xff
    mov count, 3
    shr value, count
end
";
        let out = build(src, Arch::X86_64).expect("allocation should succeed");
        // Find the `shr` and check operand 1 is rcx.
        for insn in &out {
            if insn.mnemonic == "shr" {
                if let Some(Operand::Register(name)) = insn.operands.get(1) {
                    assert_eq!(name, "rcx", "shift count must be pinned to rcx");
                    return;
                }
            }
        }
        panic!("no shr in output");
    }

    #[test]
    fn rejects_double_declaration() {
        let src = "
handler Test
    temp foo
    temp foo
end
";
        let err = build(src, Arch::X86_64).expect_err("should reject re-declaration");
        assert!(err.message.contains("declared more than once"));
    }

    #[test]
    fn rejects_shadowing_pinned_register() {
        // `pc` is a pinned (callee-saved) DSL name; declaring a temp with
        // that name would silently break dispatch, so the allocator
        // rejects it up front.
        let src = "
handler Test
    temp pc
end
";
        let err = build(src, Arch::X86_64).expect_err("should reject shadowing");
        assert!(err.message.contains("shadows"));
    }

    #[test]
    fn call_helper_with_named_input_and_output_pins_to_t1_and_t0() {
        // Named-call form: input pinned to rcx, output pinned to rax.
        // The input dies at the call (so being in the killed set is OK),
        // and the output is born there (so it survives).
        let src = "
handler Test
    temp value, result
    load_operand value, m_condition
    call_helper asm_helper_to_boolean, value, result
    branch_nonzero result, .take
    dispatch_next
.take:
    load_label value, m_target
    goto_handler value
end
";
        let out = build(src, Arch::X86_64).expect("allocation should succeed");
        let mut saw_call = false;
        for insn in &out {
            if insn.mnemonic == "call_helper" {
                saw_call = true;
                if let Some(Operand::Register(name)) = insn.operands.get(1) {
                    assert_eq!(name, "rcx", "call_helper input must be pinned to rcx");
                }
                if let Some(Operand::Register(name)) = insn.operands.get(2) {
                    assert_eq!(name, "rax", "call_helper output must be pinned to rax");
                }
            }
        }
        assert!(saw_call);
    }

    #[test]
    fn aarch64_call_helper_named_input_and_output_share_x0() {
        // On aarch64, the named-call form can put the dying input and
        // newly-born output in x0, which is both t0 and the ABI argument
        // register. That lets codegen skip the legacy x1 -> x0 bridge.
        let src = "
handler Test
    temp value, result
    load_operand value, m_condition
    call_helper asm_helper_to_boolean, value, result
    branch_nonzero result, .take
    dispatch_next
.take:
    load_label value, m_target
    goto_handler value
end
";
        let out = build(src, Arch::Aarch64).expect("allocation should succeed");
        let mut saw_call = false;
        for insn in &out {
            if insn.mnemonic == "call_helper" {
                saw_call = true;
                if let Some(Operand::Register(name)) = insn.operands.get(1) {
                    assert_eq!(name, "x0", "call_helper input must be pinned to x0");
                }
                if let Some(Operand::Register(name)) = insn.operands.get(2) {
                    assert_eq!(name, "x0", "call_helper output must be pinned to x0");
                }
            }
        }
        assert!(saw_call);
    }

    #[test]
    fn rejects_temp_live_across_call() {
        let src = "
handler Test
    temp value
    mov value, 7
    call_helper asm_helper_to_boolean
    add value, 1
end
";
        let err = build(src, Arch::X86_64)
            .expect_err("should reject value live across call_helper");
        assert!(
            err.message.contains("could not allocate") || err.message.contains("pool exhausted"),
            "unexpected error: {err:?}"
        );
    }

    #[test]
    fn separates_gpr_and_fpr_pools() {
        let src = "
handler Test
    temp i
    ftemp f
    mov i, 1
    int_to_double f, i
    fp_mov i, f
end
";
        let out = build(src, Arch::X86_64).expect("allocation should succeed");
        // `i` should land in a GPR; `f` should land in an FPR.
        let i_reg = assignment_for(&out, "mov", 0);
        assert!(
            ["rax", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11"]
                .iter()
                .any(|p| *p == i_reg)
        );
        for insn in &out {
            if insn.mnemonic == "fp_mov" {
                if let Some(Operand::Register(name)) = insn.operands.get(1) {
                    assert!(
                        ["xmm0", "xmm1", "xmm2", "xmm3"].iter().any(|p| *p == name),
                        "fp source not in xmm pool: {name}"
                    );
                    return;
                }
            }
        }
        panic!("no fp_mov in output");
    }

    #[test]
    fn flattens_macros_and_uniquifies_local_temps() {
        // Two invocations of a macro that each declare `tmp`. The
        // rewritten output must contain neither the bare name `tmp` nor
        // any unresolved `tmp_m*` form: each invocation's `tmp` is its
        // own temp, allocated independently.
        let src = "
macro use_tmp(out_arg)
    temp tmp
    mov tmp, 5
    add out_arg, tmp
end

handler Test
    temp a, b
    mov a, 0
    mov b, 0
    use_tmp a
    use_tmp b
end
";
        let out = build(src, Arch::X86_64).expect("allocation should succeed");
        for insn in &out {
            assert_ne!(insn.mnemonic, "temp");
            for op in &insn.operands {
                if let Operand::Register(name) = op {
                    assert_ne!(name, "tmp", "unresolved macro-local temp leaked: {insn:?}");
                    assert!(
                        !name.starts_with("tmp_m"),
                        "uniquified temp not assigned: {name}"
                    );
                }
            }
        }
    }

    #[test]
    fn xor_self_zeroes_without_a_phantom_use() {
        // The `xor reg, reg` idiom must not look like a self-read; if it
        // did, the allocator would treat the temp as live at the handler
        // entry and reject the program.
        let src = "
handler Test
    temp counter
    xor counter, counter
    add counter, 1
    store_operand m_dst, counter
    dispatch_next
end
";
        let _out = build(src, Arch::X86_64).expect("xor reg, reg should be a pure def");
    }

    #[test]
    fn divmod_pins_outputs_and_keeps_inputs_off_rax_rdx() {
        // x86 idiv reads rax/rdx, so dividend / divisor operands must not
        // be allocated to rax or rdx (the implicit_outputs); the quotient
        // and remainder, however, are pinned to rax/rdx by fixed_operands
        // and that pin must NOT be vetoed by the operand-forbids rule.
        let src = "
handler Test
    temp dividend, divisor, quot, rem, dst
    load_operand dividend, m_lhs
    load_operand divisor, m_rhs
    divmod quot, rem, dividend, divisor
    box_int32 dst, rem
    store_operand m_dst, dst
    dispatch_next
end
";
        let out = build(src, Arch::X86_64).expect("allocation should succeed");
        for insn in &out {
            if insn.mnemonic == "divmod" {
                let names: Vec<&str> = insn
                    .operands
                    .iter()
                    .map(|op| match op {
                        Operand::Register(n) => n.as_str(),
                        _ => "<non-reg>",
                    })
                    .collect();
                assert_eq!(names[0], "rax", "quot must be pinned to rax");
                assert_eq!(names[1], "rdx", "rem must be pinned to rdx");
                assert_ne!(names[2], "rax", "dividend must not be rax");
                assert_ne!(names[2], "rdx", "dividend must not be rdx");
                assert_ne!(names[3], "rax", "divisor must not be rax");
                assert_ne!(names[3], "rdx", "divisor must not be rdx");
                return;
            }
        }
        panic!("no divmod in output");
    }

    #[test]
    fn dst_operand_avoids_codegen_scratch_register() {
        // box_int32_clean uses rax as scratch on x86 while writing the dst
        // operand. If the allocator placed `dst` in rax, the scratch and
        // the dst would collide and the boxed value would be lost.
        // (Real-world repro: PostfixIncrement after migration produced
        // garbage because the dst temp landed in rax.)
        let src = "
handler Test
    temp value, int_value, dst
    load_operand value, m_src
    unbox_int32 int_value, value
    box_int32_clean dst, int_value
    store_operand m_dst, dst
    dispatch_next
end
";
        let out = build(src, Arch::X86_64).expect("allocation should succeed");
        for insn in &out {
            if insn.mnemonic == "box_int32_clean" {
                if let Some(Operand::Register(name)) = insn.operands.first() {
                    assert_ne!(
                        name, "rax",
                        "box_int32_clean dst must avoid rax (the codegen's scratch reg)"
                    );
                }
            }
        }
    }

    #[test]
    fn macro_local_temps_can_share_a_physical_register_across_invocations() {
        // Two non-overlapping invocations of the same macro should be
        // free to put their private `tmp` in the same physical register.
        // This exercises the per-invocation rename + per-temp live range.
        let src = "
macro touch(arg)
    temp tmp
    mov tmp, 1
    add arg, tmp
end

handler Test
    temp out
    mov out, 0
    touch out
    touch out
    store_operand m_dst, out
end
";
        let out = build(src, Arch::X86_64).expect("allocation should succeed");
        // Find the two `mov ?, 1` instructions (the two `tmp` defs) and
        // assert they target the same physical register, demonstrating
        // re-use of the slot once the first invocation's tmp dies.
        let tmp_targets: Vec<&str> = out
            .iter()
            .filter_map(|insn| {
                if insn.mnemonic == "mov" && insn.operands.len() == 2 {
                    if let Operand::Immediate(1) = insn.operands[1] {
                        if let Operand::Register(name) = &insn.operands[0] {
                            return Some(name.as_str());
                        }
                    }
                }
                None
            })
            .collect();
        assert_eq!(tmp_targets.len(), 2, "expected two mov ?, 1 emissions");
        assert_eq!(
            tmp_targets[0], tmp_targets[1],
            "macro-local tmp should re-use the same physical register \
             across non-overlapping invocations"
        );
    }
}
