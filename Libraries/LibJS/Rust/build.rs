/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Build script that generates Rust bytecode instruction types from Bytecode.def.
//!
//! This mirrors Meta/generate-libjs-bytecode-def-derived.py but generates Rust
//! code instead of C++. The generated code lives in $OUT_DIR/instruction_generated.rs
//! and is included! from src/bytecode/instruction.rs.

use std::env;
use std::fmt::Write;
use std::fs;
use std::path::PathBuf;

// ---------------------------------------------------------------------------
// .def file parser (mirrors Meta/libjs_bytecode_def.py)
// ---------------------------------------------------------------------------

#[derive(Debug, Clone)]
struct Field {
    name: String,
    ty: String,
    is_array: bool,
}

#[derive(Debug)]
struct OpDef {
    name: String,
    fields: Vec<Field>,
    is_terminator: bool,
}

fn parse_bytecode_def(path: &std::path::Path) -> Vec<OpDef> {
    let content = fs::read_to_string(path).expect("Failed to read Bytecode.def");
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
            // @nothrow is C++-only, ignore
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

// ---------------------------------------------------------------------------
// Type mapping: C++ types → Rust types
// ---------------------------------------------------------------------------

/// Returns (rust_type, c_alignment, c_size, encoding_kind).
fn field_type_info(ty: &str) -> (&'static str, usize, usize, &'static str) {
    match ty {
        "bool" => ("bool", 1, 1, "bool"),
        "u32" => ("u32", 4, 4, "u32"),
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
        "ArgumentsKind" => ("u32", 4, 4, "u32"),
        "Value" => ("u64", 8, 8, "u64"),
        other => panic!("Unknown field type: {other}"),
    }
}

fn rust_field_name(name: &str) -> String {
    // Strip m_ prefix
    if let Some(stripped) = name.strip_prefix("m_") {
        stripped.to_string()
    } else {
        name.to_string()
    }
}

fn round_up(value: usize, align: usize) -> usize {
    (value + align - 1) & !(align - 1)
}

/// The alignment of the C++ Instruction base class (`alignas(void*)`).
/// On 64-bit: alignof(void*) = 8.
const STRUCT_ALIGN: usize = 8;

/// Compute the byte offset of the m_length field within the C++ struct.
fn find_m_length_offset(fields: &[Field]) -> usize {
    let mut offset: usize = 2; // after m_type + m_strict
    for f in fields {
        if f.is_array {
            continue;
        }
        if f.name == "m_type" || f.name == "m_strict" {
            continue;
        }
        let (_, align, size, _) = field_type_info(&f.ty);
        offset = round_up(offset, align);
        if f.name == "m_length" {
            return offset;
        }
        offset += size;
    }
    panic!("m_length field not found");
}

// ---------------------------------------------------------------------------
// Code generation
// ---------------------------------------------------------------------------

fn generate_rust_code(ops: &[OpDef]) -> String {
    let mut out = String::with_capacity(64 * 1024);

    writeln!(out, "// @generated from Libraries/LibJS/Bytecode/Bytecode.def").unwrap();
    writeln!(out, "// Do not edit manually.").unwrap();
    writeln!(out).unwrap();
    writeln!(out, "use super::operand::*;").unwrap();
    writeln!(out).unwrap();

    // --- OpCode enum ---
    generate_opcode_enum(&mut out, ops);

    // --- Instruction enum ---
    generate_instruction_enum(&mut out, ops);

    // --- impl Instruction ---
    generate_instruction_impl(&mut out, ops);

    out
}

fn generate_opcode_enum(out: &mut String, ops: &[OpDef]) {
    writeln!(out, "/// Bytecode opcode (u8), matching the C++ `Instruction::Type` enum.").unwrap();
    writeln!(out, "#[derive(Debug, Clone, Copy, PartialEq, Eq)]").unwrap();
    writeln!(out, "#[repr(u8)]").unwrap();
    writeln!(out, "pub enum OpCode {{").unwrap();
    for (i, op) in ops.iter().enumerate() {
        writeln!(out, "    {} = {},", op.name, i).unwrap();
    }
    writeln!(out, "}}").unwrap();
    writeln!(out).unwrap();
}

fn generate_instruction_enum(out: &mut String, ops: &[OpDef]) {
    writeln!(out, "/// A bytecode instruction with typed fields.").unwrap();
    writeln!(out, "///").unwrap();
    writeln!(out, "/// Each variant corresponds to one C++ instruction class.").unwrap();
    writeln!(out, "/// During codegen, instructions are stored as these typed variants.").unwrap();
    writeln!(out, "/// During flattening, they are serialized to bytes matching C++ layout.").unwrap();
    writeln!(out, "#[derive(Debug, Clone)]").unwrap();
    writeln!(out, "pub enum Instruction {{").unwrap();
    for op in ops {
        let fields = user_fields(op);
        if fields.is_empty() {
            writeln!(out, "    {},", op.name).unwrap();
        } else {
            writeln!(out, "    {} {{", op.name).unwrap();
            for f in &fields {
                let (rust_ty, _, _, _) = field_type_info(&f.ty);
                let rname = rust_field_name(&f.name);
                if f.is_array {
                    writeln!(out, "        {}: Vec<{}>,", rname, rust_ty).unwrap();
                } else {
                    writeln!(out, "        {}: {},", rname, rust_ty).unwrap();
                }
            }
            writeln!(out, "    }},").unwrap();
        }
    }
    writeln!(out, "}}").unwrap();
    writeln!(out).unwrap();
}

/// Returns the user-visible fields (excludes m_type, m_strict, m_length).
fn user_fields(op: &OpDef) -> Vec<&Field> {
    op.fields
        .iter()
        .filter(|f| f.name != "m_type" && f.name != "m_strict" && f.name != "m_length")
        .collect()
}

fn generate_instruction_impl(out: &mut String, ops: &[OpDef]) {
    writeln!(out, "impl Instruction {{").unwrap();

    // opcode()
    generate_opcode_method(out, ops);

    // is_terminator()
    generate_is_terminator_method(out, ops);

    // encode()
    generate_encode_method(out, ops);

    // encoded_size()
    generate_encoded_size_method(out, ops);

    // visit_operands()
    generate_visit_operands_method(out, ops);

    // visit_labels()
    generate_visit_labels_method(out, ops);

    writeln!(out, "}}").unwrap();
}

fn generate_opcode_method(out: &mut String, ops: &[OpDef]) {
    writeln!(out, "    pub fn opcode(&self) -> OpCode {{").unwrap();
    writeln!(out, "        match self {{").unwrap();
    for op in ops {
        let fields = user_fields(op);
        if fields.is_empty() {
            writeln!(out, "            Instruction::{} => OpCode::{},", op.name, op.name).unwrap();
        } else {
            writeln!(out, "            Instruction::{} {{ .. }} => OpCode::{},", op.name, op.name).unwrap();
        }
    }
    writeln!(out, "        }}").unwrap();
    writeln!(out, "    }}").unwrap();
    writeln!(out).unwrap();
}

fn generate_is_terminator_method(out: &mut String, ops: &[OpDef]) {
    writeln!(out, "    pub fn is_terminator(&self) -> bool {{").unwrap();
    writeln!(out, "        matches!(self, ").unwrap();
    let terminators: Vec<&OpDef> = ops.iter().filter(|op| op.is_terminator).collect();
    for (i, op) in terminators.iter().enumerate() {
        let sep = if i + 1 < terminators.len() { " |" } else { "" };
        let fields = user_fields(op);
        if fields.is_empty() {
            writeln!(out, "            Instruction::{}{}", op.name, sep).unwrap();
        } else {
            writeln!(out, "            Instruction::{} {{ .. }}{}", op.name, sep).unwrap();
        }
    }
    writeln!(out, "        )").unwrap();
    writeln!(out, "    }}").unwrap();
    writeln!(out).unwrap();
}

fn generate_encoded_size_method(out: &mut String, ops: &[OpDef]) {
    writeln!(out, "    /// Returns the encoded size of this instruction in bytes.").unwrap();
    writeln!(out, "    pub fn encoded_size(&self) -> usize {{").unwrap();
    writeln!(out, "        match self {{").unwrap();

    for op in ops {
        let fields = user_fields(op);
        let has_array = op.fields.iter().any(|f| f.is_array);

        if !has_array {
            // Fixed-length: compute size statically
            let mut offset: usize = 2; // header
            for f in &op.fields {
                if f.is_array || f.name == "m_type" || f.name == "m_strict" {
                    continue;
                }
                let (_, align, size, _) = field_type_info(&f.ty);
                offset = round_up(offset, align);
                offset += size;
            }
            let final_size = round_up(offset, 8);
            let pat = if fields.is_empty() {
                format!("Instruction::{}", op.name)
            } else {
                format!("Instruction::{} {{ .. }}", op.name)
            };
            writeln!(out, "            {} => {},", pat, final_size).unwrap();
        } else {
            // Variable-length: depends on array size
            // Compute fixed part size
            let mut fixed_offset: usize = 2;
            for f in &op.fields {
                if f.is_array || f.name == "m_type" || f.name == "m_strict" {
                    continue;
                }
                let (_, align, size, _) = field_type_info(&f.ty);
                fixed_offset = round_up(fixed_offset, align);
                fixed_offset += size;
            }

            // Find the array field and its element size
            let array_field = op.fields.iter().find(|f| f.is_array).unwrap();
            let (_, _elem_align, elem_size, _) = field_type_info(&array_field.ty);
            let arr_name = rust_field_name(&array_field.name);
            // C++ computes m_length as:
            //   round_up(alignof(void*), sizeof(*this) + sizeof(elem) * count)
            // sizeof(*this) = round_up(fixed_offset, STRUCT_ALIGN) due to alignas(void*).
            let sizeof_this = round_up(fixed_offset, STRUCT_ALIGN);

            // Bind only the array field
            let bindings: Vec<String> = fields
                .iter()
                .map(|f| {
                    let rname = rust_field_name(&f.name);
                    if rname == arr_name {
                        rname
                    } else {
                        format!("{}: _", rname)
                    }
                })
                .collect();
            writeln!(out, "            Instruction::{} {{ {} }} => {{", op.name, bindings.join(", ")).unwrap();
            writeln!(out, "                let base = {} + {}.len() * {};", sizeof_this, arr_name, elem_size).unwrap();
            writeln!(out, "                (base + 7) & !7 // round up to 8").unwrap();
            writeln!(out, "            }}").unwrap();
        }
    }

    writeln!(out, "        }}").unwrap();
    writeln!(out, "    }}").unwrap();
    writeln!(out).unwrap();
}

fn generate_encode_method(out: &mut String, ops: &[OpDef]) {
    writeln!(out, "    /// Encode this instruction into bytes matching the C++ struct layout.").unwrap();
    writeln!(out, "    pub fn encode(&self, strict: bool, buf: &mut Vec<u8>) {{").unwrap();
    writeln!(out, "        let start = buf.len();").unwrap();
    writeln!(out, "        match self {{").unwrap();

    for op in ops {
        let fields = user_fields(op);
        let has_array = op.fields.iter().any(|f| f.is_array);
        let has_m_length = op.fields.iter().any(|f| f.name == "m_length");

        // Generate match arm with field bindings
        if fields.is_empty() {
            writeln!(out, "            Instruction::{} => {{", op.name).unwrap();
        } else {
            let bindings: Vec<String> = fields
                .iter()
                .map(|f| rust_field_name(&f.name))
                .collect();
            writeln!(out, "            Instruction::{} {{ {} }} => {{", op.name, bindings.join(", ")).unwrap();
        }

        // Write header: opcode (u8) + strict (u8) = 2 bytes
        writeln!(out, "                buf.push(OpCode::{} as u8);", op.name).unwrap();
        writeln!(out, "                buf.push(strict as u8);").unwrap();

        // Track offset for C++ struct layout.
        // We iterate ALL fields (including m_type, m_strict, m_length) for
        // accurate alignment but only emit writes for user fields.
        let mut offset: usize = 2;

        // Iterate all non-array fields in declaration order
        for f in &op.fields {
            if f.is_array {
                continue;
            }
            // m_type and m_strict are already written as the header
            if f.name == "m_type" || f.name == "m_strict" {
                continue;
            }

            let (_, align, size, kind) = field_type_info(&f.ty);

            // Pad to alignment
            let aligned_offset = round_up(offset, align);
            let pad = aligned_offset - offset;
            if pad > 0 {
                writeln!(out, "                buf.extend_from_slice(&[0u8; {}]);", pad).unwrap();
            }
            offset = aligned_offset;

            if f.name == "m_length" {
                // Write placeholder (patched at end for variable-length instructions)
                writeln!(out, "                buf.extend_from_slice(&[0u8; 4]); // m_length placeholder").unwrap();
            } else {
                let rname = rust_field_name(&f.name);
                emit_field_write(out, &rname, kind, false);
            }
            offset += size;
        }

        // Write trailing array elements
        if has_array {
            // sizeof(*this) in C++ = round_up(fixed_offset, STRUCT_ALIGN)
            let sizeof_this = round_up(offset, STRUCT_ALIGN);

            for f in &op.fields {
                if !f.is_array {
                    continue;
                }
                let (_, elem_align, elem_size, elem_kind) = field_type_info(&f.ty);
                let rname = rust_field_name(&f.name);

                // Pad before first element if needed
                let aligned_offset = round_up(offset, elem_align);
                let pad = aligned_offset - offset;
                if pad > 0 {
                    writeln!(out, "                buf.extend_from_slice(&[0u8; {}]);", pad).unwrap();
                }

                writeln!(out, "                for item in {} {{", rname).unwrap();
                emit_field_write(out, "item", elem_kind, true);
                writeln!(out, "                }}").unwrap();

                // Compute target size matching C++:
                //   round_up(STRUCT_ALIGN, sizeof(*this) + count * elem_size)
                writeln!(out, "                let target = ({} + {}.len() * {} + 7) & !7;",
                    sizeof_this, rname, elem_size).unwrap();
                writeln!(out, "                while (buf.len() - start) < target {{ buf.push(0); }}").unwrap();
            }

            if has_m_length {
                // Patch m_length: it's the first u32 field after the header
                let m_length_offset = find_m_length_offset(&op.fields);
                writeln!(out, "                let total_len = (buf.len() - start) as u32;").unwrap();
                writeln!(out, "                buf[start + {}..start + {}].copy_from_slice(&total_len.to_ne_bytes());",
                    m_length_offset, m_length_offset + 4).unwrap();
            }
        } else {
            // Fixed-length: pad statically
            let final_size = round_up(offset, 8);
            let tail_pad = final_size - offset;
            if tail_pad > 0 {
                writeln!(out, "                buf.extend_from_slice(&[0u8; {}]);", tail_pad).unwrap();
            }
        }

        writeln!(out, "            }}").unwrap();
    }

    writeln!(out, "        }}").unwrap();
    writeln!(out, "    }}").unwrap();
    writeln!(out).unwrap();
}

/// Emit code to write a field value into `buf`.
///
/// All bindings from pattern matching and loop iteration are references (`&T`).
/// Rust auto-derefs for method calls, but explicit `*` is needed for casts
/// and direct pushes of Copy types.
fn emit_field_write(out: &mut String, name: &str, kind: &str, is_loop_item: bool) {
    let prefix = if is_loop_item { "                    " } else { "                " };
    match kind {
        "bool" => writeln!(out, "{}buf.push(*{} as u8);", prefix, name).unwrap(),
        "u8" => writeln!(out, "{}buf.push(*{});", prefix, name).unwrap(),
        "u32" => writeln!(out, "{}buf.extend_from_slice(&{}.to_ne_bytes());", prefix, name).unwrap(),
        "u64" => writeln!(out, "{}buf.extend_from_slice(&{}.to_ne_bytes());", prefix, name).unwrap(),
        "operand" => writeln!(out, "{}buf.extend_from_slice(&{}.raw().to_ne_bytes());", prefix, name).unwrap(),
        "optional_operand" => {
            writeln!(out, "{}match {} {{", prefix, name).unwrap();
            writeln!(out, "{}    Some(op) => buf.extend_from_slice(&op.raw().to_ne_bytes()),", prefix).unwrap();
            writeln!(out, "{}    None => buf.extend_from_slice(&Operand::INVALID.to_ne_bytes()),", prefix).unwrap();
            writeln!(out, "{}}}", prefix).unwrap();
        }
        "label" => writeln!(out, "{}buf.extend_from_slice(&{}.0.to_ne_bytes());", prefix, name).unwrap(),
        "optional_label" => {
            // C++ Optional<Label> layout: u32 value, bool has_value, 3 bytes padding = 8 bytes total
            writeln!(out, "{}match {} {{", prefix, name).unwrap();
            writeln!(out, "{}    Some(lbl) => {{", prefix).unwrap();
            writeln!(out, "{}        buf.extend_from_slice(&lbl.0.to_ne_bytes());", prefix).unwrap();
            writeln!(out, "{}        buf.push(1); buf.push(0); buf.push(0); buf.push(0);", prefix).unwrap();
            writeln!(out, "{}    }}", prefix).unwrap();
            writeln!(out, "{}    None => {{", prefix).unwrap();
            writeln!(out, "{}        buf.extend_from_slice(&0u32.to_ne_bytes());", prefix).unwrap();
            writeln!(out, "{}        buf.push(0); buf.push(0); buf.push(0); buf.push(0);", prefix).unwrap();
            writeln!(out, "{}    }}", prefix).unwrap();
            writeln!(out, "{}}}", prefix).unwrap();
        }
        "u32_newtype" => writeln!(out, "{}buf.extend_from_slice(&{}.0.to_ne_bytes());", prefix, name).unwrap(),
        "optional_u32_newtype" => {
            writeln!(out, "{}match {} {{", prefix, name).unwrap();
            writeln!(out, "{}    Some(idx) => buf.extend_from_slice(&idx.0.to_ne_bytes()),", prefix).unwrap();
            writeln!(out, "{}    None => buf.extend_from_slice(&0xFFFF_FFFFu32.to_ne_bytes()),", prefix).unwrap();
            writeln!(out, "{}}}", prefix).unwrap();
        }
        "env_coord" => {
            writeln!(out, "{}buf.extend_from_slice(&{}.hops.to_ne_bytes());", prefix, name).unwrap();
            writeln!(out, "{}buf.extend_from_slice(&{}.index.to_ne_bytes());", prefix, name).unwrap();
        }
        other => panic!("Unknown encoding kind: {other}"),
    }
}

fn generate_visit_operands_method(out: &mut String, ops: &[OpDef]) {
    writeln!(out, "    /// Visit all `Operand` fields (for operand rewriting).").unwrap();
    writeln!(out, "    pub fn visit_operands(&mut self, visitor: &mut dyn FnMut(&mut Operand)) {{").unwrap();
    writeln!(out, "        match self {{").unwrap();

    for op in ops {
        let fields = user_fields(op);
        let operand_fields: Vec<&&Field> = fields
            .iter()
            .filter(|f| f.ty == "Operand" || f.ty == "Optional<Operand>")
            .collect();

        if operand_fields.is_empty() {
            let pat = if fields.is_empty() {
                format!("Instruction::{}", op.name)
            } else {
                format!("Instruction::{} {{ .. }}", op.name)
            };
            writeln!(out, "            {} => {{}}", pat).unwrap();
            continue;
        }

        // Bind the operand fields
        let bindings: Vec<String> = fields
            .iter()
            .map(|f| {
                let rname = rust_field_name(&f.name);
                if f.ty == "Operand" || f.ty == "Optional<Operand>" {
                    rname
                } else {
                    format!("{}: _", rname)
                }
            })
            .collect();
        writeln!(out, "            Instruction::{} {{ {} }} => {{", op.name, bindings.join(", ")).unwrap();

        for f in &operand_fields {
            let rname = rust_field_name(&f.name);
            if f.is_array {
                if f.ty == "Optional<Operand>" {
                    writeln!(out, "                for op in {}.iter_mut().flatten() {{ visitor(op); }}", rname).unwrap();
                } else {
                    writeln!(out, "                for item in {}.iter_mut() {{ visitor(item); }}", rname).unwrap();
                }
            } else if f.ty == "Optional<Operand>" {
                writeln!(out, "                if let Some(op) = {} {{ visitor(op); }}", rname).unwrap();
            } else {
                writeln!(out, "                visitor({});", rname).unwrap();
            }
        }

        writeln!(out, "            }}").unwrap();
    }

    writeln!(out, "        }}").unwrap();
    writeln!(out, "    }}").unwrap();
    writeln!(out).unwrap();
}

fn generate_visit_labels_method(out: &mut String, ops: &[OpDef]) {
    writeln!(out, "    /// Visit all `Label` fields (for label linking).").unwrap();
    writeln!(out, "    pub fn visit_labels(&mut self, visitor: &mut dyn FnMut(&mut Label)) {{").unwrap();
    writeln!(out, "        match self {{").unwrap();

    for op in ops {
        let fields = user_fields(op);
        let label_fields: Vec<&&Field> = fields
            .iter()
            .filter(|f| f.ty == "Label" || f.ty == "Optional<Label>")
            .collect();

        if label_fields.is_empty() {
            let pat = if fields.is_empty() {
                format!("Instruction::{}", op.name)
            } else {
                format!("Instruction::{} {{ .. }}", op.name)
            };
            writeln!(out, "            {} => {{}}", pat).unwrap();
            continue;
        }

        let bindings: Vec<String> = fields
            .iter()
            .map(|f| {
                let rname = rust_field_name(&f.name);
                if f.ty == "Label" || f.ty == "Optional<Label>" {
                    rname
                } else {
                    format!("{}: _", rname)
                }
            })
            .collect();
        writeln!(out, "            Instruction::{} {{ {} }} => {{", op.name, bindings.join(", ")).unwrap();

        for f in &label_fields {
            let rname = rust_field_name(&f.name);
            if f.is_array {
                if f.ty == "Optional<Label>" {
                    writeln!(out, "                for item in {}.iter_mut() {{", rname).unwrap();
                    writeln!(out, "                    if let Some(lbl) = item {{ visitor(lbl); }}").unwrap();
                    writeln!(out, "                }}").unwrap();
                } else {
                    writeln!(out, "                for item in {}.iter_mut() {{ visitor(item); }}", rname).unwrap();
                }
            } else if f.ty == "Optional<Label>" {
                writeln!(out, "                if let Some(lbl) = {} {{ visitor(lbl); }}", rname).unwrap();
            } else {
                writeln!(out, "                visitor({});", rname).unwrap();
            }
        }

        writeln!(out, "            }}").unwrap();
    }

    writeln!(out, "        }}").unwrap();
    writeln!(out, "    }}").unwrap();
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

fn main() {
    let manifest_dir = PathBuf::from(env::var("CARGO_MANIFEST_DIR").unwrap());
    let def_path = manifest_dir.join("../Bytecode/Bytecode.def");

    println!("cargo:rerun-if-changed={}", def_path.display());
    println!("cargo:rerun-if-changed=build.rs");

    let ops = parse_bytecode_def(&def_path);
    let code = generate_rust_code(&ops);

    let out_dir = PathBuf::from(env::var("OUT_DIR").unwrap());
    fs::write(out_dir.join("instruction_generated.rs"), &code).unwrap();
}
