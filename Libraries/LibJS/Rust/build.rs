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
use std::fs;
use std::io::Write;
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

struct FieldType {
    r_type: &'static str,
    align: usize,
    size: usize,
    kind: &'static str,
}

impl From<(&'static str, usize, usize, &'static str)> for FieldType {
    fn from(v: (&'static str, usize, usize, &'static str)) -> Self {
        Self {
            r_type: v.0,
            align: v.1,
            size: v.2,
            kind: v.3,
        }
    }
}

fn field_type_info(ty: &str) -> FieldType {
    match ty {
        "bool" => ("bool", 1, 1, "bool"),
        "u32" => ("u32", 4, 4, "u32"),
        "Operand" => ("Operand", 4, 4, "operand"),
        "Optional<Operand>" => ("Option<Operand>", 4, 4, "optional_operand"),
        "Label" => ("Label", 4, 4, "label"),
        "Optional<Label>" => ("Option<Label>", 4, 8, "optional_label"),
        "IdentifierTableIndex" => ("IdentifierTableIndex", 4, 4, "u32_newtype"),
        "Optional<IdentifierTableIndex>" => {
            ("Option<IdentifierTableIndex>", 4, 4, "optional_u32_newtype")
        }
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
        _ => unreachable!("Unknown field type: {ty}"),
    }
    .into()
}

fn rust_field_name(name: &str) -> String {
    if let Some(stripped) = name.strip_prefix("m_") {
        stripped.to_string()
    } else {
        name.to_string()
    }
}

fn round_up(value: usize, align: usize) -> usize {
    assert!(align.is_power_of_two());
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
        let FieldType { align, size, .. } = field_type_info(&f.ty);
        offset = round_up(offset, align);
        if f.name == "m_length" {
            return offset;
        }
        offset += size;
    }
    panic!("m_length field not found");
}

fn generate_rust_code(mut w: impl Write, ops: &[OpDef]) -> Result<(), Box<dyn std::error::Error>> {
    writeln!(
        w,
        "// @generated from Libraries/LibJS/Bytecode/Bytecode.def"
    )?;
    writeln!(w, "// Do not edit manually.")?;
    writeln!(w)?;
    writeln!(w, "use super::operand::*;")?;
    writeln!(w)?;

    generate_opcode_enum(&mut w, ops)?;
    generate_instruction_enum(&mut w, ops)?;
    generate_instruction_impl(&mut w, ops)?;

    Ok(())
}

fn generate_opcode_enum(
    mut w: impl Write,
    ops: &[OpDef],
) -> Result<(), Box<dyn std::error::Error>> {
    writeln!(
        w,
        "/// Bytecode opcode (u8), matching the C++ `Instruction::Type` enum."
    )?;
    writeln!(w, "#[derive(Debug, Clone, Copy, PartialEq, Eq)]")?;
    writeln!(w, "#[repr(u8)]")?;
    writeln!(w, "pub enum OpCode {{")?;
    for (i, op) in ops.iter().enumerate() {
        writeln!(w, "    {} = {},", op.name, i)?;
    }
    writeln!(w, "}}")?;
    writeln!(w)?;

    Ok(())
}

fn generate_instruction_enum(
    mut w: impl Write,
    ops: &[OpDef],
) -> Result<(), Box<dyn std::error::Error>> {
    writeln!(w, "/// A bytecode instruction with typed fields.")?;
    writeln!(w, "///")?;
    writeln!(
        w,
        "/// Each variant corresponds to one C++ instruction class."
    )?;
    writeln!(
        w,
        "/// During codegen, instructions are stored as these typed variants."
    )?;
    writeln!(
        w,
        "/// During flattening, they are serialized to bytes matching C++ layw."
    )?;
    writeln!(w, "#[derive(Debug, Clone)]")?;
    writeln!(w, "pub enum Instruction {{")?;
    for op in ops {
        let fields = user_fields(op);
        if fields.is_empty() {
            writeln!(w, "    {},", op.name)?;
        } else {
            writeln!(w, "    {} {{", op.name)?;
            for f in &fields {
                let FieldType { r_type, .. } = field_type_info(&f.ty);
                let r_name = rust_field_name(&f.name);
                if f.is_array {
                    writeln!(w, "        {}: Vec<{}>,", r_name, r_type)?;
                } else {
                    writeln!(w, "        {}: {},", r_name, r_type)?;
                }
            }
            writeln!(w, "    }},")?;
        }
    }
    writeln!(w, "}}")?;
    writeln!(w)?;

    Ok(())
}

