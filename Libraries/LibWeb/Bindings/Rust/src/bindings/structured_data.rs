/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What an interface implemented in Rust that is serializable or transferable does in a structured serialization,
//! https://html.spec.whatwg.org/multipage/structured-data.html#serializable-objects and
//! https://html.spec.whatwg.org/multipage/structured-data.html#transferable-objects
//!
//! The algorithms of crate::html::structured_serialize run the steps of such an interface through LibWeb's C++, which
//! dispatches to the entry points that the generated bindings of the interface define, which call the functions here.
//! The serialization and deserialization steps write and read the record of the algorithm, which it passes through C++
//! to them. The transfer steps and the transfer-receiving steps read and write what C++ lends them; the structs that it
//! lends are in Libraries/LibWeb/Bindings/RustSerialization.h. Unlike an object of an interface implemented in C++,
//! which is created before its deserialization steps fill it in, an object of an interface implemented in Rust is what
//! its deserialization steps and its transfer-receiving steps return.

use core::cell::Cell;
use core::ffi::c_void;
use core::marker::PhantomData;
use core::ptr::NonNull;

use ak::Utf16String;
use libjs_rust::api::{RawNativeFunctionResult, ThrowCompletionOr, Value};
use libwebcommon_rust::html::serialization_records::SerializationType;

use crate::bindings::interface::{ConstructedObject, Interface, Ref, cell_from_abi, realm_arguments, wrap};
use crate::bindings::transport::TransportHandle;
use crate::bindings::{CallContext, WebIdlResult};
use crate::html::structured_serialize::cpp::{
    OwnedSerializedTransferRecord, TransferDataDecoder, data_clone_error_from_serialization_error,
};
use crate::html::structured_serialize::reader::StructuredSerializeReader;
use crate::html::structured_serialize::writer::StructuredSerializeWriter;
use crate::html::structured_serialize::{
    Deserializer, Serializer, decode_serializable_object_header, encode_serializable_object_header,
    structured_deserialize_with_transfer_internal, structured_serialize_with_transfer,
};

unsafe extern "C" {
    fn web_bindings_transfer_data_encode_bool(data_holder: *mut c_void, value: bool);
    fn web_bindings_transfer_data_encode_records(data_holder: *mut c_void, records: *const *mut c_void, count: usize);
    fn web_bindings_transfer_data_encode_transport_handle(data_holder: *mut c_void, handle: *mut c_void);
    fn web_bindings_transfer_data_extend(data_holder: *mut c_void, record: *mut c_void);
    fn web_bindings_transfer_data_decode_bool(
        data_holder: *mut c_void,
        out_value: &mut bool,
    ) -> RawNativeFunctionResult;
    fn web_bindings_transfer_data_decode_records(
        data_holder: *mut c_void,
        records: *mut c_void,
        append: unsafe extern "C" fn(*mut c_void, *mut c_void),
    ) -> RawNativeFunctionResult;
    fn web_bindings_transfer_data_decode_transport_handle(
        data_holder: *mut c_void,
        out_handle: &mut *mut c_void,
    ) -> RawNativeFunctionResult;
    fn web_bindings_transfer_data_decoder(data_holder: *mut c_void) -> *mut c_void;
}

/// The record of a structured serialization, which the serialization steps of an object write to, and the serialization
/// that the sub-serializations of the steps run in.
pub(crate) struct SerializationWriter<'a, 'memory> {
    serializer: &'a Serializer<'a, 'a, 'memory>,
}

impl<'a, 'memory> SerializationWriter<'a, 'memory> {
    pub(crate) fn new(serializer: &'a Serializer<'a, 'a, 'memory>) -> Self {
        Self { serializer }
    }

