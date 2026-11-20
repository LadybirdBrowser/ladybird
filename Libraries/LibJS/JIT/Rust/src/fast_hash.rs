/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Hash maps and sets with a fast hasher (see `ak::FastHasher`), for the compiler's keys.

use std::hash::BuildHasherDefault;

use ak::FastHasher;

pub type HashMap<K, V> = std::collections::HashMap<K, V, BuildHasherDefault<FastHasher>>;
pub type HashSet<K> = std::collections::HashSet<K, BuildHasherDefault<FastHasher>>;
