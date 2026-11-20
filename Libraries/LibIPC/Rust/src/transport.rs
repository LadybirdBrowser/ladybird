/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The Mach port transport, which speaks with `IPC::TransportMachPort` on the other end.
//!
//! All transports of a process share one IO thread. It waits on one port set, which holds the receive right of each
//! transport and the port on which a transport hears that its peer's full queue has room. The thread that posts a
//! message hands it to the kernel itself, unless earlier messages still wait for room in the peer's queue; then it
//! waits behind them, and the IO thread sends them as the peer makes room. The peer going away shows as
//! `MACH_NOTIFY_NO_SENDERS` on the receive right, once the peer's last send right to it is gone.

use std::cell::Cell;
use std::collections::{HashMap, VecDeque};
use std::fs::File;
use std::io::Write;
use std::os::fd::OwnedFd;
use std::sync::atomic::{AtomicBool, AtomicU8, Ordering};
use std::sync::{Arc, Condvar, Mutex, MutexGuard, OnceLock};
use std::time::{Duration, Instant};

use crate::MAX_MESSAGE_ATTACHMENT_COUNT;
use crate::attachment::Attachment;
use crate::mach::{
    self, KernReturn, KernelError, MachPort, MessageBuffer, OwnedPayload, PendingMessage, PortName, ReceiveError,
    SendOutcome,
};
pub use crate::receive_queue::ReceivedMessage;

/// The receive right and send right that make up one end of a connection, as `IPC::TransportHandle` holds them.
pub struct MachPortPair {
    pub receive_right: MachPort,
    pub send_right: MachPort,
}

/// What the IO thread shows every message that arrives, before the transport's owner gets to see it, and empty bytes
/// once the peer is gone.
pub type IncomingMessageObserver = Box<dyn Fn(&[u8]) + Send>;

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
        let now = mach::clock_monotonic_nanoseconds();
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
    fn bytes(&self) -> &[u8] {
        match self {
            Self::Owned(bytes) => bytes,
            Self::Lent { bytes, .. } => bytes,
        }
    }

    fn into_owned(self) -> OwnedPayload {
        match self {
            Self::Owned(bytes) => Box::new(bytes),
            Self::Lent { hand_over, .. } => hand_over(),
        }
    }
}

const MAX_BACKLOG_MESSAGES_SENT_PER_TURN: usize = 64;
const MAX_MESSAGES_RECEIVED_BETWEEN_TURNS_OF_SENDING: usize = 64;
const IDLE_MEMORY_RELEASE_DELAY_IN_MILLISECONDS: u32 = 1000;
const RETRY_DELAY: Duration = Duration::from_millis(1);
const FLUSH_WAKEUP_REARM_INTERVAL: Duration = Duration::from_millis(50);

#[derive(Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
enum IoThreadState {
    Running = 0,
    SendPendingMessagesAndStop = 1,
    Stopped = 2,
}

// One turn of sending on the IO thread, as far as the peer has room, and the stop asked for.
enum PendingIo {
    None,
    MoreToSendNextTurn,
    // The peer's queue is full, and the kernel could not be asked to say when it has room.
    RetryAfterDelay,
    IoIsOver,
}

struct SendState {
    send_right: Option<MachPort>,
    // NB: Messages the peer's queue had no room for, in order. While there are any, a newly posted message waits
    //     behind them, and the IO thread sends them as the peer makes room.
    backlog: VecDeque<PendingMessage>,
    sending_is_over: bool,
    // NB: Set once a send fails, which the kernel only does for a message that can't reach the peer, and after which
    //     nothing more is taken to send. What the peer sent before keeps arriving.
    peer_is_unreachable: bool,
    // NB: Grows on the first send, so that a transport which only receives keeps no send storage.
    buffer: MessageBuffer,
    // NB: Where the kernel says that the peer's full queue has room, from the first time it was full.
    send_possible_notification_port: Option<MachPort>,
    send_possible_notification_is_requested: bool,
}

#[derive(Default)]
struct Incoming {
    messages: VecDeque<ReceivedMessage>,
    // NB: Set once this end's IO is over, which ends a wait for messages that can no longer come, even when the rights
    //     went to a new owner and the peer is not gone.
    local_io_is_over: bool,
    wait_until_readable_interrupted: bool,
    receive_barriers_sent: u64,
    receive_barriers_received: u64,
    read_notification_pending: bool,
    read_notification_pipe: Option<File>,
}

struct Shared {
    receive_port_name: PortName,
    io_thread_state: AtomicU8,
    is_open: AtomicBool,
    // NB: Written under the incoming lock, so that a reader under it sees it with the messages that came before.
    peer_eof: AtomicBool,
    // NB: True while release_for_transfer() moves the rights to a new owner. Shutdown of this end is then part of the
    //     handoff, not the peer going away.
    is_being_transferred: AtomicBool,
    // NB: Set by a thread that found the peer gone as it posted, for the IO thread to tell the owner, as it does when
    //     it finds that itself, so that the incoming message observer only runs there.
    posting_thread_found_peer_gone: AtomicBool,
    peer_unreachable_marker_was_sent: AtomicBool,
    // NB: As SendState::peer_is_unreachable, for is_open() to read without the send lock.
    peer_is_unreachable: AtomicBool,
    // NB: Set before the owner asks IO to stop. IO that stops on its own, as for a peer that misbehaves, hands what is
    //     left unsent to the IO thread to release, as nothing else would until the owner lets go of the transport.
    stop_was_requested_by_owner: AtomicBool,
    // NB: These two change only under the lock of the shared IO thread's attention list.
    needs_attention: AtomicBool,
    io_is_finished: AtomicBool,

    // NB: Held by whichever thread sends, which is the thread that posts a message unless the message has to wait.
    send: Mutex<SendState>,
    sent: Condvar,

    incoming: Mutex<Incoming>,
    incoming_changed: Condvar,

    // NB: This has a lock of its own, which is taken before the incoming lock: the observer may interrupt a wait.
    incoming_message_observer: Mutex<Option<IncomingMessageObserver>>,
}

fn lock<T>(mutex: &Mutex<T>) -> MutexGuard<'_, T> {
    mutex.lock().unwrap_or_else(|error| error.into_inner())
}

fn wait<'a, T>(condition: &Condvar, guard: MutexGuard<'a, T>) -> MutexGuard<'a, T> {
    condition.wait(guard).unwrap_or_else(|error| error.into_inner())
}

fn wait_for<'a, T>(condition: &Condvar, guard: MutexGuard<'a, T>, timeout: Duration) -> (MutexGuard<'a, T>, bool) {
    let (guard, result) = condition
        .wait_timeout(guard, timeout)
        .unwrap_or_else(|error| error.into_inner());
    (guard, result.timed_out())
}

impl Shared {
    fn io_thread_state(&self) -> IoThreadState {
        match self.io_thread_state.load(Ordering::Acquire) {
            0 => IoThreadState::Running,
            1 => IoThreadState::SendPendingMessagesAndStop,
            _ => IoThreadState::Stopped,
        }
    }

    fn set_io_thread_state(&self, state: IoThreadState) {
        self.io_thread_state.store(state as u8, Ordering::Release);
    }

    fn identity(self: &Arc<Self>) -> usize {
        Arc::as_ptr(self) as usize
    }

    fn notify_incoming_message_observer(&self, bytes: &[u8]) {
        // NB: The observer runs under its lock, so once a new one is set, the old one is no longer running.
        if let Some(observer) = &*lock(&self.incoming_message_observer) {
            observer(bytes);
        }
    }

