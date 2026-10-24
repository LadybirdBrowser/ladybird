/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! LibIPC's wire format, as `IPC::Encoder` and `IPC::Decoder` write and read it.
//!
//! Numbers go as their bytes in the host's order, sizes as 32-bit numbers, booleans as one byte, an optional value as a
//! boolean followed by the value, and a sequence as its size followed by its elements. File descriptors and port rights
//! go beside the bytes, as attachments, in the order they are encoded.

use std::collections::VecDeque;
#[cfg(unix)]
use std::os::fd::OwnedFd;
#[cfg(windows)]
use std::os::windows::io::OwnedHandle;

use crate::attachment::Attachment;

#[derive(Debug)]
pub enum EncodeError {
    SizeTooLarge(usize),
    InvalidValue(&'static str),
    Attachment(std::io::Error),
}

#[derive(Debug, PartialEq, Eq)]
pub enum DecodeError {
    UnexpectedEnd,
    SizeTooLarge(usize),
    InvalidBoolean(u8),
    InvalidUtf8,
    InvalidValue(&'static str),
    MissingAttachment,
    Attachment(String),
    AllocationFailed,
}

pub trait Encode {
    fn encode(&self, encoder: &mut Encoder) -> Result<(), EncodeError>;

    /// Encodes an owned value, moving its attachments into the encoder.
    fn encode_owned(self, encoder: &mut Encoder) -> Result<(), EncodeError>
    where
        Self: Sized,
    {
        self.encode(encoder)
    }

    fn encode_slice(elements: &[Self], encoder: &mut Encoder) -> Result<(), EncodeError>
    where
        Self: Sized,
    {
        for element in elements {
            element.encode(encoder)?;
        }
        Ok(())
    }

    fn encode_vec(elements: Vec<Self>, encoder: &mut Encoder) -> Result<(), EncodeError>
    where
        Self: Sized,
    {
        encoder.encode_size(elements.len())?;
        for element in elements {
            element.encode_owned(encoder)?;
        }
        Ok(())
    }
}

pub trait Decode: Sized {
    /// A lower bound on the bytes an encoded value occupies, excluding attachments.
    const MIN_ENCODED_SIZE: usize = 0;

    fn decode(decoder: &mut Decoder<'_>) -> Result<Self, DecodeError>;

    fn decode_vec(decoder: &mut Decoder<'_>, size: usize) -> Result<Vec<Self>, DecodeError> {
        decode_elements(decoder, size)
    }
}

/// What an encoder can hand the bytes it encodes to, piece by piece, instead of keeping them.
pub type Sink = Box<dyn FnMut(&[u8])>;

/// Where an encoder puts the bytes it encodes.
enum Output {
    Bytes(Vec<u8>),
    /// Each piece goes to the sink as soon as it is encoded.
    Sink(Sink),
}

pub struct Encoder {
    output: Output,
    attachments: Vec<Attachment>,
}

impl Default for Encoder {
    fn default() -> Self {
        Self {
            output: Output::Bytes(Vec::new()),
            attachments: Vec::new(),
        }
    }
}

impl Encoder {
    pub fn new() -> Self {
        Self::default()
    }

    /// An encoder that hands the bytes it encodes to `sink` as it goes, such as to append them to a C++
    /// `IPC::Encoder`'s message, instead of keeping them. `finish()` then returns no bytes.
    pub fn with_sink(sink: Sink) -> Self {
        Self {
            output: Output::Sink(sink),
            attachments: Vec::new(),
        }
    }

    pub fn encode<T: Encode + ?Sized>(&mut self, value: &T) -> Result<(), EncodeError> {
        value.encode(self)
    }

    pub fn encode_owned<T: Encode>(&mut self, value: T) -> Result<(), EncodeError> {
        value.encode_owned(self)
    }

    pub fn append_bytes(&mut self, bytes: &[u8]) {
        match &mut self.output {
            Output::Bytes(output) => output.extend_from_slice(bytes),
            Output::Sink(sink) => sink(bytes),
        }
    }

    /// Appends `pieces`, which hold `byte_count` bytes in all.
    fn append_pieces<const N: usize>(&mut self, byte_count: usize, pieces: impl Iterator<Item = [u8; N]>) {
        match &mut self.output {
            Output::Bytes(output) => {
                output.reserve(byte_count);
                for piece in pieces {
                    output.extend_from_slice(&piece);
                }
            }
            Output::Sink(sink) => {
                // NB: A sink takes a call for each append, so hand it the pieces in chunks rather than one by one.
                let mut chunk = [0u8; 512];
                let mut length = 0;
                for piece in pieces {
                    if length + N > chunk.len() {
                        sink(&chunk[..length]);
                        length = 0;
                    }
                    chunk[length..length + N].copy_from_slice(&piece);
                    length += N;
                }
                if length > 0 {
                    sink(&chunk[..length]);
                }
            }
        }
    }

