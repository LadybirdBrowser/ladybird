/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! One fast, deterministic hasher for the engine's internal tables.

use std::collections::HashMap;
use std::collections::HashSet;
use std::hash::BuildHasher;

use foldhash::fast::FixedState;
pub use foldhash::fast::FoldHasher as FastHasher;

pub(crate) fn fast_hasher() -> FastHasher {
    FixedState::default().build_hasher()
}

pub type FastMap<K, V> = HashMap<K, V, FixedState>;
pub type FastSet<K> = HashSet<K, FixedState>;
