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

use crate::html::structured_serialize::cpp::{OwnedSerializedTransferRecord, SerializedTransferRecord};

unsafe extern "C" {
    fn web_bindings_transport_handle_destroy(handle: *mut c_void);
    fn web_bindings_transport_handle_create_transport(handle: *mut c_void) -> *mut c_void;
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

/// A handle to one end of an IPC transport that this owns: an IPC::TransportHandle, which holds the rights of that end
/// until a data holder carries them to another agent, or a transport takes them.
pub(crate) struct TransportHandle(NonNull<c_void>);

impl TransportHandle {
    /// # Safety
    ///
    /// `handle` must be a handle that LibWeb hands over.
    pub(crate) unsafe fn from_raw(handle: *mut c_void) -> Self {
        Self(NonNull::new(handle).expect("LibWeb hands over a handle"))
    }

    pub(crate) fn as_raw(&self) -> *mut c_void {
        self.0.as_ptr()
    }

    /// The transport of the end that the handle holds the rights of, which it gives up.
    pub(crate) fn create_transport(self) -> MessagePortTransport {
        // SAFETY: This owns the handle, and LibWeb hands over the transport.
        unsafe { MessagePortTransport::from_raw(web_bindings_transport_handle_create_transport(self.0.as_ptr())) }
    }
}

impl Drop for TransportHandle {
    fn drop(&mut self) {
        // SAFETY: This owns the handle.
        unsafe { web_bindings_transport_handle_destroy(self.0.as_ptr()) };
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
    pub(crate) fn send(&self, record: SerializedTransferRecord) -> bool {
        // SAFETY: This owns the transport, and the call borrows the record.
        unsafe { web_bindings_message_port_transport_send(self.raw.as_ptr(), record.as_raw()) }
    }

    /// Appends every message that has arrived, each a record, and tells whether the peer is gone.
    pub(crate) fn read_as_many_messages_as_possible_without_blocking(
        &self,
        records: &mut Vec<OwnedSerializedTransferRecord>,
    ) -> ShouldShutdown {
        unsafe extern "C" fn append(records: *mut c_void, record: *mut c_void) {
            // SAFETY: The records are the Vec that the read lends, and LibWeb hands over the record.
            unsafe {
                (*records.cast::<Vec<OwnedSerializedTransferRecord>>())
                    .push(OwnedSerializedTransferRecord::from_raw(record));
            }
        }
        // SAFETY: This owns the transport, and lends the records for the call.
        let should_shutdown = unsafe {
            web_bindings_message_port_transport_read_messages(
                self.raw.as_ptr(),
                core::ptr::from_mut(records).cast(),
                append,
            )
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
