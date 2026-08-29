/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::layout::node_data::NodeSlotId;
use crate::layout::used_values;
use crate::layout::used_values::OptionalCssPixelRect;
use crate::painting::display_list::commands::{ContextRef, OptionalU32, SpatialNodeIndex};
use std::ffi::c_void;

#[derive(Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct FfiHitTestPaintableFacts {
    pub is_inert: bool,
    pub dom_node_has_parent: bool,
    pub is_editable_or_editing_host: bool,
    pub svg_mask_content_units_object_bbox: bool,
    pub svg_clip_path_units_object_bbox: bool,
    pub inside_blocking_wheel_event_handler: bool,
}

#[derive(Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct FfiHitTestTextNodeFacts {
    pub is_inert: bool,
}

#[derive(Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct FfiLineBreakCaretTarget {
    pub caret_offset: usize,
    pub rect: used_values::FfiCssPixelRect,
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
    pub has_chrome_metrics: bool,
    pub chrome_metrics: crate::painting::ffi::FfiChromeMetrics,
    pub viewport_wheel_overflow_x: u8,
    pub viewport_wheel_overflow_y: u8,
    pub local_point_for_visual_context: unsafe extern "C" fn(
        *mut c_void,
        ContextRef,
        used_values::FfiCssPixelPoint,
        bool,
        *mut libgfx_rust::FloatPoint,
    ) -> bool,
    pub line_in_scope: unsafe extern "C" fn(*mut c_void, usize) -> bool,
    pub sorting_context_group: unsafe extern "C" fn(*mut c_void, SpatialNodeIndex, *mut usize) -> bool,
    pub plane_depth_key:
        unsafe extern "C" fn(*mut c_void, SpatialNodeIndex, used_values::FfiCssPixelPoint, *mut i64) -> bool,
}

impl FfiHitTestQueryCallbacks {
    pub(crate) fn local_point_for_visual_context(
        &self,
        context: ContextRef,
        point: used_values::FfiCssPixelPoint,
        respect_clip: bool,
    ) -> Option<(f32, f32)> {
        let mut local = libgfx_rust::FloatPoint::default();
        // SAFETY: The C++ host writes the point synchronously when it returns true.
        let has_local = unsafe {
            (self.local_point_for_visual_context)(self.context, context, point, respect_clip, &raw mut local)
        };
        has_local.then_some((local.x, local.y))
    }
    pub(crate) fn line_in_scope(&self, line_index: usize) -> bool {
        // SAFETY: The C++ host answers synchronously.
        unsafe { (self.line_in_scope)(self.context, line_index) }
    }
    // The outermost 3D rendering context sorting the plane the visual context node renders into.
    pub(crate) fn sorting_context_group(&self, spatial: SpatialNodeIndex) -> Option<usize> {
        let mut group = 0usize;
        // SAFETY: The C++ host writes the group synchronously when it returns true.
        let has_group = unsafe { (self.sorting_context_group)(self.context, spatial, &raw mut group) };
        has_group.then_some(group)
    }
    // The depth of that plane at the queried point, quantized so coplanar planes compare equal.
    pub(crate) fn plane_depth_key(
        &self,
        spatial: SpatialNodeIndex,
        point: crate::css::css_pixels::CssPixelPoint,
    ) -> Option<i64> {
        let mut depth = 0i64;
        // SAFETY: The C++ host writes the depth synchronously when it returns true.
        let has_depth = unsafe { (self.plane_depth_key)(self.context, spatial, point.into(), &raw mut depth) };
        has_depth.then_some(depth)
    }
}

#[derive(Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct FfiTopmostItem {
    pub has_item: bool,
    pub index: usize,
    pub local: used_values::FfiCssPixelPoint,
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
    pub block_container_margin_rect: OptionalCssPixelRect,
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
    pub paintable: NodeSlotId,
    pub hit_node: NodeSlotId,
    pub chrome_widget_kind: u8,
    pub text_fragment_index: OptionalU32,
    pub caret_node_shell: *mut c_void,
    pub caret_offset: usize,
    pub rect: used_values::FfiCssPixelRect,
    pub caret_rect: used_values::FfiCssPixelRect,
    pub context: ContextRef,
}

#[derive(Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct FfiCaretLineExport {
    pub rect: used_values::FfiCssPixelRect,
    pub context: ContextRef,
    pub first_caret_item_index: usize,
    pub last_caret_item_index: usize,
}
