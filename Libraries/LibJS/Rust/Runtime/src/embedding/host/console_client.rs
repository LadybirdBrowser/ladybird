/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! A console client that the embedder implements: the C++ subclasses of ConsoleClient, such as WebContent's
//! DevToolsConsoleClient, become a HostConsoleClient with a table of C methods and a C++ GC cell as their context.
//!
//! Everything here runs on the thread that owns the VM.

use core::ffi::c_void;
use core::ptr::NonNull;

use libjs_runtime_macros::Trace;

use crate::console::{ConsoleClient, ConsoleClientMethods, LogLevel, PrinterArguments, Trace as ConsoleTrace};
use crate::embedding::abi_types::{
    JSErrorData, JSOwnedUtf16String, JSUtf16View, completion_from_abi, completion_writing_result_to,
    error_data_into_abi, owned_utf16_string_into_abi, vm_from_abi, vm_into_abi,
};
use crate::embedding::console::{JSConsole, log_level_into_abi};
use crate::gc::class::{GcCell, class_of, define_cell};
use crate::gc::foreign::ForeignCellSlot;
use crate::gc::root::MarkedVec;
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::host_class::{JSCompletion, JSVM, JSValue};
use crate::layout::value::Value;
use crate::runtime::completion::ThrowCompletionOr;
use crate::runtime::error_data::ErrorData;
use crate::utf16::Utf16View;

/// A ConsoleClient, whichever class implements it.
pub struct JSConsoleClient {
    _opaque: [u8; 0],
}

// The alternative of ConsoleClient::PrinterArguments that a printer receives, numbered like the Variant's types.
pub const JS_CONSOLE_PRINTER_ARGUMENTS_GROUP: u8 = 0;
pub const JS_CONSOLE_PRINTER_ARGUMENTS_TRACE: u8 = 1;
pub const JS_CONSOLE_PRINTER_ARGUMENTS_VALUES: u8 = 2;

/// Console::TraceFrame. The source file, line and column are only meaningful when their flags say they are present.
#[repr(C)]
pub struct JSConsoleTraceFrame {
    pub function_name: JSUtf16View,
    pub source_file: JSUtf16View,
    pub line: usize,
    pub column: usize,
    pub has_source_file: bool,
    pub has_line: bool,
    pub has_column: bool,
}

/// ConsoleClient::PrinterArguments, which lives for the duration of the printer call. A group has a label, a trace a
/// label and its frames, from the caller of console.trace() outwards, and values have values, which stay alive for the
/// call.
#[repr(C)]
pub struct JSConsolePrinterArguments {
    pub kind: u8,
    pub label: JSUtf16View,
    pub trace_frames: *const JSConsoleTraceFrame,
    pub trace_frame_count: usize,
    pub values: *const JSValue,
    pub value_count: usize,
}

/// The virtual methods of a ConsoleClient that the embedder implements. Each receives the context of the client, and
/// may run JavaScript. A null method does what the C++ base class does: nothing, and a printer that prints nothing
/// returns undefined.
#[repr(C)]
pub struct JSConsoleClientMethods {
    /// Printer(logLevel, args): returns a completion with a value, or throws what console method threw.
    pub printer: Option<
        unsafe extern "C" fn(
            context: *mut c_void,
            vm: *mut JSVM,
            log_level: u8,
            arguments: *const JSConsolePrinterArguments,
        ) -> JSCompletion,
    >,
    /// The CSS style of a %c directive, which applies to the rest of the message being formatted.
    pub add_css_style_to_current_message: Option<unsafe extern "C" fn(context: *mut c_void, style: JSUtf16View)>,
    /// An uncaught exception, with its name, message and the error data of the error object, and whether a promise
    /// rejected with it.
    pub report_exception: Option<
        unsafe extern "C" fn(
            context: *mut c_void,
            name: JSUtf16View,
            message: JSUtf16View,
            error_data: *const JSErrorData,
            in_promise: bool,
        ),
    >,
    pub clear: Option<unsafe extern "C" fn(context: *mut c_void)>,
    pub end_group: Option<unsafe extern "C" fn(context: *mut c_void)>,
}

/// A ConsoleClient whose virtual methods are the embedder's C methods. It keeps its context, a C++ GC cell, alive.
#[repr(C)]
#[derive(Trace)]
pub struct HostConsoleClient {
    base: ConsoleClient,
    #[gc(untraced)]
    host_methods: &'static JSConsoleClientMethods,
    context: ForeignCellSlot,
}

