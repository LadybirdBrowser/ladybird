/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The slow paths for calls, function and class creation, and their helpers.

use ak::{ScopeGuard, Utf16FlyString};
use libjs_abi::{ArgumentsKind, Builtin, FunctionNamePrefix};

use crate::bytecode::encoding::{InstructionHeader, OptionalOperand, StringTableIndex};
use crate::bytecode::op;
use crate::bytecode::property_access::Strict;
use crate::interpreter::runtime_functions::{
    SlowPathControl, asm_try, handle_asm_exception, unimplemented_runtime_function,
};
use crate::interpreter::vm::{EvalMode, Vm};
use crate::layout::cell::Gc;
use crate::layout::execution_context::ExecutionContext;
use crate::layout::value::Value;
use crate::runtime::abstract_operations::{
    self, CallerMode, create_mapped_arguments_object, create_unmapped_arguments_object, function_object_as_object,
    get_prototype_from_constructor, get_this_environment, length_of_array_like, perform_eval,
};
use crate::runtime::array::Array;
use crate::runtime::class_construction::construct_class;
use crate::runtime::class_field_definition::ClassElementName;
use crate::runtime::completion::{Must, ThrowCompletionOr};
use crate::runtime::ecmascript_function_object::{
    EcmascriptFunctionObject, as_ecmascript_function_object, value_as_ecmascript_function_object,
};
use crate::runtime::environment::InitializeBindingHint;
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;
use crate::runtime::function_environment::FunctionEnvironment;
use crate::runtime::function_object::FunctionObject;
use crate::runtime::intrinsics::Intrinsics;
use crate::runtime::math_object;
use crate::runtime::native_javascript_backed_function::NativeJavaScriptBackedFunction;
use crate::runtime::object::{Object, StackFrameInfo};
use crate::runtime::property_attributes::DEFAULT_ATTRIBUTES;
use crate::runtime::property_key::PropertyKey;
use crate::runtime::shared_function_instance_data::{ConstructorKind, FunctionKind};
use crate::runtime::string_constructor;
use crate::utf16::Utf16Display;

/// Op::CallType: how a call instruction calls its callee.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum CallType {
    Call,
    Construct,
    DirectEval,
}

fn running_execution_context(vm: &Vm) -> &ExecutionContext {
    let context = vm
        .running_execution_context()
        .expect("a slow path runs in a running frame");
    // SAFETY: The running execution context is live while the slow path runs in it. Callers only use the reference
    // before anything can unwind the frame.
    unsafe { context.as_ref() }
}

fn strict_of(header: &InstructionHeader) -> Strict {
    if header.strict { Strict::Yes } else { Strict::No }
}

fn caller_mode_of(strict: Strict) -> CallerMode {
    if strict == Strict::Yes {
        CallerMode::Strict
    } else {
        CallerMode::NonStrict
    }
}

/// Executable::get_string().
fn get_string(vm: &Vm, index: StringTableIndex) -> Utf16FlyString {
    vm.current_executable().string_table[index.0 as usize].clone()
}

/// Value::as_array_exotic_object(): the arguments the bytecode collected into an Array for a call.
fn as_array_exotic_object(value: Value) -> Gc<Array> {
    value
        .as_object()
        .downcast::<Array>()
        .expect("the arguments of the call are an Array")
}

/// Throws a new error and hands it to the interpreter, the ASM_TRY of a throw completion.
fn throw_error(
    vm: &Vm,
    pc: u32,
    kind: ErrorKind,
    error_type: ErrorType,
    arguments: &[&dyn Utf16Display],
) -> SlowPathControl {
    match vm.throw_completion::<()>(kind, error_type, arguments) {
        Err(throw) => handle_asm_exception(vm, pc, throw.value()),
        Ok(()) => unreachable!("throw_completion always throws"),
    }
}

/// FunctionObject::internal_call(), which every class of function object has.
fn internal_call(
    vm: &Vm,
    function: Gc<FunctionObject>,
    callee_context: &ExecutionContext,
    this_value: Value,
) -> ThrowCompletionOr<Value> {
    let function = function_object_as_object(function);
    let Some(internal_call) = function.internal_call_method() else {
        unimplemented_runtime_function(&format!("[[Call]] of a {}", function.class().name), 0);
    };
    internal_call(&function, vm, callee_context, this_value)
}

/// FunctionObject::internal_construct(), which every class of constructor has.
fn internal_construct(
    vm: &Vm,
    function: Gc<FunctionObject>,
    callee_context: &ExecutionContext,
    new_target: Gc<FunctionObject>,
) -> ThrowCompletionOr<Gc<Object>> {
    let function = function_object_as_object(function);
    let Some(internal_construct) = function.internal_construct_method() else {
        unimplemented_runtime_function(&format!("[[Construct]] of a {}", function.class().name), 0);
    };
    internal_construct(&function, vm, callee_context, new_target)
}