    fn serialized(&self) -> &StructuredSerializeWriter<'memory> {
        self.serializer.serialized()
    }

    /// Sets a field of the serialized form to the sub-serialization of the value,
    /// https://html.spec.whatwg.org/multipage/structured-data.html#sub-serialization
    pub(crate) fn write_value(&mut self, value: Value) -> WebIdlResult<()> {
        self.serializer.serialize(value)
    }

    pub(crate) fn write_u8(&mut self, value: u8) {
        self.serialized().encode_u8(value);
    }

    pub(crate) fn write_i32(&mut self, value: i32) {
        self.serialized().encode_i32(value);
    }

    pub(crate) fn write_u32(&mut self, value: u32) {
        self.serialized().encode_u32(value);
    }

    pub(crate) fn write_u64(&mut self, value: u64) {
        self.serialized().encode_u64(value);
    }

    pub(crate) fn write_f64(&mut self, value: f64) {
        self.serialized().encode_f64(value);
    }

    pub(crate) fn write_bool(&mut self, value: bool) {
        self.serialized().encode_bool(value);
    }

    pub(crate) fn write_i64(&mut self, value: i64) {
        self.serialized().encode_i64(value);
    }

    pub(crate) fn write_optional_u64(&mut self, value: Option<u64>) {
        self.serialized().encode_optional_u64(value);
    }

    pub(crate) fn write_utf16_string(&mut self, string: &Utf16String) {
        self.serialized().encode_utf16_string(string);
    }

    pub(crate) fn write_bytes(&mut self, bytes: &[u8]) {
        self.serialized().encode_bytes(bytes);
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
        encode_serializable_object_header(self.serialized(), T::INTERFACE_NAME)?;
        object.serialization_steps(cx, self, for_storage)
    }
}

/// The record of a structured deserialization, which the deserialization steps of an object read from, and the
/// deserialization that the sub-deserializations of the steps run in. What it reads that the record does not hold
/// throws a "DataCloneError" DOMException.
pub(crate) struct SerializationReader<'a, 'memory> {
    deserializer: &'a Deserializer<'a, 'a, 'memory>,
}

impl<'a, 'memory> SerializationReader<'a, 'memory> {
    pub(crate) fn new(deserializer: &'a Deserializer<'a, 'a, 'memory>) -> Self {
        Self { deserializer }
    }

    fn serialized(&self) -> &'a StructuredSerializeReader<'a> {
        self.deserializer.serialized()
    }

    /// The value of a field of the serialized form, which is a sub-serialization,
    /// https://html.spec.whatwg.org/multipage/structured-data.html#sub-deserialization
    pub(crate) fn read_value(&mut self) -> WebIdlResult<Value> {
        self.deserializer.deserialize()
    }

    /// Whether the record is one for storage, rather than for IPC.
    pub(crate) fn is_for_storage(&mut self) -> bool {
        self.serialized().serialization_type() == SerializationType::Storage
    }

    pub(crate) fn read_u8(&mut self) -> WebIdlResult<u8> {
        self.serialized().decode_u8()
    }

    pub(crate) fn read_i32(&mut self) -> WebIdlResult<i32> {
        self.serialized().decode_i32()
    }

    pub(crate) fn read_u32(&mut self) -> WebIdlResult<u32> {
        self.serialized().decode_u32()
    }

    pub(crate) fn read_u64(&mut self) -> WebIdlResult<u64> {
        self.serialized().decode_u64()
    }

    pub(crate) fn read_f64(&mut self) -> WebIdlResult<f64> {
        self.serialized().decode_f64()
    }

    /// Reads what SerializationWriter::write_object_in_place() writes, and returns the new object.
    pub(crate) fn read_object_in_place<T: Serializable>(&mut self, cx: &mut CallContext) -> WebIdlResult<Ref<T>> {
        decode_serializable_object_header(self.serialized(), T::INTERFACE_NAME)?;
        let object = T::deserialization_steps(cx, self)?;
        Ok(object.into_object(cx.vm()))
    }

    pub(crate) fn read_bool(&mut self) -> WebIdlResult<bool> {
        self.serialized().decode_bool()
    }

    pub(crate) fn read_i64(&mut self) -> WebIdlResult<i64> {
        self.serialized().decode_i64()
    }

    pub(crate) fn read_optional_u64(&mut self) -> WebIdlResult<Option<u64>> {
        self.serialized().decode_optional_u64()
    }

