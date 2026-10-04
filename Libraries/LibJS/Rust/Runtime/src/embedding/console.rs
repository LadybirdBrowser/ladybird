/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The state of the console object, and printing values the way console messages show them.
//!
//! Everything here runs on the thread that owns the VM.

use std::io;

use crate::console::{Console, ConsoleClient, LogLevel};
use crate::embedding::abi_types::{
    JSByteSink, JSErrorData, JSRealm, JSUtf16View, cell_from_abi, error_data_from_abi, vm_from_abi,
};
use crate::embedding::host::console_client::JSConsoleClient;
use crate::gc::class::{GcCell, class_of};
use crate::layout::cell::Gc;
use crate::layout::host_class::{JSObject, JSVM, JSValue};
use crate::layout::value::Value;
use crate::runtime::console_object::ConsoleObject;
use crate::runtime::print::{PrintContext, print};

/// The Console of a realm's console object, which keeps its counters, timers and group stack and hands what it logs
/// to its client.
pub struct JSConsole {
    _opaque: [u8; 0],
}

// ConsoleLogLevel, in the order of LibJS/ConsoleLogLevel.h, whose enumerators have these values.
pub const JS_CONSOLE_LOG_LEVEL_ASSERT: u8 = 0;
pub const JS_CONSOLE_LOG_LEVEL_COUNT: u8 = 1;
pub const JS_CONSOLE_LOG_LEVEL_COUNT_RESET: u8 = 2;
pub const JS_CONSOLE_LOG_LEVEL_DEBUG: u8 = 3;
pub const JS_CONSOLE_LOG_LEVEL_DIR: u8 = 4;
pub const JS_CONSOLE_LOG_LEVEL_DIR_XML: u8 = 5;
pub const JS_CONSOLE_LOG_LEVEL_ERROR: u8 = 6;
pub const JS_CONSOLE_LOG_LEVEL_GROUP: u8 = 7;
pub const JS_CONSOLE_LOG_LEVEL_GROUP_COLLAPSED: u8 = 8;
pub const JS_CONSOLE_LOG_LEVEL_INFO: u8 = 9;
pub const JS_CONSOLE_LOG_LEVEL_LOG: u8 = 10;
pub const JS_CONSOLE_LOG_LEVEL_TIME_END: u8 = 11;
pub const JS_CONSOLE_LOG_LEVEL_TIME_LOG: u8 = 12;
pub const JS_CONSOLE_LOG_LEVEL_TABLE: u8 = 13;
pub const JS_CONSOLE_LOG_LEVEL_TRACE: u8 = 14;
pub const JS_CONSOLE_LOG_LEVEL_WARN: u8 = 15;

pub fn log_level_into_abi(log_level: LogLevel) -> u8 {
    match log_level {
        LogLevel::Assert => JS_CONSOLE_LOG_LEVEL_ASSERT,
        LogLevel::Count => JS_CONSOLE_LOG_LEVEL_COUNT,
        LogLevel::CountReset => JS_CONSOLE_LOG_LEVEL_COUNT_RESET,
        LogLevel::Debug => JS_CONSOLE_LOG_LEVEL_DEBUG,
        LogLevel::Dir => JS_CONSOLE_LOG_LEVEL_DIR,
        LogLevel::DirXML => JS_CONSOLE_LOG_LEVEL_DIR_XML,
        LogLevel::Error => JS_CONSOLE_LOG_LEVEL_ERROR,
        LogLevel::Group => JS_CONSOLE_LOG_LEVEL_GROUP,
        LogLevel::GroupCollapsed => JS_CONSOLE_LOG_LEVEL_GROUP_COLLAPSED,
        LogLevel::Info => JS_CONSOLE_LOG_LEVEL_INFO,
        LogLevel::Log => JS_CONSOLE_LOG_LEVEL_LOG,
        LogLevel::TimeEnd => JS_CONSOLE_LOG_LEVEL_TIME_END,
        LogLevel::TimeLog => JS_CONSOLE_LOG_LEVEL_TIME_LOG,
        LogLevel::Table => JS_CONSOLE_LOG_LEVEL_TABLE,
        LogLevel::Trace => JS_CONSOLE_LOG_LEVEL_TRACE,
        LogLevel::Warn => JS_CONSOLE_LOG_LEVEL_WARN,
    }
}

pub fn log_level_from_abi(log_level: u8) -> LogLevel {
    match log_level {
        JS_CONSOLE_LOG_LEVEL_ASSERT => LogLevel::Assert,
        JS_CONSOLE_LOG_LEVEL_COUNT => LogLevel::Count,
        JS_CONSOLE_LOG_LEVEL_COUNT_RESET => LogLevel::CountReset,
        JS_CONSOLE_LOG_LEVEL_DEBUG => LogLevel::Debug,
        JS_CONSOLE_LOG_LEVEL_DIR => LogLevel::Dir,
        JS_CONSOLE_LOG_LEVEL_DIR_XML => LogLevel::DirXML,
        JS_CONSOLE_LOG_LEVEL_ERROR => LogLevel::Error,
        JS_CONSOLE_LOG_LEVEL_GROUP => LogLevel::Group,
        JS_CONSOLE_LOG_LEVEL_GROUP_COLLAPSED => LogLevel::GroupCollapsed,
        JS_CONSOLE_LOG_LEVEL_INFO => LogLevel::Info,
        JS_CONSOLE_LOG_LEVEL_LOG => LogLevel::Log,
        JS_CONSOLE_LOG_LEVEL_TIME_END => LogLevel::TimeEnd,
        JS_CONSOLE_LOG_LEVEL_TIME_LOG => LogLevel::TimeLog,
        JS_CONSOLE_LOG_LEVEL_TABLE => LogLevel::Table,
        JS_CONSOLE_LOG_LEVEL_TRACE => LogLevel::Trace,
        JS_CONSOLE_LOG_LEVEL_WARN => LogLevel::Warn,
        _ => panic!("{log_level} is not a console log level"),
    }
}

