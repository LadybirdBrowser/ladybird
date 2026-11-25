/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The Unix domain socket transport.
//!
//! The socket carries frames: a 12-byte header of a type, a payload size and a descriptor count, then the payload. A
//! message's descriptors go with the first byte of its frame. When a message has more of them than one `sendmsg()`
//! carries, attachments frames carry the rest ahead of it, and the message's frame claims them all. A payload larger
//! than a frame takes goes in an anonymous file, whose descriptor is the message's last.
//!
//! All transports of a process share one IO thread, which waits on every transport's socket at once. The thread that
//! posts a message writes it to the socket itself, unless earlier messages still wait for room in the socket; then it
//! waits behind them, and the IO thread writes them as the peer makes room.

use std::collections::{HashMap, VecDeque};
use std::io::{self, IoSlice};
use std::net::Shutdown;
use std::os::fd::{AsFd, AsRawFd, BorrowedFd, OwnedFd, RawFd};
use std::os::unix::net::UnixStream;
use std::sync::atomic::{AtomicBool, AtomicU64, AtomicUsize, Ordering};
use std::sync::{Arc, Condvar, Mutex, OnceLock};
use std::time::Duration;

use crate::attachment::Attachment;
use crate::poller::{Event, Poller};
pub use crate::receive_queue::{PayloadBytes, ReceivedMessage};
use crate::socket::{self, MAX_TRANSFER_FDS};
use crate::transport::{
    AtomicIoThreadState, AttentionList, IDLE_MEMORY_RELEASE_DELAY_IN_MILLISECONDS, Inbox, Incoming,
    IncomingMessageObserver, IoThreadMembership, IoThreadState, MonotonicDeadline, OutgoingPayload, OwnedPayload,
    SharesIoThread, is_shared_io_thread, lock, release_unused_allocator_memory, start_shared_io_thread, wait_for,
};
use crate::{MAX_MESSAGE_ATTACHMENT_COUNT, MAX_MESSAGE_PAYLOAD_SIZE};

const HEADER_SIZE: usize = 12;
const PAYLOAD_FRAME: u8 = 0;
const ATTACHMENTS_FRAME: u8 = 1;
const SHARED_MEMORY_PAYLOAD_FRAME: u8 = 2;
const MAX_INLINE_PAYLOAD_SIZE: usize = 64 * 1024;
const SOCKET_BUFFER_SIZE: usize = 128 * 1024;
const MAX_UNPROCESSED_BUFFER_SIZE: usize = 128 * 1024 * 1024;
// NB: Frames are parsed after every read, and one read takes the descriptors of at most one sendmsg(). So, the most a
//     well-behaved peer can leave unclaimed is a whole message's, plus those that arrive with the read that completes
//     it.
const MAX_UNPROCESSED_FDS: usize = MAX_MESSAGE_ATTACHMENT_COUNT + 1 + MAX_TRANSFER_FDS;

// NB: A turn of the IO thread reads and writes a bounded amount for each transport, so that a peer which never stops
//     writing, or a long backlog, does not keep it from the other transports.
const READ_BUFFER_SIZE: usize = 64 * 1024;
const MAX_READS_PER_TURN: usize = 4;
const MAX_BYTES_SENT_PER_TURN: usize = 256 * 1024;
const MAX_FRAMES_PER_SEND: usize = 32;
const MAX_UNSENDABLE_FRAMES_RELEASED_PER_TURN: usize = 64;
const FLUSH_WAKEUP_REARM_INTERVAL: Duration = Duration::from_millis(50);

fn frame_header(frame_type: u8, payload_size: usize, fd_count: usize) -> [u8; HEADER_SIZE] {
    let mut header = [0; HEADER_SIZE];
    header[0] = frame_type;
    header[4..8].copy_from_slice(&(payload_size as u32).to_ne_bytes());
    header[8..12].copy_from_slice(&(fd_count as u32).to_ne_bytes());
    header
}

/// One end of a connection, as `IPC::TransportHandle` holds it: the socket, and what the last owner read off it
/// without seeing the end of the message, which the next owner reads first.
pub struct SocketHandle {
    pub socket: OwnedFd,
    pub unprocessed_bytes: Vec<u8>,
    pub unprocessed_attachments: Vec<Attachment>,
}

impl SocketHandle {
    pub fn new(socket: OwnedFd) -> Self {
        Self {
            socket,
            unprocessed_bytes: Vec::new(),
            unprocessed_attachments: Vec::new(),
        }
    }
}

impl From<UnixStream> for SocketHandle {
    fn from(socket: UnixStream) -> Self {
        Self::new(socket.into())
    }
}

enum FramePayload<'a> {
    None,
    Lent(&'a [u8]),
    Owned(OwnedPayload),
}

impl FramePayload<'_> {
    fn bytes(&self) -> &[u8] {
        match self {
            Self::None => &[],
            Self::Lent(bytes) => bytes,
            Self::Owned(payload) => (**payload).as_ref(),
        }
    }
}

struct Frame<'a> {
    header: [u8; HEADER_SIZE],
    payload: FramePayload<'a>,
    // NB: Dropped once they have gone with a first byte, as the kernel holds its own references to files in flight.
    fds: Vec<OwnedFd>,
    sent_byte_count: usize,
    // NB: False for the frames of a message that follow its first, as attachments frames go ahead of the message.
    starts_message: bool,
}

impl Frame<'_> {
    fn new(header: [u8; HEADER_SIZE], payload: FramePayload<'_>, fds: Vec<OwnedFd>, starts_message: bool) -> Frame<'_> {
        Frame {
            header,
            payload,
            fds,
            sent_byte_count: 0,
            starts_message,
        }
    }

    fn size(&self) -> usize {
        HEADER_SIZE + self.payload.bytes().len()
    }

    fn ends_message(&self) -> bool {
        self.header[0] != ATTACHMENTS_FRAME
    }

    fn into_owned(self, hand_over: &mut dyn FnMut() -> OwnedPayload) -> Frame<'static> {
        let payload = match self.payload {
            FramePayload::None => FramePayload::None,
            FramePayload::Lent(_) => FramePayload::Owned(hand_over()),
            FramePayload::Owned(payload) => FramePayload::Owned(payload),
        };
        Frame {
            header: self.header,
            payload,
            fds: self.fds,
            sent_byte_count: self.sent_byte_count,
            starts_message: self.starts_message,
        }
    }
}

