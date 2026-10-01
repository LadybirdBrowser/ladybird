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

    /// Traces a slice of values, which some types can report in a single call.
    fn trace_slice(values: &[Self], visitor: &mut Visitor)
    where
        Self: Sized,
    {
        for value in values {
            value.trace(visitor);
        }
    }
}

/// Implements Trace for types that cannot reach any cell.
macro_rules! impl_trace_for_cell_free_types {
    ($($type:ty),* $(,)?) => {
        $(
            // SAFETY: The type holds no cells.
            unsafe impl Trace for $type {
                fn trace(&self, _: &mut Visitor) {}

                fn trace_slice(_: &[Self], _: &mut Visitor) {}
            }
        )*
    };
}

impl_trace_for_cell_free_types!(
    (),
    bool,
    char,
    u8,
    u16,
    u32,
    u64,
    usize,
    i8,
    i16,
    i32,
    i64,
    isize,
    f32,
    f64,
    &'static str,
    String,
    ak::Utf16String,
    ak::Utf16FlyString,
);

// SAFETY: Holds nothing.
unsafe impl<T: ?Sized> Trace for core::marker::PhantomData<T> {
    fn trace(&self, _: &mut Visitor) {}
}

// SAFETY: Forwards to the boxed value.
unsafe impl<T: Trace + ?Sized> Trace for Box<T> {
    fn trace(&self, visitor: &mut Visitor) {
        (**self).trace(visitor);
    }
}

// SAFETY: Forwards to every element.
unsafe impl<T: Trace> Trace for [T] {
    fn trace(&self, visitor: &mut Visitor) {
        T::trace_slice(self, visitor);
    }
}

// SAFETY: Forwards to every element.
unsafe impl<T: Trace, const N: usize> Trace for [T; N] {
    fn trace(&self, visitor: &mut Visitor) {
        T::trace_slice(self, visitor);
    }
}

// SAFETY: Forwards to every element.
unsafe impl<T: Trace> Trace for Vec<T> {
    fn trace(&self, visitor: &mut Visitor) {
        T::trace_slice(self, visitor);
    }
}

// SAFETY: Forwards to the value once it exists.
unsafe impl<T: Trace> Trace for core::cell::OnceCell<T> {
    fn trace(&self, visitor: &mut Visitor) {
        if let Some(value) = self.get() {
            value.trace(visitor);
        }
    }
}

macro_rules! impl_trace_for_tuples {
    ($(($($name:ident),+)),* $(,)?) => {
        $(
            // SAFETY: Forwards to every element.
            #[allow(non_snake_case)]
            unsafe impl<$($name: Trace),+> Trace for ($($name,)+) {
                fn trace(&self, visitor: &mut Visitor) {
                    let ($($name,)+) = self;
                    $($name.trace(visitor);)+
                }
            }
        )*
    };
}

impl_trace_for_tuples!((A, B), (A, B, C), (A, B, C, D));

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

    fn trace_slice(values: &[Self], visitor: &mut Visitor) {
        visitor.visit_values(values);
    }
}

// SAFETY: Forwards to the current value.
unsafe impl<T: Copy + Trace> Trace for Cell<T> {
    fn trace(&self, visitor: &mut Visitor) {
        self.get().trace(visitor);
    }
}