/// # Safety
///
/// `console` must point to a live Console.
unsafe fn console_from_abi(console: *mut JSConsole) -> Gc<Console> {
    let console = core::ptr::NonNull::new(console.cast::<Console>()).expect("a console is not null");
    // SAFETY: The caller guarantees that the pointer is to a live Console.
    unsafe { Gc::from_non_null(console) }
}

pub fn console_into_abi(console: Gc<Console>) -> *mut JSConsole {
    console.as_ptr().cast()
}

/// ConsoleObject::console(): the console behind a realm's console object, or null if `console_object` is some other
/// object. The console is a cell that lives as long as its console object. Only on the VM's thread.
///
/// # Safety
///
/// `console_object` must point to a live object.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_console_object_console(console_object: *mut JSObject) -> *mut JSConsole {
    // SAFETY: The caller guarantees that the pointer is to a live object.
    unsafe { cell_from_abi::<JSObject>(console_object) }
        .downcast::<ConsoleObject>()
        .map_or(core::ptr::null_mut(), |console_object| {
            console_into_abi(console_object.console())
        })
}

/// Console::realm(): the realm whose console object owns the console, which keeps it alive. Only on the VM's thread.
///
/// # Safety
///
/// `console` must point to a live console.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_console_realm(console: *mut JSConsole) -> *mut JSRealm {
    // SAFETY: The caller passes a live console.
    unsafe { console_from_abi(console) }.realm().as_ptr().cast()
}

/// Console::set_client(): makes `client` the client the console hands what it logs to, and keeps it alive with the
/// console. A client is usually one js_console_client_create() made for this console. Only on the VM's thread.
///
/// # Safety
///
/// `console` must point to a live console and `client` to a live console client.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_console_set_client(console: *mut JSConsole, client: *mut JSConsoleClient) {
    // SAFETY: The caller passes a live console.
    let console = unsafe { console_from_abi(console) };
    let client = core::ptr::NonNull::new(client.cast::<ConsoleClient>()).expect("a console client is not null");
    // SAFETY: The caller guarantees that the pointer is to a live cell, whose class the assertion checks.
    let client = unsafe { Gc::from_non_null(client) };
    assert!(
        class_of(client).is_subclass_of(ConsoleClient::CLASS),
        "only a console client can be set as one"
    );
    console.set_client(client);
}

/// Console::report_exception(): has the console's client, if it has one, report an uncaught exception, given its
/// name, message and error data and whether it was a promise's rejection. Everything stays the caller's. Only on the
/// VM's thread.
///
/// # Safety
///
/// `console` must point to a live console, `error_data` to the ErrorData of a live object, and the views must be
/// valid for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_console_report_exception(
    console: *mut JSConsole,
    name: JSUtf16View,
    message: JSUtf16View,
    error_data: *const JSErrorData,
    in_promise: bool,
) {
    // SAFETY: The caller passes a live console.
    let console = unsafe { console_from_abi(console) };
    // SAFETY: The caller passes the ErrorData of a live object.
    let error_data = unsafe { error_data_from_abi(error_data) };
    // SAFETY: The caller guarantees that the views are valid for the call.
    let (name, message) = unsafe { (name.as_view(), message.as_view()) };
    console.report_exception(name, message, error_data, in_promise);
}

/// The stream a PrintContext writes to, which hands the bytes to the embedder's sink.
struct ByteSinkWriter<'a> {
    sink: &'a JSByteSink,
}

impl io::Write for ByteSinkWriter<'_> {
    fn write(&mut self, bytes: &[u8]) -> io::Result<usize> {
        if bytes.is_empty() {
            return Ok(0);
        }
        let append = self.sink.append.expect("a sink has an append function");
        // SAFETY: The embedder's sink receives its context and bytes that outlive the call.
        if unsafe { append(self.sink.context, bytes.as_ptr(), bytes.len()) } {
            Ok(bytes.len())
        } else {
            Err(io::Error::other("the byte sink did not take the bytes"))
        }
    }

    fn flush(&mut self) -> io::Result<()> {
        Ok(())
    }
}

/// JS::print(): writes the value to the sink the way the js REPL shows it, as UTF-8 in which lone surrogates are
/// encoded on their own, in ANSI colors unless `strip_ansi` is set, and with strings quoted and escaped unless
/// `raw_strings` is set. Returns false if the sink refused bytes, which ends the printing. The sink may run JavaScript,
/// even JavaScript that changes the value being printed. Only on the VM's thread.
///
/// # Safety
///
/// `vm` must point to a live VM and `sink` to a sink with an append function.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_console_print_value(
    vm: *mut JSVM,
    value: JSValue,
    sink: *const JSByteSink,
    strip_ansi: bool,
    raw_strings: bool,
) -> bool {
    // SAFETY: The caller passes a live VM.
    let vm = unsafe { vm_from_abi(vm) };
    // SAFETY: The caller passes a live sink.
    let sink = unsafe { sink.as_ref() }.expect("a sink is not null");
    let mut writer = ByteSinkWriter { sink };
    let mut print_context = PrintContext {
        vm,
        stream: &mut writer,
        strip_ansi,
        raw_strings,
    };
    print(Value(value), &mut print_context).is_ok()
}