    pub fn encode_size(&mut self, size: usize) -> Result<(), EncodeError> {
        let size = u32::try_from(size).map_err(|_| EncodeError::SizeTooLarge(size))?;
        self.append_bytes(&size.to_ne_bytes());
        Ok(())
    }

    pub fn append_attachment(&mut self, attachment: Attachment) {
        self.attachments.push(attachment);
    }

    pub fn finish(self) -> (Vec<u8>, Vec<Attachment>) {
        let bytes = match self.output {
            Output::Bytes(bytes) => bytes,
            Output::Sink(_) => Vec::new(),
        };
        (bytes, self.attachments)
    }
}

pub struct Decoder<'a> {
    bytes: &'a [u8],
    position: usize,
    attachments: VecDeque<Attachment>,
    max_size_of_next_value: usize,
}

impl<'a> Decoder<'a> {
    pub fn new(bytes: &'a [u8], attachments: impl IntoIterator<Item = Attachment>) -> Self {
        Self {
            bytes,
            position: 0,
            attachments: attachments.into_iter().collect(),
            max_size_of_next_value: usize::MAX,
        }
    }

    pub fn decode<T: Decode>(&mut self) -> Result<T, DecodeError> {
        T::decode(self)
    }

    pub fn read_bytes(&mut self, count: usize) -> Result<&'a [u8], DecodeError> {
        let end = self.position.checked_add(count).ok_or(DecodeError::UnexpectedEnd)?;
        let bytes = self.bytes.get(self.position..end).ok_or(DecodeError::UnexpectedEnd)?;
        self.position = end;
        Ok(bytes)
    }

    pub fn decode_size(&mut self) -> Result<usize, DecodeError> {
        let max_size = std::mem::replace(&mut self.max_size_of_next_value, usize::MAX);
        let size = self.decode::<u32>()? as usize;
        if size > max_size {
            return Err(DecodeError::SizeTooLarge(size));
        }
        if size > self.remaining_bytes().saturating_add(self.attachments.len()) {
            return Err(DecodeError::UnexpectedEnd);
        }
        Ok(size)
    }

    /// Decodes a string, byte buffer or vector, which goes as its size and then its contents, refusing one larger than
    /// `max_size` before decoding its contents.
    pub fn decode_with_max_size<T: Decode>(&mut self, max_size: usize) -> Result<T, DecodeError> {
        // NB: Decoding a string, byte buffer or vector starts with decode_size(), which checks the size against this.
        //     The bound ends with the value, so it never applies to anything decoded after it.
        self.max_size_of_next_value = max_size;
        let value = self.decode();
        self.max_size_of_next_value = usize::MAX;
        value
    }

    pub fn take_attachment(&mut self) -> Result<Attachment, DecodeError> {
        self.attachments.pop_front().ok_or(DecodeError::MissingAttachment)
    }

    pub fn remaining_bytes(&self) -> usize {
        self.bytes.len() - self.position
    }

    pub fn remaining_attachments(&self) -> usize {
        self.attachments.len()
    }

    pub fn is_at_end(&self) -> bool {
        self.remaining_bytes() == 0
    }
}

macro_rules! impl_number {
    ($($type:ty),*) => {
        $(
            impl Encode for $type {
                fn encode(&self, encoder: &mut Encoder) -> Result<(), EncodeError> {
                    encoder.append_bytes(&self.to_ne_bytes());
                    Ok(())
                }

                fn encode_slice(elements: &[Self], encoder: &mut Encoder) -> Result<(), EncodeError> {
                    let byte_count = elements.len().checked_mul(size_of::<Self>()).ok_or(EncodeError::SizeTooLarge(elements.len()))?;
                    encoder.append_pieces(byte_count, elements.iter().map(|element| element.to_ne_bytes()));
                    Ok(())
                }

                fn encode_vec(elements: Vec<Self>, encoder: &mut Encoder) -> Result<(), EncodeError> {
                    elements.as_slice().encode(encoder)
                }
            }

            impl Decode for $type {
                const MIN_ENCODED_SIZE: usize = size_of::<$type>();

                fn decode(decoder: &mut Decoder<'_>) -> Result<Self, DecodeError> {
                    let bytes = decoder.read_bytes(size_of::<$type>())?;
                    Ok(<$type>::from_ne_bytes(bytes.try_into().map_err(|_| DecodeError::UnexpectedEnd)?))
                }

                fn decode_vec(decoder: &mut Decoder<'_>, size: usize) -> Result<Vec<Self>, DecodeError> {
                    let byte_count = size.checked_mul(size_of::<$type>()).ok_or(DecodeError::SizeTooLarge(size))?;
                    let bytes = decoder.read_bytes(byte_count)?;
                    Ok(bytes.as_chunks::<{ size_of::<$type>() }>().0.iter()
                        .map(|&chunk| <$type>::from_ne_bytes(chunk))
                        .collect())
                }
            }
        )*
    };
}

impl_number!(u16, u32, u64, i8, i16, i32, i64, f32, f64);

impl Encode for u8 {
    fn encode(&self, encoder: &mut Encoder) -> Result<(), EncodeError> {
        encoder.append_bytes(&[*self]);
        Ok(())
    }