// NB: The descriptors that do not fit in one sendmsg() go ahead of the message, in full attachments frames.
fn frames_of_message(payload: FramePayload<'_>, attachments: Vec<Attachment>) -> io::Result<VecDeque<Frame<'_>>> {
    let payload_size = payload.bytes().len();
    let mut fds = attachments
        .into_iter()
        .map(Attachment::into_fd)
        .collect::<io::Result<Vec<_>>>()?;
    let (frame_type, payload) = if payload_size > MAX_INLINE_PAYLOAD_SIZE {
        fds.push(socket::copy_payload_to_file(payload.bytes())?.into());
        (SHARED_MEMORY_PAYLOAD_FRAME, FramePayload::None)
    } else {
        (PAYLOAD_FRAME, payload)
    };
    let total_fd_count = fds.len();
    let attachments_frame_count = total_fd_count.saturating_sub(1) / MAX_TRANSFER_FDS;
    let mut fds = fds.into_iter();
    let mut frames = VecDeque::with_capacity(attachments_frame_count + 1);
    for index in 0..attachments_frame_count {
        frames.push_back(Frame::new(
            frame_header(ATTACHMENTS_FRAME, 0, MAX_TRANSFER_FDS),
            FramePayload::None,
            fds.by_ref().take(MAX_TRANSFER_FDS).collect(),
            index == 0,
        ));
    }
    frames.push_back(Frame::new(
        frame_header(frame_type, payload_size, total_fd_count),
        payload,
        fds.collect(),
        attachments_frame_count == 0,
    ));
    Ok(frames)
}

enum SendProgress {
    Drained,
    Incomplete,
    Failed(io::Error),
}

// NB: Writes as many frames as the socket takes, several in one sendmsg(). Descriptors go with the first byte of a
//     sendmsg(), so only its first frame may carry any: no byte goes ahead of the descriptors it needs, and no
//     descriptors go ahead of the frame that claims them, which the peer's limit on unclaimed ones counts on.
fn send_frames(
    socket: RawFd,
    frames: &mut VecDeque<Frame<'_>>,
    budget: usize,
    sent_message_count: &mut usize,
) -> SendProgress {
    let mut sent_in_total = 0;
    loop {
        if frames.is_empty() {
            return SendProgress::Drained;
        }
        if sent_in_total >= budget {
            return SendProgress::Incomplete;
        }
        let mut slices: [IoSlice<'_>; 2 * MAX_FRAMES_PER_SEND] = std::array::from_fn(|_| IoSlice::new(&[]));
        let mut slice_count = 0;
        // NB: A write offers no more than what is left of the budget, and the next turn goes on from where it ended.
        let mut budget_left = budget - sent_in_total;
        let fds: Vec<BorrowedFd<'_>> = frames[0].fds.iter().map(AsFd::as_fd).collect();
        for (frame_index, frame) in frames.iter().take(MAX_FRAMES_PER_SEND).enumerate() {
            if budget_left == 0 || (frame_index > 0 && !frame.fds.is_empty()) {
                break;
            }
            let payload = frame.payload.bytes();
            let unsent_header = &frame.header[frame.sent_byte_count.min(HEADER_SIZE)..];
            let unsent_payload = &payload[frame.sent_byte_count.saturating_sub(HEADER_SIZE)..];
            for unsent in [unsent_header, unsent_payload] {
                let offered = unsent.len().min(budget_left);
                if offered > 0 {
                    slices[slice_count] = IoSlice::new(&unsent[..offered]);
                    slice_count += 1;
                    budget_left -= offered;
                }
            }
        }
        let result = socket::send_with_fds(socket, &slices[..slice_count], &fds);
        drop(fds);
        match result {
            Ok(0) => return SendProgress::Failed(io::ErrorKind::WriteZero.into()),
            Ok(mut written) => {
                sent_in_total += written;
                frames[0].fds.clear();
                while written > 0 {
                    let frame = frames.front_mut().expect("the socket took no more than was given");
                    let unsent = frame.size() - frame.sent_byte_count;
                    if written < unsent {
                        frame.sent_byte_count += written;
                        break;
                    }
                    written -= unsent;
                    if frame.ends_message() {
                        *sent_message_count += 1;
                    }
                    frames.pop_front();
                }
            }
            Err(error) if error.kind() == io::ErrorKind::WouldBlock => return SendProgress::Incomplete,
            Err(error) if error.kind() == io::ErrorKind::Interrupted => {}
            Err(error) => return SendProgress::Failed(error),
        }
    }
}

fn read_u32(bytes: &[u8], offset: usize) -> usize {
    u32::from_ne_bytes([bytes[offset], bytes[offset + 1], bytes[offset + 2], bytes[offset + 3]]) as usize
}

// NB: Takes every complete frame at the front of the bytes, along with the descriptors its messages claim, and returns
//     the size of those frames. A malformed frame fails it, after the messages of the frames before it.
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
                // NB: Only a payload too large for a frame goes in a file.
                if payload_size > MAX_MESSAGE_PAYLOAD_SIZE
                    || in_shared_memory != (payload_size > MAX_INLINE_PAYLOAD_SIZE)
                    || fd_count > max_fd_count
                    || (in_shared_memory && fd_count == 0)
                {
                    return Err("message size");
                }
                if !in_shared_memory {
                    frame_payload_size = payload_size;
                }
                // NB: A frame's descriptors arrive ahead of it, or with the first byte of the write that carries it. So,
                //     a well-behaved peer never leaves a frame waiting for them, and what is left unparsed is at most the
                //     start of one frame.
                if fd_count > fds.len() {
                    return Err("frame without its descriptors");
                }
                if HEADER_SIZE + frame_payload_size > bytes.len() - index {
                    break;
                }
                let attachment_count = fd_count - usize::from(in_shared_memory);
                let attachments = fds.drain(..attachment_count).map(Attachment::from).collect();
                let payload_start = index + HEADER_SIZE;
                let payload = if in_shared_memory {
                    socket::payload_from_file(
                        fds.pop_front().expect("the frame's descriptors are queued"),
                        payload_size,
                    )
                    .map_err(|_| "shared memory payload")?
                } else {
                    PayloadBytes::Inline(bytes[payload_start..payload_start + payload_size].to_vec())
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

struct SendState {
    // NB: Frames the socket had no room for, in order. While there are any, a newly posted message waits behind them,
    //     and the IO thread writes them as the peer makes room.
    backlog: VecDeque<Frame<'static>>,
    backlog_message_count: usize,
    // NB: Whether the IO thread waits for room in the socket, which only it changes, while the backlog has frames.
    writable_interest: bool,
}

impl SendState {
    fn take_backlog(&mut self) -> VecDeque<Frame<'static>> {
        self.backlog_message_count = 0;
        std::mem::take(&mut self.backlog)
    }
}

struct ReceiveState {
    // NB: The bytes and descriptors of frames that have yet to arrive whole.
    unprocessed_bytes: Vec<u8>,
    unprocessed_fds: VecDeque<OwnedFd>,
    bytes_taken_from_socket: u64,
    times_found_drained: u64,
    byte_limit: usize,
    fd_limit: usize,
}

impl ReceiveState {
    // NB: The limits only count what remains of frames that are still incomplete, as parsing comes first. A peer that
    //     writes faster than this end reads would otherwise run into them with valid traffic.
    fn take_in(
        &mut self,
        bytes: &[u8],
        fds: &mut Vec<OwnedFd>,
        messages: &mut Vec<ReceivedMessage>,
    ) -> Result<(), &'static str> {
        // NB: Nothing after a malformed frame is read, and nothing before it is to be handed over again with a transfer.
        self.take_in_frames(bytes, fds, messages)
            .inspect_err(|_| self.discard_incomplete_frames())
    }

    fn discard_incomplete_frames(&mut self) {
        self.unprocessed_bytes = Vec::new();
        self.unprocessed_fds.clear();
    }

    // NB: Growing the buffer fails the peer rather than the process.
    fn keep_unprocessed(&mut self, bytes: &[u8]) -> Result<(), &'static str> {
        self.unprocessed_bytes
            .try_reserve(bytes.len())
            .map_err(|_| "amount of incomplete frames")?;
        self.unprocessed_bytes.extend_from_slice(bytes);
        Ok(())
    }

    fn take_in_frames(
        &mut self,
        bytes: &[u8],
        fds: &mut Vec<OwnedFd>,
        messages: &mut Vec<ReceivedMessage>,
    ) -> Result<(), &'static str> {
        self.unprocessed_fds.extend(fds.drain(..));
        if self.unprocessed_bytes.is_empty() {
            let parsed = parse_frames(bytes, &mut self.unprocessed_fds, messages)?;
            self.keep_unprocessed(&bytes[parsed..])?;
        } else {
            self.keep_unprocessed(bytes)?;
            let parsed = parse_frames(&self.unprocessed_bytes, &mut self.unprocessed_fds, messages)?;
            self.unprocessed_bytes.drain(..parsed);
        }
        if self.unprocessed_bytes.len() > self.byte_limit {
            return Err("amount of incomplete frames");
        }
        if self.unprocessed_fds.len() > self.fd_limit {
            return Err("number of unclaimed descriptors");
        }
        // NB: Once the frames it grew for are parsed, the buffer lets go of the room that what is left doesn't need. The
        //     rest moves to a buffer of its own, which takes an allocation that may fail, and the old buffer stays then.
        if self.unprocessed_bytes.capacity() > READ_BUFFER_SIZE.max(2 * self.unprocessed_bytes.len()) {
            let mut compacted = Vec::new();
            if compacted.try_reserve_exact(self.unprocessed_bytes.len()).is_ok() {
                compacted.extend_from_slice(&self.unprocessed_bytes);
                self.unprocessed_bytes = compacted;
            }
        }
        Ok(())
    }
}

// NB: A receive barrier is satisfied once the bytes that were on the socket when it began are taken in, or once the
//     socket is found drained after it began. The kernel's count of the bytes on a socket can include the part of a
//     message that this end has read already, so the count alone might never be reached.
#[derive(Default)]
struct ReceiveBarrier {
    bytes_published: u64,
    times_found_drained: u64,
}

enum ReadEnding {
    PeerGone,
    Misbehaved(&'static str),
}

struct ReadTurn {
    io_is_over: bool,
    found_drained: bool,
}

struct Shared {
    // NB: The transport owns the socket, and keeps it open until the IO thread is done with it.
    socket: RawFd,
    poller_token: u64,
    io_thread_state: AtomicIoThreadState,
    // NB: True while release_for_transfer() moves the socket to a new owner. Shutdown of this end is then part of the
    //     handoff, not the peer going away.
    is_being_transferred: AtomicBool,
    // NB: Set once a write fails, after which nothing more is taken to send. What the peer sent before keeps arriving.
    send_failed: AtomicBool,
    // NB: Set before the owner asks IO to stop. IO that stops on its own, as for a peer that misbehaves, hands what is
    //     left unsent to the IO thread to release, as nothing else would until the owner lets go of the transport.
    stop_was_requested_by_owner: AtomicBool,
    io_thread_membership: IoThreadMembership,
    // NB: While receive barriers wait, the IO thread reads the socket on its own until it finds it drained.
    barriers_waiting: AtomicUsize,

    // NB: Held by whichever thread writes, which is the thread that posts a message unless the message has to wait.
    send: Mutex<SendState>,
    sent: Condvar,

    // NB: Held by the IO thread while it reads, and by a receive barrier as it looks at how far reading has got.
    receive: Mutex<ReceiveState>,

    inbox: Inbox<ReceiveBarrier>,
}

impl SharesIoThread for Shared {
    fn io_thread_membership(&self) -> &IoThreadMembership {
        &self.io_thread_membership
    }
}

impl Shared {
    // NB: Gives back what can no longer be sent, for the caller to release.
    fn fail_sending(&self, send: &mut SendState, error: &io::Error) -> VecDeque<Frame<'static>> {
        if !matches!(error.kind(), io::ErrorKind::BrokenPipe | io::ErrorKind::ConnectionReset) {
            eprintln!("TransportSocket: send failed: {error}");
        }
        self.send_failed.store(true, Ordering::Release);
        // NB: The peer hears that nothing more comes from this end, and closes its own, which ends what this end reads
        //     once it has taken in what the peer sent before.
        socket::shutdown(self.socket, Shutdown::Write);
        self.sent.notify_all();
        // NB: An owner that waits for the response to what it posted learns that it no longer can, even if the peer
        //     keeps its end open.
        self.inbox.interrupt_wait_until_readable();
        send.take_backlog()
    }

