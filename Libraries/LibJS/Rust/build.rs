/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Build script that generates Rust bytecode instruction types from Bytecode.def.
//!
//! This mirrors Meta/Generators/generate_libjs_bytecode_def_derived.py but generates Rust
//! code instead of C++. The generated code lives in $OUT_DIR/instruction_generated.rs
//! and is included! from src/bytecode/instruction.rs.

use bytecode_def::{
    Field, OpDef, STRUCT_ALIGN, compute_layouts, field_type_info, find_m_length_offset, round_up, user_fields,
};
use std::env;
use std::fs;
use std::io::Write;
use std::path::PathBuf;

fn rust_field_name(name: &str) -> String {
    if let Some(stripped) = name.strip_prefix("m_") {
        stripped.to_string()
    } else {
        name.to_string()
    }
}

/// Find the count field corresponding to a given array field, using the same
/// heuristic as the Python generator: prefer `<name>_count`, fall back to
/// `<singularized name>_count`. Panics if no matching u32/size_t field is
/// found, mirroring the Python `find_count_field_name_or_die`.
fn find_count_field_name(op: &OpDef, array_field: &Field) -> String {
    let mut candidates = vec![format!("{}_count", array_field.name)];
    if let Some(stripped) = array_field.name.strip_suffix('s') {
        candidates.push(format!("{stripped}_count"));
    }
    for candidate in &candidates {
        for f in &op.fields {
            if f.is_array {
                continue;
            }
            if &f.name == candidate && (f.ty == "u32" || f.ty == "size_t") {
                return candidate.clone();
            }
        }
    }
    panic!(
        "No count field (u32/size_t) found for array field '{}' in op '{}'",
        array_field.name, op.name
    );
}

fn generate_rust_code(mut w: impl Write, ops: &[OpDef]) -> Result<(), Box<dyn std::error::Error>> {
    writeln!(w, "// @generated from Libraries/LibJS/Bytecode/Bytecode.def")?;
    writeln!(w, "// Do not edit manually.")?;
    writeln!(w)?;
    writeln!(w, "use super::operand::*;")?;
    writeln!(w)?;

    generate_opcode_enum(&mut w, ops)?;
    generate_num_opcodes_const(&mut w, ops)?;
    generate_instruction_enum(&mut w, ops)?;
    generate_instruction_impl(&mut w, ops)?;
    generate_instruction_length_from_bytes(&mut w, ops)?;
    generate_validate_instruction(&mut w, ops)?;

    Ok(())
}

fn generate_num_opcodes_const(mut w: impl Write, ops: &[OpDef]) -> Result<(), Box<dyn std::error::Error>> {
    writeln!(w, "/// Number of distinct opcodes (the valid range for the type byte).")?;
    writeln!(w, "pub const NUM_OPCODES: u32 = {};", ops.len())?;
    writeln!(w)?;
    Ok(())
}

fn generate_instruction_length_from_bytes(mut w: impl Write, ops: &[OpDef]) -> Result<(), Box<dyn std::error::Error>> {
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
        "pub fn instruction_length_from_bytes(opcode: u8, bytes: &[u8], at: usize) -> Result<usize, super::validator::ValidationErrorKind> {{"
    )?;
    writeln!(w, "    use super::validator::ValidationErrorKind;")?;
    writeln!(w, "    match opcode {{")?;

    for (i, op) in ops.iter().enumerate() {
        let has_array = op.fields.iter().any(|f| f.is_array);

        if !has_array {
            let mut offset: usize = 2;
            for f in &op.fields {
                if f.is_array || f.name == "m_type" || f.name == "m_strict" {
                    continue;
                }
                let info = field_type_info(&f.ty);
                offset = round_up(offset, info.align);
                offset += info.size;
            }
            let final_size = round_up(offset, STRUCT_ALIGN);
            let op_name = &op.name;
            writeln!(w, "        {i} => Ok({final_size}), // {op_name}")?;
        } else {
            let mut fixed_offset: usize = 2;
            for f in &op.fields {
                if f.is_array || f.name == "m_type" || f.name == "m_strict" {
                    continue;
                }
                let info = field_type_info(&f.ty);
                fixed_offset = round_up(fixed_offset, info.align);
                fixed_offset += info.size;
            }
            let minimum_length = round_up(fixed_offset, STRUCT_ALIGN);
            let m_length_offset = find_m_length_offset(&op.fields);
            let op_name = &op.name;
            writeln!(w, "        {i} => {{ // {op_name} (variable-length)")?;
            writeln!(w, "            let m_length_end = at + {m_length_offset} + 4;")?;
            writeln!(w, "            if m_length_end > bytes.len() {{")?;
            writeln!(
                w,
                "                return Err(ValidationErrorKind::TruncatedInstruction);"
            )?;
            writeln!(w, "            }}")?;
            writeln!(
                w,
                "            let raw = u32::from_ne_bytes(bytes[at + {m_length_offset}..m_length_end].try_into().unwrap());"
            )?;
            writeln!(w, "            if raw < {minimum_length} {{")?;
            writeln!(w, "                return Err(ValidationErrorKind::InvalidLength);")?;
            writeln!(w, "            }}")?;
            writeln!(w, "            Ok(raw as usize)")?;
            writeln!(w, "        }}")?;
        }
    }

    writeln!(w, "        _ => Err(ValidationErrorKind::UnknownOpcode),")?;
    writeln!(w, "    }}")?;
    writeln!(w, "}}")?;
    writeln!(w)?;
    Ok(())
}

