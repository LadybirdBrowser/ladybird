/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The framing of the WebGL command stream: how a struct and the data that belongs to it become the bytes of a record
//! or of a synchronous call, and back.

use core::mem::size_of;
use core::ptr;

use super::vocabulary::{CommandType, SyncCallType};

/// Records start at multiples of this many bytes, and so does the data that follows a struct, counted from the start
/// of the struct.
pub const RECORD_ALIGNMENT: usize = 16;

/// What precedes the struct of a record and of a request: its type as a `u16`, two unused bytes, and the number of
/// bytes that follow the header as a `u32`.
pub const HEADER_SIZE: usize = 8;
pub const HEADER_PAYLOAD_SIZE_OFFSET: usize = 4;

/// `size` bytes that start `offset` bytes after the start of the struct that has the span as a field.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(C)]
pub struct DataSpan {
    pub offset: u32,
    pub size: u32,
}

impl DataSpan {
    pub const EMPTY: Self = Self { offset: 0, size: 0 };

    pub fn following<S: WireStruct>(data: InlineData<'_>) -> Self {
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
/// `BOOL_FIELD_OFFSETS` lists where its booleans are.
pub unsafe trait WireStruct: Copy + 'static {
    /// A struct is built from it, so that the bytes that the C ABI leaves unused between its fields, which are fields
    /// of their own here, do not have to be named.
    const ZEROED: Self;
    const BOOL_FIELD_OFFSETS: &'static [usize] = &[];

    fn as_bytes(&self) -> &[u8] {
        // SAFETY: Per the contract of the trait every byte of the struct is initialized.
        unsafe { ak::slice_from_raw_parts(ptr::from_ref(self).cast::<u8>(), size_of::<Self>()) }
    }

    /// The struct that a payload starts with. None where the payload is shorter than the struct, or where it has
    /// another byte than 0 or 1 for a boolean.
    fn read_from(payload: &[u8]) -> Option<Self> {
        if payload.len() < size_of::<Self>() || Self::BOOL_FIELD_OFFSETS.iter().any(|&offset| payload[offset] > 1) {
            return None;
        }
        // SAFETY: The bytes are as many as the struct has, and per the contract of the trait and the check above they
        //         are a value of it.
        Some(unsafe { ptr::read_unaligned(payload.as_ptr().cast::<Self>()) })
    }
}

pub trait Command: WireStruct {
    const TYPE: CommandType;
}

pub trait SyncRequest: WireStruct {
    const TYPE: SyncCallType;
    type Reply: SyncReply;
}

/// # Safety
///
/// Every bit pattern is a value of the type.
pub unsafe trait SyncReply: WireStruct {
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
pub enum InlineData<'a> {
    /// No bytes take no room at all, not even for the alignment of what would follow.
    Bytes(&'a [u8]),
    /// The stream has a NUL after the bytes.
    NulTerminated(&'a [u8]),
}

impl InlineData<'_> {
    pub const NONE: Self = Self::Bytes(&[]);

    pub fn len(self) -> usize {
        match self {
            Self::Bytes(bytes) => bytes.len(),
            Self::NulTerminated(bytes) => bytes.len() + 1,
        }
    }

    pub fn is_empty(self) -> bool {
        self.len() == 0
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

pub fn padded_record_size(struct_size: usize, inline_data_size: usize) -> Option<usize> {
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
        if !inline_data.is_empty() {
            let inline_data_offset = HEADER_SIZE + payload.len().next_multiple_of(RECORD_ALIGNMENT);
            ptr::write_bytes(destination.add(written), 0, inline_data_offset - written);
            inline_data.write_to(destination.add(inline_data_offset));
            written = inline_data_offset + inline_data.len();
        }
        ptr::write_bytes(destination.add(written), 0, size.size_with_header - written);
    }
}

pub struct Record<'a> {
    command_type: CommandType,
    payload: &'a [u8],
    inline_data: InlineData<'a>,
    size: FramedSize,
}

impl<'a> Record<'a> {
    pub fn new(command_type: CommandType, payload: &'a [u8], inline_data: InlineData<'a>) -> Option<Self> {
        Some(Self {
            command_type,
            payload,
            inline_data,
            size: FramedSize::of_record(payload.len(), inline_data.len())?,
        })
    }

    pub fn size(&self) -> usize {
        self.size.size_with_header
    }

