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
//! C++ record when it finishes.

use core::cell::RefCell;

use ak::{Utf16String, Utf16StringUnits};
use libjs_rust::api::{ArrayBuffer, BigInt, Gc, MarkedVec, Vm};
use libwebcommon_rust::html::serialization_records::{RecordEncoder, SerializationType};

use crate::html::structured_serialize::cpp::{
    IPCSerializationRecord, SerializedTransferRecord, SharedBuffers, StorageSerializationRecord,
    serializable_interface_storage_identifier,
};

/// The record of a structured serialization, which the serialization writes its values to.
pub(crate) struct StructuredSerializeWriter<'vm> {
    encoder: RefCell<RecordEncoder>,
    /// The SharedArrayBuffers that an IPC record shares with a deserialization in the same process, by their index in
    /// this same-process side table.
    shared_array_buffers: MarkedVec<'vm, Gc<ArrayBuffer>>,
    shared_buffers: Option<SharedBuffers>,
}

impl<'vm> StructuredSerializeWriter<'vm> {
    pub(crate) fn new(vm: &'vm Vm, serialization_type: SerializationType) -> Self {
        Self {
            encoder: RefCell::new(RecordEncoder::new(serialization_type)),
            shared_array_buffers: MarkedVec::new(vm),
            shared_buffers: None,
        }
    }

    /// The writer of an IPC record whose SharedArrayBuffers share their cross-process shared memory through the list.
    pub(crate) fn new_with_shared_buffers(vm: &'vm Vm, shared_buffers: SharedBuffers) -> Self {
        Self {
            shared_buffers: Some(shared_buffers),
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
                let identifier = serializable_interface_storage_identifier(interface_name);
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

    /// The side list that the record shares the cross-process shared memory of its SharedArrayBuffers through, if it
    /// has one.
    pub(crate) fn shared_buffers(&self) -> Option<SharedBuffers> {
        self.shared_buffers
    }

    /// Sets the IPC record that C++ lends to what the writer wrote.
    pub(crate) fn finish_ipc_record(self, record: IPCSerializationRecord) {
        assert_eq!(self.serialization_type(), SerializationType::Ipc);
        record.set(self.encoder.borrow().bytes(), &self.shared_array_buffers.values());
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

    /// Sets the [[Serialized]] of the record of a transfer to what the writer wrote.
    pub(crate) fn finish_transfer_record(self, record: SerializedTransferRecord) {
        assert_eq!(self.serialization_type(), SerializationType::Ipc);
        record.set_serialized(self.encoder.borrow().bytes(), &self.shared_array_buffers.values());
    }
}