fn emit_scalar_field_check(
    mut w: impl Write,
    field_name: &str,
    ty: &str,
    offset: usize,
) -> Result<(), Box<dyn std::error::Error>> {
    match ty {
        "Operand" => writeln!(w, "            validate_operand(read_u32(bytes, at + {offset}), ctx)?;")?,
        "Optional<Operand>" => writeln!(
            w,
            "            validate_optional_operand(read_u32(bytes, at + {offset}), ctx)?;"
        )?,
        "Label" => writeln!(w, "            validate_label(read_u32(bytes, at + {offset}), ctx)?;")?,
        "Optional<Label>" => {
            // Encoded as 8 bytes: u32 value + u8 has_value + 3 bytes pad.
            writeln!(w, "            if bytes[at + {offset} + 4] != 0 {{")?;
            writeln!(
                w,
                "                validate_label(read_u32(bytes, at + {offset}), ctx)?;"
            )?;
            writeln!(w, "            }}")?;
        }
        "IdentifierTableIndex" => writeln!(
            w,
            "            validate_identifier_index(read_u32(bytes, at + {offset}), ctx)?;"
        )?,
        "Optional<IdentifierTableIndex>" => writeln!(
            w,
            "            validate_optional_identifier_index(read_u32(bytes, at + {offset}), ctx)?;"
        )?,
        "StringTableIndex" => writeln!(
            w,
            "            validate_string_index(read_u32(bytes, at + {offset}), ctx)?;"
        )?,
        "Optional<StringTableIndex>" => writeln!(
            w,
            "            validate_optional_string_index(read_u32(bytes, at + {offset}), ctx)?;"
        )?,
        "PropertyKeyTableIndex" => writeln!(
            w,
            "            validate_property_key_index(read_u32(bytes, at + {offset}), ctx)?;"
        )?,
        "RegexTableIndex" => {
            // The regex table is not consulted at runtime; skip range-checking.
        }
        "PropertyLookupCache*" => writeln!(
            w,
            "            validate_property_lookup_cache_index(read_u64(bytes, at + {offset}), ctx)?;"
        )?,
        "GlobalVariableCache*" => writeln!(
            w,
            "            validate_global_variable_cache_index(read_u64(bytes, at + {offset}), ctx)?;"
        )?,
        "TemplateObjectCache*" => writeln!(
            w,
            "            validate_template_object_cache_index(read_u64(bytes, at + {offset}), ctx)?;"
        )?,
        "ObjectShapeCache*" => writeln!(
            w,
            "            validate_object_shape_cache_index(read_u64(bytes, at + {offset}), ctx)?;"
        )?,
        "ObjectPropertyIteratorCache*" => writeln!(
            w,
            "            validate_object_property_iterator_cache_index(read_u64(bytes, at + {offset}), ctx)?;"
        )?,
        "u32" => {
            // The .def gives us no first-class types for SFD, class-blueprint,
            // or object-shape cache references stored as u32. Recognize the
            // canonical field names so these still get range-checked.
            if field_name == "m_shared_function_data_index" {
                writeln!(
                    w,
                    "            validate_shared_function_data_index(read_u32(bytes, at + {offset}), ctx)?;"
                )?;
            } else if field_name == "m_class_blueprint_index" {
                writeln!(
                    w,
                    "            validate_class_blueprint_index(read_u32(bytes, at + {offset}), ctx)?;"
                )?;
            } else if field_name == "m_shape_cache_index" {
                writeln!(
                    w,
                    "            validate_object_shape_cache_index(read_u32(bytes, at + {offset}) as u64, ctx)?;"
                )?;
            }
        }
        // bool, u64, Value, EnvironmentCoordinate, Builtin, Completion::Type,
        // IteratorHint, EnvironmentMode, PutKind, ArgumentsKind: not validated
        // here (no useful per-instruction bound to apply, or covered in a
        // later commit).
        _ => {}
    }
    Ok(())
}