    /// Bytes that the record holds, which it lends for as long as the deserialization runs.
    pub(crate) fn read_borrowed_bytes(&mut self) -> WebIdlResult<&'a [u8]> {
        self.serialized().decode_bytes()
    }

    /// A copy of bytes that the record holds. If the copy cannot be allocated, a "DataCloneError" DOMException.
    pub(crate) fn read_bytes(&mut self) -> WebIdlResult<Vec<u8>> {
        let bytes = self.read_borrowed_bytes()?;
        let mut copy = Vec::new();
        copy.try_reserve_exact(bytes.len()).map_err(|_| {
            data_clone_error_from_serialization_error(
                self.serialized().realm(),
                "Unable to allocate a copy of the bytes",
            )
        })?;
        copy.extend_from_slice(bytes);
        Ok(copy)
    }

    pub(crate) fn read_utf16_string(&mut self) -> WebIdlResult<Utf16String> {
        self.serialized().decode_utf16_string()
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
    fn deserialization_steps(
        cx: &mut CallContext,
        serialized: &mut SerializationReader,
    ) -> WebIdlResult<impl ConstructedObject<Self> + use<Self>>;
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

    pub(crate) fn write_bool(&mut self, value: bool) {
        // SAFETY: The data holder is lent for the steps.
        unsafe { web_bindings_transfer_data_encode_bool(self.raw.as_ptr(), value) };
    }

    /// Writes the records of structured serializations with transfer, as a list.
    pub(crate) fn write_records(&mut self, records: &[OwnedSerializedTransferRecord]) {
        let records: Vec<*mut c_void> = records.iter().map(|record| record.record().as_raw()).collect();
        // SAFETY: The data holder is lent for the steps, and the call borrows the records.
        unsafe { web_bindings_transfer_data_encode_records(self.raw.as_ptr(), records.as_ptr(), records.len()) };
    }

    /// Writes the handle of one end of an IPC transport, whose rights the data holder takes.
    pub(crate) fn write_transport_handle(&mut self, handle: TransportHandle) {
        // SAFETY: The data holder is lent for the steps, and the call borrows the handle.
        unsafe { web_bindings_transfer_data_encode_transport_handle(self.raw.as_ptr(), handle.as_raw()) };
    }

    /// Writes ! StructuredSerializeWithTransfer(value, « value ») as the data holder of value, which is all that the
    /// record holds: its [[Serialized]] is a reference to the transferred value. read_transferred_value() reads it.
    pub(crate) fn write_transferred_value(&mut self, cx: &CallContext, value: Value) -> WebIdlResult<()> {
        let record = structured_serialize_with_transfer(cx, value, &[value.as_object()])?;
        // SAFETY: The data holder is lent for the steps, and the call borrows the record.
        unsafe { web_bindings_transfer_data_extend(self.raw.as_ptr(), record.record().as_raw()) };
        Ok(())
    }
}

/// The data holder of a received object, which its transfer-receiving steps read from: a
/// Web::Bindings::RustTransferDataReader that C++ lends for the steps. What it reads that the data holder does not hold
/// throws a "DataCloneError" DOMException.
pub(crate) struct TransferDataReader<'a> {
    raw: NonNull<c_void>,
    _lent: PhantomData<&'a mut c_void>,
}

