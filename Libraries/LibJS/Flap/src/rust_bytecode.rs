/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Shared Rust code generation for bytecode instruction types.
//!
//! Both `Libraries/LibJS/Rust/build.rs` (the bytecode generator) and
//! `Libraries/LibJS/JIT/Rust/build.rs` (the JIT's bytecode decoder) generate
//! Rust code from interpreter.flap. The pieces they have in common live here so
//! that there is exactly one implementation of the instruction set loading, the
//! `Instruction` enum, the encoder and the field readers.
//!
//! The generated code expects the including module to provide the operand types
//! named by `field_type_info()` (`Operand`, `Label`, `IdentifierTableIndex`,
//! `PropertyKeyTableIndex`, `StringTableIndex`, `RegexTableIndex`,
//! `EnvironmentCoordinate`) with `from_raw` / `optional_from_raw` constructors,
//! plus `read_u16(bytes, at)`, `read_u32(bytes, at)` and `read_u64(bytes, at)` helpers reachable
//! through the `reader` path prefix passed to the field reading functions.

use crate::metadata::Field;
use crate::metadata::InstructionDefinition;
use crate::metadata::Specialization;
use crate::metadata::SpecializedInstruction;
use crate::metadata::field_type_info;
use crate::metadata::user_fields;
use std::error::Error;
use std::io::Write;
use std::path::Path;

pub type GeneratorResult = Result<(), Box<dyn Error>>;

/// All instructions defined by interpreter.flap, including the declarative
/// specializations, in opcode order.
pub struct InstructionSet {
    /// Every opcode, in opcode order. The specialized instructions follow the
    /// handlers they were derived from.
    pub ops: Vec<InstructionDefinition>,
    pub specializations: Vec<Specialization>,
    pub specialized_ops: Vec<SpecializedInstruction>,
}

impl InstructionSet {
    /// Parse and validate interpreter.flap at `flap_path`.
    pub fn load(flap_path: &Path) -> Result<Self, Box<dyn Error>> {
        let content = std::fs::read_to_string(flap_path)
            .map_err(|error| format!("failed to read {}: {error}", flap_path.display()))?;
        let source_name = flap_path.to_string_lossy();
        let mut ops = crate::metadata::parse_flap_metadata(&source_name, &content)?;
        let specializations = crate::metadata::parse_specializations(&source_name, &content)?;
        crate::validate_specializations(&ops, &specializations)?;
        let specialized_ops = crate::metadata::derive_specialized_instructions(&ops, &specializations)?;
        ops.extend(
            specialized_ops
                .iter()
                .map(|specialization| specialization.definition.clone()),
        );
        Ok(Self {
            ops,
            specializations,
            specialized_ops,
        })
    }
}

/// The Rust name of an instruction field (`m_dst` becomes `dst`).
pub fn rust_field_name(name: &str) -> String {
    if let Some(stripped) = name.strip_prefix("m_") {
        stripped.to_string()
    } else {
        name.to_string()
    }
}

/// The name of the field holding the element count of `array_field`.
pub fn count_field_name<'a>(op: &'a InstructionDefinition, array_field: &Field) -> &'a str {
    let array = op.array.as_ref().expect("the parser validates array metadata");
    assert_eq!(op.fields[array.field_index].name, array_field.name);
    &op.fields[array.count_field_index].name
}