/// What FunctionObject::get_stack_frame_info() reports for a call of `function` with `argument_count` arguments.
fn stack_frame_info_for_call(vm: &Vm, function: Gc<FunctionObject>, argument_count: u32) -> StackFrameInfo {
    let mut stack_frame_info = StackFrameInfo {
        argument_count,
        ..Default::default()
    };
    function.get_stack_frame_info(vm, &mut stack_frame_info);
    stack_frame_info
}

fn argument_count_of(count: usize) -> u32 {
    u32::try_from(count).expect("the argument count fits in u32")
}

pub fn stack_overflow(vm: &Vm, pc: u32) -> SlowPathControl {
    throw_error(vm, pc, ErrorKind::InternalError, ErrorType::CallStackSizeExceeded, &[])
}

pub fn get_super_constructor(vm: &Vm, pc: u32, values: &mut op::GetSuperConstructorValues) -> SlowPathControl {
    let super_constructor = abstract_operations::get_super_constructor(vm);
    values.dst = super_constructor.map_or(Value::NULL, Value::from_object);
    SlowPathControl::continue_at(pc + op::GetSuperConstructor::LENGTH)
}

/// The element key operands NewClass stores after its fixed fields.
fn element_key_operands(instruction: &op::NewClass) -> &[OptionalOperand] {
    // SAFETY: The bytecode stores the instruction's element_keys_count element key operands right after it.
    unsafe {
        core::slice::from_raw_parts(
            instruction.element_keys.as_ptr(),
            instruction.element_keys_count as usize,
        )
    }
}

pub fn new_class(
    vm: &Vm,
    pc: u32,
    instruction: &op::NewClass,
    values: &mut op::NewClassValues,
    element_keys: &mut [Value],
) -> SlowPathControl {
    let mut super_class = Value::UNDEFINED;
    if instruction.super_class.get().is_some() {
        super_class = values.super_class;
    }
    // NB: The element keys stay in the operand record the interpreter passed, which lives on its stack, so the keys
    //     of elements without one become undefined in place.
    for (element_key, operand) in element_keys.iter_mut().zip(element_key_operands(instruction)) {
        if operand.get().is_none() {
            *element_key = Value::UNDEFINED;
        }
    }

    let class_environment = values.class_environment.as_environment();
    let outer_environment = running_execution_context(vm).lexical_environment.get();

    let executable = vm.current_executable();
    let blueprint = executable.class_blueprint(instruction.class_blueprint_index);

    let mut binding_name = None;
    let class_name;
    if !blueprint.has_name
        && let Some(lhs_name) = instruction.lhs_name.get()
    {
        class_name = executable.get_identifier(lhs_name).clone();
    } else {
        class_name = blueprint.name.clone();
        binding_name = Some(class_name.clone());
    }

    let retval = asm_try!(
        vm,
        pc,
        construct_class(
            vm,
            blueprint,
            executable,
            Some(class_environment),
            outer_environment,
            super_class,
            element_keys,
            binding_name.as_ref(),
            &class_name,
        )
    );
    values.dst = Value::from_object(retval);
    SlowPathControl::continue_at(pc + instruction.length())
}

#[cold]
fn throw_type_error_for_asm_callee(
    vm: &Vm,
    callee: Value,
    callee_type: &str,
    expression_string: Option<StringTableIndex>,
) -> ThrowCompletionOr<()> {
    if let Some(expression_string) = expression_string {
        return vm.throw_completion(
            ErrorKind::TypeError,
            ErrorType::IsNotAEvaluatedFrom,
            &[&callee, &callee_type, &get_string(vm, expression_string)],
        );
    }

    vm.throw_completion(ErrorKind::TypeError, ErrorType::IsNotA, &[&callee, &callee_type])
}

fn throw_if_needed_for_asm_call(
    vm: &Vm,
    callee: Value,
    call_type: CallType,
    expression_string: Option<StringTableIndex>,
) -> ThrowCompletionOr<()> {
    if (call_type == CallType::Call || call_type == CallType::DirectEval) && !callee.is_function() {
        return throw_type_error_for_asm_callee(vm, callee, "function", expression_string);
    }
    if call_type == CallType::Construct && !callee.is_constructor() {
        return throw_type_error_for_asm_callee(vm, callee, "constructor", expression_string);
    }
    Ok(())
}

/// Whether `callee` is the realm's %eval%, which makes a direct eval call evaluate its argument.
fn is_intrinsic_eval_function(vm: &Vm, callee: Value) -> bool {
    let realm = vm.current_realm().expect("a call runs in a realm");
    callee == Value::from_object(realm.eval_function())
}

/// The argument PerformEval evaluates: the callee frame's first argument, or undefined.
fn eval_argument(callee_context: &ExecutionContext) -> Value {
    if callee_context.argument_count.get() > 0 {
        callee_context.arguments()[0].get()
    } else {
        Value::UNDEFINED
    }
}

