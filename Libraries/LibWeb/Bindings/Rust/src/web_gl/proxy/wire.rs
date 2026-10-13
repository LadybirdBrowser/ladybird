/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The framing of the WebGL command stream: how a struct and the data that belongs to it become the bytes of a record
//! or of a synchronous call. Libraries/LibCompositing/WebGL/WebGLCommandList.{h,cpp} is the C++ side of the same
//! format, which reads what this writes.

use core::mem::size_of;
use core::{ptr, slice};

use super::vocabulary::{CommandType, SyncCallType};

/// Records start at multiples of this many bytes, and so does the data that follows a struct, counted from the start
/// of the struct.
pub(crate) const RECORD_ALIGNMENT: usize = 16;

/// What precedes the struct of a record and of a request: its type as a `u16`, two unused bytes, and the number of
/// bytes that follow the header as a `u32`.
pub(crate) const HEADER_SIZE: usize = 8;

/// The data that a field of a struct stands for: `size` bytes that start `offset` bytes after the start of the
/// struct.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(C)]
pub(crate) struct DataSpan {
    pub(crate) offset: u32,
    pub(crate) size: u32,
}

impl DataSpan {
    pub(crate) const EMPTY: Self = Self { offset: 0, size: 0 };

    /// The span of the data that follows a struct `S`.
    pub(crate) fn following<S: WireStruct>(data: InlineData<'_>) -> Self {
        Self {
            offset: wire_u32(size_of::<S>().next_multiple_of(RECORD_ALIGNMENT)),
            size: wire_u32(data.len()),
        }
    }
}

/// A size or an offset as the stream carries it. The stream cannot carry a record of 4 GiB, which is far more than a
/// buffer that script can allocate holds.
fn wire_u32(size: usize) -> u32 {
    u32::try_from(size).expect("a WebGL command and its data are smaller than 4 GiB")
}

/// A struct that crosses the command stream as its bytes.
///
/// # Safety
///
/// The type is `repr(C)`, made of numbers, booleans and [`DataSpan`]s, and has no byte that is not part of a field.
pub(crate) unsafe trait WireStruct: Copy + 'static {
    /// The struct with every field zero. A struct is built from it, so that the bytes that the C ABI leaves unused
    /// between its fields, which are fields of their own here, do not have to be named.
    const ZEROED: Self;

    fn as_bytes(&self) -> &[u8] {
        // SAFETY: Per the contract of the trait every byte of the struct is initialized.
        unsafe { slice::from_raw_parts(ptr::from_ref(self).cast::<u8>(), size_of::<Self>()) }
    }
}

pub(crate) trait Command: WireStruct {
    const TYPE: CommandType;
}

pub(crate) trait SyncRequest: WireStruct {
    const TYPE: SyncCallType;
    type Reply: SyncReply;
}

/// # Safety
///
/// Every bit pattern is a value of the type.
pub(crate) unsafe trait SyncReply: WireStruct {
    fn from_reply_bytes(reply: &[u8]) -> Self {
        assert!(
            reply.len() >= size_of::<Self>(),
            "the reply to a WebGL call is shorter than its struct"
        );
        // SAFETY: The bytes are as many as the struct has, and per the contract of the trait any bytes are a value
        //         of it.
        unsafe { ptr::read_unaligned(reply.as_ptr().cast::<Self>()) }
    }
}

#[derive(Clone, Copy, Debug)]
pub(crate) enum InlineData<'a> {
    /// No bytes take no room at all, not even for the alignment of what would follow.
    Bytes(&'a [u8]),
    /// The stream has a NUL after the bytes.
    NulTerminated(&'a [u8]),
}

impl InlineData<'_> {
    pub(crate) const NONE: Self = Self::Bytes(&[]);

    pub(crate) fn len(self) -> usize {
        match self {
            Self::Bytes(bytes) => bytes.len(),
            Self::NulTerminated(bytes) => bytes.len() + 1,
        }
    }

    /// # Safety
    ///
    /// `destination` is valid for writes of `self.len()` bytes that do not overlap the data.
    unsafe fn write_to(self, destination: *mut u8) {
        let (Self::Bytes(bytes) | Self::NulTerminated(bytes)) = self;
        // SAFETY: The caller provides room for the bytes, and for the terminator after them.
        unsafe {
            ptr::copy_nonoverlapping(bytes.as_ptr(), destination, bytes.len());
            if matches!(self, Self::NulTerminated(_)) {
                destination.add(bytes.len()).write(0);
            }
        }
    }
}

