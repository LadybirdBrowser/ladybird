/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::css_pixels::{CssPixelRect, CssPixels};
use crate::css::ffi_support::FfiUtf16View;
use crate::layout::LayoutNodeArena;
use crate::layout::node_data::NodeSlotId;
use crate::layout::svg_formatting_context;
use crate::layout::used_values::FfiCssPixelPoint;
use crate::layout::used_values::FfiCssPixelRect;
use crate::layout::used_values::FfiCssPixelSize;
use crate::painting::filter_bytes::{FfiFilterFunction, filter_functions_graph};
use crate::painting::force_dark::ForceDarkRole;
use crate::painting::host::visual_context::FfiSvgFilterPrimitive;
use crate::painting::paint_read::GeometryRead;
use crate::painting::paintable_data::*;
use crate::painting::paintable_rows::{PaintableRowsRead, with_inline_pieces};
use crate::painting::rect_to_viewport_transform::RectToViewportTransform;
use crate::painting::scroll_chain::ViewportWheelOverflow;
use crate::painting::svg_filter::SvgFilterPrimitive;
use crate::painting::svg_paint_resources::{PublishedSvgFilter, PublishedSvgPaintServer, SvgPaintResourceKind};
use libcompositing_rust::ffi::{ffi_slice, tree_from_handle};
use libgfx_rust::filter::Filter;
use std::ffi::c_void;

mod main_thread_entries;

pub(crate) use main_thread_entries::{InputReadsBoxes, MainThreadFfiEntry, ScrollSnaps};

/// SAFETY: `arena` must be a live handle from `render_state_arena_for_unconverted_entry`, borrowed for this call on
/// the document thread.
pub(crate) unsafe fn arena_from_handle<'a>(arena: *mut c_void) -> &'a LayoutNodeArena {
    unsafe { LayoutNodeArena::from_handle(arena) }
}

/// SAFETY: `arena` must be a live handle from `render_state_arena_for_unconverted_entry`, exclusively borrowed for
/// this call on the document thread. No C++ callback may re-enter the arena during the borrow.
unsafe fn arena_from_handle_mut<'a>(arena: *mut c_void) -> &'a mut LayoutNodeArena {
    unsafe { LayoutNodeArena::from_handle_mut(arena) }
}

/// Clears the committed box of `layout_node` and tells the document's chrome state, at once or
/// once the tree build that clears it is over.
///
/// # Safety
///
/// `arena` must be a live handle from `render_state_arena_for_unconverted_entry` with no outstanding borrow, used on
/// the document thread.
pub(crate) unsafe fn paintable_cleared_from_node(
    host_calls: crate::layout::tree_mutation::HostCalls<'_>,
    arena: *mut c_void,
    layout_node: NodeSlotId,
) {
    let reset = {
        let arena = unsafe { arena_from_handle(arena) };
        arena.clear_committed_fragment_link(layout_node);
        arena.prepare_paintable_row_cleared_reset(layout_node)
    };
    if let Some(reset) = reset {
        host_calls.paintable_row_reset(reset);
        let arena = unsafe { arena_from_handle_mut(arena) };
        arena.paintable_row_cleared(reset);
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(u8)]
pub enum ScrollDirection {
    #[default]
    Horizontal,
    Vertical,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(C)]
pub struct FfiChromeMetrics {
    pub scroll_thumb_min_length: CssPixels,
    pub scroll_thumb_padding_thin: CssPixels,
    pub scroll_thumb_thickness_thin: CssPixels,
    pub scroll_thumb_thickness: CssPixels,
    pub scroll_gutter_thickness: CssPixels,
    pub resize_gripper_size: CssPixels,
    pub resize_gripper_padding: CssPixels,
}

#[derive(Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct FfiPhysicalResizeAxes {
    pub horizontal: bool,
    pub vertical: bool,
}

#[derive(Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct FfiScrollbarData {
    pub gutter_rect: FfiCssPixelRect,
    pub thumb_rect: FfiCssPixelRect,
    pub track_rect: FfiCssPixelRect,
    pub thumb_travel_to_scroll_ratio: f64,
}

#[derive(Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct FfiOptionalScrollbarData {
    pub has_value: bool,
    pub value: FfiScrollbarData,
}

/// # Safety
///
/// `arena` must be a live handle from `render_state_arena_for_unconverted_entry`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_set_chrome_state_callback(
    arena: *mut c_void,
    context: *mut c_void,
    callback: unsafe extern "C" fn(*mut c_void, NodeSlotId, PaintableRowResetKind),
) {
    unsafe { crate::layout::HostTables::from_handle(arena) }
        .chrome_state_callback
        .set(Some((context, callback)));
}

/// # Safety
///
/// `arena` must be a live handle from `render_state_arena_for_unconverted_entry`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_clear_chrome_state_callback(arena: *mut c_void) {
    unsafe { crate::layout::HostTables::from_handle(arena) }
        .chrome_state_callback
        .set(None);
}

/// # Safety
///
/// `arena` must be a live handle from `render_state_arena_for_unconverted_entry`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_paintable_row(arena: *mut c_void, slot: NodeSlotId) -> *const PaintableData {
    let arena = unsafe { arena_from_handle(arena) };
    let paintable_rows = arena.paintable_rows();
    if !paintable_rows.paintable_row_is_populated(slot) {
        return std::ptr::null();
    }
    paintable_rows.paintable_data_ptr(slot)
}

#[repr(C)]
pub struct FfiPhysicalOverflowDirections {
    pub horizontal_axis_is_positive: bool,
    pub vertical_axis_is_positive: bool,
}

/// Re-reads the scroll containers' offsets when something invalidated them since the last
/// refresh, resolves the sticky nodes' offsets on top of them, and hands the dense device-pixel
/// snapshot to `publish`. Returns whether that happened, so the caller keeps its copy otherwise;
/// `force` re-derives the snapshot even when nothing invalidated it, for verification.
///
/// # Safety
///
/// `arena` must be a live handle from `render_state_arena_for_unconverted_entry`, used on the document thread;
/// `publish` is called synchronously with `sink` and a view of the snapshot that is valid only
/// for the duration of that call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_refresh_scroll_state(
    arena: *mut c_void,
    force: bool,
    sink: *mut c_void,
    publish: unsafe extern "C" fn(*mut c_void, *const libgfx_rust::FloatPoint, usize),
) -> bool {
    let arena = unsafe { arena_from_handle(arena) };
    arena.measure_scrollable_overflow();
    let snapshot = {
        let paintable_rows = arena.paintable_rows();
        let mut paint_state = arena.paint_state().borrow_mut();
        let state = &mut paint_state.visual_context;
        if !force && !state.needs_to_refresh_scroll_state {
            return false;
        }
        state.needs_to_refresh_scroll_state = false;
        crate::painting::visual_context::refresh::refresh_scroll_state(&paintable_rows, &mut state.scroll_state);
        let mut snapshot = state
            .scroll_state
            .snapshot(arena.visual_context_tree_inputs().device_pixels_per_css_pixel);
        // https://drafts.csswg.org/css-position/#sticky-pos
        if let Some(tree) = state.tree.as_deref() {
            tree.resolve_sticky_offsets_in_place(&mut snapshot);
        }
        snapshot
    };
    // SAFETY: The C++ sink copies the offsets synchronously.
    unsafe { publish(sink, snapshot.as_ptr(), snapshot.len()) };
    true
}

/// Publishes what the render side needs to know about the viewport it draws into: the device
/// scale, where the visual viewport sits and how far it is zoomed, and the overflow the viewport
/// applies to a wheel. The document publishes it before each pass that reads it, so no pass asks.
///
/// # Safety
///
/// `arena` must be a live handle from `render_state_arena_for_unconverted_entry`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_publish_visual_context_tree_inputs(
    arena: *mut c_void,
    inputs: crate::painting::host::FfiVisualContextTreeInputs,
) {
    unsafe { arena_from_handle(arena) }.publish_visual_context_tree_inputs(inputs);
}

/// # Safety
///
/// `arena` must be a live handle from `render_state_arena_for_unconverted_entry`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_update_accumulated_visual_contexts(
    arena: *mut c_void,
    viewport: NodeSlotId,
) -> crate::painting::host::FfiVisualContextUpdateOutcome {
    use crate::painting::visual_context::dirty::{VisualContextGlobalRebuildReason, VisualContextUpdateScope};
    use crate::painting::visual_context::incremental::{
        IncrementalUpdateResult, debug_assert_every_live_node_is_owned, update_visual_context_tree,
    };
    let arena_ref = unsafe { arena_from_handle(arena) };
    arena_ref.measure_scrollable_overflow();
    if !arena_ref.paintable_row_is_populated(viewport) {
        return crate::painting::host::FfiVisualContextUpdateOutcome::default();
    }
    let inputs = arena_ref.visual_context_tree_inputs();
    let mut state = std::mem::take(&mut arena_ref.paint_state().borrow_mut().visual_context);
    state.release_quarantined_slots_while_no_handle_is_retained();

    let mut reason = state.dirty_boxes.global_reason;
    if state.tree.is_none() {
        reason = reason.max(VisualContextGlobalRebuildReason::FirstBuild);
    }
    if state.last_tree_inputs.is_some_and(|last| {
        last.device_pixels_per_css_pixel != inputs.device_pixels_per_css_pixel
            || last.viewport_wheel_overflow_x != inputs.viewport_wheel_overflow_x
            || last.viewport_wheel_overflow_y != inputs.viewport_wheel_overflow_y
    }) {
        reason = reason.max(VisualContextGlobalRebuildReason::TreeInputsChanged);
    }
    if state.tree.as_deref().is_some_and(|tree| tree.should_compact()) {
        reason = reason.max(VisualContextGlobalRebuildReason::Compaction);
    }

    loop {
        let scope = VisualContextUpdateScope::for_reason(reason);
        if scope == VisualContextUpdateScope::FreshTree {
            break;
        }
        let result = {
            let paintable_rows = arena_ref.paintable_rows();
            update_visual_context_tree(&paintable_rows, viewport, inputs, scope, &mut state)
        };
        match result {
            IncrementalUpdateResult::Applied(mut outcome) => {
                let arena_mut = unsafe { arena_from_handle_mut(arena) };
                apply_walk_assignments(arena_mut, viewport, &mut outcome, &mut state);
                arena_mut.resort_stacking_context_entries_flagged_for_resort();
                crate::painting::fragment_ownership::assign_fragment_ownership_for_pending_line_roots(arena_mut);
                let performed_full_build = scope == VisualContextUpdateScope::EveryBox;
                if performed_full_build {
                    state.build_count += 1;
                    state.last_full_build_reason = reason;
                    debug_assert_every_live_node_is_owned(
                        &arena_mut.paintable_rows(),
                        state.tree.as_deref().expect("an applied walk keeps the tree"),
                        viewport,
                    );
                } else {
                    state.incremental_update_count += 1;
                }
                let structural_epoch_changed = outcome.delta.structural_epoch_changed;
                let requires_display_list_recording = outcome.delta.requires_display_list_recording;
                state.dirty_boxes.clear();
                state.last_tree_inputs = Some(inputs);
                let structural_epoch = state.structural_epoch();
                arena_mut.paint_state().borrow_mut().visual_context = state;
                return crate::painting::host::FfiVisualContextUpdateOutcome {
                    performed_full_build,
                    structural_epoch_changed,
                    requires_display_list_recording,
                    structural_epoch,
                };
            }
            IncrementalUpdateResult::NeedsFullBuild(fallback_reason) => {
                assert!(
                    VisualContextUpdateScope::for_reason(fallback_reason) > scope,
                    "a fallback widens the update scope"
                );
                reason = fallback_reason;
            }
        }
    }

    state.last_full_build_reason = reason;
    let outcome = fresh_visual_context_tree_build(arena, viewport, inputs, &mut state);
    state.last_tree_inputs = Some(inputs);
    let arena_ref = unsafe { arena_from_handle(arena) };
    arena_ref.paint_state().borrow_mut().visual_context = state;
    outcome
}

