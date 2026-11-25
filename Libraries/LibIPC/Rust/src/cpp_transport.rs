/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The transports as C++ `IPC::Transport` reaches them: the Mach port one on Darwin, and the socket one on other Unix.
//!
//! Only LibIPC's copy of the crate is built with this, so the other libraries that bundle the crate don't export it.

#![allow(unsafe_code)]

use core::ffi::{c_int, c_void};
use std::os::fd::{FromRawFd, OwnedFd};

use crate::cpp_connection::RawAttachment;
#[cfg(target_vendor = "apple")]
use crate::mach::{MachPort, PortRight};
use crate::receive_queue::PayloadBytes;
#[cfg(not(target_vendor = "apple"))]
use crate::transport::SocketHandle;
use crate::transport::{IncomingMessageObserver, MonotonicDeadline, OutgoingPayload, OwnedPayload, Transport};

/// A transport, which C++ owns from its creation to `ipc_transport_destroy()`.
pub struct RustTransport(Transport);

/// The bytes of a received message, which C++ owns until `ipc_received_payload_destroy()`.
pub struct ReceivedPayload(PayloadBytes);

fn into_cpp_transport(transport: std::io::Result<Transport>) -> *mut RustTransport {
    match transport {
        Ok(transport) => Box::into_raw(Box::new(RustTransport(transport))),
        Err(error) => {
            eprintln!("Transport: Unable to start: {error}");
            core::ptr::null_mut()
        }
    }
}

/// Raises the number of messages a receive right queues, before a transport takes it.
#[cfg(target_vendor = "apple")]
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
#[cfg(target_vendor = "apple")]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ipc_transport_create_from_mach_ports(
    receive_right: u32,
    send_right: u32,
    read_notification_pipe: c_int,
) -> *mut RustTransport {
    let receive_right = MachPort::adopt(receive_right, PortRight::Receive);
    let send_right = MachPort::adopt(send_right, PortRight::Send);
    // SAFETY: The caller hands over the descriptor.
    let read_notification_pipe = unsafe { OwnedFd::from_raw_fd(read_notification_pipe) };
    into_cpp_transport(Transport::with_read_notification_pipe(
        receive_right,
        send_right,
        Some(read_notification_pipe),
    ))
}

/// A transport that takes over the socket, what its last owner read off it without seeing the end of a message, and
/// the write end of the read-notification pipe, or null if it could not start, having released them.
///
/// # Safety
///
/// The caller must hand over two descriptors, each open or negative, and what each raw attachment names.
#[cfg(not(target_vendor = "apple"))]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ipc_transport_create_from_socket(
    socket: c_int,
    read_notification_pipe: c_int,
    unprocessed_bytes: ak::Span<'_, u8>,
    unprocessed_attachments: ak::Span<'_, RawAttachment>,
) -> *mut RustTransport {
    // SAFETY: The caller hands over what the raw attachments name.
    let unprocessed_attachments = unsafe { RawAttachment::take_all(unprocessed_attachments.as_slice()) };
    // SAFETY: The caller hands over each descriptor that is valid.
    let adopt = |fd: c_int| (fd >= 0).then(|| unsafe { OwnedFd::from_raw_fd(fd) });
    let (Some(socket), Some(read_notification_pipe)) = (adopt(socket), adopt(read_notification_pipe)) else {
        return core::ptr::null_mut();
    };
    let handle = SocketHandle {
        socket,
        unprocessed_bytes: unprocessed_bytes.as_slice().to_vec(),
        unprocessed_attachments,
    };
    into_cpp_transport(Transport::with_read_notification_pipe(
        handle,
        Some(read_notification_pipe),
    ))
}

/// Stops the transport's IO, and waits until the IO thread no longer runs its observer.
///
/// # Safety
///
/// The transport must be one that C++ owns, which nothing uses afterwards.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ipc_transport_destroy(transport: *mut RustTransport) {
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

