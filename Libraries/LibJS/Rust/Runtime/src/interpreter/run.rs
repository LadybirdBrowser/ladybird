/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::ffi::c_void;
use core::ptr::NonNull;

use super::vm::Vm;
use crate::bytecode::executable::Executable;
use crate::layout::cell::Gc;
use crate::layout::execution_context::ExecutionContext;
use crate::layout::value::Value;
use crate::runtime::completion::{Throw, ThrowCompletionOr};
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

    /// Runs `executable` as a script in a context of its own.
    pub fn run_script_executable(&self, executable: Gc<Executable>) -> Result<Value, Value> {
        // SAFETY: The caller passes a live executable.
        let executable_ref = unsafe { executable.as_non_null().as_ref() };
        let stack = self.interpreter_stack();
        let mark = stack.top.get();
        let constant_count = u32::try_from(executable_ref.constants().len()).expect("constant count fits in u32");
        let Some(context) = stack.allocate(executable_ref.registers_and_locals_count(), constant_count, 0) else {
            crate::interpreter::runtime_functions::unimplemented_runtime_function(
                "the InternalError for exceeding the call stack size",
                0,
            );
        };
        self.push_execution_context(context);
        let result = self.run_executable(context, executable, 0);
        self.pop_execution_context();
        stack.deallocate(mark);
        match result {
            Ok(value) if value == Value::EMPTY => Ok(Value::UNDEFINED),
            other => other,
        }
    }

    /// 16.1.6 ScriptEvaluation ( scriptRecord ), https://tc39.es/ecma262/#sec-runtime-semantics-scriptevaluation
    pub fn run_script(&self, script: Script) -> ThrowCompletionOr<Value> {
        let declarations = &script.compiled.declarations;
        if !declarations.lexical_names.is_empty()
            || !declarations.var_names.is_empty()
            || !declarations.functions_to_initialize.is_empty()
            || !declarations.lexical_bindings.is_empty()
        {
            crate::interpreter::runtime_functions::unimplemented_runtime_function(
                "global declaration instantiation",
                0,
            );
        }
        let executable = Executable::create(self, script.compiled.executable);
        self.run_script_executable(executable).map_err(Throw::new)
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
