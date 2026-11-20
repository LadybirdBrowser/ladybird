/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The Mach port transport as C++ `IPC::TransportMachPort` reaches it.
//!
//! Only LibIPC's copy of the crate is built with this, so the other libraries that bundle the crate don't export it.

#![allow(unsafe_code)]

use core::ffi::{c_int, c_void};
use std::os::fd::{FromRawFd, OwnedFd};

use crate::cpp_connection::RawAttachment;
use crate::mach::{MachPort, OwnedPayload, PayloadBytes, PortRight};
use crate::transport::{IncomingMessageObserver, MonotonicDeadline, OutgoingPayload, TransportMachPort};

/// A transport, which C++ owns from `ipc_mach_transport_create()` to `ipc_mach_transport_destroy()`.
pub struct MachTransport(TransportMachPort);

/// The bytes of a received message, which C++ owns until `ipc_mach_received_payload_destroy()`.
pub struct ReceivedPayload(PayloadBytes);

/// Raises the number of messages a receive right queues, before a transport takes it.
#[unsafe(no_mangle)]
pub extern "C" fn ipc_mach_raise_receive_queue_limit(receive_right: u32) {
    // NB: The name only borrows the caller's right.
    let receive_right = core::mem::ManuallyDrop::new(MachPort::adopt(receive_right, PortRight::Receive));
    receive_right.raise_queue_limit();
}

/// A transport that takes over both rights and the write end of the read-notification pipe, or null if it could not
/// start, having released them.
///
/// # Safety
///
/// The caller must hand over a receive right, a send right (which may be a dead name), and an open descriptor.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ipc_mach_transport_create(
    receive_right: u32,
    send_right: u32,
    read_notification_pipe: c_int,
) -> *mut MachTransport {
    let receive_right = MachPort::adopt(receive_right, PortRight::Receive);
    let send_right = MachPort::adopt(send_right, PortRight::Send);
    // SAFETY: The caller hands over the descriptor.
    let read_notification_pipe = unsafe { OwnedFd::from_raw_fd(read_notification_pipe) };
    match TransportMachPort::with_read_notification_pipe(receive_right, send_right, Some(read_notification_pipe)) {
        Ok(transport) => Box::into_raw(Box::new(MachTransport(transport))),
        Err(error) => {
            eprintln!("TransportMachPort: Unable to start: {error}");
            core::ptr::null_mut()
        }
    }
}

/// Stops the transport's IO, and waits until the IO thread no longer runs its observer.
///
/// # Safety
///
/// The transport must be one from `ipc_mach_transport_create()` that nothing uses afterwards.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ipc_mach_transport_destroy(transport: *mut MachTransport) {
    // SAFETY: The caller gives the transport up.
    drop(unsafe { Box::from_raw(transport) });
}

/// The buffer C++ encoded a message into, once it hands it over for the message to wait in: where the bytes are, and
/// how to release them.
#[repr(C)]
pub struct HandedOverPayload {
    data: *const u8,
    size: usize,
    owner: *mut c_void,
    release: unsafe extern "C" fn(*mut c_void),
}

struct CppPayload(HandedOverPayload);

// SAFETY: The C++ buffer is plain memory, which any thread may read and release.
unsafe impl Send for CppPayload {}

impl AsRef<[u8]> for CppPayload {
    fn as_ref(&self) -> &[u8] {
        // SAFETY: The owner keeps size bytes at data until it is released, which only dropping this does.
        unsafe { ak::slice_from_raw_parts(self.0.data, self.0.size) }
    }
}

impl Drop for CppPayload {
    fn drop(&mut self) {
        // SAFETY: This owns the payload, which nothing reads afterwards.
        unsafe { (self.0.release)(self.0.owner) };
    }
}

/// Sends a message, or queues it behind earlier ones that wait for room in the peer's queue, taking every right the
/// attachments name. The bytes are lent; only a message that has to wait behind others asks `hand_over_payload` for
/// them. Answers false for a transport that has stopped sending.
///
/// # Safety
///
/// Each raw attachment must hand over one right, and `hand_over_payload`, called at most once, must hand over the
/// lent bytes with `payload_owner`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ipc_mach_transport_post(
    transport: &MachTransport,
    bytes: ak::Span<'_, u8>,
    attachments: ak::Span<'_, RawAttachment>,
    payload_owner: *mut c_void,
    hand_over_payload: unsafe extern "C" fn(*mut c_void) -> HandedOverPayload,
) -> bool {
    // SAFETY: The caller hands over what each raw attachment names.
    let attachments = unsafe { RawAttachment::take_all(attachments.as_slice()) };
    let mut hand_over = || -> OwnedPayload {
        // SAFETY: The caller hands the bytes over, once.
        Box::new(CppPayload(unsafe { hand_over_payload(payload_owner) }))
    };
    let payload = OutgoingPayload::Lent {
        bytes: bytes.as_slice(),
        hand_over: &mut hand_over,
    };
    transport
        .0
        .post_message_with_any_number_of_attachments(payload, attachments)
        .is_ok()
}

