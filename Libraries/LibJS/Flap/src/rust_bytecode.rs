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
//! `PropertyKeyTableIndex`, `StringTableIndex`,
//! `EnvironmentCoordinate`) with `from_raw` / `optional_from_raw` constructors,
//! plus `read_u16(bytes, at)`, `read_name_index(bytes, at)`, `read_optional_name_index(bytes, at)`, `read_u32(bytes, at)`
//! and `read_u64(bytes, at)` helpers reachable through the `reader` path prefix passed to the field reading functions,
//! and the `is_wide_name_index(index)` and `name_index_field(index, distance)` helpers of the encoder.

use crate::metadata::Field;
use crate::metadata::InstructionDefinition;
use crate::metadata::OPERAND_SIZE;
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

/// The code for `expression` elements of `element_size` bytes, without a multiplication by one.
pub fn scaled(expression: &str, element_size: usize) -> String {
    if element_size == 1 {
        expression.to_string()
    } else {
        format!("{expression} * {element_size}")
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
/// the `read_u32` / `read_u64` helpers, for example `"super::operand::"`.
pub fn read_expr_for_type(ty: &str, offset: &str, reader: &str) -> String {
    match ty {
        "bool" => format!("bytes[{offset}] != 0"),
        "u8" => format!("u32::from(bytes[{offset}])"),
        "u16" => format!("{reader}read_u16(bytes, {offset})"),
        "i32" => format!("{reader}read_u32(bytes, {offset}) as i32"),
        "u32" | "TemplateObjectCacheIndex" | "ObjectPropertyIteratorCacheIndex" | "EnvironmentShapeCacheIndex" => {
            format!("{reader}read_u32(bytes, {offset})")
        }
        "PropertyLookupCacheIndex"
        | "ProfiledPropertyLookupCacheIndex"
        | "GlobalVariableCacheIndex"
        | "EnvironmentCoordinateCacheIndex"
        | "ObjectShapeCacheIndex" => format!("u32::from({reader}read_u16(bytes, {offset}))"),
        "ArithFeedbackIndex" | "CallFeedbackIndex" | "KeyedFeedbackIndex" | "ValueFeedbackIndex" => {
            format!("{reader}read_u16(bytes, {offset})")
        }
        "u64" | "Value" => format!("{reader}read_u64(bytes, {offset})"),
        "Operand" => format!("Operand::from_raw(u32::from(bytes[{offset}]))"),
        "Optional<Operand>" => {
            format!("match bytes[{offset}] {{ u8::MAX => None, raw => Some(Operand::from_raw(u32::from(raw))) }}")
        }
        "Label" => format!("Label({reader}read_u32(bytes, {offset}))"),
        "Optional<Label>" => {
            format!("if bytes[{offset} + 4] != 0 {{ Some(Label({reader}read_u32(bytes, {offset}))) }} else {{ None }}")
        }
        "IdentifierTableIndex" => format!("IdentifierTableIndex({reader}read_name_index(bytes, {offset}))"),
        "Optional<IdentifierTableIndex>" => {
            format!("{reader}read_optional_name_index(bytes, {offset}).map(IdentifierTableIndex)")
        }
        "PropertyKeyTableIndex" => format!("PropertyKeyTableIndex({reader}read_name_index(bytes, {offset}))"),
        "StringTableIndex" => format!("StringTableIndex({reader}read_name_index(bytes, {offset}))"),
        "Optional<StringTableIndex>" => {
            format!("{reader}read_optional_name_index(bytes, {offset}).map(StringTableIndex)")
        }
        "EnvironmentCoordinate" => format!(
            "EnvironmentCoordinate {{ hops: u32::from(bytes[{offset}]), index: u32::from({reader}read_u16(bytes, {offset} + 1)) }}"
        ),
        "Builtin" | "Completion::Type" | "IteratorHint" | "EnvironmentMode" | "PutKind" | "ArgumentsKind"
        | "FunctionNamePrefix" => format!("bytes[{offset}]"),
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
/// of an error enum with `TruncatedInstruction` and `UnknownOpcode` variants.
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
        "/// Works out the size of variable-length instructions from their count; for fixed-"
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
            let array = op.array.as_ref().expect("variable op missing array metadata");
            let count_offset = op.layout.field_offsets[&op.fields[array.count_field_index].name];
            let element_size = array.element_size;
            let op_name = &op.name;
            writeln!(w, "        {i} => {{ // {op_name} (variable-length)")?;
            let count_size = field_type_info(&op.fields[array.count_field_index].ty).size;
            writeln!(w, "            let count_end = at + {count_offset} + {count_size};")?;
            writeln!(w, "            if count_end > bytes.len() {{")?;
            writeln!(w, "                return Err({error}::TruncatedInstruction);")?;
            writeln!(w, "            }}")?;
            if count_size == 1 {
                writeln!(w, "            let count = u32::from(bytes[at + {count_offset}]);")?;
            } else {
                writeln!(
                    w,
                    "            let count = u32::from_ne_bytes(bytes[at + {count_offset}..count_end].try_into().unwrap());"
                )?;
            }
            let length = scaled("count as usize", element_size);
            writeln!(w, "            Ok({minimum_length} + {length})")?;
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
        let name_fields = name_index_fields(op);

        if !name_fields.is_empty() {
            // NB: A WideNameIndices before the instruction is part of it.
            let mut bindings = Vec::new();
            let mut indices = Vec::new();
            for field in &fields {
                let name = rust_field_name(&field.name);
                if name_fields.iter().any(|name_field| name_field.name == field.name) {
                    if field.ty.starts_with("Optional<") {
                        indices.push(format!("{name}.map_or(u32::MAX, |index| index.0)"));
                    } else {
                        indices.push(format!("{name}.0"));
                    }
                    bindings.push(name);
                } else if field.is_array {
                    bindings.push(name);
                } else {
                    bindings.push(format!("{name}: _"));
                }
            }
            let size = match (op.layout.size, &op.array) {
                (Some(size), _) => size.to_string(),
                (None, Some(array)) => format!(
                    "{} + {}",
                    op.layout.minimum_size,
                    scaled(
                        &format!("{}.len()", rust_field_name(&op.fields[array.field_index].name)),
                        array.element_size
                    )
                ),
                (None, None) => unreachable!("variable op missing array metadata"),
            };
            writeln!(
                w,
                "            Instruction::{} {{ {} }} => {{",
                op.name,
                bindings.join(", ")
            )?;
            writeln!(
                w,
                "                let wide = [{}].into_iter().any(is_wide_name_index);",
                indices.join(", ")
            )?;
            writeln!(
                w,
                "                {size} + if wide {{ {WIDE_NAME_INDICES_LENGTH} }} else {{ 0 }}"
            )?;
            writeln!(w, "            }}")?;
        } else if let Some(final_size) = op.layout.size {
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
                "                {} + {}",
                op.layout.minimum_size,
                scaled(&format!("{arr_name}.len()"), array.element_size)
            )?;
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
pub fn generate_encode_method(
    mut w: impl Write,
    ops: &[InstructionDefinition],
    method_name: &str,
    encoder_type: &str,
) -> GeneratorResult {
    writeln!(
        w,
        "    /// Encode this instruction into bytes in the struct layout of its fields."
    )?;
    writeln!(w, "    pub fn {method_name}(&self, encoder: &mut {encoder_type}) {{")?;
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

        generate_encode_body(&mut w, op)?;

        writeln!(w, "            }}")?;
    }

    writeln!(w, "        }}")?;
    writeln!(w, "    }}")?;
    writeln!(w)?;
    Ok(())
}

/// How many name table indices a WideNameIndices holds, the most any instruction has.
const WIDE_NAME_INDEX_SLOTS: usize = 2;

/// The size of a WideNameIndices: its opcode and a u32 per slot.
const WIDE_NAME_INDICES_LENGTH: usize = 1 + 4 * WIDE_NAME_INDEX_SLOTS;

/// The fields of `op` that hold a name table index, in the order of their slots in a WideNameIndices.
fn name_index_fields(op: &InstructionDefinition) -> Vec<&Field> {
    let fields = user_fields(op)
        .into_iter()
        .filter(|field| matches!(field_type_info(&field.ty).kind, "u32_newtype" | "optional_u32_newtype"))
        .collect::<Vec<_>>();
    assert!(
        fields.len() <= WIDE_NAME_INDEX_SLOTS && fields.iter().all(|field| !field.is_array),
        "{}: a WideNameIndices has no room for its name table indices",
        op.name
    );
    fields
}

/// Emit the statements that encode `op`, whose fields are bound by reference.
fn generate_encode_body(mut w: impl Write, op: &InstructionDefinition) -> GeneratorResult {
    // The fixed part of the instruction goes through one buffer of its exact size.
    let fixed_size = match &op.array {
        Some(array) => array.offset,
        None => op.layout.size.expect("fixed op missing encoded size"),
    };
    writeln!(w, "                let mut out = [0u8; {fixed_size}];")?;
    writeln!(w, "                out[0] = OpCode::{} as u8;", op.name)?;

    // Name table indices too large for their fields go in a WideNameIndices right before the instruction.
    let name_fields = name_index_fields(op);
    if !name_fields.is_empty() {
        let indices = (0..WIDE_NAME_INDEX_SLOTS)
            .map(|slot| match name_fields.get(slot) {
                Some(field) if field.ty.starts_with("Optional<") => {
                    format!("{}.map_or(u32::MAX, |index| index.0)", rust_field_name(&field.name))
                }
                Some(field) => format!("{}.0", rust_field_name(&field.name)),
                None => "u32::MAX".to_string(),
            })
            .collect::<Vec<_>>();
        writeln!(
            w,
            "                let name_indices: [u32; {WIDE_NAME_INDEX_SLOTS}] = [{}];",
            indices.join(", ")
        )?;
        writeln!(
            w,
            "                if name_indices.iter().any(|&index| is_wide_name_index(index)) {{"
        )?;
        writeln!(
            w,
            "                    encoder.bytes.push(OpCode::WideNameIndices as u8);"
        )?;
        writeln!(w, "                    for index in name_indices {{")?;
        writeln!(
            w,
            "                        encoder.bytes.extend_from_slice(&index.to_ne_bytes());"
        )?;
        writeln!(w, "                    }}")?;
        writeln!(w, "                }}")?;
    }

    // NB: Only the user fields get written; the opcode is written above.
    for f in &op.fields {
        if f.is_array || f.name == "m_type" || f.name == "m_strict" {
            continue;
        }
        let info = field_type_info(&f.ty);
        let field_offset = op.layout.field_offsets[&f.name];
        assert!(
            field_offset + info.size <= fixed_size,
            "{}: field {} does not fit the fixed part",
            op.name,
            f.name
        );
        if let Some(slot) = name_fields.iter().position(|field| field.name == f.name) {
            // The distance back from the field to its slot of the WideNameIndices.
            let distance = field_offset + WIDE_NAME_INDICES_LENGTH - 1 - 4 * slot;
            writeln!(
                w,
                "                out[{field_offset}..{}].copy_from_slice(&name_index_field(name_indices[{slot}], {distance}).to_ne_bytes());",
                field_offset + 2
            )?;
            continue;
        }
        emit_field_store(&mut w, &rust_field_name(&f.name), info.kind, field_offset)?;
    }
    writeln!(w, "                let start = encoder.bytes.len();")?;
    writeln!(w, "                encoder.bytes.extend_from_slice(&out);")?;

    // Write trailing array elements
    if let Some(array) = &op.array {
        let field = &op.fields[array.field_index];
        let info = field_type_info(&field.ty);
        let rname = rust_field_name(&field.name);

        writeln!(w, "                for item in {rname} {{")?;
        emit_field_write(&mut w, "item", info.kind)?;
        writeln!(w, "                }}")?;
        writeln!(
            w,
            "                let target = {} + {};",
            op.layout.minimum_size,
            scaled(&format!("{rname}.len()"), array.element_size)
        )?;
        writeln!(w, "                encoder.bytes.resize(start + target, 0);")?;
    } else {
        writeln!(w, "                let _ = start;")?;
    }

    Ok(())
}

/// Emit a module with a plain struct for every instruction, for code that knows which instruction it writes: the fields
/// are those of `Instruction`, with a slice for a trailing array, so writing one takes no allocation and no enum.
/// `specializable_operands` names the operands of an instruction whose value may select a specialized form.
pub fn generate_typed_instructions(
    mut w: impl Write,
    ops: &[InstructionDefinition],
    module_name: &str,
    encoder_type: &str,
    specializable_operands: &dyn Fn(&str) -> Vec<String>,
) -> GeneratorResult {
    writeln!(w, "pub mod {module_name} {{")?;
    writeln!(w, "use super::*;")?;
    writeln!(w)?;
    writeln!(w, "pub trait TypedInstruction {{")?;
    writeln!(w, "    const OPCODE: OpCode;")?;
    writeln!(w, "    const IS_TERMINATOR: bool;")?;
    writeln!(
        w,
        "    /// Whether an operand that is a constant may select a specialized form."
    )?;
    writeln!(w, "    fn may_specialize(&self) -> bool;")?;
    writeln!(w, "    fn encode(&self, encoder: &mut {encoder_type});")?;
    writeln!(w, "    fn to_instruction(&self) -> Instruction;")?;
    writeln!(w, "}}")?;
    writeln!(w)?;
    for op in ops {
        let fields = user_fields(op);
        let has_array = fields.iter().any(|f| f.is_array);
        let lifetime = if has_array { "<'a>" } else { "" };
        let anonymous_lifetime = if has_array { "<'_>" } else { "" };
        writeln!(w, "#[derive(Clone, Copy)]")?;
        if fields.is_empty() {
            writeln!(w, "pub struct {} {{}}", op.name)?;
        } else {
            writeln!(w, "pub struct {}{lifetime} {{", op.name)?;
            for f in &fields {
                let info = field_type_info(&f.ty);
                let r_name = rust_field_name(&f.name);
                if f.is_array {
                    writeln!(w, "    pub {}: &'a [{}],", r_name, info.rust_type)?;
                } else {
                    writeln!(w, "    pub {}: {},", r_name, info.rust_type)?;
                }
            }
            writeln!(w, "}}")?;
        }
        writeln!(w, "impl TypedInstruction for {}{anonymous_lifetime} {{", op.name)?;
        writeln!(w, "    const OPCODE: OpCode = OpCode::{};", op.name)?;
        writeln!(w, "    const IS_TERMINATOR: bool = {};", op.is_terminator)?;
        let operands = specializable_operands(&op.name);
        writeln!(w, "    #[inline(always)]")?;
        if operands.is_empty() {
            writeln!(w, "    fn may_specialize(&self) -> bool {{ false }}")?;
        } else {
            let tests = operands
                .iter()
                .map(|operand| format!("self.{operand}.is_constant()"))
                .collect::<Vec<_>>();
            writeln!(w, "    fn may_specialize(&self) -> bool {{ {} }}", tests.join(" || "))?;
        }
        writeln!(w, "    #[inline(always)]")?;
        writeln!(w, "    fn encode(&self, encoder: &mut {encoder_type}) {{")?;
        if !fields.is_empty() {
            let bindings: Vec<String> = fields.iter().map(|f| rust_field_name(&f.name)).collect();
            writeln!(w, "                let Self {{ {} }} = self;", bindings.join(", "))?;
            for f in fields.iter().filter(|f| f.is_array) {
                let name = rust_field_name(&f.name);
                writeln!(w, "                let {name} = *{name};")?;
            }
        }
        generate_encode_body(&mut w, op)?;
        writeln!(w, "    }}")?;
        writeln!(w, "    #[inline(always)]")?;
        writeln!(w, "    fn to_instruction(&self) -> Instruction {{")?;
        if fields.is_empty() {
            writeln!(w, "        Instruction::{}", op.name)?;
        } else {
            let values: Vec<String> = fields
                .iter()
                .map(|f| {
                    let name = rust_field_name(&f.name);
                    if f.is_array {
                        format!("{name}: self.{name}.to_vec()")
                    } else {
                        format!("{name}: self.{name}")
                    }
                })
                .collect();
            writeln!(w, "        Instruction::{} {{ {} }}", op.name, values.join(", "))?;
        }
        writeln!(w, "    }}")?;
        writeln!(w, "}}")?;
    }
    writeln!(w, "}}")?;
    writeln!(w)?;
    Ok(())
}

/// Emit code to store a field of the fixed part of an instruction into `out` at `offset`.
///
/// All bindings from pattern matching are references (`&T`). Rust auto-derefs for method calls, but explicit `*` is
/// needed for casts and direct stores of Copy types.
fn emit_field_store(mut w: impl Write, name: &str, kind: &str, offset: usize) -> GeneratorResult {
    let prefix = " ".repeat(16);
    let store = |value: &str, size: usize| format!("out[{offset}..{}].copy_from_slice(&{value});", offset + size);
    match kind {
        "bool" => writeln!(w, "{prefix}out[{offset}] = *{name} as u8;")?,
        "u8" => writeln!(w, "{prefix}out[{offset}] = *{name};")?,
        "byte_count" => writeln!(
            w,
            "{prefix}out[{offset}] = u8::try_from(*{name}).expect(\"counts fit in a byte\");"
        )?,
        "i32" | "u32" => writeln!(w, "{prefix}{}", store(&format!("{name}.to_ne_bytes()"), 4))?,
        "u16" => writeln!(w, "{prefix}{}", store(&format!("{name}.to_ne_bytes()"), 2))?,
        "u64" => writeln!(w, "{prefix}{}", store(&format!("{name}.to_ne_bytes()"), 8))?,
        "operand" => writeln!(
            w,
            "{prefix}{}",
            store(
                &format!("(encoder.operand(*{name}, {offset}) as u8).to_ne_bytes()"),
                OPERAND_SIZE
            )
        )?,
        "optional_operand" => {
            let value = format!("{name}.map_or(u8::MAX, |op| encoder.operand(op, {offset}) as u8).to_ne_bytes()");
            writeln!(w, "{prefix}{}", store(&value, OPERAND_SIZE))?;
        }
        "label" => {
            writeln!(w, "{prefix}encoder.note_label({offset});")?;
            writeln!(w, "{prefix}{}", store(&format!("{name}.0.to_ne_bytes()"), 4))?;
        }
        "optional_label" => {
            // Optional<Label> layout: u32 value, then bool has_value.
            writeln!(w, "{prefix}if let Some(lbl) = {name} {{")?;
            writeln!(w, "{prefix}    encoder.note_label({offset});")?;
            writeln!(w, "{prefix}    {}", store("lbl.0.to_ne_bytes()", 4))?;
            writeln!(w, "{prefix}    out[{}] = 1;", offset + 4)?;
            writeln!(w, "{prefix}}}")?;
        }
        "cache_index" => writeln!(
            w,
            "{prefix}{}",
            store(&format!("narrow_index(*{name}).to_ne_bytes()"), 2)
        )?,
        "env_coord" => {
            writeln!(
                w,
                "{prefix}out[{offset}] = u8::try_from({name}.hops).expect(\"coordinates fit in their bytecode field\");"
            )?;
            writeln!(
                w,
                "{prefix}out[{}..{}].copy_from_slice(&u16::try_from({name}.index).expect(\"coordinates fit in their bytecode field\").to_ne_bytes());",
                offset + 1,
                offset + 3
            )?;
        }
        other => panic!("Unknown encoding kind: {other}"),
    }

    Ok(())
}

/// Emit code to append an element of the trailing array of an instruction, a reference, to the encoder.
fn emit_field_write(mut w: impl Write, name: &str, kind: &str) -> GeneratorResult {
    let prefix = " ".repeat(20);
    match kind {
        "bool" => writeln!(w, "{prefix}encoder.bytes.push(*{name} as u8);")?,
        "u8" => writeln!(w, "{prefix}encoder.bytes.push(*{name});")?,
        "i32" | "u16" | "u32" | "u64" => {
            writeln!(w, "{prefix}encoder.bytes.extend_from_slice(&{name}.to_ne_bytes());")?
        }
        "operand" => {
            writeln!(w, "{prefix}let raw = encoder.operand(*{name}, 0) as u8;")?;
            writeln!(w, "{prefix}encoder.bytes.extend_from_slice(&raw.to_ne_bytes());")?;
        }
        "optional_operand" => {
            writeln!(
                w,
                "{prefix}let raw = {name}.map_or(u8::MAX, |op| encoder.operand(op, 0) as u8);"
            )?;
            writeln!(w, "{prefix}encoder.bytes.extend_from_slice(&raw.to_ne_bytes());")?;
        }
        other => panic!("Unknown array element encoding kind: {other}"),
    }

    Ok(())
}
