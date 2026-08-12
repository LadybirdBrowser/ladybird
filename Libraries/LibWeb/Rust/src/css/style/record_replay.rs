/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The versioned event stream shared by StyleEngine recording and replay.
//!
//! Each event is independently framed and checksummed. A schema change therefore fails at the
//! first event the replayer cannot interpret instead of silently shifting the remainder of a large
//! capture. Event payloads use explicit little-endian fixed-width fields; process-local pointers
//! never enter the format.
//!
//! Every payload starts at a file offset that is a multiple of eight (the file header and event
//! headers are multiples of eight, and each payload is tail-padded to eight). Pointer-free
//! `repr(C)` row arrays are therefore stored as their raw in-memory bytes, aligned relative to the
//! payload, and a replay over a page-aligned memory mapping consumes them in place without
//! copying. Raw arrays make a capture host-native: logs do not port across architectures.

use super::fnv::fnv1a64;
use std::collections::HashMap;
use std::collections::HashSet;
use std::fmt;
use std::fs::File;
use std::fs::OpenOptions;
use std::io;
use std::io::Read;
use std::io::Write;
use std::sync::Mutex;
use std::sync::OnceLock;

const MAGIC: [u8; 8] = *b"SGREPLAY";
const FORMAT_VERSION: u64 = 2;
const EVENT_HEADER_SIZE: usize = 3 * size_of::<u64>();
const PAYLOAD_ALIGNMENT: usize = 8;

/// A pointer-free `repr(C)` row type whose arrays are recorded as raw in-memory bytes and read
/// back in place from a mapped log.
///
/// # Safety
/// Implementors must be `repr(C)` with alignment at most eight and contain no pointers, and every
/// value the recorder writes must be valid to read back as `Self` on the capturing host (enum
/// fields are recorded only from live values, so their discriminants round-trip). Interior struct
/// padding is written as-is and ignored on read.
pub unsafe trait RawRecord: Copy {}

// Stable operation identifiers in the on-disk stream (EventKind). Values are never reused.
// Removing an operation leaves a hole so an old capture cannot be mistaken for a new operation
// with the same payload shape.
include!(concat!(env!("OUT_DIR"), "/style_engine_event_kind_generated.rs"));

#[derive(Debug)]
pub enum Error {
    Io(io::Error),
    InvalidMagic,
    UnsupportedVersion(u64),
    UnknownEvent(u64),
    EventTooLarge(u64),
    CorruptEvent { expected: u64, actual: u64 },
    TruncatedPayload,
    TrailingPayload { remaining: usize },
    InvalidBoolean(u8),
    InvalidTag { category: &'static str, value: u8 },
    LengthOutOfRange(u64),
    MisalignedRawSlice,
}

impl fmt::Display for Error {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Io(error) => write!(formatter, "{error}"),
            Self::InvalidMagic => formatter.write_str("not a StyleEngine replay log"),
            Self::UnsupportedVersion(version) => {
                write!(formatter, "unsupported StyleEngine replay format version {version}")
            }
            Self::UnknownEvent(kind) => write!(formatter, "unknown StyleEngine replay event {kind}"),
            Self::EventTooLarge(size) => write!(formatter, "StyleEngine replay event is too large: {size} bytes"),
            Self::CorruptEvent { expected, actual } => write!(
                formatter,
                "corrupt StyleEngine replay event: expected checksum {expected:#018x}, got {actual:#018x}"
            ),
            Self::TruncatedPayload => formatter.write_str("truncated StyleEngine replay event payload"),
            Self::TrailingPayload { remaining } => {
                write!(
                    formatter,
                    "StyleEngine replay event has {remaining} trailing payload bytes"
                )
            }
            Self::InvalidBoolean(value) => write!(formatter, "invalid StyleEngine replay boolean {value}"),
            Self::InvalidTag { category, value } => {
                write!(formatter, "invalid StyleEngine replay {category} tag {value}")
            }
            Self::LengthOutOfRange(length) => {
                write!(formatter, "StyleEngine replay length does not fit this host: {length}")
            }
            Self::MisalignedRawSlice => {
                formatter.write_str("StyleEngine replay raw array is not aligned; raw arrays require a mapped log")
            }
        }
    }
}

impl std::error::Error for Error {}

impl From<io::Error> for Error {
    fn from(error: io::Error) -> Self {
        Self::Io(error)
    }
}

