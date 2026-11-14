/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Calls from JIT code: the call instructions it leaves to the runtime, and the runtime functions its direct calls,
//! the call stub and its call intrinsics use.

use core::ptr::NonNull;

use libjs_abi::register;
use libjs_jit::ir::Float64Function;
use libjs_runtime_macros::{jit_abi, jit_function_address};

use super::code::EntryStatus;
use super::entry_exit::{can_enter_jit_code, enter_jit_code};
use super::{frame_of, frames_above};
use crate::bytecode::encoding::Operand;
use crate::bytecode::executable::Executable;
use crate::bytecode::instruction::OpCode;
use crate::bytecode::instruction_at;
use crate::bytecode::op;
use crate::gc::root::MarkedVec;
use crate::interpreter::runtime_functions::{Runtime, RuntimeFunctions, SlowPathControl, handle_asm_exception};
use crate::interpreter::slow_paths;
use crate::interpreter::slow_paths::calls::{
    ArgumentStorage, CallType, execute_asm_call, record_callback, throw_error, throw_if_needed_for_asm_call,
};
use crate::interpreter::slow_paths::feedback::record_forwarded_call;
use crate::interpreter::slow_paths::property_access::{CachedAccessors, try_get_by_id_cache_with};
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::execution_context::ExecutionContext;
use crate::layout::feedback::CallFeedbackForwarding;
use crate::layout::function_object::{EcmascriptFunctionObject, FunctionObject, NativeFunction, RawNativeFunction};
use crate::layout::object::{IndexedStorageKind, Object};
use crate::layout::value::Value;
use crate::runtime::abstract_operations::{
    call_function_object, create_unmapped_arguments_object, function_object_as_object, length_of_array_like,
    throw_if_too_many_arguments,
};
use crate::runtime::array::Array;
use crate::runtime::bound_function::BoundFunction;
use crate::runtime::completion::Must;
use crate::runtime::ecmascript_function_object::{as_ecmascript_function_object, value_as_ecmascript_function_object};
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;
use crate::runtime::function_prototype::FunctionPrototype;
use crate::runtime::iterator::get_iterator_values;
use crate::runtime::math_object;
use crate::runtime::native_javascript_backed_function::NativeJavaScriptBackedFunction;
use crate::runtime::primitive_string::PrimitiveString;
use crate::runtime::property_attributes::DEFAULT_ATTRIBUTES;
use crate::runtime::property_key::PropertyKey;
use crate::runtime::shared_function_instance_data::{ConstructorKind, FunctionKind, ThisMode};
use crate::runtime::value_conversions;

/// Calls through Function.prototype.call/apply and bound functions are unwrapped this many times before the generic
/// call machinery takes over.
const MAX_JIT_CALL_UNWRAP_DEPTH: u32 = 4;

/// Where the result of a call from JIT code goes once the callee returns.
struct JitCallSite {
    frame: NonNull<ExecutionContext>,
    pc: u32,
    return_pc: u32,
    dst: u32,
}

impl JitCallSite {
    fn store_result(&self, value: Value) -> SlowPathControl {
        frame_of(self.frame).slots()[self.dst as usize].set(value);
        SlowPathControl::continue_at(self.return_pc)
    }
}

fn throw_call_stack_size_exceeded(vm: &Vm, pc: u32) -> SlowPathControl {
    throw_error(vm, pc, ErrorKind::InternalError, ErrorType::CallStackSizeExceeded, &[])
}

/// Finishes a call from JIT code after the interpreter (or the callee's JIT code) is done with the callee frame: either
/// the callee returned into the call site's frame, or an exception stopped at the callee frame.
fn finish_call_from_jit(vm: &Vm, frame: NonNull<ExecutionContext>, pc: u32, return_pc: u32) -> SlowPathControl {
    let running = vm.running_execution_context().expect("a frame is running");
    if running == frame {
        return SlowPathControl::continue_at(return_pc);
    }
    let running_ref = frame_of(running);
    assert!(running_ref.returns_to_native_caller.get());
    assert_eq!(running_ref.caller_frame.get(), frame.as_ptr());
    let exception = running_ref.register(register::EXCEPTION).get();
    assert!(exception != Value::EMPTY);
    vm.unwind_inline_frame_for_exception();
    handle_asm_exception(vm, pc, exception)
}

/// Runs the frame pushed for a call from JIT code, the running execution context, to completion in its JIT code or
/// the interpreter, and finishes the call.
fn run_callee_frame_from_jit(vm: &Vm, site: &JitCallSite, callee_frame: NonNull<ExecutionContext>) -> SlowPathControl {
    frame_of(callee_frame).returns_to_native_caller.set(true);
    let executable = Executable::from_head(
        frame_of(callee_frame)
            .executable
            .get()
            .expect("the callee frame runs an executable"),
    );
    if can_enter_jit_code(vm, &executable) {
        let result = enter_jit_code(vm, callee_frame);
        match result.status {
            EntryStatus::Returned => {
                vm.return_from_running_frame(Value::from_bits(result.value));
                return finish_call_from_jit(vm, site.frame, site.pc, site.return_pc);
            }
            EntryStatus::ExitInterpreter => return finish_call_from_jit(vm, site.frame, site.pc, site.return_pc),
            EntryStatus::Resume => {}
        }
    } else {
        super::tier_up::charge_call_from_compiled_code(vm, &executable);
    }
    vm.run_running_frame_in_interpreter();
    finish_call_from_jit(vm, site.frame, site.pc, site.return_pc)
}

/// Calls an ECMAScript function the way the interpreter's call fast path does, in an inline frame linked to the call
/// site's frame, and runs it to completion in its JIT code or the interpreter.
fn call_ecmascript_function_from_jit(
    vm: &Vm,
    site: &JitCallSite,
    function: Gc<EcmascriptFunctionObject>,
    this_value: Value,
    arguments: &[Value],
) -> SlowPathControl {
    if vm.did_reach_stack_space_limit() {
        return throw_call_stack_size_exceeded(vm, site.pc);
    }
    let executable = function.inline_call_executable();
    let argument_count = u32::try_from(arguments.len()).expect("the argument count fits in u32");
    let Some(callee_frame) = vm.push_inline_frame_without_this(
        function,
        executable,
        arguments,
        argument_count,
        site.return_pc,
        site.dst,
        None,
        false,
    ) else {
        return throw_call_stack_size_exceeded(vm, site.pc);
    };
    vm.bind_this_in_inline_frame(function, frame_of(callee_frame), this_value);
    run_callee_frame_from_jit(vm, site, callee_frame)
}

/// Calls a builtin written in JavaScript the way the interpreter's call slow path does, in an inline frame linked to
/// the call site's frame, and runs it to completion in its JIT code or the interpreter. Returns None for builtins the
/// interpreter cannot call inline.
fn call_builtin_from_jit(
    vm: &Vm,
    site: &JitCallSite,
    builtin: Gc<NativeJavaScriptBackedFunction>,
    this_value: Value,
    arguments: &[Value],
) -> Option<SlowPathControl> {
    let executable = builtin.inline_call_executable(vm)?;
    record_callback(vm, frame_of(site.frame), site.pc, builtin, arguments);
    if vm.did_reach_stack_space_limit() {
        return Some(throw_call_stack_size_exceeded(vm, site.pc));
    }
    let argument_count = u32::try_from(arguments.len()).expect("the argument count fits in u32");
    let Some(callee_frame) = vm.push_builtin_inline_frame(
        builtin,
        executable,
        arguments,
        argument_count,
        site.return_pc,
        site.dst,
        this_value,
    ) else {
        return Some(throw_call_stack_size_exceeded(vm, site.pc));
    };
    Some(run_callee_frame_from_jit(vm, site, callee_frame))
}

