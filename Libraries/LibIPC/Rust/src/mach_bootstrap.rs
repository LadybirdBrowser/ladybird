/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The server side of the Mach bootstrap, which hands a process that it spawned the ports of its transport.
//!
//! A helper looks up the name its spawner registered with the bootstrap server, and sends a request there with a
//! reply port. The listener takes requests on the registered port, and the server answers each with the ports it
//! holds for the requester's pid, or with a new transport.

use std::collections::HashMap;
use std::os::fd::{AsRawFd, FromRawFd, OwnedFd};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex, MutexGuard};
use std::thread::JoinHandle;

use crate::mach::{self, BootstrapRequest, KernReturn, KernelError, MachPort, ReceivedBootstrapRequest};
use crate::transport::MachPortPair;

fn lock<T>(mutex: &Mutex<T>) -> MutexGuard<'_, T> {
    mutex.lock().unwrap_or_else(|error| error.into_inner())
}

/// Takes the bootstrap requests that arrive on a port registered with the bootstrap server, on a thread of its own.
pub struct MachBootstrapListener {
    // NB: Shared with the listener's thread, and kept here as well, so that the port stays alive for the wakeup that
    //     stop() sends even when the thread has already returned.
    #[expect(dead_code, reason = "Only held to keep the port alive")]
    server_receive_right: Arc<MachPort>,
    server_send_right: MachPort,
    should_stop: Arc<AtomicBool>,
    thread: Mutex<Option<JoinHandle<()>>>,
}

impl MachBootstrapListener {
    /// Registers a new port under the name, and hands each request that arrives there to `on_request`, on the
    /// listener's thread.
    pub fn new(
        server_port_name: &str,
        on_request: impl Fn(BootstrapRequest) + Send + 'static,
    ) -> std::io::Result<Self> {
        let server_receive_right = MachPort::allocate_receive_right().map_err(KernelError)?;
        let server_send_right = server_receive_right.make_send_right().map_err(KernelError)?;
        mach::register_with_bootstrap_server(&server_receive_right, server_port_name)?;
        let server_receive_right = Arc::new(server_receive_right);
        let should_stop = Arc::new(AtomicBool::new(false));
        let thread = std::thread::Builder::new()
            .name("MachBootstrapListener".to_string())
            .spawn({
                let server_receive_right = Arc::clone(&server_receive_right);
                let should_stop = Arc::clone(&should_stop);
                move || listen(&server_receive_right, &should_stop, on_request)
            })?;
        Ok(Self {
            server_receive_right,
            server_send_right,
            should_stop,
            thread: Mutex::new(Some(thread)),
        })
    }

    /// Stops taking requests, and waits until the listener's thread is done. Stopping again, or meanwhile, waits for
    /// that as well.
    pub fn stop(&self) {
        // NB: The lock is held through the join, so that a concurrent stop also returns only once the thread is done.
        let mut listener_thread = lock(&self.thread);
        let Some(thread) = listener_thread.take() else {
            return;
        };
        self.should_stop.store(true, Ordering::Release);
        // NB: Wake the receive without waiting for room in the queue. A full queue already wakes the listener.
        let result = mach::send_header_only_message(&self.server_send_right, 0, Some(0));
        assert!(
            result == mach::MACH_MSG_SUCCESS || result == mach::MACH_SEND_TIMED_OUT,
            "the bootstrap listener is woken"
        );
        let _ = thread.join();
    }
}

impl Drop for MachBootstrapListener {
    fn drop(&mut self) {
        self.stop();
    }
}

fn listen(server_receive_right: &MachPort, should_stop: &AtomicBool, on_request: impl Fn(BootstrapRequest)) {
    while !should_stop.load(Ordering::Acquire) {
        let received = match mach::receive_bootstrap_request(server_receive_right) {
            Ok(received) => received,
            Err(error) => {
                eprintln!("MachBootstrapListener: mach_msg failed: {}", mach::error_string(error));
                return;
            }
        };
        // NB: Dropping what arrived as the listener stops gives back what it carries.
        if should_stop.load(Ordering::Acquire) {
            return;
        }
        match received {
            ReceivedBootstrapRequest::Request(request) => on_request(request),
            ReceivedBootstrapRequest::Ignored(what) => eprintln!("MachBootstrapListener: Ignoring {what}"),
        }
    }
}

/// Answers a bootstrap request with the ports of a new transport, and returns this end of it.
pub fn create_on_demand_local_transport(reply_port: Option<MachPort>) -> Result<MachPortPair, KernReturn> {
    let local_receive_right = MachPort::allocate_receive_right()?;
    let local_send_right = local_receive_right.make_send_right()?;
    let remote_receive_right = MachPort::allocate_receive_right()?;
    let remote_send_right = remote_receive_right.make_send_right()?;

    // NB: Either peer may send messages before the other has constructed its transport.
    local_receive_right.raise_queue_limit();
    remote_receive_right.raise_queue_limit();

    mach::send_transport_ports_to_child(reply_port, remote_receive_right, local_send_right)?;
    Ok(MachPortPair {
        receive_right: local_receive_right,
        send_right: remote_send_right,
    })
}

