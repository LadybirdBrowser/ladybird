/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The Windows pipe transport, which speaks with `IPC::TransportWindows` on the other end of a pipe that
//! `Core::System::duplex_pipe()` made.
//!
//! The pipe carries frames: a 12-byte header of a payload size, an attachment count and where the payload is, then the
//! payload. Attachments go through the Browser's handle broker, and payloads over 1 MiB through shared memory, neither
//! of which this transport speaks yet: it sends only frames without them, and a frame that needs either ends the
//! stream.

use std::collections::VecDeque;
use std::io;
use std::os::windows::io::OwnedHandle;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Condvar, Mutex, MutexGuard};
use std::thread::JoinHandle;

use crate::attachment::Attachment;
use crate::pipe::{Operation, Pipe, PipeError};
use crate::receive_queue::{IncomingQueue, ReceivedMessage};

pub use crate::pipe_frames::MAX_INLINE_PAYLOAD_SIZE;
use crate::pipe_frames::{inline_frame, parse_frames};

const READ_BUFFER_SIZE: usize = 64 * 1024;

#[derive(Default)]
struct OutgoingQueue {
    frames: VecDeque<Vec<u8>>,
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
        queue.frames.clear();
        self.outgoing_changed.notify_all();
    }
}

pub struct TransportWindows {
    shared: Arc<Shared>,
    pipe: Arc<Pipe>,
    reader: Option<JoinHandle<()>>,
    writer: Option<JoinHandle<()>>,
    pipe_is_open: AtomicBool,
}

impl TransportWindows {
    /// Takes ownership of an overlapped pipe handle.
    pub fn new(handle: OwnedHandle) -> io::Result<Self> {
        let shared = Arc::new(Shared {
            incoming: IncomingQueue::default(),
            outgoing: Mutex::default(),
            outgoing_changed: Condvar::new(),
        });
        let pipe = Arc::new(Pipe::new(handle)?);

        // NB: This owner stops the pipe and joins any started threads if construction fails.
        let mut transport = Self {
            shared: Arc::clone(&shared),
            pipe: Arc::clone(&pipe),
            reader: None,
            writer: None,
            pipe_is_open: AtomicBool::new(true),
        };
        let reader_pipe = Arc::clone(&pipe);
        let reader_shared = Arc::clone(&shared);
        transport.reader = Some(
            std::thread::Builder::new()
                .name("IPC reader (pipe)".to_string())
                .spawn(move || read_frames(&reader_pipe, &reader_shared))?,
        );
        let writer_shared = Arc::clone(&shared);
        transport.writer = Some(
            std::thread::Builder::new()
                .name("IPC writer (pipe)".to_string())
                .spawn(move || write_frames(&pipe, &writer_shared))?,
        );
        Ok(transport)
    }

    /// Queues a message for the writer thread to send, or refuses it once the transport has stopped sending. A message
    /// with attachments or a payload over `MAX_INLINE_PAYLOAD_SIZE` is refused as unsupported.
    pub fn post_message(&self, bytes: Vec<u8>, attachments: Vec<Attachment>) -> io::Result<()> {
        crate::validate_message_size(bytes.len(), attachments.len())?;
        if !attachments.is_empty() || bytes.len() > MAX_INLINE_PAYLOAD_SIZE {
            return Err(io::ErrorKind::Unsupported.into());
        }
        let frame = inline_frame(&bytes);

        let mut queue = self.shared.lock_outgoing();
        if queue.stopped {
            return Err(io::ErrorKind::NotConnected.into());
        }
        queue.frames.push_back(frame);
        self.shared.outgoing_changed.notify_all();
        Ok(())
    }

    /// Waits until every message posted so far is sent, or the transport stops.
    pub fn flush(&self) {
        let mut queue = self.shared.lock_outgoing();
        while !queue.stopped && (!queue.frames.is_empty() || queue.sending) {
            queue = self
                .shared
                .outgoing_changed
                .wait(queue)
                .unwrap_or_else(|error| error.into_inner());
        }
    }

    pub fn is_open(&self) -> bool {
        self.pipe_is_open.load(Ordering::Relaxed) && !self.shared.incoming.lock().peer_eof
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

    /// Stops sending and receiving, from any thread that shares the transport. Messages not sent yet are dropped, and a
    /// `receive()` returns `None` once it has taken what had arrived. The pipe handle stays open until the transport
    /// is dropped, which is when the peer sees this end go.
    pub fn shutdown(&self) {
        self.pipe_is_open.store(false, Ordering::Relaxed);
        self.shared.stop_outgoing();
        self.pipe.stop();
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

impl Drop for TransportWindows {
    fn drop(&mut self) {
        self.close();
    }
}

fn write_frames(pipe: &Pipe, shared: &Shared) {
    let operation = match Operation::new() {
        Ok(operation) => operation,
        Err(error) => {
            eprintln!("TransportWindows: Could not start writing: {error}");
            shared.stop_outgoing();
            return;
        }
    };
    loop {
        let frame = {
            let mut queue = shared.lock_outgoing();
            loop {
                if queue.stopped {
                    return;
                }
                if let Some(frame) = queue.frames.pop_front() {
                    queue.sending = true;
                    break frame;
                }
                queue = shared
                    .outgoing_changed
                    .wait(queue)
                    .unwrap_or_else(|error| error.into_inner());
            }
        };
        let mut remaining = frame.as_slice();
        while !remaining.is_empty() {
            match pipe.write(&operation, remaining) {
                Ok(written) => remaining = &remaining[written..],
                Err(PipeError::Closed | PipeError::Stopped) => {
                    shared.stop_outgoing();
                    return;
                }
                Err(PipeError::Io(error)) => {
                    eprintln!("TransportWindows: Could not send a message: {error}");
                    shared.stop_outgoing();
                    return;
                }
            }
        }
        shared.lock_outgoing().sending = false;
        shared.outgoing_changed.notify_all();
    }
}

fn read_frames(pipe: &Pipe, shared: &Shared) {
    if let Err(what) = read_frames_until_end(pipe, shared) {
        eprintln!("TransportWindows: Refusing {what}");
        pipe.stop();
        shared.stop_outgoing();
    }
    shared.incoming.mark_peer_eof();
}

/// Reads until the peer closes its end or the pipe is stopped, or answers what was wrong with what the peer sent.
fn read_frames_until_end(pipe: &Pipe, shared: &Shared) -> Result<(), String> {
    let operation = Operation::new().map_err(|error| format!("to read: {error}"))?;
    let mut buffer = vec![0u8; READ_BUFFER_SIZE];
    let mut unprocessed = Vec::new();
    let mut messages = Vec::new();
    loop {
        let read = match pipe.read(&operation, &mut buffer) {
            Ok(read) => read,
            Err(PipeError::Closed | PipeError::Stopped) => return Ok(()),
            Err(PipeError::Io(error)) => return Err(format!("to read further after an error: {error}")),
        };
        unprocessed.extend_from_slice(&buffer[..read]);
        let result = parse_frames(&unprocessed, &mut messages);
        if !messages.is_empty() {
            shared.incoming.push(messages.drain(..));
        }
        let parsed_size = result.map_err(str::to_string)?;
        unprocessed.drain(..parsed_size);
    }
}
