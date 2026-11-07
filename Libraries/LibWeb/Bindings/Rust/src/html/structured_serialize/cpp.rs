/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What structured serialization asks of LibWeb's C++ code, which implements it in
//! Libraries/LibWeb/HTML/StructuredSerializeRustIntegration.cpp: the C++ records that the serializations hand their
//! bytes over to and the deserializations read the bytes of, C++ platform objects, and the side tables that
//! SharedArrayBuffers are shared through.

use core::ffi::{c_int, c_void};
use core::marker::PhantomData;
use core::mem::ManuallyDrop;
use core::ptr::NonNull;

use libjs_rust::api::{
    ArrayBuffer, AsSharedMemory, Gc, Object, RawNativeFunctionResult, Realm, Value, raw_descriptor,
    release_stores_of_lost_messages,
};

use crate::bindings::{Exception, WebIdlResult};
use crate::html::structured_serialize::message_record::SharedMemory;

unsafe extern "C" {
    fn web_structured_serialize_set_ipc_record(
        record: *mut c_void,
        data: ak::Span<'_, u8>,
        shared_array_buffers: *const *mut c_void,
        count: usize,
    );
    fn web_structured_serialize_set_storage_record(record: *mut c_void, data: ak::Span<'_, u8>);

    fn web_structured_serialize_ipc_record_data(record: *const c_void, data: &mut ak::Span<'static, u8>);
    fn web_structured_serialize_ipc_record_shared_array_buffer(
        record: *const c_void,
        side_table_index: u32,
    ) -> *mut c_void;
    fn web_structured_serialize_data_clone_error_from_serialization_error(
        realm: *mut c_void,
        message: ak::Span<'_, u8>,
    ) -> RawNativeFunctionResult;

    fn web_structured_serialize_current_settings_object_agent_cluster(agent_cluster: &mut u64) -> bool;
    fn web_structured_serialize_current_settings_object_cross_origin_isolated_capability() -> bool;
    fn web_structured_serialize_realm_agent_cluster(realm: *mut c_void, agent_cluster: &mut u64) -> bool;

    fn web_structured_serialize_cpp_interface_name(object: *mut c_void, interface_name: &mut u16) -> bool;

    fn web_structured_serialize_is_exposed(interface_name: u16, realm: *mut c_void) -> bool;

    fn web_structured_serialize_gather_image_bitmap_pixels(
        transfer_list: *const *mut c_void,
        count: usize,
        buffer_index: u32,
        shared_memory: &mut c_int,
        size: &mut usize,
    ) -> *mut c_void;
    fn web_structured_serialize_image_bitmap_pixels_destroy(image_bitmap_pixels: *mut c_void);

    fn web_structured_serialize_transfer_record_destroy(record: *mut c_void);
    fn web_structured_serialize_shared_pixels_create(
        descriptors: *const c_int,
        sizes: *const usize,
        count: usize,
    ) -> *mut c_void;
    fn web_structured_serialize_shared_pixels_destroy(buffers: *mut c_void);
    fn web_structured_serialize_transferred_values_append(transferred_values: *mut c_void, value: *mut c_void);
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

/// The values of an array of objects, as the C ABI takes them.
pub(crate) fn objects_into_abi<T>(objects: &[Gc<T>]) -> Vec<*mut c_void> {
    objects.iter().map(|object| object_into_abi(*object)).collect()
}

/// An HTML::IPCSerializationRecord that C++ lends for a serialization to set.
pub(crate) struct IPCSerializationRecord(NonNull<c_void>);

impl IPCSerializationRecord {
    /// # Safety
    ///
    /// `record` must be a record that C++ lends while this is used.
    pub(crate) unsafe fn from_raw(record: *mut c_void) -> Self {
        Self(NonNull::new(record).expect("C++ lends a record"))
    }

    /// Sets the record to the bytes, with the SharedArrayBuffers of its same-agent side table.
    pub(crate) fn set(self, data: &[u8], shared_array_buffers: &[Gc<ArrayBuffer>]) {
        let shared_array_buffers = objects_into_abi(shared_array_buffers);
        // SAFETY: The record is lent, the call borrows the bytes, and the buffers are live.
        unsafe {
            web_structured_serialize_set_ipc_record(
                self.0.as_ptr(),
                ak::Span::new(data),
                shared_array_buffers.as_ptr(),
                shared_array_buffers.len(),
            );
        }
    }
}

/// An HTML::StorageSerializationRecord that C++ lends for a serialization to set.
pub(crate) struct StorageSerializationRecord(NonNull<c_void>);

impl StorageSerializationRecord {
    /// # Safety
    ///
    /// `record` must be a record that C++ lends while this is used.
    pub(crate) unsafe fn from_raw(record: *mut c_void) -> Self {
        Self(NonNull::new(record).expect("C++ lends a record"))
    }

    pub(crate) fn set(self, data: &[u8]) {
        // SAFETY: The record is lent, and the call borrows the bytes.
        unsafe { web_structured_serialize_set_storage_record(self.0.as_ptr(), ak::Span::new(data)) };
    }
}

/// An HTML::IPCSerializationRecord that C++ lends for a deserialization to read.
#[derive(Clone, Copy)]
pub(crate) struct ReceivedIPCSerializationRecord<'a> {
    raw: NonNull<c_void>,
    _lent: PhantomData<&'a c_void>,
}

impl<'a> ReceivedIPCSerializationRecord<'a> {
    /// # Safety
    ///
    /// `record` must be a record that C++ lends for `'a`.
    pub(crate) unsafe fn from_raw(record: *const c_void) -> Self {
        Self {
            raw: NonNull::new(record.cast_mut()).expect("C++ lends a record"),
            _lent: PhantomData,
        }
    }

    pub(crate) fn data(self) -> &'a [u8] {
        let mut data = ak::Span::default();
        // SAFETY: The record is lent for 'a, and its bytes with it.
        unsafe { web_structured_serialize_ipc_record_data(self.raw.as_ptr(), &mut data) };
        data.as_slice()
    }

    /// The SharedArrayBuffer at the index of the record's same-agent side table, if there is one.
    pub(crate) fn shared_array_buffer(self, side_table_index: u32) -> Option<Gc<ArrayBuffer>> {
        // SAFETY: The record is lent. LibWeb returns a live object or null.
        let object = unsafe {
            object_from_abi(web_structured_serialize_ipc_record_shared_array_buffer(
                self.raw.as_ptr(),
                side_table_index,
            ))
        }?;
        Some(
            object
                .downcast::<ArrayBuffer>()
                .expect("the side table holds ArrayBuffers"),
        )
    }
}

/// AD-HOC: Gathers the pixels of the ImageBitmaps of the transfer list in one buffer of shared memory, whose index in
///         the shared memory of the record of the transfer is the buffer index. Without it, as when there is not enough
///         memory for it, the pixels travel inline.
pub(crate) fn gather_image_bitmap_pixels(
    transfer_list: &[Gc<Object>],
    buffer_index: u32,
) -> (ImageBitmapSharedPixels, Option<SharedMemory>) {
    let transfer_list = objects_into_abi(transfer_list);
    let (mut descriptor, mut size) = (0, 0);
    // SAFETY: The objects are live, and the call writes to the locals.
    let pixels = ImageBitmapSharedPixels(NonNull::new(unsafe {
        web_structured_serialize_gather_image_bitmap_pixels(
            transfer_list.as_ptr(),
            transfer_list.len(),
            buffer_index,
            &mut descriptor,
            &mut size,
        )
    }));
    if pixels.0.is_none() {
        return (pixels, None);
    }
    // SAFETY: The pixels keep the descriptor open.
    match unsafe { SharedMemory::duplicate(descriptor, size) } {
        Some(shared_memory) => (pixels, Some(shared_memory)),
        // NB: Without a descriptor for the record, the pixels travel inline.
        None => (ImageBitmapSharedPixels(None), None),
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

/// The primary interface of a C++ implementation object, if the object wraps one.
pub(crate) fn cpp_serializable_interface_name(object: Gc<Object>) -> Option<u16> {
    let mut interface_name = 0;
    // SAFETY: The object is live, and the call writes the identifier.
    unsafe { web_structured_serialize_cpp_interface_name(object_into_abi(object), &mut interface_name) }
        .then_some(interface_name)
}

/// https://webidl.spec.whatwg.org/#dfn-exposed
pub(crate) fn is_exposed(interface_name: u16, realm: Gc<Realm>) -> bool {
    // SAFETY: The realm is live.
    unsafe { web_structured_serialize_is_exposed(interface_name, realm.as_ptr().cast()) }
}

/// HTML::ImageBitmapSharedPixels that this owns: the shared memory that a transfer gathers the pixels of its
/// ImageBitmaps in, if it has some.
pub(crate) struct ImageBitmapSharedPixels(Option<NonNull<c_void>>);

impl ImageBitmapSharedPixels {
    pub(crate) fn as_raw(&self) -> Option<NonNull<c_void>> {
        self.0
    }
}

impl Drop for ImageBitmapSharedPixels {
    fn drop(&mut self) {
        if let Some(image_bitmap_pixels) = self.0 {
            // SAFETY: This owns the pixels.
            unsafe { web_structured_serialize_image_bitmap_pixels_destroy(image_bitmap_pixels.as_ptr()) };
        }
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

    pub(crate) fn as_raw(self) -> *mut c_void {
        self.0.as_ptr()
    }
}

/// A record of a structured serialization with transfer that this owns.
pub(crate) struct OwnedSerializedTransferRecord(SerializedTransferRecord);

impl OwnedSerializedTransferRecord {
    /// # Safety
    ///
    /// `record` must be a record that LibWeb hands over.
    pub(crate) unsafe fn from_raw(record: *mut c_void) -> Self {
        // SAFETY: The caller hands over the record.
        Self(unsafe { SerializedTransferRecord::from_raw(record) })
    }

    pub(crate) fn record(&self) -> SerializedTransferRecord {
        self.0
    }

    /// Hands the record over to LibWeb's C++ code, which owns it from then on.
    pub(crate) fn into_raw(self) -> *mut c_void {
        let record = ManuallyDrop::new(self);
        record.0.0.as_ptr()
    }
}

impl Drop for OwnedSerializedTransferRecord {
    fn drop(&mut self) {
        // SAFETY: This owns the record.
        unsafe { web_structured_serialize_transfer_record_destroy(self.0.0.as_ptr()) };
        release_stores_of_lost_messages();
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

/// The shared memory that the ImageBitmaps of a record share their pixels through, mapped when one needs it.
pub(crate) struct SharedPixelBuffers<'a> {
    memory: &'a [SharedMemory],
    buffers: core::cell::Cell<Option<NonNull<c_void>>>,
}

impl<'a> SharedPixelBuffers<'a> {
    pub(crate) fn new(memory: &'a [SharedMemory]) -> Self {
        Self {
            memory,
            buffers: core::cell::Cell::new(None),
        }
    }

    pub(crate) fn as_raw(&self) -> Option<NonNull<c_void>> {
        if let Some(buffers) = self.buffers.get() {
            return Some(buffers);
        }
        let descriptors: Vec<_> = self
            .memory
            .iter()
            .map(|memory| raw_descriptor(memory.descriptor.as_shared_memory()))
            .collect();
        let sizes: Vec<_> = self.memory.iter().map(|memory| memory.size).collect();
        // SAFETY: The call borrows the open descriptors and their sizes, and hands over the buffer list.
        let buffers = NonNull::new(unsafe {
            web_structured_serialize_shared_pixels_create(descriptors.as_ptr(), sizes.as_ptr(), descriptors.len())
        });
        self.buffers.set(buffers);
        buffers
    }
}

impl Drop for SharedPixelBuffers<'_> {
    fn drop(&mut self) {
        if let Some(buffers) = self.buffers.get() {
            // SAFETY: This owns the list. Each image keeps its own reference to the buffers it uses.
            unsafe { web_structured_serialize_shared_pixels_destroy(buffers.as_ptr()) };
        }
    }
}