    // NB: Gives back what was left unsent, for the caller to release. Once the socket has taken part of a message, nothing
    //     else may follow it, even after a transfer: the peer would read it as the rest of that frame, or have a later
    //     message claim the descriptors that went ahead.
    fn abandon_backlog(&self, send: &mut SendState) -> VecDeque<Frame<'static>> {
        if send
            .backlog
            .front()
            .is_some_and(|frame| frame.sent_byte_count > 0 || !frame.starts_message)
        {
            socket::shutdown(self.socket, Shutdown::Write);
        }
        send.take_backlog()
    }

    // NB: Runs on the IO thread.
    fn read_turn(&self, locals: &mut IoThreadLocals, hung_up: bool) -> ReadTurn {
        let mut ending = None;
        let mut found_drained = false;
        let (published_before, published_through, times_found_drained) = {
            let mut receive = lock(&self.receive);
            let published_before = receive.bytes_taken_from_socket;
            for _ in 0..MAX_READS_PER_TURN {
                let read =
                    locals
                        .receiver
                        .receive_with_fds(self.socket, &mut locals.read_buffer, &mut locals.received_fds);
                match read {
                    Ok(0) => {
                        locals.received_fds.clear();
                        ending = Some(ReadEnding::PeerGone);
                        break;
                    }
                    Ok(read) => {
                        receive.bytes_taken_from_socket += read as u64;
                        if let Err(what) = receive.take_in(
                            &locals.read_buffer[..read],
                            &mut locals.received_fds,
                            &mut locals.messages,
                        ) {
                            ending = Some(ReadEnding::Misbehaved(what));
                            break;
                        }
                    }
                    Err(error) if error.kind() == io::ErrorKind::WouldBlock => {
                        found_drained = true;
                        break;
                    }
                    Err(error) if error.kind() == io::ErrorKind::Interrupted => {}
                    Err(error) => {
                        // NB: A read that failed may still have delivered descriptors, which are this end's to close.
                        locals.received_fds.clear();
                        ending = Some(match error.kind() {
                            io::ErrorKind::InvalidData => {
                                // NB: The read took bytes off the socket that no frame will ever be parsed from.
                                receive.discard_incomplete_frames();
                                ReadEnding::Misbehaved("control message")
                            }
                            io::ErrorKind::ConnectionReset => ReadEnding::PeerGone,
                            _ => {
                                eprintln!("TransportSocket: read failed: {error}");
                                ReadEnding::PeerGone
                            }
                        });
                        break;
                    }
                }
            }
            // NB: A socket that has hung up and has nothing left to read has no more to say.
            if found_drained && hung_up && ending.is_none() {
                ending = Some(ReadEnding::PeerGone);
            }
            if found_drained {
                receive.times_found_drained += 1;
            }
            (
                published_before,
                receive.bytes_taken_from_socket,
                receive.times_found_drained,
            )
        };

        // NB: A transfer ends this end's IO without the peer going away.
        let peer_is_gone = ending.is_some() && !self.is_being_transferred.load(Ordering::Acquire);
        if peer_is_gone
            || !locals.messages.is_empty()
            || published_through != published_before
            || self.barriers_waiting.load(Ordering::Acquire) > 0
        {
            self.inbox
                .notify_incoming_message_observer(locals.messages.iter().map(ReceivedMessage::bytes));
            let messages = locals.messages.drain(..);
            let publish_progress = |incoming: &mut Incoming<ReceiveBarrier>| {
                incoming.barrier.bytes_published = published_through;
                incoming.barrier.times_found_drained = times_found_drained;
            };
            if peer_is_gone {
                self.inbox.deliver_with_peer_eof(messages, publish_progress);
            } else {
                self.inbox.deliver(messages, publish_progress);
            }
            // NB: Frames that waited for their descriptors can make a batch far larger than what one turn reads.
            if locals.messages.capacity() > MAX_READS_PER_TURN * READ_BUFFER_SIZE / HEADER_SIZE {
                locals.messages = Vec::new();
            }
        }

        let io_is_over = match ending {
            None => false,
            Some(ReadEnding::PeerGone) => {
                self.io_thread_state.store(IoThreadState::Stopped);
                true
            }
            Some(ReadEnding::Misbehaved(what)) => {
                eprintln!("TransportSocket: Refusing a malformed {what}");
                // NB: The peer hears at once that this end is gone.
                socket::shutdown(self.socket, Shutdown::Both);
                self.io_thread_state.store(IoThreadState::Stopped);
                true
            }
        };
        ReadTurn {
            io_is_over,
            found_drained,
        }
    }

    // NB: Runs on the IO thread: writes a bounded part of the backlog, watches for room in the socket while there is a
    //     backlog left, and stops IO as asked. Answers whether the transport's IO is over.
    fn do_pending_io(&self, io_thread: &SharedIoThread, locals: &mut IoThreadLocals) -> bool {
        if self.io_thread_state.load() == IoThreadState::Stopped {
            return true;
        }
        let mut send = lock(&self.send);
        if !self.send_failed.load(Ordering::Acquire) && !send.backlog.is_empty() {
            let mut sent_message_count = 0;
            let progress = send_frames(
                self.socket,
                &mut send.backlog,
                MAX_BYTES_SENT_PER_TURN,
                &mut sent_message_count,
            );
            send.backlog_message_count -= sent_message_count;
            if let SendProgress::Failed(error) = progress {
                let unsendable = self.fail_sending(&mut send, &error);
                locals.keep_unsendable(unsendable);
            }
            if send.backlog.is_empty() {
                // NB: A queue that has drained keeps its capacity, which this lets go of.
                send.backlog = VecDeque::new();
                self.sent.notify_all();
            }
        }
        let wants_writable = !send.backlog.is_empty();
        if wants_writable != send.writable_interest {
            if let Err(error) = io_thread
                .poller
                .set_writable_interest(self.socket, self.poller_token, wants_writable)
            {
                eprintln!("TransportSocket: Unable to wait for room in the socket: {error}");
                self.io_thread_state.store(IoThreadState::Stopped);
                return true;
            }
            send.writable_interest = wants_writable;
        }
        // NB: Posting threads check the state under the send lock, so nothing is left behind in the backlog.
        if send.backlog.is_empty() && self.io_thread_state.load() == IoThreadState::SendPendingMessagesAndStop {
            self.io_thread_state.store(IoThreadState::Stopped);
            return true;
        }
        false
    }

    // NB: Runs on the IO thread as IO is over, after the socket has left the poller.
    fn finish_io(&self) -> VecDeque<Frame<'static>> {
        let unsendable = {
            // NB: Taken either way, so that a flush() that saw IO running is waiting by the time it is woken.
            let mut send = lock(&self.send);
            if self.stop_was_requested_by_owner.load(Ordering::Acquire) {
                VecDeque::new()
            } else {
                self.abandon_backlog(&mut send)
            }
        };
        self.sent.notify_all();
        // NB: Stopping for transfer tears down this end on purpose, and what it read of an incomplete message goes to
        //     the new owner, so that is not the peer going away.
        let is_being_transferred = self.is_being_transferred.load(Ordering::Acquire);
        if !is_being_transferred {
            lock(&self.receive).discard_incomplete_frames();
        }
        self.inbox.end_local_io();
        if !is_being_transferred {
            self.inbox.mark_peer_eof();
        }
        unsendable
    }
}

/// The IO thread of every socket transport in the process. It waits on all of their sockets at once, and finds the
/// transport an event is for by the token it registered the socket with.
struct SharedIoThread {
    poller: Poller,
    attention: AttentionList<Shared>,
    registrations: Mutex<HashMap<u64, Arc<Shared>>>,
    next_token: AtomicU64,
}

static IO_THREAD: OnceLock<Arc<SharedIoThread>> = OnceLock::new();

// NB: Every transport starts the thread as it is made, before anything reaches it through here.
fn shared_io_thread() -> &'static SharedIoThread {
    IO_THREAD
        .get()
        .expect("a transport starts the shared socket IO thread before it uses it")
}

fn start_socket_io_thread() -> io::Result<&'static SharedIoThread> {
    start_shared_io_thread(&IO_THREAD, "IPC IO (socket)", SharedIoThread::new, SharedIoThread::run)
}

struct IoThreadLocals {
    events: Vec<Event>,
    read_buffer: Vec<u8>,
    receiver: socket::Receiver,
    received_fds: Vec<OwnedFd>,
    messages: Vec<ReceivedMessage>,
    transports_to_serve: Vec<Arc<Shared>>,
    // NB: What transports whose IO stopped on its own left unsent, released a bounded part per turn. Each backlog stays
    //     in the deque it was detached in, so that taking it on allocates nothing in proportion to it.
    unsendable: VecDeque<VecDeque<Frame<'static>>>,
}

impl IoThreadLocals {
    fn keep_unsendable(&mut self, frames: VecDeque<Frame<'static>>) {
        if !frames.is_empty() {
            self.unsendable.push_back(frames);
        }
    }

    fn release_some_unsendable(&mut self) {
        let mut count = MAX_UNSENDABLE_FRAMES_RELEASED_PER_TURN;
        while count > 0
            && let Some(frames) = self.unsendable.front_mut()
        {
            let released = frames.len().min(count);
            frames.drain(..released);
            count -= released;
            if frames.is_empty() {
                self.unsendable.pop_front();
            }
        }
    }
}

impl SharedIoThread {
    fn new() -> io::Result<Self> {
        Ok(Self {
            poller: Poller::new()?,
            attention: AttentionList::new(),
            registrations: Mutex::new(HashMap::new()),
            next_token: AtomicU64::new(0),
        })
    }

    fn register(&self, transport: &Arc<Shared>) -> io::Result<()> {
        lock(&self.registrations).insert(transport.poller_token, Arc::clone(transport));
        if let Err(error) = self.poller.add(transport.socket, transport.poller_token) {
            lock(&self.registrations).remove(&transport.poller_token);
            return Err(error);
        }
        Ok(())
    }

    // NB: Has this thread write the transport's backlog, and stop its IO if asked to.
    fn request_attention(&self, transport: &Arc<Shared>) {
        if self.attention.request(transport) {
            self.poller.wake();
        }
    }

    fn transport_with_token(&self, token: u64) -> Option<Arc<Shared>> {
        lock(&self.registrations).get(&token).cloned()
    }

    fn run(&self) {
        let mut locals = IoThreadLocals {
            events: Vec::new(),
            read_buffer: vec![0; READ_BUFFER_SIZE],
            receiver: socket::Receiver::new(),
            received_fds: Vec::new(),
            messages: Vec::new(),
            transports_to_serve: Vec::new(),
            unsendable: VecDeque::new(),
        };
        let mut released_memory_since_last_wakeup = true;
        loop {
            self.serve_transports_needing_attention(&mut locals);
            locals.release_some_unsendable();

            // NB: Work that one turn did not get through goes on in the next, once this thread has taken in what came
            //     meanwhile.
            let has_more_to_do = !self.attention.is_empty() || !locals.unsendable.is_empty();
            let timeout = if has_more_to_do {
                Some(Duration::ZERO)
            } else if released_memory_since_last_wakeup {
                None
            } else {
                Some(Duration::from_millis(IDLE_MEMORY_RELEASE_DELAY_IN_MILLISECONDS.into()))
            };
            let mut events = std::mem::take(&mut locals.events);
            let woken = match self.poller.wait(&mut events, timeout) {
                Ok(woken) => woken,
                Err(error) => {
                    // NB: Waiting again soon is all this thread can do, without giving up on every transport at once.
                    eprintln!("TransportSocket: Waiting for the sockets failed: {error}");
                    std::thread::sleep(Duration::from_millis(1));
                    true
                }
            };
            if woken {
                released_memory_since_last_wakeup = false;
            } else if !has_more_to_do {
                release_unused_allocator_memory();
                released_memory_since_last_wakeup = true;
            }
            for event in &events {
                let Some(transport) = self.transport_with_token(event.token) else {
                    continue;
                };
                if event.readable && self.read(&transport, &mut locals, event.hung_up) {
                    continue;
                }
                if event.writable {
                    self.serve(&transport, &mut locals);
                }
            }
            locals.events = events;
        }
    }

    fn serve_transports_needing_attention(&self, locals: &mut IoThreadLocals) {
        let mut transports_to_serve = std::mem::take(&mut locals.transports_to_serve);
        self.attention.take(&mut transports_to_serve);
        for transport in transports_to_serve.drain(..) {
            self.serve(&transport, locals);
        }
        locals.transports_to_serve = transports_to_serve;
    }

    // NB: Answers whether the transport's IO is over.
    fn read(&self, transport: &Arc<Shared>, locals: &mut IoThreadLocals, hung_up: bool) -> bool {
        let turn = transport.read_turn(locals, hung_up);
        if turn.io_is_over {
            self.finish(transport, locals);
            return true;
        }
        // NB: A receive barrier waits for the socket to be found drained, which a turn that ran out of reads did not
        //     get to, and no event may come to read again.
        if !turn.found_drained && transport.barriers_waiting.load(Ordering::Acquire) > 0 {
            self.attention.request(transport);
        }
        false
    }

