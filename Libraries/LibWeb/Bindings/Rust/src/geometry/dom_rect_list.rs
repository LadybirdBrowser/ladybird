/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! https://drafts.fxtf.org/geometry/#domrectlist

use libjs_rust::api::Trace;

use super::dom_rect::DOMRect;
use crate::bindings::interface::{Ref, Wrappable};
use crate::bindings::{CallContext, WebIdlResult};

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct DOMRectList {
    rects: Vec<Ref<DOMRect>>,
}

impl Wrappable for DOMRectList {}

impl DOMRectList {
    pub(crate) fn new(rects: &[Ref<DOMRect>]) -> Self {
        Self { rects: rects.to_vec() }
    }

    /// https://drafts.fxtf.org/geometry/#dom-domrectlist-length
    pub(crate) fn length(&self, _: &mut CallContext) -> WebIdlResult<u32> {
        // The length attribute must return the total number of DOMRect objects associated with the object.
        Ok(u32::try_from(self.rects.len()).expect("a list has fewer than 2^32 rectangles"))
    }

    /// https://drafts.fxtf.org/geometry/#dom-domrectlist-item
    pub(crate) fn item(&self, _: &mut CallContext, index: u32) -> WebIdlResult<Option<Ref<DOMRect>>> {
        // The item(index) method, when invoked, must return null when index is greater than or equal to the number of
        // DOMRect objects associated with the DOMRectList. Otherwise, the DOMRect object at index must be returned.
        // Indices are zero-based.
        Ok(self.rects.get(index as usize).copied())
    }
}
