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
pub(crate) const HEADER_PAYLOAD_SIZE_OFFSET: usize = 4;

/// `size` bytes that start `offset` bytes after the start of the struct that has the span as a field.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(C)]
pub(crate) struct DataSpan {
    pub(crate) offset: u32,
    pub(crate) size: u32,
}

impl DataSpan {
    pub(crate) const EMPTY: Self = Self { offset: 0, size: 0 };

    pub(crate) fn following<S: WireStruct>(data: InlineData<'_>) -> Self {
        Self {
            offset: const { offset_of_data_following_struct(size_of::<S>()) },
            // NB: Data of 4 GiB or more has no span. It has no `FramedSize` either, so the recorder refuses what it
            //     would be sent in, and the size that stands in for the real one here is never sent.
            size: u32::try_from(data.len()).unwrap_or(u32::MAX),
        }
    }
}

const fn offset_of_data_following_struct(struct_size: usize) -> u32 {
    let offset = struct_size.next_multiple_of(RECORD_ALIGNMENT);
    assert!(offset <= u32::MAX as usize);
    offset as u32
}

/// # Safety
///
/// The type is `repr(C)`, made of numbers, booleans and [`DataSpan`]s, and has no byte that is not part of a field.
pub(crate) unsafe trait WireStruct: Copy + 'static {
    /// A struct is built from it, so that the bytes that the C ABI leaves unused between its fields, which are fields
    /// of their own here, do not have to be named.
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

fn payload_layout_size(struct_size: usize, inline_data_size: usize) -> Option<usize> {
    match inline_data_size {
        0 => Some(struct_size),
        inline_data_size => struct_size
            .checked_next_multiple_of(RECORD_ALIGNMENT)?
            .checked_add(inline_data_size),
    }
}

/// There is none for a struct with 4 GiB of data or more, as the header says in a `u32` how many bytes follow it.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct FramedSize {
    size_with_header: usize,
    size_after_header: u32,
}

impl FramedSize {
    fn with_size_after_header(size_after_header: usize) -> Option<Self> {
        Some(Self {
            size_with_header: HEADER_SIZE.checked_add(size_after_header)?,
            size_after_header: u32::try_from(size_after_header).ok()?,
        })
    }

    /// A record is padded to the alignment of records as a whole.
    fn of_record(struct_size: usize, inline_data_size: usize) -> Option<Self> {
        let size_with_header = HEADER_SIZE
            .checked_add(payload_layout_size(struct_size, inline_data_size)?)?
            .checked_next_multiple_of(RECORD_ALIGNMENT)?;
        Self::with_size_after_header(size_with_header - HEADER_SIZE)
    }

    /// What follows the header of a request is padded to the alignment of records.
    fn of_sync_request(struct_size: usize, inline_data_size: usize) -> Option<Self> {
        let size_after_header =
            payload_layout_size(struct_size, inline_data_size)?.checked_next_multiple_of(RECORD_ALIGNMENT)?;
        Self::with_size_after_header(size_after_header)
    }
}

pub(crate) fn padded_record_size(struct_size: usize, inline_data_size: usize) -> Option<usize> {
    FramedSize::of_record(struct_size, inline_data_size).map(|size| size.size_with_header)
}

/// Writes a header, a struct and its data, and zeroes what is left of the size, so that no byte of the stream is one
/// that happened to be in memory.
///
/// # Safety
///
/// `size` is that of the struct with the data, and `destination` is valid for writes of as many bytes, which overlap
/// neither the struct nor the data.
unsafe fn write_framed(
    destination: *mut u8,
    size: FramedSize,
    type_number: u16,
    payload: &[u8],
    inline_data: InlineData<'_>,
) {
    let mut header = [0; HEADER_SIZE];
    header[..2].copy_from_slice(&type_number.to_ne_bytes());
    header[HEADER_PAYLOAD_SIZE_OFFSET..].copy_from_slice(&size.size_after_header.to_ne_bytes());

    let mut written = HEADER_SIZE + payload.len();
    // SAFETY: The caller provides the bytes of the size, and every write below stays inside the layout that
    //         payload_layout_size() computes, which the size covers.
    unsafe {
        ptr::copy_nonoverlapping(header.as_ptr(), destination, HEADER_SIZE);
        ptr::copy_nonoverlapping(payload.as_ptr(), destination.add(HEADER_SIZE), payload.len());
        if inline_data.len() > 0 {
            let inline_data_offset = HEADER_SIZE + payload.len().next_multiple_of(RECORD_ALIGNMENT);
            ptr::write_bytes(destination.add(written), 0, inline_data_offset - written);
            inline_data.write_to(destination.add(inline_data_offset));
            written = inline_data_offset + inline_data.len();
        }
        ptr::write_bytes(destination.add(written), 0, size.size_with_header - written);
    }
}

pub(crate) struct Record<'a> {
    command_type: CommandType,
    payload: &'a [u8],
    inline_data: InlineData<'a>,
    size: FramedSize,
}

impl<'a> Record<'a> {
    pub(crate) fn new(command_type: CommandType, payload: &'a [u8], inline_data: InlineData<'a>) -> Option<Self> {
        Some(Self {
            command_type,
            payload,
            inline_data,
            size: FramedSize::of_record(payload.len(), inline_data.len())?,
        })
    }

    pub(crate) fn size(&self) -> usize {
        self.size.size_with_header
    }