/// Hands each message that has arrived to `on_message`, and answers whether the peer was gone as they were taken. The
/// callback owns the payload and every right the attachments name, and may call back into the transport.
///
/// # Safety
///
/// `on_message` must take over the payload and each raw attachment's right.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ipc_mach_transport_take_incoming(
    transport: &MachTransport,
    context: *mut c_void,
    on_message: unsafe extern "C" fn(*mut c_void, ak::Span<'_, u8>, *mut ReceivedPayload, ak::Span<'_, RawAttachment>),
) -> bool {
    let (messages, peer_eof) = transport.0.take_incoming();
    for message in messages {
        let (bytes, attachments) = message.into_parts();
        let raw_attachments: Vec<RawAttachment> = attachments.into_iter().map(RawAttachment::from_attachment).collect();
        let payload = Box::into_raw(Box::new(ReceivedPayload(bytes)));
        // SAFETY: The payload lives on the heap until the callback destroys it, so the bytes stay where they are.
        let payload_bytes = unsafe { (*payload).0.bytes() };
        // SAFETY: The callback takes over the payload and the rights.
        unsafe {
            on_message(
                context,
                ak::Span::new(payload_bytes),
                payload,
                ak::Span::new(&raw_attachments),
            );
        }
    }
    peer_eof
}

/// # Safety
///
/// The payload must be one that `ipc_mach_transport_take_incoming()` handed over, which nothing uses afterwards.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ipc_mach_received_payload_destroy(payload: *mut ReceivedPayload) {
    // SAFETY: The caller gives the payload up.
    drop(unsafe { Box::from_raw(payload) });
}

/// Waits until there is a message to take, the peer is gone, or the wait is interrupted, and no longer than the
/// deadline if there is one, in nanoseconds of the clock of AK's `MonotonicTime`.
#[unsafe(no_mangle)]
pub extern "C" fn ipc_mach_transport_wait_until_readable(
    transport: &MachTransport,
    has_deadline: bool,
    deadline_in_monotonic_nanoseconds: i64,
) {
    transport.0.wait_until_readable(
        has_deadline.then(|| MonotonicDeadline::from_nanoseconds(deadline_in_monotonic_nanoseconds)),
    );
}

#[unsafe(no_mangle)]
pub extern "C" fn ipc_mach_transport_interrupt_wait_until_readable(transport: &MachTransport) {
    transport.0.interrupt_wait_until_readable();
}

#[unsafe(no_mangle)]
pub extern "C" fn ipc_mach_transport_flush(transport: &MachTransport) {
    transport.0.flush();
}

#[unsafe(no_mangle)]
pub extern "C" fn ipc_mach_transport_wait_until_incoming_is_current(transport: &MachTransport) {
    transport.0.wait_until_incoming_is_current();
}

#[unsafe(no_mangle)]
pub extern "C" fn ipc_mach_transport_is_open(transport: &MachTransport) -> bool {
    transport.0.is_open()
}

#[unsafe(no_mangle)]
pub extern "C" fn ipc_mach_transport_close(transport: &MachTransport) {
    transport.0.close();
}

#[unsafe(no_mangle)]
pub extern "C" fn ipc_mach_transport_close_after_sending_all_pending_messages(transport: &MachTransport) {
    transport.0.close_after_sending_all_pending_messages();
}

#[unsafe(no_mangle)]
pub extern "C" fn ipc_mach_transport_acknowledge_read_notification(transport: &MachTransport) {
    transport.0.acknowledge_read_notification();
}

#[unsafe(no_mangle)]
pub extern "C" fn ipc_mach_transport_schedule_read_notification_if_readable(transport: &MachTransport) {
    transport.0.schedule_read_notification_if_readable();
}

/// Hands both rights over for another transport to take this end's place, answering false if they are gone already.
#[unsafe(no_mangle)]
pub extern "C" fn ipc_mach_transport_release_for_transfer(
    transport: &MachTransport,
    receive_right: &mut u32,
    send_right: &mut u32,
) -> bool {
    let Ok(pair) = transport.0.release_for_transfer() else {
        return false;
    };
    *receive_right = core::mem::ManuallyDrop::new(pair.receive_right).name();
    *send_right = core::mem::ManuallyDrop::new(pair.send_right).name();
    true
}

/// A C++ observer, which the transport owns once it is set: what it calls, and how to release it.
#[repr(C)]
pub struct CppIncomingMessageObserver {
    context: *mut c_void,
    observe: unsafe extern "C" fn(*mut c_void, ak::Span<'_, u8>),
    release: unsafe extern "C" fn(*mut c_void),
}

// SAFETY: The C++ observer is made to run on the IO thread, and to be released on any thread.
unsafe impl Send for CppIncomingMessageObserver {}

impl CppIncomingMessageObserver {
    fn observe(&self, bytes: &[u8]) {
        // SAFETY: The context lives until this releases it.
        unsafe { (self.observe)(self.context, ak::Span::new(bytes)) };
    }
}

impl Drop for CppIncomingMessageObserver {
    fn drop(&mut self) {
        // SAFETY: This owns the context, which nothing calls afterwards.
        unsafe { (self.release)(self.context) };
    }
}

/// Sets what the IO thread shows every message that arrives, and empty bytes once the peer is gone, or clears it
/// with a null `observer`. The transport owns the observer from here on, and releases the one it replaces once that
/// is no longer running, which this call does, so that concurrent calls can't release one that is still set.
///
/// # Safety
///
/// A non-null `observer` must point to one that hands its context over to the transport.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ipc_mach_transport_set_incoming_message_observer(
    transport: &MachTransport,
    observer: Option<&CppIncomingMessageObserver>,
) {
    let observer = observer.map(|observer| {
        let observer = CppIncomingMessageObserver {
            context: observer.context,
            observe: observer.observe,
            release: observer.release,
        };
        Box::new(move |bytes: &[u8]| observer.observe(bytes)) as IncomingMessageObserver
    });
    drop(transport.0.set_incoming_message_observer(observer));
}
