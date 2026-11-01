/*
 * Copyright (c) 2022, Daniel Ehrenberg <dan@littledan.dev>
 * Copyright (c) 2022, Andrew Kaster <akaster@serenityos.org>
 * Copyright (c) 2023-2024, Kenneth Myhra <kennethmyhra@serenityos.org>
 * Copyright (c) 2023, Idan Horowitz <idan.horowitz@serenityos.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The record that a structured deserialization reads: the bytes of the values in the format of the record
//! (libwebcommon_rust::html::serialization_records), which C++ lends, and the SharedArrayBuffers that the record shares
//! beside them. What the deserialization reads that the record does not hold throws a "DataCloneError" DOMException in
//! the realm that it creates its values in.

use core::ffi::c_void;
use core::ptr::NonNull;

use ak::Utf16String;
use libjs_rust::api::{
    ArrayBuffer, ArrayBufferShared, AsSharedMemory, BigInt, DataBlock, DataBlockStorage, ExternalPrimitiveStorage,
    ForeignCellSlot, GC_PRIMITIVE_STORAGE_NULL_HANDLE, Gc, Realm, SharedBackingStore, Value, Vm,
};
use libwebcommon_rust::html::serialization_records::{DecodeError, DecodedString, RecordDecoder, SerializationType};

use crate::bindings::WebIdlResult;
use crate::html::structured_serialize::cpp::{
    ReceivedIPCSerializationRecord, data_clone_error_from_serialization_error, is_ipc_interface,
    serializable_interface_name_from_storage_identifier,
};
use crate::html::structured_serialize::message_record::{MessageRecord, SharedMemory};

/// Where the record keeps the SharedArrayBuffers that it shares.
#[derive(Clone, Copy)]
enum SharedArrayBuffers<'a> {
    None,
    /// The side table of a record that C++ lends, and the shared memory that it was handed over with, if any.
    Lent(ReceivedIPCSerializationRecord<'a>, &'a [SharedMemory]),
    Message(&'a MessageRecord),
}

pub(crate) struct StructuredSerializeReader<'a> {
    decoder: RecordDecoder<'a>,
    realm: Gc<Realm>,
    shared_array_buffers: SharedArrayBuffers<'a>,
}

impl<'a> StructuredSerializeReader<'a> {
    pub(crate) fn new_ipc(
        realm: Gc<Realm>,
        record: ReceivedIPCSerializationRecord<'a>,
        shared_memory: &'a [SharedMemory],
    ) -> Self {
        Self {
            decoder: RecordDecoder::new(SerializationType::Ipc, record.data()).expect("an IPC record has no prologue"),
            realm,
            shared_array_buffers: SharedArrayBuffers::Lent(record, shared_memory),
        }
    }

    pub(crate) fn new_message(realm: Gc<Realm>, record: &'a MessageRecord) -> Self {
        Self {
            decoder: RecordDecoder::new(SerializationType::Ipc, &record.bytes).expect("an IPC record has no prologue"),
            realm,
            shared_array_buffers: SharedArrayBuffers::Message(record),
        }
    }

    /// The reader of the bytes of a storage record, which throws if they do not start with the prologue of the format.
    pub(crate) fn new_storage(realm: Gc<Realm>, data: &'a [u8]) -> WebIdlResult<Self> {
        let decoder = RecordDecoder::new(SerializationType::Storage, data)
            .map_err(|error| data_clone_error_from_serialization_error(realm, error.0))?;
        Ok(Self {
            decoder,
            realm,
            shared_array_buffers: SharedArrayBuffers::None,
        })
    }

    /// The realm that the deserialization creates its values in.
    pub(crate) fn realm(&self) -> Gc<Realm> {
        self.realm
    }

    pub(crate) fn serialization_type(&self) -> SerializationType {
        self.decoder.serialization_type()
    }

    pub(crate) fn is_at_end(&self) -> bool {
        self.decoder.is_at_end()
    }

    fn decoded<T>(&self, result: Result<T, DecodeError>) -> WebIdlResult<T> {
        result.map_err(|error| data_clone_error_from_serialization_error(self.realm, error.0))
    }

    pub(crate) fn decode_u8(&self) -> WebIdlResult<u8> {
        self.decoded(self.decoder.decode_u8())
    }

    pub(crate) fn decode_bool(&self) -> WebIdlResult<bool> {
        self.decoded(self.decoder.decode_bool())
    }

    pub(crate) fn decode_i32(&self) -> WebIdlResult<i32> {
        self.decoded(self.decoder.decode_i32())
    }

    pub(crate) fn decode_u16(&self) -> WebIdlResult<u16> {
        self.decoded(self.decoder.decode_u16())
    }

    pub(crate) fn decode_u32(&self) -> WebIdlResult<u32> {
        self.decoded(self.decoder.decode_u32())
    }

    pub(crate) fn decode_u64(&self) -> WebIdlResult<u64> {
        self.decoded(self.decoder.decode_u64())
    }

    /// A u64 that is the size of something in memory.
    pub(crate) fn decode_size(&self) -> WebIdlResult<usize> {
        let size = self.decode_u64()?;
        self.decoded(
            usize::try_from(size)
                .map_err(|_| DecodeError("Structured serialize integer does not fit in native integer type")),
        )
    }

    pub(crate) fn decode_i64(&self) -> WebIdlResult<i64> {
        self.decoded(self.decoder.decode_i64())
    }

    pub(crate) fn decode_f64(&self) -> WebIdlResult<f64> {
        self.decoded(self.decoder.decode_f64())
    }

    pub(crate) fn decode_optional_f64(&self) -> WebIdlResult<Option<f64>> {
        if !self.decode_bool()? {
            return Ok(None);
        }
        Ok(Some(self.decode_f64()?))
    }

    pub(crate) fn decode_optional_u64(&self) -> WebIdlResult<Option<u64>> {
        if !self.decode_bool()? {
            return Ok(None);
        }
        Ok(Some(self.decode_u64()?))
    }

    pub(crate) fn decode_utf16_string(&self) -> WebIdlResult<Utf16String> {
        Ok(match self.decoded(self.decoder.decode_utf16_string())? {
            DecodedString::Latin1(characters) => Utf16String::from_latin1(characters),
            DecodedString::Utf16(code_units) => Utf16String::from_utf16(&code_units),
        })
    }

    pub(crate) fn decode_optional_utf16_string(&self) -> WebIdlResult<Option<Utf16String>> {
        if !self.decode_bool()? {
            return Ok(None);
        }
        Ok(Some(self.decode_utf16_string()?))
    }

    pub(crate) fn decode_bytes(&self) -> WebIdlResult<&'a [u8]> {
        self.decoded(self.decoder.decode_bytes())
    }

    /// Bindings::InterfaceName, which a storage record holds as the identifier of the interface. It is the name of a
    /// serializable interface.
    pub(crate) fn decode_interface_name(&self) -> WebIdlResult<u16> {
        let interface_name = match self.serialization_type() {
            SerializationType::Ipc => {
                let interface_name = self.decode_u16()?;
                is_ipc_interface(interface_name).then_some(interface_name)
            }
            SerializationType::Storage => match self.decode_utf16_string()?.as_units() {
                ak::Utf16StringUnits::Ascii(identifier) => {
                    serializable_interface_name_from_storage_identifier(identifier)
                }
                ak::Utf16StringUnits::Latin1(_) | ak::Utf16StringUnits::Utf16(_) => None,
            },
        };
        self.decoded(interface_name.ok_or(DecodeError("Unknown structured serialize interface name")))
    }

    /// A new BigInt with the value that StructuredSerializeWriter::encode_big_int() wrote.
    pub(crate) fn decode_big_int(&self, vm: &Vm) -> WebIdlResult<Value> {
        let (is_negative, magnitude) = self.decoded(self.decoder.decode_big_int())?;
        Ok(Value::from_bigint(BigInt::create_from_magnitude_bytes_be(
            vm,
            is_negative,
            magnitude,
        )))
    }

    pub(crate) fn array_buffer_from_shared_memory(
        &self,
        vm: &Vm,
        index: u32,
        shared_object_id: u64,
    ) -> WebIdlResult<Value> {
        let shared_memory = match self.shared_array_buffers {
            SharedArrayBuffers::None => &[],
            SharedArrayBuffers::Lent(_, shared_memory) => shared_memory,
            SharedArrayBuffers::Message(record) => record.shared_memory.as_slice(),
        };
        let Some(shared_memory) = shared_memory.get(index as usize) else {
            return Err(data_clone_error_from_serialization_error(
                self.realm,
                "SharedArrayBuffer side-list index out of range",
            ));
        };
        let array_buffer = ArrayBuffer::create_from_shared_memory(
            vm,
            self.realm,
            shared_memory.descriptor.as_shared_memory(),
            shared_memory.size,
            shared_object_id,
        )
        .map_err(|_| data_clone_error_from_serialization_error(self.realm, "Unable to map SharedArrayBuffer memory"))?;
        Ok(Value::from_object(array_buffer))
    }

    /// A new SharedArrayBuffer that shares the storage of the buffer at the index of the record's same-process side
    /// table, if there is such a buffer, and its storage can be shared.
    pub(crate) fn array_buffer_sharing_side_table_entry(
        &self,
        vm: &Vm,
        side_table_index: u32,
        byte_length: usize,
        preserve_growability: bool,
    ) -> Option<Gc<ArrayBuffer>> {
        let source_buffer = match self.shared_array_buffers {
            SharedArrayBuffers::None => return None,
            SharedArrayBuffers::Lent(record, _) => record.shared_array_buffer(side_table_index)?,
            SharedArrayBuffers::Message(record) => *record.shared_array_buffers.get(side_table_index as usize)?,
        };
        array_buffer_sharing_storage_with(vm, self.realm, source_buffer, byte_length, preserve_growability)
    }
}

fn foreign_cell_slot(cell: NonNull<c_void>) -> ForeignCellSlot {
    let slot = ForeignCellSlot::empty();
    // SAFETY: The cell is a live cell of the heap, which the slot then keeps alive.
    unsafe { slot.set(Some(cell)) };
    slot
}

// Returns None when the source storage cannot be aliased, allowing the caller to copy it.
fn array_buffer_sharing_storage_with(
    vm: &Vm,
    realm: Gc<Realm>,
    source_buffer: Gc<ArrayBuffer>,
    byte_length: usize,
    preserve_growability: bool,
) -> Option<Gc<ArrayBuffer>> {
    assert!(source_buffer.is_shared_array_buffer());
    let fixed_byte_length = (!preserve_growability).then_some(byte_length);
    let data_block = source_buffer.with_data_block(|source_block| match &source_block.byte_buffer {
        // Flatten alias chains to the actual owner. Keep this buffer's byte length because the owner's
        // storage may be larger (for example, a fixed-length view of growable WebAssembly.Memory).
        DataBlockStorage::External(external) => Some(DataBlock::external(
            ExternalPrimitiveStorage::new(
                foreign_cell_slot(external.owner()),
                external.handle(),
                fixed_byte_length,
            ),
            ArrayBufferShared::Yes,
        )),

        // AD-HOC: A fixed-length shared block backed by cross-process shared memory is aliased by referencing the same
        //         shared object — so the clone shares its bytes, and can itself be shared with an agent in another
        //         process.
        DataBlockStorage::Shared(store) => {
            SharedBackingStore::adopt(store.shared_memory(), store.size(), store.object_id())
                .ok()
                .map(DataBlock::shared_memory)
        }

        DataBlockStorage::Owned(owned) => {
            // Zero-length buffers have no backing storage to share.
            if owned.handle() == GC_PRIMITIVE_STORAGE_NULL_HANDLE {
                return None;
            }
            // SAFETY: The source buffer is a live cell.
            let owner = unsafe { NonNull::new_unchecked(source_buffer.as_ptr().cast()) };
            Some(DataBlock::external(
                ExternalPrimitiveStorage::new(foreign_cell_slot(owner), owned.handle(), fixed_byte_length),
                ArrayBufferShared::Yes,
            ))
        }

        // A detached block has no storage to share.
        DataBlockStorage::Empty => None,
    })?;
    Some(ArrayBuffer::create_from_data_block(vm, realm, data_block))
}