/// # Safety
///
/// `arena` must be a live handle from `render_state_arena_for_unconverted_entry`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_update_visual_viewport_transform(arena: *mut c_void) -> bool {
    let arena = unsafe { arena_from_handle(arena) };
    let mut paint_state = arena.paint_state().borrow_mut();
    let Some(tree) = &mut paint_state.visual_context.tree else {
        return false;
    };
    let inputs = arena.visual_context_tree_inputs();
    std::sync::Arc::make_mut(tree).set_visual_viewport_transform(
        crate::painting::visual_context::node_values::visual_viewport_transform_data(&inputs),
    );
    true
}

/// # Safety
///
/// `arena` must be a live arena on the document thread. The callbacks must remain
/// valid until it is destroyed and must not mutate layout geometry.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_set_geometry_host(
    arena: *mut c_void,
    host: crate::painting::host::FfiGeometryHostCallbacks,
) {
    // SAFETY: Guaranteed by the caller.
    unsafe { crate::layout::HostTables::from_handle(arena) }
        .geometry_host
        .set(Some(host));
}

/// # Safety
///
/// `arena` must be a live arena used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_paintable_scrollable_overflow(
    arena: *mut c_void,
    slot: NodeSlotId,
) -> FfiOptionalOverflowData {
    let arena = unsafe { arena_from_handle(arena) };
    arena.measure_scrollable_overflow();
    let Some(rect) = crate::painting::paintable_geometry::scrollable_overflow_rect(&arena.paintable_rows(), slot)
    else {
        return FfiOptionalOverflowData::default();
    };
    let mut value = arena.committed_side_data(slot).overflow_relative_to_padding_box;
    value.rect = rect.into();
    FfiOptionalOverflowData { has_value: true, value }
}

#[derive(Default)]
#[repr(C)]
pub struct FfiRenderingPreparationOutcome {
    pub requires_display_list_recording: bool,
    pub requires_visual_context_update: bool,
    pub visual_context_values_changed: bool,
}

fn prepare_for_rendering(
    main_thread: &crate::stage::MainThread,
    arena: &LayoutNodeArena,
    root_background_source: crate::painting::host::RootBackgroundSource,
    visual_context_update_pending: bool,
) -> FfiRenderingPreparationOutcome {
    let background_source_changed = arena
        .paint_state()
        .borrow_mut()
        .update_root_background_source(arena, root_background_source);
    // This measures all overflow left unmeasured, including the root's: the root background covers
    // the viewport united with it, so a flip in its scrollability is seen here rather than while
    // recording holds the paint state.
    crate::painting::scrollable_overflow::update_scrollable_overflow(main_thread, arena);
    let changed = arena.scrollable_overflow.geometry_changed.replace(false);
    let flipped = arena.scrollable_overflow.scrollability_changed.replace(false);
    let mut visual_context_values_changed = false;
    if changed && !flipped && !visual_context_update_pending {
        let rows = arena.paintable_rows();
        let mut state = arena.paint_state().borrow_mut();
        let state = &mut state.visual_context;
        if let Some(tree) = state.tree.as_mut() {
            visual_context_values_changed = crate::painting::visual_context::refresh::refresh_sticky_constraints(
                &rows,
                &state.scroll_state,
                tree,
                &arena.visual_context_tree_inputs(),
            );
        }
        state.needs_to_refresh_scroll_state = true;
    }
    FfiRenderingPreparationOutcome {
        requires_display_list_recording: changed || background_source_changed,
        requires_visual_context_update: flipped,
        visual_context_values_changed,
    }
}

/// # Safety
///
/// `arena` must be a live arena used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_scrollable_overflow_recalculation_count(arena: *mut c_void, reset: bool) -> u64 {
    let state = &unsafe { arena_from_handle(arena) }.scrollable_overflow;
    if reset {
        state.recalculations.replace(0)
    } else {
        state.recalculations.get()
    }
}

#[derive(Default)]
#[repr(C)]
pub struct FfiOptionalOverflowData {
    pub has_value: bool,
    pub value: crate::painting::paintable_data::FfiOverflowData,
}

#[repr(C)]
#[derive(Default)]
pub struct FfiBoxModelMetrics {
    pub margin: crate::painting::paintable_data::FfiPixelBox,
    pub padding: crate::painting::paintable_data::FfiPixelBox,
    pub border: crate::painting::paintable_data::FfiPixelBox,
    pub inset: crate::painting::paintable_data::FfiPixelBox,
}

/// # Safety
///
/// `arena` must be a live handle from `render_state_arena_for_unconverted_entry`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_paintable_content_size(arena: *mut c_void, slot: NodeSlotId) -> FfiCssPixelSize {
    let arena = unsafe { arena_from_handle(arena) };
    let paintable_rows = arena.paintable_rows();
    if !paintable_rows.paintable_row_is_populated(slot) {
        return FfiCssPixelSize::default();
    }
    crate::painting::paintable_geometry::committed_content_size(&paintable_rows, slot)
}

/// # Safety
///
/// `arena` must be a live handle from `render_state_arena_for_unconverted_entry`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_paintable_svg_viewport_size(
    arena: *mut c_void,
    slot: NodeSlotId,
) -> FfiCssPixelSize {
    let arena = unsafe { arena_from_handle(arena) };
    let paintable_rows = arena.paintable_rows();
    if !paintable_rows.paintable_row_is_populated(slot) {
        return FfiCssPixelSize::default();
    }
    crate::painting::paintable_geometry::committed_svg_viewport_size(&paintable_rows, slot)
}

#[repr(C)]
pub struct FfiOptionalAffineTransform {
    pub has_value: bool,
    pub transform: svg_formatting_context::FfiAffineTransform,
}

/// # Safety
///
/// `arena` must be a live handle from `render_state_arena_for_unconverted_entry`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_paintable_svg_viewport_transform(
    arena: *mut c_void,
    slot: NodeSlotId,
) -> FfiOptionalAffineTransform {
    let arena = unsafe { arena_from_handle(arena) };
    let transform = if arena.paintable_row_is_populated(slot) {
        crate::painting::paintable_geometry::committed_svg_viewport_transform(arena, slot)
    } else {
        None
    };
    FfiOptionalAffineTransform {
        has_value: transform.is_some(),
        transform: transform.unwrap_or_default(),
    }
}

/// # Safety
///
/// `arena` must be a live handle from `render_state_arena_for_unconverted_entry`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_paintable_transform_reference_box(
    arena: *mut c_void,
    slot: NodeSlotId,
) -> FfiCssPixelRect {
    let arena = unsafe { arena_from_handle(arena) };
    if !arena.paintable_row_is_populated(slot) {
        return FfiCssPixelRect::default();
    }
    let Some(style) = arena.node_style_if_live(slot) else {
        return FfiCssPixelRect::default();
    };
    let paintable_rows = arena.paintable_rows();
    crate::painting::visual_context::node_values::transform_reference_box(style, &paintable_rows, slot).into()
}

/// # Safety
///
/// # Safety
///
/// `arena` must be a live handle from `render_state_arena_for_unconverted_entry`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_paintable_box_model(arena: *mut c_void, slot: NodeSlotId) -> FfiBoxModelMetrics {
    let arena = unsafe { arena_from_handle(arena) };
    if !arena.paintable_row_is_populated(slot) {
        return FfiBoxModelMetrics::default();
    }
    FfiBoxModelMetrics {
        margin: crate::painting::paintable_geometry::committed_margin(arena, slot),
        padding: crate::painting::paintable_geometry::committed_padding(arena, slot),
        border: crate::painting::paintable_geometry::committed_border(arena, slot),
        inset: crate::painting::paintable_geometry::committed_inset(arena, slot),
    }
}

/// # Safety
///
/// `arena` must be a live handle from `render_state_arena_for_unconverted_entry`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_paintable_absolute_rect(arena: *mut c_void, slot: NodeSlotId) -> FfiCssPixelRect {
    let arena = unsafe { arena_from_handle(arena) };
    crate::painting::paintable_geometry::absolute_rect_or_default(&arena.paintable_rows(), slot).into()
}

/// # Safety
///
/// `arena` must be a live handle from `render_state_arena_for_unconverted_entry`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_paintable_absolute_padding_box_rect(
    arena: *mut c_void,
    slot: NodeSlotId,
) -> FfiCssPixelRect {
    let arena = unsafe { arena_from_handle(arena) };
    let paintable_rows = arena.paintable_rows();
    if !paintable_rows.paintable_row_is_populated(slot) {
        return FfiCssPixelRect::default();
    }
    crate::painting::paintable_geometry::absolute_padding_box_rect(&paintable_rows, slot).into()
}