pub struct Event<'a> {
    pub kind: EventKind,
    pub payload: PayloadReader<'a>,
}

pub struct LogWriter<W> {
    output: W,
}

impl<W: Write> LogWriter<W> {
    pub fn new(mut output: W) -> Result<Self, Error> {
        output.write_all(&MAGIC)?;
        output.write_all(&FORMAT_VERSION.to_le_bytes())?;
        Ok(Self { output })
    }

    pub fn write_event(&mut self, kind: EventKind, payload: &PayloadWriter) -> Result<(), Error> {
        let payload_length = u64::try_from(payload.bytes.len()).map_err(|_| Error::EventTooLarge(u64::MAX))?;
        self.output.write_all(&(kind as u16 as u64).to_le_bytes())?;
        self.output.write_all(&payload_length.to_le_bytes())?;
        self.output.write_all(&digest(&payload.bytes).to_le_bytes())?;
        self.output.write_all(&payload.bytes)?;
        let tail_padding = payload.bytes.len().next_multiple_of(PAYLOAD_ALIGNMENT) - payload.bytes.len();
        self.output.write_all(&[0; PAYLOAD_ALIGNMENT][..tail_padding])?;
        Ok(())
    }

    pub fn flush(&mut self) -> Result<(), Error> {
        self.output.flush().map_err(Error::Io)
    }
}

pub struct LogReader<R> {
    input: R,
    #[cfg(test)]
    payload: Vec<u8>,
    verify_checksums: bool,
}

impl<R: Read> LogReader<R> {
    pub fn new(mut input: R) -> Result<Self, Error> {
        let mut magic = [0; MAGIC.len()];
        input.read_exact(&mut magic)?;
        if magic != MAGIC {
            return Err(Error::InvalidMagic);
        }
        let mut version_bytes = [0; size_of::<u64>()];
        input.read_exact(&mut version_bytes)?;
        let version = u64::from_le_bytes(version_bytes);
        if version != FORMAT_VERSION {
            // NB: Format 1 wrote its version as a u32, so this u64 read swallows part of the first
            //     event header; masking recovers the real number for the error message.
            let reported = if version >> 32 != 0 {
                version & 0xffff_ffff
            } else {
                version
            };
            return Err(Error::UnsupportedVersion(reported));
        }
        Ok(Self {
            input,
            #[cfg(test)]
            payload: Vec::new(),
            verify_checksums: true,
        })
    }

    /// Disables per-event checksum verification. The checksum pass touches every payload byte, so
    /// timing-focused replays skip it; correctness-focused replays keep the default.
    pub fn set_verify_checksums(&mut self, verify: bool) {
        self.verify_checksums = verify;
    }

    /// Reads the next event. The returned event's payload borrows this reader's scratch buffer,
    /// which the next `read_event` call reuses, so each event must be consumed before the next.
    #[cfg(test)]
    pub fn read_event(&mut self) -> Result<Option<Event<'_>>, Error> {
        let mut header = [0; EVENT_HEADER_SIZE];
        let mut bytes_read = 0;
        while bytes_read < header.len() {
            match self.input.read(&mut header[bytes_read..])? {
                0 if bytes_read == 0 => return Ok(None),
                0 => return Err(Error::Io(io::Error::from(io::ErrorKind::UnexpectedEof))),
                count => bytes_read += count,
            }
        }

        let (kind, payload_length, expected_digest) = decode_event_header(header)?;
        if payload_length > self.payload.len() {
            self.payload.resize(payload_length, 0);
        }
        let payload = &mut self.payload[..payload_length];
        self.input.read_exact(payload)?;
        let tail_padding = payload_length.next_multiple_of(PAYLOAD_ALIGNMENT) - payload_length;
        self.input.read_exact(&mut [0; PAYLOAD_ALIGNMENT][..tail_padding])?;
        verify_payload(payload, expected_digest, self.verify_checksums)?;
        Ok(Some(Event {
            kind,
            payload: PayloadReader::new(&self.payload[..payload_length]),
        }))
    }
}

