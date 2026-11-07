/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The result of StructuredSerializeWithTransfer,
//! https://html.spec.whatwg.org/multipage/structured-data.html#structuredserializewithtransfer

use core::cell::RefCell;
use core::ffi::{c_int, c_void};
use core::ptr::NonNull;

use libjs_rust::api::{
    ArrayBuffer, DeferGc, EmbedderCell, EmbedderClass, EmbedderValue, Gc, GcRefCell, SharedStorePin, Trace, Vm,
    into_raw_descriptor, lifeline_into_raw_descriptor,
};

use super::cpp::{OwnedSerializedTransferRecord, SerializedTransferRecord, objects_into_abi};
use super::message_record::{MessageRecord, SharedMemory};
use super::transfer_data_holder::TransferPayload;
use libipc_rust::wire::File;
use libipc_rust::{DecodeError, Decoder, EncodeError, Encoder};
use libjs_rust::api::{AsSharedMemory, lifeline_from_raw_descriptor, owned_from_raw_descriptor, raw_descriptor};

unsafe extern "C" {
    fn web_structured_serialize_validate_shared_memory(descriptor: c_int, size: usize) -> bool;
    fn web_structured_serialize_transfer_record_create(
        bytes: ak::Span<'_, u8>,
        array_buffers: *const *mut c_void,
        array_buffer_count: usize,
        descriptors: *const c_int,
        sizes: *const usize,
        shared_memory_count: usize,
        shared_store_lifeline: c_int,
        holders: *const *mut c_void,
        holder_count: usize,
    ) -> *mut c_void;
    fn web_structured_serialize_transfer_record_take(
        record: *mut c_void,
        context: *mut c_void,
        receive: unsafe extern "C" fn(
            *mut c_void,
            ak::Span<'_, u8>,
            *const *mut c_void,
            usize,
            *const c_int,
            *const usize,
            usize,
            c_int,
        ),
        append_holder: unsafe extern "C" fn(*mut c_void, *mut c_void),
    );
}

/// The descriptor as what the record encodes, which takes it over.
fn file_from_raw_descriptor(raw: c_int) -> File {
    #[cfg(unix)]
    // SAFETY: The caller hands the descriptor over.
    let descriptor = unsafe { std::os::fd::FromRawFd::from_raw_fd(raw) };
    #[cfg(windows)]
    // SAFETY: The caller hands the descriptor over.
    let descriptor = unsafe { libipc_rust::transport_windows::OwnedDescriptor::from_raw(raw) };
    File(descriptor)
}

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct TransferRecord {
    pub(crate) serialized: GcRefCell<Option<MessageRecord>>,
    #[gc(untraced)]
    pub(crate) data_holders: RefCell<Vec<TransferPayload>>,
}

static TRANSFER_RECORD_CLASS: EmbedderClass<TransferRecord> = EmbedderClass::new("SerializedTransferRecord");

impl EmbedderValue for TransferRecord {
    const CLASS: &'static EmbedderClass<Self> = &TRANSFER_RECORD_CLASS;
}

pub(crate) type TransferRecordRef = Gc<EmbedderCell<TransferRecord>>;

/// A record on its way to a MessagePort, which is plain data that any thread can own: the bytes, the data holders, the
/// shared memory of ImageBitmap pixels, and the pin of the stores that the bytes name.
#[derive(Default)]
pub(crate) struct PortMessage {
    bytes: Vec<u8>,
    data_holders: Vec<TransferPayload>,
    shared_memory: Vec<SharedMemory>,
    shared_store_pin: Option<SharedStorePin>,
}

impl PortMessage {
    pub(crate) fn encode_into(mut self, encoder: &mut Encoder) -> Result<(), EncodeError> {
        encoder.encode(self.bytes.as_slice())?;
        encoder.encode_owned(core::mem::take(&mut self.data_holders))?;
        encoder.encode_size(self.shared_memory.len())?;
        for memory in core::mem::take(&mut self.shared_memory) {
            encoder.encode(&true)?;
            encoder.encode(&(memory.size as u64))?;
            encoder.encode_owned(file_from_raw_descriptor(into_raw_descriptor(memory.descriptor)))?;
        }
        let shared_store_lifeline = self.shared_store_pin.take().and_then(SharedStorePin::into_lifeline);
        encoder.encode(&shared_store_lifeline.is_some())?;
        if let Some(lifeline) = shared_store_lifeline {
            encoder.encode_owned(file_from_raw_descriptor(lifeline_into_raw_descriptor(lifeline)))?;
        }
        Ok(())
    }