/// Whether constructs from JIT code with `function` as the constructor run its bytecode in an inline frame.
fn constructs_in_inline_frame(function: Gc<EcmascriptFunctionObject>) -> bool {
    function.kind() == FunctionKind::Normal && function.has_constructor() && function.bytecode_executable().is_some()
}

/// The frame of a construct from JIT code, pushed by push_construct_frame() and running next.
struct PushedConstruct {
    callee_frame: NonNull<ExecutionContext>,
    /// The this value of a base constructor.
    this_argument: Option<Gc<Object>>,
    /// Whether the this value of a derived constructor is in its frame (see
    /// ExecutionContext::caller_is_derived_construct).
    this_is_in_frame: bool,
    constructor_environment: Gc<crate::layout::environment::Environment>,
}

// 10.2.2 [[Construct]] ( argumentsList, newTarget ), https://tc39.es/ecma262/#sec-ecmascript-function-objects-construct-argumentslist-newtarget
/// Steps 2 to 7 of a construct with an ECMAScript function from JIT code: pushes its frame, an inline frame linked to
/// the call site's frame like a call, which runs next.
fn push_construct_frame(
    vm: &Vm,
    site: &JitCallSite,
    function: Gc<EcmascriptFunctionObject>,
    new_target: Gc<FunctionObject>,
    arguments: &[Value],
) -> Result<PushedConstruct, SlowPathControl> {
    if vm.did_reach_stack_space_limit() {
        return Err(throw_call_stack_size_exceeded(vm, site.pc));
    }

    // 2. Let kind be F.[[ConstructorKind]].
    let kind = function.constructor_kind();

    // 3. If kind is base, then
    let mut this_argument = None;
    if kind == ConstructorKind::Base {
        // a. Let thisArgument be ? OrdinaryCreateFromConstructor(newTarget, "%Object.prototype%").
        // NB: With room for the properties the constructor is known to add.
        match Object::create_from_constructor_for_construct(vm, new_target, function.shared_data()) {
            Ok(object) => this_argument = Some(object),
            Err(throw) => return Err(handle_asm_exception(vm, site.pc, throw.value())),
        }
    }

    // 4. Let calleeContext be PrepareForOrdinaryCall(F, newTarget).
    // NB: The frame of a base constructor does not use the construct return rule of Return; steps 10 to 17 are in
    //     finish_construct().
    // NB: The prototype lookup above may have run code and collected garbage, which may have flushed the
    //     constructor's code (see `bytecode::flushing`).
    let executable = match function.compiled_executable(vm) {
        Ok(executable) => executable,
        Err(throw) => return Err(handle_asm_exception(vm, site.pc, throw.value())),
    };
    let argument_count = u32::try_from(arguments.len()).expect("the argument count fits in u32");
    let Some(callee_frame) = vm.push_inline_frame_without_this(
        function,
        executable,
        arguments,
        argument_count,
        site.return_pc,
        site.dst,
        Some(function_object_as_object(new_target)),
        false,
    ) else {
        return Err(throw_call_stack_size_exceeded(vm, site.pc));
    };
    let callee_frame_ref = frame_of(callee_frame);
    // NB: The frame of a derived constructor without an environment for `this` has the this value, which step 15
    //     needs after the frame is gone. Returning undefined from the frame returns it instead (see
    //     ExecutionContext::caller_is_derived_construct).
    let this_is_in_frame =
        kind == ConstructorKind::Derived && !function.shared_data().function_environment_has_this_binding();
    callee_frame_ref.caller_is_construct.set(this_is_in_frame);
    callee_frame_ref.caller_is_derived_construct.set(this_is_in_frame);

    // 6. If kind is base, then
    if let Some(this_argument) = this_argument {
        // a. Perform OrdinaryCallBindThis(F, calleeContext, thisArgument).
        vm.bind_this_in_inline_frame(function, callee_frame_ref, Value::from_object(this_argument));

        // b. Let initializeResult be Completion(InitializeInstanceElements(thisArgument, F)).
        let initialize_result = this_argument.initialize_instance_elements(vm, function);

        // c. If initializeResult is an abrupt completion, then
        if let Err(throw) = initialize_result {
            // i. Remove calleeContext from the execution context stack and restore callerContext as the running execution context.
            vm.unwind_inline_frame_for_exception();

            // ii. Return ? initializeResult.
            return Err(handle_asm_exception(vm, site.pc, throw.value()));
        }
    }

    // 7. Let constructorEnv be the LexicalEnvironment of calleeContext.
    let constructor_environment = callee_frame_ref
        .lexical_environment
        .get()
        .expect("a constructor runs in an environment");

    Ok(PushedConstruct {
        callee_frame,
        this_argument,
        this_is_in_frame,
        constructor_environment,
    })
}

/// Steps 11 to 17 of a construct from JIT code (see push_construct_frame()), once its frame returned the value now in
/// the call site's destination slot.
fn finish_construct(
    vm: &Vm,
    site: &JitCallSite,
    this_argument: Option<Gc<Object>>,
    this_is_in_frame: bool,
    constructor_environment: Option<Gc<crate::layout::environment::Environment>>,
    control: SlowPathControl,
) -> SlowPathControl {
    // 11. Assert: result is a return completion.
    let result_slot = &frame_of(site.frame).slots()[site.dst as usize];
    let result = result_slot.get();

    // 12. If Type(result.[[Value]]) is Object, return result.[[Value]].
    if result.is_object() {
        return control;
    }

    // 13. If kind is base, return thisArgument.
    if let Some(this_argument) = this_argument {
        result_slot.set(Value::from_object(this_argument));
        return control;
    }

    // OPTIMIZATION: The frame returned its this value for undefined, which is empty if it is not initialized.
    if this_is_in_frame && result.is_empty() {
        result_slot.set(Value::UNDEFINED);
        return throw_error(
            vm,
            site.pc,
            ErrorKind::ReferenceError,
            ErrorType::ThisHasNotBeenInitialized,
            &[],
        );
    }

    // 14. If result.[[Value]] is not undefined, throw a TypeError exception.
    if !result.is_undefined() {
        return throw_error(
            vm,
            site.pc,
            ErrorKind::TypeError,
            ErrorType::DerivedConstructorReturningInvalidValue,
            &[],
        );
    }

    // 15. Let thisBinding be ? constructorEnv.GetThisBinding().
    let constructor_environment =
        constructor_environment.expect("a frame without the this value has an environment for it");
    let this_binding = match constructor_environment.get_this_binding(vm) {
        Ok(this_binding) => this_binding,
        Err(throw) => return handle_asm_exception(vm, site.pc, throw.value()),
    };

    // 16. Assert: Type(thisBinding) is Object.
    debug_assert!(this_binding.is_object());

    // 17. Return thisBinding.
    result_slot.set(this_binding);
    control
}

// 10.2.2 [[Construct]] ( argumentsList, newTarget ), https://tc39.es/ecma262/#sec-ecmascript-function-objects-construct-argumentslist-newtarget
/// Constructs with an ECMAScript function as the new target, in an inline frame linked to the call site's frame like a
/// call.
fn construct_ecmascript_function_from_jit(
    vm: &Vm,
    site: &JitCallSite,
    function: Gc<EcmascriptFunctionObject>,
    new_target: Gc<FunctionObject>,
    arguments: &[Value],
) -> SlowPathControl {
    let pushed = match push_construct_frame(vm, site, function, new_target, arguments) {
        Ok(pushed) => pushed,
        Err(control) => return control,
    };

    // 8. Let result be Completion(OrdinaryCallEvaluateBody(F, argumentsList)).
    // 9. Remove calleeContext from the execution context stack and restore callerContext as the running execution context.
    // 10. If result is a throw completion, then
    //     a. Return ? result.
    let control = run_callee_frame_from_jit(vm, site, pushed.callee_frame);
    if !control.continues_in_frame() {
        return control;
    }
    finish_construct(
        vm,
        site,
        pushed.this_argument,
        pushed.this_is_in_frame,
        Some(pushed.constructor_environment),
        control,
    )
}

