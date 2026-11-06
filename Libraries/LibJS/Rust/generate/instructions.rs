/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Generates Rust bytecode instruction types from Flap.
//!
//! The generated code lives in $OUT_DIR/instruction_generated.rs and is
//! included! from src/bytecode/instruction.rs.

use flapc::metadata::Field;
use flapc::metadata::InstructionDefinition;
use flapc::metadata::Specialization;
use flapc::metadata::SpecializationConstraint;
use flapc::metadata::SpecializedInstruction;
use flapc::metadata::SpecializedParameterBinding;
use flapc::metadata::compute_layouts;
use flapc::metadata::field_type_info;
use flapc::metadata::user_fields;
use flapc::rust_bytecode::InstructionSet;
use flapc::rust_bytecode::count_field_name;
use flapc::rust_bytecode::generate_encode_method;
use flapc::rust_bytecode::generate_field_reads;
use flapc::rust_bytecode::generate_instruction_enum;
use flapc::rust_bytecode::generate_instruction_is_terminator_from_opcode;
use flapc::rust_bytecode::generate_instruction_length_from_bytes;
use flapc::rust_bytecode::generate_instruction_name_from_opcode;
use flapc::rust_bytecode::generate_num_opcodes_const;
use flapc::rust_bytecode::generate_opcode_enum;
use flapc::rust_bytecode::generate_typed_instructions;
use flapc::rust_bytecode::rust_field_name;
use std::fs;
use std::io::Write;
use std::path::Path;

/// Path prefix of the `read_u32` / `read_u64` helpers used by generated field reads.
const READER: &str = "super::operand::";

fn generate_rust_code(
    mut w: impl Write,
    ops: &[InstructionDefinition],
    specializations: &[Specialization],
    specialized_ops: &[SpecializedInstruction],
) -> Result<(), Box<dyn std::error::Error>> {
    writeln!(w, "// @generated from Libraries/LibJS/Interpreter/interpreter.flap")?;
    writeln!(w, "// Do not edit manually.")?;
    writeln!(w)?;
    writeln!(w, "use super::operand::*;")?;
    writeln!(w)?;

    generate_opcode_enum(&mut w, ops)?;
    generate_num_opcodes_const(&mut w, ops)?;
    generate_instruction_enum(&mut w, ops, "Debug, Clone")?;
    generate_instruction_impl(&mut w, ops)?;
    generate_instruction_length_from_bytes(&mut w, ops, "super::operand::InstructionLengthError")?;
    generate_instruction_name_from_opcode(&mut w, ops)?;
    generate_instruction_feedback_slots_from_bytes(&mut w, ops)?;
    generate_instruction_dump_from_bytes(&mut w, ops)?;
    generate_visit_labels_from_bytes(&mut w, ops)?;
    generate_instruction_is_terminator_from_opcode(&mut w, ops)?;
    generate_specialization_selector(&mut w, ops, specializations, specialized_ops)?;
    generate_typed_instructions(
        &mut w,
        ops,
        "typed",
        "crate::bytecode::codegen::emitter::BlockEncoder<'_>",
        &|name| specializable_operands(name, specializations),
    )?;

    Ok(())
}

/// The operands of the instruction called `name` that a specialization looks at.
fn specializable_operands(name: &str, specializations: &[Specialization]) -> Vec<String> {
    let mut operands = Vec::new();
    for specialization in specializations {
        if name != "Mov" && specialization.bytecode == name {
            for parameter in &specialization.parameters {
                let operand = rust_field_name(&parameter.name);
                if !operands.contains(&operand) {
                    operands.push(operand);
                }
            }
        }
    }
    operands
}

