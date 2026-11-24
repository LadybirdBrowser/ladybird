/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! A DNS connection that never waits or performs I/O. The socket owner supplies received bytes and sends queued output.
//! Streams carry plaintext here; the embedder handles TLS and reports connection only after the handshake.

use std::collections::VecDeque;
use std::fmt;
use std::time::{Duration, Instant};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Kind {
    Udp,
    Stream,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Error {
    TimedOut,
    Closed,
    Socket(String),
    FrameTooLarge,
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Error::TimedOut => write!(f, "Connecting to the DNS server timed out"),
            Error::Closed => write!(f, "The DNS server closed the connection"),
            Error::Socket(error) => write!(f, "DNS server connection failed: {error}"),
            Error::FrameTooLarge => write!(f, "DNS message is too large for a TCP frame"),
        }
    }
}

#[derive(Debug, PartialEq, Eq)]
enum State {
    Connecting,
    Ready,
    Failed(Error),
}

pub struct Connection {
    kind: Kind,
    state: State,
    deadline: Instant,
    // Streams only: bytes of a frame whose end has not arrived yet.
    incoming: Vec<u8>,
    // Messages that arrived complete.
    messages: VecDeque<Vec<u8>>,
    // Datagrams for UDP; for streams, bytes to write in order.
    outgoing: VecDeque<Vec<u8>>,
    // Messages to send once the connection is up.
    pending: Vec<Vec<u8>>,
}

impl Connection {
    /// Initially connecting, with the given timeout.
    pub fn new(kind: Kind, now: Instant, timeout: Duration) -> Self {
        Self {
            kind,
            state: State::Connecting,
            deadline: now + timeout,
            incoming: Vec::new(),
            messages: VecDeque::new(),
            outgoing: VecDeque::new(),
            pending: Vec::new(),
        }
    }

    pub fn kind(&self) -> Kind {
        self.kind
    }

    pub fn is_ready(&self) -> bool {
        self.state == State::Ready
    }

    pub fn is_connecting(&self) -> bool {
        self.state == State::Connecting
    }

    pub fn error(&self) -> Option<&Error> {
        match &self.state {
            State::Failed(error) => Some(error),
            _ => None,
        }
    }

    /// The connection timeout deadline, only while connecting.
    pub fn deadline(&self) -> Option<Instant> {
        (self.state == State::Connecting).then_some(self.deadline)
    }

    pub fn on_tick(&mut self, now: Instant) {
        if self.deadline().is_some_and(|deadline| now >= deadline) {
            self.fail(Error::TimedOut);
        }
    }

    pub fn on_connected(&mut self) {
        if self.state != State::Connecting {
            return;
        }
        self.state = State::Ready;
        for message in std::mem::take(&mut self.pending) {
            if let Err(error) = self.send_now(message) {
                self.fail(error);
                return;
            }
        }
    }

    pub fn on_socket_error(&mut self, error: String) {
        self.fail(Error::Socket(error));
    }

    /// The peer closed its side of a stream.
    pub fn on_eof(&mut self) {
        if self.error().is_none() {
            self.fail(Error::Closed);
        }
    }

    /// For UDP, `bytes` is one datagram. For streams, it is whatever the socket read.
    pub fn on_received(&mut self, bytes: &[u8]) {
        if self.error().is_some() {
            return;
        }
        match self.kind {
            Kind::Udp => self.messages.push_back(bytes.to_vec()),
            Kind::Stream => {
                self.incoming.extend_from_slice(bytes);
                self.take_frames();
            }
        }
    }

    /// Queues a message to the server, to go out once the connection is up.
    pub fn send(&mut self, message: Vec<u8>) -> Result<(), Error> {
        match &self.state {
            State::Failed(error) => Err(error.clone()),
            State::Connecting => {
                if self.kind == Kind::Stream && message.len() > usize::from(u16::MAX) {
                    return Err(Error::FrameTooLarge);
                }
                self.pending.push(message);
                Ok(())
            }
            State::Ready => self.send_now(message),
        }
    }

    pub fn take_message(&mut self) -> Option<Vec<u8>> {
        self.messages.pop_front()
    }

    /// The next datagram, or the next bytes of the stream, to write to the socket.
    pub fn take_outgoing(&mut self) -> Option<Vec<u8>> {
        self.outgoing.pop_front()
    }