/// # Safety
///
/// `arena` must be a live handle from `render_state_arena_for_unconverted_entry`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_paintable_absolute_border_box_rect(
    arena: *mut c_void,
    slot: NodeSlotId,
) -> FfiCssPixelRect {
    let arena = unsafe { arena_from_handle(arena) };
    let paintable_rows = arena.paintable_rows();
    if !paintable_rows.paintable_row_is_populated(slot) {
        return FfiCssPixelRect::default();
    }
    crate::painting::paintable_geometry::absolute_border_box_rect(&paintable_rows, slot).into()
}

/// # Safety
///
/// `arena` must be a live handle from `render_state_arena_for_unconverted_entry`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_physical_overflow_directions(
    arena: *mut c_void,
    paintable: NodeSlotId,
) -> FfiPhysicalOverflowDirections {
    let arena = unsafe { arena_from_handle(arena) };
    let directions = if arena.paintable_row_is_populated(paintable) {
        crate::painting::scrollable_overflow::physical_overflow_directions(arena, paintable)
    } else {
        crate::painting::scrollable_overflow::PhysicalOverflowDirections::default()
    };
    FfiPhysicalOverflowDirections {
        horizontal_axis_is_positive: directions.horizontal_axis_is_positive,
        vertical_axis_is_positive: directions.vertical_axis_is_positive,
    }
}

/// # Safety
///
/// `arena` must be a live handle from `render_state_arena_for_unconverted_entry`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_visual_context_request_full_rebuild(
    arena: *mut c_void,
    reason: crate::painting::host::FfiVisualContextGlobalRebuildReason,
) {
    use crate::painting::host::FfiVisualContextGlobalRebuildReason;
    use crate::painting::visual_context::dirty::VisualContextGlobalRebuildReason;
    let arena = unsafe { arena_from_handle(arena) };
    let reason = match reason {
        FfiVisualContextGlobalRebuildReason::FirstBuild => VisualContextGlobalRebuildReason::FirstBuild,
        FfiVisualContextGlobalRebuildReason::DocumentWideStructuralChange => {
            VisualContextGlobalRebuildReason::DocumentWideStructuralChange
        }
        FfiVisualContextGlobalRebuildReason::FilterResourcesChanged => {
            VisualContextGlobalRebuildReason::FilterResourcesChanged
        }
        FfiVisualContextGlobalRebuildReason::ForcedForTesting => VisualContextGlobalRebuildReason::ForcedForTesting,
        FfiVisualContextGlobalRebuildReason::CanonicalDumpRequested => {
            VisualContextGlobalRebuildReason::CanonicalDumpRequested
        }
    };
    arena.request_full_visual_context_rebuild(reason);
}

/// # Safety
///
/// `arena` must be a live handle from `render_state_arena_for_unconverted_entry`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_visual_context_pending_dirty_box_count(arena: *mut c_void) -> usize {
    let arena = unsafe { arena_from_handle(arena) };
    let paint_state = arena.paint_state().borrow();
    paint_state.visual_context.dirty_boxes.boxes.len()
}

/// # Safety
///
/// `arena` must be a live layout arena handle used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_background_color_can_be_compositor_animated(
    arena: *mut c_void,
    slot: NodeSlotId,
) -> bool {
    let arena = unsafe { arena_from_handle(arena) };
    crate::painting::record::paint::background_resolution::background_color_can_be_compositor_animated(
        &arena.paintable_rows(),
        slot,
        crate::layout::viewport_propagation::root_background_source(arena),
    )
}

/// # Safety
///
/// `arena` must be a live handle from `render_state_arena_for_unconverted_entry`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_paintable_visual_context_node_count(
    arena: *mut c_void,
    slot: NodeSlotId,
    list: crate::painting::host::FfiVisualContextBoxNodeList,
) -> usize {
    use crate::painting::host::FfiVisualContextBoxNodeList;
    let arena = unsafe { arena_from_handle(arena) };
    arena.with_paintable_visual_context_node_handles(slot, |handles| match list {
        FfiVisualContextBoxNodeList::SpatialNodes => handles.spatial.len(),
        FfiVisualContextBoxNodeList::ClipNodes => handles.clip_handles().count(),
        FfiVisualContextBoxNodeList::EffectNodes => handles.effects.len(),
    })
}

/// # Safety
///
/// `arena` must be a live handle from `render_state_arena_for_unconverted_entry`, used on the document thread; `out`
/// must have room for `capacity` indices.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_paintable_visual_context_copy_node_indices(
    arena: *mut c_void,
    slot: NodeSlotId,
    list: crate::painting::host::FfiVisualContextBoxNodeList,
    out: *mut u32,
    capacity: usize,
) {
    use crate::painting::host::FfiVisualContextBoxNodeList;
    let arena = unsafe { arena_from_handle(arena) };
    arena.with_paintable_visual_context_node_handles(slot, |handles| {
        let indices: Vec<u32> = match list {
            FfiVisualContextBoxNodeList::SpatialNodes => handles.spatial.iter().map(|index| index.0).collect(),
            FfiVisualContextBoxNodeList::ClipNodes => handles.clip_handles().map(|index| index.0).collect(),
            FfiVisualContextBoxNodeList::EffectNodes => handles.effects.iter().map(|index| index.0).collect(),
        };
        assert!(indices.len() <= capacity);
        // SAFETY: the caller warrants `capacity` writable indices behind `out`.
        unsafe { std::ptr::copy_nonoverlapping(indices.as_ptr(), out, indices.len()) };
    });
}

fn apply_walk_assignments(
    arena: &mut crate::layout::LayoutNodeArena,
    viewport: NodeSlotId,
    outcome: &mut crate::painting::visual_context::incremental::IncrementalUpdateOutcome,
    state: &mut crate::painting::visual_context::VisualContextState,
) {
    {
        let mut paintable_rows = arena.paintable_rows_mut();
        for assignment in std::mem::take(&mut outcome.assignments) {
            assignment.apply(&mut paintable_rows);
        }
    }
    if outcome.mask_node_owners_changed {
        state.paintables_with_mask_nodes = paintables_with_mask_nodes_in_paint_order(arena, viewport);
    }
}

fn fresh_visual_context_tree_build(
    arena: *mut c_void,
    viewport: NodeSlotId,
    inputs: crate::painting::host::FfiVisualContextTreeInputs,
    state: &mut crate::painting::visual_context::VisualContextState,
) -> crate::painting::host::FfiVisualContextUpdateOutcome {
    use crate::painting::visual_context::dirty::VisualContextUpdateScope;
    use crate::painting::visual_context::incremental::{
        IncrementalUpdateResult, debug_assert_every_live_node_is_owned, update_visual_context_tree,
    };
    let fresh_tree = {
        let arena = unsafe { arena_from_handle(arena) };
        let paintable_rows = arena.paintable_rows();
        let mut fresh_tree = crate::painting::visual_context::build::create_fresh_tree_with_viewport_nodes(
            &paintable_rows,
            viewport,
            &inputs,
        );
        fresh_tree.viewport_assignment.node_identity = arena.unique_node_ids().id(viewport);
        fresh_tree
    };
    {
        let arena = unsafe { arena_from_handle_mut(arena) };
        let mut paintable_rows = arena.paintable_rows_mut();
        paintable_rows.drop_all_visual_context_records();
        fresh_tree.viewport_assignment.apply(&mut paintable_rows);
    }
    state.tree = Some(std::sync::Arc::new(fresh_tree.tree));
    state.dirty_boxes.clear();
    state.build_count += 1;
    let mut outcome = {
        let arena = unsafe { arena_from_handle(arena) };
        let paintable_rows = arena.paintable_rows();
        match update_visual_context_tree(
            &paintable_rows,
            viewport,
            inputs,
            VisualContextUpdateScope::FreshTree,
            state,
        ) {
            IncrementalUpdateResult::Applied(outcome) => *outcome,
            IncrementalUpdateResult::NeedsFullBuild(_) => {
                unreachable!("a fresh tree walk has a tree and a viewport record")
            }
        }
    };
    let arena = unsafe { arena_from_handle_mut(arena) };
    outcome.mask_node_owners_changed = true;
    // Everything records again; pushing that first keeps the per-row pushes below free.
    arena.push_all_paint_damage();
    apply_walk_assignments(arena, viewport, &mut outcome, state);
    arena.rebuild_all_stacking_context_entries_from_records(viewport);
    arena.take_line_roots_needing_fragment_ownership();
    crate::painting::fragment_ownership::assign_fragment_ownership(&arena.paintable_rows(), viewport);
    state.quarantined_slots_are_releasable = false;
    debug_assert_every_live_node_is_owned(
        &arena.paintable_rows(),
        state.tree.as_deref().expect("a fresh tree walk keeps the tree"),
        viewport,
    );
    crate::painting::host::FfiVisualContextUpdateOutcome {
        performed_full_build: true,
        structural_epoch_changed: true,
        requires_display_list_recording: true,
        structural_epoch: state.structural_epoch(),
    }
}

fn paintables_with_mask_nodes_in_paint_order(
    arena: &crate::layout::LayoutNodeArena,
    viewport: NodeSlotId,
) -> Vec<NodeSlotId> {
    let paintable_rows = arena.paintable_rows();
    let mut owners = Vec::new();
    crate::painting::paint_order::for_each_in_paint_subtree(&paintable_rows, viewport, |slot| {
        if arena
            .paintable_visual_context_record(slot)
            .is_some_and(|record| record.has_mask_nodes)
        {
            owners.push(slot);
        }
    });
    owners
}

/// The index of the sticky node the accumulated visual context tree holds for `paintable`, which
/// is where the scroll state snapshot keeps its resolved sticky offset, or `u32::MAX` when the tree
/// holds none.
///
/// # Safety
///
/// `arena` must be a live handle from `render_state_arena_for_unconverted_entry`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_sticky_spatial_node_index(arena: *mut c_void, paintable: NodeSlotId) -> u32 {
    let arena = unsafe { arena_from_handle(arena) };
    let paint_state = arena.paint_state().borrow();
    paint_state
        .visual_context
        .scroll_state
        .states()
        .iter()
        .find(|state| state.is_sticky && state.paintable == paintable)
        .map_or(u32::MAX, |state| state.node_index.0)
}

/// What the host reads of a published recording, to build its display list from.
#[repr(C)]
pub struct FfiPresentedRecording {
    pub is_identical_to_published_recording: bool,
    pub has_blocking_wheel_event_listeners: bool,
    /// The recorded display list, which the document's last recording holds until the next
    /// recording replaces it. The host takes a reference of its own to keep it longer.
    pub display_list: *const c_void,
}

