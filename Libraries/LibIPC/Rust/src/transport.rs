/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The Mach port transport, which speaks with `IPC::TransportMachPort` on the other end.
//!
//! An IO thread waits on a port set holding the transport's receive right and a private wakeup port. It sends what the
//! owner posts, and queues what arrives for the owner to take. The peer going away shows as `MACH_NOTIFY_NO_SENDERS` on
//! the receive right, once the peer's last send right to it is gone.

use std::sync::atomic::{AtomicU8, Ordering};
use std::sync::{Arc, Condvar, Mutex, MutexGuard};
use std::thread::JoinHandle;

use crate::MAX_MESSAGE_ATTACHMENT_COUNT;
use crate::attachment::Attachment;
use crate::mach::{self, KernelError, MachPort, MessageBuffer, ReceiveError};
use crate::receive_queue::IncomingQueue;
pub use crate::receive_queue::ReceivedMessage;

/// The receive right and send right that make up one end of a connection, as `IPC::TransportHandle` holds them.
pub struct MachPortPair {
    pub receive_right: MachPort,
    pub send_right: MachPort,
}

#[derive(Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
enum IoThreadState {
    Running = 0,
    SendPendingMessagesAndStop = 1,
    Stopped = 2,
}

struct PendingMessage {
    bytes: Vec<u8>,
    attachments: Vec<Attachment>,
}

#[derive(Default)]
struct SendQueue {
    pending: Vec<PendingMessage>,
    in_progress: bool,
    waiters_released: bool,
}

struct Shared {
    state: AtomicU8,
    send_queue: Mutex<SendQueue>,
    sent: Condvar,
    incoming: IncomingQueue,
    wakeup_send_right: MachPort,
}

impl Shared {
    fn state(&self) -> IoThreadState {
        match self.state.load(Ordering::Acquire) {
            0 => IoThreadState::Running,
            1 => IoThreadState::SendPendingMessagesAndStop,
            _ => IoThreadState::Stopped,
        }
    }

    fn set_state(&self, state: IoThreadState) {
        self.state.store(state as u8, Ordering::Release);
    }

    fn should_retry_send(&self) -> bool {
        self.state() != IoThreadState::Stopped
    }

    fn wake_io_thread(&self) {
        // NB: The wakeup is best-effort; a full wakeup queue already wakes the thread.
        let _ = mach::send_header_only_message(&self.wakeup_send_right, mach::IPC_WAKEUP_MESSAGE_ID, Some(0));
    }

    fn lock_send_queue(&self) -> MutexGuard<'_, SendQueue> {
        self.send_queue.lock().unwrap_or_else(|error| error.into_inner())
    }
}

pub struct TransportMachPort {
    shared: Arc<Shared>,
    io_thread: Option<JoinHandle<()>>,
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
        let io = IoThread::new(receive_right, send_right).map_err(KernelError)?;
        let shared = Arc::clone(&io.shared);
        let io_thread = std::thread::Builder::new()
            .name("IPC IO (Mach)".to_string())
            .spawn(move || {
                // NB: The IO thread only relays messages between the kernel and the owning thread. If it ran below
                //     that thread, every message would wait on a lower-priority wakeup.
                mach::set_current_thread_qos_to_user_interactive();
                io.run();
            })?;

        Ok(Self {
            shared,
            io_thread: Some(io_thread),
        })
    }

    /// Queues a message for the IO thread to send, or refuses it once the transport has stopped sending.
    pub fn post_message(&self, bytes: Vec<u8>, attachments: Vec<Attachment>) -> std::io::Result<()> {
        crate::validate_message_size(bytes.len(), attachments.len())?;
        let mut queue = self.shared.lock_send_queue();
        if self.shared.state() != IoThreadState::Running {
            return Err(std::io::ErrorKind::NotConnected.into());
        }
        let was_empty = queue.pending.is_empty();
        queue.pending.push(PendingMessage { bytes, attachments });
        drop(queue);
        if was_empty {
            self.shared.wake_io_thread();
        }
        Ok(())
    }

    /// Waits until every message posted so far is sent, or the transport stops.
    pub fn flush(&self) {
        self.shared.wake_io_thread();
        let mut queue = self.shared.lock_send_queue();
        while !queue.waiters_released && (!queue.pending.is_empty() || queue.in_progress) {
            queue = self.shared.sent.wait(queue).unwrap_or_else(|error| error.into_inner());
        }
    }

    pub fn is_open(&self) -> bool {
        self.shared.state() == IoThreadState::Running && !self.shared.incoming.lock().peer_eof
    }

    /// Waits for the next message, or returns `None` once the peer is gone and every message it sent was taken.
    pub fn receive(&self) -> Option<ReceivedMessage> {
        self.shared.incoming.receive()
    }

    /// Sends what is queued, then stops, which tells the peer that this end is gone.
    pub fn close_after_sending_all_pending_messages(&mut self) {
        self.stop(IoThreadState::SendPendingMessagesAndStop);
    }

    pub fn close(&mut self) {
        self.stop(IoThreadState::Stopped);
    }

    /// Stops sending and receiving, and tells the peer that this end is gone, from any thread that shares the transport.
    /// Messages not sent yet are dropped, and a `receive()` returns `None` once it has taken what had arrived. Dropping
    /// the transport still joins its IO thread.
    pub fn shutdown(&self) {
        self.shared.set_state(IoThreadState::Stopped);
        self.shared.wake_io_thread();
    }

    fn stop(&mut self, state: IoThreadState) {
        let Some(io_thread) = self.io_thread.take() else {
            return;
        };
        self.shared.set_state(state);
        self.shared.wake_io_thread();
        let _ = io_thread.join();
    }
}

