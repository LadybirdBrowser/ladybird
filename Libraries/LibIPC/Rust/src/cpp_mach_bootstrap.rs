/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The Mach bootstrap as C++ `IPC::MachBootstrapListener`, `IPC::TransportBootstrapMachServer` and
//! `IPC::bootstrap_transport_from_server_port()` reach it.

#![allow(unsafe_code)]

use core::ffi::c_void;
use core::mem::ManuallyDrop;

use crate::mach::{self, BootstrapError, BootstrapRequest, MACH_PORT_NULL, MachPort, PortRight};
use crate::mach_bootstrap::{self as bootstrap, MachBootstrapListener, TransportBootstrapServer};
use crate::transport::MachPortPair;

/// What became of a request for the ports of this process's transport.
#[repr(C)]
pub enum BootstrapOutcome {
    Received,
    KernelError,
    ServerClosedBeforeReplying,
    MalformedReply,
}

fn borrowed(name: u32, right: PortRight) -> ManuallyDrop<MachPort> {
    ManuallyDrop::new(MachPort::adopt(name, right))
}

fn release(port: MachPort) -> u32 {
    ManuallyDrop::new(port).name()
}

fn optional_send_once_right(name: u32) -> Option<MachPort> {
    (name != MACH_PORT_NULL).then(|| MachPort::adopt(name, PortRight::SendOnce))
}

/// Asks the server behind a send right that the caller keeps for the ports of this process's transport. A kernel
/// failure leaves its code in `kernel_error`.
#[unsafe(no_mangle)]
pub extern "C" fn ipc_mach_bootstrap_transport_from_server_port(
    server_port: u32,
    reply_timeout_in_milliseconds: u32,
    receive_right: &mut u32,
    send_right: &mut u32,
    kernel_error: &mut i32,
) -> BootstrapOutcome {
    let server_port = borrowed(server_port, PortRight::Send);
    match mach::bootstrap_transport_from_server_port(&server_port, reply_timeout_in_milliseconds) {
        Ok((receive, send)) => {
            *receive_right = release(receive);
            *send_right = release(send);
            BootstrapOutcome::Received
        }
        Err(BootstrapError::Kernel { code, .. }) => {
            *kernel_error = code;
            BootstrapOutcome::KernelError
        }
        Err(BootstrapError::ServerClosedBeforeReplying) => BootstrapOutcome::ServerClosedBeforeReplying,
        Err(BootstrapError::MalformedReply) => BootstrapOutcome::MalformedReply,
    }
}

/// Takes bootstrap requests on a port it registers under the name, for as long as the listener lives.
pub struct BootstrapListener(MachBootstrapListener);

struct CppBootstrapRequestHandler {
    context: *mut c_void,
    handle: unsafe extern "C" fn(*mut c_void, i32, u32, u32),
}

// SAFETY: The C++ handler is made to run on the listener's thread.
unsafe impl Send for CppBootstrapRequestHandler {}

impl CppBootstrapRequestHandler {
    fn handle(&self, request: BootstrapRequest) {
        let reply_port = request.reply_port.map_or(MACH_PORT_NULL, release);
        // SAFETY: The handler takes over both rights, and the caller keeps the context valid while the listener lives.
        unsafe {
            (self.handle)(self.context, request.pid, release(request.task_name_port), reply_port);
        }
    }
}

/// A listener that hands each request to `handle_request` on its thread, with the requester's pid, its task name
/// port and its reply port, which may be null; or null if the port could not be registered.
///
/// # Safety
///
/// `context` must stay valid for `handle_request` until the listener is stopped, and the handler must take over both
/// rights.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ipc_mach_bootstrap_listener_create(
    server_port_name: ak::Span<'_, u8>,
    context: *mut c_void,
    handle_request: unsafe extern "C" fn(*mut c_void, i32, u32, u32),
) -> *mut BootstrapListener {
    let Ok(server_port_name) = core::str::from_utf8(server_port_name.as_slice()) else {
        return core::ptr::null_mut();
    };
    let handler = CppBootstrapRequestHandler {
        context,
        handle: handle_request,
    };
    match MachBootstrapListener::new(server_port_name, move |request| handler.handle(request)) {
        Ok(listener) => Box::into_raw(Box::new(BootstrapListener(listener))),
        Err(error) => {
            eprintln!("MachBootstrapListener: Unable to register {server_port_name}: {error}");
            core::ptr::null_mut()
        }
    }
}