/// Fills the argument slots of a callee frame with `arguments`, and the remaining ones with undefined.
fn copy_arguments_into_callee_context(callee_context: &ExecutionContext, arguments: &[Value]) {
    let callee_context_argument_values = callee_context.arguments();
    for (slot, argument) in callee_context_argument_values.iter().zip(arguments) {
        slot.set(*argument);
    }
    for slot in &callee_context_argument_values[arguments.len()..] {
        slot.set(Value::UNDEFINED);
    }
    callee_context
        .passed_argument_count
        .set(argument_count_of(arguments.len()));
}

#[inline(never)]
#[allow(clippy::too_many_arguments)]
fn execute_asm_call(
    call_type: CallType,
    vm: &Vm,
    callee: Value,
    this_value: Value,
    arguments: &[Value],
    dst: &mut Value,
    expression_string: Option<StringTableIndex>,
    strict: Strict,
) -> ThrowCompletionOr<()> {
    throw_if_needed_for_asm_call(vm, callee, call_type, expression_string)?;

    let function = callee.as_function();

    let argument_count = argument_count_of(arguments.len());
    let stack_frame_info = stack_frame_info_for_call(vm, function, argument_count);

    let stack = vm.interpreter_stack();
    let stack_mark = stack.top.get();
    let Some(callee_context) = stack.allocate(
        stack_frame_info.registers_and_locals_count,
        stack_frame_info.constant_count,
        argument_count.max(stack_frame_info.argument_count),
    ) else {
        return vm.throw_completion(ErrorKind::InternalError, ErrorType::CallStackSizeExceeded, &[]);
    };
    let _deallocate_guard = ScopeGuard::new(|| {
        if stack.top.get() > stack_mark {
            stack.deallocate(stack_mark);
        }
    });
    // SAFETY: The frame was just allocated and stays allocated until the guard frees it.
    let callee_context = unsafe { callee_context.as_ref() };

    copy_arguments_into_callee_context(callee_context, arguments);

    let retval = if call_type == CallType::DirectEval {
        if is_intrinsic_eval_function(vm, callee) {
            perform_eval(
                vm,
                eval_argument(callee_context),
                caller_mode_of(strict),
                EvalMode::Direct,
            )?
        } else {
            internal_call(vm, function, callee_context, this_value)?
        }
    } else if call_type == CallType::Construct {
        Value::from_object(internal_construct(vm, function, callee_context, function)?)
    } else {
        internal_call(vm, function, callee_context, this_value)?
    };
    *dst = retval;
    Ok(())
}

pub fn call(
    vm: &Vm,
    pc: u32,
    instruction: &op::Call,
    values: &mut op::CallValues,
    arguments: &[Value],
) -> SlowPathControl {
    if let Some(builtin) = value_as_native_javascript_backed_function(values.callee)
        && let Some(executable) = builtin.inline_call_executable(vm)
    {
        // NB: Stack traces show the caller at its program counter.
        vm.running_execution_context_ref().program_counter.set(pc);
        if vm
            .push_builtin_inline_frame(
                builtin,
                executable,
                arguments,
                pc + instruction.length(),
                instruction.dst.0,
                values.this_value,
            )
            .is_some()
        {
            return SlowPathControl::dispatch_at(0);
        }
    }
    asm_try!(
        vm,
        pc,
        execute_asm_call(
            CallType::Call,
            vm,
            values.callee,
            values.this_value,
            arguments,
            &mut values.dst,
            instruction.expression_string.get(),
            strict_of(&instruction.header),
        )
    );
    SlowPathControl::continue_at(pc + instruction.length())
}

fn call_direct_eval_impl(
    vm: &Vm,
    callee: Value,
    this_value: Value,
    arguments: &[Value],
    dst: &mut Value,
    expression_string: Option<StringTableIndex>,
    strict: Strict,
) -> ThrowCompletionOr<()> {
    throw_if_needed_for_asm_call(vm, callee, CallType::DirectEval, expression_string)?;

    let function = callee.as_function();

    let argument_count = argument_count_of(arguments.len());
    let stack_frame_info = stack_frame_info_for_call(vm, function, argument_count);

    let stack = vm.interpreter_stack();
    let stack_mark = stack.top.get();
    let Some(callee_context) = stack.allocate(
        stack_frame_info.registers_and_locals_count,
        stack_frame_info.constant_count,
        argument_count.max(stack_frame_info.argument_count),
    ) else {
        return vm.throw_completion(ErrorKind::InternalError, ErrorType::CallStackSizeExceeded, &[]);
    };
    let _deallocate_guard = ScopeGuard::new(|| stack.deallocate(stack_mark));
    // SAFETY: The frame was just allocated and stays allocated until the guard frees it.
    let callee_context = unsafe { callee_context.as_ref() };

    copy_arguments_into_callee_context(callee_context, arguments);

    let retval = if is_intrinsic_eval_function(vm, callee) {
        perform_eval(
            vm,
            eval_argument(callee_context),
            caller_mode_of(strict),
            EvalMode::Direct,
        )?
    } else {
        internal_call(vm, function, callee_context, this_value)?
    };
    *dst = retval;
    Ok(())
}

