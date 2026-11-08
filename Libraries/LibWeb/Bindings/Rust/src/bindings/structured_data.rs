/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What an interface implemented in Rust that is serializable or transferable does in a structured serialization,
//! https://html.spec.whatwg.org/multipage/structured-data.html#serializable-objects and
//! https://html.spec.whatwg.org/multipage/structured-data.html#transferable-objects
//!
//! The algorithms of crate::html::structured_serialize call the steps of such an interface directly through the
//! generated Rust dispatch. Those steps write and read the record of the algorithm.
//! The algorithms run the transfer steps and the transfer-receiving steps themselves, which write and read the Rust
//! data holders of the transfer. Unlike an object of an interface implemented in C++, which is created before its
//! deserialization steps fill it in, an object of an interface implemented in Rust is what its deserialization steps
//! and its transfer-receiving steps return.

use core::cell::Cell;
use core::ffi::c_void;
use core::ptr::NonNull;
use std::collections::VecDeque;

use ak::Utf16String;
use libipc_rust::{Decode, DecodeError, Decoder, Encode};
use libjs_rust::api::{Gc, MarkedVec, Object, Realm, Value, Vm};
use libwebcommon_rust::html::serialization_records::SerializationType;

use crate::bindings::interface::{ConstructedObject, Interface, Ref, implementation_of, wrap};
use crate::bindings::{CallContext, Exception, WebIdlResult};
use crate::html::message_port::{TransferredTransport, TransportHandle};
use crate::html::structured_serialize::cpp::{
    ImageBitmapSharedPixels, SharedPixelBuffers, data_clone_error_from_serialization_error,
};
use crate::html::structured_serialize::reader::StructuredSerializeReader;
use crate::html::structured_serialize::transfer_data_holder::{CarriedChannelEnd, TransferDataHolder, TransferPayload};
use crate::html::structured_serialize::transfer_record::{TransferRecord, TransferRecordRef};
use crate::html::structured_serialize::writer::StructuredSerializeWriter;
use crate::html::structured_serialize::{
    Deserializer, Serializer, decode_serializable_object_header, encode_serializable_object_header,
    structured_deserialize_with_transfer_internal, structured_serialize_with_transfer,
};

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

    pub(crate) fn write_u16(&mut self, value: u16) {
        self.serialized().encode_u16(value);
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

    pub(crate) fn read_u16(&mut self) -> WebIdlResult<u16> {
        self.serialized().decode_u16()
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
    pub(crate) fn write_records(&mut self, cx: &CallContext, records: &[TransferRecordRef]) {
        self.data_holder.with_encoder(|encoder| {
            encoder.encode_size(records.len())?;
            for &record in records {
                TransferRecord::encode_into(record, cx.vm(), encoder)?;
            }
            Ok(())
        });
    }

    /// Writes what a MessagePort reaches its peer with, which the data holder takes.
    pub(crate) fn write_transport(&mut self, transport: TransferredTransport) {
        match transport {
            TransferredTransport::Channel(end) => {
                self.write_bool(true);
                self.data_holder.add_channel_end(end);
            }
            TransferredTransport::Socket(handle) => {
                self.write_bool(false);
                self.data_holder.with_encoder(|encoder| handle.encode_into(encoder));
            }
        }
    }

    /// Writes ! StructuredSerializeWithTransfer(value, « value ») as the data holder of value, which is all that the
    /// record holds: its [[Serialized]] is a reference to the transferred value. read_transferred_value() reads it.
    pub(crate) fn write_transferred_value(&mut self, cx: &CallContext, value: Value) -> WebIdlResult<()> {
        let record = structured_serialize_with_transfer(cx, value, &[value.as_object()])?;
        for holder in record.data_holders.borrow_mut().drain(..) {
            self.data_holder.extend(holder);
        }
        TransferRecord::discard(record);
        Ok(())
    }
}

/// The data holder of a received object, which its transfer-receiving steps read from, in the realm that the object is
/// created in. What the steps read that the data holder does not hold throws a "DataCloneError" DOMException.
pub(crate) struct TransferDataReader<'a> {
    decoder: Decoder<'a>,
    channel_ends: VecDeque<CarriedChannelEnd>,
    realm: Gc<Realm>,
    shared_pixels: Option<&'a SharedPixelBuffers<'a>>,
}