    fn schedule_read_notification(incoming: &mut Incoming) {
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

    fn mark_peer_eof(&self) {
        self.notify_incoming_message_observer(&[]);
        {
            let mut incoming = lock(&self.incoming);
            self.peer_eof.store(true, Ordering::Release);
            Self::schedule_read_notification(&mut incoming);
        }
        self.incoming_changed.notify_all();
    }

    // NB: The peer may have sent messages that are still on the receive right, which the IO thread has yet to take in,
    //     and which the owner gets to read before it hears that the peer is gone. A marker sent to the receive right
    //     follows them, and the peer's departure shows once the marker arrives. A send-once right bypasses the queue
    //     limit, so sending it doesn't wait.
    fn mark_peer_eof_behind_messages_already_sent(&self) {
        if self.peer_unreachable_marker_was_sent.swap(true, Ordering::AcqRel) {
            return;
        }
        // NB: The owner keeps the receive right until IO is over, and this runs on the IO thread before that.
        let marker_was_sent =
            mach::make_send_once_right_from_receive_right_named(self.receive_port_name).is_ok_and(|marker_right| {
                mach::send_header_only_message_through_send_once_right(
                    marker_right,
                    mach::IPC_PEER_UNREACHABLE_MESSAGE_ID,
                ) == mach::MACH_MSG_SUCCESS
            });
        if !marker_was_sent {
            self.mark_peer_eof();
        }
    }

    // NB: Answers whether to go on taking messages from the peer.
    fn handle_message_on_io_thread(&self, message: mach::ReceivedMessage) -> bool {
        match message.id {
            mach::IPC_RECEIVE_BARRIER_MESSAGE_ID => {
                lock(&self.incoming).receive_barriers_received += 1;
                self.incoming_changed.notify_all();
                true
            }
            mach::MACH_NOTIFY_NO_SENDERS => {
                self.mark_peer_eof();
                true
            }
            // NB: The peer may still hold a send right to this end after it let go of its receive right. What it sends
            //     after the marker would come after the owner heard that it is gone, so nothing more is taken from it.
            mach::IPC_PEER_UNREACHABLE_MESSAGE_ID => {
                self.mark_peer_eof();
                self.set_io_thread_state(IoThreadState::Stopped);
                false
            }
            mach::MACH_NOTIFY_SEND_ONCE => true,
            mach::IPC_DATA_MESSAGE_ID | mach::IPC_INLINE_DATA_MESSAGE_ID => {
                if self.process_received_message(message) {
                    return true;
                }
                self.set_io_thread_state(IoThreadState::Stopped);
                false
            }
            id => {
                eprintln!("TransportMachPort: Rejecting message with unknown id {id:#x}");
                self.mark_peer_eof();
                self.set_io_thread_state(IoThreadState::Stopped);
                false
            }
        }
    }

    // NB: Answers false for a message the peer had no business sending, after which the transport takes nothing more
    //     from it.
    fn process_received_message(&self, message: mach::ReceivedMessage) -> bool {
        let Some((bytes, ports)) = message.data else {
            self.mark_peer_eof();
            return false;
        };
        // NB: The rights and the payload region are ours once received, so dropping them releases them.
        if ports.len() > MAX_MESSAGE_ATTACHMENT_COUNT {
            eprintln!(
                "TransportMachPort: Rejecting message with {} attachments exceeding limit {MAX_MESSAGE_ATTACHMENT_COUNT}",
                ports.len()
            );
            self.mark_peer_eof();
            return false;
        }
        if bytes.bytes().is_empty() && ports.is_empty() {
            return true;
        }

        self.notify_incoming_message_observer(bytes.bytes());

        let attachments = ports
            .into_iter()
            .map(|(port, disposition)| Attachment::from_mach_port(port, disposition))
            .collect();
        {
            let mut incoming = lock(&self.incoming);
            let was_empty = incoming.messages.is_empty();
            incoming.messages.push_back(ReceivedMessage { bytes, attachments });
            if was_empty {
                Self::schedule_read_notification(&mut incoming);
            }
        }
        self.incoming_changed.notify_all();
        true
    }

    // NB: The request is armed as it is made, and answered right away if the peer's queue has room by then. Answers
    //     whether it was made. A transport that can't make one retries after a delay instead.
    fn request_send_possible_notification(self: &Arc<Self>, send: &mut SendState) -> bool {
        assert!(!send.send_possible_notification_is_requested);
        let io_thread = shared_io_thread();
        if send.send_possible_notification_port.is_none() {
            let Ok(port) = MachPort::allocate_receive_right() else {
                return false;
            };
            if io_thread
                .add(Registration::SendPossibleNotification(Arc::clone(self)), &port)
                .is_err()
            {
                return false;
            }
            send.send_possible_notification_port = Some(port);
        }
        let (Some(destination), Some(notification_port)) = (&send.send_right, &send.send_possible_notification_port)
        else {
            return false;
        };
        if !io_thread.claim_send_possible_request(self.identity(), destination) {
            return false;
        }
        match destination.request_send_possible_notification(notification_port) {
            Ok(None) => {
                send.send_possible_notification_is_requested = true;
                true
            }
            Ok(Some(replaced)) => {
                // NB: Something besides the transports made a request on the name after the claim checked it, and
                //     this one took its place. It goes back, as the dead-name request that anything but a transport
                //     makes.
                destination.restore_dead_name_notification(replaced);
                io_thread.release_send_possible_request(self.identity(), destination.name());
                false
            }
            Err(_) => {
                io_thread.release_send_possible_request(self.identity(), destination.name());
                false
            }
        }
    }

    fn handle_send_possible_notification(self: &Arc<Self>, message: &mach::ReceivedMessage) {
        if !consumes_send_possible_request(message) {
            return;
        }
        let mut send = lock(&self.send);
        send.send_possible_notification_is_requested = false;
        if let Some(destination) = &send.send_right {
            shared_io_thread().release_send_possible_request(self.identity(), destination.name());
        }
    }

    fn send(self: &Arc<Self>, send: &mut SendState, message: Outgoing<'_>) -> SendResult {
        let SendState { send_right, buffer, .. } = &mut *send;
        let Some(destination) = send_right.as_ref() else {
            return SendResult::PeerGone;
        };
        let outcome = match message {
            Outgoing::Waiting(pending) => pending.try_send(buffer, destination),
            Outgoing::New { payload, ports } => mach::send_data_message(buffer, destination, payload.bytes(), ports),
        };
        match outcome {
            SendOutcome::Sent => SendResult::Sent,
            SendOutcome::WouldBlock(pending) => {
                if !send.send_possible_notification_is_requested {
                    let _ = self.request_send_possible_notification(send);
                }
                SendResult::WouldBlock(pending)
            }
            SendOutcome::Failed(error) => {
                eprintln!("TransportMachPort: send failed: {}", mach::error_string(error));
                send.peer_is_unreachable = true;
                self.peer_is_unreachable.store(true, Ordering::Release);
                SendResult::PeerGone
            }
        }
    }

    // NB: The backlog is retried while the IO thread keeps receiving: a thread that blocked in a send would stop
    //     draining its own queue, and two such threads facing each other would wait on each other for good.
    fn do_pending_io(self: &Arc<Self>) -> PendingIo {
        if self.io_thread_state() == IoThreadState::Stopped {
            return PendingIo::IoIsOver;
        }

        let mut peer_is_gone = self.posting_thread_found_peer_gone.swap(false, Ordering::AcqRel);
        let mut pending_io = PendingIo::None;
        let mut unsendable = Vec::new();
        {
            let mut send = lock(&self.send);
            let had_backlog = !send.backlog.is_empty();
            // NB: A turn sends a bounded part of a long backlog, as of one that a peer takes again after a while, so
            //     that the IO thread gets back to receiving, and to the other transports, in between.
            let mut peer_queue_is_full = false;
            // NB: After a failed send, nothing more goes out: a message that came after one the peer never got would
            //     arrive out of turn.
            for _ in 0..MAX_BACKLOG_MESSAGES_SENT_PER_TURN {
                if send.peer_is_unreachable {
                    break;
                }
                let Some(message) = send.backlog.pop_front() else {
                    break;
                };
                match self.send(&mut send, Outgoing::Waiting(message)) {
                    SendResult::Sent => {}
                    SendResult::WouldBlock(message) => {
                        send.backlog.push_front(message);
                        peer_queue_is_full = true;
                        break;
                    }
                    SendResult::PeerGone => peer_is_gone = true,
                }
            }
            // NB: What can no longer be sent may carry rights that keep other connections open, so it goes without
            //     waiting for the owner, a bounded part per turn as releasing rights takes a while.
            if send.peer_is_unreachable {
                let count = send.backlog.len().min(MAX_BACKLOG_MESSAGES_SENT_PER_TURN);
                unsendable.extend(send.backlog.drain(..count));
                self.sent.notify_all();
            }
            if send.backlog.is_empty() {
                if had_backlog {
                    // NB: A queue that has drained keeps its capacity, which this lets go of.
                    send.backlog = VecDeque::new();
                    self.sent.notify_all();
                }
                // NB: Posting threads check the state under this lock, so nothing is left behind in the backlog.
                if self.io_thread_state() == IoThreadState::SendPendingMessagesAndStop {
                    self.set_io_thread_state(IoThreadState::Stopped);
                    pending_io = PendingIo::IoIsOver;
                }
            } else if send.peer_is_unreachable || !peer_queue_is_full {
                pending_io = PendingIo::MoreToSendNextTurn;
            } else if !send.send_possible_notification_is_requested {
                pending_io = PendingIo::RetryAfterDelay;
            }
        }
        drop(unsendable);
        if peer_is_gone {
            self.mark_peer_eof_behind_messages_already_sent();
        }
        pending_io
    }

    // NB: Runs on the IO thread as IO is over, after the receive right has left the port set. Gives back what is left
    //     unsent for the IO thread to release, unless the owner asked IO to stop, and releases it itself.
    fn stop_sending(self: &Arc<Self>) -> VecDeque<PendingMessage> {
        let io_thread = shared_io_thread();
        let mut send = lock(&self.send);
        send.sending_is_over = true;
        let unsendable = if self.stop_was_requested_by_owner.load(Ordering::Acquire) {
            VecDeque::new()
        } else {
            std::mem::take(&mut send.backlog)
        };
        // NB: A request that cancelling does not take back has been used up, and the kernel owes a notification for
        //     it, which need not be queued yet. One about a dead port holds a reference to the dead name, to release.
        let mut notification_is_owed = false;
        if send.send_possible_notification_is_requested {
            send.send_possible_notification_is_requested = false;
            match &send.send_right {
                Some(destination) => {
                    notification_is_owed = !destination.cancel_send_possible_notification();
                    io_thread.release_send_possible_request(self.identity(), destination.name());
                }
                None => notification_is_owed = true,
            }
        }
        if let Some(port) = send.send_possible_notification_port.take() {
            let mut buffer = MessageBuffer::new(256);
            while let Ok(Some(message)) = mach::receive_message_within(&mut buffer, &port, 0) {
                if consumes_send_possible_request(&message) {
                    notification_is_owed = false;
                }
            }
            if notification_is_owed {
                io_thread.keep_until_its_last_notification(port);
            } else {
                // NB: Destroying the receive right takes it out of the port set, and frees its name for reuse.
                io_thread.remove(port.name(), self);
                drop(port);
            }
        }
        drop(send);
        self.sent.notify_all();
        unsendable
    }

    // NB: Runs on the IO thread as IO is over, after the receive right has left the port set.
    fn finish_io(self: &Arc<Self>) -> VecDeque<PendingMessage> {
        assert!(self.io_thread_state() == IoThreadState::Stopped);
        let unsendable = self.stop_sending();
        lock(&self.incoming).local_io_is_over = true;
        self.incoming_changed.notify_all();
        // NB: Stopping for transfer tears down this end on purpose, and the rights are about to be adopted by the
        //     new owner, so that is not the peer going away.
        if !self.is_being_transferred.load(Ordering::Acquire) {
            self.mark_peer_eof();
        }
        unsendable
    }
}

enum Outgoing<'a> {
    New {
        payload: OutgoingPayload<'a>,
        ports: Vec<(MachPort, u32)>,
    },
    Waiting(PendingMessage),
}

