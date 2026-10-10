/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Calls from JIT code: the call instructions it leaves to the runtime, and the slow paths for property accesses it
//! leaves to the runtime.

use core::ptr::NonNull;

use libjs_abi::register;

use super::code::EntryStatus;
use super::entry_exit::{can_enter_jit_code, enter_jit_code};
use super::frame_of;
use crate::bytecode::encoding::Operand;
use crate::bytecode::executable::Executable;
use crate::bytecode::instruction::OpCode;
use crate::bytecode::op;
use crate::gc::root::MarkedVec;
use crate::interpreter::runtime_functions::{Runtime, RuntimeFunctions, SlowPathControl, handle_asm_exception};
use crate::interpreter::slow_paths::calls::{
    CallType, execute_asm_call, record_callback, throw_error, throw_if_needed_for_asm_call,
};
use crate::interpreter::slow_paths::feedback::record_forwarded_call;
use crate::interpreter::slow_paths::property_access::{CachedAccessors, try_get_by_id_cache_with};
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::execution_context::ExecutionContext;
use crate::layout::feedback::CallFeedbackForwarding;
use crate::layout::function_object::{EcmascriptFunctionObject, RawNativeFunction};
use crate::layout::object::{IndexedStorageKind, Object};
use crate::layout::value::Value;
use crate::runtime::abstract_operations::{
    create_unmapped_arguments_object, get_prototype_from_constructor, length_of_array_like,
};
use crate::runtime::bound_function::BoundFunction;
use crate::runtime::ecmascript_function_object::{as_ecmascript_function_object, value_as_ecmascript_function_object};
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;
use crate::runtime::function_prototype::FunctionPrototype;
use crate::runtime::intrinsics::Intrinsics;
use crate::runtime::iterator::get_iterator_values;
use crate::runtime::native_javascript_backed_function::NativeJavaScriptBackedFunction;
use crate::runtime::property_key::PropertyKey;
use crate::runtime::shared_function_instance_data::{ConstructorKind, FunctionKind};

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
                vm.return_from_running_frame(Value(result.value));
                return finish_call_from_jit(vm, site.frame, site.pc, site.return_pc);
            }
            EntryStatus::ExitInterpreter => return finish_call_from_jit(vm, site.frame, site.pc, site.return_pc),
            EntryStatus::Resume => {}
        }
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

// 10.2.2 [[Construct]] ( argumentsList, newTarget ), https://tc39.es/ecma262/#sec-ecmascript-function-objects-construct-argumentslist-newtarget
/// Constructs with an ECMAScript function as the new target, in an inline frame linked to the call site's frame like a
/// call.
fn construct_ecmascript_function_from_jit(
    vm: &Vm,
    site: &JitCallSite,
    function: Gc<EcmascriptFunctionObject>,
    arguments: &[Value],
) -> SlowPathControl {
    if vm.did_reach_stack_space_limit() {
        return throw_call_stack_size_exceeded(vm, site.pc);
    }

    // 2. Let kind be F.[[ConstructorKind]].
    let kind = function.constructor_kind();

    // 3. If kind is base, then
    let mut this_argument = None;
    if kind == ConstructorKind::Base {
        // a. Let thisArgument be ? OrdinaryCreateFromConstructor(newTarget, "%Object.prototype%").
        // NB: With room for the properties the constructor is known to add.
        match get_prototype_from_constructor(vm, function.as_function_object_gc(), Intrinsics::object_prototype) {
            Ok(prototype) => this_argument = Some(Object::create_for_construct(vm, prototype, function.shared_data())),
            Err(throw) => return handle_asm_exception(vm, site.pc, throw.value()),
        }
    }

    // 4. Let calleeContext be PrepareForOrdinaryCall(F, newTarget).
    // NB: The frame does not use the construct return rule of Return, which knows nothing of derived constructors;
    //     steps 10 to 17 are below.
    let executable = function.bytecode_executable().expect("a constructor has an executable");
    let argument_count = u32::try_from(arguments.len()).expect("the argument count fits in u32");
    let Some(callee_frame) = vm.push_inline_frame_without_this(
        function,
        executable,
        arguments,
        argument_count,
        site.return_pc,
        site.dst,
        Some(function.upcast()),
        false,
    ) else {
        return throw_call_stack_size_exceeded(vm, site.pc);
    };
    let callee_frame_ref = frame_of(callee_frame);

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
            return handle_asm_exception(vm, site.pc, throw.value());
        }
    }

    // 7. Let constructorEnv be the LexicalEnvironment of calleeContext.
    let constructor_environment = callee_frame_ref
        .lexical_environment
        .get()
        .expect("a constructor runs in an environment");

    // 8. Let result be Completion(OrdinaryCallEvaluateBody(F, argumentsList)).
    // 9. Remove calleeContext from the execution context stack and restore callerContext as the running execution context.
    // 10. If result is a throw completion, then
    //     a. Return ? result.
    let control = run_callee_frame_from_jit(vm, site, callee_frame);
    if !control.continues_in_frame() {
        return control;
    }

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
                let mut arguments = ArgumentStorage::filled(vm, length as usize, |slots| {
                    array_like.copy_indexed_packed_elements(slots);
                });
                return Some(call_from_jit_or_generic(
                    vm,
                    site,
                    this_value,
                    target_this,
                    arguments.values(),
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
            return Some(argument_list.with_values(|arguments| {
                call_from_jit_or_generic(vm, site, this_value, target_this, arguments, unwrap_depth + 1)
            }));
        }

        return None;
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
        let mut argument_list = ArgumentStorage::filled(vm, bound_argument_count + arguments.len(), |slots| {
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
            argument_list.values(),
            unwrap_depth + 1,
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
        None,
        crate::bytecode::property_access::Strict::No,
    ) {
        return handle_asm_exception(vm, site.pc, throw.value());
    }
    site.store_result(result)
}

/// The argument values of a call from JIT code, which it keeps alive: few enough on the stack, which the garbage
/// collector scans, and others in a rooted list as well.
///
/// NB: The argument values cannot stay alive in the slots of the calling frame: frames of inlined calls that compiled
///     code pushes for a call are not initialized, and the garbage collector does not visit their registers and locals.
struct ArgumentStorage<'vm> {
    inline: [Value; ArgumentStorage::INLINE_CAPACITY],
    count: usize,
    heap: Vec<Value>,
    _rooted: Option<MarkedVec<'vm, Value>>,
}

