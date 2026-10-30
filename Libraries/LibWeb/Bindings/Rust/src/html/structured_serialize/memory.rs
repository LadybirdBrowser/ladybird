/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The memories of structured serializations and deserializations, which C++ only passes on, as opaque pointers, to
//! the serializations of the values that the serialization steps of a platform object hold.

use core::cell::RefCell;
use core::ffi::c_void;
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

    /// The memory that C++ passes on.
    ///
    /// # Safety
    ///
    /// `memory` must be a memory that Rust lent to C++ for the call that passes it back.
    pub(crate) unsafe fn from_abi<'a>(memory: *mut c_void) -> &'a Self {
        // SAFETY: The caller passes a memory that outlives the call.
        unsafe { &*memory.cast::<Self>() }
    }

    pub(crate) fn as_abi(&self) -> *mut c_void {
        core::ptr::from_ref(self).cast_mut().cast()
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

    /// The memory that C++ passes on.
    ///
    /// # Safety
    ///
    /// `memory` must be a memory that Rust lent to C++ for the call that passes it back.
    pub(crate) unsafe fn from_abi<'a>(memory: *mut c_void) -> &'a Self {
        // SAFETY: The caller passes a memory that outlives the call.
        unsafe { &*memory.cast::<Self>() }
    }

    pub(crate) fn as_abi(&self) -> *mut c_void {
        core::ptr::from_ref(self).cast_mut().cast()
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
