/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::parser::{AsmInstruction, Handler, Operand, Program};
use std::collections::HashMap;

/// Like `writeln!`, but without the `.unwrap()` -- writing to a `String` is infallible.
macro_rules! w {
    ($dst:expr) => { { let _ = writeln!($dst); } };
    ($dst:expr, $($arg:tt)*) => { { let _ = writeln!($dst, $($arg)*); } };
}
pub(crate) use w;

/// Replace a macro parameter string with the appropriate operand type.
pub fn substitute_param(replacement: &str) -> Operand {
    if let Ok(val) = replacement.parse::<i64>() {
        Operand::Immediate(val)
    } else if replacement.starts_with('.') {
        Operand::Label(replacement.to_string())
    } else {
        Operand::Register(replacement.to_string())
    }
}

/// Expand a macro body instruction by replacing parameter names with their values.
pub fn substitute_macro(
    insn: &AsmInstruction,
    param_map: &HashMap<String, String>,
) -> AsmInstruction {
    let operands = insn
        .operands
        .iter()
        .map(|op| match op {
            Operand::Register(name) => {
                if let Some(replacement) = param_map.get(name) {
                    substitute_param(replacement)
                } else {
                    op.clone()
                }
            }
            Operand::Constant(name) | Operand::FieldRef(name) => {
                if let Some(replacement) = param_map.get(name) {
                    substitute_param(replacement)
                } else {
                    op.clone()
                }
            }
            _ => op.clone(),
        })
        .collect();

    let mnemonic = if let Some(replacement) = param_map.get(&insn.mnemonic) {
        replacement.clone()
    } else {
        insn.mnemonic.clone()
    };

    AsmInstruction { mnemonic, operands }
}

/// Uniquify local labels within a macro body so the same macro can be expanded
/// multiple times in one handler without label conflicts. Only labels that are
/// both *defined* and *referenced* within the body are renamed -- labels that
/// cross macro boundaries (defined here but referenced elsewhere, or vice
/// versa) are left alone.
pub fn uniquify_macro_labels(body: &[AsmInstruction], suffix: u32) -> Vec<AsmInstruction> {
    use std::collections::HashSet;

    // Collect label definitions and references separately.
    let mut definitions: HashSet<String> = HashSet::new();
    let mut references: HashSet<String> = HashSet::new();
    for insn in body {
        if insn.mnemonic == "label" {
            if let Some(Operand::Label(name)) = insn.operands.first() {
                if name.starts_with('.') {
                    definitions.insert(name.clone());
                }
            }
        } else {
            for op in &insn.operands {
                if let Operand::Label(name) = op {
                    if name.starts_with('.') {
                        references.insert(name.clone());
                    }
                }
            }
        }
    }

    // Only uniquify labels that are self-contained: both defined and referenced
    // within this macro body.
    let rename_map: HashMap<String, String> = definitions
        .intersection(&references)
        .map(|name| (name.clone(), format!("{name}_m{suffix}")))
        .collect();

    if rename_map.is_empty() {
        return body.to_vec();
    }

    body.iter()
        .map(|insn| {
            let operands = insn
                .operands
                .iter()
                .map(|op| {
                    if let Operand::Label(name) = op {
                        if let Some(new_name) = rename_map.get(name) {
                            return Operand::Label(new_name.clone());
                        }
                    }
                    op.clone()
                })
                .collect();
            AsmInstruction {
                mnemonic: insn.mnemonic.clone(),
                operands,
            }
        })
        .collect()
}

/// Resolve a label operand to its full name (with handler prefix for local labels).
pub fn resolve_label(op: &Operand, handler: &Handler) -> String {
    match op {
        Operand::Label(name) => {
            if name.starts_with('.') {
                format!(".Lasm_{}{name}", handler.name)
            } else {
                name.clone()
            }
        }
        _ => panic!("expected label operand"),
    }
}

/// Get the immediate value from an operand, resolving constants.
pub fn get_immediate_value(op: &Operand, program: &Program) -> Option<i64> {
    match op {
        Operand::Immediate(val) => Some(*val),
        Operand::Constant(name) => program.constants.get(name).copied(),
        _ => None,
    }
}

/// Mutable state accumulated during handler code generation.
pub struct HandlerState {
    /// Cold fixup blocks emitted after the main handler body (e.g. NaN canonicalization).
    pub cold_blocks: String,
    /// Counter for generating unique labels within a handler.
    pub unique_counter: u32,
    /// Last FP comparison operands, used to elide redundant ucomisd/fcmp instructions.
    pub last_fp_compare: Option<(String, String)>,
}

impl HandlerState {
    pub fn new() -> Self {
        Self {
            cold_blocks: String::new(),
            unique_counter: 0,
            last_fp_compare: None,
        }
    }
}

/// Resolve a field reference (m_*) to its byte offset within the handler's opcode.
pub fn resolve_field_ref(s: &str, handler: &Handler, program: &Program) -> Option<usize> {
    if !s.starts_with("m_") {
        return None;
    }
    let layout = program.op_layouts.get(&handler.name)?;
    layout.field_offsets.get(s).copied()
}
