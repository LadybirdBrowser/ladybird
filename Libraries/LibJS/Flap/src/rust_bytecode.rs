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
/// the `read_u32` / `read_u64` helpers, for example `"super::operand::"`.
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
    writeln!(
        w,
        "    pub fn {method_name}(&self, strict: bool, encoder: &mut {encoder_type}) {{"
    )?;
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

/// Emit the statements that encode `op`, whose fields are bound by reference.
fn generate_encode_body(mut w: impl Write, op: &InstructionDefinition) -> GeneratorResult {
    // The fixed part of the instruction goes through one buffer of its exact size.
    let fixed_size = match &op.array {
        Some(array) => array.offset,
        None => op.layout.size.expect("fixed op missing encoded size"),
    };
    writeln!(w, "                let mut out = [0u8; {fixed_size}];")?;
    writeln!(w, "                out[0] = OpCode::{} as u8;", op.name)?;
    writeln!(w, "                out[1] = strict as u8;")?;

    // We iterate ALL fields (including m_type, m_strict, m_length) for
    // accurate alignment but only emit writes for user fields.
    for f in &op.fields {
        if f.is_array || f.name == "m_type" || f.name == "m_strict" || f.name == "m_length" {
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
            "                let target = ({} + {}.len() * {} + 7) & !7;",
            op.layout.minimum_size, rname, array.element_size
        )?;
        writeln!(w, "                encoder.bytes.resize(start + target, 0);")?;

        let m_length_offset = op
            .layout
            .m_length_offset
            .expect("the parser requires m_length for array ops");
        writeln!(w, "                let total_len = target as u32;")?;
        writeln!(
            w,
            "                encoder.bytes[start + {}..start + {}].copy_from_slice(&total_len.to_ne_bytes());",
            m_length_offset,
            m_length_offset + 4
        )?;
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
    writeln!(w, "    fn encode(&self, strict: bool, encoder: &mut {encoder_type});")?;
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
        writeln!(w, "    fn encode(&self, strict: bool, encoder: &mut {encoder_type}) {{")?;
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
        "i32" | "u32" => writeln!(w, "{prefix}{}", store(&format!("{name}.to_ne_bytes()"), 4))?,
        "u16" => writeln!(w, "{prefix}{}", store(&format!("{name}.to_ne_bytes()"), 2))?,
        "u64" => writeln!(w, "{prefix}{}", store(&format!("{name}.to_ne_bytes()"), 8))?,
        "operand" => writeln!(
            w,
            "{prefix}{}",
            store(&format!("encoder.operand(*{name}, {offset}).to_ne_bytes()"), 4)
        )?,
        "optional_operand" => {
            let value = format!("{name}.map_or(Operand::INVALID, |op| encoder.operand(op, {offset})).to_ne_bytes()");
            writeln!(w, "{prefix}{}", store(&value, 4))?;
        }
        "label" => {
            writeln!(w, "{prefix}encoder.note_label({offset});")?;
            writeln!(w, "{prefix}{}", store(&format!("{name}.0.to_ne_bytes()"), 4))?;
        }
        "optional_label" => {
            // Optional<Label> layout: u32 value, bool has_value, 3 bytes padding = 8 bytes total
            writeln!(w, "{prefix}if let Some(lbl) = {name} {{")?;
            writeln!(w, "{prefix}    encoder.note_label({offset});")?;
            writeln!(w, "{prefix}    {}", store("lbl.0.to_ne_bytes()", 4))?;
            writeln!(w, "{prefix}    out[{}] = 1;", offset + 4)?;
            writeln!(w, "{prefix}}}")?;
        }
        "u32_newtype" => writeln!(w, "{prefix}{}", store(&format!("{name}.0.to_ne_bytes()"), 4))?,
        "optional_u32_newtype" => {
            let value = format!("{name}.map_or(0xFFFF_FFFFu32, |idx| idx.0).to_ne_bytes()");
            writeln!(w, "{prefix}{}", store(&value, 4))?;
        }
        "env_coord" => {
            writeln!(w, "{prefix}{}", store(&format!("{name}.hops.to_ne_bytes()"), 4))?;
            writeln!(
                w,
                "{prefix}out[{}..{}].copy_from_slice(&{name}.index.to_ne_bytes());",
                offset + 4,
                offset + 8
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
            writeln!(w, "{prefix}let raw = encoder.operand(*{name}, 0);")?;
            writeln!(w, "{prefix}encoder.bytes.extend_from_slice(&raw.to_ne_bytes());")?;
        }
        "optional_operand" => {
            writeln!(
                w,
                "{prefix}let raw = {name}.map_or(Operand::INVALID, |op| encoder.operand(op, 0));"
            )?;
            writeln!(w, "{prefix}encoder.bytes.extend_from_slice(&raw.to_ne_bytes());")?;
        }
        "u32_newtype" => writeln!(w, "{prefix}encoder.bytes.extend_from_slice(&{name}.0.to_ne_bytes());")?,
        other => panic!("Unknown array element encoding kind: {other}"),
    }

    Ok(())
}