/// A Rust expression reading a field of type `ty` from `bytes` at the byte
/// offset `offset` (itself a Rust expression). `reader` is the path prefix of
/// the `read_u32` / `read_u64` helpers, for example `"super::validator::"`.
pub fn read_expr_for_type(ty: &str, offset: &str, reader: &str) -> String {
    match ty {
        "bool" => format!("bytes[{offset}] != 0"),
        "i32" => format!("{reader}read_u32(bytes, {offset}) as i32"),
        "u32"
        | "Completion::Type"
        | "IteratorHint"
        | "EnvironmentMode"
        | "PutKind"
        | "ArgumentsKind"
        | "FunctionNamePrefix"
        | "PropertyLookupCacheIndex"
        | "GlobalVariableCacheIndex"
        | "EnvironmentCoordinateCacheIndex"
        | "TemplateObjectCacheIndex"
        | "ObjectShapeCacheIndex"
        | "ObjectPropertyIteratorCacheIndex"
        | "EnvironmentShapeCacheIndex" => format!("{reader}read_u32(bytes, {offset})"),
        "ArithFeedbackIndex" | "CallFeedbackIndex" | "KeyedFeedbackIndex" | "ValueFeedbackIndex" => {
            format!("{reader}read_u16(bytes, {offset})")
        }
        "u64" | "Value" => format!("{reader}read_u64(bytes, {offset})"),
        "Operand" => format!("Operand::from_raw({reader}read_u32(bytes, {offset}))"),
        "Optional<Operand>" => format!("Operand::optional_from_raw({reader}read_u32(bytes, {offset}))"),
        "Label" => format!("Label({reader}read_u32(bytes, {offset}))"),
        "Optional<Label>" => {
            format!("if bytes[{offset} + 4] != 0 {{ Some(Label({reader}read_u32(bytes, {offset}))) }} else {{ None }}")
        }
        "IdentifierTableIndex" => format!("IdentifierTableIndex({reader}read_u32(bytes, {offset}))"),
        "Optional<IdentifierTableIndex>" => {
            format!("IdentifierTableIndex::optional_from_raw({reader}read_u32(bytes, {offset}))")
        }
        "PropertyKeyTableIndex" => format!("PropertyKeyTableIndex({reader}read_u32(bytes, {offset}))"),
        "StringTableIndex" => format!("StringTableIndex({reader}read_u32(bytes, {offset}))"),
        "Optional<StringTableIndex>" => {
            format!("StringTableIndex::optional_from_raw({reader}read_u32(bytes, {offset}))")
        }
        "RegexTableIndex" => format!("RegexTableIndex({reader}read_u32(bytes, {offset}))"),
        "EnvironmentCoordinate" => format!(
            "EnvironmentCoordinate {{ hops: {reader}read_u32(bytes, {offset}), index: {reader}read_u32(bytes, {offset} + 4) }}"
        ),
        "Builtin" => format!("bytes[{offset}]"),
        other => unreachable!("Unknown field type: {other}"),
    }
}

/// Emit `let <field> = <read>;` for every non-array user field of `op`,
/// reading the instruction that starts at `at`.
pub fn generate_field_reads(w: &mut impl Write, op: &InstructionDefinition, reader: &str) -> GeneratorResult {
    for f in user_fields(op) {
        if f.is_array {
            continue;
        }
        let rname = rust_field_name(&f.name);
        let offset = op.layout.field_offsets.get(&f.name).expect("field offset missing");
        let expr = read_expr_for_type(&f.ty, &format!("at + {offset}"), reader);
        writeln!(w, "            let {rname} = {expr};")?;
    }
    Ok(())
}

