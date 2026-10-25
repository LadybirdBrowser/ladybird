/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Precise roots for values that live outside cells and outside the stack, where the collector would not find them.

use core::cell::{OnceCell, Ref, RefCell, RefMut};
use core::ops::{Deref, DerefMut};

use super::gc_ref_cell::GcRefCell;
use super::visitor::{Trace, Visitor};
use crate::interpreter::vm::Vm;

/// The roots a VM keeps alive. Entries are boxed values registered by address, so slots never move.
#[derive(Default)]
pub struct RootSet {
    entries: RefCell<Vec<Option<*const dyn Trace>>>,
    free_slots: RefCell<Vec<usize>>,
}

impl RootSet {
    fn register(&self, value: *const dyn Trace) -> usize {
        let mut entries = self.entries.borrow_mut();
        if let Some(slot) = self.free_slots.borrow_mut().pop() {
            entries[slot] = Some(value);
            return slot;
        }
        entries.push(Some(value));
        entries.len() - 1
    }

    fn unregister(&self, slot: usize) {
        self.entries.borrow_mut()[slot] = None;
        self.free_slots.borrow_mut().push(slot);
    }

    pub fn trace(&self, visitor: &mut Visitor) {
        for value in self.entries.borrow().iter().flatten() {
            // SAFETY: A registered value stays alive until its root unregisters it.
            unsafe { &**value }.trace(visitor);
        }
    }
}

/// Keeps a value and every cell it reaches alive for as long as it exists.
pub struct Root<'vm, T: Trace + 'static> {
    vm: &'vm Vm,
    value: Box<T>,
    slot: usize,
}

impl<'vm, T: Trace + 'static> Root<'vm, T> {
    pub fn new(vm: &'vm Vm, value: T) -> Self {
        let value = Box::new(value);
        let slot = vm
            .roots()
            .register(core::ptr::from_ref::<T>(&value) as *const dyn Trace);
        Self { vm, value, slot }
    }

    pub fn get(&self) -> &T {
        &self.value
    }
}

impl<T: Trace + Copy + 'static> Root<'_, T> {
    pub fn value(&self) -> T {
        *self.value
    }
}

impl<T: Trace + 'static> Drop for Root<'_, T> {
    fn drop(&mut self) {
        self.vm.roots().unregister(self.slot);
    }
}

/// A growable list of values that are kept alive, for the temporaries a Vec would hide from the collector. Elements
/// are copied in and out, or lent by values() and values_mut(), while which the list cannot change. The list registers
/// its root with the first element, so an empty one costs nothing.
pub struct MarkedVec<'vm, T: Trace + Clone + 'static> {
    vm: &'vm Vm,
    values: OnceCell<Root<'vm, GcRefCell<Vec<T>>>>,
}

impl<'vm, T: Trace + Clone + 'static> MarkedVec<'vm, T> {
    pub fn new(vm: &'vm Vm) -> Self {
        Self {
            vm,
            values: OnceCell::new(),
        }
    }

    pub fn with_capacity(vm: &'vm Vm, capacity: usize) -> Self {
        let list = Self::new(vm);
        if capacity > 0 {
            list.rooted_values().borrow_mut().reserve(capacity);
        }
        list
    }

    fn rooted_values(&self) -> &GcRefCell<Vec<T>> {
        self.values
            .get_or_init(|| Root::new(self.vm, GcRefCell::new(Vec::new())))
            .get()
    }

    pub fn push(&self, value: T) {
        self.rooted_values().borrow_mut().push(value);
    }

    pub fn extend_from_slice(&self, values: &[T]) {
        self.rooted_values().borrow_mut().extend_from_slice(values);
    }

    pub fn insert(&self, index: usize, value: T) {
        self.rooted_values().borrow_mut().insert(index, value);
    }

    pub fn pop(&self) -> Option<T> {
        self.values.get()?.get().borrow_mut().pop()
    }

    pub fn get(&self, index: usize) -> Option<T> {
        self.values.get()?.get().borrow().get(index).cloned()
    }

    pub fn set(&self, index: usize, value: T) {
        self.rooted_values().borrow_mut()[index] = value;
    }

    pub fn len(&self) -> usize {
        self.values.get().map_or(0, |values| values.get().borrow().len())
    }

    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }

    /// Lends the values until the result drops, which may outlive collections. Changing the list meanwhile panics.
    pub fn values(&self) -> MarkedValues<'_, T> {
        MarkedValues(self.values.get().map(|values| values.get().borrow()))
    }

    /// Lends the values to change in place until the result drops. Nothing may collect meanwhile, which stops the
    /// process, and using the list meanwhile panics.
    pub fn values_mut(&self) -> MarkedValuesMut<'_, T> {
        MarkedValuesMut(self.values.get().map(|values| values.get().borrow_mut()))
    }

    /// Copies the values out, for handing them to code that does not collect.
    pub fn to_vec(&self) -> Vec<T> {
        self.values
            .get()
            .map_or_else(Vec::new, |values| values.get().borrow().clone())
    }
}

/// The values of a MarkedVec, which stay alive and unchanged while this lends them.
pub struct MarkedValues<'a, T>(Option<Ref<'a, Vec<T>>>);

impl<T> Deref for MarkedValues<'_, T> {
    type Target = [T];

    fn deref(&self) -> &[T] {
        self.0.as_deref().map_or(&[], Vec::as_slice)
    }
}

/// The values of a MarkedVec, which stay alive while this lends them to change in place.
pub struct MarkedValuesMut<'a, T>(Option<RefMut<'a, Vec<T>>>);

impl<T> Deref for MarkedValuesMut<'_, T> {
    type Target = [T];

    fn deref(&self) -> &[T] {
        self.0.as_deref().map_or(&[], Vec::as_slice)
    }
}

impl<T> DerefMut for MarkedValuesMut<'_, T> {
    fn deref_mut(&mut self) -> &mut [T] {
        self.0.as_deref_mut().map_or(&mut [], Vec::as_mut_slice)
    }
}