/// Returns the user-visible fields (excludes m_type, m_strict, m_length).
fn user_fields(op: &OpDef) -> Vec<&Field> {
    op.fields
        .iter()
        .filter(|f| f.name != "m_type" && f.name != "m_strict" && f.name != "m_length")
        .collect()
}

fn generate_instruction_impl(
    mut w: impl Write,
    ops: &[OpDef],
) -> Result<(), Box<dyn std::error::Error>> {
    writeln!(w, "impl Instruction {{").unwrap();
    generate_opcode_method(&mut w, ops)?;
    generate_is_terminator_method(&mut w, ops)?;
    generate_encode_method(&mut w, ops)?;
    generate_encoded_size_method(&mut w, ops)?;
    generate_visit_operands_method(&mut w, ops)?;
    generate_visit_labels_method(&mut w, ops)?;
    writeln!(w, "    }}")?;
    Ok(())
}

fn generate_opcode_method(
    mut w: impl Write,
    ops: &[OpDef],
) -> Result<(), Box<dyn std::error::Error>> {
    writeln!(w, "    pub fn opcode(&self) -> OpCode {{")?;
    writeln!(w, "        match self {{")?;
    for op in ops {
        let fields = user_fields(op);
        if fields.is_empty() {
            writeln!(
                w,
                "            Instruction::{} => OpCode::{},",
                op.name, op.name
            )?;
        } else {
            writeln!(
                w,
                "            Instruction::{} {{ .. }} => OpCode::{},",
                op.name, op.name
            )?;
        }
    }
    writeln!(w, "        }}")?;
    writeln!(w, "    }}")?;
    writeln!(w)?;

    Ok(())
}

fn generate_is_terminator_method(
    mut w: impl Write,
    ops: &[OpDef],
) -> Result<(), Box<dyn std::error::Error>> {
    writeln!(w, "    pub fn is_terminator(&self) -> bool {{")?;
    writeln!(w, "        matches!(self, ")?;
    let terminators: Vec<&OpDef> = ops.iter().filter(|op| op.is_terminator).collect();
    for (i, op) in terminators.iter().enumerate() {
        let sep = if i + 1 < terminators.len() { " |" } else { "" };
        let fields = user_fields(op);
        if fields.is_empty() {
            writeln!(w, "            Instruction::{}{}", op.name, sep)?;
        } else {
            writeln!(w, "            Instruction::{} {{ .. }}{}", op.name, sep)?;
        }
    }
    writeln!(w, "        )")?;
    writeln!(w, "    }}")?;
    writeln!(w)?;

    Ok(())
}

fn generate_encoded_size_method(
    mut w: impl Write,
    ops: &[OpDef],
) -> Result<(), Box<dyn std::error::Error>> {
    writeln!(
        w,
        "    /// Returns the encoded size of this instruction in bytes."
    )?;
    writeln!(w, "    pub fn encoded_size(&self) -> usize {{")?;
    writeln!(w, "        match self {{")?;

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
                let FieldType { align, size, .. } = field_type_info(&f.ty);
                offset = round_up(offset, align);
                offset += size;
            }
            let final_size = round_up(offset, 8);
            let pat = if fields.is_empty() {
                format!("Instruction::{}", op.name)
            } else {
                format!("Instruction::{} {{ .. }}", op.name)
            };
            writeln!(w, "            {} => {},", pat, final_size)?;
        } else {
            // Variable-length: depends on array size
            // Compute fixed part size
            let mut fixed_offset: usize = 2;
            for f in &op.fields {
                if f.is_array || f.name == "m_type" || f.name == "m_strict" {
                    continue;
                }
                let FieldType { align, size, .. } = field_type_info(&f.ty);
                fixed_offset = round_up(fixed_offset, align);
                fixed_offset += size;
            }

            // Find the array field and its element size
            let Some(array_field) = op.fields.iter().find(|f| f.is_array) else {
                continue;
            };
            let FieldType { size, .. } = field_type_info(&array_field.ty);
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
            writeln!(
                w,
                "            Instruction::{} {{ {} }} => {{",
                op.name,
                bindings.join(", ")
            )?;
            writeln!(
                w,
                "                let base = {} + {}.len() * {};",
                sizeof_this, arr_name, size
            )?;
            writeln!(w, "                (base + 7) & !7 // round up to 8")?;
            writeln!(w, "            }}")?;
        }
    }

    writeln!(w, "        }}")?;
    writeln!(w, "    }}")?;
    writeln!(w)?;

    Ok(())
}

