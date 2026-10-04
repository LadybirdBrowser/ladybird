/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Host objects of kind JS_HOST_CLASS_OBJECT.

use crate::gc::visitor::{Trace, Visitor};
pub use crate::layout::host_object::HostObject;

// SAFETY: Visits the object and the embedder's two cells, which are all the cells a host object reaches.
unsafe impl Trace for HostObject {
    fn trace(&self, visitor: &mut Visitor) {
        self.base.trace(visitor);
        self.wrappable.trace(visitor);
        self.host_data.trace(visitor);
    }
}
