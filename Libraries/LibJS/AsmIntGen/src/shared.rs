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

/// Resolve a field reference (m_*) to its byte offset within the handler's opcode.
pub fn resolve_field_ref(s: &str, handler: &Handler, program: &Program) -> Option<usize> {
    if !s.starts_with("m_") {
        return None;
    }
    let layout = program.op_layouts.get(&handler.name)?;
    layout.field_offsets.get(s).copied()
}
