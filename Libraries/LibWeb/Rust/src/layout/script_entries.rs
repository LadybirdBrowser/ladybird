/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The entries script APIs call for an element's geometry: its boxes and client rects, the rects of a text range and of
//! a caret, and what an intersection observer computes. Each spends the one forced read of the script call that
//! reached it, and answers from the rows the document's render state published, which the host asks for again only
//! where it wrote them since.

use super::FfiCssPixelRect;
use super::node_data::NodeSlotId;
use super::used_values::FfiCssPixelSize;
use crate::css::css_pixels::CssPixelRect;
use crate::painting::ffi::{
    FfiBoxModelMetrics, FfiCaretRectResult, FfiEmptyLineCaretRect, FfiOptionalCssPixelRect, FfiRectToViewportTransform,
};
use crate::painting::paint_read::{GeometryRead, PaintRead, PaintSource};
use crate::render_state::{DocumentHost, ScriptForcedRead};
use std::ffi::c_void;

/// Mints the forced read of a script call that reaches the host through one of this module's entries.
pub(crate) struct ScriptEntry {
    _private: (),
}

const SCRIPT_ENTRY: ScriptEntry = ScriptEntry { _private: () };

/// Answers `read` from the rows of `host`'s document as of every write the host made, spending the script call's
/// forced read. Every entry here is called with a live document host, on its document's thread.
fn read_rows<R>(host: *mut DocumentHost, read: impl FnOnce(&PaintSource<'_>) -> R) -> R {
    assert!(!host.is_null(), "document host is null");
    // SAFETY: The host is live.
    unsafe { &*host }.read_rows(ScriptForcedRead::at_script_entry(&SCRIPT_ENTRY), false, read)
}

/// # Safety
///
/// `host` must be a live document host, on its document's thread, and
/// `rect_to_viewport_transform` must satisfy `rect_to_viewport_transform_from_ffi`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_script_client_rects(
    host: *mut DocumentHost,
    layout_node: NodeSlotId,
    rect_to_viewport_transform: FfiRectToViewportTransform,
    context: *mut c_void,
    push_rect: unsafe extern "C" fn(*mut c_void, FfiCssPixelRect),
) {
    read_rows(host, |arena| {
        let rect_to_viewport_transform =
            unsafe { crate::painting::ffi::rect_to_viewport_transform_from_ffi(&rect_to_viewport_transform) };
        crate::painting::client_rects::for_each_client_rect(
            arena,
            layout_node,
            rect_to_viewport_transform.as_ref(),
            |rect| {
                // SAFETY: The consumer copies the plain-data rect synchronously.
                unsafe { push_rect(context, rect.into()) };
            },
        );
    });
}

/// # Safety
///
/// `host` must be a live document host, on its document's thread, and
/// `rect_to_viewport_transform` must satisfy `rect_to_viewport_transform_from_ffi`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_script_bounding_client_rect(
    host: *mut DocumentHost,
    layout_node: NodeSlotId,
    rect_to_viewport_transform: FfiRectToViewportTransform,
) -> FfiCssPixelRect {
    read_rows(host, |arena| {
        let rect_to_viewport_transform =
            unsafe { crate::painting::ffi::rect_to_viewport_transform_from_ffi(&rect_to_viewport_transform) };
        crate::painting::client_rects::bounding_client_rect(arena, layout_node, rect_to_viewport_transform.as_ref())
            .into()
    })
}

/// # Safety
///
/// `host` must be a live document host, on its document's thread, and
/// `rect_to_viewport_transform` must satisfy `rect_to_viewport_transform_from_ffi`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_script_transform_subtree_is_clipped_outside(
    host: *mut DocumentHost,
    target: NodeSlotId,
    root_bounds: FfiCssPixelRect,
    rect_to_viewport_transform: FfiRectToViewportTransform,
) -> bool {
    read_rows(host, |arena| {
        let rect_to_viewport_transform =
            unsafe { crate::painting::ffi::rect_to_viewport_transform_from_ffi(&rect_to_viewport_transform) };
        crate::painting::intersection_observer::transform_subtree_is_clipped_outside(
            arena,
            target,
            root_bounds.into(),
            rect_to_viewport_transform.as_ref(),
        )
    })
}

