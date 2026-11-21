/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The slow paths for calls, function and class creation, and their helpers.

use ak::ScopeGuard;
use core::ops::Deref;
use libjs_abi::{ArgumentsKind, FunctionNamePrefix};

use super::strict_at;
use crate::bytecode::encoding::OptionalOperand;
use crate::bytecode::op;
use crate::bytecode::property_access::Strict;
use crate::gc::root::{MarkedValues, MarkedVec};
use crate::interpreter::runtime_functions::{
    SlowPathControl, asm_try, handle_asm_exception, unimplemented_runtime_function,
};
use crate::interpreter::slow_paths::feedback;
use crate::interpreter::vm::{EvalMode, Vm};
use crate::layout::cell::{CellHeader, Gc};
use crate::layout::execution_context::ExecutionContext;
use crate::layout::feedback::CallFeedbackForwarding;
use crate::layout::value::Value;
use crate::runtime::abstract_operations::{
    self, CallerMode, create_mapped_arguments_object, create_unmapped_arguments_object, function_object_as_object,
    function_with_frame_as_this_environment, get_this_environment, perform_eval,
};
use crate::runtime::array::Array;
use crate::runtime::array_iterator::packed_array_iteration_is_unobservable;
use crate::runtime::bound_function::BoundFunction;
use crate::runtime::class_construction::ClassHeritage;
use crate::runtime::class_construction::class_proto_parent;
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
use crate::runtime::iterator::get_iterator_values;
use crate::runtime::native_javascript_backed_function::NativeJavaScriptBackedFunction;
use crate::runtime::object::{Object, StackFrameInfo};
use crate::runtime::property_attributes::{Attribute, DEFAULT_ATTRIBUTES, PropertyAttributes};
use crate::runtime::property_key::PropertyKey;
use crate::runtime::realm::Realm;
use crate::runtime::realm_watchpoints;
use crate::runtime::shared_function_instance_data::{ClassFieldInitializerName, ConstructorKind, FunctionKind};
use crate::utf16::Utf16Display;

/// Op::CallType: how a call instruction calls its callee.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum CallType {
    Call,
    Construct,
    DirectEval,
}

fn running_execution_context(vm: &Vm) -> &ExecutionContext {
    // NB: Callers only use the reference before anything can unwind the frame.
    vm.running_execution_context_if_any()
        .expect("a slow path runs in a running frame")
}

fn caller_mode_of(strict: Strict) -> CallerMode {
    if strict == Strict::Yes {
        CallerMode::Strict
    } else {
        CallerMode::NonStrict
    }
}

/// Value::as_array_exotic_object(): the arguments the bytecode collected into an Array for a call.
fn as_array_exotic_object(value: Value) -> Gc<Array> {
    value
        .as_object()
        .downcast::<Array>()
        .expect("the arguments of the call are an Array")
}

/// Throws a new error and hands it to the interpreter, the ASM_TRY of a throw completion.
pub(crate) fn throw_error(
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
fn stack_frame_info_for_call(
    vm: &Vm,
    function: Gc<FunctionObject>,
    argument_count: u32,
) -> ThrowCompletionOr<StackFrameInfo> {
    let mut stack_frame_info = StackFrameInfo {
        argument_count,
        ..Default::default()
    };
    function.get_stack_frame_info(vm, &mut stack_frame_info)?;
    Ok(stack_frame_info)
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
    instruction.element_keys()
}

/// The [[ClassFieldInitializerName]] of the running field initializer, whose field has a computed key.
pub fn get_class_field_initializer_name(
    vm: &Vm,
    pc: u32,
    values: &mut op::GetClassFieldInitializerNameValues,
) -> SlowPathControl {
    let function = running_execution_context(vm)
        .function
        .get()
        .and_then(as_ecmascript_function_object)
        .expect("a field initializer is an ECMAScript function");
    let ClassFieldInitializerName::PropertyKey(name) = function.class_field_initializer_name() else {
        unreachable!("a field with a computed key is named by a property key");
    };
    values.dst = name.to_value(vm);
    SlowPathControl::continue_at(pc + op::GetClassFieldInitializerName::LENGTH)
}

pub fn get_class_proto_parent(vm: &Vm, pc: u32, values: &mut op::GetClassProtoParentValues) -> SlowPathControl {
    values.dst = asm_try!(vm, pc, class_proto_parent(vm, values.super_class));
    SlowPathControl::continue_at(pc + op::GetClassProtoParent::LENGTH)
}

pub fn new_class(
    vm: &Vm,
    pc: u32,
    instruction: &op::NewClass,
    values: &mut op::NewClassValues,
    element_keys: &mut [Value],
) -> SlowPathControl {
    let mut heritage = None;
    if instruction.super_class.get().is_some() {
        assert!(instruction.proto_parent.get().is_some());
        heritage = Some(ClassHeritage {
            super_class: values.super_class,
            proto_parent: values.proto_parent,
        });
    }
    // NB: The element keys stay in the operand record the interpreter passed, which lives on its stack, so the keys
    //     of elements without one become undefined in place.
    for (element_key, operand) in element_keys.iter_mut().zip(element_key_operands(instruction)) {
        if operand.get().is_none() {
            *element_key = Value::UNDEFINED;
        }
    }
    let element_keys = super::operand_list(vm, instruction.element_keys_are_in_array, element_keys);
    let element_keys = element_keys.values();
    let element_keys = &*element_keys;

    let class_environment = values.class_environment.as_environment();
    let outer_environment = running_execution_context(vm).lexical_environment.get();

    let executable = vm.current_executable();
    let blueprint = executable.class_blueprint(instruction.class_blueprint_index);

    let mut binding_name = None;
    let class_name;
    if !blueprint.has_name
        && let Some(lhs_name) = instruction.lhs_name.index_in(executable.bytecode())
    {
        class_name = ClassElementName::PropertyKey(PropertyKey::from(executable.identifier(lhs_name).clone()));
    } else if !blueprint.has_name && instruction.computed_name.get().is_some() {
        class_name = ClassElementName::PropertyKey(asm_try!(vm, pc, values.computed_name.to_property_key(vm)));
    } else {
        class_name = ClassElementName::PropertyKey(PropertyKey::from(blueprint.name.clone()));
        binding_name = Some(blueprint.name.clone());
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
            heritage,
            element_keys,
            binding_name.as_ref(),
            &class_name,
        )
    );
    values.dst = Value::from_object(retval);
    SlowPathControl::continue_at(pc + instruction.length())
}

