/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The Unix domain socket transport, which speaks with `IPC::TransportSocket` on the other end.
//!
//! The socket carries frames: a 12-byte header of a type, a payload size and a descriptor count, then the payload. A
//! message's descriptors go with the bytes of its frame. When a message has more of them than one `sendmsg()` carries,
//! attachments frames carry the rest ahead of it, and the message's frame claims them all.
//!
//! A reader thread and a writer thread each hold a descriptor of the socket, and nothing else does, so the peer sees
//! this end go once both are done. They wait on the socket and on a stop pipe together, which ends them without
//! `shutdown()`, since the sandboxes of helper processes do not allow that.

use std::collections::VecDeque;
use std::io;
use std::os::fd::{AsFd, BorrowedFd, OwnedFd};
use std::os::unix::net::UnixStream;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Condvar, Mutex, MutexGuard};
use std::thread::JoinHandle;

use crate::attachment::Attachment;
use crate::receive_queue::IncomingQueue;
pub use crate::receive_queue::ReceivedMessage;
use crate::socket::{self, MAX_TRANSFER_FDS, Stop};
use crate::{MAX_MESSAGE_ATTACHMENT_COUNT, MAX_MESSAGE_PAYLOAD_SIZE};

const HEADER_SIZE: usize = 12;
const PAYLOAD_FRAME: u8 = 0;
const ATTACHMENTS_FRAME: u8 = 1;
const SHARED_MEMORY_PAYLOAD_FRAME: u8 = 2;
const MAX_INLINE_PAYLOAD_SIZE: usize = 64 * 1024;
const READ_BUFFER_SIZE: usize = 65536;
const MAX_UNPROCESSED_BUFFER_SIZE: usize = 128 * 1024 * 1024;
const MAX_UNPROCESSED_FDS: usize = MAX_MESSAGE_ATTACHMENT_COUNT + 1 + MAX_TRANSFER_FDS;

type ThreadTask = Box<dyn FnOnce() + Send>;

fn frame_header(frame_type: u8, payload_size: usize, fd_count: usize) -> [u8; HEADER_SIZE] {
    let mut header = [0; HEADER_SIZE];
    header[0] = frame_type;
    header[4..8].copy_from_slice(&(payload_size as u32).to_ne_bytes());
    header[8..12].copy_from_slice(&(fd_count as u32).to_ne_bytes());
    header
}

struct Outgoing {
    bytes: Vec<u8>,
    attachments: Vec<Attachment>,
}

#[derive(Default)]
struct OutgoingQueue {
    messages: VecDeque<Outgoing>,
    sending: bool,
    stopped: bool,
}

struct Shared {
    incoming: IncomingQueue,
    outgoing: Mutex<OutgoingQueue>,
    outgoing_changed: Condvar,
}

impl Shared {
    fn lock_outgoing(&self) -> MutexGuard<'_, OutgoingQueue> {
        self.outgoing.lock().unwrap_or_else(|error| error.into_inner())
    }

    fn stop_outgoing(&self) {
        let mut queue = self.lock_outgoing();
        queue.stopped = true;
        queue.messages.clear();
        self.outgoing_changed.notify_all();
    }
}

pub struct TransportSocket {
    shared: Arc<Shared>,
    stop: Arc<Stop>,
    reader: Option<JoinHandle<()>>,
    writer: Option<JoinHandle<()>>,
    socket_is_open: AtomicBool,
}

impl TransportSocket {
    pub fn create_paired() -> io::Result<(Self, UnixStream)> {
        let (ours, theirs) = UnixStream::pair()?;
        Ok((Self::new(ours)?, theirs))
    }

    /// Takes ownership of the socket and sets its shared open file description to blocking mode.
    pub fn new(socket: UnixStream) -> io::Result<Self> {
        Self::new_with_setup(socket, UnixStream::try_clone, |builder, task| builder.spawn(task))
    }