    pub(crate) fn decode(decoder: &mut Decoder<'_>) -> Result<Self, DecodeError> {
        let bytes = decoder.decode::<Vec<u8>>()?;
        let data_holders = decoder.decode::<Vec<TransferPayload>>()?;
        let count = decoder.decode_size()?;
        let mut shared_memory = Vec::new();
        let mut keep_prefix = true;
        for _ in 0..count {
            if !decoder.decode::<bool>()? {
                // Match importing an invalid C++ AnonymousBuffer: its missing descriptor truncates the side table.
                keep_prefix = false;
                continue;
            }
            let size = usize::try_from(decoder.decode::<u64>()?)
                .map_err(|_| DecodeError::InvalidValue("shared memory size does not fit"))?;
            let File(descriptor) = decoder.decode::<File>()?;
            #[cfg(unix)]
            let raw = std::os::fd::IntoRawFd::into_raw_fd(descriptor);
            #[cfg(windows)]
            let raw = descriptor.into_raw();
            // SAFETY: File hands its descriptor over to the shared-memory owner.
            let memory = SharedMemory {
                descriptor: unsafe { owned_from_raw_descriptor(raw) },
                size,
            };
            // The native backing size and seal checks must run before any bitmap or ArrayBuffer can map the claim.
            // SAFETY: The owner keeps the descriptor open for validation.
            if !unsafe {
                web_structured_serialize_validate_shared_memory(
                    raw_descriptor(memory.descriptor.as_shared_memory()),
                    size,
                )
            } {
                return Err(DecodeError::InvalidValue("invalid shared memory backing"));
            }
            if keep_prefix {
                shared_memory.push(memory);
            }
        }
        let shared_store_pin = decoder
            .decode::<bool>()?
            .then(|| decoder.decode::<File>())
            .transpose()?
            .map(|File(descriptor)| {
                #[cfg(unix)]
                let raw = std::os::fd::IntoRawFd::into_raw_fd(descriptor);
                #[cfg(windows)]
                let raw = descriptor.into_raw();
                // SAFETY: File hands its descriptor over to the lifeline.
                SharedStorePin::Lifeline(unsafe { lifeline_from_raw_descriptor(raw) })
            });
        Ok(Self {
            bytes,
            data_holders,
            shared_memory,
            shared_store_pin,
        })
    }
}

impl Drop for PortMessage {
    fn drop(&mut self) {
        if let Some(pin) = self.shared_store_pin.take() {
            pin.release();
        }
    }
}

impl TransferRecord {
    pub(crate) fn discard(record: TransferRecordRef) {
        record.serialized.replace(None);
        record.data_holders.borrow_mut().clear();
    }