/// # Safety
///
/// `host` must be a live document host, on its document's thread, and
/// `rect_to_viewport_transform` must satisfy `rect_to_viewport_transform_from_ffi`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_script_intersection_observer_intersection_rect(
    host: *mut DocumentHost,
    target: NodeSlotId,
    target_rect: FfiCssPixelRect,
    intersection_root: NodeSlotId,
    root_bounds: FfiCssPixelRect,
    rect_to_viewport_transform: FfiRectToViewportTransform,
    context: *mut c_void,
    inflate_scroll_container_clip_rect_by_scroll_margin: unsafe extern "C" fn(
        *mut c_void,
        FfiCssPixelRect,
    ) -> FfiCssPixelRect,
) -> FfiCssPixelRect {
    read_rows(host, |arena| {
        let rect_to_viewport_transform =
            unsafe { crate::painting::ffi::rect_to_viewport_transform_from_ffi(&rect_to_viewport_transform) };
        crate::painting::intersection_observer::intersection_rect(
            arena,
            target,
            target_rect.into(),
            intersection_root,
            root_bounds.into(),
            rect_to_viewport_transform.as_ref(),
            |clip_rect: CssPixelRect| -> CssPixelRect {
                // SAFETY: The callback copies the plain-data rect synchronously.
                unsafe { inflate_scroll_container_clip_rect_by_scroll_margin(context, clip_rect.into()) }.into()
            },
        )
        .into()
    })
}

/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_script_can_compute_client_rects_without_visual_context_update(
    host: *mut DocumentHost,
    layout_node: NodeSlotId,
    viewport_scroll_offset_is_zero: bool,
) -> bool {
    read_rows(host, |arena| {
        crate::painting::client_rects::can_compute_client_rects_without_visual_context_update(
            arena,
            layout_node,
            viewport_scroll_offset_is_zero,
        )
    })
}

/// # Safety
///
/// `host` must be a live document host, on its document's thread, used on the document
/// thread, and
/// `rect_to_viewport_transform` must satisfy `rect_to_viewport_transform_from_ffi`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_script_text_range_rects(
    host: *mut DocumentHost,
    primary: NodeSlotId,
    selection_state: u8,
    range_start_offset: usize,
    range_end_offset: usize,
    filter_dom_start: usize,
    filter_dom_end: usize,
    rect_to_viewport_transform: FfiRectToViewportTransform,
    context: *mut c_void,
    push_rect: unsafe extern "C" fn(*mut c_void, FfiCssPixelRect),
) {
    read_rows(host, |arena| {
        let paintable_rows = arena;
        let rect_to_viewport_transform =
            unsafe { crate::painting::ffi::rect_to_viewport_transform_from_ffi(&rect_to_viewport_transform) };

        let fragments = arena.text_fragments(primary);
        let node_slots = fragments.as_slice();
        crate::painting::text_fragment::for_each_fragment_of_nodes(paintable_rows, node_slots, |block, _, fragment| {
            let fragment_dom_start = fragment.dom_start_offset_in_node;
            let fragment_dom_end = fragment.dom_end_offset_in_node;
            if fragment_dom_end <= filter_dom_start || fragment_dom_start >= filter_dom_end {
                return true;
            }

            let rect = crate::painting::text_fragment::range_rect(
                paintable_rows,
                fragment,
                selection_state,
                range_start_offset,
                range_end_offset,
            );

            let rect_in_viewport_space = if arena.slot_is_live(block) {
                crate::painting::rect_to_viewport_transform::transform_rect_to_viewport_or_identity(
                    rect_to_viewport_transform.as_ref(),
                    paintable_rows,
                    block,
                    rect,
                )
            } else {
                rect
            };

            // SAFETY: The consumer copies the plain-data rect synchronously.
            unsafe { push_rect(context, rect_in_viewport_space.into()) };
            true
        });
    });
}

/// # Safety
///
/// `host` must be a live document host, on its document's thread, used on the document
/// thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_script_text_caret_rect_in_dom_range(
    host: *mut DocumentHost,
    primary: NodeSlotId,
    offset: usize,
) -> FfiOptionalCssPixelRect {
    read_rows(host, |arena| {
        let paintable_rows = arena;
        let fragments = arena.text_fragments(primary);
        let node_slots = fragments.as_slice();
        match crate::painting::caret::caret_rect_in_dom_range(paintable_rows, node_slots, offset) {
            Some(rect) => FfiOptionalCssPixelRect {
                has_value: true,
                rect: rect.into(),
            },
            None => FfiOptionalCssPixelRect {
                has_value: false,
                rect: FfiCssPixelRect::default(),
            },
        }
    })
}

/// # Safety
///
/// `host` must be a live document host, on its document's thread, used on the document
/// thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_script_text_caret_rect_for_position(
    host: *mut DocumentHost,
    primary: NodeSlotId,
    offset: usize,
    affinity_is_downstream: bool,
) -> FfiCaretRectResult {
    let mut result = FfiCaretRectResult {
        found: false,
        rect: FfiCssPixelRect::default(),
        style_source: NodeSlotId::INVALID,
        owner_paintable: NodeSlotId::INVALID,
        nearest_self_painting_inline: NodeSlotId::INVALID,
    };
    read_rows(host, |arena| {
        let paintable_rows = arena;
        let fragments = arena.text_fragments(primary);
        let node_slots = fragments.as_slice();
        let Some(answer) =
            crate::painting::caret::caret_rect_for_position(paintable_rows, node_slots, offset, affinity_is_downstream)
        else {
            return result;
        };
        result.found = true;
        result.rect = answer.rect.into();
        result.style_source = answer.style_source;
        result.owner_paintable = answer.owner;
        result.nearest_self_painting_inline =
            crate::painting::fragment_ownership::nearest_self_painting_inline_box(paintable_rows, answer.node)
                .unwrap_or(NodeSlotId::INVALID);
        result
    })
}

