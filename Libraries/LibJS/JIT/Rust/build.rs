/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Build script that generates the JIT's bytecode decoder from Flap.
//!
//! The generated code lives in $OUT_DIR/decoder_generated.rs and is
//! included! from src/bytecode/instruction.rs. The instruction set loading,
//! the `Instruction` enum and the field readers are shared with
//! `Libraries/LibJS/Rust/build.rs` through `flapc::rust_bytecode`.

use flapc::metadata::Field;
use flapc::metadata::InstructionDefinition;
use flapc::metadata::ParameterMode;
use flapc::metadata::SlowPathAbi as FlapSlowPathAbi;
use flapc::metadata::SlowPathLayout;
use flapc::metadata::field_type_info;
use flapc::metadata::user_fields;
use flapc::rust_bytecode::InstructionSet;
use flapc::rust_bytecode::count_field_name;
use flapc::rust_bytecode::generate_encode_method;
use flapc::rust_bytecode::generate_encoded_size_method;
use flapc::rust_bytecode::generate_field_reads;
use flapc::rust_bytecode::generate_instruction_enum;
use flapc::rust_bytecode::generate_instruction_length_from_bytes;
use flapc::rust_bytecode::generate_is_terminator_method;
use flapc::rust_bytecode::generate_num_opcodes_const;
use flapc::rust_bytecode::generate_opcode_enum;
use flapc::rust_bytecode::generate_opcode_method;
use flapc::rust_bytecode::read_expr_for_type;
use flapc::rust_bytecode::rust_field_name;
use flapc::rust_bytecode::wildcard_pattern;
use std::env;
use std::error::Error;
use std::fs;
use std::io::BufWriter;
use std::io::Write;
use std::path::PathBuf;

type GeneratorResult = Result<(), Box<dyn Error>>;

/// The generated code is included into a module that defines `read_u32` and `read_u64`.
const READER: &str = "";

fn is_operand_type(ty: &str) -> bool {
    ty == "Operand" || ty == "Optional<Operand>"
}

fn is_label_type(ty: &str) -> bool {
    ty == "Label" || ty == "Optional<Label>"
}

/// Binds the fields selected by `bind` by name and ignores the others.
fn binding_pattern(op: &InstructionDefinition, bind: impl Fn(&Field) -> bool) -> String {
    let fields = user_fields(op);
    if !fields.iter().any(|field| bind(field)) {
        return wildcard_pattern(op);
    }
    let bindings = fields
        .iter()
        .map(|field| {
            let name = rust_field_name(&field.name);
            if bind(field) { name } else { format!("{name}: _") }
        })
        .collect::<Vec<_>>();
    format!("Instruction::{} {{ {} }}", op.name, bindings.join(", "))
}

fn generate_opcode_impl(mut w: impl Write, ops: &[InstructionDefinition]) -> GeneratorResult {
    writeln!(w, "impl OpCode {{")?;
    writeln!(w, "    /// The opcode with the given type byte, if there is one.")?;
    writeln!(w, "    pub fn from_u8(value: u8) -> Option<OpCode> {{")?;
    writeln!(w, "        match value {{")?;
    for (i, op) in ops.iter().enumerate() {
        writeln!(w, "            {i} => Some(OpCode::{}),", op.name)?;
    }
    writeln!(w, "            _ => None,")?;
    writeln!(w, "        }}")?;
    writeln!(w, "    }}")?;
    writeln!(w)?;
    writeln!(w, "    /// Whether instructions with this opcode end a basic block.")?;
    writeln!(w, "    pub fn is_terminator(self) -> bool {{")?;
    let terminators = ops
        .iter()
        .filter(|op| op.is_terminator)
        .map(|op| format!("OpCode::{}", op.name))
        .collect::<Vec<_>>();
    writeln!(w, "        matches!(self, {})", terminators.join(" | "))?;
    writeln!(w, "    }}")?;
    writeln!(w)?;
    writeln!(
        w,
        "    /// Whether control can continue with the next instruction: true for every"
    )?;
    writeln!(
        w,
        "    /// non-terminator and for terminators like `JumpTrue` that only sometimes jump."
    )?;
    writeln!(w, "    pub fn can_fall_through(self) -> bool {{")?;
    let enders = ops
        .iter()
        .filter(|op| op.is_terminator && !op.falls_through)
        .map(|op| format!("OpCode::{}", op.name))
        .collect::<Vec<_>>();
    writeln!(w, "        !matches!(self, {})", enders.join(" | "))?;
    writeln!(w, "    }}")?;
    writeln!(w)?;
    writeln!(w, "    pub fn name(self) -> &'static str {{")?;
    writeln!(w, "        match self {{")?;
    for op in ops {
        writeln!(w, "            OpCode::{0} => \"{0}\",", op.name)?;
    }
    writeln!(w, "        }}")?;
    writeln!(w, "    }}")?;
    writeln!(w, "}}")?;
    writeln!(w)?;
    Ok(())
}