    fn new_with_setup(
        socket: UnixStream,
        mut clone_socket: impl FnMut(&UnixStream) -> io::Result<UnixStream>,
        mut spawn_thread: impl FnMut(std::thread::Builder, ThreadTask) -> io::Result<JoinHandle<()>>,
    ) -> io::Result<Self> {
        socket.set_nonblocking(false)?;
        let shared = Arc::new(Shared {
            incoming: IncomingQueue::default(),
            outgoing: Mutex::default(),
            outgoing_changed: Condvar::new(),
        });
        let stop = Arc::new(Stop::new()?);

        // NB: This owner stops and joins any started threads if construction fails.
        let mut transport = Self {
            shared: Arc::clone(&shared),
            stop: Arc::clone(&stop),
            reader: None,
            writer: None,
            socket_is_open: AtomicBool::new(true),
        };
        let reader_socket = clone_socket(&socket)?;
        let writer_socket = clone_socket(&socket)?;
        // NB: The threads hold the only descriptors of the socket from now on.
        drop(socket);
        let reader_shared = Arc::clone(&shared);
        let reader_stop = Arc::clone(&stop);
        transport.reader = Some(spawn_thread(
            std::thread::Builder::new().name("IPC reader (socket)".to_string()),
            Box::new(move || read_frames(&reader_socket, &reader_shared, &reader_stop)),
        )?);

        let writer_shared = Arc::clone(&shared);
        transport.writer = Some(spawn_thread(
            std::thread::Builder::new().name("IPC writer (socket)".to_string()),
            Box::new(move || write_frames(&writer_socket, &writer_shared, &stop)),
        )?);

        Ok(transport)
    }

    /// Queues a message for the writer thread to send, or refuses it once the transport has stopped sending.
    pub fn post_message(&self, bytes: Vec<u8>, attachments: Vec<Attachment>) -> io::Result<()> {
        crate::validate_message_size(bytes.len(), attachments.len())?;
        let mut queue = self.shared.lock_outgoing();
        if queue.stopped {
            return Err(io::ErrorKind::NotConnected.into());
        }
        queue.messages.push_back(Outgoing { bytes, attachments });
        self.shared.outgoing_changed.notify_all();
        Ok(())
    }

    /// Waits until every message posted so far is sent, or the transport stops.
    pub fn flush(&self) {
        let mut queue = self.shared.lock_outgoing();
        while !queue.stopped && (!queue.messages.is_empty() || queue.sending) {
            queue = self
                .shared
                .outgoing_changed
                .wait(queue)
                .unwrap_or_else(|error| error.into_inner());
        }
    }

    pub fn is_open(&self) -> bool {
        self.socket_is_open.load(Ordering::Relaxed) && !self.shared.incoming.lock().peer_eof
    }

    /// Waits for the next message, or returns `None` once the peer is gone and every message it sent was taken.
    pub fn receive(&self) -> Option<ReceivedMessage> {
        self.shared.incoming.receive()
    }

    /// Sends what is queued, then closes this end, which tells the peer that it is gone.
    pub fn close_after_sending_all_pending_messages(&mut self) {
        self.flush();
        self.close();
    }

    /// Stops sending and receiving, from any thread that shares the transport, and the peer sees this end go once the
    /// threads are done. Messages not sent yet are dropped, and a `receive()` returns `None` once it has taken what had
    /// arrived.
    pub fn shutdown(&self) {
        self.socket_is_open.store(false, Ordering::Relaxed);
        self.shared.stop_outgoing();
        self.stop.signal();
    }

    pub fn close(&mut self) {
        self.shutdown();
        if let Some(writer) = self.writer.take() {
            let _ = writer.join();
        }
        if let Some(reader) = self.reader.take() {
            let _ = reader.join();
        }
    }
}

impl Drop for TransportSocket {
    fn drop(&mut self) {
        self.close();
    }
}

fn write_frames(socket: &UnixStream, shared: &Shared, stop: &Stop) {
    write_frames_with(socket, shared, stop, |socket, message| {
        write_message(socket, message, Some(stop))
    });
}

