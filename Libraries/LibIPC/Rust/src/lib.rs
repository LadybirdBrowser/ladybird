/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! LibIPC's wire format, transports and message endpoints, for Rust processes and services that speak with C++ ones.
//!
//! Everything but the operating system interface of the transports is safe Rust.

#![deny(unsafe_code)]

pub mod attachment;
mod receive_queue;
#[cfg(not(target_vendor = "apple"))]
mod shared_memory;
#[cfg(not(target_vendor = "apple"))]
mod socket;
#[cfg(target_vendor = "apple")]
pub mod transport;
#[cfg(not(target_vendor = "apple"))]
pub mod transport_socket;
pub mod wire;

/// The most attachments a message may carry, as `IPC::MAX_MESSAGE_FD_COUNT`.
pub const MAX_MESSAGE_ATTACHMENT_COUNT: usize = 16000;

/// The largest payload a socket frame or Mach descriptor can represent.
pub const MAX_MESSAGE_PAYLOAD_SIZE: usize = u32::MAX as usize;

fn validate_message_size(payload_size: usize, attachment_count: usize) -> std::io::Result<()> {
    if payload_size > MAX_MESSAGE_PAYLOAD_SIZE || attachment_count > MAX_MESSAGE_ATTACHMENT_COUNT {
        return Err(std::io::ErrorKind::InvalidInput.into());
    }
    Ok(())
}

#[cfg(target_vendor = "apple")]
pub mod mach;

pub use attachment::Attachment;
pub use wire::{Decode, DecodeError, Decoder, Encode, EncodeError, Encoder, EndpointMessage};

#[cfg(test)]
mod tests {
    use super::*;
    #[cfg(target_vendor = "apple")]
    use transport::TransportMachPort as Transport;
    #[cfg(not(target_vendor = "apple"))]
    use transport_socket::TransportSocket as Transport;

    #[test]
    fn outgoing_message_size_limits() {
        validate_message_size(MAX_MESSAGE_PAYLOAD_SIZE, MAX_MESSAGE_ATTACHMENT_COUNT).unwrap();
        for (payload_size, attachment_count) in
            [(MAX_MESSAGE_PAYLOAD_SIZE + 1, 0), (0, MAX_MESSAGE_ATTACHMENT_COUNT + 1)]
        {
            assert_eq!(
                validate_message_size(payload_size, attachment_count)
                    .unwrap_err()
                    .kind(),
                std::io::ErrorKind::InvalidInput
            );
        }
    }

    #[test]
    fn refusing_too_many_attachments_keeps_the_connection_open() {
        // NB: On sockets, every attachment holds a descriptor until the message is sent.
        #[cfg(not(target_vendor = "apple"))]
        socket::raise_descriptor_limit();
        let (a, handle) = Transport::create_paired().unwrap();
        #[cfg(target_vendor = "apple")]
        let b = Transport::from_handle(handle).unwrap();
        #[cfg(not(target_vendor = "apple"))]
        let b = Transport::new(handle).unwrap();
        let (mut reader, writer) = std::io::pipe().unwrap();
        let attachments = (0..=MAX_MESSAGE_ATTACHMENT_COUNT)
            .map(|_| Attachment::from_fd(writer.try_clone().unwrap().into()).unwrap())
            .collect();
        drop(writer);
        assert_eq!(
            a.post_message(vec![1], attachments).unwrap_err().kind(),
            std::io::ErrorKind::InvalidInput
        );
        assert_eq!(std::io::Read::read(&mut reader, &mut [0]).unwrap(), 0);
        assert!(a.is_open());
        assert!(b.is_open());
        a.post_message(vec![2], vec![]).unwrap();
        b.post_message(vec![3], vec![]).unwrap();
        a.flush();
        b.flush();
        assert_eq!(a.receive().unwrap().bytes(), [3]);
        assert_eq!(b.receive().unwrap().bytes(), [2]);
    }
}