/// Calls `callee` from JIT code. Calls of ECMAScript functions avoid the generic call machinery, and calls through
/// bound functions and Function.prototype.call/apply call their target directly. Returns nothing if the caller should
/// make the call through the generic call machinery instead, which only happens before anything observable was done.
fn call_from_jit(
    vm: &Vm,
    site: &JitCallSite,
    callee: Value,
    this_value: Value,
    arguments: &[Value],
    unwrap_depth: u32,
) -> Option<SlowPathControl> {
    if !callee.is_object() {
        return None;
    }
    if let Some(function) = value_as_ecmascript_function_object(callee) {
        if function.can_inline_call() {
            return Some(call_ecmascript_function_from_jit(
                vm, site, function, this_value, arguments,
            ));
        }
        return None;
    }
    if unwrap_depth >= MAX_JIT_CALL_UNWRAP_DEPTH {
        return None;
    }
    call_other_function_from_jit(vm, site, callee, this_value, arguments, unwrap_depth)
}

/// The part of call_from_jit() for callees that are not ECMAScript functions.
#[inline(never)]
fn call_other_function_from_jit(
    vm: &Vm,
    site: &JitCallSite,
    callee: Value,
    this_value: Value,
    arguments: &[Value],
    unwrap_depth: u32,
) -> Option<SlowPathControl> {
    let callee_object = callee.as_object();
    if let Some(builtin) = callee_object.downcast::<NativeJavaScriptBackedFunction>() {
        return call_builtin_from_jit(vm, site, builtin, this_value, arguments);
    }
    if let Some(function) = callee_object.downcast::<RawNativeFunction>() {
        if FunctionPrototype::is_call_function(vm, &function) {
            // 20.2.3.3 Function.prototype.call ( thisArg, ...args ), https://tc39.es/ecma262/#sec-function.prototype.call
            if !this_value.is_function() {
                return None;
            }
            if unwrap_depth == 0 {
                record_forwarded_call(
                    vm,
                    frame_of(site.frame),
                    site.pc,
                    callee.as_cell(),
                    this_value.as_cell(),
                    CallFeedbackForwarding::Call,
                    arguments.len().saturating_sub(1),
                );
            }
            let target_this = arguments.first().copied().unwrap_or(Value::UNDEFINED);
            let rest = arguments.get(1..).unwrap_or(&[]);
            return call_from_jit(vm, site, this_value, target_this, rest, unwrap_depth + 1);
        }

        if FunctionPrototype::is_apply_function(vm, &function) {
            // 20.2.3.1 Function.prototype.apply ( thisArg, argArray ), https://tc39.es/ecma262/#sec-function.prototype.apply
            if !this_value.is_function() {
                return None;
            }
            let target_this = arguments.first().copied().unwrap_or(Value::UNDEFINED);
            let argument_array = arguments.get(1).copied().unwrap_or(Value::UNDEFINED);
            if argument_array.is_nullish() {
                return call_from_jit(vm, site, this_value, target_this, &[], unwrap_depth + 1);
            }
            if !argument_array.is_object() {
                return None;
            }
            let array_like = argument_array.as_object();

            // NB: From here on, reading the argument list may run JS code, so the call cannot fall back to the
            //     generic slow path anymore.
            let length = match length_of_array_like(vm, &array_like) {
                Ok(length) => length,
                Err(throw) => return Some(handle_asm_exception(vm, site.pc, throw.value())),
            };
            if let Err(throw) = throw_if_too_many_arguments(vm, length) {
                return Some(handle_asm_exception(vm, site.pc, throw.value()));
            }
            if unwrap_depth == 0 {
                record_forwarded_call(
                    vm,
                    frame_of(site.frame),
                    site.pc,
                    callee.as_cell(),
                    this_value.as_cell(),
                    CallFeedbackForwarding::Apply,
                    usize::try_from(length).unwrap_or(usize::MAX),
                );
            }
            if !array_like.may_interfere_with_indexed_property_access()
                && array_like.indexed_storage_kind() == IndexedStorageKind::Packed
                && u64::from(array_like.indexed_packed_elements_span_size()) >= length
            {
                let mut arguments = ArgumentStorage::empty();
                arguments.fill(vm, length as usize, |slots| {
                    array_like.copy_indexed_packed_elements(slots);
                });
                return Some(call_from_jit_or_generic(
                    vm,
                    site,
                    this_value,
                    target_this,
                    &arguments.values(),
                    unwrap_depth + 1,
                ));
            }
            let argument_list = MarkedVec::with_capacity(vm, usize::try_from(length).unwrap_or(0).min(1 << 16));
            for index in 0..length {
                match array_like.get(vm, &PropertyKey::from_number(index)) {
                    Ok(element) => argument_list.push(element),
                    Err(throw) => return Some(handle_asm_exception(vm, site.pc, throw.value())),
                }
            }
            return Some(call_from_jit_or_generic(
                vm,
                site,
                this_value,
                target_this,
                &argument_list.values(),
                unwrap_depth + 1,
            ));
        }
    }

    if let Some(bound_function) = callee_object.downcast::<BoundFunction>() {
        // 10.4.1.1 [[Call]] ( thisArgument, argumentsList ), https://tc39.es/ecma262/#sec-bound-function-exotic-objects-call-thisargument-argumentslist
        let target = Value::from_object(bound_function.bound_target_function());
        let bound_argument_count = bound_function.bound_arguments_count();
        if unwrap_depth == 0 {
            record_forwarded_call(
                vm,
                frame_of(site.frame),
                site.pc,
                callee.as_cell(),
                target.as_cell(),
                CallFeedbackForwarding::Bound,
                bound_argument_count + arguments.len(),
            );
        }
        if bound_argument_count == 0 {
            return call_from_jit(
                vm,
                site,
                target,
                bound_function.bound_this(),
                arguments,
                unwrap_depth + 1,
            );
        }
        let mut argument_list = ArgumentStorage::empty();
        argument_list.fill(vm, bound_argument_count + arguments.len(), |slots| {
            let (bound_slots, argument_slots) = slots.split_at_mut(bound_argument_count);
            for (index, slot) in bound_slots.iter_mut().enumerate() {
                *slot = bound_function.bound_argument(index);
            }
            argument_slots.copy_from_slice(arguments);
        });
        return call_from_jit(
            vm,
            site,
            target,
            bound_function.bound_this(),
            &argument_list.values(),
            unwrap_depth + 1,
        );
    }

    if let Some(function) = callee_object.downcast::<NativeFunction>() {
        return Some(
            match call_function_object(vm, function.upcast(), this_value, arguments) {
                Ok(value) => site.store_result(value),
                Err(throw) => handle_asm_exception(vm, site.pc, throw.value()),
            },
        );
    }

    None
}

/// Like call_from_jit(), but makes the call through the generic call machinery if call_from_jit() does not.
fn call_from_jit_or_generic(
    vm: &Vm,
    site: &JitCallSite,
    callee: Value,
    this_value: Value,
    arguments: &[Value],
    unwrap_depth: u32,
) -> SlowPathControl {
    if let Some(control) = call_from_jit(vm, site, callee, this_value, arguments, unwrap_depth) {
        return control;
    }
    let mut result = Value::UNDEFINED;
    if let Err(throw) = execute_asm_call(
        CallType::Call,
        vm,
        callee,
        this_value,
        arguments,
        &mut result,
        crate::bytecode::property_access::Strict::No,
    ) {
        return handle_asm_exception(vm, site.pc, throw.value());
    }
    site.store_result(result)
}

