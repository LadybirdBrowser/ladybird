/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The global functions the runtime tests call besides the harness, and the hook that runs the test262 parser tests
//! instead of the runtime tests.

use core::ops::ControlFlow;
use std::ffi::{CStr, CString};

use ak::{Utf16FlyString, Utf16String};

use super::javascript_test_runner::{JSFileResult, parse_module, parse_script};
use super::test_runner::{Case, Suite, TestResult, get_time_in_ms, relative_path};
use crate::bytecode::property_access::Strict;
use crate::gc::class::{Extends, GcCell};
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::object::Object;
use crate::layout::realm::Realm;
use crate::layout::value::Value;
use crate::layout_forward::RawNativeFunctionPointer;
use crate::runtime::abstract_operations::{call, can_be_held_weakly};
use crate::runtime::array_buffer::{ArrayBuffer, Order, detach_array_buffer};
use crate::runtime::completion::ThrowCompletionOr;
use crate::runtime::date::clear_system_time_zone_cache;
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;
use crate::runtime::finalization_registry::FinalizationRegistry;
use crate::runtime::native_function::raw_native;
use crate::runtime::primitive_string::PrimitiveString;
use crate::runtime::symbol::Symbol;
use crate::runtime::typed_array::{Uint8Array, typed_array_from};
use crate::runtime::weak_map::WeakMap;
use crate::runtime::weak_set::WeakSet;
use crate::script::Script;
use crate::unicode::time_zone as unicode_time_zone;
use crate::utf16::Utf16View;

/// The global functions the runtime tests call besides the harness, in the order they are registered in. Like the
/// functions TESTJS_GLOBAL_FUNCTION defines, each has a length of 1.
pub const EXPOSED_GLOBAL_FUNCTIONS: &[(&str, RawNativeFunctionPointer)] = &[
    ("canParseSource", raw_native!(can_parse_source)),
    ("gc", raw_native!(collect_garbage)),
    (
        "collectGarbageOnEveryAllocation",
        raw_native!(collect_garbage_on_every_allocation),
    ),
    ("addEnginePrivateProperty", raw_native!(add_engine_private_property)),
    ("evaluateSource", raw_native!(evaluate_source)),
    ("evaluateModule", raw_native!(evaluate_module)),
    ("runQueuedPromiseJobs", raw_native!(run_queued_promise_jobs)),
    ("clearKeptObjects", raw_native!(clear_kept_objects)),
    (
        "runQueuedFinalizationRegistryCleanupJobs",
        raw_native!(run_queued_finalization_registry_cleanup_jobs),
    ),
    ("getWeakSetSize", raw_native!(get_weak_set_size)),
    ("getWeakMapSize", raw_native!(get_weak_map_size)),
    ("markAsGarbage", raw_native!(mark_as_garbage)),
    (
        "cleanupFinalizationRegistry",
        raw_native!(cleanup_finalization_registry),
    ),
    ("detachArrayBuffer", raw_native!(detach_array_buffer_function)),
    ("setTimeZone", raw_native!(set_time_zone)),
    ("toUTF8Bytes", raw_native!(to_utf8_bytes)),
    ("createDefaultTypedArray", raw_native!(create_default_typed_array)),
];

fn code_units_of(string: &Utf16String) -> Vec<u16> {
    Utf16View::of_string(string).code_units().collect()
}

/// Value::as_if<T>(): the object `value` is, if it is a T.
fn as_if<T: GcCell + Extends<Object>>(value: Value) -> Option<Gc<T>> {
    if !value.is_object() {
        return None;
    }
    value.as_object().downcast::<T>()
}

fn can_parse_source(vm: &Vm) -> ThrowCompletionOr<Value> {
    let realm = vm.current_realm().expect("canParseSource runs in a realm");
    let source = vm.argument(0).to_utf16_string(vm)?;
    let script = Script::parse(vm, &code_units_of(&source), realm);
    Ok(Value::from_bool(script.is_ok()))
}

#[allow(clippy::unnecessary_wraps, reason = "native functions return a completion")]
fn collect_garbage(vm: &Vm) -> ThrowCompletionOr<Value> {
    vm.heap().collect_garbage();
    Ok(Value::UNDEFINED)
}