define_cell!(HostConsoleClient, Other, extends: [ConsoleClient]);

static HOST_CONSOLE_CLIENT_METHODS: ConsoleClientMethods = ConsoleClientMethods {
    printer: HostConsoleClient::printer,
    add_css_style_to_current_message: HostConsoleClient::add_css_style_to_current_message,
    report_exception: HostConsoleClient::report_exception,
    clear: HostConsoleClient::clear,
    end_group: HostConsoleClient::end_group,
};

/// What a trace frame's strings are while the printer runs.
fn trace_frames_into_abi(trace: &ConsoleTrace) -> Vec<JSConsoleTraceFrame> {
    trace
        .stack
        .iter()
        .map(|frame| JSConsoleTraceFrame {
            function_name: JSUtf16View::of(Utf16View::of_string(&frame.function_name)),
            source_file: JSUtf16View::of(
                frame
                    .source_file
                    .as_ref()
                    .map_or(Utf16View::EMPTY, Utf16View::of_string),
            ),
            line: frame.line.unwrap_or(0),
            column: frame.column.unwrap_or(0),
            has_source_file: frame.source_file.is_some(),
            has_line: frame.line.is_some(),
            has_column: frame.column.is_some(),
        })
        .collect()
}

impl HostConsoleClient {
    fn of(client: &ConsoleClient) -> &HostConsoleClient {
        // SAFETY: Only HostConsoleClient has these methods.
        unsafe { &*core::ptr::from_ref(client).cast::<HostConsoleClient>() }
    }

    fn printer<'vm>(
        client: &ConsoleClient,
        vm: &'vm Vm,
        log_level: LogLevel,
        arguments: PrinterArguments<'vm>,
    ) -> ThrowCompletionOr<Value> {
        let this = Self::of(client);
        let Some(printer) = this.host_methods.printer else {
            return Ok(Value::UNDEFINED);
        };

        // The views point into `arguments` and these copies, which all live until the printer returns. The values
        // stay alive in the MarkedVec of `arguments`.
        let trace_frames;
        let values;
        let printer_arguments = match &arguments {
            PrinterArguments::Group(group) => JSConsolePrinterArguments {
                kind: JS_CONSOLE_PRINTER_ARGUMENTS_GROUP,
                label: JSUtf16View::of(Utf16View::of_string(&group.label)),
                trace_frames: core::ptr::null(),
                trace_frame_count: 0,
                values: core::ptr::null(),
                value_count: 0,
            },
            PrinterArguments::Trace(trace) => {
                trace_frames = trace_frames_into_abi(trace);
                JSConsolePrinterArguments {
                    kind: JS_CONSOLE_PRINTER_ARGUMENTS_TRACE,
                    label: JSUtf16View::of(Utf16View::of_string(&trace.label)),
                    trace_frames: trace_frames.as_ptr(),
                    trace_frame_count: trace_frames.len(),
                    values: core::ptr::null(),
                    value_count: 0,
                }
            }
            PrinterArguments::Values(marked_values) => {
                values = marked_values.to_vec();
                JSConsolePrinterArguments {
                    kind: JS_CONSOLE_PRINTER_ARGUMENTS_VALUES,
                    label: JSUtf16View::of(Utf16View::EMPTY),
                    trace_frames: core::ptr::null(),
                    trace_frame_count: 0,
                    values: values.as_ptr().cast::<JSValue>(),
                    value_count: values.len(),
                }
            }
        };

        // SAFETY: The embedder's printer receives the context it gave the client, and arguments whose strings and
        //         values outlive the call.
        let completion = unsafe {
            printer(
                this.context.as_ptr(),
                vm_into_abi(vm),
                log_level_into_abi(log_level),
                &raw const printer_arguments,
            )
        };
        completion_from_abi(completion)
    }

    fn add_css_style_to_current_message(client: &ConsoleClient, style: Utf16View<'_>) {
        let this = Self::of(client);
        if let Some(add_css_style_to_current_message) = this.host_methods.add_css_style_to_current_message {
            // SAFETY: The embedder's method receives the context it gave the client, and a view that outlives the
            //         call.
            unsafe { add_css_style_to_current_message(this.context.as_ptr(), JSUtf16View::of(style)) };
        }
    }

    fn report_exception(
        client: &ConsoleClient,
        name: Utf16View<'_>,
        message: Utf16View<'_>,
        error_data: &ErrorData,
        in_promise: bool,
    ) {
        let this = Self::of(client);
        if let Some(report_exception) = this.host_methods.report_exception {
            // SAFETY: The embedder's method receives the context it gave the client, and views and error data that
            //         outlive the call.
            unsafe {
                report_exception(
                    this.context.as_ptr(),
                    JSUtf16View::of(name),
                    JSUtf16View::of(message),
                    error_data_into_abi(error_data),
                    in_promise,
                );
            }
        }
    }

    fn clear(client: &ConsoleClient) {
        let this = Self::of(client);
        if let Some(clear) = this.host_methods.clear {
            // SAFETY: The embedder's method receives the context it gave the client.
            unsafe { clear(this.context.as_ptr()) };
        }
    }

    fn end_group(client: &ConsoleClient) {
        let this = Self::of(client);
        if let Some(end_group) = this.host_methods.end_group {
            // SAFETY: The embedder's method receives the context it gave the client.
            unsafe { end_group(this.context.as_ptr()) };
        }
    }
}