pub fn call_direct_eval(
    vm: &Vm,
    pc: u32,
    instruction: &op::CallDirectEval,
    values: &mut op::CallDirectEvalValues,
    arguments: &[Value],
) -> SlowPathControl {
    asm_try!(
        vm,
        pc,
        call_direct_eval_impl(
            vm,
            values.callee,
            values.this_value,
            arguments,
            &mut values.dst,
            instruction.expression_string.get(),
            strict_of(&instruction.header),
        )
    );
    SlowPathControl::continue_at(pc + instruction.length())
}

#[allow(clippy::too_many_arguments)]
fn call_with_argument_array_impl(
    call_type: CallType,
    vm: &Vm,
    callee: Value,
    this_value: Value,
    arguments: Value,
    dst: &mut Value,
    expression_string: Option<StringTableIndex>,
    strict: Strict,
) -> ThrowCompletionOr<()> {
    throw_if_needed_for_asm_call(vm, callee, call_type, expression_string)?;

    let function = callee.as_function();

    let argument_array = as_array_exotic_object(arguments);
    let argument_array_length = argument_array.indexed_array_like_size();

    let stack_frame_info = stack_frame_info_for_call(vm, function, argument_array_length);

    let stack = vm.interpreter_stack();
    let stack_mark = stack.top.get();
    let Some(callee_context) = stack.allocate(
        stack_frame_info.registers_and_locals_count,
        stack_frame_info.constant_count,
        argument_array_length.max(stack_frame_info.argument_count),
    ) else {
        return vm.throw_completion(ErrorKind::InternalError, ErrorType::CallStackSizeExceeded, &[]);
    };
    let _deallocate_guard = ScopeGuard::new(|| {
        if stack.top.get() > stack_mark {
            stack.deallocate(stack_mark);
        }
    });
    // SAFETY: The frame was just allocated and stays allocated until the guard frees it.
    let callee_context = unsafe { callee_context.as_ref() };

    let callee_context_argument_values = callee_context.arguments();
    let insn_argument_count = argument_array_length as usize;

    for (index, slot) in callee_context_argument_values
        .iter()
        .take(insn_argument_count)
        .enumerate()
    {
        let index = u32::try_from(index).expect("the argument index fits in u32");
        if let Some(value) = argument_array.indexed_get(index) {
            slot.set(value.value);
        } else {
            slot.set(Value::UNDEFINED);
        }
    }
    for slot in &callee_context_argument_values[insn_argument_count..] {
        slot.set(Value::UNDEFINED);
    }
    callee_context.passed_argument_count.set(argument_array_length);

    let retval = if call_type == CallType::DirectEval && is_intrinsic_eval_function(vm, callee) {
        perform_eval(
            vm,
            eval_argument(callee_context),
            caller_mode_of(strict),
            EvalMode::Direct,
        )?
    } else if call_type == CallType::Construct {
        Value::from_object(internal_construct(vm, function, callee_context, function)?)
    } else {
        internal_call(vm, function, callee_context, this_value)?
    };

    *dst = retval;
    Ok(())
}

pub fn call_with_argument_array(
    vm: &Vm,
    pc: u32,
    instruction: &op::CallWithArgumentArray,
    values: &mut op::CallWithArgumentArrayValues,
) -> SlowPathControl {
    asm_try!(
        vm,
        pc,
        call_with_argument_array_impl(
            CallType::Call,
            vm,
            values.callee,
            values.this_value,
            values.arguments,
            &mut values.dst,
            instruction.expression_string.get(),
            strict_of(&instruction.header),
        )
    );
    SlowPathControl::continue_at(pc + op::CallWithArgumentArray::LENGTH)
}

pub fn call_direct_eval_with_argument_array(
    vm: &Vm,
    pc: u32,
    instruction: &op::CallDirectEvalWithArgumentArray,
    values: &mut op::CallDirectEvalWithArgumentArrayValues,
) -> SlowPathControl {
    asm_try!(
        vm,
        pc,
        call_with_argument_array_impl(
            CallType::DirectEval,
            vm,
            values.callee,
            values.this_value,
            values.arguments,
            &mut values.dst,
            instruction.expression_string.get(),
            strict_of(&instruction.header),
        )
    );
    SlowPathControl::continue_at(pc + op::CallDirectEvalWithArgumentArray::LENGTH)
}