#[cold]
fn throw_type_error_for_asm_callee(vm: &Vm, callee: Value, callee_type: &str) -> ThrowCompletionOr<()> {
    let program_counter = vm.running_execution_context_ref().program_counter.get();
    if let Some(text) = vm.current_executable().callee_text_at(program_counter) {
        return vm.throw_completion(
            ErrorKind::TypeError,
            ErrorType::IsNotAEvaluatedFrom,
            &[&callee, &callee_type, &text],
        );
    }
    vm.throw_completion(ErrorKind::TypeError, ErrorType::IsNotA, &[&callee, &callee_type])
}

pub(crate) fn throw_if_needed_for_asm_call(vm: &Vm, callee: Value, call_type: CallType) -> ThrowCompletionOr<()> {
    if (call_type == CallType::Call || call_type == CallType::DirectEval) && !callee.is_function() {
        return throw_type_error_for_asm_callee(vm, callee, "function");
    }
    if call_type == CallType::Construct && !callee.is_constructor() {
        return throw_type_error_for_asm_callee(vm, callee, "constructor");
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

#[inline]
#[allow(clippy::too_many_arguments)]
pub(crate) fn execute_asm_call(
    call_type: CallType,
    vm: &Vm,
    callee: Value,
    this_value: Value,
    arguments: &[Value],
    dst: &mut Value,
    strict: Strict,
) -> ThrowCompletionOr<()> {
    execute_asm_call_with_argument_writer(
        call_type,
        vm,
        callee,
        this_value,
        argument_count_of(arguments.len()),
        dst,
        strict,
        |callee_context| copy_arguments_into_callee_context(callee_context, arguments),
    )
}

/// A call whose arguments get written straight into the callee's frame. Allocating that frame may compile the callee
/// and collect garbage, but runs no JavaScript.
#[inline(never)]
#[allow(clippy::too_many_arguments)]
fn execute_asm_call_with_argument_writer(
    call_type: CallType,
    vm: &Vm,
    callee: Value,
    this_value: Value,
    argument_count: u32,
    dst: &mut Value,
    strict: Strict,
    write_arguments: impl FnOnce(&ExecutionContext),
) -> ThrowCompletionOr<()> {
    throw_if_needed_for_asm_call(vm, callee, call_type)?;

    let function = callee.as_function();

    let stack_frame_info = stack_frame_info_for_call(vm, function, argument_count)?;

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

    write_arguments(callee_context);

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
    if values.callee.is_object()
        && let Some(bound) = values.callee.as_object().downcast::<BoundFunction>()
    {
        let target = Value::from_object(bound.bound_target_function()).as_cell();
        let argument_count = bound.bound_arguments_count() + arguments.len();
        feedback::record_forwarded_call(
            vm,
            vm.running_execution_context_ref(),
            pc,
            values.callee.as_cell(),
            target,
            CallFeedbackForwarding::Bound,
            argument_count,
        );
    }
    if let Some(builtin) = value_as_native_javascript_backed_function(values.callee)
        && let Some(executable) = builtin.inline_call_executable(vm)
    {
        record_callback(vm, vm.running_execution_context_ref(), pc, builtin, arguments);
        // NB: Stack traces show the caller at its program counter.
        vm.running_execution_context_ref().program_counter.set(pc);
        if vm
            .push_builtin_inline_frame(
                builtin,
                executable,
                arguments,
                argument_count_of(arguments.len()),
                pc + instruction.length(),
                instruction.dst.0.into(),
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
            strict_at(vm, pc),
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
    strict: Strict,
) -> ThrowCompletionOr<()> {
    throw_if_needed_for_asm_call(vm, callee, CallType::DirectEval)?;

    let function = callee.as_function();

    let argument_count = argument_count_of(arguments.len());
    let stack_frame_info = stack_frame_info_for_call(vm, function, argument_count)?;

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
            strict_at(vm, pc),
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
    strict: Strict,
) -> ThrowCompletionOr<()> {
    let argument_array = as_array_exotic_object(arguments);
    let argument_count = argument_array.indexed_array_like_size();
    execute_asm_call_with_argument_writer(
        call_type,
        vm,
        callee,
        this_value,
        argument_count,
        dst,
        strict,
        |callee_context| {
            let slots = callee_context.arguments();
            for (index, slot) in (0..argument_count).zip(slots) {
                slot.set(
                    argument_array
                        .indexed_get(index)
                        .map_or(Value::UNDEFINED, |value| value.value),
                );
            }
            for slot in &slots[argument_count as usize..] {
                slot.set(Value::UNDEFINED);
            }
            callee_context.passed_argument_count.set(argument_count);
        },
    )
}

pub fn call_with_argument_array(
    vm: &Vm,
    pc: u32,
    _instruction: &op::CallWithArgumentArray,
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
            strict_at(vm, pc),
        )
    );
    SlowPathControl::continue_at(pc + op::CallWithArgumentArray::LENGTH)
}

pub fn call_with_spread(
    vm: &Vm,
    pc: u32,
    _instruction: &op::CallWithSpread,
    values: &mut op::CallWithSpreadValues,
) -> SlowPathControl {
    let realm = vm.current_realm().expect("a call runs in a realm");
    if values.iterable.is_object()
        && let Some(array) = values.iterable.as_object().downcast::<Array>()
        && realm_watchpoints::array_is_iterated_unobservably(vm, realm, &array)
        && packed_array_iteration_is_unobservable(vm, realm, &array, realm.array_prototype_values_function())
    {
        // NB: Neither looking up @@iterator nor exhausting it can run JavaScript, so the elements can go straight into
        //     the callee's frame.
        let argument_count = array.indexed_packed_element_count();
        asm_try!(
            vm,
            pc,
            execute_asm_call_with_argument_writer(
                CallType::Call,
                vm,
                values.callee,
                values.this_value,
                argument_count,
                &mut values.dst,
                strict_at(vm, pc),
                |callee_context| {
                    let slots = callee_context.arguments();
                    for (index, slot) in (0..argument_count).zip(slots) {
                        slot.set(array.indexed_get(index).expect("the array is packed").value);
                    }
                    for slot in &slots[argument_count as usize..] {
                        slot.set(Value::UNDEFINED);
                    }
                    callee_context.passed_argument_count.set(argument_count);
                },
            )
        );
    } else {
        let arguments = MarkedVec::new(vm);
        let completion = get_iterator_values(vm, values.iterable, |value| {
            arguments.push(value);
            None
        });
        if completion.is_error() {
            return handle_asm_exception(vm, pc, completion.value());
        }
        asm_try!(
            vm,
            pc,
            execute_asm_call(
                CallType::Call,
                vm,
                values.callee,
                values.this_value,
                &arguments.values(),
                &mut values.dst,
                strict_at(vm, pc),
            )
        );
    }
    SlowPathControl::continue_at(pc + op::CallWithSpread::LENGTH)
}

pub fn call_direct_eval_with_argument_array(
    vm: &Vm,
    pc: u32,
    _instruction: &op::CallDirectEvalWithArgumentArray,
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
            strict_at(vm, pc),
        )
    );
    SlowPathControl::continue_at(pc + op::CallDirectEvalWithArgumentArray::LENGTH)
}