fn collect_garbage_on_every_allocation(vm: &Vm) -> ThrowCompletionOr<Value> {
    let heap = vm.heap();
    let previous = heap.should_collect_on_every_allocation();
    heap.set_should_collect_on_every_allocation(true);
    let result = call(vm, vm.argument(0), Value::UNDEFINED, &[]);
    heap.set_should_collect_on_every_allocation(previous);
    result
}

fn add_engine_private_property(vm: &Vm) -> ThrowCompletionOr<Value> {
    let object = vm.argument(0).to_object(vm)?;
    let key = Symbol::create_private(vm);
    object.set_engine_private_property(vm, key, vm.argument(1));
    Ok(Value::UNDEFINED)
}

// Based on $262.evalScript
fn evaluate_source(vm: &Vm) -> ThrowCompletionOr<Value> {
    let realm = vm.current_realm().expect("evaluateSource runs in a realm");

    let source = vm.argument(0).to_utf16_string(vm)?;

    let script = match Script::parse(vm, &code_units_of(&source), realm) {
        Ok(script) => script,
        Err(errors) => return vm.throw_completion_with_message(ErrorKind::SyntaxError, errors[0].to_string()),
    };

    vm.run_script(script, None)
}

fn evaluate_module(vm: &Vm) -> ThrowCompletionOr<Value> {
    let realm = vm.current_realm().expect("evaluateModule runs in a realm");

    let path = Utf16View::of_string(&vm.argument(0).to_utf16_string(vm)?).to_wtf8();
    let module = match parse_module(vm, &String::from_utf8_lossy(&path), realm) {
        Ok(module) => module,
        Err(error) => return vm.throw_completion_with_message(ErrorKind::SyntaxError, error.error.to_string()),
    };

    vm.run_module(module)
}

#[allow(clippy::unnecessary_wraps, reason = "native functions return a completion")]
fn run_queued_promise_jobs(vm: &Vm) -> ThrowCompletionOr<Value> {
    vm.run_queued_promise_jobs();
    Ok(Value::UNDEFINED)
}

#[allow(clippy::unnecessary_wraps, reason = "native functions return a completion")]
fn clear_kept_objects(vm: &Vm) -> ThrowCompletionOr<Value> {
    // VM::finish_execution_generation()
    vm.head.execution_generation.set(vm.head.execution_generation.get() + 1);
    Ok(Value::UNDEFINED)
}

#[allow(clippy::unnecessary_wraps, reason = "native functions return a completion")]
fn run_queued_finalization_registry_cleanup_jobs(vm: &Vm) -> ThrowCompletionOr<Value> {
    vm.run_queued_finalization_registry_cleanup_jobs();
    Ok(Value::UNDEFINED)
}

fn get_weak_set_size(vm: &Vm) -> ThrowCompletionOr<Value> {
    let object = vm.argument(0).to_object(vm)?;
    let Some(weak_set) = object.downcast::<WeakSet>() else {
        return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAnObjectOfType, &[&"WeakSet"]);
    };
    Ok(Value::from_f64(weak_set.weak_set_size() as f64))
}

fn get_weak_map_size(vm: &Vm) -> ThrowCompletionOr<Value> {
    let object = vm.argument(0).to_object(vm)?;
    let Some(weak_map) = object.downcast::<WeakMap>() else {
        return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAnObjectOfType, &[&"WeakMap"]);
    };
    Ok(Value::from_f64(weak_map.weak_map_size() as f64))
}

fn mark_as_garbage(vm: &Vm) -> ThrowCompletionOr<Value> {
    let argument = vm.argument(0);
    if !argument.is_string() {
        return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAString, &[&argument]);
    }

    let variable_name = argument.as_string();

    // In native functions we don't have a lexical environment so get the outer via the execution stack.
    let mut outer_environment = None;
    vm.for_each_execution_context_top_to_bottom(|execution_context| {
        outer_environment = execution_context.lexical_environment.get();
        if outer_environment.is_some() {
            ControlFlow::Break(())
        } else {
            ControlFlow::Continue(())
        }
    });
    let Some(outer_environment) = outer_environment else {
        return vm.throw_completion(
            ErrorKind::ReferenceError,
            ErrorType::UnknownIdentifier,
            &[&variable_name.to_utf8()],
        );
    };

    let name: Utf16FlyString = variable_name.utf16_string_view().to_utf16_fly_string();
    let reference = vm.resolve_binding(&name, Strict::No, Some(outer_environment))?;

    let value = reference.get_value(vm)?;

    if !can_be_held_weakly(value) {
        let description = format!("Variable with name {}", variable_name.to_utf8());
        return vm.throw_completion(ErrorKind::TypeError, ErrorType::CannotBeHeldWeakly, &[&description]);
    }

    reference.put_value(vm, Value::UNDEFINED)?;
    reference.delete_(vm)?;
    vm.heap().uproot_cell(value.as_cell());

    Ok(Value::UNDEFINED)
}