/// Runs the generic slow path of a call instruction, and stores its result in the frame if the call returned normally.
/// A callee frame the slow path pushed for the interpreter to run runs to completion here, since the frame's JIT code
/// waits for the call.
fn finish_generic_call(vm: &Vm, site: &JitCallSite, control: SlowPathControl, result: Value) -> SlowPathControl {
    if control.continues_in_frame() {
        frame_of(site.frame).slots()[site.dst as usize].set(result);
        return control;
    }
    let Some(running) = vm.running_execution_context() else {
        return control;
    };
    if control.0 < 0 || running == site.frame || frame_of(running).caller_frame.get() != site.frame.as_ptr() {
        // NB: An exception went to a handler in this frame, or unwound it.
        return control;
    }
    run_callee_frame_from_jit(vm, site, running)
}

/// `i64 libjs_jit_call(VM*, ExecutionContext*, u32 pc)`: runs the call instruction at `pc` of the frame, the running
/// execution context, to completion, writes its result into its destination slot, and returns a slow path control
/// word.
///
/// # Safety
///
/// JIT code calls this with its VM, its frame and the pc of one of its executable's call instructions.
#[jit_abi]
pub unsafe extern "C" fn libjs_jit_call(vm: *const Vm, frame: *mut ExecutionContext, pc: u32) -> i64 {
    // SAFETY: JIT code passes live pointers.
    let vm = unsafe { &*vm };
    let frame = NonNull::new(frame).expect("JIT code passes its frame");
    assert_eq!(vm.running_execution_context(), Some(frame));
    let frame_ref = frame_of(frame);
    frame_ref.program_counter.set(pc);
    let executable = Executable::from_head(frame_ref.executable.get().expect("the frame runs an executable"));
    let slots = frame_ref.slots();
    let load = |operand: Operand| slots[operand.0 as usize].get();
    let opcode = executable.bytecode()[pc as usize];

    macro_rules! instruction {
        ($op:ident) => {
            instruction_at::<op::$op>(&executable, pc)
        };
    }
    let site = |dst: Operand, length: u32| JitCallSite {
        frame,
        pc,
        return_pc: pc + length,
        dst: dst.0.into(),
    };

    let control = match opcode {
        _ if opcode == OpCode::Call as u8 => {
            let call = instruction!(Call);
            let site = site(call.dst, call.length());
            let operands = call.arguments();
            let mut storage = ArgumentStorage::empty();
            storage.fill_from(vm, operands.iter().map(|operand| load(*operand)));
            let arguments = storage.values();
            let callee = load(call.callee);
            let this_value = load(call.this_value);
            if let Some(control) = call_from_jit(vm, &site, callee, this_value, &arguments, 0) {
                return control.0;
            }
            let mut values = op::CallValues {
                dst: Value::EMPTY,
                callee,
                this_value,
                arguments: [],
            };
            let control = slow_paths::call_slow_path(vm, pc, call, &mut values, &arguments);
            finish_generic_call(vm, &site, control, values.dst)
        }
        _ if opcode == OpCode::CallConstruct as u8 => {
            let call = instruction!(CallConstruct);
            let site = site(call.dst, call.length());
            let operands = call.arguments();
            let mut storage = ArgumentStorage::empty();
            storage.fill_from(vm, operands.iter().map(|operand| load(*operand)));
            let arguments = storage.values();
            let callee = load(call.callee);
            if let Some(function) = value_as_ecmascript_function_object(callee)
                && constructs_in_inline_frame(function)
            {
                return construct_ecmascript_function_from_jit(
                    vm,
                    &site,
                    function,
                    function.as_function_object_gc(),
                    &arguments,
                )
                .0;
            }
            let mut values = op::CallConstructValues {
                dst: Value::EMPTY,
                callee,
                arguments: [],
            };
            let control = slow_paths::call_construct_slow_path(vm, pc, call, &mut values, &arguments);
            finish_generic_call(vm, &site, control, values.dst)
        }
        _ if opcode == OpCode::SuperCall as u8 => {
            let call = instruction!(SuperCall);
            let site = site(call.dst, call.length());
            let operands = call.arguments();
            let mut storage = ArgumentStorage::empty();
            storage.fill_from(vm, operands.iter().map(|operand| load(*operand)));
            let arguments = storage.values();
            let super_constructor = load(call.super_constructor);
            if let Some(control) = super_call_from_jit(vm, &site, call.call_feedback, super_constructor, &arguments) {
                return control.0;
            }
            let mut values = op::SuperCallValues {
                dst: Value::EMPTY,
                super_constructor,
                arguments: [],
            };
            let control = slow_paths::super_call_slow_path(vm, pc, call, &mut values, &arguments);
            finish_generic_call(vm, &site, control, values.dst)
        }
        _ if opcode == OpCode::SuperCallWithForwardedArguments as u8 => {
            let call = instruction!(SuperCallWithForwardedArguments);
            let site = site(call.dst, op::SuperCallWithForwardedArguments::LENGTH);
            let mut storage = ArgumentStorage::empty();
            storage.fill_from_passed_arguments(vm, frame_ref);
            let arguments = storage.values();
            let super_constructor = load(call.super_constructor);
            if let Some(control) = super_call_from_jit(vm, &site, call.call_feedback, super_constructor, &arguments) {
                return control.0;
            }
            let mut values = op::SuperCallWithForwardedArgumentsValues {
                dst: Value::EMPTY,
                super_constructor,
            };
            let control = Runtime::super_call_with_forwarded_arguments(vm, pc, call, &mut values);
            finish_generic_call(vm, &site, control, values.dst)
        }
        _ if opcode == OpCode::CallDirectEval as u8 => {
            let call = instruction!(CallDirectEval);
            let site = site(call.dst, call.length());
            let operands = call.arguments();
            let mut storage = ArgumentStorage::empty();
            storage.fill_from(vm, operands.iter().map(|operand| load(*operand)));
            let arguments = storage.values();
            let mut values = op::CallDirectEvalValues {
                dst: Value::EMPTY,
                callee: load(call.callee),
                this_value: load(call.this_value),
                arguments: [],
            };
            let control = slow_paths::call_direct_eval_slow_path(vm, pc, call, &mut values, &arguments);
            finish_generic_call(vm, &site, control, values.dst)
        }
        _ if opcode == OpCode::CallWithArgumentArray as u8 => {
            let call = instruction!(CallWithArgumentArray);
            let site = site(call.dst, op::CallWithArgumentArray::LENGTH);
            let mut values = op::CallWithArgumentArrayValues {
                dst: Value::EMPTY,
                callee: load(call.callee),
                this_value: load(call.this_value),
                arguments: load(call.arguments),
            };
            let control = Runtime::call_with_argument_array(vm, pc, call, &mut values);
            finish_generic_call(vm, &site, control, values.dst)
        }
        _ if opcode == OpCode::CallDirectEvalWithArgumentArray as u8 => {
            let call = instruction!(CallDirectEvalWithArgumentArray);
            let site = site(call.dst, op::CallDirectEvalWithArgumentArray::LENGTH);
            let mut values = op::CallDirectEvalWithArgumentArrayValues {
                dst: Value::EMPTY,
                callee: load(call.callee),
                this_value: load(call.this_value),
                arguments: load(call.arguments),
            };
            let control = Runtime::call_direct_eval_with_argument_array(vm, pc, call, &mut values);
            finish_generic_call(vm, &site, control, values.dst)
        }
        _ if opcode == OpCode::CallConstructWithArgumentArray as u8 => {
            let call = instruction!(CallConstructWithArgumentArray);
            let site = site(call.dst, op::CallConstructWithArgumentArray::LENGTH);
            let mut values = op::CallConstructWithArgumentArrayValues {
                dst: Value::EMPTY,
                callee: load(call.callee),
                this_value: load(call.this_value),
                arguments: load(call.arguments),
            };
            let control = Runtime::call_construct_with_argument_array(vm, pc, call, &mut values);
            finish_generic_call(vm, &site, control, values.dst)
        }
        _ if opcode == OpCode::SuperCallWithArgumentArray as u8 => {
            let call = instruction!(SuperCallWithArgumentArray);
            let site = site(call.dst, op::SuperCallWithArgumentArray::LENGTH);
            let mut values = op::SuperCallWithArgumentArrayValues {
                dst: Value::EMPTY,
                super_constructor: load(call.super_constructor),
                arguments: load(call.arguments),
            };
            let control = Runtime::super_call_with_argument_array(vm, pc, call, &mut values);
            finish_generic_call(vm, &site, control, values.dst)
        }
        _ => unreachable!("libjs_jit_call runs call instructions only"),
    };
    control.0
}