    pub(crate) fn create(
        vm: &Vm,
        build: impl FnOnce(&DeferGc<'_>) -> MessageRecord,
        data_holders: Vec<TransferPayload>,
    ) -> TransferRecordRef {
        TRANSFER_RECORD_CLASS.allocate(vm, |defer_gc| Self {
            serialized: GcRefCell::new(Some(build(defer_gc))),
            data_holders: RefCell::new(data_holders),
        })
    }

    pub(crate) fn into_cpp(record: TransferRecordRef, vm: &Vm) -> OwnedSerializedTransferRecord {
        DeferGc::scope(vm, |_defer_gc| {
            let mut message = record.serialized.replace(None).expect("a record is handed over once");
            let buffers = objects_into_abi(&message.shared_array_buffers);
            let (descriptors, sizes): (Vec<_>, Vec<_>) = core::mem::take(&mut message.shared_memory)
                .into_iter()
                .map(|memory| (into_raw_descriptor(memory.descriptor), memory.size))
                .unzip();
            let shared_store_lifeline = message
                .shared_store_pin
                .take()
                .and_then(SharedStorePin::into_lifeline)
                .map_or(-1, lifeline_into_raw_descriptor);
            let holders: Vec<_> = record
                .data_holders
                .borrow_mut()
                .drain(..)
                .map(TransferPayload::into_cpp)
                .collect();
            // SAFETY: The call borrows bytes and live buffers, and takes every descriptor and holder. GC remains
            //         deferred until the C++ carrier roots the buffer aliases.
            unsafe {
                OwnedSerializedTransferRecord::from_raw(web_structured_serialize_transfer_record_create(
                    ak::Span::new(&message.bytes),
                    buffers.as_ptr(),
                    buffers.len(),
                    descriptors.as_ptr(),
                    sizes.as_ptr(),
                    descriptors.len(),
                    shared_store_lifeline,
                    holders.as_ptr(),
                    holders.len(),
                ))
            }
        })
    }

    /// The IPC encoding of a SerializedTransferRecord, without its same-agent SharedArrayBuffers.
    pub(crate) fn encode_into(record: TransferRecordRef, vm: &Vm, encoder: &mut Encoder) -> Result<(), EncodeError> {
        Self::into_port_message(record, vm).encode_into(encoder)
    }

    /// What of the record can leave the heap of the sender. The side table of same-agent SharedArrayBuffers stays behind,
    /// as the bytes name those by the id of their store too.
    pub(crate) fn into_port_message(record: TransferRecordRef, vm: &Vm) -> PortMessage {
        DeferGc::scope(vm, |_| {
            let mut message = record.serialized.replace(None).expect("a record is handed over once");
            PortMessage {
                bytes: core::mem::take(&mut message.bytes),
                data_holders: core::mem::take(&mut *record.data_holders.borrow_mut()),
                shared_memory: core::mem::take(&mut message.shared_memory),
                shared_store_pin: message.shared_store_pin.take(),
            }
        })
    }

    /// The record of a message, in the heap of the receiver.
    pub(crate) fn from_port_message(vm: &Vm, mut message: PortMessage) -> TransferRecordRef {
        let holders = core::mem::take(&mut message.data_holders);
        Self::create(
            vm,
            |_| MessageRecord {
                bytes: core::mem::take(&mut message.bytes),
                shared_array_buffers: Vec::new(),
                shared_memory: core::mem::take(&mut message.shared_memory),
                shared_store_pin: message.shared_store_pin.take(),
            },
            holders,
        )
    }

    pub(crate) fn decode(vm: &Vm, decoder: &mut Decoder<'_>) -> Result<TransferRecordRef, DecodeError> {
        Ok(Self::from_port_message(vm, PortMessage::decode(decoder)?))
    }

    pub(crate) fn from_cpp(vm: &Vm, record: SerializedTransferRecord) -> TransferRecordRef {
        struct Received<'a> {
            defer_gc: &'a DeferGc<'a>,
            message: Option<MessageRecord>,
            holders: Vec<TransferPayload>,
        }

        unsafe extern "C" fn receive(
            context: *mut c_void,
            bytes: ak::Span<'_, u8>,
            buffers: *const *mut c_void,
            buffer_count: usize,
            descriptors: *const c_int,
            sizes: *const usize,
            shared_memory_count: usize,
            shared_store_lifeline: c_int,
        ) {
            // SAFETY: from_cpp() lends the state under its allocation's deferral, and C++ lends every input slice.
            let received = unsafe { &mut *context.cast::<Received<'_>>() };
            #[cfg_attr(
                ladybird_gc_lints,
                expect(gc_container_on_stack, reason = "the collector is deferred while C++ lends them")
            )]
            let mut message = unsafe {
                MessageRecord::received(
                    received.defer_gc,
                    bytes.as_slice(),
                    ak::slice_from_raw_parts(descriptors, shared_memory_count),
                    ak::slice_from_raw_parts(sizes, shared_memory_count),
                    shared_store_lifeline,
                )
            };
            message.shared_array_buffers = unsafe { ak::slice_from_raw_parts(buffers, buffer_count) }
                .iter()
                .map(|&buffer| {
                    // SAFETY: C++ lends live ArrayBuffers while the collector is deferred.
                    unsafe {
                        Gc::from_non_null(NonNull::new(buffer.cast::<ArrayBuffer>()).expect("C++ lends a buffer"))
                    }
                })
                .collect();
            received.message = Some(message);
        }

        unsafe extern "C" fn append(context: *mut c_void, holder: *mut c_void) {
            // SAFETY: from_cpp() lends the state, and C++ hands the holder over.
            unsafe {
                (*context.cast::<Received<'_>>())
                    .holders
                    .push(TransferPayload::from_cpp(holder));
            };
        }

        TRANSFER_RECORD_CLASS.allocate(vm, |defer_gc| {
            let mut received = Received {
                defer_gc,
                message: None,
                holders: Vec::new(),
            };
            // SAFETY: The record and the state are live, and the callbacks receive only the state and the contents.
            unsafe {
                web_structured_serialize_transfer_record_take(
                    record.as_raw(),
                    (&raw mut received).cast(),
                    receive,
                    append,
                );
            };
            Self {
                serialized: GcRefCell::new(received.message),
                data_holders: RefCell::new(received.holders),
            }
        })
    }
}