impl<'vm> ArgumentStorage<'vm> {
    const INLINE_CAPACITY: usize = 16;

    fn new(vm: &'vm Vm, values: impl ExactSizeIterator<Item = Value>) -> Self {
        Self::filled(vm, values.len(), |slots| {
            for (slot, value) in slots.iter_mut().zip(values) {
                *slot = value;
            }
        })
    }

    /// `count` values, which `fill` writes into the slots it gets. Nothing may collect garbage while it does.
    fn filled(vm: &'vm Vm, count: usize, fill: impl FnOnce(&mut [Value])) -> Self {
        let mut storage = Self {
            inline: [Value::UNDEFINED; Self::INLINE_CAPACITY],
            count,
            heap: Vec::new(),
            _rooted: None,
        };
        if count <= Self::INLINE_CAPACITY {
            fill(&mut storage.inline[..count]);
        } else {
            storage.heap = vec![Value::UNDEFINED; count];
            fill(&mut storage.heap);
            let rooted = MarkedVec::with_capacity(vm, count);
            for value in &storage.heap {
                rooted.push(*value);
            }
            storage._rooted = Some(rooted);
        }
        storage
    }

    fn values(&mut self) -> &mut [Value] {
        if self.count <= Self::INLINE_CAPACITY {
            &mut self.inline[..self.count]
        } else {
            &mut self.heap
        }
    }
}