impl FfiPresentedRecording {
    /// What the host reads of the last recording the document took in.
    pub(crate) fn of_last_recording(arena: &LayoutNodeArena) -> Self {
        let paint_state = arena.paint_state().borrow();
        let recording = paint_state.last_recording.as_deref();
        Self {
            is_identical_to_published_recording: recording
                .is_some_and(|recording| recording.is_identical_to_published_recording),
            has_blocking_wheel_event_listeners: recording
                .is_some_and(|recording| recording.has_blocking_wheel_event_listeners),
            display_list: recording.map_or(std::ptr::null(), |recording| {
                std::sync::Arc::as_ptr(&recording.display_list).cast()
            }),
        }
    }
}

/// One `<area>` of an image map, as the document hands it over: the style-tree identity to name as
/// the hit target, the state of its `shape` attribute, and where its parsed `coords` sit in the
/// flat array published beside it.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct FfiImageMapArea {
    pub style_node: u32,
    pub shape: u8,
    pub coords_offset: u32,
    pub coords_count: u32,
}

/// # Safety
///
/// `sink` must be the pointer handed to the callback, used synchronously.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_paint_push_color_stop(
    sink: *mut c_void,
    color: libgfx_rust::Color,
    position: f32,
) {
    let sink = unsafe { &mut *sink.cast::<crate::painting::svg_paint_resources::PublishedSvgPaintServer>() };
    if let crate::painting::svg_paint_resources::PublishedSvgPaintServer::Gradient(gradient) = sink {
        gradient
            .stops
            .push(crate::painting::svg_paint_resources::PublishedSvgGradientStop { color, position });
    }
}

/// # Safety
///
/// `sink` must be the pointer handed to the callback, used synchronously, and `description`
/// must be readable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_svg_paint_resources_push_gradient(
    sink: *mut c_void,
    description: *const crate::painting::host::FfiSvgGradientDescription,
) {
    let sink = unsafe { &mut *sink.cast::<crate::painting::svg_paint_resources::PublishedSvgPaintServer>() };
    *sink = crate::painting::svg_paint_resources::PublishedSvgPaintServer::Gradient(
        crate::painting::svg_paint_resources::PublishedSvgGradient {
            description: unsafe { *description },
            stops: Vec::new(),
        },
    );
}

/// # Safety
///
/// `sink` must be the pointer handed to the callback, used synchronously, `description` must be
/// readable, and `css_transform_entries` must point at `css_transform_count` readable
/// `ComputedResolvedTransform` values of a live computed style.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_svg_paint_resources_push_pattern(
    sink: *mut c_void,
    description: *const crate::painting::host::FfiSvgPatternDescription,
    css_transform_entries: *const c_void,
    css_transform_count: usize,
) {
    let sink = unsafe { &mut *sink.cast::<crate::painting::svg_paint_resources::PublishedSvgPaintServer>() };
    *sink = crate::painting::svg_paint_resources::PublishedSvgPaintServer::Pattern(
        crate::painting::svg_paint_resources::PublishedSvgPattern {
            description: unsafe { *description },
            css_transform: unsafe {
                ffi_slice(
                    css_transform_entries.cast::<crate::css::computed_value_types::ComputedResolvedTransform>(),
                    css_transform_count,
                )
            }
            .to_vec(),
        },
    );
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum FfiImagePaintRecordKind {
    DecodedFrame,
    NestedDisplayList,
    Gradient,
}

#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiImagePaintRecordInputs {
    pub kind: FfiImagePaintRecordKind,
    pub device_pixels_per_css_pixel: f64,
    pub dest_rect: libgfx_rust::FloatRect,
    pub frame_id: u64,
    pub scaling_mode: libgfx_rust::ScalingMode,
    pub nested_display_list_id: u64,
    pub nested_display_list_size: libgfx_rust::IntSize,
    pub gradient_style_value: *const c_void,
    pub gradient_tile_size: FfiCssPixelSize,
    /// The `FfiColorResolutionInput` the gradient's color stops resolve against.
    pub gradient_stop_color_resolution_input: *const c_void,
}

/// # Safety
///
/// `inputs`, and the gradient style value and color resolution input it points at, must be
/// live for the call. `consume` is called synchronously and takes ownership of the command
/// storage and visual context tree handles.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ladybird_web_record_image_paint_display_list(
    inputs: *const FfiImagePaintRecordInputs,
    context: *mut c_void,
    consume: unsafe extern "C" fn(*mut c_void, *const c_void, *const c_void),
) {
    use crate::css::color_resolution::{
        FfiColorResolutionInput, relative_color_context_from_ffi, resolution_input_from_ffi,
    };
    use crate::painting::display_list::commands::{DisplayListResourceId, ImageFrameResourceId};
    use crate::painting::display_list::device_pixels::DevicePixelConverter;
    use crate::painting::display_list::recorder::DisplayListRecorder;
    use crate::painting::record::paint::gradient_resolution::{
        record_resolved_gradient_fill, resolve_gradient_paint_with_input,
    };
    use crate::painting::visual_context::{TransformData, TransformDataRole, VisualContextTree};
    use libgfx_rust::{CompositingAndBlendingOperator, FloatMatrix4x4, FloatPoint};
    let inputs = unsafe { &*inputs };
    let tree = VisualContextTree::create(TransformData {
        matrix: FloatMatrix4x4::identity(),
        origin: FloatPoint::default(),
        sorting_context_root_index: None,
        flattens_inherited_transform: false,
        role: TransformDataRole::CssTransform,
        synthetic_plane: false,
        establishes_sorting_context: false,
    });
    // Force-dark never reaches here: an image is darkened by classifying the image itself — not by inverting the
    // fills that raster it.
    let mut recorder = DisplayListRecorder::new(None);
    let dest_rect = inputs.dest_rect;
    match inputs.kind {
        FfiImagePaintRecordKind::DecodedFrame => recorder.draw_scaled_decoded_image_frame(
            dest_rect,
            None,
            ImageFrameResourceId(inputs.frame_id),
            inputs.scaling_mode,
            CompositingAndBlendingOperator::Normal,
            None,
            ForceDarkRole::None,
        ),
        FfiImagePaintRecordKind::NestedDisplayList => recorder.paint_nested_display_list(
            DisplayListResourceId(inputs.nested_display_list_id),
            dest_rect,
            inputs.nested_display_list_size,
        ),
        FfiImagePaintRecordKind::Gradient => {
            // SAFETY: the host keeps the gradient style value and its color resolution input
            // live for the duration of the call.
            let (gradient_style_value, color_resolution_input) = unsafe {
                (
                    &*inputs
                        .gradient_style_value
                        .cast::<crate::css::style_value::StyleValueData>(),
                    &*inputs
                        .gradient_stop_color_resolution_input
                        .cast::<FfiColorResolutionInput>(),
                )
            };
            let relative_color_channels = relative_color_context_from_ffi(color_resolution_input);
            // SAFETY: the borrowed input outlives the resolution below.
            let color_input = unsafe { resolution_input_from_ffi(color_resolution_input, &relative_color_channels) };
            let resolved =
                resolve_gradient_paint_with_input(gradient_style_value, inputs.gradient_tile_size.into(), &color_input);
            record_resolved_gradient_fill(
                &mut recorder,
                DevicePixelConverter::new(inputs.device_pixels_per_css_pixel),
                &resolved,
                dest_rect,
                CompositingAndBlendingOperator::Normal,
                ForceDarkRole::None,
            );
        }
    }
    let recorded = recorder.into_builder().finish();
    unsafe {
        consume(
            context,
            std::sync::Arc::into_raw(std::sync::Arc::new(recorded)).cast(),
            std::sync::Arc::into_raw(std::sync::Arc::new(tree)).cast(),
        );
    }
}

/// # Safety
///
/// `arena` must be a live handle from `render_state_arena_for_unconverted_entry`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_paintable_computed_svg_path(
    arena: *mut c_void,
    paintable: NodeSlotId,
) -> *const c_void {
    let arena = unsafe { arena_from_handle(arena) };
    let paintable_rows = arena.paintable_rows();
    if !paintable_rows.paintable_row_is_populated(paintable) {
        return std::ptr::null();
    }
    crate::painting::paintable_geometry::committed_svg_path(&paintable_rows, paintable)
        .map_or(std::ptr::null(), |path| path.as_raw())
}

#[repr(C)]
pub struct FfiCaretRectResult {
    pub found: bool,
    pub rect: FfiCssPixelRect,
    pub style_source: NodeSlotId,
    pub owner_paintable: NodeSlotId,
    pub nearest_self_painting_inline: NodeSlotId,
}

/// # Safety
///
/// `arena` must be a live handle from `render_state_arena_for_unconverted_entry`, used on the document
/// thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_text_caret_rect_in_dom_range(
    arena: *mut c_void,
    primary: NodeSlotId,
    offset: usize,
) -> FfiOptionalCssPixelRect {
    let arena = unsafe { arena_from_handle(arena) };
    let paintable_rows = arena.paintable_rows();
    let fragments = arena.text_fragments(primary);
    let node_slots = fragments.as_slice();
    match crate::painting::caret::caret_rect_in_dom_range(&paintable_rows, node_slots, offset) {
        Some(rect) => FfiOptionalCssPixelRect {
            has_value: true,
            rect: rect.into(),
        },
        None => FfiOptionalCssPixelRect {
            has_value: false,
            rect: FfiCssPixelRect::default(),
        },
    }
}

#[repr(C)]
pub struct FfiEmptyLineCaretRect {
    pub has_value: bool,
    pub rect: FfiCssPixelRect,
    pub style_source: NodeSlotId,
}

#[repr(C)]
pub struct FfiRectToViewportTransform {
    pub visual_context_tree: *const c_void,
    pub scroll_offsets: *const libgfx_rust::FloatPoint,
    pub scroll_offsets_len: usize,
    pub device_pixels_per_css_pixel: f32,
}

/// SAFETY: A non-null `visual_context_tree` must be a live retained tree handle and `scroll_offsets`
/// must address `scroll_offsets_len` points for as long as the returned borrow lives.
unsafe fn rect_to_viewport_transform_from_ffi(
    transform: &FfiRectToViewportTransform,
) -> Option<RectToViewportTransform<'_>> {
    if transform.visual_context_tree.is_null() {
        return None;
    }
    Some(RectToViewportTransform {
        visual_context_tree: unsafe { tree_from_handle(transform.visual_context_tree) },
        scroll_offsets: unsafe { ffi_slice(transform.scroll_offsets, transform.scroll_offsets_len) },
        device_pixels_per_css_pixel: transform.device_pixels_per_css_pixel,
    })
}

