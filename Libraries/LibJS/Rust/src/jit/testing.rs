/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The `jit` object that test-js (and js with --expose-jit-testing) define on the global object, so that tests can
//! compile functions at the points they choose and check that compiled code ran, exited or was discarded:
//!
//! - `jit.enabled`: whether the JIT is on (LIBJS_JIT=on). Without it, functions are never compiled and every function
//!   below answers as if the JIT never got to them.
//! - `jit.forcesExits`: whether compiled code exits at random checks (LIBJS_JIT=stress-exits), so that it may run in
//!   the interpreter at any point. These exits are not counted.
//! - `jit.prepare(f)`: the interpreter collects feedback for `f` from now on, and `f` is only compiled by
//!   `jit.compile(f)`, never on its own.
//! - `jit.feedback(f)`: the feedback the interpreter collected for `f` (see `feedback_dump::describe_feedback()`), or
//!   undefined if it collected none.
//! - `jit.compile(f)`: compiles `f` right away from the feedback it has. Returns whether `f` has code afterwards.
//! - `jit.isCompiled(f)`: whether new calls of `f` run in compiled code.
//! - `jit.discard(f)`: drops the code of `f`; it may be compiled again.
//! - `jit.neverCompile(f)`: `f` runs in the interpreter for good.
//! - `jit.exitCount(f)`: how often the current code of `f` exited to the interpreter.
//! - `jit.exitSites(f)`: the places where compiled code exited while running `f` (also inlined into other functions),
//!   as strings like "Overflow@24" (the exit kind and the bytecode offset), which recompiles no longer speculate at.
//! - `jit.discardCount(f)`: how often the code of `f` was discarded.
//! - `jit.inCompiledCode()`: whether the function calling it runs in compiled code as the compiled function itself
//!   (calls from callees inlined into compiled code answer false).

use super::code::CompileState;
use super::feedback_dump::describe_feedback;
use super::tier_up;
use crate::interpreter::vm::Vm;
use crate::jit::InterpreterTier;
use crate::layout::cell::Gc;
use crate::layout::function_object::EcmascriptFunctionObject;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::layout_forward::RawNativeFunctionPointer;
use crate::runtime::array::Array;
use crate::runtime::completion::ThrowCompletionOr;
use crate::runtime::ecmascript_function_object::as_ecmascript_function_object;
use crate::runtime::error::ErrorKind;
use crate::runtime::native_function::raw_native;
use crate::runtime::primitive_string::PrimitiveString;
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::property_key::PropertyKey;
use crate::runtime::realm::Realm;
use ak::Utf16FlyString;

const FUNCTIONS: &[(&str, RawNativeFunctionPointer, i32)] = &[
    ("prepare", raw_native!(prepare), 1),
    ("feedback", raw_native!(feedback), 1),
    ("compile", raw_native!(compile), 1),
    ("isCompiled", raw_native!(is_compiled), 1),
    ("discard", raw_native!(discard), 1),
    ("neverCompile", raw_native!(never_compile), 1),
    ("exitCount", raw_native!(exit_count), 1),
    ("exitSites", raw_native!(exit_sites), 1),
    ("discardCount", raw_native!(discard_count), 1),
    ("inCompiledCode", raw_native!(in_compiled_code), 0),
];

fn key(name: &str) -> PropertyKey {
    PropertyKey::from(Utf16FlyString::from_utf8(name))
}

/// Defines the `jit` object on `global`.
pub fn define_jit_testing_object(vm: &Vm, realm: Gc<Realm>, global: &Object) {
    let jit = Object::create(vm, realm, Some(realm.intrinsics().object_prototype(vm)));
    let attributes = PropertyAttributes::new(Attribute::CONFIGURABLE | Attribute::WRITABLE);
    jit.define_direct_property(
        vm,
        &key("enabled"),
        Value::from_bool(vm.jit.options.enabled),
        attributes,
    );
    jit.define_direct_property(
        vm,
        &key("forcesExits"),
        Value::from_bool(vm.jit.options.stress_exits != 0),
        attributes,
    );
    for (name, function, length) in FUNCTIONS {
        jit.define_native_function(vm, realm, &key(name), *function, *length, attributes, None);
    }
    global.define_direct_property(vm, &key("jit"), Value::from_object(jit), attributes);
}

