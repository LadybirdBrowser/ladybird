/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

pub mod hit_test;
pub mod paint;
pub use libcompositing_rust::host::replay;
pub mod visual_context;

pub use hit_test::*;
pub use paint::*;
pub use replay::*;
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

/// What geometry asks the document. The fields are private, so the callbacks are reached only
/// through the methods below, which take the main thread token.
#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiGeometryHostCallbacks {
    context: *mut std::ffi::c_void,
    clamp_scroll_offset_if_nonzero: unsafe extern "C" fn(*mut std::ffi::c_void, *mut std::ffi::c_void),
}

impl FfiGeometryHostCallbacks {
    /// # Safety
    ///
    /// `layout_node_shell` must be live. The host re-enters geometry queries to clamp the offset,
    /// so no mutable arena or cache borrow may be held across this call.
    pub(crate) unsafe fn clamp_scroll_offset_if_nonzero(
        &self,
        _: &crate::stage::MainThread,
        layout_node_shell: *mut std::ffi::c_void,
    ) {
        // SAFETY: Guaranteed by the caller.
        unsafe { (self.clamp_scroll_offset_if_nonzero)(self.context, layout_node_shell) };
    }
}