fn generate_decode_function(mut w: impl Write, ops: &[InstructionDefinition]) -> GeneratorResult {
    writeln!(
        w,
        "/// Decodes the fields of the instruction with `opcode` at `bytes[at..at + length]`."
    )?;
    writeln!(
        w,
        "/// The caller has checked that the whole instruction lies within `bytes`."
    )?;
    writeln!(w, "#[allow(unused_variables)]")?;
    writeln!(
        w,
        "fn decode_fields(opcode: u8, bytes: &[u8], at: usize, length: usize) -> Result<Instruction, DecodeErrorKind> {{"
    )?;
    writeln!(w, "    match opcode {{")?;
    for (i, op) in ops.iter().enumerate() {
        let fields = user_fields(op);
        writeln!(w, "        {i} => {{ // {}", op.name)?;
        generate_field_reads(&mut w, op, READER)?;
        for field in fields.iter().filter(|field| field.is_array) {
            let name = rust_field_name(&field.name);
            let count_name = rust_field_name(count_field_name(op, field));
            let offset = op.layout.field_offsets[&field.name];
            let element_size = field_type_info(&field.ty).size;
            writeln!(w, "            let {name}_len = {count_name} as usize;")?;
            writeln!(
                w,
                "            let {name}_end = {name}_len.checked_mul({element_size}).and_then(|size| size.checked_add({offset})).ok_or(DecodeErrorKind::InvalidLength)?;"
            )?;
            writeln!(w, "            if {name}_end > length {{")?;
            writeln!(w, "                return Err(DecodeErrorKind::InvalidLength);")?;
            writeln!(w, "            }}")?;
            let element = read_expr_for_type(&field.ty, &format!("at + {offset} + index * {element_size}"), READER);
            writeln!(
                w,
                "            let {name} = (0..{name}_len).map(|index| {element}).collect::<Vec<_>>();"
            )?;
        }
        if fields.is_empty() {
            writeln!(w, "            Ok(Instruction::{})", op.name)?;
        } else {
            let names = fields
                .iter()
                .map(|field| rust_field_name(&field.name))
                .collect::<Vec<_>>();
            writeln!(w, "            Ok(Instruction::{} {{ {} }})", op.name, names.join(", "))?;
        }
        writeln!(w, "        }}")?;
    }
    writeln!(w, "        _ => Err(DecodeErrorKind::UnknownOpcode),")?;
    writeln!(w, "    }}")?;
    writeln!(w, "}}")?;
    writeln!(w)?;
    Ok(())
}

fn operand_role(mode: ParameterMode) -> &'static str {
    match mode {
        ParameterMode::In => "OperandRole::In",
        ParameterMode::Out => "OperandRole::Out",
        ParameterMode::InOut => "OperandRole::InOut",
    }
}

fn generate_for_each_operand_method(mut w: impl Write, ops: &[InstructionDefinition]) -> GeneratorResult {
    writeln!(
        w,
        "    /// Calls `visitor` with every operand of this instruction (skipping absent"
    )?;
    writeln!(
        w,
        "    /// optional operands) and whether the instruction reads it, writes it, or both."
    )?;
    writeln!(
        w,
        "    /// Operands are visited in field order, trailing array elements last."
    )?;
    writeln!(
        w,
        "    pub fn for_each_operand(&self, mut visitor: impl FnMut(Operand, OperandRole)) {{"
    )?;
    writeln!(w, "        match self {{")?;
    for op in ops {
        let pattern = binding_pattern(op, |field| is_operand_type(&field.ty));
        let operand_fields = user_fields(op)
            .into_iter()
            .filter(|field| is_operand_type(&field.ty))
            .collect::<Vec<_>>();
        if operand_fields.is_empty() {
            writeln!(w, "            {pattern} => {{}}")?;
            continue;
        }
        writeln!(w, "            {pattern} => {{")?;
        for field in operand_fields {
            let name = rust_field_name(&field.name);
            let role = operand_role(field.mode);
            match (field.is_array, field.ty == "Optional<Operand>") {
                (true, true) => writeln!(
                    w,
                    "                for operand in {name}.iter().flatten() {{ visitor(*operand, {role}); }}"
                )?,
                (true, false) => writeln!(
                    w,
                    "                for operand in {name} {{ visitor(*operand, {role}); }}"
                )?,
                (false, true) => writeln!(
                    w,
                    "                if let Some(operand) = {name} {{ visitor(*operand, {role}); }}"
                )?,
                (false, false) => writeln!(w, "                visitor(*{name}, {role});")?,
            }
        }
        writeln!(w, "            }}")?;
    }
    writeln!(w, "        }}")?;
    writeln!(w, "    }}")?;
    writeln!(w)?;
    Ok(())
}