/// The slow path of a call site that names a builtin taking one argument, which calls the builtin's implementation
/// directly when the callee is that builtin, and calls the callee otherwise.
macro_rules! define_unary_builtin_call_slow_path {
    ($snake_case_name:ident, $op:ident, $values:ident, $builtin:ident, $implementation:path) => {
        pub fn $snake_case_name(vm: &Vm, pc: u32, instruction: &op::$op, values: &mut op::$values) -> SlowPathControl {
            let arguments = [values.argument];
            let callee = values.callee;
            if callee.is_function() && callee.as_function().builtin() == Some(Builtin::$builtin) {
                values.dst = asm_try!(vm, pc, $implementation(vm, values.argument));
                return SlowPathControl::continue_at(pc + op::$op::LENGTH);
            }
            asm_try!(
                vm,
                pc,
                execute_asm_call(
                    CallType::Call,
                    vm,
                    callee,
                    values.this_value,
                    &arguments,
                    &mut values.dst,
                    instruction.expression_string.get(),
                    strict_of(&instruction.header),
                )
            );
            SlowPathControl::continue_at(pc + op::$op::LENGTH)
        }
    };
}

/// Like define_unary_builtin_call_slow_path, for a builtin taking two arguments.
macro_rules! define_binary_builtin_call_slow_path {
    ($snake_case_name:ident, $op:ident, $values:ident, $builtin:ident, $implementation:path) => {
        pub fn $snake_case_name(vm: &Vm, pc: u32, instruction: &op::$op, values: &mut op::$values) -> SlowPathControl {
            let arguments = [values.argument0, values.argument1];
            let callee = values.callee;
            if callee.is_function() && callee.as_function().builtin() == Some(Builtin::$builtin) {
                values.dst = asm_try!(vm, pc, $implementation(vm, values.argument0, values.argument1));
                return SlowPathControl::continue_at(pc + op::$op::LENGTH);
            }
            asm_try!(
                vm,
                pc,
                execute_asm_call(
                    CallType::Call,
                    vm,
                    callee,
                    values.this_value,
                    &arguments,
                    &mut values.dst,
                    instruction.expression_string.get(),
                    strict_of(&instruction.header),
                )
            );
            SlowPathControl::continue_at(pc + op::$op::LENGTH)
        }
    };
}

/// Like define_unary_builtin_call_slow_path, for a builtin taking no arguments whose implementation cannot throw.
macro_rules! define_nullary_builtin_call_slow_path {
    ($snake_case_name:ident, $op:ident, $values:ident, $builtin:ident, $implementation:path) => {
        pub fn $snake_case_name(vm: &Vm, pc: u32, instruction: &op::$op, values: &mut op::$values) -> SlowPathControl {
            let callee = values.callee;
            if callee.is_function() && callee.as_function().builtin() == Some(Builtin::$builtin) {
                values.dst = $implementation();
                return SlowPathControl::continue_at(pc + op::$op::LENGTH);
            }
            asm_try!(
                vm,
                pc,
                execute_asm_call(
                    CallType::Call,
                    vm,
                    callee,
                    values.this_value,
                    &[],
                    &mut values.dst,
                    instruction.expression_string.get(),
                    strict_of(&instruction.header),
                )
            );
            SlowPathControl::continue_at(pc + op::$op::LENGTH)
        }
    };
}

/// The slow path of a call site that names a builtin the interpreter only handles in its fast path, which calls the
/// callee with `$argument_fields` from the operand record.
macro_rules! define_generic_builtin_call_slow_path {
    ($snake_case_name:ident, $op:ident, $values:ident $(, $argument_field:ident)*) => {
        pub fn $snake_case_name(vm: &Vm, pc: u32, instruction: &op::$op, values: &mut op::$values) -> SlowPathControl {
            let arguments = [$(values.$argument_field),*];
            asm_try!(
                vm,
                pc,
                execute_asm_call(
                    CallType::Call,
                    vm,
                    values.callee,
                    values.this_value,
                    &arguments,
                    &mut values.dst,
                    instruction.expression_string.get(),
                    strict_of(&instruction.header),
                )
            );
            SlowPathControl::continue_at(pc + op::$op::LENGTH)
        }
    };
}

