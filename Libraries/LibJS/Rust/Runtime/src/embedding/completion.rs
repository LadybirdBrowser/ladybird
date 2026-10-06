/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Logging every exception where it is thrown, which WebContent's --log-all-js-exceptions turns on through
//! JS::set_log_all_js_exceptions().
//!
//! The runtime logs when it throws an error of its own, when an exception leaves the bytecode it runs, and where it
//! turns a value it did not throw into a throw completion, such as the rejection that resumes an async function or that
//! a promise reaction without a handler passes on. The embedder logs the throws it starts itself with
//! js_completion_log_exception. The log is "THROW!" and the thrown value, or the "message" of a thrown object followed
//! by the call stack, one line per execution context from the running one down.

use core::cell::Cell;
use core::ffi::c_void;
use core::ops::ControlFlow;
use core::sync::atomic::{AtomicBool, Ordering};
use std::sync::Mutex;

use crate::bytecode::executable::Executable;
use crate::embedding::abi_types::{JSByteSink, value_from_abi, vm_from_abi};
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::host_class::{JSVM, JSValue};
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::utf16::Utf16View;

static LOG_ALL_EXCEPTIONS: AtomicBool = AtomicBool::new(false);

std::thread_local! {
    static IS_READING_MESSAGE_OF_THROWN_OBJECT: Cell<bool> = const { Cell::new(false) };
}

/// Where the lines of the log go: the embedder's writer, or the standard error without one.
static EXCEPTION_LOG_LINE_WRITER: Mutex<Option<ExceptionLogLineWriter>> = Mutex::new(None);

#[derive(Clone, Copy)]
struct ExceptionLogLineWriter {
    context: *mut c_void,
    append: unsafe extern "C" fn(context: *mut c_void, bytes: *const u8, length: usize) -> bool,
}

// SAFETY: Every thread that throws writes to the one writer the embedder installed for the process, which
//         js_completion_set_log_all_exceptions() requires any thread to be able to call.
unsafe impl Send for ExceptionLogLineWriter {}

/// Turns logging every exception where it is thrown on or off for the whole process. Each line of the log goes to
/// `line_writer`, without a line terminator, or to the standard error when `line_writer` is null. The writer is copied,
/// and its context must stay valid until logging is turned off or another writer replaces it.
///
/// # Safety
///
/// `line_writer` must be null or point to a sink with an append function that any thread may call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_completion_set_log_all_exceptions(enabled: bool, line_writer: *const JSByteSink) {
    // SAFETY: The caller passes null or a readable sink.
    let line_writer = unsafe { line_writer.as_ref() }.map(|sink| ExceptionLogLineWriter {
        context: sink.context,
        append: sink.append.expect("a byte sink has an append function"),
    });
    *EXCEPTION_LOG_LINE_WRITER
        .lock()
        .unwrap_or_else(std::sync::PoisonError::into_inner) = line_writer;
    LOG_ALL_EXCEPTIONS.store(enabled, Ordering::Relaxed);
}

/// Logs `value` as a thrown exception, if exceptions are logged, for a throw that the embedder starts itself, as C++
/// code does with JS::throw_completion(). Call on the VM's thread.
///
/// # Safety
///
/// `vm` must be the embedder's VM and `value` a value of it.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_completion_log_exception(vm: *mut JSVM, value: JSValue) {
    // SAFETY: The caller passes its VM.
    log_exception_if_enabled(unsafe { vm_from_abi(vm) }, value_from_abi(value));
}

/// Where the runtime throws, logs the thrown value if exceptions are logged.
#[inline]
pub fn log_exception_if_enabled(vm: &Vm, value: Value) {
    if LOG_ALL_EXCEPTIONS.load(Ordering::Relaxed) {
        log_exception(vm, value);
    }
}

#[cold]
#[inline(never)]
fn log_exception(vm: &Vm, value: Value) {
    let lines = exception_log_lines(vm, value);
    let line_writer = EXCEPTION_LOG_LINE_WRITER
        .lock()
        .unwrap_or_else(std::sync::PoisonError::into_inner);
    for line in &lines {
        match *line_writer {
            // SAFETY: The embedder's writer takes bytes that outlive the call, with the context it came with.
            Some(writer) => unsafe {
                (writer.append)(writer.context, line.as_ptr(), line.len());
            },
            None => eprintln!("{line}"),
        }
    }
}

// The message is read with [[Get]], which runs getters such as DOMException's. Where the getter throws, the message
// shows as the object stores it. Throws while the message is read are logged too, but read no message with a getter,
// so that a getter that throws its own object cannot recurse.
fn message_of_thrown_object(vm: &Vm, object: Gc<Object>) -> Value {
    let stored_message = || object.get_without_side_effects(vm, &vm.names.message);
    if IS_READING_MESSAGE_OF_THROWN_OBJECT.get() {
        return stored_message();
    }
    IS_READING_MESSAGE_OF_THROWN_OBJECT.set(true);
    let message = object.get(vm, &vm.names.message);
    IS_READING_MESSAGE_OF_THROWN_OBJECT.set(false);
    message.unwrap_or_else(|_| stored_message())
}

fn exception_log_lines(vm: &Vm, value: Value) -> Vec<String> {
    let throw_line = |shown_value: Value| {
        format!(
            "\x1b[31;1mTHROW!\x1b[0m {}",
            Utf16View::of_string(&shown_value.to_utf16_string_without_side_effects()).to_utf8()
        )
    };

    if !value.is_object() {
        return vec![throw_line(value)];
    }

    let mut lines = vec![throw_line(message_of_thrown_object(vm, value.as_object()))];
    vm.for_each_execution_context_top_to_bottom(|context| {
        let function_name = context
            .function
            .get()
            .map(|function| function.name_for_call_stack())
            .unwrap_or_default();
        let function_name = Utf16View::of_string(&function_name).to_utf8();
        let source_range = context
            .executable
            .get()
            .and_then(|executable| Executable::from_head(executable).source_range_at(context.program_counter.get()));
        lines.push(match source_range {
            Some(source_range) => format!(
                "-> {function_name} @ {}:{},{}",
                Utf16View::of_string(source_range.filename()).to_utf8(),
                source_range.start.line,
                source_range.start.column
            ),
            None => format!("-> {function_name}"),
        });
        ControlFlow::Continue(())
    });
    lines
}
