/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What an interface implemented in Rust that is serializable or transferable does in a structured serialization,
//! https://html.spec.whatwg.org/multipage/structured-data.html#serializable-objects and
//! https://html.spec.whatwg.org/multipage/structured-data.html#transferable-objects
//!
//! StructuredSerialize of LibWeb's C++ runs the algorithms, and runs the steps of such an interface through the entry
//! points that the generated bindings of the interface define, which call the functions here. The steps read and
//! write the records of the serialization through what C++ lends them; the structs that it lends are in
//! Libraries/LibWeb/Bindings/RustSerialization.h. Unlike an object of an interface implemented in C++, which is
//! created before its deserialization steps fill it in, an object of an interface implemented in Rust is what its
//! deserialization steps and its transfer-receiving steps return.

use core::cell::Cell;
use core::ffi::c_void;
use core::marker::PhantomData;
use core::ptr::NonNull;

use ak::Utf16String;
use libjs_rust::api::{RawNativeFunctionResult, ThrowCompletionOr, Value};

use crate::bindings::interface::{Interface, allocate, cell_from_abi, realm_arguments, wrap};
use crate::bindings::{CallContext, WebIdlResult};

unsafe extern "C" {
    fn web_bindings_serialize_value(serialized: *mut c_void, value: u64) -> RawNativeFunctionResult;
    fn web_bindings_serialize_u32(serialized: *mut c_void, value: u32);
    fn web_bindings_serialize_f64(serialized: *mut c_void, value: f64);
    fn web_bindings_serialize_bool(serialized: *mut c_void, value: bool);
    fn web_bindings_serialize_i64(serialized: *mut c_void, value: i64);
    fn web_bindings_serialize_utf16_string(serialized: *mut c_void, string: usize);
    fn web_bindings_serialize_bytes(serialized: *mut c_void, bytes: ak::Span<'_, u8>);
    fn web_bindings_deserialize_value(serialized: *mut c_void) -> RawNativeFunctionResult;
    fn web_bindings_deserialize_u32(serialized: *mut c_void, out_value: &mut u32) -> RawNativeFunctionResult;
    fn web_bindings_deserialize_f64(serialized: *mut c_void, out_value: &mut f64) -> RawNativeFunctionResult;
    fn web_bindings_deserialize_bool(serialized: *mut c_void, out_value: &mut bool) -> RawNativeFunctionResult;
    fn web_bindings_deserialize_i64(serialized: *mut c_void, out_value: &mut i64) -> RawNativeFunctionResult;
    fn web_bindings_deserialize_utf16_string(
        serialized: *mut c_void,
        out_string: &mut usize,
    ) -> RawNativeFunctionResult;
    fn web_bindings_serialize_inline_object_header(serialized: *mut c_void, interface_name: u16);
    fn web_bindings_deserialize_inline_object_header(
        serialized: *mut c_void,
        interface_name: u16,
    ) -> RawNativeFunctionResult;
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

    pub(crate) fn write_f64(&mut self, value: f64) {
        // SAFETY: The writer is lent for the steps.
        unsafe { web_bindings_serialize_f64(self.raw.as_ptr(), value) };
    }

    pub(crate) fn write_bool(&mut self, value: bool) {
        // SAFETY: The writer is lent for the steps.
        unsafe { web_bindings_serialize_bool(self.raw.as_ptr(), value) };
    }

    pub(crate) fn write_i64(&mut self, value: i64) {
        // SAFETY: The writer is lent for the steps.
        unsafe { web_bindings_serialize_i64(self.raw.as_ptr(), value) };
    }

    pub(crate) fn write_utf16_string(&mut self, string: &Utf16String) {
        // SAFETY: The writer is lent for the steps, and the call borrows the string.
        unsafe { web_bindings_serialize_utf16_string(self.raw.as_ptr(), string.raw_identity()) };
    }

    pub(crate) fn write_bytes(&mut self, bytes: &[u8]) {
        // SAFETY: The writer is lent for the steps, and the call borrows the bytes.
        unsafe { web_bindings_serialize_bytes(self.raw.as_ptr(), ak::Span::new(bytes)) };
    }

    /// Writes the record of a serializable object in place, as StructuredSerializeInternal writes it, but without
    /// giving the object a position in the memory of the serialization, which a format that predates the
    /// sub-serialization of the object has none for.
    pub(crate) fn write_object_in_place<T: Serializable>(
        &mut self,
        cx: &mut CallContext,
        object: &T,
        for_storage: bool,
    ) -> WebIdlResult<()> {
        // SAFETY: The writer is lent for the steps.
        unsafe { web_bindings_serialize_inline_object_header(self.raw.as_ptr(), T::INTERFACE_NAME) };
        object.serialization_steps(cx, self, for_storage)
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

    pub(crate) fn read_f64(&mut self) -> WebIdlResult<f64> {
        let mut value = 0.0;
        // SAFETY: The reader is lent for the steps.
        unsafe { web_bindings_deserialize_f64(self.raw.as_ptr(), &mut value) }.into_completion()?;
        Ok(value)
    }

    /// Reads what SerializationWriter::write_object_in_place() writes.
    pub(crate) fn read_object_in_place<T: Serializable>(&mut self, cx: &mut CallContext) -> WebIdlResult<T> {
        // SAFETY: The reader is lent for the steps.
        unsafe { web_bindings_deserialize_inline_object_header(self.raw.as_ptr(), T::INTERFACE_NAME) }
            .into_completion()?;
        T::deserialization_steps(cx, self)
    }

    pub(crate) fn read_bool(&mut self) -> WebIdlResult<bool> {
        let mut value = false;
        // SAFETY: The reader is lent for the steps.
        unsafe { web_bindings_deserialize_bool(self.raw.as_ptr(), &mut value) }.into_completion()?;
        Ok(value)
    }

    pub(crate) fn read_i64(&mut self) -> WebIdlResult<i64> {
        let mut value = 0;
        // SAFETY: The reader is lent for the steps.
        unsafe { web_bindings_deserialize_i64(self.raw.as_ptr(), &mut value) }.into_completion()?;
        Ok(value)
    }

    pub(crate) fn read_utf16_string(&mut self) -> WebIdlResult<Utf16String> {
        let mut string = 0;
        // SAFETY: The reader is lent for the steps.
        unsafe { web_bindings_deserialize_utf16_string(self.raw.as_ptr(), &mut string) }.into_completion()?;
        // SAFETY: LibWeb hands over the string it read.
        Ok(unsafe { Utf16String::from_raw_owned(string) })
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

/// The data holder of a transferred object, which its transfer steps write to: a Web::Bindings::RustTransferDataWriter
/// that C++ lends for the steps.
pub(crate) struct TransferDataWriter<'a> {
    raw: NonNull<c_void>,
    _lent: PhantomData<&'a mut c_void>,
}

impl TransferDataWriter<'_> {
    /// The data holder as the C ABI of an interface's C++ class takes it, which encodes what the steps write.
    pub(crate) fn as_raw(&mut self) -> *mut c_void {
        self.raw.as_ptr()
    }
}

/// The data holder of a received object, which its transfer-receiving steps read from: a
/// Web::Bindings::RustTransferDataReader that C++ lends for the steps.
pub(crate) struct TransferDataReader<'a> {
    raw: NonNull<c_void>,
    _lent: PhantomData<&'a mut c_void>,
}

impl TransferDataReader<'_> {
    /// The data holder as the C ABI of an interface's C++ class takes it, which decodes what the steps read.
    pub(crate) fn as_raw(&mut self) -> *mut c_void {
        self.raw.as_ptr()
    }
}

