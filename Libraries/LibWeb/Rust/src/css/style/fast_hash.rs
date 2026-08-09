/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! One fast, deterministic hasher for the engine's internal tables.

use std::collections::HashMap;
use std::collections::HashSet;
use std::hash::BuildHasher;
use std::hash::BuildHasherDefault;
use std::hash::Hasher;

use foldhash::fast::FixedState;
pub use foldhash::fast::FoldHasher as FastHasher;

pub(crate) fn fast_hasher() -> FastHasher {
    FixedState::default().build_hasher()
}

pub type FastMap<K, V> = HashMap<K, V, FixedState>;
pub type FastSet<K> = HashSet<K, FixedState>;

// NB: Staged tree rows feed slot assignment order, which is observable through the recorded flat-
//     tree callback. Keep its established iteration until that callback has a separately authorized
//     canonical-order migration; all lookup-only engine tables use FixedState above.
#[derive(Default)]
pub(super) struct StableIterationHasher(u64);

impl StableIterationHasher {
    #[inline]
    fn fold(&mut self, value: u64) {
        self.0 = (self.0.rotate_left(26) ^ value).wrapping_mul(0x2545_f491_4f6c_dd1d);
    }
}

impl Hasher for StableIterationHasher {
    fn finish(&self) -> u64 {
        let mut mixed = self.0;
        mixed ^= mixed >> 32;
        mixed = mixed.wrapping_mul(0x2545_f491_4f6c_dd1d);
        mixed ^ (mixed >> 29)
    }

    fn write(&mut self, bytes: &[u8]) {
        let mut chunks = bytes.chunks_exact(size_of::<u64>());
        for chunk in &mut chunks {
            self.fold(u64::from_le_bytes(chunk.try_into().unwrap()));
        }
        let remainder = chunks.remainder();
        if !remainder.is_empty() {
            let mut tail = [0_u8; size_of::<u64>()];
            tail[..remainder.len()].copy_from_slice(remainder);
            self.fold(u64::from_le_bytes(tail) | ((remainder.len() as u64) << 56));
        }
    }

    fn write_u32(&mut self, value: u32) {
        self.fold(u64::from(value));
    }
}

pub(super) type StableIterationMap<K, V> = HashMap<K, V, BuildHasherDefault<StableIterationHasher>>;
