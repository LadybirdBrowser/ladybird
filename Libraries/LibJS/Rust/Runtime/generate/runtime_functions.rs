/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Writes the RuntimeFunctions trait, with one method per function the generated interpreter calls, and an
//! extern "C" entry point for each with exactly the signature the interpreter's call sites assume. The entry points
//! translate every calling convention into one Rust signature per kind of call. A method that the runtime does not
//! implement yet stops the process with the function's name.

use std::collections::HashMap;
use std::fmt::Write;

use flapc::metadata::{InstructionDefinition, ParameterMode, SlowPathAbi, SlowPathLayout};
use flapc::runtime_interface::{RuntimeFunction, RuntimeFunctionKind};

use crate::ops::{field_name, rust_identifier};

fn method_name(symbol: &str) -> String {
    rust_identifier(
        symbol
            .strip_prefix("asm_slow_path_")
            .or_else(|| symbol.strip_prefix("asm_"))
            .unwrap_or_else(|| panic!("unexpected runtime function symbol {symbol}")),
    )
}

struct Record<'a> {
    op: &'a str,
    layout: &'a SlowPathLayout,
    /// The array name and the name of the instruction field that counts its elements.
    array: Option<(String, String)>,
}

impl<'a> Record<'a> {
    fn new(op: &'a str, layout: &'a SlowPathLayout, ops: &HashMap<&str, &InstructionDefinition>) -> Self {
        let array = layout.array.as_ref().map(|array| {
            let definition = ops[op];
            let array_layout = definition.array.as_ref().expect("an operand array has a layout");
            (
                field_name(&array.name),
                field_name(&definition.fields[array_layout.count_field_index].name),
            )
        });
        Self { op, layout, array }
    }

    fn method_parameters(&self) -> String {
        let mut parameters = format!("_instruction: &op::{0}, _values: &mut op::{0}Values", self.op);
        if let Some((array, _)) = &self.array {
            let _ = write!(parameters, ", _{array}: &mut [Value]");
        }
        parameters
    }

    /// Statements that turn the raw record pointer `values` into the method's arguments.
    fn bind_record(&self, out: &mut String) {
        if let Some((array, count)) = &self.array {
            let _ = writeln!(
                out,
                "    // SAFETY: The interpreter stores the instruction's {count} operands after the record."
            );
            let _ = writeln!(
                out,
                "    let {array} = unsafe {{ core::slice::from_raw_parts_mut((&raw mut (*values).{array}).cast::<Value>(), instruction.{count} as usize) }};"
            );
        }
        let _ = writeln!(
            out,
            "    // SAFETY: The interpreter passes a record for the instruction it is executing."
        );
        let _ = writeln!(out, "    let values = unsafe {{ &mut *values }};");
    }

    fn call_arguments(&self) -> String {
        match &self.array {
            Some((array, _)) => format!("instruction, values, {array}"),
            None => "instruction, values".to_string(),
        }
    }
}