fn emit_array_elem_check(mut w: impl Write, ty: &str) -> Result<(), Box<dyn std::error::Error>> {
    match ty {
        "Operand" => writeln!(w, "                validate_operand(read_u32(bytes, __off), ctx)?;")?,
        "Optional<Operand>" => writeln!(
            w,
            "                validate_optional_operand(read_u32(bytes, __off), ctx)?;"
        )?,
        "Value" => {
            // Trailing Value array (NewPrimitiveArray): no per-element check;
            // the count was already bounded against the instruction length.
        }
        other => panic!("Array element type not supported: {other}"),
    }
    Ok(())
}

fn generate_validate_instruction(mut w: impl Write, ops: &[OpDef]) -> Result<(), Box<dyn std::error::Error>> {
    let layouts = compute_layouts(ops);

    writeln!(
        w,
        "/// Per-opcode field validation, dispatched by Pass 2 of the validator."
    )?;
    writeln!(
        w,
        "pub fn validate_instruction(opcode: u8, ctx: &super::validator::ValidationContext, at: usize) -> Result<(), super::validator::ValidationErrorKind> {{"
    )?;
    writeln!(w, "    use super::validator::*;")?;
    writeln!(w, "    let bytes = ctx.bytes;")?;
    writeln!(w, "    match opcode {{")?;

    for (i, op) in ops.iter().enumerate() {
        let layout = layouts.get(&op.name).expect("layout missing for op");
        let arrays: Vec<&Field> = op.fields.iter().filter(|f| f.is_array).collect();
        let has_array = !arrays.is_empty();

        // Map each count field's name to the array it sizes, so we can skip
        // the count field when emitting scalar checks (it gets read alongside
        // the array bound below) and avoid duplicate work.
        let mut count_field_names: std::collections::HashSet<String> = std::collections::HashSet::new();
        for af in &arrays {
            count_field_names.insert(find_count_field_name(op, af));
        }

        let op_name = &op.name;
        writeln!(w, "        {i} => {{ // {op_name}")?;

        for f in &op.fields {
            if f.is_array {
                continue;
            }
            if f.name == "m_type" || f.name == "m_strict" || f.name == "m_length" {
                continue;
            }
            if count_field_names.contains(&f.name) {
                continue;
            }
            let offset = *layout.field_offsets.get(&f.name).expect("missing field offset");
            emit_scalar_field_check(&mut w, &f.name, &f.ty, offset)?;
        }

        if has_array {
            let m_length_offset = *layout
                .field_offsets
                .get("m_length")
                .expect("variable-length op missing m_length");
            // All variable-length ops in Bytecode.def carry exactly one trailing
            // array; if that ever changes, this loop validates each independently.
            for af in &arrays {
                let array_offset = *layout.field_offsets.get(&af.name).expect("missing array offset");
                let count_field = find_count_field_name(op, af);
                let count_offset = *layout
                    .field_offsets
                    .get(&count_field)
                    .expect("missing count field offset");
                let elem_size = field_type_info(&af.ty).size;

                writeln!(
                    w,
                    "            let __m_length = read_u32(bytes, at + {m_length_offset}) as usize;"
                )?;
                writeln!(
                    w,
                    "            let __count = read_u32(bytes, at + {count_offset}) as usize;"
                )?;
                writeln!(
                    w,
                    "            let __array_bytes = __count.saturating_mul({elem_size});"
                )?;
                writeln!(
                    w,
                    "            if {array_offset}usize.saturating_add(__array_bytes) > __m_length {{"
                )?;
                writeln!(w, "                return Err(ValidationErrorKind::InvalidLength);")?;
                writeln!(w, "            }}")?;
                writeln!(w, "            let __array_off = at + {array_offset};")?;
                writeln!(w, "            for __i in 0..__count {{")?;
                writeln!(w, "                let __off = __array_off + __i * {elem_size};")?;
                emit_array_elem_check(&mut w, &af.ty)?;
                writeln!(w, "            }}")?;
            }
        }

        writeln!(w, "        }}")?;
    }

    writeln!(w, "        _ => {{}}")?;
    writeln!(w, "    }}")?;
    writeln!(w, "    Ok(())")?;
    writeln!(w, "}}")?;
    writeln!(w)?;

    Ok(())
}

