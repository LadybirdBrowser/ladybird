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
    ArrayBuffer, DeferGc, EmbedderCell, EmbedderClass, EmbedderValue, Gc, GcRefCell, Trace, Vm, into_raw_descriptor,
};

use super::cpp::{OwnedSerializedTransferRecord, SerializedTransferRecord, TransferDataCarrier, objects_into_abi};
use super::message_record::MessageRecord;

unsafe extern "C" {
    fn web_structured_serialize_transfer_record_create(
        bytes: ak::Span<'_, u8>,
        array_buffers: *const *mut c_void,
        array_buffer_count: usize,
        descriptors: *const c_int,
        sizes: *const usize,
        shared_memory_count: usize,
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
        ),
        append_holder: unsafe extern "C" fn(*mut c_void, *mut c_void),
    );
}

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct TransferRecord {
    pub(crate) serialized: GcRefCell<Option<MessageRecord>>,
    #[gc(untraced)]
    pub(crate) data_holders: RefCell<Vec<TransferDataCarrier>>,
}

static TRANSFER_RECORD_CLASS: EmbedderClass<TransferRecord> = EmbedderClass::new("SerializedTransferRecord");

impl EmbedderValue for TransferRecord {
    const CLASS: &'static EmbedderClass<Self> = &TRANSFER_RECORD_CLASS;
}

pub(crate) type TransferRecordRef = Gc<EmbedderCell<TransferRecord>>;

impl TransferRecord {
    pub(crate) fn discard(record: TransferRecordRef) {
        record.serialized.replace(None);
        record.data_holders.borrow_mut().clear();
    }

    pub(crate) fn create(
        vm: &Vm,
        build: impl FnOnce(&DeferGc<'_>) -> MessageRecord,
        data_holders: Vec<TransferDataCarrier>,
    ) -> TransferRecordRef {
        TRANSFER_RECORD_CLASS.allocate(vm, |defer_gc| Self {
            serialized: GcRefCell::new(Some(build(defer_gc))),
            data_holders: RefCell::new(data_holders),
        })
    }

    pub(crate) fn into_cpp(record: TransferRecordRef, vm: &Vm) -> OwnedSerializedTransferRecord {
        DeferGc::scope(vm, |_defer_gc| {
            let message = record.serialized.replace(None).expect("a record is handed over once");
            let buffers = objects_into_abi(&message.shared_array_buffers);
            let (descriptors, sizes): (Vec<_>, Vec<_>) = message
                .shared_memory
                .into_iter()
                .map(|memory| (into_raw_descriptor(memory.descriptor), memory.size))
                .unzip();
            let holders: Vec<_> = record
                .data_holders
                .borrow_mut()
                .drain(..)
                .map(TransferDataCarrier::into_raw)
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
                    holders.as_ptr(),
                    holders.len(),
                ))
            }
        })
    }

    pub(crate) fn from_cpp(vm: &Vm, record: SerializedTransferRecord) -> TransferRecordRef {
        struct Received<'a> {
            defer_gc: &'a DeferGc<'a>,
            message: Option<MessageRecord>,
            holders: Vec<TransferDataCarrier>,
        }

        unsafe extern "C" fn receive(
            context: *mut c_void,
            bytes: ak::Span<'_, u8>,
            buffers: *const *mut c_void,
            buffer_count: usize,
            descriptors: *const c_int,
            sizes: *const usize,
            shared_memory_count: usize,
        ) {
            // SAFETY: from_cpp() lends the state under its allocation's deferral, and C++ lends every input slice.
            let received = unsafe { &mut *context.cast::<Received<'_>>() };
            received.message = Some(MessageRecord {
                shared_array_buffers: unsafe { ak::slice_from_raw_parts(buffers, buffer_count) }
                    .iter()
                    .map(|&buffer| {
                        // SAFETY: C++ lends live ArrayBuffers while the collector is deferred.
                        unsafe {
                            Gc::from_non_null(NonNull::new(buffer.cast::<ArrayBuffer>()).expect("C++ lends a buffer"))
                        }
                    })
                    .collect(),
                ..unsafe {
                    MessageRecord::received(
                        received.defer_gc,
                        bytes.as_slice(),
                        ak::slice_from_raw_parts(descriptors, shared_memory_count),
                        ak::slice_from_raw_parts(sizes, shared_memory_count),
                    )
                }
            });
        }

        unsafe extern "C" fn append(context: *mut c_void, holder: *mut c_void) {
            // SAFETY: from_cpp() lends the state, and C++ hands the holder over.
            unsafe {
                (*context.cast::<Received<'_>>())
                    .holders
                    .push(TransferDataCarrier::from_raw(holder));
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
