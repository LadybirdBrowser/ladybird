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

#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct FfiRootBackgroundSource {
    pub use_body_background_properties: bool,
    pub body_layout_node: crate::layout::node_data::NodeSlotId,
}

#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiScrollableOverflowHostCallbacks {
    pub context: *mut std::ffi::c_void,
    pub layout_node_is_in_focused_text_control:
        unsafe extern "C" fn(*mut std::ffi::c_void, *mut std::ffi::c_void) -> bool,
}

impl FfiScrollableOverflowHostCallbacks {
    pub(crate) fn layout_node_is_in_focused_text_control(&self, layout_node_shell: *mut std::ffi::c_void) -> bool {
        // SAFETY: The C++ host answers synchronously from a live layout node shell.
        unsafe { (self.layout_node_is_in_focused_text_control)(self.context, layout_node_shell) }
    }
}
