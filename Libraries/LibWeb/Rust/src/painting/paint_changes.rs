/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What the host writes of a document's paint state, as typed changes its render state applies to the arena: what the
//! user scrolled, what is selected, and the state of the chrome the host draws.

use super::ffi::{FfiImageMapArea, ScrollDirection};
use super::host::{
    FfiCanvasPaintFacts, FfiFormControlPaintFacts, FfiLayerImagePaintFactsEntry, FfiNavigableContainerPaintFacts,
    FfiReplacedImagePaintFacts, FfiVideoPaintFacts,
};
use super::image_map_areas::{AreaCoverage, AreaShape, PublishedImageMapArea};
use super::layer_image_paint_facts::{LayerImagePaintFacts, LayerImagePaintFactsEntry};
use super::paint_read::GeometryRead;
use super::paintable_data::{FfiSearchTextRange, FfiSelectionEntry, PaintableFlag};
use super::record::damage::PaintDamage;
use super::replaced_paint_facts::{ImagePaintFacts, ReplacedPaintFacts, VideoPaintFacts};
use super::selection::HighlightStyleRecords;
use super::visual_context::dirty::VisualContextBoxDirtyKind;
use crate::css::css_pixels::{CssPixelPoint, FfiCssPixelPoint};
use crate::css::style::tree::StyleNodeID;
use crate::layout::LayoutNodeArena;
use crate::layout::node_data::NodeSlotId;
use crate::layout::tree_update_marks::MarkedBox;
use crate::render_state::{ArenaChange, DocumentHost};
use libcompositing_rust::ffi::ffi_slice;
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
    /// The find-in-page matches are `ranges`, each covering its span of `entries`, between the offsets in its start
    /// and end text.
    ApplySearchText {
        viewport: NodeSlotId,
        ranges: Box<[FfiSearchTextRange]>,
        entries: Box<[FfiSelectionEntry]>,
    },
    /// No find-in-page match is highlighted.
    ClearSearchText,
    /// The element's style changed: the rows that paint text under it take what `style_records`, the `::selection`
    /// and `::search-text` records the host holds for it (zero for none), say highlighted text paints with.
    SyncHighlightPseudoStyles {
        element: StyleNodeID,
        style_records: HighlightStyleRecords,
    },
    /// Whether the scrollbar of the row in `direction` is drawn enlarged, as it is while the user hovers or drags it.
    SetScrollbarEnlarged {
        node: NodeSlotId,
        direction: ScrollDirection,
        enlarged: bool,
    },
    /// The images the row's background, mask and border image layers paint.
    SetLayerImagePaintFacts {
        node: NodeSlotId,
        entries: Vec<LayerImagePaintFactsEntry>,
    },
    /// What the replaced content of the box `target` names, and of the rows sharing its DOM node, paints. A row whose
    /// facts changed paints again.
    SetReplacedPaintFacts {
        target: MarkedBox,
        facts: ReplacedPaintFacts,
    },
    /// Replaced content facts that later facts replace.
    SupersededReplacedPaintFacts,
    /// The `<area>` elements of the image map the image `node` is associated with, in tree order.
    PublishImageMapAreas {
        node: NodeSlotId,
        areas: Box<[PublishedImageMapArea]>,
    },
    /// What the row's visual context is derived from changed.
    NoteVisualContextBoxDirty {
        node: NodeSlotId,
        kind: VisualContextBoxDirtyKind,
    },
    /// Every row paints again.
    InvalidateAllPaintCaches,
    /// The visual context tree is built again whole before it is published next.
    RequestFullVisualContextRebuild(super::visual_context::dirty::VisualContextUpdateScope),
    /// The SVG paint resources the rows enrolled may have changed: they are synced again before they are painted next.
    SvgPaintResourcesChanged,
    /// Whether the document's recordings are traced.
    SetRecordingTraceEnabled(bool),
    /// The host published the recording `output` and takes it in as the document's last.
    TakeInRecording {
        output: Arc<super::record::RecordingOutput>,
        hit_test_list_changed: bool,
        publishes_recording: bool,
    },
}

