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

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(C)]
pub struct FfiRootBackgroundSource {
    pub use_body_background_properties: bool,
    pub root_layout_node: crate::layout::node_data::NodeSlotId,
    pub body_layout_node: crate::layout::node_data::NodeSlotId,
}

impl Default for FfiRootBackgroundSource {
    fn default() -> Self {
        Self {
            use_body_background_properties: false,
            root_layout_node: crate::layout::node_data::NodeSlotId::INVALID,
            body_layout_node: crate::layout::node_data::NodeSlotId::INVALID,
        }
    }
}

#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiGeometryHostCallbacks {
    pub context: *mut std::ffi::c_void,
    pub clamp_scroll_offset_if_nonzero: unsafe extern "C" fn(*mut std::ffi::c_void, *mut std::ffi::c_void),
    pub layout_node_is_in_focused_text_control:
        unsafe extern "C" fn(*mut std::ffi::c_void, *mut std::ffi::c_void) -> bool,
}

impl FfiGeometryHostCallbacks {
    pub(crate) fn layout_node_is_in_focused_text_control(&self, layout_node_shell: *mut std::ffi::c_void) -> bool {
        // SAFETY: The C++ host answers synchronously from a live layout node shell.
        unsafe { (self.layout_node_is_in_focused_text_control)(self.context, layout_node_shell) }
    }
}
