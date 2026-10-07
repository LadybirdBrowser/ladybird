/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::collections::HashMap;

use libjs_runtime_macros::Trace;

use crate::gc::visitor::{Trace, Visitor};
use crate::layout::value::Value;
use crate::runtime::property_attributes::{DEFAULT_ATTRIBUTES, PropertyAttributes};

#[derive(Clone, Copy, Debug, PartialEq, Eq, Trace)]
pub struct ValueAndAttributes {
    pub value: Value,
    #[gc(untraced)]
    pub attributes: PropertyAttributes,
    #[gc(untraced)]
    pub property_offset: Option<u32>,
}

impl ValueAndAttributes {
    pub fn new(value: Value, attributes: PropertyAttributes) -> Self {
        Self {
            value,
            attributes,
            property_offset: None,
        }
    }
}

impl Default for ValueAndAttributes {
    fn default() -> Self {
        Self::new(Value::UNDEFINED, DEFAULT_ATTRIBUTES)
    }
}

/// The indexed properties of an object in dictionary mode, which an object owns and traces. The order of the map is
/// never observable: every caller that enumerates indices sorts them.
#[derive(Default)]
pub struct GenericIndexedPropertyStorage {
    array_size: usize,
    sparse_elements: HashMap<u32, ValueAndAttributes, foldhash::fast::RandomState>,
}

impl GenericIndexedPropertyStorage {
    pub fn external_memory_size(&self) -> usize {
        self.sparse_elements.capacity() * (size_of::<u32>() + size_of::<ValueAndAttributes>())
    }

    pub fn has_index(&self, index: u32) -> bool {
        self.sparse_elements.contains_key(&index)
    }

    pub fn get(&self, index: u32) -> Option<ValueAndAttributes> {
        if index as usize >= self.array_size {
            return None;
        }
        self.sparse_elements.get(&index).copied()
    }

    pub fn put(&mut self, index: u32, value: Value, attributes: PropertyAttributes) {
        if index as usize >= self.array_size {
            self.array_size = index as usize + 1;
        }
        self.sparse_elements
            .insert(index, ValueAndAttributes::new(value, attributes));
    }

    pub fn remove(&mut self, index: u32) {
        assert!((index as usize) < self.array_size);
        self.sparse_elements.remove(&index);
    }

    pub fn take_first(&mut self) -> ValueAndAttributes {
        assert!(self.array_size > 0);
        self.array_size -= 1;

        let first_index = *self
            .sparse_elements
            .keys()
            .min()
            .expect("an array with a size has an element");
        self.sparse_elements
            .remove(&first_index)
            .expect("the first index is in the map")
    }

    pub fn take_last(&mut self) -> ValueAndAttributes {
        assert!(self.array_size > 0);
        self.array_size -= 1;

        let Ok(last_index) = u32::try_from(self.array_size) else {
            return ValueAndAttributes::default();
        };
        self.sparse_elements.remove(&last_index).unwrap_or_default()
    }

    pub fn size(&self) -> usize {
        self.sparse_elements.len()
    }

    pub fn array_like_size(&self) -> usize {
        self.array_size
    }

    pub fn set_array_like_size(&mut self, new_size: usize) -> bool {
        if new_size == self.array_size {
            return true;
        }

        if new_size >= self.array_size {
            self.array_size = new_size;
            return true;
        }

        let highest_non_configurable_index = self
            .sparse_elements
            .iter()
            .filter(|(index, element)| **index as usize >= new_size && !element.attributes.is_configurable())
            .map(|(index, _)| *index)
            .max();

        let failed = highest_non_configurable_index.is_some();
        let truncation_boundary = highest_non_configurable_index.map_or(new_size, |index| index as usize + 1);

        self.sparse_elements
            .retain(|index, element| !(*index as usize >= truncation_boundary && element.attributes.is_configurable()));

        self.array_size = truncation_boundary;
        !failed
    }

    pub fn sparse_elements(&self) -> &HashMap<u32, ValueAndAttributes, foldhash::fast::RandomState> {
        &self.sparse_elements
    }
}

// SAFETY: Visits the value of every element.
unsafe impl Trace for GenericIndexedPropertyStorage {
    fn trace(&self, visitor: &mut Visitor) {
        for element in self.sparse_elements.values() {
            element.trace(visitor);
        }
    }
}