/// # Safety
///
/// `arena` must be a live handle from `render_state_arena_for_unconverted_entry`, used on the document thread, and
/// `rect_to_viewport_transform` must satisfy `rect_to_viewport_transform_from_ffi`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_client_rects(
    arena: *mut c_void,
    layout_node: NodeSlotId,
    rect_to_viewport_transform: FfiRectToViewportTransform,
    context: *mut c_void,
    push_rect: unsafe extern "C" fn(*mut c_void, FfiCssPixelRect),
) {
    let arena = unsafe { arena_from_handle(arena) };
    let rect_to_viewport_transform = unsafe { rect_to_viewport_transform_from_ffi(&rect_to_viewport_transform) };
    crate::painting::client_rects::for_each_client_rect(
        &arena.paintable_rows(),
        layout_node,
        rect_to_viewport_transform.as_ref(),
        |rect| {
            // SAFETY: The consumer copies the plain-data rect synchronously.
            unsafe { push_rect(context, rect.into()) };
        },
    );
}

/// # Safety
///
/// `arena` must be a live handle from `render_state_arena_for_unconverted_entry`, used on the document thread, and
/// `rect_to_viewport_transform` must satisfy `rect_to_viewport_transform_from_ffi`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_bounding_client_rect(
    arena: *mut c_void,
    layout_node: NodeSlotId,
    rect_to_viewport_transform: FfiRectToViewportTransform,
) -> FfiCssPixelRect {
    let arena = unsafe { arena_from_handle(arena) };
    let rect_to_viewport_transform = unsafe { rect_to_viewport_transform_from_ffi(&rect_to_viewport_transform) };
    crate::painting::client_rects::bounding_client_rect(
        &arena.paintable_rows(),
        layout_node,
        rect_to_viewport_transform.as_ref(),
    )
    .into()
}

/// # Safety
///
/// `arena` must be a live handle from `render_state_arena_for_unconverted_entry`, used on the document thread, and
/// `rect_to_viewport_transform` must satisfy `rect_to_viewport_transform_from_ffi`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_transform_subtree_is_clipped_outside(
    arena: *mut c_void,
    target: NodeSlotId,
    root_bounds: FfiCssPixelRect,
    rect_to_viewport_transform: FfiRectToViewportTransform,
) -> bool {
    let arena = unsafe { arena_from_handle(arena) };
    let rect_to_viewport_transform = unsafe { rect_to_viewport_transform_from_ffi(&rect_to_viewport_transform) };
    crate::painting::intersection_observer::transform_subtree_is_clipped_outside(
        &arena.paintable_rows(),
        target,
        root_bounds.into(),
        rect_to_viewport_transform.as_ref(),
    )
}

/// # Safety
///
/// `arena` must be a live handle from `render_state_arena_for_unconverted_entry`, used on the document thread, and
/// `rect_to_viewport_transform` must satisfy `rect_to_viewport_transform_from_ffi`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_intersection_observer_intersection_rect(
    arena: *mut c_void,
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
    let arena = unsafe { arena_from_handle(arena) };
    let rect_to_viewport_transform = unsafe { rect_to_viewport_transform_from_ffi(&rect_to_viewport_transform) };
    crate::painting::intersection_observer::intersection_rect(
        &arena.paintable_rows(),
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
}

/// Whether a row has ever been given a style with `content-visibility: auto`, which is when a
/// layout commit collects the boxes with it.
///
/// # Safety
///
/// `arena` must be a live handle from `render_state_arena_for_unconverted_entry`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_may_have_auto_content_visibility(arena: *mut c_void) -> bool {
    unsafe { arena_from_handle(arena) }.may_have_auto_content_visibility()
}

/// # Safety
///
/// `arena` must be a live handle from `render_state_arena_for_unconverted_entry`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_collect_boxes_with_auto_content_visibility(
    arena: *mut c_void,
    root: NodeSlotId,
    context: *mut c_void,
    push_box: unsafe extern "C" fn(*mut c_void, NodeSlotId),
) {
    let arena = unsafe { arena_from_handle(arena) };
    crate::painting::content_visibility::for_each_box_with_auto_content_visibility(
        &arena.paintable_rows(),
        root,
        |slot| {
            // SAFETY: The C++ callback appends the slot id to a caller-owned collection.
            unsafe { push_box(context, slot) };
        },
    );
}

/// # Safety
///
/// `arena` must be a live handle from `render_state_arena_for_unconverted_entry`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_can_compute_client_rects_without_visual_context_update(
    arena: *mut c_void,
    layout_node: NodeSlotId,
    viewport_scroll_offset_is_zero: bool,
) -> bool {
    let arena = unsafe { arena_from_handle(arena) };
    crate::painting::client_rects::can_compute_client_rects_without_visual_context_update(
        &arena.paintable_rows(),
        layout_node,
        viewport_scroll_offset_is_zero,
    )
}

/// # Safety
///
/// `arena` must be a live handle from `render_state_arena_for_unconverted_entry`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_inline_paintable_has_content_pieces(
    arena: *mut c_void,
    inline_paintable: NodeSlotId,
) -> bool {
    let arena = unsafe { arena_from_handle(arena) };
    let mut has_content = false;
    with_inline_pieces(&arena.paintable_rows(), inline_paintable, |piece, _| {
        if !piece.is_geometry_only_placeholder {
            has_content = true;
            return false;
        }
        true
    });
    has_content
}

#[repr(C)]
pub struct FfiOptionalCssPixelPoint {
    pub has_value: bool,
    pub x: CssPixels,
    pub y: CssPixels,
}

/// # Safety
///
/// `arena` must be a live handle from `render_state_arena_for_unconverted_entry`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_inline_paintable_first_piece_position(
    arena: *mut c_void,
    inline_paintable: NodeSlotId,
) -> FfiOptionalCssPixelPoint {
    let mut result = FfiOptionalCssPixelPoint {
        has_value: false,
        x: CssPixels::from_raw(0),
        y: CssPixels::from_raw(0),
    };
    let arena = unsafe { arena_from_handle(arena) };
    let paintable_rows = arena.paintable_rows();
    let Some(root) = paintable_rows.inline_pieces_root(inline_paintable) else {
        return result;
    };
    let root_position = crate::painting::paintable_geometry::absolute_position(&paintable_rows, root);
    let border_widths = crate::painting::paintable_geometry::committed_border(arena, inline_paintable);
    let padding_widths = crate::painting::paintable_geometry::committed_padding(arena, inline_paintable);
    with_inline_pieces(&paintable_rows, inline_paintable, |piece, _data| {
        let border_rect = CssPixelRect::from(piece.border_box_rect);
        let rect = if piece.is_geometry_only_placeholder {
            border_rect
        } else {
            let padding_rect = piece.shrunken_by_present_edges(border_rect, border_widths);
            piece.shrunken_by_present_edges(padding_rect, padding_widths)
        };
        result.has_value = true;
        result.x = rect.x + root_position.x;
        result.y = rect.y + root_position.y;
        false
    });
    result
}

#[repr(C)]
pub struct FfiVisualLine {
    pub start_offset: usize,
    pub end_offset: usize,
    pub end_offset_with_trailing_whitespace: usize,
    pub has_fragments: bool,
    pub owner_paintable: u32,
    pub line_index: u32,
}

/// # Safety
///
/// `arena` must be a live handle from `render_state_arena_for_unconverted_entry`, used on the document
/// thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_text_visual_lines(
    arena: *mut c_void,
    primary: NodeSlotId,
    context: *mut c_void,
    push: unsafe extern "C" fn(*mut c_void, FfiVisualLine),
) {
    let arena = unsafe { arena_from_handle(arena) };
    let paintable_rows = arena.paintable_rows();
    let fragments = arena.text_fragments(primary);
    let node_slots = fragments.as_slice();
    for line in crate::painting::visual_lines::collect_visual_lines(&paintable_rows, node_slots) {
        // SAFETY: The consumer copies the POD line synchronously.
        unsafe {
            push(
                context,
                FfiVisualLine {
                    start_offset: line.start_offset,
                    end_offset: line.end_offset,
                    end_offset_with_trailing_whitespace: line.end_offset_with_trailing_whitespace,
                    has_fragments: line.has_fragments,
                    owner_paintable: line.owner.index,
                    line_index: line.line_index,
                },
            );
        }
    }
}

fn has_rendered_text_matching(
    arena: &impl PaintableRowsRead,
    node_slots: &[NodeSlotId],
    matches: impl Fn(&FragmentRecord) -> bool,
) -> bool {
    let mut found = false;
    crate::painting::text_fragment::for_each_fragment_of_nodes(arena, node_slots, |_, _, fragment| {
        if fragment.length_in_code_units > 0 && matches(fragment) {
            found = true;
            return false;
        }
        true
    });
    found
}

/// # Safety
///
/// `arena` must be a live handle from `render_state_arena_for_unconverted_entry`, used on the document
/// thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_text_has_rendered_text_before(
    arena: *mut c_void,
    primary: NodeSlotId,
    offset: usize,
) -> bool {
    let arena = unsafe { arena_from_handle(arena) };
    let fragments = arena.text_fragments(primary);
    let node_slots = fragments.as_slice();
    has_rendered_text_matching(&arena.paintable_rows(), node_slots, |fragment| {
        fragment.dom_start_offset_in_node < offset
    })
}

/// # Safety
///
/// `arena` must be a live handle from `render_state_arena_for_unconverted_entry`, used on the document
/// thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_text_has_rendered_text_after(
    arena: *mut c_void,
    primary: NodeSlotId,
    offset: usize,
) -> bool {
    let arena = unsafe { arena_from_handle(arena) };
    let fragments = arena.text_fragments(primary);
    let node_slots = fragments.as_slice();
    has_rendered_text_matching(&arena.paintable_rows(), node_slots, |fragment| {
        fragment.dom_end_offset_in_node > offset
    })
}

#[repr(C)]
pub struct FfiOptionalCssPixels {
    pub has_value: bool,
    pub value: CssPixels,
}