enum SendResult {
    Sent,
    WouldBlock(PendingMessage),
    PeerGone,
}

// NB: Answers whether the notification used up the send-possible request, which it does once the peer's queue has
//     room, or as a dead-name notification once the peer's port has died. It is only for a message that came in on a
//     notification port, which only the kernel can send to, as the dead-name notification then holds a reference to
//     the dead name, to release.
fn consumes_send_possible_request(message: &mach::ReceivedMessage) -> bool {
    match message.id {
        mach::MACH_NOTIFY_SEND_POSSIBLE => true,
        mach::MACH_NOTIFY_DEAD_NAME => {
            if let Some(dead_name) = message.dead_name {
                drop(MachPort::adopt(dead_name, mach::PortRight::Send));
            }
            true
        }
        _ => false,
    }
}

fn release_unused_allocator_memory() {
    // NB: The messages the IO thread receives are freed by the threads that handle them, and the allocator keeps that
    //     memory until this thread allocates again or collects, which an idle process may never do. Unit tests
    //     don't link AK, and use the system allocator.
    #[cfg(ladybird_links_ak)]
    ak::release_unused_allocator_memory();
}

enum Registration {
    Receive(Arc<Shared>),
    SendPossibleNotification(Arc<Shared>),
    // NB: The notification port of a transport whose IO is over, while the kernel still owes it a notification for a
    //     request that it has used up. It goes once that has arrived.
    OrphanedNotification(
        #[expect(dead_code, reason = "Only held for the right, which dropping it gives back")] MachPort,
    ),
}

enum Route {
    Receive(Arc<Shared>),
    SendPossibleNotification(Arc<Shared>),
    OrphanedNotification,
    Unknown,
}

#[derive(Default)]
struct Attention {
    transports: Vec<Arc<Shared>>,
}

thread_local! {
    static IS_SHARED_IO_THREAD: Cell<bool> = const { Cell::new(false) };
}

/// The IO thread of every transport in the process. It waits on one port set, which holds the receive right of each
/// transport, and finds the transport a message is for by the port it came in on. Each message costs this thread a
/// copy and a wakeup of its owner, so the messages of other transports hold one up only briefly.
struct SharedIoThread {
    port_set: MachPort,
    wakeup_receive_right: MachPort,
    wakeup_send_right: MachPort,

    attention: Mutex<Attention>,
    io_finished: Condvar,

    // NB: Entries go in before their port joins the set, and out before their port is destroyed, as a name can be
    //     reused at once.
    registrations: Mutex<HashMap<PortName, Registration>>,