    fn serve(&self, transport: &Arc<Shared>, locals: &mut IoThreadLocals) {
        if transport.barriers_waiting.load(Ordering::Acquire) > 0 && self.read(transport, locals, false) {
            return;
        }
        if transport.do_pending_io(self, locals) {
            self.finish(transport, locals);
        }
    }

    fn finish(&self, transport: &Arc<Shared>, locals: &mut IoThreadLocals) {
        if lock(&self.registrations).remove(&transport.poller_token).is_none() {
            return;
        }
        // NB: Once the socket has left the poller, nothing more comes in for the transport. A failure means the socket
        //     is gone from it already.
        let _ = self.poller.remove(transport.socket);
        let unsendable = self.end_io(transport);
        locals.keep_unsendable(unsendable);
    }

    // NB: Gives back what was left unsent, for the caller to release. The owner may close the socket or hand it over as
    //     soon as this returns.
    fn end_io(&self, transport: &Arc<Shared>) -> VecDeque<Frame<'static>> {
        transport.io_thread_state.store(IoThreadState::Stopped);
        let unsendable = transport.finish_io();
        self.attention.mark_io_finished(transport);
        unsendable
    }
}

pub struct TransportSocket {
    shared: Arc<Shared>,
    // NB: Owned here rather than by the IO thread, which only uses it until IO is over, so that it can go to a new
    //     owner.
    socket: Mutex<Option<OwnedFd>>,
}

impl TransportSocket {
    /// A transport for this end, and the handle that makes the other end's transport.
    pub fn create_paired() -> io::Result<(Self, SocketHandle)> {
        let (ours, theirs) = UnixStream::pair()?;
        Ok((Self::new(ours.into())?, theirs.into()))
    }

    pub fn new(socket: OwnedFd) -> io::Result<Self> {
        Self::from_handle(SocketHandle::new(socket))
    }

    pub fn from_handle(handle: SocketHandle) -> io::Result<Self> {
        Self::with_read_notification_pipe(handle, None)
    }

    /// A transport that writes a byte to the pipe when messages are there to read, or the peer is gone, and nothing
    /// more until `acknowledge_read_notification()`. It has the pipe from the start, so that what arrives before
    /// anything watches the pipe still leaves a byte in it.
    pub fn with_read_notification_pipe(
        handle: SocketHandle,
        read_notification_pipe: Option<OwnedFd>,
    ) -> io::Result<Self> {
        Self::with_limits(
            handle,
            read_notification_pipe,
            MAX_UNPROCESSED_BUFFER_SIZE,
            MAX_UNPROCESSED_FDS,
        )
    }

    fn with_limits(
        handle: SocketHandle,
        read_notification_pipe: Option<OwnedFd>,
        byte_limit: usize,
        fd_limit: usize,
    ) -> io::Result<Self> {
        let io_thread = start_socket_io_thread()?;
        let SocketHandle {
            socket,
            unprocessed_bytes,
            unprocessed_attachments,
        } = handle;
        socket::set_buffer_sizes(socket.as_raw_fd(), SOCKET_BUFFER_SIZE);

        // NB: What the last owner read without seeing the end of a message comes first.
        let mut receive = ReceiveState {
            unprocessed_bytes: Vec::new(),
            unprocessed_fds: unprocessed_attachments
                .into_iter()
                .map(Attachment::into_fd)
                .collect::<io::Result<_>>()?,
            bytes_taken_from_socket: 0,
            times_found_drained: 0,
            byte_limit,
            fd_limit,
        };
        let mut messages = Vec::new();
        // NB: The last owner parsed what it read as it read it, and a well-behaved peer's descriptors arrive with the
        //     first byte of the write whose frames claim them. So, it hands over no more than the start of one frame.
        let malformed_handover = if unprocessed_bytes.len() > byte_limit.min(HEADER_SIZE + MAX_INLINE_PAYLOAD_SIZE) {
            Some("amount of incomplete frames")
        } else if receive.unprocessed_fds.len() > fd_limit {
            Some("number of unclaimed descriptors")
        } else {
            receive
                .take_in(&unprocessed_bytes, &mut Vec::new(), &mut messages)
                .err()
        };
        let inbox = Inbox::new(read_notification_pipe);
        inbox.deliver(messages, |_| {});

        let shared = Arc::new(Shared {
            socket: socket.as_raw_fd(),
            poller_token: io_thread.next_token.fetch_add(1, Ordering::Relaxed),
            io_thread_state: AtomicIoThreadState::new(),
            is_being_transferred: AtomicBool::new(false),
            send_failed: AtomicBool::new(false),
            stop_was_requested_by_owner: AtomicBool::new(false),
            io_thread_membership: IoThreadMembership::default(),
            barriers_waiting: AtomicUsize::new(0),
            send: Mutex::new(SendState {
                backlog: VecDeque::new(),
                backlog_message_count: 0,
                writable_interest: false,
            }),
            sent: Condvar::new(),
            receive: Mutex::new(receive),
            inbox,
        });
        // NB: What the last owner left unread is from the peer, and a malformed part of it ends the messages, as it would
        //     have had the last owner read on. A descriptor the poller refuses, as one that is no socket, can't carry
        //     any. Either way, the transport starts with its IO over, which tells the owner that the peer is gone.
        let refusal = match malformed_handover {
            Some(what) => Some(format!("a malformed {what} handed over with the socket")),
            None => io_thread
                .register(&shared)
                .err()
                .map(|error| format!("a socket it can't wait on ({error})")),
        };
        if let Some(refusal) = refusal {
            eprintln!("TransportSocket: Refusing {refusal}");
            socket::shutdown(socket.as_raw_fd(), Shutdown::Both);
            drop(io_thread.end_io(&shared));
        }
        Ok(Self {
            shared,
            socket: Mutex::new(Some(socket)),
        })
    }

    /// Sends a message, or queues it behind earlier ones that wait for room in the socket. Refuses it once a stop is
    /// asked for or a send has failed, and refuses one with more attachments than a peer takes.
    pub fn post_message(&self, bytes: Vec<u8>, attachments: Vec<Attachment>) -> io::Result<()> {
        crate::validate_message_size(bytes.len(), attachments.len())?;
        self.post_message_with_any_number_of_attachments(OutgoingPayload::Owned(bytes), attachments)
    }

    /// As `post_message()`, but leaves it to the peer to refuse a message with too many attachments, as
    /// `IPC::Transport` does. Lent bytes are only handed over if the message has to wait behind others.
    pub fn post_message_with_any_number_of_attachments(
        &self,
        payload: OutgoingPayload<'_>,
        attachments: Vec<Attachment>,
    ) -> io::Result<()> {
        // NB: A frame's header can't give a larger size.
        crate::validate_message_size(payload.bytes().len(), 0)?;
        // NB: Checked again as the message goes out. A stopped transport refuses it before its payload is copied into
        //     shared memory.
        if !self.is_open() {
            return Err(io::ErrorKind::NotConnected.into());
        }
        let (payload, mut hand_over) = match payload {
            OutgoingPayload::Owned(bytes) => (FramePayload::Owned(Box::new(bytes)), None),
            OutgoingPayload::Lent { bytes, hand_over } => (FramePayload::Lent(bytes), Some(hand_over)),
        };
        let mut frames = frames_of_message(payload, attachments)?;
        let mut should_wake_io_thread = false;
        let mut unsendable = VecDeque::new();
        {
            let mut send = lock(&self.shared.send);
            // NB: Once a stop is asked for, nothing new comes in, so that a producer can't keep a graceful close or a
            //     transfer from finishing.
            if !self.is_open() {
                return Err(io::ErrorKind::NotConnected.into());
            }
            if send.backlog.is_empty() {
                match send_frames(self.shared.socket, &mut frames, usize::MAX, &mut 0) {
                    SendProgress::Drained => {}
                    SendProgress::Incomplete => should_wake_io_thread = true,
                    SendProgress::Failed(error) => unsendable = self.shared.fail_sending(&mut send, &error),
                }
            }
            if !frames.is_empty() && !self.shared.send_failed.load(Ordering::Acquire) {
                let mut hand_over_payload = || -> OwnedPayload {
                    let hand_over = hand_over
                        .as_mut()
                        .expect("a lent payload comes with a way to hand it over");
                    hand_over()
                };
                send.backlog
                    .extend(frames.drain(..).map(|frame| frame.into_owned(&mut hand_over_payload)));
                send.backlog_message_count += 1;
            }
        }
        drop(frames);
        drop(unsendable);
        if should_wake_io_thread {
            shared_io_thread().request_attention(&self.shared);
        }
        Ok(())
    }

    /// How many posted messages wait for room in the socket, so a producer can tell when its peer falls behind.
    pub fn pending_message_count(&self) -> usize {
        lock(&self.shared.send).backlog_message_count
    }