/// Sends a message, or queues it behind earlier ones that wait for the peer to make room, taking whatever the
/// attachments name. The bytes are lent; only a message that has to wait behind others asks `hand_over_payload` for
/// them. Answers false for a transport that has stopped sending.
///
/// # Safety
///
/// Each raw attachment must hand over one right, and `hand_over_payload`, called at most once, must hand over the
/// lent bytes with `payload_owner`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ipc_transport_post(
    transport: &RustTransport,
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
pub unsafe extern "C" fn ipc_transport_take_incoming(
    transport: &RustTransport,
    context: *mut c_void,
    on_message: unsafe extern "C" fn(*mut c_void, ak::Span<'_, u8>, *mut ReceivedPayload, ak::Span<'_, RawAttachment>),
) -> bool {
    let (messages, peer_eof) = transport.0.take_incoming();
    for message in messages {
        let (bytes, attachments) = message.into_parts();
        let raw_attachments: Vec<RawAttachment> = attachments.into_iter().map(RawAttachment::from_attachment).collect();
        let payload = Box::into_raw(Box::new(ReceivedPayload(bytes)));
        // SAFETY: The payload lives on the heap until the callback destroys it, so the bytes stay where they are.
        let payload_bytes: &[u8] = unsafe { (*payload).0.as_ref() };
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
/// The payload must be one that `ipc_transport_take_incoming()` handed over, which nothing uses afterwards.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ipc_received_payload_destroy(payload: *mut ReceivedPayload) {
    // SAFETY: The caller gives the payload up.
    drop(unsafe { Box::from_raw(payload) });
}

/// Waits until there is a message to take, the peer is gone, or the wait is interrupted, and no longer than the
/// deadline if there is one, in nanoseconds of the clock of AK's `MonotonicTime`.
#[unsafe(no_mangle)]
pub extern "C" fn ipc_transport_wait_until_readable(
    transport: &RustTransport,
    has_deadline: bool,
    deadline_in_monotonic_nanoseconds: i64,
) {
    transport.0.wait_until_readable(
        has_deadline.then(|| MonotonicDeadline::from_nanoseconds(deadline_in_monotonic_nanoseconds)),
    );
}

#[unsafe(no_mangle)]
pub extern "C" fn ipc_transport_interrupt_wait_until_readable(transport: &RustTransport) {
    transport.0.interrupt_wait_until_readable();
}

#[unsafe(no_mangle)]
pub extern "C" fn ipc_transport_flush(transport: &RustTransport) {
    transport.0.flush();
}

#[unsafe(no_mangle)]
pub extern "C" fn ipc_transport_wait_until_incoming_is_current(transport: &RustTransport) {
    transport.0.wait_until_incoming_is_current();
}

#[unsafe(no_mangle)]
pub extern "C" fn ipc_transport_is_open(transport: &RustTransport) -> bool {
    transport.0.is_open()
}

#[unsafe(no_mangle)]
pub extern "C" fn ipc_transport_close(transport: &RustTransport) {
    transport.0.close();
}

#[unsafe(no_mangle)]
pub extern "C" fn ipc_transport_close_after_sending_all_pending_messages(transport: &RustTransport) {
    transport.0.close_after_sending_all_pending_messages();
}

#[unsafe(no_mangle)]
pub extern "C" fn ipc_transport_acknowledge_read_notification(transport: &RustTransport) {
    transport.0.acknowledge_read_notification();
}

#[unsafe(no_mangle)]
pub extern "C" fn ipc_transport_schedule_read_notification_if_readable(transport: &RustTransport) {
    transport.0.schedule_read_notification_if_readable();
}

/// Hands both rights over for another transport to take this end's place, answering false if they are gone already.
#[cfg(target_vendor = "apple")]
#[unsafe(no_mangle)]
pub extern "C" fn ipc_transport_release_mach_ports_for_transfer(
    transport: &RustTransport,
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

/// Hands the socket over for another transport to take this end's place, along with what was read of a message that
/// has yet to arrive whole, which `on_released` takes over. Answers false if the socket is gone already.
///
/// # Safety
///
/// `on_released` must take over the socket and what each raw attachment names.
#[cfg(not(target_vendor = "apple"))]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ipc_transport_release_socket_for_transfer(
    transport: &RustTransport,
    context: *mut c_void,
    on_released: unsafe extern "C" fn(*mut c_void, c_int, ak::Span<'_, u8>, ak::Span<'_, RawAttachment>),
) -> bool {
    use std::os::fd::IntoRawFd;

    let Ok(handle) = transport.0.release_for_transfer() else {
        return false;
    };
    let raw_attachments: Vec<RawAttachment> = handle
        .unprocessed_attachments
        .into_iter()
        .map(RawAttachment::from_attachment)
        .collect();
    // SAFETY: The callback takes over the socket and what the raw attachments name.
    unsafe {
        on_released(
            context,
            handle.socket.into_raw_fd(),
            ak::Span::new(&handle.unprocessed_bytes),
            ak::Span::new(&raw_attachments),
        );
    }
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
pub unsafe extern "C" fn ipc_transport_set_incoming_message_observer(
    transport: &RustTransport,
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
