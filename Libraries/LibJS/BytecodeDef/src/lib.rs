/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Shared Bytecode.def parser and layout computation.
//!
//! Used by both `Libraries/LibJS/Rust/build.rs` (bytecode codegen) and
//! `Libraries/LibJS/AsmIntGen` (assembly interpreter codegen) to ensure a single source
//! of truth for instruction field offsets and sizes.

use std::collections::HashMap;

#[derive(Debug, Clone)]
pub struct Field {
    pub name: String,
    pub ty: String,
    pub is_array: bool,
}

#[derive(Debug, Clone)]
pub struct OpDef {
    pub name: String,
    pub fields: Vec<Field>,
    pub is_terminator: bool,
}

pub struct FieldType {
    pub rust_type: &'static str,
    pub align: usize,
    pub size: usize,
    pub kind: &'static str,
}

impl From<(&'static str, usize, usize, &'static str)> for FieldType {
    fn from(v: (&'static str, usize, usize, &'static str)) -> Self {
        Self {
            rust_type: v.0,
            align: v.1,
            size: v.2,
            kind: v.3,
        }
    }
}

/// The alignment of the C++ Instruction base class (`alignas(void*)`).
/// On 64-bit: alignof(void*) = 8.
pub const STRUCT_ALIGN: usize = 8;

pub fn parse_bytecode_def(content: &str) -> Vec<OpDef> {
    let mut ops = Vec::new();
    let mut current: Option<OpDef> = None;
    let mut in_op = false;

    for raw_line in content.lines() {
        let stripped = raw_line.trim();
        if stripped.is_empty() || stripped.starts_with("//") || stripped.starts_with('#') {
            continue;
        }

        if stripped.starts_with("op ") {
            assert!(!in_op, "Nested op blocks");
            in_op = true;
            let rest = stripped.strip_prefix("op ").unwrap().trim();
            let name = if let Some(idx) = rest.find('<') {
                rest[..idx].trim().to_string()
            } else {
                rest.to_string()
            };
            current = Some(OpDef {
                name,
                fields: Vec::new(),
                is_terminator: false,
            });
            continue;
        }

        if stripped == "endop" {
            assert!(in_op && current.is_some(), "endop without op");
            ops.push(current.take().unwrap());
            in_op = false;
            continue;
        }

        if !in_op {
            continue;
        }

        if stripped.starts_with('@') {
            if stripped == "@terminator" {
                current.as_mut().unwrap().is_terminator = true;
            }
            continue;
        }

        let (lhs, rhs) = stripped.split_once(':').expect("Malformed field line");
        let field_name = lhs.trim().to_string();
        let mut field_type = rhs.trim().to_string();
        let is_array = field_type.ends_with("[]");
        if is_array {
            field_type = field_type[..field_type.len() - 2].trim().to_string();
        }
        current.as_mut().unwrap().fields.push(Field {
            name: field_name,
            ty: field_type,
            is_array,
        });
    }
    assert!(!in_op, "Unclosed op block");

    // Remove the base "Instruction" definition (not an actual opcode).
    ops.retain(|op| op.name != "Instruction");
    ops
}

