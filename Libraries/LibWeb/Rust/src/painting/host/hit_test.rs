/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::layout::node_data::NodeSlotId;
use crate::layout::used_values;
use crate::painting::display_list::commands::ContextRef;
use std::ffi::c_void;

#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiHitTestQueryCallbacks {
    pub context: *mut c_void,
    pub device_pixels_per_css_pixel: f64,
    pub scroll_offsets: *const libgfx_rust::FloatPoint,
    pub scroll_offsets_len: usize,
    pub has_chrome_metrics: bool,
    pub chrome_metrics: crate::painting::ffi::FfiChromeMetrics,
    /// Private: reached only through the method below, which takes the main thread token.
    node_in_scope: unsafe extern "C" fn(*mut c_void, FfiNodeIdentity) -> bool,
}

impl FfiHitTestQueryCallbacks {
    pub(crate) fn node_in_scope(&self, _: &crate::stage::MainThread, node: FfiNodeIdentity) -> bool {
        // SAFETY: The C++ host answers synchronously.
        unsafe { (self.node_in_scope)(self.context, node) }
    }
    pub(crate) fn scroll_offsets(&self) -> &[libgfx_rust::FloatPoint] {
        if self.scroll_offsets.is_null() {
            return &[];
        }
        // SAFETY: QueryContext keeps the document and its resolved scroll snapshot alive for the synchronous query.
        unsafe { std::slice::from_raw_parts(self.scroll_offsets, self.scroll_offsets_len) }
    }
}

/// A caret boundary, as the node identities a hit test item can be compared against.
///
/// The five questions a caret position query asks of a candidate item all reduce to "which node
/// is it", so the host resolves the boundary's neighbourhood once before the query runs and the
/// query compares StyleNodeIDs. A node without one is named by 0, which matches nothing: an item
/// that stands for no DOM node is named by 0 as well.
#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiCaretPositionQuery {
    /// The node the boundary is inside.
    pub node: u32,
    /// The child at the boundary's offset, or 0 when the node has no child there.
    pub child_at_offset: u32,
    /// The child before the boundary's offset, or 0 at offset 0.
    pub child_before_offset: u32,
    /// The length of the node `child_before_offset` names.
    pub child_before_offset_length: usize,
    /// `child_at_offset` and the chain of first children descending from it, in order.
    pub boundary_descent: *const u32,
    pub boundary_descent_len: usize,
}

impl FfiCaretPositionQuery {
    fn names(named: u32, node: u32) -> bool {
        named != 0 && named == node
    }

    fn boundary_descent(&self) -> &[u32] {
        if self.boundary_descent.is_null() {
            return &[];
        }
        // SAFETY: The host keeps the descent alive for the synchronous query.
        unsafe { std::slice::from_raw_parts(self.boundary_descent, self.boundary_descent_len) }
    }

    pub(crate) fn is_query_node(&self, node: u32) -> bool {
        Self::names(self.node, node)
    }

    /// Whether the boundary sits immediately before `node` among its parent's children.
    pub(crate) fn boundary_precedes(&self, node: u32) -> bool {
        Self::names(self.child_at_offset, node)
    }

    /// Whether the boundary sits at the end of `node`, which ends at `end_offset`.
    pub(crate) fn boundary_follows_end(&self, node: u32, end_offset: usize) -> bool {
        Self::names(self.child_before_offset, node) && end_offset == self.child_before_offset_length
    }

    /// Whether the boundary sits on either side of `node`.
    pub(crate) fn is_adjacent_to(&self, node: u32) -> bool {
        Self::names(self.child_at_offset, node) || Self::names(self.child_before_offset, node)
    }

    /// Whether descending through first children from the boundary reaches `node`.
    pub(crate) fn boundary_descends_to(&self, node: u32) -> bool {
        node != 0 && self.boundary_descent().contains(&node)
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

pub use crate::layout::node_data::FfiNodeIdentity;

#[derive(Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct FfiResolvedHit {
    /// The node the hit dispatches its events to, and the node it falls back to when there is
    /// none.
    pub dispatch: FfiNodeIdentity,
    pub fallback_dispatch: FfiNodeIdentity,
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
    IndexOfNodeInParent = 3,
}

#[derive(Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct FfiResolvedCaret {
    pub has_position: bool,
    pub node: FfiNodeIdentity,
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