/// # Safety
///
/// `arena` must be a live handle from `render_state_arena_for_unconverted_entry`, used on the document
/// thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_visual_line_caret_inline_coordinate(
    arena: *mut c_void,
    owner_paintable: u32,
    line_index: u32,
    primary: NodeSlotId,
    offset: usize,
) -> FfiOptionalCssPixels {
    let arena = unsafe { arena_from_handle(arena) };
    let paintable_rows = arena.paintable_rows();
    let fragments = arena.text_fragments(primary);
    let node_slots = fragments.as_slice();
    let coordinate = crate::painting::visual_lines::caret_inline_coordinate(
        &paintable_rows,
        owner_paintable,
        line_index,
        node_slots,
        offset,
    );
    match coordinate {
        Some(value) => FfiOptionalCssPixels { has_value: true, value },
        None => FfiOptionalCssPixels {
            has_value: false,
            value: CssPixels::from_raw(0),
        },
    }
}

/// # Safety
///
/// `arena` must be a live handle from `render_state_arena_for_unconverted_entry`, used on the document
/// thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_visual_line_offset_closest_to_inline_coordinate(
    arena: *mut c_void,
    owner_paintable: u32,
    line_index: u32,
    primary: NodeSlotId,
    inline_coordinate: CssPixels,
    fallback_offset: usize,
) -> usize {
    let arena = unsafe { arena_from_handle(arena) };
    let paintable_rows = arena.paintable_rows();
    let fragments = arena.text_fragments(primary);
    let node_slots = fragments.as_slice();
    crate::painting::visual_lines::offset_closest_to_inline_coordinate(
        &paintable_rows,
        owner_paintable,
        line_index,
        node_slots,
        inline_coordinate,
    )
    .unwrap_or(fallback_offset)
}

/// # Safety
///
/// `arena` must be a live handle from `render_state_arena_for_unconverted_entry`, used on the document
/// thread, and
/// `rect_to_viewport_transform` must satisfy `rect_to_viewport_transform_from_ffi`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_text_range_rects(
    arena: *mut c_void,
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
    let arena = unsafe { arena_from_handle(arena) };
    let paintable_rows = arena.paintable_rows();
    let rect_to_viewport_transform = unsafe { rect_to_viewport_transform_from_ffi(&rect_to_viewport_transform) };

    let fragments = arena.text_fragments(primary);
    let node_slots = fragments.as_slice();
    crate::painting::text_fragment::for_each_fragment_of_nodes(&paintable_rows, node_slots, |block, _, fragment| {
        let fragment_dom_start = fragment.dom_start_offset_in_node;
        let fragment_dom_end = fragment.dom_end_offset_in_node;
        if fragment_dom_end <= filter_dom_start || fragment_dom_start >= filter_dom_end {
            return true;
        }

        let rect = crate::painting::text_fragment::range_rect(
            &paintable_rows,
            fragment,
            selection_state,
            range_start_offset,
            range_end_offset,
        );

        let rect_in_viewport_space = if arena.slot_is_live(block) {
            crate::painting::rect_to_viewport_transform::transform_rect_to_viewport_or_identity(
                rect_to_viewport_transform.as_ref(),
                &paintable_rows,
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
}

#[repr(C)]
pub struct FfiOptionalCssPixelRect {
    pub has_value: bool,
    pub rect: FfiCssPixelRect,
}

/// # Safety
///
/// `arena` must be a live handle from `render_state_arena_for_unconverted_entry`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_paintable_first_fragment_rect_for_node(
    arena: *mut c_void,
    block: NodeSlotId,
    node: NodeSlotId,
) -> FfiOptionalCssPixelRect {
    let mut result = FfiOptionalCssPixelRect {
        has_value: false,
        rect: FfiCssPixelRect::default(),
    };
    let arena = unsafe { arena_from_handle(arena) };
    let paintable_rows = arena.paintable_rows();
    if !paintable_rows.paintable_row_is_populated(block) {
        return result;
    }
    for fragment in arena.committed_side_data(block).fragments() {
        if fragment.layout_node != node {
            continue;
        }
        result.has_value = true;
        result.rect = crate::painting::text_fragment::absolute_rect(&paintable_rows, fragment).into();
        break;
    }
    result
}

/// # Safety
///
/// `arena` must be a live handle from `render_state_arena_for_unconverted_entry`, used on the document thread;
/// `consume` copies the byte span synchronously.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_stacking_context_structure_verification_report(
    arena: *mut c_void,
    viewport: NodeSlotId,
    context: *mut c_void,
    consume: unsafe extern "C" fn(*mut c_void, *const u8, usize),
) {
    let arena = unsafe { arena_from_handle(arena) };
    let report = crate::painting::stacking_context::verify::verification_report(arena, viewport);
    if !report.is_empty() {
        // SAFETY: The consumer copies the byte span synchronously.
        unsafe { consume(context, report.as_ptr(), report.len()) };
    }
}

/// # Safety
///
/// `arena` must be a live handle from `render_state_arena_for_unconverted_entry`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_paintable_grid_layout_json(
    arena: *mut c_void,
    paintable: NodeSlotId,
    container_node_id: i64,
    context: *mut c_void,
    consume: unsafe extern "C" fn(*mut c_void, *const u8, usize),
) {
    let arena = unsafe { arena_from_handle(arena) };
    if !arena.paintable_row_is_populated(paintable) {
        return;
    }
    if let Some(data) = crate::painting::paintable_geometry::committed_grid_layout_data(arena, paintable) {
        let json = crate::painting::devtools_layout::serialize_grid_layout(&data, container_node_id);
        unsafe { consume(context, json.as_ptr(), json.len()) };
    }
}

/// # Safety
///
/// `arena` must be a live handle from `render_state_arena_for_unconverted_entry`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_paintable_flex_layout_json(
    arena: *mut c_void,
    paintable: NodeSlotId,
    container_node_id: i64,
    context: *mut c_void,
    consume: unsafe extern "C" fn(*mut c_void, *const u8, usize),
    document_context: *const c_void,
    resolve_node_id: unsafe extern "C" fn(*const c_void, u32) -> i64,
) {
    let arena = unsafe { arena_from_handle(arena) };
    if !arena.paintable_row_is_populated(paintable) {
        return;
    }
    if let Some(data) = crate::painting::paintable_geometry::committed_flex_layout_data(arena, paintable) {
        // SAFETY: The host answers synchronously from the document the paintable belongs to.
        let json = crate::painting::devtools_layout::serialize_flex_layout(&data, container_node_id, |style_node| {
            let node_id = unsafe { resolve_node_id(document_context, style_node.raw()) };
            (node_id >= 0).then_some(node_id)
        });
        unsafe { consume(context, json.as_ptr(), json.len()) };
    }
}

/// # Safety
///
/// `arena` must be a live handle from `render_state_arena_for_unconverted_entry`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_paintable_used_grid_tracks(
    arena: *mut c_void,
    paintable: NodeSlotId,
    columns: bool,
) -> *const c_void {
    let arena = unsafe { arena_from_handle(arena) };
    if !arena.paintable_row_is_populated(paintable) {
        return std::ptr::null();
    }
    let Some(tracks) = crate::painting::paintable_geometry::committed_used_grid_tracks(arena, paintable) else {
        return std::ptr::null();
    };
    let list = if columns { &tracks.columns } else { &tracks.rows };
    std::sync::Arc::into_raw(std::sync::Arc::new(list.style_value())).cast()
}

/// # Safety
///
/// `arena` must be a live handle from `render_state_arena_for_unconverted_entry`, used on the document thread. The
/// returned tree is retained; the caller owns one reference and releases it with
/// `visual_context_tree_release`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_main_visual_context_tree_retain(arena: *mut c_void) -> *const c_void {
    let arena = unsafe { arena_from_handle(arena) };
    let paint_state = arena.paint_state().borrow();
    paint_state
        .visual_context
        .tree
        .as_ref()
        .map_or(std::ptr::null(), |tree| {
            std::sync::Arc::into_raw(std::sync::Arc::clone(tree)).cast()
        })
}

/// # Safety
///
/// `arena` must be a live handle from `render_state_arena_for_unconverted_entry`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_has_visual_context_tree(arena: *mut c_void) -> bool {
    let arena = unsafe { arena_from_handle(arena) };
    let paint_state = arena.paint_state().borrow();
    paint_state.visual_context.tree.is_some()
}

/// # Safety
///
/// `arena` must be a live handle from `render_state_arena_for_unconverted_entry`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_visual_context_tree_structural_epoch(arena: *mut c_void) -> u64 {
    let arena = unsafe { arena_from_handle(arena) };
    let paint_state = arena.paint_state().borrow();
    paint_state.visual_context.structural_epoch()
}

/// # Safety
///
/// `arena` must be a live handle from `render_state_arena_for_unconverted_entry`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_visual_context_tree_has_visual_animations(arena: *mut c_void) -> bool {
    let arena = unsafe { arena_from_handle(arena) };
    let paint_state = arena.paint_state().borrow();
    paint_state
        .visual_context
        .tree
        .as_deref()
        .is_some_and(|tree| tree.has_visual_animations())
}

/// # Safety
///
/// The returned handle is owned by the caller until `compositor_animation_effect_state_destroy`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn compositor_animation_effect_state_create() -> *mut c_void {
    Box::into_raw(Box::new(
        crate::painting::visual_animation_builder::CompositorAnimationEffectState::default(),
    ))
    .cast()
}

/// # Safety
///
/// `state` must be a handle from `compositor_animation_effect_state_create` that is given up here.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn compositor_animation_effect_state_destroy(state: *mut c_void) {
    if state.is_null() {
        return;
    }
    // SAFETY: The caller gives up the handle it created.
    drop(unsafe {
        Box::from_raw(state.cast::<crate::painting::visual_animation_builder::CompositorAnimationEffectState>())
    });
}

/// Builds the animation of the request's target kind for the effect and keeps it pending with the
/// effect. The target's nodes come from the arena's main tree.
///
/// # Safety
///
/// `state` must be a live effect state handle, `arena` a live handle from `render_state_arena_for_unconverted_entry`
/// used on the document thread, and the request and host, with every range they address, live
/// for the call. The host's callbacks run synchronously and may not touch the arena.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn compositor_animation_effect_build(
    state: *mut c_void,
    arena: *mut c_void,
    request: *const crate::painting::host::FfiCompositorAnimationRequest,
    host: *const crate::painting::host::FfiCompositorAnimationHost,
) -> crate::painting::host::FfiCompositorAnimationBuildOutcome {
    use crate::painting::visual_animation_builder::{Host, Request, effect_state_from_handle};
    let state = unsafe { effect_state_from_handle(state) };
    let arena = unsafe { arena_from_handle(arena) };
    let request = unsafe { Request::new(&*request) };
    let host = Host::new(unsafe { &*host });
    let tree = arena.paint_state().borrow().visual_context.tree.clone();
    state.build(&request, &host, |kind| {
        arena.paintable_visual_animation_target_indices(request.layout_node(), tree.as_deref(), kind)
    })
}