impl PaintChange {
    /// How far the change may write the rows. Noting what the visual context tree is to be updated for writes nothing
    /// the rows answer before the tree is updated.
    pub(crate) fn row_write(&self) -> crate::render_state::RowWrite {
        match self {
            Self::NoteVisualContextBoxDirty { .. } | Self::RequestFullVisualContextRebuild(_) => {
                crate::render_state::RowWrite::None
            }
            _ => crate::render_state::RowWrite::Rows,
        }
    }

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
            Self::ApplySearchText {
                viewport,
                ranges,
                entries,
            } => {
                if arena.paintable_row_is_populated(viewport) {
                    super::selection::apply_search_text(&mut arena.paintable_rows_mut(), &ranges, &entries);
                }
            }
            Self::ClearSearchText => super::selection::clear_search_text(&mut arena.paintable_rows_mut()),
            Self::SyncHighlightPseudoStyles { element, style_records } => {
                super::selection::sync_highlight_pseudo_styles(arena, element, style_records);
            }
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
            Self::SetLayerImagePaintFacts { node, entries } => {
                arena.set_layer_image_paint_facts(node, entries);
            }
            Self::SetReplacedPaintFacts { target, facts } => arena.set_replaced_paint_facts(target, facts),
            Self::SupersededReplacedPaintFacts => {}
            Self::PublishImageMapAreas { node, areas } => arena.image_map_areas().publish(node, areas),
            Self::NoteVisualContextBoxDirty { node, kind } => {
                if arena.paintable_row_is_populated(node) {
                    arena.note_visual_context_box_dirty(node, kind);
                }
            }
            Self::InvalidateAllPaintCaches => arena.push_all_paint_damage(),
            Self::RequestFullVisualContextRebuild(scope) => arena.request_full_visual_context_rebuild(scope),
            Self::SvgPaintResourcesChanged => {
                arena.svg_paint_resources().note_changed();
            }
            Self::SetRecordingTraceEnabled(enabled) => arena.paint_state().borrow_mut().trace_recordings = enabled,
            Self::TakeInRecording {
                output,
                hit_test_list_changed,
                publishes_recording,
            } => {
                super::record::publish::take_in_recording(arena, output, hit_test_list_changed, publishes_recording);
            }
        }
    }
}

/// Queues `change` for the render state of `host`'s document.
///
/// # Safety
///
/// `host` must come from `document_host_create` and not be destroyed yet, on the document's thread.
pub(crate) unsafe fn queue(host: &DocumentHost, change: PaintChange) {
    host.queue_change(ArenaChange::Paint(change));
}

/// # Safety
///
/// `host` must be a live document host, on the document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_set_viewport_scroll_offset(host: &DocumentHost, offset: FfiCssPixelPoint) {
    // SAFETY: Guaranteed by the caller.
    unsafe { queue(host, PaintChange::SetViewportScrollOffset(offset.into())) };
}

/// # Safety
///
/// `host` must be a live document host, on the document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_invalidate_scroll_state(host: &DocumentHost) {
    // SAFETY: Guaranteed by the caller.
    unsafe { queue(host, PaintChange::InvalidateScrollState) };
}

/// # Safety
///
/// `host` must be a live document host, on the document's thread, and `entries` must point at `entry_count` readable
/// entries.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_apply_selection(
    host: &DocumentHost,
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
pub unsafe extern "C" fn render_state_clear_selection(host: &DocumentHost, viewport: NodeSlotId) {
    // SAFETY: Guaranteed by the caller.
    unsafe { queue(host, PaintChange::ClearSelection { viewport }) };
}

/// # Safety
///
/// `host` must be a live document host, on the document's thread, `ranges` must point at `range_count` readable
/// ranges and `entries` at `entry_count` readable entries.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_apply_search_text(
    host: &DocumentHost,
    viewport: NodeSlotId,
    ranges: *const FfiSearchTextRange,
    range_count: usize,
    entries: *const FfiSelectionEntry,
    entry_count: usize,
) {
    let ranges = if range_count == 0 {
        Box::default()
    } else {
        // SAFETY: Guaranteed by the caller.
        unsafe { std::slice::from_raw_parts(ranges, range_count) }.into()
    };
    let entries = if entry_count == 0 {
        Box::default()
    } else {
        // SAFETY: Guaranteed by the caller.
        unsafe { std::slice::from_raw_parts(entries, entry_count) }.into()
    };
    host.host_tables().shows_search_text.set(range_count != 0);
    let change = PaintChange::ApplySearchText {
        viewport,
        ranges,
        entries,
    };
    // SAFETY: Guaranteed by the caller.
    unsafe { queue(host, change) };
}

/// # Safety
///
/// `host` must be a live document host, on the document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_clear_search_text(host: &DocumentHost) {
    // Rows that show no search text have none to clear, which spares the write that would leave the rows the host
    // holds stale.
    if !host.host_tables().shows_search_text.replace(false) {
        return;
    }
    // SAFETY: Guaranteed by the caller.
    unsafe { queue(host, PaintChange::ClearSearchText) };
}

