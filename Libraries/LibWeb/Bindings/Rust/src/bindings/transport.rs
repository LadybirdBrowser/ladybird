/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What LibWeb's C++ IPC transports lend to Rust: the transport that carries the messages of a MessagePort between
//! agents, and the handle of one end of such a transport, which a data holder carries. The Rust bindings hold them as
//! opaque pointers, and reach them through the C ABI of RustBindingsServices.cpp.

use core::any::Any;
use core::ffi::c_void;
use core::ptr::NonNull;

use crate::html::structured_serialize::cpp::OwnedSerializedTransferRecord;
use crate::html::structured_serialize::transfer_data_holder::{
    RawAttachment, ReceivePayload, TransferPayload, append_attachments,
};
use crate::html::structured_serialize::transfer_record::{TransferRecord, TransferRecordRef};
use libipc_rust::{Attachment, DecodeError, Decoder, Encoder};
use libjs_rust::api::{MarkedVec, Vm};

unsafe extern "C" {
    fn web_structured_serialize_transfer_handle_take(
        handle: *mut c_void,
        context: *mut c_void,
        receive: ReceivePayload,
    );
    fn web_structured_serialize_transfer_handle_create_transport(
        bytes: ak::Span<'_, u8>,
        attachments: *const RawAttachment,
        count: usize,
    ) -> *mut c_void;
    fn web_bindings_message_port_transport_create_paired(out_remote_handle: &mut *mut c_void) -> *mut c_void;
    fn web_bindings_message_port_transport_destroy(transport: *mut c_void);
    fn web_bindings_message_port_transport_release_for_transfer(transport: *mut c_void) -> *mut c_void;
    fn web_bindings_message_port_transport_set_up_read_hook(
        transport: *mut c_void,
        state: *mut c_void,
        steps: unsafe extern "C" fn(*mut c_void),
    );
    fn web_bindings_message_port_transport_is_open(transport: *mut c_void) -> bool;
    fn web_bindings_message_port_transport_close(transport: *mut c_void);
    fn web_bindings_message_port_transport_close_after_sending_all_pending_messages(transport: *mut c_void);
    fn web_bindings_message_port_transport_send(transport: *mut c_void, record: *mut c_void) -> bool;
    fn web_bindings_message_port_transport_read_messages(
        transport: *mut c_void,
        records: *mut c_void,
        append: unsafe extern "C" fn(*mut c_void, *mut c_void),
    ) -> bool;
}

/// The rights of one end of an IPC transport, which this owns until a data holder carries them to another agent, or a
/// transport takes them.
pub(crate) struct TransportHandle {
    attachments: Vec<Attachment>,
}

impl TransportHandle {
    /// # Safety
    ///
    /// `handle` must be a handle that LibWeb hands over.
    pub(crate) unsafe fn from_raw(handle: *mut c_void) -> Self {
        let mut payload = None;
        // SAFETY: The call takes the native handle and fills the lent payload slot.
        unsafe {
            web_structured_serialize_transfer_handle_take(handle, (&raw mut payload).cast(), TransferPayload::receive);
        };
        let payload: TransferPayload = payload.expect("C++ hands over the handle's payload");
        let mut decoder = Decoder::new(&payload.bytes, payload.attachments);
        let handle = Self::decode(&mut decoder).expect("a native handle has the transport's rights");
        assert!(decoder.is_at_end() && decoder.remaining_attachments() == 0);
        handle
    }

    pub(crate) fn encode_into(self, encoder: &mut Encoder) {
        append_attachments(encoder, self.attachments);
    }

    pub(crate) fn decode(decoder: &mut Decoder<'_>) -> Result<Self, DecodeError> {
        let count = if cfg!(target_vendor = "apple") { 2 } else { 1 };
        if decoder.remaining_attachments() < count {
            return Err(DecodeError::MissingAttachment);
        }
        let mut attachments = Vec::with_capacity(count);
        for index in 0..count {
            let attachment = decoder.take_attachment()?;
            #[cfg(target_vendor = "apple")]
            let attachment = {
                use libipc_rust::mach::{
                    MACH_MSG_TYPE_MOVE_RECEIVE, MACH_MSG_TYPE_MOVE_SEND, MACH_PORT_DEAD, MACH_PORT_NULL,
                };
                let (port, disposition) = attachment.into_mach_port();
                let expected = if index == 0 {
                    MACH_MSG_TYPE_MOVE_RECEIVE
                } else {
                    MACH_MSG_TYPE_MOVE_SEND
                };
                if disposition != expected
                    || port.name() == MACH_PORT_NULL
                    || (index == 0 && port.name() == MACH_PORT_DEAD)
                {
                    return Err(DecodeError::InvalidValue("invalid transport handle right"));
                }
                Attachment::from_mach_port(port, disposition)
            };
            #[cfg(not(target_vendor = "apple"))]
            let _ = index;
            attachments.push(attachment);
        }
        Ok(Self { attachments })
    }

    /// The transport of the end that the handle holds the rights of, which it gives up.
    pub(crate) fn create_transport(self) -> MessagePortTransport {
        let payload = TransferPayload {
            bytes: Vec::new(),
            attachments: self.attachments,
        };
        payload.with_raw(|bytes, attachments| {
            // SAFETY: The call takes every right, and LibWeb hands over the transport.
            unsafe {
                MessagePortTransport::from_raw(web_structured_serialize_transfer_handle_create_transport(
                    bytes,
                    attachments.as_ptr(),
                    attachments.len(),
                ))
            }
        })
    }
}