/// Emits `instruction_feedback_slots_from_bytes()`, which reads the feedback slot indices of the instruction at
/// `bytes[at..]`, one per feedback kind it records.
fn generate_instruction_feedback_slots_from_bytes(
    mut w: impl Write,
    ops: &[InstructionDefinition],
) -> Result<(), Box<dyn std::error::Error>> {
    const KINDS: [&str; 4] = [
        "ArithFeedbackIndex",
        "ValueFeedbackIndex",
        "CallFeedbackIndex",
        "KeyedFeedbackIndex",
    ];
    writeln!(
        w,
        "/// The feedback slot indices of the instruction at `bytes[at..]`: arith, value, call and keyed."
    )?;
    writeln!(
        w,
        "pub fn instruction_feedback_slots_from_bytes(opcode: u8, bytes: &[u8], at: usize) -> [Option<u16>; 4] {{"
    )?;
    writeln!(w, "    match opcode {{")?;
    for (i, op) in ops.iter().enumerate() {
        let slots = KINDS
            .iter()
            .map(|kind| {
                user_fields(op).into_iter().find(|field| field.ty == *kind).map_or_else(
                    || "None".to_string(),
                    |field| {
                        let offset = op.layout.field_offsets[&field.name];
                        format!("Some({READER}read_u16(bytes, at + {offset}))")
                    },
                )
            })
            .collect::<Vec<_>>();
        if slots.iter().all(|slot| slot == "None") {
            continue;
        }
        writeln!(w, "        {i} => [{}], // {}", slots.join(", "), op.name)?;
    }
    writeln!(w, "        _ => [None; 4],")?;
    writeln!(w, "    }}")?;
    writeln!(w, "}}")?;
    writeln!(w)?;
    Ok(())
}

fn generate_specialization_selector(
    mut w: impl Write,
    ops: &[InstructionDefinition],
    specializations: &[Specialization],
    specialized_ops: &[SpecializedInstruction],
) -> Result<(), Box<dyn std::error::Error>> {
    let mut order = (0..specializations.len()).collect::<Vec<_>>();
    order.sort_by_key(|index| std::cmp::Reverse(specializations[*index].parameters.len()));

    writeln!(w, "#[allow(unused_variables)]")?;
    writeln!(w, "#[allow(clippy::clone_on_copy)]")?;
    writeln!(w, "#[allow(clippy::collapsible_if)]")?;
    writeln!(w, "#[allow(clippy::redundant_clone)]")?;
    writeln!(w, "#[allow(clippy::unnecessary_unwrap)]")?;
    writeln!(
        w,
        "pub fn specialize_instruction(instruction: &Instruction, constants: &[super::executable_data::ConstantValue]) -> Option<Instruction> {{"
    )?;
    for index in order {
        let specialization = &specializations[index];
        let specialized_op = &specialized_ops[index];
        let op = &specialization.bytecode;
        let base_op = ops
            .iter()
            .find(|definition| definition.name == *op)
            .expect("specialization bytecodes are validated");
        let bindings = user_fields(base_op)
            .iter()
            .map(|field| rust_field_name(&field.name))
            .collect::<Vec<_>>();
        writeln!(
            w,
            "    if let Instruction::{op} {{ {} }} = instruction {{",
            bindings.join(", ")
        )?;

        let mut conditions = Vec::new();
        let mut extracted_values = Vec::new();
        for parameter in &specialization.parameters {
            let operand = &parameter.name;
            match &parameter.constraint {
                SpecializationConstraint::Int32 => {
                    let value = format!("specialized_{operand}");
                    writeln!(
                        w,
                        "            let {value} = constants.get({operand}.index() as usize).and_then(|constant| match constant {{"
                    )?;
                    writeln!(
                        w,
                        "                super::executable_data::ConstantValue::Number(number) if number.fract() == 0.0 && *number >= i32::MIN as f64 && *number <= i32::MAX as f64 && number.to_bits() != (-0.0_f64).to_bits() => Some(*number as i32),"
                    )?;
                    writeln!(w, "                _ => None,")?;
                    writeln!(w, "            }});")?;
                    conditions.push(format!("{operand}.is_constant() && {value}.is_some()"));
                    extracted_values.push(value);
                }
                SpecializationConstraint::Undefined => {
                    conditions.push(format!(
                        "{operand}.is_constant() && matches!(constants.get({operand}.index() as usize), Some(super::executable_data::ConstantValue::Undefined))"
                    ));
                }
            }
        }

        if !conditions.is_empty() {
            writeln!(w, "            if {} {{", conditions.join(" && "))?;
        }
        for value in &extracted_values {
            writeln!(w, "                let {value} = {value}.unwrap();")?;
        }
        writeln!(
            w,
            "                return Some(Instruction::{} {{",
            specialized_op.definition.name
        )?;
        for field in &specialized_op.definition.fields {
            let field_name = rust_field_name(&field.name);
            let parameter = specialized_op
                .parameters
                .iter()
                .find_map(|(parameter, binding)| {
                    (binding == &SpecializedParameterBinding::Field(field_name.clone())).then_some(parameter)
                })
                .expect("every specialized field has a source");
            let is_int32 = specialization.parameters.iter().any(|specialized| {
                &specialized.name == parameter && matches!(specialized.constraint, SpecializationConstraint::Int32)
            });
            let source = if is_int32 {
                format!("specialized_{parameter}")
            } else {
                format!("(*{parameter}).clone()")
            };
            writeln!(w, "                    {field_name}: {source},")?;
        }
        writeln!(w, "                }});")?;
        if !conditions.is_empty() {
            writeln!(w, "            }}")?;
        }
        writeln!(w, "    }}")?;
    }
    writeln!(w, "    None")?;
    writeln!(w, "}}")?;
    writeln!(w)?;
    Ok(())
}