/// The number of bytes that a struct and its data take together.
fn payload_layout_size(struct_size: usize, inline_data: InlineData<'_>) -> usize {
    match inline_data.len() {
        0 => struct_size,
        inline_data_size => struct_size.next_multiple_of(RECORD_ALIGNMENT) + inline_data_size,
    }
}

/// The number of bytes that a record takes in the command stream.
pub(crate) fn padded_record_size(struct_size: usize, inline_data: InlineData<'_>) -> usize {
    (HEADER_SIZE + payload_layout_size(struct_size, inline_data)).next_multiple_of(RECORD_ALIGNMENT)
}

/// Writes a header, a struct and its data, and zeroes what is left of `size` bytes, so that no byte of the stream is
/// one that happened to be in memory.
///
/// # Safety
///
/// `destination` is valid for writes of `size` bytes, which are at least as many as the header, the struct and the
/// data take, and which overlap neither the struct nor the data.
unsafe fn write_framed(
    destination: *mut u8,
    size: usize,
    type_number: u16,
    payload: &[u8],
    inline_data: InlineData<'_>,
) {
    let payload_size = wire_u32(size - HEADER_SIZE);
    let mut header = [0; HEADER_SIZE];
    header[..2].copy_from_slice(&type_number.to_ne_bytes());
    header[4..].copy_from_slice(&payload_size.to_ne_bytes());

    let mut written = HEADER_SIZE + payload.len();
    // SAFETY: The caller provides `size` bytes, and every write below stays inside the layout that
    //         payload_layout_size() computes, which `size` covers.
    unsafe {
        ptr::copy_nonoverlapping(header.as_ptr(), destination, HEADER_SIZE);
        ptr::copy_nonoverlapping(payload.as_ptr(), destination.add(HEADER_SIZE), payload.len());
        if inline_data.len() > 0 {
            let inline_data_offset = HEADER_SIZE + payload.len().next_multiple_of(RECORD_ALIGNMENT);
            ptr::write_bytes(destination.add(written), 0, inline_data_offset - written);
            inline_data.write_to(destination.add(inline_data_offset));
            written = inline_data_offset + inline_data.len();
        }
        ptr::write_bytes(destination.add(written), 0, size - written);
    }
}

/// Writes a record of the command stream.
///
/// # Safety
///
/// `destination` is valid for writes of `padded_record_size(payload.len(), inline_data)` bytes that overlap neither
/// the payload nor the data.
pub(crate) unsafe fn write_record(
    destination: *mut u8,
    command_type: CommandType,
    payload: &[u8],
    inline_data: InlineData<'_>,
) {
    let record_size = padded_record_size(payload.len(), inline_data);
    // SAFETY: The caller provides the bytes of the record.
    unsafe { write_framed(destination, record_size, command_type as u16, payload, inline_data) };
}

/// Appends a record to commands that are kept outside the shared command buffer.
pub(crate) fn append_record(
    commands: &mut Vec<u8>,
    command_type: CommandType,
    payload: &[u8],
    inline_data: InlineData<'_>,
) {
    let record_offset = commands.len();
    assert!(record_offset.is_multiple_of(RECORD_ALIGNMENT));
    commands.resize(record_offset + padded_record_size(payload.len(), inline_data), 0);
    // SAFETY: The vector was just grown by the size of the record, and it owns its bytes.
    unsafe {
        write_record(
            commands.as_mut_ptr().add(record_offset),
            command_type,
            payload,
            inline_data,
        );
    }
}