    fn encode_slice(elements: &[Self], encoder: &mut Encoder) -> Result<(), EncodeError> {
        encoder.append_bytes(elements);
        Ok(())
    }

    fn encode_vec(elements: Vec<Self>, encoder: &mut Encoder) -> Result<(), EncodeError> {
        elements.as_slice().encode(encoder)
    }
}

impl Decode for u8 {
    const MIN_ENCODED_SIZE: usize = 1;

    fn decode(decoder: &mut Decoder<'_>) -> Result<Self, DecodeError> {
        Ok(decoder.read_bytes(1)?[0])
    }

    fn decode_vec(decoder: &mut Decoder<'_>, size: usize) -> Result<Vec<Self>, DecodeError> {
        Ok(decoder.read_bytes(size)?.to_vec())
    }
}

// NB: A size_t encodes as the 64-bit number it is on every target Ladybird supports.
impl Encode for usize {
    fn encode(&self, encoder: &mut Encoder) -> Result<(), EncodeError> {
        (*self as u64).encode(encoder)
    }
}

impl Decode for usize {
    const MIN_ENCODED_SIZE: usize = 8;

    fn decode(decoder: &mut Decoder<'_>) -> Result<Self, DecodeError> {
        usize::try_from(decoder.decode::<u64>()?).map_err(|_| DecodeError::InvalidValue("size_t"))
    }

    fn decode_vec(decoder: &mut Decoder<'_>, size: usize) -> Result<Vec<Self>, DecodeError> {
        let byte_count = size.checked_mul(8).ok_or(DecodeError::SizeTooLarge(size))?;
        let bytes = decoder.read_bytes(byte_count)?;
        bytes
            .as_chunks::<8>()
            .0
            .iter()
            .map(|&chunk| usize::try_from(u64::from_ne_bytes(chunk)).map_err(|_| DecodeError::InvalidValue("size_t")))
            .collect()
    }
}

impl Encode for bool {
    fn encode(&self, encoder: &mut Encoder) -> Result<(), EncodeError> {
        u8::from(*self).encode(encoder)
    }
}

impl Decode for bool {
    const MIN_ENCODED_SIZE: usize = 1;

    fn decode(decoder: &mut Decoder<'_>) -> Result<Self, DecodeError> {
        match decoder.decode::<u8>()? {
            0 => Ok(false),
            1 => Ok(true),
            other => Err(DecodeError::InvalidBoolean(other)),
        }
    }
}

impl Encode for () {
    fn encode(&self, _: &mut Encoder) -> Result<(), EncodeError> {
        Ok(())
    }
}

impl Decode for () {
    fn decode(_: &mut Decoder<'_>) -> Result<Self, DecodeError> {
        Ok(())
    }
}

/// `String` and `StringView`: the size of the UTF-8 bytes, then the bytes.
impl Encode for str {
    fn encode(&self, encoder: &mut Encoder) -> Result<(), EncodeError> {
        self.as_bytes().encode(encoder)
    }
}

impl Encode for String {
    fn encode(&self, encoder: &mut Encoder) -> Result<(), EncodeError> {
        self.as_str().encode(encoder)
    }
}

impl Decode for String {
    const MIN_ENCODED_SIZE: usize = 4;

    fn decode(decoder: &mut Decoder<'_>) -> Result<Self, DecodeError> {
        let size = decoder.decode_size()?;
        let bytes = decoder.read_bytes(size)?;
        String::from_utf8(bytes.to_vec()).map_err(|_| DecodeError::InvalidUtf8)
    }
}

/// C++ `String`: UTF-8 bytes that may include lone surrogate encodings.
#[derive(Clone, Debug, Default, PartialEq, Eq, Hash)]
pub struct Utf8String(Vec<u8>);

impl Utf8String {
    pub fn from_bytes(bytes: Vec<u8>) -> Result<Self, DecodeError> {
        validate_utf8(&bytes)?;
        Ok(Self(bytes))
    }

    pub fn as_bytes(&self) -> &[u8] {
        &self.0
    }

    pub fn into_bytes(self) -> Vec<u8> {
        self.0
    }

    pub fn as_str(&self) -> Result<&str, std::str::Utf8Error> {
        std::str::from_utf8(&self.0)
    }

    pub fn into_string(self) -> Result<String, std::string::FromUtf8Error> {
        String::from_utf8(self.0)
    }
}

impl From<&str> for Utf8String {
    fn from(string: &str) -> Self {
        Self(string.as_bytes().to_vec())
    }
}

impl From<String> for Utf8String {
    fn from(string: String) -> Self {
        Self(string.into_bytes())
    }
}

impl Encode for Utf8String {
    fn encode(&self, encoder: &mut Encoder) -> Result<(), EncodeError> {
        self.as_bytes().encode(encoder)
    }
}

impl Decode for Utf8String {
    const MIN_ENCODED_SIZE: usize = 4;