    /// Waits until everything posted so far has been written to the socket.
    pub fn flush(&self) {
        let mut send = lock(&self.shared.send);
        while self.shared.io_thread_state.load() != IoThreadState::Stopped
            && !self.shared.send_failed.load(Ordering::Acquire)
            && !send.backlog.is_empty()
        {
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
        self.shared.io_thread_state.load() == IoThreadState::Running
            && !self.shared.send_failed.load(Ordering::Acquire)
            && !self.shared.inbox.peer_eof()
    }

    /// Waits for the next message, or returns `None` once the peer is gone and every message it sent was taken.
    pub fn receive(&self) -> Option<ReceivedMessage> {
        self.shared.inbox.receive()
    }

    /// Takes every message that has arrived, and whether the peer was gone as they were taken. Later arrivals can't
    /// be skipped by reporting the peer gone.
    pub fn take_incoming(&self) -> (Vec<ReceivedMessage>, bool) {
        self.shared.inbox.take_incoming()
    }

    /// Waits until there is a message to take, the peer is gone, the wait is interrupted, or the deadline passes.
    pub fn wait_until_readable(&self, deadline: Option<MonotonicDeadline>) {
        self.shared.inbox.wait_until_readable(deadline);
    }

    /// Makes `wait_until_readable()` return, even with nothing to read, now or if none is under way, the next time.
    /// Any thread may call this.
    pub fn interrupt_wait_until_readable(&self) {
        self.shared.inbox.interrupt_wait_until_readable();
    }

    /// Waits until everything the peer had written to the socket when this was called has been taken in. The end of a
    /// frame that has yet to come does not hold it up.
    pub fn wait_until_incoming_is_current(&self) {
        assert!(
            !is_shared_io_thread(),
            "the shared IO thread must not wait for a transport's reads"
        );
        let socket = lock(&self.socket);
        if socket.is_none() {
            return;
        }
        let (target, times_found_drained) = {
            let receive = lock(&self.shared.receive);
            let readable = socket::readable_byte_count(self.shared.socket) as u64;
            (receive.bytes_taken_from_socket + readable, receive.times_found_drained)
        };
        drop(socket);
        let barrier_is_reached = |incoming: &Incoming<ReceiveBarrier>| {
            incoming.barrier.bytes_published >= target
                || incoming.barrier.times_found_drained > times_found_drained
                || incoming.local_io_is_over
                || self.shared.inbox.peer_eof()
        };
        if barrier_is_reached(&self.shared.inbox.lock()) {
            return;
        }
        self.shared.barriers_waiting.fetch_add(1, Ordering::AcqRel);
        shared_io_thread().request_attention(&self.shared);
        let mut incoming = self.shared.inbox.lock();
        while !barrier_is_reached(&incoming) {
            incoming = self.shared.inbox.wait(incoming);
        }
        drop(incoming);
        self.shared.barriers_waiting.fetch_sub(1, Ordering::AcqRel);
    }

    /// Sets what the IO thread shows every message that arrives, before the owner gets to see it, and empty bytes once
    /// the peer is gone. It runs while the IO thread holds a lock, so it should only have a look. Once this returns,
    /// the observer it gives back is no longer running.
    pub fn set_incoming_message_observer(
        &self,
        observer: Option<IncomingMessageObserver>,
    ) -> Option<IncomingMessageObserver> {
        self.shared.inbox.set_incoming_message_observer(observer)
    }

    /// Lets the transport write to its read-notification pipe again, once the owner has drained it.
    pub fn acknowledge_read_notification(&self) {
        self.shared.inbox.acknowledge_read_notification();
    }

    /// Writes to the read-notification pipe if there is something to read, or the peer is gone, and no byte is
    /// already pending.
    pub fn schedule_read_notification_if_readable(&self) {
        self.shared.inbox.schedule_read_notification_if_readable();
    }

    /// Sends what is queued, then closes this end, which tells the peer that it is gone.
    pub fn close_after_sending_all_pending_messages(&self) {
        self.stop(IoThreadState::SendPendingMessagesAndStop);
        self.shut_down_socket();
    }

    pub fn close(&self) {
        self.stop(IoThreadState::Stopped);
        self.shut_down_socket();
    }

    /// Sends what is queued, stops, and hands the socket over for another transport to take this end's place, along
    /// with what was read of a message that has yet to arrive whole. Messages that arrived whole stay here to take.
    pub fn release_for_transfer(&self) -> io::Result<SocketHandle> {
        self.shared.is_being_transferred.store(true, Ordering::Release);
        self.stop(IoThreadState::SendPendingMessagesAndStop);

        let socket = self.take_socket().ok_or(io::ErrorKind::NotConnected)?;
        let mut receive = lock(&self.shared.receive);
        Ok(SocketHandle {
            socket,
            unprocessed_bytes: std::mem::take(&mut receive.unprocessed_bytes),
            unprocessed_attachments: receive.unprocessed_fds.drain(..).map(Attachment::from).collect(),
        })
    }

    // NB: A posting thread writes to the socket under the send lock, after it has checked that IO still runs. So, once
    //     IO has stopped, nothing writes to the socket after this has taken the lock.
    fn take_socket(&self) -> Option<OwnedFd> {
        let _send = lock(&self.shared.send);
        lock(&self.socket).take()
    }

    // NB: The peer hears that this end is gone even if something else still has a descriptor of the socket.
    fn shut_down_socket(&self) {
        if let Some(socket) = self.take_socket() {
            socket::shutdown(socket.as_raw_fd(), Shutdown::Both);
        }
    }

    fn stop(&self, desired_state: IoThreadState) {
        self.shared.stop_was_requested_by_owner.store(true, Ordering::Release);
        self.shared.io_thread_state.request_stop(desired_state);
        let io_thread = shared_io_thread();
        io_thread.request_attention(&self.shared);
        io_thread.attention.wait_until_io_is_finished(&self.shared);

        // NB: What was left unsent goes here rather than on the IO thread, which all transports share, as closing the
        //     descriptors of a long backlog takes a while.
        let backlog = self.shared.abandon_backlog(&mut lock(&self.shared.send));
        drop(backlog);
    }
}

impl Drop for TransportSocket {
    fn drop(&mut self) {
        self.stop(IoThreadState::Stopped);
        self.shut_down_socket();
        // NB: The IO thread may still hold the shared state for a moment, and this end's descriptors go with this.
        self.shared.inbox.close_read_notification_pipe();
        // NB: The observer goes here, on the owner's thread, rather than wherever the last reference to the shared state
        //     happens to be dropped.
        drop(self.set_incoming_message_observer(None));
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::{Read, Write};
    use std::sync::atomic::AtomicUsize;
    use std::time::Instant;

    fn paired() -> (TransportSocket, TransportSocket) {
        let (a, handle) = TransportSocket::create_paired().unwrap();
        (a, TransportSocket::from_handle(handle).unwrap())
    }

    // NB: The test plays the peer on the other end of the socket, writing frames of its own making.
    fn with_raw_peer() -> (TransportSocket, UnixStream) {
        let (ours, peer) = UnixStream::pair().unwrap();
        (TransportSocket::new(ours.into()).unwrap(), peer)
    }

    fn until<T>(mut attempt: impl FnMut() -> Option<T>) -> T {
        let deadline = Instant::now() + Duration::from_secs(20);
        loop {
            if let Some(result) = attempt() {
                return result;
            }
            assert!(Instant::now() < deadline, "timed out");
            std::thread::sleep(Duration::from_millis(1));
        }
    }

    fn write_with_fds(peer: &UnixStream, bytes: &[u8], fds: &[BorrowedFd<'_>]) {
        let mut remaining = bytes;
        let mut fds = fds;
        while !remaining.is_empty() {
            match socket::send_with_fds(peer.as_raw_fd(), &[IoSlice::new(remaining)], fds) {
                Ok(written) => {
                    remaining = &remaining[written..];
                    fds = &[];
                }
                Err(error) if error.kind() == io::ErrorKind::WouldBlock => std::thread::sleep(Duration::from_millis(1)),
                Err(error) => panic!("{error}"),
            }
        }
    }

    fn frame(frame_type: u8, payload: &[u8], fd_count: usize) -> Vec<u8> {
        let mut bytes = frame_header(frame_type, payload.len(), fd_count).to_vec();
        bytes.extend_from_slice(payload);
        bytes
    }

    fn read_on_peer(peer: &UnixStream, receiver: &mut socket::Receiver, fds: &mut Vec<OwnedFd>) -> Vec<u8> {
        let mut buffer = vec![0; READ_BUFFER_SIZE];
        let read = until(|| match receiver.receive_with_fds(peer.as_raw_fd(), &mut buffer, fds) {
            Ok(read) => Some(read),
            Err(error) if error.kind() == io::ErrorKind::WouldBlock => None,
            Err(error) => panic!("{error}"),
        });
        buffer.truncate(read);
        buffer
    }

    fn pipe_holding(byte: u8) -> OwnedFd {
        let (reader, mut writer) = std::io::pipe().unwrap();
        writer.write_all(&[byte]).unwrap();
        reader.into()
    }

    fn byte_in(fd: OwnedFd) -> u8 {
        let mut byte = [0];
        std::fs::File::from(fd).read_exact(&mut byte).unwrap();
        byte[0]
    }

    fn assert_pipe_closed(reader: std::io::PipeReader) {
        let mut bytes = Vec::new();
        std::fs::File::from(OwnedFd::from(reader))
            .read_to_end(&mut bytes)
            .unwrap();
        assert!(bytes.is_empty());
    }

    fn assert_peer_sees_end(peer: &mut UnixStream) {
        peer.set_nonblocking(false).unwrap();
        let mut buffer = [0; 4096];
        while peer.read(&mut buffer).unwrap() != 0 {}
    }

    #[test]
    fn shared_memory_frames_claim_the_last_descriptor_and_leave_following_frames() {
        let payload = vec![7; MAX_INLINE_PAYLOAD_SIZE + 1];
        let memory = socket::copy_payload_to_file(&payload).unwrap();
        let (reader, writer) = std::io::pipe().unwrap();
        let mut fds = VecDeque::from([OwnedFd::from(writer), OwnedFd::from(memory)]);
        let mut frames = frame_header(SHARED_MEMORY_PAYLOAD_FRAME, payload.len(), 2).to_vec();
        frames.extend_from_slice(&frame(PAYLOAD_FRAME, &[9], 0));
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
        let size = MAX_INLINE_PAYLOAD_SIZE + 2;
        let frames = frame_header(SHARED_MEMORY_PAYLOAD_FRAME, size, 0).to_vec();
        assert!(parse_frames(&frames, &mut VecDeque::new(), &mut Vec::new()).is_err());
        let memory = socket::copy_payload_to_file(&vec![1; size - 1]).unwrap();
        let frames = frame_header(SHARED_MEMORY_PAYLOAD_FRAME, size, 1).to_vec();
        let mut fds = VecDeque::from([OwnedFd::from(memory)]);
        assert!(parse_frames(&frames, &mut fds, &mut Vec::new()).is_err());
        assert!(fds.is_empty());
    }

    #[test]
    fn shared_memory_frames_refuse_payloads_that_fit_a_frame() {
        for size in [0, 1, MAX_INLINE_PAYLOAD_SIZE] {
            let memory = socket::copy_payload_to_file(&vec![1; size.max(1)]).unwrap();
            let frames = frame_header(SHARED_MEMORY_PAYLOAD_FRAME, size, 1).to_vec();
            let mut fds = VecDeque::from([OwnedFd::from(memory)]);
            assert!(parse_frames(&frames, &mut fds, &mut Vec::new()).is_err());
        }
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
        let attachments = (0..300).map(|i| Attachment::from(pipe_holding(i as u8))).collect();
        a.post_message(vec![9], attachments).unwrap();
        let message = b.receive().unwrap();
        assert_eq!(message.bytes(), [9]);
        assert_eq!(message.attachments.len(), 300);
        for (i, attachment) in message.attachments.into_iter().enumerate() {
            assert_eq!(byte_in(attachment.into_fd().unwrap()), i as u8);
        }
    }

    #[test]
    fn descriptors_that_do_not_fit_one_sendmsg_go_ahead_in_writes_of_their_own() {
        let (transport, peer) = with_raw_peer();
        let fd_count = 2 * MAX_TRANSFER_FDS + 5;
        let attachments = (0..fd_count).map(|_| Attachment::from(pipe_holding(0))).collect();
        transport.post_message(vec![b'A'], attachments).unwrap();
        let mut receiver = socket::Receiver::new();
        for _ in 0..2 {
            let mut fds = Vec::new();
            let bytes = read_on_peer(&peer, &mut receiver, &mut fds);
            assert_eq!(bytes, frame_header(ATTACHMENTS_FRAME, 0, MAX_TRANSFER_FDS));
            assert_eq!(fds.len(), MAX_TRANSFER_FDS);
        }
        let mut fds = Vec::new();
        let bytes = read_on_peer(&peer, &mut receiver, &mut fds);
        assert_eq!(bytes, frame(PAYLOAD_FRAME, b"A", fd_count));
        assert_eq!(fds.len(), 5);
    }

    #[test]
    fn frames_share_a_write_unless_the_bytes_would_go_ahead_of_their_descriptors() {
        let (ours, peer) = UnixStream::pair().unwrap();
        let mut frames = VecDeque::new();
        for (byte, fd_count) in [(b'A', MAX_TRANSFER_FDS), (b'B', 1), (b'C', 0), (b'D', 0)] {
            let fds = (0..fd_count).map(|_| Attachment::from(pipe_holding(0))).collect();
            frames.extend(frames_of_message(FramePayload::Owned(Box::new(vec![byte])), fds).unwrap());
        }
        assert!(matches!(
            send_frames(ours.as_raw_fd(), &mut frames, usize::MAX, &mut 0),
            SendProgress::Drained
        ));
        let mut receiver = socket::Receiver::new();
        let mut fds = Vec::new();
        assert_eq!(
            read_on_peer(&peer, &mut receiver, &mut fds),
            frame(PAYLOAD_FRAME, b"A", MAX_TRANSFER_FDS)
        );
        assert_eq!(fds.len(), MAX_TRANSFER_FDS);
        fds.clear();
        let mut expected = frame(PAYLOAD_FRAME, b"B", 1);
        expected.extend(frame(PAYLOAD_FRAME, b"C", 0));
        expected.extend(frame(PAYLOAD_FRAME, b"D", 0));
        assert_eq!(read_on_peer(&peer, &mut receiver, &mut fds), expected);
        assert_eq!(fds.len(), 1);
    }

    #[test]
    fn a_write_offers_no_more_than_the_budget() {
        let (ours, mut peer) = UnixStream::pair().unwrap();
        let mut frames = VecDeque::new();
        for byte in 0..3 {
            frames.extend(frames_of_message(FramePayload::Owned(Box::new(vec![byte; 600])), Vec::new()).unwrap());
        }
        assert!(matches!(
            send_frames(ours.as_raw_fd(), &mut frames, 1000, &mut 0),
            SendProgress::Incomplete
        ));
        drop(ours);
        let mut written = Vec::new();
        peer.read_to_end(&mut written).unwrap();
        assert_eq!(written.len(), 1000);
    }

    #[test]
    fn a_partly_sent_frame_goes_on_without_its_descriptors() {
        let (ours, peer) = UnixStream::pair().unwrap();
        let mut frames = VecDeque::new();
        for byte in *b"ab" {
            let attachments = vec![Attachment::from(pipe_holding(byte))];
            frames.extend(frames_of_message(FramePayload::Owned(Box::new(vec![byte])), attachments).unwrap());
        }
        assert!(matches!(
            send_frames(ours.as_raw_fd(), &mut frames, 1, &mut 0),
            SendProgress::Incomplete
        ));
        let mut sent_message_count = 0;
        assert!(matches!(
            send_frames(ours.as_raw_fd(), &mut frames, usize::MAX, &mut sent_message_count),
            SendProgress::Drained
        ));
        assert_eq!(sent_message_count, 2);

        let expected = [frame(PAYLOAD_FRAME, b"a", 1), frame(PAYLOAD_FRAME, b"b", 1)].concat();
        let mut receiver = socket::Receiver::new();
        let mut bytes = Vec::new();
        let mut fds = Vec::new();
        while bytes.len() < expected.len() {
            bytes.extend(read_on_peer(&peer, &mut receiver, &mut fds));
        }
        assert_eq!(bytes, expected);
        assert_eq!(fds.into_iter().map(byte_in).collect::<Vec<_>>(), [b'a', b'b']);
    }

    #[test]
    fn a_sent_descriptor_is_closed_once_written() {
        let (a, b) = paired();
        let (reader, writer) = std::io::pipe().unwrap();
        a.post_message(vec![1], vec![Attachment::from(OwnedFd::from(writer))])
            .unwrap();
        let message = b.receive().unwrap();
        drop(message);
        assert_pipe_closed(reader);
    }

    #[test]
    fn the_peer_closing_ends_the_messages() {
        let (a, b) = paired();
        a.post_message(vec![7], Vec::new()).unwrap();
        a.close_after_sending_all_pending_messages();
        assert_eq!(b.receive().unwrap().bytes(), [7]);
        assert!(b.receive().is_none());
        assert!(!b.is_open());
        b.close();
        assert!(!b.is_open());
    }

    #[test]
    fn closing_tells_the_peer_at_once() {
        let (transport, mut peer) = with_raw_peer();
        transport.close();
        assert_peer_sees_end(&mut peer);
        assert_eq!(
            transport.post_message(vec![1], Vec::new()).unwrap_err().kind(),
            io::ErrorKind::NotConnected
        );
    }

    #[test]
    fn closing_tells_the_peer_even_while_another_descriptor_of_the_socket_is_open() {
        let (ours, mut peer) = UnixStream::pair().unwrap();
        let duplicate = ours.try_clone().unwrap();
        let transport = TransportSocket::new(ours.into()).unwrap();
        transport.close();
        assert_peer_sees_end(&mut peer);
        drop(duplicate);
    }

    #[test]
    fn a_malformed_handover_ends_the_messages_after_the_whole_ones() {
        let (ours, mut peer) = UnixStream::pair().unwrap();
        (&peer).write_all(&frame(PAYLOAD_FRAME, &[6], 0)).unwrap();
        let mut unprocessed_bytes = frame(PAYLOAD_FRAME, &[5], 0);
        unprocessed_bytes.extend_from_slice(&frame_header(7, 0, 0));
        let transport = TransportSocket::from_handle(SocketHandle {
            socket: ours.into(),
            unprocessed_bytes,
            unprocessed_attachments: Vec::new(),
        })
        .unwrap();
        assert_eq!(transport.receive().unwrap().bytes(), [5]);
        assert!(transport.receive().is_none());
        assert!(!transport.is_open());
        assert_peer_sees_end(&mut peer);
    }

    #[test]
    fn posts_are_refused_once_the_owner_has_seen_the_peer_go() {
        for _ in 0..100 {
            let (transport, peer) = with_raw_peer();
            peer.shutdown(Shutdown::Write).unwrap();
            transport.wait_until_readable(None);
            assert!(transport.take_incoming().1);
            assert_eq!(
                transport.post_message(vec![1], Vec::new()).unwrap_err().kind(),
                io::ErrorKind::NotConnected
            );
        }
    }

    #[test]
    fn the_last_messages_arrive_together_with_the_departure_of_the_peer() {
        for _ in 0..100 {
            let (ours, peer) = UnixStream::pair().unwrap();
            (&peer).write_all(&frame(PAYLOAD_FRAME, &[1], 0)).unwrap();
            drop(peer);
            let transport = TransportSocket::new(ours.into()).unwrap();
            transport.wait_until_readable(None);
            let (messages, peer_eof) = transport.take_incoming();
            assert_eq!(messages.len(), 1);
            assert!(peer_eof);
            assert!(!transport.is_open());
        }
    }

    #[test]
    fn a_handover_holds_no_more_than_the_start_of_one_frame() {
        let (ours, mut peer) = UnixStream::pair().unwrap();
        let frame_count = (HEADER_SIZE + MAX_INLINE_PAYLOAD_SIZE) / HEADER_SIZE + 1;
        let unprocessed_bytes = (0..frame_count).flat_map(|_| frame(PAYLOAD_FRAME, &[], 0)).collect();
        let transport = TransportSocket::from_handle(SocketHandle {
            socket: ours.into(),
            unprocessed_bytes,
            unprocessed_attachments: Vec::new(),
        })
        .unwrap();
        assert!(transport.receive().is_none());
        assert!(!transport.is_open());
        assert_peer_sees_end(&mut peer);
    }

    #[test]
    fn a_handover_beyond_the_limits_ends_the_messages_before_any_is_taken() {
        let whole_frames: Vec<u8> = (0..3).flat_map(|_| frame(PAYLOAD_FRAME, &[], 0)).collect();
        for (unprocessed_bytes, descriptor_count) in [(whole_frames, 0), (frame(PAYLOAD_FRAME, &[], 2), 2)] {
            let (ours, mut peer) = UnixStream::pair().unwrap();
            let (reader, writer) = std::io::pipe().unwrap();
            let writer = OwnedFd::from(writer);
            let unprocessed_attachments = (0..descriptor_count)
                .map(|_| Attachment::from(writer.try_clone().unwrap()))
                .collect();
            drop(writer);
            let handle = SocketHandle {
                socket: ours.into(),
                unprocessed_bytes,
                unprocessed_attachments,
            };
            let transport = TransportSocket::with_limits(handle, None, 2 * HEADER_SIZE, 1).unwrap();
            assert!(transport.receive().is_none());
            assert!(!transport.is_open());
            assert_peer_sees_end(&mut peer);
            assert_pipe_closed(reader);
        }
    }

    #[test]
    fn a_descriptor_that_is_no_socket_ends_the_messages() {
        let file = std::fs::File::open("/dev/null").unwrap();
        let transport = TransportSocket::new(file.into()).unwrap();
        assert!(transport.receive().is_none());
        assert!(!transport.is_open());
        assert_eq!(
            transport.post_message(vec![1], Vec::new()).unwrap_err().kind(),
            io::ErrorKind::NotConnected
        );
    }

    #[test]
    fn a_message_claiming_descriptors_that_did_not_come_ends_the_messages() {
        for fd_count in [1, MAX_MESSAGE_ATTACHMENT_COUNT + 1] {
            let (transport, mut peer) = with_raw_peer();
            peer.write_all(&frame_header(PAYLOAD_FRAME, 0, fd_count)).unwrap();
            assert!(transport.receive().is_none());
            assert!(!transport.is_open());
            assert_peer_sees_end(&mut peer);
        }
    }

    #[test]
    fn a_partly_sent_frame_left_behind_ends_what_this_end_writes() {
        let (transport, peer) = with_raw_peer();
        // NB: A frame larger than the socket's buffer goes in part.
        socket::set_buffer_sizes(transport.shared.socket, 4096);
        transport
            .post_message(vec![1; MAX_INLINE_PAYLOAD_SIZE], Vec::new())
            .unwrap();
        assert!(
            lock(&transport.shared.send)
                .backlog
                .front()
                .is_some_and(|frame| frame.sent_byte_count > 0)
        );
        peer.shutdown(Shutdown::Write).unwrap();
        until(|| (!transport.is_open()).then_some(()));
        let handle = transport.release_for_transfer().unwrap();
        // NB: The peer still reads, but anything written now would pass for the rest of the frame it is in the middle of.
        assert_eq!(
            socket::send_with_fds(handle.socket.as_raw_fd(), &[IoSlice::new(&[0])], &[])
                .unwrap_err()
                .kind(),
            io::ErrorKind::BrokenPipe
        );
        drop(peer);
    }

    #[test]
    fn a_message_left_behind_after_its_descriptors_went_ahead_ends_what_this_end_writes() {
        let (transport, peer) = with_raw_peer();
        let attachments = (0..=MAX_TRANSFER_FDS)
            .map(|_| Attachment::from(pipe_holding(0)))
            .collect();
        let mut frames = frames_of_message(FramePayload::Owned(Box::new(vec![1])), attachments).unwrap();
        // NB: The socket took the attachments frame, and had no room for the frame of the message itself.
        frames.pop_front();
        let mut send = lock(&transport.shared.send);
        send.backlog.extend(frames);
        drop(transport.shared.abandon_backlog(&mut send));
        drop(send);
        assert_eq!(
            socket::send_with_fds(transport.shared.socket, &[IoSlice::new(&[0])], &[])
                .unwrap_err()
                .kind(),
            io::ErrorKind::BrokenPipe
        );
        drop(peer);
    }

    #[test]
    fn malformed_frames_end_the_messages() {
        let (transport, mut peer) = with_raw_peer();
        peer.write_all(&frame_header(7, 0, 0)).unwrap();
        assert!(transport.receive().is_none());
        assert!(!transport.is_open());
        assert_peer_sees_end(&mut peer);
    }

    #[test]
    fn complete_messages_are_delivered_before_a_malformed_frame() {
        for malformed in [
            frame_header(7, 0, 0),
            frame_header(ATTACHMENTS_FRAME, 1, 0),
            frame_header(PAYLOAD_FRAME, MAX_INLINE_PAYLOAD_SIZE + 1, 0),
            frame_header(SHARED_MEMORY_PAYLOAD_FRAME, 2, 1),
        ] {
            let (transport, mut peer) = with_raw_peer();
            let (unclaimed_reader, unclaimed_writer) = std::io::pipe().unwrap();
            let memory = socket::copy_payload_to_file(&[1]).unwrap();
            let mut bytes = frame(PAYLOAD_FRAME, &[42], 1);
            bytes.extend_from_slice(&malformed);
            let attachment = pipe_holding(b'!');
            let unclaimed_writer = OwnedFd::from(unclaimed_writer);
            write_with_fds(
                &peer,
                &bytes,
                &[attachment.as_fd(), memory.as_fd(), unclaimed_writer.as_fd()],
            );
            drop((attachment, memory, unclaimed_writer));
            let message = transport.receive().unwrap();
            assert_eq!(message.bytes(), [42]);
            let mut attachments = message.attachments;
            assert_eq!(byte_in(attachments.remove(0).into_fd().unwrap()), b'!');
            assert!(transport.receive().is_none());
            assert_pipe_closed(unclaimed_reader);
            assert_peer_sees_end(&mut peer);
        }
    }

    #[cfg(target_os = "linux")]
    #[test]
    fn a_payload_file_the_peer_did_not_seal_ends_the_messages() {
        let (transport, mut peer) = with_raw_peer();
        let payload = vec![7; MAX_INLINE_PAYLOAD_SIZE + 1];
        let file = crate::shared_memory::create_unsealed_file_holding(&payload);
        write_with_fds(
            &peer,
            &frame_header(SHARED_MEMORY_PAYLOAD_FRAME, payload.len(), 1),
            &[file.as_fd()],
        );
        assert!(transport.receive().is_none());
        assert_peer_sees_end(&mut peer);
    }

    #[test]
    fn large_payloads_arrive_mapped_and_whole() {
        let (a, b) = paired();
        let bytes: Vec<u8> = (0..(MAX_INLINE_PAYLOAD_SIZE as u32 * 3))
            .map(|i| (i % 251) as u8)
            .collect();
        a.post_message(bytes.clone(), Vec::new()).unwrap();
        let message = b.receive().unwrap();
        #[cfg(target_os = "linux")]
        assert!(matches!(message.bytes, PayloadBytes::Mapped(_)));
        assert_eq!(message.bytes(), bytes);
    }

    #[test]
    fn a_failed_send_refuses_later_posts_and_keeps_what_arrived() {
        let (transport, mut peer) = with_raw_peer();
        peer.write_all(&frame(PAYLOAD_FRAME, &[7], 0)).unwrap();
        peer.shutdown(Shutdown::Read).unwrap();
        transport.post_message(vec![1], Vec::new()).unwrap();
        assert!(!transport.is_open());
        assert_eq!(
            transport.post_message(vec![2], Vec::new()).unwrap_err().kind(),
            io::ErrorKind::NotConnected
        );
        transport.flush();
        assert_eq!(transport.receive().unwrap().bytes(), [7]);
        peer.write_all(&frame(PAYLOAD_FRAME, &[8], 0)).unwrap();
        assert_eq!(transport.receive().unwrap().bytes(), [8]);
        drop(peer);
        assert!(transport.receive().is_none());
    }

    #[test]
    fn a_failed_send_wakes_an_owner_waiting_for_a_response() {
        let (transport, mut peer) = with_raw_peer();
        peer.shutdown(Shutdown::Read).unwrap();
        transport.post_message(vec![1], Vec::new()).unwrap();
        // NB: The peer keeps its end open, so only the failed send can end this wait.
        transport.wait_until_readable(None);
        assert!(transport.take_incoming().0.is_empty());
        peer.write_all(&frame(PAYLOAD_FRAME, &[2], 0)).unwrap();
        assert_eq!(transport.receive().unwrap().bytes(), [2]);
    }

    #[test]
    fn a_failed_send_wakes_an_owner_that_already_waits() {
        let (transport, peer) = with_raw_peer();
        let deadline =
            MonotonicDeadline::from_nanoseconds(crate::transport::clock_monotonic_nanoseconds() + 30 * 1_000_000_000);
        std::thread::scope(|scope| {
            let waiter = scope.spawn(|| transport.wait_until_readable(Some(deadline)));
            until(|| transport.shared.inbox.has_waiters().then_some(()));
            peer.shutdown(Shutdown::Read).unwrap();
            let _ = transport.post_message(vec![1], Vec::new());
            // NB: The peer keeps its end open, so only the failed send can end the wait before its deadline.
            until(|| waiter.is_finished().then_some(()));
        });
    }

    #[test]
    fn a_peer_closing_with_unread_data_still_delivers_its_last_messages() {
        let (transport, peer) = with_raw_peer();
        transport.post_message(vec![1; 1000], Vec::new()).unwrap();
        for byte in 0..10 {
            (&peer).write_all(&frame(PAYLOAD_FRAME, &[byte], 0)).unwrap();
        }
        drop(peer);
        for byte in 0..10 {
            assert_eq!(transport.receive().unwrap().bytes(), [byte]);
        }
        assert!(transport.receive().is_none());
    }

    #[test]
    fn a_peer_that_stops_reading_does_not_hold_up_the_others() {
        let (stuck, _peer_that_never_reads) = with_raw_peer();
        for _ in 0..64 {
            stuck.post_message(vec![0; 32 * 1024], Vec::new()).unwrap();
        }
        assert!(stuck.pending_message_count() > 0);
        let (a, b) = paired();
        for i in 0..1000u32 {
            a.post_message(i.to_ne_bytes().to_vec(), Vec::new()).unwrap();
            assert_eq!(b.receive().unwrap().bytes(), i.to_ne_bytes());
        }
        drop(stuck);
    }

    #[test]
    fn a_backlog_longer_than_one_turn_arrives_whole() {
        let (transport, mut peer) = with_raw_peer();
        let message_count = 4 * MAX_BYTES_SENT_PER_TURN / (32 * 1024);
        for i in 0..message_count {
            transport.post_message(vec![i as u8; 32 * 1024], Vec::new()).unwrap();
        }
        assert!(transport.pending_message_count() > 0);
        let flushed = std::thread::scope(|scope| {
            let flushing = scope.spawn(|| transport.flush());
            let mut frame_bytes = vec![0; HEADER_SIZE + 32 * 1024];
            for i in 0..message_count {
                peer.read_exact(&mut frame_bytes).unwrap();
                assert_eq!(frame_bytes[..HEADER_SIZE], frame_header(PAYLOAD_FRAME, 32 * 1024, 0));
                assert!(frame_bytes[HEADER_SIZE..].iter().all(|byte| *byte == i as u8));
            }
            flushing.join().is_ok()
        });
        assert!(flushed);
        assert_eq!(transport.pending_message_count(), 0);
    }

    #[test]
    fn transports_on_the_shared_thread_keep_to_themselves_through_teardown_and_transfer() {
        let churned = AtomicUsize::new(0);
        std::thread::scope(|scope| {
            scope.spawn(|| {
                for _ in 0..200 {
                    let (a, b) = paired();
                    a.post_message(vec![1], Vec::new()).unwrap();
                    if churned.fetch_add(1, Ordering::Relaxed).is_multiple_of(2) {
                        assert_eq!(b.receive().unwrap().bytes(), [1]);
                    }
                }
            });
            for pair in 0..8u32 {
                scope.spawn(move || {
                    let (a, b) = paired();
                    let sender = std::thread::spawn(move || {
                        for i in 0..2000u32 {
                            let mut bytes = pair.to_ne_bytes().to_vec();
                            bytes.extend(i.to_ne_bytes());
                            bytes.resize(8 + (i as usize * 37) % 700, 0);
                            a.post_message(bytes, Vec::new()).unwrap();
                        }
                        a.close_after_sending_all_pending_messages();
                    });
                    let mut reader = b;
                    let mut next = 0u32;
                    let mut check = |message: ReceivedMessage| {
                        let bytes = message.bytes();
                        assert_eq!(bytes[..4], pair.to_ne_bytes());
                        assert_eq!(bytes[4..8], next.to_ne_bytes());
                        next += 1;
                    };
                    loop {
                        reader.wait_until_readable(None);
                        let (messages, peer_is_gone) = reader.take_incoming();
                        messages.into_iter().for_each(&mut check);
                        if peer_is_gone {
                            break;
                        }
                        let handle = reader.release_for_transfer().unwrap();
                        let (messages, _) = reader.take_incoming();
                        messages.into_iter().for_each(&mut check);
                        reader = TransportSocket::from_handle(handle).unwrap();
                    }
                    assert_eq!(next, 2000);
                    sender.join().unwrap();
                });
            }
        });
    }

    #[test]
    fn the_receive_barrier_does_not_wait_for_an_incomplete_frame() {
        let (transport, mut peer) = with_raw_peer();
        let bytes = frame(PAYLOAD_FRAME, b"A", 0);
        peer.write_all(&bytes[..1]).unwrap();
        transport.wait_until_incoming_is_current();
        transport.wait_until_incoming_is_current();
        peer.write_all(&bytes[1..]).unwrap();
        transport.wait_until_incoming_is_current();
        let (messages, peer_is_gone) = transport.take_incoming();
        assert_eq!(messages.len(), 1);
        assert_eq!(messages[0].bytes(), b"A");
        assert!(!peer_is_gone);
    }

    #[test]
    fn the_receive_barrier_returns_once_a_read_has_ended_inside_a_write() {
        let (transport, peer) = with_raw_peer();
        // NB: One write, longer than one read takes.
        (&peer)
            .write_all(&frame(PAYLOAD_FRAME, &vec![3; MAX_INLINE_PAYLOAD_SIZE], 0))
            .unwrap();
        for _ in 0..10 {
            transport.wait_until_incoming_is_current();
        }
        assert_eq!(transport.receive().unwrap().bytes(), vec![3; MAX_INLINE_PAYLOAD_SIZE]);
    }

    #[test]
    fn the_receive_barrier_returns_under_a_peer_that_keeps_writing() {
        let (transport, mut peer) = with_raw_peer();
        let writing = AtomicBool::new(true);
        let frames_written = AtomicUsize::new(0);
        let barrier_outlasted_the_deadline = AtomicBool::new(false);
        // NB: Stops the writer even as a failed assertion unwinds, as the scope waits for it.
        struct StopWriting<'a>(&'a AtomicBool);
        impl Drop for StopWriting<'_> {
            fn drop(&mut self) {
                self.0.store(false, Ordering::Relaxed);
            }
        }
        std::thread::scope(|scope| {
            scope.spawn(|| {
                let bytes = frame(PAYLOAD_FRAME, &[0; 1000], 0);
                while writing.load(Ordering::Relaxed) {
                    peer.write_all(&bytes).unwrap();
                    frames_written.fetch_add(1, Ordering::Release);
                }
            });
            // NB: A barrier that waits for the socket to drain would only return once the writer stops.
            scope.spawn(|| {
                let deadline = Instant::now() + Duration::from_secs(20);
                while writing.load(Ordering::Relaxed) {
                    if Instant::now() >= deadline {
                        barrier_outlasted_the_deadline.store(true, Ordering::Relaxed);
                        writing.store(false, Ordering::Relaxed);
                    }
                    std::thread::sleep(Duration::from_millis(10));
                }
            });
            let stop_writing = StopWriting(&writing);
            let mut frames_received = 0;
            for _ in 0..50 {
                let written_before_barrier = until(|| {
                    let written = frames_written.load(Ordering::Acquire);
                    (written > frames_received).then_some(written)
                });
                transport.wait_until_incoming_is_current();
                assert!(!barrier_outlasted_the_deadline.load(Ordering::Relaxed));
                frames_received += transport.take_incoming().0.len();
                assert!(frames_received >= written_before_barrier);
            }
            // NB: The IO thread keeps taking in what the writer writes, so it finishes its last frame.
            drop(stop_writing);
        });
    }

