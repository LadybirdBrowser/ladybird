/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::{Ref, RefCell, RefMut};

use super::visitor::{Trace, Visitor};

/// Interior mutability for the compound state of a cell. A collection can happen at any allocation and needs to read
/// the state, so nothing may allocate while holding a mutable borrow; a collection that finds one stops the process.
pub struct GcRefCell<T>(RefCell<T>);

impl<T> GcRefCell<T> {
    pub const fn new(value: T) -> Self {
        Self(RefCell::new(value))
    }

    pub fn borrow(&self) -> Ref<'_, T> {
        self.0.borrow()
    }

    pub fn borrow_mut(&self) -> RefMut<'_, T> {
        self.0.borrow_mut()
    }

    pub fn replace(&self, value: T) -> T {
        self.0.replace(value)
    }
}

impl<T: Default> Default for GcRefCell<T> {
    fn default() -> Self {
        Self::new(T::default())
    }
}

// SAFETY: Traces the current value.
unsafe impl<T: Trace> Trace for GcRefCell<T> {
    fn trace(&self, visitor: &mut Visitor) {
        let Ok(value) = self.0.try_borrow() else {
            eprintln!("libjs_rust: garbage collection while a GcRefCell is mutably borrowed");
            std::process::abort();
        };
        value.trace(visitor);
    }
}