/// Steps 1, 2, 5 and 6 of the evaluation of `super(...)` from JIT code, for a super constructor that is an ECMAScript
/// function: it constructs in an inline frame linked to the call site's frame, like a construct. BindThisFromSuperCall
/// does steps 7 and later. Returns nothing for other super constructors.
fn super_call_from_jit(
    vm: &Vm,
    site: &JitCallSite,
    call_feedback: u16,
    super_constructor: Value,
    arguments: &[Value],
) -> Option<SlowPathControl> {
    if let Some(function) = value_as_ecmascript_function_object(super_constructor)
        && constructs_in_inline_frame(function)
    {
        let new_target = vm.get_new_target();
        assert!(new_target.is_object());
        slow_paths::feedback::record_call(
            vm,
            call_feedback,
            super_constructor,
            crate::bytecode::feedback::call_feedback_flags::SAW_CONSTRUCT,
        );
        return Some(construct_ecmascript_function_from_jit(
            vm,
            site,
            function,
            new_target.as_function(),
            arguments,
        ));
    }
    None
}

/// `g(...arguments)`, built as NewArray, ArrayAppend of the arguments object the code never created and
/// CallWithArgumentArray at pc.
fn call_spreading_arguments_from_jit(
    vm: &Vm,
    frame: NonNull<ExecutionContext>,
    pc: u32,
    call: &op::CallWithArgumentArray,
) -> SlowPathControl {
    let frame_ref = frame_of(frame);
    let slots = frame_ref.slots();
    let site = JitCallSite {
        frame,
        pc,
        return_pc: pc + op::CallWithArgumentArray::LENGTH,
        dst: call.dst.0.into(),
    };
    // NB: Iterate an arguments object (whose iterator is the original one, since nothing saw it) like the spread does,
    //     which is observable if %ArrayIteratorPrototype%.next is not the original.
    let passed_arguments = &frame_ref.arguments()[..frame_ref.passed_argument_count.get() as usize];
    let arguments_object = create_unmapped_arguments_object(vm, passed_arguments);
    let spread_arguments = MarkedVec::with_capacity(vm, passed_arguments.len());
    let completion = get_iterator_values(vm, Value::from_object(arguments_object), |value| {
        spread_arguments.push(value);
        None
    });
    if completion.is_error() {
        return handle_asm_exception(vm, pc, completion.value());
    }
    let callee = slots[call.callee.0 as usize].get();
    if let Err(throw) = throw_if_needed_for_asm_call(vm, callee, CallType::Call) {
        return handle_asm_exception(vm, pc, throw.value());
    }
    let this_value = slots[call.this_value.0 as usize].get();
    call_from_jit_or_generic(vm, &site, callee, this_value, &spread_arguments.values(), 0)
}

/// `i64 libjs_jit_call_forwarding_arguments(VM*, ExecutionContext* frame, u32 pc)`: runs the Call instruction at `pc`
/// of the frame, `f.apply(this_arg, arguments)` whose callee is Function.prototype.apply and whose last argument is the
/// frame's arguments object, which the compiled code never created: calls `f` with `this_arg` and the frame's passed
/// arguments. Or runs `g(...arguments)` (see call_spreading_arguments_from_jit()). Returns a slow path control word
/// like libjs_jit_call().
///
/// # Safety
///
/// JIT code calls this with its VM, its frame and the pc of such an instruction.
#[jit_abi]
pub unsafe extern "C" fn libjs_jit_call_forwarding_arguments(
    vm: *const Vm,
    frame: *mut ExecutionContext,
    pc: u32,
) -> i64 {
    // SAFETY: JIT code passes live pointers.
    let vm = unsafe { &*vm };
    let frame = NonNull::new(frame).expect("JIT code passes its frame");
    assert_eq!(vm.running_execution_context(), Some(frame));
    let frame_ref = frame_of(frame);
    frame_ref.program_counter.set(pc);
    let executable = Executable::from_head(frame_ref.executable.get().expect("the frame runs an executable"));
    if executable.bytecode()[pc as usize] == OpCode::CallWithArgumentArray as u8 {
        let call = instruction_at::<op::CallWithArgumentArray>(&executable, pc);
        return call_spreading_arguments_from_jit(vm, frame, pc, call).0;
    }
    let call = instruction_at::<op::Call>(&executable, pc);
    let argument_count = call.argument_count;
    assert_eq!(argument_count, 2);
    let slots = frame_ref.slots();
    let site = JitCallSite {
        frame,
        pc,
        return_pc: pc + call.length(),
        dst: call.dst.0.into(),
    };

    // 20.2.3.1 Function.prototype.apply ( thisArg, argArray ), https://tc39.es/ecma262/#sec-function.prototype.apply
    // 1. Let func be the this value.
    let function = slots[call.this_value.0 as usize].get();
    // 2. If IsCallable(func) is false, throw a TypeError exception.
    if !function.is_function() {
        return throw_error(vm, pc, ErrorKind::TypeError, ErrorType::NotAFunction, &[&function]).0;
    }
    // NB: The argument list of the arguments object is the frame's passed arguments, which nothing changed.
    let operands = call.arguments();
    let this_argument = slots[operands[0].0 as usize].get();
    let passed_argument_count = frame_ref.passed_argument_count.get() as usize;
    let callee = slots[call.callee.0 as usize].get();
    if callee.is_cell() {
        record_forwarded_call(
            vm,
            frame_ref,
            pc,
            callee.as_cell(),
            function.as_cell(),
            CallFeedbackForwarding::Apply,
            passed_argument_count,
        );
    }
    let mut arguments = ArgumentStorage::empty();
    arguments.fill_from(
        vm,
        frame_ref.arguments()[..passed_argument_count]
            .iter()
            .map(core::cell::Cell::get),
    );
    call_from_jit_or_generic(vm, &site, function, this_argument, &arguments.values(), 1).0
}

/// `i64 libjs_jit_finish_direct_call(VM*, ExecutionContext* frame, u32 pc, u64 status)`: finishes a direct call made
/// for the Call instruction at `pc` of `frame` that did not return to JIT code: `status` is the `EntryStatus` the
/// callee's JIT code returned (Resume or ExitInterpreter), or Returned if JIT code did not enter the callee's frame,
/// which is the running execution context. Runs the callee's frame to completion in the interpreter as needed, and
/// returns a slow path control word for the call, whose result is in its destination slot.
///
/// # Safety
///
/// JIT code calls this with its VM and frame, and the pc of the Call instruction.
#[jit_abi]
pub unsafe extern "C" fn libjs_jit_finish_direct_call(
    vm: *const Vm,
    frame: *mut ExecutionContext,
    pc: u32,
    status: u64,
) -> i64 {
    // SAFETY: JIT code passes live pointers.
    let vm = unsafe { &*vm };
    let frame = NonNull::new(frame).expect("JIT code passes its frame");
    let frame_ref = frame_of(frame);
    let executable = Executable::from_head(frame_ref.executable.get().expect("the frame runs an executable"));
    let call = instruction_at::<op::Call>(&executable, pc);
    let site = JitCallSite {
        frame,
        pc,
        return_pc: pc + call.length(),
        dst: call.dst.0.into(),
    };
    finish_direct_call_frames(vm, frame, &site, status).0
}

