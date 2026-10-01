/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::ffi::c_void;
use core::ptr::NonNull;

use ak::ScopeGuard;

use super::vm::Vm;
use crate::bytecode::executable::Executable;
use crate::layout::cell::Gc;
use crate::layout::execution_context::{ExecutionContext, ScriptOrModule};
use crate::layout::function_object::EcmascriptFunctionObject;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::abstract_operations::new_function_environment;
use crate::runtime::completion::{Throw, ThrowCompletionOr};
use crate::runtime::environment::Environment;
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;
use crate::script::Script;
use libjs_abi::register;

unsafe extern "C" {
    /// Defined in the assembly flapc generates from interpreter.flap.
    fn js_interpreter(bytecode: *const u8, entry_point: u32, values: *mut Value, vm: *const c_void);
}

impl Vm {
    /// Runs `executable` in `context` from `entry_point` until it returns or throws.
    pub fn run_executable(
        &self,
        context: NonNull<ExecutionContext>,
        executable: Gc<Executable>,
        entry_point: u32,
    ) -> Result<Value, Value> {
        // SAFETY: The caller passes a live context and a live executable.
        let (context_ref, executable_ref) = unsafe { (context.as_ref(), executable.as_non_null().as_ref()) };
        let previous_running_execution_context = self.head.running_execution_context.replace(context.as_ptr());
        // SAFETY: An Executable starts with its head.
        context_ref
            .executable
            .set(Some(unsafe { Gc::from_non_null(executable.as_non_null().cast()) }));

        assert!(
            executable_ref.head.registers_and_locals_and_constants_count.get()
                <= context_ref.registers_and_constants_and_locals_and_arguments_count.get()
        );

        let this_register = context_ref.register(register::THIS_VALUE);
        if this_register.get() == Value::EMPTY {
            this_register.set(context_ref.this_value.get());
        }

        if self.interpreter_stack().is_exhausted() || self.did_reach_stack_space_limit() {
            crate::interpreter::runtime_functions::unimplemented_runtime_function(
                "the InternalError for exceeding the call stack size",
                0,
            );
        }
        // SAFETY: The interpreter runs the executable in the context, whose slots follow it.
        unsafe {
            js_interpreter(
                executable_ref.bytecode().as_ptr(),
                entry_point,
                context_ref.slots().as_ptr().cast_mut().cast(),
                core::ptr::from_ref(self).cast(),
            );
        }
        self.head
            .running_execution_context
            .set(previous_running_execution_context);
        self.head
            .execution_generation
            .set(self.head.execution_generation.get() + 1);

        let exception = context_ref.register(register::EXCEPTION).get();
        if exception != Value::EMPTY {
            return Err(exception);
        }
        Ok(context_ref.register(register::RETURN_VALUE).get())
    }