/// The ECMAScript function in argument 0.
fn function_argument(vm: &Vm) -> ThrowCompletionOr<Gc<EcmascriptFunctionObject>> {
    let argument = vm.argument(0);
    if argument.is_object()
        && let Some(function) = as_ecmascript_function_object(argument.as_object())
    {
        return Ok(function);
    }
    vm.throw_completion_with_message(ErrorKind::TypeError, "Not an ECMAScript function".into())
}

fn prepare(vm: &Vm) -> ThrowCompletionOr<Value> {
    let function = function_argument(vm)?;
    if !vm.jit.options.collects_feedback() {
        return Ok(Value::UNDEFINED);
    }
    let executable = function.compiled_executable(vm);
    if executable.jit_compile_state() == CompileState::Refused {
        return Ok(Value::UNDEFINED);
    }
    executable.set_interpreter_tier(InterpreterTier::Profiling);
    executable.head.tier_up_budget.set(i32::MAX);
    Ok(Value::UNDEFINED)
}

fn feedback(vm: &Vm) -> ThrowCompletionOr<Value> {
    let function = function_argument(vm)?;
    let description = function
        .bytecode_executable()
        .and_then(|executable| describe_feedback(&executable));
    Ok(description.map_or(Value::UNDEFINED, |description| {
        Value::from_string(PrimitiveString::create_from_utf8(vm, &description))
    }))
}

fn compile(vm: &Vm) -> ThrowCompletionOr<Value> {
    let function = function_argument(vm)?;
    Ok(Value::from_bool(tier_up::compile_now(vm, function)))
}

fn is_compiled(vm: &Vm) -> ThrowCompletionOr<Value> {
    let function = function_argument(vm)?;
    let compiled = function
        .bytecode_executable()
        .is_some_and(|executable| executable.jit_compile_state() == CompileState::Installed);
    Ok(Value::from_bool(compiled))
}

fn discard(vm: &Vm) -> ThrowCompletionOr<Value> {
    let function = function_argument(vm)?;
    if let Some(executable) = function.bytecode_executable()
        && executable.jit_compile_state() == CompileState::Installed
    {
        executable.discard_jit_code(vm);
    }
    Ok(Value::UNDEFINED)
}

fn never_compile(vm: &Vm) -> ThrowCompletionOr<Value> {
    let function = function_argument(vm)?;
    if !vm.jit.options.collects_feedback() {
        return Ok(Value::UNDEFINED);
    }
    let executable = function.compiled_executable(vm);
    tier_up::wait_for_compile_job(vm, &executable);
    if executable.jit_compile_state() == CompileState::Installed {
        executable.discard_jit_code(vm);
    }
    tier_up::refuse_jit_compile(vm, &executable);
    Ok(Value::UNDEFINED)
}

fn exit_count(vm: &Vm) -> ThrowCompletionOr<Value> {
    let function = function_argument(vm)?;
    let count = function
        .bytecode_executable()
        .and_then(|executable| executable.jit_code().map(super::code::JitCode::exit_count))
        .unwrap_or(0);
    Ok(Value::from_i32(i32::try_from(count).unwrap_or(i32::MAX)))
}

fn exit_sites(vm: &Vm) -> ThrowCompletionOr<Value> {
    let function = function_argument(vm)?;
    let realm = vm.current_realm().expect("jit.exitSites runs in a realm");
    let sites: Vec<Value> = function
        .bytecode_executable()
        .map(|executable| executable.jit_exit_sites())
        .unwrap_or_default()
        .into_iter()
        .map(|(pc, kind)| Value::from_string(PrimitiveString::create_from_utf8(vm, &format!("{kind:?}@{pc}"))))
        .collect();
    Ok(Value::from_object(Array::create_from(vm, realm, &sites)))
}

fn discard_count(vm: &Vm) -> ThrowCompletionOr<Value> {
    let function = function_argument(vm)?;
    let count = function
        .bytecode_executable()
        .map_or(0, |executable| executable.jit_discard_count());
    Ok(Value::from_i32(i32::try_from(count).unwrap_or(i32::MAX)))
}

#[allow(clippy::unnecessary_wraps, reason = "native functions return a completion")]
fn in_compiled_code(vm: &Vm) -> ThrowCompletionOr<Value> {
    // NB: The nearest frame that runs bytecode is the caller's; frames of native functions run none.
    let mut in_compiled_code = false;
    vm.for_each_live_execution_context(|frame| {
        if frame.executable.get().is_none() {
            return core::ops::ControlFlow::Continue(());
        }
        in_compiled_code = frame.runs_jit_code.get();
        core::ops::ControlFlow::Break(())
    });
    Ok(Value::from_bool(in_compiled_code))
}