impl<'d> LogReader<&'d [u8]> {
    /// Reads the next event without copying: the payload borrows the input slice itself, so with a
    /// memory-mapped log the decode loop never touches a scratch buffer.
    #[inline]
    pub fn read_event_borrowed(&mut self) -> Result<Option<Event<'d>>, Error> {
        self.read_event_borrowed_impl(false).map(|(_, event)| event)
    }

    /// Reads the next event while consuming legacy immutable-style-record markers in the same
    /// header decode pass. Returns the number of markers skipped before the event.
    #[inline]
    pub fn read_event_borrowed_skipping_redundant_style_reads(&mut self) -> Result<(u64, Option<Event<'d>>), Error> {
        self.read_event_borrowed_impl(true)
    }

    fn read_event_borrowed_impl(
        &mut self,
        skip_redundant_style_reads: bool,
    ) -> Result<(u64, Option<Event<'d>>), Error> {
        let mut skipped = 0;
        loop {
            if self.input.is_empty() {
                return Ok((skipped, None));
            }
            if self.input.len() < EVENT_HEADER_SIZE {
                return Err(Error::Io(io::Error::from(io::ErrorKind::UnexpectedEof)));
            }
            let (kind, payload_length, expected_digest) =
                decode_event_header(self.input[..EVENT_HEADER_SIZE].try_into().unwrap())?;
            let rest = &self.input[EVENT_HEADER_SIZE..];
            let padded_length = payload_length.next_multiple_of(PAYLOAD_ALIGNMENT);
            if rest.len() < padded_length {
                return Err(Error::Io(io::Error::from(io::ErrorKind::UnexpectedEof)));
            }
            let payload = &rest[..payload_length];
            self.input = &rest[padded_length..];
            if skip_redundant_style_reads
                && payload_length == 17
                && payload[16] == 0
                && matches!(kind, EventKind::StyleRecordPayloads | EventKind::StyleRecordView)
            {
                skipped += 1;
                continue;
            }
            verify_payload(payload, expected_digest, self.verify_checksums)?;
            return Ok((
                skipped,
                Some(Event {
                    kind,
                    payload: PayloadReader::new(payload),
                }),
            ));
        }
    }
}

fn decode_event_header(header: [u8; EVENT_HEADER_SIZE]) -> Result<(EventKind, usize, u64), Error> {
    let kind = EventKind::decode(u64::from_le_bytes(header[0..8].try_into().unwrap()))?;
    let payload_length = u64::from_le_bytes(header[8..16].try_into().unwrap());
    let expected_digest = u64::from_le_bytes(header[16..24].try_into().unwrap());
    let payload_length = usize::try_from(payload_length).map_err(|_| Error::EventTooLarge(payload_length))?;
    Ok((kind, payload_length, expected_digest))
}

fn verify_payload(payload: &[u8], expected_digest: u64, verify: bool) -> Result<(), Error> {
    if !verify {
        return Ok(());
    }
    let actual_digest = digest(payload);
    if actual_digest != expected_digest {
        return Err(Error::CorruptEvent {
            expected: expected_digest,
            actual: actual_digest,
        });
    }
    Ok(())
}

#[derive(Default)]
pub struct PayloadWriter {
    bytes: Vec<u8>,
}

impl PayloadWriter {
    pub fn write_bool(&mut self, value: bool) {
        self.write_u8(u8::from(value));
    }

    pub fn write_u8(&mut self, value: u8) {
        self.bytes.push(value);
    }

    pub fn write_u16(&mut self, value: u16) {
        self.bytes.extend_from_slice(&value.to_le_bytes());
    }

    pub fn write_u32(&mut self, value: u32) {
        self.bytes.extend_from_slice(&value.to_le_bytes());
    }

    pub fn write_native_u16(&mut self, value: u16) {
        self.bytes.extend_from_slice(&value.to_ne_bytes());
    }

    pub fn write_native_u32(&mut self, value: u32) {
        self.bytes.extend_from_slice(&value.to_ne_bytes());
    }

    pub fn write_u64(&mut self, value: u64) {
        self.bytes.extend_from_slice(&value.to_le_bytes());
    }

    pub fn write_length(&mut self, value: usize) {
        self.write_u64(u64::try_from(value).expect("replay payload length exceeds u64"));
    }

    pub fn write_i32(&mut self, value: i32) {
        self.bytes.extend_from_slice(&value.to_le_bytes());
    }

    pub fn write_bytes(&mut self, bytes: &[u8]) {
        self.write_length(bytes.len());
        self.bytes.extend_from_slice(bytes);
    }

