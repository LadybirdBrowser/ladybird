/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

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