    #[test]
    fn a_backlog_of_messages_with_descriptors_is_not_mistaken_for_a_flood() {
        let (ours, peer) = UnixStream::pair().unwrap();
        let batch_count = 6;
        let null = std::fs::File::open("/dev/null").unwrap();
        for _ in 0..batch_count {
            let bytes: Vec<u8> = (0..MAX_TRANSFER_FDS)
                .flat_map(|_| frame(PAYLOAD_FRAME, b"A", 1))
                .collect();
            write_with_fds(&peer, &bytes, &vec![null.as_fd(); MAX_TRANSFER_FDS]);
        }
        let transport = TransportSocket::new(ours.into()).unwrap();
        let mut received = 0;
        while received < batch_count * MAX_TRANSFER_FDS {
            let message = transport
                .receive()
                .expect("every message arrives before the peer is gone");
            assert_eq!(message.attachments.len(), 1);
            received += 1;
        }
        assert!(transport.is_open());
    }

    #[test]
    fn a_backlog_larger_than_one_read_turn_is_delivered_after_the_peer_hangs_up() {
        let (ours, peer) = UnixStream::pair().unwrap();
        // NB: A read ends with the descriptors of one write, so each of these frames takes a read of its own.
        let message_count = 3 * MAX_READS_PER_TURN;
        for i in 0..message_count {
            let descriptor = pipe_holding(i as u8);
            write_with_fds(&peer, &frame(PAYLOAD_FRAME, &[i as u8; 1024], 1), &[descriptor.as_fd()]);
        }
        drop(peer);
        let transport = TransportSocket::new(ours.into()).unwrap();
        for i in 0..message_count {
            let message = transport.receive().unwrap();
            assert_eq!(message.bytes(), [i as u8; 1024]);
            let mut attachments = message.attachments;
            assert_eq!(byte_in(attachments.remove(0).into_fd().unwrap()), i as u8);
        }
        assert!(transport.receive().is_none());
    }

