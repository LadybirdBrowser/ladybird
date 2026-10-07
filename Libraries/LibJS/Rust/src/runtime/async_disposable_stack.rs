/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::Cell;
use core::ops::Deref;

use libjs_runtime_macros::Trace;

use crate::gc::class::{Finalize, GcCell, define_cell};
use crate::gc::gc_ref_cell::GcRefCell;
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::object::Object;
use crate::runtime::abstract_operations::{DisposeCapability, new_dispose_capability};
use crate::runtime::object::MayInterfereWithIndexedPropertyAccess;

#[derive(Clone, Copy, Debug, PartialEq, Eq, Trace)]
pub enum AsyncDisposableState {
    Pending,
    Disposed,
}

#[repr(C)]
#[derive(Trace)]
pub struct AsyncDisposableStack {
    base: Object,
    async_disposable_state: Cell<AsyncDisposableState>,
    dispose_capability: GcRefCell<DisposeCapability>,
}

define_cell!(AsyncDisposableStack, Object, extends: [Object], finalize: finalize);

impl Deref for AsyncDisposableStack {
    type Target = Object;

    fn deref(&self) -> &Object {
        &self.base
    }
}

impl Finalize for AsyncDisposableStack {
    fn finalize(&self) {
        drop(self.dispose_capability.replace(new_dispose_capability()));
    }
}

impl AsyncDisposableStack {
    pub fn new(vm: &Vm, dispose_capability: DisposeCapability, prototype: Gc<Object>) -> AsyncDisposableStack {
        AsyncDisposableStack {
            base: Object::new_with_prototype(vm, Self::CLASS, prototype, MayInterfereWithIndexedPropertyAccess::No),
            async_disposable_state: Cell::new(AsyncDisposableState::Pending),
            dispose_capability: GcRefCell::new(dispose_capability),
        }
    }

    pub fn async_disposable_state(&self) -> AsyncDisposableState {
        self.async_disposable_state.get()
    }

    pub fn set_disposed(&self) {
        self.async_disposable_state.set(AsyncDisposableState::Disposed);
    }

    pub fn dispose_capability(&self) -> &GcRefCell<DisposeCapability> {
        &self.dispose_capability
    }
}
