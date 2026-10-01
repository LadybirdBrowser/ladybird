/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The slow paths for calls, function and class creation, and their helpers.

use libjs_abi::ArgumentsKind;

use crate::bytecode::op;
use crate::interpreter::runtime_functions::{SlowPathControl, handle_asm_exception};
use crate::interpreter::vm::Vm;
use crate::layout::value::Value;
use crate::runtime::abstract_operations::{create_mapped_arguments_object, create_unmapped_arguments_object};
use crate::runtime::array::Array;
use crate::runtime::completion::Must;
use crate::runtime::ecmascript_function_object::{as_ecmascript_function_object, value_as_ecmascript_function_object};
use crate::runtime::environment::InitializeBindingHint;
use crate::runtime::property_attributes::DEFAULT_ATTRIBUTES;

// Try to inline a JS-to-JS call by building the callee frame through the
// shared VM::push_inline_frame() helper. Returns whether the callee frame
// was pushed; if not, the caller keeps handling the Call itself.
pub fn try_inline_call(vm: &Vm, pc: u32, instruction: &op::Call, values: &op::CallValues, arguments: &[Value]) -> bool {
    let callee = values.callee;
    let Some(callee_function) = value_as_ecmascript_function_object(callee) else {
        return false;
    };

    if !callee_function.can_inline_call() {
        return false;
    }

    vm.push_inline_frame(
        callee_function,
        callee_function.inline_call_executable(),
        arguments,
        pc + instruction.length(),
        instruction.dst.0,
        values.this_value,
        None,
        false,
    )
    .is_some()
}

pub fn create_rest_params(
    vm: &Vm,
    pc: u32,
    instruction: &op::CreateRestParams,
    values: &mut op::CreateRestParamsValues,
) -> SlowPathControl {
    let context = vm
        .running_execution_context()
        .expect("rest parameters are created in a running frame");
    // SAFETY: The running execution context is live.
    let context = unsafe { context.as_ref() };
    let arguments = context.arguments();
    let arguments_count = context.passed_argument_count.get() as usize;
    let realm = vm.current_realm().expect("there is a current realm");
    let array = Array::create(vm, realm, 0, None).must();
    for argument in arguments
        .iter()
        .take(arguments_count)
        .skip(instruction.rest_index as usize)
    {
        array.indexed_append(argument.get(), DEFAULT_ATTRIBUTES);
    }
    values.dst = Value::from_object(array);
    SlowPathControl::continue_at(pc + op::CreateRestParams::LENGTH)
}

pub fn create_arguments(
    vm: &Vm,
    pc: u32,
    instruction: &op::CreateArguments,
    values: &mut op::CreateArgumentsValues,
) -> SlowPathControl {
    let context = vm
        .running_execution_context()
        .expect("arguments objects are created in a running frame");
    // SAFETY: The running execution context is live.
    let context = unsafe { context.as_ref() };
    let function = context
        .function
        .get()
        .expect("an arguments object is created for a function");
    let arguments = context.arguments();
    let environment = context
        .lexical_environment
        .get()
        .expect("a function runs in an environment");

    let passed_arguments = &arguments[..context.passed_argument_count.get() as usize];
    let arguments_object = if instruction.kind == ArgumentsKind::Mapped as u32 {
        let ecmascript_function =
            as_ecmascript_function_object(function).expect("only ECMAScript functions have mapped arguments objects");
        create_mapped_arguments_object(
            vm,
            function,
            &ecmascript_function.parameter_names_for_mapped_arguments(),
            passed_arguments,
            environment,
        )
    } else {
        create_unmapped_arguments_object(vm, passed_arguments)
    };

    if instruction.dst.get().is_some() {
        values.dst = Value::from_object(arguments_object);
        return SlowPathControl::continue_at(pc + op::CreateArguments::LENGTH);
    }

    let arguments_name = vm.names.arguments.as_string();
    if instruction.is_immutable {
        environment.create_immutable_binding(vm, arguments_name, false).must();
    } else {
        environment.create_mutable_binding(vm, arguments_name, false).must();
    }
    environment
        .initialize_binding(
            vm,
            arguments_name,
            Value::from_object(arguments_object),
            InitializeBindingHint::Normal,
        )
        .must();
    SlowPathControl::continue_at(pc + op::CreateArguments::LENGTH)
}

/// Handles an exception a raw native function the interpreter called threw.
pub fn handle_raw_native_exception(vm: &Vm, exception: Value) -> SlowPathControl {
    let callee_frame = vm
        .running_execution_context()
        .expect("a raw native function runs in its own frame");
    // SAFETY: The running execution context is live.
    let callee_frame = unsafe { callee_frame.as_ref() };
    assert!(!callee_frame.caller_frame.get().is_null());

    // Raw-native asm calls keep their callee frame off the VM execution
    // context stack, so we have to unwind it manually before exception
    // dispatch. Match VM::handle_exception()'s inline-frame semantics by
    // probing the caller with a PC inside the Call instruction.
    let caller_pc = callee_frame.caller_return_pc.get();
    vm.unwind_inline_frame_for_exception();
    handle_asm_exception(vm, caller_pc - 1, exception)
}