/// # Safety
///
/// `host` must be a live document host, on its document's thread, used on the document
/// thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_script_atomic_inline_caret_rect_for_position(
    host: *mut DocumentHost,
    primary: NodeSlotId,
    after: bool,
) -> FfiCaretRectResult {
    let mut result = FfiCaretRectResult {
        found: false,
        rect: FfiCssPixelRect::default(),
        style_source: NodeSlotId::INVALID,
        owner_paintable: NodeSlotId::INVALID,
        nearest_self_painting_inline: NodeSlotId::INVALID,
    };
    read_rows(host, |arena| {
        let paintable_rows = arena;
        let Some(answer) = crate::painting::caret::caret_rect_for_atomic_inline(paintable_rows, primary, after) else {
            return result;
        };
        result.found = true;
        result.rect = answer.rect.into();
        result.style_source = answer.style_source;
        result.owner_paintable = answer.owner;
        result.nearest_self_painting_inline =
            crate::painting::fragment_ownership::nearest_self_painting_inline_box(paintable_rows, answer.node)
                .unwrap_or(NodeSlotId::INVALID);
        result
    })
}

/// # Safety
///
/// `host` must be a live document host, on its document's thread, used on the document
/// thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_script_paintable_empty_line_caret_rect(
    host: *mut DocumentHost,
    block: NodeSlotId,
    primary: NodeSlotId,
    offset: usize,
) -> FfiEmptyLineCaretRect {
    let mut result = FfiEmptyLineCaretRect {
        has_value: false,
        rect: FfiCssPixelRect::default(),
        style_source: NodeSlotId::INVALID,
    };
    read_rows(host, |arena| {
        let paintable_rows = arena;
        if !paintable_rows.paintable_row_is_populated(block) {
            return result;
        }
        let fragments = arena.text_fragments(primary);
        let node_slots = fragments.as_slice();
        let side = arena.committed_side_data(block);
        let Some(first_fragment) = side.fragments().first() else {
            return result;
        };
        if !node_slots.contains(&first_fragment.layout_node) {
            return result;
        }
        for target in crate::painting::visual_lines::empty_line_caret_targets(paintable_rows, block) {
            if target.offset == offset {
                result.has_value = true;
                result.rect = target.rect.into();
                result.style_source = crate::painting::text_fragment::style_source(paintable_rows, first_fragment);
                break;
            }
        }
        result
    })
}

/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_script_paintable_absolute_rect(
    host: *mut DocumentHost,
    slot: NodeSlotId,
) -> FfiCssPixelRect {
    read_rows(host, |arena| {
        crate::painting::paintable_geometry::absolute_rect_or_default(arena, slot).into()
    })
}

/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_script_paintable_absolute_padding_box_rect(
    host: *mut DocumentHost,
    slot: NodeSlotId,
) -> FfiCssPixelRect {
    read_rows(host, |arena| {
        let paintable_rows = arena;
        if !paintable_rows.paintable_row_is_populated(slot) {
            return FfiCssPixelRect::default();
        }
        crate::painting::paintable_geometry::absolute_padding_box_rect(paintable_rows, slot).into()
    })
}

/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_script_paintable_absolute_border_box_rect(
    host: *mut DocumentHost,
    slot: NodeSlotId,
) -> FfiCssPixelRect {
    read_rows(host, |arena| {
        let paintable_rows = arena;
        if !paintable_rows.paintable_row_is_populated(slot) {
            return FfiCssPixelRect::default();
        }
        crate::painting::paintable_geometry::absolute_border_box_rect(paintable_rows, slot).into()
    })
}

/// # Safety
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_script_paintable_box_model(
    host: *mut DocumentHost,
    slot: NodeSlotId,
) -> FfiBoxModelMetrics {
    read_rows(host, |arena| {
        if !arena.paintable_row_is_populated(slot) {
            return FfiBoxModelMetrics::default();
        }
        FfiBoxModelMetrics {
            margin: crate::painting::paintable_geometry::committed_margin(arena, slot),
            padding: crate::painting::paintable_geometry::committed_padding(arena, slot),
            border: crate::painting::paintable_geometry::committed_border(arena, slot),
            inset: crate::painting::paintable_geometry::committed_inset(arena, slot),
        }
    })
}

/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_script_paintable_content_size(
    host: *mut DocumentHost,
    slot: NodeSlotId,
) -> FfiCssPixelSize {
    read_rows(host, |arena| {
        let paintable_rows = arena;
        if !paintable_rows.paintable_row_is_populated(slot) {
            return FfiCssPixelSize::default();
        }
        crate::painting::paintable_geometry::committed_content_size(paintable_rows, slot)
    })
}