fn generate_opcode_enum(mut w: impl Write, ops: &[OpDef]) -> Result<(), Box<dyn std::error::Error>> {
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

fn generate_instruction_enum(mut w: impl Write, ops: &[OpDef]) -> Result<(), Box<dyn std::error::Error>> {
    writeln!(w, "/// A bytecode instruction with typed fields.")?;
    writeln!(w, "///")?;
    writeln!(w, "/// Each variant corresponds to one C++ instruction class.")?;
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

fn generate_instruction_impl(mut w: impl Write, ops: &[OpDef]) -> Result<(), Box<dyn std::error::Error>> {
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

fn generate_opcode_method(mut w: impl Write, ops: &[OpDef]) -> Result<(), Box<dyn std::error::Error>> {
    writeln!(w, "    pub fn opcode(&self) -> OpCode {{")?;
    writeln!(w, "        match self {{")?;
    for op in ops {
        let fields = user_fields(op);
        if fields.is_empty() {
            writeln!(w, "            Instruction::{} => OpCode::{},", op.name, op.name)?;
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

fn generate_is_terminator_method(mut w: impl Write, ops: &[OpDef]) -> Result<(), Box<dyn std::error::Error>> {
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

fn generate_encoded_size_method(mut w: impl Write, ops: &[OpDef]) -> Result<(), Box<dyn std::error::Error>> {
    writeln!(w, "    /// Returns the encoded size of this instruction in bytes.")?;
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
                let info = field_type_info(&f.ty);
                offset = round_up(offset, info.align);
                offset += info.size;
            }
            let final_size = round_up(offset, 8);
            let pat = if fields.is_empty() {
                format!("Instruction::{}", op.name)
            } else {
                format!("Instruction::{} {{ .. }}", op.name)
            };
            writeln!(w, "            {pat} => {final_size},")?;
        } else {
            // Variable-length: depends on array size
            // Compute fixed part size
            let mut fixed_offset: usize = 2;
            for f in &op.fields {
                if f.is_array || f.name == "m_type" || f.name == "m_strict" {
                    continue;
                }
                let info = field_type_info(&f.ty);
                fixed_offset = round_up(fixed_offset, info.align);
                fixed_offset += info.size;
            }

            // Find the array field and its element size
            let Some(array_field) = op.fields.iter().find(|f| f.is_array) else {
                continue;
            };
            let info = field_type_info(&array_field.ty);
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
                sizeof_this, arr_name, info.size
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

fn generate_encode_method(mut w: impl Write, ops: &[OpDef]) -> Result<(), Box<dyn std::error::Error>> {
    writeln!(
        w,
        "    /// Encode this instruction into bytes matching the C++ struct layout."
    )?;
    writeln!(w, "    pub fn encode(&self, strict: bool, buf: &mut Vec<u8>) {{")?;
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

            let info = field_type_info(&f.ty);

            // Pad to alignment
            let aligned_offset = round_up(offset, info.align);
            let pad = aligned_offset - offset;
            if pad > 0 {
                writeln!(w, "                buf.extend_from_slice(&[0u8; {pad}]);")?;
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
                emit_field_write(&mut w, &rname, info.kind, false)?;
            }
            offset += info.size;
        }

        // Write trailing array elements
        if has_array {
            // sizeof(*this) in C++ = round_up(fixed_offset, STRUCT_ALIGN)
            let sizeof_this = round_up(offset, STRUCT_ALIGN);

            for f in &op.fields {
                if !f.is_array {
                    continue;
                }
                let info = field_type_info(&f.ty);
                let rname = rust_field_name(&f.name);

                // Pad before first element if needed
                let aligned_offset = round_up(offset, info.align);
                let pad = aligned_offset - offset;
                if pad > 0 {
                    writeln!(w, "                buf.extend_from_slice(&[0u8; {pad}]);")?;
                }

                writeln!(w, "                for item in {rname} {{")?;
                emit_field_write(&mut w, "item", info.kind, true)?;
                writeln!(w, "                }}")?;

                // Compute target size matching C++:
                //   round_up(STRUCT_ALIGN, sizeof(*this) + count * elem_size)
                writeln!(
                    w,
                    "                let target = ({} + {}.len() * {} + 7) & !7;",
                    sizeof_this, rname, info.size
                )?;
                writeln!(
                    w,
                    "                while (buf.len() - start) < target {{ buf.push(0); }}"
                )?;
            }

            if has_m_length {
                // Patch m_length: it's the first u32 field after the header
                let m_length_offset = find_m_length_offset(&op.fields);
                writeln!(w, "                let total_len = (buf.len() - start) as u32;")?;
                writeln!(
                    w,
                    "                buf[start + {}..start + {}].copy_from_slice(&total_len.to_ne_bytes());",
                    m_length_offset,
                    m_length_offset + 4
                )?;
            }
        } else {
            // Fixed-length: pad statically
            let final_size = round_up(offset, 8);
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
fn emit_field_write(
    mut w: impl Write,
    name: &str,
    kind: &str,
    is_loop_item: bool,
) -> Result<(), Box<dyn std::error::Error>> {
    let prefix = " ".repeat(if is_loop_item { 20 } else { 16 });
    match kind {
        "bool" => writeln!(w, "{prefix}buf.push(*{name} as u8);")?,
        "u8" => writeln!(w, "{prefix}buf.push(*{name});")?,
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
            // C++ Optional<Label> layw: u32 value, bool has_value, 3 bytes padding = 8 bytes total
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

fn generate_visit_operands_method(mut w: impl Write, ops: &[OpDef]) -> Result<(), Box<dyn std::error::Error>> {
    writeln!(w, "    /// Visit all `Operand` fields (for operand rewriting).")?;
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
            writeln!(w, "            {pat} => {{}}")?;
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

        for f in &operand_fields {
            let rname = rust_field_name(&f.name);
            if f.is_array {
                if f.ty == "Optional<Operand>" {
                    writeln!(
                        w,
                        "                for op in {rname}.iter_mut().flatten() {{ visitor(op); }}"
                    )?;
                } else {
                    writeln!(w, "                for item in {rname}.iter_mut() {{ visitor(item); }}")?;
                }
            } else if f.ty == "Optional<Operand>" {
                writeln!(w, "                if let Some(op) = {rname} {{ visitor(op); }}")?;
            } else {
                writeln!(w, "                visitor({rname});")?;
            }
        }

        writeln!(w, "            }}")?;
    }

    writeln!(w, "        }}")?;
    writeln!(w, "    }}")?;
    writeln!(w)?;

    Ok(())
}

fn generate_visit_labels_method(mut w: impl Write, ops: &[OpDef]) -> Result<(), Box<dyn std::error::Error>> {
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
            writeln!(w, "            {pat} => {{}}")?;
            continue;
        }

        let bindings: Vec<String> = fields
            .iter()
            .map(|f| {
                let rname = rust_field_name(&f.name);
                if f.ty == "Label" || f.ty == "Optional<Label>" {
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

        for f in &label_fields {
            let rname = rust_field_name(&f.name);
            if f.is_array {
                if f.ty == "Optional<Label>" {
                    writeln!(w, "                for item in {rname}.iter_mut() {{")?;
                    writeln!(w, "                    if let Some(lbl) = item {{ visitor(lbl); }}")?;
                    writeln!(w, "                }}")?;
                } else {
                    writeln!(w, "                for item in {rname}.iter_mut() {{ visitor(item); }}")?;
                }
            } else if f.ty == "Optional<Label>" {
                writeln!(w, "                if let Some(lbl) = {rname} {{ visitor(lbl); }}")?;
            } else {
                writeln!(w, "                visitor({rname});")?;
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
    println!("cargo:rerun-if-changed=cbindgen.toml");
    println!("cargo:rerun-if-env-changed=FFI_OUTPUT_DIR");
    println!("cargo:rerun-if-changed=src");

    let out_dir = PathBuf::from(env::var("OUT_DIR")?);

    let ffi_out_dir = env::var("FFI_OUTPUT_DIR")
        .map(PathBuf::from)
        .unwrap_or_else(|_| out_dir.clone());

    cbindgen::generate(manifest_dir).map_or_else(
        |error| match error {
            cbindgen::Error::ParseSyntaxError { .. } => {}
            e => panic!("{e:?}"),
        },
        |bindings| {
            let header_path = out_dir.join("RustFFI.h");
            bindings.write_to_file(&header_path);

            if ffi_out_dir != out_dir {
                bindings.write_to_file(ffi_out_dir.join("RustFFI.h"));
            }
        },
    );

    let file = fs::OpenOptions::new()
        .write(true)
        .create(true)
        .truncate(true) // empties contents of the file
        .open(out_dir.join("instruction_generated.rs"))?;
    let content = fs::read_to_string(&def_path).expect("Failed to read Bytecode.def");
    let ops = bytecode_def::parse_bytecode_def(&content);
    generate_rust_code(file, &ops)
}