/// An interface that is transferable, [Transferable] in its IDL.
pub(crate) trait Transferable: Interface {
    /// The [[Detached]] internal slot of the object, https://html.spec.whatwg.org/multipage/structured-data.html#detached
    fn detached(&self) -> &Cell<bool>;

    /// https://html.spec.whatwg.org/multipage/structured-data.html#transfer-steps
    fn transfer_steps(&self, cx: &mut CallContext, data_holder: &mut TransferDataWriter) -> WebIdlResult<()>;

    /// https://html.spec.whatwg.org/multipage/structured-data.html#transfer-receiving-steps, which create the object.
    fn transfer_receiving_steps(cx: &mut CallContext, data_holder: &mut TransferDataReader) -> WebIdlResult<Self>;
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

/// The entry point that reads the [[Detached]] internal slot of an object of `T`.
///
/// # Safety
///
/// `cell` must be the live cell of an object of `T`.
pub(crate) unsafe fn is_detached<T: Transferable>(cell: *mut c_void) -> bool {
    // SAFETY: The caller passes a live cell of an object of T.
    let object = unsafe { cell_from_abi::<T>(cell) };
    object.detached().get()
}

/// The entry point that sets the [[Detached]] internal slot of an object of `T` to true.
///
/// # Safety
///
/// `cell` must be the live cell of an object of `T`.
pub(crate) unsafe fn set_detached<T: Transferable>(cell: *mut c_void) {
    // SAFETY: The caller passes a live cell of an object of T.
    let object = unsafe { cell_from_abi::<T>(cell) };
    object.detached().set(true);
}

/// The entry point of the transfer steps of `T`.
///
/// # Safety
///
/// The pointers must be the VM, a live realm, the live cell of an object of `T`, and a data holder that C++ lends.
pub(crate) unsafe fn transfer_steps<T: Transferable>(
    vm: *mut c_void,
    realm: *mut c_void,
    cell: *mut c_void,
    data_holder: *mut c_void,
) -> RawNativeFunctionResult {
    // SAFETY: The caller passes the VM, a live realm and a live cell of an object of T.
    let ((vm, realm), object) = unsafe { (realm_arguments(vm, realm), cell_from_abi::<T>(cell)) };
    let mut cx = CallContext::new(vm, realm);
    let mut data_holder = TransferDataWriter {
        raw: NonNull::new(data_holder).expect("C++ lends a data holder"),
        _lent: PhantomData,
    };
    let result = object.transfer_steps(&mut cx, &mut data_holder);
    completion(&cx, result.map(|()| Value::UNDEFINED))
}

/// The entry point of the transfer-receiving steps of `T`, which returns the wrapper of the new object.
///
/// # Safety
///
/// The pointers must be the VM, a live realm, and a data holder that C++ lends.
pub(crate) unsafe fn transfer_receiving_steps<T: Transferable>(
    vm: *mut c_void,
    realm: *mut c_void,
    data_holder: *mut c_void,
) -> RawNativeFunctionResult {
    // SAFETY: The caller passes the VM and a live realm.
    let (vm, realm) = unsafe { realm_arguments(vm, realm) };
    let mut cx = CallContext::new(vm, realm);
    let mut data_holder = TransferDataReader {
        raw: NonNull::new(data_holder).expect("C++ lends a data holder"),
        _lent: PhantomData,
    };
    let result = T::transfer_receiving_steps(&mut cx, &mut data_holder);
    completion(&cx, result.map(|implementation| wrap_new_object(&cx, implementation)))
}