/// Replaces the contents of `request_bytes` with the request of a synchronous call.
pub(crate) fn encode_sync_request<R: SyncRequest>(
    request_bytes: &mut Vec<u8>,
    request: &R,
    inline_data: InlineData<'_>,
) {
    let payload_size = payload_layout_size(size_of::<R>(), inline_data).next_multiple_of(RECORD_ALIGNMENT);
    request_bytes.clear();
    request_bytes.resize(HEADER_SIZE + payload_size, 0);
    // SAFETY: The vector has the size of the header and the padded payload, and it owns its bytes.
    unsafe {
        write_framed(
            request_bytes.as_mut_ptr(),
            request_bytes.len(),
            R::TYPE as u16,
            request.as_bytes(),
            inline_data,
        );
    }
}

/// The data that a span of the struct of a reply stands for.
pub(crate) fn resolve_data_span(reply: &[u8], span: DataSpan) -> &[u8] {
    let start = span.offset as usize;
    start
        .checked_add(span.size as usize)
        .and_then(|end| reply.get(start..end))
        .expect("a span of the reply to a WebGL call lies inside the reply")
}

#[cfg(test)]
mod tests {
    use super::super::vocabulary::{commands, sync_calls};
    use super::*;

    fn record(command_type: CommandType, payload: &[u8], inline_data: InlineData<'_>) -> Vec<u8> {
        // NB: The bytes that a record is written over are not zero, as those of a reused buffer are not.
        let mut bytes = vec![0xAA; padded_record_size(payload.len(), inline_data)];
        // SAFETY: The vector has the size of the record.
        unsafe { write_record(bytes.as_mut_ptr(), command_type, payload, inline_data) };
        bytes
    }

    fn header(type_number: u16, payload_size: u32) -> Vec<u8> {
        let mut header = Vec::new();
        header.extend_from_slice(&type_number.to_ne_bytes());
        header.extend_from_slice(&[0, 0]);
        header.extend_from_slice(&payload_size.to_ne_bytes());
        header
    }

    #[test]
    fn a_record_is_a_header_and_a_struct_padded_to_the_alignment() {
        let command = commands::Viewport {
            x: 1,
            y: 2,
            width: 3,
            height: 4,
        };
        let bytes = record(CommandType::Viewport, command.as_bytes(), InlineData::NONE);
        assert_eq!(bytes.len(), 32);
        assert_eq!(bytes[..8], header(CommandType::Viewport as u16, 24));
        assert_eq!(bytes[8..24], *command.as_bytes());
        assert_eq!(bytes[24..], [0; 8]);
    }

    #[test]
    fn the_data_of_a_record_follows_its_struct_at_the_alignment() {
        let data = [7u8; 5];
        let command = commands::Uniform1fv {
            location: 9,
            count: 1,
            value: DataSpan::following::<commands::Uniform1fv>(InlineData::Bytes(&data)),
        };
        assert_eq!(command.value, DataSpan { offset: 16, size: 5 });
        let bytes = record(CommandType::Uniform1fv, command.as_bytes(), InlineData::Bytes(&data));
        assert_eq!(bytes.len(), 32);
        assert_eq!(bytes[..8], header(CommandType::Uniform1fv as u16, 24));
        assert_eq!(bytes[8..24], *command.as_bytes());
        assert_eq!(bytes[24..29], data);
        assert_eq!(bytes[29..], [0; 3]);
    }

    #[test]
    fn the_bytes_between_a_struct_and_its_data_are_zero() {
        let command = commands::Enable { cap: u32::MAX };
        let bytes = record(CommandType::Enable, command.as_bytes(), InlineData::Bytes(&[1]));
        assert_eq!(bytes.len(), 32);
        assert_eq!(bytes[8..12], [0xFF; 4]);
        assert_eq!(bytes[12..24], [0; 12]);
        assert_eq!(bytes[24], 1);
        assert_eq!(bytes[25..], [0; 7]);
    }

    #[test]
    fn no_data_takes_no_room() {
        let command = commands::Uniform1fv {
            location: 9,
            count: 0,
            value: DataSpan { offset: 16, size: 0 },
        };
        let bytes = record(CommandType::Uniform1fv, command.as_bytes(), InlineData::Bytes(&[]));
        assert_eq!(bytes.len(), 32);
        assert_eq!(bytes[..8], header(CommandType::Uniform1fv as u16, 24));
        assert_eq!(bytes[24..], [0; 8]);
    }

