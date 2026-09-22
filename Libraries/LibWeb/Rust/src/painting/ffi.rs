/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::css_pixels::{CssPixelPoint, CssPixelRect, CssPixels};
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
use crate::painting::paintable_data::*;
use crate::painting::paintable_rows::{PaintableRowsRead, with_inline_pieces};
use crate::painting::rect_to_viewport_transform::RectToViewportTransform;
use crate::painting::scroll_chain::ViewportWheelOverflow;
use crate::painting::svg_filter::SvgFilterPrimitive;
use libcompositing_rust::ffi::{ffi_slice, tree_from_handle};
use libgfx_rust::filter::Filter;
use std::ffi::c_void;
use std::rc::Rc;

/// SAFETY: `arena` must be a live handle from `layout_arena_create`, borrowed for this call on
/// the document thread.
pub(crate) unsafe fn arena_from_handle<'a>(arena: *mut c_void) -> &'a LayoutNodeArena {
    unsafe { LayoutNodeArena::from_handle(arena) }
}

/// SAFETY: `arena` must be a live handle from `layout_arena_create`, exclusively borrowed for
/// this call on the document thread. No C++ callback may re-enter the arena during the borrow.
unsafe fn arena_from_handle_mut<'a>(arena: *mut c_void) -> &'a mut LayoutNodeArena {
    unsafe { LayoutNodeArena::from_handle_mut(arena) }
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
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_paintable_set_scrollbar_enlarged(
    arena: *mut c_void,
    slot: NodeSlotId,
    direction: ScrollDirection,
    enlarged: bool,
) {
    let arena = unsafe { arena_from_handle_mut(arena) };
    let mut rows = arena.paintable_rows_mut();
    if !rows.paintable_row_is_populated(slot) {
        return;
    }
    let flag = match direction {
        ScrollDirection::Horizontal => PaintableFlag::HorizontalScrollbarEnlarged,
        ScrollDirection::Vertical => PaintableFlag::VerticalScrollbarEnlarged,
    };
    if rows.paintable_data(slot).has_flag(flag) == enlarged {
        return;
    }
    rows.paintable_data_mut(slot).set_flag(flag, enlarged);
    use crate::painting::record::damage::PaintDamage;
    rows.push_paint_damage(slot, PaintDamage::DRAW_OVERLAY | PaintDamage::HIT_OVERLAY);
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_paintable_physical_resize_axes(
    arena: *mut c_void,
    slot: NodeSlotId,
) -> FfiPhysicalResizeAxes {
    let arena = unsafe { arena_from_handle(arena) };
    let axes = crate::painting::chrome_geometry::physical_resize_axes(&arena.paintable_rows(), slot);
    FfiPhysicalResizeAxes {
        horizontal: axes.horizontal,
        vertical: axes.vertical,
    }
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_paintable_is_chrome_mirrored(arena: *mut c_void, slot: NodeSlotId) -> bool {
    let arena = unsafe { arena_from_handle(arena) };
    crate::painting::chrome_geometry::is_chrome_mirrored(&arena.paintable_rows(), slot)
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_paintable_compute_scrollbar_data(
    arena: *mut c_void,
    slot: NodeSlotId,
    direction: ScrollDirection,
    metrics: FfiChromeMetrics,
    viewport_overflow_x: u8,
    viewport_overflow_y: u8,
    enlarged: bool,
    has_device_scroll_offset: bool,
    device_scroll_offset: f32,
    device_pixels_per_css_pixel: f64,
) -> FfiOptionalScrollbarData {
    let arena = unsafe { arena_from_handle(arena) };
    let paintable_rows = arena.paintable_rows();
    let data = crate::painting::chrome_geometry::ChromeGeometry {
        arena: &paintable_rows,
        metrics,
        viewport_wheel_overflow_x: viewport_overflow_x,
        viewport_wheel_overflow_y: viewport_overflow_y,
    }
    .compute_scrollbar_data(
        slot,
        direction,
        enlarged,
        has_device_scroll_offset.then_some(crate::painting::chrome_geometry::ScrollbarScrollState {
            device_scroll_offset,
            device_pixels_per_css_pixel,
        }),
    );
    let Some(data) = data else {
        return FfiOptionalScrollbarData::default();
    };
    FfiOptionalScrollbarData {
        has_value: true,
        value: FfiScrollbarData {
            gutter_rect: data.gutter_rect.into(),
            thumb_rect: data.thumb_rect.into(),
            track_rect: data.track_rect.into(),
            thumb_travel_to_scroll_ratio: data.thumb_travel_to_scroll_ratio.to_double(),
        },
    }
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_paintable_minimum_scroll_offset(
    arena: *mut c_void,
    slot: NodeSlotId,
) -> FfiCssPixelPoint {
    let arena = unsafe { arena_from_handle(arena) };
    crate::painting::chrome_geometry::minimum_scroll_offset(&arena.paintable_rows(), slot).into()
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_paintable_maximum_scroll_offset(
    arena: *mut c_void,
    slot: NodeSlotId,
) -> FfiCssPixelPoint {
    let arena = unsafe { arena_from_handle(arena) };
    crate::painting::chrome_geometry::maximum_scroll_offset(&arena.paintable_rows(), slot).into()
}

fn scroll_offset_reader(
    arena: &LayoutNodeArena,
    scroll_offset_of_layout_node: unsafe extern "C" fn(*mut c_void) -> FfiCssPixelPoint,
) -> impl Fn(NodeSlotId) -> CssPixelPoint {
    move |node| {
        // SAFETY: The C++ host reads the offset of a live layout node shell synchronously.
        unsafe { scroll_offset_of_layout_node(arena.node_shell(node)) }.into()
    }
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread. The
/// host callback receives live layout node shells.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_scrolling_box_for_scroll_step(
    arena: *mut c_void,
    target: NodeSlotId,
    viewport: NodeSlotId,
    delta: FfiCssPixelPoint,
    viewport_wheel_overflow_x: u8,
    viewport_wheel_overflow_y: u8,
    scroll_offset_of_layout_node: unsafe extern "C" fn(*mut c_void) -> FfiCssPixelPoint,
) -> *mut c_void {
    let arena = unsafe { arena_from_handle(arena) };
    let scrolling_box = crate::painting::scroll_chain::scrolling_box_for_scroll_step(
        &arena.paintable_rows(),
        target,
        viewport,
        delta.into(),
        ViewportWheelOverflow {
            x: viewport_wheel_overflow_x,
            y: viewport_wheel_overflow_y,
        },
        &scroll_offset_reader(arena, scroll_offset_of_layout_node),
    );
    arena.shell_if_live(scrolling_box)
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread. The
/// host callback receives live layout node shells.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_for_each_wheel_scrollable_box_in_containing_block_chain(
    arena: *mut c_void,
    start: NodeSlotId,
    wheel_delta_x: f64,
    wheel_delta_y: f64,
    viewport_wheel_overflow_x: u8,
    viewport_wheel_overflow_y: u8,
    scroll_offset_of_layout_node: unsafe extern "C" fn(*mut c_void) -> FfiCssPixelPoint,
    context: *mut c_void,
    push_scrollable_box: unsafe extern "C" fn(*mut c_void, *mut c_void, f64, f64),
) {
    let arena = unsafe { arena_from_handle(arena) };
    crate::painting::scroll_chain::for_each_wheel_scrollable_box_in_containing_block_chain(
        &arena.paintable_rows(),
        start,
        wheel_delta_x,
        wheel_delta_y,
        ViewportWheelOverflow {
            x: viewport_wheel_overflow_x,
            y: viewport_wheel_overflow_y,
        },
        &scroll_offset_reader(arena, scroll_offset_of_layout_node),
        |node, accepted_delta_x, accepted_delta_y| {
            // SAFETY: The C++ callback appends the shell and deltas to a caller-owned collection.
            unsafe { push_scrollable_box(context, arena.node_shell(node), accepted_delta_x, accepted_delta_y) };
        },
    );
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_first_wheel_scrollable_box_in_containing_block_chain(
    arena: *mut c_void,
    start: NodeSlotId,
    viewport_wheel_overflow_x: u8,
    viewport_wheel_overflow_y: u8,
) -> *mut c_void {
    let arena = unsafe { arena_from_handle(arena) };
    let scrollable_box = crate::painting::scroll_chain::first_wheel_scrollable_box_in_containing_block_chain(
        &arena.paintable_rows(),
        start,
        ViewportWheelOverflow {
            x: viewport_wheel_overflow_x,
            y: viewport_wheel_overflow_y,
        },
    );
    arena.shell_if_live(scrollable_box)
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_paintable_wheel_scrollable_axes(
    arena: *mut c_void,
    slot: NodeSlotId,
    viewport_overflow_x: u8,
    viewport_overflow_y: u8,
) -> FfiPhysicalResizeAxes {
    let arena = unsafe { arena_from_handle(arena) };
    let axes = crate::painting::chrome_geometry::wheel_scrollable_axes(
        &arena.paintable_rows(),
        slot,
        viewport_overflow_x,
        viewport_overflow_y,
    );
    FfiPhysicalResizeAxes {
        horizontal: axes.horizontal,
        vertical: axes.vertical,
    }
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_set_chrome_state_callback(
    arena: *mut c_void,
    context: *mut c_void,
    callback: unsafe extern "C" fn(*mut c_void, NodeSlotId, PaintableRowResetKind),
) {
    let arena = unsafe { arena_from_handle(arena) };
    arena.set_chrome_state_callback(context, callback);
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_clear_chrome_state_callback(arena: *mut c_void) {
    let arena = unsafe { arena_from_handle(arena) };
    arena.clear_chrome_state_callback();
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_node_establishes_an_absolute_positioning_containing_block(
    arena: *mut c_void,
    node: NodeSlotId,
) -> bool {
    let arena = unsafe { arena_from_handle(arena) };
    crate::painting::style_queries::establishes_positioning_containing_blocks(arena, node).0
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_node_establishes_a_fixed_positioning_containing_block(
    arena: *mut c_void,
    node: NodeSlotId,
) -> bool {
    let arena = unsafe { arena_from_handle(arena) };
    crate::painting::style_queries::establishes_positioning_containing_blocks(arena, node).1
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_any_ancestor_establishes_a_fixed_position_containing_block(
    arena: *mut c_void,
    node: NodeSlotId,
) -> bool {
    let arena = unsafe { arena_from_handle(arena) };
    crate::painting::style_queries::any_ancestor_establishes_a_fixed_position_containing_block(arena, node)
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_node_has_css_transform(arena: *mut c_void, node: NodeSlotId) -> bool {
    let arena = unsafe { arena_from_handle(arena) };
    let Some(style) = arena.node_style_if_live(node) else {
        return false;
    };
    crate::painting::style_queries::has_css_transform(arena, node, style)
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread, and
/// `node` must name a live node in this arena.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_invalidate_nearest_self_painting_inline_paint_cache(
    arena: *mut c_void,
    node: NodeSlotId,
) {
    let arena = unsafe { arena_from_handle(arena) };
    if let Some(ancestor) =
        crate::painting::fragment_ownership::nearest_self_painting_inline_box(&arena.paintable_rows(), node)
    {
        use crate::painting::record::damage::PaintDamage;
        arena.push_paint_damage(ancestor, PaintDamage::ALL_DRAW | PaintDamage::ALL_HIT);
    }
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_paintable_row(arena: *mut c_void, slot: NodeSlotId) -> *const PaintableData {
    let arena = unsafe { arena_from_handle(arena) };
    let paintable_rows = arena.paintable_rows();
    if !paintable_rows.paintable_row_is_populated(slot) {
        return std::ptr::null();
    }
    paintable_rows.paintable_data_ptr(slot)
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_paintable_cleared_from_node(arena: *mut c_void, layout_node: NodeSlotId) {
    let reset = {
        let arena = unsafe { arena_from_handle(arena) };
        arena.clear_committed_fragment_link(layout_node);
        arena.prepare_paintable_row_cleared_reset(layout_node)
    };
    if let Some(reset) = reset {
        reset.invoke_callback();
        let arena = unsafe { arena_from_handle_mut(arena) };
        arena.paintable_row_cleared(reset);
    }
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_paintable_event_dispatch_node_shell(
    arena: *mut c_void,
    slot: NodeSlotId,
) -> *mut c_void {
    let arena = unsafe { arena_from_handle(arena) };
    crate::painting::hit_test::resolve::event_dispatch_shell_for_paintable(arena, slot)
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_paintable_layout_node_shell(arena: *mut c_void, slot: NodeSlotId) -> *mut c_void {
    let arena = unsafe { arena_from_handle(arena) };
    if !arena.paintable_row_is_populated(slot) {
        return std::ptr::null_mut();
    }
    arena.shell_if_live(slot)
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_paintable_has_child_paintables(arena: *mut c_void, slot: NodeSlotId) -> bool {
    let arena = unsafe { arena_from_handle(arena) };
    crate::painting::paint_order::first_paint_child(&arena.paintable_rows(), slot).is_some()
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the
/// document thread. `entries` must point at `entry_count` valid entries.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_selection_apply(
    arena: *mut c_void,
    viewport: NodeSlotId,
    entries: *const FfiSelectionEntry,
    entry_count: usize,
    range_start_offset: usize,
    range_end_offset: usize,
) {
    let arena = unsafe { arena_from_handle_mut(arena) };
    if !arena.paintable_row_is_populated(viewport) {
        return;
    }
    // SAFETY: The caller guarantees the entry span is valid for this synchronous call.
    let entries = unsafe { ffi_slice(entries, entry_count) };
    let text_states = crate::painting::selection::apply(&mut arena.paintable_rows_mut(), viewport, entries);
    arena.paint_state().borrow_mut().selection = Some(crate::painting::selection::SelectionRange {
        start_offset: range_start_offset,
        end_offset: range_end_offset,
        text_states,
    });
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_selection_clear(arena: *mut c_void, viewport: NodeSlotId) {
    let arena = unsafe { arena_from_handle_mut(arena) };
    if !arena.paintable_row_is_populated(viewport) {
        return;
    }
    crate::painting::selection::clear(&mut arena.paintable_rows_mut(), viewport);
}

#[repr(C)]
pub struct FfiPhysicalOverflowDirections {
    pub horizontal_axis_is_positive: bool,
    pub vertical_axis_is_positive: bool,
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
    unsafe { arena_from_handle(arena) }
        .scrollable_overflow
        .host
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
    let Some(rect) = crate::painting::paintable_geometry::scrollable_overflow_rect(&arena.paintable_rows(), slot)
    else {
        return FfiOptionalOverflowData::default();
    };
    let mut value = arena.paintable_side_data(slot).overflow_relative_to_padding_box.get();
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

/// # Safety
///
/// `arena` must be a live arena used on the document thread. Host callbacks must
/// remain valid for this call and must not mutate layout geometry.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_prepare_for_rendering(
    arena: *mut c_void,
    callbacks: FfiVisualContextHostCallbacks,
    root_background_source: crate::painting::host::FfiRootBackgroundSource,
    visual_context_update_pending: bool,
) -> FfiRenderingPreparationOutcome {
    let arena = unsafe { arena_from_handle(arena) };
    let background_source_changed = arena
        .paint_state()
        .borrow_mut()
        .update_root_background_source(arena, root_background_source);
    crate::painting::scrollable_overflow::update_scrollable_overflow(arena);
    // The root background covers the viewport united with the root's scrollable overflow, which
    // recording reads. Measure it here: measuring it lazily during recording could flip its
    // scrollability while the paint state is borrowed, and the flip would miss this frame.
    arena.ensure_scrollable_overflow(root_background_source.root_layout_node);
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
                &callbacks.tree_inputs(),
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
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
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
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
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
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
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
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
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
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
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
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_paintable_is_positioned(arena: *mut c_void, slot: NodeSlotId) -> bool {
    let arena = unsafe { arena_from_handle(arena) };
    if !arena.paintable_row_is_populated(slot) {
        return false;
    }
    crate::painting::style_queries::is_positioned(arena, slot)
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_paintable_absolute_rect(arena: *mut c_void, slot: NodeSlotId) -> FfiCssPixelRect {
    let arena = unsafe { arena_from_handle(arena) };
    crate::painting::paintable_geometry::absolute_rect_or_default(&arena.paintable_rows(), slot).into()
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
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
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
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
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
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
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_visual_context_note_box_dirty(
    arena: *mut c_void,
    slot: NodeSlotId,
    kind: crate::painting::host::FfiVisualContextBoxDirtyKind,
) {
    use crate::painting::host::FfiVisualContextBoxDirtyKind;
    use crate::painting::visual_context::dirty::VisualContextBoxDirtyKind;
    let arena = unsafe { arena_from_handle(arena) };
    if !arena.paintable_row_is_populated(slot) {
        return;
    }
    let kind = match kind {
        FfiVisualContextBoxDirtyKind::StyleValueChange => VisualContextBoxDirtyKind::StyleValueChange,
        FfiVisualContextBoxDirtyKind::StyleStructuralChange => VisualContextBoxDirtyKind::StyleStructuralChange,
        FfiVisualContextBoxDirtyKind::ScrollableOverflowFlipped => VisualContextBoxDirtyKind::ScrollableOverflowFlipped,
    };
    arena.note_visual_context_box_dirty(slot, kind);
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
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
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
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
    root_background_source: crate::painting::host::FfiRootBackgroundSource,
) -> bool {
    let arena = unsafe { arena_from_handle(arena) };
    crate::painting::record::paint::background_resolution::background_color_can_be_compositor_animated(
        &arena.paintable_rows(),
        slot,
        root_background_source,
    )
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
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
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread; `out`
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

use crate::painting::host::FfiVisualContextHostCallbacks;

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
    callbacks: &FfiVisualContextHostCallbacks,
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
        fresh_tree.viewport_assignment.node_identity = callbacks.node_identity(arena.shell_if_live(viewport));
        fresh_tree
    };
    {
        let arena = unsafe { arena_from_handle_mut(arena) };
        let mut paintable_rows = arena.paintable_rows_mut();
        paintable_rows.drop_all_visual_context_records();
        fresh_tree.viewport_assignment.apply(&mut paintable_rows);
    }
    state.tree = Some(Rc::new(fresh_tree.tree));
    state.dirty_boxes.clear();
    state.build_count += 1;
    let mut outcome = {
        let arena = unsafe { arena_from_handle(arena) };
        let paintable_rows = arena.paintable_rows();
        match update_visual_context_tree(
            &paintable_rows,
            callbacks,
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

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_update_accumulated_visual_contexts(
    arena: *mut c_void,
    viewport: NodeSlotId,
    callbacks: FfiVisualContextHostCallbacks,
) -> crate::painting::host::FfiVisualContextUpdateOutcome {
    use crate::painting::visual_context::dirty::{VisualContextGlobalRebuildReason, VisualContextUpdateScope};
    use crate::painting::visual_context::incremental::{
        IncrementalUpdateResult, debug_assert_every_live_node_is_owned, update_visual_context_tree,
    };
    let arena_ref = unsafe { arena_from_handle(arena) };
    if !arena_ref.paintable_row_is_populated(viewport) {
        return crate::painting::host::FfiVisualContextUpdateOutcome::default();
    }
    let inputs = callbacks.tree_inputs();
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
            update_visual_context_tree(&paintable_rows, &callbacks, viewport, inputs, scope, &mut state)
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
    let outcome = fresh_visual_context_tree_build(arena, viewport, &callbacks, inputs, &mut state);
    state.last_tree_inputs = Some(inputs);
    let arena_ref = unsafe { arena_from_handle(arena) };
    arena_ref.paint_state().borrow_mut().visual_context = state;
    outcome
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
/// `out_geometry` must point to writable storage.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_snap_container_geometry(
    arena: *mut c_void,
    snap_container: NodeSlotId,
    out_geometry: *mut crate::painting::host::FfiSnapContainerGeometry,
) -> bool {
    let arena = unsafe { arena_from_handle(arena) };
    let Some(geometry) = crate::painting::scroll_snap::snap_container_geometry(&arena.paintable_rows(), snap_container)
    else {
        return false;
    };
    // SAFETY: The caller provides writable storage for the geometry.
    unsafe { *out_geometry = geometry };
    true
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread. The
/// host callback receives each snap area's geometry, valid for the duration of the call, and the
/// area's live layout node shell.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_for_each_snap_area(
    arena: *mut c_void,
    snap_container: NodeSlotId,
    context: *mut c_void,
    push_snap_area: unsafe extern "C" fn(*mut c_void, *const crate::painting::host::FfiSnapAreaGeometry, *mut c_void),
) {
    let arena = unsafe { arena_from_handle(arena) };
    crate::painting::scroll_snap::for_each_snap_area(&arena.paintable_rows(), snap_container, |slot, area| {
        // SAFETY: The C++ callback copies the geometry into a caller-owned collection.
        unsafe { push_snap_area(context, &raw const area, arena.node_shell(slot)) };
    });
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_scroll_snapport_rect(
    arena: *mut c_void,
    snap_container: NodeSlotId,
    scrollport: FfiCssPixelRect,
) -> FfiCssPixelRect {
    let arena = unsafe { arena_from_handle(arena) };
    crate::painting::scroll_snap::scroll_snapport_rect(arena, snap_container, scrollport.into()).into()
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_update_visual_viewport_transform(
    arena: *mut c_void,
    callbacks: FfiVisualContextHostCallbacks,
) -> bool {
    let arena = unsafe { arena_from_handle(arena) };
    let mut paint_state = arena.paint_state().borrow_mut();
    let Some(tree) = &mut paint_state.visual_context.tree else {
        return false;
    };
    let inputs = callbacks.tree_inputs();
    Rc::make_mut(tree).set_visual_viewport_transform(
        crate::painting::visual_context::node_values::visual_viewport_transform_data(&inputs),
    );
    true
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_invalidate_scroll_state(arena: *mut c_void) {
    let arena = unsafe { arena_from_handle(arena) };
    arena
        .paint_state()
        .borrow_mut()
        .visual_context
        .needs_to_refresh_scroll_state = true;
}

/// The index of the sticky node the accumulated visual context tree holds for `paintable`, which
/// is where the scroll state snapshot keeps its resolved sticky offset, or `u32::MAX` when the tree
/// holds none.
///
/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_sticky_spatial_node_index(arena: *mut c_void, paintable: NodeSlotId) -> u32 {
    let arena = unsafe { arena_from_handle(arena) };
    let paint_state = arena.paint_state().borrow();
    paint_state
        .visual_context
        .scroll_state
        .states
        .iter()
        .find(|state| state.is_sticky && state.paintable == paintable)
        .map_or(u32::MAX, |state| state.node_index.0)
}

/// Re-reads the scroll containers' offsets when something invalidated them since the last
/// refresh, resolves the sticky nodes' offsets on top of them, and hands the dense device-pixel
/// snapshot to `publish`. Returns whether that happened, so the caller keeps its copy otherwise;
/// `force` re-derives the snapshot even when nothing invalidated it, for verification.
///
/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread;
/// `publish` is called synchronously with `sink` and a view of the snapshot that is valid only
/// for the duration of that call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_refresh_scroll_state(
    arena: *mut c_void,
    callbacks: FfiVisualContextHostCallbacks,
    force: bool,
    sink: *mut c_void,
    publish: unsafe extern "C" fn(*mut c_void, *const libgfx_rust::FloatPoint, usize),
) -> bool {
    let arena = unsafe { arena_from_handle(arena) };
    let snapshot = {
        let paintable_rows = arena.paintable_rows();
        let mut paint_state = arena.paint_state().borrow_mut();
        let state = &mut paint_state.visual_context;
        if !force && !state.needs_to_refresh_scroll_state {
            return false;
        }
        state.needs_to_refresh_scroll_state = false;
        crate::painting::visual_context::refresh::refresh_scroll_state(
            &paintable_rows,
            &callbacks,
            &mut state.scroll_state,
        );
        let mut snapshot = state
            .scroll_state
            .snapshot(callbacks.tree_inputs().device_pixels_per_css_pixel);
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

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
/// Input arrays and byte buffers must remain valid and immutable throughout this call;
/// fonts for enabled overlays must be live `Gfx::Font`s.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_record_display_list(
    arena: *mut c_void,
    viewport: NodeSlotId,
    inputs: crate::painting::host::FfiRecordingInputs,
) -> bool {
    let arena = unsafe { arena_from_handle(arena) };
    {
        let mut paint_state = arena.paint_state().borrow_mut();
        debug_assert!(
            paint_state.pending_recording.is_none(),
            "a recording must be published before the next one starts"
        );
        paint_state.pending_recording_trace = None;
        paint_state.pending_recording = None;
    }
    let (recording, recording_from_scratch) = {
        let paint_state = arena.paint_state().borrow();
        if !arena.paintable_row_is_populated(viewport) || arena.stacking_context_entries(viewport).is_none() {
            return false;
        }
        let visual_context = &paint_state.visual_context;
        // SAFETY: The host lends the input arrays and buffers for this call. Only owned
        // output and retained resources escape into the pending recording below.
        let inputs = unsafe {
            inputs.borrow_recording_inputs(
                visual_context
                    .last_tree_inputs
                    .expect("a recording follows a visual context update"),
                paint_state
                    .root_background_source
                    .expect("a recording follows paint preparation"),
            )
        };
        // The root background paints the union of the viewport and the root's overflow, so it
        // is the one output a viewport move can change. Drop its caches before recording
        // starts instead of treating the viewport position as a frame-wide input.
        if let Some(source) = &paint_state.published_frame {
            let root = inputs.uncaptured.root_background_source.root_layout_node;
            let rows = arena.paintable_rows();
            let canvas_rect = crate::painting::record::paint::background_resolution::root_background_canvas_rect(
                &rows,
                root,
                inputs.css_viewport_rect,
            );
            if canvas_rect != source.root_background_canvas_rect {
                arena.push_paint_damage(root, crate::painting::record::damage::PaintDamage::DRAW_BACKGROUND);
            }
        }
        if inputs.publishes_recording {
            arena.note_publishing_paint_recording_started();
        }
        let mut scratch = arena.recording_scratch().borrow_mut();
        // The retained tree describes the published tape and is written in place while a frame
        // is assembled, so only a recording that publishes may copy from that frame or touch
        // the tree; any other recording records from scratch into a tree of its own.
        let mut retained_tree = paint_state.paint_order_tree.borrow_mut();
        let mut throwaway_tree = crate::painting::record::order_tree::PaintOrderTree::default();
        let (tree, source_frame, source_items) = if inputs.publishes_recording {
            (
                &mut *retained_tree,
                paint_state.published_frame.clone(),
                paint_state.published_hit_test_items.clone(),
            )
        } else {
            (&mut throwaway_tree, None, None)
        };
        let copies_from_published_frame = source_frame.is_some();
        arena.set_paint_recording_in_progress(true);
        let recording = crate::painting::record::traversal::record_display_list(
            arena,
            &paint_state,
            &mut scratch,
            tree,
            viewport,
            &inputs,
            paint_state.hit_test_list_generation + 1,
            source_frame,
            source_items,
            true,
            paint_state.trace_recordings || crate::painting::record::verify::enabled_by_environment(),
        );
        // The oracle records the same frame from scratch into a throwaway tree whenever the
        // published frame could have been copied from.
        let recording_from_scratch =
            (crate::painting::record::verify::enabled_by_environment() && copies_from_published_frame).then(|| {
                let mut inputs_for_recording_from_scratch = inputs.clone();
                inputs_for_recording_from_scratch.publishes_recording = false;
                let mut tree_for_recording_from_scratch =
                    crate::painting::record::order_tree::PaintOrderTree::default();
                crate::painting::record::traversal::record_display_list(
                    arena,
                    &paint_state,
                    &mut scratch,
                    &mut tree_for_recording_from_scratch,
                    viewport,
                    &inputs_for_recording_from_scratch,
                    paint_state.hit_test_list_generation + 1,
                    None,
                    None,
                    false,
                    false,
                )
            });
        arena.set_paint_recording_in_progress(false);
        (recording, recording_from_scratch)
    };
    let mut paint_state = arena.paint_state().borrow_mut();
    if paint_state.trace_recordings && recording.output.capture_log_for_verification.is_some() {
        paint_state.pending_recording_trace = Some(crate::painting::paint_state::PendingRecordingTrace {
            viewport,
            should_paint_overlay: inputs.should_paint_overlay,
        });
    }
    paint_state.pending_recording = Some(crate::painting::paint_state::PendingRecording {
        recording,
        recording_from_scratch,
        publishes_recording: inputs.publishes_recording,
    });
    true
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_set_form_control_paint_facts(
    arena: *mut c_void,
    slot: NodeSlotId,
    facts: crate::painting::host::FfiFormControlPaintFacts,
) -> bool {
    let arena = unsafe { arena_from_handle(arena) };
    arena.set_replaced_paint_facts(
        slot,
        crate::painting::replaced_paint_facts::ReplacedPaintFacts::FormControl(facts),
    )
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_set_canvas_paint_facts(
    arena: *mut c_void,
    slot: NodeSlotId,
    facts: crate::painting::host::FfiCanvasPaintFacts,
) -> bool {
    let arena = unsafe { arena_from_handle(arena) };
    arena.set_replaced_paint_facts(
        slot,
        crate::painting::replaced_paint_facts::ReplacedPaintFacts::Canvas(facts),
    )
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread, and
/// `entries` must point at `count` readable entries whose frame pointers are null or live.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_set_layer_image_paint_facts(
    arena: *mut c_void,
    slot: NodeSlotId,
    entries: *const crate::painting::host::FfiLayerImagePaintFactsEntry,
    count: usize,
) -> bool {
    let arena = unsafe { arena_from_handle(arena) };
    let entries = if count == 0 {
        Vec::new()
    } else {
        unsafe { std::slice::from_raw_parts(entries, count) }
            .iter()
            .map(
                |entry| crate::painting::layer_image_paint_facts::LayerImagePaintFactsEntry {
                    list: entry.list,
                    computed_index: entry.computed_index,
                    facts: unsafe {
                        crate::painting::layer_image_paint_facts::LayerImagePaintFacts::from_ffi(&entry.facts)
                    },
                },
            )
            .collect()
    };
    arena.set_layer_image_paint_facts(slot, entries)
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread, and
/// `facts.frame` must be null or point to a live `Gfx::DecodedImageFrame`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_set_replaced_image_paint_facts(
    arena: *mut c_void,
    slot: NodeSlotId,
    facts: crate::painting::host::FfiReplacedImagePaintFacts,
) -> bool {
    let arena = unsafe { arena_from_handle(arena) };
    let facts = unsafe { crate::painting::replaced_paint_facts::ImagePaintFacts::from_ffi(&facts) };
    arena.set_replaced_paint_facts(
        slot,
        crate::painting::replaced_paint_facts::ReplacedPaintFacts::Image(facts),
    )
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread, and
/// `facts.poster_frame` must be null or point to a live `Gfx::DecodedImageFrame`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_set_video_paint_facts(
    arena: *mut c_void,
    slot: NodeSlotId,
    facts: crate::painting::host::FfiVideoPaintFacts,
) -> bool {
    let arena = unsafe { arena_from_handle(arena) };
    let facts = unsafe { crate::painting::replaced_paint_facts::VideoPaintFacts::from_ffi(&facts) };
    arena.set_replaced_paint_facts(
        slot,
        crate::painting::replaced_paint_facts::ReplacedPaintFacts::Video(facts),
    )
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_set_navigable_container_paint_facts(
    arena: *mut c_void,
    slot: NodeSlotId,
    facts: crate::painting::host::FfiNavigableContainerPaintFacts,
) -> bool {
    let arena = unsafe { arena_from_handle(arena) };
    arena.set_replaced_paint_facts(
        slot,
        crate::painting::replaced_paint_facts::ReplacedPaintFacts::NavigableContainer(facts),
    )
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_scroll_snap_axes(
    arena: *mut c_void,
    snap_container: NodeSlotId,
) -> crate::painting::host::FfiSnapAxes {
    let arena = unsafe { arena_from_handle(arena) };
    crate::painting::scroll_snap::snap_axes_of_scroll_container(arena, snap_container)
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`; the callbacks in `publish` are
/// called synchronously with their context while the recording's resources are live.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_publish_recording(
    arena: *mut c_void,
    publish: crate::painting::host::FfiRecordingPublishCallbacks,
) -> u64 {
    let arena = unsafe { arena_from_handle(arena) };
    let Some(pending) = arena.paint_state().borrow_mut().pending_recording.take() else {
        return 0;
    };
    crate::painting::record::publish::publish_recording(arena, pending, &publish)
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`; `describe_node` and `append_text`
/// are called synchronously with `context`, and the shells handed to `describe_node` are the
/// last recording's live paintable shells.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_take_recording_trace(
    arena: *mut c_void,
    context: *mut c_void,
    describe_node: unsafe extern "C" fn(*mut c_void, *mut c_void, *mut c_void),
    append_text: unsafe extern "C" fn(*mut c_void, *const u8, usize),
) -> bool {
    let arena = unsafe { arena_from_handle(arena) };
    let (pending, recording) = {
        let mut paint_state = arena.paint_state().borrow_mut();
        let Some(pending) = paint_state.pending_recording_trace.take() else {
            return false;
        };
        let Some(recording) = paint_state.last_recording.clone() else {
            return false;
        };
        (pending, recording)
    };
    let Some(log) = recording.capture_log_for_verification.as_ref() else {
        return false;
    };
    let mut name = |slot| {
        if slot == pending.viewport {
            return "@viewport".into();
        }
        let mut name = Vec::<u8>::new();
        // SAFETY: the last recording's paintable shells are still live, and the host copies the
        // description synchronously into the sink.
        unsafe { describe_node(context, arena.shell_if_live(slot), (&raw mut name).cast()) };
        String::from_utf8(name).expect("trace label must be UTF-8")
    };
    let text = format!(
        "recording (overlay={})\n{}",
        pending.should_paint_overlay,
        log.format(&mut name)
    );
    // SAFETY: the host copies the text synchronously.
    unsafe { append_text(context, text.as_ptr(), text.len()) };
    true
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread;
/// `shadows` points at `shadow_count` layers, or is null when the count is zero.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_set_node_selection_pseudo_style(
    arena: *mut c_void,
    slot: NodeSlotId,
    has_styling: bool,
    facts: crate::painting::host::FfiSelectionStyleFacts,
    shadows: *const crate::painting::host::FfiSelectionShadowLayer,
    shadow_count: usize,
) {
    let arena = unsafe { arena_from_handle(arena) };
    let rows = arena.rows_sharing_dom_node_with(slot);
    let mut paint_state = arena.paint_state().borrow_mut();
    if !has_styling {
        for row in rows {
            paint_state.selection_pseudo_styles.remove(&row);
        }
        return;
    }
    let shadows = if shadow_count == 0 {
        &[][..]
    } else {
        // SAFETY: The host passes `shadow_count` layers that stay alive for this call.
        unsafe { std::slice::from_raw_parts(shadows, shadow_count) }
    };
    let shadows = shadows
        .iter()
        .map(|layer| crate::painting::record::paint::text::ShadowLayer {
            color: layer.color.0,
            offset_x: layer.offset_x,
            offset_y: layer.offset_y,
            blur_radius: layer.blur_radius,
        })
        .collect();
    let answer = std::rc::Rc::new(crate::painting::record::paint::text::SelectionStyleAnswer { facts, shadows });
    for row in rows {
        paint_state.selection_pseudo_styles.insert(row, answer.clone());
    }
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
            Rc::into_raw(Rc::new(tree)).cast(),
        );
    }
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_last_recording_is_identical_to_published_frame(arena: *mut c_void) -> bool {
    let arena = unsafe { arena_from_handle(arena) };
    let paint_state = arena.paint_state().borrow();
    paint_state
        .last_recording
        .as_ref()
        .is_some_and(|recording| recording.is_identical_to_published_frame)
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_last_recording_has_blocking_wheel_event_listeners(arena: *mut c_void) -> bool {
    let arena = unsafe { arena_from_handle(arena) };
    let paint_state = arena.paint_state().borrow();
    paint_state
        .last_recording
        .as_ref()
        .is_some_and(|recording| recording.has_blocking_wheel_event_listeners)
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_paintable_invalidate_paint_cache(
    arena: *mut c_void,
    paintable: NodeSlotId,
    propagated_text_decorations: bool,
) {
    use crate::painting::record::damage::PaintDamage;
    let arena = unsafe { arena_from_handle(arena) };
    if propagated_text_decorations {
        arena.push_propagated_text_decoration_damage(paintable);
    } else {
        arena.push_paint_damage(paintable, PaintDamage::ALL_DRAW | PaintDamage::ALL_HIT);
    }
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_paintable_invalidate_for_repaint(
    arena: *mut c_void,
    paintable: NodeSlotId,
    include_hit_test_items: bool,
) {
    use crate::painting::record::damage::PaintDamage;
    let arena = unsafe { arena_from_handle(arena) };
    let damage = if include_hit_test_items {
        PaintDamage::ALL_PRODUCERS
    } else {
        PaintDamage::ALL_DRAW
    };
    arena.push_paint_damage_for_repaint(paintable, damage);
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_paintable_invalidate_subtree_for_repaint(
    arena: *mut c_void,
    paintable: NodeSlotId,
) {
    let arena = unsafe { arena_from_handle(arena) };
    arena.push_paint_damage_to_paint_subtree(paintable, crate::painting::record::damage::PaintDamage::ALL_PRODUCERS);
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_invalidate_all_paint_caches(arena: *mut c_void) {
    let arena = unsafe { arena_from_handle(arena) };
    arena.push_all_paint_damage();
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
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
    pub style_source: *mut c_void,
    pub owner_paintable: NodeSlotId,
    pub nearest_self_painting_inline: NodeSlotId,
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document
/// thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_text_caret_rect_for_position(
    arena: *mut c_void,
    primary: NodeSlotId,
    offset: usize,
    affinity_is_downstream: bool,
) -> FfiCaretRectResult {
    let mut result = FfiCaretRectResult {
        found: false,
        rect: FfiCssPixelRect::default(),
        style_source: std::ptr::null_mut(),
        owner_paintable: NodeSlotId::INVALID,
        nearest_self_painting_inline: NodeSlotId::INVALID,
    };
    let arena = unsafe { arena_from_handle(arena) };
    let paintable_rows = arena.paintable_rows();
    let fragments = arena.text_fragments(primary);
    let node_slots = fragments.as_slice();
    let Some(answer) =
        crate::painting::caret::caret_rect_for_position(&paintable_rows, node_slots, offset, affinity_is_downstream)
    else {
        return result;
    };
    result.found = true;
    result.rect = answer.rect.into();
    result.style_source = arena.shell_if_live(answer.style_source);
    result.owner_paintable = answer.owner;
    result.nearest_self_painting_inline =
        crate::painting::fragment_ownership::nearest_self_painting_inline_box(&paintable_rows, answer.node)
            .unwrap_or(NodeSlotId::INVALID);
    result
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document
/// thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_atomic_inline_caret_rect_for_position(
    arena: *mut c_void,
    primary: NodeSlotId,
    after: bool,
) -> FfiCaretRectResult {
    let mut result = FfiCaretRectResult {
        found: false,
        rect: FfiCssPixelRect::default(),
        style_source: std::ptr::null_mut(),
        owner_paintable: NodeSlotId::INVALID,
        nearest_self_painting_inline: NodeSlotId::INVALID,
    };
    let arena = unsafe { arena_from_handle(arena) };
    let paintable_rows = arena.paintable_rows();
    let Some(answer) = crate::painting::caret::caret_rect_for_atomic_inline(&paintable_rows, primary, after) else {
        return result;
    };
    result.found = true;
    result.rect = answer.rect.into();
    result.style_source = arena.shell_if_live(answer.style_source);
    result.owner_paintable = answer.owner;
    result.nearest_self_painting_inline =
        crate::painting::fragment_ownership::nearest_self_painting_inline_box(&paintable_rows, answer.node)
            .unwrap_or(NodeSlotId::INVALID);
    result
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document
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
    pub style_source: *mut c_void,
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document
/// thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_paintable_empty_line_caret_rect(
    arena: *mut c_void,
    block: NodeSlotId,
    primary: NodeSlotId,
    offset: usize,
) -> FfiEmptyLineCaretRect {
    let mut result = FfiEmptyLineCaretRect {
        has_value: false,
        rect: FfiCssPixelRect::default(),
        style_source: std::ptr::null_mut(),
    };
    let arena = unsafe { arena_from_handle(arena) };
    let paintable_rows = arena.paintable_rows();
    if !paintable_rows.paintable_row_is_populated(block) {
        return result;
    }
    let fragments = arena.text_fragments(primary);
    let node_slots = fragments.as_slice();
    let side = arena.paintable_side_data(block);
    let Some(first_fragment) = side.fragments().first() else {
        return result;
    };
    if !node_slots.contains(&first_fragment.layout_node) {
        return result;
    }
    for target in crate::painting::visual_lines::empty_line_caret_targets(&paintable_rows, block) {
        if target.offset == offset {
            result.has_value = true;
            result.rect = target.rect.into();
            result.style_source = arena.shell_if_live(crate::painting::text_fragment::style_source(
                &paintable_rows,
                first_fragment,
            ));
            break;
        }
    }
    result
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
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread, and
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
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread, and
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
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread, and
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
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread, and
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

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
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
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
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
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
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
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
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
/// `arena` must be a live handle from `layout_arena_create`, used on the document
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
/// `arena` must be a live handle from `layout_arena_create`, used on the document
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
/// `arena` must be a live handle from `layout_arena_create`, used on the document
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
/// `arena` must be a live handle from `layout_arena_create`, used on the document
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
/// `arena` must be a live handle from `layout_arena_create`, used on the document
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
/// `arena` must be a live handle from `layout_arena_create`, used on the document
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
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
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
    for fragment in arena.paintable_side_data(block).fragments() {
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
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_for_each_subtree_fragment_rect(
    arena: *mut c_void,
    root: NodeSlotId,
    context: *mut c_void,
    consume: unsafe extern "C" fn(*mut c_void, *mut c_void, FfiCssPixelRect),
) {
    let arena = unsafe { arena_from_handle(arena) };
    let paintable_rows = arena.paintable_rows();
    if !paintable_rows.paintable_row_is_populated(root) {
        return;
    }
    crate::painting::paint_order::for_each_in_paint_subtree(&paintable_rows, root, |current| {
        for fragment in arena.paintable_side_data(current).fragments() {
            let shell = arena.shell_if_live(fragment.layout_node);
            let rect = crate::painting::text_fragment::absolute_rect(&paintable_rows, fragment).into();
            // SAFETY: The consumer copies its plain-data arguments synchronously.
            unsafe { consume(context, shell, rect) };
        }
    });
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread;
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
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
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
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_paintable_flex_layout_json(
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
    if let Some(data) = crate::painting::paintable_geometry::committed_flex_layout_data(arena, paintable) {
        let json = crate::painting::devtools_layout::serialize_flex_layout(&data, container_node_id);
        unsafe { consume(context, json.as_ptr(), json.len()) };
    }
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
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
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread. The
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
        .map_or(std::ptr::null(), |tree| Rc::into_raw(Rc::clone(tree)).cast())
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_has_visual_context_tree(arena: *mut c_void) -> bool {
    let arena = unsafe { arena_from_handle(arena) };
    let paint_state = arena.paint_state().borrow();
    paint_state.visual_context.tree.is_some()
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_visual_context_tree_structural_epoch(arena: *mut c_void) -> u64 {
    let arena = unsafe { arena_from_handle(arena) };
    let paint_state = arena.paint_state().borrow();
    paint_state.visual_context.structural_epoch()
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
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
/// `state` must be a live effect state handle, `arena` a live handle from `layout_arena_create`
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
/// `state` must be a live effect state handle and `arena` a live handle from `layout_arena_create`
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
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
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
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
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
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread; the
/// sink pointer must stay valid for this synchronous call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_hit_test_visit_caret_roots_and_chrome_widgets(
    arena: *mut c_void,
    sink: *mut c_void,
    visit: unsafe extern "C" fn(*mut c_void, NodeSlotId, u8, *mut c_void),
) {
    with_hit_test_list_items_only(arena, (), |list, arena| {
        for item in list.items.iter() {
            let caret_node_shell = arena.shell_if_live(item.caret_node);
            if item.chrome_widget_kind == crate::painting::hit_test::CHROME_WIDGET_NONE && caret_node_shell.is_null() {
                continue;
            }
            // SAFETY: The C++ host consumes the visit synchronously.
            unsafe { visit(sink, item.paintable, item.chrome_widget_kind, caret_node_shell) };
        }
    });
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread;
/// `index` in range.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_hit_test_item_facts(
    arena: *mut c_void,
    index: usize,
) -> crate::painting::host::FfiHitTestItemExport {
    with_hit_test_list_items_only(arena, None, |list, arena| {
        let item = &list.items[index];
        assert!(
            arena.paintable_row_is_populated(item.paintable),
            "exporting a hit-test item for a non-live paintable"
        );
        assert!(
            arena.paintable_row_is_populated(item.hit_node),
            "exporting a hit-test item that names a non-live paintable"
        );
        Some(crate::painting::host::FfiHitTestItemExport {
            can_produce_caret_position: item.can_produce_caret_position,
            paintable: item.paintable,
            hit_node: item.hit_node,
            chrome_widget_kind: item.chrome_widget_kind,
            caret_node_shell: arena.shell_if_live(item.caret_node),
            caret_rect: item.caret_rect.into(),
            context: item.context,
        })
    })
    .expect("no hit-test list")
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread;
/// `item_index` must be in range for the current hit-test list.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_hit_test_item_target_shell(arena: *mut c_void, item_index: usize) -> *mut c_void {
    with_hit_test_list_items_only(arena, std::ptr::null_mut(), |list, arena| {
        list.item_target_shell(arena, item_index)
    })
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread;
/// `item_index` must be in range for the current hit-test list and `out_allow_pseudo_fallback`
/// must be writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_hit_test_item_dispatch_shell(
    arena: *mut c_void,
    item_index: usize,
    out_allow_pseudo_fallback: *mut bool,
) -> *mut c_void {
    with_hit_test_list_items_only(arena, std::ptr::null_mut(), |list, arena| {
        let (shell, allow_pseudo_fallback) = list.item_dispatch_shell(arena, item_index);
        // SAFETY: The caller provides writable storage for the synchronous result.
        unsafe { *out_allow_pseudo_fallback = allow_pseudo_fallback };
        shell
    })
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread;
/// `item_index` must be in range for the current hit-test list.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_hit_test_resolve_hit(
    arena: *mut c_void,
    item_index: usize,
    local_point: FfiCssPixelPoint,
) -> crate::painting::host::FfiResolvedHit {
    with_hit_test_list_items_only(arena, Default::default(), |list, arena| {
        list.resolve_hit(arena, item_index, local_point.into())
    })
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread;
/// `item_index` must be in range for the current hit-test list.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_hit_test_resolve_caret(
    arena: *mut c_void,
    item_index: usize,
    local_point: FfiCssPixelPoint,
    position_type: u8,
) -> crate::painting::host::FfiResolvedCaret {
    with_hit_test_list_items_only(arena, Default::default(), |list, arena| {
        list.resolve_caret(
            arena,
            item_index,
            local_point.into(),
            crate::painting::hit_test::caret::CaretPositionType::from_u8(position_type),
        )
    })
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread;
/// the callback context and function pointers must remain valid for this synchronous call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_hit_test_caret_line_for_position(
    arena: *mut c_void,
    callbacks: crate::painting::host::FfiCaretPositionQueryCallbacks,
    offset: usize,
    affinity_is_downstream: bool,
) -> crate::painting::host::FfiCaretLineForPosition {
    with_hit_test_list_and_caret_lines(arena, Default::default(), |list, arena| {
        match list.caret_line_for_position(arena, &callbacks, offset, affinity_is_downstream) {
            Some(line_index) => crate::painting::host::FfiCaretLineForPosition {
                has_line: true,
                line_index,
            },
            None => Default::default(),
        }
    })
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
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_note_svg_paint_resources_changed(arena: *mut c_void) -> bool {
    let arena = unsafe { arena_from_handle(arena) };
    arena.svg_paint_resources().note_changed()
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_has_enrolled_svg_paint_resources(arena: *mut c_void) -> bool {
    let arena = unsafe { arena_from_handle(arena) };
    arena.svg_paint_resources().has_enrolled_entries()
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread, and
/// both resolvers must answer synchronously from a live layout node shell and only push into
/// the sink whose pointer they receive.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_sync_svg_paint_resources(
    arena: *mut c_void,
    resolve_filter: unsafe extern "C" fn(*mut c_void, *const c_void, *mut c_void) -> bool,
    resolve_paint_server: unsafe extern "C" fn(*mut c_void, bool, *mut c_void),
) -> bool {
    use crate::painting::svg_paint_resources::{PublishedSvgFilter, PublishedSvgPaintServer, SvgPaintResourceKind};
    let arena = unsafe { arena_from_handle(arena) };
    let resources = arena.svg_paint_resources();
    if !resources.take_needs_sync() {
        return false;
    }
    let mut any_changed = false;
    for (slot, kind) in resources.enrolled_entries() {
        let Some(style) = arena.node_style_if_live(slot) else {
            resources.forget_slot(slot);
            continue;
        };
        if matches!(kind, SvgPaintResourceKind::Fill | SvgPaintResourceKind::Stroke) {
            let is_stroke = kind == SvgPaintResourceKind::Stroke;
            let mut published = PublishedSvgPaintServer::None;
            // SAFETY: The host resolves synchronously from the live shell and only pushes into
            // the sink it is handed.
            unsafe { resolve_paint_server(arena.shell_if_live(slot), is_stroke, (&raw mut published).cast()) };
            if resources.publish_paint_server(slot, kind, published) {
                any_changed = true;
                use crate::painting::record::damage::PaintDamage;
                arena.push_paint_damage(slot, PaintDamage::SVG | PaintDamage::SCOPE_PREAMBLE);
            }
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
        let shell = arena.shell_if_live(slot);
        let mut published = PublishedSvgFilter::default();
        for operation in filter_list.operations.as_slice() {
            if operation.kind != crate::painting::css_filter::FILTER_KIND_URL {
                continue;
            }
            let mut primitives: Vec<SvgFilterPrimitive> = Vec::new();
            // SAFETY: The host resolves synchronously from the live shell and only pushes into the
            // primitive list it is handed as its sink.
            let resolved = unsafe { resolve_filter(shell, operation.url_value.pointer, (&raw mut primitives).cast()) };
            published = PublishedSvgFilter {
                failed: !resolved,
                primitives: if resolved { primitives } else { Vec::new() },
            };
            if published.failed {
                break;
            }
        }
        if resources.publish_filter(slot, kind, published) {
            any_changed = true;
            if arena.paintable_row_is_populated(slot) {
                arena.note_visual_context_box_dirty(
                    slot,
                    crate::painting::visual_context::dirty::VisualContextBoxDirtyKind::StyleValueChange,
                );
                use crate::painting::record::damage::PaintDamage;
                arena.push_paint_damage(slot, PaintDamage::SVG | PaintDamage::SCOPE_PREAMBLE);
            }
        }
    }
    any_changed
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

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread. The
/// returned pointers borrow the last recording and stay valid until the next one replaces it.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_retain_recorded_display_list(arena: *mut c_void) -> *const c_void {
    let arena = unsafe { arena_from_handle(arena) };
    let paint_state = arena.paint_state().borrow();
    paint_state
        .last_recording
        .as_ref()
        .map_or(std::ptr::null(), |recording| {
            std::sync::Arc::into_raw(recording.display_list.clone()).cast()
        })
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`; `line_index` in range.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_hit_test_caret_line(
    arena: *mut c_void,
    line_index: usize,
) -> crate::painting::host::FfiCaretLineExport {
    with_hit_test_list_and_caret_lines(arena, Default::default(), |list, _| {
        let line = &list.caret_lines[line_index];
        crate::painting::host::FfiCaretLineExport {
            rect: line.rect.into(),
            context: line.context,
            first_caret_item_index: line.first_caret_item_index,
            last_caret_item_index: line.last_caret_item_index,
        }
    })
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_hit_test_list_generation(arena: *mut c_void) -> u64 {
    let arena = unsafe { arena_from_handle(arena) };
    arena.hit_test_list.borrow().as_ref().map_or(0, |list| list.generation)
}

fn with_hit_test_list_items_only<R>(
    arena: *mut c_void,
    default: R,
    query: impl FnOnce(&crate::painting::hit_test::HitTestList, &crate::layout::LayoutNodeArena) -> R,
) -> R {
    // SAFETY: The caller passes a live arena handle (documented on every entry point below).
    let arena = unsafe { arena_from_handle(arena) };
    let hit_test_list = arena.hit_test_list.borrow();
    let Some(list) = hit_test_list.as_ref() else {
        return default;
    };
    query(list, arena)
}

fn with_hit_test_list_and_caret_lines<R>(
    arena: *mut c_void,
    default: R,
    query: impl FnOnce(&crate::painting::hit_test::HitTestList, &crate::layout::LayoutNodeArena) -> R,
) -> R {
    // SAFETY: The caller passes a live arena handle (documented on every entry point below).
    let arena = unsafe { arena_from_handle(arena) };
    let mut hit_test_list = arena.hit_test_list.borrow_mut();
    let Some(list) = hit_test_list.as_mut() else {
        return default;
    };
    list.build_caret_lines_if_needed(arena);
    query(list, arena)
}

fn with_hit_test_list_spatial_indexes_and_visual_context_tree<R>(
    arena: *mut c_void,
    needs_caret_lines: bool,
    default: R,
    query: impl FnOnce(
        &crate::painting::hit_test::HitTestList,
        &crate::painting::visual_context::VisualContextTree,
        &crate::layout::LayoutNodeArena,
    ) -> R,
) -> R {
    // SAFETY: The caller passes a live arena handle (documented on every entry point below).
    let arena = unsafe { arena_from_handle(arena) };
    let mut hit_test_list = arena.hit_test_list.borrow_mut();
    let Some(list) = hit_test_list.as_mut() else {
        return default;
    };
    list.build_spatial_indexes_if_needed();
    if needs_caret_lines {
        list.build_caret_lines_if_needed(arena);
    }
    // Geometry queries can update overflow and dirty the visual context state. Keep the
    // current tree alive without borrowing that state for the duration of the query.
    let Some(tree) = arena.paint_state().borrow().visual_context.tree.clone() else {
        return default;
    };
    query(list, &tree, arena)
}

fn ffi_topmost(item: Option<crate::painting::hit_test::query::TopmostItem>) -> crate::painting::host::FfiTopmostItem {
    match item {
        Some(item) => crate::painting::host::FfiTopmostItem {
            has_item: true,
            index: item.index,
            local: item.local_point.into(),
        },
        None => crate::painting::host::FfiTopmostItem::default(),
    }
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_hit_test_find_topmost_item(
    arena: *mut c_void,
    callbacks: crate::painting::host::FfiHitTestQueryCallbacks,
    point: FfiCssPixelPoint,
) -> crate::painting::host::FfiTopmostItem {
    with_hit_test_list_spatial_indexes_and_visual_context_tree(arena, false, Default::default(), |list, tree, arena| {
        ffi_topmost(list.find_topmost_item(arena, tree, &callbacks, point.into()))
    })
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_hit_test_find_topmost_items_for_caret(
    arena: *mut c_void,
    callbacks: crate::painting::host::FfiHitTestQueryCallbacks,
    point: FfiCssPixelPoint,
) -> crate::painting::host::FfiTopmostItemsForCaret {
    with_hit_test_list_spatial_indexes_and_visual_context_tree(arena, false, Default::default(), |list, tree, arena| {
        let (caret_item, hit_item) = list.find_topmost_items_for_caret(arena, tree, &callbacks, point.into());
        crate::painting::host::FfiTopmostItemsForCaret {
            caret_item: ffi_topmost(caret_item),
            hit_item: ffi_topmost(hit_item),
        }
    })
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_hit_test_all(
    arena: *mut c_void,
    callbacks: crate::painting::host::FfiHitTestQueryCallbacks,
    point: FfiCssPixelPoint,
    push_context: *mut c_void,
    push: unsafe extern "C" fn(*mut c_void, usize),
) {
    let indices =
        with_hit_test_list_spatial_indexes_and_visual_context_tree(arena, false, Vec::new(), |list, tree, arena| {
            list.hit_test_all(arena, tree, &callbacks, point.into())
        });
    for index in indices {
        // SAFETY: The C++ sink consumes the index synchronously.
        unsafe { push(push_context, index) };
    }
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_hit_test_item_at_line_edge(
    arena: *mut c_void,
    line_index: usize,
    position_type: u8,
) -> usize {
    let position_type = crate::painting::hit_test::caret::CaretPositionType::from_u8(position_type);
    with_hit_test_list_and_caret_lines(arena, usize::MAX, |list, _| {
        list.item_at_line_edge(line_index, position_type)
    })
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_hit_test_caret_item_for_line(
    arena: *mut c_void,
    line_index: usize,
    point: FfiCssPixelPoint,
    mode: u8,
) -> crate::painting::host::FfiCaretItemForLine {
    with_hit_test_list_and_caret_lines(arena, Default::default(), |list, arena| {
        match list.caret_item_for_line(
            arena,
            line_index,
            point.into(),
            crate::painting::hit_test::caret::CaretPositionMode::from_u8(mode),
        ) {
            Some((item_index, position_type)) => crate::painting::host::FfiCaretItemForLine {
                has_item: true,
                item_index,
                position_type: position_type as u8,
            },
            None => Default::default(),
        }
    })
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_hit_test_line_block_coordinate(arena: *mut c_void, line_index: usize) -> i32 {
    with_hit_test_list_and_caret_lines(arena, 0, |list, _| list.line_block_coordinate(line_index).raw_value())
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_hit_test_item_is_inline_adjacent_to_line(
    arena: *mut c_void,
    item_index: usize,
    line_index: usize,
) -> bool {
    with_hit_test_list_and_caret_lines(arena, false, |list, _| {
        list.item_is_inline_adjacent_to_line(item_index, line_index)
    })
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_hit_test_find_closest_line(
    arena: *mut c_void,
    callbacks: crate::painting::host::FfiHitTestQueryCallbacks,
    point: FfiCssPixelPoint,
    mode: u8,
    scoped: bool,
    respect_clip: bool,
) -> crate::painting::host::FfiClosestLine {
    with_hit_test_list_spatial_indexes_and_visual_context_tree(arena, true, Default::default(), |list, tree, arena| {
        let closest = list.find_closest_line(
            arena,
            tree,
            &callbacks,
            point.into(),
            crate::painting::hit_test::caret::CaretPositionMode::from_u8(mode),
            scoped,
            respect_clip,
        );
        crate::painting::host::FfiClosestLine {
            has_index: closest.index.is_some(),
            index: closest.index.unwrap_or(0),
            local_x: closest.local_point.x.raw_value(),
            local_y: closest.local_point.y.raw_value(),
            block_distance: closest.block_distance.raw_value(),
            block_start_distance: closest.block_start_distance.raw_value(),
            inline_distance: closest.inline_distance.raw_value(),
            is_before_point: closest.is_before_point,
            contains_point_in_block_axis: closest.contains_point_in_block_axis,
        }
    })
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_hit_test_adjacent_line(
    arena: *mut c_void,
    callbacks: crate::painting::host::FfiHitTestQueryCallbacks,
    current_line_index: usize,
    direction: u8,
    inline_coordinate_raw: i32,
) -> crate::painting::host::FfiAdjacentLine {
    let direction = if direction == 1 {
        crate::painting::hit_test::caret::CaretLineDirection::Next
    } else {
        crate::painting::hit_test::caret::CaretLineDirection::Previous
    };
    with_hit_test_list_and_caret_lines(arena, Default::default(), |list, arena| {
        match list.adjacent_line(
            arena,
            &callbacks,
            current_line_index,
            direction,
            CssPixels::from_raw(inline_coordinate_raw),
        ) {
            Some((line_index, point)) => crate::painting::host::FfiAdjacentLine {
                has_line: true,
                line_index,
                point_x: point.x.raw_value(),
                point_y: point.y.raw_value(),
            },
            None => Default::default(),
        }
    })
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
    use crate::painting::hit_test::HitTestList;
    use crate::painting::host::FfiRootBackgroundSource;
    use crate::painting::paintable_data::FfiOverflowData;
    use crate::painting::record::damage::PaintDamage;
    use crate::painting::visual_context::dirty::VisualContextBoxDirtyKind;
    use crate::painting::visual_context::{TransformData, TransformDataRole, VisualContextTree};

    #[test]
    fn hit_test_queries_can_remeasure_viewport_overflow_and_invalidate_painting() {
        for (spatial_indexes, caret_lines) in [(true, false), (true, true), (false, true), (false, false)] {
            let mut arena = LayoutNodeArena::new();
            let viewport = arena.allocate_for_test().slot;
            arena.data(viewport).kind.set(NodeKind::Viewport);
            arena.populate_paintable_row(viewport);
            arena.scrollable_overflow.viewport.set(Some(viewport));
            let root = arena.allocate_for_test().slot;
            arena.populate_paintable_row(root);
            *arena.hit_test_list.borrow_mut() = Some(HitTestList::default());
            {
                let mut state = arena.paint_state().borrow_mut();
                state.root_background_source = Some(FfiRootBackgroundSource {
                    root_layout_node: root,
                    ..Default::default()
                });
                state.visual_context.tree = Some(Rc::new(VisualContextTree::create(TransformData {
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
            arena
                .paintable_side_data(viewport)
                .overflow_relative_to_padding_box
                .set(FfiOverflowData {
                    rect: CssPixelRect::new(
                        CssPixels::from_integer(0),
                        CssPixels::from_integer(0),
                        CssPixels::from_integer(100),
                        CssPixels::from_integer(2000),
                    )
                    .into(),
                    has_scrollable_overflow: true,
                });
            arena
                .paintable_side_data(viewport)
                .overflow_measured_this_commit
                .set(true);
            arena.note_publishing_paint_recording_started();
            arena.clear_paint_damage_consumed_by_published_recording();

            let handle = std::ptr::from_mut(&mut arena).cast();
            let query = |_: &HitTestList, arena: &LayoutNodeArena| {
                crate::painting::paintable_geometry::scrollable_overflow_rect(&arena.paintable_rows(), viewport)
            };
            let rect = if spatial_indexes {
                with_hit_test_list_spatial_indexes_and_visual_context_tree(
                    handle,
                    caret_lines,
                    None,
                    |list, _, arena| query(list, arena),
                )
            } else if caret_lines {
                with_hit_test_list_and_caret_lines(handle, None, query)
            } else {
                with_hit_test_list_items_only(handle, None, query)
            };
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
        unsafe extern "C" fn tree_inputs(_: *mut c_void) -> crate::painting::host::FfiVisualContextTreeInputs {
            unreachable!("no visual context tree exists to refresh")
        }
        unsafe extern "C" fn scroll_offset(_: *mut c_void, _: *mut c_void) -> FfiCssPixelPoint {
            unreachable!("no visual context tree exists to refresh")
        }
        unsafe extern "C" fn node_identity(_: *mut c_void, _: *mut c_void) -> i64 {
            unreachable!("no visual context tree exists to refresh")
        }

        let mut arena = LayoutNodeArena::new();
        let viewport = arena.allocate_for_test().slot;
        arena.data(viewport).kind.set(NodeKind::Viewport);
        arena.populate_paintable_row(viewport);
        arena.scrollable_overflow.viewport.set(Some(viewport));
        let root = arena.allocate_for_test().slot;
        arena.data(root).kind.set(NodeKind::BlockContainer);
        arena.populate_paintable_row(root);
        // A structural change invalidated the root's overflow, measured earlier in this commit,
        // without queueing a recalculation, so nothing but a query measures it again. Measuring
        // it drops its scrollable overflow.
        arena
            .paintable_side_data(root)
            .overflow_relative_to_padding_box
            .set(FfiOverflowData {
                rect: CssPixelRect::new(
                    CssPixels::from_integer(0),
                    CssPixels::from_integer(0),
                    CssPixels::from_integer(100),
                    CssPixels::from_integer(2000),
                )
                .into(),
                has_scrollable_overflow: true,
            });
        arena.paintable_side_data(root).overflow_measured_this_commit.set(true);
        arena.paint_state().borrow_mut().visual_context.dirty_boxes.clear();

        let handle = std::ptr::from_mut(&mut arena).cast();
        let outcome = unsafe {
            layout_arena_prepare_for_rendering(
                handle,
                FfiVisualContextHostCallbacks {
                    context: std::ptr::null_mut(),
                    tree_inputs,
                    scroll_offset,
                    node_identity,
                },
                FfiRootBackgroundSource {
                    root_layout_node: root,
                    ..Default::default()
                },
                false,
            )
        };
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