impl Drop for TransportMachPort {
    fn drop(&mut self) {
        self.close();
    }
}

struct IoThread {
    shared: Arc<Shared>,
    receive_right: MachPort,
    send_right: Option<MachPort>,
    port_set: MachPort,
    wakeup_receive_right: MachPort,
}

impl IoThread {
    fn new(receive_right: MachPort, send_right: MachPort) -> Result<Self, mach::KernReturn> {
        let port_set = MachPort::allocate_port_set()?;
        port_set.insert_member(&receive_right)?;
        let wakeup_receive_right = MachPort::allocate_receive_right()?;
        let wakeup_send_right = wakeup_receive_right.make_send_right()?;
        port_set.insert_member(&wakeup_receive_right)?;
        receive_right.raise_queue_limit();
        receive_right.request_no_senders_notification()?;

        let shared = Arc::new(Shared {
            state: AtomicU8::new(IoThreadState::Running as u8),
            send_queue: Mutex::default(),
            sent: Condvar::new(),
            incoming: IncomingQueue::default(),
            wakeup_send_right,
        });

        Ok(Self {
            shared,
            receive_right,
            send_right: Some(send_right),
            port_set,
            wakeup_receive_right,
        })
    }

    fn run(mut self) {
        let mut receive_buffer = MessageBuffer::new(mach::DEFAULT_RECEIVE_BUFFER_SIZE);
        let mut send_buffer = MessageBuffer::new(4096);
        loop {
            if self.shared.state() == IoThreadState::Stopped {
                break;
            }

            let messages_to_send = {
                let mut queue = self.shared.lock_send_queue();
                let messages = std::mem::take(&mut queue.pending);
                if !messages.is_empty() {
                    queue.in_progress = true;
                }
                messages
            };
            let sent_any = !messages_to_send.is_empty();
            for message in messages_to_send {
                if self.shared.state() == IoThreadState::Stopped {
                    break;
                }
                self.send(&mut send_buffer, &mut receive_buffer, message);
            }
            if sent_any {
                self.shared.lock_send_queue().in_progress = false;
                self.shared.sent.notify_all();
            }

            if self.shared.state() == IoThreadState::Stopped {
                break;
            }
            // NB: Waiting for a send to go through takes in what arrived meanwhile, which may include the wakeups for
            //     messages posted since this thread took the queue.
            if !self.shared.lock_send_queue().pending.is_empty() {
                continue;
            }
            if self.shared.state() == IoThreadState::SendPendingMessagesAndStop {
                self.shared.set_state(IoThreadState::Stopped);
                break;
            }

            let received = mach::receive_message(&mut receive_buffer, &self.port_set);
            if !self.handle_receive_result(received) {
                break;
            }
        }

        self.shared.set_state(IoThreadState::Stopped);
        {
            let mut queue = self.shared.lock_send_queue();
            queue.waiters_released = true;
            queue.pending.clear();
        }
        self.shared.sent.notify_all();
        // NB: As closing a socket does, dropping the send right tells the peer that this end is gone.
        self.send_right = None;
        self.shared.incoming.mark_peer_eof();
        let _ = self.port_set.extract_member(&self.receive_right);
    }

