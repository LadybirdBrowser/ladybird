/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What the host writes of a document's paint state, as typed changes its render state applies to the arena: what the
//! user scrolled, what is selected, and the state of the chrome the host draws.

use super::ffi::ScrollDirection;
use super::paint_read::GeometryRead;
use super::paintable_data::{FfiSelectionEntry, PaintableFlag};
use super::record::damage::PaintDamage;
use crate::css::css_pixels::{CssPixelPoint, FfiCssPixelPoint};
use crate::css::style::tree::StyleNodeID;
use crate::layout::LayoutNodeArena;
use crate::layout::node_data::NodeSlotId;
use crate::render_state::{ArenaChange, DocumentHost};
use std::sync::Arc;

/// One write of the host to a document's paint state.
pub(crate) enum PaintChange {
    /// What the navigable has scrolled the viewport to.
    SetViewportScrollOffset(CssPixelPoint),
    /// The scroll containers' offsets moved: the scroll state is read again before it is published next.
    InvalidateScrollState,
    /// The selection covers `entries`, between the offsets in its start and end text.
    ApplySelection {
        viewport: NodeSlotId,
        entries: Box<[FfiSelectionEntry]>,
        start_offset: usize,
        end_offset: usize,
    },
    /// Nothing is selected.
    ClearSelection { viewport: NodeSlotId },
    /// The element's style changed: the rows that paint text under it take what its published `::selection` record
    /// says selected text paints with.
    SyncSelectionPseudoStyle { element: StyleNodeID },
    /// Whether the scrollbar of the row in `direction` is drawn enlarged, as it is while the user hovers or drags it.
    SetScrollbarEnlarged {
        node: NodeSlotId,
        direction: ScrollDirection,
        enlarged: bool,
    },
}

impl PaintChange {
    /// Applies the change to `arena`, the arena of the document it was queued for.
    pub(crate) fn apply(self, arena: &mut LayoutNodeArena) {
        match self {
            Self::SetViewportScrollOffset(offset) => arena.set_viewport_scroll_offset(offset),
            Self::InvalidateScrollState => {
                arena
                    .paint_state()
                    .borrow_mut()
                    .visual_context
                    .needs_to_refresh_scroll_state = true;
            }
            Self::ApplySelection {
                viewport,
                entries,
                start_offset,
                end_offset,
            } => {
                if !arena.paintable_row_is_populated(viewport) {
                    return;
                }
                let text_states = super::selection::apply(&mut arena.paintable_rows_mut(), viewport, &entries);
                arena.paint_state().borrow_mut().selection = Some(Arc::new(super::selection::SelectionRange {
                    start_offset,
                    end_offset,
                    text_states,
                }));
            }
            Self::ClearSelection { viewport } => {
                if arena.paintable_row_is_populated(viewport) {
                    super::selection::clear(&mut arena.paintable_rows_mut(), viewport);
                }
            }
            Self::SyncSelectionPseudoStyle { element } => super::selection::sync_selection_pseudo_style(arena, element),
            Self::SetScrollbarEnlarged {
                node,
                direction,
                enlarged,
            } => {
                let mut rows = arena.paintable_rows_mut();
                if !rows.paintable_row_is_populated(node) {
                    return;
                }
                let flag = match direction {
                    ScrollDirection::Horizontal => PaintableFlag::HorizontalScrollbarEnlarged,
                    ScrollDirection::Vertical => PaintableFlag::VerticalScrollbarEnlarged,
                };
                if rows.paintable_data(node).has_flag(flag) == enlarged {
                    return;
                }
                rows.paintable_data_mut(node).set_flag(flag, enlarged);
                rows.push_paint_damage(node, PaintDamage::DRAW_OVERLAY | PaintDamage::HIT_OVERLAY);
            }
        }
    }
}

/// Queues `change` for the render state of `host`'s document.
///
/// # Safety
///
/// `host` must come from `document_host_create` and not be destroyed yet, on the document's thread.
unsafe fn queue(host: *const DocumentHost, change: PaintChange) {
    assert!(!host.is_null(), "document host is null");
    // SAFETY: Guaranteed by the caller.
    unsafe { &*host }.queue_change(ArenaChange::Paint(change));
}

/// # Safety
///
/// `host` must be a live document host, on the document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_set_viewport_scroll_offset(host: *const DocumentHost, offset: FfiCssPixelPoint) {
    // SAFETY: Guaranteed by the caller.
    unsafe { queue(host, PaintChange::SetViewportScrollOffset(offset.into())) };
}

/// # Safety
///
/// `host` must be a live document host, on the document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_invalidate_scroll_state(host: *const DocumentHost) {
    // SAFETY: Guaranteed by the caller.
    unsafe { queue(host, PaintChange::InvalidateScrollState) };
}

/// # Safety
///
/// `host` must be a live document host, on the document's thread, and `entries` must point at `entry_count` readable
/// entries.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_apply_selection(
    host: *const DocumentHost,
    viewport: NodeSlotId,
    entries: *const FfiSelectionEntry,
    entry_count: usize,
    start_offset: usize,
    end_offset: usize,
) {
    let entries = if entry_count == 0 {
        Box::default()
    } else {
        // SAFETY: Guaranteed by the caller.
        unsafe { std::slice::from_raw_parts(entries, entry_count) }.into()
    };
    let change = PaintChange::ApplySelection {
        viewport,
        entries,
        start_offset,
        end_offset,
    };
    // SAFETY: Guaranteed by the caller.
    unsafe { queue(host, change) };
}

/// # Safety
///
/// `host` must be a live document host, on the document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_clear_selection(host: *const DocumentHost, viewport: NodeSlotId) {
    // SAFETY: Guaranteed by the caller.
    unsafe { queue(host, PaintChange::ClearSelection { viewport }) };
}

/// # Safety
///
/// `host` must be a live document host, on the document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_sync_selection_pseudo_style(host: *const DocumentHost, element: u32) {
    let Some(element) = StyleNodeID::from_raw(element) else {
        return;
    };
    // SAFETY: Guaranteed by the caller.
    unsafe { queue(host, PaintChange::SyncSelectionPseudoStyle { element }) };
}

/// # Safety
///
/// `host` must be a live document host, on the document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_set_scrollbar_enlarged(
    host: *const DocumentHost,
    node: NodeSlotId,
    direction: ScrollDirection,
    enlarged: bool,
) {
    let change = PaintChange::SetScrollbarEnlarged {
        node,
        direction,
        enlarged,
    };
    // SAFETY: Guaranteed by the caller.
    unsafe { queue(host, change) };
}
