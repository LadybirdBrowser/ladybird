/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

pub mod hit_test;
pub mod paint;
pub mod visual_context;

pub use hit_test::*;
pub use paint::*;
pub use visual_context::*;

/// The boxes the canvas background is painted from, and whether it takes over the body's.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct RootBackgroundSource {
    pub use_body_background_properties: bool,
    pub root_layout_node: crate::layout::node_data::NodeSlotId,
    pub body_layout_node: crate::layout::node_data::NodeSlotId,
}

impl Default for RootBackgroundSource {
    fn default() -> Self {
        Self {
            use_body_background_properties: false,
            root_layout_node: crate::layout::node_data::NodeSlotId::INVALID,
            body_layout_node: crate::layout::node_data::NodeSlotId::INVALID,
        }
    }
}

/// What geometry tells the document. The fields are private, so the callbacks are reached only
/// through the methods below, which take the main thread token.
#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiGeometryHostCallbacks {
    context: *mut std::ffi::c_void,
    set_scroll_offset: unsafe extern "C" fn(
        *mut std::ffi::c_void,
        &crate::render_state::BegunRead,
        crate::layout::node_data::NodeSlotId,
        crate::layout::used_values::FfiCssPixelPoint,
    ),
}

impl FfiGeometryHostCallbacks {
    /// Stores a scroll offset the overflow pass settled on, after the pass, in `read`.
    ///
    /// # Safety
    ///
    /// `slot` must name a live row. The host re-enters geometry queries and writes the store the
    /// offset lives in, so no mutable arena or cache borrow may be held across this call.
    pub(crate) unsafe fn set_scroll_offset(
        &self,
        _: &crate::stage::MainThread,
        read: &crate::render_state::BegunRead,
        slot: crate::layout::node_data::NodeSlotId,
        offset: crate::layout::used_values::FfiCssPixelPoint,
    ) {
        // SAFETY: Guaranteed by the caller.
        unsafe { (self.set_scroll_offset)(self.context, read, slot, offset) };
    }
}
