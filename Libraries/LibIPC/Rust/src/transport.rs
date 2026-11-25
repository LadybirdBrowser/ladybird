/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What the transports of every platform share: what the owner of a transport gets to see of what arrives, the
//! bookkeeping of the IO thread that every transport of a process shares, and what callers hand a transport to send.

use std::cell::Cell;
use std::collections::VecDeque;
use std::fs::File;
use std::io::Write;
use std::os::fd::OwnedFd;
use std::sync::atomic::{AtomicBool, AtomicU8, Ordering};
use std::sync::{Arc, Condvar, Mutex, MutexGuard, OnceLock};
use std::time::Duration;

pub use crate::receive_queue::ReceivedMessage;
#[cfg(target_vendor = "apple")]
pub use crate::transport_mach_port::{MachPortPair, TransportMachPort};
#[cfg(all(unix, not(target_vendor = "apple")))]
pub use crate::transport_socket::{SocketHandle, TransportSocket};

/// The transport of this platform.
#[cfg(target_vendor = "apple")]
pub type Transport = TransportMachPort;
#[cfg(all(unix, not(target_vendor = "apple")))]
pub type Transport = TransportSocket;

/// What the IO thread shows every message that arrives, before the transport's owner gets to see it, and empty bytes
/// once the peer is gone.
pub type IncomingMessageObserver = Box<dyn Fn(&[u8]) + Send>;

/// The bytes of a message that waits to be sent, which it owns, wherever they are.
pub type OwnedPayload = Box<dyn AsRef<[u8]> + Send>;

/// The time on `CLOCK_MONOTONIC`, which AK's `MonotonicTime` reads, and which keeps counting while the system sleeps.
#[allow(unsafe_code)]
pub(crate) fn clock_monotonic_nanoseconds() -> i64 {
    let mut now = libc::timespec { tv_sec: 0, tv_nsec: 0 };
    // NB: now points to a local that the call writes to, and the clock always exists.
    unsafe { libc::clock_gettime(libc::CLOCK_MONOTONIC, &raw mut now) };
    now.tv_sec * 1_000_000_000 + now.tv_nsec
}

/// A point in time on `CLOCK_MONOTONIC`, the clock of AK's `MonotonicTime`, which keeps counting while the system
/// sleeps. A wait checks it again after every wakeup, so that it ends when its caller's deadline passes.
#[derive(Clone, Copy)]
pub struct MonotonicDeadline {
    nanoseconds: i64,
}

impl MonotonicDeadline {
    pub fn from_nanoseconds(nanoseconds: i64) -> Self {
        Self { nanoseconds }
    }

    fn remaining(self) -> Option<Duration> {
        let now = clock_monotonic_nanoseconds();
        (now < self.nanoseconds).then(|| Duration::from_nanos(self.nanoseconds.abs_diff(now)))
    }
}

/// The bytes of a message to post: owned already, or lent by a caller that hands them over only if the message has to
/// wait, as C++ does with the buffer it encoded the message into.
pub enum OutgoingPayload<'a> {
    Owned(Vec<u8>),
    Lent {
        bytes: &'a [u8],
        hand_over: &'a mut dyn FnMut() -> OwnedPayload,
    },
}

impl OutgoingPayload<'_> {
    pub(crate) fn bytes(&self) -> &[u8] {
        match self {
            Self::Owned(bytes) => bytes,
            Self::Lent { bytes, .. } => bytes,
        }
    }
}

pub(crate) const IDLE_MEMORY_RELEASE_DELAY_IN_MILLISECONDS: u32 = 1000;

pub(crate) fn release_unused_allocator_memory() {
    // NB: The messages the IO thread receives are freed by the threads that handle them, and the allocator keeps that
    //     memory until this thread allocates again or collects, which an idle process may never do. Unit tests
    //     don't link AK, and use the system allocator.
    #[cfg(ladybird_links_ak)]
    ak::release_unused_allocator_memory();
}

pub(crate) fn lock<T>(mutex: &Mutex<T>) -> MutexGuard<'_, T> {
    mutex.lock().unwrap_or_else(|error| error.into_inner())
}

pub(crate) fn wait<'a, T>(condition: &Condvar, guard: MutexGuard<'a, T>) -> MutexGuard<'a, T> {
    condition.wait(guard).unwrap_or_else(|error| error.into_inner())
}

pub(crate) fn wait_for<'a, T>(
    condition: &Condvar,
    guard: MutexGuard<'a, T>,
    timeout: Duration,
) -> (MutexGuard<'a, T>, bool) {
    let (guard, result) = condition
        .wait_timeout(guard, timeout)
        .unwrap_or_else(|error| error.into_inner());
    (guard, result.timed_out())
}