fn generate_encode_method(
    mut w: impl Write,
    ops: &[OpDef],
) -> Result<(), Box<dyn std::error::Error>> {
    writeln!(
        w,
        "    /// Encode this instruction into bytes matching the C++ struct layout."
    )?;
    writeln!(
        w,
        "    pub fn encode(&self, strict: bool, buf: &mut Vec<u8>) {{"
    )?;
    writeln!(w, "        let start = buf.len();")?;
    writeln!(w, "        match self {{")?;

    for op in ops {
        let fields = user_fields(op);
        let has_array = op.fields.iter().any(|f| f.is_array);
        let has_m_length = op.fields.iter().any(|f| f.name == "m_length");

        // Generate match arm with field bindings
        if fields.is_empty() {
            writeln!(w, "            Instruction::{} => {{", op.name)?;
        } else {
            let bindings: Vec<String> = fields.iter().map(|f| rust_field_name(&f.name)).collect();
            writeln!(
                w,
                "            Instruction::{} {{ {} }} => {{",
                op.name,
                bindings.join(", ")
            )?;
        }

        // Write header: opcode (u8) + strict (u8) = 2 bytes
        writeln!(w, "                buf.push(OpCode::{} as u8);", op.name)?;
        writeln!(w, "                buf.push(strict as u8);")?;

        // Track offset for C++ struct layw.
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

            let FieldType {
                align, size, kind, ..
            } = field_type_info(&f.ty);

            // Pad to alignment
            let aligned_offset = round_up(offset, align);
            let pad = aligned_offset - offset;
            if pad > 0 {
                writeln!(w, "                buf.extend_from_slice(&[0u8; {}]);", pad)?;
            }
            offset = aligned_offset;

            if f.name == "m_length" {
                // Write placeholder (patched at end for variable-length instructions)
                writeln!(
                    w,
                    "                buf.extend_from_slice(&[0u8; 4]); // m_length placeholder"
                )?;
            } else {
                let rname = rust_field_name(&f.name);
                emit_field_write(&mut w, &rname, kind, false)?;
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
                let FieldType {
                    align, size, kind, ..
                } = field_type_info(&f.ty);
                let rname = rust_field_name(&f.name);

                // Pad before first element if needed
                let aligned_offset = round_up(offset, align);
                let pad = aligned_offset - offset;
                if pad > 0 {
                    writeln!(w, "                buf.extend_from_slice(&[0u8; {}]);", pad)?;
                }

                writeln!(w, "                for item in {} {{", rname)?;
                emit_field_write(&mut w, "item", kind, true)?;
                writeln!(w, "                }}")?;

                // Compute target size matching C++:
                //   round_up(STRUCT_ALIGN, sizeof(*this) + count * elem_size)
                writeln!(
                    w,
                    "                let target = ({} + {}.len() * {} + 7) & !7;",
                    sizeof_this, rname, size
                )?;
                writeln!(
                    w,
                    "                while (buf.len() - start) < target {{ buf.push(0); }}"
                )?;
            }

            if has_m_length {
                // Patch m_length: it's the first u32 field after the header
                let m_length_offset = find_m_length_offset(&op.fields);
                writeln!(
                    w,
                    "                let total_len = (buf.len() - start) as u32;"
                )?;
                writeln!(w, "                buf[start + {}..start + {}].copy_from_slice(&total_len.to_ne_bytes());",
                    m_length_offset, m_length_offset + 4)?;
            }
        } else {
            // Fixed-length: pad statically
            let final_size = round_up(offset, 8);
            let tail_pad = final_size - offset;
            if tail_pad > 0 {
                writeln!(
                    w,
                    "                buf.extend_from_slice(&[0u8; {}]);",
                    tail_pad
                )?;
            }
        }

        writeln!(w, "            }}")?;
    }

    writeln!(w, "        }}")?;
    writeln!(w, "    }}")?;
    writeln!(w)?;
    Ok(())
}