define_unary_builtin_call_slow_path!(
    call_builtin_math_abs,
    CallBuiltinMathAbs,
    CallBuiltinMathAbsValues,
    MathAbs,
    math_object::abs_impl
);
define_unary_builtin_call_slow_path!(
    call_builtin_math_log,
    CallBuiltinMathLog,
    CallBuiltinMathLogValues,
    MathLog,
    math_object::log_impl
);
define_binary_builtin_call_slow_path!(
    call_builtin_math_pow,
    CallBuiltinMathPow,
    CallBuiltinMathPowValues,
    MathPow,
    math_object::pow_impl
);
define_unary_builtin_call_slow_path!(
    call_builtin_math_exp,
    CallBuiltinMathExp,
    CallBuiltinMathExpValues,
    MathExp,
    math_object::exp_impl
);
define_unary_builtin_call_slow_path!(
    call_builtin_math_ceil,
    CallBuiltinMathCeil,
    CallBuiltinMathCeilValues,
    MathCeil,
    math_object::ceil_impl
);
define_unary_builtin_call_slow_path!(
    call_builtin_math_floor,
    CallBuiltinMathFloor,
    CallBuiltinMathFloorValues,
    MathFloor,
    math_object::floor_impl
);
define_binary_builtin_call_slow_path!(
    call_builtin_math_imul,
    CallBuiltinMathImul,
    CallBuiltinMathImulValues,
    MathImul,
    math_object::imul_impl
);
define_nullary_builtin_call_slow_path!(
    call_builtin_math_random,
    CallBuiltinMathRandom,
    CallBuiltinMathRandomValues,
    MathRandom,
    math_object::random_impl
);
define_unary_builtin_call_slow_path!(
    call_builtin_math_round,
    CallBuiltinMathRound,
    CallBuiltinMathRoundValues,
    MathRound,
    math_object::round_impl
);
define_unary_builtin_call_slow_path!(
    call_builtin_math_sqrt,
    CallBuiltinMathSqrt,
    CallBuiltinMathSqrtValues,
    MathSqrt,
    math_object::sqrt_impl
);
define_unary_builtin_call_slow_path!(
    call_builtin_math_sin,
    CallBuiltinMathSin,
    CallBuiltinMathSinValues,
    MathSin,
    math_object::sin_impl
);
define_unary_builtin_call_slow_path!(
    call_builtin_math_cos,
    CallBuiltinMathCos,
    CallBuiltinMathCosValues,
    MathCos,
    math_object::cos_impl
);
define_unary_builtin_call_slow_path!(
    call_builtin_math_tan,
    CallBuiltinMathTan,
    CallBuiltinMathTanValues,
    MathTan,
    math_object::tan_impl
);
define_generic_builtin_call_slow_path!(
    call_builtin_regexp_prototype_exec,
    CallBuiltinRegExpPrototypeExec,
    CallBuiltinRegExpPrototypeExecValues,
    argument
);
define_generic_builtin_call_slow_path!(
    call_builtin_regexp_prototype_replace,
    CallBuiltinRegExpPrototypeReplace,
    CallBuiltinRegExpPrototypeReplaceValues,
    argument0,
    argument1
);
define_generic_builtin_call_slow_path!(
    call_builtin_regexp_prototype_split,
    CallBuiltinRegExpPrototypeSplit,
    CallBuiltinRegExpPrototypeSplitValues,
    argument0,
    argument1
);
define_generic_builtin_call_slow_path!(
    call_builtin_ordinary_has_instance,
    CallBuiltinOrdinaryHasInstance,
    CallBuiltinOrdinaryHasInstanceValues,
    argument
);
define_generic_builtin_call_slow_path!(
    call_builtin_array_iterator_prototype_next,
    CallBuiltinArrayIteratorPrototypeNext,
    CallBuiltinArrayIteratorPrototypeNextValues
);
define_generic_builtin_call_slow_path!(
    call_builtin_map_iterator_prototype_next,
    CallBuiltinMapIteratorPrototypeNext,
    CallBuiltinMapIteratorPrototypeNextValues
);
define_generic_builtin_call_slow_path!(
    call_builtin_set_iterator_prototype_next,
    CallBuiltinSetIteratorPrototypeNext,
    CallBuiltinSetIteratorPrototypeNextValues
);
define_generic_builtin_call_slow_path!(
    call_builtin_string_iterator_prototype_next,
    CallBuiltinStringIteratorPrototypeNext,
    CallBuiltinStringIteratorPrototypeNextValues
);
define_unary_builtin_call_slow_path!(
    call_builtin_string_from_char_code,
    CallBuiltinStringFromCharCode,
    CallBuiltinStringFromCharCodeValues,
    StringFromCharCode,
    string_constructor::from_char_code_impl
);
define_generic_builtin_call_slow_path!(
    call_builtin_string_prototype_char_code_at,
    CallBuiltinStringPrototypeCharCodeAt,
    CallBuiltinStringPrototypeCharCodeAtValues,
    argument
);
define_generic_builtin_call_slow_path!(
    call_builtin_string_prototype_char_at,
    CallBuiltinStringPrototypeCharAt,
    CallBuiltinStringPrototypeCharAtValues,
    argument
);