fn generate_for_each_jump_target_method(mut w: impl Write, ops: &[InstructionDefinition]) -> GeneratorResult {
    writeln!(
        w,
        "    /// Calls `visitor` with every bytecode offset this instruction can transfer"
    )?;
    writeln!(
        w,
        "    /// control to, including the continuation of a suspending generator."
    )?;
    writeln!(
        w,
        "    pub fn for_each_jump_target(&self, mut visitor: impl FnMut(Label)) {{"
    )?;
    writeln!(w, "        match self {{")?;
    for op in ops {
        let pattern = binding_pattern(op, |field| is_label_type(&field.ty));
        let label_fields = user_fields(op)
            .into_iter()
            .filter(|field| is_label_type(&field.ty))
            .collect::<Vec<_>>();
        if label_fields.is_empty() {
            writeln!(w, "            {pattern} => {{}}")?;
            continue;
        }
        writeln!(w, "            {pattern} => {{")?;
        for field in label_fields {
            let name = rust_field_name(&field.name);
            match (field.is_array, field.ty == "Optional<Label>") {
                (true, true) => writeln!(
                    w,
                    "                for label in {name}.iter().flatten() {{ visitor(*label); }}"
                )?,
                (true, false) => writeln!(w, "                for label in {name} {{ visitor(*label); }}")?,
                (false, true) => writeln!(w, "                if let Some(label) = {name} {{ visitor(*label); }}")?,
                (false, false) => writeln!(w, "                visitor(*{name});")?,
            }
        }
        writeln!(w, "            }}")?;
    }
    writeln!(w, "        }}")?;
    writeln!(w, "    }}")?;
    writeln!(w)?;
    Ok(())
}

/// `record_form_only` is set for targets whose interpreter passes every slow path a record, which is what Windows
/// does.
fn generate_slow_path_layout_function(
    mut w: impl Write,
    ops: &[InstructionDefinition],
    record_form_only: bool,
) -> GeneratorResult {
    writeln!(
        w,
        "/// How the interpreter passes the operands of `opcode` to a slow path that"
    )?;
    writeln!(
        w,
        "/// takes its `Op` record (`JS_DECLARE_SLOW_PATH_<Op>` in the generated Op.h)."
    )?;
    writeln!(w, "pub fn slow_path_layout(opcode: OpCode) -> SlowPathLayout {{")?;
    writeln!(w, "    match opcode {{")?;
    for op in ops {
        let layout = SlowPathLayout::new(op);
        let abi = match layout.abi(record_form_only) {
            FlapSlowPathAbi::Scalar => "SlowPathAbi::Scalar",
            FlapSlowPathAbi::Mixed => "SlowPathAbi::ScalarInputs",
            FlapSlowPathAbi::Record => "SlowPathAbi::Record",
        };
        let fields = layout
            .fields
            .iter()
            .map(|field| {
                format!(
                    "SlowPathField {{ instruction_offset: {}, role: {}, optional: {} }}",
                    field.instruction_offset,
                    operand_role(field.mode),
                    field.optional
                )
            })
            .collect::<Vec<_>>()
            .join(", ");
        let array = match layout.array {
            Some(array) => format!(
                "Some(SlowPathArray {{ instruction_offset: {}, count_offset: {}, optional: {} }})",
                array.instruction_offset, array.count_offset, array.optional
            ),
            None => "None".to_string(),
        };
        writeln!(
            w,
            "        OpCode::{} => SlowPathLayout {{ abi: {abi}, fields: &[{fields}], array: {array} }},",
            op.name
        )?;
    }
    writeln!(w, "    }}")?;
    writeln!(w, "}}")?;
    writeln!(w)?;
    Ok(())
}

