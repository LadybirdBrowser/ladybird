/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Lookup tables for common HTML tag and attribute names.
//!
//! The tables are generated at build time from LibWeb's TagNames.h and
//! AttributeNames.h headers so they stay in sync with the C++ side. Each
//! name gets a 1-based u16 id; id 0 is reserved for "not interned" so the
//! FFI layer can carry either an id or raw bytes in the same slot.
//!
//! The lookup functions themselves are generated as a `match` on byte
//! length and exact byte sequence, which rustc compiles to a jump table
//! plus direct memcmp per length bucket. This is dramatically faster than
//! a HashMap lookup with the default cryptographic hasher.

include!(concat!(env!("OUT_DIR"), "/interned_names_generated.rs"));

/// Look up a tag name. Returns 0 if not interned.
#[inline]
pub fn lookup_tag_name(bytes: &[u8]) -> u16 {
    lookup_tag_name_generated(bytes)
}

/// Look up an attribute name. Returns 0 if not interned.
#[inline]
pub fn lookup_attr_name(bytes: &[u8]) -> u16 {
    lookup_attr_name_generated(bytes)
}

/// Number of interned tag names (for C++ table sizing at startup).
#[inline]
pub fn tag_name_count() -> usize {
    INTERNED_TAG_NAMES.len()
}

/// Number of interned attribute names.
#[inline]
pub fn attr_name_count() -> usize {
    INTERNED_ATTR_NAMES.len()
}

/// Fetch a tag name by id (1-based). Returns None for id 0 or out of range.
#[inline]
pub fn tag_name_by_id(id: u16) -> Option<&'static [u8]> {
    if id == 0 {
        return None;
    }
    INTERNED_TAG_NAMES.get((id - 1) as usize).copied()
}

/// Fetch an attribute name by id (1-based). Returns None for id 0 or out of range.
#[inline]
pub fn attr_name_by_id(id: u16) -> Option<&'static [u8]> {
    if id == 0 {
        return None;
    }
    INTERNED_ATTR_NAMES.get((id - 1) as usize).copied()
}