pub fn call_construct(
    vm: &Vm,
    pc: u32,
    instruction: &op::CallConstruct,
    values: &mut op::CallConstructValues,
    arguments: &[Value],
) -> SlowPathControl {
    let callee = values.callee;
    if let Some(function) = value_as_ecmascript_function_object(callee)
        && function.can_inline_call()
        && callee.is_constructor()
        && function.constructor_kind() == ConstructorKind::Base
        && !function.has_class_data()
    {
        let prototype = asm_try!(
            vm,
            pc,
            get_prototype_from_constructor(vm, function.as_function_object_gc(), Intrinsics::object_prototype)
        );
        let this_object = Object::create(
            vm,
            function.realm().expect("an ECMAScript function has a realm"),
            Some(prototype),
        );
        let Some(context) = vm.push_inline_frame(
            function,
            function.inline_call_executable(),
            arguments,
            pc + instruction.length(),
            instruction.dst.0,
            Value::from_object(this_object),
            Some(function.upcast()),
            true,
        ) else {
            return throw_error(vm, pc, ErrorKind::InternalError, ErrorType::CallStackSizeExceeded, &[]);
        };
        // Constructors retain their receiver even when the body never reads this.
        // SAFETY: The frame was just entered, and the interpreter runs it next.
        unsafe { context.as_ref() }
            .this_value
            .set(Value::from_object(this_object));
        return SlowPathControl::dispatch_at(0);
    }
    asm_try!(
        vm,
        pc,
        execute_asm_call(
            CallType::Construct,
            vm,
            values.callee,
            Value::UNDEFINED,
            arguments,
            &mut values.dst,
            instruction.expression_string.get(),
            strict_of(&instruction.header),
        )
    );
    SlowPathControl::continue_at(pc + instruction.length())
}

pub fn call_construct_with_argument_array(
    vm: &Vm,
    pc: u32,
    instruction: &op::CallConstructWithArgumentArray,
    values: &mut op::CallConstructWithArgumentArrayValues,
) -> SlowPathControl {
    asm_try!(
        vm,
        pc,
        call_with_argument_array_impl(
            CallType::Construct,
            vm,
            values.callee,
            Value::UNDEFINED,
            values.arguments,
            &mut values.dst,
            instruction.expression_string.get(),
            strict_of(&instruction.header),
        )
    );
    SlowPathControl::continue_at(pc + op::CallConstructWithArgumentArray::LENGTH)
}

pub fn super_call_with_argument_array(
    vm: &Vm,
    pc: u32,
    instruction: &op::SuperCallWithArgumentArray,
    values: &mut op::SuperCallWithArgumentArrayValues,
) -> SlowPathControl {
    let new_target = vm.get_new_target();
    assert!(new_target.is_object());

    let super_constructor = values.super_constructor;
    if !super_constructor.is_constructor() {
        running_execution_context(vm).program_counter.set(pc);
        return throw_error(
            vm,
            pc,
            ErrorKind::TypeError,
            ErrorType::NotAConstructor,
            &[&"Super constructor"],
        );
    }

    let function = super_constructor.as_function();

    let argument_array = as_array_exotic_object(values.arguments);
    let argument_array_length = if instruction.is_synthetic {
        length_of_array_like(vm, &argument_array).must()
    } else {
        u64::from(argument_array.indexed_array_like_size())
    };
    let argument_array_length = u32::try_from(argument_array_length).expect("the argument count fits in u32");

    let stack_frame_info = stack_frame_info_for_call(vm, function, argument_array_length);

    let stack = vm.interpreter_stack();
    let stack_mark = stack.top.get();
    let Some(callee_context) = stack.allocate(
        stack_frame_info.registers_and_locals_count,
        stack_frame_info.constant_count,
        argument_array_length.max(stack_frame_info.argument_count),
    ) else {
        running_execution_context(vm).program_counter.set(pc);
        return throw_error(vm, pc, ErrorKind::InternalError, ErrorType::CallStackSizeExceeded, &[]);
    };
    let _deallocate_guard = ScopeGuard::new(|| {
        if stack.top.get() > stack_mark {
            stack.deallocate(stack_mark);
        }
    });
    // SAFETY: The frame was just allocated and stays allocated until the guard frees it.
    let callee_context = unsafe { callee_context.as_ref() };

    let callee_context_argument_values = callee_context.arguments();
    let insn_argument_count = argument_array_length as usize;

    if instruction.is_synthetic {
        for (index, slot) in callee_context_argument_values
            .iter()
            .take(insn_argument_count)
            .enumerate()
        {
            slot.set(argument_array.get_without_side_effects(vm, &PropertyKey::from_number(index as u64)));
        }
    } else {
        for (index, slot) in callee_context_argument_values
            .iter()
            .take(insn_argument_count)
            .enumerate()
        {
            let index = u32::try_from(index).expect("the argument index fits in u32");
            if let Some(value) = argument_array.indexed_get(index) {
                slot.set(value.value);
            } else {
                slot.set(Value::UNDEFINED);
            }
        }
    }
    for slot in &callee_context_argument_values[insn_argument_count..] {
        slot.set(Value::UNDEFINED);
    }
    callee_context.passed_argument_count.set(argument_array_length);

    let result = asm_try!(
        vm,
        pc,
        internal_construct(vm, function, callee_context, new_target.as_function())
    );

    let this_environment = get_this_environment(vm)
        .downcast::<FunctionEnvironment>()
        .expect("super() runs in a function environment");
    asm_try!(vm, pc, this_environment.bind_this_value(vm, Value::from_object(result)));

    let f = as_ecmascript_function_object(this_environment.function_object())
        .expect("super() runs in an ECMAScript function");
    asm_try!(vm, pc, result.initialize_instance_elements(vm, f));

    values.dst = Value::from_object(result);
    SlowPathControl::continue_at(pc + op::SuperCallWithArgumentArray::LENGTH)
}