pub fn call_construct(
    vm: &Vm,
    pc: u32,
    instruction: &op::CallConstruct,
    values: &mut op::CallConstructValues,
    arguments: &[Value],
) -> SlowPathControl {
    let callee = values.callee;
    // NB: Class constructors cannot be called, so their code is no inline call executable, but a construct of a base
    //     class without fields runs it in an inline frame all the same.
    if let Some(function) = value_as_ecmascript_function_object(callee)
        && (function.can_inline_call() || function.is_class_constructor())
        && callee.is_constructor()
        && function.constructor_kind() == ConstructorKind::Base
        && !function.has_class_data()
        // NB: Taken before anything allocates: a collection could flush the code of the function otherwise, which this
        //     local keeps from happening (see `bytecode::flushing`).
        && let Some(executable) = function.bytecode_executable()
    {
        // NB: With room for the properties the constructor is known to add.
        let this_object = asm_try!(
            vm,
            pc,
            Object::create_from_constructor_for_construct(vm, function.as_function_object_gc(), function.shared_data())
        );
        let Some(context) = vm.push_inline_frame(
            function,
            executable,
            arguments,
            pc + instruction.length(),
            instruction.dst.0.into(),
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
            strict_at(vm, pc),
        )
    );
    SlowPathControl::continue_at(pc + instruction.length())
}