struct ChildTransport {
    ports: MachPortPair,
    // NB: Tells the exit watcher which registration an exit belongs to, as a later child can get the pid of an earlier
    //     one.
    serial: u64,
}

#[derive(Default)]
struct ChildTransports {
    by_pid: HashMap<i32, ChildTransport>,
    next_serial: u64,
}

/// Holds the ports of the transports of the children that have yet to ask for them. Until a child asks, nothing would
/// tell the other end of its transport that the child is gone, so the server lets go of the ports of a child that
/// exits before it asks, from a thread of its own.
pub struct TransportBootstrapServer {
    child_transports: Arc<Mutex<ChildTransports>>,
    child_exit_queue: Option<Arc<ChildExitQueue>>,
    child_exit_watcher: Mutex<Option<JoinHandle<()>>>,
}

impl Default for TransportBootstrapServer {
    fn default() -> Self {
        Self::new()
    }
}

impl TransportBootstrapServer {
    pub fn new() -> Self {
        let child_exit_queue = match ChildExitQueue::new() {
            Ok(queue) => Some(Arc::new(queue)),
            Err(error) => {
                eprintln!(
                    "Unable to create the queue that reports children that exit before they take their transport: \
                     {error}"
                );
                None
            }
        };
        Self {
            child_transports: Arc::default(),
            child_exit_queue,
            child_exit_watcher: Mutex::new(None),
        }
    }

    /// Holds the ports for the child until it asks for them, or exits. Ports still held for the pid are those of an
    /// earlier child that exited before it took them, and whose exit has not been handled yet. They are replaced.
    pub fn register_child_transport(&self, pid: i32, ports: MachPortPair) {
        let mut child_transports = lock(&self.child_transports);
        let serial = child_transports.next_serial;
        child_transports.next_serial += 1;

        if let Some(child_exit_queue) = &self.child_exit_queue {
            // NB: Without a watcher, nothing would let go of the ports of a child that exits before it asks, and the
            //     other end of its transport would never hear that it is gone. Letting go of them now tells it.
            if !self.start_watching_for_child_exits(child_exit_queue) {
                return;
            }
            if let Err(error) = child_exit_queue.watch_for_exit(pid, serial) {
                // NB: The child is gone already, so it never takes these ports. Letting go of them tells the other
                //     end.
                if error.raw_os_error() == Some(libc::ESRCH) {
                    return;
                }
                eprintln!("Unable to watch for the exit of child {pid}: {error}");
            }
        }

        child_transports.by_pid.insert(pid, ChildTransport { ports, serial });
    }

    /// Lets go of the ports of a child that was ended before it asked for them.
    pub fn unregister_child_transport(&self, pid: i32) {
        drop(self.take_child_transport(pid));
    }

    /// Takes the ports held for the child, for it to have them.
    pub fn take_child_transport(&self, pid: i32) -> Option<MachPortPair> {
        // NB: The watch goes under the lock, or it could go after a registration for a new child with the same pid.
        let mut child_transports = lock(&self.child_transports);
        let taken = child_transports.by_pid.remove(&pid)?;
        self.stop_watching_for_child_exit(pid);
        drop(child_transports);
        Some(taken.ports)
    }

    // NB: Answers whether a watcher runs.
    fn start_watching_for_child_exits(&self, child_exit_queue: &Arc<ChildExitQueue>) -> bool {
        let mut child_exit_watcher = lock(&self.child_exit_watcher);
        if child_exit_watcher.is_some() {
            return true;
        }
        let child_exit_queue = Arc::clone(child_exit_queue);
        let child_transports = Arc::clone(&self.child_transports);
        match std::thread::Builder::new()
            .name("ChildExitWatch".to_string())
            .spawn(move || watch_for_child_exits(&child_exit_queue, &child_transports))
        {
            Ok(thread) => {
                *child_exit_watcher = Some(thread);
                true
            }
            Err(error) => {
                eprintln!("Unable to start watching for children that exit: {error}");
                false
            }
        }
    }

    fn stop_watching_for_child_exit(&self, pid: i32) {
        // NB: This fails for a child whose exit was reported already, which is fine.
        if let Some(child_exit_queue) = &self.child_exit_queue {
            child_exit_queue.stop_watching_for_exit(pid);
        }
    }
}