/// Stops taking requests, and waits until the listener's thread is done.
#[unsafe(no_mangle)]
pub extern "C" fn ipc_mach_bootstrap_listener_stop(listener: &BootstrapListener) {
    listener.0.stop();
}

/// # Safety
///
/// The listener must be one from `ipc_mach_bootstrap_listener_create()` that nothing uses afterwards.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ipc_mach_bootstrap_listener_destroy(listener: *mut BootstrapListener) {
    // SAFETY: The caller gives the listener up.
    drop(unsafe { Box::from_raw(listener) });
}

/// Holds the ports of the transports of children that have yet to ask for them.
pub struct BootstrapServer(TransportBootstrapServer);

#[unsafe(no_mangle)]
pub extern "C" fn ipc_mach_bootstrap_server_create() -> *mut BootstrapServer {
    Box::into_raw(Box::new(BootstrapServer(TransportBootstrapServer::new())))
}

/// # Safety
///
/// The server must be one from `ipc_mach_bootstrap_server_create()` that nothing uses afterwards.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ipc_mach_bootstrap_server_destroy(server: *mut BootstrapServer) {
    // SAFETY: The caller gives the server up.
    drop(unsafe { Box::from_raw(server) });
}

/// # Safety
///
/// The caller must hand over a receive right and a send right.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ipc_mach_bootstrap_server_register_child_transport(
    server: &BootstrapServer,
    pid: i32,
    receive_right: u32,
    send_right: u32,
) {
    server.0.register_child_transport(
        pid,
        MachPortPair {
            receive_right: MachPort::adopt(receive_right, PortRight::Receive),
            send_right: MachPort::adopt(send_right, PortRight::Send),
        },
    );
}

#[unsafe(no_mangle)]
pub extern "C" fn ipc_mach_bootstrap_server_unregister_child_transport(server: &BootstrapServer, pid: i32) {
    server.0.unregister_child_transport(pid);
}

/// Takes the ports held for the child, answering false if none are.
#[unsafe(no_mangle)]
pub extern "C" fn ipc_mach_bootstrap_server_take_child_transport(
    server: &BootstrapServer,
    pid: i32,
    receive_right: &mut u32,
    send_right: &mut u32,
) -> bool {
    let Some(ports) = server.0.take_child_transport(pid) else {
        return false;
    };
    *receive_right = release(ports.receive_right);
    *send_right = release(ports.send_right);
    true
}

/// Answers a bootstrap request through its reply port, which may be null, with the ports of the requester's
/// transport, and returns the kernel's answer.
///
/// # Safety
///
/// The caller must hand over the reply port's send-once right, a receive right and a send right.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ipc_mach_bootstrap_send_transport_ports_to_child(
    reply_port: u32,
    receive_right: u32,
    send_right: u32,
) -> i32 {
    let result = mach::send_transport_ports_to_child(
        optional_send_once_right(reply_port),
        MachPort::adopt(receive_right, PortRight::Receive),
        MachPort::adopt(send_right, PortRight::Send),
    );
    result.err().unwrap_or(mach::KERN_SUCCESS)
}

/// Answers a bootstrap request through its reply port, which may be null, with the ports of a new transport, and
/// returns the kernel's answer and, on success, this end of the transport.
///
/// # Safety
///
/// The caller must hand over the reply port's send-once right.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ipc_mach_bootstrap_create_on_demand_local_transport(
    reply_port: u32,
    receive_right: &mut u32,
    send_right: &mut u32,
) -> i32 {
    match bootstrap::create_on_demand_local_transport(optional_send_once_right(reply_port)) {
        Ok(ports) => {
            *receive_right = release(ports.receive_right);
            *send_right = release(ports.send_right);
            mach::KERN_SUCCESS
        }
        Err(error) => error,
    }
}