fn cleanup_finalization_registry(vm: &Vm) -> ThrowCompletionOr<Value> {
    let Some(finalization_registry) = as_if::<FinalizationRegistry>(vm.argument(0)) else {
        return vm.throw_completion(
            ErrorKind::TypeError,
            ErrorType::NotAnObjectOfType,
            &[&"FinalizationRegistry"],
        );
    };

    let callback = vm.argument(1);
    if vm.argument_count() > 1 && !callback.is_function() {
        return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAFunction, &[&callback]);
    }

    let cleanup_callback = if callback.is_undefined() {
        None
    } else {
        Some(vm.host_make_job_callback()(vm, callback.as_function()))
    };

    finalization_registry.cleanup(vm, cleanup_callback)?;
    Ok(Value::UNDEFINED)
}

fn detach_array_buffer_function(vm: &Vm) -> ThrowCompletionOr<Value> {
    let Some(array_buffer) = as_if::<ArrayBuffer>(vm.argument(0)) else {
        return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAnObjectOfType, &[&"ArrayBuffer"]);
    };
    if array_buffer.is_shared_array_buffer() {
        return vm.throw_completion(ErrorKind::TypeError, ErrorType::SharedArrayBuffer, &[]);
    }

    detach_array_buffer(vm, array_buffer, Some(vm.argument(1)))?;
    Ok(Value::NULL)
}

/// Core::TimeZone::set_current_time_zone(): the time zone of LibUnicode, and the TZ environment variable of the
/// process.
fn set_current_time_zone(time_zone: Utf16View<'_>) -> Result<(), String> {
    if !unicode_time_zone::set_current_time_zone(time_zone) {
        return Err("Unable to find the provided time zone".to_string());
    }
    let mut time_zone_utf8 = time_zone.to_wtf8();
    // NB: Like the null-terminated string Core::Environment::set() passes on, this ends at the first NUL.
    if let Some(nul_index) = time_zone_utf8.iter().position(|&byte| byte == 0) {
        time_zone_utf8.truncate(nul_index);
    }
    let time_zone_utf8 = CString::new(time_zone_utf8).expect("the time zone has no NUL left");
    set_time_zone_environment_variable(&time_zone_utf8)
}

#[cfg(unix)]
fn set_time_zone_environment_variable(time_zone: &CStr) -> Result<(), String> {
    unsafe extern "C" {
        fn tzset();
    }

    // SAFETY: Both strings are NUL-terminated, and the runner has no other threads that read the environment.
    unsafe {
        if libc::setenv(c"TZ".as_ptr(), time_zone.as_ptr(), 1) != 0 {
            return Err(std::io::Error::last_os_error().to_string());
        }
        tzset();
    }
    Ok(())
}

/// Core::Environment::set() on Windows, which sets the variable in the environment of the C runtime, where _tzset()
/// reads it.
#[cfg(windows)]
fn set_time_zone_environment_variable(time_zone: &CStr) -> Result<(), String> {
    // SAFETY: Both strings are NUL-terminated, and the runner has no other threads that read the environment.
    unsafe {
        let error = libc::putenv_s(c"TZ".as_ptr(), time_zone.as_ptr());
        if error != 0 {
            return Err(format!("_putenv_s failed with error {error}"));
        }
        libc::tzset();
    }
    Ok(())
}

fn set_time_zone(vm: &Vm) -> ThrowCompletionOr<Value> {
    let current_time_zone = unicode_time_zone::current_time_zone();
    let current_time_zone_string = PrimitiveString::create(vm, current_time_zone);
    let time_zone = vm.argument(0).to_utf16_string(vm)?;

    if let Err(error) = set_current_time_zone(Utf16View::of_string(&time_zone)) {
        return vm.throw_completion_with_message(ErrorKind::InternalError, format!("Could not set time zone: {error}"));
    }

    clear_system_time_zone_cache();
    Ok(Value::from_string(current_time_zone_string))
}