/// Emit code to write a field value into `w`.
///
/// All bindings from pattern matching and loop iteration are references (`&T`).
/// Rust auto-derefs for method calls, but explicit `*` is needed for casts
/// and direct pushes of Copy types.
fn emit_field_write(
    mut w: impl Write,
    name: &str,
    kind: &str,
    is_loop_item: bool,
) -> Result<(), Box<dyn std::error::Error>> {
    let prefix = " ".repeat(if is_loop_item { 20 } else { 16 });
    match kind {
        "bool" => writeln!(w, "{}buf.push(*{} as u8);", prefix, name)?,
        "u8" => writeln!(w, "{}buf.push(*{});", prefix, name)?,
        "u32" => writeln!(
            w,
            "{}buf.extend_from_slice(&{}.to_ne_bytes());",
            prefix, name
        )?,
        "u64" => writeln!(
            w,
            "{}buf.extend_from_slice(&{}.to_ne_bytes());",
            prefix, name
        )?,
        "operand" => writeln!(
            w,
            "{}buf.extend_from_slice(&{}.raw().to_ne_bytes());",
            prefix, name
        )?,
        "optional_operand" => {
            writeln!(w, "{}match {} {{", prefix, name)?;
            writeln!(
                w,
                "{}    Some(op) => buf.extend_from_slice(&op.raw().to_ne_bytes()),",
                prefix
            )?;
            writeln!(
                w,
                "{}    None => buf.extend_from_slice(&Operand::INVALID.to_ne_bytes()),",
                prefix
            )?;
            writeln!(w, "{}}}", prefix)?;
        }
        "label" => writeln!(
            w,
            "{}buf.extend_from_slice(&{}.0.to_ne_bytes());",
            prefix, name
        )?,
        "optional_label" => {
            // C++ Optional<Label> layw: u32 value, bool has_value, 3 bytes padding = 8 bytes total
            writeln!(w, "{}match {} {{", prefix, name)?;
            writeln!(w, "{}    Some(lbl) => {{", prefix)?;
            writeln!(
                w,
                "{}        buf.extend_from_slice(&lbl.0.to_ne_bytes());",
                prefix
            )?;
            writeln!(
                w,
                "{}        buf.push(1); buf.push(0); buf.push(0); buf.push(0);",
                prefix
            )?;
            writeln!(w, "{}    }}", prefix)?;
            writeln!(w, "{}    None => {{", prefix)?;
            writeln!(
                w,
                "{}        buf.extend_from_slice(&0u32.to_ne_bytes());",
                prefix
            )?;
            writeln!(
                w,
                "{}        buf.push(0); buf.push(0); buf.push(0); buf.push(0);",
                prefix
            )?;
            writeln!(w, "{}    }}", prefix)?;
            writeln!(w, "{}}}", prefix)?;
        }
        "u32_newtype" => writeln!(
            w,
            "{}buf.extend_from_slice(&{}.0.to_ne_bytes());",
            prefix, name
        )?,
        "optional_u32_newtype" => {
            writeln!(w, "{}match {} {{", prefix, name)?;
            writeln!(
                w,
                "{}    Some(idx) => buf.extend_from_slice(&idx.0.to_ne_bytes()),",
                prefix
            )?;
            writeln!(
                w,
                "{}    None => buf.extend_from_slice(&0xFFFF_FFFFu32.to_ne_bytes()),",
                prefix
            )?;
            writeln!(w, "{}}}", prefix)?;
        }
        "env_coord" => {
            writeln!(
                w,
                "{}buf.extend_from_slice(&{}.hops.to_ne_bytes());",
                prefix, name
            )?;
            writeln!(
                w,
                "{}buf.extend_from_slice(&{}.index.to_ne_bytes());",
                prefix, name
            )?;
        }
        other => panic!("Unknown encoding kind: {other}"),
    }

    Ok(())
}

