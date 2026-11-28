/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The libgc_api items the lints know by path, at the same paths.

pub mod visitor {
    pub struct Visitor;

    /// # Safety
    ///
    /// Stand-in.
    pub unsafe trait Trace {
        fn trace(&self, visitor: &mut Visitor);
    }

    unsafe impl<T: Trace> Trace for Vec<T> {
        fn trace(&self, _: &mut Visitor) {}
    }
}

pub use libgc_api_macros::Trace;
pub use visitor::{Trace, Visitor};