fn write_frames_with(
    socket: &UnixStream,
    shared: &Shared,
    stop: &Stop,
    mut write_message: impl FnMut(&UnixStream, Outgoing) -> io::Result<()>,
) {
    loop {
        let message = {
            let mut queue = shared.lock_outgoing();
            loop {
                if queue.stopped {
                    return;
                }
                if let Some(message) = queue.messages.pop_front() {
                    queue.sending = true;
                    break message;
                }
                queue = shared
                    .outgoing_changed
                    .wait(queue)
                    .unwrap_or_else(|error| error.into_inner());
            }
        };
        let result = write_message(socket, message);
        {
            let mut queue = shared.lock_outgoing();
            queue.sending = false;
            if result.is_err() {
                drop(queue);
                shared.stop_outgoing();
            }
        }
        shared.outgoing_changed.notify_all();
        if result.is_err() {
            // NB: The reader stops too, and the peer sees this end go once both are done.
            stop.signal();
            return;
        }
    }
}

fn write_message(socket: &UnixStream, message: Outgoing, stop: Option<&Stop>) -> io::Result<()> {
    let shared_memory = if message.bytes.len() > MAX_INLINE_PAYLOAD_SIZE {
        Some(socket::copy_payload_to_file(&message.bytes)?)
    } else {
        None
    };
    let mut fds: Vec<BorrowedFd<'_>> = message.attachments.iter().map(Attachment::as_fd).collect();
    if let Some(file) = &shared_memory {
        fds.push(file.as_fd());
    }
    let fd_count = fds.len();
    // NB: Send the descriptors that do not fit in one sendmsg() with the message ahead of it, in full frames.
    let fds_ahead = if fds.is_empty() {
        0
    } else {
        fds.len() - ((fds.len() - 1) % MAX_TRANSFER_FDS + 1)
    };
    let remaining = fds.split_off(fds_ahead);
    for chunk in fds.chunks(MAX_TRANSFER_FDS) {
        write_all(
            socket,
            &frame_header(ATTACHMENTS_FRAME, 0, MAX_TRANSFER_FDS),
            &[],
            chunk,
            stop,
        )?;
    }

    // NB: The count includes the descriptors that went ahead.
    let header = frame_header(
        if shared_memory.is_some() {
            SHARED_MEMORY_PAYLOAD_FRAME
        } else {
            PAYLOAD_FRAME
        },
        message.bytes.len(),
        fd_count,
    );
    let payload = if shared_memory.is_some() {
        &[][..]
    } else {
        &message.bytes
    };
    write_all(socket, &header, payload, &remaining, stop)
}

