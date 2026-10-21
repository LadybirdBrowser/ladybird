/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What an interface implemented in Rust that is serializable does in a structured serialization,
//! https://html.spec.whatwg.org/multipage/structured-data.html#serializable-objects
//!
//! StructuredSerialize of LibWeb's C++ runs the algorithms, and runs the steps of such an interface through the entry
//! points that the generated bindings of the interface define, which call the functions here. The steps read and
//! write the records of the serialization through what C++ lends them; the structs that it lends are in
//! Libraries/LibWeb/Bindings/RustSerialization.h. Unlike an object of an interface implemented in C++, which is
//! created before its deserialization steps fill it in, an object of an interface implemented in Rust is what its
//! deserialization steps return.

use core::ffi::c_void;
use core::marker::PhantomData;
use core::ptr::NonNull;

use libjs_rust::api::{RawNativeFunctionResult, ThrowCompletionOr, Value};

use crate::bindings::interface::{Interface, allocate, cell_from_abi, realm_arguments, wrap};
use crate::bindings::{CallContext, WebIdlResult};

unsafe extern "C" {
    fn web_bindings_serialize_value(serialized: *mut c_void, value: u64) -> RawNativeFunctionResult;
    fn web_bindings_serialize_u32(serialized: *mut c_void, value: u32);
    fn web_bindings_deserialize_value(serialized: *mut c_void) -> RawNativeFunctionResult;
    fn web_bindings_deserialize_u32(serialized: *mut c_void, out_value: &mut u32) -> RawNativeFunctionResult;
}

/// The record of a structured serialization, which the serialization steps of an object write to: a
/// Web::Bindings::RustSerializationWriter that C++ lends for the steps.
pub(crate) struct SerializationWriter<'a> {
    raw: NonNull<c_void>,
    _lent: PhantomData<&'a mut c_void>,
}

impl SerializationWriter<'_> {
    /// The writer as the C ABI of an interface's C++ class takes it, for what only C++ encodes.
    pub(crate) fn as_raw(&mut self) -> *mut c_void {
        self.raw.as_ptr()
    }

    /// Sets a field of the serialized form to the sub-serialization of the value,
    /// https://html.spec.whatwg.org/multipage/structured-data.html#sub-serialization
    pub(crate) fn write_value(&mut self, value: Value) -> WebIdlResult<()> {
        // SAFETY: The writer is lent for the steps, and the value is live while they run.
        unsafe { web_bindings_serialize_value(self.raw.as_ptr(), value.0) }.into_completion()?;
        Ok(())
    }

    pub(crate) fn write_u32(&mut self, value: u32) {
        // SAFETY: The writer is lent for the steps.
        unsafe { web_bindings_serialize_u32(self.raw.as_ptr(), value) };
    }
}

/// The record of a structured deserialization, which the deserialization steps of an object read from: a
/// Web::Bindings::RustSerializationReader that C++ lends for the steps. What it reads that the record does not hold
/// throws a "DataCloneError" DOMException.
pub(crate) struct SerializationReader<'a> {
    raw: NonNull<c_void>,
    _lent: PhantomData<&'a mut c_void>,
}

impl SerializationReader<'_> {
    /// The reader as the C ABI of an interface's C++ class takes it, for what only C++ decodes.
    pub(crate) fn as_raw(&mut self) -> *mut c_void {
        self.raw.as_ptr()
    }

    /// The value of a field of the serialized form, which is a sub-serialization,
    /// https://html.spec.whatwg.org/multipage/structured-data.html#sub-deserialization
    pub(crate) fn read_value(&mut self) -> WebIdlResult<Value> {
        // SAFETY: The reader is lent for the steps.
        Ok(unsafe { web_bindings_deserialize_value(self.raw.as_ptr()) }.into_completion()?)
    }

    pub(crate) fn read_u32(&mut self) -> WebIdlResult<u32> {
        let mut value = 0;
        // SAFETY: The reader is lent for the steps.
        unsafe { web_bindings_deserialize_u32(self.raw.as_ptr(), &mut value) }.into_completion()?;
        Ok(value)
    }
}

/// An interface that is serializable, [Serializable] in its IDL.
pub(crate) trait Serializable: Interface {
    /// https://html.spec.whatwg.org/multipage/structured-data.html#serialization-steps
    fn serialization_steps(
        &self,
        cx: &mut CallContext,
        serialized: &mut SerializationWriter,
        for_storage: bool,
    ) -> WebIdlResult<()>;

    /// https://html.spec.whatwg.org/multipage/structured-data.html#deserialization-steps, which create the object.
    fn deserialization_steps(cx: &mut CallContext, serialized: &mut SerializationReader) -> WebIdlResult<Self>;
}

/// The completion of steps, as the entry points return it to C++.
fn completion(cx: &CallContext, result: WebIdlResult<Value>) -> RawNativeFunctionResult {
    let result: ThrowCompletionOr<Value> = result.map_err(|exception| exception.throw(cx));
    RawNativeFunctionResult::from_completion(result)
}

/// The wrapper in the realm of the call context of the object that steps created.
fn wrap_new_object<T: Interface>(cx: &CallContext, implementation: T) -> Value {
    Value::from_object(wrap(cx, allocate(cx.vm(), implementation)))
}

/// The entry point of the serialization steps of `T`.
///
/// # Safety
///
/// The pointers must be the VM, a live realm, the live cell of an object of `T`, and a writer that C++ lends.
pub(crate) unsafe fn serialization_steps<T: Serializable>(
    vm: *mut c_void,
    realm: *mut c_void,
    cell: *mut c_void,
    serialized: *mut c_void,
    for_storage: bool,
) -> RawNativeFunctionResult {
    // SAFETY: The caller passes the VM, a live realm and a live cell of an object of T.
    let ((vm, realm), object) = unsafe { (realm_arguments(vm, realm), cell_from_abi::<T>(cell)) };
    let mut cx = CallContext::new(vm, realm);
    let mut serialized = SerializationWriter {
        raw: NonNull::new(serialized).expect("C++ lends a writer"),
        _lent: PhantomData,
    };
    let result = object.serialization_steps(&mut cx, &mut serialized, for_storage);
    completion(&cx, result.map(|()| Value::UNDEFINED))
}

/// The entry point of the deserialization steps of `T`, which returns the wrapper of the new object.
///
/// # Safety
///
/// The pointers must be the VM, a live realm, and a reader that C++ lends.
pub(crate) unsafe fn deserialization_steps<T: Serializable>(
    vm: *mut c_void,
    realm: *mut c_void,
    serialized: *mut c_void,
) -> RawNativeFunctionResult {
    // SAFETY: The caller passes the VM and a live realm.
    let (vm, realm) = unsafe { realm_arguments(vm, realm) };
    let mut cx = CallContext::new(vm, realm);
    let mut serialized = SerializationReader {
        raw: NonNull::new(serialized).expect("C++ lends a reader"),
        _lent: PhantomData,
    };
    let result = T::deserialization_steps(&mut cx, &mut serialized);
    completion(&cx, result.map(|implementation| wrap_new_object(&cx, implementation)))
}
