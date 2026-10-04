/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! https://drafts.csswg.org/css-values-5/#random-caching
//! The random base value each random caching key of the document has been given.

use super::RetainedState;
use super::fast_hash::FastMap as HashMap;
use super::tree::StyleNodeID;
use std::hash::BuildHasher;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};

/// The random base values a document's random functions have drawn, by random caching key. The
/// key's document is the engine's own; its element is the node, or none for an `element-shared`
/// sharing.
#[derive(Default)]
pub(crate) struct RandomBaseValues {
    /// The keys whose element is null, by name.
    document: HashMap<Box<[u16]>, f64>,
    /// The keys that name an element. Few elements draw, and those draw few names, so only they
    /// have a row and a row is searched in order.
    elements: HashMap<StyleNodeID, Vec<NamedBaseValue>>,
    /// Raised once any element has a row, and never lowered. The host watches it so that an element losing its
    /// identity asks for keys only once some element can have any.
    element_rows_exist: Arc<AtomicBool>,
    source: RandomSource,
}

/// A name's base value in an element's row.
pub(crate) type NamedBaseValue = (Box<[u16]>, f64);

/// The row of an element that has no style node, which the element holds: the engine moves the row in as the element
/// loses its node, and out to the node it gets next. Whichever of the element and the engine lets go of it last frees
/// it, so an element that never gets a node again takes its keys with it.
pub(crate) type ParkedBaseValues = Arc<Mutex<Vec<NamedBaseValue>>>;

/// A uniform pseudo-random source: a randomly keyed hash of a draw counter.
#[derive(Default)]
struct RandomSource {
    state: std::collections::hash_map::RandomState,
    draws: u64,
}

impl RandomSource {
    /// A pseudo-random real number in `[0, 1)`, from a uniform distribution.
    fn draw(&mut self) -> f64 {
        self.draws += 1;
        // The top 53 bits fill the mantissa exactly, so the quotient is below one.
        (self.state.hash_one(self.draws) >> 11) as f64 / (1_u64 << 53) as f64
    }
}

impl RandomBaseValues {
    /// The base value of a key, drawn the first time the key is asked for. A key that names an
    /// element with no identity draws every time: nothing could find it again.
    pub(crate) fn ensure(&mut self, node: Option<StyleNodeID>, name: &[u16], element_shared: bool) -> f64 {
        if element_shared {
            if let Some(&value) = self.document.get(name) {
                return value;
            }
            let value = self.source.draw();
            self.document.insert(name.into(), value);
            return value;
        }
        let Some(node) = node else {
            return self.source.draw();
        };
        if let Some(value) = self.get(node, name, false) {
            return value;
        }
        let value = self.source.draw();
        self.row_mut(node).push((name.into(), value));
        value
    }

    /// The base value of a key, if it has been drawn.
    pub(crate) fn get(&self, node: StyleNodeID, name: &[u16], element_shared: bool) -> Option<f64> {
        if element_shared {
            return self.document.get(name).copied();
        }
        self.elements
            .get(&node)?
            .iter()
            .find(|(row_name, _)| **row_name == *name)
            .map(|(_, value)| *value)
    }

    /// Take a key's value as given, for a replay that reproduces what a recorded draw gave.
    #[cfg(feature = "style-recording")]
    pub(crate) fn set(&mut self, node: Option<StyleNodeID>, name: &[u16], element_shared: bool, value: f64) {
        if element_shared {
            self.document.insert(name.into(), value);
            return;
        }
        let Some(node) = node else {
            return;
        };
        let row = self.row_mut(node);
        match row.iter_mut().find(|(row_name, _)| **row_name == *name) {
            Some((_, row_value)) => *row_value = value,
            None => row.push((name.into(), value)),
        }
    }

    /// An element's row, which may be new.
    fn row_mut(&mut self, node: StyleNodeID) -> &mut Vec<NamedBaseValue> {
        self.element_rows_exist.store(true, Ordering::Relaxed);
        self.elements.entry(node).or_default()
    }

    /// Takes the keys that name an element, with their values, as the element loses its identity.
    pub(crate) fn take_element_values(&mut self, node: StyleNodeID) -> Vec<NamedBaseValue> {
        self.elements.remove(&node).unwrap_or_default()
    }

    /// Give up the keys of an identity that retires. An identity can be minted again for another
    /// element, which must not see this one's values; the element itself took its own along when
    /// it lost the identity.
    pub(crate) fn retire(&mut self, node: StyleNodeID) {
        self.elements.remove(&node);
    }
}

impl RetainedState {
    /// Give an element's new style node the keys the element kept while it had none, as one buffer
    /// of name code units with a length and a value per name.
    pub fn set_element_random_base_values(
        &mut self,
        node: StyleNodeID,
        name_lengths: &[u32],
        name_units: &[u16],
        value_bits: &[u64],
    ) {
        assert_eq!(
            name_lengths.len(),
            value_bits.len(),
            "every random base value has one name"
        );
        let mut rest = name_units;
        let row = name_lengths
            .iter()
            .zip(value_bits)
            .map(|(&length, &bits)| {
                let (name, after) = rest
                    .split_at_checked(length as usize)
                    .expect("random base value names overrun their code units");
                rest = after;
                (Box::from(name), f64::from_bits(bits))
            })
            .collect::<Vec<_>>();
        if !row.is_empty() {
            *self.random_base_values.row_mut(node) = row;
        }
    }

    /// Raises `flag`, which the document's host reads, once any element has random base values, rather than a flag
    /// of the engine's own. The engine has none yet.
    pub(crate) fn share_element_random_base_values_exist(&mut self, flag: Arc<AtomicBool>) {
        debug_assert!(!self.random_base_values.element_rows_exist.load(Ordering::Relaxed));
        self.random_base_values.element_rows_exist = flag;
    }

    /// The random base value of the random caching key for a node's style and a sharing name.
    pub(crate) fn ensure_random_base_value(
        &mut self,
        node: Option<StyleNodeID>,
        name: &[u16],
        element_shared: bool,
    ) -> f64 {
        self.random_base_values.ensure(node, name, element_shared)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_key_keeps_its_value_and_an_element_key_retires_with_the_element() {
        let mut values = RandomBaseValues::default();
        let first = StyleNodeID::from_raw(1);
        let second = StyleNodeID::from_raw(2);
        let name = "--shared".encode_utf16().collect::<Vec<_>>();

        let document_value = values.ensure(first, &name, true);
        assert!((0.0..1.0).contains(&document_value));
        assert_eq!(document_value, values.ensure(second, &name, true));

        let element_value = values.ensure(first, &name, false);
        assert_eq!(element_value, values.ensure(first, &name, false));
        values.ensure(second, &name, false);
        assert_eq!(values.document.len(), 1);
        assert_eq!(values.elements.len(), 2);

        values.retire(first.unwrap());
        assert_eq!(values.elements.len(), 1);
        assert_eq!(document_value, values.ensure(second, &name, true));
    }
}