    /// # Safety
    ///
    /// `destination` is valid for writes of `self.size()` bytes that overlap neither the struct nor the data.
    pub unsafe fn write_to(&self, destination: *mut u8) {
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

    pub fn append_to(&self, commands: &mut Vec<u8>) {
        let record_offset = commands.len();
        assert!(record_offset.is_multiple_of(RECORD_ALIGNMENT));
        commands.resize(record_offset + self.size(), 0);
        // SAFETY: The vector was just grown by the size of the record, and it owns its bytes.
        unsafe { self.write_to(commands.as_mut_ptr().add(record_offset)) };
    }

    pub fn encoded(&self) -> Vec<u8> {
        let mut bytes = Vec::new();
        self.append_to(&mut bytes);
        bytes
    }
}

/// False, with no request in `request_bytes`, when the stream cannot carry the request.
#[must_use]
pub fn encode_sync_request<R: SyncRequest>(
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

pub fn read_header(bytes: &[u8]) -> Option<(u16, usize)> {
    let header: &[u8; HEADER_SIZE] = bytes.first_chunk()?;
    let type_number = u16::from_ne_bytes([header[0], header[1]]);
    let size_after_header = u32::from_ne_bytes(*header[HEADER_PAYLOAD_SIZE_OFFSET..].first_chunk()?);
    Some((type_number, usize::try_from(size_after_header).ok()?))
}

/// The records of a stream of commands, each as its type and its payload.
pub fn records(bytes: &[u8]) -> Records<'_> {
    Records { bytes }
}

pub struct Records<'a> {
    bytes: &'a [u8],
}

impl<'a> Iterator for Records<'a> {
    type Item = Result<(CommandType, &'a [u8]), &'static str>;

    fn next(&mut self) -> Option<Self::Item> {
        if self.bytes.is_empty() {
            return None;
        }
        let bytes = core::mem::take(&mut self.bytes);
        let Some((type_number, payload_size)) = read_header(bytes) else {
            return Some(Err("Truncated WebGL command header"));
        };
        let Some(command_type) = CommandType::from_number(type_number) else {
            return Some(Err("Invalid WebGL command type"));
        };
        let Some((payload, rest)) = bytes[HEADER_SIZE..].split_at_checked(payload_size) else {
            return Some(Err("Truncated WebGL command payload"));
        };
        self.bytes = rest;
        Some(Ok((command_type, payload)))
    }
}

/// The type and the payload of a synchronous request.
pub fn decode_sync_request(bytes: &[u8]) -> Result<(SyncCallType, &[u8]), &'static str> {
    let (type_number, payload_size) = read_header(bytes).ok_or("Truncated WebGL sync call header")?;
    let call_type = SyncCallType::from_number(type_number).ok_or("Invalid WebGL sync call type")?;
    let payload = &bytes[HEADER_SIZE..];
    if payload.len() != payload_size {
        return Err("Truncated WebGL sync call payload");
    }
    Ok((call_type, payload))
}

/// Where the data that follows the struct of a reply is, as [`encode_reply`] lays it out.
pub fn reply_data_spans<R: WireStruct, const N: usize>(sizes: [usize; N]) -> [DataSpan; N] {
    let mut end_of_previous_data = size_of::<R>();
    sizes.map(|size| {
        let offset = end_of_previous_data.next_multiple_of(RECORD_ALIGNMENT);
        end_of_previous_data = offset + size;
        DataSpan {
            offset: u32::try_from(offset).expect("the data of a reply starts in its first 4 GiB"),
            size: u32::try_from(size).expect("the data of a reply is less than 4 GiB"),
        }
    })
}

/// A reply: its struct, and each piece of data that is not empty at the next multiple of the alignment, padded to the
/// alignment as a whole.
pub fn encode_reply<R: WireStruct>(reply: &R, data: &[&[u8]]) -> Vec<u8> {
    let mut bytes = reply.as_bytes().to_vec();
    for data in data.iter().filter(|data| !data.is_empty()) {
        bytes.resize(bytes.len().next_multiple_of(RECORD_ALIGNMENT), 0);
        bytes.extend_from_slice(data);
    }
    bytes.resize(bytes.len().next_multiple_of(RECORD_ALIGNMENT), 0);
    bytes
}

pub fn resolve_data_span(reply: &[u8], span: DataSpan) -> &[u8] {
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

    #[test]
    fn records_are_read_as_they_are_written() {
        let mut commands = Vec::new();
        let enable = commands::Enable { cap: 7 };
        let data = [1u8, 2, 3];
        let uniform = commands::Uniform1fv {
            location: 9,
            count: 1,
            value: DataSpan::following::<commands::Uniform1fv>(InlineData::Bytes(&data)),
        };
        Record::new(CommandType::Enable, enable.as_bytes(), InlineData::NONE)
            .unwrap()
            .append_to(&mut commands);
        Record::new(CommandType::Uniform1fv, uniform.as_bytes(), InlineData::Bytes(&data))
            .unwrap()
            .append_to(&mut commands);

        let read: Vec<_> = records(&commands).collect::<Result<_, _>>().unwrap();
        assert_eq!(read.len(), 2);
        assert_eq!(read[0].0, CommandType::Enable);
        assert_eq!(commands::Enable::read_from(read[0].1), Some(enable));
        assert_eq!(read[1].0, CommandType::Uniform1fv);
        assert_eq!(commands::Uniform1fv::read_from(read[1].1), Some(uniform));
        assert_eq!(resolve_data_span(read[1].1, uniform.value), data);
    }

    #[test]
    fn a_record_that_the_stream_does_not_hold_is_rejected() {
        assert_eq!(
            records(&header(0, 0)[..7]).next(),
            Some(Err("Truncated WebGL command header"))
        );
        assert_eq!(
            records(&header(u16::MAX, 0)).next(),
            Some(Err("Invalid WebGL command type"))
        );
        let mut truncated = header(CommandType::Enable as u16, 8);
        truncated.extend_from_slice(&[0; 7]);
        let mut read = records(&truncated);
        assert_eq!(read.next(), Some(Err("Truncated WebGL command payload")));
        assert_eq!(read.next(), None);
    }

    #[test]
    fn a_struct_is_read_only_with_booleans_that_are_true_or_false() {
        let command = commands::BufferData {
            has_data: true,
            ..commands::BufferData::ZEROED
        };
        let mut bytes = command.as_bytes().to_vec();
        assert_eq!(commands::BufferData::BOOL_FIELD_OFFSETS, [16]);
        assert_eq!(commands::BufferData::read_from(&bytes), Some(command));
        bytes[16] = 2;
        assert_eq!(commands::BufferData::read_from(&bytes), None);
        assert_eq!(commands::BufferData::read_from(&bytes[..31]), None);
    }

    #[test]
    fn a_request_is_read_as_it_is_written() {
        let name = InlineData::NulTerminated(b"color");
        let request = sync_calls::GetAttribLocationRequest {
            program: 3,
            name: DataSpan::following::<sync_calls::GetAttribLocationRequest>(name),
        };
        let mut request_bytes = Vec::new();
        assert!(encode_sync_request(&mut request_bytes, &request, name));
        let (call_type, payload) = decode_sync_request(&request_bytes).unwrap();
        assert_eq!(call_type, SyncCallType::GetAttribLocation);
        assert_eq!(sync_calls::GetAttribLocationRequest::read_from(payload), Some(request));
        assert_eq!(resolve_data_span(payload, request.name), b"color\0");

        request_bytes.push(0);
        assert_eq!(
            decode_sync_request(&request_bytes),
            Err("Truncated WebGL sync call payload")
        );
        assert_eq!(
            decode_sync_request(&header(u16::MAX, 0)),
            Err("Invalid WebGL sync call type")
        );
    }

    #[test]
    fn a_reply_holds_its_data_where_its_spans_say() {
        let info_log = b"abc";
        let [span] = reply_data_spans::<sync_calls::GetShaderInfoLogReply, 1>([info_log.len()]);
        assert_eq!(span, DataSpan { offset: 16, size: 3 });
        let reply = sync_calls::GetShaderInfoLogReply {
            length: 3,
            info_log: span,
        };
        let bytes = encode_reply(&reply, &[info_log]);
        assert_eq!(bytes.len(), 32);
        assert_eq!(sync_calls::GetShaderInfoLogReply::from_reply_bytes(&bytes), reply);
        assert_eq!(resolve_data_span(&bytes, span), info_log);
        assert_eq!(bytes[19..], [0; 13]);

        let [empty, second] = reply_data_spans::<sync_calls::GetShaderInfoLogReply, 2>([0, 5]);
        assert_eq!(empty, DataSpan { offset: 16, size: 0 });
        assert_eq!(second, DataSpan { offset: 16, size: 5 });
        let bytes = encode_reply(&reply, &[b"", b"hello"]);
        assert_eq!(resolve_data_span(&bytes, second), b"hello");
        assert_eq!(encode_reply(&sync_calls::GetErrorReply::ZEROED, &[]).len(), 16);
    }
}
