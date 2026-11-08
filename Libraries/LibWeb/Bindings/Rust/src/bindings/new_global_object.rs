/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::ffi::c_void;

use ak::Utf16String;
use libjs_rust::api::{ErrorKind, Gc, JSONObject, Must, RawNativeFunctionResult, Realm, ThrowCompletionOr, Value, Vm};

unsafe extern "C" {
    fn web_bindings_throw_copy_of_error_in_current_realm(vm: *const c_void, error: u64) -> RawNativeFunctionResult;
}

pub(crate) fn run_in_a_new_global_object<T>(
    vm: &Vm,
    steps: impl FnOnce(Gc<Realm>) -> ThrowCompletionOr<T>,
) -> ThrowCompletionOr<T> {
    let execution_context = Realm::initialize_host_defined_realm(vm, None, None).must();
    let realm = vm.current_realm().expect("the new execution context has a realm");
    let result = steps(realm);
    assert_eq!(vm.pop_execution_context(), execution_context.as_non_null());

    // Recreate any thrown error in the caller's realm before it reaches script.
    result.map_err(|error| {
        // SAFETY: The VM and error are live.
        unsafe {
            web_bindings_throw_copy_of_error_in_current_realm(core::ptr::from_ref(vm).cast(), error.value().to_raw())
        }
        .into_completion()
        .expect_err("throwing a copy of the error returns a throw completion")
    })
}

pub(crate) fn serialize_to_json_in_a_new_global_object(
    vm: &Vm,
    create_value: impl FnOnce(Gc<Realm>) -> ThrowCompletionOr<Value>,
) -> ThrowCompletionOr<Utf16String> {
    let json = run_in_a_new_global_object(vm, |realm| {
        JSONObject::stringify_impl(vm, create_value(realm)?, Value::UNDEFINED, Value::UNDEFINED)
    })?;
    let Some(json) = json else {
        return vm.throw_completion_with_message(
            ErrorKind::TypeError,
            "Result of stringifying value must not be undefined".into(),
        );
    };
    Ok(json)
}