/// Whether the peer of a transport is gone, so that nothing more arrives: IPC::Transport::ShouldShutdown.
#[derive(Clone, Copy, PartialEq, Eq)]
pub(crate) enum ShouldShutdown {
    No,
    Yes,
}

/// The read hook of a transport: its steps, and the state they run with.
struct ReadHook<S> {
    state: S,
    steps: fn(&S),
}

/// The transport of one end of the channel between two MessagePorts entangled across agents, whose messages are the
/// records of structured serializations with transfer: an IPC::Transport that this owns. Its IO thread receives the
/// messages, and its read hook runs on the owner thread when they arrive.
pub(crate) struct MessagePortTransport {
    raw: NonNull<c_void>,
    /// The read hook, which the transport is lent until it is destroyed: drop() destroys the transport before this.
    read_hook: Option<Box<dyn Any>>,
}

impl MessagePortTransport {
    /// IPC::Transport::create_paired(): the transport of one end of a new channel, and the handle of the other end.
    pub(crate) fn create_paired() -> (Self, TransportHandle) {
        let mut remote_handle = core::ptr::null_mut();
        // SAFETY: LibWeb hands over a new transport and the handle.
        unsafe {
            let transport = web_bindings_message_port_transport_create_paired(&mut remote_handle);
            (Self::from_raw(transport), TransportHandle::from_raw(remote_handle))
        }
    }

    /// # Safety
    ///
    /// `transport` must be a transport that LibWeb hands over.
    unsafe fn from_raw(transport: *mut c_void) -> Self {
        Self {
            raw: NonNull::new(transport).expect("LibWeb hands over a transport"),
            read_hook: None,
        }
    }

    /// Takes the rights of this end back as a handle, for a data holder to carry to another agent. The IO thread stops
    /// first, so that what the peer sends from then on arrives at the handle's new owner, while what it sent before can
    /// still be read.
    pub(crate) fn release_for_transfer(&self) -> TransportHandle {
        // SAFETY: This owns the transport, and LibWeb hands over the handle.
        unsafe {
            TransportHandle::from_raw(web_bindings_message_port_transport_release_for_transfer(
                self.raw.as_ptr(),
            ))
        }
    }

    /// Runs the steps with the state on the owner thread whenever messages arrive, for as long as the transport lives.
    pub(crate) fn set_up_read_hook<S: 'static>(&mut self, state: S, steps: fn(&S)) {
        unsafe extern "C" fn run<S>(hook: *mut c_void) {
            // SAFETY: The transport calls its hook with the one that set_up_read_hook() lent it, which outlives the
            //         transport.
            let hook = unsafe { &*hook.cast::<ReadHook<S>>() };
            (hook.steps)(&hook.state);
        }
        let hook = Box::new(ReadHook { state, steps });
        let raw_hook: *const ReadHook<S> = &raw const *hook;
        // SAFETY: This owns the transport, and lends it the hook until it is destroyed, before the hook.
        unsafe {
            web_bindings_message_port_transport_set_up_read_hook(
                self.raw.as_ptr(),
                raw_hook.cast_mut().cast(),
                run::<S>,
            );
        }
        self.read_hook = Some(hook);
    }

    pub(crate) fn is_open(&self) -> bool {
        // SAFETY: This owns the transport.
        unsafe { web_bindings_message_port_transport_is_open(self.raw.as_ptr()) }
    }

    pub(crate) fn close(&self) {
        // SAFETY: This owns the transport.
        unsafe { web_bindings_message_port_transport_close(self.raw.as_ptr()) };
    }

    pub(crate) fn close_after_sending_all_pending_messages(&self) {
        // SAFETY: This owns the transport.
        unsafe { web_bindings_message_port_transport_close_after_sending_all_pending_messages(self.raw.as_ptr()) };
    }

    /// Sends the record to the peer as one message. False if the transport failed, which LibWeb logs.
    pub(crate) fn send(&self, vm: &Vm, record: TransferRecordRef) -> bool {
        let record = TransferRecord::into_cpp(record, vm);
        // SAFETY: This owns the transport, and the call borrows the record.
        unsafe { web_bindings_message_port_transport_send(self.raw.as_ptr(), record.record().as_raw()) }
    }

    /// Appends every message that has arrived, each a record, and tells whether the peer is gone.
    pub(crate) fn read_as_many_messages_as_possible_without_blocking(
        &self,
        vm: &Vm,
        records: &MarkedVec<'_, TransferRecordRef>,
    ) -> ShouldShutdown {
        unsafe extern "C" fn append(context: *mut c_void, record: *mut c_void) {
            // SAFETY: The read lends its VM and rooted list, and C++ hands over the carrier.
            let (vm, records) = unsafe { &*context.cast::<(&Vm, &MarkedVec<'_, TransferRecordRef>)>() };
            let carrier = unsafe { OwnedSerializedTransferRecord::from_raw(record) };
            records.push(TransferRecord::from_cpp(vm, carrier.record()));
        }
        let mut context = (vm, records);
        // SAFETY: This owns the transport, and lends the records for the call.
        let should_shutdown = unsafe {
            web_bindings_message_port_transport_read_messages(self.raw.as_ptr(), (&raw mut context).cast(), append)
        };
        if should_shutdown {
            ShouldShutdown::Yes
        } else {
            ShouldShutdown::No
        }
    }
}

impl Drop for MessagePortTransport {
    fn drop(&mut self) {
        // SAFETY: This owns the transport. Its read hook, which the state outlives, goes with it.
        unsafe { web_bindings_message_port_transport_destroy(self.raw.as_ptr()) };
    }
}
