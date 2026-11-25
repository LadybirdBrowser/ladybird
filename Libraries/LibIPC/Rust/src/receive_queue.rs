/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::attachment::Attachment;
#[cfg(windows)]
pub(crate) type PayloadBytes = Vec<u8>;

/// The bytes of a received message: copied out of what carried them, or mapped where they arrived.
#[cfg(unix)]
pub enum PayloadBytes {
    Inline(Vec<u8>),
    #[cfg(target_vendor = "apple")]
    OutOfLine(crate::mach::VmRegion),
    #[cfg(target_os = "linux")]
    Mapped(crate::shared_memory::MappedPayload),
}

#[cfg(unix)]
impl AsRef<[u8]> for PayloadBytes {
    fn as_ref(&self) -> &[u8] {
        self.bytes()
    }
}

#[cfg(unix)]
impl PayloadBytes {
    pub fn bytes(&self) -> &[u8] {
        match self {
            Self::Inline(bytes) => bytes,
            #[cfg(target_vendor = "apple")]
            Self::OutOfLine(region) => region.bytes(),
            #[cfg(target_os = "linux")]
            Self::Mapped(mapping) => mapping.as_ref(),
        }
    }
}

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