fn to_utf8_bytes(vm: &Vm) -> ThrowCompletionOr<Value> {
    let realm = vm.current_realm().expect("toUTF8Bytes runs in a realm");

    let string = Utf16View::of_string(&vm.argument(0).to_utf16_string(vm)?).to_wtf8();
    let typed_array = Uint8Array::create(vm, realm, string.len() as u32)?;

    for (i, &byte) in string.iter().enumerate() {
        typed_array.set_value_in_buffer(vm, i, Value::from_i32(i32::from(byte)), Order::SeqCst);
    }

    Ok(Value::from_object(typed_array))
}

fn create_default_typed_array(vm: &Vm) -> ThrowCompletionOr<Value> {
    let realm = vm.current_realm().expect("createDefaultTypedArray runs in a realm");

    let typed_array = typed_array_from(vm, vm.argument(0))?;
    let length = vm.argument(1).to_index(vm)?;
    let Ok(length) = u32::try_from(length) else {
        return vm.throw_completion(ErrorKind::RangeError, ErrorType::InvalidLength, &[&"typed array"]);
    };

    Ok(Value::from_object(typed_array.create_default(vm, realm, length)?))
}

/// What the run file hook tells the runner to do with a file it does not run itself.
pub enum RunFileHookResult {
    RunAsNormal,
    SkipFile,
}

#[derive(Clone, Copy)]
enum Expectation {
    Early,
    Fail,
    Pass,
    ExplicitPass,
}

/// TESTJS_RUN_FILE_FUNCTION: with --test262-parser-tests, a file only has to parse, or fail to parse, as the
/// directory it is in says.
pub fn run_file(
    vm: &Vm,
    test262_parser_tests: bool,
    test_root: &str,
    test_file: &str,
    realm: Gc<Realm>,
) -> Result<JSFileResult, RunFileHookResult> {
    if !test262_parser_tests {
        return Err(RunFileHookResult::RunAsNormal);
    }

    let start_time = get_time_in_ms();

    let dirname = lexical_path_dirname(test_file);
    let expectation = if dirname.ends_with("early") {
        Expectation::Early
    } else if dirname.ends_with("fail") {
        Expectation::Fail
    } else if dirname.ends_with("pass-explicit") {
        Expectation::ExplicitPass
    } else if dirname.ends_with("pass") {
        Expectation::Pass
    } else {
        return Err(RunFileHookResult::SkipFile);
    };

    let is_module = test_file
        .rsplit('/')
        .next()
        .is_some_and(|basename| basename.ends_with(".module.js"));
    let parse_succeeded = if is_module {
        parse_module(vm, test_file, realm).is_ok()
    } else {
        parse_script(vm, test_file, realm).is_ok()
    };

    let (expectation_string, test_passed, message) = match expectation {
        Expectation::Early | Expectation::Fail => (
            "File should not parse",
            !parse_succeeded,
            "Expected the file to fail parsing, but it did not",
        ),
        Expectation::Pass | Expectation::ExplicitPass => (
            "File should parse",
            parse_succeeded,
            "Expected the file to parse, but it did not",
        ),
    };
    let message = if test_passed { "" } else { message };

    let test_result = if test_passed {
        TestResult::Pass
    } else {
        TestResult::Fail
    };
    let test_path = relative_path(test_file, test_root).expect("the test file is below the test root");
    let duration_ms = get_time_in_ms() - start_time;
    Ok(JSFileResult {
        name: test_path.clone(),
        error: None,
        time_taken: duration_ms,
        most_severe_test_result: test_result,
        suites: vec![Suite {
            path: test_path,
            name: b"Parse file".to_vec(),
            most_severe_test_result: test_result,
            tests: vec![Case {
                name: expectation_string.as_bytes().to_vec(),
                result: test_result,
                details: message.as_bytes().to_vec(),
                duration_us: (duration_ms as u64).wrapping_mul(1000),
            }],
        }],
        logged_messages: Vec::new(),
    })
}

/// LexicalPath::dirname() of an absolute path.
fn lexical_path_dirname(path: &str) -> &str {
    match path.rfind('/') {
        Some(0) => "/",
        Some(index) => &path[..index],
        None => ".",
    }
}