fn generate_visit_operands_method(
    mut w: impl Write,
    ops: &[OpDef],
) -> Result<(), Box<dyn std::error::Error>> {
    writeln!(
        w,
        "    /// Visit all `Operand` fields (for operand rewriting)."
    )?;
    writeln!(
        w,
        "    pub fn visit_operands(&mut self, visitor: &mut dyn FnMut(&mut Operand)) {{"
    )?;
    writeln!(w, "        match self {{")?;

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
            writeln!(w, "            {} => {{}}", pat)?;
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
        writeln!(
            w,
            "            Instruction::{} {{ {} }} => {{",
            op.name,
            bindings.join(", ")
        )?;

        for f in &operand_fields {
            let rname = rust_field_name(&f.name);
            if f.is_array {
                if f.ty == "Optional<Operand>" {
                    writeln!(
                        w,
                        "                for op in {}.iter_mut().flatten() {{ visitor(op); }}",
                        rname
                    )?;
                } else {
                    writeln!(
                        w,
                        "                for item in {}.iter_mut() {{ visitor(item); }}",
                        rname
                    )?;
                }
            } else if f.ty == "Optional<Operand>" {
                writeln!(
                    w,
                    "                if let Some(op) = {} {{ visitor(op); }}",
                    rname
                )?;
            } else {
                writeln!(w, "                visitor({});", rname)?;
            }
        }

        writeln!(w, "            }}")?;
    }

    writeln!(w, "        }}")?;
    writeln!(w, "    }}")?;
    writeln!(w)?;

    Ok(())
}

fn generate_visit_labels_method(
    mut w: impl Write,
    ops: &[OpDef],
) -> Result<(), Box<dyn std::error::Error>> {
    writeln!(w, "    /// Visit all `Label` fields (for label linking).")?;
    writeln!(
        w,
        "    pub fn visit_labels(&mut self, visitor: &mut dyn FnMut(&mut Label)) {{"
    )?;
    writeln!(w, "        match self {{")?;

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
            writeln!(w, "            {} => {{}}", pat)?;
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
        writeln!(
            w,
            "            Instruction::{} {{ {} }} => {{",
            op.name,
            bindings.join(", ")
        )?;

        for f in &label_fields {
            let rname = rust_field_name(&f.name);
            if f.is_array {
                if f.ty == "Optional<Label>" {
                    writeln!(w, "                for item in {}.iter_mut() {{", rname)?;
                    writeln!(
                        w,
                        "                    if let Some(lbl) = item {{ visitor(lbl); }}"
                    )?;
                    writeln!(w, "                }}")?;
                } else {
                    writeln!(
                        w,
                        "                for item in {}.iter_mut() {{ visitor(item); }}",
                        rname
                    )?;
                }
            } else if f.ty == "Optional<Label>" {
                writeln!(
                    w,
                    "                if let Some(lbl) = {} {{ visitor(lbl); }}",
                    rname
                )?;
            } else {
                writeln!(w, "                visitor({});", rname)?;
            }
        }

        writeln!(w, "            }}")?;
    }

    writeln!(w, "        }}")?;
    writeln!(w, "    }}")?;
    Ok(())
}

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let manifest_dir = PathBuf::from(env::var("CARGO_MANIFEST_DIR")?);
    let def_path = manifest_dir.join("../Bytecode/Bytecode.def");

    println!("cargo:rerun-if-changed={}", def_path.display());
    println!("cargo:rerun-if-changed=build.rs");

    let out_dir = PathBuf::from(env::var("OUT_DIR")?);
    let file = fs::OpenOptions::new()
        .write(true)
        .create(true)
        .truncate(true) // empties contents of the file
        .open(out_dir.join("instruction_generated.rs"))?;
    let ops = parse_bytecode_def(&def_path);
    generate_rust_code(file, &ops)
}