    // NB: The task holds one notification request per name, and transports that send to the same port share the
    //     name of their send right, so only one of them at a time may have a send-possible request on it.
    send_possible_request_owners: Mutex<HashMap<PortName, usize>>,
}

static IO_THREAD: OnceLock<Arc<SharedIoThread>> = OnceLock::new();

// NB: Every transport starts the thread as it is made, before anything reaches it through here.
fn shared_io_thread() -> &'static SharedIoThread {
    IO_THREAD
        .get()
        .expect("a transport starts the shared Mach IO thread before it uses it")
}

fn start_shared_io_thread() -> std::io::Result<&'static SharedIoThread> {
    static STARTING: Mutex<()> = Mutex::new(());
    if let Some(io_thread) = IO_THREAD.get() {
        return Ok(io_thread);
    }
    let _starting = lock(&STARTING);
    if let Some(io_thread) = IO_THREAD.get() {
        return Ok(io_thread);
    }
    let io_thread = Arc::new(SharedIoThread::new().map_err(KernelError)?);
    // NB: Nothing waits for this thread, which goes with the process. If it can't start, its ports go with the last
    //     reference, and the next transport tries again.
    std::thread::Builder::new().name("IPC IO (Mach)".to_string()).spawn({
        let io_thread = Arc::clone(&io_thread);
        move || {
            // NB: This thread only relays messages between the kernel and the threads that own the transports. If
            //     it ran below them, every message would wait on a lower-priority wakeup.
            mach::set_current_thread_qos_to_user_interactive();
            io_thread.run();
        }
    })?;
    Ok(IO_THREAD.get_or_init(|| io_thread))
}

// NB: Only the IO thread uses these.
struct IoThreadLocals {
    receive_buffer: MessageBuffer,
    transports_to_serve: Vec<Arc<Shared>>,
    // NB: Transports whose peer's queue is full, which the kernel could not be asked to say when it has room.
    transports_retrying_after_delay: Vec<Arc<Shared>>,
    // NB: What transports whose IO stopped on its own left unsent, released a bounded part per turn.
    unsendable: VecDeque<PendingMessage>,
    retry_deadline: Instant,
}

impl SharedIoThread {
    fn new() -> Result<Self, KernReturn> {
        let port_set = MachPort::allocate_port_set()?;
        let wakeup_receive_right = MachPort::allocate_receive_right()?;
        let wakeup_send_right = wakeup_receive_right.make_send_right()?;
        port_set.insert_member(&wakeup_receive_right)?;
        Ok(Self {
            port_set,
            wakeup_receive_right,
            wakeup_send_right,
            attention: Mutex::default(),
            io_finished: Condvar::new(),
            registrations: Mutex::default(),
            send_possible_request_owners: Mutex::default(),
        })
    }

    // NB: Messages that arrive on the port, one of the transport's own, are the transport's from here on.
    fn add(&self, registration: Registration, port: &MachPort) -> Result<(), KernReturn> {
        lock(&self.registrations).insert(port.name(), registration);
        if let Err(error) = self.port_set.insert_member(port) {
            lock(&self.registrations).remove(&port.name());
            return Err(error);
        }
        Ok(())
    }

    fn remove(&self, port: PortName, transport: &Arc<Shared>) {
        let mut registrations = lock(&self.registrations);
        let belongs_to_transport = match registrations.get(&port) {
            Some(Registration::Receive(owner) | Registration::SendPossibleNotification(owner)) => {
                Arc::ptr_eq(owner, transport)
            }
            _ => false,
        };
        if belongs_to_transport {
            registrations.remove(&port);
        }
    }

    fn route(&self, port: PortName) -> Route {
        match lock(&self.registrations).get(&port) {
            Some(Registration::Receive(transport)) => Route::Receive(Arc::clone(transport)),
            Some(Registration::SendPossibleNotification(transport)) => {
                Route::SendPossibleNotification(Arc::clone(transport))
            }
            Some(Registration::OrphanedNotification(_)) => Route::OrphanedNotification,
            None => Route::Unknown,
        }
    }

    // NB: Nor may one take the place of a request that something else has made there. Answers whether the transport
    //     may make its request.
    fn claim_send_possible_request(&self, transport: usize, destination: &MachPort) -> bool {
        let mut owners = lock(&self.send_possible_request_owners);
        if owners.contains_key(&destination.name()) {
            return false;
        }
        match destination.port_type() {
            Ok(port_type) if port_type & (mach::MACH_PORT_TYPE_DNREQUEST | mach::MACH_PORT_TYPE_SPREQUEST) == 0 => {}
            _ => return false,
        }
        owners.insert(destination.name(), transport);
        true
    }

    fn release_send_possible_request(&self, transport: usize, destination: PortName) {
        let mut owners = lock(&self.send_possible_request_owners);
        if owners.get(&destination) == Some(&transport) {
            owners.remove(&destination);
        }
    }

    // NB: Has this thread send the transport's backlog, take note of a peer it found gone, and stop its IO if asked
    //     to. A backlog is otherwise sent when the kernel says the peer's queue has room.
    fn request_attention(&self, transport: &Arc<Shared>) {
        {
            let mut attention = lock(&self.attention);
            if transport.io_is_finished.load(Ordering::Relaxed) {
                return;
            }
            if !transport.needs_attention.load(Ordering::Relaxed) {
                transport.needs_attention.store(true, Ordering::Relaxed);
                attention.transports.push(Arc::clone(transport));
            }
        }
        self.wake();
    }

    fn wait_until_io_is_finished(&self, transport: &Shared) {
        assert!(
            !IS_SHARED_IO_THREAD.get(),
            "the shared Mach IO thread must not wait for itself"
        );
        let mut attention = lock(&self.attention);
        while !transport.io_is_finished.load(Ordering::Relaxed) {
            attention = wait(&self.io_finished, attention);
        }
    }

    fn keep_until_its_last_notification(&self, notification_port: MachPort) {
        assert!(IS_SHARED_IO_THREAD.get());
        let name = notification_port.name();
        lock(&self.registrations).insert(name, Registration::OrphanedNotification(notification_port));
    }

    fn wake(&self) {
        // NB: A full wakeup queue already has this thread on its way.
        let _ = mach::send_header_only_message(&self.wakeup_send_right, mach::IPC_WAKEUP_MESSAGE_ID, Some(0));
    }

    fn run(&self) {
        IS_SHARED_IO_THREAD.set(true);
        let mut locals = IoThreadLocals {
            receive_buffer: MessageBuffer::new(mach::DEFAULT_RECEIVE_BUFFER_SIZE),
            transports_to_serve: Vec::new(),
            transports_retrying_after_delay: Vec::new(),
            unsendable: VecDeque::new(),
            retry_deadline: Instant::now(),
        };
        let mut released_memory_since_last_message = true;
        loop {
            self.serve_transports_needing_attention(&mut locals);
            let count = locals.unsendable.len().min(MAX_BACKLOG_MESSAGES_SENT_PER_TURN);
            locals.unsendable.drain(..count);

            // NB: A backlog that one turn did not get through goes on in the next, once this thread has taken in what
            //     came meanwhile.
            if self.has_transports_needing_attention() || !locals.unsendable.is_empty() {
                for _ in 0..MAX_MESSAGES_RECEIVED_BETWEEN_TURNS_OF_SENDING {
                    if !self.receive_and_handle_message(&mut locals, Some(0)) {
                        break;
                    }
                    released_memory_since_last_message = false;
                }
                continue;
            }

            // NB: Wait on the whole port set, for messages from every peer, wakeups, and kernel notifications like
            //     MACH_NOTIFY_NO_SENDERS, or MACH_NOTIFY_SEND_POSSIBLE once a peer's full queue has room.
            let retries_after_delay = !locals.transports_retrying_after_delay.is_empty();
            let timeout = if retries_after_delay {
                let remaining = locals.retry_deadline.saturating_duration_since(Instant::now());
                Some(u32::try_from(remaining.as_micros().div_ceil(1000)).unwrap_or(u32::MAX))
            } else if !released_memory_since_last_message {
                Some(IDLE_MEMORY_RELEASE_DELAY_IN_MILLISECONDS)
            } else {
                None
            };
            if self.receive_and_handle_message(&mut locals, timeout) {
                released_memory_since_last_message = false;
                continue;
            }
            if !retries_after_delay {
                release_unused_allocator_memory();
                released_memory_since_last_message = true;
            }
        }
    }