/// # Safety
///
/// `host` must be a live document host, on the document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_sync_highlight_pseudo_styles(
    host: &DocumentHost,
    element: u32,
    selection_style_record: u64,
    search_text_style_record: u64,
    search_text_current_style_record: u64,
) {
    let Some(element) = StyleNodeID::from_raw(element) else {
        return;
    };
    let change = PaintChange::SyncHighlightPseudoStyles {
        element,
        style_records: [
            selection_style_record,
            search_text_style_record,
            search_text_current_style_record,
        ],
    };
    // SAFETY: Guaranteed by the caller.
    unsafe { queue(host, change) };
}

/// # Safety
///
/// `host` must be a live document host, on the document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_set_scrollbar_enlarged(
    host: &DocumentHost,
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

/// # Safety
///
/// `host` must be a live document host, on the document's thread, and `entries` must point at `count` readable
/// entries whose images are live.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_set_layer_image_paint_facts(
    host: &DocumentHost,
    node: NodeSlotId,
    entries: *const FfiLayerImagePaintFactsEntry,
    count: usize,
) {
    let entries = if count == 0 {
        Vec::new()
    } else {
        // SAFETY: Guaranteed by the caller.
        unsafe { std::slice::from_raw_parts(entries, count) }
            .iter()
            .map(|entry| LayerImagePaintFactsEntry {
                list: entry.list,
                computed_index: entry.computed_index,
                // SAFETY: Guaranteed by the caller.
                facts: unsafe { LayerImagePaintFacts::from_ffi(&entry.facts) },
            })
            .collect()
    };
    // A row the host gave no facts has none to clear, which spares the write that would leave the rows the host holds
    // stale.
    let mut rows_with_facts = host.host_tables().rows_with_layer_image_paint_facts.borrow_mut();
    if entries.is_empty() {
        if !rows_with_facts.remove(&node) {
            return;
        }
    } else {
        rows_with_facts.insert(node);
    }
    drop(rows_with_facts);
    // SAFETY: Guaranteed by the caller.
    unsafe { queue(host, PaintChange::SetLayerImagePaintFacts { node, entries }) };
}

/// # Safety
///
/// `host` must be a live document host, on the document's thread.
unsafe fn queue_replaced_paint_facts(host: &DocumentHost, target: MarkedBox, facts: ReplacedPaintFacts) {
    // SAFETY: Guaranteed by the caller.
    unsafe { queue(host, PaintChange::SetReplacedPaintFacts { target, facts }) };
}

/// Queues `facts`, read off the element with `element`, for the box bound to it, which the render state finds as it
/// applies them: the element may change beside a frame in flight, which holds the boxes.
///
/// # Safety
///
/// `host` must be a live document host, on the document's thread.
unsafe fn queue_element_paint_facts(host: &DocumentHost, element: u32, facts: ReplacedPaintFacts) {
    if let Some(element) = StyleNodeID::from_raw(element) {
        // SAFETY: Guaranteed by the caller.
        unsafe { queue_replaced_paint_facts(host, MarkedBox::Node(Some(element)), facts) };
    }
}

/// # Safety
///
/// `host` must be a live document host, on the document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_set_form_control_paint_facts(
    host: &DocumentHost,
    element: u32,
    facts: FfiFormControlPaintFacts,
) {
    // SAFETY: Guaranteed by the caller.
    unsafe { queue_element_paint_facts(host, element, ReplacedPaintFacts::FormControl(facts)) };
}

/// The canvas paints its surface's content of this size and generation now: where that changed, so does what its box
/// paints.
///
/// # Safety
///
/// `host` must be a live document host, on the document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_set_canvas_paint_facts(
    host: &DocumentHost,
    element: u32,
    facts: FfiCanvasPaintFacts,
) {
    // SAFETY: Guaranteed by the caller.
    unsafe { queue_element_paint_facts(host, element, ReplacedPaintFacts::Canvas(facts)) };
}

/// The navigable container paints the compositor context of its content, if it has one: where that changed, so does
/// what its row paints.
///
/// # Safety
///
/// `host` must be a live document host, on the document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_set_navigable_container_paint_facts(
    host: &DocumentHost,
    node: NodeSlotId,
    facts: FfiNavigableContainerPaintFacts,
) {
    let facts = ReplacedPaintFacts::NavigableContainer(facts);
    // SAFETY: Guaranteed by the caller.
    unsafe { queue_replaced_paint_facts(host, MarkedBox::Row(node), facts) };
}