    // 16.1.6 ScriptEvaluation ( scriptRecord ), https://tc39.es/ecma262/#sec-runtime-semantics-scriptevaluation
    pub fn run_script(
        &self,
        script_record: Gc<Script>,
        lexical_environment_override: Option<Gc<Environment>>,
    ) -> ThrowCompletionOr<Value> {
        let vm = self;

        // 1. Let globalEnv be scriptRecord.[[Realm]].[[GlobalEnv]].
        let global_environment = script_record.realm().global_environment();

        // NOTE: Spec steps are rearranged in order to compute number of registers+constants+locals before construction of the execution context.

        // 12. Let result be Completion(GlobalDeclarationInstantiation(script, globalEnv)).
        let instantiation_result = script_record.global_declaration_instantiation(vm, global_environment);
        let mut result = instantiation_result.map(|()| Value::UNDEFINED);

        // 11. Let script be scriptRecord.[[ECMAScriptCode]].
        let executable = script_record.cached_executable();
        let registers_and_locals_count = executable.registers_and_locals_count();
        let constant_count = u32::try_from(executable.constants().len()).expect("constant count fits in u32");

        // 2. Let scriptContext be a new ECMAScript code execution context.
        let stack = vm.interpreter_stack();
        let stack_mark = stack.top.get();
        let Some(script_context) = stack.allocate(registers_and_locals_count, constant_count, 0) else {
            return vm.throw_completion(ErrorKind::InternalError, ErrorType::CallStackSizeExceeded, &[]);
        };
        let _deallocate_guard = ScopeGuard::new(|| stack.deallocate(stack_mark));
        // SAFETY: The context was just allocated and stays allocated until the guard frees it.
        let script_context_ref = unsafe { script_context.as_ref() };

        // 3. Set the Function of scriptContext to null.
        // NOTE: This was done during execution context construction.

        // 4. Set the Realm of scriptContext to scriptRecord.[[Realm]].
        script_context_ref.realm.set(Some(script_record.realm()));

        // 5. Set the ScriptOrModule of scriptContext to scriptRecord.
        script_context_ref
            .script_or_module
            .set(ScriptOrModule::Script(script_record));

        // 6. Set the VariableEnvironment of scriptContext to globalEnv.
        script_context_ref
            .variable_environment
            .set(Some(global_environment.upcast()));

        // 7. Set the LexicalEnvironment of scriptContext to globalEnv.
        script_context_ref
            .lexical_environment
            .set(Some(global_environment.upcast()));

        // Non-standard: Override the lexical environment if requested.
        if let Some(lexical_environment_override) = lexical_environment_override {
            script_context_ref
                .lexical_environment
                .set(Some(lexical_environment_override));
        }

        // 8. Set the PrivateEnvironment of scriptContext to null.

        // 9. Suspend the currently running execution context.
        // 10. Push scriptContext onto the execution context stack; scriptContext is now the running execution context.
        vm.push_execution_context(script_context);

        // 13. If result.[[Type]] is normal, then
        if result.is_ok() {
            // a. Set result to Completion(Evaluation of script).
            result = vm.run_executable(script_context, executable, 0).map_err(Throw::new);

            // b. If result is a normal completion and result.[[Value]] is empty, then
            if result.is_ok_and(Value::is_empty) {
                // i. Set result to NormalCompletion(undefined).
                result = Ok(Value::UNDEFINED);
            }
        }

        // 14. Suspend scriptContext and remove it from the execution context stack.
        vm.pop_execution_context();

        // 15. Assert: The execution context stack is not empty.
        assert!(!vm.execution_context_stack_is_empty());

        // FIXME: 16. Resume the context that is now on the top of the execution context stack as the running execution context.

        vm.head.execution_generation.set(vm.head.execution_generation.get() + 1);

        // 17. Return ? result.
        result
    }

