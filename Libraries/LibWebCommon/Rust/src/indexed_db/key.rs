/*
 * Copyright (c) 2024, stelar7 <dudedbz@gmail.com>
 * Copyright (c) 2026, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Keys, https://w3c.github.io/IndexedDB/#key-construct

use std::rc::Rc;

use core::cmp::Ordering;

use ak::{Utf16String, Utf16StringUnits};
use libipc_rust::wire::{Decode, DecodeError, Decoder, Encode, EncodeError, Encoder};

// A key has an associated type which is one of: number, date, string, binary, or array.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum KeyType {
    Invalid,
    Number,
    Date,
    String,
    Binary,
    Array,
}

// A key also has an associated value, which will be either:
// * an unrestricted double if type is number or date,
// * a DOMString if type is string,
// * a byte sequence if type is binary,
// * a list of other keys if type is array.
#[derive(Clone)]
pub enum KeyValue {
    Double(f64),
    String(Utf16String),
    Bytes(Vec<u8>),
    Keys(Vec<Rc<Key>>),
}

// https://w3c.github.io/IndexedDB/#key-construct
#[derive(Clone)]
pub struct Key {
    r#type: KeyType,
    value: KeyValue,
}

impl Key {
    pub fn create(r#type: KeyType, value: KeyValue) -> Rc<Key> {
        Rc::new(Key { r#type, value })
    }

    pub fn r#type(&self) -> KeyType {
        self.r#type
    }

    pub fn value(&self) -> &KeyValue {
        &self.value
    }

    pub fn is_invalid(&self) -> bool {
        self.r#type == KeyType::Invalid
    }

    pub fn value_as_double(&self) -> f64 {
        match self.value {
            KeyValue::Double(value) => value,
            _ => unreachable!("the key's value is a double"),
        }
    }

    pub fn value_as_string(&self) -> &Utf16String {
        match &self.value {
            KeyValue::String(value) => value,
            _ => unreachable!("the key's value is a string"),
        }
    }

    pub fn value_as_byte_buffer(&self) -> &[u8] {
        match &self.value {
            KeyValue::Bytes(value) => value,
            _ => unreachable!("the key's value is a byte sequence"),
        }
    }

    pub fn value_as_vector(&self) -> &[Rc<Key>] {
        match &self.value {
            KeyValue::Keys(value) => value,
            _ => unreachable!("the key's value is a list of keys"),
        }
    }

    pub fn subkeys(&self) -> &[Rc<Key>] {
        assert_eq!(self.r#type, KeyType::Array);
        self.value_as_vector()
    }

    pub fn create_number(value: f64) -> Rc<Key> {
        Self::create(KeyType::Number, KeyValue::Double(value))
    }

    pub fn create_date(value: f64) -> Rc<Key> {
        Self::create(KeyType::Date, KeyValue::Double(value))
    }

    pub fn create_string(value: Utf16String) -> Rc<Key> {
        Self::create(KeyType::String, KeyValue::String(value))
    }

    pub fn create_binary(value: Vec<u8>) -> Rc<Key> {
        Self::create(KeyType::Binary, KeyValue::Bytes(value))
    }

    pub fn create_array(value: Vec<Rc<Key>>) -> Rc<Key> {
        Self::create(KeyType::Array, KeyValue::Keys(value))
    }

    pub fn create_invalid(value: &str) -> Rc<Key> {
        Self::create(KeyType::Invalid, KeyValue::String(Utf16String::from_utf8(value)))
    }

    // https://w3c.github.io/IndexedDB/#compare-two-keys
    pub fn compare_two_keys(a: &Key, b: &Key) -> i8 {
        // 1. Let ta be the type of a.
        let ta = a.r#type();

        // 2. Let tb be the type of b.
        let tb = b.r#type();

        // 3. If ta does not equal tb, then run these steps:
        if ta != tb {
            // 1. If ta is array, then return 1.
            if ta == KeyType::Array {
                return 1;
            }

            // 2. If tb is array, then return -1.
            if tb == KeyType::Array {
                return -1;
            }

            // 3. If ta is binary, then return 1.
            if ta == KeyType::Binary {
                return 1;
            }

            // 4. If tb is binary, then return -1.
            if tb == KeyType::Binary {
                return -1;
            }

            // 5. If ta is string, then return 1.
            if ta == KeyType::String {
                return 1;
            }

            // 6. If tb is string, then return -1.
            if tb == KeyType::String {
                return -1;
            }

            // 7. If ta is date, then return 1.
            if ta == KeyType::Date {
                return 1;
            }

            // 8. Assert: tb is date.
            assert_eq!(tb, KeyType::Date);

            // 9. Return -1.
            return -1;
        }

        // 4. Let va be the value of a.
        let va = &a.value;

        // 5. Let vb be the value of b.
        let vb = &b.value;

        // 6. Switch on ta:
        match ta {
            KeyType::Invalid => unreachable!("invalid keys are not compared"),
            // number
            // date
            KeyType::Number | KeyType::Date => {
                let (KeyValue::Double(a_value), KeyValue::Double(b_value)) = (va, vb) else {
                    unreachable!("the value of a number or a date key is a double");
                };

                // 1. If va is greater than vb, then return 1.
                if a_value > b_value {
                    return 1;
                }

                // 2. If va is less than vb, then return -1.
                if a_value < b_value {
                    return -1;
                }

                // 3. Return 0.
                0
            }
            // string
            KeyType::String => {
                let (KeyValue::String(a_value), KeyValue::String(b_value)) = (va, vb) else {
                    unreachable!("the value of a string key is a string");
                };

                // 1. If va is code unit less than vb, then return -1.
                if code_unit_less_than(a_value, b_value) {
                    return -1;
                }

                // 2. If vb is code unit less than va, then return 1.
                if code_unit_less_than(b_value, a_value) {
                    return 1;
                }

                // 3. Return 0.
                0
            }
            // binary
            KeyType::Binary => {
                let (KeyValue::Bytes(a_value), KeyValue::Bytes(b_value)) = (va, vb) else {
                    unreachable!("the value of a binary key is a byte sequence");
                };

                // 1. If va is byte less than vb, then return -1.
                if is_byte_less_than(a_value, b_value) {
                    return -1;
                }

                // 2. If vb is byte less than va, then return 1.
                if is_byte_less_than(b_value, a_value) {
                    return 1;
                }

                // 3. Return 0.
                0
            }
            // array
            KeyType::Array => {
                let (KeyValue::Keys(a_value), KeyValue::Keys(b_value)) = (va, vb) else {
                    unreachable!("the value of an array key is a list of keys");
                };

                // 1. Let length be the lesser of va’s size and vb’s size.
                let length = a_value.len().min(b_value.len());

                // 2. Let i be 0.
                let mut i = 0;

                // 3. While i is less than length, then:
                while i < length {
                    // 1. Let c be the result of recursively comparing two keys with va[i] and vb[i].
                    let c = Self::compare_two_keys(&a_value[i], &b_value[i]);

                    // 2. If c is not 0, return c.
                    if c != 0 {
                        return c;
                    }

                    // 3. Increase i by 1.
                    i += 1;
                }

                // 4. If va’s size is greater than vb’s size, then return 1.
                if a_value.len() > b_value.len() {
                    return 1;
                }

                // 5. If va’s size is less than vb’s size, then return -1.
                if a_value.len() < b_value.len() {
                    return -1;
                }

                // 6. Return 0.
                0
            }
        }
    }

    pub fn equals(a: &Key, b: &Key) -> bool {
        Self::compare_two_keys(a, b) == 0
    }

    pub fn less_than(a: &Key, b: &Key) -> bool {
        Self::compare_two_keys(a, b) < 0
    }

    pub fn greater_than(a: &Key, b: &Key) -> bool {
        Self::compare_two_keys(a, b) > 0
    }

    pub fn less_than_or_equal(a: &Key, b: &Key) -> bool {
        Self::compare_two_keys(a, b) <= 0
    }

    pub fn greater_than_or_equal(a: &Key, b: &Key) -> bool {
        Self::compare_two_keys(a, b) >= 0
    }

    /// compare two keys, as an Ordering.
    pub fn ordering(a: &Key, b: &Key) -> Ordering {
        Self::compare_two_keys(a, b).cmp(&0)
    }
}

// https://infra.spec.whatwg.org/#code-unit-less-than
fn code_unit_less_than(a: &Utf16String, b: &Utf16String) -> bool {
    code_unit_order(a, b) == Ordering::Less
}

/// https://infra.spec.whatwg.org/#code-unit-less-than, as an Ordering.
pub fn code_unit_order(a: &Utf16String, b: &Utf16String) -> Ordering {
    match (a.as_units(), b.as_units()) {
        (Utf16StringUnits::Ascii(a), Utf16StringUnits::Ascii(b)) => a.cmp(b),
        _ => a.to_utf16().cmp(&b.to_utf16()),
    }
}

// https://infra.spec.whatwg.org/#byte-less-than
fn is_byte_less_than(a: &[u8], b: &[u8]) -> bool {
    a < b
}

// NB: A key goes between processes as its type, then its value; an array key as its subkeys. An invalid key is never
//     sent, and one that arrives is refused.
impl Encode for Key {
    fn encode(&self, encoder: &mut Encoder) -> Result<(), EncodeError> {
        let tag: u8 = match self.r#type {
            KeyType::Invalid => return Err(EncodeError::InvalidValue("an invalid key")),
            KeyType::Number => 0,
            KeyType::Date => 1,
            KeyType::String => 2,
            KeyType::Binary => 3,
            KeyType::Array => 4,
        };
        encoder.encode(&tag)?;
        match &self.value {
            KeyValue::Double(value) => encoder.encode(value),
            KeyValue::String(value) => encoder.encode(&libipc_rust::wire::Utf16String(value.to_utf16().into_owned())),
            KeyValue::Bytes(value) => encoder.encode(value),
            KeyValue::Keys(subkeys) => {
                encoder.encode(&subkeys.len())?;
                for subkey in subkeys {
                    encoder.encode(subkey.as_ref())?;
                }
                Ok(())
            }
        }
    }
}

/// How deeply array keys may nest in a key that arrives from another process, so a hostile sender can't exhaust the
/// stack of the process that decodes it.
pub const MAX_DECODED_KEY_DEPTH: usize = 1024;

fn decode_key(decoder: &mut Decoder<'_>, depth: usize) -> Result<Key, DecodeError> {
    if depth > MAX_DECODED_KEY_DEPTH {
        return Err(DecodeError::InvalidValue("a key nested too deeply"));
    }
    let key = match decoder.decode::<u8>()? {
        tag @ (0 | 1) => {
            let value = decoder.decode::<f64>()?;
            if value.is_nan() {
                return Err(DecodeError::InvalidValue("a key that is NaN"));
            }
            Key {
                r#type: if tag == 1 { KeyType::Date } else { KeyType::Number },
                value: KeyValue::Double(value),
            }
        }
        2 => Key {
            r#type: KeyType::String,
            value: KeyValue::String(Utf16String::from_utf16(
                &decoder.decode::<libipc_rust::wire::Utf16String>()?.0,
            )),
        },
        3 => Key {
            r#type: KeyType::Binary,
            value: KeyValue::Bytes(decoder.decode()?),
        },
        4 => {
            let count = decoder.decode::<usize>()?;
            // NB: Each subkey takes at least its type byte, so a count beyond the bytes left is a lie.
            let mut subkeys = Vec::with_capacity(count.min(4096));
            for _ in 0..count {
                subkeys.push(Rc::new(decode_key(decoder, depth + 1)?));
            }
            Key {
                r#type: KeyType::Array,
                value: KeyValue::Keys(subkeys),
            }
        }
        _ => return Err(DecodeError::InvalidValue("a key type")),
    };
    Ok(key)
}

impl Decode for Key {
    fn decode(decoder: &mut Decoder<'_>) -> Result<Self, DecodeError> {
        decode_key(decoder, 0)
    }
}