fn generate_array_bounds(
    w: &mut impl Write,
    op: &InstructionDefinition,
    layouts: &std::collections::HashMap<String, flapc::metadata::OpLayout>,
) -> Result<(), Box<dyn std::error::Error>> {
    let layout = layouts.get(&op.name).expect("layout missing for op");
    for f in user_fields(op) {
        if !f.is_array {
            continue;
        }
        let rname = rust_field_name(&f.name);
        let count_name = rust_field_name(count_field_name(op, f));
        let offset = layout.field_offsets.get(&f.name).expect("array offset missing");
        let elem_size = field_type_info(&f.ty).size;
        writeln!(w, "            let {rname}_offset = at + {offset};")?;
        writeln!(w, "            let {rname}_count = {count_name} as usize;")?;
        writeln!(
            w,
            "            let {rname}_end = {rname}_offset + {rname}_count * {elem_size};"
        )?;
    }
    Ok(())
}

fn generate_instruction_dump_from_bytes(
    mut w: impl Write,
    ops: &[InstructionDefinition],
) -> Result<(), Box<dyn std::error::Error>> {
    let layouts = compute_layouts(ops);

    writeln!(w, "#[cfg(not(test))]")?;
    writeln!(w, "#[allow(unused_variables)]")?;
    writeln!(
        w,
        "pub fn dump_instruction_from_bytes(opcode: u8, bytes: &[u8], at: usize, dumper: &mut super::dump::BytecodeDumper<'_>) {{"
    )?;
    writeln!(w, "    match opcode {{")?;

    for (i, op) in ops.iter().enumerate() {
        if op.name == "Instruction" {
            continue;
        }
        writeln!(w, "        {i} => {{")?;
        generate_field_reads(&mut w, op, READER)?;
        generate_array_bounds(&mut w, op, &layouts)?;
        writeln!(w, "            dumper.begin_instruction(\"{}\");", op.name)?;

        let arrays: Vec<&Field> = op.fields.iter().filter(|f| f.is_array).collect();
        let mut array_to_count = std::collections::HashMap::new();
        let mut count_fields = std::collections::HashSet::new();
        for af in arrays {
            let count_field_name = count_field_name(op, af);
            count_fields.insert(count_field_name);
            array_to_count.insert(af.name.clone(), rust_field_name(count_field_name));
        }

        for f in &op.fields {
            if f.name == "m_length" || f.name == "m_cache" {
                continue;
            }

            let ty = f.ty.trim();
            let label = rust_field_name(&f.name);
            let rname = rust_field_name(&f.name);

            if f.is_array {
                let count_name = array_to_count.get(&f.name).expect("array count missing");
                match ty {
                    "Operand" => {
                        writeln!(w, "            if {count_name} != 0 {{")?;
                        writeln!(w, "                dumper.append_piece(|dumper| {{")?;
                        writeln!(
                            w,
                            "                    dumper.append_operand_list(\"{label}\", bytes, {rname}_offset, {rname}_count);"
                        )?;
                        writeln!(w, "                }});")?;
                        writeln!(w, "            }}")?;
                    }
                    "Optional<Operand>" => {
                        writeln!(w, "            if {count_name} != 0 {{")?;
                        writeln!(w, "                dumper.append_piece(|dumper| {{")?;
                        writeln!(
                            w,
                            "                    dumper.append_optional_operand_list(\"{label}\", bytes, {rname}_offset, {rname}_count);"
                        )?;
                        writeln!(w, "                }});")?;
                        writeln!(w, "            }}")?;
                    }
                    "Value" => {
                        writeln!(w, "            if {count_name} != 0 {{")?;
                        writeln!(w, "                dumper.append_piece(|dumper| {{")?;
                        writeln!(
                            w,
                            "                    dumper.append_value_list(\"{label}\", bytes, {rname}_offset, {rname}_count);"
                        )?;
                        writeln!(w, "                }});")?;
                        writeln!(w, "            }}")?;
                    }
                    "Label" => {
                        writeln!(w, "            if {count_name} != 0 {{")?;
                        writeln!(w, "                dumper.append_piece(|dumper| {{")?;
                        writeln!(
                            w,
                            "                    dumper.append_label_list(\"{label}\", bytes, {rname}_offset, {rname}_count);"
                        )?;
                        writeln!(w, "                }});")?;
                        writeln!(w, "            }}")?;
                    }
                    "Optional<Label>" => {
                        writeln!(w, "            if {count_name} != 0 {{")?;
                        writeln!(w, "                dumper.append_piece(|dumper| {{")?;
                        writeln!(
                            w,
                            "                    dumper.append_optional_label_list(\"{label}\", bytes, {rname}_offset, {rname}_count);"
                        )?;
                        writeln!(w, "                }});")?;
                        writeln!(w, "            }}")?;
                    }
                    _ => {}
                }
                continue;
            }

            match ty {
                "Operand" => writeln!(
                    w,
                    "            dumper.append_piece(|dumper| dumper.append_operand(\"{label}\", {rname}));"
                )?,
                "Optional<Operand>" => {
                    writeln!(w, "            if let Some({rname}) = {rname} {{")?;
                    writeln!(
                        w,
                        "                dumper.append_piece(|dumper| dumper.append_operand(\"{label}\", {rname}));"
                    )?;
                    writeln!(w, "            }}")?;
                }
                "Label" => writeln!(
                    w,
                    "            dumper.append_piece(|dumper| dumper.append_label(\"{label}\", {rname}.0));"
                )?,
                "Optional<Label>" => {
                    writeln!(w, "            if let Some({rname}) = {rname} {{")?;
                    writeln!(
                        w,
                        "                dumper.append_piece(|dumper| dumper.append_label(\"{label}\", {rname}.0));"
                    )?;
                    writeln!(w, "            }}")?;
                }
                "PropertyKeyTableIndex" => {
                    writeln!(
                        w,
                        "            dumper.append_piece(|dumper| dumper.append_property_key_quoted({rname}.0));"
                    )?;
                }
                "IdentifierTableIndex" => {
                    writeln!(
                        w,
                        "            dumper.append_piece(|dumper| dumper.append_identifier_quoted({rname}.0));"
                    )?;
                }
                "Optional<IdentifierTableIndex>" => {
                    let mut property_key_field = None;
                    let mut property_operand_field = None;
                    for other in &op.fields {
                        if other.ty.trim() == "PropertyKeyTableIndex" {
                            property_key_field = Some(rust_field_name(&other.name));
                            break;
                        }
                        if other.ty.trim() == "Operand" && other.name == "m_property" {
                            property_operand_field = Some(rust_field_name(&other.name));
                            break;
                        }
                    }

                    writeln!(w, "            if let Some({rname}) = {rname} {{")?;
                    if let Some(property_key_field) = property_key_field {
                        writeln!(w, "                dumper.append(\" \\u{{1b}}[37;1m(\");")?;
                        writeln!(w, "                dumper.append_identifier_plain({rname}.0);")?;
                        writeln!(w, "                dumper.append(\".\");")?;
                        writeln!(
                            w,
                            "                dumper.append_property_key_plain({property_key_field}.0);"
                        )?;
                        writeln!(w, "                dumper.append(\")\\u{{1b}}[0m\");")?;
                    } else if let Some(property_operand_field) = property_operand_field {
                        writeln!(w, "                dumper.append(\" \\u{{1b}}[37;1m(\");")?;
                        writeln!(w, "                dumper.append_identifier_plain({rname}.0);")?;
                        writeln!(w, "                dumper.append(\"[\\u{{1b}}[0m\");")?;
                        writeln!(
                            w,
                            "                dumper.append_operand(\"\", {property_operand_field});"
                        )?;
                        writeln!(w, "                dumper.append(\"\\u{{1b}}[37;1m])\\u{{1b}}[0m\");")?;
                    } else if op.name == "GetLength" {
                        writeln!(w, "                dumper.append(\" \\u{{1b}}[37;1m(\");")?;
                        writeln!(w, "                dumper.append_identifier_plain({rname}.0);")?;
                        writeln!(w, "                dumper.append(\".length)\\u{{1b}}[0m\");")?;
                    } else {
                        writeln!(w, "                dumper.append(\" \\u{{1b}}[37;1m(\");")?;
                        writeln!(w, "                dumper.append_identifier_plain({rname}.0);")?;
                        writeln!(w, "                dumper.append(\")\\u{{1b}}[0m\");")?;
                    }
                    writeln!(w, "            }}")?;
                }
                "StringTableIndex" => writeln!(
                    w,
                    "            dumper.append_piece(|dumper| dumper.append_string({rname}.0));"
                )?,
                "Optional<StringTableIndex>" => {
                    writeln!(w, "            if let Some({rname}) = {rname} {{")?;
                    writeln!(
                        w,
                        "                dumper.append_piece(|dumper| dumper.append_string({rname}.0));"
                    )?;
                    writeln!(w, "            }}")?;
                }
                "bool" => writeln!(
                    w,
                    "            dumper.append_piece(|dumper| dumper.append_bool(\"{label}\", {rname}));"
                )?,
                "PutKind" => writeln!(
                    w,
                    "            dumper.append_piece(|dumper| dumper.append_put_kind(\"{label}\", {rname}));"
                )?,
                "ArithFeedbackIndex" | "ValueFeedbackIndex" | "CallFeedbackIndex" | "KeyedFeedbackIndex" => writeln!(
                    w,
                    "            dumper.append_piece(|dumper| dumper.append_number(\"{label}\", {rname}));"
                )?,
                _ if (ty == "i32" || ty == "u32" || ty == "u64" || ty == "u8")
                    && !count_fields.contains(f.name.as_str()) =>
                {
                    writeln!(
                        w,
                        "            dumper.append_piece(|dumper| dumper.append_number(\"{label}\", {rname}));"
                    )?;
                }
                _ => {}
            }
        }

        writeln!(w, "        }}")?;
    }

    writeln!(w, "        _ => unreachable!(\"unknown bytecode opcode\"),")?;
    writeln!(w, "    }}")?;
    writeln!(w, "}}")?;
    writeln!(w)?;
    Ok(())
}