#[derive(Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub(crate) enum IoThreadState {
    Running = 0,
    SendPendingMessagesAndStop = 1,
    Stopped = 2,
}

pub(crate) struct AtomicIoThreadState(AtomicU8);

impl AtomicIoThreadState {
    pub(crate) fn new() -> Self {
        Self(AtomicU8::new(IoThreadState::Running as u8))
    }

    pub(crate) fn load(&self) -> IoThreadState {
        match self.0.load(Ordering::Acquire) {
            0 => IoThreadState::Running,
            1 => IoThreadState::SendPendingMessagesAndStop,
            _ => IoThreadState::Stopped,
        }
    }

    pub(crate) fn store(&self, state: IoThreadState) {
        self.0.store(state as u8, Ordering::Release);
    }

    // NB: The IO thread may already have stopped on its own, as when the peer hung up. Stopped is final, so it is never
    //     replaced by a new stop request.
    pub(crate) fn request_stop(&self, desired_state: IoThreadState) {
        let mut state = self.0.load(Ordering::Acquire);
        while state != IoThreadState::Stopped as u8 {
            match self
                .0
                .compare_exchange(state, desired_state as u8, Ordering::AcqRel, Ordering::Acquire)
            {
                Ok(_) => break,
                Err(current) => state = current,
            }
        }
    }
}

/// What a transport has taken in for its owner, under the transport's incoming lock. `Barrier` is what the transport
/// keeps there to tell when everything its peer sent before a receive barrier has arrived.
#[derive(Default)]
pub(crate) struct Incoming<Barrier> {
    pub(crate) messages: VecDeque<ReceivedMessage>,
    // NB: Set once this end's IO is over, which ends a wait for messages that can no longer come, even when the
    //     transport's end went to a new owner and the peer is not gone.
    pub(crate) local_io_is_over: bool,
    pub(crate) barrier: Barrier,
    wait_until_readable_interrupted: bool,
    read_notification_pending: bool,
    read_notification_pipe: Option<File>,
    // NB: The owner mostly hears of messages through the pipe, so a change wakes the condition variable only when
    //     something waits on it.
    waiter_count: usize,
}

/// The messages a transport's IO thread has taken in for the owner, and how the owner waits for them, which is the
/// same for every transport.
pub(crate) struct Inbox<Barrier> {
    // NB: Written under the incoming lock, so that a reader under it sees it with the messages that came before.
    peer_eof: AtomicBool,
    incoming: Mutex<Incoming<Barrier>>,
    changed: Condvar,
    // NB: This has a lock of its own, which is taken before the incoming lock: the observer may interrupt a wait.
    incoming_message_observer: Mutex<Option<IncomingMessageObserver>>,
}

impl<Barrier: Default> Inbox<Barrier> {
    /// An inbox that writes a byte to the pipe when messages are there to read, or the peer is gone, and nothing more
    /// until `acknowledge_read_notification()`.
    pub(crate) fn new(read_notification_pipe: Option<OwnedFd>) -> Self {
        Self {
            peer_eof: AtomicBool::new(false),
            incoming: Mutex::new(Incoming {
                read_notification_pipe: read_notification_pipe.map(File::from),
                ..Incoming::default()
            }),
            changed: Condvar::new(),
            incoming_message_observer: Mutex::new(None),
        }
    }
}

