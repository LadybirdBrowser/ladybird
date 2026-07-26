/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Hash tables for the compiler's own bookkeeping.
//!
//! Nearly every key here is a small dense index, or a value carrying an
//! identity hash it computed once. The standard library's hasher is built to
//! resist keys chosen by an attacker, which none of these tables ever see, and
//! hashing shows up across the whole compiler when compiling a program: the
//! tables are rebuilt from nothing on every compile.

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
        let mut chunks = bytes.chunks_exact(8);
        for chunk in &mut chunks {
            self.add(u64::from_le_bytes(
                chunk.try_into().expect("a chunk of eight bytes is eight bytes"),
            ));
        }
        let mut remainder = [0; 8];
        remainder[..chunks.remainder().len()].copy_from_slice(chunks.remainder());
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
