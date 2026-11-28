/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The subset of CBOR, <https://www.rfc-editor.org/rfc/rfc8949>, that CTAP2 messages use,
//! <https://fidoalliance.org/specs/fido-v2.2-ps-20250714/fido-client-to-authenticator-protocol-v2.2-ps-20250714.html>.
//! There are no floating-point numbers, no tags and no items of indefinite length.

/// How deep the arrays and maps of a data item from an authenticator can nest.
const MAXIMUM_NESTING_DEPTH: usize = 16;

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Value {
    Integer(i64),
    Bytes(Vec<u8>),
    Text(String),
    Array(Vec<Value>),
    /// The entries are written in the order given, which must be the canonical one: keys of a lower major type
    /// first, then shorter keys, then keys that sort lower byte by byte.
    Map(Vec<(Value, Value)>),
    Boolean(bool),
    Null,
}

impl From<i64> for Value {
    fn from(value: i64) -> Self {
        Self::Integer(value)
    }
}

impl From<bool> for Value {
    fn from(value: bool) -> Self {
        Self::Boolean(value)
    }
}

impl From<&str> for Value {
    fn from(value: &str) -> Self {
        Self::Text(value.to_owned())
    }
}

impl From<&[u8]> for Value {
    fn from(value: &[u8]) -> Self {
        Self::Bytes(value.to_vec())
    }
}

impl Value {
    /// The value of the entry of a map with the integer key, or nothing when this is not a map or has no such entry.
    pub fn get(&self, key: i64) -> Option<&Value> {
        let Self::Map(entries) = self else {
            return None;
        };
        entries
            .iter()
            .find(|(entry_key, _)| *entry_key == Self::Integer(key))
            .map(|(_, value)| value)
    }

    /// The value of the entry of a map with the text key, or nothing when this is not a map or has no such entry.
    pub fn get_text(&self, key: &str) -> Option<&Value> {
        let Self::Map(entries) = self else {
            return None;
        };
        entries
            .iter()
            .find(|(entry_key, _)| matches!(entry_key, Self::Text(text) if text == key))
            .map(|(_, value)| value)
    }

    pub fn as_integer(&self) -> Option<i64> {
        match self {
            Self::Integer(value) => Some(*value),
            _ => None,
        }
    }

    pub fn as_bytes(&self) -> Option<&[u8]> {
        match self {
            Self::Bytes(bytes) => Some(bytes),
            _ => None,
        }
    }

    pub fn as_text(&self) -> Option<&str> {
        match self {
            Self::Text(text) => Some(text),
            _ => None,
        }
    }

    pub fn as_array(&self) -> Option<&[Value]> {
        match self {
            Self::Array(elements) => Some(elements),
            _ => None,
        }
    }

    pub fn as_boolean(&self) -> Option<bool> {
        match self {
            Self::Boolean(value) => Some(*value),
            _ => None,
        }
    }
}

fn encode_head(buffer: &mut Vec<u8>, major_type: u8, argument: u64) {
    let initial_byte = major_type << 5;
    if argument < 24 {
        buffer.push(initial_byte | argument as u8);
    } else if let Ok(argument) = u8::try_from(argument) {
        buffer.push(initial_byte | 24);
        buffer.push(argument);
    } else if let Ok(argument) = u16::try_from(argument) {
        buffer.push(initial_byte | 25);
        buffer.extend_from_slice(&argument.to_be_bytes());
    } else if let Ok(argument) = u32::try_from(argument) {
        buffer.push(initial_byte | 26);
        buffer.extend_from_slice(&argument.to_be_bytes());
    } else {
        buffer.push(initial_byte | 27);
        buffer.extend_from_slice(&argument.to_be_bytes());
    }
}

fn encode_into(buffer: &mut Vec<u8>, value: &Value) {
    match value {
        Value::Integer(integer) if *integer >= 0 => encode_head(buffer, 0, integer.unsigned_abs()),
        // NB: The argument of a negative integer n is -1 - n.
        Value::Integer(integer) => encode_head(buffer, 1, integer.unsigned_abs() - 1),
        Value::Bytes(bytes) => {
            encode_head(buffer, 2, bytes.len() as u64);
            buffer.extend_from_slice(bytes);
        }
        Value::Text(text) => {
            encode_head(buffer, 3, text.len() as u64);
            buffer.extend_from_slice(text.as_bytes());
        }
        Value::Array(elements) => {
            encode_head(buffer, 4, elements.len() as u64);
            for element in elements {
                encode_into(buffer, element);
            }
        }
        Value::Map(entries) => {
            encode_head(buffer, 5, entries.len() as u64);
            for (key, value) in entries {
                encode_into(buffer, key);
                encode_into(buffer, value);
            }
        }
        Value::Boolean(false) => buffer.push(0xf4),
        Value::Boolean(true) => buffer.push(0xf5),
        Value::Null => buffer.push(0xf6),
    }
}

