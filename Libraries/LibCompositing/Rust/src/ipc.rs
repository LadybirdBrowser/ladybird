/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Rust codecs that the C++ IPC codecs of a type hand their encoder or decoder to. A value that Rust makes in one
//! process and reads in another then goes in the wire format of `libipc_rust`, without passing through a C++ type on
//! either side. See RustIPC.h for the C++ half.
//!
//! These codecs carry bytes only: C++ keeps the attachments of a message, so a value that would need one can't go
//! this way.

use libipc_rust::wire::{Decode, Decoder, Encode, Encoder};
use std::ffi::c_void;

/// Appends `length` bytes at `bytes` to the C++ `IPC::Encoder` that `encoder` points to.
pub type CppAppend = unsafe extern "C" fn(encoder: *mut c_void, bytes: *const u8, length: usize);

/// Encodes `value` into the message that the C++ `IPC::Encoder` at `encoder` builds, appending as it goes. Answers
/// false where the value does not encode, or would need an attachment.
///
/// # Safety
///
/// `append` must append to `encoder` before it returns, and keep no pointer to the bytes.
pub unsafe fn encode_into_cpp<T: Encode + ?Sized>(value: &T, encoder: *mut c_void, append: CppAppend) -> bool {
    let mut rust_encoder = Encoder::with_sink(Box::new(move |bytes: &[u8]| {
        // SAFETY: Guaranteed by the caller.
        unsafe { append(encoder, bytes.as_ptr(), bytes.len()) };
    }));
    if rust_encoder.encode(value).is_err() {
        return false;
    }
    let (_, attachments) = rust_encoder.finish();
    attachments.is_empty()
}

/// Decodes a value from the start of `bytes`, which are the rest of a message a C++ `IPC::Decoder` reads, and sets
/// `consumed` to how many of them it took.
pub fn decode_from_cpp<T: Decode>(bytes: &[u8], consumed: &mut usize) -> Option<T> {
    let mut decoder = Decoder::new(bytes, std::iter::empty());
    let value = decoder.decode().ok()?;
    *consumed = bytes.len() - decoder.remaining_bytes();
    Some(value)
}

#[cfg(test)]
mod tests {
    use super::*;

    unsafe extern "C" fn append_to_vec(encoder: *mut c_void, bytes: *const u8, length: usize) {
        // SAFETY: The tests below pass a live vector and the bytes of a slice.
        unsafe { (*encoder.cast::<Vec<u8>>()).extend_from_slice(ak::slice_from_raw_parts(bytes, length)) };
    }

    #[test]
    fn values_go_through_the_cpp_encoder_and_decoder_halves() {
        let mut message = Vec::new();
        // SAFETY: append_to_vec appends to the vector it is given.
        assert!(unsafe { encode_into_cpp(&vec![1u32, 2, 3], (&raw mut message).cast(), append_to_vec) });
        message.extend_from_slice(&[0xaa, 0xbb]);

        let mut consumed = 0;
        let value: Vec<u32> = decode_from_cpp(&message, &mut consumed).unwrap();
        assert_eq!(value, [1, 2, 3]);
        assert_eq!(consumed, 4 + 3 * 4);
        assert_eq!(&message[consumed..], [0xaa, 0xbb]);
    }

    #[test]
    fn a_value_that_does_not_fit_is_refused_and_consumes_nothing() {
        let mut consumed = 7;
        assert!(decode_from_cpp::<Vec<u32>>(&[3, 0, 0, 0, 1, 0, 0, 0], &mut consumed).is_none());
        assert_eq!(consumed, 7);
    }
}