    fn send(&mut self, send_buffer: &mut MessageBuffer, receive_buffer: &mut MessageBuffer, message: PendingMessage) {
        let Some(send_right) = &self.send_right else {
            return;
        };
        let ports = message
            .attachments
            .into_iter()
            .map(Attachment::into_mach_port)
            .collect();
        let result = mach::send_data_message(send_buffer, send_right, &message.bytes, ports, || {
            // NB: The peer may be waiting to send to this side while this side waits for room in its queue, so take
            //     in what it sent before retrying.
            if !self.receive_queued_messages(receive_buffer) {
                self.shared.set_state(IoThreadState::Stopped);
            }
            self.shared.should_retry_send()
        });
        if result != mach::MACH_MSG_SUCCESS {
            eprintln!("TransportMachPort: send failed: {}", mach::error_string(result));
            self.send_right = None;
            self.shared.set_state(IoThreadState::Stopped);
        }
    }

    // NB: Takes in every message that is already queued, and returns whether to go on.
    fn receive_queued_messages(&self, buffer: &mut MessageBuffer) -> bool {
        while let Some(received) = mach::try_receive_message(buffer, &self.port_set).transpose() {
            if !self.handle_receive_result(received) {
                return false;
            }
        }
        true
    }

    // NB: Handles the outcome of receiving from the port set, and returns whether to go on.
    fn handle_receive_result(&self, received: Result<mach::ReceivedMessage, ReceiveError>) -> bool {
        match received {
            Ok(message) if message.local_port == self.wakeup_receive_right.name() => true,
            Ok(message) => self.handle_received(message),
            Err(ReceiveError::Kernel(error)) => {
                eprintln!(
                    "TransportMachPort: mach_msg receive failed: {}",
                    mach::error_string(error)
                );
                false
            }
            Err(ReceiveError::Malformed(what)) => {
                eprintln!("TransportMachPort: Refusing a message with a malformed {what}");
                false
            }
        }
    }