/// `value` as a builtin written in JavaScript, if it is one.
fn value_as_native_javascript_backed_function(value: Value) -> Option<Gc<NativeJavaScriptBackedFunction>> {
    if !value.is_object() {
        return None;
    }
    value.as_object().downcast::<NativeJavaScriptBackedFunction>()
}

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

    // NB: Stack traces show the caller at its program counter.
    vm.running_execution_context_ref().program_counter.set(pc);
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

fn function_name_prefix_of(raw_prefix: u32) -> FunctionNamePrefix {
    match raw_prefix {
        prefix if prefix == FunctionNamePrefix::None as u32 => FunctionNamePrefix::None,
        prefix if prefix == FunctionNamePrefix::Get as u32 => FunctionNamePrefix::Get,
        prefix if prefix == FunctionNamePrefix::Set as u32 => FunctionNamePrefix::Set,
        _ => unreachable!("{raw_prefix} is not a function name prefix"),
    }
}

fn asm_function_name_prefix_to_string(prefix: FunctionNamePrefix) -> Option<&'static str> {
    match prefix {
        FunctionNamePrefix::None => None,
        FunctionNamePrefix::Get => Some("get"),
        FunctionNamePrefix::Set => Some("set"),
    }
}

pub fn set_function_name(
    vm: &Vm,
    pc: u32,
    instruction: &op::SetFunctionName,
    values: &mut op::SetFunctionNameValues,
) -> SlowPathControl {
    let function = value_as_ecmascript_function_object(values.function);
    let Some(function) = function.filter(|function| function.name().is_empty()) else {
        return SlowPathControl::continue_at(pc + op::SetFunctionName::LENGTH);
    };

    let property_key = asm_try!(vm, pc, values.name.to_property_key(vm));
    function.set_inferred_name(
        vm,
        &ClassElementName::PropertyKey(property_key),
        asm_function_name_prefix_to_string(function_name_prefix_of(instruction.prefix)),
    );
    SlowPathControl::continue_at(pc + op::SetFunctionName::LENGTH)
}

pub fn create_rest_params(
    vm: &Vm,
    pc: u32,
    instruction: &op::CreateRestParams,
    values: &mut op::CreateRestParamsValues,
) -> SlowPathControl {
    let context = running_execution_context(vm);
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
    let context = running_execution_context(vm);
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
            ecmascript_function.mapped_argument_names(),
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

pub fn new_function(
    vm: &Vm,
    pc: u32,
    instruction: &op::NewFunction,
    values: &mut op::NewFunctionValues,
) -> SlowPathControl {
    let shared_data = vm
        .current_executable()
        .shared_function_data(instruction.shared_function_data_index);
    let realm = vm.current_realm().expect("there is a current realm");

    let prototype = match shared_data.kind() {
        FunctionKind::Normal => realm.function_prototype(),
        FunctionKind::Generator => realm.generator_function_prototype(),
        FunctionKind::Async => realm.async_function_prototype(),
        FunctionKind::AsyncGenerator => realm.async_generator_function_prototype(),
    };

    let function = EcmascriptFunctionObject::create_from_function_data_with_prototype(
        vm,
        realm,
        shared_data,
        vm.lexical_environment(),
        running_execution_context(vm).private_environment.get(),
        prototype,
    );

    if instruction.home_object.get().is_some() {
        let home_object_value = values.home_object;
        function.make_method(home_object_value.as_object());
    }

    values.dst = Value::from_object(function);
    SlowPathControl::continue_at(pc + op::NewFunction::LENGTH)
}

/// Handles an exception a raw native function the interpreter called threw.
pub fn handle_raw_native_exception(vm: &Vm, exception: Value) -> SlowPathControl {
    let callee_frame = running_execution_context(vm);
    assert!(!callee_frame.caller_frame.get().is_null());

    // Raw-native asm calls keep their callee frame off the VM execution
    // context stack, so we have to unwind it manually before exception
    // dispatch. Match VM::handle_exception()'s inline-frame semantics by
    // probing the caller with a PC inside the Call instruction.
    let caller_pc = callee_frame.caller_return_pc.get();
    vm.unwind_inline_frame_for_exception();
    handle_asm_exception(vm, caller_pc - 1, exception)
}
