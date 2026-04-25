/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::parser::{AsmInstruction, Handler, Operand, Program};
use crate::registers::{Arch, resolve_register};
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
    let substitute_name =
        |name: &String| param_map.get(name).cloned().unwrap_or_else(|| name.clone());

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
            Operand::Memory { base, index, scale } => Operand::Memory {
                base: substitute_name(base),
                index: index.as_ref().map(substitute_name),
                scale: scale.as_ref().map(substitute_name),
            },
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
        other => panic!(
            "expected label operand in handler '{}', got {other:?}",
            handler.name
        ),
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

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum ResolvedMemoryIndex {
    None,
    Imm(i64),
    Reg(String),
    RegScale(String, i64),
    RegImm(String, i64),
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ResolvedMemoryOperand {
    pub base: String,
    pub index: ResolvedMemoryIndex,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct AdjacentMemoryPair {
    pub base: String,
    pub index: Option<String>,
    pub first_offset: i64,
}

/// Resolve a field reference (m_*) to its byte offset within the handler's opcode.
pub fn resolve_field_ref(s: &str, handler: &Handler, program: &Program) -> Option<usize> {
    if !s.starts_with("m_") {
        return None;
    }
    let layout = program.op_layouts.get(&handler.name)?;
    layout.field_offsets.get(s).copied()
}

fn resolve_memory_immediate(s: &str, handler: &Handler, program: &Program) -> Option<i64> {
    resolve_field_ref(s, handler, program)
        .map(|offset| offset as i64)
        .or_else(|| program.constants.get(s).copied())
        .or_else(|| s.parse::<i64>().ok())
}

pub fn resolve_memory_operand(
    op: &Operand,
    handler: &Handler,
    program: &Program,
    arch: Arch,
) -> Result<ResolvedMemoryOperand, String> {
    let Operand::Memory { base, index, scale } = op else {
        return Err(format!("expected memory operand, got {op:?}"));
    };

    let base = resolve_register(base, arch).unwrap_or_else(|| base.clone());
    let index = match (index, scale) {
        (Some(index), Some(scale)) => {
            let index = resolve_register(index, arch).unwrap_or_else(|| index.clone());
            if let Some(offset) = resolve_memory_immediate(scale, handler, program) {
                ResolvedMemoryIndex::RegImm(index, offset)
            } else {
                let scale = program
                    .constants
                    .get(scale.as_str())
                    .copied()
                    .or_else(|| scale.parse::<i64>().ok())
                    .ok_or_else(|| format!("invalid memory scale '{scale}'"))?;
                ResolvedMemoryIndex::RegScale(index, scale)
            }
        }
        (Some(index), None) => {
            if let Some(offset) = resolve_memory_immediate(index, handler, program) {
                ResolvedMemoryIndex::Imm(offset)
            } else {
                let index = resolve_register(index, arch).unwrap_or_else(|| index.clone());
                ResolvedMemoryIndex::Reg(index)
            }
        }
        (None, _) => ResolvedMemoryIndex::None,
    };

    Ok(ResolvedMemoryOperand { base, index })
}

fn pairable_memory_address(
    mem: &ResolvedMemoryOperand,
) -> Result<(String, Option<String>, i64), String> {
    match &mem.index {
        ResolvedMemoryIndex::None => Ok((mem.base.clone(), None, 0)),
        ResolvedMemoryIndex::Imm(offset) => Ok((mem.base.clone(), None, *offset)),
        ResolvedMemoryIndex::RegImm(index, offset) => {
            Ok((mem.base.clone(), Some(index.clone()), *offset))
        }
        ResolvedMemoryIndex::Reg(index) => Err(format!(
            "paired accesses require explicit offsets, got indexed address with base '{}' and index '{}'",
            mem.base, index
        )),
        ResolvedMemoryIndex::RegScale(index, scale) => Err(format!(
            "paired accesses require explicit offsets, got scaled address with base '{}', index '{}', and scale {}",
            mem.base, index, scale
        )),
    }
}

pub fn resolve_adjacent_memory_pair(
    first: &Operand,
    second: &Operand,
    handler: &Handler,
    program: &Program,
    arch: Arch,
    element_size: i64,
) -> Result<AdjacentMemoryPair, String> {
    let first = resolve_memory_operand(first, handler, program, arch)?;
    let second = resolve_memory_operand(second, handler, program, arch)?;

    let (first_base, first_index, first_offset) = pairable_memory_address(&first)?;
    let (second_base, second_index, second_offset) = pairable_memory_address(&second)?;

    if first_base != second_base || first_index != second_index {
        return Err(format!(
            "paired accesses must use the same base and index, got {first:?} and {second:?}"
        ));
    }

    if second_offset != first_offset + element_size {
        return Err(format!(
            "paired accesses must name adjacent {element_size}-byte fields in order, got offsets {first_offset} and {second_offset}"
        ));
    }

    Ok(AdjacentMemoryPair {
        base: first_base,
        index: first_index,
        first_offset,
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::parser::{Handler, ObjectFormat, Program};
    use bytecode_def::OpLayout;

    fn test_program() -> Program {
        let mut op_layouts = HashMap::new();
        op_layouts.insert(
            "Call".into(),
            OpLayout {
                field_offsets: HashMap::from([
                    ("m_length".into(), 4),
                    ("m_dst".into(), 8),
                    ("m_callee".into(), 12),
                    ("m_this_value".into(), 16),
                    ("m_argument_count".into(), 20),
                ]),
                size: None,
            },
        );

        Program {
            constants: HashMap::new(),
            macros: HashMap::new(),
            handlers: Vec::new(),
            op_layouts,
            opcode_list: Vec::new(),
            object_format: ObjectFormat::MachO,
            has_jscvt: false,
        }
    }

    fn call_handler() -> Handler {
        Handler {
            name: "Call".into(),
            size: None,
            instructions: Vec::new(),
        }
    }

    #[test]
    fn resolves_adjacent_bytecode_field_pair() {
        let program = test_program();
        let handler = call_handler();
        let first = Operand::Memory {
            base: "pb".into(),
            index: Some("pc".into()),
            scale: Some("m_length".into()),
        };
        let second = Operand::Memory {
            base: "pb".into(),
            index: Some("pc".into()),
            scale: Some("m_dst".into()),
        };

        let pair =
            resolve_adjacent_memory_pair(&first, &second, &handler, &program, Arch::Aarch64, 4)
                .expect("adjacent bytecode fields should validate");

        assert_eq!(
            pair,
            AdjacentMemoryPair {
                base: "x26".into(),
                index: Some("x25".into()),
                first_offset: 4,
            }
        );
    }

    #[test]
    fn rejects_reversed_field_order() {
        let program = test_program();
        let handler = call_handler();
        let first = Operand::Memory {
            base: "pb".into(),
            index: Some("pc".into()),
            scale: Some("m_dst".into()),
        };
        let second = Operand::Memory {
            base: "pb".into(),
            index: Some("pc".into()),
            scale: Some("m_length".into()),
        };

        let error =
            resolve_adjacent_memory_pair(&first, &second, &handler, &program, Arch::Aarch64, 4)
                .expect_err("reversed field order should be rejected");

        assert!(error.contains("adjacent 4-byte fields"));
    }

    #[test]
    fn rejects_non_adjacent_offsets() {
        let program = test_program();
        let handler = call_handler();
        let first = Operand::Memory {
            base: "t0".into(),
            index: Some("0".into()),
            scale: None,
        };
        let second = Operand::Memory {
            base: "t0".into(),
            index: Some("16".into()),
            scale: None,
        };

        let error =
            resolve_adjacent_memory_pair(&first, &second, &handler, &program, Arch::X86_64, 8)
                .expect_err("non-adjacent offsets should be rejected");

        assert!(error.contains("adjacent 8-byte fields"));
    }
}