    // NB: Handles a message that arrived on the receive right, and returns whether to go on.
    fn handle_received(&self, message: mach::ReceivedMessage) -> bool {
        match message.id {
            mach::MACH_NOTIFY_NO_SENDERS => {
                self.shared.incoming.mark_peer_eof();
                true
            }
            mach::MACH_NOTIFY_SEND_ONCE | mach::IPC_RECEIVE_BARRIER_MESSAGE_ID => true,
            mach::IPC_DATA_MESSAGE_ID | mach::IPC_INLINE_DATA_MESSAGE_ID => {
                let Some((bytes, ports)) = message.data else {
                    return false;
                };
                if ports.len() > MAX_MESSAGE_ATTACHMENT_COUNT {
                    eprintln!("TransportMachPort: Refusing a message with {} attachments", ports.len());
                    return false;
                }
                if bytes.bytes().is_empty() && ports.is_empty() {
                    return true;
                }
                let attachments = ports
                    .into_iter()
                    .map(|(port, disposition)| Attachment::from_mach_port(port, disposition))
                    .collect();
                let message = ReceivedMessage { bytes, attachments };
                self.shared.incoming.push([message]);
                true
            }
            id => {
                eprintln!("TransportMachPort: Refusing a message with ID {id:#x}");
                false
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::mach::{PayloadBytes, PortRight};
    use std::os::fd::OwnedFd;

    fn paired() -> (TransportMachPort, TransportMachPort) {
        let (a, handle) = TransportMachPort::create_paired().unwrap();
        (a, TransportMachPort::from_handle(handle).unwrap())
    }

    #[test]
    fn graceful_close_retries_a_full_queue() {
        let receive = MachPort::allocate_receive_right().unwrap();
        let send = receive.make_send_right().unwrap();
        let ours = MachPort::allocate_receive_right().unwrap();
        let io = IoThread::new(ours, send).unwrap();
        let destination = io.send_right.as_ref().unwrap();
        loop {
            match mach::send_header_only_message(destination, mach::IPC_WAKEUP_MESSAGE_ID, Some(0)) {
                mach::MACH_MSG_SUCCESS => (),
                mach::MACH_SEND_TIMED_OUT => break,
                error => panic!("queue fill failed: {}", mach::error_string(error)),
            }
        }
        io.shared.set_state(IoThreadState::SendPendingMessagesAndStop);
        let mut receive_buffer = MessageBuffer::new(mach::DEFAULT_RECEIVE_BUFFER_SIZE);
        let mut retried = false;
        assert_eq!(
            mach::send_data_message(&mut MessageBuffer::new(4096), destination, &[7], vec![], || {
                retried = true;
                let keep_sending = io.shared.should_retry_send();
                mach::receive_message(&mut receive_buffer, &receive).unwrap();
                keep_sending
            }),
            mach::MACH_MSG_SUCCESS
        );
        assert!(retried);
        loop {
            let message = mach::receive_message(&mut receive_buffer, &receive).unwrap();
            if let Some((payload, _)) = message.data {
                assert_eq!(payload.bytes(), [7]);
                break;
            }
        }
        io.shared.set_state(IoThreadState::Stopped);
        assert!(!io.shared.should_retry_send());
    }

    #[test]
    fn no_senders_keeps_pending_replies() {
        let (io, peer) = {
            let ours = MachPort::allocate_receive_right().unwrap();
            let peer = MachPort::allocate_receive_right().unwrap();
            (IoThread::new(ours, peer.make_send_right().unwrap()).unwrap(), peer)
        };
        io.shared.lock_send_queue().pending.push(PendingMessage {
            bytes: vec![7],
            attachments: vec![],
        });
        assert!(io.handle_received(mach::ReceivedMessage {
            id: mach::MACH_NOTIFY_NO_SENDERS,
            local_port: io.receive_right.name(),
            data: None,
        }));
        assert!(io.shared.incoming.lock().peer_eof);
        assert!(io.shared.state() == IoThreadState::Running);
        io.shared.set_state(IoThreadState::SendPendingMessagesAndStop);
        io.run();
        let message = mach::receive_message(&mut MessageBuffer::new(mach::DEFAULT_RECEIVE_BUFFER_SIZE), &peer).unwrap();
        assert_eq!(message.data.unwrap().0.bytes(), [7]);
    }

    #[test]
    fn send_failure_leaves_eof_to_the_io_thread() {
        let receive = MachPort::allocate_receive_right().unwrap();
        let invalid_send = MachPort::adopt(mach::MACH_PORT_NULL, PortRight::Send);
        let mut io = IoThread::new(receive, invalid_send).unwrap();
        let transport = TransportMachPort {
            shared: Arc::clone(&io.shared),
            io_thread: None,
        };
        io.send(
            &mut MessageBuffer::new(4096),
            &mut MessageBuffer::new(mach::DEFAULT_RECEIVE_BUFFER_SIZE),
            PendingMessage {
                bytes: vec![1],
                attachments: Vec::new(),
            },
        );
        assert!(io.shared.state() == IoThreadState::Stopped);
        assert!(!io.shared.incoming.lock().peer_eof);
        assert!(io.handle_received(mach::ReceivedMessage {
            id: mach::IPC_INLINE_DATA_MESSAGE_ID,
            local_port: io.receive_right.name(),
            data: Some((PayloadBytes::Inline(vec![7]), Vec::new())),
        }));
        io.run();
        assert_eq!(transport.receive().unwrap().bytes(), [7]);
        assert!(transport.receive().is_none());
        assert_eq!(
            transport.post_message(vec![2], Vec::new()).unwrap_err().kind(),
            std::io::ErrorKind::NotConnected
        );
        assert!(transport.shared.lock_send_queue().pending.is_empty());
        assert!(transport.receive().is_none());
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
    fn a_burst_of_posts_sends_one_wakeup() {
        let receive = MachPort::allocate_receive_right().unwrap();
        let peer = MachPort::allocate_receive_right().unwrap();
        let io = IoThread::new(receive, peer.make_send_right().unwrap()).unwrap();
        io.wakeup_receive_right.raise_queue_limit();
        let transport = TransportMachPort {
            shared: Arc::clone(&io.shared),
            io_thread: None,
        };
        let mut buffer = MessageBuffer::new(mach::DEFAULT_RECEIVE_BUFFER_SIZE);
        for _ in 0..2 {
            for number in 0..100u32 {
                transport
                    .post_message(number.to_ne_bytes().to_vec(), Vec::new())
                    .unwrap();
            }
            assert_eq!(
                mach::send_header_only_message(
                    &io.shared.wakeup_send_right,
                    mach::IPC_RECEIVE_BARRIER_MESSAGE_ID,
                    Some(0),
                ),
                mach::MACH_MSG_SUCCESS
            );
            assert_eq!(
                mach::receive_message(&mut buffer, &io.wakeup_receive_right).unwrap().id,
                mach::IPC_WAKEUP_MESSAGE_ID
            );
            assert_eq!(
                mach::receive_message(&mut buffer, &io.wakeup_receive_right).unwrap().id,
                mach::IPC_RECEIVE_BARRIER_MESSAGE_ID
            );
            let messages = std::mem::take(&mut transport.shared.lock_send_queue().pending);
            assert_eq!(messages.len(), 100);
            for (number, message) in messages.into_iter().enumerate() {
                assert_eq!(message.bytes, (number as u32).to_ne_bytes());
            }
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
    fn the_peer_closing_ends_the_messages() {
        let (mut a, b) = paired();
        a.post_message(vec![7], Vec::new()).unwrap();
        a.close_after_sending_all_pending_messages();
        assert_eq!(b.receive().unwrap().bytes(), [7]);
        assert!(b.receive().is_none());
        assert!(!b.is_open());
    }
}
