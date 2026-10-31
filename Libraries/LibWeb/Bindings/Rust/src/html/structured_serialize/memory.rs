/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The memories of structured serializations and deserializations.

use core::cell::RefCell;
use std::collections::HashMap;

use libjs_rust::api::{MarkedVec, Value, Vm};

pub(crate) struct SerializationMemory<'vm> {
    values: MarkedVec<'vm, Value>,
    positions: RefCell<HashMap<u64, u32>>,
}

impl<'vm> SerializationMemory<'vm> {
    pub(crate) fn new(vm: &'vm Vm) -> Self {
        Self {
            values: MarkedVec::new(vm),
            positions: RefCell::new(HashMap::new()),
        }
    }

    pub(crate) fn get(&self, value: Value) -> Option<u32> {
        self.positions.borrow().get(&value.to_raw()).copied()
    }

    pub(crate) fn contains(&self, value: Value) -> bool {
        self.get(value).is_some()
    }

    pub(crate) fn set(&self, value: Value, index: u32) {
        if self.positions.borrow_mut().insert(value.to_raw(), index).is_none() {
            self.values.push(value);
        }
    }

    pub(crate) fn size(&self) -> u32 {
        u32::try_from(self.positions.borrow().len()).expect("a serialization has fewer than 2^32 values")
    }
}

pub(crate) struct DeserializationMemory<'vm> {
    values: MarkedVec<'vm, Value>,
}

impl<'vm> DeserializationMemory<'vm> {
    pub(crate) fn new(vm: &'vm Vm) -> Self {
        Self {
            values: MarkedVec::new(vm),
        }
    }

    pub(crate) fn size(&self) -> usize {
        self.values.len()
    }

    pub(crate) fn append(&self, value: Value) {
        self.values.push(value);
    }

    pub(crate) fn get(&self, index: usize) -> Value {
        self.values.get(index).expect("the memory has a value at the index")
    }

    pub(crate) fn set(&self, index: usize, value: Value) {
        self.values.set(index, value);
    }
}
