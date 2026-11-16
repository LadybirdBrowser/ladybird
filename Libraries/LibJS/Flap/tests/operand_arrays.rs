/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use flapc::{Architecture, CompilationUnit, CompileOptions, Compiler, ObjectFormat, SourceInput, Target};

fn compile(program: &str) -> Result<String, String> {
    Compiler::new(CompileOptions {
        target: Target {
            architecture: Architecture::X86_64,
            object_format: ObjectFormat::Elf,
        },
        has_jscvt: false,
        has_lzcnt: false,
        has_bmi1: false,
        enable_assertions: true,
        profiling: false,
        profile: flapc::Profile::JavaScript,
    })
    .compile(CompilationUnit {
        source: SourceInput {
            name: "operand_arrays.flap",
            contents: program,
        },
        constants: Some(SourceInput {
            name: "layout.conf",
            contents: include_str!("interpreter-layout.conf"),
        }),
    })
    .map(|assembly| assembly.as_str().to_owned())
    .map_err(|error| format!("{error:?}"))
}

#[test]
fn an_inline_function_takes_the_address_of_its_operand_array_parameter() {
    let assembly = compile(
        "
inline fn first_operand(operands: Operand[]) -> u32 {
    let indices = &operands;
    u32(indices[0])
}

handler Check(
    operands_count: u8,
    operands: Operand[]
) {
    let first = first_operand(operands);
    values[first] = Value<Undefined>;
    let mut count: u32;
    load8(count, [pb, pc, operands_count]);
    let length: u32 = 3 + count;
    dispatch_variable(length);
}
",
    )
    .unwrap();
    // NB: The parameter is the handler's field, so its address is in the instruction, not its offset.
    assert!(assembly.contains("[r14 + r13 + 2]"), "{assembly}");
}

const CHECK_HANDLER: &str = "
handler Check(
    operands_count: u8,
    operands: Operand[]
) {
    let first = first_operand(operands);
    values[first] = Value<Undefined>;
    let mut count: u32;
    load8(count, [pb, pc, operands_count]);
    let length: u32 = 3 + count;
    dispatch_variable(length);
}
";

#[test]
fn an_inline_function_cannot_use_an_operand_array_parameter_as_a_value() {
    let error = compile(&format!(
        "
inline fn first_operand(operands: Operand[]) -> u32 {{
    let indices = operands;
    u32(indices[0])
}}
{CHECK_HANDLER}"
    ))
    .unwrap_err();
    assert!(
        error.contains("'first_operand' uses its parameter for the operand array 'operands' as a value"),
        "{error}"
    );
}

#[test]
fn an_inline_function_cannot_pass_an_operand_array_parameter_on_to_a_value_use() {
    let error = compile(&format!(
        "
inline fn copy_operands(operands: Operand[]) -> u32 {{
    let indices = operands;
    u32(indices[0])
}}

inline fn first_operand(operands: Operand[]) -> u32 {{
    copy_operands(operands)
}}
{CHECK_HANDLER}"
    ))
    .unwrap_err();
    assert!(
        error.contains("'first_operand' uses its parameter for the operand array 'operands' as a value"),
        "{error}"
    );
}

#[test]
fn a_handler_cannot_use_its_operand_array_as_a_value() {
    let error = compile(
        "
handler Check(
    operands_count: u8,
    operands: Operand[]
) {
    let indices = operands;
    values[u32(indices[0])] = Value<Undefined>;
    let mut count: u32;
    load8(count, [pb, pc, operands_count]);
    let length: u32 = 3 + count;
    dispatch_variable(length);
}
",
    )
    .unwrap_err();
    assert!(
        error.contains("the operand array 'operands' as a value is the field's offset"),
        "{error}"
    );
}

#[test]
fn a_handler_cannot_use_its_operand_array_in_a_condition() {
    let error = compile(
        "
handler Check(
    operands_count: u8,
    operands: Operand[]
) {
    if operands != 0 {
        values[0] = Value<Undefined>;
    }
    let mut count: u32;
    load8(count, [pb, pc, operands_count]);
    let length: u32 = 3 + count;
    dispatch_variable(length);
}
",
    )
    .unwrap_err();
    assert!(
        error.contains("the operand array 'operands' as a value is the field's offset"),
        "{error}"
    );
}

#[test]
fn an_inline_function_cannot_use_an_operand_array_parameter_in_a_condition() {
    let error = compile(&format!(
        "
inline fn first_operand(operands: Operand[]) -> u32 {{
    assert(operands != null);
    let indices = &operands;
    u32(indices[0])
}}
{CHECK_HANDLER}"
    ))
    .unwrap_err();
    assert!(
        error.contains("'first_operand' uses its parameter for the operand array 'operands' as a value"),
        "{error}"
    );
}
