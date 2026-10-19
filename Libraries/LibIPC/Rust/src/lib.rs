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
pub mod wire;

/// The most attachments a message may carry, as `IPC::MAX_MESSAGE_FD_COUNT`.
pub const MAX_MESSAGE_ATTACHMENT_COUNT: usize = 16000;

/// The largest payload a socket frame or Mach descriptor can represent.
pub const MAX_MESSAGE_PAYLOAD_SIZE: usize = u32::MAX as usize;

#[cfg(target_vendor = "apple")]
pub mod mach;

pub use attachment::Attachment;
pub use wire::{Decode, DecodeError, Decoder, Encode, EncodeError, Encoder, EndpointMessage};