impl Drop for TransportBootstrapServer {
    fn drop(&mut self) {
        let Some(thread) = lock(&self.child_exit_watcher).take() else {
            return;
        };
        if let Some(child_exit_queue) = &self.child_exit_queue {
            child_exit_queue.wake_to_stop();
        }
        let _ = thread.join();
    }
}

fn watch_for_child_exits(child_exit_queue: &ChildExitQueue, child_transports: &Mutex<ChildTransports>) {
    loop {
        match child_exit_queue.wait() {
            Ok(ChildExitEvent::Exited { pid, serial }) => {
                let mut child_transports = lock(child_transports);
                if child_transports
                    .by_pid
                    .get(&pid)
                    .is_some_and(|child_transport| child_transport.serial == serial)
                {
                    child_transports.by_pid.remove(&pid);
                }
            }
            Ok(ChildExitEvent::Stop) => return,
            Ok(ChildExitEvent::Other) => {}
            Err(error) => {
                eprintln!("Unable to wait for children to exit: {error}");
                return;
            }
        }
    }
}

enum ChildExitEvent {
    Exited { pid: i32, serial: u64 },
    Stop,
    Other,
}

/// A kqueue that reports children that exit, and a user event that wakes its waiter to stop.
struct ChildExitQueue {
    kqueue: OwnedFd,
}

// NB: What wakes the child exit watcher when the server goes away.
const STOP_WATCHING_EVENT_IDENTIFIER: usize = 1;

impl ChildExitQueue {
    #[allow(unsafe_code)]
    fn new() -> std::io::Result<Self> {
        // SAFETY: kqueue() takes no arguments, and returns a new descriptor or -1.
        let raw = unsafe { libc::kqueue() };
        if raw < 0 {
            return Err(std::io::Error::last_os_error());
        }
        // SAFETY: The descriptor is new, and nothing else owns it.
        let queue = Self {
            kqueue: unsafe { OwnedFd::from_raw_fd(raw) },
        };
        queue.change(
            STOP_WATCHING_EVENT_IDENTIFIER,
            libc::EVFILT_USER,
            libc::EV_ADD | libc::EV_CLEAR,
            0,
            0,
        )?;
        Ok(queue)
    }

    fn watch_for_exit(&self, pid: i32, serial: u64) -> std::io::Result<()> {
        self.change(
            pid as usize,
            libc::EVFILT_PROC,
            libc::EV_ADD | libc::EV_ONESHOT,
            libc::NOTE_EXIT,
            serial as usize,
        )
    }

    fn stop_watching_for_exit(&self, pid: i32) {
        let _ = self.change(pid as usize, libc::EVFILT_PROC, libc::EV_DELETE, 0, 0);
    }

    fn wake_to_stop(&self) {
        self.change(
            STOP_WATCHING_EVENT_IDENTIFIER,
            libc::EVFILT_USER,
            0,
            libc::NOTE_TRIGGER,
            0,
        )
        .expect("the child exit watcher is woken");
    }

    #[allow(unsafe_code)]
    fn change(
        &self,
        identifier: usize,
        filter: i16,
        flags: u16,
        filter_flags: u32,
        data: usize,
    ) -> std::io::Result<()> {
        let change = libc::kevent {
            ident: identifier,
            filter,
            flags,
            fflags: filter_flags,
            data: 0,
            udata: data as *mut libc::c_void,
        };
        // SAFETY: The change list holds one initialized event, and the event list is empty.
        let result = unsafe {
            libc::kevent(
                self.kqueue.as_raw_fd(),
                &raw const change,
                1,
                std::ptr::null_mut(),
                0,
                std::ptr::null(),
            )
        };
        if result < 0 {
            return Err(std::io::Error::last_os_error());
        }
        Ok(())
    }

    #[allow(unsafe_code)]
    fn wait(&self) -> std::io::Result<ChildExitEvent> {
        loop {
            // SAFETY: An all-zero kevent is a valid value, which the call overwrites.
            let mut event: libc::kevent = unsafe { std::mem::zeroed() };
            // SAFETY: The event list holds room for one event, and the change list is empty.
            let count = unsafe {
                libc::kevent(
                    self.kqueue.as_raw_fd(),
                    std::ptr::null(),
                    0,
                    &raw mut event,
                    1,
                    std::ptr::null(),
                )
            };
            if count < 0 {
                let error = std::io::Error::last_os_error();
                if error.kind() == std::io::ErrorKind::Interrupted {
                    continue;
                }
                return Err(error);
            }
            if count == 0 {
                continue;
            }
            return Ok(match event.filter {
                libc::EVFILT_USER => ChildExitEvent::Stop,
                libc::EVFILT_PROC => ChildExitEvent::Exited {
                    pid: event.ident as i32,
                    serial: event.udata as usize as u64,
                },
                _ => ChildExitEvent::Other,
            });
        }
    }
}
