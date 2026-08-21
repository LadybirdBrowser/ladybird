/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::ffi::c_void;

use crate::painting::paintable_data::PaintableSlotId;

#[derive(Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct FfiHitTestPaintableFacts {
    pub opacity_is_zero: bool,
    pub visible_for_hit_testing: bool,
    pub dom_node_has_parent: bool,
    pub is_editable_or_editing_host: bool,
    pub has_resizer: bool,
    pub could_be_scrolled_horizontally: bool,
    pub could_be_scrolled_vertically: bool,
    pub svg_path_has_fill: bool,
    pub svg_path_winding_rule: i32,
    pub svg_mask_content_units_object_bbox: bool,
    pub svg_clip_path_units_object_bbox: bool,
}

#[derive(Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct FfiHitTestTextNodeFacts {
    pub parent_opacity_is_zero: bool,
    pub parent_pointer_events_none: bool,
    pub is_inert: bool,
}

#[derive(Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct FfiLineBreakCaretTarget {
    pub caret_offset: usize,
    pub rect: crate::layout::FfiCssPixelRect,
}

#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiHitTestHostCallbacks {
    pub context: *mut c_void,
    pub paintable_facts: unsafe extern "C" fn(*mut c_void, *mut c_void) -> FfiHitTestPaintableFacts,
    pub text_node_facts: unsafe extern "C" fn(*mut c_void, *mut c_void) -> FfiHitTestTextNodeFacts,
    pub line_break_caret_targets: unsafe extern "C" fn(*mut c_void, *mut c_void, *mut c_void),
}

impl FfiHitTestHostCallbacks {
    pub(crate) fn paintable_facts(&self, layout_node_shell: *mut c_void) -> FfiHitTestPaintableFacts {
        // SAFETY: The C++ host answers synchronously from a live layout node shell.
        unsafe { (self.paintable_facts)(self.context, layout_node_shell) }
    }
    pub(crate) fn text_node_facts(&self, node_shell: *mut c_void) -> FfiHitTestTextNodeFacts {
        // SAFETY: The C++ host answers synchronously from a live layout node shell.
        unsafe { (self.text_node_facts)(self.context, node_shell) }
    }
    pub(crate) fn line_break_caret_targets(&self, layout_node_shell: *mut c_void) -> Vec<FfiLineBreakCaretTarget> {
        let mut targets: Vec<FfiLineBreakCaretTarget> = Vec::new();
        // SAFETY: The C++ host pushes into the Vec through the exported sink function, synchronously.
        unsafe { (self.line_break_caret_targets)(self.context, layout_node_shell, (&raw mut targets).cast()) };
        targets
    }
}

#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiHitTestQueryCallbacks {
    pub context: *mut c_void,
    pub local_point_for_visual_context: unsafe extern "C" fn(*mut c_void, usize, i32, i32, bool, *mut f32) -> bool,
    pub chrome_widget_contains: unsafe extern "C" fn(*mut c_void, *mut c_void, u8, i32, i32) -> bool,
    pub line_in_scope: unsafe extern "C" fn(*mut c_void, usize) -> bool,
    pub sorting_context_group: unsafe extern "C" fn(*mut c_void, usize, *mut usize) -> bool,
    pub plane_depth_key: unsafe extern "C" fn(*mut c_void, usize, i32, i32, *mut i64) -> bool,
}

impl FfiHitTestQueryCallbacks {
    pub(crate) fn local_point_for_visual_context(
        &self,
        index: usize,
        x_raw: i32,
        y_raw: i32,
        respect_clip: bool,
    ) -> Option<(f32, f32)> {
        let mut out = [0.0f32; 2];
        // SAFETY: The C++ host writes two floats synchronously.
        let inside = unsafe {
            (self.local_point_for_visual_context)(self.context, index, x_raw, y_raw, respect_clip, out.as_mut_ptr())
        };
        inside.then_some((out[0], out[1]))
    }
    pub(crate) fn chrome_widget_contains(
        &self,
        layout_node_shell: *mut c_void,
        kind: u8,
        local_point: crate::css::css_pixels::CssPixelPoint,
    ) -> bool {
        // SAFETY: The C++ host answers synchronously from a live layout node shell.
        unsafe {
            (self.chrome_widget_contains)(
                self.context,
                layout_node_shell,
                kind,
                local_point.x.raw_value(),
                local_point.y.raw_value(),
            )
        }
    }
    pub(crate) fn line_in_scope(&self, line_index: usize) -> bool {
        // SAFETY: The C++ host answers synchronously.
        unsafe { (self.line_in_scope)(self.context, line_index) }
    }
    // The outermost 3D rendering context sorting the plane the visual context node renders into.
    pub(crate) fn sorting_context_group(&self, visual_context_index: usize) -> Option<usize> {
        let mut group = 0usize;
        // SAFETY: The C++ host writes the group index synchronously.
        let in_context = unsafe { (self.sorting_context_group)(self.context, visual_context_index, &raw mut group) };
        in_context.then_some(group)
    }
    // The depth of that plane at the queried point, quantized so coplanar planes compare equal.
    pub(crate) fn plane_depth_key(
        &self,
        visual_context_index: usize,
        point: crate::css::css_pixels::CssPixelPoint,
    ) -> Option<i64> {
        let mut depth = 0i64;
        // SAFETY: The C++ host writes the depth key synchronously.
        let has_depth = unsafe {
            (self.plane_depth_key)(
                self.context,
                visual_context_index,
                point.x.raw_value(),
                point.y.raw_value(),
                &raw mut depth,
            )
        };
        has_depth.then_some(depth)
    }
}

#[derive(Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct FfiTopmostItem {
    pub has_item: bool,
    pub index: usize,
    pub local_x: i32,
    pub local_y: i32,
}

#[derive(Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct FfiTopmostItemsForCaret {
    pub caret_item: FfiTopmostItem,
    pub hit_item: FfiTopmostItem,
}

#[derive(Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct FfiClosestLine {
    pub has_index: bool,
    pub index: usize,
    pub local_x: i32,
    pub local_y: i32,
    pub block_distance: i32,
    pub block_start_distance: i32,
    pub inline_distance: i32,
    pub has_block_container_margin_rect: bool,
    pub block_container_margin_rect: crate::layout::FfiCssPixelRect,
    pub is_before_point: bool,
    pub contains_point_in_block_axis: bool,
}

#[derive(Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct FfiCaretItemForLine {
    pub has_item: bool,
    pub item_index: usize,
    pub position_type: u8,
}

#[derive(Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct FfiAdjacentLine {
    pub has_line: bool,
    pub line_index: usize,
    pub point_x: i32,
    pub point_y: i32,
}

#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct FfiHitTestItemExport {
    pub kind: u8,
    pub paintable: PaintableSlotId,
    pub chrome_widget_kind: u8,
    pub has_text_fragment_index: bool,
    pub text_fragment_index: u32,
    pub caret_node_shell: *mut c_void,
    pub caret_offset: usize,
    pub rect: crate::layout::FfiCssPixelRect,
    pub caret_rect: crate::layout::FfiCssPixelRect,
    pub visual_context_index: usize,
}

#[derive(Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct FfiCaretLineExport {
    pub rect: crate::layout::FfiCssPixelRect,
    pub visual_context_index: usize,
    pub first_caret_item_index: usize,
    pub last_caret_item_index: usize,
}
