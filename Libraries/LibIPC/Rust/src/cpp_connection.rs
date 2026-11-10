/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Messages that Rust encodes and decodes on a connection whose transport C++ owns: what a C++ `IPC::Connection`
//! hands over undecoded, and what it posts for Rust.

#![allow(unsafe_code)]

use core::ffi::{c_int, c_void};

use crate::attachment::Attachment;
use crate::wire::{
    Decode, DecodeError, Decoder, DispatchError, EncodeError, EncodedMessage, EndpointMessage, Misbehavior,
};

/// A Mach port name and its disposition on Apple, and a descriptor elsewhere: `IPC::RawAttachment`.
#[repr(C)]
pub struct RawAttachment {
    value: c_int,
    disposition: u32,
}

impl RawAttachment {
    /// # Safety
    ///
    /// The raw attachment must hand over one owned descriptor or Mach right.
    unsafe fn into_attachment(self) -> Attachment {
        #[cfg(target_vendor = "apple")]
        {
            use crate::mach::{
                MACH_MSG_TYPE_MAKE_SEND, MACH_MSG_TYPE_MAKE_SEND_ONCE, MACH_MSG_TYPE_MOVE_RECEIVE,
                MACH_MSG_TYPE_MOVE_SEND_ONCE, MachPort, PortRight,
            };
            let right = match self.disposition {
                MACH_MSG_TYPE_MOVE_RECEIVE | MACH_MSG_TYPE_MAKE_SEND | MACH_MSG_TYPE_MAKE_SEND_ONCE => {
                    PortRight::Receive
                }
                MACH_MSG_TYPE_MOVE_SEND_ONCE => PortRight::SendOnce,
                _ => PortRight::Send,
            };
            Attachment::from_mach_port(MachPort::adopt(self.value as u32, right), self.disposition)
        }
        #[cfg(all(unix, not(target_vendor = "apple")))]
        {
            use std::os::fd::FromRawFd;
            // SAFETY: The caller hands over the descriptor.
            Attachment::from(unsafe { std::os::fd::OwnedFd::from_raw_fd(self.value) })
        }
        #[cfg(windows)]
        {
            // SAFETY: The caller hands over the descriptor.
            Attachment::from_descriptor(unsafe { crate::transport_windows::OwnedDescriptor::from_raw(self.value) })
        }
    }

    /// # Safety
    ///
    /// Each of the raw attachments must hand over its descriptor or Mach right.
    pub unsafe fn take_all(raw: &[Self]) -> Vec<Attachment> {
        raw.iter()
            .map(|attachment| {
                let owned = Self {
                    value: attachment.value,
                    disposition: attachment.disposition,
                };
                // SAFETY: The caller hands over what each raw attachment names.
                unsafe { owned.into_attachment() }
            })
            .collect()
    }

    pub fn from_attachment(attachment: Attachment) -> Self {
        #[cfg(target_vendor = "apple")]
        {
            let (port, disposition) = attachment.into_mach_port();
            Self {
                value: core::mem::ManuallyDrop::new(port).name() as c_int,
                disposition,
            }
        }
        #[cfg(all(unix, not(target_vendor = "apple")))]
        {
            use std::os::fd::IntoRawFd;
            Self {
                value: attachment
                    .into_fd()
                    .expect("a Unix attachment is a descriptor")
                    .into_raw_fd(),
                disposition: 0,
            }
        }
        #[cfg(windows)]
        {
            Self {
                value: attachment.into_descriptor().into_raw(),
                disposition: 0,
            }
        }
    }
}

/// A message that a C++ connection received and hands over undecoded, with where what Rust makes of it goes:
/// `IPC::FfiRawMessage`.
#[repr(C)]
pub struct RawMessage<'a> {
    bytes_including_header: ak::Span<'a, u8>,
    attachments: ak::Span<'a, RawAttachment>,
    outcome: *mut c_void,
    respond: unsafe extern "C" fn(*mut c_void, ak::Span<'_, u8>, ak::Span<'_, RawAttachment>),
    did_misbehave: unsafe extern "C" fn(*mut c_void, ak::Span<'_, u8>),
}