pub fn generate_opcode_enum(mut w: impl Write, ops: &[InstructionDefinition]) -> GeneratorResult {
    writeln!(
        w,
        "/// Bytecode opcode (u8), numbered in the order of the instruction definitions."
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

pub fn generate_num_opcodes_const(mut w: impl Write, ops: &[InstructionDefinition]) -> GeneratorResult {
    writeln!(w, "/// Number of distinct opcodes (the valid range for the type byte).")?;
    writeln!(w, "pub const NUM_OPCODES: u32 = {};", ops.len())?;
    writeln!(w)?;
    Ok(())
}

pub fn generate_instruction_name_from_opcode(mut w: impl Write, ops: &[InstructionDefinition]) -> GeneratorResult {
    writeln!(w, "pub fn instruction_name_from_opcode(opcode: u8) -> &'static str {{")?;
    writeln!(w, "    match opcode {{")?;
    for (i, op) in ops.iter().enumerate() {
        writeln!(w, "        {i} => \"{}\",", op.name)?;
    }
    writeln!(w, "        _ => unreachable!(\"unknown bytecode opcode\"),")?;
    writeln!(w, "    }}")?;
    writeln!(w, "}}")?;
    writeln!(w)?;
    Ok(())
}

pub fn generate_instruction_is_terminator_from_opcode(
    mut w: impl Write,
    ops: &[InstructionDefinition],
) -> GeneratorResult {
    writeln!(w, "pub fn instruction_is_terminator_from_opcode(opcode: u8) -> bool {{")?;
    let terminators = ops
        .iter()
        .enumerate()
        .filter(|(_, op)| op.is_terminator)
        .map(|(i, _)| i.to_string())
        .collect::<Vec<_>>();
    writeln!(w, "    matches!(opcode, {})", terminators.join(" | "))?;
    writeln!(w, "}}")?;
    writeln!(w)?;
    Ok(())
}

/// Emit `instruction_length_from_bytes(opcode, bytes, at)`. `error` is the path
/// of an error enum with `TruncatedInstruction`, `InvalidLength` and
/// `UnknownOpcode` variants.
pub fn generate_instruction_length_from_bytes(
    mut w: impl Write,
    ops: &[InstructionDefinition],
    error: &str,
) -> GeneratorResult {
    writeln!(
        w,
        "/// Returns the encoded length in bytes of the instruction at `bytes[at..]`."
    )?;
    writeln!(
        w,
        "/// Reads `m_length` from the buffer for variable-length instructions; for fixed-"
    )?;
    writeln!(w, "/// length instructions, returns the statically-known size.")?;
    writeln!(
        w,
        "pub fn instruction_length_from_bytes(opcode: u8, bytes: &[u8], at: usize) -> Result<usize, {error}> {{"
    )?;
    writeln!(w, "    match opcode {{")?;

    for (i, op) in ops.iter().enumerate() {
        if let Some(final_size) = op.layout.size {
            let op_name = &op.name;
            writeln!(w, "        {i} => Ok({final_size}), // {op_name}")?;
        } else {
            let minimum_length = op.layout.minimum_size;
            let m_length_offset = op
                .layout
                .m_length_offset
                .expect("the parser requires m_length for array ops");
            let op_name = &op.name;
            writeln!(w, "        {i} => {{ // {op_name} (variable-length)")?;
            writeln!(w, "            let m_length_end = at + {m_length_offset} + 4;")?;
            writeln!(w, "            if m_length_end > bytes.len() {{")?;
            writeln!(w, "                return Err({error}::TruncatedInstruction);")?;
            writeln!(w, "            }}")?;
            writeln!(
                w,
                "            let raw = u32::from_ne_bytes(bytes[at + {m_length_offset}..m_length_end].try_into().unwrap());"
            )?;
            writeln!(w, "            if raw < {minimum_length} {{")?;
            writeln!(w, "                return Err({error}::InvalidLength);")?;
            writeln!(w, "            }}")?;
            writeln!(w, "            Ok(raw as usize)")?;
            writeln!(w, "        }}")?;
        }
    }

    writeln!(w, "        _ => Err({error}::UnknownOpcode),")?;
    writeln!(w, "    }}")?;
    writeln!(w, "}}")?;
    writeln!(w)?;
    Ok(())
}

/// Emit the `Instruction` enum with one variant per opcode. `derives` is the
/// list of traits to derive, for example `"Debug, Clone"`.
pub fn generate_instruction_enum(mut w: impl Write, ops: &[InstructionDefinition], derives: &str) -> GeneratorResult {
    writeln!(w, "/// A bytecode instruction with typed fields.")?;
    writeln!(w, "///")?;
    writeln!(w, "/// Each variant corresponds to one instruction definition.")?;
    writeln!(w, "#[derive({derives})]")?;
    writeln!(w, "pub enum Instruction {{")?;
    for op in ops {
        let fields = user_fields(op);
        if fields.is_empty() {
            writeln!(w, "    {},", op.name)?;
        } else {
            writeln!(w, "    {} {{", op.name)?;
            for f in &fields {
                let info = field_type_info(&f.ty);
                let r_name = rust_field_name(&f.name);
                if f.is_array {
                    writeln!(w, "        {}: Vec<{}>,", r_name, info.rust_type)?;
                } else {
                    writeln!(w, "        {}: {},", r_name, info.rust_type)?;
                }
            }
            writeln!(w, "    }},")?;
        }
    }
    writeln!(w, "}}")?;
    writeln!(w)?;
    Ok(())
}

/// The match pattern for `op` that ignores all of its fields.
pub fn wildcard_pattern(op: &InstructionDefinition) -> String {
    if user_fields(op).is_empty() {
        format!("Instruction::{}", op.name)
    } else {
        format!("Instruction::{} {{ .. }}", op.name)
    }
}

/// Emit `Instruction::opcode()`. Must be called inside an `impl Instruction`.
pub fn generate_opcode_method(mut w: impl Write, ops: &[InstructionDefinition]) -> GeneratorResult {
    writeln!(w, "    pub fn opcode(&self) -> OpCode {{")?;
    writeln!(w, "        match self {{")?;
    for op in ops {
        writeln!(w, "            {} => OpCode::{},", wildcard_pattern(op), op.name)?;
    }
    writeln!(w, "        }}")?;
    writeln!(w, "    }}")?;
    writeln!(w)?;
    Ok(())
}

/// Emit `Instruction::is_terminator()`. Must be called inside an `impl Instruction`.
pub fn generate_is_terminator_method(mut w: impl Write, ops: &[InstructionDefinition]) -> GeneratorResult {
    writeln!(w, "    pub fn is_terminator(&self) -> bool {{")?;
    writeln!(w, "        matches!(self, ")?;
    let terminators: Vec<&InstructionDefinition> = ops.iter().filter(|op| op.is_terminator).collect();
    for (i, op) in terminators.iter().enumerate() {
        let sep = if i + 1 < terminators.len() { " |" } else { "" };
        writeln!(w, "            {}{}", wildcard_pattern(op), sep)?;
    }
    writeln!(w, "        )")?;
    writeln!(w, "    }}")?;
    writeln!(w)?;
    Ok(())
}

/// Emit `Instruction::encoded_size()`. Must be called inside an `impl Instruction`.
pub fn generate_encoded_size_method(mut w: impl Write, ops: &[InstructionDefinition]) -> GeneratorResult {
    writeln!(w, "    /// Returns the encoded size of this instruction in bytes.")?;
    writeln!(w, "    pub fn encoded_size(&self) -> usize {{")?;
    writeln!(w, "        match self {{")?;

    for op in ops {
        let fields = user_fields(op);

        if let Some(final_size) = op.layout.size {
            writeln!(w, "            {} => {final_size},", wildcard_pattern(op))?;
        } else {
            let array = op.array.as_ref().expect("variable op missing array metadata");
            let array_field = &op.fields[array.field_index];
            let arr_name = rust_field_name(&array_field.name);

            // Bind only the array field
            let bindings: Vec<String> = fields
                .iter()
                .map(|f| {
                    let rname = rust_field_name(&f.name);
                    if rname == arr_name {
                        rname
                    } else {
                        format!("{rname}: _")
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
                op.layout.minimum_size, arr_name, array.element_size
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

/// Emit `Instruction::encode()`, which serializes an instruction into bytes
/// in the struct layout of its fields. Must be called inside an `impl Instruction`.
pub fn generate_encode_method(mut w: impl Write, ops: &[InstructionDefinition]) -> GeneratorResult {
    writeln!(
        w,
        "    /// Encode this instruction into bytes in the struct layout of its fields."
    )?;
    writeln!(w, "    pub fn encode(&self, strict: bool, buf: &mut Vec<u8>) {{")?;
    writeln!(w, "        let start = buf.len();")?;
    writeln!(w, "        match self {{")?;

    for op in ops {
        let fields = user_fields(op);

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

        // Track offset for the struct layout.
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

            let info = field_type_info(&f.ty);
            let field_offset = op.layout.field_offsets[&f.name];
            assert!(
                field_offset >= offset,
                "{}: tracked offset {offset} passed layout offset {field_offset} for {}",
                op.name,
                f.name
            );
            let pad = field_offset - offset;
            if pad > 0 {
                writeln!(w, "                buf.extend_from_slice(&[0u8; {pad}]);")?;
            }
            offset = field_offset;

            if f.name == "m_length" {
                // Write placeholder (patched at end for variable-length instructions)
                writeln!(
                    w,
                    "                buf.extend_from_slice(&[0u8; 4]); // m_length placeholder"
                )?;
            } else {
                let rname = rust_field_name(&f.name);
                emit_field_write(&mut w, &rname, info.kind, false)?;
            }
            offset += info.size;
        }

        // Write trailing array elements
        if let Some(array) = &op.array {
            let field = &op.fields[array.field_index];
            let info = field_type_info(&field.ty);
            let rname = rust_field_name(&field.name);
            assert!(
                array.offset >= offset,
                "{}: tracked offset {offset} passed array offset {} for {}",
                op.name,
                array.offset,
                field.name
            );
            let pad = array.offset - offset;
            if pad > 0 {
                writeln!(w, "                buf.extend_from_slice(&[0u8; {pad}]);")?;
            }

            writeln!(w, "                for item in {rname} {{")?;
            emit_field_write(&mut w, "item", info.kind, true)?;
            writeln!(w, "                }}")?;
            writeln!(
                w,
                "                let target = ({} + {}.len() * {} + 7) & !7;",
                op.layout.minimum_size, rname, array.element_size
            )?;
            writeln!(
                w,
                "                while (buf.len() - start) < target {{ buf.push(0); }}"
            )?;

            let m_length_offset = op
                .layout
                .m_length_offset
                .expect("the parser requires m_length for array ops");
            writeln!(w, "                let total_len = (buf.len() - start) as u32;")?;
            writeln!(
                w,
                "                buf[start + {}..start + {}].copy_from_slice(&total_len.to_ne_bytes());",
                m_length_offset,
                m_length_offset + 4
            )?;
        } else {
            let final_size = op.layout.size.expect("fixed op missing encoded size");
            assert!(
                final_size >= offset,
                "{}: tracked offset {offset} passed final size {final_size}",
                op.name
            );
            let tail_pad = final_size - offset;
            if tail_pad > 0 {
                writeln!(w, "                buf.extend_from_slice(&[0u8; {tail_pad}]);")?;
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
fn emit_field_write(mut w: impl Write, name: &str, kind: &str, is_loop_item: bool) -> GeneratorResult {
    let prefix = " ".repeat(if is_loop_item { 20 } else { 16 });
    match kind {
        "bool" => writeln!(w, "{prefix}buf.push(*{name} as u8);")?,
        "u8" => writeln!(w, "{prefix}buf.push(*{name});")?,
        "i32" => writeln!(w, "{prefix}buf.extend_from_slice(&{name}.to_ne_bytes());")?,
        "u16" => writeln!(w, "{prefix}buf.extend_from_slice(&{name}.to_ne_bytes());")?,
        "u32" => writeln!(w, "{prefix}buf.extend_from_slice(&{name}.to_ne_bytes());")?,
        "u64" => writeln!(w, "{prefix}buf.extend_from_slice(&{name}.to_ne_bytes());")?,
        "operand" => writeln!(w, "{prefix}buf.extend_from_slice(&{name}.raw().to_ne_bytes());")?,
        "optional_operand" => {
            writeln!(w, "{prefix}match {name} {{")?;
            writeln!(
                w,
                "{prefix}    Some(op) => buf.extend_from_slice(&op.raw().to_ne_bytes()),"
            )?;
            writeln!(
                w,
                "{prefix}    None => buf.extend_from_slice(&Operand::INVALID.to_ne_bytes()),"
            )?;
            writeln!(w, "{prefix}}}")?;
        }
        "label" => writeln!(w, "{prefix}buf.extend_from_slice(&{name}.0.to_ne_bytes());")?,
        "optional_label" => {
            // Optional<Label> layout: u32 value, bool has_value, 3 bytes padding = 8 bytes total
            writeln!(w, "{prefix}match {name} {{")?;
            writeln!(w, "{prefix}    Some(lbl) => {{")?;
            writeln!(w, "{prefix}        buf.extend_from_slice(&lbl.0.to_ne_bytes());")?;
            writeln!(w, "{prefix}        buf.push(1); buf.push(0); buf.push(0); buf.push(0);")?;
            writeln!(w, "{prefix}    }}")?;
            writeln!(w, "{prefix}    None => {{")?;
            writeln!(w, "{prefix}        buf.extend_from_slice(&0u32.to_ne_bytes());")?;
            writeln!(w, "{prefix}        buf.push(0); buf.push(0); buf.push(0); buf.push(0);")?;
            writeln!(w, "{prefix}    }}")?;
            writeln!(w, "{prefix}}}")?;
        }
        "u32_newtype" => writeln!(w, "{prefix}buf.extend_from_slice(&{name}.0.to_ne_bytes());")?,
        "optional_u32_newtype" => {
            writeln!(w, "{prefix}match {name} {{")?;
            writeln!(
                w,
                "{prefix}    Some(idx) => buf.extend_from_slice(&idx.0.to_ne_bytes()),"
            )?;
            writeln!(
                w,
                "{prefix}    None => buf.extend_from_slice(&0xFFFF_FFFFu32.to_ne_bytes()),"
            )?;
            writeln!(w, "{prefix}}}")?;
        }
        "env_coord" => {
            writeln!(w, "{prefix}buf.extend_from_slice(&{name}.hops.to_ne_bytes());")?;
            writeln!(w, "{prefix}buf.extend_from_slice(&{name}.index.to_ne_bytes());")?;
        }
        other => panic!("Unknown encoding kind: {other}"),
    }

    Ok(())
}