pub fn call_construct_with_argument_array(
    vm: &Vm,
    pc: u32,
    _instruction: &op::CallConstructWithArgumentArray,
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
            strict_at(vm, pc),
        )
    );
    SlowPathControl::continue_at(pc + op::CallConstructWithArgumentArray::LENGTH)
}

// 13.3.7.1 Runtime Semantics: Evaluation, https://tc39.es/ecma262/#sec-super-keyword-runtime-semantics-evaluation
// SuperCall : super Arguments
pub fn super_call_with_argument_array(
    vm: &Vm,
    pc: u32,
    _instruction: &op::SuperCallWithArgumentArray,
    values: &mut op::SuperCallWithArgumentArrayValues,
) -> SlowPathControl {
    let argument_array = as_array_exotic_object(values.arguments);
    let argument_count = argument_array.indexed_array_like_size();
    let result = match construct_for_super_call(vm, pc, values.super_constructor, argument_count, |slots| {
        for (index, slot) in slots.iter().enumerate() {
            let index = u32::try_from(index).expect("the argument index fits in u32");
            slot.set(
                argument_array
                    .indexed_get(index)
                    .map_or(Value::UNDEFINED, |value| value.value),
            );
        }
    }) {
        Ok(result) => result,
        Err(control) => return control,
    };
    if let Some(control) = bind_this_from_super_call_result(vm, pc, result) {
        return control;
    }
    values.dst = Value::from_object(result);
    SlowPathControl::continue_at(pc + op::SuperCallWithArgumentArray::LENGTH)
}

/// Steps 1, 2, 5 and 6 of the evaluation of `super(...)` for a SuperCall instruction, whose arguments are operands. A
/// BindThisFromSuperCall instruction does the rest once the super constructor returned into the destination.
pub fn super_call(
    vm: &Vm,
    pc: u32,
    instruction: &op::SuperCall,
    values: &mut op::SuperCallValues,
    arguments: &[Value],
) -> SlowPathControl {
    super_call_with_arguments(
        vm,
        pc,
        pc + instruction.length(),
        instruction.dst.0.into(),
        values.super_constructor,
        arguments,
        &mut values.dst,
    )
}

/// Steps 1, 2, 5 and 6 of the evaluation of `super(...)` for the SuperCallWithForwardedArguments instruction of a
/// default derived constructor, which passes on the arguments of the running frame.
pub fn super_call_with_forwarded_arguments(
    vm: &Vm,
    pc: u32,
    instruction: &op::SuperCallWithForwardedArguments,
    values: &mut op::SuperCallWithForwardedArgumentsValues,
) -> SlowPathControl {
    let frame = running_execution_context(vm);
    let mut storage = ArgumentStorage::empty();
    storage.fill_from_passed_arguments(vm, frame);
    let arguments = storage.values();
    super_call_with_arguments(
        vm,
        pc,
        pc + op::SuperCallWithForwardedArguments::LENGTH,
        instruction.dst.0.into(),
        values.super_constructor,
        &arguments,
        &mut values.dst,
    )
}