    /// Enters a frame for a call of `callee_function` that the interpreter runs inline, as Return leaves it: the
    /// frame is linked to the running one through caller_frame, rather than pushed onto the execution context stack.
    /// Returns the callee's context, or None if the interpreter stack has no room for it.
    #[allow(clippy::too_many_arguments)]
    pub fn push_inline_frame(
        &self,
        callee_function: Gc<EcmascriptFunctionObject>,
        callee_executable: Gc<Executable>,
        arguments: &[Value],
        return_pc: u32,
        dst_raw: u32,
        this_value: Value,
        new_target: Option<Gc<Object>>,
        is_construct: bool,
    ) -> Option<NonNull<ExecutionContext>> {
        let stack = self.interpreter_stack();

        let insn_argument_count = u32::try_from(arguments.len()).expect("the argument count fits in u32");
        let registers_and_locals_count = callee_executable.registers_and_locals_count();
        let constant_count =
            u32::try_from(callee_executable.constants().len()).expect("the constant count fits in u32");
        let argument_count = insn_argument_count.max(callee_function.formal_parameter_count());

        let callee_context_pointer = stack.allocate(registers_and_locals_count, constant_count, argument_count)?;
        // SAFETY: The context was just allocated, and stays allocated until the interpreter returns from it or
        // unwinds it.
        let callee_context = unsafe { callee_context_pointer.as_ref() };

        // Copy the supplied arguments into the callee's argument slots.
        let callee_argument_values = callee_context.arguments();
        for (slot, argument) in callee_argument_values.iter().zip(arguments) {
            slot.set(*argument);
        }
        for slot in &callee_argument_values[arguments.len()..] {
            slot.set(Value::UNDEFINED);
        }
        callee_context.passed_argument_count.set(insn_argument_count);

        // Set up caller linkage so Return can restore the caller frame.
        callee_context
            .caller_frame
            .set(self.head.running_execution_context.get());
        callee_context.caller_dst_raw.set(dst_raw);
        callee_context.caller_return_pc.set(return_pc);
        callee_context.caller_is_construct.set(is_construct);

        // Inlined PrepareForOrdinaryCall (avoids function call overhead on hot path).
        callee_context
            .function
            .set(Some(callee_function.as_function_object_gc()));
        callee_context.realm.set(callee_function.realm());
        callee_context.script_or_module.set(callee_function.script_or_module());
        if callee_function.function_environment_needed() {
            let local_environment = new_function_environment(self, callee_function, new_target);
            let shared_data = callee_function.shared_data();
            let function_environment_bindings_count = shared_data.function_environment_bindings_count();
            local_environment.set_environment_shape_cache(
                shared_data.function_environment_shape_cache(),
                function_environment_bindings_count,
            );
            local_environment.ensure_capacity(function_environment_bindings_count);
            callee_context.lexical_environment.set(Some(local_environment.upcast()));
            callee_context
                .variable_environment
                .set(Some(local_environment.upcast()));
        } else {
            callee_context.lexical_environment.set(callee_function.environment());
            callee_context.variable_environment.set(callee_function.environment());
        }
        callee_context
            .private_environment
            .set(callee_function.private_environment());

        // Inline JS-to-JS frames stay out of the VM execution context stack and
        // are tracked through caller_frame instead.
        self.head.running_execution_context.set(callee_context_pointer.as_ptr());

        // Bind this if the function uses it.
        if callee_function.uses_this() {
            callee_function.ordinary_call_bind_this(self, callee_context, this_value);
        }

        // Set up execution context fields that run_executable normally does.
        // NB: We must use the callee's realm (not the caller's) for global_object
        //     and global_declarative_environment, since the caller's realm may differ
        //     in cross-realm calls (e.g. iframe <-> parent).
        callee_context.executable.set(Some(Executable::head(callee_executable)));

        // Set this value register.
        callee_context
            .register(register::THIS_VALUE)
            .set(callee_context.this_value.get());

        Some(callee_context_pointer)
    }

    /// Leaves the running frame, which the interpreter entered inline, for the frame that called it.
    #[inline(never)]
    pub fn unwind_inline_frame_for_exception(&self) {
        let callee_frame = self.running_execution_context().expect("an inline frame is running");
        // SAFETY: The running context is live until it is deallocated below.
        let caller_frame = unsafe { callee_frame.as_ref() }.caller_frame.get();
        assert!(!caller_frame.is_null());

        self.interpreter_stack().deallocate(callee_frame.as_ptr().cast());
        self.head.running_execution_context.set(caller_frame);
    }

    /// Unwinds to the innermost handler of an exception thrown at `program_counter` in the running frame, leaving
    /// frames the interpreter entered without returning to the runtime.
    pub fn handle_exception(&self, mut program_counter: u32, exception: Value) -> HandleExceptionResponse {
        loop {
            let context_pointer = self
                .running_execution_context()
                .expect("an exception is thrown in a running frame");
            // SAFETY: The running context is live until it is deallocated below.
            let context = unsafe { context_pointer.as_ref() };
            let executable = context.executable.get().expect("a running frame has an executable");
            // SAFETY: Executables start with their head.
            let executable = unsafe { executable.as_non_null().cast::<Executable>().as_ref() };
            if let Some(handler) = executable.exception_handlers_for_offset(program_counter) {
                context.register(register::EXCEPTION).set(exception);
                context.program_counter.set(handler.handler_offset);
                return HandleExceptionResponse::ContinueInThisExecutable;
            }

            // If we're in an inline frame, unwind to the caller and try its handlers.
            let caller_frame = context.caller_frame.get();
            if !caller_frame.is_null() {
                let caller_pc = context.caller_return_pc.get();
                self.interpreter_stack().deallocate(context_pointer.as_ptr().cast());
                self.head.running_execution_context.set(caller_frame);

                // The caller's return address is one past the Call instruction, and handler ranges exclude their end
                // offset, so look up the handler for an offset inside the Call.
                program_counter = caller_pc - 1;
                continue;
            }

            context.register(register::EXCEPTION).set(exception);
            return HandleExceptionResponse::ExitFromExecutable;
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum HandleExceptionResponse {
    ExitFromExecutable,
    ContinueInThisExecutable,
}
