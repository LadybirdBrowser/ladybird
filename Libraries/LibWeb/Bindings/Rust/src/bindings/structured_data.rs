/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What an interface implemented in Rust that is serializable or transferable does in a structured serialization,
//! https://html.spec.whatwg.org/multipage/structured-data.html#serializable-objects and
//! https://html.spec.whatwg.org/multipage/structured-data.html#transferable-objects
//!
//! The algorithms of crate::html::structured_serialize run the serialization steps of such an interface through
//! LibWeb's C++, which dispatches to the entry points that the generated bindings of the interface define, which call
//! the functions here. Those steps write and read the record of the algorithm, which it passes through C++ to them.
//! The algorithms run the transfer steps and the transfer-receiving steps themselves, which write and read the C++
//! data holders of the transfer. Unlike an object of an interface implemented in C++, which is created before its
//! deserialization steps fill it in, an object of an interface implemented in Rust is what its deserialization steps
//! and its transfer-receiving steps return.

use core::cell::Cell;
use core::ffi::c_void;
use core::ptr::NonNull;

use ak::Utf16String;
use libipc_rust::{Decode, DecodeError, Decoder, Encode};
use libjs_rust::api::{Gc, RawNativeFunctionResult, Realm, ThrowCompletionOr, Value};
use libwebcommon_rust::html::serialization_records::SerializationType;

use crate::bindings::interface::{ConstructedObject, Interface, Ref, cell_from_abi, realm_arguments, wrap};
use crate::bindings::transport::TransportHandle;
use crate::bindings::{CallContext, WebIdlResult};
use crate::html::structured_serialize::cpp::{
    ImageBitmapSharedPixels, OwnedSerializedTransferRecord, TransferDataDecoder,
    data_clone_error_from_serialization_error,
};
use crate::html::structured_serialize::reader::StructuredSerializeReader;
use crate::html::structured_serialize::transfer_data_holder::TransferDataHolder;
use crate::html::structured_serialize::writer::StructuredSerializeWriter;
use crate::html::structured_serialize::{
    Deserializer, Serializer, decode_serializable_object_header, encode_serializable_object_header,
    structured_deserialize_with_transfer_internal, structured_serialize_with_transfer,
};

unsafe extern "C" {
    fn web_bindings_transfer_data_encode_records(data_holder: *mut c_void, records: *const *mut c_void, count: usize);
    fn web_bindings_transfer_data_encode_transport_handle(data_holder: *mut c_void, handle: *mut c_void);
    fn web_bindings_transfer_data_extend(data_holder: *mut c_void, record: *mut c_void);
    fn web_bindings_transfer_data_decode_records(
        data_holder: *mut c_void,
        realm: *mut c_void,
        offset: &mut usize,
        records: *mut c_void,
        append: unsafe extern "C" fn(*mut c_void, *mut c_void),
    ) -> RawNativeFunctionResult;
    fn web_bindings_transfer_data_decode_transport_handle(
        data_holder: *mut c_void,
        realm: *mut c_void,
        offset: &mut usize,
        out_handle: &mut *mut c_void,
    ) -> RawNativeFunctionResult;
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

/// The data holder of a transferred object, which its transfer steps write to, in the realm of the transfer, which
/// creates what the steps throw.
pub(crate) struct TransferDataWriter<'a> {
    data_holder: &'a TransferDataHolder,
    realm: Gc<Realm>,
    /// AD-HOC: The shared memory that the transfer gathers the pixels of its ImageBitmaps in, if it has some.
    image_bitmap_pixels: Option<&'a ImageBitmapSharedPixels>,
}

impl<'a> TransferDataWriter<'a> {
    pub(crate) fn new(
        data_holder: &'a TransferDataHolder,
        realm: Gc<Realm>,
        image_bitmap_pixels: Option<&'a ImageBitmapSharedPixels>,
    ) -> Self {
        Self {
            data_holder,
            realm,
            image_bitmap_pixels,
        }
    }

    pub(crate) fn realm(&self) -> Gc<Realm> {
        self.realm
    }

    /// The shared pixels as the C ABI of ImageBitmap's C++ code takes them, or null.
    pub(crate) fn image_bitmap_pixels_as_raw(&self) -> *mut c_void {
        self.image_bitmap_pixels
            .and_then(ImageBitmapSharedPixels::as_raw)
            .map_or(core::ptr::null_mut(), NonNull::as_ptr)
    }

    pub(crate) fn write<T: Encode + ?Sized>(&mut self, value: &T) {
        self.data_holder.encode(value);
    }

    pub(crate) fn write_bool(&mut self, value: bool) {
        self.write(&value);
    }

    /// Writes the records of structured serializations with transfer, as a list.
    pub(crate) fn write_records(&mut self, records: &[OwnedSerializedTransferRecord]) {
        let records: Vec<*mut c_void> = records.iter().map(|record| record.record().as_raw()).collect();
        // SAFETY: The carrier is live, and the call borrows the records.
        unsafe {
            web_bindings_transfer_data_encode_records(
                self.data_holder.carrier().as_raw(),
                records.as_ptr(),
                records.len(),
            );
        }
    }