pub fn encode(value: &Value) -> Vec<u8> {
    let mut buffer = Vec::new();
    encode_into(&mut buffer, value);
    buffer
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum DecodeError {
    CutOff,
    NestedTooDeep,
    /// A simple value other than false, true and null, or a floating-point number.
    UnsupportedSimpleValue,
    IntegerOutOfRange,
    /// An array or a map that says it has more items than its input can hold.
    LongerThanInput,
    UnsupportedMapKey,
    DuplicateMapKey,
    Tag,
    IndefiniteOrReservedLength,
}

struct Decoder<'a> {
    input: &'a [u8],
    offset: usize,
}

impl<'a> Decoder<'a> {
    fn remaining(&self) -> usize {
        self.input.len() - self.offset
    }

    fn read_byte(&mut self) -> Result<u8, DecodeError> {
        let byte = *self.input.get(self.offset).ok_or(DecodeError::CutOff)?;
        self.offset += 1;
        Ok(byte)
    }

    fn read_bytes(&mut self, size: u64) -> Result<&'a [u8], DecodeError> {
        let size = usize::try_from(size).map_err(|_| DecodeError::CutOff)?;
        if size > self.remaining() {
            return Err(DecodeError::CutOff);
        }
        let bytes = &self.input[self.offset..self.offset + size];
        self.offset += size;
        Ok(bytes)
    }

    fn read_argument(&mut self, additional_information: u8) -> Result<u64, DecodeError> {
        let argument_size = match additional_information {
            0..24 => return Ok(u64::from(additional_information)),
            24 => 1,
            25 => 2,
            26 => 4,
            27 => 8,
            _ => return Err(DecodeError::IndefiniteOrReservedLength),
        };
        let bytes = self.read_bytes(argument_size)?;
        Ok(bytes
            .iter()
            .fold(0, |argument, byte| (argument << 8) | u64::from(*byte)))
    }

    fn decode_item(&mut self, depth: usize) -> Result<Value, DecodeError> {
        if depth > MAXIMUM_NESTING_DEPTH {
            return Err(DecodeError::NestedTooDeep);
        }

        let initial_byte = self.read_byte()?;
        let major_type = initial_byte >> 5;
        let additional_information = initial_byte & 0x1f;

        if major_type == 7 {
            return match additional_information {
                20 => Ok(Value::Boolean(false)),
                21 => Ok(Value::Boolean(true)),
                22 => Ok(Value::Null),
                _ => Err(DecodeError::UnsupportedSimpleValue),
            };
        }

        let argument = self.read_argument(additional_information)?;
        match major_type {
            0 => i64::try_from(argument)
                .map(Value::Integer)
                .map_err(|_| DecodeError::IntegerOutOfRange),
            1 => i64::try_from(argument)
                .map(|argument| Value::Integer(-1 - argument))
                .map_err(|_| DecodeError::IntegerOutOfRange),
            2 => Ok(Value::Bytes(self.read_bytes(argument)?.to_vec())),
            3 => {
                let bytes = self.read_bytes(argument)?;
                // NB: A security key can cut the name of an account off in the middle of a character, to make it fit.
                //     The rest of what it sends is still good.
                Ok(Value::Text(String::from_utf8_lossy(bytes).into_owned()))
            }
            4 => {
                // NB: Every element takes one byte or more, so the input bounds the count. The count is that of the
                //     input, which is not trusted, so the elements take room as they come.
                if argument > self.remaining() as u64 {
                    return Err(DecodeError::LongerThanInput);
                }
                let mut elements = Vec::new();
                for _ in 0..argument {
                    elements.push(self.decode_item(depth + 1)?);
                }
                Ok(Value::Array(elements))
            }
            5 => {
                if argument > (self.remaining() / 2) as u64 {
                    return Err(DecodeError::LongerThanInput);
                }
                let mut entries = Vec::new();
                for _ in 0..argument {
                    let key = self.decode_item(depth + 1)?;
                    if !matches!(key, Value::Integer(_) | Value::Text(_) | Value::Bytes(_)) {
                        return Err(DecodeError::UnsupportedMapKey);
                    }
                    // NB: A reader takes the first entry of a key, so a second one would be data that nothing looks at.
                    if entries.iter().any(|(other_key, _)| *other_key == key) {
                        return Err(DecodeError::DuplicateMapKey);
                    }
                    let value = self.decode_item(depth + 1)?;
                    entries.push((key, value));
                }
                Ok(Value::Map(entries))
            }
            _ => Err(DecodeError::Tag),
        }
    }
}