/// # Safety
///
/// `state` must be a live effect state handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn compositor_animation_effect_discard_pending(
    state: *mut c_void,
    kind: crate::painting::host::FfiVisualAnimationTargetKind,
) {
    unsafe { crate::painting::visual_animation_builder::effect_state_from_handle(state) }.discard_pending(kind);
}

/// # Safety
///
/// `state` must be a live effect state handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn compositor_animation_effect_has_pending(state: *mut c_void) -> bool {
    unsafe { crate::painting::visual_animation_builder::effect_state_from_handle(state) }.has_pending()
}

/// # Safety
///
/// `state` must be a live effect state handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn compositor_animation_effect_clear_pending(state: *mut c_void) {
    unsafe { crate::painting::visual_animation_builder::effect_state_from_handle(state) }.clear_pending();
}

/// Publishes the effect's pending animations: they become the ones it retains, and copies join the
/// document's list for the current update pass.
///
/// # Safety
///
/// `state` must be a live effect state handle and `arena` a live handle from `render_state_arena_for_unconverted_entry`
/// used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn compositor_animation_effect_publish_pending(
    state: *mut c_void,
    arena: *mut c_void,
    reuse_retained_timing_anchors: bool,
) {
    let animations = unsafe { crate::painting::visual_animation_builder::effect_state_from_handle(state) }
        .publish_pending(reuse_retained_timing_anchors);
    let arena = unsafe { arena_from_handle(arena) };
    arena
        .paint_state()
        .borrow_mut()
        .visual_context
        .pending_compositor_animations
        .extend(animations);
}

/// # Safety
///
/// `state` must be a live effect state handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn compositor_animation_effect_has_retained(state: *mut c_void) -> bool {
    unsafe { crate::painting::visual_animation_builder::effect_state_from_handle(state) }.has_retained()
}

/// # Safety
///
/// `state` must be a live effect state handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn compositor_animation_effect_clear_retained(state: *mut c_void) {
    unsafe { crate::painting::visual_animation_builder::effect_state_from_handle(state) }.clear_retained();
}

/// # Safety
///
/// `state` must be a live effect state handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn compositor_animation_effect_reset(state: *mut c_void) {
    unsafe { crate::painting::visual_animation_builder::effect_state_from_handle(state) }.reset();
}

/// # Safety
///
/// The request and host, with every range they address, must be live for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn compositor_animation_effect_only_translates_horizontally(
    request: *const crate::painting::host::FfiCompositorAnimationRequest,
    host: *const crate::painting::host::FfiCompositorAnimationHost,
) -> bool {
    use crate::painting::visual_animation_builder::{Host, Request, effect_only_translates_horizontally};
    let request = unsafe { Request::new(&*request) };
    effect_only_translates_horizontally(&request, &Host::new(unsafe { &*host }))
}

/// # Safety
///
/// The request and host, with every range they address, must be live for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn compositor_animation_effect_transform_preserves_axes(
    request: *const crate::painting::host::FfiCompositorAnimationRequest,
    host: *const crate::painting::host::FfiCompositorAnimationHost,
) -> bool {
    use crate::painting::visual_animation_builder::{Host, Request, effect_transform_preserves_axes};
    let request = unsafe { Request::new(&*request) };
    effect_transform_preserves_axes(&request, &Host::new(unsafe { &*host }))
}

/// Starts an update pass: the effects publish into an empty document list.
///
/// # Safety
///
/// `arena` must be a live handle from `render_state_arena_for_unconverted_entry`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_begin_compositor_animation_update(arena: *mut c_void) {
    let arena = unsafe { arena_from_handle(arena) };
    arena
        .paint_state()
        .borrow_mut()
        .visual_context
        .pending_compositor_animations
        .clear();
}

/// Gives the arena's main tree the animations the update pass published, or none.
///
/// # Safety
///
/// `arena` must be a live handle from `render_state_arena_for_unconverted_entry`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_publish_compositor_animations(
    arena: *mut c_void,
    publish_pending: bool,
) -> crate::painting::host::FfiCompositorAnimationPublishOutcome {
    let arena = unsafe { arena_from_handle(arena) };
    let mut paint_state = arena.paint_state().borrow_mut();
    crate::painting::visual_context::publish_compositor_animations(&mut paint_state.visual_context, publish_pending)
}

/// # Safety
///
/// `sink` must be the pointer handed to the callback, used synchronously; `bytes` must point at
/// `length` readable bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_paint_push_bytes(sink: *mut c_void, bytes: *const u8, length: usize) {
    // SAFETY: `sink` is the Vec pointer handed out by the host callback wrapper; the caller
    // guarantees the byte range.
    let vec = unsafe { &mut *sink.cast::<Vec<u8>>() };
    if length > 0 {
        vec.extend_from_slice(unsafe { std::slice::from_raw_parts(bytes, length) });
    }
}

/// # Safety
///
/// Every pointer in `primitive` must be readable for the length that accompanies it, each UTF-16
/// view must satisfy [`FfiUtf16View::units`], and each non-null component transfer table must
/// hold 256 bytes.
unsafe fn svg_filter_primitive_from_ffi(primitive: &FfiSvgFilterPrimitive) -> SvgFilterPrimitive {
    let name = |view: FfiUtf16View| unsafe { view.to_utf16() }.unwrap_or_default();
    SvgFilterPrimitive {
        values: primitive.values,
        in1: name(primitive.in1),
        in2: name(primitive.in2),
        result: name(primitive.result),
        merge_inputs: unsafe { ffi_slice(primitive.merge_inputs, primitive.merge_input_count) }
            .iter()
            .map(|view| name(*view))
            .collect(),
        color_matrix_values: unsafe { ffi_slice(primitive.color_matrix_values, primitive.color_matrix_value_count) }
            .to_vec(),
        component_transfer_tables: primitive.component_transfer_tables.map(|table| {
            (!table.is_null()).then(|| {
                let table = unsafe { ffi_slice(table, 256) };
                Box::new(<[u8; 256]>::try_from(table).expect("a component transfer table holds 256 entries"))
            })
        }),
        image_frame: (!primitive.image_frame.is_null())
            .then(|| unsafe { libgfx_rust::image_frame::ImageFrameHandle::retain(primitive.image_frame) }),
    }
}

/// # Safety
///
/// `sink` must be the pointer handed to the callback, used synchronously; `primitive` must be
/// readable, with every pointer in it readable for the length that accompanies it, each UTF-16
/// view satisfying [`FfiUtf16View::units`], each non-null component transfer table holding
/// 256 bytes, and `image_frame` null or pointing to a live `Gfx::DecodedImageFrame`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_paint_push_svg_filter_primitive(
    sink: *mut c_void,
    primitive: *const FfiSvgFilterPrimitive,
) {
    let primitives = unsafe { &mut *sink.cast::<Vec<SvgFilterPrimitive>>() };
    primitives.push(unsafe { svg_filter_primitive_from_ffi(&*primitive) });
}

/// # Safety
///
/// `arena` must be a live handle from `render_state_arena_for_unconverted_entry`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_note_svg_paint_resources_changed(arena: *mut c_void) -> bool {
    let arena = unsafe { arena_from_handle(arena) };
    arena.svg_paint_resources().note_changed()
}

/// # Safety
///
/// `arena` must be a live handle from `render_state_arena_for_unconverted_entry`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_has_enrolled_svg_paint_resources(arena: *mut c_void) -> bool {
    let arena = unsafe { arena_from_handle(arena) };
    arena.svg_paint_resources().has_enrolled_entries()
}

/// Serializes the graph a list of filter functions describes and hands the bytes to `append`.
/// Returns false for an empty list, which appends nothing.
///
/// # Safety
///
/// `functions` must point to `count` readable functions, and `append` must accept `context` and
/// the byte range it is handed, synchronously.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_filter_functions_serialize(
    functions: *const FfiFilterFunction,
    count: usize,
    append: unsafe extern "C" fn(*mut c_void, *const u8, usize),
    context: *mut c_void,
) -> bool {
    let functions = unsafe { ffi_slice(functions, count) };
    let Some(graph) = filter_functions_graph(functions.iter().copied().map(Filter::from)) else {
        return false;
    };
    let bytes = graph.serialize();
    unsafe { append(context, bytes.as_ptr(), bytes.len()) };
    true
}

/// An SVG paint resource of an enrolled row the host resolves from the DOM.
enum SvgPaintResourceRequest {
    PaintServer {
        slot: NodeSlotId,
        kind: SvgPaintResourceKind,
    },
    /// A filter list, with the `url()` of each filter that names an SVG filter.
    Filter {
        slot: NodeSlotId,
        kind: SvgPaintResourceKind,
        urls: Vec<crate::css::style_value::RetainedStyleValueData>,
    },
}

/// What the host resolved of an [`SvgPaintResourceRequest`].
enum ResolvedSvgPaintResource {
    PaintServer {
        slot: NodeSlotId,
        kind: SvgPaintResourceKind,
        published: PublishedSvgPaintServer,
    },
    Filter {
        slot: NodeSlotId,
        kind: SvgPaintResourceKind,
        published: PublishedSvgFilter,
    },
}