fn super_call_with_arguments(
    vm: &Vm,
    pc: u32,
    return_pc: u32,
    dst_raw: u32,
    super_constructor: Value,
    arguments: &[Value],
    dst: &mut Value,
) -> SlowPathControl {
    // OPTIMIZATION: An ordinary ECMAScript super constructor runs in an inline frame, like the construct fast path
    //               does. BindThisFromSuperCall checks the value the frame returns.
    if let Some(function) = value_as_ecmascript_function_object(super_constructor)
        && function.kind() == FunctionKind::Normal
        && super_constructor.is_constructor()
        && match function.constructor_kind() {
            ConstructorKind::Base => !function.has_class_data(),
            // NB: The frame of such a derived constructor returns its this value for undefined (see
            //     ExecutionContext::caller_is_derived_construct).
            ConstructorKind::Derived => !function.shared_data().function_environment_has_this_binding(),
        }
        // NB: Taken before anything allocates: a collection could flush the code of the function otherwise, which this
        //     local keeps from happening (see `bytecode::flushing`).
        && let Some(executable) = function.bytecode_executable()
    {
        let new_target = vm.get_new_target();
        assert!(new_target.is_object());
        let new_target = new_target.as_function();
        let mut this_object = None;
        if function.constructor_kind() == ConstructorKind::Base {
            // 10.2.2 [[Construct]], step 3.a: Let thisArgument be ? OrdinaryCreateFromConstructor(newTarget,
            // "%Object.prototype%").
            // NB: With room for the properties the constructor is known to add.
            this_object = Some(asm_try!(
                vm,
                pc,
                Object::create_from_constructor_for_construct(vm, new_target, function.shared_data())
            ));
        }
        let Some(context) = vm.push_inline_frame_without_this(
            function,
            executable,
            arguments,
            argument_count_of(arguments.len()),
            return_pc,
            dst_raw,
            Some(function_object_as_object(new_target)),
            this_object.is_some(),
        ) else {
            return throw_error(vm, pc, ErrorKind::InternalError, ErrorType::CallStackSizeExceeded, &[]);
        };
        // SAFETY: The frame was just entered, and the interpreter runs it next.
        let context = unsafe { context.as_ref() };
        if let Some(this_object) = this_object {
            vm.bind_this_in_inline_frame(function, context, Value::from_object(this_object));
            // Constructors retain their receiver even when the body never reads this.
            context.this_value.set(Value::from_object(this_object));
        } else {
            context.caller_is_construct.set(true);
            context.caller_is_derived_construct.set(true);
        }
        return SlowPathControl::dispatch_at(0);
    }

    let result =
        match construct_for_super_call(vm, pc, super_constructor, argument_count_of(arguments.len()), |slots| {
            for (slot, value) in slots.iter().zip(arguments) {
                slot.set(*value);
            }
        }) {
            Ok(result) => result,
            Err(control) => return control,
        };
    *dst = Value::from_object(result);
    SlowPathControl::continue_at(return_pc)
}

/// Steps 7 to 12 of the evaluation of `super(...)`, after a SuperCall instruction, for the value the super constructor
/// returned. An inline frame of a derived super constructor returns its this value (empty if super() was never called
/// in it) for undefined, or what it returned otherwise, which steps 12 to 17 of its [[Construct]] check here.
pub fn bind_this_from_super_call(vm: &Vm, pc: u32, values: &mut op::BindThisFromSuperCallValues) -> SlowPathControl {
    let result = values.result;
    if !result.is_object() {
        if result.is_empty() {
            return throw_error(
                vm,
                pc,
                ErrorKind::ReferenceError,
                ErrorType::ThisHasNotBeenInitialized,
                &[],
            );
        }
        return throw_error(
            vm,
            pc,
            ErrorKind::TypeError,
            ErrorType::DerivedConstructorReturningInvalidValue,
            &[],
        );
    }
    if let Some(control) = bind_this_from_super_call_result(vm, pc, result.as_object()) {
        return control;
    }
    SlowPathControl::continue_at(pc + op::BindThisFromSuperCall::LENGTH)
}

/// Steps 1, 2, 5 and 6 of the evaluation of `super(...)`: constructs with the super constructor and the new target of
/// the running function through the generic [[Construct]], with `fill_arguments` writing the arguments.
fn construct_for_super_call(
    vm: &Vm,
    pc: u32,
    super_constructor: Value,
    argument_count: u32,
    fill_arguments: impl FnOnce(&[std::cell::Cell<Value>]),
) -> Result<Gc<Object>, SlowPathControl> {
    // 1. Let newTarget be GetNewTarget().
    let new_target = vm.get_new_target();
    // 2. Assert: newTarget is a constructor.
    assert!(new_target.is_object());

    // 5. If IsConstructor(superCtor) is false, throw a TypeError exception.
    if !super_constructor.is_constructor() {
        running_execution_context(vm).program_counter.set(pc);
        return Err(throw_error(
            vm,
            pc,
            ErrorKind::TypeError,
            ErrorType::NotAConstructor,
            &[&"Super constructor"],
        ));
    }

    let function = super_constructor.as_function();
    let stack_frame_info = match stack_frame_info_for_call(vm, function, argument_count) {
        Ok(info) => info,
        Err(throw) => return Err(handle_asm_exception(vm, pc, throw.value())),
    };

    let stack = vm.interpreter_stack();
    let stack_mark = stack.top.get();
    let Some(callee_context) = stack.allocate(
        stack_frame_info.registers_and_locals_count,
        stack_frame_info.constant_count,
        argument_count.max(stack_frame_info.argument_count),
    ) else {
        running_execution_context(vm).program_counter.set(pc);
        return Err(throw_error(
            vm,
            pc,
            ErrorKind::InternalError,
            ErrorType::CallStackSizeExceeded,
            &[],
        ));
    };
    let _deallocate_guard = ScopeGuard::new(|| {
        if stack.top.get() > stack_mark {
            stack.deallocate(stack_mark);
        }
    });
    // SAFETY: The frame was just allocated and stays allocated until the guard frees it.
    let callee_context = unsafe { callee_context.as_ref() };

    let callee_context_argument_values = callee_context.arguments();
    let passed = argument_count as usize;
    fill_arguments(&callee_context_argument_values[..passed]);
    for slot in &callee_context_argument_values[passed..] {
        slot.set(Value::UNDEFINED);
    }
    callee_context.passed_argument_count.set(argument_count);

    // 6. Let result be ? Construct(superCtor, argList, newTarget).
    internal_construct(vm, function, callee_context, new_target.as_function())
        .map_err(|throw| handle_asm_exception(vm, pc, throw.value()))
}

