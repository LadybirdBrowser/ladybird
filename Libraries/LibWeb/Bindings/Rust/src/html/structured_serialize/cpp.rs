/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What structured serialization asks of LibWeb's C++ code, which implements it in
//! Libraries/LibWeb/HTML/StructuredSerializeRustIntegration.cpp: the records that the serializations write and read,
//! which the serialization steps of the interfaces implemented in C++ write and read as well, the memories of the
//! serializations, the platform objects, and the side tables that SharedArrayBuffers are shared through.

use core::ffi::c_void;
use core::mem::ManuallyDrop;
use core::ptr::NonNull;

use ak::Utf16String;
use libjs_rust::api::{ArrayBuffer, ArrayBufferShared, Gc, Object, RawNativeFunctionResult, Realm, Value};

use crate::bindings::{CallContext, Exception, WebIdlResult};

unsafe extern "C" {
    fn web_structured_serialize_writer_type(writer: *mut c_void) -> u8;
    fn web_structured_serialize_writer_encode_u8(writer: *mut c_void, value: u8);
    fn web_structured_serialize_writer_encode_bool(writer: *mut c_void, value: bool);
    fn web_structured_serialize_writer_encode_i32(writer: *mut c_void, value: i32);
    fn web_structured_serialize_writer_encode_u32(writer: *mut c_void, value: u32);
    fn web_structured_serialize_writer_encode_u64(writer: *mut c_void, value: u64);
    fn web_structured_serialize_writer_encode_f64(writer: *mut c_void, value: f64);
    fn web_structured_serialize_writer_encode_utf16_string(writer: *mut c_void, string: usize);
    fn web_structured_serialize_writer_encode_optional_utf16_string(
        writer: *mut c_void,
        has_value: bool,
        string: usize,
    );
    fn web_structured_serialize_writer_encode_bytes(writer: *mut c_void, bytes: ak::Span<'_, u8>);
    fn web_structured_serialize_writer_encode_interface_name(writer: *mut c_void, interface_name: u16);
    fn web_structured_serialize_writer_encode_big_int(writer: *mut c_void, big_int: u64);
    fn web_structured_serialize_writer_add_shared_array_buffer(writer: *mut c_void, array_buffer: *mut c_void) -> u32;
    fn web_structured_serialize_writer_can_share_array_buffer_memory(
        writer: *mut c_void,
        array_buffer: *mut c_void,
    ) -> bool;
    fn web_structured_serialize_writer_shared_buffer_count(writer: *mut c_void) -> u32;
    fn web_structured_serialize_writer_append_shared_buffer(writer: *mut c_void, array_buffer: *mut c_void);

    fn web_structured_serialize_reader_type(reader: *mut c_void) -> u8;
    fn web_structured_serialize_reader_is_at_end(reader: *mut c_void) -> bool;
    fn web_structured_serialize_reader_decode_u8(
        reader: *mut c_void,
        realm: *mut c_void,
        value: &mut u8,
    ) -> RawNativeFunctionResult;
    fn web_structured_serialize_reader_decode_bool(
        reader: *mut c_void,
        realm: *mut c_void,
        value: &mut bool,
    ) -> RawNativeFunctionResult;
    fn web_structured_serialize_reader_decode_i32(
        reader: *mut c_void,
        realm: *mut c_void,
        value: &mut i32,
    ) -> RawNativeFunctionResult;
    fn web_structured_serialize_reader_decode_u32(
        reader: *mut c_void,
        realm: *mut c_void,
        value: &mut u32,
    ) -> RawNativeFunctionResult;
    fn web_structured_serialize_reader_decode_u64(
        reader: *mut c_void,
        realm: *mut c_void,
        value: &mut u64,
    ) -> RawNativeFunctionResult;
    fn web_structured_serialize_reader_decode_size(
        reader: *mut c_void,
        realm: *mut c_void,
        value: &mut usize,
    ) -> RawNativeFunctionResult;
    fn web_structured_serialize_reader_decode_f64(
        reader: *mut c_void,
        realm: *mut c_void,
        value: &mut f64,
    ) -> RawNativeFunctionResult;
    fn web_structured_serialize_reader_decode_utf16_string(
        reader: *mut c_void,
        realm: *mut c_void,
        string: &mut usize,
    ) -> RawNativeFunctionResult;
    fn web_structured_serialize_reader_decode_optional_utf16_string(
        reader: *mut c_void,
        realm: *mut c_void,
        has_value: &mut bool,
        string: &mut usize,
    ) -> RawNativeFunctionResult;
    fn web_structured_serialize_reader_decode_byte_buffer(
        reader: *mut c_void,
        realm: *mut c_void,
        buffer: &mut *mut c_void,
    ) -> RawNativeFunctionResult;
    fn web_structured_serialize_byte_buffer_size(buffer: *mut c_void) -> usize;
    fn web_structured_serialize_array_buffer_from_byte_buffer(
        realm: *mut c_void,
        buffer: *mut c_void,
        shared: bool,
    ) -> *mut c_void;
    fn web_structured_serialize_byte_buffer_destroy(buffer: *mut c_void);
    fn web_structured_serialize_reader_decode_interface_name(
        reader: *mut c_void,
        realm: *mut c_void,
        interface_name: &mut u16,
    ) -> RawNativeFunctionResult;
    fn web_structured_serialize_reader_decode_big_int(
        reader: *mut c_void,
        realm: *mut c_void,
    ) -> RawNativeFunctionResult;
    fn web_structured_serialize_reader_array_buffer_from_shared_memory(
        reader: *mut c_void,
        realm: *mut c_void,
        index: u32,
        shared_object_id: u64,
    ) -> RawNativeFunctionResult;
    fn web_structured_serialize_reader_array_buffer_sharing_side_table_entry(
        reader: *mut c_void,
        realm: *mut c_void,
        side_table_index: u32,
        byte_length: usize,
        preserve_growability: bool,
    ) -> *mut c_void;
    fn web_structured_serialize_data_clone_error_from_serialization_error(
        realm: *mut c_void,
        message: ak::Span<'_, u8>,
    ) -> RawNativeFunctionResult;

    fn web_structured_serialize_memory_get(memory: *mut c_void, value: u64, index: &mut u32) -> bool;
    fn web_structured_serialize_memory_set(memory: *mut c_void, value: u64, index: u32);
    fn web_structured_serialize_memory_size(memory: *mut c_void) -> u32;
    fn web_structured_serialize_memory_create() -> *mut c_void;
    fn web_structured_serialize_memory_destroy(memory: *mut c_void);
    fn web_structured_serialize_serializable_payload_version() -> u64;
    fn web_structured_deserialize_memory_size(memory: *mut c_void) -> usize;
    fn web_structured_deserialize_memory_append(memory: *mut c_void, value: u64);
    fn web_structured_deserialize_memory_get(memory: *mut c_void, index: usize) -> u64;
    fn web_structured_deserialize_memory_set(memory: *mut c_void, index: usize, value: u64);
    fn web_structured_deserialize_memory_create() -> *mut c_void;
    fn web_structured_deserialize_memory_destroy(memory: *mut c_void);

    fn web_structured_serialize_current_settings_object_agent_cluster(agent_cluster: &mut u64) -> bool;
    fn web_structured_serialize_current_settings_object_cross_origin_isolated_capability() -> bool;
    fn web_structured_serialize_realm_agent_cluster(realm: *mut c_void, agent_cluster: &mut u64) -> bool;

    fn web_structured_serialize_is_platform_object(object: *mut c_void) -> bool;
    fn web_structured_serialize_serializable_interface_name(object: *mut c_void, interface_name: &mut u16) -> bool;
    fn web_structured_serialize_serialization_steps(
        realm: *mut c_void,
        object: *mut c_void,
        writer: *mut c_void,
        for_storage: bool,
        memory: *mut c_void,
    ) -> RawNativeFunctionResult;
    fn web_structured_serialize_is_exposed(interface_name: u16, realm: *mut c_void) -> bool;
    fn web_structured_serialize_serializable_interface_is_implemented_in_rust(interface_name: u16) -> bool;
    fn web_structured_serialize_deserialize_rust_platform_object(
        interface_name: u16,
        realm: *mut c_void,
        reader: *mut c_void,
        memory: *mut c_void,
    ) -> RawNativeFunctionResult;
    fn web_structured_serialize_create_serialized_platform_object(
        interface_name: u16,
        realm: *mut c_void,
    ) -> *mut c_void;
    fn web_structured_serialize_deserialization_steps(
        realm: *mut c_void,
        object: *mut c_void,
        reader: *mut c_void,
        memory: *mut c_void,
    ) -> RawNativeFunctionResult;

    fn web_structured_serialize_is_transferable(object: *mut c_void) -> bool;
    fn web_structured_serialize_transferable_is_detached(object: *mut c_void) -> bool;
    fn web_structured_serialize_transferable_set_detached(object: *mut c_void);
    fn web_structured_serialize_transferable_primary_interface(object: *mut c_void) -> u8;
    fn web_structured_serialize_transfer_steps(
        realm: *mut c_void,
        object: *mut c_void,
        data_holder: *mut c_void,
        image_bitmap_pixels: *mut c_void,
    ) -> RawNativeFunctionResult;
    fn web_structured_serialize_is_transferable_interface_exposed(transfer_type: u8, realm: *mut c_void) -> bool;
    fn web_structured_serialize_create_transferred_platform_object(
        transfer_type: u8,
        realm: *mut c_void,
        decoder: *mut c_void,
    ) -> RawNativeFunctionResult;
    fn web_structured_serialize_gather_image_bitmap_pixels(
        writer: *mut c_void,
        transfer_list: *const *mut c_void,
        count: usize,
    ) -> *mut c_void;
    fn web_structured_serialize_image_bitmap_pixels_destroy(image_bitmap_pixels: *mut c_void);

    fn web_structured_serialize_transfer_data_holder_create() -> *mut c_void;
    fn web_structured_serialize_transfer_data_holder_encode_u8(data_holder: *mut c_void, value: u8);
    fn web_structured_serialize_transfer_data_holder_encode_bytes(data_holder: *mut c_void, bytes: ak::Span<'_, u8>);
    fn web_structured_serialize_transfer_data_holder_encode_size(data_holder: *mut c_void, value: usize);
    fn web_structured_serialize_transfer_data_holder_destroy(data_holder: *mut c_void);
    fn web_structured_serialize_transfer_record_append_data_holder(record: *mut c_void, data_holder: *mut c_void);
    fn web_structured_serialize_transfer_record_data_holder_count(record: *mut c_void) -> usize;
    fn web_structured_serialize_transfer_record_take_data_holder(record: *mut c_void, index: usize) -> *mut c_void;
    fn web_structured_serialize_transfer_record_deserialize(
        record: *mut c_void,
        realm: *mut c_void,
        memory: *mut c_void,
    ) -> RawNativeFunctionResult;
    fn web_structured_serialize_transfer_record_clear_shared_buffers(record: *mut c_void);
    fn web_structured_serialize_transferred_values_append(transferred_values: *mut c_void, value: *mut c_void);
    fn web_structured_serialize_transfer_data_decoder_decode_u8(
        decoder: *mut c_void,
        realm: *mut c_void,
        value: &mut u8,
    ) -> RawNativeFunctionResult;
    fn web_structured_serialize_transfer_data_decoder_decode_byte_buffer(
        decoder: *mut c_void,
        realm: *mut c_void,
        buffer: &mut *mut c_void,
    ) -> RawNativeFunctionResult;
    fn web_structured_serialize_transfer_data_decoder_decode_size(
        decoder: *mut c_void,
        realm: *mut c_void,
        value: &mut usize,
    ) -> RawNativeFunctionResult;
    fn web_structured_serialize_transfer_data_decoder_destroy(decoder: *mut c_void);
}

/// HTML::SerializationType.
#[derive(Clone, Copy, PartialEq, Eq)]
pub(crate) enum SerializationType {
    Ipc,
    Storage,
}

impl SerializationType {
    fn from_abi(value: u8) -> Self {
        match value {
            0 => Self::Ipc,
            1 => Self::Storage,
            _ => unreachable!("LibWeb has no other serialization type"),
        }
    }
}

fn object_into_abi<T>(object: Gc<T>) -> *mut c_void {
    object.as_ptr().cast()
}

/// # Safety
///
/// `object` must be a live object or null.
unsafe fn object_from_abi(object: *mut c_void) -> Option<Gc<Object>> {
    // SAFETY: The caller passes a live object or null.
    NonNull::new(object.cast::<Object>()).map(|object| unsafe { Gc::from_non_null(object) })
}

fn completion(result: RawNativeFunctionResult) -> WebIdlResult<Value> {
    Ok(result.into_completion()?)
}

/// The bytes of a record, which C++ decoded into a buffer that this owns until an ArrayBuffer takes it over.
pub(crate) struct DecodedBytes(NonNull<c_void>);

impl DecodedBytes {
    pub(crate) fn len(&self) -> usize {
        // SAFETY: This owns the buffer.
        unsafe { web_structured_serialize_byte_buffer_size(self.0.as_ptr()) }
    }

    /// A new ArrayBuffer in the realm, which takes over the bytes.
    pub(crate) fn into_array_buffer(self, realm: Gc<Realm>, is_shared: ArrayBufferShared) -> Gc<ArrayBuffer> {
        let buffer = ManuallyDrop::new(self);
        // SAFETY: The realm is live, and the call takes over the buffer, which LibWeb returns an ArrayBuffer over.
        let object = unsafe {
            web_structured_serialize_array_buffer_from_byte_buffer(
                realm.as_ptr().cast(),
                buffer.0.as_ptr(),
                matches!(is_shared, ArrayBufferShared::Yes),
            )
        };
        let object = NonNull::new(object.cast::<Object>()).expect("LibWeb returns an ArrayBuffer");
        // SAFETY: LibWeb returns a live ArrayBuffer.
        unsafe { Gc::from_non_null(object) }
            .downcast::<ArrayBuffer>()
            .expect("LibWeb returns an ArrayBuffer")
    }
}

impl Drop for DecodedBytes {
    fn drop(&mut self) {
        // SAFETY: This owns the buffer.
        unsafe { web_structured_serialize_byte_buffer_destroy(self.0.as_ptr()) };
    }
}

/// The record that a structured serialization writes: an HTML::StructuredSerializeWriter that C++ lends.
#[derive(Clone, Copy)]
pub(crate) struct StructuredSerializeWriter(NonNull<c_void>);

impl StructuredSerializeWriter {
    /// # Safety
    ///
    /// `writer` must be a writer that C++ lends while this is used.
    pub(crate) unsafe fn from_raw(writer: *mut c_void) -> Self {
        Self(NonNull::new(writer).expect("C++ lends a writer"))
    }

    pub(crate) fn as_raw(self) -> *mut c_void {
        self.0.as_ptr()
    }

    pub(crate) fn serialization_type(self) -> SerializationType {
        // SAFETY: The writer is lent.
        SerializationType::from_abi(unsafe { web_structured_serialize_writer_type(self.as_raw()) })
    }

    pub(crate) fn encode_u8(self, value: u8) {
        // SAFETY: The writer is lent.
        unsafe { web_structured_serialize_writer_encode_u8(self.as_raw(), value) };
    }

    pub(crate) fn encode_bool(self, value: bool) {
        // SAFETY: The writer is lent.
        unsafe { web_structured_serialize_writer_encode_bool(self.as_raw(), value) };
    }

    pub(crate) fn encode_i32(self, value: i32) {
        // SAFETY: The writer is lent.
        unsafe { web_structured_serialize_writer_encode_i32(self.as_raw(), value) };
    }

    pub(crate) fn encode_u32(self, value: u32) {
        // SAFETY: The writer is lent.
        unsafe { web_structured_serialize_writer_encode_u32(self.as_raw(), value) };
    }

    pub(crate) fn encode_u64(self, value: u64) {
        // SAFETY: The writer is lent.
        unsafe { web_structured_serialize_writer_encode_u64(self.as_raw(), value) };
    }

    pub(crate) fn encode_f64(self, value: f64) {
        // SAFETY: The writer is lent.
        unsafe { web_structured_serialize_writer_encode_f64(self.as_raw(), value) };
    }

    pub(crate) fn encode_utf16_string(self, string: &Utf16String) {
        // SAFETY: The writer is lent, and the call borrows the string.
        unsafe { web_structured_serialize_writer_encode_utf16_string(self.as_raw(), string.raw_identity()) };
    }

    pub(crate) fn encode_optional_utf16_string(self, string: Option<&Utf16String>) {
        // SAFETY: The writer is lent, and the call borrows the string.
        unsafe {
            web_structured_serialize_writer_encode_optional_utf16_string(
                self.as_raw(),
                string.is_some(),
                string.map_or(0, Utf16String::raw_identity),
            );
        }
    }

    pub(crate) fn encode_bytes(self, bytes: &[u8]) {
        // SAFETY: The writer is lent, and the call borrows the bytes.
        unsafe { web_structured_serialize_writer_encode_bytes(self.as_raw(), ak::Span::new(bytes)) };
    }

    /// Bindings::InterfaceName, which a storage record holds as the identifier of the interface.
    pub(crate) fn encode_interface_name(self, interface_name: u16) {
        // SAFETY: The writer is lent.
        unsafe { web_structured_serialize_writer_encode_interface_name(self.as_raw(), interface_name) };
    }

    /// The value of a BigInt, as encode_signed_big_integer() writes it.
    pub(crate) fn encode_big_int(self, big_int: Value) {
        // SAFETY: The writer is lent, and the BigInt is live.
        unsafe { web_structured_serialize_writer_encode_big_int(self.as_raw(), big_int.to_raw()) };
    }

    /// Puts the SharedArrayBuffer in the record's same-process side table, and returns its index there.
    pub(crate) fn add_shared_array_buffer(self, array_buffer: Gc<ArrayBuffer>) -> u32 {
        // SAFETY: The writer is lent, and the buffer is live.
        unsafe { web_structured_serialize_writer_add_shared_array_buffer(self.as_raw(), object_into_abi(array_buffer)) }
    }

    /// Whether the SharedArrayBuffer is backed by cross-process shared memory, and the writer has a side list for it.
    pub(crate) fn can_share_array_buffer_memory(self, array_buffer: Gc<ArrayBuffer>) -> bool {
        // SAFETY: The writer is lent, and the buffer is live.
        unsafe {
            web_structured_serialize_writer_can_share_array_buffer_memory(self.as_raw(), object_into_abi(array_buffer))
        }
    }

    pub(crate) fn shared_buffer_count(self) -> u32 {
        // SAFETY: The writer is lent.
        unsafe { web_structured_serialize_writer_shared_buffer_count(self.as_raw()) }
    }

    /// Appends the cross-process shared memory of the SharedArrayBuffer to the writer's side list.
    pub(crate) fn append_shared_buffer(self, array_buffer: Gc<ArrayBuffer>) {
        // SAFETY: The writer is lent, and the buffer is live.
        unsafe { web_structured_serialize_writer_append_shared_buffer(self.as_raw(), object_into_abi(array_buffer)) };
    }

    /// Gathers the pixels of the ImageBitmaps of the transfer list in one buffer of shared memory, which the writer's
    /// side list holds.
    pub(crate) fn gather_image_bitmap_pixels(self, transfer_list: &[Gc<Object>]) -> ImageBitmapSharedPixels {
        let transfer_list: Vec<*mut c_void> = transfer_list.iter().map(|object| object_into_abi(*object)).collect();
        // SAFETY: The writer is lent, and the objects are live.
        ImageBitmapSharedPixels(NonNull::new(unsafe {
            web_structured_serialize_gather_image_bitmap_pixels(
                self.as_raw(),
                transfer_list.as_ptr(),
                transfer_list.len(),
            )
        }))
    }
}

/// The record that a structured deserialization reads: an HTML::StructuredSerializeReader that C++ lends. What it
/// reads that the record does not hold throws a "DataCloneError" DOMException in the realm.
#[derive(Clone, Copy)]
pub(crate) struct StructuredSerializeReader {
    raw: NonNull<c_void>,
    realm: Gc<Realm>,
}

impl StructuredSerializeReader {
    /// # Safety
    ///
    /// `reader` must be a reader that C++ lends while this is used.
    pub(crate) unsafe fn from_raw(reader: *mut c_void, realm: Gc<Realm>) -> Self {
        Self {
            raw: NonNull::new(reader).expect("C++ lends a reader"),
            realm,
        }
    }

    pub(crate) fn as_raw(self) -> *mut c_void {
        self.raw.as_ptr()
    }

    fn realm_raw(self) -> *mut c_void {
        self.realm.as_ptr().cast()
    }

    pub(crate) fn serialization_type(self) -> SerializationType {
        // SAFETY: The reader is lent.
        SerializationType::from_abi(unsafe { web_structured_serialize_reader_type(self.as_raw()) })
    }

    pub(crate) fn is_at_end(self) -> bool {
        // SAFETY: The reader is lent.
        unsafe { web_structured_serialize_reader_is_at_end(self.as_raw()) }
    }

    pub(crate) fn decode_u8(self) -> WebIdlResult<u8> {
        let mut value = 0;
        // SAFETY: The reader is lent, and the realm is live.
        completion(unsafe { web_structured_serialize_reader_decode_u8(self.as_raw(), self.realm_raw(), &mut value) })?;
        Ok(value)
    }

    pub(crate) fn decode_bool(self) -> WebIdlResult<bool> {
        let mut value = false;
        // SAFETY: The reader is lent, and the realm is live.
        completion(unsafe {
            web_structured_serialize_reader_decode_bool(self.as_raw(), self.realm_raw(), &mut value)
        })?;
        Ok(value)
    }

    pub(crate) fn decode_i32(self) -> WebIdlResult<i32> {
        let mut value = 0;
        // SAFETY: The reader is lent, and the realm is live.
        completion(unsafe { web_structured_serialize_reader_decode_i32(self.as_raw(), self.realm_raw(), &mut value) })?;
        Ok(value)
    }

    pub(crate) fn decode_u32(self) -> WebIdlResult<u32> {
        let mut value = 0;
        // SAFETY: The reader is lent, and the realm is live.
        completion(unsafe { web_structured_serialize_reader_decode_u32(self.as_raw(), self.realm_raw(), &mut value) })?;
        Ok(value)
    }

    pub(crate) fn decode_u64(self) -> WebIdlResult<u64> {
        let mut value = 0;
        // SAFETY: The reader is lent, and the realm is live.
        completion(unsafe { web_structured_serialize_reader_decode_u64(self.as_raw(), self.realm_raw(), &mut value) })?;
        Ok(value)
    }

    pub(crate) fn decode_size(self) -> WebIdlResult<usize> {
        let mut value = 0;
        // SAFETY: The reader is lent, and the realm is live.
        completion(unsafe {
            web_structured_serialize_reader_decode_size(self.as_raw(), self.realm_raw(), &mut value)
        })?;
        Ok(value)
    }

    pub(crate) fn decode_f64(self) -> WebIdlResult<f64> {
        let mut value = 0.0;
        // SAFETY: The reader is lent, and the realm is live.
        completion(unsafe { web_structured_serialize_reader_decode_f64(self.as_raw(), self.realm_raw(), &mut value) })?;
        Ok(value)
    }

    pub(crate) fn decode_utf16_string(self) -> WebIdlResult<Utf16String> {
        let mut string = 0;
        // SAFETY: The reader is lent, and the realm is live.
        let result = unsafe {
            web_structured_serialize_reader_decode_utf16_string(self.as_raw(), self.realm_raw(), &mut string)
        };
        // SAFETY: LibWeb hands over the string it read, which is empty if it read none.
        let string = unsafe { Utf16String::from_raw_owned(string) };
        completion(result)?;
        Ok(string)
    }

    pub(crate) fn decode_optional_utf16_string(self) -> WebIdlResult<Option<Utf16String>> {
        let mut has_value = false;
        let mut string = 0;
        // SAFETY: The reader is lent, and the realm is live.
        let result = unsafe {
            web_structured_serialize_reader_decode_optional_utf16_string(
                self.as_raw(),
                self.realm_raw(),
                &mut has_value,
                &mut string,
            )
        };
        // SAFETY: LibWeb hands over the string it read, if it read one.
        let string = has_value.then(|| unsafe { Utf16String::from_raw_owned(string) });
        completion(result)?;
        Ok(string)
    }

    pub(crate) fn decode_byte_buffer(self) -> WebIdlResult<DecodedBytes> {
        let mut buffer = core::ptr::null_mut();
        // SAFETY: The reader is lent, the realm is live, and the buffer outlives the call.
        completion(unsafe {
            web_structured_serialize_reader_decode_byte_buffer(self.as_raw(), self.realm_raw(), &mut buffer)
        })?;
        Ok(DecodedBytes(NonNull::new(buffer).expect("LibWeb decodes a buffer")))
    }

    pub(crate) fn decode_interface_name(self) -> WebIdlResult<u16> {
        let mut interface_name = 0;
        // SAFETY: The reader is lent, and the realm is live.
        completion(unsafe {
            web_structured_serialize_reader_decode_interface_name(self.as_raw(), self.realm_raw(), &mut interface_name)
        })?;
        Ok(interface_name)
    }

    /// A new BigInt with the value that decode_signed_big_integer() reads.
    pub(crate) fn decode_big_int(self) -> WebIdlResult<Value> {
        // SAFETY: The reader is lent, and the realm is live.
        completion(unsafe { web_structured_serialize_reader_decode_big_int(self.as_raw(), self.realm_raw()) })
    }

    /// A new SharedArrayBuffer over the cross-process shared memory at the index of the reader's side list.
    pub(crate) fn array_buffer_from_shared_memory(self, index: u32, shared_object_id: u64) -> WebIdlResult<Value> {
        // SAFETY: The reader is lent, and the realm is live.
        completion(unsafe {
            web_structured_serialize_reader_array_buffer_from_shared_memory(
                self.as_raw(),
                self.realm_raw(),
                index,
                shared_object_id,
            )
        })
    }

    /// A new SharedArrayBuffer that shares the storage of the buffer at the index of the record's same-process side
    /// table, if there is such a buffer, and its storage can be shared.
    pub(crate) fn array_buffer_sharing_side_table_entry(
        self,
        side_table_index: u32,
        byte_length: usize,
        preserve_growability: bool,
    ) -> Option<Gc<Object>> {
        // SAFETY: The reader is lent, and the realm is live. LibWeb returns a live object or null.
        unsafe {
            object_from_abi(web_structured_serialize_reader_array_buffer_sharing_side_table_entry(
                self.as_raw(),
                self.realm_raw(),
                side_table_index,
                byte_length,
                preserve_growability,
            ))
        }
    }

    /// The realm that the deserialization creates its values in.
    pub(crate) fn realm(self) -> Gc<Realm> {
        self.realm
    }
}

/// data_clone_error_from_serialization_error(): the "DataCloneError" DOMException in the realm for a record that does
/// not hold what deserializing it reads.
pub(crate) fn data_clone_error_from_serialization_error(realm: Gc<Realm>, message: &str) -> Exception {
    // SAFETY: The realm is live, and the message is lent for the call.
    let result = completion(unsafe {
        web_structured_serialize_data_clone_error_from_serialization_error(
            realm.as_ptr().cast(),
            ak::Span::new(message.as_bytes()),
        )
    });
    match result {
        Err(exception) => exception,
        Ok(_) => unreachable!("the helper throws"),
    }
}

/// The memory of a structured serialization: an HTML::SerializationMemory that C++ lends, which maps the values that
/// the serialization serialized to their positions.
#[derive(Clone, Copy)]
pub(crate) struct SerializationMemory(NonNull<c_void>);

impl SerializationMemory {
    /// # Safety
    ///
    /// `memory` must be a memory that C++ lends while this is used.
    pub(crate) unsafe fn from_raw(memory: *mut c_void) -> Self {
        Self(NonNull::new(memory).expect("C++ lends a memory"))
    }

    pub(crate) fn as_raw(self) -> *mut c_void {
        self.0.as_ptr()
    }

    pub(crate) fn get(self, value: Value) -> Option<u32> {
        let mut index = 0;
        // SAFETY: The memory is lent.
        unsafe { web_structured_serialize_memory_get(self.as_raw(), value.to_raw(), &mut index) }.then_some(index)
    }

    pub(crate) fn contains(self, value: Value) -> bool {
        self.get(value).is_some()
    }

    pub(crate) fn set(self, value: Value, index: u32) {
        // SAFETY: The memory is lent, and it roots the value.
        unsafe { web_structured_serialize_memory_set(self.as_raw(), value.to_raw(), index) };
    }

    pub(crate) fn size(self) -> u32 {
        // SAFETY: The memory is lent.
        unsafe { web_structured_serialize_memory_size(self.as_raw()) }
    }
}

/// A memory of a structured serialization that this owns.
pub(crate) struct OwnedSerializationMemory(SerializationMemory);

impl OwnedSerializationMemory {
    pub(crate) fn new() -> Self {
        // SAFETY: LibWeb returns a new memory, which this owns.
        Self(unsafe { SerializationMemory::from_raw(web_structured_serialize_memory_create()) })
    }

    pub(crate) fn memory(&self) -> SerializationMemory {
        self.0
    }
}

impl Drop for OwnedSerializationMemory {
    fn drop(&mut self) {
        // SAFETY: This owns the memory.
        unsafe { web_structured_serialize_memory_destroy(self.0.as_raw()) };
    }
}

/// HTML::serializable_payload_version: the per-interface payload version that a storage record holds after the
/// interface name of every serializable object.
pub(crate) fn serializable_payload_version() -> u64 {
    // SAFETY: The call has no arguments.
    unsafe { web_structured_serialize_serializable_payload_version() }
}

/// The memory of a structured deserialization: an HTML::DeserializationMemory, which holds the values that the
/// deserialization created at their positions.
#[derive(Clone, Copy)]
pub(crate) struct DeserializationMemory(NonNull<c_void>);

impl DeserializationMemory {
    /// # Safety
    ///
    /// `memory` must be a memory that C++ lends while this is used.
    pub(crate) unsafe fn from_raw(memory: *mut c_void) -> Self {
        Self(NonNull::new(memory).expect("C++ lends a memory"))
    }

    pub(crate) fn as_raw(self) -> *mut c_void {
        self.0.as_ptr()
    }

    pub(crate) fn size(self) -> usize {
        // SAFETY: The memory is lent.
        unsafe { web_structured_deserialize_memory_size(self.as_raw()) }
    }

    pub(crate) fn append(self, value: Value) {
        // SAFETY: The memory is lent, and it roots the value.
        unsafe { web_structured_deserialize_memory_append(self.as_raw(), value.to_raw()) };
    }

    pub(crate) fn get(self, index: usize) -> Value {
        // SAFETY: The memory is lent, and it holds live values.
        unsafe { Value::from_raw(web_structured_deserialize_memory_get(self.as_raw(), index)) }
    }

    pub(crate) fn set(self, index: usize, value: Value) {
        // SAFETY: The memory is lent, and it roots the value.
        unsafe { web_structured_deserialize_memory_set(self.as_raw(), index, value.to_raw()) };
    }
}

/// A memory of a structured deserialization that this owns.
pub(crate) struct OwnedDeserializationMemory(DeserializationMemory);

impl OwnedDeserializationMemory {
    pub(crate) fn new() -> Self {
        // SAFETY: LibWeb returns a new memory, which this owns.
        Self(unsafe { DeserializationMemory::from_raw(web_structured_deserialize_memory_create()) })
    }

    pub(crate) fn memory(&self) -> DeserializationMemory {
        self.0
    }
}

impl Drop for OwnedDeserializationMemory {
    fn drop(&mut self) {
        // SAFETY: This owns the memory.
        unsafe { web_structured_deserialize_memory_destroy(self.0.as_raw()) };
    }
}

/// The agent cluster of the current settings object, if it has one.
pub(crate) fn current_settings_object_agent_cluster() -> Option<u64> {
    let mut agent_cluster = 0;
    // SAFETY: The call writes to the local.
    unsafe { web_structured_serialize_current_settings_object_agent_cluster(&mut agent_cluster) }
        .then_some(agent_cluster)
}

/// The current settings object's cross-origin isolated capability.
pub(crate) fn current_settings_object_cross_origin_isolated_capability() -> bool {
    // SAFETY: The call has no arguments.
    unsafe { web_structured_serialize_current_settings_object_cross_origin_isolated_capability() }
}

/// The agent cluster of the environment settings object of the realm, if it has one.
pub(crate) fn realm_agent_cluster(realm: Gc<Realm>) -> Option<u64> {
    let mut agent_cluster = 0;
    // SAFETY: The realm is live.
    unsafe { web_structured_serialize_realm_agent_cluster(realm.as_ptr().cast(), &mut agent_cluster) }
        .then_some(agent_cluster)
}

/// https://webidl.spec.whatwg.org/#dfn-platform-object
pub(crate) fn is_platform_object(object: Gc<Object>) -> bool {
    // SAFETY: The object is live.
    unsafe { web_structured_serialize_is_platform_object(object_into_abi(object)) }
}

/// The identifier of the primary interface of the object, a Bindings::InterfaceName, if it is a platform object that
/// is a serializable object.
pub(crate) fn serializable_interface_name(object: Gc<Object>) -> Option<u16> {
    let mut interface_name = 0;
    // SAFETY: The object is live.
    unsafe { web_structured_serialize_serializable_interface_name(object_into_abi(object), &mut interface_name) }
        .then_some(interface_name)
}

/// The serialization steps of the primary interface of the serializable platform object.
pub(crate) fn serialization_steps(
    cx: &CallContext,
    object: Gc<Object>,
    serialized: StructuredSerializeWriter,
    for_storage: bool,
    memory: SerializationMemory,
) -> WebIdlResult<()> {
    // SAFETY: The realm and the object are live, and the writer and the memory are lent.
    completion(unsafe {
        web_structured_serialize_serialization_steps(
            cx.realm().as_ptr().cast(),
            object_into_abi(object),
            serialized.as_raw(),
            for_storage,
            memory.as_raw(),
        )
    })?;
    Ok(())
}

/// https://webidl.spec.whatwg.org/#dfn-exposed
pub(crate) fn is_exposed(interface_name: u16, realm: Gc<Realm>) -> bool {
    // SAFETY: The realm is live.
    unsafe { web_structured_serialize_is_exposed(interface_name, realm.as_ptr().cast()) }
}

pub(crate) fn serializable_interface_is_implemented_in_rust(interface_name: u16) -> bool {
    // SAFETY: The call takes a value.
    unsafe { web_structured_serialize_serializable_interface_is_implemented_in_rust(interface_name) }
}

/// The deserialization steps of an interface implemented in Rust, which create the new instance from what they read.
pub(crate) fn deserialize_rust_platform_object(
    interface_name: u16,
    serialized: StructuredSerializeReader,
    memory: DeserializationMemory,
) -> WebIdlResult<Value> {
    // SAFETY: The realm is live, and the reader and the memory are lent.
    completion(unsafe {
        web_structured_serialize_deserialize_rust_platform_object(
            interface_name,
            serialized.realm_raw(),
            serialized.as_raw(),
            memory.as_raw(),
        )
    })
}

/// A new instance of the interface implemented in C++, created in the realm, which its deserialization steps fill in.
pub(crate) fn create_serialized_platform_object(interface_name: u16, realm: Gc<Realm>) -> Gc<Object> {
    // SAFETY: The realm is live, and LibWeb returns a live object.
    unsafe {
        object_from_abi(web_structured_serialize_create_serialized_platform_object(
            interface_name,
            realm.as_ptr().cast(),
        ))
    }
    .expect("LibWeb creates the instance")
}

/// The deserialization steps of the interface of the instance, given the record and the realm.
pub(crate) fn deserialization_steps(
    object: Gc<Object>,
    serialized: StructuredSerializeReader,
    memory: DeserializationMemory,
) -> WebIdlResult<()> {
    // SAFETY: The realm and the object are live, and the reader and the memory are lent.
    completion(unsafe {
        web_structured_serialize_deserialization_steps(
            serialized.realm_raw(),
            object_into_abi(object),
            serialized.as_raw(),
            memory.as_raw(),
        )
    })?;
    Ok(())
}

/// Whether the object is a platform object that is a transferable object, which has a [[Detached]] internal slot.
pub(crate) fn is_transferable(object: Gc<Object>) -> bool {
    // SAFETY: The object is live.
    unsafe { web_structured_serialize_is_transferable(object_into_abi(object)) }
}

/// The [[Detached]] internal slot of the transferable platform object.
pub(crate) fn transferable_is_detached(object: Gc<Object>) -> bool {
    // SAFETY: The object is live.
    unsafe { web_structured_serialize_transferable_is_detached(object_into_abi(object)) }
}

pub(crate) fn transferable_set_detached(object: Gc<Object>) {
    // SAFETY: The object is live.
    unsafe { web_structured_serialize_transferable_set_detached(object_into_abi(object)) };
}

/// The identifier of the primary interface of the transferable platform object, an HTML::TransferType.
pub(crate) fn transferable_primary_interface(object: Gc<Object>) -> u8 {
    // SAFETY: The object is live.
    unsafe { web_structured_serialize_transferable_primary_interface(object_into_abi(object)) }
}

/// The transfer steps of the interface of the transferable platform object, given it and the data holder.
pub(crate) fn transfer_steps(
    cx: &CallContext,
    object: Gc<Object>,
    data_holder: &TransferDataHolder,
    image_bitmap_pixels: &ImageBitmapSharedPixels,
) -> WebIdlResult<()> {
    // SAFETY: The realm and the object are live, and the data holder and the pixels are live while borrowed.
    completion(unsafe {
        web_structured_serialize_transfer_steps(
            cx.realm().as_ptr().cast(),
            object_into_abi(object),
            data_holder.0.as_ptr(),
            image_bitmap_pixels.0.map_or(core::ptr::null_mut(), NonNull::as_ptr),
        )
    })?;
    Ok(())
}

/// Whether the interface that the HTML::TransferType identifies is exposed in the realm.
pub(crate) fn is_transferable_interface_exposed(transfer_type: u8, realm: Gc<Realm>) -> bool {
    // SAFETY: The realm is live.
    unsafe { web_structured_serialize_is_transferable_interface_exposed(transfer_type, realm.as_ptr().cast()) }
}

/// A new instance of the interface that the HTML::TransferType identifies, created in the realm, after the
/// transfer-receiving steps of the interface given the data holder.
pub(crate) fn create_transferred_platform_object(
    transfer_type: u8,
    realm: Gc<Realm>,
    decoder: TransferDataDecoder,
) -> WebIdlResult<Value> {
    // SAFETY: The realm is live, and the decoder is lent.
    completion(unsafe {
        web_structured_serialize_create_transferred_platform_object(
            transfer_type,
            realm.as_ptr().cast(),
            decoder.0.as_ptr(),
        )
    })
}

/// HTML::ImageBitmapSharedPixels that this owns: the shared memory that a transfer gathers the pixels of its
/// ImageBitmaps in, if it has some.
pub(crate) struct ImageBitmapSharedPixels(Option<NonNull<c_void>>);

impl Drop for ImageBitmapSharedPixels {
    fn drop(&mut self) {
        if let Some(image_bitmap_pixels) = self.0 {
            // SAFETY: This owns the pixels.
            unsafe { web_structured_serialize_image_bitmap_pixels_destroy(image_bitmap_pixels.as_ptr()) };
        }
    }
}

/// A data holder of a transfer that this owns: an HTML::TransferDataEncoder.
pub(crate) struct TransferDataHolder(NonNull<c_void>);

impl TransferDataHolder {
    pub(crate) fn new() -> Self {
        // SAFETY: LibWeb returns a new data holder, which this owns.
        Self(
            NonNull::new(unsafe { web_structured_serialize_transfer_data_holder_create() }).expect("LibWeb creates it"),
        )
    }

    pub(crate) fn encode_u8(&self, value: u8) {
        // SAFETY: This owns the data holder.
        unsafe { web_structured_serialize_transfer_data_holder_encode_u8(self.0.as_ptr(), value) };
    }

    pub(crate) fn encode_bytes(&self, bytes: &[u8]) {
        // SAFETY: This owns the data holder, and the call borrows the bytes.
        unsafe { web_structured_serialize_transfer_data_holder_encode_bytes(self.0.as_ptr(), ak::Span::new(bytes)) };
    }

    pub(crate) fn encode_size(&self, value: usize) {
        // SAFETY: This owns the data holder.
        unsafe { web_structured_serialize_transfer_data_holder_encode_size(self.0.as_ptr(), value) };
    }
}

impl Drop for TransferDataHolder {
    fn drop(&mut self) {
        // SAFETY: This owns the data holder.
        unsafe { web_structured_serialize_transfer_data_holder_destroy(self.0.as_ptr()) };
    }
}

/// An HTML::SerializedTransferRecord that C++ lends.
#[derive(Clone, Copy)]
pub(crate) struct SerializedTransferRecord(NonNull<c_void>);

impl SerializedTransferRecord {
    /// # Safety
    ///
    /// `record` must be a record that C++ lends while this is used.
    pub(crate) unsafe fn from_raw(record: *mut c_void) -> Self {
        Self(NonNull::new(record).expect("C++ lends a record"))
    }

    /// Appends the data holder to the record's [[TransferDataHolders]].
    pub(crate) fn append_data_holder(self, data_holder: TransferDataHolder) {
        let data_holder = core::mem::ManuallyDrop::new(data_holder);
        // SAFETY: The record is lent, and LibWeb takes the data holder.
        unsafe { web_structured_serialize_transfer_record_append_data_holder(self.0.as_ptr(), data_holder.0.as_ptr()) };
    }

    pub(crate) fn data_holder_count(self) -> usize {
        // SAFETY: The record is lent.
        unsafe { web_structured_serialize_transfer_record_data_holder_count(self.0.as_ptr()) }
    }

    /// A decoder of the data holder at the index, which takes it from the record, unless the data holder is empty.
    pub(crate) fn take_data_holder(self, index: usize) -> Option<OwnedTransferDataDecoder> {
        // SAFETY: The record is lent, and LibWeb returns a decoder that this owns, or null.
        NonNull::new(unsafe { web_structured_serialize_transfer_record_take_data_holder(self.0.as_ptr(), index) })
            .map(|decoder| OwnedTransferDataDecoder(TransferDataDecoder(decoder)))
    }

    /// StructuredDeserialize of the record's [[Serialized]], with its shared memory.
    pub(crate) fn deserialize(self, realm: Gc<Realm>, memory: DeserializationMemory) -> WebIdlResult<Value> {
        // SAFETY: The record and the memory are lent, and the realm is live.
        completion(unsafe {
            web_structured_serialize_transfer_record_deserialize(
                self.0.as_ptr(),
                realm.as_ptr().cast(),
                memory.as_raw(),
            )
        })
    }

    pub(crate) fn clear_shared_buffers(self) {
        // SAFETY: The record is lent.
        unsafe { web_structured_serialize_transfer_record_clear_shared_buffers(self.0.as_ptr()) };
    }
}

/// The list of transferred values of a structured deserialization with transfer: a Vector<GC::Root<JS::Object>> that
/// C++ lends.
#[derive(Clone, Copy)]
pub(crate) struct TransferredValues(NonNull<c_void>);

impl TransferredValues {
    /// # Safety
    ///
    /// `transferred_values` must be a list that C++ lends while this is used.
    pub(crate) unsafe fn from_raw(transferred_values: *mut c_void) -> Self {
        Self(NonNull::new(transferred_values).expect("C++ lends a list"))
    }

    pub(crate) fn append(self, value: Gc<Object>) {
        // SAFETY: The list is lent, and the value is live.
        unsafe { web_structured_serialize_transferred_values_append(self.0.as_ptr(), object_into_abi(value)) };
    }
}

/// The decoder of a received data holder: an HTML::TransferDataDecoder that C++ lends.
#[derive(Clone, Copy)]
pub(crate) struct TransferDataDecoder(NonNull<c_void>);

impl TransferDataDecoder {
    /// # Safety
    ///
    /// `decoder` must be a decoder that C++ lends while this is used.
    pub(crate) unsafe fn from_raw(decoder: *mut c_void) -> Self {
        Self(NonNull::new(decoder).expect("C++ lends a decoder"))
    }

    pub(crate) fn decode_u8(self, realm: Gc<Realm>) -> WebIdlResult<u8> {
        let mut value = 0;
        // SAFETY: The decoder is lent, and the realm is live.
        completion(unsafe {
            web_structured_serialize_transfer_data_decoder_decode_u8(self.0.as_ptr(), realm.as_ptr().cast(), &mut value)
        })?;
        Ok(value)
    }

    pub(crate) fn decode_byte_buffer(self, realm: Gc<Realm>) -> WebIdlResult<DecodedBytes> {
        let mut buffer = core::ptr::null_mut();
        // SAFETY: The decoder is lent, the realm is live, and the buffer outlives the call.
        completion(unsafe {
            web_structured_serialize_transfer_data_decoder_decode_byte_buffer(
                self.0.as_ptr(),
                realm.as_ptr().cast(),
                &mut buffer,
            )
        })?;
        Ok(DecodedBytes(NonNull::new(buffer).expect("LibWeb decodes a buffer")))
    }

    pub(crate) fn decode_size(self, realm: Gc<Realm>) -> WebIdlResult<usize> {
        let mut value = 0;
        // SAFETY: The decoder is lent, and the realm is live.
        completion(unsafe {
            web_structured_serialize_transfer_data_decoder_decode_size(
                self.0.as_ptr(),
                realm.as_ptr().cast(),
                &mut value,
            )
        })?;
        Ok(value)
    }
}

/// A decoder of a received data holder that this owns.
pub(crate) struct OwnedTransferDataDecoder(TransferDataDecoder);

impl OwnedTransferDataDecoder {
    pub(crate) fn decoder(&self) -> TransferDataDecoder {
        self.0
    }
}

impl Drop for OwnedTransferDataDecoder {
    fn drop(&mut self) {
        // SAFETY: This owns the decoder.
        unsafe { web_structured_serialize_transfer_data_decoder_destroy(self.0.0.as_ptr()) };
    }
}
