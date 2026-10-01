/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::Cell;
use core::ptr::NonNull;

use super::capi::{GCVisitor, gc_visitor_visit_cell, gc_visitor_visit_values};
use crate::layout::cell::Gc;
use crate::layout::value::Value;

/// A LibGC visitor, which marks what it visits during a collection and roots it while roots are gathered.
pub struct Visitor(NonNull<GCVisitor>);

impl Visitor {
    /// # Safety
    ///
    /// `visitor` must be a visitor LibGC passed in, and outlive the returned value.
    pub unsafe fn from_raw(visitor: *mut GCVisitor) -> Self {
        Self(NonNull::new(visitor).expect("LibGC passes a visitor"))
    }

    pub fn visit<T>(&mut self, cell: Gc<T>) {
        // SAFETY: The visitor is live, and a Gc points to a live cell.
        unsafe { gc_visitor_visit_cell(self.0.as_ptr(), cell.as_ptr().cast()) };
    }

    pub fn visit_values(&mut self, values: &[Value]) {
        // SAFETY: The visitor is live, and Value is a NaN-boxed u64.
        unsafe { gc_visitor_visit_values(self.0.as_ptr(), values.as_ptr().cast(), values.len()) };
    }
}

/// A value that can reach cells, which it reports to the visitor.
///
/// # Safety
///
/// `trace` must visit every cell the value can reach directly; a cell it misses can be collected while still in use.
pub unsafe trait Trace {
    fn trace(&self, visitor: &mut Visitor);
}

// SAFETY: A Gc reaches exactly its cell.
unsafe impl<T> Trace for Gc<T> {
    fn trace(&self, visitor: &mut Visitor) {
        visitor.visit(*self);
    }
}

// SAFETY: Forwards to the contained value.
unsafe impl<T: Trace> Trace for Option<T> {
    fn trace(&self, visitor: &mut Visitor) {
        if let Some(value) = self {
            value.trace(visitor);
        }
    }
}

// SAFETY: LibGC decodes the value and visits its cell, if it has one.
unsafe impl Trace for Value {
    fn trace(&self, visitor: &mut Visitor) {
        visitor.visit_values(core::slice::from_ref(self));
    }
}

// SAFETY: Forwards to the current value.
unsafe impl<T: Copy + Trace> Trace for Cell<T> {
    fn trace(&self, visitor: &mut Visitor) {
        self.get().trace(visitor);
    }
}