    fn decode(decoder: &mut Decoder<'_>) -> Result<Self, DecodeError> {
        let size = decoder.decode_size()?;
        let bytes = decoder.read_bytes(size)?;
        validate_utf8(bytes)?;
        Ok(Self(bytes.to_vec()))
    }
}

/// `ByteString`: bytes in no particular encoding, which go as `String` does.
#[derive(Clone, Debug, Default, PartialEq, Eq, Hash)]
pub struct ByteString(pub Vec<u8>);

impl Encode for ByteString {
    fn encode(&self, encoder: &mut Encoder) -> Result<(), EncodeError> {
        self.0.as_slice().encode(encoder)
    }
}

impl Decode for ByteString {
    const MIN_ENCODED_SIZE: usize = 4;

    fn decode(decoder: &mut Decoder<'_>) -> Result<Self, DecodeError> {
        let size = decoder.decode_size()?;
        Ok(Self(decoder.read_bytes(size)?.to_vec()))
    }
}

/// Validates bytes as `Utf8View::validate()` does, including lonely surrogates.
pub fn validate_utf8(mut bytes: &[u8]) -> Result<(), DecodeError> {
    while let Err(error) = std::str::from_utf8(bytes) {
        bytes = &bytes[error.valid_up_to()..];
        match bytes {
            [0xed, 0xa0..=0xbf, 0x80..=0xbf, rest @ ..] => bytes = rest,
            _ => return Err(DecodeError::InvalidUtf8),
        }
    }
    Ok(())
}

/// `Utf16String`: whether its code units are stored as ASCII bytes, their count, then the code units.
#[derive(Clone, Debug, Default, PartialEq, Eq, Hash)]
pub struct Utf16String(pub Vec<u16>);

impl From<&str> for Utf16String {
    fn from(string: &str) -> Self {
        Self(string.encode_utf16().collect())
    }
}

impl Utf16String {
    pub fn to_string_lossy(&self) -> String {
        String::from_utf16_lossy(&self.0)
    }
}

impl Encode for Utf16String {
    fn encode(&self, encoder: &mut Encoder) -> Result<(), EncodeError> {
        let is_ascii = self.0.iter().all(|&code_unit| code_unit < 0x80);
        is_ascii.encode(encoder)?;
        encoder.encode_size(self.0.len())?;
        if is_ascii {
            encoder.append_pieces(self.0.len(), self.0.iter().map(|&code_unit| [code_unit as u8]));
        } else {
            u16::encode_slice(&self.0, encoder)?;
        }
        Ok(())
    }
}

impl Decode for Utf16String {
    const MIN_ENCODED_SIZE: usize = 5;

    fn decode(decoder: &mut Decoder<'_>) -> Result<Self, DecodeError> {
        let is_ascii = decoder.decode::<bool>()?;
        let length = decoder.decode_size()?;
        if is_ascii {
            let bytes = decoder.read_bytes(length)?;
            if !bytes.is_ascii() {
                return Err(DecodeError::InvalidValue("Utf16String with ASCII storage"));
            }
            return Ok(Self(bytes.iter().map(|&byte| u16::from(byte)).collect()));
        }
        let byte_count = length.checked_mul(2).ok_or(DecodeError::SizeTooLarge(length))?;
        let bytes = decoder.read_bytes(byte_count)?;
        let (code_units, _) = bytes.as_chunks::<2>();
        Ok(Self(code_units.iter().map(|&pair| u16::from_ne_bytes(pair)).collect()))
    }
}

/// `Vector` and spans: the size, then the elements.
impl<T: Encode> Encode for [T] {
    fn encode(&self, encoder: &mut Encoder) -> Result<(), EncodeError> {
        encoder.encode_size(self.len())?;
        T::encode_slice(self, encoder)
    }
}

impl<T: Encode> Encode for Vec<T> {
    fn encode(&self, encoder: &mut Encoder) -> Result<(), EncodeError> {
        self.as_slice().encode(encoder)
    }

    fn encode_owned(self, encoder: &mut Encoder) -> Result<(), EncodeError> {
        T::encode_vec(self, encoder)
    }
}

impl<T: Decode> Decode for Vec<T> {
    const MIN_ENCODED_SIZE: usize = 4;

    fn decode(decoder: &mut Decoder<'_>) -> Result<Self, DecodeError> {
        let size = decoder.decode_size()?;
        T::decode_vec(decoder, size)
    }
}

fn decode_elements<T: Decode>(decoder: &mut Decoder<'_>, size: usize) -> Result<Vec<T>, DecodeError> {
    if T::MIN_ENCODED_SIZE != 0 && size > decoder.remaining_bytes() / T::MIN_ENCODED_SIZE {
        return Err(DecodeError::UnexpectedEnd);
    }
    let mut elements = Vec::new();
    for _ in 0..size {
        let element = decoder.decode()?;
        if elements.len() == elements.capacity() {
            // NB: Grow only after decoding an element, independently of the peer's count.
            elements.try_reserve(1).map_err(|_| DecodeError::AllocationFailed)?;
        }
        elements.push(element);
    }
    Ok(elements)
}

/// `Array`: the size, which must match, then the elements.
impl<T: Encode, const N: usize> Encode for [T; N] {
    fn encode(&self, encoder: &mut Encoder) -> Result<(), EncodeError> {
        self.as_slice().encode(encoder)
    }