pub fn generate(functions: &[RuntimeFunction], ops: &[InstructionDefinition]) -> String {
    let ops: HashMap<&str, &InstructionDefinition> = ops.iter().map(|op| (op.name.as_str(), op)).collect();
    let mut methods = String::new();
    let mut entry_points = String::new();

    for function in functions {
        let symbol = &function.symbol;
        let name = method_name(symbol);
        let _ = writeln!(methods, "    /// Called as `{symbol}`.");
        let _ = writeln!(entry_points, "#[unsafe(no_mangle)]");
        match &function.kind {
            RuntimeFunctionKind::SlowPath { op, abi, layout } => {
                let record = Record::new(op, layout, &ops);
                let _ = writeln!(
                    methods,
                    "    fn {name}(_vm: &Vm, pc: u32, {}) -> SlowPathControl {{\n        unimplemented_runtime_function(\"{symbol}\", pc)\n    }}",
                    record.method_parameters()
                );
                generate_slow_path_entry_point(&mut entry_points, symbol, &name, *abi, &record);
            }
            RuntimeFunctionKind::Try { op, layout } => {
                let record = Record::new(op, layout, &ops);
                let _ = writeln!(
                    methods,
                    "    /// Returns whether it handled the instruction; if not, the interpreter takes the instruction's slow path.\n    fn {name}(_vm: &Vm, _pc: u32, {}) -> bool {{\n        false\n    }}",
                    record.method_parameters()
                );
                let _ = writeln!(
                    entry_points,
                    "pub unsafe extern \"C\" fn {symbol}(vm: *const Vm, pc: u32, instruction: *const op::{op}, values: *mut op::{op}Values) -> i64 {{"
                );
                entry_points
                    .push_str("    // SAFETY: The interpreter passes its VM and the instruction it is executing.\n");
                entry_points.push_str("    let (vm, instruction) = unsafe { (&*vm, &*instruction) };\n");
                record.bind_record(&mut entry_points);
                let _ = writeln!(
                    entry_points,
                    "    i64::from(!<Runtime as RuntimeFunctions>::{name}(vm, pc, {}))\n}}",
                    record.call_arguments()
                );
            }
            RuntimeFunctionKind::BinarySlowPath => {
                let _ = writeln!(
                    methods,
                    "    fn {name}(_vm: &Vm, pc: u32, _dst: &Cell<Value>, _lhs: Value, _rhs: Value) -> SlowPathControl {{\n        unimplemented_runtime_function(\"{symbol}\", pc)\n    }}"
                );
                let _ = writeln!(
                    entry_points,
                    "pub unsafe extern \"C\" fn {symbol}(vm: *const Vm, pc: u32, dst: *mut Value, lhs: Value, rhs: Value) -> i64 {{\n    // SAFETY: The interpreter passes its VM and a live value slot.\n    let (vm, dst) = unsafe {{ (&*vm, &*dst.cast::<Cell<Value>>()) }};\n    <Runtime as RuntimeFunctions>::{name}(vm, pc, dst, lhs, rhs).0\n}}"
                );
            }
            RuntimeFunctionKind::JumpSlowPath => {
                let _ = writeln!(
                    methods,
                    "    fn {name}(_vm: &Vm, pc: u32, _lhs: Value, _rhs: Value, _true_target: u32, _false_target: u32) -> SlowPathControl {{\n        unimplemented_runtime_function(\"{symbol}\", pc)\n    }}"
                );
                let _ = writeln!(
                    entry_points,
                    "pub unsafe extern \"C\" fn {symbol}(vm: *const Vm, pc: u32, lhs: Value, rhs: Value, true_target: u32, false_target: u32) -> i64 {{\n    // SAFETY: The interpreter passes its VM.\n    let vm = unsafe {{ &*vm }};\n    <Runtime as RuntimeFunctions>::{name}(vm, pc, lhs, rhs, true_target, false_target).0\n}}"
                );
            }
            RuntimeFunctionKind::Helper => {
                let _ = writeln!(
                    methods,
                    "    fn {name}(_argument: u64) -> u64 {{\n        unimplemented_runtime_function(\"{symbol}\", 0)\n    }}"
                );
                let _ = writeln!(
                    entry_points,
                    "pub extern \"C\" fn {symbol}(argument: u64) -> u64 {{\n    <Runtime as RuntimeFunctions>::{name}(argument)\n}}"
                );
            }
            RuntimeFunctionKind::HelperWithTwoArguments => {
                let _ = writeln!(
                    methods,
                    "    fn {name}(_first: u64, _second: u64) -> u64 {{\n        unimplemented_runtime_function(\"{symbol}\", 0)\n    }}"
                );
                let _ = writeln!(
                    entry_points,
                    "pub extern \"C\" fn {symbol}(first: u64, second: u64) -> u64 {{\n    <Runtime as RuntimeFunctions>::{name}(first, second)\n}}"
                );
            }
            RuntimeFunctionKind::FallbackHandler => {
                let _ = writeln!(
                    methods,
                    "    fn {name}(_vm: &Vm, pc: u32, _instruction: *const u8) -> SlowPathControl {{\n        unimplemented_runtime_function(\"{symbol}\", pc)\n    }}"
                );
                let _ = writeln!(
                    entry_points,
                    "pub unsafe extern \"C\" fn {symbol}(vm: *const Vm, pc: u32, instruction: *const u8) -> i64 {{\n    // SAFETY: The interpreter passes its VM.\n    let vm = unsafe {{ &*vm }};\n    <Runtime as RuntimeFunctions>::{name}(vm, pc, instruction).0\n}}"
                );
            }
            RuntimeFunctionKind::BreakpointCheck => {
                let _ = writeln!(
                    methods,
                    "    fn {name}(_vm: &Vm, pc: u32) {{\n        unimplemented_runtime_function(\"{symbol}\", pc)\n    }}"
                );
                let _ = writeln!(
                    entry_points,
                    "pub unsafe extern \"C\" fn {symbol}(vm: *const Vm, pc: u32) {{\n    // SAFETY: The interpreter passes its VM.\n    let vm = unsafe {{ &*vm }};\n    <Runtime as RuntimeFunctions>::{name}(vm, pc);\n}}"
                );
            }
            RuntimeFunctionKind::StackOverflowSlowPath => {
                let _ = writeln!(
                    methods,
                    "    fn {name}(_vm: &Vm, pc: u32) -> SlowPathControl {{\n        unimplemented_runtime_function(\"{symbol}\", pc)\n    }}"
                );
                let _ = writeln!(
                    entry_points,
                    "pub unsafe extern \"C\" fn {symbol}(vm: *const Vm, pc: u32) -> i64 {{\n    // SAFETY: The interpreter passes its VM.\n    let vm = unsafe {{ &*vm }};\n    <Runtime as RuntimeFunctions>::{name}(vm, pc).0\n}}"
                );
            }
        }
        entry_points.push('\n');
    }

    let mut out = String::new();
    out.push_str("// Generated by the libjs_runtime_rust build script -- DO NOT EDIT\n\n");
    out.push_str("pub trait RuntimeFunctions {\n");
    out.push_str(&methods);
    out.push_str("}\n\n");
    out.push_str(&entry_points);
    let _ = writeln!(
        out,
        "pub const RUNTIME_FUNCTION_SYMBOLS: [&str; {}] = [",
        functions.len()
    );
    for function in functions {
        let _ = writeln!(out, "    \"{}\",", function.symbol);
    }
    out.push_str("];\n");
    out
}

