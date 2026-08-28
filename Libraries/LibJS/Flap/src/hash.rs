/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Hash tables for the compiler's own bookkeeping.
//!
//! Most keys here are small dense indices or values carrying an identity hash
//! computed once. Some front-end tables also contain identifiers copied from
//! source. Using a deterministic hasher for those is safe only under the
//! crate-level contract that Flap compiles trusted generated input; it is not
//! collision resistant and must not silently cross an untrusted-source
//! boundary. Hashing shows up across the whole compiler, and these tables are
//! rebuilt from nothing on every compile.

use std::hash::{BuildHasherDefault, Hasher};

pub(crate) type HashMap<K, V> = std::collections::HashMap<K, V, BuildHasherDefault<Hash>>;
pub(crate) type HashSet<T> = std::collections::HashSet<T, BuildHasherDefault<Hash>>;

/// A multiply-rotate hash, in the style of the one rustc uses for its own
/// interned indices.
#[derive(Default)]
pub(crate) struct Hash {
    state: u64,
}

impl Hash {
    const MULTIPLIER: u64 = 0x517c_c1b7_2722_0a95;

    fn add(&mut self, word: u64) {
        self.state = (self.state.rotate_left(5) ^ word).wrapping_mul(Self::MULTIPLIER);
    }
}

impl Hasher for Hash {
    fn write(&mut self, bytes: &[u8]) {
        let chunks = bytes.as_chunks::<8>();
        for chunk in chunks.0 {
            self.add(u64::from_le_bytes(*chunk));
        }

        let mut remainder = [0; 8];
        remainder[..chunks.1.len()].copy_from_slice(chunks.1);
        self.add(u64::from_le_bytes(remainder));
    }

    fn write_u8(&mut self, value: u8) {
        self.add(u64::from(value));
    }

    fn write_u16(&mut self, value: u16) {
        self.add(u64::from(value));
    }

    fn write_u32(&mut self, value: u32) {
        self.add(u64::from(value));
    }

    fn write_u64(&mut self, value: u64) {
        self.add(value);
    }

    fn write_usize(&mut self, value: usize) {
        self.add(value as u64);
    }

    fn finish(&self) -> u64 {
        self.state
    }
}
