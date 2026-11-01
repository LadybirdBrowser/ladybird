/*
 * Copyright (c) 2022, Daniel Ehrenberg <dan@littledan.dev>
 * Copyright (c) 2022, Andrew Kaster <akaster@serenityos.org>
 * Copyright (c) 2023-2024, Kenneth Myhra <kennethmyhra@serenityos.org>
 * Copyright (c) 2023, Idan Horowitz <idan.horowitz@serenityos.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The record that a structured serialization writes: the bytes of the values in the format of the record
//! (libwebcommon_rust::html::serialization_records), and the SharedArrayBuffers that the record shares beside them.
//! C++ code and the UI process carry records without reading them, so a writer hands what it wrote over to LibWeb's
//! C++ record when it finishes, or to a MessageRecord that Rust keeps.

use core::cell::RefCell;

use ak::{Utf16String, Utf16StringUnits};
use libjs_rust::api::{ArrayBuffer, BigInt, DataBlockStorage, DeferGc, Gc, MarkedVec, Vm};
use libwebcommon_rust::html::serialization_records::{RecordEncoder, SerializationType};

use crate::html::structured_serialize::cpp::{
    IPCSerializationRecord, StorageSerializationRecord, serializable_interface_storage_identifier,
};
use crate::html::structured_serialize::message_record::{MessageRecord, SharedMemory};

/// The record of a structured serialization, which the serialization writes its values to.
pub(crate) struct StructuredSerializeWriter<'vm> {
    encoder: RefCell<RecordEncoder>,
    /// The SharedArrayBuffers that an IPC record shares with a deserialization in the same process, by their index in
    /// this same-process side table.
    shared_array_buffers: MarkedVec<'vm, Gc<ArrayBuffer>>,
    /// The shared memory that a MessageRecord, which may reach agents in other processes, hands over beside its bytes.
    shared_memory: Option<RefCell<Vec<SharedMemory>>>,
}

impl<'vm> StructuredSerializeWriter<'vm> {
    pub(crate) fn new(vm: &'vm Vm, serialization_type: SerializationType) -> Self {
        Self {
            encoder: RefCell::new(RecordEncoder::new(serialization_type)),
            shared_array_buffers: MarkedVec::new(vm),
            shared_memory: None,
        }
    }

    /// The writer of a MessageRecord.
    pub(crate) fn new_for_message(vm: &'vm Vm) -> Self {
        Self {
            shared_memory: Some(RefCell::new(Vec::new())),
            ..Self::new(vm, SerializationType::Ipc)
        }
    }

    pub(crate) fn serialization_type(&self) -> SerializationType {
        self.encoder.borrow().serialization_type()
    }

    pub(crate) fn encode_u8(&self, value: u8) {
        self.encoder.borrow_mut().encode_u8(value);
    }

    pub(crate) fn encode_bool(&self, value: bool) {
        self.encoder.borrow_mut().encode_bool(value);
    }

    pub(crate) fn encode_i32(&self, value: i32) {
        self.encoder.borrow_mut().encode_i32(value);
    }

    pub(crate) fn encode_u16(&self, value: u16) {
        self.encoder.borrow_mut().encode_u16(value);
    }

    pub(crate) fn encode_u32(&self, value: u32) {
        self.encoder.borrow_mut().encode_u32(value);
    }

    pub(crate) fn encode_u64(&self, value: u64) {
        self.encoder.borrow_mut().encode_u64(value);
    }

    pub(crate) fn encode_i64(&self, value: i64) {
        self.encoder.borrow_mut().encode_i64(value);
    }

    pub(crate) fn encode_f64(&self, value: f64) {
        self.encoder.borrow_mut().encode_f64(value);
    }

    pub(crate) fn encode_optional_f64(&self, value: Option<f64>) {
        self.encode_bool(value.is_some());
        if let Some(value) = value {
            self.encode_f64(value);
        }
    }

    pub(crate) fn encode_optional_u64(&self, value: Option<u64>) {
        self.encode_bool(value.is_some());
        if let Some(value) = value {
            self.encode_u64(value);
        }
    }

    pub(crate) fn encode_utf16_string(&self, string: &Utf16String) {
        self.encoder.borrow_mut().encode_utf16_string(string.as_units());
    }

    pub(crate) fn encode_optional_utf16_string(&self, string: Option<&Utf16String>) {
        self.encode_bool(string.is_some());
        if let Some(string) = string {
            self.encode_utf16_string(string);
        }
    }

    pub(crate) fn encode_bytes(&self, bytes: &[u8]) {
        self.encoder.borrow_mut().encode_bytes(bytes);
    }

    /// Bindings::InterfaceName, which a storage record holds as the identifier of the interface.
    pub(crate) fn encode_interface_name(&self, interface_name: u16) {
        match self.serialization_type() {
            SerializationType::Ipc => self.encode_u16(interface_name),
            SerializationType::Storage => {
                let identifier =
                    serializable_interface_storage_identifier(interface_name).expect("the interface is serializable");
                self.encoder
                    .borrow_mut()
                    .encode_utf16_string(Utf16StringUnits::Ascii(identifier));
            }
        }
    }

    pub(crate) fn encode_big_int(&self, big_int: Gc<BigInt>) {
        self.encoder
            .borrow_mut()
            .encode_big_int(big_int.is_negative(), &big_int.magnitude_to_bytes_be());
    }

    /// Puts the SharedArrayBuffer in the record's same-process side table, and returns its index there.
    pub(crate) fn add_shared_array_buffer(&self, array_buffer: Gc<ArrayBuffer>) -> u32 {
        let index = u32::try_from(self.shared_array_buffers.len()).expect("a record shares fewer than 2^32 buffers");
        self.shared_array_buffers.push(array_buffer);
        index
    }

    /// Hands the shared memory over beside the record, and returns its index there.
    pub(crate) fn append_shared_memory(&self, shared_memory: SharedMemory) -> u32 {
        let mut list = self
            .shared_memory
            .as_ref()
            .expect("the record hands shared memory over")
            .borrow_mut();
        let index = u32::try_from(list.len()).expect("a record shares fewer than 2^32 buffers");
        list.push(shared_memory);
        index
    }

    /// The index that the next shared memory handed over beside the record gets.
    pub(crate) fn next_shared_memory_index(&self) -> u32 {
        let list = self
            .shared_memory
            .as_ref()
            .expect("the record hands shared memory over")
            .borrow();
        u32::try_from(list.len()).expect("a record shares fewer than 2^32 buffers")
    }

    /// Hands the cross-process shared memory of the SharedArrayBuffer over beside the record, and returns its index
    /// there, if the buffer is backed by some, and the record reaches agents in other processes.
    pub(crate) fn append_shared_memory_of(&self, array_buffer: Gc<ArrayBuffer>) -> Option<u32> {
        self.shared_memory.as_ref()?;
        let shared_memory = array_buffer.with_data_block(|data_block| match &data_block.byte_buffer {
            DataBlockStorage::Shared(store) => Some(SharedMemory {
                descriptor: store.shared_memory().try_clone_to_owned().ok()?,
                size: store.size(),
            }),
            _ => None,
        })?;
        Some(self.append_shared_memory(shared_memory))
    }

    /// Sets the IPC record that C++ lends to what the writer wrote.
    pub(crate) fn finish_ipc_record(self, record: IPCSerializationRecord) {
        assert_eq!(self.serialization_type(), SerializationType::Ipc);
        record.set(self.encoder.borrow().bytes(), &self.shared_array_buffers.values());
    }

    /// The MessageRecord that the writer wrote. Its SharedArrayBuffers are traced once it is in a cell, which the caller
    /// allocates under the deferral.
    pub(crate) fn finish_message_record(self, _: &DeferGc<'_>) -> MessageRecord {
        assert_eq!(self.serialization_type(), SerializationType::Ipc);
        let shared_memory = self.shared_memory.expect("the writer is a message's");
        MessageRecord {
            bytes: self.encoder.into_inner().into_bytes(),
            shared_array_buffers: self.shared_array_buffers.values().to_vec(),
            shared_memory: shared_memory.into_inner(),
        }
    }

    /// The bytes of the storage record that the writer wrote, for Rust code that keeps them, as IndexedDB does.
    pub(crate) fn into_storage_bytes(self) -> Vec<u8> {
        assert_eq!(self.serialization_type(), SerializationType::Storage);
        assert!(
            self.shared_array_buffers.is_empty(),
            "a storage record shares no SharedArrayBuffer"
        );
        self.encoder.into_inner().into_bytes()
    }

    /// Sets the storage record that C++ lends to what the writer wrote.
    pub(crate) fn finish_storage_record(self, record: StorageSerializationRecord) {
        assert_eq!(self.serialization_type(), SerializationType::Storage);
        assert!(
            self.shared_array_buffers.is_empty(),
            "a storage record shares no SharedArrayBuffer"
        );
        record.set(self.encoder.borrow().bytes());
    }
}
