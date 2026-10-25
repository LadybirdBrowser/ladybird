/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::marker::PhantomData;
use core::ops::{Deref, DerefMut};

use super::heap::Heap;
use crate::interpreter::vm::Vm;

/// Keeps the heap from collecting garbage while it lives.
pub struct DeferGc<'heap>(&'heap Heap);

impl<'heap> DeferGc<'heap> {
    pub(crate) fn new(vm: &'heap Vm) -> Self {
        Self::of_heap(vm.heap())
    }

    /// Runs `body` while nothing collects garbage, so that what it holds by value needs no root.
    pub fn scope<R>(vm: &Vm, body: impl FnOnce(&DeferGc<'_>) -> R) -> R {
        body(&DeferGc::new(vm))
    }

    pub(super) fn of_heap(heap: &'heap Heap) -> Self {
        heap.defer_gc();
        Self(heap)
    }

    /// Ties `value`, whose cells only this deferral keeps alive, to the deferral.
    pub fn cover<T>(&self, value: T) -> Deferred<'_, T> {
        Deferred {
            value,
            _defer_gc: PhantomData,
        }
    }
}

impl Drop for DeferGc<'_> {
    fn drop(&mut self) {
        self.0.undefer_gc();
    }
}

/// A value whose cells nothing keeps alive but a deferral, such as cells in a Vec that nothing traces yet. It cannot
/// outlive the deferral.
pub struct Deferred<'d, T> {
    value: T,
    _defer_gc: PhantomData<&'d ()>,
}

impl<T> Deferred<'_, T> {
    /// Gives up the value, for moving it where the collector finds its cells.
    pub fn into_inner(self) -> T {
        self.value
    }
}

impl<T> Deref for Deferred<'_, T> {
    type Target = T;

    fn deref(&self) -> &T {
        &self.value
    }
}

impl<T> DerefMut for Deferred<'_, T> {
    fn deref_mut(&mut self) -> &mut T {
        &mut self.value
    }
}
