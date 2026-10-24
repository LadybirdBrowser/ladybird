/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Callback function values, https://webidl.spec.whatwg.org/#idl-callback-function, which LibWeb's C++ code
//! represents as a WebIDL::CallbackType: the function object and its callback context.

use core::ffi::c_void;
use core::ptr::NonNull;

use libjs_rust::api::{
    ErrorKind, ErrorType, ForeignCellSlot, RawNativeFunctionResult, ThrowCompletionOr, Trace, Value, Vm,
};

use crate::bindings::promise::{WebIdlPromise, promise_from_abi};

unsafe extern "C" {
    /// A new WebIDL::CallbackType of the callable object, whose callback context is the incumbent settings object.
    fn web_bindings_create_callback_function(callable: *mut c_void, returns_promise: bool) -> *mut c_void;

    /// WebIDL::invoke_callback() of the WebIDL::CallbackType with the this value, if `has_this_value`, and the
    /// arguments.
    fn web_bindings_invoke_callback_function(
        callback: *mut c_void,
        has_this_value: bool,
        this_value: u64,
        arguments: ak::Span<'_, Value>,
    ) -> RawNativeFunctionResult;

    /// WebIDL::invoke_promise_callback() of the WebIDL::CallbackType, whose return type is a promise type, with the this
    /// value, if `has_this_value`, and the arguments.
    fn web_bindings_invoke_promise_callback_function(
        callback: *mut c_void,
        has_this_value: bool,
        this_value: u64,
        arguments: ak::Span<'_, Value>,
    ) -> *mut c_void;
}

/// A callback function value, https://webidl.spec.whatwg.org/#idl-callback-function
#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct CallbackFunction(CallbackType);

/// A callback function value whose return type is a promise type.
#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct PromiseCallbackFunction(CallbackType);

/// The WebIDL::CallbackType of a callback function value.
#[derive(Trace)]
#[gc(crate = libjs_rust)]
struct CallbackType(ForeignCellSlot);

impl Clone for CallbackType {
    fn clone(&self) -> Self {
        // SAFETY: The slot keeps the callback alive.
        unsafe { Self::from_raw(self.0.as_ptr()) }
    }
}

impl CallbackType {
    /// # Safety
    ///
    /// `callback` must be a live WebIDL::CallbackType.
    unsafe fn from_raw(callback: *mut c_void) -> Self {
        let slot = ForeignCellSlot::empty();
        // SAFETY: The caller passes a live WebIDL::CallbackType, a cell of the heap.
        unsafe { slot.set(Some(NonNull::new(callback).expect("LibWeb passes a callback"))) };
        Self(slot)
    }

    // https://webidl.spec.whatwg.org/#js-callback-function
    fn from_js(vm: &Vm, value: Value, returns_promise: bool) -> ThrowCompletionOr<Self> {
        // 1. If the result of calling IsCallable(V) is false and the conversion to an IDL value is not being performed
        //    due to V being assigned to an attribute whose type is a nullable callback function that is annotated with
        //    [LegacyTreatNonObjectAsNull], then throw a TypeError.
        if !value.is_function() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAFunction, &[&value]);
        }

        // 2. Return the IDL callback function type value that represents a reference to the same object that V
        //    represents, with the incumbent realm as the callback context.
        // SAFETY: The value is a live object.
        Ok(unsafe {
            Self::from_raw(web_bindings_create_callback_function(
                value.as_object().as_ptr().cast(),
                returns_promise,
            ))
        })
    }
}

impl CallbackFunction {
    /// The callback function value of a JS value, https://webidl.spec.whatwg.org/#js-callback-function
    pub(crate) fn from_js(vm: &Vm, value: Value) -> ThrowCompletionOr<Self> {
        CallbackType::from_js(vm, value, false).map(Self)
    }

    /// Invokes the callback function with the this value and the arguments, https://webidl.spec.whatwg.org/#invoke-a-callback-function
    pub(crate) fn invoke(&self, this_value: Option<Value>, arguments: &[Value]) -> ThrowCompletionOr<Value> {
        // SAFETY: The slot keeps the callback alive, and the values are live for the call.
        unsafe {
            web_bindings_invoke_callback_function(
                self.0.0.as_ptr(),
                this_value.is_some(),
                this_value.unwrap_or(Value::UNDEFINED).0,
                ak::Span::new(arguments),
            )
        }
        .into_completion()
    }
}

impl PromiseCallbackFunction {
    /// The callback function value of a JS value, https://webidl.spec.whatwg.org/#js-callback-function
    pub(crate) fn from_js(vm: &Vm, value: Value) -> ThrowCompletionOr<Self> {
        CallbackType::from_js(vm, value, true).map(Self)
    }

    /// Invokes the callback function with the this value and the arguments, https://webidl.spec.whatwg.org/#invoke-a-callback-function:
    /// the promise that it returns, or one rejected with what it throws.
    pub(crate) fn invoke(&self, this_value: Option<Value>, arguments: &[Value]) -> WebIdlPromise {
        // SAFETY: The slot keeps the callback alive, and the values are live for the call.
        //         LibWeb returns a live promise.
        unsafe {
            promise_from_abi(web_bindings_invoke_promise_callback_function(
                self.0.0.as_ptr(),
                this_value.is_some(),
                this_value.unwrap_or(Value::UNDEFINED).0,
                ak::Span::new(arguments),
            ))
        }
    }
}