    pub fn write_u16_slice(&mut self, values: &[u16]) {
        self.write_length(values.len());
        for value in values {
            self.write_u16(*value);
        }
    }

    pub fn write_u32_slice(&mut self, values: &[u32]) {
        self.write_length(values.len());
        for value in values {
            self.write_u32(*value);
        }
    }

    /// Writes `values` as their raw in-memory bytes, padded so the array's offset within the
    /// payload is a multiple of the row alignment. Payloads start eight-aligned in the file, so a
    /// replay over a page-aligned mapping reads the array back in place.
    pub fn write_raw_slice<T: RawRecord>(&mut self, values: &[T]) {
        self.write_length(values.len());
        let position = self.bytes.len() + 1;
        let padding = position.next_multiple_of(align_of::<T>()) - position;
        self.write_u8(u8::try_from(padding).expect("raw array alignment padding exceeds u8"));
        self.bytes.extend(std::iter::repeat_n(0, padding));
        // SAFETY: RawRecord rows are plain repr(C) values; their bytes (including any interior
        // struct padding, which the reader ignores) are recorded verbatim.
        let bytes = unsafe { std::slice::from_raw_parts(values.as_ptr().cast::<u8>(), size_of_val(values)) };
        self.bytes.extend_from_slice(bytes);
    }

    pub fn write_raw_rows(
        &mut self,
        count: usize,
        row_size: usize,
        row_alignment: usize,
        write_rows: impl FnOnce(&mut Self),
    ) {
        assert!(row_alignment.is_power_of_two());
        self.write_length(count);
        let position = self.bytes.len() + 1;
        let padding = position.next_multiple_of(row_alignment) - position;
        self.write_u8(u8::try_from(padding).expect("raw array alignment padding exceeds u8"));
        self.bytes.extend(std::iter::repeat_n(0, padding));
        let row_start = self.bytes.len();
        write_rows(self);
        let expected_bytes = count
            .checked_mul(row_size)
            .expect("raw array byte length exceeds usize");
        assert_eq!(self.bytes.len() - row_start, expected_bytes);
    }

    pub fn as_bytes(&self) -> &[u8] {
        &self.bytes
    }

    pub fn stable_digest(&self) -> u64 {
        digest(&self.bytes)
    }
}

pub struct PayloadReader<'a> {
    bytes: &'a [u8],
    cursor: usize,
}

struct Capture {
    writer: LogWriter<File>,
    next_engine_id: u64,
    pointer_tokens: PointerTokenRegistry,
    recorded_atom_bounds: HashMap<u64, u32>,
    recorded_responses: HashSet<(u64, u8, u64)>,
}

#[derive(Default)]
struct PointerTokenRegistry {
    next_token: u64,
    tokens: HashMap<(u64, usize), u64>,
    engine_ids_by_pointer: HashMap<usize, Vec<u64>>,
}

impl PointerTokenRegistry {
    fn token(&mut self, engine_id: u64, pointer: usize) -> u64 {
        if let Some(&token) = self.tokens.get(&(engine_id, pointer)) {
            return token;
        }
        let token = self.next_token.max(1);
        self.next_token = token
            .checked_add(1)
            .expect("StyleEngine replay pointer token space exhausted");
        self.tokens.insert((engine_id, pointer), token);
        self.engine_ids_by_pointer.entry(pointer).or_default().push(engine_id);
        token
    }

    fn invalidate(&mut self, pointer: usize) {
        let Some(engine_ids) = self.engine_ids_by_pointer.remove(&pointer) else {
            return;
        };
        for engine_id in engine_ids {
            self.tokens.remove(&(engine_id, pointer));
        }
    }
}

static CAPTURE: OnceLock<Option<Mutex<Capture>>> = OnceLock::new();