    #[test]
    fn a_string_is_followed_by_a_nul() {
        let name = InlineData::NulTerminated(b"position");
        assert_eq!(name.len(), 9);
        let command = commands::RequestExtensionANGLE {
            name: DataSpan::following::<commands::RequestExtensionANGLE>(name),
        };
        assert_eq!(command.name, DataSpan { offset: 16, size: 9 });
        let bytes = record(CommandType::RequestExtensionANGLE, command.as_bytes(), name);
        assert_eq!(bytes.len(), 48);
        assert_eq!(bytes[24..33], *b"position\0");
        assert_eq!(bytes[33..], [0; 15]);

        let empty_name = InlineData::NulTerminated(b"");
        let bytes = record(CommandType::RequestExtensionANGLE, command.as_bytes(), empty_name);
        assert_eq!(bytes.len(), 32);
        assert_eq!(bytes[24..], [0; 8]);
    }

    #[test]
    fn records_are_appended_one_after_the_other() {
        let mut commands = Vec::new();
        let enable = commands::Enable { cap: 1 };
        let viewport = commands::Viewport {
            x: 1,
            y: 2,
            width: 3,
            height: 4,
        };
        append_record(&mut commands, CommandType::Enable, enable.as_bytes(), InlineData::NONE);
        append_record(
            &mut commands,
            CommandType::Viewport,
            viewport.as_bytes(),
            InlineData::NONE,
        );
        assert_eq!(commands.len(), 16 + 32);
        assert_eq!(commands[..8], header(CommandType::Enable as u16, 8));
        assert_eq!(commands[16..24], header(CommandType::Viewport as u16, 24));
    }

    #[test]
    fn a_request_is_a_header_and_a_payload_padded_to_the_alignment() {
        // NB: The replayer rejects a request whose payload is shorter than its struct, and the struct of a request
        //     without arguments is one byte in C++.
        let mut request_bytes = vec![0xAA; 64];
        encode_sync_request(
            &mut request_bytes,
            &sync_calls::GetErrorRequest::ZEROED,
            InlineData::NONE,
        );
        assert_eq!(request_bytes.len(), 24);
        assert_eq!(request_bytes[..8], header(SyncCallType::GetError as u16, 16));
        assert_eq!(request_bytes[8..], [0; 16]);

        let name = InlineData::NulTerminated(b"color");
        let request = sync_calls::GetAttribLocationRequest {
            program: 3,
            name: DataSpan::following::<sync_calls::GetAttribLocationRequest>(name),
        };
        encode_sync_request(&mut request_bytes, &request, name);
        assert_eq!(request_bytes.len(), 8 + 32);
        assert_eq!(request_bytes[..8], header(SyncCallType::GetAttribLocation as u16, 32));
        assert_eq!(request_bytes[8..20], *request.as_bytes());
        assert_eq!(request_bytes[24..30], *b"color\0");
        assert_eq!(request_bytes[30..], [0; 10]);
    }

    #[test]
    fn a_reply_starts_with_its_struct() {
        let mut reply = Vec::new();
        reply.extend_from_slice(&5i32.to_ne_bytes());
        reply.extend_from_slice(&[16, 0, 0, 0, 3, 0, 0, 0]);
        reply.extend_from_slice(&[0; 4]);
        reply.extend_from_slice(b"abc");
        let decoded = sync_calls::GetShaderInfoLogReply::from_reply_bytes(&reply);
        assert_eq!(decoded.length, 5);
        assert_eq!(resolve_data_span(&reply, decoded.info_log), b"abc");
        assert_eq!(resolve_data_span(&reply, DataSpan { offset: 19, size: 0 }), b"");
    }

    #[test]
    #[should_panic(expected = "shorter than its struct")]
    fn a_reply_that_is_shorter_than_its_struct_is_not_read() {
        sync_calls::GetShaderInfoLogReply::from_reply_bytes(&[0; 11]);
    }

    #[test]
    #[should_panic(expected = "lies inside the reply")]
    fn a_span_that_leaves_the_reply_is_not_resolved() {
        resolve_data_span(&[0; 16], DataSpan { offset: 12, size: 5 });
    }
}