    fn has_transports_needing_attention(&self) -> bool {
        !lock(&self.attention).transports.is_empty()
    }

    fn serve_transports_needing_attention(&self, locals: &mut IoThreadLocals) {
        let mut transports_to_serve = std::mem::take(&mut locals.transports_to_serve);
        {
            let mut attention = lock(&self.attention);
            std::mem::swap(&mut transports_to_serve, &mut attention.transports);
            for transport in &transports_to_serve {
                transport.needs_attention.store(false, Ordering::Relaxed);
            }
        }
        if !locals.transports_retrying_after_delay.is_empty() && Instant::now() >= locals.retry_deadline {
            for transport in std::mem::take(&mut locals.transports_retrying_after_delay) {
                if !transports_to_serve.iter().any(|other| Arc::ptr_eq(other, &transport)) {
                    transports_to_serve.push(transport);
                }
            }
        }
        for transport in transports_to_serve.drain(..) {
            self.serve(&transport, locals);
        }
        locals.transports_to_serve = transports_to_serve;
    }

    fn serve(&self, transport: &Arc<Shared>, locals: &mut IoThreadLocals) {
        match transport.do_pending_io() {
            PendingIo::None => {}
            PendingIo::MoreToSendNextTurn => {
                let mut attention = lock(&self.attention);
                if !transport.needs_attention.load(Ordering::Relaxed) {
                    transport.needs_attention.store(true, Ordering::Relaxed);
                    attention.transports.push(Arc::clone(transport));
                }
            }
            PendingIo::RetryAfterDelay => {
                if locals.transports_retrying_after_delay.is_empty() {
                    locals.retry_deadline = Instant::now() + RETRY_DELAY;
                }
                if !locals
                    .transports_retrying_after_delay
                    .iter()
                    .any(|other| Arc::ptr_eq(other, transport))
                {
                    locals.transports_retrying_after_delay.push(Arc::clone(transport));
                }
            }
            PendingIo::IoIsOver => self.finish(transport, locals),
        }
    }

    // NB: Waits for one message no longer than the timeout, if there is one, and answers whether one came.
    fn receive_and_handle_message(&self, locals: &mut IoThreadLocals, timeout: Option<u32>) -> bool {
        let received = match timeout {
            Some(timeout) => mach::receive_message_within(&mut locals.receive_buffer, &self.port_set, timeout),
            None => mach::receive_message(&mut locals.receive_buffer, &self.port_set).map(Some),
        };
        let message = match received {
            Ok(Some(message)) => message,
            Ok(None) => return false,
            Err(ReceiveError::Kernel { code, local_port }) => {
                self.handle_failed_receive(code, local_port, locals);
                return true;
            }
            Err(ReceiveError::Malformed { what, local_port }) => {
                eprintln!("TransportMachPort: Rejecting malformed message ({what})");
                if let Some(transport) = self.transport_receiving_on(local_port) {
                    transport.mark_peer_eof();
                    transport.set_io_thread_state(IoThreadState::Stopped);
                    self.finish(&transport, locals);
                }
                return true;
            }
        };

        if message.local_port == self.wakeup_receive_right.name() {
            assert_eq!(message.id, mach::IPC_WAKEUP_MESSAGE_ID);
            return true;
        }
        match self.route(message.local_port) {
            Route::Receive(transport) => {
                if !transport.handle_message_on_io_thread(message) {
                    self.finish(&transport, locals);
                }
            }
            Route::SendPossibleNotification(transport) => {
                // NB: Only the kernel can send to the port on which the transport hears that its peer's queue has
                //     room.
                transport.handle_send_possible_notification(&message);
                self.serve(&transport, locals);
            }
            Route::OrphanedNotification => {
                if consumes_send_possible_request(&message) {
                    let port = lock(&self.registrations).remove(&message.local_port);
                    drop(port);
                }
            }
            Route::Unknown => {}
        }
        true
    }

    fn handle_failed_receive(&self, error: KernReturn, port: PortName, locals: &mut IoThreadLocals) {
        eprintln!(
            "TransportMachPort: mach_msg receive failed: {}",
            mach::error_string(error)
        );
        // NB: A message that could not be received whole is gone, and the transport it was for can no longer deliver
        //     in order what its peer sends, so its IO is over. That is if the kernel got as far as naming the port it
        //     came in on. Any other failure would be one of the port set itself.
        let error = error & !mach::MACH_MSG_MASK;
        assert!(
            error == mach::MACH_RCV_HEADER_ERROR || error == mach::MACH_RCV_BODY_ERROR,
            "the shared Mach IO thread's port set failed"
        );
        let Some(transport) = self.transport_receiving_on(port) else {
            return;
        };
        transport.set_io_thread_state(IoThreadState::Stopped);
        self.finish(&transport, locals);
    }

    fn transport_receiving_on(&self, port: PortName) -> Option<Arc<Shared>> {
        if !mach::is_valid(port) || port == self.wakeup_receive_right.name() {
            return None;
        }
        match self.route(port) {
            Route::Receive(transport) | Route::SendPossibleNotification(transport) => Some(transport),
            Route::OrphanedNotification | Route::Unknown => None,
        }
    }

    // NB: The owner may destroy the transport as soon as this returns.
    fn finish(&self, transport: &Arc<Shared>, locals: &mut IoThreadLocals) {
        // NB: Once the receive right has left the set, nothing more comes in for the transport.
        self.port_set
            .extract_member_named(transport.receive_port_name)
            .expect("a transport's receive right is in the port set until its IO is over");
        self.remove(transport.receive_port_name, transport);
        locals
            .transports_retrying_after_delay
            .retain(|other| !Arc::ptr_eq(other, transport));

        let unsendable = transport.finish_io();
        locals.unsendable.extend(unsendable);

        let mut attention = lock(&self.attention);
        if transport.needs_attention.load(Ordering::Relaxed) {
            attention.transports.retain(|other| !Arc::ptr_eq(other, transport));
            transport.needs_attention.store(false, Ordering::Relaxed);
        }
        transport.io_is_finished.store(true, Ordering::Relaxed);
        self.io_finished.notify_all();
    }
}

pub struct TransportMachPort {
    shared: Arc<Shared>,
    // NB: Owned here rather than by the IO thread, which only needs the name, so that it can go to a new owner.
    receive_right: Mutex<Option<MachPort>>,
}