    #[test]
    fn a_frame_cut_in_two_by_a_transfer_arrives_whole() {
        let (transport, mut peer) = with_raw_peer();
        let bytes = frame(PAYLOAD_FRAME, b"abcd", 0);
        peer.write_all(&bytes[..HEADER_SIZE + 2]).unwrap();
        transport.wait_until_incoming_is_current();
        let handle = transport.release_for_transfer().unwrap();
        assert_eq!(handle.unprocessed_bytes, bytes[..HEADER_SIZE + 2]);
        let new_owner = TransportSocket::from_handle(handle).unwrap();
        peer.write_all(&bytes[HEADER_SIZE + 2..]).unwrap();
        assert_eq!(new_owner.receive().unwrap().bytes(), b"abcd");
        assert!(transport.take_incoming().0.is_empty());
    }

    #[test]
    fn descriptors_ahead_of_their_message_go_along_with_a_transfer() {
        let (transport, peer) = with_raw_peer();
        let first = pipe_holding(b'a');
        write_with_fds(&peer, &frame_header(ATTACHMENTS_FRAME, 0, 1), &[first.as_fd()]);
        drop(first);
        transport.wait_until_incoming_is_current();
        let handle = transport.release_for_transfer().unwrap();
        assert_eq!(handle.unprocessed_attachments.len(), 1);
        let new_owner = TransportSocket::from_handle(handle).unwrap();
        let second = pipe_holding(b'b');
        write_with_fds(&peer, &frame(PAYLOAD_FRAME, b"X", 2), &[second.as_fd()]);
        drop(second);
        let message = new_owner.receive().unwrap();
        assert_eq!(message.bytes(), b"X");
        let tags: Vec<u8> = message
            .attachments
            .into_iter()
            .map(|attachment| byte_in(attachment.into_fd().unwrap()))
            .collect();
        assert_eq!(tags, b"ab");
    }