/// The operands that follow a call instruction with an argument list.
///
/// # Safety
///
/// `arguments` is the trailing operand array of an instruction with `count` operands after it.
unsafe fn trailing_operands(arguments: &[Operand; 0], count: u32) -> &[Operand] {
    // SAFETY: As above.
    unsafe { core::slice::from_raw_parts(arguments.as_ptr(), count as usize) }
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
    let instruction = &executable.bytecode()[pc as usize..];
    let opcode = instruction[0];

    macro_rules! instruction {
        ($op:ident) => {
            // SAFETY: The bytes at pc are an instruction of this kind.
            unsafe { &*instruction.as_ptr().cast::<op::$op>() }
        };
    }
    let site = |dst: Operand, length: u32| JitCallSite {
        frame,
        pc,
        return_pc: pc + length,
        dst: dst.0,
    };

    let control = match opcode {
        _ if opcode == OpCode::Call as u8 => {
            let call = instruction!(Call);
            let site = site(call.dst, call.length());
            // SAFETY: Call instructions are followed by their argument operands.
            let operands = unsafe { trailing_operands(&call.arguments, call.argument_count) };
            let mut storage = ArgumentStorage::new(vm, operands.iter().map(|operand| load(*operand)));
            let arguments = storage.values();
            let callee = load(call.callee);
            let this_value = load(call.this_value);
            if let Some(control) = call_from_jit(vm, &site, callee, this_value, arguments, 0) {
                return control.0;
            }
            let mut values = op::CallValues {
                dst: Value::EMPTY,
                callee,
                this_value,
                arguments: [],
            };
            let control = Runtime::call(vm, pc, call, &mut values, arguments);
            finish_generic_call(vm, &site, control, values.dst)
        }
        _ if opcode == OpCode::CallConstruct as u8 => {
            let call = instruction!(CallConstruct);
            let site = site(call.dst, call.length());
            // SAFETY: CallConstruct instructions are followed by their argument operands.
            let operands = unsafe { trailing_operands(&call.arguments, call.argument_count) };
            let mut storage = ArgumentStorage::new(vm, operands.iter().map(|operand| load(*operand)));
            let arguments = storage.values();
            let callee = load(call.callee);
            if let Some(function) = value_as_ecmascript_function_object(callee)
                && function.kind() == FunctionKind::Normal
                && function.has_constructor()
                && function.bytecode_executable().is_some()
            {
                return construct_ecmascript_function_from_jit(vm, &site, function, arguments).0;
            }
            let mut values = op::CallConstructValues {
                dst: Value::EMPTY,
                callee,
                arguments: [],
            };
            let control = Runtime::call_construct(vm, pc, call, &mut values, arguments);
            finish_generic_call(vm, &site, control, values.dst)
        }
        _ if opcode == OpCode::CallDirectEval as u8 => {
            let call = instruction!(CallDirectEval);
            let site = site(call.dst, call.length());
            // SAFETY: CallDirectEval instructions are followed by their argument operands.
            let operands = unsafe { trailing_operands(&call.arguments, call.argument_count) };
            let mut storage = ArgumentStorage::new(vm, operands.iter().map(|operand| load(*operand)));
            let arguments = storage.values();
            let mut values = op::CallDirectEvalValues {
                dst: Value::EMPTY,
                callee: load(call.callee),
                this_value: load(call.this_value),
                arguments: [],
            };
            let control = Runtime::call_direct_eval(vm, pc, call, &mut values, arguments);
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
        dst: call.dst.0,
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
    if let Err(throw) = throw_if_needed_for_asm_call(vm, callee, CallType::Call, call.expression_string.get()) {
        return handle_asm_exception(vm, pc, throw.value());
    }
    let this_value = slots[call.this_value.0 as usize].get();
    spread_arguments.with_values(|arguments| call_from_jit_or_generic(vm, &site, callee, this_value, arguments, 0))
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
    let instruction = &executable.bytecode()[pc as usize..];
    if instruction[0] == OpCode::CallWithArgumentArray as u8 {
        // SAFETY: The bytes at pc are a CallWithArgumentArray instruction.
        let call = unsafe { &*instruction.as_ptr().cast::<op::CallWithArgumentArray>() };
        return call_spreading_arguments_from_jit(vm, frame, pc, call).0;
    }
    assert_eq!(instruction[0], OpCode::Call as u8);
    // SAFETY: The bytes at pc are a Call instruction.
    let call = unsafe { &*instruction.as_ptr().cast::<op::Call>() };
    assert_eq!(call.argument_count, 2);
    let slots = frame_ref.slots();
    let site = JitCallSite {
        frame,
        pc,
        return_pc: pc + call.length(),
        dst: call.dst.0,
    };

    // 20.2.3.1 Function.prototype.apply ( thisArg, argArray ), https://tc39.es/ecma262/#sec-function.prototype.apply
    // 1. Let func be the this value.
    let function = slots[call.this_value.0 as usize].get();
    // 2. If IsCallable(func) is false, throw a TypeError exception.
    if !function.is_function() {
        return throw_error(vm, pc, ErrorKind::TypeError, ErrorType::NotAFunction, &[&function]).0;
    }
    // NB: The argument list of the arguments object is the frame's passed arguments, which nothing changed.
    // SAFETY: Call instructions are followed by their argument operands.
    let operands = unsafe { trailing_operands(&call.arguments, call.argument_count) };
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
    // NB: The passed arguments stay alive in the frame while the call runs.
    let arguments: Vec<Value> = frame_ref.arguments()[..passed_argument_count]
        .iter()
        .map(core::cell::Cell::get)
        .collect();
    call_from_jit_or_generic(vm, &site, function, this_argument, &arguments, 1).0
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
        dst: instruction.dst.0,
    };
    let control = call_ecmascript_function_from_jit(vm, &site, function, values.base, &[]);
    if control.continues_in_frame() {
        values.dst = frame_of(frame).slots()[instruction.dst.0 as usize].get();
    }
    Some(control)
}
