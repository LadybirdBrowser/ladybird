/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Key ranges as data, https://w3c.github.io/IndexedDB/#range-construct, and keys in an order that sorted maps use

use std::rc::Rc;

use core::cmp::Ordering;

use libipc_rust::wire::{Decode, Encode};

use crate::indexed_db::key::Key;

/// A key that sorts as https://w3c.github.io/IndexedDB/#compare-two-keys orders keys.
#[derive(Clone)]
pub struct OrderedKey(pub Rc<Key>);

impl OrderedKey {
    pub fn key(&self) -> &Rc<Key> {
        &self.0
    }
}

impl PartialEq for OrderedKey {
    fn eq(&self, other: &Self) -> bool {
        Key::compare_two_keys(&self.0, &other.0) == 0
    }
}

impl Eq for OrderedKey {}

impl PartialOrd for OrderedKey {
    fn partial_cmp(&self, other: &Self) -> Option<Ordering> {
        Some(self.cmp(other))
    }
}

impl Ord for OrderedKey {
    fn cmp(&self, other: &Self) -> Ordering {
        Key::compare_two_keys(&self.0, &other.0).cmp(&0)
    }
}

impl core::fmt::Debug for OrderedKey {
    fn fmt(&self, formatter: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        write!(formatter, "{:?}", self.0.r#type())
    }
}

// https://w3c.github.io/IndexedDB/#range-construct
#[derive(Clone, Default, Encode, Decode)]
pub struct KeyRange {
    // A key range has an associated lower bound (null or a key).
    pub lower: Option<Key>,

    // A key range has an associated upper bound (null or a key).
    pub upper: Option<Key>,

    // A key range has an associated lower open flag. Unless otherwise stated it is false.
    pub lower_open: bool,

    // A key range has an associated upper open flag. Unless otherwise stated it is false.
    pub upper_open: bool,
}

impl KeyRange {
    /// The range that contains every key, which an unbounded key range is.
    pub fn unbounded() -> Self {
        Self::default()
    }

    // https://w3c.github.io/IndexedDB/#only
    pub fn only(key: Key) -> Self {
        Self {
            lower: Some(key.clone()),
            upper: Some(key),
            lower_open: false,
            upper_open: false,
        }
    }

    // https://w3c.github.io/IndexedDB/#in
    pub fn contains(&self, key: &Key) -> bool {
        // A key is in a key range range if both of the following conditions are fulfilled:
        // * The range's lower bound is null, or it is less than key, or it is both equal to key and the range's lower
        //   open flag is false.
        if let Some(lower) = &self.lower {
            let comparison = Key::compare_two_keys(lower, key);
            if comparison > 0 || (comparison == 0 && self.lower_open) {
                return false;
            }
        }

        // * The range's upper bound is null, or it is greater than key, or it is both equal to key and the range's
        //   upper open flag is false.
        if let Some(upper) = &self.upper {
            let comparison = Key::compare_two_keys(upper, key);
            if comparison < 0 || (comparison == 0 && self.upper_open) {
                return false;
            }
        }

        true
    }

    /// The bounds of the range for a sorted map's range queries.
    pub fn bounds(&self) -> (core::ops::Bound<OrderedKey>, core::ops::Bound<OrderedKey>) {
        use core::ops::Bound;
        let lower = match &self.lower {
            None => Bound::Unbounded,
            Some(key) if self.lower_open => Bound::Excluded(OrderedKey(Rc::new(key.clone()))),
            Some(key) => Bound::Included(OrderedKey(Rc::new(key.clone()))),
        };
        let upper = match &self.upper {
            None => Bound::Unbounded,
            Some(key) if self.upper_open => Bound::Excluded(OrderedKey(Rc::new(key.clone()))),
            Some(key) => Bound::Included(OrderedKey(Rc::new(key.clone()))),
        };
        (lower, upper)
    }

    /// Whether no key can be in the range, which a range query must not be handed: a sorted map's range panics on a
    /// lower bound above its upper bound, or on equal bounds that are both excluded.
    pub fn is_empty(&self) -> bool {
        let (Some(lower), Some(upper)) = (&self.lower, &self.upper) else {
            return false;
        };
        let comparison = Key::compare_two_keys(lower, upper);
        comparison > 0 || (comparison == 0 && (self.lower_open || self.upper_open))
    }
}