fn generate_slow_path_entry_point(out: &mut String, symbol: &str, name: &str, abi: SlowPathAbi, record: &Record) {
    let op = record.op;
    let fields = &record.layout.fields;
    let inputs: Vec<String> = fields
        .iter()
        .filter(|field| field.mode != ParameterMode::Out)
        .map(|field| field_name(&field.name))
        .collect();
    let input_parameters: String = inputs.iter().map(|input| format!(", {input}: Value")).collect();
    let initialize_values = || {
        let initializers: Vec<String> = fields
            .iter()
            .map(|field| {
                let name = field_name(&field.name);
                if field.mode == ParameterMode::Out {
                    format!("{name}: Value::EMPTY")
                } else {
                    name
                }
            })
            .collect();
        format!(
            "    let mut values = op::{op}Values {{ {} }};\n",
            initializers.join(", ")
        )
    };
    let call = format!(
        "<Runtime as RuntimeFunctions>::{name}(vm, pc, {})",
        record.call_arguments()
    );
    let bind_vm = "    // SAFETY: The interpreter passes its VM and the instruction it is executing.\n    let (vm, instruction) = unsafe { (&*vm, &*instruction) };\n";

    match abi {
        SlowPathAbi::Scalar => {
            let primary_output = fields
                .iter()
                .find(|field| field.mode == ParameterMode::Out && !field.optional)
                .map(|field| field_name(&field.name));
            let _ = writeln!(
                out,
                "pub unsafe extern \"C\" fn {symbol}(vm: *const Vm, pc: u32, instruction: *const op::{op}{input_parameters}) -> AsmSlowPathResult {{"
            );
            out.push_str(bind_vm);
            out.push_str(&initialize_values());
            let _ = writeln!(out, "    let values = &mut values;\n    let control = {call};");
            match primary_output {
                Some(output) => {
                    let _ = writeln!(
                        out,
                        "    AsmSlowPathResult {{ control: control.0, value: values.{output}.0 }}\n}}"
                    );
                }
                None => {
                    out.push_str("    AsmSlowPathResult { control: control.0, value: 0 }\n}\n");
                }
            }
        }
        SlowPathAbi::Mixed => {
            let _ = writeln!(
                out,
                "pub unsafe extern \"C\" fn {symbol}(vm: *const Vm, pc: u32, instruction: *const op::{op}, outputs: *mut op::{op}Values{input_parameters}) -> i64 {{"
            );
            out.push_str(bind_vm);
            out.push_str(&initialize_values());
            let _ = writeln!(out, "    let values = &mut values;\n    let control = {call};");
            out.push_str("    // SAFETY: The interpreter passes a record to receive the outputs.\n    let outputs = unsafe { &mut *outputs };\n");
            for field in fields.iter().filter(|field| field.mode != ParameterMode::In) {
                let name = field_name(&field.name);
                let _ = writeln!(out, "    outputs.{name} = values.{name};");
            }
            out.push_str("    control.0\n}\n");
        }
        SlowPathAbi::Record => {
            let _ = writeln!(
                out,
                "pub unsafe extern \"C\" fn {symbol}(vm: *const Vm, pc: u32, instruction: *const op::{op}, values: *mut op::{op}Values) -> i64 {{"
            );
            out.push_str(bind_vm);
            record.bind_record(out);
            let _ = writeln!(out, "    {call}.0\n}}");
        }
    }
}
