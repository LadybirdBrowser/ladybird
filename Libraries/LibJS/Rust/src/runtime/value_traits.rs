/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Hashing values the way Map and Set tell their keys apart.

use core::hash::{BuildHasher, Hash, Hasher};

use crate::layout::value::Value;
use crate::runtime::value::same_value;

/// A value as a hash key: hashed by its contents and compared with SameValue.
#[derive(Clone, Copy)]
pub struct ValueTraitsKey(pub Value);

impl Hash for ValueTraitsKey {
    fn hash<H: Hasher>(&self, state: &mut H) {
        let mut value = self.0;
        assert!(!value.is_empty());
        if value.is_string() {
            for code_unit in value.as_string().utf16_string_view().code_units() {
                state.write_u16(code_unit);
            }
            return;
        }

        if value.is_bigint() {
            value.as_bigint().big_integer().hash(state);
            return;
        }

        // In the IEEE 754 standard a NaN value is encoded as any value from 0x7ff0000000000001 to 0x7fffffffffffffff,
        // with the least significant bits (referred to as the 'payload') carrying some kind of diagnostic information
        // indicating the source of the NaN. Since ECMA262 does not differentiate between different kinds of NaN values,
        // Sets and Maps must not differentiate between them either.
        // This is achieved by replacing any NaN value by a canonical qNaN.
        if value.is_nan() {
            value = Value::from_f64(f64::NAN);
        }

        // FIXME: Is this the best way to hash pointers, doubles & ints?
        state.write_u64(value.0);
    }
}

impl PartialEq for ValueTraitsKey {
    fn eq(&self, other: &Self) -> bool {
        same_value(self.0, other.0)
    }
}

impl Eq for ValueTraitsKey {}

/// ValueTraits::hash(), for tables that keep their values elsewhere and only hold the hashes.
pub fn value_traits_hash(value: Value) -> u64 {
    foldhash::fast::FixedState::default().hash_one(ValueTraitsKey(value))
}
