/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::collections::VecDeque;
use std::sync::{Condvar, Mutex, MutexGuard};

use crate::attachment::Attachment;
#[cfg(target_vendor = "apple")]
use crate::mach::PayloadBytes;
#[cfg(not(target_vendor = "apple"))]
type PayloadBytes = Vec<u8>;

/// A message the peer sent: its bytes and its attachments.
pub struct ReceivedMessage {
    pub(crate) bytes: PayloadBytes,
    pub attachments: Vec<Attachment>,
}

impl ReceivedMessage {
    pub fn bytes(&self) -> &[u8] {
        self.bytes.as_ref()
    }

    pub fn into_parts(self) -> (PayloadBytes, Vec<Attachment>) {
        (self.bytes, self.attachments)
    }
}

#[derive(Default)]
pub(crate) struct Incoming {
    pub messages: VecDeque<ReceivedMessage>,
    pub peer_eof: bool,
}

#[derive(Default)]
pub(crate) struct IncomingQueue {
    incoming: Mutex<Incoming>,
    changed: Condvar,
}

impl IncomingQueue {
    pub fn lock(&self) -> MutexGuard<'_, Incoming> {
        self.incoming.lock().unwrap_or_else(|error| error.into_inner())
    }

    pub fn push(&self, messages: impl IntoIterator<Item = ReceivedMessage>) {
        self.lock().messages.extend(messages);
        self.changed.notify_all();
    }

    pub fn mark_peer_eof(&self) {
        self.lock().peer_eof = true;
        self.changed.notify_all();
    }

    pub fn receive(&self) -> Option<ReceivedMessage> {
        let mut incoming = self.lock();
        loop {
            if let Some(message) = incoming.messages.pop_front() {
                return Some(message);
            }
            if incoming.peer_eof {
                return None;
            }
            incoming = self.changed.wait(incoming).unwrap_or_else(|error| error.into_inner());
        }
    }
}