    fn send_now(&mut self, message: Vec<u8>) -> Result<(), Error> {
        if self.kind == Kind::Udp {
            self.outgoing.push_back(message);
            return Ok(());
        }

        // RFC 1035, 4.2.2. TCP usage.
        // The message is prefixed with a two byte length field which gives the message length, excluding the two
        // byte length field.
        let length = u16::try_from(message.len()).map_err(|_| Error::FrameTooLarge)?;
        let mut framed = Vec::with_capacity(message.len() + 2);
        framed.extend_from_slice(&length.to_be_bytes());
        framed.extend_from_slice(&message);
        self.outgoing.push_back(framed);
        Ok(())
    }

    fn fail(&mut self, error: Error) {
        self.state = State::Failed(error);
        self.pending.clear();
        self.outgoing.clear();
    }

    fn take_frames(&mut self) {
        // NB: The bytes of complete frames go all at once, rather than moving what follows each one forward.
        let mut consumed = 0;
        while let Some(length) = self.incoming[consumed..]
            .first_chunk::<2>()
            .map(|length| usize::from(u16::from_be_bytes(*length)))
        {
            let start = consumed + 2;
            if self.incoming.len() < start + length {
                break;
            }
            self.messages.push_back(self.incoming[start..start + length].to_vec());
            consumed = start + length;
        }
        self.incoming.drain(..consumed);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn udp_sends_datagrams_once_connected() {
        let mut connection = Connection::new(Kind::Udp, Instant::now(), Duration::from_secs(5));
        connection.send(vec![1, 2, 3]).unwrap();
        assert_eq!(connection.take_outgoing(), None);
        connection.on_connected();
        assert_eq!(connection.take_outgoing(), Some(vec![1, 2, 3]));
        connection.on_received(&[4, 5]);
        assert_eq!(connection.take_message(), Some(vec![4, 5]));
    }

    #[test]
    fn stream_frames_and_reassembles() {
        let mut connection = Connection::new(Kind::Stream, Instant::now(), Duration::from_secs(5));
        connection.on_connected();
        connection.send(vec![0xAA, 0xBB]).unwrap();
        assert_eq!(connection.take_outgoing(), Some(vec![0, 2, 0xAA, 0xBB]));

        connection.on_received(&[0, 3, 1]);
        assert_eq!(connection.take_message(), None);
        connection.on_received(&[2, 3, 0, 1]);
        assert_eq!(connection.take_message(), Some(vec![1, 2, 3]));
        assert_eq!(connection.take_message(), None);
        connection.on_received(&[9]);
        assert_eq!(connection.take_message(), Some(vec![9]));
    }

    #[test]
    fn stream_takes_every_frame_of_one_read() {
        let mut connection = Connection::new(Kind::Stream, Instant::now(), Duration::from_secs(5));
        connection.on_connected();
        let mut bytes = Vec::new();
        for frame in 0..100u8 {
            bytes.extend_from_slice(&[0, 1, frame]);
        }
        bytes.extend_from_slice(&[0, 2, 7]);
        connection.on_received(&bytes);
        for frame in 0..100u8 {
            assert_eq!(connection.take_message(), Some(vec![frame]));
        }
        assert_eq!(connection.take_message(), None);
        connection.on_received(&[8]);
        assert_eq!(connection.take_message(), Some(vec![7, 8]));
    }

    #[test]
    fn stream_rejects_oversized_messages() {
        let mut connection = Connection::new(Kind::Stream, Instant::now(), Duration::from_secs(5));
        assert_eq!(connection.send(vec![0; 70_000]), Err(Error::FrameTooLarge));
    }

    // A server that accepts the connection and never finishes the handshake.
    #[test]
    fn connecting_times_out() {
        let start = Instant::now();
        let mut connection = Connection::new(Kind::Stream, start, Duration::from_secs(5));
        connection.send(vec![1]).unwrap();
        connection.on_tick(start + Duration::from_secs(4));
        assert!(connection.is_connecting());
        connection.on_tick(start + Duration::from_secs(5));
        assert_eq!(connection.error(), Some(&Error::TimedOut));
        assert_eq!(connection.take_outgoing(), None);
        assert_eq!(connection.send(vec![1]), Err(Error::TimedOut));
    }

    #[test]
    fn eof_fails_the_connection() {
        let mut connection = Connection::new(Kind::Stream, Instant::now(), Duration::from_secs(5));
        connection.on_connected();
        connection.on_eof();
        assert_eq!(connection.error(), Some(&Error::Closed));
    }
}
