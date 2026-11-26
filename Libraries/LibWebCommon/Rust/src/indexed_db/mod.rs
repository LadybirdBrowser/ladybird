/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The parts of IndexedDB that need no script: keys and their order, key ranges, key generators, and the backend that
//! holds every agent's databases, https://w3c.github.io/IndexedDB/

pub mod backend;
pub mod ffi;
pub mod key;
pub mod key_generator;
pub mod key_range;

/// The name of a database, an object store or an index, as the backend holds and compares it: its UTF-16 code units,
/// lone surrogates and all.
pub type Name = libipc_rust::wire::Utf16String;