impl TransportMachPort {
    /// A transport for this end, and the handle that makes the other end's transport.
    pub fn create_paired() -> std::io::Result<(Self, MachPortPair)> {
        let receive_a = MachPort::allocate_receive_right().map_err(KernelError)?;
        let send_a = receive_a.make_send_right().map_err(KernelError)?;
        let receive_b = MachPort::allocate_receive_right().map_err(KernelError)?;
        let send_b = receive_b.make_send_right().map_err(KernelError)?;
        // NB: The peer may receive messages before it has constructed its transport.
        receive_b.raise_queue_limit();
        Ok((
            Self::new(receive_a, send_b)?,
            MachPortPair {
                receive_right: receive_b,
                send_right: send_a,
            },
        ))
    }

    pub fn from_handle(handle: MachPortPair) -> std::io::Result<Self> {
        Self::new(handle.receive_right, handle.send_right)
    }

    pub fn new(receive_right: MachPort, send_right: MachPort) -> std::io::Result<Self> {
        Self::with_read_notification_pipe(receive_right, send_right, None)
    }

    /// A transport that writes a byte to the pipe when messages are there to read, or the peer is gone, and nothing
    /// more until `acknowledge_read_notification()`. It has the pipe from the start, so that what arrives before
    /// anything watches the pipe still leaves a byte in it.
    pub fn with_read_notification_pipe(
        receive_right: MachPort,
        send_right: MachPort,
        read_notification_pipe: Option<OwnedFd>,
    ) -> std::io::Result<Self> {
        if let Some(pipe) = &read_notification_pipe {
            mach::disable_sigpipe_on_writes(pipe)?;
        }
        receive_right.raise_queue_limit();
        // NB: The kernel sends MACH_NOTIFY_NO_SENDERS to the receive right once the peer loses its last send right
        //     to it. This is how the transport notices that the peer went away.
        receive_right.request_no_senders_notification().map_err(KernelError)?;

        let shared = Arc::new(Shared {
            receive_port_name: receive_right.name(),
            io_thread_state: AtomicU8::new(IoThreadState::Running as u8),
            is_open: AtomicBool::new(true),
            peer_eof: AtomicBool::new(false),
            is_being_transferred: AtomicBool::new(false),
            posting_thread_found_peer_gone: AtomicBool::new(false),
            peer_unreachable_marker_was_sent: AtomicBool::new(false),
            peer_is_unreachable: AtomicBool::new(false),
            stop_was_requested_by_owner: AtomicBool::new(false),
            needs_attention: AtomicBool::new(false),
            io_is_finished: AtomicBool::new(false),
            send: Mutex::new(SendState {
                send_right: Some(send_right),
                backlog: VecDeque::new(),
                sending_is_over: false,
                peer_is_unreachable: false,
                buffer: MessageBuffer::new(0),
                send_possible_notification_port: None,
                send_possible_notification_is_requested: false,
            }),
            sent: Condvar::new(),
            incoming: Mutex::new(Incoming {
                read_notification_pipe: read_notification_pipe.map(File::from),
                ..Incoming::default()
            }),
            incoming_changed: Condvar::new(),
            incoming_message_observer: Mutex::new(None),
        });
        start_shared_io_thread()?
            .add(Registration::Receive(Arc::clone(&shared)), &receive_right)
            .map_err(KernelError)?;
        Ok(Self {
            shared,
            receive_right: Mutex::new(Some(receive_right)),
        })
    }

    /// Sends a message, or queues it behind earlier ones that wait for room in the peer's queue. Refuses it once a stop
    /// is asked for or a send has failed, and refuses one with more attachments than a peer takes.
    pub fn post_message(&self, bytes: Vec<u8>, attachments: Vec<Attachment>) -> std::io::Result<()> {
        crate::validate_message_size(bytes.len(), attachments.len())?;
        self.post_message_with_any_number_of_attachments(OutgoingPayload::Owned(bytes), attachments)
    }

    /// As `post_message()`, but leaves it to the peer to refuse a message with too many attachments, as
    /// `IPC::TransportMachPort` does. Lent bytes are only handed over if the message has to wait behind others.
    pub fn post_message_with_any_number_of_attachments(
        &self,
        payload: OutgoingPayload<'_>,
        attachments: Vec<Attachment>,
    ) -> std::io::Result<()> {
        let ports: Vec<_> = attachments.into_iter().map(Attachment::into_mach_port).collect();
        let mut should_wake_io_thread = false;
        {
            let mut send = lock(&self.shared.send);
            // NB: Once a stop is asked for, nothing new comes in, so that a producer can't keep a graceful close or a
            //     transfer from finishing.
            if send.sending_is_over
                || send.peer_is_unreachable
                || self.shared.io_thread_state() != IoThreadState::Running
            {
                return Err(std::io::ErrorKind::NotConnected.into());
            }
            if send.backlog.is_empty() {
                match self.shared.send(&mut send, Outgoing::New { payload, ports }) {
                    SendResult::Sent => {}
                    SendResult::WouldBlock(message) => {
                        send.backlog.push_back(message);
                        should_wake_io_thread = true;
                    }
                    SendResult::PeerGone => {
                        self.shared
                            .posting_thread_found_peer_gone
                            .store(true, Ordering::Release);
                        should_wake_io_thread = true;
                    }
                }
            } else {
                send.backlog
                    .push_back(PendingMessage::unsent(payload.into_owned(), ports));
            }
        }
        if should_wake_io_thread {
            shared_io_thread().request_attention(&self.shared);
        }
        Ok(())
    }

    /// How many posted messages wait for room in the peer's queue, so a producer can tell when its peer falls behind.
    pub fn pending_message_count(&self) -> usize {
        lock(&self.shared.send).backlog.len()
    }

    /// Waits until everything posted so far has been handed to the kernel.
    pub fn flush(&self) {
        if !self.shared.is_open.load(Ordering::Relaxed) {
            return;
        }
        let mut send = lock(&self.shared.send);
        while !send.sending_is_over && !send.peer_is_unreachable && !send.backlog.is_empty() {
            let (guard, timed_out) = wait_for(&self.shared.sent, send, FLUSH_WAKEUP_REARM_INTERVAL);
            send = guard;
            // NB: The wakeup is best-effort, so re-arm it rather than wait forever if one is ever dropped.
            if timed_out {
                shared_io_thread().request_attention(&self.shared);
            }
        }
    }

    /// False once this end is closed or asked to stop, a send has failed, or the peer is gone: whenever a post would
    /// be refused, so that an owner which checks first doesn't post into a refusal.
    pub fn is_open(&self) -> bool {
        self.shared.is_open.load(Ordering::Acquire)
            && self.shared.io_thread_state() == IoThreadState::Running
            && !self.shared.peer_is_unreachable.load(Ordering::Acquire)
            && !self.shared.peer_eof.load(Ordering::Acquire)
    }

    /// Waits for the next message, or returns `None` once the peer is gone and every message it sent was taken.
    pub fn receive(&self) -> Option<ReceivedMessage> {
        let mut incoming = lock(&self.shared.incoming);
        loop {
            if let Some(message) = incoming.messages.pop_front() {
                return Some(message);
            }
            if self.shared.peer_eof.load(Ordering::Acquire) || incoming.local_io_is_over {
                return None;
            }
            incoming = wait(&self.shared.incoming_changed, incoming);
        }
    }

    /// Takes every message that has arrived, and whether the peer was gone as they were taken. Later arrivals can't
    /// be skipped by reporting the peer gone.
    pub fn take_incoming(&self) -> (Vec<ReceivedMessage>, bool) {
        let mut incoming = lock(&self.shared.incoming);
        let messages = std::mem::take(&mut incoming.messages).into();
        (messages, self.shared.peer_eof.load(Ordering::Acquire))
    }

