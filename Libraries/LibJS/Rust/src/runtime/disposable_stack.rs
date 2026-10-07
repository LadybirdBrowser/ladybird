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
pub enum DisposableState {
    Pending,
    Disposed,
}

#[repr(C)]
#[derive(Trace)]
pub struct DisposableStack {
    base: Object,
    disposable_state: Cell<DisposableState>,
    dispose_capability: GcRefCell<DisposeCapability>,
}

define_cell!(DisposableStack, Object, extends: [Object], finalize: finalize);

impl Deref for DisposableStack {
    type Target = Object;

    fn deref(&self) -> &Object {
        &self.base
    }
}

impl Finalize for DisposableStack {
    fn finalize(&self) {
        drop(self.dispose_capability.replace(new_dispose_capability()));
    }
}

impl DisposableStack {
    pub fn new(vm: &Vm, dispose_capability: DisposeCapability, prototype: Gc<Object>) -> DisposableStack {
        DisposableStack {
            base: Object::new_with_prototype(vm, Self::CLASS, prototype, MayInterfereWithIndexedPropertyAccess::No),
            disposable_state: Cell::new(DisposableState::Pending),
            dispose_capability: GcRefCell::new(dispose_capability),
        }
    }

    pub fn disposable_state(&self) -> DisposableState {
        self.disposable_state.get()
    }

    pub fn set_disposed(&self) {
        self.disposable_state.set(DisposableState::Disposed);
    }

    pub fn dispose_capability(&self) -> &GcRefCell<DisposeCapability> {
        &self.dispose_capability
    }
}