impl<Barrier> Inbox<Barrier> {
    pub(crate) fn lock(&self) -> MutexGuard<'_, Incoming<Barrier>> {
        lock(&self.incoming)
    }

    pub(crate) fn wait<'a>(
        &self,
        mut incoming: MutexGuard<'a, Incoming<Barrier>>,
    ) -> MutexGuard<'a, Incoming<Barrier>> {
        incoming.waiter_count += 1;
        let mut incoming = wait(&self.changed, incoming);
        incoming.waiter_count -= 1;
        incoming
    }

    fn wait_for<'a>(
        &self,
        mut incoming: MutexGuard<'a, Incoming<Barrier>>,
        timeout: Duration,
    ) -> MutexGuard<'a, Incoming<Barrier>> {
        incoming.waiter_count += 1;
        let mut incoming = wait_for(&self.changed, incoming, timeout).0;
        incoming.waiter_count -= 1;
        incoming
    }

    /// Changes the incoming state, and wakes whatever waits for a change.
    pub(crate) fn update<Result>(&self, change: impl FnOnce(&mut Incoming<Barrier>) -> Result) -> Result {
        let (result, has_waiters) = {
            let mut incoming = self.lock();
            let result = change(&mut incoming);
            (result, incoming.waiter_count > 0)
        };
        if has_waiters {
            self.changed.notify_all();
        }
        result
    }

    pub(crate) fn peer_eof(&self) -> bool {
        self.peer_eof.load(Ordering::Acquire)
    }

    #[cfg(all(test, not(target_vendor = "apple")))]
    pub(crate) fn has_waiters(&self) -> bool {
        self.lock().waiter_count > 0
    }

    pub(crate) fn notify_incoming_message_observer<'a>(&self, messages: impl IntoIterator<Item = &'a [u8]>) {
        // NB: The observer runs under its lock, so once a new one is set, the old one is no longer running.
        if let Some(observer) = &*lock(&self.incoming_message_observer) {
            for bytes in messages {
                observer(bytes);
            }
        }
    }

    fn schedule_read_notification(incoming: &mut Incoming<Barrier>) {
        if incoming.read_notification_pending {
            return;
        }
        incoming.read_notification_pending = true;
        if let Some(pipe) = &incoming.read_notification_pipe {
            // NB: An interrupted write wrote nothing, so it is retried. The pipe does not block, and a full one already
            //     wakes the owner.
            let _ = (&*pipe).write_all(&[0]);
        }
    }

    /// Hands the messages to the owner, which the observer has seen already, along with what `change` does to the
    /// incoming state under the same lock.
    pub(crate) fn deliver(
        &self,
        messages: impl IntoIterator<Item = ReceivedMessage>,
        change: impl FnOnce(&mut Incoming<Barrier>),
    ) {
        self.update(|incoming| {
            let was_empty = incoming.messages.is_empty();
            incoming.messages.extend(messages);
            change(incoming);
            if was_empty && !incoming.messages.is_empty() {
                Self::schedule_read_notification(incoming);
            }
        });
    }

    /// Hands the last messages of the peer to the owner along with its departure, so that the owner can't take them
    /// without learning that the peer is gone.
    pub(crate) fn deliver_with_peer_eof(
        &self,
        messages: impl IntoIterator<Item = ReceivedMessage>,
        change: impl FnOnce(&mut Incoming<Barrier>),
    ) {
        self.notify_incoming_message_observer([&[][..]]);
        self.update(|incoming| {
            incoming.messages.extend(messages);
            change(incoming);
            self.peer_eof.store(true, Ordering::Release);
            Self::schedule_read_notification(incoming);
        });
    }

    pub(crate) fn mark_peer_eof(&self) {
        if !self.peer_eof() {
            self.deliver_with_peer_eof([], |_| {});
        }
    }

    pub(crate) fn end_local_io(&self) {
        self.update(|incoming| incoming.local_io_is_over = true);
    }

    /// Waits for the next message, or returns `None` once the peer is gone and every message it sent was taken.
    pub(crate) fn receive(&self) -> Option<ReceivedMessage> {
        let mut incoming = self.lock();
        loop {
            if let Some(message) = incoming.messages.pop_front() {
                return Some(message);
            }
            if self.peer_eof() || incoming.local_io_is_over {
                return None;
            }
            incoming = self.wait(incoming);
        }
    }

    /// Takes every message that has arrived, and whether the peer was gone as they were taken. Later arrivals can't
    /// be skipped by reporting the peer gone.
    pub(crate) fn take_incoming(&self) -> (Vec<ReceivedMessage>, bool) {
        let mut incoming = self.lock();
        let messages = std::mem::take(&mut incoming.messages).into();
        (messages, self.peer_eof())
    }

    /// Waits until there is a message to take, the peer is gone, the wait is interrupted, or the deadline passes.
    pub(crate) fn wait_until_readable(&self, deadline: Option<MonotonicDeadline>) {
        let mut incoming = self.lock();
        while incoming.messages.is_empty()
            && !self.peer_eof()
            && !incoming.local_io_is_over
            && !incoming.wait_until_readable_interrupted
        {
            match deadline {
                None => incoming = self.wait(incoming),
                Some(deadline) => {
                    let Some(remaining) = deadline.remaining() else {
                        break;
                    };
                    incoming = self.wait_for(incoming, remaining);
                }
            }
        }
        incoming.wait_until_readable_interrupted = false;
    }

    pub(crate) fn interrupt_wait_until_readable(&self) {
        self.update(|incoming| incoming.wait_until_readable_interrupted = true);
    }

    pub(crate) fn set_incoming_message_observer(
        &self,
        observer: Option<IncomingMessageObserver>,
    ) -> Option<IncomingMessageObserver> {
        std::mem::replace(&mut *lock(&self.incoming_message_observer), observer)
    }

    pub(crate) fn close_read_notification_pipe(&self) {
        let pipe = self.lock().read_notification_pipe.take();
        drop(pipe);
    }

    pub(crate) fn acknowledge_read_notification(&self) {
        self.lock().read_notification_pending = false;
    }

    pub(crate) fn schedule_read_notification_if_readable(&self) {
        let mut incoming = self.lock();
        if !incoming.messages.is_empty() || self.peer_eof() {
            Self::schedule_read_notification(&mut incoming);
        }
    }
}