fn generate_feedback_slots_method(mut w: impl Write, ops: &[InstructionDefinition]) -> GeneratorResult {
    const KINDS: [(&str, &str); 4] = [
        ("ArithFeedbackIndex", "arith"),
        ("ValueFeedbackIndex", "value"),
        ("CallFeedbackIndex", "call"),
        ("KeyedFeedbackIndex", "keyed"),
    ];
    writeln!(
        w,
        "    /// The interpreter feedback slots this instruction records into."
    )?;
    writeln!(w, "    pub fn feedback_slots(&self) -> FeedbackSlots {{")?;
    writeln!(w, "        match self {{")?;
    for op in ops {
        let feedback_fields = user_fields(op)
            .into_iter()
            .filter_map(|field| {
                KINDS
                    .iter()
                    .find(|(ty, _)| *ty == field.ty)
                    .map(|(_, kind)| (rust_field_name(&field.name), *kind))
            })
            .collect::<Vec<_>>();
        if feedback_fields.is_empty() {
            continue;
        }
        let bindings = feedback_fields
            .iter()
            .map(|(name, _)| name.clone())
            .collect::<Vec<_>>()
            .join(", ");
        let slots = feedback_fields
            .iter()
            .map(|(name, kind)| format!("{kind}: Some(*{name})"))
            .collect::<Vec<_>>()
            .join(", ");
        writeln!(
            w,
            "            Instruction::{} {{ {bindings}, .. }} => FeedbackSlots {{ {slots}, ..FeedbackSlots::default() }},",
            op.name
        )?;
    }
    writeln!(w, "            _ => FeedbackSlots::default(),")?;
    writeln!(w, "        }}")?;
    writeln!(w, "    }}")?;
    writeln!(w)?;
    Ok(())
}

fn generate_rust_code(mut w: impl Write, ops: &[InstructionDefinition], record_form_only: bool) -> GeneratorResult {
    writeln!(w, "// @generated from Libraries/LibJS/Interpreter/interpreter.flap")?;
    writeln!(w, "// Do not edit manually.")?;
    writeln!(w)?;

    generate_opcode_enum(&mut w, ops)?;
    generate_num_opcodes_const(&mut w, ops)?;
    generate_opcode_impl(&mut w, ops)?;
    generate_instruction_enum(&mut w, ops, "Debug, Clone, PartialEq, Eq")?;
    generate_instruction_length_from_bytes(&mut w, ops, "DecodeErrorKind")?;
    generate_decode_function(&mut w, ops)?;
    generate_slow_path_layout_function(&mut w, ops, record_form_only)?;

    writeln!(w, "impl Instruction {{")?;
    generate_opcode_method(&mut w, ops)?;
    generate_is_terminator_method(&mut w, ops)?;
    generate_for_each_operand_method(&mut w, ops)?;
    generate_for_each_jump_target_method(&mut w, ops)?;
    generate_feedback_slots_method(&mut w, ops)?;
    writeln!(w, "}}")?;
    writeln!(w)?;

    // The encoder lets the unit tests build instruction streams from typed instructions.
    writeln!(w, "#[cfg(test)]")?;
    writeln!(w, "#[allow(dead_code)]")?;
    writeln!(w, "impl Instruction {{")?;
    generate_encode_method(&mut w, ops)?;
    generate_encoded_size_method(&mut w, ops)?;
    writeln!(w, "}}")?;
    Ok(())
}

fn main() -> GeneratorResult {
    let manifest_dir = PathBuf::from(env::var("CARGO_MANIFEST_DIR")?);
    let flap_path = manifest_dir.join("../../Interpreter/interpreter.flap");

    println!("cargo:rerun-if-changed={}", flap_path.display());
    println!("cargo:rerun-if-changed=build.rs");

    let out_dir = PathBuf::from(env::var("OUT_DIR")?);
    let instruction_set = InstructionSet::load(&flap_path)?;
    let file = fs::File::create(out_dir.join("decoder_generated.rs"))?;
    let mut writer = BufWriter::new(file);
    // Like flapc, which gives COFF targets the record form of every slow path.
    let record_form_only = env::var("CARGO_CFG_TARGET_OS")? == "windows";
    generate_rust_code(&mut writer, &instruction_set.ops, record_form_only)?;
    writer.flush()?;
    Ok(())
}
