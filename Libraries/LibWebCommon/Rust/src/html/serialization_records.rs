/*
 * Copyright (c) 2022, Daniel Ehrenberg <dan@littledan.dev>
 * Copyright (c) 2022, Andrew Kaster <akaster@serenityos.org>
 * Copyright (c) 2023-2024, Kenneth Myhra <kennethmyhra@serenityos.org>
 * Copyright (c) 2023, Idan Horowitz <idan.horowitz@serenityos.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The formats of the records of structured serialization, https://html.spec.whatwg.org/multipage/structured-data.html
//!
//! A record, an HTML::IPCSerializationRecord or an HTML::StorageSerializationRecord of
//! LibWebCommon/HTML/SerializationRecords.h, holds the values that a serialization writes in one of two formats. The
//! IPC format is read only by the build that writes it, and holds integers at their full width in the byte order of
//! the host. The storage format is stable on disk, for IndexedDB: a storage record starts with the prologue of the
//! format, and holds integers as LEB128 varints and everything else in little-endian byte order.

use core::cell::Cell;

use ak::Utf16StringUnits;

/// HTML::storage_format_magic, which every storage record starts with.
const STORAGE_FORMAT_MAGIC: [u8; 4] = *b"LBSC";

/// HTML::storage_format_version, the on-disk version of the storage format. Bumping it is a deliberate format change.
const STORAGE_FORMAT_VERSION: u64 = 1;

/// HTML::storage_format_flags, for optional features of the envelope. A change to the encoding of values bumps the
/// version instead.
const STORAGE_FORMAT_FLAGS: u64 = 0;

/// HTML::SerializationType, the format of a record.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SerializationType {
    Ipc,
    Storage,
}

fn append_unsigned_leb128(data: &mut Vec<u8>, mut value: u64) {
    loop {
        let byte = (value & 0x7f) as u8;
        value >>= 7;
        if value == 0 {
            data.push(byte);
            return;
        }
        data.push(byte | 0x80);
    }
}

fn append_signed_leb128(data: &mut Vec<u8>, mut value: i64) {
    loop {
        let byte = (value & 0x7f) as u8;
        // NB: An arithmetic shift, which keeps the sign.
        value >>= 7;
        let sign_bit_set = byte & 0x40 != 0;
        if (value == 0 && !sign_bit_set) || (value == -1 && sign_bit_set) {
            data.push(byte);
            return;
        }
        data.push(byte | 0x80);
    }
}

/// The bytes of a record, to which the encode functions append values in the format of the record.
pub struct RecordEncoder {
    serialization_type: SerializationType,
    data: Vec<u8>,
}

impl RecordEncoder {
    pub fn new(serialization_type: SerializationType) -> Self {
        let mut data = Vec::new();
        if serialization_type == SerializationType::Storage {
            data.extend_from_slice(&STORAGE_FORMAT_MAGIC);
            append_unsigned_leb128(&mut data, STORAGE_FORMAT_VERSION);
            append_unsigned_leb128(&mut data, STORAGE_FORMAT_FLAGS);
        }
        Self {
            serialization_type,
            data,
        }
    }

    pub fn serialization_type(&self) -> SerializationType {
        self.serialization_type
    }

    pub fn bytes(&self) -> &[u8] {
        &self.data
    }

    pub fn into_bytes(self) -> Vec<u8> {
        self.data
    }

    pub fn encode_u8(&mut self, value: u8) {
        self.data.push(value);
    }

    pub fn encode_bool(&mut self, value: bool) {
        self.encode_u8(u8::from(value));
    }

    pub fn encode_u16(&mut self, value: u16) {
        match self.serialization_type {
            SerializationType::Ipc => self.data.extend_from_slice(&value.to_ne_bytes()),
            SerializationType::Storage => append_unsigned_leb128(&mut self.data, u64::from(value)),
        }
    }

    pub fn encode_i32(&mut self, value: i32) {
        match self.serialization_type {
            SerializationType::Ipc => self.data.extend_from_slice(&value.to_ne_bytes()),
            SerializationType::Storage => append_signed_leb128(&mut self.data, i64::from(value)),
        }
    }

    pub fn encode_u32(&mut self, value: u32) {
        match self.serialization_type {
            SerializationType::Ipc => self.data.extend_from_slice(&value.to_ne_bytes()),
            SerializationType::Storage => append_unsigned_leb128(&mut self.data, u64::from(value)),
        }
    }

    pub fn encode_u64(&mut self, value: u64) {
        match self.serialization_type {
            SerializationType::Ipc => self.data.extend_from_slice(&value.to_ne_bytes()),
            SerializationType::Storage => append_unsigned_leb128(&mut self.data, value),
        }
    }

    pub fn encode_i64(&mut self, value: i64) {
        match self.serialization_type {
            SerializationType::Ipc => self.data.extend_from_slice(&value.to_ne_bytes()),
            SerializationType::Storage => append_signed_leb128(&mut self.data, value),
        }
    }

    pub fn encode_f64(&mut self, value: f64) {
        match self.serialization_type {
            SerializationType::Ipc => self.data.extend_from_slice(&value.to_bits().to_ne_bytes()),
            SerializationType::Storage => self.data.extend_from_slice(&value.to_bits().to_le_bytes()),
        }
    }

    /// The length of a string or of bytes in an IPC record, which LibIPC holds as a u32.
    fn encode_ipc_size(&mut self, size: usize) {
        let size = u32::try_from(size).expect("an IPC record holds fewer than 2^32 elements of a value");
        self.data.extend_from_slice(&size.to_ne_bytes());
    }

    /// A string, whose Latin-1 storage the record keeps: an IPC record holds whether the string has it and the length of
    /// the string, and a storage record holds both in one varint, the length shifted left by one.
    pub fn encode_utf16_string(&mut self, string: Utf16StringUnits<'_>) {
        let (has_latin1_storage, length) = match string {
            Utf16StringUnits::Ascii(characters) | Utf16StringUnits::Latin1(characters) => (true, characters.len()),
            Utf16StringUnits::Utf16(code_units) => (false, code_units.len()),
        };
        match self.serialization_type {
            SerializationType::Ipc => {
                self.encode_bool(has_latin1_storage);
                self.encode_ipc_size(length);
            }
            SerializationType::Storage => {
                append_unsigned_leb128(&mut self.data, ((length as u64) << 1) | u64::from(has_latin1_storage));
            }
        }
        match string {
            Utf16StringUnits::Ascii(characters) | Utf16StringUnits::Latin1(characters) => {
                self.data.extend_from_slice(characters);
            }
            Utf16StringUnits::Utf16(code_units) => {
                for &code_unit in code_units {
                    match self.serialization_type {
                        SerializationType::Ipc => self.data.extend_from_slice(&code_unit.to_ne_bytes()),
                        SerializationType::Storage => self.data.extend_from_slice(&code_unit.to_le_bytes()),
                    }
                }
            }
        }
    }

    pub fn encode_bytes(&mut self, bytes: &[u8]) {
        match self.serialization_type {
            SerializationType::Ipc => self.encode_ipc_size(bytes.len()),
            SerializationType::Storage => append_unsigned_leb128(&mut self.data, bytes.len() as u64),
        }
        self.data.extend_from_slice(bytes);
    }

    /// A BigInt, as the bytes that LibCrypto's SignedBigInteger::export_data() writes: 1 if the value is negative and 0
    /// otherwise, then its magnitude, big-endian, without leading zeros, which is empty for zero.
    pub fn encode_big_int(&mut self, is_negative: bool, magnitude: &[u8]) {
        let mut bytes = Vec::with_capacity(1 + magnitude.len());
        bytes.push(u8::from(is_negative));
        bytes.extend_from_slice(magnitude);
        self.encode_bytes(&bytes);
    }
}

fn storage_size(size: u64) -> Result<usize, DecodeError> {
    usize::try_from(size).map_err(|_| DecodeError("Structured serialize storage size does not fit on this platform"))
}

/// Why a record cannot be read: it does not hold what reading it expects.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct DecodeError(pub &'static str);

/// A string that a record holds: the characters of a string with Latin-1 storage, or the code units of any other.
#[derive(Debug, PartialEq, Eq)]
pub enum DecodedString<'a> {
    Latin1(&'a [u8]),
    Utf16(Vec<u16>),
}

/// The bytes of a record, from which the decode functions read what the encode functions of RecordEncoder wrote. They
/// reject what AK's decoding of the formats rejected, as a truncated record or a varint too long for its type, and
/// accept the varints it accepted, as one with redundant bytes.
pub struct RecordDecoder<'a> {
    serialization_type: SerializationType,
    data: &'a [u8],
    position: Cell<usize>,
}

impl<'a> RecordDecoder<'a> {
    /// A decoder of the bytes of a record, which reads the prologue of a storage record.
    pub fn new(serialization_type: SerializationType, data: &'a [u8]) -> Result<Self, DecodeError> {
        let decoder = Self {
            serialization_type,
            data,
            position: Cell::new(0),
        };
        if serialization_type == SerializationType::Storage {
            if decoder.read_bytes(STORAGE_FORMAT_MAGIC.len())? != STORAGE_FORMAT_MAGIC {
                return Err(DecodeError("Invalid structured serialize storage magic"));
            }
            if decoder.decode_u64()? != STORAGE_FORMAT_VERSION {
                return Err(DecodeError("Unsupported structured serialize storage version"));
            }
            if decoder.decode_u64()? != STORAGE_FORMAT_FLAGS {
                return Err(DecodeError("Unsupported structured serialize storage flags"));
            }
        }
        Ok(decoder)
    }

    pub fn serialization_type(&self) -> SerializationType {
        self.serialization_type
    }

    /// Whether the decoder has read every byte of the record.
    pub fn is_at_end(&self) -> bool {
        self.position.get() == self.data.len()
    }

    fn remaining(&self) -> usize {
        self.data.len() - self.position.get()
    }

    fn read_bytes(&self, count: usize) -> Result<&'a [u8], DecodeError> {
        if count > self.remaining() {
            return Err(DecodeError("Truncated structured serialization record"));
        }
        let start = self.position.get();
        self.position.set(start + count);
        Ok(&self.data[start..start + count])
    }

    fn read_array<const N: usize>(&self) -> Result<[u8; N], DecodeError> {
        Ok(self.read_bytes(N)?.try_into().expect("read_bytes() reads N bytes"))
    }

    /// An unsigned LEB128 varint of a value of `bits` bits, as AK::LEB128 reads one: the last byte that can hold bits
    /// of the value ends the varint, and holds no bits that the value does not have.
    fn read_unsigned_leb128(&self, bits: u32) -> Result<u64, DecodeError> {
        let mut value = 0;
        let mut shift = 0;
        loop {
            let byte = self.decode_u8()?;
            value |= u64::from(byte & 0x7f) << shift;
            if shift + 7 >= bits {
                if u32::from(byte) >> (bits - shift) != 0 {
                    return Err(DecodeError("LEB128 varint does not fit its type"));
                }
                return Ok(value);
            }
            if byte & 0x80 == 0 {
                return Ok(value);
            }
            shift += 7;
        }
    }

    /// A signed LEB128 varint of a value of `bits` bits, as AK::LEB128 reads one: the last byte that can hold bits of
    /// the value ends the varint, and its bits that the value does not have are copies of the sign bit.
    fn read_signed_leb128(&self, bits: u32) -> Result<i64, DecodeError> {
        let mut value: u64 = 0;
        let mut shift = 0;
        loop {
            let byte = self.decode_u8()?;
            value |= u64::from(byte & 0x7f) << shift;
            if shift + 7 >= bits {
                let sign_and_unused = ((byte << 1) as i8) >> (bits - shift);
                if byte & 0x80 != 0 || (sign_and_unused != 0 && sign_and_unused != -1) {
                    return Err(DecodeError("LEB128 varint does not fit its type"));
                }
                let unused = 64 - bits;
                return Ok(((value << unused) as i64) >> unused);
            }
            shift += 7;
            if byte & 0x80 == 0 {
                let mut value = value as i64;
                if byte & 0x40 != 0 {
                    value |= -1 << shift;
                }
                return Ok(value);
            }
        }
    }

    /// The length of a string or of bytes: in an IPC record a u32, which is no more than the bytes of the record that
    /// follow it, as LibIPC reads one, and in a storage record a varint.
    fn decode_size(&self) -> Result<usize, DecodeError> {
        match self.serialization_type {
            SerializationType::Ipc => {
                let size = u32::from_ne_bytes(self.read_array()?) as usize;
                if size > self.remaining() {
                    return Err(DecodeError("IPC decode: Size exceeds the rest of the message"));
                }
                Ok(size)
            }
            SerializationType::Storage => storage_size(self.decode_u64()?),
        }
    }

    pub fn decode_u8(&self) -> Result<u8, DecodeError> {
        Ok(self.read_array::<1>()?[0])
    }

    pub fn decode_bool(&self) -> Result<bool, DecodeError> {
        match self.decode_u8()? {
            0 => Ok(false),
            1 => Ok(true),
            _ => Err(DecodeError("Invalid structured serialization bool")),
        }
    }

    pub fn decode_u16(&self) -> Result<u16, DecodeError> {
        match self.serialization_type {
            SerializationType::Ipc => Ok(u16::from_ne_bytes(self.read_array()?)),
            SerializationType::Storage => Ok(self.read_unsigned_leb128(16)? as u16),
        }
    }

    pub fn decode_i32(&self) -> Result<i32, DecodeError> {
        match self.serialization_type {
            SerializationType::Ipc => Ok(i32::from_ne_bytes(self.read_array()?)),
            SerializationType::Storage => Ok(self.read_signed_leb128(32)? as i32),
        }
    }

    pub fn decode_u32(&self) -> Result<u32, DecodeError> {
        match self.serialization_type {
            SerializationType::Ipc => Ok(u32::from_ne_bytes(self.read_array()?)),
            SerializationType::Storage => Ok(self.read_unsigned_leb128(32)? as u32),
        }
    }

    pub fn decode_u64(&self) -> Result<u64, DecodeError> {
        match self.serialization_type {
            SerializationType::Ipc => Ok(u64::from_ne_bytes(self.read_array()?)),
            SerializationType::Storage => self.read_unsigned_leb128(64),
        }
    }

    pub fn decode_i64(&self) -> Result<i64, DecodeError> {
        match self.serialization_type {
            SerializationType::Ipc => Ok(i64::from_ne_bytes(self.read_array()?)),
            SerializationType::Storage => self.read_signed_leb128(64),
        }
    }

    pub fn decode_f64(&self) -> Result<f64, DecodeError> {
        match self.serialization_type {
            SerializationType::Ipc => Ok(f64::from_bits(u64::from_ne_bytes(self.read_array()?))),
            SerializationType::Storage => Ok(f64::from_bits(u64::from_le_bytes(self.read_array()?))),
        }
    }

    pub fn decode_utf16_string(&self) -> Result<DecodedString<'a>, DecodeError> {
        let (has_latin1_storage, length) = match self.serialization_type {
            SerializationType::Ipc => (self.decode_bool()?, self.decode_size()?),
            SerializationType::Storage => {
                let header = self.decode_u64()?;
                (header & 1 != 0, storage_size(header >> 1)?)
            }
        };
        if has_latin1_storage {
            return Ok(DecodedString::Latin1(self.read_bytes(length)?));
        }
        let byte_count = length
            .checked_mul(2)
            .ok_or(DecodeError("Truncated structured serialization record"))?;
        let bytes = self.read_bytes(byte_count)?;
        let code_unit_from_bytes = match self.serialization_type {
            SerializationType::Ipc => u16::from_ne_bytes,
            SerializationType::Storage => u16::from_le_bytes,
        };
        Ok(DecodedString::Utf16(
            bytes
                .as_chunks::<2>()
                .0
                .iter()
                .map(|&bytes| code_unit_from_bytes(bytes))
                .collect(),
        ))
    }

    pub fn decode_bytes(&self) -> Result<&'a [u8], DecodeError> {
        let length = self.decode_size()?;
        self.read_bytes(length)
    }

    /// What encode_big_int() writes: whether the value is negative, which any sign but 0 says, and its magnitude.
    pub fn decode_big_int(&self) -> Result<(bool, &'a [u8]), DecodeError> {
        let Some((&sign, magnitude)) = self.decode_bytes()?.split_first() else {
            return Err(DecodeError("Invalid BigInt"));
        };
        Ok((sign != 0, magnitude))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const PROLOGUE: [u8; 6] = [b'L', b'B', b'S', b'C', 0x01, 0x00];

    fn storage(encode: impl FnOnce(&mut RecordEncoder)) -> Vec<u8> {
        let mut encoder = RecordEncoder::new(SerializationType::Storage);
        encode(&mut encoder);
        assert_eq!(encoder.bytes()[..PROLOGUE.len()], PROLOGUE);
        encoder.bytes()[PROLOGUE.len()..].to_vec()
    }

    fn ipc(encode: impl FnOnce(&mut RecordEncoder)) -> Vec<u8> {
        let mut encoder = RecordEncoder::new(SerializationType::Ipc);
        encode(&mut encoder);
        encoder.bytes().to_vec()
    }

    #[test]
    fn a_storage_record_starts_with_the_prologue() {
        assert_eq!(RecordEncoder::new(SerializationType::Storage).bytes(), PROLOGUE);
        assert!(RecordEncoder::new(SerializationType::Ipc).bytes().is_empty());
    }

    #[test]
    fn a_storage_record_holds_integers_as_leb128_varints() {
        assert_eq!(storage(|encoder| encoder.encode_bool(true)), [0x01]);
        assert_eq!(storage(|encoder| encoder.encode_u8(0xff)), [0xff]);
        assert_eq!(storage(|encoder| encoder.encode_u16(300)), [0xac, 0x02]);
        assert_eq!(
            storage(|encoder| encoder.encode_u32(0x0102_0304)),
            [0x84, 0x86, 0x88, 0x08]
        );
        assert_eq!(storage(|encoder| encoder.encode_u64(0)), [0x00]);
        assert_eq!(
            storage(|encoder| encoder.encode_u64(u64::MAX)),
            [0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x01]
        );
        assert_eq!(storage(|encoder| encoder.encode_i64(-2)), [0x7e]);
        assert_eq!(storage(|encoder| encoder.encode_i64(63)), [0x3f]);
        assert_eq!(storage(|encoder| encoder.encode_i64(64)), [0xc0, 0x00]);
        assert_eq!(storage(|encoder| encoder.encode_i64(-65)), [0xbf, 0x7f]);
        assert_eq!(
            storage(|encoder| encoder.encode_i32(i32::MAX)),
            [0xff, 0xff, 0xff, 0xff, 0x07]
        );
        assert_eq!(
            storage(|encoder| encoder.encode_i32(i32::MIN)),
            [0x80, 0x80, 0x80, 0x80, 0x78]
        );
    }

    #[test]
    fn a_storage_record_holds_a_double_as_its_little_endian_bits() {
        assert_eq!(
            storage(|encoder| encoder.encode_f64(1.0)),
            [0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xf0, 0x3f]
        );
        assert_eq!(storage(|encoder| encoder.encode_f64(-0.0))[7], 0x80);
    }

    #[test]
    fn an_ipc_record_holds_integers_at_their_full_width() {
        assert_eq!(ipc(|encoder| encoder.encode_bool(false)), [0x00]);
        assert_eq!(ipc(|encoder| encoder.encode_u16(0x0102)), 0x0102_u16.to_ne_bytes());
        assert_eq!(ipc(|encoder| encoder.encode_i32(-2)), (-2_i32).to_ne_bytes());
        assert_eq!(
            ipc(|encoder| encoder.encode_u32(0x0102_0304)),
            0x0102_0304_u32.to_ne_bytes()
        );
        assert_eq!(ipc(|encoder| encoder.encode_u64(1)), 1_u64.to_ne_bytes());
        assert_eq!(ipc(|encoder| encoder.encode_i64(-1)), (-1_i64).to_ne_bytes());
        assert_eq!(
            ipc(|encoder| encoder.encode_f64(13.5)),
            13.5_f64.to_bits().to_ne_bytes()
        );
    }

    #[test]
    fn a_storage_record_holds_a_latin1_string_as_one_byte_per_code_unit() {
        assert_eq!(
            storage(|encoder| encoder.encode_utf16_string(Utf16StringUnits::Ascii(b"AB"))),
            [0x05, b'A', b'B']
        );
        assert_eq!(
            storage(|encoder| encoder.encode_utf16_string(Utf16StringUnits::Latin1(&[b'A', 0xe9]))),
            [0x05, b'A', 0xe9]
        );
        assert_eq!(
            storage(|encoder| encoder.encode_utf16_string(Utf16StringUnits::Ascii(b""))),
            [0x01]
        );
    }

    #[test]
    fn a_storage_record_holds_a_utf16_string_as_little_endian_code_units() {
        assert_eq!(
            storage(|encoder| encoder.encode_utf16_string(Utf16StringUnits::Utf16(&[0x41, 0xd800, 0x42]))),
            [0x06, 0x41, 0x00, 0x00, 0xd8, 0x42, 0x00]
        );
    }

    #[test]
    fn a_storage_record_holds_the_length_of_a_string_as_a_varint() {
        let characters = [b'a'; 64];
        assert_eq!(
            storage(|encoder| encoder.encode_utf16_string(Utf16StringUnits::Ascii(&characters[..63])))[..1],
            [0x7f]
        );
        assert_eq!(
            storage(|encoder| encoder.encode_utf16_string(Utf16StringUnits::Ascii(&characters)))[..2],
            [0x81, 0x01]
        );
    }

    #[test]
    fn an_ipc_record_holds_the_storage_and_the_length_of_a_string_before_it() {
        let mut expected = vec![0x01];
        expected.extend_from_slice(&2_u32.to_ne_bytes());
        expected.extend_from_slice(b"AB");
        assert_eq!(
            ipc(|encoder| encoder.encode_utf16_string(Utf16StringUnits::Ascii(b"AB"))),
            expected
        );

        let mut expected = vec![0x00];
        expected.extend_from_slice(&1_u32.to_ne_bytes());
        expected.extend_from_slice(&0xd800_u16.to_ne_bytes());
        assert_eq!(
            ipc(|encoder| encoder.encode_utf16_string(Utf16StringUnits::Utf16(&[0xd800]))),
            expected
        );
    }

    #[test]
    fn a_record_holds_the_length_of_bytes_before_them() {
        assert_eq!(
            storage(|encoder| encoder.encode_bytes(&[0xde, 0xad])),
            [0x02, 0xde, 0xad]
        );
        assert_eq!(storage(|encoder| encoder.encode_bytes(&[])), [0x00]);

        let mut expected = 2_u32.to_ne_bytes().to_vec();
        expected.extend_from_slice(&[0xde, 0xad]);
        assert_eq!(ipc(|encoder| encoder.encode_bytes(&[0xde, 0xad])), expected);
    }

    #[test]
    fn a_record_holds_a_big_int_as_its_sign_then_its_magnitude() {
        assert_eq!(storage(|encoder| encoder.encode_big_int(false, &[])), [0x01, 0x00]);
        assert_eq!(
            storage(|encoder| encoder.encode_big_int(false, &[0x01, 0x00])),
            [0x03, 0x00, 0x01, 0x00]
        );
        assert_eq!(
            storage(|encoder| encoder.encode_big_int(true, &[0x05])),
            [0x02, 0x01, 0x05]
        );
    }

    fn storage_record(value: &[u8]) -> Vec<u8> {
        let mut bytes = PROLOGUE.to_vec();
        bytes.extend_from_slice(value);
        bytes
    }

    fn storage_decoder(bytes: &[u8]) -> RecordDecoder<'_> {
        RecordDecoder::new(SerializationType::Storage, bytes).expect("the record has a prologue")
    }

    fn round_trip(
        serialization_type: SerializationType,
        encode: impl FnOnce(&mut RecordEncoder),
        decode: impl FnOnce(&RecordDecoder),
    ) {
        let mut encoder = RecordEncoder::new(serialization_type);
        encode(&mut encoder);
        let decoder = RecordDecoder::new(serialization_type, encoder.bytes()).expect("the record has a prologue");
        decode(&decoder);
        assert!(decoder.is_at_end());
    }

    #[test]
    fn every_value_round_trips_through_both_formats() {
        for serialization_type in [SerializationType::Ipc, SerializationType::Storage] {
            round_trip(
                serialization_type,
                |encoder| {
                    encoder.encode_bool(false);
                    encoder.encode_bool(true);
                    encoder.encode_u8(0xab);
                    encoder.encode_u16(0xfedc);
                    encoder.encode_i32(-12345);
                    encoder.encode_i32(i32::MIN);
                    encoder.encode_u32(0xfedc_ba98);
                    encoder.encode_u64(0x0102_0304_0506_0708);
                    encoder.encode_u64(u64::MAX);
                    encoder.encode_i64(-0x0102_0304_0506_0708);
                    encoder.encode_i64(i64::MIN);
                    encoder.encode_f64(13.5);
                    encoder.encode_f64(-0.0);
                    encoder.encode_utf16_string(Utf16StringUnits::Ascii(b"stored string"));
                    encoder.encode_utf16_string(Utf16StringUnits::Utf16(&[0x68, 0xe9, 0xd83e, 0xdeac]));
                    encoder.encode_utf16_string(Utf16StringUnits::Ascii(b""));
                    encoder.encode_bytes(&[0xde, 0xad, 0xbe, 0xef]);
                    encoder.encode_big_int(true, &[0x01, 0x00]);
                },
                |decoder| {
                    assert_eq!(decoder.decode_bool(), Ok(false));
                    assert_eq!(decoder.decode_bool(), Ok(true));
                    assert_eq!(decoder.decode_u8(), Ok(0xab));
                    assert_eq!(decoder.decode_u16(), Ok(0xfedc));
                    assert_eq!(decoder.decode_i32(), Ok(-12345));
                    assert_eq!(decoder.decode_i32(), Ok(i32::MIN));
                    assert_eq!(decoder.decode_u32(), Ok(0xfedc_ba98));
                    assert_eq!(decoder.decode_u64(), Ok(0x0102_0304_0506_0708));
                    assert_eq!(decoder.decode_u64(), Ok(u64::MAX));
                    assert_eq!(decoder.decode_i64(), Ok(-0x0102_0304_0506_0708));
                    assert_eq!(decoder.decode_i64(), Ok(i64::MIN));
                    assert_eq!(decoder.decode_f64(), Ok(13.5));
                    assert!(
                        decoder
                            .decode_f64()
                            .is_ok_and(|value| value == 0.0 && value.is_sign_negative())
                    );
                    assert_eq!(
                        decoder.decode_utf16_string(),
                        Ok(DecodedString::Latin1(b"stored string"))
                    );
                    assert_eq!(
                        decoder.decode_utf16_string(),
                        Ok(DecodedString::Utf16(vec![0x68, 0xe9, 0xd83e, 0xdeac]))
                    );
                    assert_eq!(decoder.decode_utf16_string(), Ok(DecodedString::Latin1(b"")));
                    assert_eq!(decoder.decode_bytes(), Ok(&[0xde, 0xad, 0xbe, 0xef][..]));
                    assert_eq!(decoder.decode_big_int(), Ok((true, &[0x01, 0x00][..])));
                },
            );
        }
    }

    #[test]
    fn a_storage_record_needs_the_prologue() {
        assert!(RecordDecoder::new(SerializationType::Storage, &PROLOGUE).is_ok());
        assert!(RecordDecoder::new(SerializationType::Storage, b"LBS").is_err());
        assert!(RecordDecoder::new(SerializationType::Storage, &[b'L', b'B', b'S', b'D', 0x01, 0x00]).is_err());
        assert!(RecordDecoder::new(SerializationType::Storage, &[b'L', b'B', b'S', b'C', 0x02, 0x00]).is_err());
        assert!(RecordDecoder::new(SerializationType::Storage, &[b'L', b'B', b'S', b'C', 0x01, 0x01]).is_err());
        assert!(RecordDecoder::new(SerializationType::Ipc, &[]).is_ok());
    }

    #[test]
    fn a_record_holds_a_bool_as_0_or_1() {
        let record = storage_record(&[0x02]);
        assert!(storage_decoder(&record).decode_bool().is_err());
        assert!(
            RecordDecoder::new(SerializationType::Ipc, &[0x02]).is_ok_and(|decoder| decoder.decode_bool().is_err())
        );
    }

    #[test]
    fn a_varint_ends_at_the_last_byte_its_type_can_hold() {
        // A non-canonical varint that its type can hold reads as its value.
        let record = storage_record(&[0x81, 0x80, 0x80, 0x80, 0x00]);
        assert_eq!(storage_decoder(&record).decode_u32(), Ok(1));

        let record = storage_record(&[0xff, 0xff, 0xff, 0xff, 0x0f]);
        assert_eq!(storage_decoder(&record).decode_u32(), Ok(u32::MAX));
        let record = storage_record(&[0xff, 0xff, 0xff, 0xff, 0x1f]);
        assert!(storage_decoder(&record).decode_u32().is_err());
        let record = storage_record(&[0x80, 0x80, 0x80, 0x80, 0x80, 0x00]);
        assert!(storage_decoder(&record).decode_u32().is_err());

        let record = storage_record(&[0xff, 0xff, 0x03]);
        assert_eq!(storage_decoder(&record).decode_u16(), Ok(u16::MAX));
        let record = storage_record(&[0xff, 0xff, 0x07]);
        assert!(storage_decoder(&record).decode_u16().is_err());

        let record = storage_record(&[0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x02]);
        assert!(storage_decoder(&record).decode_u64().is_err());

        let record = storage_record(&[0x80]);
        assert!(storage_decoder(&record).decode_u64().is_err());
    }

    #[test]
    fn a_signed_varint_sign_extends_its_last_byte() {
        let record = storage_record(&[0xff, 0xff, 0xff, 0xff, 0x07]);
        assert_eq!(storage_decoder(&record).decode_i32(), Ok(i32::MAX));
        let record = storage_record(&[0x80, 0x80, 0x80, 0x80, 0x78]);
        assert_eq!(storage_decoder(&record).decode_i32(), Ok(i32::MIN));
        // The unused bits of the last byte differ from the sign bit.
        let record = storage_record(&[0xff, 0xff, 0xff, 0xff, 0x0f]);
        assert!(storage_decoder(&record).decode_i32().is_err());
        let record = storage_record(&[0x80, 0x80, 0x80, 0x80, 0x80, 0x00]);
        assert!(storage_decoder(&record).decode_i32().is_err());
        let record = storage_record(&[0xff]);
        assert!(storage_decoder(&record).decode_i32().is_err());

        let record = storage_record(&[0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x7f]);
        assert_eq!(storage_decoder(&record).decode_i64(), Ok(i64::MIN));
        let record = storage_record(&[0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x01]);
        assert!(storage_decoder(&record).decode_i64().is_err());
    }

    #[test]
    fn a_string_holds_the_characters_its_header_claims() {
        // Latin-1 storage that holds a byte that is not ASCII.
        let record = storage_record(&[0x03, 0xe9]);
        assert_eq!(
            storage_decoder(&record).decode_utf16_string(),
            Ok(DecodedString::Latin1(&[0xe9]))
        );

        // Fewer bytes than the length claims.
        let record = storage_record(&[0x07, b'a', b'b']);
        assert!(storage_decoder(&record).decode_utf16_string().is_err());
        let record = storage_record(&[0x04, 0x41, 0x00, 0x42]);
        assert!(storage_decoder(&record).decode_utf16_string().is_err());
        let record = storage_record(&[0xfe, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x01]);
        assert!(storage_decoder(&record).decode_utf16_string().is_err());

        // A non-canonical length.
        let record = storage_record(&[0x85, 0x00, b'a', b'b']);
        assert_eq!(
            storage_decoder(&record).decode_utf16_string(),
            Ok(DecodedString::Latin1(b"ab"))
        );

        // A string ends at its length, not at the end of the record.
        let record = storage_record(&[0x03, b'a', b'b']);
        let decoder = storage_decoder(&record);
        assert_eq!(decoder.decode_utf16_string(), Ok(DecodedString::Latin1(b"a")));
        assert!(!decoder.is_at_end());
    }

    #[test]
    fn bytes_need_their_whole_length() {
        let record = storage_record(&[0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x01]);
        assert!(storage_decoder(&record).decode_bytes().is_err());

        let mut record = 3_u32.to_ne_bytes().to_vec();
        record.extend_from_slice(&[0x01, 0x02]);
        assert!(
            RecordDecoder::new(SerializationType::Ipc, &record).is_ok_and(|decoder| decoder.decode_bytes().is_err())
        );
    }

    #[test]
    fn a_big_int_has_a_sign() {
        let record = storage_record(&[0x00]);
        assert!(storage_decoder(&record).decode_big_int().is_err());
        let record = storage_record(&[0x01, 0x00]);
        assert_eq!(storage_decoder(&record).decode_big_int(), Ok((false, &[][..])));
    }
}