/// Starts recording one document engine when `LIBWEB_STYLE_RECORD` names a new log file.
///
/// The environment is inspected once per process. A missing switch leaves only the returned
/// `None` in each engine, so subsequent boundary calls perform no global lookup, allocation, lock,
/// or serialization work.
pub(super) fn begin_recording_stream(device_class: u8) -> Option<u64> {
    let capture = CAPTURE.get_or_init(|| {
        let configured_path = std::env::var_os("LIBWEB_STYLE_RECORD")?;
        let path = configured_path
            .to_string_lossy()
            .replace("%p", &std::process::id().to_string());
        let file = OpenOptions::new()
            .write(true)
            .create_new(true)
            .open(&path)
            .unwrap_or_else(|error| panic!("cannot create StyleEngine replay log {path}: {error}"));
        let writer = LogWriter::new(file).expect("cannot write StyleEngine replay log header");
        Some(Mutex::new(Capture {
            writer,
            next_engine_id: 1,
            pointer_tokens: PointerTokenRegistry::default(),
            recorded_atom_bounds: HashMap::new(),
            recorded_responses: HashSet::new(),
        }))
    });
    let capture = capture.as_ref()?;
    let mut capture = capture.lock().expect("StyleEngine replay recorder lock is poisoned");
    let engine_id = capture.next_engine_id;
    capture.next_engine_id = capture
        .next_engine_id
        .checked_add(1)
        .expect("StyleEngine replay engine identity space exhausted");
    let mut payload = PayloadWriter::default();
    payload.write_u64(engine_id);
    payload.write_u8(device_class);
    payload.write_u8(super::verification_gate_bits());
    capture
        .writer
        .write_event(EventKind::CreateGraph, &payload)
        .expect("cannot record StyleEngine creation");
    Some(engine_id)
}

pub(super) fn pointer_token(engine_id: u64, pointer: usize) -> u64 {
    if pointer == 0 {
        return 0;
    }
    let capture = CAPTURE
        .get()
        .and_then(Option::as_ref)
        .expect("a recording engine must have a capture session");
    capture
        .lock()
        .expect("StyleEngine replay recorder lock is poisoned")
        .pointer_tokens
        .token(engine_id, pointer)
}

pub(crate) fn invalidate_pointer(pointer: usize) {
    let Some(capture) = CAPTURE.get().and_then(Option::as_ref) else {
        return;
    };
    capture
        .lock()
        .expect("StyleEngine replay recorder lock is poisoned")
        .pointer_tokens
        .invalidate(pointer);
}

pub(super) fn first_response(engine_id: u64, category: u8, identity: u64) -> bool {
    let capture = CAPTURE
        .get()
        .and_then(Option::as_ref)
        .expect("a recording engine must have a capture session");
    capture
        .lock()
        .expect("StyleEngine replay recorder lock is poisoned")
        .recorded_responses
        .insert((engine_id, category, identity))
}

pub(super) fn take_unrecorded_atom_bound(engine_id: u64, new_bound: u32) -> u32 {
    let capture = CAPTURE
        .get()
        .and_then(Option::as_ref)
        .expect("a recording engine must have a capture session");
    let mut capture = capture.lock().expect("StyleEngine replay recorder lock is poisoned");
    let previous = capture.recorded_atom_bounds.insert(engine_id, new_bound).unwrap_or(0);
    assert!(new_bound >= previous, "StyleEngine atom identities must be monotone");
    previous
}

/// Records the end of one document engine and makes every preceding event durable.
pub(super) fn end_recording_stream(engine_id: Option<u64>) {
    let Some(engine_id) = engine_id else {
        return;
    };
    let capture = CAPTURE
        .get()
        .and_then(Option::as_ref)
        .expect("a recording engine must have a capture session");
    let mut capture = capture.lock().expect("StyleEngine replay recorder lock is poisoned");
    let mut payload = PayloadWriter::default();
    payload.write_u64(engine_id);
    capture
        .writer
        .write_event(EventKind::DestroyGraph, &payload)
        .expect("cannot record StyleEngine destruction");
    capture.writer.flush().expect("cannot flush StyleEngine replay log");
}

pub(super) fn record_engine_event(engine_id: u64, kind: EventKind, write_payload: impl FnOnce(&mut PayloadWriter)) {
    let capture = CAPTURE
        .get()
        .and_then(Option::as_ref)
        .expect("a recording engine must have a capture session");
    let mut payload = PayloadWriter::default();
    payload.write_u64(engine_id);
    write_payload(&mut payload);
    capture
        .lock()
        .expect("StyleEngine replay recorder lock is poisoned")
        .writer
        .write_event(kind, &payload)
        .expect("cannot record StyleEngine boundary event");
}

impl<'a> PayloadReader<'a> {
    pub fn remaining_bytes(&self) -> usize {
        self.bytes.len() - self.cursor
    }

    pub fn new(bytes: &'a [u8]) -> Self {
        Self { bytes, cursor: 0 }
    }