    fn encode_owned(self, encoder: &mut Encoder) -> Result<(), EncodeError> {
        encoder.encode_size(N)?;
        for element in self {
            element.encode_owned(encoder)?;
        }
        Ok(())
    }
}

impl<T: Decode, const N: usize> Decode for [T; N] {
    const MIN_ENCODED_SIZE: usize = 4;

    fn decode(decoder: &mut Decoder<'_>) -> Result<Self, DecodeError> {
        let size = decoder.decode_size()?;
        if size != N {
            return Err(DecodeError::InvalidValue("Array size"));
        }
        let elements = T::decode_vec(decoder, N)?;
        elements.try_into().map_err(|_| DecodeError::InvalidValue("Array size"))
    }
}

/// `Optional`: whether there is a value, then the value.
impl<T: Encode> Encode for Option<T> {
    fn encode(&self, encoder: &mut Encoder) -> Result<(), EncodeError> {
        self.is_some().encode(encoder)?;
        if let Some(value) = self {
            value.encode(encoder)?;
        }
        Ok(())
    }

    fn encode_owned(self, encoder: &mut Encoder) -> Result<(), EncodeError> {
        self.is_some().encode(encoder)?;
        if let Some(value) = self {
            value.encode_owned(encoder)?;
        }
        Ok(())
    }
}

impl<T: Decode> Decode for Option<T> {
    const MIN_ENCODED_SIZE: usize = 1;

    fn decode(decoder: &mut Decoder<'_>) -> Result<Self, DecodeError> {
        if decoder.decode::<bool>()? {
            return Ok(Some(decoder.decode()?));
        }
        Ok(None)
    }
}

/// `IPC::File`: a file descriptor, which goes as an attachment.
#[cfg(unix)]
#[derive(Debug)]
pub struct File(pub OwnedFd);

#[cfg(unix)]
impl Encode for File {
    fn encode(&self, encoder: &mut Encoder) -> Result<(), EncodeError> {
        let fd = self.0.try_clone().map_err(EncodeError::Attachment)?;
        encoder.append_attachment(Attachment::from_fd(fd).map_err(EncodeError::Attachment)?);
        Ok(())
    }

    fn encode_owned(self, encoder: &mut Encoder) -> Result<(), EncodeError> {
        encoder.append_attachment(Attachment::from_fd(self.0).map_err(EncodeError::Attachment)?);
        Ok(())
    }
}

#[cfg(unix)]
impl Decode for File {
    fn decode(decoder: &mut Decoder<'_>) -> Result<Self, DecodeError> {
        let attachment = decoder.take_attachment()?;
        Ok(Self(
            attachment
                .into_fd()
                .map_err(|error| DecodeError::Attachment(error.to_string()))?,
        ))
    }
}

/// `IPC::File`: a handle, which goes as an attachment, as the int that `IPC::File` holds on Windows names one.
#[cfg(windows)]
#[derive(Debug)]
pub struct File(pub OwnedHandle);

#[cfg(windows)]
impl Encode for File {
    fn encode(&self, encoder: &mut Encoder) -> Result<(), EncodeError> {
        let handle = self.0.try_clone().map_err(EncodeError::Attachment)?;
        encoder.append_attachment(Attachment::from_handle(handle));
        Ok(())
    }

    fn encode_owned(self, encoder: &mut Encoder) -> Result<(), EncodeError> {
        encoder.append_attachment(Attachment::from_handle(self.0));
        Ok(())
    }
}

#[cfg(windows)]
impl Decode for File {
    fn decode(decoder: &mut Decoder<'_>) -> Result<Self, DecodeError> {
        Ok(Self(decoder.take_attachment()?.into_handle()))
    }
}

/// A message of an endpoint, which goes as the endpoint's magic number, the message's ID and the message's fields.
pub trait EndpointMessage: Encode {
    const ENDPOINT_MAGIC: u32;
    const MESSAGE_ID: i32;