    /// # Safety
    ///
    /// `destination` is valid for writes of `self.size()` bytes that overlap neither the struct nor the data.
    pub(crate) unsafe fn write_to(&self, destination: *mut u8) {
        // SAFETY: The caller provides the bytes of the record, whose size is that of the struct with the data.
        unsafe {
            write_framed(
                destination,
                self.size,
                self.command_type as u16,
                self.payload,
                self.inline_data,
            );
        }
    }

    pub(crate) fn append_to(&self, commands: &mut Vec<u8>) {
        let record_offset = commands.len();
        assert!(record_offset.is_multiple_of(RECORD_ALIGNMENT));
        commands.resize(record_offset + self.size(), 0);
        // SAFETY: The vector was just grown by the size of the record, and it owns its bytes.
        unsafe { self.write_to(commands.as_mut_ptr().add(record_offset)) };
    }

    pub(crate) fn encoded(&self) -> Vec<u8> {
        let mut bytes = Vec::new();
        self.append_to(&mut bytes);
        bytes
    }
}

/// False, with no request in `request_bytes`, when the stream cannot carry the request.
#[must_use]
pub(crate) fn encode_sync_request<R: SyncRequest>(
    request_bytes: &mut Vec<u8>,
    request: &R,
    inline_data: InlineData<'_>,
) -> bool {
    request_bytes.clear();
    let Some(size) = FramedSize::of_sync_request(size_of::<R>(), inline_data.len()) else {
        return false;
    };
    request_bytes.resize(size.size_with_header, 0);
    // SAFETY: The vector has the size of the request, and it owns its bytes.
    unsafe {
        write_framed(
            request_bytes.as_mut_ptr(),
            size,
            R::TYPE as u16,
            request.as_bytes(),
            inline_data,
        );
    }
    true
}

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
        let record = Record::new(command_type, payload, inline_data).unwrap();
        assert_eq!(
            Some(record.size()),
            padded_record_size(payload.len(), inline_data.len())
        );
        // NB: The bytes that a record is written over are not zero, as those of a reused buffer are not.
        let mut bytes = vec![0xAA; record.size()];
        // SAFETY: The vector has the size of the record.
        unsafe { record.write_to(bytes.as_mut_ptr()) };
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
        Record::new(CommandType::Enable, enable.as_bytes(), InlineData::NONE)
            .unwrap()
            .append_to(&mut commands);
        Record::new(CommandType::Viewport, viewport.as_bytes(), InlineData::NONE)
            .unwrap()
            .append_to(&mut commands);
        assert_eq!(commands.len(), 16 + 32);
        assert_eq!(commands[..8], header(CommandType::Enable as u16, 8));
        assert_eq!(commands[16..24], header(CommandType::Viewport as u16, 24));
    }

    #[test]
    fn a_request_is_a_header_and_a_payload_padded_to_the_alignment() {
        // NB: The replayer rejects a request whose payload is shorter than its struct, and the struct of a request
        //     without arguments is one byte in C++.
        let mut request_bytes = vec![0xAA; 64];
        assert!(encode_sync_request(
            &mut request_bytes,
            &sync_calls::GetErrorRequest::ZEROED,
            InlineData::NONE,
        ));
        assert_eq!(request_bytes.len(), 24);
        assert_eq!(request_bytes[..8], header(SyncCallType::GetError as u16, 16));
        assert_eq!(request_bytes[8..], [0; 16]);

        let name = InlineData::NulTerminated(b"color");
        let request = sync_calls::GetAttribLocationRequest {
            program: 3,
            name: DataSpan::following::<sync_calls::GetAttribLocationRequest>(name),
        };
        assert!(encode_sync_request(&mut request_bytes, &request, name));
        assert_eq!(request_bytes.len(), 8 + 32);
        assert_eq!(request_bytes[..8], header(SyncCallType::GetAttribLocation as u16, 32));
        assert_eq!(request_bytes[8..20], *request.as_bytes());
        assert_eq!(request_bytes[24..30], *b"color\0");
        assert_eq!(request_bytes[30..], [0; 10]);
    }

    #[test]
    fn the_stream_carries_what_its_header_can_count() {
        const FOUR_GIB: usize = 1 << 32;

        // The largest record: 8 bytes of header, 16 of struct, and data up to the last multiple of the alignment
        // that leaves what follows the header countable.
        let largest_data_size = FOUR_GIB - 8 - 16;
        assert_eq!(padded_record_size(16, largest_data_size), Some(FOUR_GIB));
        assert_eq!(
            FramedSize::of_record(16, largest_data_size),
            Some(FramedSize {
                size_with_header: FOUR_GIB,
                size_after_header: u32::MAX - 7
            })
        );
        assert_eq!(padded_record_size(16, largest_data_size + 1), None);
        assert_eq!(padded_record_size(16, FOUR_GIB), None);
        assert_eq!(padded_record_size(16, usize::MAX), None);
        assert_eq!(padded_record_size(16, usize::MAX - 16), None);
        assert_eq!(padded_record_size(usize::MAX, 0), None);
        assert_eq!(padded_record_size(usize::MAX, 1), None);

        // The largest request: what follows its header is padded on its own.
        assert_eq!(
            FramedSize::of_sync_request(16, FOUR_GIB - 16 - 16),
            Some(FramedSize {
                size_with_header: FOUR_GIB - 8,
                size_after_header: u32::MAX - 15
            })
        );
        assert_eq!(FramedSize::of_sync_request(16, FOUR_GIB - 16 - 15), None);
        assert_eq!(FramedSize::of_sync_request(16, usize::MAX), None);
        assert_eq!(FramedSize::of_sync_request(1, 0).unwrap().size_with_header, 8 + 16);
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