    #[inline]
    pub fn read_bool(&mut self) -> Result<bool, Error> {
        match self.read_u8()? {
            0 => Ok(false),
            1 => Ok(true),
            value => Err(Error::InvalidBoolean(value)),
        }
    }

    #[inline]
    pub fn read_u8(&mut self) -> Result<u8, Error> {
        Ok(self.take(size_of::<u8>())?[0])
    }

    #[inline]
    pub fn read_u16(&mut self) -> Result<u16, Error> {
        Ok(u16::from_le_bytes(self.take(size_of::<u16>())?.try_into().unwrap()))
    }

    #[inline]
    pub fn read_u32(&mut self) -> Result<u32, Error> {
        Ok(u32::from_le_bytes(self.take(size_of::<u32>())?.try_into().unwrap()))
    }

    #[inline]
    pub fn read_u64(&mut self) -> Result<u64, Error> {
        Ok(u64::from_le_bytes(self.take(size_of::<u64>())?.try_into().unwrap()))
    }

    pub fn read_i32(&mut self) -> Result<i32, Error> {
        Ok(i32::from_le_bytes(self.take(size_of::<i32>())?.try_into().unwrap()))
    }

    pub fn read_bytes(&mut self) -> Result<&'a [u8], Error> {
        let length = self.read_length()?;
        self.take(length)
    }

    /// Reads a length-prefixed array's encoded bytes in place. Callers decode each fixed-width
    /// element from the returned mapping-backed slice instead of allocating an owned array.
    pub fn read_fixed_width_slice(&mut self, element_size: usize) -> Result<&'a [u8], Error> {
        let length = self.read_length()?;
        let byte_length = length.checked_mul(element_size).ok_or(Error::TruncatedPayload)?;
        self.take(byte_length)
    }

    pub fn read_u16_vec(&mut self) -> Result<Vec<u16>, Error> {
        let bytes = self.read_fixed_width_slice(size_of::<u16>())?;
        Ok(bytes
            .chunks_exact(size_of::<u16>())
            .map(|chunk| u16::from_le_bytes(chunk.try_into().unwrap()))
            .collect())
    }

    pub fn read_u32_vec(&mut self) -> Result<Vec<u32>, Error> {
        let bytes = self.read_fixed_width_slice(size_of::<u32>())?;
        Ok(bytes
            .chunks_exact(size_of::<u32>())
            .map(|chunk| u32::from_le_bytes(chunk.try_into().unwrap()))
            .collect())
    }

    /// Reads a raw row array in place, without copying. The rows are only aligned when this
    /// payload sits in a page-aligned mapping of the log, which the alignment check enforces.
    pub fn read_raw_slice<T: RawRecord>(&mut self) -> Result<&'a [T], Error> {
        let length = self.read_length()?;
        let padding = usize::from(self.read_u8()?);
        self.take(padding)?;
        let byte_length = length.checked_mul(size_of::<T>()).ok_or(Error::TruncatedPayload)?;
        let bytes = self.take(byte_length)?;
        if bytes.as_ptr() as usize % align_of::<T>() != 0 {
            return Err(Error::MisalignedRawSlice);
        }
        // SAFETY: the alignment was checked above, the length was bounds-checked by take(), and
        // RawRecord guarantees every recorded bit pattern is valid to read back as T.
        Ok(unsafe { std::slice::from_raw_parts(bytes.as_ptr().cast::<T>(), length) })
    }

    pub fn finish(self) -> Result<(), Error> {
        let remaining = self.bytes.len() - self.cursor;
        if remaining == 0 {
            Ok(())
        } else {
            Err(Error::TrailingPayload { remaining })
        }
    }

    #[inline]
    pub fn read_length(&mut self) -> Result<usize, Error> {
        let length = self.read_u64()?;
        usize::try_from(length).map_err(|_| Error::LengthOutOfRange(length))
    }

    #[inline]
    fn take(&mut self, count: usize) -> Result<&'a [u8], Error> {
        let end = self.cursor.checked_add(count).ok_or(Error::TruncatedPayload)?;
        let bytes = self.bytes.get(self.cursor..end).ok_or(Error::TruncatedPayload)?;
        self.cursor = end;
        Ok(bytes)
    }
}