/// Sends the bytes and the descriptors, waiting for room as the socket needs, unless `stop` is signaled first.
fn write_all(
    socket: &UnixStream,
    header: &[u8],
    payload: &[u8],
    fds: &[BorrowedFd<'_>],
    stop: Option<&Stop>,
) -> io::Result<()> {
    let mut slices = [io::IoSlice::new(header), io::IoSlice::new(payload)];
    let mut remaining = &mut slices[..];
    let mut fds = fds;
    while !remaining.is_empty() {
        match socket::send_with_fds(socket, remaining, fds) {
            Ok(0) => return Err(io::ErrorKind::WriteZero.into()),
            Ok(sent) => {
                io::IoSlice::advance_slices(&mut remaining, sent);
                // NB: The descriptors went with the first byte.
                fds = &[];
            }
            Err(error) if error.kind() == io::ErrorKind::Interrupted => continue,
            Err(error) if error.kind() == io::ErrorKind::WouldBlock => {
                if !socket::wait_for(socket, libc::POLLOUT, stop)? {
                    return Err(io::ErrorKind::NotConnected.into());
                }
            }
            Err(error) => return Err(error),
        }
    }
    Ok(())
}

fn read_frames(socket: &UnixStream, shared: &Shared, stop: &Stop) {
    let mut receiver = socket::Receiver::new();
    read_frames_with_limits(
        socket,
        shared,
        stop,
        MAX_UNPROCESSED_BUFFER_SIZE,
        MAX_UNPROCESSED_FDS,
        |socket, buffer, fds| {
            // NB: Stopping ends the reads as the peer closing would.
            if !socket::wait_for(socket, libc::POLLIN, Some(stop))? {
                return Ok(0);
            }
            receiver.receive_with_fds(socket, buffer, fds)
        },
    );
}

fn read_frames_with_limits(
    socket: &UnixStream,
    shared: &Shared,
    stop: &Stop,
    byte_limit: usize,
    fd_limit: usize,
    mut receive: impl FnMut(&UnixStream, &mut [u8], &mut Vec<OwnedFd>) -> io::Result<usize>,
) {
    // NB: The socket is read into the buffer after the bytes of an incomplete frame, which are all that is kept
    //     between reads.
    let mut buffer = vec![0u8; READ_BUFFER_SIZE];
    let mut unprocessed_size = 0;
    let mut unprocessed_fds: VecDeque<OwnedFd> = VecDeque::new();
    let mut fds = Vec::new();
    let mut messages = Vec::new();
    let transport_error = loop {
        if buffer.len() - unprocessed_size < READ_BUFFER_SIZE {
            buffer.resize(unprocessed_size + READ_BUFFER_SIZE, 0);
        }
        let read = match receive(socket, &mut buffer[unprocessed_size..], &mut fds) {
            Ok(0) => break false,
            Ok(read) => read,
            Err(error) if error.kind() == io::ErrorKind::Interrupted => continue,
            Err(_) => break true,
        };
        if read > byte_limit.saturating_sub(unprocessed_size)
            || fds.len() > fd_limit.saturating_sub(unprocessed_fds.len())
        {
            break true;
        }
        unprocessed_size += read;
        unprocessed_fds.extend(fds.drain(..));
        let result = parse_frames(&buffer[..unprocessed_size], &mut unprocessed_fds, &mut messages);
        if !messages.is_empty() {
            shared.incoming.push(messages.drain(..));
        }
        match result {
            Ok(parsed_size) => {
                buffer.copy_within(parsed_size..unprocessed_size, 0);
                unprocessed_size -= parsed_size;
            }
            Err(what) => {
                eprintln!("TransportSocket: Refusing a malformed {what}");
                break true;
            }
        }
    };
    // NB: Release incomplete attachments before publishing EOF, and stop both directions on a transport error.
    drop(fds);
    drop(unprocessed_fds);
    if transport_error {
        stop.signal();
        shared.stop_outgoing();
    }
    shared.incoming.mark_peer_eof();
}

fn read_u32(bytes: &[u8], offset: usize) -> usize {
    u32::from_ne_bytes([bytes[offset], bytes[offset + 1], bytes[offset + 2], bytes[offset + 3]]) as usize
}

// NB: Takes every complete frame at the front of the bytes, along with the descriptors its messages claim, and returns
//     the size of those frames.
fn parse_frames(
    bytes: &[u8],
    fds: &mut VecDeque<OwnedFd>,
    messages: &mut Vec<ReceivedMessage>,
) -> Result<usize, &'static str> {
    let mut index = 0;
    while index + HEADER_SIZE <= bytes.len() {
        let frame_type = bytes[index];
        let payload_size = read_u32(bytes, index + 4);
        let fd_count = read_u32(bytes, index + 8);
        let mut frame_payload_size = 0;
        match frame_type {
            // NB: These descriptors were queued as they arrived. The payload frame that follows claims them.
            ATTACHMENTS_FRAME => {
                if payload_size != 0 || fd_count > MAX_TRANSFER_FDS {
                    return Err("attachments frame");
                }
            }
            PAYLOAD_FRAME | SHARED_MEMORY_PAYLOAD_FRAME => {
                let in_shared_memory = frame_type == SHARED_MEMORY_PAYLOAD_FRAME;
                let max_fd_count = MAX_MESSAGE_ATTACHMENT_COUNT + usize::from(in_shared_memory);
                if payload_size > MAX_MESSAGE_PAYLOAD_SIZE
                    || (!in_shared_memory && payload_size > MAX_INLINE_PAYLOAD_SIZE)
                    || fd_count > max_fd_count
                    || (in_shared_memory && fd_count == 0)
                {
                    return Err("message size");
                }
                if !in_shared_memory {
                    frame_payload_size = payload_size;
                }
                if HEADER_SIZE + frame_payload_size > bytes.len() - index || fd_count > fds.len() {
                    break;
                }
                let attachment_count = fd_count - usize::from(in_shared_memory);
                let attachments = fds.drain(..attachment_count).map(Attachment::from).collect();
                let payload_start = index + HEADER_SIZE;
                let payload = if in_shared_memory {
                    socket::copy_payload_from_file(fds.pop_front().unwrap(), payload_size)
                        .map_err(|_| "shared memory payload")?
                } else {
                    bytes[payload_start..payload_start + payload_size].to_vec()
                };
                messages.push(ReceivedMessage {
                    bytes: payload,
                    attachments,
                });
            }
            _ => return Err("frame type"),
        }
        index += HEADER_SIZE + frame_payload_size;
    }
    Ok(index)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::{Read, Write};

    fn paired() -> (TransportSocket, TransportSocket) {
        let (a, theirs) = TransportSocket::create_paired().unwrap();
        (a, TransportSocket::new(theirs).unwrap())
    }

    #[test]
    fn shared_memory_frames_claim_the_last_descriptor_and_leave_following_frames() {
        let payload = vec![7; MAX_INLINE_PAYLOAD_SIZE + 1];
        let memory = socket::copy_payload_to_file(&payload).unwrap();
        let (reader, writer) = std::io::pipe().unwrap();
        let mut fds = VecDeque::from([OwnedFd::from(writer), OwnedFd::from(memory)]);
        let mut frames = frame_header(SHARED_MEMORY_PAYLOAD_FRAME, payload.len(), 2).to_vec();
        frames.extend_from_slice(&frame_header(PAYLOAD_FRAME, 1, 0));
        frames.push(9);
        let mut messages = Vec::new();
        assert_eq!(parse_frames(&frames, &mut fds, &mut messages).unwrap(), frames.len());
        assert_eq!(messages.len(), 2);
        assert_eq!(messages[0].bytes(), payload);
        assert_eq!(messages[0].attachments.len(), 1);
        assert_eq!(messages[1].bytes(), [9]);
        assert!(fds.is_empty());
        drop(messages);
        assert_pipe_closed(reader);
    }

    #[test]
    fn shared_memory_frames_refuse_missing_or_short_files() {
        let frames = frame_header(SHARED_MEMORY_PAYLOAD_FRAME, 1, 0).to_vec();
        assert!(parse_frames(&frames, &mut VecDeque::new(), &mut Vec::new()).is_err());
        let memory = socket::copy_payload_to_file(&[1]).unwrap();
        let frames = frame_header(SHARED_MEMORY_PAYLOAD_FRAME, 2, 1).to_vec();
        let mut fds = VecDeque::from([OwnedFd::from(memory)]);
        assert!(parse_frames(&frames, &mut fds, &mut Vec::new()).is_err());
        assert!(fds.is_empty());
    }

    #[test]
    fn inline_frames_refuse_payloads_above_the_inline_limit() {
        let frames = frame_header(PAYLOAD_FRAME, MAX_INLINE_PAYLOAD_SIZE + 1, 0).to_vec();
        assert!(parse_frames(&frames, &mut VecDeque::new(), &mut Vec::new()).is_err());
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
    fn large_messages_arrive_whole() {
        let (a, b) = paired();
        let bytes: Vec<u8> = (0..1_000_000u32).map(|i| i as u8).collect();
        a.post_message(bytes.clone(), Vec::new()).unwrap();
        assert_eq!(b.receive().unwrap().bytes(), bytes);
    }

    #[test]
    fn many_descriptors_go_ahead_in_attachments_frames() {
        let (a, b) = paired();
        let mut readers = Vec::new();
        let mut attachments = Vec::new();
        for _ in 0..300 {
            let (reader, writer) = std::io::pipe().unwrap();
            readers.push(reader);
            attachments.push(Attachment::from_fd(OwnedFd::from(writer)).unwrap());
        }
        a.post_message(vec![9], attachments).unwrap();
        let message = b.receive().unwrap();
        assert_eq!(message.bytes(), [9]);
        assert_eq!(message.attachments.len(), 300);
        for (attachment, reader) in message.attachments.into_iter().zip(readers) {
            let mut writer = std::fs::File::from(attachment.into_fd().unwrap());
            std::io::Write::write_all(&mut writer, b"!").unwrap();
            drop(writer);
            let mut buffer = Vec::new();
            std::io::Read::read_to_end(&mut std::fs::File::from(OwnedFd::from(reader)), &mut buffer).unwrap();
            assert_eq!(buffer, b"!");
        }
    }

    #[test]
    fn the_peer_closing_ends_the_messages() {
        let (mut a, mut b) = paired();
        a.post_message(vec![7], Vec::new()).unwrap();
        a.close_after_sending_all_pending_messages();
        assert_eq!(b.receive().unwrap().bytes(), [7]);
        assert!(b.receive().is_none());
        assert!(!b.is_open());
        b.close();
        assert!(!b.is_open());
    }

    #[test]
    fn malformed_frames_end_the_messages() {
        let (ours, theirs) = UnixStream::pair().unwrap();
        let b = TransportSocket::new(theirs).unwrap();
        std::io::Write::write_all(&mut &ours, &frame_header(7, 0, 0)).unwrap();
        assert!(b.receive().is_none());
        assert_eq!((&ours).read(&mut [0]).unwrap(), 0);
    }

    fn empty_shared() -> Shared {
        Shared {
            incoming: IncomingQueue::default(),
            outgoing: Mutex::default(),
            outgoing_changed: Condvar::new(),
        }
    }

    fn assert_pipe_closed(reader: std::io::PipeReader) {
        let mut bytes = Vec::new();
        std::fs::File::from(OwnedFd::from(reader))
            .read_to_end(&mut bytes)
            .unwrap();
        assert!(bytes.is_empty());
    }

    #[test]
    fn complete_messages_are_delivered_before_a_malformed_frame() {
        for malformed in [
            frame_header(7, 0, 0),
            frame_header(ATTACHMENTS_FRAME, 1, 0),
            frame_header(PAYLOAD_FRAME, MAX_INLINE_PAYLOAD_SIZE + 1, 0),
            frame_header(SHARED_MEMORY_PAYLOAD_FRAME, 2, 1),
        ] {
            let (socket, peer) = UnixStream::pair().unwrap();
            let (reader, writer) = std::io::pipe().unwrap();
            let (unclaimed_reader, unclaimed_writer) = std::io::pipe().unwrap();
            let memory = socket::copy_payload_to_file(&[1]).unwrap();
            let mut frames = frame_header(PAYLOAD_FRAME, 1, 1).to_vec();
            frames.push(42);
            frames.extend_from_slice(&malformed);
            let mut fds = Vec::from([
                OwnedFd::from(writer),
                OwnedFd::from(memory),
                OwnedFd::from(unclaimed_writer),
            ]);
            let shared = empty_shared();
            let stop = Stop::new().unwrap();
            read_frames_with_limits(&socket, &shared, &stop, 1024, 4, |_, buffer, received_fds| {
                assert!(!fds.is_empty(), "reader must stop at the malformed frame");
                buffer[..frames.len()].copy_from_slice(&frames);
                received_fds.append(&mut fds);
                Ok(frames.len())
            });
            let mut incoming = shared.incoming.lock();
            assert!(incoming.peer_eof);
            assert_eq!(incoming.messages.len(), 1);
            let message = incoming.messages.pop_front().unwrap();
            assert_eq!(message.bytes(), [42]);
            assert_eq!(message.attachments.len(), 1);
            let mut writer = std::fs::File::from(message.attachments.into_iter().next().unwrap().into_fd().unwrap());
            writer.write_all(b"ok").unwrap();
            drop(writer);
            let mut bytes = Vec::new();
            std::fs::File::from(OwnedFd::from(reader))
                .read_to_end(&mut bytes)
                .unwrap();
            assert_eq!(bytes, b"ok");
            assert_pipe_closed(unclaimed_reader);
            assert!(shared.lock_outgoing().stopped);
            assert!(stop.is_signaled());
            drop(peer);
        }
    }

    #[test]
    fn writer_resource_errors_stop_both_directions() {
        let (socket, _peer) = UnixStream::pair().unwrap();
        let shared = empty_shared();
        let stop = Stop::new().unwrap();
        shared.lock_outgoing().messages.push_back(Outgoing {
            bytes: vec![1],
            attachments: Vec::new(),
        });
        write_frames_with(&socket, &shared, &stop, |_, _| {
            Err(io::Error::from_raw_os_error(libc::EMFILE))
        });
        assert!(!shared.incoming.lock().peer_eof);
        assert!(shared.lock_outgoing().stopped);
        assert!(!shared.lock_outgoing().sending);
        // NB: The reader waits on the stop pipe, which ends it too.
        assert!(stop.is_signaled());
        assert!(!socket::wait_for(&socket, libc::POLLIN, Some(&stop)).unwrap());
    }

    #[test]
    fn write_failure_waits_for_the_reader_to_publish_its_messages() {
        let mut receiver = socket::Receiver::new();
        let (socket, mut peer) = UnixStream::pair().unwrap();
        let shared = empty_shared();
        let stop = Stop::new().unwrap();
        let mut first_read = true;
        read_frames_with_limits(&socket, &shared, &stop, 1024, 0, |socket, buffer, fds| {
            if !first_read {
                if !socket::wait_for(socket, libc::POLLIN, Some(&stop))? {
                    return Ok(0);
                }
                return receiver.receive_with_fds(socket, buffer, fds);
            }
            first_read = false;
            peer.write_all(&frame_header(PAYLOAD_FRAME, 1, 0)).unwrap();
            peer.write_all(&[7]).unwrap();
            let read = receiver.receive_with_fds(socket, buffer, fds)?;
            shared.lock_outgoing().messages.push_back(Outgoing {
                bytes: vec![1],
                attachments: Vec::new(),
            });
            write_frames_with(socket, &shared, &stop, |_, _| Err(io::ErrorKind::InvalidInput.into()));
            assert!(!shared.incoming.lock().peer_eof);
            Ok(read)
        });
        let mut incoming = shared.incoming.lock();
        assert_eq!(incoming.messages.pop_front().unwrap().bytes(), [7]);
        assert!(incoming.messages.is_empty());
        assert!(incoming.peer_eof);
    }

    #[test]
    fn a_socket_write_error_stops_both_directions() {
        let (transport, mut peer) = TransportSocket::create_paired().unwrap();
        peer.shutdown(std::net::Shutdown::Read).unwrap();
        transport.post_message(vec![1], Vec::new()).unwrap();
        transport.flush();
        assert!(transport.receive().is_none());
        assert!(!transport.is_open());
        // NB: The peer sees this end go once the threads are done, which dropping the transport waits for.
        drop(transport);
        assert_eq!(peer.write(&[1]).unwrap_err().kind(), io::ErrorKind::BrokenPipe);
    }

    #[test]
    fn incomplete_bytes_are_limited_and_queued_descriptors_are_released() {
        let mut receiver = socket::Receiver::new();
        assert_eq!(MAX_UNPROCESSED_BUFFER_SIZE, 128 * 1024 * 1024);
        let (socket, mut peer) = UnixStream::pair().unwrap();
        let (reader, writer) = std::io::pipe().unwrap();
        let shared = empty_shared();
        let stop = Stop::new().unwrap();
        let mut step = 0;
        read_frames_with_limits(&socket, &shared, &stop, 24, 4, move |socket, buffer, fds| {
            match step {
                0 => write_all(&peer, &frame_header(PAYLOAD_FRAME, 0, 2), &[], &[writer.as_fd()], None).unwrap(),
                1 => peer.write_all(&[0; 12]).unwrap(),
                2 => peer.write_all(&[0]).unwrap(),
                _ => panic!("reader must stop at the byte limit"),
            }
            step += 1;
            receiver.receive_with_fds(socket, buffer, fds)
        });
        assert!(shared.incoming.lock().peer_eof);
        assert!(shared.incoming.lock().messages.is_empty());
        assert!(shared.lock_outgoing().stopped);
        assert_pipe_closed(reader);
    }

    #[test]
    fn attachments_frames_have_a_cumulative_descriptor_limit() {
        let mut receiver = socket::Receiver::new();
        assert_eq!(MAX_UNPROCESSED_FDS, 16129);
        let (socket, peer) = UnixStream::pair().unwrap();
        let (reader, writer) = std::io::pipe().unwrap();
        let shared = empty_shared();
        let stop = Stop::new().unwrap();
        let mut step = 0;
        read_frames_with_limits(&socket, &shared, &stop, 24, 2, |socket, buffer, fds| {
            assert!(step < 3, "reader must stop at the descriptor limit");
            step += 1;
            write_all(
                &peer,
                &frame_header(ATTACHMENTS_FRAME, 0, 1),
                &[],
                &[writer.as_fd()],
                None,
            )
            .unwrap();
            receiver.receive_with_fds(socket, buffer, fds)
        });
        assert_eq!(step, 3);
        assert!(shared.incoming.lock().peer_eof);
        assert!(stop.is_signaled());
        drop(writer);
        assert_pipe_closed(reader);
    }

    #[test]
    fn complete_frames_do_not_accumulate_towards_the_limits() {
        let mut receiver = socket::Receiver::new();
        let (socket, peer) = UnixStream::pair().unwrap();
        let shared = empty_shared();
        let stop = Stop::new().unwrap();
        let mut step = 0;
        read_frames_with_limits(&socket, &shared, &stop, HEADER_SIZE, 0, |socket, buffer, fds| {
            if step == 100 {
                peer.shutdown(std::net::Shutdown::Write).unwrap();
            } else {
                write_all(&peer, &frame_header(PAYLOAD_FRAME, 0, 0), &[], &[], None).unwrap();
                step += 1;
            }
            receiver.receive_with_fds(socket, buffer, fds)
        });
        assert_eq!(shared.incoming.lock().messages.len(), 100);
    }

    #[test]
    fn truncated_descriptors_close_the_connection_and_release_every_descriptor() {
        let mut receiver = socket::Receiver::new();
        let (socket, peer) = UnixStream::pair().unwrap();
        let (reader, writer) = std::io::pipe().unwrap();
        write_all(
            &peer,
            &frame_header(PAYLOAD_FRAME, 0, 3),
            &[],
            &[writer.as_fd(); 3],
            None,
        )
        .unwrap();
        drop(writer);
        let shared = empty_shared();
        let stop = Stop::new().unwrap();
        read_frames_with_limits(&socket, &shared, &stop, 1024, 10, |socket, buffer, fds| {
            receiver.receive_with_fd_capacity(socket, buffer, fds, 1)
        });
        assert!(shared.incoming.lock().peer_eof);
        assert!(shared.incoming.lock().messages.is_empty());
        assert!(stop.is_signaled());
        assert_pipe_closed(reader);
    }

    #[test]
    fn construction_prepares_all_clones_before_starting_threads() {
        for failing_clone in 0..2 {
            let (socket, mut peer) = UnixStream::pair().unwrap();
            let mut clones = 0;
            let result = TransportSocket::new_with_setup(
                socket,
                |socket| {
                    let index = clones;
                    clones += 1;
                    if index == failing_clone {
                        return Err(io::Error::from_raw_os_error(libc::EMFILE));
                    }
                    socket.try_clone()
                },
                |_, _| panic!("threads must not start before both clones succeed"),
            );
            assert_eq!(result.err().unwrap().raw_os_error(), Some(libc::EMFILE));
            assert_eq!(peer.read(&mut [0]).unwrap(), 0);
        }
    }

    #[test]
    fn construction_failure_shuts_down_and_joins_started_threads() {
        for failing_thread in 0..2 {
            let (socket, mut peer) = UnixStream::pair().unwrap();
            let finished = Arc::new(AtomicBool::new(false));
            let mut started = 0;
            let result = TransportSocket::new_with_setup(socket, UnixStream::try_clone, |builder, task| {
                let index = started;
                started += 1;
                if index == failing_thread {
                    return Err(io::Error::from_raw_os_error(libc::EAGAIN));
                }
                let finished = Arc::clone(&finished);
                builder.spawn(move || {
                    task();
                    finished.store(true, Ordering::Relaxed);
                })
            });
            assert_eq!(result.err().unwrap().raw_os_error(), Some(libc::EAGAIN));
            assert_eq!(started, failing_thread + 1);
            assert_eq!(finished.load(Ordering::Relaxed), failing_thread == 1);
            assert_eq!(peer.read(&mut [0]).unwrap(), 0);
        }
    }
}