/// Runs the callee frame of a direct call or construct from JIT code that did not return to it to completion (see
/// `libjs_jit_finish_direct_call()`), and finishes the call.
fn finish_direct_call_frames(
    vm: &Vm,
    frame: NonNull<ExecutionContext>,
    site: &JitCallSite,
    status: u64,
) -> SlowPathControl {
    // NB: The callee's frame, and the frames of the calls it inlined that its exit materialized, run in the
    //     interpreter from now on.
    for callee_frame in frames_above(vm, frame).expect("the calling frame is on the running frame's chain") {
        frame_of(callee_frame).runs_jit_code.set(false);
    }
    if status != EntryStatus::ExitInterpreter as u64 {
        let callee_frame = vm.running_execution_context().expect("the callee frame is running");
        if frame_of(callee_frame).program_counter.get() == 0 {
            super::tier_up::charge_call_from_compiled_code(vm, &super::executable_of(frame_of(callee_frame)));
        }
        vm.run_running_frame_in_interpreter();
    }
    finish_call_from_jit(vm, frame, site.pc, site.return_pc)
}

/// The call site of the CallConstruct, SuperCall or SuperCallWithForwardedArguments instruction at `pc` of `frame`.
fn construct_site(frame: NonNull<ExecutionContext>, pc: u32) -> JitCallSite {
    let executable = super::executable_of(frame_of(frame));
    let opcode = executable.bytecode()[pc as usize];
    let (dst, length) = if opcode == OpCode::CallConstruct as u8 {
        let call = instruction_at::<op::CallConstruct>(&executable, pc);
        (call.dst, call.length())
    } else if opcode == OpCode::SuperCall as u8 {
        let call = instruction_at::<op::SuperCall>(&executable, pc);
        (call.dst, call.length())
    } else {
        assert_eq!(opcode, OpCode::SuperCallWithForwardedArguments as u8);
        let call = instruction_at::<op::SuperCallWithForwardedArguments>(&executable, pc);
        (call.dst, op::SuperCallWithForwardedArguments::LENGTH)
    };
    JitCallSite {
        frame,
        pc,
        return_pc: pc + length,
        dst: dst.0.into(),
    }
}

/// What `libjs_jit_prepare_construct()` returns, in two registers.
#[repr(C)]
pub struct PreparedConstruct {
    /// The frame of the construct, or null if the construct is done.
    pub callee_frame: u64,
    /// With a frame, the value of the construct if the frame returns something other than an object: the this value
    /// of a base constructor, or the empty value. Without one, the slow path control word of the construct.
    pub value: u64,
}

/// `PreparedConstruct libjs_jit_prepare_construct(VM*, ExecutionContext* frame, u32 pc)`: starts the CallConstruct,
/// SuperCall or SuperCallWithForwardedArguments instruction at `pc` of the frame, the running execution context. A
/// construct with an ECMAScript function whose this value is not in an environment gets its frame pushed, linked to
/// the frame and running next, which JIT code enters and pops, and whose result it finishes with the value returned
/// here (see `finish_construct()`) or `libjs_jit_construct_result_error()`. Every other construct runs to completion
/// like `libjs_jit_call()`.
///
/// # Safety
///
/// JIT code calls this with its VM, its frame and the pc of such an instruction.
#[jit_abi]
pub unsafe extern "C" fn libjs_jit_prepare_construct(
    vm: *const Vm,
    frame: *mut ExecutionContext,
    pc: u32,
) -> PreparedConstruct {
    // SAFETY: JIT code passes live pointers.
    let vm_ref = unsafe { &*vm };
    let frame_pointer = NonNull::new(frame).expect("JIT code passes its frame");
    assert_eq!(vm_ref.running_execution_context(), Some(frame_pointer));
    let frame_ref = frame_of(frame_pointer);
    frame_ref.program_counter.set(pc);
    let executable = super::executable_of(frame_ref);
    let slots = frame_ref.slots();
    let load = |operand: Operand| slots[operand.0 as usize].get();
    let opcode = executable.bytecode()[pc as usize];
    let site = construct_site(frame_pointer, pc);
    let takes_frame = |function: Gc<EcmascriptFunctionObject>| {
        constructs_in_inline_frame(function)
            && (function.constructor_kind() == ConstructorKind::Base
                || !function.shared_data().function_environment_has_this_binding())
    };

    let mut storage = ArgumentStorage::empty();
    let construct = if opcode == OpCode::CallConstruct as u8 {
        let call = instruction_at::<op::CallConstruct>(&executable, pc);
        match value_as_ecmascript_function_object(load(call.callee)) {
            Some(function) if takes_frame(function) => {
                storage.fill_from(vm_ref, call.arguments().iter().map(|operand| load(*operand)));
                Some((function, function.as_function_object_gc()))
            }
            _ => None,
        }
    } else {
        // NB: The arguments of a SuperCallWithForwardedArguments are the passed arguments of the frame.
        let (super_constructor, call_feedback, argument_operands) = if opcode == OpCode::SuperCall as u8 {
            let call = instruction_at::<op::SuperCall>(&executable, pc);
            (load(call.super_constructor), call.call_feedback, Some(call.arguments()))
        } else {
            let call = instruction_at::<op::SuperCallWithForwardedArguments>(&executable, pc);
            (load(call.super_constructor), call.call_feedback, None)
        };
        match value_as_ecmascript_function_object(super_constructor) {
            Some(function) if takes_frame(function) => {
                match argument_operands {
                    Some(operands) => storage.fill_from(vm_ref, operands.iter().map(|operand| load(*operand))),
                    None => storage.fill_from_passed_arguments(vm_ref, frame_ref),
                }
                let new_target = vm_ref.get_new_target();
                assert!(new_target.is_object());
                slow_paths::feedback::record_call(
                    vm_ref,
                    call_feedback,
                    super_constructor,
                    crate::bytecode::feedback::call_feedback_flags::SAW_CONSTRUCT,
                );
                Some((function, new_target.as_function()))
            }
            _ => None,
        }
    };
    let done = |control: SlowPathControl| PreparedConstruct {
        callee_frame: 0,
        value: control.0 as u64,
    };
    let Some((function, new_target)) = construct else {
        // SAFETY: The caller's pointers are live.
        return done(SlowPathControl(unsafe { libjs_jit_call(vm, frame, pc) }));
    };
    match push_construct_frame(vm_ref, &site, function, new_target, &storage.values()) {
        Ok(pushed) => {
            frame_of(pushed.callee_frame).returns_to_native_caller.set(true);
            PreparedConstruct {
                callee_frame: pushed.callee_frame.as_ptr() as u64,
                value: pushed.this_argument.map_or(Value::EMPTY, Value::from_object).0,
            }
        }
        Err(control) => done(control),
    }
}

/// What `libjs_jit_construct_this()` returns, in two registers.
#[repr(C)]
pub struct ConstructThis {
    pub this_value: u64,
    pub new_target: u64,
}