impl RawMessage<'_> {
    /// Has `dispatch`, the dispatcher of the message's endpoint over a stub, decode the message and hand it to that
    /// stub. The connection gets the response of a synchronous message, or what its peer did that it must not do.
    ///
    /// # Safety
    ///
    /// The message must be one `IPC::RawMessage::hand_to_rust()` made, which is handed to a stub once.
    pub unsafe fn hand_to(
        self,
        dispatch: impl FnOnce(&[u8], Vec<Attachment>) -> Result<Option<EncodedMessage>, DispatchError>,
    ) {
        // SAFETY: The C++ message hands over what each of its raw attachments names.
        let attachments = unsafe { RawAttachment::take_all(self.attachments.as_slice()) };
        let did_misbehave = |what_the_peer_did: &str| {
            // SAFETY: The C++ message copies the text.
            unsafe { (self.did_misbehave)(self.outcome, ak::Span::new(what_the_peer_did.as_bytes())) };
        };
        match dispatch(self.bytes_including_header.as_slice(), attachments) {
            Ok(None) => {}
            Ok(Some((bytes, attachments))) => {
                let raw_attachments: Vec<RawAttachment> =
                    attachments.into_iter().map(RawAttachment::from_attachment).collect();
                // SAFETY: The C++ message copies the bytes and takes every attachment over.
                unsafe { (self.respond)(self.outcome, ak::Span::new(&bytes), ak::Span::new(&raw_attachments)) };
            }
            Err(DispatchError::Decode(error)) => did_misbehave(&format!("Message does not decode: {error:?}")),
            Err(DispatchError::Misbehavior(Misbehavior(what_the_peer_did))) => did_misbehave(what_the_peer_did),
            Err(DispatchError::EncodeResponse(error)) => panic!("The response to a message does not encode: {error:?}"),
        }
    }
}

/// A value as C++ encoded it for Rust to decode, lent for a call: the bytes of an `IPC::Encoded<T>`.
#[repr(transparent)]
pub struct EncodedValue<'a>(ak::Span<'a, u8>);

impl EncodedValue<'_> {
    pub fn decode<T: Decode>(&self) -> Result<T, DecodeError> {
        let mut decoder = Decoder::new(self.0.as_slice(), []);
        let value = decoder.decode()?;
        if !decoder.is_at_end() {
            return Err(DecodeError::InvalidValue("bytes after an encoded value"));
        }
        Ok(value)
    }
}

/// One reference to an `IPC::ThreadSafeMessagePoster`, and how to reach it: `IPC::FfiThreadSafeMessagePoster`.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct FfiThreadSafeMessagePoster {
    poster_with_one_reference: *mut c_void,
    try_post: unsafe extern "C" fn(*mut c_void, ak::Span<'_, u8>, ak::Span<'_, RawAttachment>) -> bool,
    r#ref: unsafe extern "C" fn(*mut c_void),
    unref: unsafe extern "C" fn(*mut c_void),
}

const _: () = {
    assert!(size_of::<RawAttachment>() == 8);
    assert!(size_of::<RawMessage<'_>>() == 7 * size_of::<usize>());
    assert!(size_of::<FfiThreadSafeMessagePoster>() == 4 * size_of::<usize>());
};

/// Posts messages to the transport of a C++ connection from any thread.
pub struct ThreadSafeMessagePoster(FfiThreadSafeMessagePoster);

// SAFETY: The C++ poster counts its references atomically and posts under its own lock.
unsafe impl Send for ThreadSafeMessagePoster {}
unsafe impl Sync for ThreadSafeMessagePoster {}

impl ThreadSafeMessagePoster {
    /// # Safety
    ///
    /// `ffi` must hold one reference to a live `IPC::ThreadSafeMessagePoster`, which the caller gives up.
    pub unsafe fn adopt(ffi: FfiThreadSafeMessagePoster) -> Self {
        Self(ffi)
    }

    /// Answers whether the peer could still be reached.
    pub fn try_post<M: EndpointMessage>(&self, message: M) -> Result<bool, EncodeError> {
        let (bytes, attachments) = message.encode_message()?;
        Ok(self.try_post_encoded(&bytes, attachments))
    }

    pub fn try_post_encoded(&self, bytes: &[u8], attachments: Vec<Attachment>) -> bool {
        let raw_attachments: Vec<RawAttachment> = attachments.into_iter().map(RawAttachment::from_attachment).collect();
        // SAFETY: This holds a reference to the poster, which copies the bytes and takes every attachment over.
        unsafe {
            (self.0.try_post)(
                self.0.poster_with_one_reference,
                ak::Span::new(bytes),
                ak::Span::new(&raw_attachments),
            )
        }
    }
}

impl Clone for ThreadSafeMessagePoster {
    fn clone(&self) -> Self {
        // SAFETY: This holds a reference to the poster.
        unsafe { (self.0.r#ref)(self.0.poster_with_one_reference) };
        Self(self.0)
    }
}

impl Drop for ThreadSafeMessagePoster {
    fn drop(&mut self) {
        // SAFETY: This gives up the reference it holds.
        unsafe { (self.0.unref)(self.0.poster_with_one_reference) };
    }
}