/// Decodes the one data item that the input starts with, and says how many bytes of the input the item took. Input
/// from an authenticator is not trusted.
pub fn decode(input: &[u8]) -> Result<(Value, usize), DecodeError> {
    let mut decoder = Decoder { input, offset: 0 };
    let value = decoder.decode_item(0)?;
    Ok((value, decoder.offset))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn round_trip(value: &Value, encoded: &[u8]) {
        assert_eq!(encode(value), encoded);
        assert_eq!(decode(encoded), Ok((value.clone(), encoded.len())));
    }

    #[test]
    fn integers_take_the_shortest_form() {
        round_trip(&Value::Integer(0), &[0x00]);
        round_trip(&Value::Integer(23), &[0x17]);
        round_trip(&Value::Integer(24), &[0x18, 0x18]);
        round_trip(&Value::Integer(255), &[0x18, 0xff]);
        round_trip(&Value::Integer(256), &[0x19, 0x01, 0x00]);
        round_trip(&Value::Integer(65536), &[0x1a, 0x00, 0x01, 0x00, 0x00]);
        round_trip(
            &Value::Integer(i64::MAX),
            &[0x1b, 0x7f, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff],
        );
        round_trip(&Value::Integer(-1), &[0x20]);
        round_trip(&Value::Integer(-7), &[0x26]);
        round_trip(&Value::Integer(-25), &[0x38, 0x18]);
        round_trip(
            &Value::Integer(i64::MIN),
            &[0x3b, 0x7f, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff],
        );
    }

    #[test]
    fn strings_arrays_and_maps_round_trip() {
        round_trip(&Value::Bytes(vec![1, 2, 3]), &[0x43, 1, 2, 3]);
        round_trip(&Value::from("id"), &[0x62, b'i', b'd']);
        round_trip(
            &Value::Array(vec![Value::Integer(1), Value::Boolean(true), Value::Null]),
            &[0x83, 0x01, 0xf5, 0xf6],
        );
        round_trip(
            &Value::Map(vec![
                (Value::Integer(1), Value::from("a")),
                (Value::from("rk"), Value::Boolean(false)),
            ]),
            &[0xa2, 0x01, 0x61, b'a', 0x62, b'r', b'k', 0xf4],
        );
    }

    #[test]
    fn map_entries_are_found_by_key() {
        let map = Value::Map(vec![
            (Value::Integer(-2), Value::Bytes(vec![9])),
            (Value::from("uv"), Value::Boolean(true)),
        ]);
        assert_eq!(map.get(-2).and_then(Value::as_bytes), Some(&[9][..]));
        assert_eq!(map.get_text("uv").and_then(Value::as_boolean), Some(true));
        assert_eq!(map.get(1), None);
        assert_eq!(map.get_text("rk"), None);
        assert_eq!(Value::Integer(1).get(1), None);
    }

    #[test]
    fn decode_says_how_much_of_the_input_the_item_took() {
        assert_eq!(decode(&[0x01, 0xff, 0xff]), Ok((Value::Integer(1), 1)));
    }

    #[test]
    fn input_that_is_cut_off_is_refused() {
        assert_eq!(decode(&[]), Err(DecodeError::CutOff));
        assert_eq!(decode(&[0x18]), Err(DecodeError::CutOff));
        assert_eq!(decode(&[0x43, 1, 2]), Err(DecodeError::CutOff));
        assert_eq!(decode(&[0x82, 0x01, 0x18]), Err(DecodeError::CutOff));
        // A byte string that says it is longer than any input.
        assert_eq!(
            decode(&[0x5b, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff]),
            Err(DecodeError::CutOff)
        );
    }

    #[test]
    fn counts_that_the_input_cannot_hold_are_refused() {
        assert_eq!(
            decode(&[0x9b, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff]),
            Err(DecodeError::LongerThanInput)
        );
        assert_eq!(decode(&[0xa2, 0x01, 0x02, 0x03]), Err(DecodeError::LongerThanInput));
    }

    #[test]
    fn what_ctap2_does_not_use_is_refused() {
        // A tag, a floating-point number, an indefinite length and the simple value "undefined".
        assert_eq!(decode(&[0xc0, 0x00]), Err(DecodeError::Tag));
        assert_eq!(
            decode(&[0xfb, 0, 0, 0, 0, 0, 0, 0, 0]),
            Err(DecodeError::UnsupportedSimpleValue)
        );
        assert_eq!(decode(&[0x5f, 0xff]), Err(DecodeError::IndefiniteOrReservedLength));
        assert_eq!(decode(&[0xf7]), Err(DecodeError::UnsupportedSimpleValue));
        // A map with an array for a key.
        assert_eq!(decode(&[0xa1, 0x80, 0x00]), Err(DecodeError::UnsupportedMapKey));
        assert_eq!(
            decode(&[0xa2, 0x01, 0xf4, 0x01, 0xf5]),
            Err(DecodeError::DuplicateMapKey)
        );
        // An unsigned integer that is too large for i64.
        assert_eq!(
            decode(&[0x1b, 0x80, 0, 0, 0, 0, 0, 0, 0]),
            Err(DecodeError::IntegerOutOfRange)
        );
    }

    #[test]
    fn text_that_is_cut_off_in_a_character_keeps_what_is_good() {
        // NB: U+00E9 is 0xc3 0xa9 in UTF-8, and the text ends after the first of the two bytes.
        assert_eq!(decode(&[0x63, b'J', b'o', 0xc3]), Ok((Value::from("Jo\u{fffd}"), 4)));
    }

    #[test]
    fn deep_nesting_is_refused() {
        let mut nested = vec![0x81; MAXIMUM_NESTING_DEPTH + 1];
        nested.push(0x00);
        assert_eq!(decode(&nested), Err(DecodeError::NestedTooDeep));
        let mut nested = vec![0x81; MAXIMUM_NESTING_DEPTH];
        nested.push(0x00);
        assert!(decode(&nested).is_ok());
    }
}
