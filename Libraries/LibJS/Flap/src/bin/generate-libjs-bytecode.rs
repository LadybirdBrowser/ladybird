/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use flapc::metadata::{
    Field, InstructionDefinition, derive_specialized_instructions, parse_flap_metadata, parse_specializations,
};
use flapc::validate_specializations;
use std::env;
use std::fmt::Write;
use std::fs;
use std::path::PathBuf;

fn parameter_name(name: &str) -> &str {
    name.strip_prefix("m_").unwrap_or(name)
}

fn count_field_name<'a>(op: &'a InstructionDefinition, array_field: &Field) -> Result<&'a str, String> {
    let plural = format!("{}_count", array_field.name);
    let singular = array_field.name.strip_suffix('s').map(|name| format!("{name}_count"));
    op.fields
        .iter()
        .find(|field| {
            !field.is_array
                && field.ty == "u32"
                && (field.name == plural || singular.as_ref().is_some_and(|name| field.name == *name))
        })
        .map(|field| field.name.as_str())
        .ok_or_else(|| {
            format!(
                "no count field found for array field '{}' in bytecode '{}'",
                array_field.name, op.name
            )
        })
}

fn cpp_type(field: &Field) -> &str {
    match field.ty.as_str() {
        "PropertyLookupCacheIndex"
        | "GlobalVariableCacheIndex"
        | "EnvironmentCoordinateCacheIndex"
        | "TemplateObjectCacheIndex"
        | "ObjectShapeCacheIndex"
        | "ObjectPropertyIteratorCacheIndex" => "u32",
        _ => &field.ty,
    }
}

fn generate_class(output: &mut String, op: &InstructionDefinition) -> Result<(), String> {
    let arrays = op.fields.iter().filter(|field| field.is_array).collect::<Vec<_>>();
    let count_fields = arrays
        .iter()
        .map(|field| count_field_name(op, field))
        .collect::<Result<Vec<_>, _>>()?;

    writeln!(output, "class {} final : public {} {{", op.name, op.parent).unwrap();
    writeln!(output, "public:").unwrap();
    if !arrays.is_empty() {
        writeln!(output, "    static constexpr bool IsVariableLength = true;").unwrap();
    }
    if op.fields.iter().any(|field| field.name == "m_length") {
        writeln!(output, "    size_t length_impl() const {{ return m_length; }}").unwrap();
    }
    if op.is_terminator {
        writeln!(output, "    static constexpr bool IsTerminator = true;").unwrap();
    }

    let mut parameters = Vec::new();
    for field in &op.fields {
        if field.is_array
            || field.ty == "EnvironmentCoordinate"
            || field.name == "m_length"
            || count_fields.contains(&field.name.as_str())
        {
            continue;
        }
        parameters.push(format!("{} {}", cpp_type(field), parameter_name(&field.name)));
    }
    for field in &arrays {
        parameters.push(format!("ReadonlySpan<{}> {}", field.ty, parameter_name(&field.name)));
    }
    writeln!(output, "    {}({})", op.name, parameters.join(", ")).unwrap();

    let mut initializers = vec![format!("{}(Type::{})", op.parent, op.name)];
    for field in &op.fields {
        if field.is_array || field.ty == "EnvironmentCoordinate" {
            continue;
        }
        if count_fields.contains(&field.name.as_str()) {
            let array = arrays
                .iter()
                .find(|array| count_field_name(op, array).is_ok_and(|name| name == field.name))
                .expect("validated count field has an array");
            initializers.push(format!("{}({}.size())", field.name, parameter_name(&array.name)));
        } else if field.name == "m_length" {
            let array = arrays.first();
            if let Some(array) = array {
                initializers.push(format!(
                    "m_length(round_up_to_power_of_two(alignof(void*), sizeof(*this) + sizeof({}) * {}.size()))",
                    array.ty,
                    parameter_name(&array.name)
                ));
            } else {
                initializers.push("m_length(0)".to_string());
            }
        } else {
            initializers.push(format!("{}({})", field.name, parameter_name(&field.name)));
        }
    }
    writeln!(output, "        : {}", initializers[0]).unwrap();
    for initializer in &initializers[1..] {
        writeln!(output, "        , {initializer}").unwrap();
    }
    writeln!(output, "    {{").unwrap();
    for field in &arrays {
        let parameter = parameter_name(&field.name);
        if field.ty == "Optional<Operand>" {
            writeln!(output, "        for (size_t i = 0; i < {parameter}.size(); ++i) {{").unwrap();
            writeln!(output, "            if ({parameter}[i].has_value())").unwrap();
            writeln!(output, "                {}[i] = {parameter}[i].value();", field.name).unwrap();
            writeln!(output, "            else").unwrap();
            writeln!(output, "                {}[i] = {{}};", field.name).unwrap();
            writeln!(output, "        }}").unwrap();
        } else {
            writeln!(output, "        for (size_t i = 0; i < {parameter}.size(); ++i)").unwrap();
            writeln!(output, "            {}[i] = {parameter}[i];", field.name).unwrap();
        }
    }
    writeln!(output, "    }}").unwrap();
    writeln!(output).unwrap();
    if !op.fields.is_empty() {
        writeln!(output).unwrap();
    }

    for field in &op.fields {
        let name = parameter_name(&field.name);
        if field.is_array {
            let count = count_field_name(op, field)?;
            writeln!(
                output,
                "    ReadonlySpan<{}> {name}() const {{ return ReadonlySpan<{}> {{ {}, {count} }}; }}",
                field.ty, field.ty, field.name
            )
            .unwrap();
        } else if field.name != "m_length" {
            writeln!(output, "    auto const& {name}() const {{ return {}; }}", field.name).unwrap();
        }
    }

    writeln!(output).unwrap();
    writeln!(output, "private:").unwrap();
    for field in &op.fields {
        if field.is_array {
            writeln!(output, "    {} {}[];", cpp_type(field), field.name).unwrap();
        } else {
            writeln!(output, "    {} {};", cpp_type(field), field.name).unwrap();
        }
    }
    writeln!(output, "}};").unwrap();
    writeln!(output, "static_assert(IsTriviallyDestructible<{}>);", op.name).unwrap();
    Ok(())
}