/// The image box `node` shows the image of the provider its box holds: its own, or its element's.
///
/// # Safety
///
/// `host` must be a live document host, on the document's thread, and the image `facts` names must be live.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_set_replaced_image_paint_facts(
    host: &DocumentHost,
    node: NodeSlotId,
    facts: FfiReplacedImagePaintFacts,
) {
    // SAFETY: Guaranteed by the caller.
    let facts = unsafe { ImagePaintFacts::from_ffi(&facts) };
    // SAFETY: Guaranteed by the caller.
    unsafe { queue_replaced_paint_facts(host, MarkedBox::Row(node), ReplacedPaintFacts::Image(facts)) };
}

/// The image of the element with `element` is the one `facts` describes, which its box shows unless the box owns the
/// provider of an image of its own.
///
/// # Safety
///
/// `host` must be a live document host, on the document's thread, and the image `facts` names must be live.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_set_element_image_paint_facts(
    host: &DocumentHost,
    element: u32,
    facts: FfiReplacedImagePaintFacts,
) {
    // SAFETY: Guaranteed by the caller.
    let facts = unsafe { ImagePaintFacts::from_ffi(&facts) };
    // SAFETY: Guaranteed by the caller.
    unsafe { queue_element_paint_facts(host, element, ReplacedPaintFacts::Image(facts)) };
}

/// # Safety
///
/// `host` must be a live document host, on the document's thread, and the poster frame `facts` names, if any, must be
/// live.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_set_video_paint_facts(
    host: &DocumentHost,
    element: u32,
    facts: FfiVideoPaintFacts,
) {
    // SAFETY: Guaranteed by the caller.
    let facts = unsafe { VideoPaintFacts::from_ffi(&facts) };
    // SAFETY: Guaranteed by the caller.
    unsafe { queue_element_paint_facts(host, element, ReplacedPaintFacts::Video(facts)) };
}

/// Publishes the `<area>` elements of the image map the image `node` is associated with, in tree order. Publishing no
/// area is how an image with no image map is named.
///
/// # Safety
///
/// `host` must be a live document host, on the document's thread; `areas` must point at `area_count` readable areas,
/// and `coords` at `coords_count` readable coordinates, which the areas index.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_publish_image_map_areas(
    host: &DocumentHost,
    node: NodeSlotId,
    areas: *const FfiImageMapArea,
    area_count: usize,
    coords: *const f64,
    coords_count: usize,
) {
    // SAFETY: Guaranteed by the caller.
    let (areas, coords) = unsafe { (ffi_slice(areas, area_count), ffi_slice(coords, coords_count)) };
    let areas = areas
        .iter()
        .map(|area| {
            let start = area.coords_offset as usize;
            PublishedImageMapArea {
                style_node: area.style_node,
                coverage: AreaCoverage::new(
                    AreaShape::from_raw(area.shape),
                    &coords[start..start + area.coords_count as usize],
                ),
            }
        })
        .collect();
    // SAFETY: Guaranteed by the caller.
    unsafe { queue(host, PaintChange::PublishImageMapAreas { node, areas }) };
}

/// Notes a style change of `node` for its visual contexts: one that changes their structure, or only their values.
///
/// # Safety
///
/// `host` must be a live document host, on the document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_note_visual_context_style_change(
    host: &DocumentHost,
    node: NodeSlotId,
    changes_structure: bool,
) {
    let kind = if changes_structure {
        VisualContextBoxDirtyKind::StyleStructuralChange
    } else {
        VisualContextBoxDirtyKind::StyleValueChange
    };
    // SAFETY: Guaranteed by the caller.
    unsafe { queue(host, PaintChange::NoteVisualContextBoxDirty { node, kind }) };
}

/// # Safety
///
/// `host` must be a live document host, on the document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_invalidate_all_paint_caches(host: &DocumentHost) {
    // SAFETY: Guaranteed by the caller.
    unsafe { queue(host, PaintChange::InvalidateAllPaintCaches) };
}

/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_visual_context_request_full_rebuild(
    host: &DocumentHost,
    scope: crate::painting::visual_context::dirty::VisualContextUpdateScope,
) {
    // SAFETY: Guaranteed by the caller.
    unsafe { queue(host, PaintChange::RequestFullVisualContextRebuild(scope)) };
}

/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_set_recording_trace_enabled(host: &DocumentHost, enabled: bool) {
    // SAFETY: Guaranteed by the caller.
    unsafe { queue(host, PaintChange::SetRecordingTraceEnabled(enabled)) };
}