/// Steps 7 to 12 of the evaluation of `super(...)` for `result`, what the super constructor returned. Returns the
/// control of a thrown exception.
fn bind_this_from_super_call_result(vm: &Vm, pc: u32, result: Gc<Object>) -> Option<SlowPathControl> {
    let f = if let Some(function) = function_with_frame_as_this_environment(vm) {
        // OPTIMIZATION: BindThisValue() of the function environment, for a running function whose frame answers for
        //               it. The empty this value of the frame means that the binding is not initialized.
        let frame = running_execution_context(vm);
        if !frame.this_value.get().is_empty() {
            return Some(throw_error(
                vm,
                pc,
                ErrorKind::ReferenceError,
                ErrorType::ThisIsAlreadyInitialized,
                &[],
            ));
        }
        frame.this_value.set(Value::from_object(result));
        function
    } else {
        // 7. Let thisER be GetThisEnvironment().
        // 8. Assert: thisER is a Function Environment Record.
        let this_environment = get_this_environment(vm)
            .downcast::<FunctionEnvironment>()
            .expect("super() runs in a function environment");
        // 9. Perform ? BindThisValue(thisER, result).
        if let Err(throw) = this_environment.bind_this_value(vm, Value::from_object(result)) {
            return Some(handle_asm_exception(vm, pc, throw.value()));
        }
        as_ecmascript_function_object(this_environment.function_object())
            .expect("super() runs in an ECMAScript function")
    };
    // 10. Let funcObj be thisER.[[FunctionObject]].
    // 11. Assert: funcObj is an ECMAScript function object.
    // 12. Perform ? InitializeInstanceElements(result, funcObj).
    // NB: JIT code initializes objects of the shape it always had here like this does (see
    //     EcmascriptFunctionObject::instance_elements_shape()).
    if f.has_class_data() {
        f.record_instance_elements_shape(vm, result.shape());
    }
    if let Err(throw) = result.initialize_instance_elements(vm, f) {
        return Some(handle_asm_exception(vm, pc, throw.value()));
    }
    None
}

/// Records the function a call of a builtin written in JavaScript passes it as its first argument, which the builtins
/// that take callbacks call back, as the call site's forwarded call.
pub(crate) fn record_callback(
    vm: &Vm,
    frame: &ExecutionContext,
    pc: u32,
    builtin: Gc<NativeJavaScriptBackedFunction>,
    arguments: &[Value],
) {
    let Some(callback) = arguments.first().filter(|callback| callback.is_function()) else {
        return;
    };
    feedback::record_forwarded_call(
        vm,
        frame,
        pc,
        builtin.upcast::<CellHeader>(),
        callback.as_cell(),
        CallFeedbackForwarding::Callback,
        0,
    );
}

/// `value` as a builtin written in JavaScript, if it is one.
pub(crate) fn value_as_native_javascript_backed_function(value: Value) -> Option<Gc<NativeJavaScriptBackedFunction>> {
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
        instruction.dst.0.into(),
        values.this_value,
        None,
        false,
    )
    .is_some()
}