fn digest(bytes: &[u8]) -> u64 {
    fnv1a64(bytes.iter().copied())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn generated_event_vocabulary_round_trips() {
        for &kind in ALL_EVENT_KINDS {
            assert_eq!(EventKind::decode(kind as u64).unwrap(), kind);
        }
        assert!(matches!(EventKind::decode(0), Err(Error::UnknownEvent(0))));
        assert!(matches!(
            EventKind::decode(u64::MAX),
            Err(Error::UnknownEvent(u64::MAX))
        ));
    }
    use std::io::Cursor;

    #[test]
    fn event_round_trip() {
        let mut output = Vec::new();
        let mut writer = LogWriter::new(&mut output).unwrap();
        let mut payload = PayloadWriter::default();
        payload.write_bool(true);
        payload.write_u8(42);
        payload.write_u16(0x1234);
        payload.write_u32(0x1234_5678);
        payload.write_u64(0x1234_5678_9abc_def0);
        payload.write_i32(-123);
        payload.write_bytes(b"StyleEngine");
        payload.write_u16_slice(&[1, 2, 3]);
        payload.write_u32_slice(&[4, 5, 6]);
        payload.write_length(2);
        payload.write_u64(0x0102_0304_0506_0708);
        payload.write_u64(0x1112_1314_1516_1718);
        writer.write_event(EventKind::CreateGraph, &payload).unwrap();
        writer.flush().unwrap();

        let mut reader = LogReader::new(Cursor::new(output)).unwrap();
        let mut event = reader.read_event().unwrap().unwrap();
        assert_eq!(event.kind, EventKind::CreateGraph);
        assert!(event.payload.read_bool().unwrap());
        assert_eq!(event.payload.read_u8().unwrap(), 42);
        assert_eq!(event.payload.read_u16().unwrap(), 0x1234);
        assert_eq!(event.payload.read_u32().unwrap(), 0x1234_5678);
        assert_eq!(event.payload.read_u64().unwrap(), 0x1234_5678_9abc_def0);
        assert_eq!(event.payload.read_i32().unwrap(), -123);
        assert_eq!(event.payload.read_bytes().unwrap(), b"StyleEngine");
        assert_eq!(event.payload.read_u16_vec().unwrap(), [1, 2, 3]);
        assert_eq!(event.payload.read_u32_vec().unwrap(), [4, 5, 6]);
        assert_eq!(
            event.payload.read_fixed_width_slice(size_of::<u64>()).unwrap(),
            [
                0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11,
            ]
        );
        event.payload.finish().unwrap();
        assert!(reader.read_event().unwrap().is_none());
    }

    #[test]
    fn borrowed_event_round_trip() {
        let mut output = Vec::new();
        let mut writer = LogWriter::new(&mut output).unwrap();
        let mut payload = PayloadWriter::default();
        payload.write_u64(7);
        payload.write_bytes(b"mapped");
        writer.write_event(EventKind::CreateGraph, &payload).unwrap();
        writer.flush().unwrap();

        let mut reader = LogReader::new(output.as_slice()).unwrap();
        let mut event = reader.read_event_borrowed().unwrap().unwrap();
        assert_eq!(event.kind, EventKind::CreateGraph);
        assert_eq!(event.payload.read_u64().unwrap(), 7);
        assert_eq!(event.payload.read_bytes().unwrap(), b"mapped");
        event.payload.finish().unwrap();
        assert!(reader.read_event_borrowed().unwrap().is_none());
    }

    #[test]
    fn redundant_style_record_reads_can_be_skipped() {
        let mut output = Vec::new();
        let mut writer = LogWriter::new(&mut output).unwrap();
        for kind in [EventKind::StyleRecordPayloads, EventKind::StyleRecordView] {
            let mut payload = PayloadWriter::default();
            payload.write_u64(7);
            payload.write_u64(42);
            payload.write_bool(false);
            writer.write_event(kind, &payload).unwrap();
        }
        let mut payload = PayloadWriter::default();
        payload.write_u64(7);
        payload.write_u64(42);
        payload.write_bool(true);
        payload.write_bool(false);
        writer.write_event(EventKind::StyleRecordPayloads, &payload).unwrap();
        writer.flush().unwrap();

        let mut reader = LogReader::new(output.as_slice()).unwrap();
        let (skipped, event) = reader.read_event_borrowed_skipping_redundant_style_reads().unwrap();
        assert_eq!(skipped, 2);
        let event = event.unwrap();
        assert_eq!(event.kind, EventKind::StyleRecordPayloads);
    }

    #[test]
    fn selector_query_atom_mappings_event_round_trip() {
        let mut output = Vec::new();
        let mut writer = LogWriter::new(&mut output).unwrap();
        let mut payload = PayloadWriter::default();
        payload.write_u64(7);
        payload.write_length(2);
        payload.write_u8(0);
        payload.write_u64(11);
        payload.write_u32(1);
        payload.write_u8(1);
        payload.write_u32(2);
        payload.write_u32(3);
        payload.write_u32(4);
        writer
            .write_event(EventKind::SelectorQueryAtomMappings, &payload)
            .unwrap();
        writer.flush().unwrap();

        let mut reader = LogReader::new(Cursor::new(output)).unwrap();
        let mut event = reader.read_event().unwrap().unwrap();
        assert_eq!(event.kind, EventKind::SelectorQueryAtomMappings);
        assert_eq!(event.payload.read_u64().unwrap(), 7);
        assert_eq!(event.payload.read_length().unwrap(), 2);
        assert_eq!(event.payload.read_u8().unwrap(), 0);
        assert_eq!(event.payload.read_u64().unwrap(), 11);
        assert_eq!(event.payload.read_u32().unwrap(), 1);
        assert_eq!(event.payload.read_u8().unwrap(), 1);
        assert_eq!(event.payload.read_u32().unwrap(), 2);
        assert_eq!(event.payload.read_u32().unwrap(), 3);
        assert_eq!(event.payload.read_u32().unwrap(), 4);
        event.payload.finish().unwrap();
        assert!(reader.read_event().unwrap().is_none());
    }

    #[test]
    fn pointer_address_reuse_gets_a_new_token() {
        let mut tokens = PointerTokenRegistry::default();
        assert_eq!(tokens.token(1, 0x1234), 1);
        assert_eq!(tokens.token(1, 0x1234), 1);
        tokens.invalidate(0x1234);
        assert_eq!(tokens.token(1, 0x1234), 2);
    }

    #[test]
    fn raw_slice_round_trip() {
        #[repr(C)]
        #[derive(Clone, Copy, Debug, PartialEq, Eq)]
        struct Row {
            a: u32,
            b: u32,
        }
        unsafe impl RawRecord for Row {}

        let rows = [Row { a: 1, b: 2 }, Row { a: 3, b: 4 }];
        let mut output = Vec::new();
        let mut writer = LogWriter::new(&mut output).unwrap();
        let mut payload = PayloadWriter::default();
        payload.write_u32(0xabcd);
        payload.write_raw_slice(&rows);
        writer.write_event(EventKind::ApplyTransaction, &payload).unwrap();
        writer.flush().unwrap();

        // Raw arrays need the eight-aligned payload base a mapped log guarantees, so re-home the
        // bytes in u64 storage before reading.
        let mut aligned = vec![0_u64; output.len().div_ceil(size_of::<u64>())];
        unsafe { std::slice::from_raw_parts_mut(aligned.as_mut_ptr().cast::<u8>(), output.len()) }
            .copy_from_slice(&output);
        let bytes = unsafe { std::slice::from_raw_parts(aligned.as_ptr().cast::<u8>(), output.len()) };
        let mut reader = LogReader::new(bytes).unwrap();
        let mut event = reader.read_event_borrowed().unwrap().unwrap();
        assert_eq!(event.kind, EventKind::ApplyTransaction);
        assert_eq!(event.payload.read_u32().unwrap(), 0xabcd);
        assert_eq!(event.payload.read_raw_slice::<Row>().unwrap(), &rows);
        event.payload.finish().unwrap();
        assert!(reader.read_event_borrowed().unwrap().is_none());
    }

    #[test]
    fn event_corruption_is_rejected() {
        let mut output = Vec::new();
        let mut writer = LogWriter::new(&mut output).unwrap();
        let mut payload = PayloadWriter::default();
        payload.write_u64(123);
        writer.write_event(EventKind::DestroyGraph, &payload).unwrap();
        writer.flush().unwrap();
        *output.last_mut().unwrap() ^= 1;

        let mut reader = LogReader::new(Cursor::new(output)).unwrap();
        assert!(matches!(reader.read_event(), Err(Error::CorruptEvent { .. })));
    }
}