// 10.2.2 [[Construct]] ( argumentsList, newTarget ), https://tc39.es/ecma262/#sec-ecmascript-function-objects-construct-argumentslist-newtarget
/// `ConstructThis libjs_jit_construct_this(VM*, ECMAScriptFunctionObject*, u64 new_target, u32 pc)`: step 3 of a
/// construct from JIT code with the function as the constructor and `new_target` (or the empty value for the
/// new.target of the running execution context, JIT code's frame), whose frame the construct stub builds like the
/// frame of a call: the this value (the empty value for a derived constructor) and the new target. Returns 0 for
/// constructs whose frame the stub must not build, before doing anything observable, or 1 and the slow path control
/// word of the exception if step 3 threw.
///
/// # Safety
///
/// JIT code calls this with its VM, an ECMAScript function and the pc of the construct instruction in its frame.
#[jit_abi]
pub unsafe extern "C" fn libjs_jit_construct_this(
    vm: *const Vm,
    function: *const EcmascriptFunctionObject,
    new_target: u64,
    pc: u32,
) -> ConstructThis {
    // SAFETY: JIT code passes live pointers.
    let (vm, function) = unsafe {
        (
            &*vm,
            Gc::from_non_null(NonNull::new(function.cast_mut()).expect("a function")),
        )
    };
    let declined = ConstructThis {
        this_value: 0,
        new_target: 0,
    };
    if !constructs_in_inline_frame(function) || vm.did_reach_stack_space_limit() {
        return declined;
    }
    let shared_data = function.shared_data();
    let new_target = match Value::from_bits(new_target) {
        new_target if new_target.is_empty() => {
            let new_target = vm.get_new_target();
            assert!(new_target.is_object());
            new_target
        }
        new_target => new_target,
    };

    // 2. Let kind be F.[[ConstructorKind]].
    // 3. If kind is base, then
    if function.constructor_kind() == ConstructorKind::Base {
        // NB: InitializeInstanceElements runs in the constructor's frame, which the runtime pushes.
        if function.has_class_data() && (function.private_methods_count() != 0 || function.fields_count() != 0) {
            return declined;
        }
        // a. Let thisArgument be ? OrdinaryCreateFromConstructor(newTarget, "%Object.prototype%").
        // NB: With room for the properties the constructor is known to add.
        let this_argument =
            match Object::create_from_constructor_for_construct(vm, new_target.as_function(), shared_data) {
                Ok(object) => object,
                Err(throw) => {
                    return ConstructThis {
                        this_value: 1,
                        new_target: handle_asm_exception(vm, pc, throw.value()).0 as u64,
                    };
                }
            };
        // NB: The prototype lookup above may have run code and collected garbage, which may have flushed the
        //     constructor's code (see `bytecode::flushing`).
        if let Err(throw) = function.compiled_executable(vm) {
            return ConstructThis {
                this_value: 1,
                new_target: handle_asm_exception(vm, pc, throw.value()).0 as u64,
            };
        }
        return ConstructThis {
            this_value: Value::from_object(this_argument).0,
            new_target: new_target.0,
        };
    }
    if shared_data.function_environment_has_this_binding() {
        return declined;
    }
    ConstructThis {
        this_value: Value::EMPTY.0,
        new_target: new_target.0,
    }
}

/// `i64 libjs_jit_throw_call_stack_size_exceeded(VM*, ExecutionContext* frame, u32 pc)`: throws the error of a call
/// at `pc` of the frame, the running execution context, with too little stack left.
///
/// # Safety
///
/// JIT code calls this with its VM, its frame and the pc of the call instruction.
#[jit_abi]
pub unsafe extern "C" fn libjs_jit_throw_call_stack_size_exceeded(
    vm: *const Vm,
    frame: *mut ExecutionContext,
    pc: u32,
) -> i64 {
    // SAFETY: JIT code passes live pointers.
    let vm = unsafe { &*vm };
    assert_eq!(vm.running_execution_context(), NonNull::new(frame));
    throw_call_stack_size_exceeded(vm, pc).0
}

/// `i64 libjs_jit_construct_result_error(VM*, ExecutionContext* frame, u32 pc, u64 result)`: throws the error of a
/// construct prepared by `libjs_jit_prepare_construct()` (for the instruction at `pc` of the frame, which runs again)
/// whose derived constructor's frame returned `result`, which is not an object, and returns its slow path control word.
///
/// # Safety
///
/// JIT code calls this with its VM, its frame and the pc of the construct instruction.
#[jit_abi]
pub unsafe extern "C" fn libjs_jit_construct_result_error(
    vm: *const Vm,
    frame: *mut ExecutionContext,
    pc: u32,
    result: u64,
) -> i64 {
    // SAFETY: JIT code passes live pointers.
    let vm = unsafe { &*vm };
    let frame = NonNull::new(frame).expect("JIT code passes its frame");
    let site = construct_site(frame, pc);
    frame_of(frame).slots()[site.dst as usize].set(Value::from_bits(result));
    finish_construct(
        vm,
        &site,
        None,
        true,
        None,
        SlowPathControl::continue_at(site.return_pc),
    )
    .0
}

/// `i64 libjs_jit_finish_direct_construct(VM*, ExecutionContext* frame, u32 pc, u64 status, u64 value)`: like
/// `libjs_jit_finish_direct_call()` for a construct prepared by `libjs_jit_prepare_construct()` whose frame did not
/// return to JIT code, which finishes the construct with the `value` that returned.
///
/// # Safety
///
/// JIT code calls this with its VM and frame, the pc of the construct instruction, the callee's status and the value.
#[jit_abi]
pub unsafe extern "C" fn libjs_jit_finish_direct_construct(
    vm: *const Vm,
    frame: *mut ExecutionContext,
    pc: u32,
    status: u64,
    value: u64,
) -> i64 {
    // SAFETY: JIT code passes live pointers.
    let vm = unsafe { &*vm };
    let frame = NonNull::new(frame).expect("JIT code passes its frame");
    let site = construct_site(frame, pc);
    let control = finish_direct_call_frames(vm, frame, &site, status);
    if !control.continues_in_frame() {
        return control.0;
    }
    let value = Value::from_bits(value);
    let this_argument = value.is_object().then(|| value.as_object());
    finish_construct(vm, &site, this_argument, this_argument.is_none(), None, control).0
}

/// What `libjs_jit_prepare_call_environment()` returns, in two registers.
#[repr(C)]
pub struct CallEnvironment {
    pub environment: u64,
    pub this_value: u64,
}

/// `CallEnvironment libjs_jit_prepare_call_environment(VM*, ECMAScriptFunctionObject*, u64 this_argument)`: the
/// environment of a call of the function that needs a function environment or the resolution of its this value, and
/// its this value (empty if it has none), or a null environment to take the generic path.
///
/// # Safety
///
/// JIT code calls this with its VM and an ECMAScript function that can be called inline.
#[jit_abi]
pub unsafe extern "C" fn libjs_jit_prepare_call_environment(
    vm: *const Vm,
    function: *const EcmascriptFunctionObject,
    this_argument: u64,
) -> CallEnvironment {
    // SAFETY: JIT code passes live pointers.
    let (vm, function) = unsafe {
        (
            &*vm,
            Gc::from_non_null(NonNull::new(function.cast_mut()).expect("a function")),
        )
    };
    let argument = Value::from_bits(this_argument);
    // NB: ToObject would make a wrapper of the caller's realm.
    if function.uses_this()
        && function.this_mode() == ThisMode::Global
        && !argument.is_nullish()
        && !argument.is_object()
        && function.realm() != vm.current_realm()
    {
        return CallEnvironment {
            environment: 0,
            this_value: 0,
        };
    }
    let environment = function.inline_call_environment(vm, None);
    let mut this_value = Value::EMPTY;
    if function.uses_this()
        && let Some(bound) = function.resolve_and_bind_this(vm, environment, argument)
    {
        this_value = bound;
    }
    if let Some(environment) = environment
        && function.function_environment_needed()
    {
        super::allocation::make_call_environment_template(vm, function, environment);
    }
    // NB: The allocations may have collected garbage and flushed the function's code (see `bytecode::flushing`),
    //     which the calling stub loads again from the function. The call takes the generic path if it cannot compile.
    if function.compiled_executable(vm).is_err() {
        return CallEnvironment {
            environment: 0,
            this_value: 0,
        };
    }
    CallEnvironment {
        environment: environment.map_or(0, |environment| environment.as_ptr() as u64),
        this_value: this_value.0,
    }
}

