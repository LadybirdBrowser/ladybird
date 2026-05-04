/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Type aliases that swap the default RandomState (SipHash) for
//! foldhash's quality variant. Used in parser/scope-collector hot paths
//! where keys come from lexer tokens (i.e. attacker-controlled JS
//! source), so we keep formal HashDoS resistance while shedding
//! SipHash's per-byte cost.

pub type HashMap<K, V> = std::collections::HashMap<K, V, foldhash::quality::RandomState>;
pub type HashSet<T> = std::collections::HashSet<T, foldhash::quality::RandomState>;
pub type IndexMap<K, V> = indexmap::IndexMap<K, V, foldhash::quality::RandomState>;
