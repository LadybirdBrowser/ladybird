/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::layout::node_data::NodeSlotId;
use crate::layout::used_values;
use crate::layout::used_values::OptionalCssPixelRect;
use crate::painting::display_list::commands::ContextRef;
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
    pub device_pixels_per_css_pixel: f64,
    pub scroll_offsets: *const libgfx_rust::FloatPoint,
    pub scroll_offsets_len: usize,
    pub has_chrome_metrics: bool,
    pub chrome_metrics: crate::painting::ffi::FfiChromeMetrics,
    pub viewport_wheel_overflow_x: u8,
    pub viewport_wheel_overflow_y: u8,
    pub shell_in_scope: unsafe extern "C" fn(*mut c_void, *mut c_void) -> bool,
}

impl FfiHitTestQueryCallbacks {
    pub(crate) fn shell_in_scope(&self, shell: *mut c_void) -> bool {
        // SAFETY: The C++ host answers synchronously.
        unsafe { (self.shell_in_scope)(self.context, shell) }
    }
    pub(crate) fn scroll_offsets(&self) -> &[libgfx_rust::FloatPoint] {
        if self.scroll_offsets.is_null() {
            return &[];
        }
        // SAFETY: QueryContext keeps the document and its resolved scroll snapshot alive for the synchronous query.
        unsafe { std::slice::from_raw_parts(self.scroll_offsets, self.scroll_offsets_len) }
    }
}

#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiCaretPositionQueryCallbacks {
    pub context: *mut c_void,
    pub shell_is_query_node: unsafe extern "C" fn(*mut c_void, *mut c_void) -> bool,
    pub query_boundary_descends_to_shell: unsafe extern "C" fn(*mut c_void, *mut c_void) -> bool,
    pub query_boundary_follows_shell_end: unsafe extern "C" fn(*mut c_void, *mut c_void, usize) -> bool,
    pub query_is_adjacent_to_shell: unsafe extern "C" fn(*mut c_void, *mut c_void) -> bool,
}

impl FfiCaretPositionQueryCallbacks {
    pub(crate) fn shell_is_query_node(&self, shell: *mut c_void) -> bool {
        // SAFETY: The C++ host compares the shell's DOM node synchronously.
        unsafe { (self.shell_is_query_node)(self.context, shell) }
    }

    pub(crate) fn query_boundary_descends_to_shell(&self, shell: *mut c_void) -> bool {
        // SAFETY: The C++ host walks the query boundary synchronously.
        unsafe { (self.query_boundary_descends_to_shell)(self.context, shell) }
    }

    pub(crate) fn query_boundary_follows_shell_end(&self, shell: *mut c_void, end_offset: usize) -> bool {
        // SAFETY: The C++ host compares the query boundary synchronously.
        unsafe { (self.query_boundary_follows_shell_end)(self.context, shell, end_offset) }
    }

    pub(crate) fn query_is_adjacent_to_shell(&self, shell: *mut c_void) -> bool {
        // SAFETY: The C++ host compares the query boundary synchronously.
        unsafe { (self.query_is_adjacent_to_shell)(self.context, shell) }
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

#[derive(Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct FfiCaretLineForPosition {
    pub has_line: bool,
    pub line_index: usize,
}

#[derive(Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct FfiResolvedHit {
    pub dispatch_shell: *mut c_void,
    pub allow_pseudo_fallback: bool,
    pub fallback_dispatch_shell: *mut c_void,
    pub has_index_in_node: bool,
    pub index_in_node: usize,
    pub is_text_fragment: bool,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(u8)]
pub enum FfiCaretBoundaryKind {
    #[default]
    Offset = 0,
    BeforeNode = 1,
    AfterNode = 2,
}

#[derive(Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct FfiResolvedCaret {
    pub has_position: bool,
    pub node_shell: *mut c_void,
    pub boundary: FfiCaretBoundaryKind,
    pub offset: usize,
    pub affinity_is_upstream: bool,
    pub has_debug_rect: bool,
    pub debug_rect: used_values::FfiCssPixelRect,
}

#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct FfiHitTestItemExport {
    pub can_produce_caret_position: bool,
    pub paintable: NodeSlotId,
    pub hit_node: NodeSlotId,
    pub chrome_widget_kind: u8,
    pub caret_node_shell: *mut c_void,
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
