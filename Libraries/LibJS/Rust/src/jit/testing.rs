/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The `jit` object that test-js defines on the global object, so that tests can choose which functions the
//! interpreter profiles:
//!
//! - `jit.prepare(f)`: the interpreter runs `f` with the profiling handlers from now on, if it collects feedback
//!   (LIBJS_JIT=on). Without it, `f` stays on the plain handlers.

use crate::interpreter::vm::Vm;
use crate::jit::InterpreterTier;
use crate::layout::cell::Gc;
use crate::layout::function_object::EcmascriptFunctionObject;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::layout_forward::RawNativeFunctionPointer;
use crate::runtime::completion::ThrowCompletionOr;
use crate::runtime::ecmascript_function_object::as_ecmascript_function_object;
use crate::runtime::error::ErrorKind;
use crate::runtime::native_function::raw_native;
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::property_key::PropertyKey;
use crate::runtime::realm::Realm;
use ak::Utf16FlyString;

const FUNCTIONS: &[(&str, RawNativeFunctionPointer, i32)] = &[("prepare", raw_native!(prepare), 1)];

fn key(name: &str) -> PropertyKey {
    PropertyKey::from(Utf16FlyString::from_utf8(name))
}

/// Defines the `jit` object on `global`.
pub fn define_jit_testing_object(vm: &Vm, realm: Gc<Realm>, global: &Object) {
    let jit = Object::create(vm, realm, Some(realm.intrinsics().object_prototype(vm)));
    let attributes = PropertyAttributes::new(Attribute::CONFIGURABLE | Attribute::WRITABLE);
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
    if !vm.jit.collects_feedback() {
        return Ok(Value::UNDEFINED);
    }
    let executable = function.compiled_executable(vm);
    executable.set_interpreter_tier(InterpreterTier::Profiling);
    // NB: A prepared function stays in the profiling tier.
    executable.head.tier_up_budget.set(i32::MAX);
    Ok(Value::UNDEFINED)
}