/// Where a transport stands with the IO thread that every transport of the process shares. Both change only under the
/// lock of that thread's attention list.
#[derive(Default)]
pub(crate) struct IoThreadMembership {
    needs_attention: AtomicBool,
    io_is_finished: AtomicBool,
}

pub(crate) trait SharesIoThread {
    fn io_thread_membership(&self) -> &IoThreadMembership;
}

/// The transports that asked the shared IO thread to send their backlog, take note of a peer they found gone, or stop
/// their IO, and how a transport's owner waits for its IO to be over.
pub(crate) struct AttentionList<Transport> {
    transports: Mutex<Vec<Arc<Transport>>>,
    io_finished: Condvar,
}

impl<Transport: SharesIoThread> AttentionList<Transport> {
    pub(crate) fn new() -> Self {
        Self {
            transports: Mutex::new(Vec::new()),
            io_finished: Condvar::new(),
        }
    }

    /// Puts the transport on the list, and answers whether the IO thread should be woken for it, which it need not be
    /// once the transport's IO is over.
    pub(crate) fn request(&self, transport: &Arc<Transport>) -> bool {
        let mut transports = lock(&self.transports);
        let membership = transport.io_thread_membership();
        if membership.io_is_finished.load(Ordering::Relaxed) {
            return false;
        }
        if !membership.needs_attention.load(Ordering::Relaxed) {
            membership.needs_attention.store(true, Ordering::Relaxed);
            transports.push(Arc::clone(transport));
        }
        true
    }

    pub(crate) fn is_empty(&self) -> bool {
        lock(&self.transports).is_empty()
    }

    /// Moves the transports that asked for attention into `into`, which the IO thread serves next.
    pub(crate) fn take(&self, into: &mut Vec<Arc<Transport>>) {
        let mut transports = lock(&self.transports);
        std::mem::swap(into, &mut *transports);
        for transport in into.iter() {
            transport
                .io_thread_membership()
                .needs_attention
                .store(false, Ordering::Relaxed);
        }
    }

    pub(crate) fn wait_until_io_is_finished(&self, transport: &Transport) {
        assert!(
            !IS_SHARED_IO_THREAD.get(),
            "the shared IO thread must not wait for itself"
        );
        let mut transports = lock(&self.transports);
        while !transport.io_thread_membership().io_is_finished.load(Ordering::Relaxed) {
            transports = wait(&self.io_finished, transports);
        }
    }

    // NB: The owner may destroy the transport as soon as this returns.
    pub(crate) fn mark_io_finished(&self, transport: &Arc<Transport>) {
        let mut transports = lock(&self.transports);
        let membership = transport.io_thread_membership();
        if membership.needs_attention.load(Ordering::Relaxed) {
            transports.retain(|other| !Arc::ptr_eq(other, transport));
            membership.needs_attention.store(false, Ordering::Relaxed);
        }
        membership.io_is_finished.store(true, Ordering::Relaxed);
        self.io_finished.notify_all();
    }
}

thread_local! {
    static IS_SHARED_IO_THREAD: Cell<bool> = const { Cell::new(false) };
}

pub(crate) fn is_shared_io_thread() -> bool {
    IS_SHARED_IO_THREAD.get()
}

/// Starts the IO thread that every transport of the process shares, the first time a transport is made. Nothing waits
/// for the thread, which goes with the process. If it can't start, what `create` made goes with the last reference,
/// and the next transport tries again.
pub(crate) fn start_shared_io_thread<IoThread: Send + Sync + 'static>(
    io_thread: &'static OnceLock<Arc<IoThread>>,
    name: &str,
    create: impl FnOnce() -> std::io::Result<IoThread>,
    run: fn(&IoThread),
) -> std::io::Result<&'static IoThread> {
    static STARTING: Mutex<()> = Mutex::new(());
    if let Some(running) = io_thread.get() {
        return Ok(running);
    }
    let _starting = lock(&STARTING);
    if let Some(running) = io_thread.get() {
        return Ok(running);
    }
    let created = Arc::new(create()?);
    std::thread::Builder::new().name(name.to_string()).spawn({
        let created = Arc::clone(&created);
        move || {
            IS_SHARED_IO_THREAD.set(true);
            run(&created);
        }
    })?;
    Ok(io_thread.get_or_init(|| created))
}
