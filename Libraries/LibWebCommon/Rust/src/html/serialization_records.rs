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

    /// A string, whose ASCII storage the record keeps: an IPC record holds whether the string has it and the length of
    /// the string, and a storage record holds both in one varint, the length shifted left by one.
    pub fn encode_utf16_string(&mut self, string: Utf16StringUnits<'_>) {
        let (has_ascii_storage, length) = match string {
            Utf16StringUnits::Ascii(characters) => (true, characters.len()),
            Utf16StringUnits::Utf16(code_units) => (false, code_units.len()),
        };
        match self.serialization_type {
            SerializationType::Ipc => {
                self.encode_bool(has_ascii_storage);
                self.encode_ipc_size(length);
            }
            SerializationType::Storage => {
                append_unsigned_leb128(&mut self.data, ((length as u64) << 1) | u64::from(has_ascii_storage));
            }
        }
        match string {
            Utf16StringUnits::Ascii(characters) => self.data.extend_from_slice(characters),
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
    fn a_storage_record_holds_an_ascii_string_as_one_byte_per_code_unit() {
        assert_eq!(
            storage(|encoder| encoder.encode_utf16_string(Utf16StringUnits::Ascii(b"AB"))),
            [0x05, b'A', b'B']
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
}