    #[test]
    fn a_read_notification_waits_for_its_acknowledgement() {
        let (ours, mut peer) = UnixStream::pair().unwrap();
        let (mut notifications, notification_writer) = UnixStream::pair().unwrap();
        notifications.set_nonblocking(true).unwrap();
        let transport =
            TransportSocket::with_read_notification_pipe(SocketHandle::from(ours), Some(notification_writer.into()))
                .unwrap();
        peer.write_all(&frame(PAYLOAD_FRAME, &[1], 0)).unwrap();
        let mut byte = [0];
        until(|| notifications.read(&mut byte).ok());
        // NB: The owner takes the message before its event loop acknowledges the notification.
        assert_eq!(transport.take_incoming().0.len(), 1);
        peer.write_all(&frame(PAYLOAD_FRAME, &[2], 0)).unwrap();
        until(|| (transport.shared.inbox.lock().messages.len() == 1).then_some(()));
        assert_eq!(
            notifications.read(&mut byte).unwrap_err().kind(),
            io::ErrorKind::WouldBlock
        );
        assert_eq!(transport.take_incoming().0.len(), 1);
        transport.acknowledge_read_notification();
        peer.write_all(&frame(PAYLOAD_FRAME, &[3], 0)).unwrap();
        until(|| notifications.read(&mut byte).ok());
    }

    #[test]
    fn incomplete_bytes_are_limited_and_queued_descriptors_are_released() {
        assert_eq!(MAX_UNPROCESSED_BUFFER_SIZE, 128 * 1024 * 1024);
        let (ours, mut peer) = UnixStream::pair().unwrap();
        let transport = TransportSocket::with_limits(SocketHandle::from(ours), None, 24, 4).unwrap();
        let (reader, writer) = std::io::pipe().unwrap();
        write_with_fds(
            &peer,
            &frame_header(ATTACHMENTS_FRAME, 0, 1),
            &[OwnedFd::from(writer).as_fd()],
        );
        peer.write_all(&frame_header(PAYLOAD_FRAME, 100, 1)).unwrap();
        peer.write_all(&[0; 20]).unwrap();
        assert!(transport.receive().is_none());
        assert_peer_sees_end(&mut peer);
        assert_pipe_closed(reader);
    }

    #[test]
    fn attachments_frames_have_a_cumulative_descriptor_limit() {
        assert_eq!(MAX_UNPROCESSED_FDS, 16129);
        let (ours, mut peer) = UnixStream::pair().unwrap();
        let transport = TransportSocket::with_limits(SocketHandle::from(ours), None, 24, 2).unwrap();
        let (reader, writer) = std::io::pipe().unwrap();
        let writer = OwnedFd::from(writer);
        for _ in 0..3 {
            write_with_fds(&peer, &frame_header(ATTACHMENTS_FRAME, 0, 1), &[writer.as_fd()]);
        }
        drop(writer);
        assert!(transport.receive().is_none());
        assert_peer_sees_end(&mut peer);
        assert_pipe_closed(reader);
    }

    #[test]
    fn complete_frames_do_not_accumulate_towards_the_limits() {
        let (ours, peer) = UnixStream::pair().unwrap();
        let transport = TransportSocket::with_limits(SocketHandle::from(ours), None, HEADER_SIZE, 0).unwrap();
        let bytes: Vec<u8> = (0..100).flat_map(|_| frame(PAYLOAD_FRAME, &[], 0)).collect();
        (&peer).write_all(&bytes).unwrap();
        drop(peer);
        for _ in 0..100 {
            assert!(transport.receive().unwrap().bytes().is_empty());
        }
        assert!(transport.receive().is_none());
    }
}