    /// Writes the handle of one end of an IPC transport, whose rights the data holder takes.
    pub(crate) fn write_transport_handle(&mut self, handle: TransportHandle) {
        // SAFETY: The carrier is live, and the call borrows the handle.
        unsafe {
            web_bindings_transfer_data_encode_transport_handle(self.data_holder.carrier().as_raw(), handle.as_raw());
        }
    }

    /// Writes ! StructuredSerializeWithTransfer(value, « value ») as the data holder of value, which is all that the
    /// record holds: its [[Serialized]] is a reference to the transferred value. read_transferred_value() reads it.
    pub(crate) fn write_transferred_value(&mut self, cx: &CallContext, value: Value) -> WebIdlResult<()> {
        let record = structured_serialize_with_transfer(cx, value, &[value.as_object()])?;
        // SAFETY: The carrier is live, and the call borrows the record.
        unsafe { web_bindings_transfer_data_extend(self.data_holder.carrier().as_raw(), record.record().as_raw()) };
        Ok(())
    }
}

/// The data holder of a received object, which its transfer-receiving steps read from, in the realm that the object is
/// created in. Rust reads its bytes, which an HTML::TransferDataDecoder that C++ lends holds, and C++ decodes the values
/// that have attachments at the offset that Rust has read to. What the steps read that the data holder does not hold
/// throws a "DataCloneError" DOMException.
pub(crate) struct TransferDataReader<'a> {
    decoder: TransferDataDecoder,
    bytes: &'a [u8],
    offset: usize,
    realm: Gc<Realm>,
}

impl<'a> TransferDataReader<'a> {
    /// # Safety
    ///
    /// `decoder` must be lent for `'a`.
    pub(crate) unsafe fn new(decoder: TransferDataDecoder, realm: Gc<Realm>) -> Self {
        Self {
            decoder,
            // SAFETY: The caller lends the decoder for 'a.
            bytes: unsafe { decoder.bytes() },
            offset: 0,
            realm,
        }
    }

    pub(crate) fn realm(&self) -> Gc<Realm> {
        self.realm
    }

    /// The data holder as the C ABI of an interface's C++ class takes it.
    pub(crate) fn as_raw(&self) -> *mut c_void {
        self.decoder.as_raw()
    }

    fn decoded<T>(&mut self, read: impl FnOnce(&mut Decoder<'a>) -> Result<T, DecodeError>) -> WebIdlResult<T> {
        let mut decoder = Decoder::new(&self.bytes[self.offset..], []);
        let value = read(&mut decoder)
            .map_err(|_| data_clone_error_from_serialization_error(self.realm, "Unable to decode transfer data"))?;
        self.offset = self.bytes.len() - decoder.remaining_bytes();
        Ok(value)
    }

    pub(crate) fn read<T: Decode>(&mut self) -> WebIdlResult<T> {
        self.decoded(Decoder::decode)
    }

    /// Reads bytes, which go as their count and then the bytes, as IPC encodes a ByteBuffer.
    pub(crate) fn read_bytes(&mut self) -> WebIdlResult<&'a [u8]> {
        self.decoded(|decoder| {
            let size = decoder.decode_size()?;
            decoder.read_bytes(size)
        })
    }

    pub(crate) fn read_bool(&mut self) -> WebIdlResult<bool> {
        self.read()
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
        // SAFETY: The data holder and the records are lent for the steps, and the realm is live.
        unsafe {
            web_bindings_transfer_data_decode_records(
                self.as_raw(),
                self.realm.as_ptr().cast(),
                &mut self.offset,
                (&raw mut records).cast(),
                append,
            )
        }
        .into_completion()?;
        Ok(records)
    }

    /// Reads what TransferDataWriter::write_transport_handle() writes.
    pub(crate) fn read_transport_handle(&mut self) -> WebIdlResult<TransportHandle> {
        let mut handle = core::ptr::null_mut();
        // SAFETY: The data holder is lent for the steps, and the realm is live.
        unsafe {
            web_bindings_transfer_data_decode_transport_handle(
                self.as_raw(),
                self.realm.as_ptr().cast(),
                &mut self.offset,
                &mut handle,
            )
        }
        .into_completion()?;
        // SAFETY: LibWeb hands over the handle it read.
        Ok(unsafe { TransportHandle::from_raw(handle) })
    }

    /// ! StructuredDeserializeWithTransfer(dataHolder, the current Realm).[[Deserialized]] for what
    /// TransferDataWriter::write_transferred_value() wrote: the transferred value, created in the realm.
    pub(crate) fn read_transferred_value(&mut self, cx: &CallContext) -> WebIdlResult<Value> {
        structured_deserialize_with_transfer_internal(cx, self)
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