/// # Safety
///
/// `client` must point to a live console client.
unsafe fn console_client_from_abi(client: *mut JSConsoleClient) -> Gc<ConsoleClient> {
    let client = NonNull::new(client.cast::<ConsoleClient>()).expect("a console client is not null");
    // SAFETY: The caller guarantees that the pointer is to a live cell, whose class the assertion checks.
    let client = unsafe { Gc::from_non_null(client) };
    assert!(class_of(client).is_subclass_of(ConsoleClient::CLASS));
    client
}

/// Creates a console client of `console` whose virtual methods are `methods`, which are called with `context`. The
/// client keeps `context` alive. Install it with js_console_set_client(); until something holds it, the caller must
/// keep it alive, as it would any other cell. Only on the VM's thread.
///
/// # Safety
///
/// `console` must point to a live console, `methods` to a table that outlives every client created with it, which is
/// static data in practice, and `context` must be null or a live C++ GC cell of the VM's heap.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_console_client_create(
    vm: *mut JSVM,
    console: *mut JSConsole,
    methods: *const JSConsoleClientMethods,
    context: *mut c_void,
) -> *mut JSConsoleClient {
    // SAFETY: The caller passes a live VM.
    let vm = unsafe { vm_from_abi(vm) };
    let console = NonNull::new(console.cast()).expect("a console is not null");
    // SAFETY: The caller passes a live console.
    let console = unsafe { Gc::from_non_null(console) };
    // SAFETY: The caller guarantees that the table outlives every client created with it.
    let host_methods = unsafe { methods.as_ref() }.expect("a console client has methods");
    let client = vm.heap().allocate(HostConsoleClient {
        base: ConsoleClient::new(HostConsoleClient::CLASS, &HOST_CONSOLE_CLIENT_METHODS, console),
        host_methods,
        context: ForeignCellSlot::empty(),
    });
    // SAFETY: The caller passes null or a live cell of the heap, which the slot keeps alive from now on.
    unsafe { client.context.set(NonNull::new(context)) };
    client.as_ptr().cast()
}

/// ConsoleClient::generically_format_values(): formats the values the way a console prints them, separated by
/// spaces. On a normal completion, `*out_formatted` receives the raw word of an AK::Utf16String the caller owns, and
/// on a throw completion it is left alone. Only on the VM's thread.
///
/// # Safety
///
/// `client` must point to a live console client, `values` to `value_count` values, and `out_formatted` to writable
/// storage.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_console_client_generically_format_values(
    vm: *mut JSVM,
    client: *mut JSConsoleClient,
    values: *const JSValue,
    value_count: usize,
    out_formatted: *mut JSOwnedUtf16String,
) -> JSCompletion {
    // SAFETY: The caller passes a live VM.
    let vm = unsafe { vm_from_abi(vm) };
    // SAFETY: The caller passes a live console client.
    let client = unsafe { console_client_from_abi(client) };
    let marked_values = MarkedVec::with_capacity(vm, value_count);
    if value_count > 0 {
        // SAFETY: The caller guarantees that `values` points to `value_count` values.
        for &value in unsafe { core::slice::from_raw_parts(values, value_count) } {
            marked_values.push(Value(value));
        }
    }
    let formatted = client
        .generically_format_values(vm, &marked_values)
        .map(owned_utf16_string_into_abi);
    // SAFETY: The caller provides writable storage for the string.
    unsafe { completion_writing_result_to(formatted, out_formatted) }
}