/// `u64 libjs_jit_array_push(Array*, u64 value)`: appends `value` to the array, which Array.prototype.push would
/// append it to without any observable step, and returns the new length as a value.
///
/// JIT code calls this with a live array whose elements are packed.
#[jit_abi]
pub extern "C" fn libjs_jit_array_push(array: &Array, value: u64) -> u64 {
    assert!(matches!(
        array.indexed_storage_kind(),
        IndexedStorageKind::None | IndexedStorageKind::Packed
    ));
    array.indexed_append(Value::from_bits(value), DEFAULT_ATTRIBUTES);
    Value::from_f64(f64::from(array.indexed_array_like_size())).0
}

/// `void libjs_jit_store_holey_element(Object*, u32 index, u64 value)`: stores `value` at `index`, below the length of
/// the holey elements of the object, growing the elements where they have no room for it (or making them a dictionary,
/// where the store is sparse), and makes them packed if it filled their last hole.
///
/// JIT code calls this with a live array whose prototype chain is the realm's default one without indexed properties,
/// so that the store runs no code.
#[jit_abi]
pub extern "C" fn libjs_jit_store_holey_element(object: &Object, index: u32, value: u64) {
    // NB: Branches on holey elements take packed ones too.
    assert!(matches!(
        object.indexed_storage_kind(),
        IndexedStorageKind::Holey | IndexedStorageKind::Packed
    ));
    assert!(index < object.indexed_array_like_size());
    object.indexed_put(index, Value::from_bits(value), DEFAULT_ATTRIBUTES);
}

/// `u64 libjs_jit_primitive_to_string(VM*, u64 primitive)`: `String(primitive)`, which is ToString of the primitive,
/// but the descriptive string of a symbol.
///
/// JIT code calls this with its VM and a primitive.
#[jit_abi]
pub extern "C" fn libjs_jit_primitive_to_string(vm: &Vm, primitive: u64) -> u64 {
    let primitive = Value::from_bits(primitive);
    assert!(!primitive.is_object());
    // 22.1.1.1 String ( value ), https://tc39.es/ecma262/#sec-string-constructor-string-value
    if primitive.is_symbol() {
        return Value::from_string(PrimitiveString::create(vm, primitive.as_symbol().descriptive_string())).0;
    }
    // NB: ToString of other primitives runs no code and cannot throw.
    Value::from_string(primitive.to_primitive_string(vm).must()).0
}

/// The address of the runtime's `double (double...)` function that computes `function`, which JIT code calls with
/// numbers (see `Op::CallFloat64Function`).
pub fn float64_function_address(function: Float64Function) -> u64 {
    match function {
        Float64Function::Exp => jit_function_address!(libjs_jit_math_exp),
        Float64Function::Log => jit_function_address!(libjs_jit_math_log),
        Float64Function::Sin => jit_function_address!(libjs_jit_math_sin),
        Float64Function::Cos => jit_function_address!(libjs_jit_math_cos),
        Float64Function::Tan => jit_function_address!(libjs_jit_math_tan),
        Float64Function::Pow => jit_function_address!(libjs_jit_math_pow),
    }
}

#[jit_abi]
extern "C" fn libjs_jit_math_exp(number: f64) -> f64 {
    math_object::exp_of_number(number)
}

#[jit_abi]
extern "C" fn libjs_jit_math_log(number: f64) -> f64 {
    math_object::log_of_number(number)
}

#[jit_abi]
extern "C" fn libjs_jit_math_sin(number: f64) -> f64 {
    math_object::sin_of_number(number)
}

#[jit_abi]
extern "C" fn libjs_jit_math_cos(number: f64) -> f64 {
    math_object::cos_of_number(number)
}

#[jit_abi]
extern "C" fn libjs_jit_math_tan(number: f64) -> f64 {
    math_object::tan_of_number(number)
}

#[jit_abi]
extern "C" fn libjs_jit_math_pow(base: f64, exponent: f64) -> f64 {
    value_conversions::exp_double(base, exponent)
}

/// `u64 libjs_jit_to_object(VM*, u64 primitive, u64 function)`: ToObject of a primitive other than undefined and
/// null, as the builtin function `function` makes it, or the empty value if that function is of another realm.
///
/// JIT code calls this with its VM, a function object and such a primitive.
#[jit_abi]
pub extern "C" fn libjs_jit_to_object(vm: &Vm, primitive: u64, function: u64) -> u64 {
    let (function, primitive) = (Value::from_bits(function).as_object(), Value::from_bits(primitive));
    assert!(!primitive.is_object() && !primitive.is_nullish());
    if vm.current_realm() != Some(function.shape().realm()) {
        return Value::EMPTY.0;
    }
    // NB: ToObject of other primitives runs no code and cannot throw.
    Value::from_object(primitive.to_object(vm).must()).0
}

/// `u64 libjs_jit_array_create(VM*, u32 length, u64 function)`: ArrayCreate(length) as the builtin function `function`
/// (the Array constructor) makes it, or the empty value if that function is of another realm.
///
/// JIT code calls this with its VM, a length below 2^31 and a function object.
#[jit_abi]
pub extern "C" fn libjs_jit_array_create(vm: &Vm, length: u32, function: u64) -> u64 {
    let realm = Value::from_bits(function).as_object().shape().realm();
    if vm.current_realm() != Some(realm) {
        return Value::EMPTY.0;
    }
    // NB: Lengths below 2^32 are valid.
    Value::from_object(Array::create(vm, realm, u64::from(length), None).must()).0
}

/// `u64 libjs_jit_slice_arguments(VM*, ExecutionContext* frame, i32 start)`: `Array.prototype.slice.call(arguments,
/// start)` with the arguments object of the frame, which the code never created, as an array value.
///
/// JIT code calls this with its VM and its frame.
#[jit_abi]
pub extern "C" fn libjs_jit_slice_arguments(vm: &Vm, frame: &ExecutionContext, start: i32) -> u64 {
    // 23.1.3.28 Array.prototype.slice ( start, end ), https://tc39.es/ecma262/#sec-array.prototype.slice
    // NB: The arguments object has every passed argument, and its length is their count.
    let length = i64::from(frame.passed_argument_count.get());
    let start = i64::from(start);
    let first = if start < 0 {
        (length + start).max(0)
    } else {
        start.min(length)
    };
    let mut arguments = ArgumentStorage::empty();
    arguments.fill_from(
        vm,
        frame.arguments()[first as usize..length as usize]
            .iter()
            .map(core::cell::Cell::get),
    );
    let realm = vm.current_realm().expect("JIT code runs in a realm");
    Value::from_object(Array::create_from(vm, realm, &arguments.values())).0
}

/// The GetById slow path of JIT code, before the full one. Generic GetById nodes have no inline cache check, so this
/// tries the property lookup cache first, like the interpreter's GetById does. Getters found there are called the way
/// calls from JIT code call their callees. Returns nothing if the full slow path should run.
pub fn get_by_id_from_jit(
    vm: &Vm,
    pc: u32,
    instruction: &op::GetById,
    values: &mut op::GetByIdValues,
) -> Option<SlowPathControl> {
    let executable = vm.current_executable();
    let cache = executable.property_lookup_cache(instruction.cache as usize);
    let value = try_get_by_id_cache_with(values.base, cache, CachedAccessors::Return);
    if value == Value::EMPTY {
        return None;
    }
    if !value.is_accessor() {
        values.dst = value;
        return Some(SlowPathControl::continue_at(pc + op::GetById::LENGTH));
    }
    let function = value
        .as_accessor()
        .getter()
        .and_then(as_ecmascript_function_object)
        .filter(|function| function.can_inline_call())?;
    let frame = vm.running_execution_context().expect("a frame is running");
    let site = JitCallSite {
        frame,
        pc,
        return_pc: pc + op::GetById::LENGTH,
        dst: instruction.dst.0.into(),
    };
    let control = call_ecmascript_function_from_jit(vm, &site, function, values.base, &[]);
    if control.continues_in_frame() {
        values.dst = frame_of(frame).slots()[instruction.dst.0 as usize].get();
    }
    Some(control)
}