/// What the host resolves of the enrolled SVG paint resources, if they changed since they were
/// resolved last. A row that went away, or whose filters name no SVG filter any more, leaves them.
fn svg_paint_resource_requests(arena: &LayoutNodeArena) -> Option<Vec<SvgPaintResourceRequest>> {
    let resources = arena.svg_paint_resources();
    if !resources.take_needs_sync() {
        return None;
    }
    let mut requests = Vec::new();
    for (slot, kind) in resources.enrolled_entries() {
        let Some(style) = arena.node_style_if_live(slot) else {
            resources.forget_slot(slot);
            continue;
        };
        if matches!(kind, SvgPaintResourceKind::Fill | SvgPaintResourceKind::Stroke) {
            requests.push(SvgPaintResourceRequest::PaintServer { slot, kind });
            continue;
        }
        let effects = style.effects();
        let filter_list = match kind {
            SvgPaintResourceKind::Filter => &effects.filter,
            SvgPaintResourceKind::BackdropFilter => &effects.backdrop_filter,
            SvgPaintResourceKind::Fill | SvgPaintResourceKind::Stroke => unreachable!(),
        };
        if !crate::painting::css_filter::contains_url(filter_list) {
            resources.withdraw(slot, kind);
            continue;
        }
        let urls = filter_list
            .operations
            .as_slice()
            .iter()
            .filter(|operation| operation.kind == crate::painting::css_filter::FILTER_KIND_URL)
            .map(|operation| {
                // SAFETY: The row's style holds the value live; the request holds a reference of its
                // own, as the host may restyle the row while it resolves.
                unsafe {
                    crate::css::style_value::RetainedStyleValueData::from_retained_optional_pointer(
                        crate::css::style_value::retain_style_value(operation.url_value.pointer.cast()),
                    )
                }
            })
            .collect();
        requests.push(SvgPaintResourceRequest::Filter { slot, kind, urls });
    }
    Some(requests)
}

/// Publishes what the host resolved of the enrolled SVG paint resources, and answers whether any
/// changed.
fn publish_resolved_svg_paint_resources(arena: &LayoutNodeArena, resolved: Vec<ResolvedSvgPaintResource>) -> bool {
    use crate::painting::record::damage::PaintDamage;
    let resources = arena.svg_paint_resources();
    let mut any_changed = false;
    for resolved in resolved {
        match resolved {
            ResolvedSvgPaintResource::PaintServer { slot, kind, published } => {
                if arena.slot_is_live(slot) && resources.publish_paint_server(slot, kind, published) {
                    any_changed = true;
                    arena.push_paint_damage(slot, PaintDamage::SVG | PaintDamage::SCOPE_PREAMBLE);
                }
            }
            ResolvedSvgPaintResource::Filter { slot, kind, published } => {
                if !arena.slot_is_live(slot) || !resources.publish_filter(slot, kind, published) {
                    continue;
                }
                any_changed = true;
                if arena.paintable_row_is_populated(slot) {
                    arena.note_visual_context_box_dirty(
                        slot,
                        crate::painting::visual_context::dirty::VisualContextBoxDirtyKind::StyleValueChange,
                    );
                    arena.push_paint_damage(slot, PaintDamage::SVG | PaintDamage::SCOPE_PREAMBLE);
                }
            }
        }
    }
    any_changed
}

/// Resolves the SVG paint resources the enrolled rows name, in two passes over the arena with the
/// host's answers between them, so that no arena borrow is alive while the host reaches the DOM.
///
/// # Safety
///
/// `arena` must be a live handle from `render_state_arena_for_unconverted_entry`, used on the document thread, and
/// both resolvers must answer synchronously for the row of the arena they are handed and only
/// push into the sink whose pointer they receive.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_sync_svg_paint_resources(
    arena: *mut c_void,
    resolve_filter: unsafe extern "C" fn(*mut c_void, NodeSlotId, *const c_void, *mut c_void) -> bool,
    resolve_paint_server: unsafe extern "C" fn(*mut c_void, NodeSlotId, bool, *mut c_void),
) -> bool {
    // SAFETY: Guaranteed by the entry point's contract; the borrow ends with the pass.
    let Some(requests) = svg_paint_resource_requests(unsafe { arena_from_handle(arena) }) else {
        return false;
    };
    let mut resolved = Vec::with_capacity(requests.len());
    for request in requests {
        match request {
            SvgPaintResourceRequest::PaintServer { slot, kind } => {
                let mut published = PublishedSvgPaintServer::None;
                // SAFETY: The host resolves synchronously for the row, and only pushes into the sink
                // it is handed.
                unsafe {
                    resolve_paint_server(
                        arena,
                        slot,
                        kind == SvgPaintResourceKind::Stroke,
                        (&raw mut published).cast(),
                    );
                }
                resolved.push(ResolvedSvgPaintResource::PaintServer { slot, kind, published });
            }
            SvgPaintResourceRequest::Filter { slot, kind, urls } => {
                let mut published = PublishedSvgFilter::default();
                for url in &urls {
                    let mut primitives: Vec<SvgFilterPrimitive> = Vec::new();
                    // SAFETY: The host resolves synchronously for the row, and only pushes into the
                    // primitive list it is handed as its sink.
                    let found =
                        unsafe { resolve_filter(arena, slot, url.pointer().cast(), (&raw mut primitives).cast()) };
                    published = PublishedSvgFilter {
                        failed: !found,
                        primitives: if found { primitives } else { Vec::new() },
                    };
                    if published.failed {
                        break;
                    }
                }
                resolved.push(ResolvedSvgPaintResource::Filter { slot, kind, published });
            }
        }
    }
    // SAFETY: As above.
    publish_resolved_svg_paint_resources(unsafe { arena_from_handle(arena) }, resolved)
}

/// # Safety
/// `arena` is live and used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_set_recording_trace_enabled(arena: *mut c_void, enabled: bool) {
    unsafe { arena_from_handle(arena) }
        .paint_state()
        .borrow_mut()
        .trace_recordings = enabled;
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::css::css_pixels::CssPixelRect;
    use crate::layout::LayoutNodeArena;
    use crate::layout::node_data::NodeKind;
    use crate::painting::host::RootBackgroundSource;
    use crate::painting::paintable_data::FfiOverflowData;
    use crate::painting::record::damage::PaintDamage;
    use crate::painting::visual_context::dirty::VisualContextBoxDirtyKind;
    use crate::painting::visual_context::{TransformData, TransformDataRole, VisualContextTree};

    #[test]
    fn publishing_measured_rows_can_remeasure_viewport_overflow_and_invalidate_painting() {
        {
            let mut arena = LayoutNodeArena::new();
            let viewport = arena.allocate_for_test().slot;
            arena.write_shape(viewport).set_kind(NodeKind::Viewport);
            arena.populate_paintable_row(viewport);
            arena.scrollable_overflow.viewport.set(Some(viewport));
            let root = arena.allocate_for_test().slot;
            arena.populate_paintable_row(root);
            {
                let mut state = arena.paint_state().borrow_mut();
                state.root_background_source = Some(RootBackgroundSource {
                    root_layout_node: root,
                    ..Default::default()
                });
                state.visual_context.tree = Some(std::sync::Arc::new(VisualContextTree::create(TransformData {
                    matrix: libgfx_rust::FloatMatrix4x4::identity(),
                    origin: Default::default(),
                    sorting_context_root_index: None,
                    flattens_inherited_transform: false,
                    role: TransformDataRole::CssTransform,
                    synthetic_plane: false,
                    establishes_sorting_context: false,
                })));
                state.visual_context.dirty_boxes.clear();
            }
            // Leave stale overflow for the hit-test geometry query to remeasure. Losing
            // scrollability must invalidate both the root background and visual context.
            arena.committed_side_data_mut(viewport).overflow_relative_to_padding_box = FfiOverflowData {
                rect: CssPixelRect::new(
                    CssPixels::from_integer(0),
                    CssPixels::from_integer(0),
                    CssPixels::from_integer(100),
                    CssPixels::from_integer(2000),
                )
                .into(),
                has_scrollable_overflow: true,
            };
            arena
                .paintable_side_data(viewport)
                .overflow_measured_this_commit
                .set(true);
            arena.note_publishing_paint_recording_started();
            arena.clear_paint_damage_consumed_by_published_recording();

            // Hit testing reads rows published with every row's overflow measured.
            let rows = arena.publish_row_snapshot(true);
            let absolute_rects = std::cell::RefCell::default();
            let source = crate::painting::paint_read::PaintSource::over_rows(&rows.paintable, &absolute_rects);
            let rect = crate::painting::paintable_geometry::scrollable_overflow_rect(&source, viewport);
            assert_eq!(rect, Some(CssPixelRect::default()));
            assert!(arena.scrollable_overflow.geometry_changed.get());
            assert!(arena.scrollable_overflow.scrollability_changed.get());
            assert!(arena.paint_damage_of_row(root).contains(PaintDamage::DRAW_BACKGROUND));
            assert!(
                arena
                    .paint_damage_of_row(viewport)
                    .contains(PaintDamage::SCROLL_METADATA)
            );
            assert!(
                arena.paint_state().borrow().visual_context.dirty_boxes.boxes[&viewport]
                    .contains(VisualContextBoxDirtyKind::ScrollableOverflowFlipped)
            );
        }
    }

    #[test]
    fn preparing_for_rendering_measures_root_overflow_before_recording_reads_it() {
        let mut arena = LayoutNodeArena::new();
        let viewport = arena.allocate_for_test().slot;
        arena.write_shape(viewport).set_kind(NodeKind::Viewport);
        arena.populate_paintable_row(viewport);
        arena.scrollable_overflow.viewport.set(Some(viewport));
        let root = arena.allocate_for_test().slot;
        arena.write_shape(root).set_kind(NodeKind::BlockContainer);
        arena.populate_paintable_row(root);
        // A structural change invalidated the root's overflow, measured earlier in this commit,
        // without queueing a recalculation, so nothing but a query measures it again. Measuring
        // it drops its scrollable overflow.
        arena.committed_side_data_mut(root).overflow_relative_to_padding_box = FfiOverflowData {
            rect: CssPixelRect::new(
                CssPixels::from_integer(0),
                CssPixels::from_integer(0),
                CssPixels::from_integer(100),
                CssPixels::from_integer(2000),
            )
            .into(),
            has_scrollable_overflow: true,
        };
        arena.paintable_side_data(root).overflow_measured_this_commit.set(true);
        arena.paint_state().borrow_mut().visual_context.dirty_boxes.clear();

        let outcome = prepare_for_rendering(
            &crate::stage::MainThread::for_test(),
            &arena,
            RootBackgroundSource {
                root_layout_node: root,
                ..Default::default()
            },
            false,
        );
        assert!(outcome.requires_visual_context_update);
        assert!(
            arena.paint_state().borrow().visual_context.dirty_boxes.boxes[&root]
                .contains(VisualContextBoxDirtyKind::ScrollableOverflowFlipped)
        );

        // Recording reads the root's overflow while it holds the paint state.
        let _paint_state = arena.paint_state().borrow();
        let canvas_rect = crate::painting::record::paint::background_resolution::root_background_canvas_rect(
            &arena.paintable_rows(),
            root,
            CssPixelRect::default(),
        );
        assert_eq!(canvas_rect, CssPixelRect::default());
    }
}
