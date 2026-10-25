/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::capi;
use super::heap::Heap;
use crate::interpreter::vm::Vm;

/// Keeps the heap from collecting garbage while it lives.
pub struct DeferGc<'heap>(&'heap Heap);

impl<'heap> DeferGc<'heap> {
    pub(crate) fn new(vm: &'heap Vm) -> Self {
        Self::of_heap(vm.heap())
    }

    pub(super) fn of_heap(heap: &'heap Heap) -> Self {
        capi::gc_heap_defer_gc(heap.handle());
        Self(heap)
    }
}

impl Drop for DeferGc<'_> {
    fn drop(&mut self) {
        capi::gc_heap_undefer_gc(self.0.handle());
    }
}