fn generate_visit_labels_from_bytes(
    mut w: impl Write,
    ops: &[InstructionDefinition],
) -> Result<(), Box<dyn std::error::Error>> {
    writeln!(w, "#[allow(unused_variables)]")?;
    writeln!(
        w,
        "pub fn visit_labels_from_bytes(opcode: u8, bytes: &[u8], at: usize, visitor: &mut dyn FnMut(u32)) {{"
    )?;
    writeln!(w, "    match opcode {{")?;

    for (i, op) in ops.iter().enumerate() {
        let label_fields: Vec<&Field> = user_fields(op)
            .into_iter()
            .filter(|f| f.ty == "Label" || f.ty == "Optional<Label>")
            .collect();
        if label_fields.is_empty() {
            continue;
        }
        writeln!(w, "        {i} => {{")?;
        generate_field_reads(&mut w, op, READER)?;
        for f in label_fields {
            let rname = rust_field_name(&f.name);
            if f.ty == "Label" {
                writeln!(w, "            visitor({rname}.0);")?;
            } else {
                writeln!(
                    w,
                    "            if let Some({rname}) = {rname} {{ visitor({rname}.0); }}"
                )?;
            }
        }
        writeln!(w, "        }}")?;
    }

    writeln!(w, "        _ => {{}}")?;
    writeln!(w, "    }}")?;
    writeln!(w, "}}")?;
    writeln!(w)?;
    Ok(())
}

fn generate_instruction_impl(
    mut w: impl Write,
    ops: &[InstructionDefinition],
) -> Result<(), Box<dyn std::error::Error>> {
    writeln!(w, "impl Instruction {{").unwrap();
    generate_encode_method(
        &mut w,
        ops,
        "encode_into_block",
        "crate::bytecode::codegen::emitter::BlockEncoder<'_>",
    )?;
    generate_visit_labels_method(&mut w, ops)?;
    writeln!(w, "    }}")?;
    Ok(())
}

fn generate_visit_labels_method(
    mut w: impl Write,
    ops: &[InstructionDefinition],
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

pub fn generate(flap_path: &Path, out_dir: &Path) -> Result<(), Box<dyn std::error::Error>> {
    let file = fs::OpenOptions::new()
        .write(true)
        .create(true)
        .truncate(true) // empties contents of the file
        .open(out_dir.join("instruction_generated.rs"))?;
    let instruction_set = InstructionSet::load(flap_path)?;
    generate_rust_code(
        file,
        &instruction_set.ops,
        &instruction_set.specializations,
        &instruction_set.specialized_ops,
    )
}