    /// Waits until there is a message to take, the peer is gone, the wait is interrupted, or the deadline passes.
    pub fn wait_until_readable(&self, deadline: Option<MonotonicDeadline>) {
        let mut incoming = lock(&self.shared.incoming);
        while incoming.messages.is_empty()
            && !self.shared.peer_eof.load(Ordering::Acquire)
            && !incoming.local_io_is_over
            && !incoming.wait_until_readable_interrupted
        {
            match deadline {
                None => incoming = wait(&self.shared.incoming_changed, incoming),
                Some(deadline) => {
                    let Some(remaining) = deadline.remaining() else {
                        break;
                    };
                    incoming = wait_for(&self.shared.incoming_changed, incoming, remaining).0;
                }
            }
        }
        incoming.wait_until_readable_interrupted = false;
    }

    /// Makes `wait_until_readable()` return, even with nothing to read, now or if none is under way, the next time.
    /// Any thread may call this.
    pub fn interrupt_wait_until_readable(&self) {
        lock(&self.shared.incoming).wait_until_readable_interrupted = true;
        self.shared.incoming_changed.notify_all();
    }

    /// Waits until everything the peer has already sent has been taken into the incoming queue.
    pub fn wait_until_incoming_is_current(&self) {
        let receive_right = lock(&self.receive_right);
        let Some(receive_right) = receive_right.as_ref() else {
            return;
        };
        let mut incoming = lock(&self.shared.incoming);
        if self.shared.peer_eof.load(Ordering::Acquire) || self.shared.io_thread_state() != IoThreadState::Running {
            return;
        }
        // NB: A marker on the receive right follows every message sent to it before, including one the IO thread
        //     has taken off the port but not queued yet. A send-once right keeps the peer's departure visible, and
        //     bypasses the queue limit, so sending under the incoming lock can't wait on a full queue.
        let barrier_right = receive_right
            .make_send_once_right()
            .expect("a send-once right for the receive barrier");
        let result =
            mach::send_header_only_message_through_send_once_right(barrier_right, mach::IPC_RECEIVE_BARRIER_MESSAGE_ID);
        assert_eq!(result, mach::MACH_MSG_SUCCESS, "the receive barrier is sent");
        incoming.receive_barriers_sent += 1;
        let barrier = incoming.receive_barriers_sent;
        while !self.shared.peer_eof.load(Ordering::Acquire)
            && self.shared.io_thread_state() == IoThreadState::Running
            && incoming.receive_barriers_received < barrier
        {
            incoming = wait(&self.shared.incoming_changed, incoming);
        }
    }

    /// Sets what the IO thread shows every message that arrives, before the owner gets to see it, and empty bytes once
    /// the peer is gone. It runs while the IO thread holds a lock, so it should only have a look. Once this returns,
    /// the observer it gives back is no longer running.
    pub fn set_incoming_message_observer(
        &self,
        observer: Option<IncomingMessageObserver>,
    ) -> Option<IncomingMessageObserver> {
        std::mem::replace(&mut *lock(&self.shared.incoming_message_observer), observer)
    }

    /// Lets the transport write to its read-notification pipe again, once the owner has drained it.
    pub fn acknowledge_read_notification(&self) {
        lock(&self.shared.incoming).read_notification_pending = false;
    }

    /// Writes to the read-notification pipe if there is something to read, or the peer is gone, and no byte is
    /// already pending.
    pub fn schedule_read_notification_if_readable(&self) {
        let mut incoming = lock(&self.shared.incoming);
        if !incoming.messages.is_empty() || self.shared.peer_eof.load(Ordering::Acquire) {
            Shared::schedule_read_notification(&mut incoming);
        }
    }

    /// Sends what is queued, then stops, which tells the peer that this end is gone.
    pub fn close_after_sending_all_pending_messages(&self) {
        self.stop(IoThreadState::SendPendingMessagesAndStop);
        self.shared.is_open.store(false, Ordering::Release);
        self.release_send_right_to_peer();
        self.release_receive_right();
    }

    pub fn close(&self) {
        self.shared.is_open.store(false, Ordering::Release);
        self.stop(IoThreadState::Stopped);
        self.release_send_right_to_peer();
        self.release_receive_right();
    }

    /// Sends what is queued, stops, and hands both rights over for another transport to take this end's place.
    pub fn release_for_transfer(&self) -> std::io::Result<MachPortPair> {
        self.shared.is_being_transferred.store(true, Ordering::Release);
        self.stop(IoThreadState::SendPendingMessagesAndStop);
        self.shared.is_open.store(false, Ordering::Release);

        let receive_right = lock(&self.receive_right)
            .take()
            .ok_or(std::io::ErrorKind::NotConnected)?;
        // NB: Otherwise this end would still hear of the peer going away, on a port it no longer owns.
        receive_right.cancel_no_senders_notification().map_err(KernelError)?;
        let send_right = lock(&self.shared.send)
            .send_right
            .take()
            .ok_or(std::io::ErrorKind::NotConnected)?;
        Ok(MachPortPair {
            receive_right,
            send_right,
        })
    }

    // NB: As closing a socket does, closing the transport tells the peer: its receive right gets MACH_NOTIFY_NO_SENDERS
    //     once this send right is gone. Sending is over by now, as IO has stopped.
    fn release_send_right_to_peer(&self) {
        let send_right = lock(&self.shared.send).send_right.take();
        drop(send_right);
    }

    // NB: Nothing takes messages off the port once IO has stopped. Destroying it makes the peer's sends fail, rather
    //     than leave the peer waiting for room in a full queue that nobody drains. What was already taken off the
    //     port can still be read.
    fn release_receive_right(&self) {
        let receive_right = lock(&self.receive_right).take();
        drop(receive_right);
    }

    fn stop(&self, desired_state: IoThreadState) {
        self.shared.stop_was_requested_by_owner.store(true, Ordering::Release);
        // NB: The IO thread may already have stopped on its own, as when the peer hung up. Stopped is final, so it is
        //     never replaced by a new stop request.
        let mut state = self.shared.io_thread_state.load(Ordering::Acquire);
        while state != IoThreadState::Stopped as u8 {
            match self.shared.io_thread_state.compare_exchange(
                state,
                desired_state as u8,
                Ordering::AcqRel,
                Ordering::Acquire,
            ) {
                Ok(_) => break,
                Err(current) => state = current,
            }
        }
        let io_thread = shared_io_thread();
        io_thread.request_attention(&self.shared);
        io_thread.wait_until_io_is_finished(&self.shared);

        // NB: What was left unsent goes here rather than on the IO thread, which all transports share, as releasing
        //     the rights of a long backlog takes a while.
        let backlog = std::mem::take(&mut lock(&self.shared.send).backlog);
        drop(backlog);
    }
}