pub fn field_type_info(ty: &str) -> FieldType {
    match ty {
        "bool" => ("bool", 1, 1, "bool"),
        "u32" => ("u32", 4, 4, "u32"),
        "u64" => ("u64", 8, 8, "u64"),
        "Operand" => ("Operand", 4, 4, "operand"),
        "Optional<Operand>" => ("Option<Operand>", 4, 4, "optional_operand"),
        "Label" => ("Label", 4, 4, "label"),
        "Optional<Label>" => ("Option<Label>", 4, 8, "optional_label"),
        "IdentifierTableIndex" => ("IdentifierTableIndex", 4, 4, "u32_newtype"),
        "Optional<IdentifierTableIndex>" => ("Option<IdentifierTableIndex>", 4, 4, "optional_u32_newtype"),
        "PropertyKeyTableIndex" => ("PropertyKeyTableIndex", 4, 4, "u32_newtype"),
        "StringTableIndex" => ("StringTableIndex", 4, 4, "u32_newtype"),
        "Optional<StringTableIndex>" => ("Option<StringTableIndex>", 4, 4, "optional_u32_newtype"),
        "RegexTableIndex" => ("RegexTableIndex", 4, 4, "u32_newtype"),
        "EnvironmentCoordinate" => ("EnvironmentCoordinate", 4, 8, "env_coord"),
        "Builtin" => ("u8", 1, 1, "u8"),
        "Completion::Type" => ("u32", 4, 4, "u32"),
        "IteratorHint" => ("u32", 4, 4, "u32"),
        "EnvironmentMode" => ("u32", 4, 4, "u32"),
        "PutKind" => ("u32", 4, 4, "u32"),
        "ArgumentsKind" => ("u32", 4, 4, "u32"),
        "Value" => ("u64", 8, 8, "u64"),
        // Cache pointer types: stored as u64, fixup pass replaces indices with pointers.
        "PropertyLookupCache*"
        | "GlobalVariableCache*"
        | "TemplateObjectCache*"
        | "ObjectShapeCache*"
        | "ObjectPropertyIteratorCache*" => ("u64", 8, 8, "u64"),
        _ => unreachable!("Unknown field type: {ty}"),
    }
    .into()
}

pub fn round_up(value: usize, align: usize) -> usize {
    assert!(align.is_power_of_two());
    (value + align - 1) & !(align - 1)
}

/// Returns the user-visible fields (excludes m_type, m_strict, m_length).
pub fn user_fields(op: &OpDef) -> Vec<&Field> {
    op.fields
        .iter()
        .filter(|f| f.name != "m_type" && f.name != "m_strict" && f.name != "m_length")
        .collect()
}

/// Compute the byte offset of the m_length field within the C++ struct.
pub fn find_m_length_offset(fields: &[Field]) -> usize {
    let mut offset: usize = 2; // after m_type + m_strict
    for f in fields {
        if f.is_array {
            continue;
        }
        if f.name == "m_type" || f.name == "m_strict" {
            continue;
        }
        let info = field_type_info(&f.ty);
        offset = round_up(offset, info.align);
        if f.name == "m_length" {
            return offset;
        }
        offset += info.size;
    }
    panic!("m_length field not found");
}

/// Computed layout info for a single opcode.
pub struct OpLayout {
    /// Byte offset of each field within the C++ struct (keyed by field name, e.g. "m_dst").
    pub field_offsets: HashMap<String, usize>,
    /// Total encoded size (for fixed-size instructions), or None for variable-length.
    pub size: Option<usize>,
}

/// Compute field offsets and total sizes for all opcodes.
pub fn compute_layouts(ops: &[OpDef]) -> HashMap<String, OpLayout> {
    let mut result = HashMap::new();

    for op in ops {
        let has_array = op.fields.iter().any(|f| f.is_array);
        let mut field_offsets = HashMap::new();
        let mut offset: usize = 2; // after m_type + m_strict header

        // First pass: fixed (non-array) fields
        for f in &op.fields {
            if f.is_array || f.name == "m_type" || f.name == "m_strict" {
                continue;
            }
            let info = field_type_info(&f.ty);
            offset = round_up(offset, info.align);
            field_offsets.insert(f.name.clone(), offset);
            offset += info.size;
        }

        // Array fields start at sizeof(*this), which is the fixed part rounded up
        if has_array {
            let sizeof_this = round_up(offset, STRUCT_ALIGN);
            for f in &op.fields {
                if !f.is_array {
                    continue;
                }
                field_offsets.insert(f.name.clone(), sizeof_this);
            }
        }

        let size = if has_array {
            None
        } else {
            Some(round_up(offset, STRUCT_ALIGN))
        };

        result.insert(op.name.clone(), OpLayout { field_offsets, size });
    }

    result
}