fn function_name_prefix_of(raw_prefix: u8) -> FunctionNamePrefix {
    match raw_prefix {
        prefix if prefix == FunctionNamePrefix::None as u8 => FunctionNamePrefix::None,
        prefix if prefix == FunctionNamePrefix::Get as u8 => FunctionNamePrefix::Get,
        prefix if prefix == FunctionNamePrefix::Set as u8 => FunctionNamePrefix::Set,
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
    let arguments_object = if instruction.kind == ArgumentsKind::Mapped as u8 {
        let ecmascript_function =
            as_ecmascript_function_object(function).expect("only ECMAScript functions have mapped arguments objects");
        // NB: The bytecode leaves creating the bindings of the parameters that a mapped arguments object aliases to it,
        //     which makes them all at once in the function environment, in the order of the parameters.
        if instruction.creates_parameter_bindings {
            for name in ecmascript_function.parameter_binding_names().iter() {
                environment.create_mutable_binding(vm, name, false).must();
            }
        }
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

/// `arguments[index]` of a function without an arguments object, for an index that is not one of a passed argument as
/// an int32. The own properties of an arguments object and properties it would not find at all are read without one;
/// anything else looks the property up on an arguments object made for this one read.
pub fn get_argument(
    vm: &Vm,
    pc: u32,
    instruction: &op::GetArgument,
    values: &mut op::GetArgumentValues,
) -> SlowPathControl {
    let property_key = asm_try!(vm, pc, values.index.to_property_key(vm));
    let context = running_execution_context(vm);
    let passed_arguments = &context.arguments()[..context.passed_argument_count.get() as usize];
    let is_mapped = instruction.kind == ArgumentsKind::Mapped as u8;
    let function = || {
        context
            .function
            .get()
            .expect("an arguments object is created for a function")
    };
    let realm = vm.current_realm().expect("there is a current realm");
    let own_value = if property_key.is_number() {
        match passed_arguments.get(property_key.as_number() as usize) {
            Some(argument) => Some(argument.get()),
            None => object_prototype_value(vm, realm, &property_key),
        }
    } else if property_key == vm.names.length {
        Some(Value::from_f64(passed_arguments.len() as f64))
    } else if property_key == vm.names.callee {
        // NB: The callee of an unmapped arguments object is an accessor that throws, which the object made below runs.
        is_mapped.then(|| Value::from_object(function()))
    } else if property_key == PropertyKey::from(vm.well_known_symbols().iterator) {
        Some(Value::from_object(realm.array_prototype_values_function()))
    } else {
        object_prototype_value(vm, realm, &property_key)
    };
    if let Some(value) = own_value {
        values.dst = value;
        return SlowPathControl::continue_at(pc + op::GetArgument::LENGTH);
    }
    // FIXME: An accessor on %Object.prototype% gets a new unmapped arguments object as `this` at each read, not one
    //        object for the whole call, and what it writes to that object (such as `this[0]` or `this.length`) does
    //        not reach later reads of `arguments` or, in a function with mapped arguments, the parameters.
    let arguments_object = create_unmapped_arguments_object(vm, passed_arguments);
    if is_mapped {
        arguments_object.define_direct_property(
            vm,
            &vm.names.callee,
            Value::from_object(function()),
            PropertyAttributes::new(Attribute::WRITABLE | Attribute::CONFIGURABLE),
        );
    }
    values.dst = asm_try!(vm, pc, arguments_object.get(vm, &property_key));
    SlowPathControl::continue_at(pc + op::GetArgument::LENGTH)
}

/// What a lookup of `property_key`, which is no own property, gets from %Object.prototype% of `realm`, the prototype of
/// an arguments object, whose own prototype is null: undefined without the property, the value of a data property, and
/// `None` for an accessor, which needs a receiver.
fn object_prototype_value(vm: &Vm, realm: Gc<Realm>, property_key: &PropertyKey) -> Option<Value> {
    match realm.object_prototype().storage_get(vm, property_key) {
        None => Some(Value::UNDEFINED),
        Some(entry) if !entry.value.is_accessor() => Some(entry.value),
        Some(_) => None,
    }
}

pub fn new_function(
    vm: &Vm,
    pc: u32,
    instruction: &op::NewFunction,
    values: &mut op::NewFunctionValues,
) -> SlowPathControl {
    let home_object = instruction.home_object.get().map(|_| values.home_object);
    values.dst = new_function_object(vm, instruction.shared_function_data_index.into(), home_object);
    SlowPathControl::continue_at(pc + op::NewFunction::LENGTH)
}

pub fn new_function_wide(
    vm: &Vm,
    pc: u32,
    instruction: &op::NewFunctionWide,
    values: &mut op::NewFunctionWideValues,
) -> SlowPathControl {
    let home_object = instruction.home_object.get().map(|_| values.home_object);
    values.dst = new_function_object(vm, instruction.shared_function_data_index, home_object);
    SlowPathControl::continue_at(pc + op::NewFunctionWide::LENGTH)
}

/// The function object of a NewFunction, for the function at `shared_function_data_index` of the running executable.
fn new_function_object(vm: &Vm, shared_function_data_index: u32, home_object: Option<Value>) -> Value {
    let shared_data = vm.current_executable().shared_function_data(shared_function_data_index);
    let realm = vm.current_realm().expect("there is a current realm");

    let prototype = match shared_data.kind() {
        FunctionKind::Normal => realm.function_prototype(),
        FunctionKind::Generator => realm.generator_function_prototype(),
        FunctionKind::Async => realm.async_function_prototype(),
        FunctionKind::AsyncGenerator => realm.async_generator_function_prototype(),
    };

    let function = if let Some(home_object) = home_object {
        EcmascriptFunctionObject::create_method(
            vm,
            realm,
            shared_data,
            vm.lexical_environment(),
            running_execution_context(vm).private_environment.get(),
            home_object.as_object(),
            prototype,
        )
    } else {
        EcmascriptFunctionObject::create_from_function_data_with_prototype(
            vm,
            realm,
            shared_data,
            vm.lexical_environment(),
            running_execution_context(vm).private_environment.get(),
            prototype,
        )
    };

    Value::from_object(function)
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

/// The argument values of a call, which it keeps alive: few enough on the stack, which the garbage collector scans,
/// and others in a rooted list.
///
/// NB: The argument values cannot stay alive in the slots of the calling frame: frames of inlined calls that compiled
///     code pushes for a call are not initialized, and the garbage collector does not visit their registers and locals.
pub(crate) struct ArgumentStorage<'vm> {
    inline: [Value; ArgumentStorage::INLINE_CAPACITY],
    count: usize,
    rooted: Option<MarkedVec<'vm, Value>>,
}

impl<'vm> ArgumentStorage<'vm> {
    const INLINE_CAPACITY: usize = 16;

    /// Storage without values, which `fill()` puts in. They go in where the storage is, and not into storage that is
    /// moved there: a move leaves its source behind on the stack, where nothing clears it (see `drop()`).
    pub(crate) fn empty() -> Self {
        Self {
            inline: [Value::UNDEFINED; Self::INLINE_CAPACITY],
            count: 0,
            rooted: None,
        }
    }

    pub(crate) fn fill_from(&mut self, vm: &'vm Vm, values: impl ExactSizeIterator<Item = Value>) {
        self.fill(vm, values.len(), |slots| {
            for (slot, value) in slots.iter_mut().zip(values) {
                *slot = value;
            }
        });
    }

    /// Takes the arguments passed to the frame `frame`.
    pub(crate) fn fill_from_passed_arguments(&mut self, vm: &'vm Vm, frame: &ExecutionContext) {
        let passed_arguments = &frame.arguments()[..frame.passed_argument_count.get() as usize];
        self.fill_from(vm, passed_arguments.iter().map(|argument| argument.get()));
    }

    /// Takes `count` values, which `fill` writes into the slots it gets. Nothing may collect garbage while it does.
    pub(crate) fn fill(&mut self, vm: &'vm Vm, count: usize, fill: impl FnOnce(&mut [Value])) {
        debug_assert!(self.count == 0 && self.rooted.is_none(), "the storage is filled once");
        self.count = count;
        if count <= Self::INLINE_CAPACITY {
            fill(&mut self.inline[..count]);
        } else {
            let rooted = MarkedVec::with_capacity(vm, count);
            rooted.resize(count, Value::UNDEFINED);
            fill(&mut rooted.values_mut());
            self.rooted = Some(rooted);
        }
    }

    pub(crate) fn values(&self) -> ArgumentValues<'_> {
        match &self.rooted {
            Some(rooted) => ArgumentValues::Rooted(rooted.values()),
            None => ArgumentValues::Inline(&self.inline[..self.count]),
        }
    }
}

impl Drop for ArgumentStorage<'_> {
    /// Clears the values on the stack once the call is done with them. The garbage collector scans the stack
    /// conservatively, and frames that do not initialize all of their slots come to lie over this one later: a value
    /// left behind would keep its cell alive for as long as such a frame is there.
    fn drop(&mut self) {
        if self.rooted.is_some() {
            return;
        }
        for slot in &mut self.inline[..self.count] {
            // NB: The writes are volatile so that they are not taken for dead stores.
            // SAFETY: The slot is a valid, aligned value of this storage.
            unsafe { core::ptr::write_volatile(slot, Value::UNDEFINED) };
        }
    }
}

/// The values of an ArgumentStorage, which it lends.
pub(crate) enum ArgumentValues<'a> {
    Inline(&'a [Value]),
    Rooted(MarkedValues<'a, Value>),
}

impl Deref for ArgumentValues<'_> {
    type Target = [Value];

    fn deref(&self) -> &[Value] {
        match self {
            Self::Inline(values) => values,
            Self::Rooted(values) => values,
        }
    }
}