impl Drop for TransportMachPort {
    fn drop(&mut self) {
        self.stop(IoThreadState::Stopped);
        self.release_send_right_to_peer();
        // NB: The observer goes here, on the owner's thread, rather than wherever the last reference to the shared state
        //     happens to be dropped.
        drop(self.set_incoming_message_observer(None));
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::os::fd::OwnedFd;

    fn paired() -> (TransportMachPort, TransportMachPort) {
        let (a, handle) = TransportMachPort::create_paired().unwrap();
        (a, TransportMachPort::from_handle(handle).unwrap())
    }

    #[test]
    fn messages_arrive_in_order() {
        let (a, b) = paired();
        for i in 0..100u32 {
            a.post_message(i.to_ne_bytes().to_vec(), Vec::new()).unwrap();
        }
        for i in 0..100u32 {
            assert_eq!(b.receive().unwrap().bytes(), i.to_ne_bytes());
        }
    }

    #[test]
    fn peers_flooding_each_other_keep_receiving() {
        let (a, b) = paired();
        let count = 10 * mach::MACH_PORT_QLIMIT_MAX as u32;
        for i in 0..count {
            a.post_message(i.to_ne_bytes().to_vec(), Vec::new()).unwrap();
            b.post_message(i.to_ne_bytes().to_vec(), Vec::new()).unwrap();
        }
        for i in 0..count {
            assert_eq!(a.receive().unwrap().bytes(), i.to_ne_bytes());
            assert_eq!(b.receive().unwrap().bytes(), i.to_ne_bytes());
        }
    }

    #[test]
    fn large_messages_go_out_of_line() {
        let (a, b) = paired();
        let bytes: Vec<u8> = (0..100_000u32).map(|i| i as u8).collect();
        a.post_message(bytes.clone(), Vec::new()).unwrap();
        assert_eq!(b.receive().unwrap().bytes(), bytes);
    }

    #[test]
    fn files_go_with_messages() {
        let (a, b) = paired();
        let (reader, writer) = std::io::pipe().unwrap();
        a.post_message(vec![1], vec![Attachment::from_fd(OwnedFd::from(writer)).unwrap()])
            .unwrap();
        let message = b.receive().unwrap();
        assert_eq!(message.bytes(), [1]);
        let mut attachments = message.attachments;
        let mut writer = std::fs::File::from(attachments.remove(0).into_fd().unwrap());
        std::io::Write::write_all(&mut writer, b"ok").unwrap();
        drop(writer);
        let mut buffer = String::new();
        std::io::Read::read_to_string(&mut std::fs::File::from(OwnedFd::from(reader)), &mut buffer).unwrap();
        assert_eq!(buffer, "ok");
    }

    #[test]
    fn a_failed_send_refuses_later_posts_and_keeps_what_arrived() {
        let receive_right = MachPort::allocate_receive_right().unwrap();
        let to_this_end = receive_right.make_send_right().unwrap();
        let peer_receive_right = MachPort::allocate_receive_right().unwrap();
        let transport = TransportMachPort::new(receive_right, peer_receive_right.make_send_right().unwrap()).unwrap();
        assert!(matches!(
            mach::send_data_message(&mut MessageBuffer::new(0), &to_this_end, &[7], Vec::new()),
            SendOutcome::Sent
        ));
        drop(peer_receive_right);

        transport.post_message(vec![1], Vec::new()).unwrap();
        assert_eq!(
            transport.post_message(vec![2], Vec::new()).unwrap_err().kind(),
            std::io::ErrorKind::NotConnected
        );
        assert_eq!(transport.receive().unwrap().bytes(), [7]);
        assert!(transport.receive().is_none());
        assert!(!transport.is_open());
    }

    #[test]
    fn a_backlog_that_can_no_longer_be_sent_lets_go_of_its_attachments() {
        let peer_receive_right = MachPort::allocate_receive_right().unwrap();
        let transport = TransportMachPort::new(
            MachPort::allocate_receive_right().unwrap(),
            peer_receive_right.make_send_right().unwrap(),
        )
        .unwrap();
        let (mut reader, writer) = std::io::pipe().unwrap();
        // NB: The peer never reads, so its queue fills, and later messages wait in the backlog with their files.
        while transport.pending_message_count() < 3 {
            let attachment = Attachment::from_fd(OwnedFd::from(writer.try_clone().unwrap())).unwrap();
            transport.post_message(vec![1], vec![attachment]).unwrap();
        }
        drop(writer);
        drop(peer_receive_right);

        // NB: Every copy of the writer is gone once the backlog lets go of its files, without the owner doing anything.
        let (done, finished) = std::sync::mpsc::channel();
        std::thread::spawn(move || done.send(std::io::Read::read(&mut reader, &mut [0]).unwrap()));
        assert_eq!(finished.recv_timeout(Duration::from_secs(10)), Ok(0));
        assert_eq!(transport.pending_message_count(), 0);
    }

    #[test]
    fn nothing_is_taken_from_a_peer_after_it_is_found_unreachable() {
        let receive_right = MachPort::allocate_receive_right().unwrap();
        let to_this_end = receive_right.make_send_right().unwrap();
        let peer_receive_right = MachPort::allocate_receive_right().unwrap();
        let transport = TransportMachPort::new(receive_right, peer_receive_right.make_send_right().unwrap()).unwrap();
        // NB: The peer lets go of its receive right, but keeps sending to this end.
        assert!(matches!(
            mach::send_data_message(&mut MessageBuffer::new(0), &to_this_end, &[7], Vec::new()),
            SendOutcome::Sent
        ));
        drop(peer_receive_right);
        transport.post_message(vec![1], Vec::new()).unwrap();
        assert_eq!(transport.receive().unwrap().bytes(), [7]);
        assert!(transport.receive().is_none());

        assert!(matches!(
            mach::send_data_message(&mut MessageBuffer::new(0), &to_this_end, &[8], Vec::new()),
            SendOutcome::Sent
        ));
        std::thread::sleep(Duration::from_millis(50));
        assert!(transport.receive().is_none());
    }

    #[test]
    fn a_backlog_left_by_io_that_stopped_on_its_own_lets_go_of_its_attachments() {
        let receive_right = MachPort::allocate_receive_right().unwrap();
        let to_this_end = receive_right.make_send_right().unwrap();
        let peer_receive_right = MachPort::allocate_receive_right().unwrap();
        let transport = TransportMachPort::new(receive_right, peer_receive_right.make_send_right().unwrap()).unwrap();
        let (mut reader, writer) = std::io::pipe().unwrap();
        // NB: The peer never reads, so its queue fills, and later messages wait in the backlog with their files.
        while transport.pending_message_count() < 3 {
            let attachment = Attachment::from_fd(OwnedFd::from(writer.try_clone().unwrap())).unwrap();
            transport.post_message(vec![1], vec![attachment]).unwrap();
        }
        drop(writer);
        // NB: A message the transport refuses stops its IO, and the peer's queue keeps the messages that reached it.
        assert_eq!(
            mach::send_header_only_message(&to_this_end, 0x1234, Some(0)),
            mach::MACH_MSG_SUCCESS
        );
        let (done, finished) = std::sync::mpsc::channel();
        let reader_thread = std::thread::spawn(move || done.send(std::io::Read::read(&mut reader, &mut [0]).unwrap()));
        drop(peer_receive_right);
        assert_eq!(finished.recv_timeout(Duration::from_secs(10)), Ok(0));
        reader_thread.join().unwrap().unwrap();
        assert!(!transport.is_open());
    }

    #[test]
    fn a_reader_of_an_endpoint_that_is_handed_over_stops_waiting() {
        let (_a, b) = paired();
        std::thread::scope(|scope| {
            let reader = scope.spawn(|| b.receive());
            // NB: The reader may or may not be waiting yet; either way it must not wait for good.
            std::thread::sleep(Duration::from_millis(10));
            let handle = b.release_for_transfer().unwrap();
            assert!(reader.join().unwrap().is_none());
            drop(handle);
        });
    }

    #[test]
    fn the_peer_closing_ends_the_messages() {
        let (a, b) = paired();
        a.post_message(vec![7], Vec::new()).unwrap();
        a.close_after_sending_all_pending_messages();
        assert_eq!(b.receive().unwrap().bytes(), [7]);
        assert!(b.receive().is_none());
        assert!(!b.is_open());
    }
}
