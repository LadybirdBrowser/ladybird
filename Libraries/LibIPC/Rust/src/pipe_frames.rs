/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The frames of the Windows pipe transport, as `IPC::TransportWindows` writes and reads them: a 12-byte header of a
//! 32-bit payload size, a 32-bit attachment count and a byte for where the payload is, then the payload. This builds
//! on every platform for its tests, since the Windows transport's tests don't run in CI.

use crate::receive_queue::ReceivedMessage;

pub(crate) const HEADER_SIZE: usize = 12;
const INLINE_PAYLOAD_LOCATION: u8 = 0;
/// The largest payload that goes inline, as `IPC::TransportWindows::MAX_INLINE_PAYLOAD_SIZE`.
pub const MAX_INLINE_PAYLOAD_SIZE: usize = 1024 * 1024;

/// A frame of `payload`, which goes inline and without attachments. The payload must fit inline.
pub(crate) fn inline_frame(payload: &[u8]) -> Vec<u8> {
    debug_assert!(payload.len() <= MAX_INLINE_PAYLOAD_SIZE);
    let mut frame = Vec::with_capacity(HEADER_SIZE + payload.len());
    frame.extend_from_slice(&(payload.len() as u32).to_ne_bytes());
    frame.extend_from_slice(&0u32.to_ne_bytes());
    // NB: The C++ header is a struct whose padding goes with it. Zero padding is as good as any.
    frame.extend_from_slice(&[INLINE_PAYLOAD_LOCATION, 0, 0, 0]);
    frame.extend_from_slice(payload);
    frame
}

/// Takes the complete frames at the start of `bytes` as messages, and answers how many bytes they took.
pub(crate) fn parse_frames(bytes: &[u8], messages: &mut Vec<ReceivedMessage>) -> Result<usize, &'static str> {
    let mut offset = 0;
    while let Some(header) = bytes.get(offset..offset + HEADER_SIZE) {
        let read_u32 = |at: usize| u32::from_ne_bytes([header[at], header[at + 1], header[at + 2], header[at + 3]]);
        let payload_size = read_u32(0) as usize;
        if read_u32(4) != 0 {
            return Err("a message with attachments, which this transport does not take yet");
        }
        if header[8] != INLINE_PAYLOAD_LOCATION {
            return Err("a message with its payload in shared memory, which this transport does not take yet");
        }
        if payload_size > MAX_INLINE_PAYLOAD_SIZE {
            return Err("an inline payload larger than a frame holds");
        }
        let payload_start = offset + HEADER_SIZE;
        let Some(payload) = bytes.get(payload_start..payload_start + payload_size) else {
            break;
        };
        messages.push(ReceivedMessage {
            bytes: payload.to_vec(),
            attachments: Vec::new(),
        });
        offset = payload_start + payload_size;
    }
    Ok(offset)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn an_inline_frame_has_a_header_then_the_payload() {
        let frame = inline_frame(&[7, 8, 9]);
        assert_eq!(frame, [3u8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 7, 8, 9]);
        let mut messages = Vec::new();
        assert_eq!(parse_frames(&frame, &mut messages), Ok(frame.len()));
        assert_eq!(messages[0].bytes(), [7, 8, 9]);
    }

    fn frame(payload: &[u8], attachment_count: u32, location: u8) -> Vec<u8> {
        let mut frame = (payload.len() as u32).to_ne_bytes().to_vec();
        frame.extend_from_slice(&attachment_count.to_ne_bytes());
        frame.extend_from_slice(&[location, 0xaa, 0xbb, 0xcc]);
        frame.extend_from_slice(payload);
        frame
    }

    #[test]
    fn complete_frames_become_messages_and_a_partial_one_waits() {
        let mut bytes = frame(&[1, 2, 3], 0, 0);
        bytes.extend(frame(&[], 0, 0));
        let partial = frame(&[4, 5, 6, 7], 0, 0);
        bytes.extend_from_slice(&partial[..HEADER_SIZE + 2]);

        let mut messages = Vec::new();
        let parsed = parse_frames(&bytes, &mut messages).unwrap();
        assert_eq!(parsed, 2 * HEADER_SIZE + 3);
        let payloads: Vec<&[u8]> = messages.iter().map(ReceivedMessage::bytes).collect();
        assert_eq!(payloads, [&[1u8, 2, 3][..], &[][..]]);
    }

    #[test]
    fn frames_this_transport_does_not_take_are_refused() {
        let mut messages = Vec::new();
        assert!(parse_frames(&frame(&[1], 1, 0), &mut messages).is_err());
        assert!(parse_frames(&frame(&[], 0, 1), &mut messages).is_err());
        let mut too_large = frame(&[], 0, 0);
        too_large[..4].copy_from_slice(&((MAX_INLINE_PAYLOAD_SIZE + 1) as u32).to_ne_bytes());
        assert!(parse_frames(&too_large, &mut messages).is_err());
        assert!(messages.is_empty());
    }
}