    fn encode_message(self) -> Result<(Vec<u8>, Vec<Attachment>), EncodeError>
    where
        Self: Sized,
    {
        let mut encoder = Encoder::new();
        encoder.encode(&Self::ENDPOINT_MAGIC)?;
        encoder.encode(&Self::MESSAGE_ID)?;
        encoder.encode_owned(self)?;
        Ok(encoder.finish())
    }
}

/// Reads the endpoint magic number and message ID that a message starts with.
pub fn decode_message_header(decoder: &mut Decoder<'_>) -> Result<(u32, i32), DecodeError> {
    Ok((decoder.decode()?, decoder.decode()?))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn round_trip<T: Encode + Decode>(value: &T) -> (T, Vec<u8>) {
        let mut encoder = Encoder::new();
        encoder.encode(value).unwrap();
        let (bytes, attachments) = encoder.finish();
        let mut decoder = Decoder::new(&bytes, attachments);
        let decoded = decoder.decode().unwrap();
        assert!(decoder.is_at_end());
        (decoded, bytes)
    }

    fn encode_into_sink<T: Encode + ?Sized>(value: &T) -> Vec<u8> {
        let output = std::rc::Rc::new(std::cell::RefCell::new(Vec::new()));
        let sink_output = std::rc::Rc::clone(&output);
        let mut encoder = Encoder::with_sink(Box::new(move |bytes| sink_output.borrow_mut().extend_from_slice(bytes)));
        encoder.encode(value).unwrap();
        let (bytes, attachments) = encoder.finish();
        assert!(bytes.is_empty());
        assert!(attachments.is_empty());
        output.take()
    }

    #[test]
    fn a_sink_receives_what_an_encoder_would_keep() {
        fn check<T: Encode + ?Sized>(value: &T) {
            let mut encoder = Encoder::new();
            encoder.encode(value).unwrap();
            assert_eq!(encode_into_sink(value), encoder.finish().0);
        }
        // NB: Long enough that numbers and ASCII code units fill the sink's chunk more than once.
        check(&(0..1000u32).collect::<Vec<_>>());
        check(&(0..1000).map(|value| f64::from(value) / 3.0).collect::<Vec<_>>());
        check(&vec![0xabu8; 2000]);
        check(&Utf16String("a".repeat(1500).encode_utf16().collect()));
        check(&Utf16String("ü".repeat(1500).encode_utf16().collect()));
        check(&Some(7i64));
        check(&Utf8String::from_bytes(b"text".to_vec()).unwrap());
        check(&vec![Some(1u16), None]);
    }

    #[test]
    fn sizes_are_bounded_before_contents_and_only_within_the_value() {
        let mut encoder = Encoder::new();
        encoder.encode("123456789").unwrap();
        encoder.encode(&None::<String>).unwrap();
        encoder.encode("123456789").unwrap();
        let (bytes, _) = encoder.finish();
        let mut decoder = Decoder::new(&bytes, []);
        assert_eq!(
            decoder.decode_with_max_size::<String>(8),
            Err(DecodeError::SizeTooLarge(9))
        );
        let mut decoder = Decoder::new(&bytes, []);
        assert_eq!(decoder.decode_with_max_size::<String>(9).unwrap(), "123456789");
        assert_eq!(decoder.decode_with_max_size::<Option<String>>(4).unwrap(), None);
        assert_eq!(decoder.decode::<String>().unwrap(), "123456789");
    }

    #[test]
    fn numbers_go_as_their_bytes() {
        let (value, bytes) = round_trip(&0x0102_0304u32);
        assert_eq!(value, 0x0102_0304);
        assert_eq!(bytes, 0x0102_0304u32.to_ne_bytes());
        assert_eq!(round_trip(&-5i64).0, -5);
        assert_eq!(round_trip(&1.5f64).0, 1.5);
        assert_eq!(round_trip(&usize::MAX).1.len(), 8);
    }

    #[test]
    fn numeric_vectors_decode_from_one_payload() {
        macro_rules! check {
            ($($type:ty),*) => {$(
                let values: Vec<$type> = vec![0 as $type, 1 as $type, <$type>::MIN, <$type>::MAX];
                let (decoded, bytes) = round_trip(&values);
                assert_eq!(decoded, values);
                let mut expected = 4u32.to_ne_bytes().to_vec();
                for value in &values {
                    expected.extend_from_slice(&value.to_ne_bytes());
                }
                assert_eq!(bytes, expected);
                let mut encoder = Encoder::new();
                encoder.encode_owned(values.clone()).unwrap();
                assert_eq!(encoder.finish().0, bytes);
                for length in 4..bytes.len() {
                    assert!(Decoder::new(&bytes[..length], []).decode::<Vec<$type>>().is_err());
                }
                assert!(Decoder::new(&0u32.to_ne_bytes(), []).decode::<Vec<$type>>().unwrap().is_empty());
                assert!(<$type>::decode_vec(&mut Decoder::new(&[], []), usize::MAX).is_err());
            )*};
        }
        check!(u8, u16, u32, u64, i8, i16, i32, i64, f32, f64, usize);
    }

    #[test]
    fn booleans_go_as_one_byte_and_must_be_zero_or_one() {
        assert_eq!(round_trip(&true).1, [1]);
        let mut decoder = Decoder::new(&[2], []);
        assert_eq!(decoder.decode::<bool>(), Err(DecodeError::InvalidBoolean(2)));
    }

    #[test]
    fn strings_go_as_their_size_and_bytes() {
        let (value, bytes) = round_trip(&"héllo".to_string());
        assert_eq!(value, "héllo");
        let mut expected = 6u32.to_ne_bytes().to_vec();
        expected.extend_from_slice("héllo".as_bytes());
        assert_eq!(bytes, expected);

        let mut invalid = 1u32.to_ne_bytes().to_vec();
        invalid.push(0xff);
        assert_eq!(
            Decoder::new(&invalid, []).decode::<String>(),
            Err(DecodeError::InvalidUtf8)
        );
    }

    #[test]
    fn cpp_strings_preserve_lone_surrogates() {
        for bytes in [b"".as_slice(), "héllo".as_bytes(), b"a\xed\xa0\x80\xed\xbf\xbfb"] {
            let value = Utf8String::from_bytes(bytes.to_vec()).unwrap();
            let (decoded, encoded) = round_trip(&value);
            assert_eq!(decoded.as_bytes(), bytes);
            assert_eq!(&encoded[4..], bytes);
            assert_eq!(decoded.as_str().is_ok(), std::str::from_utf8(bytes).is_ok());
            match decoded.into_string() {
                Ok(string) => assert_eq!(string.as_bytes(), bytes),
                Err(error) => assert_eq!(error.into_bytes(), bytes),
            }
            assert_eq!(value.into_bytes(), bytes);
        }
        assert_eq!(Utf8String::from("héllo").as_str(), Ok("héllo"));
        assert_eq!(Utf8String::from("hello".to_string()).into_string().unwrap(), "hello");
        for bytes in [
            b"\xff".as_slice(),
            b"\xc0\x80",
            b"\xed\xa0",
            b"\xed\xa0\x80\xff",
            b"\xf4\x90\x80\x80",
        ] {
            assert_eq!(Utf8String::from_bytes(bytes.to_vec()), Err(DecodeError::InvalidUtf8));
            let mut encoder = Encoder::new();
            encoder.encode(bytes).unwrap();
            assert_eq!(
                Decoder::new(&encoder.finish().0, []).decode::<Utf8String>(),
                Err(DecodeError::InvalidUtf8)
            );
        }
    }

    #[test]
    fn utf8_validation_accepts_lonely_surrogates() {
        assert_eq!(validate_utf8("héllo".as_bytes()), Ok(()));
        assert_eq!(validate_utf8(b"a\xed\xa0\x80\xed\xbf\xbfb"), Ok(()));
        for bytes in [
            b"\xed\xa0".as_slice(),
            b"\xed\xa0\x80\xff",
            b"\xc0\x80",
            b"\xf4\x90\x80\x80",
        ] {
            assert_eq!(validate_utf8(bytes), Err(DecodeError::InvalidUtf8));
        }
    }

    #[test]
    fn utf16_strings_go_as_ascii_bytes_when_they_can() {
        let (value, bytes) = round_trip(&Utf16String::from("ab"));
        assert_eq!(value.to_string_lossy(), "ab");
        assert_eq!(bytes, [1, 2, 0, 0, 0, b'a', b'b']);

        for units in [vec![], vec![0x7f], vec![0x80, 0xd800, 0xffff]] {
            let value = Utf16String(units.clone());
            let (decoded, bytes) = round_trip(&value);
            assert_eq!(decoded, value);
            let is_ascii = units.iter().all(|&unit| unit < 0x80);
            let mut expected = vec![u8::from(is_ascii)];
            expected.extend_from_slice(&(units.len() as u32).to_ne_bytes());
            for unit in units {
                if is_ascii {
                    expected.push(unit as u8);
                } else {
                    expected.extend_from_slice(&unit.to_ne_bytes());
                }
            }
            assert_eq!(bytes, expected);
        }
    }

    #[test]
    fn optionals_and_vectors_compose() {
        let value: Vec<Option<u16>> = vec![Some(1), None, Some(3)];
        assert_eq!(round_trip(&value).0, value);
        let array = [1u8, 2, 3];
        assert_eq!(round_trip(&array).0, array);
    }

    #[test]
    fn large_byte_buffers_go_as_their_size_and_bytes() {
        let buffer: Vec<u8> = (0..64 * 1024 * 1024 + 1).map(|index| index as u8).collect();
        let (decoded, bytes) = round_trip(&buffer);
        assert_eq!(decoded, buffer);
        assert_eq!(bytes[..4], (buffer.len() as u32).to_ne_bytes());
        assert_eq!(bytes[4..], buffer);
        drop(buffer);
        let mut encoder = Encoder::new();
        encoder.encode_owned(decoded).unwrap();
        assert_eq!(encoder.finish().0, bytes);
    }

    #[test]
    fn sizes_are_bounded_by_the_remaining_input() {
        let bytes = u32::MAX.to_ne_bytes();
        assert_eq!(Decoder::new(&bytes, []).decode_size(), Err(DecodeError::UnexpectedEnd));
        let bytes = [1, 0, 0, 0, 42];
        let mut decoder = Decoder::new(&bytes, []);
        assert_eq!(decoder.decode_size(), Ok(1));
        assert_eq!(decoder.decode::<u8>(), Ok(42));
    }

    #[test]
    fn truncated_input_is_refused() {
        let bytes = 4u32.to_ne_bytes();
        assert_eq!(
            Decoder::new(&bytes, []).decode::<Vec<u8>>(),
            Err(DecodeError::UnexpectedEnd)
        );
        assert_eq!(
            Decoder::new(&[1, 2], []).decode::<u32>(),
            Err(DecodeError::UnexpectedEnd)
        );
    }

    #[test]
    fn vector_counts_must_fit_the_element_encoding() {
        let mut bytes = (64 * 1024 * 1024u32).to_ne_bytes().to_vec();
        bytes.resize(1024 * 1024, 0);
        assert_eq!(
            Decoder::new(&bytes, []).decode::<Vec<String>>(),
            Err(DecodeError::UnexpectedEnd)
        );
        let mut bytes = 2u32.to_ne_bytes().to_vec();
        bytes.extend_from_slice(&1u64.to_ne_bytes());
        assert_eq!(
            Decoder::new(&bytes, []).decode::<Vec<u64>>(),
            Err(DecodeError::UnexpectedEnd)
        );
    }

    #[test]
    fn vectors_grow_as_elements_decode() {
        let value = vec!["a".to_string(); 100];
        let decoded = round_trip(&value).0;
        assert_eq!(decoded, value);
        assert!(decoded.capacity() <= value.len() * 2);

        #[cfg(unix)]
        {
            let (reader, writer) = std::io::pipe().unwrap();
            let bytes = 1u32.to_ne_bytes();
            let attachments = [Attachment::from_fd(OwnedFd::from(writer)).unwrap()];
            let files = Decoder::new(&bytes, attachments).decode::<Vec<File>>().unwrap();
            assert_eq!(files.len(), 1);
            drop(files);
            drop(reader);
            assert!(matches!(
                Decoder::new(&bytes, []).decode::<Vec<File>>(),
                Err(DecodeError::UnexpectedEnd)
            ));
        }
    }

    #[test]
    fn a_vector_of_custom_types_decodes_before_reserving() {
        struct LargeValue([u8; 8192]);
        impl Decode for LargeValue {
            fn decode(decoder: &mut Decoder<'_>) -> Result<Self, DecodeError> {
                decoder.decode::<bool>()?;
                Ok(Self([0; 8192]))
            }
        }
        let mut bytes = 1024u32.to_ne_bytes().to_vec();
        bytes.resize(4 + 1024, 2);
        assert!(matches!(
            Decoder::new(&bytes, []).decode::<Vec<LargeValue>>(),
            Err(DecodeError::InvalidBoolean(2))
        ));
        let bytes = [1, 0, 0, 0, 1];
        let decoded = Decoder::new(&bytes, []).decode::<Vec<LargeValue>>().unwrap();
        assert_eq!(decoded.capacity(), 1);
        assert_eq!(decoded[0].0[0], 0);
    }

    #[cfg(unix)]
    #[test]
    fn files_go_as_attachments() {
        let (reader, writer) = std::io::pipe().unwrap();
        let mut encoder = Encoder::new();
        encoder.encode(&File(OwnedFd::from(writer))).unwrap();
        let (bytes, attachments) = encoder.finish();
        assert!(bytes.is_empty());
        assert_eq!(attachments.len(), 1);

        let mut decoder = Decoder::new(&bytes, attachments);
        let File(fd) = decoder.decode().unwrap();
        let mut writer = std::fs::File::from(fd);
        std::io::Write::write_all(&mut writer, b"x").unwrap();
        drop(writer);
        let mut buffer = Vec::new();
        std::io::Read::read_to_end(&mut std::fs::File::from(OwnedFd::from(reader)), &mut buffer).unwrap();
        assert_eq!(buffer, b"x");
    }

    #[cfg(unix)]
    #[test]
    fn owned_containers_move_their_files() {
        use std::os::fd::AsRawFd;
        let mut readers = Vec::new();
        let mut descriptors = Vec::new();
        let files: Vec<Option<[File; 1]>> = (0..32)
            .map(|_| {
                let (reader, writer) = std::io::pipe().unwrap();
                readers.push(reader);
                descriptors.push(writer.as_raw_fd());
                Some([File(OwnedFd::from(writer))])
            })
            .collect();
        let mut encoder = Encoder::new();
        encoder.encode_owned(files).unwrap();
        let (bytes, attachments) = encoder.finish();
        let mut decoder = Decoder::new(&bytes, attachments);
        assert_eq!(decoder.remaining_attachments(), 32);
        let files = decoder.decode::<Vec<Option<[File; 1]>>>().unwrap();
        assert_eq!(decoder.remaining_attachments(), 0);
        for (file, descriptor) in files.into_iter().zip(descriptors) {
            let [File(fd)] = file.unwrap();
            #[cfg(not(target_vendor = "apple"))]
            assert_eq!(fd.as_raw_fd(), descriptor);
            #[cfg(target_vendor = "apple")]
            let _ = descriptor;
            drop(fd);
        }
        for mut reader in readers {
            assert_eq!(std::io::Read::read(&mut reader, &mut [0]).unwrap(), 0);
        }
    }
}
