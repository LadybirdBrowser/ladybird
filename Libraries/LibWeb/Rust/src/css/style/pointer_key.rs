/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Keys that find a counted value by its address and hold it, so that for as long as the key stands no other value can
//! take the address. A key can only be made from a counted reference, so one holding a bare address cannot be written:
//! `Meta/Linters/check_rust_address_keys.py` rejects the casts that would make one.

use crate::css::style_value::RetainedStyleValueData;
use std::borrow::Borrow;
use std::hash::{Hash, Hasher};
use std::sync::Arc;

/// A counted reference, whose address names what it counts for as long as it is held.
pub(crate) trait Counted: Clone {
    fn address(&self) -> usize;
}

impl<T> Counted for Arc<T> {
    fn address(&self) -> usize {
        Arc::as_ptr(self).addr()
    }
}

impl Counted for RetainedStyleValueData {
    fn address(&self) -> usize {
        self.pointer().addr()
    }
}

impl Counted for libgfx_rust::font::FrozenFontListRef {
    fn address(&self) -> usize {
        self.as_raw().addr()
    }
}

/// What a map keyed by [`PointerKey`] is looked up by, which takes no count, so that a hit costs what one by the bare
/// address would. It is made only from a counted reference, and a key is the only way to keep one in a map.
#[derive(Clone, Copy, Eq, Hash, PartialEq)]
pub(crate) struct PointerLookup<E = ()> {
    address: usize,
    extra: E,
}

impl<E> PointerLookup<E> {
    pub(crate) fn new(value: &impl Counted, extra: E) -> Self {
        Self {
            address: value.address(),
            extra,
        }
    }
}

/// A map key naming `T` by its address, with `E`, whatever else the key says. It holds a count of `T`.
#[derive(Clone)]
pub(crate) struct PointerKey<T: Counted, E = ()> {
    lookup: PointerLookup<E>,
    _held: T,
}

impl<T: Counted, E> PointerKey<T, E> {
    pub(crate) fn new(value: &T, extra: E) -> Self {
        Self {
            lookup: PointerLookup::new(value, extra),
            _held: value.clone(),
        }
    }
}

impl<T: Counted, E: Hash> Hash for PointerKey<T, E> {
    fn hash<H: Hasher>(&self, state: &mut H) {
        self.lookup.hash(state);
    }
}

impl<T: Counted, E: Eq> PartialEq for PointerKey<T, E> {
    fn eq(&self, other: &Self) -> bool {
        self.lookup == other.lookup
    }
}

impl<T: Counted, E: Eq> Eq for PointerKey<T, E> {}

impl<T: Counted, E> Borrow<PointerLookup<E>> for PointerKey<T, E> {
    fn borrow(&self) -> &PointerLookup<E> {
        &self.lookup
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_key_keeps_its_value_alive_and_a_lookup_finds_it_by_identity() {
        let mut map = crate::fast_hash::FastMap::default();
        let value = Arc::new(1);
        let equal = Arc::new(1);
        map.insert(PointerKey::new(&value, 2_u8), "value");
        assert_eq!(Arc::strong_count(&value), 2);
        assert_eq!(map.get(&PointerLookup::new(&value, 2_u8)), Some(&"value"));
        assert_eq!(map.get(&PointerLookup::new(&value, 3_u8)), None);
        assert_eq!(map.get(&PointerLookup::new(&equal, 2_u8)), None);
        assert_eq!(Arc::strong_count(&value), 2);
    }
}