impl TransferDataReader<'_> {
    /// The data holder as the C ABI of an interface's C++ class takes it, which decodes what the steps read.
    pub(crate) fn as_raw(&mut self) -> *mut c_void {
        self.raw.as_ptr()
    }

    pub(crate) fn read_bool(&mut self) -> WebIdlResult<bool> {
        let mut value = false;
        // SAFETY: The data holder is lent for the steps.
        unsafe { web_bindings_transfer_data_decode_bool(self.raw.as_ptr(), &mut value) }.into_completion()?;
        Ok(value)
    }

    /// Reads what TransferDataWriter::write_records() writes.
    pub(crate) fn read_records(&mut self) -> WebIdlResult<Vec<OwnedSerializedTransferRecord>> {
        unsafe extern "C" fn append(records: *mut c_void, record: *mut c_void) {
            // SAFETY: The records are the Vec that read_records() lends, and LibWeb hands over the record.
            unsafe {
                (*records.cast::<Vec<OwnedSerializedTransferRecord>>())
                    .push(OwnedSerializedTransferRecord::from_raw(record));
            }
        }
        let mut records = Vec::new();
        // SAFETY: The data holder and the records are lent for the steps.
        unsafe { web_bindings_transfer_data_decode_records(self.raw.as_ptr(), (&raw mut records).cast(), append) }
            .into_completion()?;
        Ok(records)
    }

    /// Reads what TransferDataWriter::write_transport_handle() writes.
    pub(crate) fn read_transport_handle(&mut self) -> WebIdlResult<TransportHandle> {
        let mut handle = core::ptr::null_mut();
        // SAFETY: The data holder is lent for the steps.
        unsafe { web_bindings_transfer_data_decode_transport_handle(self.raw.as_ptr(), &mut handle) }
            .into_completion()?;
        // SAFETY: LibWeb hands over the handle it read.
        Ok(unsafe { TransportHandle::from_raw(handle) })
    }

    /// ! StructuredDeserializeWithTransfer(dataHolder, the current Realm).[[Deserialized]] for what
    /// TransferDataWriter::write_transferred_value() wrote: the transferred value, created in the realm.
    pub(crate) fn read_transferred_value(&mut self, cx: &CallContext) -> WebIdlResult<Value> {
        // SAFETY: The data holder is lent for the steps, and LibWeb returns its decoder.
        let decoder = unsafe { TransferDataDecoder::from_raw(web_bindings_transfer_data_decoder(self.raw.as_ptr())) };
        structured_deserialize_with_transfer_internal(cx, decoder)
    }
}

/// An interface that is transferable, [Transferable] in its IDL.
pub(crate) trait Transferable: Interface {
    /// The [[Detached]] internal slot of the object, https://html.spec.whatwg.org/multipage/structured-data.html#detached
    fn detached(&self) -> &Cell<bool>;

    /// https://html.spec.whatwg.org/multipage/structured-data.html#transfer-steps
    fn transfer_steps(this: &Ref<Self>, cx: &mut CallContext, data_holder: &mut TransferDataWriter)
    -> WebIdlResult<()>;

    /// https://html.spec.whatwg.org/multipage/structured-data.html#transfer-receiving-steps, which create the object.
    fn transfer_receiving_steps(cx: &mut CallContext, data_holder: &mut TransferDataReader) -> WebIdlResult<Ref<Self>>;
}

/// The completion of steps, as the entry points return it to C++.
fn completion(cx: &CallContext, result: WebIdlResult<Value>) -> RawNativeFunctionResult {
    let result: ThrowCompletionOr<Value> = result.map_err(|exception| exception.throw(cx));
    RawNativeFunctionResult::from_completion(result)
}

/// The wrapper in the realm of the call context of the object that steps created.
fn wrap_new_object<T: Interface>(cx: &CallContext, object: impl ConstructedObject<T>) -> Value {
    Value::from_object(wrap(cx, object.into_object(cx.vm())))
}

/// The entry point of the serialization steps of `T`.
///
/// # Safety
///
/// The pointers must be the VM, a live realm, the live cell of an object of `T`, and the SerializationWriter that
/// crate::html::structured_serialize lends through C++.
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
    // SAFETY: The caller passes the writer that the serialization lends for the steps.
    let serialized = unsafe { &mut *serialized.cast::<SerializationWriter<'_, '_>>() };
    let result = object.serialization_steps(&mut cx, serialized, for_storage);
    completion(&cx, result.map(|()| Value::UNDEFINED))
}

/// The entry point of the deserialization steps of `T`, which returns the wrapper of the new object.
///
/// # Safety
///
/// The pointers must be the VM, a live realm, and the SerializationReader that crate::html::structured_serialize
/// lends through C++.
pub(crate) unsafe fn deserialization_steps<T: Serializable>(
    vm: *mut c_void,
    realm: *mut c_void,
    serialized: *mut c_void,
) -> RawNativeFunctionResult {
    // SAFETY: The caller passes the VM and a live realm.
    let (vm, realm) = unsafe { realm_arguments(vm, realm) };
    let mut cx = CallContext::new(vm, realm);
    // SAFETY: The caller passes the reader that the deserialization lends for the steps.
    let serialized = unsafe { &mut *serialized.cast::<SerializationReader<'_, '_>>() };
    let result = T::deserialization_steps(&mut cx, serialized);
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
    let result = T::transfer_steps(&object, &mut cx, &mut data_holder);
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