fn generate_op_header(ops: &[InstructionDefinition]) -> Result<String, String> {
    let mut output = String::from(
        r#"#pragma once

#include <AK/Span.h>
#include <AK/StdLibExtras.h>
#include <LibJS/Bytecode/Builtins.h>
#include <LibJS/Bytecode/IdentifierTable.h>
#include <LibJS/Bytecode/Instruction.h>
#include <LibJS/Bytecode/Label.h>
#include <LibJS/Bytecode/Operand.h>
#include <LibJS/Bytecode/PutKind.h>
#include <LibJS/Bytecode/RegexTable.h>
#include <LibJS/Bytecode/Register.h>
#include <LibJS/Bytecode/StringTable.h>
#include <LibJS/Runtime/BigInt.h>
#include <LibJS/Runtime/Environment.h>
#include <LibJS/Runtime/Iterator.h>
#include <LibJS/Runtime/Value.h>

namespace JS::Bytecode::Op {

"#,
    );
    for op in ops {
        generate_class(&mut output, op)?;
        output.push('\n');
    }
    output.push_str("} // namespace JS::Bytecode::Op");
    Ok(output)
}

fn generate_opcodes_header(ops: &[InstructionDefinition]) -> String {
    let mut output = String::from("#pragma once\n\n#define ENUMERATE_BYTECODE_OPS(O) \\\n");
    for (index, op) in ops.iter().enumerate() {
        if index + 1 == ops.len() {
            writeln!(output, "    O({})", op.name).unwrap();
        } else {
            writeln!(output, "    O({}) \\", op.name).unwrap();
        }
    }
    output
}

fn required_path(arguments: &mut impl Iterator<Item = String>, option: &str) -> Result<PathBuf, String> {
    arguments
        .next()
        .map(PathBuf::from)
        .ok_or_else(|| format!("{option} requires a path"))
}

fn parse_instruction_definitions(source_name: &str, source: &str) -> Result<Vec<InstructionDefinition>, String> {
    let mut ops = parse_flap_metadata(source_name, source).map_err(|error| error.to_string())?;
    let specializations = parse_specializations(source_name, source).map_err(|error| error.to_string())?;
    validate_specializations(&ops, &specializations)?;
    let specialized_ops = derive_specialized_instructions(&ops, &specializations)?;
    ops.extend(
        specialized_ops
            .into_iter()
            .map(|specialization| specialization.definition),
    );
    Ok(ops)
}

fn run() -> Result<(), String> {
    let mut arguments = env::args().skip(1);
    let mut input = None;
    let mut op_header = None;
    let mut opcodes_header = None;
    while let Some(argument) = arguments.next() {
        match argument.as_str() {
            "--input" => input = Some(required_path(&mut arguments, "--input")?),
            "--op-header" => op_header = Some(required_path(&mut arguments, "--op-header")?),
            "--opcodes-header" => opcodes_header = Some(required_path(&mut arguments, "--opcodes-header")?),
            _ => return Err(format!("unknown argument '{argument}'")),
        }
    }

    let input = input.ok_or_else(|| "missing --input".to_string())?;
    let op_header = op_header.ok_or_else(|| "missing --op-header".to_string())?;
    let opcodes_header = opcodes_header.ok_or_else(|| "missing --opcodes-header".to_string())?;
    let source = fs::read_to_string(&input).map_err(|error| format!("failed to read {}: {error}", input.display()))?;
    let source_name = input.to_string_lossy();
    let ops = parse_instruction_definitions(&source_name, &source)?;
    fs::write(&op_header, generate_op_header(&ops)?)
        .map_err(|error| format!("failed to write {}: {error}", op_header.display()))?;
    fs::write(&opcodes_header, generate_opcodes_header(&ops))
        .map_err(|error| format!("failed to write {}: {error}", opcodes_header.display()))?;
    Ok(())
}

fn main() {
    if let Err(error) = run() {
        eprintln!("{error}");
        std::process::exit(1);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn variable_length_operations_do_not_hide_instruction_length() {
        let op = parse_flap_metadata(
            "test.flap",
            "handler Call(length: u32, argument_count: u32, arguments: Operand[]) { dispatch_next; }",
        )
        .unwrap()
        .remove(0);
        let mut output = String::new();

        generate_class(&mut output, &op).unwrap();

        assert!(output.contains("size_t length_impl() const { return m_length; }"));
        assert!(!output.contains("auto const& length() const"));
    }

    #[test]
    fn rejects_invalid_specializations_before_header_generation() {
        let error = parse_instruction_definitions(
            "test.flap",
            r#"
handler Add(rhs: Operand) { dispatch_next; }
specialize Add(rhs: Int32);
specialize Add(rhs: Int32);
"#,
        )
        .unwrap_err();

        assert_eq!(error, "duplicate specialization name 'AddRhsInt32'");
    }
}