impl<'a> TransferDataReader<'a> {
    pub(crate) fn new(payload: &'a mut TransferPayload, realm: Gc<Realm>) -> Self {
        Self {
            decoder: Decoder::new(&payload.bytes, core::mem::take(&mut payload.attachments)),
            channel_ends: core::mem::take(&mut payload.channel_ends),
            realm,
            shared_pixels: None,
        }
    }

    pub(crate) fn with_shared_pixels(
        payload: &'a mut TransferPayload,
        realm: Gc<Realm>,
        shared_pixels: &'a SharedPixelBuffers<'a>,
    ) -> Self {
        Self {
            shared_pixels: Some(shared_pixels),
            ..Self::new(payload, realm)
        }
    }

    pub(crate) fn shared_pixels_as_raw(&self) -> WebIdlResult<*mut c_void> {
        self.shared_pixels
            .and_then(SharedPixelBuffers::as_raw)
            .map(core::ptr::NonNull::as_ptr)
            .ok_or_else(|| data_clone_error_from_serialization_error(self.realm, "Invalid ImageBitmap shared pixels"))
    }

    pub(crate) fn realm(&self) -> Gc<Realm> {
        self.realm
    }

    fn decoded<T>(&mut self, read: impl FnOnce(&mut Decoder<'a>) -> Result<T, DecodeError>) -> WebIdlResult<T> {
        read(&mut self.decoder).map_err(|error| {
            data_clone_error_from_serialization_error(self.realm, &format!("Unable to decode transfer data: {error:?}"))
        })
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
    pub(crate) fn read_records<'vm>(&mut self, vm: &'vm Vm) -> WebIdlResult<MarkedVec<'vm, TransferRecordRef>> {
        let records = MarkedVec::new(vm);
        let result = self.decoded(|decoder| {
            let count = decoder.decode_size()?;
            for _ in 0..count {
                records.push(TransferRecord::decode(vm, decoder)?);
            }
            Ok(())
        });
        if let Err(exception) = result {
            for &record in records.values().iter() {
                TransferRecord::discard(record);
            }
            return Err(exception);
        }
        Ok(records)
    }

    /// Reads what TransferDataWriter::write_transport() writes.
    pub(crate) fn read_transport(&mut self) -> WebIdlResult<TransferredTransport> {
        if !self.read::<bool>()? {
            return Ok(TransferredTransport::Socket(self.decoded(TransportHandle::decode)?));
        }
        let end = self.channel_ends.pop_front().ok_or_else(|| {
            data_clone_error_from_serialization_error(self.realm, "Missing channel end in transfer data")
        })?;
        Ok(TransferredTransport::Channel(end.into_end()))
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

pub(crate) fn serialization_steps<T: Serializable>(
    cx: &CallContext,
    object: Gc<Object>,
    serialized: &mut SerializationWriter,
    for_storage: bool,
) -> WebIdlResult<()> {
    let mut cx = CallContext::new(cx.vm(), cx.realm());
    implementation_of::<T>(object)
        .serialization_steps(&mut cx, serialized, for_storage)
        .map_err(|exception| Exception::Thrown(exception.throw(&cx)))
}

pub(crate) fn deserialization_steps<T: Serializable>(
    cx: &CallContext,
    serialized: &mut SerializationReader,
) -> WebIdlResult<Value> {
    let mut cx = CallContext::new(cx.vm(), cx.realm());
    let object = T::deserialization_steps(&mut cx, serialized)?;
    Ok(Value::from_object(wrap(&cx, object.into_object(cx.vm()))))
}
