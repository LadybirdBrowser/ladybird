/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What structured serialization asks of LibWeb's C++ code, which implements it in
//! Libraries/LibWeb/HTML/StructuredSerializeRustIntegration.cpp: the C++ records that the serializations hand their
//! bytes over to and the deserializations read the bytes of, the registry of serializable interfaces, the platform
//! objects, and the side tables that SharedArrayBuffers are shared through.

use core::ffi::{c_int, c_void};
use core::marker::PhantomData;
use core::mem::ManuallyDrop;
use core::ptr::NonNull;

use libjs_rust::api::{ArrayBuffer, DeferGc, Gc, Object, RawNativeFunctionResult, Realm, Value, into_raw_descriptor};

use crate::bindings::structured_data::{SerializationReader, SerializationWriter};
use crate::bindings::{CallContext, Exception, WebIdlResult};
use crate::html::structured_serialize::message_record::{MessageRecord, SharedMemory};
use crate::html::structured_serialize::transfer_data_holder::TransferDataHolder;

unsafe extern "C" {
    fn web_structured_serialize_set_ipc_record(
        record: *mut c_void,
        data: ak::Span<'_, u8>,
        shared_array_buffers: *const *mut c_void,
        count: usize,
    );
    fn web_structured_serialize_set_storage_record(record: *mut c_void, data: ak::Span<'_, u8>);
    fn web_structured_serialize_is_ipc_interface(interface_name: u16) -> bool;
    fn web_structured_serialize_interface_storage_identifier(
        interface_name: u16,
        identifier: &mut ak::Span<'static, u8>,
    ) -> bool;
    fn web_structured_serialize_interface_name_from_storage_identifier(
        identifier: ak::Span<'_, u8>,
        interface_name: &mut u16,
    ) -> bool;

    fn web_structured_serialize_ipc_record_data(record: *const c_void, data: &mut ak::Span<'static, u8>);
    fn web_structured_serialize_ipc_record_shared_array_buffer(
        record: *const c_void,
        side_table_index: u32,
    ) -> *mut c_void;
    fn web_structured_serialize_data_clone_error_from_serialization_error(
        realm: *mut c_void,
        message: ak::Span<'_, u8>,
    ) -> RawNativeFunctionResult;

    fn web_structured_serialize_serializable_payload_version() -> u64;

    fn web_structured_serialize_current_settings_object_agent_cluster(agent_cluster: &mut u64) -> bool;
    fn web_structured_serialize_current_settings_object_cross_origin_isolated_capability() -> bool;
    fn web_structured_serialize_realm_agent_cluster(realm: *mut c_void, agent_cluster: &mut u64) -> bool;

    fn web_structured_serialize_is_platform_object(object: *mut c_void) -> bool;
    fn web_structured_serialize_serializable_interface_name(object: *mut c_void, interface_name: &mut u16) -> bool;
    fn web_structured_serialize_serialization_steps(
        realm: *mut c_void,
        object: *mut c_void,
        serialized: *mut c_void,
        for_storage: bool,
    ) -> RawNativeFunctionResult;
    fn web_structured_serialize_is_exposed(interface_name: u16, realm: *mut c_void) -> bool;
    fn web_structured_serialize_serializable_steps(interface_name: u16) -> u8;
    fn web_structured_serialize_deserialize_rust_platform_object(
        interface_name: u16,
        realm: *mut c_void,
        serialized: *mut c_void,
    ) -> RawNativeFunctionResult;

    fn web_structured_serialize_gather_image_bitmap_pixels(
        transfer_list: *const *mut c_void,
        count: usize,
        buffer_index: u32,
        shared_memory: &mut c_int,
        size: &mut usize,
    ) -> *mut c_void;
    fn web_structured_serialize_image_bitmap_pixels_destroy(image_bitmap_pixels: *mut c_void);

    fn web_structured_serialize_transfer_data_holder_create() -> *mut c_void;
    fn web_structured_serialize_transfer_data_holder_append(data_holder: *mut c_void, bytes: ak::Span<'_, u8>);
    fn web_structured_serialize_transfer_data_holder_destroy(data_holder: *mut c_void);
    fn web_structured_serialize_transfer_record_create() -> *mut c_void;
    fn web_structured_serialize_transfer_record_destroy(record: *mut c_void);
    fn web_structured_serialize_transfer_record_set_serialized(
        record: *mut c_void,
        data: ak::Span<'_, u8>,
        shared_array_buffers: *const *mut c_void,
        shared_array_buffer_count: usize,
        shared_memory: *const c_int,
        shared_memory_sizes: *const usize,
        shared_memory_count: usize,
    );
    fn web_structured_serialize_transfer_record_shared_memory_count(record: *mut c_void) -> usize;
    fn web_structured_serialize_transfer_record_shared_memory(
        record: *mut c_void,
        index: usize,
        shared_memory: &mut c_int,
        size: &mut usize,
    );
    fn web_structured_serialize_transfer_record_append_data_holder(record: *mut c_void, data_holder: *mut c_void);
    fn web_structured_serialize_transfer_record_data_holder_count(record: *mut c_void) -> usize;
    fn web_structured_serialize_transfer_record_take_data_holder(record: *mut c_void, index: usize) -> *mut c_void;
    fn web_structured_serialize_transfer_record_serialized(record: *mut c_void) -> *const c_void;
    fn web_structured_serialize_transfer_record_clear_shared_buffers(record: *mut c_void);
    fn web_structured_serialize_transferred_values_append(transferred_values: *mut c_void, value: *mut c_void);
    fn web_structured_serialize_transfer_data_decoder_bytes(decoder: *const c_void, bytes: &mut ak::Span<'static, u8>);
    fn web_structured_serialize_transfer_data_decoder_destroy(decoder: *mut c_void);
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
fn objects_into_abi<T>(objects: &[Gc<T>]) -> Vec<*mut c_void> {
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

    /// Sets the record to the bytes, with the SharedArrayBuffers of its same-process side table.
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

/// Whether IPC records can contain this interface, including interfaces that cannot be stored.
pub(crate) fn is_ipc_interface(interface_name: u16) -> bool {
    // SAFETY: The call takes an interface identifier by value.
    unsafe { web_structured_serialize_is_ipc_interface(interface_name) }
}

/// The identifier of the serializable interface that a storage record holds, from LibWeb's registry of them, if the
/// Bindings::InterfaceName names one.
pub(crate) fn serializable_interface_storage_identifier(interface_name: u16) -> Option<&'static [u8]> {
    let mut identifier = ak::Span::default();
    // SAFETY: The call writes a span of static storage to the local.
    unsafe { web_structured_serialize_interface_storage_identifier(interface_name, &mut identifier) }
        .then(|| identifier.as_slice())
}

/// The Bindings::InterfaceName of the serializable interface with the identifier, if LibWeb's registry has one.
pub(crate) fn serializable_interface_name_from_storage_identifier(identifier: &[u8]) -> Option<u16> {
    let mut interface_name = 0;
    // SAFETY: The call borrows the identifier, and writes to the local.
    unsafe {
        web_structured_serialize_interface_name_from_storage_identifier(ak::Span::new(identifier), &mut interface_name)
    }
    .then_some(interface_name)
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

    /// The SharedArrayBuffer at the index of the record's same-process side table, if there is one.
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

/// HTML::serializable_payload_version: the per-interface payload version that a storage record holds after the
/// interface name of every serializable object.
pub(crate) fn serializable_payload_version() -> u64 {
    // SAFETY: The call has no arguments.
    unsafe { web_structured_serialize_serializable_payload_version() }
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

/// The serialization steps of the primary interface of the serializable platform object, which C++ runs through the
/// bindings of the interface, given the writer, which it passes on.
pub(crate) fn serialization_steps(
    cx: &CallContext,
    object: Gc<Object>,
    serialized: &mut SerializationWriter,
    for_storage: bool,
) -> WebIdlResult<()> {
    // SAFETY: The realm and the object are live, and the writer is lent for the call.
    completion(unsafe {
        web_structured_serialize_serialization_steps(
            cx.realm().as_ptr().cast(),
            object_into_abi(object),
            core::ptr::from_mut(serialized).cast(),
            for_storage,
        )
    })?;
    Ok(())
}

/// https://webidl.spec.whatwg.org/#dfn-exposed
pub(crate) fn is_exposed(interface_name: u16, realm: Gc<Realm>) -> bool {
    // SAFETY: The realm is live.
    unsafe { web_structured_serialize_is_exposed(interface_name, realm.as_ptr().cast()) }
}

/// Whose steps serialize and deserialize the objects of a serializable interface.
#[derive(Clone, Copy, PartialEq, Eq)]
pub(crate) enum SerializableSteps {
    /// The steps of an interface implemented in Rust, which its bindings run.
    Rust,
    /// The steps of DOMException, which run in crate::webidl::dom_exception.
    DOMException,
    /// The steps of QuotaExceededError, which run in crate::webidl::quota_exceeded_error.
    QuotaExceededError,
    /// The steps of CryptoKey, which run in crate::crypto::crypto_key.
    CryptoKey,
    /// The steps of RTCEncodedAudioFrame, which run in crate::webrtc::rtc_encoded_audio_frame.
    RTCEncodedAudioFrame,
}

pub(crate) fn serializable_steps(interface_name: u16) -> SerializableSteps {
    // SAFETY: The call takes a value.
    match unsafe { web_structured_serialize_serializable_steps(interface_name) } {
        0 => SerializableSteps::Rust,
        1 => SerializableSteps::DOMException,
        2 => SerializableSteps::QuotaExceededError,
        3 => SerializableSteps::CryptoKey,
        4 => SerializableSteps::RTCEncodedAudioFrame,
        steps => unreachable!("LibWeb names no serializable steps {steps}"),
    }
}

/// The deserialization steps of an interface implemented in Rust, which create the new instance in the realm from what
/// they read, and which C++ runs through the bindings of the interface, given the reader, which it passes on.
pub(crate) fn deserialize_rust_platform_object(
    interface_name: u16,
    realm: Gc<Realm>,
    serialized: &mut SerializationReader,
) -> WebIdlResult<Value> {
    // SAFETY: The realm is live, and the reader is lent for the call.
    completion(unsafe {
        web_structured_serialize_deserialize_rust_platform_object(
            interface_name,
            realm.as_ptr().cast(),
            core::ptr::from_mut(serialized).cast(),
        )
    })
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

/// The C++ carrier of a data holder that this owns: an HTML::TransferDataEncoder, which the record of a transfer and
/// IPC carry, and which C++ encodes the values that have attachments into.
pub(crate) struct TransferDataCarrier(NonNull<c_void>);

impl TransferDataCarrier {
    pub(crate) fn new() -> Self {
        // SAFETY: LibWeb returns a new carrier, which this owns.
        Self(
            NonNull::new(unsafe { web_structured_serialize_transfer_data_holder_create() }).expect("LibWeb creates it"),
        )
    }

    pub(crate) fn as_raw(&self) -> *mut c_void {
        self.0.as_ptr()
    }

    /// Appends bytes that are already in IPC's encoding.
    pub(crate) fn append(&self, bytes: &[u8]) {
        // SAFETY: This owns the carrier, and the call borrows the bytes.
        unsafe { web_structured_serialize_transfer_data_holder_append(self.0.as_ptr(), ak::Span::new(bytes)) };
    }

    /// Hands the carrier over to LibWeb's C++ code, which owns it from then on.
    pub(crate) fn into_raw(self) -> *mut c_void {
        ManuallyDrop::new(self).0.as_ptr()
    }
}

impl Drop for TransferDataCarrier {
    fn drop(&mut self) {
        // SAFETY: This owns the carrier.
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

    pub(crate) fn as_raw(self) -> *mut c_void {
        self.0.as_ptr()
    }

    /// Sets the record's [[Serialized]] to the message record, with its same-process side table and the shared memory
    /// of the transfer, which C++ takes over. Its SharedArrayBuffers are rooted once the record has them, so the
    /// caller hands it over under the deferral that it was finished under.
    pub(crate) fn set_serialized(self, _: &DeferGc<'_>, serialized: MessageRecord) {
        let shared_array_buffers = objects_into_abi(&serialized.shared_array_buffers);
        let (shared_memory, shared_memory_sizes): (Vec<c_int>, Vec<usize>) = serialized
            .shared_memory
            .into_iter()
            .map(|shared_memory| (into_raw_descriptor(shared_memory.descriptor), shared_memory.size))
            .unzip();
        // SAFETY: The record is lent, the call borrows the bytes, the buffers are live, and it takes over the
        //         descriptors.
        unsafe {
            web_structured_serialize_transfer_record_set_serialized(
                self.0.as_ptr(),
                ak::Span::new(&serialized.bytes),
                shared_array_buffers.as_ptr(),
                shared_array_buffers.len(),
                shared_memory.as_ptr(),
                shared_memory_sizes.as_ptr(),
                shared_memory.len(),
            );
        }
    }

    /// Duplicates of the shared memory of the record, which its [[Serialized]] and its data holders share
    /// SharedArrayBuffers and the pixels of ImageBitmaps through. A descriptor that cannot be duplicated leaves out the
    /// shared memory from it on, which deserializing then throws for.
    pub(crate) fn shared_memory(self) -> Vec<SharedMemory> {
        // SAFETY: The record is lent.
        let count = unsafe { web_structured_serialize_transfer_record_shared_memory_count(self.0.as_ptr()) };
        (0..count)
            .map_while(|index| {
                let (mut descriptor, mut size) = (0, 0);
                // SAFETY: The record is lent, and it keeps the descriptor open.
                unsafe {
                    web_structured_serialize_transfer_record_shared_memory(
                        self.0.as_ptr(),
                        index,
                        &mut descriptor,
                        &mut size,
                    );
                    SharedMemory::duplicate(descriptor, size)
                }
            })
            .collect()
    }

    /// Appends the data holder to the record's [[TransferDataHolders]].
    pub(crate) fn append_data_holder(self, data_holder: TransferDataHolder) {
        // SAFETY: The record is lent, and LibWeb takes the carrier.
        unsafe {
            web_structured_serialize_transfer_record_append_data_holder(
                self.0.as_ptr(),
                data_holder.into_carrier().into_raw(),
            );
        }
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

    /// The record's [[Serialized]], which it lends for as long as it is lent.
    ///
    /// # Safety
    ///
    /// The record must be lent for `'a`.
    pub(crate) unsafe fn serialized<'a>(self) -> ReceivedIPCSerializationRecord<'a> {
        // SAFETY: The caller lends the record for 'a, and so its [[Serialized]].
        unsafe {
            ReceivedIPCSerializationRecord::from_raw(web_structured_serialize_transfer_record_serialized(
                self.0.as_ptr(),
            ))
        }
    }

    pub(crate) fn clear_shared_buffers(self) {
        // SAFETY: The record is lent.
        unsafe { web_structured_serialize_transfer_record_clear_shared_buffers(self.0.as_ptr()) };
    }
}

/// A record of a structured serialization with transfer that this owns.
pub(crate) struct OwnedSerializedTransferRecord(SerializedTransferRecord);

impl OwnedSerializedTransferRecord {
    pub(crate) fn new() -> Self {
        // SAFETY: LibWeb returns a new record, which this owns.
        Self(unsafe { SerializedTransferRecord::from_raw(web_structured_serialize_transfer_record_create()) })
    }

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

    pub(crate) fn as_raw(self) -> *mut c_void {
        self.0.as_ptr()
    }

    /// The bytes of the data holder, which it lends for as long as it is lent.
    ///
    /// # Safety
    ///
    /// The decoder must be lent for `'a`.
    pub(crate) unsafe fn bytes<'a>(self) -> &'a [u8] {
        let mut bytes = ak::Span::default();
        // SAFETY: The decoder is lent for 'a, and its bytes with it.
        unsafe { web_structured_serialize_transfer_data_decoder_bytes(self.0.as_ptr(), &mut bytes) };
        bytes.as_slice()
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
